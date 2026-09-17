# 板载 I²C 与加速度计

本记录属于 V5+ P6。实现范围是共享总线、LIS2DW12 驱动、后台真实样本缓存与只读诊断；不是整个 Tools/Test Hub，也不构成实物验收。最新完整目标实现前不烧录、不操作设备的要求优先。

## 硬件依据

固定参考库为 `ggadc/Ryzobee_arduino_esp32` 提交 `8ec48fd2d21dc35243ffebaf2d5820e249e8a8ea`。它在 [RootMaker 初始化](https://github.com/ggadc/Ryzobee_arduino_esp32/blob/8ec48fd2d21dc35243ffebaf2d5820e249e8a8ea/src/board/rootmaker/rootmaker.cpp)中让触摸和加速度计共享 I²C0，SDA41/SCL40；[传感器包装器](https://github.com/ggadc/Ryzobee_arduino_esp32/blob/8ec48fd2d21dc35243ffebaf2d5820e249e8a8ea/src/board/rootmaker/lis2dwtr/Rootmaker_Lis2dwtr.cpp)实际实例化 `LIS2DW12Sensor`。因此本实现不是仅从 Wiki 的 LIS2DWTR 名称猜测寄存器。

ST [官方轮询示例](https://github.com/STMicroelectronics/STMems_Standard_C_drivers/blob/master/lis2dw12_STdC/examples/lis2dw12_read_data_polling.c)、固定 [寄存器驱动 d0a476d](https://github.com/STMicroelectronics/lis2dw12-pid/blob/d0a476d31f5fed47333e23fd72927f78bdd6e9b3/lis2dw12_reg.c)、[DS11811](https://www.st.com/resource/en/datasheet/lis2dw12.pdf)和 [AN5038](https://www.st.com/resource/en/application_note/an5038-lis2dw12-alwayson-3d-accelerometer-stmicroelectronics.pdf)给出 ID、采样模式及单位换算依据。当前 PCB 的精确 BOM、SA0/CS 绑线、INT42 接法和坐标安装方向没有实物证据，所以只在两个合法 7 位地址 0x18/0x19 探测，并保留传感器物理轴；不假设它有陀螺仪，不输出猜测的屏幕姿态角。

## 共享总线与触摸

[`board_i2c.h`](../../firmware/rootmaker/components/ryz_board/include/board_i2c.h)是受信 C Interface，不向 Lua 暴露原始寄存器或 IDF 句柄。总线固定 I²C0 / GPIO41、40 / 100 kHz / glitch=7 / 原内部上拉配置；成功创建后保留至本次启动结束，不因触摸或 IMU 故障删除/重建。

最多懒注册三个设备句柄：触摸 0x15、IMU 0x18、IMU 0x19。所有注册和同步读写共用互斥锁；普通操作的锁获取预算与驱动传输 timeout 各为 20ms，不宣称端到端只需 20ms。读长度最多 32B、写最多 33B；有效输出缓冲在失败时清零，不保留半包。当前生成配置 tick 为 1kHz。

单地址 probe 接受 0x08–0x77，非阻塞取得总线锁后传给 IDF 3ms timeout；扫描调用者必须逐地址让出执行，不能整轮占锁。`NOT_FOUND` 才表示未应答，`TIMEOUT` 可能是争用或物理超时，不能显示成地址不存在。IDF 5.5.4 的 combined read 某些失败返回 `INVALID_STATE`，所以 IMU 先 probe 再读 ID，不把读取错误归为未插传感器。[IDF 5.5.4 I²C 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/i2c.html)

触摸驱动仍执行 RST2 拉低 10ms、拉高等待 50ms，INT1 输入上拉，A7 身份读取、FE DisAutoSleep 写入/回读、01 数据读取与原 D/T 事件归一化。仅把总线创建和传输移入板级所有者；失败仍撤销旧按下历史。LCD/BOE 初始化和 80MHz 屏幕 SPI 未改。

## IMU 与后台缓存

[`imu.h`](../../firmware/rootmaker/components/ryz_board/include/imu.h)提供同步、受信 C 的初始化、读取和诊断。配置前必须恰好一个合法地址返回 WHO_AM_I=0x44；两者均匹配视为歧义，超时或 ACK 后读取失败如实报错，不写猜测的器件。显式初始化每次重新识别/reset/config，以支持故障后的真正恢复。

固定首版采样配置为 HP14-bit、±2g、25Hz、LPF ODR/4、BDU、自增、FIFO bypass，无中断依赖；复位轮询有次数/时间限制，初始化后丢弃首个新 DRDY 样本。成功读取六字节连贯数据后输出物理轴 mg 与单调采集时间；没有新数据返回 `NOT_FINISHED`，失败不返回旧轴值。ID/配置成功不等于硬件自检通过。

[`ryz_sensors`](../../firmware/rootmaker/components/ryz_sensors/README.md) 在独立低优先级 Worker 执行 I²C 初始化/采样；UI Owner 只启动 Worker 并在创建失败时重试，查询仅复制缓存。普通无新数据不推进旧样本时间；真实故障或持续 1 秒无新数据撤销样本有效性、清空 XYZ，并在等待 5 秒后真正重新识别和配置。看门狗在采样返回后检查，不保证硬实时 1 秒触发；正常循环后等待 40ms 也不是无丢样 25Hz 调度承诺。可选传感器失败不阻断菜单、联网、Lua 或 OTA 系统健康确认。

Workbench 启动处及每轮 UI 循环渲染前保证 Worker 创建/重试，Lua 正在执行也不屏蔽。只读 `info.sensors` 是实际生产诊断消费者，不是向 Lua 开放硬件能力，也不冒充已接通 IMU 页面。它始终返回 Worker/快照状态；只有初始化有效时附器件信息，只有 `sample_valid` 时附 mg 轴值、单调采集时间和序号，历史时间及年龄要求 `ever_sampled`。查询不触发驱动读取；新字段 JSON 分配失败有清理，不发送半份传感器对象。

## 2026-09-05 验证记录

主任务在最终源码上独立执行：

- 在 `firmware/rootmaker` 执行 `python3 tests/test_board_i2c.py -v`：8/8 PASS，3.471s。实际编译 `board_i2c.c` 和 `touch.c`，覆盖失败可重试、惰性注册、并发排他、失败清包、probe 错误区别及原 TP 初始化/回读/事件行为；其中 TP 初始化失败包含 12 个子情形，不另充作 12 项 Python 测试。
- `python3 -m unittest discover -s firmware/rootmaker/tests -p test_imu.py -v`：6/6 PASS，1.302s。生产驱动 + 寄存器对端，覆盖唯一 ID、NACK/超时区别、有界复位、读回错误、重新配置、首次新样本丢弃、mg 换算、失败无旧轴及并发拒绝。
- `python3 -m unittest discover -s firmware/rootmaker/tests -p test_sensors.py -v`：7/7 PASS，1.909s。生产 Worker + pthread/受控时钟/IMU Adapter，覆盖启动失败与并发、I²C 阻塞期间 100 次纯缓存读取、无新数据不改时间、首次/旧样本 1 秒看门狗、5 秒重试及错误优先级。它不同时执行真实 IMU 驱动或 FreeRTOS Adapter。
- 上述新增合计 21 项，严格 C11/告警/Werror、ASan/UBSan。独立编译运行 `tests/touch_protocol_test.c` + `components/ryz_board/touch_protocol.c`：`TOUCH_PROTOCOL_PASS coordinate_pairs=58564`，另含状态/错误 fixture。
- `python3 -m unittest firmware/rootmaker/tests/test_runtime_owner.py firmware/rootmaker/tests/test_workbench_input.py firmware/rootmaker/tests/test_system_ui.py -v`：31/31 PASS，18.314s（Owner 17、输入 12、系统 UI/QR 2）。不是全部工程回归或新的 V5 像素验收。
- 冻结检查须在仓库根目录执行 `sh firmware/rootmaker/tools/check_lua_freeze.sh`，不能在 `firmware/rootmaker` 执行短路径；后者在首个相对文件检查处直接退出。本轮最终复核单独重跑正确目录，五项摘要均 OK、`LUA_COMPONENT_SOURCE_UNCHANGED`、退出码 0，PASS。屏幕仍为 80MHz，触摸低 10ms/高 50ms 和 100kHz 保持；没有改依赖锁、分区、Lua/LVGL schema、系统健康确认条件或设备数据。

最终在 `firmware/rootmaker` 激活 ESP-IDF 5.5.4 后执行：

```sh
idf.py -B /private/tmp/ryzobee-p3-catalog.cjYAiI/target \
  -D SDKCONFIG=/private/tmp/ryzobee-p3-catalog.cjYAiI/sdkconfig build
```

初次实现构建及最终稳定源码重配置/增量构建均 PASS。仅为组件管理所需系统查询使用已批准权限，不使用串口/烧录。目标 map 确认 Workbench → sensors → IMU → board I²C 的实际链接链，不是未引用库。`ryzobee_rootmaker.bin` 为 2,095,312B，3MiB 应用槽余 1,050,416B；SHA-256 `f0ac42f64bfbf9d8099c61504d366e1acc798d67728935911a94dccdf488777d`。APP 核心指纹仍为 `50d3f38fe2b2e4c0ab52ed4b0753bad410806a3b8f87563ff128fd7f97c57812`（脚本/目标定义一致），但整个固件镜像已改变，不能复用旧固件硬件证据。

Workbench 的 Worker 启动/JSON 薄接线经过静态交叉复核和目标编译；没有运行真实串口 RPC 或 Host 完整 Workbench。这里只支持 X-11/X-18 中的部分驱动/资源与错误语义条件；I²C 电气、触摸并存延迟、FreeRTOS 栈峰值、真传感器采样/自检、IMU 页面与完整 P6 均未通过。未读取、复位或烧录设备。

## 尚待完成

- Tools/I²C Scan/IMU 图表、参数选择器、引导测试和用户确认还需接入有效 V5+ 页面。后续第二段已在这些板级原语之上加入默认扫描 Worker、Workbench 与 CLI（开发资料未随公开源码分发），但页面/自定义引脚未完成；不重复实现已有扫描控制。
- RGB 已有固定参考：GPIO45、一颗 WS2812、GRB/800kHz；尚未实现。GPIO45 同时出现在扩展接口，后续须纳入占用规则，不能抢用屏幕 SPI2。
- 当前参考主线电池实现为空。其它分支的 PY32 寄存器不能当本板协议；电池状态应为“协议/驱动未确认”，不能伪造 0% 或“无电池”。仍缺当前 PCB/电源固件协议、测量比例与无电池判据。
- Host 和目标构建只证明各自范围。电气总线、真实采样方向/幅度、触摸并存、RGB 发光和电池状态，均留待完整固件实现并统一烧录后由用户验收。
