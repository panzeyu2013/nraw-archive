// 泄漏探针：用最简代码复现 SDK DecodeVideoFrame 的逐帧内存增长，
// 并对像素格式 / 镜头校正 / 音频块解码做 A/B 对照，定位泄漏条件。
// 用法: nraw_leak_probe <input.R3D> <startFrame> <count> <planar|interleaved> <lens=on|off> [audio]
// 输出: 每 10 帧打印一行 "frame=<n> VmRSS=<kB> VmHWM=<kB>"
// 用途：R3D SDK 逐帧内存泄漏 A/B 定位探针（Linux-only，手动运行，不参与测试套件）。
// 用法: nraw_leak_probe <input.R3D> <startFrame> <count> <planar|interleaved> <lens=on|off|default> [audio|compare] [stride]
// 输出: 每 10 帧打印一行 "frame=<n> VmRSS=<kB> VmHWM=<kB>"；compare 模式逐像素比对两种像素格式。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#if defined(__linux__)
#include <malloc.h>
#endif

#include "R3DSDK.h"

static long vmRssKb();
static void printSmapsSummary(const char* tag)
{
    FILE* f = fopen("/proc/self/smaps_rollup", "r");
    if (!f)
        return;
    char line[256];
    long rss = -1, anon = -1, privDirty = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Rss: %ld", &rss) == 1)
            continue;
        if (sscanf(line, "Anonymous: %ld", &anon) == 1)
            continue;
        if (sscanf(line, "Private_Dirty: %ld", &privDirty) == 1)
            continue;
    }
    fclose(f);
    printf("%s: Rss=%ldkB Anonymous=%ldkB Private_Dirty=%ldkB VmRSS=%ldkB\n",
           tag, rss, anon, privDirty, vmRssKb());
}

static long vmRssKb()
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld", &rss) == 1)
            break;
    }
    fclose(f);
    return rss;
}

static long vmHwmKb()
{
    FILE* f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long hwm = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmHWM: %ld", &hwm) == 1)
            break;
    }
    fclose(f);
    return hwm;
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "用法: %s <input.R3D> <startFrame> <count> <planar|interleaved> "
                "<lens=on|off> [audio]\n",
                argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const size_t start = static_cast<size_t>(strtoull(argv[2], nullptr, 10));
    const size_t count = static_cast<size_t>(strtoull(argv[3], nullptr, 10));
    const bool planar = strcmp(argv[4], "planar") == 0;
    const bool lensOn = strcmp(argv[5], "on") == 0;
    const bool audio = argc > 6 && strcmp(argv[6], "audio") == 0;
    const bool compare = argc > 6 && strcmp(argv[6], "compare") == 0;
    const size_t stride = argc > 7 ? static_cast<size_t>(strtoull(argv[7], nullptr, 10)) : 1;

    R3DSDK::InitializeStatus st = R3DSDK::InitializeSdk(".", OPTION_RED_NONE);
    if (st != R3DSDK::ISInitializeOK) {
        fprintf(stderr, "InitializeSdk failed: %d\n", static_cast<int>(st));
        R3DSDK::FinalizeSdk();  // SDK 契约：调用过 InitializeSdk 必须配对 FinalizeSdk
        return 1;
    }

    R3DSDK::Clip* clip = new R3DSDK::Clip(path.c_str());
    if (clip->Status() != R3DSDK::LSClipLoaded) {
        fprintf(stderr, "clip load failed\n");
        return 1;
    }
    R3DSDK::ImageProcessingSettings* ip = new R3DSDK::ImageProcessingSettings();
    clip->GetDefaultImageProcessingSettings(*ip);
    const int defaultLens = static_cast<int>(ip->LensDistortionCorrection);
    fprintf(stderr, "clip default LensDistortionCorrection=%d\n", defaultLens);
    if (strcmp(argv[5], "off") == 0) {
        ip->LensDistortionCorrection = R3DSDK::LensCorrectionOff;
    } else if (strcmp(argv[5], "on") == 0) {
        ip->LensDistortionCorrection = R3DSDK::LensCorrectionOn;
    } else {
        // default：保持剪辑默认
    }
    ip->ImagePipelineMode = R3DSDK::Full_Graded;
    ip->ColorSpace = R3DSDK::ImageColorREDWideGamutRGB;
    ip->GammaCurve = R3DSDK::ImageGammaLog3G10;
    ip->CheckBounds();

    const size_t w = clip->Width();
    const size_t h = clip->Height();
    fprintf(stderr, "clip %zux%zu frames=%zu audio=%s\n", w, h,
            clip->VideoFrameCount(),
            clip->AudioChannelCount() > 0 ? "yes" : "no");

    if (audio) {
        size_t maxBytes = 0;
        const size_t blocks = clip->AudioBlockCountAndSize(&maxBytes);
        fprintf(stderr, "audio blocks=%zu maxBytes=%zu\n", blocks, maxBytes);
        void* buf = nullptr;
        if (maxBytes > 0 && posix_memalign(&buf, 512, maxBytes) != 0)
            return 1;
        printf("probe audio: start block 0 count %zu\n", count);
        for (size_t i = 0; i < count && i < blocks; ++i) {
            size_t sz = maxBytes;
            if (clip->DecodeAudioBlock(i, buf, &sz) != R3DSDK::DSDecodeOK) {
                fprintf(stderr, "audio decode failed at %zu\n", i);
                return 1;
            }
            if (i % 10 == 9 || i == count - 1)
                printf("audio block=%zu VmRSS=%ldkB VmHWM=%ldkB\n", i + 1,
                       vmRssKb(), vmHwmKb());
        }
        free(buf);
        delete ip;
        delete clip;
        R3DSDK::FinalizeSdk();
        return 0;
    }

    const size_t need = w * h * 6;
    void* buf = nullptr;
    if (posix_memalign(&buf, 16, need) != 0) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

    // compare 模式：同一帧分别用 planar / interleaved 解码，
    // 校验两布局像素值一致（允许 0 误差），并解码两次验证确定性
    if (compare) {
        void* buf2 = nullptr;
        if (posix_memalign(&buf2, 16, need) != 0)
            return 1;
        R3DSDK::VideoDecodeJob jp, ji;
        jp.Mode = ji.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
        jp.PixelType = R3DSDK::PixelType_16Bit_RGB_Planar;
        ji.PixelType = R3DSDK::PixelType_16Bit_RGB_Interleaved;
        jp.OutputBuffer = buf;
        jp.OutputBufferSize = need;
        ji.OutputBuffer = buf2;
        ji.OutputBufferSize = need;
        jp.ImageProcessing = ji.ImageProcessing = ip;
        jp.HdrProcessing = ji.HdrProcessing = nullptr;
        jp.OutputFrameMetadata = ji.OutputFrameMetadata = nullptr;
        const size_t picks[] = {0,     1,      2,      3,      7,      31,
                                100,   999,    5000,   10000,  20000,  30000,
                                43210, 50000,  60000,  72000,  80000,  90000,
                                100000, 123456, 140000, 160000, 180000, 200000,
                                220000, 240000, 260000, 270000, 280000, 287984};
        for (size_t fn : picks) {
            if (fn >= clip->VideoFrameCount())
                continue;
            if (clip->DecodeVideoFrame(fn, jp) != R3DSDK::DSDecodeOK ||
                clip->DecodeVideoFrame(fn, ji) != R3DSDK::DSDecodeOK) {
                fprintf(stderr, "decode failed at %zu\n", fn);
                return 1;
            }
            const uint16_t* p = static_cast<const uint16_t*>(buf);
            const uint16_t* q = static_cast<const uint16_t*>(buf2);
            size_t bad = 0;
            unsigned long long maxd = 0;
            const size_t n = w * h;
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 3; ++c) {
                    const long d = static_cast<long>(p[c * n + i]) -
                                   static_cast<long>(q[i * 3 + c]);
                    if (d != 0) {
                        ++bad;
                        if (static_cast<unsigned long long>(llabs(d)) > maxd)
                            maxd = static_cast<unsigned long long>(llabs(d));
                    }
                }
            }
            printf("frame %zu: mismatched=%zu/%zu maxdiff=%llu\n", fn, bad,
                   3 * n, maxd);
        }
        free(buf2);
        free(buf);
        delete ip;
        delete clip;
        R3DSDK::FinalizeSdk();
        return 0;
    }

    R3DSDK::VideoDecodeJob job;
    job.Mode = R3DSDK::DECODE_FULL_RES_PREMIUM;
    job.PixelType = planar ? R3DSDK::PixelType_16Bit_RGB_Planar
                           : R3DSDK::PixelType_16Bit_RGB_Interleaved;
    job.OutputBuffer = buf;
    job.OutputBufferSize = need;
    job.ImageProcessing = ip;
    job.HdrProcessing = nullptr;
    job.OutputFrameMetadata = nullptr;

    printf("probe: start=%zu count=%zu pixel=%s lens=%s stride=%zu\n", start, count,
           planar ? "planar" : "interleaved", lensOn ? "on" : "off", stride);
    printf("frame=%zu VmRSS=%ldkB VmHWM=%ldkB (before)\n", start, vmRssKb(),
           vmHwmKb());
    for (size_t i = 0; i < count; ++i) {
        const size_t fn = start + i * stride;
        if (clip->DecodeVideoFrame(fn, job) != R3DSDK::DSDecodeOK) {
            fprintf(stderr, "decode failed at frame %zu\n", fn);
            return 1;
        }
        if (i % 10 == 9 || i == count - 1)
            printf("frame=%zu VmRSS=%ldkB VmHWM=%ldkB\n", fn, vmRssKb(),
                   vmHwmKb());
    }

    printSmapsSummary("after_decode");
    free(buf);
    printSmapsSummary("after_free_output");
    delete ip;
    printSmapsSummary("after_delete_ip");
    delete clip;
    printSmapsSummary("after_delete_clip");
    R3DSDK::FinalizeSdk();
    printSmapsSummary("after_finalize");
#if defined(__linux__)
    malloc_trim(0);
#endif
    printSmapsSummary("after_trim");
    return 0;
}
