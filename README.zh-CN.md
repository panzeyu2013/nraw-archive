# nraw-archive — Nikon NRAW (.NEV) → HEVC 存档工具

Nikon Z8/Z9 等 N-RAW（.NEV）→ 单文件 HEVC 归档工具。双解码路径：**GPU 主路径**（AsyncDecoder 多线程解压 + REDOpenCL GPU debayer/IPP2，无 GPU 或无 OpenCL 驱动时自动回退）与 **CPU 并行路径**（SDK 同步解码 + 多进程并行：实测 SDK 经典解码为进程内全局串行 ~1fps，故 --decode cpu 派生 N 个 worker 子进程经管道并行解码，实测约 5× 提速）。零中间文件，SDK 解码与 x265 编码直接对接输出单个 MOV，附带 sidecar JSON 记录校验、元数据与解码路径（`decode_path: gpu/cpu`）。

> English → [README.md](README.md)

## 为什么不用 ffmpeg 直接解码？

FFmpeg ≥ 6.0 已内置 N-RAW（`nraw`）demuxer/decoder，但那是 codec 层解码：**不提供** RED 完整的 IPP2 相机图像管线——镜头畸变矫正、RWG/Log3G10 调色、降噪，以及达芬奇所用的色彩语义。本工具使用 RED R3DSDK，归档与相机/达芬奇共享同一套色彩科学，并应用相机 as-shot 设置（白平衡、ISO、曝光、镜头矫正）。

## 目标规格

| 项目 | 规格 |
| --- | --- |
| 容器 | QuickTime MOV，单文件（可选 `--faststart` 前置 moov） |
| 视频编码 | HEVC (x265) Main 10，4:2:0 10bit |
| 码率控制 | CRF 恒定质量，默认 14（建议 12–18） |
| 色彩空间 | RWG / Log3G10 原生（SDK 原始输出，纯编码包装，语义不变） |
| 编码矩阵 | BT.2020（VUI 写入 bt2020nc + full range） |
| VUI 标记 | 只写 matrix + range；primaries / transfer 留空 |
| 音频 | LPCM s24le（SDK 24bit 大端位精确重排） |
| 中间文件 | 无 |
| 完整性 | 原子写入（`.part` + rename）；**失败保留 `.part` 不删除**（错误信息含路径/errno/磁盘剩余空间，moov 已落盘时自动验证并提示可改名）；sidecar 记录源文件 sha256；GPU 路径 A/B 门控 |

## 构建与运行前置

依赖（Ubuntu/Debian 包名）：

- cmake ≥ 3.16、g++（C++14）
- libavcodec-dev、libavformat-dev、libavutil-dev、libswscale-dev
- opencl-headers（编译必需，CMake 强制检查）
- pkg-config

**部署要求**：二进制需 glibc ≥ 2.34（约 Ubuntu 22.04 / Debian 12 / RHEL 9+）。旧发行版请在目标机自行编译（SDK 静态库本身仅需 glibc 2.27）。

**OpenCL 运行库（GPU 路径必需，且本工具间接依赖）**：系统 FFmpeg（libavutil）在进程启动时即加载 `libOpenCL.so.1`，因此**即使只用 CPU 路径，也必须安装 OpenCL 装载器**：

```bash
sudo apt install ocl-icd-libopencl1
```

GPU 路径另需 NVIDIA 官方驱动（`sudo apt install nvidia-driver-580` 等，含 `/etc/OpenCL/vendors/nvidia.icd`）。CUDA 独立安装场景可用环境变量 `NRAW_OPENCL_LIB=/路径/libOpenCL.so.1` 覆盖库查找。

## SDK 安装（必需 — 不随仓库提供）

RED R3DSDK 是**闭源软件，不随本仓库分发**（再分发 SDK 开发包不在本项目许可范围内）。请从 RED 官网（developer.red.com）获取 **R3DSDK v9.2.1**，然后二选一：

1. **默认位置** — 把 SDK 目录放到仓库根目录（已被 .gitignore 排除）：

   ```
   <仓库>/R3DSDKv9_2_1/
     Include/...                          # 头文件（R3DSDK.h 等）
     Lib/linux64/libR3DSDK-cpp11.a        # 静态库
     Redistributable/linux/*.so           # 运行时动态库
   ```

2. **任意位置** — 配置时用 CMake 指定：

   ```bash
   cmake -B build -DR3D_SDK_ROOT=/路径/to/R3DSDKv9_2_1
   ```

构建时会把 `Redistributable/linux/*.so` 四个运行时库自动拷贝到可执行文件同目录（CMake POST_BUILD）；旧构建或手动部署时可用 `--sdk-path <dir>` 指定（默认经 /proc/self/exe 解析可执行文件所在目录，不受 PATH 调用方式影响）。

关于再分发的说明：RED SDK 许可要求运行时 `.so` 库**随你的应用程序一起分发**，但不允许再分发 SDK 开发包本身。因此本仓库两者都不包含。

**版本兼容性**：二进制与构建时所用的 SDK 版本绑定——静态库编译进程序、运行时 `.so` 从同一 SDK 目录拷贝。SDK 自身在初始化时强制运行时库与静态库版本严格一致（混用版本会报"库版本不匹配"）。如需使用其他 SDK 版本，请用 `-DR3D_SDK_ROOT` 指向该版本目录重新构建；不要在构建后手动替换 `.so`。启动时会打印实际 SDK 版本，sidecar 中也记录 `sdk_version` 字段。

## 构建

```bash
sudo apt install cmake g++ pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev opencl-headers
cmake -B build
cmake --build build -j
```

构建产物为 `build/nraw_archive`（可按习惯重命名为 `nraw-archive`；scripts 下脚本对两种名字都会查找）。

测试（可选，合成素材回路测试，无需 .NEV）：

```bash
ctest --test-dir build
```

## 命令行参数

```
nraw-archive [options] input.NEV [output.mov]
```

默认输出与源同目录同名的 `input.h265.mov`，并生成 `input.h265.mov.sidecar.json`。

| 参数 | 说明 | 默认值 |
| --- | --- | --- |
| `--kelvin <K> --tint <T> --iso <ISO>` | 白平衡 / 色调 / 感光度 | 相机元数据（as-shot） |
| `--exposure <stops>` | 曝光偏移（EV） | 相机元数据（as-shot） |
| `--lens-correction auto\|on\|off` | 镜头（畸变）矫正 | on（auto 交由 SDK 判断） |
| `--chroma-nr on\|off` | 色度降噪 | 相机默认（off） |
| `--decode gpu\|cpu\|auto` | 解码路径：auto=GPU 探测 + A/B 门控（默认）；gpu=强制 GPU；cpu=纯 CPU（多进程并行，实测 ~5×） | auto |
| `--crf <n>` | x265 恒定质量 | 14（建议 12–18） |
| `--preset <p>` | x265 速度档 | slow |
| `--keyint <n>` | GOP 长度 | auto（2 秒 × 帧率） |
| `--min-keyint <n>` | 最小 GOP | 1 |
| `--pools <n>` | x265 编码线程池大小（仅编码；与解码 worker 相互独立） | auto（由 --jobs 统一分配） |
| `--cpu-workers <n>` | CPU 解码 worker 进程数（每个约 ~1fps，N 个≈N fps） | auto（由 --jobs 分配，上限 8） |
| `--jobs <n>` | CPU 总线程预算，自动拆分为解码 worker 数与 x265 pools（显式 --cpu-workers/--pools 优先） | auto（= 可用核心数） |
| `--open-gop <0\|1>` | GOP 结构：1=open（场景切点 I 帧），0=closed（全 IDR，帧精确剪接更稳） | 1 |
| `--buffers <n>` | 帧队列深度（GPU 路径使用；CPU 路径由管道背压控制） | 16 |
| `--frames <n>` | 处理帧数上限（测试用） | 全部 |
| `--no-audio` | 不封装音频 | 关 |
| `--faststart` | moov 移到文件头 | 关 |
| `--no-sidecar` | 不写 sidecar JSON | 关 |
| `--dump-ref <file>` | 输出无损 yuv420p10le 参考帧（测试专用；auto 模式自动走 CPU 路径，跳过 GPU 探测/门控/内核编译） | 关 |
| `--sdk-path <dir>` | SDK .so 目录 | 可执行文件所在目录 |
| `--gpu-test` | 测试 GPU 路径后退出：OpenCL 初始化 + 内核编译状态 + A/B 门控（退出码 0=可用，2=初始化失败，4=门控失败）。无输入文件时仅测初始化 + 内核编译 | |
| `--version` | 显示版本号后退出 | |
| `--help` | 显示帮助后退出 | |

## 输出与 sidecar

- `xxx.h265.mov`：HEVC Main10 4:2:0 10bit，crf 14，matrix=BT.2020、full range，音频 pcm_s24le。**写入是原子的**：先写 `xxx.h265.mov.part`，成功后 rename 替换，失败/中断不破坏已有归档。
- **失败保护、自动重建与断点续传**：
  - 任何阶段失败（编码中途写错误、trailer/关闭失败）都会**保留 `.part` 部分产物**而不是删除——数小时的编码成果不会被程序自己抹掉。错误信息含保留路径、errno 详情（区分 NFS I/O 错误与磁盘满）和磁盘剩余空间；每 500 帧刷盘一次，NFS/磁盘错误在编码途中尽早暴露。
  - 收尾失败时程序**自动重建完整文件**：用样本数据重新过一遍 MOV 封装器生成正确的 moov（输出到最终路径，验证可播放后报告路径），即使关闭/flush 失败也能立即拿到可播放的归档。
  - 编码过程记录样本日志（`<out>.samples`，二进制：样本大小/时间戳/帧号）与检查点（`<out>.ckpt`，每 500 帧刷新）；**下次运行自动检测到 `.part`+检查点即自动续传**：源文件与编码参数（crf/preset/GOP/色彩/镜头矫正/解码路径等）经 SHA-256 校验一致后，已编码部分直接复用（不重新解码/编码），从续传点继续编码剩余帧——12 小时的编码中断后重跑只需几分钟收尾。续传点取**最后完整关键帧**：closed-GOP 直接关键帧对齐；open-GOP 因 scenecut 关键帧可能先于其 GOP 尾部 B 帧写入，续传点再**回退 17 帧**（=2×(bframes+P 间隔)-1）保证尾部 B 帧引用封闭，不产生解码缺口；随后按"无时间轴平移"约束**动态微调**（movenc 6.1 要求 dts 严格递增且 pts≥dts：重放区最大 dts 必须小于重编码首包 dts，否则整体平移 pts 会造成接缝跳变、甚至重放尾部引用重编码区导致解码失败——实测 open-GOP 默认配置下关键帧锚定续传点即触发该问题）。不满足时续传点逐帧前移（可落在非关键帧，新 IDR 从该帧起）直到约束满足，保证接缝处 dts/pts 严格连续；音频已编码部分同样复用，衔接点与块边界对齐。
  - 续传可反复进行：**续传会话自身再次中断后仍可再次续传**（编码器在中断时冲刷挂起帧，`.part`/`.samples`/`.ckpt` 始终自洽）。closed-GOP 续传点对齐关键帧（重放 GOP 尾部引用完整）；open-GOP 因 scenecut 关键帧可能先于尾部 B 帧写入，续传点回退 17 帧避免丢帧。
  - 信号中断（SIGINT/SIGTERM）同样保留部分产物供续传。
- `xxx.h265.mov.sidecar.json`：sha256（源文件，内置 SHA-256 实现计算，不依赖外部 sha256sum）、色彩空间 RWG/Log3G10、matrix BT.2020 full range、镜头矫正状态、解码路径（`decode_path: gpu|cpu`，含 `gpu_device` 与 A/B 门控最低 PSNR `gate_psnr_db`）、编码参数（crf/preset/keyint 等）、clip 元数据。

退出码：0 成功；1 参数错误；2 SDK/媒体打开失败（含 `--decode gpu` 强制模式下的 GPU 不可用）；4 处理失败（编码/写入/GPU 管线中断）；5 sidecar 写入失败。`--gpu-test` 模式下：0 = GPU 可用（初始化+门控通过），2 = GPU 初始化失败，4 = A/B 门控未达标或未能执行；`--gpu-test` 无输入文件时（仅初始化+内核编译）：0 = 初始化通过，2 = 初始化失败。SIGINT/SIGTERM 时保留部分产物（`.part`/`.samples`/`.ckpt`）供下次运行自动续传，以 130 退出。

处理过程中每 2 秒刷新一行进度：进度条 + 百分比 + 已处理/总时长 + 剩余时长 + 预计结束时间（墙钟）+ 实际 fps。

## 流水线

**GPU 路径**（默认，auto 探测）：AsyncDecoder 多线程 CPU 解压（200fps 级）→ 原始帧上传显存 → REDOpenCL DebayerJob 完成 debayer + 镜头矫正 + IPP2（RWG/Log3G10，与达芬奇同架构）→ 回读 16bit RGB → 按帧号排序 → x265 编码线程逐帧消费。音频在提交循环内顺序解码（SDK 串行化约束）。

**CPU 路径**（回退）：顺序解码→编码逐帧衔接（约 1fps，SDK 同步路径进程内全局串行化上限）。

**A/B 门控**（auto 模式）：首/中/尾 3 帧（片段过短时按实际帧数取样）用 CPU 参考解码与 GPU 同帧对比 PSNR，最低 ≥ 55dB 才启用 GPU，不达标或初始化失败（无 GPU / 无 OpenCL 驱动 / 驱动失败）自动回退 CPU，决策记入 sidecar（`decode_path` + `gate_psnr_db`）。`--decode gpu` 强制 GPU：初始化失败即报错（退出码 2），门控不达标仅记录并继续。

首次 GPU 运行需编译 RED OpenCL 内核（约数分钟），缓存于 `$XDG_CACHE_HOME|~/.cache/nraw-archive/opencl`，之后秒级启动。

## 色彩与精度说明

- SDK 输出 16bit 平面；传感器实际有效约 14bit。10bit 量化损失是 HEVC Main10 存档的标准预期，母版另有 NEV 16bit 双轨保留。
- 为什么用 BT.2020 矩阵：RWG 广色域在 Rec.709 系矩阵下会产生负值，触发裁剪与 rolloff；BT.2020 full range 下 RGB∈[0,1] 不裁剪、无 rolloff，RWG/Log3G10 数值原样保持。
- VUI 只写 matrix + range 的意义：解码端按 bt2020nc + full 正确还原 YUV→RGB；primaries / transfer 留空，避免播放器对 RWG/Log3G10 素材套用错误的伽马 / OOTF。色彩语义由元数据描述，与达芬奇一致。

## 音频说明

SDK 输出 24bit 大端音频 → s24le 小端位精确重排（`repack24beToS24le`），零处理、零重采样。注意：N-RAW 的浮点音频（Float32）不被支持，打开此类剪辑会报错退出（仅支持 24-bit 整数 LPCM）。音频写入上限 = min(视频实际编码帧数对应采样数, 剪辑音频采样数)，跨越上限的最后一个块按采样数截断（CPU 与 GPU 路径一致），`--frames` 截断或音视频时长漂移不会产生长于视频的音频。

## 达芬奇使用

导入 `xxx.h265.mov` 后，在 Clip Attributes 中把输入色彩空间设为 RWG / Log3G10（REDWideGamutRGB / Log3G10）。色彩语义与本工具输出一致，与达芬奇自身导出的 H.265 RWG/Log3G10 素材完全兼容。

## 验证流程

1. 质量门控：`vmaf_test.sh input.NEV [crf] [--frames N]` — 10 秒片段（@59.94；其他帧率按帧数折算）与 SDK 未压缩参考逐帧比较，VMAF ≥ 95 通过；不达标自动以 crf 12 重试对比。ffmpeg 无 libvmaf 时自动降级 PSNR+SSIM 双指标（PSNR ≥ 42 dB 且 SSIM ≥ 0.97）。
2. 回读校验：`verify.sh xxx.h265.mov` — 校验 codec/profile/pix_fmt/BT.2020/full range/音频，比对 sidecar 中 sha256 与 crf/keyint/matrix 字段。
3. 暗部 banding 检查：达芬奇中检查暗部渐变场景，用示波器确认无可见阶跃（10bit 存档下不应明显）。
4. 首 / 中 / 尾 A/B 对比：将 NEV 与 `xxx.h265.mov` 并排放置，比较首、中、尾三处静止帧。

## 归档规范

- 每个 clip 两份产物：`xxx.h265.mov` + `xxx.h265.mov.sidecar.json`，与 NEV 同名。
- NEV 双轨保留为母版（16bit 源）。
- 批量归档：`batch.sh <目录> [额外参数...]` — 遍历目录下 *.NEV / *.nev，失败记录到 `<目录>/batch_failures.log` 并继续，`--force` 强制重做、`--dry-run` 预演。
- 脚本定位可执行文件：自身同目录 / 上一级 / `../build`，或通过环境变量 `NRAW_ARCHIVE=/路径/nraw-archive` 指定。

## 已知限制

- N-RAW 不支持 GpuDecoder 全 GPU 解压（SDK 限制）；本工具 GPU 路径为「AsyncDecoder CPU 解压 + REDOpenCL GPU 图像处理」，N-RAW 官方支持该组合。
- GPU 路径需 NVIDIA/AMD 官方 OpenCL 驱动 + OpenCL 装载器（Ubuntu：`nvidia-driver-*` + `ocl-icd-libopencl1`）；装载器缺失时二进制无法启动（FFmpeg 间接依赖），驱动缺失时 GPU 路径自动回退 CPU。
- 预编译二进制对 FFmpeg 4.4（Ubuntu 22.04）ABI 固定；在 FFmpeg ≥ 5.1 的发行版（如 Ubuntu 24.04）上需自行编译（代码已做跨版本兼容）。
- GPU 路径依赖 SDK「每个成功提交的解码必然回调一次」的契约：若 SDK 未交付回调（极端驱动/IO 故障），管线会在收尾阶段挂起等待而非跳过帧——按 SDK 文档契约接受此行为。
- 首次 GPU 运行编译 OpenCL 内核（数分钟，进度提示已刷新输出）；批处理前建议先用单个 clip 预热 `~/.cache/nraw-archive/opencl` 缓存。更换显卡或 SDK 版本后缓存自动失效重建。
- 镜头矫正依赖相机写入的畸变元数据，缺失时自动关闭矫正。
- `vmaf_test.sh` 未压缩参考帧 / 解码帧临时占用大（4K 每份约 15 GB），必要时用 `--frames` 调小。

## 许可证

本仓库代码采用 **MIT 许可证** — 见 [LICENSE](LICENSE)。

RED R3DSDK 是 RED Digital Cinema 的专有软件，**不随本仓库分发**（见 [SDK 安装](#sdk-安装必需--不随仓库提供)）。构建并分发二进制时，SDK 运行时库受 RED SDK 许可协议约束。
