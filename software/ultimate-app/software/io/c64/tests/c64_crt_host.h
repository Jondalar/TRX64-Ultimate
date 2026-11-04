// Host build support for c64_crt_test.cc, force-included before c64_crt.cc.
//
// c64_crt.h includes filemanager.h, c64.h and subsys.h, which pull in the
// FreeRTOS file system and the whole C64 host. This header defines their
// include guards and provides only what the CRT loader uses. The cartridge
// type constants and cart_def come from c64.h itself, extracted by the
// Makefile into cart_defs.h, so the test sees the firmware's own values.
#ifndef C64_CRT_HOST_H
#define C64_CRT_HOST_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>

#define TRX_HOST_EF3 1 // trx_cart.h's stubs: trx_ef3_present() follows host_ef3_present
#define FILEMANAGER_H
#define C64_H
#define INFRA_SUBSYS_H_

#define ENTER_SAFE_SECTION
#define LEAVE_SAFE_SECTION

// The image's mutex. The test runs in one thread, so taking it always succeeds.
#define INC_FREERTOS_H
#define SEMAPHORE_H
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { static int m; return &m; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, uint32_t) { return 1; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return 1; }
#include "indexed_list.h"
#include "mystring.h"
#include "return_codes.h"
#include "cart_defs.h"

typedef int FRESULT;
#define FR_OK 0
#define FR_NO_FILE 4
#define FA_READ 0x01

class FileInfo {};

// A file over bytes the test registered by name for reading, or over a buffer
// the test owns for writing, so a saved cartridge can be inspected.
class File {
    static const std::vector<uint8_t> &no_data() { static const std::vector<uint8_t> e; return e; }
    const std::vector<uint8_t> &data;
    std::vector<uint8_t> *sink;
    size_t pos;
public:
    explicit File(const std::vector<uint8_t> &d) : data(d), sink(NULL), pos(0) {}
    explicit File(std::vector<uint8_t> *out) : data(no_data()), sink(out), pos(0) {}
    FRESULT read(void *buffer, uint32_t len, uint32_t *transferred) {
        size_t n = (pos < data.size()) ? std::min<size_t>(len, data.size() - pos) : 0;
        memcpy(buffer, data.data() + pos, n);
        pos += n;
        *transferred = (uint32_t)n;
        return FR_OK;
    }
    FRESULT write(const void *buffer, uint32_t len, uint32_t *transferred) {
        if (sink) {
            const uint8_t *p = (const uint8_t *)buffer;
            sink->insert(sink->end(), p, p + len);
        }
        *transferred = len;
        return FR_OK;
    }
    FRESULT seek(uint32_t offset) { pos = offset; return FR_OK; }
};

class FileManager {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    static FileManager *getFileManager() { static FileManager fm; return &fm; }
    FRESULT fopen(const char *, const char *name, int, File **f) {
        auto it = files.find(name);
        if (it == files.end()) {
            return FR_NO_FILE;
        }
        *f = new File(it->second);
        return FR_OK;
    }
    void fclose(File *f) { delete f; }
};

#define CAPAB_EEPROM 0x00400000
extern uint32_t host_fpga_capabilities;
extern uint32_t host_cart_max_rom;
static inline uint32_t getFpgaCapabilities(void) { return host_fpga_capabilities; }

// The GMod2 EEPROM in the FPGA: its contents and the dirty flag a write sets.
extern uint8_t host_eeprom[2048];
extern bool host_eeprom_dirty;

class C64 {
public:
    static uint32_t get_cartridge_max_rom(void) { return host_cart_max_rom; }
    static void set_eeprom_data(uint8_t *b) { memcpy(host_eeprom, b, 2048); host_eeprom_dirty = false; }
    static void get_eeprom_data(uint8_t *b) { host_eeprom_dirty = false; memcpy(b, host_eeprom, 2048); }
    static bool get_eeprom_dirty(void) { return host_eeprom_dirty; }
};

#endif
