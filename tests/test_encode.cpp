#include "archive.h"
#include "sha256.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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

static void producer(const CliOptions& opt, MediaInfo& info,
                     FrameQueue& frames, AudioQueue& audio)
{
    const bool audioOn = !opt.noAudio && opt.dumpRef.empty() && info.audio.present;
    size_t blockIdx = 0;
    unsigned long long decodedSamples = 0;
    size_t totalBlocks = (info.audio.samplesPerChannel + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES;

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

    for (size_t no = 0; no < FRAMES; ++no) {
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
    // 文件哈希路径
    const char* path = "/tmp/nraw_sha256_test.bin";
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
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

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s repack | sha256 [file] | dump <ref.yuv> | encode <out.mov>\n",
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
        fprintf(stderr, "用法: %s repack | sha256 [file] | dump <ref.yuv> | encode <out.mov>\n",
                argv[0]);
        return 2;
    }
    if (mode == "dump") {
        opt.dumpRef = argv[2];
    } else if (mode == "encode") {
        opt.output = argv[2];
    } else {
        fprintf(stderr, "未知模式: %s\n", mode.c_str());
        return 2;
    }

    FrameQueue frames(16);
    AudioQueue audio(128);
    std::atomic<bool> abort(false);
    std::string failMsg, encFail;

    std::thread prod(producer, std::ref(opt), std::ref(info),
                     std::ref(frames), std::ref(audio));
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
