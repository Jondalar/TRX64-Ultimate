#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "itu.h"
#include "syslog.h"
#include "c64.h"
#include "c64_subsys.h"
#include "screen.h"
#include "keyboard.h"
#include "userinterface.h"
#include "tree_browser.h"
#include "ui_mode.h"
#if U64
#include "commodore_menu.h"
#endif
#include "browsable_root.h"
#include "filemanager.h"
#include "config.h"
#include "path.h"
#include "rtc.h"
#include "stream.h"
#include "host_stream.h"
#include "ui_stream.h"
#include "stream_menu.h"
#include "overlay.h"
#include "init_function.h"
#include "stream_uart.h"
#include "stream_textlog.h"
#include "dump_hex.h"
#include "usb_base.h"
#include "home_directory.h"
#include "u2p.h"
#include "u64.h"
#include "keyboard_usb.h"
#include "i2c_drv.h"
#include "product.h"

#if U64
    #include "u64_config.h"
#endif
bool connectedToU64 = false;

static TreeBrowser *root_tree_browser;
static TreeBrowser *root_tree_browser_overlay;
#if U64
static CommodoreMenu *commodore_menu;
static CommodoreMenu *commodore_menu_overlay;
#endif

StreamMenu *root_menu;
Overlay *overlay;
C64 *c64;
C64_Subsys *c64_subsys;
StreamTextLog textLog(96*1024);
Syslog syslog;

extern "C" bool push_active_menu_button(void)
{
    if (overlay && overlay->is_accessible()) {
        overlay->setButtonPushed();
        return true;
    }
    return false;
}

#if U64
extern "C" void app_loader_bind_overlay(UserInterface *ui); // app_loader.cc
#endif
/* U5: the debug service is compiled into the C64 Ultimate II firmware alone
   (target/u64ii/riscv/ultimate/Makefile): it is the trxmon sources plus its own
   DDR2 window, and no other target has the debug block. Taken weakly, as
   tree_browser.cc and trx_timewarp_ui.cc take the other three entries of
   trx_service.h, so that every target still links. */
extern "C" void trx_service_start(void) __attribute__ ((weak));
extern "C" void (*custom_outbyte)(int c);

extern "C" {
	void print_tasks(void);
}

void outbyte_log(int c)
{
	textLog.charout(c);
}

void outbyte_log_syslog(int c)
{
	textLog.charout(c);  // Internal log
	syslog.charout(c);  // Remote syslog server
}

extern "C" void ultimate_main(void *a)
{
    // Normal boot log size is about 5k right now, so 16k should be enough to
    // give time to flush the buffer.
    const int syslog_bufsize = 16*1024;

    char time_buffer[32];

    uint32_t capabilities = getFpgaCapabilities();

    char product_version[64];
    printf("*** %s ***\n", getProductVersionString(product_version, sizeof(product_version), true));
    printf("*** FPGA Capabilities: %8x ***\n\n", capabilities);

	puts("Executing init functions.");
	InitFunction :: executeAll();
    custom_outbyte = syslog.init(syslog_bufsize) ? outbyte_log_syslog : outbyte_log;
   
    printf("%s ", rtc.get_long_date(time_buffer, 32));
    printf("%s\n", rtc.get_time_string(time_buffer, 32));

	if (capabilities & CAPAB_CARTRIDGE) {
		c64 = C64 :: getMachine();
		c64_subsys = new C64_Subsys(c64);
		c64->init();
		c64->start();
	} else {
		c64 = NULL;
	}

    usb2.initHardware();

    char title[48];
    getProductTitleString(title, sizeof(title), false);

    if(capabilities & CAPAB_ULTIMATE64) {
        system_usb_keyboard.setMatrix((volatile uint8_t *)MATRIX_KEYB);
        system_usb_keyboard.enableMatrix(true);
    }

    overlay = NULL;
    UserInterface *overlayUserInterface = NULL;

#if U64 == 2
    overlay_settings_t overlay_default;
    u64_configurator->getOverlaySettings(overlay_default);
    overlay = new Overlay(false, 12, U64II_OVERLAY_BASE, overlay_default);
    Keyboard_C64 *kb = new Keyboard_C64(overlay, &U64II_KEYB_ROW, &U64II_KEYB_COL, &U64II_KEYB_JOY);
    overlay->setKeyboard(kb);
#elif U64 == 1
    overlay_settings_t overlay_default = { 40, 25, 366, 274,  8, OVERLAY_CHARHEIGHT_9 };
    overlay = new Overlay(false, 11, U64_OVERLAY_BASE, overlay_default);
    Keyboard_C64 *kb = new Keyboard_C64(overlay, C64_PLD_PORTB, C64_PLD_PORTA, C64_PLD_PORTA);
    overlay->setKeyboard(kb);
#endif

#if U64
    overlayUserInterface = new UserInterface(title, true);
    app_loader_bind_overlay(overlayUserInterface);
    // What the menu button opens: the Commodore interface puts its own menu
    // there, the Ultimate one the file browser.
    if (ui_is_commodore()) {
        commodore_menu_overlay = new CommodoreMenu(overlayUserInterface);
        overlayUserInterface->activate_uiobject(commodore_menu_overlay); // root of all evil!
        overlayUserInterface->init(overlay);
    } else {
        Browsable *root = new BrowsableRoot();
        root_tree_browser_overlay = new TreeBrowser(overlayUserInterface, root);
        overlayUserInterface->activate_uiobject(root_tree_browser_overlay); // root of all evil!
        overlayUserInterface->init(overlay);
        if(overlayUserInterface->cfg->get_value(CFG_USERIF_START_HOME)) {
            new HomeDirectory(overlayUserInterface, root_tree_browser_overlay);
            // will clean itself up
        }
    }
#endif

    UserInterface *c64UserInterface = NULL;
    if(c64) {
        c64UserInterface = new UserInterface(title, true);
        // The same choice again, for the menu on the machine's own screen.
#if U64
        if (ui_is_commodore()) {
            commodore_menu = new CommodoreMenu(c64UserInterface);
            c64UserInterface->activate_uiobject(commodore_menu); // root of all evil!
            c64UserInterface->init(c64);
        } else
#endif
        {
            Browsable *root = new BrowsableRoot();
            root_tree_browser = new TreeBrowser(c64UserInterface, root);
            c64UserInterface->activate_uiobject(root_tree_browser); // root of all evil!
            c64UserInterface->init(c64);
            if(c64UserInterface->cfg->get_value(CFG_USERIF_START_HOME)) {
                new HomeDirectory(c64UserInterface, root_tree_browser);
                // will clean itself up
            }
        }
    }

    // First start beside a .cor (docs/contracts/trx64u-firmware-handover.md, T148 D6): the machine came
    // up with the settings from the flash, and the user decides once whether this core keeps them. The
    // flash is only read, never written, while a MultiCore host owns it. The answer creates the .cfg
    // either way, so the question is asked once and never again.
    if (ConfigManager :: import_question_open()) {
        UserInterface *ui = c64UserInterface ? c64UserInterface : overlayUserInterface;
        if (ui) {
            int yes = ui->popup("MultiCore host found.\n"
                                "\n"
                                "This core keeps its own settings\n"
                                "in its folder beside the .cor file,\n"
                                "so the flash stays untouched.\n"
                                "\n"
                                "Import the settings from /Flash\n"
                                "and use them on this core?", BUTTON_YES | BUTTON_NO);
            ConfigManager :: getConfigManager()->save_core_cfg(yes != BUTTON_YES);
            if (yes != BUTTON_YES) {
                ui->popup("This core now starts from the\n"
                          "factory settings. What is set at\n"
                          "boot time needs a restart.", BUTTON_OK);
            }
        }
    }

    /* U5: the debug service. From here the monitor, its RPC port and the
       breakpoints belong to the firmware, not to an app that has to stay
       loaded (docs/specs/U5-observer-service.md). Absent on a target that does
       not compile it in, hence the test. */
    if (trx_service_start) {
        trx_service_start();
    }

    printf("All linked modules have been initialized and are now running.\n");
    print_tasks();

    while(c64) {
        int doIt = 0;
        c64->checkButton();
        if (c64->buttonPush()) {
            doIt = 1;
        }
        switch(system_usb_keyboard.getch()) {
        case KEY_SCRLOCK:
        case KEY_F10:
            doIt = 1;
            break;
        case 0x04: // CTRL-D
            doIt = 2;
            break;
        }
        // The debug service (or an app) asked for the overlay: a breakpoint fired. This loop is
        // the queue: it is not here while a person is in a menu or an app.
        if (!doIt && overlayUserInterface && UserInterface::open_request_waiting()) {
            doIt = 3;
        }
        UserInterface *ui = c64UserInterface;
        if (overlayUserInterface) {
            if ((U64_HDMI_REG & U64_HDMI_HPD_CURRENT) && (doIt)) {
                if (overlayUserInterface->getPreferredType() == 1) {
                    ui = overlayUserInterface;
                }
            }
        }

        switch (doIt) {
        case 0:
            c64UserInterface->pollInactive();
            if (overlayUserInterface) {
                overlayUserInterface->pollInactive();
            }
            break;
        case 1:
            system_usb_keyboard.enableMatrix(false);
            ui->run_once();
            system_usb_keyboard.enableMatrix(true);
            break;
        case 2:
            ui->swapDisk();
            break;
        case 3:
            system_usb_keyboard.enableMatrix(false);
            overlayUserInterface->run_once();
            system_usb_keyboard.enableMatrix(true);
            break;
        }
        vTaskDelay(3);
    }

    custom_outbyte = 0; // stop logging
    printf("GUI running on C64 host has terminated? This should not happen.\n");

    vTaskDelay(400);
    puts(textLog.getText());

    vTaskSuspend(NULL); // Stop executing init task

    // We will never come here.

    if(overlay)
        delete overlay;
    if(root_tree_browser)
        delete root_tree_browser;
    if(root_tree_browser_overlay)
        delete root_tree_browser_overlay;
    if(overlayUserInterface)
        delete overlayUserInterface;
    if(c64UserInterface)
        delete c64UserInterface;
    if(c64_subsys)
    	delete c64_subsys;
    if(c64)
        delete c64;
    
    printf("Graceful exit!!\n");
//    return 0;
}
