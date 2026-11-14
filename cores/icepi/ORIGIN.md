# cores/icepi — our C64 core (origin: icepi-zero-c64)

Since 2026-10-01 this is TRX64 Ultimate's own code: edit it here directly.
It started as a copy of the C64 gateware of icepi-zero-c64 (m1nl, with Alex's
fork, branch easyflash-writable), taken with the former tools/vendor_icepi.sh:

| part | commit |
|---|---|
| icepi-zero-c64 | a50750d745a426e5b5f138843695ac713b50297f |
| gateware/c64_redip_cia/reDIP-CIA | 7e077151804106269eb522b63ccca29b7279d9fe |
| gateware/c64_redip_sid/reDIP-SID | f89f53e3108bcb03e8f23505bc3a70d0c206e0e6 |
| gateware/usb_hid_host | e49217661b275554807760d4e12097b0f189316a |
| gateware/vicii-kawari | ca8c43dc69358173e5f0cb632a8c3b91f309cb5a |

The former local changes are baked in (they were patches in tools/patches/icepi,
removed with the commit that wrote this file; see its parent in git history):
`default_nettype none` commented out in every Verilog file (Vivado 2024.1
Synth 8-6735), and
- 0001-kawari-hdmi-duplicate-parameters
- 0002-cartridge-io2-we-valid-reg
- 0003-keyboard-declare-before-use
- 0004-vic-native-pixel-stream
- 0005-board-keyboard-joysticks
- 0006-sid-audio-out
- 0007-cartridge-io2-write-tap
- 0008-cpu-port-8-bit-floating-bits
- 0009-cia2-port-b-own-outputs
- 0020-app-dma-bus-cycles-ultimax
- 0030-external-cartridge
- 0040-optional-internal-1541
- 0050-dma-busy-for-machine-pause

Own edits since (made in place, T60, 2026-10-06; docs/specs/T60-reu-16mb.md): `c64_reu.v` 24-bit expansion
address with Gideon's size mask (reu.vhd), $DF06 8-bit write and his read-back, status bit 4 from the size;
`c64_top.v` `reu_size` input, 16 MB command address `{1'b0, reu_mem_addr[23:1]}`, `reu_present` live from
`flags[12]` (held while the REU moves data) instead of sampled at the reset.

Own edits since (T25.4, 2026-10-07; docs/specs/T25.4-redip-ultisid.md): `c64_redip_sid/reDIP-SID/gateware/*.sv`
(the pipelines for NS SIDs, new `sid_core.sv`; commits 2dc7187, 547ebd1 and 10f43f4 are the patch for Dag Lem),
`c64_redip_sid/sid_api.sv` (NS = 2 on `sid_core`), new `c64_redip_sid/sid_multi.sv` (8 SIDs, TRX_SID=redip8).
Own edits since (U7, 2026-11-07; docs/specs/U7-sid-filter-precision.md): `sid_filter.sv` carries
`FILTER_FRAC` fractional bits in its filter state with rounded shifts (0 = Dag Lem's arithmetic bit
for bit, the default of his files; our `sid_multi.sv` and `cores/trx_sid/rtl/trx_sid.v` build 6),
`sid_core.sv` passes the parameter through, and `c64_redip_sid/muladd.sv` (ours, GPL-3) took width
parameters with the old 16x16->32 as defaults.
Own edits since (U12, 2026-11-14; docs/specs/U12-the-sound-of-a-6581.md, ADR-0071): `sid_filter.sv` has
`OPAMP_6581`: a 6581's filter output reaches the mixer through the op-amp curve libresidfp measured (new table
`sid_opamp_6581.mem`, written by `cores/trx_sid/model/gen_opamp.py --redip`), and the master volume multiply moves
to stage 12; `sid_core.sv` passes it through and takes the audio four cycles later. 0 = Dag Lem's mixer bit for bit
and cycle for cycle, the default of his files; our `sid_multi.sv` and `trx_sid.v` build 1.

Every modified reDIP file (SID and CIA) carries a dated "Modified by TRX64 Ultimate" notice under
Dag Lem's header (CERN-OHL-S v2 section 3.3 b, added 2026-10-21).

Licences: GPL-3 (icepi, kawari), CERN-OHL-S v2 (reDIP SID/CIA, settled with Dag Lem on
2026-10-06: ../../LICENSING.md). Per-part authors and licences (Apache-2.0 usb_hid_host,
MIT OR Apache-2.0 hdmi/): ../../PROVENANCE.md. Changes worth giving
back go to Alex's fork as separate commits.
