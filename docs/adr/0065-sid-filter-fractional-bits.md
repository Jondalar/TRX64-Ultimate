# 0065. The SID filter carries six fractional bits and rounds its shifts

Date: 2026-11-07

Status: Accepted

## Context

A Lemon64 post by Bansai (Tony) in our tester thread (2026-11-06) said reDIP's filter "can suffer bad filter
quantization errors due high resonance, low cutoff" and that "16b of filter accumulator state is just not enough".

Measured against our own RTL (`designs/c64_basic/sim/redip/tb_filter.cpp`, an integer model that is bit exact
against `sid_filter` plus the same filter in double precision, 108 operating points): reDIP's `dv = 17'(o >>> 17)`
floors every integrator step onto the 16 bit grid, so at a low cutoff a step of less than one LSB is lost and the
integrator stops. Worst case **−32.6 dBFS** RMS error with **45 %** of the steps lost and an SNR of **−23.8 dB** —
the quantisation is louder than the signal. 75 of 108 points are below 40 dB SNR, and the worst are all the 8580,
the chip most C64U carry. Two further results: rounding alone buys 2 dB at the worst point, and extra bits
*without* rounding make it worse, because a floored step is a systematic bias that then fires more often instead
of being lost. Details and tables: [docs/specs/U7-sid-filter-precision.md](../specs/U7-sid-filter-precision.md).

This is not a flavour difference like variant B ([ADR-0048](0048-sid-variant-b-to-m2.md)), where Dag Lem's
behaviour is one plausible rendering of a real chip. Here Dag's own difference equations are not being computed.

## Decision

Alex (2026-11-07), asked whether this goes in now or waits: *"M1 und M2 gibt es nicht mehr. Durch die Trennung
von MultiCore und hier, mach einfach"*.

`sid_filter` gets `FILTER_FRAC` fractional bits in the filter state together with rounded shifts.
**`FILTER_FRAC = 0` is reDIP's own arithmetic, bit for bit, and is the default of Dag Lem's files**; our 8-SID
block `sid_multi.sv` and the build in `cores/trx_sid/rtl/trx_sid.v` set **6**, which reaches the floor of the
16 bit output grid (−75.4 dBFS worst case; F = 8 buys nothing that can leave the chip).

The benches that compare bit exact against the unmodified reDIP (`run_equiv2.sh`, `run_equiv8.sh`,
`run_bypass.sh`, `run_trx.sh`) build with 0 and stay the gate. The direct and bypass paths are unchanged by
construction, so a tune that never touches FC, RES or FILT sounds as before.

No milestone is attached: M1 and M2 ended with the separation of MultiCore from this repository.

## Consequences

- Our filter no longer matches Dag Lem's bit for bit in a build. The equivalence proof keeps its meaning in the
  other direction: it proves we do not change his arithmetic when asked not to.
- The change goes to Dag Lem as a patch, as the three commits of T25.4 did. What he takes is his call.
- yosys shows no extra DSP48 (+30 FF, −70 LUT, shorter topological path); the real cost and the `pb_l2` density
  come from a Vivado run, and the ear test on the board is open ([U7 §7](../specs/U7-sid-filter-precision.md#7-acceptance)).
- The "milestone" column of TODO.md and the M1/M2/M3 definitions of [ADR-0003](0003-milestones-m1-m2-m3.md) now
  describe something that no longer exists. U7 carries "–"; cleaning up the model itself is not part of this ADR.

## Links

TODO.md row U7; [docs/specs/U7-sid-filter-precision.md](../specs/U7-sid-filter-precision.md);
[ADR-0048](0048-sid-variant-b-to-m2.md); [ADR-0032](0032-sid-dag-lems-redip-instead-of-own-engine.md);
[LICENSING.md](../../LICENSING.md)
