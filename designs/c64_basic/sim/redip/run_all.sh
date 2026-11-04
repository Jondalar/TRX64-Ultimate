#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: every bench of this directory, built from the tree as it is. Needs verilator and
# git history down to the reference commit; about 40 minutes niced on one core.
set -e
here=$(cd "$(dirname "$0")" && pwd)
export PATH=${OSS:-$HOME/.cache/trx64-oss/oss-cad-suite}/bin:$PATH
sh "$here/build_ref.sh" > /dev/null 2>&1
sh "$here/build_api.sh"
sh "$here/build_multi.sh" 8
sh "$here/build_multi.sh" 8 2
sh "$here/build_trx.sh"
fail=0
sh "$here/run_equiv2.sh" || fail=$((fail+1))
sh "$here/run_equiv8.sh" || fail=$((fail+1))
sh "$here/run_bypass.sh" || fail=$((fail+1))
sh "$here/run_trx.sh" || fail=$((fail+1))
sh "$here/run_new.sh" || fail=$((fail+1))
[ $fail = 0 ] && echo "redip all: PASS" || { echo "redip all: $fail FAIL"; exit 1; }
