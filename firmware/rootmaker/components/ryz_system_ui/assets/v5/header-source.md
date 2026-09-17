# V5 header icon source (2026-09-06)

Exported unchanged with `exportAsync({format: 'SVG_STRING'})` through the local
Figma Desktop Bridge. Source file: `FSiUaKHzDmhtoNBal977Sm`, Firmware Screens.
Live components: [TOP](https://www.figma.com/design/FSiUaKHzDmhtoNBal977Sm?node-id=98-3989)
and [CHILD](https://www.figma.com/design/FSiUaKHzDmhtoNBal977Sm?node-id=98-4007).
Both use the same status group at `(136,8)`, size `96×16`.
HOME `98:4026` and AP SETUP `98:4080` instances were checked against them.

| SVG | Source node | Export size | C screen position |
| --- | --- | --- | --- |
| wifi_status.svg | 98:3998 | 16×16 | 136,8 |
| ble_status.svg | 106:5141 | 16×16 | 155,8 |
| battery_outline.svg | 98:4003 | 18×12 (includes 2px stroke) | 174,10 |
| battery_terminal.svg | 98:4004 | 2×5 | 193,14 |

Figma's Bluetooth x=155.222 and battery rendered bounds x=174.444 / x=193.111,
y=13.667 are rounded to integer panel pixels. No SVG path edits, scaling or
thresholding; `tools/build_v5_assets.mjs` generates the existing LVGL A8 masks.
The 16px-high battery frame includes transparent padding; its outline and
terminal are exported separately to avoid embedding the example 75% fill.

Only header assets and their positions changed. The time stays at `(199,8)` in
the existing 33×16 slot. Wi-Fi still follows connection state; unavailable BLE
and unknown power remain disabled gray. Body icons, back arrow, fonts, brand,
touch areas and service logic are unchanged. This is not BLE or battery support.
