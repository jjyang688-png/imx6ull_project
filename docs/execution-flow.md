# 智能环境监测终端 — 完整执行流程

> **项目**: imx6ull_project  
> **SoC**: NXP i.MX6ULL (ARM Cortex-A7)  
> **内核**: Linux 4.1.15  
> **更新时间**: 2026-06-08  

本文档从系统启动到应用运行，逐步追踪每一行代码的执行路径。

---

## 目录

1. [系统启动 & 设备树加载](#1-系统启动--设备树加载)
2. [驱动模块加载（5 个 insmod）](#2-驱动模块加载)
3. [设备节点创建 & 注册](#3-设备节点创建--注册)
4. [用户程序启动 smart_monitor](#4-用户程序启动-smart_monitor)
5. [epoll 事件主循环](#5-epoll-事件主循环)
6. [UART 命令处理流程](#6-uart-命令处理流程)
7. [LED 控制多入口流程](#7-led-控制多入口流程)
8. [按键中断处理流程（硬件→用户）](#8-按键中断处理流程硬件用户)
9. [传感器数据读取流程](#9-传感器数据读取流程)
10. [程序退出 & 资源回收](#10-程序退出--资源回收)
11. [构建 & 部署流程](#11-构建--部署流程)
12. [时序总览图](#12-时序总览图)

---

## 1. 系统启动 & 设备树加载

### 1.1 U-Boot → Kernel → DTB

```
开发板上电
  │
  └─ U-Boot 启动
       │ 加载 zImage + imx6ull-14x14-emmc.dtb 到内存
       │ 配置 bootargs（console=ttymxc0,115200 root=/dev/mmcblk1p2...）
       └─ bootz 跳转到内核
            │
            └─ Linux 内核启动
                 │ 解析 .dtb
                 │ 初始化各子系统：GPIO → I2C → SPI → UART → ...
                 │
                 └─ 设备树节点被解析为 platform_device / i2c_client / spi_device
```

### 1.2 设备树中定义的设备

文件: `dts/imx6ull-smart-monitor.dtsi`

```dts
╔══════════════════════════════════════════════════════════════╗
║ 设备树节点                       → 内核对象类型              ║
╠══════════════════════════════════════════════════════════════╣
║ comp_drv_dev      (platform)    → platform_device           ║
║ key_input_dev     (platform)    → platform_device           ║
║ uart_sensor_dev   (platform)    → platform_device           ║
║ ap3216c@1e        (I2C1 子节点) → i2c_client               ║
║ icm20608@0        (ECSPI3子节点)→ spi_device               ║
╚══════════════════════════════════════════════════════════════╝
```

**关键细节:**
- `&uart3 { status = "disabled"; }` — 禁用内核自带 imx-uart 驱动，防止抢占 UART3 硬件
- `uart_sensor_dev` 独立声明 `reg = <0x021ec000 0x4000>`，直接映射硬件寄存器

---

## 2. 驱动模块加载

### 2.1 加载顺序

```bash
insmod comp_drv.ko      # ① LED 驱动 (platform)
insmod key_input.ko     # ② 按键驱动 (platform + input)
insmod ap3216c.ko       # ③ I2C 传感器
insmod icm20608.ko      # ④ SPI 传感器
insmod uart_sensor.ko   # ⑤ UART 控制台 (platform + 裸寄存器)
```

> **顺序说明**: platform/i2c/spi 驱动间无依赖，加载顺序可任意。但 smart_monitor 要求全部设备节点存在。

### 2.2 每个驱动的初始化链路

```
insmod xxx.ko
  │
  └─ module_xxx_driver() 展开为 module_init() + module_exit()
       │
       └─ 内核根据驱动类型注册到对应总线:
            ├─ platform_driver_register()    → 扫描 platform 总线
            ├─ i2c_register_driver()         → 扫描 I2C 总线
            └─ spi_register_driver()         → 扫描 SPI 总线
                 │
                 └─ 匹配成功 → 调用 probe()
```

---

## 3. 设备节点创建 & 注册 (probe 阶段)

每个驱动的 `probe()` 都遵循相同的 **CDEV 创建 5 步法**:

```
probe()
  ├─ Step 1: devm_kzalloc()        分配设备结构体
  ├─ Step 2: 获取硬件资源           GPIO / I2C client / SPI dev / ioremap
  ├─ Step 3: 初始化并发控制          mutex_init / spin_lock_init / init_waitqueue_head
  ├─ Step 4: 验证硬件               WHO_AM_I / SYS_CONF
  ├─ Step 5: alloc_chrdev_region()  动态分配设备号
  ├─ Step 6: cdev_init() + cdev_add()
  ├─ Step 7: class_create() + device_create()  创 /dev/xxx
  ├─ Step 8: 额外设施               sysfs_group / debugfs / request_irq
  └─ Step 9: dev_info()             打印 "probed OK"
```

### 3.1 各驱动 probe 对比

| 步骤 | comp_drv | key_input | ap3216c | icm20608 | uart_sensor |
|------|----------|-----------|---------|----------|-------------|
| 分配结构体 | `devm_kzalloc` | `devm_kzalloc` | `devm_kzalloc` | `devm_kzalloc` | `devm_kzalloc` |
| 获取硬件 | `devm_gpiod_get` | `devm_gpiod_get` | I2C client | SPI device | `devm_ioremap_resource` |
| 验证硬件 | — | — | 读 `SYS_CONF` | 读 `WHO_AM_I` | 复位 UART |
| 锁类型 | mutex | spinlock | mutex | mutex | mutex |
| CDEV | ✓ | — (Input子) | ✓ | ✓ | ✓ |
| class_create | ✓ | — | ✓ | ✓ | ✓ |
| 设备文件 | `/dev/comp_drv` | `/dev/input/event0` | `/dev/ap3216c` | `/dev/icm20608` | `/dev/uart_sensor` |
| 中断 | — | `request_irq` (双边沿) | — | — | `request_irq` (RX) |
| 调试接口 | sysfs + debugfs | debugfs | — | — | — |
| 额外设施 | 内核定时器 | delayed_work 消抖 | — | — | kfifo + 时钟使能 |

### 3.2 uart_sensor 特殊流程

```
uart_sensor_probe()
  ├─ ① devm_kzalloc          分配结构体
  ├─ ② devm_ioremap_resource 物理地址 0x021EC000 → 虚拟地址
  ├─ ③ platform_get_irq      获取 GIC_SPI 28 → Linux IRQ 60
  ├─ ④ devm_clk_get          获取 ipg + per 时钟
  │    clk_prepare_enable     使能时钟
  ├─ ⑤ uart_hw_init():
  │    ├─ SRST 复位 UART
  │    ├─ UCR2: RXEN | TXEN | IRTS
  │    ├─ UCR3: RXDMUXSEL = 1 (DCE 模式)
  │    ├─ UFCR: TXTL=0, RXTL=1 (收到1字节就中断)
  │    ├─ UBIR=4, UBMR=3124 (115200@80MHz)
  │    ├─ UCR1: UARTEN
  │    └─ UCR4: DREN (RX 中断使能)
  ├─ ⑥ devm_request_irq      注册 RX 中断
  ├─ ⑦ INIT_KFIFO            初始化环形缓冲
  └─ ⑧ CDEV 5步法            创 /dev/uart_sensor
```

---

## 4. 用户程序启动 smart_monitor

### 4.1 main() 执行流程

```
main(argc, argv)
  │
  ├─ ① 解析命令行参数
  │    ├─ --no-log      → g_log_enabled = 0
  │    ├─ --interval N  → g_poll_interval = N
  │    └─ --help        → print_usage() + return
  │
  ├─ ② 注册信号处理
  │    signal(SIGINT,  signal_handler)  → 设 g_running = 0
  │    signal(SIGTERM, signal_handler)
  │
  ├─ ③ 打开 CSV 日志
  │    fopen("smart_monitor.csv", "a")
  │
  ├─ ④ open_all_devices()
  │    ┌─────────────────────────────────────────────────────┐
  │    │ fd[0] = open("/dev/comp_drv",   O_RDWR)             │
  │    │ fd[1] = open("/dev/input/event0", O_RDONLY|O_NONBLOCK) │
  │    │ fd[2] = open("/dev/ap3216c",    O_RDONLY)           │
  │    │ fd[3] = open("/dev/icm20608",   O_RDONLY)           │
  │    │ fd[4] = open("/dev/uart_sensor", O_RDWR)   ← 关键   │
  │    │ fd[5] = STDIN_FILENO + fcntl(O_NONBLOCK)            │
  │    └─────────────────────────────────────────────────────┘
  │
  ├─ ⑤ setup_epoll()
  │    epfd = epoll_create1(0)
  │    for i in 0..5:
  │        epoll_ctl(epfd, EPOLL_CTL_ADD, fd[i], EPOLLIN)
  │
  ├─ ⑥ run_event_loop(epfd)  ← 主循环
  │
  └─ ⑦ cleanup(epfd)
       close(epfd) → close(所有fd) → fclose(csv)
```

### 4.2 设备 fd 索引表

| 索引 | 设备 | fd | 打开模式 | 是否关键 |
|------|------|----|---------|---------|
| [0] | comp_drv | ≥0 | O_RDWR | 否 (缺失仅警告) |
| [1] | key_input | ≥0 | O_RDONLY \| O_NONBLOCK | 否 |
| [2] | ap3216c | ≥0 | O_RDONLY | 否 |
| [3] | icm20608 | ≥0 | O_RDONLY | 否 |
| [4] | uart_sensor | ≥0 | O_RDWR | **是** (失败则退出) |
| [5] | stdin | 0 | O_NONBLOCK | 是 |

---

## 5. epoll 事件主循环

### 5.1 循环结构

```
while (g_running)
  │
  ├─ epoll_wait(epfd, events, MAX_EVENTS, 1000ms)
  │    │
  │    ├─ 超时 (0 事件) → 继续循环 → 到定时轮询
  │    ├─ EINTR (Ctrl+C) → break
  │    └─ nfds > 0 → 遍历 events[]
  │         │
  │         └─ 根据 fd 分发:
  │              ├─ fd == g_device_fds[0]  →  handle_led_event()
  │              ├─ fd == g_device_fds[1]  →  handle_key_event()
  │              ├─ fd == g_device_fds[2]  →  handle_sensor_read(ap3216c)
  │              ├─ fd == g_device_fds[3]  →  handle_sensor_read(icm20608)
  │              ├─ fd == g_device_fds[4]  →  handle_uart_rx()
  │              └─ fd == g_device_fds[5]  →  handle_stdin()
  │
  └─ 定时轮询 (每 g_poll_interval 秒)
       if (now - last_sensor_read >= g_poll_interval):
           handle_sensor_read(fd[2])  // AP3216C
           handle_sensor_read(fd[3])  // ICM20608
```

### 5.2 事件分发矩阵

| 触发源 | fd | 处理函数 | 数据流向 |
|--------|----|---------|---------|
| LED 状态变化 | g_device_fds[0] | `handle_led_event` | comp_drv.read() → struct → log |
| 按键按下 | g_device_fds[1] | `handle_key_event` | read input_event → toggle LED |
| AP3216C 可读 | g_device_fds[2] | `handle_sensor_read` | read struct → log + CSV |
| ICM20608 可读 | g_device_fds[3] | `handle_sensor_read` | read struct → log + CSV |
| UART RX 数据 | g_device_fds[4] | `handle_uart_rx` | read chunk → 拼行 → 命令分发 |
| 键盘输入 | g_device_fds[5] | `handle_stdin` | read 1 char → 查表 |
| 定时器到期 | (内部) | `handle_sensor_read` ×2 | 主动轮询传感器 |

---

## 6. UART 命令处理流程

### 6.1 完整数据路径

```
PC 串口助手 (115200-8-N-1)
  │
  │  发送 "STATUS\r\n"
  │
  ▼
RS232 线缆 → DB9 接口 → SP3232 电平转换
  │
  ▼
UART3_RXD (i.MX6ULL 引脚)
  │
  ▼
硬件 RX FIFO (32 字节深)
  │  UFCR_RXTL=1  →  收到 ≥1 字节触发中断
  │
  ▼
GIC 控制器 → GIC_SPI 28 → Linux IRQ 60
  │
  ▼
uart_rx_interrupt()                    ← 中断上下文 (上半部)
  │
  ├─ 检查 USR2_RDR = 1? 否则返回 IRQ_NONE
  │
  ├─ while (USR2_RDR):
  │     byte = readb(URXD) & 0xFF       ← 硬件寄存器读
  │     kfifo_in(&rx_fifo, &byte, 1)    ← 写入环形缓冲 (无锁)
  │     dev->rx_bytes++
  │
  └─ wake_up_interruptible(&rx_wq)      ← 唤醒阻塞的 read
       │
       ▼
smart_monitor: epoll_wait 返回 fd[4] 可读
  │
  ▼
handle_uart_rx(fd[4])                  ← 用户空间
  │
  ├─ read(fd[4], chunk, 127)             → 内核 uart_sensor_read()
  │    └─ kfifo_to_user()              ← 从环形缓冲取数据拷给用户
  │
  ├─ 行缓冲拼接:
  │    for each char in chunk:
  │      if c == '\n' or '\r':
  │        line_buf[line_pos] = '\0'
  │        handle_uart_command(fd[4], line_buf)  ← 命令分发
  │        line_pos = 0
  │      else:
  │        line_buf[line_pos++] = c
  │
  └─ handle_uart_command():
       ├─ ① 去掉 \r \n
       ├─ ② 按空格分割 → cmd + args
       ├─ ③ 遍历 uart_commands[] 查表
       └─ ④ 调用 handler(fd, args)
```

### 6.2 命令分发表

```
uart_commands[] = {
    {"STATUS", cmd_status, ...}  ─→ 读 /proc/uptime, 统计传感器
    {"SENSOR", cmd_sensor, ...}  ─→ open→read→close AP3216 + ICM20608
    {"LED",    cmd_led,    ...}  ─→ open→write("on"/"off")→close comp_drv
    {"HELP",   cmd_help,   ...}  ─→ write() 帮助文本
    {"RESET",  cmd_reset,  ...}  ─→ write("off") LED + 响应
}
```

### 6.3 cmd_sensor() 详细流程

```
cmd_sensor(uart_fd, args)
  │
  ├─ ① 临时 open("/dev/ap3216c", O_RDONLY)
  │    └─ read(fd_ap, &data, sizeof(data))
  │         └─ 内核: ap3216c_read()
  │              └─ ap3216c_read_data()
  │                   └─ i2c_transfer(adapter, msgs, 2)
  │                        msg[0]: 写 1 字节 (寄存器地址 0x0A)
  │                        msg[1]: 读 6 字节 (IR+ALS+PS)
  │         ← copy_to_user → smart_monitor
  │    └─ close(fd_ap)
  │
  ├─ ② 临时 open("/dev/icm20608", O_RDONLY)
  │    └─ read(fd_icm, &data, sizeof(data))
  │         └─ 内核: icm20608_read()
  │              └─ icm20608_read_all()
  │                   └─ icm20608_read_burst(REG_ACCEL_XOUT_H, buf, 14)
  │                        └─ spi_sync(spi, &msg)
  │                             xfer[0]: 发 1 字节 (0x3B | 0x80)
  │                             xfer[1]: 收 14 字节
  │         ← copy_to_user → smart_monitor
  │    └─ close(fd_icm)
  │
  └─ ③ snprintf(resp) + write(uart_fd, resp)
       └─ 内核: uart_sensor_write()
            └─ for each byte:
                 while (!(USR1_TRDY)) cpu_relax()
                 writeb(byte, UTXD)
            ─→ UART3_TXD → RS232 → PC 串口助手显示
```

---

## 7. LED 控制多入口流程

```
              ┌──────────┬──────────┬───────────┐
              │  键盘 '1'│ 按键 KEY0│ UART LED ON│
              │          │          │            │
              ▼          ▼          ▼            │
         handle_stdin  handle_key  cmd_led       │
              │          │          │            │
              └──────────┴──────────┴────────────┘
                         │
                         ▼
            open("/dev/comp_drv", O_WRONLY)
                         │
                         ▼
            write(fd, "on", 2) 或 write(fd, "off", 3)
                         │
                         ▼
            =========== 内核态 ===========
                         │
            comp_drv: comp_write()
              ├─ copy_from_user(kbuf, ...)
              ├─ mutex_lock(&dev->lock)
              ├─ strncmp("on"/"off") / kbuf[0]=='1'/'0'
              ├─ 停止定时器 (如果正在闪烁)
              ├─ dev->led_state = LED_ON / LED_OFF
              ├─ comp_led_update(dev)
              │    └─ gpiod_set_value(gpio, 1/0)  ← GPIO 硬件操作
              ├─ dev->write_count++
              ├─ comp_led_notify(dev)
              │    ├─ atomic_set(&dev->changed, 1)
              │    ├─ wake_up_interruptible(&wq)     ← 唤醒阻塞 read
              │    └─ kill_fasync(&fasync, SIGIO, POLL_IN)  ← SIGIO 通知
              ├─ mutex_unlock(&dev->lock)
              └─ return
                         │
                         ▼
            close(fd_led)
```

---

## 8. 按键中断处理流程（硬件→用户）

```
KEY0 物理按键 (GPIO1_IO18, active-low)
  │
  │  按下: 电平 高→低   释放: 电平 低→高
  │
  ▼
GPIO 边沿检测 → GIC 中断控制器 → Linux IRQ
  │
  ▼
key_irq_handler()                       ← 中断上半部 (硬中断上下文)
  │
  ├─ dev->irq_count++                   ← 原子操作
  ├─ schedule_delayed_work(&work, 20ms) ← 调度下半部, 延迟=消抖时间
  └─ return IRQ_HANDLED
       │
       │  ... 20ms 后, workqueue 线程唤醒 ...
       │
       ▼
key_work_func()                         ← 中断下半部 (进程上下文)
  │
  ├─ val = gpiod_get_value(key_gpio)    ← 读取稳态 GPIO 电平
  ├─ spin_lock_irqsave(&lock)
  ├─ if (val != last_state):            ← 是真边沿 (非毛刺)
  │    last_state = val
  │    spin_unlock_irqrestore
  │    input_report_key(input, KEY_ENTER, val?0:1)
  │    input_sync(input)                ← 事件包边界
  └─ else:
       spin_unlock_irqrestore           ← 毛刺, 丢弃
       │
       ▼
Input 子系统 → /dev/input/event0
       │
       ▼
smart_monitor: epoll_wait 返回 fd[1] 可读
       │
       ▼
handle_key_event(fd[1])
  ├─ read(fd, &ev, sizeof(input_event))
  ├─ if ev.type==EV_KEY && ev.value==1:  ← 只响应"按下"
  │    static int led_is_on
  │    open("/dev/comp_drv", O_WRONLY)
  │    write(fd_led, led_is_on ? "off" : "on")
  │    led_is_on = !led_is_on
  │    close(fd_led)
  └─ log + CSV 记录
```

---

## 9. 传感器数据读取流程

### 9.1 AP3216C (I2C)

```
smart_monitor: handle_sensor_read(fd[2])  或  cmd_sensor()
  │
  read(fd, &data, sizeof(struct ap3216c_data))
  │
  ▼
ap3216c_read()
  ├─ ap3216c_read_data()
  │    └─ i2c_transfer(adapter, msgs, 2)
  │         ┌──────────────────────────────────────┐
  │         │ msg[0]: addr=0x1E, flags=0 (写)      │
  │         │         len=1,  buf = [0x0A]          │
  │         │         → SCL/SDA: START-0x1E/W-ACK-  │
  │         │           0x0A-ACK                    │
  │         │                                       │
  │         │ msg[1]: addr=0x1E, flags=I2C_M_RD (读)│
  │         │         len=6,  buf = [6 bytes]       │
  │         │         → REPEATED_START-0x1E/R-ACK-  │
  │         │           D7..D0-ACK-...-NAK-STOP     │
  │         └──────────────────────────────────────┘
  │    │
  │    └─ 解析: (MSB << 8) | LSB  (AP3216C 小端!)
  │         data.ir  = (buf[1]<<8) | buf[0]
  │         data.als = (buf[3]<<8) | buf[2]
  │         data.ps  = (buf[5]<<8) | buf[4]
  │
  ├─ mutex_lock → memcpy → mutex_unlock
  └─ copy_to_user(buf, &data, count)
  │
  ▼
smart_monitor:
  log_msg("AP3216C: IR=%u ALS=%u PS=%u", ...)
  csv_log("SENSOR_AP3216C", detail)
```

### 9.2 ICM20608 (SPI)

```
smart_monitor: handle_sensor_read(fd[3])  或  cmd_sensor()
  │
  read(fd, &data, sizeof(struct icm20608_full))
  │
  ▼
icm20608_read()
  ├─ icm20608_read_all()
  │    └─ icm20608_read_burst(REG_ACCEL_XOUT_H=0x3B, buf, 14)
  │         └─ spi_sync(spi, &msg)
  │              ┌──────────────────────────────────────┐
  │              │ xfer[0]: tx = [0x3B|0x80=0xBB]       │
  │              │          len = 1 (发地址+读标志)      │
  │              │                                      │
  │              │ xfer[1]: rx = buf[14]                 │
  │              │          len = 14 (接收14字节)        │
  │              │                                      │
  │              │ SPI 时序 (CS低→tx地址→读14字节→CS高) │
  │              │ MOSI: 0xBB                            │
  │              │ MISO: D0 D1 ... D13                   │
  │              │ SCLK: 8MHz, Mode 0 (CPOL=0,CPHA=0)   │
  │              └──────────────────────────────────────┘
  │    │
  │    └─ 解析: ICM20608 大端! (MSB << 8) | LSB
  │         data.accel_x = ((s16)buf[0] << 8) | buf[1]
  │         data.accel_y = ((s16)buf[2] << 8) | buf[3]
  │         data.accel_z = ((s16)buf[4] << 8) | buf[5]
  │         data.temp    = ((s16)buf[6] << 8) | buf[7]
  │         data.gyro_x  = ((s16)buf[8] << 8) | buf[9]
  │         data.gyro_y  = ((s16)buf[10]<< 8) | buf[11]
  │         data.gyro_z  = ((s16)buf[12]<< 8) | buf[13]
  │
  ├─ mutex_lock → memcpy → mutex_unlock
  └─ copy_to_user(buf, &data, count)
  │
  ▼
smart_monitor:
  temp_c = data.temp / 340.0 + 36.53  // 温度换算公式
  log_msg("ICM20608: temp=%.1fC acc=(%d,%d,%d) gyro=(%d,%d,%d)", ...)
  csv_log("SENSOR_ICM20608", detail)
```

---

## 10. 程序退出 & 资源回收

### 10.1 触发退出

```
触发方式:
  ├─ 键盘 'q' 或 ESC → handle_stdin() → g_running = 0
  ├─ Ctrl+C           → signal_handler() → g_running = 0
  └─ SIGTERM          → signal_handler() → g_running = 0

  g_running = 0
    │
    └─ while(g_running) 循环退出
         │
         ▼
cleanup(epfd)
  ├─ ① 恢复 stdin 阻塞模式  fcntl(STDIN_FILENO, ~O_NONBLOCK)
  ├─ ② close(epfd)
  ├─ ③ 关闭 5 个设备 fd       for i in 0..4: close(g_device_fds[i])
  ├─ ④ fclose(g_csv_fp)       保存 CSV 日志
  └─ ⑤ return 0
```

### 10.2 驱动卸载 (rmmod)

```
rmmod uart_sensor.ko → remove()
  ├─ writel(0, UCR4)           关 RX 中断
  ├─ writel(0, UCR1)           关 UART 模块
  ├─ device_destroy → class_destroy → cdev_del → unregister_chrdev_region
  └─ clk_disable_unprepare ×2  关时钟

rmmod icm20608.ko → remove()
  ├─ icm20608_write_reg(PWR_MGMT_1, SLEEP)  芯片休眠
  └─ CDEV 逆序清理 + devm_ 自动释放

rmmod ap3216c.ko → remove()
  ├─ ap3216c_write_reg(SYS_CONF, 0)         关闭传感器
  └─ CDEV 逆序清理

rmmod key_input.ko → remove()
  ├─ debugfs_remove_recursive()
  ├─ cancel_delayed_work_sync()  ← 等重要! 等 work 完成再释放内存
  └─ devm_ 自动释放 (IRQ, input, GPIO, kzalloc)

rmmod comp_drv.ko → remove()
  ├─ del_timer_sync()           ← 先停定时器!
  ├─ gpiod_set_value(led, 0)    熄灭 LED
  ├─ mutex_destroy()
  └─ CDEV 逆序清理
```

---

## 11. 构建 & 部署流程

### 11.1 完整构建链路

```
./scripts/build.sh all
  │
  ├─ make -C driver KERNEL_DIR=...
  │    │
  │    ├─ 进入内核源码树 Kbuild 系统
  │    ├─ 根据 driver/Makefile 的 obj-m 编译:
  │    │    obj-m := hello.o comp_drv.o key_input.o
  │    │              ap3216c.o icm20608.o uart_sensor.o
  │    │
  │    ├─ 每个 .c → .o → .ko (内核模块格式)
  │    └─ 输出: driver/*.ko (6个内核模块)
  │
  └─ make -C app CC=arm-linux-gnueabihf-gcc
       │
       ├─ arm-linux-gnueabihf-gcc -Wall -O2 -g -Wno-unused-result
       │    app/smart_monitor.c -o app/smart_monitor
       └─ 输出: app/smart_monitor (ARM ELF 可执行文件)
```

### 11.2 部署与运行

```
./scripts/deploy.sh 192.168.70.112
  │
  ├─ scp driver/*.ko → 开发板 /lib/modules/4.1.15/
  ├─ scp app/smart_monitor → 开发板 /usr/bin/
  ├─ ssh root@board:
  │    cd /lib/modules/4.1.15
  │    insmod comp_drv.ko
  │    insmod key_input.ko
  │    insmod ap3216c.ko
  │    insmod icm20608.ko
  │    insmod uart_sensor.ko       ← 5个 insmod
  │
  └─ 运行:
       /usr/bin/smart_monitor --interval 2
```

---

## 12. 时序总览图

```
时间 ──────────────────────────────────────────────────────────►

│  系统启动      │  驱动加载          │  应用运行                     │  退出
│               │                    │                              │
│  U-Boot       │  insmod ×5         │  ./smart_monitor             │  Ctrl+C
│  ↓            │  ↓                 │  ↓                           │  ↓
│  Kernel       │  probe() ×5        │  open_all_devices ×6         │  cleanup
│  ↓            │  ↓                 │  ↓                           │  ↓
│  DTB 解析     │  CDEV/I2C/SPI注册  │  epoll_create + epoll_ctl    │  close×6
│  ↓            │  ↓                 │  ↓                           │  ↓
│  设备注册     │  /dev/xxx 创建     │  while(g_running)            │  rmmod×5
│               │  request_irq       │    ├─ epoll_wait(1000ms)     │
│               │                    │    ├─ 按键→handle_key→LED    │
│               │                    │    ├─ UART→handle_uart_rx    │
│               │                    │    │   →handle_uart_command  │
│               │                    │    │   →cmd_xxx→write resp   │
│               │                    │    ├─ 传感器→handle_sensor   │
│               │                    │    ├─ stdin→handle_stdin     │
│               │                    │    └─ 定时→传感器轮询        │
│               │                    │                              │
▼               ▼                    ▼                              ▼

硬件层:
  GPIO ─────── gpiod_set_value() ──── LED ON/OFF/BLINK
  GPIO IRQ ─── key_irq_handler ────── input_report_key → event0
  I2C1 ─────── i2c_transfer() ─────── AP3216C 数据 (6字节)
  ECSPI3 ───── spi_sync() ─────────── ICM20608 数据 (14字节)
  UART3 RX ─── uart_rx_interrupt ──── kfifo_in → read() → 命令解析
  UART3 TX ─── uart_sensor_write ──── writeb(UTXD) → PC 显示
```

---

## 附录A: 关键数据结构流动

```
┌─────────────────────────────────────────────────────────────┐
│  内核态 ↔ 用户态 数据拷贝路径                                │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  comp_drv:                                                  │
│    内核 struct comp_dev → struct comp_drv_status            │
│    → copy_to_user() → 用户 struct comp_drv_status           │
│                                                             │
│  key_input:                                                 │
│    硬件 GPIO 电平 → input_report_key() → input 子系统缓冲   │
│    → read() → 用户 struct input_event                       │
│                                                             │
│  ap3216c:                                                   │
│    硬件 I2C 寄存器 → i2c_transfer() → dev->data             │
│    → copy_to_user() → 用户 struct ap3216c_data              │
│                                                             │
│  icm20608:                                                  │
│    硬件 SPI 寄存器 → spi_sync() → dev->data                 │
│    → copy_to_user() → 用户 struct icm20608_full             │
│                                                             │
│  uart_sensor:                                               │
│    RX: 硬件 URXD → kfifo_in() → kfifo_to_user() → 用户 buf │
│    TX: 用户 buf → copy_from_user() → writeb(UTXD) → 硬件   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 附录B: 并发控制矩阵

| 驱动 | 数据 | 锁 | 读者 | 写者 | 中断访问 |
|------|------|----|------|------|---------|
| comp_drv | led_state, blink_period, read/write_count | mutex | read, ioctl, sysfs | write, ioctl | — |
| key_input | last_state | spinlock | work_func | work_func | irq_handler (只读 irq_count) |
| ap3216c | data (ir/als/ps) | mutex | read | read_data | — |
| icm20608 | data (7 fields) | mutex | read, ioctl | read_all | — |
| uart_sensor | rx_fifo | mutex (进程) / 无锁 (中断) | read, poll | — | uart_rx_interrupt (kfifo_in 无锁) |

## 附录C: 文件清单

| 文件 | 行数 | 作用 |
|------|------|------|
| `driver/comp_drv.c` | ~520 | LED 驱动: GPIO + CDEV + ioctl + poll + fasync + sysfs + debugfs + 内核定时器 |
| `driver/comp_drv.h` | ~24 | LED 驱动头文件: ioctl 命令码 + 状态结构体 |
| `driver/key_input.c` | ~280 | 按键驱动: GPIO IRQ + Input 子系统 + delayed_work 消抖 + debugfs |
| `driver/ap3216c.c` | ~317 | I2C 传感器: SMBus 单寄存器 + i2c_transfer 突发读 |
| `driver/icm20608.c` | ~484 | SPI 传感器: 单寄存器读写 + 突发读 + ioctl 分轴读取 |
| `driver/icm20608.h` | ~46 | SPI 传感器头文件: ioctl 命令码 + 数据结构 |
| `driver/uart_sensor.c` | ~576 | UART 驱动: 裸寄存器 ioremap + RX 中断 + kfifo + 阻塞/非阻塞/poll |
| `app/smart_monitor.c` | ~801 | 用户空间: epoll 守护 + UART 命令引擎 + CSV 日志 + 键盘交互 |
| `dts/imx6ull-smart-monitor.dtsi` | ~68 | 设备树覆盖: 5 个设备节点的硬件描述 |
| `Makefile` | ~35 | 顶层构建: modules + apps |
| `driver/Makefile` | ~16 | Kbuild: obj-m 模块列表 |
| `app/Makefile` | ~20 | 用户程序构建 |
| `scripts/build.sh` | ~45 | 一键编译脚本 |
| `scripts/deploy.sh` | ~42 | 一键部署 (scp + ssh + insmod) |
| `docs/API_REFERENCE.md` | ~302 | 5 个设备文件的编程接口参考 |

---

*文档版本: v1.0 | 自动生成自源码分析 | 2026-06-08*
