# 63. The application repository's CI builds one target, and Gideon's menu tests are not repaired

Date: 2026-11-04

Status: Accepted

## Context

`TREX/1541ultimate` (branch `trx64/main`) is our fork of Gideon's firmware and
carries his CI with it. For our release that CI produces nothing: the pipeline
here builds the application itself from the submodule commit it pins
([T31](../specs/T31-ci-pipeline.md), decision of 2026-10-04). Its own jobs built
`u64` (Nios II, Gideon's older Ultimate 64), `u64ii` (our target, plus the apps,
the OpenAPI checks and the e2e suites against ue2emu) and a `bundle` zip.

It had been red in both jobs since at least 2026-10-30 — six runs, none of it
the firmware we ship. Two of the three causes were our own code reaching into
shared sources for hardware that has neither our IO block nor our debug block:
`ultimate.cc` called `trx_service_start()` unconditionally, and `c64.cc` read
the state bus's shadow gate through `trx_io_read32()` in `freeze_slim()`. Both
broke the Nios II link, on the same day, one after the other.

Rebuilding the CI image to ue2emu 0.6.0 (needed for the TRX IO block, so that
`trx-service-boot` can judge the U5 boot order at all) then changed what the
emulator models: with `--trx-io` auto it is *our* bitstream for TRX firmware,
the expansion port included. `cart-dumper`, `cart-writer` and `dialog-keys`
drive trx_crt's screens in Gideon's menu and expect the stock model; they fail
on it (the dumper's status stays empty, the inspector never offers `F1:Go`).

Where this is going is already decided: the generic application of
TRX64U-MultiCore replaces Gideon's menu ([ADR-0057](0057-generic-core-app-replaces-gideons-app.md),
[U3](../specs/U3-generic-app-replaces-gideons-app.md)), and the e2e tests become
our own BDD suites ([ADR-0052](0052-robot-framework-bdd-tests.md), T71).

## Decision

Alex 2026-11-04: "Ich will langfristig auf MultiCore Only als App. Mit unseren
eigene BDD tests e2e. Und bis dahin muss das möglichst störfrei und einfach
bauen. Bistream+Gideon App."

The fork's CI keeps the one job that says something about what we ship, `u64ii`.
The `u64` (Nios II) and `bundle` jobs are removed. The three suites that test
Gideon's cart screens leave the CI list with their reason written down; they
come back as BDD scenarios on the generic application, not as repairs on a menu
that is being retired.

## Consequences

Eleven suites run on every push to the fork, `trx-service-boot` among them, so
the U5 boot order is guarded. Nothing was deleted from the repository: `make
u64` still builds, the bundle still works by hand, and
`build/ci_emulator_suites.sh` names the three suites and how to run them.

What we give up: the Nios II build was the canary for a rebase onto Gideon's
upstream, and nobody watches that now. A rebase therefore has to build `u64`
by hand before it is called done. The shared sources keep the two guards this
cost us (`trx_service_start` taken weakly, the shadow-gate wait under
`#if (U64 == 2) && !defined(RECOVERYAPP)`), which is right anyway.

## Links

- [T31 CI pipeline](../specs/T31-ci-pipeline.md)
- [U3 generic app replaces Gideon's app](../specs/U3-generic-app-replaces-gideons-app.md)
- [ADR-0057](0057-generic-core-app-replaces-gideons-app.md), [ADR-0052](0052-robot-framework-bdd-tests.md)
