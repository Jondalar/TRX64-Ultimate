# trx64u: TRX64 Ultimate as a core of the MultiCore app core (U18)

The application part of the `trx64u` core: three libraries for MultiCore's generic app core, built here against the
TRX64 SDK (docs/specs/U18-trx64u-app-part-moves-here.md, D1-D5).

| Folder | Library | Holds |
|---|---|---|
| `base/` | `trx64u`, the base | the menu (`menu.toml`), its operations (`trx64u_ops`), the C64U settings (`u64cfg`) |
| `ring/` | `ring` | the Timewarp ring, its files (`.c64re`, `.c64rering`, `.trxring`) and its operations |
| `monitor/` | `monitor`, needs `ring` | the monitor, its RPC server on TCP 4312; the engine is trxmon, `software/ultimate-app/apps/trxmon/src` (D4: one source) |

The cartridge tool `trex_crt` stays a sub-app (D5), `software/ultimate-app/apps/trex_crt`.

## State

The three libraries build on MultiCore's dev SDK 0.3.0 (base 3.3, not released yet): every link closes, the constructor
check passes, and each packs into a `.u2a` within its declared budget. The core's description (`base/menu.toml`, the UI
TOML, and `base/desc.toml`, the API TOML) compiles into record 6, and `cor_pack.py pack-large` packs a whole
`trx64u.cor` from the SDK, our three libraries, the two TOMLs and the release bitstream (U18 §5 has the command). First
run in ue2emu on MultiCore's shared 3.3 image (2026-11-23): the core is identified from our record 6 and all three
libraries load; the start stops on gaps in the base (U18 §5). Not on the board yet.

The sources of `base/`, `ring/` and `monitor/` started as MultiCore's, copied from TRX64U-MultiCore commit `3b8e2955`
(branch `t239.3-sdk`, the commit the SDK 0.1.0 was packed from; `coreapp/src` and `coreapp/cores/trx64u/menu.toml`), and
were ported onto the SDK's public API on 2026-11-22. What they took from outside the SDK before the port is measured in
`BOUNDARY.md` (`tools/boundary.py`). The ring files were an older copy of the firmware's `trx_ring` / `trx_c64re` /
`trx_trxring` code; they were brought to the firmware's state the same night and are the master since (U18 D6: the
firmware's copy is frozen, bug fixes only, each also here).

## Build

`./build.sh [trx64u] [ring] [monitor]` builds the libraries in `build/<name>/` (copies of the sources; the monitor
also gets trxmon's files from the submodule). On a Mac it runs in the local `u64-build-env` container. `build.sh --check`
only checks that the unpacked SDK matches `sdk.pin`.

trxmon itself builds as a library as it is (2026-11-22): every file compiles against the firmware's application ABI
(`software/appsdk/include`), and what is left undefined are only the platform calls `plat_*` and `srv_*`, which
`main.c` gives the firmware's trxmon and `monitor.cc` / `monsrv.cc` give the library.

## SDK

`sdk.pin` names the SDK version and the sha256 of its tarball. `./fetch_sdk.sh <tarball>` checks the tarball and unpacks
it into `.sdk/` (not in git). The compiler is the build image's `riscv32-unknown-elf-` (`u64-build-env`); a Mac's
`riscv64-elf-` has no newlib headers.
