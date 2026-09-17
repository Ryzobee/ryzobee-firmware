# 固件 UI Owner 与 Lua Worker（P1b 首段）

2026-09-05：本文保留 P1b Owner/Worker 首段批次。后续导航核心/系统页隔离已推进，当前接口与证据见[第二段记录](./firmware-ui-navigation.md)；完整 P1/V5 页面、D2 全局退出/救援与实板条件仍以主计划（开发资料未随公开源码分发）为准。

## 所有权与生命周期

| 执行上下文 | 职责及约束 |
| --- | --- |
| 启动主任务 | 首先初始化 LCD，再提前启动同核 UI Owner 显示 V5 开机页；主任务继续 TP/文件系统等初始化，通过 release/acquire 完成信号后 Owner 才进入正常 UI。显式诊断构建保留串行 boot demo；未开放用户自启动语义。详见 开机动画（开发资料未随公开源码分发） |
| `wb_ui` | 固定在原 SPI 初始化所在核，优先级 5、16 KiB 栈；唯一执行 LCD/TP/demo/LVGL、周期采样输入、恢复菜单状态并发布完成 |
| `wb_lua` | 优先级 4、32 KiB 栈；运行 legacy / `ryz-app/1` / `ryz-neuro/1`，I/O 全部经同步单槽通道 |
| 系统编排与 RX/TX | 保留 P1a 独立编排；RX 处理请求/停止，TX 使用既有有界日志队列；Worker 输出回调不再读取无锁 TP/demo 状态 |

`start_job` 在同一 `job_lock` 临界区检查准入、分配 ID、占用 current 并排队，队列满则回滚。系统编排任务尚未建立时明确拒绝新任务，由持续运行的 UI 重试创建；不把“任务存在”误当作“已联网”。

Owner 暂停系统菜单绘制、重置输入并派发 Worker。应用占用画布时，Owner 继续采样触摸、更新网络/存储状态和诊断，但不插入 Home 重绘，避免破坏 neuro 分条 DMA。Worker 完成 `ui_close`（已挂载 UI 时）、`cleanup`、`lua_close` 后，通过完成队列交还 job；Owner 恢复状态/等待松手，再发布结果、释放 source 并更新 recent。Home 物理绘制保持延后。

新增诊断字段 `runtime_gateway_ready`、`ui_owner_cycles`、`ui_input_dropped`，只提供观察入口，不代表实板性能已通过。

## 同步借用与输入

[workbench_io](../../firmware/rootmaker/components/ryz_workbench/workbench_io.h) 的非零 ticket 对应单个待执行/执行中/已完成未归还请求，并发提交直接拒绝。Owner 用 acquire/release 发布参数与结果，调用者确认完成后归还槽位。请求、Lua 字符串、场景和取消上下文同步借用到 acknowledgement；取消也不能提前释放栈或 VM。

[I/O 执行器](../../firmware/rootmaker/components/ryz_runtime/include/ryz_runtime_io.h) 只调用受信 C，不从 Owner 进入 Lua。`UI_CLOSE`/`ABORT` 不受取消过滤。Worker 等待期间不持 `job_lock`；Owner 不等待 Worker 退出而停止处理 cleanup。LVGL checked mount/pump 保留原 wrapper，取消 callback 在返回前清除，flush 使用 checked show。

[workbench_input](../../firmware/rootmaker/components/ryz_workbench/workbench_input.h) 只由 Owner 访问，后台物理采样统一进入这里：

- APP 使用 16 槽 FIFO，DOWN/UP 保序、尾部 MOVE 合并；无事件返回最新有效状态和 NONE。溢出报告一次错误、清队列并等待释放。
- legacy `touch.read()` 和 neuro 电平输入保持 current-sample：不积压错过的 tap，事件按消费者连续成功读取归一，不使用后台物理读已消耗的边沿。
- 应用开始、非瞬时错误或溢出后等待物理释放，旧手指不会成为新应用点击。APP 路径最多屏蔽连续 8 次 `ESP_ERR_TIMEOUT`：已交付的 raw 按压在恢复前保持 `pressed=true`、`event=none` 且不复用旧坐标；`ui.poll()` 同时取消旧控件 capture。恢复首样本按物理状态归一为 MOVE 或 UP，不合成 release。
- 第 9 次连续 `ESP_ERR_TIMEOUT` 锁存为终止错误，后到的成功采样不能覆盖；Lua 下一次读取先收到真实错误。正常物理取样周期 20ms，驱动先做一次短重试，Owner 仍失败后 500ms 再试；真实响应与高负载手感尚未形成长期基准。

## 取消归因修复

真实集成测试先复现 5 个失败：NEURO 初次 paint、后续 stripe、sample 在排队期间取消后被当成普通 I/O 错误；原生层与 APP 核心的超时起点也可不同。修复为三个 native 返回点重新检查控制状态，并在 APP/NEURO 核心首次读取时钟时冻结原生层 deadline 到相同毫秒边界。

5 个 RED 全部转绿；新增无取消的底层失败用例仍保持 runtime failure，没有把所有硬件错误强行改成 stopped/timeout。未留下临时日志或修改第三方 Lua。

## 2026-09-05 软件验证

未注明时在 `firmware/rootmaker` 运行。

| 命令 | 结果与证据边界 |
| --- | --- |
| `python3 tests/test_workbench_io.py -v` | 8 项通过，真实通道 + pthread，含并发、借用生命周期、迟到 ticket、2048 轮发布和 wrap |
| `python3 tests/test_workbench_input.py -v` | 12 项通过，真实输入核心/归一算法，含 FIFO/current-sample、错误、溢出及跨应用释放 |
| `python3 tests/test_runtime_owner.py -v` | 17 项通过；真实三类 runtime、冻结 Lua、通道及 I/O 执行器，BSP/LVGL 是线程与存活检查 spies。覆盖计算时 Owner 继续推进、停止/超时、UI_CLOSE/ABORT 后 VM 释放及错误分类，原 5 项 RED→GREEN；不是像素证据 |
| `sh tools/test_app_runtime.sh`、`sh tools/test_ui_host.sh`、`sh tools/test_neuro_contract.sh` | APP_RUNTIME、UI_HOST 和神经核心回归通过；非真屏/串口停止时限/FreeRTOS 集成证据 |
| `python3 tests/test_workbench_lifecycle.py -v`、`python3 tests/test_system_ui.py -v`、`python3 tests/test_system_services.py -v`、`python3 tests/test_display_driver.py -v` | 分别 1、2、8、1 项通过，覆盖旧生命周期、系统 UI/QR、P1a 与实际显示 C 驱动 Host 检查 |
| 仓库根目录 `sh firmware/rootmaker/tools/check_lua_freeze.sh` | 依赖/defaults/分区/第三方 Lua 冻结通过；一次误从固件目录调用未通过路径检查，改为根目录重跑通过，未改冻结值 |
| 仓库根目录 `npm run test --workspace @ryzobee/studio-host -- src/simulation/runtime-fingerprint.test.ts src/simulation/app-simulator.test.ts`；`npm run typecheck --workspace @ryzobee/studio-host` | 2 文件 7 项通过，真实 shell/Studio 指纹、Host 仿真/receipt/live 回归；Host 两套 TS 检查通过 |

新增 C Host 检查启用严格告警、ASan/UBSan；并发检查使用 pthread，不运行真实 FreeRTOS task loop。

### 目标构建与指纹

隔离目录 `/private/tmp/ryzobee-v5-p1b.Nl1u61/build`；ESP-IDF 5.5.4 / ESP32-S3 / 0.9.0 完整构建与修复后增量构建通过，无烧录，保留旧产物/配置。

- `ryzobee_rootmaker.bin`：2,074,336 字节；3 MiB 槽余 1,071,392 字节（约 34%）；SHA-256 `c8075475fbf9bf597a3f9a6ded0df28d33438378e43f1ebfc678b1339a0156c9`。
- APP core `24b436d25567b14398e32797324bfbadb7bec2faa87d4104bfe57b46a6ef2d1b`：实际 CMake 编译参数、shell、Studio 契约一致。
- NEURO core `da9a9c349e738962db4342e22a3f2c90a06277bff1f72a9139a120562678b137`：保持原核心/Lua/MAXSTACK 契约，去除 CMake 误加的 APP LVGL suffix。
- APP 指纹/Host 缓存纳入 I/O Adapter、握手/输入核心和 LVGL 依赖/配置；旧缓存/receipt 须重新生成，不继承旧通过记录。
- 应用和 bootloader 生成配置启用 rollback，OTA URL 仍空；LCD 80MHz、面板/TP 初始化未改。临时路径可能被清理，正式交付需归档。

## 首段结束时的剩余条件（历史，当前进度见文首）

导航栈、模态、页面代次和 D2 全局停止交互仍未实施。D2 已非阻塞询问偏好，但尚未选定、修改 Figma 或绑定屏幕入口；安全启动/用户脚本恢复属于 P3。37 个新增 Host 用例只部分支持 X-01/X-02/X-14/X-16，不通过整项验收。

本轮未做新像素截图、真屏/触摸/扫码、UART 停止、无线、OTA/rollback 或故障注入；实际栈峰值、长期稳定性和调度延迟未测。有限通道不代表底层调用有硬性时限，当前 IDF SPI acquire/queue/drain 可能无限等待，硬件故障下的停止/恢复仍是必须解决的缺口。

下一步先补与手势无关的导航、页面代次及模态输入；D2 确认后先补 Figma 再绑定退出，继续 P2 同源像素。总目标保持 active，不重新开始计划阶段。
