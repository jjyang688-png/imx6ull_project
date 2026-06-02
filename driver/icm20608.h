#ifndef _ICM20608_H
#define _ICM20608_H

#include <linux/ioctl.h>

#define ICM20608_MAGIC 'I'

/* ====== 魔数 ====== */
#define ICM20608_MAGIC 'I'

/* ====== 数据结构（先定义，供后续 ioctl 宏使用） ====== */

/* 三轴数据（加速度计 / 陀螺仪共用） */
struct icm20608_data {
    short x;
    short y;
    short z;
};

/* 完整传感器数据（一次 SPI 突发读的 14 字节） */
struct icm20608_full {
    short accel_x;  /* 加速度 X 轴 */
    short accel_y;  /* 加速度 Y 轴 */
    short accel_z;  /* 加速度 Z 轴 */
    short temp;     /* 温度原始值 */
    short gyro_x;   /* 陀螺仪 X 轴 */
    short gyro_y;   /* 陀螺仪 Y 轴 */
    short gyro_z;   /* 陀螺仪 Z 轴 */
};

/* ====== ioctl 命令定义 ====== */
/* 读取加速度计三轴数据 */
#define ICM20608_GET_ACCEL  _IOR(ICM20608_MAGIC, 1, struct icm20608_data)

/* 读取陀螺仪三轴数据 */
#define ICM20608_GET_GYRO   _IOR(ICM20608_MAGIC, 2, struct icm20608_data)

/* 读取温度值 */
#define ICM20608_GET_TEMP   _IOR(ICM20608_MAGIC, 3, int)

/* 一次性读取全部 7 个数据（加速度×3 + 温度 + 陀螺仪×3） */
#define ICM20608_GET_ALL    _IOR(ICM20608_MAGIC, 4, struct icm20608_full)

#endif /* _ICM20608_H */

