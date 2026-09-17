# Monitor

C-owned asynchronous SYSTEM diagnostics / UART1 RX capture with fixed live/frozen storage.

- Public API: `include/ryz_monitor.h`; no raw handles or device operations in readers.
- Core/ESP: one lazy CPU1 worker, configuration commit/rollback, explicit teardown confirmation.
- Stream: 32 × 96B records per live/frozen view, generation-based bounded reads.
- Sources: filtered Log V1 tap and UART1 RX-only; shared tool pin broker, no UART0 ownership change.
- Consumer: Workbench `ryz-monitor/1` RPC and `tools/board_lua.py monitor`; LVGL pages pending.

Current contracts, commands, evidence and limitations: [firmware-monitor.md](../../../../docs/software/firmware-monitor.md).
