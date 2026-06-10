# 设备接口参考手册

本文档描述全部 12 个设备文件的用户空间编程接口。

> 标注 ★ 的为 v2.0 新增驱动

---

## 1. /dev/comp_drv — LED 控制驱动

### 1.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/comp_drv` |
| 驱动模块 | `comp_drv.ko` |
| 总线类型 | platform (GPIO) |
| GPIO | GPIO1_IO03 (active-low) |

### 1.2 接口

#### read() — 读取 LED 状态

```c
#include "../driver/comp_drv.h"

struct comp_drv_status {
    enum led_state state;
    int  blink_period_ms;
    unsigned long read_count;
    unsigned long write_count;
};
struct comp_drv_status st;
read(fd, &st, sizeof(st));
```

| 模式 | 行为 |
|------|------|
| 阻塞（默认） | LED 状态无变化时阻塞 |
| 非阻塞 (O_NONBLOCK) | 立即返回当前状态 |

#### write() — 控制 LED

```c
write(fd, "on",  2);   // LED 常亮
write(fd, "off", 3);   // LED 熄灭
```

#### ioctl() — 高级控制

| 命令 | 参数 | 说明 |
|------|------|------|
| `COMP_DRV_SET_BLINK_PERIOD` | `unsigned long period_ms` | 设置闪烁周期 |
| `COMP_DRV_GET_STATUS` | `struct comp_drv_status` | 获取完整状态 |
| `COMP_DRV_START_BLINK` | — | 启动自动闪烁 |
| `COMP_DRV_STOP_BLINK` | — | 停止闪烁并关灯 |

#### poll() / fasync / sysfs

```c
// poll
struct pollfd pfd = { .fd = fd, .events = POLLIN };
poll(&pfd, 1, -1);

// 异步通知 SIGIO
signal(SIGIO, handler);
fcntl(fd, F_SETOWN, getpid());
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | FASYNC);

// sysfs
cat /sys/class/comp_drv/comp_drv/led_state    // on / off / blink
```

---

## 2. /dev/input/eventX — 按键输入驱动

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/input/event0` |
| 驱动模块 | `key_input.ko` |
| GPIO | GPIO1_IO18 (active-low) |
| 键码 | `KEY_ENTER = 28` |

```c
#include <linux/input.h>
struct input_event ev;
read(fd, &ev, sizeof(ev));
// ev.type=EV_KEY, ev.code=28, ev.value=0(释放)/1(按下)/2(长按)
```

---

## 3. /dev/ap3216c — 光感/接近传感器

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/ap3216c` |
| 驱动模块 | `ap3216c.ko` |
| 总线 | I2C1, 地址 0x1E |

```c
struct ap3216c_data {
    unsigned short ir;    // 红外光强度
    unsigned short als;   // 环境光强度 (lux)
    unsigned short ps;    // 接近距离
};
struct ap3216c_data data;
read(fd, &data, sizeof(data));
```

---

## 4. /dev/icm20608 — 6 轴姿态传感器

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/icm20608` |
| 驱动模块 | `icm20608.ko` |
| 总线 | ECSPI3 CS0, Mode 0, 8MHz |

```c
#include "../driver/icm20608.h"

// read: 全部 7 个数据 (14 字节)
struct icm20608_full data;
read(fd, &data, sizeof(data));
// 温度换算: °C = data.temp / 340.0 + 36.53

// ioctl: 分轴读取
struct icm20608_data accel;
ioctl(fd, ICM20608_GET_ACCEL, &accel);
ioctl(fd, ICM20608_GET_GYRO,  &gyro);
ioctl(fd, ICM20608_GET_TEMP,  &temp_raw);
ioctl(fd, ICM20608_GET_ALL,   &all);
```

---

## 5. /dev/uart_sensor — UART 命令控制台

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/uart_sensor` |
| 驱动模块 | `uart_sensor.ko` |
| 物理接口 | UART3 (DB9 RS232) |
| 基地址 | 0x021EC000 |
| 波特率 | 115200-8-N-1 |

```c
char buf[256];
int n = read(fd, buf, sizeof(buf) - 1);   // 阻塞读
// 非阻塞: open(fd, O_RDWR | O_NONBLOCK)

write(fd, "response\r\n", 10);             // 发送到 PC

// poll
struct pollfd pfd = { .fd = fd, .events = POLLIN };
poll(&pfd, 1, -1);
```

---

## 6. ★ /dev/dht11 — DHT11 温湿度传感器 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/dht11` |
| 驱动模块 | `dht11.ko` |
| 总线 | GPIO 单总线 (1-Wire) |
| 驱动模型 | misc |

```c
// read: 返回 4 字节
uint8_t data[4];
read(fd, data, 4);
// data[0] = 湿度整数 (%)    data[1] = 湿度小数
// data[2] = 温度整数 (°C)   data[3] = 温度小数
// 校验: data[0]+data[1]+data[2]+data[3] 末字节 == data[4]

// poll: 数据就绪 (2s 采样间隔)
struct pollfd pfd = { .fd = fd, .events = POLLIN };
```

### sysfs

```
cat /sys/class/misc/dht11/temperature  → "27.3"
cat /sys/class/misc/dht11/humidity     → "65.0"
```

---

## 7. ★ /dev/sr04 — SR04 超声波测距 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/sr04` |
| 驱动模块 | `sr04.ko` |
| 总线 | GPIO × 2 (TRIG 输出 + ECHO 输入中断) |
| 驱动模型 | misc |
| 量程 | 2cm ~ 400cm |

```c
// read: 返回 uint32 距离值 (cm), 阻塞等待回波
uint32_t distance_cm;
int ret = read(fd, &distance_cm, sizeof(distance_cm));
if (ret == sizeof(distance_cm))
    printf("距离: %u cm\n", distance_cm);
// 非阻塞: open(fd, O_RDONLY | O_NONBLOCK), 无回波返回 -EAGAIN

// poll
struct pollfd pfd = { .fd = fd, .events = POLLIN };
```

### sysfs

```
cat /sys/class/misc/sr04/distance  → "125"
```

---

## 8. ★ /dev/mq135 — MQ135 空气质量 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/mq135` |
| 驱动模块 | `mq135_adc.ko` |
| 总线 | ADC1 (裸寄存器 ioremap) |
| 驱动模型 | misc |

```c
// read: 返回 uint16 ADC 原始值 (0~4095)
uint16_t adc_value;
read(fd, &adc_value, sizeof(adc_value));

// 空气质量等级换算 (由 kthread 维护):
//   0~400  → 优    401~800  → 良
//   801~1200 → 轻度  1201~1800 → 中度
//   >1800 → 重度
```

### sysfs

```
cat /sys/class/misc/mq135/raw      → "1840"
cat /sys/class/misc/mq135/quality   → "good" / "moderate" / "poor"
```

---

## 9. ★ /dev/servo — 舵机 PWM 控制 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/servo` |
| 驱动模块 | `servo_pwm.ko` |
| 总线 | PWM (裸寄存器 ioremap) |
| 驱动模型 | misc |

```c
// write: 设置角度 0~180
write(fd, "90", 2);    // 转到 90° (中间位置)
write(fd, "0", 1);     // 转到 0°
write(fd, "180", 3);   // 转到 180°

// read: 返回当前角度字符串 "90"
char angle[4];
read(fd, angle, sizeof(angle));
```

### sysfs

```
cat /sys/class/misc/servo/angle  → "90"
```

---

## 10. ★ /dev/relay — 继电器控制 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/relay` |
| 驱动模块 | `relay.ko` |
| 总线 | GPIO 输出 |
| 驱动模型 | misc |

```c
// write: 控制继电器
write(fd, "on",  2);   // 吸合
write(fd, "off", 3);   // 断开

// read: 返回当前状态
char state[4];
read(fd, state, sizeof(state));  // "on" / "off"
```

### sysfs

```
cat /sys/class/misc/relay/state  → "on" / "off"
```

---

## 11. ★ /dev/can_ctrl — CAN 总线通信 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/can_ctrl` |
| 驱动模块 | `can_drv.ko` |
| 总线 | CAN 2.0B (SocketCAN) |
| 驱动模型 | cdev |
| 默认波特率 | 500 Kbps |

```c
// read: 接收 CAN 帧
struct can_frame {
    uint32_t id;      // 11/29-bit ID
    uint8_t  dlc;     // 数据长度 (0~8)
    uint8_t  data[8]; // 数据字节
};
struct can_frame frame;
read(fd, &frame, sizeof(frame));

// write: 发送 CAN 帧
struct can_frame tx = { .id = 0x123, .dlc = 2, .data = {0xAB, 0xCD} };
write(fd, &tx, sizeof(tx));

// poll
struct pollfd pfd = { .fd = fd, .events = POLLIN };
```

### ioctl

| 命令 | 参数 | 说明 |
|------|------|------|
| `CAN_SET_BITRATE` | `int bitrate` | 设置波特率 (如 250000/500000) |
| `CAN_SET_FILTER` | `struct can_filter` | 设置接收过滤器 |

---

## 12. ★ /dev/wdt_custom — 看门狗 (v2.0)

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/wdt_custom` |
| 驱动模块 | `wdt.ko` |
| 总线 | WDOG1 (裸寄存器 ioremap) |
| 驱动模型 | misc |
| 默认超时 | 10 秒 |

```c
// write: 喂狗 (重载计数器, 防止复位)
write(fd, "feed", 4);

// read: 查询剩余时间 (秒)
uint32_t remaining;
read(fd, &remaining, sizeof(remaining));
printf("剩余 %u 秒\n", remaining);
```

### ioctl

| 命令 | 参数 | 说明 |
|------|------|------|
| `WDT_SET_TIMEOUT` | `int seconds` | 设置超时时间 (1~128 秒) |
| `WDT_GET_TIMELEFT` | `int *seconds` | 查询剩余时间 |

---

## 13. smart_monitor — 统一监控程序

### 命令行参数

```
./smart_monitor [选项]
  --no-log         不记录日志
  --interval N     传感器轮询间隔（秒，默认 2）
  --help           显示帮助
```

### 键盘交互

| 按键 | 功能 |
|------|------|
| q / ESC | 退出 |
| s | 系统状态 |
| 1 / 2 | LED 开/关 |
| h | 帮助 |

### UART 命令集 (v2.0)

| 命令 | 格式 | 功能 |
|------|------|------|
| STATUS | `STATUS` | 系统运行时间和传感器数量 |
| SENSOR | `SENSOR` | 全部传感器当前值 |
| LED | `LED ON` / `LED OFF` / `LED BLINK=500` | LED 控制 |
| RELAY | `RELAY ON` / `RELAY OFF` | 继电器控制 ★ |
| SERVO | `SERVO 0~180` | 舵机角度 ★ |
| CAN | `CAN SEND id:data` | CAN 帧发送 ★ |
| HELP | `HELP` | 命令列表 |
| RESET | `RESET` | 重置所有设备 |

---

*文档版本：v2.0 | 日期：2026-06-10*
