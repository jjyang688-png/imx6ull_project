# 需求规格说明书

## 1. 项目概述

### 1.1 项目名称

**工业边缘计算网关**（Industrial Edge Computing Gateway）

> v1.0：智能环境监测终端（Smart Environment Monitor）— 已完成
> v2.0：工业边缘计算网关 — 当前阶段

### 1.2 目标平台

| 项目 | 值 |
|------|-----|
| SoC | NXP i.MX6ULL（ARM Cortex-A7，单核 792MHz） |
| 开发板 | 正点原子 ALPHA i.MX6ULL |
| 内核版本 | Linux 4.1.15（正点原子官方 BSP） |
| 交叉编译器 | arm-linux-gnueabihf-gcc 5.4.0 |

### 1.3 项目目标

**v1.0（已完成）**：构建嵌入式 Linux 驱动框架，通过 5 个外设采集环境数据，实现 epoll 多路复用统一监控。

**v2.0（当前）**：扩展为工业边缘计算网关，新增 7 个驱动（覆盖 8 种总线类型），自研 C 基础库（libedge），实现传感器—执行器联动控制，建立完整测试体系。

---

## 2. 功能需求

### 2.1 内核驱动层

#### 2.1.1 已有驱动（v1.0，保留）

| 编号 | 驱动 | 设备文件 | 总线 | 描述 |
|------|------|---------|------|------|
| F-D01 | LED 控制 | `/dev/comp_drv` | GPIO | on/off/blink；阻塞/非阻塞 read；poll/select；异步通知 SIGIO；ioctl 设置闪烁周期；内核定时器自动闪烁；sysfs 属性；debugfs 调试节点 |
| F-D02 | 按键输入 | `/dev/input/eventX` | GPIO+IRQ | 双边沿中断触发；delayed_work 消抖（20ms）；Input 子系统上报 KEY_ENTER |
| F-D03 | AP3216C 光感 | `/dev/ap3216c` | I2C | SMBus 字节读写 + i2c_transfer 突发读（6 字节）；读取 IR/ALS/PS |
| F-D04 | ICM20608 6 轴 | `/dev/icm20608` | SPI | SPI message 突发读（14 字节）；ioctl 分别读取加速度/角速度/温度；硬件复位 + 初始化 |
| F-D05 | UART 控制台 | `/dev/uart_sensor` | UART3 裸寄存器 | ioremap(0x021EC000)；RX 中断 → kfifo_in → wake_up；阻塞/非阻塞 read；poll；TX 轮询发送；命令解析在用户空间 |

#### 2.1.2 新增驱动（v2.0）

| 编号 | 驱动 | 设备文件 | 总线 | 描述 | 驱动模型 |
|------|------|---------|------|------|---------|
| F-D06 | DHT11 温湿度 | `/dev/dht11` | GPIO 单总线 | 主机发起始信号(拉低18ms+拉高40μs)；ktime 测量高电平长度区分 bit 0/1；读取 40bit(湿度+温度+校验)；2s 采样间隔；poll 支持 | misc |
| F-D07 | SR04 超声波 | `/dev/sr04` | GPIO+IRQ | TRIG 发 10μs 脉冲；ECHO 上升沿/下降沿中断 + ktime 计脉宽；距离 = 脉宽(ns)/58000(cm)；completion 同步；poll 支持；1s 超时 | misc |
| F-D08 | MQ135 空气质量 | `/dev/mq135` | ADC | ioremap ADC1 寄存器(0x02198000)；kthread 每 2s 采集；ADC 值 → 5 级空气质量(优/良/轻度/中度/重度)；sysfs 暴露 raw/quality | misc |
| F-D09 | 舵机控制 | `/dev/servo` | PWM | ioremap PWM 寄存器；周期 20ms(50Hz)；占空比 0.5~2.5ms → 0°~180°；write 角度值；read 当前角度 | misc |
| F-D10 | 继电器控制 | `/dev/relay` | GPIO | gpiod 输出；write "on"/"off"；read 当前状态；sysfs 状态属性 | misc |
| F-D11 | CAN 通信 | `/dev/can_ctrl` | CAN 2.0B | SocketCAN 字符设备封装；read CAN 帧(id+dlc+data)；write 发送帧；ioctl 设波特率/过滤器；select/poll 支持 | cdev |
| F-D12 | 看门狗 | `/dev/wdt_custom` | WDOG | ioremap WDOG 寄存器；write 喂狗；read 剩余时间；ioctl 设超时/查剩余；10s 默认超时 | misc |

### 2.2 驱动接口要求

| 接口类型 | 用途 | 涉及驱动 |
|---------|------|---------|
| `cdev` | 标准字符设备（open/read/write/ioctl/poll） | comp_drv, icm20608, uart_sensor, can_drv |
| `misc` 设备 | 简化字符设备注册 | dht11, sr04, mq135, servo, relay, wdt |
| `sysfs` 属性 | `/sys/class/misc/<dev>/属性` 直接 cat 查看 | 所有 misc 设备 |
| `debugfs` | `/sys/kernel/debug/` 开发调试 | comp_drv（已有） |
| `ioctl` | 设备特定命令 | comp_drv, icm20608, can_drv, wdt |
| `poll` | epoll/select/poll 支持 | comp_drv, uart_sensor, dht11, sr04, mq135, can_drv |
| `fasync` | SIGIO 异步通知 | comp_drv |

### 2.3 内核机制覆盖

| 机制 | 使用驱动 | 用途 |
|------|---------|------|
| `wait_queue` + `poll_wait` | comp_drv, uart_sensor, dht11, sr04, mq135 | 阻塞等待数据就绪 |
| `completion` | sr04 | 等待回波中断完成 |
| `kthread` | mq135_adc | 驱动内周期采样 |
| `kfifo` | uart_sensor | 环形缓冲接收数据 |
| `delayed_work` | key_input | 按键消抖 |
| `timer` | comp_drv | LED 定时闪烁 |
| `atomic` | comp_drv | 状态变化标记 |
| `mutex` | comp_drv | 设备数据保护 |

---

## 3. libedge 基础库需求

### 3.1 定位

自研 C 基础库，提供项目通用的数据结构、算法和工具函数，独立编译为 `libedge.a`，被应用层链接使用。

### 3.2 模块清单

| 编号 | 模块 | 文件 | 功能 | 关键算法 |
|------|------|------|------|---------|
| L-01 | 环形缓冲 | `ringbuf.h/c` | 泛型 FIFO 队列，O(1) 读写 | 宏实现，类型安全 |
| L-02 | 双向链表 | `linked_list.h/c` | 哨兵节点链表，增删查改排序 | 归并排序 |
| L-03 | CRC 校验 | `crc.h/c` | CRC8 / CRC16-Modbus / CRC32 | 查表法 |
| L-04 | 滑动滤波 | `filter.h/c` | 滑动平均（O(1)窗口和）+ 中值滤波 | 插入排序 |
| L-05 | 分级日志 | `logger.h/c` | ERROR/WARN/INFO/DEBUG 四级；时间戳；输出到 stdout+文件 | — |
| L-06 | INI 解析 | `ini_parser.h/c` | 解析 [section] key=value # 注释 | 逐行状态机 |
| L-07 | 消息队列 | `msgqueue.h/c` | POSIX mq_open/send/receive 封装 | — |
| L-08 | 错误码 | `edge_error.h/c` | 统一错误码枚举 + 描述字符串 | — |

### 3.3 质量要求

- 每个模块配单元测试（Unity 框架），覆盖率 > 80%
- CRC 模块必须有标准测试向量验证
- 环形缓冲/链表必须有边界条件测试（空/满/单元素）

---

## 4. 用户空间程序

### 4.1 smart_monitor（v2.0 扩展）

| 编号 | 变更 | 说明 |
|------|------|------|
| F-U01 | 设备 fd 扩展 | 从 6 个 fd → 13 个 fd（新增 7 个驱动设备 + stdin） |
| F-U02 | epoll 注册扩展 | `MAX_DEVICES` 从 6 → 13，非关键设备打开失败不致命 |
| F-U03 | UART 命令扩展 | 新增 RELAY / SERVO / CAN 三条命令 |
| F-U04 | 联动规则 | 温度 > 35°C → 继电器吸合；距离 < 30cm → 蜂鸣器告警；空气质量 ≥ 中度污染 → 蜂鸣器告警 |
| F-U05 | 日志替换(可选) | CSV → libedge logger 分级日志 |

### 4.2 测试程序

| 编号 | 程序 | 测试对象 |
|------|------|---------|
| T-01 | `test_comp_drv` | LED 驱动功能测试 |
| T-02 | `test_key` | 按键驱动功能测试 |
| T-03 | `test_ap3216c` | AP3216C 传感器测试 |
| T-04 | `test_icm20608` | ICM20608 传感器测试 |
| T-05 | `test_dht11` | DHT11 温湿度测试 |
| T-06 | `test_sr04` | SR04 超声波测试 |
| T-07 | `test_mq135` | MQ135 ADC 测试 |
| T-08 | `test_servo` | 舵机 PWM 测试 |
| T-09 | `test_relay` | 继电器控制测试 |
| T-10 | `test_can` | CAN 收发测试 |
| T-11 | `test_wdt` | 看门狗测试 |
| T-12 | `test_ringbuf` | libedge 环形缓冲单元测试 |
| T-13 | `test_linked_list` | libedge 链表单元测试 |
| T-14 | `test_crc` | libedge CRC 单元测试 |
| T-15 | `test_filter` | libedge 滤波单元测试 |
| T-16 | `test_ini_parser` | libedge INI 解析单元测试 |

---

## 5. UART 命令控制台

### 5.1 命令集（v2.0 扩展）

| 命令 | 格式 | 响应 | 版本 |
|------|------|------|------|
| STATUS | `STATUS` | `[STATUS] Uptime: xxx sec, Sensors: x/12` | v1.0 |
| SENSOR | `SENSOR` | 全部传感器当前值（温湿度/光照/距离/空气质量/姿态） | v1.0 |
| LED | `LED ON` / `LED OFF` / `LED BLINK=500` | LED 状态确认 | v1.0 |
| RELAY | `RELAY ON` / `RELAY OFF` | 继电器状态确认 | **v2.0 新增** |
| SERVO | `SERVO <0~180>` | 舵机角度确认 | **v2.0 新增** |
| CAN | `CAN SEND <id>:<data>` | CAN 帧发送确认 | **v2.0 新增** |
| HELP | `HELP` | 列出所有命令 | v1.0 |
| RESET | `RESET` | 重置所有设备到默认状态 | v1.0 |

### 5.2 串口分工

| 串口 | 物理接口 | 用途 | 驱动 |
|------|------|------|------|
| UART1 | Micro USB（CH340） | 系统调试终端（内核日志、Shell） | 内核 imx 驱动 |
| UART3 | DB9 RS232（SP3232） | 命令控制台（本项目） | uart_sensor.c |

---

## 6. 传感器—执行器联动规则

| 编号 | 规则 | 触发条件 | 动作 |
|------|------|---------|------|
| R-01 | 高温排风 | 温度 > 35°C（连续 3 次） | 继电器吸合（开排风扇）+ LED 红灯闪烁 |
| R-02 | 高温恢复 | 温度 < 30°C（连续 3 次） | 继电器断开 + LED 绿灯 |
| R-03 | 空气污染 | 空气质量 ≥ 4（中度污染） | 蜂鸣器告警 + 继电器吸合 |
| R-04 | 人员接近 | 距离 < 30cm（连续 2 次） | 蜂鸣器告警 |
| R-05 | 设备振动异常 | ICM20608 加速度幅值 > 阈值 | LED 告警 |
| R-06 | 系统死机 | WDT 超时 | 系统自动复位 |

---

## 7. 非功能需求

### 7.1 代码质量

- 所有驱动通过 Linux `checkpatch.pl` 检查
- 错误处理使用标准 goto 清理模式
- libedge 模块编译零警告（`-Wall -Wextra`）
- 统一命名规范：驱动名 = 功能名（snake_case）

### 7.2 文档要求

| 文档 | 文件 | 内容 |
|------|------|------|
| 需求规格 | `requirements.md` | 本文件 |
| 架构设计 | `architecture.md` | 系统分层架构 |
| API 参考 | `api-reference.md` | 设备接口参考 |
| 硬件资源 | `hardware.md` | 硬件接线和引脚分配 |
| 经验总结 | `lessons-learned.md` | 知识点汇总 |
| 执行流程 | `execution-flow.md` | v1.0 完整执行链路 |
| 阻塞/唤醒详解 | `poll-blocking-wakeup.md` | 内核 poll/wake_up 全链路 |
| 扩展方案 | `expansion-plan.md` | v2.0 扩展设计方案 |
| 扩展执行流程 | `expansion-execution-flow.md` | v2.0 执行流程 |
| 工业网关设计 | `industrial-edge-gateway.md` | 工业网关架构设计 |
| 阶段报告 | `phases/phase1~9_report.md` | 9 阶段开发报告 |

### 7.3 构建系统

- 内核模块使用 Kbuild 框架（`driver/Makefile`）
- 用户程序链接 `libedge.a`（`app/Makefile`）
- libedge 独立编译为静态库
- 交叉编译脚本 `scripts/build.sh`

### 7.4 测试体系

```
                    压力测试 (24h 稳定性)
                   ─────────────────────
                  集成测试 (多设备联动)
                 ───────────────────────
               功能测试 (每个驱动 open/read/write/ioctl)
              ────────────────────────────────────────
             单元测试 (libedge 每个模块, Unity 框架)
             ─────────────────────────────────────────
```

---

## 8. 硬件资源规划

### 8.1 v1.0 已有

| 外设 | 接口 | 引脚/地址 | 备注 |
|------|------|----------|------|
| LED | GPIO | GPIO1_IO03 | 低电平有效 |
| KEY0 | GPIO | GPIO1_IO18 | 低电平有效，双边沿中断 |
| AP3216C | I2C1 | 0x1E | 光照/接近传感器 |
| ICM20608 | ECSPI1 | CS0 | 6 轴传感器，SPI Mode 0，max 8MHz |
| UART3 | UART3 | DB9 RS232 | 命令控制台，115200 8N1 |

### 8.2 v2.0 新增

| 外设 | 接口 | 引脚 | 备注 |
|------|------|------|------|
| DHT11 | GPIO 单总线 | 待定 | 温湿度传感器，2s 采样间隔 |
| SR04 | GPIO×2 | TRIG + ECHO | 超声波测距，2cm~400cm |
| MQ135 | ADC1_INx | 待定 | 空气质量传感器，模拟输出 |
| 舵机 | PWMx | 待定 | SG90/MG996R，50Hz，0°~180° |
| 继电器 | GPIO | 待定 | 5V 继电器模块，控制排风扇/加热器 |
| CAN | CAN1 | CAN1_TX/RX | CAN 2.0B，需外接 CAN 收发器（TJA1050） |
| WDT | 片上 WDOG1 | — | 内部外设，无需外部引脚 |

---

## 9. 总线类型覆盖

| 总线 | 驱动 | 版本 |
|------|------|------|
| GPIO 输出 | comp_drv, relay | v1.0 / v2.0 |
| GPIO 输入 + IRQ | key_input | v1.0 |
| GPIO 单总线 | dht11 | v2.0 |
| I2C | ap3216c | v1.0 |
| SPI | icm20608 | v1.0 |
| UART（裸寄存器） | uart_sensor | v1.0 |
| ADC（裸寄存器） | mq135_adc | v2.0 |
| PWM（裸寄存器） | servo_pwm | v2.0 |
| CAN（SocketCAN） | can_drv | v2.0 |
| WDT（裸寄存器） | wdt | v2.0 |

**共计 10 种总线/外设类型。**

---

## 10. 里程碑

### 10.1 v1.0 已完成（阶段 1~9）

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | 项目立项与环境搭建 | ✅ |
| 2 | 设备树设计 | ✅ |
| 3 | LED 驱动（CDEV 基础） | ✅ |
| 4 | LED 驱动（IO 模型：阻塞/非阻塞/poll/fasync） | ✅ |
| 5 | 按键输入驱动（GPIO IRQ + input 子系统） | ✅ |
| 6 | I2C + SPI 传感器驱动（AP3216C + ICM20608） | ✅ |
| 7 | UART 裸寄存器驱动 | ✅ |
| 8 | 统一监控守护进程 smart_monitor | ✅ |
| 9 | 文档交付 + 发布 | ✅ |

### 10.2 v2.0 工业边缘网关（阶段 10~14）

| 阶段 | 内容 | 预计产出 |
|------|------|---------|
| 10 | libedge 基础库 | 8 个模块 + 单元测试 |
| 11 | 简单驱动（relay / wdt / servo） | 3 个 misc 驱动 + 功能测试 |
| 12 | 中等驱动（dht11 / sr04 / mq135 / can） | 4 个驱动 + 功能测试 |
| 13 | smart_monitor 扩展 + 联动规则 | 7 设备集成 + 联动逻辑 |
| 14 | 文档 + 测试 + 发布 | 完整测试报告 + v2.0.0 |

---

*文档版本：v2.0 | 日期：2026-06-10 | 阶段 10 起点*
