# P3 首段：Script Store 与真实文件 RPC

日期：2026-09-05。状态：存储底座和既有 Workbench/Studio 接线已实现并完成下述 Host/构建检查；**完整 P3、V5 应用页面和实板恢复尚未完成**。本轮未修改 Figma、未枚举串口、未操作或烧录设备。阶段定义见执行计划（开发资料未随公开源码分发），验收登记见 E-P3（开发资料未随公开源码分发）。

后续覆盖：下文 1–5 节保留首段执行时的范围与证据。第二段已实现可选元数据、版本绑定运行及实际 Studio/高级 Lua 页迁移，最新行为和构建证据见[应用元数据与版本绑定运行](./firmware-script-apps.md)；首段的“元数据尚未实现/Studio 尚未采用新 API”不作为第二段当前结论。

2026-09-06 覆盖：`boot.lua` 已是普通用户文件，系统诊断移至可选固件只读资源。Store 新增同锁 SHA/revision 绑定的 `copy_boot` 和固定目录快照的 `delete_all`，新事务使用 `RYZST02`；旧 v1 恢复兼容非 boot 目标，仍拒绝旧 v1 boot journal。批删不是整体原子操作，首个错误即停，只统计确定删除项，重启不续删；`complete=true` 也必须联合错误、清理及恢复字段判断。启动/重启策略没有因存储接口存在而启用。具体当前契约见[组件 README](../../firmware/rootmaker/components/ryz_script_store/README.md)。

同日后续：串口 PUT/REMOVE、Apps 单删与 Scripts 全删现在共享实际 Job 所有者的写入门禁；不再依赖 RX 串行与较早的 `busy()` 快照。Store 的锁、文件事务及恢复语义不变，具体接线与分层检查见共享文件写入准入（开发资料未随公开源码分发）。这只是独立文件服务的前置，不是 HTTP 授权或服务已实现。

同日最新覆盖：[可靠文件时间](./firmware-v5-file-times.md)已接 Time→Store→Apps/RPC→原 V5。新事务为 673 字节 `RYZST03`，另预留 213 字节时间暂存；旧 v1/v2 按兼容规则恢复。下述首段“时间未知”和 246 字节新事务容量仅为历史记录，不再代表当前实现。没有补造既有文件日期、启用用户自启动或操作开发板。

## 1. 已落地的范围

| 落点 | 当前行为 |
| --- | --- |
| [Store 公共 Interface](../../firmware/rootmaker/components/ryz_script_store/include/ryz_script_store.h) | 同一个 `.lua` 验证、排序分页、不可变源码快照、原始文件 SHA、CAS 修改、事务及恢复入口；不负责挂载、格式化或执行 Lua |
| [Store 核心与 ESP Adapter](../../firmware/rootmaker/components/ryz_script_store/README.md) | 真实 SPIFFS 路径、容量、非覆盖 rename、流式 SHA；Host 替换平台 Adapter，测试仍执行同一生产核心 |
| [文件 RPC Adapter](../../firmware/rootmaker/components/ryz_workbench/workbench_file_rpc.c) | 旧 `list/get/put/remove` 复用 Store；新增版本化 `scripts` 操作；完整旧列表不被 16 项分页截断 |
| [Workbench](../../firmware/rootmaker/components/ryz_workbench/workbench.c) | 启动前恢复；boot/run 通过 Store 读取，恢复状态不明时不执行文件；运行占用时保持旧文件命令拒绝策略 |
| Studio DeviceClient（开发资料未随公开源码分发） | 识别“已提交但清理未完”和“结果未知”，不把二者都当成可盲目重试的普通失败 |
| UI 容量 | 首页及 `info` 只读 Store 发布的内存快照；锁忙立即保留旧样本，不在 UI Owner 调用 SPIFFS 或等待垃圾回收 |

所有正常脚本写入必须经过 Store；目录不能同时存在绕开它的写入者。Store 的锁是非阻塞排他，不是异步文件执行器：调用者仍需在非 UI 上下文执行文件操作，应用运行与文件修改之间的许可由上层所有者统一管理。

## 2. 数据、容量与提交语义

- 新源码/运行快照：1–8192 字节，无内嵌 NUL；`get` 返回所读取同一份内容及其 SHA，使用 `snapshot_free` 释放。
- `describe` 仅返回原始文件大小/SHA/保护标志，允许空、含 NUL 或超限的普通文件。这些旧文件可按 SHA 删除或修复，但不能因此被当作可运行源码。ESP 使用 512 字节分块，不分配整份大文件。
- 索引：ASCII `strcmp` 顺序，最多 256 项，每页最多 16；超限明确报错。后续页绑定 revision，发生 Store 修改则旧 revision 失效；重启时由 boot ID 区分。
- 文件名协议上限 40 字节；当前 SPIFFS `OBJ_NAME_LEN=32` 包括前导 `/` 和终止符，实际 basename 上限 **30 字节**。超限拒绝，不截短、不改磁盘布局。
- `put` 需要新源码 + 246 字节 journal + 4096 字节余量；旧目标保留到提交/清理阶段。删除也需要 journal/余量，因此几乎满盘时仍可能拒绝；不是无空间条件下的紧急清理工具。
- 容量快照有 `capacity_valid/capacity_error`，在初始化、修改和恢复路径刷新。无效不能显示为空盘；永久 I/O 降级使容量失效并要求恢复。
- 作者、版本、头部描述、可靠创建/修改时间尚未实现。RPC 时间返回 `null`，后续页面显示 `—`，不伪造 NTP 时间或使用主机时间。

新事务私有名称为 `.ryz-txn/.ryz-new/.ryz-old`。journal 包含目标身份、旧/新 SHA 和记录摘要，先写入/关闭/读回确认，再准备文件、调整名称；目标提交后先清理备份/暂存，最后清理 journal。摘要检测损坏，不是认证签名。

| 结果 | 调用方处理 |
| --- | --- |
| `not_committed` | 本次操作没有建立目标修改；根据错误处理，不能未经条件复核自动覆盖 |
| `committed` | 文件修改已发生；即使 `cleanup_error` 非零也不能当成未提交重试。恢复未完时继续拒绝新修改 |
| `unknown` | 不能确认提交或回滚；先读取状态/恢复/核对，不自动重发 |

I/O 错误可能发生在 rename/remove 已生效之后，Store 会按 journal 与真实文件身份判断；重复恢复保留已验证内容。损坏 journal、身份不符、孤立暂存物或未知旧 `.upload.bak/.upload.tmp` 一律保留，不猜原目标、也不自动删除。没有新增远程“强制清理”命令；无法自动判断的情况需要受控维护。

这不是 SPIFFS 任意掉电原子性保证。Host 新进程中断模拟不覆盖 Flash 控制器、真实 SPIFFS 缓存、GC、介质损坏；`fsync` 只刷新对应描述符。ESP close 失败后不重试可能已复用的描述符，进入 sticky I/O 降级至重启。

## 3. RPC 兼容与新接口

旧命令正常成功字段维持原形状：`list` 为 `id/ok/files`，每项 `name/bytes/protected`；`get` 为 `id/ok/name/bytes/sha256/source`；`put` 为 `id/ok/bytes/sha256`；`remove` 为 `id/ok`。旧上传的可选 `sha256` 仍校验**新内容**，不是旧内容 CAS。异常提交结果才增加 `store_commit/store_recovery_required/store_cleanup_error`。

新语义不塞进旧命令的可忽略字段，而采用独立 operation，使旧固件拒绝而非悄悄忽略条件：

```json
{
  "id": "request-id",
  "op": "scripts",
  "schema": "ryz-script-store/1",
  "boot_id": "从当前 info 获取的启动标识",
  "action": "catalog",
  "offset": 0,
  "limit": 16
}
```

- `status`：ready、恢复/旧残留状态、revision、容量有效性及错误；容量无效时大小为 `null`。
- `catalog`：files/revision/offset/total/count；offset 非零必须带首个响应的 revision。
- `describe`：指定 name，返回原始大小/SHA/protected 与未知时间，不返回 source。
- `put`：name/source/新内容 sha256，加必填 `previous_sha256`。空串要求不存在；64 位小写十六进制要求旧内容精确匹配。
- `remove`：name 和必填 `previous_sha256`；同样实施旧内容比较。

所有新命令要求 schema、当前非空 boot ID 匹配。传输入口保持 JSON NUL 拒绝；运行占用的文件访问策略没有放宽。CAS、revision 是并发一致性条件，不是身份认证。

回复分配失败/确认帧丢失可能发生在提交之后，因此不补造“确定未提交”的错误。现有客户端仍遵守结果未知后不自动重发；`committed + recovery_required` 则返回已提交并记录维护提示。**Studio 本轮尚未切换到新 catalog/describe/CAS 调用**，只有旧命令结果语义接线；未来 HTTP/屏幕/Studio 新入口必须使用同一 Store 并接入运行许可。

## 4. 本轮执行证据

新增 Store Host 27 项：真实临时目录、真实 SHA、ASan/UBSan、竞争、容量、路径/非普通文件、校验、分页/版本、部分写入/close/rename/remove 故障，以及跨新进程恢复。包含 131089 字节旧文件的流式身份、CAS 修改、完整备份回滚和中断删除；status 在正常、故障、阻塞及竞争时零平台调用。

新增真实 RPC Host 7 组：旧线协议形状、新 schema/boot/CAS、完整列表/分页、保护和占用、清理失败后已提交、cJSON 分配失败、不可执行旧文件的 describe→CAS 删除。最后一组先复现旧 get 路径失败，再改为 raw describe，保留断言转绿。

在仓库根目录执行以下命令，94 项通过（含上述 27+7 项，不能重复加总）：

```sh
IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf \
PYTHONPATH=firmware/rootmaker/tests PYTHONDONTWRITEBYTECODE=1 \
python3 -m unittest -v test_app_host test_display_driver test_runtime_owner \
  test_script_store test_system_services test_system_ui test_ui_navigation \
  test_workbench_file_rpc test_workbench_input test_workbench_io test_workbench_lifecycle
```

另外通过：已有字体资产 1 项、模拟串口控制线顺序 1 项、`sh firmware/rootmaker/tools/test_app_runtime.sh`、`sh firmware/rootmaker/tools/test_ui_host.sh`、`sh firmware/rootmaker/tools/check_lua_freeze.sh`。在 `apps/studio-web` 执行 `npm run typecheck` 和 `npx vitest run src/device.test.ts src/studio/delivery/deviceLuaPort.test.ts`，最终类型检查及 39 项测试通过。

验证过程中的环境/代码问题已区分：默认 Python 全发现首次报缺 `fontTools` 与 `serial`，不是功能断言失败；分别使用现有 `/private/tmp/ryzobee-font-venv/bin/python` 与 IDF Python 环境运行对应单项通过。锁定 cJSON 在 macOS 的 `sprintf` 废弃警告只对第三方对象关闭，项目代码仍 `-Werror`。新增 Studio 测试的闭包类型推导错误已通过显式 `Peer` 类型修正，再运行原类型检查通过。未改 SDK 或下载测试依赖。

ESP32-S3 完整构建及最后增量构建通过。初次配置的 psutil 进程查询被沙箱拒绝，经批准仅重跑本地构建，未绕改组件管理器。构建命令在 `firmware/rootmaker` 执行：

```sh
export IDF_TOOLS_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/tools
source /path/to/espvm/esp-idf/versions/v5.5.4/esp-idf/export.sh
idf.py -B /private/tmp/ryzobee-p3-store.8zCOJJ/target \
  -D SDKCONFIG=/private/tmp/ryzobee-p3-store.8zCOJJ/sdkconfig \
  -D SDKCONFIG_DEFAULTS=/path/to/ryzobee-firmware/firmware/rootmaker/sdkconfig.defaults build
```

最终应用：`/private/tmp/ryzobee-p3-store.8zCOJJ/target/ryzobee_rootmaker.bin`，**2,085,088 字节**，3 MiB 槽余 **1,060,640 字节（约 34%）**；SHA-256 `3431ea372c4176a7cca36d7479c2c2198a27c452da67cc083a5ffe51d85ff1e3`。map 中真实保留 Store 核心、ESP 流式 SHA 与 RPC 入口，不只是静态库编译。应用与 bootloader 生成配置均启用 rollback，OTA URL 仍空，LVGL 池仍 64 KiB；冻结依赖、配置和分区检查通过。临时目录可被清理，不作为正式发行归档。

## 5. 续接与验收边界

本段只支持 X-04 的单 Store/串口事务子条件、X-05 的文件身份/缺时间子条件及数据保护的部分底座；**不通过完整 X-04/X-05/X-17、任何完整 V5 页面或 P3**。

下一段优先连接应用目录模型、版本绑定选择/执行与元数据，再接经确认的 LVGL 列表/详情/删除流程；新 HTTP 文件入口随后复用 Store，不另写文件事务。源变化/运行许可必须在命令接受时复核，不能只信列表曾显示的 SHA。

以下仍未实现或未验收：应用头元数据及持久时间策略、索引变化通知与页面刷新、短按/1.2 秒长按运行、全删、选择内容复制为用户 boot、系统诊断与用户 boot 分离、启动救援、文件服务、真实屏幕和受控掉电测试。现有 `boot.lua` 的保护和 1 秒旧检查只是暂时保持兼容，绝不是用户自启动功能交付。D2/D3、Figma 确认及实际设备条件按主计划单独推进。
