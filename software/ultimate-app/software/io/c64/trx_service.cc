/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * trx_service.cc — the TRX64 debug service (U5).
 *
 * What used to be the app trxmon.u2a runs here as a task of the firmware. The
 * code is the same (apps/trxmon/src, the sources the host tests build); what
 * changes is who runs it and what it is handed:
 *
 *   - a net table whose sockets are not in the app loader's list, so no app
 *     cleanup closes the RPC port when something else returns,
 *   - an acquire that does not set the loader's s_owned, so api_trx_release()
 *     never undoes the service's arming behind its back (U5 D4),
 *   - everything else straight out of g_app_api.
 *
 * The buffers of these sources do not sit in the firmware's bss: linker.x puts
 * them in the DDR2 window 0x03800000..0x03A00000 (U5 D2, agreed with MultiCore
 * in docs/contracts/trx64u-firmware-handover.md §6). Nothing zeroes that
 * window for us — after a MultiCore boot it holds the remains of a partial
 * bitstream — so the service does it itself before it touches anything.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "app_api.h"
#include "menu.h"
#include "tasks_collection.h"
#include "action.h"
#include "subsys.h"
#include "userinterface.h"
#include "trx_service.h"
#include "trxsvc.h"
#include "trx_io.h"

extern "C" {
#include "small_printf.h"
}

/* app_api_impl.cc / app_api_net.cc */
extern "C" u2p_api_t g_app_api;
extern "C" const u2p_net_api_t g_svc_net;
extern "C" int svc_trx_acquire(uint32_t flags);
extern "C" void app_trx_service_up(void);
extern "C" void app_api_refresh(void);
extern "C" void app_api_set_ui(UserInterface *ui);
extern "C" void app_api_overlay_restore(void);

/* linker.x: the service's bss, the whole window it lives in */
extern "C" uint8_t __trxsvc_start[];
extern "C" uint8_t __trxsvc_end[];

#define TRX_CAPS_DBG 0x08 /* page 3: the debug block (trx_dbg.v, drv_dbg.v) */

static u2p_api_t s_api;
static u2p_trx_api_t s_trx;
static bool s_up;

extern "C" bool trx_service_present(void) { return s_up; }

static void service_task(void *pv)
{
    (void)pv;
    trxsvc_listen(); /* may fail here: the loop keeps trying */
    trxsvc_serve();  /* never returns */
}

extern "C" void trx_service_start(void)
{
    if (s_up) {
        return;
    }
    /* The trx table of the app API is built on demand (app_api_refresh), and at
     * boot no app has run yet: without this call g_app_api.trx is the NULL of
     * its initialiser and the service would never start. */
    app_api_refresh();
    if (!(trx_io_caps() & TRX_CAPS_DBG)) {
        printf("[TRX] no debug block: the monitor service does not start\n");
        return;
    }
    if (!g_app_api.trx) {
        printf("[TRX] the app API has no trx table: the monitor service does not start\n");
        return;
    }

    /* the window first: it is not in .bss, so crt0 never cleared it */
    memset(__trxsvc_start, 0, (size_t)(__trxsvc_end - __trxsvc_start));

    s_trx = *g_app_api.trx;
    s_trx.acquire = svc_trx_acquire;
    s_api = g_app_api;
    s_api.trx = &s_trx;
    s_api.net = &g_svc_net;

    if (trxsvc_init(&s_api) < 0) {
        printf("[TRX] the monitor service refused the table: no debug link or no net\n");
        return;
    }
    app_trx_service_up();

    /* Below the network service tasks, as a detached app's task is: the service
     * only runs when they have nothing to do. */
    if (xTaskCreate(service_task, "trxsvc", configMINIMAL_STACK_SIZE * 16, NULL, tskIDLE_PRIORITY, NULL) != pdPASS) {
        printf("[TRX] the monitor service task could not be created\n");
        return;
    }
    s_up = true;
    printf("[TRX] monitor service up: it owns the debug block and watches the stops\n");
}

/* ---- the screen ----------------------------------------------------------- */

/* The UI task's own stack is a few KB; the monitor's screen runs in a task of
 * its own and the UI task waits for it, the way app_loader.cc runs an app and
 * the callback of a trx->ui_request. */
typedef struct
{
    SemaphoreHandle_t done;
    int land;
} screen_req_t;

static void screen_task(void *pv)
{
    screen_req_t *r = (screen_req_t *)pv;
    if (r->land >= 0) {
        const char *e = trxsvc_land(r->land);
        if (e) {
            printf("[TRX] the monitor cannot stand on ring entry %d: %s\n", r->land, e);
        }
    }
    trxsvc_screen();
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void open_screen(UserInterface *ui, int land)
{
    if (!s_up || !ui) {
        return;
    }
    screen_req_t r;
    r.land = land;
    r.done = xSemaphoreCreateBinary();
    if (!r.done) {
        return;
    }
    app_api_refresh();
    app_api_set_ui(ui);
    if (xTaskCreate(screen_task, "trxmon_ui", configMINIMAL_STACK_SIZE * 16, &r, tskIDLE_PRIORITY + 2, NULL) ==
        pdPASS) {
        xSemaphoreTake(r.done, portMAX_DELAY);
    }
    app_api_overlay_restore(); /* a wide screen the session left open */
    app_api_set_ui(NULL);
    vSemaphoreDelete(r.done);
}

extern "C" void trx_service_open(UserInterface *ui) { open_screen(ui, -1); }

extern "C" void trx_service_open_at(UserInterface *ui, int ring_index) { open_screen(ui, ring_index); }

/* ---- the menu entry -------------------------------------------------------- */
/* Developer > TRX64 Monitor, where the app's manifest put it until U5. The key
 * in the file browser (C= + O) and Timewarp's C= + O open the same screen. */

static SubsysResultCode_e monitor_action(SubsysCommand *cmd)
{
    if (!s_up) {
        cmd->user_interface->popup("The monitor service is not running.\nThe boot log says why.", BUTTON_OK);
        return SSRET_OK;
    }
    trx_service_open(cmd->user_interface);
    cmd->user_interface->menu_response_to_action = MENU_HIDE;
    return SSRET_OK;
}

class TrxServiceMenu : public ObjectWithMenu
{
public:
    void create_task_items(void)
    {
        if (!(trx_io_caps() & TRX_CAPS_DBG)) {
            return;
        }
        TaskCategory *cat = TasksCollection::getCategory("Developer", SORT_ORDER_DEVELOPER);
        cat->append(new Action("TRX64 Monitor", monitor_action, 0, 0));
    }
};

static TrxServiceMenu trx_service_menu;
