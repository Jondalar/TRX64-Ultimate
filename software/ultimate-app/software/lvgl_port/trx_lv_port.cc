// LVGL on the TRX64 Ultimate pixel layer. See trx_lv_port.h.
//
// Registers (pix_layer.v; 32-bit, byte 3 commits, trx_io.h):
//   0x101A6080 PX_CTRL   b0 enable, b3:1 format (1 RGB565), b5:4 scale
//   0x101A6084 PX_FRONT  the shown buffer;  6088 PX_BACK: shown from the next frame start
//   6090 PX_SIZE {h, w}, 6094 PX_STRIDE, 6098 PX_POS {y0, x0}, 609C PX_GALPHA, 60A0 PX_KEY
//   60A4 PX_STATUS R b31:16 frames started (wrapping)

#include "trx_lv_port.h"
#include "trx_io.h"
#include "lvgl.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PX_CTRL        (TRX_IO_BASE + 0x6080)
#define PX_FRONT       (TRX_IO_BASE + 0x6084)
#define PX_BACK        (TRX_IO_BASE + 0x6088)
#define PX_SIZE        (TRX_IO_BASE + 0x6090)
#define PX_STRIDE      (TRX_IO_BASE + 0x6094)
#define PX_POS         (TRX_IO_BASE + 0x6098)
#define PX_GALPHA      (TRX_IO_BASE + 0x609C)
#define PX_KEY         (TRX_IO_BASE + 0x60A0)
#define PX_STATUS      (TRX_IO_BASE + 0x60A4)
#define PX_CTRL_OFF    0x22 // RGB565, scale 2, disabled
#define PX_CTRL_FMT    0x02 // RGB565; b5:4 the scale, b0 enable

#define BUF_BYTES      TRX_LV_BUF_BYTES
#define FLIP_TIMEOUT   pdMS_TO_TICKS(100)
#define MAX_SLEEP_MS   50

static SemaphoreHandle_t lock;      // the layer's registers and the LVGL state below
static SemaphoreHandle_t flip_sem;
static bool irq_attached;
static lv_display_t *disp;
static void *raw[2];                 // malloc results
static uint16_t *buf[2];             // 16-byte aligned
static trx_layer_plan_t canvas;      // the display's size, the layer's scale, stride and place
static bool layer_on;                // LVGL switched the layer on
static volatile bool taken;          // trx_lv_yield_layer() since trx_lv_open()

bool trx_lv_available(void)
{
    uint8_t need = TRX_CAPS_VIDEO | TRX_CAPS_PX565;
    if ((trx_io_caps() & need) != need) {
        return false;
    }
    // Created here, in the init path, before any task can yield the layer.
    if (!lock) {
        lock = xSemaphoreCreateMutex();
        flip_sem = xSemaphoreCreateBinary();
    }
    return lock && flip_sem;
}

static uint32_t tick_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// A canvas the buffers hold and pix_layer.v shows: at most 960 pixels a line, scale 1..3, a stride of 16-byte steps.
static bool canvas_fits(const trx_layer_plan_t *c)
{
    return c->w >= 1 && c->w <= TRX_LAYER_MAX_W && c->h >= 1 && c->s >= 1 && c->s <= 3 && (c->stride & 15) == 0 &&
           c->stride >= 2 * c->w && c->stride * c->h <= BUF_BYTES && c->x0 >= 0 && c->y0 >= 0;
}

// Waits until n frames have started (PX_STATUS b31:16), at most 100 ms.
static void wait_frames(int n)
{
    uint16_t f0 = uint16_t(trx_io_read32(PX_STATUS) >> 16);
    for (int t = 0; t < 20 && uint16_t(uint16_t(trx_io_read32(PX_STATUS) >> 16) - f0) < n; t++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static uint8_t flip_irq(uint8_t act)
{
    BaseType_t woken = pdFALSE;
    ioWrite8(TRX_IRQ_CAUSE, act);
    xSemaphoreGiveFromISR(flip_sem, &woken);
    return woken == pdTRUE;
}

// The rvlite core has no data cache, but its stores are posted: a load from the
// last address written waits for them before the register write that follows.
static inline void stores_done(const uint16_t *last)
{
    __asm__ volatile ("fence" ::: "memory");
    (void)*(volatile const uint16_t *)last;
}

// Called by LVGL from trx_lv_handler(), so with the lock held.
static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px_map)
{
    uint16_t *fb = (uint16_t *)px_map;
    stores_done(fb + area->y2 * (canvas.stride / 2) + area->x2);
    if (taken || !lv_display_flush_is_last(d)) {
        lv_display_flush_ready(d);
        return;
    }
    if (!layer_on) {
        // First picture (after trx_lv_open or trx_lv_resize): the canvas, shown opaque.
        trx_io_write32(PX_CTRL, PX_CTRL_OFF);
        trx_io_write32(PX_SIZE, (uint32_t(canvas.h) << 16) | uint32_t(canvas.w));
        trx_io_write32(PX_STRIDE, uint32_t(canvas.stride));
        trx_io_write32(PX_POS, (uint32_t(canvas.y0) << 16) | uint32_t(canvas.x0));
        trx_io_write32(PX_GALPHA, 255);
        trx_io_write32(PX_KEY, 0);
        trx_io_write32(PX_FRONT, (uint32_t)fb);
        trx_io_write32(PX_CTRL, PX_CTRL_FMT | (uint32_t(canvas.s) << 4) | 1);
        layer_on = true;
    } else {
        xSemaphoreTake(flip_sem, 0); // drop a stale give
        ioWrite8(TRX_IRQ_CAUSE, TRX_IRQ_FLIP);
        trx_io_write32(PX_BACK, (uint32_t)fb);
        if (xSemaphoreTake(flip_sem, FLIP_TIMEOUT) != pdTRUE) {
            printf("[TRX UI] pixel layer: no flip\n");
        }
    }
    lv_display_flush_ready(d);
}

static void free_buffers(void)
{
    for (int i = 0; i < 2; i++) {
        free(raw[i]);
        raw[i] = NULL;
        buf[i] = NULL;
    }
}

bool trx_lv_open(const trx_layer_plan_t *c)
{
    if (!trx_lv_available() || disp) {
        return false;
    }
    if (!canvas_fits(c)) {
        printf("[TRX UI] pixel layer: no canvas %d x %d at scale %d (stride %d)\n", c->w, c->h, c->s, c->stride);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        raw[i] = malloc(BUF_BYTES + 15);
        if (!raw[i]) {
            printf("[TRX UI] pixel layer: no memory for frame buffer %d (%d bytes)\n", i, BUF_BYTES);
            free_buffers();
            return false;
        }
        buf[i] = (uint16_t *)(((uint32_t)raw[i] + 15) & ~15UL);
    }
    if (!irq_attached) {
        trx_irq_attach(TRX_IRQ_FLIP, flip_irq);
        irq_attached = true;
    }
    trx_irq_enable(TRX_IRQ_FLIP, true);

    xSemaphoreTake(lock, portMAX_DELAY);
    taken = false;
    layer_on = false;
    canvas = *c;
    lv_init();
    lv_tick_set_cb(tick_ms);
    disp = lv_display_create(canvas.w, canvas.h);
    if (disp) {
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers_with_stride(disp, buf[0], buf[1], BUF_BYTES, canvas.stride, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(disp, flush_cb);
    } else {
        lv_deinit();
    }
    xSemaphoreGive(lock);
    if (!disp) {
        free_buffers();
        return false;
    }
    printf("[TRX UI] LVGL %d.%d.%d on the pixel layer: %d x %d at scale %d, buffers at %p, %p, heap free %u\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, canvas.w, canvas.h, canvas.s, buf[0], buf[1],
           (unsigned)xPortGetFreeHeapSize());
    return true;
}

bool trx_lv_resize(const trx_layer_plan_t *c)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = disp && !taken;
    if (ok && layer_on) {
        trx_io_write32(PX_CTRL, PX_CTRL_OFF);
        layer_on = false;
    }
    xSemaphoreGive(lock);
    if (!ok) {
        return false;
    }
    if (!canvas_fits(c)) {
        printf("[TRX UI] pixel layer: no canvas %d x %d at scale %d (stride %d)\n", c->w, c->h, c->s, c->stride);
        return false;
    }
    wait_frames(2); // the layer reads the buffers no more
    xSemaphoreTake(lock, portMAX_DELAY);
    ok = disp && !taken;
    if (ok) {
        canvas = *c;
        lv_display_set_resolution(disp, canvas.w, canvas.h);
        lv_display_set_buffers_with_stride(disp, buf[0], buf[1], BUF_BYTES, canvas.stride, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_obj_invalidate(lv_display_get_screen_active(disp)); // the next flush draws it all and shows the layer again
    }
    xSemaphoreGive(lock);
    return ok;
}

void trx_lv_backdrop(void (*paint)(uint16_t *fb))
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (disp) {
        paint(buf[0]);
        memcpy(buf[1], buf[0], size_t(canvas.stride) * canvas.h);
        lv_obj_set_style_bg_opa(lv_display_get_layer_bottom(disp), LV_OPA_TRANSP, 0);
        lv_obj_invalidate(lv_display_get_screen_active(disp)); // its objects again, over the new pixels
    }
    xSemaphoreGive(lock);
}

int trx_lv_handler(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t ms = taken ? 0 : lv_timer_handler();
    bool gone = taken;
    xSemaphoreGive(lock);
    if (gone) {
        return -1;
    }
    return (ms > MAX_SLEEP_MS) ? MAX_SLEEP_MS : int(ms);
}

bool trx_lv_set_alpha(uint8_t alpha)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = !taken;
    if (ok && layer_on) {
        trx_io_write32(PX_GALPHA, alpha);
    }
    xSemaphoreGive(lock);
    return ok;
}

void trx_lv_close(void)
{
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (layer_on && !taken) {
        trx_io_write32(PX_CTRL, PX_CTRL_OFF);
        trx_io_write32(PX_GALPHA, 255);
    }
    layer_on = false;
    if (disp) {
        lv_display_delete(disp);
        disp = NULL;
        lv_deinit();
    }
    xSemaphoreGive(lock);
    free_buffers();
}

void trx_lv_yield_layer(void)
{
    if (!lock) {
        return; // LVGL never ran: nothing of ours on the layer
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (disp && !taken) {
        if (layer_on) {
            trx_io_write32(PX_CTRL, PX_CTRL_OFF);
            trx_io_write32(PX_GALPHA, 255);
            layer_on = false;
        }
        taken = true;
        printf("[TRX UI] pixel layer handed over\n");
    }
    xSemaphoreGive(lock);
}
