// PROVENANCE: TRX64-Ultimate software/ultimate-app/software/io/c64/trx_c64re.h, T104.2 UI; the branch for products without the ring is gone (U18).
// .c64re and .c64rering files from the TRX64 Ultimate frame ring
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §9, §10; the container and node
// shapes are TRX64's: crates/trx64-core/src/native_snapshot.rs,
// c64re_snapshot.rs, ring_dump.rs). Written with stored deflate blocks (valid
// gzip, no compressor on the RISC-V). Every file carries our own exact state in
// a "trx64Hw" node, which TRX64 ignores and trx_c64re_load() uses.
#ifndef TRX_C64RE_H
#define TRX_C64RE_H

#include <stdint.h>


// What the single-frame .c64re (format 2, as TRX64-Ultimate's trx_c64re.cc writes it) says of the machine beyond the chip block (the caller knows it, this
// file does not): which of the six System Modes runs (0 PAL, 1 NTSC, 2 PAL-60, 3 NTSC-50, 4 PAL-60/L, 5 NTSC-50/L), the cartridge as it stands now (its
// image is read through rom_read, `rom_bytes` long, never copied whole; src_name the file it came from; eeprom the 2 KB of a GMod2), the disk image on
// drive A and whether drive A is powered (its CPU and VIA state are read through the drive debug page when it is), the REU setting. The pointers stay the
// caller's and must live until the dump or load returns. Without a call the model is "c64-pal" and the file names no medium.
struct TrxC64reSrc {
    int machine;
    uint32_t rom_bytes;
    void (*rom_read)(void *ctx, uint32_t off, uint8_t *dst, uint32_t n);
    void *rom_ctx;
    const char *cart_name;
    const char *cart_src;
    int cart_type;
    uint16_t cart_require;      // the cartridge definition's require / prohibit flags (the header's machineState.cartridge carries them)
    uint16_t cart_prohibit;
    bool cart_writable;         // the C64 can change the cartridge's flash or EEPROM: a .c64rering entry carries the image (cartFlash)
    const uint8_t *eeprom;
    const char *disk_path;
    bool drive_power;
    int drive_unit;
    const char *drive_type;
    int reu;
    int reu_size;
    // for a load: put a piece of the cartridge image into DDR2; make the cartridge in the port the one the state names (the .crt at that path) WITHOUT a
    // reset, or an empty port for NULL (0: done); put the GMod2's 2 KB EEPROM back. All get rom_ctx.
    void (*rom_write)(void *ctx, uint32_t off, const uint8_t *src, uint32_t n);
    int (*cart_for_state)(void *ctx, const char *crt_path);
    void (*eeprom_put)(void *ctx, const uint8_t *data);
    // drive A's mount mode for a dump (0 read/write, 1 read only, 2 unlinked). For a load: the System Mode the file is of, put in for this run only (0: done), and
    // the image drive A had, mounted without a reset ("ro <path>" / "unlinked <path>" / "<path>", 0: done). Both get rom_ctx.
    int disk_mode;
    int (*machine_set)(void *ctx, int machine);
    int (*drive_mount)(void *ctx, const char *spec);
};
void trx_c64re_set_source(const TrxC64reSrc *src);
// The source set now, or NULL (the .trxring names its media from it).
const TrxC64reSrc *trx_c64re_source(void);

// Writes timeline entry idx as a .c64re of format 2 (U8): a binary section table, streamed, with the cartridge image as it stands at the freeze, once.
// NULL, or what went wrong - and then no file is left behind.
const char *trx_c64re_dump(const char *path, int idx, uint32_t *bytes);
// The same, calling progress(done, total, ctx) once before the first section (done 0) and after every piece written.
typedef void (*trx_progress_fn)(int done, int total, void *ctx);
const char *trx_c64re_dump_ex(const char *path, int idx, uint32_t *bytes, trx_progress_fn progress, void *ctx);

// The machine as it stands, with no frame ring: RAM, VIC registers, colour RAM and the media in the machine, format 1 (FW apps/c64re "Save .C64RE Dump").
const char *trx_c64re_dump_machine(const char *path, uint32_t *bytes);

// Writes the newest "seconds" of the ring, one entry in every "stride"
// frames (oldest first, the newest entry is the cursor), as a .c64rering.
const char *trx_c64rering_dump(const char *path, int seconds, int stride, uint32_t *bytes, int *entries);

// The same, calling progress(done, total, ctx) once the memories are read
// (done 0) and after every entry written.
const char *trx_c64rering_dump_ex(const char *path, int seconds, int stride, uint32_t *bytes, int *entries,
                                  trx_progress_fn progress, void *ctx);

// Where a state file belongs and what to name it after: the folder of the cartridge or disk the source names, else a timewarp/ folder on the first
// volume there is (U8 §5). False when there is nowhere to write at all.
bool trx_c64re_place(char *dir, int dirlen, char *base, int baselen);

// Loads a .c64re into the machine: format 2 exactly, or a format 1 file with our trx64Hw node exactly, or one of trx64's coarsely (T20 §9.4). A .trxring
// goes to trx_trxring_load(). The machine the file needs (System Mode, cartridge, drive A's image, settings) is built through the hooks of the source set
// when the file was staged; without them it is not built and what that leaves out is named by trx_c64re_missed().
const char *trx_c64re_load(const char *path);
// With the entry selector of trx_c64re_stage_entry().
const char *trx_c64re_load_entry(const char *path, const char *entry);

// The two halves of that load (T66): a menu that holds the machine while it is up reads and checks the file when it is chosen (a wrong file says so at
// once) and puts it in when it closes (trx_c64re_set_pending, trx_c64re_unfreeze). The source set at the stage goes with the staged state.
struct TrxC64reStaged;
// Reads, checks and keeps the state of the file. NULL and *out set, or what went wrong (nothing is kept then).
const char *trx_c64re_stage(const char *path, TrxC64reStaged **out);
// The same with progress(done, total, ctx) while the file is read (U9).
const char *trx_c64re_stage_ex(const char *path, TrxC64reStaged **out, trx_progress_fn progress, void *ctx);
// The same, naming which entry of a .c64rering to take (T66 part 3): NULL or "" is the cursor the file carries (current_id), else an entry's id
// ("cp_<frame>_<seq>") or label, a frame number, or "#N" for the Nth entry in the file (0 = oldest). A .c64re holds one state and ignores it.
const char *trx_c64re_stage_entry(const char *path, const char *entry, TrxC64reStaged **out);
// Puts a staged state into the machine and lets it run; frees it either way. NULL, or what the hardware said.
const char *trx_c64re_apply(TrxC64reStaged *st);
// U9 D3d: the medium a stage could not find. After a stage that failed, base[] is the name to look for and want[] the path the file asks for; base[] is
// empty when the stage failed for any other reason.
struct TrxC64reMissing {
    char want[144];
    char base[72];
};
// Stage, and say when it was the medium that was missing (U9 D3d). "miss" may be NULL. "medium" is tried before the path in the file and before the name
// beside it: a file to use, or a folder to look in (what the user answered the last refusal with). NULL for an ordinary stage.
const char *trx_c64re_stage_ask(const char *path, TrxC64reStaged **out, trx_progress_fn progress, void *ctx, TrxC64reMissing *miss,
                                const char *medium);
// U9 D6: what the last load could not put back, or NULL.
const char *trx_c64re_missed(void);
// Breaks a message into lines of at most cols characters, at a space, '/', '-' or '_' near the end where there is one.
void trx_c64re_wrap(char *dst, int cap, const char *text, int cols);
// Drops a staged state without loading it.
void trx_c64re_discard(TrxC64reStaged *st);

// The deferred load: the staged state is taken over here, and the menu's close asks for it. A second choice replaces the first.
void trx_c64re_set_pending(TrxC64reStaged *st);
bool trx_c64re_pending(void);
// From the menu's close when trx_c64re_pending(): loads and runs. What fails goes to the log - the menu is already gone by then.
void trx_c64re_unfreeze(void);


#endif
