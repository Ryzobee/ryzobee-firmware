<p align="center">
  <a href="https://wiki.ryzobee.com/zh/home"><img src="logo/ryzobee-logo.png" width="80" alt="RyzoBee logo"></a>
</p>

<h1 align="center">RYZOBEE FIRMWARE</h1>
<p align="center"><strong>Your device. Your Lua. Your tools.</strong></p>
<p align="center">Boot · Connect · Run · Create</p>
<p align="center"><a href="README.md">English</a> · <a href="README.zh-CN.md">简体中文</a></p>
<p align="center"><a href="#build-and-deploy">Build &amp; flash</a> · <a href="docs/software/user-guide-v0.10.1/README.md">User guide</a> · <a href="#desktop-ui-simulator">Simulator</a> · <a href="docs/software/firmware-lua-platform.md">Lua API</a> · <a href="CONTRIBUTING.md">Contribute</a></p>
<p align="center">V0.10.3 · ESP32-S3 · C + Lua + LVGL · 240×240 touch UI</p>
<p align="center"><a href="https://github.com/Ryzobee/ryzobee-firmware/actions/workflows/firmware.yml"><img src="https://github.com/Ryzobee/ryzobee-firmware/actions/workflows/firmware.yml/badge.svg?branch=main" alt="Firmware CI on main"></a> · <a href="#license-status">License status</a></p>

This repository contains the ESP32-S3 system firmware, native 240×240 LVGL UI, bounded Lua application runtime, factory tools, and same-source desktop simulator. C owns the hardware and system services; users build interfaces and peripheral workflows in Lua without adding a dedicated C component for each tool. RyzoBee Studio is maintained separately and is not needed to build this firmware.

<p align="center">
  <img src="docs/software/user-guide-v0.10.1/assets/home.png" width="240" alt="HOME with simulated network, load and PSRAM values">
  <img src="docs/software/user-guide-v0.10.1/assets/monitor-live.png" width="240" alt="Factory Lua Monitor showing explicitly simulated log input">
</p>

<p align="center"><em>HOME and the factory Lua Monitor, captured from the same-source LVGL/SDL simulator at 240×240. Values are demo data, not device measurements.</em></p>

## Features

- **On-device launcher:** HOME, APPS, SETTING, script metadata, deletion, and `boot.lua` autostart. Hold the screen's run button for **2 seconds**; hold physical BOOT for about **5 seconds** to stop a running script and return home.
- **Responsive native UI:** static glyphs/icons, partial redraws, gesture cancellation, and shared LVGL display ownership. C UI never uses runtime FreeType.
- **System-managed connectivity:** password-protected 2.4 GHz provisioning AP, mobile/desktop configuration page, saved-network recovery, secure BLE pairing, and NTP. Manually disabled radios remain disabled after reboot.
- **Lua DIY APIs:** bounded UI/display/touch and peripheral APIs, explicit IMU initialization, cooperative coroutines, per-job limits, and cancellation cleanup. Wi-Fi/BLE configuration and credentials remain in C; Lua queries connection state.
- **Persistent app files (V0.10.2):** generic [`fs.read/write/remove/list/info`](docs/software/firmware-lua-filesystem.md) for app-scoped text/binary data, with quotas, integrity checks and failure recovery; no tool-specific native save logic required.
- **Editable factory tools:** system/UART log monitor, I²C address scanner, and guided board checks are ordinary Lua scripts in [`firmware/rootmaker/fs`](firmware/rootmaker/fs).
- **Persistent display settings:** ST7789 hardware rotation, brightness preview/save, and idle backlight management.
- **Maintenance foundations:** USB script transfer, read-only diagnostics on the existing protected AP, version information, and HTTPS OTA components with A/B application partitions.
- **Same-source simulator:** actual C pages and factory Lua layouts run through LVGL/SDL with explicit virtual services and host checks.

See the illustrated [V0.10.1 user guide (Chinese)](docs/software/user-guide-v0.10.1/README.md) for screen-by-screen operation.

## Hardware and dependencies

The configuration targets the verified RootMaker sample: **ESP32-S3, 16 MiB Flash, 2 MiB PSRAM, and a 240×240 ST7789 touch display**. Confirm your board revision and electrical connections before flashing; this is not a generic configuration for every ESP32-S3 board.

| Dependency | Version / policy |
| --- | --- |
| ESP-IDF | **5.5.4**, ESP32-S3 target |
| LVGL | **9.5.0** |
| Lua component | **georgik/lua 5.5.0~7**, locked development snapshot |
| FreeType component | **2.14.3~1**; Lua runtime support opt-in, default off |
| QR encoder | **espressif/qrcode 0.2.0** |
| Configuration | [sdkconfig.defaults](firmware/rootmaker/sdkconfig.defaults), [dependencies.lock](firmware/rootmaker/dependencies.lock), [partitions.csv](firmware/rootmaker/partitions.csv) |

Do not assume desktop Lua numeric behavior or unrestricted LVGL access. The locked Lua component is a development snapshot, not a claim of production-stable upstream Lua.

## Build and deploy

### 1. Install ESP-IDF 5.5.4

On Linux/macOS, install OS prerequisites from the [official ESP-IDF setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/linux-macos-setup.html). Choose a development directory without spaces:

```sh
git clone --branch v5.5.4 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5.4
cd esp-idf-v5.5.4
./install.sh esp32s3
. ./export.sh
cd ..
```

Activate that installation's `export.sh` in each new terminal. Windows users should use Espressif's installation tools and a **5.5.4** ESP-IDF terminal; the shell installation above targets Linux/macOS.

### 2. Clone and compile

With the IDF environment active, from your development directory:

```sh
git clone https://github.com/Ryzobee/ryzobee-firmware.git
cd ryzobee-firmware/firmware/rootmaker
idf.py --version
idf.py build
```

The defaults select `esp32s3`. The first build downloads locked components and creates application/factory-script images in `build/`. No Studio, Node.js, or font regeneration is required. Use `idf.py menuconfig` only for intentional changes and review reproducible settings in `sdkconfig.defaults`.

Do not commit `build/`, `build-host/`, `managed_components/`, generated `sdkconfig`, logs, or credentials. Audited static font tables and asset inputs are tracked sources, not disposable build intermediates.

### 3. Flash and monitor

**A full project flash overwrites the scripts partition**, including user scripts, `boot.lua`, and persistent Lua app data. Save data you need before proceeding. Standard project flashing does **not** erase NVS, so saved Wi-Fi/BLE preferences, bindings, and display settings normally remain. It is not a factory reset; do not add an erase command just to update firmware.

Connect a data-capable USB cable. Close monitors, Studio, or browser serial sessions using the same port. Replace `PORT` with the actual device, such as `/dev/cu.…`, `/dev/ttyACM0`, or `COM3`:

```sh
# Run from firmware/rootmaker with ESP-IDF active.
idf.py -p PORT flash monitor
```

This writes the project's bootloader, partition table, OTA metadata, application, and scripts image. Exit the monitor with **Ctrl+]**. After startup, check **SETTING → VERSION** for **V0.10.3**.

The supported board uses DTR/RTS automatic download/reset. If connection fails, check the port, cable, and port owner, and keep the error output. One failure does not prove automatic download is unsupported. Only if manual download is necessary, follow the board's BOOT/reset procedure and retry the same scoped flash. Holding BOOT while resetting is different from the running firmware's 5-second exit gesture.

## First use and Lua development

1. Power on normally. Without `boot.lua`, initialization ends at HOME; a configured boot script runs directly after startup finishes.
2. Open **SETTING → WI-FI → START WI-FI SETUP**. Scan the live device QR with the **phone's built-in system scanner or camera**; most phones support this in the system camera. Do not substitute WeChat, Alipay, or third-party scanners. The QR joins the device AP; then open the displayed address in a browser, usually `http://192.168.4.1`.
3. Select a script in APPS, open APP INFO, and hold **HOLD 2S TO RUN**. BOOT held for about 5 seconds requests cancellation and cleanup, not a hard-real-time reset.
4. Upload Lua over USB without rebuilding. From the repository root, with IDF Python active and the serial port free:

```sh
python firmware/rootmaker/tools/board_lua.py --port PORT \
  put firmware/rootmaker/scripts/ui_demo.lua --name ui_demo.lua
```

This writes or replaces `ui_demo.lua`; it does not run it or enable autostart. Start it from APPS. Read the [Lua platform contract](docs/software/firmware-lua-platform.md), [peripheral API](docs/software/firmware-lua-peripherals.md), and factory examples before driving hardware.

V0.10.3 includes [`boot.poll()`](docs/software/firmware-lua-boot-events.md)
for click, double-click and 3-second long-press events. The C-owned 5-second exit remains;
a long press used to exit can also deliver the earlier 3-second event. See the bounded
[Lua example](firmware/rootmaker/scripts/boot_events_demo.lua). Earlier V0.10.2 builds are
not guaranteed to contain this interface; a simulator also needs an input backend.

## Desktop UI simulator

After a successful target build, install SDL2, CMake, Ninja, and a Clang/GCC host toolchain. From the repository root:

```sh
sh firmware/rootmaker/tools/build_ui_simulator.sh "$PWD/firmware/rootmaker/build"
# macOS:
open firmware/rootmaker/build-host/simulator/v5_simulator.app
```

The helper builds the same-source simulator and runs host checks. Quit old simulator processes before reopening; rebuilding does not hot-reload them. The current desktop workflow has been used on macOS; other hosts may need adaptation. See [simulator notes](firmware/rootmaker/host/simulator/README.md).

**Simulation is not hardware acceptance.** Network state, sensors, files, and QR credentials are fixtures. It cannot prove real BLE interoperability, touch latency, LCD signal integrity/colors, peripheral wiring, or scan success. Never scan screenshot QR codes to configure a real device.

## Current limitations

- **BLE is development HID/Consumer Control:** seven media controls, not a keyboard, mouse, or arbitrary Lua-defined GATT server. No production VID/PID is assigned; full HOGP compliance and discovery in every iPhone system version are not claimed. Other Lua apps are not restricted to HID use.
- **Online OTA is not enabled out of the box:** components exist, but `CONFIG_RYZ_OTA_URL` is empty and no default public update feed is provided.
- **FreeType defaults off** (`CONFIG_RYZ_LUA_FREETYPE=n`). C UI always uses static assets; optional Lua FreeType does not change that boundary. Offline exporters/tests may use FreeType independently.
- **Resources are bounded:** one user Lua job, 256 KiB Lua heap budget, up to 16 KiB per script, and a 1 MiB scripts partition. Free internal RAM and PSRAM vary with active services; chip capacity is not the per-script budget.
- Battery percentage, FILE SERVER, and some maintenance actions are unavailable. NTP currently uses UTC. The native UI uses English and an audited static character set, not unrestricted multilingual rendering.

## Repository map

| Path | Purpose |
| --- | --- |
| [firmware/rootmaker](firmware/rootmaker) | ESP-IDF project, C components, board configuration, tests, USB tools |
| [firmware/rootmaker/fs](firmware/rootmaker/fs) | Factory Lua packaged into the scripts image |
| [firmware/rootmaker/host](firmware/rootmaker/host) | Shared-runtime host adapters, pixel checks, SDL simulator |
| [assets/typography](assets/typography) | Audited font sources and third-party font notices |
| [docs/software](docs/software) | Current user guide, API and architecture documents |
| [.github](.github) | Build workflow and pull-request template |

## Contributing and merge policy

Read [CONTRIBUTING.md](CONTRIBUTING.md). Commits and PR titles use fixed emoji/type pairs and a required scope:

```text
🐛 fix(ui): 修复应用详情返回按钮的触摸判定
```

Update `main` **only through PRs**, including administrator changes. Both **Firmware version** and **ESP32-S3 build** must pass before merging. Every PR must increase `PROJECT_VER`, including documentation-only changes; CI also checks the built binary and native VERSION pages. CI gates **merging**, not opening a PR. Use the Chinese-first template for additions, removals, modifications, precautions, version changes, and verification. Build success does not imply device acceptance.

## License status

The maintainers have not selected a project-wide license. A public repository alone is not an open-source license grant. Third-party dependencies, fonts, and other assets retain their respective licenses and notices; do not remove them or assume one project license covers every file.
