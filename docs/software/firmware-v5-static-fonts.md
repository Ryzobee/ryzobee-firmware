# V5 系统静态字库与 Lua 动态字体分工

2026-09-06：已实现、编译及 Host 验证，**未烧录**。

## 当前分工

- C 系统 UI：`ryz_v5_widgets` 改用 `ryz_v5_font` 的静态 LVGL 字体。
  字形、字距、基线和 A8 灰度在构建前生成并放入 Flash，不再进行运行时
  FreeType 取模、逐字号全 ASCII 预检或动态字形缓存分配。
- Lua 用户应用：保留原有 `ryz_font` / FreeType / 动态 LVGL 字体路径和全部
  7 份嵌入字体；当前 Lua `ui` 仍只允许 `body_16`、`title_24` 与可打印 ASCII。
  **本轮没有新增任意字体加载、任意字号或 Unicode 的 Lua 接口。**
  后续可设计受限的字体句柄、尺寸选择、测量和释放接口，不能直接暴露 FT 指针。
- 启动时对 FreeType 的既有初始化与诊断字段保持不变，没有擅自改变 Lua 生命周期。
  但 C 系统页面已经不依赖它成功初始化，Host 单独验证了该条件。

这里取模的是单个字形，不是整页或固定字符串截图。时间、温度、SSID、脚本名等
动态文字仍由静态字形排版，原页面对象、交互及数据逻辑不变。

## 字库与视觉保持

清单在 `firmware/rootmaker/components/ryz_system_ui/ryz_v5_font_palette.def`：
共 38 组字体×字号，包含原 37 组连续页面调色板及交互分支使用的额外字号。
每组覆盖 U+0020–007E；8 组 Teko 另外覆盖 U+00B0，温度仍使用 `°` + `C`。
总计 3,618 个字形、284,238 字节 A8 像素，元数据另计。

取模重用仓库锁定的 FreeType 2.14.3、原 TTF 子集和生产 `ryz_font_rasterize`，
保持原来的 hinting、基线、整数 advance、无 kerning 行为和所有 256 级灰度。
字体来源及许可证仍见 `components/ryz_font/assets/`。

`ryz_v5_font.c` 将字体声明为 `static_bitmap=1`；当前 LVGL 9.5 软件绘制路径可
直接从静态 A8 数据混色。普通 draw-buffer 回调仍完整实现，供变换绘制及测试使用。
缺失字号、非覆盖字符返回错误，不静默回退到 FreeType 或另一个字形。

生成数据为 `ryz_v5_font_data.inc`。在已配置的同源 Host 目录运行：

```sh
cmake --build /absolute/path/to/host-build --target export_v5_fonts
/absolute/path/to/host-build/export_v5_fonts \
  firmware/rootmaker/components/ryz_system_ui/ryz_v5_font_data.inc
cmake --build /absolute/path/to/host-build
ctest --test-dir /absolute/path/to/host-build --output-on-failure
```

生成器仅用于离线构建，不进固件；正常目标构建直接编译已生成的数据。
新增系统字号时，更新清单、重新生成、通过字形及整页测试，再交付固件。
静态数据、选择器与 Widgets 已加入 MCU / Shell / Studio / 独立测试的四份运行时指纹。

## 图标合并与 focus 着色

现有 27 份图标已全部是 A8 透明蒙版，共 19,982 字节，无重复位图。
Logo 的多条 SVG 路径早已合成单张 40×40 蒙版；显示时不是逐路径创建 LVGL 对象。
`ryz_v5_image` 对同一资源调用 LVGL image recolor，选中橙色、普通灰色等状态不保存多份图片。
补充回归确认 HOME/SETTINGS 切换后沿用同一图标数据，仅颜色变化。

固定图层只有在合并后确实有收益且不需要独立显示/着色时才合并。检查发现：

- 顶部四份图标的像素数据共 738 字节，合为 59×16 后变成 944 字节，且 Wi-Fi
  不能再与 BLE/电池独立着色，所以不合并。
- 电池外框、极耳共 226 字节；合为 21×12 后像素为 252 字节。目标图像描述符
  为 28 字节，少一个描述符后只是几字节级 Flash 收益（对齐另计），虽可少一个
  LVGL 对象，本轮不为这一微小收益改动已验证资源。

本轮保持这些已复用的图标资源，没有新增颜色版本，没有将大面积透明背景打包进大图。

## 已执行验证

- 同源 Host CTest：**16/16 PASS**，ASan / UBSan；最后补充 focus 检查后，
  `v5_pixels`、`v5_static_font`、`v5_shell_ui` 再次 **3/3 PASS**。
- 全部 3,618 字形的度量和 284,238 个 A8 像素与保留的 FreeType 路径一致；
  也检查普通缓冲路径的 stride、padding、空格、控制字符及缺失字形错误。
- 在未初始化 FreeType 且拒绝堆分配时，全部静态字形可直接读取。
  `v5_pixels` 在 FreeType 未初始化状态下完成全部系统页面渲染。
- **44 张 240×240 RGB565 帧与修改前逐字节一致**：37 个页面/状态及 7 个遥测夹具。
  已查看当前首页温度截图，字体、摄氏度及上一轮顶部图标保持原效果。
- 同一主机、同一 ASan/UBSan 页面夹具的三次墙钟运行时间：
  旧版 `2.79 / 1.16 / 1.10 s`，静态版 `0.73 / 0.62 / 0.62 s`。
  中位数 `1.16 → 0.62 s`。包含进程启动、37 页导出、重复切页及清理，
  **不是单页耗时，也不是设备切页延迟**。
- 该夹具跟踪的堆峰值 `293,272 → 25,144 B`，减少 268,128 B；不包含所有
  FreeType 内部堆或 MCU 总堆，不能作为实机 PSRAM 测量值。
- 目标静态字体对象文件只引用 `lv_draw_buf_reshape` / `memcpy` / `memset`，
  没有 FreeType 或堆分配引用；最终 ELF 仍包含 `FT_Load_Char` 供动态路径使用。
- 指纹测试 **6/6 PASS**，Lua/依赖/分区冻结检查与 `git diff --check` 通过。
- ESP-IDF 5.5.4 / ESP32-S3 编译链接成功；应用 **2,167,792 B**，相比上一轮
  1,822,864 B 增加 **344,928 B（约 337 KiB）**。3 MiB 槽仍余 **977,936 B（31%）**。

## 产物与剩余工作

证据目录：`artifacts/v5-static-font-20260906.qYMd1L/`。
包含字库导出/重生成对照、图标审计、44 帧前后对照、计时、测试、目标构建日志及 BIN。

固件 SHA-256：`b3b13aeac0267b52761098364866dc62a24361f7f878c12f88ee0def0cd012c7`。
APP core：`cbf71b687756a8d09a6d3f97b5072304bdfa7d11168459d3bf6e3af761c89171`。

未修改屏幕/TP 初始化、LCD 80 MHz、Lua 公共接口或业务流程；未连接设备、未烧录。
**实机速度和视觉仍待上板验收；通用 Lua 字体接口后续单独设计与实现。**
