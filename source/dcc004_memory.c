/* Single-thread requested-byte accounting. Compile WITHOUT forced redirect.
 * Tracking-header bytes and allocator overhead are deliberately excluded. */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "dcc004_memory.h"
#ifdef _WIN32
/* On the x64 target, malloc supplies 16-byte-aligned storage. A 16-byte
 * header preserves that alignment without requiring max_align_t from a
 * particular Windows C-runtime header. Bookkeeping is not part of the metric. */
typedef union { uint64_t alignment[2]; struct { size_t bytes; } info; } Header;
_Static_assert(sizeof(Header) == 16, "Windows x64 tracking header must be 16 bytes");
#else
typedef union { max_align_t alignment; struct { size_t bytes; } info; } Header;
#endif
static uint64_t live_bytes=0,peak_bytes=0,baseline_bytes=0;
static void adjust_add(size_t n){live_bytes+=(uint64_t)n;if(live_bytes>peak_bytes)peak_bytes=live_bytes;}
void *dcc004_malloc(size_t n){
    Header *h;if(n>SIZE_MAX-sizeof(*h))return NULL;
    h=(Header*)malloc(sizeof(*h)+(n?n:1));if(!h)return NULL;
    h->info.bytes=n;adjust_add(n);return (void*)(h+1);
}
void *dcc004_calloc(size_t n,size_t s){
    size_t bytes;void *p;if(s&&n>SIZE_MAX/s)return NULL;bytes=n*s;p=dcc004_malloc(bytes);
    if(p&&bytes)memset(p,0,bytes);
    return p;
}
void dcc004_free(void *p){if(p){Header *h=(Header*)p-1;live_bytes-=(uint64_t)h->info.bytes;free(h);}}
void *dcc004_realloc(void *p,size_t n){
    Header *h,*q;size_t old;
    if(!p)return dcc004_malloc(n);
    if(!n){dcc004_free(p);return NULL;}
    if(n>SIZE_MAX-sizeof(*h))return NULL;
    h=(Header*)p-1;old=h->info.bytes;q=(Header*)realloc(h,sizeof(*h)+n);if(!q)return NULL;
    q->info.bytes=n;live_bytes-=(uint64_t)old;adjust_add(n);return (void*)(q+1);
}
void dcc004_memory_begin(void){baseline_bytes=live_bytes;peak_bytes=live_bytes;}
uint64_t dcc004_memory_peak(void){return peak_bytes-baseline_bytes;}
uint64_t dcc004_memory_live_delta(void){if(live_bytes<baseline_bytes){fputs("Memory tracker underflow\n",stderr);exit(4);}return live_bytes-baseline_bytes;}
