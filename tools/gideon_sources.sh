#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# vendor/gideon is our own copy of the parts of Gideon Zweijtzer's 1541ultimate that the c64_basic bitstream and its benches use
# (U20 D3, D5); it is in git, there is no submodule behind it.
#
#   tools/gideon_sources.sh                       check that the copy is there (what the builds and benches call; changes nothing)
#   tools/gideon_sources.sh <1541ultimate dir>    renew the copy from a checkout of Gideon's upstream (github.com/GideonZ/1541ultimate)
#                                                 at the commit to take: tools/vendor_gideon.sh (the files, the patches of
#                                                 tools/patches/gideon, VENDOR.md)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
if [ $# -ge 1 ]; then
  "$here/tools/vendor_gideon.sh" "$1" >/dev/null
  echo "vendor/gideon renewed from $1 ($(git -C "$1" rev-parse --short=8 HEAD))"
  exit 0
fi
[ -d "$here/vendor/gideon/fpga" ] || { echo "vendor/gideon is missing from this checkout (it is in git: tools/gideon_sources.sh <1541ultimate dir> makes it)" >&2; exit 1; }
