# TRX64 Ultimate

A free, alternative firmware for the Ultimate 64 Elite II board and the Commodore 64 Ultimate
(Artix-7 XC7A50T): its own FPGA bitstream plus the Ultimate application on top of it.

**Not supported for now:** the new C64C Ultimate and the C77, with the Artix-7 XC7A100T. We are working on it.

This is *only* the firmware, *not* my [MultiCore project](https://github.com/Jondalar/TRX64U-MultiCore). You can flash this to your C64U / UE2 board
and use it instead of the original firmware. MultiCore will offer this as a guest core also.

Please be aware *DOING SO YOU ARE LEAVING THE WARRANTY SPACE OF COMMODORE & Gideon* !!! This is a choice you make.
I will by no means be responsible for a bricked board! If you want to be safe, get a USB Blaster or FT232H device.

# Reason for this Firmware

It is built to serve as a hardware-based runtime for [C64RE](https://github.com/Jondalar/C64ReverseEngineeringMCP)
in C64 projects: the real machine, with the inside view a reverse engineer needs from an emulator.

## Core features

- **Timewarp ring**: the machine state of every frame in DDR2; rewind, scrub, run forward from any frame.
- **State access and dump**: every core (CPU, VIC-II, SID, CIA, drive) readable while the machine runs or is paused.
- **Monitor** at machine level: breakpoints, stepping, memory and register access, locally and over RPC.
  It is part of the firmware: its RPC server (WebSocket JSON-RPC, TCP 4312) answers from boot, with no
  application loaded, and breakpoints stay armed while anything else runs.
- **App loader**: further functions come as loadable apps (`.u2a`) instead of being built into the firmware.
- **Cartridge abstraction**: a generic cartridge decoder driven by a mapping descriptor runs `.crt` images.
  A real cartridge in the expansion port runs live on the port, as on a C64; the `trex_crt` app dumps it
  to a `.crt` when you ask it to.
- **Turbo**: the 6510 at up to 63 cycles per C64 cycle.
- **8 SIDs** on Dag Lem's reDIP SID, with our own mixer and our own sampler, compatible with the Ultimate Audio interface.

## Compatibility

Where TRX64 Ultimate aims for the behaviour of Gideon Zweijtzer's Ultimate firmware, measured on his core:

- the C64 itself: CPU, VIC-II, CIAs, timing, PAL/NTSC, palettes, open bus;
- the Ultimate functions: 1541 emulation and D64, CRT cartridges, REU up to 16 MB, UCI, USB, SD,
  Ethernet (REST, FTP, telnet), the freeze menu on HDMI, audio and debug streams over UDP;
- booting from flash, the keyboard, joysticks, the case and keyboard LEDs.

Where it deliberately differs:

- **Video**: HDMI only, no analog video out.
- **Sound**: Dag Lem's reDIP SID instead of UltiSID. In many places it is closer to the real
  6581 and 8580 than UltiSID, so the sound differs from the original firmware's.
- **Lighting**: no LED strip or lighting effects (keyboard and case LEDs stay).
- **Cartridge I/O**: no internal I/O devices (UCI, REU, Ultimate Audio) in `$DE00-$DFFF` while a
  cartridge is active.
- **Gideon's unmodified application** does not run on this bitstream; it runs with its own app.

## More

- User manual: every menu, function and app: [docs/manual](docs/manual/README.md)
- Installing, backup and going back: [INSTALL.md](INSTALL.md)
- Building, loading, designs and board tests: [BUILD.md](BUILD.md)
- Reporting a fault, asking for a function, sending a patch: [CONTRIBUTING.md](CONTRIBUTING.md)
- Licences: [LICENSING.md](LICENSING.md); origin of every gateware block: [PROVENANCE.md](PROVENANCE.md)
- C64 Reverse Engineering (C64RE): https://github.com/Jondalar/C64ReverseEngineeringMCP
- Thanks: [THANKYOU.md](THANKYOU.md)
