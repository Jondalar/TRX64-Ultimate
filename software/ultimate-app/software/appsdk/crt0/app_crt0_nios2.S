/*
 * app_crt0_nios2.S — U64 App startup stub (Nios II).
 *
 * Loader sets up the FreeRTOS task stack and jumps here with:
 *   r4 = const u2p_api_t *api
 *   r5 = const u2p_app_context_t *ctx
 *
 * We save the caller's return address (Nios II `call` clobbers ra),
 * tail-call app_main, restore ra, and return with app_main's exit code
 * intact in r2.
 */

    .section .text._app_start, "ax", @progbits
    .global  _app_start
    .type    _app_start, @function
    .align   2
_app_start:
    addi    sp, sp, -4
    stw     ra, 0(sp)
    call    app_main
    ldw     ra, 0(sp)
    addi    sp, sp, 4
    ret
    .size   _app_start, . - _app_start
