#include "archive.h"
#include "sha256.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

using namespace nraw;

static const size_t W = 640;
static const size_t H = 360;
static const size_t FRAMES = 120;
static const size_t FPS_NUM = 60000;
static const size_t FPS_DEN = 1001;
static const int SR = 48000;
static const int CH = 2;
static const size_t BLOCK_SAMPLES = 4800;

static void makeFrame(VideoFrame& f, size_t no)
{
    f.width = W;
    f.height = H;
    f.frameNo = no;
    f.rgb.resize(W * H * 6, 16);
    if (!f.rgb.data() || f.rgb.size() != W * H * 6) {
        fprintf(stderr, "内存分配失败 (makeFrame)\n");
        exit(2);
    }
    uint16_t* base = static_cast<uint16_t*>(f.rgb.data());
    for (size_t i = 0; i < W * H; ++i) {
        base[i] = static_cast<uint16_t>((i * 7 + no * 13) & 0x3FFF);
        base[W * H + i] = static_cast<uint16_t>((i * 5 + no * 17) & 0x3FFF);
        base[2 * W * H + i] = static_cast<uint16_t>((i * 3 + no * 19) & 0x3FFF);
    }
}

static void makeAudioPacket(AudioPacket& p, size_t firstSample)
{
    p.firstSample = firstSample;
    p.bytes.resize(BLOCK_SAMPLES * CH * 3);
    uint8_t* d = p.bytes.data();
    for (size_t k = 0; k < BLOCK_SAMPLES; ++k) {
        size_t g = firstSample + k;
        for (int c = 0; c < CH; ++c) {
            uint32_t v = static_cast<uint32_t>(g * 2654435761ULL + c) & 0xFFFFFFu;
            size_t o = (k * CH + c) * 3;
            d[o] = static_cast<uint8_t>(v & 0xFF);
            d[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            d[o + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
    }
}

// 测试帧源：从 startFrame/startAudioSample 开始生成（续传测试），最多 limitFrames 帧
static void producer(const CliOptions& opt, MediaInfo& info,
                     FrameQueue& frames, AudioQueue& audio,
                     size_t startFrame = 0, unsigned long long startAudioSample = 0,
                     size_t limitFrames = FRAMES)
{
    const bool audioOn = !opt.noAudio && opt.dumpRef.empty() && info.audio.present;
    size_t blockIdx = startAudioSample / BLOCK_SAMPLES;
    unsigned long long decodedSamples = startAudioSample;
    size_t totalBlocks = (info.audio.samplesPerChannel + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES;
    const size_t endFrame = startFrame + limitFrames < FRAMES ? startFrame + limitFrames
                                                              : FRAMES;

    auto pushWindow = [&](unsigned long long target) {
        if (!audioOn)
            return;
        while (blockIdx < totalBlocks && decodedSamples < target) {
            AudioPacket p;
            makeAudioPacket(p, static_cast<size_t>(decodedSamples));
            audio.push(std::move(p));
            decodedSamples += BLOCK_SAMPLES;
            ++blockIdx;
        }
    };

    for (size_t no = startFrame; no < endFrame; ++no) {
        unsigned long long target = static_cast<unsigned long long>(no + 1) *
                                    static_cast<unsigned long long>(SR) *
                                    static_cast<unsigned long long>(FPS_DEN) /
                                    static_cast<unsigned long long>(FPS_NUM);
        pushWindow(target);
        std::unique_ptr<VideoFrame> f(new VideoFrame());
        makeFrame(*f, no);
        frames.push(std::move(f));
    }
    if (audioOn) {
        while (blockIdx < totalBlocks) {
            AudioPacket p;
            makeAudioPacket(p, static_cast<size_t>(decodedSamples));
            audio.push(std::move(p));
            decodedSamples += BLOCK_SAMPLES;
            ++blockIdx;
        }
    }
    frames.setEof();
    audio.setEof();
}

static bool testRepack()
{
    const uint32_t words[] = {
        0x00800000u, 0x00008000u, 0x00000080u, 0x12345600u, 0xFFFFFF00u, 0x00000000u};
    const uint8_t expect[] = {
        0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
        0x34, 0x56, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    uint8_t dst[64] = {0};
    size_t n = repack24beToS24le(words, 6, dst);
    if (n != 18)
        return false;
    return memcmp(dst, expect, 18) == 0;
}

// SHA-256 已知向量（FIPS 180-4 附录 / 标准测试向量）
static bool testSha256()
{
    struct Vec { const char* data; size_t len; const char* hex; };
    static const Vec vecs[] = {
        {"", 0,
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", 3,
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"The quick brown fox jumps over the lazy dog", 43,
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
        // 56 字节：恰好触发填充边界
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        // 112 字节：两整块 + 填充
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
         "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
    };
    for (const auto& v : vecs) {
        std::string got = sha256Hex(v.data, v.len);
        if (got != v.hex) {
            fprintf(stderr, "FAIL: sha256(\"%s\") = %s, 期望 %s\n",
                    v.data, got.c_str(), v.hex);
            return false;
        }
    }
    // 文件哈希路径：mkstemp 避免并行 ctest 实例争用固定路径
    char path[] = "/tmp/nraw_sha256_test_XXXXXX";
    const int tfd = mkstemp(path);
    if (tfd < 0)
        return false;
    FILE* f = fdopen(tfd, "wb");
    if (!f) {
        ::close(tfd);
        unlink(path);
        return false;
    }
    const char* content = "abc";
    if (fwrite(content, 1, 3, f) != 3) {
        fclose(f);
        return false;
    }
    fclose(f);
    std::string got = sha256File(path);
    remove(path);
    if (got != vecs[1].hex) {
        fprintf(stderr, "FAIL: sha256File = %s\n", got.c_str());
        return false;
    }
    return true;
}

// 模拟收尾失败（moov 丢失）→ 自动重建 → 验证重建文件完整可播放
// 用法: nraw_test_encode repair-test <out.mov>
// 流程: 正常 encodeRun（记录样本日志）→ 截掉 moov 模拟 .part → tryRecoverMov 重建
//       → avformat 验证视频帧数/时长与原始文件一致
static int testRepair(const std::string& out)
{
    SampleLog log;
    g_sampleLog = &log;

    CliOptions opt;
    opt.output = out;
    MediaInfo info;
    info.width = W;
    info.height = H;
    info.frameCount = FRAMES;
    info.fpsNum = FPS_NUM;
    info.fpsDen = FPS_DEN;
    info.audio.present = true;
    info.audio.sampleRate = SR;
    info.audio.bits = 24;
    info.audio.channels = CH;
    info.audio.samplesPerChannel = 96000;
    info.audio.blockCount = 20;

    FrameQueue frames(16);
    AudioQueue audio(128);
    std::atomic<bool> abort(false);
    std::string encFail;
    std::thread prod(producer, std::ref(opt), std::ref(info),
                     std::ref(frames), std::ref(audio), 0, 0ULL, FRAMES);
    int rc = encodeRun(opt, info, frames, audio, abort, encFail);
    prod.join();
    if (rc != 0) {
        fprintf(stderr, "FAIL: repair-test 编码失败: %s\n",
                encFail.empty() ? "?" : encFail.c_str());
        return 1;
    }
    size_t videoLogged = 0;
    for (const auto& e : log.entries)
        if (e.video)
            ++videoLogged;
    if (videoLogged != FRAMES) {
        fprintf(stderr, "FAIL: 样本日志帧数 %zu != %zu\n", videoLogged, FRAMES);
        return 1;
    }
    if (getenv("NRAW_LOG_DUMP")) {
        for (size_t k = 0; k < 8 && k < log.entries.size(); ++k) {
            const auto& e = log.entries[k];
            fprintf(stderr, "[log] %s off=%lld size=%lld pts=%lld dts=%lld\n",
                    e.video ? "v" : "a", (long long)e.offset,
                    (long long)e.size, (long long)e.pts, (long long)e.dts);
        }
        fprintf(stderr, "[log] entries=%zu\n", log.entries.size());
    }

    // 捕获原始文件的流参数（作为重建模板）
    AVFormatContext* src = nullptr;
    if (avformat_open_input(&src, out.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(src, nullptr) < 0) {
        fprintf(stderr, "FAIL: 无法读取原始文件\n");
        return 1;
    }
    AVStream* vs = nullptr, *as = nullptr;
    for (unsigned i = 0; i < src->nb_streams; ++i) {
        if (src->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !vs)
            vs = src->streams[i];
        else if (src->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !as)
            as = src->streams[i];
    }
    if (!vs) {
        fprintf(stderr, "FAIL: 原始文件无视频流\n");
        return 1;
    }

    // 截掉 moov，模拟收尾失败后的 .part（仅 ftyp+mdat）
    FILE* f = fopen(out.c_str(), "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    long moovOff = -1;
    fseek(f, 0, SEEK_SET);
    long pos = 0;
    while (pos + 8 <= fileSize) {
        uint32_t sz = 0;
        char typ[5] = {0};
        if (fread(&sz, 4, 1, f) != 1 || fread(typ, 4, 1, f) != 1)
            break;
        sz = __builtin_bswap32(sz);
        if (strcmp(typ, "moov") == 0) {
            moovOff = pos;
            break;
        }
        if (sz < 8)
            break;
        fseek(f, sz - 8, SEEK_CUR);
        pos += sz;
    }
    fclose(f);
    if (moovOff < 0) {
        fprintf(stderr, "FAIL: 未找到 moov\n");
        return 1;
    }
    const std::string part = out + ".part.sim";
    {
        FILE* in = fopen(out.c_str(), "rb");
        FILE* pf = fopen(part.c_str(), "wb");
        char buf[65536];
        long left = moovOff;
        while (left > 0) {
            size_t n = fread(buf, 1, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf), in);
            if (n == 0)
                break;
            fwrite(buf, 1, n, pf);
            left -= (long)n;
        }
        fclose(pf);
        fclose(in);
    }

    // 自动重建
    const std::string rec = out + ".recovered";
    std::string rerr;
    size_t recFrames = 0;
    if (!tryRecoverMov(part, rec, vs->codecpar, as ? as->codecpar : nullptr,
                       vs->time_base, as ? as->time_base : AVRational{1, 48000},
                       log, recFrames, rerr)) {
        fprintf(stderr, "FAIL: 重建失败: %s\n", rerr.c_str());
        return 1;
    }
    if (recFrames != FRAMES) {
        fprintf(stderr, "FAIL: 重建帧数 %zu != %zu\n", recFrames, FRAMES);
        return 1;
    }

    // 验证重建文件
    AVFormatContext* rfc = nullptr;
    if (avformat_open_input(&rfc, rec.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(rfc, nullptr) < 0) {
        fprintf(stderr, "FAIL: 重建文件无法读取\n");
        return 1;
    }
    long long vFrames = -1, aPkts = -1;
    for (unsigned i = 0; i < rfc->nb_streams; ++i) {
        AVStream* st = rfc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            vFrames = st->nb_frames;
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            aPkts = st->nb_frames;
    }
    avformat_close_input(&rfc);
    if (vFrames != (long long)FRAMES) {
        fprintf(stderr, "FAIL: 重建文件视频帧数 %lld != %zu\n", vFrames, FRAMES);
        return 1;
    }
    if (aPkts < 0) {
        fprintf(stderr, "FAIL: 重建文件缺少音频流\n");
        return 1;
    }

    // ---- 尾部截断变体：截掉最后 20 帧（含交错音频）再重建 ----
    // 覆盖 tryRecoverMov 的尾部截断分支（cur+size > bound → break）：
    // recoveredFrames 必须精确等于截断点前的完整视频条目数，且重建产物
    // 可读、帧数一致（截断不应静默产出"看起来完整"的垃圾）
    {
        FILE* pf = fopen(part.c_str(), "rb");
        if (!pf) {
            fprintf(stderr, "FAIL: 无法打开 %s\n", part.c_str());
            return 1;
        }
        fseek(pf, 0, SEEK_END);
        const long psz = ftell(pf);
        const long mStart = nraw::findMdatStart(pf, psz);
        fclose(pf);
        if (mStart < 0) {
            fprintf(stderr, "FAIL: 截断变体找不到 mdat\n");
            return 1;
        }
        // 截断点 = 第一个 frameNo >= FRAMES-20 的视频条目起点（提交序=布局）
        long long cut = mStart;
        size_t kept = 0;
        for (const auto& e : log.entries) {
            if (e.size <= 0)
                continue;
            if (e.video && e.frameNo >= static_cast<int64_t>(FRAMES - 20))
                break;
            cut += e.size;
            if (e.video)
                ++kept;
        }
        const std::string tpart = out + ".part.trunc";
        {
            FILE* in = fopen(part.c_str(), "rb");
            FILE* tf = fopen(tpart.c_str(), "wb");
            char buf[65536];
            long left = static_cast<long>(cut);
            while (left > 0) {
                size_t n = fread(buf, 1, left < (long)sizeof(buf) ? (size_t)left : sizeof(buf), in);
                if (n == 0)
                    break;
                fwrite(buf, 1, n, tf);
                left -= (long)n;
            }
            fclose(tf);
            fclose(in);
        }
        const std::string trec = out + ".recovered.trunc";
        std::string rerr2;
        size_t trecFrames = 0;
        if (!tryRecoverMov(tpart, trec, vs->codecpar, as ? as->codecpar : nullptr,
                           vs->time_base, as ? as->time_base : AVRational{1, 48000},
                           log, trecFrames, rerr2)) {
            fprintf(stderr, "FAIL: 截断重建失败: %s\n", rerr2.c_str());
            return 1;
        }
        if (trecFrames != kept) {
            fprintf(stderr, "FAIL: 截断重建帧数 %zu != 预期 %zu\n", trecFrames, kept);
            return 1;
        }
        AVFormatContext* tfc = nullptr;
        if (avformat_open_input(&tfc, trec.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(tfc, nullptr) < 0) {
            fprintf(stderr, "FAIL: 截断重建产物无法读取\n");
            return 1;
        }
        long long tvf = -1;
        for (unsigned i = 0; i < tfc->nb_streams; ++i) {
            if (tfc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                tvf = tfc->streams[i]->nb_frames;
        }
        avformat_close_input(&tfc);
        if (tvf != static_cast<long long>(kept)) {
            fprintf(stderr, "FAIL: 截断重建产物帧数 %lld != %zu\n", tvf, kept);
            return 1;
        }
        if (getenv("NRAW_KEEP_REPAIR") == nullptr) {
            remove(tpart.c_str());
            remove(trec.c_str());
        }
        fprintf(stderr, "截断变体: 重建 %zu 帧（截断后），帧数一致\n", trecFrames);
    }

    if (getenv("NRAW_KEEP_REPAIR") == nullptr) {
        remove(part.c_str());
        remove(rec.c_str());
        // encodeRun 会话残留的续传产物一并清理（否则手工运行污染目录；
        // 脚本 TMP 由 trap 兜底，但二进制直接运行应自净）
        remove((out + ".samples").c_str());
        remove((out + ".ckpt").c_str());
    }
    fprintf(stderr, "PASS: repair-test 重建 %zu 帧视频 + 音频，帧数/流完整\n",
            recFrames);
    return 0;
}

static MediaInfo testInfo()
{
    MediaInfo info;
    info.width = W;
    info.height = H;
    info.frameCount = FRAMES;
    info.fpsNum = FPS_NUM;
    info.fpsDen = FPS_DEN;
    info.audio.present = true;
    info.audio.sampleRate = SR;
    info.audio.bits = 24;
    info.audio.channels = CH;
    info.audio.samplesPerChannel = 96000;
    info.audio.blockCount = 20;
    return info;
}

// 断点续传测试：编码 60 帧后模拟中断 → 自动检测 → 续传完成 → 验证完整
static int testResume(const std::string& out)
{
    CliOptions opt;
    opt.output = out;
    MediaInfo info = testInfo();

    // 关键帧间隔 30 + closed GOP：中断在 90 帧（已写入约 58 帧），
    // 续传点：closed-GOP 关键帧对齐；open-GOP 回退 17 帧
    opt.keyint = 30;
    opt.openGop = 0;
    if (getenv("NRAW_TEST_OPEN_GOP"))
        opt.openGop = 1;  // open-GOP 变体（默认配置）
    if (getenv("NRAW_TEST_FASTSTART"))
        opt.faststart = 1;  // faststart 续传变体（收尾重写 moov 前置）
    const size_t stopAfter = 90;

    // ===== 阶段0: 早期中断变体（vc==0 分支）=====
    // NRAW_TEST_EARLY_INT=1：编码 2 帧后硬中断 → .part 无完整样本 →
    // detectResume 应返回 0（"早期中断"），随后全新编码续跑至完成
    if (getenv("NRAW_TEST_EARLY_INT")) {
        SampleLog slog;
        g_sampleLog = &slog;
        opt.testStopAfter = 2;
        opt.sourceSha256 = "SHA-ORIGINAL-0123456789abcdef0123456789abcdef";
        FrameQueue frames(16);
        AudioQueue audio(128);
        std::atomic<bool> abort(false);
        std::string encFail;
        std::thread prod(producer, std::ref(opt), std::ref(info),
                         std::ref(frames), std::ref(audio), 0, 0ULL, 2);
        int rc = encodeRun(opt, info, frames, audio, abort, encFail);
        prod.join();
        opt.testStopAfter = -1;
        if (rc != 4) {
            fprintf(stderr, "FAIL: 早期中断 rc=%d != 4\n", rc);
            return 1;
        }
        std::string rmsg;
        const int rdet = nraw::detectResume(opt, info, rmsg);
        if (rdet != 0) {
            fprintf(stderr, "FAIL: 早期中断应 rdet==0，实际 rdet=%d: %s\n",
                    rdet, rmsg.c_str());
            return 1;
        }
        // 按全新编码续跑至完成（清理旧产物 + 复位续传字段）
        remove((out + ".part.old").c_str());
        remove((out + ".part").c_str());
        remove((out + ".samples").c_str());
        remove((out + ".ckpt").c_str());
        opt.resumeMode = false;
        opt.resumeFrame = 0;
        opt.resumeAudioSample = 0;
        slog.entries.clear();
        FrameQueue f2(16);
        AudioQueue a2(128);
        std::atomic<bool> ab2(false);
        std::string ef2;
        std::thread p2(producer, std::ref(opt), std::ref(info),
                       std::ref(f2), std::ref(a2), 0, 0ULL, FRAMES);
        rc = encodeRun(opt, info, f2, a2, ab2, ef2);
        p2.join();
        if (rc != 0) {
            fprintf(stderr, "FAIL: 早期中断后全新编码 rc=%d: %s\n", rc,
                    ef2.c_str());
            return 1;
        }
        remove((out + ".samples").c_str());
        remove((out + ".ckpt").c_str());
        fprintf(stderr, "PASS: resume-test 早期中断（vc==0 → rdet==0 → 全新编码）完成\n");
        return 0;
    }

    auto sortLog = [](SampleLog& slog) {
        // 与 purgePartFile 内部排序一致（原始 dts + 视频在前）：
        // 会话采用直写（av_write_frame），文件布局 = 提交顺序 = 日志顺序，
        // 排序后重放按此顺序读取 .part.old
        std::stable_sort(slog.entries.begin(), slog.entries.end(),
                         [](const SampleLog::Entry& a, const SampleLog::Entry& b) {
                             if (a.dts != b.dts)
                                 return a.dts < b.dts;
                             return a.video && !b.video;
                         });
    };

    // ===== 阶段1: 编码 stopAfter 帧后模拟中断（保留 .part/.samples/.ckpt）=====
    {
        opt.testStopAfter = static_cast<long>(stopAfter);
        // 写入带源文件 sha 的检查点：阶段1.5 用源文件 sha 变更的拒绝路径
        // （真实程序里 sha 来自后台哈希线程；测试直接预设固定值）
        opt.sourceSha256 = "SHA-ORIGINAL-0123456789abcdef0123456789abcdef";
        SampleLog slog;
        g_sampleLog = &slog;
        FrameQueue frames(16);
        AudioQueue audio(128);
        std::atomic<bool> abort(false);
        std::string encFail;
        std::thread prod(producer, std::ref(opt), std::ref(info),
                         std::ref(frames), std::ref(audio), 0, 0ULL, stopAfter);
        int rc = encodeRun(opt, info, frames, audio, abort, encFail);
        prod.join();
        opt.testStopAfter = -1;
        if (rc != 4) {
            fprintf(stderr, "FAIL: 中断编码 rc=%d != 4: %s\n", rc, encFail.c_str());
            return 1;
        }
        if (access((out + ".part").c_str(), F_OK) != 0 ||
            access((out + ".samples").c_str(), F_OK) != 0 ||
            access((out + ".ckpt").c_str(), F_OK) != 0) {
            fprintf(stderr, "FAIL: 中断后缺少部分产物\n");
            return 1;
        }
        if (slog.videoCount() == 0 || slog.videoCount() > stopAfter) {
            fprintf(stderr, "FAIL: 中断时样本日志视频数 %zu（应在 1..%zu）\n",
                    slog.videoCount(), stopAfter);
            return 1;
        }
        fprintf(stderr, "阶段1: 中断于 %zu/%zu 帧（已写入 %zu 视频样本）\n",
                stopAfter, FRAMES, slog.videoCount());
    }

    // ===== 阶段1.5: 拒绝续传路径（rdet==2）——settings 指纹 / 源文件 sha 变更必须拒绝 =====
    {
        std::string rmsg;
        // a) 编码参数变更（crf）→ settingsHash 不匹配 → 拒绝
        const long savedCrf = opt.crf;
        opt.crf = savedCrf + 1;
        int rdet = nraw::detectResume(opt, info, rmsg);
        opt.crf = savedCrf;
        if (rdet != 2) {
            fprintf(stderr, "FAIL: 参数变更应拒绝续传 rdet=%d（应 2）: %s\n",
                    rdet, rmsg.c_str());
            return 1;
        }
        // b) 源文件 sha 变更（阶段1 检查点已带 SHA-ORIGINAL）→ 拒绝
        const std::string savedSha = opt.sourceSha256;
        opt.sourceSha256 = "SHA-DIFFERENT-00000000000000000000000000000000";
        rdet = nraw::detectResume(opt, info, rmsg);
        opt.sourceSha256 = savedSha;
        if (rdet != 2) {
            fprintf(stderr, "FAIL: 源文件 sha 变更应拒绝续传 rdet=%d（应 2）: %s\n",
                    rdet, rmsg.c_str());
            return 1;
        }
        // c) 全部恢复 → 续传仍可用（rdet==1）；副作用 resume 字段由阶段2 自己的
        //    detectResume 重新计算覆盖，无需重置
        rdet = nraw::detectResume(opt, info, rmsg);
        if (rdet != 1) {
            fprintf(stderr, "FAIL: 恢复后应可续传 rdet=%d（应 1）: %s\n",
                    rdet, rmsg.c_str());
            return 1;
        }
        fprintf(stderr, "阶段1.5: 拒绝续传路径（参数指纹/sha 变更 → rdet==2）验证通过\n");
    }

    // ===== 阶段2: 首次续传 + 编码 resumeFrame+20 帧后再次中断 =====
    {
        SampleLog slog;
        g_sampleLog = &slog;
        std::string lerr;
        if (!slog.loadFrom(out + ".samples", lerr)) {
            fprintf(stderr, "FAIL: 样本日志加载失败: %s\n", lerr.c_str());
            return 1;
        }
        std::string rmsg;
        int rdet = nraw::detectResume(opt, info, rmsg);
        if (rdet != 1) {
            fprintf(stderr, "FAIL: 续传检测 rdet=%d: %s\n", rdet, rmsg.c_str());
            return 1;
        }
        if (opt.resumeFrame <= 0 || opt.resumeFrame >= static_cast<long>(stopAfter)) {
            fprintf(stderr, "FAIL: resumeFrame=%ld 应在 (0, %zu)\n", opt.resumeFrame,
                    stopAfter);
            return 1;
        }
        // 与 main.cpp 一致：日志截断到 .part 实际完整前缀（掉电/硬中断时
        // .samples 可能领先 .part——截断点后的条目无对应数据）
        {
            const size_t complete = nraw::countCompleteSamples(
                out + ".part", slog);
            if (complete < slog.entries.size())
                slog.entries.resize(complete);
        }
        remove((out + ".part.old").c_str());
        if (rename((out + ".part").c_str(), (out + ".part.old").c_str()) != 0) {
            fprintf(stderr, "FAIL: 无法备份旧 .part\n");
            return 1;
        }
        {
            std::string perr;
            if (!nraw::purgePartFile(out + ".part.old", slog,
                                     static_cast<int64_t>(opt.resumeFrame), perr, false)) {
                fprintf(stderr, "FAIL: 清理旧 .part 失败: %s\n", perr.c_str());
                return 1;
            }
        }
        slog.purgeVideoFrom(static_cast<int64_t>(opt.resumeFrame));
        if (slog.videoCount() != static_cast<size_t>(opt.resumeFrame)) {
            fprintf(stderr, "FAIL: purge 后日志视频数 %zu != resumeFrame %ld\n",
                    slog.videoCount(), opt.resumeFrame);
            return 1;
        }
        sortLog(slog);
        // NRAW_SINGLE_RESUME=1：单次续传直达完成（不再次中断、跳过阶段3）——
        // 用于验证续传点接缝的引用封闭与 pts 连续性（open-GOP 默认配置的
        // 关键帧-17 续传点曾被怀疑产生跨边界引用/时间轴平移）
        const bool single = getenv("NRAW_SINGLE_RESUME") != nullptr;
        opt.testStopAfter = single ? -1 : 20;  // 续传 20 帧后中断（done 只计新帧）
        FrameQueue frames(16);
        AudioQueue audio(128);
        std::atomic<bool> abort(false);
        std::string encFail;
        std::thread prod(producer, std::ref(opt), std::ref(info),
                         std::ref(frames), std::ref(audio),
                         static_cast<size_t>(opt.resumeFrame),
                         opt.resumeAudioSample, FRAMES);
        int rc = encodeRun(opt, info, frames, audio, abort, encFail);
        prod.join();
        opt.testStopAfter = -1;
        if (rc != (single ? 0 : 4)) {
            fprintf(stderr, "FAIL: 首次续传 rc=%d（应为 %d）: %s\n", rc,
                    single ? 0 : 4, encFail.c_str());
            return 1;
        }
        if (access((out + ".samples").c_str(), F_OK) != 0 ||
            access((out + ".ckpt").c_str(), F_OK) != 0 ||
            (!single && access((out + ".part").c_str(), F_OK) != 0)) {
            fprintf(stderr, "FAIL: 首次续传%s缺少部分产物\n",
                    single ? "后" : "中断后");
            return 1;
        }
        // 注意：videoCount 是"重放 + 本次新增"的总数，不是本次复用量
        // （encodeRun 已单独打印复用量）；此处仅作进度展示
        fprintf(stderr, "阶段2: 首次续传%s（日志共 %zu 帧视频），.part/.samples/.ckpt 保留\n",
                single ? "直达完成" : "后中断", slog.videoCount());
        if (single) {
            // 单次续传完成：清理续传产物（与主程序成功路径一致），
            // 最终文件内容由脚本 check_resume_content 验证
            remove((out + ".samples").c_str());
            remove((out + ".ckpt").c_str());
            remove((out + ".part.old").c_str());
            remove((out + ".part").c_str());
            fprintf(stderr, "PASS: resume-test 单次续传完成（续传点回退关键帧 %ld） "
                            "resumeAudioSample=%llu\n",
                    opt.resumeFrame, opt.resumeAudioSample);
            return 0;
        }
    }

    // ===== 阶段3: 二次续传 → 成功完成 =====
    {
        SampleLog slog;
        g_sampleLog = &slog;
        std::string lerr;
        if (!slog.loadFrom(out + ".samples", lerr)) {
            fprintf(stderr, "FAIL: 阶段3 日志加载失败: %s\n", lerr.c_str());
            return 1;
        }
        std::string rmsg;
        int rdet = nraw::detectResume(opt, info, rmsg);
        if (rdet == 0) {
            // 硬中断（SIGINT，不 flush）下短会话无新关键帧落盘：detectResume
            // 返回 0（全新编码）是合法结果——内容不丢，只是复用失效。
            // 直接清理旧产物按全新编码继续。
            fprintf(stderr, "阶段3: %s（硬中断损失复用，全新编码）\n",
                    rmsg.c_str());
            remove((out + ".part.old").c_str());
            remove((out + ".part").c_str());
            remove((out + ".samples").c_str());
            remove((out + ".ckpt").c_str());
            opt.resumeMode = false;
            opt.resumeFrame = 0;
            opt.resumeAudioSample = 0;
            slog.entries.clear();
        } else if (rdet != 1) {
            fprintf(stderr, "FAIL: 阶段3 续传检测 rdet=%d: %s\n", rdet, rmsg.c_str());
            return 1;
        } else {
            // 与 main 一致：rename 前先截断日志到 .part 实际完整前缀
            // （掉电/硬中断时 .samples 可能领先 .part）
            const size_t complete = nraw::countCompleteSamples(
                out + ".part", slog);
            if (complete < slog.entries.size())
                slog.entries.resize(complete);
            remove((out + ".part.old").c_str());
            if (rename((out + ".part").c_str(), (out + ".part.old").c_str()) != 0) {
                fprintf(stderr, "FAIL: 阶段3 备份 .part 失败\n");
                return 1;
            }
        }
        if (rdet == 1) {
            std::string perr;
            if (!nraw::purgePartFile(out + ".part.old", slog,
                                     static_cast<int64_t>(opt.resumeFrame), perr, false)) {
                fprintf(stderr, "FAIL: 阶段3 清理 .part 失败: %s\n", perr.c_str());
                return 1;
            }
            slog.purgeVideoFrom(static_cast<int64_t>(opt.resumeFrame));
            sortLog(slog);
        }
        FrameQueue frames(16);
        AudioQueue audio(128);
        std::atomic<bool> abort(false);
        std::string encFail;
        std::thread prod(producer, std::ref(opt), std::ref(info),
                         std::ref(frames), std::ref(audio),
                         static_cast<size_t>(opt.resumeFrame),
                         opt.resumeAudioSample, FRAMES);
        int rc = encodeRun(opt, info, frames, audio, abort, encFail);
        prod.join();
        if (rc != 0) {
            fprintf(stderr, "FAIL: 阶段3 续传失败 rc=%d: %s\n", rc, encFail.c_str());
            return 1;
        }

        // 验证最终文件：完整帧数 + 音频流
        AVFormatContext* fc = nullptr;
        if (avformat_open_input(&fc, out.c_str(), nullptr, nullptr) < 0 ||
            avformat_find_stream_info(fc, nullptr) < 0) {
            fprintf(stderr, "FAIL: 续传输出无法读取\n");
            return 1;
        }
        long long vf = -1, af = -1;
        for (unsigned i = 0; i < fc->nb_streams; ++i) {
            AVStream* st = fc->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                vf = st->nb_frames;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                af = st->nb_frames;
        }
        avformat_close_input(&fc);
        if (vf != FRAMES) {
            fprintf(stderr, "FAIL: 续传输出帧数 %lld != %d\n", vf,
                    static_cast<int>(FRAMES));
            return 1;
        }
        if (af < 0) {
            fprintf(stderr, "FAIL: 续传输出缺少音频流\n");
            return 1;
        }
        // 模拟主程序成功清理（NRAW_KEEP_RESUME=1 时保留供调试）
        if (getenv("NRAW_KEEP_RESUME") == nullptr) {
            remove((out + ".samples").c_str());
            remove((out + ".ckpt").c_str());
            remove((out + ".part").c_str());
            remove((out + ".part.old").c_str());
        }
        fprintf(stderr,
                "PASS: resume-test 三次中断/续传后完成（%lld 帧 + 音频，"
                "续传点回退关键帧 %ld） resumeAudioSample=%llu\n",
                vf, opt.resumeFrame, opt.resumeAudioSample);
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "用法: %s repack | sha256 [file] | dump <ref.yuv> | encode <out.mov> | "
                "repair-test <out.mov> | resume-test <out.mov>\n",
                argv[0]);
        return 2;
    }

    CliOptions opt;
    MediaInfo info;
    info.width = W;
    info.height = H;
    info.frameCount = FRAMES;
    info.fpsNum = FPS_NUM;
    info.fpsDen = FPS_DEN;
    info.audio.present = true;
    info.audio.sampleRate = SR;
    info.audio.bits = 24;
    info.audio.channels = CH;
    info.audio.samplesPerChannel = 96000;
    info.audio.blockCount = 20;

    std::string mode = argv[1];
    if (mode == "repack") {
        return testRepack() ? 0 : 1;
    }
    if (mode == "sha256") {
        if (argc >= 3) {
            std::string h = sha256File(argv[2]);
            if (h.empty()) {
                fprintf(stderr, "sha256File 失败: %s\n", argv[2]);
                return 1;
            }
            printf("%s\n", h.c_str());
            return 0;
        }
        return testSha256() ? 0 : 1;
    }
    if (argc < 3) {
        fprintf(stderr,
                "用法: %s repack | sha256 [file] | dump <ref.yuv> | encode <out.mov> | "
                "repair-test <out.mov> | resume-test <out.mov>\n",
                argv[0]);
        return 2;
    }
    if (mode == "repair-test")
        return testRepair(argv[2]);
    if (mode == "resume-test")
        return testResume(argv[2]);
    if (mode == "dump") {
        opt.dumpRef = argv[2];
    } else if (mode == "encode") {
        opt.output = argv[2];
    } else {
        fprintf(stderr, "未知模式: %s\n", mode.c_str());
        return 2;
    }

    // 与主程序一致：编码会话记录样本日志，供收尾失败时自动重建
    SampleLog slog;
    g_sampleLog = &slog;

    FrameQueue frames(16);
    AudioQueue audio(128);
    std::atomic<bool> abort(false);
    std::string failMsg, encFail;

    std::thread prod(producer, std::ref(opt), std::ref(info),
                     std::ref(frames), std::ref(audio), 0, 0ULL, FRAMES);
    int rc = encodeRun(opt, info, frames, audio, abort, encFail);
    prod.join();

    if (rc != 0) {
        fprintf(stderr, "encodeRun 失败: %s (rc=%d)\n",
                encFail.empty() ? "?" : encFail.c_str(), rc);
        return 1;
    }
    fprintf(stderr, "%s 完成 (rc=0)\n", mode.c_str());
    return 0;
}
