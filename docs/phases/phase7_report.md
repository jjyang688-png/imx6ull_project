# 阶段 7 学习总结（2026-06-04）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `driver/uart_sensor.c`（新增 577 行） | UART3 硬件寄存器驱动：ioremap + 中断 + kfifo + 阻塞/非阻塞 + poll |
| `dts/imx6ull-smart-monitor.dtsi`（更新） | 新增 `uart_sensor_dev` 节点；`&uart3` 设为 disabled；删除 sys_monitor_dev |
| `docs/REQUIREMENTS.md`（更新） | UART 控制台设计（第 5 节）；删除 F-K06 misc 驱动 |
| `CLAUDE.md`（更新） | 驱动 6→5 个；阶段 7 描述更新 |
| `driver/Makefile`（更新） | `obj-m` 加入 `uart_sensor.o` |

## 新接触的知识点

### 一、i.MX6ULL UART 硬件寄存器（Ch63）

| 知识点 | 说明 |
|------|------|
| **寄存器基地址** | UART3: `0x021EC000`，长度 `0x4000`。从设备树 `reg` 属性读取 |
| **URXD (0x00)** | 接收数据寄存器（只读）。bit[7:0]=数据字节，bit[15:8]=错误标志（framing/parity） |
| **UTXD (0x40)** | 发送数据寄存器（只写）。写入的数据自动进入 TX FIFO 移位输出 |
| **UCR1 (0x80)** | 控制寄存器 1。bit0=UARTEN（UART 总开关） |
| **UCR2 (0x84)** | 控制寄存器 2。bit0=SRST（软件复位），bit1=RXEN，bit2=TXEN，bit8=WS（字长） |
| **UCR3 (0x88)** | 控制寄存器 3。bit2=RXDMUXSEL（DCE/DTE 选择） |
| **UCR4 (0x8C)** | 控制寄存器 4。bit0=DREN（RX 数据就绪中断使能） |
| **UFCR (0x90)** | FIFO 控制寄存器。bit[13:10]=TXTL（TX 触发阈值），bit[5:0]=RXTL（RX 触发阈值） |
| **USR1 (0x94)** | 状态寄存器 1。bit0=RRDY（RX 有数据），bit13=TRDY（TX 有空位） |
| **USR2 (0x98)** | 状态寄存器 2。bit0=RDR（至少 1 字节在 FIFO 中） |
| **UBIR/UBMR** | 波特率整数/小数寄存器。公式：Baud = RefClk / (16 × 分频系数) |

### 二、硬件寄存器访问 API（ioremap / readl / writel）

| 知识点 | 说明 |
|------|------|
| **`devm_ioremap_resource()`** | 物理地址 → 内核虚拟地址映射（替代物理地址直接访问）。返回 `void __iomem *`，用 read/write 宏操作 |
| **`readb(addr)`** | 读 8 位（byte），用于 URXD |
| **`writeb(val, addr)`** | 写 8 位（byte），用于 UTXD |
| **`readl(addr)`** | 读 32 位（long），用于状态/控制寄存器 |
| **`writel(val, addr)`** | 写 32 位，用于配置控制寄存器 |
| **`cpu_relax()`** | 忙等待时提示 CPU 低功耗（替代空循环） |
| **`devm_request_irq()`** | devm 版中断注册，设备 remove 时自动释放 |

### 三、UART 硬件初始化顺序

```
① 软件复位（UCR2_SRST）→ 等硬件清零
② 帧格式（UCR2）：RXEN | TXEN | IRTS
③ 数据通路（UCR3）：RXDMUXSEL = 1（DCE 模式）
④ FIFO 阈值（UFCR）：TXTL=0, RXTL=1
⑤ 波特率（UBIR/UBMR）：115200 = RefClk/(16×分频)
⑥ 总使能（UCR1_UARTEN + UCR4_DREN）：最后一步开
```

**顺序不能乱**：先配参数再开总开关，防止用错误参数工作。

### 四、kfifo 环形缓冲（★ 本阶段核心新增）

| 知识点 | 说明 |
|------|------|
| **`DECLARE_KFIFO(name, type, size)`** | 在结构体中声明 kfifo。size 必须是 2 的幂（4096 = 4KB） |
| **`INIT_KFIFO(fifo)`** | 使用前必须初始化（在 probe 中） |
| **`kfifo_in(fifo, buf, n)`** | 写入数据到 FIFO。可在中断上下文使用（无锁） |
| **`kfifo_to_user(fifo, ubuf, n, &copied)`** | 从 FIFO 取数据 + `copy_to_user` 一次完成 |
| **`kfifo_is_empty(fifo)`** | 判断 FIFO 是否为空 |
| **vs 链表/数组** | kfifo 天然支持环形覆盖；单生产者单消费者无锁；专为内核设计 |

### 五、中断上下文编程约束

| 约束 | 原因 |
|------|------|
| 不能 `kmalloc`（GFP_KERNEL）| 可能睡眠，中断中不可调度 |
| 不能 `mutex_lock` | mutex 会睡眠，中断必须用 spinlock |
| 不能 `copy_to_user` | 可能引发缺页异常和调度 |
| 只能 `kfifo_in` + `wake_up` | 两者都是安全的（无睡眠） |

**设计原则**：中断只做最快的硬件读取 + 数据暂存，所有慢操作留给用户进程的 read()。

### 六、阻塞 vs 非阻塞 IO

| 模式 | 打开标志 | kfifo 为空时 | 使用的机制 |
|------|------|------|------|
| 阻塞 | `O_RDONLY`（默认） | 睡眠等待，RX 中断来后唤醒 | `wait_event_interruptible()` |
| 非阻塞 | `O_RDONLY \| O_NONBLOCK` | 立即返回 `-EAGAIN` | `kfifo_is_empty()` 判断 |

**完整阻塞流程**：read() → kfifo 空 → `wait_event_interruptible(rx_wq, !empty)` → 睡眠 → RX 中断 → `kfifo_in` → `wake_up` → 醒来 → `kfifo_to_user` → 返回数据

### 七、TX 发送机制

```
用户 write("hello")
  → copy_from_user() → 拿到内核拷贝
  → for 每个字节：
      while (!(USR1 & TRDY)) cpu_relax();  // 等 TX FIFO 有空位
      writeb(byte, UTXD);                   // 写入发送寄存器
  → 硬件自动移位输出到 TX 引脚
```

关键点：write 是在进程上下文调用的（不是中断），可以安全地做 `kmalloc`、`copy_from_user`。

### 八、时钟管理（Ch63 补充）

| 知识点 | 说明 |
|------|------|
| **ipg 时钟** | 外设门控时钟（IPG_CLK），控制 UART 模块的通断 |
| **per 时钟** | 外设功能时钟（PER_CLK = 80MHz），决定波特率 |
| **`devm_clk_get(dev, "name")`** | 从设备树 `clock-names` 获取时钟 |
| **`clk_prepare_enable()`** | 准备 + 使能时钟（两步合并） |
| **`clk_disable_unprepare()`** | 禁止 + 取消准备（逆序释放） |

### 九、平台驱动框架对比

| 维度 | uart_sensor | comp_drv | key_input |
|------|------|------|------|
| 总线 | platform | platform | platform |
| 硬件访问 | **ioremap + readl/writel** | gpiod API | gpiod API |
| 中断 | **半独立（直接注册）** | — | devm_request_irq |
| CDEV | 手动三步 | 手动三步 | 不用（input 子系统） |
| 缓冲 | **kfifo 环形缓冲** | — | — |
| TX | **writeb(UTXD)** | gpiod API | — |

### 十、UART 系统控制台架构

```
PC 串口助手 ──RS232──► UART3 RX ──► 中断 → kfifo → read() → 用户解析
PC 串口助手 ◄──RS232── UART3 TX ◄── write() ← 格式化响应 ← 用户生成
```

命令解析（STATUS/SENSOR/LED/HELP/RESET）在用户空间（Phase 8），不在驱动中。原因：驱动管硬件、应用管逻辑；跨驱动数据访问在内核中极其复杂。

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| RXDMUXSEL 忘设 → RX 永远收不到数据 | i.MX6ULL 的 RX 有 DCE/DTE 两条数据通路，pinctrl 用 DCE_RX，UCR3 必须选 DCE(=1) | 在 `uart_hw_init` 中显式写 `writel(RXDMUXSEL, UCR3)` |
| RXTL 设大 → 短命令不触发中断 | RXTL=8 时 FIFO 要攒够 8 字节才中断，用户发 "STATUS\n"（7字节）永远卡在 FIFO | 设 `RXTL=1`，每字节都中断 |
| `DECLARE_KFIFO` 不能用 `devm_kzalloc` 创建 | `DECLARE_KFIFO` 是结构体内联宏，编译时确定大小，不是动态分配 | 直接在结构体中使用 `DECLARE_KFIFO`，probe 中用 `INIT_KFIFO` 初始化 |
| 注释断行导致乱码 | 手动复制时中文注释被截断换行 | 保持注释在同一个逻辑段落内 |

## 代码规范要点

- UART 寄存器偏移使用 `#define` 宏，带手册注释
- 中断处理函数只做 `readb + kfifo_in + wake_up`，不分配内存不加锁
- `uart_hw_init` 按 ①→⑥ 严格顺序，每一步有中文注释说明作用
- probe 使用标准的 goto 错误清理链
- remove 与 probe 严格逆序：先关硬件 → 删 CDEV → 关时钟
- fops 中没有实现 ioctl，UART 控制台走纯 read/write 接口

## 下一阶段预告

**阶段 8：统一应用 + 集成测试** — 用户空间守护进程 `smart_monitor`：epoll 监听全部 5 个设备文件；实现 UART 命令解析引擎（STATUS/SENSOR/LED/HELP/RESET）；实时显示传感器数据；CSV 日志记录；集成测试脚本 `test_all.sh`。
