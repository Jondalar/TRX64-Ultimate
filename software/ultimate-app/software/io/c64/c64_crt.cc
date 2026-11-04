#include <stdlib.h>
#include "c64_crt.h"
#include "trx_cart.h"
#include "dump_hex.h"
#include "subsys.h"

#define CRTHDR_HDRSIZE 0x10
#define CRTHDR_TYPE    0x16
#define CRTHDR_EXROM   0x18
#define CRTHDR_GAME    0x19
#define CRTHDR_SUBTYPE 0x1A

#define CRTCHP_PKTLEN  0x04
#define CRTCHP_TYPE    0x08
#define CRTCHP_BANK    0x0A
#define CRTCHP_LOAD    0x0C
#define CRTCHP_SIZE    0x0E

const struct C64_CRT::t_cart C64_CRT::c_recognized_c64_carts[] = {
    {  0, 0xFF, CART_NORMAL,    "Normal cartridge" },
    {  1, 0xFF, CART_ACTION,    "Action Replay" }, // max 4 banks of 8K
    {  2, 0xFF, CART_KCS,       "KCS Power Cartridge" },
    {  3, 0xFF, CART_FINAL3,    "Final Cartridge III" }, // max 16 banks (FC3+)
    {  4, 0xFF, CART_SBASIC,    "Simons Basic" },
    {  5, 0xFF, CART_OCEAN_8K,  "Ocean type 1" }, // max 64 banks of 8K, with the exception of the 16K carts, which are limited to 16 banks of 16K
    {  6, 0xFF, CART_NOT_IMPL,  "Expert Cartridge" },
    {  7, 0xFF, CART_NOT_IMPL,  "Fun Play" },
    {  8, 0xFF, CART_SUPERGAMES,"Super Games" },
    {  9, 0xFF, CART_NORDIC,    "Atomic Power" }, // max 8 banks of 8K
    { 10, 0xFF, CART_EPYX,      "Epyx Fastload" },
    { 11, 0xFF, CART_WESTERMANN,"Westermann" },
    { 12, 0xFF, CART_NOT_IMPL,  "Rex" },
    { 13, 0xFF, CART_FINAL12,   "Final Cartridge I" },
    { 14, 0xFF, CART_NOT_IMPL,  "Magic Formel" },
    { 15, 0xFF, CART_SYSTEM3,   "C64 Game System" },
    { 16, 0xFF, CART_NOT_IMPL,  "Warpspeed" },
    { 17, 0xFF, CART_DINAMIC,   "Dinamic" },
    { 18, 0xFF, CART_ZAXXON,    "Zaxxon" },
    { 19, 0xFF, CART_DOMARK,    "Magic Desk, Domark, HES Australia" }, // max 16 banks of 16K
    { 20, 0xFF, CART_SUPERSNAP, "Super Snapshot 5" },
    { 21, 0xFF, CART_COMAL80,   "COMAL 80" },
    { 32, 0xFF, CART_EASYFLASH, "EasyFlash" }, // max 64 banks
    { 33, 0xFF, CART_NOT_IMPL,  "EasyFlash X-Bank" },
    { 34, 0xFF, CART_NOT_IMPL,  "Capture" },
    { 35, 0xFF, CART_NOT_IMPL,  "Action Replay 3" },
    { 36, 0xFF, CART_RETRO,     "Retro Replay" }, // max 8 banks of 8K
    { 37, 0xFF, CART_NOT_IMPL,  "MMC64" },
    { 38, 0xFF, CART_NOT_IMPL,  "MMC Replay" },
    { 39, 0xFF, CART_NOT_IMPL,  "IDE64" },
    { 40, 0xFF, CART_NOT_IMPL,  "Super Snapshot V4" },
    { 41, 0xFF, CART_NOT_IMPL,  "IEEE 488" },
    { 42, 0xFF, CART_NOT_IMPL,  "Game Killer" },
    { 43, 0xFF, CART_NOT_IMPL,  "Prophet 64" },
    { 44, 0xFF, CART_EXOS,      "EXOS" }, // Currently max 1 bank
    { 45, 0xFF, CART_NOT_IMPL,  "Freeze Frame" },
    { 46, 0xFF, CART_NOT_IMPL,  "Freeze Machine" },
    { 47, 0xFF, CART_NOT_IMPL,  "Snapshot64" },
    { 48, 0xFF, CART_NOT_IMPL,  "Super Explode V5" },
    { 49, 0xFF, CART_NOT_IMPL,  "Magic Voice" },
    { 50, 0xFF, CART_NOT_IMPL,  "Action Replay 2" },
    { 51, 0xFF, CART_NOT_IMPL,  "MACH 5" },
    { 52, 0xFF, CART_NOT_IMPL,  "Diashow Maker" },
    { 53, 0xFF, CART_PAGEFOX,   "Pagefox" },
    { 54, 0xFF, CART_BBASIC,    "Kingsoft Business Basic" },
    { 55, 0xFF, CART_NOT_IMPL,  "Silver Rock 128" },
    { 56, 0xFF, CART_NOT_IMPL,  "Formel 64" },
    { 57, 0xFF, CART_NOT_IMPL,  "RGCD" },
    { 58, 0xFF, CART_NOT_IMPL,  "RR-Net MK3" },
    { 59, 0xFF, CART_NOT_IMPL,  "Easy Calc" },
    { 60, 0xFF, CART_GMOD2,     "GMod2" },
    { 61, 0xFF, CART_NOT_IMPL,  "MAX Basic" },
    { 62, 0xFF, CART_NOT_IMPL,  "GMod3" },
    { 63, 0xFF, CART_NOT_IMPL,  "ZIPP-CODE 48" },
    { 64, 0xFF, CART_BLACKBOX8, "Blackbox V8" },
    { 65, 0xFF, CART_BLACKBOX3, "Blackbox V3" },
    { 66, 0xFF, CART_BLACKBOX4, "Blackbox V4" },
    { 67, 0xFF, CART_NOT_IMPL,  "REX RAM-Floppy" },
    { 68, 0xFF, CART_NOT_IMPL,  "BIS-Plus" },
    { 69, 0xFF, CART_NOT_IMPL,  "SD-BOX" },
    { 70, 0xFF, CART_NOT_IMPL,  "MultiMAX" },
    { 71, 0xFF, CART_BLACKBOX9, "Blackbox V9" },
    { 72, 0xFF, CART_NOT_IMPL,  "Lt. Kernal Host Adaptor" },
    { 73, 0xFF, CART_NOT_IMPL,  "RAMLink" },
    { 74, 0xFF, CART_NOT_IMPL,  "Drean" },
    { 75, 0xFF, CART_NOT_IMPL,  "IEEE Flash! 64" },
    { 76, 0xFF, CART_NOT_IMPL,  "Turtle Graphics II" },
    { 77, 0xFF, CART_NOT_IMPL,  "Freeze Frame MK2" },
    { 78, 0xFF, CART_NOT_IMPL,  "Partner 64" },
    { 79, 0xFF, CART_NOT_IMPL,  "Hyper-BASIC" },
    { 80, 0xFF, CART_NOT_IMPL,  "Universal Cartridge 1" },
    { 81, 0xFF, CART_NOT_IMPL,  "Universal Cartridge 1.5" },
    { 82, 0xFF, CART_NOT_IMPL,  "Universal Cartridge 2" },
    { 83, 0xFF, CART_NOT_IMPL,  "BMP Data Turbo" },
    { 84, 0xFF, CART_NOT_IMPL,  "Profi-DOS" },
    { 85, 0xFF, CART_MAGICDESK16, "Magic Desk 16" },
    { 86, 0xFF, CART_MEGABYTER, "Protovision Megabyter" },
    { 87, 0xFF, CART_NOT_IMPL,  "GMod4" },
    // Types beyond VICE's, numbered with TRX64 (TRX64-Ultimate PROP-002 Rev C)
    { 88, 0xFF, CART_C64MEGACART, "C64MegaCart" },  // without the TRX64 bitstream: Ocean 8K, read only
    { 89, 0xFF, CART_TWOMEGABYTER, "Protovision TwoMegabyter" },
    { 90, 0xFF, CART_EASYFLASH3, "EasyFlash 3" },    // TRX64 PROP-002 sec. 8.4: 8 MB, 8 slots of 64 banks, bank = slot * 64 + bank (T51)
    { 232, 0xFF, CART_EASYFLASH_XL, "EasyFlashXl" }, // TRX64's 4 MB EasyFlash, 256 banks

    { 0xFFFF, 0xFF, CART_NOT_IMPL, "" } };

const struct C64_CRT::t_cart C64_CRT::c_recognized_c128_carts[] = {
    { 0, 0xFF, CART_C128_STD,    "C128 Cartridge" },
    { 1, 0xFF, CART_C128_STD_IO, "C128 Cartridge with I/O Mirror" },
    { 0xFFFF, 0xFF, CART_NOT_IMPL, "" } };

__inline static uint16_t get_word(uint8_t *p)
{
    return (uint16_t(p[0]) << 8) + p[1];
}

__inline static uint32_t get_dword(uint8_t *p)
{
    return uint32_t(get_word(p) << 16) + get_word(p + 2);
}

// Holds the image's mutex for a scope. Recursive, so a locked function may call another.
class CrtLock {
    SemaphoreHandle_t mutex;
public:
    CrtLock(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
    ~CrtLock() { xSemaphoreGiveRecursive(mutex); }
};

C64_CRT *C64_CRT :: get_instance()
{
    static C64_CRT *crt = NULL;
    if (!crt) {
        crt = new C64_CRT();
    }
    return crt;
}

C64_CRT::C64_CRT() : chip_chunks(64, NULL)
{
    eeprom_buffer = NULL;
    snapshot = NULL;
    change_notice = false;
    mutex = xSemaphoreCreateRecursiveMutex();
    initialize(NULL, 0);
}

void C64_CRT::initialize(uint8_t *mem, uint32_t max_size)
{
    cleanup();

    cart_memory = mem;
    max_cart = max_size;
    local_type = CART_NOT_IMPL;
    machine = 0;
    total_read = 0;
    max_bank = 0xFF;
    highest_bank = 0;
    a000_seen = false;
    bank_multiplier = 16 * 1024;
    source = "";
    baseline_hash = 0;
}

void C64_CRT::cleanup()
{
    for (int i=0; i<chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        if (cc) {
            delete cc;
        }
    }
    chip_chunks.clear_list();

    if (eeprom_buffer) {
        delete[] eeprom_buffer;
        eeprom_buffer = NULL;
    }
    eeprom_size = 0;

    drop_snapshot();
}

void C64_CRT::clear_definition(cart_def *def)
{
    def->custom_addr = 0;
    def->length = 0;
    def->require = 0;
    def->prohibit = 0;
    def->disabled = 0;
    def->filename = "";
    def->type = CART_TYPE_NONE;
    def->name = "None";
}

SubsysResultCode_e C64_CRT::check_header(File *f, cart_def *def)
{
    uint32_t bytes_read = 0;
    FRESULT res = f->read(crt_header, 0x40, &bytes_read);
    if (bytes_read != 0x40) {
        return SSRET_FILE_READ_FAILED; // unable to read file header
    }
    machine = 0;
    const struct t_cart *cart_list;
    if (strncmp((char*)crt_header, "C64 CARTRIDGE   ", 16) == 0) {
        machine = 64;
        cart_list = c_recognized_c64_carts;
    } else if (strncmp((char*)crt_header, "C128 CARTRIDGE  ", 16) == 0) {
        machine = 128;
        cart_list = c_recognized_c128_carts;
    } else {
        return SSRET_ERROR_IN_FILE_FORMAT; // unrecognized file header
    }

    uint16_t hw_type = get_word(crt_header + CRTHDR_TYPE);
    printf("CRT Hardware type: %d ", hw_type);
    local_type = CART_NOT_IMPL;

    while (cart_list->crt_type != 0xFFFF) {
        if (uint8_t(cart_list->crt_type) == hw_type) {
            def->name = cart_list->cart_name;
            local_type = cart_list->local_type;
            max_bank = cart_list->max_bank;
            if (!trx_cart_present()) { // types only the TRX64 bitstream maps
                if (local_type == CART_C64MEGACART) {
                    local_type = CART_OCEAN_8K;
                } else if ((local_type == CART_DINAMIC) || (local_type == CART_MAGICDESK16) ||
                           (local_type == CART_EASYFLASH_XL)) {
                    local_type = CART_NOT_IMPL;
                }
            }
            if ((local_type == CART_EASYFLASH3) && !trx_ef3_present()) { // XPORT_CAPS: the bitstream has the 8 MiB window and the block (so ROM_SEL too)
                printf("EasyFlash 3 needs the TRX64 bitstream with the EasyFlash 3 block.\n");
                return SSRET_NOT_IMPLEMENTED; // not the generic fallback: 8 MB of banks would be nonsense as Ocean
            }
            if (local_type == CART_NOT_IMPL) {
                printf("%s - Not natively implemented, trying fallback...\n",
                       cart_list->cart_name);
                break;  // fall through to generic fallback below
            }
            if (local_type == CART_GMOD2) {
                if (!(getFpgaCapabilities() & CAPAB_EEPROM)) {
                    printf("GMOD2 EEPROM Not implemented.\n");
                    return SSRET_NOT_IMPLEMENTED;
                }
            }
            return SSRET_OK;
        }
        cart_list++;
    }

    // =====================================================================
    // Generic fallback for unknown or not-implemented CRT types.
    // Scan the CHIP packets to determine if it's ROML-only (8K banking)
    // or ROML+ROMH (16K). Then map to a compatible emulation type.
    // =====================================================================
    uint8_t exrom = crt_header[CRTHDR_EXROM];
    uint8_t game  = crt_header[CRTHDR_GAME];
    bool has_romh = false;

    // Peek at CHIP packets to check for ROMH
    uint32_t saved_pos = get_dword(crt_header + CRTHDR_HDRSIZE);
    f->seek(saved_pos);

    uint8_t ch[0x10];
    uint32_t tr;
    int chip_count = 0;
    while (true) {
        FRESULT fres = f->read(ch, 0x10, &tr);
        if (fres != FR_OK || tr != 0x10 || memcmp(ch, "CHIP", 4) != 0)
            break;
        uint16_t load = get_word(ch + 0x0C);
        uint32_t pkt_len = get_dword(ch + 0x04);
        if (load != 0x8000)
            has_romh = true;
        chip_count++;
        // Skip chip data
        uint32_t skip = pkt_len - 0x10;
        while (skip > 0) {
            uint8_t skipbuf[256];
            uint32_t chunk = (skip > 256) ? 256 : skip;
            f->read(skipbuf, chunk, &tr);
            if (tr == 0) break;
            skip -= tr;
        }
    }
    // Seek back for the actual loader
    f->seek(saved_pos);

    if (chip_count == 0) {
        printf("No CHIP packets found.\n");
        return SSRET_NOT_IMPLEMENTED;
    }

    // Choose fallback type based on EXROM/GAME and ROMH presence
    const char *fallback_name;
    if (has_romh) {
        // Has ROMH — treat as 16K Ocean
        local_type = CART_OCEAN_8K;  // Ocean handles both 8K and 16K variants
        max_bank = 0xFF;
        fallback_name = "Ocean 16K";
    } else {
        // ROML only — treat as Ocean 8K (Magic Desk compatible)
        local_type = CART_OCEAN_8K;
        max_bank = 0xFF;
        fallback_name = "Ocean 8K";
    }

    printf("Unknown type %d (%d chips, %s), loading as %s\n",
           hw_type, chip_count, has_romh ? "ROML+ROMH" : "ROML only", fallback_name);

    // Set a name for the cart definition
    static char fallback_desc[64];
    if (def->name && def->name[0]) {
        snprintf(fallback_desc, sizeof(fallback_desc), "%s (as %s)", def->name, fallback_name);
    } else {
        snprintf(fallback_desc, sizeof(fallback_desc), "Type %d (as %s)", hw_type, fallback_name);
    }
    def->name = fallback_desc;

    return SSRET_OK;
}

SubsysResultCode_e C64_CRT::read_chip_packet(File *f, t_crt_chip_chunk *chunk)
{
    uint8_t *chip_header = chunk->header;
    chunk->ram_location = NULL;
    chunk->mandatory = true;
    chunk->last = false;

    uint32_t bytes_read;
    FRESULT res = f->read(chip_header, 0x10, &bytes_read);
    // dump_hex_relative(chip_header, 0x10);
    if ((res != FR_OK) || (bytes_read != 0x10)) {
        chunk->last = true;
        return SSRET_OK; // stop, no error
    }
    if (strncmp((char*)chip_header, "CHIP", 4)) {
        if (chip_chunks.get_elements()) { // data after the last packet, e.g. transfer padding
            chunk->last = true;
            return SSRET_OK; // stop, no error
        }
        return SSRET_ERROR_IN_FILE_FORMAT; // stop, error
    }

    uint32_t total_length = get_dword(chip_header + CRTCHP_PKTLEN);
    uint16_t bank = get_word(chip_header + CRTCHP_BANK);
    uint16_t load = get_word(chip_header + CRTCHP_LOAD);
    uint16_t size = get_word(chip_header + CRTCHP_SIZE);
    uint16_t type = get_word(chip_header + CRTCHP_TYPE);

    // Detect C128 mode carts
    if ((load == 0xC000) || (size == 0x8000)) {
        bank_multiplier = 32 * 1024;
    }

    // Detect E2PROM
    if (load == 0xDE00) {
        printf("Reading EEPROM data, size $%4x.\n", size);
        if (size > 0x800) {
            printf("EEPROM too large!\n");
            return SSRET_EEPROM_TOO_LARGE;
        }
        if (eeprom_buffer) { //
            printf("EEPROM already read!\n");
            return SSRET_EEPROM_ALREADY_DEFINED;
        }
        eeprom_buffer = new uint8_t[size];
        eeprom_size = size;

        chunk->ram_location = eeprom_buffer;
        res = f->read(eeprom_buffer, size, &bytes_read);

        if (getFpgaCapabilities() & CAPAB_EEPROM) {
            C64 :: set_eeprom_data(eeprom_buffer);
        }

        if (res != FR_OK) {
            return SSRET_FILE_READ_FAILED;
        } else {
            return SSRET_OK; // OK
        }
    }

    if (load == 0xA000) {
        a000_seen = true; // an Ocean CRT with $A000 chips needs 16K mode
    }

    // if ((load == 0xA000) && !a000_seen) {
    //     a000_seen = true;
    //     if (bank > 0) { // strange; first time A000 is seen, it is not bank 0.
    //         max_bank = bank - 1;
    //     }
    // }
    // bank &= max_bank;

    uint32_t offset = uint32_t(bank) * bank_multiplier;
    offset += (load & 0x2000); // switch between 8000 and A000
    if (offset >= max_cart) { // exceeds max size
        return SSRET_ROM_IMAGE_TOO_LARGE;
    }

    // Valid for ROM only
    uint8_t *mem_addr = cart_memory;
    mem_addr += offset; // by default 16K; unless it is a C128 cart.
    printf("Reading chip data bank %d ($%4x) with size $%4x to 0x%8x.\n", bank, load, size, mem_addr);

    if (size && ((type == 0) || (type == 2))) {
        chunk->ram_location = mem_addr;
        res = f->read(mem_addr, size, &bytes_read);
        total_read += bytes_read;
        if (bytes_read != size) {
            printf("Just read %4x bytes\n", bytes_read);
            return SSRET_FILE_READ_FAILED;
        }
        if (bank > highest_bank) {
            highest_bank = bank;
        }
        if (bytes_read < 8192) {
            // round up to the nearest multiple of 2, minimum 256 bytes
            for (int i=256; i<size; i*=2) {
                if (bytes_read < i) {
                    bytes_read = i;
                    break;
                }
            }
            // mirror data up to 8K block
            while (bytes_read < 8192) {
                printf("Mirroring %4x bytes from %p to %p.\n", bytes_read, mem_addr, mem_addr + bytes_read);
                memcpy(mem_addr + bytes_read, mem_addr, bytes_read);
                bytes_read <<= 1;
            }
        }
    } else {
        printf("Skipping Chip packet Type %d with size %4x.\n", type, size);
    }
    return SSRET_OK;
}

void C64_CRT::clear_cart_mem(void)
{
    memset(cart_memory, 0xff, max_cart); // clear the whole cartridge region
}

// The flash chip of a cartridge the C64 can program, in 16K slots of cartridge memory; 0 for
// any other type. The 8K banks of a ROML-only cartridge take the lower half of one slot each.
static int flash_chip_slots(uint8_t local_type)
{
    switch (local_type) {
    case CART_EASYFLASH:    return 64;  // 1 MB, ROML and ROMH per bank
    case CART_GMOD2:        return 64;  // 512 KB in 8K banks
    case CART_MEGABYTER:    return 128; // 1 MB in 8K banks
    case CART_C64MEGACART:  return 256; // 2 MB in 8K banks; TRX64 bitstream only
    case CART_TWOMEGABYTER: return trx_cart_present() ? 128 : 0; // 2 MB, one chip across ROML and ROMH
    case CART_EASYFLASH_XL: return 256; // 2 x 2 MB, ROML and ROMH per bank; TRX64 bitstream only
    case CART_EASYFLASH3:   return 512; // 8 slots of 64 banks, ROML and ROMH per bank; TRX64 bitstream with the EF3 block only
    default:                return 0;
    }
}

void C64_CRT::auto_mirror(void)
{
    if (bank_multiplier == 32 * 1024) {
        return; // C128 mode
    }    
    // bank size is 16k. highest_bank is the highest bank number seen in the file.
    // First round up to the nearest power of 2, then mirror it.

    int size = 1;
    while (size <= highest_bank) {
        size <<= 1;
    }
    // A flash cartridge always has its whole chip. The banks its file leaves out are erased flash
    // and read $FF, which is what a free-bank scan looks for, and a copy there would be saved
    // back as data. Only above the chip does the image repeat, for the bank bits it ignores.
    int chip_banks = flash_chip_slots(local_type);
    if (size < chip_banks) {
        size = chip_banks;
    }
    // mirror up to the end of the cart memory, which is what the FPGA can address
    int max_banks = max_cart / bank_multiplier;
    while(size < max_banks) {
        // mirror the data
        printf("Mirroring %6x bytes from %p to %p.\n", size * bank_multiplier, cart_memory, cart_memory + size * bank_multiplier);
        memcpy(cart_memory + size * bank_multiplier, cart_memory, size * bank_multiplier);
        size <<= 1;
    }
}

// A flash cartridge is saved as the whole chip, as the C64 sees it: one packet per bank, in bank
// order, the erased ones included, so a bank the file left out and the C64 programmed is saved
// too. The banks above the chip are mirrors and are not part of it. A GMod2 EEPROM packet stays,
// after the banks.
void C64_CRT::regenerate_flash_chunks()
{
    int slots = flash_chip_slots(local_type);
    if (!slots || (bank_multiplier != 16 * 1024)) {
        return;
    }
    if (uint32_t(slots) * 16384 > max_cart) {
        slots = max_cart / 16384;
    }
    bool two_chips = (local_type == CART_EASYFLASH) || (local_type == CART_EASYFLASH_XL) || (local_type == CART_EASYFLASH3);
    bool linear = (local_type == CART_TWOMEGABYTER); // a bank is 16K of the one chip
    uint16_t chip_type = 0xFFFF;
    t_crt_chip_chunk *eeprom = NULL;
    for (int i=0; i<chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        uint16_t load = get_word(cc->header + CRTCHP_LOAD);
        uint16_t size = get_word(cc->header + CRTCHP_SIZE);
        if (load == 0xDE00) {
            eeprom = cc;
            continue;
        }
        if (linear ? !(((load == 0x8000) && (size <= 0x4000)) || ((load == 0xA000) && (size <= 0x2000)))
                   : (!two_chips && ((load != 0x8000) || (size > 0x2000)))) {
            return; // not the chip's banks: keep the packets of the file
        }
        if (chip_type == 0xFFFF) {
            chip_type = get_word(cc->header + CRTCHP_TYPE);
        }
    }
    if (chip_type == 0xFFFF) {
        chip_type = 0;
    }
    for (int i=0; i<chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        if (cc && (cc != eeprom)) {
            delete cc;
        }
    }
    chip_chunks.clear_list();

    static const uint8_t chip_header[16] = { 0x43, 0x48, 0x49, 0x50, 0x00, 0x00, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x20, 0x00};

    for (int i=0; i<slots; i++) {
        for (int c=0; c<(two_chips ? 2 : 1); c++) {
            t_crt_chip_chunk *ch = new t_crt_chip_chunk;
            ch->mandatory = true;
            ch->last = false;
            memcpy(ch->header, chip_header, 16);
            ch->header[8]  = (uint8_t)(chip_type >> 8);
            ch->header[9]  = (uint8_t) chip_type;
            ch->header[10] = (uint8_t)(i >> 8);
            ch->header[11] = (uint8_t) i;
            ch->header[12] = c ? 0xA0 : 0x80;
            if (linear) { // one 16K packet per bank
                ch->header[6] = 0x40;
                ch->header[14] = 0x40;
            }
            ch->ram_location = cart_memory + uint32_t(i) * 16384 + c * 8192;
            chip_chunks.append(ch);
        }
    }
    if (eeprom) {
        chip_chunks.append(eeprom);
    }
}

SubsysResultCode_e C64_CRT::read_crt(File *file, cart_def *def)
{
    SubsysResultCode_e retval = check_header(file, def); // pointer is now at 0x20
    if (retval != SSRET_OK) { // something went wrong
        return retval;
    }
    // The window is 8 MiB with the EasyFlash 3 block; only type 90 uses more than the 4 MiB every other
    // type is addressed with (and mirrored up to).
    if ((local_type != CART_EASYFLASH3) && (max_cart > TRX_XPORT_ROM_MAX) && trx_xport_shared()) {
        max_cart = TRX_XPORT_ROM_MAX;
    }
    clear_cart_mem();
    uint32_t dw = get_dword(crt_header + CRTHDR_HDRSIZE);
    if (dw < 0x40) {
        dw = 0x40;
    }

    printf("Header OK. Now reading chip packets starting from %6x.\n", dw);
    FRESULT fres = file->seek(dw);
    if (fres != FR_OK) {
        return SSRET_FILE_SEEK_FAILED;
    }

    do {
        t_crt_chip_chunk *chunk = new t_crt_chip_chunk;
        retval = read_chip_packet(file, chunk);

        if (retval == SSRET_OK) {
            if (chunk->last) {
                delete chunk;
                break;
            }
            chip_chunks.append(chunk);
        } else {
            delete chunk;
            break;
        }

    } while (1);

    if (retval != SSRET_OK) {
        return retval;
    }

    auto_mirror();
    regenerate_flash_chunks();
    configure_cart(def);

    // The state the C64 starts from, mirrors included.
    baseline_hash = content_hash(NULL);

    return SSRET_OK;
}

void C64_CRT::configure_cart(cart_def *def)
{
    printf("Total ROM size read: %6x bytes.\n", total_read);

    uint16_t cart_type = CART_TYPE_NONE;
    uint16_t require = 0;
    uint16_t prohibit = 0;

    switch (local_type) {
        case CART_NORMAL:
            if ((crt_header[CRTHDR_EXROM] == 1) && (crt_header[CRTHDR_GAME] == 0)) {
                cart_type = CART_TYPE_UMAX;
            } else if ((crt_header[CRTHDR_EXROM] == 0) && (crt_header[CRTHDR_GAME] == 0)) {
                cart_type = CART_TYPE_16K;
            } else if ((crt_header[CRTHDR_EXROM] == 0) && (crt_header[CRTHDR_GAME] == 1)) {
                cart_type = CART_TYPE_8K;
            }
            break;
        case CART_ACTION:
            cart_type = CART_TYPE_ACTION;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_RETRO:
            cart_type = CART_TYPE_RETRO;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_DOMARK:
            cart_type = CART_TYPE_DOMARK;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_OCEAN_8K:
            prohibit = CART_PROHIBIT_DEXX;
            if (a000_seen) { // special case!
                cart_type = CART_TYPE_OCEAN_16K | VARIANT_3; // 16 banks of 16K
            } else {
                cart_type = CART_TYPE_OCEAN_8K;
            }
            break;
        case CART_EASYFLASH:
        case CART_EASYFLASH_XL:
        case CART_EASYFLASH3:
            cart_type = (local_type == CART_EASYFLASH) ? CART_TYPE_EASY_FLASH :
                        (local_type == CART_EASYFLASH3) ? CART_TYPE_EASYFLASH3 : CART_TYPE_EASYFLASH_XL;
            if (crt_header[CRTHDR_SUBTYPE] == 1) {
                prohibit = CART_PROHIBIT_ALL_BUT_REU;
                require = CART_UCI_DE1C;
            } else {
                prohibit = CART_PROHIBIT_IO;
                require = CART_UCI_DE1C;
            }
            break;
        case CART_SUPERSNAP:
            cart_type = CART_TYPE_SS5; // Snappy
            prohibit = CART_PROHIBIT_DEXX;
            if (total_read > 65536) {
                cart_type |= VARIANT_1;
            }
            break;
        case CART_ZAXXON:
            cart_type = CART_TYPE_ZAXXON;
            // in this special case, the first chip chunk should be copied to both banks
            // the read chip chunk routine should already have doubled the 4K to 8K.
            printf("Mirroring 2000 bytes from %p to %p.\n", cart_memory, cart_memory + 0x4000);
            memcpy(cart_memory + 0x4000, cart_memory, 0x2000);
            break;
        case CART_EPYX:
            cart_type = CART_TYPE_EPYX; // Epyx
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_FINAL3:
            cart_type = CART_TYPE_FC3; // Final3
            if (crt_header[CRTHDR_SUBTYPE] == 1) {
                prohibit = CART_PROHIBIT_ALL_BUT_REU;
            } else {
                prohibit = CART_PROHIBIT_IO;
            }
            if (total_read > 65536) {
                cart_type |= VARIANT_1;
            }
            break;
        case CART_SYSTEM3:
            cart_type = CART_TYPE_SYSTEM3; // System3
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_KCS:
            cart_type = CART_TYPE_KCS; // System3
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_FINAL12:
            cart_type = CART_TYPE_FINAL12;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_COMAL80:
            if (crt_header[CRTHDR_SUBTYPE] == 1) {
                cart_type = CART_TYPE_OCEAN_16K | VARIANT_2; // Grey Comal 80: bit 6 does not switch the cartridge off
            } else if (total_read > 65536) {
                cart_type = CART_TYPE_OCEAN_16K | VARIANT_1; // Comal 80 Pakma // V5: Type = Ocean_16K, Variant = 1
            } else {
                cart_type = CART_TYPE_OCEAN_16K; // Comal 80 // V5: Type = Ocean_16K, Variant = 0
            }
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_SBASIC:
            cart_type = CART_TYPE_SBASIC; // Simons Basic
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_WESTERMANN:
            cart_type = CART_TYPE_WESTERMANN; // Westermann
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_BBASIC:
            cart_type = CART_TYPE_BBASIC; // Business Basic
            prohibit = CART_PROHIBIT_DEXX;
            // CHIPs have been loaded at 0000, 4000 and 8000; while they should be at 0000, 2000 and 6000
            // So source and destinations do not overlap; we can simply copy the data
            memcpy(cart_memory + 0x2000, cart_memory + 0x4000, 0x2000);
            memcpy(cart_memory + 0x6000, cart_memory + 0x8000, 0x2000);
            break;
        case CART_PAGEFOX:
            cart_type = CART_TYPE_PAGEFOX; // Pagefox
            // No prohibits!
            break;
        case CART_SUPERGAMES:
            cart_type = CART_TYPE_SUPERGAMES; // Super Games, 16K banks
            prohibit = CART_PROHIBIT_DFXX;
            break;
        case CART_NORDIC:
            cart_type = CART_TYPE_NORDIC;
            require = CART_WMIRROR | CART_DYNAMIC;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_BLACKBOX3:
            cart_type = CART_TYPE_BLACKBOX_V3;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_BLACKBOX4:
            cart_type = CART_TYPE_BLACKBOX_V4;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_BLACKBOX8:
            cart_type = CART_TYPE_BLACKBOX_V8;
            prohibit = CART_PROHIBIT_DFXX;
            break;
        case CART_BLACKBOX9:
            cart_type = CART_TYPE_BLACKBOX_V9;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_MEGABYTER:
            cart_type = CART_TYPE_MEGABYTER;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_TWOMEGABYTER:
            cart_type = CART_TYPE_MEGABYTER | VARIANT_1;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_GMOD2:
            prohibit = CART_PROHIBIT_DEXX;
            cart_type = CART_TYPE_GMOD2;
            find_eeprom();
            break;
        case CART_EXOS:
            require = CART_KERNAL;
            break;
        case CART_C64MEGACART: // $DE00 bank, $DF00 mode
            cart_type = CART_TYPE_C64MEGACART;
            prohibit = CART_PROHIBIT_IO;
            break;
        case CART_DINAMIC:     // bank register in $DExx
            cart_type = CART_TYPE_DINAMIC;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_MAGICDESK16: // $DE00, 16K banks
            cart_type = CART_TYPE_MAGICDESK16;
            prohibit = CART_PROHIBIT_DEXX;
            break;
        case CART_C128_STD:
            cart_type = CART_TYPE_128;
            break;

        case CART_C128_STD_IO:
            cart_type = CART_TYPE_128 | VARIANT_3; // with IO

            if (crt_header[CRTHDR_SUBTYPE] == 1) {
                prohibit = CART_PROHIBIT_ALL_BUT_REU;
                require = CART_UCI;
            } else if (crt_header[CRTHDR_SUBTYPE] == 2) {
                prohibit = CART_PROHIBIT_ALL_BUT_REU_AND_ACIA_DE; // leaves room for ACIA at DE00, REU and UCI at DF00
                require = CART_UCI;
                cart_type = CART_TYPE_128 | VARIANT_7; // with IO and ROM banking
            } else {
                prohibit = CART_PROHIBIT_IO;
            }
            break;

        default:
            break;
    }
    // TRX64 Ultimate: one module in the expansion port. A cartridge and the REU are never on
    // together (T33 spec, rule "REU or cartridge"), unlike on Gideon's bitstreams.
    if (trx_cart_present() && (cart_type != CART_TYPE_NONE)) {
        prohibit |= CART_REU | CART_MAXREU;
    }
    printf("*%b*\n", cart_type);

    def->type = cart_type;
    def->prohibit = prohibit;
    def->require = require;
}

SubsysResultCode_e C64_CRT::load_crt(const char *path, const char *filename, cart_def *def, uint8_t *mem)
{
    File *file = 0;
    FileInfo *inf;
    FileManager *fm = FileManager::getFileManager();

    clear_definition(def);
    def->filename = filename;

    FRESULT fres = fm->fopen(path, filename, FA_READ, &file);
    if (fres != FR_OK) {
        return { SSRET_CANNOT_OPEN_FILE };
    }

    C64_CRT *work = get_instance(); // Singleton.
    CrtLock lock(work->mutex);
    work->initialize(mem, C64::get_cartridge_max_rom()); // Clear previous definitions
    SubsysResultCode_e retval = work->read_crt(file, def);
    fm->fclose(file);

    if (retval != SSRET_OK) {
        work->initialize(NULL, 0); // clear remaining stuff if not successful
    } else {
        set_source(path, filename);
    }
    return { retval };
}

// REST passes an empty path and a full pathname as filename.
void C64_CRT::set_source(const char *path, const char *filename)
{
    C64_CRT *crt = get_instance();
    crt->source = path;
    int len = crt->source.length();
    if (len && (path[len - 1] != '/')) {
        crt->source += "/";
    }
    crt->source += filename;
}

const char *C64_CRT::get_source(void)
{
    return get_instance()->source.c_str();
}

// Over exactly the bytes save_crt() writes. Each step is a bijection in h for a fixed word, so two
// images that differ in one word always hash differently. No multiply: the U64 firmware is built
// with -mno-hw-mul, so one would be a library call.
// Whether the C64 can change this chunk, which is what the hash has to cover.
//
// On Gideon's bitstreams EasyFlash is the only cartridge whose ROM image the machine writes: in
// all_carts_v5.vhd it is the one type with allow_write set while addr_map is ROM, reached by
// arming $DE09 the way the patched EAPI does. GMod2 changes only through its EEPROM. The write
// paths of Action Replay, Retro Replay, KCS, SS5, Pagefox and the C128 cartridges all target
// memory outside these chunks.
//
// On the TRX64 bitstream the firmware itself applies every flash program and erase to the image
// (trx_cart.cc), for EasyFlash, EasyFlashXl, GMod2, MegaByter, TwoMegabyter and C64MegaCart, so
// all their chunks can change.
static bool chunk_is_writable_by_c64(uint8_t local_type, uint16_t load, uint16_t size)
{
    switch (local_type) {
    case CART_EASYFLASH:
        return true;
    case CART_GMOD2:
        return trx_cart_present() || ((load == 0xDE00) && (size == 0x800));
    case CART_EASYFLASH_XL:
    case CART_EASYFLASH3:
        return true;
    case CART_MEGABYTER:
    case CART_TWOMEGABYTER:
    case CART_C64MEGACART:
        return trx_cart_present();
    default:
        return false;
    }
}

// The GMod2 EEPROM chunk, or NULL. Its contents live in the FPGA and reach the image only when
// they are taken from there.
C64_CRT::t_crt_chip_chunk *C64_CRT::eeprom_chunk(void)
{
    if (!(getFpgaCapabilities() & CAPAB_EEPROM)) {
        return NULL;
    }
    for (int i = 0; i < chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        if ((get_word(cc->header + CRTCHP_LOAD) == 0xDE00) && (get_word(cc->header + CRTCHP_SIZE) == 0x800)) {
            return cc;
        }
    }
    return NULL;
}

// Over the image in memory, or with snap over a snapshot of it, which holds the chunks the C64
// can change one after the other.
uint32_t C64_CRT::content_hash(const uint8_t *snap)
{
    uint32_t h = 0x811C9DC5;
    if (!snap) {
        // A pending EEPROM change goes into its buffer first, so the hash covers it.
        t_crt_chip_chunk *ee = eeprom_chunk();
        if (ee && C64 :: get_eeprom_dirty()) {
            pull_eeprom(ee->ram_location);
        }
    }
    for (int i = 0; i < chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        uint16_t size = get_word(cc->header + CRTCHP_SIZE);
        uint16_t load = get_word(cc->header + CRTCHP_LOAD);

        if (!chunk_is_writable_by_c64(local_type, load, size)) {
            continue;
        }
        const uint32_t *w = (const uint32_t *)(snap ? snap : cc->ram_location);
        for (int n = 0; n < size / 4; n++) {
            h ^= w[n];
            h = ((h << 5) | (h >> 27)) + 0x9E3779B9;
        }
        if (snap) {
            snap += size;
        }
    }
    return h;
}

uint32_t C64_CRT::current_hash(void)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);
    return crt->content_hash(NULL);
}

uint32_t C64_CRT::get_baseline(void)
{
    return get_instance()->baseline_hash;
}

void C64_CRT::set_baseline(uint32_t hash)
{
    get_instance()->baseline_hash = hash;
}

SubsysResultCode_e C64_CRT::save_crt(File *fo)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);

    if (!crt->machine) {
        return SSRET_SUBSYS_NOT_PRESENT;
    }
    uint32_t hash = crt->content_hash(NULL); // callers stop the C64, so this is what the file receives
    SubsysResultCode_e ret = crt->write_image(fo, NULL);
    if (ret == SSRET_OK) {
        crt->baseline_hash = hash; // memory and file agree again, whichever file it was
    }
    return ret;
}

// The CRT file from the image in memory. With snap, the chunks the C64 can change come from
// that snapshot instead, in the order content_hash() takes them.
SubsysResultCode_e C64_CRT::write_image(File *fo, const uint8_t *snap)
{
    uint32_t written;

    FRESULT res = fo->write(crt_header, 0x40, &written);
    if (res != FR_OK) {
        return SSRET_DISK_ERROR;
    }

    for (int i=0; i<chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = chip_chunks[i];
        uint16_t size = get_word(cc->header + CRTCHP_SIZE);
        uint16_t load = get_word(cc->header + CRTCHP_LOAD);

        const uint8_t *src = cc->ram_location;
        if (snap && chunk_is_writable_by_c64(local_type, load, size)) {
            src = snap;
            snap += size;
        } else if ((load == 0xDE00) && (size == 0x800)) {
            // Get EEPROM state from Hardware
            if (getFpgaCapabilities() & CAPAB_EEPROM) {
                if (C64 :: get_eeprom_dirty()) {
                    // if not dirty, there is no need to update buffer
                    C64 :: get_eeprom_data(cc->ram_location);
                }
            }
        }

        if (!cc->mandatory)
        {
           bool empty = true;
           const uint32_t *src_32bit = (const uint32_t *) src;

           for (int i=0; i<size/4; i++)
              if (src_32bit[i] != 0xFFFFFFFF)
                 empty = false;

           if (empty)
               continue;
        }


        res = fo->write(cc->header, 0x10, &written);
        if (res != FR_OK) {
            break;
        }

        res = fo->write(src, size, &written);
        if (res != FR_OK) {
            break;
        }
    }

    return (res == FR_OK) ? SSRET_OK : SSRET_DISK_ERROR;
}

void C64_CRT::drop_snapshot(void)
{
    if (snapshot) {
        free(snapshot);
        snapshot = NULL;
    }
}

bool C64_CRT::take_snapshot(uint32_t *hash)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);
    crt->drop_snapshot();
    if (!crt->machine) {
        return false;
    }

    uint32_t total = 0;
    for (int i = 0; i < crt->chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = crt->chip_chunks[i];
        if (chunk_is_writable_by_c64(crt->local_type, get_word(cc->header + CRTCHP_LOAD), get_word(cc->header + CRTCHP_SIZE))) {
            total += get_word(cc->header + CRTCHP_SIZE);
        }
    }
    if (!total) {
        return false;
    }
    crt->snapshot = (uint8_t *)malloc(total); // up to 8 MiB (EasyFlash 3); new would halt the firmware when it fails
    if (!crt->snapshot) {
        return false;
    }

    uint32_t before = crt->content_hash(NULL); // also takes a pending EEPROM change
    uint8_t *dst = crt->snapshot;
    for (int i = 0; i < crt->chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *cc = crt->chip_chunks[i];
        uint16_t size = get_word(cc->header + CRTCHP_SIZE);
        if (chunk_is_writable_by_c64(crt->local_type, get_word(cc->header + CRTCHP_LOAD), size)) {
            memcpy(dst, cc->ram_location, size);
            dst += size;
        }
    }
    crt->snapshot_hash = crt->content_hash(crt->snapshot);

    // The C64 kept running. The copy is one state of the image only if the image did not move
    // while it was taken.
    if ((crt->snapshot_hash != before) || (crt->content_hash(NULL) != before)) {
        crt->drop_snapshot();
        return false;
    }
    *hash = crt->snapshot_hash;
    return true;
}

SubsysResultCode_e C64_CRT::save_snapshot(File *fo)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);
    if (!crt->snapshot || !crt->machine) {
        return SSRET_INTERNAL_ERROR;
    }
    SubsysResultCode_e ret = crt->write_image(fo, crt->snapshot);
    if (ret == SSRET_OK) {
        crt->baseline_hash = crt->snapshot_hash;
    }
    crt->drop_snapshot();
    return ret;
}

void C64_CRT::discard_snapshot(void)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);
    crt->drop_snapshot();
}

void C64_CRT::note_change(void)
{
    get_instance()->change_notice = true;
}

bool C64_CRT::take_change_notice(void)
{
    C64_CRT *crt = get_instance();
    CrtLock lock(crt->mutex);
    bool seen = crt->change_notice;
    crt->change_notice = false;

    // The EEPROM's dirty flag stays set until its contents are taken, so take them: the next
    // write sets it again.
    t_crt_chip_chunk *ee = crt->eeprom_chunk();
    if (ee && C64 :: get_eeprom_dirty()) {
        crt->pull_eeprom(ee->ram_location);
        seen = true;
    }
    return seen;
}

// The EEPROM's contents while the C64 may write to it. The TRX64 bitstream counts every write
// (WR_COUNT, also every flash operation): a copy with the same count before and after is one
// state. Without it (count 0) there is one plain copy, as before.
void C64_CRT::pull_eeprom(uint8_t *dst)
{
    for (int tries = 0; tries < 8; tries++) {
        uint16_t before = trx_cart_wr_count();
        C64 :: get_eeprom_data(dst);
        if (trx_cart_wr_count() == before) {
            return;
        }
    }
}

void C64_CRT :: find_eeprom(void)
{
    // If the hardware doesn't support EEPROM, there is no need to modify the CRT.
    if (!(getFpgaCapabilities() & CAPAB_EEPROM)) {
        return;
    }

    // If the CRT contains an eeprom chunk, no modification is required.
    for(int i=0; i < chip_chunks.get_elements(); i++) {
        t_crt_chip_chunk *ch = chip_chunks[i];
        uint16_t load = get_word(ch->header + CRTCHP_LOAD);
        uint16_t size = get_word(ch->header + CRTCHP_SIZE);
        if ((load == 0xDE00) && (size <= 0x800)) {
            // EEPROM found, no action required
            return;
        }
    }

    // EEPROM chunk not found, so let's create one and clear it.
    t_crt_chip_chunk *ch = new t_crt_chip_chunk;
    ch->mandatory = true;
    const uint8_t eeprom_header[16] = { 0x43, 0x48, 0x49, 0x50, 0x00, 0x00, 0x08, 0x10,
                                        0x00, 0x00, 0x00, 0x00, 0xDE, 0x00, 0x08, 0x00
    };
    memcpy(ch->header, eeprom_header, 16);

    // This shouldn't happen
    if (eeprom_buffer) {
        delete[] eeprom_buffer;
        eeprom_buffer = 0;
    }
    // New bit of memory, cleared
    eeprom_buffer = new uint8_t[0x800];
    eeprom_size = 0x800;
    memset(eeprom_buffer, 0xFF, 0x800);
    ch->ram_location = eeprom_buffer;

    // Add it to the chunks of the file
    chip_chunks.append(ch);

    // Clear EEPROM in hardware as well
    C64 :: set_eeprom_data(eeprom_buffer);
}

int C64_CRT :: clear_crt(void)
{
    C64_CRT *work = get_instance(); // Singleton.
    CrtLock lock(work->mutex);
    work->initialize(NULL, 0);
    return 0;
}

bool C64_CRT :: is_valid(void)
{
    C64_CRT *crt = get_instance(); // Singleton.
    return (crt->machine != 0);
}
