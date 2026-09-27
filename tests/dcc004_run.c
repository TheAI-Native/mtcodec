#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "../source/dcc004.h"
#include "../source/dcc004_memory.h"
#include "dcc_bridge.h"
#define U64(X) ((unsigned long long)(X))
typedef struct { uint8_t *p;size_t n;Dcc004Meta m;uint64_t cost;int valid; } Gold;
static const char *const gens[]={"reassignment","brotli_native_policy","matched_merge","initialization_only"};
static const char *const cnames[]={"huffman","rans","rans_highbits","adaptive_order1"};
static void hex(FILE *f,const uint8_t *p){for(int i=0;i<256;++i)fprintf(f,"%02x",p[i]);}
static uint64_t fingerprint(const uint8_t *p,size_t n){uint64_t x=UINT64_C(14695981039346656037);for(size_t i=0;i<n;++i){x^=p[i];x*=UINT64_C(1099511628211);}return x;}
static int same(const uint8_t *a,size_t an,const uint8_t *b,size_t bn){return an==bn&&(!an||(a&&b&&!memcmp(a,b,an)));}
static int read_input(const char *path,uint8_t **p,size_t *n){
    FILE *f=fopen(path,"rb");long z;*p=NULL;*n=0;
    if(!f){perror(path);return 0;}
    if(fseek(f,0,SEEK_END)||(z=ftell(f))<0||(uint64_t)z>DCC003_MAX_INPUT||fseek(f,0,SEEK_SET)){fclose(f);return 0;}
    *n=(size_t)z;*p=(uint8_t*)malloc(*n?*n:1);if(!*p){fclose(f);return 0;}
    if(fread(*p,1,*n,f)!=*n||ferror(f)){free(*p);*p=NULL;fclose(f);return 0;}
    if(fclose(f)){free(*p);*p=NULL;return 0;}return 1;
}
static FILE *result(const char *prefix,const char *suffix){
    size_t n=strlen(prefix)+strlen(suffix)+1;char *s=(char*)malloc(n);FILE *f;if(!s)return NULL;
    snprintf(s,n,"%s%s",prefix,suffix);f=fopen(s,"wb");if(!f)perror(s);free(s);return f;
}
static int close_result(FILE *f){int ok;if(!f)return 0;ok=!ferror(f);if(fclose(f))ok=0;return ok;}
static uint64_t total_bits(const Dcc003Size *s){return s->count_bits+s->map_bits+s->table_bits+s->padding_bits+s->payload_bits+8*s->state_bytes;}
static int counts_expected(int v,int *cand,int *emits){
    const Dcc004Variant *a=&dcc004_variants[v];
    *cand=a->special==3?9:(a->special?1:12);
    *emits=(a->coder==0||a->selection==2)?1:*cand;return 1;
}
static int set_gold(Gold *g,const uint8_t *p,size_t n,const Dcc003Map *m,int cap,int coder,int gen,uint64_t cost){
    uint8_t *q=(uint8_t*)malloc(n?n:1);if(!q)return 0;if(n)memcpy(q,p,n);free(g->p);g->p=q;g->n=n;
    memset(&g->m,0,sizeof(g->m));g->m.decoder=coder;g->m.generator=gen;g->m.k=m->k;g->m.requested_k=cap;memcpy(g->m.map,m->map,256);
    g->cost=cost;g->valid=1;return 1;
}
static void selected_header(FILE *f){
    fputs("variant,coder,generator,selection,original_bytes,codec_body_bytes,requested_k,realized_k,candidate_count,emissions,decoder_table_bytes,count_bits,map_bits,table_bits,padding_bits,payload_bits,state_bytes,stream_fnv64,map_hex,status\n",f);
}
static int selected_row(FILE *f,int v,const uint8_t *input,size_t n,const Gold *g,int pass){
    const Dcc004Variant *a=&dcc004_variants[v];Dcc003Size z={0};
    if(!dcc004_stats(v,g->p,g->n,input,n,&z))pass=0;
    fprintf(f,"%s,%s,%s,%d,%zu,%zu,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%016llx,",
        a->name,cnames[a->coder],a->generator>=0?gens[a->generator]:"fixed_reference",a->selection,n,g->n,g->m.requested_k,g->m.k,g->m.candidates,g->m.emissions,
        U64(dcc004_decoder_tables(v,g->m.k)),U64(z.count_bits),U64(z.map_bits),U64(z.table_bits),U64(z.padding_bits),U64(z.payload_bits),U64(z.state_bytes),U64(fingerprint(g->p,g->n)));
    hex(f,g->m.map);fprintf(f,",%s\n",pass?"PASS":"FAIL");return pass;
}
static int verify_buffer(const uint8_t *data,size_t n,const char *prefix,int inject){
    uint8_t *scratch=NULL;Dcc003Input *in=NULL;Gold gold[DCC004_NVARIANTS]={{0}};
    FILE *cf=NULL,*sf=NULL;int fail=0,rows=0,picks=0;
    scratch=(uint8_t*)malloc(n?n:1);in=dcc003_input(data,n);
    cf=result(prefix,".candidates.csv");sf=result(prefix,".selected.csv");
    if(!scratch||!in||!cf||!sf){fail=1;goto done;}
    selected_header(sf);
    fputs("generator,coder,requested_k,realized_k,active_contexts,original_bytes,codec_body_bytes,count_bits,map_bits,table_bits,padding_bits,payload_bits,state_bytes,estimated_payload_units,decoded_equal,original_decoder_equal,accounting_ok,map_hex,status\n",cf);
    for(int gen=0;gen<4&&!fail;++gen)for(int coder=0;coder<2&&!fail;++coder)for(int ci=0;ci<12&&!fail;++ci){
        int cap=dcc003_counts[ci];Dcc003Map m={0};Dcc004Cost zc={0};Dcc003Size z={0};uint8_t *p=NULL;size_t len=0;
        int ok=dcc004_generate(in,coder,gen,cap,&m)&&dcc004_cost(in,coder,&m,&zc)&&dcc003_encode_map(in,coder,&m,&p,&len,&z);
        int dec=ok&&dcc003_decode(coder,p,len,n,scratch)&&(!n||!memcmp(scratch,data,n));
        int rd=ok&&dcc_ref_decode(coder,p,len,n,scratch)&&(!n||!memcmp(scratch,data,n));
        int ac=ok&&total_bits(&z)==8*(uint64_t)len&&z.table_bits==zc.table_bits&&z.map_bits==zc.map_bits&&z.count_bits==zc.count_bits;
        if(coder==0&&ok)ac=ac&&z.payload_bits*256==zc.payload_units;
        ok=ok&&dec&&rd&&ac;
        fprintf(cf,"%s,%s,%d,%d,%d,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,",gens[gen],cnames[coder],cap,m.k,m.active_contexts,n,len,U64(z.count_bits),U64(z.map_bits),U64(z.table_bits),U64(z.padding_bits),U64(z.payload_bits),U64(z.state_bytes),U64(zc.payload_units),dec,rd,ac);
        hex(cf,m.map);fprintf(cf,",%s\n",ok?"PASS":"FAIL");fflush(cf);rows++;
        if(ok){for(int v=0;v<DCC004_NVARIANTS;++v){const Dcc004Variant *a=&dcc004_variants[v];uint64_t rank;
            if(a->coder!=coder||a->generator!=gen||(a->special==1&&ci!=0)||a->special>1)continue;
            /* Independent ranks use serialized audit sizes, not encoder decisions. */
            if(coder==0)rank=a->selection==1?z.payload_bits:z.count_bits+z.map_bits+z.table_bits+z.payload_bits;
            else if(a->selection==0)rank=(uint64_t)len;
            else if(a->selection==1)rank=z.payload_bits;
            else rank=(z.count_bits+z.map_bits+z.table_bits+z.padding_bits+32)*256+zc.payload_units;
            if(!gold[v].valid||rank<gold[v].cost)if(!set_gold(&gold[v],p,len,&m,cap,coder,gen,rank))ok=0;
        }}
        free(p);if(!ok)fail=1;
    }
    /* Independently obtain the simple/high-bits/adaptive references. */
    for(int v=12;v<15&&!fail;++v){int choices=v==13?9:1;
        for(int j=0;j<choices;++j){uint8_t *p=NULL;size_t len=0;int k=0;Dcc003Map m={0};
            int method=v==14?3:2,param=v==14?1:j;
            if(!dcc_ref_encode(method,param,data,n,&p,&len,&k)){fail=1;break;}
            m.k=v==14?256:k;
            if(v!=14)for(int c=0;c<256;++c)m.map[c]=(uint8_t)(j?c>>(8-j):0);
            if(!gold[v].valid||len<gold[v].n)if(!set_gold(&gold[v],p,len,&m,m.k,method,-1,(uint64_t)len))fail=1;
            free(p);
        }
    }
    for(int v=0;v<DCC004_NVARIANTS&&!fail;++v){Gold actual={0};int ec=0,ee=0;
        int ok=gold[v].valid&&dcc004_encode(data,n,v,&actual.p,&actual.n,&actual.m);
        if(inject&&v==0&&ok&&actual.n)actual.p[actual.n-1]^=1;
        counts_expected(v,&ec,&ee);
        ok=ok&&same(gold[v].p,gold[v].n,actual.p,actual.n)&&actual.m.k==gold[v].m.k&&actual.m.requested_k==gold[v].m.requested_k&&!memcmp(actual.m.map,gold[v].m.map,256)&&actual.m.candidates==ec&&actual.m.emissions==ee;
        if(ok)ok=dcc004_decode(v,actual.p,actual.n,n,scratch)&&(!n||!memcmp(scratch,data,n));
        if(!selected_row(sf,v,data,n,&actual,ok))fail=1;
        fflush(sf);free(actual.p);picks++;
    }
 done:
    if(cf&&!close_result(cf))fail=1;
    if(sf&&!close_result(sf))fail=1;
    for(int v=0;v<DCC004_NVARIANTS;++v)free(gold[v].p);
    free(scratch);dcc003_input_free(in);
    if(rows!=96||picks!=DCC004_NVARIANTS)fail=1;
    printf("VERIFY: candidates=%d selected=%d failures=%d\n",rows,picks,fail);return fail?4:0;
}
static int verify(const char *path,const char *prefix,int inject){
    uint8_t *data=NULL;size_t n=0;int rc;
    if(!read_input(path,&data,&n))return 2;
    rc=verify_buffer(data,n,prefix,inject);free(data);return rc;
}
#ifndef DCC004_MEMORY
static uint64_t ticks_per_second=0;
static int clock_init(FILE *f){
#ifdef _WIN32
    LARGE_INTEGER q;DWORD_PTR proc=0,system=0,bit=0,old=0;
    if(!QueryPerformanceFrequency(&q)||q.QuadPart<=0)return 0;
    ticks_per_second=(uint64_t)q.QuadPart;
    if(!GetProcessAffinityMask(GetCurrentProcess(),&proc,&system)||!proc)return 0;
    bit=proc & (~proc+1);old=SetThreadAffinityMask(GetCurrentThread(),bit);if(!old)return 0;
    if(f)fprintf(f,"timer,frequency,process_affinity,selected_thread_affinity,previous_thread_affinity\nQPC,%llu,%llu,%llu,%llu\n",U64(ticks_per_second),U64(proc),U64(bit),U64(old));
#else
    ticks_per_second=UINT64_C(1000000000);
    if(f)fprintf(f,"timer,frequency,process_affinity,selected_thread_affinity,previous_thread_affinity\nCLOCK_MONOTONIC,%llu,unrestricted,unrestricted,unrestricted\n",U64(ticks_per_second));
#endif
    return 1;
}
static uint64_t ticks(void){
#ifdef _WIN32
    LARGE_INTEGER t;if(!QueryPerformanceCounter(&t)){fputs("QPC failed\n",stderr);exit(4);}return (uint64_t)t.QuadPart;
#else
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){perror("clock_gettime");exit(4);}return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
#endif
}
static uint32_t prng(uint32_t *x){*x^=*x<<13;*x^=*x>>17;*x^=*x<<5;return *x;}
static int batch(const uint8_t *data,size_t n,int task,size_t iterations,const Gold *g,uint8_t *scratch,uint64_t *elapsed,int inject){
    int v=task/2,phase=task%2,ok=1;uint8_t *p=NULL;size_t len=0;Dcc004Meta m={0};
    uint64_t a=ticks(),b;
    for(size_t i=0;i<iterations;++i){
        if(phase==0){free(p);p=NULL;len=0;if(!dcc004_encode(data,n,v,&p,&len,&m)){ok=0;break;}}
        else if(!dcc004_decode(v,g->p,g->n,n,scratch)){ok=0;break;}
    }
    b=ticks();*elapsed=b-a;
    /* All O(n) verification is strictly AFTER the ending timestamp. */
    if(inject){if(phase==0&&p&&len)p[len-1]^=1;else if(n)scratch[n-1]^=1;}
    if(phase==0)ok=ok&&same(p,len,g->p,g->n)&&m.k==g->m.k&&m.requested_k==g->m.requested_k&&!memcmp(m.map,g->m.map,256);
    else ok=ok&&(!n||!memcmp(scratch,data,n));
    free(p);return ok&&b>a;
}
static int double_cmp(const void *a,const void *b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y?1:0;}
static double quantile(const double *sorted,int n,double p){double x=(n-1)*p;int a=(int)x,b=a+1<n?a+1:a;return sorted[a]+(x-a)*(sorted[b]-sorted[a]);}
#endif
static int bench(const char *path,const char *prefix,uint32_t seed,int quick,int inject){
#ifdef DCC004_MEMORY
    (void)path;(void)prefix;(void)seed;(void)quick;(void)inject;
    fputs("Instrumented executable cannot provide timings.\n",stderr);return 4;
#else
    uint8_t *data=NULL,*scratch=NULL;size_t n=0,iterations[2*DCC004_NVARIANTS]={0};Gold gold[DCC004_NVARIANTS]={{0}};
    FILE *f=NULL,*sum=NULL,*meta=NULL,*sel=NULL;int fail=0,reps=quick?3:11,warm=quick?1:3;
    double target=quick?0.001:0.25,samples[2*DCC004_NVARIANTS][11]={{0}};int order[2*DCC004_NVARIANTS];
    if(!read_input(path,&data,&n))return 2;
    if(!n){free(data);return 2;}
    scratch=(uint8_t*)malloc(n);if(!scratch){free(data);return 2;}memset(scratch,0,n);
    f=result(prefix,".samples.csv");sum=result(prefix,".timing-summary.csv");meta=result(prefix,".clock.csv");sel=result(prefix,".timed-models.csv");
    if(!f||!sum||!meta||!sel||!clock_init(meta)){fail=1;goto done;}
    fputs("profile,variant,phase,stage,round,position,iterations,elapsed_ticks,timer_frequency,elapsed_seconds,seconds_per_operation,original_bytes,bytes_processed,below_target,verified\n",f);selected_header(sel);
    for(int v=0;v<DCC004_NVARIANTS;++v){if(!dcc004_encode(data,n,v,&gold[v].p,&gold[v].n,&gold[v].m)||!dcc004_decode(v,gold[v].p,gold[v].n,n,scratch)||memcmp(scratch,data,n)||!selected_row(sel,v,data,n,&gold[v],1)){fail=1;goto done;}}
    for(int task=0;task<2*DCC004_NVARIANTS;++task){size_t it=1;uint64_t dt=0;int attempt=0;
        for(;;){int ok=batch(data,n,task,it,&gold[task/2],scratch,&dt,inject&&task==0&&attempt==0);double secs=(double)dt/(double)ticks_per_second;
            fprintf(f,"%s,%s,%s,calibration,%d,%d,%zu,%llu,%llu,%.17g,%.17g,%zu,%llu,%d,%d\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",dcc004_variants[task/2].name,task%2?"decode":"encode",attempt,task,it,U64(dt),U64(ticks_per_second),secs,secs/(double)it,n,U64((uint64_t)n*it),secs<target,ok);fflush(f);
            if(!ok){fail=1;goto done;}if(secs>=target)break;
            if(it>=((size_t)1<<24)){fputs("Calibration iteration limit\n",stderr);fail=1;goto done;}it*=2;attempt++;
        }iterations[task]=it;
    }
    for(int round=0;round<warm+reps;++round){
        for(int i=0;i<2*DCC004_NVARIANTS;++i)order[i]=i;
        for(int i=2*DCC004_NVARIANTS-1;i>0;--i){int j=(int)(prng(&seed)%(uint32_t)(i+1));int t=order[i];order[i]=order[j];order[j]=t;}
        for(int pos=0;pos<2*DCC004_NVARIANTS;++pos){int task=order[pos];uint64_t dt=0;size_t it=iterations[task];
            int ok=batch(data,n,task,it,&gold[task/2],scratch,&dt,0);double secs=(double)dt/(double)ticks_per_second;
            if(round>=warm)samples[task][round-warm]=secs/(double)it;
            fprintf(f,"%s,%s,%s,%s,%d,%d,%zu,%llu,%llu,%.17g,%.17g,%zu,%llu,%d,%d\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",dcc004_variants[task/2].name,task%2?"decode":"encode",round<warm?"warmup":"measurement",round<warm?round:round-warm,pos,it,U64(dt),U64(ticks_per_second),secs,secs/(double)it,n,U64((uint64_t)n*it),secs<target,ok);fflush(f);
            if(!ok){fail=1;goto done;}
        }
        printf("%s round %d complete\n",round<warm?"Warmup":"Measurement",round<warm?round+1:round-warm+1);fflush(stdout);
    }
    fputs("profile,variant,phase,original_bytes,codec_body_bytes,iterations,repeats,q1_seconds,median_seconds,q3_seconds,median_MB_per_second,status\n",sum);
    for(int task=0;task<2*DCC004_NVARIANTS;++task){double q1,med,q3;qsort(samples[task],(size_t)reps,sizeof(double),double_cmp);q1=quantile(samples[task],reps,.25);med=quantile(samples[task],reps,.5);q3=quantile(samples[task],reps,.75);
        fprintf(sum,"%s,%s,%s,%zu,%zu,%zu,%d,%.17g,%.17g,%.17g,%.17g,PASS\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",dcc004_variants[task/2].name,task%2?"decode":"encode",n,gold[task/2].n,iterations[task],reps,q1,med,q3,(double)n/med/1e6);
    }
 done:
    if(f&&!close_result(f))fail=1;
    if(sum&&!close_result(sum))fail=1;
    if(meta&&!close_result(meta))fail=1;
    if(sel&&!close_result(sel))fail=1;
    for(int v=0;v<DCC004_NVARIANTS;++v)free(gold[v].p);
    free(data);free(scratch);
    printf("BENCH: profile=%s failures=%d\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",fail);return fail?4:0;
#endif
}
static int memory_buffer(const uint8_t *data,size_t n,const char *prefix){
#ifndef DCC004_MEMORY
    (void)data;(void)n;(void)prefix;fputs("Use separate instrumented executable for memory.\n",stderr);return 4;
#else
    uint8_t *scratch=NULL;int fail=0;FILE *f=NULL;
    scratch=(uint8_t*)malloc(n?n:1);f=result(prefix,".memory.csv");if(!scratch||!f){free(scratch);if(f)fclose(f);return 2;}
    fputs("variant,original_bytes,codec_body_bytes,requested_k,realized_k,stream_fnv64,encode_peak_requested_heap_bytes,encode_return_live_bytes,encode_after_release_delta,decode_peak_requested_heap_bytes,decode_after_return_delta,decoder_table_bytes,status\n",f);
    for(int v=0;v<DCC004_NVARIANTS&&!fail;++v){uint8_t *p=NULL;size_t len=0;Dcc004Meta m={0};uint64_t ep,er,ea,dp,da;int ok;
        dcc004_memory_begin();ok=dcc004_encode(data,n,v,&p,&len,&m);ep=dcc004_memory_peak();er=dcc004_memory_live_delta();
        /* Preserve the encoded result while measuring only fresh decoder allocations. */
        dcc004_memory_begin();ok=ok&&dcc004_decode(v,p,len,n,scratch);dp=dcc004_memory_peak();da=dcc004_memory_live_delta();
        ok=ok&&(!n||!memcmp(scratch,data,n));uint64_t fp=fingerprint(p,len);
        /* To verify encode allocation release, rerun it with a fresh tracker epoch. */
        free(p);p=NULL;dcc004_memory_begin();ok=ok&&dcc004_encode(data,n,v,&p,&len,&m);free(p);ea=dcc004_memory_live_delta();
        ok=ok&&ea==0&&da==0&&ep>=er&&dp==dcc004_decoder_tables(v,m.k);
        fprintf(f,"%s,%zu,%zu,%d,%d,%016llx,%llu,%llu,%llu,%llu,%llu,%llu,%s\n",dcc004_variants[v].name,n,len,m.requested_k,m.k,U64(fp),U64(ep),U64(er),U64(ea),U64(dp),U64(da),U64(dcc004_decoder_tables(v,m.k)),ok?"PASS":"FAIL");fflush(f);if(!ok)fail=1;
    }
    if(!close_result(f))fail=1;
    free(scratch);printf("MEMORY: failures=%d\n",fail);return fail?4:0;
#endif
}
static int memory_run(const char *path,const char *prefix){
    uint8_t *data=NULL;size_t n=0;int rc;
    if(!read_input(path,&data,&n))return 2;
    rc=memory_buffer(data,n,prefix);free(data);return rc;
}
static int unit(const char *path){
    FILE *f=fopen(path,"wb");int fail=0,count=0;uint8_t input[]={0,7,0,9};Dcc003Input *in=dcc003_input(input,sizeof(input));Dcc003Map m={0};Dcc004Meta meta={0};uint8_t *out=NULL;size_t len=0;
    if(!f||!in){if(f)fclose(f);dcc003_input_free(in);return 2;}fputs("test,passed,status\n",f);
#define CHECK(N,E) do{int pass=!!(E);fprintf(f,"%s,%d,%s\n",N,pass,pass?"PASS":"FAIL");count++;if(!pass)fail=1;}while(0)
    CHECK("init_frequency_order",dcc004_generate(in,0,3,2,&m)&&m.k==2&&m.map[0]==0&&m.map[7]==1&&m.map[9]==0);
    CHECK("init_cap_above_active",dcc004_generate(in,0,3,64,&m)&&m.k==2);
    CHECK("init_reject_null",!dcc004_generate(NULL,0,3,1,&m));
    CHECK("init_reject_cap_zero",!dcc004_generate(in,0,3,0,&m));
    CHECK("encode_reject_variant",!dcc004_encode(input,4,DCC004_NVARIANTS,&out,&len,&meta));
    CHECK("encode_reject_null",!dcc004_encode(NULL,4,0,&out,&len,&meta));
    CHECK("encode_reject_length",!dcc004_encode(input,DCC003_MAX_INPUT+1,0,&out,&len,&meta));
    CHECK("encode_reject_null_output",!dcc004_encode(input,4,0,NULL,&len,&meta));
    CHECK("decode_reject_variant",!dcc004_decode(-1,input,4,4,input));
    for(int v=0;v<DCC004_NVARIANTS;++v){int ca=0,em=0;uint8_t scratch[4]={0};char name[64];counts_expected(v,&ca,&em);
        int ok=dcc004_encode(input,4,v,&out,&len,&meta)&&meta.candidates==ca&&meta.emissions==em&&dcc004_decode(v,out,len,4,scratch)&&!memcmp(scratch,input,4);free(out);out=NULL;
        snprintf(name,sizeof(name),"variant_%02d_counts_and_decode",v);CHECK(name,ok);
    }
#undef CHECK
    dcc003_input_free(in);if(!close_result(f))fail=1;printf("UNIT004: tests=%d failures=%d\n",count,fail);return fail?4:0;
}
int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--unit"))return unit(argv[2]);
    if((argc==4||argc==5)&&!strcmp(argv[1],"--verify"))return verify(argv[2],argv[3],argc==5&&!strcmp(argv[4],"--inject-mismatch"));
    if(argc==4&&!strcmp(argv[1],"--memory"))return memory_run(argv[2],argv[3]);
    if(argc>=5&&argc<=7&&!strcmp(argv[1],"--bench")){
        char *end=NULL;unsigned long x;int quick=0,inject=0;errno=0;x=strtoul(argv[4],&end,10);
        if(errno||!end||*end||x>100000)return 1;
        for(int i=5;i<argc;++i){if(!strcmp(argv[i],"--quick"))quick=1;else if(!strcmp(argv[i],"--inject-mismatch"))inject=1;else return 1;}
        return bench(argv[2],argv[3],UINT32_C(0xDCC00401)^(uint32_t)x,quick,inject);
    }
    fputs("DCC004: --unit out.csv | --verify input prefix [--inject-mismatch] | --memory input prefix | --bench input prefix ordinal [--quick] [--inject-mismatch]\n",stderr);return 1;
}
