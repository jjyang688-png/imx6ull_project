#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/debugfs.h>

#define KEY_DRV_NAME "key_input"
#define DEFAULT_DEBOUNCE_MS 20 //消抖20ms

static int default_debounce = DEFAULT_DEBOUNCE_MS;
module_param(default_debounce, int, 0644);

static int default_keycode = KEY_ENTER;   // KEY_ENTER = 28
module_param(default_keycode, int, 0644);

static struct dentry *key_dbg_dir;

//设备结构体
struct key_dev {
    struct gpio_desc *key_gpio;     /* GPIO 描述符 */
    int irq;                        /* 中断号 */
    int key_code;                   /* 按键码，如 KEY_ENTER=28 */
    int debounce_ms;                /* 去抖时间（毫秒） */

    struct input_dev *input;        /* input 子系统设备 */

    spinlock_t lock;                /* 自旋锁：保护与中断处理程序共享的数据 */
    struct delayed_work work;       /* 底半部：延迟工作队列，实现去抖 */
    bool last_state;                /* 上一次 GPIO 电平，用于检测边沿 */
    unsigned long irq_count;        /* 中断计数（供 debugfs 查看） */
};


static struct key_dev *g_key_dev;

//中断下半部
/* ===================================================================
 * 底半部 (Bottom Half)：延迟工作队列的回调函数
 *
 * 为什么用 workqueue 而不是 tasklet？
 * - tasklet 中不能睡眠，但如果将来需要 I2C/SPI 读取按键状态就必须睡眠
 * - workqueue 运行在进程上下文，允许睡眠，更通用
 * - delayed_work 自带延迟功能，天然适合"等电平稳定后再读"的场景
 *
 * 调用时机：中断顶半部调度此函数，延迟 debounce_ms 毫秒后执行
 * =================================================================== */
static void key_work_func(struct work_struct *ws)
{
    /* 1. 从 work_struct 反推出包裹它的 delayed_work */
    struct delayed_work *dwork = to_delayed_work(ws);

    /* 2. 从 delayed_work 反推出包含它的 key_dev
     *    container_of 是内核最常用的宏之一：
     *    已知成员指针 → 反算结构体起始地址 */
    struct key_dev *dev = container_of(dwork, struct key_dev, work);
    unsigned long flags;
    int val;

    /* 3. 去抖延迟结束后，读取稳定的 GPIO 电平 */
    val = gpiod_get_value(dev->key_gpio);

    /* 4. 临界区：比较 last_state，判断是否真的发生了边沿变化
     *    用自旋锁而不是 mutex，因为 last_state 被中断上下文也访问 */
    spin_lock_irqsave(&dev->lock, flags);

    if (val != dev->last_state) {
        /* 确实是有效边沿（不是毛刺），更新状态 */
        dev->last_state = val;
        spin_unlock_irqrestore(&dev->lock, flags);

        /* 5. 向 input 子系统报告按键事件
         *    input_report_key(input, code, value)
         *    value=1 表示按下，value=0 表示释放
         *    GPIO 低电平 = 按下（active-low），所以 val==0 时报告按下 */
        input_report_key(dev->input, dev->key_code, val ? 0 : 1);

        /* 6. 同步事件：把缓冲的事件立即发出，用户空间才能收到
         *    这是一个完整的事件包边界标记 */
        input_sync(dev->input);

        pr_debug(KEY_DRV_NAME ": 按键 %s (code=%d)\n",
                 val ? "释放" : "按下", dev->key_code);
    } else {
        /* 毛刺干扰：电平没变，啥也不做 */
        spin_unlock_irqrestore(&dev->lock, flags);
    }
}

static irqreturn_t key_irq_handler(int irq, void *data)
{
    struct key_dev *dev = data;

    /* 统计中断次数（供 debugfs 查看） */
    dev->irq_count++;

    schedule_delayed_work(&dev->work,msecs_to_jiffies(dev->debounce_ms));
    
    return IRQ_HANDLED;
}


/* ===================================================================
 * platform_driver -> probe：设备与驱动匹配成功时调用
 *
 * 执行顺序（9 步）：
 *   ① 分配设备结构体
 *   ② 从设备树获取 GPIO
 *   ③ 从设备树读取 key-code 和 debounce-interval
 *   ④ 记录初始 GPIO 电平
 *   ⑤ 初始化自旋锁
 *   ⑥ 初始化底半部 workqueue
 *   ⑦ 注册 input 子系统设备
 *   ⑧ 获取中断号 + 注册中断处理函数
 *   ⑨ 创建 debugfs 调试节点
 * =================================================================== */
static int key_input_probe(struct platform_device *pdev)
{
    struct device *d = &pdev->dev;
    struct input_dev *input;
    struct key_dev *dev;

    int ret;
    u32 key_code;

    dev_info(d, "key_input probing...\n");
    //① 分配设备结构体
    dev = devm_kzalloc(d, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    g_key_dev = dev;
    platform_set_drvdata(pdev, dev);

    /* ② 从设备树获取 GPIO
     *    devm_gpiod_get(设备, 后缀, 方向)
     *    "key" → 设备树属性名 "key-gpios"
     *    GPIOD_IN → 配置为输入模式 */
    dev->key_gpio = devm_gpiod_get(d, "key", GPIOD_IN);
    if (IS_ERR(dev->key_gpio)) {
        ret = PTR_ERR(dev->key_gpio);
        dev_err(d, "获取 key-gpios 失败: %d\n", ret);
        return ret;
    }

    //③ 从设备树读取 key-code 和 debounce-interval
    ret = of_property_read_u32(d->of_node, "key-code", &key_code);
    if (ret)
        key_code = default_keycode;
    dev->key_code = key_code;

    ret = of_property_read_u32(d->of_node, "debounce-interval",
                               &dev->debounce_ms);
    if (ret)
        dev->debounce_ms = default_debounce;

    //④ 记录初始 GPIO 电平
    dev->last_state = gpiod_get_value(dev->key_gpio);

    //⑤ 初始化自旋锁
    spin_lock_init(&dev->lock);

    //⑥ 初始化底半部 workqueue
    INIT_DELAYED_WORK(&dev->work, key_work_func);

    //⑦ 注册 input 子系统设备
    input = devm_input_allocate_device(d);
    if (!input)
        return -ENOMEM;

    input->name = KEY_DRV_NAME;
    input->phys = "key_input/input0";
    input->id.bustype = BUS_HOST;
    input->id.vendor  = 0x0001;
    input->id.product = 0x0001;
    input->id.version = 0x0100;


    /* 告诉内核：本设备支持 EV_KEY 类型事件，且只产生 key_code 这个键码
     * __set_bit 在内核位图中设置对应位，input 核心据此判断能力 */
    __set_bit(EV_KEY, input->evbit);
    __set_bit(dev->key_code, input->keybit);

    ret = input_register_device(input);
    if (ret) {
        dev_err(d, "input_register_device 失败: %d\n", ret);
        return ret;
    }
    dev->input = input;

    //⑧ 获取中断号 + 注册中断处理函数
    dev->irq = gpiod_to_irq(dev->key_gpio);
    if (dev->irq < 0) {
        dev_err(d, "gpiod_to_irq 失败: %d\n", dev->irq);
        return dev->irq;
    }

    ret = devm_request_irq(d,dev->irq,key_irq_handler,IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,KEY_DRV_NAME, dev);

    if (ret) {
        dev_err(d, "request_irq 失败: %d\n", ret);
        return ret;
    }

    key_dbg_dir = debugfs_create_dir(KEY_DRV_NAME, NULL);

    if (!IS_ERR_OR_NULL(key_dbg_dir)) {
        debugfs_create_u32("debounce_ms", 0444, key_dbg_dir,
                           (u32 *)&dev->debounce_ms);
        debugfs_create_u32("key_code", 0444, key_dbg_dir,
                           (u32 *)&dev->key_code);
        debugfs_create_u64("irq_count", 0444, key_dbg_dir,
                            (u64 *)&dev->irq_count);
        debugfs_create_u32("irq_num", 0444, key_dbg_dir,
                           (u32 *)&dev->irq);
    }
    dev_info(d, KEY_DRV_NAME " 初始化成功 (irq=%d, code=%d, debounce=%dms)\n",
             dev->irq, dev->key_code, dev->debounce_ms);
    return 0;

}

/* ===================================================================
 * platform_driver -> remove：驱动卸载或设备移除时调用
 *
 * 清理顺序必须是：
 *   ① 先清理 debugfs
 *   ② 再取消可能正在运行的工作队列（cancel_delayed_work_sync 会等待完成）
 *   ③ devm_ 资源（IRQ、input、GPIO、kzalloc）由内核自动释放
 * =================================================================== */
static int key_input_remove(struct platform_device *pdev)
{
    struct key_dev *dev = platform_get_drvdata(pdev);

    /* ① 递归删除 debugfs 目录及其下所有文件 */
    debugfs_remove_recursive(key_dbg_dir);

    /* ② 取消延迟工作队列并等待完成
     *    _sync 版本：如果工作正在运行，等待它完成再返回
     *    这很重要——不等待的话 work 可能在设备已释放后访问野指针 */
    cancel_delayed_work_sync(&dev->work);

    dev_info(&pdev->dev, KEY_DRV_NAME " 已卸载 (中断总数=%lu)\n",
             dev->irq_count);
    return 0;
}
/* ====== 设备树匹配表 ====== */
static const struct of_device_id key_input_of_match[] = {
    { .compatible = "smartmonitor,key-input" },
    { /* 哨兵：空条目表示数组结束 */ }
};
MODULE_DEVICE_TABLE(of, key_input_of_match);

/* ====== platform_driver 结构体 ====== */
static struct platform_driver key_input_platform_driver = {
    .probe  = key_input_probe,
    .remove = key_input_remove,
    .driver = {
        .name           = KEY_DRV_NAME,
        .of_match_table = key_input_of_match,
        .owner          = THIS_MODULE,
    },
};

module_platform_driver(key_input_platform_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("按键输入驱动 (Input 子系统 + 中断 + workqueue 去抖)");



