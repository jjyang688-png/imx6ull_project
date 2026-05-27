# 需求规格说明书

## 1. 项目概述

### 1.1 项目名称
基于 i.MX6ULL 的智能环境监测终端（Smart Environment Monitor）

### 1.2 目标平台
- **SoC**：NXP i.MX6ULL（ARM Cortex-A7，单核 792MHz）
- **开发板**：正点原子 ALPHA i.MX6ULL
- **内核版本**：Linux 4.1.15（正点原子官方 BSP）
- **交叉编译器**：arm-linux-gnueabihf-gcc（Linaro / ARM 官方工具链）

### 1.3 项目目标
构建一个完整的嵌入式 Linux 系统，通过多个外设传感器采集环境数据，实现：
- LED 状态指示（GPIO）
- 按键输入检测（GPIO + 中断）
- 环境光照与接近检测（I2C）
- 6 轴姿态检测（SPI）
- 串口传感器数据接收（UART）
- 系统综合状态监控（Misc 设备）
- 用户空间统一监控程序（epoll 多路复用）

---

## 2. 功能需求

### 2.1 内核驱动层

| 编号 | 功能 | 设备文件 | 总线 | 详细描述 |
|------|------|---------|------|---------|
| F-K01 | LED 控制驱动 | `/dev/comp_drv` | GPIO | 支持 on/off/blink 命令；支持阻塞/非阻塞 read；支持 poll/select；支持异步通知 SIGIO；支持 ioctl 设置闪烁周期；支持内核定时器自动闪烁 |
| F-K02 | 按键输入驱动 | `/dev/input/eventX` | GPIO+IRQ | 双边沿中断触发；工作队列消抖（可配置时间）；通过 Input 子系统上报按键事件 |
| F-K03 | 光照/接近传感器 | `/dev/ap3216c` | I2C | 读取红外光(IR)、环境光(ALS)、接近距离(PS)；支持 SMBus 和 I2C burst 传输 |
| F-K04 | 6 轴姿态传感器 | `/dev/icm20608` | SPI | 读取 3 轴加速度 + 3 轴角速度 + 温度；支持 ioctl 分别读取各传感器数据 |
| F-K05 | 串口传感器驱动 | `/dev/uart_sensor` | UART | 串口数据收发；kfifo 环形缓冲；阻塞/非阻塞读；poll 支持 |
| F-K06 | 系统状态监控 | `/dev/sys_monitor` | Misc | 系统运行时间、模块心跳；使用 misc_register 轻量注册 |

### 2.2 驱动接口要求

| 接口类型 | 用途 | 使用驱动 |
|---------|------|---------|
| `sysfs` 属性组 | 运行时查看/修改驱动参数 | comp_drv |
| `debugfs` 调试节点 | 开发阶段内部状态检视 | comp_drv, key_input |
| `module_param` | 模块加载时参数配置 | 全部驱动 |
| `ioctl` | 设备特定命令控制 | comp_drv, icm20608 |

### 2.3 用户空间程序

| 编号 | 程序 | 功能 |
|------|------|------|
| F-U01 | `smart_monitor` | 统一监控守护进程：epoll 监听全部设备文件，实时显示传感器数据，支持 CSV 日志记录，支持键盘交互控制 |
| F-U02 | `test_*` 系列 | 各驱动的独立功能测试程序 |

---

## 3. 非功能需求

### 3.1 代码质量
- 所有驱动通过 Linux 内核 `checkpatch.pl` 检查（允许 80 列警告）
- 使用统一的内核编码风格（`scripts/checkpatch.pl --strict`）
- 错误处理使用标准 goto 清理模式

### 3.2 文档要求
- `REQUIREMENTS.md`：本文件，需求规格
- `HARDWARE.md`：硬件资源规划
- `ARCHITECTURE.md`：系统架构设计
- `API_REFERENCE.md`：设备接口参考手册
- `LESSONS_LEARNED.md`：全项目知识点汇总

### 3.3 构建系统
- 支持交叉编译（`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-`）
- 内核模块使用 Kbuild 框架
- 用户程序使用标准 Makefile + GCC
- 一键编译脚本 `build.sh`
- 一键测试脚本 `test_all.sh`

### 3.4 Git 规范
- 分支策略：`main` 主分支 + 功能分支 `feature/xxx`
- 提交信息：Conventional Commits 规范（`feat:` / `fix:` / `docs:` 等）
- 版本标签：语义化版本 `v1.0.0`

---

## 4. 硬件资源规划（概要）

| 外设 | 接口 | 引脚/地址 | 备注 |
|------|------|----------|------|
| LED | GPIO | GPIO1_IO03 | 低电平有效 |
| KEY | GPIO | GPIO1_IO18 | 低电平有效，双边沿中断 |
| AP3216C | I2C1 | 0x1E | 光照/接近传感器 |
| ICM20608 | ECSPI1 | CS0 | 6 轴传感器，SPI Mode 0，max 8MHz |
| UART Sensor | UART3 | — | 串口传感器（115200-8-N-1） |

> 详细硬件资源表见 `docs/HARDWARE.md`（阶段 2 产出）

---

## 5. 里程碑

| 阶段 | 内容 | 预计产出 |
|------|------|---------|
| 1 | 项目立项与环境搭建 | 本文件 + 目录结构 + 构建系统验证 |
| 2 | 设备树设计 | 硬件资源文档 + DTS 文件 |
| 3 | LED 驱动（基础） | comp_drv.ko + 基本测试 |
| 4 | LED 驱动（IO模型） | 阻塞/非阻塞/Poll/异步通知 |
| 5 | 按键驱动 | key_input.ko + 输入测试 |
| 6 | I2C + SPI 驱动 | ap3216c.ko + icm20608.ko |
| 7 | UART + Misc 驱动 | uart_sensor.ko + sys_monitor.ko |
| 8 | 统一应用 + 测试 | smart_monitor + test_all.sh |
| 9 | 文档交付 + 发布 | 完整文档 + v1.0.0 发布 |

---

*文档版本：v0.1 | 日期：2026-05-27 | 阶段 1 产出*
