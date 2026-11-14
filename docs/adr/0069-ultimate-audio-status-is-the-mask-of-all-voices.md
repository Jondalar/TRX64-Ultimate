# 0069. The Ultimate Audio status byte is the pending mask of all voices

Date: 2026-11-14

Status: Accepted

## Context

Our sampler answers a read of a voice's `+0` with `{7'd0, irq_pend[that voice]}`: bit 0, that voice only
(`trx_sampler.v`, T25.6 §3). That rule was derived from the players, not measured. Tony Bybell's test program
for GideonZ/1541ultimate#972 (`ua_972_check.prg`, sha256 `bc1e0806...`) reads `$DF20` in its interrupt and waits
for `3`, bit 0 for voice 0 and bit 1 for voice 1; on ours bit 1 never comes and the program hangs with a red
border. Xander's detection uses voice 0 and waits for `$DF20 == 1`, which a mask also gives. A program that read
voice 3's flag as bit 0 of `$DF80` would see voice 0's instead; none we have read does that.

## Decision

Alex (2026-11-14): *"Maske, die anderen Programme sind fuer Gideons Cores, nicht meinen"*. The status byte is
the 8 bit pending mask of all voices, bit n = voice n, the same in every voice window. The acknowledge at each
voice's `+1F` stays as it is.

## Consequences

- Tony's test program can finish on ours; board test `U12-UA972` is expected to pass after the change.
- T25.6 §3's STATUS row changes from a derived rule to this one; `tb_sampler`'s status checks follow.
- Built under T25.6, not under U12 (U12 Q1).

## Links

TODO.md row T25; [docs/specs/T25.6-sampler.md](../specs/T25.6-sampler.md) §3;
[docs/specs/U12-the-sound-of-a-6581.md](../specs/U12-the-sound-of-a-6581.md) Q5
