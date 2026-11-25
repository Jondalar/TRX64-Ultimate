# Licensing

TRX64 Ultimate's own code is under the **GNU General Public License v3.0 or later** (`LICENSE`).
The CERN-OHL-S v2 parts by Dag Lem keep their licence; with respect to them our GPL code is an
**"Available Component"** in the sense of CERN-OHL-S v2 §1.2, with Dag Lem's agreement ("I have no
objections to you using my work, as long as the (modified) work itself stays open and under the
same license").

| Part | Where | Licence | Note |
|---|---|---|---|
| TRX64 Ultimate gateware, tools, CI, docs (ours) | everything not listed below | GPL-3.0-or-later | Available Components for the CERN-OHL-S parts |
| reDIP-SID, reDIP-CIA (incl. VIA) by Dag Lem | `cores/icepi/c64_redip_sid/reDIP-SID`, `cores/icepi/c64_redip_cia/reDIP-CIA` | CERN-OHL-S-2.0 | modifications stay open under CERN-OHL-S-2.0 |
| icepi-zero C64 core (Mateusz Nalewajski) | `cores/icepi` (except the subfolders below) | GPL-3.0-or-later | |
| VIC-II Kawari | `cores/icepi/vicii-kawari` | GPL-3.0 | |
| hdl-util HDMI | `cores/icepi/hdmi` | MIT or Apache-2.0 | |
| usb_hid_host | `cores/icepi/usb_hid_host` | Apache-2.0 | |
| Gideon's 1541 Ultimate FPGA sources, with the few files of his tree our benches read | `vendor/gideon` (our own copy, in git; upstream commit and our patches in `vendor/gideon/VENDOR.md`, origin in `PROVENANCE.md`) | GPL-3.0 | our patches (`tools/patches/gideon`) are changes to GPL-3 files, own work |
| Files of Gideon's firmware (1541ultimate) that trx64u's libraries copy or port | `software/trx64u` (each names its origin in a `PROVENANCE` line, e.g. `uci/rtc.h`, `base/u64cfg.cc`, `monitor/monsrv.cc`) | GPL-3.0 | the firmware itself (`ultimate.app`) is no longer in this tree: the Gitea branch `gideon-app` builds it (U20 D13) |

Whoever passes on a bitstream or the app passes on the corresponding sources of every part with it
(or an offer to provide them), under the licence of that part.

Our own source files carry `SPDX-License-Identifier: GPL-3.0-or-later`, except the ones made from
or taken from others' files (`software/bootrom/bootrom.c`, the pin XDCs). Every reDIP file we
changed carries a dated modification notice under Dag Lem's header (CERN-OHL-S v2 §3.3 b).
