#ifndef MACHINE_HOTKEYS_H
#define MACHINE_HOTKEYS_H

// Keys that act on the machine from any menu (doc/specs/SPEC-03): C= R reset,
// C= Z reboot, C= B power cycle (U64-II / C64U only: the U64 has no software
// power cycle, c64_subsys.cc), C= K power off. One helper, called by every menu
// object's handle_key, so a key means the same wherever the cursor is.
//
// C=+Q would be the obvious power-off key, but C=+Q is 0x11, which is RUN/STOP
// (keymap_control, keyboard_c64.cc); C=+S is HOME, C=+H is DEL, C=+M RETURN.

#ifndef RECOVERYAPP

#include "userinterface.h"
#include "subsys.h"
#include "c64.h"
#include "keyboard.h"

// Returns the menu response the caller should return (as the browser's own
// C=+R did), 0 when the user said no, or -1 when c is not one of these keys.
static inline int machine_hotkey(UserInterface *ui, int c)
{
    int function;
    const char *question = NULL;
    switch (c) {
    case KEY_CTRL_R:
        function = MENU_C64_RESET;
        break;
    case KEY_CTRL_Z:
        function = MENU_C64_REBOOT;
        break;
#if U64 == 2
    case KEY_CTRL_B:
        function = MENU_C64_POWERCYCLE;
        question = "Power cycle the machine?";
        break;
#endif
    case KEY_CTRL_K:
        function = MENU_C64_POWEROFF;
        question = "Power off the machine?";
        break;
    default:
        return -1;
    }
    if (question && (ui->popup(question, BUTTON_YES | BUTTON_NO) != BUTTON_YES)) {
        return 0;
    }
    // Releases, unfreezes and acts by itself (C64_Subsys::executeCommand)
    SubsysCommand *cmd = new SubsysCommand(ui, SUBSYSID_C64, function, 0);
    cmd->execute(); // deletes itself
    return (int)(ui->menu_response_to_action);
}

#endif // RECOVERYAPP
#endif
