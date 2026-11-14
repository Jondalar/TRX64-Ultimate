# 0070. A late sample is caught up, not dropped

Date: 2026-11-14

Status: Accepted

## Context

Tony Bybell's review (e-mail, 2026-11-09) holds that dropping a late sample is permissible and better than
letting the pacing slip. Read in `trx_sampler.v`: the counter to the next sample keeps counting while a voice
waits, a step that is not taken writes it back unchanged, and a step that is taken adds the period to the old
deadline. The grid therefore never shifts and the pitch does not drift. A sample more than a whole period late
makes the voice catch up, the missed steps one after another; no sample is lost. This only happens when DDR2 is
held for more than one period (over 20 µs at 48 kHz); how often that happens on the board is not measured.

## Decision

Alex (2026-11-14): *"So lassen und Doku korrigieren"*. The sampler keeps waiting and catching up; nothing is
dropped.

## Consequences

- `tb_sampler` section 8 (*"the voice waits, no lost step"*) stays the rule.
- The sentence in U12 Q3 that keeping it is "audible when DDR2 is busy" was wrong and is corrected.

## Links

[docs/specs/T25.6-sampler.md](../specs/T25.6-sampler.md); [docs/specs/U12-the-sound-of-a-6581.md](../specs/U12-the-sound-of-a-6581.md) Q3
