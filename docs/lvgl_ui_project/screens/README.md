# Screen map

The prototype separates stable top-level destinations from short task flows.
Primary screens keep the same shell and left navigation; task and confirmation
screens retain context but put the pending decision in the foreground.

| Screen | Purpose | Prototype routes |
| --- | --- | --- |
| `home` | current C64 plus compact drive, Timewarp and network state | Library, C64, contextual actions |
| `quick_actions` | small, contextual action set | Timewarp, machine, full action catalogue |
| `action_catalog` | complete runtime task categories | machine, drives, peripherals, files, settings, apps |
| `library` | unified recent/SD/USB/network/favorites browser | selected-item actions |
| `library_search` | local quick-seek and Assembly64 search | selected-item actions |
| `library_actions` | context-sensitive actions for one media item | Library, safe eject |
| `library_manage` | selection, copy/move/rename/delete and creation | path picker and progress flows |
| `media_eject` | clean-media eject confirmation | Library actions, Home |
| `c64` | mounted media and essential machine controls | Library actions, writeback warning, Timewarp |
| `c64_machine` | machine, reset, power and save-state actions | action catalogue |
| `c64_drives` | Drive A/B media and operational actions | Library actions, drive settings |
| `c64_peripherals` | SoftIEC, partitions, UltiCopy, tape, printer, modem | peripheral settings |
| `writeback_warning` | three-way dirty-cartridge decision | cancel, discard/eject, save/eject |
| `timewarp` | scrub, preview and explicit resume; markers via F7 | primary navigation |
| `timewarp_actions` | markers, capture, dump, ringdump and load | Timewarp |
| `tools` | diagnostics and advanced utilities | Monitor, System |
| `monitor` | terminal-backed M2 monitor surface | Tools |
| `tools_diagnostics` | trace, viewers, logs, EDID and system information | System |
| `tools_apps` | installed applications and manifest actions | Monitor and app flows |
| `tools_streams` | VIC, audio, debug and data streams | stream settings |
| `settings` | flat configuration category list and saved profile state | TRX64, Video, Network, System |
| `settings_trx64` | M2 behavior: freeze, writeback, ring, splash, turbo | Settings |
| `settings_video` | overlay appearance with live preview | Settings |
| `settings_audio` | mixer, SID and installed SID-device pages | dynamic settings page |
| `settings_controllers` | joystick, paddle, mouse and USB input | dynamic settings page |
| `settings_network` | Wi-Fi and service status | Settings |
| `settings_peripherals` | Drive A/B, SoftIEC, tape, printer, modem | dynamic settings page |
| `settings_advanced` | UI, memory, cartridge, turbo, power, LEDs, hardware | dynamic settings page, System |
| `settings_page` | reusable runtime page for every registered item kind | owning settings category |
| `system` | versions, health, log and maintenance | Settings |

All routes use `screen_create_event` only to make the UX clickable in LVGL Pro.
They do not imply firmware execution. `settings_page`, file action sheets, and
the complete action catalogue are runtime templates rather than handwritten
copies of the firmware registries. In step 2, the adapters in
`../../specs/T24-ui-wiring.md` populate them and dispatch actual commands while
the screen hierarchy stays the same. See [`../UI-COVERAGE.md`](../UI-COVERAGE.md).

Focus order follows document order. The footer copies the C64 keycaps: `↕` and
`↔` move focus, `↵` activates, and `←` goes back or exits at the root.

The TRX64 mapping is fixed and independent of Gideon's Ultimate/Commodore theme:

| Key | Destination |
| --- | --- |
| F1 | Home |
| F3 | Library |
| F5 | C64 |
| F7 | Actions |
| F2 | Timewarp |
| F4 | Tools |
| F6 | Settings |
| F8 | System |

`R/S` (`RUN/STOP`) closes the UI from any screen. The physical Freeze/Menu button
toggles the UI open and closed. The numbered tabs and stable shortcut sequences
are specified in [`../NAVIGATION.md`](../NAVIGATION.md).

LVGL Pro screen syntax: https://lvgl.io/docs/pro/syntax/screens
