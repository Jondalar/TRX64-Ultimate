// trx64u's UCI library (uci/, U18): a header of the firmware that the UCI sources include and SDK 0.4.6 no longer ships (MultiCore T239.3 D48).
// PROVENANCE: TREX/1541ultimate:software/io/command_interface/palette_command.h (1541ultimate, Gideon Zweijtzer, GPL-3.0; our fork at 0103c585), byte for byte the
// header TRX64U-MultiCore's host-app carries at t239.3 6e280710 and SDK 0.4.5 shipped in base/inc. Changes here: this header only.
#ifndef PALETTE_COMMAND_H
#define PALETTE_COMMAND_H

#include <stdint.h>
#include <string.h>

#define UCI_PALETTE_COLORS 16
#define UCI_PALETTE_BYTES (UCI_PALETTE_COLORS * 3)

static inline bool decode_palette_set(const uint8_t *message, int length,
                                      uint8_t rgb[UCI_PALETTE_COLORS][3])
{
    if (!message || length != UCI_PALETTE_BYTES + 2) {
        return false;
    }
    memcpy(rgb, message + 2, UCI_PALETTE_BYTES);
    return true;
}

static inline bool decode_palette_color_set(const uint8_t *message, int length,
                                            uint8_t *index, uint8_t rgb[3])
{
    if (!message || length != 6 || message[2] >= UCI_PALETTE_COLORS) {
        return false;
    }
    *index = message[2];
    memcpy(rgb, message + 3, 3);
    return true;
}

static inline bool valid_palette_reset(int length)
{
    return length == 2;
}

#endif
