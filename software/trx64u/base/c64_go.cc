// c64_go.h. The order of the steps and every wait are MultiCore's c64_go.cc (t239.3 beee9b09); the waits poll in steps of one tick (sys.sleep_ms).
#include "c64_go.h"
#include "trx64u_lib.h"
#include <stdio.h>

// The registers are the driver table's (trx64u_lib.h): the state bus (trx_statebus), the debugger (trx_debug, trx_dbg_run) and the capabilities (ident).
// Each is 32 bit and one access (byte 3 commits a write, byte 0 latches a read: trx_sys.v); the callers hold the io group, so the core is handed over.

// words of a chip from the state bus; each read moves the address on
static void sb_read(int chip, int word, uint32_t *dst, int n)
{
    DRV_trx_statebus_addr_WR(((uint32_t)chip << 7) | (uint32_t)word);
    for (int i = 0; i < n; i++) {
        dst[i] = DRV_trx_statebus_data_RD();
    }
}

static bool available(void)
{
    uint8_t caps = DRV_ident_caps_RD();
    return (caps & DRV_ident_caps_statebus_MASK) && (caps & DRV_ident_caps_debug_MASK) && (DRV_ident_caps2_RD() & DRV_ident_caps2_cpu_write_MASK);
}

bool c64_go_available(void)
{
    const SvcIo *io = g_api->io;
    return io && available();
}

// The CPU's flip-flops as chip 1 w8-w14 carry them (cpu6510.v sb_cur; hw_trx.c in the monitor): 7:0 A, 15:8 X, 23:16 Y, 31:24 S, 39:32 P, 55:40 PC,
// 87:80 IR, 102+4:0 state, +12 copy_d2p, +15 flags_update, +16 trigger_upd, 137:130 din_r.
#define B_A      0
#define B_X      8
#define B_Y      16
#define B_S      24
#define B_P      32
#define B_PC     40
#define B_IR     80
#define B_CTRL   102
#define B_DIN    130

static void put_bits(uint32_t *w, int lo, int n, uint32_t v)
{
    for (int i = 0; i < n; i++) {
        int b = lo + i;
        w[b >> 5] = (w[b >> 5] & ~(1u << (b & 31))) | (((v >> i) & 1u) << (b & 31));
    }
}

// What the CPU sees at addr, by the PLA ($01 as read): RAM, a ROM through the aperture, or nothing it can read without a bus cycle (I/O).
static int peek_cpu(const SvcIo *io, uint8_t port, uint16_t a)
{
    bool loram = port & 1, hiram = port & 2, charen = port & 4;
    uint8_t b = 0;
    if (a >= 0xA000 && a < 0xC000 && loram && hiram) {
        io->rom_read(DRV_OFF(c64_roms, basic) + (a - 0xA000), &b, 1);
    } else if (a >= 0xD000 && a < 0xE000 && (loram || hiram)) {
        if (charen) {
            return -1;                                    // the chips: a read is a bus cycle with side effects
        }
        io->rom_read(DRV_OFF(c64_roms, char) + (a - 0xD000), &b, 1);
    } else if (a >= 0xE000 && hiram) {
        io->rom_read(DRV_OFF(c64_roms, kernal) + (a - 0xE000), &b, 1);
    } else {
        io->ram_read(a, &b, 1);
    }
    return b;
}

static void give_back(void)
{
    DRV_trx_debug_cmd_WR(DRV_trx_debug_cmd_run);
    DRV_trx_dbg_run_owner_WR(0);
}

#define STOP_WAIT_MS 500
#define LOAD_WAIT_MS 1000

int c64_go(uint16_t pc, c64_go_place_t place, void *user, char *msg, int msglen)
{
    const SvcIo *io = g_api->io;
    if (!io || !available()) {
        snprintf(msg, msglen, "this bitstream cannot set the CPU's address (no CPU write path)");
        return 1;
    }
    DRV_trx_dbg_run_owner_WR(1);                               // the debugger owns run control; the cartridge page's stop is not ours now
    DRV_trx_debug_cmd_WR(DRV_trx_debug_cmd_pause_ib);
    uint32_t t0 = now_ms();
    uint32_t st;
    const uint32_t at_ib = DRV_trx_debug_status_shut_MASK | DRV_trx_debug_status_at_ib_MASK;
    while (((st = DRV_trx_debug_status_RD()) & at_ib) != at_ib) {
        if (now_ms() - t0 > STOP_WAIT_MS) {
            give_back();
            snprintf(msg, msglen, "the CPU did not stop on an instruction boundary (status %08X)", (unsigned)st);
            return 2;
        }
        g_api->sys->sleep_ms(1);
    }

    if (place) {
        int rc = place(user, msg, msglen);
        if (rc) {
            give_back();
            return rc;
        }
    }

    // the registers as the pending update leaves them (w4/w5) and the port ($01 as read, w2): both only meaningful at an IB with the gate shut
    uint32_t w0[6];
    sb_read(1, 0, w0, 6);
    uint8_t port = (uint8_t)(w0[2] >> 16);
    int op = peek_cpu(io, port, pc);
    if (op < 0) {
        give_back();
        snprintf(msg, msglen, "$%04X is in the I/O area: the opcode there cannot be read without a bus cycle", pc);
        return 3;
    }
    uint8_t a = (uint8_t)w0[4], x = (uint8_t)(w0[4] >> 8), y = (uint8_t)(w0[4] >> 16), s = (uint8_t)(w0[4] >> 24);
    uint8_t p = (uint8_t)w0[5];
    p &= (uint8_t)~0x0C;                                       // a program is started with interrupts allowed and binary arithmetic

    uint32_t w[7];
    sb_read(1, 8, w, 7);
    put_bits(w, B_A, 8, a);
    put_bits(w, B_X, 8, x);
    put_bits(w, B_Y, 8, y);
    put_bits(w, B_S, 8, s);
    put_bits(w, B_P, 8, p);
    put_bits(w, B_IR, 8, 0xEA);                                // the pending write-back of the previous instruction is taken out:
    put_bits(w, B_CTRL + 12, 1, 0);                            // IR = NOP, copy_d2p, flags_update, trigger_upd off
    put_bits(w, B_CTRL + 15, 1, 0);
    put_bits(w, B_CTRL + 16, 1, 0);
    put_bits(w, B_PC, 16, pc);                                 // the fetch's address; the update adds 1
    put_bits(w, B_DIN, 8, (uint32_t)op);                       // what the fetch read
    DRV_trx_statebus_status_WR(0);                             // clear "refused"
    DRV_trx_statebus_addr_WR((1u << 7) | 8u);
    for (int k = 0; k < 7; k++) {
        DRV_trx_statebus_data_WR(w[k]);
    }
    if (DRV_trx_statebus_status_RD() & DRV_trx_statebus_status_refused_MASK) {
        give_back();
        snprintf(msg, msglen, "the state bus refused the staged CPU words (the gate is not shut)");
        return 4;
    }
    DRV_trx_statebus_load_WR(DRV_trx_statebus_load_cpu_MASK);  // the CPU, at the next opening edge
    DRV_trx_debug_ipc_set_WR(DRV_trx_debug_ipc_set_valid_MASK | ((uint32_t)op << DRV_trx_debug_ipc_set_op_SHIFT) |
                             ((uint32_t)pc << DRV_trx_debug_ipc_set_pc_SHIFT));

    DRV_trx_debug_cmd_WR(DRV_trx_debug_cmd_run);
    t0 = now_ms();
    while ((DRV_trx_statebus_load_RD() & DRV_trx_statebus_load_cpu_MASK) || (DRV_trx_debug_status_RD() & DRV_trx_debug_status_shut_MASK)) {
        if (now_ms() - t0 > LOAD_WAIT_MS) {
            DRV_trx_dbg_run_owner_WR(0);
            snprintf(msg, msglen, "the machine did not take the new address in time (the load is still armed)");
            return 5;
        }
        g_api->sys->sleep_ms(1);
    }
    DRV_trx_dbg_run_owner_WR(0);                               // the cartridge page's stop is the menu's again
    return 0;
}
