# Cartridge Auto-Save

Status: builds for U64, U64-II and U2+, carried end to end through the ue2emu emulator (§7), and
the EasyFlash case run on a U64 (§6).
The Auto task (§4.8): builds for U64 and U64-II, host test green, emulator run (§8); not yet built
for U2+ and not yet run on hardware.

## 1. Purpose

A game that saves to its cartridge (EasyFlash via EAPI, GMod2 via EEPROM) only changes the
image in memory. The CRT file on disk keeps the old state until the user runs *Save Cartridge*
by hand, types a file name, and confirms the overwrite. Forgetting that loses the save.

This feature writes a changed cartridge back to the file it was loaded from. With *Ask* that
happens when the menu opens. With *Auto* it also happens while the game runs, a few seconds after
a write the firmware sees, the way a disk image follows its drive (§4.8).

## 2. Baseline (v3.15)

| Item | Where | Behaviour |
|---|---|---|
| Save Cartridge | `c64_subsys.cc:358-373` | Asks for a name, writes into the browser's current folder. |
| Load | `c64_crt.cc:724-747` | `load_crt()` keeps the file name, drops the path. The source is not recoverable. |
| Save | `c64_crt.cc:749-812` | `save_crt()` swapped the EAPI in cartridge memory around the write (baseline; the EAPI substitution is gone, see D1). |
| EEPROM dirty | `c64.cc:1665-1671` | Reading the flag clears it in hardware. |
| Cartridge image | `c64.h:429-437` | 4 MiB at `0x03C00000`, plain CPU access, arbitrated against the cart port. |

No upstream change in this area since v3.15.

## 3. Defects found in the baseline

**D1. Save from the overlay modifies a running cartridge.** (baseline; fixed by removing the EAPI substitution)
The overlay menu does not stop the C64 (`overlay.h:91-95`); REST does not either. The baseline
`save_crt()` swapped 768 bytes of EAPI code in live cartridge memory, and each save leaked 768
bytes. The firmware no longer substitutes an EAPI at all: the cartridge keeps its own, and the
bitstream's cartridge layer answers its flash commands.

**D2. System Info discards the GMod2 save state (bug-091).**
System Info (F4) calls `get_eeprom_dirty()` (`system_info.cc:88`), which clears the flag. A later
*Save Cartridge* sees a clean EEPROM (`c64_crt.cc:774`), skips `get_eeprom_data()`, and writes the
EEPROM contents from load time.

**D3. The source of a cartridge is not kept.** Required for any write-back; also upstream issue #52.

## 4. Design

### 4.1 Source tracking (D3)

`C64_CRT` keeps one full pathname, `source`:

- set by `load_crt()` on success: `path + "/" + filename`, or `filename` alone when `path` is
  empty (REST passes a full pathname that way, `route_runners.cc:183`);
- replaced by a successful *Save Cartridge*, so the cartridge follows the file it was last saved
  to (save-as semantics);
- cleared by `initialize()`, i.e. on every load, on failure, and by `clear_crt()`.

### 4.2 Save without touching memory (D1)

`save_crt()` writes the chunks as they are in memory. There is no EAPI substitution on load or on
save (`patch_easyflash_eapi()` and `original_eapi` are removed), so the file carries the
cartridge's own EAPI by construction.

*Save Cartridge* runs inside `begin_stopped_session()` / `end_stopped_session()`. From the C64
menu the machine is already frozen and this is a no-op. From the overlay it stops the C64 for the
duration of the write, so the file is one consistent image.

### 4.3 EEPROM dirty latch (D2)

A file-static flag in `c64.cc` holds the dirty state until the data is actually taken:

| Call | Hardware flag | Latch |
|---|---|---|
| `get_eeprom_dirty()` | cleared only if it read as set | set if hardware was set; returned |
| `get_eeprom_data()` | cleared before the copy | cleared |
| `set_eeprom_data()` | cleared after the copy | cleared |

Reading the flag becomes side-effect free for callers. The hardware flag is cleared only when it
was seen set, so a C64 write between read and clear is no longer lost. `get_eeprom_data()` clears
before copying: a write during the copy sets the flag again and is caught next time.

### 4.4 Change detection

32-bit hash over every chunk's data, exactly the bytes `save_crt()` would write:

    h = 0x811C9DC5
    for each 32-bit word w:  h = rotl(h ^ w, 5) + 0x9E3779B9

Each step is a bijection in `h` for a fixed `w`, so two images that differ in a single word
always hash differently. It uses no multiply because the firmware is not built with one:
`software/nios_appl_bsp/public.mk` passes `-mno-hw-mul` and `-mno-hw-mulx` to every U64 object, so
a multiply is a library call however the FPGA is fitted.

Before hashing, a set EEPROM latch is resolved by copying the EEPROM into its chunk buffer, so the
EEPROM is covered by the same hash.

Only what the C64 can change is hashed. In `all_carts_v5.vhd` EasyFlash is the one type with
`allow_write` set while `addr_map` is ROM (`ef_write`, Ultimax, `$8000`/`$E000`); every other
`allow_write` targets the separate cart RAM or GeoRAM, outside the chunks this reads. GMod2 changes
through its EEPROM alone, so its ROM is skipped and only the 2 KiB EEPROM chunk is read. Every
other type is skipped entirely, which also keeps an image changed by the firmware rather than by
the C64 from being reported as a save. The duration is logged.

The Protovision Megabyter carries flash that a game programs on the real cartridge, but the
Ultimate emulates its bank register alone (`all_carts_v5.vhd:369`), so there is nothing of it to
hash yet. An Ultimate that implements those writes has to add the type to the list.

The U64 core sources are not part of this repository, so none of this is confirmed for U64
hardware.

The baseline is taken at the end of `read_crt()`, after mirroring and chunk
regeneration — the state the C64 starts from. A flash cartridge (EasyFlash, GMod2, MegaByter; C64MegaCart on the TRX64 bitstream) is
not mirrored inside its chip: the banks its file leaves out stay $FF, as erased flash, so an EAPI
free-bank scan finds them free. Its chunks are regenerated as the whole chip, one packet per bank
in bank order (EasyFlash: ROML and ROMH), so a save writes the complete cartridge, erased banks
included, and nothing of the mirrors above the chip; a GMod2 EEPROM packet follows the banks.
The baseline is renewed after every successful save and after the user declines one.

### 4.5 Trigger

`UserInterface` gets one static hook, called in `run_once()` after the menu has appeared:

    static void set_menu_enter_hook(void (*hook)(UserInterface *));

`c64_subsys.cc` registers its handler there and owns everything that needs the user interface: the
check, the dialog and the file juggling. `c64_crt.cc` keeps only what belongs to the cartridge
image — source path, hash, `save_crt()` — and therefore still builds in the host test under
`software/io/c64/tests`, which compiles that file on its own. `userinterface.cc` gains no
dependency on the C64 code, so the updater build (`RECOVERYAPP`) is unaffected.

The hash runs without stopping the C64. In overlay mode a game writing at that moment can yield
a torn read. Two outcomes: a false "changed" (the save that follows runs stopped and is correct),
or a missed change (caught at the next menu open). The save itself runs stopped.

### 4.6 Modes

Config item **Save Changed Cartridge** (`CFG_C64_CRT_AUTOSAVE`, id `0x76`), group *Memory
Configuration*, after *Cartridge*. Values `Off`, `Ask`, `Auto`; default `Ask`.

| Condition at menu open | Off | Ask | Auto |
|---|---|---|---|
| No CRT loaded, or unchanged | — | — | — |
| Changed, source writable | — | *Save changes to NAME?* Yes: save. No: new baseline. | Save, progress bar only |
| Changed, source not writable | — | One notice, new baseline | One notice, new baseline |
| Save failed | — | Error popup, baseline kept | Error popup, baseline kept |

"Not writable" covers: no source, `/Temp` (REST uploads), `/Flash` (internal flash; too small for
large images, and not to be worn by periodic writes), and any path `is_path_writable()` rejects.
The notice points to *Save Cartridge*; the new baseline stops it from repeating until the next
change. A failed save keeps the old baseline and reports again at the next menu open.

### 4.7 Write procedure

    NAME.tmp  <- save_crt()                 fail: delete NAME.tmp, report
    NAME.bak absent:  NAME -> NAME.bak      once, keeps the file as it was first loaded
    NAME.bak present: delete NAME
    NAME.tmp -> NAME

`.bak` is appended rather than replacing `.crt`, so the backup does not show up as a loadable
cartridge. If the last rename fails, the new state stays in `NAME.tmp` and the error says so.

### 4.8 Without the menu (Auto)

A menu-only trigger leaves the case that loses the save open: power off without opening the
menu. The drives do not need the menu either; `C1541::task()` polls dirty flags and writes
changed tracks back while the C64 runs. With *Auto* the cartridge does the same, in a task of its
own (`crt_autosave_task`, `c64_subsys.cc`), which polls every 250 ms:

| Write | Seen by the firmware through |
|---|---|
| GMod2 EEPROM | its dirty flag (`C64::get_eeprom_dirty()`); the contents are taken into the image on the way, which clears it for the next write |
| EasyFlash sector erase | the UCI command the firmware's EAPI sends when the UCI is there (`CTRL_CMD_EASYFLASH`, `control_target.cc`), which calls `C64_CRT::note_change()` |
| EasyFlash byte program | nothing: EAPI arms `$DE09` and writes the ROM window directly |

A write starts a check, which runs once the C64 has been quiet for 2 s, because a game saves in
several steps. The hash decides, as at the menu: the image must differ from the file and be the
same at two checks one quiet period apart. That also takes in the bytes programmed after the
erase that gave the notice. Nothing is written without a change, and nothing is hashed while
nothing happens.

The C64 is not stopped. `C64_CRT::take_snapshot()` copies the chunks the C64 can change (up to
1 MiB, from the heap, given back after the save) and hashes the copy; if the image moved while
it was copied, the check is repeated later. `save_snapshot()` then writes the file from that
copy, while the game keeps running, through the procedure of §4.7.

One save runs at a time: the menu hook, the task and *Save Cartridge* share a mutex, and a menu
that opens while the task writes leaves the check to it. `C64_CRT` has its own mutex, so a load
from the user interface or REST cannot replace the chunks under a save.

Ask does not use the task: a question in the middle of a game is worse than one at the menu.

## 5. Known gaps

- Under Ask, REST `run_crt` and anything else that loads a cartridge without opening the menu
  replaces a changed image without a check. Under Auto, `crt_autosave_before_reset()` writes the
  changes back first, from a snapshot: before every `load_crt()` caller (REST PUT and POST
  `run_crt`, the file browser and the socket command through `FileTypeCRT`, apps) and in
  `C64_START_CART`, reset, reboot, Clear Mem and power off.
- The telnet menu (`run_remote()`) has no hook.
- A save that is in progress on the C64 when the menu opens is captured half-written. Real
  hardware has the same exposure to power-off.
- Cartridge types other than EasyFlash and GMod2 are hashed on the assumption in 4.4.
- Under Auto, an EasyFlash change without a UCI erase (a game with its own flash code, or bytes
  programmed into erased space) is found only at the next menu open. Seeing every program needs the flash model in the
  FPGA: `amd_flash.vhd` has a dirty bit and an interrupt, but `all_carts_v5.vhd` does not
  instantiate it.

## 6. Hardware test

| # | Setup | Action | Expected |
|---|---|---|---|
| 1 | EF game with EAPI save, mode Ask | Save in game, open menu | Prompt; Yes writes `NAME`, creates `NAME.bak` |
| 2 | as 1 | Save again, open menu, Yes | `NAME` updated, `NAME.bak` unchanged |
| 3 | as 1 | Open menu without saving in game | No prompt |
| 4 | as 1 | Prompt, No; open menu again | No second prompt |
| 5 | GMod2 game, mode Ask | Save in game, F4 System Info, then open menu | Prompt appears (D2 fixed) |
| 6 | EF game, overlay on HDMI | Save in game, open overlay, Yes | Saved; game keeps running after close |
| 7 | CRT via REST upload | Save in game, open menu | Notice once, no write |
| 8 | Mode Auto | Save in game, open menu | Progress bar, no prompt |
| 9 | Manual *Save Cartridge* as NEW | Change, open menu | Prompt names NEW |

Log lines to check (syslog): `[CRT] hash`, `[CRT] saved`, `[CRT] save failed`.

Row 1 has been run on a U64 (2026-09-20): an EasyFlash game that saves through EAPI, mode Ask. The
prompt appeared after the game had saved, the file was written back and `NAME.crt.bak` was created.
Run again on 2026-09-22 with the syslog captured: `[CRT] hash E66219BE, baseline A68B886F, 80 ms`
for the 1 MiB image, then `[CRT] saved /USB1/wasteland_ef_v1.0.1.crt`.
The other rows are still open.

Rows 1 to 4 and 8 are covered by `tests/e2e/io/c64/cartridge_autosave_test.py`, which builds an
EasyFlash cartridge that programs its own ROM on demand and drives the three modes through the
menu; it also checks the saved file for the cartridge's own EAPI, and the notice for a source that
cannot be written back. Rows 5 to 7 and 9 are still by hand.

## 7. Emulator run, 2026-09-20

ue2emu, U64-II firmware from this branch, C64 ROMs on, the cartridge on a USB stick built from a
host directory. Wasteland (EasyFlash, 1 MiB, EAPI save).

| Step | Observed |
|---|---|
| `PUT /v1/runners:run_crt?file=/Usb0/wasteland_ef_v1.0.1.crt` | EasyFlash, 1 MiB read (the run predates the removal of the EAPI substitution) |
| Intro SPACE, menu S, game reaches "Enter new location (Y/N)?" | the game writes its save to flash |
| Menu button | `[CRT] hash 62D526E6, baseline D05CA873, 115 ms` |
| — | popup "Cartridge changed. Save it to wasteland_ef_v1.0.1.crt?" |
| Yes | `[CRT] saved /Usb0/wasteland_ef_v1.0.1.crt`, host file 553 KB, `…crt.bak` 1 MiB |
| Load the saved file, menu button | `[CRT] hash 62D526E6, baseline 62D526E6` — no popup |
| Start the game from it | bottom bar reads "Continue Load Restart", not "Start …" |

The file is smaller than the original because `save_crt()` drops EasyFlash banks that are all
`$FF`; `read_crt()` clears the region to `$FF` first, so the image in memory is identical, which is
what the equal hashes show.

115 ms for 1 MiB in the emulator. The same image on a U64 takes 80 ms (§6): the hash uses no
multiply, so the Nios II is not penalised for lacking one, and the emulator is the slower of the
two. The log line carries the number either way.

## 8. Emulator run of the Auto task, 2026-09-27

`cartridge_autosave_test.py` against ue2emu (U64-II build of this branch), 14 checks OK. The
scenario for §4.8: the test cartridge is subtype 1, so the UCI stays at `$DE1C`; mode Auto,
cartridge restarted, then from C64 code a sector erase over the UCI (bank 8) and one byte
programmed through `$DE09` into bank 0, menu closed throughout:

| Console | Meaning |
|---|---|
| `Clearing EF Sector at $03C20000` | the erase reached the firmware, `note_change()` |
| `[CRT] saved /USB0/autosave.crt`, `[CRT] auto-save: done` | written by the task, about 4 s later |

The file then carries the programmed byte. `gmod2_eeprom_dirty_test.py` is unchanged and OK.

Two findings of that run, both outside this feature:

- An EasyFlash CRT of subtype 0, which is most of them, asked for the UCI at `$DE1C` and
  prohibited it at `$DF1C`, and `set_cartridge()` switched it off for both. Fixed by "Keep the UCI
  at $DE1C for every EasyFlash", run on a C64U: every EasyFlash now has the UCI, so its EAPI
  erases through it and the Auto task sees the erase.
- Changing any setting of *C64 and Cartridge Settings* while a cartridge runs applies them all
  again (`C64::set_emulation_flags()`), which sets the UCI from the *Command Interface* setting
  and drops the one the cartridge asked for, until the cartridge is started again.
