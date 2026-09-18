# Lua 通用外设接口

本接口用于普通用户脚本及出厂工具。工具的扫描循环、解析、日志历史、
暂停、筛选和 UI 放在 Lua；C 只提供通用驱动、资源仲裁和退出清理。
2026-09-15：以下接口及三份工具已通过公共 Lua/脚本验证、同源模拟器与
ESP-IDF 5.5.4 完整构建。最终证据（开发资料未随公开源码分发）
区分实现、虚拟外设验证和未进行的实机验收。本轮未烧录。

`feat/boot_event` 分支新增的 [`boot.poll()`](firmware-lua-boot-events.md) 是
独立、无句柄的实体 BOOT 事件接口，支持 App/legacy；不受下文 GPIO `open`
参数控制，也不开放 BOOT 引脚重配置。单击/双击/3 秒长按与系统约 5 秒退出的
关系、固定队列和错误恢复见专页。该新增接口的验证状态不沿用上述旧外设记录。

## 共同规则

- `require('gpio'/'timer'/'pwm'/'i2c'/'spi'/'uart'/'adc'/'log'/'led')`。
- `open{...}` 成功返回句柄；环境错误返回 `nil, reason`。
- 参数拼写、类型、范围错误直接产生 Lua 错误，不静默忽略未知配置。
- `handle:close()` 显式释放，成功后再次 close 安全；其他操作返回 `nil,'closed'`。
  从第一次 close 开始就撤销普通操作；返回 busy/failed 可能已部分释放，
  不能再读写或 status，只能重试 close。句柄仍占用名额，直到关闭成功或 Job 结束。
- 每个脚本最多 16 个打开的通用外设句柄；所有协程共享。同一脚本中的
  多个句柄也不能重复占用排他引脚/控制器。句柄不可跨 Job 使用。
- 不依赖 Lua GC 或用户 finally：正常结束、错误、超时和 BOOT 终止后，
  C 统一尝试关闭。未确认硬件释放时保留租约，不允许另一个脚本抢占。
- 单次二进制数据最多 256 字节；单次驱动等待最多 20 ms。长事务由 Lua
  分段组织，期间执行原有共同取消、执行期限和堆限制。
  20 ms 是显式 SDK 等待参数的上限，不是整个函数墙钟时间保证：SDK 短锁、
  日志输出、调度仍可能耗时。I²C/SPI 超时不代表事务回滚，总线上可能已经
  发生传输；重试须由用户协议确保幂等，低速总线应缩短 chunk。
- 正常用户脚本（包括自启）统一在 CPU1 的 Lua worker 创建和清理外设。
  显式旧开机诊断在 CPU0 上运行，不获得通用外设会话；返回 unavailable。
- 无后端的 Host 返回 `unavailable`，不会伪造硬件成功。测试中的明确
  虚拟外设环境不等于设备验收。
- Wi-Fi/BLE 配置仍由 C 启动固件管理；原有只读连接状态与显式 IMU 初始化不变。

## 接口

```lua
local gpio = require('gpio')
local pin = assert(gpio.open{pin=13, mode='output', pull='none', initial=0})
assert(pin:write(1))
local level = assert(pin:read()) -- 0 / 1
assert(pin:close())
```

GPIO mode：`input`（默认）、`output`、`open_drain`；pull：`none`（默认）、
`up`、`down`、`both`。板载关键功能、存储器和 USB 引脚不开放重配置。
当前 Quad 内存构建的外接候选为 GPIO13–18、21、33–38、47、48；Octal
构建另排除 33–37。候选表示软件准入，不证明你的外接线已接好；ADC、PWM
等还需满足对应控制器能力。固定 BSP 与保留脚说明（开发资料未随公开源码分发）。

```lua
local timer = assert(require('timer').open{period_ms=100, periodic=true})
local due = assert(timer:poll()) -- 自上次读取起到期次数，尚未到期为 0
```

定时器为单调时钟驱动的轮询定时器；不会在 ISR 执行 Lua，也不会隐式调度
协程。period_ms 为 1..3600000，periodic 默认 true；单次定时器到期消费后不重复。

```lua
local pwm = assert(require('pwm').open{pin=13, frequency_hz=1000, duty=500})
assert(pwm:set_duty(250)) -- 0..1000，对应 0..100%
local serial = assert(require('uart').open{port=1,rx=17,baud=115200,
    bits=8,parity='none',stop=1}) -- 省略 tx 为只收，不驱动 TX
local bytes, status = serial:read(128, 0)
```

PWM frequency_hz 为 1..1000000，实际可实现频率由后端校验。UART port 为
1/2（默认 1）；tx/rx 至少一个，省略方向为 -1；baud 300..5000000（默认
115200），bits 5..8、parity `none/even/odd`、stop 1/2。
`serial:write(bytes)` 返回接受的字节数，可能部分接受，不代表线发送完毕。
UART/log read 的 n 默认 256，timeout 默认 0；无数据成功返回空串。

```lua
local bus = assert(require('i2c').open{sda=13,scl=14,frequency_hz=100000})
local found, reason = bus:probe(0x50, 10) -- NACK 为 false，不是伪造故障成功
local bytes = bus:transfer(0x50, string.char(0), 4, 10) -- repeated START
-- bus:read(address,n[,timeout_ms]); bus:write(address,bytes[,timeout_ms])
local board_bus = assert(require('i2c').open{board=true}) -- 只允许 probe
```

I2C 地址 0x08..0x77；frequency_hz 100000 或 400000，默认 100000。自定义
总线需要 sda/scl。board 总线共享触摸和 IMU，不能改引脚或时钟，也不开放
原始写入；IMU 使用 `require('imu')`，避免破坏触摸和显式初始化契约。

```lua
local spi = assert(require('spi').open{sclk=13,mosi=14,miso=15,cs=16,
    frequency_hz=1000000,mode=0})
local received = spi:transfer(string.char(0x9f,0,0,0))
local adc = assert(require('adc').open{pin=13,attenuation_db=12})
local raw = adc:read()
local mv, reason = adc:read_mv() -- 校准不可用时明确返回错误，不冒充 mV
```

SPI 使用空闲控制器，mode 0..3，frequency_hz 100000..20000000；sclk/cs 必填，
mosi/miso 可省略其一，全部引脚必须互异。`spi:write(bytes[,timeout_ms])`、
`spi:transfer(bytes[,timeout_ms])` 和 `spi:read(n[,timeout_ms])` 均最多 256 字节，
timeout 默认 10 ms。transfer 为同长度全双工，read 在存在 MOSI 时发送零字节。
ADC attenuation_db 为 0/2/6/12（2 表示 SDK 的 2.5 dB 档位），默认 12。
ADC2 与无线资源竞争、校准不可用等均是可见失败，不返回伪造采样。

```lua
local log = require('log')
local stream = assert(log.open{level='info'})
assert(log.write('info','scan started'))
local bytes, status = stream:read(256,0)
```

日志等级为 `error/warn/info/debug/verbose`。订阅不改变全局日志等级；
系统诊断遵循安全过滤，不向脚本暴露无线凭据。read 第二返回为状态表：
UART 使用 `error_events`（观测到的溢出/帧错误/校验错误事件下限，不是丢失
字节数），LOG 使用 `dropped_bytes`（能精确计量的丢弃字节）。计数均是精确
十进制字符串（包括零 `"0"`），避免冻结的 32 位 Lua 数值溢出。两者都提供
`loss_possible`，表示可能还有无法精确计数的丢失；零计数不能证明流无损。
Monitor 的暂停/清空/历史及分行仍由 Lua 实现。

## 板载 RGB LED

```lua
local led = assert(require('led').open{board=true})
assert(led:write(255,106,0)) -- 请求已接受，尚不是发光成功
local state = led:status()
-- state.state: idle / pending / ready / failed
-- state.red / green / blue / output_known
assert(led:close()) -- 正在释放时可能 nil,'busy'，稍后重试
```

单颗 GPIO45 WS2812B，由已有通用 RMT 驱动提供 RGB8 输出。open 不驱动灯，
不借用以前脚本的完成状态；write 三个整数范围 0..255。ready 与 output_known
只证明当前会话的数据帧发送完成，不证明光学观测。Lua 自己决定红/绿/蓝测试
顺序及用户确认；C 不含 Device Test 流程。

句柄独占灯资源，包括旧 RPC 路径。close 只释放资源，不自动发送黑色；需要
关灯应先显式 write(0,0,0)，等待 ready，再 close。BOOT 取消只尝试资源清理，
不执行用户退出回调。电池目前无可信读取协议，不以 LED 或任意 ADC 数据推测电量。

## 示例与验证范围

用户选择的测试 seam 为 Lua 公共入口和脚本。SDK 内部单测与外接电气输出
不作为本轮交付的验证依据；构建只证明目标适配可编译链接，不证明电气行为。

三份可编辑、可删除的出厂源码分别是
[Monitor](../../firmware/rootmaker/scripts/tool_monitor.lua)、
[I²C Tools](../../firmware/rootmaker/scripts/tool_i2c.lua)、
[Device Test](../../firmware/rootmaker/scripts/tool_hardware.lua)。它们均无
`require('tools')`，无需增加工具专属 C 流程。默认脚本不自动恢复覆盖用户修改；
镜像打包仍只包含这三个原文件及对应 CREATED/MODIFIED 记录。

本轮不包装 ESP-IDF 的全部方法：定时器是可取消的轮询接口，不提供 Lua ISR；
SPI/I²C 是有界主机事务，LED 是单颗板载 RGB。无线仍只读连接状态。
SDK 私有 I²C 兼容检查锁定 5.5.4，升级 SDK 或改变核/内存配置必须重新审查。
Studio 的生成 SOP/通用句柄能力分析未在本轮扩展；新脚本必须配套本轮固件。
