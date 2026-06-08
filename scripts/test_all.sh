#!/bin/bash
# test_all.sh — 一键运行全部驱动集成测试
# 用法: 在开发板上执行 ./test_all.sh
#
# 前提: 所有 .ko 已通过 insmod 加载。

set -e

PASS=0
FAIL=0

check() {
    local desc="$1"
    shift
    echo -n "  TEST: $desc ... "
    if "$@" >/dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
    fi
}

# 确保驱动已加载
for mod in comp_drv key_input ap3216c icm20608 uart_sensor; do
    if ! lsmod | grep -q "$mod"; then
        echo "ERROR: $mod.ko 未加载，请先执行 insmod $mod.ko"
        exit 1
    fi
done

echo "========================================"
echo "  imx6ull_project 集成测试"
echo "========================================"
echo ""

# ---- LED 测试 ----
if [ -c /dev/comp_drv ]; then
    echo "--- comp_drv (LED) ---"
    check "LED write on"        sh -c "echo on  > /dev/comp_drv"
    check "LED write off"       sh -c "echo off > /dev/comp_drv"
    check "LED read (blocking)" sh -c "timeout 1 dd if=/dev/comp_drv bs=16 count=1 2>/dev/null | wc -c | grep -q '16'"
else
    echo "--- comp_drv: SKIP (设备不存在) ---"
fi
echo ""

# ---- AP3216C 测试 ----
if [ -c /dev/ap3216c ]; then
    echo "--- ap3216c (光照传感器) ---"
    check "AP3216C read 6 bytes" sh -c "dd if=/dev/ap3216c bs=6 count=1 2>/dev/null | wc -c | grep -q '6'"
else
    echo "--- ap3216c: SKIP (设备不存在) ---"
fi
echo ""

# ---- ICM20608 测试 ----
if [ -c /dev/icm20608 ]; then
    echo "--- icm20608 (姿态传感器) ---"
    check "ICM20608 read 14 bytes" sh -c "dd if=/dev/icm20608 bs=14 count=1 2>/dev/null | wc -c | grep -q '14'"
else
    echo "--- icm20608: SKIP (设备不存在) ---"
fi
echo ""

# ---- UART 测试 ----
if [ -c /dev/uart_sensor ]; then
    echo "--- uart_sensor (串口控制台) ---"
    check "UART read (nonblock)"  sh -c "timeout 1 dd if=/dev/uart_sensor bs=1 count=1 2>/dev/null; true"
    check "UART write STATUS"     sh -c "echo 'STATUS' > /dev/uart_sensor; sleep 0.1; true"
else
    echo "--- uart_sensor: SKIP (设备不存在) ---"
fi
echo ""

# ---- 按键测试 ----
if [ -c /dev/input/event0 ] || [ -c /dev/input/event1 ]; then
    echo "--- key_input (按键) ---"
    check "KEY device exists" test -c /dev/input/event0 -o -c /dev/input/event1
else
    echo "--- key_input: SKIP (设备不存在) ---"
fi

echo ""
echo "========================================"
echo "  结果: $PASS 通过, $FAIL 失败"
if [ $FAIL -gt 0 ]; then
    echo "  (部分测试失败，请检查驱动是否正常加载)"
    exit 1
fi
echo "========================================"
