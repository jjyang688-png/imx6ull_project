# 智能环境监测边缘节点 v2.0 — 扩展设计方案

> 在当前 imx6ull_project 基础上最大化广度，每个新模块做到"跑通 + 理解原理"即可。

---

## 目录

1. [技能覆盖总览](#1-技能覆盖总览)
2. [整体架构](#2-整体架构)
3. [阶段 A：驱动广度扩展](#3-阶段-a驱动广度扩展)
4. [阶段 B：工业现场总线](#4-阶段-b工业现场总线)
5. [阶段 C：应用基础设施](#5-阶段-c应用基础设施)
6. [阶段 D：云端 + Web + 告警](#6-阶段-d云端--web--告警)
7. [阶段 E：工程化 & DevOps](#7-阶段-e工程化--devops)
8. [目录结构变更](#8-目录结构变更)

---

## 1. 技能覆盖总览

```
                   现有    A      B      C      D      E
                   ────   ────   ────   ────   ────   ────
── 总线/外设 ──
GPIO                ✅    ✅     ✅     ✅     ✅     ✅
I2C                 ✅    ✅     ✅     ✅     ✅     ✅
SPI                 ✅    ✅     ✅     ✅     ✅     ✅
UART(裸寄存器)      ✅    ✅     ✅     ✅     ✅     ✅
单总线(1-Wire)      ✗     ✅  ←
PWM                 ✗     ✅  ←
ADC                 ✗     ✅  ←
RTC                 ✗     ✅  ←
Watchdog            ✗     ✅  ←
CAN                 ✗     ✗     ✅ ←
── 驱动机制 ──
cdev                ✅    ✅     ✅     ✅     ✅     ✅
platform_driver     ✅    ✅     ✅     ✅     ✅     ✅
input子系统         ✅    ✅     ✅     ✅     ✅     ✅
misc设备            ✗     ✅  ←
中断(IRQ)           ✅    ✅     ✅     ✅     ✅     ✅
定时器(timer)       ✅    ✅     ✅     ✅     ✅     ✅
等待队列(wait_q)    ✅    ✅     ✅     ✅     ✅     ✅
poll/fasync         ✅    ✅     ✅     ✅     ✅     ✅
内核线程(kthread)   ✗     ✅  ←
完成量(completion)  ✗     ✅  ←
sysfs属性           ✗     ✅  ←
debugfs             ✗     ✅  ←
regmap              ✗     ✅  ←
脉冲测量(ktime)     ✗     ✅  ←
── 应用编程 ──
epoll多路复用       ✅    ✅     ✅     ✅     ✅     ✅
多线程(pthread)     ✗     ✗     ✗     ✅  ←
互斥锁(mutex)       ✗     ✗     ✗     ✅  ←
消息队列(POSIX mq)  ✗     ✗     ✗     ✅  ←
CRC校验             ✗     ✗     ✗     ✅  ←
配置文件解析(INI)   ✗     ✗     ✗     ✅  ←
分级日志(log levels)✗     ✗     ✗     ✅  ←
── 通信协议 ──
UART命令引擎        ✅    ✅     ✅     ✅     ✅     ✅
I2C协议             ✅    ✅     ✅     ✅     ✅     ✅
SPI协议             ✅    ✅     ✅     ✅     ✅     ✅
Modbus RTU          ✗     ✗     ✅  ←
CAN 2.0             ✗     ✗     ✅  ←
MQTT                ✗     ✗     ✗     ✗     ✅  ←
HTTP/Web            ✗     ✗     ✗     ✗     ✅  ←
── 存储/数据 ──
CSV文件             ✅    ✅     ✅     ✅     ✅     ✅
SQLite              ✗     ✗     ✗     ✗     ✅  ←
环形缓冲(kfifo)     ✅    ✅     ✅     ✅     ✅     ✅
数据导出(JSON/CSV)  ✗     ✗     ✗     ✗     ✅  ←
── 系统/运维 ──
systemd服务         ✗     ✗     ✗     ✅  ←
交叉编译            ✅    ✅     ✅     ✅     ✅     ✅
Buildroot           ✗     ✗     ✗     ✗     ✗     ✅  ←
Docker              ✗     ✗     ✗     ✗     ✗     ✅  ←
GitHub CI           ✗     ✗     ✗     ✗     ✗     ✅  ←
OTA设计             ✗     ✗     ✗     ✗     ✗     ✅  ←
告警(邮件)          ✗     ✗     ✗     ✗     ✅  ←
── 安全 ──
TLS(可选)           ✗     ✗     ✗     ✗     (✅)←(可选)
```

**从 14 个已有知识点 → 新增 28 个知识点，总共 42 个。每个新知识点的代码量控制在 80~400 行。**

---

## 2. 整体架构

```
                         ┌─────────────────────┐
                         │    Web 仪表盘        │
                         │    HTML+Chart.js     │
                         └──────────┬──────────┘
                                    │ HTTP :8080
                         ┌──────────▼──────────┐
                         │   云端 MQTT Broker   │
                         │   + Email 告警网关   │
                         └──────────▲──────────┘
                                    │ MQTT (TLS可选)
┌───────────────────────────────────▼─────────────────────────────────────┐
│                         smart_edge 守护进程                              │
│                                                                         │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌──────────┐ │
│  │ main线程  │ │ mqtt线程  │ │ http线程  │ │ timer线程 │ │ can线程  │ │
│  │ epoll循环 │ │ 周期上报  │ │ Web API   │ │ 存储+告警 │ │ 工业总线 │ │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬────┘ │
│        │              │             │             │             │       │
│  ──────┴──────────────┴─────────────┴─────────────┴─────────────┴────  │
│        共享传感器数据 (pthread_mutex)  +  消息队列 (POSIX mq)             │
│        INI 配置文件  +  分级日志  +  CRC 校验  +  SQLite                  │
└───────────────────────────────────┬─────────────────────────────────────┘
                                    │
    ┌───────┬───────┬───────┬───────┼───────┬───────┬───────┬───────┐
    ▼       ▼       ▼       ▼       ▼       ▼       ▼       ▼       ▼
   LED    KEY   AP3216C ICM20608  UART   DHT11   SR04   MQ135   SERVO
  GPIO   GPIO    I2C     SPI    裸寄存器 单总线  脉冲   ADC     PWM
  [有]    [有]    [有]    [有]    [有]   [新]    [新]   [新]    [新]
    │       │       │       │       │       │       │       │       │
    └───────┴───────┴───────┴───────┼───────┴───────┴───────┴───────┘
                                    │
                    ┌───────┬───────┼───────┬───────┐
                    ▼       ▼       ▼       ▼       ▼
                  RELAY   RTC    WDT    CAN     MODBUS
                  GPIO   片上   片上    CAN     UART
                  [新]    [新]   [新]   [新]    [新]
```

---

## 3. 阶段 A：驱动广度扩展

**目标**：一次性补齐所有常用外设总线类型和内核机制。

### 新增 6 个驱动 + 6 个内核机制

| # | 模块 | 行数 | 总线/类型 | 核心学习点 |
|---|------|------|-----------|-----------|
| 1 | **dht11** | ~350 | GPIO 单总线 | μs 级时序、`udelay`/`ktime_get_ns`、`misc` 设备 |
| 2 | **sr04** | ~200 | GPIO+中断+脉冲 | `request_irq`、ktime 脉宽测量、`completion` |
| 3 | **mq135_adc** | ~250 | **ADC** | i.MX6ULL ADC 控制器、`iio` 子系统或裸寄存器 |
| 4 | **servo_pwm** | ~250 | **PWM** | i.MX6ULL PWM 外设、占空比控制 |
| 5 | **rtc_drv** | ~150 | **RTC** | 片上 RTC 寄存器读写、时间戳 |
| 6 | **wdt_drv** | ~120 | **Watchdog** | 看门狗使能/喂狗/超时复位 |

| 内核机制 | 应用在 | 说明 |
|----------|--------|------|
| **misc 设备** | dht11 / sr04 | 比 cdev 更简洁的注册方式 `misc_register()` |
| **completion** | sr04 | 替代 wait_event 的另一同步方式 `wait_for_completion()` |
| **kthread** | mq135_adc | 驱动里启动内核线程定期采集 ADC |
| **sysfs 属性** | 所有新驱动 | `DEVICE_ATTR` → `/sys/class/xxx/属性` 可直接 cat |
| **debugfs** | comp_drv(改) | `/sys/kernel/debug/comp_drv/status` 调试信息 |
| **regmap** | ap3216c(改) | 用 regmap API 重写 I2C 读写，更简洁 |

### 各驱动设计要点

#### DHT11 — GPIO 单总线温湿度 (~350行)

```
协议: 主机拉低18ms → 拉高40μs → DHT11应答 → 40bit数据
         bit 0: 高电平 26-28μs
         bit 1: 高电平 70μs

设备节点: /dev/dht11
sysfs:    /sys/class/misc/dht11/temp → "27.3"
          /sys/class/misc/dht11/hum  → "65.0"
read():   返回 4 字节 (湿度整数/小数/温度整数/小数)
poll():   poll_wait, DHT11 有 2s 采样间隔限制
```

#### SR04 — 超声波测距 (~200行)

```
协议: TRIG发10μs脉冲 → ECHO回波高电平 → 距离(cm) = 脉宽(μs) / 58

设备节点: /dev/sr04
sysfs:    /sys/class/misc/sr04/distance → "125"
中断:     ECHO上升沿→ktime_get_ns()记T1, 下降沿→T2, ΔT/58=距离
read():   completion 等待回波完成, 返回 uint32 距离
poll():   poll_wait
```

#### MQ135 — ADC 空气质量 (~250行)

```
设备节点: /dev/mq135
sysfs:    /sys/class/misc/mq135/raw     → "1840"  (ADC 原始值)
          /sys/class/misc/mq135/quality  → "good"  (等级)
read():   返回 uint16 ADC 原始值

i.MX6ULL ADC 简介:
  - 两个 12-bit ADC 模块 (ADC1, ADC2)
  - 最多 16 个模拟输入通道
  - 支持软件触发 / 硬件触发
  - 参考手册 Chapter 13

驱动方案(二选一):
  方案1: 用内核 IIO 子系统 (推荐, 简单)
          iio_dev → iio_trigger → sysfs 自动暴露 /sys/bus/iio/
  方案2: 裸寄存器 (学习价值高, 推荐本项目)
          ioremap(0x02198000) → 配置 ADC → poll 读取

本项目选方案2 (学习裸寄存器操作, 和 uart_sensor 思路一致):
  kthread 每 2 秒读一次 ADC 结果寄存器
  根据 ADC 值转换空气质量等级 (优/良/轻度污染/中度污染/重度污染)
```

#### Servo — PWM 舵机控制 (~250行)

```
设备节点: /dev/servo
sysfs:    /sys/class/misc/servo/angle → "90"
write():  echo "45" > /dev/servo → 转到 45°

i.MX6ULL PWM 简介:
  - 4 个 PWM 模块 (PWM1~PWM4)
  - 16-bit 计数器
  - 支持极性配置
  - 参考手册 Chapter 38

控制逻辑:
  舵机 20ms 周期, 占空比 0.5~2.5ms 对应 0°~180°
  PWM 频率 = 50Hz, 占空比 = 2.5% (0°) ~ 12.5% (180°)
  writel(period, PWM_SAR) + writel(duty, PWM_PWMR)
```

#### RTC — 片上时钟 (~150行)

```
设备节点: /dev/rtc_custom
sysfs:    /sys/class/misc/rtc_custom/time → "2026-06-09 15:30:00"
read():   读 RTC 寄存器, 返回年月日时分秒

i.MX6ULL 片上 RTC (SNVS_LP):
  - 32.768KHz 时钟源
  - 秒计数器 (SNVS_LPSRTCMR + SNVS_LPSRTCLR)
  - 参考手册 Chapter 59

注: 实际项目中直接用内核 RTC 框架即可, 
     这个驱动的目的是学习怎么写 RTC 寄存器操作
```

#### WDT — 看门狗 (~120行)

```
设备节点: /dev/wdt_custom
write():  写入任意值 → 喂狗
read():   返回看门狗剩余时间(秒)

i.MX6ULL 看门狗 (WDOG1/WDOG2):
  - 16-bit 超时计数器
  - 超时 → 系统复位
  - 参考手册 Chapter 60

ioctl():  WDT_SET_TIMEOUT 设置超时秒数
          WDT_GET_TIMELEFT 查询剩余时间
```

### 阶段 A 代码量

| 类别 | 行数 |
|------|------|
| 6 个新驱动 | ~1,320 行 |
| 现有驱动改造 (sysfs/debugfs/regmap/completion) | ~200 行 |
| 测试程序 (每个驱动 1 个) | ~300 行 |
| **合计** | **~1,820 行** |

---

## 4. 阶段 B：工业现场总线

**目标**：接触工厂自动化最常用的两种总线协议。

### 新增 2 个模块

| # | 模块 | 行数 | 类型 | 核心学习点 |
|---|------|------|------|-----------|
| 1 | **can_drv** | ~300 | CAN 2.0 | SocketCAN、帧格式、过滤器 |
| 2 | **modbus_rtu** | ~350 | Modbus RTU | 帧结构、CRC16、主从模型 |

#### CAN 总线驱动 (~300行)

```
i.MX6ULL CAN 简介:
  - 两个 CAN 控制器 (CAN1, CAN2)
  - CAN 2.0B 协议, 最高 1Mbps
  - 参考手册 Chapter 55

方案: 用内核 SocketCAN 框架 (只写应用层, 不改内核)
  1. 设备树使能 flexcan
  2. ip link set can0 type can bitrate 500000
  3. ip link set can0 up
  4. 用 SocketCAN API 收发帧

设备节点: /dev/can_ctrl (本项目封装的字符设备, 简化 CAN 操作)
read():   读取 CAN 帧 (ID + DLC + 8字节数据)
write():  发送 CAN 帧
ioctl():  设置波特率 / 过滤器

学习点:
  - CAN 帧结构: 11/29-bit ID, DLC, Data[0..7]
  - SocketCAN: socket(PF_CAN, SOCK_RAW, CAN_RAW)
  - CAN 过滤器: 只收关心的 ID
  - can-utils 工具: candump / cansend
```

#### Modbus RTU (~350行)

```
方案: 应用层实现, 复用现有 /dev/uart_sensor 或单独打开串口

Modbus RTU 帧结构:
  [地址1B][功能码1B][数据N B][CRC16 2B]
              │
              ├── 0x03: 读保持寄存器
              ├── 0x06: 写单个寄存器
              └── 0x10: 写多个寄存器

实现:
  modbus_master.c  — 主站: 发送请求帧, 等待响应, CRC 校验
  modbus_slave.c   — 从站: 等待请求, 处理, 回复

本项目的从站寄存器映射:
  寄存器地址    内容
  ─────────    ────
  0x0000       温度 (×10, int16)
  0x0001       湿度 (×10, int16)
  0x0002       光照 (lux, int16)
  0x0003       距离 (cm,  int16)
  0x0010       LED 状态 (0/1)
  0x0011       舵机角度 (0~180)
  0x0012       继电器状态 (0/1)

测试: PC 上用 Modbus Poll 工具连接板子, 读写寄存器
```

### 阶段 B 代码量

| 类别 | 行数 |
|------|------|
| CAN 封装驱动 | ~300 行 |
| Modbus RTU 协议栈 | ~350 行 |
| 测试程序 | ~150 行 |
| **合计** | **~800 行** |

---

## 5. 阶段 C：应用基础设施

**目标**：把 smart_edge 从一个单文件 demo 变成正规的工程化应用。

### 新增 6 个组件

| # | 组件 | 行数 | 学习点 |
|---|------|------|--------|
| 1 | **多线程框架** | ~300 | pthread_create/join, 线程安全 |
| 2 | **INI 配置文件** | ~200 | 解析 key=value, section, 注释 |
| 3 | **分级日志** | ~200 | ERROR/WARN/INFO/DEBUG, 输出到文件+终端 |
| 4 | **systemd 服务** | ~50 | 自启动、崩溃重启、日志集成 |
| 5 | **CRC 校验** | ~100 | CRC16/CRC32 计算和验证 |
| 6 | **数据环形缓冲** | ~150 | 内存中保留最近 N 条传感器记录 |

#### 多线程框架 (~300行)

```c
// smart_edge.c 重构: 从单线程 → 5 线程
//
// 线程1: main_thread      epoll 事件循环 (从 smart_monitor 迁移)
// 线程2: mqtt_thread       MQTT 连接/上报/订阅 (阶段 D 实现)
// 线程3: http_thread       HTTP API 服务 (阶段 D 实现)
// 线程4: timer_thread      定时采集 + 阈值告警 + 写 SQLite (阶段 D)
// 线程5: can_thread        CAN 总线收发 (阶段 B 衔接)

// 共享数据保护:
pthread_mutex_t sensor_mutex;   // 传感器数据读写锁
pthread_mutex_t config_mutex;   // 配置读写锁

// 线程间通信:
mqd_t cmd_mq;   // POSIX 消息队列, 主线程 → 工作线程
```

#### INI 配置文件 (~200行)

```ini
# /etc/smart_edge/config.ini

[system]
log_level = INFO          # ERROR | WARN | INFO | DEBUG
log_file  = /var/log/smart_edge.log

[sensors]
poll_interval = 2        # 传感器轮询间隔(秒)
temp_high_threshold = 35  # 高温告警阈值(°C)
temp_low_threshold  = 0   # 低温告警阈值(°C)
distance_alert_cm   = 30  # 距离告警阈值(cm)

[mqtt]
enabled  = 1
broker   = 192.168.1.100
port     = 1883
client_id = imx6ull_edge_001
topic_prefix = edge/room1
publish_interval = 30    # 上报间隔(秒)

[web]
enabled = 1
port    = 8080

[can]
enabled  = 0
bitrate  = 500000

[modbus]
enabled   = 1
slave_id  = 1
uart_dev  = /dev/uart_sensor
baudrate  = 9600
```

#### 分级日志 (~200行)

```c
// 4 级日志, 输出到 stdout + 文件, 带时间戳和线程 ID
#define LOG_ERROR(fmt, ...)  log_write(LOG_ERROR, __FILE__, __LINE__, fmt, ...)
#define LOG_WARN(fmt, ...)   log_write(LOG_WARN,  __FILE__, __LINE__, fmt, ...)
#define LOG_INFO(fmt, ...)   log_write(LOG_INFO,  __FILE__, __LINE__, fmt, ...)
#define LOG_DEBUG(fmt, ...)  log_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ...)

// 输出示例:
// [2026-06-09 15:30:01][INFO][main:245] 传感器数据更新: temp=27.3°C hum=65%
// [2026-06-09 15:30:32][WARN][mqtt:89] MQTT 连接断开, 3秒后重试...
```

#### systemd 服务 (~50行)

```ini
# /etc/systemd/system/smart_edge.service
[Unit]
Description=Smart Environment Edge Node
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/smart_edge
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target

# systemctl enable smart_edge   → 开机自启
# systemctl start smart_edge    → 启动
# journalctl -u smart_edge -f   → 查看日志
```

#### CRC 校验 (~100行)

```c
// CRC16-Modbus, CRC32 两种
// 用于: Modbus 帧校验 / 传感器数据完整性 / OTA 固件校验
uint16_t crc16_modbus(const uint8_t *data, size_t len);
uint32_t crc32(const uint8_t *data, size_t len);
```

### 阶段 C 代码量

| 类别 | 行数 |
|------|------|
| 多线程重构 smart_edge | ~300 行 |
| INI 配置文件解析 | ~200 行 |
| 分级日志 | ~200 行 |
| CRC 校验 | ~100 行 |
| 数据环形缓冲 | ~150 行 |
| systemd 配置 | ~50 行 |
| **合计** | **~1,000 行** |

---

## 6. 阶段 D：云端 + Web + 告警

**目标**：数据上云、浏览器查看、异常自动告警。

### 新增 5 个组件

| # | 组件 | 行数 | 学习点 |
|---|------|------|--------|
| 1 | **MQTT 客户端** | ~300 | PUBLISH/SUBSCRIBE, QoS, 重连, keep-alive |
| 2 | **SQLite 存储** | ~200 | 建表/插入/查询/聚合/索引 |
| 3 | **HTTP 服务器** | ~300 | RESTful API, JSON, CORS |
| 4 | **Web 仪表盘** | ~200 | HTML5 + Chart.js + fetch API |
| 5 | **邮件告警** | ~150 | SMTP 或 curl sendmail |

#### MQTT 客户端 (~300行)

```
库: paho.mqtt.embedded-c (纯C, ~50KB)

发布主题 (PUBLISH):
  edge/room1/temperature       {"value":27.3,"unit":"C","ts":1717920000}
  edge/room1/humidity          {"value":65.0,"unit":"%","ts":1717920000}
  edge/room1/light             {"value":850,"unit":"lux"}
  edge/room1/air_quality       {"value":3,"unit":"level"}  ← MQ135
  edge/room1/distance          {"value":120,"unit":"cm"}
  edge/room1/imu               {ax,ay,az,gx,gy,gz,temp}
  edge/room1/status            {"uptime":3600,"sensors":9}

订阅主题 (SUBSCRIBE):
  edge/room1/cmd/led           "ON" | "OFF" | "BLINK=500"
  edge/room1/cmd/relay         "ON" | "OFF"
  edge/room1/cmd/servo         "45"  (角度)
  edge/room1/cmd/reboot        (远程重启)

安全性(可选): TLS/SSL 加密 MQTT 连接
```

#### SQLite 存储 (~200行)

```sql
-- 传感器数据表
CREATE TABLE sensor_data (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT DEFAULT (datetime('now','localtime')),
    sensor    TEXT NOT NULL,
    value     REAL NOT NULL,
    unit      TEXT
);

-- 事件日志表
CREATE TABLE events (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT DEFAULT (datetime('now','localtime')),
    type      TEXT,        -- 'ALARM' | 'CMD' | 'SYSTEM'
    message   TEXT
);

-- 常用查询
SELECT * FROM sensor_data
  WHERE sensor='temperature'
  AND timestamp >= datetime('now','-24 hours')
  ORDER BY timestamp;

-- 数据保留: 超过30天的自动删除 (timer线程处理)
```

#### HTTP 服务器 + Web 仪表盘 (~500行)

```
库: libmicrohttpd (纯C, GNU, ~100KB, 嵌入式友好)

API 端点:
  GET  /                   → 返回仪表盘 HTML
  GET  /api/sensors        → 当前所有传感器值 (JSON)
  GET  /api/history?sensor=temperature&hours=24  → 历史数据 (JSON)
  GET  /api/events?limit=50  → 最近事件
  GET  /api/export?sensor=temperature&format=csv → CSV 下载
  POST /api/cmd/led        body: "ON"      → 控制 LED
  POST /api/cmd/relay      body: "ON"      → 控制继电器
  POST /api/cmd/servo      body: "90"      → 舵机转 90°

Web 仪表盘 (单页 HTML):
  ┌──────────────────────────────────────────────┐
  │  🌡 27.3°C    💧 65%    ☀ 850 lux           │  数值卡片
  │  🌬 良         📏 120cm  🔌 LED:ON          │
  ├──────────────────────────────────────────────┤
  │  📈 温湿度 24h 曲线 (Chart.js)               │  折线图
  ├──────────────────────────────────────────────┤
  │  [LED ON] [LED OFF] [舵机 90°] [继电器 ON]  │  按钮
  ├──────────────────────────────────────────────┤
  │  最近事件:                                   │  事件列表
  │  15:30 温度 27.3°C                           │
  │  15:29 继电器 ON                             │
  └──────────────────────────────────────────────┘
```

#### 邮件告警 (~150行)

```
方案: 用 libcurl 或直接 system("curl ...") 调 SMTP API
  温度 > 阈值 → 发邮件 / 微信通知 / 钉钉机器人

告警规则 (在 timer 线程中检查):
  温度 > 35°C        → 告警
  湿度 > 90%         → 告警
  空气质量 = 重度污染 → 告警
  距离 < 30cm        → 告警 (有人靠近)
  连续 N 次读取失败  → 传感器离线告警

防抖: 连续 3 次超阈值才告警, 避免误报
静默: 同类型告警 5 分钟内不重复发送
```

### 阶段 D 代码量

| 类别 | 行数 |
|------|------|
| MQTT 客户端 | ~300 行 |
| SQLite 封装 | ~200 行 |
| HTTP 服务器 | ~300 行 |
| Web 仪表盘 HTML | ~200 行 |
| 邮件告警 | ~150 行 |
| **合计** | **~1,150 行** |

---

## 7. 阶段 E：工程化 & DevOps

**目标**：学会构建完整系统、管理编译环境、自动化测试。

### 新增 4 个组件

| # | 组件 | 学习点 |
|---|------|--------|
| 1 | **Docker 编译环境** | Dockerfile, docker-compose, 可复现构建 |
| 2 | **Buildroot 包** | 把 smart_edge 做成 Buildroot 自定义包 |
| 3 | **GitHub Actions CI** | 推送自动编译, 检查警告, 运行测试 |
| 4 | **OTA 设计** | 固件升级方案设计 (不要求实现完整系统) |

#### Docker 编译环境

```dockerfile
# docker/Dockerfile
FROM ubuntu:16.04
RUN apt-get update && apt-get install -y \
    gcc-arm-linux-gnueabihf \
    build-essential \
    libsqlite3-dev \
    libmicrohttpd-dev \
    cmake
# ... 挂载项目目录, 一键 make
```

#### Buildroot 包

```
buildroot/package/smart_edge/
├── Config.in           ← 在 menuconfig 中可以看到 smart_edge 选项
└── smart_edge.mk       ← 编译规则 (make / make install)

效果: make smart_edge-rebuild → 自动交叉编译全部驱动+应用
```

#### GitHub Actions CI

```yaml
# .github/workflows/build.yml
name: Build
on: [push, pull_request]
jobs:
  build:
    runs-on: ubuntu-latest
    container: ubuntu:16.04
    steps:
      - uses: actions/checkout@v4
      - run: apt install -y gcc-arm-linux-gnueabihf ...
      - run: make all
      - run: make test
```

#### OTA 升级方案设计

```
设计文档 (不要求完整实现):
  1. 固件版本管理: /etc/smart_edge/version
  2. 升级流程: 下载 → 校验 CRC32 → 写入备用分区 → 切换 → 回滚
  3. 断点续传: HTTP Range 请求
  4. A/B 分区: 当前分区 + 备用分区

实现简化版:
  - HTTP GET /api/update?version=2.0 → 下载 new_smart_edge
  - crc32 校验 → chmod +x → kill 旧进程 → exec 新的
  - 失败回滚: 保留旧版本 backup, 新版本启动失败 3 次自动恢复
```

### 阶段 E 代码量

| 类别 | 行数 |
|------|------|
| Dockerfile + docker-compose | ~100 行 |
| Buildroot 包配置 | ~50 行 |
| GitHub Actions CI | ~50 行 |
| OTA 简化实现 | ~300 行 |
| **合计** | **~500 行** |

---

## 8. 目录结构变更

```
imx6ull_project/
│
├── driver/                          ← 12 个驱动
│   ├── comp_drv.c          [有]    LED (cdev+ioctl+poll+fasync+定时器)
│   ├── comp_drv.h          [有]
│   ├── key_input.c         [有]    KEY (GPIO IRQ+input子系统)
│   ├── ap3216c.c           [有]    I2C 光感 (SMBus+i2c_transfer)
│   ├── icm20608.c          [有]    SPI 6轴 (SPI burst+ioctl)
│   ├── icm20608.h          [有]
│   ├── uart_sensor.c       [有]    UART (ioremap+readb/writeb+kfifo)
│   │
│   ├── dht11.c             [A新]   ★ 单总线 温湿度 + misc
│   ├── sr04.c              [A新]   ★ 超声波 脉冲测量 + completion
│   ├── mq135_adc.c         [A新]   ★ ADC 空气质量
│   ├── servo_pwm.c         [A新]   ★ PWM 舵机
│   ├── rtc_drv.c           [A新]   ★ RTC 片上时钟
│   ├── wdt_drv.c           [A新]   ★ Watchdog 看门狗
│   ├── relay.c             [A新]   ★ GPIO 继电器
│   ├── can_drv.c           [B新]   ★ CAN 2.0 SocketCAN
│   └── Makefile
│
├── app/                             ← 应用层
│   ├── smart_edge.c        [C重构] ★ 5线程守护进程
│   ├── config.c/h          [C新]   ★ INI 配置文件解析
│   ├── logger.c/h          [C新]   ★ 分级日志
│   ├── crc.c/h             [C新]   ★ CRC16/CRC32
│   ├── ringbuf.c/h         [C新]   ★ 环形缓冲
│   ├── mqtt_client.c/h     [D新]   ★ MQTT 云端通讯
│   ├── database.c/h        [D新]   ★ SQLite 存储
│   ├── web_server.c/h      [D新]   ★ HTTP API
│   ├── web_dashboard.html  [D新]   ★ Web 仪表盘
│   ├── alert.c/h           [D新]   ★ 阈值告警
│   ├── modbus_rtu.c/h      [B新]   ★ Modbus RTU 协议栈
│   ├── ota.c/h             [E新]   ★ OTA 升级
│   └── Makefile
│
├── config/                          [C新]
│   └── smart_edge.ini              ★ 配置文件示例
│
├── systemd/                         [C新]
│   └── smart_edge.service          ★ systemd 服务文件
│
├── buildroot/                       [E新]
│   └── package/smart_edge/
│       ├── Config.in               ★ Buildroot 包
│       └── smart_edge.mk
│
├── docker/                          [E新]
│   ├── Dockerfile                  ★ Docker 编译环境
│   └── docker-compose.yml
│
├── .github/workflows/              [E新]
│   └── build.yml                   ★ CI 自动编译
│
├── dts/               [有]
├── docs/              [有]
├── scripts/           [有]
├── test/              [有]  + 新增 ~10 个测试文件
└── Makefile           [有]  (更新, 增加 app 链接选项)
```

---

## 总览

### 最终技能矩阵 (42 个知识点)

```
总线外设    GPIO / I2C / SPI / UART / 单总线 / ADC / PWM / CAN
外设功能    RTC / Watchdog / 继电器 / 舵机 / LED / 蜂鸣器
传感器      AP3216C / ICM20608 / DHT11 / SR04 / MQ135
驱动机制    cdev / platform / misc / input / 中断 / 定时器
            / poll / fasync / kthread / completion / sysfs / debugfs / regmap
并 发       mutex / atomic / wait_queue / 多线程 / POSIX消息队列
网 络       UART命令 / I2C / SPI / CAN / Modbus RTU / MQTT / HTTP
存 储       CSV / kfifo / SQLite / 环形缓冲 / 数据导出
协 议       CRC校验 / JSON / INI解析 / MQTT / Modbus / CAN / HTTP
运 维       systemd / 交叉编译 / Docker / Buildroot / GitHub CI / OTA
安 全       TLS(可选) / 告警 / 看门狗 / 文件权限
```

### 代码量预估

| 阶段 | 内容 | 代码量 |
|------|------|--------|
| A | 6 新驱动 + 6 内核机制 | ~1,820 行 |
| B | CAN + Modbus | ~800 行 |
| C | 多线程 + 配置 + 日志 + CRC + systemd | ~1,000 行 |
| D | MQTT + SQLite + HTTP + Web + 告警 | ~1,150 行 |
| E | Docker + Buildroot + CI + OTA | ~500 行 |
| **合计** | **42 知识点 / 12 驱动 / 完整 IoT 系统** | **~5,270 行** |

### 5 个阶段依赖关系

```
阶段 A (驱动广度)
  │
  ├──► 阶段 B (工业总线) ──► 阶段 C (应用基础设施)
  │                              │
  │                              ├──► 阶段 D (云+Web+告警)
  │                              │       │
  │                              │       └──► 阶段 E (工程化)
  │                              │
  │                              └── (B和C可并行)
  │
  └── A 是后续所有阶段的前置条件
```
