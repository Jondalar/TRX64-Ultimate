// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_c64re.cc and trx_drive.cc (.c64re and .c64rering from the frame ring, drive A's
// own state; GPL-3.0), ported for the ring library (U18 D6: this copy is the master). Changes: files through api->fs (RFile), memory through api->mem,
// text through r_snprintf; what the firmware asks the machine (System Mode, cartridge, drive A, their files) comes from the source each entry builds
// (ringmachine.cc, trx_c64re_source_begin), and a load builds the machine through that source's hooks; drive A's memory through the export "cbm1541";
// the cartridge image of a format 2 file is streamed into the machine at the load (TrxLoadMem::rom_put) instead of held; trx_c64re_dump_machine() is
// the library's own (FW apps/c64re).
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
#include "ringsys.h"
#include <string.h>

#include "trx_sha256.h"
#include "trx_json.h"                // jsmn (the firmware's software/api/jsmn.h, copied, MIT) and the token helpers
#include "trx_inflate.h"
#include "trx_c64re_nodes.h"
#include "trx_trxring.h"
#include "trx_drive.h"
#include "ringmachine.h"

#define LAYOUT         1
#define PC_ADJ         0     // chip 1 w1's PC at the snap edge minus the next opcode's address
#define W_CPU          1
#define W_VIC          4
#define W_CIA1         16
#define W_CIA2         21
#define W_CIA1H        34
#define W_CIA2H        46
#define W_CPUH         27

// ---- gzip with stored blocks, written as the document is produced ----------------
#define GZ_BLOCK 16384
#define GZ_HEAD  5          // a stored block's header: final flag, LEN, NLEN

// c64_basic's rvlite has an 8 KB instruction cache and an 8 KB write-through data cache with 16-byte lines (U19: cache_cfg_pkg.vhd, rv_dcache.vhd),
// so a load that misses is a DDR2 access and every store goes to DDR2. The hot paths keep their state in registers: per output byte one load (the CRC table), per input byte one load, the
// stores into the block. The base64 digit is computed, not looked up (a table would be one more load per byte); the decimals are made with
// multiplies (the build is -mno-div: a division is a library call).
static inline __attribute__((always_inline)) uint32_t b64c(uint32_t v)
{
    v &= 63;
    return v < 26 ? 'A' + v : v < 52 ? 'a' - 26 + v : v < 62 ? '0' - 52 + v : v == 62 ? '+' : '/';
}

// v / 10 for v < 1029 (v * 205 / 2048), and for any 32-bit v (the reciprocal 0xCCCCCCCD / 2^35)
static inline uint32_t div10_small(uint32_t v) { return (v * 205u) >> 11; }
static inline uint32_t div10(uint32_t v) { return uint32_t((uint64_t(v) * 0xCCCCCCCDu) >> 35); }

// Three ways to use it. init(): the stream in order, from the file's position on. init_count(): nothing kept or written, isize counts the bytes the
// same calls would give. init_pieces(): a stream whose length is known, written as pieces in any order, each at its place in the file (block
// headers where its bytes start a block); piece_end() gives a piece's own CRC-32, the caller joins them (crc32_combine) for finish_pieces().
class Gz {
    RFile *f;
    uint8_t *buf;
    int fill;
    int lim;                    // the fill that ends the block: GZ_BLOCK, less for a piece that starts inside a block
    uint32_t crc, isize;
    bool hash;
    bool count;
    bool pieces;
    uint64_t data;              // pieces: the file offset of block 0's header
    uint32_t total;             // pieces: the stream's length
    uint32_t at;                // pieces: the stream offset of buf[0]
public:
    TrxSha256 sha;
    bool failed;
    uint32_t written;
    uint32_t write_ms;

    // no constructor or destructor (R3): init() starts the stream, done() gives the block back
    void init(RFile *file, bool with_hash)
    {
        f = file;
        fill = 0;
        lim = GZ_BLOCK;
        crc = 0xFFFFFFFFu;
        isize = 0;
        hash = with_hash;
        count = false;
        pieces = false;
        written = 0;
        write_ms = 0;
        trx_sha_init(&sha);
        buf = (uint8_t *)r_malloc(GZ_BLOCK);
        failed = !buf;
        static const uint8_t header[10] = { 0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0x03 };
        raw(header, sizeof(header));
    }
    void init_count()
    {
        f = 0;
        buf = 0;
        fill = 0;
        lim = GZ_BLOCK;
        crc = 0xFFFFFFFFu;
        isize = 0;
        hash = false;
        count = true;
        pieces = false;
        failed = false;
        written = 0;
        write_ms = 0;
    }
    // The gzip header at the file offset `member`; `len` bytes of stream follow.
    void init_pieces(RFile *file, uint64_t member, uint32_t len)
    {
        rf_seek(file, member);
        init(file, false);
        pieces = true;
        data = member + 10;
        total = len;
    }
    // A piece from stream offset `from` on.
    void piece(uint32_t from)
    {
        at = from;
        fill = 0;
        lim = GZ_BLOCK - int(from % GZ_BLOCK);
        crc = 0xFFFFFFFFu;
        isize = 0;
    }
    uint32_t piece_end()
    {
        if (fill) {
            flush(false);
        }
        return crc ^ 0xFFFFFFFFu;
    }
    // The file offset of stream byte `u` (pieces).
    uint64_t file_at(uint32_t u) const
    {
        return data + uint64_t(u / GZ_BLOCK) * (GZ_HEAD + GZ_BLOCK) + GZ_HEAD + u % GZ_BLOCK;
    }
    // The empty final block when the stream ends on a block boundary, then the trailer: crc32 is the whole stream's.
    bool finish_pieces(uint32_t crc32)
    {
        if (!(total % GZ_BLOCK)) {
            static const uint8_t last[GZ_HEAD] = { 1, 0, 0, 0xFF, 0xFF };
            rf_seek(f, file_at(total) - GZ_HEAD);
            raw(last, GZ_HEAD);
        }
        uint8_t tail[8];
        for (int i = 0; i < 4; i++) {
            tail[i] = uint8_t(crc32 >> (8 * i));
            tail[4 + i] = uint8_t(total >> (8 * i));
        }
        rf_seek(f, file_at(total));
        raw(tail, 8);
        return !failed;
    }
    // The file's length once finish_pieces wrote the trailer.
    uint64_t pieces_size() const { return file_at(total) + 8; }
    // The stream's bytes so far: counted (init_count), or the piece's own since piece().
    uint32_t length() const { return isize; }
    void done()
    {
        r_free(buf);
        buf = 0;
    }

    void raw(const uint8_t *p, uint32_t len)
    {
        if (failed) {
            return;
        }
        if (hash) {
            trx_sha_add(&sha, p, len);
        }
        uint16_t t = r_ms_timer();
        if (!rf_write(f, p, len)) {
            failed = true;
        }
        write_ms += uint16_t(r_ms_timer() - t);
        written += len;
    }

    // In order: the block's header, then its bytes. A piece: the bytes at their place, the header in front when they start the block (the stream's
    // length says whether it is the final one and how long).
    void flush(bool final)
    {
        int len = fill;
        bool head_too = true;
        if (pieces) {
            uint32_t j = at / GZ_BLOCK;
            head_too = !(at % GZ_BLOCK);
            final = j == total / GZ_BLOCK;
            len = final ? int(total % GZ_BLOCK) : GZ_BLOCK;
            rf_seek(f, file_at(at) - (head_too ? GZ_HEAD : 0));
            at += fill;
            lim = GZ_BLOCK;
        }
        if (head_too) {
            uint8_t head[GZ_HEAD];
            head[0] = final ? 1 : 0;
            head[1] = uint8_t(len);
            head[2] = uint8_t(len >> 8);
            head[3] = uint8_t(~len);
            head[4] = uint8_t((~len) >> 8);
            raw(head, GZ_HEAD);
        }
        if (fill) {
            raw(buf, fill);
        }
        fill = 0;
    }

    void put(const char *s, int len)
    {
        if (count) {
            isize += len;
            return;
        }
        const uint32_t *const tab = trx_crc32_table();
        uint32_t c = crc;
        int fl = fill, lm = lim;
        uint8_t *const b = buf;
        for (int i = 0; i < len; i++) {
            uint32_t ch = uint8_t(s[i]);
            c = (c >> 8) ^ tab[(c ^ ch) & 0xFF];
            b[fl++] = uint8_t(ch);
            if (fl == lm) {
                fill = fl;
                flush(false);
                fl = 0;
                lm = lim;
            }
        }
        crc = c;
        fill = fl;
        isize += len;
    }

    void str(const char *s) { put(s, strlen(s)); }

    // Decimal, written here (the SDK's printf has no 64-bit conversions): the digits above 2^32 by 64-bit division (a library call, the cycle
    // counts only), the rest by the reciprocal.
    void num(const char *prefix, int64_t v)
    {
        char out[24];
        int n = sizeof(out);
        if (prefix) {
            str(prefix);
        }
        uint64_t u = v < 0 ? uint64_t(-v) : uint64_t(v);
        while (u >> 32) {
            out[--n] = char('0' + u % 10);
            u /= 10;
        }
        uint32_t w = uint32_t(u);
        do {
            uint32_t q = div10(w);
            out[--n] = char('0' + (w - q * 10));
            w = q;
        } while (w);
        if (v < 0) {
            out[--n] = '-';
        }
        put(out + n, int(sizeof(out)) - n);
    }

    // a whole array as base64, '=' padded
    void b64(const uint8_t *data, int len)
    {
        if (count) {
            isize += 4 * uint32_t((len + 2) / 3);
            return;
        }
        const uint32_t *const tab = trx_crc32_table();
        uint32_t c = crc;
        int fl = fill, lm = lim;
        uint8_t *const b = buf;
        const int whole = len - len % 3;
        int outn = 0;
        for (int i = 0; i < len; i += 3) {
            uint32_t v, pad;            // pad: how many of the four digits are '=' (the last group only)
            if (i < whole) {
                v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
                pad = 0;
            } else {
                v = uint32_t(data[i]) << 16;
                pad = 2;
                if (len - i > 1) {
                    v |= uint32_t(data[i + 1]) << 8;
                    pad = 1;
                }
            }
            for (int k = 18; k >= 0; k -= 6) {
                uint32_t ch = (k < int(6 * pad)) ? uint32_t('=') : b64c(v >> k);
                c = (c >> 8) ^ tab[(c ^ ch) & 0xFF];
                b[fl++] = uint8_t(ch);
                if (fl == lm) {
                    fill = fl;
                    flush(false);
                    fl = 0;
                    lm = lim;
                }
            }
            outn += 4;
        }
        crc = c;
        fill = fl;
        isize += outn;
    }

    void ta(const uint8_t *data, int len)
    {
        str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
        b64(data, len);
        str("\"}");
    }

    // a string as JSON text: the quote, the backslash and the control characters escaped
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

    // a file as a typed array, streamed (3072 bytes a piece is a whole number of base64 groups): a cartridge or a disk image goes into the document without
    // a copy of it in memory. A read that fails leaves the array short; failed is set.
    void file_ta(RFile *f, uint32_t len)
    {
        str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
        if (count) {                    // the pieces are whole base64 groups but the last: the same count as the whole length
            b64(0, int(len));
            str("\"}");
            return;
        }
        uint8_t piece[3072];
        uint32_t done = 0;
        while (done < len && !failed) {
            uint32_t want = len - done;
            if (want > sizeof(piece)) {
                want = sizeof(piece);
            }
            if (!rf_read(f, piece, want)) {
                failed = true;
                break;
            }
            b64(piece, int(want));
            done += want;
        }
        str("\"}");
    }

    // the same through a reader (the cartridge image through the source's rom_read), in the same 3 KB pieces
    void read_ta(void (*rd)(void *ctx, uint32_t off, uint8_t *dst, uint32_t n), void *ctx, uint32_t len)
    {
        str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
        if (count) {
            b64(0, int(len));
            str("\"}");
            return;
        }
        uint8_t piece[3072];
        for (uint32_t done = 0; done < len && !failed;) {
            uint32_t want = len - done > sizeof(piece) ? uint32_t(sizeof(piece)) : len - done;
            rd(ctx, done, piece, want);
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
            uint32_t v = data[i] & mask;            // 0-255
            uint32_t t = div10_small(v);
            uint32_t u = v - t * 10;
            if (i) {
                out[on++] = ',';
            }
            if (v >= 100) {
                uint32_t h = div10_small(t);
                out[on++] = char('0' + h);
                t -= h * 10;
            }
            if (v >= 10) {
                out[on++] = char('0' + t);
            }
            out[on++] = char('0' + u);
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

// CRC-32 of A then B from crc(A), crc(B) and B's length (zlib's crc32_combine): crc(A) times x^(8 * len_b) modulo the polynomial, plus crc(B).
static uint32_t crc_mulmod(uint32_t a, uint32_t b)
{
    uint32_t m = 1u << 31, p = 0;
    for (;;) {
        if (a & m) {
            p ^= b;
            if (!(a & (m - 1))) {
                break;
            }
        }
        m >>= 1;
        b = (b & 1) ? (b >> 1) ^ 0xEDB88320u : b >> 1;
    }
    return p;
}

static uint32_t crc32_combine(uint32_t crc_a, uint32_t crc_b, uint32_t len_b)
{
    uint32_t sq = 1u << 23;                 // x^8, bit 31 being x^0
    uint32_t p = 1u << 31;
    for (; len_b; len_b >>= 1) {
        if (len_b & 1) {
            p = crc_mulmod(sq, p);
        }
        sq = crc_mulmod(sq, sq);
    }
    return crc_mulmod(p, crc_a) ^ crc_b;
}

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

// The checkpoint tree of one entry (an object, without a key in front).
// TRX64 0.12.2's turbo node (T20 §10.1, §17.5), the one place that builds it. The core has no
// turbo yet (T29): the machine always runs at 1 MHz with badline stalls, which is the registers-
// mode reset state, written out. T29 puts its state (regs_en, prefer, d030_b0, d031, hold) on
// the state bus; this takes it from the frame's block words then.
// With the T29 engine (CAPS2 b6) it is the engine's live state: TURBO_W1/W2 (TRX64-Ultimate docs/specs/T29-turbo.md
// §12.7). Without it the machine runs at 1 MHz with badline stalls, the registers-mode reset state, written out.
static void write_turbo(Gz &g, const TrxFrame &)
{
    if (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK) {
        // T29: the engine's live state (TURBO_W1/W2); the ring records at 1 MHz only, so a frame's
        // node is the state of the machine when it is written, not per frame
        uint32_t w1 = HW_RD(trx_turbo_w1), w2 = HW_RD(trx_turbo_w2);
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

// ---- what the file says of the machine (T228 R20) ---------------------------------
static const char *const machine_names[6] = { "c64-pal", "c64-ntsc", "c64-pal60", "c64-ntsc50", "c64-pal60l", "c64-ntsc50l" };
static TrxC64reSrc g_src;
static bool g_src_on;
static RingMachine g_mach;          // the strings and the EEPROM of a source an entry built for its own call

void trx_c64re_set_source(const TrxC64reSrc *src)
{
    if (src) {
        g_src = *src;
    }
    g_src_on = src != NULL;
}

const TrxC64reSrc *trx_c64re_source(void)
{
    return g_src_on ? &g_src : NULL;
}

bool trx_c64re_source_begin(bool load)
{
    if (g_src_on) {
        return false;
    }
    ring_machine_build(&g_mach, load);
    trx_c64re_set_source(&g_mach.src);
    return true;
}

void trx_c64re_source_end(bool built)
{
    if (built) {
        trx_c64re_set_source(NULL);
    }
}

static bool is_dir(const char *path)
{
    uint64_t size;
    bool dir = false;
    return g_api->fs && g_api->fs->stat(path, &size, &dir) && dir;
}

// Where a state file belongs, and what to call it: beside the image that is
// running (Alex, 2026-11-07 - "neben das laufende Image, source kennt den
// Ordner"). The cartridge carries its own source path; with no cartridge, the
// image on drive A does; with neither, the first volume that is there and its
// timewarp/ folder. `base` gets the image's name without its extension, which
// the file is then named after. Both come from the source.
static bool place_body(char *dir, int dirlen, char *base, int baselen);

bool trx_c64re_place(char *dir, int dirlen, char *base, int baselen)
{
    bool built = trx_c64re_source_begin(false);
    bool ok = place_body(dir, dirlen, base, baselen);
    trx_c64re_source_end(built);
    return ok;
}

static bool place_body(char *dir, int dirlen, char *base, int baselen)
{
    char full[192];
    full[0] = 0;
    if (g_src_on && g_src.cart_src && g_src.cart_src[0]) {
        r_snprintf(full, sizeof(full), "%s", g_src.cart_src);
    } else if (g_src_on && g_src.disk_path && g_src.disk_path[0]) {
        r_snprintf(full, sizeof(full), "%s", g_src.disk_path);
    }
    const char *cut = strrchr(full, '/');
    if (full[0] && cut && cut != full && int(cut - full) < dirlen) {
        int dn = int(cut - full);
        memcpy(dir, full, dn);
        dir[dn] = 0;
        r_snprintf(base, baselen, "%s", cut + 1);
        char *dot = strrchr(base, '.');
        if (dot) {
            *dot = 0;
        }
        if (is_dir(dir)) {
            return true;
        }
    }
    r_snprintf(base, baselen, "state");
    static const char *volumes[] = { "/Usb0", "/Usb1", "/SD" };
    for (unsigned i = 0; i < sizeof(volumes) / sizeof(volumes[0]); i++) {
        if (!is_dir(volumes[i])) {
            continue;
        }
        r_snprintf(dir, dirlen, "%s/timewarp", volumes[i]);
        if (!is_dir(dir) && g_api->fs) {
            g_api->fs->mkdir(dir);
        }
        if (is_dir(dir)) {
            return true;
        }
    }
    return false;
}

static const char *machine_name(void)
{
    return (g_src_on && g_src.machine >= 0 && g_src.machine < 6) ? machine_names[g_src.machine] : machine_names[0];
}

#define W_GLUE         0
#define W_CART         26
#define GLUE_CART      0x08

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
// table walks, the type the machine has configured, and, when cart is given, the
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
    // cartBytes (T20 §9.3): the .crt the machine loaded, as it is on disk. Read
    // from the file, not written out of the image: the image may differ from the
    // file (a game that programmed its flash), which cartFlash below carries.
    const char *src = g_src_on ? g_src.cart_src : NULL;
    RFile cf = { 0, 0 };
    uint32_t crt_len = 0;
    bool open = src && *src && rf_open(&cf, src);
    if (open) {
        crt_len = uint32_t(rf_size(&cf));
    }
    g.str("\"cartBytes\":");
    if (!crt_len) {
        g.str("null");
    } else {
        g.file_ta(&cf, crt_len);
    }
    if (open) {
        rf_close(&cf);
    }
    // cartFlash: the live window, for a cartridge whose flash the C64 can
    // program - that is the one case where the image can differ from the file,
    // and the ring's log does not carry flash writes, so only this node brings
    // it back.
    g.str(",\"cartFlash\":");
    uint32_t rom_len = (g_src_on && g_src.cart_writable && g_src.rom_read) ? g_src.rom_bytes : 0;
    if (!rom_len) {
        g.str("null");
    } else {
        g.read_ta(g_src.rom_read, g_src.rom_ctx, rom_len);
    }
    g.str(",\"cartState\":{");
    g.num("\"currentBank\":", uint8_t(w[W_CART]));
    g.num(",\"controlRegister\":", uint8_t(w[W_CART] >> 8));
    g.num(",\"cartridgeType\":", CART_RD(type));
    g.str(",\"source\":\"");
    g.jstr(src ? src : "");
    g.str("\"");
    if (cart) {
        g.str(",\"cartRam\":");
        g.ta(cart, TRX_CART_RAM_BYTES);
    }
    g.str("}");
}

// T66 part 8: drive A's own state, read by a stub on its CPU (trx_drive_read).
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

// T20 §9.3 "media" and "drivePart": which file the source has on drive A and
// whether that drive is powered. The image itself is not in here; this says
// which file the state belongs to.
static void write_media(Gz &g)
{
    g.str("\"drivePart\":");
    if (!g_src_on || !g_src.drive_type) {
        g.str("null,\"media\":null");
        return;
    }
    g.str(g_src.drive_power ? "{\"power\":true" : "{\"power\":false");
    g.num(",\"unit\":", g_src.drive_unit);
    g.str(",\"driveType\":\"");
    g.jstr(g_src.drive_type);
    g.str("\"}");
    const char *full = g_src.disk_path;
    const char *name = full ? strrchr(full, '/') : NULL;
    name = name ? name + 1 : full;
    g.str(",\"media\":");
    if (!name || !*name) {
        g.str("null");
        return;
    }
    g.str("{\"diskPath\":\"");
    g.jstr(full);
    g.str("\",\"imageFormat\":\"");
    const char *dot = strrchr(name, '.');
    g.jstr(dot ? dot + 1 : "");
    g.str("\"}");
}

// The media of the manifest: media0 the cartridge, media1 (or media0 without a cartridge) the disk image, each hashed and streamed into mediaPayloads
// by the dump with no ring (machine_step below).
struct MediaFile {
    const char *path;
    const char *role;
    char name[80];
    char format[8];
    uint32_t size;
    uint8_t digest[32];
};

// The name and the format (the extension, lower case) of a medium, from its path.
static void media_name(MediaFile *m, const char *path, const char *role)
{
    m->path = path;
    m->role = role;
    const char *sl = strrchr(path, '/');
    strncpy(m->name, sl ? sl + 1 : path, sizeof(m->name) - 1);
    m->name[sizeof(m->name) - 1] = 0;
    const char *dot = strrchr(m->name, '.');
    m->format[0] = 0;
    for (int i = 0; dot && dot[1 + i] && i < int(sizeof(m->format)) - 1; i++) {
        char c = dot[1 + i];
        m->format[i] = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        m->format[i + 1] = 0;
    }
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

static const char *open_out(const char *path, RFile *f)
{
    if (!rf_create(f, path)) {
        return "The file could not be created";
    }
    return NULL;
}

// ---- drive A's own state (TRX64-Ultimate io/c64/trx_drive.cc, T66 part 8) ---------------------------------------------------------------------------
// Way B, no gateware: the drive's RAM is in DDR2 and read there; its CPU's registers and both 6522s are not on a bus the app reaches, so a stub of 0x90
// bytes runs on the drive's own CPU in the free part of the stack page, writes them into memory the app reads, and puts the drive back where it was. The
// drive is held (CMD_DRV_PAUSE) and moved on with CMD_DRV_STEP (drv_dbg.v), so the C64 may run, be paused or frozen. Limits as there: reading a 6522
// clears interrupt flags, the timers run during the stub's cycles, the T2 latch low is not readable.
// The drive's debugger is trx_debug's drive half (c64_basic's TOML A: cmd drv_pause / drv_run / drv_step, arg, drv_stat, drv_cycles).
#define STUB_BASE      0x0100
#define STUB_SIZE      0x90
#define STUB_NEED      (STUB_SIZE + 4)

// Drive A's memory image (64 KB in DDR2) through the export "cbm1541", slot 0: image_read, image_write (a write is complete when the call returns).
// The RAM is $0000-$07FF, mirrored up to $1800. dread / dwrite take a range inside the RAM; without the export a read gives zeros and a write is dropped.
static uint32_t dimg(uint16_t a) { return a < 0x1800 ? (a & 0x07FFu) : a; }
static void dread(uint16_t a, uint8_t *dst, uint32_t n)
{
    if (const Cbm1541Api *d = R_TABLE(Cbm1541Api, RX_CBM1541, image_read)) {
        d->image_read(0, dimg(a), dst, n);
    } else {
        memset(dst, 0, n);
    }
}
static void dwrite(uint16_t a, const uint8_t *src, uint32_t n)
{
    if (const Cbm1541Api *d = R_TABLE(Cbm1541Api, RX_CBM1541, image_write)) {
        d->image_write(0, dimg(a), src, n);
    }
}
static uint8_t dpeek(uint16_t a)
{
    uint8_t v;
    dread(a, &v, 1);
    return v;
}
static void dpoke(uint16_t a, uint8_t v) { dwrite(a, &v, 1); }
// JMP STUB_BASE at `at` (3 bytes)
static void djmp_stub(uint16_t at)
{
    const uint8_t jmp[3] = { 0x4C, uint8_t(STUB_BASE), uint8_t(STUB_BASE >> 8) };
    dwrite(at, jmp, 3);
}

static bool drv_wait(uint32_t mask, uint32_t want, int ms)
{
    uint16_t t0 = r_ms_timer();
    while (uint16_t(r_ms_timer() - t0) < uint16_t(ms)) {
        if ((HW_RD(trx_debug_drv_stat) & mask) == want) {
            return true;
        }
    }
    return (HW_RD(trx_debug_drv_stat) & mask) == want;
}
static bool drv_hold(void)
{
    HW_WR(trx_debug_cmd, DRV_trx_debug_cmd_drv_pause);
    return drv_wait(DRV_trx_debug_drv_stat_halted_MASK, DRV_trx_debug_drv_stat_halted_MASK, 50);
}
static void drv_free(void) { HW_WR(trx_debug_cmd, DRV_trx_debug_cmd_drv_run); }
static bool drv_step(int n)
{
    HW_WR(trx_debug_arg, uint32_t(n));
    HW_WR(trx_debug_cmd, DRV_trx_debug_cmd_drv_step);
    return drv_wait(DRV_trx_debug_drv_stat_stepping_MASK, 0, 200);
}
static uint16_t drv_pc(void) { return uint16_t((HW_RD(trx_debug_drv_stat) & DRV_trx_debug_drv_stat_pc_MASK) >> DRV_trx_debug_drv_stat_pc_SHIFT); }

// The stubs are assembled by hand, as there, 0x90 bytes each: code first, then data. The save stub: 68 A, 69 X, 6A Y, 6B S, 6C P, 6D MAGIC, 6E FLAG,
// 6F MAGIC2, 70..7F VIA 1, 80..8F VIA 2.
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
    /* 00 */ 0x08, 0x8D, SV_A, 0x01, 0x68, 0x8D, SV_P, 0x01, 0x78,
    /* 09 */ 0x8E, SV_X, 0x01, 0x8C, SV_Y, 0x01, 0xBA, 0x8E, SV_S, 0x01,
    /* 13 */ 0xAD, 0x0D, 0x18, 0x8D, SV_V1 + 13, 0x01, 0xAD, 0x0D, 0x1C, 0x8D, SV_V2 + 13, 0x01,
    /* 1F */ 0xA2, 0x00, 0xE0, 0x0D, 0xF0, 0x0C, 0xBD, 0x00, 0x18, 0x9D, SV_V1, 0x01, 0xBD, 0x00, 0x1C, 0x9D, SV_V2, 0x01,
    /* 31 */ 0xE8, 0xE0, 0x10, 0xD0, 0xEB,
    /* 36 */ 0xA9, SV_MARK, 0x8D, SV_MAGIC, 0x01,
    /* 3B */ 0xAD, SV_FLAG, 0x01, 0xF0, 0xFB,
    /* 40 */ 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00,
    /* 4F */ 0xA9, SV_MARK2, 0x8D, SV_MAGIC2, 0x01,
    /* 54 */ 0xAD, SV_P, 0x01, 0x48, 0xAE, SV_X, 0x01, 0xAC, SV_Y, 0x01, 0xAD, SV_A, 0x01, 0x28,
    /* 62 */ 0x4C, 0x00, 0x00
};
#define SVP_B0     0x41
#define SVP_A0     0x43
#define SVP_B1     0x46
#define SVP_A1     0x48
#define SVP_B2     0x4B
#define SVP_A2     0x4D
#define SVP_JMP    0x63
// The load stub: 6D MAGIC, 70..7F VIA 1, 80..8F VIA 2.
#define LD_MAGIC   0x6D
#define LD_V1      0x70
#define LD_V2      0x80
static const uint8_t load_stub[STUB_SIZE] = {
    /* 00 */ 0xA9, 0x7F, 0x8D, 0x0E, 0x18, 0x8D, 0x0E, 0x1C,
    /* 08 */ 0xAD, LD_V1 + 11, 0x01, 0x8D, 0x0B, 0x18, 0xAD, LD_V1 + 12, 0x01, 0x8D, 0x0C, 0x18,
    /* 14 */ 0xAD, LD_V2 + 11, 0x01, 0x8D, 0x0B, 0x1C, 0xAD, LD_V2 + 12, 0x01, 0x8D, 0x0C, 0x1C,
    /* 20 */ 0xA2, 0x00, 0xE0, 0x0D, 0xF0, 0x0C, 0xBD, LD_V1, 0x01, 0x9D, 0x00, 0x18, 0xBD, LD_V2, 0x01, 0x9D, 0x00, 0x1C,
    /* 32 */ 0xE8, 0xE0, 0x10, 0xD0, 0xEB,
    /* 37 */ 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00,
    /* 46 */ 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00, 0xA9, 0x00, 0x8D, 0x00, 0x00,
    /* 55 */ 0xA9, SV_MARK2, 0x8D, LD_MAGIC, 0x01,
    /* 5A */ 0xA2, 0x00, 0x9A, 0xA9, 0x00, 0x48, 0xA9, 0x00, 0x48, 0xA9, 0x00, 0x48, 0xA2, 0x00, 0xA0, 0x00, 0xA9, 0x00, 0x40
};
#define LDP_SLOT0  0x37
#define LDP_SLOT_N 6
#define LDP_SP     0x5B
#define LDP_PCH    0x5E
#define LDP_PCL    0x61
#define LDP_P      0x64
#define LDP_X      0x67
#define LDP_Y      0x69
#define LDP_A      0x6B

// Opcode lengths and the opcodes that have exactly one successor (FW io/c64/trx_6502.h).
static const uint8_t drv_op_len[256] = {
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    3, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,  2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
};
static bool op_falls_through(uint8_t op)
{
    switch (op) {
    case 0x00: case 0x20: case 0x40: case 0x4C: case 0x6C: case 0x60:
    case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xB0: case 0xD0: case 0xF0:
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
        return false;
    default:
        return true;
    }
}

static uint16_t find_patch(uint16_t *at_dpc, uint8_t *dpc_len)
{
    for (int tries = 0; tries < 400; tries++) {
        uint16_t pc = drv_pc();
        if (pc < 0x0800) {
            uint8_t op = dpeek(pc);
            uint8_t len = drv_op_len[op];
            uint16_t next = uint16_t(pc + len);
            if (op_falls_through(op) && next + 3 <= 0x0800) {
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

// Drive A is powered as the source says (the firmware asks its drive object; the library has no other way to know).
static bool drive_powered(void)
{
    return g_src_on && g_src.drive_type && g_src.drive_power;
}

bool trx_drive_present(void)
{
    return (trx_io_caps() & DRV_ident_caps_debug_MASK) && drive_powered();
}

const char *trx_drive_read(TrxDriveState *st, bool with_regs)
{
    memset(st, 0, sizeof(*st));
    if (!(trx_io_caps() & DRV_ident_caps_debug_MASK)) {
        return "This bitstream has no debug page, so drive A cannot be read";
    }
    if (!drive_powered()) {
        return "Drive A is not powered";
    }
    dread(0, st->ram, TRX_DRIVE_RAM);
    st->cycles = HW_RD(trx_debug_drv_cycles);
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
        r_log("[DRIVE] drive A runs in its ROM, not in its own RAM: the file carries its RAM, not its CPU and VIA registers\n");
        return NULL;                // not an error: st->regs stays false
    }
    uint8_t page[256], back[3];
    dread(STUB_BASE, page, 256);
    dread(next, back, 3);
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
    dwrite(STUB_BASE, stub, STUB_SIZE);
    djmp_stub(next);
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
        uint8_t sv[STUB_SIZE];
        dread(STUB_BASE, sv, STUB_SIZE);
        st->a = sv[SV_A];
        st->x = sv[SV_X];
        st->y = sv[SV_Y];
        st->sp = sv[SV_S];
        st->p = sv[SV_P];
        st->pc = next;
        memcpy(st->via1, sv + SV_V1, 16);
        memcpy(st->via2, sv + SV_V2, 16);
        st->cycles = HW_RD(trx_debug_drv_cycles);
        st->regs = true;
        if (st->sp < STUB_NEED) {
            r_log("[DRIVE] drive A's stack pointer was $%02X, below the stub's %d bytes: its stack was written over and is put back, but the drive may have lost data\n",
                   st->sp, STUB_SIZE);
        }
    }
    dpoke(STUB_BASE + SV_FLAG, 1);
    bool back_out = false;
    for (int i = 0; i < 20 && !back_out; i++) {
        if (!drv_step(100)) {
            break;
        }
        back_out = dpeek(STUB_BASE + SV_MAGIC2) == SV_MARK2;
    }
    dwrite(next, back, 3);
    dwrite(STUB_BASE, page, 256);
    drv_free();
    if (!back_out && !err) {
        err = "Drive A did not come back out of the stub";
    }
    if (err) {
        st->regs = false;
    }
    return err;
}

const char *trx_drive_write(const TrxDriveState *st)
{
    if (!(trx_io_caps() & DRV_ident_caps_debug_MASK)) {
        return "This bitstream has no debug page, so drive A cannot be written";
    }
    if (!drive_powered()) {
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
    uint8_t keep[3];
    if (len) {
        dread(dpc, keep, len);
    }
    dwrite(0, st->ram, TRX_DRIVE_RAM);
    if (!st->regs) {
        drv_free();
        return NULL;
    }
    if (len) {
        dwrite(dpc, keep, len);
    }
    uint8_t stub[STUB_SIZE];
    memcpy(stub, load_stub, STUB_SIZE);
    for (int i = 0; i < 16; i++) {
        stub[LD_V1 + i] = st->via1[i];
        stub[LD_V2 + i] = st->via2[i];
    }
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
    while (nf < LDP_SLOT_N) {
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
    dwrite(STUB_BASE, stub, STUB_SIZE);
    djmp_stub(next);
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
    if (!err && !drv_step(4)) {
        err = "Drive A did not step";
    }
    dwrite(STUB_BASE, st->ram + STUB_BASE, STUB_SIZE);      // the stub's bytes as the file has them
    drv_free();
    return err;
}

// ---- format 2: state and medium, binary sections, streamed (FW io/c64/trx_c64re.cc, U8) ---------------------------------------------------------------
//   41 byte head ("C64RESNP", the format byte, SHA-256 of the body) | u32 header_len | JSON header (provenance only) | padded to 4 | u16 sections | u16 0 |
//   sections x { u8 id, u8 flags, u16 0, u32 len } | the payloads, each padded to 4. An id a reader does not know is skipped by its length.
#define C64RE_FORMAT    2
#define SEC_BLOCK       1
#define SEC_RAM         2
#define SEC_COLOR       3
#define SEC_CARTRAM     4
#define SEC_CARTROM     5
#define SEC_DRIVE       6
#define SEC_EEPROM      7
#define SEC_SLOTS       9
#define SEC_ENTRY       8
#define TRX_EEPROM_BYTES 2048
#define HDR_JSON_MAX    768
#define RAW_CHUNK       16384
#define BLOCK_SEC_BYTES (TRX_BLOCK_WORDS * 4 + 8)

// Writes straight through and hashes on the way; nothing is held back.
class Raw {
    RFile *f;
public:
    TrxSha256 sha;
    bool failed;
    uint32_t written;

    void init(RFile *file)                  // no constructor (R3)
    {
        f = file;
        failed = false;
        written = 0;
        trx_sha_init(&sha);
    }
    void put(const void *p, uint32_t len)
    {
        const uint8_t *b = (const uint8_t *)p;
        while (!failed && len) {
            uint32_t want = len > RAW_CHUNK ? RAW_CHUNK : len;
            trx_sha_add(&sha, b, want);
            if (!rf_write(f, b, want)) {
                failed = true;
                return;
            }
            written += want;
            b += want;
            len -= want;
        }
    }
    void u16(uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) }; put(b, 2); }
    void u32(uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) }; put(b, 4); }
    void pad(uint32_t len)
    {
        static const uint8_t zero[4] = { 0, 0, 0, 0 };
        if (len & 3) {
            put(zero, 4 - (len & 3));
        }
    }
};

// The block section: the chip block as 32 bit words, the snap position, and the turbo word when this machine has one (T29), laid out by hand.
static uint32_t block_section(uint8_t *dst, const TrxFrame &f)
{
    uint32_t n = 0;
    for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
        uint32_t w = f.w[i];
        dst[n++] = uint8_t(w);
        dst[n++] = uint8_t(w >> 8);
        dst[n++] = uint8_t(w >> 16);
        dst[n++] = uint8_t(w >> 24);
    }
    dst[n++] = uint8_t(f.x);
    dst[n++] = uint8_t(f.x >> 8);
    uint16_t flags = 0;
    uint32_t pack = 0;
    if (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK) {
        uint32_t w1 = HW_RD(trx_turbo_w1), w2 = HW_RD(trx_turbo_w2);
        uint32_t en = (w1 >> 8) & 7, mi = w1 & 15, d031 = (w1 >> 24) & 0xFF;
        uint32_t d030 = (w1 >> 11) & 1, hold = w2 & 0x7FFFFF, stalls = (w1 & 0x80) ? 1 : 0;
        pack = ((hold >> 8) << 17) | (d031 << 9) | (d030 << 8) | (en << 5) | (stalls << 4) | mi;
        flags |= 1;
    }
    dst[n++] = uint8_t(flags);
    dst[n++] = uint8_t(flags >> 8);
    dst[n++] = uint8_t(pack);
    dst[n++] = uint8_t(pack >> 8);
    dst[n++] = uint8_t(pack >> 16);
    dst[n++] = uint8_t(pack >> 24);
    return n;
}

// The 64 bit cycle count as decimal text, by hand (written for the firmware's printf, which has no 64 bit conversion).
static int u64dec(char *dst, int cap, uint64_t v)
{
    char tmp[24];
    int k = 0;
    do {
        tmp[k++] = char('0' + int(v % 10));
        v /= 10;
    } while (v);
    int n = 0;
    while (k && n < cap - 1) {
        dst[n++] = tmp[--k];
    }
    if (n < cap) {
        dst[n] = 0;
    }
    return n;
}

// A string into the header with the characters that would break the JSON left out.
static int json_str(char *dst, int cap, const char *src)
{
    int n = 0;
    for (const char *q = src; *q && n < cap - 1; q++) {
        if (*q != '"' && *q != '\\' && uint8_t(*q) >= 0x20) {
            dst[n++] = *q;
        }
    }
    if (n < cap) {
        dst[n] = 0;
    }
    return n;
}


// The bitstream's capability bits (board.fpga_caps), 0 without the board group.
static uint32_t fpga_caps(void)
{
    const SvcBoard *b = g_api->board;
    return b ? b->fpga_caps() : 0;
}

static int header_json(char *dst, int cap, const TrxFrame &f, uint32_t rom_len)
{
    const bool cart = (f.w[W_GLUE] & GLUE_CART) && g_src_on && g_src.cart_src && g_src.cart_src[0];
    const SvcSys *sys = g_api->sys;
    int n = r_snprintf(dst, cap,
                     "{\"kind\":\"c64re\",\"version\":%d,\"writer\":\"trx64u\",\"appVersion\":\"%s\""
                     ",\"machine\":\"%s\",\"bitstreamId\":%u,\"caps\":%u,\"caps2\":%u,\"blockWords\":%d",
                     C64RE_FORMAT, SVC_HAS(SvcSys, sys, app_version) ? sys->app_version() : "", machine_name(), (unsigned)HW_RD(ident_id),
                     (unsigned)HW_RD(ident_caps), (unsigned)HW_RD(ident_caps2), TRX_BLOCK_WORDS);
    uint8_t t[6];                                 // sys.wall_time: year - 2000, month, day, hour, minute, second
    if (SVC_HAS(SvcSys, sys, wall_time) && sys->wall_time(t)) {
        int y = 2000 + t[0];
        if (y >= 2024) {
            n += r_snprintf(dst + n, cap - n, ",\"createdAt\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\"", y, int(t[1]), int(t[2]), int(t[3]), int(t[4]),
                            int(t[5]));
        }
    }
    n += r_snprintf(dst + n, cap - n, ",\"frame\":%u,\"cycles\":", (unsigned)f.marker);
    n += u64dec(dst + n, cap - n, f.cycles);
    n += r_snprintf(dst + n, cap - n, ",\"pc\":%d", int((f.w[W_CPU + 1] & 0xFFFF) - PC_ADJ) & 0xFFFF);
    if (cart) {
        // the type is C64_CARTRIDGE_TYPE, the machine's own number (the one trx_ring_cart_state_ok() and a load compare), not the .crt's hardware type
        n += r_snprintf(dst + n, cap - n, ",\"cartridgeType\":%u,\"source\":\"", (unsigned)CART_RD(type));
        n += json_str(dst + n, cap - n, g_src.cart_src);
        n += r_snprintf(dst + n, cap - n, "\",\"mediumBytes\":%u", (unsigned)rom_len);
    }
    n += r_snprintf(dst + n, cap - n, ",\"machineState\":{");
    if (cart) {
        // require/prohibit: the cartridge definition's flags as c64cart reports them (0 when the base does not know them). Written for the file's
        // reader; no load here reads them back (TrxMachineWant parses them and nothing uses the fields).
        n += r_snprintf(dst + n, cap - n, "\"cartridge\":{\"type\":%u,\"bytes\":%u,\"require\":%u,\"prohibit\":%u,\"source\":\"",
                        (unsigned)CART_RD(type), (unsigned)rom_len, (unsigned)g_src.cart_require, (unsigned)g_src.cart_prohibit);
        n += json_str(dst + n, cap - n, g_src.cart_src);
        n += r_snprintf(dst + n, cap - n, "\"},");
    }
    if (g_src_on && g_src.drive_type && g_src.disk_path && g_src.disk_path[0]) {
        n += r_snprintf(dst + n, cap - n, "\"drive\":{\"image\":\"");
        n += json_str(dst + n, cap - n, g_src.disk_path);
        n += r_snprintf(dst + n, cap - n, "\",\"mode\":\"%s\"},", g_src.disk_mode == 2 ? "unlinked" : g_src.disk_mode == 1 ? "ro" : "rw");
    }
    const UciApi *uci = R_TABLE(UciApi, RX_UCI, slot_enable);
    n += r_snprintf(dst + n, cap - n, "\"settings\":{\"uci\":%s,\"reu\":%u,\"reuSize\":%u,\"sampler\":%s}}}",
                  (fpga_caps() & CAPAB_COMMAND_INTF) && uci && uci->slot_enable() ? "true" : "false",
                  (unsigned)(CART_RD(reu) ? 1 : 0), (unsigned)CART_RD(reu_size),
                  CART_RD(sampler) ? "true" : "false");
    return n;
}

// ---- the dumps, in steps (U18; MultiCore's decision of 2026-11-22: a ring dump is a stepped operation) ------------------------------------------------
// One state per dump on the heap. trx_dump_begin builds the machine the file describes once; each trx_dump_step does one piece under the caller's core
// lock (trx_c64re.h). The bytes are those of the one-call writers; the frame and the machine are the same writes in the same order, cut between calls,
// the ring's entries go into the file newest first, each at its place. The frame (format 2) and the ring (.c64rering) come from the timeline; the
// machine (format 1) is read through the DMA window, no ring needed.
#define DUMP_PIECE      3072            // a medium is read in pieces of this size: a whole number of base64 groups (Gz::file_ta)
#define DUMP_SLICE      (21 * DUMP_PIECE)   // the bytes of a medium one step hashes or writes
#define MAX_RING_ENTRIES 64


struct SecOut {
    uint8_t id;
    const uint8_t *p;      // NULL: the cartridge image, streamed through rom_read
    uint32_t len;
};

enum { DS_RUN, DS_DONE, DS_FAILED };
enum { FP_PREP, FP_DATA };                      // a frame: its memories and the file's head; then the sections, RAW_CHUNK a step
enum { RP_PICK, RP_ENTRY };                     // a ring: the entries picked, sized and the head written; one entry materialized and written a step
enum { MP_READ, MP_PROBE, MP_DATA };            // the machine: read; the media hashed, a slice a step; written, a slice a step, then the rest

struct TrxDump {
    int kind;                   // TRX_DUMP_*
    int phase;
    int state;                  // DS_*
    char path[260];
    RingMachine mach;           // what the file says of the machine, built by trx_dump_begin
    RFile file;
    bool file_on;               // the file is created and not closed
    uint32_t bytes;
    int entries;
    int step, steps;            // the progress: steps done, of about this many (0: not known yet)
    // format 2, one frame
    int idx;
    TrxFrame *f;
    uint8_t *ram, *col, *blk, *cart, *chunk;
    char *hdr;
    TrxDriveState *drive;
    SecOut sec[SEC_SLOTS];
    int ns, si;
    uint32_t off, rom_len;
    Raw w;
    // .c64rering
    int seconds, stride, n, k;
    int ix[MAX_RING_ENTRIES];
    uint32_t len[MAX_RING_ENTRIES];     // each entry's bytes in the gzip stream, counted before any is written
    uint32_t crc[MAX_RING_ENTRIES];     // each entry's CRC-32, once written
    uint32_t head_len, head_crc, tail_len, total;
    TrxFrame *fr;
    TrxMat *mat;
    Gz g;
    bool g_on;                  // g holds its block (Gz::init ran, Gz::done not yet)
    uint32_t size_ms, mat_ms, wr_ms;
    // format 1, the machine
    uint8_t vic[0x30];
    const char *cand[2];        // the media in the machine: the cartridge's .crt, drive A's image (paths in mach)
    const char *role[2];
    int ncand;
    MediaFile mf[2];
    int nm, mi;                 // the media read whole; the one being hashed or written
    RFile in;
    bool in_on;                 // that medium's file is open
    uint32_t in_done;
    TrxSha256 sh;
};

static uint32_t file_bytes(const char *path)
{
    uint64_t size = 0;
    bool dir = false;
    return (path && path[0] && g_api->fs && g_api->fs->stat(path, &size, &dir) && !dir) ? uint32_t(size) : 0;
}

static int slices(uint32_t len, uint32_t per)
{
    return int((len + per - 1) / per);
}

// ---- format 2: the frame ---------------------------------------------------------------------------------------------------------------------------------
static const char *frame_prep(TrxDump *d)
{
    d->f = (TrxFrame *)r_malloc(sizeof(TrxFrame));
    d->ram = (uint8_t *)r_malloc(65536);
    d->col = (uint8_t *)r_malloc(1024);
    d->blk = (uint8_t *)r_malloc(BLOCK_SEC_BYTES);
    d->hdr = (char *)r_malloc(HDR_JSON_MAX);
    d->chunk = (uint8_t *)r_malloc(RAW_CHUNK);
    TrxFrame *const f = d->f;
    if (!f || !d->ram || !d->col || !d->blk || !d->hdr || !d->chunk) {
        return "Out of memory";
    }
    if (!trx_ring_frame(d->idx, f)) {
        return "No such frame";
    }
    if (!f->block) {
        return "The frame has no chip block";
    }
    // with a cartridge on, its RAM window comes out of the same pause
    if (f->w[W_GLUE] & GLUE_CART) {
        d->cart = (uint8_t *)r_malloc(TRX_CART_RAM_BYTES);
    }
    if (int e = trx_ring_materialize(d->idx, d->ram, d->col, d->cart)) {
        return trx_ring_error(e);
    }
    // a dump this build could not load back is not written at all: the gate is the cartridge, because that is where the state reaches outside the chip block
    // (FW trx_c64re_dump_ex, U9 D0); one list answers here and in replay_cart()
    if (f->w[W_GLUE] & GLUE_CART) {
        const char *cname = NULL;
        if (!trx_ring_cart_state_ok(CART_RD(type), &cname)) {
            static char refused[96];
            r_snprintf(refused, sizeof(refused), "A state of this cartridge (type %02x) cannot be loaded back yet", (int)CART_RD(type));
            return refused;
        }
        if (trx_ring_cart_has_eeprom(CART_RD(type))) {
            // U9 D3a: GMod2's saved game is in the 93C86, outside the ROM window: without it the file would hold a cartridge whose save it cannot put back
            if (!(fpga_caps() & CAPAB_EEPROM)) {
                return "This bitstream has no EEPROM: a GMod2 state cannot be saved";
            }
            if (!(g_src_on && g_src.eeprom)) {
                return "The EEPROM could not be read: a GMod2 state cannot be saved";
            }
        }
    }
    // the medium: the cartridge image as it stands now, read straight out of DDR2 when it is written (not the .crt on the stick: the game may have
    // programmed the flash since)
    if ((f->w[W_GLUE] & GLUE_CART) && g_src_on && g_src.rom_read) {
        d->rom_len = g_src.rom_bytes;
    }
    // drive A, through the stub on its own CPU. A drive that cannot be entered gives its RAM only, and one that is off gives nothing; neither stops the
    // file from being written.
    if (trx_drive_present()) {
        d->drive = (TrxDriveState *)r_malloc(sizeof(TrxDriveState));
        const char *derr = d->drive ? trx_drive_read(d->drive, true) : "Out of memory";
        if (derr) {
            r_log("[C64RE] drive A is not in the file: %s\n", derr);
            r_free(d->drive);
            d->drive = NULL;
        }
    }

    SecOut *const sec = d->sec;
    int ns = 0;
    int hl = header_json(d->hdr, HDR_JSON_MAX, *f, d->rom_len);
    sec[ns].id = SEC_BLOCK;   sec[ns].p = d->blk; sec[ns].len = block_section(d->blk, *f); ns++;
    sec[ns].id = SEC_RAM;     sec[ns].p = d->ram; sec[ns].len = 65536;                      ns++;
    sec[ns].id = SEC_COLOR;   sec[ns].p = d->col; sec[ns].len = 1024;                       ns++;
    if (d->cart) {
        sec[ns].id = SEC_CARTRAM; sec[ns].p = d->cart; sec[ns].len = TRX_CART_RAM_BYTES; ns++;
    }
    if (d->rom_len) {
        sec[ns].id = SEC_CARTROM; sec[ns].p = NULL; sec[ns].len = d->rom_len;            ns++;
    }
    if (d->drive) {
        sec[ns].id = SEC_DRIVE; sec[ns].p = (const uint8_t *)d->drive; sec[ns].len = sizeof(TrxDriveState); ns++;
    }
    if (g_src_on && g_src.eeprom && (f->w[W_GLUE] & GLUE_CART)) {
        sec[ns].id = SEC_EEPROM; sec[ns].p = g_src.eeprom; sec[ns].len = TRX_EEPROM_BYTES; ns++;
    }
    d->ns = ns;
    if (const char *e = open_out(d->path, &d->file)) {
        return e;
    }
    d->file_on = true;
    uint8_t head[41];
    memset(head, 0, sizeof(head));
    memcpy(head, "C64RESNP", 8);
    head[8] = C64RE_FORMAT;
    if (!rf_write(&d->file, head, sizeof(head))) {
        return "The file could not be written";
    }
    Raw &w = d->w;
    w.init(&d->file);
    w.u32(uint32_t(hl));
    w.put(d->hdr, uint32_t(hl));
    w.pad(uint32_t(hl));
    w.u16(uint16_t(ns));
    w.u16(0);
    for (int i = 0; i < ns; i++) {
        uint8_t e[SEC_ENTRY] = { sec[i].id, 0, 0, 0, uint8_t(sec[i].len), uint8_t(sec[i].len >> 8), uint8_t(sec[i].len >> 16), uint8_t(sec[i].len >> 24) };
        w.put(e, SEC_ENTRY);
    }
    if (w.failed) {
        return "The file could not be written";
    }
    d->steps = 1;
    for (int i = 0; i < ns; i++) {
        d->steps += slices(sec[i].len, RAW_CHUNK);
    }
    d->si = 0;
    d->off = 0;
    d->phase = FP_DATA;
    return NULL;
}

// One RAW_CHUNK of the sections; after the last one the digest goes into the head and the file is complete.
static const char *frame_data(TrxDump *d)
{
    Raw &w = d->w;
    const SecOut &s = d->sec[d->si];
    uint32_t want = s.len - d->off > RAW_CHUNK ? RAW_CHUNK : s.len - d->off;
    if (s.p) {
        w.put(s.p + d->off, want);
    } else {
        g_src.rom_read(g_src.rom_ctx, d->off, d->chunk, want);
        w.put(d->chunk, want);
    }
    d->off += RAW_CHUNK;
    if (d->off >= s.len) {
        w.pad(s.len);
        d->si++;
        d->off = 0;
    }
    if (w.failed) {
        return "The file could not be written";
    }
    if (d->si < d->ns) {
        return NULL;
    }
    uint8_t digest[32];
    trx_sha_final(&w.sha, digest);
    if (!rf_seek(&d->file, 9) || !rf_write(&d->file, digest, 32)) {
        return "The file could not be written";
    }
    d->bytes = 41 + w.written;
    rf_close(&d->file);
    d->file_on = false;
    r_log("[C64RE] .c64re format %d: %u bytes, %d sections, medium %u\n", C64RE_FORMAT, (unsigned)d->bytes, d->ns, (unsigned)d->rom_len);
    d->state = DS_DONE;
    return NULL;
}

static const char *frame_step(TrxDump *d)
{
    return d->phase == FP_PREP ? frame_prep(d) : frame_data(d);
}

// ---- format 1: the machine with no ring (FW apps/c64re/src/c64re.c: "Save .C64RE Dump") -----------------------------------------------------------------
// The machine as it stands, read through the DMA window while the menu has it frozen: the 64 KB of RAM (the window's RAM-only mode, under the I/O too), the
// VIC registers and the colour RAM (a bus cycle, only while the machine is held), the CPU port from RAM $00/$01; format 1, the media that are in the machine
// (the cartridge's .crt and drive A's image) inside it. What the ring's chip block carries (the 6510's registers, the CIA and VIC internals) is not on any
// bus here and is left out; every node is optional on the reading side, so the dump opens as a machine to look at.
static const char *machine_read(TrxDump *d)
{
    d->ram = (uint8_t *)r_malloc(65536);
    d->col = (uint8_t *)r_malloc(1024);
    d->chunk = (uint8_t *)r_malloc(DUMP_PIECE);
    if (!d->ram || !d->col || !d->chunk) {
        return "Out of memory";
    }
    if (!g_api->c64) {
        return "This core has no C64 memory window";
    }
    uint8_t *const ram = d->ram, *const col = d->col;
    bool old = HW_RD(c64_dma_mode_memonly) != 0;
    io_memonly(true);
    g_api->c64->dma_read(0, ram, 65536);
    uint32_t st = HW_RD(trx_debug_status);                    // the debugger's status: the gate shut and a stop in force make the window's D page a bus cycle
    if ((st & DRV_trx_debug_status_shut_MASK) && (st & (DRV_trx_debug_status_stop_MASK | DRV_trx_debug_status_fw_stop_MASK))) {
        io_memonly(false);
        for (int r = 0; r < 0x30; r++) {
            d->vic[r] = c64_peek(0xD000 + r);
        }
        for (int a = 0; a < 1024; a++) {
            col[a] = c64_peek(0xD800 + a) & 0x0F;
        }
    } else {
        r_log("[C64RE] the machine is not held: the VIC registers and the colour RAM are not in the dump\n");
        memset(col, 0, 1024);
    }
    io_memonly(old);
    if (const char *e = open_out(d->path, &d->file)) {
        return e;
    }
    d->file_on = true;
    uint8_t header[41];
    memset(header, 0, sizeof(header));
    memcpy(header, "C64RESNP", 8);
    header[8] = 1;
    rf_write(&d->file, header, sizeof(header));
    d->g.init(&d->file, true);
    d->g_on = true;
    if (d->g.failed) {
        return "Out of memory";
    }
    // the media: the cartridge's .crt first, then drive A's image
    if (g_src_on && g_src.cart_src && g_src.cart_src[0]) {
        d->cand[d->ncand] = g_src.cart_src;
        d->role[d->ncand++] = "cartridge";
    }
    if (g_src_on && g_src.disk_path && g_src.disk_path[0]) {
        d->cand[d->ncand] = g_src.disk_path;
        d->role[d->ncand++] = "drive8";
    }
    // the steps: this one, each medium hashed and then written a slice a step, the rest
    d->steps = 2;
    for (int i = 0; i < d->ncand; i++) {
        d->steps += 2 * slices(file_bytes(d->cand[i]), DUMP_SLICE);
    }
    d->mi = 0;
    d->phase = MP_PROBE;
    if (!d->ncand) {
        d->g.str("{\"mediaPayloads\":{");
        d->phase = MP_DATA;
    }
    return NULL;
}

// One slice of a medium's hash. A medium that cannot be read whole, or is empty, is left out (its MediaFile is taken by the next one).
static const char *machine_probe(TrxDump *d)
{
    MediaFile *m = &d->mf[d->nm];
    if (!d->in_on) {
        media_name(m, d->cand[d->mi], d->role[d->mi]);
        if (rf_open(&d->in, m->path)) {
            d->in_on = true;
            m->size = uint32_t(rf_size(&d->in));
            d->in_done = 0;
            trx_sha_init(&d->sh);
        } else {
            d->mi++;
        }
    }
    if (d->in_on) {
        bool ok = m->size != 0;
        uint32_t end = m->size - d->in_done > DUMP_SLICE ? d->in_done + DUMP_SLICE : m->size;
        while (ok && d->in_done < end) {
            uint32_t want = end - d->in_done > DUMP_PIECE ? uint32_t(DUMP_PIECE) : end - d->in_done;
            if (!rf_read(&d->in, d->chunk, want)) {
                ok = false;
                break;
            }
            trx_sha_add(&d->sh, d->chunk, want);
            d->in_done += want;
        }
        if (ok && d->in_done < m->size) {
            return NULL;
        }
        trx_sha_final(&d->sh, m->digest);
        rf_close(&d->in);
        d->in_on = false;
        if (ok) {
            d->nm++;
        }
        d->mi++;
    }
    if (d->mi >= d->ncand) {
        d->g.str("{\"mediaPayloads\":{");
        d->mi = 0;
        d->phase = MP_DATA;
    }
    return NULL;
}

// The manifest and the checkpoint after the media; then the digest goes into the head and the file is complete.
static const char *machine_rest(TrxDump *d)
{
    Gz &g = d->g;
    MediaFile *const mf = d->mf;
    const int nm = d->nm;
    g.str("},\"manifest\":{\"kind\":\"c64re-runtime-snapshot\",\"version\":1,\"createdAt\":\"1970-01-01T00:00:00Z\"");
    g.str(",\"machine\":{\"model\":\"");
    g.str(machine_name());
    g.str("\",\"runtimeVersion\":\"trx64u/c64re-dump\"}");
    g.str(",\"checkpoint\":{\"encoding\":\"ta-json-gz/1\",\"payloadRef\":\"checkpoint\",\"cycle\":0,\"pc\":0},\"media\":[");
    for (int i = 0; i < nm; i++) {
        static const char hex[] = "0123456789abcdef";
        char sha[65];
        for (int k = 0; k < 32; k++) {
            sha[2 * k] = hex[mf[i].digest[k] >> 4];
            sha[2 * k + 1] = hex[mf[i].digest[k] & 15];
        }
        sha[64] = 0;
        g.str(i ? ",{\"role\":\"" : "{\"role\":\"");
        g.str(mf[i].role);
        g.str("\",\"format\":\"");
        g.jstr(mf[i].format);
        g.str("\",\"sha256\":\"");
        g.str(sha);
        g.str("\",\"sourceName\":\"");
        g.jstr(mf[i].name);
        g.num("\",\"embeddedPayloadRef\":\"media", i);
        g.str("\"}");
    }
    g.str("]},\"checkpoint\":{\"schemaVersion\":1,\"ram\":");
    g.ta(d->ram, 65536);
    g.num(",\"cpuPortDirection\":", d->ram[0]);
    g.num(",\"cpuPortValue\":", d->ram[1]);
    g.str(",");
    uint32_t w[TRX_BLOCK_WORDS];                              // write_vic reads the registers out of the block's VIC words
    memset(w, 0, sizeof(w));
    for (int r = 0; r < 0x30; r++) {
        w[W_VIC + r / 4] |= uint32_t(d->vic[r]) << (8 * (r & 3));
    }
    write_vic(g, w, d->col, 0, 0);
    g.str("}}");
    uint8_t digest[32];
    if (!g.finish(digest) || !rf_seek(&d->file, 9) || !rf_write(&d->file, digest, 32)) {
        return "The file could not be written";
    }
    d->bytes = 41 + g.written;
    g.done();
    d->g_on = false;
    rf_close(&d->file);
    d->file_on = false;
    d->state = DS_DONE;
    return NULL;
}

// One slice of a medium into mediaPayloads (as Gz::file_ta writes it whole), or, after the last, the rest.
static const char *machine_data(TrxDump *d)
{
    Gz &g = d->g;
    if (d->mi >= d->nm) {
        return machine_rest(d);
    }
    const MediaFile &m = d->mf[d->mi];
    if (!d->in_on) {
        g.num(d->mi ? ",\"media" : "\"media", d->mi);
        g.str("\":");
        if (!rf_open(&d->in, m.path)) {
            return "The file could not be written";
        }
        d->in_on = true;
        d->in_done = 0;
        g.str("{\"$ta\":\"Uint8Array\",\"b64\":\"");
    }
    uint32_t end = m.size - d->in_done > DUMP_SLICE ? d->in_done + DUMP_SLICE : m.size;
    while (d->in_done < end && !g.failed) {
        uint32_t want = end - d->in_done > DUMP_PIECE ? uint32_t(DUMP_PIECE) : end - d->in_done;
        if (!rf_read(&d->in, d->chunk, want)) {
            g.failed = true;
            break;
        }
        g.b64(d->chunk, int(want));
        d->in_done += want;
    }
    if (g.failed) {
        return "The file could not be written";
    }
    if (d->in_done >= m.size) {
        g.str("\"}");
        rf_close(&d->in);
        d->in_on = false;
        d->mi++;
    }
    return NULL;
}

static const char *machine_step(TrxDump *d)
{
    switch (d->phase) {
    case MP_READ:
        return machine_read(d);
    case MP_PROBE:
        return machine_probe(d);
    default:
        return machine_data(d);
    }
}

// ---- .c64rering: the newest seconds of the ring ----------------------------------------------------------------------------------------------------------
// The file: the magic, then one gzip stream of stored blocks: the head, the entries oldest first, the tail. The walk gives the entries newest first
// (it undoes the log back from the head), so each is written at its place in the file as it comes, and only the walk's own copy of the memories is
// held. Its place needs the length of every older entry. That length follows from the entry's frame, the registers read while it is written (the
// turbo node, the cartridge type) and, of its memories, only the colour RAM, which goes in as decimals (write_vic): a walk of the colour records alone
// (trx_ring_mat_col_next) sizes every entry before the first is written, and an entry that comes out at another length fails the dump. The CRC-32 is
// joined from the pieces' own (crc32_combine).

// The head of the stream: the ring's figures and the cursor (the newest entry).
static void ring_head(Gz &g, const TrxDump *d)
{
    const TrxFrame &last = d->fr[d->n - 1];
    char id[32];
    g.num("{\"checkpoint_ring\":{\"budget_bytes\":", int64_t(d->n) * 256 * 1024);
    g.num(",\"max_entries\":", d->n);
    g.num(",\"seq\":", last.seq);
    r_snprintf(id, sizeof(id), "cp_%u_%u", (unsigned)(last.cycles / trx_ring_frame_cycles()), (unsigned)last.seq);
    g.str(",\"current_id\":\"");
    g.str(id);
    g.str("\",\"entries\":[");
}

// Entry k: ram is changed while it is written and put back (write_checkpoint).
static void ring_entry(Gz &g, const TrxDump *d, int k, uint8_t *ram, const uint8_t *col)
{
    const TrxFrame *f = &d->fr[k];
    char id[32];
    // frame: the ring's cycle count in frames of the running mode (seq skips the frames a rewind cut)
    uint32_t frame = uint32_t(f->cycles / trx_ring_frame_cycles());
    r_snprintf(id, sizeof(id), "cp_%u_%u", (unsigned)frame, (unsigned)f->seq);
    g.str(k ? ",{\"id\":\"" : "{\"id\":\"");
    g.str(id);
    g.num("\",\"frame\":", frame);
    g.num(",\"cycles\":", int64_t(f->cycles));
    g.str(",\"pinned\":false,\"byte_size\":96000,\"created_at_ms\":0,\"payload\":");
    // no cartridge RAM per entry: that would be a 64 KB read and 87 KB of base64 for every one of them (T66 part 5)
    write_checkpoint(g, *f, ram, col, NULL, NULL);
    g.str("}");
}

static void ring_tail(Gz &g)
{
    g.str("]},\"delta_ring\":{\"entry_cap\":1,\"writes_cap\":1,\"enabled\":false,\"entries\":\"\",\"writes\":\"\"}");
    g.str(",\"cpu_history\":{\"capacity\":1,\"enabled\":false,\"entries\":\"\"}}");
}

// The stream offset entry k starts at.
static uint32_t ring_at(const TrxDump *d, int k)
{
    uint32_t at = d->head_len;
    for (int j = 0; j < k; j++) {
        at += d->len[j];
    }
    return at;
}

// The entries, oldest first, from the newest whole entry back every `stride` frames; the pause the walk starts from (trx_ring_mat_begin); every
// entry's length from its colour RAM; the file with the head.
static const char *ring_pick(TrxDump *d)
{
    int stride = d->stride < 1 ? 1 : d->stride;
    int n_tl = trx_ring_count();
    int span = (d->seconds * (int)trx_ring_fps_x100() + 50) / 100;
    int *const idx = d->ix;
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
    d->n = n;
    d->fr = (TrxFrame *)r_malloc(n * sizeof(TrxFrame));
    if (!d->fr) {
        return "Out of memory";
    }
    // the entries as they are now (the walk may move the indices; it finds its targets by their seq)
    for (int k = 0; k < n; k++) {
        if (!trx_ring_frame(idx[k], &d->fr[k])) {
            return "No such frame";
        }
    }
    uint16_t t0 = r_ms_timer();
    if (int e = trx_ring_mat_begin(idx, n, false, &d->mat)) {
        return trx_ring_error(e);
    }
    d->mat_ms += uint16_t(r_ms_timer() - t0);
    // the lengths: each entry counted with its colour RAM, in the walk's RAM copy (write_checkpoint puts back what it changes there)
    t0 = r_ms_timer();
    uint8_t *ram, *col_now;
    trx_ring_mat_mem(d->mat, &ram, &col_now);
    Gz c;
    for (int k = n - 1; k >= 0; k--) {
        int which = -1;
        const uint8_t *col;
        if (int e = trx_ring_mat_col_next(d->mat, &which, &col)) {
            return trx_ring_error(e);
        }
        if (which != k) {
            return "The ring changed while it was being saved";
        }
        c.init_count();
        ring_entry(c, d, k, ram, col);
        d->len[k] = c.length();
    }
    c.init_count();
    ring_head(c, d);
    d->head_len = c.length();
    c.init_count();
    ring_tail(c);
    d->tail_len = c.length();
    uint64_t total = uint64_t(d->head_len) + d->tail_len;
    for (int k = 0; k < n; k++) {
        total += d->len[k];
    }
    if (total > 0xFFFFFFFFu) {              // ISIZE and the offsets are 32 bit
        return "The ring is too large for one file";
    }
    d->total = uint32_t(total);
    d->size_ms += uint16_t(r_ms_timer() - t0);
    // the file: the magic, the gzip header, the head
    t0 = r_ms_timer();
    if (const char *e = open_out(d->path, &d->file)) {
        return e;
    }
    d->file_on = true;
    static const uint8_t header[9] = { 'C', '6', '4', 'R', 'E', 'R', 'N', 'G', 1 };
    if (!rf_write(&d->file, header, sizeof(header))) {
        return "The file could not be written";
    }
    Gz &g = d->g;
    g.init_pieces(&d->file, sizeof(header), d->total);
    d->g_on = true;
    if (g.failed) {
        return "Out of memory";
    }
    g.piece(0);
    ring_head(g, d);
    d->head_crc = g.piece_end();
    if (g.failed) {
        return "The file could not be written";
    }
    d->wr_ms += uint16_t(r_ms_timer() - t0);
    d->steps = 1 + n;
    d->k = n - 1;
    d->phase = RP_ENTRY;
    return NULL;
}

// One entry, newest first: materialized, then written at its place in the file. The last (the oldest) writes the tail and the trailer.
static const char *ring_one(TrxDump *d)
{
    uint16_t t0 = r_ms_timer();
    const int k = d->k;
    int which = -1;
    int e = trx_ring_mat_next(d->mat, &which, NULL, NULL, NULL);
    d->mat_ms += uint16_t(r_ms_timer() - t0);
    if (e) {
        return trx_ring_error(e);
    }
    if (which != k) {
        return "The ring changed while it was being saved";
    }
    t0 = r_ms_timer();
    uint8_t *ram, *col;
    trx_ring_mat_mem(d->mat, &ram, &col);
    Gz &g = d->g;
    g.piece(ring_at(d, k));
    ring_entry(g, d, k, ram, col);
    d->crc[k] = g.piece_end();
    if (g.failed) {
        return "The file could not be written";
    }
    if (g.length() != d->len[k]) {             // the colour walk and this one disagree, or a register the entry reads changed since it was counted
        return "The machine changed while it was being saved";
    }
    d->wr_ms += uint16_t(r_ms_timer() - t0);
    if (--d->k >= 0) {
        return NULL;
    }
    trx_ring_mat_end(d->mat);
    d->mat = NULL;
    t0 = r_ms_timer();
    const int n = d->n;
    g.piece(d->total - d->tail_len);
    ring_tail(g);
    uint32_t crc = crc32_combine(d->head_crc, d->crc[0], d->len[0]);
    for (int j = 1; j < n; j++) {
        crc = crc32_combine(crc, d->crc[j], d->len[j]);
    }
    crc = crc32_combine(crc, g.piece_end(), d->tail_len);
    bool ok = g.length() == d->tail_len && g.finish_pieces(crc);
    d->wr_ms += uint16_t(r_ms_timer() - t0);
    r_log("[RING] .c64rering: %d entries, sized in %u ms, materialized in %u ms, written in %u ms (%u in file writes)\n", n, (unsigned)d->size_ms,
          (unsigned)d->mat_ms, (unsigned)d->wr_ms, (unsigned)g.write_ms);
    if (!ok) {
        return "The file could not be written";
    }
    d->bytes = uint32_t(g.pieces_size());
    d->entries = n;
    g.done();
    d->g_on = false;
    rf_close(&d->file);
    d->file_on = false;
    d->state = DS_DONE;
    return NULL;
}

static const char *ring_step(TrxDump *d)
{
    return d->phase == RP_PICK ? ring_pick(d) : ring_one(d);
}

// ---- the calls (trx_c64re.h) -----------------------------------------------------------------------------------------------------------------------------
// Closes what is open; a file that is not complete is removed (no half file is left behind).
static void dump_close(TrxDump *d)
{
    if (d->g_on) {
        d->g.done();
        d->g_on = false;
    }
    if (d->in_on) {
        rf_close(&d->in);
        d->in_on = false;
    }
    if (d->file_on) {
        rf_close(&d->file);
        d->file_on = false;
        rf_remove(d->path);
    }
    trx_ring_mat_end(d->mat);
    d->mat = NULL;
}

TrxDump *trx_dump_begin(int kind, const char *path, int arg, int stride, const char **err)
{
    *err = NULL;
    if (int(strlen(path)) >= int(sizeof(((TrxDump *)0)->path))) {
        *err = "The path is too long";
        return NULL;
    }
    TrxDump *d = (TrxDump *)r_calloc(1, sizeof(TrxDump));
    if (!d) {
        *err = "Out of memory";
        return NULL;
    }
    d->kind = kind;
    d->state = DS_RUN;
    r_snprintf(d->path, sizeof(d->path), "%s", path);
    d->idx = arg;
    d->seconds = arg;
    d->stride = stride;
    ring_machine_build(&d->mach, false);
    return d;
}

int trx_dump_step(TrxDump *d, const char **err)
{
    *err = NULL;
    if (!d || d->state == DS_FAILED) {
        *err = "The dump has failed";
        return -1;
    }
    if (d->state == DS_DONE) {
        return 0;
    }
    // the file describes the machine trx_dump_begin found, whatever source is set around this step
    TrxC64reSrc saved = g_src;
    bool saved_on = g_src_on;
    trx_c64re_set_source(&d->mach.src);
    const char *e = d->kind == TRX_DUMP_FRAME ? frame_step(d) : d->kind == TRX_DUMP_RING ? ring_step(d) : machine_step(d);
    g_src = saved;
    g_src_on = saved_on;
    d->step++;
    if (e) {
        dump_close(d);
        d->bytes = 0;
        d->state = DS_FAILED;
        *err = e;
        return -1;
    }
    return d->state == DS_DONE ? 0 : 1;
}

int trx_dump_pct(const TrxDump *d)
{
    if (!d || !d->steps) {
        return 0;
    }
    if (d->state == DS_DONE) {
        return 100;
    }
    int p = d->step * 100 / d->steps;
    return p > 99 ? 99 : p;
}

const char *trx_dump_path(const TrxDump *d) { return d ? d->path : ""; }
uint32_t trx_dump_bytes(const TrxDump *d) { return d ? d->bytes : 0; }
int trx_dump_entries(const TrxDump *d) { return d ? d->entries : 0; }

void trx_dump_drop(TrxDump *d)
{
    if (!d) {
        return;
    }
    dump_close(d);
    r_free(d->f);
    r_free(d->ram);
    r_free(d->col);
    r_free(d->blk);
    r_free(d->cart);
    r_free(d->chunk);
    r_free(d->hdr);
    r_free(d->drive);
    r_free(d->fr);
    r_free(d);
}

// The whole file in one call: begin, the steps in a loop, drop.
static const char *dump_whole(int kind, const char *path, int arg, int stride, uint32_t *bytes, int *entries)
{
    *bytes = 0;
    const char *err = NULL;
    TrxDump *d = trx_dump_begin(kind, path, arg, stride, &err);
    if (!d) {
        return err;
    }
    int rc;
    while ((rc = trx_dump_step(d, &err)) > 0) {
    }
    if (rc == 0) {
        *bytes = d->bytes;
        if (entries) {
            *entries = d->entries;
        }
    }
    trx_dump_drop(d);
    return rc == 0 ? NULL : err;
}

const char *trx_c64re_dump(const char *path, int idx, uint32_t *bytes)
{
    return dump_whole(TRX_DUMP_FRAME, path, idx, 0, bytes, NULL);
}

const char *trx_c64re_dump_machine(const char *path, uint32_t *bytes)
{
    return dump_whole(TRX_DUMP_MACHINE, path, 0, 0, bytes, NULL);
}

const char *trx_c64rering_dump(const char *path, int seconds, int stride, uint32_t *bytes, int *entries)
{
    return dump_whole(TRX_DUMP_RING, path, seconds, stride, bytes, entries);
}

// ---- load ------------------------------------------------------------------------
// U9 D1: the machine the file says its state needs. Filled while the header is
// still parsed and carried into the staged state, because the token array is
// gone by the time trx_c64re_apply() runs.
struct TrxMachineWant {
    bool     present;               // the file carries a machineState at all
    int      cart_type;             // C64_CARTRIDGE_TYPE at the freeze, -1 for none
    uint16_t cart_require;          // the header's require / prohibit, parsed and carried;
    uint16_t cart_prohibit;         // no load step reads them
    char     cart_src[144];         // the .crt it came from, "" when unknown
    char     drive_img[144];        // what was in drive A, "" for nothing
    uint8_t  drive_mode;            // 0 read/write, 1 read-only, 2 unlinked
    bool     has_settings;
    bool     uci, sampler;
    uint8_t  reu, reu_size;
    int      machine;               // the video standard the file is of, -1 unknown
};

// The state of a file, read and checked, waiting to go into the machine. Its
// buffers are the ones trx_ring_load_state() wants. The cartridge image of a
// format 2 file is not held: it is read from the file again while the machine
// stands (rom_put), so the file's path and the image's offset are kept.
struct TrxC64reStaged {
    uint8_t *ram;                   // 65536, the RAM under the I/O already in place
    uint8_t *col;                   // 1024, the colour RAM's low nibbles
    uint8_t *cart;                  // TRX_CART_RAM_BYTES of cartridge RAM, or NULL (T66 part 5)
    uint8_t *rom;                   // the cartridge's ROM / flash window of a format 1 file (cartFlash), or NULL
    uint32_t rom_len;               // the image's length, held in rom or at rom_off of the file at path
    uint32_t rom_off;
    char     path[192];
    TrxDriveState *drive;           // drive A's own state, or NULL (T66 part 8)
    uint32_t w[TRX_BLOCK_WORDS];    // the chip block
    uint16_t x;                     // the snap position it belongs on
    bool     turbo;                 // a turbo word goes in with it (T29)
    uint32_t turbo_pack;
    uint8_t *eeprom;                // TRX_EEPROM_BYTES of the 93C86 (GMod2), or NULL
    TrxMachineWant want;            // the machine to build before the state goes in
    TrxC64reSrc src;                // the source when the file was staged: the hooks the apply builds the machine with
    bool     src_on;
};

static TrxC64reStaged *pending_state;   // the menu's choice, taken at its close

// Which of the six System Modes a header names, or -1 for a machine this build is not.
static int machine_index(const char *js, const jsmntok_t &t)
{
    for (int i = 0; i < 6; i++) {
        if (tok_eq(js, t, machine_names[i])) {
            return i;
        }
    }
    return -1;
}

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

static trx_progress_fn g_stage_progress;    // the load's bar (U9): set for one trx_c64re_stage_ask()
static void *g_stage_ctx;

// ---- format 1, streamed (MultiCore's decision, U18 §5) ------------------------------------------------------------------------------------------------------
// A .c64re of format 1 or a .c64rering is one gzip member holding one JSON document; a ring of 64 entries unpacks to 6 MB. Neither the file nor the
// document is held: the member is inflated through a 16 KB input buffer (a 32 KB window only for a stream with Huffman blocks, TRX64's), and a scanner
// walks the document as it streams past. Pass 1 reads the whole file: the SHA-256 of a .c64re's body, the member's CRC-32 and length, the entry of a
// ring that is wanted (the first that matches, as ring_pick did over the whole document), and the size of everything pass 2 keeps. Pass 2 reads only
// that checkpoint again (from the stored block it starts in, or from the stream's start when the stream has Huffman blocks before its end) and keeps,
// of its members, the ones a load reads: their text, except three typed arrays (ram, cartFlash, cartState.cartRam), which are decoded straight into
// the buffers the staged state keeps and left as "" in the text. The text kept is a document of its own, {"manifest":{"machine":{"model":..}},
// "checkpoint":{..}}, which jsmn parses as the whole file was parsed before, so everything after it reads the nodes as it did.
#define F1_IN        16384          // the input buffer
#define F1_DEPTH     48             // nesting the scanner follows (deeper is refused as bad JSON)
#define F1_SMALL     96             // a short value kept whole: the model, current_id, an entry's id, label, frame

// The checkpoint members a load reads (stage_entry, trx_c64re_nodes.cc), in a fixed order: their bit in F1Lvl::seen is 1 << (index + 1).
static const char *const f1_keep[] = { "cpu", "ram", "cpuPortDirection", "cpuPortValue", "cia1", "cia2", "vic", "keyboard", "cartState",
                                       "cartFlash", "media", "trx64Drive", "turbo", "trx64Hw" };
enum { F1K_RAM = 1, F1K_CARTSTATE = 8, F1K_CARTFLASH = 9, F1K_N = 14 };

enum F1Role { R_OTHER, R_ROOT, R_MANIFEST, R_MACHINE, R_RING, R_ENTRIES, R_ENTRY, R_CP, R_KEEP, R_TA_RAM, R_TA_FLASH, R_CARTSTATE, R_TA_CRAM };
enum F1Str { FS_NONE, FS_MODEL, FS_CUR, FS_ID, FS_LABEL, FS_FRAME, FS_RAM, FS_FLASH, FS_CRAM };
enum F1State { F_VALUE, F_KEY_OR_END, F_KEY, F_COLON, F_STR, F_PRIM, F_AFTER, F_DONE };

// A typed array's base64 as it streams past: counted (pass 1, cartFlash: tok_ta_len), or decoded into dst (pass 2).
struct F1Side {
    uint8_t *dst;
    uint32_t cap;
    uint32_t got;                   // the bytes it decodes to, all of them (as tok_ta counts), written while below cap
    uint32_t chars, pads;           // its base64 characters and its '=' (tok_ta_len)
    uint32_t acc;
    int nb;
    bool seen;
};

// A short value kept as its raw text (a string's without the quotes); longer ones are marked and never equal anything.
struct F1Small {
    char s[F1_SMALL];
    int len;
    bool str, over, seen;
};

// What pass 1 found of one checkpoint (the .c64re's, or a ring entry's payload) and pass 2 needs.
struct F1Cand {
    bool there, obj;                // a value was there; it is an object (else the document gets null)
    uint32_t start, end;            // stream offsets: its '{', one past its '}'
    uint64_t blk_in;                // the stored block it starts in: its header's offset in the member's deflate data, its first stream byte
    uint32_t blk_out;
    uint32_t text;                  // the bytes pass 2 keeps of it
    uint32_t flash_chars, flash_pads;
    bool flash_seen, cram_there;    // cartFlash's base64 was there; cartState.cartRam is there
};

struct F1Lvl {
    uint8_t role;
    bool obj;
    uint8_t key;                    // the member being read: its code for this role (0: none of the names this role looks at)
    uint32_t seen;                  // the codes seen in this object (json_key takes the first of a name)
    uint32_t count;                 // the members or elements so far
};

struct F1Scan {
    // set by the caller
    bool ring;                      // the document is a .c64rering
    bool pass2;
    bool cursor, by_index;          // which entry is wanted (ring_pick's rules)
    int64_t idx_want;
    const char *want;
    char *text;                     // pass 2: where the kept text goes (text_cap bytes); pass 1: NULL, only counted
    uint32_t text_cap;
    F1Side ram, flash, cram;        // pass 2: the buffers the typed arrays are decoded into
    // the result
    bool bad;                       // not JSON as the scanner follows it
    F1Small model, cur;
    bool ring_obj, entries_arr;     // checkpoint_ring is an object, its entries an array
    bool hit, hit_payload;          // the entry wanted came (and had a payload)
    uint32_t hit_k;                 // its index
    bool undecided;                 // the cursor: an entry ended before current_id came (pass 1 runs again)
    F1Cand cp;                      // the checkpoint (a .c64re) or the entry that matched
    // the scan
    uint32_t pos;                   // the stream offset of the byte being read
    int depth;
    F1Lvl lv[F1_DEPTH];
    int state;
    int str;                        // FS_*: what the string being read is
    bool esc, key_over, copy;
    bool cram_flag;                 // the checkpoint being read has cartState.cartRam
    char key[24];
    int key_len;
    int cp_depth;                   // the depth of the checkpoint object (-1: not in one)
    uint32_t text_len;
    F1Small id, label, frame;       // the entry being read
    bool pay_there;
    F1Cand ent;                     // its payload
    uint32_t entry_k;
    // the inflater's position (pass 1): where the current stored block starts, for F1Cand
    const TrxInflate *z;
    uint32_t base;                  // the stream offset of the chunk being read
};

static void f1_emit(F1Scan *s, const char *p, uint32_t n)
{
    if (s->text) {
        if (s->text_len + n > s->text_cap) {
            s->bad = true;          // pass 2 found more than pass 1 counted: the file changed
            return;
        }
        memcpy(s->text + s->text_len, p, n);
    }
    s->text_len += n;
}

static void small_start(F1Small *v, bool str)
{
    v->len = 0;
    v->s[0] = 0;
    v->str = str;
    v->over = false;
    v->seen = true;
}

static void small_add(F1Small *v, const uint8_t *p, uint32_t n)
{
    if (v->len + int(n) >= F1_SMALL) {
        v->over = true;
        return;
    }
    memcpy(v->s + v->len, p, n);
    v->len += int(n);
    v->s[v->len] = 0;
}

static bool small_is(const F1Small *v, const char *w)
{
    return v->seen && !v->over && v->len == int(strlen(w)) && !memcmp(v->s, w, size_t(v->len));
}

static void side_add(F1Side *d, const uint8_t *p, uint32_t n, bool decode)
{
    uint32_t acc = d->acc, got = d->got, chars = d->chars;
    int nb = d->nb;
    for (uint32_t i = 0; i < n; i++) {
        int v = b64val(char(p[i]));
        if (v < 0) {
            if (p[i] == '=') {
                d->pads++;
            }
            continue;
        }
        chars++;
        if (!decode) {
            continue;
        }
        acc = (acc << 6) | uint32_t(v);
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            if (got < d->cap) {
                d->dst[got] = uint8_t(acc >> nb);
            }
            got++;
        }
    }
    d->acc = acc;
    d->nb = nb;
    d->got = got;
    d->chars = chars;
}

// The code of a key for the role of the object it is in (0: not one this role looks at).
static uint8_t f1_code(int role, const char *k, int len)
{
    static const char *const root[] = { "checkpoint", "checkpoint_ring", "manifest" };
    static const char *const ring[] = { "current_id", "entries" };
    static const char *const entry[] = { "id", "label", "frame", "payload" };
    const char *const *names = NULL;
    int n = 0;
    switch (role) {
    case R_ROOT: names = root; n = 3; break;
    case R_MANIFEST: if (len == 7 && !memcmp(k, "machine", 7)) return 1; return 0;
    case R_MACHINE: if (len == 5 && !memcmp(k, "model", 5)) return 1; return 0;
    case R_RING: names = ring; n = 2; break;
    case R_ENTRY: names = entry; n = 4; break;
    case R_CP: names = f1_keep; n = F1K_N; break;
    case R_TA_RAM: case R_TA_FLASH: case R_TA_CRAM: if (len == 3 && !memcmp(k, "b64", 3)) return 1; return 0;
    case R_CARTSTATE: if (len == 7 && !memcmp(k, "cartRam", 7)) return 1; return 0;
    default: return 0;
    }
    for (int i = 0; i < n; i++) {
        if (int(strlen(names[i])) == len && !memcmp(k, names[i], size_t(len))) {
            return uint8_t(i + 1);
        }
    }
    return 0;
}

// An entry of checkpoint_ring.entries ended (or the array ended before the cursor's): is it the one wanted? ring_pick's rules, in its order.
static void f1_entry_end(F1Scan *s, bool obj)
{
    uint32_t k = s->entry_k;
    bool hit = false;
    if (s->by_index) {
        hit = int64_t(k) == s->idx_want;
    } else if (!obj) {
        hit = false;
    } else if (s->cursor) {
        if (!s->cur.seen) {
            s->undecided = true;    // current_id comes after the entries: pass 1 again, with it
            return;
        }
        hit = s->id.seen && !s->id.over && !s->cur.over && s->id.len == s->cur.len && !memcmp(s->id.s, s->cur.s, size_t(s->id.len));
    } else {
        hit = (s->id.seen && s->id.str && small_is(&s->id, s->want)) || (s->label.seen && s->label.str && small_is(&s->label, s->want)) ||
              (s->frame.seen && !s->frame.over && all_digits(s->want) && r_strtoll(s->frame.s) == r_strtoll(s->want));
    }
    if (hit) {
        s->hit = true;
        s->hit_k = k;
        s->hit_payload = obj && s->pay_there;
        s->cp = s->ent;
    }
}

// A value starts at depth s->depth (its parent is lv[depth - 1], or none for the root): kind '{', '[', '"' or 'p'. Sets what it is.
static void f1_value_start(F1Scan *s, char kind)
{
    int role = R_OTHER;
    s->str = FS_NONE;
    if (s->depth == 0) {
        role = s->pass2 ? R_CP : R_ROOT;
        if (s->pass2 && kind != '{') {
            s->bad = true;
        }
    } else {
        F1Lvl &p = s->lv[s->depth - 1];
        int c = 0;
        if (p.obj) {
            c = p.key;
            if (c && (p.seen & (1u << c))) {
                c = 0;              // a name seen before in this object: json_key never reads it
            } else if (c) {
                p.seen |= 1u << c;
            }
        }
        switch (p.role) {
        case R_ROOT:
            if (c == 1 && !s->ring) {
                s->cp.there = true;
                s->cp.obj = kind == '{';
                role = s->cp.obj ? R_CP : R_OTHER;
            } else if (c == 2 && s->ring) {
                s->ring_obj = kind == '{';
                role = s->ring_obj ? R_RING : R_OTHER;
            } else if (c == 3 && kind == '{') {
                role = R_MANIFEST;
            }
            break;
        case R_MANIFEST:
            role = (c == 1 && kind == '{') ? R_MACHINE : R_OTHER;
            break;
        case R_MACHINE:
            if (c == 1 && (kind == '"' || kind == 'p')) {
                s->str = FS_MODEL;
                small_start(&s->model, kind == '"');
            }
            break;
        case R_RING:
            if (c == 1) {
                small_start(&s->cur, kind == '"');
                if (kind == '"' || kind == 'p') {
                    s->str = FS_CUR;
                } else {
                    s->cur.over = true;     // a container: never equal to an id
                }
            } else if (c == 2) {
                s->entries_arr = kind == '[';
                role = s->entries_arr ? R_ENTRIES : R_OTHER;
            }
            break;
        case R_ENTRIES:
            s->entry_k = p.count - 1;
            s->id.seen = s->label.seen = s->frame.seen = false;
            s->pay_there = false;
            memset(&s->ent, 0, sizeof(s->ent));
            if (kind == '{' && !s->hit && !s->undecided) {
                role = R_ENTRY;
            } else if (!s->hit && !s->undecided) {
                f1_entry_end(s, false);     // an element that is not an object: only "#N" can name it, and it has no payload
            }
            break;
        case R_ENTRY:
            if (c == 1 || c == 2 || c == 3) {
                F1Small *v = c == 1 ? &s->id : c == 2 ? &s->label : &s->frame;
                small_start(v, kind == '"');
                if (kind == '"' || kind == 'p') {
                    s->str = c == 1 ? FS_ID : c == 2 ? FS_LABEL : FS_FRAME;
                } else {
                    v->over = true;
                }
            } else if (c == 4) {
                s->pay_there = true;
                s->ent.there = true;
                s->ent.obj = kind == '{';
                role = s->ent.obj ? R_CP : R_OTHER;
            }
            break;
        case R_CP:
            if (c) {
                const char *k = f1_keep[c - 1];
                if (p.seen & 1u) {          // bit 0: a member is kept already (the codes start at 1)
                    f1_emit(s, ",", 1);
                }
                p.seen |= 1u;
                f1_emit(s, "\"", 1);
                f1_emit(s, k, uint32_t(strlen(k)));
                f1_emit(s, "\":", 2);
                s->copy = true;
                if (kind == '{') {
                    role = c - 1 == F1K_RAM ? R_TA_RAM : c - 1 == F1K_CARTFLASH ? R_TA_FLASH : c - 1 == F1K_CARTSTATE ? R_CARTSTATE : R_KEEP;
                } else {
                    role = R_KEEP;
                }
            }
            break;
        case R_TA_RAM: case R_TA_FLASH: case R_TA_CRAM:
            if (c == 1 && kind == '"') {
                s->str = p.role == R_TA_RAM ? FS_RAM : p.role == R_TA_FLASH ? FS_FLASH : FS_CRAM;
                F1Side *d = p.role == R_TA_RAM ? &s->ram : p.role == R_TA_FLASH ? &s->flash : &s->cram;
                d->seen = true;
                if (p.role == R_TA_FLASH && !s->pass2) {
                    d->chars = d->pads = 0;
                }
            }
            role = R_KEEP;
            break;
        case R_CARTSTATE:
            if (c == 1) {
                s->cram_flag = true;
            }
            role = (c == 1 && kind == '{') ? R_TA_CRAM : R_KEEP;
            break;
        case R_KEEP:
            role = R_KEEP;
            break;
        default:
            break;
        }
    }
    if (kind == '{' || kind == '[') {
        if (s->depth >= F1_DEPTH) {
            s->bad = true;
            return;
        }
        F1Lvl &l = s->lv[s->depth++];
        l.role = uint8_t(role);
        l.obj = kind == '{';
        l.key = 0;
        l.seen = 0;
        l.count = 0;
        if (role == R_CP) {
            s->cp_depth = s->depth - 1;
            F1Cand &c = (s->ring && !s->pass2) ? s->ent : s->cp;
            c.start = s->pos;
            c.blk_in = s->z ? s->z->blk_in : 0;
            c.blk_out = s->z ? s->z->blk_out : 0;
            c.flash_seen = false;
            c.cram_there = false;
            s->cram_flag = false;
            s->flash.chars = s->flash.pads = 0;
            s->flash.seen = false;
            s->text_len = 0;
            f1_emit(s, "{", 1);
        }
    }
}

// The value at depth s->depth ended (a container: it was just popped back to that depth).
static void f1_value_end(F1Scan *s, int role)
{
    if (s->copy && s->depth == s->cp_depth + 1) {
        s->copy = false;            // a member of the checkpoint ends
    }
    if (role == R_CP) {
        F1Cand &c = (s->ring && !s->pass2) ? s->ent : s->cp;
        f1_emit(s, "}", 1);
        c.end = s->pos + 1;
        c.text = s->text_len;
        c.flash_seen = s->flash.seen;
        c.flash_chars = s->flash.chars;
        c.flash_pads = s->flash.pads;
        c.cram_there = s->cram_flag;
        s->cp_depth = -1;
        if (s->pass2) {
            s->state = F_DONE;
        }
    } else if (role == R_ENTRY && !s->hit && !s->undecided) {
        f1_entry_end(s, true);
    }
}

// The end of a string's or a primitive's text.
static void f1_str_end(F1Scan *s)
{
    s->str = FS_NONE;
}

// Bytes p[0..n) of the stream, at stream offset s->pos. Answers how many were taken (all, or up to the end of pass 2's checkpoint).
static uint32_t f1_feed(F1Scan *s, const uint8_t *p, uint32_t n)
{
    uint32_t i = 0;
    while (i < n && !s->bad) {
        uint8_t c = p[i];
        s->pos = s->base + i;
        switch (s->state) {
        case F_VALUE:
        case F_KEY_OR_END:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                i++;
                continue;
            }
            if (s->state == F_KEY_OR_END) {
                if (c == '"') {
                    s->state = F_KEY;
                    s->key_len = 0;
                    s->key_over = false;
                    s->esc = false;
                    if (s->copy) {
                        f1_emit(s, "\"", 1);
                    }
                    i++;
                    continue;
                }
                if (c == '}' && s->lv[s->depth - 1].count == 0) {
                    goto close;
                }
                s->bad = true;
                continue;
            }
            if (c == ']' && s->depth && !s->lv[s->depth - 1].obj && s->lv[s->depth - 1].count == 0) {
                goto close;
            }
            if (s->depth && !s->lv[s->depth - 1].obj) {
                s->lv[s->depth - 1].count++;
            }
            if (c == '{' || c == '[') {
                f1_value_start(s, char(c));
                if (s->copy) {
                    f1_emit(s, (const char *)&c, 1);
                }
                s->state = c == '{' ? F_KEY_OR_END : F_VALUE;
                i++;
                continue;
            }
            if (c == '"') {
                f1_value_start(s, '"');
                if (s->copy) {
                    f1_emit(s, "\"", 1);
                }
                s->esc = false;
                s->state = F_STR;
                i++;
                continue;
            }
            if (c == '}' || c == ']' || c == ',' || c == ':') {
                s->bad = true;
                continue;
            }
            f1_value_start(s, 'p');
            s->state = F_PRIM;
            continue;               // the byte is the primitive's first
        case F_KEY:
            if (s->esc) {
                s->esc = false;
            } else if (c == '\\') {
                s->esc = true;
            } else if (c == '"') {
                if (s->copy) {
                    f1_emit(s, "\"", 1);
                }
                F1Lvl &l = s->lv[s->depth - 1];
                l.key = s->key_over ? 0 : f1_code(l.role, s->key, s->key_len);
                l.count++;
                s->state = F_COLON;
                i++;
                continue;
            }
            if (s->key_len < int(sizeof(s->key))) {
                s->key[s->key_len++] = char(c);
            } else {
                s->key_over = true;
            }
            if (s->copy) {
                f1_emit(s, (const char *)&c, 1);
            }
            i++;
            continue;
        case F_COLON:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                i++;
                continue;
            }
            if (c != ':') {
                s->bad = true;
                continue;
            }
            if (s->copy) {
                f1_emit(s, ":", 1);
            }
            s->state = F_VALUE;
            i++;
            continue;
        case F_STR: {
            // the run up to the next quote or backslash in one piece
            uint32_t j = i;
            if (!s->esc) {
                while (j < n && p[j] != '"' && p[j] != '\\') {
                    j++;
                }
            } else {
                j = i + 1;          // the escaped byte
                s->esc = false;
            }
            if (j > i) {
                const uint8_t *q = p + i;
                uint32_t m = j - i;
                switch (s->str) {
                case FS_MODEL: small_add(&s->model, q, m); break;
                case FS_CUR: small_add(&s->cur, q, m); break;
                case FS_ID: small_add(&s->id, q, m); break;
                case FS_LABEL: small_add(&s->label, q, m); break;
                case FS_FRAME: small_add(&s->frame, q, m); break;
                case FS_RAM: if (s->pass2) side_add(&s->ram, q, m, true); break;
                case FS_CRAM: if (s->pass2) side_add(&s->cram, q, m, true); break;
                case FS_FLASH: side_add(&s->flash, q, m, s->pass2); break;
                default: break;
                }
                if (s->copy && s->str != FS_RAM && s->str != FS_CRAM && s->str != FS_FLASH) {
                    f1_emit(s, (const char *)q, m);
                }
                i = j;
                continue;
            }
            if (c == '\\') {
                s->esc = true;
                switch (s->str) {
                case FS_MODEL: small_add(&s->model, p + i, 1); break;
                case FS_CUR: small_add(&s->cur, p + i, 1); break;
                case FS_ID: small_add(&s->id, p + i, 1); break;
                case FS_LABEL: small_add(&s->label, p + i, 1); break;
                case FS_FRAME: small_add(&s->frame, p + i, 1); break;
                case FS_RAM: if (s->pass2) side_add(&s->ram, p + i, 1, true); break;
                case FS_CRAM: if (s->pass2) side_add(&s->cram, p + i, 1, true); break;
                case FS_FLASH: side_add(&s->flash, p + i, 1, s->pass2); break;
                default: break;
                }
                if (s->copy && s->str != FS_RAM && s->str != FS_CRAM && s->str != FS_FLASH) {
                    f1_emit(s, "\\", 1);
                }
                i++;
                continue;
            }
            // the closing quote
            if (s->copy) {
                f1_emit(s, "\"", 1);
            }
            f1_str_end(s);
            f1_value_end(s, R_OTHER);
            s->state = F_AFTER;
            i++;
            continue;
        }
        case F_PRIM:
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                f1_str_end(s);
                f1_value_end(s, R_OTHER);
                s->state = F_AFTER;
                continue;           // the delimiter is the container's
            }
            switch (s->str) {
            case FS_MODEL: small_add(&s->model, p + i, 1); break;
            case FS_CUR: small_add(&s->cur, p + i, 1); break;
            case FS_ID: small_add(&s->id, p + i, 1); break;
            case FS_LABEL: small_add(&s->label, p + i, 1); break;
            case FS_FRAME: small_add(&s->frame, p + i, 1); break;
            default: break;
            }
            if (s->copy) {
                f1_emit(s, (const char *)p + i, 1);
            }
            i++;
            continue;
        case F_AFTER:
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                i++;
                continue;
            }
            if (!s->depth) {
                s->state = F_DONE;  // the root ended; what follows is not read
                continue;
            }
            if (c == ',') {
                if (s->copy) {
                    f1_emit(s, ",", 1);
                }
                s->state = s->lv[s->depth - 1].obj ? F_KEY_OR_END : F_VALUE;
                i++;
                continue;
            }
            if (c == '}' || c == ']') {
                goto close;
            }
            s->bad = true;
            continue;
        default:                    // F_DONE
            return i;
        }
    close:
        {
            F1Lvl &l = s->lv[s->depth - 1];
            if ((c == '}') != l.obj) {
                s->bad = true;
                continue;
            }
            if (s->copy) {
                f1_emit(s, (const char *)&c, 1);
            }
            int role = l.role;
            s->depth--;
            f1_value_end(s, role);
            if (s->state != F_DONE) {
                s->state = F_AFTER;
            }
            i++;
            if (s->state == F_DONE) {
                return i;
            }
        }
    }
    return i;
}

// The member's deflate data from the file (the trailer's 8 bytes are not in it); pass 1 hashes a .c64re's body on the way and shows the progress.
struct F1Src {
    RFile *f;
    uint64_t at, end;
    TrxSha256 *sha;                 // NULL: no hash
    uint32_t from, size, next;      // the progress: the offset the data starts at, the file's size, the next offset to report at
    uint16_t last;                  // the time: r_ms_timer at the last read, the milliseconds summed (each read is short, so the 16-bit timer does)
    uint32_t ms;
};

static void f1_tick(F1Src *s)
{
    uint16_t now = r_ms_timer();
    s->ms += uint16_t(now - s->last);
    s->last = now;
}

static int f1_src(void *ctx, uint8_t *buf, uint32_t cap)
{
    F1Src *s = (F1Src *)ctx;
    if (s->at >= s->end) {
        return 0;
    }
    uint32_t n = s->end - s->at > cap ? cap : uint32_t(s->end - s->at);
    f1_tick(s);
    if (!rf_seek(s->f, s->at) || !rf_read(s->f, buf, n)) {
        return -1;
    }
    if (s->sha) {
        trx_sha_add(s->sha, buf, n);
    }
    s->at += n;
    if (s->next && s->at >= s->next && g_stage_progress) {
        g_stage_progress(int((s->at - s->from) >> 10), int((s->size - s->from) >> 10), g_stage_ctx);
        s->next += (s->size - s->from) / 32 + 1;     // about 32 steps whatever the size: each one may redraw the menu
    }
    return int(n);
}

struct F1Sink {
    F1Scan *sc;
    uint32_t out;                   // the stream offset of the next byte
    uint32_t isize;                 // pass 1: what the trailer says the stream is
    bool full;                      // pass 1: the stream went past it
    bool pass2;
    uint32_t start;                 // pass 2: the checkpoint's first byte
};

static int f1_sink(void *ctx, const uint8_t *p, uint32_t n)
{
    F1Sink *k = (F1Sink *)ctx;
    uint32_t at = k->out;
    k->out += n;
    F1Scan *sc = k->sc;
    if (!k->pass2) {
        if (k->out > k->isize || k->out < at) {
            k->full = true;
            return -1;
        }
        if (sc->state != F_DONE && !sc->bad) {
            sc->base = at;
            f1_feed(sc, p, n);
        }
        return 0;
    }
    if (at + n <= k->start) {
        return 0;
    }
    uint32_t skip = at < k->start ? k->start - at : 0;
    sc->base = at + skip;
    f1_feed(sc, p + skip, n - skip);
    return (sc->state == F_DONE || sc->bad) ? 1 : 0;
}

static inline uint32_t f1_le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// What the format 1 read gives the stage: the document kept (js, jl), and the typed arrays decoded into the buffers the staged state keeps.
struct F1Out {
    char *js;
    uint32_t jl;
    uint8_t *ram, *col, *cart, *rom;
    uint32_t ram_got;               // ram's bytes (65536 when it is whole), 0 when the checkpoint had no typed ram
    int cart_got;                   // cartState.cartRam's bytes as trx_c64re_cart_ram counts them, -1: not a typed array, read it from js
    uint32_t rom_cap, rom_got;      // cartFlash: the bytes wanted (tok_ta_len, capped at the source's image), the bytes decoded
};

static void f1_free(F1Out *o)
{
    r_free(o->js);
    r_free(o->ram);
    r_free(o->col);
    r_free(o->cart);
    r_free(o->rom);
    memset(o, 0, sizeof(*o));
}

static void f1_scan_init(F1Scan *sc, bool ring, const char *want, const TrxInflate *z)
{
    memset(sc, 0, sizeof(*sc));
    sc->ring = ring;
    sc->cursor = !want || !*want;
    sc->by_index = !sc->cursor && want[0] == '#';
    sc->idx_want = sc->by_index ? r_strtoll(want + 1) : -1;
    sc->want = want;
    sc->cp_depth = -1;
    sc->state = F_VALUE;
    sc->z = z;
}

// Reads a .c64re of format 1 (head 41 bytes, the SHA-256 of the body at 9) or a .c64rering (head 9 bytes) in two streamed passes (see above).
static const char *f1_read(RFile *file, uint32_t size, bool ring, const uint8_t *digest, const char *want, F1Out *o)
{
    memset(o, 0, sizeof(*o));
    const uint32_t hdr = ring ? 9 : 41;
    uint8_t tail[8], magic[3];
    if (!rf_seek(file, size - 8) || !rf_read(file, tail, 8) || !rf_seek(file, hdr) || !rf_read(file, magic, 3)) {
        return "The file could not be read";
    }
    const uint32_t crc_want = f1_le32(tail), isize = f1_le32(tail + 4);
    uint8_t *in = (uint8_t *)r_malloc(F1_IN);
    F1Scan *sc = (F1Scan *)r_malloc(sizeof(F1Scan));
    uint8_t *win = NULL;
    const char *err = (in && sc) ? NULL : "Out of memory";
    TrxSha256 sha;
    TrxInflate z;
    F1Src src;
    F1Sink sk;
    F1Small cur_kept;
    bool cur_preset = false;
    uint32_t ms1 = 0, ms2 = 0;      // the passes' times
    bool gz_ok = magic[0] == 0x1F && magic[1] == 0x8B && magic[2] == 8 && isize;
    // pass 1: the whole file; again with a window when a Huffman block came, again with current_id when it came after the entries
    while (!err) {
        f1_scan_init(sc, ring, want, &z);
        if (cur_preset) {
            sc->cur = cur_kept;
        }
        memset(&src, 0, sizeof(src));
        src.f = file;
        src.at = hdr;
        src.end = size - 8;
        src.sha = ring ? NULL : &sha;
        src.from = hdr;
        src.size = size;
        src.next = hdr + 1;
        src.last = r_ms_timer();
        src.ms = ms1;
        if (!ring) {
            trx_sha_init(&sha);
        }
        memset(&sk, 0, sizeof(sk));
        sk.sc = sc;
        sk.isize = isize;
        memset(&z, 0, sizeof(z));
        z.src = f1_src;
        z.src_ctx = &src;
        z.in = in;
        z.in_cap = F1_IN;
        z.sink = f1_sink;
        z.sink_ctx = &sk;
        z.win = win;
        trx_inf_init(&z);
        int e = gz_ok ? trx_inf_gzip_header(&z) : TRX_INF_OK;
        if (gz_ok && !e) {
            e = trx_inf_run(&z);
        }
        f1_tick(&src);
        ms1 = src.ms;
        if (e == TRX_INF_NEEDWIN && !win) {
            win = (uint8_t *)r_malloc(TRX_INF_WSIZE);
            if (!win) {
                err = "Out of memory";
            }
            continue;
        }
        // a .c64re: the rest of the body into the hash (what the inflate did not read, the trailer), then the hash before anything else
        if (!ring) {
            while (src.at < src.end) {
                int got = f1_src(&src, in, F1_IN);
                if (got <= 0) {
                    break;
                }
            }
            trx_sha_add(&sha, tail, 8);
            uint8_t d[32];
            trx_sha_final(&sha, d);
            if (memcmp(d, digest, 32)) {
                err = "Hash mismatch";
                break;
            }
        }
        if (!gz_ok) {
            err = "Not a gzip body";
        } else if (sk.full) {
            err = trx_inflate_error(TRX_INF_FULL);
        } else if (e) {
            err = trx_inflate_error(e);
        } else if (z.out != isize || (z.crc != crc_want)) {
            err = trx_inflate_error(TRX_INF_BADCRC);
        } else if (sc->bad || (sc->state != F_DONE && !(sc->depth == 0 && (sc->state == F_AFTER || sc->state == F_PRIM)))) {
            err = "Bad JSON";
        } else if (ring && sc->undecided && sc->cur.seen && !cur_preset) {
            cur_kept = sc->cur;
            cur_preset = true;
            continue;
        } else if (ring) {
            if (!sc->ring_obj || !sc->entries_arr || (sc->cursor && !sc->cur.seen)) {
                err = "No checkpoint_ring entries";
            } else if (!sc->hit) {
                err = sc->cursor ? "The current entry is not in the file" : "No such entry in the file";
            } else if (!sc->hit_payload) {
                err = "That entry carries no payload";
            }
        }
        break;
    }
    const F1Cand cp = sc ? sc->cp : F1Cand();
    const uint32_t first_huff = z.first_huff;
    const F1Small model = sc ? sc->model : F1Small();
    const uint32_t hit_k = sc ? sc->hit_k : 0;
    // the buffers the staged state keeps, and the document
    uint32_t fl = 0;
    if (!err && cp.obj && cp.flash_seen) {
        uint32_t whole = (cp.flash_chars / 4) * 3;       // tok_ta_len
        switch (cp.flash_chars % 4) {
        case 2: fl = whole + 1; break;
        case 3: fl = whole + 2; break;
        default: fl = whole - (cp.flash_pads > 2 ? 2 : cp.flash_pads); break;
        }
        if (g_src_on && g_src.rom_bytes && fl > g_src.rom_bytes) {
            fl = g_src.rom_bytes;
        }
    }
    char pre[F1_SMALL + 64];
    int pl = 0;
    if (!err) {
        pl = r_snprintf(pre, sizeof(pre), "{");
        if (model.seen) {
            pl += r_snprintf(pre + pl, int(sizeof(pre)) - pl, "\"manifest\":{\"machine\":{\"model\":%s%.*s%s%s}}%s", model.str ? "\"" : "",
                             model.len, model.s, model.over ? "~" : "", model.str ? "\"" : "", cp.there ? "," : "");
        }
        if (cp.there) {
            pl += r_snprintf(pre + pl, int(sizeof(pre)) - pl, "\"checkpoint\":%s", cp.obj ? "" : "null");
        }
        o->ram = (uint8_t *)r_malloc(65536);
        o->col = (uint8_t *)r_malloc(1024);
        o->cart = cp.obj && cp.cram_there ? (uint8_t *)r_malloc(TRX_CART_RAM_BYTES) : NULL;
        o->rom = fl ? (uint8_t *)r_malloc(fl) : NULL;
        o->rom_cap = fl;
        o->jl = uint32_t(pl) + (cp.obj ? cp.text : 0) + 1;
        o->js = (char *)r_malloc(o->jl + 1);
        if (!o->ram || !o->col || !o->js) {
            err = "Out of memory";
        } else {
            memcpy(o->js, pre, size_t(pl));
            o->js[o->jl - 1] = '}';
            o->js[o->jl] = 0;
        }
    }
    o->cart_got = -1;
    // pass 2: the checkpoint again, from the stored block it starts in (or from the start when Huffman blocks come before its end)
    if (!err && cp.obj) {
        f1_scan_init(sc, ring, want, NULL);
        sc->pass2 = true;
        sc->text = o->js + pl;
        sc->text_cap = cp.text;
        sc->ram.dst = o->ram;
        sc->ram.cap = 65536;
        sc->cram.dst = o->cart;
        sc->cram.cap = o->cart ? TRX_CART_RAM_BYTES : 0;
        sc->flash.dst = o->rom;
        sc->flash.cap = o->rom ? fl : 0;
        bool seek = first_huff >= cp.end;
        memset(&src, 0, sizeof(src));
        src.f = file;
        src.at = seek ? hdr + cp.blk_in : hdr;
        src.end = size - 8;
        src.last = r_ms_timer();
        memset(&sk, 0, sizeof(sk));
        sk.sc = sc;
        sk.pass2 = true;
        sk.start = cp.start;
        sk.out = seek ? cp.blk_out : 0;
        memset(&z, 0, sizeof(z));
        z.src = f1_src;
        z.src_ctx = &src;
        z.in = in;
        z.in_cap = F1_IN;
        z.sink = f1_sink;
        z.sink_ctx = &sk;
        z.win = seek ? NULL : win;
        z.out = sk.out;
        trx_inf_init(&z);
        int e = seek ? TRX_INF_OK : trx_inf_gzip_header(&z);
        if (!e) {
            e = trx_inf_run(&z);
        }
        f1_tick(&src);
        ms2 = src.ms;
        if (e < 0 && e != TRX_INF_SINK) {
            err = trx_inflate_error(e);
        } else if (sc->bad || sc->state != F_DONE || sc->text_len != cp.text) {
            err = "The file changed while it was read";
        } else {
            o->ram_got = sc->ram.seen ? sc->ram.got : 0;
            if (sc->cram.seen) {
                o->cart_got = int(sc->cram.got < TRX_CART_RAM_BYTES ? sc->cram.got : TRX_CART_RAM_BYTES);
            }
            o->rom_got = sc->flash.got < fl ? sc->flash.got : fl;
        }
    }
    if (!err) {
        char what[24];
        what[0] = 0;
        if (ring) {
            r_snprintf(what, sizeof(what), ", entry #%u", (unsigned)hit_k);
        }
        r_log("[C64RE] %s%s: %u bytes from %u, pass 1 %u ms%s, pass 2 %u ms (%u bytes of the checkpoint kept)\n", ring ? ".c64rering" : ".c64re format 1",
              what, (unsigned)isize, (unsigned)size, (unsigned)ms1, win ? " (Huffman blocks: with the window)" : "", (unsigned)ms2, (unsigned)cp.text);
    }
    r_free(in);
    r_free(win);
    r_free(sc);
    if (err) {
        f1_free(o);
    }
    return err;
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
    char name[41];
    r_copy(name, sizeof(name), js + t[m].start, size_t(t[m].end - t[m].start));
    r_snprintf(msg, sizeof(msg), "The file is for machine \"%s\", not c64-pal", name);
    return msg;
}

// What the coarse load took and what it could not take, in the log: the
// user is told the load is not exact, and by how much.
static void say_coarse(const TrxNodeState *ns)
{
    r_log("[C64RE] coarse load (no trx64Hw node):%s%s%s%s%s%s%s%s%s\n", (ns->have & TRX_NODE_CPU) ? " cpu" : "",
          (ns->have & TRX_NODE_PORT) ? " port" : "", (ns->have & TRX_NODE_CIA1) ? " cia1" : "",
          (ns->have & TRX_NODE_CIA2) ? " cia2" : "", (ns->have & TRX_NODE_VIC) ? " vic" : "",
          (ns->have & TRX_NODE_COLOR) ? " colour" : "", (ns->have & TRX_NODE_RAM) ? " ram" : "",
          (ns->have & TRX_NODE_CART) ? " cart-banking" : "",
          (ns->have & TRX_NODE_CARTRAM) ? " cart-ram" : "");
    r_log("[C64RE] not restored: the VIC's raster position (file line %d cycle %d) and pixel pipeline, "
          "the SID, the drive\n", ns->line, ns->cycle);
    if (!(ns->have & TRX_NODE_CPU)) {
        r_log("[C64RE] the file has no cpu node: the 6510's registers are the running machine's (its newest frame)\n");
    }
    if (!(ns->have & (TRX_NODE_CIA1 | TRX_NODE_CIA2))) {
        r_log("[C64RE] the file has no cia nodes: both CIAs are the running machine's\n");
    }
    if (ns->keys_pressed) {
        r_log("[C64RE] %d key(s) were held down in the file; they are not pressed here\n", ns->keys_pressed);
    }
}

// The file's "media" node against what the source has on drive A (T66 part 5).
// Nothing is mounted or unmounted here: a load puts the machine's state back,
// and which disk is in the drive is the user's. A mismatch is said once,
// because the program in the snapshot will read sectors that are no longer
// under the head.
static void say_media(const char *js, const jsmntok_t *t, int n, int cp)
{
    int md = json_key(js, t, n, cp, "media");
    int dp = json_key(js, t, n, md, "diskPath");
    if (dp < 0) {
        return;
    }
    int len = t[dp].end - t[dp].start;
    const char *want = js + t[dp].start;
    const char *name = (g_src_on && g_src.disk_path) ? strrchr(g_src.disk_path, '/') : NULL;
    name = name ? name + 1 : (g_src_on && g_src.disk_path ? g_src.disk_path : "");
    int nl = int(strlen(name));
    bool same = false;
    if (nl && len > nl) {
        same = !strncmp(want + len - nl, name, nl);
    }
    if (!same) {
        char w[96];
        r_copy(w, sizeof(w), want, size_t(len));
        r_log("[C64RE] the snapshot was taken with \"%s\" in drive A; now it is \"%s\"\n", w, nl ? name : "nothing");
    }
}

// ---- format 2: the reader (U8 §4) -----------------------------------------------
// Streamed: the section table says where every payload is and how long, so each one
// is read once, straight into the buffer the hardware is handed. The peak is the
// sum of those buffers - never the file, and never a second representation; the
// cartridge image is hashed on the way past and read again at the load.

static inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

// Reads and hashes in step, so the digest covers exactly the bytes of the body.
class Rd {
    RFile *f;
public:
    TrxSha256 sha;
    bool failed;
    uint32_t pos;                   // the file position of the next byte
    void init(RFile *file, uint32_t at)     // no constructor (R3)
    {
        f = file;
        failed = false;
        pos = at;
        trx_sha_init(&sha);
    }
    bool get(void *p, uint32_t len)
    {
        if (failed || !len) {
            return !failed;
        }
        if (!rf_read(f, p, len)) {
            failed = true;
            return false;
        }
        trx_sha_add(&sha, (const uint8_t *)p, len);
        pos += len;
        return true;
    }
    // an id this reader does not keep, or the padding: read it so the digest is
    // right, then throw it away
    bool eat(uint32_t len, uint8_t *scratch, uint32_t cap)
    {
        while (len && !failed) {
            uint32_t want = len > cap ? cap : len;
            if (!get(scratch, want)) {
                return false;
            }
            len -= want;
        }
        return !failed;
    }
};

#define HDR_JSON_LIMIT 4096      // a header is a few hundred bytes; this is the refusal
#define SEC_LIMIT        64      // sections one file may declare

// A string token into a fixed buffer. An absent key or one that does not fit
// leaves the buffer empty, which every caller reads as "the file does not say".
static void tok_copy(const char *js, const jsmntok_t *t, int i, char *dst, int cap)
{
    dst[0] = 0;
    if (i < 0 || t[i].type != JSMN_STRING) {
        return;
    }
    int n = t[i].end - t[i].start;
    if (n <= 0 || n >= cap) {
        return;
    }
    memcpy(dst, js + t[i].start, n);
    dst[n] = 0;
}

static TrxC64reMissing *g_stage_miss;       // U9 D3d: filled when the medium is why it failed
static const char *g_stage_medium;          // the answer to the last refusal, tried first

static const char *base_of(const char *path)
{
    const char *cut = strrchr(path, '/');
    return cut ? cut + 1 : path;
}

static bool is_there(const char *path)
{
    return path && path[0] && rf_exists(path);
}

// dir + name, with exactly one '/' between them. False when it does not fit,
// which is a path that could not be opened anyway.
static bool join(char *dst, int cap, const char *dir, int dirlen, const char *name)
{
    while ((dirlen > 0) && (dir[dirlen - 1] == '/')) {
        dirlen--;
    }
    int need = dirlen + 1 + int(strlen(name));
    if (need >= cap) {
        return false;
    }
    memcpy(dst, dir, dirlen);
    dst[dirlen] = '/';
    strcpy(dst + dirlen + 1, name);
    return true;
}

// U9 D3c (Alex, 2026-11-08): a file written on another machine names its medium
// by that machine's path - TRX64 writes "/Users/...", which no board has. Three
// places are looked at, in this order: what the user last answered a refusal
// with (D3d), the path in the file, and the same file name beside the .c64re -
// that last one is how the two reach the board, in one folder. Nothing wider is
// searched on its own: that would insert an image of that name rather than the
// image. The path that holds replaces the one from the file, because
// build_cartridge() and build_drive() mount by that string later, after the
// header's tokens are gone. Only the cartridge is asked about, so only it reads
// the answer and only it records what was missing.
static bool resolve_medium(char *want, int cap, const char *self, bool cartridge)
{
    const char *base = base_of(want);
    char tmp[192];

    if (cartridge && g_stage_medium && g_stage_medium[0]) {
        const char *chosen = g_stage_medium;
        int clen = int(strlen(chosen));
        bool folder = (clen > 0) && (chosen[clen - 1] == '/');
        if (!folder && is_there(chosen) && (clen < cap)) {
            r_log("[C64RE] '%s' is used, as chosen\n", chosen);
            strcpy(want, chosen);
            return true;
        }
        // A folder was chosen, or a file that is gone again: the name from the
        // state in the folder that was chosen.
        if (base[0] && join(tmp, int(sizeof(tmp)), chosen, clen, base) &&
            is_there(tmp) && (int(strlen(tmp)) < cap)) {
            r_log("[C64RE] '%s' is used, from the chosen folder\n", tmp);
            strcpy(want, tmp);
            return true;
        }
    }
    if (want[0] && is_there(want)) {
        return true;
    }
    const char *cut = self ? strrchr(self, '/') : NULL;
    if (base[0] && cut && join(tmp, int(sizeof(tmp)), self, int(cut - self) + 1, base) &&
        is_there(tmp) && (int(strlen(tmp)) < cap)) {
        r_log("[C64RE] '%s' is not there; '%s' is used instead\n", want, tmp);
        strcpy(want, tmp);
        return true;
    }
    if (cartridge && g_stage_miss) {
        r_snprintf(g_stage_miss->want, sizeof(g_stage_miss->want), "%s", want);
        r_snprintf(g_stage_miss->base, sizeof(g_stage_miss->base), "%s", base);
    }
    return false;
}

static const char *stage_v2(RFile *file, const uint8_t *head, uint32_t size, const char *self,
                            TrxC64reStaged **out)
{
    Rd rd;
    rd.init(file, 41);
    uint8_t buf[8];
    const char *err = NULL;
    char *hdr = NULL;
    uint8_t *tab = NULL;
    uint8_t *blk = NULL;
    uint8_t *ram = NULL, *col = NULL, *cart = NULL, *drv = NULL, *eeprom = NULL;
    uint8_t *scratch = (uint8_t *)r_malloc(RAW_CHUNK);  // what is read past (the cartridge image among it) goes through here
    uint32_t rom_len = 0, rom_off = 0;
    bool have_rom = false;
    uint32_t hl = 0;
    int ns = 0;
    TrxMachineWant mw;              // `want` below is the section loop's own length
    memset(&mw, 0, sizeof(mw));
    mw.cart_type = -1;
    mw.machine = -1;

    if (!scratch) {
        err = "Out of memory";
    } else if (!rd.get(buf, 4)) {
        err = "The file could not be read";
    } else if ((hl = le32(buf)) < 2 || hl > HDR_JSON_LIMIT) {
        err = "Not a .c64re (format 2): the header does not fit";
    } else if (!(hdr = (char *)r_malloc(hl + 1))) {
        err = "Out of memory";
    } else if (!rd.get(hdr, hl) || !rd.eat((4 - (hl & 3)) & 3, scratch, RAW_CHUNK)) {
        err = "The file could not be read";
    }
    if (!err) {
        hdr[hl] = 0;
        // what the header has to agree on: the block layout, because the words go
        // onto the state bus by position, and the machine
        jsmn_parser p;
        jsmn_init(&p);
        int need = jsmn_parse(&p, hdr, hl, NULL, 0);
        jsmntok_t *t = (need > 0) ? (jsmntok_t *)r_malloc(sizeof(jsmntok_t) * need) : NULL;
        if (!t) {
            err = need > 0 ? "Out of memory" : "Bad header";
        } else {
            jsmn_init(&p);
            int n = jsmn_parse(&p, hdr, hl, t, need);
            int bw = (n > 0) ? json_key(hdr, t, n, 0, "blockWords") : -1;
            int mk = (n > 0) ? json_key(hdr, t, n, 0, "machine") : -1;
            if (n < 1) {
                err = "Bad header";
            } else if (bw >= 0 && tok_int(hdr, t, bw) != TRX_BLOCK_WORDS) {
                err = "The file's chip block is not this machine's";
            } else if (mk >= 0 && (mw.machine = machine_index(hdr, t[mk])) < 0) {
                static char msg[96];
                char name[41];
                r_copy(name, sizeof(name), hdr + t[mk].start, size_t(t[mk].end - t[mk].start));
                r_snprintf(msg, sizeof(msg), "The file is for machine \"%s\", which this build does not run", name);
                err = msg;
            }
            // U9 D1: the machine this state needs. A file written before U9 has
            // no machineState; then the flat cartridgeType is all there is, and
            // the machine has to be the one it was taken on (D5, the check
            // format 1 had and format 2 lost).
            int ms = (!err && n > 0) ? json_key(hdr, t, n, 0, "machineState") : -1;
            if (!err && ms >= 0 && t[ms].type == JSMN_OBJECT) {
                mw.present = true;
                int ct = json_key(hdr, t, n, ms, "cartridge");
                if (ct >= 0) {
                    mw.cart_type = int(tok_int_or(hdr, t, json_key(hdr, t, n, ct, "type"), -1));
                    mw.cart_require  = uint16_t(tok_int_or(hdr, t, json_key(hdr, t, n, ct, "require"), 0));
                    mw.cart_prohibit = uint16_t(tok_int_or(hdr, t, json_key(hdr, t, n, ct, "prohibit"), 0));
                    tok_copy(hdr, t, json_key(hdr, t, n, ct, "source"), mw.cart_src, sizeof(mw.cart_src));
                }
                int dr = json_key(hdr, t, n, ms, "drive");
                if (dr >= 0) {
                    tok_copy(hdr, t, json_key(hdr, t, n, dr, "image"), mw.drive_img, sizeof(mw.drive_img));
                    int md = json_key(hdr, t, n, dr, "mode");
                    mw.drive_mode = (md >= 0 && tok_eq(hdr, t[md], "unlinked")) ? 2 :
                                    (md >= 0 && tok_eq(hdr, t[md], "ro")) ? 1 : 0;
                }
                int se = json_key(hdr, t, n, ms, "settings");
                if (se >= 0) {
                    mw.has_settings = true;
                    mw.uci     = tok_true(hdr, t, json_key(hdr, t, n, se, "uci"));
                    mw.sampler = tok_true(hdr, t, json_key(hdr, t, n, se, "sampler"));
                    mw.reu      = uint8_t(tok_int_or(hdr, t, json_key(hdr, t, n, se, "reu"), 0));
                    mw.reu_size = uint8_t(tok_int_or(hdr, t, json_key(hdr, t, n, se, "reuSize"), 0));
                }
            } else if (!err && n > 0) {
                mw.cart_type = int(tok_int_or(hdr, t, json_key(hdr, t, n, 0, "cartridgeType"), -1));
            }
            r_free(t);
        }
    }
    // U9 D5: the cartridge, before a byte is put anywhere. Two cases, and they
    // are different questions. With a machineState the load builds the machine,
    // so what matters is whether this build can build it at all. Without one -
    // a file from before U9 - nothing will be inserted, so the machine has to
    // already be the one the file was taken on, which is format 1's check.
    if (!err && mw.present && mw.cart_type > 0 &&
        !trx_ring_cart_state_ok(uint8_t(mw.cart_type), NULL)) {
        static char msg[80];
        r_snprintf(msg, sizeof(msg), "This build cannot put back a cartridge of type %02x", mw.cart_type);
        err = msg;
    }
    // The cartridge is inserted from its own .crt, which supplies the geometry
    // the cartridge's definition is made of. That cannot be rebuilt from the
    // type and a byte count, so without the file this build would insert a
    // cartridge it could not describe, and the next save would write a cartridge
    // machine with no medium. D0 says refuse, and refuse before anything moves.
    if (!err && mw.present && mw.cart_type > 0) {
        if (!resolve_medium(mw.cart_src, sizeof(mw.cart_src), self, true)) {
            static char msg[352];
            if (!mw.cart_src[0]) {
                r_snprintf(msg, sizeof(msg), "The cartridge image is needed and the file does not name one");
            } else {
                r_snprintf(msg, sizeof(msg), "The cartridge image is needed and was not found:\n%s\nnor '%s' beside this file",
                           mw.cart_src, base_of(mw.cart_src));
            }
            err = msg;
        }
    }
    // The disk is not a refusal: build_drive() reports an image it could not
    // mount once the machine runs again (D6), so this only widens where it looks.
    if (!err && mw.present && mw.drive_img[0]) {
        (void)resolve_medium(mw.drive_img, sizeof(mw.drive_img), self, false);
    }
    if (!err && !mw.present && mw.cart_type >= 0 && mw.cart_type != int(CART_RD(type))) {
        static char msg[80];
        r_snprintf(msg, sizeof(msg), "The file is of cartridge type %02x; this machine has %02x", mw.cart_type, (int)CART_RD(type));
        err = msg;
    }
    if (!err && (!rd.get(buf, 4) || (ns = le16(buf)) < 1 || ns > SEC_LIMIT)) {
        err = rd.failed ? "The file could not be read" : "Bad section table";
    }
    if (!err && !(tab = (uint8_t *)r_malloc(uint32_t(ns) * SEC_ENTRY))) {
        err = "Out of memory";
    }
    if (!err && !rd.get(tab, uint32_t(ns) * SEC_ENTRY)) {
        err = "The file could not be read";
    }
    // the bar counts the same steps the writer counts: one per RAW_CHUNK of
    // payload plus one per section, so a load and a save of the same file move
    // at the same rate
    int steps = 0, step = 0;
    for (int i = 0; !err && i < ns; i++) {
        uint32_t l = le32(tab + i * SEC_ENTRY + 4);
        steps += int((l + RAW_CHUNK - 1) / RAW_CHUNK);   // the chunks the loop below takes
        if (!l) {
            steps++;
        }
    }
    if (g_stage_progress) {
        g_stage_progress(0, steps, g_stage_ctx);
    }
    for (int i = 0; !err && i < ns; i++) {
        uint8_t id = tab[i * SEC_ENTRY];
        uint32_t len = le32(tab + i * SEC_ENTRY + 4);
        uint8_t **dst = NULL;
        uint32_t want = 0;
        switch (id) {
        case SEC_BLOCK:   dst = &blk;  want = 4 * TRX_BLOCK_WORDS + 8;  break;
        case SEC_RAM:     dst = &ram;  want = 65536;                    break;
        case SEC_COLOR:   dst = &col;  want = 1024;                     break;
        case SEC_CARTRAM: dst = &cart; want = TRX_CART_RAM_BYTES;       break;
        case SEC_DRIVE:   dst = &drv;  want = sizeof(TrxDriveState);    break;
        case SEC_EEPROM:  dst = &eeprom; want = TRX_EEPROM_BYTES;       break;
        default:          break;      // an id from a later writer: skipped by its length
        }
        if (id == SEC_CARTROM && !have_rom) {
            // the medium: hashed on the way past, its place kept for the load
            have_rom = true;
            rom_off = rd.pos;
            rom_len = len;
            for (uint32_t left = len; left && !err;) {
                uint32_t take = left > RAW_CHUNK ? RAW_CHUNK : left;
                if (!rd.get(scratch, take)) {
                    err = "The file could not be read";
                }
                left -= take;
                if (g_stage_progress) {
                    g_stage_progress(++step, steps, g_stage_ctx);
                }
            }
        } else if (!dst || len != want || *dst) {
            if (!rd.eat(len, scratch, RAW_CHUNK)) {
                err = "The file could not be read";
            }
        } else if (!(*dst = (uint8_t *)r_malloc(len ? len : 1))) {
            err = "Out of memory";
        } else {
            // in RAW_CHUNK pieces, the size the writer writes in, so the bar moves
            // through the medium instead of jumping between sections
            uint32_t left = len;
            uint8_t *p = *dst;
            while (left && !err) {
                uint32_t take = left > RAW_CHUNK ? RAW_CHUNK : left;
                if (!rd.get(p, take)) {
                    err = "The file could not be read";
                    break;
                }
                p += take;
                left -= take;
                if (g_stage_progress) {
                    g_stage_progress(++step, steps, g_stage_ctx);
                }
            }
        }
        if (!err && !rd.eat((4 - (len & 3)) & 3, scratch, RAW_CHUNK)) {
            err = "The file could not be read";
        }
    }
    if (!err && (!blk || !ram || !col)) {
        err = "The file has no machine state";
    }
    if (!err) {
        // the body's digest, over everything behind the 41 byte head. Checked
        // before anything is put into the machine.
        uint8_t digest[32];
        trx_sha_final(&rd.sha, digest);
        if (memcmp(digest, head + 9, 32)) {
            err = "Hash mismatch";
        }
    }
    TrxC64reStaged *st = NULL;
    if (!err && !(st = (TrxC64reStaged *)r_calloc(1, sizeof(TrxC64reStaged)))) {
        err = "Out of memory";
    }
    if (!err) {
        for (int i = 0; i < TRX_BLOCK_WORDS; i++) {
            st->w[i] = le32(blk + i * 4);
        }
        st->x = le16(blk + 4 * TRX_BLOCK_WORDS);
        uint16_t flags = le16(blk + 4 * TRX_BLOCK_WORDS + 2);
        uint32_t pack = le32(blk + 4 * TRX_BLOCK_WORDS + 4);
        if ((flags & 1) && (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK)) {
            st->turbo = true;
            st->turbo_pack = pack;
        } else if (flags & 1) {
            r_log("[C64RE] the file's turbo state is not applied: this bitstream has no turbo\n");
        }
        st->ram = ram;
        st->col = col;
        st->cart = cart;
        st->rom_len = rom_len;
        st->rom_off = rom_off;
        r_snprintf(st->path, sizeof(st->path), "%s", self);
        st->drive = (TrxDriveState *)drv;
        st->eeprom = eeprom;
        st->want = mw;
        r_log("[C64RE] .c64re format 2: %u bytes, %d sections, medium %u, drive %s, eeprom %s\n",
              (unsigned)size, ns, (unsigned)rom_len, drv ? "yes" : "no", eeprom ? "yes" : "no");
        if (mw.present) {
            r_log("[C64RE] the file's machine: cartridge type %02x from '%s', drive A '%s'\n",
                  mw.cart_type, mw.cart_src[0] ? mw.cart_src : "-", mw.drive_img[0] ? mw.drive_img : "-");
        }
        *out = st;
    } else {
        r_free(ram);
        r_free(col);
        r_free(cart);
        r_free(drv);
        r_free(eeprom);
    }
    r_free(scratch);
    r_free(hdr);
    r_free(tab);
    r_free(blk);
    return err;
}

const char *trx_c64re_stage(const char *path, TrxC64reStaged **out)
{
    return trx_c64re_stage_entry(path, NULL, out);
}

const char *trx_c64re_stage_ex(const char *path, TrxC64reStaged **out, trx_progress_fn progress, void *ctx)
{
    return trx_c64re_stage_ask(path, out, progress, ctx, NULL, NULL);
}

const char *trx_c64re_stage_ask(const char *path, TrxC64reStaged **out, trx_progress_fn progress, void *ctx,
                                TrxC64reMissing *miss, const char *medium)
{
    if (miss) {
        miss->want[0] = 0;
        miss->base[0] = 0;
    }
    g_stage_progress = progress;
    g_stage_ctx = ctx;
    g_stage_miss = miss;
    g_stage_medium = medium;
    const char *err = trx_c64re_stage_entry(path, NULL, out);
    g_stage_progress = NULL;
    g_stage_ctx = NULL;
    g_stage_miss = NULL;
    g_stage_medium = NULL;
    return err;
}

static const char *stage_entry(const char *path, const char *want, TrxC64reStaged **out)
{
    RFile file_o = { 0, 0 };
    RFile *file = &file_o;
    if (!rf_open(file, path)) {
        return "The file could not be opened";
    }
    uint32_t size = uint32_t(rf_size(file));
    // format 2 is read where it stands, section by section (U8 §4); format 1 streams through f1_read (the whole file and its unpacked
    // document in the heap at once was "Out of memory" on a 2.9 MB file, U8 §1, and on every 64-entry ring)
    uint8_t head[41];
    memset(head, 0, sizeof(head));
    if (!rf_read(file, head, size < sizeof(head) ? size : uint32_t(sizeof(head)))) {
        rf_close(file);
        return "The file could not be read";
    }
    if (size >= sizeof(head) && !memcmp(head, "C64RESNP", 8) && head[8] == C64RE_FORMAT) {
        const char *e2 = stage_v2(file, head, size, path, out);
        rf_close(file);
        return e2;
    }
    const char *err = NULL;
    bool ring = size >= 9 + 18 && !memcmp(head, "C64RERNG", 8) && head[8] == 1;
    if (!ring && (size < 41 + 18 || memcmp(head, "C64RESNP", 8) || head[8] != 1)) {
        err = "Not a .c64re or .c64rering (format 1)";
    }
    F1Out fo;
    memset(&fo, 0, sizeof(fo));
    if (!err) {
        err = f1_read(file, size, ring, head + 9, want, &fo);
    }
    rf_close(file);
    char *js = fo.js;
    fo.js = NULL;
    jsmntok_t *t = NULL;
    int n = 0;
    if (!err) {
        jsmn_parser p;
        jsmn_init(&p);
        int need = jsmn_parse(&p, js, fo.jl, NULL, 0);
        if (need < 1) {
            err = "Bad JSON";
        } else {
            t = (jsmntok_t *)r_malloc(sizeof(jsmntok_t) * need);
            if (!t) {
                err = "Out of memory";
            } else {
                jsmn_init(&p);
                n = jsmn_parse(&p, js, fo.jl, t, need);
                if (n < 1) {
                    err = "Bad JSON";
                }
            }
        }
    }
    // the checkpoint: the .c64re's, or the entry of the ring that f1_read picked (the cursor's, or the one `want` names: T66 part 3)
    int cp = err ? -1 : json_key(js, t, n, 0, "checkpoint");
    uint8_t *ram = NULL, *col = NULL, *cart = NULL, *rom = NULL;
    TrxDriveState *drive = NULL;
    uint32_t rom_len = 0;
    uint32_t w[TRX_BLOCK_WORDS];
    int x = 0;
    bool have_turbo = false;
    uint32_t turbo_pack = 0;
    if (!err) {
        // a file from a turbo-capable machine (TRX64 or a later board)
        int tb = json_key(js, t, n, cp, "turbo");
        if (tb >= 0 && (HW_RD(ident_caps2) & DRV_ident_caps2_turbo_MASK)) {
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
            r_log("[C64RE] the file's turbo state (speed index %d) is not applied: this bitstream has no turbo\n",
                  (int)tok_int(js, t, json_key(js, t, n, tb, "menuIndex")));
        }
        int hw = json_key(js, t, n, cp, "trx64Hw");
        // f1_read made them: ram and col always, cart when the cartState node has cartRam (it rides in both file kinds; a file without it
        // leaves the window as it is, T66 part 5), rom when there is a cartFlash typed array
        ram = fo.ram;
        col = fo.col;
        cart = fo.cart;
        fo.ram = fo.col = fo.cart = NULL;
        // A cartridge snapshot only fits the cartridge that is on: the banking
        // registers, the RAM window and the ROM image all belong to that type.
        int ft = trx_c64re_cart_type(js, t, n, cp);
        if (ft >= 0 && ft != int(CART_RD(type))) {
            static char msg[80];
            r_snprintf(msg, sizeof(msg), "The file is of cartridge type %02x; this machine has %02x", ft, (int)CART_RD(type));
            err = msg;
        }
        // cartFlash: the ROM / flash window as the file has it. It goes back
        // into DDR2 with the load, because no log carries flash writes. The
        // firmware caps it at the ROM window; here the cap is the image the
        // cartridge in the port has (the source's rom_bytes), when it says one.
        // f1_read decoded it (tok_ta_len's length, capped as here) into fo.rom.
        uint32_t fl = err ? 0 : fo.rom_cap;
        if (fl) {
            if (fo.rom && fo.rom_got == fl) {
                rom = fo.rom;
                fo.rom = NULL;
                rom_len = fl;
            } else {                // short or no memory: the window stays as it is
                r_log("[C64RE] the file's cartFlash node (%u KB) could not be read: the cartridge "
                      "image in memory stays as it is\n", (unsigned)(fl >> 10));
            }
        }
        // drive A (T66 part 8): both kinds of file carry the node the same way (a ring entry's is null: nothing to read)
        int dn = err ? -1 : json_key(js, t, n, cp, "trx64Drive");
        if (dn >= 0 && t[dn].type == JSMN_OBJECT && trx_drive_present()) {
            drive = (TrxDriveState *)r_malloc(sizeof(TrxDriveState));
            if (drive && !trx_c64re_drive_from_node(js, t, n, cp, drive)) {
                r_free(drive);
                drive = NULL;
                r_log("[C64RE] the file's drive node could not be read: drive A stays as it is\n");
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
                r_log("[C64RE] trx64Hw layout %d is not this build's %d: the nodes are used instead\n",
                      (int)tok_int(js, t, json_key(js, t, n, hw, "layout")), LAYOUT);
            }
            err = model_refusal(js, t, n);
            TrxNodeState ns;
            ns.cart = cart;
            ns.ram_bytes = int(fo.ram_got);     // f1_read decoded the typed arrays: from_nodes takes what it got
            ns.cart_bytes = fo.cart_got;
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
                    r_free(cart);
                    cart = NULL;
                }
                say_coarse(&ns);
            }
        } else if (fo.ram_got != 65536 ||
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
                (fo.cart_got >= 0 ? fo.cart_got : trx_c64re_cart_ram(js, t, n, cp, cart, TRX_CART_RAM_BYTES)) != TRX_CART_RAM_BYTES) {
                r_free(cart);               // a short cartRam node: leave the window alone
                cart = NULL;
            }
        }
    }
    r_free(t);
    r_free(js);
    f1_free(&fo);                   // what was not taken over (all of it after an error)
    TrxC64reStaged *st = NULL;
    if (!err) {
        st = (TrxC64reStaged *)r_calloc(1, sizeof(TrxC64reStaged));
        if (!st) {
            err = "Out of memory";
        }
    }
    if (err) {
        r_free(ram);
        r_free(col);
        r_free(cart);
        r_free(rom);
        r_free(drive);
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
    st->want.cart_type = -1;        // a format 1 file builds no machine (it has no machineState)
    st->want.machine = -1;
    *out = st;
    return NULL;
}

const char *trx_c64re_stage_entry(const char *path, const char *want, TrxC64reStaged **out)
{
    *out = NULL;
    bool built = trx_c64re_source_begin(true);
    const char *err = stage_entry(path, want, out);
    if (!err && *out) {
        (*out)->src = g_src;        // the hooks the apply builds the machine with, also after the source is gone
        (*out)->src_on = g_src_on;
    }
    trx_c64re_source_end(built);
    return err;
}

void trx_c64re_discard(TrxC64reStaged *st)
{
    if (!st) {
        return;
    }
    r_free(st->ram);
    r_free(st->col);
    r_free(st->cart);
    r_free(st->rom);
    r_free(st->drive);
    r_free(st->eeprom);
    r_free(st);
}

// ---- U9: the machine the state needs, built before the state goes in --------------

static char g_missed[160];      // what the last load could not do, for the message (D6)

static void note(char *buf, int cap, const char *what)
{
    int n = int(strlen(buf));
    if (n + int(strlen(what)) + 3 >= cap) {
        return;
    }
    if (n) {
        buf[n++] = ',';
        buf[n++] = ' ';
    }
    strcpy(buf + n, what);
}

// D2: insert, never boot: the source's cart_for_state puts the cartridge of the
// .crt into the port without a reset, or empties the port (NULL).
//
// The machine's own state is not consulted: whatever is in the port now, the
// state's cartridge is what the machine gets (Alex, 2026-11-08: "ich will den
// state wiederherstellen, egal was gerade der zustand ist"). Re-inserting the
// same cartridge is not a waste - it is what makes the result independent of
// what came before.
static void build_cartridge(const TrxMachineWant &mw, char *missed, int cap)
{
    if (!g_src_on || !g_src.cart_for_state) {
        const C64CartApi *cc = R_TABLE(C64CartApi, RX_C64CART, active);
        if (mw.cart_type > 0 || (cc && cc->active())) {
            note(missed, cap, "the cartridge");     // no hook to insert or take out a cartridge with
        }
        return;
    }
    if (mw.cart_type <= 0) {
        // the state had no cartridge, so neither does the machine now
        if (g_src.cart_for_state(g_src.rom_ctx, NULL) != 0) {
            note(missed, cap, "the cartridge");
        } else {
            r_log("[C64RE] no cartridge in this state: the port is empty\n");
        }
        return;
    }
    if (g_src.cart_for_state(g_src.rom_ctx, mw.cart_src) != 0) {
        // stage_v2() has already proved the file is there and readable, so this
        // is a .crt that is there but broken: nothing was inserted
        r_log("[C64RE] '%s' could not be read: no cartridge was inserted\n", mw.cart_src);
        note(missed, cap, "the cartridge");
        return;
    }
    r_log("[C64RE] cartridge type %02x inserted from %s, now type %02x\n", mw.cart_type, mw.cart_src, (int)CART_RD(type));
    if (int(CART_RD(type)) != mw.cart_type) {
        note(missed, cap, "the cartridge");
    }
}

// The image drive A had. Mounted before the drive's own state goes in, because
// a mount resets the drive and would overwrite it.
static void build_drive(const TrxMachineWant &mw, char *missed, int cap)
{
    if (!mw.drive_img[0]) {
        return;
    }
    if (!g_src_on || !g_src.drive_mount) {
        note(missed, cap, "the disk in drive A");
        return;
    }
    // mounted unconditionally, for the same reason the cartridge is inserted
    // unconditionally: the state decides, not what happens to be in the drive
    const char *name = base_of(mw.drive_img);
    const char *dot = strrchr(name, '.');
    if (name == mw.drive_img || !dot || (r_strcasecmp(dot, ".g64") && r_strcasecmp(dot, ".d71") &&
                                         r_strcasecmp(dot, ".d81") && r_strcasecmp(dot, ".d64"))) {
        r_log("[C64RE] drive A: '%s' is not an image this can mount\n", mw.drive_img);
        note(missed, cap, "the disk in drive A");
        return;
    }
    char spec[160];
    r_snprintf(spec, sizeof(spec), "%s%s", mw.drive_mode == 2 ? "unlinked " : mw.drive_mode == 1 ? "ro " : "", mw.drive_img);
    if (g_src.drive_mount(g_src.rom_ctx, spec) != 0) {
        r_log("[C64RE] drive A: '%s' was not mounted\n", mw.drive_img);
        note(missed, cap, "the disk in drive A");
    } else {
        r_log("[C64RE] drive A: '%s' mounted\n", mw.drive_img);
    }
}

// D4: the registers, directly, for as long as this state runs. The settings
// are not written, so nothing reaches the flash.
static void build_settings(const TrxMachineWant &mw, char *missed, int cap)
{
    if (!mw.has_settings) {
        return;
    }
    const UciApi *uci = R_TABLE(UciApi, RX_UCI, set_slot_enable) ? R_TABLE(UciApi, RX_UCI, set_slot_base) : NULL;
    if ((fpga_caps() & CAPAB_COMMAND_INTF) && uci) {
        uci->set_slot_enable(mw.uci ? 1 : 0);
        if (mw.uci) {
            uci->set_slot_base(0x47);       // $DF1C
        }
    } else if (mw.uci) {
        note(missed, cap, "UCI");
    }
    if (fpga_caps() & CAPAB_SAMPLER) {
        CART_WR(sampler, mw.sampler ? 1 : 0);
    } else if (mw.sampler) {
        note(missed, cap, "the sampler");
    }
    // the REU is off in every state a ring can produce (A08); this only puts
    // back an "off" that the machine may currently contradict
    CART_WR(reu, mw.reu ? 1 : 0);
    CART_WR(reu_size, mw.reu_size);
}

// The video standard, before anything else: it drives the PLL and the colour
// timing, and the chip block that follows is of a machine running at that rate.
// The source's machine_set puts it in for this run only, so the user's own
// setting never reaches the flash - the same bargain as the other settings (U9 D4).
static void build_machine_mode(const TrxMachineWant &mw, char *missed, int cap)
{
    if (mw.machine < 0 || (g_src_on && mw.machine == g_src.machine)) {
        return;
    }
    if (!g_src_on || !g_src.machine_set || g_src.machine_set(g_src.rom_ctx, mw.machine) != 0) {
        note(missed, cap, "the video standard");
        return;
    }
    r_log("[C64RE] system mode %d -> %d (%s)\n", g_src_on ? g_src.machine : -1, mw.machine, machine_names[mw.machine]);
}

static void build_machine(TrxC64reStaged *st, char *missed, int cap)
{
    build_machine_mode(st->want, missed, cap);   // also for a file from before U9
    if (!st->want.present) {
        return;                     // nothing else in it to build
    }
    build_cartridge(st->want, missed, cap);
    build_drive(st->want, missed, cap);
    build_settings(st->want, missed, cap);
}

// Breaks text into lines that fit cols: at a space or a '/' (or '-', '_') where
// there is one near the end, and hard where there is not.
void trx_c64re_wrap(char *dst, int cap, const char *text, int cols)
{
    int n = 0;
    if (cols < 8) {
        cols = 8;
    }
    while (*text && n < cap - 1) {
        const char *eol = strchr(text, '\n');
        int left = eol ? int(eol - text) : int(strlen(text));
        while (left > cols && n < cap - 1) {
            int cut = cols;
            for (int i = cols; i > cols / 2; i--) {
                // a break the eye expects: the separator starts the next line, so
                // a name breaks as "wasteland_ef_v1.0.1-20261108" / "-152059.c64re"
                if (text[i] == ' ' || text[i] == '/' || text[i] == '-' || text[i] == '_') {
                    cut = i;
                    break;
                }
            }
            for (int i = 0; i < cut && n < cap - 1; i++) {
                dst[n++] = text[i];
            }
            text += cut;
            left -= cut;
            while (*text == ' ') {
                text++;
                left--;
            }
            if (n < cap - 1) {
                dst[n++] = '\n';
            }
        }
        for (int i = 0; i < left && n < cap - 1; i++) {
            dst[n++] = text[i];
        }
        text += left;
        if (*text == '\n') {
            text++;
            if (n < cap - 1) {
                dst[n++] = '\n';
            }
        }
    }
    dst[n < cap ? n : cap - 1] = 0;
}

const char *trx_c64re_missed(void)
{
    return g_missed[0] ? g_missed : NULL;
}

// The cartridge image into DDR2 through the source's rom_write while the machine stands (TrxLoadMem::rom_put): from the staged buffer of a format 1
// file, or from the format 2 file again, piece by piece (a cartridge is megabytes).
static void rom_put(void *ctx)
{
    TrxC64reStaged *st = (TrxC64reStaged *)ctx;
    if (st->rom) {
        for (uint32_t k = 0; k < st->rom_len; k += RAW_CHUNK) {
            uint32_t want = st->rom_len - k > RAW_CHUNK ? RAW_CHUNK : st->rom_len - k;
            g_src.rom_write(g_src.rom_ctx, k, st->rom + k, want);
        }
        return;
    }
    RFile f;
    uint8_t *chunk = (uint8_t *)r_malloc(RAW_CHUNK);
    if (!chunk || !rf_open(&f, st->path)) {
        r_free(chunk);
        r_log("[C64RE] the cartridge image could not be read back from %s\n", st->path);
        return;
    }
    for (uint32_t k = 0; k < st->rom_len; k += RAW_CHUNK) {
        uint32_t want = st->rom_len - k > RAW_CHUNK ? RAW_CHUNK : st->rom_len - k;
        if (!rf_seek(&f, uint64_t(st->rom_off) + k) || !rf_read(&f, chunk, want)) {
            r_log("[C64RE] the cartridge image could not be read back from %s\n", st->path);
            break;
        }
        g_src.rom_write(g_src.rom_ctx, k, chunk, want);
    }
    rf_close(&f);
    r_free(chunk);
}

const char *trx_c64re_apply(TrxC64reStaged *st)
{
    if (!st) {
        return "Nothing staged";
    }
    // the hooks of the source the file was staged with
    TrxC64reSrc keep = g_src;
    bool keep_on = g_src_on;
    g_src = st->src;
    g_src_on = st->src_on;
    // U9 D2, and the order is the whole of it: build the machine the state needs
    // before the state goes into it, and never through a reset. What each step
    // could not do is collected for D6 rather than stopping the load - the state
    // itself is what the user came for.
    char missed[160];
    missed[0] = 0;
    build_machine(st, missed, sizeof(missed));

    if (st->turbo) {
        trx_ring_set_turbo(st->turbo_pack);     // one shot, taken by the load below
    }
    bool rom = st->rom_len && g_src_on && g_src.rom_write;
    if (st->rom_len && !rom) {
        note(missed, sizeof(missed), "the cartridge image");
    }
    TrxLoadMem mem = { st->ram, st->col, st->cart, rom ? rom_put : NULL, st };
    int e = trx_ring_load_state(&mem, st->w, st->x);
    if (!e && st->eeprom) {
        // U9 D3a: after the block, because replay_cart()'s $DE00 write drives the
        // 93C86's lines on its way past
        if (g_src_on && g_src.eeprom_put) {
            g_src.eeprom_put(g_src.rom_ctx, st->eeprom);
        } else {
            note(missed, sizeof(missed), "the cartridge's EEPROM");
        }
    }
    if (!e && st->drive) {
        // drive A after the machine, and after the mount above: the stub runs on
        // the drive's own CPU and is stepped, so the C64's state does not matter
        // (T66 part 8), but a mount would overwrite what the stub just wrote
        const char *derr = trx_drive_write(st->drive);
        if (derr) {
            r_log("[C64RE] drive A was not put back: %s\n", derr);
            note(missed, sizeof(missed), "drive A");
        }
    }
    if (!e && missed[0]) {
        r_log("[C64RE] the state is in, but not: %s\n", missed);
    }
    r_snprintf(g_missed, sizeof(g_missed), "%s", missed);
    g_src = keep;
    g_src_on = keep_on;
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
        r_log("[C64RE] loading the snapshot failed: %s\n", err);
    }
}
