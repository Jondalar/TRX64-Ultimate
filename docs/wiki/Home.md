# TRX64 Ultimate

TRX64 Ultimate is an alternative firmware for the **Commodore 64 Ultimate** and the
**Ultimate 64 Elite II**: its own FPGA bitstream for the Artix-7 XC7A50T plus a modified
Ultimate application (3.15) on top of it. Both machines use the same mainboard; one build
installs on both. The new C64C Ultimate and the C77, with the XC7A100T, are not supported for
now; we are working on it.

It is built as a hardware runtime for [C64RE](https://github.com/Jondalar/C64ReverseEngineeringMCP):
the real C64, with the inside view a reverse engineer otherwise only gets from an emulator
(monitor, Timewarp, state access, an RPC interface). It also brings a set of Tools which I call "Apps" (.u2a). 
With those you can enhance the functionality of the Firmware. My examples are in the release package, it also includes
a cartridge tool for EF, EF3, GMOD2 and PTV Megabyter. You can dump and write cartridges within 1-2 minutes with the tool.

So the focus of this is absolutely *not to use it to play games*, please use the original firmwares for this. I also recommend
you use our [MultiCore project](https://github.com/Jondalar/TRX64U-MultiCore) if you want to tinker around with the capabilities of this firmware. This way
you will be able to easily switch between different versions. 

## Pages

| Page | What it covers |
|---|---|
| [Architecture](Architecture.md) | How the chip is cut: what comes from Gideon's Ultimate, what is new, what was left out; the paths through the chip |
| [Capabilities](Capabilities.md) | What the firmware does, area by area, and what it does not do |
| [User manual](../manual/README.md) | Every menu, function and app, with screenshots |
| [Resources](Resources.md) | Where the FPGA's LUTs, block RAM and the flash go |

Installing, backup and going back: `INSTALL.md` in the repository. Origin and licence of every
gateware block: `PROVENANCE.md`. Building: `BUILD.md`.

## Status

Release 0.3.0 (build `main-31e69921`, 2026-11-16); the files are on the releases page, the way to
install them in `INSTALL.md`. The milestones of the beginning ended when the platform work moved into
its own project, TRX64U-MultiCore (ADR-0066 in the repository); what is open is in `TODO.md`.

## Thanks

Gideon Zweijtzer (the Ultimate project), Mateusz Nalewajski (icepi-zero-c64, the C64 core this
one grew from), Dag Lem (reDIP SID and CIA), Randy Rossi (VIC-II Kawari), Sameer Puri (HDMI),
Christian Gleissner, the VICE team. Details in `THANKYOU.md` and `PROVENANCE.md`.
