# 59. The T135 installer is the updater for good, and the application version is ours

Date: 2026-10-30

Status: Accepted

## Context

The updater does not live in this repository. `update_trx.cc` sits in the app fork `TREX/1541ultimate`, which this repository consumes as the submodule `software/ultimate-app` and `TREX/TRX64U-MultiCore` as `apps/ultimate`. The MultiCore session built the installer of its T135/T132 work on the branch `t135-installer` (tip `32e5fd5f`): it asks where the cores go, packs the backed-up firmware as a large `.cor`, writes `/Flash/boot.cfg` and asks two warranty questions. `trx64/main` stood at `aa2ef5dc`, without any of it, and `main` here pinned `234a0abb`.

The application also still called itself by Gideon's number, `APPL_VERSION_NUMBER "3.15"` in `software/application/versions.h`, although the tree has been ours for months.

## Decision

Alex 2026-10-30 (translated; "Der updater soll dauerhaft verwendet werden", "da steht Gideons Versionsnummer drin … Die muss gegen unsere gewechselt werden ab jetzt … 0.2.0"):

1. The T135 installer is the updater of TRX64 Ultimate from now on, not a one-off for a test package.
2. The application carries our own version, `0.2.0`, in place of Gideon's `3.15`.
3. It must not break MultiCore's updater build ("das DARF nicht kaputt gehen dadurch").

Carried out as: `trx64/main` fast-forwarded `aa2ef5dc` → `32e5fd5f` (the installer), then `558a8ed5` on top for the version; `main` here pins `558a8ed5`. The branch `t135-installer` is left exactly where it was.

## Consequences

- MultiCore is unaffected: it pins the commit `32e5fd5f`, a SHA, so a moving branch label cannot change what it checks out. Its `.gitmodules` says `branch = trx64/main`, which before the fast-forward pointed at a commit *without* the installer — `git submodule update --remote` there would have thrown the installer out. After it, that command is harmless. Confirmed by the MultiCore session on 2026-10-30: nothing on its side resolves the branch, and the `aa2ef5dc` mentions in its `PROVENANCE.md` and `apps/*/README.md` stay true because the five changed files are updater-only.
- The installer degrades by itself for our packages. `package_manages_cores()` searches the packaged application for the literal `/Flash/boot.cfg`, which only the MultiCore host contains; ours does not. So a TRX64 Ultimate package gets one popup ("About to update/restore. Continue?"), the standard backup and the plain T49 flash — no warranty questions, no SD/USB cores question, no large backup `.cor`, no `/Flash/boot.cfg`.
- The version may move backwards without harm: `need_update()` compares the two strings for inequality only (`strcmp != 0`), never for order, so an update still offers itself over an installed 3.15. `product.cc` budgets 11 characters for `APPL_VERSION_ASCII` and `0.2.0` is 5. `tests/lib/machine.py` `classify()` keys on the product string; the firmware string decides nothing there, it only names the machine in a skip reason.
- `0.2.0` is what the system info page, `/v1/info` `firmware_version`, the LVGL version line and the flash image header report, and what a backup `.cor` records as the version of the firmware it came from. `BOOT_VERSION` and `FPGA_VERSION` in `versions.h` are untouched: they name other things.

## Links

- [T49 update packages](../specs/T49-update-packages.md#status-and-history)
- MultiCore `docs/specs/T135-direct-boot.md` D8/D8a/D9, `docs/todo/T132.md`
- App fork commits `32e5fd5f` (installer), `558a8ed5` (version)
