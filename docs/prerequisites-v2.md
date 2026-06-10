# v2.0 系统学习指南

本文档为 v2.0 工业边缘网关（阶段 10~14）提供**系统性学习方案**：知识体系 → 依赖关系 → 动手练习 → 时间规划。

> **已掌握（阶段 1~9 产出）**：platform_driver / i2c_driver / spi_driver / cdev / gpiod / request_irq /
> wait_queue + poll_wait / mutex + spinlock / kfifo / ioctl + sysfs + debugfs /
> delayed_work + timer / fasync / epoll（用户空间）/ input 子系统 /
> SMBus + i2c_transfer + spi_sync / ioremap（UART 寄存器）

---

## 一、学习深度分级

每个知识点按掌握深度分为三级：

| 级别 | 名称 | 标准 | 考核方式 |
|------|------|------|---------|
| **L1** | 理解原理 | 能解释概念、画出流程图、说出适用场景和限制 | 口头/文档描述 |
| **L2** | 能写代码 | 不看参考独立写出正确实现，通过单元测试 | 代码 + 测试通过 |
| **L3** | 能调试排错 | 能定位常见错误（时序异常/并发竞争/内存泄漏），能用 printk/ftrace/gdb 排障 | 故障注入场景 |

### 各知识点深度要求

| 编号 | 知识点 | 最低深度 | 说明 |
|------|--------|---------|------|
| 1.1 | misc_register | **L2** | 6 个驱动依赖，必须能独立写 |
| 1.2 | ktime 高精度时间 | **L2** | dht11 + sr04 双驱动依赖 |
| 1.3 | local_irq_save/restore | **L1** | 理解原理即可，dht11 中直接套用模式 |
| 1.4 | completion | **L2** | sr04 核心同步机制 |
| 1.5 | kthread | **L2** | mq135_adc 采集线程 |
| 1.6 | ioremap 扩展 | **L2** | 3 个外设（ADC/PWM/WDOG）各自独立 |
| 1.7 | 边沿触发中断 | **L1** | 阶段 5 已有基础，SR04 仅扩展用法 |
| 2.1 | DHT11 单总线协议 | **L2** | 核心难点：时序控制 |
| 2.2 | SR04 超声波原理 | **L1** | 原理简单，实践中学习 |
| 2.3 | SAR ADC 基础 | **L1** | 理解 ADC 工作流程即可 |
| 2.4 | PWM 与舵机控制 | **L2** | 需计算占空比，理解时钟链 |
| 2.5 | CAN 总线 + SocketCAN | **L2** | 协议 + API 双重学习 |
| 2.6 | 看门狗原理 | **L1** | 原理简单，注意调试陷阱 |
| 3.1 | C 宏泛型编程 | **L2** | ringbuf 核心实现手段 |
| 3.2 | 环形缓冲算法 | **L2** | 需处理满/空边界条件 |
| 3.3 | 双向链表+哨兵 | **L2** | 需实现归并排序 |
| 3.4 | CRC 校验算法 | **L2** | 需通过标准测试向量验证 |
| 3.5 | 滑动平均与中值滤波 | **L2** | 需验证 O(1) 优化正确性 |
| 3.6 | POSIX 消息队列 | **L1** | 封装后使用，不必深究内核实现 |
| 3.7 | 可变参数函数 | **L1** | logger 格式化接口 |
| 3.8 | 状态机解析 | **L1** | INI 语法简单，模式固定 |
| 3.9 | 错误码模式 | **L1** | 设计模式级别，无算法难度 |
| 3.10 | Unity 测试框架 | **L2** | 所有模块测试依赖 |
| 4.1 | smart_monitor 扩展 | **L2** | 修改现有代码，需理解原架构 |
| 4.2 | 静态库链接 | **L1** | Makefile 技能 |

---

## 二、知识依赖关系图

```
                    ┌─────────────────────────────────────────────┐
                    │          v2.0 全知识依赖关系图               │
                    └─────────────────────────────────────────────┘

阶段 10 ─── libedge 基础库 ───
                                    ┌──────────────────────┐
                                    │  C 语言基础 (已有)     │
                                    └──────┬───────────────┘
                           ┌───────────────┼───────────────┐
                           ▼               ▼               ▼
                    3.7 可变参数    3.1 宏泛型编程    3.8 状态机解析
                    (va_list)      (typeof/##/        (逐行FSM)
                                    container_of)
                           │          │    │              │
                           ▼          ▼    ▼              ▼
                    3.5 滤波算法  3.2 环形缓冲  3.3 链表+哨兵
                    (O(1)滑动平均) (head/tail)  (归并排序)
                           │          │    │              │
                           └──────────┼────┘              │
                                     ▼                    │
                               3.4 CRC 查表法              │
                               (多项式除法→预计算)          │
                                     │                    │
                                     ▼                    ▼
                               3.10 Unity 测试 ←── 3.9 错误码 + 3.6 POSIX mq
                                     │
                                     ▼
                               阶段 10 完成 ──────────────┘

阶段 11 ─── 简单驱动 ───
                                    1.1 misc_register (必须先学)
                                         │
                    ┌────────────────────┼────────────────────┐
                    ▼                    ▼                    ▼
               2.4 PWM+舵机          2.6 看门狗            GPIO 输出 (已有)
              (时钟链+SAR计算)       (喂狗序列)            (gpiod API)
                    │                    │                    │
                    ▼                    ▼                    ▼
              1.6 ioremap          1.6 ioremap           relay.c
              (PWM寄存器)          (WDOG寄存器)          (最简驱动)
                    │                    │
                    ▼                    ▼
              servo_pwm.c            wdt.c
                                         │
                                         ▼
                                   阶段 11 完成 ──────────────┘

阶段 12 ─── 中等驱动 ───
                                    1.1 misc_register (已有)
                                         │
          ┌──────────────┬───────────────┼───────────────┬──────────────┐
          ▼              ▼               ▼               ▼              ▼
    2.1 DHT11协议   2.2 SR04原理   2.3 ADC基础    2.5 CAN协议     1.7 边沿触发
   (单总线时序)    (脉冲测量)     (SAR 12-bit)   (帧格式/仲裁)   (IRQ双边沿)
          │              │               │               │              │
          ▼              ▼               ▼               ▼              │
    1.2 ktime      1.4 completion   1.5 kthread     SocketCAN API ─────┘
   (ns级计时)     (同步原语)      (周期线程)      (PF_CAN)
          │              │               │               │
          ▼              ▼               ▼               ▼
    1.3 local_irq   1.2 ktime      1.6 ioremap      can_drv.c
   (关中断保护)    (脉宽测量)      (ADC寄存器)
          │              │               │
          ▼              ▼               ▼
      dht11.c         sr04.c        mq135_adc.c
          │              │               │
          └──────────────┼───────────────┘
                         ▼
                   阶段 12 完成 ──────────────────────────────────┘

阶段 13 ─── smart_monitor 扩展 ───
                                    12 个驱动全部完成
                                         │
                              ┌──────────┼──────────┐
                              ▼          ▼          ▼
                        4.1 架构扩展  4.2 静态库链接  联动规则设计
                       (6fd→13fd)   (libedge.a)    (连续N次去抖)
                              │          │          │
                              └──────────┼──────────┘
                                         ▼
                                   阶段 13 完成 ──────────────┘
```

---

## 三、分阶段学习计划

---

### 阶段 10：libedge 基础库（预估 5~7 天）

> **目标**：独立完成 8 个库模块 + 5 个单元测试，`libedge.a` 通过全部测试。
> **前置依赖**：仅需 C 语言基础（函数指针、结构体、指针运算），阶段 1~9 已覆盖。

#### 第 1 步：可变参数函数（0.5 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 3.7 | **L1** | logger 模块的格式化接口 |

**学什么：** `va_list` / `va_start` / `va_arg` / `va_end` / `vsnprintf`

**动手练习：**
```c
// 任务：实现一个 my_printf(const char *fmt, ...)
// 要求：支持 %s %d %x %c 四种格式符
// 验证：my_printf("Hello %s, value=%d (0x%x)\n", "world", 255, 255);
// 预期输出：Hello world, value=255 (0xff)
```

**自检：** 能解释为什么需要 `va_end`？`va_arg` 展开后是什么？

#### 第 2 步：C 宏泛型编程（1 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 3.1 | **L2** | ringbuf + linked_list 的实现基础 |

**学什么：**
- `#define` 多行宏（`\` 续行）
- `typeof()` 类型推导
- `container_of(ptr, type, member)` — Linux 内核核心宏
- 宏参数拼接 `##`（生成函数名）

**动手练习：**
```c
// 练习 1：实现 container_of 宏
#define container_of(ptr, type, member)  /* 你来填 */

// 验证
struct foo { int a; char b; };
struct foo f;
assert(container_of(&f.b, struct foo, b) == &f);

// 练习 2：实现类型安全的 SWAP 宏
#define SWAP(a, b)  /* 你来填 — 需处理类型，不引入临时变量或使用 typeof */
```

**自检：** `container_of` 为什么能工作？它在编译期还是运行期生效？

#### 第 3 步：环形缓冲算法（1 天）

| 知识点 | 深度 | 说明 |
|------|------|------|
| 3.2 | **L2** | 泛型环形缓冲的核心算法 |

**学什么：**
- 读写指针推进：`head = (head + 1) % size`，`tail = (tail + 1) % size`
- 空/满判断两种方案：(A) 浪费一个槽位 (B) 维护 count 计数
- 宏生成类型安全的 `ringbuf_push_##type()` / `ringbuf_pop_##type()`

**动手练习：**
```c
// 任务：实现 ringbuf.h，提供以下宏
RINGBUF_DEFINE(int_rb, int, 16);           // 定义 int 型环形缓冲
int_rb_push(&rb, 42);                       // 入队
int val = int_rb_pop(&rb);                  // 出队
bool empty = int_rb_empty(&rb);

// 测试边界条件（必须全部通过）：
// 1. 空缓冲 pop 返回什么？
// 2. 满缓冲 push 返回什么？
// 3. 循环写入 2*size 次后，最早的数据是否被覆盖？
```

**自检：** 为什么环形缓冲的 size 通常取 2 的幂？与取模运算的 `%` 相比有什么优化？

#### 第 4 步：双向链表 + 哨兵节点 + 归并排序（1 天）

| 知识点 | 深度 | 说明 |
|------|------|------|
| 3.3 | **L2** | 哨兵模式 + 链表排序 |

**学什么：**
- 哨兵节点：空链表时 `head->next == head`，`head->prev == head`
- 插入：`new->next = pos; new->prev = pos->prev; pos->prev->next = new; pos->prev = new;`
- 删除：`node->prev->next = node->next; node->next->prev = node->prev;`
- 归并排序：快慢指针找中点 → 递归分解 → 合并有序子链表

**动手练习：**
```c
// 任务 1：实现带哨兵的双向链表（插入/删除/查找/遍历）
// 任务 2：实现 list_merge_sort()，对链表按升序排序
// 验证：随机 100 个整数 → 排序 → 遍历验证升序
// 边界：空链表、单元素、已排序、逆序
```

**自检：** 哨兵相比 NULL-terminated 链表好在哪？归并排序在链表上为什么比快排更适合？

#### 第 5 步：CRC 校验算法（1 天）

| 知识点 | 深度 | 说明 |
|------|------|------|
| 3.4 | **L2** | 查表法实现 CRC8/CRC16/CRC32 |

**学什么：**
- 模 2 除法原理（手工计算一个字节的 CRC8 来理解）
- 查表法优化：预计算 256 个值，每个字节一步查表
- 三种 CRC 的参数差异（多项式 / 初始值 / 输入反转 / 输出反转 / 异或值）

**动手练习：**
```c
// 任务 1：手工计算 {0x31, 0x32, 0x33} 的 CRC8（多项式 0x07）
// 任务 2：实现 crc8() / crc16_modbus() / crc32()
// 任务 3：用标准测试向量验证
assert(crc16_modbus(test_data, 6) == 0x840A);  // Modbus 标准向量
assert(crc32("123456789", 9) == 0xCBF43926);   // CRC32 标准向量
```

**自检：** 查表法比逐 bit 计算快多少（量化）？为什么 Modbus CRC16 的测试向量是 `0x840A` 而不是 `0x0A84`？

#### 第 6 步：滑动平均 + 中值滤波（0.5 天）

| 知识点 | 深度 | 说明 |
|------|------|------|
| 3.5 | **L2** | O(1) 滑动平均 + 中值滤波 |

**学什么：**
- 滑动平均 O(N) → O(1) 优化：维护 `sum`，每次 `sum = sum - oldest + newest`
- 中值滤波：窗口内插入排序，取中值

**动手练习：**
```c
// 任务：实现 moving_avg_t 和 median_filter_t
// 验证 1（滑动平均）：
//   输入 {10,20,30,40,50}，窗口 3 → 输出 {10,15,20,30,40}
//   手动验证 sum 是否维护正确
// 验证 2（中值滤波）：
//   输入 {1,5,3,9,2}，窗口 3 → 输出 {1,3,3,5,3}
```

**自检：** 什么时候用滑动平均，什么时候用中值滤波？MQ135 应该用哪个？

#### 第 7 步：剩余模块（1 天）

| 知识点 | 深度 | 模块 |
|--------|------|------|
| 3.8 | L1 | ini_parser — 逐行状态机解析 key=value |
| 3.6 | L1 | msgqueue — POSIX mq 封装（阻塞/非阻塞收发） |
| 3.9 | L1 | edge_error — 枚举 + 查表字符串映射 |
| 3.10 | L2 | Unity — 测试框架集成 |

**动手练习：**
```c
// ini_parser：解析以下内容
const char *ini = "[network]\nip=192.168.1.1\nport=8080\n# comment\n[device]\nname=gateway\n";
// 要求正确解析 2 个 section、3 个 key-value，忽略注释

// msgqueue：创建队列 → 发送 "hello" → 接收 → 验证内容一致

// Unity：为 ringbuf 写 5 个测试用例
//   test_empty_buffer / test_push_pop / test_full_buffer / test_wraparound / test_overwrite
```

#### 阶段 10 自检清单

| # | 检查项 | 通过标准 |
|---|--------|---------|
| 1 | 所有 8 个模块编译为 `libedge.a` | `ar t libedge.a` 列出全部 .o |
| 2 | 5 个单元测试全部 PASS | Unity 输出 0 failures |
| 3 | CRC16 标准向量通过 | `0x01,0x03,0x00,0x00,0x00,0x01` → `0x840A` |
| 4 | 环形缓冲边界测试通过 | 空 pop / 满 push / 循环覆盖 |
| 5 | 链表排序验证通过 | 随机 100 元素排序后升序 |
| 6 | 编译零警告 | `-Wall -Wextra` 无输出 |

---

### 阶段 11：简单驱动（预估 4~5 天）

> **目标**：完成 relay / wdt / servo 三个 misc 驱动 + 功能测试。
> **前置依赖**：阶段 10 完成 + misc_register 已学。

#### 第 1 步：misc_register 上手（1 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 1.1 | **L2** | 6 个驱动依赖，必须先掌握 |

**学什么（对比已有 cdev 经验）：**

| | cdev（阶段 3） | misc（本阶段） |
|------|-------------|-------------|
| 注册 | `alloc_chrdev_region` + `cdev_init` + `cdev_add` + `class_create` + `device_create` | `misc_register(&miscdev)` |
| 注销 | 5 步反向 | `misc_deregister(&miscdev)` |
| sysfs | 手动 `device_create_file` | 自动 `/sys/class/misc/<name>/` |
| 代码量 | ~30 行 | ~10 行 |

**动手练习：**
```c
// 任务：写一个"echo 驱动" — 写什么就读什么（练习 misc 框架）
// 1. 定义 file_operations（open/release/read/write）
// 2. misc_register 注册
// 3. insmod 后验证：
//    echo "hello" > /dev/echo_drv
//    cat /sys/class/misc/echo_drv/value  → "hello"
// 4. rmmod 注销验证无内存泄漏
```

**自检：** misc 设备的主设备号是多少？

#### 第 2 步：relay 驱动（0.5 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| misc | L2 | 最简单的 misc 驱动，练手首选 |
| gpiod | — | 已有基础 |

**动手练习：**
```c
// 任务：实现 relay.c
// write("on", 2)   → gpiod_set_value(relay_gpio, 1)
// write("off", 3)  → gpiod_set_value(relay_gpio, 0)
// read(buf, len)   → 返回当前状态字符串
// sysfs: /sys/class/misc/relay/state → "on"/"off"

// 测试：写脚本 100 次 on/off 切换，示波器验证 GPIO 输出
```

#### 第 3 步：看门狗驱动（1 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.6 | **L1** | 理解 WDOG 硬件原理 |
| 1.6 | **L2** | ioremap WDOG1 寄存器 |

**学什么：**
- WDOG 寄存器：WCR（控制，含超时值）、WSR（喂狗序列）、WRSR（复位状态）
- 喂狗序列：必须**先写 0x5555，再写 0xAAAA**，两次写间隔不超过 16 个时钟周期
- 使能后不可禁（by design）：调试时用 JTAG 暂停 WDOG

**动手练习：**
```c
// 任务：实现 wdt.c
// - ioctl WDT_SET_TIMEOUT：计算 WCR 值并写入
// - ioctl WDT_GET_TIMELEFT：读取 WCR 计算剩余时间
// - write("feed", 4)：依次写 WSR 0x5555 → 0xAAAA

// 危险实验（可选，理解 WDOG 行为）：
// 设置 2s 超时 → 使能 WDOG → 不喂狗 → 2s 后观察系统复位
// （确保保存所有工作！）
```

**自检：** 为什么喂狗需要特定的序列值（0x5555/0xAAAA）而不是随意写一个值？

#### 第 4 步：舵机 PWM 驱动（1.5 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.4 | **L2** | PWM 时钟链 + 占空比计算 |

**学什么：**
- PWM 时钟链：`PER_CLK(80MHz) → 预分频器(PR) → 计数器 → 比较器`
- SAR（周期寄存器）：`SAR = PER_CLK / (PR+1) / freq - 1`
- PWMR（占空比寄存器）：`PWMR = pulse_us × (PER_CLK / (PR+1)) / 1_000_000`
- 角度→占空比映射：`pulse_us = 500 + angle × 2000 / 180`

**动手练习：**
```c
// 任务 1：手工计算（写在纸上）
// PER_CLK=80MHz, PR=15 → 计数器频率 = ?
// 目标 50Hz → SAR = ?
// 目标 0° (0.5ms) → PWMR = ?
// 目标 180° (2.5ms) → PWMR = ?

// 任务 2：实现 servo_pwm.c
// write("90", 2) → 计算并设置 90° 对应的 PWMR
// read → 返回当前角度字符串

// 任务 3：功能测试 — 循环 0°→90°→180°→90°→0°
// 示波器观测 PWM 输出验证占空比变化
```

**自检：** 如果 PER_CLK 改为 66.5MHz，如何重新计算？PWM 使能前为什么要先设 SAR？

#### 阶段 11 自检清单

| # | 检查项 | 通过标准 |
|---|--------|---------|
| 1 | relay 100 次 on/off 无异常 | GPIO 输出正确 |
| 2 | wdt 喂狗周期内系统不复位 | 定时器正常运行 |
| 3 | wdt 不喂狗超时后系统复位 | 验证看门狗生效 |
| 4 | servo 0°/90°/180° PWM 输出正确 | 示波器测量脉宽在 ±5% 内 |
| 5 | 三个模块 `lsmod` 可见，`rmmod` 无泄漏 | 无内核报错 |

---

### 阶段 12：中等驱动（预估 7~9 天）

> **目标**：完成 dht11 / sr04 / mq135 / can 四个驱动 + 功能测试。
> **前置依赖**：阶段 11 完成 + ktime / completion / kthread / 边沿触发已学。

#### 第 1 步：DHT11 单总线驱动（2.5 天）— 最难驱动

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.1 | **L2** | 单总线协议 |
| 1.2 | **L2** | ktime 纳秒计时 |
| 1.3 | **L1** | 关中保护时序 |

**学什么 — 协议时序（必须能默画时序图）：**

```
主机发起                DHT11响应         bit传输 (×40)
───────                ────────         ────────────
     ┌──┐                 ┌──┐    ┌─┐          ┌────┐
     │  │ 40μs            │  │80μs│ │ 50μs     │    │
─────┘  └─────       ─────┘  └──┘ └─┘          └────┘
  ←18ms→                        ←──80μs──→   ← 26μs(0) / 70μs(1) →
  拉低    释放                   拉低  拉高    拉低    拉高(读此段)
```

**动手练习：**
```c
// 任务 1：在纸上默画完整时序图（从主机起始到 40bit 结束）

// 任务 2：实现 dht11.c read() 函数
// 关键流程：
//   1. mutex_lock
//   2. local_irq_save          ← 关中保护
//   3. gpiod_direction_output → 拉低 18ms (usleep_range)
//   4. gpiod_direction_output → 拉高 40μs (udelay)
//   5. gpiod_direction_input  → 释放总线
//   6. 等待 DHT11 响应 (80μs低 + 80μs高, 超时=200μs)
//   7. for i in 0..39:         ← 读 40 bit
//        while gpiod_get == 0;  ← 跳过 50μs 低
//        t = ktime_get_ns();
//        while gpiod_get == 1;  ← 测高电平
//        if (ktime_get_ns() - t > 50μs) → bit=1 else bit=0
//   8. local_irq_restore       ← 恢复中断
//   9. 校验 → copy_to_user

// 任务 3：调试技巧 — 在 ISR 中加打印
//   pr_info("bit[%d] = %d (high time: %lld ns)\n", i, bit, delta);

// 任务 4：连续读 5 次，验证每 2s 能读到有效数据
```

**常见错误与排查：**
| 症状 | 可能原因 | 排查方法 |
|------|---------|---------|
| 全 0 | DHT11 未响应 | 示波器看 DATA 线，检查上拉电阻 |
| 校验失败 | 某 bit 计时不准 | printk 每个 bit 的高电平时间 |
| 时好时坏 | 中断打断时序 | 检查 local_irq_save 位置 |

#### 第 2 步：SR04 超声波驱动（2 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.2 | **L1** | 超声波原理 |
| 1.4 | **L2** | completion 同步 |
| 1.2 | **L2** | ktime 脉冲测量 |
| 1.7 | **L1** | 双边沿中断 |

**学什么 — completion 流程：**

```
用户调用 read()
  │
  ▼
sr04_read():
  init_completion(&echo_done)        ← 初始化
  gpiod_set_value(trig, 1)           ← TRIG 发 10μs
  udelay(10)
  gpiod_set_value(trig, 0)
  wait_for_completion_interruptible_timeout(&echo_done, 1*HZ)  ← 阻塞等中断
  │                                     │
  │                    ┌─────────────────┘
  │                    ▼
  │             ECHO 上升沿 ISR:
  │               t_rise = ktime_get_ns()
  │                    │
  │                    ▼
  │             ECHO 下降沿 ISR:
  │               t_fall = ktime_get_ns()
  │               pulse_ns = t_fall - t_rise
  │               complete(&echo_done)      ← 唤醒 read()
  │                    │
  └────────────────────┘
  ▼
distance_cm = pulse_ns / 58000
copy_to_user()
```

**动手练习：**
```c
// 任务 1：画出 completion 的状态转移图
// 任务 2：实现 sr04.c
// 任务 3：手动测试 — 在不同距离（10cm/50cm/100cm/200cm）各测 5 次
//   记录测量精度（与卷尺对比），计算平均误差

// 任务 4（排障练习）：模拟以下故障场景
//   场景 A：SR04 未连接 → 1s 超时返回 -ETIMEDOUT
//   场景 B：物体超出量程(>4m) → 超时返回 -ETIMEDOUT
//   验证你的驱动是否正确处理
```

#### 第 3 步：MQ135 ADC 驱动（1.5 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.3 | **L1** | SAR ADC 原理 |
| 1.5 | **L2** | kthread 周期采样 |
| 1.6 | **L2** | ioremap ADC1 寄存器 |

**学什么 — kthread 模式：**

```c
static int mq135_thread(void *data) {
    struct mq135_dev *dev = data;
    while (!kthread_should_stop()) {
        // 软件触发转换
        writel(1 << dev->channel, ADC1_HC0);
        // 等待转换完成（轮询或中断）
        while (!(readl(ADC1_HS) & COCO0))
            cpu_relax();
        // 读取结果
        dev->adc_value = readl(ADC1_R0) & 0xFFF;
        // 应用滑动平均 (from libedge)
        dev->filtered = moving_avg_update(&dev->filter, dev->adc_value);
        // 计算空气质量等级
        dev->quality = classify_quality(dev->filtered);
        ssleep(2);  // 2s 采样间隔
    }
    return 0;
}
```

**动手练习：**
```c
// 任务 1：实现 mq135_adc.c
//   - probe 中 ioremap ADC1，kthread_run
//   - remove 中 kthread_stop，iounmap
//   - read 返回最新 ADC 值 + 质量等级
//   - sysfs：raw / quality 属性

// 任务 2：验证滑动平均效果
//   记录 100 个原始 ADC 值，离线对比原始值 vs 滑动平均后的曲线
//   （Excel 画折线图即可）
```

#### 第 4 步：CAN 总线驱动（2 天）

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 2.5 | **L2** | CAN 协议 + SocketCAN |

**学什么 — SocketCAN 架构：**

```
用户空间                      内核空间
───────                      ────────
smart_monitor                can_drv.c (本项目)
    │                            │
    │ open("/dev/can_ctrl")      │
    │ read/write/ioctl           │
    │                            │ socket(PF_CAN, SOCK_RAW, CAN_RAW)
    │                            │ bind(ifr.can_ifindex = "can0")
    │                            │ setsockopt(CAN_RAW_FILTER)
    │                            │ read/write(can_fd, &frame, sizeof(frame))
    ▼                            ▼
           ┌──────────────────────────┐
           │     SocketCAN 子系统      │
           │  (内核原生 CAN 协议栈)     │
           └──────────┬───────────────┘
                      │
           ┌──────────▼───────────────┐
           │     FlexCAN 控制器驱动    │
           │     (内核自带)            │
           └──────────┬───────────────┘
                      │
           ┌──────────▼───────────────┐
           │  i.MX6ULL CAN1 硬件      │
           │  + TJA1050 收发器        │
           └──────────────────────────┘
```

**动手练习：**
```c
// 任务 1：CAN 协议基础题
//   - 画一个标准 CAN 数据帧的结构图（标注每个字段位数）
//   - 为什么 CAN 总线上必须有 120Ω 终端电阻？（提示：信号反射）
//   - ID=0x001 和 ID=0x7FF 同时发送，谁赢？为什么？

// 任务 2：实现 can_drv.c
//   - open: socket()+bind() 到 can0
//   - read: 从 socket 读 can_frame → copy_to_user
//   - write: copy_from_user → send to socket
//   - ioctl: CAN_SET_BITRATE, CAN_SET_FILTER

// 任务 3：功能测试（需要两块板子或 CAN 分析仪）
//   板子 A: echo "0x123#AB CD" → 发送 CAN 帧
//   板子 B: ./smart_monitor → 接收到 ID=0x123, data=[0xAB, 0xCD]
```

#### 阶段 12 自检清单

| # | 检查项 | 通过标准 |
|---|--------|---------|
| 1 | DHT11 连续 10 次读数校验通过 | 无校验失败 |
| 2 | DHT11 读取期间系统其他任务不卡死 | 关中 < 10ms |
| 3 | SR04 10cm 处测量误差 < 1cm | \|测量值-真实值\| < 1cm |
| 4 | SR04 无回波 1s 超时返回 -ETIMEDOUT | 不永久阻塞 |
| 5 | MQ135 kthread 正确启停 | insmod→启动 rmmod→停止 |
| 6 | CAN 收发数据一致 | 发送=接收 |

---

### 阶段 13：smart_monitor 扩展 + 联动（预估 2~3 天）

> **目标**：集成全部 12 设备 + 6 条联动规则 + 链接 libedge。
> **前置依赖**：所有驱动完成 + libedge 完成。

| 知识点 | 深度 | 说明 |
|--------|------|------|
| 4.1 | **L2** | 架构扩展 |
| 4.2 | **L1** | 静态库链接 |

**动手练习：**
```c
// 任务 1：扩展 MAX_DEVICES 6→13
//   新增 fd 数组：dht11 / sr04 / mq135 / servo / relay / can / wdt
//   关键设备失败退出，非关键设备失败跳过

// 任务 2：实现联动规则引擎
// 规则 R-01 示例：
static int high_temp_count = 0;
if (g_sensors.temperature > 35.0) {
    if (++high_temp_count >= 3) {
        write(relay_fd, "on", 2);
        ioctl(led_fd, COMP_DRV_START_BLINK);
        log_warn("Rule R-01: High temperature, relay ON");
        high_temp_count = 0;  // 防止重复触发
    }
} else if (g_sensors.temperature < 30.0) {
    high_temp_count = 0;
}

// 任务 3：集成测试
//   用吹风机加热 DHT11 → 观察继电器自动吸合
//   用手靠近 SR04 → 观察蜂鸣器告警
```

#### 阶段 13 自检清单

| # | 检查项 | 通过标准 |
|---|--------|---------|
| 1 | 13 个 fd 全部注册到 epoll | 无崩溃 |
| 2 | 缺少非关键设备不阻止启动 | 正常运行 |
| 3 | R-01~06 联动规则全部触发验证 | 实际触发并执行 |
| 4 | 连续 N 次去抖机制有效 | 不误触发 |
| 5 | libedge logger 替换 CSV 日志 | 日志文件正常 |

---

## 四、四周学习时间表

```
第 1 周 ─── 阶段 10：libedge 基础库 ───
  一     二     三     四     五     六     日
  3.7   3.1    3.2   3.3    3.4   3.5    3.6+3.8
  va_   宏泛型  环形   链表   CRC   滤波   +3.9+3.10
  list          缓冲   +排序  查表   算法   +Unity
  0.5天  1天   1天   1天    1天   0.5天   1天
  ─────────────────────────────────────────────
  预计 6 天完成（含缓冲 1 天）

第 2 周 ─── 阶段 11：简单驱动 ───
  一     二     三     四     五     六     日
  1.1    relay  wdt    servo  servo  (缓冲)  (缓冲)
  misc   +测试  驱动    PWM     测试
  1天   0.5天  1天   1天    0.5天
  ─────────────────────────────────────────────
  预计 4 天完成（含缓冲 2 天）

第 3 周 ─── 阶段 12（前半）：dht11 + sr04 ───
  一     二      三      四      五     六     日
  2.1   1.2     dht11   dht11   2.2   1.4    sr04
  协议   ktime   驱动(1) 驱动(2) 原理   compl  驱动(1)
  0.5天  0.5天   1天    0.5天   0.5天 0.5天  1天
  ─────────────────────────────────────────────
  预计 5 天完成（含缓冲）

第 4 周 ─── 阶段 12（后半）+ 阶段 13 ───
  一     二      三      四      五     六     日
  sr04   mq135   mq135   can    can    阶段13 阶段13
  测试   +ADC    +kthread 协议   驱动   扩展   联动
  0.5天  0.5天   1天    1天   1天    1天   1天
  ─────────────────────────────────────────────
  预计 6 天完成（含阶段 13）

总计：~22 天（含缓冲），实际约 1 个月
```

---

## 五、API 速查

### 内核新增 API

```c
// ─── misc ───
#include <linux/miscdevice.h>
struct miscdevice mdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "dht11",
    .fops  = &dht11_fops,
};
misc_register(&mdev);
misc_deregister(&mdev);

// ─── ktime ───
u64 t1 = ktime_get_ns();
u64 delta = ktime_get_ns() - t1;

// ─── completion ───
DECLARE_COMPLETION_ONSTACK(echo_done);
wait_for_completion_interruptible_timeout(&echo_done, HZ);  // 1s 超时
// 在 ISR 中：complete(&echo_done);

// ─── kthread ───
struct task_struct *task;
task = kthread_run(thread_fn, dev, "mq135_adc");
kthread_stop(task);  // 线程内用 kthread_should_stop() 检查

// ─── local_irq ───
unsigned long flags;
local_irq_save(flags);
// ... 关键时序区 ...
local_irq_restore(flags);

// ─── ioremap ───
void __iomem *base = ioremap(0x02198000, 0x4000);  // ADC1
u32 val = readl(base + ADC_HS);
writel(val | (1<<5), base + ADC_CFG);
iounmap(base);
```

### libedge API

```c
// ─── ringbuf ───
RINGBUF_DEFINE(int_rb, int, 16);
int_rb_push(&rb, 42);
int val = int_rb_pop(&rb);

// ─── linked_list ───
LIST_DEFINE(my_list, sensor_data_t);
list_append(&my_list, &new_node);
list_sort(&my_list, compare_func);

// ─── crc ───
uint16_t crc = crc16_modbus(data, len);

// ─── filter ───
moving_avg_t filt = MOVING_AVG_INIT(8);
float smooth = moving_avg_update(&filt, raw_value);

// ─── logger ───
log_info("Temperature: %.1f°C", temp);
log_warn("Distance < 30cm: %ucm", dist);
log_error("Sensor read failed: %s", strerror(errno));
```

---

## 六、参考资源

| 资源 | 用途 | 获取方式 |
|------|------|---------|
| i.MX6ULL Reference Manual | 寄存器定义（ADC/PWM/WDOG/FlexCAN） | NXP 官网 |
| DHT11 数据手册 | 单总线时序参数 | 厂商数据手册 |
| HC-SR04 数据手册 | 超声波参数 | 厂商数据手册 |
| MQ135 数据手册 | 传感器特性曲线 | 厂商数据手册 |
| SG90/MG996R 数据手册 | 舵机参数 | 厂商数据手册 |
| CAN 2.0B 规范 | CAN 协议细节 | Bosch 官方文档 |
| Linux `Documentation/networking/can.rst` | SocketCAN 官方文档 | 内核源码树 |
| Linux `drivers/char/misc.c` | misc 驱动参考实现 | 内核源码树 |
| Linux `include/linux/ktime.h` | ktime API | 内核源码树 |
| Linux `include/linux/completion.h` | completion API | 内核源码树 |
| Linux `include/linux/kthread.h` | kthread API | 内核源码树 |
| Linux `lib/crc16.c` | CRC16 内核参考实现 | 内核源码树 |
| Unity 测试框架 | libedge 单元测试框架 | https://github.com/ThrowTheSwitch/Unity |
| Modbus 协议规范 | CRC16-Modbus 测试向量 | Modbus.org 标准文档 |
| 本项目 README.md | 项目全貌 | `Z:\imx6ull_project\README.md` |

---

*文档版本：v2.0 | 日期：2026-06-10*
