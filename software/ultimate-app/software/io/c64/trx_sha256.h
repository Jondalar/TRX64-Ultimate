// SHA-256 (FIPS 180-4), for the containers that carry one: the .c64re's hash
// over its gzip body (T20 §9.1) and the .trxring's over its parts (§10.2).
// Plain C so the host test bank and the firmware use the same code.
#ifndef TRX_SHA256_H
#define TRX_SHA256_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint8_t  block[64];
    uint32_t len_lo, len_hi;
    int      fill;
} TrxSha256;

void trx_sha_init(TrxSha256 *s);
void trx_sha_add(TrxSha256 *s, const uint8_t *p, uint32_t len);
void trx_sha_final(TrxSha256 *s, uint8_t *out);   // 32 bytes

#ifdef __cplusplus
}
#endif

#endif
