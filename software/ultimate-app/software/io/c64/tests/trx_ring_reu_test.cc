// A08: when the ring stops and starts again as the REU enable changes (trx_ring_reu.h).
#include <stdio.h>
#include <string.h>
#include "../trx_ring_reu.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    // the menu's popup has its own short text; REST, RPC and trxmon keep the long one
    CHECK(strlen(TRX_REU_OFF_POPUP) <= 36);
    CHECK(strlen(TRX_REU_OFF_TEXT) > 40);
    CHECK(strstr(TRX_REU_OFF_TEXT, "A08") != NULL);
    TrxReuGate g = { false, false };
    // REU off, ring running: nothing to do
    CHECK(trx_reu_gate_step(&g, false, false, true) == TRX_REU_NONE && !g.blocked);
    // REU on: the ring stops, and is due back
    CHECK(trx_reu_gate_step(&g, true, false, true) == TRX_REU_STOP && g.blocked && g.resume);
    // still on, the log is off now: the gate does not act again and does not forget
    CHECK(trx_reu_gate_step(&g, true, false, false) == TRX_REU_NONE && g.blocked && g.resume);
    // off again: it starts over, once
    CHECK(trx_reu_gate_step(&g, false, false, false) == TRX_REU_START && !g.blocked);
    CHECK(trx_reu_gate_step(&g, false, false, true) == TRX_REU_NONE);

    // the user had stopped the ring (REST timewarp:stop): it stays stopped when the REU goes off
    memset(&g, 0, sizeof(g));
    CHECK(trx_reu_gate_step(&g, true, false, false) == TRX_REU_STOP && !g.resume);
    CHECK(trx_reu_gate_step(&g, false, false, false) == TRX_REU_NONE && !g.blocked);

    // a scrub or land holds the machine: the ring is left alone until it ends, both ways
    memset(&g, 0, sizeof(g));
    CHECK(trx_reu_gate_step(&g, true, true, true) == TRX_REU_NONE && !g.blocked);
    CHECK(trx_reu_gate_step(&g, true, false, true) == TRX_REU_STOP && g.blocked);
    CHECK(trx_reu_gate_step(&g, false, true, false) == TRX_REU_NONE && g.blocked);
    CHECK(trx_reu_gate_step(&g, false, false, false) == TRX_REU_START && !g.blocked);

    // REU on at boot, before the ring ever ran: the first start waits (the ring task sets resume), then it runs
    memset(&g, 0, sizeof(g));
    g.blocked = true;
    g.resume = true;
    CHECK(trx_reu_gate_step(&g, true, false, false) == TRX_REU_NONE);
    CHECK(trx_reu_gate_step(&g, false, false, false) == TRX_REU_START);

    // on, off, on, off quickly: one stop and one start each
    memset(&g, 0, sizeof(g));
    int stops = 0, starts = 0;
    bool log_on = true;
    for (int i = 0; i < 6; i++) {
        TrxReuAction a = trx_reu_gate_step(&g, (i & 1) == 0, false, log_on);
        if (a == TRX_REU_STOP) { stops++; log_on = false; }
        if (a == TRX_REU_START) { starts++; log_on = true; }
    }
    CHECK(stops == 3 && starts == 3 && log_on);

    printf("trx_ring_reu_test: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
