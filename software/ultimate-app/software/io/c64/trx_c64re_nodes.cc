// See trx_c64re_nodes.h.
//
// Every bit position below is read off the gateware, not off a comment:
//
//  chip 1 words 8-14 (209 bits), cpu6510.v:247
//      sb_cur = {nmi_r, irq_r, rdy_r, fall7, fall6, pfloat, pout_i, pdir, pin_r, din_r, core_cur}
//    core_cur (130 bits), proc_core.vhd:271 - sb_cur(101 downto 0) <= index_carry & sb_cur_r
//      bits 100..0  proc_registers.vhd:484
//          pc_carry_d & pc_carry_i & so_d & adh_clash & ready_d1 & h_reg & i_reg & dreg
//          & adh & adl & pch & pcl & p_reg & s_reg & y_reg & x_reg & a_reg
//      bit 101      index_carry
//      bits 123..102 proc_control.vhd:587
//          vect_reg & a16 & intg & vectoring & trig_sl & flags_update & irq_inhibit
//          & dummy_cycle & copy_d2p & latch_dreg & rwn & dout_mux(3) & a_mux(2) & state(5)
//      bits 129..124 proc_interrupt.vhd:60
//          resetting & nmi_act & nmi_d2 & nmi_d1 & nmi_c & irq_c
//
//  chips 3 and 4 words 8-19 (363 bits), c64_redip_cia.v:112 and cia_core.sv:157ff
//      [1:0] {res, phi2_prev}  [7:2] two edge detectors  [42:8] ports
//      [82:43] timer A  [122:83] timer B  [243:123] TOD  [286:244] serial
//      [316:287] interrupt  [326:317] control A  [336:327] control B  [362:337] TOD counter
//    ports (cia_pkg.sv ports_t = {pra, prb, ddra, ddrb}, cia_ports.sv:125 {prb_rw, ports})
//    timer (cia_timer.sv:137 {..., counter, prescaler}, prescaler = the latch)
//    TOD   (cia_tod.sv:290 {..., h12, clock, alarm}, tod_t = {10ths, sec, min, hr})
//    interrupt (cia_interrupt.sv:171 {w_mask_prev, mask, ..., flags_prev, irq_prev,
//               icr7_prev, icr_prev, ir_set_prev, sources_prev})
//    control (cia_control.sv:126 {cnt_prev, cr_w_prev, cr})
//
// Which of them a load even uses is the subset of ARCH-lean-state-access.md
// §15.9: the port registers, both timer latches, the ICR mask and the TOD alarm
// go in as bus writes (trx_ring.cc replay_cia reads them out of these same
// bits), the rest is staged.
#include "trx_c64re_nodes.h"

#include <string.h>

#define W_GLUE         0
#define W_CPU          1
#define W_VIC          4
#define W_CIA1        16
#define W_CIA2        21
#define W_CART        26
#define W_CPUH        27
#define W_CIA1H       34
#define W_CIA2H       46
#define GLUE_CART   0x08         // chip 0 word 0 bit 3: a cartridge is on

// chip 1 words 8-14
#define CH_A           0
#define CH_X           8
#define CH_Y          16
#define CH_S          24
#define CH_P          32
#define CH_PCL        40
#define CH_PCH        48
#define CH_DREG       72
#define CH_IREG       80
#define CH_VECTORING 119
#define CH_INTG      120
#define CH_NMI_D2    127
#define CH_NMI_ACT   128
#define CH_RESETTING 129
#define CH_DIN       130
#define CH_PDIR      144
#define CH_POUT      152

// chips 3 and 4 words 8-19
#define CI_DDRB        8
#define CI_DDRA       16
#define CI_PRB        24
#define CI_PRA        32
#define CI_TA_LATCH   43
#define CI_TA_CNT     59
#define CI_TB_LATCH   83
#define CI_TB_CNT     99
#define CI_ALARM      123        // {hour, min, sec, 10ths}, a byte each
#define CI_CLOCK     155
#define CI_SDR       244
#define CI_ICR       293         // the ICR as it reads (8 bits)
#define CI_FLAGS     303         // the latched interrupt flags (5 bits)
#define CI_MASK      311         // the ICR mask (5 bits)
#define CI_CRA       317
#define CI_CRB       327

static void set_bits(uint32_t *w, int lo, int n, uint32_t v)
{
    for (int b = 0; b < n; b++) {
        int i = (lo + b) / 32, s = (lo + b) % 32;
        w[i] = (w[i] & ~(1u << s)) | (((v >> b) & 1u) << s);
    }
}

static void set_byte(uint32_t *w, int word0, int reg, uint8_t v)
{
    int i = word0 + reg / 4, s = 8 * (reg & 3);
    w[i] = (w[i] & ~(0xFFu << s)) | (uint32_t(v) << s);
}

// ---- the CIAs --------------------------------------------------------------------
// A CiaSnapshot node (TRX64 c64re_snapshot.rs; what write_cia in trx_c64re.cc
// emits) into the register images and the hidden words of one CIA.
static bool cia_from_node(const char *js, const jsmntok_t *t, int n, int node, uint32_t *w, int wr, int wh)
{
    if (node < 0) {
        return false;
    }
    uint8_t c[16];
    memset(c, 0, sizeof(c));
    int got = tok_any_bytes(js, t, n, json_key(js, t, n, node, "c_cia"), c, 16);
    if (got < 16) {
        return false;
    }
    uint32_t ta_l = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "ta_latch"), c[4] | (c[5] << 8)));
    uint32_t tb_l = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "tb_latch"), c[6] | (c[7] << 8)));
    uint32_t ta_c = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "ta_cnt"), ta_l));
    uint32_t tb_c = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "tb_cnt"), tb_l));
    uint32_t ifr = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "irqflags"), 0)) & 0xFF;
    uint8_t alarm[4] = { 0, 0, 0, 0 };
    tok_any_bytes(js, t, n, json_key(js, t, n, node, "todalarm"), alarm, 4);

    uint32_t *h = w + wh;
    // the four port registers and both timer latches: these go in as bus
    // writes, and replay_cia takes them from exactly these bits
    set_bits(h, CI_DDRB, 8, c[3]);
    set_bits(h, CI_DDRA, 8, c[2]);
    set_bits(h, CI_PRB, 8, c[1]);
    set_bits(h, CI_PRA, 8, c[0]);
    set_bits(h, CI_TA_LATCH, 16, ta_l);
    set_bits(h, CI_TB_LATCH, 16, tb_l);
    // staged: the counters, the control registers, the serial register, the
    // interrupt latch and its mask, the TOD clock and alarm
    set_bits(h, CI_TA_CNT, 16, ta_c);
    set_bits(h, CI_TB_CNT, 16, tb_c);
    set_bits(h, CI_CRA, 8, c[14]);
    set_bits(h, CI_CRB, 8, c[15]);
    set_bits(h, CI_SDR, 8, c[12]);
    set_bits(h, CI_FLAGS, 5, ifr & 0x1F);
    set_bits(h, CI_ICR, 8, ifr);
    set_bits(h, CI_MASK, 5, c[13] & 0x1F);
    set_bits(h, CI_ALARM + 0, 8, alarm[3]);     // hour
    set_bits(h, CI_ALARM + 8, 8, alarm[2]);     // minute
    set_bits(h, CI_ALARM + 16, 8, alarm[1]);    // second
    set_bits(h, CI_ALARM + 24, 8, alarm[0]);    // tenth
    set_bits(h, CI_CLOCK + 0, 8, c[11]);
    set_bits(h, CI_CLOCK + 8, 8, c[10]);
    set_bits(h, CI_CLOCK + 16, 8, c[9]);
    set_bits(h, CI_CLOCK + 24, 8, c[8]);

    // the register images (what a dump of this machine would show again)
    for (int r = 0; r < 16; r++) {
        set_byte(w, wr, r, c[r]);
    }
    set_byte(w, wr, 4, uint8_t(ta_c));
    set_byte(w, wr, 5, uint8_t(ta_c >> 8));
    set_byte(w, wr, 6, uint8_t(tb_c));
    set_byte(w, wr, 7, uint8_t(tb_c >> 8));
    set_byte(w, wr, 13, uint8_t(ifr));
    return true;
}

// ---- the VIC ---------------------------------------------------------------------
// regs[0x40] plus the three latch bytes the restore compares against
// ($D019 in chip 2 w6 byte 1, $D01E/$D01F w7 bytes 2/3).
static bool vic_from_node(const char *js, const jsmntok_t *t, int n, int node, uint32_t *w, int *line, int *cycle)
{
    if (node < 0) {
        return false;
    }
    uint8_t regs[0x40];
    memset(regs, 0, sizeof(regs));
    int got = tok_any_bytes(js, t, n, json_key(js, t, n, node, "regs"), regs, 0x40);
    if (got < 0x30) {
        return false;
    }
    *line = int(tok_int_or(js, t, json_key(js, t, n, node, "raster_line"), -1));
    *cycle = int(tok_int_or(js, t, json_key(js, t, n, node, "raster_cycle"), -1));
    for (int r = 0; r <= 0x2E; r++) {
        set_byte(w, W_VIC, r, regs[r]);
    }
    uint32_t irq = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "irq_status"), regs[0x19])) & 0x0F;
    uint32_t ss = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "sprite_sprite_collisions"), 0)) & 0xFF;
    uint32_t sb = uint32_t(tok_int_or(js, t, json_key(js, t, n, node, "sprite_background_collisions"), 0)) & 0xFF;
    set_byte(w, W_VIC, 0x19, uint8_t(irq | (irq ? 0x80 : 0)));
    set_byte(w, W_VIC, 0x1E, uint8_t(ss));
    set_byte(w, W_VIC, 0x1F, uint8_t(sb));
    return true;
}

// ---- the cartridge ---------------------------------------------------------------
int trx_c64re_cart_ram(const char *js, const jsmntok_t *t, int n, int cp, uint8_t *out, int len)
{
    int cs = json_key(js, t, n, cp, "cartState");
    if (cs < 0 || t[cs].type != JSMN_OBJECT) {
        return 0;
    }
    return tok_any_bytes(js, t, n, json_key(js, t, n, cs, "cartRam"), out, len);
}

uint32_t trx_c64re_cart_flash_len(const char *js, const jsmntok_t *t, int n, int cp)
{
    return tok_ta_len(js, t, n, json_key(js, t, n, cp, "cartFlash"));
}

uint32_t trx_c64re_cart_flash(const char *js, const jsmntok_t *t, int n, int cp, uint8_t *out, uint32_t len)
{
    return uint32_t(tok_ta_upto(js, t, n, json_key(js, t, n, cp, "cartFlash"), out, int(len)));
}

int trx_c64re_cart_type(const char *js, const jsmntok_t *t, int n, int cp)
{
    int cs = json_key(js, t, n, cp, "cartState");
    if (cs < 0 || t[cs].type != JSMN_OBJECT) {
        return -1;
    }
    return int(tok_int_or(js, t, json_key(js, t, n, cs, "cartridgeType"), -1));
}

// cartState into the block: the banking registers the replay writes back
// (trx_ring.cc replay_cart reads chip 7 word 0) and the "a cartridge is on"
// bit the replay asks for first.
static bool cart_from_node(const char *js, const jsmntok_t *t, int n, int cp, uint32_t *w)
{
    int cs = json_key(js, t, n, cp, "cartState");
    if (cs < 0 || t[cs].type != JSMN_OBJECT) {
        return false;
    }
    int bank = json_key(js, t, n, cs, "currentBank");
    int ctrl = json_key(js, t, n, cs, "controlRegister");
    if (bank < 0 && ctrl < 0) {
        return false;
    }
    uint32_t r0 = uint32_t(tok_int_or(js, t, bank, 0)) & 0xFF;
    uint32_t r1 = uint32_t(tok_int_or(js, t, ctrl, 0)) & 0xFF;
    w[W_CART] = (w[W_CART] & 0xFFFF0000u) | r0 | (r1 << 8);
    w[W_GLUE] |= GLUE_CART;
    return true;
}

// ---- drive A -----------------------------------------------------------------------
bool trx_c64re_drive_from_node(const char *js, const jsmntok_t *t, int n, int cp, TrxDriveState *st)
{
    int dn = json_key(js, t, n, cp, "trx64Drive");
    if (dn < 0 || t[dn].type != JSMN_OBJECT) {
        return false;
    }
    memset(st, 0, sizeof(*st));
    if (tok_any_bytes(js, t, n, json_key(js, t, n, dn, "ram"), st->ram, TRX_DRIVE_RAM) != TRX_DRIVE_RAM) {
        return false;
    }
    st->cycles = uint32_t(tok_int_or(js, t, json_key(js, t, n, dn, "cycles"), 0));
    if (!tok_true(js, t, json_key(js, t, n, dn, "regs"))) {
        return true;                // RAM only: the stub could not be entered when it was written
    }
    if (tok_any_bytes(js, t, n, json_key(js, t, n, dn, "via1"), st->via1, 16) != 16 ||
        tok_any_bytes(js, t, n, json_key(js, t, n, dn, "via2"), st->via2, 16) != 16) {
        return true;
    }
    st->a = uint8_t(tok_int_or(js, t, json_key(js, t, n, dn, "a"), 0));
    st->x = uint8_t(tok_int_or(js, t, json_key(js, t, n, dn, "x"), 0));
    st->y = uint8_t(tok_int_or(js, t, json_key(js, t, n, dn, "y"), 0));
    st->sp = uint8_t(tok_int_or(js, t, json_key(js, t, n, dn, "sp"), 0xFF));
    st->p = uint8_t(tok_int_or(js, t, json_key(js, t, n, dn, "p"), 0x20));
    int pc = int(tok_int_or(js, t, json_key(js, t, n, dn, "pc"), -1));
    if (pc < 0 || pc > 0xFFFF) {
        return true;
    }
    st->pc = uint16_t(pc);
    st->regs = true;
    return true;
}

// ---- the whole checkpoint --------------------------------------------------------
const char *trx_c64re_from_nodes(const char *js, const jsmntok_t *t, int n, int cp, TrxNodeState *st)
{
    st->have = 0;
    st->line = st->cycle = -1;
    st->keys_pressed = 0;
    if (cp < 0) {
        return "No checkpoint in the file";
    }
    int cpu = json_key(js, t, n, cp, "cpu");
    if (cpu < 0) {
        return "No cpu node";
    }
    int ram = json_key(js, t, n, cp, "ram");
    if (!tok_ta(js, t, n, ram, st->ram, 65536)) {
        return "No ram node of 65536 bytes";
    }
    st->have |= TRX_NODE_RAM;

    int pc = int(tok_int_or(js, t, json_key(js, t, n, cpu, "pc"), -1));
    if (pc < 0 || pc > 0xFFFF) {
        return "The cpu node has no pc";
    }
    uint8_t a = uint8_t(tok_int_or(js, t, json_key(js, t, n, cpu, "a"), 0));
    uint8_t xr = uint8_t(tok_int_or(js, t, json_key(js, t, n, cpu, "x"), 0));
    uint8_t yr = uint8_t(tok_int_or(js, t, json_key(js, t, n, cpu, "y"), 0));
    uint8_t sp = uint8_t(tok_int_or(js, t, json_key(js, t, n, cpu, "sp"), 0xFF));
    uint8_t p = uint8_t(tok_int_or(js, t, json_key(js, t, n, cpu, "flags"), 0x20));
    uint32_t *h = st->w + W_CPUH;
    set_bits(h, CH_A, 8, a);
    set_bits(h, CH_X, 8, xr);
    set_bits(h, CH_Y, 8, yr);
    set_bits(h, CH_S, 8, sp);
    set_bits(h, CH_P, 8, p);
    set_bits(h, CH_PCL, 8, uint32_t(pc) & 0xFF);
    set_bits(h, CH_PCH, 8, uint32_t(pc) >> 8);
    // The sequencer state is the base's: every snap is in the opcode fetch
    // cycle, so the opcode at the file's pc is fetched from the bus after the
    // load and i_reg / dreg / din_r are written before they are read. What must
    // not come from the base is an interrupt it was about to take: the file's
    // own IRQ sources are in the CIAs loaded with it, and the CPU sees them
    // again by itself.
    set_bits(h, CH_VECTORING, 1, 0);
    set_bits(h, CH_INTG, 1, 0);
    set_bits(h, CH_NMI_D2, 1, 0);
    set_bits(h, CH_NMI_ACT, 1, 0);
    set_bits(h, CH_RESETTING, 1, 0);
    st->w[W_CPU] = uint32_t(a) | (uint32_t(xr) << 8) | (uint32_t(yr) << 16) | (uint32_t(sp) << 24);
    st->w[W_CPU + 1] = (st->w[W_CPU + 1] & 0xFF000000u) | uint32_t(pc) | (uint32_t(p) << 16);
    st->have |= TRX_NODE_CPU;

    int dirn = json_key(js, t, n, cp, "cpuPortDirection");
    int valn = json_key(js, t, n, cp, "cpuPortValue");
    if (dirn >= 0 && valn >= 0) {
        uint8_t dir = uint8_t(tok_int(js, t, dirn)), val = uint8_t(tok_int(js, t, valn));
        set_bits(h, CH_PDIR, 8, dir);
        set_bits(h, CH_POUT, 8, val);
        // $01 as read: the output bits as driven, the input bits as the C64
        // pulls them (b0-b2 the ROM lines, b4 the cassette sense switch open)
        uint8_t rd = uint8_t((val & dir) | (0x17 & ~dir));
        st->w[W_CPU + 2] = uint32_t(dir) | (uint32_t(val) << 8) | (uint32_t(rd) << 16);
        st->have |= TRX_NODE_PORT;
    }

    if (cia_from_node(js, t, n, json_key(js, t, n, cp, "cia1"), st->w, W_CIA1, W_CIA1H)) {
        st->have |= TRX_NODE_CIA1;
    }
    if (cia_from_node(js, t, n, json_key(js, t, n, cp, "cia2"), st->w, W_CIA2, W_CIA2H)) {
        st->have |= TRX_NODE_CIA2;
    }

    int vic = json_key(js, t, n, cp, "vic");
    if (vic_from_node(js, t, n, vic, st->w, &st->line, &st->cycle)) {
        st->have |= TRX_NODE_VIC;
    }
    // as TRX64's own restore refuses it
    if (st->line >= 312 || st->cycle >= 63) {
        return "The file's VIC position is outside 312 x 63";
    }
    // Colour RAM: the vic node carries it. A file without it keeps what the
    // machine has (TRX64 writes it always).
    int cr = json_key(js, t, n, vic, "color_ram");
    if (tok_any_bytes(js, t, n, cr, st->col, 1024) == 1024) {
        for (int i = 0; i < 1024; i++) {
            st->col[i] &= 0x0F;
        }
        st->have |= TRX_NODE_COLOR;
    } else {
        memset(st->col, 0, 1024);
    }
    if (cart_from_node(js, t, n, cp, st->w)) {
        st->have |= TRX_NODE_CART;
    }
    if (st->cart && trx_c64re_cart_ram(js, t, n, cp, st->cart, TRX_CART_RAM_BYTES) == TRX_CART_RAM_BYTES) {
        st->have |= TRX_NODE_CARTRAM;
    } else {
        st->cart = NULL;            // nothing to put back
    }
    int kb = json_key(js, t, n, cp, "keyboard");
    int lp = json_key(js, t, n, kb, "livePressed");
    if (lp >= 0 && t[lp].type == JSMN_ARRAY) {
        st->keys_pressed = t[lp].size;
    }
    return NULL;
}
