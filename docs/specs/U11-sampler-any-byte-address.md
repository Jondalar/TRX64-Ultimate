# U11 — A 16 bit sample starts at any byte address

**Status:** BUILT in the gateware and green in the benches (2026-11-09). No bitstream carries it yet, so the
board hear test `U11-HEAR` is open and nothing has been heard.
**Repo:** TRX64-Ultimate (gateware: `cores/trx_sid/rtl/trx_sampler.v`; bench:
`designs/c64_basic/sim/sampler/`).
**Number:** U11 (registry: the TODO table in `TODO.md`).
**Milestone:** the release U7 is in (C64 Ultimate 0.2.x).
**Origin:** issue [Jondalar/TRX64-Ultimate#2](https://github.com/Jondalar/TRX64-Ultimate/issues/2) by
`tbybell`, 2026-11-09, from code inspection; Alex, 2026-11-09: *"spec schreiben, bauen"*.

**Depends on:** [T25.6](T25.6-sampler.md) (the sampler itself). It **changes a decision of that spec**
— see D6.

---

## §1 The problem

T25.6 §3 says, of a 16 bit voice: *"addresses and lengths are bytes, the step is 2, bit 0 of
START/LENGTH/A/B is ignored for 16 bit"*. So this is not a slip in the gateware; the gateware does
what our own spec asked for. The issue says the reference implementation does something else: its
byte fetch state machine requests one byte at a time and never drops the low address bit, so an odd
START, and a sample crossing an aligned 32 bit word, are both supported there.

**What ours does, measured** (`designs/c64_basic/sim/sampler/`, the module driven through its
`$DF20` register window, the DDR2 port answered with `byte(n) = n[7:0]`, LENGTH 8, RATE 8):

| START | addresses the module asks for | sample it builds | the two bytes at that address |
|---|---|---|---|
| `0x001000` (even) | 1000, 1002, 1004, 1006 | 0100, 0302, 0504, 0706 | the same |
| `0x001001` (odd) | 1001, 1003, 1005, 1007 | 0100, 0302, 0504, 0706 | 0201, 0403, 0605, 0807 |

The address is right and the data is not: `trx_sampler.v` selected the 16 bit half with
`f_a10[1]` alone, so byte offsets 0 and 1 both gave the word's low half and 2 and 3 both the high
half. 8 bit is correct at either start — it selects with both low bits. A voice from an odd start
still stops at LENGTH (four samples, then `run` clears): `rel` counts from 0, so its parity does not
depend on START's.

**Who:** a player that lays 16 bit sample data out at an odd offset — which the two players we have
read do not do, which is why nothing was heard. The defect is silent: wrong bytes, no error.

**What this spec does not establish:** that the reference supports odd starts. That claim comes from
the issue's reading of `sampler2.vhd`, and that file is Gideon's; the clean room rule of this repo
forbids opening it, so it was not read here. What is measured above is ours alone.

**Parts (MECE):**

- **P1** Byte offset 1 selects the wrong pair although both its bytes are in the word that came back.
- **P2** Byte offset 3 has only its low byte in that word: the high byte is in the next one, and the
  module issues one read per sample.
- **P3** The LENGTH / REPEAT compare drops bit 0 in 16 bit mode, so an odd boundary value is treated
  as the even one below it (measured: LENGTH 9 stops after four samples, exactly as LENGTH 8).
- **P4** T25.6 writes the old rule down, and the manual of the feature is that spec: the documented
  rule has to move with the gateware or the next reader believes the wrong one.

## §2 Scenarios — what proves it is solved

```gherkin
Scenario: An odd start plays the bytes that are there
  Given a 16 bit voice with START on an odd byte address
  When  it plays
  Then  every sample is the two bytes at START + rel, low byte first
  And   the voice stops at LENGTH, as it does from an even start

Scenario: A sample across the word boundary
  Given a 16 bit sample whose low byte is the last byte of an aligned 32 bit word
  When  it is fetched
  Then  both bytes are read and the sample is published only once both are there
  And   no half-built sample reaches the output

Scenario: Nothing aligned pays for it
  Given a 16 bit voice with START on an even byte address
  When  it plays
  Then  it issues exactly one DDR2 read per sample, as before
  And   the sample period is the one RATE asks for

Scenario: A retrigger during the second read
  Given a voice whose split sample has its first half back and its second in flight
  When  the C64 stops or restarts that voice
  Then  neither half is written into the voice's sample words
```

| # | Solves | Scenario | Test | Where |
|---|---|---|---|---|
| S1 | P1 | offsets 0-2 give the bytes at the address | `tb_sampler` section 10 | host |
| S2 | P2 | offset 3 is read in two parts and published whole | `tb_sampler` section 10 | host |
| S3 | P2 | the split survives a random DDR2 latency and retriggers | `tb_stress` (starts on any byte) | host |
| S4 | — | **negative:** aligned play is bit identical to before, one read per sample | `tb_sampler` 1-8, `run_mix`, `spur.py`, `tb_sm_io` | host |
| S5 | P3 | an odd LENGTH keeps behaving as the even one below (D5) | `tb_sampler` section 10 | host |
| S6 | P4 | the Heartbeat and MOD songs sound as they did | `U11-HEAR` | board |

## §3 Decision

**D1 — A 16 bit sample is the two bytes at `START + rel` and `START + rel + 1`.** Low byte first
(little endian, as T25.6 §3 already says), whatever the parity of either. In the word that comes back
from DDR2 that is, by the two low bits of the address: `0 -> [15:0]`, `1 -> [23:8]`, `2 -> [31:16]`,
`3 -> the split of D2`. Offsets 0-2 keep costing one read.

**D2 — Offset 3 is two reads, published atomically.** The first read gives the low byte
(`f_data[31:24]`), which is held; a second read of the next aligned word gives the high byte
(`f_data[7:0]`). Nothing is written into the voice's sample words until both are in hand, so no
half-built sample is ever played. The consequence is stated rather than hidden: **a split sample
lands one C64 cycle later** than an aligned one, which is the same late landing T25.6 already
describes for a step that waits for the handler (weight 128, no blend). With an odd START every
second sample is a split one.

**D3 — The in-flight cancel covers both halves.** `fkill` already drops a landing sample when the
C64 stops or restarts the voice while a fetch is out; it now also drops the held low byte and the
second read, so a cancelled split cannot deliver a sample built from two different notes.

**D4 — The sample period does not change.** The second read is memory traffic, not a programmed
wait: the step counter, `ratee` and the handler's decision are untouched. What the extra read does
cost is throughput at the ceiling T25.6 names (about 0.9 M steps/s over all voices): a stream of
split samples asks for two reads per sample, so that ceiling halves for such a stream.

**D5 — The LENGTH / REPEAT compare keeps dropping bit 0.** `rel` starts at 0 and steps by 2 in 16
bit mode, so it is always even: an exact compare would make an odd LENGTH, REPEAT_A or REPEAT_B
never match, and the voice would run on to the 24 bit wrap instead of stopping or looping. The issue
asks for this to be "considered" and it is: dropping the bit is what makes an odd boundary usable at
all, and it is independent of D1 — the parity of `rel` never depended on START's. **This is the one
place where U11 does not follow the issue; it is written here to be overruled rather than argued.**

**D6 — T25.6's rule is replaced, not reinterpreted.** Its line *"bit 0 of START/LENGTH/A/B is ignored
for 16 bit"* becomes: **bit 0 of START counts** (D1); bit 0 of LENGTH, REPEAT_A and REPEAT_B stays
ignored (D5). The sentence is changed in T25.6 in the same commit, with a history entry, because
that spec is the manual of the register interface.

## §4 Out

- **Interleaved strides.** The issue reports that the reference changes the step when an interleave
  flag is set (8 bit +2, 16 bit +4). Our register map has no such bit — T25.6 §3 lists CONTROL b0
  enable, b1 repeat, b2 IRQ, b4 16 bit and nothing else, and the two players we read never write
  one. So this is not a defect of ours but a feature we do not have, and the only description of its
  encoding is in Gideon's VHDL, which this repo may not read. **Deliberately not built**; it needs a
  source that is not that file, and Alex's decision, before it can be.
- **A reference comparison per sample.** Clean room: no output of Gideon's sampler is used here. The
  bench compares against the bytes in memory, which is what the register interface promises.
- **The one-sample look-ahead** T25.6's history proposes for late landings. D2 makes one more case
  land late; the cure for all of them is that look-ahead, which is its own piece of work.

## §5 Decisions taken while building

**Two traps in the existing fetch machinery, found by building it.** Neither is visible from the extraction
line the issue points at:

1. The handler's test for "the fetch is back" is `!fbusy || fq`, and after the first read of a split both are
   true. A step queued for that voice would have run at `t = 49` and overwritten `f_addr` and `f_req_tgl` while
   the second read was still out - the sample would have come back from somewhere else entirely. The test is
   now `!fbusy || (fq && !f_first)`.
2. `fkill` dropped the landing sample but knew nothing of a held byte. A retrigger between the two reads would
   have published the new note's high byte over the old note's low byte. The cancel now clears `f_split`,
   `f_half` and the hold with it (D3).

**The bench model had the old rule in it too.** `tb_stress` masked the address bit (`ddr16(a)` did `a &= ~1`)
and expected every read to be the previous one plus 2, so it would have declared the correct new behaviour a
failure. It now reads the two bytes at the address, recognises the second read of a split by the module's own
`f_split`/`f_half`, and starts voices on any byte instead of forcing `& ~3`.

**Cost** (yosys generic synth of the module alone, the state RAM counted as FFs there, so only the delta
means anything): 9404 -> 9547 cells, +12 FF (`f_lo` 8, `f_split`, `f_half`, the carry of the +1). The figure
that matters is Vivado's in a CI run; `trx_sampler` was 639 LUT at the last one T25.6 records.

**Measured** (2026-11-09, host): `tb_sampler` 329 checks 0 failed with the change and **4 failed without it**
(offsets 1 and 3, both the values and the number of reads), the old bench 315/0 against the new gateware, so
nothing that was tested before moved. `tb_stress` seeds 1, 2, 4 at latency 8..80 with starts on any byte: about
205 000 fetches each, F 0, S 0 over ~205 000 sample checks, R 0, no stale bytes. `run_mix` PASS, `spur.py` the
same dB figures as before (-95.9 / -94.3 / -106.6 dB), `tb_sm_io` 95/0.
