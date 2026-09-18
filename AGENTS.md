# Ryzobee workspace instructions

## Firmware font boundary

- Native C/LVGL system UI is performance-critical and must use the checked-in
  static glyph tables/assets generated from the audited font subsets. Do not
  route C UI text through FreeType at runtime.
- FreeType is reserved for the user-facing Lua UI API and future Lua-rendered
  scenes. Changes to native UI typography must update the static font
  generator/assets and their host tests when coverage changes.
- `CONFIG_RYZ_LUA_FREETYPE` is opt-in and defaults off. C and Lua UI share
  static glyphs by default; offline font exporters/tests may still use FreeType.

## Lua platform boundary

For Lua bindings, follow `docs/software/firmware-lua-platform.md`: radio
configuration stays in C, IMU initialization is explicit per Lua job, and all
coroutines must retain the shared cancellation/deadline/heap limits.

## Visual design

When designing or changing Ryzobee interfaces, read `docs/software/ryzobee-cyberdeck-interface-style.md` first. Treat `docs/RyzoBee 配色与字体规范.md` and `assets/typography/src/theme.css` as the source of truth for color and typography.

## Repository scope

This independent repository owns firmware and simulator sources. For Studio integration or simulator packaging, read `docs/software/repository-split.md`. Commit firmware work here; Studio changes belong in the sibling `ryzobee-studio` repository.

Before implementing firmware changes, committing, pushing, or opening a pull
request, read `CONTRIBUTING.md` for the firmware version-bump checklist, fixed
emoji/type pairs, required scope, Chinese PR sections, and build gate.
Use a feature branch and a PR to update `main`; apply the same process to
automated changes. Keep build outputs, device backups, and credentials local.

When a task involves Figma, invoke the `figma-connect` skill first. Use the local Figma Desktop Bridge for status, file selection, Plugin API operations, and screenshot validation; only fall back to cloud Figma MCP when the local bridge is unavailable and Figma work is still required, and state that fallback explicitly.

## Completion notification

After a task is genuinely complete and its requested verification has finished, run `./scripts/notify-task-complete.zsh` immediately before the final response. Trigger it only for completed tasks; partial updates, questions, and blocked outcomes end without a notification.
