#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/interrupt.h>    /* ← request_irq，key_input 也用过的 */
#include <linux/kfifo.h>        /* ← ★ 本阶段新增：环形缓冲区 */
#include <linux/io.h>           /* ← ★ 新增：ioremap / readb / writeb */
#include <linux/clk.h>          /* ← ★ 新增：时钟管理 */
#include <linux/delay.h>        /* ← udelay，TX 等待用 */
#include <linux/jiffies.h>

#define UART_SENSOR_NAME  "uart_sensor"
#define DEVICE_COUNT      1
#define RX_FIFO_SIZE      4096      /* kfifo 大小，必须是 2 的幂 */


/*
* i.MX6ULL UART 寄存器偏移（相对基地址）
*
* 注意：这些寄存器地址是 i.MX6ULL 芯片手册定义的，
* 不随 Linux 版本变化——硬件就是硬件。
*/
#define URXD    0x00    /* 接收数据寄存器（读）        */
#define UTXD    0x40    /* 发送数据寄存器（写）        */
#define UCR1    0x80    /* 控制寄存器 1               */
#define UCR2    0x84    /* 控制寄存器 2               */
#define UCR3    0x88    /* 控制寄存器 3               */
#define UCR4    0x8C    /* 控制寄存器 4               */
#define UFCR    0x90    /* FIFO 控制寄存器             */
#define USR1    0x94    /* 状态寄存器 1               */
#define USR2    0x98    /* 状态寄存器 2               */
#define UTS     0xB4    /* 测试寄存器                  */
#define UBIR    0xA4    /* 波特率整数部分              */
#define UBMR    0xA8    /* 波特率小数部分              */


/* UCR1 位定义 */
#define UCR1_UARTEN     (1 << 0)    /* UART 使能             */

/* UCR2 位定义 */
#define UCR2_SRST       (1 << 0)    /* 软件复位              */
#define UCR2_RXEN       (1 << 1)    /* 接收使能              */
#define UCR2_TXEN       (1 << 2)    /* 发送使能              */
#define UCR2_WS         (1 << 8)    /* 字长：0=8bit, 1=7bit  */
#define UCR2_IRTS       (1 << 14)   /* 忽略 RTS（如果不用流控） */

/* UCR3 位定义 */
#define UCR3_RXDMUXSEL  (1 << 2)    /* RX 数据多路选择       */

/* UCR4 位定义 */
#define UCR4_DREN       (1 << 0)    /* RX 数据就绪中断使能    */

/* UFCR 位定义 */
#define UFCR_TXTL(x)    ((x) << 10) /* TX FIFO 触发阈值       */
#define UFCR_RXTL(x)    ((x) << 0)  /* RX FIFO 触发阈值       */

/* USR1 位定义 */
#define USR1_RRDY       (1 << 0)    /* RX 数据就绪（接收 FIFO 非空） */
#define USR1_TRDY       (1 << 13)   /* TX 数据就绪（发送 FIFO 非满） */

/* USR2 位定义 */
#define USR2_RDR        (1 << 0)    /* RX 数据就绪（至少 1 个数据在FIFO） */


struct uart_sensor_dev {
    /* ===== 硬件 ===== */
    void __iomem *base;         /* ioremap 后的虚拟地址（readb/writeb用） */
    int irq;                    /* 中断号（60） */
    struct clk *clk_ipg;        /* 外设门控时钟 */
    struct clk *clk_per;        /* 外设功能时钟 */
    int baud_rate;              /* 波特率（默认 115200） */

    /* ===== CDEV ===== */
    dev_t dev_id;
    struct cdev cdev;
    struct class *class;
    struct device *device;

    /* ===== kfifo 缓冲 ===== */
    DECLARE_KFIFO(rx_fifo, u8, RX_FIFO_SIZE);  /* ★ 环形缓冲区 */

    /* ===== IO 模型 ===== */
    wait_queue_head_t rx_wq;     /* 阻塞 read 等待队列 */
    struct mutex lock;           /* 保护 kfifo 和设备状态 */

    /* ===== 统计 ===== */
    atomic_t open_count;
    unsigned long rx_bytes;
    unsigned long tx_bytes;
};

static struct uart_sensor_dev *g_uart_dev;

/*
* uart_hw_init — 配置 UART3 硬件控制寄存器
*
* 目标：115200-8-N-1，使能 RX 中断
*
* 顺序不能乱：先复位清空 → 配帧格式 → 配 FIFO → 配波特率 → 最后使能
*/
static int uart_hw_init(struct uart_sensor_dev *dev)
{
    void __iomem *base = dev->base;
    /*
    * ① 软件复位 — 清空 TX/RX FIFO + 所有状态位
    *
    * UCR2 bit0 = SRST：写 1 触发复位，硬件完成复位后自动变 0。
    * while 循环等硬件清零，表示复位完成。
    */
    writel(UCR2_SRST, base + UCR2);
    while (readl(base + UCR2) & UCR2_SRST)
        cpu_relax();

   /*
    * ② 配置帧格式 + 使能收发 — UCR2
    *
    * RXEN  = bit1：使能接收器
    * TXEN  = bit2：使能发送器
    * WS    = bit8=0：数据位 8 位（1 则为 7 位）
    * PREN  = bit5=0：无奇偶校验
    * STPB  = bit7=0：1 个停止位
    * IRTS  = bit14=1：忽略 RTS（无硬件流控）
    *
    * 默认值写入即可，只需显式置 RXEN | TXEN | IRTS。
    */
    writel(UCR2_RXEN | UCR2_TXEN | UCR2_IRTS, base + UCR2);

    /*
    * ③ 数据通路选择 — UCR3
    *
    * RXDMUXSEL = bit2=1
    *
    * ★ 关键：i.MX6ULL 的 RX 有两个数据源，DCE 模式和 DTE 模式。
    *   设备树 pinctrl 用的是 DCE_RX，所以这里必须选 DCE（=1）。
    *   忘了这步 → RX 永远收不到数据，中断永远不触发。
    */
    writel(UCR3_RXDMUXSEL, base + UCR3);

    /*
    * ④ FIFO 触发阈值 — UFCR
    *
    * TXTL=0  →  TX FIFO 空了立刻开始发送
    * RXTL=1  →  RX FIFO 收到 1 个字节就触发中断
    *
    * RXTL 设大了的问题：用户发了 3 字符命令，FIFO 不够阈值，不中断，
    * 数据永远卡在硬件里，用户 read() 永远等不到。
    */
    writel(UFCR_TXTL(0) | UFCR_RXTL(1), base + UFCR);

    /*
    * ⑤ 波特率 — UBIR / UBMR
    *
    * 公式：Baud = RefClk / (16 × 分频系数)
    * RefClk = clk_per = 80MHz
    *
    * 查表（芯片手册附录）：
    *   115200 → UBIR=4, UBMR=3124
    */
    writel(4,    base + UBIR);
    writel(3124, base + UBMR);

    /*
    * ⑥ 总开关 + RX 中断使能
    *
    * UCR1 bit0  = UARTEN：UART 模块使能
    * UCR4 bit0  = DREN：RX 数据就绪中断
    *
    * 所有参数配好后最后一步才开——防止用错误的参数工作。
    */
    writel(UCR1_UARTEN, base + UCR1);
    writel(UCR4_DREN,   base + UCR4);

    return 0;
}

/*
   * uart_rx_interrupt — RX 接收中断处理函数
   *
   * 触发条件：UART3 RX FIFO 中收到 ≥1 字节（UFCR_RXTL=1
  时触发）
   * 数据流：
   *   PC 串口助手 ──RS232──► UART3_RXD ──► 硬件 RX FIFO
   *                               │
   *                          USR2_RDR=1（触发本函数）
   *                               │
   *                          readb(URXD) 读走 1 字节
   *                               │
   *                          kfifo_in() 写入环形缓冲
   *                               │
   *                          wake_up_interruptible()
  唤醒阻塞的 read
   *
   * 设计要点：
   *   中断里不能做耗时操作（不能 kmalloc、不能
  mutex_lock、不能 copy_to_user）。
   *   kfifo_in 是无锁的，wake_up
  是安全的，所以本函数只做这两件事。
   */

static irqreturn_t uart_rx_interrupt(int irq, void *dev_id)
{
    struct uart_sensor_dev *dev = dev_id;
    void __iomem *base = dev->base;
    u8 byte;

    /* 1. 确认是 RX 数据就绪中断（有时可能是其他原因触发） */
    if (!(readl(base + USR2) & USR2_RDR))
        return IRQ_NONE;  /* 不是我们的中断，返回 IRQ_NONE */

    /* ② 循环读空硬件FIFO（一次中断可能积攒了多个字节） */
    while (readl(base + USR2) & USR2_RDR){
        byte = readb(base + URXD) & 0xFF;  //只取数据字节
        kfifo_in(&dev->rx_fifo, &byte, 1);    //写入环形缓冲
        dev->rx_bytes++;                      //统计接收字节数
    }

    /* ③ 唤醒阻塞在 read() 上的进程 */
    wake_up_interruptible(&dev->rx_wq);
    return IRQ_HANDLED;
}

/*
   * uart_sensor_open — 打开设备 /dev/uart_sensor
   *
   * 把全局设备指针 g_uart_dev 存入 filp->private_data，
   * 后续 read/write/poll
  都通过它拿到设备结构体，无需再查全局变量。
   *
   * open_count 用 atomic 计数，方便 remove
  时判断是否还有进程在使用设备。
   */
static int uart_sensor_open(struct inode *inode,struct file *filp)
{
    filp->private_data = g_uart_dev;
    atomic_inc(&g_uart_dev->open_count);
    return 0;
}


static int uart_sensor_release(struct inode *inode,struct file *filp)
{
    struct uart_sensor_dev *dev = filp->private_data;
    atomic_dec(&dev->open_count);
    return 0;
}

/*
   * uart_sensor_read — 从 kfifo
  环形缓冲读数据到用户空间
   *
   * 两种模式：
   *   阻塞（默认）：kfifo 空 → wait_event_interruptible
  睡眠
   *             → RX 中断来 → kfifo_in → wake_up 唤醒
   *             → kfifo_to_user 取数据拷给用户
   *
   *   非阻塞（O_NONBLOCK）：kfifo 空 → 直接返回
  -EAGAIN，不等待
   *
   * 返回值：实际拷贝给用户的字节数，或被信号打断则返回
  -ERESTARTSYS
   */
static ssize_t uart_sensor_read(struct file *filp, char __user *buf,size_t count, loff_t *offset)
{
    struct uart_sensor_dev *dev = filp->private_data;
    unsigned int copied;
    int ret;

    /* === 非阻塞模式：没数据就直接返回 -EAGAIN === */
    if (filp->f_flags & O_NONBLOCK) {
        if (kfifo_is_empty(&dev->rx_fifo))
            return -EAGAIN;  /* 没数据了，非阻塞直接返回 */
    } else {
        /* === 阻塞模式：没数据就睡眠等待 === */
        ret = wait_event_interruptible(dev->rx_wq, !kfifo_is_empty(&dev->rx_fifo));
        if (ret)
            return ret;  /* 被信号打断，返回错误 */
    }
    /* === kfifo_to_user：一次完成取数据 + 拷到用户空间 === */
    ret = kfifo_to_user(&dev->rx_fifo, buf, count, &copied);
    if (ret)
        return ret;  /* kfifo_to_user 内部 copy_to_user 失败 */
    
    return copied;  /* 成功返回实际拷给用户的字节数 */
}

/*
   * uart_sensor_write — 把用户数据通过 UART3 TX 发送给
  PC
   *
   * 这是 TX 的唯一实现：逐字节写入硬件 UTXD 寄存器。
   * 每次写之前轮询 USR1_TRDY 确保 TX FIFO 有空位。
   *
   * 数据流：
   *   用户 write("STATUS\n")
   *     → 驱动逐字节 writeb(UTXD)
   *     → 硬件自动移位输出到 TX 引脚
   *     → DB9 → RS232 → PC 串口助手显示
   */
static ssize_t uart_sensor_write(struct file *filp, const char __user *buf,size_t count, loff_t *offset)
{
    struct uart_sensor_dev *dev = filp->private_data;
    void __iomem *base = dev->base;
    char *kbuf;
    int i;

    if (count == 0)
          return 0;
    /* 从用户空间拷数据到内核 */
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    /*
       * 逐字节发送：
       *   ① 等 USR1_TRDY=1 —— TX FIFO 有空位，可以写入
       *   ② writeb(UTXD) —— 写入发送寄存器，硬件自动移位输出
       */
    for (i = 0; i < count; i++) {
        /* ① 等 TX FIFO 有空位 */
        while (!(readl(base + USR1) & USR1_TRDY))
            cpu_relax();  /* 等待硬件准备好，避免死循环占 CPU */

        /* ② 写数据到 UTXD 寄存器 */
        writeb(kbuf[i], base + UTXD);
    }
    dev->tx_bytes += count;  /* 统计发送字节数 */
    kfree(kbuf);
    
    return count;  /* 成功返回写入的字节数 */
}

/*
   * uart_sensor_poll — poll / select / epoll 支持
   *
   * POLLIN  ：kfifo 非空时可读
   * POLLOUT ：始终可写
   */
static unsigned int uart_sensor_poll(struct file *filp, poll_table *wait)
{
    struct uart_sensor_dev *dev = filp->private_data;
    unsigned int mask = 0;

    /* 注册等待队列，等待 kfifo 有数据时唤醒 */
    poll_wait(filp, &dev->rx_wq, wait);

    /* kfifo 非空时可读 */
    if (!kfifo_is_empty(&dev->rx_fifo))
        mask |= POLLIN | POLLRDNORM;  /* 可读 */

    /* UART 发送永远准备好，所以始终可写 */
    mask |= POLLOUT | POLLWRNORM;   /* 可写 */

    return mask;
}

/* 文件操作注册表 */
static const struct file_operations uart_sensor_fops =
{
    .owner   = THIS_MODULE,
    .open    = uart_sensor_open,
    .release = uart_sensor_release,
    .read    = uart_sensor_read,
    .write   = uart_sensor_write,
    .poll    = uart_sensor_poll,
};

/* ===================================================================
* probe：驱动匹配到设备树节点时调用
*
* 前半部分做 5 件事：
*   ① 分配内存
*   ② ioremap 寄存器
*   ③ 获取中断号
*   ④ 获取并使能时钟
*   ⑤ 读波特率
* ===================================================================
*/
static int uart_sensor_probe(struct platform_device *pdev)
{
    struct device *d = &pdev->dev;
    struct uart_sensor_dev *dev;
    struct resource *res;
    u32 baud;
    int ret;

    /* ① 分配内存 */
    dev = devm_kzalloc(d, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    g_uart_dev = dev;  /* 方便中断处理函数访问 */
    platform_set_drvdata(pdev, dev);  /* 关联设备和驱动数据结构 */

    /* ===== ② ioremap：物理地址 → 虚拟地址 ===== */
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(d, "无法获取寄存器资源\n");
        return -ENODEV;
    }
    dev->base = devm_ioremap_resource(d, res);
    if (IS_ERR(dev->base))
        return PTR_ERR(dev->base);

    /* ===== ③ 获取中断号 ===== */
    dev->irq = platform_get_irq(pdev, 0);
    if (dev->irq < 0) {
        dev_err(d, "无法获取中断号\n");
        return -ENODEV;
    }

    /* ===== ④ 获取并使能时钟 ===== */
    dev->clk_ipg = devm_clk_get(d, "ipg");
    dev->clk_per  = devm_clk_get(d, "per");
    if (IS_ERR(dev->clk_ipg) || IS_ERR(dev->clk_per)) {
        dev_err(d, "无法获取时钟\n");
        return -ENODEV;
    }

    ret = clk_prepare_enable(dev->clk_ipg);
    if (ret) {
        dev_err(d, "无法使能 ipg 时钟\n");
        return -ENODEV;
    }
    ret = clk_prepare_enable(dev->clk_per);
    if (ret) {
        dev_err(d, "无法使能 per 时钟\n");
        clk_disable_unprepare(dev->clk_ipg);
        return -ENODEV;
    }

    /* ===== ⑤ 读设备树中的波特率 ===== */
    if (of_property_read_u32(d->of_node, "current-speed", &baud))
          baud = 115200;                     /* 设备树没配则默认 115200*/
      dev->baud_rate = baud;

    dev_info(d, "probe 成功：base=0x%p, irq=%d, baud=%d\n",
             dev->base, dev->irq, dev->baud_rate);


    /* ⑥ 初始化 UART 硬件寄存器 */
    ret = uart_hw_init(dev);
    if (ret) {
        dev_err(d, "UART 硬件初始化失败\n");
        goto err_clk;
    }

    /* ⑦ 注册 RX 接收中断 */
    ret = devm_request_irq(d, dev->irq, uart_rx_interrupt, 0,
                           UART_SENSOR_NAME, dev);
    if (ret) {
        dev_err(d, "无法请求中断\n");
        goto err_clk;
    }

    /* ⑧ 初始化 kfifo + 等待队列 + 互斥锁 */
    INIT_KFIFO(dev->rx_fifo);
    init_waitqueue_head(&dev->rx_wq);
    mutex_init(&dev->lock);
    atomic_set(&dev->open_count, 0);

    /* ⑨ 创建字符设备 /dev/uart_sensor */
    ret = alloc_chrdev_region(&dev->dev_id, 0, DEVICE_COUNT, UART_SENSOR_NAME);
    if (ret) {
        dev_err(d, "无法分配字符设备号\n");
        goto err_clk;
    }

    cdev_init(&dev->cdev, &uart_sensor_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->dev_id, DEVICE_COUNT);
    if (ret) {
        dev_err(d, "无法添加字符设备\n");
        goto err_cdev;
    }

    dev->class = class_create(THIS_MODULE, UART_SENSOR_NAME);
    if (IS_ERR(dev->class)) {
        dev_err(d, "无法创建设备类\n");
        ret = PTR_ERR(dev->class);
        goto err_class;
    }

    dev->device = device_create(dev->class, NULL, dev->dev_id, NULL, UART_SENSOR_NAME);
    if (IS_ERR(dev->device)) {
        dev_err(d, "无法创建设备节点\n");
        ret = PTR_ERR(dev->device);
        goto err_device;
    }

    dev_info(d, "uart_sensor 初始化成功 major=%d irq=%d baud=%d\n",
               MAJOR(dev->dev_id), dev->irq, dev->baud_rate);

    return 0;
err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);
err_clk:
    clk_disable_unprepare(dev->clk_per);
    clk_disable_unprepare(dev->clk_ipg);
    return ret;
}



/* ===================================================================
   * uart_sensor_remove — 模块卸载 / 
  设备树节点删除时调用
   *
   * 清理顺序与 probe 严格相反。
   * devm_ 开头的资源（kzalloc / ioremap /
  request_irq）由内核自动释放，
   * 不需要手动处理。非 devm 资源必须手动清理。
   * =================================================================== */

static int uart_sensor_remove(struct platform_device *pdev)
{
    struct uart_sensor_dev *dev = platform_get_drvdata(pdev);

    /* ① 关 UART 硬件：先关 RX 中断，再关总开关 */
    writel(0, dev->base + UCR4);  /* 关 RX 中断 */
    writel(0, dev->base + UCR1);  /* 关 UART */
    /* ② 销毁字符设备（逆序：device → class → cdev →设备号） */

    device_destroy(dev->class, dev->dev_id);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);

    clk_disable_unprepare(dev->clk_per);
    clk_disable_unprepare(dev->clk_ipg);

    dev_info(&pdev->dev, "uart_sensor 已卸载 (rx=%lu bytes, tx=%lu bytes)\n",dev->rx_bytes, dev->tx_bytes);
    return 0;
}

/* 设备树匹配表 —— 与 dts 中的 compatible 字符串对应
  */
static const struct of_device_id uart_sensor_of_match[] = {
      { .compatible = "smartmonitor,uart-sensor" },
      { /* sentinel */ }
  };
MODULE_DEVICE_TABLE(of, uart_sensor_of_match);

/* 平台驱动结构体 —— 内核用它匹配设备树节点、调用
  probe / remove */
static struct platform_driver uart_sensor_driver = {
    .probe = uart_sensor_probe,
    .remove = uart_sensor_remove,
    .driver = {
        .name = UART_SENSOR_NAME,
        .of_match_table = uart_sensor_of_match,
    },
};


module_platform_driver(uart_sensor_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("UART3 command console driver for smart environment monitor");