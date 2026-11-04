# TRX64 Ultimate user manual

How to use TRX64 Ultimate on the C64 Ultimate and the Ultimate 64 Elite II: every menu, every
function and the apps. It describes release `0.2.0`, build `main-63761352` (host `6376135`, app
`99fccc38`). The release before it, `b151154b`, is the state before the monitor moved into the
firmware. For installing and going back to the stock firmware see [INSTALL.md](../../INSTALL.md).

| Chapter | What it covers |
|---|---|
| [1. Basics](01-basics.md) | Opening the menu, the two looks (Commodore and Ultimate), Freeze and Overlay, keys |
| [2. File browser](02-file-browser.md) | Drives, navigation, what each file type offers |
| [3. Action menu](03-action-menu.md) | The task menu: machine, drive, configuration, streams, developer |
| [4. Settings](04-settings.md) | Every settings page and item, with values, defaults and what they do on this bitstream |
| [5. Cartridges](05-cartridges.md) | CRT files, real cartridges in the port, saving changes, the cartridge tool `trex_crt` |
| [6. Monitor](06-monitor.md) | The monitor in the firmware: commands, breakpoints, observers, the C64RE connection |
| [7. Timewarp](07-timewarp.md) | Rewinding the machine, the Timewarp bar |
| [8. Apps](08-apps.md) | Installing apps; `softboot` and `unzip` |
| [9. Network](09-network.md) | REST API, FTP, Telnet, streams, discovery |
| [10. Differences](10-differences.md) | What the stock firmware has and this one does not, and settings without effect |

## Conventions

- Key names are those printed on the C64 keyboard. **C=** is the Commodore key; C= R means hold C=
  and press R.
- Menu paths are written **Menu > Entry > Entry**.
- "Our bitstream" is the TRX64 Ultimate FPGA bitstream. Where the stock bitstream behaves
  differently it is said.

## About the pictures

The screenshots come from the ue2emu emulator with the capability word of our bitstream, so menus,
texts and order are the firmware's own. Most of them were taken from the `b151154b` package; the
freeze menu (1.) on 2026-10-31 from the U5 build, because the old picture was cut off on the left;
the C64 Machine category (3.), the Timewarp bar (7.) and the three pictures of the snapshot app (8.)
on 2026-10-31 from the build that carries that app; the monitor's first two pictures (6.) on
2026-11-01 from the build whose service starts at boot, over a cartridge running in the expansion
port, so the disassembly is a program and not empty memory. The other two monitor pictures are from
the board. Values that depend on the machine (IP and MAC addresses, the USB stick's name, the host name, dates) are the
emulator's. Pages whose content depends on hardware the emulator does not model the same way
(the reSID8 page, the drive menu, real cartridges) are described in tables only.
