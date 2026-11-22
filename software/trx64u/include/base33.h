// The base 3.3 groups as the trx64u libraries reach them until the SDK's LibApi carries them (U18, round 2). The contract is MultiCore's
// (../contract/, copied unchanged); the members join LibApi after `lib` (sync, settings, ctx, io, board), SvcSys and SvcFs get members appended.
//
// Each accessor is declared and not defined: a library's link then lists exactly the 3.3 groups it uses, and nothing else is pending. When an SDK
// with base 3.3 exists, each library defines them in one place from its LibApi (api->sync, ...; the appended SvcSys / SvcFs members are then read
// from api->sys / api->fs), returning null when the base's LibApi is shorter (test the size first).
#ifndef TRX64U_BASE33_H
#define TRX64U_BASE33_H

#include "svc_sync.h"
#include "svc_settings.h"
#include "svc_ctx.h"
#include "svc_io.h"
#include "svc_board.h"
#include "svc_sys.h"
#include "svc_fs.h"

#ifdef __cplusplus
extern "C" {
#endif

const SvcSync *b33_sync(void);
const SvcSettings *b33_settings(void);
const SvcCtx *b33_ctx(void);
const SvcIo *b33_io(void);
const SvcBoard *b33_board(void);
const SvcSysAdd *b33_sys(void);       // the members 3.3 appends to SvcSys
const SvcFsAdd *b33_fs(void);         // the member 3.3 appends to SvcFs

#ifdef __cplusplus
}
#endif

#endif
