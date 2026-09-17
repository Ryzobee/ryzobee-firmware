# P3 应用数据：版本绑定运行与后台读取

最新范围（2026-09-06）：原 V5 非工具页面由 LVGL+C 实现，V5-T01–T08 暂缓、原设计保留，Version/OTA 六状态仍在本期。第四段见Apps 页面业务接线（开发资料未随公开源码分发）：Controller 已接真实 Owner/RX，直接用于原 V5 Apps；V5.1 不再作为替代基线或审批前置。三份默认 Lua 工具仅打印未实现提示、允许修改/覆盖/删除，不调用 UI/硬件、不自动补回。

本文第 1–5 节保留 2026-09-05 第二段版本绑定运行，第 7 节保留第三段后台读取，均接续 [Script Store](./firmware-script-store.md)，不是完整 Apps 页面或用户自启动。历史阶段用户追加全量烧录及清空配网，证据见第 8 节；这不授权当前阶段烧录。

## 1. 运行的实际闭环

旧 Studio 交付入口先按文件名运行，收到任务后再比较 SHA；若文件在读取与启动之间变化，发现不符时另一份代码可能已经执行。本段将身份校验前移到设备：

1. 客户端提交当前 boot ID、文件名与所选源码 SHA，不隐式停止或覆盖文件。
2. 设备检查 schema/boot、运行占用及存储恢复状态，取得单个不可变 Store 快照并比较 SHA；变化、丢失、损坏或恢复不明均拒绝启动。
3. 运行回调复制**这份快照的字节**，不按路径二次读取。共享 job-start 核心在最终锁内重检当前任务、网关、任务 ID，并完整分配确认数据，才发布到任务队列。
4. Lua Worker 执行任务自己的源码。此后磁盘被改变不会偷换已经接受的源码；任务 SHA 对应该份源码，完成/停止沿用现有 Owner 清理及事件路径。

文件读取与比较是所选快照的接受点，不承诺运行时文件仍是磁盘“最新版本”。正常文件写入必须经过同一个非 UI 命令所有者及运行许可；Store 自身不是所有后续 HTTP/屏幕动作的运行仲裁器。

底层文本 Console 的 `lua --run-async --path` 与旧 `run/eval` 仍保留原协议；它们是显式的兼容命令，不构成新界面已绑定源码的证据。新版受控启动不会在旧固件不支持时自动降级到这些命令。

## 2. 新接口与源码所有权

新增版本化文件 RPC 动作：

```json
{
  "id": "request-id",
  "op": "scripts",
  "schema": "ryz-script-store/1",
  "boot_id": "当前 info 返回的启动标识",
  "action": "run",
  "name": "demo.lua",
  "sha256": "所选源码的64位小写SHA-256"
}
```

`run` 是异步任务，沿用当前 `run-async` 的无总运行时长限制；查询/停止仍走现有任务接口。新请求不设置用户启动脚本。当前系统全局屏幕退出还受 D2 约束，不能把串口停止当作屏幕退出验收。

- 成功响应包含 schema、boot ID、完整 job 身份/状态及 `output`；明确拒绝不携带 job，也不发布任务。
- 回复丢失、启动后响应分配失败或身份无法核对仍是结果未知，不自动重试。入队前 ACK 分配失败不会发布任务；入队后不为“补错误响应”而销毁运行源码。
- Store 快照由 RPC 释放；job 持有独立 malloc 源码并由现有 UI Owner 完成路径释放。共享核心拒绝时消费并释放传入源码，调用方不得重复释放。
- 任务计数器耗尽或 ID 无法完整容纳时拒绝，不复用同 boot ID 下的任务标识。

生产落点：[RPC](../../firmware/rootmaker/components/ryz_workbench/workbench_file_rpc.c)、[共享启动核心](../../firmware/rootmaker/components/ryz_workbench/workbench_job_start.c)、[Workbench 的 FreeRTOS 适配](../../firmware/rootmaker/components/ryz_workbench/workbench.c)。Host 与目标调用同一启动核心，不在测试里复制一份准入/分配/发布顺序。

## 3. 详情与元数据

`scripts.inspect` 使用 name 获取受限源码快照，返回 name/bytes/SHA/protected、未知的 created_at/modified_at（`null`），以及 metadata.author/version/description。每个字段包含 `value/present/truncated/invalid`，未提供字段的 value 为 null。元数据和 SHA 来自同一快照；不混用另一时刻的目录信息。空/NUL/超限文件仍可通过首段 raw `describe` 识别、删除或修复，不能由 inspect 冒充可执行源码。

可选头部格式：

```lua
-- ryz-app/1
-- @author: RyzoBee
-- @version: 1.2.0
-- @description: A small device application.

print('Ready')
```

保持原 runtime marker 在开头，不为了添加元数据而移动它。标签是普通注释，旧脚本无需修改；不解释 Lua 字符串、插值、block comment 或可执行表达式，也不把版本字符串当固件升级版本规则。

[纯 C 解析器](../../firmware/rootmaker/components/ryz_script_metadata/README.md)无 I/O、堆分配或 Lua 执行。作者/版本/描述分别最多 80/32/256 个 UTF-8 字节，按完整码点截断，记录截断/异常状态；只读连续文件头行注释，首个代码行或长注释开头停止。首个识别值优先，重复标签标异常但不覆盖；完整验证首值，包括截断范围之外的字节。非法 UTF-8/控制格式字符不会进入展示字段。原始源码不被修改，描述文字不构成作者真实性、权限或脚本运行安全证明。

屏幕仍使用 ASCII 子集，能解析并保留 UTF-8 不等于屏幕可显示所有字形。D3 的显示回退/分页和字体策略、创建/修改时间的持久化仍待落实。

## 4. 本段验证记录

本轮最终结果：

| 检查 | 结果及实际覆盖 |
| --- | --- |
| 主 Host 回归 | 100 项通过，包含 Store 27、metadata 5、真实 RPC 8，以及既有 runtime Owner/导航/输入/显示等；不能把子组再重复加总 |
| Store→RPC→job-start→真实 Lua | 独立 6 组通过，执行实际生产核心和 Lua/Owner 通道，BSP 是明确的 Host 适配而非像素/实板 |
| 补充既有检查 | 字体资产 1 项、模拟串口控制线顺序 1 项，app-runtime、UI-host、冻结依赖/配置/分区检查通过 |
| Web | 43 文件、454 项通过；指定交付集成另 6 项通过；Web 生产/测试及根 tests 类型检查通过 |
| 目标构建 | 隔离 ESP32-S3 完整及最终增量构建通过；map 保留共享启动核心、元数据解析器、RPC 和 Store |

新增执行链测试验证：选中 A 接受后磁盘换为 B，任务仍实际执行 A；选择后但请求前已换 B 则拒绝；schema/boot/hash/缺执行器/busy/recovery 均不排队；在快照回调暂停时另一个请求取得 Owner 或网关关闭，最终准入仍拒绝；队列失败、legacy 无即时 ACK、任务 ID 耗尽有一致结果。两组 cJSON 分配预算扫描覆盖发布前完整 ACK 与发布后 RPC 版本字段失败；后者仅无确认，已接受源码仍可执行。每轮 JSON 计数与真实 Lua 内存归零。

元数据裸 CR 末尾处理曾被独立测试复现，修正后转绿。Studio 的旧无 SHA 入口、损坏 ACK、版本化响应缺 boot、新 unknown 键不能通过核对等也有实际 RED→GREEN；迁移后的 fake Peer/DOM 测试并不代表真实 Web Serial/设备验证。测试 fixtures 只适配新的实际入口，未改变交付策略去放宽断言。

主 Host 命令在仓库根执行（100 项）：

```sh
IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf \
PYTHONPATH=firmware/rootmaker/tests PYTHONDONTWRITEBYTECODE=1 \
python3 -m unittest -v test_app_host test_display_driver test_runtime_owner \
  test_script_store test_script_metadata test_system_services test_system_ui \
  test_ui_navigation test_workbench_file_rpc test_workbench_input \
  test_workbench_io test_workbench_lifecycle

IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf \
PYTHONDONTWRITEBYTECODE=1 python3 firmware/rootmaker/tests/test_script_run_integration.py -v
```

执行链 runner 复用已有 Host runtime fixture，仍链接真实 `lua_runtime/app_runtime/nervous_runtime/ryz_runtime_io/onelua`，不在新测试中重写 Lua 行为。严格 C 警告、ASan/UBSan 启用；仅锁定第三方 cJSON 对象关闭 macOS `sprintf` 弃用警告。

前端命令：仓库根 `npm run test --workspace @ryzobee/studio-web`、`npm run typecheck --workspace @ryzobee/studio-web`、`npx tsc --noEmit -p tests/tsconfig.json`；指定交付集成通过根测试配置运行，路径 `tests/integration/studio-app-delivery.integration.test.tsx`。回归发现的旧 Console 运行 fixtures 已迁到 `runFile`，没有为通过测试添加生产回退。

构建使用 ESP-IDF 5.5.4 / ESP32-S3 / 应用 0.9.0，目录 `/private/tmp/ryzobee-p3-apps.NmTKIe/target`，SDKCONFIG 为同级 `sdkconfig`，defaults 指向仓库 `firmware/rootmaker/sdkconfig.defaults`。复现命令沿首段构建命令，仅将其两处临时路径换为本目录。最终 `ryzobee_rootmaker.bin` **2,088,048 字节**，3 MiB 槽余 **1,057,680 字节（约 34%）**；SHA-256 `d63aaadaf2aa1daa3d56b0a29e40e9c607fce41095578693fa1d791ad7647556`。应用/bootloader rollback 均启用，OTA URL 仍空，LVGL 池 64 KiB，未改 BOE/TP 初始化或 LCD80MHz。临时目录不是发行归档；本轮未烧录、未做截图/实屏/扫码/无线/断电验收。

## 5. 当前客户端行为

DeviceClient.runFile（开发资料未随公开源码分发）明确绑定 name/SHA/boot，在排队发送前复核 boot，验证 ACK 后才发布该 job，不从运行 ACK 导入附带的 board info/jobs。明确拒绝用 `RunFileRejectedError` 表示未启动；丢回复、坏身份或重启为 `UnknownResultError`，均不自动重试。

DeviceLuaPort（开发资料未随公开源码分发）与现有交付入口已使用新方法，并保留启动前后 boot 和任务身份复核；高级 Lua 页的运行按钮及文件运行文本同样迁移，查询/停止等保持旧通道。Studio 核对流程同时识别新 `scripts.run <name>` 与历史旧命令的 unknown 键。

**兼容影响：新版网页执行文件需要支持 `scripts.run` 的固件；旧固件明确拒绝并提示更新，不会偷偷运行无 SHA 的替代路径。** 本轮没有给设备升级，当前接入旧固件时出现该提示是预期行为。元数据的 `scripts.inspect` 已有 C/RPC 数据基础，但尚未增加网页详情展示或板端 LVGL 详情页。

## 6. 剩余范围

第三段已补后台目录/详情，第四段已补 Controller 动作与版本绑定启动。完整 P3 仍需在原 V5 C/LVGL 页面呈现数据，接入滚动/短按/1.2 秒长按、删除确认，完成用户 boot 与系统诊断分离、全删及启动救援。无需等待 V5.1 确认或为其扩展 Lua 字号；D2/D3 的具体退出/文本问题按原 V5 单项解决，实际屏幕验收交给用户。这些片段不通过完整 X-04/X-05、任何完整 V5 页面或整个 P3。

三份 `fs/tool_*.lua` 是普通可编辑/删除的打印占位；单项/全部删除应按普通文件规则处理，普通重启或 app OTA 不恢复删除。当前 legacy `boot.lua` 的保护/迁移仍是独立未完成事项，不把占位文件当作系统保护文件；打印成功不等于工具功能完成。

## 7. 第三段：后台目录/详情与路由生命周期

新增 [ryz_apps](../../firmware/rootmaker/components/ryz_apps/README.md)，将目录扫描、源码读取及元数据解析放在独立 reader。UI 侧提交、读取快照、离页撤销均只做有界内存操作。目录分页保留 Store revision；数据改变自动刷新当前页，已选详情则失效，不能悄悄把同名新文件重新选中。Store 同 revision 的 recovery/unready 同样清除缓存；读取失败、存储忙和空目录各有独立结果，FAILED/STALE 不自动反复扫描。

请求为 latest-wins，非回绕 64 位 ID 与完成发布复检撤销迟到结果；页面控制者还要核对已有导航 token。有效详情的 SHA/metadata 来自同份已释放的源码快照；不可执行文件用双 raw 身份与同错复读确认，保留可供后续 CAS 修复的身份，不伪造元数据/时间。`source_valid` 只指大小/NUL 检查，不是执行安全认证。

实际 Workbench 已薄接线：进入现有 Apps 路由的新页面代次请求第一页，离页/模态/启动 Lua 时关闭旧请求；reader 创建失败独立每 5 秒重试，不关停 runtime gateway。新增 `info.apps` 是缓存诊断，既不扫盘也不控制页面，ID 用十进制字符串。**尚未向旧静态屏幕绘制目录/错误/详情，屏幕详情入口也未调用新 API。** 不以该接线通过 V5-C03–C06。

### 本次实际验证

| 检查 | 本次结果及边界 |
| --- | --- |
| 新 Apps Host | 10 项通过；真实 core/reader/Store/metadata，POSIX 文件、CommonCrypto 和受控 pthread；C11 严格告警、ASan/UBSan |
| 既有 Host 与执行链 | 106 项通过：原主 Host 100 + Store→RPC→job-start→真实 Lua 6，未重复加总；与新增 10 项合计 116 |
| 依赖冻结 | 锁文件、manifest、defaults、分区及 Lua 组件源码校验通过；首次在错误工作目录运行退出，改为仓库根后通过，未修改冻结值 |
| ESP32-S3 | 独立临时目录完整及最终增量构建通过；Apps core、reader、FreeRTOS 适配和 Workbench 接线编译/链接；实际栈余量和时序尚未实测 |

新增用例覆盖启动失败重试/并发、参数及输出清零、排序/分页/过滤、目录 revision 刷新、失效详情、真实读取暂停时快照/新请求/close 零文件 I/O、旧完成撤销、无热重试、同 revision 恢复失效与 Store 锁竞争。独立静态审查发现两处分页问题并在本次修正、补回归：17 个文件的第二页删除唯一项后得到 `offset==total` 空尾页，应自动回第一页；回退后再次增加文件不能跳回旧 offset16。测试最终两种情况均通过，不宣称该两例曾运行修改前的 RED。

在仓库根复现本次 Host 检查：

```sh
PYTHONDONTWRITEBYTECODE=1 python3 firmware/rootmaker/tests/test_apps.py -v

IDF_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/esp-idf \
PYTHONPATH=firmware/rootmaker/tests PYTHONDONTWRITEBYTECODE=1 \
python3 -m unittest -v test_app_host test_display_driver test_runtime_owner \
  test_script_store test_script_metadata test_system_services test_system_ui \
  test_ui_navigation test_workbench_file_rpc test_workbench_input \
  test_workbench_io test_workbench_lifecycle test_script_run_integration

sh firmware/rootmaker/tools/check_lua_freeze.sh
```

Workbench 的 FreeRTOS 路由生命周期和 `info.apps` JSON 分配路径本次为静态审查/目标编译证据，不在上述 pthread 核心用例中冒充动态覆盖。未修改 TypeScript，本次未重跑 Web/类型检查；第 4 节 Web 结果仍只属于第二段。

构建在 `firmware/rootmaker` 执行：

```sh
export IDF_TOOLS_PATH=/path/to/espvm/esp-idf/versions/v5.5.4/tools
source /path/to/espvm/esp-idf/versions/v5.5.4/esp-idf/export.sh
idf.py -B /private/tmp/ryzobee-p3-catalog.cjYAiI/target \
  -D SDKCONFIG=/private/tmp/ryzobee-p3-catalog.cjYAiI/sdkconfig \
  -D SDKCONFIG_DEFAULTS=/path/to/ryzobee-firmware/firmware/rootmaker/sdkconfig.defaults build
```

ESP-IDF 5.5.4 / ESP32-S3 / 应用 0.9.0；最终 bin 为 **2,091,152 字节**，3 MiB 槽余 **1,054,576 字节（约 34%）**，SHA-256 为 `57f872834a14f12523546c3afc845a8d13a2e1fa82809a1111fc4df344bb5d3d`。五份烧录镜像及 sdkconfig 已归档至本地 Git 忽略目录 `artifacts/firmware-p3-20260905.RmQDFs/`，不只依赖临时构建目录。

BOE/TP 初始化与 LCD SPI80MHz 未动；Flash 仍为原 40 MHz 配置，与 LCD 总线无关。应用/bootloader rollback 均启用，OTA URL 仍空，LVGL 池 64 KiB；reader 新增一个 8 KiB 任务栈及缓存/同步资源，资源峰值仍未验收。详情请求入口尚无产品调用，会被最终 ELF 裁掉；Host 已覆盖其公共行为，不伪装屏幕详情已连通。未做截图/真屏/扫码/无线/掉电验收。

## 8. 用户追加的全量烧录与配网重置

用户先明确“只需要烧录好，验收我来”，随后要求“全量烧录直接覆盖”，并明确不保留配网、要测试配网功能。后两个要求覆盖本次早先保留脚本/NVS 的准备方案；不据此扩大为整片擦除或 eFuse 操作。

烧录前实时识别 `/dev/cu.usbmodemPORT`、USB `1A86:55D3`，只读 RPC 确认 ESP32-S3 / 16 MiB Flash / 2 MiB PSRAM / 0.7.0。原分区表与本次构建逐字节一致；OTA 记录0为 seq1/state2/CRC有效，当前槽 ota_0；ota_1 起始为空。芯片只读安全信息显示 Secure Boot 和 Flash Encryption 均未启用，未执行 eFuse 写入。

先保存完整 **16,777,216 字节**备份，再用设备端摘要比对整片成功：`artifacts/private-flash-20260905.yC8YXk/full-before.bin`，SHA-256 `5991a36a19091c6a4ca41d1ecf0ccfd1575ca17b213bcdc11680fc916758024d`。目录权限 0700、备份0600、Git忽略；可能含凭据，不上传或提交。脚本及配置片段也留在同一私有目录，供需要时受控恢复。

按本次生成的 `target/flash_args` 全量写入，以下五段均返回 `Hash of data verified`：

| 区域 | 偏移 | 镜像字节数 |
| --- | --- | ---: |
| bootloader | `0x0` | 20,912 |
| 分区表 | `0x8000` | 3,072 |
| OTA 初始数据 | `0xD000` | 8,192 |
| 应用 ota_0 | `0x10000` | 2,091,152 |
| scripts | `0x610000` | 1,048,576 |

脚本分区已按用户要求覆盖为仓库 `fs/boot.lua` 对应镜像，原用户脚本只存在于上述备份，不声称被保留在设备。随后按用户不保留配网要求，定向清除已核实的 NVS `0x9000` / `0x4000`，工具返回 `Erase completed successfully`；这也重置原 AP 访问口令和可重建的网络/射频缓存，不是整片擦除。

第一次烧录后只读握手曾超时，不能据此宣称启动成功；受控 RTS 复位捕获后看到 0.9.0、`RYZOBEE_READY`、Portal 就绪和健康门槛通过，没有因此修改固件。原超时的确切主机控制线原因未做电气验证。清空 NVS 后再次受控复位，最低启动结果为：

- 从 `0x10000` 加载，应用 0.9.0，ELF SHA 前缀 `3e90d3256`。
- `RYZOBEE_READY`，`provisioning portal ready on SSID RYZOBEE-C5F8`，`running image health gate: passed`。
- 最终重新打开串口的只读 `info` 成功：filesystem/display/font/touch、runtime gateway 与 system services 均为 ready。`apps.reader_running=false` 是进入 Apps 前的惰性启动状态，不是已运行目录交互的证据。

只确认写入与基本启动，没有运行串口 smoke 全套、用户 Lua、触摸/目录、扫码、STA 联网、BLE、OTA 或掉电验收。设备保留在已启动 AP、未保存目标 Wi-Fi 凭据的状态交给用户；旧 AP 密码不可继续依赖，应使用设备当前显示的配网信息。用户实机验收与完整 V5+ 功能实现仍分开记录。
