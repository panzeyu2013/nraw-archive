# nraw-archive

**Nikon N-RAW (.NEV) → single-file HEVC archive tool** for Nikon Z8/Z9 footage.

Two decode paths: a **GPU path** (AsyncDecoder multi-threaded decompression + REDOpenCL GPU debayer/IPP2, with automatic fallback when no GPU or OpenCL driver is present) and a **CPU path** (synchronous SDK decode parallelized across N worker processes: the classic SDK decode is globally serialized within a process (~1 fps), so `--decode cpu` forks N workers decoding frames k mod N over pipes, measured ~5× faster). Zero intermediate files — SDK decode feeds x265 encoding directly into a single MOV, with a sidecar JSON recording checksums, metadata, and the decode path (`decode_path: gpu/cpu`).

> 中文版 → [README.zh-CN.md](README.zh-CN.md)

## Why not just use ffmpeg?

FFmpeg ≥ 6.0 ships a native N-RAW (`nraw`) demuxer/decoder, but it is a codec-level decode: it does **not** provide RED's full IPP2 camera image pipeline — lens distortion correction, RWG/Log3G10 color grading, noise reduction, and the color semantics used by DaVinci Resolve. nraw-archive uses the RED R3DSDK so the archive carries the same color science as the camera and Resolve, with the camera's as-shot settings (white balance, ISO, exposure, lens correction) applied.

## Features

| Item | Spec |
| --- | --- |
| Container | QuickTime MOV, single file (optional `--faststart` moov-fronting) |
| Video codec | HEVC (x265) Main 10, 4:2:0 10-bit |
| Rate control | CRF constant quality, default 14 (recommended 12–18) |
| Color space | RWG / Log3G10 native (raw SDK output, pure encode wrapper — semantics unchanged) |
| Matrix | BT.2020 (VUI writes bt2020nc + full range) |
| VUI tags | only matrix + range written; primaries / transfer left empty |
| Audio | LPCM s24le (bit-exact repack from the SDK's 24-bit big-endian output) |
| Intermediate files | none |
| Integrity | atomic writes (`.part` + rename); **partial output preserved on failure** (never deleted — errors report path/errno/free space, auto-verifies and suggests rename when moov landed); sidecar sha256 of the source; A/B PSNR gate for the GPU path |

## Requirements

Dependencies (Ubuntu/Debian package names):

- cmake ≥ 3.16, g++ (C++14)
- libavcodec-dev, libavformat-dev, libavutil-dev, libswscale-dev
- opencl-headers (required at compile time; CMake fails without it)
- pkg-config

**Deployment:** the binary needs glibc ≥ 2.34 (≈ Ubuntu 22.04 / Debian 12 / RHEL 9+). On older distros build on the target machine (the SDK static library itself only needs glibc 2.27).

**OpenCL runtime (required for the GPU path, and indirectly for any run):** system FFmpeg (libavutil) loads `libOpenCL.so.1` at process startup, so the OpenCL loader must be installed even for CPU-only use:

```bash
sudo apt install ocl-icd-libopencl1
```

The GPU path additionally needs an official NVIDIA/AMD OpenCL driver (e.g. `nvidia-driver-*`, which ships `/etc/OpenCL/vendors/nvidia.icd`). For standalone CUDA installs, point the loader lookup at your library with the `NRAW_OPENCL_LIB` environment variable.

## SDK setup (required — not bundled)

The RED R3DSDK is **proprietary and not bundled with this repository**; redistributing the SDK development kit is outside this project's license. Obtain **R3DSDK v9.2.1** from RED (developer.red.com), then either:

1. **Default location** — place the SDK folder at the repository root (gitignored):

   ```
   <repo>/R3DSDKv9_2_1/
     Include/...                          # headers (R3DSDK.h etc.)
     Lib/linux64/libR3DSDK-cpp11.a        # static library
     Redistributable/linux/*.so           # runtime libraries
   ```

2. **Anywhere else** — point CMake at it:

   ```bash
   cmake -B build -DR3D_SDK_ROOT=/path/to/R3DSDKv9_2_1
   ```

At build time the four `Redistributable/linux/*.so` runtime libraries are copied next to the executable (CMake POST_BUILD); with an existing build or a manual deployment, point the binary at them with `--sdk-path <dir>` (default: the executable's own directory, resolved via `/proc/self/exe`).

Note on redistribution: RED's SDK license requires the runtime `.so` libraries to be distributed **with your application**; it does not permit redistributing the SDK development kit itself. This repository therefore contains neither.

**Version compatibility:** the binary is bound to the SDK version it was built against — the static library is compiled in and the runtime `.so` files are copied from the same SDK tree. The SDK itself enforces an exact runtime/static version match at initialization (mixing versions fails with *"library version mismatch"*). To use a different SDK version, point `-DR3D_SDK_ROOT` at it and rebuild; don't swap the runtime `.so` files after the build. The effective SDK version is printed at startup and recorded in the sidecar (`sdk_version`).

## Build

```bash
sudo apt install cmake g++ pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev opencl-headers
cmake -B build
cmake --build build -j
```

The binary is `build/nraw_archive` (the scripts look for both `nraw-archive` and `nraw_archive`).

Tests (optional, synthetic footage, no .NEV needed):

```bash
ctest --test-dir build
```

## Usage

```
nraw-archive [options] input.NEV [output.mov]
```

Default output: `input.h265.mov` next to the source, plus `input.h265.mov.sidecar.json`.

### CLI options

| Option | Description | Default |
| --- | --- | --- |
| `--kelvin <K> --tint <T> --iso <ISO>` | white balance / tint / ISO | camera metadata (as-shot) |
| `--exposure <stops>` | exposure offset (EV) | camera metadata (as-shot) |
| `--lens-correction auto\|on\|off` | lens (distortion) correction | on (auto lets the SDK decide) |
| `--chroma-nr on\|off` | chroma noise reduction | camera default (off) |
| `--decode gpu\|cpu\|auto` | decode path: auto = GPU probe + A/B gate (default); gpu = force GPU; cpu = pure CPU (multi-process parallel, measured ~5×) | auto |
| `--crf <n>` | x265 constant quality | 14 (recommended 12–18) |
| `--preset <p>` | x265 speed preset | slow |
| `--keyint <n>` | GOP length | auto (2 s × frame rate) |
| `--min-keyint <n>` | minimum GOP | 1 |
| `--pools <n>` | x265 encoding thread pool (encode only; independent of decode workers) | auto (split by --jobs) |
| `--cpu-workers <n>` | CPU decode worker processes (each ~1 fps, N workers ≈ N fps) | auto (split by --jobs, capped at 8) |
| `--jobs <n>` | total CPU thread budget, auto-split into decode workers and x265 pools (explicit --cpu-workers/--pools win) | auto (= online cores) |
| `--open-gop <0\|1>` | GOP structure: 1 = open (scenecut I-frames), 0 = closed (all IDR, frame-accurate cuts) | 1 |
| `--buffers <n>` | frame queue depth (GPU path only) | 16 |
| `--frames <n>` | process at most N frames (testing) | all |
| `--no-audio` | don't mux audio | off |
| `--faststart` | move moov to the front | off |
| `--no-sidecar` | don't write the sidecar JSON | off |
| `--dump-ref <file>` | write lossless yuv420p10le reference frames (testing; auto mode skips the GPU probe/gate and uses the CPU path) | off |
| `--sdk-path <dir>` | directory containing the RED*.so runtime libraries | executable directory |
| `--gpu-test` | test the GPU path and exit: OpenCL init + kernel compile status + A/B gate (exit 0 = usable, 2 = init failed, 4 = gate failed). Without an input file, only init + kernel compile are tested | |
| `--version` | print version and exit | |
| `--help` | print help and exit | |

## Output & sidecar

- `xxx.h265.mov` — HEVC Main10 4:2:0 10-bit, crf 14, matrix=BT.2020, full range, `pcm_s24le` audio. **Writes are atomic**: the file is written as `xxx.h265.mov.part` and renamed over the target on success; failures/interrupts never corrupt an existing archive.
- **Failure protection, auto-rebuild & resume** —
  - A failure at any stage (mid-stream write error, trailer/close error) **preserves the `.part` file instead of deleting it**, so hours of encoding are never discarded by the program itself. Errors include the preserved path, an errno detail (NFS I/O vs disk-full), and free disk space. Output is flushed every 500 frames so NFS/disk errors surface mid-encode instead of at finalization.
  - On finalization failure the tool **automatically rebuilds a complete file** (sample data re-muxed through a fresh MOV muxer to generate a correct moov, written to the final path, verified playable before reporting) — even a failed close/flush yields a usable archive immediately.
  - The encode records a sample log (`<out>.samples`, binary: sample size/timestamps/frame numbers) and a checkpoint (`<out>.ckpt`, refreshed every 500 frames). On the **next run, the tool auto-detects `.part` + checkpoint and resumes**: after verifying the source file and encode settings (crf/preset/GOP/color/lens/decode path etc.) via SHA-256 fingerprints, the already-encoded part is reused directly (no re-decode/re-encode) and encoding continues from the resume point — an interrupted 12-hour run finishes in minutes. The resume point is the **last complete keyframe**: closed-GOP resumes keyframe-aligned; open-GOP backs off a further **17 frames** (=2×(bframes+P-interval)−1) because scenecut keyframes may be written before their GOP-tail B-frames, so no frames are lost and no decode gaps appear; the point is then **finely adjusted under a "no timeline shift" constraint** (movenc 6.1 requires strictly increasing dts and pts≥dts: the replayed region's max dts must be below the re-encoded region's first-packet dts, otherwise a whole-timeline pts shift would jump the seam — or worse, replayed tail B-frames referencing re-encoded frames fail to decode, which the default open-GOP keyframe-anchored point hits in practice). If no keyframe-anchored candidate satisfies the constraint, the resume point walks forward frame-by-frame (it may land on a non-keyframe; a fresh IDR starts there) until it holds, so the seam's dts/pts stay exactly continuous; already-encoded audio is reused too, with the seam block-aligned.
  - Resume is repeatable: **a second resume after the resume session itself was interrupted works too** (the encoder flushes pending frames on interrupt; `.part`/`.samples`/`.ckpt` stay self-consistent). Closed-GOP resume points are keyframe-aligned (replayed GOP tails keep their references intact); open-GOP resumes back off 17 frames because scenecut keyframes may be written before their GOP-tail B-frames.
  - Signal interrupts (SIGINT/SIGTERM) also preserve the partials for resume.
- `xxx.h265.mov.sidecar.json` — sha256 of the source (computed with a built-in SHA-256 implementation, no external `sha256sum` dependency), color space RWG/Log3G10, matrix BT.2020 full range, lens-correction state, decode path (`decode_path: gpu|cpu`, with `gpu_device` and the A/B gate minimum PSNR `gate_psnr_db`), encode parameters (crf/preset/keyint/etc.), and clip metadata.

**Exit codes:** 0 success; 1 bad arguments; 2 SDK/media open failure (including forced-GPU unavailable with `--decode gpu`); 4 processing failure (encode/write/GPU pipeline); 5 sidecar write failure. With `--gpu-test`: 0 = GPU path usable (init + gate passed), 2 = GPU initialization failed, 4 = A/B gate failed or could not run. With `--gpu-test` and **no input file** (init + kernel compile only): 0 = init passed, 2 = init failed. SIGINT/SIGTERM preserves the partial artifacts (`.part`/`.samples`/`.ckpt`) for automatic resume on the next run and exits 130.

During processing a progress line is printed every 2 s showing a progress bar, percent, processed/total media duration, remaining duration, estimated wall-clock finish time, and actual fps.

## Pipeline

**GPU path** (default under auto): AsyncDecoder multi-threaded CPU decompression (200 fps class) → raw frames uploaded to VRAM → REDOpenCL DebayerJob performs debayer + lens correction + IPP2 (RWG/Log3G10, same architecture as Resolve) → 16-bit RGB read back → frames sorted by number → x265 encode thread consumes frame by frame. Audio is decoded in order inside the submission loop (SDK serialization constraint).

**CPU path** (fallback): sequential decode → encode, frame by frame (≈1 fps, bounded by the SDK's in-process serialization of the synchronous path).

**A/B gate** (auto mode): first/middle/last 3 frames (sampled to actual frame count for short clips) are decoded both by the CPU reference and by the GPU path and compared by PSNR; GPU is enabled only if the minimum ≥ 55 dB. On failure to initialize (no GPU / no OpenCL driver / driver failure) it falls back to CPU. The decision is recorded in the sidecar (`decode_path` + `gate_psnr_db`). `--decode gpu` forces the GPU: initialization failure is an error (exit 2), a below-threshold gate is recorded but processing continues.

The first GPU run compiles RED OpenCL kernels (a few minutes) and caches them at `$XDG_CACHE_HOME|~/.cache/nraw-archive/opencl`; subsequent starts take seconds.

## Color & precision notes

- The SDK outputs 16-bit planar data; the sensor's effective depth is ~14 bits. The 10-bit quantization loss is the standard expectation for a HEVC Main10 archive; the NEV 16-bit master is kept as the second track.
- Why the BT.2020 matrix: RWG's wide gamut produces negative values under Rec.709-family matrices, triggering clipping and rolloff; under BT.2020 full range, RGB ∈ [0,1] stays unclipped with no rolloff, and RWG/Log3G10 values are preserved as-is.
- Why VUI writes only matrix + range: decoders correctly map YUV→RGB with bt2020nc + full; leaving primaries/transfer empty avoids players applying a wrong gamma/OOTF to RWG/Log3G10 footage. Color semantics are described by metadata, consistent with Resolve.

## Audio

The SDK outputs 24-bit big-endian audio → bit-exact little-endian repack to s24le (`repack24beToS24le`), zero processing, zero resampling. Note: N-RAW float (Float32) audio is **not** supported — such clips fail to open with an error (24-bit integer LPCM only). The audio written is capped at `min(video frames' worth of samples, clip audio samples)`; the final block straddling the cap is truncated per-sample (CPU and GPU paths behave identically), so `--frames` truncation or A/V duration drift can never produce audio longer than the video.

## DaVinci Resolve

Import `xxx.h265.mov`, then in Clip Attributes set Input Color Space to **RWG / Log3G10** (REDWideGamutRGB / Log3G10). The color semantics match this tool's output and are fully compatible with Resolve's own H.265 RWG/Log3G10 exports.

## Verification

1. Quality gate: `vmaf_test.sh input.NEV [crf] [--frames N]` — a ~10 s segment (at 59.94; other frame rates scale by frame count) is compared frame-by-frame against the SDK's uncompressed reference; VMAF ≥ 95 passes; on failure it automatically retries at crf 12 for comparison. Without libvmaf in ffmpeg it degrades to PSNR+SSIM (PSNR ≥ 42 dB and SSIM ≥ 0.97).
2. Read-back check: `verify.sh xxx.h265.mov` — validates codec/profile/pix_fmt/BT.2020/full range/audio, and compares the sidecar's sha256 and crf/keyint/matrix fields.
3. Shadow-band check: in Resolve, inspect dark gradients on a scope for visible stepping (shouldn't be noticeable at 10-bit).
4. First/middle/last A/B comparison: put the NEV and `xxx.h265.mov` side by side and compare stills.

## Batch archiving

- Two artifacts per clip: `xxx.h265.mov` + `xxx.h265.mov.sidecar.json`, named after the NEV.
- Keep the NEV as the master (16-bit source).
- `batch.sh <dir> [extra args...]` — walks `*.NEV`/`*.nev` in the directory, logs failures to `<dir>/batch_failures.log` and continues; `--force` redoes existing outputs, `--dry-run` previews.
- Scripts locate the binary in their own directory / parent / `../build`, or via the `NRAW_ARCHIVE` environment variable.

## Known limitations

- N-RAW does not support the all-GPU GpuDecoder decompression path (SDK limitation); this tool's GPU path is "AsyncDecoder CPU decompress + REDOpenCL GPU image processing", which N-RAW officially supports.
- The GPU path needs an official NVIDIA/AMD OpenCL driver plus the OpenCL loader (Ubuntu: `nvidia-driver-*` + `ocl-icd-libopencl1`); without the loader the binary won't start (FFmpeg's indirect dependency); without the driver the GPU path falls back to CPU.
- Prebuilt binaries are ABI-pinned to FFmpeg 4.4 (Ubuntu 22.04); on distros with FFmpeg ≥ 5.1 (e.g. Ubuntu 24.04) build from source (the code is version-compatible).
- The GPU path relies on the SDK's contract that every successfully submitted decode invokes its callback exactly once; if the SDK never delivers a callback (extreme driver/IO failure), the pipeline waits during finalization rather than skipping the frame — accepted per the SDK contract.
- The first GPU run compiles OpenCL kernels (a few minutes, progress is printed); pre-warm `~/.cache/nraw-archive/opencl` with a single clip before batch runs. The cache is rebuilt automatically when the GPU or SDK version changes.
- Lens correction depends on distortion metadata written by the camera; when missing, correction is disabled automatically.
- `vmaf_test.sh` uncompressed reference / decoded frames are large (≈15 GB each at 4K); use `--frames` to shrink if needed.

## License

The code in this repository is licensed under the **MIT License** — see [LICENSE](LICENSE).

The RED R3DSDK is proprietary software owned by RED Digital Cinema; it is **not** distributed with this repository (see [SDK setup](#sdk-setup-required--not-bundled)). When you build and distribute binaries, the SDK runtime libraries are governed by RED's SDK license agreement.
