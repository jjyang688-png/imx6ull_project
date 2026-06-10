# 工业边缘网关 — 驱动 + 基础库 实施方案

> **现阶段定位**：专注驱动层和 C 基础库，复用现有 smart_monitor 应用框架。
> C++ / 多线程 / Web 留到下一阶段。

---

## 基本策略

```
现在就能做的（你熟悉的）          后面再说（暂时跳过）
─────────────────────            ────────────────────
✅ C 驱动开发 (cdev/misc)         ⏸️ C++ 设备抽象层
✅ platform_driver / i2c / spi   ⏸️ 多线程框架
✅ 现有 epoll + UART 命令引擎     ⏸️ Modbus TCP / MQTT / HTTP
✅ 阻塞/非阻塞/poll/fasync        ⏸️ 数据管道 (Pipes & Filters)
✅ 纯 C 基础库 (libedge)          ⏸️ Web 仪表盘
✅ 单元测试 (Unity, 纯C)          ⏸️ 设计模式 (Factory/Strategy)
✅ Doxygen 文档                   ⏸️ SQLite
```

---

## 项目结构（仅 C 层）

```
imx6ull_project/
│
├── driver/                           ← 12 个驱动 (纯 C)
│   ├── comp_drv.c           [有]     LED (cdev + ioctl + poll + fasync)
│   ├── comp_drv.h           [有]
│   ├── key_input.c          [有]     KEY (platform + IRQ + input子系统)
│   ├── ap3216c.c            [有]     AP3216C (I2C SMBus)
│   ├── icm20608.c           [有]     ICM20608 (SPI burst + ioctl)
│   ├── icm20608.h           [有]
│   ├── uart_sensor.c        [有]     UART3 (ioremap + kfifo + RX中断)
│   │
│   ├── dht11.c              [新] ★   DHT11 温湿度 (misc + 单总线)
│   ├── sr04.c               [新] ★   SR04 超声波 (misc + 中断 + completion)
│   ├── mq135_adc.c          [新] ★   MQ135 空气质量 (misc + ADC + kthread)
│   ├── servo_pwm.c          [新] ★   舵机 PWM (misc + 占空比)
│   ├── relay.c              [新] ★   继电器 GPIO (misc)
│   ├── can_drv.c            [新] ★   CAN SocketCAN 封装 (cdev)
│   ├── wdt.c                [新] ★   看门狗 (misc + ioctl)
│   └── Makefile
│
├── libedge/                          ← ★ 自研 C 基础库 (对标 JD: 标准库维护)
│   ├── include/
│   │   ├── ringbuf.h                 │  泛型环形缓冲
│   │   ├── linked_list.h             │  双向链表
│   │   ├── crc.h                     │  CRC8/16/32 (查表法)
│   │   ├── filter.h                  │  滑动平均 / 中值滤波
│   │   ├── logger.h                  │  分级日志
│   │   ├── ini_parser.h              │  INI 配置文件解析
│   │   ├── msgqueue.h                │  POSIX 消息队列封装
│   │   └── edge_error.h              │  统一错误码
│   ├── src/
│   │   ├── ringbuf.c
│   │   ├── linked_list.c
│   │   ├── crc.c
│   │   ├── filter.c
│   │   ├── logger.c
│   │   ├── ini_parser.c
│   │   ├── msgqueue.c
│   │   └── Makefile
│   ├── tests/                        │  库的单元测试
│   │   ├── test_ringbuf.c
│   │   ├── test_linked_list.c
│   │   ├── test_crc.c
│   │   ├── test_filter.c
│   │   └── test_ini_parser.c
│   └── Makefile
│
├── app/                              ← 应用层 (沿用现有架构)
│   ├── smart_monitor.c      [扩展]   │  + 新增 7 个 fd 的 epoll 注册
│   │                                 │  + 新增 UART 命令 (RELAY/SERVO/CAN)
│   │                                 │  + CSV → 可选 logger 日志
│   └── Makefile                      │  链接 libedge.a
│
├── test/                             ← 驱动功能测试
│   ├── test_comp_drv.c      [有]
│   ├── test_ap3216c.c       [有]
│   ├── test_icm20608.c      [有]
│   ├── test_key.c           [有]
│   ├── test_dht11.c         [新]
│   ├── test_sr04.c          [新]
│   ├── test_mq135.c         [新]
│   ├── test_servo.c         [新]
│   ├── test_relay.c         [新]
│   ├── test_can.c           [新]
│   └── test_wdt.c           [新]
│
├── docs/                    [有+扩]  文档
├── dts/                     [有+扩]  设备树
├── scripts/                 [有]
└── Makefile                 [有+扩]  顶层编译
```

---

## 7 个新驱动设计

### 1. DHT11 — GPIO 单总线温湿度 (~350行)

```
设备节点: /dev/dht11
总线:     GPIO 单总线 (1-Wire 协议)
驱动模型: misc_register() — 比 cdev 更简洁

文件操作:
  open:   gpiod_get("dht11_data")     ← 申请 GPIO
  read:   返回 4 字节 (湿度整数/小数/温度整数/小数)
          协议流程:
            1. gpiod_set_value(0); mdelay(20);   // 主机拉低 20ms
            2. gpiod_set_value(1); udelay(40);    // 拉高 40μs
            3. gpiod_direction_input()            // 切换输入
            4. 等 DHT11 应答 (80μs低+80μs高)
            5. 读 40bit 数据 (ktime_get_ns 测高电平长度)
               · 26-28μs → bit 0
               · 70μs    → bit 1
            6. 校验: Hum_H + Hum_L + Temp_H + Temp_L == CheckSum
            7. copy_to_user()
  poll:   poll_wait, DHT11 有 2s 采样间隔
  release: gpiod_put
```

### 2. SR04 — 超声波测距 (~200行)

```
设备节点: /dev/sr04
总线:     GPIO + 中断 + ktime 脉冲测量
驱动模型: misc_register()

文件操作:
  open:   gpiod_get("trig")           ← TRIG 引脚 (输出)
          gpiod_get("echo")           ← ECHO 引脚 (输入)
          request_irq(echo_irq, sr04_isr, ...)  ← 注册中断
          init_completion(&done)       ← 完成量
  read:   gpiod_set_value(trig, 1); udelay(10); gpiod_set_value(trig, 0)
          wait_for_completion_interruptible_timeout(&done, 1*HZ)
          距离 = pulse_ns / 58000  (cm)
          copy_to_user(&dist, 4)
  poll:   poll_wait
  release: gpiod_put + free_irq

中断处理:
  sr04_isr():
    if (gpiod_get_value(echo))        // 上升沿
        start_ns = ktime_get_ns()
    else                               // 下降沿
        pulse_ns = ktime_get_ns() - start_ns
        complete(&done)               // 唤醒 read()
```

### 3. MQ135 — ADC 空气质量 (~250行)

```
设备节点: /dev/mq135
总线:     ADC (裸寄存器 ioremap)
驱动模型: misc_register()

文件操作:
  open:   ioremap(ADC_BASE)         ← 映射 ADC1 寄存器 (0x02198000)
          配置 ADC 通道 / 采样频率
          启动 kthread 周期采集
  read:   copy_to_user(adc_value)
  poll:   poll_wait
  release: iounmap + stop kthread

kthread: 每 2 秒读一次 ADC 结果寄存器
         把 ADC 值转换为 5 级空气质量:
           0~400   → 优
           401~800 → 良
           801~1200→ 轻度污染
           1201~1800→ 中度污染
           >1800   → 重度污染

sysfs:  /sys/class/misc/mq135/raw     → ADC 原始值
        /sys/class/misc/mq135/quality  → "good" 等字符串
```

### 4. Servo — PWM 舵机 (~250行)

```
设备节点: /dev/servo
总线:     PWM (裸寄存器 ioremap)
驱动模型: misc_register()

文件操作:
  open:   ioremap(PWM_BASE)          ← 映射 PWM 寄存器
          配置: 周期 20ms (50Hz), 极性正常
  write:  "90" → 占空比 = 1.5ms → 90°
          解析用户写入的角度值 (0~180)
          角度→占空比映射: duty_us = 500 + (angle * 2000 / 180)
          范围校验
          writel(period, PWM_SAR)
          writel(duty, PWM_PWMR)
  read:   返回当前角度
  release: iounmap

sysfs:  /sys/class/misc/servo/angle → "90"
```

### 5. Relay — GPIO 继电器 (~120行)

```
设备节点: /dev/relay
总线:     GPIO 输出
驱动模型: misc_register()

文件操作:
  open:   gpiod_get("relay")          ← 申请 GPIO，初始值 0 (断开)
  write:  "on"  → gpiod_set_value(1) → 继电器吸合
          "off" → gpiod_set_value(0) → 继电器断开
  read:   返回 "on" / "off"
  release: gpiod_put

sysfs:  /sys/class/misc/relay/state → "on" / "off"
```

### 6. CAN — SocketCAN 封装 (~300行)

```
设备节点: /dev/can_ctrl
方案:     内核已有 flexcan 驱动 (设备树使能即可)
          本驱动是对 SocketCAN 的字符设备封装，简化应用层操作

文件操作:
  open:   socket(PF_CAN, SOCK_RAW, CAN_RAW) → bind can0
          设置过滤器 (可选)
  read:   读 CAN 帧 → 返回 {id, dlc, data[8]}
          read(sockfd, &frame, sizeof(can_frame))
  write:  发送 CAN 帧
          写入 {id, dlc, data[8]} → 构造 can_frame → write(sockfd)
  ioctl:  CAN_SET_BITRATE → 500000
          CAN_SET_FILTER  → 只收特定 ID
  poll:   用 sockfd 的 poll 机制
  release: close(sockfd)
```

### 7. WDT — 看门狗 (~120行)

```
设备节点: /dev/wdt_custom
方案:     裸寄存器操作 i.MX6ULL WDOG

文件操作:
  open:   ioremap(WDOG_BASE)          ← 映射看门狗寄存器
          使能看门狗, 设置超时 (默认 10s)
  write:  写入任意值 → 喂狗 (重载计数器)
  read:   读取剩余时间 (秒)
  ioctl:  WDT_SET_TIMEOUT → 设置超时秒数
          WDT_GET_TIMELEFT → 查询剩余时间
  release: 不关闭 (看门狗一旦开启不能关)
```

---

## libedge 基础库

### 每个模块的设计

```
ringbuf      泛型环形缓冲, O(1) push/pop, 可用于传感器数据缓冲
linked_list  双向链表, 哨兵节点, push/pop/find/sort/foreach
crc          CRC8/CRC16-Modbus/CRC32, 查表法, 有标准测试向量
filter       滑动平均 (O(1)维护窗口和) / 中值滤波 (插入排序)
logger       4 级: ERROR/WARN/INFO/DEBUG, 输出到 stdout+文件, 带时间戳
ini_parser   解析 [section] key=value # 注释, 简单够用
msgqueue     POSIX mq_open/mq_send/mq_receive 封装, 减少 boilerplate
edge_error   统一错误码枚举, 错误描述字符串, 方便调试
```

### 环形缓冲示例

```c
// 泛型宏 — 一行定义一个环形缓冲
RINGBUF_DEFINE(sensor_rb, struct sensor_data, 100);

// 使用
struct sensor_rb rb;
ringbuf_init(&rb);

struct sensor_data sd = {.temp = 27.3, .hum = 65.0};
ringbuf_push(&rb, &sd);    // 入队

struct sensor_data out;
ringbuf_pop(&rb, &out);     // 出队 (FIFO)
ringbuf_peek(&rb, 5, &out); // 查看第 5 个 (不移除)
```

### CRC16 测试向量

```c
// test_crc.c
void test_crc16_modbus(void) {
    // Modbus 标准测试向量
    uint8_t data[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_UINT16(0x840A, crc16_modbus(data, 6));
}
```

---

## smart_monitor.c 扩展 (最小改动)

在现有 5 个 fd 基础上加 7 个，UART 命令引擎加几条命令：

```c
// ── 设备索引扩展 ──
enum {
    DEV_COMP_DRV = 0,   // LED          [有]
    DEV_KEY_INPUT,      // KEY          [有]
    DEV_AP3216C,        // I2C 光感     [有]
    DEV_ICM20608,       // SPI 6轴      [有]
    DEV_UART_SENSOR,    // UART         [有]
    DEV_STDIN,          // 键盘         [有]
    DEV_DHT11,          // ★ 单总线
    DEV_SR04,           // ★ 超声波
    DEV_MQ135,          // ★ ADC
    DEV_SERVO,          // ★ PWM
    DEV_RELAY,          // ★ GPIO
    DEV_CAN,            // ★ CAN
    DEV_WDT,            // ★ 看门狗
    MAX_DEVICES         // 13
};

// ── UART 命令扩展 ──
static struct uart_command uart_commands[] = {
    {"STATUS",   cmd_status,   "Show system status"},
    {"SENSOR",   cmd_sensor,   "Read all sensors"},
    {"LED",      cmd_led,      "LED ON|OFF|BLINK=ms"},
    {"RELAY",    cmd_relay,    "RELAY ON|OFF"},        // [新]
    {"SERVO",    cmd_servo,    "SERVO 0~180"},          // [新]
    {"CAN",      cmd_can_send, "CAN SEND id:data"},     // [新]
    {"HELP",     cmd_help,     "Show help"},
    {"RESET",    cmd_reset,    "Reset system"},
    {NULL, NULL, NULL}
};

// ── epoll 注册 (加 7 行) ──
// 在 setup_epoll() 的 for 循环里, MAX_DEVICES 从 6 变为 13
// 非关键设备打开失败不致命, 跳过即可
```

---

## 实施顺序

```
阶段1: libedge 基础库         (~1 周)
  ├── ringbuf + linked_list + crc
  ├── logger + ini_parser + msgqueue + edge_error
  └── 每个模块配单元测试 (Unity 框架)

阶段2: 简单驱动先行           (~1 周)
  ├── relay.c   (最简单, GPIO 输出, 30分钟)
  ├── wdt.c     (寄存器读写, 30分钟)
  ├── servo_pwm.c (PWM 寄存器, 半天)
  └── dht11.c   (单总线时序, 半天, 最有学习价值)

阶段3: 中等难度驱动           (~1 周)
  ├── sr04.c    (中断+ktime, 半天)
  ├── mq135_adc.c (ADC+kthread, 半天)
  └── can_drv.c (SocketCAN, 半天)

阶段4: 集成                   (~3 天)
  ├── smart_monitor.c 扩展 (加 fd + 命令)
  ├── 联动逻辑: 温度>30 → 继电器ON / 距离<20 → 蜂鸣器
  └── 集成测试

阶段5: 文档 + 测试            (~3 天)
  ├── 测试: 每个驱动写功能测试, libedge 单元测试全跑通
  ├── Doxygen 注释 + 生成文档
  └── README + 架构图
```

---

## 代码量预估

| 部分 | 行数 |
|------|------|
| 7 个新驱动 | ~1,600 行 |
| libedge (8模块) | ~1,200 行 |
| 单元测试 | ~800 行 |
| 功能测试 (7个) | ~500 行 |
| smart_monitor 扩展 | ~100 行 |
| 文档 | ~500 行 |
| **合计** | **~4,700 行** |

> 全部是纯 C，用你熟悉的技术栈。

---

## 和 JD 的对应 (仅 C 层就覆盖的)

| JD 要求 | 对应 |
|---------|------|
| 嵌入式系统软件开发 | 12 个驱动, 覆盖 6 种总线 |
| 测试和维护 | 单元测试 + 功能测试, Unity 框架 |
| 编写软件设计文档 | Doxygen + 设计文档 |
| 标准库维护 | libedge 自研基础库, 版本化 |
| C 语言 | 全部纯 C |
| 数据结构与算法 | ringbuf / linked_list(归并排序) / CRC16查表法 / 滑动滤波 / 中值滤波 |
| 串口通信 | UART 裸寄存器 + 命令协议 |
| 网口通信 | SocketCAN (网络协议栈) |
