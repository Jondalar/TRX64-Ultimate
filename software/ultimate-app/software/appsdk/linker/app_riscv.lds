/*
 * U64 App linker script — RISC-V rv32 (U64-II, C64U), fixed load address 0x03A00000.
 *
 * Apps are built as a flat binary (no ELF loader in firmware). tools/mk_u2a.py
 * takes the allocated sections as the payload and reads _app_start and _bss_end.
 *
 * gp belongs to the firmware. Apps are compiled with -msmall-data-limit=0 and
 * -mno-relax, linked with --no-relax, and this script defines no
 * __global_pointer$, so no code addresses data relative to gp.
 */

OUTPUT_FORMAT("elf32-littleriscv", "elf32-littleriscv", "elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_app_start)

MEMORY
{
    app_area : ORIGIN = 0x03A00000, LENGTH = 0x00200000   /* 2 MB */
}

SECTIONS
{
    .text 0x03A00000 :
    {
        KEEP(*(.app_header))              /* reserved: mk_u2a.py fills this */
        KEEP(*(.text._app_start))         /* crt0 entry — must be first code */
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        _etext = .;
    } > app_area

    .data : ALIGN(4)
    {
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        _data_end = .;
    } > app_area

    _payload_end = .;      /* end of loaded bytes (code + rodata + data) */

    .bss : ALIGN(4)
    {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } > app_area

    /DISCARD/ : { *(.comment) *(.debug*) *(.eh_frame) *(.note*) }
}
