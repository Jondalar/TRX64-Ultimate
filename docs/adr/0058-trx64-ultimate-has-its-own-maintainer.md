# 58. TRX64 Ultimate has its own maintainer session

Date: 2026-10-25

Status: Accepted (amends [0056](0056-trx64-ultimate-is-the-monolith.md) and [0057](0057-generic-core-app-replaces-gideons-app.md) in who owns the work, not in what is built)

## Context

TRX64 Ultimate work had been done from a session rooted in the 1541ultimate fork, and the application work of U3/T104.3 had started on a
branch of `TREX/TRX64U-MultiCore` (`t104-3-app-ui`). Pushing that branch started a CI run on MultiCore's only builder while MultiCore was
in the middle of its host rework. Alex: "mit euch parallel zu arbeiten klappt nicht" (working with you in parallel does not work).

## Decision

Alex 2026-10-25 (translated): "The new session there [in TRX64-Ultimate] shall become the maintainer of TRX64 Ultimate, without ifs and
buts. Not bound to Gideon's 1541, and its firmware is only 'loaded' in MultiCore. It is also an equal developer of the LVGL UI and the
app." And earlier the same day: "You should build here, TRX64-Ultimate. The core, isolated, without disturbing the rest, no effect on the
much more critical rebuilds of MultiCore itself."

- One session owns this repository: specs, TODO, ADRs, releases, gateware and firmware of TRX64 Ultimate.
- It is not bound to the 1541ultimate fork: the app is a submodule here (`software/ultimate-app`) and is this repository's to change.
- MultiCore only loads TRX64 Ultimate's firmware (the full bitstream through IPROG, MultiCore T115); it does not build or change it.
- The LVGL UI and the generic core app (MultiCore T104, T104.3) have two equal developers: this session and the MultiCore session.
  Work on them is built here, isolated, compiled locally, proven in ue2emu, the board last; it reaches MultiCore as a hand-over, never
  as a push into MultiCore's CI while its rework runs.

## Consequences

The handover for the new session is [docs/HANDOVER-maintainer.md](../HANDOVER-maintainer.md). The T104.3 code on MultiCore's branch
`t104-3-app-ui` comes here first (U3).

## Links

- [0056](0056-trx64-ultimate-is-the-monolith.md), [0057](0057-generic-core-app-replaces-gideons-app.md)
- [U3](../specs/U3-generic-app-replaces-gideons-app.md)
