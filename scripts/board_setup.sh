#!/bin/sh
# board_setup.sh — 加载所有 v1.0 驱动 (v2 — 适配新DTB)
echo "=== Checking DTB ==="
cat /proc/device-tree/model 2>/dev/null
echo ""

echo "=== Checking for conflicting drivers ==="
# 检查 gpio-keys 是否加载（与 key_input 冲突 GPIO1_IO18）
if [ -d /sys/bus/platform/drivers/gpio-keys ]; then
    echo "gpio-keys found — unbinding..."
    ls /sys/bus/platform/drivers/gpio-keys/ | grep -v "unbind\|bind\|uevent" | while read dev; do
        echo -n "$dev" > /sys/bus/platform/drivers/gpio-keys/unbind 2>/dev/null && echo "  unbind $dev OK" || echo "  unbind $dev FAIL"
    done
else
    echo "gpio-keys not loaded — OK"
fi

# 检查 imx-uart 是否占用 UART3（021ec000.serial）
if [ -d /sys/bus/platform/drivers/imx-uart ]; then
    echo "imx-uart found — checking for 21ec000..."
    if [ -d /sys/bus/platform/drivers/imx-uart/21ec000.serial ]; then
        echo "UART3 claimed by imx-uart — unbinding..."
        timeout 3 sh -c 'echo -n 21ec000.serial > /sys/bus/platform/drivers/imx-uart/unbind' 2>/dev/null
        echo "  done"
    else
        echo "UART3 not claimed by imx-uart — OK"
    fi
else
    echo "imx-uart not loaded — OK"
fi

echo ""
echo "=== Unloading old modules ==="
for mod in uart_sensor icm20608 ap3216c key_input comp_drv; do
    if lsmod | grep -q "^$mod "; then
        rmmod $mod 2>/dev/null && echo "$mod removed" || echo "$mod remove FAIL (may be busy)"
    fi
done

echo ""
echo "=== Loading drivers ==="
cd /lib/modules/4.1.15
insmod comp_drv.ko 2>&1    && echo "comp_drv   OK" || echo "comp_drv   FAIL: $?"
insmod key_input.ko 2>&1   && echo "key_input  OK" || echo "key_input  FAIL: $?"
insmod ap3216c.ko 2>&1     && echo "ap3216c   OK" || echo "ap3216c   FAIL: $?"
insmod icm20608.ko 2>&1    && echo "icm20608  OK" || echo "icm20608  FAIL: $?"
insmod uart_sensor.ko 2>&1 && echo "uart_sensor OK" || echo "uart_sensor FAIL: $?"

echo ""
echo "=== Device nodes ==="
ls -la /dev/comp_drv /dev/ap3216c /dev/icm20608 /dev/input/event* /dev/uart_sensor 2>&1

echo ""
echo "=== dmesg tail ==="
dmesg | grep -E "comp_drv|key_input|ap3216c|icm20608|uart_sensor" | tail -15

echo ""
echo "=== DONE ==="
