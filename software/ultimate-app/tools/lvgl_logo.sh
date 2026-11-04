#!/usr/bin/env bash
# Converts the splash logo into an LVGL image in C (TRX64-Ultimate
# docs/specs/T24-lvgl-ui.md §8): software/lvgl_port/assets/trx_logo.c, which the
# u64ii application build picks up by itself (target/u64ii/riscv/ultimate/Makefile,
# TRX_HAVE_LOGO) and the splash shows above its text.
#
#   tools/lvgl_logo.sh <logo.png|logo.svg> [width]
#
# width: pixels on the layer (960 x 540, shown at 2x: 480 is half the screen
# width). An SVG is rendered at that width (rsvg-convert, brew install librsvg);
# a PNG is scaled to it (sips) when given, else used as it is.
# Colour format: RGB565 with the transparent parts blended onto the splash's
# black (2 bytes per pixel, in the application binary: 480 x 200 = 192,000 bytes).
#   LOGO_CF=RGB565A8 keeps the alpha (3 bytes per pixel), for a logo that has to
#   sit on something other than black.
# Needs python3 with pypng and lz4 (pip3 install pypng lz4). To remove the logo, delete
# software/lvgl_port/assets/trx_logo.c.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IN="${1:?usage: tools/lvgl_logo.sh <logo.png|logo.svg> [width]}"
W="${2:-}"
CF="${LOGO_CF:-RGB565}"
OUT="$ROOT/software/lvgl_port/assets"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

case "$IN" in
    *.svg|*.SVG)
        rsvg-convert ${W:+-w "$W"} -o "$TMP/trx_logo.png" "$IN" ;;
    *.png|*.PNG)
        cp "$IN" "$TMP/trx_logo.png"
        if [ -n "$W" ]; then
            sips --resampleWidth "$W" "$TMP/trx_logo.png" >/dev/null
        fi ;;
    *)
        echo "PNG or SVG only" >&2; exit 1 ;;
esac

python3 - "$TMP/trx_logo.png" <<'EOF'
import sys, png
w, h, _, _ = png.Reader(filename=sys.argv[1]).read()
if w > 960 or h > 540:
    sys.exit("logo is %d x %d; the layer is 960 x 540" % (w, h))
print("logo %d x %d" % (w, h))
EOF

mkdir -p "$OUT"
python3 "$ROOT/software/lvgl/scripts/LVGLImage.py" --ofmt C --cf "$CF" --background 0x000000 \
    --name trx_logo -o "$OUT" "$TMP/trx_logo.png"
echo "wrote $OUT/trx_logo.c; rebuild the application"
