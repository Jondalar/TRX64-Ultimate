// Host test of trx_hdmi.h (TRX64-Ultimate docs/specs/U16-hdmi-modes.md D5, D6): the mode the application asks for, and
// the freeze still's place and sampling on the pixel layer in every row and scale of the generated table.
// The scaler is modelled the way vic_scaler.v counts (one accumulator per axis, a step of hn / hd per output pixel,
// saturating at the window's end), not by the formula trx_hdmi.h uses, so the two are checked against each other.
// And of trx_splash.h (T24 §8, U16 §5): the boot splash's canvas and picture in every row, against the numbers worked out
// by hand from each row's active area and CEA / VESA picture aspect (not from the table's asp), the picture painted with
// the splash asset (software/lvgl_port/assets/trx_splash.c) and with made-up pictures, compared pixel for pixel with a
// plain model (decode everything, then sample each canvas pixel at its centre), and the splash before U16 at 1080p.
// Controls: wrong layouts and a broken paint run through the same checks, which must find them.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>
#include "trx_hdmi.h"
#include "trx_splash.h"

// software/lvgl_port/assets/trx_splash.c
extern "C" const uint16_t trx_splash_w, trx_splash_h, trx_splash_x, trx_splash_y;
extern "C" const uint16_t trx_splash_pal[16];
extern "C" const uint32_t trx_splash_len;
extern "C" const uint8_t trx_splash_rle[];

static int checks = 0, failures = 0;

static void check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(bool ok, const char *fmt, ...)
{
    checks++;
    if (!ok) {
        va_list ap;
        va_start(ap, fmt);
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
        failures++;
    }
}

// vic_scaler.v: the window's output pixel k (0 .. n - 1) shows VIC pixel out[k]; the accumulator starts at 0 on the window's
// first pixel and moves on one VIC pixel whenever the sum of the steps reaches the denominator, held at the window's end.
static void scaler_axis(int step_n, int step_d, int vic_n, int n, int *out)
{
    int acc = 0, v = 0;
    for (int k = 0; k < n; k++) {
        out[k] = v;
        if (v != vic_n) {
            int sum = acc + step_n;
            if (sum >= step_d) {
                acc = sum - step_d;
                v++;
            } else {
                acc = sum;
            }
        }
    }
}

// ---- the splash ------------------------------------------------------------------------------------------------------

// Per row, worked out by hand: the screen's shape (CEA-861 VIC 17 and VIC 1 4:3, VIC 4 / 19 and 16 / 31 16:9; 800 x 600
// VESA, 4:3 with square pixels) and the place the splash must have.
struct splash_want {
    int row, dan, dad;      // display aspect dan : dad
    int s, w, h;            // canvas
    int pw, ph, px, py;     // picture
};
static const splash_want splash_wants[7] = {
    { 0, 4, 3, 1, 720, 576, 720, 432, 0, 72 },   // 576p: pixels 16/15 wide, 720 x 432 is 768 x 432 on the screen, 16:9
    { 1, 16, 9, 2, 640, 360, 640, 360, 0, 0 },   // 720p50
    { 2, 16, 9, 2, 960, 540, 960, 540, 0, 0 },   // 1080p50: the splash before U16
    { 6, 4, 3, 1, 640, 480, 640, 360, 0, 60 },   // 480p
    { 7, 16, 9, 2, 640, 360, 640, 360, 0, 0 },   // 720p60
    { 8, 16, 9, 2, 960, 540, 960, 540, 0, 0 },   // 1080p60
    { 9, 4, 3, 1, 800, 600, 800, 450, 0, 75 },   // 800 x 600
};

// The checks of one layout; the number of those that fail (printed when `say`).
static int splash_faults(const trx_hdmi_geom_t *g, const trx_splash_plan_t *p, int dan, int dad, bool say)
{
    typedef long long A;
    const A FW = TRX_SPLASH_FRAME_W, FH = TRX_SPLASH_FRAME_H;
    int n = 0;
    const trx_layer_plan_t &c = p->c;
    struct { bool ok; const char *what; } t[] = {
        { c.s >= 1 && c.s <= 3 && c.w >= 1 && c.w <= TRX_LAYER_MAX_W, "scale 1..3, width 1..960 (pix_layer.v)" },
        { c.s == 1 || (g->hact + c.s - 2) / (c.s - 1) > TRX_LAYER_MAX_W, "the smallest scale whose width fits" },
        { c.s * c.w == g->hact && c.s * c.h == g->vact, "the canvas covers the active area, no more" },
        { c.x0 == 0 && c.y0 == 0, "the canvas starts at the active origin (PX_POS 0)" },
        { c.stride % 16 == 0 && c.stride >= 2 * c.w && c.stride < 2 * c.w + 16, "the stride" },
        { c.stride * c.h <= TRX_LAYER_MAX_BYTES && c.h <= TRX_LAYER_MAX_H, "within one frame buffer (TRX_LV_BUF_BYTES)" },
        { p->pw >= 1 && p->ph >= 1 && p->px >= 0 && p->py >= 0 && p->px + p->pw <= c.w && p->py + p->ph <= c.h,
          "the picture inside the canvas" },
        { p->pw == c.w || p->ph == c.h, "the picture as large as fits (one side full)" },
        { (c.w - p->pw) - 2 * p->px >= 0 && (c.w - p->pw) - 2 * p->px <= 1 && (c.h - p->ph) - 2 * p->py >= 0 &&
          (c.h - p->ph) - 2 * p->py <= 1, "the picture centred" },
        // On the screen the picture spans pw / w of its width and ph / h of its height: (pw / w) dan : (ph / h) dad must be
        // FRAME_W : FRAME_H (16:9), i.e. pw h dan FH = ph w dad FW, to half a pixel of the side that was rounded.
        { (p->pw == c.w) ? 2 * llabs(A(p->ph) * c.w * dad * FW - A(p->pw) * c.h * dan * FH) <= A(c.w) * dad * FW
                         : 2 * llabs(A(p->pw) * c.h * dan * FH - A(p->ph) * c.w * dad * FW) <= A(c.h) * dan * FH,
          "the picture's shape on the screen is the 1080p one (16:9)" },
        // the version line: at most TRX_SPLASH_VERSION_MAX characters, bottom right, a margin on every side
        { TRX_SPLASH_VERSION_MAX * TRX_SPLASH_FONT_W + 2 * TRX_SPLASH_VERSION_MARGIN <= c.w &&
          TRX_SPLASH_FONT_H + 2 * TRX_SPLASH_VERSION_MARGIN <= c.h, "the longest version line fits the canvas" },
    };
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        if (!t[k].ok) {
            n++;
            if (say) {
                printf("FAIL row %d: %s\n", g->row, t[k].what);
            }
        }
    }
    return n;
}

// The frame (TRX_SPLASH_FRAME_W x _H RGB565) with the picture's runs decoded into it, black around it. False if the runs
// do not cover the picture exactly.
static bool splash_frame(const trx_splash_pic_t *pic, std::vector<uint16_t> &f)
{
    f.assign(TRX_SPLASH_FRAME_W * TRX_SPLASH_FRAME_H, 0);
    std::vector<uint16_t> px;
    for (uint32_t k = 0; k < pic->len;) {
        uint8_t b = pic->rle[k++];
        int run = b & 15;
        if (!run) {
            run = (k < pic->len) ? pic->rle[k++] + 16 : 0;
        }
        px.insert(px.end(), run, pic->pal[b >> 4]);
    }
    if (px.size() != size_t(pic->w) * pic->h) {
        return false;
    }
    for (int y = 0; y < pic->h; y++) {
        for (int x = 0; x < pic->w; x++) {
            f[(pic->y0 + y) * TRX_SPLASH_FRAME_W + pic->x0 + x] = px[y * pic->w + x];
        }
    }
    return true;
}

// The model: every canvas pixel inside the picture shows the frame pixel at its centre, the rest is black.
static void splash_model(const trx_splash_plan_t *p, const std::vector<uint16_t> &f, std::vector<uint16_t> &out)
{
    const int spx = p->c.stride / 2;
    out.assign(size_t(spx) * p->c.h, 0);
    for (int j = 0; j < p->ph; j++) {
        int fy = ((2 * j + 1) * TRX_SPLASH_FRAME_H) / (2 * p->ph);
        for (int i = 0; i < p->pw; i++) {
            int fx = ((2 * i + 1) * TRX_SPLASH_FRAME_W) / (2 * p->pw);
            out[(p->py + j) * spx + p->px + i] = f[fy * TRX_SPLASH_FRAME_W + fx];
        }
    }
}

static int splash_differ(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b)
{
    int n = (a.size() == b.size()) ? 0 : 1;
    for (size_t k = 0; k < a.size() && k < b.size(); k++) {
        n += a[k] != b[k];
    }
    return n;
}

// The look: on every canvas line (column) of the picture, every colour run of at least min_run pixels of the frame line
// (column) it samples has a canvas pixel of its colour inside it. The picture's stripes and gaps are 3 pixels and more, the
// tips of its rounded bar ends 1 or 2; at a factor of at least 2/3 a run of 2 always keeps a pixel. The number of lines and
// columns where a run is lost; *tips: the shorter runs no canvas pixel shows.
static int splash_runs_lost(const trx_splash_plan_t *p, const std::vector<uint16_t> &f, const std::vector<uint16_t> &cv,
                            int min_run, int *tips)
{
    const int spx = p->c.stride / 2;
    int bad = 0;
    *tips = 0;
    for (int dir = 0; dir < 2; dir++) {
        const int nl = dir ? p->pw : p->ph;          // canvas lines (dir 0) or columns (dir 1)
        const int nk = dir ? p->ph : p->pw;          // canvas pixels along one
        const int fn = dir ? TRX_SPLASH_FRAME_H : TRX_SPLASH_FRAME_W;
        for (int l = 0; l < nl; l++) {
            int fl = ((2 * l + 1) * (dir ? TRX_SPLASH_FRAME_W : TRX_SPLASH_FRAME_H)) / (2 * (dir ? p->pw : p->ph));
            bool lost = false;
            int k = 0;
            for (int s0 = 0; s0 < fn;) {
                uint16_t v = dir ? f[s0 * TRX_SPLASH_FRAME_W + fl] : f[fl * TRX_SPLASH_FRAME_W + s0];
                int s1 = s0;
                while (s1 < fn && (dir ? f[s1 * TRX_SPLASH_FRAME_W + fl] : f[fl * TRX_SPLASH_FRAME_W + s1]) == v) {
                    s1++;
                }
                bool shown = false;
                while (k < nk && ((2 * k + 1) * fn) / (2 * nk) < s1) {
                    if (((2 * k + 1) * fn) / (2 * nk) >= s0) {
                        uint16_t c = dir ? cv[(p->py + k) * spx + p->px + l] : cv[(p->py + l) * spx + p->px + k];
                        shown |= c == v;
                    }
                    k++;
                }
                if (!shown) {
                    if (s1 - s0 >= min_run) {
                        lost = true;
                    } else {
                        (*tips)++;
                    }
                }
                s0 = s1;
            }
            bad += lost;
        }
    }
    return bad;
}

// The splash before U16 (trx_ui.cc paint_splash): the runs straight into a 960 x 540 buffer, the picture at (x, y).
static void splash_before_u16(const trx_splash_pic_t *pic, std::vector<uint16_t> &fb)
{
    fb.assign(960 * 540, 0);
    uint16_t *row = &fb[pic->y0 * 960 + pic->x0];
    uint32_t x = 0, y = 0;
    for (uint32_t k = 0; k < pic->len && y < uint32_t(pic->h);) {
        uint8_t b = pic->rle[k++];
        uint32_t run = b & 15;
        if (!run) {
            if (k >= pic->len) {
                break;
            }
            run = pic->rle[k++] + 16;
        }
        uint16_t c = pic->pal[b >> 4];
        while (run && y < uint32_t(pic->h)) {
            uint32_t n = pic->w - x;
            if (n > run) {
                n = run;
            }
            for (uint32_t i = 0; i < n; i++) {
                row[x + i] = c;
            }
            run -= n;
            x += n;
            if (x == uint32_t(pic->w)) {
                x = 0;
                y++;
                row += 960;
            }
        }
    }
}

// tools/splash_rle.py encode(): colour indices -> runs.
static std::vector<uint8_t> splash_encode(const std::vector<uint8_t> &idx)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < idx.size()) {
        size_t j = i;
        while (j < idx.size() && idx[j] == idx[i] && j - i < 271) {
            j++;
        }
        size_t run = j - i;
        if (run <= 15) {
            out.push_back(uint8_t(idx[i] << 4 | run));
        } else {
            out.push_back(uint8_t(idx[i] << 4));
            out.push_back(uint8_t(run - 16));
        }
        i = j;
    }
    return out;
}

// Paints p with pic and compares it with the model; the number of pixels that differ.
static int splash_paint_vs_model(const trx_splash_plan_t *p, const trx_splash_pic_t *pic, std::vector<uint16_t> *painted)
{
    std::vector<uint16_t> f, model, cv(size_t(p->c.stride / 2) * p->c.h + 64, 0xDEAD);
    if (!splash_frame(pic, f)) {
        return -1;
    }
    splash_model(p, f, model);
    trx_splash_paint(&cv[0], p, pic);
    for (int k = 0; k < 64; k++) {
        if (cv[cv.size() - 64 + k] != 0xDEAD) {
            return -2; // written past the canvas
        }
    }
    cv.resize(cv.size() - 64);
    if (painted) {
        *painted = cv;
    }
    return splash_differ(cv, model);
}

int main(int, char **)
{
    printf("-- the request (U16 D5): SD, HD and FullHD as they are, the PC modes on the family's row\n");
    static const int want50[6] = { 0, 1, 2, 0, 0, 0 };
    static const int want60[6] = { 0, 1, 2, 3, 3, 3 };
    for (int s = 0; s < 6; s++) {
        check(trx_hdmi_request(s, false) == want50[s], "setting %d at 50 Hz asks for %d", s, want50[s]);
        check(trx_hdmi_request(s, true) == want60[s], "setting %d at 60 Hz asks for %d", s, want60[s]);
        // every request names a row the bitstream runs (mode_req.v refuses the others)
        int row50 = trx_hdmi_request(s, false), row60 = 6 + trx_hdmi_request(s, true);
        check(trx_hdmi_geom(row50, 1) != 0 && trx_hdmi_geom(row60, 1) != 0, "setting %d: both families' rows exist", s);
    }
    check(trx_hdmi_request(-1, false) == 2 && trx_hdmi_request(6, true) == 2, "an unknown setting asks for FullHD");

    printf("-- the table: 7 rows x 3 scales, nothing else\n");
    static const int rows[7] = { 0, 1, 2, 6, 7, 8, 9 };
    int found = 0;
    for (int r = 0; r < 12; r++) {
        for (int sc = 0; sc < 4; sc++) {
            bool in = false;
            for (int k = 0; k < 7; k++) {
                in |= (rows[k] == r) && sc < 3;
            }
            check((trx_hdmi_geom(r, sc) != 0) == in, "row %d scale %d %s", r, sc, in ? "is in the table" : "is not");
            found += in;
        }
    }
    check(found == TRX_HDMI_N_GEOM, "%d entries", found);

    printf("-- every row and scale: the layer within pix_layer.v's limits, over the window, sampled as the scaler shows it\n");
    int max_bytes = 0;
    static int sx[2048], sy[2048];
    for (int n = 0; n < TRX_HDMI_N_GEOM; n++) {
        const trx_hdmi_geom_t *g = &trx_hdmi_geoms[n];
        trx_layer_plan_t p = trx_hdmi_plan(g);
        int wh = trx_hdmi_win_h(g);
        int bytes = p.stride * p.h;
        if (bytes > max_bytes) {
            max_bytes = bytes;
        }
        printf("   row %d scale %d: window %4d x %4d at (%3d, %3d) -> layer %3d x %3d at scale %d, stride %4d, %7d bytes\n",
               g->row, g->scale, g->ww, g->wh, g->xw0, g->yw0, p.w, p.h, p.s, p.stride, bytes);
        check(p.s >= 1 && p.s <= 3 && p.w <= TRX_LAYER_MAX_W, "row %d scale %d: S %d, width %d", g->row, g->scale, p.s, p.w);
        check(p.s == 1 || (g->ww + p.s - 2) / (p.s - 1) > TRX_LAYER_MAX_W, "row %d scale %d: S %d is the smallest", g->row, g->scale, p.s);
        check(p.h <= TRX_LAYER_MAX_H && bytes <= TRX_LAYER_MAX_BYTES, "row %d scale %d: within TRX_LAYER_MAX_H / _BYTES", g->row, g->scale);
        check(p.stride % 16 == 0 && p.stride >= 2 * p.w && p.stride < 2 * p.w + 16, "row %d scale %d: stride %d", g->row, g->scale, p.stride);
        check(p.s * p.w >= g->ww && p.s * p.w - g->ww < p.s && p.s * p.h >= g->wh && p.s * p.h - g->wh < p.s,
              "row %d scale %d: the layer covers the window and no more than one layer pixel beyond it", g->row, g->scale);
        check(p.x0 == g->xw0 && p.y0 == g->yw0 && p.x0 + p.s * p.w <= g->hact && p.y0 + p.s * p.h <= g->vact,
              "row %d scale %d: the layer lies inside the active area", g->row, g->scale);
        scaler_axis(g->hn, g->hd, TRX_HDMI_WIN_W, g->ww, sx);
        scaler_axis(g->vn, g->vd, wh, g->wh, sy);
        check(sx[g->ww - 1] == TRX_HDMI_WIN_W - 1 && sy[g->wh - 1] == wh - 1, "row %d scale %d: the window ends on the last VIC pixel / line", g->row, g->scale);
        bool seen_x[TRX_HDMI_WIN_W] = { false }, seen_y[270] = { false };
        int bad = 0;
        for (int i = 0; i < p.w; i++) {
            int v = trx_hdmi_vic_x(g, &p, i);
            int x = p.s * i;
            if (v != sx[x < g->ww ? x : g->ww - 1]) {
                bad++;
            }
            seen_x[v] = true;
        }
        check(bad == 0, "row %d scale %d: %d layer columns sample another VIC pixel than the scaler shows", g->row, g->scale, bad);
        bad = 0;
        for (int j = 0; j < p.h; j++) {
            int v = trx_hdmi_vic_y(g, &p, j);
            int y = p.s * j;
            if (v != sy[y < g->wh ? y : g->wh - 1]) {
                bad++;
            }
            seen_y[v] = true;
        }
        check(bad == 0, "row %d scale %d: %d layer lines sample another VIC line than the scaler shows", g->row, g->scale, bad);
        int miss = 0;
        for (int v = 0; v < TRX_HDMI_WIN_W; v++) {
            miss += !seen_x[v];
        }
        for (int v = 0; v < wh; v++) {
            miss += !seen_y[v];
        }
        check(miss == 0, "row %d scale %d: %d VIC pixels / lines of the window are not on the layer", g->row, g->scale, miss);
        // the text area: inside the layer, its 320 x 200 pixels in order
        int first = -1, last = -1, prev = -1;
        bool mono = true;
        for (int i = 0; i < p.w; i++) {
            int t = trx_hdmi_text_x(g, &p, i);
            if (t >= 0) {
                if (first < 0) {
                    first = t;
                }
                mono &= (t >= prev);
                prev = last = t;
            }
        }
        check(first == 0 && last == TRX_HDMI_TEXT_W - 1 && mono, "row %d scale %d: the text columns run 0..319 on the layer", g->row, g->scale);
        {
            bool seen[TRX_HDMI_TEXT_W] = { false };
            int gaps = 0;
            for (int i = 0; i < p.w; i++) {
                int t = trx_hdmi_text_x(g, &p, i);
                if (t >= 0) {
                    seen[t] = true;
                }
            }
            for (int t = 0; t < TRX_HDMI_TEXT_W; t++) {
                gaps += !seen[t];
            }
            check(gaps == 0, "row %d scale %d: %d text columns are on no layer column", g->row, g->scale, gaps);
        }
        first = last = prev = -1;
        mono = true;
        for (int j = 0; j < p.h; j++) {
            int t = trx_hdmi_text_y(g, &p, j);
            if (t >= 0) {
                if (first < 0) {
                    first = t;
                }
                mono &= (t >= prev);
                prev = last = t;
            }
        }
        check(first == 0 && last == TRX_HDMI_TEXT_H - 1 && mono, "row %d scale %d: the text lines run 0..199 on the layer", g->row, g->scale);
        {
            bool seen[TRX_HDMI_TEXT_H] = { false };
            int gaps = 0;
            for (int j = 0; j < p.h; j++) {
                int t = trx_hdmi_text_y(g, &p, j);
                if (t >= 0) {
                    seen[t] = true;
                }
            }
            for (int t = 0; t < TRX_HDMI_TEXT_H; t++) {
                gaps += !seen[t];
            }
            check(gaps == 0, "row %d scale %d: %d text lines are on no layer line", g->row, g->scale, gaps);
        }
    }
    printf("   largest buffer %d bytes\n", max_bytes);
    check(max_bytes == TRX_LAYER_MAX_BYTES && max_bytes == 960 * 540 * 2, "the largest buffer is 960 x 540 RGB565 (1,036,800 bytes), not %d", max_bytes);

    printf("-- 1080p at Fit (and every bitstream before U16): the layer of today\n");
    for (int r = 2; r <= 8; r += 6) {
        const trx_hdmi_geom_t *g = trx_hdmi_geom(r, 1);
        trx_layer_plan_t p = trx_hdmi_plan(g);
        check(p.s == 2 && p.w == 800 && p.h == 540 && p.stride == 1600 && p.x0 == 160 && p.y0 == 0,
              "row %d Fit: 800 x 540 at scale 2 from (160, 0)", r);
        int bad = 0;
        for (int j = 0; j < p.h; j++) {
            // the renderer before U16: 50 Hz two layer rows per VIC line, 60 Hz 9 per 4 (still_row(l))
            bad += trx_hdmi_vic_y(g, &p, j) != ((r == 8) ? (j * 4) / 9 : j / 2);
        }
        for (int i = 0; i < p.w; i++) {
            bad += trx_hdmi_vic_x(g, &p, i) != i / 2;
        }
        check(bad == 0, "row %d Fit: the still sampled as before U16 (%d differ)", r, bad);
        bad = 0;
        for (int i = 0; i < p.w; i++) {
            int t = trx_hdmi_text_x(g, &p, i);
            bad += t != ((i >= 80 && i < 720) ? (i - 80) / 2 : -1);
        }
        for (int j = 0; j < p.h; j++) {
            int t = trx_hdmi_text_y(g, &p, j);
            bad += t != ((j >= 70 && j < 470) ? (j - 70) / 2 : -1);
        }
        check(bad == 0, "row %d Fit: the text area at window (40, 35), 2 x 2 per pixel, as before U16 (%d differ)", r, bad);
    }

    printf("-- the boot splash (trx_splash.h): every row's canvas, the picture's size, shape and place, the version line\n");
    trx_splash_pic_t pic;
    pic.w = trx_splash_w;
    pic.h = trx_splash_h;
    pic.x0 = trx_splash_x;
    pic.y0 = trx_splash_y;
    pic.pal = trx_splash_pal;
    pic.rle = trx_splash_rle;
    pic.len = trx_splash_len;
    std::vector<uint16_t> frame;
    check(splash_frame(&pic, frame), "the splash asset's runs cover its %d x %d pixels exactly", pic.w, pic.h);
    check(pic.w <= TRX_SPLASH_FRAME_W && pic.h <= TRX_SPLASH_FRAME_H && pic.x0 + pic.w <= TRX_SPLASH_FRAME_W &&
          pic.y0 + pic.h <= TRX_SPLASH_FRAME_H, "the asset lies inside the 960 x 540 frame");
    int max_splash = 0;
    for (int k = 0; k < 7; k++) {
        const splash_want &w = splash_wants[k];
        const trx_hdmi_geom_t *g = trx_hdmi_geom(w.row, 1);
        trx_splash_plan_t p = trx_splash_plan(g);
        printf("   row %d: active %4d x %4d -> canvas %3d x %3d at scale %d, stride %4d, %7d bytes; picture %3d x %3d at (%d, %2d)\n",
               w.row, g->hact, g->vact, p.c.w, p.c.h, p.c.s, p.c.stride, p.c.stride * p.c.h, p.pw, p.ph, p.px, p.py);
        if (p.c.stride * p.c.h > max_splash) {
            max_splash = p.c.stride * p.c.h;
        }
        for (int sc = 0; sc < 3; sc++) {
            trx_splash_plan_t q = trx_splash_plan(trx_hdmi_geom(w.row, sc));
            check(trx_splash_same(&p, &q), "row %d: the same splash at every scale (%d)", w.row, sc);
        }
        check(splash_faults(g, &p, w.dan, w.dad, true) == 0, "row %d: the splash layout", w.row);
        check(p.c.s == w.s && p.c.w == w.w && p.c.h == w.h && p.pw == w.pw && p.ph == w.ph && p.px == w.px && p.py == w.py,
              "row %d: canvas %d x %d at %d, picture %d x %d at (%d, %d) as worked out by hand", w.row, w.w, w.h, w.s, w.pw, w.ph,
              w.px, w.py);
        std::vector<uint16_t> cv;
        int d = splash_paint_vs_model(&p, &pic, &cv);
        check(d == 0, "row %d: the painted splash is the model's (%d pixels differ)", w.row, d);
        if (d != 0) {
            continue;
        }
        bool pal_only = true;
        for (size_t q = 0; q < cv.size(); q++) {
            bool in = false;
            for (int c = 0; c < 16; c++) {
                in |= cv[q] == pic.pal[c];
            }
            pal_only &= in;
        }
        check(pal_only, "row %d: only the picture's colours on the canvas", w.row);
        int tips = 0, lost = splash_runs_lost(&p, frame, cv, 2, &tips);
        check(lost == 0, "row %d: every stripe and gap of the picture is on the canvas (%d lines / columns lose one)", w.row, lost);
        printf("   row %d: %d single-pixel tips of the rounded bar ends left out by the shrink\n", w.row, tips);
        // the version line's box, bottom right, holds no pixel of the picture
        int vx0 = p.c.w - TRX_SPLASH_VERSION_MARGIN - TRX_SPLASH_VERSION_MAX * TRX_SPLASH_FONT_W;
        int vy0 = p.c.h - TRX_SPLASH_VERSION_MARGIN - TRX_SPLASH_FONT_H, lit = 0;
        for (int y = vy0; y < p.c.h - TRX_SPLASH_VERSION_MARGIN; y++) {
            for (int x = vx0; x < p.c.w - TRX_SPLASH_VERSION_MARGIN; x++) {
                lit += cv[y * (p.c.stride / 2) + x] != 0;
            }
        }
        check(vx0 >= TRX_SPLASH_VERSION_MARGIN && lit == 0, "row %d: the version line lies on black inside the canvas (%d picture pixels under it)",
              w.row, lit);
    }
    check(max_splash == TRX_LAYER_MAX_BYTES, "the largest splash canvas is 1,036,800 bytes (TRX_LV_BUF_BYTES), not %d", max_splash);

    printf("-- the splash at 1080p (and on every bitstream before U16, trx_ui.cc takes row 2): the layer and the pixels of before\n");
    for (int r = 2; r <= 8; r += 6) {
        trx_splash_plan_t p = trx_splash_plan(trx_hdmi_geom(r, 1));
        check(p.c.s == 2 && p.c.w == 960 && p.c.h == 540 && p.c.stride == 1920 && p.c.x0 == 0 && p.c.y0 == 0,
              "row %d: PX_SIZE 540/960, PX_STRIDE 1920, PX_POS 0, scale 2 as before U16", r);
        std::vector<uint16_t> before, now;
        splash_before_u16(&pic, before);
        splash_paint_vs_model(&p, &pic, &now);
        check(splash_differ(before, now) == 0, "row %d: the splash pixel for pixel as before U16", r);
    }

    printf("-- the paint with made-up pictures (offsets, odd sizes, 16 colours, runs of one pixel) on every row and on odd places\n");
    {
        uint32_t lcg = 12345;
        std::vector<trx_splash_plan_t> plans;
        for (int k = 0; k < 7; k++) {
            plans.push_back(trx_splash_plan(trx_hdmi_geom(splash_wants[k].row, 1)));
        }
        trx_splash_plan_t odd;
        odd.c.s = 1; odd.c.w = 400; odd.c.h = 220; odd.c.stride = 800; odd.c.x0 = odd.c.y0 = 0;
        odd.pw = 333; odd.ph = 187; odd.px = 17; odd.py = 9;
        plans.push_back(odd);
        trx_splash_plan_t big = odd;   // drawn larger than the frame: lines and columns shown twice
        big.c.w = 960; big.c.h = 600; big.c.stride = 1920; big.pw = 960; big.ph = 600; big.px = 0; big.py = 0;
        plans.push_back(big);
        static const int sizes[4][4] = { { 960, 540, 0, 0 }, { 300, 92, 330, 224 }, { 1, 1, 959, 539 }, { 517, 333, 3, 101 } };
        uint16_t pal[16];
        for (int c = 0; c < 16; c++) {
            pal[c] = uint16_t(c ? 0x1111 * c + 7 : 0);
        }
        for (int z = 0; z < 4; z++) {
            int w = sizes[z][0], h = sizes[z][1];
            std::vector<uint8_t> idx(size_t(w) * h);
            for (size_t q = 0; q < idx.size(); q++) {
                lcg = lcg * 1103515245u + 12345u;
                // mostly long runs, now and then a short one
                idx[q] = (q && ((lcg >> 16) % 23)) ? idx[q - 1] : uint8_t((lcg >> 8) & 15);
            }
            std::vector<uint8_t> rle = splash_encode(idx);
            trx_splash_pic_t mp;
            mp.w = w; mp.h = h; mp.x0 = sizes[z][2]; mp.y0 = sizes[z][3]; mp.pal = pal; mp.rle = &rle[0]; mp.len = uint32_t(rle.size());
            for (size_t k = 0; k < plans.size(); k++) {
                int d = splash_paint_vs_model(&plans[k], &mp, 0);
                check(d == 0, "picture %d x %d at (%d, %d) on a %d x %d canvas, %d x %d at (%d, %d): %d pixels differ from the model",
                      w, h, mp.x0, mp.y0, plans[k].c.w, plans[k].c.h, plans[k].pw, plans[k].ph, plans[k].px, plans[k].py, d);
            }
        }
    }

    printf("-- controls: wrong splashes the checks above must find\n");
    {
        // the canvas before U16 (960 x 540 at scale 2, the picture unshrunk) in every row
        int caught = 0, rows_off = 0;
        for (int k = 0; k < 7; k++) {
            const splash_want &w = splash_wants[k];
            const trx_hdmi_geom_t *g = trx_hdmi_geom(w.row, 1);
            trx_splash_plan_t old;
            old.c.s = 2; old.c.w = 960; old.c.h = 540; old.c.stride = 1920; old.c.x0 = old.c.y0 = 0;
            old.pw = 960; old.ph = 540; old.px = old.py = 0;
            bool is1080 = g->hact == 1920;
            rows_off += !is1080;
            caught += !is1080 && splash_faults(g, &old, w.dan, w.dad, false) > 0;
            check(!is1080 || splash_faults(g, &old, w.dan, w.dad, false) == 0, "control: the 1080p canvas is right at 1080p (row %d)", w.row);
        }
        check(caught == rows_off, "control: the 960 x 540 canvas of before U16 is caught in %d of the %d other rows", caught, rows_off);
        // 576p with its pixels taken as square: the picture 720 x 405, 9 % too wide on the screen
        const trx_hdmi_geom_t *g0 = trx_hdmi_geom(0, 1);
        trx_splash_plan_t sq = trx_splash_plan(g0);
        sq.ph = 405;
        sq.py = (576 - 405) / 2;
        check(splash_faults(g0, &sq, 4, 3, false) > 0, "control: 576p with square pixels (720 x 405) is caught by the shape check");
        // a picture not centred, a canvas a pixel short, a stride not of 16-byte steps
        trx_splash_plan_t m = trx_splash_plan(g0);
        m.py += 2;
        check(splash_faults(g0, &m, 4, 3, false) > 0, "control: the 576p picture two lines low is caught");
        m = trx_splash_plan(trx_hdmi_geom(9, 1));
        m.c.w = 799;
        m.c.stride = 1600;
        check(splash_faults(trx_hdmi_geom(9, 1), &m, 4, 3, false) > 0, "control: an 800 x 600 canvas of 799 pixels is caught");
        m = trx_splash_plan(trx_hdmi_geom(9, 1));
        m.c.stride = 1608;
        check(splash_faults(trx_hdmi_geom(9, 1), &m, 4, 3, false) > 0, "control: a stride of 1608 bytes is caught");
        // a broken paint: one pixel off is found by the model compare
        trx_splash_plan_t p6 = trx_splash_plan(trx_hdmi_geom(6, 1));
        std::vector<uint16_t> f, model, cv(size_t(p6.c.stride / 2) * p6.c.h);
        splash_frame(&pic, f);
        splash_model(&p6, f, model);
        trx_splash_paint(&cv[0], &p6, &pic);
        cv[(p6.py + 100) * (p6.c.stride / 2) + 300] ^= 0x0801;
        check(splash_differ(cv, model) == 1, "control: one pixel changed in the 480p splash is found");
        // a shrink to a quarter loses the picture's runs of 3 pixels: the look check finds it
        trx_splash_plan_t q = trx_splash_plan(trx_hdmi_geom(6, 1));
        q.pw = 240;
        q.ph = 135;
        q.px = (q.c.w - q.pw) / 2;
        q.py = (q.c.h - q.ph) / 2;
        std::vector<uint16_t> cq(size_t(q.c.stride / 2) * q.c.h);
        trx_splash_paint(&cq[0], &q, &pic);
        int tq = 0;
        check(splash_runs_lost(&q, frame, cq, 2, &tq) > 0, "control: the picture at a quarter of its size loses stripes, and the check says so");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
