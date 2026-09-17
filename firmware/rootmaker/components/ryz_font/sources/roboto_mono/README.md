# Roboto Mono source provenance

`RobotoMono[wght].ttf` is the unmodified normal-style variable font, version
3.001, from the official Google Fonts repository. It is not a system font.

- Repository: https://github.com/google/fonts
- Fixed catalog revision: `aa24661a5ba24c8f8a550a16fb13198719fc2c23`
- Path: `ofl/robotomono/RobotoMono[wght].ttf`
- Exact download: https://raw.githubusercontent.com/google/fonts/aa24661a5ba24c8f8a550a16fb13198719fc2c23/ofl/robotomono/RobotoMono%5Bwght%5D.ttf
- Bytes: `183700`
- SHA-256: `66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b`
- Catalog metadata: https://github.com/google/fonts/blob/aa24661a5ba24c8f8a550a16fb13198719fc2c23/ofl/robotomono/METADATA.pb
- Metadata's upstream: `https://github.com/googlefonts/RobotoMono`, revision
  `111eb14e367888c9374da4da0b018e72cf8ac46d`, `sources/config.yaml`.

The catalog metadata declares `OFL`, the source font's name table declares SIL
OFL 1.1, and the matching original license (including the 2015 project-author
copyright) is archived at `../../LICENSES/RobotoMono-OFL.txt`. No reserved font
name is specified after that copyright statement.

`tools/build_font_assets.py` freezes the `wght` axis at 400, 500 and 600 and subsets
each instance to printable ASCII U+0020–U+007E. Firmware embeds only those three
static subsets, not this variable source. Generation is offline and pinned to
the existing fontTools 4.60.2 toolchain; output identities live in
`../../assets/manifest.json`.
