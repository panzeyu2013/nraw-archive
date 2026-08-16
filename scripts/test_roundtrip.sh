#!/usr/bin/env bash
# test_roundtrip.sh — ctest 注册的端到端回路测试
# 用法: test_roundtrip.sh <nraw_test_encode 路径>
# 通过条件: 编码 rc=0、SHA-256 向量/文件哈希一致、ffprobe 规格正确、PSNR>=45、音频字节级一致
set -u

BIN="${1:-}"
[ -x "$BIN" ] || { echo "FAIL: 缺少测试二进制"; exit 1; }
for t in ffmpeg ffprobe python3; do
    command -v "$t" >/dev/null 2>&1 || { echo "FAIL: 缺少 $t"; exit 1; }
done

TMP="$(mktemp -d /tmp/nraw_rt_XXXXXX 2>/dev/null)" || exit 1
trap 'rm -rf "$TMP"' EXIT

"$BIN" repack 2>/dev/null || { echo "FAIL: repack 模式"; exit 1; }
"$BIN" sha256 2>/dev/null || { echo "FAIL: sha256 向量"; exit 1; }
"$BIN" dump "$TMP/ref.yuv" 2>/dev/null || { echo "FAIL: dump 模式"; exit 1; }
"$BIN" encode "$TMP/test.mov" 2>/dev/null || { echo "FAIL: encode 模式"; exit 1; }

# 内置 SHA-256 与系统 sha256sum 交叉校验（sha256sum 缺失时跳过该检查）
if command -v sha256sum >/dev/null 2>&1; then
    H1="$("$BIN" sha256 "$TMP/test.mov" 2>/dev/null)"
    H2="$(sha256sum "$TMP/test.mov" | awk '{print $1}')"
    [ -n "$H1" ] && [ "$H1" = "$H2" ] || { echo "FAIL: sha256 文件哈希不一致"; exit 1; }
fi

SPEC="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,color_space,color_range -of csv=p=0 "$TMP/test.mov")"
case "$SPEC" in
    hevc,"Main 10",yuv420p10le,*)
        echo "$SPEC" | grep -q bt2020nc || { echo "FAIL: 缺 bt2020nc: $SPEC"; exit 1; }
        echo "$SPEC" | grep -q "pc" || { echo "FAIL: 缺 full range: $SPEC"; exit 1; }
        ;;
    *) echo "FAIL: 规格不符: $SPEC"; exit 1 ;;
esac

ffmpeg -y -v error -i "$TMP/test.mov" -f rawvideo -pix_fmt yuv420p10le "$TMP/dec.yuv" || { echo "FAIL: 解码"; exit 1; }
ffmpeg -v error -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/ref.yuv" \
    -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/dec.yuv" \
    -lavfi "[0:v][1:v]psnr=stats_file=$TMP/p.log;[0:v][1:v]ssim=stats_file=$TMP/s.log" \
    -f null - 2>/dev/null
PSNR="$(grep -oE 'psnr_avg:[-0-9.]+' "$TMP/p.log" | cut -d: -f2 | awk '{s+=$1;n++} END{printf "%.2f", s/n}')"
awk -v p="$PSNR" 'BEGIN{exit !(p>=45)}' || { echo "FAIL: PSNR 偏低 $PSNR"; exit 1; }

ffmpeg -y -v error -i "$TMP/test.mov" -vn -f s24le "$TMP/au.raw" 2>/dev/null || { echo "FAIL: 音频解码"; exit 1; }
python3 - "$TMP/au_ref.raw" <<'EOF'
import sys
data = bytearray()
for firstSample in range(0, 96000, 4800):
    for k in range(4800):
        g = firstSample + k
        for c in range(2):
            v = (g * 2654435761 + c) & 0xFFFFFF
            data += bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])
open(sys.argv[1], 'wb').write(data)
EOF
cmp -s "$TMP/au.raw" "$TMP/au_ref.raw" || { echo "FAIL: 音频不一致"; exit 1; }

echo "PASS: 回路测试通过 (PSNR=$PSNR)"
exit 0
