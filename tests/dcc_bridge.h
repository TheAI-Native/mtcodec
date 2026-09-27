#ifndef DCC_BRIDGE_H
#define DCC_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
/* method: 0 Huffman body, 1 clustered rANS body, 2 high-bits rANS body,
 *         3 adaptive body, 4 complete original file container.
 * param: requested k (0 = selected), ctx_bits, adaptive order, or container
 *        method (0=h,1=r,2=a), respectively.
 */
typedef struct {
    int available, k;
    uint64_t count_bits, map_bits, table_bits, padding_bits, payload_bits;
    uint64_t state_bytes;
    uint8_t cmap[256];
} DccStats;
#define DCC_DECLARE(P) \
int P##_encode(int method, int param, const uint8_t *s, size_t n, \
               uint8_t **p, size_t *len, int *k); \
int P##_decode(int method, const uint8_t *p, size_t len, size_t n, uint8_t *out); \
int P##_stats(int method, const uint8_t *p, size_t len, const uint8_t *s, \
              size_t n, DccStats *st)
DCC_DECLARE(dcc_ref);
DCC_DECLARE(dcc_exp);
int dcc_exp_with_map(int coder, const uint8_t *s, size_t n, int k,
                     const uint8_t map[256], uint8_t **p, size_t *len);
#endif
