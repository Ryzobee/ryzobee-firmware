# 固件导航与系统输入隔离（P1b 第二段）

2026-09-05：导航核心和当前系统页面的代次/输入隔离已实现并通过软件验证。原显示资产、布局、AP 返回 Home、Lua 接口和 D2 未确认手势保持不变。完整 P1/V5 页面与实板验收仍未完成，进度见执行计划（开发资料未随公开源码分发）。

## 接口与实际接入

[导航核心](../../firmware/rootmaker/components/ryz_system_ui/include/ryz_ui_navigation.h) 是 Owner 独占、无动态分配、无 LVGL/BSP 依赖的 C 模块。八层页面栈和独立单模态槽支持 ROOT/PUSH/REPLACE/BACK/PRESENT/DISMISS；调用携带动作创建时捕获的 `{generation, route, modal}`，不能在迟到回调执行时重新读取当前 token。过期、非法、满栈、根页 BACK、模态冲突均拒绝且保留历史。

导航/reset/invalidate 递增代次，同名页面不能复活旧 token。模态打开时底层页面 token 无效，BACK 优先关闭模态；关闭后需要新页面 token。64 位耗尽在递增前永久闭锁，不回绕；此极限只有静态核对，没有执行 2^64 次跳转的证据。

[系统 UI](../../firmware/rootmaker/components/ryz_system_ui/include/ryz_system_ui.h) 已使用该核心管理真实状态：Home/Apps/Settings 使用 ROOT，AP 使用 PUSH；AP Back 仍显式返回 Home。`page_token()` / `navigate(expected, route)` 拒绝旧页面跳转。切页、应用交接、输入或渲染失败清捕获和待交付意图；网络/存储的普通数值刷新不换代。

输入仅在完整传输成功且页面代次一致后开放；在途渲染回调也不能提前取意图。Workbench 对物理读取成功和失败都调用 `process_sample(sample, read_status, snapshot, action_out)`，只有成功才执行动作。失败忽略可能未初始化的 sample、清 action，要求物理 UP 或 `NONE + pressed=false` 后才能重新布防。捕获绑定一次接触，移出目标永久取消；空白 DOWN、移出后的重复 DOWN 不能重新瞄准按钮，切页后的 UP 不穿透。

服务操作 ID 与页面 token 分离：旧 UI 事件失效不自动取消已受理的后台业务。模态目前只完成公共状态/令牌规则，尚未接入真实 LVGL 弹窗命中、背景吞触摸或滚动/长按认领。

## 复现与验证

原 REPROVISION 的 UP 先设置动作再渲染。SHOW 超时后 Workbench 先 take 到局部动作，再 reset 全局状态，最后仍执行局部动作。真实 UI + display spy 的最小检查在 `action == NONE` 失败，根任务重复复现。修复为失败撤销代次/意图、仅成功交付；原 RED 断言保留并转绿。物理读失败原本没有清捕获，现已通过 Workbench 实际采用的公共入口覆盖失败、恢复持按、释放和新点击；Host 不执行原 FreeRTOS task loop。

未注明时在 `firmware/rootmaker` 运行：

| 命令 | 实得与范围 |
| --- | --- |
| `python3 tests/test_ui_navigation.py -v` | 7 项通过，真实核心、严格 C11/ASan/UBSan，含八层栈压力、2048 轮同名页面往返与 1024 轮 reset/失效 |
| `python3 tests/test_system_ui.py -v` | 2 项聚合通过：真实 UI 的 34 条 C 行为（新增 11 条，含原 RED）及 QR Adapter。原断言保留，fixture 增加真实首次渲染和 released NONE |
| `python3 tests/test_runtime_owner.py -v` | 17 项通过；真实 Lua/runtime 与替代 BSP/LVGL，非 FreeRTOS 或像素验收 |
| `python3 tests/test_workbench_io.py -v`、`python3 tests/test_workbench_input.py -v` | 分别 8、12 项通过 |
| `python3 tests/test_workbench_lifecycle.py -v`、`python3 tests/test_system_services.py -v`、`python3 tests/test_display_driver.py -v` | 分别 1、8、1 项通过 |
| 根目录 `sh firmware/rootmaker/tools/check_lua_freeze.sh` | 通过；曾误从固件目录调用导致路径检查失败，按要求根目录重跑 |

ESP-IDF 5.5.4 / ESP32-S3 / 0.9.0 完整构建通过：目录 `/private/tmp/ryzobee-v5-nav.9unO5m/build`，SDKCONFIG `/private/tmp/ryzobee-v5-nav.9unO5m/sdkconfig`，使用仓库 defaults。初次配置被 macOS 沙箱拒绝 `psutil` 查询父进程，获准放行同一构建后完成；未改 SDK。

- bin 2,075,584 字节，3 MiB 槽余 1,070,144 字节（约 34%），比 Owner/Worker 批次增加 1,248 字节。
- SHA-256：`556eae564be360d07fc4f4c092d4ae1f61c8f54eac9fc29d71c73cb9676612fd`。
- APP core `24b436d25567b14398e32797324bfbadb7bec2faa87d4104bfe57b46a6ef2d1b`：当前 shell、实际编译参数及 bin 内嵌值一致。本批未改其覆盖的应用执行文件。
- 应用/bootloader 均启用 rollback；OTA URL 仍空；LCD 80MHz、面板/TP 初始化、Lua 冻结和分区未改。临时产物未作正式归档。

## 剩余工作

本批仅支持 X-01/X-14/X-16 子条件，不通过完整用例。下一步接入真实 LVGL 页面/模态，补滚动与 1.2 秒长按的互斥认领和真实像素；D2 确认后先改 Figma 再绑定全局退出。SPI 硬件故障下有界停止、资源峰值和长期稳定性仍需独立证据。

本轮未修改 Figma、未新截图、未枚举/操作设备或烧录，不把 Host 检查算作整页 V5 交付。
