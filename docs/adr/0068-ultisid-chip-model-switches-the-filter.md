# 0068. The UltiSID chip model switches the filter

Date: 2026-11-14

Status: Accepted; superseded in part by [0071](0071-u12-is-built-into-redip.md) - U12 is built into reDIP, whose menu
already has "Chip Model" and a curve that follows it, so the rename and the curve default of the UltiSID store
are not built. In `trx_sid_engine` the chip model does switch the filter path (U12 D1g).

## Context

A review of our `trx_sid` by Tony Bybell (e-mail, 2026-11-09) heard *"Meanwhile the Planet"* as too scratchy and
8580-like. Read in the code: the filter is reSID 1.0's linear 8580 path for both chip models, and the model
setting `cfg_8580` reaches only the combined-waveform tables. The menu calls that setting "Combined Waveforms"
(6581/8580, default 6581; reSID8 calls the same value "Chip Model"), and the separate "Filter Curve" item
defaults to `8580 Lo`. A UltiSID left at its defaults therefore plays 6581 waveforms through an 8580 curve and a
linear filter. U12 adds the 6581's op-amp non-linearity (shape (a), [U12 §3 D1f](../specs/U12-the-sound-of-a-6581.md)).

Asked: does the chip model switch the filter, or does a separate "Filter Model" item (default linear) carry it?

## Decision

Alex (2026-11-14): *"A"*. The item is renamed "Chip Model". 6581 means the 6581 waveforms, the op-amp node of
U12 and, by default, the 6581 cutoff curve; 8580 means today's path, bit for bit. "Filter Curve" stays for a
specific chip's curve.

## Consequences

- A UltiSID left at its defaults sounds different after U12: it becomes a 6581, without the user changing
  anything. That is the point of U12.
- The 8580 setting is unchanged; the benches that compare the 8580 path keep their meaning.
- How "the model's curve by default" reaches a machine whose stored settings already hold `8580 Lo` is open
  for the build: the stored value is an enum index, so a new list entry goes at the end, never at 0.

## Links

TODO.md row U12; [docs/specs/U12-the-sound-of-a-6581.md](../specs/U12-the-sound-of-a-6581.md) D3, Q2
