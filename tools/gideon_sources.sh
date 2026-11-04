#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Make cores/gideon (generated, not in git) from the submodule
# software/ultimate-app: Gideon's VHDL as tools/vendor_gideon.sh takes it, with
# our patches from tools/patches/gideon. Does nothing while the submodule
# commit, the patches and the vendor script are those it was made from.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
app=$here/software/ultimate-app
[ -e "$app/.git" ] || git -C "$here" submodule update --init software/ultimate-app
want="$(git -C "$app" rev-parse HEAD) $(cat "$here"/tools/patches/gideon/*.patch "$here/tools/vendor_gideon.sh" | git hash-object --stdin)"
have=$(cat "$here/cores/gideon/.stamp" 2>/dev/null || true)
[ "$want" = "$have" ] && exit 0
"$here/tools/vendor_gideon.sh" "$app" >/dev/null
echo "$want" > "$here/cores/gideon/.stamp"
echo "cores/gideon made from software/ultimate-app $(git -C "$app" rev-parse --short=8 HEAD)"
