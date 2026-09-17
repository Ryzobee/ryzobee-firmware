# Lua Workbench 串口协议 v2

固件 0.4.0。一个 UART RX 所有者，115200、8N1。UTF-8 单行 JSON；接收最大 24575 字节（含 JSON 编码开销，不含结束 NUL）。网页以 LF 结束；终端同时支持 CR / CRLF。丢弃超长行至下一换行后恢复。JSON 必须完整、不得有尾随内容，字符串不得含 NUL。

## 请求与响应

请求直接发送 JSON，无前缀。`id` 为调用方生成的唯一字符串，正常响应原样带回。不要重复使用 ID 来假定幂等性：**固件没有执行请求去重表**，重复发送运行命令属于第二次运行请求。

```json
{"id":"web-1","op":"console","command":"lua --run-async --path hello.lua"}
```

设备每帧以换行分隔，并使用以下前缀区分：

```text
RYZOBEE_RPC {"id":"web-1","ok":true,"job":{"job_id":"a1b2c3d4-1","name":"hello.lua","sha256":"...","state":"running","stop_requested":false,"elapsed_ms":0,"log_next_seq":0,"dropped_bytes":0},"output":"Lua job started"}
RYZOBEE_EVENT {"event":"output","boot_id":"a1b2c3d4","job_id":"a1b2c3d4-1","seq":0,"data_b64":"SGVsbG8K"}
RYZOBEE_EVENT {"event":"job","boot_id":"a1b2c3d4","job":{"job_id":"a1b2c3d4-1","name":"hello.lua","sha256":"...","state":"done","stop_requested":false,"elapsed_ms":20,"phase":"done","error":"","log_next_seq":1,"dropped_bytes":0,"lua_peak_bytes":12345}}
```

上面为结构示例，不代表真实任务结果。其他无前缀行可能是 ESP-IDF 启动/系统日志。按流拼完整行，不把一次串口读取当一帧；不从提示符猜测命令是否执行结束。只匹配待处理 `id`，忽略已经超时或旧会话的响应。

运行确认和任务完成事件可能因为任务调度而交错；完成状态不能被迟到的 `running` 确认覆盖。控制帧优先，最终输出事件也可能晚于完成事件。输出 `seq` 按产生的块编号，丢弃仍递增；按同一任务增量解码 Base64 的原始字节并拼 UTF-8，遇到缺号提示丢帧。

## RPC 操作

| `op` | 请求参数 | 主要返回 | 运行中允许 |
| --- | --- | --- | --- |
| `info` | 无 | 芯片/固件/IDF/Lua、`device_id`、`boot_id`、`protocol_version`、资源、`job`、`recent_job` | 是 |
| `console` | `command`，1–512 字节单行 | `output` / `job` / `jobs` / info 字段，或明确错误 | 是；启动第二个任务拒绝 |
| `list` | 无 | `files: [{name,bytes,protected}]` | 否 |
| `get` | `name` | `name,source,sha256,bytes` | 否 |
| `put` | `name,source`，可选 `sha256` | `sha256,bytes` | 否 |
| `remove` | `name` | `ok` | 否 |
| `eval`（兼容） | `source`，可选 `timeout_ms` | 执行结束后的旧结果格式 | 空闲时可启动 |
| `run`（兼容） | `name`，可选 `timeout_ms` | 执行结束后的旧结果格式 | 空闲时可启动 |

`put` 的 SHA-256 在旧客户端中可省略；网页总是提供并校验响应。源码 1–8192 字节；`boot.lua` 仅允许读取/执行，拒绝写入/删除。路径无目录层级，不接受 `../`、绝对路径或双后缀伪装。详细长度以固件和 SPIFFS 返回为准。

`device_id` 是应用固件以 ESP32-S3 eFuse 基础 MAC 加固定域分隔符计算出的 SHA-256 小写十六进制标识；它不直接暴露 MAC，重启后保持不变，用于将真机诊断和项目记忆绑定到同一物理设备。`boot_id` 仍是每次启动变化的会话标识，不能替代 `device_id`。旧固件可能不返回 `device_id`；客户端必须兼容读取，但此时应 fail closed，不创建或召回硬件经验。

```json
{"id":"web-2","op":"put","name":"hello.lua","source":"print(42)\n","sha256":"<UTF-8 源码的 SHA-256 小写十六进制>"}
{"id":"web-3","op":"get","name":"hello.lua"}
{"id":"web-4","op":"list"}
{"id":"web-5","op":"remove","name":"hello.lua"}
```

未知操作/命令、缺失参数、文件非法/不存在、忙碌、校验失败等返回 `ok:false,error:"..."`。无法解析 JSON/超长行时没有可信请求 ID，使用空 ID；主机必须使原请求超时而不能猜测匹配。

## 任务状态

`job_id = boot_id + '-' + 本次启动计数`。`boot_id` 每次启动随机生成。保存当前任务和最近一个完成任务；设备复位不会持久化执行状态。

`state`：`running / done / stopped / timeout / failed`。

- `stop_requested` 是请求标记，不能当作完成状态。
- 结束时包含 `phase`、`error`、`lua_peak_bytes`；`phase` 沿用执行封装的 `done/syntax/runtime/memory/timeout/stopped` 等值，以实际结果为准。
- `sha256` 关联本次执行的源码，不能将报错行号应用到不同版本。
- `lua --job` 额外返回 Base64 `log_b64`（末尾最多 512 字节）和 `log_truncated`。截取可能从 UTF-8 中间开始，应作为带截断提示的最近日志，不能当完整源码/精确全文。
- `dropped_bytes` 表示实时输出队列丢弃的字节，不包括浏览器本地缓存淘汰。

## 连接恢复契约

重连先发只读 `info`：启动标识相同则恢复任务；不同则报告设备重启。未知写入结果通过 `get`+SHA-256 核对；未知启动通过 `lua --jobs` 核对。**不得自动重发副作用请求**。

设备不主动重跑断线前的脚本，也不因串口关闭取消正在运行的任务。主机驱动引起的复位必须通过启动标识识别，而不是掩盖为“重连成功且任务继续”。

## 限制与错误恢复

这是可信开发环境协议，无加密/认证/远程权限模型；普通终端和 JSON RPC 权限相同。控制队列也是有界的，恶意洪泛控制请求仍可丢响应；客户端必须处理超时与不确定性。部分 JSON 上传中断不会写文件；传输后若卡在半行，下一条只读探测可能用于结束坏帧，后续只读探测恢复同步。文件替换过程中掉电不保证事务恢复，见使用指南。
