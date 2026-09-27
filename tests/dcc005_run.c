/* DCC005 workload driver. Uses the pinned DCC004 paths with two registry additions.
 * A workload is ONE whole file, or up to 20 independently modeled sample blocks.
 * No statistics/model/map/state is carried across members. This is not a new
 * compressed container: we report the sum of independent codec bodies.
 */
#define main dcc005_preserved_validation_main
#include "dcc004_run.c"
#undef main
#include <limits.h>
#define MAX_MEMBERS 20
#define MAX_TASKS 24
#define MAX_TIMED_VARIANTS 12

typedef struct {
    char id[100]; uint8_t *data; size_t n; uint64_t offset,fp;
} Member;
typedef struct { Member member[MAX_MEMBERS];int count;size_t total; } Workload;
#ifndef DCC004_MEMORY
static const int symmetric_variants[]={1,10,3,15,5,16};
static const int block_variants[]={0,2,4,1,10,3,15,5,16,11,12,13};
#endif

static void workload_free(Workload *w){for(int i=0;i<w->count;++i)free(w->member[i].data);memset(w,0,sizeof(*w));}
static int u64_parse(const char *s,uint64_t *v,int base){
    char *end=NULL;unsigned long long z;
    if(!s||!*s||*s=='-'||*s=='+'||*s==' '||*s=='\t')return 0;
    errno=0;z=strtoull(s,&end,base);if(errno||!end||*end)return 0;*v=(uint64_t)z;return 1;
}
static int workload_load(const char *list,Workload *w){
    char line[8192]={0};FILE *f;int ok=1;memset(w,0,sizeof(*w));f=fopen(list,"rb");if(!f){perror(list);return 0;}
    if(!fgets(line,sizeof(line),f)||strcmp(line,"member\toffset\tlength\tfnv64\tpath\n")){
        /* Also accept the explicitly recorded CRLF list representation. */
        if(strcmp(line,"member\toffset\tlength\tfnv64\tpath\r\n")){fclose(f);return 0;}
    }
    while(ok&&fgets(line,sizeof(line),f)){
        char *field[5];size_t nn=strlen(line);uint64_t offset=0,len=0,expected=0;FILE *in=NULL;long end=0;Member *m;
        if(!nn||line[nn-1]!='\n'){ok=0;break;}while(nn&&(line[nn-1]=='\n'||line[nn-1]=='\r'))line[--nn]=0;
        field[0]=line;for(int j=1;j<5;++j){char *t=strchr(field[j-1],'\t');if(!t){ok=0;break;}*t=0;field[j]=t+1;}
        if(!ok||strchr(field[4],'\t')||w->count>=MAX_MEMBERS){ok=0;break;}
        if(!*field[0]||strlen(field[0])>=sizeof(w->member[0].id)||!u64_parse(field[1],&offset,10)||!u64_parse(field[2],&len,10)||!u64_parse(field[3],&expected,16)||!*field[4]){ok=0;break;}
        for(char *c=field[0];*c;++c)if(!((*c>='0'&&*c<='9')||(*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||*c=='-'||*c=='_'))ok=0;
        for(int j=0;j<w->count;++j)if(!strcmp(w->member[j].id,field[0]))ok=0;
        if(!ok||offset>LONG_MAX||len>DCC003_MAX_INPUT||len>SIZE_MAX-w->total||offset+len>(uint64_t)LONG_MAX){ok=0;break;}
        in=fopen(field[4],"rb");if(!in){perror(field[4]);ok=0;break;}
        if(fseek(in,0,SEEK_END)||(end=ftell(in))<0||offset+len>(uint64_t)end||fseek(in,(long)offset,SEEK_SET)){fclose(in);ok=0;break;}
        m=&w->member[w->count];memset(m,0,sizeof(*m));snprintf(m->id,sizeof(m->id),"%s",field[0]);m->n=(size_t)len;m->offset=offset;
        m->data=(uint8_t*)malloc(m->n?m->n:1);if(!m->data){fclose(in);ok=0;break;}
        w->count++;w->total+=m->n;
        if(fread(m->data,1,m->n,in)!=m->n||ferror(in)){fclose(in);ok=0;break;}
        if(fclose(in)){ok=0;break;}m->fp=fingerprint(m->data,m->n);if(m->fp!=expected){fputs("Loaded member fingerprint mismatch\n",stderr);ok=0;break;}
    }
    if(ferror(f))ok=0;
    if(fclose(f))ok=0;
    if(!ok||!w->count){workload_free(w);fputs("Invalid workload or source range\n",stderr);return 0;}return 1;
}
static int loaded_record(const Workload *w,const char *prefix){
    FILE *f=result(prefix,".loaded.csv");if(!f)return 0;fputs("member,offset,length,fnv64,status\n",f);
    for(int i=0;i<w->count;++i)fprintf(f,"%s,%llu,%zu,%016llx,PASS\n",w->member[i].id,U64(w->member[i].offset),w->member[i].n,U64(w->member[i].fp));
    return close_result(f);
}
static int member_prefix(const char *prefix,const char *id,char *out,size_t cap){int n=snprintf(out,cap,"%s-%s",prefix,id);return n>0&&(size_t)n<cap;}
static int audit_workload(const char *list,const char *prefix,int memory,int inject){
    Workload w={0};int rc=0;if(!workload_load(list,&w))return 2;
    if(!loaded_record(&w,prefix)){workload_free(&w);return 2;}
    for(int i=0;i<w.count;++i){char name[8192];if(!member_prefix(prefix,w.member[i].id,name,sizeof(name))){rc=2;break;}
        printf("Member %d/%d: %s\n",i+1,w.count,w.member[i].id);fflush(stdout);
        rc=memory?memory_buffer(w.member[i].data,w.member[i].n,name):verify_buffer(w.member[i].data,w.member[i].n,name,inject&&i==0);
        if(rc)break;
    }
    workload_free(&w);return rc;
}
#ifndef DCC004_MEMORY
static int workload_batch(const Workload *w,int v,int phase,size_t it,const Gold *g,Gold *actual,uint8_t **scratch,uint64_t *dt,int inject){
    uint64_t begin,end;int ok=1;
    /* No final output from another batch enters this timed interval. */
    for(int j=0;j<w->count;++j){free(actual[j].p);memset(&actual[j],0,sizeof(actual[j]));}
    begin=ticks();
    for(size_t r=0;r<it&&ok;++r)for(int j=0;j<w->count&&ok;++j){
        const Member *m=&w->member[j];
        if(!phase){free(actual[j].p);actual[j].p=NULL;actual[j].n=0;ok=dcc004_encode(m->data,m->n,v,&actual[j].p,&actual[j].n,&actual[j].m);}
        else ok=dcc004_decode(v,g[j].p,g[j].n,m->n,scratch[j]);
    }
    end=ticks();*dt=end-begin;
    if(inject){if(!phase&&actual[0].p&&actual[0].n)actual[0].p[actual[0].n-1]^=1;else if(w->member[0].n)scratch[0][w->member[0].n-1]^=1;}
    /* Verify EVERY member of the final workload OUTSIDE the timed interval. */
    for(int j=0;j<w->count&&ok;++j){if(!phase)ok=same(actual[j].p,actual[j].n,g[j].p,g[j].n)&&actual[j].m.k==g[j].m.k&&actual[j].m.requested_k==g[j].m.requested_k&&!memcmp(actual[j].m.map,g[j].m.map,256)&&actual[j].m.emissions==g[j].m.emissions;
        else ok=!w->member[j].n||!memcmp(scratch[j],w->member[j].data,w->member[j].n);
    }
    for(int j=0;j<w->count;++j){free(actual[j].p);actual[j].p=NULL;}
    return ok&&end>begin;
}
static void sample_row(FILE *f,int quick,const char *name,int phase,const char *stage,int round,int pos,size_t it,uint64_t dt,const Workload *w,int ok){
    double secs=(double)dt/(double)ticks_per_second;fprintf(f,"%s,%s,%s,%s,%d,%d,%zu,%llu,%llu,%.17g,%.17g,%zu,%llu,%d,%d,%d\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",name,phase?"decode":"encode",stage,round,pos,it,U64(dt),U64(ticks_per_second),secs,secs/(double)it,w->total,U64((uint64_t)w->total*it),secs<(quick?0.001:0.25),ok,w->count);fflush(f);
}
#endif
static int bench_workload(const char *list,const char *prefix,const char *mode,uint32_t seed,int quick,int inject){
#ifdef DCC004_MEMORY
    (void)list;(void)prefix;(void)mode;(void)seed;(void)quick;(void)inject;
    fputs("Instrumented executable cannot provide timings.\n",stderr);return 4;
#else
    Workload w={0};Gold gold[MAX_TIMED_VARIANTS][MAX_MEMBERS]={{{0}}},actual[MAX_MEMBERS]={{0}};
    uint8_t *scratch[MAX_MEMBERS]={0};size_t iterations[MAX_TASKS]={0};double sample[MAX_TASKS][11]={{0}};
    FILE *f=NULL,*summary=NULL,*clock=NULL,*models=NULL;int fail=0,nv=0,nt=0,reps=quick?3:11,warm=quick?1:3;
    const int *vars=NULL;size_t lengths[MAX_TIMED_VARIANTS]={0};double target=quick?0.001:0.25;
    if(!strcmp(mode,"symmetric")){vars=symmetric_variants;nv=6;}else if(!strcmp(mode,"blocks")){vars=block_variants;nv=12;}else return 1;nt=2*nv;
    if(!workload_load(list,&w))return 2;
    if(!w.total){workload_free(&w);return 2;}
    if(!loaded_record(&w,prefix)){workload_free(&w);return 2;}
    for(int j=0;j<w.count;++j){scratch[j]=(uint8_t*)malloc(w.member[j].n?w.member[j].n:1);if(!scratch[j]){fail=1;goto done;}memset(scratch[j],0,w.member[j].n);}
    f=result(prefix,".samples.csv");summary=result(prefix,".timing-summary.csv");clock=result(prefix,".clock.csv");models=result(prefix,".timed-models.csv");
    if(!f||!summary||!clock||!models||!clock_init(clock)){fail=1;goto done;}
    fputs("profile,variant,phase,stage,round,position,iterations,elapsed_ticks,timer_frequency,elapsed_seconds,seconds_per_operation,original_bytes,bytes_processed,below_target,verified,members\n",f);
    fputs("member,variant,original_bytes,codec_body_bytes,requested_k,realized_k,emissions,stream_fnv64,map_hex,status\n",models);
    for(int vi=0;vi<nv;++vi)for(int j=0;j<w.count;++j){int v=vars[vi];Gold *g=&gold[vi][j];const Member *m=&w.member[j];
        if(!dcc004_encode(m->data,m->n,v,&g->p,&g->n,&g->m)||!dcc004_decode(v,g->p,g->n,m->n,scratch[j])||(m->n&&memcmp(scratch[j],m->data,m->n))){fail=1;goto done;}
        lengths[vi]+=g->n;fprintf(models,"%s,%s,%zu,%zu,%d,%d,%d,%016llx,",m->id,dcc004_variants[v].name,m->n,g->n,g->m.requested_k,g->m.k,g->m.emissions,U64(fingerprint(g->p,g->n)));hex(models,g->m.map);fputs(",PASS\n",models);fflush(models);
    }
    for(int task=0;task<nt;++task){size_t it=1;int attempt=0;uint64_t dt=0;
        for(;;){int vi=task/2,phase=task%2;int ok=workload_batch(&w,vars[vi],phase,it,gold[vi],actual,scratch,&dt,inject&&task==0&&attempt==0);
            double secs=(double)dt/(double)ticks_per_second;sample_row(f,quick,dcc004_variants[vars[vi]].name,phase,"calibration",attempt,task,it,dt,&w,ok);
            if(!ok){fail=1;goto done;}if(secs>=target)break;if(it>=((size_t)1<<24)){fail=1;goto done;}it*=2;attempt++;
        }iterations[task]=it;
    }
    for(int round=0;round<warm+reps;++round){int order[MAX_TASKS];for(int i=0;i<nt;++i)order[i]=i;
        for(int i=nt-1;i>0;--i){int j=(int)(prng(&seed)%(uint32_t)(i+1));int t=order[i];order[i]=order[j];order[j]=t;}
        for(int pos=0;pos<nt;++pos){int task=order[pos],vi=task/2,phase=task%2;uint64_t dt=0;
            int ok=workload_batch(&w,vars[vi],phase,iterations[task],gold[vi],actual,scratch,&dt,0);
            sample_row(f,quick,dcc004_variants[vars[vi]].name,phase,round<warm?"warmup":"measurement",round<warm?round:round-warm,pos,iterations[task],dt,&w,ok);
            if(!ok){fail=1;goto done;}if(round>=warm)sample[task][round-warm]=(double)dt/(double)ticks_per_second/(double)iterations[task];
        }
        printf("%s round %d complete\n",round<warm?"Warmup":"Measurement",round<warm?round+1:round-warm+1);fflush(stdout);
    }
    fputs("profile,variant,phase,original_bytes,codec_body_bytes,iterations,repeats,q1_seconds,median_seconds,q3_seconds,median_MB_per_second,members,status\n",summary);
    for(int task=0;task<nt;++task){int vi=task/2;double q1,med,q3;qsort(sample[task],(size_t)reps,sizeof(double),double_cmp);q1=quantile(sample[task],reps,.25);med=quantile(sample[task],reps,.5);q3=quantile(sample[task],reps,.75);
        fprintf(summary,"%s,%s,%s,%zu,%zu,%zu,%d,%.17g,%.17g,%.17g,%.17g,%d,PASS\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",dcc004_variants[vars[vi]].name,task%2?"decode":"encode",w.total,lengths[vi],iterations[task],reps,q1,med,q3,(double)w.total/med/1e6,w.count);
    }
 done:
    if(f&&!close_result(f))fail=1;
    if(summary&&!close_result(summary))fail=1;
    if(clock&&!close_result(clock))fail=1;
    if(models&&!close_result(models))fail=1;
    for(int vi=0;vi<MAX_TIMED_VARIANTS;++vi)for(int j=0;j<MAX_MEMBERS;++j)free(gold[vi][j].p);
    for(int j=0;j<MAX_MEMBERS;++j){free(actual[j].p);free(scratch[j]);}workload_free(&w);
    printf("BENCH005: %s failures=%d\n",quick?"QUICK_NOT_FOR_PUBLICATION":"FULL",fail);return fail?4:0;
#endif
}
int main(int argc,char **argv){
    if((argc==4||argc==5)&&!strcmp(argv[1],"--audit-list"))return audit_workload(argv[2],argv[3],0,argc==5&&!strcmp(argv[4],"--inject-mismatch"));
    if(argc==4&&!strcmp(argv[1],"--memory-list"))return audit_workload(argv[2],argv[3],1,0);
    if(argc>=6&&argc<=8&&!strcmp(argv[1],"--bench-list")){uint64_t ordinal=0;int quick=0,inject=0;if(!u64_parse(argv[5],&ordinal,10)||ordinal>100000)return 1;
        for(int i=6;i<argc;++i){if(!strcmp(argv[i],"--quick"))quick=1;else if(!strcmp(argv[i],"--inject-mismatch"))inject=1;else return 1;}
        return bench_workload(argv[2],argv[3],argv[4],UINT32_C(0xDCC00501)^(uint32_t)ordinal,quick,inject);
    }
    return dcc005_preserved_validation_main(argc,argv);
}
