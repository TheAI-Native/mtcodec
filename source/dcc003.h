#ifndef DCC003_H
#define DCC003_H
#include <stdint.h>
#include <stddef.h>
/* Research interface: non-reentrant DCC002 codec; use one worker per process.
 * All inputs are raw bytes, max 512 MiB, previous byte initialized to zero.
 * coder: 0=Huffman; 1=rANS. generator: 0=original reassignment;
 * 1=Brotli-native population/map-cost policy; 2=matched-cost merge/remap.
 */
#define DCC003_MAX_INPUT ((size_t)512 * 1024 * 1024)
#define DCC003_NCANDIDATES 12
extern const int dcc003_counts[DCC003_NCANDIDATES];
typedef struct Dcc003Input Dcc003Input;
typedef struct {
    int k, active_contexts;
    uint8_t map[256];
} Dcc003Map;
typedef struct {
    uint64_t count_bits, map_bits, table_bits, padding_bits, payload_bits, state_bytes;
} Dcc003Size;
Dcc003Input *dcc003_input(const uint8_t *s, size_t n);
void dcc003_input_free(Dcc003Input *in);
int dcc003_generate(const Dcc003Input *in, int coder, int generator, int cap, Dcc003Map *map);
int dcc003_encode_map(const Dcc003Input *in, int coder, const Dcc003Map *map,
                      uint8_t **out, size_t *bytes, Dcc003Size *sz);
int dcc003_decode(int coder, const uint8_t *p, size_t len, size_t n, uint8_t *out);
int dcc003_original(const Dcc003Input *in, int coder, int cap,
                    uint8_t **out, size_t *bytes, int *k);
/* Returns deterministic histogram costs in bits, excluding context-map cost. */
double dcc003_huff_cost(const uint32_t counts[256]);
double dcc003_rans_cost(const uint32_t counts[256]);
/* Compact histograms are in ascending active-context order; inactive contexts
 * are not sent to the clustering routine. Symbols are dense on return. */
int dcc003_native_policy(const uint32_t *counts, size_t rows, size_t cap,
                         uint32_t *symbols, size_t *k);
int dcc003_huff_policy(const uint32_t *counts, size_t rows, size_t cap,
                       uint32_t *symbols, size_t *k);
int dcc003_rans_policy(const uint32_t *counts, size_t rows, size_t cap,
                       uint32_t *symbols, size_t *k);
#endif
