# 10. Differences to the stock firmware

The application is Gideon Zweijtzer's Ultimate application 3.15, modified, on our own bitstream.
It reports itself as version 0.2.0, ours and not his (ADR-0059).
These parts of the stock firmware are not here.

## Not available

| Function | What you see |
|---|---|
| Drive B, 1571, 1581 | No drive B entries; `.d71`/`.d81`/`.dnp` cannot be mounted; BUILT-IN DRIVE B opens nothing |
| Tape port (Datasette, tapecart) | No tape entries; `.tap` has no menu |
| Software IEC drive, printer emulation, UltiCopy | No such pages or entries; PRINTERS opens nothing |
| Modem (SwiftLink/ACIA) | MODEMS opens nothing |
| Built-in Machine Code Monitor | Replaced by the monitor in the firmware ([6.](06-monitor.md)); C= O opens it |
| UltiSID filter tables | Replaced by the reSID8 engines and their curves |
| Analog video out, HDMI modes other than 1080p, DVI | The output is always 1080p HDMI |
| LED strip and keyboard light effects | The settings stay; only SHIFT LOCK lights |
| Drive and tape sound | The mixer items stay without effect |
| Debug and IEC streams | Only the video and audio streams |
| Serial bus mode, parallel cable, burst patch | The settings stay without effect |
| User port | No user port |
| Real-time clock | The time comes from the network at start |
| Pause/Resume, bus measurement, developer pokes | Not in the menu |

## Settings without effect

HDMI Scan Resolution, Analog Video Mode, Digital Video Mode, Vol/Pan Drive and Tape, Case Lights,
Keyboard Lights, Bus Operation Mode, Burst Mode Patch, Parallel Cable to Drive A, Serial Bus Mode,
Stream Debug to, Debug Stream Mode, Ulticopy Uses disk name.

## Only here

The monitor `trxmon`, part of the firmware, with the C64RE connection, Timewarp, reSID8 (up to 8 SIDs), the turbo up to
64 MHz, writing changed cartridge images back to their files, the cartridge tool for real
cartridges, and the Freeze menu over the still picture.
