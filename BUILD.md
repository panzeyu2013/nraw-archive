# 构建指南

## 依赖（两平台通用）

- **FFmpeg 6.x**（libavformat/libavcodec/libavutil/libswscale，含 **libx265** 编码支持）
- **R3DSDK v9.2.1**：闭源 SDK，不入库。把 `R3DSDKv9_2_1/` 目录放到仓库根目录（
  或 `cmake -B build -DR3D_SDK_ROOT=/path/to/R3DSDKv9_2_1`）

## Linux x64

```bash
# Ubuntu 22.04+/Debian 12+
sudo apt install pkg-config libavformat-dev libavcodec-dev libavutil-dev \
     libswscale-dev libx265-dev opencl-headers ocl-icd-opencl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build            # 端到端回路测试（含全部续传/重建变体）
```

产物：`build/nraw_archive` + 同目录自动拷贝的 `RED*.so`（运行时需与二进制同目录）。

## macOS arm64（Apple Silicon，也兼容 Intel）

```bash
# 需要 Homebrew；OpenCL 用系统自带 framework，无需额外安装
brew install ffmpeg x265

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
ctest --test-dir build
```

- CMake 自动选择 `R3DSDKv9_2_1/Lib/mac64/libR3DSDK-libcpp.a`（x86_64+arm64 通用）
  并拷贝 `RED*.dylib` 到可执行文件目录。
- 产物为通用二进制或 arm64（取决于构建机器）；如需发布 arm64 专用产物，在
  Apple Silicon 机器上构建即可（`file build/nraw_archive` 可查架构）。
- 打包发布：
  ```bash
  mkdir -p dist/nraw-archive-macos-arm64
  cp build/nraw_archive build/RED*.dylib dist/nraw-archive-macos-arm64/
  cp README.md LICENSE README.zh-CN.md dist/nraw-archive-macos-arm64/
  tar czf dist/nraw-archive-macos-arm64.tar.gz dist/nraw-archive-macos-arm64
  ```
  然后在 release 页手动上传，或用 gh：
  ```bash
  gh release upload v1.1.0 dist/nraw-archive-macos-arm64.tar.gz
  ```

## GitHub Actions（可选）

`.github/workflows/release.yml` 提供 ubuntu-22.04 (x64) + macos-14 (arm64)
双平台发布构建。R3DSDK 需通过仓库 Secret `R3D_SDK_URL` 提供私有下载地址。
