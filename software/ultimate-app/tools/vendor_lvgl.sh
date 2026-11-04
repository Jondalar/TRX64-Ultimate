#!/usr/bin/env bash
# Puts a pinned LVGL release into software/lvgl (TRX64-Ultimate T24,
# docs/specs/T24-lvgl-ui.md §3).
#
#   tools/vendor_lvgl.sh [tag]        default: the tag in software/lvgl/VERSION
#
# Copied: src/, include/, the top headers, the licence files and the image
# converter scripts/LVGLImage.py. Not copied: demos, examples, tests, docs.
# software/lvgl/VERSION records the tag and the commit. Local changes to the
# copy are not allowed: a fix goes upstream or into lv_conf.h, so the next run
# of this script stays a plain upgrade.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/software/lvgl"
TAG="${1:-}"
if [ -z "$TAG" ]; then
    TAG="$(sed -n 's/^tag //p' "$DEST/VERSION")"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
git clone -q --depth 1 --branch "$TAG" https://github.com/lvgl/lvgl.git "$TMP/lvgl"
COMMIT="$(git -C "$TMP/lvgl" rev-parse HEAD)"

rm -rf "$DEST"
mkdir -p "$DEST/scripts"
cp -R "$TMP/lvgl/src" "$TMP/lvgl/include" "$DEST/"
cp "$TMP/lvgl/lvgl.h" "$TMP/lvgl/lvgl_private.h" "$TMP/lvgl/lv_version.h" \
   "$TMP/lvgl/LICENCE.txt" "$TMP/lvgl/COPYRIGHTS.md" "$TMP/lvgl/lv_conf_template.h" "$DEST/"
cp "$TMP/lvgl/scripts/LVGLImage.py" "$DEST/scripts/"
# C++ sources (ThorVG) are never built here (target/libs/riscv/lvgl/makefile
# compiles the C files only); they stay for a later LV_USE_THORVG.
printf 'tag %s\ncommit %s\nfrom https://github.com/lvgl/lvgl.git\n' "$TAG" "$COMMIT" > "$DEST/VERSION"
echo "LVGL $TAG ($COMMIT) in $DEST"
