# trx64u: TRX64 Ultimate as a core of the MultiCore app core (U18)

The application part of the `trx64u` core: three libraries and two sub-apps for MultiCore's generic app core, and the UCI
library linked into trx64u's app, built here against the TRX64 SDK (docs/specs/U18-trx64u-app-part-moves-here.md, D1-D5).

| Folder | Library | Holds |
|---|---|---|
| `base/` | `trx64u`, the base | the menu (`menu.toml`), its operations (`trx64u_ops`), the C64U settings (`u64cfg`) |
| `ring/` | `ring` | the Timewarp ring, its files (`.c64re`, `.c64rering`, `.trxring`) and its operations; the picture under the Timewarp page, the frame and the bar (`twstill.cc`, `twbar.cc`) |
| `uci/` | `uci`, linked into the app | the Ultimate Command Interface ($DF1C): DOS targets 1 and 2, NETWORK 3, CONTROL 4, the slot registers and the export `uci`; MultiCore's `uci.cc`, `uci_slot.cc`, `uci_exports.cc` and headers as `trxuci*` (PROVENANCE in each), ours since the owner split (U18 §5); the command interface server and its NETWORK target from SDK 0.4.6's hand-over (`command_intf.cc`, `network_target.cc`, T239.3 D48), its registers from the TOML A's block `cmd_if`, and six firmware headers the SDK no longer ships |
| `monitor/` | `monitor`, needs `ring` | the monitor, its RPC server on TCP 4312; the engine is trxmon, `trxmon/` |
| `trxmon/` | part of `monitor` | trxmon, the monitor's engine (mon, mach, ring, trace, rpc, ui, ...) and `trx_lens.*`: moved from the firmware fork `TREX/1541ultimate` at 0103c585 (U20 A2; `apps/trxmon/src` without `main.c`, `libc_min.c` and `trxsvc.h`, `software/io/c64/trx_lens.*`), Alex's own code; the only copy this tree builds, no longer one source with the firmware's (each file names its origin) |
| `fwabi/` | - | the firmware's application ABI (major 2: `app_api.h`, `u2p_app.h`, `u2p_app_api.h`), which trxmon, `monitor/monitor.cc` and `ring/ring_api.cc` read: moved from the fork at 0103c585 (U20 A2; `software/app_loader/app_api.h`, `software/appsdk/include/`), Alex's own code |
| `gateware/` | - | `c64_basic.toml`: the TOML A of the bitstream (record version 7), the registers as the app's I/O bus sees them |
| `app/` | - | `toml_b.toml`: the TOML B of trx64u's app, the libraries it is linked with: the SDK's shared ones and `uci/` (MultiCore T239.3 D31) |
| `apps/trex_crt/` | sub-app `trex_crt` | TREX CRT, the cartridge tools (Dump, Inspect, Flash; Write to Cartridge on `.crt`), loaded on demand (D5) |
| `apps/online/` | sub-app `online` | the Assembly64 / CommoServe search: the F3 tab Online, `online.*` and the folder `/Online` (resident, record 7); MultiCore's `t239.3` `6e280710` `apps/online`, ours since U20 D22 (its README) |
| `tools/` | - | `emu_ops.py`: runs `trx64u.cor` in ue2emu and exports what the running app registers; `boundary.py` |

TREX CRT is MultiCore's port onto ABI 3 (TRX64U-MultiCore `t239.3` `8ff28ba9`, `apps/trex_crt`) of our firmware's
tool at fork commit `aa2ef5dc` (its README lists what the port changed), moved here by MultiCore T239.3 P2-5 and built
against LibApi 3.3 with the SDK's `mk/app.mk`; the function table comes with MultiCore's P2-6. Softboot is not part of
trx64u (Alex, 2026-11-24): it starts the firmware application inside a `.cfw` on the running bitstream, and c64_basic
under this app is not the bitstream that application needs. It stays in the firmware build (`TREX/1541ultimate:apps/softboot`); it is not
ours to ship (U20 D22). The online search is a copy of MultiCore's `apps/online` at `t239.3` `6e280710` (U20 D22, MultiCore T261 D6: their
SDK lane no longer builds it), built here and carried in record 7; its build equals the `online.u2a` that `sdk.pin` pinned before, byte for byte.
Both sub-apps are assets of the `fw-<branch>-<sha8>` pre-release beside `trx64u.cor` (MultiCore's package takes them from there).

## State

The three libraries and the two sub-apps build on MultiCore's dev SDK 0.4.6 (base 3.4, not released yet): every link
closes, the constructor check passes, and each library packs into a `.u2a` within its declared budget (TREX CRT declares
none, the online search heap_keep 0 and heap_peak 16 MiB). The core's description (`base/menu.toml`, the UI
TOML, and `base/desc.toml`, the API TOML) compiles into record 6. The SDK ships no app image (since 0.4.0):
`pack_cor.sh` links trx64u's app from the SDK's base archive with `gateware/c64_basic.toml` (TOML A) and
`app/toml_b.toml` (TOML B), runs the SDK's symbol check over it and packs `trx64u.cor` with the heap of word 0 (U18 §5
has the figures and what is open). The ring library's Timewarp picture (U18 T-3) needs SDK 0.4.3 or later
(`api->overlay->frame`, `still_palette`), and 0.4.5 for its bar on the smaller canvases (`canvas()`) and as the only
bar (the base draws none since 0.4.5); the pin is 0.4.6 (U18 §5).
`pack_cor.sh --emu` runs the packed `trx64u.cor` in ue2emu 0.7.4 on the Mac: BASIC READY from the started core, and what
the app registers equals `base/ops_trx64u.txt` (2026-11-24, U18 §5 step 2b; on 0.4.6 too, and the UCI answers the DOS target from our own server,
U18 §5). Earlier runs: ue2emu on MultiCore's shared
3.3 image and the board (2026-11-23, U18 §5). With the submodule empty and the SID player gone (U20 A2, A3, 2026-11-24) the
build, the pack and `--emu` pass the same way: 388 lines, none missing either way.

The SID and music player is gone (Alex, U20 D6, A3): no `.sid`, `.mus` or `.str` file types, no operations `c64.sid_play`,
`c64.sid_songs`, `c64.sid_valid`, `c64.sid_info` and `dma.sidcrt`, no settings group "SID player behavior" (its two settings were read
only by the player's autoconfig), no `sidplayer/` files in record 8, and the app is linked without the SDK library `sid`. The DMA
service's SOCKET_CMD_LOADSIDCRT finds no operation and is logged and dropped. The machine's SIDs stay: the sockets, the reDIP:SIDs,
reSID8, their settings and the socket detection.

The registers (MultiCore T239.3 P1-7, 2026-11-24): `gateware/c64_basic.toml` is c64_basic's TOML A, written from what the
RTL of `designs/c64_basic` decodes, one block per register range and driver (D34), each with the RTL lines that decode it.
`base/` and `ring/` take every register address from its driver table, `drvtab.h` (`DRV_<block>_<property>_*`, api.md
6.17), which the build makes with the SDK's `descrec.py drivers`; `c64u_regs.h` is gone, and `rtl_check.py lint-src` finds
no literal I/O address in `base/`, `ring/`, `monitor/` or `uci/` (the SHA-256 constant of `ring/trx_sha256.c` allowed); the lint
stage runs it (`ci/lint.sh`). `trxmon/` is not in it: `hw_trx.c` names the firmware's C64 windows by four literal addresses, and
`ws.c`'s SHA-1 constant 0x10325476 lies in the I/O space (U20 A2, measured 2026-11-24). The owner split (Alex, 2026-11-24, U18 §5): the mixer's and the speaker's gains, both
palettes, reSID8, the command interface and the GCR coder are core blocks, their properties read from the RTL, and
`trx_pixel` is the base's; `base/desc.toml` keeps record 6's data and no register window. A decided gap (Alex): trx64u
has no Volume and no Tone of the base, because c64_basic has no RTL for them (no block `audio_master`). The monitor's engine (trxmon, `trxmon/`) still reaches
the TRX block through `api->trx->regs` and its own offsets (U18 §5).

The sources of `base/`, `ring/` and `monitor/` started as MultiCore's, copied from TRX64U-MultiCore commit `3b8e2955`
(branch `t239.3-sdk`, the commit the SDK 0.1.0 was packed from; `coreapp/src` and `coreapp/cores/trx64u/menu.toml`), and
were ported onto the SDK's public API on 2026-11-22. What they took from outside the SDK before the port is measured in
`BOUNDARY.md` (`tools/boundary.py`). The ring files were an older copy of the firmware's `trx_ring` / `trx_c64re` /
`trx_trxring` code; they were brought to the firmware's state the same night and are the master since (U18 D6: the
firmware's copy is frozen, bug fixes only, each also here).

## Build

`./build.sh [trx64u] [ring] [monitor] [trex_crt] [online]` builds the libraries and the two sub-apps in `build/<name>/` (copies of the sources; the monitor
also gets `trxmon/`; the monitor and the ring library read `fwabi/`). Nothing of the firmware submodule is read (U20 A2). The driver table of `base/` and `ring/` is made there first from
`gateware/c64_basic.toml` (their Makefiles: `descrec.py drivers` of the SDK with the SDK's `maps/`; `DRV_TOOLS` and
`DRV_MAPS` name others). On a Mac it runs in the local `u64-build-env` container. `build.sh --check` only checks that
the unpacked SDK matches `sdk.pin`. The app is linked by `pack_cor.sh` (below), in `build/app/`. Each build folder
keeps the sha256 of the SDK tarball it was built with in `.sdk`; a folder of another SDK starts clean (its objects'
`.d` files name the old SDK's headers, which stay in `.sdk/`, so make would take them as up to date).

trxmon itself builds as a library as it is (2026-11-22): every file compiles against the firmware's application ABI
(`fwabi/`), and what is left undefined are only the platform calls `plat_*` and `srv_*`, which the fork's `main.c` gives
the firmware's trxmon and `monitor/lib_monitor.cc` / `monitor/monsrv.cc` give the library.

## SDK

`sdk.pin` names the SDK version and the sha256 of its tarball. `./fetch_sdk.sh <tarball>` checks it and unpacks it into `.sdk/` (not in git);
the online sub-app is no longer pinned (U20 D22: `apps/online`, built against this SDK's `online_api.h`: 0.4.6's base takes no smaller table,
U18 §5). The compiler is the build image's `riscv32-unknown-elf-` (`u64-build-env`); a Mac's
`riscv64-elf-` has no newlib headers.

## Packing trx64u.cor

1. The SDK: `./fetch_sdk.sh --release` takes the pinned tarball from MultiCore's SDK release (`sdk.pin` `sdk_release`, U20 D11),
   `./fetch_sdk.sh --download` from our Gitea registry (`sdk.pin` `sdk_package`, TREX generic `trx64-sdk/<version>-<commit8>`: a copy of
   MultiCore's `t239.3` build; their D26: we take the SDK from `t239.3` and keep it), `./fetch_sdk.sh <tarball>` a local file
   (MultiCore's `.ci/sdk/` holds them). A new pin: fetch the SDK of a MultiCore release, put its sha256 and hashes into `sdk.pin`, and check
   that `apps/online` still builds against it (the base takes the table "online" only when it is at least its own `OnlineApi`'s size).
2. The bitstream: `c64_basic.bit` of our CI package `<branch>-<commit8>`:
   `curl -o c64_basic.bit $GITEA_URL/api/packages/TREX/generic/trx64-ultimate/<branch>-<commit8>/c64_basic.bit` (`GITEA_URL`: our
   Gitea server, the `url:` of the tea login).
   It must have the RISC-V caches (U19, from `main` 23d25554 on); without them every menu step takes seconds.
3. `./pack_cor.sh --bit c64_basic.bit --bit-commit <commit8> --version <version> [--emu <ue2emu>]` builds the three libraries
   and the two sub-apps, links the app in `build/app/` (record 6 compiled against the TOML B's sizes, then the SDK's
   `tools/app_build.py build` with `gateware/c64_basic.toml`, `app/toml_b.toml` and the sources of `uci/`), runs the SDK's symbol check
   (`libtab.py symcheck`: no object of the base uses a symbol of a linked library), packs `build/cor/trx64u.cor` (`--out`
   for another name) with the heap of word 0 and runs `cor_pack.py check` on it; it prints the heap figures, the size and
   the sha256. By hand the version is `<firmware version>-dev+<commit8 of this tree>`, e.g. `0.3.1-dev+4f3ba9f5`. With `--emu`
   (the Mac's ue2emu) it then runs the packed `.cor` in the emulator and compares what the running app registers with
   `base/ops_trx64u.txt` (`tools/emu_ops.py --check`, below).
4. On the board: FTP to `/SD/cores/<name>.cor` (`curl -T <file> ftp://<board>/SD/cores/<name>.cor`, then read it back and
   compare). Board steps are Alex's.

What goes in:

| Part | From |
|---|---|
| the app (record 3) | linked by `pack_cor.sh` from the SDK's base archive (`sdk.pin`) with `gateware/c64_basic.toml` and `app/toml_b.toml` |
| `cor_pack.py`, `app_build.py`, the symbol check | the SDK (`sdk.pin`) |
| record 7: libraries `trx64u`, `ring`, `monitor` | `build.sh` |
| record 7: the online sub-app `online.u2a` | `build.sh` (`apps/online`, U20 D22): the F3 tab Online and `online.*` |
| record 6 (menu and API) | `base/menu.toml`, `base/desc.toml` |
| the registers | `gateware/c64_basic.toml`: the app's and the libraries' driver table (`drvtab.h`) and the app's I/O addresses; no TOML A record in the `.cor` (record 6 holds a description or a TOML A, not both; the record type that carries it beside the description comes with a later MultiCore package: U18 §5) |
| icon | `base/icon.rgb565`: TRX64's logo, a copy of MultiCore's `tools/core_logos/trx64/icon.rgb565` (sha256 `d521f548...`) |
| record 8 (files the core installs) | `apps/trex_crt.u2a` (`build.sh`), which the base finds in the core's folder `apps/` from the first start: it installs record 8 before it scans the folder (MultiCore's `coreapp/src/run.cc` at SDK 0.4.6's source commit 6e280710, :778 and :786) |
| bitstream | `--bit`, with `--bit-commit` |
| the operations | `base/ops_trx64u.txt`: what the running app exports (REST `/v1/ops`, `/v1/state`), ours (U18 R6), written from `tools/emu_ops.py` (MultiCore's `tests/ui/export.py` format). `cor_pack.py --ops` checks record 6's names and bindings against it at every pack; `--emu` checks the running app against it both ways. The SDK's link step makes no operation table, so the list is measured; the Gitea pipeline runs no emulator, the GitHub matrix does (`ci/lane.sh core:trx64u emu`, U20 B) |

The heap (MultiCore T239.3 P4-1): word 0, the region left after the linked app and the stack reserve. `pack_cor.sh` passes
`--heap 0`, prints `cor_pack.py`'s figures and checks them against the image: heap = 0xE70000 - align16(E), E the linker's
`_end` of `build/app/app.elf`, 0xE70000 the end of the application region (`__heap_limit` 0xE90000) less the stack reserve
0x20000, both read from the image's heap record. The figure follows from what the app links (U18 §5 has the current one).
`trx64u_u19.cor` (board, 2026-11-23) was packed on SDK 0.3.0 with MultiCore's prebuilt app image; that path is gone.
