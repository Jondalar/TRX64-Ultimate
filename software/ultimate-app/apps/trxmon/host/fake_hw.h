#ifndef TRXMON_FAKE_HW_H
#define TRXMON_FAKE_HW_H

#include <stdint.h>

#include "../src/hw.h"
#include "trx_lens.h"

typedef struct
{
    uint8_t ram[65536];
    uint8_t rom[0x6000];
    uint8_t vic[48], cia1[16], cia2[16];
    uint8_t cia2_pa, cia2_ddra;
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t ddr, latch, port, game, exrom, cart;
    /* the lens unit's cartridge */
    int has_lk, cart_ext, lk_calls;
    uint8_t cart_roml[0x2000], cart_romh[0x2000], cart_io[256];
    /* U13: the look covers the I/O page (CAPS3 b1): its bytes, and where it has no read view (ext) */
    int has_iolook;
    uint8_t io_view[0x1000], io_ext[0x1000];
    int shut, running, stop_new, fw_stop, dbg_stop, owner, memonly;
    int stops, runs, last_cmd, bus_cycles, cpu_writes;
    uint64_t cycles;
    uint32_t instrs, ms;
    uint32_t cyc_cmp;
    int cyc_cmp_en;
    uint32_t bmp[2048];
    int bmp_en;
    struct
    {
        uint32_t ctrl, reg, data;
        uint16_t lo, hi, pass, hits;
    } unit[2];
    /* the debug block's log action: the records waiting for hw_log_read, and whether the ring runs for them */
    int log_present;
    int log_n, log_rd;
    hw_logrec_t log[256];
    hw_stop_t rec;
    uint16_t announced;
    uint8_t prev_op;
    int irq_after; /* an interrupt is taken after this many more instructions (0: none) */
    /* the frame ring: ring_n entries, one per 19656 cycles, entry i at seq
     * 100 + i, its snap at CYCLES ring_c0 + i * 19656 */
    int ring_n, ring_landed, ring_commits, ring_lands;
    int ring_reu_off; /* A08: the firmware says the REU is on: no ring */
    uint64_t ring_c0;
    char ring_path[64]; /* the last path hw_ring_dump/save/load was given (T66) */
    char ring_entry[40]; /* the entry hw_ring_load_entry was given (T66 part 3) */
    int ring_native;     /* hw_ring_save_native was called (T66 part 6) */
    /* drive A */
    uint8_t dmem[65536];
    uint16_t dpc, drv_cmp;
    int drv_cmp_on;
    uint32_t dcycles;
    /* the video mode (T40): 1 = 60 Hz (65 x 263) */
    int hz60;
    /* the instruction trace: the history oldest first, the writes its records point at (stream 0) */
    int trace_present, trace_on, trace_gap;
    int trace_n;
    hw_trace_rec_t trace[4096];
    int writes_n;
    hw_trace_write_t writes[8192];
    uint64_t land_tc;  /* the timeline's count at the land and the machine's CYCLES then */
    uint64_t land_hw;
    /* trx->ui_request: how often it was asked, the callback queued, and whether the call exists */
    int ui_requests, ui_unsupported;
    int session;        /* a shadow session answers the window (T12 §14.8) */
    uint8_t cram[1024]; /* colour RAM, what the session's window reads */
    void (*ui_fn)(void *);
    void *ui_arg;
    int mid;           /* the CPU stopped inside an instruction (a STEP_INSTR finishes it, executing nothing more) */
    int held;          /* the CPU is held (RDY, bus DMA): STATUS b4 */
    int stuck;         /* the CPU executes a JAM opcode: no boundary comes, the cycles run on (the 6502N stays in decode) */
    int trace_replay;  /* STEP_CYCLES after a land puts PC and registers where the record of that cycle says */
} fake_t;

extern fake_t F;

void fake_reset(void);
/* The machine stops by itself at pc (a breakpoint, a unit, CYC_CMP) */
void fake_stop(uint16_t pc, int reason, int unit);
/* Runs up to n instructions while running, stopping as the debug block would */
void fake_tick(int n);
/* A CPU bus access (by the instruction at F.pc, registers as they are): the watch units that match it count a
 * hit, stop the machine (returns 1; the stop record names the address and data) or log. */
int fake_access(uint16_t addr, uint8_t data, int write);
/* Appends a record: its cycle, PC, opcode and operand bytes, the registers before, and the
 * writes it makes ({addr, old, new} triples) as consecutive stream 0 records */
void fake_trace_add(uint64_t cycle, uint16_t pc, uint8_t op, uint8_t b1, uint8_t b2, uint8_t a, uint8_t x, uint8_t y,
                    uint8_t sp, uint8_t p, int nwrites, const uint8_t (*w)[4]);

#endif
