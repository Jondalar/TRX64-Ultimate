#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch a bitstream the NAS runner built and load it into the C64 Ultimate's
# FPGA SRAM through the USB-Blaster (nothing is written to flash).
#
#   tools/load.sh [design] [version]      # defaults: hdmi_test, main-latest
#   tools/load.sh path/to/file.bit        # load a local file
#
# Back to the flashed image: unplug the Blaster's USB, power off, power on.
set -euo pipefail
# Gitea's server and token: GITEA_URL and GITEA_TOKEN, else the url: and token: of the tea login (U20 A8: the tree names no host)
TEA_CONFIG="${TEA_CONFIG:-$HOME/Library/Application Support/tea/config.yml}"
GITEA="${GITEA_URL:-$(awk '/^ *url:/{print $2; exit}' "$TEA_CONFIG" 2>/dev/null || true)}"
TOKEN="${GITEA_TOKEN:-$(awk '/token:/{print $2; exit}' "$TEA_CONFIG" 2>/dev/null || true)}"

if [[ "${1:-}" == *.bit ]]; then
    bit="$1"
else
    design="${1:-hdmi_test}"; version="${2:-main-latest}"
    [ -n "$GITEA" ] || { echo "no Gitea server: set GITEA_URL (or log in with tea)" >&2; exit 1; }
    bit="$(mktemp -d)/$design.bit"
    curl -sf -H "Authorization: token $TOKEN" -o "$bit" \
        "$GITEA/api/packages/TREX/generic/trx64-ultimate/$version/$design.bit" \
        || { echo "no $design.bit in trx64-ultimate $version" >&2; exit 1; }
    echo "fetched $design $version ($(shasum -a 256 "$bit" | cut -c1-16))"
fi
for attempt in 1 2 3; do
    if openFPGALoader -c usb-blaster -m "$bit" 2>&1 | grep -q "done 1"; then
        echo "loaded into SRAM: $bit"
        exit 0
    fi
    sleep 1
done
echo "load failed (is the C64U on and the Blaster on the dock?)" >&2
exit 1
