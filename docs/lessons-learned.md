# 全项目知识点汇总

本文档按主题汇总全部知识点，每一条标注来源阶段。

> ★ 标注为 v2.0 新增知识点

---

## 一、Linux 内核驱动框架

### 驱动注册方式（6 种）

| 注册方式 | 使用场景 | 驱动 | 关键 API | 阶段 |
|------|------|------|------|------|
| `platform_driver` + CDEV | 平台设备 (GPIO/UART) | comp_drv, uart_sensor | `module_platform_driver()` | 3, 7 |
| `platform_driver` + Input | 输入设备 | key_input | `input_register_device()` | 5 |
| `i2c_driver` + CDEV | I2C 从设备 | ap3216c | `module_i2c_driver()` | 6 |
| `spi_driver` + CDEV | SPI 从设备 | icm20608 | `module_spi_driver()` | 6 |
| **`misc_register`** | **简单字符设备** ★ | **dht11, sr04, mq135, servo, relay, wdt** | **`misc_register(&miscdev)`** | **10-12** |
| **cdev + SocketCAN** | **CAN 设备** ★ | **can_drv** | **`cdev_init + socket(PF_CAN)`** | **12** |

### misc 设备 vs cdev ★

| | cdev | misc |
|------|------|------|
| 注册方式 | `alloc_chrdev_region` + `cdev_init` + `cdev_add` + `class_create` + `device_create` | `misc_register(&miscdev)` |
| 代码量 | ~30 行 | ~10 行 |
| 主设备号 | 动态分配 | 固定 10 |
| sysfs | 需手动 `device_create_file` | 自动创建 `/sys/class/misc/<name>/` |
| 适用场景 | 复杂设备 (多 ioctl/多属性) | 简单传感器/执行器 |

---

## 二、设备树 (DTS)

| 知识点 | 说明 | 阶段 |
|------|------|------|
| `compatible` | 驱动匹配字符串 | 2 |
| `reg` | I2C=从地址, SPI=片选, platform=基地址 | 2 |
| `interrupts` | 3 元组 `<GIC_SPI 号 触发类型>` | 2, 7 |
| `gpios` | `<&gpioX 编号 有效电平>` | 2 |
| `pinctrl-0` | 引用引脚复用配置 | 2 |
| `&uart3 { status = "disabled" }` | 禁用内核自带驱动 | 7 |
| **ADC 通道绑定** ★ | **`io-channels = <&adc1 0>`** | **12** |
| **PWM 输出绑定** ★ | **`pwms = <&pwm1 0 20000000>`** | **12** |
| **CAN 控制器使能** ★ | **`&flexcan1 { status = "okay" }`** | **12** |

---

## 三、总线与通信协议

### 总线类型总览 (10 种) ★

| 总线 | 驱动 | 速度 | 线数 | 阶段 |
|------|------|------|------|------|
| GPIO 输出 | comp_drv, relay | — | 1 | 3, 11 |
| GPIO 输入 + IRQ | key_input, sr04 | — | 1 | 5, 11 |
| **GPIO 单总线** ★ | **dht11** | **~1Kbps** | **1** | **11** |
| I2C | ap3216c | 100kHz | 2 | 6 |
| SPI | icm20608 | 8MHz | 4 | 6 |
| UART (裸寄存器) | uart_sensor | 115200 | 2 (RX/TX) | 7 |
| **ADC (裸寄存器)** ★ | **mq135_adc** | **~1Msps** | **1 (模拟)** | **12** |
| **PWM (裸寄存器)** ★ | **servo_pwm** | **50Hz** | **1** | **11** |
| **CAN 2.0B** ★ | **can_drv** | **500Kbps** | **2 (差分)** | **12** |
| **WDOG (裸寄存器)** ★ | **wdt** | — | **片上** | **11** |

### DHT11 单总线协议 ★

| 要点 | 说明 |
|------|------|
| 起始信号 | 主机拉低 18ms → 拉高 40μs |
| 从机应答 | 80μs 低 + 80μs 高 |
| bit 0 | 高电平 26-28μs |
| bit 1 | 高电平 70μs |
| 数据帧 | 40bit = 湿度整数 + 湿度小数 + 温度整数 + 温度小数 + 校验 |
| 采样间隔 | ≥ 2s |
| API | `gpiod_set_value` / `gpiod_get_value` / `ktime_get_ns` / `udelay` |

### SR04 脉冲测量 ★

| 要点 | 说明 |
|------|------|
| 触发 | TRIG 发 10μs 高脉冲 |
| 回波 | ECHO 返回高电平, 脉宽 ∝ 距离 |
| 距离换算 | cm = pulse_ns / 58000 |
| 中断 | ECHO 上升沿/下降沿各触发一次, 记录 ktime |
| 同步 | `init_completion` + `wait_for_completion_interruptible_timeout` |

### I2C vs SPI 对比

| | SPI | I2C |
|------|------|------|
| 线数 | 4 线 | 2 线 |
| 双工 | 全双工 | 半双工 |
| 速度 | 8MHz+ | 100~400kHz |
| 字节序 | 大端 (ICM20608) | 小端 (AP3216C) |
| DTS reg | CS 编号 | 从地址 |
| 驱动类型 | spi_driver | i2c_driver |

---

## 四、中断处理

| 机制 | 上下文 | 可睡眠？ | 场景 | 阶段 |
|------|------|------|------|------|
| 上半部 (ISR) | 中断 | 否 | 读寄存器、调度下半部 | 5, 7 |
| delayed_work | 进程 | 是 | 按键去抖 (20ms) | 5 |
| **kthread** ★ | **进程** | **是** | **ADC 周期采样** | **12** |
| **completion** ★ | **进程** | **是** | **等待 ECHO 回波完成** | **11** |

### 中断上下文约束

| 操作 | 进程上下文 | 中断上下文 |
|------|------|------|
| mutex_lock | ✅ | ❌ |
| kmalloc(GFP_KERNEL) | ✅ | ❌ |
| copy_to_user | ✅ | ❌ |
| kfifo_in | ✅ | ✅ |
| wake_up_interruptible | ✅ | ✅ |
| complete | ✅ | ✅ |

---

## 五、并发与同步

| 锁/机制 | 可睡眠？ | 使用场景 | 驱动 | 阶段 |
|------|------|------|------|------|
| mutex | 是 | 进程上下文保护设备状态 | comp_drv, ap3216c, icm20608, uart_sensor, mq135 | 4, 6, 7, 12 |
| spinlock | 否 | 中断上下文 | key_input | 5 |
| atomic | — | 标记/计数器 | comp_drv (changed) | 4 |
| wait_queue | 是 | 阻塞读 | comp_drv, uart_sensor | 4, 7 |
| **completion** ★ | **是** | **一次性同步** | **sr04** | **11** |
| **poll_wait (misc)** ★ | — | **epoll 支持** | **dht11, sr04, mq135, can** | **11-12** |

---

## 六、IO 模型

| IO 模型 | 驱动端 API | 用户端 API | 驱动 | 阶段 |
|------|------|------|------|------|
| 阻塞读 | `wait_event_interruptible()` | `read()` | comp_drv, uart_sensor | 4, 7 |
| **阻塞读 (completion)** ★ | **`wait_for_completion_*()`** | **`read()`** | **sr04** | **11** |
| 非阻塞读 | 检查 `O_NONBLOCK`, 返回 `-EAGAIN` | `open(O_NONBLOCK)` | comp_drv, uart_sensor, can | 4, 7, 12 |
| poll/epoll | `poll_wait()` | `poll()`/`epoll_wait()` | comp_drv, uart_sensor, dht11, sr04, mq135, can | 4, 7, 11-12 |
| 异步通知 | `kill_fasync()` | `SIGIO` | comp_drv | 4 |
| ioctl | `unlocked_ioctl` | `ioctl()` | comp_drv, icm20608, can, wdt | 4, 6, 12 |

### epoll 三件套

```c
int epfd = epoll_create1(0);
epoll_ctl(epfd, EPOLL_CTL_ADD, dev_fd, &event);
int nfds = epoll_wait(epfd, events, max, timeout_ms);
```

---

## 七、UART 裸寄存器操作

### 寄存器访问 API

| API | 用途 | 阶段 |
|------|------|------|
| `devm_ioremap_resource()` | 物理地址 → 虚拟地址 | 7 |
| `readb(addr)` / `writeb(val, addr)` | 8 位读写 (URXD/UTXD) | 7 |
| `readl(addr)` / `writel(val, addr)` | 32 位读写 (UCRx/USRx) | 7 |

### 初始化顺序

```
UCR2_SRST 复位 → UCR2 帧格式 → UCR3 DCE → UFCR 阈值
→ UBIR/UBMR 波特率 → UCR1+UCR4 总使能
```

### kfifo 环形缓冲

| API | 说明 |
|------|------|
| `DECLARE_KFIFO(name, type, size)` | 结构体中声明 |
| `INIT_KFIFO(fifo)` | 使用前初始化 |
| `kfifo_in(fifo, buf, n)` | 写入，中断安全 |
| `kfifo_to_user(fifo, ubuf, n, &copied)` | 取出 + copy_to_user |
| `kfifo_is_empty(fifo)` | 判空 |

---

## 八、用户空间编程

### 核心 API

| API | 用途 | 阶段 |
|------|------|------|
| `epoll_create1` / `epoll_ctl` / `epoll_wait` | IO 多路复用 | 8 |
| `struct input_event` | 读取按键事件 | 5, 8 |
| `fcntl(fd, F_SETFL, O_NONBLOCK)` | 设置非阻塞 | 8 |
| `/proc/uptime` | 系统运行秒数 | 8 |
| `access(path, F_OK)` | 检查文件是否存在 | 8 |

### 设计模式

| 模式 | 说明 | 应用 |
|------|------|------|
| 查表法 | `struct {name, handler}[]` 替代 if-else 链 | UART 命令引擎 |
| 行缓冲拼包 | `static char buf[] + line_pos` 处理字节流 | UART 接收 |
| Toggle 状态机 | `static int` 实现按键交替开关 | 按键处理 |
| 信号优雅退出 | `signal()` 只置标志位, 主循环判断退出 | smart_monitor |

---

## 九、数据结构与算法 ★

### libedge 模块 (v2.0 新增)

| 模块 | 数据结构 | 算法 | 复杂度 |
|------|------|------|------|
| **ringbuf** | **泛型环形缓冲 (宏实现)** | **O(1) push/pop** | O(1) |
| **linked_list** | **双向链表 (哨兵节点)** | **归并排序 O(n log n)** | O(1) 增删, O(n) 查找 |
| **crc** | **预计算查表 256 字节** | **CRC8/CRC16-Modbus/CRC32** | O(n) |
| **filter** | **滑动窗口 + 中值缓冲** | **滑动平均(维护和, O(1)) / 中值滤波** | O(n) |
| **logger** | **环形日志缓冲** | **4 级过滤 + 时间戳** | — |
| **ini_parser** | **状态机解析** | **逐行解析 key=value** | O(n) |
| **msgqueue** | **POSIX mq 封装** | **阻塞/非阻塞收发** | — |
| **edge_error** | **枚举 + 字符串映射** | **查表法错误描述** | O(1) |

### CRC16-Modbus 查表法 ★

```c
// 原理: 预计算 256 个 CRC 值, 查表 O(n)
// 标准测试向量: {0x01,0x03,0x00,0x00,0x00,0x01} → CRC16 = 0x840A
uint16_t crc16_modbus(const uint8_t *data, size_t len);
```

### 滑动平均滤波 ★

```c
// 原理: 维护窗口内元素和, 每次更新 O(1)
// new_avg = (old_avg * N - oldest + newest) / N
typedef struct {
    float *window; int size, idx, filled; float sum;
} moving_avg_t;
float moving_avg_update(moving_avg_t *f, float val);
```

---

## 十、新增外设学习点 ★

### ADC (MQ135) ★

| 知识点 | 说明 |
|------|------|
| ADC 寄存器 | ADCx_CFG / ADCx_GC / ADCx_HS / ADCx_R0 |
| 裸寄存器访问 | `ioremap(0x02198000)`, `readl(ADC1_R0)` |
| 软件触发 | 写 ADCx_HC0 触发一次转换 |
| kthread | `kthread_run(adc_thread, dev, "mq135_adc")` |

### PWM (舵机) ★

| 知识点 | 说明 |
|------|------|
| PWM 寄存器 | PWM_SAR (周期) / PWM_PWMR (占空比) / PWM_CR (控制) |
| 舵机周期 | 20ms (50Hz), 占空比 0.5~2.5ms → 0°~180° |
| 角度换算 | `duty_cycles = base + angle * range / 180` |

### CAN (SocketCAN) ★

| 知识点 | 说明 |
|------|------|
| 初始化 | `socket(PF_CAN, SOCK_RAW, CAN_RAW)` + `bind` |
| 发帧 | `write(sockfd, &frame, sizeof(can_frame))` |
| 收帧 | `read(sockfd, &frame, sizeof(can_frame))` |
| 过滤器 | `setsockopt(SOL_CAN_RAW, CAN_RAW_FILTER)` |

### Watchdog ★

| 知识点 | 说明 |
|------|------|
| 寄存器 | WDOGx_WCR (控制) / WDOGx_WSR (喂狗序列) |
| 喂狗 | 依次写 0x5555, 0xAAAA 到 WSR |
| 超时 | 默认 10s, 计算公式: `timeout = (WCR[7:0] + 1) × 0.5s` |

---

## 十一、字节序

| | ICM20608（大端） | AP3216C（小端） |
|------|------|------|
| 0x1234 存储 | `[0]=0x12, [1]=0x34` | `[0]=0x34, [1]=0x12` |
| 组合 | `(buf[0]<<8) \| buf[1]` | `(buf[1]<<8) \| buf[0]` |

---

## 十二、踩坑总录

| 问题 | 原因 | 解决 | 阶段 |
|------|------|------|------|
| RXDMUXSEL 忘设 | DCE/DTE 通路未选择 | `writel(RXDMUXSEL, UCR3)` | 7 |
| RXTL 设大不触发中断 | FIFO 阈值太高 | RXTL=1 | 7 |
| 大写"ON"→LED不响应 | comp_drv 只认小写 | 改为小写 | 8 |
| KEY0 按一次闪一下就灭 | 没过滤 value=0 | 只处理 value==1 | 8 |
| `spi_write_then_read` 不存在 | 4.1.15 无此 API | `spi_sync`+手动拼 transfer | 6 |
| `va_list` 编译报错 | 缺少 `<stdarg.h>` | 添加头文件 | 8 |
| **DHT11 时序不稳定** ★ | **中断打断导致 μs 级偏差** | **关本地中断 `local_irq_save`** | **11** |
| **SR04 测距偏大** ★ | **声速温度补偿缺失** | **或简化: 常温下 cm=us/58 足够** | **11** |
| **MQ135 读数跳跃** ★ | **ADC 噪声** | **用滑动平均滤波** | **12** |

---

## 十三、完整文件索引

### 驱动层
| 文件 | 规划行数 | 核心知识点 |
|------|------|------|
| `driver/comp_drv.c` | 520 | CDEV, mutex, wait_event, poll, fasync, ioctl, sysfs, debugfs, 定时器 |
| `driver/key_input.c` | 280 | Input 子系统, IRQ, delayed_work, spinlock |
| `driver/ap3216c.c` | 320 | I2C, SMBus, i2c_transfer, 小端字节序 |
| `driver/icm20608.c` | 450 | SPI, spi_message, 大端字节序, ioctl |
| `driver/uart_sensor.c` | 580 | ioremap, 裸寄存器, RX中断, kfifo, 阻塞/非阻塞, poll |
| `driver/dht11.c` ★ | 350 | misc, GPIO单总线, 时序控制, ktime, sysfs |
| `driver/sr04.c` ★ | 200 | misc, GPIO中断, 脉冲测量, completion |
| `driver/mq135_adc.c` ★ | 250 | misc, ioremap ADC, kthread, sysfs |
| `driver/servo_pwm.c` ★ | 250 | misc, ioremap PWM, 占空比, sysfs |
| `driver/relay.c` ★ | 120 | misc, gpiod, sysfs |
| `driver/can_drv.c` ★ | 300 | cdev, SocketCAN, ioctl, poll |
| `driver/wdt.c` ★ | 120 | misc, ioremap WDOG, ioctl |

### 用户层
| 文件 | 规划行数 | 核心知识点 |
|------|------|------|
| `app/smart_monitor.c` | 900 | epoll, input_event, 行缓冲, 查表法, 联动规则 |
| `libedge/` ★ | 1200 | ringbuf, linked_list, crc, filter, logger, ini_parser, msgqueue |

### 测试/文档
| 文件 | 说明 |
|------|------|
| `test/test_*.c` | 11 个功能测试 + 5 个单元测试 |
| `docs/*.md` | 10 份技术文档 + 9 份阶段报告 |

---

*文档版本：v2.0 | 日期：2026-06-10*
