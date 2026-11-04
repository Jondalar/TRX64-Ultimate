# SPEC-06 Heap figures, leaks, flash protection

| | |
|---|---|
| Status | Done (branch `feature/spiffy`) |
| Boards | U64, C64U |
| TODO | S7, S8, S9 |
| Source | Spiffy `7d930f2c`, `d37c3b70`, `98ac6e74` |

## Requirements

### Heap in System Info

- **R1** System Info shows heap total, free and low-water mark, taken from
  `xPortGetFreeHeapSize()` / `xPortGetMinimumEverFreeHeapSize()` /
  `configTOTAL_HEAP_SIZE` like `GET /v1/machine:heap` (`route_machine.cc:653-658`).
  Not `mallinfo()`: malloc is wrapped onto the FreeRTOS heap on both boards, so
  newlib's figures do not see it.

### Leaks

- **R2** Fix the leaks still open in `commodore_menu.cc:223, 236, 262`. Do not take
  the other hunks of `d37c3b70`: those leaks are fixed already
  (`filemanager.cc:1185,1361`, `system_info.cc:153`, `config_menu.cc:22`) and the
  patch would free twice.

### Flash protection

- **R3** The application binary contains no code that lifts the flash write
  protection or programs the ESP32: `-DFLASH_EXTRA_PROTECTION` for both
  application targets; the updaters keep it.
- **R4** On the U64 the application also builds `filetype_esp.cc`, which calls
  `esp32.Download()` / `Flash()` (`filetype_esp.cc:112,146`): drop it from the U64
  application or guard it, so ESP images are handled by the updater only.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | System Info heap figures equal `GET /v1/machine:heap` within one allocation | emulator |
| A2 | Opening and leaving the Commodore main menu 50× leaves heap free unchanged | emulator, commodore mode |
| A3 | `nm` of both application ELFs has no `protect_disable` and no ESP flash routine | build check |
| A4 | Firmware update through the updater still works | both boards |
