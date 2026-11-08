# U7 — The filter keeps the bits it needs: fractional state and rounded shifts in reDIP's filter

**Status:** spec and gateware 2026-11-07; the Vivado numbers and the board hear test are open.
**Task:** TODO.md U7. **Decision:** [ADR-0065](../adr/0065-sid-filter-fractional-bits.md).
**Depends on:** [T25.4](T25.4-redip-ultisid.md) (reDIP at UltiSID level, 8 SIDs), [T25.5](T25.5-resid8-menus.md) (the reSID8 menus).
**Touches:** [ADR-0001](../adr/0001-reDIP-stays-default-no-copper.md) and [ADR-0032](../adr/0032-sid-dag-lems-redip-instead-of-own-engine.md)
(reDIP is the engine), [ADR-0048](../adr/0048-sid-variant-b-to-m2.md) (variant B),
[LICENSING.md](../../LICENSING.md) (CERN-OHL-S v2 §3.3 b notices on every reDIP file we change).

**Origin.** Lemon64, 2026-11-06, in our own tester thread ("Looking for Testers // MultiCore FPGA
Firmware for Ultimate Elite 2 (and C64U)", page 4), post by Bansai (Tony), one of five notes on
sound:

> "Note also that you want to ask the LLM to make the reDIP-SID filter math wider as it can suffer
> bad filter quantization errors due high resonance, low cutoff, etc. 16b of filter accumulator
> state is just not enough. Listen to the beginning of Meanwhile the Planet and possibly also parts
> of F20."

Nothing of his code or data is used here: the claim was measured against our own RTL and the fix is
ours. The other four notes in that post are **not** part of this task (§8).

Alex, 2026-11-07, on whether this goes in now: *"M1 und M2 gibt es nicht mehr. Durch die Trennung
von MultiCore und hier, mach einfach"*.

## 1. What is wrong

`sid_filter.sv` is three integrators in a ring (its own comment at the state declaration):

    vlp = vlp - w0*vbp
    vbp = vbp - w0*vhp
    vhp = 1/Q*vbp - vlp - vi

and in hardware each step is a 16x16 multiply followed by a shift:

| | code before U7 | what it is |
|---|---|---|
| state | `sid::s16_t v [N]` | vlp, vbp, vhp per SID, **16 bits, whole LSB** |
| step | `dv = 17'(o >>> 17)` | `w0_T_lsl17` is w0*T in Q0.17, so `>>> 17` brings the product back to whole LSB |
| writeback | `v_next = clamp(... 17'(v[N-1]) + dv)` | clamped to the audio range |

`>>>` is an arithmetic shift, so the step is **floored** onto the 16 bit grid. At a low cutoff w0 is
small (the 8580's own `5*fc` is 20 at FC 4), and as soon as `|w0*v| < 1` the step is exactly 0: the
integrator does not creep, it **stops**. High resonance feeds that error back through `1/Q*vbp`.
There are no bits below the audio LSB for the remainder of a step to fall into.

## 2. How it was measured

`designs/c64_basic/sim/redip/tb_filter.cpp`, with `build_filter.sh [F]` and `run_filter.sh [F] [passes]`.
Verilator, one core, about 40 s for the sweep; no Vivado, no board.

It drives `sid_filter` alone (NS = 1, LEGACY_FC = 2) on the schedule `sid_core` gives it, one phi2
pass at a time: a sine of a quarter of the voice DCA range into voice 0 through the filter, LP
selected, master volume 15. `w0_T_lsl17` and `_1_Q_lsl10` are read out of the RTL each pass, so the
reference uses the **same** cutoff and resonance and the comparison isolates arithmetic from the
cutoff curve. Three things run in lockstep:

1. the RTL,
2. an integer model of it — the self check: it must be **bit exact**, otherwise the measurement
   describes something that is not the hardware,
3. the same filter in double precision — the reference. RTL minus reference is the error, reported
   as RMS in LSB of the 16 bit mixer input.

The model also runs with F extra fractional bits, floored and rounded, so the fix is costed in
dB before any RTL is touched.

Sweep: 6581 and 8580 x FC 4, 16, 64, 256, 1024, 2047 x res 0, 8, 15 x 100, 500, 2000 Hz = 108
points, 20000 passes (20 ms) each.

**What the bench does not cover:** only LP is selected, so the s17/sDW wrap of the summed filter
outputs is modelled once at the end instead of per add (identical for one output); one voice; a
sine, not music. The ear test of §7 is not replaced by it.

## 3. The numbers

Worst RMS error over all 108 points, as dBFS of the mixer input (`run_filter.sh 0 20000`):

| state | floored | rounded |
|---|---|---|
| F = 0 (reDIP as it is) | **−32.6** | −34.6 |
| F = 2 | −37.8 | −45.5 |
| F = 4 | −48.9 | −61.6 |
| F = 6 | −61.0 | **−75.4** |
| F = 8 | −73.1 | −90.7 |

Worst point: **8580, FC 4, res 15, 100 Hz** — 768 LSB RMS, and **45 %** of the integrator steps are
swallowed. Relative to the signal that should pass:

| | signal | error (F = 0) | SNR | steps lost |
|---|---|---|---|---|
| 8580 FC 4 res 15, 2 kHz | −61.6 dBFS | −37.8 dBFS | **−23.8 dB** | 45.0 % |
| 8580 FC 16 res 15, 2 kHz | −54.0 | −35.0 | −19.0 | 17.7 % |
| 8580 FC 16 res 8, 500 Hz | −43.1 | −39.1 | −4.0 | 23.7 % |

Negative SNR means the quantisation is **louder than the signal**. 75 of the 108 points are below
40 dB SNR. The worst cases are all the **8580**, the chip most C64U carry; the 6581 is milder only
because its own curve saturates to w0 = 87 for FC 4…256, and it still reaches −32.5 dBFS.

**Two findings decide the shape of the fix:**

1. **Rounding alone is not enough.** 6 dB in the mean, 2 dB at the worst point.
2. **Bits alone make it worse.** A floored step is a systematic negative bias; the hardware's
   stalled step is at least an honest zero, while a wider state lets the bias fire more often and
   integrate into a drift. F = 4 floored is −48.9 dBFS, F = 4 rounded is −61.6 dBFS.

So both, and **F = 6** is the knee: at F = 8 the remaining ~0.3…0.5 LSB is no longer the filter but
the 16 bit output grid itself, so more bits buy nothing that can leave the chip.

## 4. The fix

`parameter FILTER_FRAC` (FF) on `sid_filter`, with `VW = 16 + FF` (state), `DW = 17 + FF` (before the
clamp, and the step) and `OW = 32 + FF` (multiply-add). **FF = 0 is this file's arithmetic bit for
bit** and is what the equivalence benches build.

| | before | with FF |
|---|---|---|
| state | `sid::s16_t v [N]` | `logic signed [VW-1:0] v [N]` |
| step | `dv = 17'(o >>> 17)` | `dv = DW'((o + RND17) >>> 17)` |
| hp | `clamp(o[10+:17])` | `clamp_v(DW'((o + RND10) >>> 10))` |
| clamp | `clamp` (17 → 16, file scope) | `clamp_v` (DW → VW): the same two top bits, one grid wider |
| voice into hp | `-(((32'(v[0]) << 7) + 32'(vi)) << 3)` | `-(((OW'(v[0]) << 7) + (OW'(vi) <<< FF)) << 3)` |
| mixer in | `clamp(17'(vd1 >>> 7) + vf)` | `clamp(17'((DW'(DW'(vd1 >>> 7) <<< FF) + vf + RNDF) >>> FF))` |
| multiplier | `muladd` 16x16→32 | `muladd #(.AW(16), .BW(VW), .OW(OW))` |

`RND17`, `RND10` and `RNDF` are zero at FF = 0, so the rounding arrives with the bits and not
before them.

The direct (unfiltered) path keeps whole LSB and is scaled up into the sum, so the **filter-off and
bypass paths are provably unchanged**: `(x << FF) + 2^(FF-1) >> FF` is `x` for integer `x`. A tune
that never touches FC, RES or FILT sounds exactly as before.

`muladd.sv` (ours, GPL-3, from icepi-zero-c64) takes width parameters with the old 16/16/32 as
defaults, so `sid_voice`'s DCA instance is untouched.

**No format changes anywhere.** The module boundary of `sid_filter` does not move: `audio_o` stays
`sid::s20_t` and means the same thing. The state is not in the capture table either - the chip block
of the ring holds glue, 6510, VIC, CIA 1, CIA 2 and the cartridge (`c64_core.v`, the map at the
capture shadows), no SID - so `TRX_BLOCK_WORDS` stays 58 and `.trxring` / `.c64re` files written
before this change still load (`trx_trxring_check` compares `blockWords` and `frameBytes`).

## 5. What it costs

`synth.sh multi 8` (yosys `synth_xilinx`, xc7, relative numbers — yosys usually lands 15-25 % above
Vivado), `FR=0` against `FR=6`:

| | LUT | FF | SRL | DSP48 | CARRY4 | longest topological path |
|---|---|---|---|---|---|---|
| FILTER_FRAC = 0 | 1906 | 2130 | 173 | 2 | 168 | 2707 |
| FILTER_FRAC = 6 | 1836 | 2160 | 179 | **2** | 190 | 2636 |

**No extra DSP48** and no new RAM: +30 FF and +22 CARRY4 against −70 LUT, which is within yosys's
own noise for a changed datapath. The 6 bits of state are 3·NS·6 = 144 bits, but most of the ring is
already SRL/LUTRAM, which is why the flip-flop count moves by 30 and not by 144.

**Open:** the real LUT/FF/DSP/WNS from a Vivado run (CI), and `pb_l2` density against the ~80 %
threshold of the gateware method.

## 6. The parameter chain, and what stays at zero

| file | setting | why |
|---|---|---|
| `reDIP-SID/gateware/sid_filter.sv` | `FILTER_FRAC = 0` | Dag Lem's file keeps its own behaviour as its default |
| `reDIP-SID/gateware/sid_core.sv` | passes it through, default 0 | same |
| `c64_redip_sid/sid_api.sv` | not set → 0 | the two-SID reference wrapper: `run_equiv2.sh` stays bit exact |
| `c64_redip_sid/sid_multi.sv` (ours) | **`FILTER_FRAC = 6`** | the 8-SID hardware block |
| `cores/trx_sid/rtl/trx_sid.v` | 6, and **0** under `SID_MULTI_EQUIV` | the build, and the bench against the original reDIP |
| `sim/redip/build_multi.sh` | pinned `-GFILTER_FRAC=0` | `run_equiv8.sh` and `run_bypass.sh` compare bit exact against unmodified reDIP |
| `sim/redip/build_new.sh` | not set → 6 | the hardware configuration bench |

That keeps the bit-exactness proof of T25.4 as a gate: it now proves "we did not change reDIP's
arithmetic when we are asked not to", which is exactly what it is for. `build_new.sh` is the one
bench left on the build's own setting, so `run_new.sh` checks the fix against the spec's own values
rather than against reDIP.

## 7. Acceptance

| | state |
|---|---|
| `run_filter.sh 0`: model bit exact against the RTL, numbers of §3 unchanged | **done** (0 of 2.16 M samples differ) |
| `run_filter.sh 6`: model bit exact against the RTL at the build's setting | **done** (0 differ) |
| worst-case error at F = 6 at or below −70 dBFS | **done** (−75.4) |
| `run_all.sh` (equiv2, equiv8, bypass, trx, new) green with the chain of §6 | **done** 2026-11-07: `redip all: PASS`, 86 PASS, 0 FAIL |
| yosys cost, no new DSP | **done** (§5) |
| Vivado run: LUT/FF/DSP/WNS, pb_l2 density | open |
| board hear test, 6581 and 8580: the beginning of "Meanwhile the Planet" and parts of F20 (Tony's references), A/B against `FILTER_FRAC = 0` | open |

## 8. Not in this task

The other four notes of the same Lemon64 post are separate and stay out of U7:

- the 8580 combined waveform tables from Swallow/Censor and the `$54` (ptr) table — we use reDIP's
  own 8 bit tables and a 2048-entry mirrored P_T (`saw_tri[10:0]`); taking his needs the provenance
  and licence answer Alex asked him for by mail on 2026-11-07, and his tables are not his to license
  on their own;
- the `$D417`/`$D418` DC constant tables for Mahoney (`mah_6581[]`);
- the 8580 test-bit refill after the noise lock-up — measured on the board on 2026-10-09 (ours
  `00 02 00 00 00 00 00 08`, real chip `fe fc fc fc f8 f8 f8 f0`) and decided then: *"wir bleiben
  bei Dags reSID"* (`docs/todo/T25.md`);
- variant B of the 6581 combined waveforms — [ADR-0048](../adr/0048-sid-variant-b-to-m2.md).

Giving the change back to Dag Lem is a separate step, as with the three commits of T25.4
(`docs/todo/T25.md`: "Patch for Dag"). It is his design; what he takes is his call.

## 9. Status and history

Dated entries, newest first.

- **2026-11-07, built (agent; gateware, no Vivado run, no board):** `FILTER_FRAC` on `sid_filter`
  /`sid_core`, 6 in `sid_multi` and `trx_sid`, 0 under `SID_MULTI_EQUIV` and in `build_multi.sh`;
  `muladd.sv` parameterised (defaults unchanged, so `sid_voice` is untouched). New bench
  `tb_filter.cpp` + `build_filter.sh` + `run_filter.sh`, `synth.sh` takes `FR=`. Measured: the
  hardware's worst RMS error is **−32.6 dBFS** with **45 %** of the integrator steps lost and an SNR
  of **−23.8 dB** at 8580 FC 4 res 15; at `FILTER_FRAC = 6` it is **−75.4 dBFS**. The model is bit
  exact against the RTL at both settings. yosys: no new DSP48, +30 FF, −70 LUT, shorter topological
  path. `run_all.sh`: **`redip all: PASS`** (86 PASS, 0 FAIL) — `run_equiv2` (sid_api, 0),
  `run_equiv8` and `run_bypass` (sid_multi, pinned 0) and `run_trx` (`SID_MULTI_EQUIV`, 0) are bit
  exact against the unmodified reDIP, and `run_new` ran in the build's own configuration
  (`FILTER_FRAC = 6`) and passed every spec-derived check (w0, 1/Q, voice DC, the two UltiSID sums,
  envelope levels, ENV3, reset). Open: the Vivado numbers and the board hear test.
- **2026-11-07, decision (Alex):** build it, no milestone — *"M1 und M2 gibt es nicht mehr. Durch die
  Trennung von MultiCore und hier, mach einfach"*. [ADR-0065](../adr/0065-sid-filter-fractional-bits.md).
- **2026-11-06, origin:** Bansai (Tony) on Lemon64, in our tester thread.
