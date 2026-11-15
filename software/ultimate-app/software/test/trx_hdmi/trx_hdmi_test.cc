// Host test of trx_hdmi.h (TRX64-Ultimate docs/specs/U16-hdmi-modes.md D5, D6): the mode the application asks for, and
// the freeze still's place and sampling on the pixel layer in every row and scale of the generated table.
// The scaler is modelled the way vic_scaler.v counts (one accumulator per axis, a step of hn / hd per output pixel,
// saturating at the window's end), not by the formula trx_hdmi.h uses, so the two are checked against each other.
#include <stdio.h>
#include <string.h>
#include "trx_hdmi.h"

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

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
