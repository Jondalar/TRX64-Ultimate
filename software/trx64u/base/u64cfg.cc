// The C64U's hardware configuration from the core's settings (u64cfg.h). PROVENANCE: vendor/gideon/software/u64/u64_config.cc (Gideon Zweijtzer, GPL-3.0):
// U64Config::effectuate_settings, setMixer, setSpeakerMixer, U64SidSockets/U64UltiSids/U64SidAddressing::effectuate_settings, setFilter, setSidEmuParams,
// setCpuSpeed, setLedSelector, auto_mirror, the palette programming. The register addresses are not Gideon's (u64.h): every one is c64_basic's, from
// its TOML A (../gateware/c64_basic.toml) through the driver table (drvtab.h, trx64u_lib.h). c64_basic has no resampler (ultimate_sys.vhd decodes none
// at 0x10100580), so SetResampleFilter has no counterpart here.
// What is different: the values come from this application's settings (menu.toml groups video, audio, sid, input, expert) instead of a ConfigStore, and
// the system mode goes to the host's picture layer (board.video_set_system, which applies it through app/video/videomode.cc's video_apply).
// U18: built as the trx64u library against the TRX64 SDK (base 3.3). The settings are read and written through the settings
// group (setting.cc), the SID chips through the C64's memory window (api->c64), the keys, the SID sockets' supply, the video PLL, the picture layer, the
// case LEDs and the ESP32 through the 3.3 groups io and board (g_api->io, board); every register through the driver table: the C64's I/O page, the U64
// page, the speaker, reSID8, the mixer's and the speaker's gains, both palettes (core blocks of the TOML A since Alex's split, U18 §5 2026-11-24) and the
// cartridge port's page. lib_trx64u.cc lists the operations and states of this file with their names and arguments.
#include "u64cfg.h"
#include "trx64u_lib.h"
#include "sid_decide.h"
#include "resid8_filter.h"
#include <string.h>


// ---- the settings this file reads: id, key (the menu.toml `key`) -----------------------------------------------------------------------------------
#define KEYS(X)                                                                                                                                      \
    X(SYSMODE, "System Mode") X(SCANLINES, "HDMI Scan lines") X(ANALOG, "Analog Video Mode") X(DIGITAL, "Digital Video Mode")                          \
    X(PPM, "Adjust Color Clock") X(PALETTE, "Palette Definition")                                                                                    \
    X(MASTER, "Vol Master")                                                                                                                          \
    X(VOL0, "Vol UltiSid 1") X(VOL1, "Vol UltiSid 2") X(VOL2, "Vol Socket 1") X(VOL3, "Vol Socket 2") X(VOL4, "Vol Sampler L") X(VOL5, "Vol Sampler R")  \
    X(PAN0, "Pan UltiSID 1") X(PAN1, "Pan UltiSID 2") X(PAN2, "Pan Socket 1") X(PAN3, "Pan Socket 2") X(PAN4, "Pan Sampler L") X(PAN5, "Pan Sampler R") \
    X(SPK_EN, "Speaker Enable")                                                                                                                      \
    X(SPK0, "Spk Vol UltiSid 1")                                                                                                                     \
    X(S1_EN, "SID Socket 1") X(S2_EN, "SID Socket 2") X(S1_SHUNT, "SID Socket 1 1K Ohm Resistor") X(S2_SHUNT, "SID Socket 2 1K Ohm Resistor")          \
    X(S1_CAPS, "SID Socket 1 Capacitors") X(S2_CAPS, "SID Socket 2 Capacitors")                                                                      \
    X(S1_ADDR, "SID Socket 1 Address") X(S2_ADDR, "SID Socket 2 Address") X(STEREO, "Ext DualSID Range Split")                                       \
    X(U1_ADDR, "UltiSID 1 Address") X(U2_ADDR, "UltiSID 2 Address") X(USPLIT, "UltiSID Range Split") X(PADDLE, "Paddle Override")                    \
    X(MIRROR, "Auto Address Mirroring") X(PLAYER_AUTO, "SID Player Autoconfig") X(ALLOW_EMU, "Allow Autoconfig uses UltiSid")                          \
    X(U1_FILTER, "UltiSID 1 Filter Curve") X(U2_FILTER, "UltiSID 2 Filter Curve") X(U1_RES, "UltiSID 1 Filter Resonance")                            \
    X(U2_RES, "UltiSID 2 Filter Resonance") X(U1_WAVES, "UltiSID 1 Combined Waveforms") X(U2_WAVES, "UltiSID 2 Combined Waveforms")                  \
    X(U1_DIGI, "UltiSID 1 Digis Level") X(U2_DIGI, "UltiSID 2 Digis Level")                                                                          \
    X(U1_FON, "UltiSID 1 Filter") X(U2_FON, "UltiSID 2 Filter") X(U1_BASE, "UltiSID 1 6581 Base") X(U2_BASE, "UltiSID 2 6581 Base")                  \
    X(U1_OFFS, "UltiSID 1 6581 Offset") X(U2_OFFS, "UltiSID 2 6581 Offset")                                                                          \
    X(USERPORT, "UserPort Power Enable") X(TURBO, "Turbo Control") X(SPEED, "CPU Speed") X(BADLINE, "Badline Timing")                                \
    X(SUPERCPU, "SuperCPU Detect (D0BC)") X(IECMODE, "Serial Bus Mode") X(PARCABLE, "SpeedDOS Parallel Cable") X(BURST, "Burst Mode Patch")          \
    X(LED0, "LED Select Top") X(LED1, "LED Select Bot")

enum {
#define X(i, k) K_##i,
    KEYS(X)
#undef X
    K_N
};
static const char *const key_names[K_N] = {
#define X(i, k) k,
    KEYS(X)
#undef X
};

// the keys the menu does not have, said once (their defaults are used). Not looked for: the palette (a text setting).
static void note_missing(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    for (int i = 0; i < K_N; i++) {
        if (i != K_PALETTE && !setting_known(key_names[i])) {
            lib_logf("[u64cfg] the menu has no setting \"%s\"; its default is used", key_names[i]);
        }
    }
}

// The index of the value of a setting (an enum's position, a range's number minus its low end), `def` when the menu has no such item.
static int V(int id, int def = 0)
{
    note_missing();
    return setting_ix(key_names[id], def);
}

// A range setting's number as the menu shows it (the 6581 base 0 .. 511, the 6581 offset -1024 .. 1023), `def` when the menu has no such item.
static int VN(int id, int def)
{
    note_missing();
    return setting_num(key_names[id], def);
}

// ---- video ---------------------------------------------------------------------------------------------------------------------------------
static const uint8_t default_colors[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xF7, 0xF7, 0xF7 }, { 0x8D, 0x2F, 0x34 }, { 0x6A, 0xD4, 0xCD }, { 0x98, 0x35, 0xA4 }, { 0x4C, 0xB4, 0x42 },
    { 0x2C, 0x29, 0xB1 }, { 0xEF, 0xEF, 0x5D }, { 0x98, 0x4E, 0x20 }, { 0x5B, 0x38, 0x00 }, { 0xD1, 0x67, 0x6D }, { 0x4A, 0x4A, 0x4A },
    { 0x7B, 0x7B, 0x7B }, { 0x9F, 0xEF, 0x93 }, { 0x6D, 0x6A, 0xEF }, { 0xB2, 0xB2, 0xB2 } };

static uint8_t g_palette[16][3];
static bool g_palette_valid;
static char g_palette_name[64];

// Colour i of the two palettes, the C64's picture (c64_palette) and the overlay's characters on HDMI (hdmi_palette): red, green and blue, runs of 16
// in the TOML A. Gideon's YUV copy of the C64 palette 0x400 behind its RGB (U64Config::rgb_to_yuv) is not written: c64_basic decodes no register
// there (ultimate_sys.vhd pal_we only with address bits 10:6 = 0).
static void put_colour(int i, const uint8_t rgb[3])
{
    HW_WR_I(c64_palette_red, i, rgb[0]);
    HW_WR_I(c64_palette_green, i, rgb[1]);
    HW_WR_I(c64_palette_blue, i, rgb[2]);
    HW_WR_I(hdmi_palette_red, i, rgb[0]);
    HW_WR_I(hdmi_palette_green, i, rgb[1]);
    HW_WR_I(hdmi_palette_blue, i, rgb[2]);
}

static void program_palette(const uint8_t rgb[16][3])
{
    memcpy(g_palette, rgb, sizeof(g_palette));
    g_palette_valid = true;
    for (int i = 0; i < 16; i++) {
        put_colour(i, rgb[i]);
    }
}

// The colours of the stopped machine's picture (api->overlay->still_palette, SDK 0.4.3; U18 T-3): the base paints its still (the picture under the open
// menu) and a library's frame() (the ring library's Timewarp still) from the 16 colours of a source, else from its own (none for this core: MultiCore's c64
// core sets it, and that code is not in trx64u's app), else Colodore. This library's source is the palette in force (program_palette, u64cfg_palette);
// false before the first. It runs on the task that takes the picture, inside the base's grab lock: a copy, no lock, no call of api->overlay. A palette
// written while it copies may give that one picture colours of both.
static bool still_colours(uint8_t rgb[16][3])
{
    if (!g_palette_valid) {
        return false;
    }
    memcpy(rgb, g_palette, sizeof(g_palette));
    return true;
}

// The base takes the source only once its overlay service is up, after the core's start (api.md 4.17.1.1, 4.17.15.6: OVL_E_NOSERVICE before; the core's
// start hook runs before the service starts, run.cc). The core's service poll calls this at each pass until the base has it (c64core.cc c64_service): the
// first pass after the service starts, 20 ms at most, sets it; the hook menu_video tries it too. A base without the member: nothing.
void u64cfg_still_palette(void)
{
    static volatile bool done;
    if (done) {
        return;
    }
    const SvcOverlay *ov = LIB_API_COVERS(g_api, overlay) ? g_api->overlay : 0;
    if (ov && ov->size < offsetof(SvcOverlay, still_palette) + sizeof(ov->still_palette)) {
        done = true;
        return;
    }
    if (SVC_HAS(SvcOverlay, ov, still_palette) && ov->still_palette(still_colours) == 0) {
        done = true;
        lib_logf("[u64cfg] the stopped machine's picture is painted in the palette in force");
    }
}

// CONTROL 0x51-0x54 of the command interface (control_target.cc:158-199, U64Config::get_palette_rgb / set_palette_rgb / set_palette_color / reset_palette, u64_config.cc:3099-3170).
// what 0 reads the 16 colours in force, 1 sets all, 2 sets colour idx from rgb[0], 3 back to the default colours. A palette set here is not the setting's: the next change of the
// palette's group loads the configured one again (the name is forgotten, as the firmware's effectuate always reads the file again).
void u64cfg_palette(int what, uint8_t idx, uint8_t (*rgb)[3])
{
    if (what == 0) {
        memcpy(rgb, g_palette_valid ? g_palette : default_colors, sizeof(g_palette));
        return;
    }
    if (what == 2) {
        if (idx >= 16) {                                            // the palettes have 16 colours; trxuci_api.h leaves the bound to the callee
            return;
        }
        if (!g_palette_valid) {
            memcpy(g_palette, default_colors, sizeof(g_palette));
            g_palette_valid = true;
        }
        memcpy(g_palette[idx], rgb[0], 3);
        put_colour(idx, rgb[0]);
    } else {
        uint8_t all[16][3];
        memcpy(all, what == 1 ? rgb : default_colors, sizeof(all));
        program_palette(all);
    }
    g_palette_name[0] = 0;
}

// sscanf's %x: blanks, an optional 0x, at least one hex digit
static bool scan_hex(const char **p, unsigned *v)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == '\v' || *s == '\f') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && lc_isxdigit((unsigned char)s[2])) {
        s += 2;
    }
    if (!lc_isxdigit((unsigned char)*s)) {
        return false;
    }
    unsigned x = 0;
    for (; lc_isxdigit((unsigned char)*s); s++) {
        x = x * 16 + (unsigned)(lc_isdigit((unsigned char)*s) ? *s - '0' : lc_tolower((unsigned char)*s) - 'a' + 10);
    }
    *v = x;
    *p = s;
    return true;
}

// A .vpl (Gideon FileTypePalette::parseVplFile): lines of three hex numbers r g b, '#' starts a comment; 16 of them.
static bool parse_vpl(const char *text, int n, uint8_t rgb[16][3])
{
    int idx = 0, p = 0;
    while (p < n && idx < 16) {
        char line[80];
        int l = 0;
        while (p < n && text[p] != '\n') {
            if (text[p] != '\r' && l < (int)sizeof(line) - 1) {
                line[l++] = text[p];
            }
            p++;
        }
        p++;
        line[l] = 0;
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        unsigned r, g, b;
        if (!line[strspn(line, " \t")]) {
            continue;
        }
        const char *s = line;
        if (!scan_hex(&s, &r) || !scan_hex(&s, &g) || !scan_hex(&s, &b)) {
            return false;
        }
        rgb[idx][0] = (uint8_t)r;
        rgb[idx][1] = (uint8_t)g;
        rgb[idx][2] = (uint8_t)b;
        idx++;
    }
    return idx == 16;
}

static const char *palette_dir(void) { return "/flash/data"; }

// 0 on success; the file by name from the data folder of the flash, or a full path.
static int load_palette(const char *name, char *msg, int msglen)
{
    uint8_t rgb[16][3];
    char path[200];
    if (name[0] == '/') {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "%s/%s", palette_dir(), name);
    }
    uint64_t size = 0;
    bool dir = false;
    if (!g_api->fs->stat(path, &size, &dir) || dir || size > 8192 || size < 8) {
        snprintf(msg, msglen, "%s: no palette file (8 bytes to 8 KB)", path);
        return 1;
    }
    char *buf = (char *)g_api->mem->malloc(8200);                    // while the file is read
    if (!buf) {
        snprintf(msg, msglen, "%s: no memory to read it", path);
        return 1;
    }
    int got = g_api->fs->load(path, buf, 8200);
    bool ok = got >= 0 && parse_vpl(buf, got, rgb);
    g_api->mem->free(buf);
    if (!ok) {
        snprintf(msg, msglen, "%s: not a palette (16 lines of three hex numbers)", path);
        return 1;
    }
    program_palette(rgb);
    snprintf(g_palette_name, sizeof(g_palette_name), "%s", name);
    return 0;
}

// The colour timing of each system mode (the position of "System Mode"): the video format, the phase increment, and the audio divider Gideon's resampler
// filter is picked by (no resampler in c64_basic: audio_div is kept with its row, unused). PROVENANCE:
// software/ultimate-app/software/u64/color_timings.cc (Gideon Zweijtzer, GPL-3.0), color_timings[] in its order: c_pal_50_283_5, c_ntsc_60_227_5,
// c_pal_60_free, c_ntsc_50_free, c_pal_60_281_5, c_ntsc_50_228_5. mode_bits are its VIDEO_FMT_* (u64.h: NTSC_ENCODING 0x01, 60_HZ 0x02, NTSC_FREQ 0x08,
// CYCLES_63 0x00, CYCLES_65 0x20), phase_inc includes its TABLE_520 bit 0x80. MultiCore keeps no copy for libraries (svc_board.h: this library keeps its own).
static const struct {
    uint8_t mode_bits, phase_inc, audio_div;
} color_timing[6] = {
    { 0x00, 0x37, 77 },                               // PAL 50, 283.5
    { 0x2B, 0x87, 80 },                               // NTSC 60, 227.5
    { 0x2A, 0x22, 80 },                               // PAL 60, free
    { 0x01, 0x98, 77 },                               // NTSC 50, free
    { 0x2A, 0xB3, 80 },                               // PAL 60, 281.5
    { 0x01, 0x09, 77 },                               // NTSC 50, 228.5
};

static int g_sys = -1, g_ppm = 0;                 // the host brought the picture up with no offset

int fx_video(CoreCtx *, const OpArgs &, OpOut &o)
{
    int sys = V(K_SYSMODE);
    HW_WR(c64_scanlines_on, (uint8_t)V(K_SCANLINES, 1));
    int dig = V(K_DIGITAL);
    HW_WR(u64_connectors_hdmi_enable, dig == 2 ? 0 : 1);  // Auto: the screen is HDMI unless the user says DVI (the EDID tells nothing a DVI monitor does not also say)
    uint8_t format = V(K_ANALOG) ? DRV_c64_video_format_rgb_output_MASK : 0;
    if (sys < 0 || sys >= (int)(sizeof(color_timing) / sizeof(color_timing[0]))) {
        snprintf(o.msg, o.msglen, "No colour timing for system mode %d", sys);
        return OPE_FAILED;
    }
    HW_WR(c64_phase_incr, color_timing[sys].phase_inc);
    int ppm = VN(K_PPM, 0);                           // the colour clock's offset, -100 .. 100 ppm
    c64_video_format((uint8_t)(color_timing[sys].mode_bits | format));
    const SvcBoard *b = g_api->board;
    if (sys != g_sys) {
        const char *name = setting_label(key_names[K_SYSMODE], sys);
        if (name) {
            lib_logf("[u64cfg] system mode %s", name);
        } else {
            lib_logf("[u64cfg] system mode %d", sys);
        }
        if (b) {
            b->video_set_system(sys);                 // the picture layer takes the new family: its mode and the Picture row are picked again (FW u64_config.cc:1471-1508)
        }
        g_ppm = 0;
        g_sys = sys;
    }
    if (ppm != g_ppm && b) {
        b->video_set_pll(sys, ppm);                   // the colour clock's offset goes with the PLL setting (Gideon's setPllOffset is empty), then the HDMI PLL's reset
        g_ppm = ppm;
    }
    // the palette: the file named in the setting, else the default colours
    const char *pal = setting_text(key_names[K_PALETTE]);
    if (*pal) {
        if (strcmp(pal, g_palette_name)) {
            if (load_palette(pal, o.msg, o.msglen)) {
                program_palette(default_colors);
                g_palette_name[0] = 0;
                return OPE_FAILED;
            }
        }
    } else if (!g_palette_valid || g_palette_name[0]) {
        program_palette(default_colors);
        g_palette_name[0] = 0;
    }
    return OPE_OK;
}

int op_palette_apply(CoreCtx *, const OpArgs &a, OpOut &o)
{
    return load_palette(a.s(0), o.msg, o.msglen) ? OPE_FAILED : OPE_OK;
}

// "Set as Default" (a .vpl in the Library): the palette is applied and its name is the setting, so every start takes it
int op_palette_default(CoreCtx *, const OpArgs &a, OpOut &o)
{
    if (load_palette(a.s(0), o.msg, o.msglen)) {
        return OPE_FAILED;
    }
    OpIn in[2] = { in_str("key", "Palette Definition"), in_str("value", a.s(0)) };
    OpResult r;
    g_api->ops->call("settings.set", in, 2, &r);
    snprintf(o.msg, o.msglen, "Palette %s is the default", a.s(0));
    return OPE_OK;
}

// T228 R21 #16, FW U64Config::list_palettes (u64_config.cc:3072-3097): the choices of "Palette Definition" are the empty text and the files with the extension .vpl in the
// data folder of the flash. The rows of the pick list (picklist.h): value, label. The app ABI's entry keeps 55 characters of a name: one that long may have been cut and is left out.
const char *state_palettes(CoreCtx *)
{
    static char t[24 * 60];
    static SvcFsEntry ent[24];
    int p = snprintf(t, sizeof(t), "\tBuilt-in\n");
    int n = g_api->fs->list(palette_dir(), ent, 24);
    for (int i = 0; i < n; i++) {
        const char *x = strrchr(ent[i].name, '.');
        bool cut = strlen(ent[i].name) >= sizeof(ent[i].name) - 1;
        if (ent[i].dir || cut || !x || strcasecmp(x, ".vpl") || p + (int)strlen(ent[i].name) + 3 >= (int)sizeof(t)) {
            continue;
        }
        p += snprintf(t + p, sizeof(t) - p, "%s\n", ent[i].name);
    }
    return t;
}

const char *state_palette(CoreCtx *)
{
    return g_palette_name[0] ? g_palette_name : "Default colours";
}

// ---- audio ---------------------------------------------------------------------------------------------------------------------------------
static const uint8_t volume_ctrl[] = {
    0x00, 0x01, 0x02, 0x04, 0x06, 0x08, 0x10, 0x12, 0x14, 0x17, 0x1a, 0x1d, 0x20, 0x24, 0x28, 0x2d,
    0x33, 0x39, 0x40, 0x48, 0x51, 0x5b, 0x66, 0x72, 0x80, 0x90, 0xa1, 0xb5, 0xcb, 0xe4, 0xff };
static const uint16_t pan_ctrl[] = { 0, 40, 79, 116, 150, 181, 207, 228, 243, 253, 256 };

// source x master x pan in one step (Gideon mixer_side): a side panned away or OFF stays 0, anything else is at least 1
static uint8_t mixer_side(uint8_t source_raw, uint8_t master_raw, uint16_t pan)
{
    if (!source_raw || !master_raw || !pan) {
        return 0;
    }
    uint32_t v = ((uint32_t)source_raw * master_raw * pan + (1u << 14)) >> 15;
    if (v > 0xFF) {
        v = 0xFF;
    }
    return v ? (uint8_t)v : 1;
}

// The mixer's gains (the TOML A's block audio_mixer, a run of 12): a right and a left byte per channel, in Gideon's channel order UltiSID 1, UltiSID 2,
// socket 1, socket 2, sampler L, sampler R. Gideon's mixer goes on with drive 1 (the hidden settings "Vol Drive 1" and "Pan Drive 1"), drive 2, tape
// read and tape write: c64_basic decodes no byte of the mixer from 0x0C up (ultimate_sys.vhd:1257-1274), so those channels are not written.
#define MIXER_CHANNELS 6
static_assert(2 * MIXER_CHANNELS == DRV_audio_mixer_gain_COUNT, "a right and a left gain per channel");

static void write_mixer(void)
{
    uint8_t master = volume_ctrl[V(K_MASTER, 24)];
    for (int i = 0; i < MIXER_CHANNELS; i++) {
        int vol = V(K_VOL0 + i, 24);
        int pan = V(K_PAN0 + i, 5);
        uint8_t src = volume_ctrl[vol];
        HW_WR_I(audio_mixer_gain, 2 * i, mixer_side(src, master, pan_ctrl[10 - pan]));    // right
        HW_WR_I(audio_mixer_gain, 2 * i + 1, mixer_side(src, master, pan_ctrl[pan]));     // left
    }
}

// The speaker's four gains (blocks speaker_mixer: gain0, gain1 and speaker_mixer_b: gain2, gain3), of which the bitstream plays the mono mix at the
// highest (ultimate_sys.vhd mix_spk), so the one speaker volume goes to all four.
static void write_speaker_gains(uint8_t vol)
{
    HW_WR(speaker_mixer_gain0, vol);
    HW_WR(speaker_mixer_gain1, vol);
    HW_WR(speaker_mixer_b_gain2, vol);
    HW_WR(speaker_mixer_b_gain3, vol);
}

// FW u64_config.cc:1445-1447 (U2PIO_SPEAKER_EN b0: speaker_enable) and setSpeakerMixer :1749-1765. The per-source volumes of the FW have no gain of their
// own in c64_basic and stay out.
static void write_speaker(void)
{
    bool on = V(K_SPK_EN, 1) != 0;
    HW_WR(speaker_enable_ctrl, on ? DRV_speaker_enable_ctrl_on_MASK : 0);
    write_speaker_gains(on ? volume_ctrl[V(K_SPK0, 0)] : 0);
}

int fx_audio(CoreCtx *, const OpArgs &, OpOut &)
{
    write_mixer();
    write_speaker();
    return OPE_OK;
}

void u64cfg_power_on_keys(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    if (!setting_known("System Mode")) {
        return;
    }
    const int ctrl = 58, key_p = 41, key_n = 39;                   // the matrix index col * 8 + row (hotkeys.h): CTRL, P, N
    const SvcIo *io = g_api->io;
    uint8_t m[8];
    bool rs;
    if (!io || !io->keys_read(m, &rs) || !((m[ctrl >> 3] >> (ctrl & 7)) & 1)) {
        return;
    }
    int mode = ((m[key_p >> 3] >> (key_p & 7)) & 1) ? 0 : ((m[key_n >> 3] >> (key_n & 7)) & 1) ? 1 : -1;   // PAL = 0, NTSC = 1: the first two values of System Mode
    if (mode >= 0) {
        setting_override("System Mode", mode);                     // not applied here: the init hook (c64core.cc) calls this before the settings are applied
        lib_logf("[u64cfg] power-on key: %s for this start", mode ? "NTSC" : "PAL");
    }
}

void u64cfg_mute_sids(void)
{
    for (int i = 0; i < 8; i++) {                                   // the four SIDs' channels, both sides
        HW_WR_I(audio_mixer_gain, i, 0);
    }
    write_speaker_gains(0);                                         // the speaker's four bytes
}

void u64cfg_restore_mixer(CoreCtx *)
{
    write_mixer();
    write_speaker();
}

// ---- SID sockets, addressing and the UltiSIDs ----------------------------------------------------------------------------------------------
static const uint8_t sid_offsets[] = {
    0x01, 0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5A, 0x5C, 0x5E,
    0x60, 0x62, 0x64, 0x66, 0x68, 0x6A, 0x6C, 0x6E, 0x70, 0x72, 0x74, 0x76, 0x78, 0x7A, 0x7C, 0x7E,
    0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE, 0xF0, 0xF2, 0xF4, 0xF6, 0xF8, 0xFA, 0xFC, 0xFE };
static const uint8_t stereo_bits[] = { 0x00, 0x02, 0x04, 0x08, 0x10, 0x20 };
static const uint8_t split_bits[] = { 0x00, 0x02, 0x04, 0x08, 0x10, 0x06, 0x12, 0x18 };

// the SID found in each socket by the last detection (u64cfg_detect): 0 none, 1 6581, 2 8580, 3 ...
static int g_sid_type[2];

// Gideon U64Config::auto_mirror: A5..A9 are don't care where all the decodes in D400-D7FF agree, so the space fills with mirrors without new overlaps
static void auto_mirror(uint8_t *base, uint8_t *mask, uint8_t *split, int count)
{
    for (int i = 0; i < 4; i++) {
        base[i] &= ~split[i];
        mask[i] &= ~split[i];
    }
    for (int a = 5; a <= 9; a++) {
        bool same = true, set = false;
        int bit = 0, temp;
        for (int i = 0; i < count; i++) {
            if ((base[i] < 0x40) || (base[i] >= 0x80)) {
                continue;
            }
            if (!set) {
                bit = (((base[i] >> (a - 4)) & 1) << 1) | ((mask[i] >> (a - 4)) & 1);
                set = true;
            } else {
                temp = (((base[i] >> (a - 4)) & 1) << 1) | ((mask[i] >> (a - 4)) & 1);
                if (temp != bit) {
                    same = false;
                }
            }
        }
        if (same) {
            for (int i = 0; i < count; i++) {
                if ((base[i] < 0x40) || (base[i] >= 0x80)) {
                    continue;
                }
                mask[i] &= ~(1 << (a - 4));
                base[i] &= ~(1 << (a - 4));
            }
        }
    }
}

// the resulting decodes, for the address map view (cfg state "u64.sid_map")
static uint8_t g_base[4], g_mask[4], g_split[4];
static uint8_t g_stereo_bak;                                       // FW C64_STEREO_ADDRSEL_BAK

static void write_sid_addressing(void)
{
    uint8_t base[4], mask[4], split[4];
    base[0] = sid_offsets[V(K_S1_ADDR, 1)];
    base[1] = sid_offsets[V(K_S2_ADDR, 1)];
    base[2] = sid_offsets[V(K_U1_ADDR, 1)];
    base[3] = sid_offsets[V(K_U2_ADDR, 1)];
    mask[0] = mask[1] = mask[2] = mask[3] = 0xFE;
    split[0] = split[1] = stereo_bits[V(K_STEREO)];
    split[2] = split[3] = split_bits[V(K_USPLIT)];
    for (int i = 0; i < 4; i++) {                                   // fix_splits
        base[i] &= ~split[i];
        mask[i] &= ~split[i];
    }
    if (V(K_MIRROR, 1)) {
        auto_mirror(base, mask, split, 4);
    }
    HW_WR_I(c64_sid_addr_base, 0, base[0]);
    HW_WR_I(c64_sid_addr_base, 1, base[1]);
    HW_WR_I(c64_sid_addr_base, 2, base[2]);
    HW_WR_I(c64_sid_addr_base, 3, base[3]);
    HW_WR_I(c64_sid_addr_mask, 0, mask[0]);
    HW_WR_I(c64_sid_addr_mask, 1, mask[1]);
    HW_WR_I(c64_sid_addr_mask, 2, mask[2]);
    HW_WR_I(c64_sid_addr_mask, 3, mask[3]);
    HW_WR(c64_sid_sock_stereo_addrsel, (uint8_t)V(K_STEREO));
    g_stereo_bak = (uint8_t)V(K_STEREO);
    HW_WR(c64_sid_cfg_split, (uint8_t)V(K_USPLIT));
    memcpy(g_base, base, 4);
    memcpy(g_mask, mask, 4);
    memcpy(g_split, split, 4);
}

// CONTROL GET_HWINFO device 1 on the U64 (control_target.cc:290-340): the SID map the machine decodes now - the two UltiSIDs, then each enabled socket - as 5 bytes each: address low
// ($D4 + base << 4), high, 0, 0 (the stereo address of a socket value 4 would go in bytes 3-4: this core's enable is a switch, never 4) and the type (2 / 3 an UltiSID by its mask,
// 4 / 5 a socket, bit 7 set). Returns the length.
int u64cfg_sid_map(uint8_t *data)
{
    bool s1 = V(K_S1_EN) != 0, s2 = V(K_S2_EN) != 0;
    data[0] = (uint8_t)(2 + (s1 ? 1 : 0) + (s2 ? 1 : 0));
    int len = 11 + (s1 ? 5 : 0) + (s2 ? 5 : 0);
    int at = 1;
    for (int k = 0; k < 4; k++) {
        if ((k == 2 && !s1) || (k == 3 && !s2)) {
            continue;
        }
        int d = k >= 2 ? k - 2 : k + 2;                                  // g_base order: socket 1, socket 2, UltiSID 1, UltiSID 2; the firmware lists the UltiSIDs first
        unsigned base = g_base[d], mask = g_mask[d];
        data[at + 0] = (uint8_t)(base << 4);
        data[at + 1] = (uint8_t)(0xd0 | (base >> 4));
        data[at + 2] = 0;
        data[at + 3] = 0;
        data[at + 4] = (uint8_t)((d >= 2 ? (mask == 0xFE ? 2 : 3) : (mask == 0xFE ? 4 : 5)) | 128);
        at += 5;
    }
    return len;
}

// ---- the SID in a socket (FW u64_config.cc U64SidSockets::detect, detectFPGASID, detectRemakes, S_SidDetector, DetectSidImpl) ---------------------
// Run once at the start, as FW runs it in U64Config's constructor, on the machine let out of reset and stopped, over the DMA window. Types as sid_type_names:
// 1 6581, 2 8580, 3 FPGASID, 4 SwinSID, 5 ARMSID, 6 ARM2SID, 8 FPGASID Dukestah, 9 PDsid, 10 SIDKick, 11 SIDKick Pico. A found FPGASID is set up from its settings
// as SidDeviceFpgaSid does (sid_device_fpgasid.cc:126-144, S_effectuate :235); the Dukestah probe follows FW detectDukestahAdapter (u64_config.cc:746).
// The chips are reached through the C64's memory window (api->c64), one byte per access: register r of socket s is $D400 + $100 * s + r.
static uint8_t sr_rd(int s, uint32_t r) { return c64_peek(0xD400u + 0x100u * (uint32_t)s + r); }
static void sr_wr(int s, uint32_t r, uint8_t v) { c64_poke(0xD400u + 0x100u * (uint32_t)s + r, v); }
#define PEEK2() ((void)c64_peek(2))

static void detect_addresses(void)
{
    // FW S_SetupDetectionAddresses: socket 1 at $D400, socket 2 at $D500, the UltiSIDs out of the way
    HW_WR_I(c64_sid_addr_base, 0, 0x40);
    HW_WR_I(c64_sid_addr_base, 1, 0x50);
    HW_WR_I(c64_sid_addr_mask, 0, 0xF0);
    HW_WR_I(c64_sid_addr_mask, 1, 0xF0);
    HW_WR_I(c64_sid_addr_base, 2, 0x60);
    HW_WR_I(c64_sid_addr_base, 3, 0x60);
    HW_WR_I(c64_sid_addr_mask, 2, 0xFE);
    HW_WR_I(c64_sid_addr_mask, 3, 0xFE);
    HW_WR(c64_sid_sock_socket1_en, 1);
    HW_WR(c64_sid_sock_socket2_en, 1);
    HW_WR(c64_sid_sock_stereo_addrsel, 0);
    wait_ms(1);
}

// FW U64Config::detectFPGASID (:781) and the device's constructor (sid_device_fpgasid.cc:126): in DIAG mode the unique ID (bytes 4..11) and the revision (byte 3) are read
struct FsidDev {
    bool dev;
    uint8_t uid[8], rev;
};
static FsidDev g_fsid[2];
static bool g_dukestah;                                          // FW FpgaSidConfig::dukestahAdapterPresent

static int detect_fpgasid(int s)
{
    sr_wr(s, 25, 0xEE);
    sr_wr(s, 26, 0xAB);
    uint8_t id1 = sr_rd(s, 0), id2 = sr_rd(s, 1);
    bool is = id1 == 0x1D && id2 == 0xF5;
    if (is) {
        for (int i = 0; i < 8; i++) {
            g_fsid[s].uid[i] = sr_rd(s, (uint32_t)i + 4);
        }
        g_fsid[s].rev = sr_rd(s, 3);
        g_fsid[s].dev = true;
    }
    sr_wr(s, 25, 0x00);
    sr_wr(s, 26, 0x00);
    return is ? 3 : 0;
}

// sys.delay_us: the busy wait of the chips' register handshakes; nothing without the member
static void delay_us(uint32_t us)
{
    const SvcSys *x = g_api->sys;
    if (SVC_HAS(SvcSys, x, delay_us)) {
        x->delay_us(us);
    }
}

static int detect_remakes(int s)
{
    sr_wr(s, 29, 'S'); delay_us(10);
    sr_wr(s, 30, 'I'); delay_us(10);
    sr_wr(s, 31, 'D'); delay_us(10);
    wait_ms(10);
    uint8_t id1 = sr_rd(s, 27);
    delay_us(10);
    uint8_t id2 = sr_rd(s, 28);
    delay_us(10);
    if (id1 == 'S' && id2 == 'W') {
        sr_wr(s, 29, 0);
        return 4;
    }
    if (id1 == 'N' && id2 == 'O') {
        sr_wr(s, 31, 'I'); delay_us(10);
        sr_wr(s, 30, 'I'); delay_us(10);
        wait_ms(10);
        uint8_t l = sr_rd(s, 27);
        delay_us(10);
        sr_wr(s, 29, 0);
        delay_us(10);
        return (l == 'L' || l == 'R') ? 6 : 5;
    }
    sr_wr(s, 29, 'P');
    PEEK2();
    sr_wr(s, 30, 'D');
    PEEK2();
    if (sr_rd(s, 30) == 'S') {
        return 9;
    }
    uint8_t v[32];
    sr_wr(s, 0x1f, 0xff);
    for (int i = 0; i < 32; i++) {
        sr_wr(s, 0x1e, (uint8_t)(224 + i));
        v[i] = sr_rd(s, 0x1d);
    }
    sr_wr(s, 0x1f, 0xfd);
    static const uint8_t pico[] = { 0x53, 0x4b, 0x10, 0x09, 0x03, 0x0f };
    static const uint8_t teen[] = { 0x53, 0x49, 0x44, 0x4b, 0x09, 0x03, 0x0b };
    if (!memcmp(v, pico, sizeof(pico))) {
        return 11;
    }
    if (!memcmp(v, teen, sizeof(teen))) {
        return 10;
    }
    return 0;
}

// OSC3 of both sockets: voltage-independent 6581 / 8580 test. buf[0..15] socket 1 first read, 16..31 second, 32.. socket 2 (DetectSidImpl)
static bool detect_osc3(uint8_t *buf)
{
    uint32_t timeout = 1000000;
    while (c64_peek(0xD012) != 0xFF) {
        if (!timeout--) {
            return false;
        }
    }
    for (int x = 0; x < 16; x++) {
        for (int s = 0; s < 2; s++) {
            sr_wr(s, 0x12, 0x48);
            sr_wr(s, 0x0F, 0x48);
            sr_wr(s, 0x12, 0x24);
            for (int i = 0; i < 3; i++) { sr_wr(s, 0x1F, 0); }
            buf[32 * s + x] = sr_rd(s, 0x1B);
            for (int i = 0; i < 4; i++) { sr_wr(s, 0x1F, 0); }
            buf[32 * s + 16 + x] = sr_rd(s, 0x1B);
        }
    }
    return true;
}

// ---- the FPGASID's settings (FW sid_device_fpgasid.cc fpga_sid_config :63): keys "FPGASID <n> <item>", n = socket 1 or 2 ------------------------------------------------
enum { F_MODE, F_OUT, F_LEDS, F_SID0, F_N = F_SID0 + 20 };         // then per SID (1, 2): quick, filter, crunchy, mixed, delay, readback, extin, digifix, bias, voices
enum { FS_QUICK, FS_FILTER, FS_CRUNCHY, FS_MIXED, FS_DELAY, FS_READBACK, FS_EXTIN, FS_DIGIFIX, FS_BIAS, FS_VOICES };
static const char *const fsid_items[3 + 10] = { "Fundamental Mode", "Output Mode", "LEDs", "Quick type select", "Filter Mode", "Crunchy DAC", "Mixed Waveforms",
                                                "Register Delay", "Register Read Back", "EXT IN Source", "DigiFix Value", "6581 Filter Bias", "Enabled Voices" };
static uint8_t g_flast[2][8];                                      // the bytes last written to a chip
static bool g_fhave[2];

static const char *fsid_key(int s, int i, char *key, int n)
{
    if (i < 3) {
        snprintf(key, n, "FPGASID %d %s", s + 1, fsid_items[i]);
    } else {
        snprintf(key, n, "FPGASID %d SID%d %s", s + 1, (i - F_SID0) / 10 + 1, fsid_items[3 + (i - F_SID0) % 10]);
    }
    return key;
}

// the value of an item: the index plus the base of a range (DigiFix -128 .. 127, filter bias -8 .. 7)
static int FV(int s, int id)
{
    char key[48];
    return setting_num(fsid_key(s, id, key, sizeof(key)), 0);
}

static void fsid_bytes_of(int s, uint8_t r[8])
{
    FsidSet f;
    f.mode = FV(s, F_MODE);
    f.outmode = FV(s, F_OUT);
    f.leds = FV(s, F_LEDS);
    for (int k = 0; k < 2; k++) {
        int o = F_SID0 + 10 * k;
        f.sid[k] = { FV(s, o + FS_FILTER), FV(s, o + FS_CRUNCHY), FV(s, o + FS_MIXED), FV(s, o + FS_DELAY), FV(s, o + FS_READBACK),
                     FV(s, o + FS_EXTIN), FV(s, o + FS_DIGIFIX), FV(s, o + FS_BIAS), FV(s, o + FS_VOICES) };
    }
    fsid_bytes(f, g_dukestah && s == 0, r);
}

// FW FpgaSidConfig::S_effectuate: SID 1 in config mode ($80), SID 2 through the swap ($82), then out. The socket is addressed and the machine stopped.
static void fsid_put(int s)
{
    uint8_t r[8];
    fsid_bytes_of(s, r);
    for (int k = 0; k < 2; k++) {
        sr_wr(s, 25, k ? 0x82 : 0x80);
        sr_wr(s, 26, 0x65);
        sr_wr(s, 30, r[4 * k]);
        sr_wr(s, 31, r[4 * k + 1]);
        sr_wr(s, 28, r[4 * k + 2]);
        sr_wr(s, 29, r[4 * k + 3]);
    }
    sr_wr(s, 25, 0);
    sr_wr(s, 26, 0);
    memcpy(g_flast[s], r, 8);
    g_fhave[s] = true;
}

// FW access_socket_pre/post (u64_config.cc:2915): the machine stops, the sockets are at the detection addresses, and afterwards the addresses are the settings' again
static bool g_in_detect;                                          // the start: the machine is stopped and the sockets are at the detection addresses already
static bool sock_run(int s, void (*fn)(int))
{
    if (g_in_detect) {
        fn(s);
        return true;
    }
    if (!c64_window()) {
        return false;
    }
    bool was = (CART_RD(stop) & DRV_cart_regs_stop_has_stopped_MASK) != 0;
    if (!was) {
        c64_stop(true);
        uint32_t t0 = now_ms();
        while (!(CART_RD(stop) & DRV_cart_regs_stop_has_stopped_MASK) && now_ms() - t0 < 500) {
        }
        if (!(CART_RD(stop) & DRV_cart_regs_stop_has_stopped_MASK)) {
            c64_stop(false);
            return false;
        }
    }
    detect_addresses();
    fn(s);
    write_sid_addressing();
    HW_WR(c64_sid_sock_socket1_en, V(K_S1_EN) ? 1 : 0);
    HW_WR(c64_sid_sock_socket2_en, V(K_S2_EN) ? 1 : 0);
    if (!was) {
        c64_stop(false);
    }
    return true;
}
static bool fsid_write(int s) { return sock_run(s, fsid_put); }

// a settings change (FW's change hooks, :180-203 of the device, all rewrite what the item makes of the bytes): written when the bytes differ from the chip's
static int fsid_sync(int s)
{
    if (!g_fsid[s].dev) {
        return OPE_OK;
    }
    uint8_t r[8];
    fsid_bytes_of(s, r);
    if (g_fhave[s] && !memcmp(r, g_flast[s], 8)) {
        return OPE_OK;
    }
    return fsid_write(s) ? OPE_OK : OPE_FAILED;
}

// FW S_cfg_fpgasid_sidN_quick (:290): one value sets filter, crunchy DAC, mixed waveforms, register delay and (x3) the read back. The first call is the start applying
// the stored value. The five are stored without their apply (settings.set): the bytes go to the chip once, below.
static int fsid_quick(int s, int k, int value)
{
    static int last[2][2] = { { -1, -1 }, { -1, -1 } };
    if (!g_fsid[s].dev || last[s][k] == value || last[s][k] < 0) {
        last[s][k] = value;
        return OPE_OK;
    }
    last[s][k] = value;
    int o = F_SID0 + 10 * k;
    const int ids[5] = { FS_FILTER, FS_CRUNCHY, FS_MIXED, FS_DELAY, FS_READBACK };
    for (int i = 0; i < 5; i++) {
        char key[48];
        if (setting_known(fsid_key(s, o + ids[i], key, sizeof(key)))) {
            setting_set(key, i == 4 ? value * 3 : value);
        }
    }
    return fsid_sync(s);
}

int fx_fsid(CoreCtx *, const OpArgs &, OpOut &)
{
    int rc = OPE_OK;
    for (int s = 0; s < 2; s++) {
        rc |= fsid_sync(s);
    }
    return rc;
}
int fx_fsid_q10(CoreCtx *, const OpArgs &a, OpOut &) { return fsid_quick(0, 0, (int)a.i(0)); }
int fx_fsid_q11(CoreCtx *, const OpArgs &a, OpOut &) { return fsid_quick(0, 1, (int)a.i(0)); }
int fx_fsid_q20(CoreCtx *, const OpArgs &a, OpOut &) { return fsid_quick(1, 0, (int)a.i(0)); }
int fx_fsid_q21(CoreCtx *, const OpArgs &a, OpOut &) { return fsid_quick(1, 1, (int)a.i(0)); }

// ---- the other chips with a page of their own (FW sid_device_swinsid.cc, sid_device_armsid.cc, sid_device_pdsid.cc, sid_device_sidkick.cc) ----------------------------
// keys "<chip> <n> <item>"; the device is made where the probe finds it (u64_config.cc:826-857), SwinSID sets itself up from its settings, the others read what the chip is in
enum { CK_SWIN, CK_ARM, CK_PD, CK_KICK, CK_N };
static const struct {
    const char *pre;
    int n;
    const char *item[5];
} ck_def[CK_N] = {
    { "SwinSID", 3, { "Fundamental Mode", "Pitch", "Audio In", 0, 0 } },
    { "ARMSID", 5, { "Fundamental Mode", "6581 Filter Strength", "6581 Lowest Filt Freq", "8580 Highest Filt Freq", "8580 Lowest Filt Freq" } },
    { "PDsid", 1, { "Emulation Mode", 0, 0, 0, 0 } },
    { "SidKick", 1, { "Emulation Mode", 0, 0, 0, 0 } } };
static int g_ckind[2] = { -1, -1 };
static int g_clast[2][5];
static bool g_chave[2];
static char g_armver[2][12];
static bool g_flash_save;                                         // a change the person made: the ARMSID's own flash takes it, not only its RAM

static int ck_of_type(int t)
{
    switch (t) {
    case 4: return CK_SWIN;
    case 5: case 6: return CK_ARM;
    case 9: return CK_PD;
    case 10: case 11: return CK_KICK;
    }
    return -1;
}

static const char *ck_key(int s, int i, char *key, int n)
{
    snprintf(key, n, "%s %d %s", ck_def[g_ckind[s]].pre, s + 1, ck_def[g_ckind[s]].item[i]);
    return key;
}

static int CV(int s, int i)
{
    char key[48];
    return setting_ix(ck_key(s, i, key, sizeof(key)), 0);
}

static void ck_store(int s, int i, int v)                           // the value the chip is in becomes the setting (FW setValueQuietly, clamped as check_bounds)
{
    char key[48];
    ck_key(s, i, key, sizeof(key));
    if (setting_known(key)) {
        int mx = setting_count(key) - 1;
        setting_set(key, v > mx ? mx : v);
    }
}

static void ck_snapshot(int s)
{
    for (int i = 0; i < ck_def[g_ckind[s]].n; i++) {
        g_clast[s][i] = CV(s, i);
    }
    g_chave[s] = true;
}

static void ck_put(int s)
{
    switch (g_ckind[s]) {
    case CK_SWIN:                                                  // S_effectuate, sid_device_swinsid.cc:84
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'E'); PEEK2();
        sr_wr(s, 31, CV(s, 0) ? '8' : '6');
        wait_ms(5);
        sr_wr(s, 31, CV(s, 1) ? 'S' : 'L');
        wait_ms(5);
        sr_wr(s, 31, CV(s, 2) ? 'A' : 'D');
        wait_ms(5);
        sr_wr(s, 29, 0);
        break;
    case CK_ARM: {                                                 // S_effectuate and S_set_filt, sid_device_armsid.cc:178, :236
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        sr_wr(s, 31, 'D');
        wait_ms(10);
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'E'); PEEK2();
        sr_wr(s, 31, CV(s, 0) ? '8' : '6');
        wait_ms(10);
        const int v[4] = { CV(s, 1) - 7, CV(s, 2) - 1, CV(s, 3) - 3, CV(s, 4) - 3 };
        for (int i = 0; i < 4; i++) {
            sr_wr(s, 31, (uint8_t)(((uint8_t)(v[i] & 15)) | (0x80 + 0x10 * i)));
            PEEK2();
            sr_wr(s, 30, 'E');
            wait_ms(10);
        }
        sr_wr(s, 31, 0xC0); PEEK2();                               // S_save_ram
        sr_wr(s, 30, 'E');
        wait_ms(10);
        sr_wr(s, 29, 0);
        if (g_flash_save) {                                        // ArmSidConfig::write (:192): S_config_mode, S_save_flash. Every setting is saved at once here (D9-D11), as FW's save of the config
            sr_wr(s, 29, 'S'); PEEK2();
            sr_wr(s, 30, 'I'); PEEK2();
            sr_wr(s, 31, 'D');
            wait_ms(10);
            sr_wr(s, 31, 0xCF); PEEK2();                           // S_save_flash
            sr_wr(s, 30, 'E');
            wait_ms(10);
            sr_wr(s, 29, 0);
        }
        break;
    }
    case CK_PD:                                                    // SetSidType, sid_device_pdsid.cc:39
        sr_wr(s, 29, 'P'); PEEK2();
        sr_wr(s, 30, 'D'); PEEK2();
        sr_wr(s, 31, CV(s, 0) ? 0x01 : 0x00); PEEK2();
        sr_wr(s, 29, 0); PEEK2();
        break;
    case CK_KICK:                                                  // SidKickConfig::effectuate, sid_device_sidkick.cc:117
        sr_wr(s, 0x1f, 0xFF);
        sr_wr(s, 0x1e, 0x00);
        sr_wr(s, 0x1d, (uint8_t)(CV(s, 0) & 3));
        sr_wr(s, 0x1f, 0xFD);
        wait_ms(5);
        break;
    }
}

// what the chip is in, into the settings: ARMSID readParams (sid_device_armsid.cc:101), PDsid at_open_config (:89), SidKick at_open_config (:146)
static void ck_read(int s)
{
    switch (g_ckind[s]) {
    case CK_ARM: {
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        sr_wr(s, 31, 'D');
        wait_ms(2);
        sr_wr(s, 31, 'V'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        wait_ms(2);
        uint8_t vh = sr_rd(s, 27); PEEK2();
        uint8_t vl = sr_rd(s, 28); PEEK2();
        sr_wr(s, 31, 'F'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        wait_ms(2);
        uint8_t chip = sr_rd(s, 27); PEEK2();
        sr_wr(s, 31, 'U'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        wait_ms(10);
        (void)sr_rd(s, 27); PEEK2();
        (void)sr_rd(s, 28);
        sr_wr(s, 31, 'H'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        wait_ms(10);
        uint8_t p = sr_rd(s, 27); PEEK2();
        uint8_t q = sr_rd(s, 28);
        ck_store(s, 1, ((p >> 4) + 7) & 15);
        ck_store(s, 2, ((p & 15) + 1) & 15);
        ck_store(s, 3, ((q >> 4) + 3) & 15);
        ck_store(s, 4, ((q & 15) + 3) & 15);
        ck_store(s, 0, chip == '6' ? 0 : 1);
        snprintf(g_armver[s], sizeof(g_armver[s]), "%d.%d", vh, vl);
        sr_wr(s, 29, 0);
        break;
    }
    case CK_PD:
        sr_wr(s, 29, 'P'); PEEK2();
        sr_wr(s, 30, 'D'); PEEK2();
        ck_store(s, 0, sr_rd(s, 31) & 1);
        sr_wr(s, 29, 0);
        break;
    case CK_KICK: {
        sr_wr(s, 0x1f, 0xFF);
        sr_wr(s, 0x1e, 0x00);
        uint8_t first = 0;
        for (int i = 0; i < 64; i++) {
            uint8_t v = sr_rd(s, 0x1d);
            first = i ? first : v;
        }
        sr_wr(s, 0x1f, 0xFD);
        ck_store(s, 0, first & 3);
        wait_ms(5);
        break;
    }
    }
}

// FW SidDeviceArmSid::at_open_config (:177): the page opens, the chip's own settings are read again (and PDsid, SidKick the same, :89, :146)
static int ck_open(int s)
{
    if (!settings_ready() || g_ckind[s] < 0 || g_ckind[s] == CK_SWIN) {
        return OPE_OK;
    }
    if (!sock_run(s, ck_read)) {
        return OPE_FAILED;
    }
    ck_snapshot(s);
    return OPE_OK;
}
int fx_arm_read1(CoreCtx *, const OpArgs &, OpOut &) { return ck_open(0); }
int fx_arm_read2(CoreCtx *, const OpArgs &, OpOut &) { return ck_open(1); }

// the device of a probe that found this type: SwinSID effectuates its stored settings, the others take what the chip is in
static void ck_init(int s, int type)
{
    g_ckind[s] = ck_of_type(type);
    g_chave[s] = false;
    g_armver[s][0] = 0;
    if (g_ckind[s] < 0) {
        return;
    }
    if (g_ckind[s] == CK_SWIN) {
        ck_put(s);
    } else {
        ck_read(s);
    }
    ck_snapshot(s);
}

int fx_chip(CoreCtx *, const OpArgs &, OpOut &)
{
    int rc = OPE_OK;
    for (int s = 0; s < 2; s++) {
        if (g_ckind[s] < 0 || !settings_ready()) {
            continue;
        }
        bool same = g_chave[s];
        for (int i = 0; i < ck_def[g_ckind[s]].n; i++) {
            same = same && g_clast[s][i] == CV(s, i);
        }
        if (same) {
            continue;
        }
        g_flash_save = true;
        const bool ok = sock_run(s, ck_put);
        g_flash_save = false;
        if (ok) {
            ck_snapshot(s);
        } else {
            rc = OPE_FAILED;
        }
    }
    return rc;
}

// FW U64Config::detectDukestahAdapter (:746): SID 1 of the FPGASID in stereo mode, SID 2's register 28 reads back what socket 2's address wrote when the adapter
// maps socket 2 onto it. The probe leaves the chip as its writes did, so the device's settings go in again (:773-774).
static bool detect_dukestah(void)
{
    sr_wr(0, 25, 0x81);
    sr_wr(0, 26, 0x65);
    sr_wr(0, 30, 4);
    sr_wr(0, 25, 0x82);
    sr_wr(0, 26, 0x65);
    sr_wr(0, 28, 3);
    sr_wr(0, 25, 0x81);
    sr_wr(0, 26, 0x65);
    sr_wr(0, 28, 5);
    sr_wr(1, 28, 13);
    sr_wr(0, 25, 0x82);
    sr_wr(0, 26, 0x65);
    uint8_t tmp1 = sr_rd(0, 28) & 15;
    sr_wr(0, 25, 0x00);
    sr_wr(0, 26, 0x00);
    g_dukestah = tmp1 == 13;
    fsid_put(0);
    return g_dukestah;
}

static void detect_sids(int type[2])
{
    c64_ram_memonly(false);                                           // the SIDs are on the bus, not in the RAM behind them
    type[0] = type[1] = 0;
    memset(g_fsid, 0, sizeof(g_fsid));
    memset(g_fhave, 0, sizeof(g_fhave));
    g_dukestah = false;
    g_ckind[0] = g_ckind[1] = -1;
    g_in_detect = true;
    for (int s = 0; s < 2; s++) {
        detect_addresses();
        type[s] = detect_fpgasid(s);
    }
    for (int s = 0; s < 2; s++) {
        if (g_fsid[s].dev) {
            fsid_put(s);                                              // FW: the device's constructor effectuates its stored settings
        }
    }
    if (!type[0] || !type[1]) {
        wait_ms(100);
        for (int s = 0; s < 2; s++) {
            detect_addresses();
            if (!type[s]) {
                type[s] = detect_remakes(s);
            }
        }
    }
    for (int s = 0; s < 2; s++) {
        ck_init(s, type[s]);                                         // FW: the device of the chip the probe found
    }
    if (!type[0] || !type[1]) {
        detect_addresses();
        uint8_t buf[64];
        for (int attempt = 0; attempt < 3; attempt++) {
            int r[2] = { 0, 0 };
            if (!detect_osc3(buf)) {
                break;
            }
            bool ok = true;
            for (int s = 0; s < 2; s++) {
                r[s] = sid_osc3_type(buf + 32 * s);
                ok &= r[s] != 0 || type[s] != 0;                      // FW: a socket that answered nothing sane gives the next attempt
            }
            for (int s = 0; s < 2; s++) {
                if (r[s] && !type[s]) {
                    type[s] = r[s];
                }
            }
            if (ok) {
                break;
            }
        }
    }
    if (type[0] == 3 && type[1] == 0) {                              // FW u64_config.cc:904
        detect_addresses();
        if (detect_dukestah()) {
            type[0] = type[1] = 8;
        }
    }
    g_in_detect = false;
    lib_logf("[u64cfg] SID detect: socket 1 = %d, socket 2 = %d", type[0], type[1]);
}

static const char *const type_keys[2] = { "Pad SID Detected Socket 1", "Pad SID Detected Socket 2" };   // the settings keep only "Pad " extras (Settings::extra_set)
static bool g_detected;

// FW U64SidSockets::detect and its tail: the machine is let out of reset and stopped, the chips are looked at, the type is stored, and a changed type
// sets the socket's enable, caps and shunt (sid_type_change). The machine goes back into reset: boot_start's init holds it until the release. The
// supply of a 6581 is only ever raised for a type 1 (write_sid_sockets) that the read-outs agreed on, and a new one switches its socket off. If the
// machine does not stop in time nothing is written to a chip and every type stays 0 (FW would have used the stored type; a swapped chip must never
// keep a 6581's supply). The type is stored after FW's CFG_SID1_TYPE (the settings file), and compared with the one of the last start. Without the
// C64's memory window (api->c64) no chip can be looked at: the detection waits for a later call.
static void sid_types_detect(void)
{
    const SvcSettings *set = g_api->settings;
    if (g_detected || (ctx_state() & (CTX_STARTED | CTX_FROZEN)) || !set || !settings_ready() || !c64_window()) {
        return;
    }
    g_detected = true;
    c64_mode_reset(false);                                            // C64_MODE_UNRESET
    uint32_t t0 = now_ms();
    while ((CART_RD(clock_detect) & DRV_cart_regs_clock_detect_reset_seen_MASK) && now_ms() - t0 < 500) {     // while c64_reset_detect()
    }
    CART_WR(stop_mode, DRV_cart_regs_stop_mode_force);
    c64_stop(true);
    t0 = now_ms();
    while (!(CART_RD(stop) & DRV_cart_regs_stop_has_stopped_MASK) && now_ms() - t0 < 500) {
    }
    int t[2] = { 0, 0 };
    bool stopped = (CART_RD(stop) & DRV_cart_regs_stop_has_stopped_MASK) != 0;
    if (stopped) {
        detect_sids(t);
    } else {
        lib_logf("[u64cfg] SID detect: the machine did not stop, no chip was touched");
    }
    c64_stop(false);
    c64_mode_reset(true);
    if (!stopped) {
        return;
    }
    char note[80] = "";
    for (int s = 0; s < 2; s++) {
        g_sid_type[s] = t[s];
        char v[8];
        int old = set->extra_get(type_keys[s], v, sizeof(v)) ? atoi(v) : 0;
        if (t[s] == old) {
            continue;
        }
        snprintf(v, sizeof(v), "%d", t[s]);
        set->extra_set(type_keys[s], v);
        SidChange ch = sid_type_change(t[s]);
        int key[3] = { s ? K_S2_EN : K_S1_EN, s ? K_S2_CAPS : K_S1_CAPS, s ? K_S2_SHUNT : K_S1_SHUNT };
        int val[3] = { ch.enable, ch.caps, ch.shunt };
        for (int k = 0; k < 3; k++) {
            if (val[k] >= 0 && setting_known(key_names[key[k]])) {
                setting_set(key_names[key[k]], val[k]);                    // the start, before the executor: one task, no menu yet; a write of the settings file at most
            }
        }
        snprintf(note, sizeof(note), "SID changed. Please review settings");
    }
    const SvcCtx *x = g_api->ctx;
    if (note[0] && x) {
        x->notice(note, true);                                         // only when no notice waits (this file's rule)
    }
}

// The sockets' supply, shunt and capacitors (FW U64SidSockets::effectuate_settings): one call per socket, the base packs the bits and writes the board's
// expander (board.sid_socket); then the sockets' enables.
static void write_sid_sockets(void)
{
    sid_types_detect();
    const SvcBoard *b = g_api->board;
    for (int s = 0; s < 2; s++) {
        int reg = SID_SOCKET_OFF;
        if (V(s ? K_S2_EN : K_S1_EN)) {
            reg = g_sid_type[s] == 1 ? SID_SOCKET_ON_TYPE1 : SID_SOCKET_ON;
        }
        int rc = b ? b->sid_socket(s, reg, V(s ? K_S2_SHUNT : K_S1_SHUNT) != 0, V(s ? K_S2_CAPS : K_S1_CAPS) != 0) : 0;
        if (rc < 0) {
            lib_logf("[u64cfg] SID socket %d: the board refused its settings (%d)", s + 1, rc);
        }
    }
    HW_WR(c64_sid_sock_socket1_en, V(K_S1_EN) ? 1 : 0);
    HW_WR(c64_sid_sock_socket2_en, V(K_S2_EN) ? 1 : 0);
}

// The reSID8 filter (software/ultimate-app/software/io/c64/trx_io.cc trx_io_resid8, _filter, _bypass): the TOML A's block resid8, a core block (Alex's
// split, U18 §5 2026-11-24: the SID settings are this library's, so no filter call of the SID library is needed), written only when ident's caps3 says
// the SID is the 8-SID reDIP.
static bool trx_resid8(void)
{
    return g_api->io && (DRV_ident_caps3_RD() & DRV_ident_caps3_resid8_MASK);
}

static void trx_resid8_filter(int u, uint8_t src, uint8_t auto_bits, int base, int offset)
{
    if (!trx_resid8()) {
        return;
    }
    u &= 1;
    base &= 0x1FF;
    offset &= 0x7FF;
    DRV_resid8_fcsel_WR(u, src & 3);
    DRV_resid8_auto_WR(u, auto_bits & 3);
    DRV_resid8_curve_base_WR(u, (uint16_t)base);                       // 9 bit, one 16-bit write: low byte first
    DRV_resid8_curve_offset_WR(u, (uint16_t)offset);                   // 11 bit
}

static void trx_resid8_bypass(int u, bool bypass)
{
    if (trx_resid8()) {
        DRV_resid8_bypass_WR(u & 1, bypass ? 1 : 0);
    }
}

// FW u64_config.cc setResid8 :577-594: the plan of resid8_filter.h (a copy of the FW header) -> trx_io 0x040-0x04C and the UltiSID RES / DIGI registers.
// The custom curve's base and offset are the numbers the menu shows (0 .. 511, -1024 .. 1023).
static void write_ultisid(void)
{
    for (int u = 0; u < 2; u++) {
        Resid8Plan p = resid8_plan(V(u ? K_U2_FILTER : K_U1_FILTER), V(u ? K_U2_RES : K_U1_RES), V(u ? K_U2_DIGI : K_U1_DIGI),
                                   VN(u ? K_U2_BASE : K_U1_BASE, 245), VN(u ? K_U2_OFFS : K_U1_OFFS, -780), V(u ? K_U2_FON : K_U1_FON) == 0);
        if (u == 0) {
            HW_WR(c64_sid_cfg_res1, p.res_reg);
            HW_WR(c64_sid_cfg_digi1, p.digi_reg);
        } else {
            HW_WR(c64_sid_cfg_res2, p.res_reg);
            HW_WR(c64_sid_cfg_digi2, p.digi_reg);
        }
        trx_resid8_filter(u, p.src, p.auto_bits, p.base, p.offset);
        trx_resid8_bypass(u, p.bypass != 0);
    }
    HW_WR(c64_sid_cfg_waves1, (uint8_t)V(K_U1_WAVES));
    HW_WR(c64_sid_cfg_waves2, (uint8_t)V(K_U2_WAVES));
}

int fx_sid(CoreCtx *, const OpArgs &, OpOut &)
{
    write_sid_sockets();
    write_sid_addressing();
    write_ultisid();
    HW_WR(c64_sid_sock_paddle_en, (uint8_t)V(K_PADDLE, 1));
    return OPE_OK;
}

// "$D400 mirrored on ..." style lines of the decodes for the viewer: one row per decode with A9 .. A5 as Gideon's show_mapping prints them
static const char *state_sid_map_n(int i)
{
    static char line[4][64];
    static const char *const names[4] = { "Socket 1", "Socket 2", "UltiSID 1", "UltiSID 2" };
    int p = snprintf(line[i], sizeof(line[i]), "%s  ", names[i]);
    for (int a = 9 - 4; a >= 5 - 4; a--) {
        int m = (g_mask[i] >> a) & 1, b = (g_base[i] >> a) & 1;
        p += snprintf(line[i] + p, sizeof(line[i]) - p, "%s", m ? (b ? "1 " : "0 ") : (b ? "#! " : "- "));
    }
    snprintf(line[i] + p, sizeof(line[i]) - p, " $%02X00+$%02X", 0xD0 + (g_base[i] >> 4) * 1, (g_base[i] & 0x1F) * 2);
    return line[i];
}
const char *state_sid_map0(CoreCtx *) { return state_sid_map_n(0); }
const char *state_sid_map1(CoreCtx *) { return state_sid_map_n(1); }
const char *state_sid_map2(CoreCtx *) { return state_sid_map_n(2); }
const char *state_sid_map3(CoreCtx *) { return state_sid_map_n(3); }

static const char *const sid_type_names[] = { "None", "6581", "8580", "FPGASID", "SwinSID Ultimate", "ARMSID", "ARM2SID", "SidFx", "FPGASID Dukestah",
                                              "PDsid", "SIDKick (Teensy)", "SIDKick Pico" };
const char *u64cfg_sid_type_name(int type) { return type >= 0 && type < 12 ? sid_type_names[type] : "?"; }
const char *state_sid1(CoreCtx *) { return u64cfg_sid_type_name(g_sid_type[0]); }
const char *state_sid2(CoreCtx *) { return u64cfg_sid_type_name(g_sid_type[1]); }

static const char *fsid_uid(int s)
{
    static char t[2][20];
    if (!g_fsid[s].dev) {
        return "";
    }
    for (int i = 0; i < 8; i++) {
        snprintf(t[s] + 2 * i, 3, "%02x", g_fsid[s].uid[i]);
    }
    return t[s];
}
static const char *fsid_rev(int s)
{
    static char t[2][4];
    if (!g_fsid[s].dev) {
        return "";
    }
    snprintf(t[s], sizeof(t[s]), "%02x", g_fsid[s].rev);
    return t[s];
}
// the info lines of the page hide while there is no device (a node's `hide`: "not shown while the state has a value")
const char *state_fsid1_none(CoreCtx *) { return g_fsid[0].dev ? "" : "none"; }
const char *state_fsid2_none(CoreCtx *) { return g_fsid[1].dev ? "" : "none"; }
static const char *ck_state(int s, int k) { return g_ckind[s] == k ? "yes" : ""; }
static const char *ck_none(int s, int k) { return g_ckind[s] == k ? "" : "none"; }
const char *state_s1_swin(CoreCtx *) { return ck_state(0, CK_SWIN); }
const char *state_s2_swin(CoreCtx *) { return ck_state(1, CK_SWIN); }
const char *state_s1_arm(CoreCtx *) { return ck_state(0, CK_ARM); }
const char *state_s2_arm(CoreCtx *) { return ck_state(1, CK_ARM); }
const char *state_s1_pd(CoreCtx *) { return ck_state(0, CK_PD); }
const char *state_s2_pd(CoreCtx *) { return ck_state(1, CK_PD); }
const char *state_s1_kick(CoreCtx *) { return ck_state(0, CK_KICK); }
const char *state_s2_kick(CoreCtx *) { return ck_state(1, CK_KICK); }
const char *state_s1_noarm(CoreCtx *) { return ck_none(0, CK_ARM); }
const char *state_s2_noarm(CoreCtx *) { return ck_none(1, CK_ARM); }
const char *state_s1_armver(CoreCtx *) { return g_armver[0]; }
const char *state_s2_armver(CoreCtx *) { return g_armver[1]; }

const char *state_fsid1_uid(CoreCtx *) { return fsid_uid(0); }
const char *state_fsid2_uid(CoreCtx *) { return fsid_uid(1); }
const char *state_fsid1_rev(CoreCtx *) { return fsid_rev(0); }
const char *state_fsid2_rev(CoreCtx *) { return fsid_rev(1); }
// FW sid_device_fpgasid.cc:134: output mode and LEDs are disabled for a revision up to 9
const char *state_fsid1_new(CoreCtx *) { return g_fsid[0].dev && g_fsid[0].rev > 9 ? "yes" : ""; }
const char *state_fsid2_new(CoreCtx *) { return g_fsid[1].dev && g_fsid[1].rev > 9 ? "yes" : ""; }

// ---- the SID player's autoconfig (T228 R21 item 7) ---------------------------------------------------------------------------------------------
// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/u64/u64_config.cc (Gideon Zweijtzer, GPL-3.0): U64Config::SidAutoConfig (:2409), MapSid (:2318),
// SetSidAddress (:2253), GetSidType (:2159), unmapAllSids (:2397), SetMixerAutoSid (:2352), SetSidType (:2305), access_socket_pre / post (:2915, :2938) and the
// SetSidType of sid_device_fpgasid.cc:97, sid_device_swinsid.cc:35, sid_device_armsid.cc:318, sid_device_pdsid.cc:39, sid_device_sidkick.cc:61. What is different:
// FW's *_BAK shadows are g_base / g_mask / g_stereo_bak (the values the settings last wrote, which the map view shows), the stored socket type is g_sid_type
// (the last detection), the machine is stopped by the caller (FW's load() has it stopped, so access_socket_pre does not stop it), no interrupt lock, and FW's
// skipReset is not needed: this application writes the settings again only when it resets the machine itself (boot_apply_settings).
#define UNMAPPED_BASE 0x01
#define UNMAPPED_MASK 0xFE

static void unmap_all_sids(void)
{
    for (int i = 0; i < 4; i++) {                                   // socket 1, socket 2, UltiSID 1, UltiSID 2
        g_base[i] = UNMAPPED_BASE;
        g_mask[i] = UNMAPPED_MASK;
        HW_WR_I(c64_sid_addr_base, i, UNMAPPED_BASE);
        HW_WR_I(c64_sid_addr_mask, i, UNMAPPED_MASK);
    }
}

// the types a logical SID can be: 0 none, 1 6581, 2 8580, 3 either (a chip that can be told)
static uint8_t get_sid_type(int slot)
{
    switch (slot) {
    case 0:
    case 1: {
        if (!V(slot ? K_S2_EN : K_S1_EN)) {
            return 0;
        }
        switch (g_sid_type[slot]) {
        case 1: return 1;
        case 2: return 2;
        case 3: case 5: case 6: case 4: case 8: return 3;          // FPGASID, ARMSID, ARM2SID, SwinSID, Dukestah: we can TELL the chip its type
        default: return 0;
        }
    }
    case 2: case 3: case 6: case 7:
        return V(K_ALLOW_EMU, 1) ? 3 : 0;
    case 4: return g_sid_type[0] > 2 ? 3 : 0;                       // the other channel of a socket
    case 5: return g_sid_type[1] > 2 ? 3 : 0;
    }
    return 0;
}

static bool set_sid_address(int slot, bool single, uint8_t actual_type, uint8_t base)
{
    uint8_t other = 0x00;
    if (actual_type >= 3) {
        other = stereo_bits[g_stereo_bak];
    }
    if (single) {
        other = 0x3F;                                              // the only SID: every address bit
    }
    if (slot < 4) {
        if (base & other) {
            return false;
        }
        g_base[slot] = base;
        g_mask[slot] = (uint8_t)(0xFE & ~other);
        HW_WR_I(c64_sid_addr_base, slot, g_base[slot]);
        HW_WR_I(c64_sid_addr_mask, slot, g_mask[slot]);
        return true;
    }
    switch (slot) {                                                 // the odd ones: the existing mapping of the other channel
    case 4: return (uint8_t)(g_base[0] | other) == base;
    case 5: return (uint8_t)(g_base[1] | other) == base;
    case 6: return (uint8_t)(g_base[2] | other) == base;
    case 7: return (uint8_t)(g_base[3] | other) == base;
    }
    return false;
}

// access_socket_post: the registers go back to the mapping so far
static void restore_detection_addresses(void)
{
    HW_WR_I(c64_sid_addr_base, 0, g_base[0]);
    HW_WR_I(c64_sid_addr_base, 1, g_base[1]);
    HW_WR_I(c64_sid_addr_mask, 0, g_mask[0]);
    HW_WR_I(c64_sid_addr_mask, 1, g_mask[1]);
    HW_WR_I(c64_sid_addr_base, 2, g_base[2]);
    HW_WR_I(c64_sid_addr_base, 3, g_base[3]);
    HW_WR_I(c64_sid_addr_mask, 2, g_mask[2]);
    HW_WR_I(c64_sid_addr_mask, 3, g_mask[3]);
    HW_WR(c64_sid_sock_socket1_en, V(K_S1_EN) ? 1 : 0);
    HW_WR(c64_sid_sock_socket2_en, V(K_S2_EN) ? 1 : 0);
    HW_WR(c64_sid_sock_stereo_addrsel, g_stereo_bak);
}

// U64Config::SetSidType (the socket's chip is told its type): FW's device classes by the detected type
static void set_sid_type(int slot, uint8_t sid_type)
{
    if (slot >= 2) {
        return;
    }
    int chip = g_sid_type[slot];
    if (chip < 3 || chip == 7 || chip == 8) {
        return;                                                     // no device class tells a plain chip (FW: dev is null or has no effect)
    }
    c64_ram_memonly(false);                                         // the chips are on the bus
    detect_addresses();                                             // access_socket_pre
    int s = slot;                                                   // socket 1 at $D400, socket 2 at $D500
    uint8_t pre_mode = CART_RD(mode);
    CART_WR(mode, DRV_cart_regs_mode_ultimax);                      // force the I/O range on
    int t = sid_type - 1;                                           // received 1 = 6581, 2 = 8580 => 0 and 1
    switch (chip) {
    case 3:                                                         // FPGASID
        sr_wr(s, 25, 0x80);
        sr_wr(s, 26, 0x65);
        sr_wr(s, 31, t ? 0xFF : 0x40);                              // bit 7 and 5..0 set when 8580: ExtIn is DigiFix for an 8580 and disabled for a 6581
        sr_wr(s, 25, 0x00);
        sr_wr(s, 26, 0x00);
        break;
    case 4:                                                         // SwinSID Ultimate
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'E'); PEEK2();
        sr_wr(s, 31, t ? '8' : '6'); PEEK2();
        break;
    case 5: case 6:                                                 // ARMSID, ARM2SID: config mode, then the mode
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'I'); PEEK2();
        sr_wr(s, 31, 'D');
        wait_ms(10);
        sr_wr(s, 29, 'S'); PEEK2();
        sr_wr(s, 30, 'E'); PEEK2();
        sr_wr(s, 31, t ? '8' : '6');
        wait_ms(10);
        break;
    case 9:                                                         // PDsid
        sr_wr(s, 29, 'P'); PEEK2();
        sr_wr(s, 30, 'D'); PEEK2();
        sr_wr(s, 31, (sid_type == 1) ? 0x00 : 0x01); PEEK2();
        sr_wr(s, 29, 0); PEEK2();                                   // exit config mode
        break;
    case 10: case 11:                                               // SIDKick
        sr_wr(s, 0x1f, 0xFF);                                       // enter config mode
        sr_wr(s, 0x1e, 0x00);                                       // choose the profile to update
        sr_wr(s, 0x1d, t ? 0x0C : 0x08);                            // SID_TYPE_8580_BOOST / SID_TYPE_6581 of sid_device_sidkick.h
        sr_wr(s, 0x1f, 0xfd);                                       // leave config mode
        wait_ms(5);
        break;
    }
    CART_WR(mode, pre_mode);
    restore_detection_addresses();                                  // access_socket_post
}

static bool map_sid(int index, int total, uint16_t &mapped, uint8_t *on_slot, const uint8_t *base, const uint8_t *type, bool any)
{
    for (int i = 0; i < 8; i++) {
        if (mapped & (1 << i)) {
            continue;
        }
        uint8_t actual = get_sid_type(i);
        if ((actual & type[index]) || (any && actual)) {
            if (set_sid_address(i, total == 1, actual, base[index])) {
                mapped |= (uint16_t)(1 << i);
                on_slot[index] = (uint8_t)i;
                lib_logf("[u64cfg] SID %d (type %d) on logical SID %d (type %d), at $D%02x0", index, type[index] & 3, i, actual, base[index]);
                if (actual == 3) {
                    set_sid_type(i, type[index]);
                }
                return true;
            }
        }
    }
    return false;
}

static uint8_t combine_gain(uint8_t source_raw, uint8_t master_raw)
{
    if (!source_raw || !master_raw) {
        return 0;
    }
    uint16_t combined = (uint16_t)(((uint16_t)source_raw * master_raw + 0x40) >> 7);
    if (combined > 0xFF) {
        combined = 0xFF;
    }
    return combined ? (uint8_t)combined : 1;                        // both on: never rounded to 0 (mute)
}

static void set_mixer_auto_sid(const uint8_t *slots, int count)
{
    static const uint8_t channel_map[4] = { 4, 6, 0, 2 };
    uint8_t sel[4], vol[4];
    uint8_t master = volume_ctrl[V(K_MASTER, 24)];
    sel[0] = (uint8_t)V(K_VOL2, 24);
    sel[1] = (uint8_t)V(K_VOL3, 24);
    sel[2] = (uint8_t)V(K_VOL0, 24);
    sel[3] = (uint8_t)V(K_VOL1, 24);
    for (int i = 0; i < 4; i++) {
        vol[i] = combine_gain(volume_ctrl[sel[i]], master);
    }
    for (int i = 0; i < 8; i++) {
        HW_WR_I(audio_mixer_gain, i, 0);                            // first mute the SIDs
    }
    if (count > 4) {
        count = 4;
    }
    static const int panning[20] = { 0, 0, 0, 0, 5, 0, 0, 0, 3, 7, 0, 0, 5, 2, 8, 0, 3, 7, 1, 9 };
    for (int i = 0; i < count; i++) {
        uint8_t slot = slots[i];
        uint8_t ms = slot & 3;
        uint8_t v = vol[ms];
        if (!v && master && !sel[ms]) {
            v = 0x80;
        }
        int pan = panning[4 * count + i];
        HW_WR_I(audio_mixer_gain, 0 + channel_map[ms], mixer_side(v, 0x80, pan_ctrl[10 - pan]));
        HW_WR_I(audio_mixer_gain, 1 + channel_map[ms], mixer_side(v, 0x80, pan_ctrl[pan]));
    }
}

bool u64cfg_sid_autoconfig(CoreCtx *, int count, const uint8_t *base, const uint8_t *type)
{
    if (!V(K_PLAYER_AUTO, 1)) {
        lib_logf("[u64cfg] SID Player Autoconfig disabled, not configuring");
        return true;
    }
    sid_types_detect();
    uint16_t mapped = 0;
    uint8_t on_slot[8];
    if (count > 8) {
        count = 8;
    }
    memset(on_slot, 0, sizeof(on_slot));
    unmap_all_sids();
    bool failed = false;
    for (int i = 0; i < count; i++) {
        if (!map_sid(i, count, mapped, on_slot, base, type, false)) {
            failed = true;
        }
    }
    if (failed) {                                                   // again, from the last one down
        unmap_all_sids();
        mapped = 0;
        memset(on_slot, 0, sizeof(on_slot));
        failed = false;
        for (int i = count - 1; i >= 0; i--) {
            if (!map_sid(i, count, mapped, on_slot, base, type, false)) {
                failed = true;
            }
        }
    }
    if (failed) {                                                   // again, any chip that exists
        unmap_all_sids();
        mapped = 0;
        memset(on_slot, 0, sizeof(on_slot));
        failed = false;
        for (int i = 0; i < count; i++) {
            if (!map_sid(i, count, mapped, on_slot, base, type, true)) {
                failed = true;
            }
        }
    }
    if (failed) {
        lib_logf("[u64cfg] SID player: no valid mapping found");
        return false;
    }
    set_mixer_auto_sid(on_slot, count);
    return true;
}

// ---- the rest: user port, turbo, serial bus, LEDs -------------------------------------------------------------------------------------------
static const uint8_t speedregs_regvalues[] = { 0x00, 0x00, 0x01, 0x05 };
static const uint8_t c_iec_connectors[] = { 0x70, 0x60, 0x30, 0x50 };      // 4: ext, 5: ult, 6: cia

int fx_misc(CoreCtx *, const OpArgs &, OpOut &)
{
    uint8_t en = V(K_USERPORT, 1) ? 3 : 0;
    HW_WR(u64_userport_en, en);
    HW_WR(u64_pwm_duty, en ? 0xD8 : 0x00);
    // turbo (setCpuSpeed)
    uint8_t prefReg, turboRegs;
    int tc = V(K_TURBO);
    if (tc == 0) {
        prefReg = 0x80;
        turboRegs = 0;
    } else {
        prefReg = (uint8_t)(V(K_SPEED) | (V(K_BADLINE, 1) << 7));
        turboRegs = (uint8_t)(speedregs_regvalues[tc] + (V(K_SUPERCPU) ? 0x02 : 0x00));
    }
    HW_WR(c64_turbo_en_regs, turboRegs);
    HW_WR(c64_bus_speed_prefer, prefReg);
    HW_WR(c64_bus_speed_update, 1);
    // serial bus, burst mode, the parallel cable
    HW_WR(u64_connectors_internal, (uint8_t)(V(K_PARCABLE) | (V(K_BURST) << 1) | c_iec_connectors[V(K_IECMODE)]));
    // the case LEDs' sources: the low nibble LED 1 ("LED Select Top"), the high nibble LED 2 ("LED Select Bot"); a base without the member writes nothing
    const SvcBoard *b = g_api->board;
    if (SVC_HAS(SvcBoard, b, case_led_select)) {
        b->case_led_select((uint8_t)((V(K_LED1, 4) << 4) | V(K_LED0, 0)));
    }
    return OPE_OK;
}

// ---- power -----------------------------------------------------------------------------------------------------------------------------------
// The U64-II has no power register (U64_POWER_REG is the first U64's): power goes through the ESP32 control module (Gideon c64_subsys.cc MENU_C64_POWEROFF,
// wifi_machine_off() / wifi_machine_reboot(): rpc_calls.h CMD_MACHINE_OFF 0x0F, CMD_MACHINE_REBOOT 0x13). The module answers nothing, the machine is gone.
#define ESP_CMD_MACHINE_OFF    0x0F
#define ESP_CMD_MACHINE_REBOOT 0x13
// The mounted media written back first (the base's "media.flush_for_core", T154, T180 D4), then the command; a medium that cannot be written back keeps
// the machine on (1, the reason in msg). Else board.esp32_rpc's answer: the module answers nothing (timeout 0: 0 sent, -3 not sent).
static int power_cmd(uint8_t cmd, OpOut &o)
{
    if (op_call("media.flush_for_core", 0, 0, o.msg, o.msglen) != OPE_OK) {
        return 1;
    }
    const SvcBoard *b = g_api->board;
    return b ? b->esp32_rpc(cmd, 0, 0, 0, 0, 0) : -3;
}

int op_power_off(CoreCtx *, const OpArgs &, OpOut &o)
{
    int rc = power_cmd(ESP_CMD_MACHINE_OFF, o);
    if (rc > 0) {
        return OPE_FAILED;
    }
    if (rc < 0) {
        snprintf(o.msg, o.msglen, "The ESP32 control module is not there (no link)");
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Power off sent to the control module");
    return OPE_OK;
}
int op_power_cycle(CoreCtx *, const OpArgs &, OpOut &o)
{
    int rc = power_cmd(ESP_CMD_MACHINE_REBOOT, o);
    if (rc > 0) {
        return OPE_FAILED;
    }
    if (rc < 0) {
        snprintf(o.msg, o.msglen, "The ESP32 control module is not there (no link)");
        return OPE_FAILED;
    }
    snprintf(o.msg, o.msglen, "Power cycle sent to the control module");
    return OPE_OK;
}
