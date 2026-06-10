# 系统架构设计

## 1. 系统分层

```
┌──────────────────────────────────────────────────────────────────┐
│                     用户空间 (User Space)                         │
│                                                                   │
│  ┌──────────────┐  ┌──────────┐  ┌───────────┐  ┌─────────────┐ │
│  │ smart_monitor │  │ test_*   │  │ shell     │  │ libedge.a   │ │
│  │ epoll 守护    │  │ 测试程序  │  │ cat/echo  │  │ 基础库      │ │
│  └──┬──┬──┬──┬──┘  └────┬─────┘  └────┬──────┘  └──────┬──────┘ │
│     │  │  │  │           │              │                │        │
│   open/read/write/ioctl/poll     sysfs/cat        静态链接       │
└─────┼──┼──┼──┼───────────┼──────────────┼────────────────┼──────┘
      │  │  │  │            │              │                │
══════│══│══│══│════════════│══════════════│════════════════│══════
      │  │  │  │            │              │                │
┌─────┼──┼──┼──┼────────────┼──────────────┼────────────────┼──────┐
│     │  │  │  │      内核空间 (Kernel Space)                  │     │
│     │  │  │  │                                               │     │
│  ┌──▼──▼──▼──▼──┐  ┌──────────┐  ┌─────────┐  ┌──────────┐ │     │
│  │ 字符设备层    │  │ misc 设备 │  │ Input   │  │ sysfs    │ │     │
│  │ (cdev)       │  │ /dev/dht* │  │ /dev/   │  │ debugfs  │ │     │
│  │ /dev/comp*   │  │ /dev/sr*  │  │ input/  │  │          │ │     │
│  │ /dev/icm*    │  │ /dev/mq*  │  │ eventX  │  │          │ │     │
│  │ /dev/uart*   │  │ /dev/serv*│  │          │  │          │ │     │
│  │ /dev/can*    │  │ /dev/rel* │  │          │  │          │ │     │
│  └──┬──┬──┬──┬──┘  │ /dev/wdt* │  └────┬─────┘  └──────────┘ │     │
│     │  │  │  │     └──┬──┬──┬──┘       │                       │     │
│     │  │  │  │        │  │  │           │                       │     │
│  ┌──▼──▼──▼──▼────────▼──▼──▼──┐  ┌───▼───────────┐           │     │
│  │      平台/总线驱动层          │  │ Input 子系统    │           │     │
│  │  comp_drv | uart_sensor     │  │ key_input      │           │     │
│  │  dht11 | sr04 | relay      │  └────────────────┘           │     │
│  │  mq135_adc | servo_pwm     │                                │     │
│  │  can_drv  | wdt            │                                │     │
│  └──┬──┬──┬──┬──┬──┬──┬──┬───┘                                │     │
│     │  │  │  │  │  │  │  │                                     │     │
│  ┌──▼──▼──▼──▼──▼──▼──▼──▼──────────────────────────────────┐ │     │
│  │              内核子系统层                                   │ │     │
│  │  GPIO │ I2C │ SPI │ UART(裸) │ ADC(裸) │ PWM(裸) │ CAN   │ │     │
│  │        │     │     │          │         │         │(Socket)│ │     │
│  └──┬──┬──┼──┬──┼──┬──┼──────────┼─────────┼─────────┼───────┘ │     │
│     │  │  │  │  │  │  │          │         │         │          │     │
└─────│──│──│──│──│──│──│──────────│─────────│─────────│──────────┘     │
      │  │  │  │  │  │  │          │         │         │                 │
══════│══│══│══│══│══│══│══════════│═════════│═════════│═════════════════│
      │  │  │  │  │  │  │          │         │         │                 │
┌─────│──│──│──│──│──│──│──────────│─────────│─────────│─────────────────┐
│     │  │  │  │  │  │  │    硬件 (Hardware)   │         │                │
│  ┌──▼┐┌─▼┐┌─▼─┐┌▼──┐┌▼──┐┌▼──┐┌▼────┐┌───▼─┐┌────▼───┐┌──────┐┌───┐│
│  │LED││KEY││DHT││SR ││MQ ││舵机││继电器││AP3216││ICM20608││UART3 ││CAN││
│  │   ││   ││11 ││04 ││135││PWM││GPIO  ││I2C   ││SPI    ││0x021E││收发 ││
│  └───┘└───┘└───┘└───┘└───┘└───┘└─────┘└─────┘└───────┘│C000  ││器  ││
│                                                          └──────┘└───┘│
└───────────────────────────────────────────────────────────────────────┘
```

## 2. 驱动注册方式对比

| 驱动 | 注册方式 | 设备节点 | 驱动模型 | 特点 |
|------|------|------|------|------|
| comp_drv | platform_driver + CDEV | /dev/comp_drv | cdev | read/write/ioctl/poll/fasync + sysfs + debugfs + 定时器 |
| key_input | platform_driver + Input | /dev/input/eventX | input | 中断上半部 + delayed_work 下半部 |
| ap3216c | i2c_driver + CDEV | /dev/ap3216c | cdev | SMBus + i2c_transfer 突发读 |
| icm20608 | spi_driver + CDEV | /dev/icm20608 | cdev | SPI message/transfer + ioctl |
| uart_sensor | platform_driver + CDEV | /dev/uart_sensor | cdev | ioremap + readb/writeb + RX中断 + kfifo |
| **dht11** | **misc_register** | **/dev/dht11** | **misc** | **单总线时序 + sysfs 属性** |
| **sr04** | **misc_register** | **/dev/sr04** | **misc** | **GPIO中断 + ktime脉冲测量 + completion** |
| **mq135_adc** | **misc_register** | **/dev/mq135** | **misc** | **ioremap ADC + kthread + sysfs** |
| **servo_pwm** | **misc_register** | **/dev/servo** | **misc** | **ioremap PWM + 占空比控制** |
| **relay** | **misc_register** | **/dev/relay** | **misc** | **gpiod 输出 + sysfs** |
| **can_drv** | **cdev** | **/dev/can_ctrl** | **cdev** | **SocketCAN 封装 + ioctl** |
| **wdt** | **misc_register** | **/dev/wdt_custom** | **misc** | **ioremap WDOG + ioctl** |

## 3. 数据流

### 3.1 传感器读取流程 (v2.0 扩展)

```
触发源:
  ├─ epoll 事件 (LED/按键/UART 有事件时)
  ├─ 定时轮询 (timer 周期, 每 N 秒)
  └─ UART SENSOR 命令

流程:
  smart_monitor.read(fd)
    → 系统调用 read()
    → 驱动 read():
        ├─ ap3216c:   i2c_transfer() → 读 I2C 寄存器 → copy_to_user()
        ├─ icm20608:  spi_sync() → 读 SPI 寄存器 → copy_to_user()
        ├─ dht11:     gpiod 时序 → 40bit 数据 → 校验 → copy_to_user()
        ├─ sr04:      TRIG 脉冲 → 等 completion → copy_to_user()
        ├─ mq135_adc: readl(ADC) → copy_to_user()
        └─ servo:     返回当前角度 → copy_to_user()
    → 用户空间解析 → 打印 + 日志 + 联动判断
```

### 3.2 联动控制流程

```
sensor → g_sensors (共享区)
  │
  ├── 温度 > 35°C (连续3次) → write(relay_fd, "on") + write(led_fd, "blink")
  ├── 温度 < 30°C (连续3次) → write(relay_fd, "off") + write(led_fd, "on")
  ├── 距离 < 30cm (连续2次) → write(beep_fd, "on")
  └── 空气质量 ≥ 4 级       → write(beep_fd, "on") + write(relay_fd, "on")
```

## 4. IO 模型支持矩阵 (v2.0 扩展)

| 驱动 | 阻塞读 | 非阻塞读 | poll | ioctl | 写操作 | fasync |
|------|------|------|------|------|------|------|
| comp_drv | ✅ wait_event | ✅ -EAGAIN | ✅ POLLIN/OUT | ✅ 4命令 | ✅ on/off | ✅ SIGIO |
| key_input | — | ✅ -EAGAIN | ✅ | — | — | — |
| ap3216c | — | — | — | — | — | — |
| icm20608 | — | — | — | ✅ 4命令 | — | — |
| uart_sensor | ✅ wait_event | ✅ -EAGAIN | ✅ POLLIN/OUT | — | ✅ TX发送 | — |
| **dht11** | **✅ poll_wait** | — | **✅ POLLIN** | — | — | — |
| **sr04** | **✅ completion** | — | **✅ POLLIN** | — | — | — |
| **mq135_adc** | — | — | **✅ POLLIN** | — | — | — |
| **servo_pwm** | — | — | — | — | **✅ 角度** | — |
| **relay** | — | — | — | — | **✅ on/off** | — |
| **can_drv** | — | **✅ -EAGAIN** | **✅ POLLIN/OUT** | **✅ 2命令** | **✅ 帧** | — |
| **wdt** | — | — | — | **✅ 2命令** | **✅ 喂狗** | — |

## 5. 并发控制策略

| 驱动 | 锁类型 | 保护范围 | 为什么选它 |
|------|------|------|------|
| comp_drv | mutex | 设备状态 + 定时器 | 可能睡眠 |
| key_input | spinlock | 中断共享状态 | 中断上下文 |
| ap3216c | mutex | 传感器数据缓存 | 进程上下文 |
| icm20608 | mutex | 传感器数据缓存 | 进程上下文 |
| uart_sensor | mutex | kfifo + 设备配置 | 进程上下文，中断用无锁 kfifo_in |
| **sr04** | **completion** | **等待回波完成** | **一次性同步，比 wait_event 更简单** |
| **mq135_adc** | **mutex** | **ADC 读数 + kthread 睡眠** | **kthread 在进程上下文** |

## 6. 模块依赖关系

```
smart_monitor (用户态)
  ├── /dev/comp_drv       ← comp_drv.ko        (GPIO 子系统)
  ├── /dev/input/event0   ← key_input.ko       (Input 子系统 + GPIO)
  ├── /dev/ap3216c        ← ap3216c.ko         (I2C 子系统)
  ├── /dev/icm20608       ← icm20608.ko        (SPI 子系统)
  ├── /dev/uart_sensor    ← uart_sensor.ko     (platform + 裸寄存器)
  ├── /dev/dht11          ← dht11.ko           (misc + 裸GPIO时序)
  ├── /dev/sr04           ← sr04.ko            (misc + GPIO中断)
  ├── /dev/mq135          ← mq135_adc.ko       (misc + 裸ADC寄存器)
  ├── /dev/servo          ← servo_pwm.ko       (misc + 裸PWM寄存器)
  ├── /dev/relay          ← relay.ko           (misc + GPIO)
  ├── /dev/can_ctrl       ← can_drv.ko         (cdev + SocketCAN)
  ├── /dev/wdt_custom     ← wdt.ko             (misc + 裸WDOG寄存器)
  └── libedge.a            ← libedge/           (静态链接)

内核子系统依赖:
  GPIO    — comp_drv, key_input, dht11, sr04, relay
  Input   — key_input
  I2C     — ap3216c
  SPI     — icm20608
  UART    — uart_sensor (裸寄存器, 不使用内核 UART 子系统)
  ADC     — mq135_adc (裸寄存器)
  PWM     — servo_pwm (裸寄存器)
  CAN     — can_drv (SocketCAN)
  WDOG    — wdt (裸寄存器)
  kfifo   — uart_sensor
```

## 7. 关键技术决策记录

| 决策 | 选项 A | 选项 B | 选择 | 原因 |
|------|------|------|------|------|
| UART 驱动方式 | 内核 UART 子系统 | 裸寄存器操作 | B | 学习寄存器级操作 |
| 按键去抖 | tasklet | delayed_work | B | workqueue 可睡眠 |
| 命令解析位置 | 驱动内 | 用户空间 | B | 跨驱动调用困难；Unix 哲学 |
| IO 多路复用 | select | epoll | B | epoll O(1) 复杂度 |
| **DHT11 驱动模型** | **cdev** | **misc** | **B** | **代码量减半，自动 sysfs** |
| **SR04 同步方式** | **wait_event** | **completion** | **B** | **一次性脉冲，completion 语义更准确** |
| **MQ135 采集方式** | **用户空间轮询** | **kthread 内核采集** | **B** | **学习 kthread，降低用户空间压力** |
| **CAN 接入方式** | **裸寄存器** | **SocketCAN** | **B** | **SocketCAN 是 Linux 标准，工业级稳定** |

## 8. 项目文件统计 (v2.0 规划)

| 目录 | 说明 | 规划行数 |
|------|------|------|
| driver/ | 12 个驱动 (5有 + 7新) + Makefile | ~4,500 |
| libedge/ | 8 个基础库模块 + 单元测试 | ~2,000 |
| app/ | smart_monitor (扩展) + Makefile | ~900 |
| test/ | 11 个功能测试 + 5 个单元测试 | ~1,300 |
| dts/ | 设备树 (扩展节点) | ~100 |
| docs/ | 10 份 MD 文档 + 9 份阶段报告 | ~4,000 |
| scripts/ | 4 个脚本 | ~150 |

---

*文档版本：v2.0 | 日期：2026-06-10*
