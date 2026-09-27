/* DCC004 complete encoder paths. Included once, after frozen codec definitions.
 * No candidate logging, output verification, memory tracking, or wall clock
 * calls occur here. Allocator tracking is a separate instrumented build.
 */
#include "dcc004.h"
const Dcc004Variant dcc004_variants[DCC004_NVARIANTS] = {
 {"huff_reassignment",0,0,0,0},{"rans_reassignment",1,0,0,0},
 {"huff_native",0,1,0,0},{"rans_native",1,1,0,0},
 {"huff_matched",0,2,0,0},{"rans_matched",1,2,0,0},
 {"huff_init_only",0,3,0,0},{"rans_init_only",1,3,0,0},
 {"huff_payload_only",0,0,1,0},{"rans_payload_only",1,0,1,0},
 {"rans_estimated_total",1,0,2,0},
 {"huff_single",0,0,0,1},{"rans_single",2,-1,0,2},
 {"rans_highbits",2,-1,0,3},{"adaptive_order1",3,-1,0,4},
 {"rans_native_estimated",1,1,2,0},{"rans_matched_estimated",1,2,2,0}
};
int dcc004_generate(const Dcc003Input *in,int coder,int gen,int cap,Dcc003Map *map) {
    int active[256],order[256],na=0,c,i;
    if(gen!=3)return dcc003_generate(in,coder,gen,cap,map);
    if(!in||!map||coder<0||coder>1||cap<1||cap>64)return 0;
    memset(map,0,sizeof(*map));
    for(c=0;c<256;++c)if(in->bg->nsym[c])active[na++]=c;
    map->active_contexts=na;map->k=na?(na<cap?na:cap):1;
    /* Exact frequency-ordered stable round-robin initialization from DCC002. */
    for(i=0;i<na;++i){int x=active[i],j=i;
        while(j>0&&in->bg->total[order[j-1]]<in->bg->total[x]){order[j]=order[j-1];--j;}
        order[j]=x;
    }
    for(i=0;i<na;++i)map->map[order[i]]=(uint8_t)(i%cap);
    return 1;
}
int dcc004_cost(const Dcc003Input *in,int coder,const Dcc003Map *m,Dcc004Cost *z) {
    int t,c;
    if(!in||!m||!z||coder<0||coder>1||!dcc_context_map_valid(m->k,m->map))return 0;
    memset(z,0,sizeof(*z));z->count_bits=8;
    if(m->k>1)z->map_bits=(uint64_t)256*(uint64_t)huff_cmap_bits(m->k);
    if(coder==0){uint8_t lens[64][256];
        huff_cluster_lengths(in->bg,m->map,m->k,lens);
        for(t=0;t<m->k;++t)z->table_bits+=huff_lengths_cost_bits(lens[t]);
        for(c=0;c<256;++c)z->payload_units+=huff_assignment_cost(in->bg,c,lens[m->map[c]])*COST_ONE;
    }else{uint16_t freq[64][256];
        rans_cluster_tables(in->bg,m->map,m->k,freq);
        for(t=0;t<m->k;++t)z->table_bits+=rans_table_cost_bits(freq[t]);
        for(c=0;c<256;++c)z->payload_units+=rans_assignment_cost(in->bg,c,freq[m->map[c]]);
    }
    return 1;
}
uint64_t dcc004_selection_cost(int coder,int selection,const Dcc004Cost *z,size_t bytes,const Dcc003Size *sz) {
    if(coder==0)return selection==1?z->payload_units/COST_ONE:
        z->count_bits+z->map_bits+z->table_bits+z->payload_units/COST_ONE;
    if(selection==0)return (uint64_t)bytes;
    if(selection==1)return sz->payload_bits; /* Actual renormalization bytes; constant state excluded. */
    /* Complete byte-aligned header and constant state, plus fixed-point payload. */
    return ((((z->count_bits+z->map_bits+z->table_bits+7)/8)*8)+32)*COST_ONE+z->payload_units;
}
static int dcc004_emit(const Dcc003Input *in,int coder,const Dcc003Map *m,Buf *b) {
    return coder==0?huff_encode_with_map(in->s,in->n,in->bg,m->k,m->map,b):
        rans_mt_encode_with_map(in->s,in->n,in->bg,m->k,m->map,b);
}
int dcc004_encode(const uint8_t *s,size_t n,int variant,uint8_t **out,size_t *len,Dcc004Meta *meta) {
    const Dcc004Variant *v;Dcc003Input *in=NULL;Buf best;Dcc003Map winner={0};
    uint64_t mincost=UINT64_MAX;int best_cap=0,ok=1;
    if(!out||!len||!meta)return 0;
    *out=NULL;*len=0;memset(meta,0,sizeof(*meta));buf_init(&best);
    if(variant<0||variant>=DCC004_NVARIANTS||(n&&!s)||n>DCC003_MAX_INPUT)return 0;
    v=&dcc004_variants[variant];meta->decoder=v->coder;meta->generator=v->generator;
    if(v->special==2||v->special==3){
        int choices=v->special==2?1:9;
        for(int b=0;b<choices;++b){Buf tmp;buf_init(&tmp);meta->candidates++;meta->emissions++;
            if(!rans_encode(s,n,b,&tmp)){buf_free(&tmp);ok=0;break;}
            if(best_cap==0||tmp.len<best.len){buf_free(&best);best=tmp;tmp.p=NULL;winner.k=1<<b;best_cap=winner.k;
                for(int c=0;c<256;++c)winner.map[c]=(uint8_t)rans_ctx_of(c,b);}
            buf_free(&tmp);
        }
    }else if(v->special==4){meta->candidates=1;meta->emissions=1;best_cap=256;winner.k=256;
        ok=adapt_encode(s,n,1,&best);
    }else{
        in=dcc003_input(s,n);if(!in)return 0;
        int count=v->special==1?1:12;
        for(int ci=0;ci<count&&ok;++ci){
            int cap=dcc003_counts[ci];Dcc003Map m={0};Dcc004Cost cost={0};
            Dcc003Size sz={0};Buf tmp;uint64_t rank;buf_init(&tmp);meta->candidates++;
            ok=dcc004_generate(in,v->coder,v->generator,cap,&m);
            if(ok&&(v->coder==0||v->selection==2))ok=dcc004_cost(in,v->coder,&m,&cost);
            if(ok&&v->coder==1&&v->selection!=2){
                ok=dcc004_emit(in,1,&m,&tmp);meta->emissions++;
                if(ok&&v->selection==1){DccStats st={0};
                    /* Parsing headers is needed to rank actual payload alone. No decode/memcmp here. */
                    ok=dcc003_legacy_stats(1,tmp.p,tmp.len,s,n,&st);sz.payload_bits=st.payload_bits;
                }
            }
            if(!ok){buf_free(&tmp);break;}
            rank=dcc004_selection_cost(v->coder,v->selection,&cost,tmp.len,&sz);
            if(best_cap==0||rank<mincost){
                mincost=rank;winner=m;best_cap=cap;
                if(v->coder==1&&v->selection!=2){buf_free(&best);best=tmp;tmp.p=NULL;}
            }
            buf_free(&tmp);
        }
        if(ok&&(v->coder==0||v->selection==2)){ok=dcc004_emit(in,v->coder,&winner,&best);meta->emissions++;}
        dcc003_input_free(in);
    }
    if(!ok||!best_cap){buf_free(&best);return 0;}
    meta->requested_k=best_cap;meta->k=winner.k;memcpy(meta->map,winner.map,256);
    *out=best.p;*len=best.len;return 1;
}
int dcc004_decode(int variant,const uint8_t *p,size_t len,size_t n,uint8_t *out) {
    if(variant<0||variant>=DCC004_NVARIANTS||!p||!out||n>DCC003_MAX_INPUT)return 0;
    switch(dcc004_variants[variant].coder){
    case 0:return huff_decode(p,len,n,out);
    case 1:return rans_mt_decode(p,len,n,out);
    case 2:return rans_decode(p,len,n,out);
    case 3:return adapt_decode(p,len,n,out);
    default:return 0;
    }
}
int dcc004_stats(int variant,const uint8_t *p,size_t len,const uint8_t *s,size_t n,Dcc003Size *z) {
    DccStats st={0};int coder;
    if(variant<0||variant>=DCC004_NVARIANTS||!z)return 0;
    memset(z,0,sizeof(*z));coder=dcc004_variants[variant].coder;
    if(coder==3)return 1; /* No invented adaptive size decomposition. */
    if(!dcc003_legacy_stats(coder,p,len,s,n,&st))return 0;
    z->count_bits=st.count_bits;z->map_bits=st.map_bits;z->table_bits=st.table_bits;
    z->padding_bits=st.padding_bits;z->payload_bits=st.payload_bits;z->state_bytes=st.state_bytes;
    return 1;
}
uint64_t dcc004_decoder_tables(int variant,int k) {
    if(variant<0||variant>=DCC004_NVARIANTS||k<1||k>256)return 0;
    switch(dcc004_variants[variant].coder){
    case 0:return (uint64_t)k*sizeof(HuffDec);
    case 1:case 2:return (uint64_t)k*sizeof(Tab);
    case 3:return (uint64_t)256*sizeof(Model);
    default:return 0;
    }
}
