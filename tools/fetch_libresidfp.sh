#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch libresidfp (GPL-2.0-or-later) at a pinned commit for the T25.1 SID
# engine's table generator and golden-model harness (cores/trx_sid/model).
# Not vendored: the checkout goes to $1 (default: build/libresidfp next to the
# caller's sim directory). Prints the source directory.
set -e
dst=${1:-$(pwd)/build/libresidfp}
rev=dcf6b8ac96a7aa385c75e9c9167914100fc61f78
if [ ! -d "$dst/.git" ]; then
  git clone -q https://github.com/libsidplayfp/libresidfp.git "$dst"
fi
git -C "$dst" checkout -q $rev
s=$dst/src
sed -e 's/@RESIDFP_BRANCH_HINTS@/1/; s/@HAVE_BUILTIN_EXPECT@/1/; s/@PACKAGE_VERSION@/git/; s/@RESIDFP_INLINING@/1/; s/@RESIDFP_INLINE@/inline/' \
  "$s/siddefs-fp.h.in" > "$s/siddefs-fp.h"
echo "$s"
