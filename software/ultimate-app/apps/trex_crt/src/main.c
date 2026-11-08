/*
 * main.c
 *
 *  TREX CRT Tool entry point. Opens the Cart Inspector, the cartridge dumper
 *  or the cartridge writer, chosen by the manifest action the firmware passes
 *  in the launch context. A plain "Run App" shows a chooser first.
 */

#include "cart_base.h"
#include "cart_view_text.h"
#include "ui_shim.h"

const u2p_api_t *g_api;

// The tools run on the character screen of this app (cart_view_text.c); the cores behind it know no screen
#define run_dumper    cart_view_text_dumper
#define run_writer    cart_view_text_writer
#define run_inspector cart_view_text_inspector

// Same entries as the task menu of the manifest
static void run_chooser(void)
{
    static const char *items[] = { "Cart Inspector", "Dump Cartridge", "Flash Cartridge" };
    switch (u2p_ui_menu("TREX CRT Tool", items, 3)) {
    case 0:
        run_inspector();
        break;
    case 1:
        run_dumper();
        break;
    case 2:
        run_writer("", "");
        break;
    default:
        break;
    }
}

// Splits "/Usb0/games/foo.crt" into "/Usb0/games" and "foo.crt"
static void split_path(const char *full, char *dir, int dir_size, char *name, int name_size)
{
    int slash = -1;
    for (int i = 0; full[i]; i++) {
        if (full[i] == '/') {
            slash = i;
        }
    }
    int n = 0;
    if (slash == 0) {
        dir[n++] = '/';
    }
    for (int i = 0; i < slash && n < dir_size - 1; i++) {
        dir[n++] = full[i];
    }
    dir[n] = 0;
    strncpy(name, full + slash + 1, name_size - 1);
    name[name_size - 1] = 0;
}

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx)
{
    g_api = api;
    u2p_ui_init(api);
    cart_port_set(cart_port_platform()); // the machine under the tools, before anything touches it
    core_detect();

    const char *action = NULL;
    const char *selected = NULL;
    if (ctx && ctx->size >= sizeof(u2p_app_context_t)) {
        action = ctx->action;
        selected = ctx->selected_file;
    }
    LOG("[TrexCrt] action=%s file=%s\n", action ? action : "-", selected ? selected : "-");

    if (!action) {
        run_chooser();
    } else if (strcmp(action, "dump") == 0) {
        run_dumper();
    } else if (strcmp(action, "inspect") == 0) {
        run_inspector();
    } else if (strcmp(action, "flash") == 0) {
        run_writer("", "");
    } else if (strcmp(action, "write") == 0 && selected) {
        static char dir[256];
        static char name[256];
        split_path(selected, dir, sizeof(dir), name, sizeof(name));
        run_writer(dir, name);
    } else if (strcmp(action, "write") == 0) {
        run_writer("", "");
    } else if (strcmp(action, "verify") == 0 && selected) {
        static char dir[256];
        static char name[256];
        split_path(selected, dir, sizeof(dir), name, sizeof(name));
        run_writer(dir, name); // the writer's screen: F5 verifies
    } else {
        run_chooser();
    }
    slot_wr_close(); // the tools close it themselves; once more for any way out they missed
    return 0;
}
