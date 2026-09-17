# Yoke 模块目录

Yoke 是围绕 Root 主机设计的模块化配件系统。当前 Wiki 分类页公开了 6 个 RS-Port 模块；F-BUS 与 S-BUS 分类仍以“即将推出”为主。下表为页面信息的研发摘要，不替代版本化数据手册。

| 模块 | 主要功能 | 接口/通信 | 页面标称供电 | 关键边界 |
| --- | --- | --- | --- | --- |
| [Yoke-KEYW](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-KEYW-ZH) | 带底灯轻触开关 | RS-Port / GPIO | 3.3 V，最大 10 mA | 结构不要持续压迫按键；导光键帽用于可见背光 |
| [Yoke-MOTO](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-MOTO-ZH) | 小型直流电机正反转、PWM 调速 | RS-Port / GPIO、PWM | 3.3–5 V，约 450 mA 过流保护 | 电机堵转、启动电流和机构侧向力是主要风险；禁止热插拔 |
| [Yoke-RAD60](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-RAD60-ZH) | 60 GHz 人体存在/运动/微动检测 | RS-Port / UART | 3.0–5.5 V，典型 5 V / 80 mA | 59–64 GHz、水平/俯仰约 ±60°、页面称最远约 10 m；不支持串接 |
| [Yoke-RGBW](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-RGBW-ZH) | 单颗 RGBW 灯 | RS-Port / 单线数字灯效协议 | 3.3–5 V，最大 90 mA | 支持级联；满亮白光需重点核算供电和温升 |
| [Yoke-TRGBW](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-TRGBW-ZH) | 3 颗 RGBW 灯 | RS-Port / 单线数字灯效协议 | 3.3–5 V，最大 250 mA | 支持级联；与 RGBW 混用时注意数据箭头方向 |
| [Yoke-TOUCH4](https://wiki.ryzobee.com/zh/Yoke_Model/Yoke-TOUCH4-ZH) | 4 路电容触摸输入 | RS-Port / 4 路触摸端子 | 3.3 V，最大 20 mA | 触摸线越短越稳；远离金属、电机、电源与高频线路 |

## 选型提示

- 想做最小交互验证：KEYW + RGBW/TRGBW。
- 想做静态人体存在检测：RAD60；需在实际外壳、安装方向和目标姿态下重新标定。
- 想做隐藏式交互面板：TOUCH4；先用短导线和小面积触摸片建立基线。
- 想驱动机构：MOTO；先测电机空载、启动、堵转电流，不要用 450 mA 保护值代替正常工作预算。

## 共同安装规则

- 一律断电接线，确认接口方向。
- 先验证单模块，再逐个增加负载。
- 3D 外壳需为线缆、散热、维护和受力隔离留空间。
- 雷达前方不要用金属、金属涂层或碳纤维；多个雷达避免正对，Wiki 建议间距 1 m 以上。
- 灯效高亮、电机堵转或多模块并用导致主机重启时，优先检查电源余量和线缆压降。

## 机械文件入口

官方页面提供 RootMaker 和各模块的 STEP 下载入口，但本仓库不保存副本。链接见 [来源索引](../sources.md)。

