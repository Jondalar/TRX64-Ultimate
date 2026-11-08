# TREX CRT Tool

Ultimate 64 cartridge tools as a loadable `.u2a` app: Cart Inspector,
Dump Cartridge and Flash Cartridge / Write to Cartridge. They are 1:1 C
ports of the former firmware tools (`CartBase`, `CartDumper`,
`CartWriter`, `CartInspector`) and look and behave the same.

## Bundle contents

```
apps/trex_crt/
├── Makefile          # produces trex_crt.u2a
├── build.sh          # container wrapper for Makefile
├── manifest.json     # file types and task menu entries (embedded in the .u2a)
├── src/
│   ├── main.c        # entry point, dispatch on the manifest action
│   ├── cart_port.h          # the platform: bus, registers, time, heap, files
│   ├── cart_port_u2p.c      # it filled from the app ABI (another ABI = another file)
│   ├── cart_view.c/.h       # what the tools report and ask; no screen in it
│   ├── cart_fmt.c/.h        # the tools' snprintf (freestanding, no libc)
│   ├── cart_base.c/.h       # bank switching, detection, scanning, bank map
│   ├── cart_flash.c/.h      # the flash profile of a type, from its descriptor
│   ├── cart_dumper.c/.h     # Dump Cartridge: probe, scan, dump
│   ├── cart_writer.c/.h     # Flash Cartridge: write, verify, erase
│   ├── cart_view_text.c/.h  # the 40 x 22 screen: the keys, the dialogs, the drawing
│   ├── cart_screen.c/.h     # the frame, the bank map and the lines it draws
│   ├── ui_shim.c/.h         # Window, key loop, hex input, file dialogs over the app ABI
│   └── cart_inspector.c/.h  # Cart Inspector (a hex view, it draws its own window)
(SDK: software/appsdk, shared by every app)
    ├── include/{u2p_app.h, u2p_app_api.h, app_format.h}
    ├── crt0/app_crt0_nios2.S, crt0/app_crt0_riscv.S
    ├── linker/app_nios2.lds, linker/app_riscv.lds
    ├── app.mk        # build rules for both variants
    └── tools/mk_u2a.py
```

No dependencies on the firmware source tree. Drop this directory
anywhere with the Nios II and RISC-V toolchains in `$PATH` and `make`.

## Build

Inside the repo (container build):

```
./apps/trex_crt/build.sh
```

Or directly, once `nios2-elf-gcc` and `riscv32-unknown-elf-gcc` are on `$PATH`:

```
cd apps/trex_crt
make
```

Result: `trex_crt.u2a` in the bundle root.

## Install and start

Copy `trex_crt.u2a` to the U64 (USB stick or SD). Requires U64 firmware
with the App Loader (format version 2). Freeze the C64, select the file
in the File Browser and choose `Run App`. The manifest registers:

| Where                         | Entry             | Action    |
|-------------------------------|-------------------|-----------|
| F5 task menu, C64 Machine     | Dump Cartridge    | `dump`    |
| F5 task menu, C64 Machine     | Cart Inspector    | `inspect` |
| F5 task menu, C64 Machine     | Flash Cartridge   | `flash`   |
| Context menu of `.crt` files  | Write to Cartridge | `write`  |

A plain `Run App` shows a chooser with the three tools. Flash Cartridge
without a file only scans the flash cart; writing needs a `.crt` from the
context menu.

## Keys

| Tool           | Keys                                                           |
|----------------|----------------------------------------------------------------|
| Dumper         | F1 scan, F3 dump, F5 hint, F7 slot (EasyFlash 3), RUN/STOP exit |
| Writer         | F1 scan, F3 write, F5 verify, F7 slot (EasyFlash 3), RUN/STOP exit |
| Inspector      | F1 go to, F3 write byte, F5 set `$01`, F7 set `$DE00`, cursor up/down scroll, left/right bank, RUN/STOP exit |

## Notes

- Dumper and writer need access to the cartridge. This differs per core:
  - Gideon's core (v3.15): with Cartridge Preference Auto or External and
    a cartridge in the slot DMA does not reach it, and the tools ask to set
    it to Internal and reboot. Nothing else changed on this path.
  - TRX64 Ultimate (found by `api->trx` and the "TRX2" ID, the cartridge
    port block at `0x101A7000`): the port serves the cartridge while
    Cartridge Preference is Auto or External (the default) and the C64 was
    reset with the cartridge in. With Internal, or a cartridge plugged
    in after the last reset, the tools say "The port is not enabled": set
    Auto or External, reset the C64 and freeze again. All three tools
    take the freeze's Ultimax off first, so ROML/ROMH reach the port.
  - TRX64 Ultimate: ROML/ROMH writes are blocked by the hardware unless
    `ROM_WR_EN` (`SLOT_CTRL` b1) is set. The writer raises it only around
    its flash writes (and the EF-Directory sector of an EasyFlash 3),
    puts it back to the value it found on every way out and chips left in
    command state are reset first. The firmware policy "Cartridge Port
    Writes" (Auto by default: the bit is up while the port serves ROM, so
    in-game saves work) decides what the bit is outside the writer.
  - TRX64 Ultimate: bank registers and the EasyFlash mode are write-only,
    so a tool that touched the cartridge resets the C64 when it ends
    ("Resetting C64"); the program restarts from the cartridge.
- The dumper reads the whole cartridge first and writes the file when the
  cartridge is done ("Saving to file..."), so a drive that drops out during
  the long read cannot spoil the dump. The CRT is staged, in this order:
  1. in 64 KB blocks from the app heap (`api->mem->malloc`), all allocated
     before reading (the whole EasyFlash 3 is 8405056 bytes, 129 blocks);
  2. if the heap has not enough, in a file on the RAM disk `/Temp`, copied
     to the target in 16 KB pieces afterwards. The app API has no file
     delete, so after a successful copy the temp file is emptied (0 bytes);
  3. if `/Temp` cannot be written either, straight into the target file
     while reading, as before.
  The log shows the path taken and how many blocks the heap gave. If the
  write to the target fails, the image stays (in RAM or in `/Temp`) and
  "Saving failed. Retry?" appears: plug the drive again and answer YES to
  write again without reading the cartridge a second time; NO discards the
  image (a partly written target file stays on the drive).
- One slot of an EasyFlash 3 is saved as an EasyFlash CRT (type 32) like
  `ef3_image.py extract` writes it: EXROM 1 / GAME 0 in the header, CHIP
  type 2 (flash), and the Am/M29F040 EAPI in bank 0 ROMH at `$1800` when
  the bank carries the "eapi" signature there (the slot's MX29LV640 driver
  is of no use outside the EasyFlash 3). The writer puts the MX29LV640
  EAPI back when such a CRT is written to an EasyFlash 3. The whole
  EasyFlash 3 (type 90) stays a 1:1 copy.
- C64MegaCart dumps and writes carry CRT type 88 (T33), not Magic Desk's
  19. TwoMegabyter is not supported.
- GMod2 dumps get the 2 KB EEPROM appended as a CHIP packet with load
  address `$DE00`.
- EasyFlash and EasyFlash 3 writes replace the EAPI of the CRT like
  EasyProg: when bank 0 ROMH holds the signature "eapi" at `$1800`, the
  768 bytes there are written with skoe's driver for the detected chip
  ("Am/M29F040 V1.4" on an EasyFlash, "MX29LV640EB 1.2" on an EasyFlash 3,
  `src/eapi_drivers.c`). Verify compares against the replaced driver.
- GMod2 writes: after the flash is written and verified, the writer puts
  the CRT's EEPROM packet (load address `$DE00`) into the EEPROM, word by
  word where it differs, and reads it back. A CRT without that packet
  leaves the EEPROM erased (all `$FFFF`), so saves are gone. Verify (F5)
  checks the EEPROM against the CRT as well.
- EasyFlash 3: found by its `$DE01` slot register, so only while it runs
  in EasyFlash mode (menu or a slot started from it, not in KERNAL or
  freezer mode). Dumper and writer ask for one of slots 1-7, listed by
  their names in the EF-Directory; F7 picks another slot. A dump holds the
  64 banks of that slot as an EasyFlash CRT named after the slot. The
  writer takes EasyFlash CRTs (type 32) only, erases just the 16 sectors
  of that slot (never the whole chip), programs with the sequence of
  skoe's `eapi-mx29640b.s` and then asks for the cartridge name and saves
  it in the EF-Directory the way EasyProg does. Slot 0 (menu, KERNALs,
  freezers) is never written apart from the directory. The slot active
  at start is selected again when the tool exits.
- Cart Inspector: it only reads to recognise an EasyFlash 3. With bank 0
  selected it looks for the EAPI header at `$B800` (16K) and `$F800`
  (Ultimax); only when that driver is "MX29LV640EB" does it show
  "EasyFlash 3" and the slot read from `$DE01`.

## Host tests

`apps/trex_crt/host/run.sh` builds the app's real C files for the host
against a mock `u2p_api_t` (C64 bus with the PLA, the CPU port and the
freeze's Ultimax, the slot registers, `ROM_WR_EN`) and models of ROM16,
EasyFlash 1 and 3, MegaByter, GMod2 (with its EEPROM) and C64MegaCart. It
needs only a host C compiler and takes well under a minute. It checks the
stock traffic against `golden/stock.txt` (taken from the app as it was
before T57), dumps and writes on our core, the write inhibit, the cartridge
stopping at every access of a write run, the arming messages, the restore
and the reset, and the policy bit.

It then links a second binary, `trex_core_test` (`host/core_only.c`), out of
the cores alone: no `cart_view_text.c`, no `cart_screen.c`, no `ui_shim.c`,
no `main.c`. It probes, scans, writes and verifies a MegaByter with no view
installed and compares the flash with the CRT, so a core that reaches for a
window, a key code or a dialog again fails the link (T95 S5).
