#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/trxdbg.py (or any script that imports it) on the UE2.

trxdbg.py checks the JTAG IDCODE against the C64U's XC7A50T and refuses anything else. The UE2's XC7A100T is the
same board with a larger FPGA (T53), so this wrapper lets that one IDCODE through and changes nothing else:
the shared tools/trxdbg.py stays untouched. Use it exactly like trxdbg.py:

    ue2_trxdbg.py console | boot staged ultimate.app | regs | ...
    ue2_trxdbg.py --run tests/lorenz.py [args]     run another script with the same patch (lorenz, t40_modes, ...)

Caveat: trxdbg's `boot check --dump` compares against the app at 0x220000 (the 50T slot); on the 100T the
slot is 0x3C0000, so do not use that option here.
"""
import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import trxdbg  # noqa: E402

IDC_50T, IDC_100T = 0x0362C093, 0x03631093
_orig = trxdbg.DebugTap.idcode


def _idcode(self):
    v = _orig(self)
    if v & 0x0FFFFFFF == IDC_100T:
        return (v & 0xF0000000) | IDC_50T
    return v


trxdbg.DebugTap.idcode = _idcode

if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--run":
        script = sys.argv[2]
        sys.argv = [script] + sys.argv[3:]
        runpy.run_path(script, run_name="__main__")
    else:
        sys.exit(trxdbg.main(sys.argv[1:]))
