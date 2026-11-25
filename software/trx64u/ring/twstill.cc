// The picture under the base's Timewarp page (U18 T-3; MultiCore T256 D11, Alex's answer A of 2026-11-24): the ring library owns it. While the page shows,
// the library holds a surface of the still element's size, paints into it the frame the machine drew last (api->overlay->frame, SDK 0.4.3) and Timewarp's
// bar over it (twbar.cc), and shows it as the overlay's still; after each scrub step it paints both again and asks for the redraw (ringops.cc calls
// tw_still_scrubbed); when the page goes the element goes, and the surface is freed. The base takes its own still again from the frame drawn last
// where the machine stands on (api.md 4.17.10.3: the menu stays open, or it closes into Pause); while the library's still shows, the base holds none of
// its own (SDK 0.4.5, MultiCore T256 D14). The page's coming and going is the trx64u library's core hook menu_video, through the export "ringstill" (ring_still.h).
// Tasks: page() runs on the task that draws the menu, without the core lock (a call of remove() from there does not wait for that task); a scrub step
// runs where its operation runs, the executor with the core lock, for the menu's keys and for REST alike, so a REST call can meet a page that comes or
// goes: one mutex orders the two. frame() may wait for the base's grab lock inside it; nothing here holds that lock or waits for anything under it.
#include "ringsys.h"
#include "ring_still.h"
#include "ringops.h"
#include "twbar.h"

// The still element is the base's (its UI TOML's row "still": 960 x 540 at the top left, base_ui.toml); base/menu.toml has no [[element]] that replaces
// it. An element keeps its size on every canvas and is cut at the canvas (overlay.cc ovl_place_on), so the part of the surface a mode shows is its top
// left, the canvas cut to 960 x 540: 640 x 480 at 480p, 720 x 540 of 576p's 720 x 576, 800 x 540 of 800 x 600, all of it above (api.md 4.17.16, 6.16.4).
// The bar lies at the bottom of that part. A base without canvas() (before SDK 0.4.5) has the canvas of 960 x 540.
#define STILL_W 960
#define STILL_H 540
#define STILL_BYTES (STILL_W * STILL_H * 2)     // 1,036,800 bytes of the library's heap while the page shows (Makefile LIB_BUDGET)

static SvcMutex *g_mu;                          // made at the first page; the library's until its end (api.md 4.12.3)
static SvcSurface g_sf;                         // the surface while the page shows; px null otherwise
static int g_el;                                // the handle show() gave; 0: not shown
static int g_vw, g_vh;                          // the part of the surface the canvas showed at the last paint; 0 before the first paint of a surface
static uint16_t g_pal[16];                      // the bar's colours (RGB565): the person's palette as the page came, as the firmware's tw_begin took it

// The overlay group with frame() (SDK 0.4.3, a member after `error`), else null.
static const SvcOverlay *overlay(void)
{
    const SvcOverlay *o = LIB_API_COVERS(g_api, overlay) ? g_api->overlay : 0;
    return SVC_HAS(SvcOverlay, o, frame) ? o : 0;
}

// The part of the surface the canvas shows now (the comment above STILL_W): asked at every paint, as the canvas follows a change of the video mode.
static void shown_part(const SvcOverlay *o, int *w, int *h)
{
    int cw = STILL_W, ch = STILL_H;
    if (!SVC_HAS(SvcOverlay, o, canvas) || o->canvas(&cw, &ch) != 0) {
        cw = STILL_W;
        ch = STILL_H;
    }
    *w = cw < STILL_W ? cw : STILL_W;
    *h = ch < STILL_H ? ch : STILL_H;
}

// The paint, with g_mu held: the bar's rectangle black, the frame the machine drew last over the surface, the bar over the frame. frame() leaves what the
// picture does not cover as it was (api.md 4.17.14.2): under the bar that is black then, not the bar of the paint before, which would be mixed again. A
// canvas that changed since the last paint (a mode change while the page shows) clears the whole surface first, so no bar of the old place stays.
// Returns frame()'s answer; ms[0] the time of frame(), ms[1] the rest (the clears and the bar).
static int paint(const SvcOverlay *o, uint16_t ms[2])
{
    const uint16_t t0 = r_ms_timer();
    int vw, vh;
    shown_part(o, &vw, &vh);
    if (g_vw && (vw != g_vw || vh != g_vh)) {
        memset(g_sf.px, 0, STILL_BYTES);
    }
    g_vw = vw;
    g_vh = vh;
    const TwRect r = tw_bar_rect(vw, vh);
    for (int y = r.y; y < r.y + r.h; y++) {
        memset(g_sf.px + y * STILL_W + r.x, 0, size_t(r.w) * 2);
    }
    const uint16_t t1 = r_ms_timer();
    const int rc = o->frame("still", &g_sf);
    const uint16_t t2 = r_ms_timer();
    char pos[24], info[64];
    TwBar b;
    ringops_bar(&b, pos, sizeof(pos), info, sizeof(info));
    tw_bar_paint(g_sf.px, STILL_W, STILL_H, vw, vh, g_pal, &b);
    ms[0] = uint16_t(t2 - t1);
    ms[1] = uint16_t(r_ms_timer() - t2 + t1 - t0);
    return rc;
}

// The element goes (the surface is the library's again when remove() returns), then the surface. With g_mu held.
static void close_page(const SvcOverlay *o)
{
    if (g_el > 0) {
        const int rc = o->remove(g_el);
        if (rc) {
            r_log("[RING] Timewarp still: remove: %s", o->error(rc));
        }
        g_el = 0;
    }
    if (g_sf.px) {
        r_free(g_sf.px);
        g_sf.px = NULL;
    }
    g_vw = g_vh = 0;
}

// The surface (cleared once: black where the picture does not reach), the paint, the element. A page that comes while one is shown (menu_video says each
// change once, so it should not) is painted again. With g_mu held. Without memory, or when the base refuses the element, the page keeps the base's still.
static void open_page(const SvcOverlay *o, const uint8_t rgb[16][3])
{
    for (int i = 0; rgb && i < 16; i++) {
        g_pal[i] = uint16_t(((rgb[i][0] & 0xF8) << 8) | ((rgb[i][1] & 0xFC) << 3) | (rgb[i][2] >> 3));
    }
    if (!g_sf.px) {
        uint16_t *px = (uint16_t *)r_malloc(STILL_BYTES);
        if (!px) {
            r_log("[RING] Timewarp still: no memory for its %u bytes; the page keeps the base's still", (unsigned)STILL_BYTES);
            return;
        }
        memset(px, 0, STILL_BYTES);
        g_sf.px = px;
        g_sf.bytes = STILL_BYTES;
        g_sf.w = STILL_W;
        g_sf.h = STILL_H;
    }
    uint16_t ms[2];
    const int rc = paint(o, ms);
    if (g_el > 0) {
        o->update(g_el);
    } else {
        const int el = o->show("still", &g_sf);
        if (el <= 0) {
            r_log("[RING] Timewarp still: show: %s; the page keeps the base's still", o->error(el));
            close_page(o);
            return;
        }
        g_el = el;
    }
    r_log("[RING] Timewarp still shown (frame: %s), frame() %u ms, the bar %u ms", o->error(rc), (unsigned)ms[0], (unsigned)ms[1]);
}

// The export's member (ring_still.h): the page came (behind 1) or went (0).
static void page(int behind, const uint8_t rgb[16][3])
{
    const SvcOverlay *o = overlay();
    const SvcSync *s = g_api->sync;
    if (!o || !s) {
        r_log("[RING] Timewarp still: %s", o ? "the base has no sync group" : "the base has no api->overlay->frame (SDK 0.4.3)");
        return;
    }
    if (!g_mu && !(g_mu = s->mutex_new())) {
        r_log("[RING] Timewarp still: no memory for its mutex");
        return;
    }
    s->mutex_lock(g_mu, SYNC_FOREVER);
    if (behind) {
        open_page(o, rgb);
    } else {
        close_page(o);
    }
    s->mutex_unlock(g_mu);
}

void tw_still_scrubbed(void)
{
    const SvcSync *s = g_api->sync;
    SvcMutex *m = g_mu;
    if (!m || !s) {
        return;                                 // no page has come yet
    }
    s->mutex_lock(m, SYNC_FOREVER);
    const SvcOverlay *o = overlay();
    if (o && g_sf.px && g_el > 0) {
        uint16_t ms[2];
        const int rc = paint(o, ms);
        o->update(g_el);
        r_log("[RING] Timewarp still painted again (frame: %s), frame() %u ms, the bar %u ms", o->error(rc), (unsigned)ms[0], (unsigned)ms[1]);
    }
    s->mutex_unlock(m);
}

// The export "ringstill" (lib_ring.cc).
static const RingStill s_still = { sizeof(RingStill), page };

const void *ring_still_table(uint32_t *size)
{
    *size = sizeof(s_still);
    return &s_still;
}
