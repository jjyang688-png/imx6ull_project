# 全项目知识点汇总

本文档按主题汇总 9 个阶段学到的全部知识点，每一条标注来源阶段，方便按需翻阅对应的阶段报告 (`docs/phases/phaseN_report.md`)。

---

## 一、Linux 内核驱动框架

### 驱动注册方式（4 种）

| 注册方式 | 使用场景 | 对应驱动 | 关键 API | 阶段 |
|------|------|------|------|------|
| `platform_driver` + CDEV | 平台设备 (GPIO/UART) | comp_drv, uart_sensor | `module_platform_driver()` | 3, 7 |
| `platform_driver` + Input 子系统 | 输入设备 | key_input | `input_register_device()` | 5 |
| `spi_driver` + CDEV | SPI 从设备 | icm20608 | `module_spi_driver()` | 6 |
| `i2c_driver` + CDEV | I2C 从设备 | ap3216c | `module_i2c_driver()` | 6 |

### CDEV 三步注册（贯穿全项目）

```c
alloc_chrdev_region(&dev_id, 0, count, "name");   // ① 分配设备号
cdev_init(&cdev, &fops);                            // ② 初始化 cdev
cdev_add(&cdev, dev_id, count);                     // ③ 注册到内核
class_create() + device_create();                   // 创建 /dev/xxx 节点
```

### probe / remove 模式

| 要点 | 阶段 |
|------|------|
| probe 成功才创建设备节点，失败必须释放所有已申请资源 | 3-7 |
| remove 顺序与 probe 严格相反 | 3-7 |
| goto 错误清理链是内核标准模式 | 3 |
| devm_ 资源自动释放，非 devm 必须手动释放 | 3-7 |

---

## 二、设备树 (Device Tree)（阶段 2）

### DTS 编写要点

| 知识点 | 说明 |
|------|------|
| `compatible` 属性 | 驱动和设备的匹配字符串 |
| `reg` 属性 | I2C=从地址、SPI=片选号、platform=寄存器基地址 |
| `interrupts` | 3 元组 `<GIC_SPI 号 触发类型>` |
| `gpios` | `<&gpioX 编号 有效电平>` |
| `pinctrl-0` | 引用引脚复用配置 |
| `&uart3 { status = "disabled" }` | 禁用内核自带驱动 |
| `status = "okay"` | 启用设备节点 |

### DTS → 驱动取值 API

| 属性类型 | API | 示例属性 |
|------|------|------|
| GPIO | `devm_gpiod_get(dev, suffix, flags)` | `led-gpios = <...>` |
| 中断号 | `platform_get_irq(pdev, 0)` / `gpiod_to_irq()` | `interrupts = <...>` |
| u32 数值 | `of_property_read_u32(node, prop, &val)` | `key-code = <28>` |
| 内存资源 | `platform_get_resource(pdev, IORESOURCE_MEM, 0)` | `reg = <addr len>` |
| 时钟 | `devm_clk_get(dev, name)` | `clocks = <...>` |

---

## 三、并发与中断（阶段 4、5、7）

### 锁选择

| 锁类型 | 可睡眠？ | 使用场景 | 驱动 |
|------|------|------|------|
| mutex | 是 | 进程上下文，保护设备状态 | comp_drv, ap3216c, icm20608, uart_sensor |
| spinlock | 否 | 中断上下文，保护与 ISR 共享的数据 | key_input |

### 中断上下文约束

| 操作 | 进程上下文 | 中断上下文 |
|------|------|------|
| mutex_lock | OK | 禁止 |
| kmalloc(GFP_KERNEL) | OK | 禁止 |
| copy_to_user | OK | 禁止 |
| kfifo_in | OK | OK |
| wake_up_interruptible | OK | OK |

### 中断处理模式

| 机制 | 上下文 | 可睡眠？ | 场景 |
|------|------|------|------|
| 上半部 (ISR) | 中断 | 否 | 读寄存器、调度下半部 |
| tasklet | 软中断 | 否 | 简单快速的下半部 |
| workqueue | 进程 | 是 | 耗时操作 |
| delayed_work | 进程 | 是 | 自带延迟，天然适合去抖 |

---

## 四、IO 模型（阶段 4、7、8）

| IO 模型 | 驱动端 API | 用户端 API | 驱动 |
|------|------|------|------|
| 阻塞读 | `wait_event_interruptible()` | `read()` | comp_drv, uart_sensor |
| 非阻塞读 | 检查 `O_NONBLOCK`，返回 `-EAGAIN` | `open(O_NONBLOCK)` | comp_drv, uart_sensor |
| poll/epoll | `poll_wait()` | `poll()` / `epoll_wait()` | comp_drv, uart_sensor |
| 异步通知 | `kill_fasync()` | `fcntl(FASYNC)` + `signal(SIGIO)` | comp_drv |
| ioctl | `unlocked_ioctl` | `ioctl()` | comp_drv, icm20608 |

### epoll 三件套（阶段 8）

```c
int epfd = epoll_create1(0);
epoll_ctl(epfd, EPOLL_CTL_ADD, dev_fd, &event);
int nfds = epoll_wait(epfd, events, max, timeout_ms);
```

**select vs poll vs epoll：** select 有 fd 上限 (FD_SETSIZE=1024)；poll 无上限但每次全量遍历；epoll 红黑树+就绪链表，O(1)。

---

## 五、总线通信（阶段 6）

### I2C 关键 API

| API | 场景 |
|------|------|
| `i2c_smbus_read_byte_data(client, reg)` | 单寄存器读 |
| `i2c_smbus_write_byte_data(client, reg, val)` | 单寄存器写 |
| `i2c_transfer(adapter, msgs, num)` | 多字节突发读（一次 START-STOP） |

### SPI 关键 API

| API | 场景 |
|------|------|
| `spi_message_init(&msg)` | 初始化 message |
| `spi_message_add_tail(&t, &msg)` | 追加 transfer |
| `spi_sync(spi, &msg)` | 同步执行 |
| 一次读 = 2 个 transfer（发地址 + 收数据） | CS 全程保持低 |

### SPI vs I2C

| | SPI | I2C |
|------|------|------|
| 线数 | 4 线 | 2 线 |
| 双工 | 全双工 | 半双工 |
| 速度 | 8MHz+ | 100~400kHz |
| DTS 的 reg | CS 编号 | 从地址 |
| 驱动类型 | spi_driver | i2c_driver |

---

## 六、UART 裸寄存器（阶段 7）

### 寄存器访问 API

| API | 用途 |
|------|------|
| `devm_ioremap_resource()` | 物理地址 → 虚拟地址 |
| `readb(addr)` / `writeb(val, addr)` | 8 位读写 (URXD/UTXD) |
| `readl(addr)` / `writel(val, addr)` | 32 位读写 (UCRx/USRx) |

### 初始化顺序

```
① UCR2_SRST 复位  → ② UCR2 帧格式  → ③ UCR3 DCE 选择
→ ④ UFCR 阈值    → ⑤ UBIR/UBMR 波特率 → ⑥ UCR1+UCR4 总使能
```

### kfifo 环形缓冲

| API | 说明 |
|------|------|
| `DECLARE_KFIFO(name, type, size)` | 结构体中声明 |
| `INIT_KFIFO(fifo)` | 使用前初始化 |
| `kfifo_in(fifo, buf, n)` | 写入，中断安全 |
| `kfifo_to_user(fifo, ubuf, n, &copied)` | 取出+拷贝到用户空间 |
| `kfifo_is_empty(fifo)` | 判空 |

---

## 七、用户空间编程（阶段 8）

### 核心 API

| API | 用途 |
|------|------|
| `epoll_create1(0)` | 创建 epoll 实例 |
| `epoll_ctl(epfd, ADD/MOD/DEL, fd, &ev)` | 注册/修改/移除监听 |
| `epoll_wait(epfd, events, max, timeout)` | 阻塞等待事件 |
| `struct input_event` | 读取按键事件 |
| `fcntl(fd, F_SETFL, O_NONBLOCK)` | 设置非阻塞 |
| `/proc/uptime` | 系统运行秒数 |
| `access(path, F_OK)` | 检查文件是否存在 |

### 设计模式

| 模式 | 说明 |
|------|------|
| 查表法 | `struct {name, handler}[]` 替代 if-else 链 |
| 行缓冲拼包 | `static char buf[] + line_pos` 处理字节流分片 |
| Toggle 状态机 | `static int` 实现按键交替开关 |
| 信号优雅退出 | `signal()` 只置标志位，主循环判断退出 |

---

## 八、字节序（阶段 6）

| | ICM20608（大端） | AP3216C（小端） |
|------|------|------|
| 0x1234 存储 | `[0]=0x12, [1]=0x34` | `[0]=0x34, [1]=0x12` |
| 组合 | `(buf[0]<<8) \| buf[1]` | `(buf[1]<<8) \| buf[0]` |

---

## 九、踩坑总录

| 问题 | 原因 | 解决 | 阶段 |
|------|------|------|------|
| RXDMUXSEL 忘设 | DCE/DTE 通路未选择 | `writel(RXDMUXSEL, UCR3)` | 7 |
| RXTL 设大不触发中断 | FIFO 阈值太高 | RXTL=1 | 7 |
| 大写"ON"→LED不响应 | comp_drv 只认小写 | 改为小写 | 8 |
| KEY0 按一次闪一下就灭 | 没过滤 value=0 | 只处理 value==1 | 8 |
| `spi_write_then_read` 不存在 | 4.1.15 无此 API | `spi_sync`+手动拼 transfer | 6 |
| `va_list` 编译报错 | 缺少 `<stdarg.h>` | 添加头文件 | 8 |

---

## 十、完整文件索引

### 驱动层
| 文件 | 行 | 核心知识点 |
|------|------|------|
| `driver/comp_drv.c` | 520 | CDEV, mutex, wait_event, poll, fasync, ioctl, sysfs, debugfs, 内核定时器 |
| `driver/key_input.c` | 280 | Input 子系统, 中断+下半部, workqueue, spinlock |
| `driver/ap3216c.c` | 320 | I2C, SMBus, i2c_transfer, 小端字节序 |
| `driver/icm20608.c` | 450 | SPI, spi_message/transfer, 大端字节序, ioctl |
| `driver/uart_sensor.c` | 580 | ioremap, 裸寄存器, RX中断, kfifo, 阻塞/非阻塞, poll |

### 用户层
| 文件 | 行 | 核心知识点 |
|------|------|------|
| `app/smart_monitor.c` | 800 | epoll, input_event, 行缓冲, 查表法, CSV 日志 |

### 文档层
| 文件 | 内容 |
|------|------|
| `docs/REQUIREMENTS.md` | 需求规格 |
| `docs/ARCHITECTURE.md` | 系统架构 |
| `docs/API_REFERENCE.md` | 接口手册 |
| `docs/HARDWARE.md` | 硬件资源 |
| `docs/LESSONS_LEARNED.md` | 本文件 |
| `docs/phases/phase1~8_report.md` | 8 份阶段学习报告 |

---

*文档版本：v1.0 | 日期：2026-06-08*
