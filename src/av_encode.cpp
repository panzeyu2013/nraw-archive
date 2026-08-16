#include "archive.h"

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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace nraw {

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
    bool writeFrame(const VideoFrame& f, std::string& err);
    bool writeAudioPkt(const AudioPacket& p, std::string& err);
    bool finishSession(std::string& err);
    ~EncodeSessionImpl();

private:
    bool flushEnc(std::string& err);
    bool sendFrame(const VideoFrame& f, std::string& err);

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
    bool renameFailed_ = false;
};

bool EncodeSessionImpl::openSession(const CliOptions& opt, const MediaInfo& info,
                                    std::string& err)
{
    info_ = info;
    dumpOnly_ = !opt.dumpRef.empty();
    finalPath_ = opt.output;
    outPath_ = dumpOnly_ ? opt.dumpRef : (opt.output + ".part");

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
    if (avcodec_open2(enc_, codec, nullptr) < 0) {
        err = "x265 初始化失败 (preset=\"" + opt.preset + "\", crf=" +
              std::to_string(opt.crf) + ")";
        return false;
    }

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
        ast_->codecpar->channels = info.audio.channels;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
        AVChannelLayout lay;
        av_channel_layout_default(&lay, info.audio.channels);
        av_channel_layout_copy(&ast_->codecpar->ch_layout, &lay);
#else
        ast_->codecpar->channel_layout = av_get_default_channel_layout(info.audio.channels);
#endif
        ast_->codecpar->bits_per_coded_sample = 24;
        ast_->codecpar->block_align = info.audio.channels * 3;
        ast_->codecpar->bit_rate = info.audio.sampleRate * info.audio.channels * 24;
        ast_->codecpar->codec_tag = MKTAG('i', 'n', '2', '4');
    }

    AVDictionary* hdrOpts = nullptr;
    if (opt.faststart)
        av_dict_set(&hdrOpts, "movflags", "faststart", 0);
    int hr = avformat_write_header(oc_, opt.faststart ? &hdrOpts : nullptr);
    av_dict_free(&hdrOpts);
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
    if (av_interleaved_write_frame(oc_, &pkt) < 0) {
        err = "音频写入失败";
        return false;
    }
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
            err = "x265 收尾失败";
            return false;
        }
        pkt.stream_index = vst_->index;
        av_packet_rescale_ts(&pkt, enc_->time_base, vst_->time_base);
        if (av_interleaved_write_frame(oc_, &pkt) < 0) {
            err = "收尾包写入失败 (可能磁盘已满)";
            return false;
        }
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
            return true;
        if (r < 0) {
            err = "x265 收包失败";
            return false;
        }
        pkt.stream_index = vst_->index;
        av_packet_rescale_ts(&pkt, enc_->time_base, vst_->time_base);
        if (av_interleaved_write_frame(oc_, &pkt) < 0) {
            err = "视频包写入失败 (可能磁盘已满)";
            return false;
        }
    }
}

bool EncodeSessionImpl::finishSession(std::string& err)
{
    bool ok = true;
    if (oc_ && headerWritten_) {
        if (!flushEnc(err))
            ok = false;
        if (av_write_trailer(oc_) < 0 && ok) {
            err = "写入 trailer 失败 (可能磁盘已满)";
            ok = false;
        }
    }
    if (oc_ && oc_->pb) {
        if (avio_close(oc_->pb) < 0 && ok) {
            err = "关闭输出文件失败";
            ok = false;
        }
        oc_->pb = nullptr;
    }
    if (ok && !dumpOnly_ && headerWritten_) {
        int fd = open(outPath_.c_str(), O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
        if (rename(outPath_.c_str(), finalPath_.c_str()) != 0) {
            err = "重命名输出文件失败 (保留部分产物 " + outPath_ + ")";
            ok = false;
            renameFailed_ = true;
        } else {
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
        }
    }
    finished_ = ok;
    if (!ok && !dumpOnly_ && headerWritten_ && !renameFailed_)
        remove(outPath_.c_str());
    return ok;
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

void EncodeSession::cleanup()
{
    if (!impl_)
        return;
    delete static_cast<EncodeSessionImpl*>(impl_);
    impl_ = nullptr;
}

EncodeSessionImpl::~EncodeSessionImpl()
{
    if (oc_ && oc_->pb && headerWritten_ && !finished_)
        av_write_trailer(oc_);
    if (oc_ && oc_->pb) {
        avio_close(oc_->pb);
        oc_->pb = nullptr;
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
    if (!dumpOnly_ && headerWritten_ && !finished_ && !renameFailed_)
        remove(outPath_.c_str());
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
