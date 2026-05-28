#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
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

    enum led_state led_state;
    unsigned long write_count; //写计数
};

static struct comp_dev *g_dev;  /* probe 时赋值，全驱动共用 */


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

    // 3. 填充要返回的数据：LED状态 + 写次数
    status.state       = dev->led_state;
    status.write_count = dev->write_count;

    // 5. 用户要读的长度不能超过我们结构体大小
    if (count > sizeof(status))
        count = sizeof(status);

    // 6. 把内核数据拷贝到用户空间（不能直接赋值！）
    ret = copy_to_user(buf, &status, count);
    if (ret)
        return -EFAULT;

    // 7. 返回实际读到的字节数
    return count;
}

static ssize_t comp_write(struct file *filp,const char __user *buf,
                          size_t count, loff_t *ppos)
{
    // 1. 从文件私有数据拿到我们的设备结构体（包含LED、状态等）
    struct comp_dev *dev = filp->private_data;
    char kbuf[16];  // 内核缓冲区，用来临时存放用户发来的数据

    // 2. 安全限制：用户写的数据太长就截断，防止内核缓冲区溢出
    if (count > sizeof(kbuf) - 1)
        count = sizeof(kbuf) - 1;

    // 3. 从用户空间拷贝数据到内核空间（必须用这个函数！）
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;  // 拷贝失败，返回错误

    // 4. 手动添加字符串结束符，确保是合法C字符串
    kbuf[count] = '\0';

    // 5. 判断用户输入的内容，控制LED亮灭
    if (strncmp(kbuf, "on", 2) == 0 || kbuf[0] == '1') {
        // 写 "on" 或 "1" → 点亮LED
        gpiod_set_value(dev->led_gpio, 1);
        dev->led_state = LED_ON;
    } else if (strncmp(kbuf, "off", 3) == 0 || kbuf[0] == '0') {
        // 写 "off" 或 "0" → 熄灭LED
        gpiod_set_value(dev->led_gpio, 0);
        dev->led_state = LED_OFF;
    } else {
        // 无效命令 → 返回参数错误
        return -EINVAL;
    }

    // 6. 记录写操作次数
    dev->write_count++;

    // 7. 返回成功写入的字节数
    return count;
}

static const struct file_operations comp_fops = {
    .owner = THIS_MODULE,
    .open = comp_open,
    .release = comp_release,
    .read    = comp_read,
    .write   = comp_write,
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

    dev_info(d, DRV_NAME " probed OK (major=%d)\n", MAJOR(dev->dev_id));
    return 0;
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
    gpiod_set_value(dev->led_gpio, 0);
    device_destroy(dev->class, dev->dev_id);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->dev_id, DEVICE_CNT);

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