# Ryzobee 同源 LVGL / SDL 模拟器

2026-09-14 / 开机 Wi-Fi 进度：同源 `v5_boot_ui_test` 覆盖 14 个英文状态/异常条件，输出 `boot-wifi-*.bmp` 原始 240×240 图。首帧与 Figma SVG 像素差为零，文字仅刷新底部，原往返进度条保留。31/31 CTest 通过；状态是测试输入，不是桌面真实 Wi-Fi 连接。[截图和设备验证边界](../../../../docs/software/evidence/boot-wifi-scan-20260914.md)。

2026-09-14 / HOME 回退：固定 WLAN 趋势，温度/LOAD/PSRAM 同时显示，点击 WLAN 进入 Wi-Fi；指标点击不再切换。断网不隐藏本地指标。真实 SDL 回放、31/31 CTest 和在线/离线两张 240×240 截图通过；模拟数值不代表设备读数。本轮未重开交互窗口，已编译并烧录固件。[证据](../../../../docs/software/evidence/home-rollback-20260914.md)。下方单指标交互为历史记录。

2026-09-14 / V5 滑动同步：新增三种缩放下 INFO 连续回拖/固定底部 9 组、普通 Wi-Fi/BLE/OTA 按钮防滑动点击 36 组和 UPDATE AVAILABLE 长短说明滚动；原 APPS/Display/HOME/Monitor 回归保留。31/31 CTest、9 张 240×240 截图及目标构建通过，已重开新模拟器到 APPS。DEMO 说明不代表真实更新源；未烧录。[本轮验证记录](../../../../docs/software/evidence/v5-gesture-sync-20260914.md)。

2026-09-14 / 亮度预览：同步真实 C UI 的预览与放弃闭环，DISPLAY 窗口标题显示 `PWM intent` 的 DEMO 数值。不会调节 Mac 背光，像素仍为原生 RGB565；SAVE 仍拒绝模拟硬件持久化。31/31 CTest 和 240×240 状态截图检查通过，已重开最新窗口。[验证记录](../../../../docs/software/evidence/brightness-preview-20260914.md)。

2026-09-14 / APPS 横滑误选：同步固件的行点击取消和 80ms 稳定按压高亮；新增 30 条真实 SDL 事件回放，覆盖三种缩放下左右长/短滑、斜滑、折返、之后正常点击及 Back。31/31 CTest 和 240×240 截图检查通过，已重开新窗口，未烧录。[本轮证据与实机边界](../../../../docs/software/evidence/apps-swipe-20260914.md)。

2026-09-14 / HOME 新交互：点击 WLAN、温度、LOAD、PSRAM 选择单一趋势，切换立即清空，重复选择保留；`N` 键只模拟 NET 断网/恢复。通过与固件相同的 `workbench_telemetry.c` 记录明确的 SIMULATED 样本，未连接设备。31/31 CTest、四指标/空白/OFFLINE 原始 240×240 截图和原生窗口点击检查见[本次交付记录](../../../../docs/software/evidence/v5-home-metrics-20260914.md)。

本机 GUI 可交互运行固件的 240×240 C UI，以及三份真实出厂 Lua：`tool_monitor.lua`、`tool_i2c.lua`、`tool_hardware.lua`。不是重新写一套网页或以截图代替 UI。C 页面和 Lua 布局均使用固件源码；服务状态、总线字节和传感器样本明确使用虚拟数据，不连接设备。上方带日期的测试数量与交付记录属于当时结果，不是本轮总数。

2026-09-14：已同步本地 Figma 最新 V5 的 APPS/详情连续滚动、三类 INFO、Scripts、Fault 和 Display。原生共 32 个路由（BLE 新状态复用原路由）；当前 31 项 CTest 通过。已重开 macOS 原生窗口实点检查 Display 草稿、放弃和返回，详见[本次同步交付与边界](../../../../docs/software/evidence/v5-firmware-sync-20260914.md)。设置第四项为 Display；DEMO 保存明确拒绝硬件写入，不伪装 Saved。本文下方日期记录保留历史范围。

## 启动和重建

本机已生成 `firmware/rootmaker/build-host/simulator/v5_simulator.app`，可在 Finder 打开。

在仓库根目录：

```sh
# 首次需要已配置的真实 ESP-IDF build（不要传 sdkconfig 文件）。
sh firmware/rootmaker/tools/build_ui_simulator.sh /absolute/path/to/idf-build

# 后续复用已选择的配置；重新编译并运行全部检查。
sh firmware/rootmaker/tools/build_ui_simulator.sh

open firmware/rootmaker/build-host/simulator/v5_simulator.app
```

当前使用仓库的 `firmware/rootmaker/build`，不再引用早期临时目录。不要用自造 `lv_conf.h` 跳过配置一致性检查。**修改源码后退出旧窗口再重新启动，运行中的进程不会自动热更新。** 窗口标题包含入口和 main 编译时间，便于辨认旧进程。2026-09-08 曾确认可见窗口仍是 9 月 6 日启动的进程，即使磁盘中的二进制已重建也不会自动更新。

依赖：SDL2、CMake、Ninja、C/C++ 编译器及项目已锁定的 managed components；本次没有新安装系统包。macOS 已验证，Linux 启动命令仅供后续移植使用，未实测。

## 操作

- 鼠标左键：C 页面映射到生产 `ryz_system_ui_process_sample`；三份工具映射到生产 Lua `ui.poll()`。支持短按、长按、移动取消和窗口失焦取消。
- `1` / `2` / `3`：1× / 2× / 3× 整数放大，最近邻，不重新排版；原始画布始终 240×240。
- `←` / `→`：切换 32 个路由的样例快照，**是预览快捷键，不是设备导航功能**。
- `Home`：取消当前 Lua 并回首页；系统 UI 中直接回首页。
- `B`：系统 UI 中重播原固件开机画面（模拟等待 1.6 秒，只有底部进度段移动）；Lua 中连续按住 5 秒取消脚本，释放或窗口失焦会清零计时。桌面时钟验证不等于实机 BOOT 时序验收。
- `M` / `I` / `T`：分别运行真实出厂 Monitor、I²C Tools、Device Test。
- Monitor：11 行视口、最多 64 条完成行。LIVE 跟随新输入；PAUSE 冻结当前快照，按住左键下拖回看、上推回到最新；RESUME 返回实时末尾，CLEAR 清空历史。后台超过容量的旧行会淘汰，不是无限日志存储。CONFIG 包含 SYSTEM/UART、PORT、BAUD 和 FORMAT 草稿。
- I²C：初始扫描虚拟内部总线；PINS 中显式选择 SDA/SCL、100/400 kHz，再点 SCAN PINS 才扫描虚拟外接总线。结果仅表示地址 ACK，不识别器件型号。结果区可拖动，CANCEL、重扫和未完成状态由同一 Lua 脚本处理。
- Device Test：LCD、9 格触摸、模拟 IMU 运动、RGB 确认和 Battery 状态；RGB 要 START 后才提交虚拟颜色，完成帧后手动 MATCH，退出前熄灭并关闭。Battery 没有可验证读取接口，只能 CHECK/SKIP，不展示虚构电量或自动 PASS。
- `Escape`：任一 Lua 工具中取消并回 HOME；系统菜单中退出模拟器。

系统窗口标注 DEMO，工具窗口标注 `SIMULATED / NO DEVICE`。首页温度、负载、PSRAM、存储、时间均为内存样例，不读取 Mac 或开发板的真实数值。Monitor 接收带 SIM 标识的虚拟日志/UART 字节；I²C 内部总线仅有虚拟 `0x15` ACK，外接总线另有 `0x50`、`0x68`；IMU 是按模拟时间产生的新序号样本，RGB 只是异步虚拟帧。模拟器不打开真实串口、网络、GPIO，也不控制 Mac 的 LED/背光。未提供的外设返回 unavailable。

APPS 包含三份可运行出厂工具和两份用于原生菜单回归的 `sample_*.lua` 占位。**2 秒长按 RUN** 会发出原 UI 请求，三份工具按所选文件交给真实 Lua VM；占位脚本运行及全部删除请求被拒绝。每次运行都是新 Lua 作业，退出释放界面和资源，不保留前一作业的结果/草稿。网络/OTA 请求仅打印，BLE 和脚本写入能力未接入。方向键预览成功页不代表执行了业务操作。二维码只编码样例凭据，不代表有可连接的 AP。

可从命令行直达工具（先退出旧窗口，选择一条运行）：

```sh
open firmware/rootmaker/build-host/simulator/v5_simulator.app --args --monitor
open firmware/rootmaker/build-host/simulator/v5_simulator.app --args --i2c
open firmware/rootmaker/build-host/simulator/v5_simulator.app --args --device-test
```

三份工具使用实际 `app_runtime.c`、公共 Lua 绑定、Lua VM、LVGL 和静态字体。`factory_demo.c` 只适配鼠标/时钟、虚拟字节和通用外设回调，不实现工具状态机，不调用旧 `tools.*` 或链接 `ryz_monitor_stream.c`。布局、日志历史、扫描/引脚草稿、测试步骤/结果都在 Lua。主线程先释放 C 页面的 LVGL 所有权再运行 Lua，退出/取消后归还，不能在原生回调内重入 Lua。普通 Studio app-host 的硬件不可用边界保持不变。

CMake 从 `fs/tool_{monitor,i2c,hardware}.lua` 原样生成字节数组、版本和 SHA256；文件变化会自动触发重新配置和编译。APPS 详情显示该版本/哈希，每次作业退出时终端打印文件名、版本、哈希和字节数。没有第二套工具 UI 源码；`scripts/` 是维护源，打包到 `fs/` 后才成为模拟器和固件的共同输入。

## 同源范围和验证边界

复用 `host/pixels` 的库：生产页面、导航、输入捕获、状态映射、开机动画、静态字体、A8 图标、QR 编码、LVGL 屏幕租约和 RGB565 提交。只增加 SDL 窗口、鼠标适配、时间推进及显式样例数据，没有修改生产 UI 或设备配置。

LVGL 使用 `dependencies.lock` 中的 **9.5.0**，从真实设备 build 配置生成 Host 配置。SDL 只显示 BSP 已提交的帧，不直接创建第二个 LVGL display，不绕过系统 owner 调用 `lv_timer_handler`。

自动检查输出在 `build-host/simulator/`，包含 `sdl-*.bmp`、原始 RGB565 和原有页面测试产物。`v5_sdl_simulator` 使用 SDL dummy/software 后端、同一个事件处理函数，验证：

- 开机 400ms 前后只有 80×2 的底部动画区域变化；随后进入 HOME。
- 1×/2×/3× 鼠标导航；SDL 渲染器读回像素与固件 RGB565 帧逐像素一致。
- APPS 列表、详情、半程长按和 3 秒单次请求；不存在的 sample 脚本运行仍被隔离。
- APPS 的真实 Monitor 入口、Lua 暂停/清空/参数/选择器/APPLY 回首页；同进程重复运行、Esc 和关闭窗口释放，以及失焦后的迟到松手取消。
- SDL DOWN/MOVE/UP 进入同源 `ui.poll("logs")`，PAUSED 拖动能回看原先已离屏的日志；公共 Lua 字节 fixture 另覆盖分片输入、64 行淘汰、冻结快照、暂停/恢复和清空。
- 失焦后的迟到松手不能切页；缩放后重新呈现恢复输入门禁。
- 32 个路由各自实际渲染并核对路由没有被周期状态映射意外替换。

`factory_sdl_simulator` 另从实际 SDL 事件验证 I/T 和 APPS 三秒长按入口、I²C 引脚/频率草稿与扫描/拖动、Device Test 五项与 Summary、失焦防误点、Esc 和模拟 BOOT 5 秒退出，以及同进程重新打开。输出 `sdl-i2c-*`、`sdl-test-*` 和返回 HOME 的原生 240×240 RGB565/BMP，不是另一套测试页面。

只重跑工具公共入口和 SDL 交互（先编译最新产物）：

```sh
cmake --build firmware/rootmaker/build-host/simulator --target v5_simulator monitor_script_pixels i2c_script_pixels hardware_script_pixels --parallel 8
ctest --test-dir firmware/rootmaker/build-host/simulator -R '^(v5_sdl_simulator|factory_sdl_simulator|monitor_script_.*|i2c_script_.*|hardware_script_.*|pixel_host_config)$' --output-on-failure
```

`pixel_host_config` 检查目标配置、固件实际 C UI 源文件、Lua 同源编译，以及三份内嵌脚本与出厂文件逐字节相同、版本/SHA256 一致。当前测试数量以 CTest 实际输出为准，历史数量不代表当前构建；保持 ASan/UBSan。桌面测试仍不能验证 SPI 80MHz、ST7789 初始化/拖影、CST816 中断延迟、DMA、FreeRTOS 调度、实屏色彩、扫码成功率、BLE、OTA 或真实外设行为。没有烧录或访问设备；目标板内存与时延不能由桌面样本推断。

2026-09-08 补交：**28/28 CTest PASS**，另有配置比较单测 **5/5**。已退出 9 月 6 日旧进程，重开当前构建并在 macOS 原生窗口实点检查 Monitor、暂停、PINS 和 UART 参数；240×240 截图为 `sdl-monitor-*.bmp`。不再把无窗口 fixture 截图当成交互模拟器交付。日志 `/tmp/ryz-sdl-monitor-full.log`，详细内容见 [Monitor 交付记录](../../../../docs/software/firmware-monitor-lua.md)。

2026-09-06 本机结果：最终 **19/19 CTest PASS**；macOS 原生窗口截图检查了 HOME、APPS 列表、详情及 1× SETTINGS，实点验证列表/详情/返回/设置切页。没有在本轮重新读取或修改 Figma，也没有将模拟器性能当作 MCU 性能。构建验证日志为 `/private/tmp/ryz-sdl-final-check.log`，测试明细同时保存在 `build-host/simulator/Testing/Temporary/LastTest.log`。

## 官方参考仓库

[lvgl/lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode) 已递归浅克隆到忽略目录 `host/vendor/lv_port_pc_vscode`：

- 主仓库：`2ab7eefcc2de348723d2b13c658f39dd4972134f`
- LVGL 子模块：`f45e3a3e87f517ee134d52d01ad7c8a887efd199`，**9.6.0-dev**
- FreeRTOS 子模块：`7d6890e6501a014e06d5b1fb97c994a71299c6f5`，本次未启用

官方 Widgets 示例已独立编译、打开原生窗口并截图检查。它证明本机 SDL 环境可用，不作为产品配置一致性的证据；Ryzobee 不链接它的 9.6 开发版。

当前克隆的三个本地适配保存在 `upstream-macos.patch`：CMake 导出目录改为 BUILD_INTERFACE；开启已启用矢量绘图所要求的 ThorVG；输出 macOS `.app` 以便原生窗口检查。没有宣称原仓库零修改可编译。

复现官方示例（在一个新的、尚不存在的 vendor 目录克隆；已有克隆不要覆盖）：

```sh
git clone --recurse-submodules https://github.com/lvgl/lv_port_pc_vscode.git firmware/rootmaker/host/vendor/lv_port_pc_vscode
git -C firmware/rootmaker/host/vendor/lv_port_pc_vscode checkout 2ab7eefcc2de348723d2b13c658f39dd4972134f
git -C firmware/rootmaker/host/vendor/lv_port_pc_vscode submodule update --init --recursive
git -C firmware/rootmaker/host/vendor/lv_port_pc_vscode apply ../../simulator/upstream-macos.patch
cmake -S firmware/rootmaker/host/vendor/lv_port_pc_vscode -B firmware/rootmaker/host/vendor/lv_port_pc_vscode/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSE_FREERTOS=OFF
cmake --build firmware/rootmaker/host/vendor/lv_port_pc_vscode/build --parallel 8
open firmware/rootmaker/host/vendor/lv_port_pc_vscode/bin/main.app
```

本轮按 `diagnosing-bugs` 用失败配置/事件回归定位问题；桌面适配的输入失效恢复被新增测试锁定。诊断日志代码已移除。
