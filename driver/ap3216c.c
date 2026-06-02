#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/i2c.h>          
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#define AP3216C_NAME  "ap3216c"
#define DEVICE_COUNT  1

static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "启用调试输出");

#define DBG(dev, fmt, ...) \
    do { if (debug) dev_info(&(dev)->client->dev, fmt, ##__VA_ARGS__); } while (0)

/* ====== AP3216C 寄存器地址 ======
   *
   * 此芯片参与寄存器只有 4 个：
   *   0x00  SYS_CONF     系统配置（开关 ALS、PS、复位）
   *   0x0A~0x0B  IR      红外光（2 字节）
   *   0x0C~0x0D  ALS     环境光（2 字节）
   *   0x0E~0x0F  PS      接近距离（2 字节）
   *
   * 6 个数据寄存器从 0x0A 连续到 0x0F，可一次 I2C 读 6 字节 */
#define REG_SYS_CONF       0x00   /* 系统配置寄存器 */
#define REG_IR_DATA_LOW    0x0A   /* IR 数据低字节 */
#define REG_IR_DATA_HIGH   0x0B   /* IR 数据高字节 */
#define REG_ALS_DATA_LOW   0x0C   /* ALS 数据低字节 */
#define REG_ALS_DATA_HIGH  0x0D   /* ALS 数据高字节 */
#define REG_PS_DATA_LOW    0x0E   /* PS 数据低字节 */
#define REG_PS_DATA_HIGH   0x0F   /* PS 数据高字节 */

/* 系统配置寄存器的位定义 */
#define SYS_CONF_ALS_ON    (1 << 0)  /* bit0: 开启环境光检测 */
#define SYS_CONF_PS_ON     (1 << 1)  /* bit1: 开启接近检测 */
#define SYS_CONF_RESET     (1 << 7)  /* bit7: 软件复位 */

/* ====== 传感器数据结构（返回给用户空间） ====== */
struct ap3216c_data {
    unsigned short ir;    /* 红外光强度 */
    unsigned short als;   /* 环境光强度 (lux) */
    unsigned short ps;    /* 接近距离 */
};

struct ap3216c_dev {
    dev_t dev_id;
    struct cdev cdev;
    struct class *class;
    struct device *device;
    struct i2c_client *client;

    struct mutex lock;
    atomic_t open_count;

    struct ap3216c_data data;
};

static struct ap3216c_dev *g_ap3216c;

//单寄存器读
static int ap3216c_read_reg(struct ap3216c_dev *dev, u8 reg, u8 *val)
{
    struct i2c_client *client = dev->client;
    int ret;

    ret = i2c_smbus_read_byte_data(client, reg);
    if (ret < 0) {
        dev_err(&client->dev, "I2C 读寄存器 0x%02x 失败: %d\n", reg,
        ret);
        return ret;
    }

    *val = (u8)ret;
    return 0;
}

//单寄存器写
static int ap3216c_write_reg(struct ap3216c_dev *dev, u8 reg, u8 val)
{
    struct i2c_client *client = dev->client;
    int ret;

    ret = i2c_smbus_write_byte_data(client,reg,val);
    if (ret < 0)
        dev_err(&client->dev, "I2C 写寄存器 0x%02x 失败: %d\n", reg,
        ret);
    return ret;
}

static int ap3216c_read_data(struct ap3216c_dev *dev)
{  
    u8 reg_addr = REG_IR_DATA_LOW;    /* 起始寄存器地址 0x0A */
    u8 buf[6];                         /* 接收 6 字节 IR+ALS+PS */
    int ret;

    /* I2C 底层传输：两条消息串在一起，中间不释放总线 */
    struct i2c_msg msgs[2] = {
        {
            .addr   = dev->client->addr,  /* I2C 从设备地址 = 0x1E */
            .flags  = 0,                  /* 0 = 写操作（不发 STOP 前是写） */
            .len    = 1,                  /* 先写 1 字节… */
            .buf    = &reg_addr,          /* …内容是寄存器地址 0x0A */
        },
        {
            .addr   = dev->client->addr,  /* 同一个设备地址 */
            .flags  = I2C_M_RD,           /* 读操作 */
            .len    = 6,                  /* 再读 6 字节 */
            .buf    = buf,                /* 数据存到这里 */
        },
    };

    ret = i2c_transfer(dev->client->adapter, msgs, 2);
    if (ret < 0) {
        dev_err(&dev->client->dev, "i2c_transfer 失败: %d\n", ret);
        return ret;
    }

    /* 小端序解析（AP3216C 是 LSB 在前，和 ICM20608 相反） */
    mutex_lock(&dev->lock);
    dev->data.ir  = ((u16)buf[1] << 8) | buf[0];   /* buf[0]=低字节 */
    dev->data.als = ((u16)buf[3] << 8) | buf[2];
    dev->data.ps  = ((u16)buf[5] << 8) | buf[4];
    mutex_unlock(&dev->lock);

    DBG(dev, "IR=%u ALS=%u PS=%u\n", dev->data.ir, dev->data.als,dev->data.ps);
    return 0;
}


static int ap3216c_open(struct inode *inode, struct file *filp)
{  
    filp->private_data = g_ap3216c;
    atomic_inc(&g_ap3216c->open_count);
    return 0;
}

static int ap3216c_release(struct inode *inode, struct file *filp)
{
    atomic_dec(&g_ap3216c->open_count);
    return 0;
}

static ssize_t ap3216c_read(struct file *filp, char __user *buf,size_t count, loff_t *ppos)
{
    struct ap3216c_dev *dev = filp->private_data;
    struct ap3216c_data data;
    int ret;

    /* ① 从 I2C 传感器读取原始数据 */
    ret = ap3216c_read_data(dev);
    if (ret)
        return ret;

    /* ② 加锁拷贝一份（防止并发修改） */
    mutex_lock(&dev->lock);
    memcpy(&data, &dev->data, sizeof(data));
    mutex_unlock(&dev->lock);

    /* ③ 限制长度不超过结构体大小 */
    if (count > sizeof(data))
        count = sizeof(data);

    /* ④ 拷给用户空间 */
    if (copy_to_user(buf, &data, count))
        return -EFAULT;

    return count;
}

static const struct file_operations ap3216c_fops = {
    .owner   = THIS_MODULE,
    .open    = ap3216c_open,
    .release = ap3216c_release,
    .read    = ap3216c_read,
};

static int ap3216c_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
    struct ap3216c_dev *dev;
    int ret;
    u8 val;

    dev_info(&client->dev, "ap3216c probing at addr 0x%02x...\n",client->addr);

    /* ① 验证芯片存在：读 SYS_CONF 寄存器 */
    ret = i2c_smbus_read_byte_data(client, REG_SYS_CONF);
    if (ret < 0) {
        dev_err(&client->dev, "读取芯片失败: %d\n", ret);
        return -ENODEV;
    }

    /* ② 分配设备结构体 */
    dev = devm_kzalloc(&client->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    g_ap3216c = dev;
    dev->client = client;
    i2c_set_clientdata(client, dev);

    mutex_init(&dev->lock);
    atomic_set(&dev->open_count, 0);

    ap3216c_write_reg(dev, REG_SYS_CONF,SYS_CONF_ALS_ON | SYS_CONF_PS_ON);
    msleep(10);

    /* ④ 验证配置写入成功 */
    ret = ap3216c_read_reg(dev, REG_SYS_CONF, &val);
    if (ret) {
        dev_err(&client->dev, "验证读取失败\n");
        return ret;
    }
    dev_info(&client->dev, "SYS_CONF = 0x%02x\n", val);


    ret = alloc_chrdev_region(&dev->dev_id, 0, DEVICE_COUNT,AP3216C_NAME);
    if (ret < 0) {
        dev_err(&client->dev, "alloc_chrdev_region: %d\n", ret);
        return ret;
    }

    cdev_init(&dev->cdev, &ap3216c_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->dev_id, DEVICE_COUNT);
    if (ret) {
        dev_err(&client->dev, "cdev_add: %d\n", ret);
        goto err_cdev;
    }

    dev->class = class_create(THIS_MODULE, AP3216C_NAME);
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_class;
    }

    dev->device = device_create(dev->class, &client->dev,dev->dev_id, NULL, AP3216C_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        goto err_device;
    }

    dev_info(&client->dev, "ap3216c 初始化成功 (major=%d)\n",MAJOR(dev->dev_id));
    
    return 0;

    /* 级联清理 */
err_device:
    class_destroy(dev->class);
err_class:
    cdev_del(&dev->cdev);
err_cdev:
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);
    return ret;
}

static int ap3216c_remove(struct i2c_client *client)
{
    struct ap3216c_dev *dev = i2c_get_clientdata(client);

    /* 关闭传感器功能（写入 0 = 禁用 ALS + PS） */
    ap3216c_write_reg(dev, REG_SYS_CONF, 0);

    device_destroy(dev->class, dev->dev_id);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->dev_id, DEVICE_COUNT);

    dev_info(&client->dev, "ap3216c 已卸载\n");
    return 0;
}

/* ====== 设备树匹配表 ====== */
static const struct of_device_id ap3216c_of_match[] = {
    { .compatible = "smartmonitor,ap3216c" },
    { .compatible = "liteon,ap3216c" },           /* 主线内核兼容 */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

/* ====== 传统 I2C 设备 ID 表 ====== */
static const struct i2c_device_id ap3216c_id[] = {
    { "ap3216c", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

/* ====== I2C 驱动结构体 ======
*
* 和 SPI 驱动的区别：
*   .probe 签名：probe(client, id) vs probe(spi)
*   .remove 签名：remove(client) vs remove(spi)
*   .id_table 类型：i2c_device_id vs spi_device_id
*/
static struct i2c_driver ap3216c_driver = {
    .probe    = ap3216c_probe,
    .remove   = ap3216c_remove,
    .id_table = ap3216c_id,
    .driver   = {
        .name           = AP3216C_NAME,
        .of_match_table = ap3216c_of_match,
        .owner          = THIS_MODULE,
      },
};

module_i2c_driver(ap3216c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jjyang688");
MODULE_DESCRIPTION("AP3216C I2C 传感器驱动 (Ch61)");