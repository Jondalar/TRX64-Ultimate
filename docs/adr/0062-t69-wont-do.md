# 62. T69 (named SID filter profiles) is won't do

Date: 2026-11-04

Status: Accepted

## Context

T69 was the last open item of the SID work: storage, naming and a menu/REST
list for named filter profiles, with C64RE's own parametrisation as the first
profile ([ADR-0039](0039-resid8-filter-curves-dag-only-no-table.md),
[ADR-0040](0040-filter-on-off-per-engine.md)). The scope was already cut to
Dag Lem's curve choice, base/offset and Filter On/Off; the parameters were to
come from the C64RE session and never arrived.

The shipped reSID8 menu covers the same ground by hand: eight of Dag's curves,
`6581 Custom` with base and offset as items, Filter On/Off per engine,
resonance and digis. All of it is stored in the app's own
"reSID8 Configuration" page, so a setting survives a reboot without a profile
file.

## Decision

Alex 2026-11-04: "Lass und das nicht machen, es klingt gut so und die Firmware
zielt auf debugging". No named filter profiles. The default sound stays Dag's
original reDIP, and the menu items stay the whole user interface to the filter.

## Consequences

Nothing is removed: `resid8_plan()` in `software/u64/resid8_filter.h` and the
registers of T25.5 §3.2 keep being the one path from a filter choice to the
hardware — the menu and the host test (`software/test/resid8`) use them, not a
profile loader. T25.5 §6 ("only the interface is kept ready") therefore
describes a seam that stays unused; the comments in `resid8_filter.h` and
`u64_config.cc` that name T69 as future work are stale and are corrected the
next time the application is built.

The C64RE parameter set is no longer being waited for. Should it ever show up
and sound better than the presets, it is a new item, not this one.

## Links

- [T69](../../docs/todo/T69.md)
- [T25.5 reSID8 menus](../specs/T25.5-resid8-menus.md#6-named-filter-profiles-t69--wont-do-this-is-what-the-interface-would-have-been)
