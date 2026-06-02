/**
 * icm20608.c — ICM20608 6轴 SPI 传感器驱动 (Phase 6)
 *
 * 涉及内核知识点：
 *
 * Ch62    — SPI 子系统：spi_driver、spi_sync、spi_message、spi_transfer
 * Ch43/44 — 设备树：of_match_table、SPI 节点绑定
 * Ch42    — CDEV：alloc_chrdev_region、cdev_init、device_create
 * Ch48    — Mutex：保护 SPI 总线访问
 * Ch47    — 原子操作：open_count
 *
 * ICM20608: 6轴 MEMS 传感器（3轴加速度 + 3轴陀螺仪 + 温度）
 * 接口: SPI (Mode 0/3), 最大 8MHz
 * 设备文件: /dev/icm20608
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spi/spi.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include "icm20608.h"

#define ICM20608_NAME  "icm20608"
#define DEVICE_COUNT   1

/* ====== 模块参数 ====== */
static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "启用调试输出 (0=关闭, 1=开启)");

/* 调试打印宏：仅在 debug=1 时输出 */
#define DBG(dev, fmt, ...) \
    do { if (debug) dev_info(&(dev)->spi->dev, fmt, ##__VA_ARGS__); } while (0)

/* ====== ICM20608 寄存器地址 ====== */
#define REG_WHO_AM_I      0x75   /* 芯片ID寄存器，应返回 0x98 或 0xAF */
#define REG_USER_CTRL     0x6A   /* 用户控制 */
#define REG_PWR_MGMT_1    0x6B   /* 电源管理1（复位、睡眠、时钟源） */
#define REG_PWR_MGMT_2    0x6C   /* 电源管理2 */

/* 加速度计输出（起始 0x3B，连续 6 字节） */
#define REG_ACCEL_XOUT_H  0x3B
#define REG_ACCEL_XOUT_L  0x3C
#define REG_ACCEL_YOUT_H  0x3D
#define REG_ACCEL_YOUT_L  0x3E
#define REG_ACCEL_ZOUT_H  0x3F
#define REG_ACCEL_ZOUT_L  0x40

/* 温度输出（连续 2 字节） */
#define REG_TEMP_OUT_H    0x41
#define REG_TEMP_OUT_L    0x42

/* 陀螺仪输出（起始 0x43，连续 6 字节） */
#define REG_GYRO_XOUT_H   0x43
#define REG_GYRO_XOUT_L   0x44
#define REG_GYRO_YOUT_H   0x45
#define REG_GYRO_YOUT_L   0x46
#define REG_GYRO_ZOUT_H   0x47
#define REG_GYRO_ZOUT_L   0x48

/* ====== REG_PWR_MGMT_1 位定义 ====== */
#define PWR_MGMT1_DEVICE_RESET  (1 << 7)   /* bit7: 写1复位芯片 */
#define PWR_MGMT1_SLEEP         (1 << 6)   /* bit6: 写1进入睡眠 */
#define PWR_MGMT1_CLKSEL_AUTO   0x01       /* bit[2:0]=001: 自动时钟源 */

/* ====== 设备结构体 ====== */
struct icm20608_dev {
    dev_t dev_id;                  /* 设备号 */
    struct cdev cdev;              /* 字符设备 */
    struct class *class;           /* 设备类 */
    struct device *device;         /* 设备节点 */
    struct spi_device *spi;        /* SPI 从设备 */

    struct mutex lock;             /* 保护 SPI 总线 + 传感器数据 */
    atomic_t open_count;           /* 打开计数 */

    struct icm20608_full data;     /* 最新传感器数据缓存 */
};

static struct icm20608_dev *g_icm20608;

/* 前置声明：icm20608_read_reg 复用它，所以在前面声明 */
static int icm20608_read_burst(struct icm20608_dev *dev,
                                u8 start_reg, u8 *buf, int len);

/* ===================================================================
 * SPI 单寄存器读 — 直接复用突发读（长度=1）
 * =================================================================== */
static int icm20608_read_reg(struct icm20608_dev *dev, u8 reg, u8 *val)
{
    return icm20608_read_burst(dev, reg, val, 1);
}

/* ===================================================================
 * SPI 单寄存器写
 * =================================================================== */
static int icm20608_write_reg(struct icm20608_dev *dev, u8 reg, u8 val)
{
    u8 tx_buf[2] = { reg & 0x7F, val };   /* bit7=0: 写操作 */

    struct spi_transfer xfer = {
        .tx_buf = tx_buf,
        .len    = 2,
    };
    struct spi_message msg;

    spi_message_init(&msg);
    spi_message_add_tail(&xfer, &msg);
    return spi_sync(dev->spi, &msg);
}

/* ===================================================================
 * SPI 突发读：从 start_reg 开始连续读 len 字节
 *
 * ICM20608 支持地址自动递增：
 *   发一次起始地址 → 芯片连续返回 N 个寄存器的值
 *   无需重复发送地址。
 * =================================================================== */
static int icm20608_read_burst(struct icm20608_dev *dev,
                                u8 start_reg, u8 *buf, int len)
{
    int ret;
    u8 tx_reg = start_reg | 0x80;

    struct spi_transfer xfer[2] = {
        { .tx_buf = &tx_reg, .len = 1     },   /* 发地址 */
        { .rx_buf = buf,     .len = len    },   /* 收 len 字节 */
    };
    struct spi_message msg;

    spi_message_init(&msg);
    spi_message_add_tail(&xfer[0], &msg);
    spi_message_add_tail(&xfer[1], &msg);

    ret = spi_sync(dev->spi, &msg);
    if (ret < 0)
        dev_err(&dev->spi->dev,
                "SPI 突发读(从 0x%02x 起 %d 字节)失败: %d\n",
                start_reg, len, ret);

    return ret;
}

/* ===================================================================
 * 一次读取全部传感器数据（14 字节：加速度×6 + 温度×2 + 陀螺仪×6）
 *
 * 从 0x3B (ACCEL_XOUT_H) 开始连续读 14 字节：
 *   buf[0:1]  = ACCEL_X  (大端: MSB在前, LSB在后)
 *   buf[2:3]  = ACCEL_Y
 *   buf[4:5]  = ACCEL_Z
 *   buf[6:7]  = TEMP
 *   buf[8:9]  = GYRO_X
 *   buf[10:11] = GYRO_Y
 *   buf[12:13] = GYRO_Z
 *
 * 大端字节序 (Big-Endian)：
 *   高字节在低地址，低字节在高地址
 *   例：ACCEL_X 原始值 0x1234
 *       buf[0] = 0x12 (MSB)  ← 先读出来的
 *       buf[1] = 0x34 (LSB)  ← 后读出来的
 *   组合：((s16)buf[0] << 8) | buf[1]  →  0x1234
 * =================================================================== */
static int icm20608_read_all(struct icm20608_dev *dev)
{
    u8 buf[14];
    int ret;

    /* ① SPI 突发读：一次性读 14 字节 */
    ret = icm20608_read_burst(dev, REG_ACCEL_XOUT_H, buf, sizeof(buf));
    if (ret < 0)
        return ret;

    /* ② 加锁，把解析结果写入 dev->data */
    mutex_lock(&dev->lock);

    /* 大端转小端（CPU 通常是小端） */
    dev->data.accel_x = ((s16)buf[0]  << 8) | buf[1];
    dev->data.accel_y = ((s16)buf[2]  << 8) | buf[3];
    dev->data.accel_z = ((s16)buf[4]  << 8) | buf[5];
    dev->data.temp    = ((s16)buf[6]  << 8) | buf[7];
    dev->data.gyro_x  = ((s16)buf[8]  << 8) | buf[9];
    dev->data.gyro_y  = ((s16)buf[10] << 8) | buf[11];
    dev->data.gyro_z  = ((s16)buf[12] << 8) | buf[13];

    mutex_unlock(&dev->lock);

    DBG(dev, "accel(%d,%d,%d) temp=%d gyro(%d,%d,%d)\n",
        dev->data.accel_x, dev->data.accel_y, dev->data.accel_z,
        dev->data.temp,
        dev->data.gyro_x, dev->data.gyro_y, dev->data.gyro_z);

    return 0;
}

/* ===================================================================
 * open — 打开设备时调用
 *
 * 和 comp_drv 一样：把全局设备指针存到 filp->private_data，
 * 后续 read/ioctl/release 都能通过 filp->private_data 拿回来。
 * =================================================================== */
static int icm20608_open(struct inode *inode, struct file *filp)
{
    filp->private_data = g_icm20608;
    atomic_inc(&g_icm20608->open_count);
    pr_info(ICM20608_NAME ": 已打开 (open_count=%d)\n",
            atomic_read(&g_icm20608->open_count));
    return 0;
}

/* ===================================================================
 * release — 关闭设备时调用
 *
 * 只递减计数，不释放任何硬件。传感器持续上电，下次 open 直接用。
 * =================================================================== */
static int icm20608_release(struct inode *inode, struct file *filp)
{
    atomic_dec(&g_icm20608->open_count);
    return 0;
}

/* ===================================================================
 * read — 用户 read(fd, buf, size) 时调用
 *
 * 流程：读 SPI → 拷贝数据 → 返回用户
 * 每次 read 都重新从硬件读取（不是缓存），保证数据是最新的。
 * =================================================================== */
static ssize_t icm20608_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *ppos)
{
    struct icm20608_dev *dev = filp->private_data;
    struct icm20608_full data;
    int ret;

    /* ① 从 SPI 读取原始数据 */
    ret = icm20608_read_all(dev);
    if (ret)
        return ret;

    /* ② 加锁拷贝一份（防止读取过程中数据被并发修改） */
    mutex_lock(&dev->lock);
    memcpy(&data, &dev->data, sizeof(data));
    mutex_unlock(&dev->lock);

    /* ③ 限制拷贝长度：不超过结构体大小 */
    if (count > sizeof(data))
        count = sizeof(data);

    /* ④ 拷给用户空间 */
    if (copy_to_user(buf, &data, count))
        return -EFAULT;

    return count;
}

/* ===================================================================
 * ioctl — 支持 4 条命令，每次调用都重新从硬件读取最新数据
 *
 * ICM20608_GET_ACCEL → 只返回加速度 (x,y,z)
 * ICM20608_GET_GYRO  → 只返回陀螺仪 (x,y,z)
 * ICM20608_GET_TEMP  → 只返回温度原始值
 * ICM20608_GET_ALL   → 返回全部 7 个值
 * =================================================================== */
static long icm20608_ioctl(struct file *filp, unsigned int cmd,
                           unsigned long arg)
{
    struct icm20608_dev *dev = filp->private_data;
    struct icm20608_full full;
    struct icm20608_data axis;
    int ret = 0;

    /* ① 先读硬件，拿到最新数据 */
    ret = icm20608_read_all(dev);
    if (ret)
        return ret;

    /* ② 加锁拷贝一份 */
    mutex_lock(&dev->lock);
    memcpy(&full, &dev->data, sizeof(full));
    mutex_unlock(&dev->lock);

    /* ③ 按命令分发 */
    switch (cmd) {

    case ICM20608_GET_ACCEL:
        axis.x = full.accel_x;
        axis.y = full.accel_y;
        axis.z = full.accel_z;
        if (copy_to_user((void __user *)arg, &axis, sizeof(axis)))
            ret = -EFAULT;
        break;

    case ICM20608_GET_GYRO:
        axis.x = full.gyro_x;
        axis.y = full.gyro_y;
        axis.z = full.gyro_z;
        if (copy_to_user((void __user *)arg, &axis, sizeof(axis)))
            ret = -EFAULT;
        break;

    case ICM20608_GET_TEMP:
        if (copy_to_user((void __user *)arg, &full.temp, sizeof(short)))
            ret = -EFAULT;
        break;

    case ICM20608_GET_ALL:
        if (copy_to_user((void __user *)arg, &full, sizeof(full)))
            ret = -EFAULT;
        break;

    default:
        ret = -ENOTTY;   /* 不认识的命令 */
        break;
    }

    return ret;
}

/* ====== 字符设备操作函数表 ====== */
static const struct file_operations icm20608_fops = {
    .owner          = THIS_MODULE,
    .open           = icm20608_open,
    .release        = icm20608_release,
    .read           = icm20608_read,
    .unlocked_ioctl = icm20608_ioctl,
};

/* ===================================================================
 * SPI probe — SPI 主控检测到设备树中匹配的设备时调用
 *
 * 初始化顺序（7 步）：
 *   ① 分配设备结构体
 *   ② 初始化锁和计数
 *   ③ 验证芯片身份（WHO_AM_I）
 *   ④ 唤醒芯片（退出睡眠 + 选择时钟源）
 *   ⑤ 注册字符设备 /dev/icm20608
 *   ⑥ 创建设备类和节点
 *   ⑦ 打印完成信息
 * =================================================================== */
static int icm20608_probe(struct spi_device *spi)
{
    struct icm20608_dev *dev;
    int ret;
    u8 whoami;

    dev_info(&spi->dev, "icm20608 probing (CS=%d, max_speed=%u Hz)...\n",
             spi->chip_select, spi->max_speed_hz);

    /* ① 分配设备结构体 */
    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    g_icm20608 = dev;
    dev->spi = spi;
    spi_set_drvdata(spi, dev);   /* 存到 SPI 设备，remove 时取回 */

    /* ② 初始化锁和计数 */
    mutex_init(&dev->lock);
    atomic_set(&dev->open_count, 0);

    /* ③ 验证芯片：读 WHO_AM_I 寄存器，应返回 0x98 或 0xAF */
    ret = icm20608_read_reg(dev, REG_WHO_AM_I, &whoami);
    if (ret) {
        dev_err(&spi->dev, "读取 WHO_AM_I 失败: %d\n", ret);
        return -ENODEV;
    }
    dev_info(&spi->dev, "WHO_AM_I = 0x%02x\n", whoami);

    /* ④ 唤醒芯片：退出睡眠 + 自动时钟源 */
    icm20608_write_reg(dev, REG_PWR_MGMT_1, PWR_MGMT1_CLKSEL_AUTO);
    msleep(10);   /* 等芯片稳定 */

    /* ⑤ 动态分配设备号 + 注册 cdev */
    ret = alloc_chrdev_region(&dev->dev_id, 0, DEVICE_COUNT, ICM20608_NAME);
    if (ret < 0) {
        dev_err(&spi->dev, "alloc_chrdev_region 失败: %d\n", ret);
        return ret;
    }

    cdev_init(&dev->cdev, &icm20608_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->dev_id, DEVICE_COUNT);
    if (ret) {
        dev_err(&spi->dev, "cdev_add 失败: %d\n", ret);
        goto err_cdev;
    }

    /* ⑥ 创建设备类 + 设备节点 */
    dev->class = class_create(THIS_MODULE, ICM20608_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        dev_err(&spi->dev, "class_create 失败: %d\n", ret);
        goto err_class;
    }

    dev->device = device_create(dev->class, &spi->dev,
                                dev->dev_id, NULL, ICM20608_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        goto err_device;
    }

    dev_info(&spi->dev, "icm20608 初始化成功 (major=%d)\n",
             MAJOR(dev->dev_id));
    return 0;

    /* 级联清理（与 probe 顺序相反） */
err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);
    return ret;
}

/* ===================================================================
 * SPI remove — 驱动卸载或设备移除时调用
 *
 * 顺序和 probe 严格相反：先销毁设备节点，再删 cdev，最后归还设备号。
 * 退出前让芯片进入睡眠以省电。
 * =================================================================== */
static int icm20608_remove(struct spi_device *spi)
{
    struct icm20608_dev *dev = spi_get_drvdata(spi);

    /* 让芯片进入睡眠模式 */
    icm20608_write_reg(dev, REG_PWR_MGMT_1, PWR_MGMT1_SLEEP);

    device_destroy(dev->class, dev->dev_id);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);

    dev_info(&spi->dev, "icm20608 已卸载\n");
    return 0;
}

/* ====== 设备树匹配表 ====== */
static const struct of_device_id icm20608_of_match[] = {
    { .compatible = "smartmonitor,icm20608" },
    { .compatible = "invensense,icm20608" },     /* 也匹配主线内核 */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

/* ====== 传统 SPI 设备 ID 表（非设备树匹配的回退方案） ====== */
static const struct spi_device_id icm20608_id[] = {
    { "icm20608", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

/* ====== SPI 驱动结构体 ======
 *
 * 与 platform_driver (comp_drv)、i2c_driver (ap3216c) 的区别：
 *   - probe 签名为 probe(struct spi_device *spi)
 *   - remove 签名为 remove(struct spi_device *spi)
 *   - 必须有 .id_table（传统匹配）和 .of_match_table（设备树匹配）双保险
 */
static struct spi_driver icm20608_spi_driver = {
    .probe    = icm20608_probe,
    .remove   = icm20608_remove,
    .id_table = icm20608_id,
    .driver   = {
        .name           = ICM20608_NAME,
        .of_match_table = icm20608_of_match,
        .owner          = THIS_MODULE,
    },
};

module_spi_driver(icm20608_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("ICM20608 6轴 SPI 传感器驱动 (Ch62)");
