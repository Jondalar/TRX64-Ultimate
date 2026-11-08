// Host test of the flash geometry (trx_flash_geom.h): the EasyFlash 3's chip order -> image offset
// permutation, the MX29LV640EBT sector map (8 x 8 KiB boot sectors at the bottom, 127 x 64 KiB) and what a
// sector erase / chip erase wipes (a chip erase is the whole 8 MiB image), against trx_cart_desc.h (generated from tools/cart_descriptors/*.json).
#include <stdio.h>
#include <string.h>
#include <vector>
#include "trx_flash_geom.h"

static int checks, failures;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static const trx_flash_part_t *part_of(uint8_t cart_type)
{
    for (unsigned i = 0; i < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); i++) {
        if (c_cart_descs[i].cart_type == cart_type) {
            return c_cart_descs[i].flash;
        }
    }
    return NULL;
}

int main()
{
    const trx_flash_part_t *ef3 = part_of(0x12);
    CHECK(ef3 != NULL, "no flash part for CART_TYPE $12");
    if (!ef3) {
        return 1;
    }
    CHECK(!strcmp(ef3->part, "MX29LV640EBT") && ef3->chips == 1 && ef3->linear == 2, "part %s chips %d linear %d", ef3->part, ef3->chips, ef3->linear);
    CHECK(ef3->magic1 == 0x0AAA && ef3->magic2 == 0x0555 && ef3->mask == 0x0FFF, "unlock %03x/%03x mask %03x", ef3->magic1, ef3->magic2, ef3->mask);
    CHECK(ef3->id_mfr == 0xC2 && ef3->id_dev == 0xCB && ef3->id_ofs == 2, "ids %02x %02x at %d", ef3->id_mfr, ef3->id_dev, ef3->id_ofs);
    CHECK(ef3->chip_size == 0x800000, "chip size %x", ef3->chip_size);
    CHECK(ef3->runs == 2 && ef3->run[0].count == 8 && ef3->run[0].size == 0x2000 && ef3->run[1].count == 127 && ef3->run[1].size == 0x10000,
          "sector runs: bottom boot, 8 x 8K then 127 x 64K");

    // the chip address {slot, bank_hi, ROMH, bank_lo, A12-A0}: a worked example
    uint32_t chip = (3u << 20) | (5u << 17) | (1u << 16) | (2u << 13) | 0x123;
    uint32_t bank = 3 * 64 + 5 * 8 + 2;
    CHECK(trx_image_offset(ef3, chip, 0) == bank * 0x4000 + 0x2000 + 0x123, "image offset of slot 3 bank $2A ROMH: %x", trx_image_offset(ef3, chip, 0));
    chip &= ~(1u << 16);
    CHECK(trx_image_offset(ef3, chip, 0) == bank * 0x4000 + 0x123, "image offset of slot 3 bank $2A ROML: %x", trx_image_offset(ef3, chip, 0));

    // a bijection on the 8 MiB, byte offsets in a block kept
    std::vector<uint8_t> seen(0x800000 >> 13, 0);
    bool kept = true;
    for (uint32_t a = 0; a < 0x800000; a += 0x2000) {
        uint32_t off = trx_image_offset(ef3, a + 0x1FFF, 0);
        if ((off & 0x1FFF) != 0x1FFF) {
            kept = false;
        }
        seen[off >> 13]++;
    }
    int bad = 0;
    for (uint8_t c : seen) {
        bad += (c != 1);
    }
    CHECK(kept && !bad, "the permutation is not a bijection on the 8 KiB blocks (%d blocks hit 0 or 2 times)", bad);

    // sector map: boot sectors, then 64 KiB sectors
    uint32_t start, size;
    trx_sector_of(ef3, 0x0000, &start, &size); CHECK(start == 0 && size == 0x2000, "sector of 0: %x+%x", start, size);
    trx_sector_of(ef3, 0x6123, &start, &size); CHECK(start == 0x6000 && size == 0x2000, "sector of $6123: %x+%x", start, size);
    trx_sector_of(ef3, 0xFFFF, &start, &size); CHECK(start == 0xE000 && size == 0x2000, "sector of $FFFF: %x+%x", start, size);
    trx_sector_of(ef3, 0x10000, &start, &size); CHECK(start == 0x10000 && size == 0x10000, "sector of $10000: %x+%x", start, size);
    trx_sector_of(ef3, 0x7FFFFF, &start, &size); CHECK(start == 0x7F0000 && size == 0x10000, "sector of the last byte: %x+%x", start, size);
    int sectors = 0;
    uint32_t a = 0;
    while (a < 0x800000) {
        trx_sector_of(ef3, a, &start, &size);
        if (start != a) {
            break;
        }
        a += size;
        sectors++;
    }
    CHECK(a == 0x800000 && sectors == 135, "the map covers %x with %d sectors, want 8 MiB and 135", a, sectors);

    // a 64 KiB sector is ROML (or ROMH) of the eight banks of one bank_hi group: image blocks
    // {slot, bank_hi, lo = 0..7, half} with one slot, one bank_hi, one half
    bool shape = true;
    for (uint32_t s = 0x10000; s < 0x800000; s += 0x10000) {
        uint32_t first = trx_image_offset(ef3, s, 0) >> 13;
        for (uint32_t lo = 0; lo < 8; lo++) {
            uint32_t blk = trx_image_offset(ef3, s + lo * 0x2000, 0) >> 13;
            if ((blk >> 4) != (first >> 4) || (blk & 1) != (first & 1) || ((blk >> 1) & 7) != lo) {
                shape = false;
            }
        }
    }
    CHECK(shape, "a 64 KiB sector is not the eight ROML (or ROMH) halves of one bank_hi group");
    // the 8 boot sectors are ROML of slot 0 banks 0-7: the eight KERNALs
    bool boot = true;
    for (uint32_t n = 0; n < 8; n++) {
        if (trx_image_offset(ef3, n * 0x2000, 0) != n * 0x4000) {
            boot = false;
        }
    }
    CHECK(boot, "boot sector n is not ROML of slot 0 bank n");

    // the other parts keep their layouts
    const trx_flash_part_t *ef = part_of(0x11);
    CHECK(ef && trx_image_offset(ef, 0x12345, 1) == ((0x12345u >> 13) << 14) + 0x2000 + (0x12345 & 0x1FFF), "EasyFlash image offset");
    const trx_flash_part_t *tmb = part_of(0x2F);
    CHECK(tmb && trx_image_offset(tmb, 0x12345, 0) == 0x12345, "TwoMegabyter image offset");

    // what an erase wipes, applied to a filled 8 MiB image through the permutation as trx_cart.cc does
    {
        std::vector<uint8_t> img(0x800000, 0x5A);
        auto erase = [&](bool chip_erase, uint32_t a) {
            uint32_t st, sz;
            trx_erase_span(ef3, chip_erase, a, &st, &sz);
            for (uint32_t x = st; x < st + sz; x += 0x2000) {
                memset(&img[trx_image_offset(ef3, x, 0)], 0xFF, 0x2000);
            }
        };
        auto count = [&]() { size_t n = 0; for (uint8_t b : img) n += (b == 0xFF); return n; };
        erase(false, 0x123456);                      // one 64 KiB sector
        CHECK(count() == 0x10000, "sector erase wiped %zu bytes", count());
        erase(false, 0x3000);                        // one 8 KiB boot sector
        CHECK(count() == 0x12000, "boot sector erase wiped %zu bytes in all", count());
        erase(true, 0x3000);                         // chip erase: everything, the system area too
        CHECK(count() == 0x800000, "chip erase wiped %zu of 8 MiB", count());
        // and a program into the erased image only clears bits
        uint32_t off = trx_image_offset(ef3, (2u << 20) | 0x123, 0);
        img[off] &= 0x3C;
        CHECK(img[off] == 0x3C && img[off + 1] == 0xFF, "program after the chip erase");
    }
    {
        uint32_t st, sz;
        trx_erase_span(ef, true, 0, &st, &sz);
        CHECK(st == 0 && sz == ef->chip_size, "EasyFlash chip erase span %x+%x", st, sz);
        trx_erase_span(tmb, false, 0x30000, &st, &sz);
        CHECK(st == 0x30000 && sz == 0x10000, "TwoMegabyter sector span %x+%x", st, sz);
    }

    // T95 D1: the inverse of trx_image_offset(). The invariant over every part and every chip address:
    // the image offset of a chip address is the access's bank slot plus its window plus its offset.
    for (unsigned i = 0; i < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); i++) {
        const trx_cart_desc_t *d = &c_cart_descs[i];
        const trx_flash_part_t *p = d->flash;
        if (!p) {
            continue;
        }
        int bad = 0, n = 0;
        for (uint32_t a = 0; a < p->chip_size; a += 0x1A7) {       // a prime-ish stride: every window, every bank
            for (int chip = 0; chip < p->chips; chip++) {
                trx_chip_access_t acc = trx_chip_access(p, a, chip);
                uint32_t want = trx_image_offset(p, a, acc.window);
                uint32_t got = (uint32_t)acc.bank * 0x4000 + (uint32_t)acc.window * 0x2000 + acc.off;
                n++;
                if (want != got) {
                    bad++;
                    if (bad == 1) {
                        fprintf(stderr, "  %s a=%06X chip=%d -> bank %d win %d off %04X (image %06X != %06X)\n",
                                p->part, a, chip, acc.bank, acc.window, acc.off, want, got);
                    }
                }
            }
        }
        CHECK(bad == 0, "%s (%s): %d of %d chip addresses do not round-trip", d->name, p->part, bad, n);
    }

    // The flash command addresses of the profile reach exactly the CPU addresses the hand-written tool
    // used (apps/trex_crt/src/cart_writer.c:16-60 and gmod2_flash_cycle before T95), which is what makes
    // the six flash_chip_info_t structs removable.
    struct { uint8_t ct; uint32_t cmd; int chip; uint16_t cpu; uint16_t bank; const char *was; } expect[] = {
        { 0x11, 0x555,  0, 0x8555, 0, "easyflash_info.cmd_addr_1 0x8555" },
        { 0x11, 0x2AA,  0, 0x82AA, 0, "easyflash_info.cmd_addr_2 0x82AA" },
        { 0x11, 0x555,  1, 0xE555, 0, "easyflash_info.cmd_addr_h1 0xE555" },
        { 0x11, 0x2AA,  1, 0xE2AA, 0, "easyflash_info.cmd_addr_h2 0xE2AA" },
        { 0x0F, 0xAAA,  0, 0x8AAA, 0, "megabyter_info.cmd_addr_1 0x8AAA" },
        { 0x0F, 0x555,  0, 0x8555, 0, "megabyter_info.cmd_addr_2 0x8555" },
        { 0x12, 0xAAA,  0, 0x8AAA, 0, "ef3_info.cmd_addr_1 0x8AAA, slot 0 bank 0" },
        { 0x12, 0x555,  0, 0x8555, 0, "ef3_info.cmd_addr_2 0x8555" },
        { 0x31, 0x555,  0, 0x8555, 0, "EasyFlashXl ROML, as the EasyFlash" },
        { 0x31, 0x555,  1, 0xE555, 0, "EasyFlashXl ROMH" },
        { 0x48, 0x5555, 0, 0xF555, 2, "gmod2_flash_cycle(0x5555): bank 2, 0xE000|0x1555" },
        { 0x48, 0x2AAA, 0, 0xEAAA, 1, "gmod2_flash_cycle(0x2AAA): bank 1, 0xE000|0x0AAA" },
        { 0x68, 0xAAA,  0, 0xEAAA, 0, "megacart_info.cmd_addr_1 0xEAAA (ROMH_W, GAMEW)" },
        { 0x68, 0x555,  0, 0xE555, 0, "megacart_info.cmd_addr_2 0xE555" },
        { 0x2F, 0x555,  0, 0x8555, 0, "TwoMegabyter: $AA to $8555 (T33 D5, Protovision's API)" },
        { 0x2F, 0x2AA,  0, 0x82AA, 0, "TwoMegabyter: $55 to $82AA" },
    };
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        const trx_cart_desc_t *d = NULL;
        for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
            if (c_cart_descs[j].cart_type == expect[i].ct) {
                d = &c_cart_descs[j];
            }
        }
        CHECK(d && d->flash, "no flash part for CART_TYPE $%02X", expect[i].ct);
        if (!d || !d->flash) {
            continue;
        }
        trx_chip_access_t a = trx_flash_cmd(d, expect[i].cmd, expect[i].chip, true);
        CHECK(a.off == expect[i].cpu && a.bank == expect[i].bank,
              "%s: chip addr %04X chip %d -> bank %d $%04X, expected bank %d $%04X (%s)",
              d->name, expect[i].cmd, expect[i].chip, a.bank, a.off, expect[i].bank, expect[i].cpu, expect[i].was);
    }

    // rmode vs wmode where they differ: the TwoMegabyter reads its banks in 16 K (ROMH at $A000) and
    // programs them in ULTIMAX (ROMH at $E000); the C64MegaCart reads at $A000 and writes at $E000.
    {
        const trx_cart_desc_t *d = NULL;
        for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
            if (c_cart_descs[j].cart_type == 0x2F) {
                d = &c_cart_descs[j];
            }
        }
        CHECK(d != NULL, "no TwoMegabyter descriptor");
        if (d) {
            CHECK(d->flash->wmode == 1 && d->flash->rmode == 0, "TMB wmode %d rmode %d", d->flash->wmode, d->flash->rmode);
            CHECK(trx_window_cpu(d, d->flash->wmode, 1, true) == 0xE000, "TMB ROMH for programming");
            CHECK(trx_window_cpu(d, d->flash->rmode, 1, false) == 0xA000, "TMB ROMH for reading");
            CHECK(trx_window_cpu(d, d->flash->rmode, 0, false) == 0x8000, "TMB ROML");
        }
    }
    {
        const trx_cart_desc_t *d = NULL;
        for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
            if (c_cart_descs[j].cart_type == 0x68) {
                d = &c_cart_descs[j];
            }
        }
        if (d) {
            CHECK(trx_window_cpu(d, d->flash->wmode, 1, true) == 0xE000, "C64MegaCart ROMH in a write cycle");
            CHECK(trx_window_cpu(d, d->flash->wmode, 1, false) == 0xA000, "C64MegaCart ROMH in a read cycle");
        }
    }

    // The read direction: GMod2 and the C64MegaCart are programmed through ROMH and read through ROML, so
    // the same chip address answers at two CPU addresses (gmod2_flash_poll read $8000 | offset).
    {
        struct { uint8_t cart_type; uint32_t a; uint16_t rd; uint8_t bank; bool needs_bank; const char *was; } rd[] = {
            { 0x48, 0x5555, 0x9555, 2, true,  "gmod2_flash_poll: 8K read mode, $8000 | offset" },
            { 0x48, 0x0000, 0x8000, 0, true,  "gmod2_chip_erase: DQ7 polled at $8000" },
            { 0x68, 0x0AAA, 0x8AAA, 0, false, "C64MegaCart reads at $8000, writes at $E000" },
            { 0x0F, 0x0AAA, 0x8AAA, 0, false, "MegaByter reads and writes at $8000" },
            { 0x2F, 0x0555, 0x8555, 0, false, "TwoMegabyter reads at $8000 in 16K mode" },
        };
        for (unsigned i = 0; i < sizeof(rd) / sizeof(rd[0]); i++) {
            const trx_cart_desc_t *d = NULL;
            for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
                if (c_cart_descs[j].cart_type == rd[i].cart_type) {
                    d = &c_cart_descs[j];
                }
            }
            if (!d || !d->flash) {
                CHECK(false, "CART_TYPE $%02X has no flash descriptor", rd[i].cart_type);
                continue;
            }
            trx_chip_access_t a = trx_flash_cmd(d, rd[i].a, 0, false);
            CHECK(a.off == rd[i].rd, "%s: read $%04X, want $%04X", rd[i].was, a.off, rd[i].rd);
            CHECK(a.bank == rd[i].bank, "%s: bank %d, want %d", rd[i].was, a.bank, rd[i].bank);
            CHECK(trx_flash_cmd_needs_bank(d->flash) == rd[i].needs_bank, "%s: command cycle needs a bank: %d",
                  rd[i].was, trx_flash_cmd_needs_bank(d->flash));
        }
    }

    // The register side (T95 D1/D2): the addresses and values the tool used to carry as FLASH_MODE_* with
    // an ultimax_val and a normal_val per cartridge. Every row is what apps/trex_crt/src/cart_writer.c
    // held as a literal before the rebuild; nothing here is new knowledge, it all comes out of the rules
    // and the mode field of the descriptor plus flash.wmode / flash.rmode.
    {
        struct {
            uint8_t cart_type;
            uint16_t mode_addr, bank_addr;
            uint8_t wval, rval, bank_width;
            const char *was;
        } expect[] = {
            { 0x0F, 0xDE02, 0xDE00, 0x03, 0x00, 7, "megabyter_info: FLASH_MODE_DE02, $03, $00" },
            { 0x68, 0xDF00, 0xDE00, 0xC0, 0x00, 8, "megacart_info: FLASH_MODE_DF00, $C0, $00" },
            { 0x48, 0xDE00, 0xDE00, 0xC0, 0x00, 6, "gmod2_info: FLASH_MODE_DE00, $C0, $00" },
            { 0x11, 0xDE02, 0xDE00, 0x05, 0x05, 6, "easyflash_info: FLASH_MODE_DE02_EF, $05, $05" },
            { 0x12, 0xDE02, 0xDE00, 0x05, 0x05, 6, "ef3_info: FLASH_MODE_DE02_EF, $05, $05" },
            { 0x31, 0xDE02, 0xDE00, 0x05, 0x05, 8, "EasyFlashXl, as easyflash_info" },
            { 0x2F, 0xDE02, 0xDE00, 0x01, 0x00, 7, "TwoMegabyter: no code held this" },
        };
        for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
            const trx_cart_desc_t *d = NULL;
            for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
                if (c_cart_descs[j].cart_type == expect[i].cart_type) {
                    d = &c_cart_descs[j];
                }
            }
            CHECK(d != NULL && d->flash != NULL, "CART_TYPE $%02X has no flash descriptor", expect[i].cart_type);
            if (!d || !d->flash) {
                continue;
            }
            trx_reg_field_t m = trx_desc_mode(d), b = trx_desc_bank(d, 1);
            CHECK(m.addr == expect[i].mode_addr, "%s: mode register at $%04X, want $%04X", expect[i].was, m.addr, expect[i].mode_addr);
            CHECK(b.addr == expect[i].bank_addr, "%s: bank register at $%04X, want $%04X", expect[i].was, b.addr, expect[i].bank_addr);
            CHECK(b.width == expect[i].bank_width, "%s: bank field %d bits, want %d", expect[i].was, b.width, expect[i].bank_width);
            uint8_t wv = trx_desc_reg_value(d, m.reg, 0, d->flash->wmode);
            uint8_t rv = trx_desc_reg_value(d, m.reg, 0, d->flash->rmode);
            CHECK(wv == expect[i].wval, "%s: write mode value $%02X, want $%02X", expect[i].was, wv, expect[i].wval);
            CHECK(rv == expect[i].rval, "%s: read mode value $%02X, want $%02X", expect[i].was, rv, expect[i].rval);
        }
        // GMod2 carries bank and mode in one register, so selecting a bank in flash mode is one write:
        // $DE00 = $C0 | bank, which is what gmod2_flash_bank() did by hand.
        const trx_cart_desc_t *g = NULL;
        for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
            if (c_cart_descs[j].cart_type == 0x48) {
                g = &c_cart_descs[j];
            }
        }
        if (g) {
            trx_reg_field_t m = trx_desc_mode(g), b = trx_desc_bank(g, 1);
            CHECK(m.reg == b.reg && m.addr == b.addr, "GMod2: bank and mode in one register");
            CHECK(trx_desc_reg_value(g, 0, 2, g->flash->wmode) == 0xC2, "GMod2: $DE00 = $C2 for bank 2 in flash mode");
            CHECK(trx_desc_reg_value(g, 0, 2, 2) == 0x82, "GMod2: $DE00 = $82 for bank 2 in 8K read mode");
        }
        // EasyFlash 3's slot is the high segment of a two-register bank field, written at $DE01 (T51).
        const trx_cart_desc_t *e3 = NULL;
        for (unsigned j = 0; j < sizeof(c_cart_descs) / sizeof(c_cart_descs[0]); j++) {
            if (c_cart_descs[j].cart_type == 0x12) {
                e3 = &c_cart_descs[j];
            }
        }
        if (e3) {
            trx_reg_field_t hi = trx_desc_bank(e3, 0);
            CHECK(hi.width == 3 && hi.reg == 2 && hi.addr == 0xDE01, "EF3 slot: %d bits in R%d at $%04X", hi.width, hi.reg, hi.addr);
            CHECK(trx_desc_reg_value(e3, 2, 3 * 64 + 5, 0) == 3, "EF3: bank 197 selects slot 3 in R2");
            CHECK(trx_desc_reg_value(e3, 0, 3 * 64 + 5, 0) == 5, "EF3: bank 197 selects bank 5 in R0");
        }
    }

    fprintf(stderr, "trx_flash_geom_test: %s (%d checks, %d failed)\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
