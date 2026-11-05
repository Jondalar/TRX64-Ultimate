/*
 * fake_hw.c — hw.h for the host tests: a machine that does not execute, but
 * keeps registers, memory and the debug block's run state the way the
 * monitor sees them. A step moves PC by the instruction's length; a stop
 * while running is made by the test (fake_stop).
 */
#include <stdio.h>
#include <string.h>

#include "../src/dis6502.h"
#include "../src/hw.h"
#include "trx_ea.h"
#include "fake_hw.h"

fake_t F;

static void record(int reason)
{
    if (F.stop_new) {
        return;
    }
    F.stop_new = 1;
    F.rec.reason = (uint8_t)reason;
    F.rec.flags = 1; /* at an IB */
    F.rec.ipc = F.pc;
    F.rec.addr = F.pc;
    F.rec.pos = (100 << 7) | 12;
    F.stops++;
}

static int bmp_hit(uint16_t pc)
{
    return F.bmp_en && ((F.bmp[pc >> 5] >> (pc & 31)) & 1);
}

static uint16_t rom_vec(uint16_t a) { return (uint16_t)(F.rom[0x2000 + (a & 0x1FFF)] | (F.rom[0x2000 + ((a + 1) & 0x1FFF)] << 8)); }

static void push3(uint16_t ret)
{
    F.ram[0x100 + F.s] = (uint8_t)(ret >> 8);
    F.ram[0x100 + (uint8_t)(F.s - 1)] = (uint8_t)ret;
    F.ram[0x100 + (uint8_t)(F.s - 2)] = F.p;
    F.s = (uint8_t)(F.s - 3);
}

/* One instruction: only JSR, RTS, JMP, BRK and RTI move the PC anywhere but on; an interrupt can be due */
static void exec1(void)
{
    uint8_t op = F.ram[F.pc];
    if (((op & 0x0F) == 0x02) && ((op >> 4) != 0x8) && ((op >> 4) != 0xA) && ((op >> 4) != 0xC) && ((op >> 4) != 0xE)) {
        F.stuck = 1;
        F.cycles += 1000;
        return;
    }
    F.stuck = 0;
    uint16_t t = (uint16_t)(F.ram[(uint16_t)(F.pc + 1)] | (F.ram[(uint16_t)(F.pc + 2)] << 8));
    F.prev_op = op;
    if (op == 0x20) {
        uint16_t r = (uint16_t)(F.pc + 2);
        F.ram[0x100 + F.s] = (uint8_t)(r >> 8);
        F.ram[0x100 + (uint8_t)(F.s - 1)] = (uint8_t)r;
        F.s = (uint8_t)(F.s - 2);
        F.pc = t;
    } else if (op == 0x60) {
        F.s = (uint8_t)(F.s + 2);
        F.pc = (uint16_t)((F.ram[0x100 + (uint8_t)(F.s - 1)] | (F.ram[0x100 + F.s] << 8)) + 1);
    } else if (op == 0x4C) {
        F.pc = t;
    } else if (op == 0x00) {
        push3((uint16_t)(F.pc + 2));
        F.pc = rom_vec(0x1FFE + 0xE000);
    } else if (op == 0x40) {
        F.p = F.ram[0x100 + (uint8_t)(F.s + 1)];
        uint16_t r = (uint16_t)(F.ram[0x100 + (uint8_t)(F.s + 2)] | (F.ram[0x100 + (uint8_t)(F.s + 3)] << 8));
        F.s = (uint8_t)(F.s + 3);
        F.pc = r;
    } else {
        F.pc = (uint16_t)(F.pc + dis_len(op));
    }
    if (F.irq_after && (--F.irq_after == 0)) {
        push3(F.pc);
        F.pc = rom_vec(0x1FFE + 0xE000);
    }
    F.cycles += 2;
    F.instrs++;
}

static int unit_hit(int n)
{
    uint32_t c = F.unit[n].ctrl;
    if (!(c & U_ENABLE) || ((c & 0x0E) != U_T_IB) || (F.pc < F.unit[n].lo) || (F.pc > F.unit[n].hi)) {
        return 0;
    }
    if ((c & U_PREV_RET) && (F.prev_op != 0x60) && (F.prev_op != 0x40)) {
        return 0;
    }
    uint32_t reg = F.unit[n].reg;
    if ((reg & 7) == R_SEL_S && ((reg >> 8) & 3) == R_OP_GE && F.s < ((reg >> 16) & 0xFF)) {
        return 0;
    }
    if (F.unit[n].hits++ < F.unit[n].pass) {
        return 0;
    }
    if (((c >> 5) & 3) == 2) {
        if (F.log_n < 256) {
            hw_logrec_t *r = &F.log[F.log_n++];
            r->addr = F.pc;
            r->data = F.ram[F.pc];
            r->unit = (uint8_t)n;
            r->write = 0;
            r->ib = 1;
            r->ipc = F.pc;
            r->cyc15 = (uint16_t)(F.cycles & 0x7FFF);
        }
        return 0;
    }
    if (c & U_ONESHOT) {
        F.unit[n].ctrl &= ~(uint32_t)U_ENABLE;
    }
    return 1;
}

void fake_tick(int n)
{
    for (int i = 0; F.running && (i < n); i++) {
        exec1();
        if (F.cyc_cmp_en && ((uint32_t)F.cycles > F.cyc_cmp)) {
            F.cyc_cmp_en = 0;
            F.running = 0;
            F.shut = 1;
            record(RS_CYC_CMP);
        } else if (bmp_hit(F.pc)) {
            F.running = 0;
            F.shut = 1;
            record(RS_BITMAP);
        } else {
            for (int u = 0; u < 2; u++) {
                if (unit_hit(u)) {
                    F.running = 0;
                    F.shut = 1;
                    record(RS_UNIT_ADDR);
                    F.rec.unit = (uint8_t)u;
                    break;
                }
            }
        }
    }
}

void fake_reset(void)
{
    memset(&F, 0, sizeof(F));
    F.ring_landed = -1;
    F.shut = 1;
    F.fw_stop = 1;
    F.s = 0xF6;
    F.p = 0x24;
    F.pc = 0xC000;
    F.ddr = 0x2F;
    F.latch = 0x37;
    F.port = 0x37;
    F.game = 1;
    F.exrom = 1;
    for (int i = 0; i < 0x2000; i++) {
        F.rom[0x2000 + i] = (uint8_t)(i & 0xFF); /* KERNAL */
    }
    F.rom[0x2000 + 0x1FFE] = 0x48; /* $FFFE/F -> $FF48 */
    F.rom[0x2000 + 0x1FFF] = 0xFF;
    F.rom[0x2000 + 0x1FFA] = 0x43;
    F.rom[0x2000 + 0x1FFB] = 0xFE;
    F.vic[0x11] = 0x1B;
    F.vic[0x12] = 0x30;
    F.vic[0x18] = 0x15;
    F.vic[0x20] = 0xFE;
    F.vic[0x21] = 0xF6;
    F.cia2[0] = 0x97;
    F.cia2_pa = 0x97;
    F.cia2_ddra = 0x3F;
}

void fake_stop(uint16_t pc, int reason, int unit)
{
    F.pc = pc;
    F.running = 0;
    F.shut = 1;
    F.cycles += 1000;
    F.instrs += 300;
    record(reason);
    F.rec.unit = (uint8_t)unit;
}

int hw_init(const void *trx_api) { (void)trx_api; return 1; }
uint32_t hw_caps(void) { return 0xFF; }

uint32_t hw_status(void)
{
    return (F.shut ? ST_SHUT : 0) | ST_AT_IB | (F.stop_new ? ST_STOP_NEW : 0) | (F.fw_stop ? ST_FW_STOP : 0) | (F.held ? ST_HELD : 0) |
           (F.dbg_stop ? ST_DBG_STOP : 0) |
           ((uint32_t)F.rec.reason << 8) | (F.owner ? ST_OWNER : 0) | ((uint32_t)(F.stops & 0xFF) << 24);
}

void hw_cmd(uint8_t cmd, uint16_t arg)
{
    F.last_cmd = cmd;
    switch (cmd) {
    case CMD_PAUSE_CB:
    case CMD_PAUSE_IB:
        if (F.stuck && F.running && (cmd == CMD_PAUSE_IB)) {
            break; /* no instruction boundary ever comes */
        }
        if (F.running) {
            F.running = 0;
            F.shut = 1;
            F.cycles += 3;
        }
        record(1);
        if (F.stuck && (F.rec.flags & 1)) {
            F.rec.flags = 0; /* a cycle boundary */
        }
        break;
    case CMD_RUN:
        F.running = 1;
        F.shut = 0;
        F.stop_new = 0;
        F.runs++;
        break;
    case CMD_STEP_INSTR: {
        F.stop_new = 0;
        int n = arg ? arg : 1;
        int reason = 2;
        for (int i = 0; i < n; i++) {
            if (F.mid) {
                F.mid = 0; /* the instruction already begun ends */
                continue;
            }
            exec1();
            if (bmp_hit(F.pc)) {
                reason = 3;
                break;
            }
        }
        record(reason);
        break;
    }
    case CMD_STEP_CYCLES:
        F.stop_new = 0;
        F.cycles += arg ? arg : 1;
        if (F.trace_replay && (F.ring_landed >= 0)) {
            uint64_t tc = F.land_tc + (F.cycles - F.land_hw);
            for (int i = 0; i < F.trace_n; i++) {
                if (F.trace[i].cycle == tc) {
                    F.pc = F.trace[i].pc;
                    F.a = F.trace[i].a;
                    F.x = F.trace[i].x;
                    F.y = F.trace[i].y;
                    F.s = F.trace[i].sp;
                    F.p = F.trace[i].p;
                }
            }
        }
        record(2);
        F.rec.flags = 0;
        break;
    case CMD_DRV_STEP: {
        F.stop_new = 0;
        int n = arg ? arg : 1;
        for (int i = 0; i < n; i++) {
            F.dpc = (uint16_t)(F.dpc + dis_len(F.dmem[F.dpc]));
            F.dcycles += 2;
        }
        record(RS_DRV_STEP);
        break;
    }
    case CMD_CLEAR_STOP: F.stop_new = 0; break;
    default: break;
    }
}

void hw_owner(int on) { F.owner = on; }
uint64_t hw_cycles(void) { return F.cycles; }
uint32_t hw_instrs(void) { return F.instrs; }
void hw_cyc_cmp(uint32_t at) { F.cyc_cmp = at; F.cyc_cmp_en = 1; }
void hw_cyc_cmp_off(void) { F.cyc_cmp_en = 0; }
void hw_stop_record(hw_stop_t *r) { *r = F.rec; }

void hw_cpu(hw_cpu_t *c)
{
    memset(c, 0, sizeof(*c));
    c->a = F.a;
    c->x = F.x;
    c->y = F.y;
    c->s = F.s;
    c->p = F.p;
    c->ipc = F.pc;
    c->pc = (uint16_t)(F.pc + 1);
    c->op = F.ram[F.pc];
    c->at_ib = !F.mid;
    c->settled = !F.mid;
    c->ddr = F.ddr;
    c->latch = F.latch;
    c->port = F.port;
}

static uint32_t pack(const uint8_t *b, int word)
{
    return (uint32_t)b[4 * word] | ((uint32_t)b[4 * word + 1] << 8) | ((uint32_t)b[4 * word + 2] << 16) |
           ((uint32_t)b[4 * word + 3] << 24);
}

uint32_t hw_sb(int chip, int word)
{
    switch (chip) {
    case 0: return 0x01 | ((uint32_t)F.game << 1) | ((uint32_t)F.exrom << 2) | ((uint32_t)F.cart << 3);
    case 1:
        if (word == 2) {
            return F.ddr | ((uint32_t)F.latch << 8) | ((uint32_t)F.port << 16);
        }
        return 0;
    case 2: return (word < 12) ? pack(F.vic, word) : 0;
    case 3: return (word < 4) ? pack(F.cia1, word) : 0;
    case 4:
        if (word == 4) {
            return F.cia2_pa | ((uint32_t)F.cia2_ddra << 16);
        }
        return (word < 4) ? pack(F.cia2, word) : 0;
    default: return 0;
    }
}

void hw_sb_block(int chip, int word, uint32_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = hw_sb(chip, word + i);
    }
}

void hw_bmp_word(int idx, uint32_t bits) { F.bmp[idx & 2047] = bits; }
void hw_bmp_enable(int on) { F.bmp_en = on; }

void hw_unit(int n, uint32_t ctrl, uint16_t lo, uint16_t hi, uint32_t data, uint32_t reg, uint16_t pass)
{
    F.unit[n].ctrl = ctrl;
    F.unit[n].lo = lo;
    F.unit[n].hi = hi;
    F.unit[n].reg = reg;
    F.unit[n].data = data;
    F.unit[n].pass = pass;
    F.unit[n].hits = 0;
}

void hw_unit_off(int n) { F.unit[n].ctrl = 0; }
uint32_t hw_hits(void) { return (uint32_t)F.unit[0].hits | ((uint32_t)F.unit[1].hits << 16); }

/* trx_dbg.v: the register qualifier (reg & RMASK) ROP RVAL */
static int reg_ok(uint32_t reg)
{
    int sel = reg & 7;
    if (!sel) {
        return 1;
    }
    uint8_t v = sel == R_SEL_A ? F.a : sel == R_SEL_X ? F.x : sel == R_SEL_Y ? F.y : sel == R_SEL_S ? F.s : F.p;
    uint8_t m = (uint8_t)(reg >> 24), rv = (uint8_t)(reg >> 16);
    int op = (reg >> 8) & 3;
    uint8_t t = v & m;
    return op == R_OP_EQ ? t == rv : op == R_OP_NE ? t != rv : op == R_OP_LT ? t < rv : t >= rv;
}

int fake_access(uint16_t addr, uint8_t data, int write)
{
    int stopped = 0;
    for (int u = 0; u < 2; u++) {
        uint32_t c = F.unit[u].ctrl;
        int type = (c >> 1) & 7;
        if (!(c & U_ENABLE) || (c & U_RASTER) || (type < 2) || (type > 4)) {
            continue;
        }
        if (((type == 2) && write) || ((type == 3) && !write)) {
            continue;
        }
        if ((addr < F.unit[u].lo) || (addr > F.unit[u].hi)) {
            continue;
        }
        uint32_t d = F.unit[u].data;
        if (((data ^ (d & 0xFF)) & ((d >> 8) & 0xFF)) || !reg_ok(F.unit[u].reg)) {
            continue;
        }
        uint16_t n = F.unit[u].hits++;
        if (n < F.unit[u].pass) {
            continue;
        }
        int action = (c >> 5) & 3;
        if (action == 2) {
            if (F.log_n < 256) {
                hw_logrec_t *r = &F.log[F.log_n++];
                r->addr = addr;
                r->data = data;
                r->unit = (uint8_t)u;
                r->write = (uint8_t)write;
                r->ib = 0;
                r->ipc = F.pc;
                r->cyc15 = (uint16_t)(F.cycles & 0x7FFF);
            }
        } else if (action == 0 && !stopped && F.running) {
            F.running = 0;
            F.shut = 1;
            record(RS_UNIT_ADDR);
            F.rec.unit = (uint8_t)u;
            F.rec.addr = addr;
            F.rec.data = data;
            F.rec.flags = 0; /* an access: mid-instruction */
            stopped = 1;
        }
        if (c & U_ONESHOT) {
            F.unit[u].ctrl &= ~(uint32_t)U_ENABLE;
        }
    }
    return stopped;
}

int hw_log_present(void) { return F.log_present; }

int hw_log_read(hw_logrec_t *out, int max, uint32_t *lost)
{
    *lost = 0;
    if (max <= 0) {
        F.log_rd = F.log_n;
        return 0;
    }
    int n = 0;
    while ((F.log_rd < F.log_n) && (n < max)) {
        out[n++] = F.log[F.log_rd++];
    }
    return n;
}
uint32_t hw_drv_stat(void) { return ((uint32_t)F.dpc << 16) | (F.drv_cmp_on ? 8u : 0u) | 3u; }
int hw_drive_peek(uint16_t a) { return ((a >= 0x1800) && (a < 0x2000)) ? -1 : F.dmem[a]; }
uint32_t hw_drv_cycles(void) { return F.dcycles; }
void hw_drv_cmp(uint16_t a, int on)
{
    F.drv_cmp = a;
    F.drv_cmp_on = on;
}

void hw_ram_read(uint16_t a, uint8_t *d, int n)
{
    for (int i = 0; i < n; i++) {
        d[i] = F.ram[(uint16_t)(a + i)];
    }
}

void hw_ram_write(uint16_t a, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        F.ram[(uint16_t)(a + i)] = s[i];
    }
}

uint8_t hw_rom(uint16_t off) { return (off < sizeof(F.rom)) ? F.rom[off] : 0xFF; }

/* The lens unit: a cartridge of 8K ROML and 8K ROMH ($FF in the look's open bus below
 * the image), IO1 a register, a real-port flag; REAL needs a held machine */
int hw_has_lk(void) { return F.has_lk; }

uint32_t hw_lk(uint16_t a, int cls, int real)
{
    F.lk_calls++;
    if (!F.has_lk) {
        return 0;
    }
    uint32_t off = a & 0x1FFFu;
    if (real) {
        if (!F.shut) {
            return LK_DONE | LK_REFUSED;
        }
        F.bus_cycles++;
        uint8_t v = (cls == 0) ? F.cart_roml[off] : (cls == 1) ? F.cart_romh[off] : F.cart_io[a & 0xFF];
        return LK_DONE | LK_HIT | LK_REAL | v;
    }
    if (F.cart_ext) {
        return LK_DONE | LK_EXT | 0xFF;
    }
    if (cls == 0) {
        return LK_DONE | LK_HIT | F.cart_roml[off];
    }
    if (cls == 1) {
        return LK_DONE | LK_HIT | F.cart_romh[off];
    }
    return LK_DONE | LK_HIT | F.cart_io[a & 0xFF];
}

uint8_t hw_bus_read(uint16_t a)
{
    F.bus_cycles++;
    return (a >= 0xD400 && a < 0xD800) ? 0x42 : 0xFF;
}

int hw_session_read(uint16_t a) { return (F.session && F.shut && (F.fw_stop || F.dbg_stop)) ? (0xF0 | (F.cram[a & 0x3FF] & 0x0F)) : -1; }
int hw_bus_held(void) { return F.shut && (F.fw_stop || F.dbg_stop); }
int hw_bus_read_held(uint16_t a) { return hw_bus_held() ? hw_bus_read(a) : -1; }

void hw_bus_write(uint16_t a, uint8_t v)
{
    F.bus_cycles++;
    if (a >= 0xD000 && a < 0xD030) {
        F.vic[a - 0xD000] = v;
    }
}

int hw_acquire(void)
{
    F.owner = 1;
    record(1); /* PAUSE_CB while shut */
    if (F.stuck && (F.rec.flags & 1)) {
        F.rec.flags = 0; /* a stuck CPU stopped by the menu: a cycle boundary */
    }
    return 0;
}
void hw_release(void) { F.owner = 0; }
void hw_announce(uint16_t port) { F.announced = port; }
__attribute__((weak)) int hw_detach(void (*fn)(void *), void *arg)
{
    (void)fn;
    (void)arg;
    return -6; /* the host server serves inline */
}
__attribute__((weak)) int hw_stop_requested(void) { return 0; }
__attribute__((weak)) int hw_ui_request(void (*fn)(void *), void *arg)
{
    F.ui_requests++;
    F.ui_fn = fn;
    F.ui_arg = arg;
    return F.ui_unsupported ? -6 : 0;
}

/* The CPU write path: the fake applies at once (the hardware at the next edge) */
int hw_has_cpu_write(void) { return 1; }

int hw_cpu_write(const hw_regs_t *r, int pc, uint8_t op)
{
    if (!F.shut) {
        return -1;
    }
    F.a = r->a;
    F.x = r->x;
    F.y = r->y;
    F.s = r->s;
    F.p = r->p;
    if (pc >= 0) {
        F.pc = (uint16_t)pc;
        (void)op;
    }
    F.cpu_writes++;
    return 0;
}

int hw_cpu_write_pending(void) { return 0; }

void hw_ipc_set(uint16_t ipc, uint8_t op, int at_ib)
{
    (void)op;
    (void)at_ib;
    F.pc = ipc;
}

void hw_cpu_fetch(int *in_fetch, int *intg, uint16_t *pc, uint8_t *din)
{
    *in_fetch = 1;
    *intg = 0;
    *pc = F.pc;
    *din = F.ram[F.pc];
}

/* The frame ring: a land puts PC at $C000 + index and the CYCLES counter goes
 * on (as the board's: a land costs machine time it does not report) */
int hw_ring_present(void) { return F.ring_n > 0; }

int hw_ring_status(hw_ring_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!F.ring_n) {
        return -1;
    }
    st->present = 1;
    st->on = !F.ring_reu_off;
    st->reu_off = F.ring_reu_off;
    st->frames = F.ring_n;
    st->targets = F.ring_n;
    st->oldest_seq = 100;
    st->head_seq = 100 + (uint32_t)F.ring_n - 1;
    st->landed = F.ring_landed >= 0;
    st->landed_seq = F.ring_landed >= 0 ? 100 + (uint32_t)F.ring_landed : 0;
    return 0;
}

int hw_ring_frame(int index, hw_ring_frame_t *f)
{
    if ((index < 0) || (index >= F.ring_n)) {
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->seq = 100 + (uint32_t)index;
    f->cycles = (uint64_t)index * 19656u;
    f->c24 = (uint32_t)(F.ring_c0 + f->cycles) & 0xFFFFFF;
    f->line = 0;
    f->x = 120;
    f->target = 1;
    return 0;
}

int hw_ring_find(uint32_t seq)
{
    int i = (int)seq - 100;
    return ((i >= 0) && (i < F.ring_n)) ? i : -1;
}

const char *hw_ring_land(int index)
{
    if ((index < 0) || (index >= F.ring_n)) {
        return "no such frame";
    }
    F.cycles += 777;
    F.land_tc = (uint64_t)index * 19656u;
    F.land_hw = F.cycles;
    F.pc = (uint16_t)(0xC000 + index);
    F.shut = 1;
    F.running = 0;
    F.ring_landed = index;
    F.ring_lands++;
    F.stop_new = 0;
    record(RS_CYC_CMP);
    F.rec.flags = 0;
    return 0;
}

void hw_ring_commit(void)
{
    if (F.ring_landed >= 0) {
        F.ring_n = F.ring_landed + 1;
        F.ring_c0 = 0; /* not modelled further */
        F.ring_landed = -1;
        F.ring_commits++;
    }
}

const char *hw_ring_dump(const char *path, int index, uint32_t *bytes)
{
    (void)index;
    snprintf(F.ring_path, sizeof(F.ring_path), "%s", path);
    *bytes = 1234;
    return 0;
}

const char *hw_ring_save(const char *path, int seconds, int stride, uint32_t *bytes, int *entries)
{
    snprintf(F.ring_path, sizeof(F.ring_path), "%s", path);
    *bytes = 99999;
    *entries = seconds * 50 / stride;
    return 0;
}

const char *hw_ring_save_native(const char *path, uint32_t *bytes)
{
    snprintf(F.ring_path, sizeof(F.ring_path), "%s", path);
    F.ring_native = 1;
    if (bytes) {
        *bytes = 4242424;
    }
    return 0;
}

const char *hw_ring_load(const char *path)
{
    snprintf(F.ring_path, sizeof(F.ring_path), "%s", path);
    F.running = 1;
    F.shut = 0;
    return 0;
}

const char *hw_ring_load_entry(const char *path, const char *entry)
{
    snprintf(F.ring_entry, sizeof(F.ring_entry), "%s", entry ? entry : "");
    return hw_ring_load(path);
}

/* The video mode */
void hw_video(hw_video_t *v)
{
    v->hz60 = F.hz60;
    v->lines = F.hz60 ? 263 : 312;
    v->cycles_per_line = F.hz60 ? 65 : 63;
    v->frame_cycles = F.hz60 ? 17095 : 19656;
    v->cpu_hz = F.hz60 ? 1022727 : 985248;
    v->fps_x100 = F.hz60 ? 5983 : 5012;
}

/* The instruction trace: F.trace is the history, F.writes stream 0 by physical index */
void fake_trace_add(uint64_t cycle, uint16_t pc, uint8_t op, uint8_t b1, uint8_t b2, uint8_t a, uint8_t x, uint8_t y,
                    uint8_t sp, uint8_t p, int nwrites, const uint8_t (*w)[4])
{
    hw_trace_rec_t *r = &F.trace[F.trace_n++];
    r->cycle = cycle;
    r->write_start = (uint32_t)F.writes_n;
    r->pc = pc;
    r->write_count = (uint16_t)nwrites;
    r->a = a;
    r->x = x;
    r->y = y;
    r->sp = sp;
    r->p = p;
    r->opcode = op;
    r->b1 = b1;
    r->b2 = b2;
    for (int i = 0; i < nwrites; i++) {
        F.writes[F.writes_n].addr = (uint16_t)(w[i][0] | (w[i][1] << 8));
        F.writes[F.writes_n].old_value = w[i][2];
        F.writes[F.writes_n].new_value = w[i][3];
        F.writes_n++;
    }
}

int hw_trace_present(void) { return F.trace_present; }

int hw_trace_status(hw_trace_status_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!F.trace_present) {
        return -1;
    }
    st->present = 1;
    st->on = F.trace_on;
    st->gap = F.trace_gap;
    st->records = (uint32_t)F.trace_n;
    st->capacity = 349461;
    if (F.trace_n) {
        st->oldest_cycle = F.trace[0].cycle;
        st->newest_cycle = F.trace[F.trace_n - 1].cycle;
    }
    return 0;
}

int hw_trace_ctl(int on)
{
    if (!F.trace_present) {
        return -1;
    }
    if (on >= 0) {
        F.trace_on = on;
    }
    return F.trace_on;
}

int hw_trace_read(uint32_t first, uint32_t n, hw_trace_rec_t *out)
{
    int got = 0;
    for (; (got < (int)n) && (first + (uint32_t)got < (uint32_t)F.trace_n); got++) {
        out[got] = F.trace[first + (uint32_t)got];
    }
    return got;
}

int hw_trace_writes(uint32_t start, uint32_t n, hw_trace_write_t *out)
{
    int got = 0;
    for (; (got < (int)n) && (start + (uint32_t)got < (uint32_t)F.writes_n); got++) {
        out[got] = F.writes[start + (uint32_t)got];
    }
    return got;
}

int hw_trace_whowrote(uint16_t addr, int limit, hw_trace_hit_t *hits, int *exhausted)
{
    int found = 0;
    if ((addr >= 0xD000) && (addr <= 0xDFFF)) {
        /* as the firmware: an I/O address is found from the instruction records (trx_ea.h) */
        int unres = 0;
        for (int i = F.trace_n - 1; (i >= 0) && (found < limit); i--) {
            const hw_trace_rec_t *r = &F.trace[i];
            trx_store_t st;
            int k = trx_store_ea(r->opcode, r->b1, r->b2, r->a, r->x, r->y, &st);
            if (k == TRX_EA_UNRESOLVED) {
                unres = 1;
            } else if ((k == TRX_EA_KNOWN) && (st.ea == addr)) {
                hw_trace_hit_t *h = &hits[found++];
                memset(h, 0, sizeof(*h));
                h->cycle = r->cycle;
                h->addr = addr;
                h->pc = r->pc;
                h->new_value = st.new_known ? st.new_value : 0;
                h->flags = (uint16_t)(HW_HIT_OLD_UNKNOWN | (st.new_known ? 0 : HW_HIT_NEW_UNKNOWN));
                h->index = (uint32_t)i;
            }
        }
        *exhausted = (found < limit ? HW_WHO_EXHAUSTED : 0) | (unres ? HW_WHO_UNRESOLVED : 0);
        return found;
    }
    for (int i = F.trace_n - 1; (i >= 0) && (found < limit); i--) {
        for (int w = F.trace[i].write_count - 1; (w >= 0) && (found < limit); w--) {
            const hw_trace_write_t *r = &F.writes[F.trace[i].write_start + (uint32_t)w];
            if (r->addr == addr) {
                hw_trace_hit_t *h = &hits[found++];
                memset(h, 0, sizeof(*h));
                h->cycle = F.trace[i].cycle;
                h->addr = addr;
                h->pc = F.trace[i].pc;
                h->old_value = r->old_value;
                h->new_value = r->new_value;
                h->index = (uint32_t)i;
            }
        }
    }
    *exhausted = found < limit ? HW_WHO_EXHAUSTED : 0;
    return found;
}
