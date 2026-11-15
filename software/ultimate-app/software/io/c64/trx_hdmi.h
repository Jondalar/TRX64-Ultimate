// U16 (TRX64-Ultimate docs/specs/U16-hdmi-modes.md D5, D6): the HDMI mode the application asks the TRX64 Ultimate
// bitstream for, and where the freeze still goes on the pixel layer so that it lies under the scaler's picture in every
// row and scale. Pure functions over the generated table (trx_hdmi_modes.h, tools/hdmi_modes.py), checked on the host
// by software/test/trx_hdmi (target/pc/linux/trxhdmitest).
#ifndef TRX_HDMI_H
#define TRX_HDMI_H

#include "trx_hdmi_modes.h"

// "HDMI Scan Resolution" (u64_config.cc scan_modes[]: 0 SD, 1 HD, 2 FullHD, 3 800 x 600, 4 1024 x 768, 5 1280 x 1024) ->
// the value for 0x4F00. mode_req.v puts the System Mode's family into it and refuses a row that does not lock, so the
// application asks only for rows that exist: the 60 Hz family takes 800 x 600 for both larger PC modes, the 50 Hz family
// has no PC row and takes 576p for each of them (U16 D5).
static inline int trx_hdmi_request(int setting, bool v60)
{
    if (setting < 0 || setting > 5) {
        return 2;
    }
    if (setting <= 2) {
        return setting;
    }
    return v60 ? 3 : 0;
}

// The geometry of a row (family * 6 + hdmi, 0x4F07) at a scale (0 Integer, 1 Fit, 2 Fill, 0x4F0A); NULL for a pair the
// bitstream does not run.
static inline const trx_hdmi_geom_t *trx_hdmi_geom(int row, int scale)
{
    for (int i = 0; i < TRX_HDMI_N_GEOM; i++) {
        if (trx_hdmi_geoms[i].row == row && trx_hdmi_geoms[i].scale == scale) {
            return &trx_hdmi_geoms[i];
        }
    }
    return 0;
}

// The scaler's VIC window: 400 pixels, 270 lines (rows 6-9, the 60 Hz family: 240). The C64 text area: 320 x 200.
#define TRX_HDMI_WIN_W  400
#define TRX_HDMI_TEXT_W 320
#define TRX_HDMI_TEXT_H 200

static inline int trx_hdmi_win_h(const trx_hdmi_geom_t *g)
{
    return (g->row >= 6) ? 240 : 270;
}

// The pixel layer (pix_layer.v): at most 960 source pixels a line, each shown as S x S output pixels (S 1..3), the stride a
// multiple of 16 bytes, the position relative to the active origin. The still covers the scaler's window: S is the smallest
// factor whose width fits (1 up to 960 output pixels, 2 above), the size rounded up so that no window pixel is left out.
#define TRX_LAYER_MAX_W 960
// The largest layer any row and scale of the table needs (the host test proves both): 800 x 600 at scale 1 (lines), 960 x 540
// RGB565 at scale 2 (1080p Fill, bytes).
#define TRX_LAYER_MAX_H     600
#define TRX_LAYER_MAX_BYTES (960 * 540 * 2)

typedef struct {
    int s;          // output pixels per layer pixel, both axes
    int w, h;       // layer pixels
    int stride;     // bytes from line to line (RGB565)
    int x0, y0;     // PX_POS: the window's first output pixel / line in the active area
} trx_layer_plan_t;

static inline trx_layer_plan_t trx_hdmi_plan(const trx_hdmi_geom_t *g)
{
    trx_layer_plan_t p;
    p.s = 1;
    while ((g->ww + p.s - 1) / p.s > TRX_LAYER_MAX_W) {
        p.s++;
    }
    p.w = (g->ww + p.s - 1) / p.s;
    p.h = (g->wh + p.s - 1) / p.s;
    p.stride = (p.w * 2 + 15) & ~15;
    p.x0 = g->xw0;
    p.y0 = g->yw0;
    return p;
}

// Layer column i / line j -> the VIC pixel / line of the window the scaler shows at the layer pixel's first output pixel
// (window pixel S i, line S j): floor(x hn / hd), floor(y vn / vd), as vic_scaler.v's accumulators count.
static inline int trx_hdmi_vic_x(const trx_hdmi_geom_t *g, const trx_layer_plan_t *p, int i)
{
    int v = (p->s * i * g->hn) / g->hd;
    return (v < TRX_HDMI_WIN_W) ? v : TRX_HDMI_WIN_W - 1;
}

static inline int trx_hdmi_vic_y(const trx_hdmi_geom_t *g, const trx_layer_plan_t *p, int j)
{
    int v = (p->s * j * g->vn) / g->vd;
    int n = trx_hdmi_win_h(g);
    return (v < n) ? v : n - 1;
}

// Layer column i / line j -> the C64 text pixel / line c64_text.v draws there (its area from (xt0, yt0), steps hn / hd and
// tvn / tvd), -1 outside the text area.
static inline int trx_hdmi_text_x(const trx_hdmi_geom_t *g, const trx_layer_plan_t *p, int i)
{
    int x = g->xw0 + p->s * i - g->xt0;
    if (x < 0) {
        return -1;
    }
    int t = (x * g->hn) / g->hd;
    return (t < TRX_HDMI_TEXT_W) ? t : -1;
}

static inline int trx_hdmi_text_y(const trx_hdmi_geom_t *g, const trx_layer_plan_t *p, int j)
{
    int y = g->yw0 + p->s * j - g->yt0;
    if (y < 0) {
        return -1;
    }
    int t = (y * g->tvn) / g->tvd;
    return (t < TRX_HDMI_TEXT_H) ? t : -1;
}

#endif
