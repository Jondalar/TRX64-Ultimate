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

    fprintf(stderr, "trx_flash_geom_test: %s (%d checks, %d failed)\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
