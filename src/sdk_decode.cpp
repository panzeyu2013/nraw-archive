#include "archive.h"

#include <cmath>
#include <new>
#include <string>
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
    try {
        out.rgb.resize(gInfo.width * gInfo.height * 6, 16);
    } catch (const std::bad_alloc&) {
        err = "内存不足 (OOM) at frame " + std::to_string(frameNo);
        return false;
    }
    if (!out.rgb.data() || !out.rgb.size()) {
        err = "内存分配失败 at frame " + std::to_string(frameNo);
        return false;
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

}
