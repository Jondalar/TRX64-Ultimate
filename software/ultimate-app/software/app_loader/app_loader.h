/*
 * app_loader.h
 *
 * U64 App Loader — loads and runs external .u2a apps.
 * See doc/AppLoader_Architecture.md for design.
 */

#ifndef APP_LOADER_APP_LOADER_H_
#define APP_LOADER_APP_LOADER_H_

#include <stdint.h>

#include "app_format.h"
#include "app_manifest.h"
#include "subsys.h"

/* SubSystem ID (distinct from SUBSYSID_USER_STANDARD..SUBSYSID_ULTICOPY = 0..11) */
#define SUBSYSID_APP_LOADER 12

/* SubsysCommand functions */
#define APP_CMD_RUN 0x01        /* "Run App": launch .u2a at (path, filename), register its manifest */
#define APP_CMD_RUN_ACTION 0x02 /* registry action: launch .u2a at (path, filename), buffer = AppLaunchParams */

/* Context of a launch from a registry entry (doc/AppLoader_Spec.md section 3.4) */
struct AppLaunchParams
{
    const char *selected_file; /* full path of the browsed file, NULL from the task menu */
    uint32_t launch_source;    /* enum u2p_launch_source_e */
    const char *action;        /* manifest action */
    const char *args;          /* REST launch: passed on as ctx->args, else NULL */
    char *reply;               /* REST launch: receives what the app hands to ctx->reply, else NULL */
    int reply_size;            /* bytes at reply, including the terminating NUL */
    int exit_code;             /* set by the loader when the app has returned */
};

class AppLoader : public SubSystem
{
  public:
    AppLoader();
    virtual ~AppLoader();

    virtual const char *identify(void) override { return "App Loader"; }

  protected:
    virtual SubsysResultCode_e executeCommand(SubsysCommand *cmd) override;

  private:
    SubsysResultCode_e run_app(SubsysCommand *cmd, AppLaunchParams *params);
};

/* Reads and validates the manifest of the app file at 'path' without loading it and without a popup.
 * Returns NULL on success, otherwise a short reason (also for a file without manifest). */
const char *app_read_manifest(const char *path, AppManifest &out);

/* Global instance, constructed at static init time */
extern AppLoader *g_app_loader;

#endif /* APP_LOADER_APP_LOADER_H_ */
