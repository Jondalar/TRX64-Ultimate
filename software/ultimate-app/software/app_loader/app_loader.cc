/*
 * app_loader.cc
 *
 * Validates a .u2a app file (format version 2, doc/AppLoader_Spec.md section 1),
 * loads the variant for this CPU to the fixed app area and runs it in a
 * dedicated FreeRTOS task. The loader blocks until the app returns, then frees
 * whatever the app left allocated.
 */

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "app_api.h"
#include "app_api_net.h"
#include "app_format.h"
#include "app_loader.h"
#include "app_manifest.h"
#include "app_registry.h"
#include "c64.h"
#include "file.h"
#include "filemanager.h"
#include "init_function.h"
#include "ui_elements.h"
#include "userinterface.h"

#if defined(RISCV)
#define U2A_ARCH_NATIVE U2A_ARCH_RISCV32
#elif defined(NIOS)
#define U2A_ARCH_NATIVE U2A_ARCH_NIOS2
#else
#error "App Loader: unsupported CPU"
#endif

#if defined(RISCV)
// The NEORV32 instruction cache (fpga/cpu_unit/vhdl_source/neorv32_wrapper.vhd: direct-mapped, 256 blocks of 8
// bytes) is only invalidated by fence.i, which traps on cores built without Zifencei. Blocks of a previous app can
// still be cached when a new app is loaded to the same address. Fetching 2 KB of NOPs from firmware text refills
// every cache block before the new app runs.
extern "C" void app_icache_sweep(void);
__asm__(".pushsection .text.app_icache_sweep, \"ax\", @progbits\n"
        ".balign 4\n"
        ".global app_icache_sweep\n"
        "app_icache_sweep:\n"
        ".rept 512\n"
        "nop\n"
        ".endr\n"
        "ret\n"
        ".popsection\n");
#endif

/* Defined in app_api_impl.cc */
extern "C" u2p_api_t g_app_api;
extern "C" void app_api_refresh(void);
extern "C" void app_api_set_ui(UserInterface *ui);
extern "C" void app_api_set_reply(char *buf, int cap);
extern "C" int app_api_reply(const char *text);
extern "C" void app_api_clear_deferred(void);
extern "C" bool app_api_run_deferred(void);
extern "C" bool app_api_take_close_menu(void);
extern "C" void app_api_overlay_restore(void);
extern "C" bool app_ui_request_pending(void);
extern "C" bool app_ui_request_take(void (**fn)(void *), void **arg);
extern "C" void app_alloc_init(void);
extern "C" void app_alloc_cleanup(void);

AppLoader *g_app_loader = NULL;

/* trx->ui_request (T21 spec 8.5): the UI task calls this from UserInterface::run_once with the
 * overlay's interface. The app's callback runs here with a valid ui table; the browser underneath
 * is drawn again when it returns. */
static bool open_request_waiting(void) { return app_ui_request_pending(); }

typedef struct
{
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} open_request_task_t;

static void open_request_task(void *pv)
{
    open_request_task_t *r = (open_request_task_t *)pv;
    r->fn(r->arg);
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

// The UI task's own stack is a few KB: the app's callback runs in a task like an app's, with the
// same stack, and the UI task waits for it as run_app waits for app_main.
static void open_request_run(UserInterface *ui)
{
    open_request_task_t r = { NULL, NULL, NULL };
    if (!app_ui_request_take(&r.fn, &r.arg) || !r.fn) {
        return;
    }
    r.done = xSemaphoreCreateBinary();
    if (!r.done) {
        return;
    }
    app_api_refresh();
    app_api_set_ui(ui);
    if (xTaskCreate(open_request_task, "app_ui", configMINIMAL_STACK_SIZE * 16, &r, tskIDLE_PRIORITY + 2, NULL) ==
        pdPASS) {
        xSemaphoreTake(r.done, portMAX_DELAY);
    }
    app_api_overlay_restore(); /* a wide screen the callback left open */
    app_api_set_ui(NULL);
    vSemaphoreDelete(r.done);
    ui->appear();
}

extern "C" bool app_ui_overlay_ready(void) { return UserInterface::open_request_ui() != NULL; }

/* ultimate.cc: the overlay's interface is the one that answers a ui_request */
extern "C" void app_loader_bind_overlay(UserInterface *ui)
{
    UserInterface::set_open_request_hooks(ui, open_request_waiting, open_request_run);
}

AppLoader::AppLoader() : SubSystem(SUBSYSID_APP_LOADER)
{
    app_alloc_init();
    printf("AppLoader: constructed (ABI %d.%d)\n", U2P_ABI_MAJOR, U2P_ABI_MINOR);
}

AppLoader::~AppLoader() {}

/* Internal: task entry                                                */

typedef struct
{
    uint32_t entry_addr;
    u2p_app_context_t ctx;
    SemaphoreHandle_t done_sem;
    int exit_code;
} app_task_params_t;

static void app_task_entry(void *pv)
{
    app_task_params_t *p = (app_task_params_t *)pv;

    typedef int (*app_entry_fn)(const u2p_api_t *, const u2p_app_context_t *);
    app_entry_fn entry = (app_entry_fn)p->entry_addr;

    printf("AppLoader: jumping to app entry at 0x%08x\n", (unsigned int)p->entry_addr);
    p->exit_code = entry(&g_app_api, &p->ctx);
    printf("AppLoader: app returned %d (signalling done)\n", p->exit_code);

    xSemaphoreGive(p->done_sem);
    printf("AppLoader: app task exiting\n");
    vTaskDelete(NULL); /* self-terminate */
}

/* Validation and loading                                              */

static SubsysResultCode_e reject(UserInterface *ui, const char *msg, SubsysResultCode_e code)
{
    printf("AppLoader: %s\n", msg);
    if (ui) {
        ui->popup(msg, BUTTON_OK);
    }
    return code;
}

// Validates the open file in the order of doc/AppLoader_Spec.md section 1.3 and loads the variant for this CPU
// into the app area. Every failure shows a popup. 'manifest' receives the manifest; its id stays empty when the
// app has none.
static SubsysResultCode_e load_app(File *f, UserInterface *ui, uint32_t &entry_addr, AppManifest &manifest)
{
    memset(&manifest, 0, sizeof(manifest));
    uint32_t file_size = f->get_size32();
    uint32_t n_read = 0;

    // 1. magic, header size
    u2p_app_header_t hdr;
    FRESULT fres = f->read(&hdr, sizeof(hdr), &n_read);
    if ((fres != FR_OK) || (n_read != sizeof(hdr)) || (hdr.magic != U2A_MAGIC) ||
        (hdr.header_size != U2A_HEADER_SIZE)) {
        return reject(ui, "This is not a valid app file.", SSRET_ERROR_IN_FILE_FORMAT);
    }

    // 2. format version
    if (hdr.format_version != U2A_FORMAT_VERSION) {
        return reject(ui, "Old app format. Please rebuild the app.", SSRET_ERROR_IN_FILE_FORMAT);
    }

    // 3. ABI major version: the app has to know this one
    if (hdr.api_version_min > U2P_ABI_MAJOR) {
        return reject(ui, "This app needs a newer firmware.", SSRET_NOT_IMPLEMENTED);
    }
    if (hdr.api_version_max < U2P_ABI_MAJOR) {
        return reject(ui, "This app was built for an older firmware.\nPlease rebuild the app.", SSRET_NOT_IMPLEMENTED);
    }

    // 4. a variant for this CPU
    if ((hdr.variant_count < 1) || (hdr.variant_count > U2A_MAX_VARIANTS) ||
        (hdr.variant_table_offset != U2A_HEADER_SIZE) ||
        (hdr.variant_table_offset + hdr.variant_count * U2A_VARIANT_SIZE > file_size)) {
        return reject(ui, "This is not a valid app file.", SSRET_ERROR_IN_FILE_FORMAT);
    }
    u2p_app_variant_t variants[U2A_MAX_VARIANTS];
    uint32_t table_size = hdr.variant_count * U2A_VARIANT_SIZE;
    fres = f->seek(hdr.variant_table_offset);
    if (fres == FR_OK) {
        fres = f->read(variants, table_size, &n_read);
    }
    if ((fres != FR_OK) || (n_read != table_size)) {
        return reject(ui, "Cannot read the app file.", SSRET_FILE_READ_FAILED);
    }
    const u2p_app_variant_t *v = NULL;
    for (int i = 0; i < hdr.variant_count; i++) {
        if (variants[i].arch == U2A_ARCH_NATIVE) {
            v = &variants[i];
            break;
        }
    }
    if (!v) {
        return reject(ui, "This app has no build for this device.", SSRET_NOT_IMPLEMENTED);
    }

    // 5. load address, size, payload inside the file
    if (v->load_addr != U2A_LOAD_ADDR) {
        return reject(ui, "Unsupported app load address.", SSRET_ERROR_IN_FILE_FORMAT);
    }
    if ((v->payload_size > U2A_AREA_SIZE) || (v->bss_size > U2A_AREA_SIZE - v->payload_size)) {
        return reject(ui, "The app is too large.", SSRET_ROM_IMAGE_TOO_LARGE);
    }
    if ((v->payload_offset > file_size) || (v->payload_size > file_size - v->payload_offset) ||
        (v->entry_offset >= v->payload_size)) {
        return reject(ui, "The app file is damaged.", SSRET_ERROR_IN_FILE_FORMAT);
    }

    // 6. manifest
    if (hdr.manifest_size > 0) {
        if ((hdr.manifest_size > U2A_MANIFEST_MAX_SIZE) || (hdr.manifest_offset < U2A_HEADER_SIZE) ||
            (hdr.manifest_offset > file_size) || (hdr.manifest_size > file_size - hdr.manifest_offset)) {
            return reject(ui, "Invalid app manifest.", SSRET_ERROR_IN_FILE_FORMAT);
        }
        char *text = new char[hdr.manifest_size];
        const char *error = "cannot read";
        fres = f->seek(hdr.manifest_offset);
        if (fres == FR_OK) {
            fres = f->read(text, hdr.manifest_size, &n_read);
        }
        if ((fres == FR_OK) && (n_read == hdr.manifest_size)) {
            error = app_manifest_parse(text, hdr.manifest_size, manifest);
        }
        if (!error) {
            printf("AppLoader: manifest id=%s version=%s, %d file type(s), %d task(s)\n", manifest.id, manifest.version,
                   manifest.num_filetypes, manifest.num_tasks);
        }
        delete[] text;
        if (error) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Invalid app manifest:\n%s", error);
            return reject(ui, msg, SSRET_ERROR_IN_FILE_FORMAT);
        }
    }

    // Load the payload to the fixed app area and zero the bss behind it. The area lies above the firmware heap
    // and below the cartridge ROM, see app_format.h.
    // The payload is read in blocks smaller than any sector, like dma_load does. FatFS then copies from its own
    // sector buffer; a larger read would let it read whole sectors straight into the app area at an address that
    // follows the file position and need not be word aligned, which the U64 flash drivers do not support.
    const uint32_t read_block = 256;
    uint8_t *dst = (uint8_t *)U2A_LOAD_ADDR;
    uint32_t loaded = 0;
    fres = f->seek(v->payload_offset);
    while ((fres == FR_OK) && (loaded < v->payload_size)) {
        uint32_t chunk = v->payload_size - loaded;
        if (chunk > read_block) {
            chunk = read_block;
        }
        fres = f->read(dst + loaded, chunk, &n_read);
        if ((fres != FR_OK) || (n_read == 0)) {
            break;
        }
        loaded += n_read;
    }
    if ((fres != FR_OK) || (loaded != v->payload_size)) {
        return reject(ui, "Cannot read the app file.", SSRET_FILE_READ_FAILED);
    }
    memset(dst + v->payload_size, 0, v->bss_size);

    entry_addr = U2A_LOAD_ADDR + v->entry_offset;
    return SSRET_OK;
}

// Reads the manifest of the app file at 'path' without loading it and without any popup: the header checks of
// load_app() up to the variant table, the payload of this CPU's variant inside the file, then the manifest.
// Returns NULL on success, otherwise a short reason. An app without a manifest is an error here.
const char *app_read_manifest(const char *path, AppManifest &out)
{
    memset(&out, 0, sizeof(out));
    FileManager *fm = FileManager::getFileManager();
    File *f = NULL;
    if ((fm->fopen(path, FA_READ, &f) != FR_OK) || !f) {
        return "cannot open";
    }
    const char *error = NULL;
    uint32_t file_size = f->get_size32();
    uint32_t n_read = 0;
    u2p_app_header_t hdr;
    u2p_app_variant_t variants[U2A_MAX_VARIANTS];
    if ((f->read(&hdr, sizeof(hdr), &n_read) != FR_OK) || (n_read != sizeof(hdr)) || (hdr.magic != U2A_MAGIC) ||
        (hdr.header_size != U2A_HEADER_SIZE)) {
        error = "not an app file";
    } else if (hdr.format_version != U2A_FORMAT_VERSION) {
        error = "old app format";
    } else if ((hdr.api_version_min > U2P_ABI_MAJOR) || (hdr.api_version_max < U2P_ABI_MAJOR)) {
        error = "ABI mismatch";
    } else if ((hdr.variant_count < 1) || (hdr.variant_count > U2A_MAX_VARIANTS) ||
               (hdr.variant_table_offset != U2A_HEADER_SIZE) ||
               (hdr.variant_table_offset + hdr.variant_count * U2A_VARIANT_SIZE > file_size)) {
        error = "bad variant table";
    } else if ((hdr.manifest_size == 0) || (hdr.manifest_size > U2A_MANIFEST_MAX_SIZE) ||
               (hdr.manifest_offset < U2A_HEADER_SIZE) || (hdr.manifest_offset > file_size) ||
               (hdr.manifest_size > file_size - hdr.manifest_offset)) {
        error = "no valid manifest range";
    }
    if (!error) {
        uint32_t table_size = hdr.variant_count * U2A_VARIANT_SIZE;
        if ((f->seek(hdr.variant_table_offset) != FR_OK) || (f->read(variants, table_size, &n_read) != FR_OK) ||
            (n_read != table_size)) {
            error = "cannot read variants";
        }
    }
    if (!error) {
        const u2p_app_variant_t *v = NULL;
        for (int i = 0; i < hdr.variant_count; i++) {
            if (variants[i].arch == U2A_ARCH_NATIVE) {
                v = &variants[i];
                break;
            }
        }
        // A file that was only partly written fails here, because the payload lies behind the manifest.
        if (!v) {
            error = "no build for this device";
        } else if ((v->payload_offset > file_size) || (v->payload_size > file_size - v->payload_offset) ||
                   (v->payload_size > U2A_AREA_SIZE)) {
            error = "payload outside the file";
        }
    }
    if (!error) {
        char *text = new char[hdr.manifest_size];
        error = "cannot read manifest";
        if ((f->seek(hdr.manifest_offset) == FR_OK) && (f->read(text, hdr.manifest_size, &n_read) == FR_OK) &&
            (n_read == hdr.manifest_size)) {
            error = app_manifest_parse(text, hdr.manifest_size, out);
        }
        delete[] text;
    }
    fm->fclose(f);
    return error;
}

// Pauses or resumes the C64 the way PUT /v1/machine:pause and :resume do.
static bool c64_command(int function)
{
    SubsysCommand *cmd = new SubsysCommand(NULL, SUBSYSID_C64, function, 0);
    return cmd->execute().status == SSRET_OK;
}

/* run_app: load and launch                                            */

// 'params' is NULL for a plain "Run App".
SubsysResultCode_e AppLoader::run_app(SubsysCommand *cmd, AppLaunchParams *params)
{
    if (cmd->path.length() == 0 || cmd->filename.length() == 0) {
        return SSRET_INVALID_PARAMETER;
    }

    /* An app that went on in a task of its own (trx->detach) lives in the app
     * area: another one would overwrite it. A person starting an app from the
     * menu ends it first (up to 3 s) and then starts the one they asked for
     * (T21 7.6); REST (no user interface) is refused. */
    if (app_resident_active() && cmd->user_interface) {
        app_resident_stop(3000);
    }
    if (app_resident_active()) {
        if (cmd->user_interface) {
            cmd->user_interface->popup("An app still runs in the background.\nEnd it first.", BUTTON_OK);
        }
        return SSRET_NO_LOCK;
    }

    /* Apps run only while the C64 is stopped: no bus peeks/pokes from an app
     * into a running machine. A launch from the overlay menu or REST finds
     * the C64 running (there is no freeze to press), so the loader stops it
     * for the app's run and lets it go on afterwards (an app that holds the
     * machine itself, like trxmon, keeps it through its own hold). */
    bool i_stopped_it = false;
    C64 *machine = C64::getMachine();
    if (machine && machine->exists() && !machine->is_stopped()) {
        i_stopped_it = c64_command(MENU_C64_PAUSE);
    }
    if (machine && machine->exists() && !machine->is_stopped()) {
        if (cmd->user_interface) {
            cmd->user_interface->popup("Apps require Freeze mode.\n"
                                       "Freeze the C64 first, then try again.",
                                       BUTTON_OK);
        }
        return SSRET_WRONG_MOUNT_MODE;
    }

    /* Build full path */
    mstring full = cmd->path;
    int plen = full.length();
    if (plen > 0 && full.c_str()[plen - 1] != '/')
        full += "/";
    full += cmd->filename;
    const char *full_path = full.c_str();

    FileManager *fm = FileManager::getFileManager();
    File *f = NULL;
    FRESULT fres = fm->fopen(full_path, FA_READ, &f);
    if (fres != FR_OK || !f) {
        printf("AppLoader: cannot open %s (%d)\n", full_path, fres);
        if (i_stopped_it) {
            c64_command(MENU_C64_RESUME);
        }
        return SSRET_CANNOT_OPEN_FILE;
    }

    uint32_t entry_addr = 0;
    AppManifest *manifest = new AppManifest;
    SubsysResultCode_e result = load_app(f, cmd->user_interface, entry_addr, *manifest);
    fm->fclose(f);

    /* A plain "Run App" registers an app with a manifest before it starts (section 3.1). */
    AppRegistry *registry = AppRegistry::getRegistry();
    if ((result == SSRET_OK) && !params && manifest->id[0] && registry) {
        registry->register_app(cmd->user_interface, full_path, *manifest);
    }
    app_set_current_id((result == SSRET_OK) ? manifest->id : "");
    delete manifest;
    if (result != SSRET_OK) {
        if (i_stopped_it) {
            c64_command(MENU_C64_RESUME);
        }
        return result;
    }
#if defined(RISCV)
    app_icache_sweep();
#endif

    /* Task parameters are shared with the app task, so they live on the heap. */
    app_task_params_t *p = (app_task_params_t *)pvPortMalloc(sizeof(*p));
    if (!p) {
        return SSRET_OUT_OF_MEMORY;
    }
    memset(p, 0, sizeof(*p));
    p->entry_addr = entry_addr;
    p->ctx.size = sizeof(p->ctx);
    p->ctx.app_path = full_path; /* borrowed; valid until mstring dies */
    p->ctx.selected_file = params ? params->selected_file : NULL;
    p->ctx.launch_source = params ? params->launch_source : (uint32_t)U2P_LAUNCH_BROWSER;
    p->ctx.args = params ? params->args : NULL;
    p->ctx.action = params ? params->action : NULL;
    p->ctx.has_ui = (cmd->user_interface != NULL);
    p->ctx.reply = app_api_reply;
    p->done_sem = xSemaphoreCreateBinary();
    p->exit_code = -1;
    if (!p->done_sem) {
        vPortFree(p);
        if (i_stopped_it) {
            c64_command(MENU_C64_RESUME);
        }
        return SSRET_OUT_OF_MEMORY;
    }

    /* Hand the current UI over to the API impl for the duration of the app. */
    app_api_refresh();
    app_api_overlay_restore(); /* an app that starts finds the firmware's screen, never a wide one */
    app_api_set_ui(cmd->user_interface);
    app_api_set_reply(params ? params->reply : NULL, params ? params->reply_size : 0);
    app_api_clear_deferred();

    /* Spawn the app task */
    TaskHandle_t th = NULL;
    BaseType_t ok = xTaskCreate(app_task_entry, "app_task", configMINIMAL_STACK_SIZE * 16, /* ~64 KB */
                                p, tskIDLE_PRIORITY + 2, &th);
    if (ok != pdPASS) {
        app_api_set_ui(NULL);
        app_api_set_reply(NULL, 0);
        vSemaphoreDelete(p->done_sem);
        vPortFree(p);
        if (i_stopped_it) {
            c64_command(MENU_C64_RESUME);
        }
        return SSRET_INTERNAL_ERROR;
    }

    /* Block until the app returns; a running app cannot be cancelled. */
    xSemaphoreTake(p->done_sem, portMAX_DELAY);
    vSemaphoreDelete(p->done_sem);
    int exit_code = p->exit_code;
    vPortFree(p);
    app_api_overlay_restore(); /* the overlay is 40 x 25 again whatever the app did with it */
    if (machine && machine->exists()) {
        machine->menu_back(); /* the freeze menu's view and charset, which the app's run or land may have changed */
    }
    app_api_set_ui(NULL);
    app_api_set_reply(NULL, 0);
    if (params) {
        params->exit_code = exit_code;
    }

    /* Auto-free any allocations the app forgot to release, close its sockets and
     * take the TRX64 debug block back from it, unless the app left a task
     * running (trx->detach): that task does it when it ends. */
    if (!app_resident_active()) {
        app_alloc_cleanup();
        app_net_cleanup();
        app_trx_cleanup();
    }

    /* A program, cartridge or reset the app asked for starts now. It takes
     * the machine away from the menu, which is then left alone. */
    bool handed_over = app_api_run_deferred();

    /* ui->close_menu: the menu the app was started from closes (MENU_HIDE goes up through the
     * browser's context menu), the C64 runs on. Taken whatever happens, so it never outlives
     * the run. */
    bool close_menu = app_api_take_close_menu();

    /* The app drew over our UI (popups, custom screens). Re-render the
     * UserInterface object stack so the File Browser title and body
     * come back. */
    if (!handed_over && cmd->user_interface) {
        if (close_menu) {
            cmd->user_interface->menu_response_to_action = MENU_HIDE;
        } else {
            cmd->user_interface->appear();
        }
    }
    if (!handed_over && i_stopped_it) {
        c64_command(MENU_C64_RESUME);
    }

    printf("AppLoader: app exit %d\n", exit_code);
    return SSRET_OK;
}

SubsysResultCode_e AppLoader::executeCommand(SubsysCommand *cmd)
{
    switch (cmd->functionID) {
    case APP_CMD_RUN:
        return run_app(cmd, NULL);
    case APP_CMD_RUN_ACTION:
        if (!cmd->buffer || (cmd->bufferSize != sizeof(AppLaunchParams))) {
            return SSRET_INVALID_PARAMETER;
        }
        return run_app(cmd, (AppLaunchParams *)cmd->buffer);
    default:
        return SSRET_UNDEFINED_COMMAND;
    }
}

/* Boot-time construction                                              */

static void initAppLoader(void *obj, void *param)
{
    (void)obj;
    (void)param;
    if (!g_app_loader) {
        g_app_loader = new AppLoader();
    }
}

InitFunction appLoader_initializer("App Loader", initAppLoader, NULL, NULL);
