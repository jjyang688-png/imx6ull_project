# imx6ull_project — 智能环境监测终端

基于 NXP i.MX6ULL (ARM Cortex-A7)，Linux 4.1.15，从零构建完整嵌入式 Linux 驱动项目。

## 开发环境

| 项目 | 值 |
|------|-----|
| Windows 代码编辑 | VS Code 通过 Samba 编辑 Linux 上的文件 |
| Samba 共享 | `\\192.168.80.106\imx6ull` → 项目目录；`\\192.168.80.106\linux` → `/home/yang/linux`（含内核源码树） |
| VS Code IntelliSense | `.vscode/c_cpp_properties.json` 已配置内核头文件路径，支持跳转定义 |
| Linux 编译环境 | Ubuntu 16.04 @ 192.168.80.106，用户 yang |
| 交叉编译器 | ARM gcc 5.4.0 (`arm-linux-gnueabihf-gcc`) |
| 内核源码树 | `/home/yang/linux/alentek_uboot/linux/linux-imx-rel_imx_4.1.15_2.1.0_ga_alientek` |
| 目标板 | 正点原子 i.MX6ULL ALPHA |

## 编译 & 部署

```bash
# SSH 到 Linux 编译（从 MobaXterm 或 ssh yang@192.168.80.106）
cd ~/linux/imx6ull_project
make all          # 编译驱动 + 应用
make modules      # 仅编译驱动
make apps         # 仅编译应用
make clean        # 清理

# 部署到开发板
# 通过 tftp/nfs 将 .ko 传到板子，insmod 加载
```

## 项目结构

```
driver/     — 6个内核驱动（CDEV / Platform / I2C / SPI / UART / Misc）
app/        — 用户空间统一监控程序
dts/        — 设备树源文件
docs/       — 需求文档 / 架构设计 / API参考 / 阶段学习报告
scripts/    — build.sh / deploy.sh / test_all.sh / checkpatch.sh
```

## 9 阶段开发计划

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | 项目立项与环境搭建 | ✓ 完成 |
| 2 | 设备树设计 | 待开始 |
| 3 | LED 驱动 (CDEV) | 待开始 |
| 4 | 并发+IO 模型 | 待开始 |
| 5 | 按键输入驱动 | 待开始 |
| 6 | I2C + SPI 传感器 | 待开始 |
| 7 | UART + Misc 驱动 | 待开始 |
| 8 | 统一应用+集成测试 | 待开始 |
| 9 | 文档交付+发布 | 待开始 |

## 参考

- 前身项目 firstcc: `C:\Users\2104022\Desktop\code\firstcc`（完整参考实现）
- GitHub: `jjyang688-png/firstcc` (已推送), `jjyang688-png/imx6ull_project` (待推送)
