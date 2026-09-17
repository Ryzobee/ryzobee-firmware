# 原 V5 Apps：可靠文件时间

2026-09-06。本增量补齐 V5-C05 的创建/修改时间数据链，不改原稿、字体、格子或导航，也不实施工具页。全量固件目标仍未完成，未枚举或操作开发板、串口、复位或烧录；人工验收继续交给用户。

## 实际链路与语义

### 2026-09-08：补齐出厂文件的日期

此前 `spiffs_create_partition_image(scripts fs ...)` 直接打包三个 Lua 文件，没有它们的 `.ryz-t*` 时间记录。真实 Store 对这份输入读取到 `created_unix=0, modified_unix=0`，所以 APP INFO 按约定显示 `-`；不是页面已经收到日期却无法绘制，也不是 Lua 头部注释解析失败。

现在构建先运行 `tools/prepare_factory_scripts.py`，把同样的三个 Lua 文件原样复制到 `build/factory-scripts/`，并为每个文件生成 Store 现有 `RYZTM01` 格式的 213 字节时间记录，再由锁定的 SPIFFS 生成器打包。记录绑定文件名与源码 SHA，含原有校验摘要；不改变运行时存储格式、C/LVGL 页面或联网规则，不新增 Lua 脚本。`fs/` 原文不被构建器修改。

这里 CREATED、MODIFIED 的初值表示**这次出厂镜像中的文件打包时间**，不是无法恢复的历史创作日期。默认使用构建主机 UTC；可设置 `SOURCE_DATE_EPOCH` 固定发布时刻，实现相同输入的可复现打包。每次重新制作出厂镜像会重新记录该镜像的文件创建时间。生成清单位于打包目录外；不进入设备文件系统，未知文件或被手工修改的生成文件会阻止覆盖。

设备上通过 Store 修改文件后，仍保留创建时间并记录有效 UTC 修改时间；未校时的修改保持未知，不伪造日期。未知历史日期不会在联网后自动回填。普通 app OTA 不更新脚本分区；本修复需部署新的 `scripts.bin` 才能改变旧设备的出厂文件日期，部署会覆盖该分区的原脚本。

同时补齐 `scripts/` 中 8 个既有示例的作者、版本、简介注释；保留已有 runtime marker、脚本逻辑和已存在的其他版本信息。三个出厂脚本原本已有这些标签，无需新增或替换工具。

验证：

- 修改前，`test_script_store_time.py` 的出厂用例复现 `tool_monitor.lua CREATED=0 MODIFIED=0`；`test_script_metadata.py` 对 8 个示例报缺作者字段。
- 修改后，元数据解析 6 项、Store 时间 20 项、打包边界 7 项通过。真实 C Store 在无可信时钟时读取生成记录，验证创建/修改时间；再验证有时钟修改与无时钟修改的既有规则。打包器覆盖可复现、源文不变、删除旧工厂文件后无残留、SHA、非法输入与保护未知改动。
- 同源 `v5_apps_ui` CTest 通过，含日期缺值、日历边界、UTC 格式与字格；未改变 UI 代码。Monitor 源文同步检查、冻结检查通过。
- 按 `esp-build` 复用 ESP-IDF v5.5.4 / Python 3.9.6 / GCC 14.2.0，执行 `"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" build` 成功，约 11.97 秒。首次配置被沙箱 sysctl/psutil 阻止后，原环境授权重试通过；未安装依赖、未烧录。
- 最终 `scripts.bin` 确认包含三份完整二进制时间记录，均为 `2026-09-08T10:39:34Z`；大小 1,048,576 字节，SHA-256 `c2a59b320a14ac54c5f323217106b57a1964f05eb7b5becb139135dd73f3ed02`。应用镜像未改变，仍为 1,950,912 字节，SHA-256 `feaf5ba1a244f2295107e1ca3807fa0a15319224ea33d967558ef46f6f8efaf2`。构建日志 `/var/folders/4r/zln0m62n2xg733zqq87x6syw0000gn/T/esp-build.3HhxZee3te`。

以下是既有运行时记录规则，继续保持。

`ryz_time_sample_utc` → Store ESP Adapter → 同锁源文件/时间事务 → Apps reader 的同一份 SHA 快照 → C/LVGL 原 CREATED/MODIFIED 格。串口 `describe/inspect` 同时接入。

- Time 通过单次 try-lock 读取校准 UTC＋单调锚，保留亚秒进位；不等待网络。离线已校准时钟可用，未同步、非法回调、倒退或溢出为未知。`last_sync_unix` 不是当前时间，原始 SPIFFS mtime 不作为可信来源。可信表示本次启动经现有 SNTP 校准，不表示时间经过密码学认证。
- 新文件创建/修改取同一次准备写入的 UTC 样本；覆盖保留可靠创建时间并更新修改时间。未校时覆盖后修改时间必须未知，不沿用旧日期。旧文件创建时间不补造。同内容 no-op 不读时钟、不改文件或 revision；删除后同名同内容重建不继承旧历史。
- `copy_boot` 按目标 `boot.lua` 的生命周期记时，不继承所选源文件创建时间；没有因此开放用户自启动、保存按钮或 D2 救援策略。
- 独立时间值为 0 时未知；有效存储范围为 2024-01-01 至 9999-12-31 UTC。页面按原稿显示 `MM-DD`，缺值显示 `-`；串口返回完整 ISO UTC 字符串或 `null`。未改全局时区设置。
- 时间只进入 snapshot/description，不给列表逐文件读盘；UI Owner 继续只读缓存。

## 提交、恢复和迁移边界

新 `RYZST03` journal 为 673 字节，保存旧/新完整时间记录；每文件隐藏 sidecar 为 `.ryz-t`＋文件名 SHA 前 24 位，共 30 字节，符合当前 SPIFFS 限制。213 字节 `RYZTM01` 记录含完整文件名、源 SHA、两个定长时间和 checksum。完整文件名检查哈希前缀碰撞；摘要用于检测损坏，不是认证。

源码沿既有 rename 提交点生效，时间通过 `.ryz-time-new` 独占写、读回、验证旧值后删除和非覆盖 rename 安装。journal 最后才清理。恢复先判断源码提交/回滚，再安装该 journal 指定的对应时间，绝不重新取当前时间。时间写入失败返回已提交源码＋清理错误／要求恢复，不将“源码成功、时间失败”报成完整成功，也不允许盲目重传。

当前 sidecar 必须逐字节匹配 journal 的旧值或新值；其他记录保留。时间暂存仅在有效 V3 journal 下、内容是其旧/新记录的精确前缀时可重建；无 journal 的孤儿、损坏数据和非普通文件保留并阻断。无 journal 的启动/恢复也扫描规范 sidecar 命名空间，避免损坏记录仍在却误报 clean。恢复未完时不发布时间值。

V1/V2 的原 246 字节 journal 仍可恢复，v1 仍不允许 boot 目标；与新时间 sidecar/stage 混合则拒绝猜测。仅支持所有正常写入经过当前 Store 的目录：**不保证旧固件或外部写入者完成同名同内容删除重建后还能识别旧时间记录**。没有改 SPIFFS 几何、分区、出厂脚本内容或格式化数据。

串口既有 `ok` 表示源码已提交；必须结合 `store_cleanup_error` 与 `store_recovery_required`，不能把它单独当成时间事务或整个磁盘健康的证据。SPIFFS 的实际断电可靠性仍须实板验收，Host `_Exit` 不替代真实掉电。

## 验证与产物

根任务最终合跑 **145/145 unittest 项 PASS，57.073s**：原有 Store 38、时间恢复 19、Apps reader 10、文件 RPC 17、Apps 控制器 16、Scripts 控制器 11、Time/ESP/SystemServices 14、运行链/重启 20。新增时间恢复矩阵包含 69 次隔离 C 进程调用、4 次 `_Exit(86)` 后新进程恢复。测试使用真实生产 C 核心和明确的平台替身，启用严格告警与 ASan/UBSan。

可合并复跑：

```sh
IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf python3 -m unittest \
  firmware/rootmaker/tests/test_script_store.py \
  firmware/rootmaker/tests/test_script_store_time.py \
  firmware/rootmaker/tests/test_workbench_file_rpc.py \
  firmware/rootmaker/tests/test_apps.py \
  firmware/rootmaker/tests/test_workbench_apps.py \
  firmware/rootmaker/tests/test_workbench_scripts.py \
  firmware/rootmaker/components/ryz_time/test_host/test_time.py \
  firmware/rootmaker/components/ryz_time/test_host/test_time_esp.py \
  firmware/rootmaker/tests/test_system_time_integration.py \
  firmware/rootmaker/tests/test_script_run_integration.py \
  firmware/rootmaker/tests/test_workbench_restart.py -q
```

同源 LVGL 9.5 / FreeType 2.14.3~1：完整 native **15/15 PASS，35.46s**。Apps 7 组包含 11 种日期组合、闰日/年界/9999、独立 unknown、异常值、两个进程 TZ 不改变 UTC、撤销旧文本和原 62px/mono7 字格实际字体边界。

本地 Figma Bridge 实时复核 `79:2121` 原详情稿；按 design-to-code 技能请求云端 context 时仍返回 Starter 额度限制，未反复重试或改稿。根任务逐张检查以下真实 C/LVGL **240×240** 导出；图中 `DEMO INPUT` 和 `demo_times.lua` 明确为夹具，不是设备事实，格线与新日期无相碰或裁切：

- 、、、、。

ESP-IDF 5.5.4 / ESP32-S3 本地构建通过；应用 **1,818,576 B**，3 MiB 槽剩余 **1,327,152 B（42%）**。当前构建目录 `/private/tmp/ryzobee-p3-catalog.cjYAiI/target`，仅构建，不执行输出中的 flash 提示。Lua/依赖/config/partition 冻结检查通过。

| 产物 | SHA-256 |
| --- | --- |
| `ryzobee_rootmaker.bin` | `6b3ebdc6cceb4a58647c57247edcc51221ed05d1dd4db9a990a0ae174d2e6f00` |
| `ryzobee_rootmaker.elf` | `9248912aaf1ba89eb728c91350de4a1422f01c81ab81cd373fe143fe17c65848` |
| `scripts.bin`（未变） | `300f85876ce59b5c961516d4af92bd70c68b6fd6e42634eb245ed91f3a7ea12a` |

另外按目标 `compile_commands.json` 的真实 Xtensa/`-Os` 配置，在独立 `/private/tmp/ryz-stack-utc.kKguU8` 为 12 个单元追加 `-fstack-usage` 编译，12/12 成功无告警、183 个函数均为静态帧，源码编译前后 SHA 未变。原对象和构建未覆盖。根任务已读取报告与相关 `.su`：Store 写入→恢复→时间读取为 3,840 B；更长的无 journal 时间索引恢复分支 4,304 B。已核对的 main/RX 选定链最大 5,264 B／32 KiB，Apps reader 4,064 B／8 KiB；UI 网络刷新已测前缀为 7,456 B／16 KiB。

这些数字**不含完整 SDK/libc/mbedTLS/LVGL 调用及 RTOS/中断开销**，余差不能作为实测可用栈；不代替真实 high-water 或栈安全验收。既有 Apps/UI 栈预算值得在最终实板验收重点观察，本次没有据此盲目加大内存。独立静态报告位于上述目录的 `REPORT.md` / `commands.json` / `.su`，源码和分层边界已核对。

只增加 E-P3 / V5-C05、X-05 的时间/身份和 X-13 的取样子条件，不把完整 P3/P4、动态 Unicode、用户启动/退出、BLE、文件服务或完整 OTA 记为完成。
