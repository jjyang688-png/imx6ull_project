# 阶段 4 学习总结（2026-05-29）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `driver/comp_drv.h`（更新） | 新增 ioctl 命令码、`LED_BLINK` 状态、`read_count` 字段 |
| `driver/comp_drv.c`（重写） | 在 Phase 3 CDEV 基础上新增：mutex 并发保护、阻塞/非阻塞 read、poll/select、fasync 异步通知、内核定时器闪烁、ioctl、sysfs、debugfs |
| `driver/comp_drv.ko` | 交叉编译产物 |

## 新接触的知识点

| 知识点 | 说明 |
|--------|------|
| **`_IO` / `_IOR` / `_IOW` 宏** | 这三个宏定义 ioctl 命令码，本质是 32 位整数，由方向 + 数据大小 + 魔数 + 序号编码而成。`_IO(magic, nr)` = 纯命令无数据（如 START_BLINK、STOP_BLINK）；`_IOR(magic, nr, type)` = 内核读数据给用户（如 GET_STATUS）；`_IOW(magic, nr, type)` = 用户写数据给内核（如 SET_BLINK_PERIOD）。三个参数：magic 是每个驱动唯一的字符（0~255），相当于命名空间防止不同驱动命令码碰撞；nr 是命令序号（1~255）；type 是数据类型，`sizeof(type)` 编码进命令码，内核据此校验用户态传入 arg 的大小，不匹配直接返回 `-ENOTTY`。ioctl 与 read/write 的区别：read 只单向流出数据，write 只单向流入数据，ioctl 则是"发一条命令 + 可选附带双向数据"，适合控制类操作 |
| **`unlocked_ioctl` vs 旧 `ioctl`** | 内核 2.6.36 后用 `unlocked_ioctl` 替代 `ioctl`，去掉了全局 BKL（Big Kernel Lock）。旧 `ioctl` 由内核统一加锁，新 `unlocked_ioctl` 要求**驱动自己加锁**——这就是 `comp_ioctl` 开头调 `mutex_lock_interruptible` 的原因 |
| **mutex 并发保护** | 多个用户进程可能同时 `open` 同一个 `/dev/comp_drv` 并发调 `write`/`ioctl`。不加锁的话，A 进程改 `led_state` 到一半、B 进程也改，最终状态不可预期。`mutex_lock_interruptible` 获取锁（可被 Ctrl+C 打断返回 `-ERESTARTSYS`），临界区内从"读状态→判断→改状态→更新硬件→通知"构成一个原子事务。mutex 允许持有者睡眠（区别于自旋锁），所以临界区内可以安全调用 `del_timer_sync`、`copy_from_user` 等可能阻塞的函数 |
| **`atomic_t` 使用场景** | `changed` 字段用 `atomic_t` 而非普通 `int`，因为它被两方访问：进程上下文（`comp_read`、`comp_poll`）和软中断上下文（定时器回调 `comp_timer_callback`）。进程上下文可以加 mutex，但软中断绝对不能睡眠——两个上下文之间协调只能用原子操作。`atomic_set` / `atomic_read` 在 ARM 上底层是单向指令，无需锁、不会睡眠 |
| **阻塞 IO：`wait_event_interruptible`** | 用户 `open` 不带 `O_NONBLOCK` 然后 `read()` → 进入 `comp_read()` → `wait_event_interruptible(wq, condition)` 将进程挂到等待队列，让出 CPU，进程进入 TASK_INTERRUPTIBLE 状态。另一进程 `echo on > /dev/comp_drv` → `comp_led_notify()` → `wake_up_interruptible(&wq)` 唤醒。关键：唤醒不等于条件满足——内核调度到该进程后**重新检查 condition**，为真才真正返回，为假继续睡。这防止了"虚假唤醒"（spurious wakeup）导致返回过期数据。返回值非零表示被信号打断（如 Ctrl+C），驱动返回 `-ERESTARTSYS` 让内核 VFS 层处理 |
| **非阻塞 IO：`O_NONBLOCK`** | 用户 `open("/dev/comp_drv", O_NONBLOCK)` 后 `filp->f_flags` 会带 `O_NONBLOCK` 标志。`comp_read` 检测到该标志后跳过 `wait_event`，拿锁、填充当前数据、立即返回。无论有没有新数据，绝不阻塞。适合轮询场景 |
| **`poll_wait` 机制** | `poll`/`select`/`epoll` 三个系统调用的内核入口都是 `file_operations->poll`。驱动实现只做两件事：① `poll_wait(filp, &wq, wait)` 把当前进程注册到等待队列（**不阻塞、不睡眠**，只是登记一下，立即返回）；② 返回可读/可写的位掩码（`POLLIN`=可读，`POLLOUT`=可写）。真正的阻塞发生在调用者那层（`do_select`/`do_poll`），驱动状态变化时通过 `wake_up_interruptible(&wq)` 唤醒所有注册的进程 |
| **fasync 异步通知（SIGIO）** | 用户态注册三步缺一不可：`signal(SIGIO, handler)` 安装信号处理函数（纯用户态）；`fcntl(fd, F_SETOWN, getpid())` 告诉驱动"信号发给谁"（存到 `filp->f_owner.pid`）；`fcntl(fd, F_SETFL, FASYNC)` 触发 `comp_fasync()` → `fasync_helper()` 将进程加入 `dev->fasync` 链表。之后驱动调 `kill_fasync(&dev->fasync, SIGIO, POLL_IN)` 时，内核遍历链表向每个进程发 SIGIO，用户进程的信号处理函数被调用，在回调中 `read()` 取数据。SIGIO 内部用自旋锁保护链表而不需要 mutex——所以定时器回调中也可以调 `kill_fasync` |
| **阻塞 read / poll / SIGIO 三者的定位** | 三种方式解决不同场景，不重复。阻塞 read：进程只关心这一个设备，"没数据就睡觉，有数据就起来读"，代码最简单；poll/select/epoll：一个线程同时等多个 fd，"哪条路有数据就从哪条路走"；SIGIO：驱动主动推送给进程，进程主循环干别的事，信号来了再处理——"轮询"的反面。三者的通知在驱动侧走同一条路径 `comp_led_notify()` → `wake_up` 照顾前两者 + `kill_fasync` 照顾第三者 |
| **内核定时器全流程** | `setup_timer(&timer, callback, (unsigned long)dev)` 初始化定时器并绑定回调；`mod_timer(&timer, jiffies + msecs_to_jiffies(ms))` 激活/修改到期时间；`del_timer_sync(&timer)` 同步删除（如果回调正在另一个 CPU 上跑，会等它跑完再返回——返回后保证不再触发）。`jiffies` 是内核全局变量，每次时钟中断 +1，`msecs_to_jiffies(500)` 把 500ms 换算成 jiffies 数。定时器回调跑在**软中断上下文**：不能睡眠、不能调 mutex、不能调 `copy_to_user`，只能做安全的原子操作 |
| **LED 闪烁状态机** | 三种状态：`LED_OFF` → `write("on")` / `ioctl(STOP)` → `LED_ON`；`LED_ON` → `write("off")` → `LED_OFF`；任意非 OFF 状态 → `ioctl(START)` → `LED_BLINK`。进入 `LED_BLINK` 时启动定时器，定时器回调中翻转 GPIO 然后 `mod_timer` 重新设定下一次到期，形成"到期→翻转→再定时"的循环。离开 `LED_BLINK` 时（write on/off 或 ioctl STOP）必须 `del_timer_sync` 停止定时器——否则定时器仍在跑，下次到期会把 GPIO 翻回去，LED 实际停不下来 |
| **sysfs 全流程** | 用户 `cat /sys/class/comp_drv/comp_drv/led_state` → VFS 根据文件对应的 `device_attribute` 找到 `led_state_show(d, attr, buf)` → 函数往 `buf` 写入字符串 → 返回写入字节数 → 内核把 buf 内容拷给用户态 → 终端显示。`DEVICE_ATTR(name, mode, show, store)` 宏展开为 `struct device_attribute dev_attr_name`。`sysfs_create_group(&dev->kobj, &grp)` 批量注册——`kobj` 是 `struct device` 内嵌的 `struct kobject`，代表该设备在 sysfs 中的目录。核心规则：一文件一值、纯文本输出、不超过 PAGE_SIZE（4096），用 `scnprintf` 返回实际写入字节数。创建失败应报错（因为通常意味着系统异常），不能像 debugfs 那样忽略 |
| **debugfs 全流程** | `debugfs_create_dir(DRV_NAME, NULL)` 创建 `/sys/kernel/debug/comp_drv/` 目录；`debugfs_create_file("info", 0444, dir, dev, &fops)` 创建 info 文件。用户 `cat` 时：`comp_debug_open` → `single_open(filp, show_fn, i_private)` 分配 `seq_file` 并关联 `comp_debug_show` → `comp_debug_show(s, v)` 通过 `seq_printf` dump 所有状态（dev 通过 `s->private` 拿到 → 即创建文件时传的 `dev`） → 内核 `seq_read()` 自动处理分页并拷给用户 → `single_release` 释放。与 sysfs 核心区别：debugfs 一个文件就可以 dump 一切，sysfs 严格要求一文件一值；debugfs 创建失败无所谓（可能 `CONFIG_DEBUG_FS` 没开），sysfs 创建失败必须处理 |
| **probe 和 remove 的对称性** | probe 中资源申请的顺序和 remove 中释放的顺序**严格相反**。最关键的两个点：① `del_timer_sync` 必须在 `cdev_del`、资源释放之前——如果先销毁 cdev 再停定时器，期间定时器到期回调访问已释放的 `dev` 直接 Oops。② sysfs group 和 debugfs 目录由内核框架在 `device_destroy` 时自动回收，不需要手动 `sysfs_remove_group` 或 `debugfs_remove`。`mutex_destroy` 显式销毁互斥锁（开启 `CONFIG_DEBUG_MUTEXES` 时内核会检查是否仍有人持有锁） |
| **goto 级联清理链的扩展** | Phase 4 新增 sysfs，清理链多了一环：`err_sysfs → device_destroy → err_device → class_destroy → err_class → cdev_del → err_cdev → unregister`。每个标签只清理"自己这层已经申请的资源"，然后 fall through 到上一层。标签命名对应出错位置——在哪一步出错就 goto 到哪个标签 |

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `COMP_DRV_MAGIC` 未定义 | 头文件中 4 个 ioctl 宏引用了 `COMP_DRV_MAGIC` 但忘了定义 | 添加 `#define COMP_DRV_MAGIC 'C'` |
| `comp_debug_fops` 末尾缺少分号 | 结构体定义漏写 `;` | 补上分号 |
| `comp_fops` 缺少 3 个函数指针 | 写了 `comp_poll` / `comp_fasync` / `comp_ioctl` 但忘了挂到 fops 结构体 | 补上 `.poll` / `.fasync` / `.unlocked_ioctl` |
| 闪烁中 write("on") 后 LED 又灭了 | 状态切到 `LED_ON` 后 GPIO 置 1，但定时器没停，下次到期又翻转 GPIO | 离开 `LED_BLINK` 前先 `del_timer_sync` |
| `#include <linux/fs.h>` 重复 | 多次编辑过程中重复添加 | 删除重复行 |
| `#include <linux/kfifo.h>` 未使用 | 最初计划用环形缓冲区，最终方案不需要 | 删除 |

## 代码规范要点

- 进程上下文用 `mutex_lock_interruptible`（可被打断），软中断用 `atomic_t`（不能睡眠）
- `wait_event_interruptible` 第二个参数是条件表达式，每次被唤醒后重新求值，防止虚假唤醒
- 离开 `LED_BLINK` 前必须 `del_timer_sync` 且在所有其他操作之前
- `del_timer_sync` 在 mutex 持有下调是安全的（mutex 可睡眠，回调不拿同一把锁）
- sysfs 创建失败 → `goto err_sysfs`；debugfs 创建失败 → 不管
- goto 标签按资源申请逆序排列，每个标签只清自己那一层
- remove 中 `del_timer_sync` 必须最先执行
- `scnprintf` 而非 `snprintf`——前者返回实际写入字节数
- 编译零警告是底线

## 下一阶段预告

**阶段 5：按键输入驱动** — 基于中断 + tasklet/workqueue 下半部、按键去抖、input subsystem 注册、event 节点上报。
