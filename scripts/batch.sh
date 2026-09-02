#!/usr/bin/env bash
#
# batch.sh — 目录批处理：遍历目录下 *.NEV / *.nev，逐个调用 nraw-archive
#
# 用法:
#   batch.sh <目录> [nraw-archive 额外参数...]
#     --force    已存在同名 .h265.mov 时也强制重做
#     --dry-run  只打印将要执行的命令，不实际运行
#
# 说明:
#   - 额外参数原样透传；自动过滤 --output/-o 及其取值、忽略多余位置参数
#     （输出固定为源文件同目录的 同名.h265.mov）
#   - 单文件失败不会中断批处理：打印错误、记入 <目录>/batch_failures.log，
#     继续处理下一个文件
#   - 结束打印成功/失败/跳过统计与总耗时（date +%s 差值）
set -u

# ---------- 定位 nraw-archive 可执行文件 ----------
source "$(cd "$(dirname "$0")" && pwd)/common.sh"

BIN="$(find_bin)" || {
    echo "错误: 找不到 nraw-archive 可执行文件" >&2
    echo "       可设置环境变量 NRAW_ARCHIVE=/路径/nraw-archive 指定" >&2
    exit 1
}

# ---------- 解析参数 ----------
dir=""
force=0
dry=0
extra=()
skip_next=0
skip_drop=0
last_opt=""
value_opts="--kelvin --tint --iso --exposure --lens-correction --chroma-nr --decode --crf --preset --keyint --min-keyint --open-gop --encoders --decoders --jobs --buffers --frames --worker-batch --sdk-path --output -o"
reject_opts="--dump-ref"
for a in "$@"; do
    if [ "$skip_drop" -eq 1 ]; then
        skip_drop=0
        continue
    fi
    if [ "$skip_next" -eq 1 ]; then
        skip_next=0
        extra+=("$a")
        continue
    fi
    case "$a" in
        --force)      force=1 ;;
        --dry-run)    dry=1 ;;
        --output|-o)
            echo "警告: nraw-archive 无 --output/-o 选项（输出为位置参数），已忽略；批处理模式自动生成输出路径" >&2
            skip_drop=1
            last_opt="$a" ;;
        --output=*|-o=*)
            echo "警告: nraw-archive 无 --output/-o 选项（输出为位置参数），已忽略；批处理模式自动生成输出路径" >&2 ;;
        --dump-ref=*)
            echo "警告: 忽略 --dump-ref（批处理模式不支持参考输出）" >&2 ;;
        --*=*)        extra+=("$a") ;;
        --*)
            case " $reject_opts " in
                *" $a "*)
                    echo "警告: 忽略 $a（批处理模式不支持该参数及其值）" >&2
                    skip_drop=1
                    last_opt="$a"
                    ;;
                *)
                    extra+=("$a")
                    case " $value_opts " in
                        *" $a "*) skip_next=1; last_opt="$a" ;;
                    esac
                    ;;
            esac
            ;;
        *)
            if [ -z "$dir" ]; then
                dir="$a"
            else
                echo "警告: 忽略多余位置参数 '$a'（输出路径由脚本自动生成）" >&2
            fi
            ;;
    esac
done

if [ "$skip_next" -eq 1 ] || [ "$skip_drop" -eq 1 ]; then
    echo "错误: 参数 $last_opt 缺少取值" >&2
    exit 2
fi

if [ -z "$dir" ]; then
    echo "用法: batch.sh <目录> [nraw-archive 额外参数...]" >&2
    exit 2
fi
if [ ! -d "$dir" ]; then
    echo "错误: 目录不存在: $dir" >&2
    exit 2
fi

# ---------- 收集待处理文件 ----------
shopt -s nullglob
files=("$dir"/*.NEV "$dir"/*.nev)
if [ "${#files[@]}" -eq 0 ]; then
    echo "目录 $dir 下未找到 .NEV/.nev 文件"
    exit 1
fi

# ---------- 主循环 ----------
log_tmp="$(mktemp /tmp/batch_log_XXXXXX 2>/dev/null)" || log_tmp="/tmp/batch_log_$$"
fail_log="$dir/batch_failures.log"
trap 'rm -f "$log_tmp"' EXIT

start=$(date +%s)
total=${#files[@]}
ok=0
fail=0
skipped=0
i=0

for f in "${files[@]}"; do
    i=$((i + 1))
    if [ ! -f "$f" ]; then
        echo "[$i/$total] $(basename "$f") ... 跳过（非普通文件）"
        continue
    fi
    base="$(basename "$f")"
    out="$dir/${base%.*}.h265.mov"

    if [ -f "$out" ] && [ "$force" -eq 0 ]; then
        # sidecar 命名：工具只剥最后一个扩展名 → sidecar_<基名>.h265.json
        if [ -f "$dir/sidecar_${base%.*}.h265.json" ]; then
            echo "[$i/$total] $base ... 已存在 $(basename "$out")，跳过（--force 强制重做）"
            skipped=$((skipped + 1))
            continue
        fi
        echo "[$i/$total] $base ... 发现无 sidecar 的残留 $(basename "$out")，重新处理"
    fi

    echo "[$i/$total] $base ..."
    if [ "$dry" -eq 1 ]; then
        echo "  $BIN ${extra[*]} \"$f\" \"$out\""
        ok=$((ok + 1))
        continue
    fi

    if "$BIN" "${extra[@]}" "$f" "$out" >"$log_tmp" 2>&1; then
        ok=$((ok + 1))
        echo "  完成 → $out"
    else
        fail=$((fail + 1))
        echo "  错误: 处理失败（明细见 batch_failures.log，下方为最后 3 行输出）"
        tail -n 3 "$log_tmp" 2>/dev/null | sed 's/^/    /'
        echo "$(date '+%Y-%m-%d %H:%M:%S') [失败] 文件: $f" >>"$fail_log"
        echo "  命令: $BIN ${extra[*]} \"$f\" \"$out\"" >>"$fail_log"
        echo "  输出:" >>"$fail_log"
        tail -n 5 "$log_tmp" 2>/dev/null | sed 's/^/    /' >>"$fail_log"
        echo >>"$fail_log"
    fi
done

end=$(date +%s)

# ---------- 统计 ----------
echo "----------------------------------------"
if [ "$dry" -eq 1 ]; then
    echo "dry-run 模拟完成: 将处理 $ok 个 / 跳过 $skipped 个 / 共 $total，总耗时 $((end - start))s"
else
    echo "批处理完成: 成功 $ok / 失败 $fail / 跳过 $skipped / 共 $total，总耗时 $((end - start))s"
    if [ "$fail" -gt 0 ]; then
        echo "失败明细已记录: $fail_log"
        exit 1
    fi
fi
exit 0
