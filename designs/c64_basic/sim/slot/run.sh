#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run every T16 bench test (build.sh first), one after the other, niced.
# Exit code 0 = all PASS.
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/build/run" || exit 1
rc=0
for t in ${TESTS:-idle 8k 16k ef ef_ro pause lens lens_soak lens_pause dma_wr cpuport cpuport_cp btn openbus sock_dec sock_rw sock_split sock_detect sock_de00 sock_pause sock_idle}; do
  nice -n 15 ../tb_slot $t . > "../$t.log" 2>&1 || rc=1
  tail -1 "../$t.log"
done
exit $rc
