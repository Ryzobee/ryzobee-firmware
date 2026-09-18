# Lua 通用文件存储（V0.10.2）

`require('fs')` 为 App 和 legacy Lua 提供通用应用数据文件读写，适用于配置、进度、
计数器、小型记录和库存等，不包含特定工具的业务逻辑。`ryz-neuro` 不提供此模块；
`io/os/package/loadfile/dofile` 的限制不变。

## 公共接口

```lua
local fs = require('fs')
local bytes, reason = fs.read('settings.txt')
local ok, reason = fs.write('settings.txt', 'theme=orange')
local files, reason = fs.list()
local info, reason = fs.info()
local ok, reason = fs.remove('settings.txt')
```

| 方法 | 成功返回 | 含义 |
| --- | --- | --- |
| `read(name)` | 字符串 | 整文件读取；空文件是 `''`，不是 `nil` |
| `write(name, bytes)` | `true` | 新建或整体覆盖；支持内嵌 NUL 的二进制字符串 |
| `remove(name)` | `true` | 明确删除当前应用的单个数据文件 |
| `list()` | 数组 | ASCII 文件名排序，每项 `{name='...', size=字节数}` |
| `info()` | table | `used_bytes/file_count/max_file_bytes/quota_bytes/max_files` |

没有文件句柄、目录、任意路径、追加模式或 Lua 可选择的命名空间。
修改小文件先读出、在 Lua 中处理，再整体保存。多文件写入不是事务；需要一致性的数据
应放在同一个文件中。序列化格式由脚本决定，不执行文件内容，也不自动解析 JSON。

## 名称、隔离与容量

- 数据文件名 1–24 个 ASCII 字节，首位为字母或数字，后续允许字母、数字、`_`、`-`、`.`；不允许连续 `..`、路径分隔符、绝对路径、NUL 或非 ASCII 字符。
- 内容支持任意字节，包括 UTF-8；不改变 UI 静态字库的字符范围。
- 单文件最多 **8192 字节**；每个应用最多 **32768 字节、16 个逻辑文件**。
- 全部应用共享最多 **131072 字节、128 个逻辑文件**。这是限额，不是预留空间；实际 Flash 不足仍返回 `no_space`。
- 身份来自 C 启动器的脚本文件名 `[A-Za-z0-9_-]{1,36}.lua`，不取可编辑 metadata，也不由 Lua 参数指定；可选内部 `@` 前缀会剥除。
- 同名脚本更新内容仍访问原存档；不同文件名隔离。匿名 `eval`、路径式 chunk 名或未注入后端的 Host 返回 `unavailable`。
- 自启动是源码副本 `boot.lua`，其数据与手动运行原文件**分开**；改名不自动迁移存档。删除脚本不自动删除数据；重装同名脚本可再访问。

这是应用名访问隔离，不是针对拥有刷机/上传权限者的加密或身份认证。能替换同名脚本
的用户也能访问其存档。Lua 不能借 `fs` 读取其它应用数据、脚本源码、系统 NVS 或无线凭据。

## 错误和保存语义

正常 I/O 失败返回 `nil, reason`，不要只检查结果是不是 `false`：

| reason | 处理建议 |
| --- | --- |
| `not_found` | 文件确实不存在；只有此时可按应用规则初始化默认值 |
| `unavailable` | 后端/脚本身份/挂载状态不可用，不是假装空目录 |
| `quota` / `too_many_files` | 应用或全局限额不足，清理不再需要的本应用文件 |
| `no_space` / `no_memory` | 实际 Flash 或内存不足 |
| `busy` | 并发冲突，有界等待后重试，不无限忙等 |
| `corrupt` / `recovery_required` | 损坏或不完整状态，不能当作首次运行静默重置 |
| `commit_unknown` | 提交结果不确定；重新读取/核实，不盲目重试累计计数或扣库存 |
| `io` | 底层操作或后端返回值异常 |
| `invalid_name` / `invalid_app` / `too_large` | 后端拒绝请求；正常 Lua 入参会更早触发参数错误 |

错误参数、额外参数、错误类型、非法名称、超过单文件上限抛 Lua 参数错误。
正常 I/O 失败走上述返回值，不依赖被禁用的 `pcall/xpcall`。

实现使用隐藏文件、校验和提交记录，成功返回前检查写入、同步、关闭与读回，
并确认提交后的 ACK 尾标记。残留的较新 ACK 记录与缺失的提交记录不一致时，
返回 `recovery_required`，不能把旧记录冒充最新存档。
未完成数据写入不能冒充成功新值；已提交数据损坏不能悄悄回滚成旧库存。
`remove(name)` 是调用者明确丢弃该文件的入口，不自动清空其它文件或分区。
关闭失败会封锁同卷后续 I/O，需要重启；不重复关闭可能已被回收的数字描述符。
`write/list/info` 为核算全局限额会扫描受管数据；其它键存在无法判定的提交状态，
也可能使这些操作保守报错。`read` 只检查目标键，显式 `remove` 可恢复本应用的指定键，
不能通过当前应用清除其它应用的损坏存档。

**不是 SPIFFS 在任意断电/损坏下的绝对原子性保证。** 应用级校验和恢复不能修复
整卷损坏/磨损；重要业务仍需备份、记录格式版本及实机断电测试。
避免逐帧写 Flash，在用户确认或业务状态提交时保存。

操作在 Lua worker 执行，无长期打开的句柄，调用前后检查取消/期限。
Lua hook 不能即时打断正在进行的同步 Flash 操作。BOOT 取消可能发生在数据已写入、
但 Lua 尚未收到返回值时：**退出不等于回滚**。协程共享该应用的数据及运行配额。
`read/list` 的 Lua 暂存及返回字符串计入该作业共享的 Lua 堆限额。
后端临时分配失败返回 `nil, 'no_memory'`；Lua 自身分配失败会终止作业，
不保证还能构造可恢复的 `nil, reason` 返回值。

## 升级和空间显示

复用 `/scripts` SPIFFS，不改变分区表、不格式化、不新增系统 NVS 键。
物理记录存在冗余和校验，`fs.info().used_bytes` 仅为当前应用逻辑数据，
不是整卷剩余空间；首页 STORAGE 包括脚本、应用数据和文件系统开销。
写入/删除后由 worker 刷新容量缓存，UI 不为每帧查询 Flash。
普通写入会检查物理余量，并额外留出 8 KiB 恢复余量供删除/脚本事务使用；
这不是对其它组件的空间锁定，也不保证 SPIFFS 在满盘或损坏后仍可写入。
后端使用有界临时缓冲区并优先分配 PSRAM，不缓存所有应用的文件内容。

**全量项目烧录会覆盖 scripts 镜像，应用数据同样会丢失。** 仅写应用分区的升级不主动
删除数据，但应先核对烧录范围并导出重要数据。本版未新增电脑端通用数据浏览/导出界面，
原脚本上传协议仍只管理 `.lua`。

## 示例与验证

[fs_demo.lua](../../firmware/rootmaker/scripts/fs_demo.lua) 展示持久计数器、二进制/空文件、
删除、列表和容量。它仅是源码示例，不默认打包进 `fs/`，不覆盖 `boot.lua`。
损坏或读取失败时停止，不以默认值覆盖；同名再次运行可观察计数递增。

普通模拟器未注入存储后端时返回 `unavailable`。Host 临时文件、模拟器与设备 SPIFFS
是不同验证层级；Host 成功不代表实际断电/Flash 磨损验证完成。

准备好仓库锁定的 Lua 组件后，可在 `firmware/rootmaker` 运行：

```sh
python3 -m unittest tests/test_lua_fs.py tests/test_lua_fs_store.py tests/test_lua_fs_persistence.py
python3 -m unittest tests/test_runtime_owner.py tests/test_script_store.py
sh tools/test_app_runtime.sh
```

三套文件接口测试分别覆盖真实 Lua facade、实际 C 存储核心与故障注入、
实际示例跨进程持久化。Host 后端只在临时目录中读写，不访问设备分区。
