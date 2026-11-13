# U13 — The monitor reads $D000-$DFFF as the CPU would

**Status:** BUILT in the gateware and the app and green in the benches (2026-11-13): `tb_statebus look`, `tb_uci_core`,
the equivalence proofs of §5. No bitstream carries it yet, so the board test `U13-MON` is open.
**Repo:** TRX64-Ultimate (gateware: `cores/icepi/c64_bus_arbiter.v`, `c64_top.v`, `c64_turbo.v`,
`cores/trx_sid/rtl/trx_sid.v`, `c64_trx_sid.v`, `trx_sampler.v`, `designs/c64_basic/c64_core.v`, `dma_win.v`,
`cart_slot.v` + `cores/icepi/c64_cartridge.v`, `top.v`, `trx_io.v`/`trx_sys.v` (register docs); app:
`software/io/c64/trx_lens.c`, `trx_lens_fw.cc`, `apps/trxmon/src/hw_trx.c`, `mach.c`).
**Number:** U13 (registry: the TODO table in `TODO.md`).
**Milestone:** the release U7 is in (C64 Ultimate 0.2.x).
**Origin:** MetalDust, 2026-11-13: trxmon `m d030 d031` showed `FF FF` while the game had written `$0E` to
`$D031` and its own read-back saw it. Alex: *"der IO Teil, d000-dfff muss doch aber vom Monitor gescheit zu lesen
sein ... Wie lösen wir das sauber und for good?"*, then *"Spec und dann bauen"*; design decided 2026-11-13 (§2).

**Depends on:** [T21](T21-monitor.md) §3.4 (the lenses), §3.4.1 (the lens unit and the look); [T29](T29-turbo.md)
(the turbo registers); [T28](T28-uci.md) (the UCI); [T25.6](T25.6-sampler.md) (the sampler's registers).
**Changes:** T21 §3.4 (the `io` lens without `sidefx`) and §3.4.1 (classes 2 and 3 of the look).

---

## §1 The problem

Without `sidefx`, `trx_lens.c` `io_peek()` does not read I/O through the bus. It assembles each byte from a
source chosen in software:

| Range | Source today | Wrong where |
|---|---|---|
| `$D000-$D3FF` | the VIC's state-bus words, register `& $3F`; `$30-$3F` give `$FF` (`trx_lens.c:189`) | the turbo registers `$D030/$D031/$D03C` and their mirrors (T29): `$FF` while the CPU reads the register |
| `$D400-$D7FF` | none: `--` | everything (the UltiSIDs, a socket) |
| `$D800-$DBFF` | the session window, gone with SLIM (`hw_session_read` needs CAPS2 b3): `--` | everything |
| `$DC00-$DDFF` | the CIAs' state-bus words | right (`cia_core.sv` exports the read view) |
| `$DE00-$DFFF` | the look (`pk_*`): the emulated cartridge and the REU | the UCI ("shows what is below it", T21 §3.4.1), the sampler's registers (`$DF20-$DFFF`), a socket mapped there |

The decode (which device answers which address) lives twice: in the arbiter for the CPU, and in this table for
the monitor. Every device added after T21 had to be added to the second copy by hand; T29's registers were not,
T25.6's were not, T37's sockets were not. That is the bug class, not one bug.

## §2 Decision (Alex, 2026-11-13)

**The look, zero cycles.** The monitor's I/O read is a look, not a bus cycle. A bus cycle on a stopped machine opens
the gate for one whole C64 cycle (`sim/statebus` §4c: at least 32 gclk edges per byte): the VIC, the CIA timers and
the SID run on while the CPU is held, so `m d000 d0ff` would push the machine 256 cycles on, its timing after the
resume would differ, and Timewarp would record a different history. A side-effect-free bus cycle ("dry", proposed
first) was rejected for that reason.

**One decode.** The arbiter's I/O decode becomes a function that the bus path and the look both call, so the look
cannot pick a different device than the CPU's read would. Every device on the I/O bus gives the look a read view: the
value a CPU read would return now, from the same registers, without the read strobe. Where a device has two read paths
(bus and look), both call one function.

## §3 Design

### 3.1 The decode

`c64_bus_arbiter.v` `io_decode(a, we, ...)` returns the device for an I/O-mapped access to `a`: VIC, SID, colour RAM,
CIA 1, CIA 2, cartridge IO1, cartridge IO2, REU or none, and the socket select. It is the former case statement of the
bus path (`$D0-$D3` VIC ... the IO2 arbitration between REU and cartridge with `reu_override`, the sockets, "a
cartridge keeps IO1/IO2"), unchanged in content. The bus path calls it with the bus address; the look calls it with
the look address and `we = 0`. Outputs to the look: the device, the socket select, `xsel` (our own responder's range:
`$DE00-$DFFF` without a socket, as `io_ext_sel`) and `last_bus` (the open bus).

### 3.2 The read views

| Device | Read view | Two paths, one function |
|---|---|---|
| Turbo registers `$D030/$D031/$D07A/$D07B/$D0BC` + mirrors | `c64_turbo.v` `treg()`: register, enable word, `$FF` without it | CPU read (`tr_oe`) and look |
| VIC `$00-$2E` | kawari `sb_regs` (the read case without its strobe, `registers_scaled.v:255`); `$2F-$3F` `$FF` as the read case's `default` | – |
| UltiSID | `trx_sid.v` `rdsel()`: POT, OSC3, ENV3, the bus value | bus read, the turbo CPU's fast read, look |
| SID socket (T37) | none: a real chip, needs a bus cycle → `ext` | – |
| Colour RAM | the low nibble from the colour RAM's second port (`dma_win`), the high nibble the open bus | – |
| CIA 1/2 | `cia_core.sv` `sb_regs` ("the 16 registers as a read returns them") | – |
| REU | `c64_reu.v` `pk_dout` ("without clearing the status") | – |
| Emulated cartridge | `cart_slot` `pk_*`; the EEPROM DO bit (GMod2) merged with the open bus by `pk_io_mask` as `dout` does | – |
| Physical cartridge (T16) | none: needs a bus cycle → `ext` | – |
| Sampler `$DF20-$DFFF` (T25.6) | `trx_sampler.v` `rd_val()` | bus read and look |
| UCI (T28) | Gideon's command interface answers the slot bus address (its response and status RAM are read through a pointer, so there is no second read port): the look puts its address on that bus (`c64_core` `bus_addr`) whenever the bus master's address is not in `$DE00-$DFFF` or the machine is halted, and takes the answer after 4 clocks of that (the UCI registers it twice). A CPU or DMA read takes the UCI's byte only for an access in `$DE00-$DFFF`, and the address is the master's again before that byte is sampled, so the look works stopped and running; a program that keeps the bus in `$DE00-$DFFF` makes it wait (`pk_wait`, a timeout of 8192 clocks answers `ext`). Gideon's VHDL is unchanged | – |
| Nothing | `last_bus`, the arbiter's open bus | – |

Priority, as the arbiter's I/O mux: turbo, VIC, socket, SID, CIA 1, CIA 2, REU, cartridge; over `$DE00-$DFFF`
(`xsel`) our own responder (UCI, sampler) last, as `io_ext_oe` does on the bus.

**Open bus.** The look returns `last_bus` at the stop. A CPU read in its next cycle would get the VIC's phi1 fetch of
that cycle instead. The look marks such bits (`LK_RES` b13 OPEN); they are not a chip's state.

### 3.3 The lens unit

`LK_REQ` class 2 and 3 ask the **whole I/O page** by address (CAPS3 b1; before U13 they asked the cartridge's IO1/IO2
only, and `$DE00-$DFFF` gives the same answers as before plus the UCI, the sampler and the sockets). `dma_win` waits 8
clocks, and longer while `pk_wait` (the UCI), then: `ext` → done with `ext`; colour RAM → reads the nibble through its
port (after the window's own colour RAM job, never beside it); cartridge RAM → the DDR2 read as before; else the byte.
`LK_RES` b13 OPEN: some bits are the open bus. No bus cycle, no C64 clock edge, no strobe.

`REAL` (b18) is unchanged: one bus cycle with side effects, the machine paused, for `sidefx`.

### 3.4 The app

`trx_lens.c` `io_peek()` with CAPS3 b1: every `$D000-$DFFF` byte is a look; `ext` → `--` (with `sidefx`: `REAL`, as
before). Without CAPS3 b1 (an older bitstream) the old sources stay. The state bus is read only for the PLA (`$01`,
GAME/EXROM). trxmon and the firmware share `trx_lens.c`; `hw_lk`/`fw_lk` and the CAPS check change in both.

`sidefx` and the REST call without `lens` (Gideon's DMA read: a bus cycle, side effects included) are unchanged.

## §4 Out (decided with this spec)

- **A bus cycle for the look**, with or without side effects: §2.
- **The physical port and the sockets** without `sidefx`: no read view exists; `--` as for the port today.
- **The 1541's VIAs** (`drive8` lens): unchanged, not on the state bus.
- **REST `machine:readmem` without `lens`**: Gideon's DMA read; a DMA read of `$D031` keeps answering the VIC (`$FF`),
  the arbiter shows the turbo registers to the CPU only (`!reu_dma_active`). What Gideon's core answers there is not
  measured.

## §5 Verification

- **`sim/statebus` mode `look`** (`tb_statebus look <state>`): from READY, with the CIA 1 ICR flag set (IRQ masked, timer
  A run out), a sprite-sprite collision (`$D01E`), a finished REU transfer (status END), the turbo registers on
  (`$D031` written by a program): for every address `$D000-$DFFF`: look twice (equal, no gclk edge), a `REAL` read,
  look again; the `REAL` byte equals the first or the second look outside the OPEN bits. The side effects are
  exercised (ICR, collision, REU status: first look ≠ third), and the state-bus words before and after the whole
  look sweep are equal.
- **`sim/uci` (`tb_uci_core`)**: on a halted machine the look of the UCI's registers equals what the C64 reads next,
  the look pops nothing (the C64 still reads the whole response), and with the machine running the look answers too.
- **Unchanged:** `sim/slot` (`lens`), `sim/statebus` (`run`), `sim/turbo`, `sim/sid`, `sim/sampler`, `sim/uci`,
  `sim/dbgsess`.
- **Board `U13-MON`**: trxmon with the turbo on, the REU and UA on, a program that writes `$D031` and dumps
  `$D000-$DFFF` through the CPU into RAM; stopped, `m d000 dfff` equals that dump outside the time-varying
  registers.

## §6 Cost

No block RAM. LUTs: the VIC view (48:1 x 8), the CIA views (2 x 16:1 x 8), the SID look port, the turbo and sampler
functions, the decode a second time, the look mux, the lens unit's states: estimated 400-500 LUT.

## Status and history

- **2026-11-13, built and green in the benches.**
  - Equivalence (yosys `equiv_make`/`equiv_induct` against the commit before, the new look ports taken out): the arbiter
    with `io_decode()` 380 of 380 cells proven (a swapped CIA 1/2 in the gate: 2 unproven, the negative control);
    `c64_turbo` with `treg()` 765/765; `trx_sid` with `rdsel()` 1572/1572; `trx_sampler` with `rd_val()` 3387/3387.
  - `tb_statebus look`: 2 x 4096 looks, 0 C64 clock edges, the state bus unchanged, 0 `ext`, 1280 with open-bus bits
    (`$DE00-$DEFF`, where nothing answers, and the colour RAM's high nibble; the bench's UltiSID decode answers all of
    `$D400-$D7FF`); look/REAL/look for every address: 0 differ, 17 addresses where the bus read changed
    the look (ICR, collisions, REU status, the SID's bus value); turbo registers equal the program's own reads
    (`$D030 FE`, `$D031 70`, `$D071 70`, `$D131 FF`); the look of `$D031` while running. Negative control: the CIA 1
    view wired to CIA 2 gives 81 differing bytes and the ICR/collision checks fail. The first run found the 12 turbo
    register addresses (`$D030/$D031/$D03C` and their mirrors) where a DMA read gives the VIC's `$FF`: §4, checked
    against the CPU since.
  - `tb_uci_core`: halted with the response validated, `$DF1C-$DF1F` = `E0 C9 55 30` twice ('U', '0'), no UCI read
    counted; the program then reads the whole response and status (54 reads, as before); running, `$DF1D` = `C9`
    without a UCI read. Both benches are in CI tier 2 (`statebus` runs `look` too, `uci_core` is new).
  - `sim/slot` `lens`, `lens_pause`, `openbus`, `sock_dec`, `sock_de00`, `sock_rw` green (the look at a real-port
    cartridge still says `ext`, the socket decode unchanged); `tb_statebus run` (the existing sections) green; the trxmon host tests 1131/0 (new: the look at the I/O page, `ext`
    with and without `sidefx`, an older bitstream); the firmware builds.
  - Design change on the way: the UCI's window exported from Gideon's `command_protocol.vhd` (to refuse it while running)
    was replaced by the address handover above, which reads the UCI running as well and leaves his VHDL alone.
- 2026-11-13: spec written after the design question (§2) was decided with Alex.
