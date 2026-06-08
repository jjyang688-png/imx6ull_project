# 硬件资源规划

## 1. 开发板信息

| 项目 | 值 |
|------|------|
| SoC | NXP i.MX6ULL (ARM Cortex-A7, 792MHz) |
| 开发板 | 正点原子 ALPHA i.MX6ULL |
| 内核 | Linux 4.1.15 (正点原子 BSP) |
| 调试串口 | UART1 (Micro USB, CH340) |

## 2. 外设资源汇总

| 外设 | 接口 | 引脚/地址 | 中断号 | 备注 |
|------|------|------|------|------|
| LED | GPIO | GPIO1_IO03 | — | 低电平有效 (active-low) |
| KEY0 | GPIO + IRQ | GPIO1_IO18 | 双边沿触发 | 低电平有效, 去抖 20ms |
| AP3216C | I2C1 | 0x1E | — | 光照+红外+接近, 100kHz |
| ICM20608 | ECSPI3 CS0 | `reg = <0>` | — | 6轴传感器, SPI Mode 0, 8MHz |
| UART3 | UART (裸寄存器) | 基地址 0x021EC000 | GIC_SPI 28 (Linux IRQ 60) | DB9 RS232, 115200-8-N-1 |

## 3. 引脚分配详情

| 功能 | 引脚名称 | GPIO 编号 | 电气特性 | 复用功能 |
|------|------|------|------|------|
| LED | GPIO1_IO03 | gpio-3 | 3.3V 推挽输出 | GPIO |
| KEY0 | GPIO1_IO18 | gpio-18 | 3.3V 输入, 内部上拉 | GPIO |
| I2C1_SCL | UART4_TXD | — | 开漏, 2.2kΩ 上拉 | ALT2 (I2C1) |
| I2C1_SDA | UART4_RXD | — | 开漏, 2.2kΩ 上拉 | ALT2 (I2C1) |
| ECSPI3_SCLK | UART2_RTS_B | — | 推挽 | ALT1 (ECSPI3) |
| ECSPI3_MOSI | UART2_CTS_B | — | 推挽 | ALT1 (ECSPI3) |
| ECSPI3_MISO | UART2_RXD | — | 输入 | ALT1 (ECSPI3) |
| ECSPI3_CS0 | UART2_TXD | — | 推挽 | ALT1 (ECSPI3) |
| UART3_RXD | — | — | RS232 电平 (SP3232) | ALT0 (UART3) |
| UART3_TXD | — | — | RS232 电平 (SP3232) | ALT0 (UART3) |

## 4. 内存映射

| 外设 | 物理地址 | 长度 | 说明 |
|------|------|------|------|
| UART3 | 0x021EC000 | 16KB (0x4000) | ioremap 后用 readl/writel 访问 |
| GPIO1 | 0x0209C000 | 4KB | 由内核 GPIO 子系统管理 |
| I2C1 | 0x021A0000 | 4KB | 由内核 I2C 子系统管理 |
| ECSPI3 | 0x02018000 | 4KB | 由内核 SPI 子系统管理 |

## 5. I2C 设备地址

| 设备 | 总线 | 7位地址 | 8位地址(写) | 8位地址(读) |
|------|------|------|------|------|
| AP3216C | I2C1 | 0x1E | 0x3C | 0x3D |

## 6. SPI 设备参数

| 设备 | 控制器 | 片选 | 模式 (CPOL/CPHA) | 最大频率 | 数据位宽 |
|------|------|------|------|------|------|
| ICM20608 | ECSPI3 | CS0 | Mode 0 (0/0) | 8 MHz | 8 bit |

## 7. UART 配置

| 参数 | 值 |
|------|------|
| 串口号 | UART3 |
| 物理接口 | DB9 公头 (RS232 电平, 经 SP3232 转换) |
| 波特率 | 115200 |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验 | 无 (N) |
| 参考时钟 | PER_CLK = 80MHz |
| UBIR | 4 |
| UBMR | 3124 |

---

*文档版本：v1.0 | 日期：2026-06-08*
