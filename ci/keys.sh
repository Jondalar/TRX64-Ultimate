#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 A7 (MultiCore's ci/keys.sh form): the cache entry names of this tree's gateware, one per line. One entry today:
#   c64u-<K>    every designs/*/build.tcl for the XC7A50T (ci/bitstream.sh c64u), K = gateware_key of ci/lib.sh
# The name is the directory ci/bitstream.sh keeps in its bitstream cache and, with $BUILDS_TAG_PREFIX "bit-" in front (ci/pkg.env), the tag of the
# builder's release. Build options come from ci/vivado.env, the part from the c64u branch of ci/bitstream.sh: nothing is read from the caller's
# environment, so every caller gets the same names.
#   ci/keys.sh [--env FILE] [--data DIR]
#     --env FILE   replaces ci/vivado.env (tests)
#     --data DIR   the checkout whose files are hashed and read (default: the tree this script sits in). The scripts still run from this script's
#                  tree (ci/lib.sh: CI_CODE, ROOT), so DIR is only read. The builder copies the key code of its trusted ref (ci/keys.sh, ci/lib.sh)
#                  into a folder of its own and runs it with --data <a checkout of the commit>.
set -euo pipefail
CODE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
data="$CODE"; envfile=""
while [ $# -gt 0 ]; do
  case "$1" in
    --env) envfile="${2:?ci/keys.sh --env FILE}"; shift 2 ;;
    --data) data="${2:?ci/keys.sh --data DIR}"; shift 2 ;;
    *) echo "usage: ci/keys.sh [--env FILE] [--data DIR]" >&2; exit 2 ;;
  esac
done
CI_DATA_ROOT="$(cd "$data" && pwd)"
export CI_DATA_ROOT
# shellcheck source=lib.sh
source "$CODE/ci/lib.sh"      # CI_CODE = this tree, ROOT = the tree that is read
[ -n "$envfile" ] || envfile="$ROOT/ci/vivado.env"

# vivado_env_load <file>: export the KEY=VALUE lines, after dropping every option variable the caller had (a stray TRX_FEATURES would move the key).
# The file is the commit's own data and this code runs over it with the builder's rights, so a line is taken only when its name is one of the
# build option families (TRX_*, T79_*, T16_*: never PATH, LD_PRELOAD) and its value has no character a shell or a path could use
# ([A-Za-z0-9_.,:+-] only). Comments and empty lines are skipped; anything else ends the run with exit 1 and the line number.
vivado_env_load() {
  local k line n=0
  while IFS= read -r k; do case "$k" in TRX_*|T79_*|T16_*) unset "$k" ;; esac; done < <(compgen -e)
  [ -f "$1" ] || { echo "ci/keys.sh: no $1" >&2; exit 1; }
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    [[ "$line" =~ ^[[:space:]]*(#.*)?$ ]] && continue
    if [[ "$line" =~ ^((TRX|T79|T16)_[A-Z0-9_]+)=([A-Za-z0-9_.,:+-]*)$ ]]; then
      export "${BASH_REMATCH[1]}=${BASH_REMATCH[3]}"
    else
      echo "ci/keys.sh: $1 line $n is not a build option (NAME is TRX_*, T79_* or T16_*; value only A-Za-z0-9_.,:+-)" >&2
      exit 1
    fi
  done < "$1"
}
vivado_env_load "$envfile"

part="$(sed -n 's/^  c64u) part=\([^;]*\);.*/\1/p' "$ROOT/ci/bitstream.sh")"
[ -n "$part" ] || { echo "ci/keys.sh: no c64u part in ci/bitstream.sh" >&2; exit 1; }
key="$(gateware_key "$part")"
[[ "$key" =~ ^[0-9a-f]{16}$ ]] || { echo "ci/keys.sh: gateware_key gave '$key'" >&2; exit 1; }
echo "c64u-$key"
