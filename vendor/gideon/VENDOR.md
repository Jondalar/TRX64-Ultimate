# 1541ultimate (Gideon Zweijtzer), vendored parts

Our own copy of the parts of Gideon Zweijtzer's 1541ultimate that the c64_basic bitstream and its benches read (U20 D3, D5), kept in git, no submodule behind it. Made by `tools/gideon_sources.sh <1541ultimate checkout>` (tools/vendor_gideon.sh; the file list is `files` there). Do not edit here: renew the copy from upstream or add a patch in tools/patches/gideon.

| repository | commit | date |
|---|---|---|
| https://github.com/GideonZ/1541ultimate | 1ba94abd314186559dfdeee9690e16d5d251f552 | 2026-09-20 |

Renewing: check out the upstream commit to take in a clone of that repository, then `tools/gideon_sources.sh <clone>`. The script copies the listed files, gives the drive's 6502n entities the prefix drv_, applies the patches below and writes this file. Check the result with the benches of ci/sims.txt and a bitstream build.

Licence: GPL-3.0 (LICENSE.txt); no file here carries the restricted clause (the script refuses such a file). Gideon's UltiSID (fpga/sid6581), Copper (fpga/io/copper), lp_filter and sys_to_aud are not on the list and stay out.
`bootloader_u64ii.c` is kept as the reference for `software/bootrom/`.
The char_generator files and font_pkg are the reference for designs/c64_basic/overlay.v (fonts via tools/font2mem.py).
`nano_minimal.nan` (the USB nano CPU program the app loads) and `parse_nano.py` are kept for the USB simulation (designs/c64_basic/sim/usb).
dpram_8x16/dpram_8x32 come as entities only; designs/c64_basic/usb_dpram.vhd has the architectures.
fpga/6502n: the entities carry the prefix drv_ (the C64 core has the same ones; see tools/vendor_gideon.sh).
fpga/ip/memory pseudo_dpram_8x32: only the entity; designs/c64_basic/pseudo_dpram_8x32_x7.vhd is its architecture.
fpga/io/command_interface: the UCI (T28, designs/c64_basic/uci_unit.vhd), with dpram, slot_bus_pkg, io_bus_bridge2 and synchronizer_gzw.
`software/io/audio/sid_coeff.c` (the app's SID filter curve arrays) is read by the SID reference bench only (designs/c64_basic/sim/sid/run_m1.sh, cores/trx_sid/model/gen_curves.py); no bitstream reads it.
designs/c64_basic/sim/eth/run.sh orig makes Gideon's unpatched eth_transmit.vhd from this copy by reversing 0003 at bench time.

Patches applied from tools/patches/gideon:
- 0001-c1541-drive-debug-status.patch
- 0002-floppy-head-on-track-18.patch
- 0003-eth-transmit-store-and-forward.patch
- 0004-drive-cpu-sync.patch
- 0005-rvlite-dbg-pc.patch
- 0006-rvlite-mul-pipeline.patch
