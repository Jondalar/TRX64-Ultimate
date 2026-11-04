# T37 — Real SID chips in the C64U's SID sockets

**Status (Alex, 2026-10-03):** not in the M1 release candidate; no chip to test with. Alex tries to get a Dual ARMSID. Q1 answered: no SID, FPGASID, ARMSID or SIDKick here today.

**Status (2026-10-16):** merged into integ-m1 (Alex: "T37 can go in too as far as I'm concerned"; [ADR-0056](../adr/0056-trx64-ultimate-is-the-monolith.md)). Q2, Q4, Q5 and Q7 stay open; the merged RTL uses the proposals below.

**Status (build trial, 2026-10-15):** RTL for §4 is built on branch `t37-sockets` to see whether Vivado routes and
meets timing (Alex: "try on a branch whether the build works"). The milestone stays M2 (ADR-0047). Q2, Q4, Q5 and Q7 are
not decided: the trial uses the proposals and every place that depends on them is marked "build trial uses the
proposal, decision open". Details and coverage: §9.

**Status:** spec 2026-10-03, branch `m1-specs`. **Task:** TODO.md T37. **Milestone:** M1 (what Gideon's
app offers for the sockets works on our core).
**Shares with:** T16 (the socket bus is the port's bus, `slot_port.v`), T25.1/T25.3 (UltiSID
decode, mixer), T14 (app I2C master), T12 (pause, shadow session), T47 (POT lines).
Evidence rule: only code (app source under `software/ultimate-app/software`, our RTL), the pin
measurements in the tools fork (`1541ultimate-tools/docs/c64u-pinmap.md`) and Gideon's XDC.
Hypotheses are marked **H**.

## 1. Goal

A real SID (6581, 8580, FPGASID, ARMSID, SwinSID, PDsid, SIDKick, ...) in socket 1 or 2 works as on
Gideon's core: the app detects it, the user enables it, it answers at the configured address, and its
audio reaches HDMI/I2S through the app's mixer. Non-goal: anything that needs the closed UltiSID
(T25).

## 2. What the app does today (code)

### 2.1 Config items (`u64/u64_config.cc`)

| Item | Where | Values |
|---|---|---|
| SID Socket 1 / 2 (`CFG_SOCKET1/2_ENABLE`) | `:408-409` | Disabled / Enabled |
| SID Detected Socket 1 / 2 (`CFG_SID1/2_TYPE`, read-only) | `:410-411`, `:285` | None, 6581, 8580, FPGASID, SwinSID Ultimate, ARMSID, ARM2SID, SidFx, FPGASID Dukestah, PDsid, SIDKick (Teensy), SIDKick Pico |
| SID Socket 1 / 2 1K Ohm Resistor (`CFG_SID1/2_SHUNT`) | `:412-413`; greyed out unless `isEliteBoard()` (`:545-548`) | Off / On |
| SID Socket 1 / 2 Capacitors (`CFG_SID1/2_CAPS`, read-only) | `:414-415`, `:287` | 470 pF / 22 nF |
| SID Socket 1 / 2 Address (`CFG_SID1/2_ADDRESS`) | `:419-420`, `:219-236` | Unmapped, `$D400-$D7E0` in steps of `$20`, `$DE00-$DFE0` (49 entries) |
| Ext DualSID Range Split (`CFG_STEREO_DIFF`) | `:421`, `:266`, `:324` | Off, A5, A6, A7, A8, A9 |
| Auto Address Mirroring | `:426`, `auto_mirror` `:2446` | clears the A5..A9 compare bits that agree across all mapped SIDs |
| Vol / Pan Socket 1, Socket 2 | `:440-461` | mixer sources 2 and 3 (T25.3 §1.1) |
| Bus Operation Mode (`CFG_BUS_MODE`, default index 0 = Quiet) | `io/c64/c64.cc:159`, `:148` | `C64_BUS_BRIDGE` = 0, 1, 2, 3, 5 |

### 2.2 Registers written (`system/u64.h:110-122`, base `C64_IO_BASE` = 0x10180000)

| Reg | Offset | Written by | Value |
|---|---|---|---|
| `C64_SID1/2_BASE` | +0x08/+0x09 | `U64SidAddressing::effectuate_settings` `:871-901`; `SetSidAddress` `:1913`; `S_SetupDetectionAddresses` `:2336` | `u64_sid_offsets[idx]` = A11..A4 of the address (`$D400` = 0x40, `$DE00` = 0xE0, unmapped = 0x01) |
| `C64_SID1/2_MASK` | +0x0C/+0x0D | same | 0xFE & ~split bit (default 0xFE; 0xF0 during detection) |
| `C64_SID1/2_EN` | +0x11/+0x12 | `U64SidSockets::effectuate_settings` `:829-832`; detection sets 1 | 0/1 |
| `C64_STEREO_ADDRSEL` | +0x14 | `:894`; detection sets 0 | index 0-5 (= Off, A5..A9); `stereo_bits` `:324` = {0x00, 0x02, 0x04, 0x08, 0x10, 0x20} are bit positions inside the A11..A4 byte, i.e. A5 = 0x02 |
| `C64_BUS_BRIDGE` | +0x2A | `C64::ConfigureU64SystemBus` `c64.cc:1877` | see §2.1 |

The split is shared by both sockets (`get_sid_addresses` `:1258-1274`). With the split on, the chip
at base `b` is two SIDs: the app sets `mask = 0xFE & ~split`, `base &= ~split`
(`fix_splits` `:1276`) and `SetSidAddress` for a type >= 3 (FPGASID, ARMSID, ...) refuses a
base that already has the split bit (`:1913-1930`). The "B" SID of a socket is the access with the
split bit set (`MapSid` slots 4/5, `:1979-2010`). **H:** `SLOT_SID_ADDR` (K17) is that address bit,
for the chip's second personality (FPGASID/ARM2SID pin that selects the second SID).

### 2.3 Supply, shunt and capacitor selection: I2C, not FPGA

`U64SidSockets::effectuate_settings` (`:767-834`) packs per socket `reg | shunt << 2 | cap << 3`
(`reg` = 0 socket disabled, 3 for a 6581 = 12 V regulator on, 2 otherwise = 9 V; bit 0 voltage,
bit 1 regulator enable, bit 2 shunt, bit 3 caps, `:772-824`), socket 2 in the high nibble, and writes
it with `i2c_write_byte(0x40, 0x01, v)`, `(0x40, 0x03, 0x00)` on the 1.8 V channel (`:817-824`): a
PCA9554-class expander (output register 1, direction register 3). `custom_hardware_init` writes
`0x00` to it at boot (`system/u64ii_init.cc:128-130`). Our `app_i2c.v` passes everything except
address 0x64 to the real pins (header, `designs/c64_basic/app_i2c.v:9-22`), so **this part already
reaches the board** and needs no FPGA work. Risk note: a wrong voltage destroys a chip; T37 never
touches this path and the first test reads the PCA registers back before any chip is inserted.

### 2.4 Detection and access (everything through DMA to `$D4xx/$D5xx`)

- `U64SidSockets::detect()` (`:669-765`, at app start): `C64_MODE_UNRESET`, wait for reset sense
  (T39), `S_SetupDetectionAddresses` (`:2336`): socket 1 at `$D400-$D4FF`, socket 2 at
  `$D500-$D5FF` (base 0x40/0x50, mask 0xF0 = compare A11..A8 only), UltiSIDs parked at `$D600`,
  `SID1/2_EN` = 1, split 0, then 1 ms wait.
- `detectFPGASID` (`:586`): `hard_stop`, DMA writes `0xEE`, `0xAB` to registers 25/26, reads 0 and 1,
  expects `0x1D 0xF5`. `detectRemakes` (`:609`): writes 'S','I','D' to 29-31 with 10 us gaps, reads 27/28
  ("SW", "NO"), then PDsid and SIDKick probes. `detectDukestahAdapter` (`:551`). `S_SidDetector`
  (`:2369`): runs `DetectSidImpl` (6502-side timing loop through C64_PEEK/POKE DMA) for 6581/8580.
- After a changed detection the sockets stay disabled until the user approves; 6581 sets caps 470 pF
  and shunt On, 8580 sets 22 nF and shunt Off (`:721-760`).
- `access_socket_pre/post` (`:2576-2610`): same detection addresses, C64 stopped, for the SID editor
  and the chip drivers (`u64/sid_device_*.cc`).
- `u64_mute_sids` (`:1345`) zeroes mixer bytes 0-7 (UltiSID 1/2 and sockets 1/2, R/L): the **freezer
  mutes the sockets only through the mixer**, so socket audio is mixed digitally in the FPGA.

### 2.5 Audio path (code facts, mapping **H**)

The codec init enables the ADCs with AUXIN to both ADCs (`io/audio/nau8822.cc:21-23, 41-42`; our
`c64u_keyboard.v:56-66` already sends the same writes) and Gideon's XDC has `AUDIO_SDO` on AB3
(`1541ultimate-tools/pinmap/c64u_gideon_issue615.xdc:80, 317`) next to MCLK/BCLK/LRCLK/SDI. Our
`pins.xdc:126-132` and `top.v:63` have no `AUDIO_SDO`. Mixer sources "Socket 1/2" are bytes 4-7
(`setMixer` `:1369-1391`). **H:** the real SIDs' analog output goes into the codec's AUXIN, ADC
data comes back on SDO, and the FPGA mixes it as sources 2/3; which ADC channel is which socket, and
what "Sampler L/R" (sources 4/5) is, is not visible in the app (M5).

## 3. What our core has / lacks (code)

State of integ-m1 a122bed, before the build trial (§9.2 lists what the branch changes).

| Piece | State | Where |
|---|---|---|
| Socket balls `SLOT_SID1n/2n`, `SLOT_SID_ADDR` | ports, held 1 / 1 / 0 | `designs/c64_basic/top.v:116-118`, `pins.xdc:99-104`, T16 §13.1 |
| Registers 0x08/09/0C/0D/11/12/14 | page-011 RAM latches, no effect | `ultimate_sys.vhd:996-1006` only wires 0x0A/0B/0E/0F/20-29 |
| Decode of the sockets | none; the arbiter decodes `$D400-$D7FF` as one `sid_cen` | `cores/icepi/c64_bus_arbiter.v:372-377`, `:672-677` |
| Internal SID answer | reDIP: `sid_hit = 1'b1`, **no address decode at all** (the "UltiSID Address" menu has no effect in the default build); T25.1 engine (`TRX_SID=1`): decode in `trx_sid.v:77-79`, `$D400-$D7FF` only | `cores/icepi/c64_top.v:507` |
| The port's bus machine | ROM/IO1/IO2 strobes, per-half-cycle direction, park state, contention guard | `designs/c64_basic/slot_port.v:88-239`, T16 §13.3 |
| DMA cycles to the port | ordinary cycles (app DMA while paused) | T16 §8.3, `c64_core.v:442-457`, arbiter `bus_addr` mux `c64_bus_arbiter.v:411-416` |
| Audio in from the codec | none (no `AUDIO_SDO`) | `top.v:63`, `pins.xdc:126-132` |
| Mixer sources 2/3 | not connected | T25.3 E4 (`T25.3-lp-filter.md:167`) |
| I2C to the PCA9554 (0x40) | passes to the pins | `app_i2c.v:9-22` |

Pin facts from Gideon's bitstream (SAMPLE, sockets enabled, no chip; `c64u-pinmap.md:177-215`):
socket 1 select (M20) is low only for `$D400`, socket 2 select (N20) only for `$D420` (not A5);
"in Quiet mode the bus is only driven for socket cycles, so address lines idle otherwise"; the
sockets share A0-A15, D0-D7 behind the 74ALVC164245 transceivers with the port.

## 4. Design

### 4.1 One bus, three kinds of slave

The sockets are two more chip selects on the port's bus. They get the port's machinery
(`slot_port.v`) unchanged, with four additions.

1. **Decode** (arbiter, C64 clock, next to `cart_io1_cen`): for socket `n`
   `sockn_sel = io_enable && sock_en[n] && ((addr[11:4] & mask[n]) == base[n])`, where `io_enable`
   is the PLA's "I/O visible at `$D000-$DFFF`". `$DE00-$DFFF` bases are decoded here too (the
   `u64_sid_offsets` table has them); at `$DE00/$DF00` a cartridge (plugged or `.crt`)
   keeps IO1/IO2 and the socket gets no select; otherwise the socket wins over the REU and the UCI (Q4, guardrail). Compare convention: base compared unmasked, as `trx_sid.v:78` (T25.1 Q1: same open point, one
   measurement settles both, M2). The config bytes cross from `cpu_clk` with two flip-flops as
   `trx_sid.v` does (`cfg_c1/cfg_c`).
2. **Strobes and bus gating** in `slot_port.v`: new inputs `sock1_sel`, `sock2_sel` (CPU half only,
   like `io1_n`), static `sock_en[1:0]`, `stereo_sel[2:0]`. In the CPU half of a cycle that hits a
   socket:
   - `sel_now` (`slot_port.v:129`) includes the hit, so a read sets `h_rd` and DATADIR/DATAOEn follow the existing
     schedule (T16 §13.3: DIR 0 from port tick 2, OE on 3-17, sample D at the end of tick 15,
     `:154`, `:184`); a write sets `h_wr`/`h_ws` and drives D as for IO1 writes.
   - `SLOT_SID1n/2n` (low) from port tick 3 to 17, registered like `p_io1` (`:175`), one tick
     later than the address so that A0-A4/R/W are stable at the chip when CS falls (the tick-2 address
     already sits behind the transceiver, T16 §13.3).
   - `SLOT_SID_ADDR` = `addr[4 + stereo_sel]` for `stereo_sel` 1-5 (A5..A9; M3, 2026-10-15), registered with the
     address, held through the cycle. Idle level (M3): low with `stereo_sel` 0 (Off), **high** with a split on, outside
     the cycles whose address the port drives. (Until 2026-10-15 this line read `addr[3 + stereo_sel]` and "idle 0":
     the first is off by one for A5 = `addr[5]`, the second contradicts M3.)
   - The write inhibit of T16 §6.1 does not apply (SID writes always pass).
3. **Arming per cycle, not per machine** (Gideon's "Quiet" default): the port is armed for a
   resource that stays selected (cart ROM/IO, `present`, `:114`, T16 §13.1); a socket is *not* a standing
   selection. `p_aoen`, `p_ahi_t` (A12-A15 and R/W: the SID needs R/W), `p_doen` for a socket cycle
   come on at port tick 2 only if the decision at core tick 1 (`tk == 5'd1`, `:142`) found a socket hit,
   off again after tick 18; the address transceiver is otherwise off (`:170-171` use `act`
   today). The guard (`g_idle`, `:207`) learns a third cause "socket cycle". With a cartridge armed
   the existing every-cycle drive covers the sockets too. `SLOT_PHI2`/`DOTCLK` must run while a
   socket is enabled and its chip needs the clock (M1, Q2).
4. **Read path into the core:** `slot_port.dout` (the last D sample, already routed to
   `ext_cart_dout`, `c64_core.v:454`) gets a socket flavour: the arbiter's I/O read mux
   (`c64_bus_arbiter.v:372-377`) takes it for a socket hit in place of `sid_dout`. A hit on both a
   socket and an internal SID: the socket answers reads (Q5), writes go to both (the mirrored write is
   what a SID player with "UltiSID + socket" wants).

Register work in `ultimate_sys.vhd`: latches for 0x08/09/0C/0D/11/12/14 (0x2A stays a latch),
outputs `sock_base[15:0]`, `sock_mask[15:0]`, `sock_en[1:0]`, `stereo_sel[2:0]`, cleared by the
RISC-V reset (EN = 0, base 0x01 = unmapped, the value the app writes for "Unmapped",
`u64_config.cc:203`).

### 4.2 Detection must work through DMA

`detect()` and the chip drivers do DMA at `$D4xx/$D5xx` with the CPU stopped. The arbiter's decode
works on `bus_addr`, which carries CPU, REU and app DMA addresses (`c64_bus_arbiter.v:411-416`), so
DMA hits the sockets with no extra path. Two checks: (a) the `dma_stub` answers of `ultimate_sys`
(`ultimate_sys.vhd:413, 892, 903`) are for the time before the core runs, not after; (b) a DMA read of
a register the chip does not drive must not hang: the sample at tick 15 returns whatever the bus
holds (pull-ups on D0-D7? unknown, M7), which the app reads as "no chip" (`detectFPGASID` compares
against `0x1D 0xF5`).

### 4.3 reDIP / T25.1 and the sockets

- **Address decode for reDIP.** `sid_hit = 1'b1` (`c64_top.v:507`) means the internal SID answers
  every `$D400-$D7FF` read. With a socket mapped inside that range the internal answer must be gated:
  the reDIP build gets the same masked compare `trx_sid.v` uses (move it into the arbiter's decode,
  ~20 LUT), fed by `sid_base/sid_mask` which the build already has (`top.v:366`).
- **Priority** is Q5; the default proposal is §4.1 item 4.
- **Audio.** Socket audio is a 48 kHz stereo stream from the codec ADC, not a C64-rate sample. It is
  added at the I2S frame after the SID mix and the pause gate (`top.v:755-758`), with the app's bytes
  4-7 as gains (0x80 = 0 dB, same law as T25.3 §1.1) and the same sum into HDMI audio and UDP audio.
  New: `AUDIO_SDO` port (AB3, LVCMOS18, `pins.xdc`), a 24-bit I2S receiver in the `mclk` domain
  (BCLK/LRCLK are ours, `i2s_out.v`), two 8 x 24 multipliers shared over time (one DSP).
- **Pause.** The machine pause parks `SLOT_PHI2` low (`slot_port.v:128, :165`). A real SID with its
  clock stopped holds its output and (**H**) its state. The freezer mutes via mixer bytes 0-7
  (`u64_mute_sids`), which we honour, so the pause is silent either way. Whether a 6581/8580
  keeps its registers over a 60 s stop is B6; the alternative (free-running `PHI2` while sockets are
  enabled, CS only in phase) breaks CS/PHI2 alignment after a resume.
- **POT.** The sockets' POT pins are board lines; they see paddles only physically (T47).

## 5. Measurement plan on Gideon's core (measured on his bitstream wins)

All with the app's configuration backed up and restored (`pinmap/runs/cfg.py`), nothing saved to flash.

| # | Measures | Needs | How |
|---|---|---|---|
| M1 | `SLOT_PHI2`/`DOTCLK`/`SLOT_BA` with sockets enabled and empty vs disabled, Quiet and Compatibility | USB-Blaster, no chip | SAMPLE 300 captures per state (as the T16 reset series) |
| M2 | Which address bits select socket 1/2: base/mask compare incl. unmasked base, mask 0xF0 (detection) vs 0xFE, `$D400..$D7FF`, `$DE00/$DF00` (does a socket hit suppress IO1/IO2?), overlap of both sockets, overlap with an UltiSID | no chip | write loops from a RAM program, low share of M20/N20 vs address pattern (`pinmap/runs/addr16.py` pattern); IO1 low share while a socket sits at `$DE00` |
| M3 | `SLOT_SID_ADDR` (K17) against `C64_STEREO_ADDRSEL` 0-5: is it A5..A9 of the access, polarity, idle level | no chip | same loops with the split set through REST config ("Ext DualSID Range Split") |
| M4 | Bus drive per cycle in Quiet: AOEn/DOEn/ADDRDIR/DATADIR/A12-A15/RWn low shares, DATADIR 0 on a socket read | no chip | same captures; answers Q2 and §4.1 item 3 |
| M5 | Audio path: is the socket level in the HDMI/I2S stream proportional to mixer bytes 4-7, which channel is which socket | a real SID, an audio capture | play a tone on the chip, mixer socket volume 0 / mid / max, pan hard left / right; capture the UDP audio stream |
| M6 | Who answers a read when UltiSID and a socket hit the same address; do writes reach both | a real SID | voice 3 noise, read `$D41B` 1000 times, compare the distribution with chip-only and UltiSID-only runs |
| M7 | Read data when no chip drives D (open bus value, `detectFPGASID` result) | none | PEEK of an enabled empty socket via DMA |
| M8 | PCA9554 register after a "6581 enable" / disable cycle (supply bits only, nothing inserted) | none; a multimeter at the socket later | `tools/i2cx.py` read of 0x40 reg 1 |

### 5.1 Results 2026-10-15 (coordinator; boundary scan + REST on Gideon's core)

Setup: SIDKick Pico in socket 1, its adapter in socket 2 (A5/A6 wire on the "SID A[5..9]" header = K17, A8/IO wire on
socket 2 pin 8). Values are low shares of 200 SAMPLE captures per state while a 6502 loop writes `$FC` to one address
(raw: scratchpad `k17.json`, not kept). Gideon's detection: socket 1 "SIDKick Pico", socket 2 "None" (only the adapter).

| M | Result | Used in the design |
|---|---|---|
| M3 (answered) | K17 (`SLOT_SID_ADDR`) carries the address bit chosen by "Ext DualSID Range Split" (A5..A9) in socket/port bus cycles: split A5, `$D419` low, `$D439` high; verified for A5, A6, A7, A8, A9 on socket 1 and socket 2. Split Off: K17 constantly low. K17 also follows the bit in IO2 port cycles that are not socket hits (`$DF79`, split A7: low as A7 = 0). With a split on, K17 is low only inside the cycles that drive the address (its low share equals the SIDn / address-line low share) and **high** outside them | §4.1 item 2: `addr[4 + stereo_sel]`, driven with the address window of every socket or armed-port cycle, idle level 1 with a split, 0 without |
| M2 (partial) | Socket 1 select (M20) low only inside its window; socket 2 at `$DF40` selects N20 for `$DF40-$DF5F` (split Off), widened by the split; no select for `$D440` / `$C800`. With a split the socket's select covers both halves (split A5: socket 1 selects `$D400-$D43F`; split Off only `$D400-$D41F`). Address lines and R/W are driven only in socket/port cycles (Quiet); the low share of SIDn is about the R/W low share. **Not answered:** unmasked-base convention, mask `0xF0` vs `0xFE` on the wire, `$DE00/$DF00` against IO1/IO2, overlap of both sockets, overlap with an UltiSID | decode = the app's own mask/base (`mask = 0xFE & ~split`, `base &= ~split`) as in §4.1 item 1; no extra split logic in the decode |
| M4 (partial) | Address lines and R/W driven only in socket/port cycles. DOEn/ADDRDIR/DATADIR/A12-A15 not captured | §4.1 item 3 (Quiet per cycle) |
| M5 (partial) | `AUDIO_SDO` (AB3) is an input and toggles all the time (codec ADC running). A 440 Hz triangle written to `$D400` and to `$D420` (UltiSIDs unmapped) shows in the UDP audio stream on L **and** R at equal level (RMS about 1160) although Pan Socket 1 = "Left 3". `$DF40/$DF50` OPL writes give FM sound (the SIDKick's FM through socket 2's CS = SFX Sound Expander addresses). `$D440` and SID writes at `$DF40` are silent | §4.3 audio: the ADC pair is read as two mono sources; which ADC channel is which socket stays **H** (L and R carrying the same level says both channels hold the same mix, or that the app's pan bytes are not applied to this path on Gideon's core) |
| M2 (`$DE00/$DF00`, 2026-10-16) | EF3 in the port, socket 1 at `$DE00`, socket 2 at `$DF40`, split Off, Quiet and Compatibility, 200 SAMPLEs per state: a loop on `$DE19` gives IO1 (AB8) and socket 1 select (M20) low in the same share (0.07-0.19, write and read); `$DF59` gives IO2 (V8) and socket 2 select (N20) together. Unmapped sockets: IO1/IO2 only. Without a module in the port Gideon's core never strobes IO1/IO2 (0.00 in every state). | Q4: Alex rejects Gideon's behaviour, the module wins |
| M6 (read side, 2026-10-16) | SIDKick Pico in socket 1, split Off. A 6502 program puts the SIDKick into config mode (`$D41F`=$FF), selects version bytes (`$D41E`=$E0+i) and reads `$D41D` (Gideon's `SidDeviceSidKick::detect`). UltiSID 1 unmapped, socket 1 `$D400`: `53 4b 10 09 03 0f` ("SK"). UltiSID 1 `$D400`, socket unmapped: `e0 e1 e2...` (the UltiSID returns the last written byte). Both at `$D400`: `53 4b 10 09 03 0f`: **the socket answers the CPU read**. | Q5 proposal (socket first) matches Gideon |

Still open: M1, M6, M7, M8, the rest of M2, M4, M5 (gain law and channel assignment need a mixer sweep with one chip).

## 6. Test plan

**Simulation** (`designs/c64_basic/sim/slot`, extended; Verilator):
1. Decode: both sockets, mask 0xF0/0xFE, split bit, unmapped (0x01), `$DE00` hit, overlap with IO1.
2. A SID model on the port (behavioural 6581: CS, R/W, A0-A4, D, PHI2; 200 ns read latency) beside the
   transceiver model: write/read each register through the CPU and through the app DMA; the D
   contention checker stays on.
3. Quiet: AOEn/DOEn off in non-socket cycles with no cart; with a cart armed unchanged (the T16
   regression suite passes unmodified).
4. `SID_ADDR` follows the split bit; both sockets at once (A and B personality).
5. Priority and mirrored writes (internal SID + socket at one address), reDIP gate.
6. App flow: the `detectFPGASID`/`detectRemakes` byte sequences against a model that answers the
   FPGASID and ARMSID handshakes; `S_SidDetector` is timing-dependent: smoke run only.
7. Pause/resume with a socket cycle in flight (park state, no CS pulse on resume).

**Board** (Alex, chip in, one test at a time; I2C readback before power to the chip):
B1 PCA register value per type (M8); B2 "SID Detected" shows the chip; B3 enable, play a SID in
socket 1 from `$D400` (HDMI audio, analog out); B4 socket 2 at `$D420` with the split; B5 UltiSID +
socket at once, SID player autoconfig (`SidAutoConfig` `:2070`); B6 freeze/pause for 60 s, no state
loss; B7 EasyFlash in the port while a socket plays; B8 mute through the freezer.

## 7. Resources (rough, not measured)

| Block | LUT | FF | BRAM | DSP |
|---|---:|---:|---:|---:|
| socket decode + stereo mux (arbiter) | 25 | 4 | 0 | 0 |
| `slot_port.v` additions (select, per-cycle gating, guard cause) | 30 | 10 | 0 | 0 |
| reDIP address gate (reDIP builds only) | 20 | 0 | 0 | 0 |
| register latches + synchronizers (`ultimate_sys`, `trx_sid`-style) | 25 | 100 | 0 | 0 |
| I2S receiver, socket gain/sum | 70 | 90 | 0 | 1 |
| Total | about 170 | about 200 | 0 | 1 |

Baseline 25,346 LUT, 77.7 % (`docs/lut-budget.md`). T38's measured `app_input` (42 LUT for the same kind
of latches and OR, `T38-T39-input-reset.md` §8) is the calibration.

## 8. Open questions (Alex)

**Guardrail (Alex, 2026-10-16): "Physical chip *before* FPGA."** A chip in a socket wins over every internal (FPGA) device on the same address; the one exception is a cartridge (plugged or from a `.crt`), which wins over a socket at `$DE00/$DF00` (Q4).

1. Do you have a real SID or an FPGASID/ARMSID/SIDKick for the sockets (M5, M6, B1-B8)? Which types?
2. Quiet (bus driven only for socket/port cycles, Gideon's default) or drive every cycle once a socket
   is enabled (simpler, what T16 does for an armed cart)? **Answered 2026-10-16 (Alex): Quiet, as Gideon's core** ("Ja" to: drive the socket lines only in socket/port cycles, as measured on Gideon's core). M2/M4 (2026-10-15) show Gideon's core drives address and R/W only in socket/port cycles.
3. `SLOT_SID_ADDR`: A5..A9 of the access (H) with idle level 0? M3 answers; no decision needed before it. Answered by M3 (2026-10-15): A5..A9 of the access, idle level high with a split on, low without.
4. Socket mapped at `$DE00/$DF00`: socket wins over IO1/IO2 (proposal), or both strobes go out? **Answered 2026-10-16 (Alex): a plugged module wins; we catch the conflict instead of copying Gideon** ("If I put my SID on cartridge IO and then plug a cartridge in, I'm daft. So NO, we don't support the stupidity, we catch it"). Measured on Gideon's core the same day (EF3 in the port, §5.1): a socket at `$DE00`/`$DF40` and IO1/IO2 strobe in the same cycle, nobody wins. Same for a cartridge from a file (`.crt`, Alex: "that is basically the same!"): whenever the cartridge logic takes IO1/IO2 (`cart_present`), the socket gets no select there. The merged RTL still does the opposite (socket wins) and has to change.
5. Read priority when an internal SID and a socket both hit: socket (proposal) or UltiSID; M6 first. **Answered 2026-10-16 (Alex): the socket answers, as Gideon's core** (M6 read side, §5.1).
6. Pause with a real SID in the socket: park `PHI2` (proposal, consistent with T16) or keep it running?
   **Answered 2026-10-16 (Alex): park `PHI2`** (no real SID here for the 60 s test B6).
7. Should the reDIP build get address decode now (needed for sockets inside `$D400-$D7FF`), or do we
   require `TRX_SID=1` (T25.1 engine) when sockets are used? **Answered 2026-10-16 (Alex): the reDIP gets the address gate**, under the guardrail "physical chip *before* FPGA".

## 9. Build trial (branch `t37-sockets`, 2026-10-15)

Goal: RTL that implements §4 completely and passes lint and sim, so that one Vivado run shows whether it routes and
meets timing. Choices that stand on open questions (build trial uses the proposal, decision open): **Q2** Quiet gating
per cycle; **Q4** the socket wins over the cartridge's IO1/IO2 at `$DE00/$DF00` (the REU is left alone; changed 2026-10-16, see §8 Q4); **Q5** the socket
answers reads, writes go to both; **Q7** the reDIP (`SID_REDIP`) gets the address gate.

### 9.1 Comparison (read-compare-plan)

| Signal | Producer | Consumer (what it samples, when) | Needs | What the change gives |
|---|---|---|---|---|
| `sock_en/base/mask/stereo_sel` | `ultimate_sys` latches (regs 0x11/12, 08/09, 0C/0D, 14), cpu_clk | `c64_core` two-FF sync on `clk`, then arbiter decode and `slot_port` | quasi-static, app writes them with the C64 stopped (`detect()`); reset: EN 0, base 0x01 | decode and gating follow the app |
| `sock_sel[1:0]` (new) | arbiter: `io_enable && en && ((A11..A4 & mask) == base)`, from `bus_addr` | `slot_port` at core tick 1 (same instant as `io1_n` for `sel_now`), `c64_core` `port_rd` | stable from core tick 1, CPU half only | strobes, read data, write inhibit not applied |
| `cart_io1_cen/io2_cen` | arbiter | `cart_slot` (internal CRT) and `slot_port` (`io1_n/io2_n`) | unchanged; a socket select is dropped while one of them is low (Q4, 2026-10-16) | the cartridge keeps the cycle; `reu_cen` and `io_ext_sel` (UCI) are off in a socket cycle |
| read mux | arbiter | CPU / DMA read data | socket before `sid_hit` (Q5); `ext_cart_dout` = `s_dout` for a socket hit | real chip answers; internal SID still gets the write |
| `sid_hit` (`SID_REDIP`) | was `1'b1` | arbiter read mux | UltiSID compare as `trx_sid.v` (Q7) | internal SID stops answering outside its range |
| `p_aoen/p_ahi_t/p_rw/p_addr` | `slot_port` (`act`) | transceivers at the port | on for a socket cycle from port tick 2 to 18 even without a cartridge (Q2) | Quiet: idle bus stays released |
| `p_doen/p_ddir/p_d_t` | `slot_port` | data transceiver | read: DIR 0 from tick 2, OE 3-17; write: DIR 1, drive 3-18; no VIC-half byte (T74) without a cartridge | chip reads and writes |
| `SLOT_SID1n/2n` (new) | registered from `h_s[n]` (latched at tick 1) | the chip | low port tick 3-17, after the address | CS after the address is stable |
| `SLOT_SID_ADDR` | `slot_port` | the chip's pin | `addr[4+sel]` in the address window, idle 1 with a split, 0 without (M3) | A/B personality |
| `SLOT_PHI2/DOTCLK` | `slot_port` (`run`) | the chip | free-running while a socket is enabled, parked low in the pause | chip clock |
| guard `g_idle/g_turn/g_dout` | `slot_port` | `fault` latch | knows the socket cycle as a cause for active pins | no false fault, same checks |
| DMA read of `$D400-$D7FF` | `ultimate_sys` stub (`$FF`, "no SID") | the app's detection | a real bus cycle while a socket is enabled | `detectFPGASID` etc. reach the chip |
| `AUDIO_SDO` (AB3) | codec ADC | `i2s_out` receiver (mclk) | sample at the middle of BCLK; 24 bit, MSB one BCLK after the LRCK edge | `rx_l/rx_r` per frame |
| socket mix | `sock_mix` (mclk) | `snd_l/snd_r` after the pause gate, then I2S, HDMI and UDP | bytes 4-7 of the mixer (R, L per socket), 0x80 = 0 dB, saturate | sockets audible, freezer mutes through the bytes |

### 9.2 Implementation on the branch

| Block | Where | State |
|---|---|---|
| Registers, DMA read of `$D4xx` as a bus cycle | `ultimate_sys.vhd` | built, tested in `sim/dma_bus` |
| Decode, IO1/IO2 suppression, read mux, `sock_we` for a socket cycle (not `cart_we`: run 833) | `cores/icepi/c64_bus_arbiter.v`, `c64_top.v` (`ext_cart_sock`, `ext_cart_sock_we`) | built |
| reDIP address gate (`SID_REDIP` only) | `cores/icepi/c64_top.v` | built, no bench builds `SID_REDIP`: yosys parse only |
| Strobes, per-cycle gating, SID_ADDR, guard | `designs/c64_basic/slot_port.v` | built, `sim/slot` (tests `sock_*`) |
| Pins | `top.v` (`SLOT_SID1n/2n/ADDR` from `slot_port`) | built |
| `AUDIO_SDO` (AB3, LVCMOS18, false path), I2S receiver, mixer, sum into `snd_l/snd_r` | `pins.xdc`, `i2s_out.v` (`cnt`), `sock_audio.v`, `top.v` | built, `sim/tb_sock_audio.v` (iverilog, `ci/sims.txt` `sock_audio`) |

### 9.3 Simulation coverage of §6 (state of the branch)

`designs/c64_basic/sim/slot` (Verilator, c64_core with a behavioural SID on each socket: read data valid 7 ticks = 222 ns
after the address, FPGASID / ARMSID / SwinSID handshakes of `u64_config.cc`; checks for CS against PHI2, address and
R/W setup before CS and hold at PHI2's fall, no activity in the park state, no bus line without a socket cycle, K17,
two chips on D) and `sim/dma_bus` (registers). The T16 suite (14 tests) passes unmodified, with the same simulated times.

| §6 item | Test | Covered | Not covered |
|---|---|---|---|
| 1 decode | `sock_dec`, `sock_de00` | both sockets, mask `$F0` / `$FE`, split A5 / A8, unmapped, disabled, one socket only, `$DE00` / `$DF00`, both sockets on one address, I/O banked out, `$DE00` socket over an armed cart's IO1 | `$DF00` against a cart's IO2, a REU at `$DF00`, REU DMA into a socket |
| 2 SID model | `sock_rw`, `sock_split` | 25 registers of each chip written and read back with CPU cycles (IRQ program, `STA abs,X` dummy reads included) and with the app's DMA; D contention checker on | analog behaviour of a 6581 (the model is a register file), the real 200 ns latency (7 ticks = 222 ns) |
| 3 Quiet | `sock_idle`, every `sock_*` run, the T16 suite | no bus line or select outside a socket cycle (window of one tick around CS), PHI2 only with a socket or an armed cart, K17 idle level | the 16 other tests do not enable a socket |
| 4 SID_ADDR | `sock_split`, K17 check in every run | split A5, A7, A9 on both sockets, personalities A / B | A6 and A8 in the read-back test (A8 in the decode test only) |
| 5 priority, mirrored write | `sock_rw`, `sock_dec` | the socket answers where the testbench's internal SID (decodes everything) also would; a socket write is a write of the internal SID bus as well (`sid_we` with `sid_cen` low, counted) | the internal SID's received value, the reDIP gate (no bench builds `SID_REDIP`: yosys parse only) |
| 6 app flow | `sock_detect` | the byte sequences of `detectFPGASID` and `detectRemakes` (FPGASID, ARMSID, SwinSID), a plain chip, no chip (the read completes) | `detectDukestahAdapter`, PDsid, SIDKick, ARM2SID probes, `S_SidDetector` (spec: smoke run only, not done), the app firmware itself |
| 7 pause / resume | `sock_pause` | 40 pauses at random points of the IRQ program: no CS or PHI2 in the park state, no hold violation, results intact | a pause at every tick of a socket cycle; after a resume inside a cycle the select returns for the rest of the window (as the cartridge strobes do) |

Audio: `sim/tb_sock_audio.v` (i2s_out, sock_audio and a model of the codec's ADC: bit-exact mix of known samples for unity, half
and full gains with saturation, a socket not enabled, both off, gain 0; the swapped channel parameter; 175 frames). The channel
assignment (ADC left = socket 1, right = socket 2) and the sampling phase are decisions of the model, not measurements.

Not covered anywhere: the turbo CPU's posted SID writes (`c64_turbo.v`) skip the bus cycle, so a socket under an UltiSID
range sees no turbo-mode writes; Vivado, timing, the board.

### 9.4 Resource estimate of the trial (yosys `synth_xilinx`, per block, 2026-10-15; Vivado has not run)

| Block | LUT | FF | DSP | §7 estimate (LUT / FF) |
|---|---:|---:|---:|---|
| `slot_port.v` (selects, per-cycle gating, SID_ADDR, guard) | +18 | +8 | 0 | 30 / 10 |
| arbiter (decode, IO1/IO2 suppression, read mux, `sock_we`) | +38 | 0 | 0 | 25 / 4 |
| `c64_core.v` synchronizers (37 bits, two stages) | 0 | +74 | 0 | in the 100 FF of the latches |
| reDIP address gate (`SID_REDIP` builds only) | about 20 (not synthesized separately) | 64 | 0 | 20 / 0 |
| `ultimate_sys.vhd` latches and write decode (VHDL, not synthesized here) | about 30 | +69 | 0 | 25 / 100 |
| `sock_audio.v` (receiver, one multiplier, mixer) | 140 | 222 | 1 | 70 / 90 |
| sum with the SID mix and saturation in `top.v` (two 25-bit adders) | about 100 (estimate) | 0 | 0 | not in §7 |
| Total (default build, without the reDIP gate) | about 325 | about 375 | 1 | 170 / 200 |

The trial is about twice the §7 estimate, mostly the mixer and its saturating sum. Baseline 25,346 LUT (`docs/lut-budget.md`).

## Status and history

The TODO.md table is an overview only; the row detail lives here (convention at the top of TODO.md). Decisions of Alex that apply project-wide are in docs/adr/.

T37: Real SID chips in the C64U's SID sockets

Milestone M2, status 🔨. Open (one line, as in the TODO table): On main since 2026-10-18, all questions answered; not yet on the board; ADC slot per socket needs a real SID

Decisions: [ADR-0004 M1 release candidate for TREX-internal testers](../adr/0004-m1-release-candidate-internal-testers.md).

Moved from the TODO.md row on 2026-10-07, wording unchanged. Dated entries, newest first; undated text stays attached to the entry before it.

- **2026-10-15:** build trial branch `t37-sockets` (based on integ-m1 a122bed): RTL for §4 and the sim, to see whether Vivado routes and meets timing. Milestone stays M2. Not merged, not pushed by the builder. Measurements M3, M2 (partial), M4 (partial), M5 (partial) written into §5.1.
- **2026-10-15:** run 833 (#407): routed, WNS +0.007 ns, but DRC LUTLP-1 stopped bitgen: the trial set `cart_we` from the socket decode (`bus_addr`), and `cart_we` runs through the cartridge models and the REU arbitration back into `bus_addr`. The socket R/W is now its own signal (`sock_we`, ORed into the port's `we` in `c64_core.v` only); slot bench 21/21 pass.
- **2026-10-15:** run 834 (#408), 9a6e90f: whole pipeline green (lint, sims, app, 50T and 100T bitstream, package, update). 50T: WNS +0.027 ns; 26,830 LUT (82.3 %) against 26,584 on integ-m1 (package 78a5f4f9, same gateware as a122bed): +246 LUT, +384 FF, +2 DSP, BRAM unchanged (60.5). Not on the board yet.
- **2026-10-15:** board, run 834 build in SRAM (app 1791f3c), SIDKick Pico in socket 1 + its adapter in socket 2: detected, same address map as Gideon's app. Stream, 440 Hz at $D400 and $D420 (identical): both sockets 0 dB L 1187 / R 1191; socket 1 OFF L 889 / R 296; socket 2 OFF L 298 / R 895. The Pico drives the same signal on both socket audio pins, so the ADC slot per socket (SOCK_SWAP) cannot be measured with it (needs a real SID). Socket 1 alone (Pan "Left 3") is louder right: pan inverted in the mixer or L/R swapped in the stream; open, Alex checks by ear on HDMI (headphones), then the same on Gideon's core.
- **2026-10-15:** by ear (Alex): socket 1 alone, Pan "Left 3": left on the headphone jack and on HDMI, on this build and on Gideon's core alike. The pan is right. Only the UDP audio stream differs: Gideon's stream L 895 / R 268, ours L 298 / R 895, so our stream has L and R swapped (udp_audio.v, likely for every source, not T37). Alex: fine as is; no change made.

- **2026-10-08:** moved to M2 (Alex: "Genauso T37. nach M2."): [ADR-0047](../adr/0047-tape-and-user-port-to-m2.md).
- Task text: Not in the M1 release candidate (Alex, 2026-10-03: no SID chip here; he tries to get a Dual ARMSID; reDIP-FPGASID is sold out). Real SID chips in the C64U's SID sockets as Gideon's app offers them (SID Socket 1/2: enable, address, shunt, caps): drive SLOT_SID1n/2n and SID_ADDR, share A0-A15/D0-D7 with the port (pins in docs/specs/T16-cartridge-port.md) Spec: docs/specs/T37-sid-sockets.md (socket decode and strobes on slot_port.v, DMA detection, AUDIO_SDO mix; measurements M1-M8 on Gideon's core).
