/*
 * U64 App linker script — Nios II, fixed load address 0x03A00000.
 *
 * Apps are built as a flat binary (no ELF loader in firmware).
 * objcopy -O binary produces the payload; tools/mk_u2a.py prepends the
 * 64-byte u2p_app_header_t.
 */

OUTPUT_FORMAT("elf32-littlenios2", "elf32-littlenios2", "elf32-littlenios2")
OUTPUT_ARCH(nios2)
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

/* Symbols surfaced to mk_u2a.py via `nm`: */
PROVIDE(__app_entry         = _app_start);
PROVIDE(__app_payload_size  = _payload_end - 0x03A00000);
PROVIDE(__app_bss_size      = _bss_end - _bss_start);
