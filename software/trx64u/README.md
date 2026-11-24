# trx64u: TRX64 Ultimate as a core of the MultiCore app core (U18)

The application part of the `trx64u` core: three libraries and a sub-app for MultiCore's generic app core, built here
against the TRX64 SDK (docs/specs/U18-trx64u-app-part-moves-here.md, D1-D5).

| Folder | Library | Holds |
|---|---|---|
| `base/` | `trx64u`, the base | the menu (`menu.toml`), its operations (`trx64u_ops`), the C64U settings (`u64cfg`) |
| `ring/` | `ring` | the Timewarp ring, its files (`.c64re`, `.c64rering`, `.trxring`) and its operations; the picture under the Timewarp page, the frame and the bar (`twstill.cc`, `twbar.cc`) |
| `monitor/` | `monitor`, needs `ring` | the monitor, its RPC server on TCP 4312; the engine is trxmon, `software/ultimate-app/apps/trxmon/src` (D4: one source) |
| `gateware/` | - | `c64_basic.toml`: the TOML A of the bitstream (record version 7), the registers as the app's I/O bus sees them |
| `app/` | - | `toml_b.toml`: the TOML B of trx64u's app, the shared libraries of the SDK it is linked with (MultiCore T239.3 D31) |
| `apps/trex_crt/` | sub-app `trex_crt` | TREX CRT, the cartridge tools (Dump, Inspect, Flash; Write to Cartridge on `.crt`), loaded on demand (D5) |
| `tools/` | - | `emu_ops.py`: runs `trx64u.cor` in ue2emu and exports what the running app registers; `boundary.py` |

The sub-app is MultiCore's port onto ABI 3 (TRX64U-MultiCore `t239.3` `8ff28ba9`, `apps/trex_crt`) of our firmware's
tool at submodule `aa2ef5dc` (its README lists what the port changed), moved here by MultiCore T239.3 P2-5 and built
against LibApi 3.3 with the SDK's `mk/app.mk`; the function table comes with MultiCore's P2-6. Softboot is not part of
trx64u (Alex, 2026-11-24): it starts the firmware application inside a `.cfw` on the running bitstream, and c64_basic
under this app is not the bitstream that application needs. It stays in the firmware build (`software/ultimate-app/apps/softboot`). Online search
is MultiCore's sub-app `online.u2a` (pinned in `sdk.pin`, carried in record 7).

## State

The three libraries and the sub-app build on MultiCore's dev SDK 0.4.5 (base 3.3, not released yet): every link
closes, the constructor check passes, and each library packs into a `.u2a` within its declared budget (the sub-app declares
none). The core's description (`base/menu.toml`, the UI
TOML, and `base/desc.toml`, the API TOML) compiles into record 6. The SDK ships no app image (since 0.4.0):
`pack_cor.sh` links trx64u's app from the SDK's base archive with `gateware/c64_basic.toml` (TOML A) and
`app/toml_b.toml` (TOML B), runs the SDK's symbol check over it and packs `trx64u.cor` with the heap of word 0 (U18 §5
has the figures and what is open). The ring library's Timewarp picture (U18 T-3) needs SDK 0.4.3 or later
(`api->overlay->frame`, `still_palette`), and 0.4.5 for its bar on the smaller canvases (`canvas()`) and as the only
bar (the base draws none since 0.4.5); the pin is 0.4.5 (U18 §5).
`pack_cor.sh --emu` runs the packed `trx64u.cor` in ue2emu 0.7.4 on the Mac: BASIC READY from the started core, and what
the app registers equals `base/ops_trx64u.txt` (2026-11-24, U18 §5 step 2b). Earlier runs: ue2emu on MultiCore's shared
3.3 image and the board (2026-11-23, U18 §5).

The registers (MultiCore T239.3 P1-7, 2026-11-24): `gateware/c64_basic.toml` is c64_basic's TOML A, written from what the
RTL of `designs/c64_basic` decodes, one block per register range and driver (D34), each with the RTL lines that decode it.
`base/` and `ring/` take every register address from its driver table, `drvtab.h` (`DRV_<block>_<property>_*`, api.md
6.17), which the build makes with the SDK's `descrec.py drivers`; `c64u_regs.h` is gone, and `rtl_check.py lint-src` finds
no literal I/O address in `base/`, `ring/` or `monitor/` (the SHA-256 constant of `ring/trx_sha256.c` allowed); the lint
stage runs it (`ci/lint.sh`). `base/desc.toml` keeps record 6's data and four register windows whose owner is open
(audio_mixer, speaker_mixer, c64_palette, hdmi_palette; U18 §5). The monitor's engine (trxmon, submodule) still reaches
the TRX block through `api->trx->regs` and its own offsets (U18 §5).

The sources of `base/`, `ring/` and `monitor/` started as MultiCore's, copied from TRX64U-MultiCore commit `3b8e2955`
(branch `t239.3-sdk`, the commit the SDK 0.1.0 was packed from; `coreapp/src` and `coreapp/cores/trx64u/menu.toml`), and
were ported onto the SDK's public API on 2026-11-22. What they took from outside the SDK before the port is measured in
`BOUNDARY.md` (`tools/boundary.py`). The ring files were an older copy of the firmware's `trx_ring` / `trx_c64re` /
`trx_trxring` code; they were brought to the firmware's state the same night and are the master since (U18 D6: the
firmware's copy is frozen, bug fixes only, each also here).

## Build

`./build.sh [trx64u] [ring] [monitor] [trex_crt]` builds the libraries and the sub-app in `build/<name>/` (copies of the sources; the monitor
also gets trxmon's files from the submodule). The driver table of `base/` and `ring/` is made there first from
`gateware/c64_basic.toml` (their Makefiles: `descrec.py drivers` of the SDK with the SDK's `maps/`; `DRV_TOOLS` and
`DRV_MAPS` name others). On a Mac it runs in the local `u64-build-env` container. `build.sh --check` only checks that
the unpacked SDK matches `sdk.pin`. The app is linked by `pack_cor.sh` (below), in `build/app/`. Each build folder
keeps the sha256 of the SDK tarball it was built with in `.sdk`; a folder of another SDK starts clean (its objects'
`.d` files name the old SDK's headers, which stay in `.sdk/`, so make would take them as up to date).

trxmon itself builds as a library as it is (2026-11-22): every file compiles against the firmware's application ABI
(`software/appsdk/include`), and what is left undefined are only the platform calls `plat_*` and `srv_*`, which
`main.c` gives the firmware's trxmon and `monitor.cc` / `monsrv.cc` give the library.

## SDK

`sdk.pin` names the SDK version and the sha256 of its tarball, and MultiCore's online sub-app with its own
commit (`online_*`: 8ff28ba9; apps/online has not changed since). `./fetch_sdk.sh <tarball> [<online.u2a>]` checks both and puts them into `.sdk/` (not in git). The compiler is the build image's `riscv32-unknown-elf-` (`u64-build-env`); a Mac's
`riscv64-elf-` has no newlib headers.

## Packing trx64u.cor

1. The SDK and the online sub-app: `./fetch_sdk.sh --download` takes the pinned tarball and `online.u2a` from our registry
   (`sdk.pin` `sdk_package`, TREX generic `trx64-sdk/<version>-<commit8>`, and `online_package`, `trx64-online/<version>-<commit8>`:
   copies of MultiCore's `t239.3` build; their D26: we take the SDK from `t239.3` and keep it). `./fetch_sdk.sh <tarball>
   <online.u2a>` takes local files (MultiCore's `.ci/sdk/` holds both). A new pin: build or fetch the SDK and `online.u2a` at a
   `t239.3` commit (MultiCore's `tools/sdk_pack.sh`, `ci/trxapps.sh`), upload them, put their sha256 and hashes into `sdk.pin`.
2. The bitstream: `c64_basic.bit` of our CI package `<branch>-<commit8>`:
   `curl -o c64_basic.bit http://gitea.example/api/packages/TREX/generic/trx64-ultimate/<branch>-<commit8>/c64_basic.bit`.
   It must have the RISC-V caches (U19, from `main` 23d25554 on); without them every menu step takes seconds.
3. `./pack_cor.sh --bit c64_basic.bit --bit-commit <commit8> --version <version> [--emu <ue2emu>]` builds the three libraries,
   the sub-app and the SID player's files, links the app in `build/app/` (record 6 compiled against the TOML B's sizes, then the SDK's
   `tools/app_build.py build` with `gateware/c64_basic.toml` and `app/toml_b.toml`), runs the SDK's symbol check
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
| record 7: the online sub-app `online.u2a` | MultiCore's build of `t239.3` `8ff28ba9` (`sdk.pin` `online_*`, `fetch_sdk.sh`): the F3 tab Online and `online.*` |
| record 6 (menu and API) | `base/menu.toml`, `base/desc.toml` |
| the registers | `gateware/c64_basic.toml`: the app's and the libraries' driver table (`drvtab.h`) and the app's I/O addresses; no TOML A record in the `.cor` (record 6 holds a description or a TOML A, not both; the record type that carries it beside the description comes with a later MultiCore package: U18 §5) |
| icon | `base/icon.rgb565`: TRX64's logo, a copy of MultiCore's `tools/core_logos/trx64/icon.rgb565` (sha256 `d521f548...`) |
| record 8 (files the core installs) | `sidplayer/sidcrt.bin`, `muscrt.bin`, `musplayer.bin`, assembled from `software/ultimate-app/software/6502/sidcrt` by the submodule's 64tass; `apps/trex_crt.u2a` (`build.sh`), which the base finds in the core's folder `apps/` (from the start after they are written: the base scans the folder before it installs record 8, U18 §5) |
| bitstream | `--bit`, with `--bit-commit` |
| the operations | `base/ops_trx64u.txt`: what the running app exports (REST `/v1/ops`, `/v1/state`), ours (U18 R6), written from `tools/emu_ops.py` (MultiCore's `tests/ui/export.py` format). `cor_pack.py --ops` checks record 6's names and bindings against it at every pack; `--emu` checks the running app against it both ways. The SDK's link step makes no operation table, so the list is measured; CI has no emu stage yet (U18 §5) |

The heap (MultiCore T239.3 P4-1): word 0, the region left after the linked app and the stack reserve. `pack_cor.sh` passes
`--heap 0`, prints `cor_pack.py`'s figures and checks them against the image: heap = 0xE70000 - align16(E), E the linker's
`_end` of `build/app/app.elf`, 0xE70000 the end of the application region (`__heap_limit` 0xE90000) less the stack reserve
0x20000, both read from the image's heap record. The figure follows from what the app links (U18 §5 has the current one).
`trx64u_u19.cor` (board, 2026-11-23) was packed on SDK 0.3.0 with MultiCore's prebuilt app image; that path is gone.
