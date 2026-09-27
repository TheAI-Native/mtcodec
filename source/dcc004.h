#ifndef DCC004_H
#define DCC004_H
#include "dcc003.h"
#define DCC004_NVARIANTS 17
/* Generator 3 is the prespecified initialization-only ablation. */
typedef struct {
    uint64_t count_bits, map_bits, table_bits, payload_units;
} Dcc004Cost;
typedef struct {
    int decoder, generator, requested_k, k, candidates, emissions;
    uint8_t map[256];
} Dcc004Meta;
typedef struct { const char *name; int coder, generator, selection, special; } Dcc004Variant;
extern const Dcc004Variant dcc004_variants[DCC004_NVARIANTS];
int dcc004_generate(const Dcc003Input*,int coder,int generator,int cap,Dcc003Map*);
int dcc004_cost(const Dcc003Input*,int coder,const Dcc003Map*,Dcc004Cost*);
int dcc004_encode(const uint8_t*,size_t,int variant,uint8_t**,size_t*,Dcc004Meta*);
int dcc004_decode(int variant,const uint8_t*,size_t,size_t,uint8_t*);
int dcc004_stats(int variant,const uint8_t*,size_t,const uint8_t*,size_t,Dcc003Size*);
uint64_t dcc004_decoder_tables(int variant,int k);
uint64_t dcc004_selection_cost(int coder,int selection,const Dcc004Cost*,size_t bytes,const Dcc003Size*);
#endif
