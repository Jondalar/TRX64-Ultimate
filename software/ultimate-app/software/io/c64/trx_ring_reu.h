// A08 (TRX64-Ultimate docs/specs/T20-delta-ring.md, "REU means no Timewarp"): the ring does not
// record REU contents, so while the REU is enabled there is no ring. This is the decision of when
// the ring stops and when it starts again, with no hardware in it (tests/trx_ring_reu_test.cc);
// trx_ring.cc acts on the answer.
#ifndef TRX_RING_REU_H
#define TRX_RING_REU_H

#define TRX_REU_OFF_TEXT "Timewarp is off while the REU is enabled (A08)"
// The menu's popup is 40 columns wide: its own short text (REST, RPC and trxmon keep the long one).
#define TRX_REU_OFF_POPUP "Timewarp is off while REU is on"
static_assert(sizeof(TRX_REU_OFF_POPUP) - 1 <= 36, "the popup text must fit the 40 column popup");

struct TrxReuGate {
    bool blocked;   // the ring has been stopped (or kept from starting) because the REU is on
    bool resume;    // the ring ran, or was due to start, when it was blocked: it starts again
};

enum TrxReuAction { TRX_REU_NONE, TRX_REU_STOP, TRX_REU_START };

// One look at the REU enable. scrubbing: a Timewarp scrub or a debugger land holds the machine; the
// ring is left alone until it ends (the poll looks again). log_on: the log runs now.
static inline TrxReuAction trx_reu_gate_step(TrxReuGate *g, bool reu_on, bool scrubbing, bool log_on)
{
    if (scrubbing) {
        return TRX_REU_NONE;
    }
    if (reu_on && !g->blocked) {
        g->blocked = true;
        g->resume = log_on;
        return TRX_REU_STOP;
    }
    if (!reu_on && g->blocked) {
        g->blocked = false;
        bool r = g->resume;
        g->resume = false;
        return r ? TRX_REU_START : TRX_REU_NONE;
    }
    return TRX_REU_NONE;
}

#endif
