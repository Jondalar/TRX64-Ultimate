# 9. Network

Wired Ethernet or Wi-Fi ([4. Settings](04-settings.md#network)). Each service can be switched off in
**Network Settings**; **Network Password** protects them.

| Service | Port | Use |
|---|---|---|
| REST API | HTTP 80 | Remote control, below |
| FTP | 21 (passive 51000-61000) | Files on all drives |
| Telnet | 23 | The menu in a terminal (Ultimate look) |
| DMA service | TCP 64 | Load and run programs, keyboard, reset: the protocol of the Ultimate's tools |
| Discovery | UDP 64 | Answers with product, versions and host name; on our bitstream also a `trx64` block (core, capabilities, board, and the monitor's RPC port) |
| Monitor | TCP 4312 | The monitor for C64RE ([6.](06-monitor.md)); served from the start, one connection at a time |
| Streams | UDP, multicast | Video and audio of the C64 ([3.](03-action-menu.md#streams)) |

## REST API

All routes start with `/v1`. `GET /v1/help` lists them.

| Route | Methods | Effect |
|---|---|---|
| `/version`, `/info` | GET | Versions; `/info` has a `trx64` block on our bitstream |
| `/configs`, `/configs/{category}`, `/configs/{category}/{item}` | GET, PUT, POST | Read and change settings; POST uploads a `.cfg` |
| `/configs:save_to_flash`, `:load_from_flash`, `:reset_to_default` | PUT | As in the Configuration menu |
| `/machine:reset`, `:reboot`, `:pause`, `:resume`, `:poweroff`, `:menu_button` | PUT | Machine control |
| `/machine:readmem`, `:writemem` | GET, PUT/POST | C64 memory |
| `/machine:menu_screen` | GET | The menu's text while it is open |
| `/machine:input` | GET, POST | Keyboard and joystick input |
| `/machine:debugreg` | GET, PUT | The `$D7FF` debug register |
| `/runners:sidplay`, `:load_prg`, `:run_prg`, `:run_crt`, `:modplay` | PUT (file on the machine), POST (upload) | Play or run a file |
| `/drives`, `/drives/{drive}:mount`, `:remove`, `:reset`, `:on`, `:off`, `:unlink`, `:load_rom`, `:set_mode` | GET, PUT, POST | Drive A |
| `/files/{path}:info`, `:create_d64`, `:create_d71`, `:create_d81`, `:create_dnp` | GET, PUT | Files and blank images |
| `/streams/{video\|audio}:start`, `:stop` | PUT | Streams |
| `/apps`, `/apps/{id}` | GET | Installed apps |
| `/apps/{id}:run`, `/apps:run_file` | PUT | Run an app (only actions its manifest allows; of the three here only `softboot` `run`) |
| `/timewarp:status`, `:frame` | GET | Timewarp ring state |
| `/timewarp:start`, `:stop`, `:rewind`, `:scrub`, `:scrubend` | PUT | Timewarp control |

`/machine:measure` exists but answers that the bitstream cannot measure the bus.
