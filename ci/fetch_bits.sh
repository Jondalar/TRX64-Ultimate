#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 B (MultiCore T239.1 D19): restore the bitstream entries a run needs from the builder's releases in the builds repository (ci/fetch_bits.py has the checks).
#   ci/fetch_bits.sh [--names]       env: CI_BIT_CACHE, CI_HANDOFF, GH_TOKEN (optional)
set -euo pipefail
exec python3 -I "$(dirname "${BASH_SOURCE[0]}")/fetch_bits.py" "$@"
