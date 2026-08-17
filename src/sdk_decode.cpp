#include "archive.h"

#include <cmath>
#include <csignal>
#include <cstring>
#include <map>
#include <new>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "R3DSDK.h"

namespace nraw {

namespace {

R3DSDK::Clip* gClip = nullptr;
R3DSDK::ImageProcessingSettings* gIp = nullptr;
std::string gClipPath;
MediaInfo gInfo;
bool gOpen = false;

void addMeta(R3DSDK::Clip* clip,
             std::vector<std::pair<std::string, std::string>>& meta,
             const char* key)
{
    if (!clip->MetadataExists(key))
        return;
    if (clip->MetadataItemType(key) == R3DSDK::MetadataTypeInt)
        meta.emplace_back(key, std::to_string(clip->MetadataItemAsInt(key)));
    else
        meta.emplace_back(key, clip->MetadataItemAsString(key));
}

}

const char* initStatusText(R3DSDK::InitializeStatus st)
{
    switch (st) {
    case R3DSDK::ISInitializeOK:
        return "OK";
    case R3DSDK::ISLibraryNotLoaded:
        return "SDK 库未加载";
    case R3DSDK::ISR3DSDKLibraryNotFound:
        return "找不到 REDR3D-x64.so";
    case R3DSDK::ISRedCudaLibraryNotFound:
        return "找不到 REDCuda-x64.so";
    case R3DSDK::ISRedOpenCLLibraryNotFound:
        return "找不到 REDOpenCL-x64.so";
    case R3DSDK::ISR3DDecoderLibraryNotFound:
        return "找不到 REDDecoder-x64.so";
    case R3DSDK::ISLibraryVersionMismatch:
        return "库版本不匹配";
    case R3DSDK::ISInvalidR3DSDKLibrary:
        return "REDR3D-x64.so 无效";
    case R3DSDK::ISInvalidRedCudaLibrary:
        return "REDCuda-x64.so 无效";
    case R3DSDK::ISInvalidRedOpenCLLibrary:
        return "REDOpenCL-x64.so 无效";
    case R3DSDK::ISInvalidR3DDecoderLibrary:
        return "REDDecoder-x64.so 无效";
    case R3DSDK::ISRedCudaLibraryInitializeFailed:
        return "REDCuda 初始化失败";
    case R3DSDK::ISRedOpenCLLibraryInitializeFailed:
        return "REDOpenCL 初始化失败";
    case R3DSDK::ISR3DDecoderLibraryInitializeFailed:
        return "REDDecoder 初始化失败";
    case R3DSDK::ISR3DSDKLibraryInitializeFailed:
        return "REDR3D 初始化失败";
    case R3DSDK::ISInvalidPath:
        return "SDK 路径无效";
    case R3DSDK::ISInternalError:
        return "SDK 内部错误";
    default:
        return "未知错误";
    }
}

bool initSdk(const std::string& redistDir, std::string& err, bool withOpenCL)
{
    int options = OPTION_RED_NONE;
    if (withOpenCL)
        options |= OPTION_RED_OPENCL;
    R3DSDK::InitializeStatus st = R3DSDK::InitializeSdk(redistDir.c_str(), options);
    if (st != R3DSDK::ISInitializeOK) {
        err = "R3D SDK 初始化失败: " + std::string(initStatusText(st)) +
              " (status " + std::to_string(static_cast<int>(st)) + ")";
        if (st == R3DSDK::ISLibraryVersionMismatch) {
            err += "。运行时 RED*.so 必须与构建二进制时使用的 SDK 版本一致"
                   "（静态库与运行时库来自同一个 SDK 目录）；RED SDK 不支持混用版本。"
                   "如需使用其他 SDK 版本，请以 -DR3D_SDK_ROOT 指向该版本目录重新构建。";
        }
        return false;
    }
    return true;
}

void shutdownSdk()
{
    R3DSDK::FinalizeSdk();
}

std::string sdkVersion()
{
    const char* v = R3DSDK::GetSdkVersion();
    return v ? std::string(v) : std::string();
}

bool openMedia(const std::string& path, const CliOptions& opt, MediaInfo& info, std::string& err)
{
    closeMedia();

    R3DSDK::Clip* clip = nullptr;
    try {
        clip = new R3DSDK::Clip(path.c_str());
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    if (clip->Status() != R3DSDK::LSClipLoaded) {
        err = "failed to open clip " + path + ", status " +
              std::to_string(static_cast<int>(clip->Status()));
        delete clip;
        return false;
    }

    MediaInfo mi;
    mi.width = clip->Width();
    mi.height = clip->Height();
    mi.frameCount = clip->VideoFrameCount();

    if (clip->MetadataExists(R3DSDK::RMD_RECORD_FRAMERATE_NUMERATOR)) {
        mi.fpsNum = clip->MetadataItemAsInt(R3DSDK::RMD_RECORD_FRAMERATE_NUMERATOR);
        mi.fpsDen = clip->MetadataExists(R3DSDK::RMD_RECORD_FRAMERATE_DENOMINATOR)
                        ? clip->MetadataItemAsInt(R3DSDK::RMD_RECORD_FRAMERATE_DENOMINATOR)
                        : 1;
    } else {
        mi.fpsNum = clip->MetadataItemAsInt(R3DSDK::RMD_FRAMERATE_NUMERATOR);
        mi.fpsDen = clip->MetadataExists(R3DSDK::RMD_FRAMERATE_DENOMINATOR)
                        ? clip->MetadataItemAsInt(R3DSDK::RMD_FRAMERATE_DENOMINATOR)
                        : 1;
    }
    if (mi.fpsNum == 0)
        mi.fpsNum = 1;
    if (mi.fpsDen == 0)
        mi.fpsDen = 1;
    if (mi.fpsDen > 1000000 || mi.fpsNum > 2000000) {
        err = "异常的帧率元数据: " + std::to_string(mi.fpsNum) + "/" +
              std::to_string(mi.fpsDen);
        delete clip;
        return false;
    }

    size_t maxBytes = 0;
    mi.audio.blockCount = clip->AudioBlockCountAndSize(&maxBytes);
    mi.audio.maxBlockBytes = maxBytes;
    mi.audio.channels = static_cast<int>(clip->AudioChannelCount());
    mi.audio.present = mi.audio.channels > 0 && mi.audio.blockCount > 0;
    if (mi.audio.present) {
        if (clip->MetadataExists(R3DSDK::RMD_SAMPLERATE))
            mi.audio.sampleRate = clip->MetadataItemAsInt(R3DSDK::RMD_SAMPLERATE);
        if (mi.audio.sampleRate <= 0 || mi.audio.sampleRate > 384000) {
            mi.audio.sampleRate = 48000;
        }
        if (clip->MetadataExists(R3DSDK::RMD_SAMPLE_SIZE))
            mi.audio.bits = clip->MetadataItemAsInt(R3DSDK::RMD_SAMPLE_SIZE);
        if (mi.audio.bits != 24) {
            err = "不支持的音频位深 " + std::to_string(mi.audio.bits) +
                  " (仅支持 24-bit LPCM)";
            delete clip;
            return false;
        }
        mi.audio.samplesPerChannel = static_cast<size_t>(clip->AudioSampleCount());
    }

    static const char* metaKeys[] = {
        R3DSDK::RMD_SENSOR_NAME,
        R3DSDK::RMD_CAMERA_MODEL,
        R3DSDK::RMD_LENS_NAME,
        R3DSDK::RMD_REDCODE,
        R3DSDK::RMD_LENS_DISTORTION_CORRECTION,
        R3DSDK::RMD_ISO,
        R3DSDK::RMD_WHITE_BALANCE_KELVIN,
        R3DSDK::RMD_WHITE_BALANCE_TINT,
        R3DSDK::RMD_LOCAL_DATE,
        R3DSDK::RMD_LOCAL_TIME,
        R3DSDK::RMD_START_ABSOLUTE_TIMECODE,
        R3DSDK::RMD_CLIP_UUID
    };
    for (const char* key : metaKeys)
        addMeta(clip, mi.meta, key);

    R3DSDK::ImageProcessingSettings* ip = nullptr;
    try {
        ip = new R3DSDK::ImageProcessingSettings();
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        delete clip;
        return false;
    }
    clip->GetDefaultImageProcessingSettings(*ip);
    if (opt.kelvin != 0)
        ip->Kelvin = static_cast<float>(opt.kelvin);
    if (!std::isnan(opt.tint))
        ip->Tint = opt.tint;
    if (opt.iso != 0)
        ip->ISO = static_cast<size_t>(opt.iso);
    if (!std::isnan(opt.exposure))
        ip->ExposureAdjust = opt.exposure;
    if (opt.lensCorrection == 1)
        ip->LensDistortionCorrection = R3DSDK::LensCorrectionOn;
    else if (opt.lensCorrection == 2)
        ip->LensDistortionCorrection = R3DSDK::LensCorrectionOff;
    if (opt.chromaNr == 0)
        ip->ChromaNoiseReduction = false;
    else if (opt.chromaNr == 1)
        ip->ChromaNoiseReduction = true;
    ip->ImagePipelineMode = R3DSDK::Full_Graded;
    ip->ColorSpace = R3DSDK::ImageColorREDWideGamutRGB;
    ip->GammaCurve = R3DSDK::ImageGammaLog3G10;
    ip->CheckBounds();

    gClip = clip;
    gClipPath = path;
    gIp = ip;
    gInfo = mi;
    gOpen = true;

    info = mi;
    return true;
}

bool appliedSettings(AppliedSettings& out)
{
    if (!gOpen || !gIp)
        return false;
    out.kelvin = gIp->Kelvin;
    out.tint = gIp->Tint;
    out.iso = gIp->ISO;
    out.exposure = gIp->ExposureAdjust;
    out.lensCorrection = static_cast<int>(gIp->LensDistortionCorrection);
    return true;
}

void closeMedia()
{
    if (gClip) {
        delete gClip;
        gClip = nullptr;
    }
    if (gIp) {
        delete gIp;
        gIp = nullptr;
    }
    gOpen = false;
}

bool SequentialDecoder::open(const CliOptions& opt, const MediaInfo& info, std::string& err)
{
    close();
    audioOn_ = !opt.noAudio && opt.dumpRef.empty() && info.audio.present;

    R3DSDK::Clip* clip = nullptr;
    try {
        clip = new R3DSDK::Clip(gClipPath.c_str());
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM)";
        return false;
    }
    if (clip->Status() != R3DSDK::LSClipLoaded) {
        err = "failed to open decode clip";
        delete clip;
        return false;
    }
    clip_ = clip;
    audioBlockIdx_ = 0;
    decodedSamples_ = 0;
    return true;
}

bool SequentialDecoder::decodeFrame(size_t frameNo, VideoFrame& out, std::string& err)
{
    R3DSDK::Clip* clip = static_cast<R3DSDK::Clip*>(clip_);
    out.width = gInfo.width;
    out.height = gInfo.height;
    out.frameNo = frameNo;
    const size_t need = gInfo.width * gInfo.height * 6;
    if (out.rgb.size() != need) {
        try {
            out.rgb.resize(need, 16);
        } catch (const std::bad_alloc&) {
            err = "内存不足 (OOM) at frame " + std::to_string(frameNo);
            return false;
        }
        if (!out.rgb.data() || !out.rgb.size()) {
            err = "内存分配失败 at frame " + std::to_string(frameNo);
            return false;
        }
    }

    R3DSDK::VideoDecodeJob job;
    job.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
    job.PixelType = R3DSDK::PixelType_16Bit_RGB_Planar;
    job.OutputBuffer = out.rgb.data();
    job.OutputBufferSize = out.rgb.size();
    job.ImageProcessing = gIp;

    if (clip->DecodeVideoFrame(frameNo, job) != R3DSDK::DSDecodeOK) {
        err = "video decode failed at frame " + std::to_string(frameNo);
        return false;
    }
    return true;
}

bool SequentialDecoder::decodeAudioWindow(unsigned long long targetSamples,
                                          AudioQueue& audio, std::string& err)
{
    if (!audioOn_)
        return true;
    R3DSDK::Clip* clip = static_cast<R3DSDK::Clip*>(clip_);
    const size_t needBlock = gInfo.audio.maxBlockBytes;
    if (needBlock > audioBlockBuf_.size())
        audioBlockBuf_.resize(needBlock, 512);
    const size_t needRepack = (needBlock / 4) * 3;
    if (needRepack > audioRepack_.size()) {
        try {
            audioRepack_.resize(needRepack);
        } catch (const std::bad_alloc&) {
            err = "内存不足 (OOM)";
            return false;
        }
    }
    if (needBlock > 0 && !audioBlockBuf_.data()) {
        err = "内存不足 (OOM)";
        return false;
    }
    while (audioBlockIdx_ < gInfo.audio.blockCount &&
           decodedSamples_ < targetSamples) {
        size_t bufSize = gInfo.audio.maxBlockBytes;
        if (clip->DecodeAudioBlock(audioBlockIdx_, audioBlockBuf_.data(), &bufSize) !=
            R3DSDK::DSDecodeOK) {
            err = "audio decode failed at block " + std::to_string(audioBlockIdx_);
            return false;
        }
        if (bufSize > gInfo.audio.maxBlockBytes || bufSize % 4 != 0) {
            err = "audio decode returned invalid size at block " +
                  std::to_string(audioBlockIdx_);
            return false;
        }
        size_t words = bufSize / 4;
        ++audioBlockIdx_;
        if (words == 0)
            continue;
        if (gInfo.audio.channels <= 0 ||
            words % static_cast<size_t>(gInfo.audio.channels) != 0) {
            err = "audio block size not aligned to channels at block " +
                  std::to_string(audioBlockIdx_ - 1);
            return false;
        }
        size_t outBytes =
            repack24beToS24le(audioBlockBuf_.data(), words, audioRepack_.data());

        AudioPacket p;
        try {
            p.bytes.assign(audioRepack_.data(), audioRepack_.data() + outBytes);
        } catch (const std::bad_alloc&) {
            err = "内存不足 (OOM)";
            return false;
        }
        p.firstSample = decodedSamples_;
        try {
            audio.push(std::move(p));
        } catch (const std::bad_alloc&) {
            err = "内存不足 (OOM)";
            return false;
        }

        size_t blockSamples = words / static_cast<size_t>(gInfo.audio.channels);
        decodedSamples_ += blockSamples;
    }
    return true;
}

bool SequentialDecoder::drainAudio(AudioQueue& audio,
                                   unsigned long long targetSamplesLimit,
                                   std::string& err)
{
    if (!audioOn_)
        return true;
    while (audioBlockIdx_ < gInfo.audio.blockCount &&
           decodedSamples_ < targetSamplesLimit) {
        if (!decodeAudioWindow(targetSamplesLimit, audio, err))
            return false;
    }
    return true;
}

void* sharedIpSettings()
{
    return gIp;
}

void SequentialDecoder::close()
{
    if (clip_) {
        delete static_cast<R3DSDK::Clip*>(clip_);
        clip_ = nullptr;
    }
}



// ---------------------------------------------------------------------------
// CpuAsyncDecoder —— 多进程 CPU 解码
//
// 实测经典同步 API（Clip::DecodeVideoFrame，DECODE_FULL_RES_PREMIUM）在 4K 下仅
// ~1fps，且是 SDK 进程内全局串行：单进程内并行 Clip/线程无效，但跨进程近线性扩展
// （实测 2 进程 2.0fps、4 进程 3.6fps）。因此 --decode cpu 由父进程 spawn 出 N 个
// worker 子进程（各自用经典 API 解码 帧号 ≡ k (mod N)，每个 ~1fps），通过管道把
// 解码帧 [u64 frameNo][u32 size][payload RGB planar 16-bit] 流式回传；父进程按帧号
// 排序后交给 x265 编码，解码与编码流水线重叠。纯 CPU，不依赖 GPU/OpenCL。
// 子进程用 posix_spawn（不复制父进程内存，避免继承 SDK 内部线程锁）。
// ---------------------------------------------------------------------------

class CpuAsyncDecoderImpl {
public:
    CpuAsyncDecoderImpl() = default;
    ~CpuAsyncDecoderImpl() { close(); }
    CpuAsyncDecoderImpl(const CpuAsyncDecoderImpl&) = delete;
    CpuAsyncDecoderImpl& operator=(const CpuAsyncDecoderImpl&) = delete;

    bool open(const CliOptions& opt, const MediaInfo& info, size_t frames,
              std::string& err);
    bool waitFrame(size_t frameNo, VideoFrame& out, std::string& err);
    bool decodeAudioWindow(unsigned long long targetSamples,
                           AudioQueue& audio, std::string& err)
    {
        return audio_.decodeAudioWindow(targetSamples, audio, err);
    }
    bool drainAudio(AudioQueue& audio, unsigned long long targetSamplesLimit,
                    std::string& err)
    {
        return audio_.drainAudio(audio, targetSamplesLimit, err);
    }
    void close();

private:
    bool pumpOnce(std::string& err);
    bool readWorkerFrame(size_t idx, std::string& err);
    void markDead(size_t idx);

    struct Worker {
        int fd = -1;          // 管道读端（父进程）
        pid_t pid = -1;
        bool dead = false;
    };

    std::mutex m_;
    std::condition_variable cv_;
    std::vector<Worker> workers_;
    std::map<size_t, VideoFrame> ready_;   // 已到达但尚未按序消费的帧
    size_t nWorkers_ = 8;
    size_t width_ = 0, height_ = 0;
    size_t frameSize_ = 0;
    bool fail_ = false;
    std::string failMsg_;
    SequentialDecoder audio_;              // 音频：父进程内经典 Clip::DecodeAudioBlock
};

namespace {

// 循环读满 n 字节；返回实际读到的字节数（0=EOF，-1=错误）
ssize_t readFull(int fd, void* buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, static_cast<char*>(buf) + got, n - got);
        if (r > 0) {
            got += static_cast<size_t>(r);
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        return -1;
    }
    return static_cast<ssize_t>(got);
}

// 循环写满 n 字节；0=全部写完，-1=错误（EPIPE 父进程已退出）
ssize_t writeFull(int fd, const void* buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, static_cast<const char*>(buf) + put, n - put);
        if (w > 0) {
            put += static_cast<size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

// worker 子进程命令行（父进程构造，posix_spawn /proc/self/exe）
std::vector<std::string> workerArgs(const CliOptions& opt, size_t id,
                                    size_t count, size_t total)
{
    std::vector<std::string> a;
    a.emplace_back("nraw_archive");
    a.push_back("--decode-worker");
    a.push_back(std::to_string(id));
    a.push_back("--worker-count");
    a.push_back(std::to_string(count));
    a.push_back("--worker-frames");
    a.push_back(std::to_string(total));
    a.push_back("--decode");
    a.push_back("cpu");
    if (opt.kelvin != 0) {
        a.push_back("--kelvin");
        a.push_back(std::to_string(opt.kelvin));
    }
    if (!std::isnan(opt.tint)) {
        a.push_back("--tint");
        a.push_back(std::to_string(opt.tint));
    }
    if (opt.iso != 0) {
        a.push_back("--iso");
        a.push_back(std::to_string(opt.iso));
    }
    if (!std::isnan(opt.exposure)) {
        a.push_back("--exposure");
        a.push_back(std::to_string(opt.exposure));
    }
    a.push_back("--lens-correction");
    a.push_back(opt.lensCorrection == 0
                    ? "auto"
                    : (opt.lensCorrection == 1 ? "on" : "off"));
    if (opt.chromaNr == 0) {
        a.push_back("--chroma-nr");
        a.push_back("off");
    } else if (opt.chromaNr == 1) {
        a.push_back("--chroma-nr");
        a.push_back("on");
    }
    if (!opt.sdkPath.empty()) {
        a.push_back("--sdk-path");
        a.push_back(opt.sdkPath);
    }
    a.push_back(opt.input);
    return a;
}

} // namespace

bool CpuAsyncDecoderImpl::open(const CliOptions& opt, const MediaInfo& info,
                               size_t frames, std::string& err)
{
    close();
    width_ = info.width;
    height_ = info.height;
    frameSize_ = width_ * height_ * 6;
    if (width_ == 0 || height_ == 0) {
        err = "无效的分辨率";
        return false;
    }
    if (!audio_.open(opt, info, err))
        return false;

    const size_t want = opt.cpuWorkers > 0 ? static_cast<size_t>(opt.cpuWorkers) : 8;
    const size_t n = frames == 0 ? 1 : (frames < want ? frames : want);
    nWorkers_ = n;

    for (size_t k = 0; k < nWorkers_; ++k) {
        int fds[2];
        if (pipe(fds) != 0) {
            err = "pipe 创建失败: " + std::string(strerror(errno));
            return false;
        }
        std::vector<std::string> args = workerArgs(opt, k, nWorkers_, frames);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args)
            argv.push_back(&s[0]);
        argv.push_back(nullptr);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], 3);
        posix_spawn_file_actions_addclose(&fa, fds[1]);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        pid_t pid = -1;
        int sr = posix_spawn(&pid, "/proc/self/exe", &fa, nullptr,
                             argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        ::close(fds[1]);
        if (sr != 0 || pid <= 0) {
            ::close(fds[0]);
            err = "worker spawn 失败 (status " + std::to_string(sr) + ")";
            return false;
        }
        Worker w;
        w.fd = fds[0];
        w.pid = pid;
        workers_.push_back(w);
    }
    return true;
}

bool CpuAsyncDecoderImpl::waitFrame(size_t frameNo, VideoFrame& out,
                                    std::string& err)
{
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(m_);
            auto it = ready_.find(frameNo);
            if (it != ready_.end()) {
                out = std::move(it->second);
                ready_.erase(it);
                return true;
            }
            if (fail_) {
                err = failMsg_;
                return false;
            }
            const size_t w = frameNo % nWorkers_;
            if (workers_[w].dead) {
                err = "解码 worker " + std::to_string(w) + " 提前退出（帧 " +
                      std::to_string(frameNo) + " 未产出）";
                fail_ = true;
                failMsg_ = err;
                return false;
            }
        }
        if (!pumpOnce(err)) {
            std::lock_guard<std::mutex> lk(m_);
            if (!fail_) {
                fail_ = true;
                failMsg_ = err;
            }
            return false;
        }
    }
}

bool CpuAsyncDecoderImpl::pumpOnce(std::string& err)
{
    std::vector<pollfd> pfds;
    std::vector<size_t> idx;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].fd >= 0 && !workers_[i].dead) {
                pfds.push_back(pollfd{workers_[i].fd, static_cast<short>(POLLIN), 0});
                idx.push_back(i);
            }
        }
    }
    if (pfds.empty()) {
        err = "所有解码 worker 均已退出";
        return false;
    }
    int r = poll(pfds.data(), pfds.size(), -1);
    if (r < 0) {
        if (errno == EINTR)
            return true;
        err = "poll 失败: " + std::string(strerror(errno));
        return false;
    }
    for (size_t j = 0; j < pfds.size(); ++j) {
        if (!(pfds[j].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        if (!readWorkerFrame(idx[j], err))
            return false;
    }
    return true;
}

bool CpuAsyncDecoderImpl::readWorkerFrame(size_t idx, std::string& err)
{
    Worker& w = workers_[idx];
    uint64_t no = 0;
    uint32_t sz = 0;
    ssize_t hr = readFull(w.fd, &no, sizeof(no));
    if (hr == 0) {  // 正常 EOF：该 worker 已写完
        markDead(idx);
        return true;
    }
    if (hr < 0 || hr != static_cast<ssize_t>(sizeof(no)) ||
        readFull(w.fd, &sz, sizeof(sz)) != static_cast<ssize_t>(sizeof(sz))) {
        markDead(idx);
        err = "读取 worker 帧头失败 (worker " + std::to_string(idx) + ")";
        return false;
    }
    if (sz != frameSize_) {
        markDead(idx);
        err = "worker 帧大小不符 (" + std::to_string(sz) + " != " +
              std::to_string(frameSize_) + ")";
        return false;
    }
    VideoFrame f;
    f.width = width_;
    f.height = height_;
    f.frameNo = static_cast<size_t>(no);
    f.rgb.resize(frameSize_, 16);
    if (!f.rgb.data() ||
        readFull(w.fd, f.rgb.data(), frameSize_) !=
            static_cast<ssize_t>(frameSize_)) {
        markDead(idx);
        err = "读取 worker 帧数据失败 (worker " + std::to_string(idx) + ")";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        ready_[f.frameNo] = std::move(f);
        cv_.notify_all();
    }
    return true;
}

void CpuAsyncDecoderImpl::markDead(size_t idx)
{
    std::lock_guard<std::mutex> lk(m_);
    Worker& w = workers_[idx];
    if (w.fd >= 0) {
        ::close(w.fd);
        w.fd = -1;
    }
    w.dead = true;
    int st = 0;
    waitpid(w.pid, &st, WNOHANG);
    cv_.notify_all();
}

void CpuAsyncDecoderImpl::close()
{
    std::vector<pid_t> pids;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& w : workers_) {
            if (w.fd >= 0) {
                ::close(w.fd);
                w.fd = -1;
            }
            if (w.pid > 0)
                pids.push_back(w.pid);
        }
        ready_.clear();
        fail_ = false;
        failMsg_.clear();
    }
    for (pid_t p : pids)
        kill(p, SIGTERM);
    for (pid_t p : pids) {
        int st = 0;
        waitpid(p, &st, 0);
    }
    workers_.clear();
    audio_.close();
}

bool CpuAsyncDecoder::open(const CliOptions& opt, const MediaInfo& info,
                           size_t frames, std::string& err)
{
    if (impl_)
        return true;
    CpuAsyncDecoderImpl* s = new CpuAsyncDecoderImpl();
    if (!s->open(opt, info, frames, err)) {
        delete s;
        return false;
    }
    impl_ = s;
    return true;
}

bool CpuAsyncDecoder::waitFrame(size_t frameNo, VideoFrame& out, std::string& err)
{
    return impl_ &&
           static_cast<CpuAsyncDecoderImpl*>(impl_)->waitFrame(frameNo, out, err);
}

bool CpuAsyncDecoder::decodeAudioWindow(unsigned long long targetSamples,
                                        AudioQueue& audio, std::string& err)
{
    return impl_ && static_cast<CpuAsyncDecoderImpl*>(impl_)
                        ->decodeAudioWindow(targetSamples, audio, err);
}

bool CpuAsyncDecoder::drainAudio(AudioQueue& audio,
                                 unsigned long long targetSamplesLimit,
                                 std::string& err)
{
    return impl_ && static_cast<CpuAsyncDecoderImpl*>(impl_)
                        ->drainAudio(audio, targetSamplesLimit, err);
}

void CpuAsyncDecoder::close()
{
    if (impl_) {
        delete static_cast<CpuAsyncDecoderImpl*>(impl_);
        impl_ = nullptr;
    }
}

int runDecodeWorker(const CliOptions& opt)
{
    // 子进程：重置信号处理，避免继承父进程的清理逻辑；SIGPIPE 默认终止
    // （父进程退出时写管道 EPIPE → 正常退出）
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    std::string err;
    if (!initSdk(opt.sdkPath, err, false)) {
        fprintf(stderr, "worker %ld: initSdk 失败: %s\n", opt.workerId,
                err.c_str());
        return 1;
    }
    MediaInfo info;
    if (!openMedia(opt.input, opt, info, err)) {
        fprintf(stderr, "worker %ld: 打开媒体失败: %s\n", opt.workerId,
                err.c_str());
        return 1;
    }
    SequentialDecoder dec;
    if (!dec.open(opt, info, err)) {
        fprintf(stderr, "worker %ld: 解码器打开失败: %s\n", opt.workerId,
                err.c_str());
        return 1;
    }

    const size_t total = static_cast<size_t>(opt.workerFrames);
    const size_t id = static_cast<size_t>(opt.workerId);
    const size_t n = static_cast<size_t>(opt.workerCount);
    const size_t frameSize = info.width * info.height * 6;
    const int outFd = 3;

    for (size_t f = id; f < total; f += n) {
        VideoFrame fr;
        if (!dec.decodeFrame(f, fr, err)) {
            fprintf(stderr, "worker %ld: 解码失败: %s\n", opt.workerId,
                    err.c_str());
            return 1;
        }
        const uint64_t no = static_cast<uint64_t>(f);
        const uint32_t sz = static_cast<uint32_t>(frameSize);
        if (writeFull(outFd, &no, sizeof(no)) < 0 ||
            writeFull(outFd, &sz, sizeof(sz)) < 0 ||
            writeFull(outFd, fr.rgb.data(), frameSize) < 0) {
            // 父进程已退出（管道关闭）
            return 1;
        }
    }
    dec.close();
    closeMedia();
    shutdownSdk();
    return 0;
}



} // namespace nraw
