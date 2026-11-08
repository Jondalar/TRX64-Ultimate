/*
 * Loading a .c64re / .c64rering from the file browser (T66,
 * docs/specs/T20-delta-ring.md §8.2 and §9.4; the decision is ADR-0064).
 *
 * The file carries this board's own chip block, so the machine comes back
 * exactly as it was at that frame boundary - the CPU registers and the CIA
 * flip-flops included, which is why it runs on instead of only being looked
 * at. A file without this build's chip block - a dump from the c64re app, or
 * one from TRX64 - takes the coarse load of §9.4 instead: its nodes are laid
 * on the machine's own newest frame, so the ring has to be running.
 *
 * Why in two steps: the browser runs with the machine frozen, and the menu
 * puts its own backup back when it closes, which would undo a load made
 * while it was up. So the file is read and checked when it is chosen - a
 * wrong file says so at once, in front of the user - and goes into the
 * machine when the menu closes, the way a Timewarp rewind does
 * (trx_ring_set_pending / C64::unfreeze()).
 */
#include "filetype_c64re.h"
#include "filemanager.h"
#include "c64.h"
#include "menu.h"
#include "userinterface.h"
#include "subsys.h"
#include "trx_c64re.h"
#include "trx_trxring.h"
#include "trx_ring.h"
#include "trx_ring_reu.h"
#include "trx_io.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// tester instance
FactoryRegistrator<BrowsableDirEntry *, FileType *> tester_c64re(FileType :: getFileTypeFactory(), FileTypeC64re :: test_type);

#define C64REFILE_LOAD   0x6401

#define C64RE_TYPE_SNAP  0      // .c64re, one frame
#define C64RE_TYPE_RING  1      // .c64rering, the ring save (its current entry loads)
#define C64RE_TYPE_NATIVE 2     // .trxring, the ring itself (T20 §10.2)

// The extension field of a FileInfo holds three characters (file_info.h), so
// both names arrive as "C64": the name itself says which one it is.
static bool name_ends_in(const char *name, const char *suffix)
{
    int n = strlen(name), s = strlen(suffix);
    if (n < s) {
        return false;
    }
    for (int i = 0; i < s; i++) {
        if (tolower(name[n - s + i]) != tolower(suffix[i])) {
            return false;
        }
    }
    return true;
}

FileTypeC64re :: FileTypeC64re(BrowsableDirEntry *node, int type)
{
    this->node = node;
    this->type = type;
}

FileTypeC64re :: ~FileTypeC64re()
{
}

int FileTypeC64re :: fetch_context_items(IndexedList<Action *> &list)
{
    if (!(trx_io_caps() & 0x04)) {      // CAPS b2: no frame ring, nothing to load into
        return 0;
    }
    // "Load State" is the counterpart of "Save State" in the C64 Machine menu
    // (U8 §5); a .trxring still brings the whole ring back, which is its own
    // thing and keeps its own name.
    list.append(new Action(type == C64RE_TYPE_NATIVE ? "Load Ring" : "Load State",
                           FileTypeC64re :: execute_st, C64REFILE_LOAD));
    return 1;
}

FileType *FileTypeC64re :: test_type(BrowsableDirEntry *obj)
{
    FileInfo *inf = obj->getInfo();
    const char *name = obj->getName();
    // get_extension() keeps three characters: ".c64re" and ".c64rering" both
    // arrive as "C64", ".trxring" as "TRX".
    if (strcmp(inf->extension, "TRX") == 0 && name_ends_in(name, ".trxring")) {
        return new FileTypeC64re(obj, C64RE_TYPE_NATIVE);
    }
    if (strcmp(inf->extension, "C64") != 0) {
        return NULL;
    }
    if (name_ends_in(name, ".c64rering")) {
        return new FileTypeC64re(obj, C64RE_TYPE_RING);
    }
    if (name_ends_in(name, ".c64re")) {
        return new FileTypeC64re(obj, C64RE_TYPE_SNAP);
    }
    return NULL;
}

SubsysResultCode_e FileTypeC64re :: execute_st(SubsysCommand *cmd)
{
    if (!cmd->user_interface) {
        return SSRET_NO_USER_INTERFACE;
    }
    const char *off = trx_ring_reu_refusal();   // A08: no ring, so nothing to load into
    if (off) {
        cmd->user_interface->popup(TRX_REU_OFF_POPUP, BUTTON_OK);
        return SSRET_OK;
    }
    // The browser hands the directory and the name apart; whether the
    // directory carries its slash depends on where it came from.
    const char *dir = cmd->path.c_str();
    size_t len = strlen(dir);
    const char *sep = (len && dir[len - 1] == '/') ? "" : "/";
    char path[1024];
    snprintf(path, sizeof(path), "%s%s%s", dir, sep, cmd->filename.c_str());

    if (cmd->mode == C64REFILE_LOAD && trx_trxring_is_one(path)) {
        // A .trxring: only the header is looked at now - the file is tens of
        // megabytes and is read when the menu closes.
        const char *bad = trx_trxring_check(path);
        if (bad) {
            char msg[192];
            snprintf(msg, sizeof(msg), "This ring cannot be loaded:\n%s", bad);
            cmd->user_interface->popup(msg, BUTTON_OK);
            return SSRET_CANNOT_OPEN_FILE;
        }
        C64 *machine = C64 :: getMachine();
        if (machine && machine->is_frozen()) {
            trx_trxring_set_pending(path);
            cmd->user_interface->menu_response_to_action = MENU_HIDE;
            return SSRET_OK;
        }
        const char *failed = trx_trxring_load(path);
        if (failed) {
            char msg[192];
            snprintf(msg, sizeof(msg), "The ring could not be loaded:\n%s", failed);
            cmd->user_interface->popup(msg, BUTTON_OK);
            return SSRET_CANNOT_OPEN_FILE;
        }
        cmd->user_interface->menu_response_to_action = MENU_HIDE;
        return SSRET_OK;
    }

    // U9: the same progress box the save has - a .c64re is over a megabyte and
    // reading it off an SD card or a stick is a visible wait
    struct LoadBox {
        UserInterface *ui;
        bool shown;
    } box = { cmd->user_interface, false };
    trx_progress_fn bar = [](int done, int total, void *ctx) {
        LoadBox *b = (LoadBox *)ctx;
        char m[32];
        if (done == 0) {
            b->ui->show_progress("Loading the state", total);
            b->shown = true;
            return;
        }
        sprintf(m, "Loading the state %d / %d", done, total);
        b->ui->update_progress(m, 1);
    };
    TrxC64reStaged *staged = NULL;
    const char *err = trx_c64re_stage_ex(path, &staged, bar, &box);
    if (box.shown) {
        cmd->user_interface->hide_progress();
    }
    if (err) {
        char line[224], msg[320];
        snprintf(line, sizeof(line), "This file cannot be loaded:\n%s", err);
        trx_c64re_wrap(msg, sizeof(msg), line,
                       cmd->user_interface->get_screen()->get_size_x() - 6);
        cmd->user_interface->popup(msg, BUTTON_OK);
        return SSRET_CANNOT_OPEN_FILE;
    }

    C64 *c64 = C64 :: getMachine();
    if (c64 && c64->is_frozen()) {
        // The menu holds the machine: load when it closes, as a rewind does.
        trx_c64re_set_pending(staged);
        cmd->user_interface->menu_response_to_action = MENU_HIDE;
        return SSRET_OK;
    }
    // No menu over the machine (the overlay browser with the machine running):
    // the file goes in now.
    err = trx_c64re_apply(staged);
    if (err) {
        char line[224], msg[320];
        snprintf(line, sizeof(line), "The state could not be loaded:\n%s", err);
        trx_c64re_wrap(msg, sizeof(msg), line,
                       cmd->user_interface->get_screen()->get_size_x() - 6);
        cmd->user_interface->popup(msg, BUTTON_OK);
        return SSRET_CANNOT_OPEN_FILE;
    }
    // U9 D6: the machine is back; say what did not come with it rather than
    // leave the user to notice. Over a frozen machine the load happens at
    // unfreeze, with the menu already gone - there the console carries it.
    const char *missed = trx_c64re_missed();
    if (missed) {
        char line[224], msg[320];
        snprintf(line, sizeof(line), "The state is loaded, but not:\n%s", missed);
        trx_c64re_wrap(msg, sizeof(msg), line,
                       cmd->user_interface->get_screen()->get_size_x() - 6);
        cmd->user_interface->popup(msg, BUTTON_OK);
    }
    cmd->user_interface->menu_response_to_action = MENU_HIDE;
    return SSRET_OK;
}
