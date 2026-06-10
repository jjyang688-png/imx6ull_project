# Linux 驱动阻塞/唤醒全链路详解

> 以 i.MX6ULL 智能环境监测项目为例，完整追踪从用户空间 `epoll_wait()` 阻塞，到硬件中断唤醒，再到用户空间接收事件的全过程。

---

## 目录

1. [核心概念速览](#1-核心概念速览)
2. [三种机制对照](#2-三种机制对照)
3. [关键数据结构](#3-关键数据结构)
4. [完整链路追踪（以 UART 为主线）](#4-完整链路追踪以-uart-为主线)
5. [comp_drv (LED) 的阻塞/唤醒链路](#5-comp_drv-led-的阻塞唤醒链路)
6. [key_input (按键) 的隐式 poll](#6-key_input-按键的隐式-poll)
7. [无 poll 的传感器驱动](#7-无-poll-的传感器驱动)
8. [阻塞 read 与 poll 的关系](#8-阻塞-read-与-poll-的关系)
9. [常见误解澄清](#9-常见误解澄清)
10. [附录：完整时序图](#10-附录完整时序图)

---

## 1. 核心概念速览

| 概念 | 做什么 | 发生在 |
|------|--------|--------|
| `poll_wait()` | 在驱动的等待队列上**注册**回调函数（不阻塞！） | `epoll_ctl()` 调用时 |
| `wake_up()` | 遍历等待队列，**唤醒**所有等待者 | 中断 / write / ioctl 中 |
| `epoll_wait()` | 检查就绪链表，空则**睡眠**，有则**返回** | 用户态主循环 |
| `schedule()` | 让出 CPU，进程真正睡觉的内核函数 | `epoll_wait` 内部 |

**一句话**：`poll_wait` 是"留电话号码"，`wake_up` 是"打电话"，`epoll_wait` 是"等着电话响"。

---

## 2. 三种机制对照

本项目 5 个驱动使用了三种不同的"可读通知"方式：

| 机制 | 驱动 | 特点 |
|------|------|------|
| **显式 poll + 等待队列** | `uart_sensor.c`、`comp_drv.c` | 驱动自己实现 `.poll`，自己调用 `wake_up` |
| **隐式 poll（内核提供）** | `key_input.c` | 驱动用 `input_report_key()` 上报，内核 `evdev.c` 自动提供 poll |
| **无 poll（默认可读）** | `ap3216c.c`、`icm20608.c` | 没有 `.poll`，VFS 默认行为：永远返回 `POLLIN` |

---

## 3. 关键数据结构

### 3.1 驱动侧：等待队列头 + 等待队列条目

```c
// 驱动里定义一个"等待队列头"——所有等待者都挂在这下面
// 类型：wait_queue_head_t（实际是 spinlock + list_head）

// uart_sensor.c — 在设备结构体里
struct uart_sensor_dev {
    // ...
    wait_queue_head_t rx_wq;   // ← "等待队列头"，所有等数据的人排在这里
    DECLARE_KFIFO(rx_fifo, unsigned char, 64);  // 数据缓冲区
};

// comp_drv.c — 在设备结构体里
struct comp_dev {
    // ...
    wait_queue_head_t wq;      // ← 同上，等 LED 状态变化的人排在这里
    atomic_t changed;           // 状态变化标记
};
```

**等待队列头 vs 等待队列条目**：

```
wait_queue_head_t (头，嵌在设备结构体里，驱动持有)
    │
    ├── wait_queue_entry_t (条目1) ← 由 epoll_ctl 时创建，包含回调函数指针
    ├── wait_queue_entry_t (条目2) ← 由阻塞 read() 时创建，包含进程指针
    └── wait_queue_entry_t (条目3) ← ...
```

### 3.2 epoll 侧：epoll 实例 + epitem + eppoll_entry

```c
// 内核源码 fs/eventpoll.c（简化版）

struct eventpoll {
    wait_queue_head_t wq;        // epoll 自己的等待队列（进程睡这里）
    struct list_head rdllist;    // 就绪链表（已就绪的 fd 列表）
    struct rb_root_cached rbr;   // 红黑树（所有被监听的 fd）
};

struct epitem {
    struct rb_node rbn;          // 红黑树节点
    struct list_head rdllink;    // 就绪链表节点
    struct eventpoll *ep;        // 指向所属 epoll 实例
    struct epoll_event event;    // 用户传入的 events（含 EPOLLIN 等）
    struct file *ffd;            // 被监听的文件
};

struct eppoll_entry {
    struct epitem *epi;          // 指向所属 epitem
    wait_queue_entry_t wait;     // 等待队列条目（挂在驱动 wq 上）
    wait_queue_head_t *whead;    // 指向驱动的等待队列头
};
```

**三者的关系图**：

```
epoll 实例 (struct eventpoll)
├── rbr 红黑树
│   └── epitem (uart_fd) ──────► struct file (uart_sensor)
│       │                           │
│       │  "被谁监听"                 │  "指向谁的私有数据"
│       │                           ▼
│       │                  uart_sensor_dev
│       │                  ├── rx_wq (wait_queue_head_t)
│       │                  │     │
│       │                  │     ├── entry: eppoll_entry.wait ──┐
│       │                  │     │       回调: ep_poll_callback  │
│       │                  │     │       回指: epitem ◄──────────┘
│       │                  │     │
│       │                  │     └── entry: 阻塞 read 的等待条目
│       │                  │              (直接指向进程 task_struct)
│       │                  │
│       │                  └── rx_fifo (kfifo)
│       │
└── wq (epoll 自己的等待队列)
    └── 进程 A 的等待条目 (smart_monitor 进程睡在这里)
```

---

## 4. 完整链路追踪（以 UART 为主线）

### 第一阶段：epoll 初始化 (`smart_monitor.c`)

#### Step 1-1: 创建 epoll 实例

```c
// smart_monitor.c:427
int epfd = epoll_create1(0);
```

<details>
<summary><b>内核内部做了什么？</b></summary>

```
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
    // 1. 分配 struct eventpoll
    struct eventpoll *ep = kzalloc(sizeof(*ep));

    // 2. 初始化 epoll 自己的等待队列头
    init_waitqueue_head(&ep->wq);

    // 3. 初始化就绪链表（空的）
    INIT_LIST_HEAD(&ep->rdllist);

    // 4. 初始化红黑树（空的，等 epoll_ctl 添加）
    ep->rbr = RB_ROOT_CACHED;

    // 5. 分配一个文件描述符并返回
    fd = get_unused_fd_flags(flags);
    file = anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep, ...);
    fd_install(fd, file);
    return fd;   // ← 返回给用户空间: epfd = 3 (假设)
}
```

此时 epoll 实例是空的——没有任何 fd 被监视，红黑树和就绪链表都是空的。
</details>

#### Step 1-2: 注册 fd 到 epoll

```c
// smart_monitor.c:434-450
for (int i = 0; i < MAX_DEVICES; i++) {
    int fd = g_device_fds[i];     // 比如 uart_fd = 7
    if (fd < 0) continue;

    struct epoll_event ev;
    ev.events   = EPOLLIN;              // 告诉内核：我关心"可读"
    ev.data.ptr = &g_device_fds[i];     // 附带指针，唤醒时用它识别是谁

    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}
```

<details>
<summary><b>一次 epoll_ctl 调用的内核完整路径</b></summary>

```
SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd, struct epoll_event __user *, event)
│
├── 1. 通过 epfd 找到 struct eventpoll *ep
│
├── 2. 通过 fd  找到 struct file *file (指向 uart_sensor 的 file)
│
├── 3. ep_insert(ep, event, file, fd)
│       │
│       ├── 3a. 分配 struct epitem
│       │       epi = kmem_cache_alloc(epitem_cache, GFP_KERNEL);
│       │       epi->ep   = ep;                // 指向 epoll 实例
│       │       epi->ffd  = file;              // 指向被监听的文件
│       │       epi->event = *event;           // 保存用户传入的 EPOLLIN
│       │
│       ├── 3b. 插入红黑树
│       │       ep_rbtree_insert(ep, epi);     // key 是 fd 编号
│       │
│       ├── 3c. ★★★ 核心：设置 poll 回调 ★★★
│       │       epq.pt._qproc = ep_ptable_queue_proc;  // 设置回调构建函数
│       │       epq.pt._key   = epi->event.events;     // POLLIN
│       │
│       ├── 3d. ★★★ 调用驱动的 .poll() ★★★
│       │       revents = ep_item_poll(epi, &epq.pt);
│       │       │
│       │       │  // ep_item_poll 内部：
│       │       │  revents = file->f_op->poll(file, &epq.pt);
│       │       │  //              ↑
│       │       │  //              这就是我们驱动里的 uart_sensor_poll()！
│       │       │
│       │       └── uart_sensor_poll() 返回了什么？
│       │              │
│       │              ├── 调用 poll_wait(filp, &dev->rx_wq, &epq.pt)
│       │              │       │
│       │              │       └── epq.pt._qproc(filp, &dev->rx_wq, epq.pt)
│       │              │           = ep_ptable_queue_proc(filp, &dev->rx_wq, epq.pt)
│       │              │               │
│       │              │               ├── 分配 struct eppoll_entry *pwq
│       │              │               ├── pwq->whead = &dev->rx_wq  (指向驱动的 wq)
│       │              │               ├── pwq->epi   = epi
│       │              │               ├── 设置 pwq->wait.func = ep_poll_callback
│       │              │               │        ★ 这就是驱动 wake_up 时会调的回调函数
│       │              │               │
│       │              │               └── add_wait_queue(&dev->rx_wq, &pwq->wait);
│       │              │                   ★ 把这个条目挂到驱动的 rx_wq 上
│       │              │
│       │              ├── 检查 kfifo_is_empty(&dev->rx_fifo)
│       │              │       ↓
│       │              │   ┌── 空：还没收到过数据
│       │              │   │   → 不设置 POLLIN
│       │              │   │   → 返回 mask = POLLOUT | POLLWRNORM
│       │              │   │
│       │              │   └── 非空：之前中断已经写入了数据
│       │              │       → 设置 mask |= POLLIN | POLLRDNORM
│       │              │       → 返回 mask = POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM
│       │              │
│       │              └── revents = 返回值
│       │
│       └── 3e. 根据 revents 决定是否加入就绪链表
│               │
│               ├── revents & POLLIN (即有数据)：
│               │   ep_poll_safewake(ep, epi);  ← 加入就绪链表，唤醒 epoll_wait
│               │   ★ 如果 epoll_wait() 已经在等，会立即被唤醒
│               │
│               └── revents 不含 POLLIN (没数据)：
│                   不做任何事。fd 已在红黑树中，回调已挂好，等待将来唤醒。
│
└── 4. 返回 0（成功）
```

</details>

**关键理解**：`poll_wait()` 在 `epoll_ctl` 时被调用，**不是在 `epoll_wait` 时**。它只做一件事：把 epoll 的回调函数挂到驱动的等待队列上，然后**立即返回**。

### 第二阶段：epoll_wait 阻塞

#### Step 2-1: 进入主循环

```c
// smart_monitor.c:662-665
while (g_running)
{
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
    // 如果所有 fd 都没有数据，进程停在这里
```

<details>
<summary><b>epoll_wait 内核路径</b></summary>

```
SYSCALL_DEFINE4(epoll_wait, int, epfd, struct epoll_event __user *, events,
                int, maxevents, int, timeout)
{
    // 1. 通过 epfd 找到 struct eventpoll *ep
    struct eventpoll *ep = ...

    // 2. 调用 ep_poll(ep, events, maxevents, timeout)
    return ep_poll(ep, events, maxevents, timeout);
}

ep_poll(ep, events, maxevents, timeout):
│
├── 1. 检查就绪链表 rdllist
│       if (!list_empty(&ep->rdllist))
│           goto send_events;     // ← 已有就绪 fd，不睡眠，直接返回
│
├── 2. 就绪链表为空 → 准备睡眠
│       init_waitqueue_entry(&wait, current);   // 用当前进程创建等待条目
│
├── 3. 把进程挂到 epoll 自己的等待队列上
│       __add_wait_queue_exclusive(&ep->wq, &wait);
│
├── 4. 设置进程状态为可中断睡眠
│       set_current_state(TASK_INTERRUPTIBLE);
│
├── 5. ★★★ 进程在这里交出 CPU ★★★
│       if (!list_empty(&ep->rdllist))
│           break;           // 再检查一次（防止睡眠前刚好来了事件）
│       schedule();          // ← 调度器：进程睡眠，CPU 运行别的进程
│       │
│       │  ... 时间流逝，进程不占用任何 CPU ...
│       │  ... 直到被 wake_up 唤醒 ...
│       │
│
├── 6. 被唤醒后：清理状态
│       set_current_state(TASK_RUNNING);
│       __remove_wait_queue(&ep->wq, &wait);
│
└── 7. 收集就绪事件
    send_events:
        遍历 ep->rdllist
        每个就绪的 epitem 调用 ep_item_poll() 再次确认可读
        把结果拷贝到用户空间 events[]
        返回事件数量 nfds
```

</details>

**此时用户态的状态**：

```c
// smart_monitor.c:665
int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
// ★ 进程在这里"卡住了"
// ★ top/htop 显示进程状态为 "S" (Sleeping/Interruptible)
// ★ CPU 占用为 0%，不空转
```

### 第三阶段：硬件中断到达

#### Step 3-1: 物理层

```
PC 串口助手 ──RS232──► UART3_RXD 引脚 (GPIO_IO16)
                          │
                          ▼
                   UART3 硬件模块 (基址 0x021EC000)
                   ├── 移位寄存器 → 采样 RXD 线
                   ├── RX FIFO (32×8bit) ← 硬件自动填充
                   ├── USR2 寄存器: RDR bit = 1 (Receive Data Ready)
                   └── UCR4 寄存器: DREN bit = 1 (RX Ready Interrupt Enable)
                          │
                          ▼  中断请求 (SPI 编号 33 + 32 = 65，见参考手册)
                   ┌──────────────┐
                   │  GIC (Generic │  通用中断控制器
                   │  Interrupt   │  路由到 Cortex-A7 核 0
                   │  Controller) │
                   └──────┬───────┘
                          ▼
                   ARM Cortex-A7
                   ├── 保存当前上下文 (寄存器、PC、CPSR)
                   ├── 切换到 IRQ 模式
                   ├── 跳转到中断向量表 (0xFFFF0018)
                   │     → vector_irq → __irq_svc → gic_handle_irq
                   └── 读取 GIC_IAR 获取硬件中断号
```

#### Step 3-2: 中断框架层

```
gic_handle_irq()
  │
  ├── 硬件中断号 → Linux 虚拟中断号 (hwirq_to_virq)
  │
  ├── handle_level_irq(irq) 或 handle_edge_irq(irq)
  │     │
  │     ├── 屏蔽此中断（防止嵌套）
  │     │
  │     └── handle_irq_event(desc)
  │           │
  │           └── 遍历 desc->action 链表
  │                 调用每个 action->handler(irq, action->dev_id)
  │                    │
  │                    └── ★ 这就是我们注册的中断处理函数 ★
```

#### Step 3-3: 驱动中断处理函数

```c
// driver/uart_sensor.c:210-229
static irqreturn_t uart_rx_interrupt(int irq, void *dev_id)
{
    struct uart_sensor_dev *dev = dev_id;    // ← probe 时传入的
    void __iomem *base = dev->base;          // = ioremap(0x021EC000)
    u8 byte;

    // ── 第一步：确认中断来源 ──
    if (!(readl(base + USR2) & USR2_RDR))    // USR2 = base + 0x98
        return IRQ_NONE;                     // 不是 RX 中断，不处理

    // ── 第二步：循环读空硬件 RX FIFO ──
    while (readl(base + USR2) & USR2_RDR)    // FIFO 不空就继续读
    {
        byte = readb(base + URXD) & 0xFF;    // URXD = base + 0x00
        kfifo_in(&dev->rx_fifo, &byte, 1);   // 写入软件环形缓冲
        dev->rx_bytes++;                     // 统计
    }

    // ── 第三步：★ 唤醒等待者 ★ ──
    wake_up_interruptible(&dev->rx_wq);
    //        │
    //        └── 详细展开见下方 3.4

    return IRQ_HANDLED;
}
```

#### Step 3-4: wake_up_interruptible 的完整路径

```
wake_up_interruptible(&dev->rx_wq)
│
│  // 内核源码 include/linux/wait.h:
│  #define wake_up_interruptible(x)  __wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
│
└── __wake_up(&dev->rx_wq, TASK_INTERRUPTIBLE, 1, NULL)
      │
      └── __wake_up_common(&dev->rx_wq, TASK_INTERRUPTIBLE, 1, 0, NULL)
            │
            ├── 拿锁：spin_lock_irqsave(&wq_head->lock, flags)
            │
            ├── 遍历驱动等待队列 dev->rx_wq 上的所有条目：
            │     list_for_each_entry_safe(curr, next, &wq_head->head, entry)
            │     {
            │         // 对每个条目调用其 .func 回调
            │         ret = curr->func(curr, TASK_INTERRUPTIBLE, 1, key);
            │         //              ↑
            │         //   对于 epoll 条目: .func = ep_poll_callback
            │         //   对于阻塞 read 条目: .func = autoremove_wake_function
            │         //             (autoremove_wake_function 内部调用 default_wake_function
            │         //              把对应进程设为 TASK_RUNNING 并加入调度器运行队列)
            │     }
            │
            └── 放锁：spin_unlock_irqrestore(&wq_head->lock, flags)
```

**对于 epoll 注册的条目**，`ep_poll_callback` 被调用：

```c
// 内核源码 fs/eventpoll.c
static int ep_poll_callback(wait_queue_entry_t *wait, unsigned mode, int sync, void *key)
{
    // 1. 从等待条目找到 eppoll_entry
    struct eppoll_entry *pwq = container_of(wait, struct eppoll_entry, wait);

    // 2. 找到 epitem
    struct epitem *epi = pwq->epi;

    // 3. 找到 epoll 实例
    struct eventpoll *ep = epi->ep;

    // 4. ★ 把这个 epitem 加入 epoll 的就绪链表 ★
    if (!ep_is_linked(&epi->rdllink))
        list_add_tail(&epi->rdllink, &ep->rdllist);

    // 5. ★ 唤醒睡在 ep->wq 上的进程 ★
    //    （就是我们的 smart_monitor 进程）
    wake_up(&ep->wq);
    //    │
    //    └── 这会调用 ep->wq 上等待条目的 .func
    //        即 default_wake_function(current)
    //        把 smart_monitor 进程:
    //          - 设为 TASK_RUNNING
    //          - 放入调度器运行队列
    //          - 等待下次调度拿到 CPU

    return 1;
}
```

**对于阻塞 read() 注册的条目**，`autoremove_wake_function` 被调用：

```c
// 内核源码 kernel/sched/wait.c
int autoremove_wake_function(wait_queue_entry_t *wq_entry, unsigned mode, int sync, void *key)
{
    // 1. 从等待队列中摘下这个条目
    list_del_init(&wq_entry->entry);

    // 2. 唤醒该条目对应的进程
    return default_wake_function(wq_entry, mode, sync, key);
}

int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int sync, void *key)
{
    // ★ 把进程设为可运行状态，放入调度器运行队列
    return try_to_wake_up(curr->private, mode, sync);
    // curr->private = task_struct (调用 wait_event_interruptible 时设置的)
}
```

### 第四阶段：进程被唤醒

#### Step 4-1: 调度器恢复进程

```
schedule() 下次调度时:
  │
  ├── 发现 smart_monitor 进程状态 = TASK_RUNNING
  ├── 选中它运行
  └── 进程从 schedule() 调用处返回（回到 ep_poll 函数中）
```

#### Step 4-2: epoll_wait 继续执行

```c
// 回到 ep_poll() 函数内部
ep_poll(ep, events, maxevents, timeout):
│
│  // ... 从 schedule() 返回 ...
│
├── 设置回 TASK_RUNNING
│       set_current_state(TASK_RUNNING);
│
├── 从 ep->wq 中移除自己
│       __remove_wait_queue(&ep->wq, &wait);
│
├── 遍历就绪链表 ep->rdllist
│       for each epitem in ep->rdllist:
│           │
│           ├── 再次确认可读（调用驱动的 .poll）
│           │       revents = ep_item_poll(epi, &pt);
│           │       // = uart_sensor_poll(file, &pt)
│           │       //   → poll_wait()  (这次注册的条目基本没用，立即被舍弃)
│           │       //   → !kfifo_is_empty() → true
│           │       //   → 返回 POLLIN | POLLRDNORM
│           │
│           ├── 如果确实可读 (revents & events)：
│           │       填 events[idx].events   = revents & epi->event.events
│           │       填 events[idx].data.ptr = epi->event.data.ptr
│           │       //                  ↑ = &g_device_fds[4]  ← 用户态传入的指针
│           │       idx++
│           │
│           └── 如果不可读了（数据已被别的 reader 消费？）：
│                   从就绪链表中移除，但不从红黑树删除
│
└── 返回 idx（就绪的 fd 数量）
```

#### Step 4-3: 回到用户空间

```c
// smart_monitor.c:665 — 进程从这里醒来
int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
// nfds = 1 ← 有一个 fd 就绪了

// smart_monitor.c:676-678
for (int i = 0; i < nfds; i++) {
    int *fd_ptr = (int *)events[i].data.ptr;  // 解出指针
    int fd      = *fd_ptr;                    // fd = g_device_fds[4] = uart_fd

    // smart_monitor.c:696-700
    if (fd == g_device_fds[4]) {
        handle_uart_rx(fd);   // ← 读数据 + 解析命令
    }
}
```

#### Step 4-4: 消费数据

```c
// smart_monitor.c:568-593
static void handle_uart_rx(int fd)
{
    static char line_buf[CMD_BUF_SIZE];
    static int line_pos = 0;
    char chunk[128];
    int n;

    // read() → 系统调用 → 驱动 uart_sensor_read()
    n = read(fd, chunk, sizeof(chunk) - 1);
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        char c = chunk[i];
        if (c == '\n' || c == '\r') {           // 遇到换行
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                handle_uart_command(fd, line_buf);  // 解析完整命令行
                line_pos = 0;
            }
        } else {
            if (line_pos < sizeof(line_buf) - 1)
                line_buf[line_pos++] = c;       // 累积字符
        }
    }
}
```

驱动侧的 `read`：

```c
// driver/uart_sensor.c:275-298 (简化)
static ssize_t uart_sensor_read(struct file *filp, char __user *buf,
                                size_t count, loff_t *ppos)
{
    struct uart_sensor_dev *dev = filp->private_data;

    if (filp->f_flags & O_NONBLOCK) {
        // 非阻塞：没数据直接返回 -EAGAIN
        if (kfifo_is_empty(&dev->rx_fifo))
            return -EAGAIN;
    } else {
        // 阻塞：没数据就在这里睡
        wait_event_interruptible(dev->rx_wq, !kfifo_is_empty(&dev->rx_fifo));
    }

    // 从 kfifo 拷数据到用户空间 buf
    ret = kfifo_to_user(&dev->rx_fifo, buf, count, &copied);
    return copied;
}
```

**注意**：在 epoll 模式下，`handle_uart_rx` 被调用时数据肯定在 kfifo 里（因为 epoll 已经确认可读了），所以 `read()` 不会阻塞，即时返回数据。

### 第五阶段：回到睡眠

```c
// smart_monitor.c:662 — for 循环结束，回到 while 开头
while (g_running)
{
    int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
    // ↑ 再次调用
    //
    // 如果这次 read() 后 kfifo 已被读空：
    //   epoll_wait → uart_sensor_poll → kfifo 空 → 不设置 POLLIN
    //   → 进程再次睡眠，等待下一个数据到来
    //
    // 如果 read() 后 kfifo 里还有数据（中断里一次读了多个字节）：
    //   epoll_wait → uart_sensor_poll → kfifo 非空 → 设置 POLLIN
    //   → epoll_wait 立即返回！不需要等新中断
    //   → 继续消费剩下的数据
}
```

---

## 5. comp_drv (LED) 的阻塞/唤醒链路

LED 驱动的模式与 UART 类似，但触发源不同：不是硬件中断，而是**用户的写操作或 ioctl**。

### 5.1 等待队列 + 条件变量

```c
// driver/comp_drv.c

struct comp_dev {
    // ...
    wait_queue_head_t wq;        // 等待队列头
    atomic_t changed;            // 状态变化标记（为什么不用 bool？因为 atomic 有内存屏障）
};

// 初始化（probe 中）
init_waitqueue_head(&dev->wq);
```

### 5.2 poll 函数

```c
// driver/comp_drv.c:195-208
static unsigned int comp_poll(struct file *filp, poll_table *wait)
{
    struct comp_dev *dev = filp->private_data;
    unsigned int mask = 0;

    // ① 注册回调 — 和 uart_sensor 一样的套路
    poll_wait(filp, &dev->wq, wait);

    // ② 检查条件 — LED 状态有没有被改过？
    if (atomic_read(&dev->changed))
        mask |= POLLIN | POLLRDNORM;  // 改过 → 可读

    // ③ LED 随时可写（没有"写缓冲区满"的概念）
    mask |= POLLOUT | POLLWRNORM;

    return mask;
}
```

### 5.3 唤醒函数

```c
// driver/comp_drv.c:68-73
static void comp_led_notify(struct comp_dev *dev)
{
    atomic_set(&dev->changed, 1);        // ① 设置标记（这样 poll 会返回 POLLIN）
    wake_up_interruptible(&dev->wq);     // ② 唤醒 epoll 和阻塞 read()
    kill_fasync(&dev->fasync, SIGIO, POLL_IN); // ③ 额外：向进程发 SIGIO 信号
}
```

### 5.4 唤醒的触发点（3 处）

| 触发点 | 代码位置 | 场景 |
|--------|----------|------|
| `comp_write()` | line 187 | 用户写入 "on" / "off" |
| `comp_ioctl()` START_BLINK | line 275 | 用户发送闪烁命令 |
| `comp_ioctl()` STOP_BLINK | line 285 | 用户发送停止闪烁命令 |
| `comp_timer_callback()` | line 76 | 定时器翻转 LED，通过 comp_led_notify 通知 |

### 5.5 链路时序

```
用户 A (键盘/shell)           内核 comp_drv              用户 B (smart_monitor epoll_wait)
─────────────────            ────────────               ──────────────────────────
                                                          epoll_wait() → 睡眠

echo on > /dev/comp_drv
  → write() 系统调用
                              comp_write()
                                gpiod_set_value(1)  ← 硬件：开灯
                                atomic_set(changed, 1)
                                wake_up(&dev->wq)
                                  │
                                  ├─→ ep_poll_callback()
                                  │     加入 epoll 就绪链表
                                  │     wake_up(&ep->wq)
                                  │       │
                                  │       └──────────→ smart_monitor 被唤醒
                                  │                       epoll_wait() 返回
                                  │                       handle_led_event(fd)
                                  │                         read(fd) → comp_read()
                                  │                           copy_to_user(led状态)
                                  │                           atomic_set(changed, 0)
                                  │                         打印 LED 状态到终端
                                  │
                                  └─→ kill_fasync()
                                        向注册了 FASYNC 的进程发送 SIGIO
```

---

## 6. key_input (按键) 的隐式 poll

按键驱动使用了 Linux **输入子系统**（input subsystem），自己没有实现 `.poll`，但 poll/epoll 支持由内核 `evdev.c` 自动提供。

### 6.1 驱动做了什么

```c
// driver/key_input.c — 中断处理函数
static irqreturn_t key_irq_handler(int irq, void *dev_id)
{
    struct key_input_dev *dev = dev_id;

    // 硬件消抖：用 delayed_work 延时 20ms 后再读 GPIO
    schedule_delayed_work(&dev->work, msecs_to_jiffies(20));

    return IRQ_HANDLED;
}

// 延时工作队列的回调
static void key_work_func(struct work_struct *work)
{
    // 读 GPIO 确认按键确实按下
    int val = gpiod_get_value(dev->key_gpio);

    // ★ 向上报告按键事件 ★
    input_report_key(dev->input, KEY_ENTER, !val);
    input_sync(dev->input);
    //   │
    //   └── 内部调用链：
    //       input_event() → input_handle_event() → input_pass_values()
    //       → evdev_event() → evdev_pass_values()
    //       → 写入 evdev 客户端的环形缓冲 client->buffer
    //       → wake_up_interruptible(&evdev->wait);  ← 唤醒 epoll / read
}
```

### 6.2 evdev 提供的 poll（驱动看不见）

```c
// 内核源码 drivers/input/evdev.c
static __poll_t evdev_poll(struct file *file, poll_table *wait)
{
    struct evdev_client *client = file->private_data;
    __poll_t mask = 0;

    // ① 注册到 evdev 自己的等待队列
    poll_wait(file, &client->evdev->wait, wait);

    // ② 检查客户端环形缓冲是否有数据
    if (client->packet_head != client->tail)
        mask |= EPOLLIN | EPOLLRDNORM;  // 有事件 → 可读

    return mask;
}
```

### 6.3 链路

```
KEY0 按下
  │
  ▼
GPIO 中断 → key_irq_handler()
  │
  ▼  20ms 后
key_work_func()
  ├── input_report_key(dev->input, KEY_ENTER, 1)
  │     └── evdev_event() → 写入 client->buffer
  └── input_sync(dev->input)
          └── wake_up_interruptible(&evdev->wait)
                │
                ├─→ ep_poll_callback() (epoll 的回调)
                │     → 标记 key_input fd 就绪
                │     → wake_up(&ep->wq) → 唤醒 smart_monitor
                │
                └─→ 阻塞 read() 的进程也被唤醒

smart_monitor epoll_wait 返回:
  fds[1] 可读 → handle_key_event(fd)
                  read(fd, &input_event) → 读到按键事件
                  toggle LED
```

---

## 7. 无 poll 的传感器驱动

`ap3216c.c` 和 `icm20608.c` 没有实现 `.poll`，也没有中断。它们的 `read()` 每次被调用时，**现场**发起 I2C/SPI 总线读操作拿到最新数据。

### 7.1 为什么 epoll_wait 能"工作"？

当 `epoll_ctl(ADD)` 时，内核调用驱动 `f_op->poll`：

```c
// struct file_operations 中没有 .poll → f_op->poll = NULL
//
// 内核 VFS 层的默认行为：
// 如果 file->f_op->poll == NULL，则默认返回
//   DEFAULT_POLLMASK = EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM
//
// 这意味着 epoll_ctl 时就被认为"永远可读"
// → epoll_wait 每次都会立即返回这个 fd
```

### 7.2 实际行为

```
while (g_running)
{
    epoll_wait()                            // timeout = 1000ms
      │
      │  如果没有其他事件，
      │  ap3216c 和 icm20608 始终"就绪"  → epoll_wait 立即返回（不睡眠！）
      │  handle_sensor_read() 被调用
      │    → read(fd) → 驱动发起 I2C/SPI 事务 → 拿到值 → 返回
      │
      │  但如果只有这两个 fd 就绪，会快速循环（CPU 浪费）
      │  所以项目中做了保护：
      │
      ├────────────────── 定时轮询 (line 709-718) ──────────────────
      │  time_t now = time(NULL);
      │  if (now - last_sensor_read >= g_poll_interval) {  // 默认 2 秒
      │      handle_sensor_read(g_device_fds[2]);  // ap3216c
      │      handle_sensor_read(g_device_fds[3]);  // icm20608
      │      last_sensor_read = now;
      │  }
      │  ★ 实际上传感器主要由定时轮询驱动，而不是 epoll 事件驱动
```

### 7.3 实际项目的两套读取机制

对于传感器，`epoll_wait` 返回 + 定时轮询**同时**起作用：

| 机制 | 代码位置 | 触发条件 |
|------|----------|----------|
| epoll 事件 | `run_event_loop`:691-694 | 只要 ap3216c/icm20608 的 epoll 认为"可读"（永远可读，所以只要有其他事件顺便带着） |
| 定时轮询 | `run_event_loop`:709-718 | 每 2 秒无条件读一次（可配 `--interval`） |

---

## 8. 阻塞 read 与 poll 的关系

一个驱动通常同时支持两种读取方式：

### 8.1 阻塞 read() 方式（直接睡眠在驱动 wq 上）

```c
// 用户态
char buf[256];
int n = read(uart_fd, buf, sizeof(buf));  // 没数据就阻塞
```

```c
// 驱动态 uart_sensor_read()
if (filp->f_flags & O_NONBLOCK) {
    if (kfifo_is_empty(&dev->rx_fifo))
        return -EAGAIN;         // 非阻塞：直接返回
} else {
    // 阻塞模式：没数据就睡在 dev->rx_wq 上
    ret = wait_event_interruptible(dev->rx_wq,
                                   !kfifo_is_empty(&dev->rx_fifo));
    //        │
    //        └── 内部做了三件事：
    //           ① 创建等待条目，.func = autoremove_wake_function
    //           ② add_wait_queue(&dev->rx_wq, &wait)
    //           ③ set_current_state(TASK_INTERRUPTIBLE)
    //           ④ schedule() ← 睡眠
    //           ⑤ 被 wake_up 唤醒后：从 wq 摘下条目，恢复 TASK_RUNNING
    //           ⑥ 再次检查条件，满足则继续
}
kfifo_to_user(&dev->rx_fifo, buf, count, &copied);
return copied;
```

### 8.2 epoll 方式（通过中间人）

```
read() 阻塞:                epoll 方式:
────────────                ─────────

进程 ──直接挂到──► rx_wq   进程 ──挂在──► ep->wq
                           epoll 回调 ──挂在──► rx_wq

wake_up(rx_wq)             wake_up(rx_wq)
  ├─ 唤醒进程 (read)         ├─ 触发 ep_poll_callback
  └─ 触发 ep_poll_callback     │   ├─ 标记 fd 就绪
                               │   └─ wake_up(ep->wq)
                               │        └─ 唤醒进程
                               │
                            epoll_wait 返回
                            read(fd) → 由于数据在 kfifo 里，不会阻塞
```

**关键区别**：
- 阻塞 read：**一个 fd 等它自己的数据**，进程睡在驱动的 wq 上
- epoll：**一个进程等多个 fd**，进程睡在 epoll 的 wq 上，每个 fd 通过回调间接通知 epoll

### 8.3 本项目实际使用

```c
// smart_monitor 里 uart_fd 以 O_RDWR 打开
g_device_fds[4] = open(DEV_UART_SENSOR, O_RDWR);  // 没有 O_NONBLOCK
// 但 read() 只在 epoll 确认可读时才调用 → 实际不会阻塞

// 按键 fd 以 O_NONBLOCK 打开
g_device_fds[1] = open(DEV_KEY_INPUT, O_RDONLY | O_NONBLOCK);
// 防止没有按键事件时 read() 阻塞
```

---

## 9. 常见误解澄清

### ❌ 误解 1："poll_wait 会让进程睡眠"

**错误。** `poll_wait()` 只是一个**注册**操作，不涉及任何睡眠。它把回调函数挂到驱动等待队列上就返回了。真正睡眠发生在 `epoll_wait` → `schedule()`。

### ❌ 误解 2："epoll_wait 中直接调用了驱动的 poll"

**部分正确。** 有两个时机：

| 时机 | 调用者 | 目的 |
|------|--------|------|
| `epoll_ctl(ADD)` | `ep_insert()` → `ep_item_poll()` | ① 通过 `poll_wait` 注册回调 ② 检查当前是否已就绪 |
| `epoll_wait` 返回前 | `ep_send_events()` → `ep_item_poll()` | 再次确认 fd 确实可读（防止虚假唤醒） |

### ❌ 误解 3："wake_up 直接唤醒进程"

**不完全是。** `wake_up(&dev->rx_wq)` 遍历 `rx_wq` 上的所有等待条目，对每条调用其 `.func`。如果条目是 epoll 挂的，`.func` = `ep_poll_callback`，这个回调再去唤 epoll 的 wq。如果条目是 `wait_event_interruptible` 挂的，`.func` = `autoremove_wake_function`，这个才直接唤醒进程。

### ❌ 误解 4："驱动需要知道 epoll 的存在"

**不需要。** 驱动只需要：
1. 提供 `.poll` 函数，里面调用 `poll_wait()` + 返回正确的 mask
2. 在数据就绪时调用 `wake_up()`

驱动不知道、也不需要知道调用者用的是 epoll、poll、还是 select。所有三种多路复用机制都通过 `poll_wait` 注册回调，对驱动完全透明。

### ❌ 误解 5："没有 .poll 的 fd 不能被 epoll 监听"

**可以被监听**，但内核会给默认行为：永远返回 `EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM`。这意味着 `epoll_wait` 会**立即返回**（不阻塞），因为没有等待队列可以注册回调。这就是 `ap3216c` 和 `icm20608` 的行为。

---

## 10. 附录：完整时序图

### 10.1 UART 接收一个字符的完整时间线

```
层             时间轴
──             ──────────────────────────────────────────────────────────►

硬件           [RXD 电平跳变] → [URXD 锁存] → [USR2.RDR=1] → [GIC 发 IRQ]

中断框架        保存上下文 → GIC 识别 → handle_level_irq → handle_irq_event

驱动 ISR      [确认 RDR] → [readb(URXD)] → [kfifo_in()] → [wake_up(rx_wq)]
                                                             │
                                                             ├─ 遍历 rx_wq
                                                             │
epoll 层                                                    │
                                                             ├─ ep_poll_callback
                                                             │   ├─ list_add(rdllist)
                                                             │   └─ wake_up(ep->wq)
                                                             │
                                                             ├─ autoremove_wake_function
                                                             │   └─ try_to_wake_up(进程2)
                                                             │       (如果另有进程在阻塞 read)
                                                            ...
调度器                                                         [标记 TASK_RUNNING]
                                                            ... [下次调度] ...
                                                             │
用户态                                                        ▼
                                            smart_monitor 从 epoll_wait 返回
                                            ├─ events[0].data.ptr → fd
                                            ├─ fd == g_device_fds[4] → handle_uart_rx()
                                            ├─ read(fd, chunk, 128)
                                            │    └─ uart_sensor_read()
                                            │         └─ kfifo_to_user() → copy_to_user()
                                            ├─ 解析命令: handle_uart_command()
                                            └─ 回到 while → 再次 epoll_wait
                                                                  │
                                                                  ├─ kfifo 还有数据？
                                                                  │   YES → 立即返回，继续读
                                                                  │   NO  → schedule() 睡眠
```

### 10.2 LED 状态变化的完整时间线

```
层             时间轴
──             ──────────────────────────────────────────────────────────►

终端 A         echo on > /dev/comp_drv
               │
               └── write() 系统调用

驱动           comp_write()
               ├── copy_from_user() → "on"
               ├── 判断命令
               ├── gpiod_set_value(1)         (硬件：LED 亮)
               ├── dev->write_count++
               └── comp_led_notify(dev)
                     ├── atomic_set(changed, 1)
                     └── wake_up_interruptible(&dev->wq)

epoll 层         └── ep_poll_callback()
                       ├── 标记 fd[0] 就绪
                       └── wake_up(ep->wq)

用户态 B                                        smart_monitor 从 epoll_wait 返回
                                                ├─ fd == g_device_fds[0] → handle_led_event()
                                                ├─ read(fd)
                                                │    └─ comp_read()
                                                │         ├─ atomic_read(changed) == 1
                                                │         ├─ 填 status 结构体 (含 LED 状态)
                                                │         ├─ copy_to_user()
                                                │         └─ atomic_set(changed, 0) ← 消费标记
                                                └─ 打印: "LED: state=1 ..."
```

### 10.3 三个驱动同时有事件时的处理

```
                          epoll_wait(epfd) 阻塞中
                                  │
         ┌────────────────────────┼────────────────────────┐
         │                        │                        │
    UART RX 中断              KEY0 按下              定时器 (2s)
    收到 "STATUS\r\n"       GPIO 中断触发             传感器轮询时间到
         │                        │                        │
         ▼                        ▼                        │
    uart_rx_interrupt       key_work_func                  │
    ├─ kfifo_in             ├─ input_report_key             │
    └─ wake_up(rx_wq)       └─ wake_up(evdev->wait)        │
         │                        │                        │
         ├────────────────────────┼────────────────────────┘
         │            ep_poll_callback 被调用 3 次
         │            就绪链表: [uart_fd, key_fd, ap3216c_fd, icm20608_fd]
         │                  │
         │                  ▼
         │            wake_up(ep->wq)
         │                  │
         │                  ▼
         └──────► epoll_wait() 返回 nfds = 3 (或更多)
                  │
                  ├── events[0] → uart_fd → handle_uart_rx() → cmd_status()
                  ├── events[1] → key_fd  → handle_key_event() → toggle LED
                  ├── events[2] → ap3216c → handle_sensor_read() → 打印 IR/ALS/PS
                  └── events[3] → icm20608 → handle_sensor_read() → 打印 accel/gyro
```

### 10.4 本项目中 6 个 fd 的 poll 特性汇总

| idx | 设备 | 驱动 | .poll 实现 | 等待队列 | 唤醒源 | 默认行为 |
|-----|------|------|-----------|---------|--------|---------|
| 0 | comp_drv | comp_drv.c | `comp_poll` | `dev->wq` | `comp_led_notify()` | 可读需等待 changed=1 |
| 1 | key_input | evdev.c (内核) | `evdev_poll` | `evdev->wait` | `input_report_key()` | 可读需等待事件 |
| 2 | ap3216c | ap3216c.c | 无 (NULL) | 无 | 无 | 永远可读 |
| 3 | icm20608 | icm20608.c | 无 (NULL) | 无 | 无 | 永远可读 |
| 4 | uart_sensor | uart_sensor.c | `uart_sensor_poll` | `dev->rx_wq` | `uart_rx_interrupt()` | 可读需等待 kfifo 非空 |
| 5 | stdin | tty (内核) | `tty_poll` | `tty->wq` | 键盘中断 → `tty_flip_buffer_push()` | 可读需等待输入 |

---

## 附录 A：相关文件清单

| 文件 | 角色 |
|------|------|
| `app/smart_monitor.c` | 用户态 epoll 事件循环 |
| `driver/uart_sensor.c` | 有显式 `.poll` 的 UART 驱动 |
| `driver/comp_drv.c` | 有显式 `.poll` 的 LED 驱动 |
| `driver/key_input.c` | 无 `.poll`，由 evdev 提供 |
| `driver/ap3216c.c` | 无 `.poll`，默认永远可读 |
| `driver/icm20608.c` | 无 `.poll`，默认永远可读 |
| `fs/eventpoll.c` | 内核 epoll 实现（非本仓库） |
| `drivers/input/evdev.c` | 内核 evdev poll 实现（非本仓库） |
| `kernel/sched/wait.c` | 内核 wait_queue 实现（非本仓库） |

---

## 附录 B：关键内核 API 速查

| API | 作用 | 调用上下文 |
|-----|------|-----------|
| `init_waitqueue_head(wq)` | 初始化等待队列头 | probe |
| `poll_wait(file, wq, poll_table)` | 注册回调到驱动 wq | `.poll` 函数中 |
| `wake_up(wq)` | 唤醒所有等待者 | 进程上下文 |
| `wake_up_interruptible(wq)` | 唤醒可中断睡眠的等待者 | 中断上下文（常用） |
| `wait_event_interruptible(wq, cond)` | 等待条件满足 | 进程上下文（read 中） |
| `kfifo_is_empty(fifo)` | 检查环形缓冲是否为空 | 任意 |
| `kfifo_in(fifo, data, n)` | 写入环形缓冲 | 中断上下文（安全） |
| `kfifo_to_user(fifo, buf, n, &copied)` | 从环形缓冲拷到用户空间 | 进程上下文 |
| `atomic_read(atm)` | 读取原子变量 | 任意 |
| `atomic_set(atm, val)` | 设置原子变量 | 任意 |
