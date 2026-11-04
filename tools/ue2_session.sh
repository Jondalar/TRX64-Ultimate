#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# UE2 session helper: tools/ue2_session.py with the JTAG venv (pyftdi), niced. See the script's docstring and
# docs/specs/T53-ue2-session-till.md.   tools/ue2_session.sh doctor|fetch|detect|backup|sram|flash|restore|reset|checks
#   --dry-run as first argument: fake board, no hardware, no venv needed.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
VENV="${U64II_JTAG_VENV:-${XDG_CACHE_HOME:-$HOME/.cache}/1541ultimate-tools/jtag-venv}"
PY="$VENV/bin/python"
[[ "${1:-}" == "--dry-run" || ! -x "$PY" ]] && PY=python3
exec nice -n 15 "$PY" -u "$HERE/ue2_session.py" "$@"
