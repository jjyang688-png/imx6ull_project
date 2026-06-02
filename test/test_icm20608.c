/**
 * test_icm20608.c — ICM20608 SPI 传感器测试程序 (Phase 6)
 *
 * 演示通过字符设备读取 SPI 传感器数据（read + ioctl）。
 *
 * 用法:
 *   ./test_icm20608              read 方式，读 10 次全部数据
 *   ./test_icm20608 accel        ioctl 只读加速度
 *   ./test_icm20608 gyro         ioctl 只读陀螺仪
 *   ./test_icm20608 temp         ioctl 只读温度
 *   ./test_icm20608 all 5        read 方式，读 5 次
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "../driver/icm20608.h"

int main(int argc, char **argv)
{
    const char *mode  = (argc > 1) ? argv[1] : "all";
    int count = (argc > 2) ? atoi(argv[2]) : 10;
    int fd, i;

    fd = open("/dev/icm20608", O_RDONLY);
    if (fd < 0) {
        perror("打开 /dev/icm20608 失败");
        printf("请确认 icm20608 驱动已加载 (insmod icm20608.ko)\n");
        return 1;
    }

    if (strcmp(mode, "accel") == 0) {
        /* ====== ioctl 模式：只读加速度 ====== */
        printf("读取加速度 %d 次...\n", count);
        printf("%-8s %-8s %-8s\n", "AX", "AY", "AZ");
        for (i = 0; i < count; i++) {
            struct icm20608_data data;
            if (ioctl(fd, ICM20608_GET_ACCEL, &data) < 0) {
                perror("ioctl GET_ACCEL");
                break;
            }
            printf("%-8d %-8d %-8d\n", data.x, data.y, data.z);
            usleep(100000);
        }

    } else if (strcmp(mode, "gyro") == 0) {
        /* ====== ioctl 模式：只读陀螺仪 ====== */
        printf("读取陀螺仪 %d 次...\n", count);
        printf("%-8s %-8s %-8s\n", "GX", "GY", "GZ");
        for (i = 0; i < count; i++) {
            struct icm20608_data data;
            if (ioctl(fd, ICM20608_GET_GYRO, &data) < 0) {
                perror("ioctl GET_GYRO");
                break;
            }
            printf("%-8d %-8d %-8d\n", data.x, data.y, data.z);
            usleep(100000);
        }

    } else if (strcmp(mode, "temp") == 0) {
        /* ====== ioctl 模式：只读温度 ====== */
        printf("读取温度 %d 次...\n", count);
        for (i = 0; i < count; i++) {
            short temp;
            if (ioctl(fd, ICM20608_GET_TEMP, &temp) < 0) {
                perror("ioctl GET_TEMP");
                break;
            }
            /* 温度转换公式来自 ICM20608 数据手册 */
            printf("温度 = %.2f °C\n", temp / 340.0 + 36.53);
            usleep(500000);
        }

    } else {
        /* ====== read 模式：一次读全部 ====== */
        printf("读取全部传感器数据 %d 次...\n\n", count);
        printf("%-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
               "AX", "AY", "AZ", "TEMP", "GX", "GY", "GZ");
        printf("-------- -------- -------- -------- "
               "-------- -------- --------\n");

        for (i = 0; i < count; i++) {
            struct icm20608_full all;
            int n = read(fd, &all, sizeof(all));
            if (n < 0) {
                perror("read");
                break;
            }
            printf("%-8d %-8d %-8d %-8d %-8d %-8d %-8d\n",
                   all.accel_x, all.accel_y, all.accel_z,
                   all.temp,
                   all.gyro_x, all.gyro_y, all.gyro_z);
            usleep(100000);
        }
    }

    close(fd);
    return 0;
}
