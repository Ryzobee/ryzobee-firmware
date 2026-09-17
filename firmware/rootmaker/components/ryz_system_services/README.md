# C 系统状态编排（P1a）

`ryz_system_services` 在独立的 boot-lifetime C task 中初始化并推进 Provisioning → NTP → OTA 前置条件。它不操作 LCD、触摸或 LVGL，不执行 Lua，也不代替这些模块各自的状态机。SPI/UI/Lua 的任务所有权由 Workbench 管理，不由本组件迁移。

## 接口与所有权

公共接口见 [ryz_system_services.h](include/ryz_system_services.h)：

- `start()` 成功只表示后台任务已建立。各依赖的初始化结果由快照报告；重复调用幂等，并发启动返回 `ESP_ERR_INVALID_STATE`，创建失败可以重试。
- `get_snapshot()` 复制最近一个完整周期的状态及当前命令状态，不通过调用它驱动后台工作。`network_valid`、`time_valid` 是取样有效性，不等于联网或时钟有效。
- `request_reprovision()` 投递现有的显式忘记网络并重新配网动作，单槽涵盖排队与执行。`requested != completed` 表示尚未完成；返回受理不等于凭据删除成功。完成 ID、结果及操作后的完整状态周期一起发布，新请求不改变上次完成结果。ID 仅在当前启动内有意义。
- `request_network()` 在同一槽加入 ON/OFF/OPEN_AP；显式 ON/OFF 先保存下次启动偏好再控制无线，保存失败保持当前无线状态并报告命令失败，运行错误不回滚已保存意图。三者均保留凭据，ON 完成只表示启动已受理，OPEN_AP 通过独立 `ryz_provisioning_open_portal()` 打开本地配网页，不改持久开关、不借用 ON 或忘记凭据路径。RPC/CLI 将 OPEN_AP 命名为 `setup`，要求当前 boot 身份；破坏性的 `reprovision` 仍单独要求确认。`network_operation` 含本次动作、上次完成动作及结果；`provisioning_started` 的历史名字仅表示初始启动/明确意图已成功落实，恢复的 OFF 也算，不是无线运行状态。缺少开关记录默认 ON，读取错误沿用初始化 5 秒退避；详见[开关持久化](../../../../docs/software/firmware-network-preference.md)。

编排任务唯一调用三个依赖的初始化、初始配网启动、重新配网及网络/时钟前置条件更新。Wi-Fi/NVS/netif 仍由 `ryz_provisioning` 管理，SNTP 回调由 `ryz_time` 管理，下载/取消由 `ryz_ota` 管理；既有 OTA 串口控制继续使用 OTA 的线程安全接口。

快照包含屏幕生成 AP QR 所需的本地 AP 密码，只供受信 C 调用者使用，不能整体写入日志、RPC 或 Lua。Workbench 仅输出调度计数、有效性和操作 ID/结果等非凭据字段。

## 推进和失败规则

正常周期结束后等待 200ms；该值不是包括 Wi-Fi/NVS 调用时间在内的硬实时期限。ESP Adapter 使用优先级 5、6144 字节任务栈和短时快照互斥锁，实际栈峰值、延迟及无线并存仍须实板测量。

初始化及 SNTP 启动/初次同步超时失败采用 5 秒退避，各依赖独立初始化。2026-09-14 起，首次无线启动改为一次性启动恢复决策：保存的 OFF/AP_READY/ONLINE 立即收束；保存的 STA 从 start 返回起最多等待 10 秒，失败或到期后调用独立的条件 AP 回退。回退会再次核实实时 STA，保留刚成功连接的网络，不直接使用无条件手动 OPEN_AP。保存的密码、AP 身份和开关均不变。`boot_network_settled` 单调收束，`boot_network_error` 保留结果，Workbench 在动画中等该字段；它不表示联网成功，也不替代 OTA 健康确认。详见[启动无线策略](../../../../docs/software/firmware-radio-boot-policy.md)。

初始化/启动/回退/采样失败可以结束启动等待以进入恢复 UI，但不会在首页后台再自动首次启动无线；初始化自身仍按原退避重试，之后由显式 ON/Setup 恢复。启动决策完成后，后续掉线仍由原配网模块处理，不重新套用开机 10 秒规则。离线立即绕过 SNTP 退避、撤销网络准入，正常掉线不抹除已有可信时钟。时间模块现由周期 ONLINE 调用观察固定 60 秒窗口，超时或异步无效时间先返回错误，让此处既有退避生效；详见[时间契约和真实组合检查](../ryz_time/README.md)。

即使网络 revision 不变，每周期仍重新读取时间并更新 OTA 前置条件。网络取样失败按离线处理，时钟取样失败按不可信处理。网络命令先锁存 OTA hold，owner 等待实际 worker 退出和清理确认才动 Wi-Fi/NVS；VERIFY 已开始则等待其最后 SDK 调用结束。失败不伪造 OFF，更新完整网络/前置条件后通过同一准入门释放 hold 并发布完成。任何明确命令都抑制后台初始开网重试，防止 OFF 被覆盖；不新增自动重试写操作。

依赖调用不持有编排快照锁。自动启动/回退期间持有独立的非阻塞命令准入门，竞争的命令快速返回忙；已经受理的手动命令始终优先。自动回退同样等待 OTA 真实清理；手动命令接管自动回退的 hold 时，不误释放新命令的 hold。错误日志只含固定阶段、错误码及抑制计数，并按阶段限流。

## 可选芯片指标

同一个系统任务通过内部 `ryz_system_metrics` 每秒尝试采集温度与双核 LOAD，
不创建新任务，不调用 I2C/GPIO，不加入依赖初始化或启动健康门。
网络调用或 SDK IPC 排队可能拖延这一周期，因此 `metrics.sampled_at_us` 保留真正的采样起点；
没有重新采样的周期不会刷新时间。UI 应按自身单调时钟过滤超过 2.5 秒的样本，
只把有效性与显示数值映射到页面，不因采样时间变化触发整页重绘。

- 温度来自 ESP32-S3 内部传感器，单位为十分之一摄氏度；不是环境温度或精密测温。
  ESP Adapter 使用新的 `esp_driver_tsens`、默认 -10..80°C 初始量程及 boot-lifetime
  单 Owner 句柄；SDK 可在硬件支持范围内调整量程。只有 SDK 成功且有限、位于硬件
  -40..125°C 范围的值才有效。安装/启用失败在下次采样重试，读失败不复用旧数值。
- LOAD 是两个核心各自最近两次样本间的非 idle 时间占比的平均值，范围 0..1000；一核完全忙、
  另一核完全 idle 为 500。它是调度运行时间统计，不是内存占用、主频、Owner 循环
  频率、功耗或硬实时中断负载测量。首轮缺测；时间倒退、计数重置/异常或平台失败
  撤销有效性并重建基线，两项错误分别报告。
  Adapter 复用 SDK 开机已经创建的两个 IPC 任务，依次通过 `esp_ipc_call_blocking`
  在目标核读取本核 idle 累计时间与 `esp_timer` 端点，不创建额外任务。当前 SDK
  仅在切换时累加 TCB 计数；IPC 回调运行前该核先前的 idle 段已完成记账，公共 getter
  以此双核端口的内核锁保护 U64 读取。两核分别计算间隔，不能把串行 IPC 的不同
  采样时刻共用一个分母。任一核 IPC/样本失败都不发布部分结果，并撤销两核基线。
  回调不阻塞、不嵌套 IPC、不取本组件锁；系统任务等待时不持有快照锁。SDK IPC
  的互斥/完成等待使用 `portMAX_DELAY`，没有本模块可保证的完成上界，网络 owner
  后续周期也可能因此延迟；缓存仍可读取，由消费者按原采样起点判过期。
- `sdkconfig.defaults` 开启 FreeRTOS runtime stats、ESP_TIMER 时钟源和 U64 计数，
  避免默认 U32 在约 71 分钟后回绕；SDK 会因此启用 trace/格式化支持及每任务计数，
  但本模块不遍历任务、获取任务名或调用格式化统计函数。现有配置文件需明确同步
  这三项后重新配置构建；只改 defaults 不会覆盖已经保存的 sdkconfig。

各项 `valid=false` 时对应数值清零，不能将零解释为测量结果；失败不改变网络、
NTP、OTA 前置条件。`get_snapshot()` 仍只复制缓存，不驱动传感器或调度器读取。

## 验证与未覆盖部分

启动策略新增的 Host 检查覆盖 10 秒边界、慢启动返回后的期限、OFF/AP/ONLINE 提前收束、刚联网保留、错误后无隐藏重启、OTA 清理及显式 OFF 接管。下述 19 项为原有测试基线，新增用例见 `test_system_services.py`；真实 core + ESP Adapter 与设备验证必须分开。

在 `firmware/rootmaker` 执行：

```sh
python3 tests/test_system_services.py -v
```

19 项 Host 测试编译实际 `ryz_system_services.c` 和 `ryz_system_metrics.c`，以 pthread、受控时钟及可注入依赖结果覆盖独立推进、启动竞态/重试、失败关闭、NTP 退避、共享命令槽与完整周期发布、日志限流、OTA 等待/清理失败、release 重入和 OFF 抑制自动开网，以及指标同任务单秒采样、阻塞期间缓存可读、各项独立失败/恢复、负载重置、时钟回退及两核不同采样区间。OPEN_AP 定向用例验证独立调用、排队/执行阶段和真实完成动作、OTA 清理前不执行、执行阻塞时公共快照仍可读、失败不自动重试，以及非法枚举不落到 ON/OFF/忘记凭据。完成时网络状态分类还验证同周期采样、取样失败显式无效、后续遥测及下一请求等待不覆盖历史。测试启用严格编译告警与 ASan/UBSan；Provisioning/OTA 依赖为替身，不证明 AP SDK 或真实凭据保存行为。分层 ESP/OTA/CLI 检查与目标构建见[网络控制证据](../../../../docs/software/firmware-network-control.md)。完整 V5 页面、取消/超时及持久化策略仍未闭合。

`python3 tests/test_system_metrics_esp.py -v` 的 5 项 Host 检查编译真实指标 ESP
Adapter 并使用锁定 SDK 的温度驱动及 IPC 公共头，覆盖安装/启用失败重试、失败输出清理、
范围/NaN、两个核心 IPC 内的 U64/独立时间传递、IPC 失败/错误核/缺失回调/半份样本
清理及拒绝 U32 构建。IPC-local getter 约束已先在旧直接远端读取实现上获得 RED，
再经当前 Adapter 得到 GREEN；它证明调用链位置，不模拟或证明真实调度器记账。
时钟、FreeRTOS、SDK 调用仍为替身，
不执行真实 SDK allocator、ADC/eFuse 校准或实机调度。目标配置、目标链接和实机
数值可信度必须分别验证，不能用 Host PASS 替代。

P1a 本身不拆分 Lua/UI 执行上下文；后续 [P1b 首段](../../../../docs/software/firmware-ui-owner.md) 已建立单一 UI Owner / 独立 Lua Worker。若本组件的 OS 任务尚未建立，Workbench 拒绝新 Lua 任务并持续重试，不把创建失败称为后台已运行。应用期间暂停系统菜单绘制但继续 Owner 状态/输入推进，系统健康确认仍在 UI 成功渲染后进行。导航/模态、屏幕停止入口、启动救援及实板验收继续由 P1b/P3 承担。
