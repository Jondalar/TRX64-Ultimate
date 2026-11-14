# U16 — More HDMI modes: 576p, 480p, 720p and 800 x 600 beside 1080p

**Status:** SPEC (2026-11-14). Nothing built.
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

**D3 — The picture keeps today's proportions, as large as the mode allows** (Claude). Per mode the VIC window
(400 x 270, 60 Hz 400 x 240) is scaled by the largest factor that fits the active area with the same aspect as
today's 1080p picture (1600 x 1080), centred; factors need not be whole numbers (the 60 Hz 1080p picture is 4.5x
vertically already). Examples: 720p 1067 x 720, 576p 720 x 486, 480p 640 x 432, 800 x 600 800 x 540. A "Scale"
setting (Integer / Fit / Fill, MultiCore T107 D8) is **not** part of U16; it is [U17](../todo/U17.md).

**D4 — The request register is MultiCore's** (Claude). `mode_req.v` as in MultiCore's host: write 0x4F00 of the
application's overlay window = the index; read 0x4F04-0x4F09 = magic 0x48, version 1, flags, the mode running, the
request counters. This design is a C64-type core (`c64_type = 1`): the family part of a request is replaced by the
System Mode's, and only a locked row is taken; anything else is refused and nothing changes. An accepted request
restarts the CDCE sequence, the transmitters and the pixel MMCM, as MultiCore's does.

**D5 — Power-up is 1080p, the application sets the mode** (Claude). The gateware starts as today (index 2 or 8).
- `trx64u` runs MultiCore's generic coreapp, which already writes 0x4F00 from its "Picture" setting, with the
  host's mode as default (T105 D3, T107 D11c). Nothing to build in TRX64-Ultimate beyond the register; MultiCore
  has to let its coreapp offer the modes for `trx64u` (its descriptor).
- `trx64g` runs our application: it writes 0x4F00 from `CFG_HDMI_RESOLUTION` at start and on every change. 1024 x
  768 and 1280 x 1024 are taken as 800 x 600 in the 60 Hz family; in the 50 Hz family every PC mode is taken as
  576p (no 50 Hz PC row exists). Gideon's timing and scaler registers stay unread.

**D6 — Every layer follows the active area** (Claude). `hdmi_timing` gives the counters, the active area's size and
origin; `vic_scaler`, `overlay.v`, `c64_text.v` and `pix_layer.v` take them instead of their 1080p constants:
the menu box centred on the active area with the largest character scale that fits, the freeze text in the scaled
picture's window, the pixel layer's positions relative to the active area.

**D7 — The hand-over contract gains one line** (both sides): `trx64u` and `trx64g` take the HDMI mode request at
0x4F00 like MultiCore's host, for locked rows only. Changed only with MultiCore's agreement.

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
  frame's clock count in every row, the window lands inside the active area, the ring is never overtaken.
- iverilog: `mode_req` (refusal, family replacement, restart sequence), from MultiCore's bench if it has one.
- Vivado: the c64u build routes and meets timing (it stands at 81 % LUT, 99.89 % slices).
- Board (Alex): each mode on a TV and on the projector; the menu, the freeze text and a pixel-layer picture in each.

## 5. Status and history

- **2026-11-14, spec.** Written after MultiCore's request and Alex's go-ahead; nothing built.
