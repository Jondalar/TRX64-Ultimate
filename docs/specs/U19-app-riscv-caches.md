# U19 — The app RISC-V gets MultiCore's caches: an 8 KB data cache and an 8 KB instruction cache, filled 16 bytes at a time

**Status:** BUILT (2026-11-23). Gateware, benches and CI green (pipeline on `54dc9f9e` with the update files); `trx64u` on the
board accepted (Alex, 2026-11-23); the `trx64g` `.cfw` on this bitstream not yet run on the board (§6).
**Repo:** TRX64-Ultimate (gateware `designs/c64_basic/`; a comment in `software/ultimate-app`).
**Number:** U19 (registry: the TODO table in `TODO.md`).
**Origin:** U18's first board run of `trx64u.cor` on base 3.3 (Alex, 2026-11-23): menu open from the C64 5-8 s, category
change 3-5 s, tab change 3-5 s; the cursor in a table as fast as in the other cores. Alex: *"ok, dann bau mach das so,
bitstream anpassen, dann den rest. neuen Core bauen"*.

---

## 1. Why

- `trx64u.cor` runs on our `c64_basic` bitstream. Its RISC-V was Gideon's `rvlite_wrapper` with Gideon's 2 KB
  instruction cache and **no data cache**: every load from DDR2 waits for the bridge and the controller.
- MultiCore's host (where the C128 and SuperCPU cores and the Amiga menu run) has had a data cache and a larger
  instruction cache since its T133, both filled from the DDR2 read's whole 128 bits. Their board numbers for one
  menu workload: 7.9 s without caches, 6.7 s with the instruction cache, 4.4 s with the data cache, 3.4 s with both.
- The menu's work is the base's (LVGL), the same as the Amiga's: in ue2emu (no cache model) PC sampling put 75 % in
  LVGL, 9 % in FreeRTOS and the heap, 7 % in the file manager's `strcasecmp`; our three libraries had no measurable share.
  ue2emu (no memory latency) opens the menu in 710 ms. So the board's seconds are memory latency, which a data cache removes.

## 2. What is built

Taken from TRX64U-MultiCore at `e4f99648` (the base that `trx64u` is built on), logic unchanged, comments moved to our paths:

| File | What |
|---|---|
| `rv_dcache.vhd` | write-through, direct-mapped data cache; stores do not allocate; a hit answers in the next clock; cacheable below byte 0xEA0000 |
| `rv_icache.vhd` | Gideon's `icache.vhd` with the size and the line as generics; tag RAM in LUTs |
| `rvlite_wrapper_dc.vhd` | Gideon's wrapper (entity `rvlite_wrapper`, `dbg_pc`) with both caches; the new ports have defaults |
| `cache_cfg_pkg.vhd` | sizes: data and instruction cache 2^13 bytes, lines of 2^2 words |
| `ddr2_port.v` | `mig_arb`: `rline0` (client 0's whole 128-bit read word) and `ext_wr_tgl`; `cpu_mem_bridge`: `line`, `line_adr` |
| `ultimate_sys.vhd` | ports `dc_ext_tgl`, `dc_line`, `dc_line_adr`; the snoop from the arbitrated memory bus |
| `top.v` | the wiring between `mig_arb`, `cpu_mem_bridge` and `ultimate_sys` |
| `build.tcl` | the four files in place of Gideon's `rvlite_wrapper.vhd` (his `icache.vhd` stays listed, unused) |

`dc_ctl` is not wired: the wrapper's default `X"0007"` (cache, snoop and walk on) applies. The counters (`dc_stat`,
`dc_data`) are left open; `rv_dbg.v` does not read `rv_tap[127:104]`. The `ui_clk` → `cpu_clk` crossing of `rline0` is
taken in the toggle handshake that already carries `rdata`; the two clocks are asynchronous groups in `pins.xdc`.

## 3. Who else writes DDR2, and how the cache learns of it

Path 1 (exact, `cpu_clk`): a write on the arbitrated memory bus of `ultimate_sys` whose tag is not the CPU's
(0x20, 0x21) clears that line. Path 2 (`ui_clk`): `mig_arb` toggles `ext_wr_tgl` for every write that is not client 0's
below word address 0x750000 (byte 0xEA0000); the cache then clears all its tags (1,024 clocks) and bypasses meanwhile.

| Writer | Where (byte) | Path |
|---|---|---|
| WiFi UART DMA (tags 0x1E, 0x1F), Ethernet (0x12), USB (0x14) | app buffers | 1 |
| drive A (`drive_mem`, client 2) | `g_ram_base` 0xEE0000; any write below the window | above / 2 |
| REU, JTAG window (client 1) | REU at 0x1000000; JTAG anywhere | above / 2 |
| `cart_fill` | slot ROM 0xF00000, cart RAM 0xEF0000, block ROM 0x1000000, legacy 0x3C00000 | above |
| ring engine | areas 0x5C00000-0x7FFFFFF; its bounce buffers in the app heap | above / 2 |
| still capture (video client) | `trx64g`: the app heap (`trx_video.cc` `alloc_aligned`) | 2 |

The CPU's own stores are write-through. After a CPU reset the data cache walks once (all tags cleared).

## 4. Software

- `trx_video.cc` `buf_done` relied on the load waiting for posted stores. With a cached line it does not wait. Safe
  anyway: `PX_FRONT`/`PX_BACK` take the buffer at the next frame start (`pix_layer.v` `flip_req`, `la`), and the buffer
  is read line by line; the stores, one open at a time, land in microseconds. Comment corrected (submodule branch `u19`).
- MultiCore's base has the same pattern (`trx_lv_port.cc stores_done`, its comment says "no data cache"); it runs
  with this cache on their host already, and on `c64_basic` the flip is the same `pix_layer`. Their file, their repo.

## 5. Limits

- **Still capture keeps the walk running** while the C64 runs in `trx64g` (54,000 bytes per frame into the app heap):
  the data cache bypasses then. Paused (menu), capture stops and the cache works. Slower, not wrong.
- **The instruction cache is not cleared by a CPU reset** (as Gideon's was not). Loading a different app over JTAG
  into a running bitstream can execute stale lines; load the bitstream again first. The user paths reconfigure the
  FPGA: a `.cor` start, the updater's restart (IPROG), power-up.

## 6. Checks

- Benches (GHDL, `trx64-sim-env:4`): `dcache` (new, MultiCore's T133 bench: S1-S9 and random, 3 seeds),
  `flash_boot` and `flash_gate` (the real CPU with the new wrapper), `ultsys` (stub with the new ports).
- Lint: `ci/lint.sh` green; yosys reads `top.v` without implicit nets.
- CI (`54dc9f9e`, every stage green): Vivado `c64_basic` 27,593 LUT (84.6 %; before 27,120), 61 of 75 block RAM tiles
  (before 58), WNS +0.104 ns, WHS +0.006 ns. Update files `trx64_update_54dc9f9e_20261123.cfw`.
- Board: `trx64u_u19.cor` (Alex, 2026-11-23): *"das Menu ist jetzt akzeptabel. Bitstream hat gepasst ... das kann so bleiben"*.
  Open: the `trx64g` `.cfw` on this bitstream (menu, browse, a disk, a cart).

## History

- 2026-11-23: built; cause found the same day (U18 board run, ue2emu sampling, MultiCore's menusim). CI green;
  `trx64u_u19.cor` packed on branch `u18` (its libraries at `4f3ba9f5`, this bitstream) for the board test.
- 2026-11-23: board: `trx64u`'s menu acceptable with this bitstream, it stays (Alex).
- 2026-11-23: merged into `main` (fast-forward; submodule `trx64/main` at `45f32fb0`).
