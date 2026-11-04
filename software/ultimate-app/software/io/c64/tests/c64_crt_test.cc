// Host test for the CRT loader (C64_CRT::load_crt).
//
// For every CRT type the firmware accepts, it checks which cartridge logic and
// I/O restrictions the loader selects, where each chip lands in cartridge
// memory, and how the image is repeated over the whole cartridge region. The
// FPGA reads that memory directly, so a change here changes what every
// cartridge of the affected types sees. The CRT images are built in code.
//
// Results go to stderr; the loader's own log goes to stdout.

#include "c64_crt.h"

uint32_t host_fpga_capabilities = CAPAB_EEPROM;
uint32_t host_cart_max_rom = 0;
uint8_t host_eeprom[2048];
bool host_eeprom_dirty = false;
bool host_ef3_present = false; // trx_cart.h's stub with TRX_HOST_EF3: the bitstream has the EasyFlash 3 block

static const uint32_t K = 1024;
static const uint32_t MB = 1024 * K;
static const uint32_t GUARD = 64 * K;
static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

struct Chip {
    uint16_t bank;
    uint16_t load;
    uint16_t type;
    std::vector<uint8_t> data;
};

// Every chip gets its own bytes, so a misplaced or missing chip is visible.
static std::vector<uint8_t> payload(uint32_t seed, size_t size)
{
    std::vector<uint8_t> d(size);
    uint32_t x = seed * 2654435761u + 1;
    for (auto &b : d) {
        x = x * 1103515245u + 12345u;
        b = uint8_t(x >> 16);
    }
    return d;
}

struct Crt {
    int machine = 64;
    uint16_t hw = 0;
    uint8_t exrom = 0;
    uint8_t game = 0;
    uint8_t subtype = 0;
    std::vector<Chip> chips;
    std::vector<uint8_t> trailer;

    Crt(uint16_t hw_type, int for_machine = 64) : machine(for_machine), hw(hw_type) {}

    Crt &chip(uint16_t bank, uint16_t load, uint16_t size, uint16_t type = 0) {
        chips.push_back({ bank, load, type, payload(uint32_t(chips.size()) * 131 + bank * 7 + load + hw * 1009, size) });
        return *this;
    }
    Crt &banks(int n, uint16_t load, uint16_t size) {
        for (int b = 0; b < n; b++) {
            chip(b, load, size);
        }
        return *this;
    }
    Crt &header(uint8_t e, uint8_t g, uint8_t s) {
        exrom = e;
        game = g;
        subtype = s;
        return *this;
    }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out(0x40, 0);
        memcpy(out.data(), machine == 128 ? "C128 CARTRIDGE  " : "C64 CARTRIDGE   ", 16);
        out[0x13] = 0x40;
        out[0x14] = 1;
        out[0x16] = hw >> 8;
        out[0x17] = hw & 0xFF;
        out[0x18] = exrom;
        out[0x19] = game;
        out[0x1A] = subtype;
        for (const Chip &c : chips) {
            uint32_t len = 0x10 + c.data.size();
            const uint8_t h[16] = { 'C', 'H', 'I', 'P', uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len),
                                    uint8_t(c.type >> 8), uint8_t(c.type), uint8_t(c.bank >> 8), uint8_t(c.bank),
                                    uint8_t(c.load >> 8), uint8_t(c.load), uint8_t(c.data.size() >> 8), uint8_t(c.data.size()) };
            out.insert(out.end(), h, h + 16);
            out.insert(out.end(), c.data.begin(), c.data.end());
        }
        out.insert(out.end(), trailer.begin(), trailer.end());
        return out;
    }
};

struct Loaded {
    SubsysResultCode_e rc;
    uint16_t type;
    uint16_t prohibit;
    uint16_t require;
    std::vector<uint8_t> mem;   // cartridge region followed by a guard band
    bool guard_intact;

    const uint8_t *at(uint32_t offset) const { return mem.data() + offset; }
};

static Loaded load(const Crt &crt, uint32_t max_rom)
{
    FileManager::getFileManager()->files["test.crt"] = crt.bytes();
    host_cart_max_rom = max_rom;
    Loaded r;
    r.mem.assign(max_rom + GUARD, 0x5A);
    cart_def def;
    r.rc = C64_CRT::load_crt("/", "test.crt", &def, r.mem.data());
    r.type = def.type;
    r.prohibit = def.prohibit;
    r.require = def.require;
    r.guard_intact = std::all_of(r.mem.begin() + max_rom, r.mem.end(), [](uint8_t b) { return b == 0x5A; });
    return r;
}

static bool all_ff(const uint8_t *p, uint32_t n)
{
    return std::all_of(p, p + n, [](uint8_t b) { return b == 0xFF; });
}

// A 256K Ocean image as VICE's cartconv writes it: banks 0 to 15 at $8000 and
// banks 16 to 31 at $A000. The chips at $A000 need 16K mode.
static Crt ocean_256k(void)
{
    Crt crt(5);
    for (int b = 0; b < 32; b++) {
        crt.chip(b, b < 16 ? 0x8000 : 0xA000, 0x2000);
    }
    return crt;
}

// The cartridge logic and I/O restrictions each CRT type selects.
static void test_cartridge_types(void)
{
    struct Row {
        const char *what;
        Crt crt;
        uint16_t type;
        uint16_t prohibit;
        uint16_t require;
    };
    const int SMALL = 4;    // 32K of 8K chips
    const int LARGE = 16;   // 128K of 8K chips, above the 64K variant threshold
    Row rows[] = {
        { "normal 16K",              Crt(0).header(0, 0, 0).banks(1, 0x8000, 0x4000),     CART_TYPE_16K,             0, 0 },
        { "normal 8K",               Crt(0).header(0, 1, 0).banks(1, 0x8000, 0x2000),     CART_TYPE_8K,              0, 0 },
        { "normal Ultimax",          Crt(0).header(1, 0, 0).chip(0, 0xE000, 0x2000),      CART_TYPE_UMAX,            0, 0 },
        { "normal, both lines high", Crt(0).header(1, 1, 0).banks(1, 0x8000, 0x2000),     CART_TYPE_NONE,            0, 0 },
        { "Action Replay",           Crt(1).banks(SMALL, 0x8000, 0x2000),                 CART_TYPE_ACTION,          CART_PROHIBIT_IO, 0 },
        { "KCS Power Cartridge",     Crt(2).banks(2, 0x8000, 0x2000),                     CART_TYPE_KCS,             CART_PROHIBIT_IO, 0 },
        { "Final Cartridge III",     Crt(3).banks(SMALL, 0x8000, 0x4000),                 CART_TYPE_FC3,             CART_PROHIBIT_IO, 0 },
        { "Final Cartridge III, subtype 1", Crt(3).header(0, 0, 1).banks(SMALL, 0x8000, 0x4000), CART_TYPE_FC3,     CART_PROHIBIT_ALL_BUT_REU, 0 },
        { "Final Cartridge III+",    Crt(3).banks(LARGE, 0x8000, 0x4000),                 CART_TYPE_FC3PLUS,         CART_PROHIBIT_IO, 0 },
        { "Simons Basic",            Crt(4).banks(2, 0x8000, 0x2000),                     CART_TYPE_SBASIC,          CART_PROHIBIT_DEXX, 0 },
        { "Ocean, chips at $8000",   Crt(5).banks(LARGE, 0x8000, 0x2000),                 CART_TYPE_OCEAN_8K,        CART_PROHIBIT_DEXX, 0 },
        { "Ocean, 256K with chips at $A000", ocean_256k(),                                CART_TYPE_OCEAN_16K | VARIANT_3, CART_PROHIBIT_DEXX, 0 },
        { "Super Games",             Crt(8).banks(SMALL, 0x8000, 0x4000),                 CART_TYPE_SUPERGAMES,      CART_PROHIBIT_DFXX, 0 },
        { "Atomic Power",            Crt(9).banks(SMALL, 0x8000, 0x2000),                 CART_TYPE_NORDIC,          CART_PROHIBIT_IO, CART_WMIRROR | CART_DYNAMIC },
        { "Epyx FastLoad",           Crt(10).banks(1, 0x8000, 0x2000),                    CART_TYPE_EPYX,            CART_PROHIBIT_IO, 0 },
        { "Westermann",              Crt(11).banks(1, 0x8000, 0x4000),                    CART_TYPE_WESTERMANN,      CART_PROHIBIT_IO, 0 },
        { "Final Cartridge I",       Crt(13).banks(1, 0x8000, 0x4000),                    CART_TYPE_FINAL12,         CART_PROHIBIT_IO, 0 },
        { "C64 Game System",         Crt(15).banks(64, 0x8000, 0x2000),                   CART_TYPE_SYSTEM3,         CART_PROHIBIT_DEXX, 0 },
        { "Zaxxon",                  Crt(18).chip(0, 0x8000, 0x1000).chip(0, 0xA000, 0x2000).chip(1, 0xA000, 0x2000), CART_TYPE_ZAXXON, 0, 0 },
        { "Magic Desk",              Crt(19).banks(LARGE, 0x8000, 0x2000),                CART_TYPE_DOMARK,          CART_PROHIBIT_DEXX, 0 },
        { "Super Snapshot 5",        Crt(20).banks(SMALL, 0x8000, 0x4000),                CART_TYPE_SS5,             CART_PROHIBIT_DEXX, 0 },
        { "Super Snapshot 5, 128K",  Crt(20).banks(8, 0x8000, 0x4000),                    CART_TYPE_SS5 | VARIANT_1, CART_PROHIBIT_DEXX, 0 },
        { "COMAL 80",                Crt(21).banks(SMALL, 0x8000, 0x4000),                CART_TYPE_OCEAN_16K,       CART_PROHIBIT_DEXX, 0 },
        { "COMAL 80, 128K",          Crt(21).banks(8, 0x8000, 0x4000),                    CART_TYPE_OCEAN_16K | VARIANT_1, CART_PROHIBIT_DEXX, 0 },
        { "COMAL 80 grey, subtype 1", Crt(21).header(0, 0, 1).banks(SMALL, 0x8000, 0x4000), CART_TYPE_OCEAN_16K | VARIANT_2, CART_PROHIBIT_DEXX, 0 },
        { "COMAL 80 grey, 128K",     Crt(21).header(0, 0, 1).banks(8, 0x8000, 0x4000),    CART_TYPE_OCEAN_16K | VARIANT_2, CART_PROHIBIT_DEXX, 0 },
        { "EasyFlash",               Crt(32).header(1, 0, 0).banks(SMALL, 0x8000, 0x2000), CART_TYPE_EASY_FLASH,     CART_PROHIBIT_IO, CART_UCI_DE1C },
        { "EasyFlash, subtype 1",    Crt(32).header(1, 0, 1).banks(SMALL, 0x8000, 0x2000), CART_TYPE_EASY_FLASH,     CART_PROHIBIT_ALL_BUT_REU, CART_UCI_DE1C },
        { "Retro Replay",            Crt(36).banks(8, 0x8000, 0x2000),                    CART_TYPE_RETRO,           CART_PROHIBIT_DEXX, 0 },
        { "EXOS",                    Crt(44).header(1, 0, 0).chip(0, 0xE000, 0x2000),     CART_TYPE_NONE,            0, CART_KERNAL },
        { "Pagefox",                 Crt(53).banks(SMALL, 0x8000, 0x4000),                CART_TYPE_PAGEFOX,         0, 0 },
        { "Kingsoft Business Basic", Crt(54).banks(3, 0x8000, 0x2000),                    CART_TYPE_BBASIC,          CART_PROHIBIT_DEXX, 0 },
        { "GMod2",                   Crt(60).banks(SMALL, 0x8000, 0x2000),                CART_TYPE_GMOD2,           CART_PROHIBIT_DEXX, 0 },
        { "Blackbox V8",             Crt(64).banks(SMALL, 0x8000, 0x4000),                CART_TYPE_BLACKBOX_V8,     CART_PROHIBIT_DFXX, 0 },
        { "Blackbox V3",             Crt(65).banks(1, 0x8000, 0x2000),                    CART_TYPE_BLACKBOX_V3,     CART_PROHIBIT_IO, 0 },
        { "Blackbox V4",             Crt(66).banks(1, 0x8000, 0x4000),                    CART_TYPE_BLACKBOX_V4,     CART_PROHIBIT_IO, 0 },
        { "Blackbox V9",             Crt(71).banks(2, 0x8000, 0x4000),                    CART_TYPE_BLACKBOX_V9,     CART_PROHIBIT_DEXX, 0 },
        { "Megabyter",               Crt(86).banks(LARGE, 0x8000, 0x2000),                CART_TYPE_MEGABYTER,       CART_PROHIBIT_DEXX, 0 },
        { "C64MegaCart, Ocean 8K here", Crt(88).banks(LARGE, 0x8000, 0x2000),             CART_TYPE_OCEAN_8K,        CART_PROHIBIT_DEXX, 0 },
        { "TwoMegabyter",            Crt(89).banks(LARGE, 0x8000, 0x4000),                CART_TYPE_MEGABYTER | VARIANT_1, CART_PROHIBIT_DEXX, 0 },
        { "C128",                    Crt(0, 128).banks(1, 0x8000, 0x8000),                CART_TYPE_128,             0, 0 },
        { "C128 with I/O",           Crt(1, 128).banks(1, 0x8000, 0x8000),                CART_TYPE_128 | VARIANT_3, CART_PROHIBIT_IO, 0 },
        { "C128 with I/O, subtype 1", Crt(1, 128).header(0, 0, 1).banks(1, 0x8000, 0x8000), CART_TYPE_128 | VARIANT_3, CART_PROHIBIT_ALL_BUT_REU, CART_UCI },
        { "C128 with I/O, subtype 2", Crt(1, 128).header(0, 0, 2).banks(4, 0x8000, 0x8000), CART_TYPE_128 | VARIANT_7, CART_PROHIBIT_ALL_BUT_REU_AND_ACIA_DE, CART_UCI },
    };
    for (const Row &row : rows) {
        Loaded r = load(row.crt, 4 * MB);
        CHECK(r.rc == SSRET_OK, "%s: load returned %d", row.what, r.rc);
        CHECK(r.type == row.type, "%s: cartridge type $%02X, want $%02X", row.what, r.type, row.type);
        CHECK(r.prohibit == row.prohibit, "%s: prohibit $%03X, want $%03X", row.what, r.prohibit, row.prohibit);
        CHECK(r.require == row.require, "%s: require $%03X, want $%03X", row.what, r.require, row.require);
    }
}

// CRT types the firmware does not emulate, and files that are not CRT images.
static void test_refused(void)
{
    const uint16_t accepted[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 13, 15, 18, 19, 20, 21, 32, 36, 44, 53, 54, 60, 64, 65, 66, 71, 86, 88, 89 };
    for (uint16_t hw = 0; hw < 100; hw++) {
        if (std::find(std::begin(accepted), std::end(accepted), hw) != std::end(accepted)) {
            continue;
        }
        Loaded r = load(Crt(hw).banks(1, 0x8000, 0x2000), 4 * MB);
        CHECK(r.rc == SSRET_NOT_IMPLEMENTED, "CRT type %d: load returned %d, want not implemented", hw, r.rc);
    }
    Loaded r = load(Crt(232).banks(1, 0x8000, 0x2000), 4 * MB); // EasyFlashXl: TRX64 bitstream only
    CHECK(r.rc == SSRET_NOT_IMPLEMENTED, "CRT type 232: load returned %d, want not implemented", r.rc);
    r = load(Crt(2, 128).banks(1, 0x8000, 0x8000), 4 * MB);
    CHECK(r.rc == SSRET_NOT_IMPLEMENTED, "C128 CRT type 2: load returned %d, want not implemented", r.rc);

    host_fpga_capabilities = 0;
    r = load(Crt(60).banks(4, 0x8000, 0x2000), 4 * MB);
    CHECK(r.rc == SSRET_NOT_IMPLEMENTED, "GMod2 without EEPROM support: load returned %d, want not implemented", r.rc);
    host_fpga_capabilities = CAPAB_EEPROM;

    Crt bad(0);
    bad.banks(1, 0x8000, 0x2000);
    std::vector<uint8_t> bytes = bad.bytes();
    memcpy(bytes.data(), "C65 CARTRIDGE   ", 16);
    FileManager::getFileManager()->files["bad.crt"] = bytes;
    std::vector<uint8_t> mem(MB);
    cart_def def;
    host_cart_max_rom = MB;
    CHECK(C64_CRT::load_crt("/", "bad.crt", &def, mem.data()) == SSRET_ERROR_IN_FILE_FORMAT, "unknown signature is not refused");
}

// Packet-level results: a bank beyond the cartridge region, a first packet
// that is not CHIP, an oversized EEPROM chunk.
static void test_packets(void)
{
    Loaded r = load(Crt(32).header(1, 0, 0).banks(1, 0x8000, 0x2000).chip(64, 0x8000, 0x2000), MB);
    CHECK(r.rc == SSRET_ROM_IMAGE_TOO_LARGE, "bank 64 with 1 MB: load returned %d, want image too large", r.rc);
    r = load(Crt(32).header(1, 0, 0).banks(1, 0x8000, 0x2000).chip(255, 0x8000, 0x2000), 4 * MB);
    CHECK(r.rc == SSRET_OK, "bank 255 with 4 MB: load returned %d", r.rc);

    Crt no_chip(0);
    no_chip.trailer.assign(96, 0x1A);
    r = load(no_chip, MB);
    CHECK(r.rc == SSRET_ERROR_IN_FILE_FORMAT, "first packet not CHIP: load returned %d, want format error", r.rc);

    r = load(Crt(60).banks(1, 0x8000, 0x2000).chip(0, 0xDE00, 0x1000), MB);
    CHECK(r.rc == SSRET_EEPROM_TOO_LARGE, "4K EEPROM chunk: load returned %d, want EEPROM too large", r.rc);
}

// A chip at bank B and load address L lands at B * 16K + (L & $2000), or at
// B * 32K for C128 images. Chips shorter than 8K repeat to 8K.
static void test_chip_placement(void)
{
    const uint16_t types[] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 13, 15, 19, 20, 21, 32, 36, 44, 53, 60, 64, 65, 66, 71, 86, 87 };
    for (uint16_t hw : types) {
        Crt crt(hw);
        for (int b = 0; b < 4; b++) {
            crt.chip(b, 0x8000, 0x2000).chip(b, 0xA000, 0x2000);
        }
        Loaded r = load(crt, MB);
        CHECK(r.rc == SSRET_OK, "CRT type %d: load returned %d", hw, r.rc);
        for (const Chip &c : crt.chips) {
            uint32_t offset = c.bank * 16 * K + (c.load & 0x2000);
            CHECK(!memcmp(r.at(offset), c.data.data(), c.data.size()), "CRT type %d: chip of bank %d at $%04X is not at offset $%05X", hw, c.bank, c.load, offset);
        }
    }

    Crt c128(1, 128);
    c128.banks(3, 0x8000, 0x8000);
    Loaded r = load(c128, MB);
    for (const Chip &c : c128.chips) {
        CHECK(!memcmp(r.at(c.bank * 32 * K), c.data.data(), c.data.size()), "C128: chip of bank %d is not at offset $%05X", c.bank, c.bank * 32 * K);
    }

    Crt small(0);
    small.header(0, 1, 0).chip(0, 0x8000, 0x1000);
    r = load(small, MB);
    const std::vector<uint8_t> &half = small.chips[0].data;
    CHECK(!memcmp(r.at(0), half.data(), 0x1000) && !memcmp(r.at(0x1000), half.data(), 0x1000), "a 4K chip does not repeat to 8K");

    // Zaxxon: the 4K chip at $8000 serves both $A000 banks, so it is copied to bank 1.
    Crt zaxxon(18);
    zaxxon.chip(0, 0x8000, 0x1000).chip(0, 0xA000, 0x2000).chip(1, 0xA000, 0x2000);
    r = load(zaxxon, MB);
    CHECK(!memcmp(r.at(0x4000), r.at(0), 0x2000), "Zaxxon: bank 1 at $8000 is not a copy of bank 0");
    CHECK(!memcmp(r.at(0x2000), zaxxon.chips[1].data.data(), 0x2000) && !memcmp(r.at(0x6000), zaxxon.chips[2].data.data(), 0x2000),
          "Zaxxon: the $A000 chips are not in place");

    // Kingsoft Business Basic lists three 8K chips at $8000; the loader moves
    // banks 1 and 2 to the offsets the cartridge logic reads.
    Crt bbasic(54);
    bbasic.banks(3, 0x8000, 0x2000);
    r = load(bbasic, MB);
    CHECK(!memcmp(r.at(0x0000), bbasic.chips[0].data.data(), 0x2000) && !memcmp(r.at(0x2000), bbasic.chips[1].data.data(), 0x2000) &&
          !memcmp(r.at(0x6000), bbasic.chips[2].data.data(), 0x2000), "Business Basic: chips are not at $0000, $2000 and $6000");
}

// The image repeats over the whole cartridge region: its size is rounded up to
// a power of two banks, and that block is copied until the region is full.
// A bank register wider than the image then reads the image, as on hardware
// that does not decode the upper bits: an EasyFlash writing $78 to $DE00 reads
// bank $38. A flash cartridge has its whole chip, so for an EasyFlash the block
// is at least its 64 banks, and the banks the file leaves out read $FF.
static void test_mirroring(void)
{
    for (uint32_t max_rom : { MB, 4 * MB }) {
        for (int n : { 1, 3, 4, 5, 64, 100, 255 }) {
            uint32_t slots = max_rom / (16 * K);
            if ((uint32_t)n > slots) {
                continue;
            }
            Crt crt(32);
            crt.header(1, 0, 0);
            for (int b = 0; b < n; b++) {
                crt.chip(b, 0x8000, 0x2000).chip(b, 0xA000, 0x2000);
            }
            Loaded r = load(crt, max_rom);
            CHECK(r.rc == SSRET_OK, "EasyFlash, %d banks, %d MB: load returned %d", n, max_rom / MB, r.rc);
            CHECK(r.guard_intact, "EasyFlash, %d banks, %d MB: wrote past the cartridge region", n, max_rom / MB);
            uint32_t block = 64;
            while (block < (uint32_t)n) {
                block <<= 1;
            }
            int wrong = 0;
            uint32_t first_wrong = 0;
            for (uint32_t slot = 0; slot < slots; slot++) {
                uint32_t bank = slot % block;
                bool ok = (bank < (uint32_t)n)
                    ? !memcmp(r.at(slot * 16 * K), crt.chips[2 * bank].data.data(), 0x2000) &&
                      !memcmp(r.at(slot * 16 * K + 0x2000), crt.chips[2 * bank + 1].data.data(), 0x2000)
                    : all_ff(r.at(slot * 16 * K), 16 * K);
                if (!ok && !wrong++) {
                    first_wrong = slot;
                }
            }
            CHECK(!wrong, "EasyFlash, %d banks, %d MB: %d of %d banks do not repeat the image, first bank %d",
                  n, max_rom / MB, wrong, slots, first_wrong);
        }
    }

    // A ROM cartridge's image repeats from the next power of two: three Magic Desk
    // banks fill four, and banks 4 to 6 read banks 0 to 2 again.
    Crt desk(19);
    desk.banks(3, 0x8000, 0x2000);
    Loaded d = load(desk, MB);
    CHECK(d.rc == SSRET_OK, "Magic Desk: load returned %d", d.rc);
    CHECK(all_ff(d.at(3 * 16 * K), 0x2000), "Magic Desk: bank 3 is not $FF");
    for (int b = 0; b < 3; b++) {
        CHECK(!memcmp(d.at((4 + b) * 16 * K), desk.chips[b].data.data(), 0x2000), "Magic Desk: bank %d does not repeat bank %d", 4 + b, b);
    }

    // C128 images use 32K banks and are not repeated.
    Crt c128(0, 128);
    c128.banks(3, 0x8000, 0x8000);
    Loaded r = load(c128, 4 * MB);
    CHECK(all_ff(r.at(3 * 32 * K), 4 * MB - 3 * 32 * K), "C128: memory after the image is not $FF");
}

// The chip packet for one bank and load address in a saved CRT file.
static const uint8_t *saved_chip(const std::vector<uint8_t> &file, uint16_t bank, uint16_t load, uint16_t *size_out)
{
    size_t pos = 0x40;
    while (pos + 16 <= file.size()) {
        const uint8_t *h = file.data() + pos;
        if (memcmp(h, "CHIP", 4)) {
            break;
        }
        uint32_t packet = (uint32_t(h[4]) << 24) | (uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7];
        if ((packet < 16) || (pos + packet > file.size())) {
            break;
        }
        if ((((h[10] << 8) | h[11]) == bank) && (((h[12] << 8) | h[13]) == load)) {
            *size_out = (h[14] << 8) | h[15];
            return h + 16;
        }
        pos += packet;
    }
    return NULL;
}

// Where a cartridge came from, so that a changed image can be written back to
// the file it was loaded from.
static void test_source(void)
{
    Crt crt(32);
    crt.header(1, 0, 0).chip(0, 0x8000, 0x2000).chip(0, 0xA000, 0x2000);
    load(crt, MB);
    CHECK(!strcmp(C64_CRT::get_source(), "/test.crt"), "source after loading is '%s'", C64_CRT::get_source());

    // REST passes the whole pathname as the name, with an empty path.
    FileManager::getFileManager()->files["/Usb0/game.crt"] = crt.bytes();
    cart_def def;
    std::vector<uint8_t> mem(MB, 0xFF);
    host_cart_max_rom = MB;
    C64_CRT::load_crt("", "/Usb0/game.crt", &def, mem.data());
    CHECK(!strcmp(C64_CRT::get_source(), "/Usb0/game.crt"), "source from an empty path is '%s'", C64_CRT::get_source());

    C64_CRT::set_source("/Usb0", "saved.crt");
    CHECK(!strcmp(C64_CRT::get_source(), "/Usb0/saved.crt"), "source after saving as is '%s'", C64_CRT::get_source());
    C64_CRT::set_source("/Usb0/", "saved.crt");
    CHECK(!strcmp(C64_CRT::get_source(), "/Usb0/saved.crt"), "a path ending in a slash gives '%s'", C64_CRT::get_source());

    // A file that cannot be opened leaves the cartridge that is loaded alone,
    // source included: it is still the one the C64 is running.
    C64_CRT::load_crt("/", "absent.crt", &def, mem.data());
    CHECK(!strcmp(C64_CRT::get_source(), "/Usb0/saved.crt"),
          "a load that could not open its file moved the source to '%s'", C64_CRT::get_source());

    // A file that is not a cartridge drops the image, and with it the source.
    FileManager::getFileManager()->files["junk.crt"] = std::vector<uint8_t>(0x80, 0x11);
    C64_CRT::load_crt("/", "junk.crt", &def, mem.data());
    CHECK(!*C64_CRT::get_source(), "a refused file left the source at '%s'", C64_CRT::get_source());
    CHECK(!C64_CRT::is_valid(), "a refused file left a cartridge loaded");
}

// The hash that tells a cartridge the C64 wrote to from one it did not.
static void test_change_detection(void)
{
    Crt crt(32);
    crt.header(1, 0, 0);
    for (int b = 0; b < 4; b++) {
        crt.chip(b, 0x8000, 0x2000).chip(b, 0xA000, 0x2000);
    }
    Loaded r = load(crt, 4 * MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash: load returned %d", r.rc);

    const uint32_t base = C64_CRT::get_baseline();
    CHECK(C64_CRT::current_hash() == base, "an untouched image does not hash to its baseline");

    // One flipped bit anywhere in the 64 banks an EasyFlash describes is seen.
    int missed = 0;
    uint32_t first_missed = 0;
    for (uint32_t offset : { 0u, 0x1FFFu, 0x2000u, 0x3800u, 16 * K, 63 * 16 * K, 64 * 16 * K - 1 }) {
        uint8_t *p = (uint8_t *)r.at(offset);
        *p ^= 0x01;
        if (C64_CRT::current_hash() == base) {
            if (!missed++) {
                first_missed = offset;
            }
        }
        *p ^= 0x01;
    }
    CHECK(!missed, "%d single-bit changes went unnoticed, first at offset %6x", missed, first_missed);
    CHECK(C64_CRT::current_hash() == base, "undoing the change did not restore the hash");

    // Above those banks the image is only mirrored, and not part of the file.
    uint8_t *beyond = (uint8_t *)r.at(64 * 16 * K);
    *beyond ^= 0xFF;
    CHECK(C64_CRT::current_hash() == base, "a change above the described banks changed the hash");
    *beyond ^= 0xFF;
}

// Saving must not disturb the cartridge the C64 is running, and the cartridge's own EAPI
// is neither replaced on load nor on save.
static void test_save_keeps_memory(void)
{
    Crt crt(32);
    crt.header(1, 0, 0);
    crt.chip(0, 0x8000, 0x2000).chip(0, 0xA000, 0x2000).chip(1, 0x8000, 0x2000).chip(1, 0xA000, 0x2000);

    // The EAPI of the cartridge sits at $B800, which is bank 0 at $A000 plus $1800.
    std::vector<uint8_t> &romh = crt.chips[1].data;
    const uint8_t signature[4] = { 'e', 'a', 'p', 'i' };
    memcpy(&romh[0x1800], signature, 4);
    for (int i = 4; i < 768; i++) {
        romh[0x1800 + i] = uint8_t(0xC0 + (i & 0x1F));
    }
    std::vector<uint8_t> original_eapi(romh.begin() + 0x1800, romh.begin() + 0x1800 + 768);

    Loaded r = load(crt, MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash with EAPI: load returned %d", r.rc);
    CHECK(!memcmp(r.at(0x3800), original_eapi.data(), 768), "loading changed the cartridge's own EAPI in memory");

    std::vector<uint8_t> file;
    File out(&file);
    SubsysResultCode_e rc = C64_CRT::save_crt(&out);
    CHECK(rc == SSRET_OK, "save returned %d", rc);
    CHECK(!memcmp(r.at(0x3800), original_eapi.data(), 768), "saving changed the EAPI in cartridge memory");

    uint16_t size = 0;
    const uint8_t *chip = saved_chip(file, 0, 0xA000, &size);
    CHECK(chip && (size == 0x2000), "the saved file has no bank 0 chip at $A000");
    if (chip && (size == 0x2000)) {
        CHECK(!memcmp(chip + 0x1800, original_eapi.data(), 768), "the saved file does not carry the cartridge's own EAPI");
        CHECK(!memcmp(chip, r.at(0x2000), 0x1800), "the bytes before the EAPI are not the ones in memory");
    }

    // After a save the file and the image agree again.
    CHECK(C64_CRT::get_baseline() == C64_CRT::current_hash(), "the baseline was not renewed by the save");

    uint8_t *p = (uint8_t *)r.at(0x1000);
    *p ^= 0x55;
    CHECK(C64_CRT::current_hash() != C64_CRT::get_baseline(), "a write after the save is not seen");
    *p ^= 0x55;
}

// The banks of a saved CRT file, packet by packet: bank and load address, and the end of the
// packets, which must be the end of the file.
struct SavedPacket { uint16_t type, bank, load, size; };
static std::vector<SavedPacket> saved_packets(const std::vector<uint8_t> &file, bool *clean_end)
{
    std::vector<SavedPacket> list;
    size_t pos = 0x40;
    while (pos + 16 <= file.size()) {
        const uint8_t *h = file.data() + pos;
        uint32_t packet = (uint32_t(h[4]) << 24) | (uint32_t(h[5]) << 16) | (uint32_t(h[6]) << 8) | h[7];
        if (memcmp(h, "CHIP", 4) || (packet < 16) || (pos + packet > file.size())) {
            break;
        }
        list.push_back({ uint16_t((h[8] << 8) | h[9]), uint16_t((h[10] << 8) | h[11]), uint16_t((h[12] << 8) | h[13]), uint16_t((h[14] << 8) | h[15]) });
        pos += packet;
    }
    *clean_end = (pos == file.size());
    return list;
}

// A flash cartridge is saved as the whole chip the C64 sees: every bank in bank order, erased
// ones included, and nothing of the mirrors above the chip.
static void test_save_whole_chip(void)
{
    Crt crt(32);
    crt.header(1, 0, 0);
    for (int b = 0; b < 4; b++) {
        crt.chip(b, 0x8000, 0x2000, 2).chip(b, 0xA000, 0x2000, 2);
    }
    Loaded r = load(crt, 4 * MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash: load returned %d", r.rc);
    CHECK(all_ff(r.at(4 * 16 * K), 60 * 16 * K), "EasyFlash: the banks the file leaves out are not $FF");

    // A bank the C64 programs, which the file did not have.
    uint8_t *p = (uint8_t *)r.at(40 * 16 * K + 0x2000 + 0x100);
    *p = 0x42;
    std::vector<uint8_t> file;
    File out(&file);
    SubsysResultCode_e rc = C64_CRT::save_crt(&out);
    CHECK(rc == SSRET_OK, "save returned %d", rc);
    CHECK(file.size() == 0x40 + 128 * (0x10 + 0x2000), "EasyFlash with 4 banks saved as %d bytes, want 64 banks of ROML and ROMH", (int)file.size());
    bool clean = false;
    std::vector<SavedPacket> list = saved_packets(file, &clean);
    int out_of_order = 0;
    for (size_t n = 0; n < list.size(); n++) {
        if ((list[n].bank != n / 2) || (list[n].load != ((n & 1) ? 0xA000 : 0x8000)) || (list[n].size != 0x2000) || (list[n].type != 2)) {
            out_of_order++;
        }
    }
    CHECK(clean && (list.size() == 128) && !out_of_order, "EasyFlash: %d packets, %d not in bank order", (int)list.size(), out_of_order);
    uint16_t size = 0;
    const uint8_t *chip = saved_chip(file, 3, 0xA000, &size);
    CHECK(chip && !memcmp(chip, crt.chips[7].data.data(), 0x2000), "EasyFlash: bank 3 at $A000 is not the file's");
    chip = saved_chip(file, 40, 0xA000, &size);
    CHECK(chip && (size == 0x2000) && (chip[0x100] == 0x42) && all_ff(chip, 0x100), "the programmed bank 40 at $A000 is not in the file");
    chip = saved_chip(file, 40, 0x8000, &size);
    CHECK(chip && all_ff(chip, 0x2000), "the erased bank 40 at $8000 is not in the file as $FF");

    // Reloading the saved file gives the same image.
    std::vector<uint8_t> image(r.mem.begin(), r.mem.begin() + 64 * 16 * K);
    FileManager::getFileManager()->files["test.crt"] = file;
    host_cart_max_rom = 4 * MB;
    std::vector<uint8_t> mem2(4 * MB);
    cart_def def;
    rc = C64_CRT::load_crt("/", "test.crt", &def, mem2.data());
    CHECK(rc == SSRET_OK, "the saved EasyFlash file does not load (%d)", rc);
    CHECK(!memcmp(mem2.data(), image.data(), image.size()), "the saved EasyFlash file does not give the same image");

    // GMod2: 64 banks of 8K at $8000, then its EEPROM.
    Crt gmod(60);
    gmod.banks(8, 0x8000, 0x2000);
    r = load(gmod, 4 * MB);
    CHECK(r.rc == SSRET_OK, "GMod2: load returned %d", r.rc);
    p = (uint8_t *)r.at(50 * 16 * K + 0x10);
    *p = 0x24;
    file.clear();
    File out2(&file);
    rc = C64_CRT::save_crt(&out2);
    CHECK(rc == SSRET_OK, "GMod2 save returned %d", rc);
    CHECK(file.size() == 0x40 + 64 * (0x10 + 0x2000) + 0x10 + 0x800, "GMod2 with 8 banks saved as %d bytes, want 64 banks and the EEPROM", (int)file.size());
    list = saved_packets(file, &clean);
    out_of_order = 0;
    for (size_t n = 0; (n < list.size()) && (n < 64); n++) {
        if ((list[n].bank != n) || (list[n].load != 0x8000) || (list[n].size != 0x2000)) {
            out_of_order++;
        }
    }
    CHECK(clean && (list.size() == 65) && !out_of_order && (list.back().load == 0xDE00) && (list.back().size == 0x800),
          "GMod2: %d packets, %d not in bank order, the EEPROM not last", (int)list.size(), out_of_order);
    chip = saved_chip(file, 50, 0x8000, &size);
    CHECK(chip && (chip[0x10] == 0x24), "GMod2: the programmed bank 50 is not in the file");

    // Megabyter: 128 banks of 8K.
    r = load(Crt(86).banks(16, 0x8000, 0x2000), 4 * MB);
    CHECK(r.rc == SSRET_OK, "Megabyter: load returned %d", r.rc);
    file.clear();
    File out3(&file);
    rc = C64_CRT::save_crt(&out3);
    CHECK(rc == SSRET_OK, "Megabyter save returned %d", rc);
    CHECK(file.size() == 0x40 + 128 * (0x10 + 0x2000), "Megabyter with 16 banks saved as %d bytes, want 128 banks", (int)file.size());
    list = saved_packets(file, &clean);
    CHECK(clean && (list.size() == 128) && (list.back().bank == 127), "Megabyter: %d packets, last bank %d", (int)list.size(), list.empty() ? -1 : list.back().bank);

    // A ROM cartridge keeps the packets of its file.
    r = load(Crt(19).banks(3, 0x8000, 0x2000), 4 * MB);
    file.clear();
    File out4(&file);
    rc = C64_CRT::save_crt(&out4);
    CHECK(file.size() == 0x40 + 3 * (0x10 + 0x2000), "Magic Desk with 3 banks saved as %d bytes", (int)file.size());
}

// EasyFlash 3 (T51): CRT type 90, 512 banks slot-major (bank field = slot * 64 + bank), 8 MiB window; the
// image is loaded as is (no EAPI patch), kept whole, and saved as 1024 packets in bank order.
static void test_easyflash3(void)
{
    Crt sparse(90);
    sparse.header(1, 0, 0);
    const int banks[] = { 0, 1, 197, 511 };  // slot 0 bank 0 and 1, slot 3 bank 5, slot 7 bank 63
    for (int b : banks) {
        sparse.chip(b, 0x8000, 0x2000).chip(b, 0xA000, 0x2000);
    }
    // an EAPI signature at slot 0 bank 0 ROMH $1800: the TRX bitstream answers the flash itself, nothing is patched
    sparse.chips[1].data[0x1800] = 0x65; sparse.chips[1].data[0x1801] = 0x61; sparse.chips[1].data[0x1802] = 0x70; sparse.chips[1].data[0x1803] = 0x69;
    std::vector<uint8_t> eapi_before(sparse.chips[1].data.begin() + 0x1800, sparse.chips[1].data.begin() + 0x1800 + 768);

    host_ef3_present = false;
    Loaded r = load(sparse, 8 * MB);
    CHECK(r.rc == SSRET_NOT_IMPLEMENTED, "EasyFlash 3 without the bitstream's block: load returned %d, want not implemented", r.rc);

    host_ef3_present = true;
    r = load(sparse, 8 * MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash 3: load returned %d", r.rc);
    CHECK(r.type == 0x12, "EasyFlash 3: CART_TYPE %02x, want 12", r.type);
    CHECK(r.prohibit == CART_PROHIBIT_IO, "EasyFlash 3: prohibit %04x", r.prohibit);
    CHECK(r.require == CART_UCI_DE1C, "EasyFlash 3: require %04x", r.require);
    CHECK(r.guard_intact, "EasyFlash 3: the guard past 8 MiB was written");
    for (size_t i = 0; i < sparse.chips.size(); i++) {
        const Chip &c = sparse.chips[i];
        const uint8_t *at = r.at(uint32_t(c.bank) * 16 * K + (c.load == 0xA000 ? 0x2000 : 0));
        CHECK(!memcmp(at, c.data.data(), 0x2000), "EasyFlash 3: bank %d at $%04X is not at bank * 16K (+8K)", c.bank, c.load);
    }
    CHECK(!memcmp(r.at(0x2000 + 0x1800), eapi_before.data(), 768), "EasyFlash 3: the EAPI of the image was changed");
    CHECK(all_ff(r.at(2 * 16 * K), (197 - 2) * 16 * K), "EasyFlash 3: banks 2-196 are not erased flash ($FF)");
    CHECK(all_ff(r.at(198 * 16 * K), (511 - 198) * 16 * K), "EasyFlash 3: banks 198-510 are not erased flash ($FF)");

    // a read-only load keeps the 4 MiB window for every other type
    // (the window clamp needs ROM_SEL; the host has none, the loader's reach is the test's max_rom)

    // the C64 programs a byte in slot 5 (bank field 5 * 64 + 17) and erases slot 6's first ROML chip
    uint32_t prog = (5 * 64 + 17) * 16 * K + 0x2000 + 0x123;
    *(uint8_t *)r.at(prog) = 0x42;
    memset((void *)r.at(6 * 64 * 16 * K), 0xFF, 0x2000);
    std::vector<uint8_t> file;
    File out(&file);
    SubsysResultCode_e rc = C64_CRT::save_crt(&out);
    CHECK(rc == SSRET_OK, "EasyFlash 3: save returned %d", rc);
    CHECK(file.size() == 0x40 + 1024 * (0x10 + 0x2000), "EasyFlash 3 saved as %d bytes, want 512 banks of ROML and ROMH", (int)file.size());
    bool clean = false;
    std::vector<SavedPacket> list = saved_packets(file, &clean);
    int out_of_order = 0;
    for (size_t n = 0; n < list.size(); n++) {
        if ((list[n].bank != n / 2) || (list[n].load != ((n & 1) ? 0xA000 : 0x8000)) || (list[n].size != 0x2000)) {
            out_of_order++;
        }
    }
    CHECK(clean && (list.size() == 1024) && !out_of_order, "EasyFlash 3: %d packets, %d not slot-major in bank order", (int)list.size(), out_of_order);
    CHECK(!memcmp(file.data(), sparse.bytes().data(), 0x40), "EasyFlash 3: the header changed in the save");
    uint16_t size = 0;
    const uint8_t *chip = saved_chip(file, 5 * 64 + 17, 0xA000, &size);
    CHECK(chip && size == 0x2000 && chip[0x123] == 0x42, "EasyFlash 3: the programmed byte of slot 5 is not in the file");
    chip = saved_chip(file, 511, 0xA000, &size);
    CHECK(chip && !memcmp(chip, sparse.chips[7].data.data(), 0x2000), "EasyFlash 3: slot 7 bank 63 at $A000 is not the file's");

    // reloading the saved file gives the same image
    std::vector<uint8_t> image(r.mem.begin(), r.mem.begin() + 8 * MB);
    FileManager::getFileManager()->files["test.crt"] = file;
    host_cart_max_rom = 8 * MB;
    std::vector<uint8_t> mem2(8 * MB);
    cart_def def;
    rc = C64_CRT::load_crt("/", "test.crt", &def, mem2.data());
    CHECK(rc == SSRET_OK, "the saved EasyFlash 3 file does not load (%d)", rc);
    CHECK(!memcmp(mem2.data(), image.data(), image.size()), "the saved EasyFlash 3 file does not give the same image");

    // a bank field beyond 511 does not fit the window
    Crt big(90);
    big.header(1, 0, 0).chip(512, 0x8000, 0x2000);
    r = load(big, 8 * MB);
    CHECK(r.rc == SSRET_ROM_IMAGE_TOO_LARGE, "EasyFlash 3: bank 512 returned %d, want image too large", r.rc);
    host_ef3_present = false;
}

// The auto-save task writes the file while the C64 keeps running: from a copy of what the C64
// can change, so a write after the copy is neither in the file nor lost.
static void test_save_from_snapshot(void)
{
    Crt crt(32);
    crt.header(1, 0, 0);
    crt.chip(0, 0x8000, 0x2000).chip(0, 0xA000, 0x2000).chip(1, 0x8000, 0x2000).chip(1, 0xA000, 0x2000);
    std::vector<uint8_t> &romh = crt.chips[1].data;
    const uint8_t signature[4] = { 'e', 'a', 'p', 'i' };
    memcpy(&romh[0x1800], signature, 4);
    Loaded r = load(crt, MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash: load returned %d", r.rc);

    uint8_t *saved = (uint8_t *)r.at(0x4010);   // bank 1, what a game's save changes
    uint8_t *later = (uint8_t *)r.at(0x4020);   // written after the copy
    *saved ^= 0x5A;
    uint32_t hash = 0;
    CHECK(C64_CRT::take_snapshot(&hash), "no snapshot of an EasyFlash image");
    CHECK(hash == C64_CRT::current_hash(), "the snapshot's hash is not the image's");
    CHECK(hash != C64_CRT::get_baseline(), "the change before the copy is not in the snapshot's hash");

    *later ^= 0xA5;
    std::vector<uint8_t> from_copy;
    File out(&from_copy);
    SubsysResultCode_e rc = C64_CRT::save_snapshot(&out);
    CHECK(rc == SSRET_OK, "save from the snapshot returned %d", rc);
    CHECK(C64_CRT::get_baseline() == hash, "the baseline is not the state that was saved");
    CHECK(C64_CRT::current_hash() != C64_CRT::get_baseline(), "the write after the copy is taken as saved");
    rc = C64_CRT::save_snapshot(&out);
    CHECK(rc != SSRET_OK, "a snapshot was saved twice");

    // The same state, saved the usual way, gives the same file.
    *later ^= 0xA5;
    std::vector<uint8_t> direct;
    File out2(&direct);
    rc = C64_CRT::save_crt(&out2);
    CHECK(rc == SSRET_OK, "save returned %d", rc);
    CHECK(from_copy == direct, "the file from the snapshot differs from a direct save (%d vs %d bytes)",
          (int)from_copy.size(), (int)direct.size());
}

// What tells the auto-save task that the C64 wrote: an EasyFlash erase over UCI, or the GMod2
// EEPROM's dirty flag, whose contents are taken into the image on the way.
static void test_change_notice(void)
{
    Loaded r = load(Crt(32).header(1, 0, 0).chip(0, 0x8000, 0x2000).chip(0, 0xA000, 0x2000), MB);
    CHECK(r.rc == SSRET_OK, "EasyFlash: load returned %d", r.rc);
    CHECK(!C64_CRT::take_change_notice(), "a notice without a change");
    C64_CRT::note_change();
    CHECK(C64_CRT::take_change_notice(), "an erase over UCI was not noticed");
    CHECK(!C64_CRT::take_change_notice(), "the notice was not taken");

    r = load(Crt(60).banks(1, 0x8000, 0x2000), MB);
    CHECK(r.rc == SSRET_OK, "GMod2: load returned %d", r.rc);
    const uint32_t base = C64_CRT::get_baseline();
    CHECK(!C64_CRT::take_change_notice(), "a GMod2 notice without a change");
    host_eeprom[7] ^= 0x33;
    host_eeprom_dirty = true;
    CHECK(C64_CRT::take_change_notice(), "an EEPROM write was not noticed");
    CHECK(!host_eeprom_dirty, "the EEPROM's dirty flag is still set");
    CHECK(C64_CRT::current_hash() != base, "the EEPROM change did not reach the image");
    CHECK(!C64_CRT::take_change_notice(), "the EEPROM notice came twice");
}

int main()
{
    test_cartridge_types();
    test_refused();
    test_packets();
    test_chip_placement();
    test_mirroring();
    test_source();
    test_change_detection();
    test_save_keeps_memory();
    test_save_from_snapshot();
    test_save_whole_chip();
    test_easyflash3();
    test_change_notice();
    fprintf(stderr, "c64_crt_test: %s (%d checks, %d failed)\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
