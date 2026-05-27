# 阶段 2 学习总结（2026-05-27）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `dts/imx6ull-smart-monitor.dtsi` | 项目设备树 include 文件，定义 6 个外设节点 |
| `imx6ull-alientek-emmc.dts`（修改） | 注释旧节点 + 添加 `#include "imx6ull-smart-monitor.dtsi"` |
| `imx6ull-alientek-emmc.dtb` | 完整编译后的设备树二进制 |

## 新接触的知识点

| 知识点 | 说明 |
|--------|------|
| **设备树分层架构** | `.dts`（板级顶层）→ `.dtsi`（SoC 公共定义 / 项目外设片段）→ `.dtb`（内核读取的二进制）。`.dtsi` 不能单独编译，必须被 `.dts` include 后才能与主文件一起送给 dtc |
| **`.dtsi` 的工程价值** | 将项目的外设改动隔离在独立文件中，主 DTS 只加一行 `#include`，原厂模板保持干净。多板子共用时尤其有用 |
| **SPI 设备树中 `reg` 的含义** | SPI 总线上 `reg = <0>` 不是地址，而是**片选索引**（Chip Select index）。`reg = <0>` 表示使用该 SPI 控制器的第 0 个片选信号（CS0）。I2C 的 `reg` 才是真的从机地址 |
| **GPIO 绑定三要素** | `gpios = <&gpioX pin flags>`：①指向哪个 GPIO 控制器 ②引脚编号 ③有效电平。`GPIO_ACTIVE_LOW` 宏展开为 0，`GPIO_ACTIVE_HIGH` 为 1 |
| **中断绑定** | `interrupt-parent = <&gpio1>` 指定中断控制器（GPIO 控制器也是中断控制器），`interrupts = <18 IRQ_TYPE_EDGE_BOTH>` 指定引脚号和触发方式（上升沿+下降沿） |
| **Pinctrl 两步走** | ①在 `&iomuxc {}` 中**定义**引脚组（引脚复用功能 + 电气属性）②在设备节点中**引用**（`pinctrl-0 = <&pinctrl_xxx>`）。两步分离：IOMUXC 定义"能用什么"，设备声明"我用哪个" |
| **总线控制器 vs 子设备的 pinctrl 职责** | I2C/SPI/UART 的引脚配置在**控制器**层完成（`&i2c1 { pinctrl-0 = <&pinctrl_i2c1>; }`），总线下的子设备只管数据收发，不需重复配置引脚。独立 GPIO 设备（LED、按键）则必须在自身节点中引用 pinctrl |
| **`compatible` 是驱动与设备树的桥梁** | 内核驱动声明 `of_match_table`，设备树提供 `compatible` 字符串。内核启动时遍历设备树节点，匹配成功就调用驱动的 `probe` 函数。这个机制让同一套驱动代码可以适配不同板子——硬件差异全在设备树中 |
| **`status = "okay"`** | 设备树节点默认 `disabled`（或继承父节点状态），显式设为 `"okay"` 内核才会启用该设备。`uart3` 在原厂 DTS 中已 `okay`，include 后再写一遍不冲突——同属性合并 |
| **dtc 编译验证流程** | 不是直接 `dtc` 编译 `.dtsi`（会报错，因为它不完整），而是让内核 Makefile 预处理 + 编译主 `.dts`：`make ARCH=arm ... imx6ull-alientek-emmc.dtb`，展开所有 `#include` 后统一编译 |
| **设备树节点命名规范** | `label: device@address { }` —— label 供其他地方通过 `&label` 引用，device@address 的 address 在 I2C 上是从机地址、SPI 上是片选号、内存映射设备上是基地址 |
| **平台设备 vs 总线设备** | 放在根节点 `/ {}` 下的是**平台设备**（Platform Device，如 LED、按键、Misc），挂在 `&i2c1 {}`、`&ecspi3 {}` 等总线节点下的是**总线设备**。区别在于 probe 时内核提供的资源类型不同 |

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `dtc` 直接编译 `.dtsi` 报语法错误 | `.dtsi` 引用了主 DTS 中定义的标签（`&i2c1`、`&gpio1`、`GPIO_ACTIVE_LOW` 宏等），单独编译时这些符号不存在 | `.dtsi` 必须作为被 include 文件，通过 `make ... xx.dtb` 让内核构建系统预处理 + 编译 |
| `.dtsi` 放在项目目录，内核 make 找不到 | Makefile 在 `arch/arm/boot/dts/` 执行，`#include "xxx.dtsi"` 从当前目录搜索 | 将 `.dtsi` 复制到内核源码树的 `arch/arm/boot/dts/` 目录（或用绝对路径，但不推荐） |
| 旧节点冲突 | 原厂 DTS 中已有的 `gpioled`、`key`、`ap3216@1e`、`icm20608@0` 与项目节点使用同一 GPIO / I2C 地址 / SPI 片选 | 注释掉原厂 DTS 中的旧节点，保留 `&iomuxc` 中的引脚组定义供新节点引用 |

## 代码规范要点

- `.dtsi` 文件需要在开头加注释说明用途和引用方式
- 节点之间用空行分隔，每个节点前用 `/* ==== ... ==== */` 注释标注用途
- SPI 设备：`spi-max-frequency` 必填，单位 Hz
- I2C 设备：`reg` 用 7 位地址写十六进制（如 `0x1e`）
- 独立 GPIO 设备必须同时设置 `pinctrl-names` + `pinctrl-0` 和 `xxx-gpios`

## 下一阶段预告

**阶段 3：LED 驱动 (CDEV)** — 编写第一个字符设备驱动 `comp_drv.c`，实现 GPIO 控制 LED 的 on/off/blink，注册设备号、创建设备节点、实现 file_operations（open/release/write/read）。
