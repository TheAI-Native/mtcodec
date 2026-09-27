#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
/* DCC003-1.0. Correctness/serialization study, NOT a timing harness.
 * A competing method is never failed because its compressed output is larger.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "../source/dcc003.h"
#include "dcc_bridge.h"
#include "oracle_pools.h"
static const char *const generators[3]={"reassignment","brotli_native_policy","matched_merge"};
static const char *const coders[2]={"huffman","rans"};
static void hex_map(FILE *f,const uint8_t *p,size_t n) {
    size_t i;for(i=0;i<n;++i)fprintf(f,"%02x",p[i]);
}
static int same_bytes(const uint8_t *a,size_t an,const uint8_t *b,size_t bn) {
    return an==bn && (an==0 || (a && b && !memcmp(a,b,an)));
}
static int read_input(const char *path,uint8_t **p,size_t *n) {
    FILE *f=fopen(path,"rb"); long len;
    *p=NULL;*n=0;if(!f){perror(path);return 0;}
    if(fseek(f,0,SEEK_END)|| (len=ftell(f))<0 || (uint64_t)len>DCC003_MAX_INPUT || fseek(f,0,SEEK_SET)) {
        fprintf(stderr,"Cannot size/read input, or input exceeds 512 MiB: %s\n",path);fclose(f);return 0;
    }
    *n=(size_t)len;*p=(uint8_t*)malloc(*n?*n:1);
    if(!*p){fclose(f);return 0;}
    if(fread(*p,1,*n,f)!=*n || ferror(f)){free(*p);*p=NULL;fclose(f);return 0;}
    if(fclose(f)){free(*p);*p=NULL;return 0;}return 1;
}
static FILE *open_result(const char *prefix,const char *suffix) {
    size_t a=strlen(prefix),b=strlen(suffix);char *path=(char*)malloc(a+b+1);FILE *f;
    if(!path)return NULL;
    memcpy(path,prefix,a);memcpy(path+a,suffix,b+1);
    f=fopen(path,"wb");if(!f)perror(path);free(path);return f;
}
static int save_bytes(const char *prefix,const char *suffix,const uint8_t *p,size_t n) {
    FILE *f=open_result(prefix,suffix);int ok;if(!f)return 0;
    ok=(fwrite(p,1,n,f)==n);if(fclose(f))ok=0;return ok;
}
static uint64_t size_total(const Dcc003Size *s) {
    return s->count_bits+s->map_bits+s->table_bits+s->padding_bits+s->payload_bits+8*s->state_bytes;
}
static int oracle_check(const char *vectors,const char *result,int inject) {
    FILE *f=fopen(vectors,"rb"),*o=NULL;char line[2048];int failures=0,rows_seen=0,seen[12][12]={{0}};
    uint32_t *counts=(uint32_t*)calloc(256*256,sizeof(*counts));
    if(!f||!counts){if(f)fclose(f);free(counts);return 2;}
    o=fopen(result,"wb");if(!o){fclose(f);free(counts);return 2;}
    fputs("pool,rows,requested_k,expected_k,actual_k,partition_equal,status\n",o);
    if(!fgets(line,sizeof(line),f)||strcmp(line,"pool,rows,requested_k,realized_k,symbols_hex\n"))failures++;
    while(fgets(line,sizeof(line),f)) {
        int id,cap,kr,nrows,ci;char hex[1025];uint32_t sym[256],expected[256];size_t k=0,n;int ok=1;
        if(sscanf(line,"%d,%d,%d,%d,%1024s",&id,&nrows,&cap,&kr,hex)!=5 || id<0||id>=12 || nrows<1||nrows>256){failures++;break;}
        for(ci=0;ci<12&&dcc003_counts[ci]!=cap;++ci){}
        if(ci==12||seen[id][ci]++||strlen(hex)!=(size_t)nrows*2){failures++;break;}
        n=dcc_oracle_pool(id,counts);if(n!=(size_t)nrows)ok=0;
        for(size_t j=0;j<n;++j){unsigned int x=0;if(sscanf(hex+2*j,"%2x",&x)!=1)ok=0;expected[j]=(uint32_t)x;}
        if(inject && rows_seen==0)expected[0]^=1;
        if(!dcc003_native_policy(counts,n,(size_t)cap,sym,&k))ok=0;
        if(k!=(size_t)kr || memcmp(sym,expected,n*sizeof(sym[0])))ok=0;
        fprintf(o,"%d,%d,%d,%d,%zu,%d,%s\n",id,nrows,cap,kr,k,ok,ok?"PASS":"FAIL");
        if(!ok)failures++;
        rows_seen++;
    }
    if(ferror(f)||ferror(o)||rows_seen!=144)failures++;
    if(fclose(f))failures++;
    if(fclose(o))failures++;
    free(counts);printf("ORACLE: rows=%d failures=%d\n",rows_seen,failures);return failures?4:0;
}
static int unit_check(const char *path) {
    FILE *f=fopen(path,"wb");uint8_t bytes[4]={0,7,0,9};Dcc003Input *in;
    Dcc003Map m;uint32_t count[256]={0},symbols[256];size_t k=0;
    int tests=0,failures=0;
    if(!f)return 2;
    in=dcc003_input(bytes,sizeof(bytes));if(!in){fclose(f);return 2;}
    fputs("test,passed,status\n",f);
#define CHECK(NAME,COND) do { int pass=!!(COND);fprintf(f,"%s,%d,%s\n",NAME,pass,pass?"PASS":"FAIL");tests++;if(!pass)failures++; } while(0)
    CHECK("huffman_empty_header",dcc003_huff_cost(count)==14.0);
    CHECK("rans_empty_header",dcc003_rans_cost(count)==257.0);
    count[7]=1000;
    CHECK("huffman_singleton_cost",dcc003_huff_cost(count)==1034.0);
    CHECK("rans_singleton_cost",dcc003_rans_cost(count)==281.0);
    memset(count,0,sizeof(count));count[0]=count[1]=4;
    CHECK("huffman_two_symbol_cost",dcc003_huff_cost(count)==34.0);
    CHECK("rans_two_symbol_cost",dcc003_rans_cost(count)==309.0);
    CHECK("reject_null_input",!dcc003_generate(NULL,0,0,1,&m));
    CHECK("reject_null_map",!dcc003_generate(in,0,0,1,NULL));
    CHECK("reject_bad_coder",!dcc003_generate(in,2,0,1,&m));
    CHECK("reject_bad_generator",!dcc003_generate(in,0,3,1,&m));
    CHECK("reject_zero_cap",!dcc003_generate(in,0,1,0,&m));
    CHECK("reject_65_cap",!dcc003_generate(in,0,2,65,&m));
    CHECK("reject_null_bytes",dcc003_input(NULL,1)==NULL);
    CHECK("reject_oversized_bytes",dcc003_input(bytes,DCC003_MAX_INPUT+1)==NULL);
    CHECK("native_reject_null_counts",!dcc003_native_policy(NULL,1,1,symbols,&k));
    CHECK("native_reject_zero_rows",!dcc003_native_policy(count,0,1,symbols,&k));
    CHECK("native_reject_257_rows",!dcc003_native_policy(count,257,1,symbols,&k));
    CHECK("native_reject_65_cap",!dcc003_native_policy(count,1,65,symbols,&k));
    CHECK("huff_policy_reject_null",!dcc003_huff_policy(NULL,1,1,symbols,&k));
    CHECK("rans_policy_reject_null",!dcc003_rans_policy(NULL,1,1,symbols,&k));
    for(int coder=0;coder<2;++coder){
        uint8_t *p=NULL;size_t len=0;Dcc003Size sz;int ok;
        memset(&m,0,sizeof(m));m.k=2;m.map[255]=2;
        ok=!dcc003_encode_map(in,coder,&m,&p,&len,&sz);free(p);
        CHECK(coder==0?"huffman_reject_invalid_map":"rans_reject_invalid_map",ok);
    }
    {
        uint32_t pools[3*256]={0};
        pools[0]=pools[1]=pools[256]=pools[257]=1000;
        pools[512+200]=pools[512+201]=1000;
        CHECK("matched_huffman_merges_identical",dcc003_huff_policy(pools,3,2,symbols,&k)&&k==2&&symbols[0]==symbols[1]&&symbols[0]!=symbols[2]);
        CHECK("matched_rans_merges_identical",dcc003_rans_policy(pools,3,2,symbols,&k)&&k==2&&symbols[0]==symbols[1]&&symbols[0]!=symbols[2]);
        CHECK("matched_huffman_respects_one_cap",dcc003_huff_policy(pools,3,1,symbols,&k)&&k==1&&symbols[0]==0&&symbols[1]==0&&symbols[2]==0);
        CHECK("matched_rans_respects_one_cap",dcc003_rans_policy(pools,3,1,symbols,&k)&&k==1&&symbols[0]==0&&symbols[1]==0&&symbols[2]==0);
    }
#undef CHECK
    dcc003_input_free(in);if(ferror(f))failures++;if(fclose(f))failures++;
    printf("UNIT: tests=%d failures=%d\n",tests,failures);return failures?4:0;
}
static int case_check(const char *path,const char *prefix,int inject) {
    uint8_t *data=NULL,*scratch=NULL;size_t n=0;Dcc003Input *in=NULL;
    FILE *c=NULL,*s=NULL;int failed=0,candidate_rows=0,selected_rows=0,injected=0;
    size_t single_bytes[2]={0,0};
    uint8_t native_maps[12][256];int native_k[12]={0};
    if(!read_input(path,&data,&n))return 2;
    in=dcc003_input(data,n);scratch=(uint8_t*)malloc(n?n:1);
    c=open_result(prefix,".candidates.csv");s=open_result(prefix,".selected.csv");
    if(!in||!scratch||!c||!s){failed=1;goto cleanup;}
    fputs("generator,coder,requested_k,realized_k,active_contexts,original_bytes,codec_body_bytes,count_bits,map_bits,table_bits,padding_bits,payload_bits,state_bytes,map_repeat_equal,stream_repeat_equal,decoded_equal,original_decoder_equal,accounting_ok,original_stream_equal,native_map_coder_independent,map_hex,status\n",c);
    fputs("generator,coder,selected_requested_k,realized_k,original_bytes,codec_body_bytes,selection_cost,single_table_bytes,single_table_bound,original_selected_equal,map_hex,status\n",s);
    for(int gen=0;gen<3&&!failed;++gen)for(int coder=0;coder<2&&!failed;++coder){
        uint8_t *best=NULL;size_t best_len=0;uint64_t best_cost=UINT64_MAX;
        Dcc003Map best_map = {0};int best_cap=0;
        for(int ci=0;ci<12&&!failed;++ci){
            int cap=dcc003_counts[ci],original_equal=-1,map_coder_equal=-1;
            Dcc003Map m = {0},again = {0};Dcc003Size z,z2;uint8_t *a=NULL,*b=NULL;size_t an=0,bn=0;
            int okmap=dcc003_generate(in,coder,gen,cap,&m)&&dcc003_generate(in,coder,gen,cap,&again);
            int maprepeat=okmap&&m.k==again.k&&m.active_contexts==again.active_contexts&&!memcmp(m.map,again.map,256);
            int enc=maprepeat&&dcc003_encode_map(in,coder,&m,&a,&an,&z)&&dcc003_encode_map(in,coder,&again,&b,&bn,&z2);
            int repeat=0,dec=0,refdec=0,account=0;
            if(enc){
                if(inject&&!injected){b[bn-1]^=1;injected=1;
                    if(!save_bytes(prefix,".expected.bin",a,an)||!save_bytes(prefix,".modified.bin",b,bn))failed=1;}
                repeat=same_bytes(a,an,b,bn);
                memset(scratch,0xa5,n);dec=dcc003_decode(coder,a,an,n,scratch)&&(n==0||!memcmp(data,scratch,n));
                memset(scratch,0xa5,n);refdec=dcc_ref_decode(coder,a,an,n,scratch)&&(n==0||!memcmp(data,scratch,n));
                account=size_total(&z)==8*(uint64_t)an && z.padding_bits<8;
                if(gen==0){
                    uint8_t *orig=NULL;size_t olen=0;int k=0;
                    original_equal=dcc_ref_encode(coder,cap,data,n,&orig,&olen,&k)&&k==m.k&&same_bytes(a,an,orig,olen);free(orig);
                }
                if(gen==1){
                    if(coder==0){memcpy(native_maps[ci],m.map,256);native_k[ci]=m.k;map_coder_equal=1;}
                    else map_coder_equal=native_k[ci]==m.k&&!memcmp(native_maps[ci],m.map,256);
                }
                if(ci==0){
                    if(gen==0)single_bytes[coder]=an;
                    else if(an!=single_bytes[coder])failed=1;
                }
            } else {memset(&m,0,sizeof(m));memset(&z,0,sizeof(z));}
            int ok=enc&&maprepeat&&repeat&&dec&&refdec&&account&&original_equal!=0&&map_coder_equal!=0&&!failed;
            fprintf(c,"%s,%s,%d,%d,%d,%zu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%d,%d,%d,%d,%d,%d,%d,",
                generators[gen],coders[coder],cap,m.k,m.active_contexts,n,an,
                (unsigned long long)z.count_bits,(unsigned long long)z.map_bits,(unsigned long long)z.table_bits,
                (unsigned long long)z.padding_bits,(unsigned long long)z.payload_bits,(unsigned long long)z.state_bytes,
                maprepeat,repeat,dec,refdec,account,original_equal,map_coder_equal);
            hex_map(c,m.map,256);fprintf(c,",%s\n",ok?"PASS":"FAIL");fflush(c);candidate_rows++;
            if(ok){
                uint64_t cost=coder==0 ? z.count_bits+z.map_bits+z.table_bits+z.payload_bits : (uint64_t)an;
                if(best_cap==0||cost<best_cost){free(best);best=a;best_len=an;best_cost=cost;best_map=m;best_cap=cap;a=NULL;}
            }else{fprintf(stderr,"Candidate failed: %s/%s k=%d\n",generators[gen],coders[coder],cap);failed=1;}
            free(a);free(b);
        }
        if(!failed){
            int bound=best_len<=single_bytes[coder],equal=-1;
            if(gen==0){uint8_t *orig=NULL;size_t olen=0;int k=0;
                equal=dcc_ref_encode(coder,0,data,n,&orig,&olen,&k)&&k==best_map.k&&same_bytes(best,best_len,orig,olen);free(orig);}
            int ok=bound&&equal!=0;
            fprintf(s,"%s,%s,%d,%d,%zu,%zu,%llu,%zu,%d,%d,",generators[gen],coders[coder],best_cap,best_map.k,n,best_len,
                    (unsigned long long)best_cost,single_bytes[coder],bound,equal);
            hex_map(s,best_map.map,256);fprintf(s,",%s\n",ok?"PASS":"FAIL");fflush(s);selected_rows++;
            if(!ok)failed=1;
        }
        free(best);
    }
cleanup:
    if(c){if(ferror(c))failed=1;if(fclose(c))failed=1;}
    if(s){if(ferror(s))failed=1;if(fclose(s))failed=1;}
    dcc003_input_free(in);free(data);free(scratch);
    if(candidate_rows!=72||selected_rows!=6)failed=1;
    printf("CASE: candidates=%d selected=%d failures=%d\n",candidate_rows,selected_rows,failed);
    return failed?4:0;
}
int main(int argc,char **argv){
    if(argc>=4&&!strcmp(argv[1],"--oracle"))return oracle_check(argv[2],argv[3],argc==5&&!strcmp(argv[4],"--inject-mismatch"));
    if(argc==3&&!strcmp(argv[1],"--unit"))return unit_check(argv[2]);
    if(argc==3|| (argc==4&&!strcmp(argv[3],"--inject-mismatch")))return case_check(argv[1],argv[2],argc==4);
    fputs("usage: dcc003_check input prefix [--inject-mismatch]\n       dcc003_check --oracle vectors.csv results.csv [--inject-mismatch]\n       dcc003_check --unit results.csv\n",stderr);return 1;
}
