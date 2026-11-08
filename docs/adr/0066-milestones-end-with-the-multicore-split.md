# 0066. The milestones end with the MultiCore separation

Date: 2026-11-07

Status: Accepted

Supersedes [0003](0003-milestones-m1-m2-m3.md) (M1, M2, M3), which [0056](0056-trx64-ultimate-is-the-monolith.md)
had already amended once.

## Context

[ADR-0003](0003-milestones-m1-m2-m3.md) gave the work an order when there was none: M1 the drop-in replacement
for Gideon's core, M2 the decoupling from Gideon, M3 the platform. Every TODO row carried its milestone in a
column `M`.

Since then the ground moved twice. [ADR-0056](0056-trx64-ultimate-is-the-monolith.md) (2026-10-16) took M2's
content — the decoupling, softboot, the own UI — out of this repository: it lives in `TREX/TRX64U-MultiCore`.
[ADR-0057](0057-generic-core-app-replaces-gideons-app.md) sent the application there too, and the multi-core
work of M3 went with it (2026-10-13). M1 closed on 2026-10-10 with RC1. What was left here was "the monolith":
Gideon's core plus ring, Timewarp, monitor, SID and the tools, and a milestone column in which almost every
open row said `2` for no reason that decided anything.

Asked where U7 (the SID filter's word length) belongs, Alex, 2026-11-07:

> "M1 und M2 gibt es nicht mehr. Durch die Trennung von MultiCore und hier, mach einfach"

and, asked to make the documents say so: *"Ne räum das mal auf bitte"*.

## Decision

The milestones M1, M2 and M3 are retired in this repository. The `M` column leaves TODO.md, and its sections
are named after what they hold instead of after a milestone:

| was | is |
|---|---|
| M1: drop-in replacement for Gideon's core plus the TRX64 functions | Done: the drop-in line and the TRX64 functions |
| M2: open work on the monolith | The monolith: the open work |
| M3: platform | Parked: platform, other cores, the old U64 |
| Extra (no milestone) | Extra |

The order of the work stays what it has been: Alex sets it, row by row, and the status icons carry it
(🔨 in progress, ⏭ next, 📅 planned, ⏸ later, ✅ done, ✖/❌ dropped).

**One gate disappears with the milestones:** [ADR-0052](0052-robot-framework-bdd-tests.md) made the full
migration of the e2e suites to Robot Framework "an exit criterion of M2". There is no M2 to reach, so the
migration is the goal of T71 like any other row, not a gate in front of anything. The Robot Framework decision
itself stands.

**M-words already written stay written.** Older ADRs, spec sections and dated history entries keep theirs
(ADR-0003, 0009, 0030, 0047, 0048, 0051, 0052 and 0056 carry them; so do the "Status and history" entries of
the specs, the board-test records in `docs/board-tests-pending.md` and `docs/AlexTest101026.md`, and the spec
anchors TODO.md links to). They were true when written, and in a decision of the form "this moves to M2" the
M-word now reads as **"deferred, not now"** — the row's own status says whether it is still deferred.

## Consequences

- TODO.md is one list with four named sections and no milestone column; nothing about how it is kept changes
  otherwise (the rules at the top of the file stand).
- No row changes its status or its order through this ADR. U7 was the first row written without a milestone.
- A reader of an old document needs this ADR to read its M-words; that is what the paragraph above is for.
- If a release gate is wanted again later, it gets its own ADR and names what it gates.

## Links

- [TODO.md](../../TODO.md)
- [0003](0003-milestones-m1-m2-m3.md) (superseded), [0056](0056-trx64-ultimate-is-the-monolith.md),
  [0057](0057-generic-core-app-replaces-gideons-app.md), [0052](0052-robot-framework-bdd-tests.md),
  [0065](0065-sid-filter-fractional-bits.md)
