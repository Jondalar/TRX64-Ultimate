#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: the ROM part of TRX64U-MultiCore ci/emu_kit.sh (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B. Changed here: only
# the ROM folder (software/trx64u/tools/emu_ops.py makes its own stick, SD card and flash).
# The emulator's ROM folder for the emulator step of the core lane, made in the run from a public source (the repository holds no ROM, U20 D14):
# roms/ of GideonZ/1541ultimate at the commit pinned below, which git checks byte for byte through the commit id, and the folder is checked against
# the git tree id of roms/ at that commit. emu_ops.py takes the C64 and 1541 ROMs of its core folder from there (basic.901226-01.bin,
# kernal.901227-03.bin, characters.901225-01.bin, 1541-ii.bin), and ue2emu the rest (its overlay font chars.bin, the drive ROMs).
#   ci/emu_roms.sh DIR [--from DIR2]
#     DIR          made (replaced) as the ROM folder
#     --from DIR2  a restored copy (actions/cache, keyed by the commit below) instead of the fetch; refused unless it has the pinned tree id
#   ci/emu_roms.sh --commit      prints the pinned commit (the cache key of the workflow)
set -euo pipefail
GIDEON_REPO="https://github.com/GideonZ/1541ultimate.git"
GIDEON_COMMIT="700c7f2fab0b7d52da418d21f0c2e65e99725805"
GIDEON_ROMS_TREE="e308506696d7507486b7b0981eeacc4ae5bfe373"     # git rev-parse 700c7f2f:roms (fetched 2026-11-25)
[ "${1:-}" != --commit ] || { echo "$GIDEON_COMMIT"; exit 0; }
out="${1:?usage: ci/emu_roms.sh DIR [--from DIR2] | --commit}"; shift
from=""
while [ $# -gt 0 ]; do
  case "$1" in
    --from) from="${2:?--from DIR2}"; shift 2 ;;
    *) echo "emu_roms: unknown argument $1" >&2; exit 2 ;;
  esac
done
# the git tree id of a folder, made in a scratch repository (add -A -f takes ignored files too; attributes and excludes point at nothing). Refused first,
# because git add skips them without a word: a name .git (any case), an empty folder, an entry that is no file, folder or link.
tree_of() {
  if [ -n "$(find "$1" \( -iname '.git' -o \( -type d -empty \) -o ! \( -type f -o -type d -o -type l \) \) -print -quit)" ]; then
    echo refused; return 0
  fi
  local g; g="$(mktemp -d)"; git -C "$g" init -q
  git -C "$g" -c core.autocrlf=false -c core.safecrlf=false -c core.attributesFile=/dev/null -c core.excludesFile=/dev/null --work-tree="$1" add -A -f
  git -C "$g" write-tree; rm -rf "${g:?}"
}
rm -rf "$out"; mkdir -p "$(dirname "$out")"
if [ -n "$from" ]; then
  [ -d "$from" ] || { echo "emu_roms: --from $from is no directory" >&2; exit 1; }
  cp -R "$from" "$out"
else
  src="$(mktemp -d)"; trap 'rm -rf "${src:?}"' EXIT
  git -C "$src" init -q
  git -C "$src" fetch -q --depth 1 --filter=blob:none "$GIDEON_REPO" "$GIDEON_COMMIT"
  [ "$(git -C "$src" rev-parse 'FETCH_HEAD^{commit}')" = "$GIDEON_COMMIT" ] || { echo "emu_roms: the fetched commit is not $GIDEON_COMMIT" >&2; exit 1; }
  git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD -- roms
  cp -R "$src/roms" "$out"
fi
[ "$(tree_of "$out")" = "$GIDEON_ROMS_TREE" ] || { rm -rf "$out"; echo "emu_roms: the folder is not roms/ of $GIDEON_COMMIT (tree id $GIDEON_ROMS_TREE)" >&2; exit 1; }
echo "emu_roms: $out is roms/ of GideonZ/1541ultimate $GIDEON_COMMIT ($(find "$out" -type f | wc -l | tr -d ' ') files)"
