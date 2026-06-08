#!/bin/bash
# checkpatch.sh — 运行内核 checkpatch.pl 检查驱动代码风格
# 用法: ./scripts/checkpatch.sh
#
# 环境变量:
#   KERNEL_DIR — 内核源码树路径（含 scripts/checkpatch.pl）

KERNEL_DIR="${KERNEL_DIR:-/home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek}"

CHECKPATCH="${KERNEL_DIR}/scripts/checkpatch.pl"

if [ ! -x "$CHECKPATCH" ]; then
    echo "ERROR: checkpatch.pl 未找到 ($CHECKPATCH)"
    echo "请设置 KERNEL_DIR 环境变量，例如:"
    echo "  export KERNEL_DIR=/path/to/linux"
    echo "  ./scripts/checkpatch.sh"
    exit 1
fi

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== 运行 checkpatch.pl --strict ==="
echo "内核源码: ${KERNEL_DIR}"
echo ""

TOTAL_WARN=0
TOTAL_ERR=0

for f in "$PROJECT_DIR"/driver/*.c "$PROJECT_DIR"/driver/*.h; do
    [ -f "$f" ] || continue

    fname=$(basename "$f")
    echo "--- ${fname} ---"

    # --strict: 严格模式
    # --no-tree: 不在内核树中运行
    # -f: 检查单个文件
    "$CHECKPATCH" --strict --no-tree -f "$f" || true

    echo ""
done

echo "=== 完成 ==="
echo "注意: 80 列超长警告可以接受，其余应尽量修复。"
