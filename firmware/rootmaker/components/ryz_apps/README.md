# Apps 后台读取

本组件只为系统 Apps 提供目录/详情值快照。它不拥有页面，不执行 Lua，不启动、删除或修改文件，也不替代 Store 的 CAS 和运行许可。生产依赖 `ryz_script_store`、`ryz_script_metadata`；私有平台接口只适配线程、互斥锁和唤醒。

## 调用与所有权

- `ryz_apps_start()` 幂等创建一个 boot-lifetime reader；创建失败可重试，并发启动未完成时返回 `ESP_ERR_INVALID_STATE`。成功不代表 Store 可读。
- 页面控制者提交 `request_page` 或 `request_detail`，随后只调用 `get_snapshot`。提交、快照和 `close` 不做文件 I/O；缓存锁内只有有界参数/值复制。一个在读请求加一个 latest 请求，新请求覆盖尚未处理的旧意图。
- 请求 ID 为 boot-local、非零、不回绕的 64 位值。完成只在 ID/view 仍匹配且状态仍为 LOADING 时发布。`close` 立即清空视图，不等待、也不能中止已进入的文件系统调用。
- 单一页面控制者还必须比较导航 generation/route/modal。reader 不验证导航，也不主动跳页；自动目录刷新会产生新 request ID，不能永远等待第一次请求的 ID。
- 公共快照仅在 READY/EMPTY 时有可用数据，其余状态清除 page/detail。`store_status_valid=false` 表示健康状态未知，不可把默认 `store_ready=false` 当作已确认离线。

## 状态与文件身份

| 情形 | 结果 |
| --- | --- |
| 首次或显式刷新 | LOADING → READY / EMPTY / FAILED / STALE；不存在、失败、过期不伪装成 EMPTY |
| 已显示目录的 Store revision 改变 | 自动读取当前页；删除后该页为空/越界则回第一页，后续增长仍保持实际页偏移 |
| 显式请求不存在的非零偏移（包括 offset 等于 total） | FAILED / `ESP_ERR_INVALID_ARG`，不隐式改为第一页 |
| 已选详情的 revision 改变 | STALE，清除身份/元数据；必须刷新并重新选择，不自动选中新版本 |
| 同 revision 的 unready/recovery | FAILED 并撤销缓存；不只盯 revision |
| 周期 status 竞争返回 TIMEOUT | 保留已有有效快照，视为一次未观察；实际请求的读取 TIMEOUT 则失败，需显式重试 |
| FAILED / STALE | 不周期重扫目录或自做恢复；显式刷新、重新选择或重新进入重试 |

单页最多 16 项，Store 索引最多 256 项；这些是数据读取上限，不决定最终 240×240 页面行数。正常写入必须经过 Store；绕过 Store 的外部文件系统写入不在自动 revision 观察契约内。

有效源码的文件信息、SHA 和元数据来自同一个 Store 快照，解析后释放源码，不向 UI 发布源码指针。空、超限或含 NUL 文件可得到 `source_valid=false` 的原始身份：两次 raw describe 包夹同样失败的读取，并复核 revision/健康状态、字节数、名称及 SHA。`source_valid` 只说明 Store 的大小/NUL 条件，**不是** Lua 语法、可信作者或运行安全认证。后续运行/删除仍须对所选 SHA 做准入；本组件不执行这些动作。

## 当前接线与证据边界

Workbench UI Owner 在进入现有 Apps 路由的新导航代次时启动 reader 并请求第一页，离页、模态或 Lua 任务接管屏幕时撤销。启动错误每 5 秒重试，独立于 runtime gateway。`info.apps` 只读取缓存诊断，不提交或取消页面请求；request/completed ID 用十进制字符串避免 JSON 数字精度损失。

当前屏幕仍是原 renderer，没有新增 LVGL 列表、详情、长按、滚动或错误提示控件。详情 API 已有 Host 行为证据，尚无产品页调用；目标链接会剔除未引用的 `ryz_apps_request_detail` 入口，不据此声称整页接通。

FreeRTOS 适配使用优先级 3、8192 字节栈和 200 ms status 观察等待；这不是实机扫描耗时、响应上限或栈峰值的验收结论。Host 用受控 pthread 调度与实际生产 reader/Store/metadata，使用 POSIX 临时文件和 CommonCrypto：

```sh
# 在仓库根执行
PYTHONDONTWRITEBYTECODE=1 python3 firmware/rootmaker/tests/test_apps.py -v
```

完整命令、构建及分层结果见 [P3 执行记录](../../../../docs/software/firmware-script-apps.md#7-第三段后台目录详情与路由生命周期)。
