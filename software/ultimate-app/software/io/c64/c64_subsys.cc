/*
 * c64_subsys.cc
 *
 *  Created on: Jul 16, 2015
 *      Author: Gideon
 */

#include "c64.h"
#include "c64_crt.h"
#include "trx_cart.h"
#include "trx_lens_fw.h"
#include "c64_subsys.h"
#include <ctype.h>
#include <strings.h>
#include "init_function.h"
#include "userinterface.h"
#include "u64.h"
#include "c1541.h"
#include "endianness.h"
#include "monitor_init.h"
#if U64 == 2
#include "wifi.h"
#endif

#define C64_BOOTCRT_DOSYNC    0x014F
#define C64_BOOTCRT_RUNCODE   0x0172
#define C64_BOOTCRT_DRIVENUM  0x0173
#define C64_BOOTCRT_NAMELEN   0x0174
#define C64_BOOTCRT_NAME      0x0175
#define C64_BOOTCRT_HANDSHAKE 0x02
#define C64_BOOTCRT_JUMPADDR  0x00AA

/* other external references */
extern uint8_t _bootcrt_65_start;
extern uint8_t _bootcrt_65_end;

static bool contains_path_separator(const char *name)
{
    while (*name) {
        if ((*name == '/') || (*name == '\\')) {
            return true;
        }
        name++;
    }
    return false;
}

// Returns either the name itself, or a trimmed copy in the caller's buffer.
// Plain names are passed on untouched: CbmFileName cuts them to 16 PETSCII
// characters itself, and needs the extension that trimming here would remove.
static const char *format_bootcrt_display_name(const char *name, char *trimmed)
{
    const int kDisplayChars = 16;
    const int kTrimmedTailChars = 13;
    int length = strlen(name);

    if ((length <= kDisplayChars) || !contains_path_separator(name)) {
        return name;
    }

    memcpy(trimmed, "...", 3);
    memcpy(trimmed + 3, name + length - kTrimmedTailChars, kTrimmedTailChars);
    trimmed[kDisplayChars] = 0;
    return trimmed;
}

cart_def boot_cart; // static => initialized with all zeros.

static void initBootCart(void *object, void *param)
{
    int size = (int)&_bootcrt_65_end - (int)&_bootcrt_65_start;
    boot_cart.custom_addr = new uint8_t[8192];
    boot_cart.length = size;
    boot_cart.type = CART_TYPE_8K;

    memcpy(boot_cart.custom_addr, &_bootcrt_65_start, size);
    printf("%d bytes copied into boot_cart.\n", size);
}
InitFunction bootCart_initializer("Boot Cart", initBootCart, NULL, NULL);

// --- Cartridge auto-save ---------------------------------------------------------------------
// C64_CRT knows what the image holds and whether it changed. The decision, the dialog and the
// file juggling live here, where the user interface and the machine are in scope, so that the
// cartridge file code stays free of them and remains testable on the host.

static bool crt_source_is_writable(void)
{
    const char *src = C64_CRT :: get_source();
    const char *slash = strrchr(src, '/');
    if (!slash || (slash == src)) {
        return false; // no file, or directly in the root, which is not a drive
    }
    // Uploads land in /Temp. The internal flash is small and should not take a write per save.
    if ((strncasecmp(src, "/Temp/", 6) == 0) || (strncasecmp(src, "/Flash/", 7) == 0)) {
        return false;
    }
    char dir[256];
    int len = slash - src;
    if (len >= (int)sizeof(dir)) {
        return false;
    }
    memcpy(dir, src, len);
    dir[len] = 0;
    Path path(dir);
    return FileManager :: getFileManager()->is_path_writable(&path);
}

// The popup for a change that crt_source_is_writable() refuses, worded by the reason.
static const char *crt_not_writable_message(void)
{
    const char *src = C64_CRT :: get_source();
    if (strncasecmp(src, "/Flash/", 7) == 0) {
        return "Cartridge changed. Write-back to\nthe internal flash is disabled.\nSave Cartridge to SD or USB\nto keep it.";
    }
    if (strncasecmp(src, "/Temp/", 6) == 0) {
        return "Cartridge changed. Uploaded\ncartridges are not written back.\nSave Cartridge to SD or USB\nto keep it.";
    }
    return "Cartridge changed, but its file\ncannot be written back. Use\nSave Cartridge to keep it.";
}

// One save at a time: the menu hook, the auto-save task and Save Cartridge all write files.
static SemaphoreHandle_t crt_save_mutex;

// NAME.tmp first. The file as first loaded is kept once, as NAME.bak (not .crt, so it does not
// look like a cartridge to load). Then NAME.tmp becomes NAME. Returns NULL or an error message.
// With a user interface the C64 is stopped while the file is written; without one, from the
// auto-save task, the file comes from the snapshot the caller took and the C64 keeps running.
static const char *crt_save_in_place(UserInterface *ui)
{
    FileManager *fm = FileManager :: getFileManager();
    C64 *c64 = C64 :: getMachine();
    mstring target(C64_CRT :: get_source());
    mstring temp(target);
    mstring backup(target);
    temp += ".tmp";
    backup += ".bak";

    File *f = NULL;
    FRESULT res = fm->fopen(temp.c_str(), FA_WRITE | FA_CREATE_ALWAYS, &f);
    if (res != FR_OK) {
        return FileSystem :: get_error_string(res);
    }
    uint32_t previous = C64_CRT :: get_baseline(); // saving moves it; put back if NAME survives
    SubsysResultCode_e ret;
    if (ui) {
        ui->show_progress("Saving cartridge..", 1);
        bool stopped = c64->begin_stopped_session();
        ret = C64_CRT :: save_crt(f);
        c64->end_stopped_session(stopped);
        ui->hide_progress();
    } else {
        ret = C64_CRT :: save_snapshot(f);
    }
    fm->fclose(f);

    if (ret != SSRET_OK) {
        fm->delete_file(temp.c_str());
        C64_CRT :: set_baseline(previous);
        printf("[CRT] save failed: %s\n", target.c_str());
        return "Writing the cartridge failed.";
    }

    File *existing = NULL;
    if (fm->fopen(backup.c_str(), FA_READ, &existing) == FR_OK) {
        fm->fclose(existing);
        res = fm->delete_file(target.c_str());
        if (res == FR_NO_FILE) {
            // An earlier attempt got this far and then failed to rename, leaving the saved
            // state in NAME.tmp and no NAME at all. Taking that as a failure here would
            // delete NAME.tmp below, so the only copy left would be NAME.bak, and every
            // later attempt would end the same way. The rename that follows puts NAME back.
            res = FR_OK;
        }
    } else {
        res = fm->rename(target.c_str(), backup.c_str());
    }
    if (res != FR_OK) {
        fm->delete_file(temp.c_str());
        C64_CRT :: set_baseline(previous);
        printf("[CRT] save failed: cannot replace %s\n", target.c_str());
        return FileSystem :: get_error_string(res);
    }
    res = fm->rename(temp.c_str(), target.c_str());
    if (res != FR_OK) {
        C64_CRT :: set_baseline(previous);
        printf("[CRT] save failed: new state left in %s\n", temp.c_str());
        return "Saved, but renaming the .tmp\nfile failed. Check the folder.";
    }
    printf("[CRT] saved %s\n", target.c_str());
    return NULL;
}

// TRX64 bitstream (trx_cart.cc): the firmware applies every flash change itself, so it knows what
// changed without a hash. The copy is taken with flash operations held off (the C64 sees the chip
// busy), which makes it one state of the image; the file is then written from the copy while the
// C64 runs on. NULL on success, or an error message. Caller holds crt_save_mutex.
static const char *crt_trx_writeback(void)
{
    uint32_t hash;
    trx_cart_hold(true);
    bool copied = C64_CRT :: take_snapshot(&hash); // fails only on an EEPROM write during the copy, or memory
    if (copied) {
        trx_cart_begin_writeback();
    }
    trx_cart_hold(false);
    if (!copied) {
        return "Copying the cartridge failed.";
    }
    const char *error = crt_save_in_place(NULL);
    trx_cart_end_writeback(error == NULL);
    C64_CRT :: discard_snapshot(); // one that was not written
    return error;
}

// Before a reset, reboot, Clear Mem or power off, and before another cartridge is loaded: with
// Auto, write the pending changes back now. A reboot reloads the cartridge from its file and a
// new cartridge overwrites the image, so what is not written here is lost.
void crt_autosave_before_reset(void)
{
    if (!crt_save_mutex || !C64_CRT :: is_valid() ||
        (C64 :: getMachine()->get_cfg_value(CFG_C64_CRT_AUTOSAVE) != CRT_AUTOSAVE_AUTO) ||
        !crt_source_is_writable()) {
        return;
    }
    xSemaphoreTake(crt_save_mutex, portMAX_DELAY);
    if (trx_cart_present()) {
        if (trx_cart_pending()) {
            const char *error = crt_trx_writeback();
            printf("[CRT] write-back before reset: %s\n", error ? error : "done");
        }
        xSemaphoreGive(crt_save_mutex);
        return;
    }
    if (C64_CRT :: current_hash() == C64_CRT :: get_baseline()) {
        xSemaphoreGive(crt_save_mutex); // nothing changed, or no part the C64 can change
        return;
    }
    uint32_t hash;
    bool taken = false;
    for (int tries = 0; (tries < 4) && !taken; tries++) {
        taken = C64_CRT :: take_snapshot(&hash); // fails when the C64 writes during the copy
    }
    if (!taken) {
        printf("[CRT] write-back before reset: no stable copy\n");
    } else if (hash != C64_CRT :: get_baseline()) {
        const char *error = crt_save_in_place(NULL);
        printf("[CRT] write-back before reset: %s\n", error ? error : "done");
    }
    C64_CRT :: discard_snapshot(); // one that was not written
    xSemaphoreGive(crt_save_mutex);
}

// Hooked into UserInterface::run_once(). From the overlay the C64 is still running: the hash is
// taken without stopping it, the save runs stopped. A torn read only means an extra save now or a
// change found at the next menu open.
static void crt_autosave_menu_check(UserInterface *ui);

static void crt_autosave_menu_hook(UserInterface *ui)
{
    // Busy means the auto-save task is writing this very change.
    if (xSemaphoreTake(crt_save_mutex, 0) != pdTRUE) {
        return;
    }
    crt_autosave_menu_check(ui);
    xSemaphoreGive(crt_save_mutex);
}

// The menu check on the TRX64 bitstream: "changed" is the firmware's own map, no hash.
static void crt_trx_menu_check(UserInterface *ui, int mode)
{
    if (!trx_cart_pending()) {
        return;
    }
    if (!crt_source_is_writable()) {
        ui->popup(crt_not_writable_message(), BUTTON_OK);
        trx_cart_forget(); // told once per change
        return;
    }
    if (mode == CRT_AUTOSAVE_ASK) {
        const char *name = strrchr(C64_CRT :: get_source(), '/') + 1; // writable implies a slash
        char msg[80];
        strcpy(msg, "Cartridge changed. Save it to\n");
        int len = strlen(msg);
        strncpy(msg + len, name, 28);
        msg[len + 28] = 0;
        strcat(msg, "?");
        if (ui->popup(msg, BUTTON_YES | BUTTON_NO) != BUTTON_YES) {
            trx_cart_forget(); // declined: ask again only after the next change
            return;
        }
    }
    // The whole save runs with the C64 stopped and flash operations held off: one state.
    trx_cart_hold(true);
    trx_cart_begin_writeback();
    const char *error = crt_save_in_place(ui);
    trx_cart_end_writeback(error == NULL);
    trx_cart_hold(false);
    if (error) {
        ui->popup(error, BUTTON_OK); // kept pending, so this comes back
    }
}

static void crt_autosave_menu_check(UserInterface *ui)
{
    int mode = C64 :: getMachine()->get_cfg_value(CFG_C64_CRT_AUTOSAVE);
    if ((mode == CRT_AUTOSAVE_OFF) || !C64_CRT :: is_valid()) {
        return;
    }
    if (trx_cart_present()) {
        crt_trx_menu_check(ui, mode);
        return;
    }

    TickType_t start = xTaskGetTickCount();
    uint32_t hash = C64_CRT :: current_hash();
    int ms = (int)((xTaskGetTickCount() - start) * 1000 / configTICK_RATE_HZ);
    printf("[CRT] hash %8x, baseline %8x, %d ms\n", hash, C64_CRT :: get_baseline(), ms);
    if (hash == C64_CRT :: get_baseline()) {
        return;
    }

    if (!crt_source_is_writable()) {
        ui->popup(crt_not_writable_message(), BUTTON_OK);
        C64_CRT :: set_baseline(hash); // told once per change
        return;
    }

    if (mode == CRT_AUTOSAVE_ASK) {
        const char *name = strrchr(C64_CRT :: get_source(), '/') + 1; // writable implies a slash
        char msg[80];
        strcpy(msg, "Cartridge changed. Save it to\n");
        int len = strlen(msg);
        strncpy(msg + len, name, 28);
        msg[len + 28] = 0;
        strcat(msg, "?");
        if (ui->popup(msg, BUTTON_YES | BUTTON_NO) != BUTTON_YES) {
            C64_CRT :: set_baseline(hash); // declined: ask again only after the next change
            return;
        }
    }

    const char *error = crt_save_in_place(ui);
    if (error) {
        ui->popup(error, BUTTON_OK); // baseline kept, so this comes back at the next menu open
    }
}

// With Auto the file follows the cartridge without the menu, the way a disk follows its drive.
// A write the firmware sees (C64_CRT::take_change_notice) starts a check; it runs once the C64
// has been quiet for CRT_QUIET_MS, because a game saves in more than one step. The hash has the
// last word: it has to differ from the file and stay the same over another quiet period, which
// also covers EasyFlash bytes programmed without a notice after the erase that gave one. A
// change nothing announces is still found by the hash at the next menu open.
#define CRT_POLL_MS  250
#define CRT_QUIET_MS 2000
#define CRT_RETRY_MS 10000 // TRX64 bitstream: after a failed write-back

static void crt_autosave_task(void *param)
{
    bool pending = false;       // a write was seen and is not in the file yet
    bool hashed = false;        // last_hash holds the image as of the last quiet check
    uint32_t last_hash = 0;
    TickType_t quiet_since = 0;
    TickType_t trx_retry_at = xTaskGetTickCount();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CRT_POLL_MS));
        C64 *c64 = C64 :: getMachine();
        if (!C64_CRT :: is_valid() || (c64->get_cfg_value(CFG_C64_CRT_AUTOSAVE) != CRT_AUTOSAVE_AUTO)) {
            pending = false;
            continue;
        }
        TickType_t now = xTaskGetTickCount();
        if (trx_cart_present()) {
            // Quiet for 2 s, or changes pending for 30 s, or a flush asked for (trx_cart_due).
            if (((int32_t)(now - trx_retry_at) < 0) || !trx_cart_due() || !crt_source_is_writable()) {
                continue; // not writable: the menu says so at its next open
            }
            xSemaphoreTake(crt_save_mutex, portMAX_DELAY);
            const char *error = crt_trx_writeback();
            xSemaphoreGive(crt_save_mutex);
            printf("[CRT] write-back: %s\n", error ? error : "done");
            trx_retry_at = error ? now + pdMS_TO_TICKS(CRT_RETRY_MS) : now; // kept pending on failure
            continue;
        }
        if (C64_CRT :: take_change_notice()) {
            pending = true;
            hashed = false;
            quiet_since = now;
            continue;
        }
        if (!pending || ((now - quiet_since) < pdMS_TO_TICKS(CRT_QUIET_MS))) {
            continue;
        }

        xSemaphoreTake(crt_save_mutex, portMAX_DELAY);
        uint32_t hash;
        if (!C64_CRT :: take_snapshot(&hash)) {
            quiet_since = now; // written to while copied, or no memory: look again later
        } else if (hash == C64_CRT :: get_baseline()) {
            pending = false; // already in the file: saved from the menu, or written back unchanged
        } else if (!hashed || (hash != last_hash)) {
            hashed = true; // still moving, or first look: one more quiet period
            last_hash = hash;
            quiet_since = now;
        } else if (!crt_source_is_writable()) {
            pending = false; // the menu says so at its next open
        } else {
            const char *error = crt_save_in_place(NULL);
            printf("[CRT] auto-save: %s\n", error ? error : "done");
            pending = false; // after an error the baseline is kept, so the menu offers it again
        }
        C64_CRT :: discard_snapshot(); // one that was not written
        xSemaphoreGive(crt_save_mutex);
    }
}

C64_Subsys::C64_Subsys(C64 *machine)  : SubSystem(SUBSYSID_C64)
{
	taskHandle = 0;
	c64 = machine;
	fm = FileManager :: getFileManager();
	taskHandle = 0;

	taskCategory = TasksCollection :: getCategory("C64 Machine", SORT_ORDER_C64);
	crt_save_mutex = xSemaphoreCreateMutex();
	UserInterface :: set_menu_enter_hook(crt_autosave_menu_hook);
#ifndef RECOVERYAPP
	xTaskCreate(crt_autosave_task, "CRT Auto-Save", configMINIMAL_STACK_SIZE, NULL, PRIO_BACKGROUND, NULL);
#endif
}

C64_Subsys::~C64_Subsys() {
    if (taskHandle) {
    	vTaskDelete(taskHandle);
    }

}

void C64_Subsys :: create_task_items(void)
{
    myActions.reset    = new Action("Reset C64", SUBSYSID_C64, MENU_C64_RESET);
    myActions.reboot   = new Action("Reboot C64", SUBSYSID_C64, MENU_C64_REBOOT);
    myActions.clearmem = new Action("Reboot (Clr Mem)", SUBSYSID_C64, MENU_C64_CLEARMEM);
    myActions.powercyc = new Action("Power Cycle", SUBSYSID_C64, MENU_C64_POWERCYCLE);
    myActions.off      = new Action("Power OFF", SUBSYSID_C64, MENU_C64_POWEROFF);
    myActions.pause    = new Action("Pause",  SUBSYSID_C64, MENU_C64_PAUSE);
    myActions.resume   = new Action("Resume", SUBSYSID_C64, MENU_C64_RESUME);
    myActions.savereu  = new Action("Save REU Memory", SUBSYSID_C64, MENU_C64_SAVEREU);
    myActions.savemem  = new Action("Save C64 Memory", SUBSYSID_C64, MENU_U64_SAVERAM);
    myActions.save_crt = new Action("Save Cartridge", SUBSYSID_C64, MENU_C64_SAVE_CARTRIDGE);
    myActions.savemp3a = new Action("Save MP3 Drv A", SUBSYSID_C64, MENU_C64_SAVE_MP3_DRV_A);
    myActions.savemp3b = new Action("Save MP3 Drv B", SUBSYSID_C64, MENU_C64_SAVE_MP3_DRV_B);
    myActions.savemp3c = new Action("Save MP3 Drv C", SUBSYSID_C64, MENU_C64_SAVE_MP3_DRV_C);
    myActions.savemp3d = new Action("Save MP3 Drv D", SUBSYSID_C64, MENU_C64_SAVE_MP3_DRV_D);
    myActions.measure  = new Action("Measure Cart Bus", SUBSYSID_C64, MENU_MEASURE_TIMING);
#if !U64
    myActions.monitor  = register_c64_machine_monitor_task ? register_c64_machine_monitor_task(MENU_C64_MONITOR) : NULL;
#endif

    taskCategory->append(myActions.reset);
    taskCategory->append(myActions.reboot);
#if U64
    taskCategory->append(myActions.clearmem);
    taskCategory->append(myActions.off);
#endif
#if U64 == 2
    taskCategory->append(myActions.powercyc);
#endif
#if DEVELOPER > 0
    taskCategory->append(myActions.pause);
    taskCategory->append(myActions.resume);
    taskCategory->append(myActions.measure);
#endif
#if U64
    taskCategory->append(myActions.savemem);
#endif
    taskCategory->append(myActions.savereu);
    taskCategory->append(myActions.save_crt);
    taskCategory->append(myActions.savemp3a);
    taskCategory->append(myActions.savemp3b);
    taskCategory->append(myActions.savemp3c);
    taskCategory->append(myActions.savemp3d);
}

void C64_Subsys :: update_task_items(bool writablePath)
{
    if (C64 :: isMP3RamDrive(0) > 0) myActions.savemp3a->show(); else myActions.savemp3a->hide();
    if (C64 :: isMP3RamDrive(1) > 0) myActions.savemp3b->show(); else myActions.savemp3b->hide();
    if (C64 :: isMP3RamDrive(2) > 0) myActions.savemp3c->show(); else myActions.savemp3c->hide();
    if (C64 :: isMP3RamDrive(3) > 0) myActions.savemp3d->show(); else myActions.savemp3d->hide();

    if (C64_CRT :: is_valid()) {
        myActions.save_crt->show();
    } else {
        myActions.save_crt->hide();
    }

    if (writablePath) {
#if U64
        myActions.savemem  ->enable();
#endif
        myActions.savereu  ->enable();
        myActions.save_crt ->enable();
        myActions.savemp3a ->enable();
        myActions.savemp3b ->enable();
        myActions.savemp3c ->enable();
        myActions.savemp3d ->enable();
    } else {
#if U64
        myActions.savemem  ->disable();
#endif
        myActions.savereu  ->disable();
        myActions.save_crt ->disable();
        myActions.savemp3a ->disable();
        myActions.savemp3b ->disable();
        myActions.savemp3c ->disable();
        myActions.savemp3d ->disable();
    }
}


void C64_Subsys :: restoreCart(void)
{
	for (int i=0; (i<500) && (C64_CARTRIDGE_ACTIVE); i++) {
		vTaskDelay(2);
	}
	if (C64_CARTRIDGE_ACTIVE) {
	    printf("Error.. cart did not get disabled.\n");
	    c64->init_cartridge(); // hard reset
	} else {
	    printf("Cart got disabled, now restoring.\n");
	    c64->set_cartridge(NULL);
        C64_CARTRIDGE_KILL = 2; // Force update
        C64_CARTRIDGE_KILL = 2; // Force update
	}
}


SubsysResultCode_e C64_Subsys::executeCommand(SubsysCommand *cmd)
{
    File *f = 0;
    FRESULT res;
    CachedTreeNode *po;
    uint32_t transferred;
    int ram_size;
    char buffer[64] = "memory";
    uint8_t *pb;
    SubsysResultCode_e result = SSRET_OK;

    switch (cmd->functionID) {
        case C64_PUSH_BUTTON:
            c64->setButtonPushed();
            break;
        case C64_UNFREEZE:
            if (c64->client) { // we can't execute this yet
                c64->client->release_host(); // disconnect from user interface
                c64->client = 0;
            }
            c64->unfreeze();
            break;
        case MENU_C64_RESET:
            crt_autosave_before_reset(); // with Auto only
            if (machine_monitor_request_global_reset_cancel) {
                machine_monitor_request_global_reset_cancel();
            }
            if (c64->client) { // we can't execute this yet
                c64->client->release_host(); // disconnect from user interface
                c64->client = 0;
            }
            c64->unfreeze();
            // Cold-start if Debug may have left ROM banked out.
            if (machine_monitor_global_reset_sees_debug_session &&
                machine_monitor_global_reset_sees_debug_session()) {
                c64->start_cartridge(NULL);
            } else {
                c64->reset();
            }
            break;

        case MENU_C64_PAUSE:
            c64->stop(false);
            break;

        case MENU_C64_RESUME:
            c64->resume();
            break;

        case MENU_C64_CLEARMEM:
            crt_autosave_before_reset(); // with Auto only
#if U64
            if (c64->client) { // we can't execute this yet
                c64->client->release_host(); // disconnect from user interface
                c64->client = 0;
            }
            c64->clear_ram();
            c64->start_cartridge(NULL);
            break;
#endif

        case MENU_C64_POWERCYCLE:
            crt_autosave_before_reset(); // with Auto only
#if U64 == 2
            wifi_machine_reboot();
#else
            result = SSRET_NOT_IMPLEMENTED;
#endif
            break;
        case MENU_C64_POWEROFF:
            crt_autosave_before_reset(); // with Auto only
#if U64 == 2
            wifi_machine_off();
#elif U64 == 1
            U64_POWER_REG = 0x2B;
            U64_POWER_REG = 0xB2;
            U64_POWER_REG = 0x2B;
            U64_POWER_REG = 0xB2;
#else
            result = SSRET_NOT_IMPLEMENTED;
#endif
            break;

        case MENU_C64_REBOOT:
            crt_autosave_before_reset(); // with Auto only
            if (c64->client) { // we can't execute this yet
                c64->client->release_host(); // disconnect from user interface
                c64->client = 0;
            }
            c64->start_cartridge(NULL);
            break;

        case C64_START_CART:
            crt_autosave_before_reset(); // with Auto only
            if (c64->client) { // we can't execute this yet
                c64->client->release_host(); // disconnect from user interface
                c64->client = 0;
            }
            c64->start_cartridge((cart_def *)cmd->mode);
            break;

        case MENU_C64_SAVEREU:
            if (!C64 :: reu_memory_usable()) { // T60: the memory holds a cartridge image, not REU data
                cmd->user_interface->popup(C64 :: reu_memory_busy_text(), BUTTON_OK);
                result = SSRET_REU_IN_USE;
                break;
            }
            ram_size = 128 * 1024;
            ram_size <<= c64->cfg->get_value(CFG_C64_REU_SIZE);

            if ((cmd->user_interface->string_box("Save REU memory as..", buffer, 22) > 0) && (*buffer)) {
                fix_filename(buffer);
                set_extension(buffer, ".reu", 32);

                res = create_file_ask_if_exists(fm, cmd->user_interface, cmd->path.c_str(), buffer, &f);
                if (res == FR_OK) {
                    printf("Opened file successfully.\n");

                    uint32_t reu_sizes[] =
                            { 0x20000, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000, 0x1000000 };

                    uint32_t current_reu_size = reu_sizes[c64->cfg->get_value(CFG_C64_REU_SIZE)];
                    uint32_t sectors = (current_reu_size >> 9);
                    uint32_t secs_per_step = (sectors + 31) >> 5;
                    uint32_t bytes_per_step = secs_per_step << 9;
                    uint32_t bytes_written;
                    uint32_t remain = current_reu_size;
                    uint8_t *src;
                    transferred = 0;

                    cmd->user_interface->show_progress("Saving REU file..", 32);
                    src = (uint8_t *)REU_MEMORY_BASE;

                    while (remain != 0) {
                        f->write(src, bytes_per_step, &bytes_written);
                        transferred += bytes_written;
                        cmd->user_interface->update_progress(NULL, 1);
                        remain -= bytes_per_step;
                        src += bytes_per_step;
                    }
                    cmd->user_interface->hide_progress();

                    printf("written: %d...", transferred);
                    sprintf(buffer, "Bytes saved: %d ($%8x)", transferred, transferred);
                    cmd->user_interface->popup(buffer, BUTTON_OK);

                    fm->fclose(f);
                } else {
                    printf("Couldn't open file..\n");
                    cmd->user_interface->popup(FileSystem::get_error_string(res), BUTTON_OK);
                }
            }
            break;

        case MENU_U64_SAVERAM:
            ram_size = 64 * 1024;
            if ((cmd->user_interface->string_box("Save RAM as..", buffer, 22) > 0) && (*buffer)) {
                fix_filename(buffer);
                set_extension(buffer, ".bin", 32);

                res = create_file_ask_if_exists(fm, cmd->user_interface, cmd->path.c_str(), buffer, &f);
                if (res == FR_OK) {
                    printf("Opened file successfully.\n");

                    pb = new uint8_t[ram_size];
                    c64->get_all_memory(pb);

                    uint32_t bytes_written;
                    f->write(pb, ram_size, &bytes_written);

                    printf("written: %d...", bytes_written);
                    sprintf(buffer, "bytes saved: %d ($%8x)", bytes_written, bytes_written);
                    cmd->user_interface->popup(buffer, BUTTON_OK);

                    fm->fclose(f);
                    delete[] pb;
                } else {
                    printf("Couldn't open file..\n");
                    cmd->user_interface->popup(FileSystem::get_error_string(res), BUTTON_OK);
                }
            }
            break;

        case MENU_C64_SAVE_CARTRIDGE:
            buffer[0] = 0;
            if ((cmd->user_interface->string_box("Save Cartridge as..", buffer, 28) > 0) && (*buffer)) {
                fix_filename(buffer);
                set_extension(buffer, ".crt", 32);

                res = create_file_ask_if_exists(fm, cmd->user_interface, cmd->path.c_str(), buffer, &f);
                if (res == FR_OK) {
                    // Overlay and REST get here with the C64 running; stopped, the file is one image.
                    xSemaphoreTake(crt_save_mutex, portMAX_DELAY);
                    bool stopped = c64->begin_stopped_session();
                    SubsysResultCode_e retval = C64_CRT::save_crt(f);
                    c64->end_stopped_session(stopped);
                    xSemaphoreGive(crt_save_mutex);
                    fm->fclose(f);
                    if (retval != SSRET_OK) {
                        cmd->user_interface->popup(SubsysCommand::error_string(retval), BUTTON_OK);
                    } else {
                        C64_CRT::set_source(cmd->path.c_str(), buffer); // the cartridge now belongs to this file
                    }
                }
            }
            break;

        case MENU_C64_SAVEFLASH: // doesn't belong here, but i need it fast
            res = create_file_ask_if_exists(fm, cmd->user_interface, cmd->path.c_str(), "flash_dump.bin", &f);

            if (res == FR_OK) {
                int pages = c64->flash->get_number_of_pages();
                int page_size = c64->flash->get_page_size();
                uint8_t *page_buf = new uint8_t[page_size];
                for (int i = 0; i < pages; i++) {
                    c64->flash->read_page(i, page_buf);
                    f->write(page_buf, page_size, &transferred);
                }
                delete[] page_buf;
                fm->fclose(f);
            } else {
                printf("Couldn't open file..\n");
            }
            break;

        case MENU_MEASURE_TIMING:
            measure_timing(cmd->path.c_str());
            break;

        case MENU_MEASURE_TIMING_API:
            c64->measure_timing((uint8_t *)cmd->buffer);
            break;

        case C64_DMA_LOAD:
            res = fm->fopen(cmd->path.c_str(), cmd->filename.c_str(), FA_READ, &f);
            if (res == FR_OK) {
                dma_load(f, NULL, 0, cmd->filename.c_str(), cmd->mode, c64->cfg->get_value(CFG_C64_DMA_ID));
                fm->fclose(f);
            } else {
                result = SSRET_CANNOT_OPEN_FILE;
            }
            break;

#ifndef RECOVERYAPP
        case C64_DMA_LOAD_MNT:
            res = fm->fopen(cmd->path.c_str(), cmd->filename.c_str(), FA_READ, &f);
            if (res == FR_OK) {
                dma_load(f, NULL, 0, cmd->filename.c_str(), cmd->mode, c1541_A->get_current_iec_address());
                fm->fclose(f);
            } else {
                result = SSRET_CANNOT_OPEN_FILE;
            }
            break;
#endif
        case C64_DMA_LOAD_RAW:
            res = fm->fopen(cmd->path.c_str(), cmd->filename.c_str(), FA_READ, &f);
            if (res == FR_OK) {
                dma_load_raw(f);
                fm->fclose(f);
            } else {
                result = SSRET_CANNOT_OPEN_FILE;
            }
            break;
        case C64_DRIVE_LOAD:
            dma_load(0, NULL, 0, cmd->filename.c_str(), cmd->mode, cmd->path.c_str()[0] & 0x1F);
            if (cmd->user_interface) {
                cmd->user_interface->menu_response_to_action = MENU_HIDE;
            }
            break;
        case C64_DMA_BUFFER:
            // dma_load() returns -1 when the boot-cart handshake times out,
            // which means nothing was launched. Discarding it reports a load
            // that never happened as a success, and callers that act on the
            // result then wait for a program that is not running.
            if (dma_load(0, (const uint8_t *)cmd->buffer, cmd->bufferSize, cmd->filename.c_str(), cmd->mode,
                    c64->cfg->get_value(CFG_C64_DMA_ID)) < 0) {
                result = SSRET_GENERIC_ERROR;
            }
            break;
        case C64_DMA_RAW_WRITE:
            dma_load_raw_buffer((uint16_t)cmd->mode, (uint8_t *)cmd->buffer, cmd->bufferSize, 0);
            break;
        case C64_DMA_RAW_READ:
            dma_load_raw_buffer((uint16_t)cmd->mode, (uint8_t *)cmd->buffer, cmd->bufferSize, 1);
            break;
        case C64_LENS_READ:
            lens_read_buffer((uint16_t)cmd->mode, cmd->path.c_str(), cmd->filename == "sidefx", (uint8_t *)cmd->buffer,
                             cmd->bufferSize);
            break;
        case C64_STOP_COMMAND:
            c64->stop(false);
            break;
        case MENU_C64_HARD_BOOT:
            c64->flash->reboot(0);
            break;
        case C64_READ_FLASH:
            switch (cmd->mode) {
                case FLASH_CMD_PAGESIZE:
                    *(int*)cmd->buffer = c64->flash->get_page_size();
                    break;
                case FLASH_CMD_NOPAGES:
                    *(int*)cmd->buffer = c64->flash->get_number_of_pages();
                    break;
                default:
                    if (cmd->bufferSize >= c64->flash->get_page_size())
                        c64->flash->read_page(cmd->mode - FLASH_CMD_GETPAGE, cmd->buffer);
            }
            break;
        case C64_SET_KERNAL:
            c64->enable_kernal((uint8_t*)cmd->buffer);
            // c64->reset();
            break;
        case MENU_C64_SAVE_MP3_DRV_A:
            case MENU_C64_SAVE_MP3_DRV_B:
            case MENU_C64_SAVE_MP3_DRV_C:
            case MENU_C64_SAVE_MP3_DRV_D:
            {
            int devNo = cmd->functionID - MENU_C64_SAVE_MP3_DRV_A;
            int ftype = C64::isMP3RamDrive(devNo);
            uint8_t* reu = (uint8_t *)(REU_MEMORY_BASE);
            int expSize = 1;
            const char* extension = 0;

            if (ftype == 1541) {
                expSize = 174848;
                extension = ".D64";
            }
            if (ftype == 1571) {
                expSize = 2 * 174848;
                extension = ".D71";
            }
            if (ftype == 1581) {
                expSize = 819200;
                extension = ".D81";
            }
            if (ftype == DRVTYPE_MP3_DNP) {
                expSize = C64::getSizeOfMP3NativeRamdrive(devNo);
                extension = ".DNP";
            }

            uint8_t ramBase = reu[0x7dc7 + devNo];
            uint8_t* srcAddr = reu + (((uint32_t)ramBase) << 16);

            if ((cmd->user_interface->string_box("Save RAMDISK as..", buffer, 22) > 0) && (*buffer)) {
                fix_filename(buffer);
                set_extension(buffer, extension, 32);

                res = create_file_ask_if_exists(fm, cmd->user_interface, cmd->path.c_str(), buffer, &f);
                if (res == FR_OK) {
                    printf("Opened file successfully.\n");
                    uint32_t bytes_written;

                    if (ftype == 1571) {
                        f->write(srcAddr, expSize / 2, &bytes_written);
                        uint32_t tmp;
                        f->write(srcAddr + 700 * 256, expSize / 2, &tmp);
                        bytes_written += tmp;
                    } else {
                        f->write(srcAddr, expSize, &bytes_written);
                    }
                    printf("written: %d...", bytes_written);
                    sprintf(buffer, "bytes saved: %d ($%8x)", bytes_written, bytes_written);
                    cmd->user_interface->popup(buffer, BUTTON_OK);

                    fm->fclose(f);
                } else {
                    printf("Couldn't open file..\n");
                    cmd->user_interface->popup(FileSystem::get_error_string(res), BUTTON_OK);
                }
            }
        }
            break;

        default:
            break;
    }

    return result;
}

int C64_Subsys :: dma_load_raw(File *f)
{
	bool i_stopped_it = false;
	if (c64->client) {
    	c64->client->release_host(); // disconnect from user interface
    	c64->release_ownership();
	}
	if(!c64->isFrozen) {
		c64->stop(false);
        C64_MODE = MODE_NORMAL;
		i_stopped_it = true;
	}
	int bytes = load_file_dma(f, 0);

	if (i_stopped_it) {
		c64->resume();
	}
	return bytes;
}

// REST readmem with a lens (TRX64 bitstream, T21 §3.4.1). Unlike the DMA path it does not take the menu's
// client from the user interface. The lenses that read the state bus hold the machine still for the
// call (the freeze menu's stop, or the debugger's, is left as it is); RAM and ROM need no pause.
int C64_Subsys :: lens_read_buffer(uint16_t offset, const char *lens, bool sidefx, uint8_t *buffer, int length)
{
    bool i_stopped_it = false;
    if (trx_lens_needs_pause(lens) && !c64->is_stopped()) {
        c64->stop(false);
        i_stopped_it = true;
    }
    trx_lens_read(lens, offset, buffer, length, sidefx);
    if (c64->isFrozen && (!strcmp(lens, "ram") || !strcmp(lens, "cpu") || !strcmp(lens, "io"))) {
        c64->overlay_frozen_backups(offset, buffer, length);
    }
    if (i_stopped_it) {
        c64->resume();
    }
    return length;
}

int C64_Subsys :: dma_load_raw_buffer(uint16_t offset, uint8_t *buffer, int length, int rw)
{
    // Must not release the host while frozen, or it would tear down the freeze
    // monitor; the frozen path instead routes through dma_transfer_frozen,
    // which honours the freezer's own mode/cart banking.
    bool i_stopped_it = false;
    if (c64->client && !c64->isFrozen) {
        c64->client->release_host(); // disconnect from user interface
        c64->release_ownership();
    }
    if(!c64->is_stopped()) {
        c64->stop(false);
        i_stopped_it = true;
    }

    if (c64->isFrozen) {
        c64->dma_transfer_frozen(offset, buffer, length, rw);
    } else {
        volatile uint8_t *dest = (volatile uint8_t *)(C64_MEMORY_BASE + offset);

        if (rw) {
            memcpy(buffer, (void *)dest, length);
        } else {
            memcpy((void *)dest, buffer, length);
        }
    }

    if (i_stopped_it) {
        c64->resume();
    }
	return length;
}

int C64_Subsys :: dma_load(File *f, const uint8_t *buffer, const int bufferSize,
		const char *name, uint8_t run_code, uint8_t drv, uint16_t reloc)
{
	// prepare DMA load
    // Keep the client attached while a debug session owns the machine: a
    // cartridge-target debug step's run comes back through this boot cart, and
    // releasing it here skips the release_ownership() unfreeze, leaving the
    // machine DMA-held after the monitor closes.
    if(c64->client && !(machine_monitor_global_reset_sees_debug_session &&
                        machine_monitor_global_reset_sees_debug_session())) {
    	c64->client->release_host(); // disconnect from user interface
    	c64->client = 0;
	}
    if(!c64->isFrozen) {
    	c64->stop(false);
    }

    C64_POKE(C64_BOOTCRT_RUNCODE, run_code);

    C64_POKE(C64_BOOTCRT_DOSYNC, (c64->cfg->get_value(CFG_C64_DO_SYNC) == 1) ? 1 : 0);

    char trimmed_name[17];
    const char *display_name = format_bootcrt_display_name(name, trimmed_name);

    CbmFileName cbm;
    cbm.init(display_name);
    const char *p = cbm.getName();
    for(int i=0; i<16; i++) {
        C64_POKE(C64_BOOTCRT_NAME+i, p[i]);
    }
    C64_POKE(C64_BOOTCRT_NAMELEN, strlen(p));

    C64_POKE(C64_BOOTCRT_HANDSHAKE, 0x80); // initial boot cart handshake

    boot_cart.custom_addr = (void *)&_bootcrt_65_start;
    c64->start_cartridge(&boot_cart);

	// perform DMA load
    wait_ms(100);

	// handshake with boot cart
    c64->stop(false);

    C64_POKE(C64_BOOTCRT_DRIVENUM, drv);    // drive number for printout

	C64_POKE(C64_BOOTCRT_HANDSHAKE, 0x40);  // signal cart ready for DMA load

	if ( !(run_code & RUNCODE_REAL_BIT) ) {
        int timeout = 0;
        while(C64_PEEK(2) != 0x01) {
        	c64->resume();
            timeout++;
            if(timeout == 60) {
                c64->init_cartridge();
                return -1;
            }
            wait_ms(25);
            printf("_");
            c64->stop(false);
        }
        if (f) {
        	load_file_dma(f, 0);
        } else {
            // If we need to jump, the first two bytes are the jump address
            if (run_code & RUNCODE_JUMP_BIT) {
            	C64_POKE(C64_BOOTCRT_JUMPADDR, buffer[0]);
            	C64_POKE(C64_BOOTCRT_JUMPADDR+1, buffer[1]);
                if (bufferSize > 2) {
                    load_buffer_dma(buffer+2, bufferSize-2, 0);
                }
            } else {
            	load_buffer_dma(buffer, bufferSize, 0);
            }
        }

        C64_POKE(C64_BOOTCRT_HANDSHAKE, 0); // signal DMA load done
        C64_POKE(C64_BOOTCRT_RUNCODE, run_code);
        C64_POKE(0x00BA, drv);    // fix drive number
	}

	printf("Resuming..\n");
    c64->resume();

    restoreCart();
    return 0;
}

int C64_Subsys :: load_file_dma(File *f, uint16_t reloc)
{
	uint32_t dma_load_buffer[128];
    uint32_t transferred = 0;
    uint16_t load_address = 0;
    uint8_t  *dma_load_buffer_b;
    dma_load_buffer_b = (uint8_t *)dma_load_buffer;

    if (f) {
        f->read(&load_address, 2, &transferred);
        if(transferred != 2) {
            printf("Can't read from file..\n");
            return -1;
        }
        load_address = le2cpu(load_address); // make sure we can interpret the word correctly (endianness)
        printf("Load address: %4x...", load_address);
    }
    if(reloc) {
    	load_address = reloc;
    	printf(" -> %4x ..", load_address);
    }
    int max_length = 65536 - int(load_address); // never exceed $FFFF
    int block = 510;
    printf("Now loading...");
	volatile uint8_t *dest = (volatile uint8_t *)(C64_MEMORY_BASE + load_address);

	/* Now actually load the file */
	int total_trans = 0;
	while (max_length > 0) {
		FRESULT fres = f->read(dma_load_buffer, block, &transferred);
		if (fres != FR_OK) {
			printf("Error reading from file. %s\n", FileSystem :: get_error_string(fres));
			return -1;
		}
		//printf("[%d]", transferred);
		total_trans += transferred;
		volatile uint8_t *d = dest;
		for (int i=0;i<transferred;i++) {
			*(d++) = dma_load_buffer_b[i];
		}
		if (transferred < block) {
			break;
		}
		dest += transferred;
		max_length -= transferred;
		block = (max_length > 512) ? 512 : max_length;
	}
	uint16_t end_address = load_address + total_trans;
	printf("DMA load complete: $%4x-$%4x\n", load_address, end_address);

	// The following pokes are documented in the C64 documentation and are not
	// Ultimate specific.
	C64_POKE(0x002D, end_address);
	C64_POKE(0x002E, end_address >> 8);
	C64_POKE(0x002F, end_address);
	C64_POKE(0x0030, end_address >> 8);
	C64_POKE(0x0031, end_address);
	C64_POKE(0x0032, end_address >> 8);
	C64_POKE(0x00AE, end_address);
	C64_POKE(0x00AF, end_address >> 8);

	C64_POKE(0x0090, 0x40); // Load status
	C64_POKE(0x0035, 0);    // FRESPC
	C64_POKE(0x0036, 0xA0);

	return total_trans;
}

int C64_Subsys :: load_buffer_dma(const uint8_t *buffer, const int bufferSize, uint16_t reloc)
{
    if (!buffer || bufferSize < 2) {
        return 0;
    }
    uint16_t load_address = 0;

    load_address = (uint16_t)buffer[0] | ((uint16_t)buffer[1]) << 8;
	printf("Load address: %4x...", load_address);

	if (reloc) {
    	load_address = reloc;
    	printf(" -> %4x ..", load_address);
    }
	volatile uint8_t *dest = (volatile uint8_t *)(C64_MEMORY_BASE + load_address);

	memcpy((void *)dest, buffer + 2, bufferSize - 2);

	uint16_t end_address = load_address + bufferSize - 2;
	printf("DMA load complete: $%4x-$%4x\n", load_address, end_address);

    // The following pokes are documented in the C64 documentation and are not
    // Ultimate specific.
	C64_POKE(0x002D, end_address);
	C64_POKE(0x002E, end_address >> 8);
	C64_POKE(0x002F, end_address);
	C64_POKE(0x0030, end_address >> 8);
	C64_POKE(0x0031, end_address);
	C64_POKE(0x0032, end_address >> 8);
	C64_POKE(0x00AE, end_address);
	C64_POKE(0x00AF, end_address >> 8);

	C64_POKE(0x0090, 0x40); // Load status
	C64_POKE(0x0035, 0);    // FRESPC
	C64_POKE(0x0036, 0xA0);

	return bufferSize - 2;
}

bool C64_Subsys :: write_vic_state(File *f)
{
    uint32_t transferred;
    uint8_t mode = C64_MODE;
    C64_MODE = 0;

    // find bank
    uint8_t bank = 3 - (c64->cia_backup[1] & 0x03);
    uint32_t addr = C64_MEMORY_BASE + (uint32_t(bank) << 14);
    if(bank == 0) {
        f->write((uint8_t *)C64_MEMORY_BASE, 0x0400, &transferred);
        f->write(c64->screen_backup, 0x0400, &transferred);
        f->write(c64->ram_backup, 0x0800, &transferred);
        f->write((uint8_t *)(C64_MEMORY_BASE + 0x1000), 0x3000, &transferred);
    } else {
        f->write((uint8_t *)addr, 16384, &transferred);
    }
    f->write(c64->color_backup, 0x0400, &transferred);
    f->write(c64->vic_backup, NUM_VICREGS, &transferred);

    C64_MODE = mode;
    return true;
}

void C64_Subsys :: measure_timing(const char *path)
{
    uint8_t *buffer = new uint8_t[64 * 1024];

    c64->measure_timing(buffer);

    // write file
    mstring fn(path);
    fn += "bus_measurement.bin";
    FILE *fo = fopen(fn.c_str(), "wb");
    if (fo) {
        int t = fwrite(buffer, 2048, 32, fo);
        printf("%d blocks written.\n", t);
        fclose(fo);
    } else {
        printf("Couldn't open file %s\n", fn.c_str());
    }

    delete[] buffer;
}
