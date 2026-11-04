# 60. The monitor becomes a firmware service; trxmon.u2a is retired

Date: 2026-10-30

Status: Accepted

## Context

On the board Alex set a breakpoint, left the monitor and played: the stop never came the way he
expected, and opening the monitor again after a freeze went wrong. Reading the code agreed with
him. The breakpoint table is `M.bp[]` / `M.obs[]` in the app's RAM, and `api_trx_release()`
(`software/app_loader/app_api_net.cc`) wipes the exec bitmap, both watch units, the drive
comparator, CYC_CMP and OWNER when an app returns. Half a service existed — `trx->detach` kept
trxmon alive with armed stops, `trx->ui_request` brought its screen back — but it sat in the one
app area, so any other app took it, and a REST launch was refused outright.

Alex 2026-10-30: "BK und Observer sitzen im Monitor … ich denke, das ist schlechte Architektur und
wir brauchen eine Art Observer-Service, der dauerhaft läuft, und RPC und MonitorUI sind Clients
davon", and on where the server belongs: "Der Server sollte mit in die Firmware dann."

## Decision

The monitor, its JSON-RPC server and the stops move into the firmware as a service that starts with
the box and never ends. `trxmon.u2a` is retired: `apps/trxmon` keeps `src/` (which the firmware
compiles) and `host/` (the tests), but no Makefile and no manifest, so nothing builds a second copy
to fight the first for the debug block and for TCP 4312.

- The service owns the debug block. An app's `trx->acquire()` is refused while stops are armed
  instead of wiping them when it returns.
- A breakpoint stops the machine and opens the monitor, wherever the person was. Observers that
  count or log keep running.
- A C64 reset keeps the stops, a reboot does not; nothing is written to flash.
- The service's buffers live in DDR2 at `0x03800000..0x03A00000`, agreed with the MultiCore session
  and written into the contract, because they do not fit in the image's RAM.

## Consequences

Built 2026-10-30 (app repo `87d3cab8`, `3159bae8`, `46e182b8`). Measured: text +132 KB, data +35 KB,
`ultimate.app` 1,591,696 B of the 1,966,080 the APPL slot holds, and 510,976 B of buffers in the
window instead of in the image. Ctrl-O, Timewarp's C= + O and Developer > TRX64 Monitor all open the
same screen; the registry entries that launched the app are gone. The host tests still drive
`app_main`, so the app path stays covered even though no `.u2a` is shipped.

This reverses the placement T21 chose (the monitor as an app) and leaves
[ADR-0031](0031-trxmon-single-rpc-connection.md) standing: one RPC connection, same methods, same
port — only now the server is there before any client and stays after the last one.

## Links

- [docs/specs/U5-observer-service.md](../specs/U5-observer-service.md)
- [docs/specs/T21-monitor.md](../specs/T21-monitor.md)
- [docs/contracts/trx64u-firmware-handover.md](../contracts/trx64u-firmware-handover.md) section 6
