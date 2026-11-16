# Capabilities

State of `main`. "Untested" means built, but not yet confirmed on the board.

## C64 machine

| Function | State |
|---|---|
| 6510, VIC-II, 2 × CIA, PLA | PAL, NTSC, PAL-60, NTSC-50; custom palettes as in Gideon's core; unmapped I/O reads return the VIC's byte (open bus) |
| Turbo | 6510 at up to 63 cycles per C64 cycle |
| SID | reSID8: up to 8 SIDs on Dag Lem's reDIP, own mixer and sampler (Ultimate Audio compatible) |
| Real SID chips in the sockets | Built, untested |
| REU | Up to 16 MB, as in Gideon's core |
| UCI | Ultimate Command Interface at $DF1C-$DF1F |
| 1541 | Drive A on the IEC bus, D64 from SD card or USB stick |

## Cartridges

| Function | State |
|---|---|
| CRT files | Normal 8K, 16K, Ultimax, Ocean, Simons' BASIC, C64GS, Dinamic, Magic Desk, EasyFlash, EasyFlash 3 (all 8 slots, type 90), EasyFlash XL, MegaByter, 2MegaByter, GMod2, C64MegaCart, Action Replay, Retro Replay; flash and EEPROM changes written back to the CRT file |
| Expansion port | Physical cartridges; tested: EasyFlash 1, EasyFlash 3, MegaByter, GMod2 |
| Cartridge tool (`trex_crt`) | Dump a physical cartridge to a CRT file, write a CRT file to a physical cartridge, inspect a CRT; EasyFlash 3 one slot or all 8 |
| Internal I/O while a cartridge runs | REU, Ultimate Audio and UCI are off while a cartridge (physical or CRT) is active |

## Monitor, Freeze, Timewarp

| Function | State |
|---|---|
| Monitor (`trxmon`) | VICE monitor command set plus extensions: breakpoints, observers with conditions, raster breakpoints, single step, cycle step, step over, reverse step, side-effect-free memory reads, assembler, disassembler, the 1541 CPU as a second target |
| C64RE interface | JSON-RPC 2.0 over WebSocket, TCP port 4312, TRX64 method names |
| Freeze | Stops the whole machine; the Freeze menu shows the monitor with 80 columns |
| Timewarp | Delta ring in DDR2: rewind and scrub on the Timewarp bar, run forward from any frame; instruction trace of the last second (history per cycle, last writers of an address, causal chain to a wild PC) |

## Ultimate system

| Function | State |
|---|---|
| Menu, file browser, settings | Gideon's Ultimate application 3.15, on HDMI and in the C64 picture |
| Storage | SD card, USB sticks and hub, flash drive |
| Network | REST, FTP, Telnet; video and audio as UDP streams |
| Input | C64U keyboard and P7 keyboard, joysticks, keyboard and joystick injection from the app |
| LEDs | Case LEDs, keyboard LEDs, SHIFT LOCK |
| Apps | `.u2a` apps from the file browser; installed apps add entries to the F5 task menu and the file context menus |
| Update | Updater for bitstream and application with a restore bundle and a raw flash backup; boots from flash without JTAG |

## Not supported

- Machines with the XC7A100T
- Analog video out; HDMI resolutions other than 1080p
- UltiSID, Copper, LED strip and lighting effects
- Drive B, 1571, 1581
- Tape port (Datasette, tapecart), user port
- Paddles, mouse, keyboard-to-joystick mapping, joystick port swap
- `.c64re` snapshot save and load; Timewarp ring save and load; trace export
- EasyFlash 3 freezer modes (AR, RR, NP, SS5) and its USB port; GMod4
- Gideon's unmodified application on this bitstream

## Planned

Snapshots (`.c64re`) saved and loaded by the firmware, tape port, drives beyond one 1541,
Timewarp UI extras, a leaner ring engine, Erase Cartridge in `trex_crt`.
