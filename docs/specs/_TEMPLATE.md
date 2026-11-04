# TNN — <what is true when this is done, as one sentence>

**Status:** PROPOSED (YYYY-MM-DD).
**Repo:** TRX64-Ultimate (gateware, app, tools); name any part that lives in C64RE or ue2emu.
**Number:** TNN (registry: the TODO table in `TODO.md`, next free number from there, never from memory).
**Milestone:** M1 | M2 | …
**Origin:** who asked, when, in their words if there are any (translated to English).
**Depends on:** specs, branches or builds this needs, or "nothing".

Adopted 2026-10-12 from the C64RE spec template (`C64ReverseEngineeringMCP/specs/_TEMPLATE.md`,
Alex: "we use that from now on"). Older specs keep their own layout.

---

## §1 The problem

Written for the reader who opens this in three months and remembers nothing.

- **Who** has the problem: Alex at the board, the app, a user, a CI gate, the other sessions.
- **What they are trying to do**, in their terms, not the system's.
- **What happens today** instead, concretely: the register, the report line, the LUT count, the
  photo. Read from the code and measured (CI report, board, ue2emu), not from a comment or an
  older spec.
- **Why it matters**: what it costs (LUT, BRAM, timing, board time), what it blocks.

One worked example with real numbers or output beats three paragraphs.

**Break the problem into parts, MECE:** they do not overlap and together cover the whole use case.
Number them (P1, P2, …). §2 is checked against this list.

## §2 Scenarios — what proves it is solved

The acceptance, as behaviour. Each scenario names the test that runs it; a scenario without a
test is a wish. Write these BEFORE the design.

```gherkin
Scenario: <one behaviour, from the user's side>
  Given <the state the board / app / build starts from>
  When  <what is done: the REST call, the JTAG command, the push, the key>
  Then  <what is observed: exact, checkable>
  And   <what must NOT happen>
```

| # | Solves | Scenario | Test (bench / CI stage / board script) | Where it runs |
|---|---|---|---|---|
| S1 | P1 | … | `designs/c64_basic/sim/...` → `ci/sims.txt` row | CI `sims` |
| S2 | P2 | … | board, by hand (Alex's photo) — why it cannot run in CI | board |

Rules:
- Every part of §1 has at least one scenario, and every scenario solves exactly one part.
- The negative is a scenario too: what must stay unchanged (timing met, RC1 untouched, the app
  still boots).
- A scenario that needs the board, Commodore ROMs or third-party test programs says so and is run
  by hand; nothing third-party is checked in.

## §3 Decision

D1, D2, … — what is built, each with the reason. Name files and modules. Where a choice was
weighed, say what lost and why, in one line.

## §4 Out

What this spec deliberately does not do, and why. Anything not built is listed here, decided with
Alex, not by omission.

## §5 Decisions taken while building

Filled in at the end: every choice the spec left open, and what was chosen.

---
<!--
Closing: Status → DONE (date) + which scenarios are green where; TODO row ✅ and history entry,
in the same commit as the code.
-->
