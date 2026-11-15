// The boot splash in the HDMI row that runs (TRX64-Ultimate docs/specs/T24-lvgl-ui.md §8, U16-hdmi-modes.md §5): where its
// LVGL canvas lies on the pixel layer and how the splash picture is drawn into it. Pure functions; checked on the host by
// software/test/trx_hdmi (target/pc/linux/trxhdmitest).
//
// The canvas covers the row's whole active area, as pix_layer.v can show it: S the smallest layer scale whose width fits
// (trx_layer_scale: 1 at 576p, 480p and 800 x 600, 2 at 720p and 1080p), hact / S x vact / S layer pixels from the active
// origin. The picture was made for the 1080p canvas, a TRX_SPLASH_FRAME_W x _H frame of square pixels (tools/splash_rle.py
// centres it there). On every canvas the frame is drawn at the largest size that has the shape it has on a 1080p screen,
// centred, the rest black; at 1080p that is the frame itself, pixel for pixel. The shape of an output pixel is the row's AVI
// picture aspect over its active area (asp, hm_aspect): 576p (720 x 576 named 4:3) has pixels 16/15 as wide as high, the
// other rows square ones (480p is 640 x 480 named 4:3, 720p and 1080p are named 16:9, 800 x 600 names none).
// The picture is shrunk while it is decoded: each canvas pixel shows the frame pixel at its centre, so it keeps its colours,
// and a run of n frame pixels at the factor f covers floor(n f) or ceil(n f) canvas pixels (none is lost while n f >= 1).
#ifndef TRX_SPLASH_H
#define TRX_SPLASH_H

#include <stdint.h>
#include <string.h>
#include "trx_hdmi.h"

#define TRX_SPLASH_FRAME_W 960  // tools/splash_rle.py LAYER_W, LAYER_H: the 1080p canvas (1920 x 1080 at S = 2)
#define TRX_SPLASH_FRAME_H 540

// The version line (trx_ui.cc): at most TRX_SPLASH_VERSION_MAX characters of lv_font_unscii_8 (8 pixels a character, a line
// 9 high), TRX_SPLASH_VERSION_MARGIN canvas pixels from the bottom right corner. A pixel font drawn at its own size: 8 x 9
// output pixels at S = 1, 16 x 18 at S = 2.
#define TRX_SPLASH_VERSION_MAX    47
#define TRX_SPLASH_VERSION_MARGIN 6
#define TRX_SPLASH_FONT_W         8
#define TRX_SPLASH_FONT_H         9

typedef struct {
    trx_layer_plan_t c;     // the canvas: the LVGL display, the layer's PX_SIZE c.w x c.h at scale c.s from PX_POS (c.x0, c.y0)
    int pw, ph;             // the picture on the canvas: its size
    int px, py;             // and its top left pixel
} trx_splash_plan_t;

static inline trx_splash_plan_t trx_splash_plan(const trx_hdmi_geom_t *g)
{
    trx_splash_plan_t p;
    p.c.s = trx_layer_scale(g->hact);
    p.c.w = (g->hact + p.c.s - 1) / p.c.s;
    p.c.h = (g->vact + p.c.s - 1) / p.c.s;
    p.c.stride = (p.c.w * 2 + 15) & ~15;
    p.c.x0 = 0;
    p.c.y0 = 0;
    // An output pixel is wn / wd as wide as it is high: (an vact) / (ad hact) for the aspect an : ad, square without one.
    int64_t an = g->hact, ad = g->vact;
    if (g->asp == 1) {
        an = 4;
        ad = 3;
    } else if (g->asp == 2) {
        an = 16;
        ad = 9;
    }
    const int64_t wn = an * g->vact, wd = ad * g->hact, fw = TRX_SPLASH_FRAME_W, fh = TRX_SPLASH_FRAME_H;
    // On the screen pw wn / wd : ph = fw : fh. The full width if the height fits, else the full height; the nearest pixel.
    p.pw = p.c.w;
    p.ph = int((2 * p.pw * wn * fh + wd * fw) / (2 * wd * fw));
    if (p.ph > p.c.h) {
        p.ph = p.c.h;
        p.pw = int((2 * p.ph * wd * fw + wn * fh) / (2 * wn * fh));
        if (p.pw > p.c.w) {
            p.pw = p.c.w;
        }
    }
    p.px = (p.c.w - p.pw) / 2;
    p.py = (p.c.h - p.ph) / 2;
    return p;
}

static inline bool trx_splash_same(const trx_splash_plan_t *a, const trx_splash_plan_t *b)
{
    return a->c.s == b->c.s && a->c.w == b->c.w && a->c.h == b->c.h && a->c.stride == b->c.stride && a->c.x0 == b->c.x0 &&
           a->c.y0 == b->c.y0 && a->pw == b->pw && a->ph == b->ph && a->px == b->px && a->py == b->py;
}

// The splash picture (software/lvgl_port/assets/trx_splash.c, tools/splash_rle.py describes the runs).
typedef struct {
    int w, h;               // pixels
    int x0, y0;             // where they lie in the TRX_SPLASH_FRAME_W x _H frame
    const uint16_t *pal;    // 16 RGB565 colours by index
    const uint8_t *rle;     // the runs, row by row
    uint32_t len;
} trx_splash_pic_t;

// floor((2 k + 1) n / (2 m)) for k = 0, 1, 2, ...: the frame pixel at the centre of canvas pixel k when n frame pixels are drawn
// on m; carried with its remainder, no division per step.
typedef struct {
    int q, r, n2, m2;
} trx_splash_step_t;

static inline void trx_splash_step_init(trx_splash_step_t *s, int n, int m)
{
    s->n2 = 2 * n;
    s->m2 = 2 * m;
    s->q = n / s->m2;
    s->r = n % s->m2;
}

static inline void trx_splash_step_next(trx_splash_step_t *s)
{
    s->r += s->n2;
    while (s->r >= s->m2) {
        s->r -= s->m2;
        s->q++;
    }
}

// Canvas line j of the picture, if it shows picture line y (frame line y0 + y); sx and i are then set to the line's first
// canvas column inside the picture. 0 if it shows another line.
static inline uint16_t *trx_splash_line(const trx_splash_plan_t *p, const trx_splash_pic_t *pic, uint16_t *top, int j,
                                        const trx_splash_step_t *sy, int y, trx_splash_step_t *sx, int *i)
{
    if (j >= p->ph || sy->q != pic->y0 + y) {
        return 0;
    }
    trx_splash_step_init(sx, TRX_SPLASH_FRAME_W, p->pw);
    *i = 0;
    while (*i < p->pw && sx->q < pic->x0) {
        (*i)++;
        trx_splash_step_next(sx);
    }
    return top + j * (p->c.stride / 2);
}

// The canvas of p into fb (p->c.stride bytes a line): black, and in the pw x ph rectangle at (px, py) the frame, the picture's
// runs decoded and sampled on the way. A picture line no canvas line shows is decoded and dropped.
static inline void trx_splash_paint(uint16_t *fb, const trx_splash_plan_t *p, const trx_splash_pic_t *pic)
{
    memset(fb, 0, size_t(p->c.stride) * p->c.h);
    if (p->pw < 1 || p->ph < 1 || pic->w < 1 || pic->h < 1) {
        return;
    }
    const int spx = p->c.stride / 2;
    uint16_t *top = fb + p->py * spx + p->px;
    trx_splash_step_t sy, sx;
    trx_splash_step_init(&sy, TRX_SPLASH_FRAME_H, p->ph);
    int j = 0;                      // the next canvas line of the rectangle
    while (j < p->ph && sy.q < pic->y0) {
        j++;                        // frame lines above the picture: black
        trx_splash_step_next(&sy);
    }
    int x = 0, y = 0;               // the picture pixel the runs have reached
    int i = 0;                      // on line j: the next column
    uint16_t *dst = trx_splash_line(p, pic, top, j, &sy, 0, &sx, &i);
    for (uint32_t k = 0; k < pic->len && y < pic->h && j < p->ph;) {
        uint8_t b = pic->rle[k++];
        int run = b & 15;
        if (!run) {
            if (k >= pic->len) {
                break;
            }
            run = pic->rle[k++] + 16;
        }
        const uint16_t c = pic->pal[b >> 4];
        while (run > 0 && y < pic->h) {
            int n = pic->w - x;
            if (n > run) {
                n = run;
            }
            if (dst) {
                const int end = pic->x0 + x + n; // the frame column after the run
                while (i < p->pw && sx.q < end) {
                    dst[i++] = c;
                    trx_splash_step_next(&sx);
                }
            }
            run -= n;
            x += n;
            if (x == pic->w) {
                if (dst) {
                    j++;
                    trx_splash_step_next(&sy);
                    while (j < p->ph && sy.q == pic->y0 + y) { // drawn larger than the frame: the same line again
                        memcpy(top + j * spx, dst, size_t(p->pw) * 2);
                        j++;
                        trx_splash_step_next(&sy);
                    }
                }
                x = 0;
                y++;
                dst = trx_splash_line(p, pic, top, j, &sy, y, &sx, &i);
            }
        }
    }
}

#endif
