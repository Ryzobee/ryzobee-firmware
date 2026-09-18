# RyzoBee RootMaker firmware — V0.10.2

这里是 ESP32-S3 的 ESP-IDF 工程入口，包含 C 系统固件、LVGL 界面、Lua 运行时、出厂脚本、Host 测试与模拟器。

## 开发入口

- [中文说明与完整安装、编译、烧录步骤](../../README.zh-CN.md)
- [English overview and build/flash instructions](../../README.md)
- [V0.10.1 用户使用指南](../../docs/software/user-guide-v0.10.1/README.md)
- [贡献、commit 与 PR 规范](../../CONTRIBUTING.md)
- [Lua 平台契约](../../docs/software/firmware-lua-platform.md)与[外设 API](../../docs/software/firmware-lua-peripherals.md)
- [同源 LVGL / SDL 模拟器](host/simulator/README.md)
- [Lua 通用文件存储与持久化](../../docs/software/firmware-lua-filesystem.md)（V0.10.2）

## 最短构建路径

激活 **ESP-IDF 5.5.4** 后，在本目录执行：

```sh
idf.py build
# PORT 换为设备实际串口；先阅读下方数据覆盖警告。
idf.py -p PORT flash monitor
```

默认目标 `esp32s3`，组件由 `dependencies.lock` 固定。普通构建不需要 Studio 或重新生成已提交字形。中间产物和下载缓存不属于提交内容。

**全量项目烧录会覆盖 scripts 分区及用户脚本、自启动副本和 Lua 应用数据。** 先另存需要保留的内容；标准流程不擦 NVS，不等于恢复出厂设置。不要仅为升级而擦除整片 Flash。连接失败先检查端口、占用和 DTR/RTS 下载链路；详见顶层 README。

## 当前操作与边界

- APP INFO 屏幕按住 **2 秒运行**；运行中长按实体 **BOOT 约 5 秒**请求退出并回 HOME。
- C/LVGL 用静态字体；Lua FreeType 可选、默认关闭。
- Wi-Fi/BLE 配置由 C 持有，Lua 通过受控 API 查询连接状态和使用业务能力。
- 配网二维码用**手机系统自带扫码器/相机**，不要用第三方 App 的“扫一扫”。
- BLE HID 为开发验证版，无正式 VID/PID，不保证所有手机系统设置能发现。
- OTA 组件存在但默认更新 URL 为空。模拟器使用虚拟服务，不证明实机通过验收。
