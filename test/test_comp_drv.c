#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <errno.h>
#include "../driver/comp_drv.h"

int fd;

/* ============ 异步通知测试：SIGIO 处理函数 ============ */
void sigio_handler(int signo)
{
    struct comp_drv_status s;
    if (read(fd, &s, sizeof(s)) > 0)
        printf("  🔔 SIGIO 通知: LED=%d, blink=%dms, w=%lu, r=%lu\n",
               s.state, s.blink_period_ms, s.write_count, s.read_count);
}

/* ============ 辅助函数 ============ */
void print_sep(const char *title)
{
    printf("\n========== %s ==========\n", title);
}

void do_write(const char *cmd)
{
    if (write(fd, cmd, strlen(cmd)) < 0)
        perror("  write");
    else
        printf("  write(\"%s\") OK\n", cmd);
    usleep(100000);  /* 给驱动一点时间处理 */
}

void do_read_blocking()
{
    struct comp_drv_status s;
    printf("  阻塞 read 中 (等待数据变化)...\n");
    if (read(fd, &s, sizeof(s)) > 0)
        printf("  read 返回: LED=%d, blink=%dms, w=%lu, r=%lu\n",
               s.state, s.blink_period_ms, s.write_count, s.read_count);
}

void do_read_nonblock()
{
    struct comp_drv_status s;
    if (read(fd, &s, sizeof(s)) > 0)
        printf("  非阻塞 read: LED=%d, blink=%dms, w=%lu, r=%lu\n",
               s.state, s.blink_period_ms, s.write_count, s.read_count);
    else
        printf("  非阻塞 read: (无新数据)\n");
}

/* ============ 测试用例 ============ */

void test1_basic_read_write()
{
    print_sep("测试1: 基本读写 (Phase 3)");

    do_write("on");
    do_read_nonblock();

    do_write("off");
    do_read_nonblock();

    do_write("1");
    do_read_nonblock();

    do_write("0");
    do_read_nonblock();

    do_write("bad");  /* 非法输入，应返回 -EINVAL */
    printf("  write(\"bad\") → 预期失败 (无效参数)\n");
}

void test2_nonblocking_read()
{
    print_sep("测试2: 非阻塞读 (Phase 4)");

    /* 先确保有新数据 */
    do_write("on");

    /* 连续两次非阻塞读：第一次有数据，第二次 changed=0 仍返回当前状态 */
    printf("  第1次非阻塞 read:\n");
    do_read_nonblock();
    printf("  第2次非阻塞 read (无新变化也立即返回):\n");
    do_read_nonblock();
}

void test3_blocking_read()
{
    print_sep("测试3: 阻塞读 (Phase 4)");

    printf("  提示: 另开一个终端执行如下命令来唤醒本进程:\n");
    printf("    echo on  > /dev/comp_drv\n");
    printf("    echo off > /dev/comp_drv\n");
    printf("  或 Ctrl+C 退出阻塞\n\n");

    /* 不断阻塞等待 */
    while (1)
        do_read_blocking();
}

void test4_poll()
{
    print_sep("测试4: poll/select (Phase 4)");

    do_write("on");  /* 制造数据 */
    usleep(100000);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    printf("  poll 等待中 (超时 2000ms)...\n");
    int ret = poll(&pfd, 1, 2000);
    if (ret > 0) {
        printf("  poll 返回: ");
        if (pfd.revents & POLLIN)  printf("POLLIN ");
        if (pfd.revents & POLLOUT) printf("POLLOUT ");
        printf("\n");
        do_read_nonblock();
    } else if (ret == 0) {
        printf("  poll 超时 (2秒内无数据)\n");
    } else {
        perror("  poll");
    }
}

void test5_ioctl()
{
    print_sep("测试5: ioctl (Phase 4)");

    /* 5a. 设置闪烁周期 */
    unsigned long period = 1000;  /* 1秒 */
    if (ioctl(fd, COMP_DRV_SET_BLINK_PERIOD, &period) == 0)
        printf("  SET_BLINK_PERIOD 1000ms OK\n");

    /* 5b. 获取状态 */
    struct comp_drv_status s;
    if (ioctl(fd, COMP_DRV_GET_STATUS, &s) == 0)
        printf("  GET_STATUS: LED=%d, blink=%dms, w=%lu, r=%lu\n",
               s.state, s.blink_period_ms, s.write_count, s.read_count);

    /* 5c. 启动闪烁 */
    printf("  启动 LED 闪烁 (1s 周期)... 观察 LED 5 秒\n");
    if (ioctl(fd, COMP_DRV_START_BLINK) == 0)
        printf("  START_BLINK OK\n");
    else
        perror("  START_BLINK");

    sleep(5);

    /* 5d. 停止闪烁 */
    if (ioctl(fd, COMP_DRV_STOP_BLINK) == 0)
        printf("  STOP_BLINK OK, LED 应熄灭\n");
    else
        perror("  STOP_BLINK");

    /* 5e. 先设置周期再启动闪烁 (正常流程) */
    period = 500;
    ioctl(fd, COMP_DRV_SET_BLINK_PERIOD, &period);
    printf("  设置 500ms 周期并启动闪烁... 观察 3 秒\n");
    ioctl(fd, COMP_DRV_START_BLINK);
    sleep(3);
    ioctl(fd, COMP_DRV_STOP_BLINK);

    /* 5f. write "on" 停止闪烁 */
    printf("  闪烁中 write(\"on\") → 应停止闪烁并常亮\n");
    period = 500;
    ioctl(fd, COMP_DRV_SET_BLINK_PERIOD, &period);
    ioctl(fd, COMP_DRV_START_BLINK);
    sleep(2);
    do_write("on");
    printf("  LED 应常亮 (不再闪烁)\n");
    sleep(2);
}

void test6_fasync()
{
    print_sep("测试6: fasync 异步通知 (Phase 4)");

    /* 注册 SIGIO */
    signal(SIGIO, sigio_handler);
    fcntl(fd, F_SETOWN, getpid());
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | FASYNC);

    printf("  SIGIO 已注册, 等待异步通知...\n");
    printf("  另开终端执行: echo on > /dev/comp_drv\n");
    printf("  或 Ctrl+C 退出\n");

    while (1)
        pause();
}

int test_usage()
{
    printf("用法: %s <test_number>\n\n", "test_comp_drv");
    printf("  0  全部自动化测试 (不包含阻塞/SIGIO)\n");
    printf("  1  基本读写\n");
    printf("  2  非阻塞 read\n");
    printf("  3  阻塞 read\n");
    printf("  4  poll\n");
    printf("  5  ioctl (闪烁)\n");
    printf("  6  fasync 异步通知\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        return test_usage();

    fd = open("/dev/comp_drv", O_RDWR);
    if (fd < 0) {
        perror("无法打开 /dev/comp_drv，请先 insmod comp_drv.ko");
        return 1;
    }

    int t = atoi(argv[1]);

    switch (t) {
    case 0:
        test1_basic_read_write();
        test2_nonblocking_read();
        test4_poll();
        test5_ioctl();
        break;
    case 1: test1_basic_read_write(); break;
    case 2: test2_nonblocking_read(); break;
    case 3: test3_blocking_read();    break;
    case 4: test4_poll();             break;
    case 5: test5_ioctl();            break;
    case 6: test6_fasync();           break;
    default: test_usage();            break;
    }

    close(fd);
    printf("\n测试完成\n");
    return 0;
}
