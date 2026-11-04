# TREX TODO

Open work on `trex/main`. Boards: **U64** = U64 MK1 (Nios II), **C64U** = U64-II / C64U (RISC-V).
Strike an item when it is done; put the commit next to it.
Specs for these items: `doc/specs/` (index in `doc/specs/README.md`).

## From the Spiffy / prkl forks (convenience for us)

Sources: `spiffycrew/Spiffy_Ultimate` (branch `spiffy-master-1.1.0`), `jusii/prkl_ultimate` (branch `prkl-1.1.0`).

| # | Item | Spiffy commit | Boards | Notes for our tree |
|---|---|---|---|---|
| S1 | ~~Hotkeys in the menu: C= X reset, C= Z reboot, C= B power cycle, C= O power off~~ done `d413a6c2` (SPEC-03), power off on C= K | `c0c7a589` | both | C= B (power cycle) does nothing on the U64: `#if U64 == 2` only, `c64_subsys.cc:389-394`; power off works there. C= O is our machine monitor (`tree_browser.cc:489`), C= R our reset: pick other keys |
| S2 | ~~C= J (swap joysticks) and C= L (log) also in the main menu; HOME / C= HOME go to / set the home directory~~ done `d413a6c2` (SPEC-03); C= HOME stays in the browser | `c0c7a589`, `5571b3de` | both | only `CommodoreMenu::handle_key`; the Commodore interface is built and selectable on the U64 too (`ultimate.cc:143`) |
| S3 | ~~Smart mount: reopening the same directory puts "Mount Disk" before "Run Disk" (disk swaps)~~ done `7442aa75` | `feb83add` | both | the patch keys off `#if COMMODORE`; ours decides at runtime with `ui_is_commodore()` (`filetype_d64.cc:77-86`) |
| S4 | ~~Copy keeps long extensions (`index.html` stays, not `.htm`)~~ done `9782d61f` | `b53fb820` | both | `FileManager::fcopy`, `filemanager.cc:1373-1379` still has the old code |
| S5 | ~~ChoiceBox: left cancels, right confirms (keys and joystick); screen restore and leak fix~~ done `6c68c564` | `72c04833`, `16a75020` | both | `ui_elements.cc:530-590`; `UserInterface::choice` deletes the box without `deinit` (`userinterface.cc:915`) |
| S6 | ~~Right arrow enters a settings item~~ done `c67d93f0` | `fd499929` | both | `config_menu.cc:283-285` |
| S7 | ~~Heap figures in System Info~~ done `f63e0a30` | `7d930f2c` | both | the patch uses `mallinfo()`, which does not see our heap (malloc is wrapped to FreeRTOS heap_4): use `xPortGetFreeHeapSize()` as `route_machine.cc:655` does |
| S8 | ~~Memory leaks in `commodore_menu`, `config_menu`, `filemanager`: check which still apply~~ done `0f96b673` | `d37c3b70` | both | mostly fixed already (`filemanager.cc:1185,1361`, `system_info.cc:153`, `config_menu.cc:22`); taking the patch would free twice. Open: `commodore_menu.cc:223,236,262` |
| S9 | ~~Flash unlock code only in the updater, not in the application~~ done `bc6019a8` | `98ac6e74` | both | the patch only sets `-DFLASH_EXTRA_PROTECTION` for the C64U. On the U64 also `filetype_esp.cc` (built there, calls `esp32.Download()/Flash()`) must be dropped or guarded |
| S10 | ~~Menu control from joystick port 2 as well: compare with our joystick injection first~~ done `0860709e` | `36b3a928` | **C64U only** | the loop is `#if U64 == 2` and toggles `U64II_KEYB_JOY`; the U64 menu reads `C64_PLD_PORTA`, swapping only on Elite boards (`u64_config.cc:1094`). Our scan has diverged (`keyboard_c64.cc:248-270`): manual merge |
| S11 | ~~Assembly64 servers from `/flash/config/server.json`~~ won't do: we already switch Assembly64 / CommoServe by client id | `d9ef6770` | both | our `assembly.cc` has diverged (host, runtime client id `assembly.h:25`): manual merge |
| S12 | ~~Branding (title, logo, colours) from `/flash/config/branding.json`~~ won't do: title and logo follow board and theme | `683ed3c5` | both | `userinterface.cc` only; our logo is per theme already (`userinterface.cc:756-783`); U64 overlay glyphs $14-$1F not checked |
| P1 | ~~Web UI: live C64 screen and data streams panel~~ done (SPEC-09) | prkl `6c00b6aa` | both | `/Flash/html`, readmem and stream routes, `data_streamer.cc` built on both |
| P2 | ~~`readmem` over REST does not pull the menu up; ARP retry fix~~ done (SPEC-09): readmem was already fixed upstream (#714), ARP retry taken | prkl `717e8034` | both | ARP fix in `data_streamer.cc:204-206` |

Board scope checked against our Makefiles and `#if U64` guards (2026-09-23). Only S10 is C64U only.

## Our own

| # | Item | Boards |
|---|---|---|
| T1 | ~~Path picker: start in the last picked path (else the browser's), a key for home / root, typing switches to a path box. All in `pick_path()` (`tree_browser.cc:821`), so copy, move, IEC and the apps get it~~ done `9a640edd` (SPEC-02) | both |
| T2 | Launcher / UI selector: decisions D1–D7 in `doc/TREX_Launcher_Architecture.html` | both |
| T3 | ~~Kickstart on C64U~~ done as an app: `apps/softboot` offers Run as Softboot on .cfw/.ue2/.u64, checks the image carries the bitstream in flash, starts its application from RAM; ABI calls `flash_read` and `machine_handover`. `tests/e2e/firmware/softboot_app_test.py` passes in the emulator; real C64U open | C64U |
| T4 | ~~Kickstart on U64~~ same app, Nios variant (bitstream truncated to 0x290000, application at load 0). Not run yet: needs a real U64 | U64 |
| T5 | `c64_dma_read_frozen` hangs the whole firmware with a real cartridge in the port (EF3 + running game): bus read with `C64_DMA_MEMONLY=1` at `$0000–$03FF` never completes; "Save C64 Memory" (`get_all_memory`, MEMONLY=0) is the working path | C64U, U64 to check |
| T6 | `C64_CRT::get_source()` is never cleared: a .c64re dump embeds a CRT file that is no longer the cartridge in the machine. Open again since 2026-10-31: `apps/c64re` is back in the release, so the snapshot writes this field again (TRX64 Ultimate [ADR-0061]) | both |
| T7 | .c64re dump of a physical cartridge (EF3): read it over the bus or reference a separate dump; the bank state cannot be read back. Open again since 2026-10-31 with `apps/c64re`; note T5 above - `dma_read_frozen` hangs with a real cartridge in the port, which is the path this app uses | both |
| T8 | USB stick drops during a long EF3 write when files are read at the same time (USB mutex held > 5 s, `-9`); the writer now avoids it by working from RAM, the cause is open | C64U |
| T9 | Guru meditation (address 0, illegal instruction) in the UI task after USB1 dropped out | C64U |
| T10 | Wasteland EF hangs after the intro on a real EF3 (runs as emulated EF); with the WL1 session | both |
| T11 | Dumper F5 only says "Use Cart Inspector from menu" | both |
| T12 | CWBI proposal `doc/cartridge_writeback_interface.md` is not committed | – |

## Board scope

- Bling Board (keyboard lighting): C64U only. `bling_board.cc` is only in the RISC-V build and runs when `BLINGBOARD_INSTALLED` is set.
- LED strip, USB host, USB keyboard and HID: both builds (`led_strip.cc`, `usb_base.cc`, `usb_hid.cc`, `keyboard_usb.cc` in both Makefiles).
