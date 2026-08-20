#!/usr/bin/env bash
# test_roundtrip.sh — ctest 注册的端到端回路测试
# 用法: test_roundtrip.sh <nraw_test_encode 路径>
# 通过条件: 编码 rc=0、SHA-256 向量/文件哈希一致、ffprobe 规格正确、PSNR>=45、音频字节级一致、
#           repair-test（模拟 moov 丢失后自动重建，重建文件帧数/流完整）
set -u
# 清除可能残留的测试环境变量（否则变体静默退化/污染）
unset NRAW_TEST_OPEN_GOP NRAW_HARD_INT NRAW_KEEP_RESUME NRAW_SINGLE_RESUME \
    NRAW_KEEP_REPAIR NRAW_LOG_DUMP NRAW_TEST_FASTSTART 2>/dev/null || true

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

# 音频参考先行生成（resume 内容级验证也使用）
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

# resume 产物内容级校验（120 帧 + pts 序列 + 逐帧 PSNR + 音频字节一致）。
# 重放部分是与阶段1 lookahead 上下文一致的原始编码，与全新编码逐帧比对
# 无意义；pts 集合校验是捕获帧缺失/重复/错序的关键断言。
# 注意：函数必须先定义再调用（repair-test 之后立刻用到）。
check_resume_content() {
    local f="$1" label="$2" tag
    tag="$(basename "$f" .mov)"
    [ "$(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of csv=p=0 "$f")" = "120" ] \
        || { echo "FAIL: $label 帧数 != 120"; exit 1; }
    local pts
    pts="$(ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 "$f" | sort -n)"
    printf '%s\n' "$pts" | awk 'NR==1&&$1!=0{f=1} NR>1&&$1!=s+1001{f=1} {s=$1;n++} END{exit (f||n!=120)?1:0}' \
        || { echo "FAIL: $label pts 非 {i*1001}"; exit 1; }
    ffmpeg -v error -xerror -i "$f" -f null - 2>/dev/null \
        || { echo "FAIL: $label 解码错误"; exit 1; }
    ffmpeg -y -v error -xerror -i "$f" -f rawvideo -pix_fmt yuv420p10le "$TMP/${tag}_dec.yuv" \
        || { echo "FAIL: $label 解码"; exit 1; }
    ffmpeg -v error -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/ref.yuv" \
        -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/${tag}_dec.yuv" \
        -lavfi "[0:v][1:v]psnr=stats_file=$TMP/${tag}_p.log" -f null - 2>/dev/null
    local pmin
    pmin="$(awk '/psnr_avg:/{for(i=1;i<=NF;i++)if($i~/^psnr_avg:/){v=substr($i,10)+0; if(min==""||v<min)min=v}} END{print min}' "$TMP/${tag}_p.log")"
    [ -n "$pmin" ] && awk -v p="$pmin" 'BEGIN{exit !(p>=45)}' \
        || { echo "FAIL: $label 逐帧 PSNR 过低 $pmin"; exit 1; }
    ffmpeg -y -v error -i "$f" -vn -f s24le "$TMP/${tag}_au.raw" 2>/dev/null \
        || { echo "FAIL: $label 音频解码"; exit 1; }
    cmp -s "$TMP/${tag}_au.raw" "$TMP/au_ref.raw" || { echo "FAIL: $label 音频不一致"; exit 1; }
}

"$BIN" repair-test "$TMP/repair.mov" 2>/dev/null || { echo "FAIL: repair-test 模式"; exit 1; }
# 内容级校验必须针对重建产物（repair.mov.recovered）：repair.mov 是原始编码
# 输出（moov 完好），重建文件在测试内部另存为 .recovered——NRAW_KEEP_REPAIR=1
# 保留它供此处校验（$TMP 由 trap 清理，无磁盘污染）
NRAW_KEEP_REPAIR=1 "$BIN" repair-test "$TMP/repair_keep.mov" 2>/dev/null \
    || { echo "FAIL: repair-test 模式"; exit 1; }
check_resume_content "$TMP/repair_keep.mov.recovered" "repair 重建产物"
"$BIN" resume-test "$TMP/resume.mov" 2>/dev/null || { echo "FAIL: resume-test 模式"; exit 1; }

# 内置 SHA-256 与系统 sha256sum 交叉校验（sha256sum 缺失时跳过该检查）
if command -v sha256sum >/dev/null 2>&1; then
    H1="$("$BIN" sha256 "$TMP/test.mov" 2>/dev/null)"
    H2="$(sha256sum "$TMP/test.mov" | awk '{print $1}')"
    [ -n "$H1" ] && [ "$H1" = "$H2" ] || { echo "FAIL: sha256 文件哈希不一致"; exit 1; }
fi

# open-GOP（默认配置）续传变体：回退 17 帧的引用封闭必须验证
OG_LOG="$(NRAW_TEST_OPEN_GOP=1 NRAW_KEEP_RESUME=1 "$BIN" resume-test "$TMP/resume_og.mov" 2>&1)"
printf '%s\n' "$OG_LOG" | grep -q "PASS: resume-test" \
    || { echo "FAIL: open-GOP 续传"; exit 1; }
# 数值断言：closed-GOP 续传点=60（最后关键帧），open-GOP 必须真正回退到 <60，
# 否则说明 NRAW_TEST_OPEN_GOP 静默失效（变体未执行）
og_resume="$(printf '%s\n' "$OG_LOG" | sed -nE 's/.*回退关键帧 ([0-9]+).*/\1/p' | head -1)"
[ -n "$og_resume" ] && [ "$og_resume" -lt 60 ] \
    || { echo "FAIL: open-GOP 续传点 '$og_resume' 未回退（应 <60，变体未执行？）"; exit 1; }
check_resume_content "$TMP/resume_og.mov" "open-GOP 续传"
# 硬中断（模拟真实 SIGINT 不 flush）续传变体：靠 countCompleteSamples 校准
HI_LOG="$(NRAW_HARD_INT=1 NRAW_KEEP_RESUME=1 "$BIN" resume-test "$TMP/resume_hi.mov" 2>&1)"
printf '%s\n' "$HI_LOG" | grep -q "PASS: resume-test" \
    || { echo "FAIL: 硬中断续传"; exit 1; }
printf '%s\n' "$HI_LOG" | grep -q "硬中断" \
    || { echo "FAIL: 硬中断续传未走硬中断路径（变体未执行？）"; exit 1; }
check_resume_content "$TMP/resume_hi.mov" "硬中断续传"
# closed-GOP 默认续传
check_resume_content "$TMP/resume.mov" "resume"
# 单次续传（一次中断 → 一次续传直达完成）：三阶段链会掩盖中间接缝（下一阶段
# 从更早处重编码、丢弃损坏数据），单次续传是真实使用场景，直接验证续传点
# 接缝的引用封闭（-xerror 解码）、pts 连续性（{i*1001}）与音频字节一致
S_LOG="$(NRAW_SINGLE_RESUME=1 "$BIN" resume-test "$TMP/resume_s.mov" 2>&1)"
printf '%s\n' "$S_LOG" | grep -q "PASS: resume-test 单次续传完成" \
    || { echo "FAIL: 单次续传（closed-GOP）"; exit 1; }
check_resume_content "$TMP/resume_s.mov" "单次续传"
OGS_LOG="$(NRAW_TEST_OPEN_GOP=1 NRAW_SINGLE_RESUME=1 "$BIN" resume-test "$TMP/resume_sog.mov" 2>&1)"
printf '%s\n' "$OGS_LOG" | grep -q "PASS: resume-test 单次续传完成" \
    || { echo "FAIL: 单次续传（open-GOP）"; exit 1; }
printf '%s\n' "$OGS_LOG" | grep -q "偏移" \
    && { echo "FAIL: open-GOP 单次续传发生时间轴平移（应无偏移）"; exit 1; }
check_resume_content "$TMP/resume_sog.mov" "单次续传 open-GOP"
# faststart 单次续传变体：收尾 moov 前置重写 + 续传的交互（moov 必须在文件头）
FS_LOG="$(NRAW_TEST_FASTSTART=1 NRAW_SINGLE_RESUME=1 "$BIN" resume-test "$TMP/resume_fs.mov" 2>&1)"
printf '%s\n' "$FS_LOG" | grep -q "PASS: resume-test 单次续传完成" \
    || { echo "FAIL: 单次续传（faststart）"; exit 1; }
[ "$(head -c 64 "$TMP/resume_fs.mov" | grep -c moov)" = "1" ] \
    || { echo "FAIL: faststart 产物 moov 不在文件头"; exit 1; }
check_resume_content "$TMP/resume_fs.mov" "单次续传 faststart"
# 早期中断变体（vc==0 分支）：硬中断 + 极小帧数 → 无完整样本 → 全新编码续跑
EI_LOG="$(NRAW_TEST_EARLY_INT=1 NRAW_HARD_INT=1 "$BIN" resume-test "$TMP/resume_ei.mov" 2>&1)"
printf '%s\n' "$EI_LOG" | grep -q "PASS: resume-test 早期中断" \
    || { echo "FAIL: 早期中断变体（vc==0）"; exit 1; }
check_resume_content "$TMP/resume_ei.mov" "早期中断续跑"

SPEC="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,pix_fmt,color_space,color_range -of csv=p=0 "$TMP/test.mov")"
case "$SPEC" in
    hevc,"Main 10",yuv420p10le,*)
        echo "$SPEC" | grep -q bt2020nc || { echo "FAIL: 缺 bt2020nc: $SPEC"; exit 1; }
        echo "$SPEC" | grep -q "pc" || { echo "FAIL: 缺 full range: $SPEC"; exit 1; }
        ;;
    *) echo "FAIL: 规格不符: $SPEC"; exit 1 ;;
esac

ffmpeg -y -v error -xerror -i "$TMP/test.mov" -f rawvideo -pix_fmt yuv420p10le "$TMP/dec.yuv" || { echo "FAIL: 解码"; exit 1; }
ffmpeg -v error -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/ref.yuv" \
    -f rawvideo -pix_fmt yuv420p10le -s 640x360 -i "$TMP/dec.yuv" \
    -lavfi "[0:v][1:v]psnr=stats_file=$TMP/p.log;[0:v][1:v]ssim=stats_file=$TMP/s.log" \
    -f null - 2>/dev/null
PSNR_MIN="$(awk '/psnr_avg:/{for(i=1;i<=NF;i++)if($i~/^psnr_avg:/){v=substr($i,10)+0; if(min==""||v<min)min=v}} END{print min}' "$TMP/p.log")"
[ -n "$PSNR_MIN" ] && awk -v p="$PSNR_MIN" 'BEGIN{exit !(p>=45)}' \
    || { echo "FAIL: 逐帧 PSNR 过低 $PSNR_MIN"; exit 1; }
PSNR="$(grep -oE 'psnr_avg:[-0-9.]+' "$TMP/p.log" | tail -1 | cut -d: -f2)"
awk -v p="$PSNR" 'BEGIN{exit !(p>=45)}' || { echo "FAIL: PSNR 偏低 $PSNR"; exit 1; }


ffmpeg -y -v error -i "$TMP/test.mov" -vn -f s24le "$TMP/au.raw" 2>/dev/null || { echo "FAIL: 音频解码"; exit 1; }
cmp -s "$TMP/au.raw" "$TMP/au_ref.raw" || { echo "FAIL: 音频不一致"; exit 1; }

echo "PASS: 回路测试通过 (PSNR=$PSNR)"
exit 0
