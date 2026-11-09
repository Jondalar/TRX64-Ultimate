# U12 — The sound of a 6581: the filter's non-linearity, its dirt, and the bits for Mahoney

**Status:** PROPOSED (2026-11-09). Nothing built; §5 has the questions that decide the scope.
**Repo:** TRX64-Ultimate (gateware: `cores/trx_sid/rtl/trx_sid_engine.v`; tables and model:
`cores/trx_sid/model/`, `cores/trx_sid/mem/`; app: `software/io/audio/`, `software/u64/u64_config.cc`).
**Number:** U12 (registry: the TODO table in `TODO.md`).
**Milestone:** after the release U7-U11 are in (C64 Ultimate 0.2.x).
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
| S3 | P2 | the dirty guitar | `U12-DIRT` | board, ears |
| S4 | P4 | 7 bits or better, measured | `spur.py`-style measurement of a volume-register sample | host + board |
| S5 | P5 | WL13 5/5, WL14 level checks | the two programs, GPZ's `sidcheck` | board |
| S6 | — | **negative:** the 8580 path unchanged, F20 clean | the `sid` benches, `U12-F20` | host + board |
| S7 | P6 | ours can be heard on a host against reSID | the C harness of D5 | host |

## §3 Decision

**D1 — The filter gets a 6581 path of its own, from libresidfp.** Not another curve: the
non-linearity is in the topology. libresidfp's 6581 filter model is already the source of our
combined-waveform tables and our provenance line, so it costs no new reading and no clean-room
exception. The 8580 path stays exactly as it is, bit for bit, and which path a chip takes follows
the chip model (D3).

**D2 — Distortion is its own stage, switchable, and off until it has been heard.** It sits after the
6581 path, per chip, so that a wrong fit cannot spoil the 8580 side or a 6581 that someone liked
better without it. Mechanicus 2 and 3 are the listening test; F20 the stress test that it did not
break the sweeps.

**D3 — The chip model reaches the filter, and a 6581 powers up as a 6581.** `cfg_8580` picks the
filter path as well as the waveform tables, and the curve RAM's power-up content becomes the curve
that belongs to the configured model instead of `8580 Lo` for both. The seven curve selections stay:
a user who wants a specific chip's cutoff keeps choosing it.

**D4 — The DC offset becomes a number, not four steps.** Per chip, app-written like the curve, with
the present four levels kept as presets so nothing changes for anyone who does not touch it. What it
is tuned against is a measurement of bit depth, not an opinion.

**D5 — Ours gets heard on a host, from our own RTL.** Verilator already turns `trx_sid_engine.v`
into C++ for the benches; the same object, wrapped so that it can be fed a register trace and asked
for samples, gives a `.wav` beside reSID's for the same tune. That is the reviewer's suggestion
without taking his code: no VICE patch is needed to start, and the reference stays a GPL model we
already use.

**D6 — Combined waveforms are tested before they are touched.** WL13 (GPZ's back-engineered routine
in VICE's `sidcheck`) and WL14 (which depends on the accumulator reaching a particular value, and on
how many distinct levels the LFSR yields) become benches against libresidfp first. Only a difference
that a test shows is then chased in the tables.

## §4 Out

- **The reDIP / reSID8 engine.** A different implementation with its own filter (U7). Nothing here
  touches it.
- **Voice multiplexing and microcoded filter arithmetic.** We have our own answers to both
  (T25.1); the review confirms they are not the problem.
- **`v78` of UltiSID, and any other HDL of Gideon's.** D6 of the clean room: declined, see §1.
- **The sampler items of the same review.** They belong to T25.6 and U11, and §6 lists them so they
  are not lost in an inbox.

## §5 Questions for Alex

**Q5 — Is the IRQ status register a per-voice flag or a mask of all voices?** This decides whether
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

**Q1 — How much of this is one piece of work?** D1 alone (a second filter topology, its benches and
its board test) is the size of U7. D1 + D3 is the smallest thing that makes a 6581 sound like one;
D2 and D4 are each their own change with their own listening test; D5 is a tool. The spec is written
so they can be built in that order, but "fertig" has to mean something, so it has to be said now
which of them U12 is.

**Q2 — Does the chip model switch the filter (D3), or stay a waveform setting?** Making it switch is
what the name promises and it changes what existing users hear without them asking. The alternative
is a separate "Filter Model" item, which is honest and one more thing to explain.

**Q3 — The sampler's late samples (§6).** The reviewer says dropping a late sample is permissible
and better than letting the pacing slip. Ours waits, and a bench checks that it waits
(`tb_sampler` section 8, *"the voice waits, no lost step"*). Changing it is audible on long samples;
keeping it is audible when DDR2 is busy.

**Q4 — The reply.** The reviewer offers `bybell@rocketmail.com` for files. Whether we answer, and
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
- **Dropping late samples:** Q3.
- **The sub-cycle blend** (T25.6, 2026-10-08: 452 -> 639 LUT for -45 dB of step error) he suspects is
  overkill because the output resampler removes what it fixes. Worth measuring after the resampler
  rather than before, which no bench does today; if he is right, 187 LUT come back.
