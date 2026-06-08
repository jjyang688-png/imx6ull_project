# 设备接口参考手册

本文档描述 5 个设备文件的用户空间编程接口，供应用开发者参考。

---

## 1. /dev/comp_drv — LED 控制驱动

### 1.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/comp_drv` |
| 驱动模块 | `comp_drv.ko` |
| 总线类型 | platform (GPIO) |
| GPIO | GPIO1_IO03 (active-low) |

### 1.2 接口列表

#### read() — 读取 LED 状态

```c
#include "../driver/comp_drv.h"

struct comp_drv_status {
    enum led_state state;        /* 0=OFF, 1=ON, 2=BLINK */
    int  blink_period_ms;       /* 闪烁周期(ms)，0=未闪烁  */
    unsigned long read_count;   /* 累计读次数              */
    unsigned long write_count;  /* 累计写次数              */
};

struct comp_drv_status st;
read(fd, &st, sizeof(st));
```

| 模式 | 行为 |
|------|------|
| 阻塞（默认） | LED 状态无变化时阻塞，变化后返回 |
| 非阻塞 (O_NONBLOCK) | 立即返回当前状态 |

#### write() — 控制 LED

```
echo on  > /dev/comp_drv    → LED 常亮
echo off > /dev/comp_drv    → LED 熄灭
echo 1   > /dev/comp_drv    → 同 on
echo 0   > /dev/comp_drv    → 同 off
```

```c
write(fd, "on",  2);   /* LED 常亮 */
write(fd, "off", 3);   /* LED 熄灭 */
```

**注意：** 驱动只识别小写 `"on"` / `"off"`，大写无效。

#### ioctl() — 高级控制

| 命令 | 方向 | 参数 | 说明 |
|------|------|------|------|
| `COMP_DRV_SET_BLINK_PERIOD` | 写 | `unsigned long period_ms` | 设置闪烁周期 |
| `COMP_DRV_GET_STATUS` | 读 | `struct comp_drv_status` | 获取完整状态 |
| `COMP_DRV_START_BLINK` | 无 | — | 启动自动闪烁 |
| `COMP_DRV_STOP_BLINK` | 无 | — | 停止闪烁并关闭 LED |

```c
unsigned long period = 500;
ioctl(fd, COMP_DRV_SET_BLINK_PERIOD, &period);
ioctl(fd, COMP_DRV_START_BLINK);   /* LED 以 500ms 周期闪烁 */
sleep(5);
ioctl(fd, COMP_DRV_STOP_BLINK);    /* 停止 */
```

#### poll() — 状态变化通知

```c
struct pollfd pfd = { .fd = fd, .events = POLLIN };
poll(&pfd, 1, -1);
if (pfd.revents & POLLIN) {
    /* LED 状态已变化，调用 read() 获取 */
    read(fd, &st, sizeof(st));
}
```

#### fasync — 异步通知 (SIGIO)

```c
signal(SIGIO, my_handler);          /* 注册信号处理函数     */
fcntl(fd, F_SETOWN, getpid());      /* 设置信号接收者       */
fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | FASYNC);
```

#### sysfs — 属性文件

```
cat /sys/class/comp_drv/comp_drv/led_state     → on / off / blink
cat /sys/class/comp_drv/comp_drv/write_count   → 累计写次数
```

---

## 2. /dev/input/eventX — 按键输入驱动

### 2.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/input/event0`（可能为 event1，取决于系统配置） |
| 驱动模块 | `key_input.ko` |
| 总线类型 | platform (GPIO + IRQ) |
| GPIO | GPIO1_IO18 (active-low, KEY0) |
| 键码 | `KEY_ENTER = 28` |

### 2.2 接口

```c
#include <linux/input.h>

struct input_event ev;
read(fd, &ev, sizeof(ev));

/*
 * ev.type  — EV_KEY (1) = 按键事件
 * ev.code  — 键码 (28 = KEY_ENTER)
 * ev.value — 0=释放, 1=按下, 2=长按
 */
```

| 操作 | 返回值 |
|------|------|
| 按键未按下 + 非阻塞模式 | `read()` 返回 -1, `errno = EAGAIN` |
| 按键按下/释放 | `read()` 返回 `sizeof(struct input_event)` |

---

## 3. /dev/ap3216c — 光感/接近传感器

### 3.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/ap3216c` |
| 驱动模块 | `ap3216c.ko` |
| 总线类型 | I2C1 |
| 从地址 | 0x1E |
| 芯片 ID | 上电默认值 0x00 (SYS_CONF 寄存器) |

### 3.2 接口

```c
struct ap3216c_data {
    unsigned short ir;    /* 红外光强度 (0~65535)    */
    unsigned short als;   /* 环境光强度 (lux)         */
    unsigned short ps;    /* 接近距离 (越小越近)      */
};

struct ap3216c_data data;
read(fd, &data, sizeof(data));
```

**无 ioctl、无 write、无 poll。** 每次 `read()` 触发一次 I2C 突发读，返回当前 3 个传感器值。

---

## 4. /dev/icm20608 — 6 轴姿态传感器

### 4.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/icm20608` |
| 驱动模块 | `icm20608.ko` |
| 总线类型 | ECSPI3 (Mode 0, max 8MHz) |
| 片选 | CS0 |
| 芯片 ID | WHO_AM_I (0x75) = 0x98 或 0xAF |

### 4.2 接口

#### read() — 读取全部 7 个数据（14 字节）

```c
#include "../driver/icm20608.h"

struct icm20608_full data;
read(fd, &data, sizeof(data));

/*
 * data.accel_x / _y / _z  — 加速度计原始值 (signed 16-bit)
 * data.gyro_x  / _y / _z  — 陀螺仪原始值 (signed 16-bit)
 * data.temp                — 温度原始值 (signed 16-bit)
 *
 * 温度换算: °C = data.temp / 340.0 + 36.53
 */
```

#### ioctl() — 分轴读取

| 命令 | 参数类型 | 说明 |
|------|------|------|
| `ICM20608_GET_ACCEL` | `struct icm20608_data` | 只读加速度计 (x/y/z) |
| `ICM20608_GET_GYRO` | `struct icm20608_data` | 只读陀螺仪 (x/y/z) |
| `ICM20608_GET_TEMP` | `int` | 只读温度原始值 |
| `ICM20608_GET_ALL` | `struct icm20608_full` | 一次读全部 7 个数据 |

```c
struct icm20608_data accel;
ioctl(fd, ICM20608_GET_ACCEL, &accel);

int temp_raw;
ioctl(fd, ICM20608_GET_TEMP, &temp_raw);
```

---

## 5. /dev/uart_sensor — UART 命令控制台

### 5.1 设备信息

| 属性 | 值 |
|------|------|
| 设备路径 | `/dev/uart_sensor` |
| 驱动模块 | `uart_sensor.ko` |
| 总线类型 | platform (裸 UART 寄存器) |
| 物理接口 | UART3 (DB9 RS232) |
| 基地址 | 0x021EC000 |
| 中断号 | GIC_SPI 28 (Linux IRQ 60) |
| 波特率 | 115200-8-N-1 |

### 5.2 接口

#### read() — 接收 PC 发来的数据

```c
char buf[256];
int n = read(fd, buf, sizeof(buf) - 1);
buf[n] = '\0';   /* n > 0 时为收到的字节数 */
```

| 模式 | 行为 |
|------|------|
| 阻塞（默认） | kfifo 为空时睡眠，等 RX 中断来唤醒 |
| 非阻塞 (O_NONBLOCK) | kfifo 为空时返回 -1, errno=EAGAIN |

#### write() — 发送数据给 PC

```c
write(fd, "[STATUS] Hello\r\n", 17);
/* 数据通过 UART3 TX → RS232 → PC 串口助手显示 */
```

#### poll() — 等待数据

```c
struct pollfd pfd = { .fd = fd, .events = POLLIN };
poll(&pfd, 1, -1);    /* 阻塞等待，直到 PC 发来数据 */
```

---

## 6. smart_monitor — 统一监控程序

### 6.1 基本信息

| 属性 | 值 |
|------|------|
| 程序路径 | `app/smart_monitor` |
| 依赖 | 5 个设备文件全部可用（非关键设备缺失仅打印警告） |

### 6.2 命令行参数

```
./smart_monitor [选项]

  --no-log         不记录 CSV 日志
  --interval N     传感器轮询间隔（秒，默认 2）
  --help           显示帮助
```

### 6.3 键盘交互（程序运行中）

| 按键 | 功能 |
|------|------|
| q / ESC | 退出程序 |
| s | 查询系统状态 (STATUS) |
| 1 | LED 开 |
| 2 | LED 关 |
| h | 显示键盘帮助 |

### 6.4 UART 命令（通过 PC 串口发送）

| 命令 | 格式 | 响应示例 |
|------|------|------|
| STATUS | `STATUS` | `[STATUS] Uptime: 3600 sec, Sensors Loaded: 2` |
| SENSOR | `SENSOR` | `[SENSOR] ir=320 als=450 lux ps=10  temp=36.5C acc=(...) gyro=(...)` |
| LED | `LED ON` / `LED OFF` | `[LED] LED turned ON` |
| HELP | `HELP` | 列出所有命令 |
| RESET | `RESET` | `[RESET] System reset OK` |

---

*文档版本：v1.0 | 日期：2026-06-08*
