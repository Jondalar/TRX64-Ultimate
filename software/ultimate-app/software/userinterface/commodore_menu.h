#ifndef COMMODORE_MENU_H
#define COMMODORE_MENU_H

#include "context_menu.h"
#include "tree_browser.h"
#include "menu.h"
#include "subsys.h"

// The main menu of the Commodore interface: a list of destinations rather than
// a directory. Where the Ultimate interface opens in the file browser and
// reaches the settings through F2, this opens on the settings and reaches the
// browser through an entry of its own.
//
// It is a ContextMenu, which already knows how to draw a list and move a bar
// through it. What it adds is the list itself, a window in the middle of the
// screen instead of one hooked to a line, and the keys the Commodore layout
// puts the categories on.
class CommodoreMenu: public ContextMenu, ObjectWithMenu
{
    static SubsysResultCode_e S_file_browser(Action *act, void *context);
    static SubsysResultCode_e S_cfg_page(Action *act, void *context);
    static SubsysResultCode_e S_cfg_group(Action *act, void *context);
    static SubsysResultCode_e S_cfg_audio(Action *act, void *context);
    static SubsysResultCode_e S_advanced(Action *act, void *context);
    static SubsysResultCode_e S_assembly64(Action *act, void *context);
    static SubsysResultCode_e S_sysinfo(Action *act, void *context);

    // The entry the task menu offers to come back here with.
    static SubsysResultCode_e S_SendMenuKey(SubsysCommand *cmd) {
        cmd->user_interface->send_keystroke(KEY_MENU);
        return SSRET_OK;
    }

    TreeBrowser *root_tree_browser;
    int handle_key(int c);

public:
    CommodoreMenu(UserInterface *ui);
    virtual ~CommodoreMenu();

    virtual void init();
    void redraw();
    int poll(int);
    int select_item(void);

    void create_task_items(void);
};

#endif
