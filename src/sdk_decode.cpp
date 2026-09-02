#include "archive.h"

#include <cmath>
#include <csignal>
#include <cstring>
#include <map>
#include <new>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <utility>
#include <vector>

#if defined(__APPLE__)
extern char** environ;  // macOS 的 unistd.h 不声明 environ，需显式声明
#endif

#include "R3DSDK.h"

namespace nraw {

namespace {

R3DSDK::Clip* gClip = nullptr;
R3DSDK::ImageProcessingSettings* gIp = nullptr;
std::string gClipPath;
MediaInfo gInfo;
bool gOpen = false;

// ---- 共享帧缓冲协议（方案 B：memfd + 双槽 + 控制/确认管道） ----
// worker→父进程（fd 3）：填充记录 [u64 frameNo][u32 slotIdx]（12 字节）
// 父进程→worker（fd 5）：槽释放确认 [u32 slotIdx]（4 字节）
// worker 共享帧缓冲（fd 4）：memfd，MAP_SHARED，kShmSlots 个槽，
// 每槽 slotSize = w*h*6 + kFrameGuard 字节；SDK 直接解码进槽，
// 免去管道双向 115MB/帧 的拷贝。双槽让"解码下一帧"与"父进程消费当前帧"
// 完全重叠；槽复用前 worker 必须收到对应槽的确认（pendingAck）。
constexpr int kWorkerOutFd = 3;
constexpr int kShmFd = 4;
constexpr int kAckFd = 5;
constexpr size_t kShmSlots = 2;
constexpr size_t kFrameGuard = 65536;  // SDK 行填充余量（哨兵校验用）


}  // namespace

// 信号处理用的 worker PID 槽（main.cpp 的 onSignal 引用）：主线程在
// spawn/close 时写入，信号处理器只读。固定数组 + sig_atomic_t 计数，
// 无锁、无分配。容量与 --decoders 上限（256）一致。
// 写序约定（保证 async-signal-safe 语义）：先写槽位再自增计数——
// 处理器读到计数 n 时，槽 0..n-1 必已写全；仅存在"漏杀最新 worker"
// 的无害竞态（该 worker 会经 EPIPE/SIGPIPE 自退）。严禁先自增后写
// （会在槽位为 0 时被处理器 kill(0) 杀伤整个进程组）。
constexpr sig_atomic_t kMaxWorkerSlots = 256;
volatile sig_atomic_t g_workerPidSlots[kMaxWorkerSlots] = {0};
volatile sig_atomic_t g_workerPidCount = 0;

namespace {

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
        // SDK 契约：无论 InitializeSdk 成功与否，只要调用过就必须配对
        // FinalizeSdk（R3DSDK.h 明示）。
        R3DSDK::FinalizeSdk();
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
    // 分辨率合理性上限：防损坏/恶意元数据导致 w*h*6 溢出 size_t
    // （后续 sentinel/重排循环以 w*h 为界）。
    if (mi.width == 0 || mi.height == 0 ||
        mi.width > 16384 || mi.height > 16384) {
        err = "异常的剪辑分辨率: " + std::to_string(mi.width) + "x" +
              std::to_string(mi.height);
        delete clip;
        return false;
    }

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
    // 分配余量（64KB）：RED SDK 要求 16 字节对齐且按行对齐的缓冲，奇数高度
    // （如 2322）时 SDK 内部行填充可能向缓冲末尾之外写入 w×6×补齐行字节
    // （堆越界 → 数小时后随机 SIGSEGV）。余量 + 哨兵校验用于证实/排除。
    const size_t guard = 65536;
    const size_t alloc = need + guard;

    // 关键：SDK 的 PixelType_16Bit_RGB_Planar 解码路径逐帧泄漏匿名内存
    // （实测 ~1MB/帧，delete Clip / FinalizeSdk / malloc_trim 均无法回收，
    // 生产 5 worker 任务因此 OOM）；PixelType_16Bit_RGB_Interleaved 与
    // Planar 输出逐位一致（首/中/尾帧抽检 0 差异）且 RSS 平坦。
    // 因此先解码到 Interleaved 复用缓冲，再在本应用内重排为 Planar——
    // 重排是 57.5MB 纯内存拷贝（~30ms），远小于解码耗时（~1s）。
    if (scratch_.size() != alloc) {
        scratch_.resize(alloc, 16);
    }
    if (!scratch_.data()) {
        err = "内存分配失败 at frame " + std::to_string(frameNo);
        return false;
    }
    // 哨兵：解码前在 need 边界处填特征值，解码后检查是否被 SDK 改写
    uint8_t* const sentinel = static_cast<uint8_t*>(scratch_.data()) + need;
    const uint64_t sentinelVal = 0xA55A5AA55AA55A5AULL;
    memcpy(sentinel, &sentinelVal, sizeof(sentinelVal));

    R3DSDK::VideoDecodeJob job;
    job.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
    job.PixelType = R3DSDK::PixelType_16Bit_RGB_Interleaved;
    job.OutputBuffer = scratch_.data();
    job.OutputBufferSize = static_cast<uint64_t>(alloc);  // 允许 SDK 用余量
    job.ImageProcessing = gIp;
    // 显式清零，不依赖闭源默认构造函数的字段初始化
    job.HdrProcessing = nullptr;
    job.OutputFrameMetadata = nullptr;

    if (clip->DecodeVideoFrame(frameNo, job) != R3DSDK::DSDecodeOK) {
        err = "video decode failed at frame " + std::to_string(frameNo);
        return false;
    }
    // 哨兵检查：被改写 = SDK 越界写（记录一次，不阻塞解码）
    if (memcmp(sentinel, &sentinelVal, sizeof(sentinelVal)) != 0) {
        fprintf(stderr, "[SDK越界] frame %zu: RED SDK 写越过了 w*h*6 缓冲边界 "
                        "(需增大余量或按 SDK 要求对齐)\n",
                frameNo);
    }

    // 重排目标是紧致 Planar 帧（need 字节）；guard/哨兵只服务于 scratch_
    if (out.rgb.size() != need) {
        out.rgb.resize(need, 16);
    }
    if (!out.rgb.data()) {
        err = "内存分配失败 at frame " + std::to_string(frameNo);
        return false;
    }
    // Interleaved (RGBRGBRGB...) → Planar (RRR...GGG...BBB)
    const uint16_t* src = static_cast<const uint16_t*>(scratch_.data());
    uint16_t* dst = static_cast<uint16_t*>(out.rgb.data());
    const size_t n = gInfo.width * gInfo.height;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[3 * i];
        dst[n + i] = src[3 * i + 1];
        dst[2 * n + i] = src[3 * i + 2];
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

bool SequentialDecoder::discardAudioTo(unsigned long long sample, std::string& err)
{
    // 续传：解码目标采样号之前的音频块并直接丢弃（不经过队列，避免阻塞死锁）。
    // 音频解码便宜，重解少量块可接受。
    if (!audioOn_)
        return true;
    R3DSDK::Clip* clip = static_cast<R3DSDK::Clip*>(clip_);
    const size_t needBlock = gInfo.audio.maxBlockBytes;
    if (needBlock > audioBlockBuf_.size())
        audioBlockBuf_.resize(needBlock, 512);
    // 与其他两个解码路径一致的 OOM 防护：缓冲分配失败时 data() 为 nullptr，
    // 直接把 nullptr 传给 DecodeAudioBlock 会段错误（续传 seek 是恢复的第一
    // 步，必须以干净错误失败而不是崩溃）
    if (needBlock > 0 && !audioBlockBuf_.data()) {
        err = "内存不足 (OOM)";
        return false;
    }
    while (audioBlockIdx_ < gInfo.audio.blockCount &&
           decodedSamples_ < sample) {
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
        // 与 decodeAudioWindow/decodeOneAudioBlock 一致的声道对齐校验：
        // 不对齐时静默取整会让续传起点偏移 <channels 个采样（轻微重复）。
        // 正常剪辑不可达（同样的问题会在正常解码路径报错），仅防御一致。
        if (gInfo.audio.channels <= 0 ||
            words % static_cast<size_t>(gInfo.audio.channels) != 0) {
            err = "audio block size not aligned to channels at block " +
                  std::to_string(audioBlockIdx_ - 1);
            return false;
        }
        const size_t blockSamples =
            words / static_cast<size_t>(gInfo.audio.channels);
        decodedSamples_ += blockSamples;
    }
    return true;
}

bool SequentialDecoder::seekAudioTo(unsigned long long sample, std::string& err)
{
    return discardAudioTo(sample, err);
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

// 单块音频解码：生成 ≤1 个音频块（供收尾"解一块写一块"——AudioQueue 容量
// 有限，drainAudio 一次生成大量块会因队列满而单线程死锁）
bool SequentialDecoder::decodeOneAudioBlock(AudioQueue& audio,
                                            unsigned long long targetSamples,
                                            std::string& err)
{
    if (!audioOn_)
        return false;  // 无音频：完成
    if (audioBlockIdx_ >= gInfo.audio.blockCount ||
        decodedSamples_ >= targetSamples)
        return false;  // 无更多块：完成（err 为空）
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
        return true;  // 空块：继续下一块
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
    // 与 drainAudio/discardAudioTo 一致：decodedSamples_ 是每声道采样数
    // （words = 全部声道的 32 位字）。此前误用 words 会使立体声下采样号
    // 虚增 2 倍：收尾/续传的音频 pts 错位、目标限制提前到达（音频截半）。
    decodedSamples_ += words / static_cast<size_t>(gInfo.audio.channels);
    try {
        audio.push(std::move(p));
    } catch (const std::bad_alloc&) {
        // 与 decodeAudioWindow 一致：队列满/OOM 必须转为干净错误，
        // 否则异常逃逸到 main → std::terminate
        err = "内存不足 (OOM)";
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
// worker 子进程（各自用经典 API 解码 帧号 ≡ k (mod N)，每个 ~1fps）。
//
// 帧传输走共享帧缓冲协议（memfd + 双槽）：SDK 直接解码进共享内存槽
// （Interleaved），worker 只写 12 字节填充记录，父进程从共享内存重排为
// Planar（57.5MB 读+写）后确认槽释放。相比管道直传省去 115MB/帧 的双向
// 内核拷贝，且"解码下一帧"与"父进程消费当前帧"完全重叠。纯 CPU，不依赖
// GPU/OpenCL。
//
// SDK 解码路径存在进程内无法回收的内存积累（Planar 像素路径 ~1MB/帧，且随机
// 跨区解码进一步积累；delete Clip / FinalizeSdk / malloc_trim 均无法回收——
// 实测）。为此：1) 统一使用 Interleaved 解码（输出与 Planar 逐位一致），消除
// Planar 路径泄漏；2) 代际回收（--worker-batch）：每个 worker 进程每代只解码
// 一批帧后干净退出，父进程检测干净 EOF 后 spawn 新一代继续（共享帧缓冲
// 代际复用，不重建），进程退出即归还全部内存——峰值内存有界，与 SDK
// 内部状态无关。
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
    bool decodeOneAudioBlock(AudioQueue& audio, unsigned long long targetSamples,
                             std::string& err)
    {
        return audio_.decodeOneAudioBlock(audio, targetSamples, err);
    }
    bool seekAudioTo(unsigned long long sample, std::string& err)
    {
        return audio_.seekAudioTo(sample, err);
    }
    void close();

private:
    bool pumpOnce(std::string& err, size_t focus);
    bool readWorkerFrame(size_t idx, std::string& err);
    void markDead(size_t idx);
    // 代际回收：worker idx 的当前批次完成（干净 EOF）后，spawn 新一代
    // 从 nextStart 继续解码（模分布）。旧进程退出即归还其全部内存。
    bool respawnWorker(size_t idx, size_t nextStart, std::string& err);

    struct Worker {
        int fd = -1;          // 控制管道读端（父进程，接收填充记录）
        int ackFd = -1;       // 确认管道写端（父进程，发送槽释放确认）
        int shmFd = -1;       // 共享帧缓冲 memfd（父进程持有，代际复用）
        void* shmMap = nullptr;  // 共享帧缓冲映射（父进程，重排时读取）
        size_t slotSize = 0;  // 单槽字节数（w*h*6 + guard）
        pid_t pid = -1;
        bool dead = false;
        std::string exitDetail;  // worker 退出信号/码（markDead 记录）
    };

    std::mutex m_;
    std::vector<Worker> workers_;
    CliOptions opt_;                       // 代际回收 spawn 新一代 worker 所需参数
    std::vector<size_t> received_;  // 每 worker 已收到的帧数（帧号校验）
    std::map<size_t, VideoFrame> ready_;   // 已到达但尚未按序消费的帧
    size_t nWorkers_ = 8;
    size_t start_ = 0;                     // 续传：解码起始帧
    size_t frames_ = 0;                    // 需要解码的总帧数（clamp 后）
    size_t width_ = 0, height_ = 0;
    size_t frameSize_ = 0;
    // ready_ 有界背压（见 pumpOnce/readWorkerFrame）：waitFrame 按序消费
    // （1 帧/轮）而 pumpOnce 每轮会尝试读所有 worker 的下一帧。若 x265
    // 编码慢（enc.write 阻塞），waitFrame 停在当前帧，pumpOnce 仍持续把
    // 后续帧塞进 ready_（每帧 57.5MB）→ 无上限无限膨胀 → OOM（实测单
    // worker RSS 涨到 12.6GB）。上限 = nWorkers×2+1：允许每 worker 在途
    // 1 帧 + 等待帧（focus 帧豁免，瞬态至多 +1）；超过则跳过读取（数据
    // 留在管道，worker writeFull 阻塞形成自然背压，解码速度自适应编码速度）。
    size_t maxReady_ = 16;
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

// worker 子进程命令行（父进程构造，posix_spawn 路径用 selfExePath() 解析）。
// 模分布调度：worker id 解码 帧号 ≡ id (mod count)，workerStart = 本代
// 首帧 - id（首帧 = workerStart + id）。代际回收：每代解码 --worker-batch
// 帧后干净退出，父进程为下一批重新 spawn。
std::vector<std::string> workerArgs(const CliOptions& opt, size_t id,
                                    size_t count, size_t total,
                                    size_t workerStart)
{
    std::vector<std::string> a;
    a.emplace_back("nraw_archive");
    a.push_back("--decode-worker");
    a.push_back(std::to_string(id));
    a.push_back("--worker-count");
    a.push_back(std::to_string(count));
    a.push_back("--worker-frames");
    a.push_back(std::to_string(total));
    a.push_back("--worker-start");
    a.push_back(std::to_string(workerStart));
    a.push_back("--worker-batch");
    a.push_back(std::to_string(opt.workerBatch));
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

// 创建控制/确认管道并 posix_spawn worker（open 与 respawnWorker 共用）。
// 子进程 fd 布局：3=填充记录管道写端，4=共享帧缓冲 memfd，5=确认管道读端。
// 关键约束：
//  - 所有继承 fd 均 FD_CLOEXEC（防 worker 间互相继承）
//  - 任何一端都不能占用 fd 3：file_actions 顺序为 dup2(pipeW→3)、
//    close(pipeW)、close(pipeR)，若 pipeR==3 会在子进程内把刚 dup2 的
//    写端关掉 → worker 输出 fd 失效（F_DUPFD_CLOEXEC 搬迁到高位）
//  - selfExePath() 为空时明确报错（不交给 posix_spawn 返回含糊的 ENOENT）
static bool spawnWorkerProc(const CliOptions& opt, size_t id, size_t count,
                            size_t total, size_t workerStart, int shmFd,
                            int ackReadFd, int& readFd, pid_t& pid,
                            std::string& err)
{
    int fds[2];
    if (pipe(fds) != 0) {
        err = "pipe 创建失败: " + std::string(strerror(errno));
        return false;
    }
    for (int k = 0; k < 2; ++k) {
        if (fds[k] == 3) {
            const int nfd = fcntl(fds[k], F_DUPFD_CLOEXEC, 10);
            if (nfd < 0) {
                ::close(fds[0]);
                ::close(fds[1]);
                err = "管道 fd 搬迁失败: " + std::string(strerror(errno));
                return false;
            }
            ::close(fds[k]);
            fds[k] = nfd;
        }
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(shmFd, F_SETFD, FD_CLOEXEC);
    fcntl(ackReadFd, F_SETFD, FD_CLOEXEC);
    std::vector<std::string> args = workerArgs(opt, id, count, total, workerStart);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args)
        argv.push_back(&s[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], kWorkerOutFd);
    posix_spawn_file_actions_adddup2(&fa, shmFd, kShmFd);
    posix_spawn_file_actions_adddup2(&fa, ackReadFd, kAckFd);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    const std::string selfExe = nraw::selfExePath();
    if (selfExe.empty()) {
        posix_spawn_file_actions_destroy(&fa);
        ::close(fds[0]);
        ::close(fds[1]);
        err = "无法解析自身可执行文件路径（selfExePath 为空）";
        return false;
    }
    int sr = posix_spawn(&pid, selfExe.c_str(), &fa, nullptr, argv.data(),
                         environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (sr != 0 || pid <= 0) {
        ::close(fds[0]);
        err = "worker spawn 失败 (status " + std::to_string(sr) + ")";
        return false;
    }
    readFd = fds[0];
    return true;
}

// 创建共享帧缓冲（双槽映射）。Linux 用免特权 memfd_create（容器内可用）；
// 其他平台回退 POSIX shm_open（立即 unlink 解链，映射仍有效）。失败明确报错。
static bool createShmSlots(size_t frameSize, int& shmFd, void*& shmMap,
                           std::string& err)
{
    shmFd = -1;
    shmMap = nullptr;
#if defined(__linux__)
#ifdef MFD_CLOEXEC
    shmFd = memfd_create("nraw_frame_shm", MFD_CLOEXEC);
#else
    shmFd = static_cast<int>(syscall(SYS_memfd_create, "nraw_frame_shm", 0));
#endif
    if (shmFd < 0) {
        err = "memfd_create 失败: " + std::string(strerror(errno));
        return false;
    }
#else
    // POSIX 共享内存回退（macOS 等无 memfd 的平台）
    static int shmCounter = 0;
    char name[64];
    snprintf(name, sizeof(name), "/nraw_shm_%d_%d",
             static_cast<int>(getpid()), shmCounter++);
    shmFd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (shmFd < 0) {
        err = "shm_open 失败: " + std::string(strerror(errno));
        return false;
    }
    shm_unlink(name);  // 立即解链：fd 与映射不受影响，退出自动清理
#endif
    fcntl(shmFd, F_SETFD, FD_CLOEXEC);
    const size_t slotSize = frameSize + kFrameGuard;
    const size_t total = slotSize * kShmSlots;
    if (ftruncate(shmFd, static_cast<off_t>(total)) != 0) {
        err = "ftruncate 共享帧缓冲失败: " + std::string(strerror(errno));
        ::close(shmFd);
        shmFd = -1;
        return false;
    }
    void* m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd,
                   0);
    if (m == MAP_FAILED) {
        err = "mmap 共享帧缓冲失败: " + std::string(strerror(errno));
        ::close(shmFd);
        shmFd = -1;
        return false;
    }
    shmMap = m;
    return true;
}

} // namespace

bool CpuAsyncDecoderImpl::open(const CliOptions& opt, const MediaInfo& info,
                               size_t frames, std::string& err)
{
    close();
    opt_ = opt;
    width_ = info.width;
    height_ = info.height;
    frameSize_ = width_ * height_ * 6;
    if (width_ == 0 || height_ == 0) {
        err = "无效的分辨率";
        return false;
    }
    if (!audio_.open(opt, info, err))
        return false;

    const size_t want = opt.cpuWorkers > 0 ? static_cast<size_t>(opt.cpuWorkers) : 3;  // 防御值（CPU 路径下必已被调度块解析）
    const size_t n = frames == 0 ? 1 : (frames < want ? frames : want);
    nWorkers_ = n;
    start_ = opt.workerStart > 0 ? static_cast<size_t>(opt.workerStart) : 0;
    frames_ = frames;
    // 模分布调度（帧号 ≡ k (mod N)）：waitFrame 按序消费，focus 背压只
    // poll 归属 worker——所有 worker 全程并行解码（连续区间会因按序消费
    // 串行化 worker，实测 3.2fps → 0.9fps）。SDK 随机跨区解码的内存积累
    // 由代际回收（--worker-batch）封顶：每代进程退出即归还全部内存。
    // 背压上限：每 worker 在途 1 帧 + 等待帧（+1 余量防抖动）
    maxReady_ = nWorkers_ * 2 + 1;
    received_.assign(nWorkers_, 0);

    for (size_t k = 0; k < nWorkers_; ++k) {
        // 每 worker：共享帧缓冲（memfd 双槽）+ 确认管道（父→worker）
        Worker w;
        if (!createShmSlots(frameSize_, w.shmFd, w.shmMap, err))
            return false;
        w.slotSize = frameSize_ + kFrameGuard;
        int ackPipe[2];
        if (pipe(ackPipe) != 0) {
            err = "确认管道创建失败: " + std::string(strerror(errno));
            ::close(w.shmFd);
            munmap(w.shmMap, w.slotSize * kShmSlots);
            return false;
        }
        fcntl(ackPipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(ackPipe[1], F_SETFD, FD_CLOEXEC);
        w.ackFd = ackPipe[1];

        int readFd = -1;
        pid_t pid = -1;
        if (!spawnWorkerProc(opt, k, nWorkers_, frames, start_, w.shmFd,
                             ackPipe[0], readFd, pid, err)) {
            ::close(ackPipe[0]);
            ::close(w.ackFd);
            ::close(w.shmFd);
            munmap(w.shmMap, w.slotSize * kShmSlots);
            return false;
        }
        ::close(ackPipe[0]);
        w.fd = readFd;
        w.pid = pid;
        workers_.push_back(w);
        // 注册到信号处理槽（主线程写，信号处理器只读；先写槽再增计数）
        if (g_workerPidCount < kMaxWorkerSlots) {
            g_workerPidSlots[g_workerPidCount] =
                static_cast<sig_atomic_t>(pid);
            ++g_workerPidCount;
        }
    }
    return true;
}

// 代际回收：当前批次完成（干净 EOF）后 spawn 新一代 worker 继续解码
// （首帧 = nextStart，模分布）。仅主线程调用（pumpOnce → readWorkerFrame），
// 整个 CpuAsyncDecoderImpl 严格单线程（m_ 仅作防御），无并发。
// 新一代进程从零初始化 SDK，上一代的全部内存（含 SDK 不可回收
// 的积累）随进程退出归还操作系统——峰值内存与泄漏速率无关。
bool CpuAsyncDecoderImpl::respawnWorker(size_t idx, size_t nextStart,
                                        std::string& err)
{
    // 新一代沿用同一共享帧缓冲（memfd 属父进程，代际间不重建），
    // 确认管道每代新建（旧一代的读端随进程退出关闭）。
    Worker& w = workers_[idx];
    int ackPipe[2];
    if (pipe(ackPipe) != 0) {
        err = "确认管道创建失败: " + std::string(strerror(errno));
        return false;
    }
    fcntl(ackPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(ackPipe[1], F_SETFD, FD_CLOEXEC);

    int readFd = -1;
    pid_t pid = -1;
    if (!spawnWorkerProc(opt_, idx, nWorkers_, frames_, nextStart - idx,
                         w.shmFd, ackPipe[0], readFd, pid, err)) {
        ::close(ackPipe[0]);
        ::close(ackPipe[1]);
        err = "worker respawn 失败: " + err;
        return false;
    }
    ::close(ackPipe[0]);
    // 替换控制管道读端、确认管道写端与 PID，并更新信号槽。
    // 调用方（EOF 分支）已在 SIGINT/SIGTERM 屏蔽临界区内收殓旧 pid。
    const pid_t oldPid = w.pid;
    if (w.fd >= 0)
        ::close(w.fd);
    if (w.ackFd >= 0)
        ::close(w.ackFd);
    w.fd = readFd;
    w.ackFd = ackPipe[1];
    w.pid = pid;
    w.dead = false;
    w.exitDetail.clear();
    bool slotSet = false;
    for (volatile sig_atomic_t i = 0;
         i < nraw::g_workerPidCount && i < kMaxWorkerSlots; ++i) {
        if (nraw::g_workerPidSlots[i] == static_cast<sig_atomic_t>(oldPid)) {
            nraw::g_workerPidSlots[i] = static_cast<sig_atomic_t>(pid);
            slotSet = true;
            break;
        }
    }
    if (!slotSet && nraw::g_workerPidCount < kMaxWorkerSlots) {
        nraw::g_workerPidSlots[nraw::g_workerPidCount] =
            static_cast<sig_atomic_t>(pid);
        ++nraw::g_workerPidCount;
    }
    fprintf(stderr, "worker %zu 批次完成（已解码 %zu 帧），重启下一批（PID %d）\n",
            idx, received_[idx], pid);
    return true;
}

bool CpuAsyncDecoderImpl::waitFrame(size_t frameNo, VideoFrame& out,
                                    std::string& err)
{
    for (;;) {
        size_t w = 0;
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
            // 模分布：帧归属 = (frameNo - start_) % nWorkers_
            w = frameNo >= start_ ? (frameNo - start_) % nWorkers_ : 0;
            if (workers_[w].dead) {
                err = "解码 worker " + std::to_string(w) + " 提前退出（帧 " +
                      std::to_string(frameNo) + " 未产出）" +
                      (workers_[w].exitDetail.empty()
                           ? ""
                           : " [" + workers_[w].exitDetail + "]");
                fail_ = true;
                failMsg_ = err;
                return false;
            }
        }
        if (!pumpOnce(err, w)) {
            std::lock_guard<std::mutex> lk(m_);
            if (!fail_) {
                fail_ = true;
                failMsg_ = err;
            }
            return false;
        }
    }
}

// focus = 目标 worker（waitFrame 等待帧的归属 worker）；ready_ 积压超限时
// 只 poll 该 worker——它产出的下一帧正是 waitFrame 需要的帧号（模分布
// 保证），其余 worker 的数据留在管道（writeFull 阻塞 → 自然背压）。
// ready_ 上界 = maxReady_（focus 帧豁免硬上限、读入即被 waitFrame 消费，
// 瞬态至多 maxReady_+1）：focus 启发只决定"本轮 poll 谁"，读取循环内对
// 非 focus 帧做硬上限检查（pumpOnce 单线程调用，检查与插入之间无其他
// 写者）。
bool CpuAsyncDecoderImpl::pumpOnce(std::string& err, size_t focus)
{
    std::vector<pollfd> pfds;
    std::vector<size_t> idx;
    {
        std::lock_guard<std::mutex> lk(m_);
        // 背压启发：ready_ 接近上限时只 poll focus（减少本轮 poll 的无用
        // 唤醒；真正的一帧一验上限在下方读取循环）
        const bool backpressured = ready_.size() + 1 >= maxReady_;
        for (size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].fd >= 0 && !workers_[i].dead) {
                // 背压：只 poll 目标 worker（其他 worker 的帧留给后续轮次）
                if (backpressured && i != focus)
                    continue;
                pfds.push_back(pollfd{workers_[i].fd, static_cast<short>(POLLIN), 0});
                idx.push_back(i);
            }
        }
    }
    if (pfds.empty()) {
        err = "所有解码 worker 均已退出";
        return false;
    }
    // 带超时 poll（1000ms）：防单 worker 挂死（如 NFS 卡死）时永久阻塞——
    // 超时返回后 waitFrame 重新检查 worker 死亡标志（workers_[w].dead）
    // 与 fail_，从而能报错退出而非无限等待。
    int r = poll(pfds.data(), pfds.size(), 1000);
    if (r < 0) {
        if (errno == EINTR)
            return true;
        err = "poll 失败: " + std::string(strerror(errno));
        return false;
    }
    if (r == 0)
        return true;  // 超时：无数据，等 waitFrame 下一轮重查
    for (size_t j = 0; j < pfds.size(); ++j) {
        if (!(pfds[j].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        {
            std::lock_guard<std::mutex> lk(m_);
            // 硬上限：剩余帧留在管道（worker 自然背压）。
            // focus 豁免：它产出的正是 waitFrame 等待的下一帧（模分布
            // 保证），读入后立即被消费——若不豁免，ready_ 满时 focus 帧
            // 被拒读 → waitFrame 永久空转（livelock）。
            if (ready_.size() >= maxReady_ && idx[j] != focus)
                break;
        }
        if (!readWorkerFrame(idx[j], err))
            return false;
    }
    return true;
}

bool CpuAsyncDecoderImpl::readWorkerFrame(size_t idx, std::string& err)
{
    Worker& w = workers_[idx];
    uint64_t no = 0;
    uint32_t slot = 0;
    ssize_t hr = readFull(w.fd, &no, sizeof(no));
    if (hr == 0) {  // 该 worker 写端已关闭
        // 正常批次完成 → 代际回收重启下一批；异常提前退出 → 失败。
        // 本代下一帧 = start_ + idx + received_[idx]×nWorkers_（模分布）。
        // 关键区：暂掩 SIGINT/SIGTERM，保证"回收旧 pid → 替换信号槽"
        // 之间信号处理器不会对可能已被 OS 复用的旧 pid 执行 kill。
        sigset_t ss, oldMask;
        sigemptyset(&ss);
        sigaddset(&ss, SIGINT);
        sigaddset(&ss, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &ss, &oldMask);

        size_t next = 0;
        bool needMore = false;
        {
            std::lock_guard<std::mutex> lk(m_);
            next = start_ + idx + received_[idx] * nWorkers_;
            needMore = next < frames_;
        }
        int st = 0;
        bool reaped = waitpid(w.pid, &st, WNOHANG) == w.pid;
        if (!reaped)
            reaped = waitpid(w.pid, &st, 0) == w.pid;  // EOF 即已退出，阻塞收殓安全
        const bool cleanExit =
            reaped && WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (needMore && cleanExit) {
            std::string rerr;
            const bool ok = respawnWorker(idx, next, rerr);
            pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
            if (!ok) {
                markDead(idx);
                err = rerr;
                return false;
            }
            return true;
        }
        if (!cleanExit) {
            if (WIFSIGNALED(st))
                w.exitDetail = "signal " + std::to_string(WTERMSIG(st));
            else if (WIFEXITED(st))
                w.exitDetail = "exit " + std::to_string(WEXITSTATUS(st));
            else
                w.exitDetail = "unknown status";
        }
        markDead(idx);
        pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
        if (needMore && !cleanExit) {
            err = "worker " + std::to_string(idx) + " 提前退出（批内）";
            return false;
        }
        if (!cleanExit)
            fprintf(stderr,
                    "worker %zu 收尾阶段异常退出 [%s]（帧数据已完整，忽略）\n",
                    idx, w.exitDetail.c_str());
        return true;
    }
    if (hr < 0 || hr != static_cast<ssize_t>(sizeof(no)) ||
        readFull(w.fd, &slot, sizeof(slot)) !=
            static_cast<ssize_t>(sizeof(slot))) {
        markDead(idx);
        err = "读取 worker 填充记录失败 (worker " + std::to_string(idx) + ")";
        return false;
    }
    if (slot >= kShmSlots) {
        markDead(idx);
        err = "worker 槽位号非法 (" + std::to_string(slot) + " >= " +
              std::to_string(kShmSlots) + ")";
        return false;
    }
    // 校验帧号 = 该 worker 应产出的下一帧（start_ + idx + 已收帧数×nWorkers_，模分布）：
    // 坏帧号会让 ready_ 滞留 57.5MB/帧（内存膨胀）或把错误帧喂给 x265（错帧链）
    bool badFrame = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const size_t expect =
            start_ + idx + received_[idx] * workers_.size();
        if (no != expect) {
            // 锁内不能调 markDead：markDead 会再次加 m_（非递归互斥 → 自死锁
            // EDEADLK → std::system_error 未捕获 → std::terminate）。先记录
            // 错误，释放锁后再标记 worker 死亡。
            err = "worker " + std::to_string(idx) + " 帧号乱序（收到 " +
                  std::to_string(no) + "，期望 " + std::to_string(expect) + "）";
            badFrame = true;
        } else {
            ++received_[idx];
        }
    }
    if (badFrame) {
        markDead(idx);
        return false;
    }
    VideoFrame f;
    f.width = width_;
    f.height = height_;
    f.frameNo = static_cast<size_t>(no);
    f.rgb.resize(frameSize_, 16);
    if (!f.rgb.data()) {
        // 父进程内存不足：不要误报为 worker 故障/管道错误
        markDead(idx);
        err = "父进程内存不足 (OOM)，无法接收 worker " + std::to_string(idx) +
              " 的帧 " + std::to_string(no);
        return false;
    }
    // 从共享帧缓冲重排：Interleaved (RGBRGB...) → Planar (RRR...GGG...BBB)
    // 管道确认/填充记录的内存序保证槽内容已就绪。
    const uint16_t* src = reinterpret_cast<const uint16_t*>(
        static_cast<uint8_t*>(w.shmMap) + static_cast<size_t>(slot) * w.slotSize);
    uint16_t* dst = static_cast<uint16_t*>(f.rgb.data());
    const size_t n = width_ * height_;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = src[3 * i];
        dst[n + i] = src[3 * i + 1];
        dst[2 * n + i] = src[3 * i + 2];
    }
    // 槽释放确认（EPIPE = worker 已退出/新一代接管，忽略）
    {
        ssize_t aw = write(w.ackFd, &slot, sizeof(slot));
        (void)aw;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        ready_[f.frameNo] = std::move(f);
    }
    return true;
}

void CpuAsyncDecoderImpl::markDead(size_t idx)
{
    // 收殓 + 中和信号槽必须处于同一信号屏蔽临界区：一旦 waitpid 回收旧
    // pid，OS 可能将其复用给无关进程，槽位里残留的旧 pid 会让后续
    // SIGINT/SIGTERM 的 onSignal 误杀无辜进程（长任务中最终 EOF 的 worker
    // 其槽位会残留到整个编码结束，复用概率不低）。中和为 0 后 onSignal
    // 的 p>0 守卫会跳过该槽位。
    // 注意：pthread_sigmask 只屏蔽当前线程；其余线程（sha256/x265）理论上
    // 仍可能处理信号。但收殓→中和窗口仅微秒级，且 Linux pid_max=4194304，
    // 该窗口内 pid 被复用需要数百万进程创建——实际不可达，风险可忽略。
    sigset_t ss, oldMask;
    sigemptyset(&ss);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &ss, &oldMask);

    std::lock_guard<std::mutex> lk(m_);
    Worker& w = workers_[idx];
    if (w.fd >= 0) {
        ::close(w.fd);
        w.fd = -1;
    }
    w.dead = true;
    int st = 0;
    if (w.pid > 0 && waitpid(w.pid, &st, WNOHANG) == w.pid) {
        // 记录退出信号/码（SIGSEGV=139 等），供错误信息区分崩溃原因
        if (WIFSIGNALED(st))
            w.exitDetail = "signal " + std::to_string(WTERMSIG(st));
        else if (WIFEXITED(st))
            w.exitDetail = "exit " + std::to_string(WEXITSTATUS(st));
        else
            w.exitDetail = "unknown status";
    }
    // 中和该 worker 的信号槽（可能已由 respawnWorker 原位替换为新 pid，
    // 此时槽位属于新一代 worker，跳过；仅旧 pid 匹配时清零）
    for (volatile sig_atomic_t i = 0;
         i < nraw::g_workerPidCount && i < kMaxWorkerSlots; ++i) {
        if (nraw::g_workerPidSlots[i] ==
            static_cast<sig_atomic_t>(w.pid)) {
            nraw::g_workerPidSlots[i] = 0;
        }
    }
    pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
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
            if (w.ackFd >= 0) {
                ::close(w.ackFd);
                w.ackFd = -1;
            }
            if (w.shmMap) {
                munmap(w.shmMap, w.slotSize * kShmSlots);
                w.shmMap = nullptr;
            }
            if (w.shmFd >= 0) {
                ::close(w.shmFd);
                w.shmFd = -1;
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
    g_workerPidCount = 0;  // 清空信号槽（信号处理器此时不再需要 kill）
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

bool CpuAsyncDecoder::decodeOneAudioBlock(AudioQueue& audio,
                                          unsigned long long targetSamples,
                                          std::string& err)
{
    return impl_ && static_cast<CpuAsyncDecoderImpl*>(impl_)
                        ->decodeOneAudioBlock(audio, targetSamples, err);
}

bool CpuAsyncDecoder::seekAudioTo(unsigned long long sample, std::string& err)
{
    return impl_ && static_cast<CpuAsyncDecoderImpl*>(impl_)
                        ->seekAudioTo(sample, err);
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

    const size_t total = static_cast<size_t>(opt.workerFrames);
    const size_t start = opt.workerStart > 0
                              ? static_cast<size_t>(opt.workerStart)
                              : 0;
    const size_t id = static_cast<size_t>(opt.workerId);
    const size_t n = static_cast<size_t>(opt.workerCount);
    const size_t frameSize = info.width * info.height * 6;
    const size_t slotSize = frameSize + kFrameGuard;

    // ---- 共享帧缓冲协议 ----
    // fd 4 = memfd（父进程创建，MAP_SHARED 双槽）；fd 5 = 确认管道读端。
    // SDK 直接解码进槽（Interleaved），父进程从共享内存重排并确认——
    // 免去每帧 115MB 的管道双向拷贝，解码下一帧与消费当前帧完全重叠。
    struct stat shmSt;
    if (fstat(kShmFd, &shmSt) != 0 || shmSt.st_size <= 0) {
        fprintf(stderr, "worker %ld: 共享帧缓冲 fd 无效\n", opt.workerId);
        return 1;
    }
    const size_t nSlots = static_cast<size_t>(shmSt.st_size) / slotSize;
    if (nSlots < kShmSlots || shmSt.st_size % slotSize != 0) {
        fprintf(stderr,
                "worker %ld: 共享帧缓冲尺寸异常 (%lld / %zu)\n",
                opt.workerId, static_cast<long long>(shmSt.st_size), slotSize);
        return 1;
    }
    void* shm = mmap(nullptr, static_cast<size_t>(shmSt.st_size),
                     PROT_READ | PROT_WRITE, MAP_SHARED, kShmFd, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "worker %ld: mmap 共享帧缓冲失败: %s\n", opt.workerId,
                strerror(errno));
        return 1;
    }

    // 模分布解码：本 worker 解码 帧号 ≡ id (mod n)（REDCODE 帧内压缩，
    // 任意帧号可直接解码）。每代只解码 --worker-batch 帧后干净退出——
    // SDK 解码路径存在 FinalizeSdk 无法回收的内存积累，进程退出即归还
    // 全部内存（代际回收，峰值与泄漏速率无关）。
    const size_t batch = static_cast<size_t>(opt.workerBatch);  // parseArgs 保证 ≥1
    uint32_t pendingAck[kShmSlots] = {0, 0};
    size_t i = 0;
    for (size_t f = start + id; f < total && i < batch; f += n, ++i) {
        const uint32_t s = static_cast<uint32_t>(i % kShmSlots);
        // 等待该槽被父进程确认释放（阻塞读确认；父进程退出 → EOF → 退出）
        while (pendingAck[s] > 0) {
            uint32_t ackSlot = 0;
            const ssize_t r = read(kAckFd, &ackSlot, sizeof(ackSlot));
            if (r == 0)
                return 1;  // 父进程已退出
            if (r < 0 && errno == EINTR)
                continue;
            if (r == static_cast<ssize_t>(sizeof(ackSlot)) &&
                ackSlot < kShmSlots)
                --pendingAck[ackSlot];
        }
        uint8_t* slot = static_cast<uint8_t*>(shm) +
                        static_cast<size_t>(s) * slotSize;
        // SDK 直接解码进共享槽（Interleaved；哨兵校验行填充越界）
        uint8_t* const sentinel = slot + frameSize;
        const uint64_t sentinelVal = 0xA55A5AA55AA55A5AULL;
        memcpy(sentinel, &sentinelVal, sizeof(sentinelVal));
        R3DSDK::VideoDecodeJob job;
        job.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
        job.PixelType = R3DSDK::PixelType_16Bit_RGB_Interleaved;
        job.OutputBuffer = slot;
        job.OutputBufferSize = static_cast<uint64_t>(slotSize);
        job.ImageProcessing = gIp;
        job.HdrProcessing = nullptr;
        job.OutputFrameMetadata = nullptr;
        const R3DSDK::DecodeStatus dst = gClip->DecodeVideoFrame(f, job);
        if (dst != R3DSDK::DSDecodeOK) {
            fprintf(stderr, "worker %ld: 解码失败 frame %zu (status %d)\n",
                    opt.workerId, f, static_cast<int>(dst));
            return 1;
        }
        if (memcmp(sentinel, &sentinelVal, sizeof(sentinelVal)) != 0) {
            fprintf(stderr,
                    "[SDK越界] frame %zu: RED SDK 写越过了 w*h*6 缓冲边界 "
                    "(需增大余量或按 SDK 要求对齐)\n",
                    f);
        }
        // 填充记录 [u64 frameNo][u32 slotIdx] → 父进程
        const uint64_t no = static_cast<uint64_t>(f);
        if (writeFull(kWorkerOutFd, &no, sizeof(no)) < 0 ||
            writeFull(kWorkerOutFd, &s, sizeof(s)) < 0) {
            // 父进程已退出（管道关闭）
            return 1;
        }
        ++pendingAck[s];
    }
    closeMedia();
    shutdownSdk();
    return 0;
}



} // namespace nraw
