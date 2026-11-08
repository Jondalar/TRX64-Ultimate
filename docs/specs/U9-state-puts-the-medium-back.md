# U9 — Loading a state puts the medium back in, and the machine it needs

**Status:** BUILT and running on the board (2026-11-08). `U9-COLD` passes: a cold machine with
an empty port loads a state, the cartridge comes back, the game runs on and its flash write-back
reaches the real `.crt`. The other board rows are open. D3b narrowed while building - §3, §5 Q5.
**Repo:** TRX64-Ultimate (app: `software/io/c64/`, `software/filetypes/`).
**Number:** U9 (registry: the TODO table in `TODO.md`).
**Milestone:** the release U8 is in (C64 Ultimate 0.2.x).
**Origin:** Alex, 2026-11-08, after the first load on the board worked:
> "Whats the point wenn das alles nicht passiert das CRT mit zu speichern. Es ist der State, also
> alle Chips, Ram, PC, Register, Medien, deren state soweit möglich."
> — "Ich will eine fertige Version, wir wollen live gehen."

**Depends on:** [U8](U8-c64re-v2-state-and-medium.md) (the format and the writer). Nothing in the
file format has to change for the bytes already written; the header gains fields.

---

## §1 The problem

U8 put the medium **into** the file. It does not put the medium **back**.

**Who:** Alex at the board, and after the release every user who saves a state and comes back to it
— on a machine that has been switched off in between, has another cartridge in, or none.

**What they are trying to do:** click a `.c64re`, and be where they were. Not: set the machine up
by hand first and then click it.

**What happens today**, measured on the board 2026-11-07 and 2026-11-08:

- The write is complete. `wasteland_ef_v1.0.1-20261107-200156.c64re`, 1 183 369 bytes, six
  sections, section 5 is the whole 1 MiB EasyFlash **as it stood at the freeze**.
- The load is exact **when the right cartridge is already in**. Alex, 2026-11-08: the intro came
  back clean; only the SID voices the program had not initialised yet were silent, and they came
  back within a second or two, which is the ring's documented limit (it carries no voice phase).
- `trx_c64re_apply()` writes RAM, colour RAM, cart RAM, cart ROM, the chip block, drive A and the
  turbo state. **Nothing else.**
- The cartridge is never inserted. `replay_cart()` (`trx_ring.cc:765`) reads
  `C64_CARTRIDGE_TYPE` **live out of the machine**, not out of the file, and replays $DE00/$DE02
  on top of whatever is there.
- Nothing refuses a state that does not fit. Format 1 checked the type
  (`trx_c64re.cc:1802`, *"The file is of cartridge type %02x; this machine has %02x"*);
  `stage_v2()` never reads `cartridgeType` out of its own header. **A regression U8 introduced.**
- No setting is recorded. UCI, REU, sampler and the drive enables are `cfg` values that
  `C64::set_emulation_flags()` (`c64.cc`) turns into `CMD_IF_SLOT_ENABLE`, `C64_REU_ENABLE`,
  `C64_SAMPLER_ENABLE`. A state carries none of them, so it lands in whatever machine is there.
- No mounted disk is recorded. Section 6 is the drive's RAM and registers, not its image.
  `c1541_A->get_last_mounted_file()` is already called — by `trx_c64re_place()`, to decide where
  the file goes — and then thrown away.

**Found while writing this spec, and it is a write-side gap, not a load-side one:** a **GMod2
cartridge's saved game is in no state file at all.** Its EEPROM (up to 2 KiB) lives in
`C64_CRT::eeprom_buffer` and reaches the hardware through `C64::set_eeprom_data()`
(`c64_crt.cc:364`); U8's section 5 reads `C64::get_cartridge_rom_addr()` and never sees it. On an
EasyFlash the saved game is in the flash and therefore in the file; on a GMod2 it is not. That is a
section of its own, on both sides.

**Why it matters:** a state that only loads onto a machine someone has already set up by hand is
half a feature, and it is the half that cannot go in a release.

**Parts (MECE):**

- **P1** The medium is not put back as a medium: the cartridge is not inserted, the disk is not
  mounted.
- **P2** Nothing refuses a state that does not fit the machine in front of it.
- **P3** The machine settings a state depends on are neither recorded nor restored.
- **P4** A load gives no account of what it could not put back.
- **P5** A cartridge whose state is not all in its ROM window is saved incomplete (GMod2's EEPROM).

## §2 Scenarios — what proves it is solved

```gherkin
Scenario: A state loads onto a machine that has nothing in it
  Given the C64 has been switched off and on, and no cartridge is inserted
  When  I click wasteland_ef_v1.0.1-<date>.c64re and choose Load State
  Then  the cartridge named in the file is inserted from its own path
  And   its flash holds what section 5 holds, not what the .crt on disk holds
  And   the machine runs on from the saved moment
  And   the machine was never reset: the state was not booted, it was put back

Scenario: The cartridge image is gone from the medium
  Given the .crt the state names has been deleted or the stick is not in
  When  I load the state
  Then  it is refused by name, before anything in the machine is touched
  And   the message says which file is missing
  And   the running machine is exactly as it was

Scenario: The machine has the wrong cartridge in
  Given a GMod2 cartridge is inserted and the state was taken on an EasyFlash
  When  I load the state
  Then  it is refused by name: "The file is of cartridge type 11; this machine has 48"
  And   nothing was written: the running cartridge is untouched

Scenario: A disk was in drive A
  Given the state was taken with /SD/C64/game.d64 in drive A
  When  I load it on a machine with an empty drive A
  Then  that image is mounted on drive A, write-protected as it was
  And   the drive's own state (section 6) goes in after the image, not before

Scenario: A setting differs
  Given UCI was off when the state was saved and is on now
  When  I load the state
  Then  UCI is off for as long as this state runs
  And   my own configuration in flash is unchanged

Scenario: A dump that could not be undumped is never written
  Given an Action Replay cartridge is running, whose banking registers
        replay_cart() does not put back
  When  I choose Save State
  Then  it is refused by name before anything is written
  And   no file is left behind
  And   the message says which cartridge and that it is the load that cannot do it

Scenario: A load says what it could not do
  Given a state whose disk image is missing
  When  I load it
  Then  the machine still comes back, and one line names what was not restored
  And   the console carries the detail
```

| # | Solves | Scenario | Test | Where |
|---|---|---|---|---|
| S1 | P1 | cold machine, cartridge inserted from the file | `U9-COLD` | board |
| S2 | P1/P2 | source `.crt` missing: refused by name, machine untouched | `U9-NOSRC` | board |
| S3 | P2 | wrong cartridge type refused by name | `U9-TYPE` | board |
| S4 | P1 | disk image mounted on drive A | `U9-DISK` | board |
| S5 | P3 | a setting differs and is put back | `U9-SET` | board |
| S6 | P4 | the report names what was not restored | `U9-REPORT` | board |
| S7 | P1-P3 | the header carries the machine, round trip | `tools/test_c64re_read.py` | host |
| S8 | P2 | a cartridge the load cannot put back is refused **at save time** | `U9-GATE` | board |
| S9 | — | **negative:** a state saved and loaded on an unchanged machine behaves exactly as it does today (U8-LOAD stays green) | `U8-LOAD`, `run-tests -s timewarp-files` | board + CI |

S7 is the host half: the reader is extended with the new header fields and the test bank builds
them from this spec. S1-S6 can only be answered by the board.

## §3 Decision

**D0 — What a dump holds, an undump puts back. Otherwise the dump is not offered.**

Alex, 2026-11-08: *"Alles was wir speichern im dump, muss auch ein undump können."*

This is the law the rest of the spec follows, and it reads in both directions:

- Every section and every header field the writer emits has a reader that puts it back.
- Where the reader cannot, **the writer does not write** — it refuses, by name, before the file
  exists. Not a half file, not a console line after the fact.
- The set of what can be put back is **one list, used by both sides**, in one place in the code.

It is a rule about the file's contents, not about perfection. What the hardware has no tap for is
never written and so never promised: the SID, the VIC's internals and write-only registers, the
REU (`trx_ring.cc:36`). Alex saw that on the board on 2026-11-08 and it is the expected shape —
the voices a program had not initialised were silent for a second until it wrote them again.

**Today this is broken, and U8 did not introduce it.** `replay_cart()` (`trx_ring.cc:761`) puts
the banking registers back for EasyFlash, MegaByter, Ocean, Ocean 16K, Magic Desk / Magic Desk 16
and Normal. For every other type it returns false, and the loader answers with one line —

```
trx_ring.cc:1040   printf("[RING] cartridge type %02x: banking registers not replayed\n", ...)
```

— and loads anyway. Action Replay, Simons' BASIC, Dinamic, C64GS, GMod2, C64MegaCart, Super Games,
Zaxxon and the rest come back on whatever bank happens to be selected, and `Save State` offers
itself for all of them. Under D0 that is a refusal at save time.

**D1 — The header carries the machine, not only provenance.** Format 2 stays; the header JSON
gains one object. The 41-byte head, the section table and every payload are unchanged, so files
written before this still load and `tools/c64re_read.py` keeps reading both.

```json
"machineState":{
  "cartridge":{"type":17,"source":"/SD/C64/wasteland_ef_v1.0.1.crt","bytes":1048576},
  "drive":{"image":"/SD/C64/game.d64","mode":"ro"},
  "settings":{"uci":false,"reu":0,"sampler":true}
}
```

`cartridgeType` and `source` move in here; the flat fields stay as they are for one release so
nothing that reads them breaks.

**D2 — Insert, do not boot.** The load inserts the cartridge the way the file browser does
(`filetype_crt.cc:70`):

```cpp
C64_CRT::load_crt(dir, name, &def, C64::get_cartridge_rom_addr());
C64::set_cartridge(&def);
```

**Never `C64::start_cartridge()`** — that one calls `hard_stop()`, `goUltimax()`, clears VIC
registers and ends with `C64_MODE = C64_MODE_UNRESET` (`c64.cc:1764`): it boots a cartridge and
destroys the state. `set_cartridge()` has no reset in it: it writes `C64_CARTRIDGE_TYPE`, calls
`trx_cart_configure()` and `set_emulation_flags()`, and applies the cart's require-flags.

**Order, and it matters:**

1. Refuse if this state cannot be put into this machine (D5).
2. Insert the cartridge → the `.crt` supplies the **definition** (type, EXROM/GAME, prohibit).
3. Mount the disks.
4. Apply the settings (D4).
5. Section 5 over the cartridge image → the **content** of the freeze replaces the `.crt`'s.
6. The chip block, RAM, colour RAM, cart RAM — today's `trx_ring_load_state()`, unchanged.
7. Drive state (section 6) **after** the mount, so the mount does not overwrite it.
8. Unfreeze.

**D3b — The cartridge comes from its own `.crt`, or the load is refused.**
*Changed while building, 2026-11-08; it was "the file is enough".* `C64_CRT` derives the image
geometry from the CRT's CHIP packets - `bank_multiplier`, `image_banks` and the mirror written
above the image by `auto_mirror()` (`c64_crt.cc:463-488`). Those do not come back from a hardware
type and a byte count: `get_image_bytes()` would read 0, and **the next `Save State` of that
machine would write a cartridge state with no medium** - the exact thing D0 forbids, one turn
later. So a state whose cartridge is not already in gets its `.crt` or nothing: `stage_v2()` opens
the path from the header before a byte moves, and refuses by name if it is not there. The bytes of
section 5 still win over the file's - the `.crt` supplies the geometry and the definition, the
section supplies the content of the freeze. **§5 Q5** is what it would take to lift this.

**D3 — One list of types, and it gates the save.** A single table says, per hardware cartridge
type, whether a state of it can be put back whole: the banking registers (`replay_cart()`), the
`require`/`prohibit` flags for a `cart_def` built without the `.crt`, and whether all of the
cartridge's own state is in the ROM window and the cart RAM.

- A type on the list: `Save State` writes, `Load State` puts it back, with or without the `.crt`.
- A type not on it: **`Save State` refuses by name** — *"A state of a &lt;name&gt; cartridge cannot be
  loaded back yet"* — and writes nothing.
- The list starts as the six `replay_cart()` already handles. Every type added to it is added on
  both sides at once, or not at all.

**D3a — GMod2 gets its EEPROM, and the list is those six plus GMod2.** Alex, 2026-11-08, Q2 = (b).

- **Section 7, `SEC_EEPROM`, 2048 bytes.** Written with `C64::get_eeprom_data()`, put back with
  `C64::set_eeprom_data()` (`c64.cc:2328`, `:2335`): the app's own port onto the 93C86's array,
  which `cart_slot.v` holds as a 2 KB block RAM with a second port and an `ee_tgl` toggle per
  change. Our core carries it — `CAPAB_EEPROM` is set, so `load_crt()` does not refuse a GMod2.
- **GMod2 joins `replay_cart()`.** Its `$DE00` is one register: `48_gmod2.json` gives bank in bits
  0-5 and mode in bits 6-7, and that byte is `W_CART`. One bus write restores bank and mode
  exactly, the same way Ocean's does. The standing comment — *"GMod2 ($DE00 also drives the
  EEPROM's lines)"* — is an **order**, not an obstacle: R0 first, then the EEPROM contents over the
  top, because `set_eeprom_data()` writes the array directly and owes nothing to the serial lines.
- **What is not carried, and therefore not promised:** the 93C86's own serial state machine, where
  in a command and which bit it stands. It is not on the state bus. A freeze taken in the middle of
  an EEPROM transaction comes back with the shift register out of step — one spoiled save, which
  the game repeats. Named here so no one reads D0 as covering it.

**D4 — Settings are an override, not a write.** Alex, 2026-11-08, Q1 = **(a)**: what the file says
holds for as long as the loaded state runs, and the user's configuration in flash is not touched.

The mechanism is **not** `cfg->set_override()`. That one is display only — `config.h:140`: *"while
set, the item shows this and the menu refuses edits; the saved value stays"* (T72). It changes no
value and no register.

What carries the setting is the hardware register, and `C64::set_emulation_flags()` (`c64.cc`) is
what writes it **from `cfg`**:

| Setting | Register | Written from |
|---|---|---|
| UCI | `CMD_IF_SLOT_ENABLE`, `CMD_IF_SLOT_BASE` | `CFG_CMD_ENABLE` |
| REU | `C64_REU_ENABLE`, `C64_REU_SIZE` | `CFG_C64_REU_EN`, `CFG_C64_REU_SIZE` |
| Sampler | `C64_SAMPLER_ENABLE` | `CFG_C64_MAP_SAMP` |

So the load writes those registers itself, **after** `set_cartridge()` — which calls
`set_emulation_flags()` and would otherwise stamp the user's `cfg` onto the hardware — and then
calls `cfg->set_override(id, "set by the loaded state")` on each one, which is exactly what T72
built that flag for: the menu shows who is in charge and refuses an edit that would not stick.
No `cfg` value is written, so nothing reaches the flash.

**The override lives as long as a cartridge would.** Alex, 2026-11-08: *"es sollte einen reset
überleben, weil das tun cartridges nun mal. power off/on dann nicht mehr."* The two menu entries
fall on either side of one function, so the rule needs one hook and no special cases:

| | path | `set_emulation_flags()` | the override |
|---|---|---|---|
| **Reset C64** | `c64->reset()` — `C64_MODE_RESET`, wait, `UNRESET` | not reached | **survives, by itself** |
| **Reboot C64** | `start_cartridge(NULL)` -> `set_cartridge(NULL)` (`c64_subsys.cc:629`) | reached (`c64.cc:1932`) | **ends** |
| a setting changed | `effectuate_settings()` (`c64.cc:397`) | reached | **ends** — the hardware really did go back to `cfg` |
| power off/on | `C64::init()` -> `init_cartridge()` | reached | **ends**; nothing persists |

`C64::reset()` touches `C64_MODE` and nothing else: not the cartridge type, not the image, not the
flag registers. So a reset keeps the loaded state's settings the way it keeps the cartridge in the
port, which is what Alex asked for and what the hardware already does.

**The hook is one line in `set_emulation_flags()`:** after it has written `cfg` onto the hardware,
clear every override text. Everything that puts `cfg` back goes through it, and nothing else does.

One edge, named because it is real: `MENU_C64_RESET` calls `start_cartridge(NULL)` instead of
`reset()` when a debug session may have left ROM banked out (`c64_subsys.cc:573`). That reset ends
the override. It is the rare path and it is honest — that reset really is a reboot.

**D5 — The refusal comes back, with both numbers.** The cartridge type is compared before anything
is written, as format 1 did: *"The file is of cartridge type 11; this machine has 48"*. A state
whose cartridge cannot be built is refused, not half-loaded. With D3 this is the second gate — the
first one already stopped the file being written.

**D6 — A load gives an account.** One popup line with what was not restored, the detail on the
console as `[C64RE]` lines, in the shape the coarse load already uses.

## §4 Out

Decided with Alex before the work — to be confirmed in refinement:

- **REU contents.** Timewarp is off while the REU is on, so no state can carry one, and the load
  already refuses while it is on (`filetype_c64re.cc:108`). Nothing to do.
- **Drive B and the 1581.** Not a decision: **this core has one drive.** One `drive_unit` in
  `ultimate_sys.vhd:1412`, and `c1541_B` exists only with `CAPAB_DRIVE_1541_2`, which c64_basic
  does not set. The header carries one `drive` object, not a list, and §2's S4 is the whole of the
  drive story.
- **Tape.**
- **SID addressing and audio routing.** They change what a program hears, not what the state is,
  and the file carries no SID state to go with them.
- **The trx64 side.** It follows, as in U8.
- **Rebuilding a cartridge the file has no bytes for.** A state without section 5 is a state of a
  machine without a cartridge.

## §5 Decisions taken while building

**Q1 — answered 2026-11-08, Alex: (a).** Override for as long as the state runs, flash untouched.
Folded into D4, with the correction that `cfg->set_override()` is display only and the registers
are written directly.

**Q2 — answered 2026-11-08, Alex: (b).** The list for going live is the six `replay_cart()` already
handles — EasyFlash, MegaByter, Ocean, Ocean 16K, Magic Desk / Magic Desk 16, Normal — **plus
GMod2**, which gains the EEPROM section and a `replay_cart()` case. Folded into D3a. Every other
type is refused at save time by D0/D3. Adding one later is one entry in one list, on both sides.

**Q3 — void, 2026-11-08.** It asked about drive B and the 1581. **This core has one drive.**
`designs/c64_basic/ultimate_sys.vhd:1412` instantiates a single `drive_unit` ("Drive A", header
comment line 62), and `c1541.cc:1261` creates `c1541_B` only under `CAPAB_DRIVE_1541_2`, which
c64_basic does not set. There is no 1581 either. Drive A is not a choice, it is the whole of it —
see §4.

**Inserting a cartridge needs a reset pulse, and only a pulse (2026-11-08, found on the board over
JTAG).** D2 said "insert, never boot", which was right and not enough. Two measured failures:

1. `set_cartridge()` left `C64_CARTRIDGE_ACTIVE` at 0 and the glue's cartridge line at 0, stable
   over eight reads of `tools/trxdbg.sh sb`, while the intro ran out of RAM and the first bank
   switch landed on nothing. `designs/c64_basic/cart_slot.v:149` is the rule:
   `cart_en <= reset_in && cart_type[4:0] != 5'd0 && desc[8*36]` - **`cart_en` can only go high
   while the C64 is in reset**, and the comment beside it says "a forced type starts off". Writing
   `C64_CARTRIDGE_KILL = 2` on a running machine therefore switches the cartridge *off*.
2. Holding the reset across the load instead answered `[C64RE] loading the snapshot failed: the
   state bus refused the staged words`. The bus takes nothing while the machine is in reset.

So the reset is a **pulse around the descriptor latch**, and what keeps the C64 from running
meanwhile is `hard_stop()`. `C64::insert_cartridge_for_state()` holds the machine, pulses
`C64_MODE_RESET`, inserts, forces twice, releases the reset and leaves the machine as held as it
found it. `cart_en` survives the release; the gateware clears it only on a kill or another force.
This is Alex's own order, given before the fix: *"Cartridge einlegen / reset aber nicht laufen
lassen damit es keinen CPU state gibt / State(s) herstellen / los lassen."*

**The machine's current state never decides anything (2026-11-08).** Alex: *"ich will den state
wiederherstellen, egal was gerade der zustand ist."* The cartridge is inserted even when the type
already matches, the disk is mounted even when the same image is already in, a state without a
cartridge empties the port, and the `.crt` is required whenever the state names one. The early
return that skipped the insert on a matching type is also why `U9-COLD` was the only test that
could have caught the defect above.

**A load has the save's progress box (2026-11-08).** Reading 1.18 MB off an SD card is a visible
wait; `trx_c64re_stage_ex()` counts the same steps the writer counts, so both bars move alike.

**Q5 — new, from building D3 (2026-11-08).** Loading a state onto a machine without its cartridge
needs the `.crt` (D3b). Lifting that means teaching `C64_CRT` to adopt an image that is already in
cart memory: `local_type` and `bank_multiplier` from the hardware type, `image_banks` from the byte
count, and the mirror re-run from the restored content rather than from the file's. That is a piece
of work inside the CRT class and it is **not built**. Until it is, S2 is a refusal, which is why
the scenario above reads the way it does. Worth noting the normal case is unaffected: the state
file is written beside the image it came from, so the two travel together.

**Q4 — answered 2026-11-08, Alex:** the override survives **Reset C64** and ends at **Reboot C64**
and at power off/on, *"weil das tun cartridges nun mal"*. It falls out of the code with one hook in
`set_emulation_flags()`; folded into D4 with the table of which path reaches it.

---

**All four questions are answered. The spec is ready for the build.**
