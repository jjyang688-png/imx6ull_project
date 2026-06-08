# 阶段 8 学习总结（2026-06-08）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `app/smart_monitor.c`（新增 800 行） | 统一监控守护进程：epoll 多路复用 + UART 命令引擎 + 传感器采集 + CSV 日志 |
| `app/Makefile`（更新） | 加入 `smart_monitor` 编译目标 |
| `driver/icm20608.h` | 用户空间复用（`struct icm20608_full`、ioctl 命令码） |
| `driver/comp_drv.h` | 用户空间复用（`struct comp_drv_status`、`enum led_state`） |

## 新接触的知识点

### 一、epoll — Linux 最高效的 IO 多路复用

| 知识点 | 说明 |
|------|------|
| **`epoll_create1(0)`** | 创建 epoll 实例，返回 epoll fd。参数 `0`=无特殊标志，`EPOLL_CLOEXEC`=fork+exec 时自动关闭 |
| **`epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev)`** | 将 fd 注册到 epoll 监听列表。第三个参数还可以是 `EPOLL_CTL_MOD`（修改）、`EPOLL_CTL_DEL`（移除） |
| **`epoll_wait(epfd, events, max, timeout)`** | 阻塞等待事件。`timeout=-1`=永久等，`timeout=0`=立即返回，`timeout>0`=最多等 N 毫秒 |
| **`struct epoll_event`** | `events`=监听的事件类型（`EPOLLIN` 可读、`EPOLLOUT` 可写、`EPOLLERR` 错误）；`data`=用户自定义数据（本项目传 `&fd` 指针用于分发） |
| **epoll vs poll vs select** | select：fd 数受限（FD_SETSIZE=1024），每次调用需全量拷贝 fd_set；poll：无 fd 上限，但仍需全量遍历；epoll：内核维护红黑树+就绪链表，事件驱动，O(1) 复杂度 |

**epoll 核心原理：**

```
epoll_ctl(ADD, fd)  →  内核将 fd 插入红黑树 + 注册回调
                           │
epoll_wait()           内核只遍历"就绪链表"（有事件的 fd）
                           │  不需要像 poll/select 那样遍历全部 fd
返回事件数组              │
                           │
用户处理完后继续 epoll_wait（fd 仍在红黑树中，不用重新注册）
```

### 二、epoll 事件分发模式

本项目用 `ev.data.ptr = &g_device_fds[i]` 传递 fd 地址，主循环中通过解引用拿到 fd 后与设备数组比对完成分发：

```
epoll_wait → events[i].data.ptr → *(int*)ptr = fd
  ├─ fd == fd[0] → handle_led_event()
  ├─ fd == fd[1] → handle_key_event()
  ├─ fd == fd[2] → handle_sensor_read()
  ├─ fd == fd[3] → handle_sensor_read()
  ├─ fd == fd[4] → handle_uart_rx()     ★ 最核心
  └─ fd == fd[5] → handle_stdin()
```

为什么不把函数指针直接放进 `data.ptr`？因为 `epoll_event.data` 是 union，存函数指针也可以，但存 fd 更灵活——可以根据运行时状态做额外判断。

### 三、UART 串口字节流 → 行缓冲拼包

| 知识点 | 说明 |
|------|------|
| **核心问题** | 串口是字节流，一次 `read()` 可能只拿到半行命令（"STA"），下次拿到剩余部分（"TUS\r\n"） |
| **解决方案** | 行缓冲（line buffer）：用 `static` 变量 `line_buf[]` + `line_pos` 跨 epoll 事件累积字节，遇 `\n` 或 `\r` 触发解析 |
| **static 变量安全性** | 用户态单线程程序，无竞争条件，不需要加锁 |
| **防御性编程** | `line_pos < sizeof(buf)-1` 防止缓冲区溢出；连续 `\r\n` 忽略空行 |

**行缓冲工作流程：**

```
read1: "STA"        → line_buf="STA",      pos=3, 无\n → 继续
read2: "TUS\r\n"    → line_buf="STATUS\r", pos=7, 遇\r → 触发 handle_uart_command("STATUS"), pos=0
read3: "\n"         → pos=0, 空行忽略
```

### 四、命令表（查表法）vs if-else 链

```c
/* 查表法 —— O(n) 但 n=5，可读性远胜 if-else */
static struct uart_command uart_commands[] = {
    {"STATUS", cmd_status, "..."},
    {"SENSOR", cmd_sensor, "..."},
    {"LED",    cmd_led,    "..."},
    {"HELP",   cmd_help,   "..."},
    {"RESET",  cmd_reset,  "..."},
    {NULL, NULL, NULL}   // 哨兵
};

for (int i = 0; uart_commands[i].name; i++)
    if (strcasecmp(cmd, uart_commands[i].name) == 0) {
        uart_commands[i].handler(uart_fd, args);
        return;
    }
```

**优势**：加新命令只需加一行结构体 + 一个处理函数；表格式一目了然；`strcasecmp` 大小写不敏感。

### 五、input 子系统用户空间读取

| 知识点 | 说明 |
|------|------|
| **`struct input_event`** | 内核 input 子系统上报的标准格式：`time`（时间戳）+ `type`（事件类型）+ `code`（键码/轴码）+ `value`（值） |
| **`EV_KEY`** | 按键事件类型，`value=1` 按下、`value=0` 释放、`value=2` 长按 |
| **`KEY_ENTER=28`** | 本项目 KEY0 的默认键码（来自 `key_input.c` 的 `default_keycode`） |
| **事件过滤** | 必须同时过滤 `type==EV_KEY`、`value==1`（只响应按下）、`code==28`（只响应 KEY0），否则一次物理按键触发两次 toggle 或响应了其他设备的事件 |
| **`/dev/input/event0`** | 第一个 input 设备节点，用 `O_RDONLY \| O_NONBLOCK` 打开，没按键时 `read()` 返回 -1（EAGAIN） |

### 六、CSV 日志持久化

| 知识点 | 说明 |
|------|------|
| **格式** | `时间,事件,详情` — 一行一条记录 |
| **`fopen(f, "a")`** | 追加模式，不会覆盖历史日志 |
| **`fflush(fp)`** | 每条记录立即刷盘——嵌入式设备可能突然断电，不刷盘会丢失缓冲区中的数据 |
| **`localtime()`** | 将 `time_t` 转为本地时间的 `struct tm`，`tm_year+1900` 得到真实年份 |

### 七、信号处理实现优雅退出

```c
signal(SIGINT,  signal_handler);   // Ctrl+C
signal(SIGTERM, signal_handler);   // kill 命令

static void signal_handler(int signo) {
    g_running = 0;   // 只置标志位，不在信号处理中做重操作
}
```

| 原则 | 说明 |
|------|------|
| 信号处理函数只置标志位 | 不在信号处理中调用 `printf`、`free`、`close`（不是信号安全的） |
| 主循环检查标志位 | `while(g_running)` — 下次循环判断时自然退出 |
| `epoll_wait` 被信号打断 | 返回 -1，`errno = EINTR`，break 退出 |

### 八、传感器数据读取模式对比

| 传感器 | 读取方式 | 数据格式 | 温度公式 |
|------|------|------|------|
| AP3216C | `read(fd, &data, 6)` | 3×u16（ir, als, ps） | — |
| ICM20608 | `read(fd, &data, 14)` | 7×s16（acc×3, temp, gyro×3） | `°C = raw/340.0 + 36.53` |

**为什么要定时轮询传感器？** AP3216C 和 ICM20608 的 read() 是"被动"的——驱动不主动 push 数据，只有用户 read 时才触发 I2C/SPI 读取。epoll 没法监听这种"无事件"的 fd，所以用 `epoll_wait` 超时 + 定时器配合，每秒强制读一次。

### 九、架构总览：用户态分层

```
┌─────────────────────────────────────────┐
│  main()                                 │
│  ├─ open_all_devices()   设备层         │
│  ├─ setup_epoll()        事件层         │
│  └─ run_event_loop()    调度层          │
│       ├─ handle_uart_rx()    → 传输层   │
│       │    └─ handle_uart_command()     │
│       │         ├─ cmd_status()  业务层 │
│       │         ├─ cmd_sensor()         │
│       │         ├─ cmd_led()            │
│       │         ├─ cmd_help()           │
│       │         └─ cmd_reset()          │
│       ├─ handle_key_event()   业务层    │
│       ├─ handle_sensor_read() 业务层    │
│       └─ handle_stdin()       交互层    │
└─────────────────────────────────────────┘
```

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `cmd_led` 写大写 "ON" → LED 不响应 | `comp_drv` 的 write 只认小写 `strncmp(kbuf, "on", 2)` | 改为 `led_cmd = "on"` |
| 按键按一次 LED 闪一下就灭 | 没过滤 `value=0`（释放事件），按下→开→释放→关，看起来像没反应 | 只处理 `ev.value == 1` |
| `cmd_table[i]` 编译报错 | 变量名不一致——定义了 `uart_commands[]` 却引用 `cmd_table` | 统一为 `uart_commands` |
| `va_list` 编译报错 | 缺少 `<stdarg.h>` | 在文件头部添加 `#include <stdarg.h>` |
| `epoll_wait` 超时设为 -1，传感器数据不刷新 | AP3216C/ICM20608 不会主动产生 epoll 事件 | 改超时为 1000ms，配合定时轮询 |
| 代码重复：LED 处理在主循环中内联 | 不公平——其他 fd 都有独立处理函数 | 提取为 `handle_led_event()` |

## 代码规范要点

- 6 个 fd 事件统一为 `handle_xxx(fd)` 风格，主循环只做分发
- 命令解析用查表法，不用 if-else 链
- 传感器每次独立 open/close，不长期占用
- CSV 每行 `fflush`，防掉电丢数据
- 信号处理只置标志位，不在 handler 中做复杂操作
- `static` 变量用于行缓冲和 toggle 状态，用户态单线程安全
- 所有设备路径用 `#define` 统一管理

## 下一阶段预告

**阶段 9：文档交付 + 发布** — 完善 `ARCHITECTURE.md` / `API_REFERENCE.md` / `LESSONS_LEARNED.md`；集成测试脚本 `test_all.sh`；`v1.0.0` 版本发布。
