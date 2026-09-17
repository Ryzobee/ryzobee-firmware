# Lua GC 回调准入与退出修复

2026-09-08 更新：App/legacy 已加入受控协程和只读无线状态、显式 IMU API，见 [Lua 平台接口](firmware-lua-platform.md)。下文是 GC 修复的历史记录；“不提供 coroutine” 不再描述当前版本，GC 限制保持不变。

2026-09-05：App 与 legacy 共享沙箱已拒绝不可中断的用户 `__gc` 注册；普通元表和 `__close` 保留。对应 P1/P8 的退出子条件，不是整个固件或所有 C 调用的有界证明。未操作设备或烧录。

## 原因与兼容规则

锁定 Lua 的 `lgc.c` / GCTM 执行最终器时设置 `allowhook=0`，`lua_sethook` 不会重新开启该开关。原生 closing guard 只能阻止原生调用，纯 Lua `while true do end` 不进入该检查。App/legacy 真实 VM 均已复现：正文结束后停在 `lua_close`，50ms 执行期限无效，最终由 2s Host 进程 watchdog 终止。去掉全部 UI 仍复现；相同死循环放在正文可被现有 hook 结束，排除了 UI 清理与 Host 时钟作为根因。

共享私有 [lua_sandbox.h](../../firmware/rootmaker/components/ryz_runtime/lua_sandbox.h) 在打开 base 后、用户代码执行前包装 `setmetatable`：

- 用 raw 查询拒绝候选元表中任何非 nil 的 `__gc`，包括 false、数值和可调用 table，不运行元表的 `__index`。
- 其余行为委托原 base 函数，保留对象/元表身份、修改、弱表、保护元表、nil 卸载、返回值及普通参数检查；不开放保存原函数的 C upvalue。
- 给已经安装且未登记的元表晚加 `__gc` 不会登记最终器，这是锁定 VM 的既有语义；再次安装或装到新对象仍经过门禁。
- `__close` 保留。正常返回及错误展开中的 hook 仍可生效，取消/超时会中断其 Lua 循环；它不是不受约束的退出回调。
- GC 内存回收和标准库私有 UBox 的可信 C 最终器仍存在。只限制用户 GC 回调，不关闭收集器、不修改冻结 VM、不删除整个元表机制。

这是一项有意的沙箱兼容变化：旧脚本注册 `__gc` 时现在返回 runtime error `__gc finalizers are not supported; use explicit cleanup`，不会执行该回调。普通显式逻辑、cancel/stop 与 C Job 回收继续使用。Lua 源文件仍可编辑/删除，出厂脚本没有额外权限。

前提是当前允许的 base/table/string/math/utf8 和受控 native 模块，不提供 debug/io/package/coroutine/动态 loader 或可修改最终化 userdata。`getmetatable('')` 不会将字符串登记为最终器；把其带 `__gc` 的元表装到 table 仍被拒。gmatch/random/buffer 的私有对象不可从允许的 API 取得。以后新增 native 模块若暴露此类 userdata，必须重新审查；不能仅凭此门禁宣布任意 C 调用都可停止。

## 本轮实际验证

- `python3 firmware/rootmaker/tests/test_runtime_owner.py -v`：35/35 PASS。新增 App/legacy 两个原始 RED→GREEN 和六类双方言回归；每条潜在卡死场景独立进程 2s watchdog。包含 false/可调用表/共享/重装/晚加、raw 查询、普通元表/弱表/保护/内置 buffer，以及 `__close` 的正常返回、用户错误、既有超时、OOM、嵌套、取消与字符串元表路径。断言最终 phase、Owner cleanup 一次、通道 idle 与测试堆归零，不只检查首先抛出的错误。
- `sh firmware/rootmaker/tools/test_app_runtime.sh`：全部 PASS；原 UI 最终器用例已改为明确的注册拒绝，不能误称仍执行 GC 后才拦住原生调用。
- `python3 firmware/rootmaker/tests/test_app_tools.py -v`：11/11 PASS，真实 App facade + Lua；覆盖零工具调用、此前已成功提交仍由 C cleanup、显式 cancel/stop 原 token、普通元表与正常 `__close`。平台 cleanup 为替身，不冒充物理资源释放。
- `IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf python3 firmware/rootmaker/tests/test_script_run_integration.py -v`：6/6 PASS。首次未设置 IDF_PATH，测试未开始；按项目要求设置既有 checkout 后通过，没有安装 SDK。
- Studio catalog/evaluator/provider-agent/app-simulator/runtime-fingerprint 五文件133/133 PASS，Host typecheck PASS。首次与目标构建并行时两项到达原5s测试期限；构建结束后不改测试期限、原命令复跑全绿。SOP 更新到1.6.0，16334B，不扩大硬件权限；新规则在既有 Runtime contract 中集中说明。
- 真库像素 CTest4/4、旧UI_HOST与第三方冻结检查 PASS；没有改变字形、图标或面板初始化。测试使用真实冻结Lua/LVGL/FreeType与Host平台替代，不含真实FreeRTOS、无线、SPI/TP或用户验收。

ESP-IDF5.5.4/ESP32-S3 目标构建通过。当前隔离目录 `/private/tmp/ryzobee-p3-catalog.cjYAiI/target`；app2121584B，3MiB槽余1024144B（33%）。bin SHA-256 `5c3cca393dfcd6fbb8d7ad6fba36458676530ad3ab76cf3dad20609cb23e2f89`。App指纹 `83781fab661a53b5637341bf6de901b62eaf438980b925cdbda3dfbff5ab8e82`，目标编译参数/shell/Studio一致并包含新header；neuro无base元表入口，未改其源码或指纹材料。

完整P1仍缺屏幕全局退出/救援规则与硬件故障恢复，完整P8仍缺默认应用及其他选定能力；X-02/X-16保持部分支持。下一步回到可见产品闭环，不重复审计已经冻结的GC注册规则。
