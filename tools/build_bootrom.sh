#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the rvlite boot ROM (software/bootrom) into designs/c64_basic/bootrom_pkg.vhd,
# with riscv32-unknown-elf-gcc from the 1541ultimate build image (u64-build-env,
# Apple's container CLI; see 1541ultimate/build/build.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
container run --rm --rosetta --arch amd64 -v "$ROOT:/trx" -w /trx/software/bootrom \
    u64-build-env make
