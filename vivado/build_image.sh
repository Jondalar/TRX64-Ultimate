#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the vivado-env image on the NAS from an installed Vivado tree.
#
#   vivado/build_image.sh [install dir on the NAS]
#
# The install dir is a finished Vivado 2024.1 install (Artix-7 only, see
# install_config_2024.1.txt); it is only read. The image stays in the NAS
# docker as vivado-env:2024.1.
# The NAS from the environment as in tools/vivado.sh: TRX_NAS, TRX_NAS_DATA, TRX_NAS_DOCKER (U20 A8; the defaults are placeholders).
set -euo pipefail
NAS="${TRX_NAS:-admin@192.0.2.2}"
DOCKER="${TRX_NAS_DOCKER:-docker}"
SRC="${1:-${TRX_NAS_DATA:-/srv}/xilinx/install}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP="${TRX_NAS_DATA:-/srv}/vivado-image-build"

ssh "$NAS" "rm -rf '$TMP' && mkdir -p '$TMP'"
scp -q "$HERE/Dockerfile" "$NAS:$TMP/Dockerfile"
scp -q "$HERE/dockerignore" "$NAS:$SRC/.dockerignore"
ssh "$NAS" "$DOCKER build -t vivado-env:2024.1 -f '$TMP/Dockerfile' '$SRC'; rc=\$?; rm -f '$SRC/.dockerignore'; rm -rf '$TMP'; exit \$rc"
ssh "$NAS" "$DOCKER images vivado-env"
