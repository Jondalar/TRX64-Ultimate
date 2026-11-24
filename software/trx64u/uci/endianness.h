// trx64u's UCI library (uci/, U18): a header of the firmware that the UCI sources include and SDK 0.4.6 no longer ships (MultiCore T239.3 D48).
// PROVENANCE: software/ultimate-app/software/portable/riscv/endianness.h (1541ultimate, Gideon Zweijtzer, GPL-3.0; our submodule at 0103c585), byte for byte the
// header TRX64U-MultiCore's host-app carries at t239.3 6e280710 and SDK 0.4.5 shipped in base/inc. Changes here: this header only.
#ifndef ENDIANNESS_H
#define ENDIANNESS_H

__inline uint32_t cpu_to_32le(uint32_t a)
{
    return a;
}

__inline uint16_t cpu_to_16le(uint16_t p)
{
    return p;
}

#define le2cpu(x)       cpu_to_16le(x)
#define cpu2le(x)       cpu_to_16le(x)
#define le16_to_cpu(x)  cpu_to_16le(x)
#define le_to_cpu_32(x) cpu_to_32le(x)

#endif /* ENDIANNESS_H */
