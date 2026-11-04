// SPDX-License-Identifier: GPL-3.0-or-later
// T25.1 SID engine: software reference model (M1). See sidref.h for the
// provenance of each part. The digital part follows libresidfp
// (GPL-2.0-or-later) function by function; names in comments refer to it.
#include "sidref.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace trxsid {

// EnvelopeGenerator::adsrtable (libresidfp, from kevtris' die analysis)
const uint16_t ADSR_TABLE[16] = {
    0x007f, 0x3000, 0x1e00, 0x0660, 0x0182, 0x5573, 0x000e, 0x3805,
    0x2424, 0x2220, 0x090c, 0x0ecd, 0x010e, 0x23f7, 0x5237, 0x64a8};

// 1024/Q. "Low": reSID 0.16's 1/(0.707 + res/15); "High": reSID 1.0's MOS
// 8580 table, 1024 * 2^((4 - res)/8) (src/filter.cc set_Q).
const uint16_t DIVQ_LOW[16] = {1448, 1324, 1219, 1129, 1052, 984, 925, 872,
                               826, 783, 745, 711, 679, 651, 624, 600};
const uint16_t DIVQ_HIGH[16] = {1448, 1328, 1218, 1117, 1024, 939, 861, 790,
                                724, 664, 609, 558, 512, 470, 431, 395};

// WaveformGenerator.cpp timing constants (6581R3 / 8580R5)
static const uint32_t FLOAT_TTL[2] = {54000, 800000};
static const uint32_t FLOAT_FADE[2] = {1400, 50000};
static const uint32_t SR_RESET[2] = {50000, 986000};
static const uint32_t SR_FADE[2] = {15000, 314300};
// SID.cpp bus value TTL
static const uint32_t BUS_TTL[2] = {0x01d00, 0xa2000};

static const uint32_t SHIFT_MASK =
    ~((1u << 2) | (1u << 4) | (1u << 8) | (1u << 11) | (1u << 13) | (1u << 17) | (1u << 20) | (1u << 22));

// ---------------------------------------------------------------------------
bool Tables::load(const char* memdir)
{
    auto rd = [&](const char* name, int n, auto store) {
        std::string p = std::string(memdir) + "/" + name;
        FILE* f = fopen(p.c_str(), "r");
        if (!f) return false;
        char line[512];
        int i = 0;
        while (i < n && fgets(line, sizeof line, f)) {
            if (line[0] == '/' || line[0] == '\n') continue;
            store(i++, unsigned(strtoul(line, nullptr, 16)));
        }
        fclose(f);
        return i == n;
    };
    return rd("sid_wf_dir.mem", 4096, [&](int i, unsigned v) { dir[i] = uint16_t(v); }) &&
           rd("sid_wf_data.mem", 4096, [&](int i, unsigned v) { data[i] = uint8_t(v); });
}

// table select by waveform & 7 (WaveformGenerator::setPulldown)
static int pd_table(unsigned waveform)
{
    switch (waveform & 7) {
    case 3: return 0;                       // ST
    case 4: return (waveform & 8) ? 4 : -1; // NP
    case 5: return 1;                       // PT
    case 6: return 2;                       // PS
    case 7: return 3;                       // PST
    default: return -1;
    }
}

unsigned Tables::pulldown(bool is8580, unsigned waveform, unsigned wv) const
{
    static const unsigned base[5] = {0x0000, 0x1000, 0x1800, 0x2800, 0x3800};
    int t = pd_table(waveform);
    unsigned idx = t == 1 ? wv >> 1 : t == 4 ? (wv >> 4) & 0xff : wv;
    unsigned e = (unsigned(is8580) << 14) | (base[t] + idx);
    return unsigned(data[(dir[e >> 3] << 3) | (e & 7)]) << 4;
}

bool Sid::table_used(unsigned v) const { return w[v].waveform != 0 && pd_table(w[v].waveform) >= 0; }

// ---------------------------------------------------------------------------
static bool do_writeback(unsigned wf_old, unsigned wf_new, bool is6581)
{
    if (wf_old <= 8) return false;
    if (wf_new < 8) return false;
    if (wf_new == 8 && wf_old != 0xf) return false;
    if (is6581 && ((((wf_old & 3) == 1) && ((wf_new & 3) == 2)) || (((wf_old & 3) == 2) && ((wf_new & 3) == 1))))
        return false;
    if (wf_old == 0xc) return false;
    if (wf_new == 0xc) return false;
    return true;
}

static uint32_t noise_writeback(uint32_t o)
{
    return ((o & (1u << 11)) >> 9) | ((o & (1u << 10)) >> 6) | ((o & (1u << 9)) >> 1) | ((o & (1u << 8)) << 3) |
           ((o & (1u << 7)) << 6) | ((o & (1u << 6)) << 11) | ((o & (1u << 5)) << 15) | ((o & (1u << 4)) << 18);
}

void Sid::set_noise_output(unsigned v)
{
    uint32_t s = w[v].sr;
    w[v].noise_out = ((s & (1u << 2)) << 9) | ((s & (1u << 4)) << 6) | ((s & (1u << 8)) << 1) |
                     ((s & (1u << 11)) >> 3) | ((s & (1u << 13)) >> 6) | ((s & (1u << 17)) >> 11) |
                     ((s & (1u << 20)) >> 15) | ((s & (1u << 22)) >> 18);
}

void Sid::shift_phase2(unsigned v, unsigned wf_old, unsigned wf_new)
{
    Wave& x = w[v];
    if (do_writeback(wf_old, wf_new, !cfg.is8580))
        x.sr_latch = (x.sr & SHIFT_MASK) | noise_writeback(x.wav_out);
    uint32_t bit22 = ((x.test_or_reset ? 1u : 0u) | x.sr_latch) << 22;
    uint32_t bit0 = (bit22 ^ (x.sr_latch << 17)) & (1u << 22);
    x.sr = ((x.sr_latch >> 1) | bit0) & 0x7fffff;
    set_noise_output(v);
}

void Sid::wave_ctrl(unsigned v, uint8_t control)
{
    Wave& x = w[v];
    unsigned wf_prev = x.waveform;
    bool test_prev = x.test;
    x.waveform = (control >> 4) & 0xf;
    x.test = (control & 0x08) != 0;
    x.sync = (control & 0x02) != 0;
    x.ring = ((~control >> 5) & (control >> 2) & 1) != 0;
    if (x.waveform != wf_prev && x.waveform == 0) x.float_ttl = FLOAT_TTL[cfg.is8580];
    if (x.test != test_prev) {
        if (x.test) {
            x.acc = 0;
            x.sr_pipe = 0;
            x.sr_latch = x.sr;
            x.sr_reset = SR_RESET[cfg.is8580];
        } else {
            shift_phase2(v, wf_prev, x.waveform);
        }
    }
}

// EnvelopeGenerator::writeCONTROL_REG
void Sid::env_ctrl(unsigned v, uint8_t control)
{
    Env& x = e[v];
    bool gate_next = (control & 1) != 0;
    if (gate_next == x.gate) return;
    x.gate = gate_next;
    if (gate_next) {
        x.next_state = 0;
        x.state_pipe = 2;
        if (x.reset_lfsr || x.exp_pipe == 2)
            x.env_pipe = (x.exp_period == 1 || x.exp_pipe == 2) ? 2 : 4;
        else if (x.exp_pipe == 1)
            x.state_pipe = 3;
    } else {
        x.next_state = 2;
        x.state_pipe = x.env_pipe > 0 ? 3 : 2;
    }
}

void Sid::env_ad(unsigned v)
{
    Env& x = e[v];
    uint8_t ad = reg[7 * v + 5];
    if (x.state == 0) x.rate_idx = ad >> 4;
    else if (x.state == 1) x.rate_idx = ad & 15;
}

void Sid::env_sr(unsigned v)
{
    Env& x = e[v];
    if (x.state == 2) x.rate_idx = reg[7 * v + 6] & 15;
}

void Sid::apply_write(unsigned r, uint8_t val)
{
    bus_value = val;
    bus_ttl = BUS_TTL[cfg.is8580];
    reg[r] = val;
    if (r < 21) {
        unsigned v = r / 7;
        switch (r % 7) {
        case 4: wave_ctrl(v, val); env_ctrl(v, val); break;
        case 5: env_ad(v); break;
        case 6: env_sr(v); break;
        default: break;
        }
    }
}

void Sid::apply_read(unsigned r)
{
    if (r >= 0x19 && r <= 0x1c) {
        bus_value = pend_v;
        bus_ttl = BUS_TTL[cfg.is8580];
    } else {
        bus_ttl /= 2;
    }
}

uint8_t Sid::read(unsigned r)
{
    r &= 31;
    uint8_t v;
    switch (r) {
    case 0x19: v = pot_x; break;
    case 0x1a: v = pot_y; break;
    case 0x1b: v = uint8_t(w[2].osc3 >> 4); break;
    case 0x1c: v = e[2].env3; break;
    default: v = bus_value; break;
    }
    pend = 2;
    pend_r = r;
    pend_v = v;
    return v;
}

void Sid::write(unsigned r, uint8_t v)
{
    pend = 1;
    pend_r = r & 31;
    pend_v = v;
}

// Our /RES puts the whole SID into its power-on state (reSIDfp keeps the
// accumulators and envelope counters over a reset; the RTL clears all state).
void Sid::reset()
{
    memset(reg, 0, sizeof reg);
    for (unsigned v = 0; v < 3; v++) {
        w[v] = Wave();
        e[v] = Env();
        Wave& x = w[v];
        x.msb_rising = false;
        x.waveform = 0;
        x.osc3 = 0;
        x.test = x.sync = x.ring = false;
        x.pulse_out = true;
        x.sr_reset = 0;
        x.sr = 0x7fffff;
        x.test_or_reset = true;
        x.sr_latch = x.sr;
        shift_phase2(v, 0, 0);
        x.sr_pipe = 0;
        x.wav_out = 0;
        x.float_ttl = 0;
        Env& y = e[v];
        y.env_pipe = 0;
        y.state_pipe = 0;
        y.gate = false;
        y.reset_lfsr = true;
        y.exp_cnt = 0;
        y.exp_period = 1;
        y.new_exp_period = 0;
        y.state = 2;
        y.counter_enabled = true;
        y.rate_idx = 0;
    }
    vlp = vbp = vhp = 0;
    bus_value = 0;
    bus_ttl = 0;
    pend = 0;
}

// WaveformGenerator::clock
void Sid::wave_clock(unsigned v)
{
    Wave& x = w[v];
    if (x.test) {
        if (x.sr_reset != 0 && --x.sr_reset == 0) {
            x.sr |= x.sr >> 1;
            x.sr |= 0x400000;
            if (x.sr != 0x7fffff) x.sr_reset = SR_FADE[cfg.is8580];
            x.sr_latch = x.sr;
            set_noise_output(v);
        }
        x.test_or_reset = true;
        x.pulse_out = true;
    } else {
        uint32_t freq = reg[7 * v] | (reg[7 * v + 1] << 8);
        uint32_t old = x.acc;
        x.acc = (x.acc + freq) & 0xffffff;
        uint32_t set = ~old & x.acc;
        x.msb_rising = (set & 0x800000) != 0;
        if (set & 0x080000) {
            x.sr_pipe = 2;
        } else if (x.sr_pipe != 0) {
            if (--x.sr_pipe == 0) {
                shift_phase2(v, x.waveform, x.waveform);
            } else {
                x.test_or_reset = false;
                x.sr_latch = x.sr;
            }
        }
    }
}

// WaveformGenerator::output (+ write_shift_register)
void Sid::wave_output(unsigned v)
{
    Wave& x = w[v];
    const Wave& prev = w[(v + 2) % 3];
    const bool is8580 = cfg.is8580;
    uint32_t pw = reg[7 * v + 2] | ((reg[7 * v + 3] & 15) << 8);
    if (x.waveform != 0) {
        uint32_t ix = (x.acc ^ ((~prev.acc & (x.ring ? 0x800000u : 0u)))) >> 12;
        ix &= 0xfff;
        uint32_t base;
        switch (x.waveform & 3) {
        case 0: base = 0xfff; break;
        case 1: base = ((ix & 0x800) ? (ix ^ 0xfff) : ix) << 1 & 0xfff; break;
        case 2: base = ix; break;
        default: base = ix & (ix << 1) & 0xfff; break;
        }
        uint32_t pmask = (x.waveform & 4) ? (x.pulse_out ? 0xfff : 0) : 0xfff;
        uint32_t nmask = (x.waveform & 8) ? x.noise_out : 0xfff;
        bool pd = pd_table(x.waveform) >= 0;
        uint32_t wv = base & pmask & nmask;
        x.wav_out = pd ? tab->pulldown(is8580, x.waveform, wv) : wv;
        if (is8580 && (x.waveform & 3)) {
            uint32_t o = x.tsp & pmask & nmask;
            x.osc3 = pd ? tab->pulldown(is8580, x.waveform, o) : o;
            x.tsp = base;
        } else {
            x.osc3 = x.wav_out;
        }
        // 6581: saw combinations may pull the accumulator MSB low
        if (!is8580 && (x.waveform & 2) && !(x.wav_out & 0x800)) {
            x.msb_rising = false;
            x.acc &= 0x7fffff;
        }
        if (x.waveform > 8) {
            if (x.sr_pipe != 1 && !x.test) {
                x.sr &= SHIFT_MASK | noise_writeback(x.wav_out);
                x.noise_out &= x.wav_out;
            } else {
                x.noise_out = x.wav_out;
            }
        }
    } else if (x.float_ttl != 0 && --x.float_ttl == 0) {
        x.wav_out &= x.wav_out >> 1;
        x.osc3 = x.wav_out;
        if (x.wav_out != 0) x.float_ttl = FLOAT_FADE[is8580];
    }
    x.pulse_out = (x.acc >> 12) >= pw;
}

// EnvelopeGenerator::clock
void Sid::env_clock(unsigned v)
{
    Env& x = e[v];
    x.env3 = x.counter;
    if (x.new_exp_period > 0) {
        x.exp_period = x.new_exp_period;
        x.new_exp_period = 0;
    }
    if (x.state_pipe) {
        // state_change
        x.state_pipe--;
        switch (x.next_state) {
        case 0:
            if (x.state_pipe == 1) {
                x.rate_idx = reg[7 * v + 5] & 15;
            } else if (x.state_pipe == 0) {
                x.state = 0;
                x.rate_idx = reg[7 * v + 5] >> 4;
                x.counter_enabled = true;
            }
            break;
        case 1:
            if (x.state_pipe == 0) {
                x.state = 1;
                x.rate_idx = reg[7 * v + 5] & 15;
            }
            break;
        default:
            if ((x.state == 0 && x.state_pipe == 0) || (x.state == 1 && x.state_pipe == 1)) {
                x.state = 2;
                x.rate_idx = reg[7 * v + 6] & 15;
            }
            break;
        }
    }
    if (x.env_pipe != 0 && --x.env_pipe == 0) {
        if (x.counter_enabled) {
            if (x.state == 0) {
                if (++x.counter == 0xff) {
                    x.next_state = 1;
                    x.state_pipe = 3;
                }
            } else {
                if (--x.counter == 0x00) x.counter_enabled = false;
            }
            // set_exponential_counter
            switch (x.counter) {
            case 0xff: case 0x00: x.new_exp_period = 1; break;
            case 0x5d: x.new_exp_period = 2; break;
            case 0x36: x.new_exp_period = 4; break;
            case 0x1a: x.new_exp_period = 8; break;
            case 0x0e: x.new_exp_period = 16; break;
            case 0x06: x.new_exp_period = 30; break;
            default: break;
            }
        }
    } else if (x.exp_pipe != 0 && --x.exp_pipe == 0) {
        x.exp_cnt = 0;
        if ((x.state == 1 && x.counter != ((reg[7 * v + 6] & 0xf0) | (reg[7 * v + 6] >> 4))) || x.state == 2)
            x.env_pipe = 1;
    } else if (x.reset_lfsr) {
        x.lfsr = 0x7fff;
        x.reset_lfsr = false;
        if (x.state == 0) {
            x.exp_cnt = 0;
            x.env_pipe = 2;
        } else if (x.counter_enabled && ++x.exp_cnt == x.exp_period) {
            x.exp_pipe = x.exp_period != 1 ? 2 : 1;
        }
    }
    if (x.lfsr != ADSR_TABLE[x.rate_idx]) {
        unsigned fb = ((x.lfsr << 14) ^ (x.lfsr << 13)) & 0x4000;
        x.lfsr = uint16_t((x.lfsr >> 1) | fb);
    } else {
        x.reset_lfsr = true;
    }
}

static int64_t sat(int64_t v, int bits)
{
    int64_t hi = (int64_t(1) << (bits - 1)) - 1, lo = -(int64_t(1) << (bits - 1));
    return v > hi ? hi : v < lo ? lo : v;
}

// Linear analog part (T25.1 §3.4/§3.5): DCA, voice DC (digis), Chamberlin SVF
// with the app's cutoff curve, mixer and master volume.
void Sid::analog()
{
    const int32_t zero = cfg.is8580 ? 0x800 : 0x380;
    const int32_t dc = cfg.digi ? (0x800 * 0xff) >> (3 - cfg.digi) : 0;
    const unsigned filt = reg[0x17] & 7, res = reg[0x17] >> 4, mode = reg[0x18];
    int32_t vi = 0, vnf = 0;
    for (unsigned v = 0; v < 3; v++) {
        vo[v] = (int32_t(w[v].wav_out) - zero) * int32_t(e[v].counter);
        int32_t x = vo[v] + dc;
        if (filt & (1u << v)) vi += x;
        else if (!(v == 2 && (mode & 0x80))) vnf += x;
    }
    const unsigned fc = (unsigned(reg[0x16]) << 3) | (reg[0x15] & 7);
    const int64_t w0 = cfg.curve ? cfg.curve[fc >> 1] : 0;
    const int64_t divq = cfg.res_high ? DIVQ_HIGH[res] : DIVQ_LOW[res];
    const int64_t dvbp = (w0 * vhp) >> 16;
    const int64_t dvlp = (w0 * vbp) >> 16;
    vbp = int32_t(sat(vbp - dvbp, 25));
    vlp = int32_t(sat(vlp - dvlp, 25));
    vhp = int32_t(sat(((int64_t(vbp) * divq) >> 10) - vlp - vi, 25));
    int64_t vf = ((mode & 0x10) ? vlp : 0) + ((mode & 0x20) ? int64_t(vbp) : 0) + ((mode & 0x40) ? int64_t(vhp) : 0);
    out = int32_t(sat(((vnf + vf) * int64_t(mode & 15)) >> 7, 20));
}

void Sid::clock()
{
    if (pend == 1) apply_write(pend_r, pend_v);
    else if (pend == 2) apply_read(pend_r);
    pend = 0;
    if (bus_ttl != 0 && --bus_ttl == 0) bus_value = 0;
    for (unsigned v = 0; v < 3; v++) wave_clock(v);
    for (unsigned v = 0; v < 3; v++) env_clock(v);
    for (unsigned v = 0; v < 3; v++) wave_output(v);
    // synchronize. reSIDfp (SID::voiceSync) runs it only in the cycles in which
    // the MSB of a voice not in test, whose follower has SYNC set, rises; then
    // every voice syncs its follower on its msb_rising, which a voice in test
    // keeps from before (stale). Evaluated here every cycle.
    bool event = false;
    for (unsigned v = 0; v < 3; v++)
        event |= w[v].msb_rising && !w[v].test && w[(v + 1) % 3].sync;
    bool rst[3];
    for (unsigned v = 0; v < 3; v++) {
        const Wave& x = w[v];
        const Wave& nx = w[(v + 1) % 3];
        const Wave& pv = w[(v + 2) % 3];
        rst[(v + 1) % 3] = event && x.msb_rising && nx.sync && !(x.sync && pv.msb_rising);
    }
    for (unsigned v = 0; v < 3; v++)
        if (rst[v]) w[v].acc = 0;
    analog();
}

} // namespace trxsid
