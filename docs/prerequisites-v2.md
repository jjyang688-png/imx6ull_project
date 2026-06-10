# v2.0 前置知识清单

本文档列出开始 v2.0 工业边缘网关（阶段 10~14）开发前需要学习的前置知识，不包含阶段 1~9 已覆盖的内容。

> **已掌握（阶段 1~9）**：platform_driver / i2c_driver / spi_driver / cdev / gpiod / request_irq /
> wait_queue + poll_wait / mutex + spinlock / kfifo / ioctl + sysfs + debugfs /
> delayed_work + timer / fasync / epoll（用户空间）/ input 子系统 /
> SMBus + i2c_transfer + spi_sync / ioremap（UART 寄存器）

---

> ★★★ = 核心必学（多个驱动/模块依赖）　★★ = 重要（单个驱动依赖）　★ = 了解即可

---

## 一、内核驱动 — 新增机制

### 1.1 misc_register — 简化字符设备注册 ★★★

**重要度**：必须在阶段 11 前掌握（6 个驱动使用）

| 知识点 | 说明 |
|--------|------|
| 概念 | misc（miscellaneous）设备是主设备号为 10 的简化字符设备框架 |
| 核心结构体 | `struct miscdevice { int minor; const char *name; const struct file_operations *fops; }` |
| 注册/注销 | `misc_register()` / `misc_deregister()` — 一行替代 cdev_init+cdev_add+class_create+device_create |
| 自动 sysfs | 注册后自动生成 `/sys/class/misc/<name>/` 目录 |
| 与 cdev 对比 | cdev 需 ~30 行注册代码，misc 只需 ~10 行；但 misc 不区分 class，不适合多属性复杂设备 |
| 次设备号 | MISC_DYNAMIC_MINOR 自动分配 |

**参考**：`drivers/char/misc.c`（内核源码）

### 1.2 ktime 高精度时间 ★★★

**重要度**：必须在阶段 12 前掌握（dht11 单总线时序 + sr04 脉冲测量）

| 知识点 | 说明 |
|--------|------|
| `ktime_get_ns()` | 获取当前时间（纳秒精度），返回 `ktime_t` 或 `u64` |
| `ktime_get_real_ns()` | 获取墙上时间（可被系统时间调整影响） |
| `ktime_sub()` | 两个 ktime 相减 |
| `ktime_to_ns()` | ktime 转换为纳秒 |
| 时序测量模式 | `t_start = ktime_get_ns(); ...; delta = ktime_get_ns() - t_start;` |
| 精度注意 | DHT11 需要 μs 级精度（26μs vs 70μs 区分 bit 0/1），ARM Cortex-A7 @792MHz 足够 |

**参考**：`include/linux/ktime.h`

### 1.3 local_irq_save / local_irq_restore — 关本地中断 ★★

**重要度**：阶段 12（dht11 单总线时序保护）

| 知识点 | 说明 |
|--------|------|
| 目的 | DHT11 单总线时序是 μs 级，若在读取 40bit 期间被中断打断会导致计时错误 |
| `local_irq_save(flags)` | 保存当前中断状态到 flags，关闭本地 CPU 中断 |
| `local_irq_restore(flags)` | 恢复之前的中断状态 |
| 注意 | 只关本地 CPU 中断，不影响其他 CPU（本项目单核，无此问题） |
| 关闭时间 | 必须尽可能短（DHT11 约 5ms），长时间关中会导致系统响应问题 |

**参考**：`include/linux/irqflags.h`

### 1.4 completion — 一次性同步原语 ★★★

**重要度**：必须在阶段 12 前掌握（sr04 ECHO 回波等待）

| 知识点 | 说明 |
|--------|------|
| 概念 | 轻量级一次性同步：线程 A 等待事件发生，线程 B（通常是中断）触发事件 |
| `init_completion(&comp)` | 静态/动态初始化完成量 |
| `reinit_completion(&comp)` | 重置完成量（准备下一次使用） |
| `wait_for_completion(&comp)` | 阻塞等待，不可中断 |
| `wait_for_completion_interruptible(&comp)` | 可被信号中断的等待 |
| `wait_for_completion_interruptible_timeout(&comp, timeout)` | 带超时的等待（SR04 用 1s 超时） |
| `complete(&comp)` | 唤醒一个等待者（中断中调用安全） |
| `complete_all(&comp)` | 唤醒所有等待者 |
| 与 wait_event 区别 | completion 专为"等待某事件发生一次"设计，语义更清晰；wait_event 适合"条件满足"循环等待 |

**参考**：`include/linux/completion.h`、`kernel/sched/completion.c`

### 1.5 kthread — 内核线程 ★★

**重要度**：阶段 12（mq135_adc 周期采样）

| 知识点 | 说明 |
|--------|------|
| 概念 | 在内核中创建独立线程执行周期性任务 |
| `kthread_run(thread_fn, data, name_fmt, ...)` | 创建并唤醒线程 |
| `kthread_create(thread_fn, data, name_fmt, ...)` | 创建但不立即运行 |
| `kthread_stop(task)` | 停止线程（在线程内调用 `kthread_should_stop()` 配合） |
| `kthread_should_stop()` | 线程主循环中检查是否应退出 |
| 睡眠 | `ssleep(n)` / `msleep(n)`（进程上下文，可睡眠） |
| 与 workqueue 区别 | workqueue 适合"一次性任务"；kthread 适合"持续周期性采集" |

**参考**：`include/linux/kthread.h`

### 1.6 ioremap 扩展 — ADC / PWM / WDOG 寄存器 ★★

**重要度**：阶段 11~12

| 知识点 | 说明 |
|--------|------|
| 已有基础 | UART3 裸寄存器操作（ioremap + readb/writeb/readl/writel） |
| ADC1 基地址 | `0x02198000`，需了解 ADCx_CFG（配置）、ADCx_HS（采样）、ADCx_R0（结果） |
| PWM1 基地址 | `0x02080000`，PWM_SAR（周期）、PWM_PWMR（占空比）、PWM_CR（控制） |
| WDOG1 基地址 | `0x020BC000`，WDOG_WCR（控制）、WDOG_WSR（喂狗序列） |
| 通用方法 | 阶段 7 已掌握，只需查阅 i.MX6ULL 参考手册找到寄存器偏移 |

**参考**：i.MX6ULL Reference Manual — Chapter 14 (ADC), Chapter 39 (PWM), Chapter 59 (WDOG)

### 1.7 IRQF_TRIGGER_RISING / IRQF_TRIGGER_FALLING — 边沿触发 ★★

**重要度**：阶段 12（sr04 ECHO 双边沿中断）

| 知识点 | 说明 |
|--------|------|
| 已有基础 | 阶段 5 按键使用 `IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING` |
| SR04 用法 | ECHO 上升沿记录 `t_rise`，下降沿计算 `pulse_ns = t_fall - t_rise` |
| `IRQ_TYPE_EDGE_BOTH` | 设备树中设置双边沿触发 |
| ISR 中判断 | 用 `gpiod_get_value()` 区分上升沿（=1）还是下降沿（=0） |

---

## 二、通信协议与硬件

### 2.1 GPIO 单总线协议（DHT11） ★★★

**重要度**：必须在阶段 12 前理解（dht11 驱动）

| 知识点 | 说明 |
|--------|------|
| 物理层 | 单根数据线，主机和从机双向通信，引脚需上拉 4.7kΩ |
| 主机起始信号 | 拉低 ≥18ms → 拉高 20~40μs → 释放总线 |
| 从机响应 | DHT11 拉低 80μs → 拉高 80μs |
| bit 编码 | 每 bit 以 50μs 低电平开始，高电平长度区分：26~28μs = 0，70μs = 1 |
| 数据帧 | 40bit = 湿度整数(8) + 湿度小数(8) + 温度整数(8) + 温度小数(8) + 校验和(8) |
| 校验 | 前 4 字节之和的末 8 位等于校验字节 |
| 采样限制 | 两次采样之间需间隔 ≥2s |
| GPIO 方向切换 | 输出时 `gpiod_direction_output()` 拉低，输入时 `gpiod_direction_input()` 读 |
| 内核 API | `gpiod_set_value()` / `gpiod_get_value()` / `udelay()` / `usleep_range()` |

**参考**：DHT11 数据手册、`Documentation/driver-api/gpio/`（Linux 内核文档）

### 2.2 超声波测距原理（SR04） ★★

**重要度**：阶段 12

| 知识点 | 说明 |
|--------|------|
| 工作流程 | TRIG 发 10μs 脉冲 → 模块自动发 8 个 40kHz 脉冲 → ECHO 输出高电平 = 回波时间 |
| 距离公式 | `distance(cm) = pulse_width(μs) / 58`（声速 340m/s 换算） |
| 量程 | 2cm ~ 400cm |
| 声速温度修正 | 精确公式 `c = 331.5 + 0.607 × T`（本项目简化为常温，不修正） |
| GPIO 配置 | TRIG = 推挽输出，ECHO = 输入 + 双边沿中断 |

**参考**：HC-SR04 数据手册

### 2.3 SAR ADC 基础（MQ135） ★★

**重要度**：阶段 12

| 知识点 | 说明 |
|--------|------|
| SAR ADC 原理 | 逐次逼近型 ADC（Successive Approximation Register），12 位精度 |
| 参考电压 | i.MX6ULL ADC 参考电压 3.3V |
| 数值换算 | `V_in = ADC_value / 4095 × 3.3V` |
| ADC 通道 | 需要确定 MQ135 连接的模拟通道（ADC1_INx） |
| 软件触发 | 写 ADCx_HC0 寄存器的对应位启动一次转换 |
| 转换完成 | 轮询 ADCx_HS 的 COCO0 位（Conversion Complete） |
| MQ135 特性 | 需预热 >2 分钟，输出电压与有害气体（NH3/NOx/苯/CO2）浓度正相关 |
| 传感器校准 | 商用校准需专业设备，本项目使用 ADC 原始值 + 经验阈值分级 |

**参考**：i.MX6ULL RM Chapter 14、MQ135 数据手册

### 2.4 PWM 与舵机控制 ★★

**重要度**：阶段 11

| 知识点 | 说明 |
|--------|------|
| PWM 概念 | 脉冲宽度调制（Pulse Width Modulation） |
| 周期（Period） | 一个完整 PWM 波形的时间，舵机 = 20ms（50Hz） |
| 占空比（Duty Cycle） | 高电平时间占周期的百分比 |
| 舵机角度映射 | 0° = 0.5ms（2.5%），90° = 1.5ms（7.5%），180° = 2.5ms（12.5%） |
| 角度→脉宽公式 | `pulse_us = 500 + angle × (2500 - 500) / 180` |
| PWM 时钟链 | PER_CLK → 预分频器（PWM_PR）→ 计数器 → 比较器 → 输出 |
| 寄存器关键值 | SAR = 周期计数值，PWMR = 占空比计数值 |
| 常用舵机 | SG90（微型，0~180°）、MG996R（大力矩，0~180°） |

**参考**：i.MX6ULL RM Chapter 39、SG90/MG996R 数据手册

### 2.5 CAN 总线基础 ★★★

**重要度**：必须在阶段 12 前理解（can_drv 驱动）

| 知识点 | 说明 |
|--------|------|
| CAN 物理层 | 双绞线差分信号（CAN_H / CAN_L），需 120Ω 终端电阻 |
| CAN 2.0A vs 2.0B | 2.0A = 11-bit 标准 ID，2.0B = 29-bit 扩展 ID |
| 帧格式 | SOF + ID(11/29) + RTR + IDE + DLC(0~8) + Data(0~8 bytes) + CRC + ACK + EOF |
| 仲裁机制 | ID 越小优先级越高（显性 0 覆盖隐性 1），非破坏性仲裁 |
| 波特率 | 常用 125K / 250K / 500K / 1M bps |
| 错误处理 | 位错误/填充错误/CRC 错误/格式错误/ACK 错误，硬件自动重发 |
| SocketCAN | Linux 原生 CAN 子系统，将 CAN 设备抽象为网络接口（`can0`） |
| 核心结构体 | `struct can_frame { canid_t can_id; __u8 can_dlc; __u8 data[8]; }` |
| SocketCAN API | `socket(PF_CAN, SOCK_RAW, CAN_RAW)` → `bind()` → `read()`/`write()` |
| 硬件需求 | i.MX6ULL 内置 FlexCAN 控制器，需外接 TJA1050/SN65HVD230 收发器 |

**参考**：`Documentation/networking/can.rst`、SocketCAN 官方文档、CAN 2.0B 规范

### 2.6 看门狗定时器原理 ★★

**重要度**：阶段 11

| 知识点 | 说明 |
|--------|------|
| 原理 | 硬件计数器倒计时到 0 时触发系统复位 |
| 喂狗（Kick） | 应用程序在计数器到 0 前周期性重载计数器 |
| 喂狗序列 | WDOG 需要依次写特定值：先 `0x5555` 再 `0xAAAA` 到 WSR 寄存器 |
| 超时计算 | `timeout = (WCR[7:0] + 1) × 0.5s`，范围 0.5s ~ 128s |
| 使能后不可禁 | WDOG 一旦使能，软件无法关闭（安全设计） |
| 调试注意 | JTAG 调试时需先禁用 WDOG，否则断点期间会复位 |

**参考**：i.MX6ULL RM Chapter 59

---

## 三、libedge 基础库

### 3.1 C 宏泛型编程（ringbuf / linked_list） ★★★

**重要度**：阶段 10（libedge 核心模块）

| 知识点 | 说明 |
|--------|------|
| `#define` 带参数宏 | 实现类型无关的数据结构 |
| 宏中 `typeof()` | 推导参数类型 |
| `container_of` | 从成员指针推导结构体指针（Linux 内核惯用法） |
| 宏拼接 `##` | 生成类型特定的函数名 |
| 示例模式 | `#define RINGBUF_DEFINE(name, type, size)` 定义类型安全的环形缓冲 |

**参考**：Linux 内核 `include/linux/kfifo.h`、`include/linux/list.h`

### 3.2 环形缓冲算法 ★★

| 知识点 | 说明 |
|--------|------|
| 原理 | 固定大小数组 + 读写指针，O(1) 入队/出队 |
| 满/空判断 | 空：head == tail；满：(tail + 1) % size == head（浪费一个槽位） |
| 或满判断 | 使用 count 计数替代空槽法 |

### 3.3 双向链表 + 哨兵节点 ★★

| 知识点 | 说明 |
|--------|------|
| 哨兵节点（sentinel） | 空链表也有一个特殊节点，简化插入/删除逻辑（消除 NULL 检查） |
| prev/next 指针 | 双向遍历 |
| 归并排序 | 链表上 O(n log n) 排序，适合链表（无需随机访问） |
| Linux list.h 风格 | `struct list_head` 嵌入结构体而非独立节点 |

### 3.4 CRC 校验算法 ★★

| 知识点 | 说明 |
|--------|------|
| CRC 原理 | 多项式除法取余，发送方附校验值，接收方验证 |
| 查表法 | 预计算 256 字节查找表，O(n) 计算（比逐 bit O(8n) 快 8 倍） |
| CRC8 | 多项式 `0x07`（Dallas/Maxim 1-Wire 风格） |
| CRC16-Modbus | 多项式 `0x8005`，初始值 `0xFFFF`，输入/输出不反转 |
| 标准测试向量 | `{0x01,0x03,0x00,0x00,0x00,0x01}` → CRC16 = `0x840A` |
| CRC32 | 多项式 `0xEDB88320`（Ethernet / gzip 标准），查表 256 个 `uint32_t` |

**参考**：Modbus 协议规范、`lib/crc16.c`（Linux 内核源码）

### 3.5 滑动平均与中值滤波 ★★

| 知识点 | 说明 |
|--------|------|
| 滑动平均 | 窗口大小 N，每次新值替换最旧值 |
| O(1) 优化 | 维护 `sum` — 每次更新 `sum = sum - oldest + newest`，均值 = sum/N |
| 中值滤波 | 取窗口内 N 个值排序后取中值，去噪效果好 |
| 中值排序 | 滑动窗口前向 O(N) 插入排序（窗口小，性能可接受） |

### 3.6 POSIX 消息队列 ★★

| 知识点 | 说明 |
|--------|------|
| 头文件 | `<mqueue.h>`，链接时加 `-lrt` |
| `mq_open(name, O_CREAT | O_RDWR, mode, &attr)` | 创建/打开队列 |
| `mq_send(mqd, buf, len, prio)` | 发送消息 |
| `mq_receive(mqd, buf, len, &prio)` | 接收消息（可阻塞/非阻塞） |
| `mq_close(mqd)` / `mq_unlink(name)` | 关闭/删除队列 |
| 消息优先级 | 优先级高的消息先出队（可选特性） |

### 3.7 可变参数函数（logger 模块） ★

| 知识点 | 说明 |
|--------|------|
| `#include <stdarg.h>` | 可变参数宏定义 |
| `va_list args` | 声明参数列表变量 |
| `va_start(args, fmt)` | 初始化，fmt 是最后一个固定参数 |
| `vsnprintf(buf, size, fmt, args)` | 格式化到缓冲区 |
| `va_end(args)` | 清理 |

### 3.8 状态机文本解析（ini_parser） ★

| 知识点 | 说明 |
|--------|------|
| 状态枚举 | `SECTION` / `KEY` / `VALUE` / `COMMENT` / `BLANK` |
| 逐行读取 | `fgets()` 逐行读，根据上下文状态分发处理 |
| 处理注释 | `#` 或 `;` 开头为注释行 |
| 处理 section | `[name]` 进入新节 |
| 键值对 | `key = value` 去除前后空格，存入配置结构体 |

### 3.9 错误码模式 ★

| 知识点 | 说明 |
|--------|------|
| 枚举定义 | `typedef enum { EDGE_OK=0, EDGE_ERR_NOMEM=-1, ... } edge_err_t;` |
| 字符串映射 | `const char *edge_strerror(edge_err_t err);` — 查表法返回描述 |

### 3.10 Unity 测试框架 ★

| 知识点 | 说明 |
|--------|------|
| 特点 | 极简 C 单元测试框架（单头文件），适合嵌入式 |
| 核心宏 | `TEST_ASSERT_EQUAL(expected, actual)` / `TEST_ASSERT_TRUE(cond)` |
| 测试注册 | `RUN_TEST(test_func)` |
| 使用方式 | 链接 `unity.c`，写测试文件调用 Unity API |

**参考**：https://github.com/ThrowTheSwitch/Unity

---

## 四、用户空间扩展

### 4.1 smart_monitor 架构扩展 ★

| 知识点 | 说明 |
|--------|------|
| 已有基础 | epoll 多路复用 + 查表法命令引擎 + 信号优雅退出 |
| MAX_DEVICES | 从 6 扩展到 13（新增 7 个设备 fd） |
| 容错启动 | 非关键设备打开失败不退出（commented out fd） |
| 联动规则 | 全局 `g_sensors` 状态 + 连续 N 次条件触发（去抖） |

### 4.2 静态库链接 ★

| 知识点 | 说明 |
|--------|------|
| 编译 | `gcc -c *.c → *.o` → `ar rcs libedge.a *.o` |
| 链接 | `gcc ... -L../libedge -ledge` |
| Makefile 依赖 | libedge 先编译，app 后编译 |

---

## 五、学习路线建议

按阶段开发顺序排列学习优先级：

```
阶段 10（libedge）
├── 3.1 宏泛型编程 ← 最先学
├── 3.2 环形缓冲算法
├── 3.3 链表 + 哨兵
├── 3.4 CRC 查表法
├── 3.5 滑动滤波
├── 3.7 可变参数
├── 3.8 状态机解析
├── 3.6 POSIX mq
├── 3.9 错误码
└── 3.10 Unity 测试

阶段 11（relay / wdt / servo）
├── 1.1 misc_register ← 必须先学
├── 2.4 PWM 与舵机
├── 2.6 看门狗原理
└── 1.6 ioremap 扩展（PWM/WDOG）

阶段 12（dht11 / sr04 / mq135 / can）
├── 1.2 ktime 高精度时间
├── 1.3 local_irq_save/restore
├── 1.4 completion
├── 1.5 kthread
├── 2.1 GPIO 单总线协议
├── 2.2 超声波原理
├── 2.3 SAR ADC 基础
├── 2.5 CAN 总线 + SocketCAN
└── 1.7 边沿触发

阶段 13（smart_monitor 扩展 + 联动）
├── 4.1 架构扩展
└── 4.2 静态库链接
```

---

## 六、关键参考资源

| 资源 | 用途 |
|------|------|
| i.MX6ULL Reference Manual | 寄存器定义（ADC/PWM/WDOG/FlexCAN） |
| DHT11 数据手册 | 单总线时序 |
| HC-SR04 数据手册 | 超声波参数 |
| MQ135 数据手册 | 传感器特性 |
| SG90/MG996R 数据手册 | 舵机参数 |
| CAN 2.0B 规范 | CAN 协议细节 |
| Linux `Documentation/networking/can.rst` | SocketCAN 官方文档 |
| Linux `drivers/char/misc.c` | misc 驱动参考实现 |
| Linux `include/linux/ktime.h` | ktime API |
| Linux `include/linux/completion.h` | completion API |
| Linux `include/linux/kthread.h` | kthread API |
| Linux `lib/crc16.c` | CRC16 内核参考实现 |
| Unity 测试框架 | https://github.com/ThrowTheSwitch/Unity |
| Modbus 协议规范 | CRC16-Modbus 算法验证 |

---

*文档版本：v2.0 | 日期：2026-06-10*
