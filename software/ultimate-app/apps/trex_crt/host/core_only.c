/*
 * core_only.c - T95 S5: the cartridge cores without any view.
 *
 * This binary links cart_base, cart_flash, cart_fmt, cart_port, cart_port_u2p, cart_dumper, cart_writer and
 * cart_view against the mock. cart_view_text.c, cart_screen.c, ui_shim.c and main.c are NOT linked: if a core
 * ever reaches for a window, a key code or an SDK dialog again, this link fails.
 *
 * It then does the work headless - probe, scan, write, verify - and compares the flash with the CRT, so the
 * proof is not just that it links but that the operations run with no view installed at all.
 */

#define _GNU_SOURCE

#include "cart_base.h"
#include "cart_dumper.h"
#include "cart_port.h"
#include "cart_view.h"
#include "cart_writer.h"
#include "crtbuf.h"
#include "mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// main.c is the app's entry point and is not part of this link; the platform filler needs the ABI pointer
const u2p_api_t *g_api;

static int fails, checks;
#define CHECK(cond, ...)                                                                                       \
    do {                                                                                                       \
        checks++;                                                                                              \
        if (!(cond)) {                                                                                         \
            fails++;                                                                                           \
            printf("FAIL: " __VA_ARGS__);                                                                       \
            printf("\n");                                                                                      \
        }                                                                                                      \
    } while (0)

static char root[256];

// The same machine the harness's scenarios run on (harness.c gideon_cfg/trx_cfg)
static void machine(cart_t *c, bool trx, bool from_dump)
{
    mock_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trx = trx;
    cfg.has_slot = trx;
    cfg.armed = trx;
    cfg.ext_mask = trx ? 15 : 0;
    cfg.pref = trx ? 0 : 1;
    cfg.has_media_reset = true;
    cfg.prog_pdir = 0x2F;
    cfg.prog_pout = 0x35;
    cfg.app_ultimax = trx || from_dump; // the freeze left Ultimax on; a Gideon write starts in normal mode
    cfg.max_accesses = 60000000;
    mock_init(c, &cfg);
    g_api = mock_api();
    cart_port_set(cart_port_platform());
    core_detect();
}

int main(void)
{
    char tmpl[] = "/tmp/trexcoreXXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(root, sizeof(root), "%s", tmpl);
    snprintf(mock_fsroot, sizeof(mock_fsroot), "%s", root);
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/crt", root);
    mkdir(dir, 0755);
    crt_root(root);
    make_write_crt("megabyter", "/crt/megabyter.crt");

    CHECK(g_view == NULL, "a view is installed in the core-only link");

    // ---- the dumper: probe and scan, headless ----
    {
        cart_t *c = cart_megabyter();
        cart_fill_pattern(c, 1);
        machine(c, false, true);
        static cart_dumper_t d;
        cart_dumper_open(&d);
        CHECK(d.probe_result.hw_type == 86, "probe says type %d, want 86", d.probe_result.hw_type);
        CHECK(d.det_banks == 128, "probe found %d banks, want 128", d.det_banks);
        cart_dumper_scan(&d);
        // Every bank the model holds data in must stand as filled, every erased one as empty
        int wrong = 0;
        for (int b = 0; b < d.det_banks; b++) {
            bool has_data = false;
            for (int i = 0; i < 0x2000 && !has_data; i++)
                has_data = c->bank_byte(c, b, 0, i) != 0xFF;
            if (d.bank_status_lo[b] != (has_data ? BANK_FILLED : BANK_EMPTY))
                wrong++;
        }
        CHECK(wrong == 0, "%d banks of the map do not match the cartridge", wrong);
        cart_dumper_close(&d);
        CHECK(mock.accesses > 0, "the headless dumper touched the bus %ld times", mock.accesses);
        CHECK(mock.popups == 0, "the core asked for %d dialogs with no view", mock.popups);
        cart_free(c);
    }

    // ---- the writer: write and verify a CRT, headless ----
    {
        cart_t *c = cart_megabyter();
        cart_fill_pattern(c, 7); // a cartridge with a game on it: the writer erases it first
        machine(c, true, false);
        static cart_writer_t w;
        cart_writer_open(&w, "/crt", "megabyter.crt");
        CHECK(w.chip != NULL, "the writer found no flash cart");
        CHECK(w.chip && w.chip->crt_type == 86, "the writer took the wrong type");
        CHECK(w.crt_num_chips == 3, "the CRT summary says %d chips, want 3", w.crt_num_chips);
        cart_writer_write(&w);
        cart_writer_verify(&w);

        // The three banks make_write_crt() wrote (seed 300 + bank), the rest erased
        static const int banks[] = { 0, 1, 127 };
        uint8_t want[0x2000];
        int bad = 0;
        for (int b = 0; b < 128; b++) {
            int i_bank = -1;
            for (int i = 0; i < 3; i++)
                if (banks[i] == b)
                    i_bank = i;
            if (i_bank < 0) {
                memset(want, 0xFF, sizeof(want));
            } else {
                sparse(want, 300u + (unsigned)b, 1);
            }
            for (int i = 0; i < 0x2000; i++)
                if (c->bank_byte(c, b, 0, i) != want[i])
                    bad++;
        }
        CHECK(bad == 0, "%d flash bytes differ from the CRT after a headless write", bad);
        CHECK(mock.unguarded_rom_writes == 0, "%ld ROM writes without the lock", mock.unguarded_rom_writes);
        CHECK(mock.popups == 0, "the core asked for %d dialogs with no view", mock.popups);
        cart_writer_close(&w);
        cart_free(c);
    }

    printf("%s: %d checks, %d failures (core without a view)\n", fails ? "FAILED" : "PASS", checks, fails);
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd)) {
    }
    return fails ? 1 : 0;
}
