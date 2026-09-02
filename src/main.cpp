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

// 信号处理用的 worker PID 槽（定义在 sdk_decode.cpp 的 nraw namespace）：
// 主线程 spawn/close 时写入，信号处理器只读——固定数组 + sig_atomic_t
// 计数保证 async-signal-safe（无锁、无分配）。声明在匿名 namespace 外，
// onSignal 用 nraw:: 全限定引用。
extern volatile sig_atomic_t g_workerPidSlots[256];
extern volatile sig_atomic_t g_workerPidCount;
constexpr sig_atomic_t kMaxWorkerSlots = 256;

namespace {

// 中断时保留输出部分产物（.part/.samples/.ckpt）供下次运行自动续传，
// 只清理 sidecar 临时文件与测试参考文件
const char* g_cleanPath2 = nullptr;
const char* g_cleanPath3 = nullptr;

void onSignal(int)
{
    // 输出部分产物不删除：它们是断点续传的资产
    if (g_cleanPath2)
        unlink(g_cleanPath2);
    if (g_cleanPath3)
        unlink(g_cleanPath3);
    // 主动终止 worker 子进程：_exit 不跑析构，worker 会变孤儿（仅靠写管道
    // EPIPE 退出，延迟 ~1s+）；反复中断叠加会滞留内存。SIGKILL 直接回收。
    // 只读固定数组 + sig_atomic_t 计数（async-signal-safe，无锁）。
    // pid <= 0 的槽位跳过：kill(0, SIGKILL) 会杀伤整个进程组
    // （nohup/脚本链场景下会连带杀死无关进程）。
    const volatile sig_atomic_t n = nraw::g_workerPidCount;
    for (volatile sig_atomic_t i = 0; i < n && i < kMaxWorkerSlots; ++i) {
        const pid_t p = static_cast<pid_t>(nraw::g_workerPidSlots[i]);
        if (p > 0)
            kill(p, SIGKILL);
    }
    const char msg[] =
        "\n已中断：保留部分产物（.part/.samples/.ckpt），下次运行将自动续传\n";
    const ssize_t wr = write(2, msg, sizeof(msg) - 1);
    (void)wr;
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
    std::string p = nraw::selfExePath();
    if (!p.empty()) {
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
            return slash == 0 ? "/" : p.substr(0, slash);
    }
    if (!argv0 || !*argv0)
        return ".";
    p = argv0;
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
    printf("  --decode <gpu|cpu|auto> 解码路径: auto=GPU 探测+A/B 门控(默认), gpu=强制 GPU, cpu=纯 CPU(多进程并行)\n");
    printf("  --crf <N>               HEVC 质量 (默认 14)\n");
    printf("  --preset <name>         x265 预设 (默认 slow)\n");
    printf("  --keyint <N>            关键帧间隔 (默认 0=round(fps*2))\n");
    printf("  --min-keyint <N>        最小关键帧间隔 (默认 1)\n");
    printf("  --pools <N>             x265 编码线程池大小 (默认 0=auto，由 --jobs 统一分配)\n");
    printf("  --cpu-workers <N>       CPU 解码 worker 进程数 (默认 0=auto，每个约 ~1fps，8 个≈8fps)\n");
    printf("  --worker-batch <N>     worker 代际回收批次大小 (默认 1000 帧/代；R3D SDK 解码存在无法在进程内回收的内存积累，worker 每解码 N 帧后干净退出并由父进程重启下一批，内存随进程退出归还)\n");
    printf("  --jobs <N>              CPU 总线程预算 (默认 0=auto=核心数)；自动拆分 worker 数与 x265 pools\n");
    printf("  --open-gop <0|1>        GOP 结构: 1=open(场景切点,默认), 0=closed\n");
    printf("  --buffers <N>           帧队列深度 (默认 16)\n");
    printf("  --frames <N>            仅处理前 N 帧 (默认 -1=全部)\n");
    printf("  --no-audio              不写入音频\n");
    printf("  --faststart             输出 moov 前置的 MOV (收尾时重写)；默认 moov 在文件尾\n");
    printf("  (任何阶段失败都会保留 .part 部分产物，错误信息含路径、errno 与抢救提示；\n");
    printf("   下次运行自动检测到 .part+检查点即自动续传（源文件与编码参数经\n");
    printf("   SHA-256 校验一致后复用已编码数据），closed-GOP 关键帧对齐、\n");
    printf("   open-GOP 回退 17 帧继续，已编码部分不重新编码)\n");
    printf("  --no-sidecar            不生成 sidecar_<输出名>.json\n");
    printf("  --dump-ref <file>       输出 YUV420P10LE 参考数据后退出 (测试用)\n");
    printf("  --sdk-path <dir>        包含 RED*.so 的目录 (默认程序所在目录)\n");
    printf("  --gpu-test              测试 GPU 路径后退出; 无输入文件时仅测初始化+内核编译\n");
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
        {"cpu-workers",     required_argument, nullptr, 'J'},
        {"jobs",            required_argument, nullptr, 'j'},
        {"buffers",         required_argument, nullptr, 'b'},
        {"frames",          required_argument, nullptr, 'f'},
        {"open-gop",        required_argument, nullptr, 'g'},
        {"no-audio",        no_argument,       nullptr, 'A'},
        {"faststart",       no_argument,       nullptr, 'F'},
        {"no-sidecar",      no_argument,       nullptr, 'S'},
        {"dump-ref",        required_argument, nullptr, 'D'},
        {"sdk-path",        required_argument, nullptr, 's'},
        {"gpu-test",        no_argument,       nullptr, 'G'},
        {"decode-worker",   required_argument, nullptr, 'W'},
        {"worker-count",    required_argument, nullptr, 'Y'},
        {"worker-frames",   required_argument, nullptr, 'Z'},
        {"worker-start",    required_argument, nullptr, 'R'},
        {"worker-batch",    required_argument, nullptr, 'B'},
        {"test-stop-after", required_argument, nullptr, 'T'},
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
        case 'J': {
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 256) {
                fprintf(stderr, "无效的 --cpu-workers 值: %s\n", optarg);
                return 1;
            }
            opt.cpuWorkers = static_cast<int>(v);
            break;
        }
        case 'j': {
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 4096) {
                fprintf(stderr, "无效的 --jobs 值: %s\n", optarg);
                return 1;
            }
            opt.jobs = static_cast<int>(v);
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
        case 'W': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 100000) {
                fprintf(stderr, "无效的 --decode-worker 值: %s\n", optarg);
                return 1;
            }
            opt.decodeWorker = true;
            opt.workerId = v;
            break;
        }
        case 'Y': {
            if (!opt.decodeWorker) {
                fprintf(stderr,
                        "错误: --worker-count 是内部参数，仅用于 --decode-worker 子进程\n");
                return 1;
            }
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 256) {
                fprintf(stderr, "无效的 --worker-count 值: %s\n", optarg);
                return 1;
            }
            opt.workerCount = v;
            break;
        }
        case 'Z': {
            if (!opt.decodeWorker) {
                fprintf(stderr,
                        "错误: --worker-frames 是内部参数，仅用于 --decode-worker 子进程\n");
                return 1;
            }
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 1000000000L) {
                fprintf(stderr, "无效的 --worker-frames 值: %s\n", optarg);
                return 1;
            }
            opt.workerFrames = v;
            break;
        }
        case 'R': {
            if (!opt.decodeWorker) {
                fprintf(stderr,
                        "错误: --worker-start 是内部参数，仅用于 --decode-worker 子进程\n");
                return 1;
            }
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 1000000000L) {
                fprintf(stderr, "无效的 --worker-start 值: %s\n", optarg);
                return 1;
            }
            opt.workerStart = v;
            break;
        }
        case 'B': {
            long v;
            if (!parseLong(optarg, v) || v < 1 || v > 1000000000L) {
                fprintf(stderr, "无效的 --worker-batch 值: %s\n", optarg);
                return 1;
            }
            opt.workerBatch = v;
            break;
        }
        case 'T': {
            long v;
            if (!parseLong(optarg, v) || v < 0 || v > 1000000000L) {
                fprintf(stderr, "无效的 --test-stop-after 值: %s\n", optarg);
                return 1;
            }
            opt.testStopAfter = v;
            break;
        }
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
    if (pos.empty() && !opt.gpuTest) {
        fprintf(stderr, "缺少输入文件\n");
        return 1;
    }
    if (pos.size() > 2) {
        fprintf(stderr, "参数过多\n");
        return 1;
    }
    if (!pos.empty())
        opt.input = pos[0];
    if (pos.size() > 1)
        opt.output = pos[1];
    return 0;
}

}

// sidecar 路径：sidecar_<输出基名>.json
// 输出 /path/DSC_1775.mov → /path/sidecar_DSC_1775.json
// 输出 /path/out（无扩展）→ /path/sidecar_out.json
std::string sidecarPathFor(const std::string& outPath)
{
    const size_t slash = outPath.find_last_of('/');
    const size_t dot = outPath.find_last_of('.');
    // 仅当扩展名点位于最后一个路径分隔符之后才剥离（如 .mov/.mp4）
    const bool hasExt = dot != std::string::npos &&
                        (slash == std::string::npos || dot > slash);
    const std::string dir = slash == std::string::npos
                                ? std::string()
                                : outPath.substr(0, slash + 1);
    const std::string name = outPath.substr(
        slash == std::string::npos ? 0 : slash + 1,
        (hasExt ? dot : outPath.size()) -
            (slash == std::string::npos ? 0 : slash + 1));
    return dir + "sidecar_" + name + ".json";
}

int writeSidecar(const std::string& outPath, const CliOptions& opt,
                 const MediaInfo& info, size_t framesDone,
                 const AppliedSettings& applied, const GpuStatus& gpu,
                 const std::string& inputHash, bool ok)
{
    // sidecar 命名规范：sidecar_<输出基名>.json（如 DSC_1775.mov →
    // sidecar_DSC_1775.json），与输出文件同目录、不再带 .mov 扩展
    const std::string sidecarPath = sidecarPathFor(outPath);
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
    CpuAsyncDecoder dec;
    EncodeSession enc;
    AudioQueue audio(64);

    if (!dec.open(opt, info, frameCount, err)) {
        fprintf(stderr, "\n解码器打开失败: %s\n", err.c_str());
        return 2;
    }
    if (!enc.open(opt, info, err)) {
        fprintf(stderr, "\n编码器打开失败: %s\n", err.c_str());
        dec.close();
        return 4;
    }
    // 会话打开后立即写初始检查点（与 GPU 路径 encodeRun 一致）；
    // waitHash=false 不阻塞编码（sha 可能为空，detectResume 跳过校验）
    {
        std::string cerr_;
        nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                              *nraw::g_sampleLog, cerr_, false);
    }

    // 续传：重放旧 .part 已编码样本 + 音频跳到续传点
    const size_t startFrame = opt.resumeMode && opt.resumeFrame > 0
                                  ? static_cast<size_t>(opt.resumeFrame)
                                  : 0;
    if (startFrame > 0) {
        if (!enc.replayOldSamples(opt.output + ".part.old", err)) {
            fprintf(stderr, "\n续传重放失败: %s\n", err.c_str());
            dec.close();
            return 4;
        }
        printf("续传: 已复用 %zu 帧已编码数据，从第 %zu 帧继续\n",
               enc.replayedFrames(), startFrame);
        if (!dec.seekAudioTo(opt.resumeAudioSample, err)) {
            fprintf(stderr, "\n续传音频跳转失败: %s\n", err.c_str());
            dec.close();
            return 4;
        }
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

    // 多进程 CPU 解码：worker 子进程自主解码并通过管道流式回传，
    // waitFrame 按帧号排序取帧，解码与 x265 编码流水线重叠。
    bool ok = true;
    size_t flushCounter = 0;
    size_t ckCounter = 0;
    auto tStart = std::chrono::steady_clock::now();
    auto tLast = tStart;
    for (size_t done = startFrame; done < frameCount; ++done) {
        unsigned long long target =
            static_cast<unsigned long long>(done + 1) *
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
        if (!dec.waitFrame(done, f, err)) {
            fprintf(stderr, "\n解码失败: %s\n", err.c_str());
            ok = false;
            break;
        }
        if (!enc.write(f, err)) {
            fprintf(stderr, "\n处理失败: %s\n", err.c_str());
            ok = false;
            break;
        }
        // 样本日志每 20 帧 flushSamples（SIGINT 时 stdio 缓冲会丢）；检查点 500 帧
        if (++flushCounter >= 20) {
            flushCounter = 0;
            enc.flushSamples();
        }
        if (++ckCounter >= 500) {
            ckCounter = 0;
            std::string cerr_;
            // waitHash=false：不阻塞编码等哈希（收尾 sidecar 记录完整 sha）
            nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                                  *nraw::g_sampleLog, cerr_, false);
        }
        if (opt.testStopAfter >= 0 &&
            done + 1 - startFrame >= static_cast<size_t>(opt.testStopAfter)) {
            // 测试钩子：模拟中断（保留 .part/.samples/.ckpt 供续传测试）。
            // 与 encodeRun 一致：先 flushEnc 冲刷编码器挂起帧（lookahead
            // 中的帧包此刻才产出），否则中断产物缺挂起帧、续传点检测不到
            // 新关键帧
            std::string ferr;
            enc.flushEnc(ferr);
            enc.flushSamples();
            std::string cerr_;
            nraw::writeCheckpoint(opt.output + ".ckpt", opt, info,
                                  *nraw::g_sampleLog, cerr_, false);
            fprintf(stderr, "\n[测试] 模拟中断（保留部分产物供续传测试）\n");
            ok = false;
            break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - tLast >= std::chrono::seconds(2)) {
            double dt = std::chrono::duration<double>(now - tStart).count();
            // 续传时 done 是绝对帧号（含已复用旧数据），必须减去 startFrame
            // 才是本会话实际编码的帧数，否则 fps/ETA 被旧数据虚高
            double fps = dt > 0.0
                             ? static_cast<double>(done + 1 - startFrame) / dt
                             : 0.0;
            fprintf(stderr, "\r%s",
                    nraw::progressLine(done + 1, frameCount, info.fpsNum,
                                       info.fpsDen, fps)
                        .c_str());
            tLast = now;
        }
    }

    if (ok) {
        // 防御性收尾：逐帧循环的最后一个 target 已把解码推进到 limit（或
        // 剪辑音频末尾），正常路径此处立即返回；仅当未来逐帧解码窗口改变、
        // 解码器未追平时，此循环才真正补解码（解一块写一块，队列 ≤1 块，
        // 避免 drainAudio 一次生成大量块在单线程下因队列满而自死锁）
        for (;;) {
            std::string aerr;
            const bool more = dec.decodeOneAudioBlock(audio, limit, aerr);
            if (!aerr.empty()) {
                fprintf(stderr, "\n解码失败: %s\n", aerr.c_str());
                ok = false;
                break;
            }
            if (!flushAudio()) {
                fprintf(stderr, "\n处理失败: %s\n", err.c_str());
                ok = false;
                break;
            }
            if (!more)
                break;  // 解码完成
        }
    }

    if (ok) {
        double dt = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - tStart)
                        .count();
        // 与循环内一致：减去续传起点，fps 只统计本会话编码的帧数
        double fps = dt > 0.0
                         ? static_cast<double>(frameCount - startFrame) / dt
                         : 0.0;
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

    // 续传：音频跳到续传点（视频由 gpu.submit(frameNo) 随机访问解码）
    const size_t startFrame = opt.resumeMode && opt.resumeFrame > 0
                                  ? static_cast<size_t>(opt.resumeFrame)
                                  : 0;
    if (startFrame > 0) {
        if (!dec.seekAudioTo(opt.resumeAudioSample, err)) {
            fprintf(stderr, "\n续传音频跳转失败: %s\n", err.c_str());
            abort.store(true);
            audio.setEof();
            frames.setEof();
            gpu.close();
            encTh.join();
            dec.close();
            return 4;
        }
    }

    if (!gpu.start(frames, &abort,
                    opt.resumeFrame > 0 ? static_cast<size_t>(opt.resumeFrame) : 0,
                    err)) {
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
    for (size_t frameNo = startFrame; frameNo < frameCount; ++frameNo) {
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
        // 检查点由 encodeRun 编码线程独占写入（500 帧周期），此处不写，
        // 避免与 encTh 并发迭代 g_sampleLog（vector 扩容数据竞争）
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

    // 样本日志：编码会话写入，供收尾失败时自动重建 moov
    nraw::SampleLog sampleLog;
    nraw::g_sampleLog = &sampleLog;

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

    // 多进程 CPU 解码的 worker 子进程入口：独立完成解码并把帧写入 fd 3，
    // 不参与父进程的其余流程（sidecar/门控/编码等）
    if (opt.decodeWorker)
        return nraw::runDecodeWorker(opt);

    static std::string sidecarPartPath = nraw::sidecarPathFor(opt.output) + ".part";
    static std::string dumpRefPath = opt.dumpRef;  // 拷贝进 static，防 c_str 悬垂
    nraw::g_cleanPath2 = opt.dumpRef.empty() ? nullptr : dumpRefPath.c_str();
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
    // auto(0)/gpu(1) 模式都需加载 REDOpenCL 组件（GPU 路径 REDCL 依赖它）；
    // 仅纯 CPU 模式（--decode cpu, 2）不加载
    if (!nraw::initSdk(opt.sdkPath, err, opt.decodeMode != 2)) {
        fprintf(stderr, "initSdk 失败: %s\n", err.c_str());
        return 2;
    }

    // --gpu-test 无输入文件模式：只做 GPU 初始化（OpenCL 加载/枚举 + REDCL
    // 内核编译），不打开剪辑、不做 A/B 门控。退出码 0 = 初始化通过，2 = 失败。
    if (opt.gpuTest && opt.input.empty()) {
        nraw::GpuPipeline gpu;
        std::string gerr;
        printf("GPU 测试（无输入文件模式）: 仅初始化 OpenCL + REDCL + 内核编译\n");
        fflush(stdout);
        if (!gpu.initGpuOnly(gerr)) {
            fprintf(stderr, "GPU 初始化失败: %s\n", gerr.c_str());
            gpu.close();
            nraw::shutdownSdk();
            return 2;
        }
        printf("GPU 测试结果: 通过（设备 %s，初始化 + 内核编译完成）\n",
               gpu.deviceName().c_str());
        gpu.close();
        nraw::shutdownSdk();
        return 0;
    }

    nraw::MediaInfo info;
    if (!nraw::openMedia(opt.input, opt, info, err)) {
        fprintf(stderr, "打开媒体失败: %s\n", err.c_str());
        nraw::shutdownSdk();
        return 2;
    }

    std::thread hashTh;
    // RAII：任何提前 return 路径都自动 join 哈希线程（不 join 会
    // std::terminate——"terminate called without an active exception"）
    struct HashJoinGuard {
        std::thread& t;
        ~HashJoinGuard() {
            if (t.joinable())
                t.join();
        }
    } hashJoinGuard{hashTh};
    const bool needHash = !opt.noSidecar && opt.dumpRef.empty() && !opt.gpuTest;
    if (needHash) {
        std::promise<std::string> hp;
        nraw::g_sourceHashFut = hp.get_future().share();
        hashTh = std::thread([p = std::move(hp), &opt]() mutable {
            std::string h;
            try {
                h = nraw::sha256File(opt.input);
            } catch (const std::exception& e) {
                fprintf(stderr, "警告: 源文件 sha256 计算异常: %s\n", e.what());
            }
            if (h.empty())
                fprintf(stderr, "警告: 无法计算源文件 sha256（读取失败），"
                                 "sidecar 将记录空哈希\n");
            p.set_value(h);  // 异常也 set（空串），避免 get() 抛 future_error
        });
    }
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
                if (needHash && nraw::g_sourceHashFut.valid())
                    (void)nraw::g_sourceHashFut.get();  // 等哈希线程结束，防泄漏
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

    // 清理 purgePartFile 中途被杀可能残留的 .purging 临时文件
    remove((opt.output + ".part.purging").c_str());

    // 源文件哈希在后台并行计算（编码不阻塞）：detectResume 校验
    // ck.sourceSha256 vs opt.sourceSha256——有 .part（续传候选）时在此
    // 等待哈希完成（全新编码无 .part 不等，避免 270GB 哈希阻塞启动）
    if (needHash && nraw::g_sourceHashFut.valid()) {
        struct stat stb;
        if (stat((opt.output + ".part").c_str(), &stb) == 0)
            opt.sourceSha256 = nraw::g_sourceHashFut.get();
    }

    // ---- 断点续传自动检测 ----
    std::string resumeMsg;
    int rdet = 0;
    if (opt.dumpRef.empty() && !opt.gpuTest && !opt.decodeWorker) {
        rdet = nraw::detectResume(opt, info, resumeMsg);
        if (rdet == 0) {
            // 续传轮转窗口恢复：rename(.part→.part.old) 之后、新 .part 落盘
            // 完整样本之前崩溃（purge 全程 + 重放开头，分钟级窗口）→ 只剩
            // .part.old + .samples/.ckpt。此时若直接删 .part.old 会把数小时
            // 成果抹掉——rename 回退为 .part 再重新检测续传。
            struct stat stb;
            const bool hasOld =
                stat((opt.output + ".part.old").c_str(), &stb) == 0;
            const bool hasPart =
                stat((opt.output + ".part").c_str(), &stb) == 0;
            if (hasOld && !hasPart) {
                if (rename((opt.output + ".part.old").c_str(),
                           (opt.output + ".part").c_str()) == 0) {
                    fprintf(stderr, "恢复: 检测到未完成的续传（.part.old 存在、"
                                    ".part 缺失），已回退为 .part 并重新检测\n");
                    rdet = nraw::detectResume(opt, info, resumeMsg);
                } else {
                    fprintf(stderr, "警告: 无法回退 .part.old（%s），保留原文件\n",
                            strerror(errno));
                }
            } else if (hasOld) {
                // .part 与 .part.old 并存，但 rdet==0 说明当前 .part 无完整
                // 样本或不足一个完整关键帧（上一轮续传在重放早期崩溃，.part
                // 只是空壳/残缺前缀）：此时 .part.old 是上一轮的全部成果，且
                // .samples/.ckpt 仍与其一致——直接删除会把数小时编码成果抹掉。
                // 回退 .part.old（原子覆盖空壳 .part）再重新检测续传。
                if (rename((opt.output + ".part.old").c_str(),
                           (opt.output + ".part").c_str()) == 0) {
                    fprintf(stderr, "恢复: 检测到 .part 无有效进度且存在 .part.old，"
                                    "已回退为 .part 并重新检测\n");
                    rdet = nraw::detectResume(opt, info, resumeMsg);
                } else {
                    fprintf(stderr, "警告: 无法回退 .part.old（%s），保留原文件\n",
                            strerror(errno));
                }
            }
        } else if (rdet == 2) {
            fprintf(stderr, "\n错误: %s\n", resumeMsg.c_str());
            gpuPipe.close();
            nraw::closeMedia();
            nraw::shutdownSdk();
            return 4;
        }
        if (rdet == 1) {
            printf("续传: %s\n", resumeMsg.c_str());
            if (opt.sourceSha256.empty())
                fprintf(stderr, "警告: 源文件 SHA-256 为空（--no-sidecar 或哈希失败），"
                                 "本次续传未校验源文件未变\n");
            // 加载样本日志供重放，并丢弃续传点之后的旧视频条目
            // （其数据将被重新编码，不在新 .part 中；保留会导致重放/重建布局错位）
            std::string lerr;
            if (!nraw::g_sampleLog->loadFrom(opt.output + ".samples", lerr)) {
                fprintf(stderr, "\n错误: 样本日志加载失败: %s\n", lerr.c_str());
                gpuPipe.close();
                nraw::closeMedia();
                nraw::shutdownSdk();
                return 4;
            }
            // 日志截断到 .part 实际完整的前缀（countCompleteSamples 语义）：
            // 掉电/截断时 .samples 可能比 .part 持久化得更靠前（日志有条目、
            // 数据未落盘）。若只按 purgeVideoFrom 清视频，截断点之后的音频
            // 幻影条目会留在日志里、排序后混入保留区，重放按顺序累计偏移
            // 读到错误字节 → 静默损坏。截断后音视频条目与 .part 一一对应。
            {
                const size_t complete = nraw::countCompleteSamples(
                    opt.output + ".part", *nraw::g_sampleLog);
                if (complete < nraw::g_sampleLog->entries.size())
                    nraw::g_sampleLog->entries.resize(complete);
            }
            // 旧 .part 改名为 .part.old 作为只读数据源，会话写规范的 .part：
            // 续传再失败时 .part/.samples/.ckpt 保持自洽，可再次续传。
            // 注意顺序：
            //  1) purgePartFile 的 origOff 必须按原始（提交顺序 = .part 物理布局）
            //     累计"全部条目"（含将被丢弃的视频帧），故必须先于
            //     purgeVideoFrom 调用，否则偏移漏掉被丢弃条目的数据 → 读错位；
            //  2) purgeVideoFrom 再裁剪内存日志（视频 >= 续传点）；
            //  3) 最后按 (dts, 视频在前) 排序，与 purge 输出 / 重放顺序一致。
            const std::string oldPart = opt.output + ".part";
            const std::string oldPartBak = opt.output + ".part.old";
            // 不预先 remove：POSIX rename 原子覆盖旧 .part.old——
            // 预删会扩大"无任何备份"的瞬时窗口（配合 S1 恢复逻辑）
            if (rename(oldPart.c_str(), oldPartBak.c_str()) != 0) {
                fprintf(stderr, "\n错误: 无法备份旧 .part: %s\n", oldPart.c_str());
                gpuPipe.close();
                nraw::closeMedia();
                nraw::shutdownSdk();
                return 4;
            }
            {
                std::string perr;
                if (!nraw::purgePartFile(
                        oldPartBak, *nraw::g_sampleLog,
                        static_cast<int64_t>(opt.resumeFrame), perr, false)) {
                    fprintf(stderr, "\n错误: 清理旧 .part 失败: %s\n", perr.c_str());
                    gpuPipe.close();
                    nraw::closeMedia();
                    nraw::shutdownSdk();
                    return 4;
                }
            }
            nraw::g_sampleLog->purgeVideoFrom(
                static_cast<int64_t>(opt.resumeFrame));
            // 按 (dts, 视频在前) 排序：与 purge 输出 / 重放顺序一致
            std::stable_sort(nraw::g_sampleLog->entries.begin(),
                             nraw::g_sampleLog->entries.end(),
                             [](const nraw::SampleLog::Entry& a,
                                const nraw::SampleLog::Entry& b) {
                                 if (a.dts != b.dts)
                                     return a.dts < b.dts;
                                 return a.video && !b.video;
                             });
            // 立即把排序后的日志持久化到 .samples：purgePartFile 已把
            // .part.old 改写为排序布局，但磁盘 .samples 此刻仍是旧的提交序
            // 日志（要等 openSession 才重写）。若在此窗口崩溃（SIGINT/_exit），
            // 下次运行 S1 恢复会把"排序的 .part"与"提交序 .samples"配对，
            // countCompleteSamples/purge/重放按提交序累计偏移读取排序文件 →
            // 静默写入垃圾帧（无任何报错）。saveTo 保证磁盘状态自洽；写入
            // 撕裂安全：loadFrom 容忍截断，且排序日志前缀与排序文件前缀
            // 偏移一致，续传只会从更早的关键帧开始。
            {
                std::string serr;
                if (!nraw::g_sampleLog->saveTo(opt.output + ".samples", serr)) {
                    fprintf(stderr, "\n错误: 续传日志持久化失败: %s\n", serr.c_str());
                    gpuPipe.close();
                    nraw::closeMedia();
                    nraw::shutdownSdk();
                    return 4;
                }
            }
        }
    }

    int rc = 0;
    if (useGpu) {
        rc = nraw::runGpuPath(opt, info, frameCount, gpuPipe);
    } else {
        // 统一 CPU 调度：总预算 = --jobs（默认 = 可用核心数），拆分为
        //   解码 worker 进程数（每个 ~2 核：1 核解码 + SDK 内部线程开销）与
        //   x265 编码线程池（剩余预算）。显式指定的 --cpu-workers / --pools 优先。
        if (opt.jobs <= 0) {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            opt.jobs = n > 0 ? static_cast<int>(n) : 8;
        }
        if (opt.cpuWorkers <= 0) {
            long w = opt.jobs / 4;   // 每 worker 按 ~4 核预算 1 个（保守）
            opt.cpuWorkers = w < 1 ? 1 : (w > 8 ? 8 : static_cast<int>(w));
        }
        if (opt.pools <= 0) {
            long p = static_cast<long>(opt.jobs) - 2L * opt.cpuWorkers;
            opt.pools = p < 1 ? 1 : (p > 1024 ? 1024 : static_cast<int>(p));
        }
        printf("CPU 调度: %d 个解码 worker 进程 + x265 pools=%d（总预算 %d 核；"
               "--cpu-workers/--pools 可显式覆盖）\n",
               opt.cpuWorkers, opt.pools, opt.jobs);
        rc = nraw::runCpuPath(opt, info, frameCount);
    }
    gpuPipe.close();

    if (rc == 0 && opt.dumpRef.empty()) {
        // 成功后清理续传检查点与样本日志（新输出已 rename 到最终路径）
        remove((opt.output + ".samples").c_str());
        remove((opt.output + ".ckpt").c_str());
        // 续传成功后旧 .part 数据源（.part.old）残留：必须删除，
        // 否则下次运行 detectResume 会因"有 .part 无检查点"而拒绝执行
        if (opt.resumeMode) {
            remove((opt.output + ".part.old").c_str());
            remove((opt.output + ".part").c_str());
        }
    }

    if (rc != 0) {
        if (!opt.dumpRef.empty())
            remove(opt.dumpRef.c_str());
        nraw::closeMedia();
        nraw::shutdownSdk();
        return rc;
    }

    if (!opt.noSidecar && opt.dumpRef.empty()) {
        if (needHash && nraw::g_sourceHashFut.valid())
            opt.sourceSha256 = nraw::g_sourceHashFut.get();  // sidecar 需要 sha256
        nraw::AppliedSettings applied;
        nraw::appliedSettings(applied);
        if (nraw::writeSidecar(opt.output, opt, info, frameCount, applied, gpu,
                               nraw::g_sourceHashFut.valid() ? opt.sourceSha256
                                                             : std::string(),
                               true) != 0) {
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
