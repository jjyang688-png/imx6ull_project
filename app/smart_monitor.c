/**
   * smart_monitor.c — 智能环境监测终端统一监控守护进程 (Phase 8)
   *
   * 功能：
   *   - epoll 同时监听 5 个设备 + 标准输入
   *   - UART 命令解析引擎（STATUS / SENSOR / LED / HELP / RESET）
   *   - 实时传感器数据显示
   *   - CSV 日志记录
   *   - 键盘交互控制
   *
   * 设备文件：
   *   /dev/comp_drv       LED 控制
   *   /dev/input/eventX   按键输入
   *   /dev/ap3216c        光感/接近传感器
   *   /dev/icm20608       6 轴姿态传感器
   *   /dev/uart_sensor    UART 命令控制台
   *
   * 用法：
   *   ./smart_monitor [选项]
   *     --no-log    不记录 CSV 日志
   *     --interval N  传感器轮询间隔（秒，默认 2）
   *     --help      显示帮助
   */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <stdarg.h>     /* va_list, va_start, va_end, vprintf */

/* ===== 设备路径 ===== */
#define DEV_COMP_DRV    "/dev/comp_drv"
#define DEV_KEY_INPUT   "/dev/input/event0"   
/*可能需要根据实际系统调整 */
#define DEV_AP3216C     "/dev/ap3216c"
#define DEV_ICM20608    "/dev/icm20608"
#define DEV_UART_SENSOR "/dev/uart_sensor"

#define MAX_EVENTS      16      /* epoll每次最多返回的事件数 */
#define MAX_DEVICES     6       /* 5 设备 + 1 stdin*/
#define CMD_BUF_SIZE    256     /* UART命令缓冲区大小 */
#define RESP_BUF_SIZE   512     /* 响应缓冲区大小*/
#define CSV_LOG_FILE    "smart_monitor.csv"

/* =====直接引用驱动头文件（保证结构体定义一致）===== */
#include "../driver/comp_drv.h"     /* struct comp_drv_status, enum led_state, ioctl 命令码 */
#include "../driver/icm20608.h"     /* struct icm20608_full, struct icm20608_data, ioctl 命令码*/

/* ===== AP3216C 数据结构（与 ap3216c驱动保持一致）===== */
struct ap3216c_data {
    unsigned short ir;
    unsigned short als;
    unsigned short ps;
};

/* ===== UART 命令表 ===== */
typedef void (*cmd_handler_t)(int uart_fd, const char *args);

struct uart_command {
    const char *name;
    cmd_handler_t handler;
    const char *help;
};

/* ===== 全局状态 ===== */
static int g_running = 1;               /*主循环控制标志 */
static FILE *g_csv_fp = NULL;           /* CSV日志文件 */
static int g_log_enabled = 1;           /*是否记录日志 */
static int g_poll_interval = 2;         /*传感器轮询间隔（秒） */
static int g_device_fds[MAX_DEVICES];   /* 设备 fd数组，[-1]=未打开 */

/* ===================================================================
   * 辅助函数
   * =================================================================== */
/* 带时间戳的打印（替代裸 printf，每条输出自动加时间前缀） */
static void log_msg(const char *fmt, ...)
{
    va_list args;
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    printf("[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    va_start(args, fmt);
    vprintf(fmt, args);//实现printf功能的核心函数，接受可变参数列表并格式化输出
    va_end(args);   
    printf("\n");
}
 /* 写入 CSV 日志文件 */
static void csv_log(const char *event, const char *detail)
{
    if (!g_log_enabled || !g_csv_fp) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    fprintf(g_csv_fp, "%04d-%02d-%02d %02d:%02d:%02d,%s,%s\n",
            tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
            tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
            event, detail);
    fflush(g_csv_fp); //确保数据及时写入文件
}

/* 获取系统运行时间（秒），读 /proc/uptime第一个字段 */
static long get_uptime(void)
{
    FILE *fp = fopen("/proc/uptime", "r");
    long uptime = 0;
    if (fp) {
        (void)fscanf(fp, "%ld", &uptime);
        fclose(fp);
    }

    return uptime;
}
/* 检查驱动是否已加载（通过判断设备文件是否存在）*/
static int driver_loaded(const char *dev_path)
{
    return (access(dev_path, F_OK) == 0) ? 1 : 0;
}
/* 统计已加载的传感器驱动数量 */
static int count_sensors(void)
{
    int count = 0;
    if (driver_loaded(DEV_AP3216C)) count++;
    if (driver_loaded(DEV_ICM20608)) count++;
    return count;
}

/* ===================================================================
   * UART 命令处理函数（每个命令对应一个）
   *
   * 参数：
   *   uart_fd  — /dev/uart_sensor 的 fd，回复通过write(uart_fd, ...) 发送
   *   args     — 命令参数（如 "LED ON" 中的"ON"），NULL 表示无参数
   * =================================================================== */
static void cmd_status(int uart_fd, const char *args)
{
    long uptime = get_uptime();
    int sensors = count_sensors();
    char resp[RESP_BUF_SIZE];

    snprintf(resp, sizeof(resp), "[STATUS] Uptime: %ld sec, Sensors Loaded: %d", uptime, sensors);

    (void)write(uart_fd, resp, strlen(resp));
    log_msg("UART CMD: STATUS -> %s", resp);
    csv_log("CMD", "STATUS");
}
static void cmd_sensor(int uart_fd, const char *args)
{
    char resp[RESP_BUF_SIZE];
    struct ap3216c_data ap;
    struct icm20608_full icm;

    /*
    * 尝试读取传感器数据。
    * 传感器驱动可能未加载，此时 fd 为-1，跳过读取。
    * 每个传感器用非阻塞模式打开，无数据时立即返回状态。
    */
    snprintf(resp, sizeof(resp), "[SENSOR] ");
    int fd_ap = open(DEV_AP3216C, O_RDONLY);
    if (fd_ap >= 0) {
        if (read(fd_ap, &ap, sizeof(ap)) == sizeof(ap))
            snprintf(resp + strlen(resp) , sizeof(resp) - strlen(resp),
                    "ir=%u als=%u lux ps=%u  ", ap.ir, ap.als, ap.ps);
        close(fd_ap);
    }

    int fd_icm = open(DEV_ICM20608, O_RDONLY);
    if (fd_icm >= 0) {
        if (read(fd_icm, &icm, sizeof(icm)) == sizeof(icm)){
            float temp_c = (float)icm.temp / 340.0f + 36.53f;
            snprintf(resp + strlen(resp) , sizeof(resp) - strlen(resp),
                    "accel=(%d,%d,%d) gyro=(%d,%d,%d) temp=%.2fC",
                    icm.accel_x, icm.accel_y, icm.accel_z,
                    icm.gyro_x, icm.gyro_y, icm.gyro_z,
                    temp_c);
        }
        close(fd_icm);
    }
    strcat(resp, "\r\n");
    (void)write(uart_fd, resp, strlen(resp));
    log_msg("UART CMD: SENSOR -> %s", resp);
    csv_log("CMD", "SENSOR");
}

static void cmd_led(int uart_fd, const char *args)
{
    char resp[RESP_BUF_SIZE];
    const char *led_cmd = NULL;
    const char *led_msg  = NULL;

    if (args == NULL || strlen(args) == 0) {
        snprintf(resp, sizeof(resp), "[LED] usage:LED ON or LED OFF\r\n");
        goto send;
    }

    if (strcasecmp(args, "ON") == 0) {
        led_cmd = "on";
        led_msg = "LED turned ON";
    } else if (strcasecmp(args, "OFF") == 0) {
        led_cmd = "off";
        led_msg = "LED turned OFF";
    } else {
        snprintf(resp, sizeof(resp), "[LED] invalid argument: %s\r\n", args);
        goto send;
    }

    /* 打开 LED 设备 → 写入命令 → 关闭（每次操作独立打开，避免长期占用） */
    int fd_led = open(DEV_COMP_DRV, O_WRONLY);
    if (fd_led < 0) {
        snprintf(resp, sizeof(resp), "[LED] LED driver not loaded\r\n");
        goto send;
    }

    (void)write(fd_led, led_cmd, strlen(led_cmd));
    close(fd_led);

    snprintf(resp, sizeof(resp), "[LED] %s\r\n" , led_msg);

send:
    (void)write(uart_fd, resp, strlen(resp));
    log_msg("UART CMD: LED %s -> %s", args ? args : "(null)", resp);
    csv_log("CMD", led_msg ? led_msg : "LED_ERROR");
}


static void cmd_help(int uart_fd, const char *args)
{
    const char *help_text =
        "\r\n"
        "=== Smart Environment Monitor ===\r\n"
        "Commands:\r\n"
        "  STATUS          Show system status and uptime\r\n"
        "  SENSOR          Read all sensor data\r\n"
        "  LED ON|OFF      Control LED\r\n"
        "  HELP            Show this help message\r\n"
        "  RESET           Reset sensor state\r\n"
        "===================================\r\n";

    (void)write(uart_fd, help_text, strlen(help_text));
    log_msg("CMD HELP");
    csv_log("CMD", "HELP");
}

static void cmd_reset(int uart_fd, const char *args)
{
    /*
    * 重置：关闭 LED、刷新传感器状态。
    * 实际实现取决于传感器驱动的 ioctl 能力。
    */
    int fd_led = open(DEV_COMP_DRV, O_WRONLY);
    if (fd_led >= 0) {
        (void)write(fd_led, "off", 3); // 关闭 LED
        close(fd_led);
    }

    char resp[] = "[RESET] System reset OK\r\n";
    (void)write(uart_fd, resp, strlen(resp));
    log_msg("CMD RESET");
    csv_log("CMD", "RESET");
}

/* ===== 命令表：查表法替代 if-else 链 ===== */
static struct uart_command uart_commands[] = {
    {"STATUS", cmd_status, "Show system status"},
    {"SENSOR", cmd_sensor, "Read sensor data"},
    {"LED",    cmd_led,    "Control LED (LED ON|OFF)"},
    {"HELP",   cmd_help,   "Show help message"},
    {"RESET",  cmd_reset,  "Reset system state"},
    {NULL, NULL, NULL} // 哨兵，表示数组结束
};

/* ===== 命令分发器 =====
   *
   * 从 UART 收到的原始字符串中提取命令名和参数，查表找到处理函数并调用。
   *
   * 输入示例：
   *   "STATUS\r\n"      → cmd="STATUS"  args=NULL
   *   "LED ON\r\n"      → cmd="LED"     args="ON"
   *   "LED OFF\r\n"     → cmd="LED"     args="OFF"
   *
   * 处理流程：
   *   ① 去掉末尾 \r \n
   *   ② 按空格分割命令名和参数
   *   ③ 遍历命令表，匹配则调用 handler
   *   ④ 匹配不到返回 "Unknown command"
   */
static void handle_uart_command(int uart_fd, const char *raw)
{
    char buf[CMD_BUF_SIZE];
    char *cmd, *args = NULL;

    /* ① 拷贝一份，去掉换行符 */
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[--len] = '\0';
    }
    if (len == 0) return;   /* 空行忽略 */

    /* ② 分割命令名和参数 */
    cmd = buf;
    char *space = strchr(buf, ' ');
    if (space) {
        *space = '\0';       /* 截断命令字符串 */
        args = space + 1;    /* 参数指向空格后的内容 */
        
        /* 跳过参数前的连续空格 */
        while (*args == ' ') {
            args++;
        }
    }

    /* ③ 命令查表匹配 */
    for (int i = 0; uart_commands[i].name; i++) {
        if (strcasecmp(cmd, uart_commands[i].name) == 0) {
            /* 找到匹配命令，执行对应处理函数 */
            uart_commands[i].handler(uart_fd, args);
            return;
        }
    }

    /* ④ 未知命令 */
    char resp[128];
    snprintf(resp, sizeof(resp),"[ERROR] Unknown command: %s (typeHELP)\r\n", cmd);
    (void)write(uart_fd, resp, strlen(resp));
    log_msg("Unknown command: %s", cmd);
}

/* ====================================================================
   * 设备管理
   * =================================================================== */

  /*
   * open_all_devices — 打开全部 5 个设备文件
   *
   * 非关键设备（传感器）打开失败不致命，只打印警告。
   * 关键设备（uart_sensor）打开失败则报错退出。
   *
   * fd 数组索引约定：
   *   [0] comp_drv      [1] key_input     [2] ap3216c
   *   [3] icm20608      [4] uart_sensor   [5] stdin
   *
   * 返回值：成功打开的设备数量
   */
static int open_all_devices(void)
{
    int count = 0;
    /* 初始化全部为 -1（表示未打开） */
    for (int i = 0; i < MAX_DEVICES; i++)
        g_device_fds[i] = -1;

    /* LED 设备（可读写） */
    g_device_fds[0] = open(DEV_COMP_DRV, O_RDWR);
    if (g_device_fds[0] < 0)
        log_msg("WARN: 无法打开 %s (LED控制不可用)", DEV_COMP_DRV);
    else
        count++;

    /* 按键设备（只读，非阻塞：没按键时不等待） */
    g_device_fds[1] = open(DEV_KEY_INPUT, O_RDONLY | O_NONBLOCK);
    if (g_device_fds[1] < 0)
        log_msg("WARN: 无法打开 %s (按键输入不可用)", DEV_KEY_INPUT);
    else
        count++;

    /* AP3216C 传感器 */
    g_device_fds[2] = open(DEV_AP3216C, O_RDONLY);
    if (g_device_fds[2] < 0)
        log_msg("WARN: 无法打开 %s (AP3216C传感器不可用)", DEV_AP3216C);
    else
        count++;

    /* ICM20608 传感器 */
    g_device_fds[3] = open(DEV_ICM20608, O_RDONLY);
    if (g_device_fds[3] < 0)
        log_msg("WARN: 无法打开 %s (ICM20608传感器不可用)", DEV_ICM20608);
    else
        count++;    

    /* UART 命令控制台（v1.0 必需设备，v2.0 降级为非致命 — 需 DTB 中禁用 &uart3） */
    g_device_fds[4] = open(DEV_UART_SENSOR, O_RDWR);
    if (g_device_fds[4] < 0) {
        log_msg("WARN: 无法打开 %s (UART控制台不可用 — 请检查DTB中&uart3是否disabled)", DEV_UART_SENSOR);
        /* 非致命：继续运行，仅失去UART命令功能 */
    } else {
        count++;
    }

    /* 标准输入（只读，非阻塞） */
    g_device_fds[5] = STDIN_FILENO;
    /* 将 stdin 设为非阻塞模式 */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);   
    count++;

    log_msg("成功打开 %d/%d 个设备", count, MAX_DEVICES);
    return count;
}

/*
   * setup_epoll — 创建 epoll 实例，注册所有已打开的fd
   *
   * 返回 epoll fd，失败返回 -1。
   *
   * 每个 fd 只在 POLLIN（可读）时触发：
   *   - 传感器设备可读 → 有新的传感器数据
   *   - uart_sensor 可读   → PC 发来了命令
   *   - stdin 可读         → 用户按了键盘
   *
   * epoll_create1(0) 是 epoll_create 的现代替代，
   * 参数 0 表示没有特殊标志（EPOLL_CLOEXEC 在fork+exec 场景用）。
   */
static int setup_epoll(void)
{
    // 创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1 failed");
        return -1;
    }

    // 遍历所有设备，添加到 epoll 监听
    for (int i = 0; i < MAX_DEVICES; i++) {
        int fd = g_device_fds[i];

        // 跳过未打开的设备
        if (fd < 0)
            continue;

        struct epoll_event ev;
        ev.events   = EPOLLIN;                // 监听“有数据可读”事件
        ev.data.ptr = &g_device_fds[i];       // 保存设备 fd 指针

        // 将设备添加到 epoll 监听列表
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl add failed");
            // 添加失败不影响整体，继续添加其他设备
        }
    }

    log_msg("epoll 初始化完成 (epfd = %d)", epfd);
    return epfd;
}

/* SIGINT (Ctrl+C) 或 SIGTERM的处理函数：置标志位让主循环优雅退出 */
static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        log_msg("收到信号 %d，正在退出...", signo);
        g_running = 0;
    }
}

/*
   * cleanup — 关闭所有 fd 和 CSV 文件
   *
   * 主循环退出后调用，确保没有资源泄漏。
   */
static void cleanup(int epfd)
{
    /* 恢复 stdin 为阻塞模式 */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);

    /* 关闭 epoll */
    close(epfd);

    /* 关闭所有设备（stdin fd=0 不关，留给 shell）*/
    for (int i = 0; i < 5; i++) {
        if (g_device_fds[i] >= 0) {
            close(g_device_fds[i]);
            g_device_fds[i] = -1;
        }
    }

    /* 关闭 CSV 日志 */
    if (g_csv_fp) {
        fclose(g_csv_fp);
        log_msg("CSV 日志已保存到 %s",CSV_LOG_FILE);
    }

    log_msg("smart_monitor 已退出");
}

/* =============================================================
 * 主循环 + 事件处理函数
 * =============================================================
 */
/*
 * handle_key_event — KEY0 按键 toggle LED
 *
 * 硬件：key_input.c → /dev/input/event0，键码 KEY_ENTER(28)
 *
 * 只响应按键"按下"（value=1），忽略"释放"和长按，
 * 避免一次物理按键触发两次 toggle。
 */
static void handle_key_event(int fd)
{
    struct input_event ev;
    int n = read(fd, &ev, sizeof(ev));

    /* 只处理 KEY0：按下事件 + 键码必须匹配 */
    if (n != sizeof(ev) || ev.type != EV_KEY || ev.value != 1)
        return;
    if (ev.code != 28)   /* KEY_ENTER = 28（key_input.c 默认键码） */
        return;

    log_msg("KEY0 按下 (code=%d)", ev.code);
    csv_log("KEY", "KEY0按下");

    /* Toggle LED：static 变量跨调用保持状态 */
    static int led_is_on = 0;
    int fd_led = open(DEV_COMP_DRV, O_WRONLY);
    if (fd_led >= 0) {
        if (led_is_on) {
            (void)write(fd_led, "off", 3);
            log_msg("  → LED 关");
            csv_log("LED", "KEY0→OFF");
            led_is_on = 0;
        } else {
            (void)write(fd_led, "on", 2);
            log_msg("  → LED 开");
            csv_log("LED", "KEY0→ON");
            led_is_on = 1;
        }
        close(fd_led);
    } else {
        log_msg("  → LED 驱动未加载，无法控制");
    }
}

static void handle_sensor_read(int fd)
{
    if (fd == g_device_fds[2]) { // AP3216C
        struct ap3216c_data data;
        if (read(fd, &data, sizeof(data)) == sizeof(data)) {
            log_msg("AP3216C 数据: IR=%u ALS=%u PS=%u", data.ir, data.als, data.ps);
            char detail[128];
            snprintf(detail, sizeof(detail), "IR=%u ALS=%u PS=%u", data.ir, data.als, data.ps);
            csv_log("SENSOR_AP3216C", detail);
        }
    } else if (fd == g_device_fds[3]) { // ICM20608
        struct icm20608_full data;
         int n = read(fd, &data, sizeof(data));
        if (n == sizeof(data)) {
            float temp = (float)data.temp / 340.0f + 36.53f;
            log_msg("ICM20608: temp=%.1fC acc=(%d,%d,%d)  gyro=(%d,%d,%d)",
                    temp,
                    data.accel_x, data.accel_y,
                    data.accel_z,
                    data.gyro_x,  data.gyro_y,
                    data.gyro_z);
          }
    }
}

static void handle_uart_rx(int fd)
{
    static char line_buf[CMD_BUF_SIZE];
    static int line_pos = 0;
    char chunk[128];
    int n,i;

    n = read(fd, chunk, sizeof(chunk) - 1);
    if (n <= 0)
        return;
    for (i = 0; i < n; i++)
    {
        char c = chunk[i];
        if (c == '\n' || c == '\r') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                handle_uart_command(fd, line_buf);
                line_pos = 0;
            }
        } else {
            if (line_pos < sizeof(line_buf) - 1) {
                line_buf[line_pos++] = c;
            }
        }
    }
}
   
static void handle_stdin(int fd)
{
    char c;
    if (read(fd, &c, 1) != 1)
        return;

    switch (c) {
    case 'q':
    case 27:   /* ESC 键的 ASCII 码 */
        log_msg("用户请求退出");
        g_running = 0;
        break;

    case 's':
        cmd_status(g_device_fds[4], NULL);
        break;

    case '1':
        cmd_led(g_device_fds[4], "on");
        break;

    case '2':
        cmd_led(g_device_fds[4], "off");
        break;

    case 'h':
        log_msg("键盘: q=退出 s=状态 1=LED开 2=LED关 h=帮助");
        break;

    default:
        break;
    }
}

/*
 * handle_led_event — LED 状态变化通知
 *
 * comp_drv 在 LED 状态变化时通过 read() 返回最新状态。
 * 这里只读取并打印，不主动控制（控制走 UART 命令或按键）。
 */
static void handle_led_event(int fd)
{
    struct comp_drv_status st;
    if (read(fd, &st, sizeof(st)) > 0)
        log_msg("LED: state=%d blink=%dms r=%lu w=%lu",
                st.state, st.blink_period_ms,
                st.read_count, st.write_count);
}

/*
 * run_event_loop - epoll 主循环（整个程序的核心调度器）
 * @epfd: epoll 文件描述符
 *
 * 功能：
 *  1. 监听所有硬件设备事件（按键、传感器、UART、键盘、LED）
 *  2. 有事件时，分发给对应处理函数
 *  3. 定时轮询读取传感器（保证周期性更新数据）
 */
static void run_event_loop(int epfd)
{
    struct epoll_event events[MAX_EVENTS];
    time_t last_sensor_read = 0;

    log_msg("进入主循环，等待事件...");
    log_msg("按键: q=退出 s=状态 1=LED开 2=LED关 h=帮助");

    // 主循环：g_running = 0 时退出程序
    while (g_running)
    {
        /* 1. 等待事件发生，最多等待 1000ms（1秒）*/
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);

        // 出错处理：被信号打断（Ctrl+C）则退出，其他错误忽略
        if (nfds < 0)
        {
            if (errno == EINTR)
                break;
            continue;
        }

        /* 2. 遍历所有触发事件的设备，逐个处理 */
        for (int i = 0; i < nfds; i++)
        {
            int *fd_ptr = (int *)events[i].data.ptr;
            int fd      = *fd_ptr;

            if (fd == g_device_fds[0])
            {
                /* LED 设备：读取并打印状态 */
                handle_led_event(fd);
            }
            else if (fd == g_device_fds[1])
            {
                /* 开发板物理按键 */
                handle_key_event(fd);
            }
            else if (fd == g_device_fds[2] || fd == g_device_fds[3])
            {
                /* 传感器（AP3216 / ICM20608） */
                handle_sensor_read(fd);
            }
            else if (fd == g_device_fds[4])
            {
                /* UART 串口接收命令 */
                handle_uart_rx(fd);
            }
            else if (fd == g_device_fds[5])
            {
                /* 终端键盘输入（TTL/SSH） */
                handle_stdin(fd);
            }
        }

        /* 3. 定时任务：每隔 g_poll_interval 秒主动读一次传感器 */
        time_t now = time(NULL);
        if (now - last_sensor_read >= g_poll_interval)
        {
            last_sensor_read = now;

            if (g_device_fds[2] >= 0)
                handle_sensor_read(g_device_fds[2]);
            if (g_device_fds[3] >= 0)
                handle_sensor_read(g_device_fds[3]);
        }
    }
}


static void print_usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("\n");
    printf("选项:\n");
    printf("  --no-log       不记录 CSV 日志\n");
    printf("  --interval N   传感器轮询间隔（秒，默认 2）\n");
    printf("  --help         显示帮助\n");
    printf("\n");
    printf("键盘控制（程序运行中）:\n");
    printf("  q / ESC    退出\n");
    printf("  s          查询系统状态\n");
    printf("  1          LED 开\n");
    printf("  2          LED 关\n");
    printf("  h          帮助\n");
}

int main(int argc, char *argv[])
{
    int epfd;
    int dev_count;

    /* ① 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--no-log") == 0) {
            g_log_enabled = 0;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            g_poll_interval = atoi(argv[++i]);
            if (g_poll_interval < 1)
                g_poll_interval = 1;
        }
    }

    /* 打印程序启动标题 */
    printf("\n");
    printf("╔═══════════════════════════════════════╗\n");
    printf("║  Smart Environment Monitor  v1.0      ║\n");
    printf("║  i.MX6ULL 智能环境监测终端              ║\n");
    printf("╚═══════════════════════════════════════╝\n\n");

    /* ② 注册信号处理（Ctrl+C 退出） */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ③ 打开 CSV 日志文件 */
    if (g_log_enabled) {
        g_csv_fp = fopen(CSV_LOG_FILE, "a");
        if (g_csv_fp)
            log_msg("CSV 日志: %s", CSV_LOG_FILE);
        else
            log_msg("WARN: 无法创建 CSV 日志文件");
    }
    /* ④ 打开设备 */
    dev_count = open_all_devices();
    if (dev_count < 0) {
        cleanup(-1);
        return 1;
    }

    /* ⑤ 初始化 epoll 监听 */
    epfd = setup_epoll();
    if (epfd < 0) {
        cleanup(-1);
        return 1;
    }

    /* ⑥ 主循环：等待事件，处理事件 */
    run_event_loop(epfd);

    /* ⑦ 清理资源，退出 */
    cleanup(epfd);

    return 0;

}
