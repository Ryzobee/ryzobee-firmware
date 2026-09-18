# 贡献与提交规范 / Contributing

本仓库维护 RootMaker 固件与同源模拟器，不包含 Studio 应用。先读 [中文 README](README.zh-CN.md) 或 [English README](README.md)，使用锁定的 ESP-IDF 5.5.4 和组件版本。

## 1. 分支与合并 / Branches and merging

- 从最新 `main` 创建主题分支，例如 `fix/apps-back-touch`、`feat/lua-example`；每个 PR 聚焦一组相关改动。
- **禁止直接更新 main，包括管理员；所有更新通过 PR。** 不强推、不删除主分支、不绕过保护。
- 合并前必须通过 **Firmware version** 与 **ESP32-S3 build** 两项强制 CI 门禁；没有额外强制审批人数。
- 可以先创建 PR，再由 CI 检查；“编译通过”是**合并前提**，不是创建 PR 的前提。新提交要等待对应的新结果，不能用旧提交的成功状态替代。
- CI 编译后核对实际固件版本，并执行同源 VERSION 页面检查；其它 Host、模拟器或实机检查按改动风险执行并记录，不能被编译成功替代。

**English:** update `main` only through PRs, including administrator changes. Both required checks, `Firmware version` and `ESP32-S3 build`, must pass before merging. The build check also verifies the binary version and native VERSION pages. Opening a PR does not need an earlier successful run. No additional approval-count requirement is imposed. Report hardware and simulator checks separately from compilation.

## 2. Commit 和 PR 标题 / Message format

统一格式：

```text
emoji prefix(scope): summary
```

emoji 后一个空格，前缀小写，范围放英文括号中且**必填**，英文冒号后一个空格，简介说明实际改动。不要写 `update`、`fix bug` 或“修改一些内容”。标题末尾不加句号。

只使用以下八种固定配对，不交换 emoji 或自行增加同义前缀：

| 固定配对 | 用途 / Use |
| --- | --- |
| `✨ feat` | 新功能 / New behavior |
| `🐛 fix` | 缺陷修复 / Bug fix |
| `📝 docs` | 文档与说明 / Documentation |
| `⚡ perf` | 性能、资源优化 / Performance |
| `♻️ refactor` | 不改变外部行为的重构 / Behavior-preserving refactor |
| `✅ test` | 测试与夹具 / Tests and fixtures |
| `👷 ci` | CI 工作流、构建门禁 / CI workflows |
| `🔧 chore` | 维护、依赖、发布准备 / Maintenance |

范围用简短小写名称，可带连字符，例如 `ui`、`lua`、`ble`、`wifi`、`display`、`storage`、`board`、`simulator`、`build`、`docs`、`repo`。跨相关模块选能概括它的范围；无关改动拆分提交。

```text
✨ feat(lua): 增加协作式传感器显示示例
🐛 fix(ui): 修复应用详情返回按钮的触摸判定
📝 docs(guide): 补充系统相机扫码配网步骤
⚡ perf(display): 缩小长按反馈刷新区域
♻️ refactor(storage): 合并目录快照的内部读取路径
✅ test(ble): 覆盖取消配对后的迟到回调
👷 ci(build): 增加 ESP32-S3 干净构建检查
🔧 chore(repo): 初始化 V0.10.1 固件仓库
```

**English:** use exactly one fixed emoji/type pair, a non-empty lowercase scope, and a concise actual-change summary. PR titles follow the same format. Chinese summaries are preferred; clear English is acceptable when needed.

## 3. PR 简介 / Pull-request description

使用 [PR 模板](.github/pull_request_template.md)，**尽量用中文**，接口名、日志和命令保留原文。每节均填写；无内容写“无”：

1. **目的与背景：**为什么需要改动，关联什么问题。
2. **新增了什么：**新功能、文件、接口。
3. **删除了什么：**删除的功能、代码、接口、资产及替代路径。
4. **修改了什么：**既有行为、配置、交互或资源变化。
5. **注意事项：**兼容性、数据覆盖、分区/NVS、无线安全、内存、接线和升级风险。
6. **版本更新：**原版本 → 新版本、同步位置和验证结果，按下方[固件版本更新](#固件版本更新)执行。
7. **验证：**命令、结果、CI 链接，以及实际执行的测试、模拟器或实机检查；未执行说明原因。

UI 改动可附同源模拟器截图，注明原始分辨率、场景和模拟数据。区分“编译成功”“写入校验通过”“正常启动”和“功能验收通过”。

若使用 squash 合并，最终提交标题仍须符合规范；不要保留无意义的默认标题。不能用强推绕过主分支规则。

**English:** prefer Chinese prose and include additions, removals, modifications, precautions, and actual verification. Write `无 / None` when not applicable. Identify checks not run; a screenshot or build is not device acceptance.

## 4. 提交前自检 / Before committing

```sh
git status --short
git diff --check
git diff
# 激活 ESP-IDF 5.5.4 后，在 firmware/rootmaker 中：
idf.py build
```

- 检查暂存内容，只添加本次需要的文件，不盲目 `git add .`。
- 不提交中间产物、managed component 缓存、本地 `sdkconfig`、IDE 缓存、串口日志、设备转储、备份、密钥、Wi-Fi 密码、配对凭据或诊断中的个人信息。
- 已审定字体子集、静态字形表和图标是必要资产；修改时保留来源和第三方声明。
- UI/API 变化同步更新文档；出厂脚本检查实际打包输入 `firmware/rootmaker/fs`，不要只修改未被打包的示例。
- 不默认擦除 NVS，不把改分区、删脚本或烧录设备当作无风险验证。
- 写明剩余问题，不用“全部完成”替代验证边界。

### 固件版本更新

**所有面向 main 的 PR 都必须在同一 PR 中递增实际固件版本，包括纯文档、测试和 CI 改动。**
不留到合并后或下次烧录，也没有路径过滤、标签或说明文字豁免。没有另行指定发布版本时，
默认末位加一，例如 `0.10.2 → 0.10.3`；合并前重新核对目标分支，避免并行 PR 重用版本。
版本必须是无前导零的数字三段 `major.minor.patch`，按数值比较严格大于 PR 目标分支版本。

1. 以 `firmware/rootmaker/CMakeLists.txt` 的 `PROJECT_VER` 为版本源，先更新它，
   再检查固件构建描述、SETTING → VERSION 页面及同源模拟器是否读取一致的值。
   界面应沿用版本数据源，不另写一个仅供显示的新版本字符串。
2. 同步中英文 README 的当前版本、使用/升级说明、相关接口文档中的当前支持状态，
   以及依赖当前版本的测试或生成资产。保留历史发布记录、旧版本指南和“首次支持版本”的含义，
   不做全仓库无差别替换。
3. 运行相关版本测试并重新编译，核对新构建产物的版本元数据；涉及界面显示时核对同源
   模拟器的 VERSION 页面。只有实际烧录并读取过的设备版本才能作为实机证据。
4. 提交前在 PR 的“版本更新”中填写原/新版本、同步位置、执行结果及未执行项。
   存在遗漏时不能将该 PR 宣称为交付完成。

`Firmware version` 对所有 PR 比较目标提交和当前 `PROJECT_VER`，同号、回退、非法或重复
声明都失败。`ESP32-S3 build` 进一步检查生成配置、构建描述与 `.bin` 中的应用版本，
并运行原生 Setting → Version → Info 的同源 Host 测试。分支保护要求与 main 保持最新，
防止两个 PR 同时使用同一个新版本。主分支 push 比较更新前版本；手动运行校验当前版本
与产物一致性，使用独立的 `Manual ...` 检查名，不代替 PR 的两项必需检查。CI 不代替实机验收。

本地运行（从仓库根目录，先抓取最新 main；产物校验需先构建）：

```sh
python3 -m unittest discover -s tools/tests -p 'test_check_firmware_version.py'
python3 tools/check_firmware_version.py --base-ref origin/main
python3 tools/check_firmware_version.py --build-dir firmware/rootmaker/build
```

**English:** every PR into main, including documentation, tests and CI, must include a
strictly increasing numeric three-part firmware version, by default incrementing the last
number. Use `PROJECT_VER` as the source of truth, synchronize the
VERSION page, simulator and current documentation, and record verification in the PR.
The two required checks enforce source-version growth, built-binary consistency and native
version-page tests. Recheck the target branch before merging. There are no opt-outs.
Preserve historical versions and distinguish Host evidence from device verification.

## 5. 架构边界 / Architecture boundaries

- C/LVGL 原生 UI 使用静态字形；`CONFIG_RYZ_LUA_FREETYPE` 仅为 Lua 可选能力，默认关闭。
- Wi-Fi/BLE 配置、凭据与配对由 C 系统层持有，Lua 不泄露密钥或绕过状态机。
- Lua 外设和协程保持共同的取消、限时、内存与租约边界；IMU 由每个 Lua 作业显式初始化。
- 模拟器复用固件 UI/运行时和出厂脚本，硬件适配使用明确虚拟数据；不要另写一套外观相似的演示冒充同源验证。
- 普通 Lua 工具不增加仅该工具可用的隐藏 C 业务页面；新增能力先定义可复用公共接口。

详见 [Lua 平台契约](docs/software/firmware-lua-platform.md) 与 [外设 API](docs/software/firmware-lua-peripherals.md)。

## 6. 许可与敏感信息 / Licensing and sensitive information

项目许可证尚待维护者选定。第三方代码、字体和其它资产遵守各自许可证；不自行宣称全仓库已采用某种许可证，不移除第三方声明。

公开 PR 前检查截图、QR、日志和附件。QR 可能含 AP 密码，报告可能含用户输出；用明确虚构数据替代凭据，但不要伪造测试结果。
