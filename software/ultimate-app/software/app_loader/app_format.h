/*
 * app_format.h
 *
 * U64 App Loader - binary layout of .u2a files, format version 2.
 * See doc/AppLoader_Spec.md section 1. All multi-byte values are little-endian.
 *
 *   header (64 bytes) | variant table (24 bytes each) | manifest (JSON, NUL) | payloads
 */

#ifndef APP_LOADER_APP_FORMAT_H_
#define APP_LOADER_APP_FORMAT_H_

#include <stdint.h>

#define U2A_MAGIC 0x50413255U /* "U2AP" */
#define U2A_HEADER_SIZE 64
#define U2A_VARIANT_SIZE 24
#define U2A_FORMAT_VERSION 2
#define U2A_MAX_VARIANTS 4
#define U2A_MANIFEST_MAX_SIZE 4096 /* bytes including the terminating NUL */

/* Variant architectures */
#define U2A_ARCH_NIOS2 1   /* Nios II (U64) */
#define U2A_ARCH_RISCV32 2 /* RISC-V rv32 (U64-II, C64U) */

/* Fixed load address for all variants.
 *
 * U64 DDR is 64 MB (0x0 .. 0x04000000). Since v3.15 the cartridge ROM takes
 * 0x03C00000 .. 0x04000000, so the app area sits right below it, at the top of
 * the updater area (0x03000000 .. 0x03C00000), which is only in use during a
 * firmware update. U64 and U64-II share this memory map. */
#define U2A_LOAD_ADDR 0x03A00000U
#define U2A_AREA_SIZE 0x00200000U /* 2 MB */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct u2p_app_header_s
{
    uint32_t magic;                /* U2A_MAGIC */
    uint16_t header_size;          /* U2A_HEADER_SIZE */
    uint16_t format_version;       /* U2A_FORMAT_VERSION */
    uint16_t api_version_min;      /* lowest ABI version the app needs */
    uint16_t api_version_max;      /* highest ABI version the app knows */
    uint16_t variant_count;        /* 1 .. U2A_MAX_VARIANTS */
    uint16_t manifest_size;        /* bytes including the terminating NUL, 0 = no manifest */
    uint32_t variant_table_offset; /* U2A_HEADER_SIZE */
    uint32_t manifest_offset;      /* file offset, 0 = no manifest */
    uint32_t flags;                /* 0 */
    char name[28];                 /* display name, zero-padded */
    uint8_t reserved[8];           /* 0 */
} u2p_app_header_t;

typedef struct u2p_app_variant_s
{
    uint16_t arch;           /* U2A_ARCH_* */
    uint16_t reserved;       /* 0 */
    uint32_t load_addr;      /* U2A_LOAD_ADDR */
    uint32_t entry_offset;   /* entry point relative to load_addr */
    uint32_t payload_offset; /* file offset of the raw binary */
    uint32_t payload_size;   /* bytes */
    uint32_t bss_size;       /* bytes to zero after the payload */
} u2p_app_variant_t;

/* Compile-time size checks */
typedef char u2p_app_header_size_check[(sizeof(u2p_app_header_t) == U2A_HEADER_SIZE) ? 1 : -1];
typedef char u2p_app_variant_size_check[(sizeof(u2p_app_variant_t) == U2A_VARIANT_SIZE) ? 1 : -1];

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* APP_LOADER_APP_FORMAT_H_ */
