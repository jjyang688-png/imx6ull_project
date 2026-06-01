# 阶段 5 学习总结（2026-06-01）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `driver/key_input.c`（新增 283 行） | platform_driver + Input 子系统 + 中断顶半部/底半部 + workqueue 去抖 + debugfs |
| `test/test_key.c`（新增 79 行） | 从 `/dev/input/eventX` 读取标准 `struct input_event`，演示阻塞 read + poll |
| `driver/Makefile`（更新） | `obj-m` 增加 `key_input.o` |
| `app/Makefile`（更新） | 增加 `test_key` 编译目标 |

## 新接触的知识点

| 知识点 | 说明 |
|------|------|
| **中断顶半部 (Top Half)** | 运行在硬中断上下文（hardirq），要求：不能睡眠、不能做耗时操作、必须尽快返回。`key_irq_handler()` 只做两件事：原子计数 `irq_count++` 和调度底半部 `schedule_delayed_work()`。计时单位：微秒级。核心原则——"中断里能不干的就不干，登记一下然后把活丢给底半部" |
| **中断底半部 (Bottom Half) — workqueue** | 运行在进程上下文（kworker 内核线程），允许睡眠，可以做耗时操作。`key_work_func()` 在延迟结束后读取稳定 GPIO 电平、判边沿、向 input 子系统报告事件。内核提供三种底半部方案：**tasklet**（软中断上下文，不能睡眠，适合极轻量的任务）、**workqueue**（进程上下文，能睡眠，最通用）、**softirq**（最底层，极少直接用）。本驱动选择 workqueue 因为它天然适合"延迟执行 + 可能睡眠"的场景 |
| **delayed_work 去抖机制** | `INIT_DELAYED_WORK(&dev->work, key_work_func)` 初始化并绑定回调；`schedule_delayed_work(&dev->work, msecs_to_jiffies(20))` 把 work 延迟 20ms 后执行；`cancel_delayed_work_sync(&dev->work)` 取消 + 等待正在执行的完成（remove 时必须，否则回调可能访问已释放的内存）。去抖原理：按键按下时 GPIO 在几毫秒内多次抖动跳变，每次跳变都触发中断，但每次都只"重置"20ms 定时器。只有抖动结束后 20ms 内再无中断，work 才真正执行——此时电平已稳定，读到的是真实物理状态 |
| **schedule_work vs schedule_delayed_work** | `schedule_work(&work)` 立即将 work 排队执行；`schedule_delayed_work(&dwork, delay)` 先启动定时器，delay 到期后再排队执行。延迟版本天然适合去抖、超时处理、防抖等场景 |
| **spinlock 自旋锁（对比 mutex）** | 当数据同时被**中断上下文**和**进程上下文**访问时，必须用 spinlock。因为中断上下文绝对不能睡眠——如果中断里拿不到锁去睡觉，整个中断栈就废了。`spin_lock_irqsave(&lock, flags)` 做三件事：① 保存当前 CPU 的中断屏蔽状态到 flags；② 关本地中断；③ 获取自旋锁。`spin_unlock_irqrestore(&lock, flags)` 反过来：解锁 + 恢复到 flags 中保存的中断状态。这和 `spin_lock_irq` + `spin_unlock_irq` 的关键区别：后者不管调用前中断是开是关，解锁后一律开中断——如果调用者本来关了中断，就被错误地打开了 |
| **容器宏 container_of** | Linux 内核最核心的设计模式之一：已知成员指针 → 反算包含它的结构体起始地址。`container_of(ptr, type, member)` = `(type *)((char *)ptr - offsetof(type, member))`。在 key_input 中：内核回调只传 `work_struct *ws`，通过 `to_delayed_work(ws)` → `container_of(dwork, struct key_dev, work)` 拿回完整的设备结构体。这和面向对象的"从接口指针向下转型"是同一个思想 |
| **Input 子系统全流程** | ① `devm_input_allocate_device(d)` 分配 input 设备；② `__set_bit(EV_KEY, input->evbit)` 声明本设备产生按键类事件；③ `__set_bit(dev->key_code, input->keybit)` 声明能产生哪个具体键码（如 KEY_ENTER=28）；④ `input_register_device(input)` 注册后内核自动创建 `/dev/input/eventX` 节点并提供完整的 `file_operations`（阻塞/非阻塞 read、poll/select/epoll）；⑤ `input_report_key(input, code, value)` 报告事件；⑥ `input_sync(input)` 标记事件包边界（必须调！否则用户态 read 一直等不到完整事件） |
| **Input 子系统 vs 字符设备 (CDEV)** | CDEV 驱动需要自己实现完整的 `file_operations`（open/read/write/poll/ioctl），每个驱动的实现都不一样。Input 子系统把这些都封装好了：input 核心层提供统一的 `file_operations`，驱动只需"声明能力 + 报告事件"。用户态 `read(fd, &ev, sizeof(ev))` 读到标准 `struct input_event {time, type, code, value}`，格式统一，所有输入设备一视同仁 |
| **evbit / keybit 位图机制** | `__set_bit(nr, bitmap)` 在内核位图数组中将第 nr 位置 1。`evbit` 位图声明设备支持的事件类型（EV_KEY=按键, EV_REL=相对位移, EV_ABS=绝对坐标…），`keybit` 位图声明具体按键码（每个按键占一位，一个键盘驱动可能置上百位）。用户空间通过 `evtest` 或直接 ioctl 查询这些位图来确定设备能力——没有这两个 `__set_bit`，用户程序不知道该设备能干什么 |
| **双边沿触发中断** | `IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING` — GPIO 配置为 active-low 时，按键按下产生下降沿（高→低），释放产生上升沿（低→高）。注册双沿触发后，两个方向都产生中断。这与设备树 `interrupts = <18 IRQ_TYPE_EDGE_BOTH>` 对应 |
| **GPIO 输入 vs 输出模式** | comp_drv（LED）用 `devm_gpiod_get(d, "led", GPIOD_OUT_LOW)` — 输出模式，初始低电平（LED 灭）；key_input（按键）用 `devm_gpiod_get(d, "key", GPIOD_IN)` — 输入模式，读取外部信号电平。前者调用 `gpiod_set_value()` 主动输出，后者调用 `gpiod_get_value()` 被动读取 |
| **gpiod_to_irq** | GPIO 描述符 → 全局 IRQ 编号的转换。硬件路径：SoC 引脚 → IOMUXC 复用为 GPIO → GPIO 控制器中对应位 → GIC 中断控制器的对应 IRQ 线。`gpiod_to_irq()` 查询 GPIO 控制器的中断映射表返回该 IRQ 号 |
| **devm_ 托管资源机制** | `devm_kzalloc`、`devm_gpiod_get`、`devm_input_allocate_device`、`devm_request_irq` — 全部以 `d`（`struct device *`）为锚点。probe 失败或 remove 返回后，内核按**后进先出**顺序自动释放所有托管资源（IRQ → input → GPIO → kzalloc）。不需要手动 `free_irq`、`input_unregister_device`、`gpiod_put`、`kfree`。但注意：Linux 4.1.15 没有 `devm_debugfs_create_dir`，所以 debugfs 仍需手动清理；workqueue 也不是 devm_ 资源，需要手动 cancel |
| **module_param 模块参数** | `module_param(var, type, perm)` 宏将全局变量暴露为可配置参数：insmod 时命令行传入（`insmod key_input.ko default_debounce=30`）；运行时通过 sysfs 读写（`/sys/module/key_input/parameters/default_debounce`）；`modinfo` 可查看（配合 `MODULE_PARM_DESC`）。类型名是内核专用的：`int`、`bool`、`charp`（字符串指针）等。等效于用户态程序的命令行参数解析 |
| **msecs_to_jiffies** | 把人类可读的毫秒数转换为内核的 `jiffies` 单位。`jiffies` 是内核全局变量，每次定时器中断（tick）+1。在 HZ=100 的系统上，`msecs_to_jiffies(20)` = 2（20ms = 2 个 tick） |
| **GFP_KERNEL vs GFP_ATOMIC** | 内存分配标志：`GFP_KERNEL` 允许睡眠（可能触发页面回收），用于进程上下文（probe、read、write）；`GFP_ATOMIC` 不允许睡眠，用于中断上下文或持自旋锁时。本驱动 probe 中全用 `GFP_KERNEL`（probe 跑在进程上下文，可以睡眠） |

## 与 comp_drv 的核心差异

| 维度 | comp_drv (Phase 3/4) | key_input (Phase 5) |
|------|------|------|
| 设备类型 | 字符设备 (CDEV) | Input 子系统设备 |
| 用户接口 | 自己实现 file_operations | 内核提供 /dev/input/eventX |
| 触发源 | 用户态主动 write | 硬件中断被动触发 |
| GPIO 方向 | 输出 (GPIOD_OUT_LOW) | 输入 (GPIOD_IN) |
| 去抖 | 不适用（无物理抖动） | delayed_work 20ms 去抖 |
| 锁类型 | mutex（可睡眠） | spinlock（不可睡眠） |
| 底半部 | 无（定时器回调本身在软中断上下文） | delayed_work（进程上下文） |
| 核心输出 API | copy_to_user | input_report_key + input_sync |
| 特殊头文件 | fs.h, cdev.h, poll.h, timer.h | interrupt.h, input.h, workqueue.h, spinlock.h |

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| （编译验证待完成） | 尚未在 Linux 编译环境验证 | 下一批次编译验证后补充 |

## 代码规范要点

- 中断处理函数（顶半部）绝不能睡眠，只能 spinlock + 原子操作
- `spin_lock_irqsave` / `spin_unlock_irqrestore` 必须严格配对（保存/恢复中断状态）
- `input_report_key` 后必须紧跟 `input_sync`，否则事件包不完整
- `cancel_delayed_work_sync` 必须在 `devm_*` 自动释放前调用（remove 中第一优先级）
- debugfs 创建失败用 `IS_ERR_OR_NULL` 宽容处理（内核可能没开启 CONFIG_DEBUG_FS）
- `container_of` 的前提是确保传入的成员指针确实属于目标结构体
- GPIO active-low 时，`gpiod_get_value() == 0` = 按下，`== 1` = 释放，注意逻辑反转
- 模块参数权限用八进制（如 0644 = 用户可读写，组和其他只读）

## 下一阶段预告

**阶段 6：I2C + SPI 传感器驱动** — I2C 子系统驱动 AP3216C 光照/接近传感器；SPI 子系统驱动 ICM20608 六轴传感器；基于 I2C/SPI 总线框架的 probe/remove；`struct i2c_driver` 和 `struct spi_driver` 注册模型；`regmap` 简化寄存器访问（可选）；`input_report_abs` / `input_report_rel` 上报传感器数据；通过 sysfs 暴露原始传感器值。
