# 基于 i.MX6ULL 的智能环境监测终端

> Smart Environment Monitor based on i.MX6ULL

## 项目简介

本项目从零构建一个完整的嵌入式 Linux 系统，涵盖 6 个内核驱动（字符设备 / Platform / I2C / SPI / UART / Misc）和用户空间统一监控程序。

适用于嵌入式 Linux 驱动工程师的技术能力展示和面试项目。

## 技术栈

- **SoC**：NXP i.MX6ULL (ARM Cortex-A7)
- **内核**：Linux 4.1.15
- **工具链**：arm-linux-gnueabihf-
- **总线覆盖**：GPIO / I2C / SPI / UART / Platform
- **IO 模型**：阻塞 / 非阻塞 / Poll / epoll / 异步通知

## 项目结构

```
imx6ull_project/
├── driver/              # 内核驱动模块（Kbuild）
├── app/                 # 用户空间程序
├── dts/                 # 设备树源文件
├── docs/                # 项目文档
│   ├── REQUIREMENTS.md  # 需求规格说明书
│   ├── HARDWARE.md      # 硬件资源规划
│   ├── ARCHITECTURE.md  # 系统架构设计
│   ├── API_REFERENCE.md # 设备接口参考手册
│   └── phases/          # 各阶段学习报告
├── scripts/             # 自动化脚本
├── Makefile             # 顶层构建文件
└── README.md
```

## 快速开始

```bash
# 编译全部（驱动 + 应用）
export KERNEL_DIR=/path/to/linux-imx
./scripts/build.sh

# 仅编译驱动
make -C driver KERNEL_DIR=$KERNEL_DIR

# 仅编译应用
make -C app
```

## 文档

详细文档见 `docs/` 目录。

## 许可证

GPL v2
