#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T12 §14.8: the debugger's stop is a freeze. Builds the bench (Verilator), makes the two
# cartridges, boots the machine (and the cartridges), runs the tests. One process, niced.
set -e
here=$(cd "$(dirname "$0")" && pwd)
nice -n 15 sh "$here/build.sh"
cd "$here/build/run"
python3 "$here/gen_crts.py" "$here/build/crts"
nice -n 15 ../tb_dbgsess boot ../tb.state "$here/build/crts/ef.crt" ../tb_ef.state "$here/build/crts/n16.crt" ../tb_n16.state
nice -n 15 ../tb_dbgsess run ../tb.state EasyFlash ../tb_ef.state "$here/build/crts/ef.crt" Normal16K ../tb_n16.state "$here/build/crts/n16.crt"
