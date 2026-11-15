// The freeze menu on the TRX64 Ultimate pixel layer. See trx_video.h.
//
// Hardware (TRX64-Ultimate designs/c64_basic/trx_sys.v, freeze_resp.v, pix_layer.v;
// 32-bit registers, trx_io.h):
//   0x101A0100 GATE_CTRL    b0 the app's C64_STOP pauses the machine and opens
//                           freeze sessions (reset 0: CPU-only pause); R b8 halted
//   0x101A5000 SH_CTRL      b1:0 SH_MODE, b3 PROTO: the session's writes are
//                           Gideon's freeze protocol and are not committed at its
//                           end (CAPS2 b3; the hardware clears it at the end).
//                           Only on a core with the freeze session; on the slim
//                           core (trx_io_slim()) it, SH_STATUS and the counters
//                           of page 5 read 0 and ignore writes
//   0x101A5004 SH_STATUS    b0 session, b31:16 sessions started (not on the slim core)
//   0x101A5028 TX_VIEW      the slim core's VIC view for the text layer: b15 ECM,
//                           b14 BMM, b13 MCM, b12:6 $D018 bits 7:1, b5:2 $D021,
//                           b1:0 the CIA 2 bank pins; reset 0x0243 = the menu's
//   0x101A6060 STILL_CTRL   b0 capture;  6064 STILL_BASE (slot 1 at +0x10000)
//   0x101A6068 STILL_STATUS b0 valid, b1 slot of the last complete frame, b2 that
//                           frame is 60 Hz (400 x 240; else 400 x 270), b31:8 count
//   0x101A6080 PX_CTRL      b0 enable, b3:1 format (1 RGB565), b5:4 scale
//   0x101A6084 PX_FRONT, 6088 PX_BACK (flip at the next frame start, IRQ_CAUSE b7),
//   6090 PX_SIZE {h, w}, 6094 PX_STRIDE, 6098 PX_POS {y0, x0}, 609C PX_GALPHA,
//   60A0 PX_KEY {b16 on, b15:0 the transparent RGB565 value}
//   0x101A60C0 TX_CTRL      b0 show: the C64 text layer (c64_text.v) draws the
//                           menu, b1 opaque; reads b31:24 = 1 where the layer exists
//                           (it draws while a freeze session runs), 2 on the slim core
//   0x101A60C4 TX_STATUS    b0 shown now, b15:8 lines not drawn (late fetch)
// Only a bitstream with CAPS b7 (RGB565 layer) is used; on an older one (ARGB8888)
// the freeze stays the CPU-only pause with the menu on the VIC.
//
// Picture: the scaler shows a 400 x 270 VIC window (raster x 96.., line 16..; the
// 60 Hz System Modes 400 x 240 from line 31) in the HDMI row and at the scale the
// bitstream runs (U16: mode_req.v reads them back at 0x4F07 / 0x4F0A; a bitstream
// before U16 runs 1080p at the scale now called Fit). Where the window lies and
// which VIC pixel each output pixel shows is the generated table trx_hdmi_modes.h
// (tools/hdmi_modes.py, the numbers of the gateware's hm_geom); trx_hdmi.h turns
// it into the layer's plan: the window at scale 1 where it is at most 960 output
// pixels wide, else at scale 2 at half its size, from the window's first output
// pixel, and every layer pixel the VIC pixel the scaler shows at its first output
// pixel. 1080p at Fit is 800 x 540 at scale 2 from (160, 0), as before U16. The
// layer covers only the C64 window, not the bars: at most 960 x 540 (1080p Fill,
// 1,036,800 bytes) per buffer. The geometry is taken when the freeze starts and
// again whenever the running row or scale changes while it is up (the menu's
// "HDMI Scan Resolution" and "HDMI Picture Scale"); the buffers only grow.
// Every pixel is opaque: the still mixed with the menu's background colour
// ($D021), out = still * t + background * (1 - t) per channel, t from the
// "Overlay Transparency" setting (0, 25, 50, 75 %; trx_video_set_look()), and on
// it the text screen's pixels, the background colour ($D021, multicolour 00, ECM
// background 0) left out so the mixed still shows through it, as in Gideon's HDMI
// overlay box. No border band (T12 open question 4).
// The colour key stays off: the still is in the same buffer, so "background
// colour = key" would show the live scaler picture below instead of the frozen
// frame, and outside the window the layer is not drawn at all (the picture below
// passes there without a key). With the key off no palette colour, dimmed
// or not, can turn transparent by accident.
//
// The text: on a bitstream with the C64 text layer (TX_CTRL reads 1 or 2 in b31:24)
// the hardware draws it on top of the pixel layer, from the C64's RAM, colour
// RAM and char ROM and the VIC registers written in the session (the slim core:
// TX_VIEW), every frame, so a key press shows in the next frame. It shows while
// TX_CTRL b0 is set (T12 §11 Q3): trx_video_menu_on() sets it at the end of
// C64::freeze(), once the menu's screen is up, trx_video_menu_off() clears it at
// the start of the restore, before the program's bytes go back, and the teardown
// and Timewarp clear it too. Its background ($D021) is transparent, so the
// mixed still shows through as below. The task then only watches for the end
// of the freeze. Without the layer the task draws the text in software:
//
// The text screen is read through the app's DMA window, as the app itself
// writes it: inside a freeze session every access is answered by the responder
// without a C64 cycle. The window decodes as the machine's PLA with Ultimax
// forced, so only $0000-$0FFF of RAM is visible: the menu's screen ($0400) and
// charset ($0800) are; a screen or charset elsewhere (the program's own, at a
// moment the menu is not shown) is left out. The char ROM comes from the app's
// own copy. (Only on a core with the freeze session: on the slim core the text
// layer draws, and nothing here reads the VIC.)
//
// No black frame on the way in or out: the pause blanks the C64 window at once
// (vic_scaler.blank(c64_halted)) and keeps it blank until c64_core resumes the
// VIC after whole frames. So freeze_enter() copies the last complete still while
// the machine still runs, draws it into a buffer, switches the layer on and waits
// for a frame start before freeze() stops the machine; the menu text comes later
// over it (text layer or task). freeze_leave() keeps the layer until the machine runs
// again (GATE_CTRL b8 clear) and a whole new frame has gone out, then switches it
// off at a frame start. The still on the layer is that copy, a few frames older
// than the pause, and stays the same for the whole freeze.
//
// The slim core has no freeze session and no commit list: C64::freeze() stops the
// machine and touches no chip (C64::freeze_slim), the app's accesses are bus
// cycles on the paused machine, and what the menu draws on is memory that is saved
// and put back. The commit list below (T12 §11 Q1, CAPS2 b3) is that of the cores
// before it: the I/O writes a session collects reach the chips at its end.
// Gideon's freeze protocol (stop, backup_io, init_io; restore_io, resume) puts
// back what it changes, and on the paused machine some of its writes would be
// wrong for real ($D012 from determine_d012(), $D019 acknowledges, the SID
// dummies), so SH_CTRL PROTO is set around it: from freeze_enter() to menu_on()
// and from menu_off() to the end of the session. The menu's own writes (colours,
// keyboard scan) and writemem/monitor pokes while the menu is up are listed;
// restore_io() puts the menu's back, so for a plain freeze the commit writes
// nothing.
//
// Timewarp (trx_video_tw_*, T20 §16.7) takes the layer over while it is open:
// the text layer off, the still at full brightness (t = 4/4) and one bar over
// the lower border drawn into the same buffer; after a scrub step the still is
// copied again (the frame the machine just drew). The teardown ends it.
//
// Software rendering (no text layer): a task, every 40 ms while a session shows
// the menu: reads registers, screen and colour RAM, draws the cells that differ
// from what the back buffer holds, flips (PX_BACK, waits for the flip-done
// interrupt). With one buffer (the second did not fit) it draws into the shown one.

#include "trx_video.h"
#include "trx_io.h"
#include "trx_hdmi.h"
#include "trx_lv_port.h"
#include "c64.h"
#include "u64.h"
#include "init_function.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_CTRL      (TRX_IO_BASE + 0x0100)
#define GATE_FRZ       0x001
#define GATE_HALTED    0x100
#define SH_CTRL        (TRX_IO_BASE + 0x5000)
#define SH_MODE_MASK   0x03
#define SH_PROTO       0x08
#define SH_STATUS      (TRX_IO_BASE + 0x5004)
#define SH_SESSION     0x01
#define TX_VIEW        (TRX_IO_BASE + 0x5028)
#define TX_VIEW_MENU   0x0243   // text mode, screen $0400, charset $0800, background 0, bank 0
#define STILL_CTRL     (TRX_IO_BASE + 0x6060)
#define STILL_BASE     (TRX_IO_BASE + 0x6064)
#define STILL_STATUS   (TRX_IO_BASE + 0x6068)
#define PX_CTRL        (TRX_IO_BASE + 0x6080)
#define PX_FRONT       (TRX_IO_BASE + 0x6084)
#define PX_BACK        (TRX_IO_BASE + 0x6088)
#define PX_SIZE        (TRX_IO_BASE + 0x6090)
#define PX_STRIDE      (TRX_IO_BASE + 0x6094)
#define PX_POS         (TRX_IO_BASE + 0x6098)
#define PX_GALPHA      (TRX_IO_BASE + 0x609C)
#define PX_KEY         (TRX_IO_BASE + 0x60A0)
#define PX_STATUS      (TRX_IO_BASE + 0x60A4)
#define PX_VBLANK      0x02
#define PX_CTRL_OFF    0x22 // RGB565, scale 2, disabled
#define PX_CTRL_FMT    0x02 // b3:1 = 1: RGB565; b5:4 the scale, b0 enable (px_ctrl_on())
#define TX_CTRL        (TRX_IO_BASE + 0x60C0)
#define TX_SHOW        0x01

// The scaler's window (vic_scaler.v X0 = 96, Y0 = 16; 60 Hz Y0 = 31)
#define WIN_W          TRX_HDMI_WIN_W
#define WIN_H          270
#define TEXT_COLS      40

#define STILL_SLOT     0x10000
#define STILL_FRAME    (WIN_W * WIN_H / 2)
#define WIN_H_60       240  // the 60 Hz window: 240 rows
#define STILL_FRAME_60 (WIN_W * WIN_H_60 / 2)
#define STILL_BYTES    (STILL_SLOT + STILL_FRAME)

#define RENDER_MS      40
#define CHARS_EVERY    25   // passes between charset re-reads

#define NCELLS         1000

// The C64 palette the picture is drawn with (T59: the app's, as written to
// C64_PALETTE and shown by vic_scaler.v / c64_text.v), so the menu matches the
// picture. Colodore until refresh_palette() has read the app's.
static uint32_t c_palette[16] = {
    0x000000, 0xFFFFFF, 0x813338, 0x75CEC8, 0x8E3C97, 0x56AC4D, 0x2E2C9B, 0xEDF171,
    0x8E5029, 0x553800, 0xC46C71, 0x4A4A4A, 0x7B7B7B, 0xA9FF9F, 0x706DEB, 0xB2B2B2,
};
// u64_config.cc; weak because the updater application links this file without it
extern void u64_get_palette_rgb(uint8_t rgb[16][3]) __attribute__((weak));
static uint16_t pal[16];      // RGB565
static uint16_t dim[16];      // the mixed still colours, RGB565
static int dim_bg = -1;       // the background and weight dim was made for
static int dim_t = -1;
static volatile int look_t = 1;   // Overlay Transparency: still weight t / 4
static volatile int look_bg = 6;  // the menu's background colour, as last set

static inline uint16_t rgb565(uint32_t c)
{
    return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
}

extern uint8_t _default_chars_bin_start[];

typedef struct {             // no padding: compared with memcmp
    uint8_t d011, d016, d018, d021, d022, d023, d024;
    uint8_t chr_rom;
    uint16_t scr, chr;      // C64 addresses
} vic_sig_t;

typedef struct {
    uint16_t *px;           // 16-byte aligned, in raw
    uint8_t *raw;           // as malloc gave it
    uint32_t bg_gen;        // still_gen of the still drawn, 0 none
    vic_sig_t sig;
    uint32_t chars_gen;
    bool cells_valid;
    uint8_t scr[NCELLS], col[NCELLS];
} layer_buf_t;

static bool usable;
static bool hw_text;        // the bitstream draws the text (TX_CTRL)
static bool slim;           // the slim core: no freeze session, the text layer's view is TX_VIEW
static bool has_list;       // the bitstream has the commit list (CAPS2 b3: PROTO)
static volatile bool menu_shown; // between trx_video_menu_on() and _off()
static uint8_t *still;      // 16-byte aligned, two slots
static uint8_t *still_copy; // this freeze's still (STILL_FRAME bytes)
static bool still_ok;       // still_copy holds a frame
static bool still_60;       // ... of a 60 Hz mode: 400 x 240 rows, else 400 x 270
static uint32_t still_gen;  // changes with the copy or the colours it is drawn in

// Takes the app's current palette (config, .vpl or UCI may have changed it)
// into c_palette and pal; the mixed still colours are made again on next use.
static void refresh_palette(void)
{
    uint8_t rgb[16][3];
    if (!u64_get_palette_rgb) {
        return;
    }
    u64_get_palette_rgb(rgb);
    bool changed = false;
    for (int i = 0; i < 16; i++) {
        uint32_t c = (uint32_t(rgb[i][0]) << 16) | (uint32_t(rgb[i][1]) << 8) | rgb[i][2];
        if (c != c_palette[i]) {
            c_palette[i] = c;
            pal[i] = rgb565(c);
            changed = true;
        }
    }
    if (changed) {
        dim_bg = -1;                    // make_dim runs again (draw)
        if (++still_gen == 0) {
            still_gen = 1;
        }
    }
}
static layer_buf_t bufs[2];
static int nbufs;           // 0: not allocated (yet), 1 or 2
static int buf_bytes;       // the size of each, enough for the plan

// The picture's place (trx_hdmi.h): the row and scale it was taken for, the layer's plan, and per layer column / line the
// VIC pixel / line of the window (lut_vx, lut_vy: in the still's rows) and the C64 text pixel / line (lut_tx, lut_ty, -1
// outside the text area); tx_first / ty_first: the first layer column / line of every text column / line, [320] / [200]
// the first after the text area.
static int geom_row = -1, geom_scale = -1;
static const trx_hdmi_geom_t *geom;
static trx_layer_plan_t plan;
static int16_t *lut_vx, *lut_tx, *lut_vy, *lut_ty, *tx_first, *ty_first;
#define LUT_ENTRIES    (2 * TRX_LAYER_MAX_W + 2 * TRX_LAYER_MAX_H + (TRX_HDMI_TEXT_W + 1) + (TRX_HDMI_TEXT_H + 1))
static int front;
static bool layer_on;
static volatile bool active;
static bool seen_session;
static int passes;
static bool wide_hw;        // the 80 column monitor has the layers off (trx_video_wide_begin)
static uint32_t wide_tx;    // TX_CTRL as it was
static volatile bool tw_on; // Timewarp's bar is up: no menu text, the still at full brightness

static vic_sig_t cur_sig;
static uint8_t cur_scr[NCELLS], cur_col[NCELLS];
static uint8_t chars[2048];
static uint32_t chars_gen;
static uint16_t chars_from = 0xFFFF;
static bool chars_ok;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static SemaphoreHandle_t flip_sem;
static SemaphoreHandle_t vsync_sem;

static inline uint8_t c64_peek(uint32_t addr)
{
    return *((volatile uint8_t *)(C64_MEMORY_BASE + (addr & 0xFFFF)));
}

static void *alloc_aligned(size_t size)
{
    uint8_t *p = (uint8_t *)malloc(size + 15);
    if (!p) {
        return NULL;
    }
    return (void *)(((uint32_t)p + 15) & ~15UL);
}

// ---- reading the C64 text screen ------------------------------------------------

// What the window shows of the VIC's memory: RAM $0000-$0FFF, the char ROM.
static bool read_chars(uint16_t chr, bool rom)
{
    uint8_t tmp[2048];
    const uint8_t *src = tmp;
    if (rom) {
        src = _default_chars_bin_start + (chr & 0x0800);
    } else if (chr + 2048 <= 0x1000) {
        for (int i = 0; i < 2048; i++) {
            tmp[i] = c64_peek(chr + i);
        }
    } else {
        return false;
    }
    if (chr != chars_from || memcmp(chars, src, 2048)) {
        memcpy(chars, src, 2048);
        chars_from = chr;
        chars_gen++;
    }
    return true;
}

// Reads registers, screen and colour RAM. False when the window does not show
// them now (the app's DMA mode is not the menu's) or the screen is not in it.
static bool read_screen(void)
{
    uint8_t mode = C64_MODE;
    if (!(mode & C64_MODE_ULTIMAX) || C64_DMA_MEMONLY) {
        return false; // a dma_transfer_frozen chunk of the app's: try again later
    }
    vic_sig_t s;
    memset(&s, 0, sizeof(s));
    s.d011 = VIC_REG(0x11);
    s.d016 = VIC_REG(0x16);
    s.d018 = VIC_REG(0x18);
    s.d021 = VIC_REG(0x21) & 15;
    s.d022 = VIC_REG(0x22) & 15;
    s.d023 = VIC_REG(0x23) & 15;
    s.d024 = VIC_REG(0x24) & 15;
    // VIC bank from the CIA 2 pins: PRA | ~DDRA (T12 §4.1)
    uint8_t pins = CIA2_DPA | ~CIA2_DDRA;
    uint16_t bank = (3 - (pins & 3)) * 0x4000;
    s.scr = bank + ((s.d018 & 0xF0) << 6);
    s.chr = bank + ((s.d018 & 0x0E) << 10);
    s.chr_rom = ((bank & 0x4000) == 0) && ((s.chr & 0x3000) == 0x1000);
    if (s.scr + NCELLS > 0x1000) {
        return false;
    }
    for (int i = 0; i < NCELLS; i++) {
        cur_scr[i] = c64_peek(s.scr + i);
    }
    for (int i = 0; i < NCELLS; i++) {
        cur_col[i] = c64_peek(0xD800 + i) & 15;
    }
    if (C64_MODE != mode || C64_DMA_MEMONLY) {
        return false;
    }
    bool sig_changed = memcmp(&s, &cur_sig, sizeof(s)) != 0;
    memcpy(&cur_sig, &s, sizeof(s));
    if (sig_changed || !chars_ok || (passes % CHARS_EVERY) == 0) {
        chars_ok = read_chars(s.chr, s.chr_rom);
    }
    return true;
}

// ---- drawing --------------------------------------------------------------------

// The row and scale the picture runs in: mode_req.v's read-back (U16), or, on a bitstream before U16, 1080p at the scale it
// always had (Fit) in the family of the still.
static void read_mode(int *row, int *scale)
{
    if (trx_io_hdmi_modes()) {
        *row = ioRead8(TRX_MR_ROW) & 15;
        *scale = ioRead8(TRX_MR_SCALE_USED) & 3;
    } else {
        *row = still_60 ? 8 : 2;
        *scale = 1;
    }
}

// The first index of v (n entries) from `from` on whose value is at least t: the values ascend inside the text area and are
// -1 outside it, and every text column / line has a layer column / line (the host test, software/test/trx_hdmi).
static int first_at_least(const int16_t *v, int from, int n, int t)
{
    while (from < n && v[from] < t) {
        from++;
    }
    return from;
}

// Takes the place of the picture for this row and scale: the plan and the lookup tables. A pair the table does not have
// (it cannot come from mode_req.v) takes 1080p at Fit.
static void set_geometry(int row, int scale)
{
    const trx_hdmi_geom_t *g = trx_hdmi_geom(row, scale);
    if (!g) {
        g = trx_hdmi_geom(still_60 ? 8 : 2, 1);
    }
    geom = g;
    geom_row = row;
    geom_scale = scale;
    plan = trx_hdmi_plan(g);
    int rows = still_60 ? WIN_H_60 : WIN_H; // the still may be of the other family while a System Mode switch settles
    for (int i = 0; i < plan.w; i++) {
        lut_vx[i] = trx_hdmi_vic_x(g, &plan, i);
        lut_tx[i] = trx_hdmi_text_x(g, &plan, i);
    }
    for (int j = 0; j < plan.h; j++) {
        int v = trx_hdmi_vic_y(g, &plan, j);
        lut_vy[j] = (v < rows) ? v : rows - 1;
        lut_ty[j] = trx_hdmi_text_y(g, &plan, j);
    }
    int i = first_at_least(lut_tx, 0, plan.w, 0);
    for (int t = 0; t < TRX_HDMI_TEXT_W; t++) {
        i = first_at_least(lut_tx, i, plan.w, t);
        tx_first[t] = i;
    }
    while (i < plan.w && lut_tx[i] >= 0) {
        i++;
    }
    tx_first[TRX_HDMI_TEXT_W] = i;
    int j = first_at_least(lut_ty, 0, plan.h, 0);
    for (int t = 0; t < TRX_HDMI_TEXT_H; t++) {
        j = first_at_least(lut_ty, j, plan.h, t);
        ty_first[t] = j;
    }
    while (j < plan.h && lut_ty[j] >= 0) {
        j++;
    }
    ty_first[TRX_HDMI_TEXT_H] = j;
}

static inline uint32_t px_ctrl_on(void)
{
    return PX_CTRL_FMT | (uint32_t(plan.s) << 4) | 1;
}

// The rvlite core has no data cache, but its stores are posted: a load from the
// last address written waits for them before the register write that follows.
static inline void buf_done(layer_buf_t *b)
{
    __asm__ volatile ("fence" ::: "memory");
    (void)*(volatile uint32_t *)(b->px + (plan.stride / 2) * plan.h - 2);
}

// The still's colours: each palette colour mixed with the background bg,
// t / 4 of the still (t = 0: the background only).
static void make_dim(int bg, int t)
{
    uint32_t b = c_palette[bg & 15];
    for (int i = 0; i < 16; i++) {
        uint32_t s = c_palette[i], c = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t sc = (s >> sh) & 255, bc = (b >> sh) & 255;
            c |= ((sc * t + bc * (4 - t)) / 4) << sh;
        }
        dim[i] = rgb565(c);
    }
    dim_bg = bg & 15;
    dim_t = t;
}

// The last complete frame of the still capture into still_copy, while the
// machine runs: the capture writes the other slot meanwhile, and the slot read
// here only once a further frame has completed. A copy over which the frame
// count moved is taken again.
static void grab_still(void)
{
    still_ok = false;
    for (int tries = 0; tries < 3; tries++) {
        uint32_t st = trx_io_read32(STILL_STATUS);
        if (!(st & 1)) {
            return;
        }
        still_60 = st & 4;
        memcpy(still_copy, still + ((st & 2) ? STILL_SLOT : 0), still_60 ? STILL_FRAME_60 : STILL_FRAME);
        still_ok = true;
        if ((trx_io_read32(STILL_STATUS) >> 8) == (st >> 8)) {
            return;
        }
    }
}

// The still's colour index under layer pixel (i, j): the VIC pixel the scaler shows there (lut_vx, lut_vy); two VIC
// pixels per still byte, the first in the low nibble.
static inline int still_index(int i, int j)
{
    uint32_t k = WIN_W * lut_vy[j] + lut_vx[i];
    uint8_t b = still_copy[k >> 1];
    return (k & 1) ? (b >> 4) : (b & 15);
}

static inline uint16_t still_px(int i, int j)
{
    return still_ok ? dim[still_index(i, j)] : dim[0];
}

// The whole layer; a line that shows the same VIC line as the one above is a copy of it.
static void draw_still(uint16_t *px)
{
    const int sw = plan.stride / 2;
    int last = -1;
    for (int j = 0; j < plan.h; j++) {
        uint16_t *p = px + j * sw;
        int s = still_ok ? lut_vy[j] : 0;
        if (s == last) {
            memcpy(p, p - sw, plan.w * 2);
            continue;
        }
        last = s;
        if (!still_ok) {
            for (int i = 0; i < plan.w; i++) {
                p[i] = dim[0];
            }
            continue;
        }
        const uint8_t *row = still_copy + (WIN_W / 2) * s;
        for (int i = 0; i < plan.w; i++) {
            int x = lut_vx[i];
            uint8_t v = row[x >> 1];
            p[i] = dim[(x & 1) ? (v >> 4) : (v & 15)];
        }
    }
}

// Waits for the next frame start (PX_STATUS frames, vic_scaler cy = 0).
static bool wait_frame_start(void)
{
    xSemaphoreTake(vsync_sem, 0); // drop a stale give
    ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_VSYNC);
    trx_irq_enable(TRX_IRQ_VSYNC, true);
    bool ok = xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(100)) == pdTRUE;
    trx_irq_enable(TRX_IRQ_VSYNC, false);
    return ok;
}

// One cell, by the rules of TRX64 render.rs (standard, multicolour and ECM
// text; other modes show the still). Colour -1 = background 0: the still.
// The cell's pixels are where c64_text.v draws them (lut_tx, lut_ty).
static void draw_cell(uint16_t *px, int cell)
{
    const vic_sig_t &v = cur_sig;
    int cx = 8 * (cell % TEXT_COLS);
    int cy = 8 * (cell / TEXT_COLS);
    uint8_t code = cur_scr[cell];
    uint8_t col = cur_col[cell];
    bool ecm = v.d011 & 0x40, bmm = v.d011 & 0x20, mcm = v.d016 & 0x10;
    bool den = v.d011 & 0x10;
    int bg = -1;
    int mode; // 0 hires, 1 multicolour, 2 none
    if (!den || bmm || !chars_ok || (ecm && mcm)) {
        mode = 2;
    } else if (ecm) {
        int sel = code >> 6;
        uint8_t regs[4] = { v.d021, v.d022, v.d023, v.d024 };
        bg = sel ? regs[sel] : -1;
        code &= 63;
        mode = 0;
    } else if (mcm && (col & 8)) {
        mode = 1;
    } else {
        mode = 0;
    }
    const int sw = plan.stride / 2;
    const int i0 = tx_first[cx], i1 = tx_first[cx + 8];
    const int j0 = ty_first[cy], j1 = ty_first[cy + 8];
    for (int j = j0; j < j1; j++) {
        uint8_t bits = (mode == 2) ? 0 : chars[code * 8 + (lut_ty[j] - cy)];
        uint16_t *p = px + j * sw;
        for (int i = i0; i < i1; i++) {
            int b = lut_tx[i] - cx; // 0..7
            int c;
            if (mode == 1) {
                switch ((bits >> (6 - (b & 6))) & 3) {
                case 1:  c = v.d022; break;
                case 2:  c = v.d023; break;
                case 3:  c = col & 7; break;
                default: c = -1; break;
                }
            } else {
                c = (bits & (0x80 >> b)) ? col : bg;
            }
            p[i] = (c < 0) ? still_px(i, j) : pal[c];
        }
    }
}

// Brings buffer b to the current screen. True when it changed.
static bool draw(layer_buf_t *b)
{
    bool all = false, any = false;
    if (cur_sig.d021 != dim_bg) {
        // the menu's background is not the one the still was mixed with
        make_dim(cur_sig.d021, dim_t);
        still_gen++;
    }
    if (b->bg_gen != still_gen) {
        draw_still(b->px);
        b->bg_gen = still_gen;
        all = true;
    }
    if (!b->cells_valid || b->chars_gen != chars_gen || memcmp(&b->sig, &cur_sig, sizeof(cur_sig))) {
        all = true;
    }
    for (int i = 0; i < NCELLS; i++) {
        if (all || b->scr[i] != cur_scr[i] || b->col[i] != cur_col[i]) {
            draw_cell(b->px, i);
            any = true;
        }
    }
    memcpy(b->scr, cur_scr, NCELLS);
    memcpy(b->col, cur_col, NCELLS);
    memcpy(&b->sig, &cur_sig, sizeof(cur_sig));
    b->chars_gen = chars_gen;
    b->cells_valid = true;
    buf_done(b);
    return any || all;
}

static void show_first(layer_buf_t *b)
{
    trx_io_write32(PX_CTRL, PX_CTRL_OFF);
    trx_io_write32(PX_SIZE, (uint32_t(plan.h) << 16) | uint32_t(plan.w));
    trx_io_write32(PX_STRIDE, uint32_t(plan.stride));
    trx_io_write32(PX_POS, (uint32_t(plan.y0) << 16) | uint32_t(plan.x0)); // from the active origin (pix_layer.v)
    trx_io_write32(PX_GALPHA, 255);
    trx_io_write32(PX_KEY, 0); // key off: every pixel opaque (header)
    trx_io_write32(PX_FRONT, (uint32_t)b->px);
    trx_io_write32(PX_CTRL, px_ctrl_on());
    layer_on = true;
}

static void flip_to(int n)
{
    xSemaphoreTake(flip_sem, 0); // drop a stale give
    ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_FLIP);
    trx_io_write32(PX_BACK, (uint32_t)bufs[n].px);
    if (xSemaphoreTake(flip_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        printf("[TRX] pixel layer: no flip\n");
    }
    front = n;
}

// ---- Timewarp's bar (trx_video.h; TRX64-Ultimate docs/specs/T20-delta-ring.md §16.7)
// In layer pixels, laid out for the 800 x 540 layer of 1080p (2 x 2 output pixels per VIC pixel: the lower border, window
// lines 235-269, is layer lines 470-539, and the bar sits in it) and scaled to the layer's width and height for every other
// row and scale (U16): the large text at 2 (16 pixels) on a layer at least 720 wide, else at 1.
#define TW_BAR_RGB     0x101018

typedef struct {
    int big;                // the large text's scale
    int x, y, w, h;         // the bar
    int glyph_x, pos_x, info_x, hint_end;
    int row_y, small_y;     // the large row (glyph, position) and the small texts, centred on it
    int track_x, track_w, track_y;
} tw_layout_t;

static tw_layout_t tw_layout(void)
{
    tw_layout_t l;
    const int w = plan.w, h = plan.h;
    l.big = (w >= 720) ? 2 : 1;
    l.h = 16 + 16 * l.big;              // 48 (1080p) / 32
    l.x = 16 * w / 800;
    l.w = w - 2 * l.x;                  // 768
    l.y = h - l.h - 16 * h / 540;       // 476
    l.row_y = l.y + 4 * l.big;          // 484
    l.small_y = l.row_y + 4 * (l.big - 1); // 488
    l.glyph_x = l.x + 12 * w / 800;     // 28
    l.pos_x = l.x + 36 * w / 800;       // 52
    l.info_x = l.x + 172 * w / 800;     // 188
    l.hint_end = l.x + l.w - 12 * w / 800; // 772
    l.track_x = l.glyph_x;
    l.track_w = l.hint_end - l.glyph_x; // 744
    l.track_y = l.y + 8 + 12 * l.big;   // 508
    return l;
}

static uint16_t barmix[16];    // a palette colour under the bar: 1/4 still, 3/4 bar

// 8 x 8, bit 7 left: pause, back, forward, oldest, newest (TW_GLYPH_*)
static const uint8_t tw_glyphs[5][8] = {
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00 },
    { 0x11, 0x33, 0x77, 0xFF, 0x77, 0x33, 0x11, 0x00 },
    { 0x88, 0xCC, 0xEE, 0xFF, 0xEE, 0xCC, 0x88, 0x00 },
    { 0x83, 0x87, 0x8F, 0x9F, 0x8F, 0x87, 0x83, 0x00 },
    { 0xC1, 0xE1, 0xF1, 0xF9, 0xF1, 0xE1, 0xC1, 0x00 },
};

static void tw_fill(uint16_t *px, int x, int y, int w, int h, uint16_t c)
{
    for (int r = 0; r < h; r++) {
        uint16_t *p = px + (y + r) * (plan.stride / 2) + x;
        for (int k = 0; k < w; k++) {
            p[k] = c;
        }
    }
}

static void tw_glyph(uint16_t *px, int x, int y, const uint8_t *g, int scale, uint16_t c)
{
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            if (g[j] & (0x80 >> i)) {
                tw_fill(px, x + i * scale, y + j * scale, scale, scale, c);
            }
        }
    }
}

// ASCII to a screen code of the char ROM's lower-case set
static uint8_t tw_scode(char c)
{
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 65;
    }
    if (c >= 32 && c < 64) {
        return c;
    }
    return c == '@' ? 0 : 32;
}

static int tw_text(uint16_t *px, const tw_layout_t &l, int x, int y, const char *s, int scale, uint16_t c)
{
    const uint8_t *font = _default_chars_bin_start + 0x800;
    for (; s && *s && x + 8 * scale <= l.x + l.w; s++) {
        tw_glyph(px, x, y, font + 8 * tw_scode(*s), scale, c);
        x += 8 * scale;
    }
    return x;
}

static void tw_draw_bar(uint16_t *px, const trx_tw_bar_t *b)
{
    const tw_layout_t l = tw_layout();
    const int sw = plan.stride / 2;
    // the background: the still under it, mixed
    for (int y = l.y; y < l.y + l.h; y++) {
        uint16_t *p = px + y * sw;
        for (int x = l.x; x < l.x + l.w; x++) {
            p[x] = still_ok ? barmix[still_index(x, y)] : barmix[0];
        }
    }
    int g = (b->glyph < 0 || b->glyph > 4) ? 0 : b->glyph;
    tw_glyph(px, l.glyph_x, l.row_y, tw_glyphs[g], l.big, pal[14]);
    tw_text(px, l, l.pos_x, l.row_y, b->pos, l.big, pal[1]);
    tw_text(px, l, l.info_x, l.small_y, b->info, 1, b->warn ? pal[10] : pal[15]);
    if (b->hint) {
        int w = 8 * strlen(b->hint);
        tw_text(px, l, (w < l.hint_end - l.info_x) ? l.hint_end - w : l.info_x, l.small_y, b->hint, 1, pal[15]);
    }
    if (b->track_len > 0) {
        int len = b->track_len > 1 ? b->track_len - 1 : 1;
        int pos = b->track_pos < 0 ? 0 : (b->track_pos > len ? len : b->track_pos);
        int cx = l.track_x + (pos * (l.track_w - 4)) / len;
        tw_fill(px, l.track_x, l.track_y, cx - l.track_x, 4, pal[12]);
        tw_fill(px, cx, l.track_y, l.track_x + l.track_w - cx, 4, pal[11]);
        tw_fill(px, cx, l.track_y - 3, 4, 10, pal[1]);
    }
}

bool trx_video_tw_begin(void)
{
    if (!usable) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = active && layer_on && !wide_hw;
    if (ok) {
        tw_on = true;
        if (hw_text) {
            trx_io_write32(TX_CTRL, 0);   // the box goes (T12 §11 Q3: the app's show bit)
        }
        refresh_palette();
        make_dim(dim_bg < 0 ? look_bg : dim_bg, 4);     // t = 4/4: the still undimmed
        if (++still_gen == 0) {
            still_gen = 1;
        }
        uint32_t bar = TW_BAR_RGB;
        for (int i = 0; i < 16; i++) {
            uint32_t s = c_palette[i], c = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t sc = (s >> sh) & 255, bc = (bar >> sh) & 255;
                c |= ((sc + 3 * bc) / 4) << sh;
            }
            barmix[i] = rgb565(c);
        }
    }
    xSemaphoreGive(lock);
    return ok;
}

void trx_video_tw_still(void)
{
    if (!usable) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tw_on) {
        grab_still();
        if (++still_gen == 0) {
            still_gen = 1;
        }
    }
    xSemaphoreGive(lock);
}

void trx_video_tw_bar(const trx_tw_bar_t *bar)
{
    if (!usable) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tw_on && nbufs && layer_on) {
        int n = (nbufs == 2) ? 1 - front : front;
        layer_buf_t *b = &bufs[n];
        if (b->bg_gen != still_gen) {
            draw_still(b->px);
            b->bg_gen = still_gen;
            b->cells_valid = false;
        }
        tw_draw_bar(b->px, bar);
        buf_done(b);
        if (nbufs == 2) {
            flip_to(n);
        }
    }
    xSemaphoreGive(lock);
}

// SH_CTRL PROTO on or off, SH_MODE kept (CAPS2 b3 only).
static void set_proto(bool on)
{
    if (has_list) {
        uint32_t mode = trx_io_read32(SH_CTRL) & SH_MODE_MASK;
        trx_io_write32(SH_CTRL, mode | (on ? SH_PROTO : 0));
    }
}

// With the lock held.
static void teardown(void)
{
    active = false;
    tw_on = false;
    wide_hw = false;
    menu_shown = false;
    if (hw_text) {
        trx_io_write32(TX_CTRL, 0);
    }
    set_proto(false);
    trx_io_write32(PX_CTRL, PX_CTRL_OFF);
    trx_io_write32(GATE_CTRL, 0);
    layer_on = false;
}

// The menu owns the screen: from the end of freeze() (init_io done) to the
// start of unfreeze() (before restore_io), and the app is not inside one of
// its DMA-mode windows, which change what the window decodes.
static bool menu_up(void)
{
    C64 *c64 = C64::getMachine();
    return c64 && c64->keyboard_scan_allowed() && !c64->keyboard_scan_deferred();
}

// The running row or scale changed while the freeze is up (the menu's HDMI items, U16): the layer off, two frame starts so
// that it reads the buffers no more, the geometry and, if the new place needs more, the buffers for it, the still drawn
// again and shown. The text follows: the hardware's by itself, the task's cells and Timewarp's bar on their next pass.
static bool alloc_layer(void);
static void relayout(int row, int scale)
{
    trx_io_write32(PX_CTRL, PX_CTRL_OFF);
    layer_on = false;
    wait_frame_start();
    wait_frame_start();
    set_geometry(row, scale);
    if (!alloc_layer()) {
        return;
    }
    for (int i = 0; i < nbufs; i++) {
        bufs[i].bg_gen = 0;
        bufs[i].cells_valid = false;
    }
    int n = front;
    draw_still(bufs[n].px);
    bufs[n].bg_gen = still_gen;
    buf_done(&bufs[n]);
    show_first(&bufs[n]);
    printf("[TRX] freeze still: row %d scale %d, %d x %d at scale %d from (%d, %d)\n", row, scale, plan.w, plan.h, plan.s,
           plan.x0, plan.y0);
}

// One look, with the lock held.
static void step(void)
{
    // A freeze session on the cores that have one; on the slim core the freeze is the machine pause.
    uint32_t st = slim ? 0 : trx_io_read32(SH_STATUS);
    bool session = slim ? (trx_io_read32(GATE_CTRL) & GATE_HALTED) != 0 : (st & SH_SESSION) != 0;
    int count = st >> 16;
    if (!session) {
        // The machine runs again without an unfreeze (start_cartridge ends the
        // freeze that way): put the layer and the gate away. A debugger run to
        // a breakpoint with the menu up stays frozen and keeps them.
        C64 *c64 = C64::getMachine();
        if (seen_session && !(trx_io_read32(GATE_CTRL) & GATE_HALTED) && !(c64 && c64->is_frozen())) {
            teardown();
        }
        return;
    }
    seen_session = true;
    if (layer_on && !wide_hw && nbufs) {
        int row, scale;
        read_mode(&row, &scale);
        if (row != geom_row || scale != geom_scale) {
            relayout(row, scale);
        }
    }
    if (!nbufs) {
        return; // no layer this freeze: a larger place found no memory (relayout)
    }
    if (hw_text || tw_on || !menu_shown || !menu_up()) {
        return; // the hardware draws it, Timewarp hides it; or before the menu, or the restore after it
    }
    passes++;
    if (!read_screen()) {
        return;
    }
    // Still the same session with the menu up after the last read: then every
    // read came before unfreeze() started, so none of them saw the program's bytes.
    uint32_t st2 = trx_io_read32(SH_STATUS);
    if (!(st2 & SH_SESSION) || int(st2 >> 16) != count || !menu_shown || !menu_up()) {
        return;
    }
    if (!layer_on) {
        int n = (nbufs == 2) ? 1 - front : front;
        draw(&bufs[n]);
        front = n;
        show_first(&bufs[n]);
        return;
    }
    if (nbufs == 1) {
        draw(&bufs[0]);
        return;
    }
    int back = 1 - front;
    if (draw(&bufs[back])) {
        flip_to(back);
    }
}

static void render_task(void *)
{
    for (;;) {
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        if (active) {
            refresh_palette();
            step();
        }
        xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(RENDER_MS));
    }
}

static uint8_t video_irq(uint8_t act)
{
    BaseType_t woken = pdFALSE;
    ioWrite8(TRX_IRQ_CAUSE, act);
    if (act & TRX_IRQ_FLIP) {
        xSemaphoreGiveFromISR(flip_sem, &woken);
    }
    if (act & TRX_IRQ_VSYNC) {
        xSemaphoreGiveFromISR(vsync_sem, &woken);
    }
    return woken == pdTRUE;
}

// The two buffers, as large as the plan needs, kept (a monitor step unfreezes and freezes again); a place that needs more
// (U16: another row or scale) replaces them, with the layer off. 1080p at Fit needs 864,000 bytes each, as before U16; the
// largest, 1080p Fill, 1,036,800 (TRX_LAYER_MAX_BYTES).
static bool alloc_layer(void)
{
    int need = plan.stride * plan.h;
    if (nbufs && buf_bytes >= need) {
        return true;
    }
    for (int i = 0; i < nbufs; i++) {
        free(bufs[i].raw);
        bufs[i].raw = NULL;
        bufs[i].px = NULL;
    }
    nbufs = 0;
    buf_bytes = 0;
    front = 0;
    for (int i = 0; i < 2; i++) {
        uint8_t *raw = (uint8_t *)malloc(need + 15);
        if (!raw) {
            break;
        }
        bufs[i].raw = raw;
        bufs[i].px = (uint16_t *)(((uint32_t)raw + 15) & ~15UL);
        bufs[i].bg_gen = 0;
        bufs[i].cells_valid = false;
        nbufs++;
    }
    if (nbufs) {
        buf_bytes = need;
    }
    printf("[TRX] pixel layer: %d buffer(s) of %d bytes (%d x %d at scale %d), heap free %u\n", nbufs, need, plan.w, plan.h,
           plan.s, (unsigned)xPortGetFreeHeapSize());
    return nbufs > 0;
}

static void init(void *, void *)
{
    uint8_t need = TRX_CAPS_GATE | TRX_CAPS_FREEZE | TRX_CAPS_VIDEO | TRX_CAPS_PX565;
    if ((trx_io_caps() & need) != need) {
        return;
    }
    // Still capture into our own memory: the slots are read while the machine
    // is paused, and capture runs whenever it is not.
    still = (uint8_t *)alloc_aligned(STILL_BYTES);
    still_copy = (uint8_t *)malloc(STILL_FRAME);
    int16_t *luts = (int16_t *)malloc(LUT_ENTRIES * sizeof(int16_t));
    lock = xSemaphoreCreateMutex();
    flip_sem = xSemaphoreCreateBinary();
    vsync_sem = xSemaphoreCreateBinary();
    if (luts) {
        lut_vx = luts;
        lut_tx = lut_vx + TRX_LAYER_MAX_W;
        lut_vy = lut_tx + TRX_LAYER_MAX_W;
        lut_ty = lut_vy + TRX_LAYER_MAX_H;
        tx_first = lut_ty + TRX_LAYER_MAX_H;
        ty_first = tx_first + TRX_HDMI_TEXT_W + 1;
    }
    if (!still || !still_copy || !luts || !lock || !flip_sem || !vsync_sem) {
        printf("[TRX] pixel layer: out of memory\n");
        return;
    }
    memset(still, 0, STILL_BYTES);
    refresh_palette();
    for (int i = 0; i < 16; i++) {
        pal[i] = rgb565(c_palette[i]);
    }
    trx_io_write32(PX_CTRL, PX_CTRL_OFF);
    trx_io_write32(STILL_BASE, (uint32_t)still);
    trx_io_write32(STILL_CTRL, 1);
    hw_text = (trx_io_read32(TX_CTRL) >> 24) >= 1;
    slim = trx_io_slim();
    trx_io_write32(TX_CTRL, 0);
    has_list = (ioRead8(TRX_CAPS2) & TRX_CAPS2_SESSION) != 0;
    trx_irq_attach(TRX_IRQ_FLIP | TRX_IRQ_VSYNC, video_irq); // VSYNC enabled only while waited for
    trx_irq_enable(TRX_IRQ_FLIP, true);
    if (xTaskCreate(render_task, "TRX Video", 1024, NULL, tskIDLE_PRIORITY + 1, &task_handle) != pdPASS) {
        return;
    }
    usable = true;
    printf("[TRX] freeze menu on the pixel layer: still at %p, text in %s%s%s\n", still,
           hw_text ? "hardware" : "software", has_list ? ", commit list" : "", slim ? ", slim core" : "");
}

InitFunction trx_video_init("TRX Video", init, NULL, NULL, 90);

bool trx_video_has_layer(void)
{
    uint8_t need = TRX_CAPS_GATE | TRX_CAPS_FREEZE | TRX_CAPS_VIDEO | TRX_CAPS_PX565;
    return (trx_io_caps() & need) == need;
}

void trx_video_set_look(int transparency, int background)
{
    look_t = (transparency < 0) ? 0 : (transparency > 3) ? 3 : transparency;
    look_bg = background & 15;
}

void trx_video_freeze_enter(void)
{
    trx_lv_yield_layer(); // one owner of the layer: the freeze takes it from the LVGL UI (T24)
    if (!usable || active) {
        return;
    }
    if (!(U64_HDMI_REG & U64_HDMI_HPD_CURRENT)) {
        return; // nobody would see the layer: keep the menu on the VIC
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    // The picture before the stop (header): the still on the layer, from a
    // frame start on, while the machine still runs, where the scaler shows it.
    make_dim(look_bg, look_t);
    grab_still();
    int row, scale;
    read_mode(&row, &scale);
    set_geometry(row, scale);
    if (!alloc_layer()) {
        xSemaphoreGive(lock);
        return;
    }
    seen_session = false;
    passes = 0;
    tw_on = false;
    wide_hw = false;
    chars_ok = false;
    for (int i = 0; i < nbufs; i++) {
        bufs[i].bg_gen = 0;
        bufs[i].cells_valid = false;
    }
    if (++still_gen == 0) {
        still_gen = 1;
    }
    int n = (nbufs == 2) ? 1 - front : front;
    draw_still(bufs[n].px);
    bufs[n].bg_gen = still_gen;
    buf_done(&bufs[n]);
    wait_frame_start();          // switch on in the vertical blank
    show_first(&bufs[n]);
    front = n;
    flip_to(n);                  // and from the next frame start on every line is the layer
    menu_shown = false;
    set_proto(true);             // stop(), backup_io(), init_io() follow: Gideon's protocol (not on the slim core)
    active = true;
    trx_io_write32(GATE_CTRL, GATE_FRZ);
    xSemaphoreGive(lock);
    xTaskNotifyGive(task_handle);
}

void trx_video_menu_on(void)
{
    if (!usable || !active) {
        return;
    }
    set_proto(false);            // the menu's writes and pokes from here on are listed
    menu_shown = true;
    if (slim) {
        trx_io_write32(TX_VIEW, TX_VIEW_MENU | (uint32_t(look_bg & 15) << 2));
    }
    if (hw_text && !tw_on) {
        trx_io_write32(TX_CTRL, TX_SHOW);
    }
}

bool trx_video_wide_begin(void)
{
    if (!usable) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = active && hw_text && layer_on && menu_shown && !tw_on && !wide_hw;
    if (ok) {
        wide_tx = trx_io_read32(TX_CTRL) & 0xFF;
        trx_io_write32(TX_CTRL, 0);
        trx_io_write32(PX_CTRL, PX_CTRL_OFF);
        wide_hw = true;
    }
    xSemaphoreGive(lock);
    return ok;
}

void trx_video_wide_end(void)
{
    if (!usable) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (wide_hw) {
        wide_hw = false;
        if (active && layer_on && menu_shown) {
            trx_io_write32(PX_CTRL, px_ctrl_on());
            trx_io_write32(TX_CTRL, wide_tx);
        }
    }
    xSemaphoreGive(lock);
}

bool trx_video_slim_active(void)
{
    return usable && active && slim && hw_text;
}

void trx_video_menu_view(void)
{
    if (!usable || !slim || !menu_shown) {
        return;
    }
    uint32_t want = TX_VIEW_MENU | (uint32_t(look_bg & 15) << 2);
    uint32_t now = trx_io_read32(TX_VIEW) & 0xFFFF;
    if (now != want) {
        printf("[TRX] menu view was %04x, written back as %04x\n", (unsigned)now, (unsigned)want);
        trx_io_write32(TX_VIEW, want);
    }
}

void trx_video_menu_background(int background)
{
    look_bg = background & 15;
    if (usable && slim && menu_shown) {
        trx_io_write32(TX_VIEW, TX_VIEW_MENU | (uint32_t(look_bg & 15) << 2));
    }
}

void trx_video_menu_off(void)
{
    if (!usable) {
        return;
    }
    menu_shown = false;
    if (wide_hw) {
        wide_hw = false; // the freeze ends under the monitor: the pixel layer goes back, the text is off below
        if (active && layer_on) {
            trx_io_write32(PX_CTRL, px_ctrl_on());
        }
    }
    if (hw_text) {
        trx_io_write32(TX_CTRL, 0);
    }
    if (active) {
        set_proto(true);         // the restore and resume() follow: Gideon's protocol
    }
}

void trx_video_freeze_leave(void)
{
    if (!usable || !active) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (active) {
        if (layer_on) {
            // The window stays black until c64_core resumes the VIC after whole
            // frames: keep the layer until then, and for one whole frame of the
            // running machine after it, so the ring of the scaler holds only new
            // lines; switch off at a frame start.
            for (int i = 0; i < 40 && (trx_io_read32(GATE_CTRL) & GATE_HALTED); i++) {
                vTaskDelay(1);
            }
            wait_frame_start();
            wait_frame_start();
        }
        teardown();
    }
    xSemaphoreGive(lock);
}
