// The calls the ring library makes that base API 3.1 does not have (U18). Each one is declared here, called where the code needs it and left
// unimplemented: the link of ring.u2a fails on exactly these names. That list is what the base (or another library's SvcLib export) still has to give;
// BOUNDARY.md names the item each one stands for. The sync_* and sys_* names are MultiCore's for base 3.3; the others name what the code needs.
// Declared extern "C" so the link error names them plainly.
#ifndef RING_PENDING_API_H
#define RING_PENDING_API_H

#include <stdint.h>
#include <stdbool.h>

struct CoreCtx;
struct SyncMutex;

extern "C" {

// ---- base 3.3 (MultiCore) --------------------------------------------------------------------------------------------------------------------------------
// A short critical section, nothing blocks inside: the four byte accesses of one 32-bit TRX register (FreeRTOS: vTaskEnterCritical / vTaskExitCritical).
void sync_enter(void);
void sync_leave(void);
// The engine's lock, taken by every ring call (FreeRTOS: xQueueCreateMutex, xQueueSemaphoreTake, xQueueGenericSend). timeout_ms 0 tries once,
// SYNC_FOREVER waits; true: taken.
#define SYNC_FOREVER 0xFFFFFFFFu
SyncMutex *sync_mutex_create(void);
bool sync_mutex_lock(SyncMutex *m, uint32_t timeout_ms);
void sync_mutex_unlock(SyncMutex *m);
// The 1 ms hardware counter the engine's busy waits time themselves by (itu: getMsTimer).
uint32_t sys_ms_timer(void);

// ---- the C64 machine's registers (c64.h, u64.h, iomap.h: C64_CARTREGS_BASE, C64_IO_BASE, CMD_IF_BASE) -----------------------------------------------
// C64_STOP: 1 the app's stop holds the machine, 0 lets it go (writes of C64_MODE go through api->c64->cart_mode).
void c64_stop(uint8_t on);
// C64_MODE as it stands (the scrub and the land put the menu's mode back).
uint8_t c64_mode(void);
// C64_DMA_MEMONLY: 1 the memory window sees RAM only, 0 the C64's memory map.
uint8_t c64_dma_memonly(void);
void c64_set_dma_memonly(uint8_t v);
// C64_CARTRIDGE_TYPE: the type of the cartridge in the port (the .c64re's cartridge gate and the banking replay).
uint8_t c64_cartridge_type(void);
// C64_REU_ENABLE and C64_REU_SIZE: A08 (no ring while the REU is on), the settings a .c64re carries and a load puts back.
uint8_t c64_reu_enable(void);
void c64_set_reu_enable(uint8_t v);
uint8_t c64_reu_size(void);
void c64_set_reu_size(uint8_t v);
// C64_SAMPLER_ENABLE: a .c64re setting.
uint8_t c64_sampler_enable(void);
void c64_set_sampler_enable(uint8_t v);
// C64_VIDEOFORMAT (b1 60 Hz): the frame rate of the System Mode that runs (SvcTrx::video calls the ring for it, so it cannot be used here).
uint8_t c64_videoformat(void);
// The UCI's slot (CMD_IF_BASE + 1 enable, + 0 base): a .c64re setting.
uint8_t cmd_if_slot_enable(void);
void cmd_if_set_slot_enable(uint8_t v);
void cmd_if_set_slot_base(uint8_t v);
// The cartridge's RAM window in DDR2, 64 KB (cart_slot.v: 0xEF0000): a rewind and a load put its bytes back, a dump reads it.
volatile uint8_t *c64_cart_ram(void);
// Drive A's memory image in DDR2 (drive_unit.vhd g_ram_base: 0xEE0000): the .c64re's drive section.
volatile uint8_t *drive_a_image(void);
// getFpgaCapabilities (itu): CAPAB_COMMAND_INTF, CAPAB_SAMPLER, CAPAB_EEPROM.
#define CAPAB_COMMAND_INTF 0x00040000u
#define CAPAB_SAMPLER      0x00200000u
#define CAPAB_EEPROM       0x00400000u
uint32_t fpga_capabilities(void);

// ---- the board -------------------------------------------------------------------------------------------------------------------------------------------
// The FAT time of now and whether the clock was ever set (filesystem_fat, rtc_dummy: get_fattime, rtc_time_valid): a .c64re's createdAt.
uint32_t get_fattime(void);
bool rtc_time_valid(void);
// The application's version text (gitinfo: APP_VERSION_STRING): a .c64re's appVersion.
const char *app_version_string(void);

// ---- the core application and its settings ---------------------------------------------------------------------------------------------------------------
// A setting overridden for this run only and applied (Settings::find / override_value, regset_apply_setting): a .c64re's System Mode. 0: the
// setting has that value now.
int settings_override(CoreCtx *c, const char *key, int value);
// The progress of an operation that answers `more` (CoreCtx::job_step, job_pct, job_path, job_cancel): whether the caller calls again and shows a
// bar; the share done and the file it writes; a cancel the person asked for (read and cleared).
bool ops_job_step(CoreCtx *c);
void ops_job_progress(CoreCtx *c, int pct, const char *path);
bool ops_job_cancelled(CoreCtx *c);
// The EEPROM of the cartridge port (CoreIo::eeprom_read / eeprom_write): a GMod2's 93C86, its array at +2048, its valid flag at 0.
void core_eeprom_read(CoreCtx *c, uint32_t off, void *p, uint32_t n);
void core_eeprom_write(CoreCtx *c, uint32_t off, const void *p, uint32_t n);

// ---- the trx64u base library (cross ring -> trx64u) ------------------------------------------------------------------------------------------------------
// The full path of a new file `name` + ext on the medium the menu saves to; false: no medium (trx64u_ops.cc trx_target_path).
bool trx_target_path(CoreCtx *c, const char *name, const char *ext, char *out, int n);

// ---- MultiCore's shared cartridge library (D12, c64_cart.h) ----------------------------------------------------------------------------------------------
bool c64_cart_active(void);
const char *c64_cart_name(void);
const char *c64_cart_path(void);
int c64_cart_crt_type(void);
uint32_t c64_cart_image_bytes(void);
void c64_cart_image_read(CoreCtx *c, uint32_t off, uint8_t *dst, uint32_t n);
void c64_cart_image_write(CoreCtx *c, uint32_t off, const uint8_t *src, uint32_t n);
int c64_cart_for_state(CoreCtx *c, const char *path, char *msg, int msglen);

// ---- MultiCore's shared 1541 drive library (D12, cbm1541.h, d1541_drive.h) -------------------------------------------------------------------------------
// Mounts drive A's image without a reset: "ro <path>", "unlinked <path>" or "<path>". 0: done.
int cbm1541_slot_mount(CoreCtx *c, const char *spec, char *msg, int msglen);
// Drive A as it stands (CoreCtx::drive, CbmDrive type / powered / bus_id / image_path / mount_mode): false when there is none. type "1541" or "1581"
// (no 1571: the drive library refuses it, T103 Q3), mode 0 read/write, 1 read only, 2 unlinked.
bool cbm1541_drive_a(CoreCtx *c, const char **type, bool *powered, int *unit, const char **image, int *mode);

}

#endif
