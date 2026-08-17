#ifndef NRAW_ARCHIVE_H
#define NRAW_ARCHIVE_H

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <utility>

namespace nraw {

constexpr const char* kToolVersion = "1.0.0";

// ---------- 进度条 ----------

inline double wallNow()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 秒数 → "H:MM:SS"（≥1 小时）或 "MM:SS"
inline std::string fmtDuration(double sec)
{
    if (sec < 0)
        sec = 0;
    long long s = static_cast<long long>(sec + 0.5);
    long long h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    char b[32];
    if (h > 0)
        snprintf(b, sizeof(b), "%lld:%02lld:%02lld", h, m, ss);
    else
        snprintf(b, sizeof(b), "%02lld:%02lld", m, ss);
    return b;
}

// epoch 秒 → 本地时间 "HH:MM:SS"（预计结束时间）
inline std::string wallClock(double epochSec)
{
    time_t t = static_cast<time_t>(epochSec);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

// 渲染进度行：进度条 + 百分比 + 已处理/总时长 + 剩余时长 + 预计结束时间（墙钟）+ 实际 fps
// done/total 为帧数；fpsNum/fpsDen 为媒体帧率（用于标称时长）；fps 为实际处理速度
inline std::string progressLine(size_t done, size_t total,
                                size_t fpsNum, size_t fpsDen, double fps)
{
    const double frac = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
    const double f = frac > 1.0 ? 1.0 : frac;
    const int kBar = 20;
    int filled = static_cast<int>(f * kBar + 0.5);
    std::string bar(kBar, '-');
    for (int i = 0; i < filled && i < kBar; ++i)
        bar[i] = '#';
    const double num = fpsNum > 0 ? static_cast<double>(fpsNum) : 1.0;
    const double den = fpsDen > 0 ? static_cast<double>(fpsDen) : 1.0;
    const double doneSec = static_cast<double>(done) * den / num;
    const double totalSec = static_cast<double>(total) * den / num;
    const double remainSec = totalSec - doneSec;
    double eta = wallNow();
    double remainProcSec = 0.0;
    if (fps > 0.01) {
        // ETA = 当前墙钟 + 剩余帧数 / 处理速度（帧/秒）
        // 注意不能写成 remainSec / fps：remainSec 是剩余"媒体时长"秒，
        // 除以帧/秒单位错误，会低估约一个帧率的倍数
        const double remainFrames =
            static_cast<double>(total) - static_cast<double>(done);
        remainProcSec = remainFrames / fps;
        eta += remainProcSec;
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
             "[%s] %5.1f%%  %s / %s  剩余媒体 %s  处理剩余 %s  预计结束 %s  %.1f fps",
             bar.c_str(), f * 100.0,
             fmtDuration(doneSec).c_str(), fmtDuration(totalSec).c_str(),
             fmtDuration(remainSec).c_str(), fmtDuration(remainProcSec).c_str(),
             wallClock(eta).c_str(), fps);
    std::string s = buf;
    if (s.size() < 80)
        s.append(80 - s.size(), ' '); // 覆盖上一行更长内容的残留
    return s;
}

struct CliOptions {
    std::string input;
    std::string output;
    std::string sdkPath;             // dir containing RED*.so, default = executable dir
    std::string dumpRef;             // lossless yuv420p10le reference dump (testing only)
    long maxFrames = -1;             // -1 = all
    bool noAudio = false;
    bool faststart = false;
    bool noSidecar = false;

    int   kelvin = 0;                // 0 = as-shot
    float tint = NAN;                // NAN = as-shot
    long  iso = 0;                   // 0 = as-shot
    float exposure = NAN;            // NAN = as-shot (ExposureAdjust, stops)
    int   lensCorrection = 1;        // 0=auto 1=on(默认) 2=off
    int   chromaNr = -1;             // -1 = as-shot, 0=off, 1=on
    int   decodeMode = 0;            // 0=auto 1=gpu 2=cpu

    // 多进程 CPU 解码内部参数（--decode-worker 等，隐藏，仅由父进程 exec 子进程时使用）
    bool  decodeWorker = false;      // 当前进程是解码 worker
    long  workerId = 0;              // worker 序号（解码 帧号 ≡ id (mod count)）
    long  workerCount = 1;           // worker 总数
    long  workerFrames = 0;          // 需解码的帧总数（父进程 clamp 后的值）

    int   crf = 14;
    std::string preset = "slow";
    int   keyint = 0;                // 0 = auto (round(fps*2))
    int   minKeyint = 1;
    int   openGop = 1;               // 1 = open GOP (scenecut), 0 = closed GOP
    int   pools = 0;                 // x265 编码线程池 (0 = auto，由 --jobs 统一分配)
    int   cpuWorkers = 0;            // CPU 解码 worker 进程数 (0 = auto，由 --jobs 分配)
    int   jobs = 0;                  // CPU 总线程预算 (0 = auto = 核心数)；拆分 worker 数与 x265 pools
    int   buffers = 16;
    bool  gpuTest = false;           // --gpu-test: GPU 初始化+内核编译+A/B 门控测试后退出
};

struct AudioInfo {
    bool present = false;
    int  sampleRate = 48000;
    int  bits = 24;
    int  channels = 0;
    size_t blockCount = 0;
    size_t maxBlockBytes = 0;
    size_t samplesPerChannel = 0;
};

struct MediaInfo {
    size_t width = 0;
    size_t height = 0;
    size_t frameCount = 0;
    size_t fpsNum = 0;               // exact record framerate rational, e.g. 60000/1001
    size_t fpsDen = 1;
    AudioInfo audio;
    std::vector<std::pair<std::string, std::string>> meta; // for sidecar
};

struct AppliedSettings {
    float kelvin = 0.0f;
    float tint = 0.0f;
    size_t iso = 0;
    float exposure = 0.0f;
    int lensCorrection = 0;          // 0/1/2
};

struct GpuStatus {
    bool used = false;               // run used the GPU decode path
    bool gated = false;              // A/B gate was performed
    double gatePsnr = 0.0;           // worst-case gate PSNR in dB (0 = not gated)
    std::string device;              // OpenCL device name
    std::string note;                // decision note for the sidecar
};

class AlignedBuffer {
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(size_t size, size_t align = 16) { resize(size, align); }
    ~AlignedBuffer() { release(); }
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& o) noexcept { *this = std::move(o); }
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) { release(); ptr_ = o.ptr_; size_ = o.size_; o.ptr_ = nullptr; o.size_ = 0; }
        return *this;
    }
    void resize(size_t size, size_t align = 16) {
        release();
        if (size == 0) return;
        void* p = nullptr;
        if (posix_memalign(&p, align, size) != 0) return;
        ptr_ = p;
        size_ = size;
    }
    void* data() const { return ptr_; }
    size_t size() const { return size_; }

private:
    void release() {
        if (ptr_) { free(ptr_); ptr_ = nullptr; size_ = 0; }
    }
    void* ptr_ = nullptr;
    size_t size_ = 0;
};

struct VideoFrame {
    size_t width = 0;
    size_t height = 0;
    size_t frameNo = 0;
    AlignedBuffer rgb;               // 3 planar 16-bit LE channels, w*h*6 bytes, 16-aligned
};

struct AudioPacket {
    std::vector<uint8_t> bytes;      // s24le, channels interleaved
    size_t firstSample = 0;          // per-channel sample index
};

class FrameQueue {
public:
    // 有界有序帧队列，供 GPU worker 单生产者 + 编码线程单消费者使用。
    // 契约: 生产者必须严格按帧号递增 push（每次 +1）；帧号 <= 上一次入队帧号
    // 的乱序/重复帧会被静默丢弃（push 立即返回），调用方不得依赖丢帧语义。
    // 队满时 push 阻塞等待，队空时 pop 阻塞等待；setEof() 后 push 直接丢弃、
    // pop 排空后返回 nullptr。
    explicit FrameQueue(size_t capacity) : cap_(capacity ? capacity : 1) {}
    void push(std::unique_ptr<VideoFrame> f) {
        std::unique_lock<std::mutex> lk(m_);
        const size_t no = f->frameNo;
        notFull_.wait(lk, [&] {
            return eof_ || no < next_ || (no >= next_ && q_.size() < cap_);
        });
        if (eof_ || no < next_)
            return;
        q_.push_back(std::move(f));
        next_ = no + 1;
        notEmpty_.notify_one();
        notFull_.notify_all();
    }
    std::unique_ptr<VideoFrame> pop() {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return !q_.empty() || eof_; });
        if (q_.empty()) return nullptr;
        auto f = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_all();
        return f;
    }
    void setEof() {
        std::lock_guard<std::mutex> lk(m_);
        eof_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    size_t cap_;
    std::deque<std::unique_ptr<VideoFrame>> q_;
    std::mutex m_;
    std::condition_variable notFull_, notEmpty_;
    bool eof_ = false;
    size_t next_ = 0;
};

class AudioQueue {
public:
    explicit AudioQueue(size_t capacity) : cap_(capacity ? capacity : 1) {}
    void push(AudioPacket p) {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [&] { return q_.size() < cap_ || eof_; });
        if (eof_) return;
        q_.push_back(std::move(p));
        notEmpty_.notify_one();
    }
    bool pop(AudioPacket& out) {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [&] { return !q_.empty() || eof_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    bool tryPop(AudioPacket& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void setEof() {
        std::lock_guard<std::mutex> lk(m_);
        eof_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

private:
    size_t cap_;
    std::deque<AudioPacket> q_;
    std::mutex m_;
    std::condition_variable notFull_, notEmpty_;
    bool eof_ = false;
};

// sdk_decode.cpp
bool   initSdk(const std::string& redistDir, std::string& err, bool withOpenCL = false);
void   shutdownSdk();
std::string sdkVersion();
bool   openMedia(const std::string& path, const CliOptions& opt, MediaInfo& info, std::string& err);
bool   appliedSettings(AppliedSettings& out);
void   closeMedia();
void*  sharedIpSettings();          // shared IPP2 ImageProcessingSettings* (stable during run)

class SequentialDecoder {
public:
    SequentialDecoder() = default;
    ~SequentialDecoder() { close(); }
    SequentialDecoder(const SequentialDecoder&) = delete;
    SequentialDecoder& operator=(const SequentialDecoder&) = delete;

    bool open(const CliOptions& opt, const MediaInfo& info, std::string& err);
    bool decodeFrame(size_t frameNo, VideoFrame& out, std::string& err);
    bool decodeAudioWindow(unsigned long long targetSamples,
                           AudioQueue& audio, std::string& err);
    bool drainAudio(AudioQueue& audio, unsigned long long targetSamplesLimit,
                    std::string& err);
    bool audioOn() const { return audioOn_; }
    void close();

private:
    void* clip_ = nullptr;
    bool audioOn_ = false;
    size_t audioBlockIdx_ = 0;
    unsigned long long decodedSamples_ = 0;
    AlignedBuffer audioBlockBuf_;        // reusable decode block buffer
    std::vector<uint8_t> audioRepack_;   // reusable s24le repack buffer
};

// 多线程 CPU 解码器（多进程实现）：SDK 经典同步解码（Clip::DecodeVideoFrame）实测
// ~1fps 且为进程内全局串行（并行 Clip/线程无效，4 进程并发实测 ~3.6fps 近线性扩展），
// 因此 --decode cpu 通过 fork 出 N 个 worker 子进程（各自 ~1fps，帧号 ≡ k (mod N)），
// 经管道流式回传 RGB 帧，父进程按帧号排序后交给 x265 编码；解码与编码流水线重叠。
// 仅用于显式 --decode cpu（纯 CPU，不依赖 GPU/OpenCL）。音频在父进程内解码。
// 用法: waitFrame() 按帧号递增取回解码帧（子进程自主解码，父进程背压由管道提供）。
class CpuAsyncDecoder {
public:
    CpuAsyncDecoder() = default;
    ~CpuAsyncDecoder() { close(); }
    CpuAsyncDecoder(const CpuAsyncDecoder&) = delete;
    CpuAsyncDecoder& operator=(const CpuAsyncDecoder&) = delete;

    // frames = 需解码的帧总数（父进程按 --frames 裁剪后的值；worker 数与帧号分配基于它）
    bool open(const CliOptions& opt, const MediaInfo& info, size_t frames,
              std::string& err);
    // 等待 frameNo 解码完成（按帧号递增调用），拷贝到 out；失败置 err。
    bool waitFrame(size_t frameNo, VideoFrame& out, std::string& err);
    bool decodeAudioWindow(unsigned long long targetSamples,
                           AudioQueue& audio, std::string& err);
    bool drainAudio(AudioQueue& audio, unsigned long long targetSamplesLimit,
                    std::string& err);
    void close();

private:
    void* impl_ = nullptr;
};

// 解码 worker 子进程入口（--decode-worker）：经典同步解码 帧号 ≡ id (mod count)，
// 帧数据 [u64 frameNo][u32 size][payload] 写入 fd 3；返回 0=成功。
int runDecodeWorker(const CliOptions& opt);

// gpu_process.cpp
class GpuPipeline {
public:
    GpuPipeline();
    ~GpuPipeline();
    GpuPipeline(const GpuPipeline&) = delete;
    GpuPipeline& operator=(const GpuPipeline&) = delete;

    // OpenCL load + REDCL init + compatibility check (long on first run)
    bool init(const CliOptions& opt, const MediaInfo& info, std::string& err);
    // 无素材 GPU 初始化测试：OpenCL 加载/枚举 + REDCL 构造 + 内核编译，
    // 不依赖输入文件（供 --gpu-test 无输入模式使用）
    bool initGpuOnly(std::string& err);
    // synchronous single-frame decode, used by the A/B gate (before start())
    bool decodeSync(size_t frameNo, VideoFrame& out, std::string& err);
    // start AsyncDecoder + worker thread; frames is the ordered encoder queue;
    // abort is the shared encoder-abort flag the worker must raise on any failure
    bool start(FrameQueue& frames, std::atomic<bool>* abort, std::string& err);
    // submit frame for async decompress; blocks while the raw pool is full
    bool submit(size_t frameNo, std::string& err);
    // wait for all submitted frames, join worker, EOF the frame queue
    bool finish(std::string& err);
    void close();
    const std::string& deviceName() const;
    // 管线最后一次失败的具体原因（未失败时为空串）；供 runGpuPath 失败分支
    // 在 finish() 未被执行的情况下仍能取回真实错误信息
    std::string lastError() const;

private:
    void* impl_ = nullptr;
};

// audio.cpp
size_t repack24beToS24le(const void* src, size_t wordCount, void* dst);

// av_encode.cpp
class EncodeSession {
public:
    EncodeSession() = default;
    ~EncodeSession() { cleanup(); }
    EncodeSession(const EncodeSession&) = delete;
    EncodeSession& operator=(const EncodeSession&) = delete;

    bool open(const CliOptions& opt, const MediaInfo& info, std::string& err);
    bool write(const VideoFrame& f, std::string& err);
    bool writeAudio(const AudioPacket& p, std::string& err);
    bool finish(std::string& err);
    void cleanup();

private:
    void* impl_ = nullptr;
};

int encodeRun(const CliOptions& opt, const MediaInfo& info,
              FrameQueue& frames, AudioQueue& audio,
              std::atomic<bool>& abort, std::string& failDetail);

// main.cpp
int writeSidecar(const std::string& outPath, const CliOptions& opt,
                 const MediaInfo& info, size_t framesDone,
                 const AppliedSettings& applied, const GpuStatus& gpu,
                 const std::string& inputHash, bool ok);

} // namespace nraw

#endif
