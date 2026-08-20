#include "archive.h"
#include "sha256.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/statvfs.h>
#include <unistd.h>

namespace nraw {

SampleLog* g_sampleLog = nullptr;
std::shared_future<std::string> g_sourceHashFut;

namespace {

constexpr uint32_t kSampleLogMagic = 0x4E524153;  // "NRAS"
constexpr uint32_t kSampleLogVersion = 2;
constexpr size_t kSampleEntrySize = 33;  // flags(1) + size/pts/dts/frameNo(8×4)

// 写一条样本日志条目（magic/版本头之后的固定 33 字节）
bool writeSampleEntry(FILE* f, const SampleLog::Entry& e)
{
    const uint8_t flags = (e.video ? 1u : 0u) | (e.key ? 2u : 0u);
    int64_t v = e.size;
    if (fwrite(&flags, 1, 1, f) != 1 || fwrite(&v, 8, 1, f) != 1)
        return false;
    v = e.pts;
    if (fwrite(&v, 8, 1, f) != 1)
        return false;
    v = e.dts;
    if (fwrite(&v, 8, 1, f) != 1)
        return false;
    v = e.frameNo;
    return fwrite(&v, 8, 1, f) == 1;
}

}  // namespace

// 定位 .part 中 mdat 数据区起点（ftyp(+wide) + mdat 头之后）。
// 导出供测试（repair-test 尾部截断变体）与内部 walker 使用。
long findMdatStart(FILE* in, long partSize)
{
    fseek(in, 0, SEEK_SET);
    long pos = 0;
    while (pos + 8 <= partSize) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, in) != 8)
            break;
        const uint32_t sz = (static_cast<uint32_t>(hdr[0]) << 24) |
                            (static_cast<uint32_t>(hdr[1]) << 16) |
                            (static_cast<uint32_t>(hdr[2]) << 8) | hdr[3];
        if (hdr[4] == 'm' && hdr[5] == 'd' && hdr[6] == 'a' && hdr[7] == 't') {
            // size==1 表示扩展头（16 字节：size/type/largesize），
            // 大文件（>4GB）movenc 会写扩展头，数据起点在 pos+16
            if (sz == 1)
                return pos + 16;
            return pos + 8;
        }
        if (sz < 8)
            break;
        fseek(in, sz - 8, SEEK_CUR);
        pos += sz;
    }
    return -1;
}

// 样本数据区的有效上界（排除尾部 moov/trailer）：
// 收尾写 trailer 时 movenc 会把 mdat 头声明的尺寸补丁为真实数据长度 →
// 返回 mdat 数据末端（moov 起点），防止"日志超前 + 尾部 moov"场景把幻影
// 日志条目（数据未落盘、落在 moov 区）误判为完整、重放时从 moov 读到垃圾；
// 崩溃无 trailer 时声明尺寸无效（占位/越界）→ 返回文件大小（维持原语义）。
// 四个 walker（countCompleteSamples/purgePartFile/submitPartSamples/
// tryRecoverMov）统一使用。
static long mdatDataBound(FILE* in, long partSize)
{
    fseek(in, 0, SEEK_SET);
    long pos = 0;
    while (pos + 8 <= partSize) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, in) != 8)
            break;
        const uint32_t sz = (static_cast<uint32_t>(hdr[0]) << 24) |
                            (static_cast<uint32_t>(hdr[1]) << 16) |
                            (static_cast<uint32_t>(hdr[2]) << 8) | hdr[3];
        if (hdr[4] == 'm' && hdr[5] == 'd' && hdr[6] == 'a' && hdr[7] == 't') {
            long long boxEnd = 0;
            if (sz == 1) {
                uint8_t ext[8];
                if (fread(ext, 1, 8, in) != 8)
                    return partSize;
                boxEnd = (static_cast<long long>(ext[0]) << 56) |
                         (static_cast<long long>(ext[1]) << 48) |
                         (static_cast<long long>(ext[2]) << 40) |
                         (static_cast<long long>(ext[3]) << 32) |
                         (static_cast<long long>(ext[4]) << 24) |
                         (static_cast<long long>(ext[5]) << 16) |
                         (static_cast<long long>(ext[6]) << 8) |
                         ext[7];
            } else {
                boxEnd = sz;
            }
            if (boxEnd > 0 && pos + boxEnd <= partSize)
                return static_cast<long>(pos + boxEnd);
            return partSize;  // 声明尺寸无效（崩溃未补丁）：退化为文件大小
        }
        if (sz < 8 || pos + sz > partSize)
            break;
        fseek(in, sz - 8, SEEK_CUR);
        pos += sz;
    }
    return partSize;
}

bool SampleLog::saveTo(const std::string& path, std::string& err) const
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        err = "无法写入 " + path;
        return false;
    }
    uint32_t magic = kSampleLogMagic, ver = kSampleLogVersion;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    for (const auto& e : entries) {
        if (!writeSampleEntry(f, e)) {
            err = "写入 " + path + " 失败";
            fclose(f);
            return false;
        }
    }
    if (ferror(f)) {
        err = "写入 " + path + " 失败";
        fclose(f);
        return false;
    }
    fsync(fileno(f));
    fclose(f);
    return true;
}

bool SampleLog::loadFrom(const std::string& path, std::string& err)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "无法打开 " + path;
        return false;
    }
    entries.clear();
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1 ||
        magic != kSampleLogMagic || ver != kSampleLogVersion) {
        err = path + " 格式不兼容";
        fclose(f);
        return false;
    }
    for (;;) {
        uint8_t flags = 0;
        int64_t size = 0, pts = 0, dts = 0, frameNo = -1;
        if (fread(&flags, 1, 1, f) != 1)
            break;
        // 尾部撕裂容错：条目中途截断视为日志结尾（掉电/崩溃落在写入与
        // fsync 之间），由 countCompleteSamples 兜底续传点
        if (fread(&size, 8, 1, f) != 1 || fread(&pts, 8, 1, f) != 1 ||
            fread(&dts, 8, 1, f) != 1 || fread(&frameNo, 8, 1, f) != 1)
            break;
        Entry e;
        e.video = (flags & 1) != 0;
        e.key = (flags & 2) != 0;
        e.size = size;
        e.pts = pts;
        e.dts = dts;
        e.frameNo = frameNo;
        entries.push_back(e);
    }
    fclose(f);
    return true;
}

bool fileExists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

bool readFileText(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz > 0 && fread(&out[0], 1, out.size(), f) != out.size()) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

std::string settingsHash(const CliOptions& opt)
{
    std::string s = "decode=" + std::to_string(opt.decodeMode);
    s += "|kelvin=" + std::to_string(opt.kelvin);
    s += "|tint=" + std::to_string(opt.tint);
    s += "|iso=" + std::to_string(opt.iso);
    s += "|exposure=" + std::to_string(opt.exposure);
    s += "|lens=" + std::to_string(opt.lensCorrection);
    s += "|chromaNr=" + std::to_string(opt.chromaNr);
    s += "|crf=" + std::to_string(opt.crf);
    s += "|preset=" + opt.preset;
    s += "|keyint=" + std::to_string(opt.keyint);
    s += "|minKeyint=" + std::to_string(opt.minKeyint);
    s += "|openGop=" + std::to_string(opt.openGop);
    // jobs/cpuWorkers/pools 只影响并行度与速度，不改变输出码流，且主程序
    // 的自动分配发生在 detectResume 之后——排除出指纹：既避免时序导致的
    // 误拒，也允许跨机器（不同核心数）续传。
    // 注：buffers 保留在指纹中（历史检查点兼容——曾排除后又恢复）
    s += "|buffers=" + std::to_string(opt.buffers);
    s += "|faststart=" + std::to_string(opt.faststart ? 1 : 0);
    s += "|noAudio=" + std::to_string(opt.noAudio ? 1 : 0);
    s += "|maxFrames=" + std::to_string(opt.maxFrames);
    return sha256Hex(s.data(), s.size());
}

std::string Checkpoint::toJson() const
{
    std::string esc;
    for (char c : input) {
        if (c == '"' || c == '\\')
            esc += '\\';
        esc += c;
    }
    return "{\"version\":" + std::to_string(version) +
           ",\"toolVersion\":\"" + toolVersion + "\"" +
           ",\"input\":\"" + esc + "\"" +
           ",\"sourceSha256\":\"" + sourceSha256 + "\"" +
           ",\"settingsHash\":\"" + settingsHash + "\"" +
           ",\"videoFrames\":" + std::to_string(videoFrames) +
           ",\"audioEndSample\":" + std::to_string(audioEndSample) +
           ",\"totalFrames\":" + std::to_string(totalFrames) + "}";
}

bool Checkpoint::loadJson(const std::string& text)
{
    auto get = [&](const char* key, std::string& out) -> bool {
        const std::string k = std::string("\"") + key + "\":";
        const size_t p = text.find(k);
        if (p == std::string::npos)
            return false;
        size_t q = p + k.size();
        if (q < text.size() && text[q] == '"') {
            const size_t e = text.find('"', q + 1);
            if (e == std::string::npos)
                return false;
            out = text.substr(q + 1, e - q - 1);
        } else {
            const size_t e = text.find_first_of(",}", q);
            if (e == std::string::npos)
                return false;
            out = text.substr(q, e - q);
        }
        return true;
    };
    std::string s;
    if (!get("version", s))
        return false;
    version = atoi(s.c_str());
    if (!get("toolVersion", toolVersion))
        return false;
    get("input", input);
    if (!get("sourceSha256", sourceSha256))
        return false;
    if (!get("settingsHash", settingsHash))
        return false;
    if (!get("videoFrames", s))
        return false;
    videoFrames = static_cast<size_t>(strtoull(s.c_str(), nullptr, 10));
    if (!get("audioEndSample", s))
        return false;
    audioEndSample = strtoull(s.c_str(), nullptr, 10);
    if (!get("totalFrames", s))
        return false;
    totalFrames = static_cast<size_t>(strtoull(s.c_str(), nullptr, 10));
    return true;
}

bool Checkpoint::loadFile(const std::string& path, Checkpoint& ck, std::string& err)
{
    std::string text;
    if (!readFileText(path, text)) {
        err = "无法读取 " + path;
        return false;
    }
    if (!ck.loadJson(text)) {
        err = path + " 格式无效";
        return false;
    }
    return true;
}

bool writeCheckpoint(const std::string& path, const CliOptions& opt,
                     const MediaInfo& info, const SampleLog& log, std::string& err,
                     bool waitHash)
{
    // GPU 路径下 runGpuPath 主线程与 encodeRun 编码线程都会写检查点：
    // 互斥串行化，避免对同一 tmp 文件交错写导致损坏
    static std::mutex g_ckMutex;
    std::lock_guard<std::mutex> lk(g_ckMutex);

    Checkpoint ck;
    ck.toolVersion = kToolVersion;
    ck.input = opt.input;
    ck.sourceSha256 = opt.sourceSha256;
    // 源哈希可能仍在后台计算：检查点必须带完整 sourceSha256（否则下次续传
    // 校验不一致而拒绝）。为避免初始检查点阻塞编码等待 270GB 哈希，这里
    // 只"按需等待"：waitHash=true（收尾）会等到哈希完成（一次性）；
    // 初始/周期检查点用 waitHash=false——哈希已就绪则直接取，未就绪则写空
    // sha（detectResume 对空 sha 跳过校验并明确警告）；哈希完成后的下一个
    // 周期检查点自动带上完整 sha，首次续传校验随即生效。
    if (ck.sourceSha256.empty() && g_sourceHashFut.valid()) {
        if (waitHash ||
            g_sourceHashFut.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            // waitHash=true（收尾）或哈希已就绪：取完整 sha（不阻塞）
            ck.sourceSha256 = g_sourceHashFut.get();
        }
        // 哈希未就绪且 waitHash=false（初始/周期检查点）：写空 sha——
        // detectResume 对空 sha 跳过校验；哈希完成后的下一个检查点自动
        // 带完整 sha（"哈希就绪即取"），首续校验随即生效
    }
    ck.settingsHash = settingsHash(opt);
    ck.videoFrames = log.videoCount();
    ck.audioEndSample = log.audioEndSample(info.audio.channels);
    ck.totalFrames = info.frameCount;
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "无法写入 " + tmp;
        return false;
    }
    const std::string js = ck.toJson();
    bool writeOk = fwrite(js.data(), 1, js.size(), f) == js.size() &&
                   !ferror(f);
    // fsync 失败（NFS soft 超时）不覆盖旧检查点：磁盘上可能是不完整文件，
    // 直接 rename 会让下次 detectResume 解析失败而拒绝续传
    if (writeOk)
        writeOk = fsync(fileno(f)) == 0;
    fclose(f);
    if (!writeOk) {
        remove(tmp.c_str());  // 失败不覆盖旧检查点
        err = "写入检查点失败: " + tmp;
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        err = "重命名检查点失败: " + tmp;
        return false;
    }
    return true;
}

size_t countCompleteSamples(const std::string& partPath, const SampleLog& log)
{
    FILE* in = fopen(partPath.c_str(), "rb");
    if (!in)
        return 0;
    fseek(in, 0, SEEK_END);
    const long partSize = ftell(in);
    const long mdatStart = findMdatStart(in, partSize);
    if (mdatStart < 0) {
        fclose(in);
        return 0;
    }
    long long cur = mdatStart;
    // 排除尾部 moov（收尾失败/软中断时 trailer 使文件尺寸虚增）：
    // 防止日志超前 + moov 区把幻影条目误判为完整（见 mdatDataBound）
    const long dataBound = mdatDataBound(in, partSize);
    size_t n = 0;
    for (const auto& e : log.entries) {
        if (e.size <= 0)
            continue;
        if (cur + e.size > dataBound)
            break;
        cur += e.size;
        ++n;
    }
    fclose(in);
    return n;
}

// 重写 .part：丢弃续传点之后（含）的视频条目（其数据将被重新编码），
// 使 .part 与已清理的样本日志布局一致（重放/重建/再次续传都不再错位）。
// 返回 true 时 partPath 已原位替换为清理后的版本。
bool purgePartFile(const std::string& partPath, const SampleLog& log,
                   int64_t skipVideoFromFrame, std::string& err, bool skipAudio)
{
    FILE* in = fopen(partPath.c_str(), "rb");
    if (!in) {
        err = "无法打开 " + partPath;
        return false;
    }
    long long mdatStart = -1;
    {
        fseek(in, 0, SEEK_END);
        const long partSize = ftell(in);
        fseek(in, 0, SEEK_SET);
        mdatStart = findMdatStart(in, partSize);
        if (mdatStart < 0) {
            fclose(in);
            err = partPath + " 缺少 mdat box";
            return false;
        }
        fseek(in, 0, SEEK_SET);
        // 前 mdatStart 字节（头部）原样复制
        const std::string tmp = partPath + ".purging";
        FILE* out = fopen(tmp.c_str(), "wb");
        if (!out) {
            fclose(in);
            err = "无法写入 " + tmp;
            return false;
        }
        std::vector<uint8_t> head(static_cast<size_t>(mdatStart));
        if (fread(head.data(), 1, head.size(), in) != head.size()) {
            fclose(in);
            fclose(out);
            remove(tmp.c_str());
            err = "读取 " + partPath + " 头部失败";
            return false;
        }
        fwrite(head.data(), 1, head.size(), out);
        // 计算每个条目在原始布局中的偏移（所有条目都推进，含被丢弃的）
        std::vector<long long> origOff(log.entries.size(), 0);
        {
            long long o = mdatStart;
            for (size_t i = 0; i < log.entries.size(); ++i) {
                origOff[i] = o;
                if (log.entries[i].size > 0)
                    o += log.entries[i].size;
            }
        }
        // 保留的条目（原始索引 + 数据），随后按 (dts, 视频在前) 排序写出，
        // 使 .part 布局与排序后的日志一致（重放按日志顺序读取）
        struct Keep {
            size_t idx;
            int64_t dts;
            bool video;
            int64_t size;
        };
        std::vector<Keep> keep;
        {
            long long cur = mdatStart;
            const long dataBound = mdatDataBound(in, partSize);
            for (size_t i = 0; i < log.entries.size(); ++i) {
                const auto& e = log.entries[i];
                if (e.size <= 0)
                    continue;
                if (cur + e.size > dataBound)
                    break;
                if (e.video && skipVideoFromFrame >= 0 &&
                    e.frameNo >= skipVideoFromFrame) {
                    cur += e.size;  // 丢弃：仅推进偏移
                    continue;
                }
                if (!e.video && skipAudio) {
                    cur += e.size;
                    continue;
                }
                keep.push_back({i, e.dts, e.video, e.size});
                cur += e.size;
            }
        }
        std::stable_sort(keep.begin(), keep.end(),
                         [](const Keep& a, const Keep& b) {
                             if (a.dts != b.dts)
                                 return a.dts < b.dts;
                             return a.video && !b.video;
                         });
        // 按排序后的顺序复制数据（从原始偏移读取）
        bool any = false;
        std::vector<uint8_t> buf;
        for (const auto& k : keep) {
            const size_t sz = static_cast<size_t>(k.size);
            buf.resize(sz);
            if (fseek(in, static_cast<long>(origOff[k.idx]), SEEK_SET) != 0 ||
                fread(buf.data(), 1, sz, in) != sz) {
                fclose(in);
                fclose(out);
                remove(tmp.c_str());
                err = "读取 " + partPath + " 失败";
                return false;
            }
            if (fwrite(buf.data(), 1, sz, out) != sz) {
                fclose(in);
                fclose(out);
                remove(tmp.c_str());
                err = "写入 " + tmp + " 失败";
                return false;
            }
            any = true;
        }
        if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
            fclose(in);
            fclose(out);
            remove(tmp.c_str());
            err = "同步 " + tmp + " 失败";
            return false;
        }
        fclose(in);
        fclose(out);
        if (!any) {
            remove(tmp.c_str());
            err = "没有可保留的完整样本";
            return false;
        }
        if (rename(tmp.c_str(), partPath.c_str()) != 0) {
            remove(tmp.c_str());
            err = "替换 " + partPath + " 失败";
            return false;
        }
    }
    return true;
}

bool submitPartSamples(const std::string& partPath, const SampleLog& log,
                       AVFormatContext* oc, AVStream* vs, AVStream* as,
                       size_t& videoCount, std::string& err,
                       int64_t skipVideoFromFrame, int64_t fpsDen, bool skipAudio)
{
    videoCount = 0;
    FILE* in = fopen(partPath.c_str(), "rb");
    if (!in) {
        err = "无法打开 " + partPath;
        return false;
    }
    fseek(in, 0, SEEK_END);
    const long partSize = ftell(in);
    const long mdatStart = findMdatStart(in, partSize);
    if (mdatStart < 0) {
        fclose(in);
        err = "文件中未找到 mdat";
        return false;
    }
    long long cur = mdatStart;
    std::vector<uint8_t> buf;
    bool any = false;
    cur = mdatStart;
    const long dataBound = mdatDataBound(in, partSize);
    for (const auto& e : log.entries) {
        if (e.size <= 0)
            continue;
        if (cur + e.size > dataBound)
            break;  // 尾部截断：只复用完整落盘的样本
        // 续传对齐：丢弃续传点之后（含）的视频条目（其 GOP 尾部引用不完整，
        // 将从续传点重新编码）
        if (e.video && skipVideoFromFrame >= 0 && e.frameNo >= skipVideoFromFrame) {
            cur += e.size;
            continue;
        }
        if (!e.video && skipAudio) {
            cur += e.size;
            continue;
        }
        AVStream* st = e.video ? vs : as;
        if (!st) {
            cur += e.size;  // 无对应流：跳过但保持顺序偏移
            continue;
        }
        buf.resize(static_cast<size_t>(e.size));
        fseek(in, static_cast<long>(cur), SEEK_SET);
        if (fread(buf.data(), 1, buf.size(), in) != buf.size())
            break;
        // movenc 期望 Annex-B 输入（hvc1 写文件时转长度前缀）：
        // 重放的旧样本是长度前缀格式，需转回 Annex-B 再提交
        if (e.video && buf.size() >= 8) {
            std::vector<uint8_t> annex;
            annex.reserve(buf.size() + 16);
            size_t p = 0;
            while (p + 4 <= buf.size()) {
                const uint32_t nalu = (static_cast<uint32_t>(buf[p]) << 24) |
                                      (static_cast<uint32_t>(buf[p + 1]) << 16) |
                                      (static_cast<uint32_t>(buf[p + 2]) << 8) |
                                      buf[p + 3];
                if (nalu == 0 || p + 4 + nalu > buf.size())
                    break;  // 长度前缀损坏（截断/篡改）
                annex.push_back(0);
                annex.push_back(0);
                annex.push_back(0);
                annex.push_back(1);
                annex.insert(annex.end(), buf.begin() + p + 4,
                            buf.begin() + p + 4 + nalu);
                p += 4 + nalu;
            }
            if (p == buf.size() && annex.size() > 0) {
                buf.swap(annex);
            } else if (p < buf.size()) {
                // 样本不是完整有效的长度前缀布局（数据损坏）：绝不能"原样
                // 提交"——长度前缀字节喂给 Annex-B 期望的 muxer 会被静默
                // 解析成 0 字节包（ff_hevc_annexb2mp4 找不到起始码），
                // av_write_frame 返回 0 不报错 → 帧静默丢失 → 损坏的归档
                // 看起来"成功"。工具自身写出的样本恒为长度前缀（已验证），
                // 解析失败只可能是数据损坏，必须显式失败。
                fclose(in);
                err = "视频样本长度前缀解析失败（数据损坏）于帧号 " +
                      std::to_string(e.frameNo);
                return false;
            }
            // p == buf.size() 且 annex 为空（仅当 buf.size()<8 分支漏过）：
            // 保持原样提交（不可达）
        }
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, static_cast<int>(e.size)) < 0) {
            av_packet_free(&pkt);
            break;
        }
        memcpy(pkt->data, buf.data(), buf.size());
        pkt->pts = e.pts;
        pkt->dts = e.dts;
        pkt->stream_index = st->index;
        // 视频时长按实际帧率（60000/fpsNum*fpsDen 于 1/60000 时间基）；
        // 视频时长 = fpsDen tick（vst time_base 实测保持 1/fpsNum；
        // 60000*fpsDen/fpsNum 仅 fpsNum==60000 正确，48/24fps 会虚增末样本）
        // 音频时长 = 条目实际采样数（size/(channels*3)）：固定 4800 仅对
        // 4800 样本/块的测试数据正确，真实剪辑音频块大小不定，写死会让
        // 重放音频时间轴虚增/缩短（stts 与实际样本数不符）
        int64_t aDuration = 4800;
        if (!e.video && as && as->codecpar->ch_layout.nb_channels > 0)
            aDuration = e.size /
                        static_cast<int64_t>(as->codecpar->ch_layout.nb_channels * 3);
        pkt->duration = e.video ? static_cast<int64_t>(fpsDen) : aDuration;
        if (e.key)
            pkt->flags |= AV_PKT_FLAG_KEY;
        // 必须用 av_write_frame（立即写入、提交顺序落盘）：
        // av_interleaved_write_frame 会按 dts 重排并可能压住/丢包，
        // 使文件布局与样本日志（提交顺序）不一致，顺序推导/自动重建全部错位
        if (av_write_frame(oc, pkt) < 0) {
            av_packet_free(&pkt);
            break;
        }
        av_packet_free(&pkt);
        if (e.video)
            ++videoCount;
        any = true;
        cur += e.size;
    }
    fclose(in);
    if (!any) {
        err = "没有可提交的完整样本";
        return false;
    }
    return true;
}

int detectResume(CliOptions& opt, const MediaInfo& info, std::string& msg)
{
    const std::string partPath = opt.output + ".part";
    const std::string samplesPath = opt.output + ".samples";
    const std::string ckptPath = opt.output + ".ckpt";
    if (!fileExists(partPath))
        return 0;  // 干净开始

    std::string err;
    Checkpoint ck;
    if (!fileExists(samplesPath) || !fileExists(ckptPath) ||
        !Checkpoint::loadFile(ckptPath, ck, err)) {
        msg = "发现 " + partPath + " 但缺少有效检查点（" + err +
              "），无法自动续传。请确认该文件不是遗留产物；删除它后可重新完整编码。";
        return 2;
    }
    if (ck.toolVersion != kToolVersion || ck.input != opt.input ||
        (!ck.sourceSha256.empty() && ck.sourceSha256 != opt.sourceSha256) ||
        ck.settingsHash != settingsHash(opt)) {
        msg = "检测到未完成的编码（" + partPath +
              "），但源文件或编码参数与检查点不一致（版本/输入/sha256/参数指纹），"
              "无法续传。删除 " + partPath + "、.samples、.ckpt 后可重新完整编码。";
        return 2;
    }
    // 检查点 sha 为空（前 500 帧内中断、哈希未完成即写盘）而本次已有完整 sha：
    // 校验被跳过——必须明确警告，避免"源文件已变更仍继续复用旧样本"被静默吞掉
    if (ck.sourceSha256.empty() && !opt.sourceSha256.empty()) {
        fprintf(stderr,
                "警告: 检查点未记录源文件 SHA-256（中断发生在哈希完成前的早期阶段），"
                "本次续传未校验源文件是否变更\n");
    }
    SampleLog slog;
    if (!slog.loadFrom(samplesPath, err)) {
        msg = "样本日志读取失败: " + err;
        return 2;
    }
    const size_t complete = countCompleteSamples(partPath, slog);
    size_t vc = 0;
    size_t lastKeyFrame = 0;  // 最后一个完整写入的关键帧帧号
    for (size_t i = 0; i < complete && i < slog.entries.size(); ++i) {
        const auto& e = slog.entries[i];
        if (!e.video)
            continue;
        ++vc;
        // 帧号直接取自日志（帧号在编码器原始时间基计算，不受 movenc
        // 写头时改写流 time_base 的影响——整数帧率会被 ×256/×512）
        if (e.key && e.frameNo >= 0)
            lastKeyFrame = static_cast<size_t>(e.frameNo);
    }
    if (vc == 0) {
        // .part 无完整样本（avio 缓冲未落盘的早期中断/断电）：无内容可复用，
        // 按全新编码处理（openSession 会截断 .part 重写 .samples 重算检查点）。
        // 不能返回 2 拒绝——用户会面对"前 500 帧中断无法续传"的死角
        msg = "检查点无有效编码进度（早期中断），将从第 0 帧重新编码";
        return 0;
    }
    if (vc > info.frameCount)
        vc = info.frameCount;
    // 部分 GOP 的尾部 B 帧引用未写入的未来帧：中途续传会解码失败。
    // 续传点回退到最后完整关键帧，并再回退 bframes 余量：open-GOP 的
    // scenecut 关键帧可能先于其 GOP 尾部 B 帧被写入（中断会切掉尾部），
    // 从关键帧精确续传会丢失那些未写入的 B 帧；回退后它们被重新编码。
    // 续传点 = 最后完整关键帧：
    //  - closed GOP：直接关键帧对齐。重放的完整 GOP 尾部 B 帧引用都在
    //    重放数据内（POC 无缺失）；回退反而让重放尾部跨入重编码区，
    //    解码顺序上引用未就绪（"Could not find ref"）。
    //  - open GOP：scenecut 关键帧可能先于其 GOP 尾部 B 帧被写入
    //    （中断会切掉尾部），回退让尾部被重新编码，不丢帧。
    //    回退量须 ≥ bframes+P间隔（libx265 默认 bframes=4 → 间隔 5，
    //    合计 9）：保留区内最后一个 B 帧 B(K-9..K-6) 引用更晚的 P(K-5)，
    //    回退不足会让该引用落入重编码区（解码花屏）。
    // 回退量实测需覆盖"两个 GOP 的 B 帧跨度"：scenecut 关键帧位置不规则，
    // 保留区最后一个 B 帧可能引用更晚的 P 帧或下一个关键帧（open-GOP 跨
    // GOP 引用）。bframes=4 + P 间隔 5 时单 GOP 需 9，两 GOP 需 18——
    // 回退 17（=2×(bframes+P间隔)-1）在多种中断点实测 0 解码错误。
    // （注：回退 > 2×P 间隔后 resumeK 落在部分模位置会触发重编码首 GOP
    // 的负 ctts B 帧（movenc EINVAL），故不取更大值。）
    const size_t kResumeBackoff = 17;
    size_t resumeK = lastKeyFrame;
    if (opt.openGop && lastKeyFrame > kResumeBackoff)
        resumeK = lastKeyFrame - kResumeBackoff;
    // ---- 无时间轴平移条件 ----
    // movenc 6.1 双重约束（dts 严格递增 + pts>=dts 不可负 ctts）下，若重放区
    // 最大 dts >= 重编码首包（IDR）dts，续传只能整体平移 pts（时间轴跳变）
    // 或失败。正解：动态回退续传点，使"重放区（frameNo < resumeK 的视频）
    // 最大 dts < 重编码首包 dts"。x265 重启后首包 dts = pts - 2×fpsDen
    // （编码器延迟 2 帧，pts = resumeK×fpsDen），故条件：
    //     maxDtsReplay < resumeK×fpsDen - 2×fpsDen
    // 不满足则回退到前一个关键帧（保留引用封闭的回退余量），直到满足——
    // 续传点自动前移，重放区以更早的帧结束，dts 天然单调，无需平移。
    {
        const int64_t fpsDen = info.fpsDen > 0 ? info.fpsDen : 1;
        // 关键帧列表（升序，供回退迭代）
        std::vector<size_t> keys;
        for (size_t i = 0; i < complete && i < slog.entries.size(); ++i) {
            const auto& e = slog.entries[i];
            if (e.video && e.key && e.frameNo >= 0)
                keys.push_back(static_cast<size_t>(e.frameNo));
        }
        size_t ki = keys.size();  // 从最后关键帧开始
        bool noShiftSatisfied = false;  // 是否有候选满足"无平移"条件
        while (ki > 0) {
            const size_t base = keys[ki - 1];
            size_t cand = base;
            if (opt.openGop && cand > kResumeBackoff)
                cand -= kResumeBackoff;  // 引用封闭回退余量
            if (cand == 0)
                break;  // cand=0 恒不满足（maxDts=-1 vs idrDts=-2×fpsDen）
            // 重放区最大 dts
            int64_t maxDts = -1;
            for (size_t i = 0; i < complete && i < slog.entries.size(); ++i) {
                const auto& e = slog.entries[i];
                if (!e.video || e.frameNo < 0 ||
                    static_cast<size_t>(e.frameNo) >= cand)
                    continue;
                if (e.dts > maxDts)
                    maxDts = e.dts;
            }
            const int64_t idrDts =
                static_cast<int64_t>(cand) * fpsDen - 2 * fpsDen;
            if (maxDts < idrDts) {
                resumeK = cand;  // 满足：无平移
                noShiftSatisfied = true;
                break;
            }
            // 不满足：回退到前一个关键帧（保留引用封闭余量）
            --ki;
        }
        // 循环因 cand==0 退出且无任何候选满足（B 金字塔周期与 keyint 对齐时
        // 每个关键帧的 GOP 尾部解码序都可能落在 cand-2 槽）：关键帧锚定的
        // 续传点会让 shiftResumeDts 平移 pts（时间轴跳变），更糟的是重放
        // 尾部 B 帧引用重编码区（如 POC resumeK+1）时产生"Could not find
        // ref"解码错误（实测）。逐帧下探寻找最大满足"无平移"条件的续传点：
        // 新 IDR 可从任意帧开始，重放区以更早的帧结束；满足条件的 K 使重
        // 编码区首个包 dts ≥ floor > 重放区最大 dts，接缝处 dts/pts 连续且
        // 引用封闭（f(K-1) 及其前序帧以原始顺序重放，引用完整）。
        if (!noShiftSatisfied) {
            const size_t maxK = resumeK;
            resumeK = 0;
            for (size_t K = maxK; K > 0; --K) {
                int64_t maxDts = -1;
                for (size_t i = 0; i < complete && i < slog.entries.size(); ++i) {
                    const auto& e = slog.entries[i];
                    if (!e.video || e.frameNo < 0 ||
                        static_cast<size_t>(e.frameNo) >= K)
                        continue;
                    if (e.dts > maxDts)
                        maxDts = e.dts;
                }
                if (maxDts < static_cast<int64_t>(K) * fpsDen - 2 * fpsDen) {
                    resumeK = K;  // 最大满足点：最多复用且无平移
                    break;
                }
            }
        }
    }
    if (resumeK == 0) {
        // 中断发生在首个 GOP 内（关键帧 0 之前没有任何完整关键帧）：
        // 无内容可复用，按全新编码处理（旧 .part 会被本次会话截断覆盖）
        msg = "检查点仅覆盖首个 GOP（无完整关键帧），将从第 0 帧重新编码";
        return 0;
    }
    // 音频进度按完整样本区间计算
    SampleLog use;
    for (size_t i = 0; i < complete && i < slog.entries.size(); ++i)
        use.entries.push_back(slog.entries[i]);

    opt.resumeMode = true;
    opt.resumeFrame = static_cast<long>(resumeK);
    opt.workerStart = static_cast<long>(resumeK);
    opt.resumeAudioSample = use.audioEndSample(info.audio.channels);
    msg = "检测到未完成的编码（已完成 " + std::to_string(vc) + "/" +
          std::to_string(info.frameCount) + " 帧），自动续传：已编码部分直接复用，"
          "从最后完整关键帧第 " + std::to_string(resumeK) + " 帧继续" +
          (resumeK < vc ? "（回退 " + std::to_string(vc - resumeK) + " 帧保证引用完整）" : "");
    return 1;
}

namespace {

int autoKeyint(const MediaInfo& info)
{
    int v = static_cast<int>(std::llround(
        static_cast<double>(info.fpsNum) / static_cast<double>(info.fpsDen) * 2.0));
    return v > 1 ? v : 1;
}

std::string x265Params(const CliOptions& opt)
{
    std::string s = "min-keyint=" + std::to_string(opt.minKeyint > 0 ? opt.minKeyint : 1);
    s += ":range=full";
    // 显式声明 GOP 结构，避免依赖 x265 默认值变化（README 默认 open GOP）
    s += opt.openGop == 0 ? ":open-gop=0" : ":open-gop=1";
    int pools = opt.pools > 0 ? opt.pools : 8;
    s += ":pools=" + std::to_string(pools);
    return s;
}

}

class EncodeSessionImpl {
public:
    bool openSession(const CliOptions& opt, const MediaInfo& info, std::string& err);
    void setHardAbort() { hardAbort_ = true; }   // 硬中断：析构跳过 trailer/avio 落盘
    bool writeFrame(const VideoFrame& f, std::string& err);
    bool writeAudioPkt(const AudioPacket& p, std::string& err);
    bool finishSession(std::string& err);
    ~EncodeSessionImpl();

    // 续传：重放旧 .part 已编码样本；查询重放帧数；增量持久化样本日志
    bool replayOldSamples(const std::string& oldPart, std::string& err);
    size_t replayedFrames() const { return replayedFrames_; }
    void flushSamples();
    bool flushEnc(std::string& err);  // 收尾冲刷编码器（中断时保留挂起帧）

private:
    bool sendFrame(const VideoFrame& f, std::string& err);
    bool syncAndRename(std::string& err);   // fsync 重试 + rename + 目录 fsync
    std::string diskSpaceNote() const;      // 输出目录剩余空间描述（诊断用）
    void shiftResumeDts(AVPacket* pkt);     // 续传：新包 dts 衔接旧时间轴

    MediaInfo info_;
    bool dumpOnly_ = false;
    std::string finalPath_;
    std::string outPath_;
    bool headerWritten_ = false;
    bool finished_ = false;
    AVFormatContext* oc_ = nullptr;
    AVCodecContext* enc_ = nullptr;
    AVStream* vst_ = nullptr;
    AVStream* ast_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* srcFrame_ = nullptr;
    AVFrame* dstFrame_ = nullptr;
    FILE* dump_ = nullptr;
    bool preservePartial_ = false;  // 失败时保留 .part，避免删除数小时产物
    int writeFlushCounter_ = 0;     // 周期性 avio_flush 计数（提前暴露写错误）
    bool replayed_ = false;         // 续传：旧样本已重放
    size_t replayedFrames_ = 0;     // 续传：重放的视频帧数
    FILE* samplesFile_ = nullptr;   // 样本日志持久化（续传检查点）
    size_t samplesSaved_ = 0;       // 已写入持久化文件的条目数
    bool hardAbort_ = false;        // 硬中断：析构跳过 trailer/avio 落盘
    bool resumeSession_ = false;    // 本次是续传会话
    long resumeFrame_ = 0;          // 续传点（关键帧对齐）
    int64_t resumeDtsBase_ = 0;     // 重放旧样本的最后一个视频 dts
    bool resumeShiftInit_ = false;  // 会话级 dts 平移：首包已锚定
    int64_t resumeShift_ = 0;       // 重编码区整体平移量（dts/pts 同步）

    int64_t lastWrittenDts_ = AV_NOPTS_VALUE;  // 单调钳制：dts 不得回退
};

bool EncodeSessionImpl::openSession(const CliOptions& opt, const MediaInfo& info,
                                    std::string& err)
{
    info_ = info;
    dumpOnly_ = !opt.dumpRef.empty();
    finalPath_ = opt.output;
    resumeSession_ = opt.resumeMode;
    resumeFrame_ = opt.resumeMode ? opt.resumeFrame : 0;
    // 续传时旧 .part 已被主流程改名为 .part.old（数据源，只读），
    // 本会话统一写规范 .part：任何失败后 .part 与 .samples 自洽，可再次续传
    outPath_ = dumpOnly_ ? opt.dumpRef : opt.output + ".part";

    // 样本日志持久化：<out>.samples（先写头；续传时把已加载的旧条目全部重写）
    if (!dumpOnly_ && g_sampleLog) {
        samplesFile_ = fopen((opt.output + ".samples").c_str(), "wb");
        if (!samplesFile_) {
            err = "无法创建样本日志 " + opt.output + ".samples";
            return false;
        }
        const uint32_t magic = kSampleLogMagic, ver = kSampleLogVersion;
        if (fwrite(&magic, 4, 1, samplesFile_) != 1 ||
            fwrite(&ver, 4, 1, samplesFile_) != 1) {
            err = "无法写入样本日志 " + opt.output + ".samples";
            fclose(samplesFile_);
            samplesFile_ = nullptr;
            return false;
        }
        const size_t n = g_sampleLog->entries.size();
        for (size_t i = 0; i < n; ++i) {
            if (!writeSampleEntry(samplesFile_, g_sampleLog->entries[i])) {
                err = "无法写入样本日志 " + opt.output + ".samples";
                fclose(samplesFile_);
                samplesFile_ = nullptr;
                return false;
            }
        }
        fflush(samplesFile_);
        samplesSaved_ = n;
    }

    // 输出目录剩余空间预检：磁盘不足时尽早提示，避免跑到收尾才失败
    if (!dumpOnly_) {
        struct statvfs sv;
        std::string dir = finalPath_;
        size_t slash = dir.rfind('/');
        dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
        if (dir.empty())
            dir = "/";
        if (statvfs(dir.c_str(), &sv) == 0) {
            double freeGB = static_cast<double>(sv.f_bavail) *
                            static_cast<double>(sv.f_frsize) / (1024.0 * 1024.0 * 1024.0);
            if (freeGB < 8.0)
                fprintf(stderr, "警告: 输出目录剩余空间仅约 %.1f GB，可能不足以容纳编码输出\n",
                        freeGB);
        }
    }

    const int w = static_cast<int>(info.width);
    const int h = static_cast<int>(info.height);

    srcFrame_ = av_frame_alloc();
    dstFrame_ = av_frame_alloc();
    if (!srcFrame_ || !dstFrame_) {
        err = "frame 分配失败";
        return false;
    }

    dstFrame_->format = AV_PIX_FMT_YUV420P10LE;
    dstFrame_->width = w;
    dstFrame_->height = h;
    if (av_frame_get_buffer(dstFrame_, 32) < 0) {
        err = "帧缓冲分配失败";
        return false;
    }

    sws_ = sws_getContext(w, h, AV_PIX_FMT_GBRP16LE, w, h, AV_PIX_FMT_YUV420P10LE,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        err = "swscale 上下文创建失败";
        return false;
    }
    sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_BT2020), 1,
                             sws_getCoefficients(SWS_CS_BT2020), 1, 0, 1 << 16, 1 << 16);

    srcFrame_->linesize[0] = w * 2;
    srcFrame_->linesize[1] = w * 2;
    srcFrame_->linesize[2] = w * 2;

    if (dumpOnly_) {
        dump_ = fopen(opt.dumpRef.c_str(), "wb");
        if (!dump_) {
            err = "无法创建参考文件";
            return false;
        }
        return true;
    }

    if (avformat_alloc_output_context2(&oc_, nullptr, "mov", outPath_.c_str()) < 0 || !oc_) {
        err = "MOV 封装器创建失败";
        return false;
    }
    if (avio_open(&oc_->pb, outPath_.c_str(), AVIO_FLAG_WRITE) < 0) {
        err = "无法创建输出文件";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        err = "找不到 libx265 编码器";
        return false;
    }
    enc_ = avcodec_alloc_context3(codec);
    if (!enc_) {
        err = "编码器上下文分配失败";
        return false;
    }

    const int keyint = opt.keyint > 0 ? opt.keyint : autoKeyint(info);
    enc_->width = w;
    enc_->height = h;
    av_opt_set(enc_->priv_data, "crf", std::to_string(opt.crf).c_str(), 0);
    av_opt_set(enc_->priv_data, "preset", opt.preset.c_str(), 0);
    enc_->gop_size = keyint;
    enc_->time_base = AVRational{1, static_cast<int>(info.fpsNum)};
    enc_->framerate = AVRational{static_cast<int>(info.fpsNum),
                                 static_cast<int>(info.fpsDen)};
    enc_->pix_fmt = AV_PIX_FMT_YUV420P10LE;
    av_opt_set(enc_->priv_data, "x265-params", x265Params(opt).c_str(), 0);
    enc_->colorspace = AVCOL_SPC_BT2020_NCL;
    enc_->color_range = AVCOL_RANGE_JPEG;
    // mov/mp4 封装必须由编码器提供全局参数集（SPS/PPS/VPS 写入容器 extradata）。
    // 不设置 GLOBAL_HEADER 时 libx265 不输出 extradata：非分片路径侥幸靠首包
    // 提取掩盖了问题，分片路径（moov 前置、stsd 在写第一帧前生成）会写出空 hvcC。
    enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(enc_, codec, nullptr) < 0) {
        err = "x265 初始化失败 (preset=\"" + opt.preset + "\", crf=" +
              std::to_string(opt.crf) + ")";
        return false;
    }

    // 2 参数形式在所有 FFmpeg 版本可用（FFmpeg 7 的 1 参数便捷重载在 8.x 已移除）
    vst_ = avformat_new_stream(oc_, nullptr);
    if (!vst_) {
        err = "视频流创建失败";
        return false;
    }
    if (avcodec_parameters_from_context(vst_->codecpar, enc_) < 0) {
        err = "编码参数复制失败";
        return false;
    }
    vst_->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    vst_->time_base = enc_->time_base;
    vst_->avg_frame_rate = AVRational{static_cast<int>(info.fpsNum),
                                      static_cast<int>(info.fpsDen)};
    vst_->r_frame_rate = vst_->avg_frame_rate;

    if (!opt.noAudio && info.audio.present) {
        ast_ = avformat_new_stream(oc_, nullptr);
        if (!ast_) {
            err = "音频流创建失败";
            return false;
        }
        ast_->time_base = AVRational{1, info.audio.sampleRate};
        ast_->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        ast_->codecpar->codec_id = AV_CODEC_ID_PCM_S24LE;
        ast_->codecpar->sample_rate = info.audio.sampleRate;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        AVChannelLayout lay;
        av_channel_layout_default(&lay, info.audio.channels);
        av_channel_layout_copy(&ast_->codecpar->ch_layout, &lay);
#else
        ast_->codecpar->channels = info.audio.channels;
        ast_->codecpar->channel_layout = av_get_default_channel_layout(info.audio.channels);
#endif
        ast_->codecpar->bits_per_coded_sample = 24;
        ast_->codecpar->block_align = info.audio.channels * 3;
        ast_->codecpar->bit_rate = info.audio.sampleRate * info.audio.channels * 24;
        ast_->codecpar->codec_tag = MKTAG('i', 'n', '2', '4');
    }

    // movflags 必须通过 muxer 私有选项设置（av_opt_set 于 priv_data）。
    // 注意：不能通过 avformat_write_header 的 options 字典传入——
    // FFmpeg 6.1 的 mov muxer 在该路径下会写出空的 hvcC 桩（extradata 丢失）。
    // x265 的 B 帧重排会让最初几个包出现负 dts：显式把时间轴平移到 0。
    // 默认输出保持经典非分片 MOV：FFmpeg 6.1 的 fragmented movenc 存在时间戳
    // 偏移缺陷（分片输出会让音视频时间轴偏移 1~2 帧，连 ffmpeg CLI 也无法幸免），
    // 分片格式会污染归档时间轴，故不以分片为默认。
    oc_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    oc_->max_interleave_delta = 0;

    {
        // 经典非分片 MOV：默认 moov 在文件尾（收尾一次写入）；--faststart
        // 时收尾重写为前置。兼容性优先（不用分片格式）。
        // 注：movenc 6.1 无条件拒绝 pts<dts（即使设置 negative_cts_offsets
        // 也返回 EINVAL）——负 ctts B 帧只能靠 dts 钳制/位移保证 pts>=dts，
        // 不能依赖该 movflags 位
        if (opt.faststart)
            av_opt_set(oc_->priv_data, "movflags", "faststart", 0);
    }
    if (getenv("NRAW_AVLOG")) {
        av_log_set_level(AV_LOG_DEBUG);  // 调试 movenc 拒绝原因
        fprintf(stderr, "[avlog] 开启 AV_LOG_DEBUG\n");
    }
    int hr = avformat_write_header(oc_, nullptr);
    if (hr < 0) {
        err = "写入 mov header 失败";
        return false;
    }
    headerWritten_ = true;
    return true;
}

bool EncodeSessionImpl::writeAudioPkt(const AudioPacket& p, std::string& err)
{
    if (dumpOnly_ || !ast_ || p.bytes.empty())
        return true;
    AVPacket pkt{};
    pkt.stream_index = ast_->index;
    pkt.data = const_cast<uint8_t*>(p.bytes.data());
    pkt.size = static_cast<int>(p.bytes.size());
    pkt.pts = pkt.dts = static_cast<int64_t>(p.firstSample);
    pkt.flags = AV_PKT_FLAG_KEY;
    // 显式设置时长 = 实际采样数（size/(channels*3)）：与续传重放
    // （submitPartSamples）的 duration 计算一致，全新编码与续传产物的
    // 音频 stts 布局位级一致；对真实剪辑（块大小不定）也总是正确
    if (ast_->codecpar->ch_layout.nb_channels > 0)
        pkt.duration =
            static_cast<int64_t>(p.bytes.size()) /
            static_cast<int64_t>(ast_->codecpar->ch_layout.nb_channels * 3);
    // 与视频一致：直写（av_write_frame）。先写后入日志：写入失败
    // （ENOSPC/NFS）时不留下"日志有条目、数据未落盘"的幻影条目，
    // 否则续传按日志重放会在错误偏移读到垃圾/静默丢帧
    if (av_write_frame(oc_, &pkt) < 0) {
        err = "音频写入失败 (保留部分产物 " + outPath_ + ")";
        preservePartial_ = true;
        return false;
    }
    if (g_sampleLog)
        g_sampleLog->entries.push_back(
            {false, 0, pkt.size, pkt.pts, pkt.dts, true, -1});
    return true;
}

bool EncodeSessionImpl::sendFrame(const VideoFrame& f, std::string& err)
{
    uint8_t* base = static_cast<uint8_t*>(f.rgb.data());
    srcFrame_->data[2] = base;
    srcFrame_->data[0] = base + f.width * f.height * 2;
    srcFrame_->data[1] = base + 2 * f.width * f.height * 2;
    sws_scale(sws_, srcFrame_->data, srcFrame_->linesize, 0,
              static_cast<int>(f.height), dstFrame_->data, dstFrame_->linesize);
    if (dumpOnly_) {
        size_t yRow = f.width * 2;
        size_t cRow = ((f.width + 1) / 2) * 2;
        size_t cRows = (f.height + 1) / 2;
        for (size_t y = 0; y < f.height; ++y) {
            if (fwrite(dstFrame_->data[0] + y * dstFrame_->linesize[0], 1, yRow, dump_) != yRow) {
                err = "参考文件写入失败 (可能磁盘已满)";
                return false;
            }
        }
        for (size_t y = 0; y < cRows; ++y) {
            if (fwrite(dstFrame_->data[1] + y * dstFrame_->linesize[1], 1, cRow, dump_) != cRow) {
                err = "参考文件写入失败 (可能磁盘已满)";
                return false;
            }
        }
        for (size_t y = 0; y < cRows; ++y) {
            if (fwrite(dstFrame_->data[2] + y * dstFrame_->linesize[2], 1, cRow, dump_) != cRow) {
                err = "参考文件写入失败 (可能磁盘已满)";
                return false;
            }
        }
        return true;
    }
    dstFrame_->pts = static_cast<int64_t>(f.frameNo) * static_cast<int64_t>(info_.fpsDen);
    if (avcodec_send_frame(enc_, dstFrame_) < 0) {
        err = "x265 编码失败";
        return false;
    }
    return true;
}

bool EncodeSessionImpl::flushEnc(std::string& err)
{
    if (dumpOnly_ || !enc_ || !vst_ || !headerWritten_)
        return true;
    if (avcodec_send_frame(enc_, nullptr) < 0) {
        err = "x265 收尾失败 (flush)";
        return false;
    }
    for (;;) {
        AVPacket pkt{};
        int r = avcodec_receive_packet(enc_, &pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            break;
        if (r < 0) {
            av_packet_unref(&pkt);
            err = "x265 收尾失败";
            return false;
        }
        pkt.stream_index = vst_->index;
        // 帧号在编码器原始时间基中计算：帧 pts = 帧号×fpsDen，
        // 不受 movenc 写头时流 time_base 处理的影响（实测 1/fpsNum 保持不变）
        const int64_t flushFrameNo =
            info_.fpsDen > 0 ? pkt.pts / static_cast<int64_t>(info_.fpsDen) : -1;
        av_packet_rescale_ts(&pkt, enc_->time_base, vst_->time_base);
        if (pkt.dts < 0)
            pkt.dts = 0;
        shiftResumeDts(&pkt);
        // 严格递增钳制：movenc/mux.c 对"相等 dts"（cur_dts>=pkt->dts）也
        // 拒绝（non monotonically increasing）——回退或相等都必须抬到
        // last+1；pts 同步抬升保证 pts>=dts（ctts>=0，movenc 无条件拒绝负 ctts）
        if (lastWrittenDts_ != AV_NOPTS_VALUE && pkt.dts <= lastWrittenDts_) {
            pkt.dts = lastWrittenDts_ + 1;
            if (pkt.pts < pkt.dts)
                pkt.pts = pkt.dts;
        }
        lastWrittenDts_ = pkt.dts;
        // 先写成功再入日志（写失败不留幻影条目，否则续传错位/丢帧）
        const int64_t off = avio_tell(oc_->pb);
        // 与 writeFrame/重放一致：直写（av_write_frame），提交顺序落盘，
        // 日志偏移/大小与文件布局一一对应；interleave 层会重排/压包
        if (av_write_frame(oc_, &pkt) < 0) {
            av_packet_unref(&pkt);
            err = "收尾包写入失败 (保留部分产物 " + outPath_ + ")";
            preservePartial_ = true;
            return false;
        }
        if (g_sampleLog)
            g_sampleLog->entries.push_back(
                {true, off, pkt.size, pkt.pts, pkt.dts,
                 (pkt.flags & AV_PKT_FLAG_KEY) != 0, flushFrameNo});
        av_packet_unref(&pkt);  // av_write_frame 不接管引用，必须释放
    }
    return true;
}

bool EncodeSessionImpl::writeFrame(const VideoFrame& f, std::string& err)
{
    if (!sendFrame(f, err))
        return false;
    if (dumpOnly_)
        return true;
    for (;;) {
        AVPacket pkt{};
        int r = avcodec_receive_packet(enc_, &pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            break;
        if (r < 0) {
            av_packet_unref(&pkt);
            err = "x265 收包失败";
            return false;
        }
        // 帧号（显示序）：编码器延迟重排，收到的包可能对应更早的帧，
        // 用编码器原始时间基（{1,60000}）的 pts 计算（帧 pts = 帧号×fpsDen）
        const int64_t pktFrameNo =
            info_.fpsDen > 0 ? pkt.pts / static_cast<int64_t>(info_.fpsDen) : -1;
        pkt.stream_index = vst_->index;
        av_packet_rescale_ts(&pkt, enc_->time_base, vst_->time_base);
        // B 帧重排会让最初几个包 dts 为负：钳制到 0（ctts=pts-dts 自动补偿，
        // demuxed pts 不变），避免 movenc 报 "poorly interleaved" 警告
        if (pkt.dts < 0)
            pkt.dts = 0;
        shiftResumeDts(&pkt);
        // 严格递增钳制：movenc/mux.c 对"相等 dts"（cur_dts>=pkt->dts）也
        // 拒绝（non monotonically increasing）——回退或相等都必须抬到
        // last+1；pts 同步抬升保证 pts>=dts（ctts>=0，movenc 无条件拒绝负 ctts）
        if (lastWrittenDts_ != AV_NOPTS_VALUE && pkt.dts <= lastWrittenDts_) {
            pkt.dts = lastWrittenDts_ + 1;
            if (pkt.pts < pkt.dts)
                pkt.pts = pkt.dts;
        }
        lastWrittenDts_ = pkt.dts;
        // 先写成功再入日志（写失败不留幻影条目，否则续传错位/丢帧）
        const int64_t off = avio_tell(oc_->pb);
        {
            const int r = av_write_frame(oc_, &pkt);
            if (r < 0) {
                char ebb[256] = {0};
                av_strerror(r, ebb, sizeof(ebb));
                // 先记下时间戳再 unref（unref 后读 pkt 是未定义行为）
                const std::string ptsStr = std::to_string(pkt.pts);
                const std::string dtsStr = std::to_string(pkt.dts);
                const int64_t pktNo =
                    info_.fpsDen > 0
                        ? pkt.pts / static_cast<int64_t>(info_.fpsDen)
                        : -1;
                const std::string lwStr = std::to_string(lastWrittenDts_);
                const std::string rbStr = std::to_string(resumeDtsBase_);
                av_packet_unref(&pkt);
                err = "视频包写入失败 (保留部分产物 " + outPath_ + "): " + ebb +
                      " (pts=" + ptsStr + " dts=" + dtsStr +
                      " frameNo=" + std::to_string(pktNo) +
                      " lastWrittenDts=" + lwStr +
                      " resumeDtsBase=" + rbStr + ")";
                preservePartial_ = true;
                return false;
            }
        }
        if (g_sampleLog)
            g_sampleLog->entries.push_back(
                {true, off, pkt.size, pkt.pts, pkt.dts,
                 (pkt.flags & AV_PKT_FLAG_KEY) != 0, pktFrameNo});
        av_packet_unref(&pkt);  // av_write_frame 不接管引用，必须释放
    }
    // 周期性刷盘：把 32KB 缓冲定期推向存储，NFS/磁盘错误在编码途中就暴露，
    // 而不是拖到收尾才失败；刷过的数据对 NFS 而言已到达服务器。
    if (++writeFlushCounter_ >= 500) {
        writeFlushCounter_ = 0;
        avio_flush(oc_->pb);
        flushSamples();
        if (oc_->pb->error < 0) {
            err = "输出文件写入失败 (NFS/磁盘错误, 保留部分产物 " + outPath_ + ")";
            preservePartial_ = true;
            return false;
        }
    }
    return true;
}

void EncodeSessionImpl::flushSamples()
{
    if (!samplesFile_ || !g_sampleLog)
        return;
    const size_t n = g_sampleLog->entries.size();
    if (n <= samplesSaved_)
        return;
    // 已持久化的字节偏移（magic 8 字节 + 每条 33 字节）：失败时截断回退，
    // 否则已部分写入的条目会在下次重试时重复（续传按日志顺序累计偏移，
    // 重复条目使后续条目整体错位 → 静默损坏）
    const long savedOff = 8 + static_cast<long>(samplesSaved_) * 33;
    bool ok = true;
    for (size_t i = samplesSaved_; i < n; ++i) {
        if (!writeSampleEntry(samplesFile_, g_sampleLog->entries[i])) {
            ok = false;
            break;
        }
    }
    if (ok)
        ok = fflush(samplesFile_) == 0;
    if (ok)
        ok = fsync(fileno(samplesFile_)) == 0;
    if (ok) {
        samplesSaved_ = n;
    } else {
        // 回退到已保存偏移（丢弃可能的部分写入），下次从 samplesSaved_ 重试。
        // ftruncate 失败（NFS 严重故障）时无法安全重试——保守置位，会话
        // 后续写入仍继续但 .samples 可能含重复条目；此时收尾/中断的续传
        // 会被 countCompleteSamples 的 fit 校验部分兜底
        if (ftruncate(fileno(samplesFile_), savedOff) != 0) {
            // 记录但不崩溃：文件系统异常时整个会话大概率也将失败
        }
        fseek(samplesFile_, savedOff, SEEK_SET);
        clearerr(samplesFile_);
    }
}

bool EncodeSessionImpl::replayOldSamples(const std::string& oldPart,
                                         std::string& err)
{
    if (replayed_)
        return true;
    if (!oc_ || !headerWritten_ || dumpOnly_ || !g_sampleLog) {
        err = "续传前置条件不满足";
        return false;
    }
    // 重放视频的衔接点 dts（新会话 dts 需衔接 + 音频尾部裁剪）。
    // 直接取"frameNo < 续传点 的视频条目最大 dts"（B 帧重排下解码序最后
    // 的帧才是衔接点，不能用日志最后一条）
    int64_t maxVideoDts = -1;
    for (const auto& e : g_sampleLog->entries) {
        if (e.video && e.frameNo >= 0 &&
            e.frameNo < static_cast<int64_t>(resumeFrame_) && e.dts > maxVideoDts)
            maxVideoDts = e.dts;
    }
    resumeDtsBase_ = maxVideoDts;
    if (!submitPartSamples(oldPart, *g_sampleLog, oc_, vst_,
                           ast_ ? ast_ : nullptr, replayedFrames_, err,
                           static_cast<int64_t>(resumeFrame_), info_.fpsDen,
                           false /* 重放音频 */))
        return false;
    replayed_ = true;
    return true;
}

// 续传会话：把新编码包的 dts 抬升到衔接点之上（避免时间轴回退）。
// 只抬升低于 重放末尾+1tick 的包：x265 重启后 dts 是绝对值（首个 GOP 的
// IDR dts = 帧 pts - 编码延迟），与重放末尾相等时 +1 tick 即可严格递增；
// 其余包保持编码器原始 dts——B 帧的 pts>=dts 关系不被破坏
// （movenc 6.1 无条件拒绝 pts<dts，也不写负 ctts；整体 +1001 平移
// 会让 ctts 为 0 的 B 帧变成负 ctts 而失败）
void EncodeSessionImpl::shiftResumeDts(AVPacket* pkt)
{
    if (!resumeSession_ || dumpOnly_)
        return;
    // 会话级整体平移：重编码区首个包（x265 重启后 dts 从编码器延迟偏移
    // 重新开始，远小于重放区最大 dts）锚定到 floor=重放最大 dts+1，其后
    // 所有包（含 flush 挂起帧）加同一偏移。dts 与 pts 同步平移 → ctts 不变，
    // dts 保持原始递增序（严格递增，movenc 拒绝相等 dts——"non monotonically
    // increasing dts: A >= A"）。绝不能用"逐包钳到 floor"——多个包会相等。
    const int64_t floor = resumeDtsBase_ + 1;
    if (!resumeShiftInit_) {
        resumeShiftInit_ = true;
        resumeShift_ = (pkt->dts < floor) ? (floor - pkt->dts) : 0;
        if (resumeShift_ > 0) {
            // 固有限制：movenc 6.1 双重约束（dts 严格递增 + pts>=dts 不可负
            // ctts）下，重放区最大 dts >= 重编码首包 dts 时只能"保 dts 单调 +
            // ctts、牺牲 pts 连续"——重编码区 pts 整体 +shift tick
            fprintf(stderr,
                    "警告: 续传点时间轴偏移 %lld tick（重放区 dts 高于重编码"
                    "首包 dts，pts 整体平移以保证 dts 单调且 ctts>=0）\n",
                    static_cast<long long>(resumeShift_));
        }
    }
    if (resumeShift_ > 0) {
        pkt->dts += resumeShift_;
        pkt->pts += resumeShift_;
    }
}

bool EncodeSessionImpl::finishSession(std::string& err)
{
    bool ok = true;
    if (oc_ && headerWritten_) {
        if (!flushEnc(err)) {
            err += " (保留部分产物 " + outPath_ + ")";
            ok = false;
            preservePartial_ = true;
        }
        if (ok) {
            // 关闭前先把缓冲推向存储：NFS 瞬时错误在此暴露并保留在 pb->error，
            // 之后 avio_close 会一并返回
            avio_flush(oc_->pb);
            if (oc_->pb->error < 0) {
                err = "收尾刷盘失败 (保留部分产物 " + outPath_ + ")";
                ok = false;
                preservePartial_ = true;
            }
        }
        if (av_write_trailer(oc_) < 0 && ok) {
            err = "写入 trailer 失败 (保留部分产物 " + outPath_ + ")";
            ok = false;
            preservePartial_ = true;
        }
    }
    if (oc_ && oc_->pb) {
        int r = avio_close(oc_->pb);
        oc_->pb = nullptr;
        if (r < 0 && ok) {
            char eb[128];
            av_strerror(r, eb, sizeof(eb));
            err = "关闭输出文件失败 (保留部分产物 " + outPath_ + "): " + eb +
                  diskSpaceNote();
            ok = false;
            preservePartial_ = true;
        }
    }
    // 收尾前把样本日志落盘：若本次失败，下次运行可自动续传
    flushSamples();
    if (ok && !dumpOnly_ && headerWritten_) {
        if (!syncAndRename(err)) {
            ok = false;
            preservePartial_ = true;
        }
    }
    // 收尾失败：用样本日志自动重建完整文件（moov 由 movenc 生成，保证正确）。
    // 这是对"数小时编码成果"的兜底——即使关闭/flush 失败，也能把 .part 恢复成
    // 可播放的完整文件，而不是让用户面对一个缺 moov 的残片。
    if (!ok && !dumpOnly_ && headerWritten_ && g_sampleLog &&
        !g_sampleLog->entries.empty()) {
        std::string rerr;
        std::string rec = finalPath_;
        if (::access(rec.c_str(), F_OK) == 0)
            rec += ".recovered";
        AVRational atb = ast_ ? ast_->time_base : AVRational{1, 48000};
        size_t recFrames = 0;
        if (tryRecoverMov(outPath_, rec, vst_->codecpar,
                          ast_ ? ast_->codecpar : nullptr, vst_->time_base, atb,
                          *g_sampleLog, recFrames, rerr)) {
            err += "；已自动重建完整文件 " + rec + "（视频 " +
                   std::to_string(recFrames) + " 帧，已验证可播放；" + outPath_ +
                   " 保留为备份）";
        } else {
            err += "；自动重建失败: " + rerr + "（可用 untrunc 修复或直接重跑）";
        }
    }
    finished_ = ok;
    // 收尾失败一律保留 .part 供人工抢救（自动重建成功后它只是备份），
    // 绝不删除——数小时编码成果不应被程序自己抹掉
    if (!ok && !dumpOnly_ && headerWritten_ && !preservePartial_)
        remove(outPath_.c_str());
    return ok;
}

bool EncodeSessionImpl::syncAndRename(std::string& err)
{
    // fsync 失败重试（NFS 瞬时抖动常见，重试通常可成功）
    bool synced = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int fd = open(outPath_.c_str(), O_RDONLY);
        if (fd >= 0) {
            int sr = fsync(fd);
            close(fd);
            if (sr == 0) {
                synced = true;
                break;
            }
        }
        if (attempt < 2)
            usleep(1000000);  // 1s 后重试
    }
    if (!synced) {
        err = "fsync 输出文件失败 (保留部分产物 " + outPath_ + ")";
        return false;
    }
    if (rename(outPath_.c_str(), finalPath_.c_str()) != 0) {
        err = "重命名输出文件失败 (保留部分产物 " + outPath_ + ")";
        return false;
    }
    std::string dir = finalPath_;
    size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
    if (dir.empty())
        dir = "/";
    int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

std::string EncodeSessionImpl::diskSpaceNote() const
{
    struct statvfs sv;
    std::string dir = finalPath_;
    size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? "." : dir.substr(0, slash);
    if (dir.empty())
        dir = "/";
    if (statvfs(dir.c_str(), &sv) == 0) {
        double freeGB = static_cast<double>(sv.f_bavail) *
                        static_cast<double>(sv.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        char buf[64];
        snprintf(buf, sizeof(buf), "; 磁盘剩余约 %.1f GB", freeGB);
        return buf;
    }
    return "";
}

bool tryRecoverMov(const std::string& partPath, const std::string& outPath,
                   AVCodecParameters* vpar, AVCodecParameters* apar,
                   const AVRational& vtb, const AVRational& atb,
                   const SampleLog& log, size_t& recoveredFrames, std::string& err)
{
    recoveredFrames = 0;
    FILE* in = fopen(partPath.c_str(), "rb");
    if (!in) {
        err = "无法打开 " + partPath;
        return false;
    }
    fseek(in, 0, SEEK_END);
    const long partSize = ftell(in);

    // 定位 mdat 数据区起点：复用 findMdatStart（正确处理 >4GB 的 64 位
    // 扩展头 sz==1 → 数据区在 pos+16；手写扫描漏掉该分支会让重建整体
    // 偏移 8 字节，样本数据全错位）
    const long mdatStart = findMdatStart(in, partSize);
    if (mdatStart < 0) {
        fclose(in);
        err = "文件中未找到 mdat";
        return false;
    }

    // 主路径用 av_write_frame 立即写入（无交织队列重排）：
    // 文件布局 = 提交顺序，每个样本偏移 = mdat 数据区起点 + 之前所有样本大小之和。
    // 布局由构造保证精确，不依赖任何运行时的偏移记录。
    struct Use {
        const SampleLog::Entry* e;
        int64_t off;
    };
    std::vector<Use> use;
    {
        int64_t cur = mdatStart;
        bool anyVideo = false;
        const long dataBound = mdatDataBound(in, partSize);
        for (const auto& e : log.entries) {
            if (e.size <= 0)
                continue;
            if (e.video)
                anyVideo = true;
            if (cur + e.size > dataBound)
                break;  // 尾部截断：之后的样本必然也不完整
            use.push_back({&e, cur});
            cur += e.size;
        }
        if (!anyVideo) {
            fclose(in);
            err = "文件中没有完整视频样本";
            return false;
        }
    }

    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mov", outPath.c_str()) < 0 || !oc) {
        fclose(in);
        err = "创建 MOV 封装器失败";
        return false;
    }
    oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    oc->max_interleave_delta = 0;  // 与主路径一致：立即写入、时间戳归一化一致
    AVStream* vs = avformat_new_stream(oc, nullptr);
    if (!vs || avcodec_parameters_copy(vs->codecpar, vpar) < 0) {
        fclose(in);
        avformat_free_context(oc);
        err = "视频流参数复制失败";
        return false;
    }
    vs->time_base = vtb;
    AVStream* as = nullptr;
    if (apar) {
        as = avformat_new_stream(oc, nullptr);
        if (!as || avcodec_parameters_copy(as->codecpar, apar) < 0) {
            fclose(in);
            avformat_free_context(oc);
            err = "音频流参数复制失败";
            return false;
        }
        as->time_base = atb;
    }
    if (avio_open(&oc->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        err = "无法创建 " + outPath;
        fclose(in);
        avformat_free_context(oc);
        return false;
    }
    if (avformat_write_header(oc, nullptr) < 0) {
        err = "写入重建文件 header 失败";
        avio_close(oc->pb);
        avformat_free_context(oc);
        fclose(in);
        return false;
    }

    std::vector<uint8_t> buf;
    // 视频包时长 = fpsDen tick（vtb=1/fpsNum，帧时长 = fpsDen/fpsNum s =
    // fpsDen tick）。不改变签名地由日志推导：排序后的视频 pts 相邻差值的
    // 众数即帧时长 fpsDen——对续传时间轴平移免疫（整体 +S 平移不改差值；
    // GCD 则会被破坏：GCD(1001, 1002)=1 → 恢复产物每帧时长 1 tick → 视频
    // 时长虚短千倍）。恢复产物与主路径一致地给每个包显式时长——否则最后一
    // 个视频样本 stts 时长为 0，mdhd 视频时长比全新编码少 1 帧
    int64_t vDuration = 0;
    {
        std::vector<int64_t> pts;
        for (const auto& e : log.entries) {
            if (e.video && e.pts > 0)
                pts.push_back(e.pts);
        }
        if (!pts.empty()) {
            std::sort(pts.begin(), pts.end());
            // 众数：统计相邻差值出现次数（帧数少时取唯一差值即可）
            int64_t best = 0;
            size_t bestCnt = 0;
            int64_t cur = 0;
            size_t curCnt = 0;
            for (size_t i = 1; i < pts.size(); ++i) {
                const int64_t d = pts[i] - pts[i - 1];
                if (d <= 0)
                    continue;
                if (d == cur) {
                    ++curCnt;
                } else {
                    cur = d;
                    curCnt = 1;
                }
                if (curCnt > bestCnt) {
                    bestCnt = curCnt;
                    best = cur;
                }
            }
            vDuration = best;
        }
        if (vDuration <= 0)
            vDuration = 1;  // 单帧日志等极端情况：兜底 1 tick
    }
    for (const auto& u : use) {
        if (u.off < 0 || u.off + u.e->size > partSize)
            continue;  // 尾部截断：跳过未完整落盘的样本
        AVStream* st = u.e->video ? vs : as;
        buf.resize(static_cast<size_t>(u.e->size));
        fseek(in, static_cast<long>(u.off), SEEK_SET);
        if (fread(buf.data(), 1, buf.size(), in) != buf.size())
            continue;
        AVPacket* pkt = av_packet_alloc();
        if (!pkt || av_new_packet(pkt, static_cast<int>(u.e->size)) < 0) {
            av_packet_free(&pkt);
            continue;
        }
        memcpy(pkt->data, buf.data(), buf.size());
        pkt->pts = u.e->pts;
        pkt->dts = u.e->dts;
        pkt->stream_index = st->index;
        // 与 submitPartSamples/writeAudioPkt 相同的时长约定：
        // 视频 = fpsDen tick；音频 = 实际采样数 size/(channels*3)
        if (u.e->video) {
            pkt->duration = vDuration;
        } else if (as && as->codecpar->ch_layout.nb_channels > 0) {
            pkt->duration =
                static_cast<int64_t>(u.e->size) /
                static_cast<int64_t>(as->codecpar->ch_layout.nb_channels * 3);
        }
        if (u.e->key)
            pkt->flags |= AV_PKT_FLAG_KEY;
        // 必须用 av_write_frame（立即写入、提交顺序落盘）：
        // av_interleaved_write_frame 会按 dts 重排（V0,V1,A0,V2...），
        // 使文件布局与样本日志（提交顺序）不一致，顺序推导/自动重建全部错位
        if (av_write_frame(oc, pkt) < 0) {
            av_packet_free(&pkt);
            break;
        }
        av_packet_free(&pkt);
        if (u.e->video)
            ++recoveredFrames;
    }
    av_write_trailer(oc);
    avio_close(oc->pb);
    avformat_free_context(oc);
    fclose(in);

    // 验证重建产物可读
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, outPath.c_str(), nullptr, nullptr) < 0) {
        err = "重建产物无法读取";
        return false;
    }
    bool ok = false;
    if (avformat_find_stream_info(fc, nullptr) >= 0) {
        for (unsigned k = 0; k < fc->nb_streams; ++k) {
            if (fc->streams[k]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                ok = true;
                break;
            }
        }
    }
    avformat_close_input(&fc);
    if (!ok) {
        err = "重建产物缺少视频流";
        return false;
    }
    return true;
}

bool EncodeSession::open(const CliOptions& opt, const MediaInfo& info, std::string& err)
{
    if (impl_)
        return true;
    EncodeSessionImpl* s = new EncodeSessionImpl();
    if (!s->openSession(opt, info, err)) {
        delete s;
        if (opt.dumpRef.empty())
            remove((opt.output + ".part").c_str());
        else
            remove(opt.dumpRef.c_str());
        return false;
    }
    impl_ = s;
    return true;
}

// 续传：把旧 .part 的已编码样本重放进本会话（复用数据，不重新编码）
bool EncodeSession::replayOldSamples(const std::string& oldPart, std::string& err)
{
    if (!impl_)
        return false;
    return static_cast<EncodeSessionImpl*>(impl_)->replayOldSamples(oldPart, err);
}

size_t EncodeSession::replayedFrames() const
{
    if (!impl_)
        return 0;
    return static_cast<EncodeSessionImpl*>(impl_)->replayedFrames();
}

void EncodeSession::flushSamples()
{
    if (impl_)
        static_cast<EncodeSessionImpl*>(impl_)->flushSamples();
}

bool EncodeSession::flushEnc(std::string& err)
{
    if (!impl_)
        return true;
    return static_cast<EncodeSessionImpl*>(impl_)->flushEnc(err);
}

bool EncodeSession::write(const VideoFrame& f, std::string& err)
{
    if (!impl_)
        return false;
    return static_cast<EncodeSessionImpl*>(impl_)->writeFrame(f, err);
}

bool EncodeSession::writeAudio(const AudioPacket& p, std::string& err)
{
    if (!impl_)
        return false;
    return static_cast<EncodeSessionImpl*>(impl_)->writeAudioPkt(p, err);
}

bool EncodeSession::finish(std::string& err)
{
    if (!impl_)
        return false;
    bool ok = static_cast<EncodeSessionImpl*>(impl_)->finishSession(err);
    delete static_cast<EncodeSessionImpl*>(impl_);
    impl_ = nullptr;
    return ok;
}

void EncodeSession::setHardAbort()
{
    if (impl_)
        static_cast<EncodeSessionImpl*>(impl_)->setHardAbort();
}

void EncodeSession::cleanup()
{
    if (!impl_)
        return;
    delete static_cast<EncodeSessionImpl*>(impl_);
    impl_ = nullptr;
}

EncodeSessionImpl::~EncodeSessionImpl()
{
    if (samplesFile_) {
        fclose(samplesFile_);
        samplesFile_ = nullptr;
    }
    if (hardAbort_) {
        // 硬中断（模拟真实 SIGINT/_exit）：不写 trailer。注意 avio_close 会
        // 冲刷 avio 用户态缓冲（比真实 _exit 略"软"），.part 多保留 ≤32KB
        // 数据——续传靠 countCompleteSamples 对"日志超前 .part"的截断兜底，
        // 多出的落盘数据不会造成不一致
        if (oc_ && oc_->pb) {
            avio_close(oc_->pb);
            oc_->pb = nullptr;
        }
    } else {
        if (oc_ && oc_->pb && headerWritten_ && !finished_)
            av_write_trailer(oc_);
        if (oc_ && oc_->pb) {
            avio_close(oc_->pb);
            oc_->pb = nullptr;
        }
    }
    av_frame_free(&srcFrame_);
    av_frame_free(&dstFrame_);
    if (enc_)
        avcodec_free_context(&enc_);
    if (oc_)
        avformat_free_context(oc_);
    if (sws_)
        sws_freeContext(sws_);
    if (dump_)
        fclose(dump_);
    // 中途失败也保留 .part（帧数据完整，moov 已落盘时可播放）。
    // 打开阶段失败（header 未写入）由 EncodeSession::open 负责清理；
    // 信号中断由 main 的信号处理器按文档清理。
}

int encodeRun(const CliOptions& opt, const MediaInfo& info,
              FrameQueue& frames, AudioQueue& audio,
              std::atomic<bool>& abort, std::string& failDetail)
{
    const bool dumpOnly = !opt.dumpRef.empty();
    const bool audioOn = !dumpOnly && !opt.noAudio && info.audio.present;

    std::string err;
    EncodeSession session;
    if (!session.open(opt, info, err)) {
        failDetail = err;
        abort.store(true);
        frames.setEof();
        audio.setEof();
        return 4;
    }

    size_t frameCount = info.frameCount;
    if (opt.maxFrames >= 0 && static_cast<size_t>(opt.maxFrames) < frameCount)
        frameCount = static_cast<size_t>(opt.maxFrames);

    // 会话打开后立即写初始检查点：全新编码在前 500 帧内被信号中断时
    // 磁盘上已有有效 .ckpt（waitHash=false 不阻塞编码；sha 可能为空，
    // detectResume 对空 sha 跳过校验，500 帧后检查点带上完整 sha）。
    // 计数仅供参考，detectResume 以 countCompleteSamples 校准 .part
    {
        std::string cerr_;
        nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                              *nraw::g_sampleLog, cerr_, false);
    }

    // 续传：先重放旧 .part 的已编码样本（复用数据，不重新编码）
    if (opt.resumeMode && opt.resumeFrame > 0) {
        if (!session.replayOldSamples(opt.output + ".part.old", err)) {
            failDetail = err;
            abort.store(true);
            frames.setEof();
            audio.setEof();
            return 4;
        }
        fprintf(stderr, "续传: 已复用 %zu 帧已编码数据，继续编码剩余帧\n",
                session.replayedFrames());
    }

    // 音频写入上限 = min(视频实际编码帧数对应的采样数, 剪辑自身音频采样数)，
    // 防止音视频时长失配（如 --frames 截断或音频长于视频）时写入超出视频长度
    // 的音频；跨越上限的最后一个块按采样数截断（与 CPU 路径 runCpuPath 一致）。
    const unsigned long long drainLimit =
        static_cast<unsigned long long>(frameCount) *
        static_cast<unsigned long long>(info.audio.sampleRate) *
        static_cast<unsigned long long>(info.fpsDen ? info.fpsDen : 1) /
        static_cast<unsigned long long>(info.fpsNum ? info.fpsNum : 1);
    const size_t perChA = static_cast<size_t>(info.audio.channels) * 3;

    AudioPacket pending;
    bool hasPending = false;
    size_t done = 0;
    bool writeOk = true;
    size_t flushCounter = 0;
    size_t ckCounter = 0;

    auto tStart = std::chrono::steady_clock::now();
    auto tLast = tStart;

    for (;;) {
        std::unique_ptr<VideoFrame> f = frames.pop();
        if (!f)
            break;
        if (audioOn) {
            int64_t targetSamples = static_cast<int64_t>(f->frameNo) *
                                    static_cast<int64_t>(info.audio.sampleRate) *
                                    static_cast<int64_t>(info.fpsDen) /
                                    static_cast<int64_t>(info.fpsNum);
            if (!hasPending)
                hasPending = audio.tryPop(pending);
            while (hasPending &&
                   static_cast<int64_t>(pending.firstSample) < targetSamples) {
                if (perChA > 0 &&
                    drainLimit < pending.firstSample + pending.bytes.size() / perChA)
                    pending.bytes.resize(
                        static_cast<size_t>(drainLimit - pending.firstSample) * perChA);
                if (!session.writeAudio(pending, err)) {
                    writeOk = false;
                    break;
                }
                hasPending = audio.tryPop(pending);
            }
            if (!writeOk)
                break;
        }
        if (!session.write(*f, err)) {
            writeOk = false;
            break;
        }
        ++done;
        // 样本日志每 20 帧 flushSamples（fflush+fsync，< 编码器 lookahead）：
        // SIGINT/_exit 丢 stdio 缓冲——日志不落盘则续传无法用 countCompleteSamples
        // 对齐 .part 的已落盘数据，只能全新编码；20 帧粒度 fsync 开销可接受；
        // 检查点保持 500 帧
        if (++flushCounter >= 20) {
            flushCounter = 0;
            session.flushSamples();
        }
        if (++ckCounter >= 500) {
            ckCounter = 0;
            std::string cerr_;
            // waitHash=false：不阻塞编码等待 270GB 哈希（哈希在后台算，
            // 完成前检查点 sha 为空——detectResume 对空 sha 跳过校验；
            // 收尾 sidecar 由 main 等待哈希并记录完整 sha）
            nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                                  *nraw::g_sampleLog, cerr_, false);
        }
        if (opt.testStopAfter >= 0 &&
            done >= static_cast<size_t>(opt.testStopAfter)) {
            // 测试钩子：模拟中断（保留 .part/.samples/.ckpt 供续传测试）。
            if (getenv("NRAW_HARD_INT") == nullptr) {
                // 干净中断：flush 编码器（lookahead 帧包此刻才产出）+ 日志 + 检查点
                if (!session.flushEnc(err))
                    writeOk = false;
                session.flushSamples();
                std::string cerr_;
                nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                                      *nraw::g_sampleLog, cerr_, false);
            } else {
                // 硬中断（模拟真实 SIGINT/_exit）：不 flush 编码器、不写日志、
                // 不写检查点，且析构跳过 trailer/avio 落盘（丢 avio 缓冲）——
                // .part 只有已落盘数据，续传靠 countCompleteSamples 校准
                session.setHardAbort();
                fprintf(stderr, "\n[测试] 硬中断（模拟 SIGINT，不 flush）\n");
            }
            writeOk = false;
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - tLast >= std::chrono::seconds(2)) {
            double dt = std::chrono::duration<double>(now - tStart).count();
            double fps = dt > 0.0 ? static_cast<double>(done) / dt : 0.0;
            fprintf(stderr, "\r%s",
                    nraw::progressLine(done, frameCount, info.fpsNum,
                                       info.fpsDen, fps)
                        .c_str());
            tLast = now;
        }
    }

    if (writeOk && audioOn) {
        while (hasPending || audio.pop(pending)) {
            if (static_cast<unsigned long long>(pending.firstSample) >= drainLimit)
                break;
            if (perChA > 0 &&
                drainLimit < pending.firstSample + pending.bytes.size() / perChA)
                pending.bytes.resize(
                    static_cast<size_t>(drainLimit - pending.firstSample) * perChA);
            if (!session.writeAudio(pending, err)) {
                writeOk = false;
                break;
            }
            hasPending = false;
        }
    }

    if (writeOk) {
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - tStart).count();
        double fps = dt > 0.0 ? static_cast<double>(done) / dt : 0.0;
        fprintf(stderr, "\r%s  done.\n",
                nraw::progressLine(done, frameCount, info.fpsNum,
                                   info.fpsDen, fps)
                    .c_str());
    }

    if (writeOk && abort.load()) {
        writeOk = false;
        failDetail = "处理被上游中止（GPU 管线失败）";
    }
    if (writeOk) {
        if (!session.finish(err)) {
            failDetail = err;
            abort.store(true);
            return 4;
        }
        return 0;
    }
    failDetail = err.empty() ? "编码或写入失败" : err;
    session.cleanup();
    abort.store(true);
    frames.setEof();
    audio.setEof();
    return 4;
}

}
