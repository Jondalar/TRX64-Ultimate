#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run a Vivado batch build of one design on the NAS and fetch its bitstream.
#
#   tools/vivado.sh designs/hdmi_test [build.tcl]
#
# The design directory is copied to the NAS work area, Vivado 2024.1 runs in
# the vivado-env:2024.1 container there (tools baked into the image, see
# vivado/), and build/ comes back next to the sources.
# The NAS from the environment (U20 A8: the public tree names no host or NAS path): TRX_NAS (login@host), TRX_NAS_DATA (the
# data volume that holds trx64-build/), TRX_NAS_DOCKER (the docker binary there); the defaults are placeholders.
set -euo pipefail

NAS="${TRX_NAS:-admin@192.0.2.2}"
BASE="${TRX_NAS_DATA:-/srv}/trx64-build"
DOCKER="${TRX_NAS_DOCKER:-docker}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

design="${1:?design directory, e.g. designs/hdmi_test}"
script="${2:-build.tcl}"
name="$(basename "$design")"
work="$BASE/work/$name"

"$ROOT/tools/gideon_sources.sh"

ssh "$NAS" "mkdir -p '$work' && rm -rf '$work/src' && mkdir -p '$work/src'"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT" --exclude build -czf - "$design" constraints cores vendor tools/vivado_lib.tcl | ssh "$NAS" "tar -C '$work/src' -xzf -"
ssh "$NAS" "mkdir -p '$BASE/cache' && $DOCKER run --rm --cpus=3 -v '$work:/work' \
    -v '$BASE/cache:/cache' -e TRX_CACHE=/cache -e HOME=/tmp \
    -w '/work/src/$design' vivado-env:2024.1 \
    vivado -mode batch -nojournal -log /work/vivado.log -source $script" \
    | grep -E "^(ERROR|CRITICAL WARNING|INFO: \[Common 17-206\]|Bitstream|##|===)" || true
rm -rf "$ROOT/$design/build"
mkdir -p "$ROOT/$design/build"
scp -q "$NAS:$work/src/$design/build/*" "$ROOT/$design/build/" 2>/dev/null || true
scp -q "$NAS:$work/vivado.log" "$ROOT/$design/build/vivado.log"
ls -l "$ROOT/$design/build" | grep -E "\.bit|\.rpt" || { echo "no bitstream; see $design/build/vivado.log"; exit 1; }
