# Ryzobee embedded fonts

FreeType is optional and **disabled by default** (`CONFIG_RYZ_LUA_FREETYPE=n`).
The default firmware contains neither its engine nor the seven subset TTFs;
C and Lua UI share the checked-in static A8 glyphs. All current Lua font names,
including the factory Monitor palette, retain identical face/size/metrics.

When explicitly enabled, this module owns FreeType and seven immutable subset
faces for Lua UI only. C system UI always stays static. Offline Host font
export/equivalence tests keep FreeType independently of the product backend.
Neither backend adds Unicode to the separate legacy 5x7 renderer.

- Body: Noto Sans Regular 400.
- Display: Teko SemiBold 600.
- Body SemiBold: Noto Sans SemiBold 600 (appended face ID; existing IDs unchanged).
- Body Medium: Noto Sans Medium 500.
- Mono / Mono Medium: Roboto Mono Regular 400 / Medium 500.
- Mono SemiBold: Roboto Mono SemiBold 600.
- Coverage: printable ASCII `U+0020-U+007E` for all faces; Teko additionally
  includes its native `U+00B0` degree sign, while Noto Sans Medium and Roboto
  Mono Medium also include `U+00B7` (middle dot) used by the V5 status copy.
  Celsius uses `°` + `C`, not the unsupported single code point `U+2103` (`℃`).
- Input sources: the checked-in Noto Sans/Teko WOFF2 files used by Studio and
  the pinned official Roboto Mono variable TTF in `sources/roboto_mono/`.
- Firmware format: uncompressed, static-instance TTF because the ESP-IDF
  FreeType port disables WOFF2/Brotli support.

The public seam returns copied 8-bit glyph alpha maps; callers never retain a
FreeType face or glyph pointer. The module serializes access because a face's
glyph slot is overwritten by the next load.

The C pixel-height range is 6–64. This supports the original V5 information
font sizes without adding or changing Lua's public font names. Tiny type is
not a general UI readability recommendation. The real FreeType/LVGL Host
matrix covers all 95 ASCII glyphs for Noto Sans 400/500/600 and Roboto Mono
400/500/600 at 6/7px, plus the larger V5 sizes. Teko is tested from 8px up, including
V5's 16/17/20/22px: its hinted `(` at 7px extends 1px above the independently
rounded face line box, so rasterization support is not a no-clipping promise
for that unused display-font combination. No device readability is inferred.

The seven embedded subsets total 75,968 bytes. Restoring Teko's native degree
adds 104 bytes; the two Medium faces add the V5 middle dot while the other
five TTF files and their original ASCII glyphs are unchanged. The full
183,700-byte Roboto Mono source is used only
for reproducible generation and is not embedded in firmware.

`ryz_font_supports_codepoint` queries the fixed per-face coverage without init.
`ryz_font_rasterize` renders supported subset code points and rejects missing
glyphs instead of drawing `.notdef`. The compatibility function
`ryz_font_rasterize_ascii` still rejects every non-ASCII code point; Lua's
existing ASCII scene policy is not expanded by this change.

`ryz_font_get_metrics` returns copied pixel ascender/descender/line-height from
the same locked face; a requested pixel size is not the line height. The new
`ryz_lvgl_brand_font` adapter uses these metrics and the glyph rasterizer for
real LVGL A8 rendering with a bounded PSRAM cache. Lua scenes now use Noto Sans
400 at 16px and Noto Sans 600 at 24px instead of Montserrat. Native V5 pages use
their original face/size combinations through static glyph tables. See the
[adapter contract and real Host evidence](../../../../docs/software/firmware-lvgl-brand-font.md).

Regenerate and verify assets from the project directory:

```sh
python3 -m venv .font-venv
.font-venv/bin/python -m pip install -r tools/requirements-fonts.txt
.font-venv/bin/python tools/build_font_assets.py
.font-venv/bin/python -m unittest tests/test_font_assets.py
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  components/ryz_font/test_host/test_font_bitmap.py
```

Font provenance and hashes are recorded in `assets/manifest.json`. Distribution
notices and the complete SIL OFL 1.1 and FreeType License texts are kept with
the component in `THIRD_PARTY_NOTICES.md` and `LICENSES/`; license metadata is
also preserved in the generated font name tables.

The separate [Unicode asset preparation](../../../../docs/software/firmware-unicode-font-assets.md)
tool decodes the vendored Noto Sans / Noto Sans SC variable shards into an
explicit offline directory. Those artifacts are not embedded or loaded by
this component, and do not make current SSID/metadata labels Unicode-capable.
