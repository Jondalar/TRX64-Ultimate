# Building and loading TRX64 Ultimate

## Status of the bring-up

- Pinout: `constraints/c64u.xdc`, Gideon's constraints from
  [issue #615](https://github.com/GideonZ/1541ultimate/issues/615), checked against
  our own boundary-scan measurements (TREX/1541ultimate-tools, `docs/c64u-pinmap.md`).
- Loading: USB-Blaster on P5, `openFPGALoader -c usb-blaster -m design.bit` into
  SRAM only; a power cycle with the adapter unplugged restores the flashed image.
- Toolchain: Vivado 2024.1 (Artix-7 only) in a container on the NAS, see `vivado/`.

## Build and load

- Every push runs the pipeline of `.gitea/workflows/pipeline.yml` on Gitea's runners, one stage after the other
  (`docs/specs/T31-ci-pipeline.md`): lint, the XC7A50T bitstreams, package, then sims and cor (`trx64u.cor`).
  The 100T (Elite II) stage `bit-ue2` is commented out in `pipeline.yml` since 2026-10-16 (no Elite II at home).
  A push that changes nothing that goes into a bitstream gets the last good one (`ci/bitstream.sh`). The stages are
  `ci/*.sh`; `ci/lint.sh` and `ci/sims.sh` run the same way on the Mac. The result is the generic package
  `trx64-ultimate` (`<branch>-<sha>` and `<branch>-latest`): bitstreams, reports, `manifest.json`, `trx64u_<sha>.cor`.
- The Gideon-app line (`ultimate.app`, the `.cfw`/`.ue2` update files, `trx64g.cor`) is not built here since U20 A5
  (D13): the Gitea branch `gideon-app` keeps the firmware submodule and builds them by hand (its pipeline has no push
  trigger).
- `tools/load.sh [design] [version]` fetches one and loads it over the USB-Blaster;
  `tools/vivado.sh designs/<name>` builds by hand on the NAS.

## Designs

What comes next and in which order: [TODO.md](TODO.md).


- `designs/hdmi_test`: 720p60 test picture on HDMI through the four GTP lanes; programs
  the board's clock synthesizers to 148.5 MHz. First picture 2026-09-30. Lane 2 (TMDS 0)
  has P and N swapped on the board.
- `designs/c64_basic`: the icepi-zero-c64 C64 core (6502n, vicii-kawari, reDIP SID/CIA)
  from block RAM, its ROMs written by the Ultimate app (no ROM in the bitstream: without
  the app the C64 stays in reset); own 1080p50 scaler (`vic_scaler.v`, 4x, pillarbox,
  frame-locked to the VIC); C64U keyboard over I2C (`c64u_keyboard.v`), joysticks and
  RESTORE from their pins. Boots to READY. Debug cells in the left bar while bringing up.
  Next to it Gideon's RISC-V (rvlite, 100 MHz) with the stubs the U64-II "ultimate" app
  needs (`ultimate_sys.vhd`, `docs/ultimate-app-hw-spec.md` step A): the app, loaded
  over JTAG (`tools/trxdbg.sh riscv ultimate.bin follow`), boots to its main loop; the
  boot ROM also boots the app image at flash 0x220000 as Gideon's does, but only when the
  bitstream in flash carries the running bitstream's build stamp, so SRAM-loaded test
  bitstreams never start the flash app (`docs/specs/T15-flash-boot.md`); its
  console is read over JTAG (`tools/trxdbg.sh console`). DDR2 map: app 0-64 MB, REU
  from 64 MB. Steps B-D: Ethernet (Gideon's RMII MAC on the KSZ8081, DHCP, REST,
  telnet, FTP), the ESP32 UART, the app's C64 control (reset, pause, DMA into C64
  RAM, run_prg through its boot cartridge, keyboard buffer) and the U64 UDP video
  stream; checks: `tests/run_app_c64_test.sh <ip>`, `tools/stream_shot.py <ip>`.
- `designs/ddr2_test`: the 128 MB DDR2 through our own controller `designs/c64_basic/ddr2_lean.v`
  (DDR2-300, `docs/specs/OPT-ddr2.md`), calibrated by a hardware sequencer, full-memory test in
  both orders with write masks and pingpong; result on the case LEDs and over JTAG
  (`tools/ddr2_status.py`: calibration window, rounds, errors, first error).

## Tests on the board

- `tests/run_lorenz.sh`: Lorenz's 6510/CIA suite over JTAG.
- `tests/run_cart_tests.sh`: cartridge flash and EEPROM write-back (EasyFlash EAPI,
  AMD flash, GMod2 EEPROM).
- `tests/run_vice_testprogs.sh [--only NAME ...] [--group G ...] [--all]`: the VICE
  cartridge testprogs (C64-carts, C64-freezer, C64-bankio, C64-openio from
  `$VICE_TESTPROGS`, default `~/Development/C64/Tools/vice/testprogs`): each CRT is
  started over REST, a test .prg put into RAM over JTAG, the verdict read from the
  border colour (state bus), the screen, or the video stream against VICE's reference
  pictures; types the model lacks are listed as unsupported, freezer tests as manual;
  reboots the machine at the end. Results in `.cache/vice_testprogs/`; the baseline of
  the icepi cartridge model is `docs/cart-vice-baseline.md`.

## Sources

Origin, version and licence of every gateware block in the bitstreams: [PROVENANCE.md](PROVENANCE.md).

- `cores/icepi`: our C64 core, edited here directly. It started as the icepi-zero-c64
  gateware (Alex's fork); origin commits and the former patches are listed in
  `cores/icepi/ORIGIN.md`. Licences: GPL-3 (icepi, kawari), CERN-OHL-S v2
  (reDIP SID/CIA); fine for internal use, to be settled before anything is published.
- `vendor/gideon`: our copy in git of the parts of Gideon Zweijtzer's 1541ultimate
  (GPL-3) that the bitstream and the benches read, from his upstream commit in
  `vendor/gideon/VENDOR.md` with our patches from `tools/patches/gideon` applied;
  `tools/gideon_sources.sh <1541ultimate checkout>` renews it, without an argument it
  only checks that the copy is there (builds and simulations call it); never the files
  marked "not to be used in other projects". `software/bootrom`: our rvlite boot ROM from his `bootloader_u64ii.c`,
  built by `tools/build_bootrom.sh` (1541ultimate build image) into
  `designs/c64_basic/bootrom_pkg.vhd`.
- The Ultimate app (Gideon's firmware with our changes, the fork `TREX/1541ultimate`,
  branch `trx64/main`) is no longer in this tree (U20 A5, D13): the Gitea branch
  `gideon-app` pins it as the submodule `software/ultimate-app` and builds it by hand;
  its `ultimate.bin` is what `tools/trxdbg.sh riscv` loads. This tree's application side
  is `software/trx64u` (`trx64u.cor`, built against MultiCore's SDK, `software/trx64u/README.md`).
- `roms/`: the stock C64 and 1541-II ROMs (Commodore/Cloanto) for the simulations only,
  not tracked (U20 D14): `ci/roms.py fetch` puts them there from the pins of `ci/roms.txt`
  (`roms/README.md`); no bitstream contains them. On the board the Ultimate app loads
  the C64's Kernal, BASIC and character ROM from `/roms` (its "Cartridge and ROM
  Settings") and writes them through its ROM apertures, as on Gideon's core; the C64
  stays in reset until it has. `tools/rom2mem.py <dir>` writes `c64_rom.mem` into a
  bench's run directory (preload with `TRX_SIM_ROMS`).

## Rules

- Test bitstreams go into SRAM, never into the flash.
- Unplug the JTAG adapter's USB before a power cycle; with it powered the C64U does not
  load its bitstream from flash.
- Check which pins a design leaves unused before loading it (Vivado pulls them down,
  which can enable the slot transceivers); no cartridge in the port for experiments.
