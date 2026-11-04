#!/bin/bash
# Build script for 1541 Ultimate firmware using Apple Container
#
# Prerequisites:
#   1. Apple Container CLI: brew install container && brew services start container
#   2. For U64 (Nios II): Quartus 18.1 Lite Edition with Nios II EDS
#   3. For U64-II (RISC-V): No extra tools needed (toolchain is in container)
#
# Usage:
#   ./build/build.sh                    # Build U64 firmware (update.u64)
#   ./build/build.sh u64ii             # Build U64 Elite II firmware
#   ./build/build.sh clean             # Clean all sw build artifacts
#   QUARTUS_PATH=/path/to/18.1 ./build/build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONTAINER_NAME="u64-builder"
IMAGE_NAME="u64-build-env"
TARGET="${1:-u64}"

# Handle clean target
if [ "$TARGET" = "clean" ] || [ "$2" = "clean" ]; then
    echo ">>> Cleaning software build artifacts..."
    make -C "$PROJECT_DIR" sw_clean 2>/dev/null || true
    echo "Done."
    [ "$TARGET" = "clean" ] && exit 0
fi

# Determine if we need Quartus (Nios II targets only)
NEEDS_QUARTUS=true
case "$TARGET" in
    u64ii|u2|u2plus|u2plus_L)
        NEEDS_QUARTUS=false
        ;;
esac

# Locate Quartus installation (only for Nios II targets)
QUARTUS_PATH="${QUARTUS_PATH:-}"
QUARTUS_MOUNT=""
if [ "$NEEDS_QUARTUS" = true ]; then
    if [ -z "$QUARTUS_PATH" ]; then
        for candidate in \
            "$PROJECT_DIR/quartus-18.1" \
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
        echo "Run: ./build/setup-quartus.sh"
        echo "Or set QUARTUS_PATH=/path/to/quartus-18.1"
        exit 1
    fi
    QUARTUS_MOUNT="-v $QUARTUS_PATH:/opt/altera_lite/18.1:ro"
fi

echo "=== 1541 Ultimate Build ==="
echo "Target:  $TARGET"
if [ "$NEEDS_QUARTUS" = true ]; then
    echo "Quartus: $QUARTUS_PATH"
else
    echo "CPU:     RISC-V (toolchain in container)"
fi
echo "Project: $PROJECT_DIR"
echo ""

# Build the container image if it doesn't exist
if ! container image ls 2>/dev/null | grep -q "$IMAGE_NAME"; then
    echo ">>> Building container image (this takes a while the first time)..."
    container build --arch amd64 --tag "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# Stop and remove existing container if running
container rm -f "$CONTAINER_NAME" 2>/dev/null || true

# The host tools under tools/ are shared with anything that builds on the Mac
# itself: tests/e2e/lib/assembler.py builds tools/64tass there to assemble a
# fixture. The container cannot execute a Mach-O binary and make reports only
# "Error 127", so a tool that is not a Linux binary means all of them are
# rebuilt. Same check as the emulator's scripts/build-firmware.sh, other way round.
for tool in bin2hex hex2bin make_array make_mem makeappl promgen checksum \
            dump_vcd dump_bus_trace dump_rtos_trace swap svf_dump 64tass/64tass; do
    [ -f "$PROJECT_DIR/tools/$tool" ] || continue
    case "$(file -b "$PROJECT_DIR/tools/$tool")" in
        ELF*) ;;
        *)  echo ">>> tools/$tool was built for this Mac; rebuilding the host tools for the container"
            make -C "$PROJECT_DIR/tools" clean >/dev/null 2>&1 || true
            break ;;
    esac
done

# Always clean software build artifacts
echo ">>> Cleaning software build artifacts..."
make -C "$PROJECT_DIR" sw_clean 2>/dev/null || true

# Run the build
echo ">>> Starting build for target: $TARGET"
container run --rm \
    --rosetta \
    --arch amd64 \
    --name "$CONTAINER_NAME" \
    --cpus 4 \
    --memory 4g \
    -v "$PROJECT_DIR:/project" \
    $QUARTUS_MOUNT \
    "$IMAGE_NAME" \
    make "$TARGET"

echo ""
echo "=== Build complete ==="
ls -la "$PROJECT_DIR"/update.* 2>/dev/null || echo "No update files found (check build output above)"
