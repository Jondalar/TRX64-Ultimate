#!/bin/bash
# Build U64 .u2a apps inside the container.
#
# Usage:
#   ./build/build_app.sh hello              # one sample (software/appsdk/samples/)
#   ./build/build_app.sh trex_crt           # one app bundle (apps/)
#   ./build/build_app.sh all                # every sample
#   ./build/build_app.sh list               # list available apps
#
# Produces .u2a files in format version 2 with a Nios II (U64) and a RISC-V
# (U64-II, C64U) variant. Fast: ~5-10s per app. Does NOT rebuild firmware.
# Requires Quartus (nios2-elf toolchain) — same mount as build.sh; the RISC-V
# toolchain is part of the image.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="u64-build-env"
SAMPLES_DIR="$PROJECT_DIR/software/appsdk/samples"
APPS_DIR="$PROJECT_DIR/apps"

APP="${1:-}"

if [ -z "$APP" ] || [ "$APP" = "help" ] || [ "$APP" = "-h" ] || [ "$APP" = "--help" ]; then
    echo "Usage: $0 <app|all|list>"
    echo "Available apps:"
    for d in "$SAMPLES_DIR"/*/ "$APPS_DIR"/*/; do
        [ -d "$d" ] && [ -f "$d/Makefile" ] && echo "  $(basename "$d")"
    done
    exit 0
fi

if [ "$APP" = "list" ]; then
    echo "Available apps under $SAMPLES_DIR and $APPS_DIR:"
    for d in "$SAMPLES_DIR"/*/ "$APPS_DIR"/*/; do
        [ -d "$d" ] && [ -f "$d/Makefile" ] && echo "  $(basename "$d")"
    done
    exit 0
fi

# Locate Quartus (needed for nios2-elf toolchain)
QUARTUS_PATH="${QUARTUS_PATH:-}"
if [ -z "$QUARTUS_PATH" ]; then
    for candidate in \
        "$HOME/quartus-18.1" \
        "$HOME/altera_lite/18.1" \
        "$HOME/intelFPGA_lite/18.1" \
        "/opt/altera_lite/18.1"; do
        if [ -d "$candidate/nios2eds" ]; then
            QUARTUS_PATH="$candidate"
            break
        fi
    done
fi

if [ -z "$QUARTUS_PATH" ] || [ ! -d "$QUARTUS_PATH/nios2eds" ]; then
    echo "ERROR: Quartus 18.1 Nios II EDS not found."
    echo "Set QUARTUS_PATH=/path/to/quartus-18.1"
    exit 1
fi

# Build one app by name.
build_one() {
    local name="$1"
    local dir="$SAMPLES_DIR/$name"
    [ -f "$dir/Makefile" ] || dir="$APPS_DIR/$name"
    if [ ! -f "$dir/Makefile" ]; then
        echo "ERROR: no Makefile for $name in $SAMPLES_DIR or $APPS_DIR"
        return 1
    fi
    echo ">>> Building $name"
    # the git sha of this tree, for apps that put a build id in their answers (the container sees no worktree's git)
    local build_id
    build_id="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$PROJECT_DIR" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        build_id="$build_id-dirty"
    fi
    container run --rm --rosetta --arch amd64 \
        -e "TRXMON_BUILD=$build_id" \
        -v "$PROJECT_DIR:/project" \
        -v "$QUARTUS_PATH:/opt/altera_lite/18.1:ro" \
        "$IMAGE_NAME" \
        bash -c "cd /project/${dir#"$PROJECT_DIR"/} && make clean >/dev/null 2>&1 && make 2>&1" \
        | grep -vE '^\*|ESP-IDF|Checking "|Activating|Python|Done!|idf\.py|Go to' \
        | tail -8
    echo ">>> $name -> $dir/$name.u2a"
    ls -la "$dir/$name.u2a" 2>/dev/null || { echo "  FAILED"; return 1; }
}

if [ "$APP" = "all" ]; then
    FAIL=0
    for d in "$SAMPLES_DIR"/*/; do
        name="$(basename "$d")"
        [ -f "$d/Makefile" ] || continue
        build_one "$name" || FAIL=$((FAIL+1))
    done
    exit $FAIL
else
    build_one "$APP"
fi
