# trx64g: TRX64 Ultimate (Gideon app) as a large core

`trx64g.cor` is the large core "TRX64 Ultimate (Gideon app)": our `c64_basic` bitstream with our Gideon-based firmware
application (`ultimate.app`, built from `software/ultimate-app`), both files of one version of our CI package
`trx64-ultimate`. MultiCore's host starts it from its boot menu. MultiCore packed it until T239.3 D46 (Alex, 2026-11-24:
"Yes, of course, please. If they pack the trx64u cor, the trx64g cor is not much harder."). Now our CI packs it beside
`trx64u.cor`, and MultiCore's package takes the file by its sha256.

## The files: MultiCore's, copied

Alex (2026-11-24): take MultiCore's scripts as they are. Every file here is a copy from TRX64U-MultiCore, branch `t239.3`,
commit `cfa7872a`; `pack_import_cor.sh` from `0242d0ca5`, which changed only its header comment. Each file's PROVENANCE header names what changed, and that is only what this tree forces: paths,
`cor_pack.py` taken from the pinned SDK, and the pin values. A `diff` against MultiCore's file shows exactly those lines.
The comments inside are MultiCore's, so they name MultiCore's tree.

| File | Does | MultiCore's file |
|---|---|---|
| `pack_import_cor.sh` | checks the pin's two files against their sha256, cuts the `.app`'s 12-byte record header, checks the version against the manifest, calls `pack_large_cor.sh` | `tools/pack_import_cor.sh` |
| `pack_large_cor.sh` | `cor_pack.py pack-large`, then `cor_pack.py check`; `cor_pack.py` is the pinned SDK's (`software/trx64u/sdk.pin`), the one `trx64u.cor` is packed with | `tools/pack_large_cor.sh` |
| `core_toml.py` | called by `pack_large_cor.sh`; trx64g has no descriptor here, so it finds none | `tools/core_toml.py` |
| `version.sh` | `version.sh with <x.y.z>`: `x.y.z` on a tag build, else `x.y.z-dev+<commit8>` | `ci/version.sh` |
| `pkg.env` | `FW_PKG=trx64-ultimate`, the package a pin is downloaded from when no local files are given | `ci/pkg.env` |
| `trx64g.pin` | `ID`, `NAME`, `BIT_FILE`, `APP_FILE`, `ICON`. The package values are empty here, because each run brings its own | `release/imports/trx64g.pin` |
| `icon.rgb565` | TRX64's logo, 32 x 32 RGB565, sha256 `d521f548...`, the same bytes as `software/trx64u/base/icon.rgb565` | `tools/core_logos/trx64/icon.rgb565` |

Why the folder is a separate one: trx64g is a core of its own, with its own inputs, and none of trx64u's libraries.
`software/trx64u` is the app-core subproject. The two share only the SDK pin, so both `.cor` files of a run are packed
by the same `cor_pack.py`.

## In CI

`ci/cor.sh` (stage `cor`, after `package`) does the following:

1. It fetches `manifest.json`, `c64_basic.bit` and `ultimate.app` of the package version `<branch>-<commit8>` and
   checks both files against the manifest.
2. It writes the pin of the run: `trx64g.pin` plus `VERSION=<commit8>`, `SEMVER=<the manifest's "version">`,
   `PKG_VERSION=<branch>-<commit8>`, `BIT_SHA256` and `APP_SHA256` from the manifest.
3. It runs `pack_import_cor.sh` with the three local files and `--app-commit` (the manifest's `app_commit`, 8 hex).
4. It publishes `trx64g_<commit8>.cor` beside `trx64u_<commit8>.cor`, both with their lines in `SHA256SUMS`, and
   removes the older `.cor` files of the version and of `<branch>-latest`.

The version inside is `x.y.z-dev+<commit8>` (`x.y.z` on a tag build). `x.y.z` is the manifest's `"version"`, which is
the application's `APPL_VERSION_NUMBER` (`ci/app.sh`), and `<commit8>` is the package version's commit. That is
MultiCore's `version.sh with` and the rule of the contract's §2. In the `.cor`'s own manifest,
`bit_commit` is `<commit8>` and `app_commit` the submodule commit.

## By hand

1. Fetch the SDK: `software/trx64u/fetch_sdk.sh --download`.
2. Fetch the three files of the package version, each with
   `curl -O http://gitea.example/api/packages/TREX/generic/trx64-ultimate/<branch>-<commit8>/<file>`:
   `manifest.json`, `c64_basic.bit` and `ultimate.app`.
3. Make the pin: copy `trx64g.pin` and append the values below, with the hashes taken from the manifest. Give `SEMVER`
   in the dev form: a plain `x.y.z` makes `version.sh` add the commit of this checkout, not the package's.

   ```
   VERSION=<commit8>
   SEMVER=<x.y.z>-dev+<commit8>
   PKG_VERSION=<branch>-<commit8>
   BIT_SHA256=<sha256 of c64_basic.bit>
   APP_SHA256=<sha256 of ultimate.app>
   ```

4. Pack: `software/trx64g/pack_import_cor.sh --pin <pin> --out <dir> --bit c64_basic.bit --app-file ultimate.app
   --manifest manifest.json --app-commit <8 hex>`. The result is `<dir>/trx64g.cor`, checked.

## What it holds

`trx64g.cor` holds four records:

- record 1, the manifest;
- record 3, the app: `ultimate.app` without its 12-byte header, byte for byte;
- record 4, the icon;
- record 5, `c64_basic.bit` as it is.

There is no record 6, 7 or 8. The pin has no `DESC`, `OPS` or `FILES`, and no `cores/trx64g/` descriptor exists.

So of the Commodore ROMs, the bundle holds what the application holds and nothing more. The application's build links
these files (`SRCS_BIN` of `software/ultimate-app/target/u64ii/riscv/ultimate/Makefile`), measured against the ROMs
901226-01 and 901225-01:

| File | Size | What it is |
|---|---|---|
| `roms/basic.bin` | 8 KiB | the BASIC ROM 901226-01, byte for byte |
| `roms/chars.bin` | 2 KiB | 225 of its 256 glyphs are glyphs of the character ROM; the menu's font (`c64.cc` `char_set`) |
| `software/u64/default_chars.bin` | 4 KiB | 252 of its 512 glyphs are glyphs of the character ROM; the character set the app writes when no ROM file is set (`c64.cc`) |
| `software/ModPlayer_16k/module.bin` | 16 KiB | the "MOD Player Cartridge", a 16K cartridge image the app starts for a `.mod` file (`filetype_reu.cc`) |

The KERNAL in the app is Gideon's own start code (`software/u64/default_kernal.tas`), not Commodore's. The bitstream
holds no Commodore ROM (`PROVENANCE.md`).
