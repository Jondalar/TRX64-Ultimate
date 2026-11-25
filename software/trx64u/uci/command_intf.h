// trx64u's UCI library (uci/, U18; MultiCore T239.3 D48/D49): the command interface server.
// PROVENANCE: TRX64 SDK 0.4.6 handover/uci/command_intf.h (TRX64U-MultiCore t239.3 6e280710), which is 1541ultimate (Gideon Zweijtzer)
// software/io/command_interface/command_intf.h at commit 32e5fd5f, GPL-3.0, as the core app compiled it until SDK 0.4.5 (MultiCore's changes: uci_reset_hook,
// and start() is called by the interface's owner, uci_start in trxuci.cc). Changes here: this header, and the registers come from the driver table of c64_basic's TOML A
// (block cmd_if) in place of iomap.h's CMD_IF_BASE plus an offset (U18; the TOML A's comment names the RTL lines).
#ifndef COMMAND_INTF_H
#define COMMAND_INTF_H

#include "integer.h"
#include "config.h"
#include "iomap.h"
#include "subsys.h"
#include "command_target.h"
#include "drvtab.h"

// TRX64-Ultimate: the block cmd_if of c64_basic's TOML A (../gateware/c64_basic.toml, MultiCore T239.3 D48), its properties named as command_if_pkg.vhd's
// c_cif_io_*; the firmware's names stay. +4 and +5 are two properties each: command_start and command_end read, irq_mask_set and irq_mask_clear write.
#define CMD_IF_REG(p)          (*DRV_PTR(uint8_t, DRV_cmd_if_##p##_ADDR))
#define CMD_IF_RAM_BASE        DRV_cmd_if_buffer_ADDR

#define CMD_IF_RAM(x)          *((volatile uint8_t *)(CMD_IF_RAM_BASE +x))

#define CMD_IF_SLOT_BASE       CMD_IF_REG(slot_base)
#define CMD_IF_SLOT_ENABLE     CMD_IF_REG(slot_enable)
#define CMD_IF_HANDSHAKE_OUT   CMD_IF_REG(handshake_out)
#define CMD_IF_STATUSBYTE      CMD_IF_REG(handshake_in)
#define CMD_IF_COMMAND_START   CMD_IF_REG(command_start)
#define CMD_IF_COMMAND_END     CMD_IF_REG(command_end)
#define CMD_IF_RESPONSE_START  CMD_IF_REG(response_start)
#define CMD_IF_RESPONSE_END    CMD_IF_REG(response_end)
#define CMD_IF_STATUS_START    CMD_IF_REG(status_start)
#define CMD_IF_STATUS_END      CMD_IF_REG(status_end)
#define CMD_IF_STATUS_LENGTH   CMD_IF_REG(status_length)
#define CMD_IF_IRQMASK         CMD_IF_REG(irq_mask)
#define CMD_IF_RESPONSE_LEN_L  CMD_IF_REG(response_len_l)
#define CMD_IF_RESPONSE_LEN_H  CMD_IF_REG(response_len_h)
#define CMD_IF_COMMAND_LEN_L   CMD_IF_REG(command_len_l)
#define CMD_IF_COMMAND_LEN_H   CMD_IF_REG(command_len_h)
#define CMD_IF_IRQMASK_SET     CMD_IF_REG(irq_mask_set)
#define CMD_IF_IRQMASK_CLEAR   CMD_IF_REG(irq_mask_clear)

#define CMD_NEW_COMMAND           0x01
#define CMD_DATA_ACCEPTED         0x02
#define CMD_ABORT_DATA            0x04
#define CMD_MORE_OR_ABORT         0x06
#define CMD_STATE_BITS            0x30
#define CMD_STATE_IDLE            0x00
#define CMD_STATE_PROCESSING      0x10
#define CMD_STATE_DATA_LAST       0x20
#define CMD_STATE_DATA_MORE       0x30

#define HANDSHAKE_RESET           0x87
#define HANDSHAKE_ACCEPT_COMMAND  0x01
#define HANDSHAKE_ACCEPT_NEXTDATA 0x02
#define HANDSHAKE_ACCEPT_ABORT    0x04
#define HANDSHAKE_VALIDATE_LAST   0x10
#define HANDSHAKE_VALIDATE_MORE   0x30

#define CMD_HS_DMA_ACTIVE         0x80
#define CMD_HS_TRIGGER_ACTIVE     0x40

#define CMD_TARGET_NONE 0xFF

#define CMD_MAX_COMMAND_LEN 896
#define CMD_MAX_REPLY_LEN   896
#define CMD_MAX_STATUS_LEN  256


class SubSystem;

class CommandInterface : public SubSystem
{
	TaskHandle_t taskHandle;
	TaskHandle_t resetTaskHandle;

	int response_offset;
	uint8_t *command_buffer;
    uint8_t *response_buffer;
    uint8_t *status_buffer;

    Message incoming_command;
    uint8_t target;    
    void copy_result(Message *data, Message *status);
    int cart_mode;
    
    static void start_task(void *a);
    static void reset_task(void *a);
    void run_task(void);
    void run_reset_task(void);

public:
	QueueHandle_t queue; // needed from IRQ

	CommandInterface();
    ~CommandInterface();
    void start(void);   // TRX64U: FW did this in the constructor
    
    void dump_registers(void);
    const char *identify(void) { return "Command Interface"; }
    bool is_dma_active(void);
    void set_kernal_device_id(uint8_t id);
};

extern CommandInterface cmd_if;
extern void (*uci_reset_hook)(void);

#endif
