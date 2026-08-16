#!/usr/bin/env bash
#
# vmaf_test.sh — 10 秒片段质量门控
#
# 用法:
#   vmaf_test.sh <input.NEV> [crf] [--frames N]
#     crf     默认 14
#     frames  默认 600（≈10 秒 @ 59.94fps；片段更短时按实际帧数处理）
#
# 流程:
#   1. 导出 SDK 未压缩参考帧（--dump-ref，yuv420p10le，与编码走同一 swscale 路径）
#   2. 以指定 crf 编码 10 秒测试片段
#   3. 解码回原始 yuv420p10le
#   4. 与参考帧逐帧比较: VMAF（ffmpeg 无 libvmaf 时自动降级 PSNR+SSIM 双指标，
#      判据 PSNR≥42 dB 且 SSIM≥0.97）
#   门控: VMAF ≥ 95 通过；< 95 自动以 --crf 12 重试一次并对比
# 退出码: 0 = 通过, 1 = 质量未达标, 2 = 用法/环境错误
set -u

# ---------- 定位 nraw-archive 可执行文件 ----------
source "$(cd "$(dirname "$0")" && pwd)/common.sh"

# ---------- 参数 ----------
input="${1:-}"
crf="${2:-14}"
frames=""
frames_flag=0
case "${2:-}" in
    --frames=*) frames="${2#--frames=}"; frames_flag=1; crf=14 ;;
    --frames)   frames="${3:-}";         frames_flag=1; crf=14 ;;
esac
case "${3:-}" in
    --frames=*) frames="${3#--frames=}"; frames_flag=1 ;;
    --frames)   frames="${4:-}";         frames_flag=1 ;;
esac
if [ "$frames_flag" -eq 0 ] && [ -n "${3:-}" ]; then
    frames="$3"; frames_flag=1
fi
if [ "$frames_flag" -eq 1 ]; then
    case "$frames" in
        ''|*[!0-9]*) echo "错误: 帧数参数无效: $frames" >&2; exit 2 ;;
    esac
    if [ "$frames" -lt 1 ]; then
        echo "错误: 帧数必须 ≥ 1" >&2
        exit 2
    fi
fi
case "$crf" in
    ''|*[!0-9]*) echo "错误: crf 参数无效: $crf" >&2; exit 2 ;;
esac
if [ "$crf" -gt 51 ]; then
    echo "错误: crf 超出范围 (0-51): $crf" >&2
    exit 2
fi
if [ -z "$input" ]; then
    echo "用法: vmaf_test.sh <input.NEV> [crf] [--frames N]" >&2
    exit 2
fi
if [ ! -f "$input" ]; then
    echo "错误: 输入文件不存在: $input" >&2
    exit 2
fi

BIN="$(find_bin)" || {
    echo "错误: 找不到 nraw-archive 可执行文件（可用 NRAW_ARCHIVE 环境变量指定）" >&2
    exit 2
}
for t in ffmpeg ffprobe; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "错误: 缺少依赖命令: $t" >&2
        exit 2
    fi
done
JQ="$(command -v jq 2>/dev/null || true)"

# ---------- 帧数自动折算 ----------
# 未显式指定 --frames 时，按容器帧率折算 10 秒（约 600 帧 @59.94）；
# ffprobe 读不到帧率（如无容器元数据）时回退 600。
if [ "$frames_flag" -eq 0 ]; then
    frames=600
    fps_raw="$(ffprobe -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$input" 2>/dev/null)"
    case "$fps_raw" in
        [0-9]*/*)
            den="${fps_raw##*/}"
            case "$den" in
                [1-9][0-9]*)
                    frames=$(( ( (${fps_raw%%/*}) * 10 + den / 2 ) / den ))
                    [ "$frames" -lt 1 ] && frames=1
                    ;;
            esac
            ;;
    esac
fi

# ---------- libvmaf 支持检测 ----------
use_vmaf=1
if ! ffmpeg -hide_banner -filters 2>/dev/null | grep -q libvmaf; then
    use_vmaf=0
    echo "[提示] 当前 ffmpeg 未编译 libvmaf（ffmpeg -filters 中无 vmaf），降级为 PSNR+SSIM 双指标评估。"
    echo "       降级判据: PSNR ≥ 42 dB 且 SSIM ≥ 0.97"
fi

# ---------- 临时目录 ----------
tmpdir="$(mktemp -d /tmp/nraw_vmaf_XXXXXX 2>/dev/null)" || tmpdir="/tmp/nraw_vmaf_$$"
mkdir -p "$tmpdir"
ref="$tmpdir/ref.yuv"
enc="$tmpdir/enc.yuv"
mov="$tmpdir/test.mov"
vjson="$tmpdir/vmaf.json"
psnr_log="$tmpdir/psnr.log"
ssim_log="$tmpdir/ssim.log"
trap 'rm -rf "$tmpdir"' EXIT

echo "== 质量门控: $input =="
echo "参数: crf=$crf, 帧数=$frames（≈10 秒 @59.94；未指定 --frames 时按容器帧率折算，读不到则默认 600）"
echo "[提示] 未压缩参考帧/解码帧占用临时空间大（4K 每份约 15 GB），必要时用 --frames 调小"

# ---------- 1. 导出参考帧 ----------
echo "[1/4] 导出参考帧（SDK 未压缩 yuv420p10le）..."
"$BIN" --frames "$frames" --no-audio --no-sidecar --dump-ref "$ref" "$input" || {
    echo "错误: 参考帧导出失败（若提示 SDK .so 缺失，请把 R3DSDKv9_2_1/Redistributable/linux/*.so 放到可执行文件同目录，或用 nraw-archive 的 --sdk-path 指定）" >&2
    exit 2
}

# ---------- 编码 + 评分 ----------
# 全局输出: score（VMAF 均值或 PSNR 值）、ssim_val
score=""
ssim_val=""

ge() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a>=b)}'; }
lt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a<b)}'; }

run_crf() {  # $1=crf $2=输出 mov $3=输出 yuv $4=VMAF json 路径
    local c="$1" mv="$2" yy="$3" jf="$4"
    local W H
    echo "  编码 crf=$c ..."
    "$BIN" --frames "$frames" --no-audio --no-sidecar --crf "$c" "$input" "$mv" || {
        echo "  错误: 编码失败"; return 1; }
    echo "  解码为原始 yuv420p10le ..."
    ffmpeg -y -v error -i "$mv" -f rawvideo -pix_fmt yuv420p10le "$yy" || {
        echo "  错误: 解码失败"; return 1; }
    W="$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$mv")"
    H="$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$mv")"
    if [ "$use_vmaf" -eq 1 ]; then
        echo "  计算 VMAF（${W}x${H}）..."
        ffmpeg -y -v error \
            -f rawvideo -pix_fmt yuv420p10le -s "${W}x${H}" -i "$ref" \
            -f rawvideo -pix_fmt yuv420p10le -s "${W}x${H}" -i "$yy" \
            -lavfi "libvmaf=log_fmt=json:log_path=$jf" -f null - || {
            echo "  错误: VMAF 计算失败"; return 1; }
        if [ -n "$JQ" ]; then
            score="$(jq -r '[.. | objects | .mean? // empty] | .[0]' "$jf" 2>/dev/null)"
        fi
        if [ -z "$score" ]; then
            score="$(grep -oE '"mean"[[:space:]]*:[[:space:]]*[0-9.]+' "$jf" 2>/dev/null \
                     | head -1 | sed -E 's/.*:[[:space:]]*//')"
        fi
        if [ -z "$score" ]; then
            # 极老版本 libvmaf 无 pooled_metrics，取首个 vmaf 值近似
            score="$(grep -oE '"vmaf"[[:space:]]*:[[:space:]]*[0-9.]+' "$jf" 2>/dev/null \
                     | head -1 | sed -E 's/.*:[[:space:]]*//')"
        fi
    else
        echo "  计算 PSNR / SSIM（${W}x${H}）..."
        ffmpeg -y -v error \
            -f rawvideo -pix_fmt yuv420p10le -s "${W}x${H}" -i "$ref" \
            -f rawvideo -pix_fmt yuv420p10le -s "${W}x${H}" -i "$yy" \
            -lavfi "[0:v][1:v]psnr=stats_file=$psnr_log;[0:v][1:v]ssim=stats_file=$ssim_log" -f null - || {
            echo "  错误: PSNR/SSIM 计算失败"; return 1; }
        score="$(grep -oE 'psnr_avg:(-?inf|[-0-9.]+)' "$psnr_log" | cut -d: -f2 | sed 's/-inf/0/; s/inf/100/' | awk '{s+=$1;n++} END{if(n>0) printf "%.3f", s/n}')"
        ssim_val="$(grep -oE 'All:(inf|[0-9.]+)' "$ssim_log" | cut -d: -f2 | sed 's/inf/1/' | awk '{s+=$1;n++} END{if(n>0) printf "%.5f", s/n}')"
    fi
    if [ -z "$score" ]; then
        echo "  错误: 无法解析质量评分" >&2
        return 1
    fi
    return 0
}

# ---------- 2. 首轮评估 ----------
echo "[2/4] 编码测试片段并评估..."
run_crf "$crf" "$mov" "$enc" "$vjson" || exit 2
s1="$score"
v1="$ssim_val"
if [ "$use_vmaf" -eq 1 ]; then
    echo "VMAF = $s1 (≥95 PASS / <95 FAIL → 建议 --crf 12 重试)"
else
    echo "PSNR = $s1 dB, SSIM = $v1（libvmaf 不可用，降级评估; 判据 PSNR≥42 且 SSIM≥0.97）"
fi

# ---------- 3. 不达标自动重试 crf 12 ----------
retry=0
if [ "$use_vmaf" -eq 1 ]; then
    lt "$s1" 95 && retry=1
else
    { lt "$s1" 42 || lt "$v1" 0.97; } && retry=1
fi
if [ "$retry" -eq 1 ]; then
    echo "[3/4] 未达标，自动以 --crf 12 重试..."
    run_crf 12 "$tmpdir/test12.mov" "$tmpdir/enc12.yuv" "$tmpdir/vmaf12.json" || exit 2
    if [ "$use_vmaf" -eq 1 ]; then
        echo "VMAF crf$crf = $s1 / crf12 = $score"
    else
        echo "PSNR crf$crf = $s1 / crf12 = $score, SSIM crf12 = $ssim_val"
    fi
else
    echo "[3/4] 首轮即达标，无需重试"
fi

# ---------- 4. 结论 ----------
echo "[4/4] 结论"
pass=0
if [ "$use_vmaf" -eq 1 ]; then
    ge "$score" 95 && pass=1
else
    { ge "$score" 42 && ge "$ssim_val" 0.97; } && pass=1
fi
if [ "$pass" -eq 1 ]; then
    echo "结果: PASS"
    exit 0
else
    echo "结果: FAIL（建议正式归档使用 --crf 12；仍不达标请检查源文件噪点/镜头或 --frames 覆盖范围）"
    exit 1
fi
