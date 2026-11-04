#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run 64tass for the Mac: built once from the 1541ultimate checkout's
# tools/64tass source into .cache/64tass (that checkout's own binary is the
# Linux one its build container makes). Same assembler as its e2e tests use.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${TRX_64TASS_SRC:-$HOME/Development/1541ultimate/tools/64tass}"
BIN="$ROOT/.cache/64tass/64tass"
if [[ ! -x "$BIN" ]]; then
    rm -rf "$ROOT/.cache/64tass" && mkdir -p "$ROOT/.cache"
    cp -R "$SRC" "$ROOT/.cache/64tass"
    make -s -C "$ROOT/.cache/64tass" clean >/dev/null 2>&1 || true
    make -s -C "$ROOT/.cache/64tass" >/dev/null
fi
exec "$BIN" "$@"
