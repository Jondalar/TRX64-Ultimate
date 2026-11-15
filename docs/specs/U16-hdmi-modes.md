# U16 — More HDMI modes (576p, 480p, 720p and 800 x 600 beside 1080p) and the picture's scale (Integer / Fit / Fill)

**Status:** DONE (2026-11-15, Alex). Gateware, the `trx64g` application part and the boot splash in every row built; every bench
and the app's host tests pass; CI (Vivado, the tier 1 benches, the app) green; the board test passed on a monitor at 50 Hz (§5).
Not tested: the 60 Hz rows and a projector. Left open with the done: D7's contract line, the CI gaps and the 576p shape (§5).
**Repo:** TRX64-Ultimate (gateware `designs/c64_basic/`, application `software/ultimate-app` for `trx64g`).
**Number:** U16 (registry: the TODO table in `TODO.md`).
**Milestone:** none ([ADR-0066](../adr/0066-milestones-end-with-the-multicore-split.md)).
**Origin:** MultiCore (cross-session, 2026-11-14), for Alex: a projector reports 1080p in its EDID and cannot show
it. MultiCore's host and its cores run it at 720p; `trx64u` and `trx64g` stay at 1080p50 and give no picture.
Gideon's firmware 3.15 works on it at "HDMI Scan Resolution" = "HD (720p)". Alex, 2026-11-14: *"mach einfach was,
damit es mehr Auflösungen gibt"* — the decisions below were left to Claude and are marked so.

---

## 1. What is there

- `vic_scaler.v` puts out one fixed mode: 1080p with 2640 x 1125 clocks per frame (1080p50 blanking) or 2200 x 1125
  in the 60 Hz System Modes (T40, `v60`), a 400 x 270 (400 x 240) VIC window at 4x horizontally and 4x (4.5x)
  vertically, from a ring of 64 VIC lines (`ring`, 32K x 4 bit block RAM). There is no frame buffer.
- That works because the HDMI frame **is** the VIC frame: `cdce_i2c.v` sets the CDCEL824 to 55/14 (990/263) of the
  37.89 MHz it gets (125/104 of dot4x), so one HDMI frame lasts exactly 63 x 312 (65 x 263) C64 cycles.
- The pixel clock is the GTP reference (`hdmi_gtp.v`, two pixels per 20-bit word, 1.489 Gb/s per lane).
- Every layer over the picture is placed for 1920 x 1080: `overlay.v` (the app's menu, centred on the frame),
  `c64_text.v` (the freeze text, window from cx 1088), `pix_layer.v` (the app's pictures).
- Gideon's application (`trx64g`) writes its own timing and scaler registers for the mode it believes it drives
  (`u64/hdmi_scan.cc` `SetVideoMode1080p`, from `CFG_HDMI_RESOLUTION`); this gateware does not read them.
- MultiCore's host (which grew out of this design, T94 D1) has the general version: `host/hdmi_modes.vh` (table, from
  `tools/hdmi_modes.py`), `host/hdmi_timing.v`, `host/cdce_mode.v`, `host/mode_req.v` (overlay window 0x4F00 / 0x4F04),
  pixel repetition ("gear") in the transmitter path, and a layer that scales to any output size (`host/pix_layer.v`,
  T164 WP5).

## 2. Decisions

**D1 — The HDMI frame stays the VIC frame (locked), no frame buffer** (Claude, recommended to Alex 2026-11-14, then
"mach einfach"). Every mode gets a CDCE setting that makes its frame exactly one VIC frame, as 1080p has today. No
memory, no added latency, no doubled or dropped frames; the refresh is the C64's (50.12 / 59.83 Hz), as at 1080p
now. A frame buffer (MultiCore's way for its Amiga and SNES) would cost ~12 RAMB36 or DDR2 bandwidth beside the REU,
in a design at 99.9 % of its slices.

**D2 — The modes** (Claude). Which rows lock was computed with MultiCore's `lock_for` (`tools/hdmi_modes.py`, exact
rational arithmetic on the CDCE's N, M, P, Q, R):

| Index (family x 6 + hdmi) | Mode | Timing | Locked |
|---|---|---|---|
| 0 | 576p at 50.12 Hz | 720 x 576, 864 x 625 | yes |
| 1 | 720p at 50.12 Hz | 1280 x 720, 1980 x 750 | yes |
| 2 | 1080p at 50.12 Hz | 1920 x 1080, 2640 x 1125 (today) | yes |
| 6 | 480p at 59.83 Hz | 640 x 480, **800 x 526** | yes; CEA has 525 lines, which does not lock |
| 7 | 720p at 59.83 Hz | 1280 x 720, 1650 x 750 | yes |
| 8 | 1080p at 59.83 Hz | 1920 x 1080, 2200 x 1125 (today) | yes |
| 9 | 800 x 600 at 59.83 Hz | 1052 x 650 | yes |

1024 x 768 and 1280 x 1024 do not lock and are not built. The family (50 / 60 Hz) follows the System Mode, as today.
The index scheme and the hdmi numbers are MultiCore's and Gideon's (`scan_modes[]`: SD, HD, FullHD, 800 x 600,
1024 x 768, 1280 x 1024), so one number means the same mode on all three. Row 6 differs from MultiCore's table (525
lines, not locked there): it is the one row this design adds.

**D3 — The picture's scale is a setting: Integer, Fit, Fill; power-up and default Fit** (Alex, 2026-11-14: U17 folded into
U16, "Ja ich will U17 gleich dazu"). The VIC window is 400 x 270 (60 Hz 400 x 240); per mode and scale:
- **Fit:** the largest factor that fits the active area with the same aspect as today's 1080p picture (1600 x 1080), centred; factors
  need not be whole numbers (the 60 Hz 1080p picture is 4.5x vertically already). Examples: 720p 1067 x 720, 576p 720 x 486, 480p
  640 x 432, 800 x 600 800 x 540.
- **Integer:** the largest whole factor k, the same on both axes, whose picture fits the active area, centred, black border (1080p
  50 Hz: 4 -> 1600 x 1080; 1080p 60 Hz: 4 -> 1600 x 960; 720p 50 Hz: 2 -> 800 x 540). **Where k would be 1 (576p, 480p) Integer is Fit**
  (Alex, 2026-11-14, "a"): a window read at one output line per VIC line gains 135 (120) VIC lines on the write in one frame and
  overtakes the 64-line ring (§5).
- **Fill:** the whole active area: x factor hact / 400, y factor vact / 270 (60 Hz: vact / 240).

The scale is written at 0x4F01 (D4) and taken at a frame start, without the restart sequence (no CDCE, transmitter or MMCM restart).
Each row and scale has its own align line (`hm_align(m, sc)`, the smallest at which every window pixel is read at least 3 lines after
the VIC wrote it; Claude, 2026-11-14), so a scale change re-aligns the output frame once: one disturbed frame, the picture keeps its
place. Where the new scale's align line is the old one the lock holds and nothing is disturbed (vic_scaler.v). 1080p at Fit keeps
today's lines (20 and 35); 1080p60 Integer aligns at 37 (the same picture place, 2 lines more delay).

**D4 — The request register is MultiCore's** (Claude). `mode_req.v` as in MultiCore's host: write 0x4F00 of the
application's overlay window = the index; read 0x4F04-0x4F09 = magic 0x48, version 1, flags, the mode running, the
request counters. U16 adds (D3): write 0x4F01 = the scale (0 Integer, 1 Fit, 2 Fill; any other value is refused and nothing
changes), read 0x4F0A = the scale in use (power-up 1, Fit; the version at 0x4F05 stays 1). Both are free in both designs (checked 2026-11-14: overlay.v decodes no write in 0x4xxx
and reads 0 there; MultiCore's mode_req.v writes only 0x4F00 and reads 0x4F04-0x4F09, its hdmi_out.v read mux takes only those, its
applications define only 0x4F00 and 0x4F04-0x4F09 in `app/video/videomode.h`). This design is a C64-type core (`c64_type = 1`): the family part of a request is replaced by the
System Mode's, and only a locked row is taken; anything else is refused and nothing changes. An accepted request
restarts the CDCE sequence, the transmitters and the pixel MMCM, as MultiCore's does. A System Mode switch moves the row to
the other family; where the request's row does not exist there (800 x 600 has no 50 Hz row) the family's 576p row is taken, as the
application maps it (D5; Claude, 2026-11-15: MultiCore's module has only the two 1080p rows locked and needs no such rule, here row 3
would have reached the synthesizer and the timing).

**D5 — Power-up is 1080p, the application sets the mode** (Claude). The gateware starts as today (index 2 or 8).
- `trx64u` runs MultiCore's generic coreapp, which already writes 0x4F00 from its "Picture" setting, with the
  host's mode as default (T105 D3, T107 D11c). Nothing to build in TRX64-Ultimate beyond the register; MultiCore
  has to let its coreapp offer the modes for `trx64u` (its descriptor) and write 0x4F01 for the scale (MultiCore's side; nothing of
  it is built here).
- `trx64g` runs our application: it writes 0x4F00 from `CFG_HDMI_RESOLUTION` at start and on every change; its default becomes
  FullHD (1080p) in our fork (Alex, 2026-11-14, "a"; Gideon's is SD), so a machine that never set it keeps 1080p. A machine whose
  settings file already holds SD comes up at 576p after the update until the user picks FullHD (accepted, Alex, 2026-11-14, "a"; no
  migration): the `.cfg` beside the `.cor` stores every item with its value, changed or not (`components/config.cc`
  `ConfigItem::pack`, `save_core_cfg`), so a new default reaches only a file written as defaults only, or none. A user-visible
  change: the app commit says so. 1024 x
  768 and 1280 x 1024 are taken as 800 x 600 in the 60 Hz family; in the 50 Hz family every PC mode is taken as
  576p (no 50 Hz PC row exists). A new config item "HDMI Picture Scale" (Integer / Fit / Fill, default Fit) beside "HDMI Scan
  Resolution" is written to 0x4F01 at start and on every change. Gideon's timing and scaler registers stay unread.

**D6 — Every layer follows the active area** (Claude). `hdmi_timing` gives the counters, the active area's size and
origin; `vic_scaler`, `overlay.v`, `c64_text.v` and `pix_layer.v` take them instead of their 1080p constants:
the menu box centred on the active area with the largest character scale that fits, the freeze text in the scaled
picture's window, the pixel layer's positions relative to the active area. The freeze still the app draws on the pixel layer
(`trx_video.cc`) is placed and sized from the running mode and scale (0x4F07, 0x4F0A), so it lies under the picture in every row and
scale (Claude, 2026-11-14). The boot splash (`trx_ui.cc`, T24 §8) covers the running row's active area, its picture shrunk while it
is decoded to the largest size with its 1080p shape, centred (Alex, 2026-11-15, "a"; §5).

**D8 — The encoder is MultiCore's fork** (Claude, 2026-11-14). `hdmi_enc/` (pixel enable, the frame at run time). Two behaviour changes
against the vendor encoder this design used, accepted: the AVI InfoFrame's picture aspect at 1080p is 16:9 instead of "no data"
(`hm_aspect`), and three data island packets per line instead of seven (MAX_PACKETS 3: 576p and 480p have only 144 / 160 clocks of
blanking).

**D7 — The hand-over contract gains one line** (both sides): `trx64u` and `trx64g` take the HDMI mode request at
0x4F00 like MultiCore's host, for locked rows only, and the scale at 0x4F01. Changed only with MultiCore's agreement.

## 3. Scenarios

```gherkin
Scenario: A screen that cannot show 1080p
  Given the projector that shows MultiCore at 720p and nothing from trx64g
  When  trx64g starts with "HDMI Scan Resolution" = "HD (720p)"
  Then  the C64 picture and the menu appear at 720p

Scenario: Each mode
  Given each row of D2, in its family
  When  it is requested at 0x4F00
  Then  0x4F07 reads it, the screen shows a stable picture, and the HDMI frame lasts exactly one VIC frame

Scenario: A mode that does not lock
  Given index 4 (1024 x 768) or a 50 Hz request for a 60 Hz row
  When  it is written to 0x4F00
  Then  the refused flag is set and the picture does not change

Scenario: Nothing that worked stops working
  Given 1080p at 50 and 60 Hz
  Then  the picture, the menu, the freeze text and the pixel layer are where they are today
```

## 4. Tests

- iverilog: `hdmi_timing` per row (counts, sync, active area) and the scaler: the output frame has exactly the VIC
  frame's clock count in every row and every scale (Integer, Fit, Fill), the window lands inside the active area, the ring is never
  overtaken.
- iverilog: `mode_req` (refusal, family replacement, restart sequence), from MultiCore's bench if it has one.
- Vivado: the c64u build routes and meets timing (CI run 1924: WNS +0.109 ns, 83.3 % LUT, 8,147 of 8,150 slices; §5).
- Board (Alex): each mode on a TV and on the projector; the boot splash, the menu, the freeze text and a pixel-layer picture in each;
  a System Mode switch (API) while the splash shows.
  Run 2026-11-15 on a Samsung monitor at 50 Hz; there is no projector here (§5).

## 5. Status and history

- **2026-11-14, spec.** Written after MultiCore's request and Alex's go-ahead; nothing built.
- **2026-11-14: Alex folded U17 (scale) into U16.** D3 is the Scale setting (Integer / Fit / Fill, default Fit), D4 gains 0x4F01 /
  0x4F0A, D5 the app's "HDMI Picture Scale" item and MultiCore's side for `trx64u`; `docs/todo/U17.md` and its TODO row are gone.
- **2026-11-14, first step (agent), blocked on one question.** In: `tools/hdmi_modes.py` (ported from MultiCore's, host edition;
  `table`, `gen`, `check`, `selftest`: 17 of 17 mutations caught) and the generated `designs/c64_basic/hdmi_modes.vh`: the 7 rows of
  D2, every one locked (lock bytes proven exactly, checked against the chip rules for both System Modes of a family; row 6 has vfp 11,
  the added line is a front-porch line), the pixel rate for both System Modes of a family (`hm_rate`, `hm_rate_alt`; the 1080p rows
  reproduce top.v's four rates and cdce_i2c.v's two blobs), per row the frame align line (`hm_align`), per row and scale the scaler's
  steps and window and the text layer's area (`hm_geom`). `hdmi_timing.v` (MultiCore's, with htot_m1 / vtot_m1 out) and `hdmi_enc/`
  (MultiCore's encoder fork, its diffs regenerated against cores/icepi/hdmi) are in, not wired. `check` fails on the question below.
  **The question (stops the work):** Integer at 576p and 480p is k = 1 (720 / 400 and 640 / 400 are below 2): a 400 x 270 (400 x 240)
  window read in 270 (240) output lines, which last 135 (120) VIC lines. The read gains 135 (120) lines on the VIC in one frame, so it
  must start that far behind the write, and the scaler's 64-line ring (D1: no frame buffer) is overtaken by 74 (60) lines
  (`tools/hdmi_modes.py check`: "576p/480p 50 Hz Integer: the ring is overtaken ... -74.28 lines", 60 Hz -59.89). A ring large enough
  holds 256 lines (16 RAMB36 instead of 4). Every other row and scale fits the 64-line ring with margins of at least 3 lines, but only
  with an align line per row and scale (a scale change then re-aligns the output frame once; the tool still computes one line per row,
  which fails for 576p and 480p while Integer is in the set, and moves 1080p60 from 35 to 37). Open for Alex: what Integer shows at
  576p and 480p.
  Choices taken so far (Claude, for review): the menu box takes MultiCore's rule (largest character scale that fits the active area,
  at most 2 x SY wide), which keeps 1080p with the app's 1080p settings at 960 x 575 as today; the freeze text follows the picture
  horizontally and, vertically, the factor the 50 Hz window has at that scale (1080p60 Fit stays 4 x 4 at (1088, 185) as today); the
  align line is the smallest at which every window pixel is read 3 lines after the VIC wrote it (1080p rows at Fit: 20 and 35 as
  before). Found while reading: Gideon's default for "HDMI Scan Resolution" is SD (`u64_config.cc`), so D5 puts a machine with default
  settings on 576p; the freeze still the app draws on the pixel layer (`trx_video.cc`: 800 x 540 at scale 2 from (160, 0)) is placed
  for 1080p and is not in D5 (a second question once the first is answered).
- **2026-11-14, the answers (Alex, via the coordinator) and the choices taken with them (Claude).** Integer at 576p and 480p is Fit
  (Alex, "a"; D3). The align line is one per row and scale (`hm_align(m, sc)`): with Integer gone from 576p / 480p the tool finds every
  row and scale inside the ring with margins of at least 3 lines (576p 50 Hz Fit aligns at line 24, 480p at 47; 1080p Fit 20 and 35 as
  shipped, 1080p60 Integer 37); a scale change re-aligns the frame once (D3). The freeze still is in scope (D6), the encoder's two
  behaviour changes are accepted (D8). `tools/hdmi_modes.py check` passes, `selftest` 17 of 17; `hdmi_modes.vh` regenerated. trx64g's
  default for "HDMI Scan Resolution" becomes FullHD (Alex, "a"; D5).
- **2026-11-14, trx64g's default (Alex, via the coordinator).** "HDMI Scan Resolution" defaults to FullHD in our fork; machines
  whose `.cfg` holds SD get 576p after the update until the user changes it, accepted, no migration (D5, with the reason).
- **2026-11-15, the gateware (agent).** In: `cdce_mode.v` (replaces `cdce_i2c.v`, gone: the row's lock bytes from `hm_cdce_lock`;
  restarted by the lock loss of a System Mode switch and by `mode_req`'s `vm_rst`), `mode_req.v` (MultiCore's with `c64_type` = 1
  fixed; 0x4F01 / 0x4F0A, the scale taken at a frame start, power-up Fit), `tmds_rep.v` (MultiCore's), `vic_scaler.v` (rewritten:
  on `hdmi_timing`'s counters with `ce`, one accumulator per axis from `hm_geom`, the align line per row and scale handed over with
  the VIC side's toggle, `align` held until a `ce`, the lock dropped only where the align line changes), `overlay.v` (MultiCore's:
  the box centred on the active area), `c64_text.v` (`ce`, fractional steps, the area from `hm_geom`), `pix_layer.v` (`ce`,
  positions from the active origin), `top.v` (the wiring; encoder rate `hm_rate` / `hm_rate_alt` by System Mode, `hm_aspect`,
  `tmds_rep` to the GTPs, the T40 measure from the timing's vsync), `build.tcl` (`hdmi_enc/` and the three vendor files, the new
  modules); stale 1080p comments fixed in `hdmi_gtp.v`, `build.tcl`, `pins.xdc`, `top.v`, `pix_layer.v`, `c64_text.v`.
  **Found and fixed:** the ring slot was the raster line, so at 60 Hz the window's rows 225-231 (lines 256-262) shared their slots
  with the next frame's lines 0-6, written 7 lines later, before the scaler read them: 1080p60 as shipped showed lines 0-6 of the
  next frame in the bottom border rows (invisible on a plain border; the new bench caught it). The slot is now the window row
  (`line - Y0` modulo the frame), the tool's ring model follows (50 Hz rows 256-269 keep their slot 56 lines, every other row 64;
  `check` still passes, smallest ring margin 14.3 lines). Also found: with 800 x 600 requested, a switch to a 50 Hz System Mode
  moved the row to 3, which does not exist; `mode_req` now takes 576p there (D4).
  Benches (iverilog): `sim/scaler/run.sh` (7 rows x 3 scales, two frames each: clocks and time per frame = the VIC frame, the window
  in place, every pixel, every ring read; 4 scale changes, palette and scan lines; 3 controls caught), `tb_hdmi_timing.v` (7 rows,
  9 refused values, align on every row), `tb_mode_req.v` (path in both families, CDCE bytes of every row, the scale register),
  `tb_tmds_rep.v`, `sim/text/run.sh` (the freeze text's geometry, 7 x 3, control caught), `sim/ntsc/run.sh` (mode_ctl with
  `mode_req` + `cdce_mode`), `overlay_sim.py` (its model brought to the RTL, SX / SY and the dimmed box: 0 pixels off in the three
  menu frames), `pixlayer/run.sh` and `run_still.sh`: all PASS. The Verilator tops (`statebus`, `ring_core`, tier 2) need GHDL in the
  CI container and were not run here; their `c64_text` instances with the new ports elaborate (iverilog, a wrong port name as
  control). Not run: Vivado, the board.
- **2026-11-15, the application (agent).** `software/ultimate-app` branch `u16` (not pushed). In: "HDMI Scan Resolution" defaults to
  FullHD (`u64_config.cc`; a stored SD stays, D5); the new item "HDMI Picture Scale" (Integer / Fit / Fill, default Fit, id 0xAE, free on
  the page and in every store table) after it in Video Configuration, hidden on a bitstream without the request; `trx_io_hdmi_modes()`
  (`trx_io.cc`: the TRX2 block, then magic 0x48 and version 1 at 0x4F04, so Gideon's HDMI registers in that window are never touched);
  the request (D5) at start and on every change of the resolution or the System Mode, the scale at start and on its change (a change
  hook). Mapping and the freeze still's place are pure functions over a table `tools/hdmi_modes.py gen` now also writes into the app
  (`software/io/c64/trx_hdmi_modes.h`, hm_geom's numbers; `check` compares it when the submodule is there): `trx_hdmi.h`.
  The freeze still (`trx_video.cc`, D6) takes the row and scale from 0x4F07 / 0x4F0A (before U16: 1080p Fit), puts the layer on the
  scaler's window (scale 1 up to 960 output pixels wide, else 2 at half size; PX_POS = the window's origin), samples each layer pixel
  at the VIC pixel the scaler shows at its first output pixel (`floor(x hn / hd)`, `floor(y vn / vd)`), and draws the software text
  cells where c64_text.v draws them and Timewarp's bar from a layout scaled to the layer (800 x 540 gives the old constants exactly). A
  row or scale change while the freeze is up re-lays the layer (off, two frame starts, new place, still drawn again). Buffers: in the
  FreeRTOS heap (heap_4, `configTOTAL_HEAP_SIZE` 0x7A0000 = 7,995,392 bytes, the app's `malloc` wrapped into it), allocated at the
  first freeze and kept, sized by the place and only grown: 1080p Fit 864,000 bytes each as before, 1080p Fill 1,036,800 (the largest;
  1080p60 Integer 768,000, 720p Fit 385,920, 800 x 600 Fill 960,000, 576p Fill 829,440).
  **Found while reading.** (a) mode_req checks a request against the family that runs when it arrives, and mode_ctl moves the family
  only after halting the machine and reprogramming the clocks: a 800 x 600 request written right after a switch to 60 Hz would be
  refused. The app waits (up to 2 s) until 0x4F07 is in the System Mode's family. (b) An accepted request restarts the synthesizer,
  the transmitters and the pixel clock even for the row that runs, and the app's effectuate runs on every C64 reset: the app writes
  only when the running row differs (the default at power-up writes nothing). (c) Gideon's path stays: `SetVideoMode1080p` is harmless
  on this bitstream (app_i2c.v drops writes to the synthesizer, 0xC8; overlay.v ignores 0x4000-0x401D; the MMCM writes at 0x10200000
  hit no region of ultimate_sys.vhd) and still needed on Gideon's. (d) The menu's character settings (`DetermineOverlaySettings`) are
  now for the mode that runs: a 1280 x 1024 request at 50 Hz runs 576p, where its 600-line box would not fit.
  **Tests.** Host test `make -C target/pc/linux/trxhdmitest test` (in `make host_tests`): 369 checks, the mapping for 6 settings x 2
  families, the table's 21 entries, for every row and scale the plan within pix_layer.v's limits and over the window inside the active
  area, every layer column and line sampled as a model of vic_scaler.v's accumulators shows it, every VIC pixel / line and text pixel /
  line on the layer, 1080p Fit equal to the renderer before U16; controls: 3 of 4 mutations caught (the fourth, a floor for the
  height, is equal on this table: every height divides). Build: `make -C target/u64ii/riscv/ultimate` in `u64-build-env` (Apple
  container): no warning; `app_space.py measure`: 50T 1641.4 of 1920 KiB, 100T 1641.4 of 1792 KiB; 180.0 KiB above the image (gate
  128). ue2emu 0.7.3 (`--board c64u --trx-io --caps 25E40202`, Interface Type Freeze; its C64 core has no mode request, so the
  fallback path): the freeze still lies where the live picture was (window 160..1760, screen area 140..940 in both frames), 2 buffers
  of 864,000 bytes, heap free 2,738,688 after them; the splash had 2 x 1,036,800 (adjacent) and 2,756,032 free at boot. heap_4 here
  has no largest-block figure: whether a grow to 2 x 1,036,800 finds its holes after the first freeze is for the board's log line
  (1080p Fill); if it does not, the freeze runs with one buffer or none, as before. Not run: the request, the scale and the re-lay
  (they need the board).
  **Open for Alex (not in this spec):** the LVGL boot splash (`trx_ui.cc`, `trx_lv_port.h`) and the apps' LVGL UI draw a 960 x 540
  canvas at scale 2, the whole 1080p frame; in every other row it is cut (720p shows its top left 640 x 360 of 960 x 540).
  (Answered and built, next entry; the splash is the port's only caller in the app.)
- **2026-11-15, the boot splash in every row (Alex, "a"; agent).** Alex's answer: the splash picture is shrunk while it is decoded to
  fit the active area and centred, following the row as the freeze still does (D6). App `u16` `18af6de9`. `trx_splash.h` (new, pure):
  the canvas is the running row's active area at the smallest layer scale whose width fits (`trx_layer_scale`, now shared with the
  freeze still's plan) from the active origin: 960 x 540 at 2 (1080p, as before), 640 x 360 at 2 (720p), 720 x 576, 640 x 480 and
  800 x 600 at 1. The picture's 960 x 540 frame (`trx_splash.c`) is drawn at the largest size with its 1080p shape on the screen,
  centred on black. The shape of an output pixel is the row's AVI aspect over its active area, now in the app's table (`asp`,
  `tools/hdmi_modes.py` `ch()`; `hdmi_modes.vh` unchanged): only 576p (720 x 576 named 4:3) has pixels 16/15 wide, so it gets
  720 x 432 at (0, 72); 480p 640 x 360 at (0, 60), 800 x 600 800 x 450 at (0, 75), 720p 640 x 360, 1080p the frame itself. Found while
  reading: the scaler's Fit does not correct this for the C64 picture (`steps()` keeps 1600 : 1080 in output pixels), so at 576p the
  C64 picture is 16/15 wider on the screen than at 1080p; not changed. Each canvas pixel shows the frame pixel at its centre: the
  colours, every stripe and gap (3 pixels and more) kept; single-pixel tips of the rounded bar ends drop out (9 at 720p and 480p, 8 at
  800 x 600, none at 576p). No new asset. The version line keeps unscii 8 (now set on the label) at its own size, 8 x 9 output pixels
  at scale 1, 16 x 18 at 2; its longest form (47 characters) fits the narrowest canvas (640) and lies on black in every row. The port
  (`trx_lv_port`) takes the display size, stride, scale and PX_POS from the canvas at run time; its two buffers stay 1,036,800 bytes
  each, which the 1080p canvas fills exactly (800 x 600: 960,000, 576p 829,440, 480p 614,400, 720p 460,800; the rest of a buffer is
  unused while the splash shows, all of it is freed when it ends). `trx_lv_resize` moves the display: layer off, two frame starts
  (PX_STATUS count, at most 100 ms), the new size and stride, painted again, shown by the next flush.
  **Ordering (from the code).** The row asked for at boot runs before the splash exists: "U64 Config" (init function 1) constructs
  U64Config, whose first `effectuate_settings` writes 0x4F00 and waits for the family (up to 2 s) and for the row (up to 1 s) in the
  init task; "TRX UI" (91) creates the splash task, which reads 0x4F07 then. A row can still change while the splash shows: a System
  Mode switch from the API or a state (`u64_set_system_mode`, any task), or a boot request slower than those waits. So the task reads
  0x4F07 in its wait loop (every 20-50 ms) and moves the splash when its canvas changes (720p and 1080p keep theirs across the
  families); during the 300 ms fade a change ends the splash at once. A freeze takes the layer as before (`trx_lv_yield_layer`).
  **Tests.** Host test (`make host_tests`): 489 checks; per row the canvas and picture against numbers worked out by hand from the
  active area and the CEA / VESA aspect, the layout rules (pix_layer.v's limits, exactly the active area, centred, one side full, the
  16:9 shape to half a pixel, the longest version line inside), the paint against a plain model pixel for pixel (the asset, four
  made-up pictures, every row and two odd places, one drawn larger), only the palette's colours, every stripe and gap kept, the
  version line on black, 1080p pixel for pixel the splash before U16. Controls in the test, all caught: the 960 x 540 canvas of before
  in the 5 other rows, 576p with square pixels, a picture 2 lines low, a canvas a pixel short, a stride of 1608, one pixel changed, a
  quarter-size shrink that loses stripes; 7 mutations of the header caught. Build (`u64-build-env`): no warning; `app_space.py
  measure` 1645.6 KiB on 50T (of 1920) and 100T (of 1792), +4,344 bytes against the same commit before; 175.8 KiB above the image (gate
  128); `app_space_test`, `openapi_check` pass. ue2emu 0.7.3 (`--board c64u --trx-io`, no mode request in its C64 core: the fallback):
  "splash up: row 2, canvas 960 x 540 at scale 2, picture 960 x 540 at (0, 0)", heap free 2,756,032 at boot as before, the screen pixel
  for pixel the base build's except the version line (that build had no git hash). Not run: the splash in the other rows and its move
  (the emulator models the mode request only for MultiCore's host, with that host's layer); they need the board.
- **2026-11-15, CI.** Run 1924 (d48734d8, the gateware): c64u routed, WNS +0.109 ns (before +0.018); Slice LUTs 27,144 of 32,600
  (83.26 %, +691 against c8ceafa3), registers 29,338 (+477), slices 8,147 of 8,150 (99.96 %, 3 left; was 8,141), BRAM 58, DSP 24
  unchanged. Every CI bench passes, the new ones included (`mode_req`, `hdmi_timing`, `tmds_rep`, `text_geom`, `scaler`); the
  Simulations job now takes 34 min (was 9), the scaler bench runs alone as a long bench. Run 1929 (5a54a1d6, the app part): green, the
  app built, bitstream and benches from the cache. The splash commit (5e3c1c01) rides on the next run.
- **2026-11-15, 576p's pixel shape for the C64 picture (Alex: "Erstmal den wenigsten Aufwand").** The scaler stays as built: at 576p
  (720 x 576, AVI 4:3, pixels 16/15 wide) Fit and Fill do not correct the shape, so the C64 picture is 16/15 wider on the screen than
  at 1080p. A corrected Fit would be 675 x 486: row 0's geometry in `tools/hdmi_modes.py` (the gateware's
  `hdmi_modes.vh` and the app's `trx_hdmi_modes.h` are generated from it), then the benches again.
- **2026-11-15, board (Alex): done.** `u16-422f9dce` (`trx64u.cor`, `trx64g.cor`, put on the SD by MultiCore) on a Samsung monitor,
  System Mode 50 Hz: every "HDMI Scan Resolution" setting tried; the rows the monitor shows look right, a few it does not show (the
  monitor's limits: *"mein Monitor halt einige nicht kann einfach/nicht will"*); the mode changes work. Alex: *"es sieht alles gut
  aus"*, *"Mach das als done bitte"*. Not tested: a projector (none here; MultiCore's projector is the origin of this spec), the
  60 Hz rows (480p, 720p60, 1080p60, 800 x 600). Left open with the done (Alex's call): D7's contract line needs MultiCore's side;
  the app's host tests (`trxhdmitest`, the splash) are not in CI (`ci/app.sh` runs `app_space_test` and `openapi_check`, not `make host_tests`); the tier 2 benches (`statebus`,
  `ring_core`) run only on main or with `[sims]`, so they first run with the merge; 576p keeps the square-pixel Fit (entry above).
