#!/usr/bin/env bash
# Pack the .cor of one LARGE core (a full 50T bitstream started by IPROG; T115 section 7 "The large .cor").
#
#   software/trx64g/pack_large_cor.sh --id <core id> --name "<menu name>" --version <v> --bit <id>_mono.bit --app-bin <coreapp.bin> \
#       [--icon cores/<id>/logo/icon.rgb565] [--short <tile text>] [--bit-commit <sha>] [--app-commit <sha>] [--heap <bytes>] [--origin trx|foreign|backup] [--flash-id <chip>] [--out <dir>] [--desc menu.toml [--desc desc.toml] --ops ops.txt]
#
# The same two inputs as tools/pack_cfw_mc.sh (the flat bitstream of ci/bitstream_mono.sh and the generic core application coreapp.bin,
# the raw image linked to 0x30000), packed with tools/cor_pack.py pack-large instead of into an updater: no container image, no
# make_update.sh, python3 only. Output: <out>/<id>.cor, read back with `cor_pack.py check` (the host's checks: IDCODE, hashes, map).
# The icon defaults to cores/<id>/logo/icon.rgb565 when that is 2048 bytes. The pipeline calls this after the bit-mono stage
# (cores/<id>/mono/build.sh --pack-cor forwards here).
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: copied from TRX64U-MultiCore, branch t239.3, commit cfa7872a, tools/pack_large_cor.sh (GPL-3.0-or-later, MultiCore's LICENSING.md), for
# MultiCore T239.3 D46. Changed here, only what this tree forces: the path of the usage line, and cor_pack.py is the pinned SDK's (SDK below, from
# software/trx64u/sdk.pin, fetch_sdk.sh), the one trx64u.cor is packed with. ROOT stays this folder's parent, software/: there is no cores/trx64g/ under
# it, so the core.toml parts find nothing for trx64g. Everything else is MultiCore's text; its comments name MultiCore's tree. pack_import_cor.sh calls it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SDK="$ROOT/trx64u/.sdk/trx64-sdk-$(sed -n 's/^sdk_version=//p' "$ROOT/trx64u/sdk.pin")/tools"
ID="" NAME="" VER="" BIT="" APPBIN="" ICON="" BITC="" APPC="" OUT="$ROOT/dist" DESCS=() OPS="" ORIGIN="" FLASHID="" SHORT="" FILESDIR="" HEAP=""
usage() { sed -n '2,12p' "$0"; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
        --id) ID="$2"; shift 2;;
        --name) NAME="$2"; shift 2;;
        --version) VER="$2"; shift 2;;
        --bit) BIT="$2"; shift 2;;
        --app-bin) APPBIN="$2"; shift 2;;
        --icon) ICON="$2"; shift 2;;
        --short) SHORT="$2"; shift 2;;
        --bit-commit) BITC="$2"; shift 2;;
        --app-commit) APPC="$2"; shift 2;;
        --origin) ORIGIN="$2"; shift 2;;
        --flash-id) FLASHID="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --heap) HEAP="$2"; shift 2;;                # T239.3 D15: the core's heap in bytes; default: the core's descriptor `heap`, else the image's own
        --desc) DESCS+=("$2"); shift 2;;      # repeatable: a core's menu.toml and desc.toml are one record 6
        --ops) OPS="$2"; shift 2;;
        --files) FILESDIR="$2"; shift 2;;          # record 8 from a folder of the caller (an imported core has no cores/<id>/core.toml: trx64u, T228 R21 item 7)
        -h|--help) usage;;
        *) echo "unknown option $1" >&2; usage;;
    esac
done
[ -n "$ID" ] && [ -n "$NAME" ] && [ -n "$VER" ] && [ -n "$BIT" ] && [ -n "$APPBIN" ] || usage
[ -s "$BIT" ] && [ -s "$APPBIN" ] || { echo "input file missing or empty" >&2; exit 1; }
[ -n "$ICON" ] || { [ "$(wc -c < "$ROOT/cores/$ID/logo/icon.rgb565" 2>/dev/null | tr -d ' ')" = 2048 ] && ICON="$ROOT/cores/$ID/logo/icon.rgb565" || true; }
mkdir -p "$OUT"
args=(--id "$ID" --name "$NAME" --version "$VER" --bit "$BIT" --app "$APPBIN")
[ -z "$ICON" ] || args+=(--icon "$ICON")
[ -z "$SHORT" ] || args+=(--short "$SHORT")
[ -z "$BITC" ] || args+=(--bit-commit "$BITC")
[ -z "$APPC" ] || args+=(--app-commit "$APPC")
# T239.3 D17, D25: the core's data description (cores/<id>/app/desc.toml) is record 6 only when cores/<id>/app/sdk.toml says desc_record = true; the caller's own --desc files (menu.toml) join it as one record, a table or key in two files is refused by cor_pack
DESC_REC="$(python3 "$HERE/core_toml.py" sdk "$ID" desc_record)"      # assigned first: a failing core_toml.py stops the packer
if [ "$DESC_REC" = true ] && [ -s "$ROOT/cores/$ID/app/desc.toml" ]; then
    DESCS+=("$ROOT/cores/$ID/app/desc.toml")
fi
for f in ${DESCS[@]+"${DESCS[@]}"}; do args+=(--desc "$f"); done
[ -z "$OPS" ] || args+=(--ops "$OPS")
[ -z "$ORIGIN" ] || args+=(--origin "$ORIGIN")
[ -z "$FLASHID" ] || args+=(--flash-id "$FLASHID")
[ -z "$FILESDIR" ] || args+=(--files "$FILESDIR")
# the files the core installs on the SD (record 8): the folders its descriptor names (core.toml install_dir / install_old, relative to cores/<id>/)
for d in "$ROOT/cores/$ID/mono/core.toml" "$ROOT/cores/$ID/core.toml"; do
    [ -f "$d" ] || continue
    idir="$(TRX_CORE_DESC="$d" python3 "$HERE/core_toml.py" get "$ID" install_dir)"
    iold="$(TRX_CORE_DESC="$d" python3 "$HERE/core_toml.py" get "$ID" install_old)"
    [ -n "$HEAP" ] || HEAP="$(TRX_CORE_DESC="$d" python3 "$HERE/core_toml.py" get "$ID" heap)"
    [ -z "$idir" ] || args+=(--files "$ROOT/cores/$ID/$idir")
    [ -z "$iold" ] || args+=(--files-old "$ROOT/cores/$ID/$iold")
    break
done
[ -z "$HEAP" ] || args+=(--heap "$HEAP")
python3 "$SDK/cor_pack.py" pack-large -o "$OUT/$ID.cor" "${args[@]}"
python3 "$SDK/cor_pack.py" check "$OUT/$ID.cor" > /dev/null
ls -l "$OUT/$ID.cor"
shasum -a 256 "$OUT/$ID.cor" 2>/dev/null || sha256sum "$OUT/$ID.cor"
