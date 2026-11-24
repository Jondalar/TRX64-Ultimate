// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_c64re_nodes.h, copied for the ring library (U18 D6: this copy is the master).
// A chip block built from a .c64re's own nodes, for a file this board did not
// write (TRX64-Ultimate docs/specs/T20-delta-ring.md §9.4 second case, T66
// part 4). The exact case - a file with a "trx64Hw" node - does not come
// through here; it takes the block out of the file.
//
// What this can and cannot do, in one paragraph. A load puts the CPU's and the
// CIAs' flip-flops in over the state bus and replays the VIC and CIA registers
// as bus cycles (trx_ring.cc put_back), so a block only has to carry those.
// The fields a TRX64 file names - registers, timers, TOD, CPU registers, the
// CPU port, colour RAM - are written into a BASE block taken from the machine
// as it runs now (a frame boundary of its own ring). Everything else, above all
// the CPU's instruction sequencer, keeps the base's value: every frame snap
// sits in the same place (the opcode fetch cycle, proc_control.vhd sync_i), so
// that state is the same at every boundary and does not belong to the file.
// Not restored at all: the VIC's raster position and pixel pipeline (the coarse
// cut), the SID (the board cannot read it back, so no file carries ours), and
// the drive.
//
// Pure: no hardware and no file system, so the host bank
// (software/io/c64/host) tests it.
#ifndef TRX_C64RE_NODES_H
#define TRX_C64RE_NODES_H

#include <stdint.h>
#include "trx_ring.h"
#include "trx_json.h"
#include "trx_drive.h"

// What was found in the file and used (the loader reports it).
#define TRX_NODE_CPU      0x0001
#define TRX_NODE_PORT     0x0002
#define TRX_NODE_RAM      0x0004
#define TRX_NODE_CIA1     0x0008
#define TRX_NODE_CIA2     0x0010
#define TRX_NODE_VIC      0x0020
#define TRX_NODE_COLOR    0x0040
#define TRX_NODE_IEC      0x0080
#define TRX_NODE_INTS     0x0100
#define TRX_NODE_KEYS     0x0200
#define TRX_NODE_CART     0x0400
#define TRX_NODE_DRIVE    0x0800
#define TRX_NODE_MEDIA    0x1000
#define TRX_NODE_CARTRAM  0x2000

struct TrxNodeState {
    uint32_t w[TRX_BLOCK_WORDS];    // in: the base block; out: the file's state in it
    uint8_t *ram;                   // in: 65536 bytes to fill
    uint8_t *col;                   // in: 1024 bytes to fill (low nibbles)
    uint8_t *cart;                  // in: TRX_CART_RAM_BYTES or NULL; out: NULL unless it was filled
    int      ram_bytes;             // in: -1 to decode the ram node into ram here; else the bytes the caller decoded into it already
    int      cart_bytes;            // in: -1 to read cartState.cartRam into cart here; else the bytes the caller decoded into it already
    uint16_t x;                     // in/out: the snap position (the base's, kept)
    int      line, cycle;           // out: the VIC position the file named (not restored)
    unsigned have;                  // out: TRX_NODE_*
    int      keys_pressed;          // out: keys in keyboard.livePressed (not restored)
};

// Builds st->w / st->ram / st->col from checkpoint token cp. NULL, or what is
// missing or refused.
const char *trx_c64re_from_nodes(const char *js, const jsmntok_t *t, int n, int cp, TrxNodeState *st);

// The cartridge RAM window of a "cartState" node into out (len bytes): how
// many bytes came, 0 when the node or its cartRam is not there. The exact load
// uses this too - the block comes out of trx64Hw, the RAM out of this node
// (T66 part 5).
int trx_c64re_cart_ram(const char *js, const jsmntok_t *t, int n, int cp, uint8_t *out, int len);

// The "cartFlash" node (the cartridge's ROM / flash window as the file has
// it): its length without decoding, and the bytes. 0 when it is not there.
uint32_t trx_c64re_cart_flash_len(const char *js, const jsmntok_t *t, int n, int cp);
uint32_t trx_c64re_cart_flash(const char *js, const jsmntok_t *t, int n, int cp, uint8_t *out, uint32_t len);

// A "trx64Drive" node (T66 part 8) into st: true when it was there. The node
// is ours, not TRX64's: the VICE modules its "drive1541" wants (DRIVE8,
// DRIVECPU0, VIA1D0, VIA2D0) are a shape this board cannot check against
// anything here, so nothing is written under that name (T20 §13.3).
bool trx_c64re_drive_from_node(const char *js, const jsmntok_t *t, int n, int cp, TrxDriveState *st);

// The cartridge type a "cartState" node names, or -1 when the file has none.
int trx_c64re_cart_type(const char *js, const jsmntok_t *t, int n, int cp);

#endif
