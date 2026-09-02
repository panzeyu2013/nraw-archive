#!/usr/bin/env bash
#
# verify.sh — 回读校验 nraw-archive 归档产物 xxx.h265.mov
#
# 用法:
#   verify.sh <xxx.h265.mov>
#
# 依赖: ffprobe、sha256sum；jq 有则用，无则 grep 兜底
# 任一检查失败 → 打印 VERIFY FAIL 并以非零退出
set -u

# ---------- 参数 ----------
mov="${1:-}"
if [ -z "$mov" ]; then
    echo "用法: verify.sh <xxx.h265.mov>" >&2
    exit 2
fi
if [ ! -f "$mov" ]; then
    echo "VERIFY FAIL: 文件不存在: $mov" >&2
    exit 1
fi

for t in ffprobe sha256sum; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "VERIFY FAIL: 缺少依赖命令: $t" >&2
        exit 1
    fi
done
JQ="$(command -v jq 2>/dev/null || true)"

# sidecar 命名规范：sidecar_<输出基名>.json（如 DSC_1775.mov → sidecar_DSC_1775.json）。
# 兼容旧命名 <输出>.sidecar.json：优先新命名，找不到再回退旧命名。
sidecar="$mov.sidecar.json"
base="$(basename "$mov")"
sidecar_new="$(dirname "$mov")/sidecar_${base%.*}.json"
if [ -f "$sidecar_new" ]; then
    sidecar="$sidecar_new"
fi
jf="$(mktemp /tmp/verify_probe_XXXXXX.json 2>/dev/null)" || jf="/tmp/verify_probe_$$.json"
trap 'rm -f "$jf"' EXIT

# ---------- 读取媒体信息 ----------
if ! ffprobe -v error \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,color_space,color_range,color_transfer,color_primaries,r_frame_rate,sample_rate,channels,bit_depth \
    -of json "$mov" >"$jf" 2>&1; then
    echo "VERIFY FAIL: ffprobe 无法读取: $mov" >&2
    cat "$jf" >&2
    exit 1
fi

# 取字段: json_get <jq 表达式> <grep 兜底键名>
json_get() {
    if [ -n "$JQ" ]; then
        jq -r "$1" "$jf" 2>/dev/null | head -1
    else
        grep -oE "\"$2\"[[:space:]]*:[[:space:]]*\"?[^\",}]+" "$jf" 2>/dev/null \
            | head -1 | sed -E 's/^[^:]*:[[:space:]]*"?//'
    fi
}

video_count=0
audio_count=0
vcodec=""
vprofile=""
vw=0
vh=0
vpix=""
vcs=""
vcr=""
arate=0
achan=0

if [ -n "$JQ" ]; then
    video_count=$(jq -r '[.streams[] | select(.width != null)] | length' "$jf" 2>/dev/null)
    audio_count=$(jq -r '[.streams[] | select(.sample_rate != null)] | length' "$jf" 2>/dev/null)
    if [ "${video_count:-0}" -gt 0 ]; then
        vcodec=$(json_get '[.streams[] | select(.width != null)][0].codec_name' codec_name)
        vprofile=$(json_get '[.streams[] | select(.width != null)][0].profile' profile)
        vw=$(json_get '[.streams[] | select(.width != null)][0].width' width)
        vh=$(json_get '[.streams[] | select(.width != null)][0].height' height)
        vpix=$(json_get '[.streams[] | select(.width != null)][0].pix_fmt' pix_fmt)
        vcs=$(json_get '[.streams[] | select(.width != null)][0].color_space' color_space)
        vcr=$(json_get '[.streams[] | select(.width != null)][0].color_range' color_range)
    fi
    if [ "${audio_count:-0}" -gt 0 ]; then
        arate=$(json_get '[.streams[] | select(.sample_rate != null)][0].sample_rate' sample_rate)
        achan=$(json_get '[.streams[] | select(.sample_rate != null)][0].channels' channels)
    fi
else
    video_count=$(grep -c '"width"' "$jf")
    audio_count=$(grep -c '"sample_rate"' "$jf")
    vcodec=$(json_get '' codec_name)
    vprofile=$(json_get '' profile)
    vw=$(json_get '' width)
    vh=$(json_get '' height)
    vpix=$(json_get '' pix_fmt)
    vcs=$(json_get '' color_space)
    vcr=$(json_get '' color_range)
    arate=$(json_get '' sample_rate)
    achan=$(json_get '' channels)
fi

# ---------- 逐项检查 ----------
fails=0
note()  { echo "  [通过] $1"; }
nfail() { echo "  [失败] $1"; fails=1; }

echo "== 校验: $mov =="

if [ "${video_count:-0}" -ge 1 ]; then
    note "视频流存在（共 $video_count 条）"
else
    nfail "缺少视频流"
fi

if [ "$vcodec" = "hevc" ]; then
    note "codec = hevc"
else
    nfail "codec 应为 hevc，实际: ${vcodec:-（无）}"
fi

case "$vprofile" in
    *"Main"*"10"*) note "profile = Main 10" ;;
    *) nfail "profile 应为 Main 10，实际: ${vprofile:-（无）}" ;;
esac

if [ "$vpix" = "yuv420p10le" ]; then
    note "pix_fmt = yuv420p10le"
else
    nfail "pix_fmt 应为 yuv420p10le，实际: ${vpix:-（无）}"
fi

case "$vcs" in
    bt2020nc|bt2020*) note "color_space = $vcs（要求 bt2020nc）" ;;
    *) nfail "color_space 应为 bt2020nc，实际: ${vcs:-（无）}" ;;
esac

case "$vcr" in
    pc)   note "color_range = pc" ;;
    jpeg) note "color_range = jpeg（等价于 pc，视为通过）" ;;
    *)    nfail "color_range 应为 pc（ffprobe 报 jpeg 亦视为通过），实际: ${vcr:-（无）}" ;;
esac

if [ "${vw:-0}" -gt 0 ] && [ "${vh:-0}" -gt 0 ]; then
    note "分辨率 ${vw}x${vh}"
else
    nfail "width/height 无效: ${vw}x${vh}"
fi

if [ "${audio_count:-0}" -ge 1 ]; then
    if [ "${arate:-0}" -ge 44000 ] && [ "${arate:-0}" -le 52000 ]; then
        note "采样率 ${arate} Hz（48000 附近）"
    else
        nfail "采样率不在 48000 附近: ${arate:-（无）} Hz"
    fi
    if [ "${achan:-0}" -ge 1 ]; then
        note "声道数 ${achan}"
    else
        nfail "声道数异常: ${achan:-0}"
    fi
else
    echo "  [信息] 无音轨，跳过音频检查"
fi

# ---------- sidecar 校验 ----------
sha_get() {  # $1 = sidecar 路径
    local f="$1" v="" k=""
    if [ -n "$JQ" ]; then
        for k in sha256 sha_256 sha-256 hash; do
            v="$(jq -r --arg k "$k" '.. | objects | .[$k]? // empty' "$f" 2>/dev/null | head -1)"
            [ -n "$v" ] && { echo "$v"; return 0; }
        done
    else
        v="$(grep -oiE '"(sha256|sha_256|sha-256|hash)"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' "$f" \
            | grep -oiE '[0-9a-f]{64}' | head -1)"
        [ -n "$v" ] && { echo "$v"; return 0; }
    fi
    return 1
}

sidecar_field() {  # $1 = sidecar 路径, $2 = 键名（兼容带引号字符串 / 裸数字 / null）
    local f="$1" k="$2" v=""
    if [ -n "$JQ" ]; then
        v="$(jq -r --arg k "$k" '.. | objects | .[$k]? // empty' "$f" 2>/dev/null | head -1)"
    else
        v="$(grep -oE "\"$k\"[[:space:]]*:[[:space:]]*(\"[^\"]*\"|[0-9.eE+-]+|null)" "$f" 2>/dev/null \
            | head -1 | sed -E 's/^[^:]*:[[:space:]]*"?//; s/"$//')"
    fi
    [ -n "$v" ] && echo "$v"
}

sidecar_has() {  # $1 = sidecar 路径, $2 = 键名（键存在即为真，null 也算存在）
    local f="$1" k="$2"
    if [ -n "$JQ" ]; then
        [ "$(jq -r --arg k "$k" 'if has($k) then "yes" else "no" end' "$f" 2>/dev/null)" = "yes" ]
    else
        grep -q "\"$k\"" "$f" 2>/dev/null
    fi
}

if [ -f "$sidecar" ]; then
    echo "== sidecar: $sidecar =="
    sha_s="$(sha_get "$sidecar" || true)"
    if [ -n "$sha_s" ]; then
        # sidecar 记录的是源 NEV 的哈希；优先按 source 字段找源文件核对
        matched=0
        found=0
        for k in input_file input_file_path source_file source filename; do
            src="$(sidecar_field "$sidecar" "$k" || true)"
            [ -n "$src" ] || continue
            [ -f "$src" ] || src="$(dirname "$mov")/$(basename "$src")"
            if [ -f "$src" ]; then
                found=1
                sha_src="$(sha256sum "$src" | awk '{print $1}')"
                if [ "$sha_src" = "$sha_s" ]; then
                    note "SHA256 匹配（源文件 $src）"
                    matched=1
                    break
                fi
            fi
        done
        if [ "$matched" -eq 0 ] && [ "$found" -eq 1 ]; then
            nfail "SHA256 失配: sidecar=${sha_s:0:16}…，源文件哈希不一致"
        elif [ "$matched" -eq 0 ]; then
            echo "  [警告] 无法找到源文件核对 sidecar 的 SHA256（sidecar=${sha_s:0:16}…）"
        fi
    else
        echo "  [警告] sidecar 中未找到 sha256 字段，跳过哈希校验"
    fi
    while read -r key; do
        if grep -qi "\"$key\"" "$sidecar"; then
            note "字段 $key 存在"
        else
            nfail "sidecar 缺少 $key 字段"
        fi
    done <<EOF
crf
keyint
matrix
EOF
    dec_path="$(sidecar_field "$sidecar" decode_path || true)"
    case "$dec_path" in
        gpu|cpu) note "decode_path = $dec_path" ;;
        *) nfail "decode_path 应为 gpu|cpu，实际: ${dec_path:-（无）}" ;;
    esac
    if [ "$dec_path" = "gpu" ]; then
        gpu_dev="$(sidecar_field "$sidecar" gpu_device || true)"
        if [ -n "$gpu_dev" ]; then
            note "gpu_device = $gpu_dev"
        else
            nfail "decode_path=gpu 但 sidecar 缺少 gpu_device"
        fi
        if sidecar_has "$sidecar" gate_psnr_db; then
            gate_psnr="$(sidecar_field "$sidecar" gate_psnr_db || true)"
            if [ -n "$gate_psnr" ]; then
                note "gate_psnr_db = $gate_psnr"
            else
                note "gate_psnr_db = null（门控未执行/无数据）"
            fi
        else
            nfail "decode_path=gpu 但 sidecar 缺少 gate_psnr_db 字段"
        fi
    fi
else
    echo "  [信息] 未找到 sidecar（$sidecar），跳过哈希与编码参数校验"
fi

# ---------- 结论 ----------
echo "----------------------------------------"
if [ "$fails" -eq 0 ]; then
    echo "VERIFY PASS"
    exit 0
else
    echo "VERIFY FAIL"
    exit 1
fi
