# RyzoBee 字体资源

Google Fonts 官方 WOFF2 资源及来源清单；使用方式和许可见 [品牌主题说明](../../../../../docs/software/brand-theme.md)。

- `manifest.json`：2026-08-28 下载的原始 URL、字重、大小与 SHA-256。
- [src/fonts.css](../../fonts.css) 保留官方字符范围；不根据当前 UI 文本二次裁字。
- [public/fonts](../../../public/fonts/) 含三份完整 OFL-1.1 许可，随生产构建分发。
- 120 个 WOFF2 共 5,533,268 字节；Teko 500–700，Noto Sans / Noto Sans SC 400–700，另含 Noto Sans 斜体。

更新来源时同时更新资源、CSS 与清单，运行 `npm test`、`npm run build`，并用开发服务的 `/tests/fonts.html` 检查真实浏览器加载。
