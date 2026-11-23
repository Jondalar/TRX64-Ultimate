// The C64 descriptor's code (c64core.cc): its operations and states, which lib_trx64u.cc lists with their names and arguments, and its hooks, which
// lib_trx64u.cc exports in "corehooks". Its data is desc.toml.
#ifndef TRX64U_C64CORE_H
#define TRX64U_C64CORE_H

#include "ops.h"

int c64_mem_read(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_mem_write(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_put_basic(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_put_kernal(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_put_char(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_freeze_mode(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_run(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_run_with_app(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_load(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_run_crt(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_dma(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_dma_write(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_run_t64(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_dma_t64(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_set_rom(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_remove_cart(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_service(CoreCtx *c, const OpArgs &a, OpOut &o);
int c64_type(CoreCtx *c, const OpArgs &a, OpOut &o);
const char *c64_prg_name(CoreCtx *c);
const char *c64_prg_pending(CoreCtx *c);
const char *c64_prg_path(CoreCtx *c);

// The descriptor's hooks (core_hooks.h, same meaning as CoreDesc::init, release, media_flush, media_pending): init holds the machine and ends what the reset
// ends, release lets it go, the media hooks are drive A's write-back.
int c64_hook_init(char *msg, int msglen);
int c64_hook_release(char *msg, int msglen);
int c64_hook_media_flush(char *msg, int msglen);
bool c64_hook_media_pending(void);

#endif
