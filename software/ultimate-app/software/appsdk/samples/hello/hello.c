/*
 * hello.c — simplest .u2a app. Prints a greeting, returns.
 */

#include "u2p_app.h"

int app_main(const u2p_api_t *api, const u2p_app_context_t *ctx)
{
    api->sys->log("Hello from U64 App!\n");
    api->sys->log("  app_path = %s\n", ctx->app_path ? ctx->app_path : "(null)");
    api->sys->log("  firmware = %s, ABI %d.%d\n", api->firmware_version, api->abi_major, api->abi_minor);
    if (ctx->selected_file) {
        api->sys->log("  selected = %s\n", ctx->selected_file);
    }

    /* Exercise malloc tracking (intentionally leak one — loader should auto-free). */
    void *leak = api->mem->malloc(123);
    api->sys->log("  malloc(123) = %p (leaked on purpose)\n", leak);
    void *freed = api->mem->malloc(456);
    api->mem->free(freed);

    /* A REST launch has no screen: answer the caller instead. */
    if (!ctx->has_ui) {
        ctx->reply("hello");
        return 0;
    }

    /* Custom screen demo. */
    void *scr = api->ui->screen_open("Hello Screen", 38, 10);
    if (scr) {
        api->ui->screen_puts(scr, 2, 2, "Custom Window", 1);
        api->ui->screen_puts(scr, 2, 4, "Press OK to continue...", -1);
        api->ui->popup("Screen API", "Can you see the custom window behind?");
        api->ui->screen_close(scr);
    }

    api->ui->popup("Hello App", "All good, returning.");
    return 0;
}
