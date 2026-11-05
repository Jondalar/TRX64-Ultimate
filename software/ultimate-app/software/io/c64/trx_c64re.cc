// .c64re / .c64rering writer and .c64re loader for the frame ring. See
// trx_c64re.h. The gzip, SHA-256 and JSON pieces follow apps/c64re/src/c64re.c
// (the same container, written from a .u2a there).
//
// Node mapping (T20 §9.3), from a timeline entry's chip block (c64_core.v map)
// and the RAM / colour RAM the ring materializes for it:
//   cpu        A X Y S P PC from chip 1 w0/w1; cycles = the ring's phi2 count
//   ram        64 KB, $D800-$DBFF the colour nibbles (TRX64's own captures do
//              this: its VIC reads colour from there); the RAM under the I/O
//              rides in trx64Hw.ramUnderIo
//   cpuPort*   $00 and the $01 latch (chip 1 w2)
//   cia1/cia2  v2 nodes: registers as read, latches, mask and alarm from the
//              hidden words, timer states from CRA/CRB (partial)
//   vic        registers as read, raster position of the snap, colour RAM;
//              the internal counters and draw pipeline cold (partial)
//   cartBytes  null (TRX64 detaches a cartridge), no sid, drive or REU node
//   trx64Hw    {layout, x, line, block (58 words LE), ramUnderIo, colorRam}:
//              everything trx_c64re_load() needs to put the board back exactly

#include "trx_c64re.h"
#include "trx_ring.h"
#include "trx_io.h"
#include "filemanager.h"
#include "c1541.h"
#include "c64_crt.h"
#include "trx_cart.h"
#include "itu.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trx_sha256.h"
#include "trx_json.h"
#include "trx_inflate.h"
#include "trx_c64re_nodes.h"
#include "trx_trxring.h"
#include "trx_drive.h"

extern C1541 *c1541_A;    // drive A, for the "media" and "drivePart" nodes

#define LAYOUT         1
#define PC_ADJ         0     // chip 1 w1's PC at the snap edge minus the next opcode's address
#define W_CPU          1
#define W_VIC          4
#define W_CIA1         16
#define W_CIA2         21
#define W_CIA1H        34
#define W_CIA2H        46
#define W_CPUH         27
#define W_GLUE          0
#define W_CART         26
#define GLUE_CART    0x08    // chip 0 word 0 bit 3: a cartridge is on (c64_core.v s_cart)

// ---- gzip with stored blocks, written as the document is produced ----------------
#define GZ_BLOCK 16384

// The rvlite core reads DDR2 without a cache (about a microsecond per load),
// so the hot paths keep their state in locals: one load per input byte, the
// CRC bitwise in registers, stores (posted) into the block.
static inline uint32_t crc8(uint32_t c)
{
    for (int k = 0; k < 8; k++) {
        c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}

static inline char b64c(uint32_t v)
{
    v &= 63;
    return v < 26 ? char('A' + v) : v < 52 ? char('a' + v - 26) : v < 62 ? char('0' + v - 52) : v == 62 ? '+' : '/';
}

class Gz {
    File *f;
    uint8_t *buf;
    int fill;
    uint32_t crc, isize;
    bool hash;
public:
    TrxSha256 sha;
    bool failed;
    uint32_t written;
    uint32_t write_ms = 0;

    Gz(File *file, bool with_hash) : f(file), fill(0), crc(0xFFFFFFFFu), isize(0), hash(with_hash), failed(false), written(0)
    {
        trx_sha_init(&sha);
        buf = (uint8_t *)malloc(GZ_BLOCK);
        failed = !buf;
        static const uint8_t header[10] = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0x03 };
        raw(header, sizeof(header));
    }
    ~Gz() { free(buf); }

    void raw(const uint8_t *p, uint32_t len)
    {
        if (failed) {
            return;
        }
        if (hash) {
            trx_sha_add(&sha, p, len);
        }
        uint32_t tr = 0;
        uint16_t t = getMsTimer();
        if (f->write(p, len, &tr) != FR_OK || tr != len) {
            failed = true;
        }
        write_ms += uint16_t(getMsTimer() - t);
        written += len;
    }

    void flush(bool final)
    {
        uint8_t head[5];
        head[0] = final ? 1 : 0;
        head[1] = uint8_t(fill);
        head[2] = uint8_t(fill >> 8);
        head[3] = uint8_t(~fill);
        head[4] = uint8_t((~fill) >> 8);
        raw(head, 5);
        if (fill) {
            raw(buf, fill);
        }
        fill = 0;
    }

    void put(const char *s, int len)
    {
        uint32_t c = crc;
        int fl = fill;
        uint8_t *const b = buf;
        for (int i = 0; i < len; i++) {
            uint8_t ch = uint8_t(s[i]);
            c = crc8(c ^ ch);
            b[fl++] = ch;
            if (fl == GZ_BLOCK) {
                fill = fl;
                flush(false);
                fl = 0;
            }
        }
        crc = c;
        fill = fl;
        isize += len;
    }

    void str(const char *s) { put(s, strlen(s)); }

    // s as the body of a JSON string (without the quotes)
    void jstr(const char *s)
    {
        char esc[8];
        for (const char *p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') {
                esc[0] = '\\';
                esc[1] = char(c);
                put(esc, 2);
            } else if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
                esc[4] = hex[c >> 4]; esc[5] = hex[c & 15];
                put(esc, 6);
            } else {
                put((const char *)p, 1);
            }
        }
    }

    void num(const char *prefix, int64_t v)
    {
        char tmp[24];
        int n = 0;
        if (prefix) {
            str(prefix);
        }
        uint64_t u = v < 0 ? uint64_t(-v) : uint64_t(v);
        do {                            // the printf here has no 64-bit conversions
            tmp[n++] = char('0' + u % 10);
            u /= 10;
        } while (u);
        if (v < 0) {
            tmp[n++] = '-';
        }
        char out[24];
        for (int i = 0; i < n; i++) {
            out[i] = tmp[n - 1 - i];
        }
        put(out, n);
    }

    // a whole array as base64, '=' padded
    void b64(const uint8_t *data, int len)
    {
        uint32_t c = crc;
        int fl = fill;
        uint8_t *const b = buf;
        int outn = 0;
        for (int i = 0; i < len; i += 3) {
            int rem = len - i;
            uint32_t v = uint32_t(data[i]) << 16;
            if (rem > 1) {
                v |= uint32_t(data[i + 1]) << 8;
            }
            if (rem > 2) {
                v |= data[i + 2];
            }
            char o[4] = { b64c(v >> 18), b64c(v >> 12), rem > 1 ? b64c(v >> 6) : '=', rem > 2 ? b64c(v) : '=' };
            for (int k = 0; k < 4; k++) {
                c = crc8(c ^ uint8_t(o[k]));
                b[fl++] = uint8_t(o[k]);
                if (fl == GZ_BLOCK) {
                    fill = fl;
                    flush(false);
                    fl = 0;
                }
            }
            outn += 4;
        }
        crc = c;
        fill = fl;
        isize += outn;
    }

    // len bytes as lower-case hex (a sha256 in the manifest)
    void hex(const uint8_t *data, int len)
    {
        static const char h[] = "0123456789abcdef";
        char out[64];
        int on = 0;
        for (int i = 0; i < len; i++) {
            out[on++] = h[data[i] >> 4];
            out[on++] = h[data[i] & 15];
            if (on == int(sizeof(out))) {
                put(out, on);
                on = 0;
            }
        }
        if (on) {
            put(out, on);
        }
    }

    void ta(const uint8_t *data, int len)
    {
        str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
        b64(data, len);
        str("\"}");
    }

    // The same from an open file, in 3 KB pieces (a multiple of 3, so every
    // piece is a whole number of base64 groups): a cartridge or a disk image
    // goes into the document without a copy of it in memory (T66 part 5).
    // A read that fails leaves the array short; failed is set.
    void file_ta(File *f, uint32_t len)
    {
        str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
        uint8_t piece[3072];
        uint32_t done = 0;
        while (done < len && !failed) {
            uint32_t want = len - done;
            if (want > sizeof(piece)) {
                want = sizeof(piece);
            }
            uint32_t got = 0;
            if (f->read(piece, want, &got) != FR_OK || got != want) {
                failed = true;
                break;
            }
            b64(piece, int(want));
            done += want;
        }
        str("\"}");
    }

    void array(const uint8_t *data, int count, int mask)
    {
        char out[4 * 64 + 4];
        int on = 0;
        put("[", 1);
        for (int i = 0; i < count; i++) {
            int v = data[i] & mask;
            if (i) {
                out[on++] = ',';
            }
            if (v >= 100) {
                out[on++] = char('0' + v / 100);
            }
            if (v >= 10) {
                out[on++] = char('0' + (v / 10) % 10);
            }
            out[on++] = char('0' + v % 10);
            if (on > 4 * 64) {
                put(out, on);
                on = 0;
            }
        }
        out[on++] = ']';
        put(out, on);
    }

    bool finish(uint8_t *digest)
    {
        flush(true);
        crc ^= 0xFFFFFFFFu;
        uint8_t tail[8];
        for (int i = 0; i < 4; i++) {
            tail[i] = uint8_t(crc >> (8 * i));
            tail[4 + i] = uint8_t(isize >> (8 * i));
        }
        raw(tail, 8);
        if (digest) {
            trx_sha_final(&sha, digest);
        }
        return !failed;
    }
};

// ---- the checkpoint --------------------------------------------------------------
static uint32_t bits(const uint32_t *w, int lo, int n)
{
    uint32_t v = 0;
    for (int b = 0; b < n; b++) {
        v |= ((w[(lo + b) / 32] >> ((lo + b) % 32)) & 1u) << b;
    }
    return v;
}

static uint8_t wbyte(const uint32_t *w, int word0, int reg)
{
    return uint8_t(w[word0 + reg / 4] >> (8 * (reg & 3)));
}

static int timer_state(uint8_t cr)
{
    if (!(cr & 1)) {
        return (cr & 8) ? 0x008 : 0;
    }
    if (cr & 0x60) {            // counts CNT or timer A: no phi2 input
        return 0x001 | (cr & 8);
    }
    return 0x863 | ((cr & 8) ? 0x1108 : 0);
}

static void write_cia(Gz &g, const char *name, const uint32_t *w, int wr, int wh, uint64_t cycles)
{
    const uint32_t *h = w + wh;
    uint8_t rd[16], c[16];
    for (int i = 0; i < 16; i++) {
        rd[i] = wbyte(w, wr, i);
        c[i] = rd[i];
    }
    uint32_t ta = bits(h, 43, 16), tb = bits(h, 83, 16);
    c[0] = bits(h, 32, 8);
    c[1] = bits(h, 24, 8);
    c[2] = bits(h, 16, 8);
    c[3] = bits(h, 8, 8);
    c[4] = ta & 0xFF; c[5] = ta >> 8;
    c[6] = tb & 0xFF; c[7] = tb >> 8;
    c[13] = bits(h, 311, 5);
    g.str("\"");
    g.str(name);
    g.str("\":{\"v\":2,\"c_cia\":");
    g.array(c, 16, 0xFF);
    g.num(",\"irqflags\":", rd[13] & 0x1F);
    g.str(",\"ack_irqflags\":0,\"new_irqflags\":0");
    g.num(",\"irq_enabled\":", (rd[13] & c[13] & 0x1F) ? 1 : 0);
    g.str(",\"rdi\":0,\"ifr_clock\":0,\"ifr_delay\":0,\"tat\":0,\"tbt\":0");
    g.num(",\"ta_state\":", timer_state(rd[14]));
    g.num(",\"ta_latch\":", ta);
    g.num(",\"ta_cnt\":", rd[4] | rd[5] << 8);
    g.num(",\"ta_clk\":", cycles);
    g.num(",\"tb_state\":", timer_state(rd[15]));
    g.num(",\"tb_latch\":", tb);
    g.num(",\"tb_cnt\":", rd[6] | rd[7] << 8);
    g.num(",\"tb_clk\":", cycles);
    g.str(",\"sr_bits\":0,\"sdr_valid\":0,\"sdr_force_finish\":0,\"shifter\":0,\"sdr_delay\":0");
    g.str(",\"sp_in_state\":0,\"cnt_in_state\":0,\"cnt_out_state\":0");
    uint8_t alarm[4] = { uint8_t(bits(h, 147, 8)), uint8_t(bits(h, 139, 8)), uint8_t(bits(h, 131, 8)),
                         uint8_t(bits(h, 123, 8)) };
    g.str(",\"todalarm\":");
    g.array(alarm, 4, 0xFF);
    g.str(",\"todlatch\":");
    g.array(rd + 8, 4, 0xFF);
    g.str(",\"todlatched\":0,\"todstopped\":0,\"todticks\":0,\"todclk\":0,\"todtickcounter\":0");
    g.str(",\"power_tickcounter\":0,\"power_ticks\":0,\"old_pa\":255,\"old_pb\":255");
    g.num(",\"read_clk\":", cycles);
    g.str(",\"read_offset\":0,\"last_read\":0,\"model\":0}");
}

static void write_vic(Gz &g, const uint32_t *w, const uint8_t *color, int line, int cycle)
{
    uint8_t regs[0x40];
    for (int r = 0; r < 0x40; r++) {
        regs[r] = r < 0x30 ? wbyte(w, W_VIC, r) : 0xFF;
    }
    g.str("\"vic\":{\"model\":0,\"regs\":");
    g.array(regs, 0x40, 0xFF);
    g.num(",\"raster_cycle\":", cycle);
    g.num(",\"cycle_flags\":0,\"raster_line\":", line);
    g.num(",\"start_of_frame\":0,\"irq_status\":", regs[0x19] & 0x0F);
    g.str(",\"raster_irq_line\":0,\"raster_irq_triggered\":0");
    g.str(",\"vbuf\":[],\"cbuf\":[],\"gbuf\":0,\"dbuf_offset\":0,\"dbuf\":[]");
    g.num(",\"ysmooth\":", regs[0x11] & 7);
    g.str(",\"allow_bad_lines\":0,\"sprite_sprite_collisions\":0");
    g.str(",\"sprite_background_collisions\":0,\"clear_collisions\":0,\"idle_state\":1");
    g.str(",\"vcbase\":0,\"vc\":0,\"rc\":0,\"vmli\":0,\"bad_line\":0");
    g.str(",\"light_pen\":{\"state\":0,\"triggered\":0,\"x\":0,\"y\":0,\"x_extra_bits\":0,\"trigger_cycle\":0}");
    g.str(",\"reg11_delay\":0,\"prefetch_cycles\":0,\"sprite_display_bits\":0,\"sprite_dma\":0");
    g.str(",\"last_color_reg\":0,\"last_color_value\":0,\"last_read_phi1\":0,\"last_bus_phi2\":0");
    g.str(",\"vborder\":1,\"set_vborder\":1,\"main_border\":1,\"refresh_counter\":0");
    g.str(",\"color_ram\":");
    g.array(color, 0x400, 0x0F);
    g.str(",\"sprite\":[]");
    g.str(",\"drawCycle\":{\"gbuf_pipe0_reg\":0,\"cbuf_pipe0_reg\":0,\"vbuf_pipe0_reg\":0");
    g.str(",\"gbuf_pipe1_reg\":0,\"cbuf_pipe1_reg\":0,\"vbuf_pipe1_reg\":0");
    g.num(",\"xscroll_pipe\":", regs[0x16] & 7);
    g.str(",\"vmode11_pipe\":0,\"vmode16_pipe\":0,\"vmode16_pipe2\":0");
    g.str(",\"gbuf_reg\":0,\"gbuf_mc_flop\":0,\"gbuf_pixel_reg\":0,\"cbuf_reg\":0,\"vbuf_reg\":0,\"dmli\":0");
    g.str(",\"sprite_x_pipe\":[],\"sprite_pri_bits\":0,\"sprite_mc_bits\":0,\"sprite_expx_bits\":0");
    g.str(",\"sprite_pending_bits\":0,\"sprite_active_bits\":0,\"sprite_halt_bits\":0");
    g.str(",\"sbuf_reg\":null,\"sbuf_pixel_reg\":null,\"sbuf_expx_flops\":0,\"sbuf_mc_flops\":0,\"border_state\":0");
    g.str(",\"render_buffer\":null,\"pri_buffer\":null,\"pixel_buffer\":null,\"cregs\":");
    g.ta(regs, 0x2F);
    g.str(",\"last_color_reg\":0,\"last_color_value\":0,\"cycle_flags_pipe\":0}}");
}

// T20 §9.3 "iec": the serial bus as far as this board captures it, which is the
// C64's own half - CIA 2 port A (b3 ATN, b4 CLK, b5 DATA out, b6 CLK, b7 DATA
// in). The drive's side sits in state-bus chips 9-12, which the capture table
// does not walk (c64_core.v reads chips 0-4 and 7), so it is not in here. The
// bits are given as they are, out of the port latch and out of the read image,
// with the raw bytes next to them: nothing here claims which way round the
// C64's inverters put a line. The field names are ours until the TRX64 session
// says what its own node calls them (T20 §13.3).
static void write_iec(Gz &g, const uint32_t *w)
{
    const uint32_t *h = w + W_CIA2H;
    uint8_t latch = uint8_t(bits(h, 32, 8)), ddr = uint8_t(bits(h, 16, 8)), rd = wbyte(w, W_CIA2, 0);
    g.str("\"iec\":{");
    g.num("\"atnOutBit\":", (latch >> 3) & 1);
    g.num(",\"clockOutBit\":", (latch >> 4) & 1);
    g.num(",\"dataOutBit\":", (latch >> 5) & 1);
    g.num(",\"clockInBit\":", (rd >> 6) & 1);
    g.num(",\"dataInBit\":", (rd >> 7) & 1);
    g.num(",\"cia2PaLatch\":", latch);
    g.num(",\"cia2PaRead\":", rd);
    g.num(",\"cia2Ddra\":", ddr);
    g.str("}");
}

// T20 §9.3 "cpuIntStatus": the interrupt lines at the snap and which chip was
// asserting. The lines are the 6510 wrapper's own registered samples, active
// high there (cpu6510.v drives the core's irq_n/nmi_n inverted); the sources
// are the IR bit of each chip's interrupt register image. pendingInt is VICE's
// encoding (1 IRQ, 2 NMI), which is what TRX64 reads.
static void write_ints(Gz &g, const uint32_t *w)
{
    const uint32_t *h = w + W_CPUH;
    int irq = int(bits(h, 207, 1)), nmi = int(bits(h, 208, 1));
    bool cia1 = (wbyte(w, W_CIA1, 13) & 0x80) != 0;
    bool cia2 = (wbyte(w, W_CIA2, 13) & 0x80) != 0;
    bool vic = (wbyte(w, W_VIC, 0x19) & 0x80) != 0;
    g.str("\"cpuIntStatus\":{");
    g.num("\"pendingInt\":", (irq ? 1 : 0) | (nmi ? 2 : 0));
    g.str(",\"intNames\":[");
    const char *sep = "";
    if (cia1) {
        g.str("\"cia1\"");
        sep = ",";
    }
    if (vic) {
        g.str(sep);
        g.str("\"vic\"");
        sep = ",";
    }
    if (cia2) {
        g.str(sep);
        g.str("\"cia2\"");
    }
    g.str("]");
    g.num(",\"irqLine\":", irq);
    g.num(",\"nmiLine\":", nmi);
    g.num(",\"interruptPending\":", int(bits(h, 120, 1)));      // proc_control intg_i
    g.num(",\"nmiActive\":", int(bits(h, 128, 1)));             // proc_interrupt nmi_act
    g.num(",\"irqInhibit\":", int(bits(h, 116, 1)));            // proc_control irq_inhibit
    g.str("}");
}

// T20 §9.3 "cartState": the cartridge's own registers out of state-bus chip 7
// word 0 ({reg_1, reg_0} of the icepi model), which is all of it the capture
// table walks, the type the app configured, and, when cart is given, the
// cartridge RAM window as it was at this frame. Not in here: easyflashJumper,
// because this board has no jumper state (the flash responder is configured by
// the app and takes writes whenever it is on), and the flash / 93C86 states,
// which the capture does not reach either.
static void write_cart(Gz &g, const uint32_t *w, const uint8_t *cart)
{
    if (!(w[W_GLUE] & GLUE_CART)) {
        g.str("\"cartBytes\":null,\"cartFlash\":null,\"cartState\":null");
        return;
    }
    // cartBytes (T20 §9.3): the .crt the app loaded, as it is on disk. Read
    // from the file, not written out of the image, because serialising it
    // through C64_CRT::save_crt() would tell the app the image is saved and a
    // write-back that is still due would be dropped.
    const char *src = C64_CRT::get_source();
    File *cf = NULL;
    uint32_t crt_len = 0;
    if (src && *src && FileManager::getFileManager()->fopen(src, FA_READ, &cf) == FR_OK) {
        crt_len = uint32_t(cf->get_size());
    }
    g.str("\"cartBytes\":");
    if (!crt_len) {
        g.str("null");
    } else {
        g.file_ta(cf, crt_len);
    }
    if (cf) {
        FileManager::getFileManager()->fclose(cf);
    }
    // cartFlash: the live window, for a cartridge whose flash the C64 can
    // program - that is the one case where the image can differ from the file,
    // and the ring's log does not carry flash writes, so only this node brings
    // it back. Reading it costs a DDR2 read per byte.
    g.str(",\"cartFlash\":");
    uint32_t rom_len = trx_cart_writable() ? C64_CRT::get_image_bytes() : 0;
    if (!rom_len) {
        g.str("null");
    } else {
        g.ta(C64::get_cartridge_rom_addr(), rom_len);
    }
    g.str(",\"cartState\":{");
    g.num("\"currentBank\":", uint8_t(w[W_CART]));
    g.num(",\"controlRegister\":", uint8_t(w[W_CART] >> 8));
    g.num(",\"cartridgeType\":", C64_CARTRIDGE_TYPE);
    g.str(",\"source\":\"");
    g.jstr(src ? src : "");
    g.str("\"");
    if (cart) {
        g.str(",\"cartRam\":");
        g.ta(cart, TRX_CART_RAM_BYTES);
    }
    g.str("}");
}

// T66 part 8: drive A's own state, read by a stub on its CPU (trx_drive.cc).
// The node is ours - "trx64Drive" - because TRX64's "drive1541" is a set of
// VICE modules whose layout cannot be checked against anything here (T20
// §13.3). It goes only into a single-frame .c64re: reading it holds the drive
// for a moment, which is nothing to do once per ring entry.
static void write_drive(Gz &g, const TrxDriveState *d)
{
    if (!d) {
        g.str("\"trx64Drive\":null");
        return;
    }
    g.str("\"trx64Drive\":{");
    g.str(d->regs ? "\"regs\":true" : "\"regs\":false");
    g.num(",\"cycles\":", int64_t(d->cycles));
    if (d->regs) {
        g.num(",\"pc\":", d->pc);
        g.num(",\"a\":", d->a);
        g.num(",\"x\":", d->x);
        g.num(",\"y\":", d->y);
        g.num(",\"sp\":", d->sp);
        g.num(",\"p\":", d->p);
        g.str(",\"via1\":");
        g.array(d->via1, 16, 0xFF);
        g.str(",\"via2\":");
        g.array(d->via2, 16, 0xFF);
    }
    g.str(",\"ram\":");
    g.ta(d->ram, TRX_DRIVE_RAM);
    g.str("}");
}

// T20 §9.3 "media" and "drivePart": which file the app has on drive A and
// whether that drive is powered. The image itself is not in here (the media
// payloads of part 5); this says which file the state belongs to.
static void write_media(Gz &g)
{
    g.str("\"drivePart\":");
    if (!c1541_A) {
        g.str("null,\"media\":null");
        return;
    }
    g.str(c1541_A->get_drive_power() ? "{\"power\":true" : "{\"power\":false");
    g.num(",\"unit\":", c1541_A->get_current_iec_address());
    g.str(",\"driveType\":\"");
    g.jstr(c1541_A->get_drive_type_string());
    g.str("\"}");
    mstring path, name;
    c1541_A->get_last_mounted_file(path, name);
    g.str(",\"media\":");
    if (!name.length()) {
        g.str("null");
        return;
    }
    g.str("{\"diskPath\":\"");
    g.jstr(path.c_str());
    if (path.length() && path[path.length() - 1] != '/') {
        g.str("/");
    }
    g.jstr(name.c_str());
    g.str("\",\"imageFormat\":\"");
    const char *dot = strrchr(name.c_str(), '.');
    g.jstr(dot ? dot + 1 : "");
    g.str("\"}");
}

// The checkpoint tree of one entry (an object, without a key in front).
// TRX64 0.12.2's turbo node (T20 §10.1, §17.5), the one place that builds it. With the T29 engine
// (CAPS2 b6) it is the engine's live state: TURBO_W1/W2 (TRX64-Ultimate docs/specs/T29-turbo.md
// §12.7). Without it the machine runs at 1 MHz with badline stalls, the registers-mode reset
// state, written out.
static void write_turbo(Gz &g, const TrxFrame &)
{
    if (ioRead8(TRX_CAPS2) & TRX_CAPS2_TURBO) {
        // T29: the engine's live state (TURBO_W1/W2); the ring records at 1 MHz only, so a frame's
        // node is the state of the machine when it is written, not per frame
        uint32_t w1 = trx_io_read32(TRX_TURBO_W1), w2 = trx_io_read32(TRX_TURBO_W2);
        g.str("\"turbo\":{");
        g.num("\"enable\":", (w1 >> 8) & 7);
        g.num(",\"menuIndex\":", w1 & 15);
        g.str((w1 & 0x80) ? ",\"menuStalls\":true" : ",\"menuStalls\":false");
        g.num(",\"d031\":", w1 >> 24);
        g.num(",\"d030\":", 0xFE | ((w1 >> 11) & 1));
        g.num(",\"resetHold\":", w2 & 0x7FFFFF);
        g.str("}");
        return;
    }
    g.str("\"turbo\":{\"enable\":1,\"menuIndex\":0,\"menuStalls\":true,\"d031\":0,\"d030\":0,\"resetHold\":0}");
}

static void write_checkpoint(Gz &g, const TrxFrame &f, uint8_t *ram, const uint8_t *col, const uint8_t *cart,
                             const TrxDriveState *drive)
{
    const uint32_t *w = f.w;
    static uint8_t under_io[1024];  // one writer at a time (the REST task or the menu)
    memcpy(under_io, ram + 0xD800, 1024);
    for (int i = 0; i < 1024; i++) {
        ram[0xD800 + i] = col[i] & 0x0F;
    }
    uint32_t r0 = w[W_CPU], r1 = w[W_CPU + 1], r2 = w[W_CPU + 2];
    int pc = int((r1 & 0xFFFF) - PC_ADJ) & 0xFFFF;
    int cycle = f.x >> 3;
    g.str("{\"schemaVersion\":1,\"atInstructionBoundary\":true,\"cpu\":{");
    g.num("\"pc\":", pc);
    g.num(",\"a\":", r0 & 0xFF);
    g.num(",\"x\":", (r0 >> 8) & 0xFF);
    g.num(",\"y\":", (r0 >> 16) & 0xFF);
    g.num(",\"sp\":", (r0 >> 24) & 0xFF);
    g.num(",\"flags\":", (r1 >> 16) & 0xFF);
    g.num(",\"cycles\":", int64_t(f.cycles));
    g.str("},\"ram\":");
    g.ta(ram, 65536);
    g.num(",\"cpuPortDirection\":", r2 & 0xFF);
    g.num(",\"cpuPortValue\":", (r2 >> 8) & 0xFF);
    g.str(",");
    write_cia(g, "cia1", w, W_CIA1, W_CIA1H, f.cycles);
    g.str(",");
    write_cia(g, "cia2", w, W_CIA2, W_CIA2H, f.cycles);
    g.str(",");
    write_vic(g, w, col, f.line, cycle);
    g.str(",\"alarmsMaincpu\":[],\"cartBytes\":null,");
    write_iec(g, w);
    g.str(",");
    write_ints(g, w);
    g.str(",");
    write_cart(g, w, cart);
    g.str(",");
    write_media(g);
    g.str(",");
    write_drive(g, drive);
    g.str(",");
    write_turbo(g, f);
    g.num(",\"trx64Hw\":{\"layout\":", LAYOUT);
    g.num(",\"x\":", f.x);
    g.num(",\"line\":", f.line);
    g.num(",\"marker\":", f.marker);
    g.num(",\"flags\":", f.flags);
    g.str(",\"block\":");
    uint8_t blk[TRX_BLOCK_WORDS * 4];
    for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
        for (int b = 0; b < 4; b++) {
            blk[4 * i + b] = uint8_t(w[i] >> (8 * b));
        }
    }
    g.ta(blk, sizeof(blk));
    g.str(",\"ramUnderIo\":");
    g.ta(under_io, 1024);
    g.str(",\"colorRam\":");
    g.ta(col, 1024);
    g.str("}}");
    memcpy(ram + 0xD800, under_io, 1024);
}

static const char *open_out(const char *path, File **f)
{
    FileManager *fm = FileManager::getFileManager();
    if (fm->fopen(path, FA_WRITE | FA_CREATE_ALWAYS, f) != FR_OK) {
        return "The file could not be created";
    }
    return NULL;
}

// The image mounted on drive A, for manifest.media[] and mediaPayloads (T20
// §9.1, T66 part 5). The file is read twice: once to hash it, because the
// manifest stands in front of the payload in the document, and once into the
// document itself. A ring file has no manifest and carries no payload.
struct MediaFile {
    char     full[192];
    char     name[80];
    char     format[8];
    uint32_t size;
    uint8_t  digest[32];
};

static bool media_probe(MediaFile *m)
{
    if (!c1541_A) {
        return false;
    }
    mstring path, name;
    c1541_A->get_last_mounted_file(path, name);
    if (!name.length()) {
        return false;
    }
    bool slash = path.length() && path[path.length() - 1] == '/';
    if (int(path.length() + name.length()) + (slash ? 0 : 1) >= int(sizeof(m->full))) {
        printf("[C64RE] the mounted image's path is too long for the manifest: %s\n", name.c_str());
        return false;
    }
    strcpy(m->full, path.c_str());
    if (!slash) {
        strcat(m->full, "/");
    }
    strcat(m->full, name.c_str());
    strncpy(m->name, name.c_str(), sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = 0;
    const char *dot = strrchr(m->name, '.');
    m->format[0] = 0;
    for (int i = 0; dot && dot[1 + i] && i < int(sizeof(m->format)) - 1; i++) {
        char c = dot[1 + i];
        m->format[i] = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        m->format[i + 1] = 0;
    }
    File *f = NULL;
    if (FileManager::getFileManager()->fopen(m->full, FA_READ, &f) != FR_OK) {
        return false;
    }
    m->size = uint32_t(f->get_size());
    TrxSha256 sh;
    trx_sha_init(&sh);
    uint8_t piece[3072];
    uint32_t left = m->size;
    bool ok = m->size != 0;
    while (ok && left) {
        uint32_t want = left > sizeof(piece) ? uint32_t(sizeof(piece)) : left;
        uint32_t got = 0;
        if (f->read(piece, want, &got) != FR_OK || got != want) {
            ok = false;
            break;
        }
        trx_sha_add(&sh, piece, want);
        left -= want;
    }
    trx_sha_final(&sh, m->digest);
    FileManager::getFileManager()->fclose(f);
    return ok;
}

// The payload itself, streamed from the file into the document.
static void media_payload(Gz &g, const MediaFile *m)
{
    File *f = NULL;
    if (FileManager::getFileManager()->fopen(m->full, FA_READ, &f) != FR_OK) {
        g.str("{}");
        return;
    }
    g.str("{\"media0\":");
    g.file_ta(f, m->size);
    g.str("}");
    FileManager::getFileManager()->fclose(f);
}

const char *trx_c64re_dump(const char *path, int idx, uint32_t *bytes)
{
    TrxFrame *f = (TrxFrame *)malloc(sizeof(TrxFrame));
    uint8_t *ram = (uint8_t *)malloc(65536);
    uint8_t *col = (uint8_t *)malloc(1024);
    const char *err = NULL;
    File *file = NULL;
    uint8_t *cart = NULL;
    if (!f || !ram || !col) {
        err = "Out of memory";
    } else if (!trx_ring_frame(idx, f)) {
        err = "No such frame";
    } else if (!f->block) {
        err = "The frame has no chip block";
    } else {
        // with a cartridge on, its RAM window comes out of the same pause
        // (T66 part 5); without one there is nothing to read
        if (f->w[W_GLUE] & GLUE_CART) {
            cart = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
        }
        if (int e = trx_ring_materialize(idx, ram, col, cart)) {
            err = trx_ring_error(e);
        } else {
            err = open_out(path, &file);
        }
    }
    MediaFile mf;
    bool have_media = !err && media_probe(&mf);
    // drive A, through the stub on its own CPU (T66 part 8). A drive that
    // cannot be entered gives its RAM only, and one that is off gives nothing;
    // neither stops the file from being written.
    TrxDriveState *drive = NULL;
    if (!err && trx_drive_present()) {
        drive = (TrxDriveState *)malloc(sizeof(TrxDriveState));
        const char *derr = drive ? trx_drive_read(drive, true) : "Out of memory";
        if (derr) {
            printf("[C64RE] drive A is not in the file: %s\n", derr);
            free(drive);
            drive = NULL;
        }
    }
    if (!err) {
        uint8_t header[41];
        memset(header, 0, sizeof(header));
        memcpy(header, "C64RESNP", 8);
        header[8] = 1;
        uint32_t tr;
        file->write(header, sizeof(header), &tr);
        Gz g(file, true);
        g.str("{\"manifest\":{\"kind\":\"c64re-runtime-snapshot\",\"version\":1,\"createdAt\":\"1970-01-01T00:00:00Z\"");
        g.str(",\"machine\":{\"model\":\"c64-pal\",\"runtimeVersion\":\"trx64-ultimate/t20-app\"}");
        g.num(",\"checkpoint\":{\"encoding\":\"ta-json-gz/1\",\"payloadRef\":\"checkpoint\",\"cycle\":", int64_t(f->cycles));
        g.num(",\"pc\":", int((f->w[W_CPU + 1] & 0xFFFF) - PC_ADJ) & 0xFFFF);
        g.str("},\"media\":[");
        if (have_media) {
            g.str("{\"role\":\"disk\",\"format\":\"");
            g.jstr(mf.format);
            g.str("\",\"sha256\":\"");
            g.hex(mf.digest, 32);
            g.str("\",\"sourceName\":\"");
            g.jstr(mf.name);
            g.str("\",\"embeddedPayloadRef\":\"media0\"}");
        }
        g.str("]},\"checkpoint\":");
        write_checkpoint(g, *f, ram, col, cart, drive);
        g.str(",\"mediaPayloads\":");
        if (have_media) {
            media_payload(g, &mf);
        } else {
            g.str("{}");
        }
        g.str("}");
        uint8_t digest[32];
        if (!g.finish(digest)) {
            err = "The file could not be written";
        } else {
            file->seek(9);
            if (file->write(digest, 32, &tr) != FR_OK || tr != 32) {
                err = "The file could not be written";
            }
        }
        *bytes = 41 + g.written;
        FileManager::getFileManager()->fclose(file);
    }
    free(f);
    free(ram);
    free(col);
    free(cart);
    free(drive);
    return err;
}

#define MAX_RING_ENTRIES 64

const char *trx_c64rering_dump_ex(const char *path, int seconds, int stride, uint32_t *bytes, int *entries,
                                  trx_progress_fn progress, void *ctx)
{
    if (stride < 1) {
        stride = 1;
    }
    int n_tl = trx_ring_count();
    int span = (seconds * (int)trx_ring_fps_x100() + 50) / 100;
    int idx[MAX_RING_ENTRIES];
    int n = 0;
    // from the newest whole entry back, every stride frames
    int i = trx_ring_target(0);
    int lo = n_tl - 1 - span;
    while (i >= 0 && i >= lo && n < MAX_RING_ENTRIES) {
        idx[n++] = i;
        int next = i - stride;
        if (next < 0) {
            break;
        }
        i = trx_ring_target(n_tl - 1 - next);
    }
    if (!n) {
        return "The ring holds no frame";
    }
    // oldest first
    for (int k = 0; k < n / 2; k++) {
        int t = idx[k];
        idx[k] = idx[n - 1 - k];
        idx[n - 1 - k] = t;
    }
    uint8_t **rams = (uint8_t **)calloc(n, sizeof(uint8_t *));
    uint8_t **cols = (uint8_t **)calloc(n, sizeof(uint8_t *));
    TrxFrame *fr = (TrxFrame *)malloc(n * sizeof(TrxFrame));
    const char *err = NULL;
    if (!rams || !cols || !fr) {
        err = "Out of memory";
    }
    // the entries as they are now (materialize may move the indices; it
    // finds its targets by their seq)
    for (int k = 0; !err && k < n; k++) {
        if (!trx_ring_frame(idx[k], &fr[k])) {
            err = "No such frame";
        }
    }
    for (int k = 0; !err && k < n; k++) {
        rams[k] = (uint8_t *)malloc(65536);
        cols[k] = (uint8_t *)malloc(1024);
        if (!rams[k] || !cols[k]) {
            err = "Out of memory";
        }
    }
    uint16_t t0 = getMsTimer();
    if (!err) {
        if (int e = trx_ring_materialize_many(idx, n, rams, cols)) {
            err = trx_ring_error(e);
        }
    }
    uint16_t t1 = getMsTimer();
    if (!err && progress) {
        progress(0, n, ctx);        // the RAM of every entry is in memory; the JSON and the file follow
    }
    File *file = NULL;
    if (!err) {
        err = open_out(path, &file);
    }
    if (!err) {
        static const uint8_t header[9] = { 'C', '6', '4', 'R', 'E', 'R', 'N', 'G', 1 };
        uint32_t tr;
        file->write(header, sizeof(header), &tr);
        Gz g(file, false);
        char id[32];
        const TrxFrame &last = fr[n - 1];
        g.num("{\"checkpoint_ring\":{\"budget_bytes\":", int64_t(n) * 256 * 1024);
        g.num(",\"max_entries\":", n);
        g.num(",\"seq\":", last.seq);
        sprintf(id, "cp_%u_%u", (unsigned)(last.cycles / trx_ring_frame_cycles()), (unsigned)last.seq);
        g.str(",\"current_id\":\"");
        g.str(id);
        g.str("\",\"entries\":[");
        for (int k = 0; k < n; k++) {
            const TrxFrame *f = &fr[k];
            // frame: the ring's cycle count in frames of the running mode (seq skips the frames a rewind cut)
            uint32_t frame = uint32_t(f->cycles / trx_ring_frame_cycles());
            sprintf(id, "cp_%u_%u", (unsigned)frame, (unsigned)f->seq);
            g.str(k ? ",{\"id\":\"" : "{\"id\":\"");
            g.str(id);
            g.num("\",\"frame\":", frame);
            g.num(",\"cycles\":", int64_t(f->cycles));
            g.str(",\"pinned\":false,\"byte_size\":96000,\"created_at_ms\":0,\"payload\":");
            // no cartridge RAM per entry: that would be a 64 KB read and 87 KB
            // of base64 for every one of them (T66 part 5)
            write_checkpoint(g, *f, rams[k], cols[k], NULL, NULL);
            g.str("}");
            if (progress) {
                progress(k + 1, n, ctx);
            }
        }
        g.str("]},\"delta_ring\":{\"entry_cap\":1,\"writes_cap\":1,\"enabled\":false,\"entries\":\"\",\"writes\":\"\"}");
        g.str(",\"cpu_history\":{\"capacity\":1,\"enabled\":false,\"entries\":\"\"}}");
        if (!g.finish(NULL)) {
            err = "The file could not be written";
        }
        printf("[RING] .c64rering: %d entries, materialized in %u ms, written in %u ms (%u in file writes)\n", n,
               (unsigned)uint16_t(t1 - t0), (unsigned)uint16_t(getMsTimer() - t1), (unsigned)g.write_ms);
        *bytes = 9 + g.written;
        *entries = n;
        FileManager::getFileManager()->fclose(file);
    }
    for (int k = 0; k < n && rams && cols; k++) {
        free(rams[k]);
        free(cols[k]);
    }
    free(rams);
    free(cols);
    free(fr);
    return err;
}

const char *trx_c64rering_dump(const char *path, int seconds, int stride, uint32_t *bytes, int *entries)
{
    return trx_c64rering_dump_ex(path, seconds, stride, bytes, entries, NULL, NULL);
}

// ---- load ------------------------------------------------------------------------
// The body of a gzip member: stored blocks, as this board writes them, or a
// real deflate stream, as TRX64 writes them (T66 part 4, trx_inflate.c).
// Returns the JSON, zero-terminated, for the caller to free.
static char *gunzip(const uint8_t *p, uint32_t len, uint32_t *out_len, const char **err)
{
    uint32_t isize = trx_gzip_isize(p, len);
    if (len < 18 || p[0] != 0x1F || p[1] != 0x8B || p[2] != 8 || !isize) {
        *err = "Not a gzip body";
        return NULL;
    }
    char *out = (char *)malloc(isize + 1);
    if (!out) {
        static char msg[64];
        sprintf(msg, "The file unpacks to %lu KB, which did not fit", (unsigned long)(isize >> 10));
        *err = msg;
        return NULL;
    }
    uint32_t got = 0;
    int e = trx_gunzip(p, len, (uint8_t *)out, isize, &got);
    if (e) {
        free(out);
        *err = trx_inflate_error(e);
        return NULL;
    }
    out[got] = 0;
    *out_len = got;
    return out;
}

// A .c64re, or the current entry of a .c64rering (checkpoint_ring.current_id;
// the entry's payload is the same checkpoint node), written by this board.
// The state of a file, read and checked, waiting to go into the machine. Its
// two buffers are the ones trx_ring_load_state() wants (T66; the browser
// stages while the menu holds the machine and applies when it closes).
struct TrxC64reStaged {
    uint8_t *ram;                   // 65536, the RAM under the I/O already in place
    uint8_t *col;                   // 1024, the colour RAM's low nibbles
    uint8_t *cart;                  // TRX_CART_RAM_BYTES of cartridge RAM, or NULL (T66 part 5)
    uint8_t *rom;                   // the cartridge's ROM / flash window, or NULL
    uint32_t rom_len;
    TrxDriveState *drive;           // drive A's own state, or NULL (T66 part 8)
    uint32_t w[TRX_BLOCK_WORDS];    // the chip block
    uint16_t x;                     // the snap position it belongs on
    bool     turbo;                 // a turbo word goes in with it (T29)
    uint32_t turbo_pack;
};

static TrxC64reStaged *pending_state;   // the browser's choice, taken at unfreeze

// Is the whole string a decimal number (so it can mean a frame)?
static bool all_digits(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

// Which entry of a `.c64rering` to load (T66 part 3): the cursor when want is
// NULL or empty - that is what the file calls `current_id` - or the entry whose
// `id` ("cp_<frame>_<seq>") or `label` is want, or whose `frame` is want read as
// a number, or, written as "#N", the Nth entry in the file (0 = oldest, the
// order §10.1 writes them in). Returns the payload token, or -1 with *err set.
static int ring_pick(const char *js, const jsmntok_t *t, int n, const char *want, const char **err)
{
    int cr = json_key(js, t, n, 0, "checkpoint_ring");
    int cur = json_key(js, t, n, cr, "current_id");
    int ents = json_key(js, t, n, cr, "entries");
    bool cursor = !want || !*want;
    if (ents < 0 || t[ents].type != JSMN_ARRAY || (cursor && cur < 0)) {
        *err = "No checkpoint_ring entries";
        return -1;
    }
    bool by_index = !cursor && want[0] == '#';
    long idx_want = by_index ? strtol(want + 1, NULL, 10) : -1;
    int at = 0;
    for (int i = ents + 1; i < n && t[i].start < t[ents].end; i++) {
        if (t[i].parent != ents) {
            continue;
        }
        int this_one = at++;
        int id = json_key(js, t, n, i, "id");
        bool hit;
        if (by_index) {
            hit = this_one == idx_want;
        } else if (cursor) {
            hit = id >= 0 && t[id].end - t[id].start == t[cur].end - t[cur].start &&
                  !strncmp(js + t[id].start, js + t[cur].start, t[cur].end - t[cur].start);
        } else {
            int lb = json_key(js, t, n, i, "label");
            int fr = json_key(js, t, n, i, "frame");
            hit = (id >= 0 && tok_eq(js, t[id], want)) ||
                  (lb >= 0 && tok_eq(js, t[lb], want)) ||
                  (fr >= 0 && all_digits(want) && tok_int(js, t, fr) == strtol(want, NULL, 10));
        }
        if (hit) {
            int pl = json_key(js, t, n, i, "payload");
            if (pl < 0) {
                *err = "That entry carries no payload";
            }
            return pl;
        }
    }
    *err = cursor ? "The current entry is not in the file" : "No such entry in the file";
    return -1;
}

// The chip block of the machine as it runs now: the newest timeline entry that
// carries a whole one. A node-built block keeps every flip-flop no file names
// (above all the CPU's instruction sequencer) from it, so it must come from
// this machine and this bitstream, not from a table of constants.
static bool base_block(uint32_t *w, uint16_t *x)
{
    trx_ring_poll();
    for (int i = trx_ring_count() - 1; i >= 0; i--) {
        TrxFrame f;
        if (trx_ring_frame(i, &f) && f.block && f.x >= 96 && f.x <= 504) {
            memcpy(w, f.w, sizeof(f.w));
            *x = f.x;
            return true;
        }
    }
    return false;
}

// manifest.machine.model: only a PAL C64 state fits this machine (T20 §9.4).
static const char *model_refusal(const char *js, const jsmntok_t *t, int n)
{
    int m = json_key(js, t, n, json_key(js, t, n, json_key(js, t, n, 0, "manifest"), "machine"), "model");
    if (m < 0) {
        return NULL;                // a ring entry has no manifest of its own
    }
    if (tok_eq(js, t[m], "c64-pal")) {
        return NULL;
    }
    static char msg[80];
    int len = t[m].end - t[m].start;
    if (len > 40) {
        len = 40;
    }
    sprintf(msg, "The file is for machine \"%.*s\", not c64-pal", len, js + t[m].start);
    return msg;
}

// What the coarse load took and what it could not take, on the console: the
// user is told the load is not exact, and by how much.
static void say_coarse(const TrxNodeState *ns)
{
    printf("[C64RE] coarse load (no trx64Hw node): cpu%s%s%s%s%s%s%s%s\n",
           (ns->have & TRX_NODE_PORT) ? " port" : "", (ns->have & TRX_NODE_CIA1) ? " cia1" : "",
           (ns->have & TRX_NODE_CIA2) ? " cia2" : "", (ns->have & TRX_NODE_VIC) ? " vic" : "",
           (ns->have & TRX_NODE_COLOR) ? " colour" : "", (ns->have & TRX_NODE_RAM) ? " ram" : "",
           (ns->have & TRX_NODE_CART) ? " cart-banking" : "",
           (ns->have & TRX_NODE_CARTRAM) ? " cart-ram" : "");
    printf("[C64RE] not restored: the VIC's raster position (file line %d cycle %d) and pixel pipeline, "
           "the SID, the drive\n", ns->line, ns->cycle);
    if (ns->keys_pressed) {
        printf("[C64RE] %d key(s) were held down in the file; they are not pressed here\n", ns->keys_pressed);
    }
}

// The file's "media" node against what is mounted now (T66 part 5). Nothing is
// mounted or unmounted here: a load puts the machine's state back, and which
// disk is in the drive is the user's. A mismatch is said once, because the
// program in the snapshot will read sectors that are no longer under the head.
static void say_media(const char *js, const jsmntok_t *t, int n, int cp)
{
    int md = json_key(js, t, n, cp, "media");
    int dp = json_key(js, t, n, md, "diskPath");
    if (dp < 0) {
        return;
    }
    int len = t[dp].end - t[dp].start;
    const char *want = js + t[dp].start;
    mstring path, name;
    if (c1541_A) {
        c1541_A->get_last_mounted_file(path, name);
    }
    bool same = false;
    if (name.length() && len > int(name.length())) {
        same = !strncmp(want + len - name.length(), name.c_str(), name.length());
    }
    if (!same) {
        printf("[C64RE] the snapshot was taken with \"%.*s\" in drive A; now it is \"%s\"\n", len, want,
               name.length() ? name.c_str() : "nothing");
    }
}

const char *trx_c64re_stage(const char *path, TrxC64reStaged **out)
{
    return trx_c64re_stage_entry(path, NULL, out);
}

const char *trx_c64re_stage_entry(const char *path, const char *want, TrxC64reStaged **out)
{
    *out = NULL;
    FileManager *fm = FileManager::getFileManager();
    File *file;
    if (fm->fopen(path, FA_READ, &file) != FR_OK) {
        return "The file could not be opened";
    }
    uint32_t size = file->get_size();
    uint8_t *raw = (uint8_t *)malloc(size);
    const char *err = NULL;
    uint32_t tr = 0;
    if (!raw) {
        err = "Out of memory";
    } else if (file->read(raw, size, &tr) != FR_OK || tr != size) {
        err = "The file could not be read";
    }
    fm->fclose(file);
    bool ring = !err && size >= 9 + 18 && !memcmp(raw, "C64RERNG", 8) && raw[8] == 1;
    if (!err && !ring && (size < 41 + 18 || memcmp(raw, "C64RESNP", 8) || raw[8] != 1)) {
        err = "Not a .c64re or .c64rering (format 1)";
    }
    if (!err && !ring) {
        TrxSha256 s;
        uint8_t d[32];
        trx_sha_init(&s);
        trx_sha_add(&s, raw + 41, size - 41);
        trx_sha_final(&s, d);
        if (memcmp(d, raw + 9, 32)) {
            err = "Hash mismatch";
        }
    }
    char *js = NULL;
    uint32_t jl = 0;
    if (!err) {
        uint32_t hdr = ring ? 9 : 41;
        js = gunzip(raw + hdr, size - hdr, &jl, &err);
    }
    free(raw);
    jsmntok_t *t = NULL;
    int n = 0;
    if (!err) {
        jsmn_parser p;
        jsmn_init(&p);
        int need = jsmn_parse(&p, js, jl, NULL, 0);
        if (need < 1) {
            err = "Bad JSON";
        } else {
            t = (jsmntok_t *)malloc(sizeof(jsmntok_t) * need);
            if (!t) {
                err = "Out of memory";
            } else {
                jsmn_init(&p);
                n = jsmn_parse(&p, js, jl, t, need);
                if (n < 1) {
                    err = "Bad JSON";
                }
            }
        }
    }
    int cp = -1;
    if (!err && !ring) {
        cp = json_key(js, t, n, 0, "checkpoint");
    } else if (!err) {
        // checkpoint_ring.entries[k].payload - the cursor's entry, or the one
        // `want` names (T66 part 3)
        cp = ring_pick(js, t, n, want, &err);
    }
    uint8_t *ram = NULL, *col = NULL, *cart = NULL, *rom = NULL;
    TrxDriveState *drive = NULL;
    uint32_t rom_len = 0;
    uint32_t w[TRX_BLOCK_WORDS];
    int x = 0;
    bool have_turbo = false;
    uint32_t turbo_pack = 0;
    if (!err) {
        // a file from a turbo-capable machine (TRX64 or a later board): this core runs at 1 MHz
        int tb = json_key(js, t, n, cp, "turbo");
        if (tb >= 0 && (ioRead8(TRX_CAPS2) & TRX_CAPS2_TURBO)) {
            // T29: the file's turbo state goes in with the machine (state bus chip 0 word 1)
            uint32_t en = tok_int(js, t, json_key(js, t, n, tb, "enable")) & 7;
            uint32_t mi = tok_int(js, t, json_key(js, t, n, tb, "menuIndex")) & 15;
            int ms = json_key(js, t, n, tb, "menuStalls");
            bool stalls = ms < 0 || strncmp(js + t[ms].start, "false", 5) != 0;
            uint32_t d031 = tok_int(js, t, json_key(js, t, n, tb, "d031")) & 0xFF;
            uint32_t d030 = tok_int(js, t, json_key(js, t, n, tb, "d030")) & 1;
            uint32_t hold = tok_int(js, t, json_key(js, t, n, tb, "resetHold")) & 0x7FFFFF;
            turbo_pack = ((hold >> 8) << 17) | (d031 << 9) | (d030 << 8) | (en << 5) |
                         ((stalls ? 1u : 0u) << 4) | mi;
            have_turbo = true;
        } else if (tb >= 0 && (tok_int(js, t, json_key(js, t, n, tb, "menuIndex")) > 0 ||
                        (tok_int(js, t, json_key(js, t, n, tb, "d031")) & 0x0F) > 0)) {
            printf("[C64RE] the file's turbo state (speed index %d) is not applied: this bitstream has no turbo\n",
                   (int)tok_int(js, t, json_key(js, t, n, tb, "menuIndex")));
        }
        int hw = json_key(js, t, n, cp, "trx64Hw");
        ram = (uint8_t *)malloc(65536);
        col = (uint8_t *)malloc(1024);
        // cartridge RAM rides in the cartState node of both file kinds; a file
        // without it leaves the window as it is (T66 part 5)
        if (json_key(js, t, n, json_key(js, t, n, cp, "cartState"), "cartRam") >= 0) {
            cart = (uint8_t *)malloc(TRX_CART_RAM_BYTES);
        }
        // A cartridge snapshot only fits the cartridge that is on: the banking
        // registers, the RAM window and the ROM image all belong to that type.
        int ft = trx_c64re_cart_type(js, t, n, cp);
        if (ft >= 0 && ft != int(C64_CARTRIDGE_TYPE)) {
            static char msg[80];
            sprintf(msg, "The file is of cartridge type %02x; this machine has %02x", ft,
                    (int)C64_CARTRIDGE_TYPE);
            err = msg;
        }
        // cartFlash: the ROM / flash window as the file has it. It goes back
        // into DDR2 with the load, because no log carries flash writes.
        uint32_t fl = err ? 0 : trx_c64re_cart_flash_len(js, t, n, cp);
        if (fl) {
            if (fl > C64::get_cartridge_max_rom()) {
                fl = C64::get_cartridge_max_rom();
            }
            rom = (uint8_t *)malloc(fl);
            if (rom && trx_c64re_cart_flash(js, t, n, cp, rom, fl) == fl) {
                rom_len = fl;
            } else {
                free(rom);          // short or no memory: the window stays as it is
                rom = NULL;
                printf("[C64RE] the file's cartFlash node (%lu KB) could not be read: the cartridge "
                       "image in memory stays as it is\n", (unsigned long)(fl >> 10));
            }
        }
        // drive A (T66 part 8): both kinds of file carry the node the same way
        if (!err && json_key(js, t, n, cp, "trx64Drive") >= 0 && trx_drive_present()) {
            drive = (TrxDriveState *)malloc(sizeof(TrxDriveState));
            if (drive && !trx_c64re_drive_from_node(js, t, n, cp, drive)) {
                free(drive);
                drive = NULL;
                printf("[C64RE] the file's drive node could not be read: drive A stays as it is\n");
            }
        }
        static uint8_t blk[TRX_BLOCK_WORDS * 4];
        static uint8_t under[1024];
        bool exact = hw >= 0 && tok_int(js, t, json_key(js, t, n, hw, "layout")) == LAYOUT;
        if (err) {
            // refused above (the cartridge type): nothing more to read
        } else if (!ram || !col) {
            err = "Out of memory";
        } else if (!exact) {
            // A file from TRX64, or one from a board with another block layout:
            // the coarse load of T20 §9.4, the block built from the nodes on top
            // of the machine's own newest frame (T66 part 4).
            if (hw >= 0) {
                printf("[C64RE] trx64Hw layout %d is not this build's %d: the nodes are used instead\n",
                       (int)tok_int(js, t, json_key(js, t, n, hw, "layout")), LAYOUT);
            }
            err = model_refusal(js, t, n);
            TrxNodeState ns;
            ns.cart = cart;
            if (!err && !base_block(ns.w, &ns.x)) {
                err = "No frame of this machine to build on: let Timewarp run first";
            }
            if (!err) {
                ns.ram = ram;
                ns.col = col;
                err = trx_c64re_from_nodes(js, t, n, cp, &ns);
            }
            if (!err) {
                say_media(js, t, n, cp);
                memcpy(w, ns.w, sizeof(w));
                x = ns.x;
                if (!ns.cart) {
                    free(cart);
                    cart = NULL;
                }
                say_coarse(&ns);
            }
        } else if (!tok_ta(js, t, n, json_key(js, t, n, cp, "ram"), ram, 65536) ||
                   !tok_ta(js, t, n, json_key(js, t, n, hw, "block"), blk, sizeof(blk)) ||
                   !tok_ta(js, t, n, json_key(js, t, n, hw, "ramUnderIo"), under, 1024) ||
                   !tok_ta(js, t, n, json_key(js, t, n, hw, "colorRam"), col, 1024)) {
            err = "Missing or short arrays";
        } else {
            memcpy(ram + 0xD800, under, 1024);
            for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
                w[i] = blk[4 * i] | blk[4 * i + 1] << 8 | blk[4 * i + 2] << 16 | uint32_t(blk[4 * i + 3]) << 24;
            }
            x = int(tok_int(js, t, json_key(js, t, n, hw, "x")));
            if (x < 96 || x > 504) {
                err = "Bad snap position";
            }
            if (!err) {
                say_media(js, t, n, cp);
            }
            if (!err && cart &&
                trx_c64re_cart_ram(js, t, n, cp, cart, TRX_CART_RAM_BYTES) != TRX_CART_RAM_BYTES) {
                free(cart);                 // a short cartRam node: leave the window alone
                cart = NULL;
            }
        }
    }
    free(t);
    free(js);
    TrxC64reStaged *st = NULL;
    if (!err) {
        st = (TrxC64reStaged *)malloc(sizeof(TrxC64reStaged));
        if (!st) {
            err = "Out of memory";
        }
    }
    if (err) {
        free(ram);
        free(col);
        free(cart);
        free(rom);
        free(drive);
        return err;
    }
    st->ram = ram;
    st->col = col;
    st->cart = cart;
    st->rom = rom;
    st->rom_len = rom_len;
    st->drive = drive;
    memcpy(st->w, w, sizeof(st->w));
    st->x = uint16_t(x);
    st->turbo = have_turbo;
    st->turbo_pack = turbo_pack;
    *out = st;
    return NULL;
}

void trx_c64re_discard(TrxC64reStaged *st)
{
    if (!st) {
        return;
    }
    free(st->ram);
    free(st->col);
    free(st->cart);
    free(st->rom);
    free(st->drive);
    free(st);
}

const char *trx_c64re_apply(TrxC64reStaged *st)
{
    if (!st) {
        return "Nothing staged";
    }
    if (st->turbo) {
        trx_ring_set_turbo(st->turbo_pack);     // one shot, taken by the load below
    }
    TrxLoadMem mem = { st->ram, st->col, st->cart, st->rom, st->rom_len };
    int e = trx_ring_load_state(&mem, st->w, st->x);
    if (!e && st->drive) {
        // drive A after the machine: the stub runs on the drive's own CPU and
        // is stepped, so the C64's state does not matter (T66 part 8)
        const char *derr = trx_drive_write(st->drive);
        if (derr) {
            printf("[C64RE] drive A was not put back: %s\n", derr);
        }
    }
    trx_c64re_discard(st);
    return e ? trx_ring_error(e) : NULL;
}

const char *trx_c64re_load(const char *path)
{
    return trx_c64re_load_entry(path, NULL);
}

const char *trx_c64re_load_entry(const char *path, const char *entry)
{
    if (trx_trxring_is_one(path)) {
        // one way in for every file the ring can load: a .trxring brings the
        // ring back as well, and has no entries to choose from (T66 part 6)
        if (entry && *entry) {
            return "A .trxring holds the whole ring; it has no entry to choose";
        }
        return trx_trxring_load(path);
    }
    TrxC64reStaged *st = NULL;
    const char *err = trx_c64re_stage_entry(path, entry, &st);
    return err ? err : trx_c64re_apply(st);
}

void trx_c64re_set_pending(TrxC64reStaged *st)
{
    TrxC64reStaged *old = pending_state;
    pending_state = st;
    trx_c64re_discard(old);                     // a second choice replaces the first
}

bool trx_c64re_pending(void)
{
    return pending_state != NULL;
}

void trx_c64re_unfreeze(void)
{
    TrxC64reStaged *st = pending_state;
    pending_state = NULL;
    if (!st) {
        return;
    }
    const char *err = trx_c64re_apply(st);
    if (err) {
        // The menu is gone: nothing can be shown any more. The machine is
        // released either way (put_back or the early exit in load_state).
        printf("[C64RE] loading the snapshot failed: %s\n", err);
    }
}
