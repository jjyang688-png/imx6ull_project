#!/bin/bash
# Build script for imx6ull_project
# Usage: ./scripts/build.sh [modules|apps|all|clean]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

KERNEL_DIR="${KERNEL_DIR:-/home/alientek/linux/imx6ull}"
ARCH="${ARCH:-arm}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

export ARCH
export CROSS_COMPILE

cd "$PROJECT_DIR"

case "${1:-all}" in
    modules)
        echo "=== Building kernel modules ==="
        make -C driver KERNEL_DIR="$KERNEL_DIR"
        ;;
    apps)
        echo "=== Building user apps ==="
        make -C app CC="${CROSS_COMPILE}gcc"
        ;;
    all)
        echo "=== Building all (modules + apps) ==="
        make -C driver KERNEL_DIR="$KERNEL_DIR"
        make -C app CC="${CROSS_COMPILE}gcc"
        ;;
    clean)
        echo "=== Cleaning ==="
        make -C driver KERNEL_DIR="$KERNEL_DIR" clean
        make -C app clean
        ;;
    *)
        echo "Usage: $0 [modules|apps|all|clean]"
        exit 1
        ;;
esac

echo "=== Done ==="
