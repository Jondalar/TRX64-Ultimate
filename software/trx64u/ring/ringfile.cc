// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_c64re.cc and trx_drive.cc (.c64re and .c64rering from the frame ring, drive A's
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
        uint32_t c = crc;
        int fl = fill, lm = lim;
        uint8_t *const b = buf;
        for (int i = 0; i < len; i++) {
            uint8_t ch = uint8_t(s[i]);
            c = crc8(c ^ ch);
            b[fl++] = ch;
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

    void num(const char *prefix, int64_t v)
    {
        char tmp[24];
        int n = 0;
        if (prefix) {
            str(prefix);
        }
        uint64_t u = v < 0 ? uint64_t(-v) : uint64_t(v);
        do {                            // written for the firmware's printf, which has no 64-bit conversions
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
        if (count) {
            isize += 4 * uint32_t((len + 2) / 3);
            return;
        }
        uint32_t c = crc;
        int fl = fill, lm = lim;
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
    if (trx_rd8(TRX_CAPS2) & TRX_CAPS2_TURBO) {
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
    g.num(",\"cartridgeType\":", io_cart_rd(C64U_CART_TYPE));
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
#ifndef CAPS_DBG
#define CAPS_DBG       0x08
#endif
#define P3(o)          (TRX_IO_BASE + 0x3000 + (o))
#define DRV_CMD        P3(0x000)
#define DRV_ARG        P3(0x004)
#define DRV_STAT       P3(0x0C4)
#define DRV_CYCLES     P3(0x0C8)
#define CMD_DRV_PAUSE  0x10
#define CMD_DRV_RUN    0x11
#define CMD_DRV_STEP   0x12
#define ST_HALTED      0x01
#define ST_STEPPING    0x04
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
        if ((trx_io_read32(DRV_STAT) & mask) == want) {
            return true;
        }
    }
    return (trx_io_read32(DRV_STAT) & mask) == want;
}
static bool drv_hold(void)
{
    trx_io_write32(DRV_CMD, CMD_DRV_PAUSE);
    return drv_wait(ST_HALTED, ST_HALTED, 50);
}
static void drv_free(void) { trx_io_write32(DRV_CMD, CMD_DRV_RUN); }
static bool drv_step(int n)
{
    trx_io_write32(DRV_ARG, uint32_t(n));
    trx_io_write32(DRV_CMD, CMD_DRV_STEP);
    return drv_wait(ST_STEPPING, 0, 200);
}
static uint16_t drv_pc(void) { return uint16_t(trx_io_read32(DRV_STAT) >> 16); }

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
    return (trx_io_caps() & CAPS_DBG) && drive_powered();
}

const char *trx_drive_read(TrxDriveState *st, bool with_regs)
{
    memset(st, 0, sizeof(*st));
    if (!(trx_io_caps() & CAPS_DBG)) {
        return "This bitstream has no debug page, so drive A cannot be read";
    }
    if (!drive_powered()) {
        return "Drive A is not powered";
    }
    dread(0, st->ram, TRX_DRIVE_RAM);
    st->cycles = trx_io_read32(DRV_CYCLES);
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
        st->cycles = trx_io_read32(DRV_CYCLES);
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
    if (!(trx_io_caps() & CAPS_DBG)) {
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
    if (trx_rd8(TRX_CAPS2) & TRX_CAPS2_TURBO) {
        uint32_t w1 = trx_io_read32(TRX_TURBO_W1), w2 = trx_io_read32(TRX_TURBO_W2);
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
                     C64RE_FORMAT, SVC_HAS(SvcSys, sys, app_version) ? sys->app_version() : "", machine_name(), (unsigned)trx_io_read32(TRX_ID),
                     (unsigned)trx_rd8(TRX_CAPS), (unsigned)trx_rd8(TRX_CAPS2), TRX_BLOCK_WORDS);
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
        n += r_snprintf(dst + n, cap - n, ",\"cartridgeType\":%u,\"source\":\"", (unsigned)io_cart_rd(C64U_CART_TYPE));
        n += json_str(dst + n, cap - n, g_src.cart_src);
        n += r_snprintf(dst + n, cap - n, "\",\"mediumBytes\":%u", (unsigned)rom_len);
    }
    n += r_snprintf(dst + n, cap - n, ",\"machineState\":{");
    if (cart) {
        // require/prohibit: the cartridge definition's flags as c64cart reports them (0 when the base does not know them). Written for the file's
        // reader; no load here reads them back (TrxMachineWant parses them and nothing uses the fields).
        n += r_snprintf(dst + n, cap - n, "\"cartridge\":{\"type\":%u,\"bytes\":%u,\"require\":%u,\"prohibit\":%u,\"source\":\"",
                        (unsigned)io_cart_rd(C64U_CART_TYPE), (unsigned)rom_len, (unsigned)g_src.cart_require, (unsigned)g_src.cart_prohibit);
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
                  (unsigned)(io_cart_rd(C64U_CART_REU_ENABLE) ? 1 : 0), (unsigned)io_cart_rd(C64U_CART_REU_SIZE),
                  io_cart_rd(C64U_CART_SAMPLER) ? "true" : "false");
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

// The register the dump with no ring tests before it reads the VIC registers and the colour RAM as bus cycles (machine_read).
#define DRV_STATUS_REG (TRX_IO_BASE + 0x3008)

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
        if (!trx_ring_cart_state_ok(io_cart_rd(C64U_CART_TYPE), &cname)) {
            static char refused[96];
            r_snprintf(refused, sizeof(refused), "A state of this cartridge (type %02x) cannot be loaded back yet", (int)io_cart_rd(C64U_CART_TYPE));
            return refused;
        }
        if (trx_ring_cart_has_eeprom(io_cart_rd(C64U_CART_TYPE))) {
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
    bool old = io_bus_rd(C64U_BUS_DMA_MEMONLY) != 0;
    io_memonly(true);
    g_api->c64->dma_read(0, ram, 65536);
    uint32_t st = trx_io_read32(DRV_STATUS_REG);
    if ((st & 1) && (st & (0x02 | 0x20))) {                  // the gate is shut and a stop is in force: the window's D page is a bus cycle
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
    char *out = (char *)r_malloc(isize + 1);
    if (!out) {
        static char msg[64];
        r_snprintf(msg, sizeof(msg), "The file unpacks to %u KB, which did not fit", (unsigned)(isize >> 10));
        *err = msg;
        return NULL;
    }
    uint32_t got = 0;
    int e = trx_gunzip(p, len, (uint8_t *)out, isize, &got);
    if (e) {
        r_free(out);
        *err = trx_inflate_error(e);
        return NULL;
    }
    out[got] = 0;
    *out_len = got;
    return out;
}

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
    int64_t idx_want = by_index ? r_strtoll(want + 1) : -1;
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
                  (fr >= 0 && all_digits(want) && tok_int(js, t, fr) == r_strtoll(want));
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
    char name[41];
    r_copy(name, sizeof(name), js + t[m].start, size_t(t[m].end - t[m].start));
    r_snprintf(msg, sizeof(msg), "The file is for machine \"%s\", not c64-pal", name);
    return msg;
}

// What the coarse load took and what it could not take, in the log: the
// user is told the load is not exact, and by how much.
static void say_coarse(const TrxNodeState *ns)
{
    r_log("[C64RE] coarse load (no trx64Hw node): cpu%s%s%s%s%s%s%s%s\n",
          (ns->have & TRX_NODE_PORT) ? " port" : "", (ns->have & TRX_NODE_CIA1) ? " cia1" : "",
          (ns->have & TRX_NODE_CIA2) ? " cia2" : "", (ns->have & TRX_NODE_VIC) ? " vic" : "",
          (ns->have & TRX_NODE_COLOR) ? " colour" : "", (ns->have & TRX_NODE_RAM) ? " ram" : "",
          (ns->have & TRX_NODE_CART) ? " cart-banking" : "",
          (ns->have & TRX_NODE_CARTRAM) ? " cart-ram" : "");
    r_log("[C64RE] not restored: the VIC's raster position (file line %d cycle %d) and pixel pipeline, "
          "the SID, the drive\n", ns->line, ns->cycle);
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

static trx_progress_fn g_stage_progress;    // the load's bar (U9): set for one stage_v2()
static void *g_stage_ctx;
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
    if (!err && !mw.present && mw.cart_type >= 0 && mw.cart_type != int(io_cart_rd(C64U_CART_TYPE))) {
        static char msg[80];
        r_snprintf(msg, sizeof(msg), "The file is of cartridge type %02x; this machine has %02x", mw.cart_type, (int)io_cart_rd(C64U_CART_TYPE));
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
        if ((flags & 1) && (trx_rd8(TRX_CAPS2) & TRX_CAPS2_TURBO)) {
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
    // format 2 is read where it stands, section by section (U8 §4); only format 1
    // needs the whole file plus its unpacked document in the heap at once, which
    // is what "Out of memory" on a 2.9 MB file was (U8 §1).
    uint8_t head[41];
    if (size >= sizeof(head) && rf_read(file, head, sizeof(head)) && !memcmp(head, "C64RESNP", 8) && head[8] == C64RE_FORMAT) {
        const char *e2 = stage_v2(file, head, size, path, out);
        rf_close(file);
        return e2;
    }
    rf_seek(file, 0);
    uint8_t *raw = (uint8_t *)r_malloc(size ? size : 1);
    const char *err = NULL;
    if (!raw) {
        err = "Out of memory";
    } else if (!rf_read(file, raw, size)) {
        err = "The file could not be read";
    }
    rf_close(file);
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
    r_free(raw);
    jsmntok_t *t = NULL;
    int n = 0;
    if (!err) {
        jsmn_parser p;
        jsmn_init(&p);
        int need = jsmn_parse(&p, js, jl, NULL, 0);
        if (need < 1) {
            err = "Bad JSON";
        } else {
            t = (jsmntok_t *)r_malloc(sizeof(jsmntok_t) * need);
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
        // a file from a turbo-capable machine (TRX64 or a later board)
        int tb = json_key(js, t, n, cp, "turbo");
        if (tb >= 0 && (trx_rd8(TRX_CAPS2) & TRX_CAPS2_TURBO)) {
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
        ram = (uint8_t *)r_malloc(65536);
        col = (uint8_t *)r_malloc(1024);
        // cartridge RAM rides in the cartState node of both file kinds; a file
        // without it leaves the window as it is (T66 part 5)
        if (json_key(js, t, n, json_key(js, t, n, cp, "cartState"), "cartRam") >= 0) {
            cart = (uint8_t *)r_malloc(TRX_CART_RAM_BYTES);
        }
        // A cartridge snapshot only fits the cartridge that is on: the banking
        // registers, the RAM window and the ROM image all belong to that type.
        int ft = trx_c64re_cart_type(js, t, n, cp);
        if (ft >= 0 && ft != int(io_cart_rd(C64U_CART_TYPE))) {
            static char msg[80];
            r_snprintf(msg, sizeof(msg), "The file is of cartridge type %02x; this machine has %02x", ft, (int)io_cart_rd(C64U_CART_TYPE));
            err = msg;
        }
        // cartFlash: the ROM / flash window as the file has it. It goes back
        // into DDR2 with the load, because no log carries flash writes. The
        // firmware caps it at the ROM window; here the cap is the image the
        // cartridge in the port has (the source's rom_bytes), when it says one.
        uint32_t fl = err ? 0 : trx_c64re_cart_flash_len(js, t, n, cp);
        if (fl) {
            if (g_src_on && g_src.rom_bytes && fl > g_src.rom_bytes) {
                fl = g_src.rom_bytes;
            }
            rom = (uint8_t *)r_malloc(fl);
            if (rom && trx_c64re_cart_flash(js, t, n, cp, rom, fl) == fl) {
                rom_len = fl;
            } else {
                r_free(rom);        // short or no memory: the window stays as it is
                rom = NULL;
                r_log("[C64RE] the file's cartFlash node (%u KB) could not be read: the cartridge "
                      "image in memory stays as it is\n", (unsigned)(fl >> 10));
            }
        }
        // drive A (T66 part 8): both kinds of file carry the node the same way
        if (!err && json_key(js, t, n, cp, "trx64Drive") >= 0 && trx_drive_present()) {
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
                r_free(cart);               // a short cartRam node: leave the window alone
                cart = NULL;
            }
        }
    }
    r_free(t);
    r_free(js);
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
    r_log("[C64RE] cartridge type %02x inserted from %s, now type %02x\n", mw.cart_type, mw.cart_src, (int)io_cart_rd(C64U_CART_TYPE));
    if (int(io_cart_rd(C64U_CART_TYPE)) != mw.cart_type) {
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
        io_cart_wr(C64U_CART_SAMPLER, mw.sampler ? 1 : 0);
    } else if (mw.sampler) {
        note(missed, cap, "the sampler");
    }
    // the REU is off in every state a ring can produce (A08); this only puts
    // back an "off" that the machine may currently contradict
    io_cart_wr(C64U_CART_REU_ENABLE, mw.reu ? 1 : 0);
    io_cart_wr(C64U_CART_REU_SIZE, mw.reu_size);
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
