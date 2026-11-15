// The TRX64 Ultimate's own LVGL UI on the pixel layer (TRX64-Ultimate
// docs/specs/T24-lvgl-ui.md). Phase 1: the boot splash.
//
// At boot, on a bitstream with the RGB565 pixel layer (trx_lv_port.h), a UI task
// shows the splash opaque over the whole picture while the C64 starts behind
// it. 2 s after it appeared, or once the C64 waits at BASIC's READY, whichever
// is later (at most 4 s), the splash fades out over 300 ms through the layer's
// global alpha and the UI shuts down: the layer is off and LVGL's memory (two
// frame buffers of 1,036,800 bytes) is free again for the freeze menu
// (trx_video.cc). A freeze in the meantime takes the layer at once
// (trx_lv_yield_layer() from trx_video_freeze_enter()).
//
// The HDMI row (TRX64-Ultimate U16): the splash covers the active area of the
// row that runs (0x4F07; 1080p on a bitstream without the mode request), its
// canvas and the picture's size and place from trx_splash.h. The row asked for
// at boot runs before this task exists: U64Config's first effectuate (init
// function 1) writes the request and waits until the row runs (and before
// that, after a System Mode switch, until the family has moved), all in the
// init task; this one is created at 91. A row that changes while the splash
// is up (a System Mode switch from the API or a state, a request that came
// later than those waits) is followed: the wait loop reads the row every
// 20-50 ms and moves the splash to the new canvas (trx_lv_resize, the picture
// painted again). During the fade it is not moved: the splash ends at once.
//
// The picture: software/lvgl_port/assets/trx_splash.c (tools/splash_rle.py),
// decoded into both frame buffers, shrunk on the way where the canvas is
// smaller than the 1080p one (trx_splash_paint); LVGL draws the version line
// over it in the bottom right corner. Without it: a title (under a
// tools/lvgl_logo.sh logo, if there is one) on black, centred on the canvas
// (the logo is not shrunk).
//
// READY: the 6510's PC from the state bus (chip 1 word 1, ARCH §15.3), loaded by
// a frame capture every CAP_EVERY frames while the splash waits; the KERNAL's
// keyboard wait loop ($E5CD-$E5D5, reached by BASIC's input line after the
// banner and READY.) means the C64 is at the prompt. A capture only copies
// state into shadow registers, the machine does not notice it. A cartridge or
// KERNAL that never runs that loop ends the splash at 4 s.
//
// The C64 is started (c64->start()) right after the init functions; this one
// runs after "TRX Video" (90), whose init switches the layer off.

#include "trx_lv_port.h"
#include "trx_io.h"
#include "trx_splash.h"
#include "lvgl.h"
#include "init_function.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

#include "versions.h"
#include "gitinfo.h"

#ifdef TRX_HAVE_SPLASH
// software/lvgl_port/assets/trx_splash.c (tools/splash_rle.py, which describes the format)
extern "C" const uint16_t trx_splash_w, trx_splash_h, trx_splash_x, trx_splash_y;
extern "C" const uint16_t trx_splash_pal[16];
extern "C" const uint32_t trx_splash_len;
extern "C" const uint8_t trx_splash_rle[];
#elif defined(TRX_HAVE_LOGO)
extern "C" const lv_image_dsc_t trx_logo; // software/lvgl_port/assets/trx_logo.c (tools/lvgl_logo.sh)
#endif

#define CAP_CTRL       (TRX_IO_BASE + 0x0104)
#define SB_ADDR        (TRX_IO_BASE + 0x1000)
#define SB_DATA        (TRX_IO_BASE + 0x1004)
#define SB_CPU_PC      ((1 << 7) | 1)   // chip 1 (6510) word 1: {opcode fetch, P, PC}
#define CAP_EVERY      5                // frames between captures while waiting

#define READY_LOOP_LO  0xE5CD           // LDA $C6 / STA $CC / STA $0292 / BEQ $E5CD
#define READY_LOOP_HI  0xE5D5

#define SPLASH_MIN_MS  2000
#define SPLASH_MAX_MS  4000
#define SPLASH_NOPC_MS 3000             // without the state bus: no READY to wait for
#define FADE_MS        300
#define FADE_STEPS     15
#define LOGO_GAP       24               // pixels between logo and text

#define UI_STACK       (2 * configMINIMAL_STACK_SIZE)
#define UI_PRIO        (tskIDLE_PRIORITY + 1) // above the init task (0): the splash comes up first

static bool have_pc;
static uint8_t cap_saved;
static int splash_row;                  // the HDMI row the splash is laid out for
static trx_splash_plan_t splash;        // its canvas and the picture's place there

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void pc_watch(bool on)
{
    if (on) {
        cap_saved = uint8_t(trx_io_read32(CAP_CTRL));
        trx_io_write32(CAP_CTRL, cap_saved ? cap_saved : CAP_EVERY);
    } else {
        trx_io_write32(CAP_CTRL, cap_saved);
    }
}

static bool c64_at_ready(void)
{
    trx_io_write32(SB_ADDR, SB_CPU_PC);
    uint16_t pc = uint16_t(trx_io_read32(SB_DATA));
    return pc >= READY_LOOP_LO && pc <= READY_LOOP_HI;
}

// The row the HDMI timing runs (mode_req.v, U16), or 1080p on a bitstream without the request.
static int running_row(void)
{
    return trx_io_hdmi_modes() ? (ioRead8(TRX_MR_ROW) & 15) : 2;
}

// The splash's place in a row. The active area and the aspect are the row's, every scale's entry has them; a row the
// table does not have (mode_req.v runs none) takes 1080p's.
static trx_splash_plan_t plan_for(int row)
{
    const trx_hdmi_geom_t *g = trx_hdmi_geom(row, 1);
    return trx_splash_plan(g ? g : trx_hdmi_geom(2, 1));
}

// True when the row that runs now puts the splash elsewhere; p is then its place. 720p and 1080p have the same canvas in
// both families.
static bool canvas_moved(trx_splash_plan_t *p)
{
    int row = running_row();
    if (row == splash_row) {
        return false;
    }
    splash_row = row;
    *p = plan_for(row);
    return !trx_splash_same(p, &splash);
}

static void log_place(const char *what)
{
    printf("[TRX UI] splash %s: row %d, canvas %d x %d at scale %d, picture %d x %d at (%d, %d)\n", what, splash_row,
           splash.c.w, splash.c.h, splash.c.s, splash.pw, splash.ph, splash.px, splash.py);
}

#ifdef TRX_HAVE_SPLASH
// The splash runs decoded into a frame buffer, at the picture's place on the canvas; the rest of it black.
static void paint_splash(uint16_t *fb)
{
    trx_splash_pic_t pic;
    pic.w = trx_splash_w;
    pic.h = trx_splash_h;
    pic.x0 = trx_splash_x;
    pic.y0 = trx_splash_y;
    pic.pal = trx_splash_pal;
    pic.rle = trx_splash_rle;
    pic.len = trx_splash_len;
    trx_splash_paint(fb, &splash, &pic);
}
#endif

// The row moved the canvas: the display follows, the picture is painted again for it. False when the layer was taken
// away meanwhile.
static bool move_splash(const trx_splash_plan_t *p)
{
    splash = *p;
    if (!trx_lv_resize(&splash.c)) {
        return false;
    }
#ifdef TRX_HAVE_SPLASH
    trx_lv_backdrop(paint_splash);
#endif
    log_place("moved");
    return true;
}

static void build_splash(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
#ifdef TRX_HAVE_SPLASH
    // The picture is in the frame buffers; LVGL draws only the version line on it.
    trx_lv_backdrop(paint_splash);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
#else
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "TRX64-Ultimate");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
#ifdef TRX_HAVE_LOGO
    // Logo above the title, the pair centred.
    int32_t lh = lv_font_get_line_height(LV_FONT_DEFAULT);
    int32_t ih = trx_logo.header.h;
    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &trx_logo);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -(lh + LOGO_GAP) / 2);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, (ih + LOGO_GAP) / 2);
#else
    lv_obj_center(title);
#endif
#endif
    // Version line, bottom right of the canvas: the application version and,
    // when the build had git, its commit. Its font and length are what the host
    // test checks against the smallest canvas (trx_splash.h).
    static char version[TRX_SPLASH_VERSION_MAX + 1];
    const char *hash = APP_VERSION_HASH;
    snprintf(version, sizeof(version), hash[0] ? "V%s %s" : "V%s", APPL_VERSION_ASCII, hash);
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text_static(ver, version);
    lv_obj_set_style_text_font(ver, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x7B7B7B), 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -TRX_SPLASH_VERSION_MARGIN, -TRX_SPLASH_VERSION_MARGIN);
}

static void ui_task(void *)
{
    splash_row = running_row();
    splash = plan_for(splash_row);
    if (!trx_lv_open(&splash.c)) {
        vTaskDelete(NULL);
        return;
    }
    build_splash();
    log_place("up");
    bool gone = trx_lv_handler() < 0; // renders and shows the splash
    const char *end = gone ? "ended by a freeze" : "done";
    uint32_t t0 = now_ms();
    if (have_pc) {
        pc_watch(true);
    }
    bool ready = false;
    trx_splash_plan_t p;
    while (!gone) {
        if (canvas_moved(&p) && !move_splash(&p)) {
            gone = true;
            end = "ended by a freeze";
            break;
        }
        uint32_t t = now_ms() - t0;
        if (!ready && have_pc) {
            ready = c64_at_ready();
            if (ready) {
                printf("[TRX UI] C64 at READY after %u ms of splash\n", (unsigned)t);
            }
        }
        bool done = have_pc ? ((ready && t >= SPLASH_MIN_MS) || t >= SPLASH_MAX_MS) : (t >= SPLASH_NOPC_MS);
        if (done) {
            break;
        }
        int ms = trx_lv_handler();
        if (ms < 0) {
            gone = true;
            end = "ended by a freeze";
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ms < 20 ? 20 : ms));
    }
    if (have_pc) {
        pc_watch(false);
    }
    for (int i = FADE_STEPS - 1; i >= 0 && !gone; i--) {
        if (canvas_moved(&p)) {
            end = "ended by an HDMI row change";
            break; // the last 300 ms are not worth a move: off now (trx_lv_close)
        }
        if (!trx_lv_set_alpha(uint8_t(255 * i / FADE_STEPS))) {
            gone = true;
            end = "ended by a freeze";
        }
        vTaskDelay(pdMS_TO_TICKS(FADE_MS / FADE_STEPS));
    }
    trx_lv_close();
    printf("[TRX UI] splash %s; UI parked\n", end);
    vTaskDelete(NULL);
}

static void init(void *, void *)
{
    if (!trx_lv_available()) {
        return; // stock bitstream, or a layer that is not RGB565: nothing changes
    }
    uint8_t sb = TRX_CAPS_GATE | TRX_CAPS_STATEBUS;
    have_pc = (trx_io_caps() & sb) == sb;
    if (xTaskCreate(ui_task, "TRX UI", UI_STACK, NULL, UI_PRIO, NULL) != pdPASS) {
        printf("[TRX UI] no task\n");
    }
}

InitFunction trx_ui_init("TRX UI", init, NULL, NULL, 91);
