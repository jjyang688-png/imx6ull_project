# 阶段 3 学习总结（2026-05-28）

## 本阶段交付物

| 文件 | 说明 |
|------|------|
| `driver/comp_drv.h` | LED 驱动头文件：状态枚举 + 用户态数据结构 |
| `driver/comp_drv.c` | LED 字符设备驱动：Platform 框架 + CDEV 注册 + GPIO 控制 |
| `driver/Makefile`（修改） | 添加 `comp_drv.o` 编译目标 |
| `driver/comp_drv.ko` | 交叉编译产物，可加载到开发板 |
| `dts/imx6ull-alientek-emmc.dtb` | 同步更新，拷贝到项目 `dts/` 目录 |

## 新接触的知识点

| 知识点 | 说明 |
|--------|------|
| **字符设备驱动全流程** | `alloc_chrdev_region`（申请设备号）→ `cdev_init` + `cdev_add`（绑定 fops 注册到 VFS）→ `class_create` + `device_create`（让 udev 创建设备节点）→ `cdev_del` + `unregister_chrdev_region`（逆序清理）。这 6 个 API 是 Linux 字符设备驱动的标准模板，后续所有驱动复用同一套路 |
| **Platform 驱动模型** | `platform_driver` 包含 probe/remove 和 `of_match_table`。内核启动时从设备树根节点下的 `compatible` 属性创建 `platform_device`，然后遍历 `platform_driver` 链表，匹配成功就调用 probe。这是设备树和驱动的桥梁 |
| **`platform_device` 何时被创建** | 内核启动时解析设备树：根节点 `/` 下每个带 `compatible` 属性的节点，由 OF 子系统自动创建为 `struct platform_device`。而 I2C/SPI 总线下的子节点（如 `&i2c1 { ap3216c@1e {} }`）由对应总线子系统创建为 `i2c_client`/`spi_device`，不是 platform_device |
| **`devm_gpiod_get(d, "led", flags)` vs 旧的 OF GPIO 方式** | 新的 gpiod（GPIO descriptor）API，函数名 `"led"` 自动匹配设备树的 `led-gpios` 属性；`devm_` 前缀表示设备托管，驱动卸载时自动释放；内部自动处理 `GPIO_ACTIVE_LOW` 逻辑反转。旧方式是 `of_get_named_gpio()` → `gpio_request()` → `gpio_direction_output()`，需手动管理生命周期且不处理逻辑电平。gpiod 是现代内核推荐做法 |
| **`gpiod_set_value` 的逻辑电平** | 参数 1 = 激活（asserted），0 = 未激活（de-asserted）。设备树中设了 `GPIO_ACTIVE_LOW` 后，物理电平自动反转——代码写 1 亮灯（物理低电平），写 0 灭灯（物理高电平）。驱动代码只需关心"逻辑上的开关"，不用管硬件电平极性 |
| **`platform_set_drvdata` vs 全局变量** | `platform_set_drvdata(pdev, dev)` 把设备指针存入 `platform_device` 内部，供 `remove()`/`suspend()`/`resume()` 等平台回调通过 `platform_get_drvdata(pdev)` 获取。全局变量 `g_dev` 则服务于**无法接收 pdev 或 filp 的回调**（如内核定时器回调只有 `unsigned long`、sysfs show/store 只有 `struct device *`）。两者分工不同，单设备驱动中并行使用 |
| **`private_data` 传递机制** | `open()` 时 `filp->private_data = g_dev`，后续 `read()`/`write()`/`release()` 通过 `filp->private_data` 取回设备指针。这是 VFS 层提供的"每次打开一个文件就有独立的 `file` 对象"机制，让文件操作函数无需全局变量就能拿到设备上下文 |
| **`copy_from_user` / `copy_to_user`** | 内核不能直接解引用用户空间指针——①地址可能无效（野指针）②用户态和内核态地址空间不同（ARM 上尤其严格）。这两个函数在拷贝前会校验地址合法性，失败返回未拷贝的字节数。write 中 `copy_from_user` 收数据，read 中 `copy_to_user` 发数据 |
| **goto 级联清理模式** | 内核标准错误处理：从最后一层出错跳转到对应标签，每个标签清理自己这层已申请的资源，然后 fall through 到上一层。清理顺序必须是申请顺序的逆序。这是避免资源泄漏的最简洁写法 |
| **read 是否只能读一次** | 取决于 `*ppos` 的处理。状态类设备（LED 驱动、传感器驱动）通常每次都返回当前完整状态，不使用 `*ppos` 累加——这样用户可以反复 `cat` 获取最新数据。流式设备（串口、音频）才需要 `*ppos` 记录读取进度 |

## 踩过的坑

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `comp_drv.h` 枚举分号写错 | `LED_OFF = 0;` 枚举项间应为逗号 | 改为 `LED_OFF = 0,` |
| `file_operations` 结构体缺少变量名 | 写成 `static const struct file_operations = {...}` | 加变量名：`static const struct file_operations comp_fops = {...}` |
| probe 中 6 个 API 未检查返回值 | `alloc_chrdev_region` 等失败后 probe 仍返回 0，后续访问设备直接 Oops | 每个 API 调用后检查返回值，失败时 goto 到对应清理标签 |
| goto 清理标签定义但未被使用 | 由于未检查错误，标签处于不可达状态，编译器报警 | 补上错误检查后 goto 标签被正确使用，警告消失 |

## 代码规范要点

- probe 中每个可能失败的 API 必须检查返回值，失败走 goto 清理链
- goto 标签名对应出错位置：最后的操作出问题跳 `err_device`，往上一层跳 `err_class`，以此类推
- 设备结构体用 `devm_kzalloc` 分配（托管内存，自动释放）
- GPIO 用 `devm_gpiod_get`（托管 GPIO，自动释放）
- 模块末尾必须有 `MODULE_LICENSE("GPL")` / `MODULE_AUTHOR` / `MODULE_DESCRIPTION`
- 编译零警告是底线

## 下一阶段预告

**阶段 4：并发 + IO 模型** — 在 `comp_drv.c` 基础上添加：mutex 并发保护、阻塞/非阻塞 read（`wait_queue`）、poll/select 支持、SIGIO 异步通知、内核定时器实现 LED 闪烁、ioctl 配置闪烁周期。
