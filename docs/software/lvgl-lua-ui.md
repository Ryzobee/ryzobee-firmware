# LVGL 与 Lua `ui` V1

## 状态与证据边界

RootMaker 0.9.0 已把 `lvgl/lvgl ==9.5.0` 接入 ESP-IDF 固件，依赖同时由 [`components/ryz_lvgl/idf_component.yml`](../../firmware/rootmaker/components/ryz_lvgl/idf_component.yml) 的精确版本约束和 [`dependencies.lock`](../../firmware/rootmaker/dependencies.lock) 的解析结果固定。公开 Lua 接口仍属于 `ryz-app/1` / `ryz-capabilities/1`，用 `ui.mount()`、`ui.update()` 和 `ui.poll()` 表达受控场景，没有暴露 LVGL 指针、全局对象树、任意 style/property、C 回调或自定义 allocator。

Studio 能力推导将 `ui.mount` / `ui.update` 对应到 `display.canvas`，将 `ui.poll` 对应到 `display.canvas` + `touch.read`，因此不需要为 V1 扩充 capability catalog 版本。这些 metadata 不取代固件中的 owner 和参数校验。

当前证据包括 Host 约束测试、真实 `display.c` 传输边界测试和 ESP-IDF 目标构建。这证明依赖能解析、接口能编译链接，并且共享 Lua 运行时遵守场景/触摸 owner 合约；**本轮未烧录开发板，未验收真屏字形、圆角、色彩、触摸命中、刷新时序、长时间稳定性和运行时堆余量**。

官方依赖与集成参考：[Espressif Component Registry 中的 LVGL 9.5.0](https://components.espressif.com/components/lvgl/lvgl/versions/9.5.0/versions?language=en)、[LVGL 的 ESP-IDF 集成文档](https://lvgl.io/docs/open/integration/chip_vendors/espressif/add_lvgl_to_esp32_idf_project)。

## 架构与所有权

```text
Lua ui.mount(scene) / ui.update(generation, patches) / ui.poll()
                 |
                 v
ryz_runtime: schema、资源限制、hit-test、screen/input owner
                 |
                 v
Workbench 单槽同步 I/O 通道：Lua Worker -> UI Owner
                 |
                 v
ryz_lvgl: 单任务生命周期、LVGL object、partial flush
                 |
                 v
ryz_board: 240x240 RGB565 canvas -> ST7789 80 MHz SPI
```

`ryz_runtime` 先把 Lua table 解析为纯 C 的 `ryz_ui_scene_t`，二次边界校验后才交给 `ryz_lvgl`。`ryz_lvgl` 负责 LVGL 初始化、对象创建、渲染和释放，并限定 mount / update / pump / release 都在同一 FreeRTOS task 调用。触摸事件由共享 C 运行时做有界 button hit-test，不把 Lua 回调注册到 LVGL 对象。

系统 Home/Apps/Settings 仍属于 `ryz_system_ui` 和原有有界 renderer，未迁移到 LVGL。P1b 已将三条 Lua 路径放到独立 Worker；上述 LVGL 操作经同步通道交给原 SPI 核上的 UI Owner，Lua 字符串/场景借用至完成确认，不改变公开 Lua schema。Workbench 在任务开始前暂停系统菜单绘制，但 Owner 继续输入取样和状态推进；无论脚本正常结束、报错还是超时，运行时都会在 `lua_close()` 之前释放已挂载场景，Workbench 再恢复 Home 并等待一次物理松手，避免残留手势跨任务传递。通道、输入模式、checked flush、取消与验证边界见 [UI Owner 记录](./firmware-ui-owner.md)；屏幕全局退出尚未接通。

单个 Lua VM 内的互斥规则是：

- `ui.mount()` 声明 `ui.scene` 为 screen owner；不能再调用 `display.clear()` / `rect()` / `text()` / `show()`。
- 先调用任一原始 `display` 绘图接口后，同一 VM 也不能再 `ui.mount()`。
- `ui.poll()` 声明 `ui.events` 为 input owner；不能再调用 `touch.read()`。
- 先调用 `touch.read()` 后，同一 VM 不能再 `ui.poll()`。
- 活动 UI 场景不与 `touch.demo(true)` 混用。

冲突会直接进入 Lua runtime error，而不是由后调用者静默抢占资源。

## 受信 C 系统页面接入（2026-09-05）

新增 [`ryz_lvgl_system.h`](../../firmware/rootmaker/components/ryz_lvgl/include/ryz_lvgl_system.h)，与现有 Lua Adapter 共用 `ryz_lvgl.c` 内唯一 display、时钟、240×16 RGB565 buffer 和 checked flush；没有第二个 LVGL task/显示驱动，也没有扩大 Lua V1 schema。

- `system_create` 取得系统租约并创建借用的 candidate root，最多一个活动 root 加一个 candidate。系统页面可直接组合 LVGL 子对象，不受 Lua 32 对象/40 字节限制，但仍受同一个 64 KiB 池约束。
- `system_commit` 消费合法 candidate：成功替换活动 root；失败删除 candidate、恢复旧逻辑 root，系统租约仍保留。参数/非 Owner 错误不消费对象。
- `system_pump` 仅推进已提交系统页面的 LVGL timer/input/draw；没有到期刷新时也可能返回成功，所以不能凭一次 `pump == ESP_OK` 宣告物理画面已修复。
- `system_release` 释放系统 root/candidate 和租约，不刷黑屏；共享 display/buffer 保留复用。调用者必须先撤销 indev、timer、异步引用和交互代次，然后才释放字体/图像资源。

Lua/System 租约互斥，非 Owner 或重入生命周期调用被拒绝；原 `ryz_lvgl_release()` 不能释放系统租约。非阻塞原子入口保护覆盖同步取消/删除等回调的重入，不能据此允许任意任务直接调用 LVGL。回调只发业务意图，不应直接切换/删除页面。

`cancelled_out` 区分本次实际观察到的取消与底层传输超时，底层 DMA drain 失败保持原错误。失败后的逻辑回退不修复已部分传输的屏幕；页面调用者须闭锁输入，完成成功重绘、检查字体 sticky status 和导航 token 后再发布动作。LVGL 内部任意分配失败仍不保证可恢复。

本段只提供共享显示接入并保持 Lua 生产路径；`ryz_system_ui` 的旧画面尚未替换，系统 indev/公共组件/41 状态及 Apps 动作仍待接线。APP 指纹纳入新 header 并随生产 Adapter 更新，Host/目标使用同一材料；公开 Lua 契约与 neuro 指纹不变。最新“不完整不烧录”门禁见执行计划（开发资料未随公开源码分发），下文历史设备命令不是本轮授权。

## `ui.mount(scene)` V1 schema

`ui.mount(scene)` 在挂载成功后返回当前 Lua VM 内递增的正整数 `generation`。运行时会先完整校验 candidate scene，然后再交给 C Adapter；非法 scene 不会替换已发布的运行时 scene。

scene 只接受下列字段，未知字段直接拒绝：

| 字段 | 要求 |
| --- | --- |
| `id` | 必填；1–24 字节；正则 `[a-z][a-z0-9_]*` |
| `background` | 必填；0–65535 的 RGB565 整数 |
| `objects` | 必填；从 1 开始、无空洞的 Lua 数组；0–32 项 |

object 也拒绝未知字段；同一 scene 内 `id` 必须唯一：

| 字段 | 必填 | 值/默认值 |
| --- | --- | --- |
| `id` | 是 | 1–24 字节，`[a-z][a-z0-9_]*` |
| `kind` | 是 | `"box"` / `"label"` / `"button"` |
| `x`, `y` | 是 | 0–239 |
| `width`, `height` | 是 | 1–240；对象必须完整落在 240×240 内 |
| `text` | label/button 是 | 1–40 字节的可打印 ASCII；box 不接受非空 text |
| `foreground` | 否 | RGB565，默认 `0xFFFF` |
| `background` | 否 | RGB565，默认 `0x0000` |
| `border` | 否 | RGB565，默认为 `foreground` |
| `border_width` | 否 | 0–2，默认 0 |
| `radius` | 否 | 0–32，默认 0 |
| `font` | 否 | 下表中的字体名，默认 `"body_16"` |
| `align` | 否 | `"left"` / `"center"` / `"right"`；label 默认 left，其他默认 center |
| `visible` | 否 | boolean，默认 `true` |
| `enabled` | 否 | boolean，默认 `true` |

Lua 页面通过品牌字体 Adapter（开发资料未随公开源码分发）使用真实 FreeType ASCII 栅格。2026-09-08 为出厂 Monitor 补齐以下受限字号，复用已审计的字体子集，不开放任意字体路径或尺寸：

| 字体名 | 字体 / 字重 / 像素字号 |
| --- | --- |
| `body_16` | Noto Sans Regular 400 / 16px |
| `title_24` | Noto Sans SemiBold 600 / 24px |
| `button_14` | Noto Sans SemiBold 600 / 14px |
| `display_20` | Teko SemiBold 600 / 20px |
| `mono_12`, `mono_14` | Roboto Mono Regular 400 / 12px、14px |
| `mono_semibold_12`, `mono_semibold_14` | Roboto Mono SemiBold 600 / 12px、14px |
| `mono_medium_12` | Roboto Mono Medium 500 / 12px |
| `body_12`, `body_14` | Noto Sans Regular 400 / 12px、14px |
| `button_12` | Noto Sans SemiBold 600 / 12px |
| `medium_14` | Noto Sans Medium 500 / 14px |

每个 Lua scene 仅创建实际引用的 face/size 缓存，并在退出或换场景时释放。像素字号不等于行框高度，布局须留出真实行高；字体初始化失败拒绝挂载，不静默回退。C/LVGL 原生系统菜单仍使用静态字模，未改为 FreeType。此 API 扩展已纳入 APP 源码指纹，必须使用配套固件；普通 app-host 的粗粒度字图不是字体像素验收依据，应使用 `host/pixels` 同源渲染器。

`visible=false` 不渲染也不命中；`enabled=false` 在 ESP 渲染中会降低对象不透明度，button 不命中。对象数组的后项在重叠区域优先命中。

## `ui.update(generation, patches)` 同代次更新

用于更新已挂载场景的文字、对象颜色和可见/可用状态，不重建对象树。正整数 `generation` 必须来自当前 VM 最近一次成功的 mount，范围 `1..2147483647`；成功返回同一个整数，不产生新代次。需要更改布局时重新 mount；代次耗尽时拒绝 mount，不回绕。

`patches` 为不含元表的稠密数组，0–32 项；每项也是无元表 table，只接受：

| 字段 | 约束 |
| --- | --- |
| `id` | 必填；已有对象 ID；同批不得重复 |
| `text` | 可选；仅 label/button；严格 string、1–40 字节可打印 ASCII |
| `foreground`, `background`, `border` | 可选；严格整数 RGB565，0–65535 |
| `visible`, `enabled` | 可选；严格 boolean |

不能更新场景背景、对象顺序、kind、位置/尺寸、字体、对齐、圆角或线宽，也不能新增/删除对象。先验证整批，再最多调用一次 Owner Adapter；非法后项不会造成前项提前绘制。空批次、只有 ID 或值完全相同的批次不调用 Adapter、不刷新。

label/box 的文字和颜色变化保留当前捕获；任意 button 的实际变化撤销旧捕获，避免同一按压跨越按钮内容/状态变化。隐藏/禁用正在捕获的滚动 box、重新 mount 也撤销捕获。原位更新固定 C 字符串存储，不借用 Lua 字符串；原位不等于小矩形 SPI 更新，当前同步刷新仍会使完整活动 root 无效并传输。

```lua
local generation = ui.mount(scene)
-- 状态与画面对应时，先发布 mark，再更新文字。
board.mark("count", count)
assert(ui.update(generation, {{id="counter", text="COUNT " .. count}}) == generation)
```

显示/字体失败或取消可能发生在像素已经改变之后，不承诺回滚。C Adapter 把失败树标记为不可再 pump/update；运行时结束该 Lua 并按 Owner 确认顺序释放场景。原生 C 调用者必须成功重挂载或释放后才能继续。脚本结束进入 closing 期后，`__gc` 中的 mount/update/poll 等原生调用被拒绝，不允许清理后重建 UI，也不覆盖原脚本执行结果。

最新退出策略：App/legacy 已在共享沙箱拒绝注册任何非 nil raw `__gc`，因此用户GC回调不再进入上述退出路径；closing继续作为原生调用的防御。普通元表、GC内存回收和可受hook检查的 `__close` 保留。兼容影响与实际RED→GREEN见[沙箱修复](./firmware-lua-sandbox.md)；不是任意C调用/硬件故障都可强制退出的证明。

真实示例 [`ui_update_demo.lua`](../../firmware/rootmaker/scripts/ui_update_demo.lua) 是 API 诊断脚本，不是出厂工具或 V5 产品画板；具体验证与像素证据见本批记录（开发资料未随公开源码分发）。

## `ui.poll()` V1 事件

`ui.poll()` 要求已有活动 scene。每次调用先 pump LVGL timer，再从 C 层读取一个指针 sample。无完整激活时返回 `nil`；在可见、可用 button 上按下，并在同一 button 上松手时返回：

```lua
{
  kind = "activate",
  scene = "home",
  generation = 1,
  id = "apps",
  at_ms = 120,
}
```

`at_ms` 是该 pointer sample 的设备启动毫秒时间。无坐标的 UP sample 使用最近一次有效坐标完成 hit-test。移出原 button 后松手、在空白处按下或 disabled/hidden button 上操作均返回 `nil`。

`ui.poll()` 每次最多消费一个 sample，消费者停顿过久可能造成底层队列溢出和捕获取消；业务循环应短时 sleep 并持续 poll。不提供长按、多点、focus、键盘/编码器和 Lua 回调。

### 可选垂直拖动（2026-09-08）

`ui.poll("logs")` 为本次轮询选择一个已挂载的 `kind="box"` 对象作为垂直滚动区域；无参数或 `nil` 保持原按钮行为。最多一个参数，严格 string、1–24 字节合法 ID；未知 ID、label/button 或其它类型被拒绝。不改变 `ui.mount` 对象 schema，不开放 LVGL 指针，不需要混用互斥的 `touch.read()`。

在可见、可用 box 内按下且未命中按钮时捕获拖动。首段垂直移动累计达到 6px 后产生事件，随后只返回相对上次事件的非零 `dy`；有坐标的 UP 也可补交最后一段，没有 MOVE 的普通点击不会激活 box：

```lua
local event = ui.poll("logs")
-- 拖动时：{kind="scroll", scene="monitor", generation=1,
--          id="logs", at_ms=140, dy=20}
-- dy > 0：手指向下；dy < 0：手指向上，单位为原始 240px 画布像素。
```

按钮优先；在日志中开始的手势移到按钮上松手也不产生 activate。拖出 box 后仍保留本次捕获，直到松手、中断、mount、按钮变化、box 隐藏/禁用或轮询取消/切换滚动区域。标签更新不打断拖动。API 不保存日志、不实现惯性或整页平移；历史容量、行偏移与自动跟随由 Lua 维护。固件与 SDL 编译同一段事件判定，必须使用包含此扩展的配套固件。

### Monitor 全屏扩展（2026-09-08，1.1.0）

以下为新增的受限固件/SDL 能力，覆盖上文原 V1 的对应限制；不开放任意 LVGL 属性或文件路径。使用配套新固件，原 `box` / `label` / `button` 脚本保持兼容。

- `kind="viewport"`：必填 `content_height`，范围为视口高度至 1024；可选 `scroll_y`，范围 `0..content_height-height`。视口自身必须完整落在 240×240 内。
- 可选 `parent`：必须引用对象数组中更早出现的 viewport ID，仅允许一层。子对象坐标相对未滚动内容，必须完整落在其内容矩形；显示和命中都按同一 `scroll_y` 平移并裁剪。隐藏或禁用父视口也禁止子按钮命中。固定操作栏不放进该视口。
- `kind="image"`：必填白名单 `asset`。`rootmaker_a_face` 固定 88×88，`monitor_divider` 固定 232×2；不能传外部路径、任意指针或缩放尺寸。图形来自本地 Figma 导出的 SVG，经离线生成器转为 RGB565 调色板的 I8 常量图。
- label/button 仍限 1–40 字节，但允许 ASCII 文本中的 LF 换行；其它控制字符和非 ASCII 文本仍拒绝。可选 `line_height` 为 0（自然行高）或 12–64，不改变字形像素字号；裁剪与行框需实际截图验收。
- `ui.update` 支持 viewport 的 `scroll_y`、box 的 `y` / `height` / `width`；box 必须仍在原画布/父内容内。宽度最小为 1；零值进度使用 `visible=false`，不以零宽度绕过几何校验。其它 kind 的几何仍不可修改。整批校验后才调用 Adapter，不因前项有效就提前绘制。
- `ui.poll(viewport_id)` 允许从子按钮按下开始候选点击；垂直累计 6px 后转为拖动，永不再激活该次按钮。已经开始的滚动允许更新子按钮颜色和滚动偏移；隐藏/禁用、挂载新页、中断、切换轮询区域仍撤销捕获。UP 后无新 DOWN 的 MOVE 不接受为拖动。

Monitor 用 224×176 区域显示 11 行 12px 日志，16px 行距，只有 PAUSED 允许历史拖动；业务状态仍由 Lua 和原 Monitor 服务管理，不写进 C 页面。普通 Studio/app-host 的简化语义预览未新增硬件或这两类对象的像素实现，不可代替已接通真实生产源码的桌面 SDL 模拟器。最新截图、测试及 16 KiB 源码容量记录见 Monitor V5 全屏交付（开发资料未随公开源码分发）。

## 渲染路径与固定资源

LVGL 以 RGB565、single software draw unit、no-OS 模式构建，启用 label；配置保留 Montserrat 16/24，但 Lua 对象实际绑定品牌字体。渲染使用单个 240×16 行的 partial draw buffer；flush Adapter 将 native RGB565 像素复制到现有 C 显示画布，只在 LVGL 最后一个 flush area 时提交累计的 dirty window。

配置可直接核算的开销为：

- 现有 240×240 RGB565 画布：115,200 字节 PSRAM。
- 现有两个 240×16 RGB565 DMA 条带：15,360 字节内部 DMA-capable RAM。
- LVGL built-in allocator：构建期固定保留 64 KiB 内部 RAM 池；scene 对象从该池分配。
- LVGL partial draw buffer：7,680 字节 PSRAM，首次 `ui.mount()` 时申请并保留供后续场景复用。
- Lua VM 堆：上限 256 KiB PSRAM，不包含上述 C/LVGL/显示开销。

上述数字不包含字体常量的 Flash 体积、驱动结构、FreeRTOS task/stack 和堆碎片。64 KiB 池是配置容量，不代表最大 32 对象场景已在实板上验证有足够余量。尤其是 A→B 原子重挂载会短暂同时保留两棵对象树；当前没有 target/实板峰值压力数据，而且 LVGL 内部并非所有子分配失败都能安全返回，因此 V1 不承诺内存耗尽时仍可回滚。32 项是接口上限，不是已验证容量结论。

## Host preview 边界

当前 Studio/app-host 的语义 Host Adapter 不运行 LVGL 像素渲染。它使用与 ESP 端相同的 `app_runtime.c` 做 schema、owner、hit-test 和事件判定，再把 scene/update 语义转成既有 `clear` / `rect` / `text` 操作：边框近似为外矩形+内矩形，16/24px 字体近似为 5×7 位图字体的 scale 2/3。独立 `host/pixels` 使用真实库软件栅格，尚未接入此 Studio 预览或完整 V5 页面；两类证据不能混用。

因此 Host 可验证场景是否能表达、文本是否出现、button 是否产生确定性 `activate` 事件；它会按对象宽高降低位图倍率、截短或隐藏放不下的文本，保证预览操作不越过对象边界。但它不能验证 LVGL 的实际字形尺寸、基线、裁剪结果、圆角、不透明度、抗锯齿、RGB565 像素细节和 ST7789 真实刷新。Host frame 是语义预览/诊断信号，不是像素 parity oracle。

## 构建与验证

在固件目录下运行 Host 合约和真实 display driver 回归：

```zsh
cd /path/to/ryzobee-firmware/firmware/rootmaker
sh tools/test_app_runtime.sh
sh tools/build_app_host.sh
python3 tests/test_display_driver.py -v
```

可用虚拟时间与定时 pointer 重放 UI demo。下面的按下/松手坐标位于 `apps` button 内：

```zsh
printf 'pointer 20 down 30 90\npointer 40 up\nend 100\n' | \
  build-host/app-host scripts/ui_demo.lua
```

从仓库根目录校验第三方冻结文件：

```zsh
cd /path/to/ryzobee-firmware
sh firmware/rootmaker/tools/check_lua_freeze.sh
```

目标构建必须使用 ESP-IDF 5.5.x：

```zsh
cd /path/to/ryzobee-firmware/firmware/rootmaker
source /path/to/espvm/espvm.sh
espvm use v5.5.4
idf.py build
```

上述 Host 命令不需设备；`idf.py build` 证明 ESP32-S3 目标的编译/链接与分区容量，但不等于烧录、真屏目视或真实触摸验证。要做实板验收，先按 README 的分区/备份要求烧录 0.9.0，再以交互超时运行 demo：

```zsh
python tools/board_lua.py --port /dev/cu.usbmodemPORT \
  exec scripts/ui_demo.lua --timeout-ms 5000
```

运行后在 5 秒内点击任一 button；成功时应输出 `UI_ACTIVATE`，随后释放 scene 并恢复系统 Home。该命令是待执行验收步骤，不是本轮已获得的实板结果。
# 2026-09-08 字体后端更新

默认已关闭 FreeType：C 与 Lua UI 共用静态字形，现有字体名/字号/像素不变。
FreeType 仅作为可选 Lua 构建后端保留。详见 [Lua 平台接口](firmware-lua-platform.md)。
