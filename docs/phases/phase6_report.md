# 阶段 6 学习总结（2026-06-02）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `driver/icm20608.h`（新增 43 行） | ICM20608 ioctl 命令码 + 数据结构定义 |
| `driver/icm20608.c`（新增 451 行） | SPI 传感器驱动：SPI 寄存器读写 + 突发读 + CDEV + ioctl |
| `driver/ap3216c.c`（新增 317 行） | I2C 传感器驱动：I2C 寄存器读写 + i2c_transfer 突发读 + CDEV |
| `test/test_icm20608.c`（新增 100 行） | 支持 read/ioctl 两种模式读取传感器 |
| `test/test_ap3216c.c`（新增 47 行） | 纯 read 模式读取光感/接近传感器 |
| `driver/Makefile`（更新） | `obj-m` 加入 `ap3216c.o icm20608.o` |
| `app/Makefile`（更新） | 加入两个传感器测试程序编译目标 |

## 新接触的知识点

### 一、SPI 子系统（Ch62）

| 知识点 | 说明 |
|------|------|
| **`spi_driver`** | SPI 设备驱动结构体，含 probe( spi_device * )、remove( spi_device * )，对比 platform_driver 的 probe(struct platform_device *) |
| **`module_spi_driver()`** | 注册 SPI 驱动的便捷宏，等价于 module_init + module_exit |
| **`spi_device`** | 由 SPI 控制器驱动创建并传入 probe，包含 chip_select（片选号，来自 dts 的 `reg = <0>`）、max_speed_hz（来自 `spi-max-frequency`）、mode（CPOL/CPHA） |
| **`spi_message` + `spi_transfer` + `spi_sync`** | SPI 数据传输三层结构：`spi_transfer` = 一段连续数据块（tx_buf + rx_buf + len）；`spi_message` = 一个 CS（片选）周期内的 transfer 链表；`spi_sync` = 同步阻塞执行整条 message。CS 在 message 期间始终保持低电平，message 结束才释放 |
| **SPI 协议细节** | ICM20608 的命令字节：bit7 = 1 表示读（地址 \| 0x80），bit7 = 0 表示写（地址 & 0x7F）。读操作需要两个 transfer：发地址（tx_buf）→ 收数据（rx_buf），写操作只需一个 transfer：发地址+数据 |
| **SPI 突发读** | 发一次起始地址，芯片内部地址自动递增，连续返回 N 个寄存器的值。14 字节加速度+温度+陀螺仪一次读出 |
| **SPI vs SMBus API 复杂度** | SPI 没有 I2C 的 SMBus 封装（spi_write_then_read 在内核 4.1.15 中不存在），必须手动拼 spi_message + spi_transfer，比 I2C 的 i2c_smbus_read_byte_data 更底层 |

### 二、I2C 子系统（Ch61）

| 知识点 | 说明 |
|------|------|
| **`i2c_driver`** | I2C 设备驱动结构体，probe 签名为 probe(client, id)（比 SPI 多一个 i2c_device_id 参数） |
| **`module_i2c_driver()`** | 注册 I2C 驱动的便捷宏 |
| **`i2c_client`** | 由 I2C 控制器驱动创建并传入 probe，包含 addr（从设备地址，来自 dts 的 `reg = <0x1e>`）、adapter（I2C 控制器，用于 i2c_transfer）、dev |
| **`i2c_smbus_read_byte_data()`** | SMBus 规范的单字节读：I2C 总线上写 1 字节（寄存器地址）+ 读 1 字节（数据），内核封装好的一行调用。返回值：成功=数据(0~255)，失败=负值 errno |
| **`i2c_smbus_write_byte_data()`** | SMBus 单字节写：写寄存器地址 + 写数据值 |
| **`i2c_msg` + `i2c_transfer()`** | I2C 底层传输：msg[0] 写寄存器地址（flags=0），msg[1] 读数据（flags=I2C_M_RD）。一次 transfer 只产生一对 START-STOP，比 6 次 smbus 调用快 6 倍 |
| **`i2c_transfer` vs `spi_sync`** | 都是底层同步传输 API，但 I2C 的 i2c_msg 自带方向和地址（硬件协议层面），SPI 的 spi_transfer 靠 tx_buf/rx_buf 决定方向（硬件线层面） |

### 三、SPI vs I2C — 总线对比

| 维度 | SPI (icm20608) | I2C (ap3216c) |
|------|------|------|
| 线数 | 4 线（CS, CLK, MOSI, MISO） | 2 线（SCL, SDA） |
| 通信模式 | 全双工 | 半双工 |
| 速度 | 快（8MHz+） | 慢（100kHz~400kHz） |
| 寻址方式 | CS 片选线（每个设备一根） | 广播地址（7 位从地址） |
| 设备树 reg | `reg = <0>`（CS 编号） | `reg = <0x1e>`（I2C 地址） |
| 驱动结构体 | `spi_driver` | `i2c_driver` |
| probe 签名 | `probe(struct spi_device *)` | `probe(struct i2c_client *, const struct i2c_device_id *)` |
| 存/取驱动数据 | `spi_set_drvdata` / `spi_get_drvdata` | `i2c_set_clientdata` / `i2c_get_clientdata` |
| 注册宏 | `module_spi_driver()` | `module_i2c_driver()` |
| ID 表类型 | `spi_device_id` | `i2c_device_id` |
| 单字节读 API | 手动拼 spi_message | `i2c_smbus_read_byte_data()` |
| 多字节读 API | `spi_sync` + 2 个 transfer | `i2c_transfer` + 2 条 msg |

### 四、devm_ 资源管理（补充 Phase 3-5）

| 知识点 | 说明 |
|------|------|
| **`devm_kzalloc(锚点, 大小, 标志)`** | 从**内核通用堆**分配内存并清零。第一个参数是"锚点"（struct device *），标记"这块内存归属于哪个设备"，NOT "分配到哪个设备"。设备 remove 时内核自动 kfree |
| **`spi_set_drvdata(spi, dev)`** | 把私有数据指针存到 spi_device 内部。目的：remove/suspend/resume 回调只收到 spi_device，通过 `spi_get_drvdata(spi)` 取回。和全局变量 g_xxx 的区别：全局变量只能管一个设备，set_drvdata 支持多个同类设备 |
| **`i2c_set_clientdata(client, dev)`** | 同 `spi_set_drvdata`，但是 I2C 版本 |
| **`g_xxx = dev`（全局变量）** | 给 open/read/ioctl 等 fops 函数用的快捷方式。fops 收不到 spi_device/i2c_client，只能通过 filp->private_data 取回 |

### 五、字节序：大端 vs 小端

| | ICM20608（大端 Big-Endian） | AP3216C（小端 Little-Endian） |
|------|------|------|
| 16 位值 0x1234 的存储 | buf[0]=0x12(MSB), buf[1]=0x34(LSB) | buf[0]=0x34(LSB), buf[1]=0x12(MSB) |
| 组合表达式 | `((s16)buf[0] << 8) \| buf[1]` | `((u16)buf[1] << 8) \| buf[0]` |
| 记忆口诀 | 高字节在低地址（高→低 → 大端） | 低字节在低地址（低→低 → 小端） |

### 六、芯片验证方法

| 传感器 | 验证方式 | 芯片 ID |
|------|------|------|
| ICM20608 | 读 WHO_AM_I (0x75) | 0x98 或 0xAF |
| AP3216C | 读 SYS_CONF (0x00) | 默认上电值 0x00 |

### 七、四驱动对比总结

| 维度 | comp_drv | key_input | icm20608 | ap3216c |
|------|------|------|------|------|
| 总线 | platform | platform | **SPI** | **I2C** |
| probe 参数 | platform_device | platform_device | **spi_device** | **i2c_client** |
| 驱动注册 | module_platform_driver | module_platform_driver | **module_spi_driver** | **module_i2c_driver** |
| 设备节点 | /dev/comp_drv | /dev/input/eventX | /dev/icm20608 | /dev/ap3216c |
| self fops | ✅（自己写） | ❌（input 子系统） | ✅（自己写） | ✅（自己写） |
| write | ✅ | ❌ | ❌ | ❌ |
| ioctl | ✅ | ❌ | ✅ | ❌ |
| 触发方式 | 用户主动写 | 硬件中断 | 用户主动读 | 用户主动读 |
| GPIO 方向 | 输出 | 输入 | — | — |
| 锁类型 | mutex | **spinlock** | mutex | mutex |
| 字节序 | — | — | **大端** | **小端** |

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `spi_write_then_read` 不存在 | Linux 4.1.15 内核没有这个 API | 改用 `spi_sync` + `spi_message` + `spi_transfer` |
| 头文件重复 + DBG 宏断行 | 手动编辑时重复 include，`while(0)` 单独一行 | 重新整理，移除重复，宏写在同一逻辑行 |
| icm20608.h 中结构体定义在宏后面 | 阅读代码时先看到宏引用未定义的类型 | 调整顺序：先定义结构体，再定义引用它们的宏 |

## 代码规范要点

- SPI 读操作：地址 \| 0x80（bit7=1），写操作：地址 & 0x7F（bit7=0）
- I2C 多字节读用 `i2c_transfer`（1 次 STOP），不要循环调 `i2c_smbus_read_byte_data`（N 次 STOP）
- `mutex_lock` 保护的是数据一致性（dev->data），不是 I2C/SPI 总线（总线自身有锁）
- CDEV 三步（alloc_chrdev_region → cdev_init → cdev_add）与总线无关，I2C/SPI/platform 完全一样
- remove 中先关硬件再删设备节点（先 `write_reg(..., SLEEP)` 再 `device_destroy`）
- 测试程序中 struct 定义必须和驱动的头文件保持同步

## 下一阶段预告

**阶段 7：UART + Misc 驱动** — UART 串口传感器驱动（TTY 层 + line discipline 或直接 UART 驱动）；Misc 设备（杂项设备驱动框架，简化注册流程）；系统状态监控（CPU 温度、运行时间等 sysfs 节点）。
