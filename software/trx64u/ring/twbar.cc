// Timewarp's bar (twbar.h): the firmware's tw_draw_bar and tw_layout (software/ultimate-app/software/io/c64/trx_video.cc :643-758, its 800 x 540 layer of
// 1080p) at the place of the base's copy (SDK 0.4.3 screens.cc build_timeline :919-977, the same bar on the canvas of 960 x 540). The firmware drew the
// texts with the C64 character ROM's lower-case set (trx_video.cc :706-725); here they are drawn with the 8 x 8 font of c64_basic's overlay, built in
// (twfont.h, made by twfont.py from designs/c64_basic/mem/ovl_font8.mem, ASCII order), the position twice as wide and high. Not api->overlay->text():
// the UI task writes those texts into the surface at its next frame, so they would be no part of this paint (and of no test on the build host).
#include "twbar.h"
#include "twfont.h"

#define TW_BAR_RGB 0x101018                      // the bar's colour (trx_video.cc TW_BAR_RGB)
// its RGB565 channels: 5, 6 and 5 bits, the low bits dropped as the base turns a palette's colour into RGB565 (video.cc still_colours)
#define TW_BAR_R5 ((TW_BAR_RGB >> 19) & 31)
#define TW_BAR_G6 ((TW_BAR_RGB >> 10) & 63)
#define TW_BAR_B5 ((TW_BAR_RGB >> 3) & 31)

// 8 x 8, bit 7 left: pause, back, forward, oldest, newest (trx_video.cc tw_glyphs, TW_GLYPH_*)
static const uint8_t tw_glyphs[5][8] = {
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00 },
    { 0x11, 0x33, 0x77, 0xFF, 0x77, 0x33, 0x11, 0x00 },
    { 0x88, 0xCC, 0xEE, 0xFF, 0xEE, 0xCC, 0x88, 0x00 },
    { 0x83, 0x87, 0x8F, 0x9F, 0x8F, 0x87, 0x83, 0x00 },
    { 0xC1, 0xE1, 0xF1, 0xF9, 0xF1, 0xE1, 0xC1, 0x00 },
};

TwRect tw_bar_rect(int cw, int ch)
{
    TwRect r;
    r.w = cw - 32 < 768 ? cw - 32 : 768;
    r.x = (cw - r.w) / 2;
    r.h = 48;
    r.y = ch - 64;
    return r;
}

// A rectangle in one colour, cut to the surface.
static void fill(uint16_t *px, int w, int h, int x, int y, int fw, int fh, uint16_t c)
{
    int x1 = x + fw, y1 = y + fh;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    x1 = x1 > w ? w : x1;
    y1 = y1 > h ? h : y1;
    for (int j = y; j < y1; j++) {
        uint16_t *p = px + j * w;
        for (int i = x; i < x1; i++) {
            p[i] = c;
        }
    }
}

// An 8 x 8 pattern at scale x scale pixels per bit; the bits that are 0 leave the pixels as they are.
static void glyph(uint16_t *px, int w, int h, int x, int y, const uint8_t g[8], int scale, uint16_t c)
{
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            if (g[j] & (0x80 >> i)) {
                fill(px, w, h, x + i * scale, y + j * scale, scale, scale, c);
            }
        }
    }
}

// The characters of s from x on, as many as end at `end` or before it (trx_video.cc tw_text: a character is drawn whole or not at all). A character
// outside ASCII 32 to 126 is a blank.
static void text(uint16_t *px, int w, int h, int x, int y, int end, const char *s, int scale, uint16_t c)
{
    static const uint8_t blank[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (; s && *s && x + 8 * scale <= end; s++) {
        const unsigned ch = (unsigned char)*s;
        glyph(px, w, h, x, y, ch >= 32 && ch <= 126 ? tw_font8[ch - 32] : blank, scale, c);
        x += 8 * scale;
    }
}

static int text_len(const char *s)
{
    int n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

void tw_bar_paint(uint16_t *px, int w, int h, int cw, int ch, const uint16_t pal[16], const TwBar *b)
{
    const TwRect l = tw_bar_rect(cw, ch);
    // the background: the frame under it, 1/4 of each channel, and 3/4 the bar's colour (the firmware's barmix)
    const int x0 = l.x < 0 ? 0 : l.x, x1 = l.x + l.w > w ? w : l.x + l.w;
    const int y0 = l.y < 0 ? 0 : l.y, y1 = l.y + l.h > h ? h : l.y + l.h;
    for (int j = y0; j < y1; j++) {
        uint16_t *p = px + j * w;
        for (int i = x0; i < x1; i++) {
            const unsigned s = p[i];
            const unsigned r = ((s >> 11) + 3 * TW_BAR_R5) >> 2, g = (((s >> 5) & 63) + 3 * TW_BAR_G6) >> 2, bl = ((s & 31) + 3 * TW_BAR_B5) >> 2;
            p[i] = (uint16_t)((r << 11) | (g << 5) | bl);
        }
    }
    // trx_video.cc tw_layout at the large scale 2: the glyph and the position on the row 8 below the top, the small texts 4 lower, the track 32 below the top
    const int end = l.x + l.w;
    const int info_x = l.x + 172, hint_end = l.x + l.w - 12;
    const int g = (b->glyph < 0 || b->glyph > 4) ? 0 : b->glyph;
    glyph(px, w, h, l.x + 12, l.y + 8, tw_glyphs[g], 2, pal[14]);
    text(px, w, h, l.x + 36, l.y + 8, end, b->pos, 2, pal[1]);
    text(px, w, h, info_x, l.y + 12, end, b->info, 1, b->warn ? pal[10] : pal[15]);
    if (b->hint) {
        const int hw = 8 * text_len(b->hint);
        text(px, w, h, hw < hint_end - info_x ? hint_end - hw : info_x, l.y + 12, end, b->hint, 1, pal[15]);
    }
    if (b->track_len > 0) {
        const int len = b->track_len > 1 ? b->track_len - 1 : 1;
        const int pos = b->track_pos < 0 ? 0 : (b->track_pos > len ? len : b->track_pos);
        const int tx = l.x + 12, tw = l.w - 24, ty = l.y + 32;
        const int cx = tx + (pos * (tw - 4)) / len;
        fill(px, w, h, tx, ty, cx - tx, 4, pal[12]);
        fill(px, w, h, cx, ty, tx + tw - cx, 4, pal[11]);
        fill(px, w, h, cx, ty - 3, 4, 10, pal[1]);
    }
}
