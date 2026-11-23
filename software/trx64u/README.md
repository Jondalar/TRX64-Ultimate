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

## Packing trx64u.cor

1. The SDK: `./fetch_sdk.sh --download` takes the pinned tarball from our registry (`sdk.pin` `sdk_package`, TREX generic
   `trx64-sdk/0.3.0-e4f99648`, a copy of MultiCore's `t239.3` build; their D26: we take the SDK from `t239.3` and keep it).
   `./fetch_sdk.sh <tarball>` takes a local file. A new pin: build or fetch the SDK at a `t239.3` commit (MultiCore's
   `tools/sdk_pack.sh`), upload it to `trx64-sdk/<version>-<commit8>`, put its sha256 and hashes into `sdk.pin`.
2. The bitstream: `c64_basic.bit` of our CI package `<branch>-<commit8>`:
   `curl -o c64_basic.bit http://gitea.example/api/packages/TREX/generic/trx64-ultimate/<branch>-<commit8>/c64_basic.bit`.
   It must have the RISC-V caches (U19, from `main` 23d25554 on); without them every menu step takes seconds.
3. `./pack_cor.sh --bit c64_basic.bit --bit-commit <commit8> --version <version>` builds the three libraries and the SID
   player's files, packs `build/cor/trx64u.cor` (`--out` for another name) and runs `cor_pack.py check` on it; it prints
   the size and sha256. By hand the version is `<firmware version>-dev+<commit8 of this tree>`, e.g. `0.3.1-dev+4f3ba9f5`.
4. On the board: FTP to `/SD/cores/<name>.cor` (`curl -T <file> ftp://<board>/SD/cores/<name>.cor`, then read it back and
   compare). Board steps are Alex's.

What goes in:

| Part | From |
|---|---|
| app core image (`coreapp.bin`), `cor_pack.py` | the SDK (`sdk.pin`) |
| libraries `trx64u`, `ring`, `monitor` | `build.sh` |
| record 6 (menu and API) | `base/menu.toml`, `base/desc.toml` |
| icon | `base/icon.rgb565`: TRX64's logo, a copy of MultiCore's `tools/core_logos/trx64/icon.rgb565` (sha256 `d521f548...`) |
| record 8 (files the core installs) | `sidplayer/sidcrt.bin`, `muscrt.bin`, `musplayer.bin`, assembled from `software/ultimate-app/software/6502/sidcrt` by the submodule's 64tass |
| bitstream | `--bit`, with `--bit-commit` |
| check of record 6 | `base/ops_trx64u.txt`: the operations and states the application exports, ours (U18 R6); taken from MultiCore's `tests/coreapp/golden/ops_trx64u.txt` at the pinned `e4f99648`. MultiCore names every op the base adds or drops in its note for that SDK commit (D25 adds `mouse.sens`, `mouse.accel`) |

The heap (MultiCore T239.3 D15, D11, D13): sized by trx64u's measured need, kept in the core's TOML (`heap`, as MultiCore's
`core.toml`) and passed on by `pack_cor.sh` as `--heap`, the SDK's interface (its tools read no heap key themselves). **Not measured yet** (open, U18): until it is, the image's own 0x7A0000 applies, as on the board. The SDK
0.3.0 image allows at most 0x7B7878 (image end 0x6B8788 + heap + stack reserve 0x20000 below 0xE90000); that image is
MultiCore's whole application with the other cores' code and about 1.7 MB of their static buffers and the old trx64u
monitor, a defect on their side; a base image without any core's code is announced (MultiCore, 2026-11-23).
`trx64u_u19.cor`, on the board on 2026-11-23, is what `pack_cor.sh` makes from the same inputs, byte for byte.
