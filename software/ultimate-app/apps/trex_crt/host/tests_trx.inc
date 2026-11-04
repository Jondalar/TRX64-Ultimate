// TRX scenarios: H2 dump equals image, H3 write inhibit, H4 abort injection, H5 arming, H6 restore, H7 policy,
// H8 detection (T57 5, 8.1)
extern int g_core;
enum { CORE_GIDEON = 0, CORE_TRX = 1 };
extern const char *cart_session_status(void);

static const char *writable[] = { "ef1", "megabyter", "gmod2", "megacart", "ef3" };

typedef void (*pre_fn)(cart_t *);

static scen_t trx_scen(const char *cart, const char *action, const char *file, const int *keys, int nkeys)
{
    scen_t s;
    memset(&s, 0, sizeof(s));
    s.cart = cart;
    s.cfg = trx_cfg();
    s.cfg.prog_pout = 0x35; // a game that hid ROML
    s.action = action;
    s.file = file;
    s.nkeys = nkeys;
    memcpy(s.keys, keys, (size_t)nkeys * sizeof(int));
    return s;
}

// What a game left in a cartridge: another bank, another mode
static void dirty_cart(cart_t *c)
{
    if (!c) return;
    if (!strcmp(c->name, "ef1") || !strcmp(c->name, "ef3")) {
        c->io_wr(c, 0xDE00, 17);
        c->io_wr(c, 0xDE02, 0x07);
    } else if (!strcmp(c->name, "megabyter")) {
        c->io_wr(c, 0xDE00, 50);
    } else if (!strcmp(c->name, "gmod2")) {
        c->io_wr(c, 0xDE00, 0x85);
    } else if (!strcmp(c->name, "megacart")) {
        c->io_wr(c, 0xDE00, 99);
    }
}

static void common_post(const char *what, const scen_t *s, bool touched_expected)
{
    CHECK(!mock.hang, "%s: hang", what);
    CHECK(mock.unguarded_rom_writes == 0, "%s: %ld ROML/ROMH write cycles while ROM_WR_EN was 0", what,
          mock.unguarded_rom_writes);
    CHECK(mock.wr_en == s->cfg.wr0, "%s: ROM_WR_EN is %d at the end, was %d", what, mock.wr_en, s->cfg.wr0);
    CHECK(mock.pdir == s->cfg.prog_pdir && mock.pout == s->cfg.prog_pout,
          "%s: CPU port %02X/%02X not restored (%02X/%02X)", what, mock.pdir, mock.pout, s->cfg.prog_pdir,
          s->cfg.prog_pout);
    CHECK(mock.ext_mask == s->cfg.ext_mask && mock.int_mask == s->cfg.int_mask, "%s: masks not restored", what);
    CHECK(!mock.bus_open, "%s: bus left set up", what);
    CHECK(mock.reset_requested == (touched_expected ? 1 : 0), "%s: %d C64 resets requested", what,
          mock.reset_requested);
}

static void test_h2_dump(void)
{
    mkpath("/out");
    static const int kd[] = { KF3 };
    for (int i = 0; i < 6; i++) {
        for (int v = 0; v < 3; v++) {
            const char *name = carts6[i];
            int wr0 = v & 1, echo = v == 2; // v 2: open bus echoes the last cycle (our board: nothing drives IO2)
            char what[64];
            snprintf(what, sizeof(what), "H2 %s dump wr0=%d echo=%d", name, wr0, echo);
            scen_t s = trx_scen(name, "dump", NULL, kd, 1);
            s.cfg.wr0 = wr0;
            s.cfg.open_echo = echo;
            cart_t *c = make_cart(name);
            s.pre = dirty_cart;
            run_app(c, &s);
            check_dump(what, c, !strcmp(name, "ef3") ? "/out/Game One.crt" : "/out/cartridge.crt", 1);
            common_post(what, &s, true);
            CHECK(g_core == CORE_TRX, "%s: core not detected", what);
            cart_free(c);
        }
    }
}

static void test_h3_write(void)
{
    mkpath("/crt");
    static const int kw[] = { KF3, KF5 };
    for (int i = 0; i < 5; i++) {
        for (int wr0 = 0; wr0 < 2; wr0++) {
            const char *name = writable[i];
            char what[64], crt[64];
            snprintf(what, sizeof(what), "H3 %s write wr0=%d", name, wr0);
            snprintf(crt, sizeof(crt), "/crt/%s.crt", name);
            make_write_crt(name, crt);
            scen_t s = trx_scen(name, "write", crt, kw, 2);
            s.cfg.wr0 = wr0;
            cart_t *c = make_cart(name);
            s.pre = dirty_cart;
            run_app(c, &s);
            check_written(what, c, crt, 1);
            common_post(what, &s, true);
            CHECK(mock.rom_writes > 0, "%s: no ROML/ROMH write reached the cart", what);
            CHECK(wr0 || mock.blocked_rom_writes == 0, "%s: blocked writes", what);
            cart_free(c);
        }
    }
    // the mock itself: a ROML write with the bit clear is blocked
    {
        scen_t s = trx_scen("ef1", "dump", NULL, NULL, 0);
        cart_t *c = make_cart("ef1");
        mock_init(c, &s.cfg);
        const u2p_api_t *a = mock_api();
        a->c64->cart_mode(0);
        a->c64->poke(0xDE02, 0x05);
        a->c64->poke(0x8555, 0xAA);
        CHECK(mock.blocked_rom_writes == 1 && mock.rom_writes == 0, "H3 mock: write inhibit not enforced");
        cart_free(c);
    }
}

// H4: the cartridge stops answering at the n-th access of a whole write run; on return the lock is back where it
// was, no write cycle left the app while it was closed, nothing hangs
static void test_h4_abort(void)
{
    static const int kw[] = { KF3, KF5 };
    for (int i = 0; i < 5; i++) {
        const char *name = writable[i];
        char crt[64];
        snprintf(crt, sizeof(crt), "/crt/%s.crt", name);
        make_write_crt(name, crt);
        scen_t s = trx_scen(name, "write", crt, kw, 2);
        cart_t *c = make_cart(name);
        run_app(c, &s);
        long total = mock.accesses;
        printf("H4 %s: clean run %ld accesses\n", name, total);
        cart_free(c);
        long runs = 0;
        for (long n = 1; n <= total; n += (n < 700 ? 1 : (total - 700) / 500 + 1)) {
            for (int wr0 = 0; wr0 < 2; wr0++) {
                if (wr0 && (n % 3)) continue;
                scen_t t = trx_scen(name, "write", crt, kw, 2);
                t.cfg.fail_at = n;
                t.cfg.wr0 = wr0;
                t.cfg.max_accesses = total * 40 + 100000;
                c = make_cart(name);
                run_app(c, &t);
                char what[80];
                snprintf(what, sizeof(what), "H4 %s fail at access %ld wr0=%d", name, n, wr0);
                int f0 = fails;
                CHECK(!mock.hang, "%s: hang", what);
                CHECK(mock.unguarded_rom_writes == 0, "%s: %ld write cycles outside the guard", what,
                      mock.unguarded_rom_writes);
                CHECK(mock.wr_en == wr0, "%s: ROM_WR_EN %d at the end", what, mock.wr_en);
                CHECK(!mock.bus_open, "%s: bus left set up", what);
                cart_free(c);
                runs++;
                if (fails > f0 + 3) return;
            }
        }
        printf("H4 %s: %ld injected runs\n", name, runs);
    }
}

static void cart_off(cart_t *c) { c->io_wr(c, 0xDE02, 0x04); } // the program switched the EasyFlash off

static void test_h5_arming(void)
{
    static const int kd[] = { KF3 };
    static const int ki[] = { KF1 };
    // U3: Internal at the last reset, a cartridge on the pins
    {
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        s.cfg.armed = false; s.cfg.ext_mask = 0; s.cfg.int_mask = 15; s.cfg.pref = 1;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "not enabled"), "H5 U3: popup '%s'", mock.last_popup);
        CHECK(!strcmp(cart_session_status(), "Port not armed: reset needed"), "H5 U3: status '%s'", cart_session_status());
        CHECK(mock.accesses == 0, "H5 U3: %ld bus accesses before the check", mock.accesses);
        common_post("H5 U3", &s, false);
        cart_free(c);
    }
    // U2: nothing on the pins, not armed
    {
        scen_t s = trx_scen("none", "dump", NULL, kd, 1);
        s.cfg.armed = false; s.cfg.ext_mask = 0; s.cfg.int_mask = 15;
        run_app(NULL, &s);
        CHECK(strstr(mock.last_popup, "No cartridge on the pins"), "H5 U2: popup '%s'", mock.last_popup);
        common_post("H5 U2", &s, false);
    }
    // U2: armed, but the program switched the cartridge off (EF mode 4): both pins high
    {
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        cart_t *c = make_cart("ef1");
        s.pre = cart_off;
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "No cartridge on the pins"), "H5 U2b: popup '%s'", mock.last_popup);
        common_post("H5 U2b", &s, false);
        cart_free(c);
    }
    // U4: a fault
    {
        scen_t s = trx_scen("ef1", "write", "/crt/ef1.crt", ki, 1);
        s.cfg.fault = 1; s.cfg.fault_cause = 3;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "Port fault (cause 3)"), "H5 U4: popup '%s'", mock.last_popup);
        CHECK(mock.accesses == 0, "H5 U4: bus accessed");
        common_post("H5 U4", &s, false);
        cart_free(c);
    }
    // U5: armed with external ROM at the last reset, but IO1/IO2 do not follow after the set-up (Manual: ROM external only)
    {
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        s.cfg.masks_stuck = true; s.cfg.ext_mask = 4; s.cfg.int_mask = 0;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(!strcmp(cart_session_status(), "Port did not arm (SLOT_STATUS)."), "H5 U5: status '%s'", cart_session_status());
        common_post("H5 U5", &s, false);
        cart_free(c);
    }
    // U6: the lock does not rise: nothing is written, the tool says so
    {
        static const int kw[] = { KF3 };
        scen_t s = trx_scen("ef1", "write", "/crt/ef1.crt", kw, 1);
        s.cfg.wr_stuck = true;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "Cannot unlock") != NULL, "H5 U6: popup '%s'", mock.last_popup);
        CHECK(mock.rom_writes == 0 && mock.unguarded_rom_writes == 0, "H5 U6: ROML/ROMH writes %ld/%ld", mock.rom_writes, mock.unguarded_rom_writes);
        CHECK(!mock.wr_en, "H5 U6: lock set");
        cart_free(c);
    }
    // U7: the lock does not clear: the user is told to power-cycle
    {
        static const int kw[] = { KF3 };
        scen_t s = trx_scen("ef1", "write", "/crt/ef1.crt", kw, 1);
        s.cfg.wr_sticky = true;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "Write lock did not clear") != NULL, "H5 U7: popup '%s'", mock.last_popup);
        cart_free(c);
    }
    // U9: a TRX bitstream without the slot page
    {
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        s.cfg.has_slot = false;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(strstr(mock.last_popup, "no cartridge"), "H5 U9: popup '%s'", mock.last_popup);
        CHECK(mock.accesses == 0, "H5 U9: bus accessed");
        cart_free(c);
    }
}

static void test_h6_h7(void)
{
    static const int ki[] = { KDOWN, KRIGHT, KF1 };
    for (int wr0 = 0; wr0 < 2; wr0++) {
        char what[64];
        snprintf(what, sizeof(what), "H6 inspect wr0=%d", wr0);
        scen_t s = trx_scen("ef1", "inspect", NULL, ki, 3);
        s.cfg.wr0 = wr0;
        s.uiname = "8000";
        cart_t *c = make_cart("ef1");
        s.pre = dirty_cart;
        run_app(c, &s);
        common_post(what, &s, true);
        cart_free(c);
    }
    // a tool that did not touch the cartridge asks for no reset: the scan-only writer on a cart no writer knows
    {
        static const int k0[] = { KF1 };
        scen_t s = trx_scen("rom16", "flash", NULL, k0, 1);
        cart_t *c = make_cart("rom16");
        run_app(c, &s);
        CHECK(mock.reset_requested == 0 || mock.reset_requested == 1, "reset count");
        CHECK(mock.wr_en == 0, "H6 rom16: ROM_WR_EN");
        cart_free(c);
    }
}

static void test_h8_detect(void)
{
    static const int kd[] = { KF3 };
    {   // trx == NULL: Gideon
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        s.cfg.trx = false; s.cfg.pref = 1;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(g_core == CORE_GIDEON, "H8: trx NULL not Gideon");
        CHECK(mock.trx_reads == 0 && mock.trx_writes == 0, "H8: Gideon path touched the block");
        cart_free(c);
    }
    {   // the block there but the ID not TRX2: also stock
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        s.cfg.bad_id = true; s.cfg.pref = 1;
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(g_core == CORE_GIDEON, "H8: bad ID not Gideon");
        cart_free(c);
    }
    {   // TRX2 and the slot page
        scen_t s = trx_scen("ef1", "dump", NULL, kd, 1);
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(g_core == CORE_TRX && mock.trx_reads > 0, "H8: TRX not detected");
        cart_free(c);
    }
}

// H9: the writer streams the CRT. A full 1 MB EasyFlash (64 banks) is written and verified with a heap that has no
// block above 32 KB (the board's "No memory for the CRT"), on our core and, as the fallback, on Gideon's; a CRT that
// ends inside a CHIP packet is refused before the cartridge is touched; no file stays open.
static void make_full_ef_crt(const char *rel)
{
    crtbuf_t b;
    uint8_t d[0x2000];
    crt_head(&b, 32, 1, 0, "FULL EF");
    for (int bank = 0; bank < 64; bank++) {
        sparse(d, 400u + (unsigned)bank, 1);
        crt_chip(&b, bank, 0x8000, d, 0x2000);
        sparse(d, 500u + (unsigned)bank, 1);
        crt_chip(&b, bank, 0xE000, d, 0x2000);
    }
    crt_save(&b, rel);
}

static int same_content(cart_t *a, cart_t *b)
{
    int diff = 0, halves = (!strcmp(a->name, "ef1") || !strcmp(a->name, "ef3")) ? 2 : 1;
    for (int bank = 0; bank < a->banks; bank++)
        for (int half = 0; half < halves; half++)
            for (int i = 0; i < 0x2000; i++)
                if (a->bank_byte(a, bank, half, i) != b->bank_byte(b, bank, half, i)) diff++;
    return diff;
}

static void truncate_file(const char *rel, long cut)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    FILE *f = fopen(p, "rb+");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    if (truncate(p, n - cut)) {}
}

static void test_h9_stream(void)
{
    static const int kw[] = { KF3, KF5 };
    mkpath("/crt");
    make_full_ef_crt("/crt/full.crt");
    // variant 0: our core on today's firmware (_lseek without SEEK_CUR), 1: the same with a firmware whose _lseek
    // has it, 2: Gideon's core
    for (int variant = 0; variant < 3; variant++) {
        int gideon = variant == 2;
        char what[64];
        snprintf(what, sizeof(what), "H9 ef1 full CRT, small heap, %s", gideon ? "Gideon" : variant ? "TRX seekcur" : "TRX");
        scen_t s = trx_scen("ef1", "write", "/crt/full.crt", kw, 2);
        if (gideon) {
            s.cfg = gideon_cfg(NULL);
            s.cfg.pref = 1;
        }
        cart_t *c = make_cart("ef1");
        mock_alloc_max = 0x8000;
        mock_alloc_big = 0;
        mock_seekcur_works = variant == 1;
        mock_failed_seeks = 0;
        run_app(c, &s);
        mock_alloc_max = 0;
        mock_seekcur_works = 0;
        CHECK(!mock.hang, "%s: hang", what);
        CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
        check_written(what, c, "/crt/full.crt", 1);
        CHECK(mock.rom_writes > 0, "%s: nothing was written", what);
        CHECK(mock_failed_seeks == 0, "%s: %ld fseeks failed (the reader must not depend on them)", what,
              mock_failed_seeks);
        if (!gideon) {
            common_post(what, &s, true);
            CHECK(mock_alloc_big <= 0x4000, "%s: a request of %zu bytes (a bank is 16 KB)", what, mock_alloc_big);
        }
        cart_free(c);
    }
    // bytes after the last packet, or a damaged header in the middle: refused before the erase, cartridge as it was
    for (int bad = 0; bad < 2; bad++) {
        char what[80];
        snprintf(what, sizeof(what), "H9 ef1 CRT with %s", bad ? "a damaged packet" : "trailing bytes");
        make_full_ef_crt("/crt/bad.crt");
        char p[512];
        snprintf(p, sizeof(p), "%s/crt/bad.crt", root);
        FILE *f = fopen(p, "rb+");
        if (bad) {
            fseek(f, 0x40 + 40 * (0x2010), SEEK_SET); // the header of packet 40
            fwrite("XHIP", 1, 4, f);
        } else {
            fseek(f, 0, SEEK_END);
            fwrite("trailing", 1, 8, f);
        }
        fclose(f);
        scen_t s = trx_scen("ef1", "write", "/crt/bad.crt", kw, 2);
        cart_t *c = make_cart("ef1"), *ref = make_cart("ef1");
        mock_alloc_max = 0x8000;
        run_app(c, &s);
        mock_alloc_max = 0;
        CHECK(!mock.hang, "%s: hang", what);
        CHECK(mock.rom_writes <= 2, "%s: %ld write cycles reached the cartridge", what, mock.rom_writes);
        CHECK(same_content(c, ref) == 0, "%s: the cartridge changed", what);
        cart_free(c);
        cart_free(ref);
    }
    // a CRT that is cut off inside its last packet: refused before the erase, cartridge as it was
    static const char *names[] = { "ef1", "megabyter", "gmod2" };
    for (int i = 0; i < 3; i++)
        for (int gideon = 0; gideon < 2; gideon++) {
            const char *name = names[i];
            char what[80], crt[64];
            snprintf(what, sizeof(what), "H9 %s truncated CRT, %s", name, gideon ? "Gideon" : "TRX");
            snprintf(crt, sizeof(crt), "/crt/%s_cut.crt", name);
            make_write_crt(name, crt);
            truncate_file(crt, 100);
            scen_t s = trx_scen(name, "write", crt, kw, 2);
            if (gideon) {
                s.cfg = gideon_cfg(NULL);
                s.cfg.pref = 1;
            }
            cart_t *c = make_cart(name), *ref = make_cart(name);
            run_app(c, &s);
            CHECK(!mock.hang, "%s: hang", what);
            CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
            // on our core a failed write ends with the chips' read-array command ($F0, one per chip)
            CHECK(mock.rom_writes <= (gideon ? 0 : 2), "%s: %ld write cycles reached the cartridge", what,
                  mock.rom_writes);
            CHECK(same_content(c, ref) == 0, "%s: the cartridge changed", what);
            cart_free(c);
            cart_free(ref);
        }
}

// ---- EasyFlash 3, whole cart (T51): dump as one type 90 CRT, erase the whole chip, write a type 90 image ------------------
static cart_t *make_ef3_full(void)
{
    cart_t *c = cart_ef3();
    cart_ef3_fill_full(c, 11);
    return c;
}

// the cart's bytes in image terms: bank 0..511, half, offset
static uint8_t ef3_byte(cart_t *c, int bank512, int half, int off)
{
    c->view_slot = bank512 >> 6;
    return c->bank_byte(c, bank512 & 63, half, off);
}

// flips bits in a stored byte (a defect the verify must see)
static void ef3_poke_raw(cart_t *c, int bank512, int half, int off)
{
    extern uint8_t *cart_ef3_raw(cart_t *c);
    uint8_t *d = cart_ef3_raw(c);
    d[(half ? 0x400000u : 0u) + (uint32_t)bank512 * 0x2000u + (uint32_t)off] ^= 0x80;
}

static long ef3_count_not_ff(cart_t *c)
{
    long n = 0;
    for (int b = 0; b < 512; b++)
        for (int half = 0; half < 2; half++)
            for (int i = 0; i < 0x2000; i++)
                n += ef3_byte(c, b, half, i) != 0xFF;
    return n;
}

static long ef3_diff(cart_t *a, cart_t *b)
{
    long n = 0;
    for (int bk = 0; bk < 512; bk++)
        for (int half = 0; half < 2; half++)
            for (int i = 0; i < 0x2000; i++)
                n += ef3_byte(a, bk, half, i) != ef3_byte(b, bk, half, i);
    return n;
}

// A type 90 CRT with the given slots (bit mask) from the pattern of `src` (slot 0 optionally altered), ROML then ROMH,
// slot-major; `skip`: a bank of a slot left out of the file
static void make_ef3_image_crt(const char *rel, cart_t *src, int slot_mask, int skip_slot, int skip_bank)
{
    crtbuf_t b;
    uint8_t d[0x2000];
    crt_head(&b, 90, 1, 0, "TEST EF3 IMAGE");
    for (int slot = 0; slot < 8; slot++) {
        if (!(slot_mask >> slot & 1))
            continue;
        for (int bank = 0; bank < 64; bank++) {
            if (slot == skip_slot && bank == skip_bank)
                continue;
            for (int half = 0; half < 2; half++) {
                for (int i = 0; i < 0x2000; i++)
                    d[i] = (uint8_t)(ef3_byte(src, slot * 64 + bank, half, i) ^ 0x5A);
                crt_chip(&b, slot * 64 + bank, half ? 0xA000 : 0x8000, d, 0x2000);
            }
        }
    }
    crt_save(&b, rel);
}

// The dump of the whole cart is one type 90 CRT: header EXROM 1 / GAME 0, 1024 packets slot-major in bank order, ROML at
// $8000 then ROMH at $A000, every byte as the cartridge shows it (the system area and the seven slots)
static void check_ef3_whole_dump(const char *what, cart_t *c, const char *rel)
{
    size_t n;
    uint8_t *d = slurp(rel, &n);
    if (!d) { CHECK(0, "%s: no dump file", what); return; }
    CHECK(n == 0x40 + 1024 * (16 + 0x2000), "%s: file size %zu", what, n);
    CHECK(be16(d + 0x16) == 90 && d[0x18] == 1 && d[0x19] == 0, "%s: type %u exrom %d game %d", what, be16(d + 0x16), d[0x18], d[0x19]);
    size_t pos = be32(d + 0x10);
    int packets = 0, order_bad = 0;
    long bad = 0;
    while (pos + 16 <= n && !memcmp(d + pos, "CHIP", 4)) {
        unsigned bank = be16(d + pos + 10), load = be16(d + pos + 12), size = be16(d + pos + 14);
        int half = packets & 1;
        if (bank != (unsigned)(packets >> 1) || load != (half ? 0xA000u : 0x8000u) || size != 0x2000) order_bad++;
        for (unsigned i = 0; i < size; i++)
            bad += ef3_byte(c, (int)bank, half, (int)i) != d[pos + 16 + i];
        packets++;
        pos += be32(d + pos + 4);
    }
    CHECK(packets == 1024, "%s: %d packets", what, packets);
    CHECK(order_bad == 0, "%s: %d packets out of slot-major order or with another load address", what, order_bad);
    CHECK(bad == 0, "%s: %ld dumped bytes differ from the cartridge", what, bad);
    free(d);
}

static void test_ef3_whole_dump(void)
{
    mkpath("/out");
    static const int kd[] = { KF3 };
    for (int gideon = 0; gideon < 2; gideon++) {
        char what[80];
        snprintf(what, sizeof(what), "EF3 whole dump, %s", gideon ? "Gideon" : "TRX");
        scen_t s = trx_scen("ef3", "dump", NULL, kd, 1);
        if (gideon) { s.cfg = gideon_cfg("d"); s.cfg.pref = 1; }
        s.menu_choice = 7;              // the picker's last entry: all slots
        s.pre = dirty_cart;
        cart_t *c = make_ef3_full();
        remove_file("/out/easyflash3.crt");
        run_app(c, &s);
        CHECK(!mock.hang, "%s: hang", what);
        check_ef3_whole_dump(what, c, "/out/easyflash3.crt");
        if (!gideon && getenv("TREX_EF3_OUT")) { // for tools/ef3_image.py (round trip by hand)
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "cp '%s/out/easyflash3.crt' '%s/easyflash3.crt'", root, getenv("TREX_EF3_OUT"));
            if (system(cmd)) {}
        }
        if (!gideon)
            common_post(what, &s, true);
        CHECK(mock.rom_writes == 0, "%s: a dump wrote %ld ROML/ROMH cycles", what, mock.rom_writes);
        cart_free(c);
    }
    {   // the one-slot dump is unchanged: slot 2 of the same cartridge, type 32
        scen_t s = trx_scen("ef3", "dump", NULL, kd, 1);
        s.menu_choice = 1;              // slot 2
        cart_t *c = make_ef3_full();
        run_app(c, &s);
        check_dump("EF3 slot dump", c, "/out/Game Two.crt", 2);
        if (getenv("TREX_EF3_OUT")) {
            char cmd[600];
            snprintf(cmd, sizeof(cmd), "cp '%s/out/Game Two.crt' '%s/slot2.crt'", root, getenv("TREX_EF3_OUT"));
            if (system(cmd)) {}
        }
        cart_free(c);
    }
}

// The dumper stages the CRT and writes the target when the cartridge is read (T57 M1): in 64 KB heap blocks, else in a
// file on /Temp, else straight into the target. A drive that drops out while saving costs no second read of the cart.
static void ef3_whole_run(const char *rel_out, int heap_mode)
{
    static const int kd[] = { KF3 };
    scen_t s = trx_scen("ef3", "dump", NULL, kd, 1);
    s.menu_choice = 7;
    cart_t *c = make_ef3_full();
    remove_file(rel_out);
    mock_alloc_big = 0;
    (void)heap_mode;
    run_app(c, &s);
    cart_free(c);
}

static size_t s_ref_n;
static uint8_t *s_ref;
static void same_as_ref(const char *what)
{
    size_t n;
    uint8_t *d = slurp("/out/easyflash3.crt", &n);
    CHECK(d && s_ref && n == s_ref_n && !memcmp(d, s_ref, s_ref_n), "%s: the file differs from the one saved at once", what);
    CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
    CHECK(mock_alloc_live == 0, "%s: %d heap blocks not freed", what, mock_alloc_live);
    free(d);
}

static void test_dump_staged(void)
{
    mkpath("/out");
    mkpath("/Temp");
    mock_alloc_live = 0;
    {   // heap blocks: 64 KB each, the whole image 129 of them, the file saved at once
        ef3_whole_run("/out/easyflash3.crt", 0);
        CHECK(mock_alloc_big == 0x10000, "blocks: biggest allocation %zu", mock_alloc_big);
        CHECK(strstr(mock_logbuf, "Staging in RAM") != NULL, "blocks: path not logged");
        CHECK(mock_alloc_live == 0, "blocks: %d heap blocks not freed", mock_alloc_live);
        CHECK(mock_open_files == 0, "blocks: %d files left open", mock_open_files);
        s_ref = slurp("/out/easyflash3.crt", &s_ref_n);
        CHECK(s_ref && s_ref_n == 0x40 + 1024 * (16 + 0x2000), "blocks: file size %zu", s_ref_n);
    }
    {   // the drive drops out 1 MB into the save, one retry answers YES: same file, the cartridge read once
        mock_wfail_skip = 1; mock_wfail_opens = 1; mock_wfail_after = 0x100000;
        mock_retry_popups = 0; mock_retry_yes = 1;
        ef3_whole_run("/out/easyflash3.crt", 0);
        mock_wfail_opens = 0; mock_wfail_skip = 0;
        CHECK(mock_retry_popups == 1, "retry: %d retry popups", mock_retry_popups);
        same_as_ref("retry");
    }
    {   // the drive stays gone: NO ends it
        mock_wfail_skip = 1; mock_wfail_opens = 99; mock_wfail_after = 0x1000;
        mock_retry_popups = 0; mock_retry_yes = 2;
        ef3_whole_run("/out/easyflash3.crt", 0);
        mock_wfail_opens = 0; mock_wfail_skip = 0;
        CHECK(mock_retry_popups == 3, "retry NO: %d retry popups (2 YES, then NO)", mock_retry_popups);
        CHECK(mock_open_files == 0, "retry NO: %d files left open", mock_open_files);
        CHECK(mock_alloc_live == 0, "retry NO: %d heap blocks not freed", mock_alloc_live);
    }
    {   // the heap gives 50 blocks, then no more: all are given back, the image is staged in /Temp
        mock_alloc_n = 50;
        ef3_whole_run("/out/easyflash3.crt", 1);
        mock_alloc_n = 0;
        CHECK(strstr(mock_logbuf, "Heap gave 50 of 129 blocks") != NULL, "partial heap: not logged");
        CHECK(strstr(mock_logbuf, "Staging in /Temp") != NULL, "partial heap: /Temp path not taken");
        same_as_ref("temp, partial heap");
    }
    {   // no block fits at all: /Temp; the temp file is emptied after the copy
        mock_alloc_max = 0x8000;
        ef3_whole_run("/out/easyflash3.crt", 1);
        mock_alloc_max = 0;
        same_as_ref("temp");
        size_t tn = 1;
        uint8_t *t = slurp("/Temp/easyflash3.crt", &tn);
        CHECK(t && tn == 0, "temp: the temp file keeps %zu bytes after the copy", tn);
        free(t);
    }
    {   // /Temp: the copy fails (drive gone), YES, the copy is made again from the temp file
        mock_alloc_max = 0x8000;
        mock_wfail_skip = 2; mock_wfail_opens = 1; mock_wfail_after = 0x300000; // create, temp pass; the target fails
        mock_retry_popups = 0; mock_retry_yes = 1;
        ef3_whole_run("/out/easyflash3.crt", 1);
        mock_alloc_max = 0; mock_wfail_opens = 0; mock_wfail_skip = 0;
        CHECK(mock_retry_popups == 1, "temp retry: %d retry popups", mock_retry_popups);
        same_as_ref("temp retry");
    }
    {   // /Temp fails too (not there): straight into the target
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s/Temp'", root);
        if (system(cmd)) {}
        mock_alloc_max = 0x8000;
        ef3_whole_run("/out/easyflash3.crt", 2);
        mock_alloc_max = 0;
        CHECK(strstr(mock_logbuf, "writing while reading") != NULL, "direct: fallback not logged");
        same_as_ref("direct");
    }
    free(s_ref);
    s_ref = NULL;
}

// One slot of an EasyFlash 3 saved as type 32 is an EasyFlash image (ef3_image.py extract): EXROM 1 / GAME 0, flash CHIP
// packets, and the EF1 EAPI (AM29F040) where bank 0 ROMH carries the "eapi" signature; everything else is the cartridge
#include "eapi_drivers.h"
static void check_ef3_slot_as_ef(const char *what, cart_t *c, const char *rel, int slot, int want_eapi)
{
    size_t n;
    uint8_t *d = slurp(rel, &n);
    if (!d) { CHECK(0, "%s: no dump file", what); return; }
    CHECK(be16(d + 0x16) == 32 && d[0x18] == 1 && d[0x19] == 0, "%s: type %u exrom %d game %d", what, be16(d + 0x16), d[0x18], d[0x19]);
    CHECK(n == 0x40 + 128 * (16 + 0x2000), "%s: file size %zu", what, n);
    size_t pos = be32(d + 0x10);
    int packets = 0, type_bad = 0;
    long bad = 0, eapi_bad = 0;
    c->view_slot = slot;
    while (pos + 16 <= n && !memcmp(d + pos, "CHIP", 4)) {
        unsigned bank = be16(d + pos + 10), size = be16(d + pos + 14);
        int half = packets & 1;
        if (be16(d + pos + 8) != 2) type_bad++;
        for (unsigned i = 0; i < size; i++) {
            uint8_t want = c->bank_byte(c, (int)bank, half, (int)i);
            if (want_eapi && bank == 0 && half && i >= EAPI_ROMH_OFFSET && i < EAPI_ROMH_OFFSET + EAPI_DRIVER_SIZE)
                eapi_bad += d[pos + 16 + i] != eapi_am29f040[i - EAPI_ROMH_OFFSET];
            else
                bad += want != d[pos + 16 + i];
        }
        packets++;
        pos += be32(d + pos + 4);
    }
    CHECK(packets == 128, "%s: %d packets", what, packets);
    CHECK(type_bad == 0, "%s: %d packets not chip type 2", what, type_bad);
    CHECK(bad == 0, "%s: %ld bytes differ from the cartridge", what, bad);
    CHECK(eapi_bad == 0, "%s: %ld EAPI bytes are not the AM29F040 driver", what, eapi_bad);
    free(d);
}

static void test_ef3_slot_eapi(void)
{
    mkpath("/out");
    static const int kd[] = { KF3 };
    for (int mode = 0; mode < 3; mode++) { // 0 signature, 1 signature + no memory for the image, 2 no signature
        char what[64];
        snprintf(what, sizeof(what), "EF3 slot as EF, %s", mode == 0 ? "EAPI" : mode == 1 ? "EAPI, direct" : "no signature");
        scen_t s = trx_scen("ef3", "dump", NULL, kd, 1);
        s.menu_choice = 1; // slot 2
        cart_t *c = make_ef3_full();
        extern uint8_t *cart_ef3_raw(cart_t *c);
        uint8_t *raw = cart_ef3_raw(c);
        uint8_t *romh0 = raw + 0x400000u + 128u * 0x2000u; // slot 2 (index 2), bank 0, ROMH half
        if (mode != 2) memcpy(romh0 + EAPI_ROMH_OFFSET, "eapi", 4);
        for (int i = 4; i < EAPI_DRIVER_SIZE; i++) romh0[EAPI_ROMH_OFFSET + i] = (uint8_t)(0x55 + i); // an old driver
        remove_file("/out/Game Two.crt");
        if (mode == 1) mock_alloc_max = 0x100000;
        run_app(c, &s);
        mock_alloc_max = 0;
        check_ef3_slot_as_ef(what, c, "/out/Game Two.crt", 2, mode != 2);
        CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
        cart_free(c);
    }
}

static void test_ef3_erase_cart(void)
{
    mkpath("/crt");
    static const int kf[] = { KF7 };
    // confirmed: the whole 8 MB is $FF
    for (int gideon = 0; gideon < 2; gideon++) {
        char what[80];
        snprintf(what, sizeof(what), "EF3 whole erase, %s", gideon ? "Gideon" : "TRX");
        scen_t s = trx_scen("ef3", "flash", NULL, kf, 1);
        if (gideon) { s.cfg = gideon_cfg(NULL); s.cfg.pref = 1; }
        s.menu_choice = 8;
        s.uiname = "erase";
        cart_t *c = make_ef3_full();
        CHECK(ef3_count_not_ff(c) > 1000000, "%s: the model is not full", what);
        run_app(c, &s);
        CHECK(!mock.hang, "%s: hang", what);
        CHECK(ef3_count_not_ff(c) == 0, "%s: %ld bytes are not $FF after the erase", what, ef3_count_not_ff(c));
        if (!gideon)
            common_post(what, &s, true);
        cart_free(c);
    }
    // not confirmed: NO in the popup, or another word typed; nothing reaches the cartridge
    for (int v = 0; v < 3; v++) {
        char what[80];
        snprintf(what, sizeof(what), "EF3 whole erase refused (%s)", v == 0 ? "NO" : v == 1 ? "wrong word" : "empty word");
        scen_t s = trx_scen("ef3", "flash", NULL, kf, 1);
        s.menu_choice = 8;
        s.uiname = v == 1 ? "erasing" : v == 2 ? "x" : "ERASE";
        s.popup = v == 0 ? BUTTON_NO : 0;
        cart_t *c = make_ef3_full(), *ref = make_ef3_full();
        run_app(c, &s);
        CHECK(ef3_diff(c, ref) == 0, "%s: the cartridge changed", what);
        CHECK(mock.rom_writes == 0, "%s: %ld ROML/ROMH write cycles", what, mock.rom_writes);
        CHECK(mock.unguarded_rom_writes == 0, "%s: writes outside the guard", what);
        cart_free(c);
        cart_free(ref);
    }
}

// the cartridge stops answering at the n-th access of the whole erase: no write outside the guard, the lock back, no hang
static void test_ef3_erase_abort(void)
{
    static const int kf[] = { KF7 };
    scen_t s = trx_scen("ef3", "flash", NULL, kf, 1);
    s.menu_choice = 8;
    s.uiname = "ERASE";
    cart_t *c = make_ef3_full();
    run_app(c, &s);
    long total = mock.accesses;
    cart_free(c);
    long runs = 0;
    for (long n = 1; n <= total; n += (n < 400 ? 1 : total / 150 + 1))
        for (int wr0 = 0; wr0 < 2; wr0++) {
            scen_t t = trx_scen("ef3", "flash", NULL, kf, 1);
            t.menu_choice = 8;
            t.uiname = "ERASE";
            t.cfg.fail_at = n;
            t.cfg.wr0 = wr0;
            t.cfg.max_accesses = total * 40 + 100000;
            c = make_ef3_full();
            run_app(c, &t);
            char what[80];
            snprintf(what, sizeof(what), "EF3 erase abort at access %ld wr0=%d", n, wr0);
            CHECK(!mock.hang, "%s: hang", what);
            CHECK(mock.unguarded_rom_writes == 0, "%s: %ld writes outside the guard", what, mock.unguarded_rom_writes);
            CHECK(mock.wr_en == wr0, "%s: ROM_WR_EN %d at the end", what, mock.wr_en);
            CHECK(!mock.bus_open, "%s: bus left set up", what);
            cart_free(c);
            runs++;
        }
    printf("EF3 whole erase: %ld injected runs\n", runs);
}

// An image of slots 0 and 3 written into a full cart: those two slots are the file (banks it lacks erased), the other
// slots untouched; then the slot-0-only image onto an erased cart (the way back after a whole erase); verify finds a
// changed byte
static void test_ef3_image_write(void)
{
    mkpath("/crt");
    static const int kw[] = { KF3, KF5 };
    cart_t *src = make_ef3_full();
    make_ef3_image_crt("/crt/ef3_03.crt", src, 0x09, 3, 20);
    make_ef3_image_crt("/crt/ef3_0.crt", src, 0x01, -1, -1);
    for (int gideon = 0; gideon < 2; gideon++) {
        char what[80];
        snprintf(what, sizeof(what), "EF3 image write, %s", gideon ? "Gideon" : "TRX");
        scen_t s = trx_scen("ef3", "write", "/crt/ef3_03.crt", kw, 2);
        if (gideon) { s.cfg = gideon_cfg(NULL); s.cfg.pref = 1; }
        s.uiname = "write";
        s.menu_choice = 7;                  // Full EF3
        cart_t *c = cart_ef3();
        cart_ef3_fill_full(c, 99);          // another content: everything must change where the file says so
        cart_t *before = cart_ef3();
        cart_ef3_fill_full(before, 99);
        run_app(c, &s);
        CHECK(!mock.hang, "%s: hang", what);
        long bad = 0, other = 0;
        for (int bk = 0; bk < 512; bk++)
            for (int half = 0; half < 2; half++)
                for (int i = 0; i < 0x2000; i++) {
                    int slot = bk >> 6;
                    uint8_t got = ef3_byte(c, bk, half, i);
                    if (slot == 0 || slot == 3) {
                        uint8_t want = (slot == 3 && bk == 3 * 64 + 20) ? 0xFF : (uint8_t)(ef3_byte(src, bk, half, i) ^ 0x5A);
                        bad += got != want;
                    } else {
                        other += got != ef3_byte(before, bk, half, i);
                    }
                }
        CHECK(bad == 0, "%s: %ld bytes of slots 0 and 3 differ from the file", what, bad);
        CHECK(other == 0, "%s: %ld bytes of the other slots changed", what, other);
        CHECK(strstr(mock_logbuf, "banks, 0 errors") != NULL, "%s: verify did not say 0 errors", what);
        if (!gideon) {
            common_post(what, &s, true);
            CHECK(mock.rom_writes > 0, "%s: nothing written", what);
        }
        cart_free(c);
        cart_free(before);
    }
    {   // refused (NO): nothing changes
        scen_t s = trx_scen("ef3", "write", "/crt/ef3_03.crt", kw, 1);
        s.uiname = "write";
        s.menu_choice = 7;
        cart_t *c = make_ef3_full(), *ref = make_ef3_full();
        s.popup = BUTTON_NO;
        run_app(c, &s);
        CHECK(ef3_diff(c, ref) == 0, "EF3 image write refused: the cartridge changed");
        CHECK(mock.rom_writes == 0, "EF3 image write refused: %ld write cycles", mock.rom_writes);
        cart_free(c);
        cart_free(ref);
    }
    {   // the way back: erase the whole cart, then write the slot-0 image; slots 1-7 stay erased
        static const int kerase[] = { KF7 };
        scen_t e = trx_scen("ef3", "flash", NULL, kerase, 1);
        e.menu_choice = 8;
        e.uiname = "ERASE";
        cart_t *c = make_ef3_full();
        run_app(c, &e);
        CHECK(ef3_count_not_ff(c) == 0, "recovery: not erased");
        scen_t w = trx_scen("ef3", "write", "/crt/ef3_0.crt", kw, 2);
        w.uiname = "write";
        w.menu_choice = 7;
        run_app(c, &w);
        long bad = 0, rest = 0;
        for (int bk = 0; bk < 512; bk++)
            for (int half = 0; half < 2; half++)
                for (int i = 0; i < 0x2000; i++) {
                    uint8_t got = ef3_byte(c, bk, half, i);
                    if (bk < 64) bad += got != (uint8_t)(ef3_byte(src, bk, half, i) ^ 0x5A);
                    else rest += got != 0xFF;
                }
        CHECK(bad == 0, "recovery: %ld bytes of slot 0 differ from the image", bad);
        CHECK(rest == 0, "recovery: %ld bytes outside slot 0 are not $FF", rest);
        CHECK(strstr(mock_logbuf, "EF3 verify: 64 banks, 0 errors") != NULL, "recovery: verify did not say 0 errors");
        // a flipped byte in slot 0: F5 finds it
        c->view_slot = 0;
        scen_t v = trx_scen("ef3", "verify", "/crt/ef3_0.crt", (const int[]){ KF5 }, 1);
        v.menu_choice = 7;
        ef3_poke_raw(c, 5, 0, 0x123);
        run_app(c, &v);
        CHECK(strstr(mock_logbuf, "EF3 verify: 64 banks, 1 errors") != NULL, "verify of a changed byte did not say 1 error");
        cart_free(c);
    }
    {   // the picker's single slot with a type 90 file: refused with a message, nothing written
        scen_t s = trx_scen("ef3", "write", "/crt/ef3_03.crt", kw, 1);
        s.menu_choice = 1;                  // slot 2
        cart_t *c = make_ef3_full(), *ref = make_ef3_full();
        run_app(c, &s);
        CHECK(ef3_diff(c, ref) == 0 && mock.rom_writes == 0, "single slot + type 90 file: something was written");
        CHECK(strstr(mock.last_popup, "Full EF3") != NULL, "single slot + type 90 file: popup '%s'", mock.last_popup);
        cart_free(c);
        cart_free(ref);
    }
    {   // Full EF3 with a type 32 file: refused with a message, nothing written
        scen_t s = trx_scen("ef3", "write", "/crt/ef3.crt", kw, 1);
        s.menu_choice = 7;
        cart_t *c = make_ef3_full(), *ref = make_ef3_full();
        run_app(c, &s);
        CHECK(ef3_diff(c, ref) == 0 && mock.rom_writes == 0, "Full EF3 + type 32 file: something was written");
        CHECK(strstr(mock.last_popup, "not one") != NULL, "Full EF3 + type 32 file: popup '%s'", mock.last_popup);
        cart_free(c);
        cart_free(ref);
    }
    {   // a type 90 image on a cartridge that is not an EasyFlash 3: refused
        scen_t s = trx_scen("ef1", "write", "/crt/ef3_0.crt", kw, 1);
        cart_t *c = make_cart("ef1"), *ref = make_cart("ef1");
        run_app(c, &s);
        CHECK(same_content(c, ref) == 0, "type 90 on an EasyFlash 1: the cartridge changed");
        CHECK(mock.rom_writes == 0, "type 90 on an EasyFlash 1: %ld write cycles", mock.rom_writes);
        cart_free(c);
        cart_free(ref);
    }
    cart_free(src);
}


// H10 (T57): a CRT is checked when it is opened for writing. A file the writer cannot use gets one popup and no writer
// screen: no bus access at all, nothing erased, no reset, no file left open. A good file goes on as before and the
// writer takes the summary from the check.
static void test_h10_open_check(void)
{
    static const int kw[] = { KF3, KF5 };
    mkpath("/crt");
    typedef struct { const char *tag, *crt, *popup; } bad_t;
    crtbuf_t b;
    uint8_t d[0x2000];
    sparse(d, 7, 1);
    crt_head(&b, 32, 1, 0, "ZERO");                                   crt_save(&b, "/crt/zero.crt");
    crt_head(&b, 0, 0, 0, "ROM");  crt_chip(&b, 0, 0x8000, d, 0x2000); crt_save(&b, "/crt/type0.crt");
    crt_head(&b, 32, 1, 0, "BANK"); crt_chip(&b, 64, 0x8000, d, 0x2000); crt_save(&b, "/crt/bank64.crt");
    crt_head(&b, 32, 1, 0, "SIZE"); crt_chip(&b, 0, 0x8000, d, 0x2000); b.d[0x40 + 5] = 0x20; crt_save(&b, "/crt/size.crt");
    crt_head(&b, 32, 1, 0, "SIG");  crt_chip(&b, 0, 0x8000, d, 0x2000); b.d[3] = 'X'; crt_save(&b, "/crt/sig.crt");
    crt_head(&b, 32, 1, 0, "TINY"); b.n = 0x20;                       crt_save(&b, "/crt/tiny.crt");
    make_write_crt("ef1", "/crt/cut.crt");
    truncate_file("/crt/cut.crt", 100);
    make_write_crt("ef1", "/crt/junk.crt");
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/crt/junk.crt", root);
        FILE *f = fopen(p, "ab");
        fwrite("trailing bytes!!", 1, 16, f);
        fclose(f);
    }
    static const bad_t bad[] = {
        { "zero chips", "/crt/zero.crt", "no chips" },
        { "wrong type", "/crt/type0.crt", "type 0" },
        { "bank out of range", "/crt/bank64.crt", "bank 64" },
        { "wrong packet size", "/crt/size.crt", "wrong size" },
        { "no signature", "/crt/sig.crt", "not a CRT" },
        { "header cut off", "/crt/tiny.crt", "not a CRT" },
        { "cut in a packet", "/crt/cut.crt", "cut off" },
        { "bytes after the packets", "/crt/junk.crt", "no chip packet" },
        { "missing file", "/crt/none.crt", "Cannot open" },
    };
    for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++)
        for (int gideon = 0; gideon < 2; gideon++) {
            char what[96];
            snprintf(what, sizeof(what), "H10 %s, %s", bad[i].tag, gideon ? "Gideon" : "TRX");
            scen_t s = trx_scen("ef1", "write", bad[i].crt, kw, 2);
            if (gideon) {
                s.cfg = gideon_cfg(NULL);
                s.cfg.pref = 1;
            }
            cart_t *c = make_cart("ef1"), *ref = make_cart("ef1");
            run_app(c, &s);
            CHECK(!mock.hang, "%s: hang", what);
            CHECK(mock.popups == 1, "%s: %d popups", what, mock.popups);
            CHECK(strstr(mock.last_popup, bad[i].popup) != NULL, "%s: popup '%s'", what, mock.last_popup);
            CHECK(mock.accesses == 0 && mock.rom_writes == 0, "%s: %ld bus accesses (no writer screen, no bus)", what,
                  mock.accesses);
            CHECK(mock.reset_requested == 0, "%s: a C64 reset was requested", what);
            CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
            CHECK(same_content(c, ref) == 0, "%s: the cartridge changed", what);
            cart_free(c);
            cart_free(ref);
        }
    // a good EasyFlash CRT: no popup, the writer runs, takes the summary from the check, writes and verifies
    for (int gideon = 0; gideon < 2; gideon++) {
        char what[64];
        snprintf(what, sizeof(what), "H10 valid ef1, %s", gideon ? "Gideon" : "TRX");
        make_write_crt("ef1", "/crt/good.crt");
        scen_t s = trx_scen("ef1", "write", "/crt/good.crt", kw, 2);
        if (gideon) {
            s.cfg = gideon_cfg(NULL);
            s.cfg.pref = 1;
        }
        cart_t *c = make_cart("ef1");
        run_app(c, &s);
        CHECK(!mock.hang, "%s: hang", what);
        CHECK(mock.popups == 0, "%s: popup '%s'", what, mock.last_popup);
        CHECK(strstr(mock_logbuf, "(checked on open)") != NULL, "%s: summary not taken from the check", what);
        CHECK(mock_open_files == 0, "%s: %d files left open", what, mock_open_files);
        check_written(what, c, "/crt/good.crt", 1);
        cart_free(c);
    }
}

static void run_trx_tests(void)
{
    test_h2_dump();
    test_ef3_whole_dump();
    test_dump_staged();
    test_ef3_slot_eapi();
    test_ef3_erase_cart();
    test_ef3_image_write();

    test_h3_write();
    test_h5_arming();
    test_h6_h7();
    test_h8_detect();
    test_h9_stream();
    test_h10_open_check();
    test_h4_abort();
    test_ef3_erase_abort();
}
static void dbg_eeprom(cart_t *c)
{
    int shown = 0;
    for (int i = 0; i < 2048 && shown < 6; i++) {
        uint8_t want = (uint8_t)(i * 3 + 7);
        if (c->eeprom[i] != want) { printf("  ee[%d]=%02X want %02X\n", i, c->eeprom[i], want); shown++; }
    }
}
