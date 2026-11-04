/*
 * app_crt0_riscv.S — U64 App startup stub (RISC-V rv32, U64-II / C64U).
 *
 * The loader runs the app in its own FreeRTOS task and calls this entry with:
 *   a0 = const u2p_api_t *api
 *   a1 = const u2p_app_context_t *ctx
 *
 * We save the caller's return address, call app_main with a0/a1 untouched,
 * restore ra and return with app_main's exit code intact in a0. The stack
 * frame keeps the 16-byte ABI alignment. sp and gp are never loaded: the task
 * stack and the global pointer belong to the firmware.
 */

    .option  norelax
    .section .text._app_start, "ax", @progbits
    .global  _app_start
    .type    _app_start, @function
    .balign  4
_app_start:
    addi    sp, sp, -16
    sw      ra, 12(sp)
    call    app_main
    lw      ra, 12(sp)
    addi    sp, sp, 16
    ret
    .size   _app_start, . - _app_start
