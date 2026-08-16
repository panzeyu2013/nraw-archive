#include "archive.h"
#include "sha256.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <getopt.h>
#include <csignal>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace nraw {

namespace {

const char* g_cleanPath1 = nullptr;
const char* g_cleanPath2 = nullptr;
const char* g_cleanPath3 = nullptr;

void onSignal(int)
{
    if (g_cleanPath1)
        unlink(g_cleanPath1);
    if (g_cleanPath2)
        unlink(g_cleanPath2);
    if (g_cleanPath3)
        unlink(g_cleanPath3);
    _exit(130);
}

void installSignalHandlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

bool parseLong(const char* s, long& v)
{
    if (!s || !*s)
        return false;
    errno = 0;
    char* end = nullptr;
    long r = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    v = r;
    return true;
}

bool parseDouble(const char* s, double& v)
{
    if (!s || !*s)
        return false;
    errno = 0;
    char* end = nullptr;
    double r = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    v = r;
    return true;
}

std::string defaultOutput(const std::string& input)
{
    std::string out = input;
    size_t slash = out.rfind('/');
    size_t dot = out.rfind('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        out.erase(dot);
    out += ".h265.mov";
    return out;
}

std::string exeDir(const char* argv0)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string p(buf);
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
            return slash == 0 ? "/" : p.substr(0, slash);
    }
    if (!argv0 || !*argv0)
        return ".";
    std::string p(argv0);
    size_t slash = p.rfind('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return p.substr(0, slash);
}

std::string jsonEsc(const std::string& s)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string jsonNum(double v)
{
    if (!std::isfinite(v))
        return "null";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

const char* lensCorrectionText(int v)
{
    switch (v) {
    case 1:
        return "on";
    case 2:
        return "off";
    default:
        return "auto";
    }
}

int sidecarKeyint(const CliOptions& opt, const MediaInfo& info)
{
    if (opt.keyint > 0)
        return opt.keyint;
    int v = static_cast<int>(std::lround(
        static_cast<double>(info.fpsNum) / static_cast<double>(info.fpsDen) * 2.0));
    return v > 1 ? v : 1;
}

void printHelp()
{
    printf("用法: nraw_archive [选项] <input.NEV> [output.mov]\n");
    printf("Nikon NRAW (.NEV) -> HEVC 归档工具\n\n");
    printf("选项:\n");
    printf("  --kelvin <N>            白平衡色温 (0=原始值, 默认 0)\n");
    printf("  --tint <T>              色调偏移 (默认=原始值)\n");
    printf("  --iso <N>               ISO 感光度 (0=原始值, 默认 0)\n");
    printf("  --exposure <stops>      曝光补偿, 单位档位 (默认 as-shot)\n");
    printf("  --lens-correction <auto|on|off>  镜头畸变校正 (默认 on)\n");
    printf("  --chroma-nr <on|off>    色度降噪 (默认=原始值 as-shot)\n");
    printf("  --decode <gpu|cpu|auto> 解码路径: auto=GPU 探测+A/B 门控(默认), gpu=强制 GPU, cpu=纯 CPU\n");
    printf("  --crf <N>               HEVC 质量 (默认 14)\n");
    printf("  --preset <name>         x265 预设 (默认 slow)\n");
    printf("  --keyint <N>            关键帧间隔 (默认 0=round(fps*2))\n");
    printf("  --min-keyint <N>        最小关键帧间隔 (默认 1)\n");
    printf("  --pools <N>             x265 线程池大小 (默认 0=8)\n");
    printf("  --open-gop <0|1>        GOP 结构: 1=open(场景切点,默认), 0=closed\n");
    printf("  --buffers <N>           帧队列深度 (默认 16)\n");
    printf("  --frames <N>            仅处理前 N 帧 (默认 -1=全部)\n");
    printf("  --no-audio              不写入音频\n");
    printf("  --faststart             启用 faststart (moov 前置)\n");
    printf("  --no-sidecar            不生成 .sidecar.json\n");
    printf("  --dump-ref <file>       输出 YUV420P10LE 参考数据后退出 (测试用)\n");
    printf("  --sdk-path <dir>        包含 RED*.so 的目录 (默认程序所在目录)\n");
    printf("  --gpu-test              测试 GPU 路径: 初始化+内核编译状态+A/B 门控, 然后退出\n");
    printf("  --version               显示版本号\n");
    printf("  --help                  显示本帮助\n");
}

int parseArgs(int argc, char** argv, CliOptions& opt, bool& wantHelp, bool& wantVersion)
{
    static const struct option longOpts[] = {
        {"kelvin",          required_argument, nullptr, 'K'},
        {"tint",            required_argument, nullptr, 't'},
        {"iso",             required_argument, nullptr, 'I'},
        {"exposure",        required_argument, nullptr, 'E'},
        {"lens-correction", required_argument, nullptr, 'L'},
        {"chroma-nr",       required_argument, nullptr, 'C'},
        {"decode",          required_argument, nullptr, 'd'},
        {"crf",             required_argument, nullptr, 'q'},
        {"preset",          required_argument, nullptr, 'p'},
        {"keyint",          required_argument, nullptr, 'k'},
        {"min-keyint",      required_argument, nullptr, 'm'},
        {"pools",           required_argument, nullptr, 'o'},
        {"buffers",         required_argument, nullptr, 'b'},
        {"frames",          required_argument, nullptr, 'f'},
        {"open-gop",        required_argument, nullptr, 'g'},
        {"no-audio",        no_argument,       nullptr, 'A'},
        {"faststart",       no_argument,       nullptr, 'F'},
        {"no-sidecar",      no_argument,       nullptr, 'S'},
        {"dump-ref",        required_argument, nullptr, 'D'},
        {"sdk-path",        required_argument, nullptr, 's'},
        {"gpu-test",        no_argument,       nullptr, 'G'},
        {"version",         no_argument,       nullptr, 'V'},
        {"help",            no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", longOpts, nullptr)) != -1) {
        switch (c) {
        case 'K': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 100000) {
                fprintf(stderr, "无效的 --kelvin 值: %s\n", optarg);
                return 1;
            }
            opt.kelvin = static_cast<int>(v);
            break;
        }
        case 't': {
            double v;
            if (!parseDouble(optarg, v) || !std::isfinite(v) || v < -100.0 || v > 100.0) {
                fprintf(stderr, "无效的 --tint 值: %s\n", optarg);
                return 1;
            }
            opt.tint = static_cast<float>(v);
            break;
        }
        case 'I': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 10000000) {
                fprintf(stderr, "无效的 --iso 值: %s\n", optarg);
                return 1;
            }
            opt.iso = v;
            break;
        }
        case 'E': {
            double v;
            if (!parseDouble(optarg, v) || !std::isfinite(v) || v < -100.0 || v > 100.0) {
                fprintf(stderr, "无效的 --exposure 值: %s\n", optarg);
                return 1;
            }
            opt.exposure = static_cast<float>(v);
            break;
        }
        case 'L': {
            if (strcmp(optarg, "auto") == 0)
                opt.lensCorrection = 0;
            else if (strcmp(optarg, "on") == 0)
                opt.lensCorrection = 1;
            else if (strcmp(optarg, "off") == 0)
                opt.lensCorrection = 2;
            else {
                fprintf(stderr, "无效的 --lens-correction 值: %s (auto|on|off)\n", optarg);
                return 1;
            }
            break;
        }
        case 'C': {
            if (strcmp(optarg, "on") == 0)
                opt.chromaNr = 1;
            else if (strcmp(optarg, "off") == 0)
                opt.chromaNr = 0;
            else {
                fprintf(stderr, "无效的 --chroma-nr 值: %s (on|off)\n", optarg);
                return 1;
            }
            break;
        }
        case 'd': {
            if (strcmp(optarg, "auto") == 0)
                opt.decodeMode = 0;
            else if (strcmp(optarg, "gpu") == 0)
                opt.decodeMode = 1;
            else if (strcmp(optarg, "cpu") == 0)
                opt.decodeMode = 2;
            else {
                fprintf(stderr, "无效的 --decode 值: %s (gpu|cpu|auto)\n", optarg);
                return 1;
            }
            break;
        }
        case 'q': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 51) {
                fprintf(stderr, "无效的 --crf 值: %s\n", optarg);
                return 1;
            }
            opt.crf = static_cast<int>(v);
            break;
        }
        case 'p':
            opt.preset = optarg;
            break;
        case 'k': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 1000000) {
                fprintf(stderr, "无效的 --keyint 值: %s\n", optarg);
                return 1;
            }
            opt.keyint = static_cast<int>(v);
            break;
        }
        case 'm': {
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 1000000) {
                fprintf(stderr, "无效的 --min-keyint 值: %s\n", optarg);
                return 1;
            }
            opt.minKeyint = static_cast<int>(v);
            break;
        }
        case 'o': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 1024) {
                fprintf(stderr, "无效的 --pools 值: %s\n", optarg);
                return 1;
            }
            opt.pools = static_cast<int>(v);
            break;
        }
        case 'b': {
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 4096) {
                fprintf(stderr, "无效的 --buffers 值: %s\n", optarg);
                return 1;
            }
            opt.buffers = static_cast<int>(v);
            break;
        }
        case 'f': {
            long v;
            if (!parseLong(optarg, v) || (v == 0) || (v < -1) || v > 1000000000L) {
                fprintf(stderr, "无效的 --frames 值: %s\n", optarg);
                return 1;
            }
            opt.maxFrames = v;
            break;
        }
        case 'g': {
            long v;
            if (!parseLong(optarg, v) || (v != 0 && v != 1)) {
                fprintf(stderr, "无效的 --open-gop 值: %s (0|1)\n", optarg);
                return 1;
            }
            opt.openGop = static_cast<int>(v);
            break;
        }
        case 'A':
            opt.noAudio = true;
            break;
        case 'F':
            opt.faststart = true;
            break;
        case 'S':
            opt.noSidecar = true;
            break;
        case 'D':
            opt.dumpRef = optarg;
            break;
        case 's':
            opt.sdkPath = optarg;
            break;
        case 'G':
            opt.gpuTest = true;
            break;
        case 'h':
            wantHelp = true;
            break;
        case 'V':
            wantVersion = true;
            break;
        default:
            fprintf(stderr, "未知选项: %s\n",
                    optind > 0 ? argv[optind - 1] : (optarg ? optarg : ""));
            return 1;
        }
    }

    if (wantHelp || wantVersion)
        return 0;

    if (opt.keyint > 0 && opt.minKeyint > opt.keyint) {
        fprintf(stderr, "--min-keyint (%d) 不能大于 --keyint (%d)\n",
                opt.minKeyint, opt.keyint);
        return 1;
    }

    std::vector<std::string> pos;
    for (int i = optind; i < argc; ++i)
        pos.push_back(argv[i]);
    if (pos.empty()) {
        fprintf(stderr, "缺少输入文件\n");
        return 1;
    }
    if (pos.size() > 2) {
        fprintf(stderr, "参数过多\n");
        return 1;
    }
    opt.input = pos[0];
    if (pos.size() > 1)
        opt.output = pos[1];
    return 0;
}

}

int writeSidecar(const std::string& outPath, const CliOptions& opt,
                 const MediaInfo& info, size_t framesDone,
                 const AppliedSettings& applied, const GpuStatus& gpu,
                 const std::string& inputHash, bool ok)
{
    const std::string sidecarPath = outPath + ".sidecar.json";
    const std::string partPath = sidecarPath + ".part";
    FILE* fp = fopen(partPath.c_str(), "w");
    if (!fp) {
        fprintf(stderr, "警告: 无法写入 sidecar %s: %s\n", sidecarPath.c_str(),
                strerror(errno));
        return 1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"tool_version\": \"nraw-archive %s\",\n", kToolVersion);
    fprintf(fp, "  \"sdk_version\": \"%s\",\n", jsonEsc(sdkVersion()).c_str());
    fprintf(fp, "  \"source\": \"%s\",\n", jsonEsc(opt.input).c_str());
    fprintf(fp, "  \"sha256\": \"%s\",\n", jsonEsc(inputHash).c_str());
    fprintf(fp, "  \"width\": %zu,\n", info.width);
    fprintf(fp, "  \"height\": %zu,\n", info.height);
    fprintf(fp, "  \"frame_count\": %zu,\n", framesDone);
    fprintf(fp, "  \"clip_frame_count\": %zu,\n", info.frameCount);
    fprintf(fp, "  \"fps\": \"%zu/%zu\",\n", info.fpsNum, info.fpsDen);
    fprintf(fp, "  \"audio\": {\"channels\": %d, \"sample_rate\": %d, \"bits\": %d},\n",
            info.audio.channels, info.audio.sampleRate, info.audio.bits);
    fprintf(fp, "  \"color_space\": \"RWG/Log3G10 (native, Primary_Development_Only)\",\n");
    fprintf(fp, "  \"matrix\": \"BT.2020 full range\",\n");
    fprintf(fp, "  \"lens_correction\": \"%s\",\n", lensCorrectionText(applied.lensCorrection));
    const std::string devJson = gpu.device.empty()
                                    ? std::string("null")
                                    : ("\"" + jsonEsc(gpu.device) + "\"");
    const std::string noteJson = gpu.note.empty()
                                     ? std::string("null")
                                     : ("\"" + jsonEsc(gpu.note) + "\"");
    fprintf(fp, "  \"decode_path\": \"%s\",\n", gpu.used ? "gpu" : "cpu");
    fprintf(fp, "  \"gpu_device\": %s,\n", devJson.c_str());
    fprintf(fp, "  \"gate_psnr_db\": %s,\n",
            gpu.gated && gpu.gatePsnr > 0.0 ? jsonNum(gpu.gatePsnr).c_str() : "null");
    fprintf(fp, "  \"decode_note\": %s,\n", noteJson.c_str());
    fprintf(fp, "  \"applied\": {\"kelvin\": %s, \"tint\": %s, \"iso\": %zu, "
                "\"exposure\": %s},\n",
            jsonNum(applied.kelvin).c_str(), jsonNum(applied.tint).c_str(),
            applied.iso, jsonNum(applied.exposure).c_str());
    fprintf(fp, "  \"encode\": {\"codec\": \"hevc main10\", \"crf\": %d, \"preset\": \"%s\", "
                "\"keyint\": %d, \"min_keyint\": %d, \"pools\": %d},\n",
            opt.crf, jsonEsc(opt.preset).c_str(), sidecarKeyint(opt, info),
            opt.minKeyint > 0 ? opt.minKeyint : 1, opt.pools > 0 ? opt.pools : 8);
    fprintf(fp, "  \"success\": %s,\n", ok ? "true" : "false");
    fprintf(fp, "  \"meta\": [");
    for (size_t i = 0; i < info.meta.size(); ++i) {
        if (i > 0)
            fprintf(fp, ",");
        fprintf(fp, "\n    [\"%s\", \"%s\"]",
                jsonEsc(info.meta[i].first).c_str(),
                jsonEsc(info.meta[i].second).c_str());
    }
    fprintf(fp, "\n  ]\n");
    fprintf(fp, "}\n");
    int fe = fflush(fp);
    int ce = fclose(fp);
    if (fe != 0 || ce != 0) {
        fprintf(stderr, "警告: 无法写入 sidecar %s: %s\n", sidecarPath.c_str(),
                strerror(errno));
        remove(partPath.c_str());
        return 1;
    }
    if (rename(partPath.c_str(), sidecarPath.c_str()) != 0) {
        fprintf(stderr, "警告: 无法写入 sidecar %s: %s\n", sidecarPath.c_str(),
                strerror(errno));
        remove(partPath.c_str());
        return 1;
    }
    return 0;
}

namespace {

constexpr double kPerfectPsnrDb = 99.0;  // mse == 0 时的 PSNR 上限
constexpr double kGatePsnrDb = 55.0;     // A/B 门控最低 PSNR（dB）

std::string numStr(double v, int prec)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

double psnrRGB(const VideoFrame& a, const VideoFrame& b)
{
    if (a.width != b.width || a.height != b.height || !a.rgb.data() || !b.rgb.data())
        return 0.0;
    const uint16_t* pa = static_cast<const uint16_t*>(a.rgb.data());
    const uint16_t* pb = static_cast<const uint16_t*>(b.rgb.data());
    const size_t n = a.width * a.height;
    unsigned long long se = 0;
    for (size_t p = 0; p < 3; ++p) {
        const uint16_t* sa = pa + p * n;
        const uint16_t* sb = pb + p * n;
        for (size_t i = 0; i < n; ++i) {
            long d = static_cast<long>(sa[i]) - static_cast<long>(sb[i]);
            se += static_cast<unsigned long long>(d * d);
        }
    }
    double mse = static_cast<double>(se) / (3.0 * static_cast<double>(n));
    if (mse <= 0.0)
        return kPerfectPsnrDb;
    return 10.0 * log10(65535.0 * 65535.0 / mse);
}

struct GateResult {
    bool gated = false;
    bool pass = false;
    double psnr = 0.0;
    std::string detail;
};

GateResult runGate(GpuPipeline& gpu, const MediaInfo& info, size_t frameCount)
{
    GateResult r;
    if (frameCount == 0) {
        r.gated = true;
        r.pass = true;
        return r;
    }
    std::vector<size_t> picks;
    picks.push_back(0);
    if (frameCount > 1)
        picks.push_back(frameCount - 1);
    if (frameCount > 2)
        picks.push_back((frameCount - 1) / 2);
    std::sort(picks.begin(), picks.end());
    picks.erase(std::unique(picks.begin(), picks.end()), picks.end());

    std::string err;
    SequentialDecoder ref;
    if (!ref.open(CliOptions{}, info, err)) {
        r.detail = "CPU 参考解码器打开失败: " + err;
        return r;
    }
    double worst = 1e18;
    for (size_t n : picks) {
        VideoFrame a, b;
        if (!ref.decodeFrame(n, a, err)) {
            r.detail = "CPU 参考帧解码失败 at " + std::to_string(n) + ": " + err;
            return r;
        }
        if (!gpu.decodeSync(n, b, err)) {
            r.detail = "GPU 帧解码失败 at " + std::to_string(n) + ": " + err;
            return r;
        }
        double p = psnrRGB(a, b);
        worst = std::min(worst, p);
        char pbuf[32];
        snprintf(pbuf, sizeof(pbuf), "%.2f", p);
        r.detail += (r.detail.empty() ? "" : " ");
        r.detail += "帧" + std::to_string(n) + "=" + pbuf + "dB";
    }
    r.gated = true;
    r.pass = worst >= kGatePsnrDb;
    r.psnr = worst;
    return r;
}

// --gpu-test: 初始化 GPU 路径（含 OpenCL 内核编译）+ A/B 门控，输出状态后退出。
// 退出码: 0 = GPU 可用（初始化+门控通过）；2 = GPU 初始化失败；4 = 门控未达标/未执行
int runGpuTest(const CliOptions& opt, const MediaInfo& info)
{
    std::string err;
    GpuPipeline gpu;
    const auto t0 = std::chrono::steady_clock::now();

    printf("GPU 测试\n");
    printf("  1/3 初始化 GPU 路径（加载 OpenCL、枚举设备、REDCL 兼容性检查）...\n");
    fflush(stdout);
    if (!gpu.init(opt, info, err)) {
        printf("  ✗ GPU 不可用: %s\n", err.c_str());
        printf("GPU 测试结果: 失败（初始化失败）\n");
        return 2;
    }
    const double initSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    printf("  ✓ 初始化完成（%.1fs，含 OpenCL 内核编译）\n", initSec);
    printf("  ✓ 设备: %s\n", gpu.deviceName().c_str());
    printf("  ✓ 内核编译状态: 完成（缓存于 $XDG_CACHE_HOME|~/.cache/nraw-archive/opencl）\n");

    printf("  2/3 运行 A/B 门控（CPU 参考 vs GPU，首/中/尾帧，阈值 ≥ %.0f dB）...\n",
           kGatePsnrDb);
    fflush(stdout);
    GateResult gate = runGate(gpu, info, info.frameCount);
    if (!gate.gated) {
        printf("  ✗ 门控未执行: %s\n",
               gate.detail.empty() ? "未知原因" : gate.detail.c_str());
        gpu.close();
        printf("GPU 测试结果: 失败（门控未能执行）\n");
        return 4;
    }
    if (gate.detail.empty())
        printf("  ✓ 空剪辑，无帧可验证\n");
    else
        printf("  ✓ %s\n", gate.detail.c_str());

    printf("  3/3 结论\n");
    if (gate.pass) {
        printf("  ✓ GPU 路径可用：A/B 门控最低 PSNR %.2f dB ≥ %.0f dB\n",
               gate.psnr, kGatePsnrDb);
        gpu.close();
        printf("GPU 测试结果: 通过\n");
        return 0;
    }
    printf("  ✗ GPU 输出与 CPU 参考不一致：最低 PSNR %.2f dB < %.0f dB\n",
           gate.psnr, kGatePsnrDb);
    gpu.close();
    printf("GPU 测试结果: 失败（门控未达标）\n");
    return 4;
}

int runCpuPath(const CliOptions& opt, const MediaInfo& info, size_t frameCount)
{
    std::string err;
    SequentialDecoder dec;
    EncodeSession enc;
    AudioQueue audio(64);

    if (!dec.open(opt, info, err)) {
        fprintf(stderr, "\n解码器打开失败: %s\n", err.c_str());
        return 2;
    }
    if (!enc.open(opt, info, err)) {
        fprintf(stderr, "\n编码器打开失败: %s\n", err.c_str());
        dec.close();
        return 4;
    }

    // 音频写入上限 = min(视频实际编码帧数对应的采样数, 剪辑自身音频采样数)，
    // 防止音视频时长失配（如 --frames 截断或音频长于视频）时写入超出视频长度
    // 的音频；跨越上限的最后一个块按采样数截断。GPU 路径 encodeRun 使用同一
    // 上限，两条路径行为一致。
    unsigned long long limit =
        static_cast<unsigned long long>(frameCount) *
        static_cast<unsigned long long>(info.audio.sampleRate) *
        static_cast<unsigned long long>(info.fpsDen ? info.fpsDen : 1) /
        static_cast<unsigned long long>(info.fpsNum ? info.fpsNum : 1);
    if (limit > info.audio.samplesPerChannel)
        limit = info.audio.samplesPerChannel;
    const size_t perCh = static_cast<size_t>(info.audio.channels) * 3;
    auto flushAudio = [&]() -> bool {
        nraw::AudioPacket p;
        while (audio.tryPop(p)) {
            if (perCh == 0)
                break;
            if (p.firstSample >= limit)
                break; // 超出视频时长的音频丢弃（按序入队，正常流程不会出现）
            if (limit < p.firstSample + p.bytes.size() / perCh)
                p.bytes.resize(static_cast<size_t>(limit - p.firstSample) * perCh);
            if (!enc.writeAudio(p, err))
                return false;
        }
        return true;
    };

    bool ok = true;
    auto tStart = std::chrono::steady_clock::now();
    auto tLast = tStart;
    for (size_t frameNo = 0; frameNo < frameCount; ++frameNo) {
        unsigned long long target =
            static_cast<unsigned long long>(frameNo + 1) *
            static_cast<unsigned long long>(info.audio.sampleRate) *
            static_cast<unsigned long long>(info.fpsDen) /
            static_cast<unsigned long long>(info.fpsNum);
        if (!dec.decodeAudioWindow(target, audio, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
            break;
        }
        if (!flushAudio()) {
            fprintf(stderr, "\n处理失败: %s\n", err.c_str());
            ok = false;
            break;
        }

        nraw::VideoFrame f;
        if (!dec.decodeFrame(frameNo, f, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
            break;
        }
        if (!enc.write(f, err)) {
            fprintf(stderr, "\n处理失败: %s\n", err.c_str());
            ok = false;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - tLast >= std::chrono::seconds(2)) {
            double dt = std::chrono::duration<double>(now - tStart).count();
            double fps = dt > 0.0 ? static_cast<double>(frameNo + 1) / dt : 0.0;
            fprintf(stderr, "\r%s",
                    nraw::progressLine(frameNo + 1, frameCount, info.fpsNum,
                                       info.fpsDen, fps)
                        .c_str());
            tLast = now;
        }
    }

    if (ok) {
        if (!dec.drainAudio(audio, limit, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
        } else if (!flushAudio()) {
            fprintf(stderr, "\n处理失败: %s\n", err.c_str());
            ok = false;
        }
    }

    if (ok) {
        double dt = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - tStart)
                        .count();
        double fps = dt > 0.0 ? static_cast<double>(frameCount) / dt : 0.0;
        fprintf(stderr, "\r%s  done.\n",
                nraw::progressLine(frameCount, frameCount, info.fpsNum,
                                   info.fpsDen, fps)
                    .c_str());
        if (!enc.finish(err)) {
            fprintf(stderr, "\n处理失败: %s\n", err.c_str());
            ok = false;
        }
    }

    dec.close();
    if (!ok)
        return 4;
    return 0;
}

int runGpuPath(const CliOptions& opt, const MediaInfo& info, size_t frameCount,
               GpuPipeline& gpu)
{
    std::string err;
    SequentialDecoder dec;
    if (!dec.open(opt, info, err)) {
        fprintf(stderr, "\n解码器打开失败: %s\n", err.c_str());
        return 2;
    }

    FrameQueue frames(opt.buffers > 0 ? opt.buffers : 16);
    AudioQueue audio(64);
    std::atomic<bool> abort(false);
    std::string failDetail;
    std::thread encTh(encodeRun, opt, info, std::ref(frames), std::ref(audio),
                      std::ref(abort), std::ref(failDetail));

    if (!gpu.start(frames, &abort, err)) {
        fprintf(stderr, "\nGPU 管线启动失败: %s\n", err.c_str());
        abort.store(true);
        audio.setEof();
        frames.setEof();
        gpu.close();
        encTh.join();
        dec.close();
        return 4;
    }

    bool ok = true;
    for (size_t frameNo = 0; frameNo < frameCount; ++frameNo) {
        unsigned long long target =
            static_cast<unsigned long long>(frameNo + 1) *
            static_cast<unsigned long long>(info.audio.sampleRate) *
            static_cast<unsigned long long>(info.fpsDen) /
            static_cast<unsigned long long>(info.fpsNum);
        if (!dec.decodeAudioWindow(target, audio, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
            break;
        }
        if (abort.load()) {
            fprintf(stderr, "\n编码线程已中止，正在清理...\n");
            ok = false;
            break;
        }
        if (!gpu.submit(frameNo, err)) {
            fprintf(stderr, "\nGPU 提交失败: %s\n", err.c_str());
            ok = false;
            break;
        }
    }

    if (ok) {
        // 音频写入上限 = min(视频实际编码帧数对应的采样数, 剪辑自身音频采样数)，
        // 防止音视频时长失配时写入超出视频长度的音频
        unsigned long long limit =
            static_cast<unsigned long long>(frameCount) *
            static_cast<unsigned long long>(info.audio.sampleRate) *
            static_cast<unsigned long long>(info.fpsDen ? info.fpsDen : 1) /
            static_cast<unsigned long long>(info.fpsNum ? info.fpsNum : 1);
        if (limit > info.audio.samplesPerChannel)
            limit = info.audio.samplesPerChannel;
        if (!dec.drainAudio(audio, limit, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
        }
    }
    audio.setEof();

    if (!ok)
        abort.store(true);
    if (ok)
        ok = gpu.finish(err);
    gpu.close();
    encTh.join();
    dec.close();

    if (!ok) {
        std::string msg = err;
        if (msg.empty())
            msg = gpu.lastError();
        if (msg.empty())
            msg = failDetail;
        if (msg.empty())
            msg = "未知错误";
        fprintf(stderr, "\n处理失败: %s\n", msg.c_str());
        return 4;
    }
    if (abort.load()) {
        std::string msg = gpu.lastError();
        if (msg.empty())
            msg = failDetail;
        fprintf(stderr, "\n处理失败: %s\n", msg.c_str());
        return 4;
    }
    return 0;
}

}

}

int main(int argc, char** argv)
{
    nraw::installSignalHandlers();

    nraw::CliOptions opt;
    bool wantHelp = false;
    bool wantVersion = false;
    if (nraw::parseArgs(argc, argv, opt, wantHelp, wantVersion) != 0)
        return 1;
    if (wantVersion) {
        printf("nraw_archive %s\n", nraw::kToolVersion);
        return 0;
    }
    if (wantHelp) {
        nraw::printHelp();
        return 0;
    }
    if (opt.output.empty())
        opt.output = nraw::defaultOutput(opt.input);
    // --dump-ref 是参考帧导出（测试专用）：auto 模式下跳过 GPU 探测/A-B 门控与
    // OpenCL 内核编译，直接走 CPU 顺序路径。参考帧与解码路径无关（两路径输出
    // 一致，门控正是验证这一点），显式 --decode gpu 仍可强制 GPU 导出。
    if (!opt.dumpRef.empty() && opt.decodeMode == 0)
        opt.decodeMode = 2;
    if (opt.sdkPath.empty())
        opt.sdkPath = nraw::exeDir(argv[0]);

    static std::string partPath = opt.output + ".part";
    static std::string sidecarPartPath = opt.output + ".sidecar.json.part";
    nraw::g_cleanPath1 = partPath.c_str();
    nraw::g_cleanPath2 = opt.dumpRef.empty() ? nullptr : opt.dumpRef.c_str();
    nraw::g_cleanPath3 = (!opt.noSidecar && opt.dumpRef.empty())
                             ? sidecarPartPath.c_str()
                             : nullptr;

    {
        auto sameFile = [](const std::string& a, const std::string& b) -> bool {
            char* ra = realpath(a.c_str(), nullptr);
            char* rb = ra ? realpath(b.c_str(), nullptr) : nullptr;
            bool same = ra && rb && strcmp(ra, rb) == 0;
            if (!same && ra)
                same = (b == a);
            if (!same && ra && rb) {
                struct stat sa, sb;
                if (stat(ra, &sa) == 0 && stat(rb, &sb) == 0 &&
                    sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino)
                    same = true;
            }
            free(ra);
            free(rb);
            return same;
        };
        if (sameFile(opt.input, opt.output)) {
            fprintf(stderr, "输入与输出不能是同一文件: %s\n", opt.input.c_str());
            return 1;
        }
        if (!opt.dumpRef.empty() && sameFile(opt.input, opt.dumpRef)) {
            fprintf(stderr, "输入与参考输出不能是同一文件: %s\n", opt.input.c_str());
            return 1;
        }
        if (!opt.dumpRef.empty() && sameFile(opt.output, opt.dumpRef)) {
            fprintf(stderr, "输出与参考输出不能是同一文件: %s\n", opt.output.c_str());
            return 1;
        }
    }

    std::string err;
    if (!nraw::initSdk(opt.sdkPath, err, opt.decodeMode == 1)) {
        fprintf(stderr, "initSdk 失败: %s\n", err.c_str());
        return 2;
    }

    nraw::MediaInfo info;
    if (!nraw::openMedia(opt.input, opt, info, err)) {
        fprintf(stderr, "打开媒体失败: %s\n", err.c_str());
        nraw::shutdownSdk();
        return 2;
    }

    std::string inputHash;
    std::thread hashTh;
    const bool needHash = !opt.noSidecar && opt.dumpRef.empty() && !opt.gpuTest;
    if (needHash)
        hashTh = std::thread([&] {
            inputHash = nraw::sha256File(opt.input);
            if (inputHash.empty())
                fprintf(stderr, "警告: 无法计算源文件 sha256（读取失败），"
                                 "sidecar 将记录空哈希\n");
        });

    printf("输入: %s\n输出: %s\n", opt.input.c_str(), opt.output.c_str());
    printf("SDK 版本: %s\n", nraw::sdkVersion().c_str());
    printf("分辨率: %zux%zu  帧数: %zu  帧率: %zu/%zu\n",
           info.width, info.height, info.frameCount, info.fpsNum, info.fpsDen);
    if (info.audio.present)
        printf("音频: %d 通道, %d Hz, %d-bit\n",
               info.audio.channels, info.audio.sampleRate, info.audio.bits);
    else
        printf("音频: 无\n");

    // --gpu-test: 只做 GPU 初始化 + 内核编译 + A/B 门控测试，不进行编码
    if (opt.gpuTest) {
        int rc = nraw::runGpuTest(opt, info);
        nraw::closeMedia();
        nraw::shutdownSdk();
        return rc;
    }

    size_t frameCount = info.frameCount;
    if (opt.maxFrames >= 0 && static_cast<size_t>(opt.maxFrames) < frameCount)
        frameCount = static_cast<size_t>(opt.maxFrames);

    nraw::GpuStatus gpu;
    nraw::GpuPipeline gpuPipe;
    bool useGpu = false;
    const bool forcedGpu = opt.decodeMode == 1;
    if (opt.decodeMode != 2) {
        if (!gpuPipe.init(opt, info, err)) {
            if (forcedGpu) {
                fprintf(stderr, "GPU 路径初始化失败: %s\n", err.c_str());
                gpuPipe.close();
                if (needHash)
                    hashTh.join();
                nraw::closeMedia();
                nraw::shutdownSdk();
                return 2;
            }
            gpu.note = "GPU 不可用（" + err + "），回退 CPU";
            printf("解码路径: CPU（%s）\n", err.c_str());
        } else {
            nraw::GateResult gate = nraw::runGate(gpuPipe, info, frameCount);
            gpu.device = gpuPipe.deviceName();
            gpu.gated = gate.gated;
            gpu.gatePsnr = gate.psnr;
            if (!gate.gated) {
                if (gate.detail.empty())
                    gate.detail = "门控未执行";
                if (forcedGpu) {
                    useGpu = true;
                    gpu.used = true;
                    gpu.note = "--decode gpu 强制；门控未能执行（" + gate.detail + "）";
                    printf("解码路径: GPU（--decode gpu 强制；门控未能执行: %s）\n",
                           gate.detail.c_str());
                } else {
                    gpu.note = "GPU 门控失败（" + gate.detail + "），回退 CPU";
                    printf("解码路径: CPU（%s）\n", gate.detail.c_str());
                }
            } else if (gate.pass || forcedGpu) {
                useGpu = true;
                gpu.used = true;
                if (forcedGpu && !gate.pass)
                    gpu.note = "--decode gpu 强制；A/B 门控 PSNR " + nraw::numStr(gate.psnr, 2) +
                               " dB < " + nraw::numStr(nraw::kGatePsnrDb, 0) + " 仍继续 GPU";
                else if (gate.psnr > 0.0)
                    gpu.note = "A/B 门控 PSNR " + nraw::numStr(gate.psnr, 2) + " dB ≥ " +
                               nraw::numStr(nraw::kGatePsnrDb, 0);
                else
                    gpu.note = "A/B 门控通过（空剪辑，无帧可验证）";
                printf("解码路径: GPU（AsyncDecoder + REDOpenCL，A/B 门控最低 PSNR %.2f dB%s）\n",
                       gate.psnr, forcedGpu && !gate.pass ? "，未达标但强制" : "");
            } else {
                gpu.note = "GPU A/B 门控 PSNR " + nraw::numStr(gate.psnr, 2) + " dB < " +
                           nraw::numStr(nraw::kGatePsnrDb, 0) + "，回退 CPU";
                printf("解码路径: CPU（A/B 门控 PSNR %.2f dB < %.0f dB，GPU 结果与 CPU 参考不一致）\n",
                       gate.psnr, nraw::kGatePsnrDb);
            }
        }
    } else {
        gpu.note = "--decode cpu 显式指定";
        printf("解码路径: CPU（--decode cpu）\n");
    }

    int rc = 0;
    if (useGpu)
        rc = nraw::runGpuPath(opt, info, frameCount, gpuPipe);
    else
        rc = nraw::runCpuPath(opt, info, frameCount);
    gpuPipe.close();
    if (needHash)
        hashTh.join();

    if (rc != 0) {
        if (!opt.dumpRef.empty())
            remove(opt.dumpRef.c_str());
        nraw::closeMedia();
        nraw::shutdownSdk();
        return rc;
    }

    if (!opt.noSidecar && opt.dumpRef.empty()) {
        nraw::AppliedSettings applied;
        nraw::appliedSettings(applied);
        if (nraw::writeSidecar(opt.output, opt, info, frameCount, applied, gpu,
                               inputHash, true) != 0) {
            fprintf(stderr, "\n错误: sidecar 写入失败\n");
            nraw::closeMedia();
            nraw::shutdownSdk();
            return 5;
        }
    } else if (opt.dumpRef.empty() && opt.noSidecar) {
        printf("未生成 sidecar (--no-sidecar)\n");
    }

    nraw::closeMedia();
    nraw::shutdownSdk();
    return 0;
}
