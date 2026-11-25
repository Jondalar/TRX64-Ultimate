// trx64u's UCI library (uci/, U18; MultiCore T239.3 D48/D49): the targets' base class.
// PROVENANCE: TRX64 SDK 0.4.6 handover/uci/command_target.h (TRX64U-MultiCore t239.3 6e280710), which is 1541ultimate (Gideon Zweijtzer)
// software/io/command_interface/command_target.h at commit 32e5fd5f, GPL-3.0, as the core app compiled it until SDK 0.4.5 (MultiCore's changes: uci_reset_hook,
// and start() is called by the interface's owner, uci_start in trxuci.cc). Changes here: this header only.
#ifndef _COMMAND_TARGET_H
#define _COMMAND_TARGET_H

#include <stdint.h>

/* Command Targets */

#define CMD_IF_MAX_TARGET   0x0F
#define CMD_IF_NO_REPLY     0x80

// Mandatory commands for all targets
#define CMD_IDENTIFY   0x01

typedef struct _message
{
    int   length;
    bool  last_part;
    uint8_t *message;
} Message;

extern Message c_message_no_target;
extern Message c_status_ok;
extern Message c_status_unknown_command;
extern Message c_message_empty;

class CommandTarget
{
public:
    CommandTarget() { }
    virtual ~CommandTarget() { }

    virtual void parse_command(Message *command, Message **reply, Message **status) {
        if(command->message[1] == CMD_IDENTIFY) {
            *reply  = &c_message_no_target;
            *status = &c_status_ok;
        } else {
            *reply  = &c_message_empty;
            *status = &c_status_unknown_command; 
        }        
    }
    virtual void get_more_data(Message **reply, Message **status) {
        *reply  = &c_message_empty;
        *status = &c_status_ok; 
    }
    
    virtual void abort(int a) { }

    // The C64 was reset, so the program talking to this target is gone.
    virtual void c64_reset(void) { }
};

extern CommandTarget *command_targets[];

#ifndef CMD_MAX_REPLY_LEN
#define CMD_MAX_COMMAND_LEN 896
#define CMD_MAX_REPLY_LEN   896
#define CMD_MAX_STATUS_LEN  256
#endif

#endif