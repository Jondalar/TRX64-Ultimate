// See trx_drive.h. The two stubs are assembled by hand below; every byte is
// written out with its address, so what the drive runs can be read here.
//
// The drive is never let run free while a stub is in its memory: it is held
// (CMD_DRV_PAUSE) and moved on with CMD_DRV_STEP, which runs the drive alone
// for a number of instructions whatever the C64 does (drv_dbg.v). So the save
// and the load work with the machine running, paused or frozen, and nothing
// the drive does between two steps can race the app's writes.
#include "trx_drive.h"

#if (U64 == 2) && !defined(RECOVERYAPP)

#include "trx_io.h"
#include "trx_6502.h"
#include "c1541.h"
#include "itu.h"

#include <stdio.h>
#include <string.h>

extern C1541 *c1541_A;

#define P3(o)          (TRX_IO_BASE + 0x3000 + (o))
#define DRV_CMD        P3(0x000)
#define DRV_ARG        P3(0x004)
#define DRV_STAT       P3(0x0C4)
#define DRV_CYCLES     P3(0x0C8)
#define CMD_DRV_PAUSE  0x10
#define CMD_DRV_RUN    0x11
#define CMD_DRV_STEP   0x12
#define ST_HALTED      0x01         // halted (the machine pause or the debugger)
#define ST_HELD        0x02         // held by the debugger
#define ST_STEPPING    0x04         // a step runs
#define CAPS_DBG       0x08

#define DRIVE_IMAGE    0x0EE0000u   // drive_unit.vhd g_ram_base of drive A
#define STUB_BASE      0x0100       // the stack page: below the drive's stack pointer it is dead
#define STUB_SIZE      0x90
#define STUB_NEED      (STUB_SIZE + 4)  // the stack pointer must be at least this high

static inline uint32_t r32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void w32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

// The drive's memory in DDR2: RAM is 2 K mirrored up to $1800, the ROM sits at
// its own address. The VIAs ($1800-$1FFF) are not in the image - that is what
// the stub is for.
static inline uint8_t dpeek(uint16_t a)
{
    return *(volatile uint8_t *)(DRIVE_IMAGE + (a < 0x1800 ? (a & 0x07FFu) : a));
}

static inline void dpoke(uint16_t a, uint8_t v)
{
    *(volatile uint8_t *)(DRIVE_IMAGE + (a < 0x1800 ? (a & 0x07FFu) : a)) = v;
}

static void dfence(void)
{
    // drive_mem.vhd drops its cached line when it sees a write of another
    // master on the system bus; the fence keeps the CPU's stores in front of
    // the step that lets the drive read them.
    __asm__ volatile("fence" ::: "memory");
}

// ---- the drive's run control ------------------------------------------------------
static bool drv_wait(uint32_t mask, uint32_t want, int ms)
{
    uint16_t t0 = getMsTimer();
    while (uint16_t(getMsTimer() - t0) < uint16_t(ms)) {
        if ((r32(DRV_STAT) & mask) == want) {
            return true;
        }
    }
    return (r32(DRV_STAT) & mask) == want;
}

static bool drv_hold(void)
{
    w32(DRV_CMD, CMD_DRV_PAUSE);
    return drv_wait(ST_HALTED, ST_HALTED, 50);
}

static void drv_free(void)
{
    w32(DRV_CMD, CMD_DRV_RUN);
}

// n instructions, with the C64 wherever it is. False when the step did not end.
static bool drv_step(int n)
{
    w32(DRV_ARG, uint32_t(n));
    w32(DRV_CMD, CMD_DRV_STEP);
    return drv_wait(ST_STEPPING, 0, 200);
}

static uint16_t drv_pc(void)
{
    return uint16_t(r32(DRV_STAT) >> 16);
}

// ---- the stubs --------------------------------------------------------------------
// Both stubs are 0x90 bytes: code first, then their data. They are written to
// $0100, the bottom of the stack page, which is dead for the program that runs
// on the drive as long as its stack pointer stays above them.
//
// The save stub, with its data at 68 A, 69 X, 6A Y, 6B S, 6C P, 6D MAGIC,
// 6E FLAG, 6F MAGIC2, 70..7F VIA 1, 80..8F VIA 2.
#define SV_A      0x68
#define SV_X      0x69
#define SV_Y      0x6A
#define SV_S      0x6B
#define SV_P      0x6C
#define SV_MAGIC  0x6D
#define SV_FLAG   0x6E
#define SV_MAGIC2 0x6F
#define SV_V1     0x70
#define SV_V2     0x80
#define SV_MARK   0x5A
#define SV_MARK2  0xA5

static const uint8_t save_stub[STUB_SIZE] = {
    /* 00 */ 0x08,                     // PHP
    /* 01 */ 0x8D, SV_A, 0x01,         // STA SAVE_A
    /* 04 */ 0x68,                     // PLA            (A = P)
    /* 05 */ 0x8D, SV_P, 0x01,         // STA SAVE_P
    /* 08 */ 0x78,                     // SEI
    /* 09 */ 0x8E, SV_X, 0x01,         // STX SAVE_X
    /* 0C */ 0x8C, SV_Y, 0x01,         // STY SAVE_Y
    /* 0F */ 0xBA,                     // TSX
    /* 10 */ 0x8E, SV_S, 0x01,         // STX SAVE_S
    /* 13 */ 0xAD, 0x0D, 0x18,         // LDA $180D      the IFRs first: reading the
    /* 16 */ 0x8D, SV_V1 + 13, 0x01,   // STA V1+13      others clears flags
    /* 19 */ 0xAD, 0x0D, 0x1C,         // LDA $1C0D
    /* 1C */ 0x8D, SV_V2 + 13, 0x01,   // STA V2+13
    /* 1F */ 0xA2, 0x00,               // LDX #$00
    /* 21 */ 0xE0, 0x0D,               // L1: CPX #$0D   (13 is read already)
    /* 23 */ 0xF0, 0x0C,               // BEQ skip
    /* 25 */ 0xBD, 0x00, 0x18,         // LDA $1800,X
    /* 28 */ 0x9D, SV_V1, 0x01,        // STA V1,X
    /* 2B */ 0xBD, 0x00, 0x1C,         // LDA $1C00,X
    /* 2E */ 0x9D, SV_V2, 0x01,        // STA V2,X
    /* 31 */ 0xE8,                     // skip: INX
    /* 32 */ 0xE0, 0x10,               // CPX #$10
    /* 34 */ 0xD0, 0xEB,               // BNE L1
    /* 36 */ 0xA9, SV_MARK,            // LDA #$5A
    /* 38 */ 0x8D, SV_MAGIC, 0x01,     // STA MAGIC      "the registers are there"
    /* 3B */ 0xAD, SV_FLAG, 0x01,      // park: LDA FLAG
    /* 3E */ 0xF0, 0xFB,               // BEQ park       (set when the app has read them)
    /* 40 */ 0xA9, 0x00,               // LDA #b0        the three bytes the JMP covered
    /* 42 */ 0x8D, 0x00, 0x00,         // STA next+0
    /* 45 */ 0xA9, 0x00,               // LDA #b1
    /* 47 */ 0x8D, 0x00, 0x00,         // STA next+1
    /* 4A */ 0xA9, 0x00,               // LDA #b2
    /* 4C */ 0x8D, 0x00, 0x00,         // STA next+2
    /* 4F */ 0xA9, SV_MARK2,           // LDA #$A5
    /* 51 */ 0x8D, SV_MAGIC2, 0x01,    // STA MAGIC2     "the code is back, I am leaving"
    /* 54 */ 0xAD, SV_P, 0x01,         // LDA SAVE_P
    /* 57 */ 0x48,                     // PHA
    /* 58 */ 0xAE, SV_X, 0x01,         // LDX SAVE_X
    /* 5B */ 0xAC, SV_Y, 0x01,         // LDY SAVE_Y
    /* 5E */ 0xAD, SV_A, 0x01,         // LDA SAVE_A
    /* 61 */ 0x28,                     // PLP
    /* 62 */ 0x4C, 0x00, 0x00,         // JMP next
    /* 65 */ 0, 0, 0,
    /* 68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
// where the app patches the save stub
#define SVP_B0     0x41
#define SVP_A0     0x43
#define SVP_B1     0x46
#define SVP_A1     0x48
#define SVP_B2     0x4B
#define SVP_A2     0x4D
#define SVP_JMP    0x63

// The load stub, with its data at 6D MAGIC, 70..7F VIA 1, 80..8F VIA 2.
#define LD_MAGIC   0x6D
#define LD_V1      0x70
#define LD_V2      0x80

static const uint8_t load_stub[STUB_SIZE] = {
    /* 00 */ 0xA9, 0x7F,               // LDA #$7F
    /* 02 */ 0x8D, 0x0E, 0x18,         // STA $180E      every interrupt off first, so
    /* 05 */ 0x8D, 0x0E, 0x1C,         // STA $1C0E      writing IER below only sets
    /* 08 */ 0xAD, LD_V1 + 11, 0x01,   // LDA V1+11      ACR and PCR before the timers
    /* 0B */ 0x8D, 0x0B, 0x18,         // STA $180B      are started
    /* 0E */ 0xAD, LD_V1 + 12, 0x01,   // LDA V1+12
    /* 11 */ 0x8D, 0x0C, 0x18,         // STA $180C
    /* 14 */ 0xAD, LD_V2 + 11, 0x01,   // LDA V2+11
    /* 17 */ 0x8D, 0x0B, 0x1C,         // STA $1C0B
    /* 1A */ 0xAD, LD_V2 + 12, 0x01,   // LDA V2+12
    /* 1D */ 0x8D, 0x0C, 0x1C,         // STA $1C0C
    /* 20 */ 0xA2, 0x00,               // LDX #$00
    /* 22 */ 0xE0, 0x0D,               // L1: CPX #$0D   (the IFR cannot be written)
    /* 24 */ 0xF0, 0x0C,               // BEQ skip
    /* 26 */ 0xBD, LD_V1, 0x01,        // LDA V1,X
    /* 29 */ 0x9D, 0x00, 0x18,         // STA $1800,X
    /* 2C */ 0xBD, LD_V2, 0x01,        // LDA V2,X
    /* 2F */ 0x9D, 0x00, 0x1C,         // STA $1C00,X
    /* 32 */ 0xE8,                     // skip: INX
    /* 33 */ 0xE0, 0x10,               // CPX #$10
    /* 35 */ 0xD0, 0xEB,               // BNE L1
    /* 37 */ 0xA9, 0x00,               // LDA #b         six bytes of the drive's own code
    /* 39 */ 0x8D, 0x00, 0x00,         // STA addr       back to what the file says
    /* 3C */ 0xA9, 0x00,               // LDA #b
    /* 3E */ 0x8D, 0x00, 0x00,         // STA addr
    /* 41 */ 0xA9, 0x00,               // LDA #b
    /* 43 */ 0x8D, 0x00, 0x00,         // STA addr
    /* 46 */ 0xA9, 0x00,               // LDA #b
    /* 48 */ 0x8D, 0x00, 0x00,         // STA addr
    /* 4B */ 0xA9, 0x00,               // LDA #b
    /* 4D */ 0x8D, 0x00, 0x00,         // STA addr
    /* 50 */ 0xA9, 0x00,               // LDA #b
    /* 52 */ 0x8D, 0x00, 0x00,         // STA addr
    /* 55 */ 0xA9, SV_MARK2,           // LDA #$A5
    /* 57 */ 0x8D, LD_MAGIC, 0x01,     // STA MAGIC      "the VIAs and the code are set"
    /* 5A */ 0xA2, 0x00,               // LDX #S
    /* 5C */ 0x9A,                     // TXS
    /* 5D */ 0xA9, 0x00,               // LDA #pc_hi
    /* 5F */ 0x48,                     // PHA
    /* 60 */ 0xA9, 0x00,               // LDA #pc_lo
    /* 62 */ 0x48,                     // PHA
    /* 63 */ 0xA9, 0x00,               // LDA #p
    /* 65 */ 0x48,                     // PHA
    /* 66 */ 0xA2, 0x00,               // LDX #x
    /* 68 */ 0xA0, 0x00,               // LDY #y
    /* 6A */ 0xA9, 0x00,               // LDA #a
    /* 6C */ 0x40,                     // RTI
    /* 6D */ 0, 0, 0,
    /* 70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
// where the app patches the load stub
#define LDP_SLOT0  0x37                // six slots of five bytes: LDA #b / STA addr
#define LDP_SLOT_N 6
#define LDP_SP     0x5B
#define LDP_PCH    0x5E
#define LDP_PCL    0x61
#define LDP_P      0x64
#define LDP_X      0x67
#define LDP_Y      0x69
#define LDP_A      0x6B

// ---- finding the place a JMP can go ----------------------------------------------
// The drive is halted at an instruction boundary, so the opcode at DPC is
// already in the CPU: a JMP goes behind that instruction, and only behind one
// whose successor is the next address. Steps the drive until that holds and
// both the instruction and the three bytes behind it are in RAM. Returns the
// address the JMP goes to, or 0.
static uint16_t find_patch(uint16_t *at_dpc, uint8_t *dpc_len)
{
    for (int tries = 0; tries < 400; tries++) {
        uint16_t pc = drv_pc();
        if (pc < 0x0800) {
            uint8_t op = dpeek(pc);
            uint8_t len = trx_op_len[op];
            uint16_t next = uint16_t(pc + len);
            if (trx_op_falls_through(op) && next + 3 <= 0x0800) {
                *at_dpc = pc;
                *dpc_len = len;
                return next;
            }
        }
        if (!drv_step(1)) {
            return 0;
        }
    }
    return 0;
}

bool trx_drive_present(void)
{
    return (trx_io_caps() & CAPS_DBG) && c1541_A && c1541_A->get_drive_power();
}

// ---- reading -----------------------------------------------------------------------
const char *trx_drive_read(TrxDriveState *st, bool with_regs)
{
    memset(st, 0, sizeof(*st));
    if (!(trx_io_caps() & CAPS_DBG)) {
        return "This bitstream has no debug page, so drive A cannot be read";
    }
    if (!c1541_A || !c1541_A->get_drive_power()) {
        return "Drive A is not powered";
    }
    for (int a = 0; a < TRX_DRIVE_RAM; a++) {
        st->ram[a] = dpeek(uint16_t(a));
    }
    st->cycles = r32(DRV_CYCLES);
    if (!with_regs) {
        return NULL;
    }
    if (!drv_hold()) {
        return "Drive A did not stop";
    }
    uint16_t dpc = 0, next = 0;
    uint8_t len = 0;
    next = find_patch(&dpc, &len);
    if (!next) {
        drv_free();
        printf("[DRIVE] drive A runs in its ROM, not in its own RAM: the file carries its RAM, not its "
               "CPU and VIA registers\n");
        return NULL;                // not an error: st->regs stays false
    }
    // what the stub overwrites, kept to put back
    uint8_t page[256], back[3];
    for (int i = 0; i < 256; i++) {
        page[i] = dpeek(uint16_t(STUB_BASE + i));
    }
    for (int i = 0; i < 3; i++) {
        back[i] = dpeek(uint16_t(next + i));
    }
    uint8_t stub[STUB_SIZE];
    memcpy(stub, save_stub, STUB_SIZE);
    stub[SVP_B0] = back[0];
    stub[SVP_B1] = back[1];
    stub[SVP_B2] = back[2];
    stub[SVP_A0] = uint8_t(next);
    stub[SVP_A0 + 1] = uint8_t(next >> 8);
    stub[SVP_A1] = uint8_t(next + 1);
    stub[SVP_A1 + 1] = uint8_t((next + 1) >> 8);
    stub[SVP_A2] = uint8_t(next + 2);
    stub[SVP_A2 + 1] = uint8_t((next + 2) >> 8);
    stub[SVP_JMP] = uint8_t(next);
    stub[SVP_JMP + 1] = uint8_t(next >> 8);
    for (int i = 0; i < STUB_SIZE; i++) {
        dpoke(uint16_t(STUB_BASE + i), stub[i]);
    }
    dpoke(next, 0x4C);
    dpoke(uint16_t(next + 1), uint8_t(STUB_BASE));
    dpoke(uint16_t(next + 2), uint8_t(STUB_BASE >> 8));
    dfence();

    // the drive runs only while it is stepped: nothing of its own can come
    // between the stub's writes and the app's reads
    const char *err = NULL;
    bool got = false;
    for (int i = 0; i < 40 && !got; i++) {
        if (!drv_step(200)) {
            err = "Drive A did not step";
            break;
        }
        got = dpeek(STUB_BASE + SV_MAGIC) == SV_MARK;
    }
    if (!err && !got) {
        err = "The stub on drive A did not finish";
    }
    if (!err) {
        st->a = dpeek(STUB_BASE + SV_A);
        st->x = dpeek(STUB_BASE + SV_X);
        st->y = dpeek(STUB_BASE + SV_Y);
        st->sp = dpeek(STUB_BASE + SV_S);
        st->p = dpeek(STUB_BASE + SV_P);
        st->pc = next;              // where the drive goes on when the stub hands back
        for (int i = 0; i < 16; i++) {
            st->via1[i] = dpeek(uint16_t(STUB_BASE + SV_V1 + i));
            st->via2[i] = dpeek(uint16_t(STUB_BASE + SV_V2 + i));
        }
        st->cycles = r32(DRV_CYCLES);
        st->regs = true;
        if (st->sp < STUB_NEED) {
            printf("[DRIVE] drive A's stack pointer was $%02X, below the stub's %d bytes: its stack was "
                   "written over and is put back, but the drive may have lost data\n", st->sp, STUB_SIZE);
        }
    }
    // let it out of the park loop and back into its own code
    dpoke(STUB_BASE + SV_FLAG, 1);
    dfence();
    bool back_out = false;
    for (int i = 0; i < 20 && !back_out; i++) {
        if (!drv_step(100)) {
            break;
        }
        back_out = dpeek(STUB_BASE + SV_MAGIC2) == SV_MARK2;
    }
    // the stub puts the three bytes back itself; if it never got there, the app does
    for (int i = 0; i < 3; i++) {
        dpoke(uint16_t(next + i), back[i]);
    }
    for (int i = 0; i < 256; i++) {
        dpoke(uint16_t(STUB_BASE + i), page[i]);
    }
    dfence();
    drv_free();
    if (!back_out && !err) {
        err = "Drive A did not come back out of the stub";
    }
    if (err) {
        st->regs = false;
    }
    return err;
}

// ---- writing -----------------------------------------------------------------------
const char *trx_drive_write(const TrxDriveState *st)
{
    if (!(trx_io_caps() & CAPS_DBG)) {
        return "This bitstream has no debug page, so drive A cannot be written";
    }
    if (!c1541_A || !c1541_A->get_drive_power()) {
        return "Drive A is not powered";
    }
    if (!drv_hold()) {
        return "Drive A did not stop";
    }
    uint16_t dpc = 0, next = 0;
    uint8_t len = 0;
    if (st->regs) {
        next = find_patch(&dpc, &len);
        if (!next) {
            drv_free();
            return "Drive A runs in its ROM: its registers cannot be put back (its RAM was not touched)";
        }
        if (st->sp < STUB_NEED) {
            drv_free();
            return "The file's drive stack pointer is too low for the stub";
        }
    }
    // The RAM first. The bytes of the instruction the drive has already
    // fetched keep their old value until it has run; the stub puts the file's
    // bytes there afterwards.
    uint8_t keep[3];
    for (int i = 0; i < len; i++) {
        keep[i] = dpeek(uint16_t(dpc + i));
    }
    for (int a = 0; a < TRX_DRIVE_RAM; a++) {
        dpoke(uint16_t(a), st->ram[a]);
    }
    if (!st->regs) {
        dfence();
        drv_free();
        return NULL;                // RAM only: the file had no registers
    }
    for (int i = 0; i < len; i++) {
        dpoke(uint16_t(dpc + i), keep[i]);
    }
    uint8_t stub[STUB_SIZE];
    memcpy(stub, load_stub, STUB_SIZE);
    for (int i = 0; i < 16; i++) {
        stub[LD_V1 + i] = st->via1[i];
        stub[LD_V2 + i] = st->via2[i];
    }
    // the six restore slots: the file's bytes where the app had to keep the
    // drive's own, and where the JMP went
    struct {
        uint16_t addr;
        uint8_t val;
    } fix[LDP_SLOT_N];
    int nf = 0;
    for (int i = 0; i < len && nf < LDP_SLOT_N; i++) {
        fix[nf].addr = uint16_t(dpc + i);
        fix[nf].val = st->ram[(dpc + i) & 0x07FF];
        nf++;
    }
    for (int i = 0; i < 3 && nf < LDP_SLOT_N; i++) {
        fix[nf].addr = uint16_t(next + i);
        fix[nf].val = st->ram[(next + i) & 0x07FF];
        nf++;
    }
    while (nf < LDP_SLOT_N) {       // the unused slots write the first one again
        fix[nf] = fix[0];
        nf++;
    }
    for (int i = 0; i < LDP_SLOT_N; i++) {
        int o = LDP_SLOT0 + 5 * i;
        stub[o + 1] = fix[i].val;
        stub[o + 3] = uint8_t(fix[i].addr);
        stub[o + 4] = uint8_t(fix[i].addr >> 8);
    }
    stub[LDP_SP] = st->sp;
    stub[LDP_PCH] = uint8_t(st->pc >> 8);
    stub[LDP_PCL] = uint8_t(st->pc);
    stub[LDP_P] = st->p;
    stub[LDP_X] = st->x;
    stub[LDP_Y] = st->y;
    stub[LDP_A] = st->a;
    stub[LD_MAGIC] = 0;
    for (int i = 0; i < STUB_SIZE; i++) {
        dpoke(uint16_t(STUB_BASE + i), stub[i]);
    }
    dpoke(next, 0x4C);
    dpoke(uint16_t(next + 1), uint8_t(STUB_BASE));
    dpoke(uint16_t(next + 2), uint8_t(STUB_BASE >> 8));
    dfence();

    const char *err = NULL;
    bool done = false;
    for (int i = 0; i < 40 && !done; i++) {
        if (!drv_step(200)) {
            err = "Drive A did not step";
            break;
        }
        done = dpeek(STUB_BASE + LD_MAGIC) == SV_MARK2;
    }
    if (!err && !done) {
        err = "The stub on drive A did not finish";
    }
    // the stack page below the file's stack pointer: the stub's bytes are dead
    // there, but the file's own belong back
    if (!err) {
        if (!drv_step(4)) {         // over the RTI, out of the stub
            err = "Drive A did not step";
        }
    }
    for (int i = 0; i < STUB_SIZE; i++) {
        dpoke(uint16_t(STUB_BASE + i), st->ram[(STUB_BASE + i) & 0x07FF]);
    }
    dfence();
    drv_free();
    return err;
}

#endif
