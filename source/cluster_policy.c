/* DCC003: instantiate the SAME upstream cluster_inc.h three times in separate
 * objects. Native keeps both original cost functions. Matched variants replace
 * population cost and remove Brotli's entropy-map surrogate (our map is fixed
 * width and is charged exactly by the outer selector). No other algorithm edits.
 */
#include "dcc003.h"
#include "brotli_support.h"
#ifndef DCC_POLICY
#error Define DCC_POLICY: 0 native, 1 matched Huffman, 2 matched rANS.
#endif
#define FN(X) X ## Dcc
#define DATA_SIZE 256
#define DataType uint8_t
#include "../vendor/brotli-v1.1.0/histogram_inc.h"
#undef DataType
#undef DATA_SIZE
#if DCC_POLICY == 0
#include "../vendor/brotli-v1.1.0/bit_cost_inc.h"
static inline double ClusterCostDiff(size_t a,size_t b) {
    size_t c=a+b;
    return (double)a * FastLog2(a) + (double)b * FastLog2(b) - (double)c * FastLog2(c);
}
#define POLICY_NAME dcc003_native_policy
#elif DCC_POLICY == 1
static double BrotliPopulationCostDcc(const HistogramDcc *h) { return dcc003_huff_cost(h->data_); }
static inline double ClusterCostDiff(size_t a,size_t b) { (void)a; (void)b; return 0.0; }
#define POLICY_NAME dcc003_huff_policy
#elif DCC_POLICY == 2
static double BrotliPopulationCostDcc(const HistogramDcc *h) { return dcc003_rans_cost(h->data_); }
static inline double ClusterCostDiff(size_t a,size_t b) { (void)a; (void)b; return 0.0; }
#define POLICY_NAME dcc003_rans_policy
#else
#error Invalid DCC_POLICY.
#endif
#define CODE(X) X
/* Only the upstream constant-false OOM guards produce C4127 here. */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4127)
#endif
#include "../vendor/brotli-v1.1.0/cluster_inc.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef CODE
#undef FN

int POLICY_NAME(const uint32_t *counts,size_t rows,size_t cap,uint32_t *symbols,size_t *k) {
    HistogramDcc *in=NULL, *out=NULL;
    MemoryManager m={0}; uint64_t total=0;
    size_t i,s; int valid=1;
    if (!counts || !symbols || !k || rows<1 || rows>256 || cap<1 || cap>64) return 0;
    for (i=0; i<rows*256; ++i) total+=counts[i];
    if (total>DCC003_MAX_INPUT) return 0;
    in=(HistogramDcc*)malloc(rows*sizeof(*in));
    out=(HistogramDcc*)malloc(rows*sizeof(*out));
    if (!in || !out) { free(in); free(out); return 0; }
    for (i=0;i<rows;++i) {
        HistogramClearDcc(&in[i]);
        for (s=0;s<256;++s) { in[i].data_[s]=counts[256*i+s]; in[i].total_count_+=in[i].data_[s]; }
    }
    *k=0;
    BrotliClusterHistogramsDcc(&m,in,rows,cap,out,k,symbols);
    if (*k<1 || *k>cap) valid=0;
    for (i=0;i<rows;++i) if (symbols[i]>=*k) valid=0;
#ifndef DCC004_PRODUCTION
    /* Validation executable ONLY: not part of the timed production wrapper. */
    if (valid) {
        HistogramDcc *pooled=(HistogramDcc*)calloc(*k,sizeof(*pooled));
        if (!pooled) valid=0;
        else {
            for (i=0;i<rows;++i) HistogramAddHistogramDcc(&pooled[symbols[i]],&in[i]);
            for (i=0;i<*k;++i)
                if (pooled[i].total_count_!=out[i].total_count_ ||
                    memcmp(pooled[i].data_,out[i].data_,sizeof(out[i].data_))) valid=0;
            free(pooled);
        }
    }
#endif
    free(in); free(out); return valid;
}
