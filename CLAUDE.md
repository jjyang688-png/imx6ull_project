# imx6ull_project — 工业边缘计算网关

基于 NXP i.MX6ULL (ARM Cortex-A7)，Linux 4.1.15，从零构建完整嵌入式 Linux 驱动项目。

> **v1.0**（阶段 1~9 ✅）：智能环境监测终端 — 5 驱动 + epoll 守护进程
> **v2.0**（阶段 10~14 🔲）：工业边缘计算网关 — 12 驱动 + libedge 基础库 + 联动控制

## 开发环境

| 项目 | 值 |
|------|-----|
| Windows 代码编辑 | VS Code 通过 Samba 编辑 Linux 上的文件 |
| Samba 共享 | `\\192.168.80.106\imx6ull` → 项目目录；`\\192.168.80.106\linux` → `/home/yang/linux`（含内核源码树） |
| VS Code IntelliSense | `.vscode/c_cpp_properties.json` 已配置内核头文件路径，支持跳转定义 |
| Linux 编译环境 | Ubuntu 16.04 @ 192.168.80.106，用户 yang |
| 交叉编译器 | ARM gcc 5.4.0 (`arm-linux-gnueabihf-gcc`) |
| 内核源码树 | `/home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek` |
| 目标板 | 正点原子 i.MX6ULL ALPHA |

## 编译 & 部署

```bash
# SSH 到 Linux 编译（从 MobaXterm 或 ssh yang@192.168.80.106）
cd ~/linux/imx6ull_project
make all          # 编译驱动 + 应用 + libedge
make modules      # 仅编译驱动
make apps         # 仅编译应用 + libedge
make clean        # 清理

# 部署到开发板
# 通过 tftp/nfs 将 .ko 传到板子，insmod 加载
```

## 项目结构

```
driver/     — 12个内核驱动（5 已有 + 7 新增）
  comp_drv.c / .h        LED 控制 (platform + cdev)
  key_input.c            按键输入 (platform + Input 子系统)
  ap3216c.c              光感传感器 (I2C + cdev)
  icm20608.c / .h        6轴传感器 (SPI + cdev)
  uart_sensor.c          UART 命令控制台 (裸寄存器 + cdev)
  dht11.c         🔲     DHT11 温湿度 (misc + 单总线)
  sr04.c          🔲     SR04 超声波 (misc + GPIO IRQ)
  mq135_adc.c     🔲     MQ135 空气质量 (misc + 裸 ADC)
  servo_pwm.c     🔲     舵机控制 (misc + 裸 PWM)
  relay.c         🔲     继电器控制 (misc + GPIO)
  can_drv.c       🔲     CAN 通信 (cdev + SocketCAN)
  wdt.c           🔲     看门狗 (misc + 裸 WDOG)

app/        — 用户空间统一监控程序
  smart_monitor.c        epoll 守护进程 (v1.0: ~800行 → v2.0: ~900行)

libedge/    — 自研 C 基础库 🔲
  include/               ringbuf.h / linked_list.h / crc.h / filter.h / logger.h / ini_parser.h / msgqueue.h / edge_error.h
  src/                   对应 .c 源文件
  tests/                 单元测试 (Unity 框架)

test/       — 功能测试程序（v1.0: 4个 → v2.0: 16个）
dts/        — 设备树源文件 (imx6ull-smart-monitor.dtsi)
docs/       — 完整文档体系 (15份 .md)
scripts/    — build.sh / deploy.sh / test_all.sh / checkpatch.sh
```

## 开发里程碑

### v1.0（阶段 1~9，✅ 已完成）

| 阶段 | 内容 | 关键技术 |
|------|------|---------|
| 1 | 项目立项与环境搭建 | 交叉编译工具链，内核源码树 |
| 2 | 设备树设计 | compatible, reg, interrupts, gpios |
| 3 | LED CDEV 驱动 | cdev_init, cdev_add, class_create, device_create |
| 4 | 并发+IO 模型 | 阻塞/非阻塞/poll/fasync, wait_queue, mutex, atomic |
| 5 | 按键输入驱动 | GPIO IRQ, Input 子系统, delayed_work 消抖 |
| 6 | I2C + SPI 传感器 | i2c_driver, spi_driver, SMBus, spi_message |
| 7 | UART 裸寄存器驱动 | ioremap, readb/writeb, kfifo, RX 中断 |
| 8 | 统一应用+集成测试 | epoll 多路复用, 查表命令引擎, 信号优雅退出 |
| 9 | 文档交付+发布 | 需求/架构/API/硬件文档 + 9 份阶段报告 |

### v2.0（阶段 10~14，🔲 待实现）

| 阶段 | 内容 | 预计产出 |
|------|------|---------|
| 10 | libedge 基础库 | 8 个模块 + 5 个单元测试 |
| 11 | 简单驱动 (relay / wdt / servo) | 3 个 misc 驱动 + 功能测试 |
| 12 | 中等驱动 (dht11 / sr04 / mq135 / can) | 4 个驱动 + 功能测试 |
| 13 | smart_monitor 扩展 + 联动规则 | 13 fd 集成 + 6 条联动规则 |
| 14 | 文档 + 测试 + 发布 | 完整测试报告 + v2.0.0 |

## 技术关键词

**内核**：platform_driver, i2c_driver, spi_driver, misc_register, cdev, SocketCAN,
ioremap + readl/writel, wait_queue + poll_wait, completion, kthread, kfifo,
delayed_work, timer_list, mutex, spinlock, atomic, ktime_get_ns, local_irq_save

**用户空间**：epoll_create1 + epoll_ctl + epoll_wait, struct input_event,
timerfd_create, fcntl(O_NONBLOCK), signal(SIGINT/SIGTERM), 查表法命令引擎

**libedge**：C 宏泛型, 环形缓冲, 双向链表+哨兵, CRC 查表法, 滑动平均 O(1) 优化,
中值滤波, 可变参数 va_list, POSIX mq, INI 状态机, Unity 测试框架

## 文档索引

| 文件 | 内容 |
|------|------|
| `README.md` | 项目总览（16 章，全貌） |
| `docs/requirements.md` | 需求规格说明书 v2.0（12 驱动 + libedge + 联动规则 + 14 里程碑） |
| `docs/architecture.md` | 系统架构设计 v2.0（分层图 + IO 矩阵 + 决策记录） |
| `docs/api-reference.md` | 12 设备 API 参考手册（read/write/ioctl/poll/sysfs） |
| `docs/hardware.md` | 硬件资源规划 v2.0（引脚表 + 内存映射 + 时序参数） |
| `docs/lessons-learned.md` | 全项目知识点汇总 v2.0（13 章 50+ 知识点） |
| `docs/prerequisites-v2.md` | v2.0 前置知识清单（24 个知识点 + 学习路线） |
| `docs/execution-flow.md` | v1.0 完整执行流程 |
| `docs/poll-blocking-wakeup.md` | 内核 poll/wake_up/epoll 全链路详解 |
| `docs/industrial-edge-gateway.md` | v2.0 工业网关架构设计 |
| `docs/expansion-plan.md` | v2.0 扩展方案 |
| `docs/phases/phase1~9_report.md` | 9 份阶段开发报告 |

## 编码约定

- **命名**：驱动/模块名 snake_case（`comp_drv`, `uart_sensor`），设备节点 snake_case（`/dev/comp_drv`）
- **头文件保护**：`#ifndef __DRIVER_NAME_H` 风格
- **错误处理**：内核态 goto 清理模式（`goto fail_label`），用户态 `if (ret < 0) { perror; return -1; }`
- **代码风格**：内核代码通过 `scripts/checkpatch.sh` 检查，用户态 `-Wall -Wextra` 零警告
- **文档**：kebab-case 文件名（`api-reference.md`），Markdown 格式，表驱动组织

## 参考

- 前身项目 firstcc：`C:\Users\2104022\Desktop\code\firstcc`（完整参考实现）
- GitHub：`jjyang688-png/firstcc`（已推送），`jjyang688-png/imx6ull_project`（已推送）
- 内核文档树：`/home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek/Documentation/`
- i.MX6ULL 参考手册：寄存器定义（ADC/PWM/WDOG/FlexCAN/UART）
