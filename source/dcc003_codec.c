/* The codec is the exact, SHA-pinned DCC002 source. No edits to its generator,
 * normalizer, header format, or decoder. New methods enter ONLY through maps. */
#define main dcc003_preserved_dcc002_main
#include "mtcodec_dcc002.c"
#undef main
#include "dcc003.h"
#include "../tests/dcc_bridge.h"
#define DCC_PREFIX dcc003_legacy
#include "../tests/dcc_bridge_impl.h"

const int dcc003_counts[DCC003_NCANDIDATES]={1,2,3,4,6,8,12,16,24,32,48,64};
struct Dcc003Input { const uint8_t *s; size_t n; Bigrams *bg; };
Dcc003Input *dcc003_input(const uint8_t *s,size_t n) {
    Dcc003Input *in;
    if ((n && !s) || n>DCC003_MAX_INPUT) return NULL;
    in=(Dcc003Input*)malloc(sizeof(*in)); if (!in) return NULL;
    in->s=s; in->n=n; in->bg=bigrams_build(s,n);
    if (!in->bg) { free(in); return NULL; }
    rans_cost_init(); return in;
}
void dcc003_input_free(Dcc003Input *in) { if (in) { free(in->bg); free(in); } }

double dcc003_huff_cost(const uint32_t counts[256]) {
    uint64_t c[256],bits; uint8_t lens[256]; int s;
    for (s=0;s<256;++s) c[s]=counts[s];
    huff_code_lengths(c,lens); bits=huff_lengths_cost_bits(lens);
    for (s=0;s<256;++s) bits+=c[s]*lens[s];
    return (double)bits;
}
double dcc003_rans_cost(const uint32_t counts[256]) {
    uint16_t freq[256]; uint64_t units; int s;
    rans_cost_init(); rans_normalize(counts,freq);
    units=rans_table_cost_bits(freq)*COST_ONE;
    for (s=0;s<256;++s) units+=(uint64_t)counts[s]*rans_cost_tab[freq[s]];
    return (double)units / (double)COST_ONE;
}
int dcc003_generate(const Dcc003Input *in,int coder,int generator,int cap,Dcc003Map *map) {
    int c,j,na=0,active[256];
    if (!in || !map || coder<0 || coder>1 || generator<0 || generator>2 || cap<1 || cap>64) return 0;
    memset(map,0,sizeof(*map));
    for (c=0;c<256;++c) if (in->bg->nsym[c]) active[na++]=c;
    map->active_contexts=na;
    if (generator==0) {
        map->k=coder==0 ? huff_assign_contexts(in->bg,cap,map->map) : rans_assign_contexts(in->bg,cap,map->map);
    } else if (!na) { map->k=1; }
    else {
        uint32_t *flat=(uint32_t*)calloc((size_t)na*256,sizeof(*flat));
        uint32_t symbols[256]; size_t k=0; int ok;
        if (!flat) return 0;
        for (j=0;j<na;++j) {
            c=active[j];
            for (int z=0;z<in->bg->nsym[c];++z)
                flat[256*j+in->bg->sym[c][z]]=in->bg->cnt[c][z];
        }
        if (generator==1) ok=dcc003_native_policy(flat,(size_t)na,(size_t)cap,symbols,&k);
        else if (coder==0) ok=dcc003_huff_policy(flat,(size_t)na,(size_t)cap,symbols,&k);
        else ok=dcc003_rans_policy(flat,(size_t)na,(size_t)cap,symbols,&k);
        free(flat);
        if (!ok) return 0;
        map->k=(int)k;
        for (j=0;j<na;++j) map->map[active[j]]=(uint8_t)symbols[j];
    }
    if (map->k<1 || map->k>cap || !dcc_context_map_valid(map->k,map->map)) return 0;
    if (na) {
        int used[64]={0};
        for (j=0;j<na;++j) used[map->map[active[j]]]=1;
        for (j=0;j<map->k;++j) if (!used[j]) return 0;
    }
    return 1;
}
int dcc003_encode_map(const Dcc003Input *in,int coder,const Dcc003Map *map,uint8_t **out,size_t *bytes,Dcc003Size *sz) {
    Buf b; DccStats st = {0}; int ok;
    if (!in || !map || !out || !bytes || !sz || coder<0 || coder>1) return 0;
    *out=NULL; *bytes=0; memset(sz,0,sizeof(*sz)); buf_init(&b);
    ok=coder==0 ? huff_encode_with_map(in->s,in->n,in->bg,map->k,map->map,&b) :
                  rans_mt_encode_with_map(in->s,in->n,in->bg,map->k,map->map,&b);
    if (ok) ok=dcc003_legacy_stats(coder,b.p,b.len,in->s,in->n,&st);
    if (ok) ok=st.k==map->k && !memcmp(st.cmap,map->map,256);
    if (!ok) { buf_free(&b); return 0; }
    sz->count_bits=st.count_bits; sz->map_bits=st.map_bits; sz->table_bits=st.table_bits;
    sz->padding_bits=st.padding_bits; sz->payload_bits=st.payload_bits; sz->state_bytes=st.state_bytes;
    *out=b.p; *bytes=b.len; return 1;
}
int dcc003_decode(int coder,const uint8_t *p,size_t len,size_t n,uint8_t *out) {
    if (coder<0 || coder>1 || !p || !out || n>DCC003_MAX_INPUT) return 0;
    return coder==0 ? huff_decode(p,len,n,out) : rans_mt_decode(p,len,n,out);
}
int dcc003_original(const Dcc003Input *in,int coder,int cap,uint8_t **out,size_t *bytes,int *k) {
    if (!in || coder<0 || coder>1 || cap<0 || cap>64) return 0;
    return dcc003_legacy_encode(coder,cap,in->s,in->n,out,bytes,k);
}

#include "dcc004_engine_impl.h"
