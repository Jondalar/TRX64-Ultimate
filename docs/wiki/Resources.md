# Resources

Post-route numbers of the current `main` gateware (CI build of 2026-10-22, Vivado 2024.1,
`c64_basic.utilization.rpt` and `c64_basic.utilization_hier.rpt` of the package).

| Resource | Used | Of | |
|---|---:|---:|---:|
| LUT | 26,744 | 32,600 | 82.0 % |
| Slices | 8,146 | 8,150 | 99.9 % |
| Block RAM tiles | 60.5 | 75 | 80.7 % |
| DSP | 23 | 120 | 19.2 % |
| Timing (WNS) | +0.028 ns | | |

The slices are full: LUTs are spread over almost every slice. New logic has to replace
something or come with savings; the per-module report of every CI build is the reference.

## LUT per area

Sums of the hierarchy report; the area sums overlap by less than 0.5 %.

| Area | LUT | Largest blocks |
|---|---:|---|
| C64 machine | 10,944 | SID path 3,429 (reDIP 8-SID 1,861, mixer/audio 1,197), VIC-II 2,014, 6510 1,351, CIAs 1,062, cartridge decoder and port ~970, REU 834, sampler 643, turbo 323 |
| Gideon's Ultimate system | 6,441 | RISC-V 2,140, 1541 drive 1,822, USB 685, WiFi UART 498, Ethernet 418, flash gate 173, UCI 167 |
| Monitor, Freeze, Timewarp | 3,547 | ring engine 1,362, application registers 822, breakpoints and observers 700, pixel layer 555, freeze 108 |
| Video and audio out | 1,924 | HDMI encoder 630, overlay 399, HDMI transmitter 318, menu in the C64 picture 188, scaler 168 |
| Memory | 1,461 | DDR2 controller 845, arbiter 338, cartridge data load 254 |
| Development | 884 | JTAG debug 700 |
| Input and LEDs | 702 | P7 keyboard 288, C64U keyboard 104 |
| Network streams | 690 | video 292, audio 250, RMII 143 |
| Rest | 238 | top level, bridges, USB PHY, resets |

## Flash (16 MB, W25Q128)

| Range | Holds |
|---|---|
| 0x000000-0x21FFFF | Bitstream |
| 0x220000-0x3FFFFF | Ultimate application |
| 0x400000-0xFE7FFF | Flash drive (FAT): ROMs, cartridges, apps, configuration files |
| 0xFE8000-0xFFFFFF | Settings |

The boot ROM in the bitstream loads the application from 0x220000 only when the bitstream at
0x000000 carries the running bitstream's build stamp. The updater writes both ranges and keeps
the flash drive and the settings.
