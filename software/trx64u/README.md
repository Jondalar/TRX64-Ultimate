# trx64u: TRX64 Ultimate as a core of the MultiCore app core (U18)

The application part of the `trx64u` core: three libraries for MultiCore's generic app core, built here against the
TRX64 SDK (docs/specs/U18-trx64u-app-part-moves-here.md, D1-D5).

| Folder | Library | Holds |
|---|---|---|
| `base/` | `trx64u`, the base | the menu (`menu.toml`), its operations (`trx64u_ops`), the C64U settings (`u64cfg`) |
| `ring/` | `ring` | the Timewarp ring, its files (`.c64re`, `.c64rering`, `.trxring`) and its operations |
| `monitor/` | `monitor`, needs `ring` | the monitor, its RPC server on TCP 4312; the engine is trxmon, `software/ultimate-app/apps/trxmon/src` (D4: one source) |
| `gateware/` | - | `c64_basic.toml`: the TOML A of the bitstream (record version 7), the registers as the app's I/O bus sees them |
| `app/` | - | `toml_b.toml`: the TOML B of trx64u's app, the shared libraries of the SDK it is linked with (MultiCore T239.3 D31) |

The cartridge tool `trex_crt` stays a sub-app (D5), `software/ultimate-app/apps/trex_crt`.

## State

The three libraries build on MultiCore's dev SDK 0.4.1 (base 3.3, not released yet): every link closes, the constructor
check passes, and each packs into a `.u2a` within its declared budget. The core's description (`base/menu.toml`, the UI
TOML, and `base/desc.toml`, the API TOML) compiles into record 6. The SDK ships no app image (since 0.4.0):
`pack_cor.sh` links trx64u's app from the SDK's base archive with `gateware/c64_basic.toml` (TOML A) and
`app/toml_b.toml` (TOML B), runs the SDK's symbol check over it and packs `trx64u.cor` with the heap of word 0 (U18 §5
has the figures and what is open).
Earlier runs: ue2emu on MultiCore's shared 3.3 image and the board (2026-11-23, U18 §5).

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

`./build.sh [trx64u] [ring] [monitor]` builds the libraries in `build/<name>/` (copies of the sources; the monitor
also gets trxmon's files from the submodule). The driver table of `base/` and `ring/` is made there first from
`gateware/c64_basic.toml` (their Makefiles: `descrec.py drivers` of the SDK with the SDK's `maps/`; `DRV_TOOLS` and
`DRV_MAPS` name others). On a Mac it runs in the local `u64-build-env` container. `build.sh --check` only checks that
the unpacked SDK matches `sdk.pin`. The app is linked by `pack_cor.sh` (below), in `build/app/`.

trxmon itself builds as a library as it is (2026-11-22): every file compiles against the firmware's application ABI
(`software/appsdk/include`), and what is left undefined are only the platform calls `plat_*` and `srv_*`, which
`main.c` gives the firmware's trxmon and `monitor.cc` / `monsrv.cc` give the library.

## SDK

`sdk.pin` names the SDK version and the sha256 of its tarball. `./fetch_sdk.sh <tarball>` checks the tarball and unpacks
it into `.sdk/` (not in git). The compiler is the build image's `riscv32-unknown-elf-` (`u64-build-env`); a Mac's
`riscv64-elf-` has no newlib headers.

## Packing trx64u.cor

1. The SDK: `./fetch_sdk.sh --download` takes the pinned tarball from our registry (`sdk.pin` `sdk_package`, TREX generic
   `trx64-sdk/<version>-<commit8>`, a copy of MultiCore's `t239.3` build; their D26: we take the SDK from `t239.3` and keep it).
   `./fetch_sdk.sh <tarball>` takes a local file. A new pin: build or fetch the SDK at a `t239.3` commit (MultiCore's
   `tools/sdk_pack.sh`), upload it to `trx64-sdk/<version>-<commit8>`, put its sha256 and hashes into `sdk.pin`.
2. The bitstream: `c64_basic.bit` of our CI package `<branch>-<commit8>`:
   `curl -o c64_basic.bit http://gitea.example/api/packages/TREX/generic/trx64-ultimate/<branch>-<commit8>/c64_basic.bit`.
   It must have the RISC-V caches (U19, from `main` 23d25554 on); without them every menu step takes seconds.
3. `./pack_cor.sh --bit c64_basic.bit --bit-commit <commit8> --version <version>` builds the three libraries and the SID
   player's files, links the app in `build/app/` (record 6 compiled against the TOML B's sizes, then the SDK's
   `tools/app_build.py build` with `gateware/c64_basic.toml` and `app/toml_b.toml`), runs the SDK's symbol check
   (`libtab.py symcheck`: no object of the base uses a symbol of a linked library), packs `build/cor/trx64u.cor` (`--out`
   for another name) with the heap of word 0 and runs `cor_pack.py check` on it; it prints the heap figures, the size and
   the sha256. By hand the version is `<firmware version>-dev+<commit8 of this tree>`, e.g. `0.3.1-dev+4f3ba9f5`.
4. On the board: FTP to `/SD/cores/<name>.cor` (`curl -T <file> ftp://<board>/SD/cores/<name>.cor`, then read it back and
   compare). Board steps are Alex's.

What goes in:

| Part | From |
|---|---|
| the app (record 3) | linked by `pack_cor.sh` from the SDK's base archive (`sdk.pin`) with `gateware/c64_basic.toml` and `app/toml_b.toml` |
| `cor_pack.py`, `app_build.py`, the symbol check | the SDK (`sdk.pin`) |
| libraries `trx64u`, `ring`, `monitor` | `build.sh` |
| record 6 (menu and API) | `base/menu.toml`, `base/desc.toml` |
| the registers | `gateware/c64_basic.toml`: the app's and the libraries' driver table (`drvtab.h`) and the app's I/O addresses; no TOML A record in the `.cor` (record 6 holds a description or a TOML A, not both; the record type that carries it beside the description comes with a later MultiCore package: U18 §5) |
| icon | `base/icon.rgb565`: TRX64's logo, a copy of MultiCore's `tools/core_logos/trx64/icon.rgb565` (sha256 `d521f548...`) |
| record 8 (files the core installs) | `sidplayer/sidcrt.bin`, `muscrt.bin`, `musplayer.bin`, assembled from `software/ultimate-app/software/6502/sidcrt` by the submodule's 64tass |
| bitstream | `--bit`, with `--bit-commit` |
| check of record 6 | `base/ops_trx64u.txt`: the operations and states the application exports, ours (U18 R6); taken from MultiCore's `tests/coreapp/golden/ops_trx64u.txt` at `e4f99648` (SDK 0.3.0). It is a fixed list: the check compares record 6 with it, not with the app, and the app linked on the pinned SDK exports less than it lists (U18 §5) |

The heap (MultiCore T239.3 P4-1): word 0, the region left after the linked app and the stack reserve. `pack_cor.sh` passes
`--heap 0`, prints `cor_pack.py`'s figures and checks them against the image: heap = 0xE70000 - align16(E), E the linker's
`_end` of `build/app/app.elf`, 0xE70000 the end of the application region (`__heap_limit` 0xE90000) less the stack reserve
0x20000, both read from the image's heap record. The figure follows from what the app links (U18 §5 has the current one).
`trx64u_u19.cor` (board, 2026-11-23) was packed on SDK 0.3.0 with MultiCore's prebuilt app image; that path is gone.
