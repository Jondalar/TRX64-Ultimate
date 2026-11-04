#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `app`: the Ultimate app of the submodule commit, ultimate.app and
# ultimate.elf, for both boards (one RISC-V image; the board is told apart at
# run time, see docs/specs/T53-ue2-build.md). Runs in u64-build-env:ci.
#
# The result is cached by the submodule commit: a run that does not move the
# pointer takes /cache/app/<sha>/ and builds nothing. The build is what
# `make u64ii` does up to ultimate.app, without the ESP32 image (ESP-IDF) and
# without the update package, which T49 makes from our own bitstream.
#
#   ci/app.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
release_reuse_skip app

app="$ROOT/software/ultimate-app"
sha="$(git -C "$app" rev-parse HEAD)"
cache="${CI_APP_CACHE:-/cache/app}/$sha"
out="$(stage_dir app)"

if [ -f "$cache/DONE" ]; then
  echo "=== app $sha: from cache $cache"
  cp "$cache"/* "$out/"
else
  heavy_lock
  echo "=== app $sha: building"
  # ESP-IDF's environment brings the `python` the app's makefiles call; it goes
  # in before the ccache wrappers so it cannot put its own paths in front of them.
  if [ -f /opt/esp/esp-idf/export.sh ]; then
    set +u; . /opt/esp/esp-idf/export.sh >/dev/null; set -u
  fi
  # The makefiles call the compilers by bare name, so a directory of wrappers in
  # front of PATH routes them through ccache (same as the app repo's CI).
  if command -v ccache >/dev/null; then
    export CCACHE_DIR="${CCACHE_DIR:-/ccache}" CCACHE_MAXSIZE=5G
    mkdir -p "$CCACHE_DIR" /tmp/ccache-shim
    for t in riscv32-unknown-elf-gcc riscv32-unknown-elf-g++ riscv32-unknown-elf-c++; do
      real="$(command -v "$t" || true)"
      [ -n "$real" ] || continue
      printf '#!/bin/sh\nexec ccache %s "$@"\n' "$real" > "/tmp/ccache-shim/$t"
      chmod +x "/tmp/ccache-shim/$t"
    done
    export PATH="/tmp/ccache-shim:$PATH"
    ccache --zero-stats >/dev/null 2>&1 || true
  fi
  cd "$app"
  make app_space_test openapi_check
  make -C tools
  make -C target/libs/riscv/lwip
  make -C target/libs/riscv/lvgl
  make -C target/u64ii/riscv/ultimate
  python3 tools/app_space.py measure u64e2_50t 2>&1 | tee "$out/space_50t.txt"
  python3 tools/app_space.py measure u64e2_100t 2>&1 | tee "$out/space_100t.txt"
  # U5 §7: both budgets are checked here, not read off a table by eye. app_space.py
  # measure only prints "OVER LIMIT" (its own gate, `report`, wants every variant's
  # image and this job builds one), and the RAM left between the image and the top of
  # the linker's region is what the boot/ISR stack lives in: the makefile's own note
  # calls 127 KB the floor.
  ! grep -q "OVER LIMIT" "$out/space_50t.txt" "$out/space_100t.txt" || {
    echo "the application is over its APPL slot" >&2; exit 1; }
  python3 - "target/u64ii/riscv/ultimate/linker.x" "target/u64ii/riscv/ultimate/result/ultimate.elf" <<'RAMGATE'
import re, subprocess, sys
script, elf = sys.argv[1], sys.argv[2]
text = open(script, encoding="utf-8").read()
m = re.search(r"memory\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+)\s*,\s*LENGTH\s*=\s*(0x[0-9A-Fa-f]+)", text)
if not m:
    sys.exit("linker.x: no memory region found")
top = int(m.group(1), 16) + int(m.group(2), 16)
nm = subprocess.run(["riscv32-unknown-elf-nm", elf], capture_output=True, text=True, check=True).stdout
end = [int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == "_end"]
if not end:
    sys.exit("no _end in the image")
free = top - end[0]
print("RAM: image ends at 0x%08X, region top 0x%08X, %d bytes (%.1f KiB) left for stack and heap"
      % (end[0], top, free, free / 1024.0))
if free < 128 * 1024:
    sys.exit("less than 128 KiB left above the image: the boot/ISR stack needs it")
RAMGATE
  cp target/u64ii/riscv/ultimate/result/ultimate.app target/u64ii/riscv/ultimate/result/ultimate.elf "$out/"
  command -v ccache >/dev/null && ccache --show-stats | head -8 || true
  # only a complete build is cached
  mkdir -p "$cache.tmp.$$"
  cp "$out"/* "$cache.tmp.$$/"
  : > "$cache.tmp.$$/DONE"
  rm -rf "$cache"
  mv "$cache.tmp.$$" "$cache"
fi
printf '%s\n' "$sha" > "$out/app_commit"
ls -l "$out"
sha256sum "$out/ultimate.app"
: > "$out/DONE"
echo "=== app done in $(lap)"
