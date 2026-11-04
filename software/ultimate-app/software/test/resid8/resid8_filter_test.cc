// Host test of resid8_filter.h: what the reSID8 menu's filter / resonance / digi choices mean for the hardware
// (TRX64-Ultimate docs/specs/T25.5-resid8-menus.md section 3).
#include <stdio.h>
#include <string.h>
#include "resid8_filter.h"

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    printf("%s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

// the value strings of the menu, in the order of resid8_filter_sel[] of u64_config.cc
static const char *names[RS8_N_FILTERS] = { "Chip Model", "6581 Follin", "6581 Galway", "6581 Average", "6581 Strong",
                                            "6581 Extreme", "6581 Custom", "8580" };

int main(int, char **)
{
    printf("-- the default is the original reDIP\n");
    Resid8Plan d = resid8_plan(0, 0, 0, 111, 222);
    check(d.src == RS8_SRC_MODEL, "Chip Model selects the chip model's own filter");
    check(d.base == 245 && d.offset == -780, "its 6581 parameters are reDIP's (245, -780)");
    check(d.auto_bits == 3, "resonance and digis Auto are on at the defaults");

    printf("-- only Dag's eight values (no tables of Gideon's)\n");
    check(RS8_N_FILTERS == 8, "the Filter Curve list has eight values, 0-7");

    printf("-- the 6581 curve fits and the 8580\n");
    static const struct { int idx, base, offset; } fits[5] = {
        { 1, 245, -780 }, { 2, 280, -405 }, { 3, 250, 0 }, { 4, 260, 400 }, { 5, 200, 760 } };
    for (int i = 0; i < 5; i++) {
        Resid8Plan p = resid8_plan(fits[i].idx, 0, 0, 0, 0);
        char what[80];
        snprintf(what, sizeof what, "%s: own 6581 curve, base %d offset %d", names[fits[i].idx], fits[i].base, fits[i].offset);
        check(p.src == RS8_SRC_6581 && p.base == fits[i].base && p.offset == fits[i].offset, what);
    }
    check(resid8_plan(7, 0, 0, 0, 0).src == RS8_SRC_8580, "8580 selects the own 8580 filter");
    Resid8Plan c = resid8_plan(RS8_FILTER_CUSTOM, 0, 0, 301, -17);
    check(c.src == RS8_SRC_6581 && c.base == 301 && c.offset == -17, "6581 Custom takes base and offset from the items");
    Resid8Plan nc = resid8_plan(2, 0, 0, 301, -17);
    check(nc.base == 280 && nc.offset == -405, "a preset ignores the Custom items");

    printf("-- the parameter ranges of the hardware\n");
    bool inrange = true;
    for (int i = 0; i < RS8_N_FILTERS; i++) {
        inrange = inrange && resid8_presets[i].base >= 0 && resid8_presets[i].base <= 511 &&
                  resid8_presets[i].offset >= -1024 && resid8_presets[i].offset <= 1023;
    }
    check(inrange, "every preset fits 9 bit base, 11 bit signed offset");

    printf("-- resonance and digis\n");
    check(resid8_plan(0, 0, 3, 0, 0).auto_bits == 1, "resonance Auto, digis explicit: auto bits 1");
    check(resid8_plan(0, 2, 0, 0, 0).auto_bits == 2, "resonance explicit, digis Auto: auto bits 2");
    check(resid8_plan(0, 1, 1, 0, 0).auto_bits == 0, "both explicit: no auto bit");
    check(resid8_plan(0, 1, 3, 0, 0).res_reg == 0 && resid8_plan(0, 2, 3, 0, 0).res_reg == 1, "Low is 0, High is 1");
    int ok = 1;
    for (int v = 1; v <= 4; v++) {
        ok = ok && resid8_plan(0, 1, v, 0, 0).digi_reg == v - 1;
    }
    check(ok, "Off, Low, Medium, High are 0 .. 3");

    printf("-- Filter On / Off\n");
    check(resid8_plan(0, 0, 0, 0, 0).bypass == 0, "the default (Filter On) does not bypass");
    check(resid8_plan(0, 0, 0, 0, 0, 1).bypass == 0, "Filter On does not bypass");
    check(resid8_plan(0, 0, 0, 0, 0, 0).bypass == 1, "Filter Off bypasses");
    Resid8Plan off = resid8_plan(7, 2, 3, 0, 0, 0);
    check(off.bypass == 1 && off.src == RS8_SRC_8580, "Off keeps the curve choice (it applies again when the filter is On)");

    printf("-- a value out of range falls back to the default\n");
    check(resid8_plan(99, 0, 0, 0, 0).src == RS8_SRC_MODEL, "an unknown filter index is the chip model's filter");
    check(resid8_plan(8, 0, 0, 0, 0).src == RS8_SRC_MODEL, "8 (the first old table value) is out of range: the chip model's filter");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
