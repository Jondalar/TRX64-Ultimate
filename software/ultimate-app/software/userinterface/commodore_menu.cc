extern "C" {
    #include "small_printf.h"
}
#include "userinterface.h"
#include <stdlib.h>
#include <string.h>
#include "mystring.h"
#include "commodore_menu.h"
#include "browsable_root.h"
#include "config_menu.h"
#include "config_collection.h"
#include "tree_browser.h"
#include "assembly_search.h"
#include "system_info.h"
#include "task_menu.h"
#include "stream_textlog.h"

extern StreamTextLog textLog; // the global log
int swap_joystick();          // tree_browser.cc, u64_config.cc

// The destinations the menu offers. Some are config groups, which gather items
// from several subsystems under one heading, and some are whole config pages,
// which is why there are two handlers rather than one: the names live in the
// same table because an entry does not care which of the two it turns out to
// be, and BrowsableConfigRootPredefined looks for a group before a page.
typedef enum {
    e_audio_mixer = 0,
    e_sid_sockets,
    e_ultisid,
    e_c64_carts,
    e_clock,
    e_network,
    e_ethernet,
    e_wifi,
    e_led_strip,
    e_streams,
    e_softiec,
    e_printer,
    e_modem,
    e_user_interface,
    e_tape,
    e_drive_a,
    e_drive_b,
    e_memory,
    e_video,
    e_audio,
    e_leds,
    e_turbo,
    e_joystick,
} config_menus_t;

static const char *config_menu_names[] = {
    "Audio Mixer",
    "SID Sockets Configuration",
    "UltiSID Configuration",
    "C64 and Cartridge Settings",
    "Clock Settings",
    "Network Settings",
    "Ethernet Settings",
    "WiFi Settings",
    "LED Strip Settings",
    "Data Streams",
    "SoftIEC Drive Settings",
    "Printer Settings",
    "Modem Settings",
    "User Interface Settings",
    "Tape Settings",
    "Drive A Settings",
    "Drive B Settings",
    "Memory Configuration",
    "Video Configuration",
    "Audio Configuration",
    GROUP_NAME_LEDS,
    "Turbo Settings",
    "Joystick Settings",
};

CommodoreMenu :: CommodoreMenu(UserInterface *ui) : ContextMenu(ui, NULL, 0, 0, MENU_HIDE, 0)
{
    appendAction(new Action("DISK FILE BROWSER", S_file_browser, 0));
    appendAction(new Action("COMMOSERVE FILE SEARCH", S_assembly64, 0));
    appendAction(new Action("MEMORY & ROMS", S_cfg_group, e_memory));
    appendAction(new Action("TURBO BOOST", S_cfg_group, e_turbo));
    appendAction(new Action("VIDEO SETUP", S_cfg_group, e_video));
    appendAction(new Action("AUDIO SETUP", S_cfg_audio, 0));
    appendAction(new Action("JOYSTICK & CONTROLLERS", S_cfg_group, e_joystick));
    appendAction(new Action("LED LIGHTING", S_cfg_group, e_leds));
    appendAction(new Action("NETWORK SERVICES & TIMEZONE", S_cfg_page, e_network));
    appendAction(new Action("WIRED NETWORK SETUP", S_cfg_page, e_ethernet));
    appendAction(new Action("WI-FI NETWORK SETUP", S_cfg_page, e_wifi));
    appendAction(new Action("MODEMS", S_cfg_page, e_modem));
    appendAction(new Action("PRINTERS", S_cfg_page, e_printer));
    appendAction(new Action("USER INTERFACE", S_cfg_page, e_user_interface));
    appendAction(new Action("BUILT-IN DRIVE A", S_cfg_page, e_drive_a));
    appendAction(new Action("BUILT-IN DRIVE B", S_cfg_page, e_drive_b));
    appendAction(new Action("ADVANCED SETTINGS", S_advanced, 0));
    appendAction(new Action("SYSTEM INFORMATION", S_sysinfo, 0));

    // The file browser this menu opens, kept rather than made on demand: it is
    // the same browser each time, so leaving it and coming back finds the
    // directory where it was left.
    Browsable *root = new BrowsableRoot();
    root_tree_browser = new TreeBrowser(ui, root);
    root_tree_browser->allow_exit = true;
}

CommodoreMenu :: ~CommodoreMenu()
{
    if (root_tree_browser) {
        delete root_tree_browser;
        root_tree_browser = NULL;
    }
}

void CommodoreMenu :: init() // call on root!
{
    this->screen = get_ui()->get_screen();
    this->keyb = get_ui()->get_keyboard();
    // Forty columns in the middle of whatever screen this is, from the row
    // below the header to the row above the key hint.
    window = new Window(screen, (screen->get_size_x() - 40) >> 1, 2, 40, screen->get_size_y() - 3);
    window->draw_border();
}

int CommodoreMenu :: poll(int prev)
{
    mstring *msg = this->user_interface->getMessage();
    if (msg) {
        user_interface->popup(msg->c_str(), BUTTON_OK);
        delete msg;
    }

    int ret = 0;
    if (subContext) {
        if (prev < 0) {
            delete subContext;
            subContext = NULL;
            draw();
        } else if (prev > 0) {
            draw();
            ret = subContext->executeSelected("");
            delete subContext;
            subContext = NULL;
            if (user_interface->has_focus(this)) {
                draw();
            }
        }
        return ret;
    }

    return ContextMenu::poll(prev);
}

int CommodoreMenu :: handle_key(int c)
{
    switch (c) {
    case KEY_TASKS:
        subContext = new TaskMenu(user_interface, NULL, NULL);
        subContext->init(window, keyb);
        user_interface->activate_uiobject(subContext);
        return 0;
    case KEY_CONFIG:
        ConfigBrowser::start(user_interface);
        return 0;
    case KEY_SYSINFO:
        SystemInfo::generate(user_interface);
        draw();
        return 0;
    case KEY_SEARCH:
        AssemblyInGui::S_OpenSearch(user_interface);
        return 0;
    // The browser's keys, here too (SPEC-03 R3): this menu is where the
    // Commodore interface opens, so it is where they are looked for.
    case KEY_CTRL_J:
        return swap_joystick();
    case KEY_CTRL_L:
        user_interface->run_editor(textLog.getText(), textLog.getLength());
        draw();
        return 0;
#ifndef RECOVERYAPP
    case KEY_HOME: {
        // Into the file browser, in the home directory when one is set
        S_file_browser(NULL, this);
        const char *home = user_interface->cfg->get_string(CFG_USERIF_HOME_DIR);
        if (home && *home) {
            root_tree_browser->cd(home);
        }
        return 0;
    }
#endif
    }
    return ContextMenu::handle_key(c);
}

void CommodoreMenu :: redraw()
{
    draw();
    screen->move_cursor(0, screen->get_size_y() - 1);
    if (user_interface->navmode == 0) {
        screen->output("\e1 CRSR+TYPE to NAV F3/F5=PGUP/DN F7=HELP");
    } else {
        screen->output("\e1 WASD=NAV F1=MENU F3/F5=PGUP/DN F7=HELP");
    }
    context_state = e_active;
}

int CommodoreMenu :: select_item(void)
{
    // Unlike an ordinary context menu, an entry here does not hand a chosen
    // action back to whoever opened the menu: it opens what it names, and the
    // menu stays where it is.
    selectedAction = actions[item_index];
    if (selectedAction && selectedAction->direct_func) {
        selectedAction->direct_func(selectedAction, this);
    }
    return 0;
}

void CommodoreMenu :: create_task_items(void)
{
    // One entry in the task menu to come back here with, registered once
    // however many menus exist: the overlay and the machine's own screen each
    // have one, and both send the same key.
    static bool done = false;
    if (!done) {
        TaskCategory *cat = TasksCollection::getCategory("Return to Main Menu", 9999);
        cat->append(new Action("Return", S_SendMenuKey, 0, 0));
        done = true;
    }
}

SubsysResultCode_e CommodoreMenu :: S_file_browser(Action *act, void *context)
{
    CommodoreMenu *menu = (CommodoreMenu *)context;
    menu->root_tree_browser->init();
    menu->user_interface->activate_uiobject(menu->root_tree_browser);
    return SSRET_OK;
}

// A ConfigBrowser does not own its root, and each entry below made a new one
// that was never freed: some 580 bytes per page opened (SPEC-06 R2). The last
// one is kept and freed when the next is made. By then its browser is gone:
// the next page is opened from this menu, which the browser had to be left for.
static Browsable *s_last_config_page;

static Browsable *replace_config_page(Browsable *page)
{
    delete s_last_config_page;
    s_last_config_page = page;
    return page;
}

SubsysResultCode_e CommodoreMenu :: S_cfg_page(Action *act, void *context)
{
    // Straight into one page, rather than into the list of all of them.
    CommodoreMenu *menu = (CommodoreMenu *)context;
    ConfigStore *store = ConfigManager::getConfigManager()->find_store(config_menu_names[act->function]);
    if (store) {
        Browsable *configPage = replace_config_page(new BrowsableConfigStore(store));
        ConfigBrowser *configBrowser = new ConfigBrowser(menu->user_interface, configPage, 1);
        configBrowser->init();
        menu->user_interface->activate_uiobject(configBrowser);
    }
    return SSRET_OK;
}

SubsysResultCode_e CommodoreMenu :: S_cfg_group(Action *act, void *context)
{
    CommodoreMenu *menu = (CommodoreMenu *)context;
    ConfigGroup *group = ConfigGroupCollection::getGroup(config_menu_names[act->function], -1);
    if (group) {
        Browsable *configPage = replace_config_page(new BrowsableConfigGroup(group));
        ConfigBrowser *configBrowser = new ConfigBrowser(menu->user_interface, configPage, 1);
        configBrowser->init();
        menu->user_interface->activate_uiobject(configBrowser);
    }
    return SSRET_OK;
}

SubsysResultCode_e CommodoreMenu :: S_cfg_audio(Action *act, void *context)
{
    // Everything that makes a sound, gathered under one entry.
    CommodoreMenu *menu = (CommodoreMenu *)context;
    static const char *names[] = { "Audio Mixer", "Speaker Mixer", "SID Sockets Configuration",
                                   "UltiSID Configuration", "SID Addressing", "SID Player Behavior" };
    static BrowsableConfigRootPredefined br(6, names);

    ConfigBrowser *configBrowser = new ConfigBrowser(menu->user_interface, &br);
    configBrowser->init();
    menu->user_interface->activate_uiobject(configBrowser);
    return SSRET_OK;
}

SubsysResultCode_e CommodoreMenu :: S_advanced(Action *act, void *context)
{
    // The whole tree, for what the entries above do not reach.
    CommodoreMenu *menu = (CommodoreMenu *)context;
    // One tree for the life of the menu, as ConfigBrowser::start keeps its own
    static BrowsableConfigRoot configRoot;
    Browsable *configRoot_p = &configRoot;
    ConfigBrowser *configBrowser = new ConfigBrowser(menu->user_interface, configRoot_p);
    configBrowser->init();
    menu->user_interface->activate_uiobject(configBrowser);
    return SSRET_OK;
}

SubsysResultCode_e CommodoreMenu :: S_assembly64(Action *act, void *context)
{
    CommodoreMenu *menu = (CommodoreMenu *)context;
    AssemblyInGui::S_OpenSearch(menu->user_interface);
    return SSRET_OK;
}

SubsysResultCode_e CommodoreMenu :: S_sysinfo(Action *act, void *context)
{
    CommodoreMenu *menu = (CommodoreMenu *)context;
    SystemInfo::generate(menu->user_interface);
    menu->draw();
    return SSRET_OK;
}
