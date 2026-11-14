# U12 — The sound of a 6581: the filter's non-linearity, its dirt, and the bits for Mahoney

**Status:** DECIDED with Alex, 2026-11-14 (§5: Q1 the scope, Q2 (A), Q7 (a), Q8 the engine is reDIP). Built:
D5 (`tb_wav`), the measuring instrument D1e (`filt_cmp`), shape (a) in the model (D1f), in `trx_sid_engine` (D1g,
only in a `TRX_SID=1` build) and in reDIP, the release engine (D1i), all on the filter output (D1h); benches green,
Vivado and the board open. Open: D4 in reDIP. The reply to the reviewer waits for a joint listening test (Q4).
**Engine (Q8, Alex 2026-11-14: "B"):** the release bitstream runs reDIP with 8 SIDs (`build.tcl`: `TRX_SID`
unset = `redip8`, menu "reSID8"); the T25.1 engine this spec was first written against, and the one the review
read, is only in a `TRX_SID=1` build. U12 is therefore built into reDIP's `sid_filter.sv`, behind a parameter that
leaves reDIP bit exact when it is off, as `FILTER_FRAC` does ([ADR-0065](../adr/0065-sid-filter-fractional-bits.md)).
D3 is reSID8's already: its "Chip Model" item picks the model, and its "Filter Curve" defaults to "Chip Model".
The work in `trx_sid_engine` stays (D1g). [ADR-0071](../adr/0071-u12-is-built-into-redip.md).
**Scope (Q1, Alex 2026-11-14):** U12 is D1 in shape (a) in the RTL, D3 and D4; D2 is part of D1 (a). D6 is
[U14](../todo/U14.md), Q6 is [U15](../todo/U15.md), and the sampler's Q3 and Q5 belong to
[T25.6](T25.6-sampler.md).
**Repo:** TRX64-Ultimate (gateware: reDIP's `cores/icepi/c64_redip_sid/reDIP-SID/gateware/sid_filter.sv`, the
release engine, and `cores/trx_sid/rtl/trx_sid_engine.v`, the T25.1 engine (D1g); tables and model:
`cores/trx_sid/model/`, `cores/trx_sid/mem/`; app: `software/io/audio/`, `software/u64/u64_config.cc`).
**Number:** U12 (registry: the TODO table in `TODO.md`).
**Milestone:** none ([ADR-0066](../adr/0066-milestones-end-with-the-multicore-split.md)).
**Origin:** a review of our `trx_sid` RTL by Tony Bybell (`tbybell`), by e-mail to Alex, 2026-11-09,
after his issue [#2](https://github.com/Jondalar/TRX64-Ultimate/issues/2) (which became
[U11](U11-sampler-any-byte-address.md)). Alex, 2026-11-09: *"Cleanroom bleibt, aber: … dann eine
Specs zum Sound"*.

**Depends on:** [T25.1](T25.1-sid-engine.md) (the engine), [T25.6](T25.6-sampler.md) (Ultimate
Audio), [U7](U7-sid-filter-precision.md) (the reDIP filter's precision — a **different** engine).

---

## §1 The problem

A reviewer who writes SID implementations for a living played our engine and said, in substance:
the arithmetic holds up, the noise and test-bit behaviour match his own, and what is missing is
**the 6581**. *"Something sounded off with Meanwhile the Planet being way too scratchy. It sounded
more 8580-like to me."* And: *"Filter distortion is missing."*

**What our code says, read rather than assumed:**

- `trx_sid_engine.v:9` — the filter follows **reSID 1.0's linear 8580 path**. There is one topology,
  and it is the linear one. No voltage non-linearity, no distortion stage, for either chip.
- The cutoff curve *is* a per-chip choice: a 2048 x 16 RAM (`trx_sid_engine.v:256-258`), addressed
  `cv[{c_sid[2], c_fc[10:1]}]`, written by the app, 1024 entries per UltiSID. The menu offers seven
  (`u64_config.cc:300`): `8580 Lo`, `8580 Hi`, `6581`, `6581 Alt`, `U2 Low`, `U2 Mid`, `U2 High`,
  plus a custom base/offset per chip over `trx_io` (`041/044-04B`).
- Those curves are the **app's** arrays (`model/gen_curves.py` reads `software/io/audio/sid_coeff.c`),
  not fits of a 6581 model.
- The power-up content of that RAM is **curve 0, `8580 Lo`, for both chips** (`gen_curves.py`, the
  `init.mem` it writes). So a harness that instantiates the RTL and configures nothing — which is
  how the review was done, from a single SID extracted out of the Verilog — hears an 8580 cutoff
  curve through a linear 8580 filter. Part of *"sounded 8580-like"* is that, and the rest is the
  topology, which no curve can fix.
- `cfg_8580` does **not** reach the filter: it picks the combined-waveform tables
  (`pd_index(is8580, …)`) and nothing else. Chip model and filter character are two unrelated
  settings today.
- The DC offset for digi material exists and is coarse: `g_dc`
  (`trx_sid_engine.v:855-857`) is `0`, `522240>>2`, `>>1`, `>>0` by the menu's
  `Digis Level` = Off / Low / Medium / High, default Medium. The reviewer measures
  **5.5-6 bits** of resolution out of ours against **7.3** out of his, and names DC offset
  tuning as the reason.
- Combined waveforms come from libresidfp tables (`mem/sid_wf_*.mem`, `model/gen_tables.cpp`). The
  reviewer: `$80` noise with the test bit and its clocking **matches** his, which is what Censor
  needs; combined waves **with** noise do not, which he calls unimportant because the LFSR degrades
  to zero either way; the "sideways dictionary" combined waves *"don't really work too well"*, with
  Wonderland 13 and 14 as the hard cases.
- Nothing of ours can be listened to without the board: the host benches compare against
  `model/sidref.cpp` sample by sample, which proves equality with our own model and says nothing
  about how it sounds.

**Who:** anyone whose music was written for a 6581 — which is most of what people play on this
machine.

**Clean room, unchanged.** The reviewer offers *"v78 of ultisid"* and says no "secret sauce" of
Gideon's is left in it. **We decline** (Alex, 2026-11-09: *"Cleanroom bleibt"*). What U12 reads is
what we already read: libresidfp and reSID (GPL-2.0-or-later), the app's own C, public GitHub
issues, and the behaviour of real machines as others have documented it. Gideon's UltiSID, Copper,
`lp_filter` and sampler VHDL stay shut, and a claim that arrives by e-mail is a pointer to a
measurement, never the measurement itself.

**Parts (MECE):**

- **P1** One filter topology, the linear 8580 one, for both chip models.
- **P2** No distortion: the 6581's dirt is absent even with a 6581 curve selected.
- **P3** The chip model does not reach the filter, and the curve RAM powers up 8580 for both chips,
  so "6581" is a thing the user has to know to go and set.
- **P4** Digi resolution is about a bit and a half short of what the hardware can carry, with a
  four-step offset as the only knob.
- **P5** Combined waveforms: the hard programs (WL13, WL14) are untested, and "test + noise" is what
  they hinge on.
- **P6** No host way to hear ours against a reference, so every sound judgement costs a board
  session and someone's ears.

## §2 Scenarios — what proves it is solved

```gherkin
Scenario: A 6581 tune sounds like a 6581
  Given UltiSID 1 is set to chip model 6581
  When  "Meanwhile the Planet" plays
  Then  it is not scratchy, and side by side with a real 6581 or reSID's 6581 nobody picks ours out

Scenario: The dirty guitar
  Given a 6581 with distortion enabled
  When  Mechanicus songs 2 and 3 play
  Then  the guitar is dirty, the way it is on the hardware

Scenario: Mahoney's bit depth
  Given a sample played through the volume register, as Mahoney's players do
  When  the output is measured
  Then  the resolution is at least 7 bits, not 5.5

Scenario: The hard combined waveforms
  Given Wonderland 13 and Wonderland 14
  When  they run
  Then  WL13 scores 5/5 on GPZ's check, and WL14's level checks hold

Scenario: Nothing that worked stops working
  Given F20's resonance and cutoff sweeps and the 8580 material
  When  they play after the change
  Then  the 8580 path is bit identical to before and F20 is still clean
```

| # | Solves | Scenario | Test | Where |
|---|---|---|---|---|
| S1 | P1 P3 | a 6581 tune sounds like one | `U12-6581` | board, ears |
| S2 | P1 | the 6581 path matches libresidfp's 6581 filter per sample | `sid` bench vs the model | host |
| S3 | P2 | the dirty guitar (D1 (a), D2 is part of it) | `U12-DIRT` | board, ears |
| S4 | P4 | 7 bits or better, measured | `spur.py`-style measurement of a volume-register sample | host + board |
| S5 | P5 | WL13 5/5, WL14 level checks - **moved to [U14](../todo/U14.md)** | the two programs, GPZ's `sidcheck` | board |
| S6 | — | **negative:** the 8580 path unchanged, F20 clean | the `sid` benches, `U12-F20` | host + board |
| S7 | P6 | ours can be heard on a host against reSID | the C harness of D5 | host |

## §3 Decision

**D1 — The filter gets a 6581 path of its own, from libresidfp.** Not another curve: the
non-linearity is in the topology. libresidfp's 6581 filter model is already the source of our
combined-waveform tables and our provenance line, so it costs no new reading and no clean-room
exception (and explicitly **not** reDIP's HDL: `trx_sid_engine.v:11` says the engine was written
without it, and that stays true). The 8580 path stays exactly as it is, bit for bit, and which path
a chip takes follows the chip model (D3).

**D1a — The model, read 2026-11-09** (`Integrator6581.cpp:32-96`, pinned commit
`dcf6b8ac` of libresidfp). Per integrator and sample:

```
Vgst = nVddt - vx ;  Vgdt = nVddt - vi                  two 16 x 16 squares
n_I_snake = kSnake * ((Vgst^2 - Vgdt^2) >> 15)          one multiply
nVg  = vcr_nVg[(nVddt_Vw_2 + (Vgdt^2 >> 1)) >> 16]      table
kVgt = (nVg - nVt) - nVmin
If   = vcr_n_Ids_term[kVgt - vx - INT16_MIN] << 15      table
Ir   = vcr_n_Ids_term[kVgt - vi - INT16_MIN] << 15      table
vc  += n_I_snake + (If - Ir)
vx   = opamp_rev[(vc >> 15) - INT16_MIN]                table
vo   = vx - (vc >> 14)
```

So three distinct tables, four lookups, three multiplies, and one piece of state (`vc`) per
integrator. The tables are chip constants, not per-SID, which is the good news.

**D1b — The tables cannot be copied, and the real figure is worse than the first one in this spec.**
A first pass counted the integrator's three (`vcr_nVg`, `vcr_n_Ids_term`, `opamp_rev`, each
`1<<16` x 16 bit = 384 KB together). Reading on, the filter's **summing, mixing, volume and
resonance nodes each have their own stack of tables** as well
(`FilterModelConfig.h:150-225`): `summer` 20 x 65536, `mixer` 28 x 65536 + 1, `volume` 16 x 65536,
the resonance gain another 16 x 65536. With the three above that is **about 5.44 million 16 bit
entries, 10.9 MB, roughly 2 400 BRAM36 tiles**. This part (Artix-7 50T) has **75**, of which 56.5
are in use with T25.1 §5's ceiling at 60 — **3.5 tiles free**, and T25.1 already names BRAM as the
tight resource. Not a factor of 24 out: a factor of about 700.

**But all of those tables are one function.** Every entry is `opampModel.solve(n, vin)`
(`OpAmp.cpp:33`), a Newton-Raphson over the **measured opamp transfer curve** —
`f(x) = (n+1)(Vddt-x)^2 - n(Vddt-vi)^2 - (Vddt-opamp(x))^2` — tabulated once per discrete gain `n`
(2..6 summer inputs, 0..7 mixer inputs, 16 volume steps, 16 resonance steps). So what has to go into
the gateware is **the curve and the solve**, not the tables: one shared 1024-entry spline of value
and slope (1024 x 32 bit = 32 Kb, one RAMB36, replicated two or three times for read ports) and two
or three Newton steps of two multiplies each.

**D1c — The arithmetic budget, counted.** A frame is **64 clk2x** and carries all eight SIDs. Per
SID and C64 cycle the full model wants roughly six opamp solves (the summer's three outputs, the
mixer, the volume, the resonance gain) plus the two integrators' four lookups and three multiplies.
At three Newton steps per solve that is about **26 lookups and 60 multiplies per SID**, so **210
lookups and 480 multiplies per frame** for eight. Against 64 clocks that needs about **8 DSPs** (we
use 14 of 120, so that part is free) and **four read ports** on the curve (two replicated RAMB36,
dual-ported). The arithmetic therefore fits; what is not free is the LUT and the routing of a
pipeline that deep, in a region at 68.5 % LUT with a measured routing ceiling near 80 %
(the `fpga-gateware` note on `pb_l2`). **This is not a step, it is a piece of work the size of U7 or
larger, and it may not fit at all.** Hence Q7.

**D1e — The instrument, and the baseline it measured. BUILT 2026-11-09.**
`cores/trx_sid/model/filt_cmp.cpp` runs the same register log through libresidfp's SID and through
ours, samples both every C64 cycle, aligns them by the best correlation lag, matches their levels
and reports the residual in dB. It exists because **`compare_resid` only ever checked the digital
part** — accumulator, LFSR, waveform, envelope — so our filter and mixer had never been measured
against anything but themselves. `EXTFILT=1` runs the C64's own output stage (a 16 kHz RC low-pass
and a 1.6 Hz DC blocker, the time constants of libresidfp's `ExternalFilter.h`) over our samples,
because libresidfp has it and T25.3 decimates instead; leaving that in the number blames the filter
for the C64's wiring. The lag search is not decoration: without it the same comparison reported a
residual near 0 dB, which is what two unaligned signals always report.

Baseline, `filter.log`, 200 000 cycles, `EXTFILT=1`:

| | correlation | residual |
|---|---|---|
| ours as 6581 (curve 2) vs libresidfp 6581 | **0.887** | **-6.4 dB** |
| ours as 8580 (curve 0) vs libresidfp 8580 | **0.831** | **-4.7 dB** |

Two things follow, and the second one matters more than D1 did. The external stage alone moves the
6581 figure from -3.8 to -6.4 dB and the best lag from 8 samples to 1, so a good part of what looked
like a filter difference was the C64's output network. And **our 8580 path is further from
libresidfp's 8580 than our 6581 config is from its 6581** — the 8580 path is the one we claim
follows reSID 1.0, so the gap is not a 6581 problem. What is shared by both is the analog tail:
the voice and envelope DAC imperfections libresidfp models (`Dac.cpp`), its per-voice DC
(`voiceDC`), and the opamp non-linearity that sits in **both** models' summer, mixer and volume
nodes. Before any of D1's deep pipeline, that tail is where the measurable distance is.

Side observation, not chased: on `adsr.log` our model's output stays at 0 while libresidfp's does
not. Either that log never opens our volume or something in the DC path differs; it is a lead, not
a finding.

**D1f — (a) built in the model and measured, 2026-11-09.** Three things went into `sidref.cpp`
behind options that are **off by default**, so every existing bench is untouched (`tb_sid`: 2.6 M
cycles, 0 mismatches after the change):

- `cfg.opamp_drive` — the 6581's measured op-amp transfer (33 points, `OPAMP[]`) as a waveshaper at
  the **mixing node**, scaled for unity small-signal gain around the working point (4.54 V). The
  parameter is how many volts of the curve full scale reaches into.
- `cfg.wav_sat` — libresidfp's cubic saturation of the waveform DAC (`1.1x - 0.11x^3`).
- `cfg.voice_dc` — its envelope-dependent per-voice DC (0.2143 V at full envelope) in place of our
  constant one.

Measured with `filt_cmp` on `filter.log`, 200 000 cycles, `EXTFILT=1`, against libresidfp's 6581:

| | correlation | residual |
|---|---|---|
| as it ships (Digis Level Medium, linear mixer) | 0.8868 | -6.40 dB |
| + the op-amp mixer, drive 0.5 V | **0.9429** | **-9.33 dB** |
| Digis Level High, linear mixer | 0.9399 | -9.11 dB |
| Digis Level High + op-amp, drive 0.3 V | **0.9625** | **-11.11 dB** |

**The op-amp node is worth building: it improves at every DC setting, and 4.7 dB at the best pair.**
**Corrected 2026-11-14 (D1h): measured on `filter.log` only, where every voice goes through the filter. On
`tones.log` (direct voices) the curve on the whole sum makes it much worse - 0.987 -> 0.896 at Medium / 0.5 V,
0.947 -> 0.574 at High / 0.3 V. The node now takes the filter output alone.**
Two other results are worth more than that one, though:

- **The cubic DAC saturation measures nothing** (±0.005 correlation anywhere). Not built.
- **Our "Digis Level" is doing part of the 6581's work by accident.** With a *linear* mixer the
  agreement rises monotonically with it — 0.746 / 0.832 / 0.887 / 0.940 for Off / Low / Medium /
  High. In a linear chain a DC offset cannot change the sound, so what it changes is where the
  signal sits against the **20 bit saturation** of `out`: one-sided clipping, which is a crude
  non-linearity standing in for the missing one. The two knobs therefore interact (High + 0.5 V is
  *worse* than High + 0.3 V), and the drive has to be fitted against whatever DC ships, not in
  isolation.
- **`digi.log` and the fuzz logs are untouched by all of this** (0.44 to 0.71 before and after).
  Whatever dominates there is neither the mixer node nor the DAC; the next measurement, not the next
  build, has to find it. The filter topology itself (D1c, option (b)) and the DC content of the
  combined waveforms are the candidates.

**D1g — (a) built in `trx_sid_engine`, 2026-11-14** (before Q8 moved U12 to reDIP; it stays; on the filter output
only since D1h). The table: `cores/trx_sid/model/gen_opamp.py` turns the 33 measured points into 1024 knots x
22 bit (`mem/sid_opamp6581.mem`, drive 0.5 V): the input clamped to 24 bits (+-16 full scale), knots 16384 apart,
linear between two knots with a floor shift. The model (`sidref.cpp` `opamp_lookup`) and the engine do exactly that
in integers, so the benches stay bit exact; the double-precision waveshaper of D1f is gone. In the engine the node
is F7..F9 on the filter output (a dual-port ROM read of two neighbouring knots, one DSP for the step), the direct
voices join it at F10 with the volume, the saturation is F11. An 8580 bypasses the table with the same latency, bit
for bit what it was; the filter state and the bus value are still written back at F8, only the samples come three
clocks later. A 6581 always takes it: in this engine the chip model now switches the filter path. **Measured**
(on the filter output, D1h): `tb_sid` 2 642 583 cycles, 8 SIDs (four 6581, four 8580), 0 mismatches at 64 clocks
per cycle; `tb_sid_bus` 300 000 cycles, 0 mismatches; `filt_cmp` with the integer table, D1h's table below. The
first version, on the whole sum, matched D1f's double figures to the last digit. At 48 clocks per cycle `tb_sid` reports 146
mismatches, all in the state-port reads after the last pass; the engine before U12 gives the same 146 (not caused
by this; the samples match there too). No Vivado run: the release does not build this engine.

**D1h — The curve on the filter output, not on the whole sum, 2026-11-14.** Measuring all of `gen_logs.py`'s logs
instead of `filter.log` alone showed the whole-sum node hurting the direct voices: they carry the digi DC, which
moves the op-amp's working point into the flat part of its curve. Taking the curve on the filter output only (the
direct voices are added after it, linear) keeps the gain and costs the direct path nothing. Correlation with
libresidfp's 6581, `filt_cmp`, 200 000 cycles, `EXTFILT=1`, curve 2:

| T25.1 model | filter | tones | fuzz0 | fuzz1 | fuzz2 | fuzz3 | digi |
|---|---|---|---|---|---|---|---|
| Digis Medium, linear | 0.887 | 0.987 | 0.437 | 0.706 | 0.669 | 0.616 | 0.714 |
| Digis Medium, whole sum, 0.5 V | 0.943 | **0.896** | | | | 0.640 | |
| Digis Medium, filter output, 0.5 V | **0.943** | 0.987 | 0.461 | 0.691 | 0.670 | 0.615 | 0.714 |
| Digis High, linear | 0.940 | 0.947 | 0.402 | 0.687 | 0.659 | 0.629 | 0.714 |
| Digis High, whole sum, 0.3 V | 0.963 | **0.574** | | 0.648 | | 0.647 | |
| Digis High, filter output, 0.3 V | **0.963** | 0.947 | 0.430 | 0.669 | 0.661 | 0.630 | 0.714 |

`fuzz1` (random register writes, which no variant follows well) loses 0.015-0.02 everywhere; nothing else does.
Two other centrings were tried and dropped: the whole sum around the direct voices' DC, and around three voices'
DC (both worse on `filter` or `tones`).

**D1i — Built in reDIP (Q8), 2026-11-14.** `sid_filter.sv` parameter `OPAMP_6581` (0 = Dag Lem's mixer, bit for bit
and cycle for cycle; `sid_multi.sv` and `trx_sid.v` build 1, the equivalence builds 0). reDIP's voices are in the
same units as ours (wave - 0x380 times envelope, the same digi DC); its mixer works on the sum >> 7 in whole LSB, 17
bits before its 16 bit clamp. The table (`gen_opamp.py --redip`, `sid_opamp_6581.mem`, 1024 x 16 bit, knots 128
apart) takes the filter output rounded to whole LSB; the direct path is added after it, then the same 16 bit clamp.
Four clocks of pipeline (stages 8-11: two knots out of a dual-port ROM, the step, the add); the master volume
multiply moves from stage 8 to stage 12 for every SID, the one slot of the five where the shared multiplier gets no
operands, and `sid_core.sv` takes SID k's audio on cycle 13 + 5k (pass 56 clocks for 8 SIDs instead of 53). An 8580
gets reDIP's own sum, unchanged. Drive fitted for reDIP's default (Digis "Auto": a 6581 gets the full digi DC) with
a new instrument: `designs/c64_basic/sim/redip/tb_cmp.cpp` plays a log through `sid_multi` (hardware build, the
app's reSID8 defaults) and writes one sample per C64 cycle, which `filt_cmp` (`OURS=<file>`) compares with
libresidfp. reDIP as a 6581:

| reDIP | filter | tones | fuzz0 | fuzz1 | fuzz2 | fuzz3 | digi |
|---|---|---|---|---|---|---|---|
| as released | 0.943 | 0.918 | 0.352 | 0.646 | 0.672 | 0.517 | 0.714 |
| filter output, 0.2 V | 0.956 | 0.918 | 0.371 | 0.634 | 0.675 | 0.515 | 0.714 |
| **filter output, 0.3 V** | **0.964** | 0.918 | 0.380 | 0.629 | 0.675 | 0.515 | 0.714 |
| filter output, 0.5 V | 0.924 | 0.918 | 0.388 | 0.609 | 0.676 | 0.513 | 0.714 |

0.3 V is what is built. **Benches:** `tb_new` (hardware build) has a check 7: every SID's sample is its volume
times the 16 bit clamp of the direct path plus the table's value for the filter output (6581) or of reDIP's own
sum (8580), from the RTL's own stage-8 registers - 239 968 checks per seed, 0 errors. Its sum check now counts the
eight per-SID samples to their pass rather than to a 64-clock window: with the node the last of them leaves after
clock 64 (the stream is not used in the build; `c64_trx_sid` takes the two sums). `tb_sym` takes the eight samples
as a set when SID 7's arrives, for the same reason. `build_multi.sh` pins `OPAMP_6581 = 0` for the equivalence benches.

**D1d — What proves it.** A host bench against libresidfp's own `Filter6581` per sample (the model
harness already links it, `cores/trx_sid/model/compare_resid.cpp`), reporting the worst and the RMS
difference the way U7 reported dB, and the same sweeps F20 uses. The interpolated tables are
accepted when that error is below the 8580 path's own error against its model, and not before.
`tb_wav` (D5) is for the ear afterwards, not for the proof.

**D2 — No distortion stage of its own: it is part of D1 (a).** The first draft put a switchable stage
after the 6581 path. Reading libresidfp (D1b) showed there is no such stage: the dirt is the op-amp solves at
the summing, mixing, volume and resonance nodes, and (a) is the first of them. Mechanicus 2 and 3 stay the
listening test, now of D1 (a); F20 the stress test that it did not break the sweeps.

**D3 — The chip model switches the filter (Q2 answered (A), Alex 2026-11-14,
[ADR-0068](../adr/0068-ultisid-chip-model-switches-the-filter.md)).** UltiSID's menu item "Combined
Waveforms" (6581/8580, default 6581, `u64_config.cc:470-471`) is the chip model in all but name: it is
`cfg_8580` (`CFG_EMUSID*_WAVES`, the item reSID8 already calls "Chip Model"). It is renamed "Chip Model" and
picks the waveform tables as today, the filter path (6581: the op-amp node of D1 (a); 8580: today's path, bit
for bit) and, by default, the cutoff curve of that model; the curve RAM's power-up content follows the model
too, instead of `8580 Lo` for both. The seven curve selections stay: a user who wants a specific chip's
cutoff keeps choosing it. Today a UltiSID left at its defaults mixes 6581 waveforms with curve `8580 Lo`
(the curve item's default 0) through the linear 8580 filter; after D3 it is a 6581. Open for the build: how
"the model's curve by default" reaches a machine whose stored settings already hold `8580 Lo` - the value is
an enum index, so a new entry goes at the end of the list, never at 0.
**Since Q8 not built for the UltiSID store:** reDIP's menu already does this (reSID8's "Chip Model", "Filter
Curve" = "Chip Model" by default), and the rename and the curve default would only reach a `TRX_SID=1` build.
In `trx_sid_engine` the filter path follows the chip model since D1g.

**D4 — The DC offset becomes a number, not four steps.** Per chip, app-written like the curve, with
the present four levels kept as presets so nothing changes for anyone who does not touch it. What it
is tuned against is a measurement of bit depth, not an opinion.

**D5 — Ours gets heard on a host, from our own RTL. BUILT 2026-11-09.**
`designs/c64_basic/sim/sid/tb_wav.cpp`, built by that directory's `build.sh`, is a Verilator bench
on **`c64_trx_sid`** — the very module `c64_top` instantiates, so `trx_sid` (decode and the T25.1
engine) *and* T25.3's `trx_audio` (mixer gains, CIC, halfbands, fractional FIR, DC blocker) are in
it. What it writes is what the board sends to I2S:

    designs/c64_basic/sim/sid/build/tb_wav <memdir> <curve.mem|-> <trace|-> <out.wav> [cycles]
    env: MODEL8580=0|1  DIGI=0..3  RES=0|1  VMODE=0..3

`<curve.mem>` is one of `cores/trx_sid/model/gen_curves.py`'s `curve0..6.mem` (`-` keeps the
power-up content, which is `8580 Lo` — see §1); `<trace>` is a register log in the format
`gen_logs.py` writes (`<idle> W <reg> <val>`, hex), and `-` plays a built-in demo (two notes, a
cutoff sweep, a release) so the tool runs with no input file at all. It prints frames, peak and RMS,
so a difference can be stated before anyone argues about it.

First measurement, the same demo through four configurations (1.2 s each, PAL):

| | peak | rms |
|---|---|---|
| curve 0 (`8580 Lo`), model 6581 | 2 912 260 | 509 617 |
| curve 2 (`6581`), model 6581 | 1 892 932 | 443 712 |
| curve 0, model 8580 | 1 904 380 | 435 759 |
| curve 2, `DIGI=0` (the DC offset off) | 1 429 106 | 391 455 |

So every knob this spec is about moves the output, and from here on D1-D4 can be judged by ear and
by number without a board. What it is **not** yet: a real tune. That needs a register trace of one,
which siddump or VICE produces and we do not generate ourselves (§5 Q6).

**D6 — Combined waveforms are tested before they are touched.** WL13 (GPZ's back-engineered routine
in VICE's `sidcheck`) and WL14 (which depends on the accumulator reaching a particular value, and on
how many distinct levels the LFSR yields) become benches against libresidfp first. Only a difference
that a test shows is then chased in the tables.

## §4 Out

- **The reDIP / reSID8 engine** was out in the first draft. Since Q8 it is where U12 is built (D1 (a), D4);
  U7's precision work stays U7's.
- **Voice multiplexing and microcoded filter arithmetic.** We have our own answers to both
  (T25.1); the review confirms they are not the problem.
- **`v78` of UltiSID, and any other HDL of Gideon's.** D6 of the clean room: declined, see §1.
- **The sampler items of the same review.** They belong to T25.6 and U11, and §6 lists them so they
  are not lost in an inbox. Q3 and Q5 are answered and go to [T25.6](T25.6-sampler.md).
- **The combined waveforms (D6)** are [U14](../todo/U14.md), and **register traces of real tunes (Q6)**
  are [U15](../todo/U15.md) (Q1, Alex 2026-11-14): each is work of its own and neither is needed for a
  UltiSID that sounds like a 6581.

## §5 Questions for Alex

**Q8 — answered 2026-11-14, Alex: "B".** Asked after D1g was built, when reading the app's menu showed that
the release runs reDIP and not the engine U12 had been written for: A (stay with `trx_sid_engine`, heard by
nobody unless the release changed engine, against [ADR-0001](../adr/0001-reDIP-stays-default-no-copper.md)),
B (build it into reDIP, the release engine), C (both). B: [ADR-0071](../adr/0071-u12-is-built-into-redip.md).

**Q7 — answered 2026-11-09, Alex: (a).** *"A ist ok"* - the opamp non-linearity at the output
stage, the integrators left alone, and listen before anything deeper is considered. D1e's baseline
says that is also where the measurable distance is for **both** chip models, so (a) is no longer
only the cheap option: it is the one the measurement points at. The order of work is the model
first (`sidref.cpp`, measured with `filt_cmp`), then the RTL, because the benches compare the RTL
against that model bit for bit.

**Q7 (as asked) — How much 6581 do we build, now that the model has been counted?** D1a-D1c measured what
libresidfp's 6581 really is: one measured opamp curve solved by Newton-Raphson at every summing,
mixing, volume and resonance node, which is also **where the distortion of D2 actually lives** — in
residfp there is no separate distortion stage, the dirt is those opamp solves. Three honest shapes,
cheapest first:

- **(a) The output stage only.** The opamp non-linearity at the mixer and the volume ladder, the
  integrators left as they are. One or two solves per SID and cycle, so 1-3 DSPs and one curve
  table: it fits today's schedule with a small change. It is where most of what an ear calls "6581"
  sits, and it is **not** residfp-exact.
- **(b) The whole filter, streaming.** D1c's 210 lookups and 480 multiplies a frame. Arithmetically
  possible, a deep pipeline, a rewrite of the filter stage, and a real risk of not routing in
  `pb_l2`. This is what "sounds like a 6581" in the strict sense.
- **(c) Neither, for now.** Keep the linear path, take the 6581 cutoff curve that already exists
  (D3, which is cheap), and leave the topology until someone has heard (a) against a real 6581.

I would build **(a)** first and listen with `tb_wav` before deciding whether (b) is worth its risk,
because (a) is also the honest prerequisite of (b): the curve, its table and its solve are the same
in both. What I will not do is start (b) blind — the `fpga-gateware` rule is read, compare, plan,
and the plan now says the thing might not route.

**Q6 — moved to [U15](../todo/U15.md) (Q1, Alex 2026-11-14).**

**Q6 (as asked) — Where do register traces of real tunes come from?** D5 plays a built-in demo and any log in
`gen_logs.py`'s format, which is enough to compare two builds of ours. To hear *Meanwhile the
Planet* or Mechanicus, something has to turn a `.sid` into that log: `siddump` (external, Tony's own
route), VICE, or our own c64re runtime, which already stores bus events and is ours. The third is
the only one that costs no new dependency, and none of the three is built.

**Q5 — answered 2026-11-14, Alex: the mask.** *"Maske, die anderen Programme sind fuer Gideons Cores,
nicht meinen"*. The status byte of every voice window becomes the 8 bit pending mask of all voices;
[ADR-0069](../adr/0069-ultimate-audio-status-is-the-mask-of-all-voices.md), built under
[T25.6](T25.6-sampler.md) on 2026-11-14 (benches green). `U12-UA972` is expected to pass on a bitstream with it.

**Q5 (as asked) — Is the IRQ status register a per-voice flag or a mask of all voices?** This decides whether
his test program can pass on our hardware at all, and it is the one thing in the review that is a
defect rather than a missing feature.

- **Ours** (`trx_sampler.v`, the `dout` of the C64 side): a read of a voice's `+0` returns
  `{7'd0, irq_pend[that voice]}` — bit 0, that voice, no other bit, which is what T25.6 §3 wrote
  down. That sentence was derived from the players, not measured against hardware.
- **His test** reads `$DF20` — voice 0's window — inside the interrupt and expects **bit 0 = voice 0,
  bit 1 = voice 1**; it waits for `$DF20 == 3` and acknowledges each voice at its own `+1F`
  (`$DF3F`, `$DF5F`, which is ours exactly). On ours bit 1 is never set, so the program cannot
  finish: it sits in its wait loop with a red border. That is read off his source, not a board run
  (`U12-UA972`).
- **Both public programs agree** if the status byte is the 8 bit pending mask of all voices, the same
  in every window: Xander's detection (T25.6 §3) uses **voice 0** — it acknowledges `$DF3F` and polls
  `$DF20` for 1 — so a mask reads 1 for it too.
- **What it would cost:** a program that polls voice 3's own flag at `$DF80` bit 0 would then see
  voice 0's. Nothing we have read does that, and nothing rules it out either; Heartbeat's IRQ use is
  not recorded in T25.6.
- I would make it the mask, because it is the only reading under which both known programs work, and
  write it into T25.6 as a measured rule instead of a derived one. It is register-visible, so it is
  Alex's.

**Q1 — answered 2026-11-14, Alex: "Ja"** to: U12 is D1 (a) in the RTL, D3 and D4, with D2 part of D1 (a);
D6 and Q6 get numbers of their own (U14, U15); Q5 is a correction in T25.6.

**Q1 (as asked) — How much of this is one piece of work?** D1 alone (a second filter topology, its benches and
its board test) is the size of U7. D1 + D3 is the smallest thing that makes a 6581 sound like one;
D2 and D4 are each their own change with their own listening test; D5 is a tool. The spec is written
so they can be built in that order, but "fertig" has to mean something, so it has to be said now
which of them U12 is.

**Q2 — answered 2026-11-14, Alex: (A).** The chip model switches the filter; see D3.

**Q2 (as asked) — Does the chip model switch the filter (D3), or stay a waveform setting?** Making it switch is
what the name promises and it changes what existing users hear without them asking. The alternative
is a separate "Filter Model" item, which is honest and one more thing to explain.

**Q3 — answered 2026-11-14, Alex: keep it** (*"So lassen und Doku korrigieren"*;
[ADR-0070](../adr/0070-a-late-sample-is-caught-up-not-dropped.md)). The question as first asked was
wrong about ours. Read in `trx_sampler.v`: the counter to the next sample keeps counting down while a voice
waits (`:185`), a step that is not taken writes the counter back unchanged (`:207`, `:259`), and when it is
taken the period is added to the old deadline, not to the moment the sample came. So the grid never shifts
and the pitch does not drift - the "pacing slip" the reviewer warns of does not happen here. A sample more
than a whole period late makes the voice catch up, the missed steps one after another, so no sample is lost
and the position in the sample is right again; dropping would trade that short hurried moment for a missing
sample, and only when DDR2 is held for more than a period (over 20 µs at 48 kHz). How often that happens on
the board is not measured.

**Q3 (as asked) — The sampler's late samples (§6).** The reviewer says dropping a late sample is permissible
and better than letting the pacing slip. Ours waits, and a bench checks that it waits
(`tb_sampler` section 8, *"the voice waits, no lost step"*).

**Q4 — answered 2026-11-14, Alex: not yet.** *"wir machen die Arbeit weiter und antworten erst wenn wir alles
mal durchgehoert und getestet haben gemeinsam"* - no reply, by mail or on issue #2, until Alex and Claude have
listened to and tested the result together on the board.

**Q4 (as asked) — The reply.** The reviewer offers `bybell@rocketmail.com` for files. Whether we answer, and
with what, is Alex's.

## §6 The same review on Ultimate Audio (T25.6 / U11)

Kept here so the e-mail's content survives in the repo, not to be built under this number:

- **The 8-vs-16-bit rate asymmetry** he documented as GideonZ/1541ultimate#972 (read 2026-11-09; an
  earlier draft of this spec called it a "1-vs-2-byte slowdown" and said it described the stereo
  interleave — it does neither). In his reading of Gideon's VHDL the fetch states after each divider
  expiry are not prescaled, so a 16 bit voice pays two unprescaled visits per sample against a mono8
  voice's one, and two voices at the same RATE drift apart without bound (80 ns per sample on the
  U64-II). **Ours does not have it, measured rather than assumed:** `tb_sampler` section 11 runs an
  8 bit and a 16 bit voice at the same RATE (194 = his $00c2, 50, 400) and counts what each delivers
  in the same time — equal to within the one sample they start apart. Our period comes from `ratee`
  and the 1/256-tick counter, and neither sees the bit depth.
- **Misaligned 16 bit fetches:** fixed, [U11](U11-sampler-any-byte-address.md), benches green, board
  hear test open.
- **Stereo interleave:** he thinks it is missing, and it is. #972 says nothing about it and neither
  does his test program, so **the exclusion of U11 §4 stands**: Gideon's VHDL is still the only
  description of its encoding we know of, and this repo does not open it. It needs another source —
  his own words by mail would be one, Xander's manual another — before it can be built.
- **The IRQ:** he had not checked whether ours is wired. It is — `c64_core.v:591`,
  `.ext_io_irq(io_irq | smp_irq)`. His test program (`ua_972_check.prg`, posted in #972, sha256
  `bc1e0806c2e9b596216a1e50a0021aa404d95806920822fdd2293307d89c7439`) does exercise it, and reading
  its source turned up something bigger than the rate it was written for — **Q5**.
- **Dropping late samples:** Q3, answered: ours keeps waiting and catches up; its grid does not slip.
- **The sub-cycle blend** (T25.6, 2026-10-08: 452 -> 639 LUT for -45 dB of step error) he suspects is
  overkill because the output resampler removes what it fixes. Worth measuring after the resampler
  rather than before, which no bench does today; if he is right, 187 LUT come back.
