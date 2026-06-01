/**
 * test_key.c — 按键输入测试程序 (Phase 5)
 *
 * 从 /dev/input/eventX 读取 input_event，
 * 演示 input 子系统的用户空间接口（阻塞 read + poll）。
 *
 * 用法: ./test_key [设备路径]
 *   默认: /dev/input/event0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <linux/input.h>       /* struct input_event, EV_KEY */

int main(int argc, char **argv)
{
    const char *dev_path = (argc > 1) ? argv[1] : "/dev/input/event0";
    struct input_event ev;
    int fd;

    fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        printf("提示: 请指定设备路径, 如 %s /dev/input/event1\n", argv[0]);
        return 1;
    }

    printf("从 %s 读取按键事件 (Ctrl+C 退出)...\n", dev_path);

    /* ====== 演示1：poll 超时等待 ====== */
    {
        struct pollfd pfd;
        pfd.fd     = fd;
        pfd.events = POLLIN;

        printf("等待按键按下 (poll, 超时 5 秒)...\n");
        int ret = poll(&pfd, 1, 5000);
        if (ret < 0) {
            perror("poll");
        } else if (ret == 0) {
            printf("5 秒内无按键，切换到阻塞 read 模式...\n");
        } else {
            printf("有按键事件就绪！\n");
        }
    }

    /* ====== 演示2：阻塞 read 循环 ====== */
    printf("阻塞 read 模式 — 按下开发板按键...\n");

    while (1) {
        int n = read(fd, &ev, sizeof(ev));
        if (n < 0) {
            perror("read");
            break;
        }
        if (n != sizeof(ev)) {
            printf("短读: %d 字节\n", n);
            continue;
        }

        /* 只关心按键事件 */
        if (ev.type == EV_KEY) {
            printf("[KEY] code=%-3d %s (time=%ld.%06ld)\n",
                   ev.code,
                   ev.value == 1 ? "按下  ▲" :
                   ev.value == 0 ? "释放  ▼" : "重复  ↻",
                   ev.time.tv_sec, ev.time.tv_usec);
        }
    }

    close(fd);
    return 0;
}
