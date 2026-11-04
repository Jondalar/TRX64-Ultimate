# U64 CRT Dumper

Dumps physical cartridges inserted in the U64 cart slot to VICE-compatible
.CRT files. All cart types are read via DMA — no 6502 code needed.

## Usage

1. Insert the physical cartridge into the U64 cart slot
2. Open the Ultimate menu, navigate to the desired save folder
3. Press **F5** (Task Menu) and select **Dump Cartridge to CRT**
4. The dumper auto-detects the cartridge type (~0.5 sec via DMA)
5. Confirm the detected type — or change it with **Up/Down**, then **RETURN**
6. Review the bank map, enter a filename, save

## MODE_NORMAL Fix

After freeze, the dumper sets `C64_MODE = MODE_NORMAL`. This releases the
cart's native GAME/EXROM pins from FPGA override, allowing correct detection
and reading of all cart types — including 16K carts where ROMH@$A000 requires
both GAME and EXROM low. Without this fix, the FPGA holds its own GAME/EXROM
state from before freeze, masking the cart's actual pin configuration.

## Detection Algorithm

Detection runs entirely via DMA. Tests are performed in this order:

1. **GAME/EXROM pins** — read `U64_CART_DETECT` to determine 8K/16K/Ultimax mode
2. **$DE00 banking** — write bank 1, compare $8000 with bank 0
3. **$DF00 RAM test** — write $A5 to $DF00, read back. Definitive for EasyFlash (RAM always visible, even in kill mode)
4. **EEPROM presence** — M93C86 serial read via $DE00. Definitive for GMod2 (replaces unreliable flash autoselect — some AM29F040B chips don't respond)
5. **Bank count** — compare bank 0 with banks 64, 128, 256, 512 to find wrap-around point
6. **$DE02=$03 Ultimax test** — write $03, check $E000 change. Identifies MegaByter (128+ banks)
7. **$DF00=$C0 Ultimax test** — write $C0, check $E000 change. Identifies C64MegaCart (256+ banks)
8. **$DF00 banking** — write 0/1 to $DF00, compare $8000. Identifies Super Games (16K mode)

### Decision Flow

```
1. Read GAME/EXROM → No cart (both high)? Stop.
2. Test $DE00 banking → NO? Normal cart (type 0). Stop.
3. Test $DF00 RAM → YES? EasyFlash (type 32). Stop.
4. Test EEPROM presence → YES? GMod2 (type 60). Stop.
5. Count banks (wrap-around at 64/128/256/512)
6. Banks ≥128 → Test $DE02=$03 → YES? MegaByter (type 86). Stop.
7. Banks ≥256 → Test $DF00=$C0 → YES? C64MegaCart (type 19). Stop.
8. Banks >256 → GMod3 (type 62). Stop.
9. 16K mode → Test $DF00 banking → YES? Super Games (type 8). Stop.
10. Fallback: Ocean / Magic Desk (type 5).
```

## Bank Map UI

After detection, a bank map is displayed as a 16-column hex grid showing
which banks contain data. Two separate arrays track status: `bank_status_lo`
(ROML) and `bank_status_hi` (ROMH).

- **Reverse space** = bank contains data
- **Dot (.)** = bank is empty ($FF)

For dual-chip carts (EasyFlash), two grids are shown labeled ROML and ROMH.

## Cart Writer

The dumper includes write support for reflashable cartridges:

| Cart | Flash Chip | Unlock Sequence |
|------|-----------|-----------------|
| MegaByter | AM29F040 | Standard $555/$2AA |
| C64MegaCart | AM29F040 | Standard $555/$2AA |
| GMod2 | AM29F040 | Standard $555/$2AA |
| EasyFlash ROML | AM29F040 | $8555/$82AA (lower 11 bits through banking) |
| EasyFlash ROMH | AM29F040 | $E555/$E2AA (lower 11 bits through banking) |

### EasyFlash Verify

Verification collects ROML+ROMH CHIP packets from the CRT into a 16K buffer
(matching the write logic's bank grouping), then verifies ROML@$8000 and
ROMH@$E000 in Ultimax mode ($DE02=$05).

## Supported Cart Types

| Type | CRT ID | Mode | Banking | Banks |
|------|--------|------|---------|-------|
| Normal 8K | 0 | 8K | None | 1 |
| Normal 16K | 0 | 16K | None | 1 |
| Normal Ultimax | 0 | Ultimax | None | 1 |
| Ocean type 1 / Magic Desk | 5 | 8K/16K | $DE00 data | up to 64 |
| Super Games | 8 | 16K | $DF00 data | 4 |
| C64MegaCart | 19 | 8K | $DE00 data, $DF00 Ultimax | up to 256 |
| EasyFlash | 32 | Ultimax | $DE00 bank + $DE02 mode | 64 |
| GMod2 | 60 | 8K | $DE00 bits 5:0 | 64 |
| GMod3 | 62 | 8K | $DE00 data | 256-2048 |
| MegaByter | 86 | 8K | $DE00 bank + $DE02 Ultimax | 128 |

## CRT File Format

Standard VICE CRT files:
- **64-byte header**: Signature, version 1.0, hardware type, EXROM/GAME state
- **CHIP packets**: 16-byte header + 8K ROM data per bank

For 16K and EasyFlash carts, each bank produces two 8K CHIP packets
(ROML at $8000 + ROMH at $A000).

## Source Files

| File | Description |
|------|-------------|
| `software/io/c64/cart_dumper.h` | CartDumper class, ProbeResult struct |
| `software/io/c64/cart_dumper.cc` | Detection, bank map, CRT generation |
| `software/io/c64/cart_base.h` | CartBase class (shared DMA/detection primitives) |
| `software/io/c64/cart_base.cc` | Bus setup, bank scanning, EEPROM detection |
| `software/io/c64/cart_writer.h` | CartWriter class |
| `software/io/c64/cart_writer.cc` | Flash write/erase/verify for all writable carts |
| `software/io/c64/c64_subsys.cc` | Menu integration (MENU_C64_DUMP_CRT) |
