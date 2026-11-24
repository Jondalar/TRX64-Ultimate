// Timewarp's bar, painted into the picture under the Timewarp page (U18 T-3; MultiCore T256 D3, D11: the bar is the ring library's, the base only shows
// the surface). The firmware's tw_draw_bar (software/ultimate-app/software/io/c64/trx_video.cc) on the surface the ring library shows as the still
// (twstill.cc): its box, glyph, texts and track, at the place the base's own copy of it used (screens.cc build_timeline of SDK 0.4.3, 0.4.4). twbar.cc calls
// nothing of the base, so a test on the build host compiles it alone. C and C++.
#ifndef RING_TWBAR_H
#define RING_TWBAR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the bar shows: the state values tw.glyph, tw.pos, tw.info and tw.track of ringops.cc (ringops_bar) and the page's hint.
typedef struct TwBar {
    int glyph;                    // 0 pause, 1 back, 2 forward, 3 oldest, 4 newest; any other value is pause
    const char *pos;              // the position, in the large text
    const char *info;             // after the position, small
    const char *hint;             // small, ending at the bar's right inner edge; null: none
    bool warn;                    // info in colour 10, else 15
    int track_pos, track_len;     // the entry shown and the entries of the timeline; track_len 0: no track
} TwBar;

typedef struct TwRect {
    int x, y, w, h;
} TwRect;

// The bar on a canvas of cw x ch pixels: 768 wide (less when the canvas is narrower than 800), centred, 48 high, its top 64 above the canvas's bottom;
// on the canvas of 960 x 540, x 96, y 476.
TwRect tw_bar_rect(int cw, int ch);

// Paints the bar into px, a surface of w x h RGB565 pixels (stride w) that holds the frame as it lies on the canvas (surface pixel = canvas pixel, as the
// still element lies at the canvas's top left). Under the bar's rectangle each pixel becomes 1/4 itself and 3/4 the bar's colour 0x101018, per channel in
// RGB565 (the firmware's barmix); over that the glyph (colour 14), the position at twice the size (colour 1), info (10 or 15), the hint (15) and the track
// (12 up to the entry, 11 after it, the marker 1), in the colours pal[] (RGB565). Nothing outside the surface is written.
void tw_bar_paint(uint16_t *px, int w, int h, int cw, int ch, const uint16_t pal[16], const TwBar *b);

#ifdef __cplusplus
}
#endif

#endif
