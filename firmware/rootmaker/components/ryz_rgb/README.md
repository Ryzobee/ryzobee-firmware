# ryz_rgb

板载单颗 GPIO45 GRB 灯珠的受信 C Module。公共 Interface 提供异步颜色提交、按原请求 token 的 cleanup-only 和缓存状态；实际 RMT/时序/恢复由单一 CPU1 Worker 持有。查询不初始化硬件，黑色是实际发送帧，发送完成不是光学自检。

`ryz_rgb_cleanup(request_id)` 只接受当前请求，排队发送可被抢占，正在发送时由同一 Worker 返回后再清理。它不发送黑帧、不更改最后完成颜色策略；重复 CLEANING/CLEANED 幂等，失败需显式按同 token 重试。cleanup 不产生新编号，所以颜色请求编号耗尽后仍可调用。

`resources_held` 包括保留的 channel/encoder，不只是正在发送；COMPLETED 通常仍持有句柄。仅 CLEANED、`resources_held=false` 且 `cleanup_error=ESP_OK` 确认本工具资源已回收。活动阶段缓存是上次 Worker 观察，false 不证明正在执行的调用没有资源。`error` 保留原颜色执行结果，`cleanup_error` 单独报告清理结果；成功清理不会把失败颜色改判为成功，也不会撤销已经完成的历史颜色。

清理顺序为停止、回收未完成 transaction、删除 encoder/channel；任何失败保留剩余句柄。删除开始后禁止直接重用可能已断开 GPIO 的旧 channel，后续显式写入先完成旧清理。此路径锁定 ESP-IDF 5.5.4、ESP32-S3 双核 non-SMP、PM 关闭、无 DMA 和同一 CPU1 task；不是对其他 SDK/配置部分析构可重试性的承诺。

硬件依据、时序、失败时资源保留、JSON/CLI、验证命令及未完成引导流程见[固件 RGB 契约](../../../../docs/software/firmware-rgb.md)。完整目标实现前禁止阶段性烧录或设备操作。
