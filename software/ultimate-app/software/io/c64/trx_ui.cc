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
// The picture: software/lvgl_port/assets/trx_splash.c (tools/splash_rle.py),
// decoded straight into both frame buffers; LVGL draws the version line over it
// in the bottom right corner. Without it: a title (under a tools/lvgl_logo.sh
// logo, if there is one) on black.
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
#define VERSION_MARGIN 6                // version line from the bottom right corner

#define UI_STACK       (2 * configMINIMAL_STACK_SIZE)
#define UI_PRIO        (tskIDLE_PRIORITY + 1) // above the init task (0): the splash comes up first

static bool have_pc;
static uint8_t cap_saved;

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

#ifdef TRX_HAVE_SPLASH
// Decodes the splash runs into a frame buffer; the rest of it stays black.
static void paint_splash(uint16_t *fb)
{
    memset(fb, 0, TRX_LV_W * TRX_LV_H * 2);
    uint16_t *row = fb + trx_splash_y * TRX_LV_W + trx_splash_x;
    uint32_t x = 0, y = 0;
    for (uint32_t k = 0; k < trx_splash_len && y < trx_splash_h;) {
        uint8_t b = trx_splash_rle[k++];
        uint32_t run = b & 15;
        if (!run) {
            if (k >= trx_splash_len) {
                break;
            }
            run = trx_splash_rle[k++] + 16;
        }
        uint16_t c = trx_splash_pal[b >> 4];
        while (run && y < trx_splash_h) {
            uint32_t n = trx_splash_w - x;
            if (n > run) {
                n = run;
            }
            for (uint32_t i = 0; i < n; i++) {
                row[x + i] = c;
            }
            run -= n;
            x += n;
            if (x == trx_splash_w) {
                x = 0;
                y++;
                row += TRX_LV_W;
            }
        }
    }
}
#endif

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
    // Version line, bottom right: the application version and, when the build
    // had git, its commit.
    static char version[48];
    const char *hash = APP_VERSION_HASH;
    snprintf(version, sizeof(version), hash[0] ? "V%s %s" : "V%s", APPL_VERSION_ASCII, hash);
    lv_obj_t *ver = lv_label_create(scr);
    lv_label_set_text_static(ver, version);
    lv_obj_set_style_text_color(ver, lv_color_hex(0x7B7B7B), 0);
    lv_obj_align(ver, LV_ALIGN_BOTTOM_RIGHT, -VERSION_MARGIN, -VERSION_MARGIN);
}

static void ui_task(void *)
{
    if (!trx_lv_open()) {
        vTaskDelete(NULL);
        return;
    }
    build_splash();
    bool gone = trx_lv_handler() < 0; // renders and shows the splash
    uint32_t t0 = now_ms();
    if (have_pc) {
        pc_watch(true);
    }
    bool ready = false;
    while (!gone) {
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
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ms < 20 ? 20 : ms));
    }
    if (have_pc) {
        pc_watch(false);
    }
    for (int i = FADE_STEPS - 1; i >= 0 && !gone; i--) {
        if (!trx_lv_set_alpha(uint8_t(255 * i / FADE_STEPS))) {
            gone = true;
        }
        vTaskDelay(pdMS_TO_TICKS(FADE_MS / FADE_STEPS));
    }
    trx_lv_close();
    printf("[TRX UI] splash %s; UI parked\n", gone ? "ended by a freeze" : "done");
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
