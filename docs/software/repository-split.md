# 固件仓库边界与 Studio 联合调试

本仓库独立维护 RootMaker 固件、Lua 运行时、原生模拟器、测试、静态资产和字体许可。
Studio 的 Electron/Web、AI/MCP 功能和构建缓存不属于本仓库。
本次公开版本从 V0.10.1 源码快照开始，不包含旧迁移历史、设备备份或内部开发记录。

## 独立构建

从仓库内的 `firmware/rootmaker` 使用 ESP-IDF v5.5.4 构建。所需静态字体已经提交；
离线导出工具使用 `assets/typography` 中的源字体，不依赖 Studio checkout。
具体安装、编译和烧录步骤见[中文 README](../../README.zh-CN.md)。

## 可选 Studio 联合调试

如果另有兼容的 RyzoBee Studio 开发仓库，可以通过下列环境变量指定本仓库。
值指向固件工程目录，而不是外层 Git 根目录：

```sh
export RYZOBEE_FIRMWARE_ROOT=/absolute/path/ryzobee-firmware/firmware/rootmaker
# 在 Studio 仓库中执行其开发启动命令。
```

Studio 与固件各自提交、各自通过 PR 更新，不要求安装在固定的个人目录下。
支持 `simulator.lock.json` 的 Studio 版本应在固件修改合并到 main 后，更新并提交
对应的固件 revision 和运行时指纹；遵循该 Studio 版本自己的打包说明。
模拟器通过不等同于真实无线、电气接口或设备触摸验收。
