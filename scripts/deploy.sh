#!/bin/bash
# deploy.sh — 部署到 i.MX6ULL 开发板
# 用法: ./scripts/deploy.sh [board_ip]
#
# 默认 IP: 192.168.80.100，可通过参数覆盖。

set -e

BOARD_IP="${1:-192.168.80.100}"
BOARD_USER="root"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== 部署到开发板 ${BOARD_IP} ==="

# 传输内核模块
echo "--- 传输驱动模块 ---"
scp "${PROJECT_DIR}"/driver/*.ko ${BOARD_USER}@${BOARD_IP}:/lib/modules/4.1.15/

# 传输应用程序
echo "--- 传输应用程序 ---"
scp "${PROJECT_DIR}"/app/smart_monitor ${BOARD_USER}@${BOARD_IP}:/usr/bin/
scp "${PROJECT_DIR}"/app/test_*     ${BOARD_USER}@${BOARD_IP}:/usr/bin/

# 加载驱动
echo "--- 加载驱动模块 ---"
ssh ${BOARD_USER}@${BOARD_IP} '
    cd /lib/modules/4.1.15
    insmod comp_drv.ko
    insmod key_input.ko
    insmod ap3216c.ko
    insmod icm20608.ko
    insmod uart_sensor.ko
    echo "已加载模块:"
    lsmod | grep -E "comp_drv|key_input|ap3216c|icm20608|uart_sensor"
'

echo ""
echo "=== 部署完成 ==="
echo "运行 smart_monitor:"
echo "  ssh ${BOARD_USER}@${BOARD_IP}"
echo "  /usr/bin/smart_monitor"
