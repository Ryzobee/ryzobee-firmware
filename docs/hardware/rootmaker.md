# RootMaker 硬件摘要

## 产品角色

RootMaker 是当前公开的 Root 系列主控，面向 3D 打印互动作品、桌面装置、教育项目和快速硬件原型。官方资料描述其支持 Wi-Fi、BLE、USB-C、板载外设以及 RS-Port、F-BUS、S-BUS 扩展。

## 官方页面给出的主要规格

| 项目 | 摘要 | 证据状态 |
| --- | --- | --- |
| 主控 | ESP32-S3 | Wiki Arduino API 页 |
| Flash | 16 MB，QIO 80 MHz | Wiki Arduino API 页；与 GitHub 组织简介的 8 MB 冲突 |
| PSRAM | 2 MB QSPI | Wiki Arduino API 页与 GitHub 资料一致 |
| 屏幕 | 240×240、ST7789、SPI | Wiki Arduino API 页；产品页将屏幕作为套件尺寸列出 |
| 触摸 | 资料为 CST816T；当前样板 ID 为 CST816D | Wiki/BSP 命名；2026-08-28 实读 `0xB6`，见触摸记录 |
| IMU | LIS2DWTR 三轴加速度计 | Wiki Arduino API 页 |
| LED | WS2812B RGB | Wiki Arduino API 页 |
| 电池管理 | TP4054 充电 + PY32 电量管理 | Wiki Arduino API 页 |
| 无线 | 2.4 GHz Wi-Fi、Bluetooth LE | 产品页 |
| USB | USB-C，供电、烧录、串口调试与配置 | 产品页 |
| 输入 | 5 V DC；官方建议 5 V / 1 A 以上 | 产品页 |
| 尺寸 | 基础组件 40×40×10.1 mm；屏幕套件 40×40×15.4 mm | 产品页 |

以上不是采购验收规格。Flash 容量、屏幕是否属于基础配置、USB 串口芯片及各硬件版本差异必须按实物复核。

2026-08-27 实测补充：本次接入样板经 `esptool` 与应用运行时双重确认，使用 ESP32-S3 revision v0.2、16 MiB Flash、2 MiB PSRAM；USB 描述符为 `1A86:55D3`。这只确认当前样板，不覆盖其他硬件版本，亦未验证屏幕/触摸等外设。详见 Lua 实机记录（开发资料未随公开源码分发）。

2026-08-28 更新：0.2.0 按官方 BSP 固定提交接入 ST7789 显示驱动，清屏/矩形/文字的画布与 SPI 传输检查通过。0.3.0 继续接入触摸：实物 ID 为 `0xB6`（CST816D），已完成 I²C/Lua 检查并记录到按下/松手，实屏坐标对齐仍待用户确认。见 屏幕记录（开发资料未随公开源码分发）与触摸记录（开发资料未随公开源码分发）。

来源：[RootMaker 产品页](https://wiki.ryzobee.com/zh/root/rootmaker)、[RootMaker Arduino API](https://wiki.ryzobee.com/zh/dev/rootmaker_arduino_api)、[GitHub 组织页](https://github.com/Ryzobee)。

## 扩展接口

### RS-Port

- 4 个带锁扣接口；产品页给出的物理连接器为 GH1.25-4P。
- 每个接口提供 2 个 GPIO，逻辑电平为 3.3 V TTL。
- 页面标称每口 3.3 V 最大 0.8 A，整机 3.3 V 输出总计不大于 1.5 A。
- 适合通过线缆连接按键、灯光、触摸、电机和传感器等模块。

### F-BUS

- 主机顶部与底部各有接口，页面给出的连接器为 1.27 mm 2×12 排母。
- 页面标称 16 个 GPIO、3.3 V TTL、3.3 V 供电最大 1.0 A、VBAT 最大 1.0 A。
- 面向屏幕、热成像、电池等信号更多或需要结构固定的模块。
- 原始规格表备注存在明显复制错误，使用前必须依据对应硬件版本的针脚表确认。

### S-BUS

- 金手指形态，页面称为 SFP 金手指连接器。
- 页面标称 14 个 GPIO、3.3 V TTL、3.3 V 最大 1.0 A、VBAT 最大 1.0 A。
- VBUS 随供电来源在 VBAT 到 5 V 间变化，且受整机开关机控制。
- 适合语音、音频、调试或专用模块；机械固定弱于 RS-Port 和 F-BUS。

来源：[RootMaker 产品页的接口规格](https://wiki.ryzobee.com/zh/root/rootmaker#%E8%AF%A6%E7%BB%86%E6%8E%A5%E5%8F%A3%E8%A7%84%E6%A0%BC)。

## 使用与安全约束

- 模块应断电插拔；官方对电机模块明确写为禁止热插拔。
- 使用超过 3.3 V 的逻辑信号必须加电平转换，否则存在损坏 GPIO 的风险。
- 多个灯光、电机、显示或电池模块并用时，不能只看单口上限，还要核算整机 3.3 V 总预算、VBUS/VBAT 路径和温升。
- USB 烧录需要支持数据传输的 USB-C 线缆。
- 官方快速指南 PDF 与 STEP 文件只在 [来源索引](../sources.md) 中列远程入口，未在本仓库镜像。
