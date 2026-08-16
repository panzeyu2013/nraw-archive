#!/usr/bin/env bash
# common.sh — batch.sh / vmaf_test.sh 共用函数库
# 用法: source "$(cd "$(dirname "$0")" && pwd)/common.sh"（脚本自身与 common.sh 同目录）
#
# 说明: 依赖 set -u 安全；函数内不引用调用方未定义的变量。

# ---------- 定位 nraw-archive 可执行文件 ----------
# 查找顺序: $NRAW_ARCHIVE 环境变量 → 自身同目录 → 上一级 → ../build → PATH
find_bin() {
    if [ -n "${NRAW_ARCHIVE:-}" ] && [ -x "${NRAW_ARCHIVE:-}" ]; then
        echo "$NRAW_ARCHIVE"
        return 0
    fi
    local dir cand
    dir="$(cd "$(dirname "$0")" && pwd)"
    for cand in \
        "$dir/nraw-archive" "$dir/nraw_archive" \
        "$dir/../nraw-archive" "$dir/../nraw_archive" \
        "$dir/../build/nraw-archive" "$dir/../build/nraw_archive"; do
        if [ -x "$cand" ]; then
            echo "$cand"
            return 0
        fi
    done
    if cand="$(command -v nraw-archive 2>/dev/null || command -v nraw_archive 2>/dev/null)"; then
        echo "$cand"
        return 0
    fi
    return 1
}
