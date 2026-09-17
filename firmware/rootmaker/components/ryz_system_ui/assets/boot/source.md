# V5 boot source

2026-09-14: the first frame remains pixel-identical to this exported artwork.
During wireless restoration, a pre-created hidden footer replaces STARTING
with live English Wi-Fi status, using the audited static Noto Sans Medium
12 px glyphs in (12,212), 216×18. The rail geometry/timing and artwork are
unchanged. No additional raster asset or FreeType runtime is used.

- Figma: `FSiUaKHzDmhtoNBal977Sm`, **Ryzobee Firmware UI — Cyberpunk Menu**.
- Component: `155:3413`, **V5 / BOOT / ROOTMAKER A-FACE**, 240×240.
- Read/export: 2026-09-06, local Figma Desktop Bridge; no design mutations.
- `screen.svg` is the whole component's unmodified `SVG_STRING` export. Text
  is outlined by Figma, preserving the original fonts, spacing and baselines.
- A-face: `156:3429`, (24,12), 192×192, original Landingpage Group 57 vectors.
- RYZOBEE: `155:3422`, Teko SemiBold 600 / 32 px, 2% tracking, line height 32.
- STARTING: `155:3424`, Noto Sans Medium 500 / 10 px, 8% tracking, line height 14.
- Rail: `155:3425`, (80,234), 80×2, `#3A3A3A`.
- Traveling segment: `155:3426`, (110,234), 20×2, `#FF6A00`.

The static resource crops the whole export to (24,12), 192×216. This preserves
all artwork/text while excluding the rail; C/LVGL creates that one moving part.
The generator indexes **124 distinct RGB565 colors without quantization loss**:
41,472 index bytes + 1,024 palette bytes = **42,496 bytes**. A same-size RGB565
image would use 82,944 bytes. No duplicated color/focus variants are stored.
Current `CONFIG_LV_BIN_DECODER_RAM_LOAD=n` decodes indexed images by line.

The cloud motion/design endpoints returned Starter-plan quota errors. Local
nodes have no prototype reactions. The user specifies only the rail moves;
**800 ms per leg / 1,600 ms round trip, linear, center-first** is an explicit
implementation default, not timing claimed to have been read from Figma.

Regenerate from the repository root:

```sh
node firmware/rootmaker/tools/build_v5_boot_asset.mjs
node firmware/rootmaker/tools/check_v5_boot_pixels.mjs /path/to/boot-0000.rgb565
```
