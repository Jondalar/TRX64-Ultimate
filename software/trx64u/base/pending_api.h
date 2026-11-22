// The calls this library needs that base API 3.1 does not have (U18 §5; BOUNDARY.md). They are declared here and implemented nowhere: the link of the
// library fails on exactly these names, and that list is what MultiCore's core application, its shared libraries (T239.3 D12) or base 3.3 have to bring.
// Nothing here is meant as the final shape; each line says what the call does for this library and which item of BOUNDARY.md it stands for. A C++
// method of the core application is a plain function here, and no call takes the CoreCtx: the base knows its context.
//
// Not here because base 3.1 has them: setting reads (api->ops->state "set.<key>"), operations (api->ops->call), files (api->fs), the TRX block
// (api->trx), the C64's memory window and the port's GAME/EXROM (api->c64), time and delays in milliseconds (api->sys). Not here because they are the
// C64's own registers (c64u_regs.h). Not here by decision (MultiCore, 2026-11-22): the menu's look, name squeeze and the keyboard joystick (ui_set_look,
// ui_set_name_squeeze, keyb_joy_select, wasd_to_joy) are settings of the generic application; Play MOD and c64_cart_run_modplayer (T239.3 D5).
#ifndef TRX64U_PENDING_API_H
#define TRX64U_PENDING_API_H

#include <stdint.h>
#include "ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- settings (BOUNDARY "Settings": Settings::set, override_value, extra_get, extra_set). The reads go through api->ops->state("set.<key>").
bool settings_store(const char *key, int index);            // Settings::set: the value (index) stored without calling the setting's apply operation
void settings_override(const char *key, int index);         // Settings::override_value: a value for this run only, never stored (CTRL+P / CTRL+N at power-on)
bool settings_extra_get(const char *key, char *val, int n); // Settings::extra_get: a "Pad " key the settings file keeps beside the items (the detected SID types)
bool settings_extra_set(const char *key, const char *val);  // Settings::extra_set

// ---- the core application's state: CoreCtx members the moving code read (not in BOUNDARY.md: member reads leave no symbol)
bool machine_started(void);                                 // CoreCtx::started: the machine has been started once
bool machine_frozen(void);                                  // CoreCtx::frozen
bool machine_paused(void);                                  // CoreCtx::paused: C64_STOP stays asserted after the menu closes
void machine_set_paused(bool on);                           // CoreCtx::paused = on
bool menu_is_open(void);                                    // CoreCtx::menu_open
const char *menu_media_dir(void);                           // CoreCtx::media_dir: the folder the menu was on ("" none)
bool ui_notice_pending(void);                               // CoreCtx::notice_pending: a notice waits to be shown
void ui_notice(const char *text);                           // CoreCtx::notice + notice_pending: a line the menu shows when it next opens
bool board_has_uci(void);                                   // CoreCtx::board->probes & BC_UCI (true without a board record): the command interface is in the bitstream
bool corefolder_dir(char *out, int outlen);                 // BOUNDARY "Files" corefolder_dir (of CoreCtx::cor_path): the folder the core's .cor installed its data into; false: no .cor

// ---- the core's I/O (CoreIo of io.h; not in BOUNDARY.md: virtual calls leave no symbol)
void io_ddr_write(uint32_t off, const void *p, uint32_t n); // CoreIo::ddr_write: into the DDR2 window (the REU's memory from offset 0), posted writes completed
void io_ddr_read(uint32_t off, void *p, uint32_t n);        // CoreIo::ddr_read
bool io_keys_read(uint8_t m[8], bool *restore);             // CoreIo::keys_read: the physical key matrix now (m[col] bit row) and RESTORE; false: cannot say
void sys_delay_us(uint32_t us);                             // CoreIo::delay_us: a busy wait of microseconds (the SID chips' register handshakes)

// ---- the start and the reset (BOUNDARY "Board and app core": boot_cold_reset, boot_put_builtin_rom)
int boot_cold_reset(char *msg, int msglen);                 // hold, the reset-time settings, release: the machine starts again (0, or the reason)
int boot_put_builtin_rom(const char *put_op, char *msg, int msglen);   // the built-in ROM of a role (c64.put_kernal, ..., drive.put_rom) back in (0, or the reason)

// ---- the board (BOUNDARY "Board and app core")
int hw_esp32_rpc(uint8_t cmd, const uint8_t *args, int nargs, uint8_t *reply, int maxreply, int timeout_ms);   // a command to the ESP32 control module (power)
// The SID sockets' supply, 1 kOhm shunt and filter capacitors (BOUNDARY i2c / I2C_Driver::i2c_write_byte: write_sid_sockets' expander 0x40). The base keeps
// the control byte, packs this socket's nibble and writes it under the "SID Configuration" lock. 0 done, < 0 a bad argument.
enum { SID_SOCKET_OFF = 0, SID_SOCKET_ON = 1, SID_SOCKET_ON_TYPE1 = 2 };   // regulator: off, on, on for a 6581 (the type 1 the detection found)
int board_sid_socket(int socket, int regulator, bool shunt, bool filter_caps);   // socket 0, 1; filter_caps: the setting's value (the base inverts it)
void SetVideoPll(int mode, int ppm);                        // BOUNDARY SetVideoPll: the video PLL for a system mode (t_video_mode) with the colour clock's offset in ppm
void ResetHdmiPll(void);                                    // BOUNDARY ResetHdmiPll
bool video_color_timing(int mode, uint8_t *mode_bits, uint8_t *phase_inc, uint8_t *audio_div);   // BOUNDARY color_timings[mode]: the fields fx_video uses
void picture_set_system(int sys);                           // BOUNDARY picture_set_system: the picture layer takes a new system mode (its HDMI mode, the Picture row)
const char *video_sys_name(int sys);                        // BOUNDARY video_sys_name: "PAL", "NTSC", ... for the log
bool mouse_sink_scaling(int cfg_key, int *value);           // BOUNDARY mouse_sink_scaling: a core with a mouse sink answers the USB HID driver's mouse items itself
enum { WB_WHY_RESET = 0, WB_WHY_EJECT, WB_WHY_CORE, WB_WHY_POWER, WB_WHY_MENU, WB_WHY_TIMER };   // writeback.h WbWhy
int media_writeback(int why, char *msg, int msglen);        // BOUNDARY media_writeback: the mounted media written back first (0), or the reason the machine stays on
void tempdisk_set(bool auto_cleanup, bool subfolders);      // BOUNDARY tempdisk_set: "Temp Auto Cleanup", "Temp Subfolders"

// ---- the C64's cartridge port (BOUNDARY D12 c64_cart_*)
bool c64_cart_active(void);                                 // a cartridge is in the port (started from the Library, it stays until removed)
const char *c64_cart_name(void);                            // its file name
const char *c64_cart_path(void);                            // the full path of its file
int c64_cart_save(const char *path, char *msg, int msglen); // the cartridge in the port written out as a .crt (0, or the reason)
bool c64_cart_ef_clear(uint8_t bank, bool high);            // the UCI's EasyFlash erase: 8 banks from `bank`, ROMH when high; false without a cartridge
int c64_cart_run_image(const char *name, const uint8_t *img, char *msg, int msglen);   // the SID player's 16 KB cartridge from RAM, the machine held in reset

// ---- the SID file (BOUNDARY D12 sidfile: sid_parse, sid_info_text, sid_plan, sid_requests, sid_mus_stereo, sid_mus_header); the types are sidfile.h's
#define SID_HDR_BYTES 0x80
#define SID_HDR_READ  0x7E                                  // the bytes FW reads from a .sid
#define SID_SSL_BYTES 512                                   // the song-lengths table the player takes (cartridge image +0x3000)
struct SidTune {
    uint8_t hdr[SID_HDR_BYTES];
    int version;
    uint32_t data_offset;
    int songs;
    bool mus;
    bool sid;
    bool valid;
    uint16_t flags;
    uint16_t song, default_song;
    uint16_t start, end, header_location;
    int skip;
    uint8_t extra_lo;
};
struct SidReq {
    uint8_t base;                                           // 0x40 = $D400
    uint8_t type;                                           // 1 6581, 2 8580, 3 either
};
bool sid_parse(const uint8_t *head, uint32_t n, const char *name, bool ext_mus, struct SidTune *t);   // the header (false: no tune)
int sid_info_text(const struct SidTune *t, char *out, int outlen);   // the text of "Show info"; the bytes written
int sid_plan(struct SidTune *t, uint32_t fsize, uint16_t load_le, int song, const char **why);   // start, end, header place, song (0, or the reason)
int sid_requests(const struct SidTune *t, struct SidReq out[3]);    // the SIDs the tune asks for (1 to 3)
bool sid_mus_stereo(const struct SidTune *t, uint32_t load_end, uint8_t (*rd)(void *, uint32_t), void *user, uint32_t *first_end);   // a second song behind the first
void sid_mus_header(struct SidTune *t, bool stereo);        // a MUS tune's header: the (stereo) player's addresses

// ---- the command interface (BOUNDARY D12 uci_start); the machine side is this library's (UciHost, the layout of uci.h)
struct UciHost {
    int (*reu)(CoreCtx *c, uint32_t *size, uint32_t *offset, char *img, int imgn, char *busy, int busyn);
    bool (*drive_info)(CoreCtx *c, int idx, uint8_t *type, uint8_t *addr, bool *power);
    int (*drive_do)(CoreCtx *c, int idx, int what, const char *path, char *msg, int msglen);
    int (*sid_map)(CoreCtx *c, uint8_t *data);
    const char *memory_file;
    bool (*ef_clear)(CoreCtx *c, uint8_t bank, bool high);
    void (*ramdrive)(CoreCtx *c, int n, int *type, uint32_t *size, uint32_t *base);
    void (*palette)(CoreCtx *c, int what, uint8_t idx, uint8_t (*rgb)[3]);
};
void uci_start(const struct UciHost *host);                 // starts the UCI server and its targets once, with the machine's side

// ---- drive A (BOUNDARY D12 cbm1541_drive and CbmDrive's methods; the inline ones as well: set_twist, image_name, ...). The enums are d1541_drive.h's.
struct CbmDrive;
enum { DT_1541 = 0, DT_1571 = 1, DT_1581 = 2, DT_UNSET = 3 };
enum { MM_RW = 0, MM_RO = 1, MM_UNLINKED = 2 };
enum { DF_D64 = 0, DF_G64 = 1, DF_D81 = 2 };
struct CbmDrive *cbm1541_drive(char *msg, int msglen);      // the core's drive A, null when there is none (the reason in msg; msg may be null)
void cbm_drive_set_twist(struct CbmDrive *d, int hundreds); // "Track Twist" for the next D64 mount
void cbm_drive_set_align(struct CbmDrive *d, bool on);      // "GCR Save Align Tracks"
void cbm_drive_set_reset_with_c64(struct CbmDrive *d, bool on);   // "Resets when C64 resets"
void cbm_drive_set_freeze(struct CbmDrive *d, bool on);     // "Freezes in menu", written with the next drive reset
void cbm_drive_set_geos(struct CbmDrive *d, int mode);      // "D64 Geos Copy Protection" for the next D64 mount
const char *cbm_drive_image_name(struct CbmDrive *d);       // the disk's file name, null: no disk
const char *cbm_drive_last_dir(struct CbmDrive *d);         // the folder of the disk
const char *cbm_drive_mode_name(struct CbmDrive *d);        // "rw", "ro", "unlinked", "empty"
int cbm_drive_swap_delay_ms(struct CbmDrive *d);            // the time the drive is empty during a swap
int cbm_drive_type(struct CbmDrive *d);                     // DT_*
int cbm_drive_bus_id(struct CbmDrive *d);                   // the IEC address
bool cbm_drive_powered(struct CbmDrive *d);
int cbm_drive_create(struct CbmDrive *d, const char *path, int format, const char *name, char *msg, int msglen);   // a blank image file (0, or the reason)
int cbm_drive_mount(struct CbmDrive *d, const char *path, int mode, char *msg, int msglen);   // MM_* (0, or the reason)
int cbm_drive_eject(struct CbmDrive *d, char *msg, int msglen);     // what the head wrote goes back to the file first (0, or the reason)

#ifdef __cplusplus
}
#endif

#endif
