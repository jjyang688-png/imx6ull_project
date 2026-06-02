/**
 * test_ap3216c.c — AP3216C I2C 传感器测试程序 (Phase 6)
 *
 * 通过 /dev/ap3216c 读取光感/接近传感器数据。
 * AP3216C 只有 read，没有 ioctl——每次 read 返回全部 3 个值。
 *
 * 用法:
 *   ./test_ap3216c        读 5 次
 *   ./test_ap3216c 10     读 10 次
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* AP3216C 数据结构（与驱动中的定义保持一致） */
struct ap3216c_data {
    unsigned short ir;    /* 红外光强度 */
    unsigned short als;   /* 环境光强度 */
    unsigned short ps;    /* 接近距离 */
};

int main(int argc, char **argv)
{
    int count = (argc > 1) ? atoi(argv[1]) : 5;
    int fd, i;

    fd = open("/dev/ap3216c", O_RDONLY);
    if (fd < 0) {
        perror("打开 /dev/ap3216c 失败");
        printf("请确认 ap3216c 驱动已加载 (insmod ap3216c.ko)\n");
        return 1;
    }

    printf("读取 AP3216C 传感器 %d 次...\n", count);
    printf("%-8s %-8s %-8s\n", "IR", "ALS", "PS");
    printf("-------- -------- --------\n");

    for (i = 0; i < count; i++) {
        struct ap3216c_data data;
        int n = read(fd, &data, sizeof(data));
        if (n < 0) {
            perror("read");
            break;
        }
        printf("%-8u %-8u %-8u\n", data.ir, data.als, data.ps);
        sleep(1);
    }

    close(fd);
    return 0;
}
