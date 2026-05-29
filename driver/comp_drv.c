#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/timer.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include "comp_drv.h"

#define DRV_NAME "comp_drv"
#define DEVICE_CNT  1

struct comp_dev
{
    dev_t dev_id;
    struct cdev cdev; //字符设备
    struct class *class;
    struct device *device;
    struct gpio_desc *led_gpio; //GPIO描述符

    /* ===== 状态数据（受 mutex 保护） ===== */
    struct mutex lock;          // 互斥锁，保护下面所有字段
    enum led_state led_state;
    int  blink_period_ms;       // 闪烁周期，0 = 不闪烁
    unsigned long write_count; //写计数
    unsigned long read_count;
    atomic_t changed;           // 原子变量：LED 状态是否变化过（用于阻塞 read）

    /* ===== IO 模型 ===== */
    wait_queue_head_t wq;       // 等待队列头（阻塞 read 就睡在这上面）
    struct fasync_struct *fasync; // fasync 链表（SIGIO （异步）通知用）

    /* ===== 内核定时器 ===== */
    struct timer_list timer;    // 闪烁定时器

    //用于debugfs
    struct dentry *debugfs_dir;   // 目录项：/sys/kernel/debug/comp_drv
    struct dentry *debugfs_file;  // 文件项：/sys/kernel/debug/comp_drv/info
};

static struct comp_dev *g_dev;  /* probe 时赋值，全驱动共用 */


//把 LED 状态同步到硬件 GPIO
static void comp_led_update(struct comp_dev *dev)
{
    if (dev->led_state == LED_ON || dev->led_state == LED_BLINK)
        gpiod_set_value(dev->led_gpio, 1);//亮
    else
        gpiod_set_value(dev->led_gpio, 0);//灭
}


//通知所有等待者
static void comp_led_notify(struct comp_dev *dev)
{
    atomic_set(&dev->changed, 1);        // 标记“数据变了”
    wake_up_interruptible(&dev->wq);     // 唤醒阻塞 read
    kill_fasync(&dev->fasync, SIGIO, POLL_IN); // 发信号给应用
}

//led翻转
static void comp_timer_callback(unsigned long data)
{
    struct comp_dev *dev = (struct comp_dev *)data;
    static int toggle;  // 0→1→0→1... 实现翻转

    toggle = !toggle;
    gpiod_set_value(dev->led_gpio, toggle);

    /* 重新设置定时器，下一次到期 = 当前时间 + period_ms */
    mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->blink_period_ms));
}

//file_operations实现
static int comp_open(struct inode *inode,struct file *filp)
{
    filp->private_data = g_dev;
    pr_info(DRV_NAME ": opened\n");
    return 0 ;

}

static int comp_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t comp_read(struct file *filp,
                         char __user *buf,
                         size_t count,
                         loff_t *ppos)
{
    // 1. 从文件私有数据拿到设备结构体
    struct comp_dev *dev = filp->private_data;

    // 2. 定义要返回给用户空间的状态结构体
    struct comp_drv_status status;
    int ret;

    /* ===== 非阻塞模式：直接返回 ===== */
    if (filp->f_flags & O_NONBLOCK) {//由用户打开文件时确定O_NONBLOCK
        if (mutex_lock_interruptible(&dev->lock))
            return -ERESTARTSYS;
        goto fill;  // 拿锁后直接填充数据
    }

    /* ===== 阻塞模式：等待有变化才返回 ===== */
    ret = wait_event_interruptible(dev->wq, atomic_read(&dev->changed));
    if (ret)
        return -ERESTARTSYS;  // 被信号唤醒（不是数据准备好）
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

fill:
    status.state          = dev->led_state;
    status.blink_period_ms = dev->blink_period_ms;
    status.read_count     = dev->read_count;
    status.write_count    = dev->write_count;

    atomic_set(&dev->changed, 0);  // 消费掉"变化"标记
    dev->read_count++;

    mutex_unlock(&dev->lock);

    if (count > sizeof(status))
        count = sizeof(status);

    if (copy_to_user(buf, &status, count))
        return -EFAULT;

    return count;
}

static ssize_t comp_write(struct file *filp,const char __user *buf,
                          size_t count, loff_t *ppos)
{
    // 1. 从文件私有数据拿到我们的设备结构体（包含LED、状态等）
    struct comp_dev *dev = filp->private_data;
    char kbuf[16];  // 内核缓冲区，用来临时存放用户发来的数据
    int ret = 0;

    // 2. 安全限制：用户写的数据太长就截断，防止内核缓冲区溢出
    if (count > sizeof(kbuf) - 1)
        count = sizeof(kbuf) - 1;

    // 3. 从用户空间拷贝数据到内核空间（必须用这个函数！）
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;  // 拷贝失败，返回错误

    // 4. 手动添加字符串结束符，确保是合法C字符串
    kbuf[count] = '\0';

    /* ===== 临界区开始 ===== */
    if (mutex_lock_interruptible(&dev->lock))//上锁
        return -ERESTARTSYS;
    
    if (strncmp(kbuf, "on", 2) == 0 || kbuf[0] == '1') {
        if (dev->led_state == LED_BLINK)
            del_timer_sync(&dev->timer);
        dev->led_state = LED_ON;
        comp_led_update(dev);
    } else if (strncmp(kbuf, "off", 3) == 0 || kbuf[0] == '0') {
        if (dev->led_state == LED_BLINK)
            del_timer_sync(&dev->timer);
        dev->led_state = LED_OFF;
        comp_led_update(dev);
    } else {
        ret = -EINVAL;
        goto out;
    }

    dev->write_count++;
    comp_led_notify(dev);

out:
    mutex_unlock(&dev->lock);

    return ret ? ret : count;
}
// poll/select 支持
static unsigned int comp_poll(struct file *filp, poll_table *wait)
{
    struct comp_dev *dev = filp->private_data;
    unsigned int mask = 0;
    
    poll_wait(filp, &dev->wq, wait);

    if (atomic_read(&dev->changed))
        mask |= POLLIN | POLLRDNORM;

    mask |= POLLOUT | POLLWRNORM;

    return mask;
}
//fasync 异步通知 (SIGIO)
static int comp_fasync(int fd, struct file *filp, int on)
{
    struct comp_dev *dev = filp->private_data;
    return fasync_helper(fd, filp, on, &dev->fasync);
}

//ioctl 实现
// 这是驱动的 ioctl 操作函数（OPS 里的 .unlocked_ioctl）
static long comp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    // 1. 拿到设备结构体（led状态、锁、定时器等）
    struct comp_dev *dev = filp->private_data;
    unsigned long period;       // 用来存闪烁周期
    struct comp_drv_status status; // 用来存设备状态
    int ret = 0;                // 返回值

    /* ========== 上锁：保护设备数据，防止多进程同时操作 ========== */
    // 上锁，可被信号打断
    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    /* ========== 根据用户发来的命令 cmd 执行不同操作 ========== */
    switch (cmd) {

    /* ==========================================
     * 命令1：设置 LED 闪烁周期
     * ========================================== */
    case COMP_DRV_SET_BLINK_PERIOD:
        // 从用户空间拷贝周期数值到内核
        if (copy_from_user(&period, (void __user *)arg, sizeof(period))) {
            ret = -EFAULT;
            break;
        }
        // 把周期存到设备结构体
        dev->blink_period_ms = period;
        break;

    /* ==========================================
     * 命令2：获取设备状态（LED状态、周期、读写次数）
     * ========================================== */
    case COMP_DRV_GET_STATUS:
        // 填充状态结构体
        status.state           = dev->led_state;
        status.blink_period_ms = dev->blink_period_ms;
        status.read_count      = dev->read_count;
        status.write_count     = dev->write_count;

        // 把状态拷贝回用户空间
        if (copy_to_user((void __user *)arg, &status, sizeof(status)))
            ret = -EFAULT;
        break;

    /* ==========================================
     * 命令3：启动 LED 自动闪烁
     * ========================================== */
    case COMP_DRV_START_BLINK:
        // 周期必须 >0，否则报错
        if (dev->blink_period_ms <= 0) {
            ret = -EINVAL;
            break;
        }
        dev->led_state = LED_BLINK;   // 设为闪烁模式
        comp_led_update(dev);         // 更新硬件LED
        // 启动定时器：周期到了自动切换LED
        mod_timer(&dev->timer, jiffies + msecs_to_jiffies(dev->blink_period_ms));
        comp_led_notify(dev);         // 通知用户：状态变了
        break;

    /* ==========================================
     * 命令4：停止闪烁，关掉LED
     * ========================================== */
    case COMP_DRV_STOP_BLINK:
        del_timer_sync(&dev->timer);  // 删掉定时器，停止闪烁
        dev->led_state = LED_OFF;     // 设为关闭
        comp_led_update(dev);         // 更新硬件
        comp_led_notify(dev);         // 通知用户
        break;

    /* ==========================================
     * 不认识的命令
     * ========================================== */
    default:
        ret = -ENOTTY;  // 命令不支持
        break;
    }

    mutex_unlock(&dev->lock); // 解锁
    return ret;
}

// show 函数——当用户 cat 时内核调用它（sysyfs）
static ssize_t led_state_show(struct device *d,
                            struct device_attribute *attr, char *buf)
{
    struct comp_dev *dev = g_dev;
    const char *state_str;
    int len;

    mutex_lock(&dev->lock);
    switch (dev->led_state) {
        case LED_ON:    state_str = "on";    break;
        case LED_OFF:   state_str = "off";   break;
        case LED_BLINK: state_str = "blink"; break;
        default:        state_str = "unknown"; break;
    }
    len = scnprintf(buf, PAGE_SIZE, "%s\n", state_str);
    mutex_unlock(&dev->lock);
    return len;
}

static ssize_t write_count_show(struct device *d,
                               struct device_attribute *attr, char *buf)
{
    struct comp_dev *dev = g_dev;
    int len;

    mutex_lock(&dev->lock);
    len = scnprintf(buf, PAGE_SIZE, "%lu\n", dev->write_count);
    mutex_unlock(&dev->lock);
    return len;
}
//把 show 函数注册为 sysfs文件
static DEVICE_ATTR(led_state, S_IRUGO, led_state_show, NULL);
static DEVICE_ATTR(write_count, S_IRUGO, write_count_show, NULL);

static struct attribute *comp_attrs[] = {
    &dev_attr_led_state.attr,
    &dev_attr_write_count.attr,
    NULL,   // 哨兵，表示数组结束
};

static const struct attribute_group comp_attr_group = {
    .attrs = comp_attrs,
};

//另外一种调试模式debugfs
static int comp_debug_show(struct seq_file *s, void *v)
{
    struct comp_dev *dev = s->private;

    mutex_lock(&dev->lock);
    seq_printf(s, "=== comp_drv debug info ===\n");
    seq_printf(s, "led_state     : %d\n", dev->led_state);
    seq_printf(s, "blink_period  : %d ms\n", dev->blink_period_ms);
    seq_printf(s, "read_count    : %lu\n", dev->read_count);
    seq_printf(s, "write_count   : %lu\n", dev->write_count);
    seq_printf(s, "changed       : %d\n", atomic_read(&dev->changed));
    mutex_unlock(&dev->lock);

    return 0;
}

static int comp_debug_open(struct inode *inode, struct file *filp)
{
    return single_open(filp, comp_debug_show, inode->i_private);
}

static const struct file_operations comp_debug_fops = {
    .owner   = THIS_MODULE,
    .open    = comp_debug_open,
    .read    = seq_read,        // 内核提供的，不用自己写
    .llseek  = seq_lseek,       // 内核提供的
    .release = single_release,  // 内核提供的
};

static const struct file_operations comp_fops = {
    .owner = THIS_MODULE,
    .open = comp_open,
    .release = comp_release,
    .read    = comp_read,
    .write   = comp_write,
    .poll           = comp_poll,
    .fasync         = comp_fasync,
    .unlocked_ioctl = comp_ioctl,
};
static int comp_probe(struct platform_device *pdev)
{
    struct device *d = &pdev->dev;
    struct comp_dev *dev;
    int ret;

    dev_info(d, DRV_NAME ": probing...\n");

    /* ① 分配设备结构体 */
    dev = devm_kzalloc(d, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    g_dev = dev;
    platform_set_drvdata(pdev, dev);

    /* ② 从设备树获取 GPIO */
    dev->led_gpio = devm_gpiod_get(d, "led", GPIOD_OUT_LOW);
    if (IS_ERR(dev->led_gpio)) {
        ret = PTR_ERR(dev->led_gpio);
        dev_err(d, "failed to get led-gpios: %d\n", ret);
        return ret;
    }

    /* === 新增：并发控制初始化 === */
    mutex_init(&dev->lock);
    init_waitqueue_head(&dev->wq);
    atomic_set(&dev->changed, 0);

    /* === 新增：定时器初始化 === */
    setup_timer(&dev->timer, comp_timer_callback, (unsigned long)dev);


    /* ③ 动态分配设备号 */
    ret = alloc_chrdev_region(&dev->dev_id, 0, DEVICE_CNT, DRV_NAME);
    if (ret < 0) {
        dev_err(d, "alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    /* ④ 初始化 cdev 并注册 */
    cdev_init(&dev->cdev, &comp_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->dev_id, DEVICE_CNT);
    if (ret) {
        dev_err(d, "cdev_add failed: %d\n", ret);
        goto err_cdev;
    }

    /* ⑤ 创建设备类 */
    dev->class = class_create(THIS_MODULE, DRV_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        dev_err(d, "class_create failed: %d\n", ret);
        goto err_class;
    }

    /* ⑥ 创建设备节点 /dev/comp_drv */
    dev->device = device_create(dev->class, d, dev->dev_id, NULL, DRV_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        dev_err(d, "device_create failed: %d\n", ret);
        goto err_device;
    }

    /* === 新增：sysfs 属性组 === */
    //sysfs_create_group 调用后，两个文件会同时出现在 /sys/class/comp_drv/comp_drv/ 下
    ret = sysfs_create_group(&d->kobj, &comp_attr_group);
    if (ret) {
        dev_err(d, "sysfs_create_group failed: %d\n", ret);
        goto err_sysfs;
    }

    /* === 新增：debugfs 调试节点 === */
    //创建debugfs
    dev->debugfs_dir  = debugfs_create_dir(DRV_NAME, NULL);
    dev->debugfs_file = debugfs_create_file("info", S_IRUGO,
                        dev->debugfs_dir, dev, &comp_debug_fops);

    dev_info(d, DRV_NAME " probed OK (major=%d)\n", MAJOR(dev->dev_id));
    return 0;

err_sysfs:
    device_destroy(dev->class, dev->dev_id);
err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->dev_id, DEVICE_CNT);
    return ret;
}

static int comp_remove(struct platform_device *pdev)
{
    struct comp_dev *dev = platform_get_drvdata(pdev);

    /* === 新增：停止定时器（必须先做！） === */
    del_timer_sync(&dev->timer);

    gpiod_set_value(dev->led_gpio, 0);
    device_destroy(dev->class, dev->dev_id);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->dev_id, DEVICE_CNT);

    /* === 新增：销毁互斥锁 === */
    mutex_destroy(&dev->lock);

    dev_info(&pdev->dev, DRV_NAME " removed\n");

    return 0;
}
static const struct of_device_id comp_of_match[] = {
    {.compatible = "smartmonitor,comp-drv"},
    {}
};

MODULE_DEVICE_TABLE(of, comp_of_match);

static struct platform_driver comp_platform_driver = {
    .probe = comp_probe,
    .remove = comp_remove,
    .driver = {
        .name           = DRV_NAME,
        .of_match_table = comp_of_match,
        .owner          = THIS_MODULE,
    },
};

module_platform_driver(comp_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("LED character device driver for smart monitor");