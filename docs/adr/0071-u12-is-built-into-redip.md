# 0071. U12 is built into reDIP, the engine the release runs

Date: 2026-11-14

Status: Accepted

## Context

U12 (the 6581's op-amp non-linearity at the mixing node, [U12](../specs/U12-the-sound-of-a-6581.md)) was written
against the T25.1 engine, `cores/trx_sid/rtl/trx_sid_engine.v`, because that is the RTL Tony Bybell reviewed. While
building it, reading `designs/c64_basic/build.tcl` showed that the release bitstream does not contain that engine:
`TRX_SID` unset builds reDIP with 8 SIDs (menu "reSID8"); the T25.1 engine (menu "UltiSID") is only in a
`TRX_SID=1` build ([ADR-0001](0001-reDIP-stays-default-no-copper.md)). reDIP has no op-amp at its mixer either:
its 6581 cutoff curve is modelled (tanh), the mixer carries a DC offset and ends in a hard 16 bit clamp.

By then (a) was built in `trx_sid_engine` and in the model, bit exact (U12 D1g).

## Decision

Alex (2026-11-14), offered A (stay with `trx_sid_engine`), B (build it into reDIP) and C (both): *"B"*.

The op-amp node goes into reDIP's `sid_filter.sv` for a 6581, behind a parameter; with the parameter off reDIP stays
bit exact against the unmodified original, as with `FILTER_FRAC` ([ADR-0065](0065-sid-filter-fractional-bits.md)).
D4 (the DC offset as a number) is built for reDIP too. The work in `trx_sid_engine` stays in the repository.

## Consequences

- The release gets the change; the equivalence benches against the original reDIP (`run_equiv2.sh`,
  `run_equiv8.sh`, `run_bypass.sh`, `run_trx.sh`) stay the gate with the parameter off.
- D3 of U12 is already true in reDIP's menu: "Chip Model" picks the model and "Filter Curve" defaults to "Chip
  Model". The rename and curve default for the UltiSID store ([ADR-0068](0068-ultisid-chip-model-switches-the-filter.md))
  are not built.
- The change modifies Dag Lem's files again; it goes to him as a patch, as U7 did.
- A measuring instrument for reDIP's output against libresidfp is needed; `filt_cmp` measures the T25.1 model.

## Links

TODO.md row U12; [docs/specs/U12-the-sound-of-a-6581.md](../specs/U12-the-sound-of-a-6581.md) Q8, D1g;
[ADR-0068](0068-ultisid-chip-model-switches-the-filter.md); [ADR-0065](0065-sid-filter-fractional-bits.md)
