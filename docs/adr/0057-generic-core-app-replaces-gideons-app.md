# 57. The generic core app replaces Gideon's app in TRX64 Ultimate

Date: 2026-10-18

Status: Accepted (supersedes the "Gideon's app" part of [0056](0056-trx64-ultimate-is-the-monolith.md); recorded for both repositories as T104 D9 in `TREX/TRX64U-MultiCore`, commit ccd58253)

## Context

[0056](0056-trx64-ultimate-is-the-monolith.md) (2026-10-16) kept Gideon's app as the application of the monolith and moved the UI/API decoupling (T24, T32, T36, T48, T52) to MultiCore. MultiCore's own LVGL apps use Gideon's code only as a library; its C64 cores still ran Gideon's app (T104 D5). In Gideon's app menu, action and logic are one C++ object, so any UI built inside it carries his structure for ever.

## Decision

Alex 2026-10-18 (translated):

1. Gideon's app is replaced fully, also for TRX64 Ultimate; gaps are fixed piece by piece. No compat mode ("Then we drop the compat stuff").
2. One generic core app for all cores (C64, VC-20, C16, Amiga and this monolith): settings, media, browser, update, app ABI. Core-specific functions live in libraries (1541/IEC with GCR for C64, VC-20, C16; the Amiga links none).
3. The menus result from (a) the settings, (b) a declarative description, (c) the libraries, (d) the apps. Gideon's app is not a model: it does not and cannot do (b), (c), (d). His code is at most a source of library code (e.g. GCR, FatFS, USB).
4. Libraries load dynamically, through the same door as the `.u2a` apps.
5. The generic app lives in `TREX/TRX64U-MultiCore` (`app/`, `coreapp/`), not in an own repository; TRX64 Ultimate consumes it from there.
6. TRX64-FW (the session of this repository) drives the replacement for TRX64 Ultimate, in its own clone and branch of MultiCore; the MultiCore session reviews and merges.
7. Day-1 list, before the generic app replaces Gideon's app on `main` here: firmware update and flash; browse files and start `.d64`/`.crt`/`.prg`; reset, freeze and leaving every state by menu; network and REST enough for tests and deploy; Timewarp; monitor.

## Consequences

- Until the day-1 list runs on the board, `main` keeps Gideon's app (0056 stays valid for the gateware scope: Gideon's core plus ring, Timewarp, monitor, SID sockets).
- [T24 wiring §7](../specs/T24-ui-wiring.md#7-replacement-not-coexistence-alex-2026-10-03) (replacement inside Gideon's app) is superseded in its means; its rule "no way back to Gideon's UI" holds.
- The safety net is Gideon's original firmware, which stays installable, not a mode inside our app.

## Links

- MultiCore `docs/specs/T104-generic-core-app.md` D9
- [TODO.md](../../TODO.md) (M2 section)
