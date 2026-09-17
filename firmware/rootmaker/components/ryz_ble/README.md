# RyzoBee BLE C owner

该组件是 ESP-IDF **5.5.4 / ESP32-S3 / NimBLE Peripheral** 后端。注册 GAP/GATT、
HID Consumer Control、Battery 和 Device Information 服务；尚无自定义 Lua 数据
GATT 协议。`service_known/service_ready` 保留通用业务状态的旧语义（false）；
HID 使用独立的 `hid_ready/hid_busy/hid_sdk_error`，不能混淆手机订阅与 App 就绪。

## HID 媒体控制开发验证版（2026-09-15）

用户已批准真实 HID 媒体功能，同时明确暂无分配 VID/PID，先做开发验证版。
**这不是完整 HOGP 合规产品，也尚未证明 iPhone 系统设置首次发现已成功。**

- HID `0x1812`、BAS `0x180F`、DIS `0x180A` 由同一 NimBLE host 注册；没有第二套
  蓝牙栈。HID Report Map 只有 7 位 Consumer Control：播放/暂停、停止、下一曲、
  上一曲、静音、音量加/减；其余一位 padding。Report Reference ID=1/type=input。
  包含 HID Information、Report Map、Input Report、Report Reference、Control Point
  的 Suspend/Resume。不是键盘/鼠标，无 Boot Protocol、RemoteWake 或自动输入。
- NormallyConnectable=0，匹配本产品有限配对/开机回连窗口；不谎称永久可连接。
- 主广播为 Flags + 不完整服务列表中的 HID UUID + Generic HID Appearance +
  ANCS Solicitation，共 29 B。完整名称与实际 TX Power 放扫描响应，共 12 B。
  两者设置都成功后才启动广播。DIS UUID 不放广播。
- DIS 提供真实 Manufacturer/Model/固件版本。没有合法 PnP identity 时读 PnP ID
  明确返回 ATT 错误，不借用其他厂商 ID。私有 identity setter 可接后续分配值，
  目前未配置。BAS 尚无板载有效百分比来源，读电量返回未知错误，不发送伪造百分比。
  **这两个缺口可能影响 host 的 HID 枚举，必须在实机验证中保留；不能宣称全协议完成。**
- 所有 HID 业务值访问及通知检查当前加密/MITM/16 字节密钥和已确认持久绑定；
  CCCD 由 SDK 强制加密/MITM。订阅恢复由 SDK 发事件给同一 owner，连接 epoch
  排除旧回调。取消 CCCD 只允许当前已认证 peer 的精确条目删除，整记录提交/回读，
  不允许 SDK 自动删除密钥或替换旧绑定。
- C 公共输入租约与 Lua Job 对应；同一时间最多一个 lease 和一个 pending tap。
  `open` 不产生按键，`tap` 只接受枚举按键，成功仅表示准入。原 100 ms owner poll
  执行一次 press，100 ms release deadline 到达后释放；没有新增线程。实际调度延迟
  需实测，不承诺硬实时 100 ms 或手机一定收到。
- Job 结束/错误/超时/BOOT 取消撤销 lease，取消尚未 dispatch 的输入。已交给
  host 的输入无法撤回，但继续请求释放；新 Job 等待 cleanup，不复用 token。
  断连清除待执行输入和 per-link 句柄状态，显式下一次 tap 才能作用于新连接。
- 关闭前尽量发零报告，再终止连接；释放/权限/传输失败由 owner 断链。终止请求
  失败按原 100 ms owner 周期重试，最多到原 5 s 停止期限；成功受理后等真实
  DISCONNECT，TERM_FAILURE 才重新允许尝试。持续控制器故障仍保持 STOPPING、
  拒绝新输入，不能保证物理断链或谎报 OFF，必要时需用户恢复设备。
- Lua `hid` 是独立能力，不强迫其他 Lua App 使用 HID，不开放无线配置或动态
  Report Map。自定义 GATT 数据通道仍未实现，不能把 HID 当作任意数据通道。

回归分别覆盖真实 Profile 的 GATT 定义/访问/报告、真实 owner 的 SDK/租约路径，
以及真实 Lua → ESP Adapter 的取消清理；这些 seam 不替代 iPhone 实机验收。
详见 [HID 交付记录](../../../../docs/software/evidence/ble-hid-development-20260915.md)。

## iPhone 系统发现：ANCS 配对与发现阶段

- 本机仍是 GAP Peripheral，同时作为 GATT Client 请求 iPhone 提供的 ANCS。
  主广播使用 **AD type 0x15 / Service Solicitation**，UUID 为
  `7905F431-B5CE-4E99-A40F-4B1E122D00D0`，不是把 ANCS 宣称为本机提供的服务。
  初版 ANCS-only 将完整名称放主广播（30 B）；当前 HID 开发版按上节分配到
  主广播与扫描响应，ANCS solicitation 仍保留。
- 手动 `PAIR`/`RETRY` 的既有 30 秒窗口使用 20 ms 广播间隔；保存设备回连使用
  152.5 ms。只打开总开关、尚未进入配对窗口时不对陌生手机开放配对；持久 OFF、
  已绑定单 peer、取消和超时策略不变。
- 仅在加密、Numeric Comparison 与耐久绑定验证成功后异步发现 GATT 的
  Service Changed 特征，订阅它的 indication，再发现 ANCS 和必需的
  Notification Source 特征。标准 Service Changed 只携带四字节属性句柄范围；
  它使 iPhone 发布/撤销 ANCS 后能够重新发现，而不是永久缓存“未找到”。
- **不订阅 ANCS Notification Source / Data Source，不读取通知正文、标题、
  应用名，不写 ANCS Control Point，不执行通知动作，不新增 Lua 配置/通知 API。**
  这是获准的配对与服务发现阶段，不是完整的通知接收器。协议实现依据
  [Apple ANCS 规范](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html)、
  [TI 的系统设置发现示例](https://github.com/TexasInstruments/ble-sdk-210-extra/blob/master/Projects/ble/ancs/README.md#service-soliciation)
  和 [Apple 广播间隔指导](https://developer.apple.com/library/archive/qa/qa1931/_index.html)。
- `info` 的 `ble` 对象新增 `ancs_state`、`ancs_sdk_error`、
  `ancs_service_changed_subscribed`，均不包含通知内容、密钥或属性句柄。
  `ancs_state`：0 IDLE、1 DISCOVERING、2 DISCOVERED、3 UNAVAILABLE、4 ERROR。
  DISCOVERED 仅表示发现必需特征，**不表示通知授权或数据通道就绪**；原有 UI 的
  `SERVICE` 和 Lua 安全连接语义不变。
- 每个 ATT 操作最多等待 10 秒；发现错误不删除有效绑定、不降低加密要求。
  超时不并发重发仍由 SDK 持有的 ATT 请求，需断连后重试。关闭、断连、控制器
  reset 清除临时句柄；独立请求序号与连接 epoch 拒绝旧会话/旧发现回调。
  Service Changed 在请求中到达时合并处理，先收束旧请求再重新发现。
- IDF 5.5.4 的 `BT_NIMBLE_GATT_CLIENT` 依赖 `BT_NIMBLE_ROLE_CENTRAL`，因此开启
  这两个**编译选项**；本机不主动扫描或发起连接，运行时仍由 iPhone 连接本机。
  保留单连接、单绑定、Host PSRAM 分配，不修改 SDK、不开放用户无线配置。

Host 回归与编译不能证明某版 iOS 的系统列表已经发现或配对成功。最终须在实机
配对窗口内从 iPhone「设置 → 蓝牙」检查 `RyzoBee`，完成双端数字确认并读取
`info`。已有其他手机绑定时须由用户明确执行 Replace/Forget；固件不会自动清除。

## 所有权与安全边界

- `ryz_ble_start()` 只申请独立任务；NVS、Host、广播和配对工作不在 UI/Lua 任务执行。NimBLE Host 事件任务是状态唯一写入者；UI/Lua 只能复制快照或通过受控固件请求入口提交单条命令。
- “OFF”表示没有可接入广播、没有活动连接；**Host/controller 保持初始化驻留**，不表示回收了 Bluetooth RAM 或关闭整个控制器电源。停止失败保持 STOPPING/checking，不能显示成已关闭。
- 只保留一个设备绑定。新配对必须由用户从 EMPTY/RETRY 明确打开 30 秒窗口；必须使用 Secure Connections、MITM、128-bit key 和 **Numeric Comparison**。六位数字含前导零，确认同时校验 operation ID、显示值、当前连接 epoch、实际 peer identity 和期限。不支持 Just Works、静态 passkey、Legacy Pairing 或自动删除旧绑定以重配。
- NimBLE 的 `PARING_COMPLETE`（SDK 常量原拼写）也用于保存密钥恢复；恢复流程不打开新密钥写入权限。新配对的这个事件只允许暂存候选密钥，最终 `ENC_CHANGE` 还必须验证真实安全状态与完整耐久提交。
- `linked` 只表示 GAP 链路。`authenticated` 还要求真实加密/MITM/16-byte key、匹配本地确认保存的 SC 绑定。Lua 只能查询这个安全连接状态，不接触广播、配对、密钥、SDK handle 或配置。
- 每次连接的事件回调带独立 epoch，复用相同 connection handle 不能接纳旧会话事件。取消/拒绝/OFF 的准入先撤销候选写入权限，再通知 Host；COMMITTING 阶段拒绝竞争命令，不接受无法兑现的取消。关闭期间忽略配对/加密/重复配对回调，只允许真实断连推进既定退出目标。

## 启动与持久化

- 保存 OFF：下次启动不广播。保存 ON + 已绑定：开机期间最多等 10 秒，由手机/电脑发起安全回连。超时/失败只把**本次**关闭，保留持久 ON 和绑定。无绑定不自动开放配对，启动直接收束为 OFF。
- 单个 `ryz_ble/record` NVS blob，固定 256 字节，版本、显式字节编码、CRC32。保存开关、本机 IRK、一个 peer 的 OUR_SEC/PEER_SEC、最多 4 条标准 GATT CCCD 和 CSFC；不直接序列化 NimBLE bitfield/padding。
- 缺少记录按首次启动 ON/无绑定；坏格式、CRC、类型、读写错误不当成无绑定。每次保存须 `set_blob → commit → close → 新 readonly handle → 完整字节比对` 全部成功。密钥不会进入日志或公共快照。
- 配对写入失败会读取实际结果，并始终用配对前已知的本机身份/偏好尝试清除候选，即使回读失败。如果清除仍失败，保持 unknown/checking，停止链路，不发配对成功或“已删除”。**持续硬件/NVS 故障无法保证物理擦除**；不得据错误返回宣称存储仍是旧值，需修复存储后重新读取确认。
- Forget/Replace 等待广播停止和连接结束，确认 controller resolving-list 清理，再提交并回读无 peer 的记录。只有确认删除后 Replace 才重新打开完整 30 秒窗口；普通 Forget 返回 EMPTY，不自动配对。不能替手机/电脑删除其 OS 绑定。
- RPA→identity 映射是当前会话的有限临时数据；耐久保存的是真实 peer identity 和 IRK。重启时依照保存的 IRK 重建并确认 controller resolver，不把随机空口地址当作设备身份。

## 固定 SDK 适配约束

`ryz_ble_sdk.h` 对 ESP-IDF 版本进行编译检查。禁用 `BT_NIMBLE_STATIC_TO_DYNAMIC`（它会重装默认 store callbacks）、SDK `NVS_PERSIST`、Legacy Pairing 和自动 IRK reset。控制器保持 S3 controller-based privacy。

Public `ble_gap_unpair()` 和 SDK 自动 IRK 恢复路径可能吞掉错误，因此使用经本地源码核对的私有 `ble_hs_pvcy_set_our_irk()` 与 `ble_hs_pvcy_add_entry()`：重新安装**相同本机 IRK**时对清表、启用、本机条目和 peer 添加逐一保留真实返回结果，不引用不可链接的 static helper。任何 SDK 升级都必须重新审查这些调用、store callback 调用锁、SMP 事件顺序和 Kconfig，不应直接移除版本检查。

Runtime `ble_hs_cfg.sm_sec_lvl=4` 按此版本 C 头文件语义表示 Authenticated Secure Connections；不要与 Kconfig 的选择值编码混用。

SDK 的 `ble_store_read_*()` 接收成员结构体大小的输出对象，再转换为 union
指针调用回调。`store_read()` 只能在成功查找后写对应成员，绝不能按整个
`union ble_store_value` 清零或复制，否则会覆盖 IRK/CCCD 等调用方的栈。
失败不修改输出；回归必须使用成员大小对象，完整 union fixture 会隐藏此错误。

RootMaker 使用 `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` 将 Host 动态
缓冲放在 PSRAM；controller/DMA 分配和任务栈不因此迁移。Workbench 的常驻
24 KiB 文本行也显式使用 PSRAM，UART 仍经内部 staging buffer 读取。实机
故障、修复和限制见[启动内存修复记录](../../../../docs/software/evidence/ble-init-memory-20260914.md)。

## 生产只读启动诊断

通过组件链接选项 `--wrap=ble_buf_alloc`、
`--wrap=esp_vhci_host_register_callback` 记录首次 `nimble_port_init()` 内部
两个调用的真实返回值，以及调用前后的 internal 8-bit free/largest block。
包装器原样转发参数、只调用真实函数一次、保留返回值；不修改 SDK、缓冲区配置、
分配策略、开关偏好或失败后的行为，也不重试初始化。初始化之外的调用继续原样
转发，且不能覆盖首次诊断。

`ryz_ble_get_init_diagnostics()` 返回独立的一致只读副本，包含 `port`、
`buffers`、`vhci` 三个 probe 和 `finished`。必须先检查 `entered` / `returned`，
才能解释 `result`；零初始化、未执行的分支不是成功。数值均为
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 字节，不代表 PSRAM 或总 RAM。
为尽量减少对被测分配顺序的影响，只在整个 port init 返回失败后输出一次
`startup failed` 摘要，包含最先观测到的失败阶段、原始错误码、port 返回码及
该阶段前后的内存数值。初始化成功不输出额外日志。所有内容均不含地址或密钥。

结合固定 SDK 的 `hci inits failed` 日志：buffers 返回非零定位到缓冲池；
buffers 成功而 vhci 返回非零定位到注册；两者成功且仍出现该条 HCI 错误，才可
定位到 HCI 信号量分配。仅 `port.result != ESP_OK` 不能推断信号量失败，因为
Host 的后续初始化也可能报错。探针不能代替实机证据，也不能仅凭其他时刻的
free 数值推断失败瞬间是否缺少连续内存。

这是一组保留在生产固件中的有界启动状态，不是持续 profiling：只使用固定大小
静态记录，无动态分配、轮询任务或按包统计；只在首次初始化查询堆状态，后续
包装调用直接转发且不覆盖记录。保留原始错误的价值在于 SDK 会把多个不同 HCI
错误归并为 `ESP_FAIL`，仅记录 port 返回值会再次丢失故障原因。已移除临时无条件
debug 日志，不通过诊断接口改变 BLE 状态。

包装的符号及跨对象调用方式按固定 SDK 5.5.4 审核；升级 SDK 时必须重新核对
链接 call site、函数签名、初始化顺序，并确认没有新增未被记录的失败分支。
这些诊断本身不修复内存不足，也不自动改变池大小或内存分配策略。

## 验证

`python3 firmware/rootmaker/tests/test_ble_backend.py` 编译真实 owner + storage + SDK Adapter，只替换外部 NVS/HCI/NPL/任务传输；启用 ASan/UBSan。覆盖启动偏好、10/30 秒、真实 SDK 恢复事件顺序、六位确认、取消/迟到事件、提交竞争、NVS 失败和确认删除等。这些 Host 测试不证明实际 RF、手机兼容性、掉电闪存行为或屏幕触摸确认效果；目标构建和用户实机验收是独立证据。
