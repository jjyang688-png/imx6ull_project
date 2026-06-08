# 系统架构设计

## 1. 系统分层

```
┌──────────────────────────────────────────────────────┐
│                   用户空间 (User Space)               │
│                                                      │
│  ┌─────────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │ smart_monitor│  │ test_*   │  │ shell (echo/cat) │ │
│  │ epoll 守护   │  │ 单测程序  │  │ sysfs/debugfs    │ │
│  └──┬──┬──┬──┬─┘  └────┬─────┘  └────────┬─────────┘ │
│     │  │  │  │          │                  │          │
│   open/read/write/ioctl/poll   sysfs/debugfs         │
└─────┼──┼──┼──┼──────────┼──────────────────┼─────────┘
      │  │  │  │          │                  │
══════│══│══│══│══════════│══════════════════│═════════
      │  │  │  │          │                  │
┌─────┼──┼──┼──┼──────────┼──────────────────┼─────────┐
│     │  │  │  │    内核空间 (Kernel Space)   │         │
│     │  │  │  │                              │         │
│  ┌──▼──▼──▼──▼─┐  ┌───────┐  ┌─────────┐   │         │
│  │ 字符设备层   │  │Input  │  │  sysfs   │   │         │
│  │ (CDEV)      │  │子系统  │  │ debugfs  │   │         │
│  └──┬──┬──┬──┬─┘  └───┬───┘  └─────────┘   │         │
│     │  │  │  │         │                     │         │
│  ┌──▼──▼──▼──▼──┐  ┌──▼──────────┐          │         │
│  │ 平台驱动层    │  │ key_input    │          │         │
│  │ comp_drv     │  │ (platform)   │          │         │
│  │ uart_sensor  │  └──────────────┘          │         │
│  │ (platform)   │                             │         │
│  └──┬──────┬────┘                             │         │
│     │      │                                   │         │
│  ┌──▼──┐ ┌─▼──────┐  ┌──────────┐  ┌───────┐ │         │
│  │GPIO │ │UART HW │  │I2C 驱动   │  │SPI 驱动│ │         │
│  │子系统│ │寄存器   │  │ap3216c   │  │icm20608│ │         │
│  └─────┘ └────────┘  └────┬─────┘  └───┬───┘ │         │
│                            │             │      │         │
└────────────────────────────│─────────────│──────┘         │
                             │             │                 │
                    ┌────────▼──┐  ┌───────▼──┐             │
                    │ I2C1 总线 │  │ ECSPI3   │             │
                    │ (100kHz)  │  │ (8MHz)   │             │
                    └─────┬─────┘  └─────┬────┘             │
                          │              │                   │
══════════════════════════│══════════════│═══════════════════│
                          │              │                   │
┌─────────────────────────│──────────────│───────────────────┐
│                   硬件 (Hardware)       │                   │
│                          │              │                   │
│  ┌──────┐  ┌──────┐  ┌──▼─────┐  ┌────▼─────┐  ┌───────┐ │
│  │ LED  │  │ KEY0 │  │AP3216C │  │ ICM20608 │  │UART3  │ │
│  │GPIO3 │  │GPIO18│  │0x1E    │  │ CS0      │  │0x021E │ │
│  └──────┘  └──────┘  └────────┘  └──────────┘  │C000   │ │
│                                                  └───────┘ │
└───────────────────────────────────────────────────────────┘
```

## 2. 数据流

### 2.1 传感器读取流程

```
触发源:
  ├─ 定时轮询 (epoll 超时, 每 N 秒)
  └─ UART SENSOR 命令

流程:
  smart_monitor.read(fd)
    → 系统调用 read()
    → 驱动 read():
        ├─ ap3216c: i2c_transfer() → 读 I2C 寄存器 → copy_to_user()
        └─ icm20608: spi_sync() → 读 SPI 寄存器 → copy_to_user()
    → 用户空间解析 struct → 打印 + CSV 日志
```

### 2.2 UART 命令控制台流程

```
PC 串口助手 ──RS232──► DB9 ──► UART3_RXD
                                │
                        硬件 RX FIFO
                                │
                        触发中断 (GIC_SPI 28)
                                │
                        uart_rx_interrupt()
                          ├─ readb(URXD)          读硬件寄存器
                          ├─ kfifo_in()           写入环形缓冲
                          └─ wake_up()            唤醒阻塞 read

                        smart_monitor.read(fd[4])
                          ├─ kfifo_to_user()      从缓冲取数据
                          ├─ handle_uart_rx()     行缓冲拼包
                          └─ handle_uart_command() 查表分发
                               ├─ cmd_status() → write(fd, "[STATUS]...")
                               ├─ cmd_sensor() → open/read/close 传感器
                               ├─ cmd_led()    → open/write/close LED
                               ├─ cmd_help()
                               └─ cmd_reset()

                        smart_monitor.write(fd[4], resp)
                          └─ uart_sensor_write()
                               └─ 逐字节 writeb(UTXD) → UART3_TXD
                                                         │
                                                  RS232 → PC 串口助手
```

### 2.3 LED 控制流程（多入口）

```
                    ┌─ UART:  PC 发 "LED ON"  → cmd_led()
   LED 状态变更 ◄───├─ 按键:  KEY0 按下       → handle_key_event()
                    └─ 键盘:  按 '1' 或 '2'   → handle_stdin()

  统一走: open("/dev/comp_drv") → write("on"/"off") → close()
                                    │
                            comp_drv.write()
                              ├─ strncmp("on"/"off")
                              ├─ gpiod_set_value()
                              ├─ comp_led_notify()
                              │    ├─ wake_up()     阻塞 read 醒来
                              │    └─ kill_fasync() SIGIO 通知
                              └─ return
```

## 3. 模块依赖关系

```
smart_monitor (用户态)
  ├─ /dev/comp_drv       ← comp_drv.ko       (GPIO 子系统)
  ├─ /dev/input/event0   ← key_input.ko      (Input 子系统 + GPIO)
  ├─ /dev/ap3216c        ← ap3216c.ko        (I2C 子系统)
  ├─ /dev/icm20608       ← icm20608.ko       (SPI 子系统)
  └─ /dev/uart_sensor    ← uart_sensor.ko    (platform 驱动 + 硬件寄存器)

内核子系统依赖:
  GPIO  ─── comp_drv, key_input
  Input ─── key_input
  I2C   ─── ap3216c
  SPI   ─── icm20608
  UART  ─── uart_sensor (裸寄存器, 不使用内核 UART 子系统)
```

## 4. 驱动注册方式对比

| 驱动 | 注册方式 | 设备节点 | fops 来源 | 特点 |
|------|------|------|------|------|
| comp_drv | platform_driver + CDEV | /dev/comp_drv | 自己写 | 最完整：read/write/ioctl/poll/fasync + sysfs + debugfs + 内核定时器 |
| key_input | platform_driver + Input | /dev/input/eventX | input 子系统 | 不需要自己写 fops；中断上半部 + 下半部 (workqueue) |
| ap3216c | i2c_driver + CDEV | /dev/ap3216c | 自己写 | I2C SMBus + i2c_transfer 突发读 |
| icm20608 | spi_driver + CDEV | /dev/icm20608 | 自己写 | SPI message/transfer + ioctl 分轴读取 |
| uart_sensor | platform_driver + CDEV | /dev/uart_sensor | 自己写 | 裸寄存器操作 (ioremap + readb/writeb)；RX 中断 + kfifo；阻塞/非阻塞/poll |

## 5. IO 模型支持矩阵

| 驱动 | 阻塞读 | 非阻塞读 | poll | ioctl | 异步通知 | 写操作 |
|------|------|------|------|------|------|------|
| comp_drv | wait_event | O_NONBLOCK | POLLIN/OUT | 4 个命令 | SIGIO | on/off |
| key_input | — | O_NONBLOCK → EAGAIN | 通用 | — | — | — |
| ap3216c | mutex_lock | — | — | — | — | — |
| icm20608 | mutex_lock | — | — | 4 个命令 | — | — |
| uart_sensor | wait_event | O_NONBLOCK → EAGAIN | POLLIN/OUT | — | — | TX 发送 |

## 6. 并发控制策略

| 驱动 | 锁类型 | 保护范围 | 为什么选它 |
|------|------|------|------|
| comp_drv | mutex | 设备状态 + 定时器 | 可能睡眠（等待 GPIO） |
| key_input | spinlock | last_state 标志 | 中断上下文访问，不能睡眠 |
| ap3216c | mutex | 传感器数据缓存 | 进程上下文，可能睡眠（I2C 传输） |
| icm20608 | mutex | 传感器数据缓存 | 进程上下文，可能睡眠（SPI 传输） |
| uart_sensor | mutex | kfifo + 设备配置 | 进程上下文；中断只用无锁 kfifo_in |

## 7. 项目文件统计

| 目录 | 文件 | 代码行数 | 说明 |
|------|------|------|------|
| driver/ | 6 个 .c + 2 个 .h + Makefile | ~2800 | 5 个功能驱动 + 1 个示例 |
| app/ | smart_monitor.c + Makefile | ~810 | 统一守护进程 |
| test/ | 4 个 .c | ~450 | 各驱动独立测试 |
| dts/ | 1 个 .dtsi | ~60 | 设备树覆盖 |
| docs/ | 10 个 .md | ~3700 | 需求 + 架构 + API + 8 阶段报告 |
| scripts/ | 4 个 .sh | ~150 | 构建 + 部署 + 测试 + 风格检查 |

## 8. 关键技术决策记录

| 决策 | 选项 A | 选项 B | 选择 | 原因 |
|------|------|------|------|------|
| UART 驱动方式 | 内核 UART 子系统 | 裸寄存器操作 | B | 学习寄存器级操作；项目不要求 TTY 兼容 |
| 按键去抖 | tasklet | delayed_work | B | workqueue 可睡眠，未来可扩展 I2C/SPI 按键 |
| SPI 读 API | spi_write_then_read | spi_sync + transfer | B | Linux 4.1.15 没有 spi_write_then_read |
| 命令解析位置 | 驱动内 | 用户空间 | B | 跨驱动调用在内核中困难；Unix 哲学 |
| IO 多路复用 | select | epoll | B | epoll O(1) 复杂度，Linux 标准方案 |
| 项目监控 | 独立 misc 驱动 | /proc + sysfs | 删除 misc | 功能与现有内核机制重复 |

---

*文档版本：v1.0 | 日期：2026-06-08*
