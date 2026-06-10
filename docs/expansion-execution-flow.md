# 智能环境监测边缘节点 v2.0 — 完整执行流程

> 从系统上电到 Web 仪表盘显示数据，全链路追踪。

---

## 目录

1. [系统启动](#1-系统启动)
2. [驱动加载](#2-驱动加载)
3. [smart_edge 守护进程启动](#3-smart_edge-守护进程启动)
4. [主线程：epoll 事件循环](#4-主线程epoll-事件循环)
5. [定时器线程：传感器采集 + 存储 + 告警](#5-定时器线程传感器采集--存储--告警)
6. [MQTT 线程：云端上报 + 远程控制](#6-mqtt-线程云端上报--远程控制)
7. [HTTP 线程：Web 仪表盘](#7-http-线程web-仪表盘)
8. [CAN 线程：工业总线通信](#8-can-线程工业总线通信)
9. [UART 命令引擎](#9-uart-命令引擎)
10. [关机退出](#10-关机退出)
11. [关键数据流](#11-关键数据流)
12. [时序总览](#12-时序总览)

---

## 1. 系统启动

```
┌─────────────────────────────────────────────────────────────┐
│  上电                                                        │
│    │                                                         │
│    ▼                                                         │
│  Boot ROM (芯片出厂固化)                                       │
│    │  读取 BOOT_MODE 引脚 → 选择启动介质 (eMMC)                │
│    ▼                                                         │
│  U-Boot (SPL → u-boot.img)                                   │
│    │  初始化 DDR / 时钟 / eMMC                                │
│    │  加载 DTB + zImage 到内存                                │
│    │  bootz 0x80800000 - 0x83000000                          │
│    ▼                                                         │
│  Linux Kernel 4.1.15                                          │
│    │  解压 → 架构初始化 → 外设时钟 → 驱动模型                  │
│    │  start_kernel() → rest_init() → kernel_init()            │
│    │  → do_initcalls()  ← 这里加载所有内核模块                 │
│    ▼                                                         │
│  /sbin/init (BusyBox)                                         │
│    │  挂载 /proc /sys /dev /tmp                               │
│    │  启动 syslogd / klogd                                    │
│    ▼                                                         │
│  systemd 启动服务                                              │
│    │  smart_edge.service → 开机自启动                          │
│    ▼                                                         │
│  smart_edge 守护进程运行                                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 驱动加载

```
do_initcalls()
  │
  ├── arch_initcall
  │     GPIO / I2C / SPI / UART / ADC / PWM / CAN 控制器驱动
  │     (内核自带, 使能后自动加载)
  │
  ├── module_init(各驱动)
  │
  │   ┌─ comp_drv_init()        [有] LED
  │   │    platform_driver_register → probe:
  │   │      gpiod_get("led")           ← 从 DTS 拿 GPIO
  │   │      cdev_init + cdev_add       ← 注册 /dev/comp_drv
  │   │      class_create + device_create ← /sys/class/comp_drv/
  │   │      debugfs_create_file        ← /sys/kernel/debug/comp_drv/status
  │   │      init_waitqueue_head(&wq)   ← 等待队列
  │   │      init_timer()               ← 闪烁定时器
  │   │
  │   ├─ key_input_init()        [有] KEY0
  │   │    platform_driver_register → probe:
  │   │      gpiod_get("key")          ← 从 DTS 拿 GPIO
  │   │      request_irq()             ← 注册中断 key_irq_handler
  │   │      input_register_device()   ← 注册到 input 子系统
  │   │      INIT_DELAYED_WORK()       ← 20ms 消抖
  │   │
  │   ├─ ap3216c_init()          [有] I2C 光感
  │   │    i2c_add_driver → probe:
  │   │      i2c_smbus_read_byte_data() ← 读芯片 ID 验证
  │   │      cdev_init + cdev_add       ← 注册 /dev/ap3216c
  │   │
  │   ├─ icm20608_init()         [有] SPI 6轴
  │   │    spi_register_driver → probe:
  │   │      spi_setup()              ← 配置 SPI 模式/速率
  │   │      spi_write_then_read()    ← 读 WHO_AM_I 验证
  │   │      cdev_init + cdev_add     ← 注册 /dev/icm20608
  │   │
  │   ├─ uart_sensor_init()      [有] UART3 裸寄存器
  │   │    platform_driver_register → probe:
  │   │      devm_ioremap_resource()  ← 映射 0x021EC000
  │   │      devm_request_irq()       ← 注册 uart_rx_interrupt
  │   │      INIT_KFIFO(rx_fifo)      ← 初始化接收缓冲
  │   │      init_waitqueue_head()    ← 初始化 rx_wq
  │   │      cdev_init + cdev_add     ← 注册 /dev/uart_sensor
  │   │
  │   ├─ dht11_init()            [A新] ★ 单总线 温湿度
  │   │    misc_register()            ← 更简洁: /dev/dht11 + sysfs
  │   │    gpiod_get("data")          ← 拿数据引脚
  │   │    device_create_file()       ← sysfs: temp, hum 属性
  │   │
  │   ├─ sr04_init()             [A新] ★ 超声波
  │   │    misc_register()            ← /dev/sr04
  │   │    gpiod_get("trig")          ← 触发引脚 (输出)
  │   │    gpiod_get("echo")          ← 回波引脚 (输入)
  │   │    request_irq(echo_irq)      ← 注册 ECHO 中断
  │   │    init_completion()          ← 完成量
  │   │
  │   ├─ mq135_adc_init()        [A新] ★ ADC 空气质量
  │   │    misc_register()            ← /dev/mq135
  │   │    ioremap(ADC_BASE)          ← 映射 ADC 寄存器
  │   │    kthread_run(adc_thread)    ← 内核线程定期采集
  │   │    device_create_file()       ← sysfs: raw, quality
  │   │
  │   ├─ servo_pwm_init()        [A新] ★ PWM 舵机
  │   │    misc_register()            ← /dev/servo
  │   │    ioremap(PWM_BASE)          ← 映射 PWM 寄存器
  │   │    配置周期 20ms (50Hz)       ← 舵机标准周期
  │   │
  │   ├─ rtc_drv_init()          [A新] ★ RTC
  │   │    misc_register()            ← /dev/rtc_custom
  │   │    ioremap(SNVS_BASE)         ← 映射 RTC 寄存器
  │   │
  │   ├─ wdt_drv_init()          [A新] ★ Watchdog
  │   │    misc_register()            ← /dev/wdt_custom
  │   │    ioremap(WDOG_BASE)         ← 映射 WDT 寄存器
  │   │    设置超时 10 秒
  │   │
  │   ├─ relay_init()            [A新] ★ 继电器
  │   │    misc_register()            ← /dev/relay
  │   │    gpiod_get("relay")         ← 拿 GPIO
  │   │
  │   ├─ can_drv_init()          [B新] ★ SocketCAN
  │   │    内核 flexcan 驱动已在设备树使能
  │   │    本模块是应用层封装 cdev
  │   │    cdev_init → /dev/can_ctrl
  │   │
  │   └─ modbus_rtu 是纯应用层协议栈, 不需要驱动加载
  │
  └── 驱动加载完成
         /dev 目录下出现 15+ 个设备节点
         /sys/class/misc/ 下有 8+ 个传感器目录
```

### 设备节点清单

```
/dev/comp_drv      LED 控制      [有]
/dev/input/event0  按键输入      [有]
/dev/ap3216c       I2C 光感     [有]
/dev/icm20608      SPI 6轴      [有]
/dev/uart_sensor   UART 控制台   [有]
/dev/dht11         温湿度        [A新]
/dev/sr04          超声波        [A新]
/dev/mq135         空气质量      [A新]
/dev/servo         舵机          [A新]
/dev/rtc_custom    时钟          [A新]
/dev/wdt_custom    看门狗        [A新]
/dev/relay         继电器        [A新]
/dev/can_ctrl      CAN 总线      [B新]
```

---

## 3. smart_edge 守护进程启动

```
systemd 启动 smart_edge
  │
  ├── 1. 读取配置文件
  │       parse_ini("/etc/smart_edge/config.ini")
  │       填充全局配置结构体 g_config
  │
  ├── 2. 初始化日志系统
  │       log_init(g_config.log_file, g_config.log_level)
  │       LOG_INFO("smart_edge v2.0 启动中...")
  │
  ├── 3. 初始化 SQLite
  │       sqlite3_open("/var/lib/smart_edge/data.db", &db)
  │       创建表 (sensor_data, events)
  │       清理过期数据 (>30天)
  │
  ├── 4. 初始化 POSIX 消息队列
  │       cmd_mq = mq_open("/smart_edge_cmd", O_CREAT|O_RDWR)
  │
  ├── 5. 初始化互斥锁
  │       pthread_mutex_init(&sensor_mutex, NULL)
  │       pthread_mutex_init(&config_mutex, NULL)
  │
  ├── 6. 初始化传感器共享数据结构
  │       memset(&g_sensors, 0, sizeof(g_sensors))
  │
  ├── 7. 初始化环形缓冲 (最近 100 条记录)
  │       ringbuf_init(&g_recent_data, 100)
  │
  ├── 8. 打开所有设备文件 (13 个 fd)
  │       for i in 0..12:
  │           g_device_fds[i] = open(dev_paths[i], O_RDWR|O_NONBLOCK)
  │       非关键设备打开失败不致命
  │
  ├── 9. 启动子线程
  │       pthread_create(&tid_mqtt,  NULL, mqtt_thread,  NULL)
  │       pthread_create(&tid_http,  NULL, http_thread,  NULL)
  │       pthread_create(&tid_timer, NULL, timer_thread, NULL)
  │       pthread_create(&tid_can,   NULL, can_thread,   NULL)
  │
  ├── 10. 注册信号处理
  │        signal(SIGINT,  signal_handler)
  │        signal(SIGTERM, signal_handler)
  │
  ├── 11. 初始化 epoll
  │        epfd = epoll_create1(0)
  │        for i in 0..12:
  │            epoll_ctl(epfd, EPOLL_CTL_ADD, g_device_fds[i], EPOLLIN)
  │        epoll_ctl(epfd, EPOLL_CTL_ADD, STDIN_FILENO, EPOLLIN)
  │
  ├── 12. 喂狗 (告知系统程序已就绪)
  │        write(wdt_fd, "feed", 4)
  │
  └── 13. 进入主循环
           LOG_INFO("smart_edge 启动完成, 进入主循环")
           run_main_loop(epfd)
```

---

## 4. 主线程：epoll 事件循环

```
void run_main_loop(int epfd)
{
    struct epoll_event events[32];  // 最多同时返回 32 个事件
    time_t last_wdt_feed = time(NULL);

    while (g_running)
    {
        // ── 步骤1: 等待事件 ──
        int nfds = epoll_wait(epfd, events, 32, 1000);
        //           阻塞等待                               ↑ 超时 1s

        if (nfds < 0) {
            if (errno == EINTR) break;  // 信号中断, 退出
            continue;
        }

        // ── 步骤2: 遍历就绪的 fd ──
        for (int i = 0; i < nfds; i++)
        {
            int fd = *(int *)events[i].data.ptr;

            if      (fd == g_dev[COMP_DRV])   handle_led(fd);
            else if (fd == g_dev[KEY_INPUT])  handle_key(fd);
            else if (fd == g_dev[UART])       handle_uart_rx(fd);
            else if (fd == g_dev[STDIN])      handle_stdin(fd);
            // 注: DHT11/SR04/MQ135 等传感器由 timer 线程主动轮询
            //     不依赖 epoll 事件触发
        }

        // ── 步骤3: 喂狗 (每 5 秒) ──
        if (time(NULL) - last_wdt_feed >= 5) {
            write(g_dev[WDT], "feed", 4);
            last_wdt_feed = time(NULL);
        }
    }

    // ── 退出清理 ──
    LOG_INFO("主循环退出, 开始清理...");
    g_running = 0;  // 通知所有子线程退出
    pthread_join(tid_mqtt,  NULL);
    pthread_join(tid_http,  NULL);
    pthread_join(tid_timer, NULL);
    pthread_join(tid_can,   NULL);
    cleanup(epfd);
}
```

### 主线程的事件分发表

```
事件源                  触发条件                 处理函数              动作
────────               ────────                 ────────              ────
/dev/comp_drv          LED 状态改变             handle_led()          读状态 + 打印
/dev/input/event0      KEY0 按下                 handle_key()          toggle LED
/dev/uart_sensor       PC 发来命令               handle_uart_rx()     解析命令 + 回复
stdin                   用户键盘输入              handle_stdin()       q=退出/s=状态/...
/dev/dht11              (由 timer 线程处理)      ─                    ─
/dev/sr04               (由 timer 线程处理)      ─                    ─
/dev/mq135              (由 timer 线程处理)      ─                    ─
```

---

## 5. 定时器线程：传感器采集 + 存储 + 告警

```
void *timer_thread(void *arg)
{
    struct timespec ts;

    while (g_running)
    {
        // ── 每 2 秒执行一次 ──
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += g_config.poll_interval;  // 默认 2s
        // (使用 sem_timedwait 或简单的 sleep 实现周期)

        // ── ===== 1. 主动读取所有轮询型传感器 =====
        pthread_mutex_lock(&sensor_mutex);

        // DHT11 温湿度
        if (g_dev[DHT11] >= 0) {
            read(g_dev[DHT11], dht11_buf, 4);
            g_sensors.temperature = dht11_buf[2] + dht11_buf[3]*0.01;
            g_sensors.humidity    = dht11_buf[0] + dht11_buf[1]*0.01;
        }

        // AP3216C 光照/接近
        if (g_dev[AP3216C] >= 0) {
            read(g_dev[AP3216C], &ap_data, sizeof(ap_data));
            g_sensors.light     = ap_data.als;
            g_sensors.proximity = ap_data.ps;
        }

        // ICM20608 6 轴
        if (g_dev[ICM20608] >= 0) {
            read(g_dev[ICM20608], &imu_data, sizeof(imu_data));
            memcpy(&g_sensors.imu, &imu_data, sizeof(imu_data));
        }

        // SR04 超声波
        if (g_dev[SR04] >= 0) {
            read(g_dev[SR04], &dist, sizeof(dist));
            g_sensors.distance_cm = dist;
        }

        // MQ135 空气质量
        if (g_dev[MQ135] >= 0) {
            read(g_dev[MQ135], &adc_val, sizeof(adc_val));
            g_sensors.air_quality_raw = adc_val;
            g_sensors.air_quality_level = classify_air(adc_val);
        }

        g_sensors.last_update = time(NULL);
        pthread_mutex_unlock(&sensor_mutex);

        // ── ===== 2. 写入环形缓冲 =====
        ringbuf_push(&g_recent_data, &g_sensors);

        // ── ===== 3. 写入 SQLite =====
        sqlite_log_sensors(db, &g_sensors);

        // ── ===== 4. 检查告警阈值 =====
        check_alerts(&g_sensors);

        // ── ===== 5. 处理消息队列中的命令 =====
        check_cmd_queue();
    }
    return NULL;
}
```

### 告警检查逻辑

```
check_alerts(sensors):
  ├── temperature > 35°C  (连续3次)  → alert_high_temp()
  ├── humidity > 90%      (连续3次)  → alert_high_humidity()
  ├── air_quality >= 4    (连续3次)  → alert_bad_air()
  ├── distance < 30cm     (连续2次)  → alert_proximity()
  └── 传感器连续5次读失败           → alert_sensor_offline()

alert_xxx():
  1. 写事件到 SQLite events 表
  2. 通过消息队列通知 mqtt 线程 (紧急上报)
  3. 通过消息队列通知 http 线程 (Web 推送)
  4. 如果启用邮件, 调用 send_email_alert()
  5. 同类型告警 5 分钟内静默 (不重复发送)
```

---

## 6. MQTT 线程：云端上报 + 远程控制

```
void *mqtt_thread(void *arg)
{
    // ── 1. 初始化 MQTT 客户端 ──
    //     从 config.ini 读取 broker / port / client_id
    mqtt_client_t client;
    mqtt_init(&client, g_config.mqtt.broker, g_config.mqtt.port);

    // ── 2. 连接 + 订阅控制主题 ──
    mqtt_connect(&client, g_config.mqtt.client_id);
    mqtt_subscribe(&client, "edge/room1/cmd/#", 1);  // QoS 1

    time_t last_publish = 0;

    while (g_running)
    {
        // ── 3. 每 30 秒上报一次传感器数据 ──
        if (time(NULL) - last_publish >= g_config.mqtt.publish_interval)
        {
            pthread_mutex_lock(&sensor_mutex);

            mqtt_publish_json(&client, "edge/room1/temperature",
                              g_sensors.temperature, "C");
            mqtt_publish_json(&client, "edge/room1/humidity",
                              g_sensors.humidity, "%");
            mqtt_publish_json(&client, "edge/room1/light",
                              g_sensors.light, "lux");
            mqtt_publish_json(&client, "edge/room1/air_quality",
                              g_sensors.air_quality_level, "level");
            mqtt_publish_json(&client, "edge/room1/distance",
                              g_sensors.distance_cm, "cm");
            // IMU 数据量较大, 60 秒才上报一次
            if (time(NULL) % 60 < 30) {
                mqtt_publish_imu_json(&client, &g_sensors.imu);
            }

            pthread_mutex_unlock(&sensor_mutex);
            last_publish = time(NULL);
        }

        // ── 4. 检查收到的控制命令 ──
        mqtt_message_t msg;
        if (mqtt_receive(&client, &msg, 100))  // 100ms 超时
        {
            handle_mqtt_command(&msg);
            //   "edge/room1/cmd/led"    "ON"
            //   "edge/room1/cmd/relay"  "OFF"
            //   "edge/room1/cmd/servo"  "90"
            //   "edge/room1/cmd/reboot" ""
        }

        // ── 5. 保持 MQTT 连接 ──
        mqtt_yield(&client, 100);

        // ── 6. 重连机制 ──
        if (!mqtt_is_connected(&client)) {
            LOG_WARN("MQTT 断开, 3秒后重连...");
            sleep(3);
            mqtt_reconnect(&client);
        }
    }

    mqtt_disconnect(&client);
    return NULL;
}
```

---

## 7. HTTP 线程：Web 仪表盘

```
void *http_thread(void *arg)
{
    // ── 1. 初始化 microhttpd 服务器 ──
    struct MHD_Daemon *daemon;
    daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION,  // 每个连接一个线程
        g_config.web.port,              // 8080
        NULL, NULL,
        &http_request_handler, NULL,    // ★ 请求处理回调
        MHD_OPTION_END
    );

    LOG_INFO("HTTP 服务器启动: http://0.0.0.0:%d", g_config.web.port);

    // ── 2. 等待退出信号 ──
    while (g_running) {
        check_message_queue();  // 处理其他线程发来的消息
        sleep(1);
    }

    MHD_stop_daemon(daemon);
    return NULL;
}

// ── HTTP 请求路由 ──
int http_request_handler(void *cls, struct MHD_Connection *conn,
                         const char *url, const char *method, ...)
{
    // ── 路由分发 ──
    if (strcmp(url, "/") == 0)
        return serve_dashboard(conn);           // 返回 HTML 页面

    if (strcmp(url, "/api/sensors") == 0)
        return serve_sensors_json(conn);        // 返回当前传感器 JSON

    if (strstr(url, "/api/history") == url)
        return serve_history_json(conn, url);   // 查询历史数据

    if (strcmp(url, "/api/export") == 0)
        return serve_export_csv(conn, url);     // 导出 CSV

    if (strstr(url, "/api/cmd/") == url)
        return handle_web_command(conn, url, method);
        // POST /api/cmd/led  body: "ON"     → write(led_fd, "on", 2)
        // POST /api/cmd/relay body: "ON"    → write(relay_fd, "on", 2)
        // POST /api/cmd/servo body: "90"    → write(servo_fd, "90", 2)

    return MHD_NO;
}

// ── 返回传感器数据 JSON ──
serve_sensors_json(conn):
    pthread_mutex_lock(&sensor_mutex);
    构造 JSON:
    {
      "temperature": 27.3,
      "humidity": 65.0,
      "light": 850,
      "air_quality": {"raw":1840, "level":"good"},
      "distance_cm": 120,
      "imu": {"ax":0.01, "ay":0.02, ...},
      "led": "ON",
      "relay": "OFF",
      "servo_angle": 90,
      "uptime": 3600,
      "ts": 1717920000
    }
    pthread_mutex_unlock(&sensor_mutex);
    返回 HTTP 200 + JSON body
```

### Web 仪表盘数据流

```
浏览器                           smart_edge                   驱动
──────                          ──────────                   ────
打开 http://192.168.1.100:8080
  ──GET /──────────────────────► serve_dashboard()
  ◄── HTML + CSS + JS ─────────                              返回静态页面

  setInterval 每 3 秒:
    ──GET /api/sensors─────────► serve_sensors_json()
    │                              │
    │                              ├── pthread_mutex_lock
    │                              ├── 读共享 g_sensors
    │                              ├── pthread_mutex_unlock
    │                              └── 构造 JSON
    ◄── JSON ────────────────────
    │
    更新 DOM:
      temp.innerText = "27.3°C"
      hum.innerText  = "65%"
      chart.data.push({x: now, y: 27.3})
      // Chart.js 自动重绘曲线

  用户点 [LED ON]:
    ──POST /api/cmd/led─────────► handle_web_command()
    │    body: "ON"                │
    │                              ├── write(led_fd, "on", 2)
    │                              │       │
    │                              │       └──► comp_write()
    │                              │              gpiod_set_value(1)
    │                              │              comp_led_notify()
    │                              │                wake_up(wq) → epoll 通知主线程
    │                              │
    │                              └── 写事件到 SQLite
    ◄── {"status":"ok"} ────────
```

---

## 8. CAN 线程：工业总线通信

```
void *can_thread(void *arg)
{
    if (!g_config.can.enabled) {
        LOG_INFO("CAN 未启用, 线程退出");
        return NULL;
    }

    // ── 1. 初始化 SocketCAN ──
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    struct ifreq ifr;
    strcpy(ifr.ifr_name, "can0");
    ioctl(s, SIOCGIFINDEX, &ifr);

    struct sockaddr_can addr;
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    bind(s, (struct sockaddr *)&addr, sizeof(addr));

    // ── 2. 设置过滤器 (只收 ID 0x100~0x1FF) ──
    struct can_filter filter;
    filter.can_id   = 0x100;
    filter.can_mask = 0xF00;
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter));

    // ── 3. 收发循环 ──
    struct can_frame frame;
    fd_set rdfs;
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};

    while (g_running)
    {
        FD_ZERO(&rdfs);
        FD_SET(s, &rdfs);

        if (select(s+1, &rdfs, NULL, NULL, &tv) > 0)
        {
            read(s, &frame, sizeof(frame));
            // 收到 CAN 帧:
            //   frame.can_id  = 0x123   (11/29-bit ID)
            //   frame.can_dlc = 8       (数据长度)
            //   frame.data[0..7]        (数据)
            handle_can_frame(&frame);
        }

        // 定期发送传感器数据到 CAN 总线 (给 PLC/工控机)
        if (time(NULL) % 5 == 0) {
            send_sensor_can_frame(s, &g_sensors);
        }
    }

    close(s);
    return NULL;
}
```

---

## 9. UART 命令引擎

```
// ── 从 smart_monitor 继承, 扩展新命令 ──

命令表:
┌─────────────┬──────────────────┬─────────────────────────────────┐
│ 命令        │ 处理函数         │ 功能                            │
├─────────────┼──────────────────┼─────────────────────────────────┤
│ STATUS      │ cmd_status()     │ 系统状态: 运行时间, 传感器数量   │
│ SENSOR      │ cmd_sensor()     │ 全部传感器当前值               │
│ LED ON/OFF  │ cmd_led()        │ LED 控制                       │
│ RELAY ON/OFF│ cmd_relay() [新] ★ 继电器控制                    │
│ SERVO 90    │ cmd_servo() [新] ★ 舵机角度                      │
│ HISTORY     │ cmd_history()[新]★ 查询最近 N 条记录              │
│ UPLOAD      │ cmd_upload() [新]★ 立即上报一次数据到 MQTT        │
│ CAN SEND    │ cmd_can()   [新] ★ 手动发送 CAN 帧               │
│ LOG LEVEL   │ cmd_loglevel()[新]★ 动态调整日志级别              │
│ RELOAD      │ cmd_reload() [新]★ 重新加载配置文件               │
│ HELP        │ cmd_help()        │ 帮助信息                      │
│ RESET       │ cmd_reset()       │ 重置                          │
└─────────────┴──────────────────┴─────────────────────────────────┘

命令处理流程 (和 smart_monitor 一致):
  UART RX 中断 → kfifo_in → wake_up(rx_wq)
    → epoll_wait 返回 → handle_uart_rx()
    → 逐字符累积 → 遇 \n 触发 → handle_uart_command()
    → 空格分割命令名/参数 → 查表 → 调用 handler
    → handler 执行 → write(uart_fd, 回复)
```

---

## 10. 关机退出

```
用户发送 SIGTERM (systemctl stop smart_edge 或 Ctrl+C)

signal_handler(SIGTERM):
  │
  ├── LOG_INFO("收到退出信号");
  │
  ├── g_running = 0;              // ① 通知所有线程退出
  │
  ├── epoll_wait 返回 (被信号打断或超时)
  │     检查 g_running == false → break 退出主循环
  │
  ├── 等待子线程 join
  │     mqtt_thread:  mqtt_disconnect() → 退出
  │     http_thread:  MHD_stop_daemon() → 退出
  │     timer_thread: 写最后一条 SQLite → 退出
  │     can_thread:   close(socket) → 退出
  │
  ├── cleanup(epfd)
  │     ├── 关闭 13 个设备 fd
  │     ├── 恢复 stdin 阻塞模式
  │     ├── 关闭 epoll fd
  │     ├── 关闭 SQLite db
  │     ├── 销毁消息队列 mq_unlink
  │     ├── 销毁互斥锁
  │     ├── 释放环形缓冲
  │     └── 关闭日志文件
  │
  ├── LOG_INFO("smart_edge 已退出");
  │
  └── exit(0)
```

---

## 11. 关键数据流

### 11.1 一次完整的传感器数据旅程

```
物理世界                       内核                           应用
────────                       ────                           ────
温度 27.3°C
  │
  ▼
DHT11 传感器 → 模拟信号
  │
  ▼
GPIO 单总线 ────────────► dht11_read()
  │ 40bit 数据              │ gpiod_get_value() 循环
  │ 00100001 00001001...    │ 解析: 湿度 65%, 温度 27.3°C
  │                         │ copy_to_user() → 返回 4 字节
  │                         │
  ◄── 4 字节数据 ──────────
  │
  ▼
timer_thread:
  read(dht11_fd) → 27.3
  pthread_mutex_lock
  g_sensors.temperature = 27.3   ←── 写入共享区
  g_sensors.humidity    = 65.0
  g_sensors.last_update = now
  pthread_mutex_unlock
  │
  ├──► ringbuf_push()           ←── 环形缓冲 (最近100条)
  │
  ├──► sqlite_log()              ←── 持久化存储
  │      INSERT INTO sensor_data(sensor,value,unit)
  │      VALUES('temperature',27.3,'C')
  │
  └──► check_alerts()            ←── 告警检查
         (27.3 < 35, 不触发)

...(30 秒后)...

mqtt_thread:
  pthread_mutex_lock
  read g_sensors.temperature     ←── 读共享区
  pthread_mutex_unlock
  mqtt_publish("edge/room1/temperature", 27.3)  ←── 上云

http_thread:
  (浏览器请求 /api/sensors)
  pthread_mutex_lock
  read all g_sensors             ←── 读共享区
  pthread_mutex_unlock
  return JSON → 浏览器          ←── Web 显示
```

### 11.2 一次远程控制的完整旅程

```
手机 MQTT App                     smart_edge                    驱动
─────────────                    ──────────                    ────
PUBLISH edge/room1/cmd/led "ON"
  │
  ▼
MQTT Broker
  │
  ▼
mqtt_thread:
  mqtt_receive() → "ON"
  │
  ├── 方案A: 直接操作
  │     write(led_fd, "on", 2)
  │       │
  │       └──► comp_write()
  │              gpiod_set_value(1)
  │              comp_led_notify()
  │                atomic_set(changed,1)
  │                wake_up(wq)
  │                  │
  │                  └──► epoll_wait 返回
  │                         handle_led() → 打印 "LED ON"
  │
  ├── 方案B: 通过消息队列 (推荐, 避免跨线程直接 IO)
  │     mq_send(cmd_mq, {.type=CMD_LED, .value=1})
  │       │
  │       └──► timer_thread 处理:
  │              write(led_fd, "on", 2)
  │
  ├── LOG_INFO("远程命令: LED ON")
  │
  └── sqlite_log_event("CMD", "MQTT: LED ON")
```

---

## 12. 时序总览

```
时间轴 (一次 2 秒的周期)

t=0ms     epoll_wait() 阻塞
            │
t=0~1000ms  可能的事件:
            ├── KEY0 按下     → handle_key()    → toggle LED
            ├── UART 收到命令  → handle_uart()   → 解析执行 + 回复
            ├── stdin 按键    → handle_stdin()  → q/s/1/2/h
            └── LED 状态变化  → handle_led()    → 打印状态

t=1000ms  epoll_wait 超时返回 (nfds=0)

t=1002ms  ── 进入 timer_thread ──
t=1002ms  read(dht11_fd)    → temp=27.3  hum=65.0
t=1004ms  read(ap3216c_fd)  → ir=120  als=850  ps=200
t=1006ms  read(icm20608_fd) → accel(x,y,z) gyro(x,y,z) temp
t=1008ms  read(sr04_fd)     → distance=120cm
t=1010ms  read(mq135_fd)    → adc=1840 → level=good
t=1012ms  pthread_mutex_lock → 更新 g_sensors
t=1013ms  ringbuf_push      → 入环形缓冲
t=1015ms  sqlite INSERT      → 持久化
t=1017ms  check_alerts()    → 无异常
t=1018ms  pthread_mutex_unlock

t=1020ms  (mqtt_thread 异步)
t=1020ms  mqtt_yield() → 检查是否有 QOS 确认 / 新订阅消息

t=1025ms  (http_thread 异步)
t=1025ms  浏览器 fetch /api/sensors → 返回 JSON → 更新仪表盘

t=2000ms  回到 epoll_wait() 阻塞
          ↓
t=2000~3000ms  (下一周期)
```

---

## 线程交互矩阵

```
                main    timer   mqtt    http    can
                ────    ─────   ────    ────    ────
epoll 事件       ○       ─       ─       ─       ─
读传感器         ─       ○       ─       ─       ─
写传感器         ○       ○       ※       ※       ─
共享数据区      读      读写    读      读      (读)
SQLite 写入      ─       ○       ※       ─       ─
SQLite 查询      ─       ─       ─       ○       ─
MQTT 上报        ─       ─       ○       ─       ─
MQTT 控制        ─       ─       ○→mq    ─       ─
HTTP API         ─       ─       ─       ○       ─
CAN 收发         ─       ─       ─       ─       ○
配置文件         启动读  (读)    (读)    (读)    (读)
日志写入         全线程
看门狗喂狗       ○       ─       ─       ─       ─

○ = 该线程负责    读/写 = 对共享数据的操作
※ = 通过消息队列间接操作    mq = 通过消息队列
─ = 不参与
```

---

## 附录：主要系统调用速查

| 类别 | 系统调用 | 出现在 |
|------|---------|--------|
| 文件 | open/read/write/close/ioctl | 所有驱动/dev访问 |
| epoll | epoll_create1/epoll_ctl/epoll_wait | 主线程事件循环 |
| 线程 | pthread_create/pthread_join/pthread_mutex_lock | 多线程框架 |
| IPC | mq_open/mq_send/mq_receive/mq_unlink | 线程间命令传递 |
| 时钟 | time/clock_gettime/sleep | 定时任务 |
| 网络 | socket/bind/connect/send/recv (MQTT/CAN) | MQTT线程/CAN线程 |
| 信号 | signal/sigaction | 退出处理 |
| 数据库 | sqlite3_open/sqlite3_exec/sqlite3_close | SQLite 操作 |
