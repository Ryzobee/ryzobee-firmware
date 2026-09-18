# Lua 平台接口：静态字体、连接状态、IMU 与协程

2026-09-08。适用于当前 `ryz-app/1` 和 legacy Lua 入口。接口由 C 固件提供；本轮不包含烧录或真实无线/IMU验收。

2026-09-15：GPIO、轮询定时器、PWM、I²C、SPI、UART、ADC、日志与板载 LED 的新公共接口见[Lua 通用外设](firmware-lua-peripherals.md)。它们同样支持两种普通 Lua 入口，并共享本页的协程取消与无线配置边界；没有向 `ryz-neuro` 增加这些模块。Studio 的旧 `tools.*` 能力目录/SOP 不是这些新模块的使用文档，本轮未扩展其通用句柄分析或生成流程。

V0.10.2 新增通用 `fs.read/write/remove/list/info`，支持按脚本名隔离的持久数据文件。
详见 [Lua 文件存储](firmware-lua-filesystem.md)。不解除 `io/os/package` 限制，
也不是某个工具的专用 C 业务实现。

## 默认关闭 FreeType

`CONFIG_RYZ_LUA_FREETYPE` 默认 `n`。默认固件不嵌入七个 TTF，不链接实际 FreeType 引擎代码，不建立 Lua 动态字形缓存。

C/LVGL 与 Lua `ui` 共用原有静态 A8 字形；补齐 Noto Sans 400 的 14/16 px 和 600 的 24 px。现有字体名、字号、行高、布局保持不变，包括出厂 Monitor。静态字库离线用锁定的 FreeType 和原字体生成，不在设备运行时取模。Host 的离线字库导出/对照测试仍使用 FreeType，不能把这些测试程序当成产品模拟器。

需要时可在 `menuconfig → Ryzobee firmware → Enable the optional Lua FreeType font backend` 打开。它仅改变 Lua 字体后端，C 系统 UI 始终静态。本轮没有新增任意字体文件加载或任意字号的 Lua 接口。

Studio 当前 Host 配置固定为默认静态字体档位；字体开关已计入部署指纹。可选 FreeType 固件不能复用静态档位的仿真验收结果，需匹配相应 Host 配置后重新验证。

`font_engine_ready=false` 在默认构建中是正常状态；系统使用独立的 `ui_fonts_ready` 判断 UI 字体是否可用。分区表不变：释放的应用分区空间不会自动扩容 `/scripts`，Lua 源码上限仍为 16,384 字节、运行堆仍为 256 KiB，所有协程共享该堆。

## 只读连接状态

```lua
local wifi = require('wifi')
local ble = require('ble')
local connected, reason = wifi.is_connected()
```

两个模块都只开放无参数的 `is_connected()`：

| 返回 | 意义 |
| --- | --- |
| `true` | 已连接；Wi-Fi 是 STA 已获得 IPv4，不等于互联网可达 |
| `false` | 服务可读取，但当前未连接；保存过账号或 AP 有手机接入不算 STA 联网 |
| `false, 'unavailable'` | 平台服务尚不可用，或当前没有实现该后端 |

Wi-Fi 读取 C 配网组件的实时快照。AP 配网、账号保存、开关偏好和重连仍归现有 C 固件管理。Lua 无法扫描/配置/连接/断开无线网络，无法读取密码、配对密钥或原生句柄；本轮不新增 socket、HTTP、BLE 收发接口。

2026-09-14 源码已接入 C 所有的 `ryz_ble` Peripheral 服务。`ble.is_connected()` 只有在服务可读、链路存在、加密认证成功且绑定已确认持久保存时返回 `true`；单纯广播、物理链路或未保存的配对候选均不算成功。服务初始化失败仍返回 `false,'unavailable'`。首次配对须在设备屏幕核对并确认六位数字；开关、30 秒配对窗口、解绑/替换和重启恢复都由 C 控制，Lua 无配置权限。

上述 2026-09-14 版本只包含基础服务，未定义业务数据协议。2026-09-15 增加下节的
独立 HID 媒体控制开发能力；`ble` 仍只读，未开放自定义数据收发。安全连接不等于
某个业务 App 已就绪，BLE INFO 的 `SERVICE` 仍保留 `--` 的通用业务语义。

Studio 审核能力名为 `wifi.status`、`ble.status`。普通 Host 没有硬件模型：调用后返回 unavailable，并拒绝生成通过的硬件验收结果，即使 Lua 自己处理了 unavailable。

## HID 媒体控制（开发验证版，2026-09-15）

独立模块 `local hid = require('hid')`，不改变其他 Lua App 的用途，也不开放无线
配置、任意 Report Map、键盘/鼠标或自定义 GATT 数据收发。

| 方法 | 返回及含义 |
| --- | --- |
| `hid.is_ready()` | 当前安全、已绑定且手机已订阅 HID 报告时 `true`；未就绪 `false`；无后端 `false,'unavailable'`。不申请输入 lease。 |
| `hid.tap(key)` | `true` 仅表示 C owner 接受了一次按下/释放请求；失败 `nil,reason`，reason 为 `unavailable/not_ready/busy/failed`。不是手机执行确认。 |

`key` 必须是一个完整字符串：`play_pause`、`stop`、`next_track`、
`previous_track`、`mute`、`volume_up`、`volume_down`。额外参数、非字符串、
内嵌 NUL、未知名字触发 Lua 参数错误；不把任意字节转成 HID。

只有脚本显式调用 `tap` 才申请媒体输入；固件不会因启动、连接、重连或 BOOT
退出自动发送。脚本作者应按用户期望安排调用。每个 Job 首次 `tap` 申请独立
native lease，协程共享；同一时刻仅一条
pending/执行中的按键，忙时拒绝，不累积按键。脚本结束、错误、内存耗尽、超时、
BOOT 取消均关闭 lease：取消未 dispatch 请求，释放已 dispatch 按键。紧接着 `tap`
就退出脚本可能把它取消，交互脚本应保持事件循环；不能靠无限重试推断手机已执行。
真实后端释放失败会尝试断链，持久硬件故障仍可能需要用户恢复。

Host/模拟器没有手机连接或 HID 硬件模型时返回 `unavailable`，不能借模拟器通过
断言“手机首次发现或按键已成功”。本轮未新增 Studio 的 HID 可视化编辑/审核模型。
开发版暂无合法 VID/PID、电量百分比来源；PnP/电量读取明确报错，不能称为完整
HOGP 合规。实现/编译/实机验收分层见HID 交付记录（开发资料未随公开源码分发）。

## 用户显式初始化 IMU

系统不再启动自动初始化/采样 IMU 的后台任务，开机动画也不等它初始化。触摸使用的共享 I²C 总线照常由 C 板级组件管理。

```lua
local imu = require('imu')
local ok, reason = imu.init()
local sample, reason = imu.read()
local ok, reason = imu.deinit()
```

- 所有方法无参数；能力名 `imu.sample`。
- `init()` 成功返回 `true`；本脚本已初始化且设备仍 ready 时重复调用不会重复复位。故障后可显式重试。
- `read()` 只读取新样本，返回 `{x_mg,y_mg,z_mg,timestamp_us,sequence}`。轴值单位 mg（1000 mg = 1 g），对应传感器物理轴，不隐式旋转到屏幕坐标。
- `timestamp_us` 是单调采集完成时间，不是 UTC；它和 `sequence` 都是精确十进制字符串。锁定 Lua 使用 32 位整数和浮点，不能把大计数转换为 number 后仍宣称精确。
- 失败返回 `nil,reason`：`not_initialized`、`not_ready`（没有新样本，包括初始化后丢弃的首个新转换）、`unavailable` 或 `failed`。失败不返回旧轴数据。
- 固定使用已有 LIS2DW12 驱动：14 位高性能、±2 g、25 Hz、LPF ODR/4。它是加速度计，**不提供陀螺仪**。不开放寄存器/引脚配置或总线句柄。
- `deinit()` 停止转换并撤销就绪状态；重复调用安全。底层失败会返回 failed，不能据此声称硬件已断电。
- IMU 属于整个脚本 Job，协程间共享。正常结束、运行错误、超时和 BOOT 退出都会由 C 尝试停止转换、释放脚本使用权；失败时保留驱动错误诊断。不会删除/复位触摸共用的 I²C 总线。

初始化和读取是在 Lua worker 上执行的有界 C 调用，不在 UI owner 执行。Lua hook 不能打断正在执行的 C 调用；BOOT 取消会在其返回时继续处理。这里不声称完整初始化有 100 ms 上限，100 ms 仅是驱动的软复位轮询上限。

## 协程

提供全局 `coroutine`，也可 `require('coroutine')`；保留 `create/resume/yield/wrap/status/running/isyieldable/close`。

协程是单 VM 内的合作式执行，不是 FreeRTOS 并行任务。`coroutine.yield()` 交还调用者，由其显式 resume；`board.sleep_ms()` 暂停整个 Lua worker，不会自动运行其他协程。C 层配网和系统任务仍独立运行。

新线程继承冻结 VM 的 hook 与主线程 extraspace；所有线程共享内存限额、执行期限、终止状态及 native 资源。包装 resume/wrap/close 的前后检查，防止子线程把 BOOT、超时或 Host 完成信号变成可以吞掉的 `false,error`。普通 Lua 协程错误仍遵循标准返回语义。

`pcall/xpcall/debug/io/os/package/load` 等原有限制不变，用户 `__gc` 仍禁止，普通 `__close` 保留并受终止检查约束。不要依赖退出回调释放硬件；C Job 负责最终清理。专用 `ryz-neuro` 图运行时继续使用自己的调度器，本轮不向它开放这些新模块。Studio 的函数图约束也没有变为通用协程调度图解析器。

## 触摸取消标记（2026-09-14）

两种 Lua 入口的 `touch.read()` 都返回布尔字段 `interrupted`。为 `true` 时应取消当前点击/拖动捕获，不执行点击；该样本为 `event='none'` 且没有坐标，不是一次人为生成的 UP。显示设置应用或唤醒屏蔽触点时，已交付触点以此结束；暂时总线超时的既有恢复流程也可使用此标记。`ui.poll()` 已在 C 层处理取消。真实读取错误仍遵循原错误/超时预算，不伪装成成功。

## 最小使用示例

源码见 [lua_platform_demo.lua](../../firmware/rootmaker/scripts/lua_platform_demo.lua)。这是可选示例，不会写入出厂文件系统，也不会成为开机自启脚本。

先通过固件配网，脚本只等状态；IMU 采样与调度均显式进行。若服务 unavailable 或发生真实读取错误，示例打印原因并结束，不伪造数据。
