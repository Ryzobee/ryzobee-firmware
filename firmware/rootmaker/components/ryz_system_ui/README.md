# Ryzobee system UI

This component renders the product shell on the fixed 240 x 240 RGB565 display.
It owns navigation state and touch gestures, but it does not own Wi-Fi, storage,
Lua execution, or the display task. `ryz_workbench` supplies an immutable system
snapshot. The single `wb_ui` Owner performs rendering and physical input;
the independent `wb_lua` Worker sends synchronous I/O requests to that Owner.

## Screen contract

- Top bar: 32 px.
- Content: 164 px.
- Bottom navigation: 44 px, with 80 px Home, Apps, and Set tabs.
- Home: dense status HUD, WLAN history shortcut, and a dynamic `STORAGE`
  meter for the mounted `scripts` SPIFFS partition. This is filesystem usage
  reported by the C storage layer, not Lua VM memory or whole-chip flash usage.
- Apps: four 104 x 64 slots. `LUA TOOL` is intentionally rendered disabled
  until an application-launch action is connected.
- Settings: Network, disabled Display, and Reprovision rows.
- AP Setup: no bottom navigation; a 132 px QR region and an invisible 44 x 44
  Back hit target leave the maximum Version 10 QR unobstructed.

The source design uses Teko for display headings and Noto Sans/Noto Sans SC for
information. The current device renderer has only the bounded 5 x 7 bitmap font,
so firmware uses uppercase scale/spacing as the embedded fallback while keeping
the same layout and color roles. Brand orange is RGB565 `0xFB40` (`#FF6A00`).

## QR and actions

The AP password is accepted only from the trusted local provisioning snapshot.
It is escaped into a standard `WIFI:T:WPA;S:...;P:...;;` payload, passed to the
pinned Espressif QR encoder, and never logged or exposed through the UI API.
Rendering reserves at least four quiet modules on every side.

Espressif qrcode 0.2.0 logs its complete input at INFO, which would expose the
SoftAP password. This component compiles that dependency with local logging
disabled and requires `CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=y` as a second runtime
guard; configurations that disable dynamic log-level control are rejected at
compile time.

`ryz_system_ui_take_action()` currently emits only `REPROVISION`. Workbench uses
`ryz_system_ui_process_sample()` for both successful and failed physical reads,
and consumes its action only on success. Input requires a presented frame and
physical release after reset, navigation or error. The navigation core provides
eight history entries, one modal and generation-bound tokens; actual LVGL
modal/scroll/long-press integration is pending. Interface and verification are
recorded in [navigation and system input](../../../../docs/software/firmware-ui-navigation.md).

## Verification

```sh
python3 tests/test_system_ui.py -v
python3 tests/test_ui_navigation.py -v
```

The host test compiles the production state machine with display and QR spies
under AddressSanitizer and UndefinedBehaviorSanitizer. A real LCD and phone are
still required to validate optical scan distance, brightness, glare, touch
alignment, and captive-portal behavior.
