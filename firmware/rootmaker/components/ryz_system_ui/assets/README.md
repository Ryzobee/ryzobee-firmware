# Native UI asset sources

V0.10.2 renders the system UI through native LVGL objects, static glyph tables
and generated icon masks. `v5/` contains the monochrome SVG inputs used by
`tools/build_v5_assets.mjs`; `boot/` contains the boot artwork inputs.
Generated C tables are checked in so ordinary firmware builds need no image
conversion toolchain.

Old V3/V5 full-screen RGB565 preview snapshots are not part of this release.
The configured native UI build does not embed or load those files. Historical
non-native preview paths are not a supported alternative build configuration.
