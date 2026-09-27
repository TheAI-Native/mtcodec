#define _CRT_SECURE_NO_WARNINGS

/* DCC002: behavior-preserving map/encoding separation and test-status fixes.
 * Algorithms, normalization, candidate order, and byte formats are unchanged.
 * See DCC002/CHANGES.txt and the differential regression evidence.
 */
#define MT_DCC002_VERSION "DCC002-1.0"

/*============================================================================
 *  mtcodec.c  --  Multi-table entropy codec: context-Huffman AND context-rANS
 *
 *  A merge of two programs:
 *
 *    r6c.c            multi-table static rANS (table = high bits of the
 *                     previous byte) + adaptive range coder, inside a
 *                     candidate-selection container.
 *    best_program.py  multi-tree canonical Huffman; tree = cmap[previous
 *                     byte], where the 256 contexts are clustered into k
 *                     trees by a k-means-style search and k is chosen per
 *                     file by exact total cost (header + payload).
 *
 *  In both coders the table/tree used for a symbol is a deterministic
 *  function of the previously decoded byte, so table selection costs ZERO
 *  bits in the stream; only the headers are paid for.
 *
 *  Container (superset of r6c's, so files written by r6c still decode):
 *
 *      'R','6','C','1' | ver u8 | orig_len u32 LE | inner_mode u8 | payload
 *
 *      inner_mode 0  stored
 *      inner_mode 1  multi-table rANS          (identical to r6c mode 1)
 *      inner_mode 2  adaptive range coder      (identical to r6c mode 2)
 *      inner_mode 4  clustered multi-table rANS (NEW; same header layout as
 *                                               mode 3 with rANS tables)
 *      inner_mode 3  multi-table Huffman       (NEW; payload is bit-for-bit
 *                                               the "body" that
 *                                               best_program.py emits after
 *                                               its own 5-byte header)
 *
 *  Mode 3 payload, one MSB-first bitstream:
 *      u8   k                      number of trees (encoder emits 1..64)
 *      if k > 1: 256 * ceil(log2 k) bits       cmap[prev] = tree id
 *      k tree headers, each an RLE list of 256 code lengths:
 *          6-bit token; nonzero = code length of next symbol;
 *          zero = next 8 bits hold (run-1) of zero lengths
 *      canonical Huffman codes; context "prev" starts at 0
 *
 *  Build (portable C99):
 *      gcc -O2 -std=c99 -o mtcodec mtcodec.c
 *      cl  /O2 mtcodec.c
 *
 *  Usage:
 *      mtcodec c h <in> <out>    compress, multi-table Huffman
 *      mtcodec c r <in> <out>    compress, multi-table rANS (clustered or
 *                                high-bits partition, whichever is smaller)
 *      mtcodec c a <in> <out>    compress, auto: every candidate, keep best
 *      mtcodec d <in> <out>      decompress (method is read from the file)
 *      mtcodec t <in>            self-test: round-trip h, r and a in memory
 *      mtcodec b <in>            bench: every configuration, verified
 *      mtcodec s [dir ...]       corpus run: write <dir>.csv per folder plus
 *                                averages.csv (default folders: calgary,
 *                                canterbury, large).  Running mtcodec with
 *                                no arguments at all does the same thing.
 *
 *  Whatever method is requested, `stored` remains the floor, so output is
 *  never larger than input + 10 bytes.
 *
 *  Public domain / CC0. No warranty.
 *==========================================================================*/

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L           /* opendir/stat under -std=c99    */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/*--------------------------------------------------------------------------*/
/* inner modes and compression methods                                      */
/*--------------------------------------------------------------------------*/
#define MODE_STORED   0
#define MODE_RANS     1
#define MODE_ADAPT    2
#define MODE_HUFF     3
#define MODE_RANS_MT  4                   /* clustered multi-table rANS     */

#define METHOD_HUFF   0
#define METHOD_RANS   1
#define METHOD_AUTO   2

#define CONTAINER_VER 2                   /* r6c wrote 1; we read 1 and 2  */

/*--------------------------------------------------------------------------*/
/* growable byte buffer                                                     */
/*--------------------------------------------------------------------------*/
typedef struct
{
    uint8_t *p;
    size_t   len;
    size_t   cap;
} Buf;

static void buf_init(Buf *b)
{
    b->p   = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_free(Buf *b)
{
    free(b->p);
    b->p   = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_reserve(Buf *b, size_t need)
{
    size_t   nc;
    uint8_t *np;

    if (b->cap >= need)
    {
        return;
    }
    nc = b->cap ? b->cap * 2 : 64;
    while (nc < need)
    {
        nc *= 2;
    }
    np = (uint8_t *)realloc(b->p, nc);
    if (!np)
    {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    b->p   = np;
    b->cap = nc;
}

static void buf_push(Buf *b, uint8_t v)
{
    if (b->len == b->cap)
    {
        buf_reserve(b, b->len + 1);
    }
    b->p[b->len++] = v;
}

static void buf_append(Buf *b, const uint8_t *s, size_t n)
{
    if (n == 0)
    {
        return;
    }
    buf_reserve(b, b->len + n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/*--------------------------------------------------------------------------*/
/* MSB-first bit writer (64-bit accumulator; < 8 bits pending between calls)*/
/*--------------------------------------------------------------------------*/
typedef struct
{
    Buf     *b;
    uint64_t acc;
    int      nbits;
} BitW;

static void bw_init(BitW *w, Buf *b)
{
    w->b     = b;
    w->acc   = 0;
    w->nbits = 0;
}

static void bw_bits32(BitW *w, uint32_t v, int n)         /* n in 0..32 */
{
    if (n == 0)
    {
        return;
    }
    if (n < 32)
    {
        v &= ((uint32_t)1 << n) - 1;
    }
    w->acc    = (w->acc << n) | v;
    w->nbits += n;
    while (w->nbits >= 8)
    {
        w->nbits -= 8;
        buf_push(w->b, (uint8_t)(w->acc >> w->nbits));
    }
    w->acc &= ((uint64_t)1 << w->nbits) - 1;
}

static void bw_bits(BitW *w, uint64_t v, int n)           /* n in 0..64 */
{
    if (n > 32)
    {
        bw_bits32(w, (uint32_t)(v >> 32), n - 32);
        n = 32;
    }
    bw_bits32(w, (uint32_t)v, n);
}

static void bw_bit(BitW *w, int bit)
{
    bw_bits32(w, (uint32_t)(bit & 1), 1);
}

static void bw_flush(BitW *w)             /* pad final partial byte with 0s */
{
    if (w->nbits)
    {
        buf_push(w->b, (uint8_t)(w->acc << (8 - w->nbits)));
        w->acc   = 0;
        w->nbits = 0;
    }
}

static int bit_length(uint32_t v)
{
    int nb = 0;

    while (v)
    {
        nb++;
        v >>= 1;
    }
    return nb;
}

static void bw_gamma(BitW *w, uint32_t v)                 /* Elias-gamma, v>=1 */
{
    int nb = bit_length(v);

    bw_bits32(w, 0, nb - 1);
    bw_bits32(w, v, nb);
}

/*--------------------------------------------------------------------------*/
/* MSB-first bit reader. acc is left-aligned; reads past the end yield 0s   */
/* and are detected afterwards with br_overrun().                           */
/*--------------------------------------------------------------------------*/
typedef struct
{
    const uint8_t *p;
    size_t         len;
    size_t         pos;                   /* next byte to load              */
    uint64_t       acc;
    int            nbits;                 /* valid bits in acc              */
    uint64_t       used;                  /* bits consumed so far           */
} BitR;

static void br_init(BitR *r, const uint8_t *p, size_t len)
{
    r->p     = p;
    r->len   = len;
    r->pos   = 0;
    r->acc   = 0;
    r->nbits = 0;
    r->used  = 0;
}

static void br_refill(BitR *r)            /* afterwards nbits >= 57         */
{
    while (r->nbits <= 56)
    {
        uint64_t byte = 0;

        if (r->pos < r->len)
        {
            byte = r->p[r->pos];
        }
        r->pos++;
        r->acc   |= byte << (56 - r->nbits);
        r->nbits += 8;
    }
}

static void br_skip(BitR *r, int n)       /* n in 0..32, after a refill     */
{
    r->acc  <<= n;
    r->nbits -= n;
    r->used  += (uint64_t)n;
}

static uint32_t br_bits(BitR *r, int n)   /* n in 0..32                     */
{
    uint32_t v;

    if (n == 0)
    {
        return 0;
    }
    br_refill(r);
    v = (uint32_t)(r->acc >> (64 - n));
    br_skip(r, n);
    return v;
}

static int br_bit(BitR *r)
{
    return (int)br_bits(r, 1);
}

static uint32_t br_gamma(BitR *r)         /* returns 0 if malformed         */
{
    int      z = 0;
    int      i;
    uint32_t v = 1;

    while (br_bit(r) == 0)
    {
        if (++z > 31)
        {
            return 0;
        }
    }
    for (i = 0; i < z; ++i)
    {
        v = (v << 1) | (uint32_t)br_bit(r);
    }
    return v;
}

static void br_align(BitR *r)             /* drop to next byte boundary     */
{
    br_bits(r, (int)((8 - (r->used & 7)) & 7));
}

static size_t br_bytepos(const BitR *r)   /* valid after br_align           */
{
    return (size_t)(r->used >> 3);
}

static int br_overrun(const BitR *r)
{
    return r->used > (uint64_t)r->len * 8;
}

/*==========================================================================*/
/*                                                                          */
/*  CODER A: multi-table static rANS  (from r6c.c, inner_mode 1)            */
/*                                                                          */
/*==========================================================================*/
#define PROB_BITS   12
#define PROB_SCALE  (1u << PROB_BITS)     /* M = 4096                       */
#define RANS_L      (1u << 23)            /* lower bound of normalized state*/
#define FIXED_BITS  13                    /* a freq in [0,4096] needs 13    */

/* ctx_bits values tried by the encoder; k = 1 << ctx_bits tables.          */
/* (r6c.c tried only 0 and 2; the decoder has always accepted 0..8.)     */
static const int RANS_CTX_CHOICES[] =
{
    0, 1, 2, 3, 4, 5, 6, 7, 8
};
#define RANS_NCHOICES ((int)(sizeof RANS_CTX_CHOICES / sizeof RANS_CTX_CHOICES[0]))

typedef struct
{
    uint16_t freq[256];
    uint16_t cum[256];
    uint8_t  slot[PROB_SCALE];
    int      used;
} Tab;

/* counts[256] -> freq[256] summing to PROB_SCALE; every symbol with        */
/* count > 0 gets freq >= 1; an unused table stays all-zero.                */
static int rans_normalize(const uint32_t counts[256], uint16_t freq[256])
{
    uint64_t total = 0;
    uint32_t sum   = 0;
    int      maxi  = 0;
    uint16_t maxf  = 0;
    int      i;

    for (i = 0; i < 256; ++i)
    {
        total += counts[i];
    }
    if (total == 0)
    {
        memset(freq, 0, 256 * sizeof freq[0]);
        return 0;
    }
    for (i = 0; i < 256; ++i)
    {
        uint64_t f;

        if (counts[i] == 0)
        {
            freq[i] = 0;
            continue;
        }
        f = ((uint64_t)counts[i] * PROB_SCALE) / total;
        if (f == 0)
        {
            f = 1;
        }
        freq[i] = (uint16_t)f;
        sum    += freq[i];
        if (freq[i] > maxf)
        {
            maxf = freq[i];
            maxi = i;
        }
    }
    if (sum < PROB_SCALE)
    {
        freq[maxi] = (uint16_t)(freq[maxi] + (PROB_SCALE - sum));
    }
    else
    {
        while (sum > PROB_SCALE)          /* shave the largest freq > 1     */
        {
            int      bi = -1;
            uint16_t bf = 0;

            for (i = 0; i < 256; ++i)
            {
                if (freq[i] > 1 && freq[i] > bf)
                {
                    bf = freq[i];
                    bi = i;
                }
            }
            if (bi < 0)
            {
                break;
            }
            freq[bi]--;
            sum--;
        }
    }
    return 1;
}

static int rans_ctx_of(int prev, int ctx_bits)
{
    if (ctx_bits == 0)
    {
        return 0;
    }
    return prev >> (8 - ctx_bits);
}

/* one table: 1 flag bit (0 = fixed 13-bit, 1 = Elias-gamma of freq+1),     */
/* then whichever form is smaller                                           */
static void rans_write_table(BitW *w, const uint16_t freq[256])
{
    int fixed_bits = 256 * FIXED_BITS;
    int gamma_bits = 0;
    int i;

    for (i = 0; i < 256; ++i)
    {
        gamma_bits += 2 * bit_length((uint32_t)freq[i] + 1) - 1;
    }
    if (gamma_bits < fixed_bits)
    {
        bw_bit(w, 1);
        for (i = 0; i < 256; ++i)
        {
            bw_gamma(w, (uint32_t)freq[i] + 1);
        }
    }
    else
    {
        bw_bit(w, 0);
        for (i = 0; i < 256; ++i)
        {
            bw_bits32(w, freq[i], FIXED_BITS);
        }
    }
}

/* returns 0 if the table is malformed (sum must be 0 or PROB_SCALE)        */
static int rans_read_table(BitR *r, uint16_t freq[256])
{
    int      flag = br_bit(r);
    uint32_t sum  = 0;
    int      i;

    for (i = 0; i < 256; ++i)
    {
        uint32_t f;

        if (flag)
        {
            f = br_gamma(r);
            if (f == 0)
            {
                return 0;
            }
            f -= 1;
        }
        else
        {
            f = br_bits(r, FIXED_BITS);
        }
        if (f > PROB_SCALE)
        {
            return 0;
        }
        freq[i] = (uint16_t)f;
        sum    += f;
    }
    return sum == 0 || sum == PROB_SCALE;
}

/* cumulative + slot->symbol; caller guarantees sum(freq) is 0 or PROB_SCALE*/
static void rans_tab_build(Tab *t)
{
    uint32_t c = 0;
    int      s;

    for (s = 0; s < 256; ++s)
    {
        t->cum[s] = (uint16_t)c;
        memset(t->slot + c, s, t->freq[s]);
        c += t->freq[s];
    }
    t->used = (c == PROB_SCALE);
}

/* payload = ctx_bits u8 | k tables (bit-packed, byte aligned) |            */
/*           final state u32 LE | renormalization bytes                     */
static int rans_encode(const uint8_t *S, size_t n, int ctx_bits, Buf *out)
{
    int       k = 1 << ctx_bits;
    uint32_t (*counts)[256];
    Tab      *tabs;
    Buf       hdr;
    BitW      w;
    size_t    cap;
    uint8_t  *tmp;
    uint8_t  *ptr;
    uint32_t  state = RANS_L;
    int       prev  = 0;
    int       ok    = 1;
    int       c;
    size_t    i;

    counts = calloc((size_t)k, sizeof *counts);
    tabs   = calloc((size_t)k, sizeof *tabs);
    cap    = n + (n >> 1) + 1024;         /* worst case is 12 bits/symbol   */
    tmp    = malloc(cap);
    if (!counts || !tabs || !tmp)
    {
        free(counts);
        free(tabs);
        free(tmp);
        return 0;
    }

    for (i = 0; i < n; ++i)
    {
        counts[rans_ctx_of(prev, ctx_bits)][S[i]]++;
        prev = S[i];
    }
    for (c = 0; c < k; ++c)
    {
        rans_normalize(counts[c], tabs[c].freq);
        rans_tab_build(&tabs[c]);
    }

    buf_init(&hdr);
    buf_push(&hdr, (uint8_t)ctx_bits);
    bw_init(&w, &hdr);
    for (c = 0; c < k; ++c)
    {
        rans_write_table(&w, tabs[c].freq);
    }
    bw_flush(&w);

    /* encode in REVERSE so the decoder emits forward; the context of S[i]  */
    /* is S[i-1], known on both sides.                                      */
    ptr = tmp + cap;
    for (i = n; i-- > 0 && ok; )
    {
        int       pv = (i > 0) ? S[i - 1] : 0;
        Tab      *t  = &tabs[rans_ctx_of(pv, ctx_bits)];
        uint32_t  f  = t->freq[S[i]];
        uint32_t  cs = t->cum[S[i]];
        uint32_t  x  = state;
        uint32_t  x_max;

        if (f == 0)
        {
            ok = 0;
            break;
        }
        x_max = ((RANS_L >> PROB_BITS) << 8) * f;
        while (x >= x_max)
        {
            if (ptr <= tmp)
            {
                ok = 0;
                break;
            }
            *--ptr = (uint8_t)x;
            x >>= 8;
        }
        state = ((x / f) << PROB_BITS) + (x % f) + cs;
    }

    if (ok)
    {
        int s;

        buf_append(out, hdr.p, hdr.len);
        for (s = 0; s < 4; ++s)
        {
            buf_push(out, (uint8_t)(state >> (8 * s)));
        }
        buf_append(out, ptr, (size_t)((tmp + cap) - ptr));
    }
    free(tmp);
    free(counts);
    free(tabs);
    buf_free(&hdr);
    return ok;
}

static int rans_decode(const uint8_t *pay, size_t plen, size_t n, uint8_t *out)
{
    int            ctx_bits;
    int            k;
    int            c;
    int            s;
    int            prev = 0;
    int            ok   = 1;
    BitR           r;
    Tab           *tabs;
    size_t         pos;
    size_t         i;
    uint32_t       state = 0;
    const uint8_t *rp;
    const uint8_t *rend;

    if (plen < 1 || pay[0] > 8)
    {
        return 0;
    }
    ctx_bits = pay[0];
    k        = 1 << ctx_bits;
    tabs     = calloc((size_t)k, sizeof *tabs);
    if (!tabs)
    {
        return 0;
    }

    br_init(&r, pay + 1, plen - 1);
    for (c = 0; c < k; ++c)
    {
        if (!rans_read_table(&r, tabs[c].freq))
        {
            free(tabs);
            return 0;
        }
        rans_tab_build(&tabs[c]);
    }
    br_align(&r);
    pos = 1 + br_bytepos(&r);
    if (br_overrun(&r) || pos + 4 > plen)
    {
        free(tabs);
        return 0;
    }

    for (s = 0; s < 4; ++s)
    {
        state |= (uint32_t)pay[pos + s] << (8 * s);
    }
    rp   = pay + pos + 4;
    rend = pay + plen;

    for (i = 0; i < n && ok; ++i)
    {
        Tab      *t    = &tabs[rans_ctx_of(prev, ctx_bits)];
        uint32_t  slot = state & (PROB_SCALE - 1);
        int       sym;
        uint32_t  x;

        if (!t->used)
        {
            ok = 0;
            break;
        }
        sym    = t->slot[slot];
        out[i] = (uint8_t)sym;
        prev   = sym;
        x      = t->freq[sym] * (state >> PROB_BITS) + slot - t->cum[sym];
        while (x < RANS_L)
        {
            if (rp >= rend)
            {
                ok = 0;
                break;
            }
            x = (x << 8) | *rp++;
        }
        state = x;
    }

    /* free integrity check: the decoder must land exactly on the encoder's */
    /* initial state with every byte consumed                               */
    if (ok && (state != RANS_L || rp != rend))
    {
        ok = 0;
    }
    free(tabs);
    return ok;
}

/*==========================================================================*/
/*                                                                          */
/*  CODER B: multi-table canonical Huffman (port of best_program.py,        */
/*           inner_mode 3)                                                  */
/*                                                                          */
/*==========================================================================*/
#define HUFF_MAX_LEN         63           /* fits the 6-bit header field    */
#define HUFF_MAX_TREES       64           /* encoder limit; decoder: 255    */
#define HUFF_KMEANS_ITERS    8
#define HUFF_ABSENT_PENALTY  20           /* pseudo-length, absent symbols  */
#define HUFF_LUT_BITS        11           /* fast-path decode table         */

static const int HUFF_K_CANDIDATES[] =
{
    1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64
};
#define HUFF_NCANDIDATES ((int)(sizeof HUFF_K_CANDIDATES / sizeof HUFF_K_CANDIDATES[0]))

/*--------------------------------------------------------------------------*/
/* code lengths: plain Huffman via a min-heap keyed on (weight, node index).*/
/* The index tie-break reproduces Python's heapq-on-tuples order exactly,   */
/* which is what makes the output bit-identical to best_program.py.         */
/*--------------------------------------------------------------------------*/
typedef struct
{
    uint64_t w;
    int      idx;
} HeapItem;

static int heap_less(const HeapItem *a, const HeapItem *b)
{
    if (a->w != b->w)
    {
        return a->w < b->w;
    }
    return a->idx < b->idx;
}

static void heap_push(HeapItem *h, int *n, HeapItem v)
{
    int i = (*n)++;

    while (i > 0)
    {
        int parent = (i - 1) / 2;

        if (!heap_less(&v, &h[parent]))
        {
            break;
        }
        h[i] = h[parent];
        i    = parent;
    }
    h[i] = v;
}

static HeapItem heap_pop(HeapItem *h, int *n)
{
    HeapItem top  = h[0];
    HeapItem last = h[--(*n)];
    int      i    = 0;

    for (;;)
    {
        int child = 2 * i + 1;

        if (child >= *n)
        {
            break;
        }
        if (child + 1 < *n && heap_less(&h[child + 1], &h[child]))
        {
            child++;
        }
        if (!heap_less(&h[child], &last))
        {
            break;
        }
        h[i] = h[child];
        i    = child;
    }
    if (*n > 0)
    {
        h[i] = last;
    }
    return top;
}

static void huff_code_lengths(const uint64_t counts_in[256], uint8_t lens[256])
{
    uint64_t counts[256];
    int      i;

    memcpy(counts, counts_in, sizeof counts);
    for (;;)
    {
        HeapItem heap[256];
        int      hn = 0;
        int      leaf_sym[256];
        int      left[512];
        int      right[512];
        int      depth[512];
        int      nn = 0;                  /* nodes so far; leaves first     */
        int      nleaf;
        int      too_deep = 0;

        memset(lens, 0, 256);
        for (i = 0; i < 256; ++i)
        {
            if (counts[i])
            {
                HeapItem it;

                it.w         = counts[i];
                it.idx       = nn;
                leaf_sym[nn] = i;
                nn++;
                heap_push(heap, &hn, it);
            }
        }
        nleaf = nn;
        if (nleaf == 0)
        {
            return;
        }
        if (nleaf == 1)
        {
            lens[leaf_sym[0]] = 1;
            return;
        }
        while (hn > 1)
        {
            HeapItem a = heap_pop(heap, &hn);
            HeapItem b = heap_pop(heap, &hn);
            HeapItem m;

            left[nn]  = a.idx;
            right[nn] = b.idx;
            m.w       = a.w + b.w;
            m.idx     = nn;
            nn++;
            heap_push(heap, &hn, m);
        }
        /* children always have smaller indices than their parent, so one   */
        /* top-down sweep assigns every depth                               */
        depth[nn - 1] = 0;
        for (i = nn - 1; i >= nleaf; --i)
        {
            depth[left[i]]  = depth[i] + 1;
            depth[right[i]] = depth[i] + 1;
        }
        for (i = 0; i < nleaf; ++i)
        {
            if (depth[i] > HUFF_MAX_LEN)
            {
                too_deep = 1;
                break;
            }
            lens[leaf_sym[i]] = (uint8_t)depth[i];
        }
        if (!too_deep)
        {
            return;
        }
        for (i = 0; i < 256; ++i)         /* flatten and retry              */
        {
            if (counts[i])
            {
                counts[i] = (counts[i] + 1) >> 1;
            }
        }
    }
}

/* canonical codes, symbols ordered by (length, symbol value) */
static void huff_canonical_codes(const uint8_t lens[256], uint64_t codes[256])
{
    uint32_t bl[HUFF_MAX_LEN + 1];
    uint64_t next[HUFF_MAX_LEN + 1];
    uint64_t code = 0;
    int      l;
    int      s;

    memset(bl, 0, sizeof bl);
    for (s = 0; s < 256; ++s)
    {
        bl[lens[s]]++;
    }
    bl[0]   = 0;
    next[0] = 0;
    for (l = 1; l <= HUFF_MAX_LEN; ++l)
    {
        code    = (code + bl[l - 1]) << 1;
        next[l] = code;
    }
    for (s = 0; s < 256; ++s)
    {
        codes[s] = lens[s] ? next[lens[s]]++ : 0;
    }
}

/*--------------------------------------------------------------------------*/
/* tree header: RLE list of 256 code lengths                                */
/*--------------------------------------------------------------------------*/
static void huff_write_lengths(BitW *w, const uint8_t lens[256])
{
    int i = 0;

    while (i < 256)
    {
        if (lens[i] == 0)
        {
            int j = i;

            while (j < 256 && lens[j] == 0)
            {
                j++;
            }
            bw_bits32(w, 0, 6);
            bw_bits32(w, (uint32_t)(j - i - 1), 8);
            i = j;
        }
        else
        {
            bw_bits32(w, lens[i], 6);
            i++;
        }
    }
}

static uint64_t huff_lengths_cost_bits(const uint8_t lens[256])
{
    uint64_t bits = 0;
    int      i    = 0;

    while (i < 256)
    {
        if (lens[i] == 0)
        {
            while (i < 256 && lens[i] == 0)
            {
                i++;
            }
            bits += 14;
        }
        else
        {
            bits += 6;
            i++;
        }
    }
    return bits;
}

static void huff_read_lengths(BitR *r, uint8_t lens[256])
{
    int i = 0;

    while (i < 256)
    {
        uint32_t v = br_bits(r, 6);

        if (v == 0)
        {
            int run = (int)br_bits(r, 8) + 1;

            while (run-- > 0 && i < 256)
            {
                lens[i++] = 0;
            }
        }
        else
        {
            lens[i++] = (uint8_t)v;
        }
    }
}

/*--------------------------------------------------------------------------*/
/* order-1 statistics, stored sparsely per context                          */
/*--------------------------------------------------------------------------*/
typedef struct
{
    int      nsym[256];                   /* distinct successors of ctx     */
    uint8_t  sym[256][256];
    uint32_t cnt[256][256];
    uint64_t total[256];
} Bigrams;

static Bigrams *bigrams_build(const uint8_t *S, size_t n)
{
    uint32_t (*dense)[256] = calloc(256, sizeof *dense);
    Bigrams  *bg           = calloc(1, sizeof *bg);
    int       prev         = 0;
    int       ctx;
    int       s;
    size_t    i;

    if (!dense || !bg)
    {
        free(dense);
        free(bg);
        return NULL;
    }
    for (i = 0; i < n; ++i)
    {
        dense[prev][S[i]]++;
        prev = S[i];
    }
    for (ctx = 0; ctx < 256; ++ctx)
    {
        for (s = 0; s < 256; ++s)
        {
            if (dense[ctx][s])
            {
                int j = bg->nsym[ctx]++;

                bg->sym[ctx][j]  = (uint8_t)s;
                bg->cnt[ctx][j]  = dense[ctx][s];
                bg->total[ctx]  += dense[ctx][s];
            }
        }
    }
    free(dense);
    return bg;
}

/*--------------------------------------------------------------------------*/
/* MODEL SELECTION  (the EVOLVE-BLOCK of best_program.py)                   */
/*--------------------------------------------------------------------------*/
static void huff_cluster_lengths(const Bigrams *bg, const uint8_t cmap[256],
                                 int k, uint8_t lens[][256])
{
    int t;

    for (t = 0; t < k; ++t)
    {
        uint64_t sums[256];
        int      ctx;
        int      j;

        memset(sums, 0, sizeof sums);
        for (ctx = 0; ctx < 256; ++ctx)
        {
            if (cmap[ctx] == t)
            {
                for (j = 0; j < bg->nsym[ctx]; ++j)
                {
                    sums[bg->sym[ctx][j]] += bg->cnt[ctx][j];
                }
            }
        }
        huff_code_lengths(sums, lens[t]);
    }
}

static uint64_t huff_assignment_cost(const Bigrams *bg, int ctx,
                                     const uint8_t lens[256])
{
    uint64_t cost = 0;
    int      j;

    for (j = 0; j < bg->nsym[ctx]; ++j)
    {
        int l = lens[bg->sym[ctx][j]];

        cost += (uint64_t)bg->cnt[ctx][j] * (uint64_t)(l ? l : HUFF_ABSENT_PENALTY);
    }
    return cost;
}

/* cluster the 256 contexts into <= k trees; returns the number used */
static int huff_assign_contexts(const Bigrams *bg, int k, uint8_t cmap[256])
{
    uint8_t        lens[HUFF_MAX_TREES][256];
    int            active[256];
    int            order[256];
    int            used[HUFF_MAX_TREES];
    int            remap[HUFF_MAX_TREES];
    int            na = 0;
    int            k_eff = 0;
    int            ctx;
    int            it;
    int            i;
    int            t;

    memset(cmap, 0, 256);
    for (ctx = 0; ctx < 256; ++ctx)
    {
        if (bg->nsym[ctx])
        {
            active[na++] = ctx;
        }
    }
    if (na == 0)
    {
        return 1;
    }

    /* init: round-robin by descending context frequency (stable sort)      */
    for (i = 0; i < na; ++i)
    {
        int x = active[i];
        int j = i;

        while (j > 0 && bg->total[order[j - 1]] < bg->total[x])
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = x;
    }
    for (i = 0; i < na; ++i)
    {
        cmap[order[i]] = (uint8_t)(i % k);
    }

    for (it = 0; it < HUFF_KMEANS_ITERS; ++it)
    {
        int changed = 0;

        huff_cluster_lengths(bg, cmap, k, lens);
        for (i = 0; i < na; ++i)
        {
            int      best_t;
            uint64_t best_cost;

            ctx       = active[i];
            best_t    = cmap[ctx];
            best_cost = huff_assignment_cost(bg, ctx, lens[best_t]);
            for (t = 0; t < k; ++t)
            {
                uint64_t c;

                if (t == cmap[ctx])
                {
                    continue;
                }
                c = huff_assignment_cost(bg, ctx, lens[t]);
                if (c < best_cost)
                {
                    best_t    = t;
                    best_cost = c;
                }
            }
            if (best_t != cmap[ctx])
            {
                cmap[ctx] = (uint8_t)best_t;
                changed   = 1;
            }
        }
        if (!changed)
        {
            break;
        }
    }

    /* drop empty clusters, renumber densely */
    memset(used, 0, sizeof used);
    for (i = 0; i < na; ++i)
    {
        used[cmap[active[i]]] = 1;
    }
    for (t = 0; t < k; ++t)
    {
        remap[t] = used[t] ? k_eff++ : 0;
    }
    for (i = 0; i < na; ++i)
    {
        cmap[active[i]] = (uint8_t)remap[cmap[active[i]]];
    }
    return k_eff;
}

static int huff_cmap_bits(int k)
{
    int b = bit_length((uint32_t)(k - 1));

    return b < 1 ? 1 : b;
}

/* pick (k, cmap) minimising exact header + payload bits.                   */
/* force_k > 0 restricts the search to that single candidate (bench only).  */
static int huff_select_model(const Bigrams *bg, int force_k, uint8_t cmap[256])
{
    uint8_t        lens[HUFF_MAX_TREES][256];
    uint8_t        trial[256];
    uint64_t       best_total = 0;
    int            best_k     = 0;
    int            ci;

    for (ci = 0; ci < HUFF_NCANDIDATES; ++ci)
    {
        int      k = force_k > 0 ? force_k : HUFF_K_CANDIDATES[ci];
        int      k_eff;
        int      t;
        int      ctx;
        uint64_t total = 0;

        k_eff = huff_assign_contexts(bg, k, trial);
        huff_cluster_lengths(bg, trial, k_eff, lens);
        for (t = 0; t < k_eff; ++t)
        {
            total += huff_lengths_cost_bits(lens[t]);
        }
        if (k_eff > 1)
        {
            total += 256u * (uint64_t)huff_cmap_bits(k_eff);
        }
        for (ctx = 0; ctx < 256; ++ctx)
        {
            if (bg->nsym[ctx])
            {
                total += huff_assignment_cost(bg, ctx, lens[trial[ctx]]);
            }
        }
        if (best_k == 0 || total < best_total)
        {
            best_total = total;
            best_k     = k_eff;
            memcpy(cmap, trial, 256);
        }
        if (force_k > 0)
        {
            break;
        }
    }
    return best_k;
}

/*--------------------------------------------------------------------------*/
/* encoder                                                                  */
/*--------------------------------------------------------------------------*/
typedef struct
{
    uint8_t  lens[256];
    uint64_t codes[256];
} HuffEnc;

/* DCC002: externally supplied maps are validated, never reclustered here.
 * The caller must supply statistics for exactly S[0..n).  Context IDs need not
 * be dense or all active; every stored map entry must be in [0,k).
 */
static int dcc_context_map_valid(int k, const uint8_t cmap[256])
{
    int c;
    if (!cmap || k < 1 || k > HUFF_MAX_TREES) return 0;
    for (c = 0; c < 256; ++c)
        if (cmap[c] >= k) return 0;
    return 1;
}

static int huff_encode_with_map(const uint8_t *S, size_t n, const Bigrams *bg,
                                int k, const uint8_t cmap[256], Buf *out)
{
    uint8_t        lens[HUFF_MAX_TREES][256];
    const HuffEnc *ctab[256];
    HuffEnc       *enc;
    BitW           w;
    int            t;
    int            ctx;
    int            prev = 0;
    size_t         i;

    if (!bg || !out || (n && !S) || !dcc_context_map_valid(k, cmap))
    {
        return 0;
    }
    huff_cluster_lengths(bg, cmap, k, lens);

    enc = malloc((size_t)k * sizeof *enc);
    if (!enc)
    {
        return 0;
    }
    for (t = 0; t < k; ++t)
    {
        memcpy(enc[t].lens, lens[t], 256);
        huff_canonical_codes(enc[t].lens, enc[t].codes);
    }
    for (ctx = 0; ctx < 256; ++ctx)
    {
        ctab[ctx] = &enc[cmap[ctx]];
    }

    bw_init(&w, out);
    bw_bits32(&w, (uint32_t)k, 8);
    if (k > 1)
    {
        int b = huff_cmap_bits(k);

        for (ctx = 0; ctx < 256; ++ctx)
        {
            bw_bits32(&w, cmap[ctx], b);
        }
    }
    for (t = 0; t < k; ++t)
    {
        huff_write_lengths(&w, enc[t].lens);
    }
    for (i = 0; i < n; ++i)
    {
        const HuffEnc *e = ctab[prev];
        int            s = S[i];

        bw_bits(&w, e->codes[s], e->lens[s]);
        prev = s;
    }
    bw_flush(&w);

    free(enc);
    return 1;
}

/* Original entry point: the generator/selector and tie rules are unchanged. */
static int huff_encode(const uint8_t *S, size_t n, int force_k, Buf *out,
                       int *k_used)
{
    Bigrams *bg;
    uint8_t cmap[256];
    int k, ok;
    if (force_k < 0 || force_k > HUFF_MAX_TREES) return 0;
    bg = bigrams_build(S, n);
    if (!bg) return 0;
    k = huff_select_model(bg, force_k, cmap);
    ok = huff_encode_with_map(S, n, bg, k, cmap, out);
    free(bg);
    if (ok && k_used) *k_used = k;
    return ok;
}


/*--------------------------------------------------------------------------*/
/* decoder: HUFF_LUT_BITS-wide lookup for short codes, canonical            */
/* first-code walk for the rest                                             */
/*--------------------------------------------------------------------------*/
typedef struct
{
    int      maxlen;
    uint64_t first[HUFF_MAX_LEN + 1];     /* first canonical code per len   */
    uint32_t cnt[HUFF_MAX_LEN + 1];
    uint32_t off[HUFF_MAX_LEN + 1];       /* index into syms[] per len      */
    uint8_t  syms[256];                   /* symbols in canonical order     */
    uint16_t lut[1 << HUFF_LUT_BITS];     /* (len << 8) | sym; 0 = no entry */
} HuffDec;

/* returns 0 if the lengths oversubscribe the code space (Kraft sum > 1)    */
static int huff_dec_build(HuffDec *d, const uint8_t lens[256])
{
    uint64_t kraft = 0;
    uint64_t code  = 0;
    uint32_t idx   = 0;
    int      l;
    int      s;

    memset(d, 0, sizeof *d);
    for (s = 0; s < 256; ++s)
    {
        if (lens[s] > HUFF_MAX_LEN)
        {
            return 0;
        }
        if (lens[s])
        {
            d->cnt[lens[s]]++;
            if (lens[s] > d->maxlen)
            {
                d->maxlen = lens[s];
            }
            kraft += (uint64_t)1 << (HUFF_MAX_LEN - lens[s]);
            if (kraft > ((uint64_t)1 << HUFF_MAX_LEN))
            {
                return 0;
            }
        }
    }
    for (l = 1; l <= d->maxlen; ++l)
    {
        code        = (code + d->cnt[l - 1]) << 1;
        d->first[l] = code;
        d->off[l]   = idx;
        for (s = 0; s < 256; ++s)
        {
            if (lens[s] == l)
            {
                if (l <= HUFF_LUT_BITS)
                {
                    uint64_t c     = d->first[l] + (idx - d->off[l]);
                    uint32_t base  = (uint32_t)(c << (HUFF_LUT_BITS - l));
                    uint32_t span  = (uint32_t)1 << (HUFF_LUT_BITS - l);
                    uint32_t j;

                    for (j = 0; j < span; ++j)
                    {
                        d->lut[base + j] = (uint16_t)((l << 8) | s);
                    }
                }
                d->syms[idx++] = (uint8_t)s;
            }
        }
    }
    return 1;
}

static int huff_decode(const uint8_t *pay, size_t plen, size_t n, uint8_t *out)
{
    BitR           r;
    uint8_t        cmap[256];
    uint8_t        lens[256];
    const HuffDec *dtab[256];
    HuffDec       *decs;
    int            k;
    int            t;
    int            ctx;
    int            prev = 0;
    int            ok   = 1;
    size_t         i;

    br_init(&r, pay, plen);
    k = (int)br_bits(&r, 8);
    if (k == 0)
    {
        return 0;
    }
    memset(cmap, 0, sizeof cmap);
    if (k > 1)
    {
        int b = huff_cmap_bits(k);

        for (ctx = 0; ctx < 256; ++ctx)
        {
            uint32_t v = br_bits(&r, b);

            if (v >= (uint32_t)k)
            {
                return 0;
            }
            cmap[ctx] = (uint8_t)v;
        }
    }

    decs = malloc((size_t)k * sizeof *decs);
    if (!decs)
    {
        return 0;
    }
    for (t = 0; t < k; ++t)
    {
        huff_read_lengths(&r, lens);
        if (!huff_dec_build(&decs[t], lens))
        {
            free(decs);
            return 0;
        }
    }
    for (ctx = 0; ctx < 256; ++ctx)
    {
        dtab[ctx] = &decs[cmap[ctx]];
    }

    for (i = 0; i < n; ++i)
    {
        const HuffDec *d = dtab[prev];
        uint16_t       e;
        int            sym = -1;

        br_refill(&r);
        e = d->lut[r.acc >> (64 - HUFF_LUT_BITS)];
        if (e)
        {
            br_skip(&r, e >> 8);
            sym = e & 0xFF;
        }
        else
        {
            uint64_t code = 0;
            int      l;

            for (l = 1; l <= d->maxlen; ++l)
            {
                code = (code << 1) | (uint64_t)br_bit(&r);
                if (code >= d->first[l] && code - d->first[l] < d->cnt[l])
                {
                    sym = d->syms[d->off[l] + (uint32_t)(code - d->first[l])];
                    break;
                }
            }
            if (sym < 0)
            {
                ok = 0;
                break;
            }
        }
        out[i] = (uint8_t)sym;
        prev   = sym;
    }
    if (br_overrun(&r))
    {
        ok = 0;
    }
    free(decs);
    return ok;
}

/*==========================================================================*/
/*                                                                          */
/*  CODER D: clustered multi-table rANS (inner_mode 4)                      */
/*                                                                          */
/*  The Huffman coder's context clustering and exact-cost model selection,  */
/*  applied to rANS.  Table selection is again a deterministic function of  */
/*  the previous byte, so it costs zero bits in the stream.                 */
/*                                                                          */
/*  Mode 4 payload:                                                         */
/*      u8   k                        number of tables (encoder emits 1..32)*/
/*      if k > 1: 256 * ceil(log2 k) bits      cmap[prev] = table id       */
/*      k tables in the mode-1 format (1 flag bit + gamma or fixed-13)      */
/*      byte align | final state u32 LE | renormalization bytes             */
/*                                                                          */
/*  Costs are kept in 1/256-bit units: the price of symbol s under table t  */
/*  is 12 - log2(freq_t[s]) bits, and a symbol the table cannot code at all */
/*  gets the same pseudo-cost the Huffman clustering uses for an absent     */
/*  code, so both coders cluster with the same rules.                       */
/*==========================================================================*/
#define RANS_MAX_TABLES     HUFF_MAX_TREES
#define COST_FRAC_BITS      8
#define COST_ONE            (1u << COST_FRAC_BITS)

static uint32_t rans_cost_tab[PROB_SCALE + 1];   /* -log2(f/M), 1/256 bit  */

/* log2(f) in 1/256 units, f >= 1, by repeated squaring of the mantissa;    */
/* exact to the rounding of the last fractional bit, no libm needed.        */
static uint32_t log2_fixed(uint32_t f)
{
    int      ip   = bit_length(f) - 1;
    uint64_t m    = (uint64_t)f << (31 - ip);        /* mantissa in [2^31, 2^32) */
    uint32_t frac = 0;
    int      i;

    for (i = 0; i < COST_FRAC_BITS; ++i)
    {
        m = (m * m) >> 31;                           /* m*m < 2^64: no overflow */
        frac <<= 1;
        if (m >= ((uint64_t)1 << 32))
        {
            m >>= 1;
            frac |= 1;
        }
    }
    return ((uint32_t)ip << COST_FRAC_BITS) | frac;
}

static void rans_cost_init(void)
{
    uint32_t f;

    if (rans_cost_tab[1])
    {
        return;
    }
    rans_cost_tab[0] = HUFF_ABSENT_PENALTY * COST_ONE;
    for (f = 1; f <= PROB_SCALE; ++f)
    {
        rans_cost_tab[f] = ((uint32_t)PROB_BITS << COST_FRAC_BITS) - log2_fixed(f);
    }
}

/* bits used by rans_write_table for this table (flag + smaller form)       */
static uint64_t rans_table_cost_bits(const uint16_t freq[256])
{
    uint64_t gamma_bits = 0;
    int      i;

    for (i = 0; i < 256; ++i)
    {
        gamma_bits += 2 * bit_length((uint32_t)freq[i] + 1) - 1;
    }
    return 1 + (gamma_bits < 256 * FIXED_BITS ? gamma_bits : 256 * FIXED_BITS);
}

/* one normalized table per cluster from the bigram statistics              */
static void rans_cluster_tables(const Bigrams *bg, const uint8_t cmap[256],
                                int k, uint16_t freq[][256])
{
    int t;

    for (t = 0; t < k; ++t)
    {
        uint32_t sums[256];
        int      ctx;
        int      j;

        memset(sums, 0, sizeof sums);
        for (ctx = 0; ctx < 256; ++ctx)
        {
            if (cmap[ctx] == t)
            {
                for (j = 0; j < bg->nsym[ctx]; ++j)
                {
                    sums[bg->sym[ctx][j]] += bg->cnt[ctx][j];
                }
            }
        }
        rans_normalize(sums, freq[t]);
    }
}

static uint64_t rans_assignment_cost(const Bigrams *bg, int ctx,
                                     const uint16_t freq[256])
{
    uint64_t cost = 0;
    int      j;

    for (j = 0; j < bg->nsym[ctx]; ++j)
    {
        cost += (uint64_t)bg->cnt[ctx][j] * rans_cost_tab[freq[bg->sym[ctx][j]]];
    }
    return cost;
}

/* same procedure as huff_assign_contexts with the rANS cost metric        */
static int rans_assign_contexts(const Bigrams *bg, int k, uint8_t cmap[256])
{
    static uint16_t freq[RANS_MAX_TABLES][256];
    int             active[256];
    int             order[256];
    int             used[RANS_MAX_TABLES];
    int             remap[RANS_MAX_TABLES];
    int             na    = 0;
    int             k_eff = 0;
    int             ctx;
    int             it;
    int             i;
    int             t;

    memset(cmap, 0, 256);
    for (ctx = 0; ctx < 256; ++ctx)
    {
        if (bg->nsym[ctx])
        {
            active[na++] = ctx;
        }
    }
    if (na == 0)
    {
        return 1;
    }
    for (i = 0; i < na; ++i)
    {
        int x = active[i];
        int j = i;

        while (j > 0 && bg->total[order[j - 1]] < bg->total[x])
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = x;
    }
    for (i = 0; i < na; ++i)
    {
        cmap[order[i]] = (uint8_t)(i % k);
    }

    for (it = 0; it < HUFF_KMEANS_ITERS; ++it)
    {
        int changed = 0;

        rans_cluster_tables(bg, cmap, k, freq);
        for (i = 0; i < na; ++i)
        {
            int      best_t;
            uint64_t best_cost;

            ctx       = active[i];
            best_t    = cmap[ctx];
            best_cost = rans_assignment_cost(bg, ctx, freq[best_t]);
            for (t = 0; t < k; ++t)
            {
                uint64_t c;

                if (t == cmap[ctx])
                {
                    continue;
                }
                c = rans_assignment_cost(bg, ctx, freq[t]);
                if (c < best_cost)
                {
                    best_t    = t;
                    best_cost = c;
                }
            }
            if (best_t != cmap[ctx])
            {
                cmap[ctx] = (uint8_t)best_t;
                changed   = 1;
            }
        }
        if (!changed)
        {
            break;
        }
    }

    memset(used, 0, sizeof used);
    for (i = 0; i < na; ++i)
    {
        used[cmap[active[i]]] = 1;
    }
    for (t = 0; t < k; ++t)
    {
        remap[t] = used[t] ? k_eff++ : 0;
    }
    for (i = 0; i < na; ++i)
    {
        cmap[active[i]] = (uint8_t)remap[cmap[active[i]]];
    }
    return k_eff;
}

/* pick (k, cmap) minimising exact header bits + estimated payload bits     */
static int rans_select_model(const Bigrams *bg, int force_k, uint8_t cmap[256])
{
    static uint16_t freq[RANS_MAX_TABLES][256];
    uint8_t         trial[256];
    uint64_t        best_total = 0;
    int             best_k     = 0;
    int             ci;

    for (ci = 0; ci < HUFF_NCANDIDATES; ++ci)
    {
        int      k = force_k > 0 ? force_k : HUFF_K_CANDIDATES[ci];
        int      k_eff;
        int      t;
        int      ctx;
        uint64_t total = 0;

        k_eff = rans_assign_contexts(bg, k, trial);
        rans_cluster_tables(bg, trial, k_eff, freq);
        for (t = 0; t < k_eff; ++t)
        {
            total += rans_table_cost_bits(freq[t]) << COST_FRAC_BITS;
        }
        if (k_eff > 1)
        {
            total += (256u * (uint64_t)huff_cmap_bits(k_eff)) << COST_FRAC_BITS;
        }
        for (ctx = 0; ctx < 256; ++ctx)
        {
            if (bg->nsym[ctx])
            {
                total += rans_assignment_cost(bg, ctx, freq[trial[ctx]]);
            }
        }
        if (best_k == 0 || total < best_total)
        {
            best_total = total;
            best_k     = k_eff;
            memcpy(cmap, trial, 256);
        }
        if (force_k > 0)
        {
            break;
        }
    }
    return best_k;
}

static int rans_mt_encode_with_map(const uint8_t *S, size_t n, const Bigrams *bg,
                                   int k, const uint8_t cmap[256], Buf *out)
{
    static uint16_t freq[RANS_MAX_TABLES][256];
    Tab            *tabs;
    Buf             hdr;
    BitW            w;
    size_t          cap;
    uint8_t        *tmp;
    uint8_t        *ptr;
    uint32_t        state = RANS_L;
    int             ok    = 1;
    int             t;
    int             ctx;
    size_t          i;

    if (!bg || !out || (n && !S) || !dcc_context_map_valid(k, cmap))
    {
        return 0;
    }
    rans_cluster_tables(bg, cmap, k, freq);

    tabs = calloc((size_t)k, sizeof *tabs);
    cap  = n + (n >> 1) + 1024;
    tmp  = malloc(cap);
    if (!tabs || !tmp)
    {
        free(tabs);
        free(tmp);
        return 0;
    }
    for (t = 0; t < k; ++t)
    {
        memcpy(tabs[t].freq, freq[t], sizeof tabs[t].freq);
        rans_tab_build(&tabs[t]);
    }

    buf_init(&hdr);
    bw_init(&w, &hdr);
    bw_bits32(&w, (uint32_t)k, 8);
    if (k > 1)
    {
        int b = huff_cmap_bits(k);

        for (ctx = 0; ctx < 256; ++ctx)
        {
            bw_bits32(&w, cmap[ctx], b);
        }
    }
    for (t = 0; t < k; ++t)
    {
        rans_write_table(&w, tabs[t].freq);
    }
    bw_flush(&w);

    ptr = tmp + cap;
    for (i = n; i-- > 0 && ok; )
    {
        int       pv = (i > 0) ? S[i - 1] : 0;
        Tab      *tb = &tabs[cmap[pv]];
        uint32_t  f  = tb->freq[S[i]];
        uint32_t  cs = tb->cum[S[i]];
        uint32_t  x  = state;
        uint32_t  x_max;

        if (f == 0)
        {
            ok = 0;
            break;
        }
        x_max = ((RANS_L >> PROB_BITS) << 8) * f;
        while (x >= x_max)
        {
            if (ptr <= tmp)
            {
                ok = 0;
                break;
            }
            *--ptr = (uint8_t)x;
            x >>= 8;
        }
        state = ((x / f) << PROB_BITS) + (x % f) + cs;
    }

    if (ok)
    {
        int s;

        buf_append(out, hdr.p, hdr.len);
        for (s = 0; s < 4; ++s)
        {
            buf_push(out, (uint8_t)(state >> (8 * s)));
        }
        buf_append(out, ptr, (size_t)((tmp + cap) - ptr));
    }
    free(tmp);
    free(tabs);
    buf_free(&hdr);
    return ok;
}

static int rans_mt_encode_k(const uint8_t *S, size_t n, const Bigrams *bg,
                            int force_k, Buf *out, int *k_used)
{
    uint8_t cmap[256];
    int k, ok;
    if (force_k < 0 || force_k > RANS_MAX_TABLES) return 0;
    k = rans_select_model(bg, force_k, cmap);
    ok = rans_mt_encode_with_map(S, n, bg, k, cmap, out);
    if (ok && k_used) *k_used = k;
    return ok;
}


/* force_k > 0: encode with that clustering.  force_k == 0: every candidate */
/* k is clustered by estimated cost, then actually encoded, and the         */
/* smallest real stream wins; byte ties retain the first requested candidate*/
static int rans_mt_encode(const uint8_t *S, size_t n, int force_k, Buf *out,
                          int *k_used)
{
    Bigrams *bg;
    Buf      best;
    int      best_k = 0;
    int      ci;

    rans_cost_init();
    bg = bigrams_build(S, n);
    if (!bg)
    {
        return 0;
    }
    buf_init(&best);
    for (ci = 0; ci < HUFF_NCANDIDATES; ++ci)
    {
        Buf c;
        int k = 0;
        int ok;

        buf_init(&c);
        ok = rans_mt_encode_k(S, n, bg, force_k > 0 ? force_k : HUFF_K_CANDIDATES[ci], &c, &k);
        if (ok && (best_k == 0 || c.len < best.len))
        {
            buf_free(&best);
            best   = c;
            best_k = k;
        }
        else
        {
            buf_free(&c);
        }
        if (force_k > 0)
        {
            break;
        }
    }
    free(bg);
    if (best_k == 0)
    {
        return 0;
    }
    buf_append(out, best.p, best.len);
    buf_free(&best);
    if (k_used)
    {
        *k_used = best_k;
    }
    return 1;
}

static int rans_mt_decode(const uint8_t *pay, size_t plen, size_t n, uint8_t *out)
{
    BitR           r;
    uint8_t        cmap[256];
    Tab           *tabs;
    size_t         pos;
    size_t         i;
    uint32_t       state = 0;
    const uint8_t *rp;
    const uint8_t *rend;
    int            prev = 0;
    int            ok   = 1;
    int            k;
    int            t;
    int            ctx;
    int            s;

    br_init(&r, pay, plen);
    k = (int)br_bits(&r, 8);
    if (k == 0)
    {
        return 0;
    }
    memset(cmap, 0, sizeof cmap);
    if (k > 1)
    {
        int b = huff_cmap_bits(k);

        for (ctx = 0; ctx < 256; ++ctx)
        {
            uint32_t v = br_bits(&r, b);

            if (v >= (uint32_t)k)
            {
                return 0;
            }
            cmap[ctx] = (uint8_t)v;
        }
    }
    tabs = calloc((size_t)k, sizeof *tabs);
    if (!tabs)
    {
        return 0;
    }
    for (t = 0; t < k; ++t)
    {
        if (!rans_read_table(&r, tabs[t].freq))
        {
            free(tabs);
            return 0;
        }
        rans_tab_build(&tabs[t]);
    }
    br_align(&r);
    pos = br_bytepos(&r);
    if (br_overrun(&r) || pos + 4 > plen)
    {
        free(tabs);
        return 0;
    }
    for (s = 0; s < 4; ++s)
    {
        state |= (uint32_t)pay[pos + s] << (8 * s);
    }
    rp   = pay + pos + 4;
    rend = pay + plen;

    for (i = 0; i < n && ok; ++i)
    {
        Tab      *tb   = &tabs[cmap[prev]];
        uint32_t  slot = state & (PROB_SCALE - 1);
        int       sym;
        uint32_t  x;

        if (!tb->used)
        {
            ok = 0;
            break;
        }
        sym    = tb->slot[slot];
        out[i] = (uint8_t)sym;
        prev   = sym;
        x      = tb->freq[sym] * (state >> PROB_BITS) + slot - tb->cum[sym];
        while (x < RANS_L)
        {
            if (rp >= rend)
            {
                ok = 0;
                break;
            }
            x = (x << 8) | *rp++;
        }
        state = x;
    }
    if (ok && (state != RANS_L || rp != rend))
    {
        ok = 0;
    }
    free(tabs);
    return ok;
}

/*==========================================================================*/
/*                                                                          */
/*  CODER C: adaptive header-free range coder (from r6c.c, inner_mode 2).   */
/*  Kept so r6c files still decode; competes only under method "auto".      */
/*                                                                          */
/*==========================================================================*/
#define RC_TOP      (1u << 24)
#define RC_BOT      (1u << 16)
#define MODEL_INC   24
#define MODEL_LIMIT (RC_BOT - 1)          /* total stays < RC_BOT           */

typedef struct
{
    uint16_t f[256];
    uint32_t tot;
} Model;

static void model_init(Model *m)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        m->f[i] = 1;
    }
    m->tot = 256;
}

static void model_update(Model *m, int sym)
{
    m->f[sym] += MODEL_INC;
    m->tot    += MODEL_INC;
    if (m->tot >= MODEL_LIMIT)
    {
        int i;

        m->tot = 0;
        for (i = 0; i < 256; ++i)
        {
            m->f[i]  = (uint16_t)((m->f[i] >> 1) | 1);
            m->tot  += m->f[i];
        }
    }
}

typedef struct
{
    uint32_t low;
    uint32_t range;
    Buf     *out;
} REnc;

static void renc_init(REnc *e, Buf *o)
{
    e->low   = 0;
    e->range = 0xFFFFFFFFu;
    e->out   = o;
}

static void renc_encode(REnc *e, uint32_t cum, uint32_t freq, uint32_t tot)
{
    e->range /= tot;
    e->low   += cum * e->range;
    e->range *= freq;
    for (;;)                              /* Subbotin carryless normalize   */
    {
        if ((e->low ^ (e->low + e->range)) >= RC_TOP)
        {
            if (e->range >= RC_BOT)
            {
                break;
            }
            e->range = (0 - e->low) & (RC_BOT - 1);
        }
        buf_push(e->out, (uint8_t)(e->low >> 24));
        e->low   <<= 8;
        e->range <<= 8;
    }
}

static void renc_flush(REnc *e)
{
    int i;

    for (i = 0; i < 4; ++i)
    {
        buf_push(e->out, (uint8_t)(e->low >> 24));
        e->low <<= 8;
    }
}

typedef struct
{
    uint32_t       low;
    uint32_t       range;
    uint32_t       code;
    const uint8_t *p;
    const uint8_t *end;
} RDec;

static uint8_t rdec_byte(RDec *d)
{
    return (d->p < d->end) ? *d->p++ : 0;
}

static void rdec_init(RDec *d, const uint8_t *p, size_t len)
{
    int i;

    d->low   = 0;
    d->range = 0xFFFFFFFFu;
    d->code  = 0;
    d->p     = p;
    d->end   = p + len;
    for (i = 0; i < 4; ++i)
    {
        d->code = (d->code << 8) | rdec_byte(d);
    }
}

static uint32_t rdec_getfreq(RDec *d, uint32_t tot)
{
    d->range /= tot;
    return (d->code - d->low) / d->range;
}

static void rdec_decode(RDec *d, uint32_t cum, uint32_t freq)
{
    d->low   += cum * d->range;
    d->range *= freq;
    for (;;)
    {
        if ((d->low ^ (d->low + d->range)) >= RC_TOP)
        {
            if (d->range >= RC_BOT)
            {
                break;
            }
            d->range = (0 - d->low) & (RC_BOT - 1);
        }
        d->code    = (d->code << 8) | rdec_byte(d);
        d->low   <<= 8;
        d->range <<= 8;
    }
}

static Model *models_new(int order)
{
    int    nm = (order == 0) ? 1 : 256;
    Model *ms = malloc(sizeof(Model) * (size_t)nm);
    int    i;

    if (ms)
    {
        for (i = 0; i < nm; ++i)
        {
            model_init(&ms[i]);
        }
    }
    return ms;
}

static int adapt_encode(const uint8_t *S, size_t n, int order, Buf *out)
{
    Model *ms = models_new(order);
    REnc   e;
    int    prev = 0;
    size_t i;

    if (!ms)
    {
        return 0;
    }
    buf_push(out, (uint8_t)order);
    renc_init(&e, out);
    for (i = 0; i < n; ++i)
    {
        Model   *m   = (order == 0) ? &ms[0] : &ms[prev];
        int      sym = S[i];
        uint32_t cum = 0;
        int      j;

        for (j = 0; j < sym; ++j)
        {
            cum += m->f[j];
        }
        renc_encode(&e, cum, m->f[sym], m->tot);
        model_update(m, sym);
        prev = sym;
    }
    renc_flush(&e);
    free(ms);
    return 1;
}

static int adapt_decode(const uint8_t *pay, size_t plen, size_t n, uint8_t *out)
{
    Model *ms;
    RDec   d;
    int    order;
    int    prev = 0;
    int    ok   = 1;
    size_t i;

    if (plen < 1 || pay[0] > 1)
    {
        return 0;
    }
    order = pay[0];
    ms    = models_new(order);
    if (!ms)
    {
        return 0;
    }
    rdec_init(&d, pay + 1, plen - 1);
    for (i = 0; i < n; ++i)
    {
        Model   *m   = (order == 0) ? &ms[0] : &ms[prev];
        uint32_t f   = rdec_getfreq(&d, m->tot);
        uint32_t c   = 0;
        int      sym = 0;

        while (sym < 256 && c + m->f[sym] <= f)
        {
            c += m->f[sym];
            sym++;
        }
        if (sym == 256)                   /* corrupt stream                 */
        {
            ok = 0;
            break;
        }
        rdec_decode(&d, c, m->f[sym]);
        out[i] = (uint8_t)sym;
        model_update(m, sym);
        prev = sym;
    }
    free(ms);
    return ok;
}

/*==========================================================================*/
/* candidate driver: build every candidate the method allows, keep the      */
/* smallest.  blob = inner_mode u8 | payload                                */
/*==========================================================================*/
static void keep_if_smaller(Buf *best, Buf *cand, int ok,
                            char *desc, size_t dlen, const char *cand_desc)
{
    if (ok && cand->len < best->len)
    {
        buf_free(best);
        *best = *cand;
        snprintf(desc, dlen, "%s", cand_desc);
    }
    else
    {
        buf_free(cand);
    }
}

static void encode_stream(const uint8_t *S, size_t n, int method, Buf *blob,
                          char *desc, size_t dlen)
{
    Buf  best;
    Buf  c;
    char what[64];
    int  ok;
    int  i;

    buf_init(&best);
    buf_push(&best, MODE_STORED);
    buf_append(&best, S, n);
    snprintf(desc, dlen, "stored");

    if (method == METHOD_HUFF || method == METHOD_AUTO)
    {
        int k = 0;

        buf_init(&c);
        buf_push(&c, MODE_HUFF);
        ok = huff_encode(S, n, 0, &c, &k);
        snprintf(what, sizeof what, "multi-table Huffman, %d tree%s", k, k == 1 ? "" : "s");
        keep_if_smaller(&best, &c, ok, desc, dlen, what);
    }
    if (method == METHOD_RANS || method == METHOD_AUTO)
    {
        for (i = 0; i < RANS_NCHOICES; ++i)
        {
            int k = 1 << RANS_CTX_CHOICES[i];

            buf_init(&c);
            buf_push(&c, MODE_RANS);
            ok = rans_encode(S, n, RANS_CTX_CHOICES[i], &c);
            snprintf(what, sizeof what, "high-bits rANS, %d table%s", k, k == 1 ? "" : "s");
            keep_if_smaller(&best, &c, ok, desc, dlen, what);
        }
        {
            int k = 0;

            buf_init(&c);
            buf_push(&c, MODE_RANS_MT);
            ok = rans_mt_encode(S, n, 0, &c, &k);
            snprintf(what, sizeof what, "clustered rANS, %d table%s", k, k == 1 ? "" : "s");
            keep_if_smaller(&best, &c, ok, desc, dlen, what);
        }
    }
    if (method == METHOD_AUTO)
    {
        for (i = 0; i <= 1; ++i)
        {
            buf_init(&c);
            buf_push(&c, MODE_ADAPT);
            ok = adapt_encode(S, n, i, &c);
            snprintf(what, sizeof what, "adaptive range coder, order-%d", i);
            keep_if_smaller(&best, &c, ok, desc, dlen, what);
        }
    }
    buf_append(blob, best.p, best.len);
    buf_free(&best);
}

static int decode_stream(const uint8_t *blob, size_t blen, size_t n, uint8_t *out)
{
    const uint8_t *pay;
    size_t         plen;

    if (n == 0)
    {
        return 1;
    }
    if (blen < 1)
    {
        return 0;
    }
    pay  = blob + 1;
    plen = blen - 1;
    switch (blob[0])
    {
        case MODE_STORED:
        {
            if (plen < n)
            {
                return 0;
            }
            memcpy(out, pay, n);
            return 1;
        }
        case MODE_RANS:
        {
            return rans_decode(pay, plen, n, out);
        }
        case MODE_ADAPT:
        {
            return adapt_decode(pay, plen, n, out);
        }
        case MODE_HUFF:
        {
            return huff_decode(pay, plen, n, out);
        }
        case MODE_RANS_MT:
        {
            return rans_mt_decode(pay, plen, n, out);
        }
        default:
        {
            return 0;
        }
    }
}

/*==========================================================================*/
/* file container: 'R','6','C','1' | ver u8 | orig_len u32 LE | blob        */
/*==========================================================================*/
static int compress_buffer(const uint8_t *S, size_t n, int method, Buf *out,
                           char *desc, size_t dlen)
{
    int s;

    if ((uint64_t)n > 0xFFFFFFFFu)
    {
        fprintf(stderr, "input too large: the container holds a 32-bit length\n");
        return 0;
    }
    buf_append(out, (const uint8_t *)"R6C1", 4);
    buf_push(out, CONTAINER_VER);
    for (s = 0; s < 4; ++s)
    {
        buf_push(out, (uint8_t)((uint32_t)n >> (8 * s)));
    }
    snprintf(desc, dlen, "empty");
    if (n > 0)
    {
        encode_stream(S, n, method, out, desc, dlen);
    }
    return 1;
}

static int decompress_buffer(const uint8_t *C, size_t clen, Buf *out)
{
    uint32_t n = 0;
    int      s;

    if (clen < 9 || memcmp(C, "R6C1", 4) != 0)
    {
        fprintf(stderr, "bad magic\n");
        return 0;
    }
    if (C[4] < 1 || C[4] > CONTAINER_VER)
    {
        fprintf(stderr, "unsupported container version %d\n", C[4]);
        return 0;
    }
    for (s = 0; s < 4; ++s)
    {
        n |= (uint32_t)C[5 + s] << (8 * s);
    }
    if (n == 0)
    {
        return 1;
    }
    buf_reserve(out, n);
    out->len = n;
    return decode_stream(C + 9, clen - 9, n, out->p);
}

/*--------------------------------------------------------------------------*/
/* file I/O                                                                 */
/*--------------------------------------------------------------------------*/
static int read_file(const char *path, Buf *b)
{
    static uint8_t tmp[65536];
    FILE          *f = fopen(path, "rb");
    size_t         r;

    buf_init(b);
    if (!f)
    {
        perror(path);
        return 0;
    }
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
    {
        buf_append(b, tmp, r);
    }
    {
        int ok = !ferror(f);
        if (fclose(f) != 0) ok = 0;
        if (!ok)
        {
            fprintf(stderr, "%s: read failed\n", path);
            buf_free(b);
        }
        return ok;
    }
}

static int write_file(const char *path, const uint8_t *p, size_t n)
{
    FILE *f  = fopen(path, "wb");
    int   ok = 1;

    if (!f)
    {
        perror(path);
        return 0;
    }
    if (n && fwrite(p, 1, n, f) != n)
    {
        ok = 0;
    }
    if (fclose(f) != 0)
    {
        ok = 0;
    }
    if (!ok)
    {
        fprintf(stderr, "%s: write failed\n", path);
    }
    return ok;
}

/*==========================================================================*/
/* bench: every configuration of both coders on the whole input, each one   */
/* round-tripped so the numbers are honest.  Sizes are payload bytes        */
/* (headers of the coder included, 10-byte container framing excluded).     */
/*==========================================================================*/
typedef int (*DecodeFn)(const uint8_t *, size_t, size_t, uint8_t *);

static double ms_since(clock_t t0)
{
    return 1000.0 * (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
}

static int bench_row(const char *name, const uint8_t *S, size_t n,
                      const Buf *enc, int enc_ok, double enc_ms,
                      DecodeFn dec, size_t *best, const char **best_name)
{
    uint8_t *tmp = malloc(n);
    clock_t  t0  = clock();
    int      ok  = enc_ok && tmp && dec(enc->p, enc->len, n, tmp)
                          && memcmp(tmp, S, n) == 0;
    double   dec_ms = ms_since(t0);

    printf("%-34s %12zu %10.4f %9.1f %9.1f  %s\n", name, enc->len,
           8.0 * (double)enc->len / (double)n, enc_ms, dec_ms,
           ok ? "ok" : "FAIL");
    if (ok && best && enc->len < *best)
    {
        *best      = enc->len;
        *best_name = name;
    }
    free(tmp);
    return ok;
}

static int bench(const uint8_t *S, size_t n)
{
    static char  names[96][48];
    int          nn         = 0;
    size_t       best_h     = (size_t)-1;
    size_t       best_r     = (size_t)-1;
    const char  *best_hname = "";
    const char  *best_rname = "";
    Buf          e;
    clock_t      t0;
    int          ok;
    int          i;
    int          k = 0;
    int          all_ok = 1;

    if (n == 0)
    {
        printf("empty input\n");
        return 1;
    }
    printf("input: %zu bytes\n\n", n);
    printf("%-34s %12s %10s %9s %9s\n", "method", "bytes", "bits/byte", "enc ms", "dec ms");
    printf("%-34s %12s %10s %9s %9s\n", "------", "-----", "---------", "------", "------");

    for (i = 0; i < HUFF_NCANDIDATES; ++i)
    {
        buf_init(&e);
        t0 = clock();
        ok = huff_encode(S, n, HUFF_K_CANDIDATES[i], &e, &k);
        snprintf(names[nn], sizeof names[nn], "Huffman  k=%-2d asked, %2d used",
                 HUFF_K_CANDIDATES[i], k);
        if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), huff_decode, &best_h, &best_hname)) all_ok = 0;
        nn++;
        buf_free(&e);
    }
    buf_init(&e);
    t0 = clock();
    ok = huff_encode(S, n, 0, &e, &k);
    snprintf(names[nn], sizeof names[nn], "Huffman  model-selected (k=%d)", k);
    if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), huff_decode, NULL, NULL)) all_ok = 0;
    nn++;
    buf_free(&e);
    printf("\n");

    for (i = 0; i < RANS_NCHOICES; ++i)
    {
        buf_init(&e);
        t0 = clock();
        ok = rans_encode(S, n, RANS_CTX_CHOICES[i], &e);
        snprintf(names[nn], sizeof names[nn], "rANS     ctx_bits=%d, %3d table%s",
                 RANS_CTX_CHOICES[i], 1 << RANS_CTX_CHOICES[i],
                 RANS_CTX_CHOICES[i] ? "s" : "");
        if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), rans_decode, &best_r, &best_rname)) all_ok = 0;
        nn++;
        buf_free(&e);
    }
    printf("\n");

    for (i = 0; i < HUFF_NCANDIDATES; ++i)
    {
        buf_init(&e);
        t0 = clock();
        ok = rans_mt_encode(S, n, HUFF_K_CANDIDATES[i], &e, &k);
        snprintf(names[nn], sizeof names[nn], "rANS-cl  k=%-2d asked, %2d used",
                 HUFF_K_CANDIDATES[i], k);
        if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), rans_mt_decode, &best_r, &best_rname)) all_ok = 0;
        nn++;
        buf_free(&e);
    }
    buf_init(&e);
    t0 = clock();
    ok = rans_mt_encode(S, n, 0, &e, &k);
    snprintf(names[nn], sizeof names[nn], "rANS-cl  model-selected (k=%d)", k);
    if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), rans_mt_decode, NULL, NULL)) all_ok = 0;
    nn++;
    buf_free(&e);
    printf("\n");

    for (i = 0; i <= 1; ++i)
    {
        buf_init(&e);
        t0 = clock();
        ok = adapt_encode(S, n, i, &e);
        snprintf(names[nn], sizeof names[nn], "adaptive range coder, order-%d", i);
        if (!bench_row(names[nn], S, n, &e, ok, ms_since(t0), adapt_decode, NULL, NULL)) all_ok = 0;
        nn++;
        buf_free(&e);
    }
    printf("%-34s %12zu %10.4f\n", "stored", n, 8.0);

    if (best_h != (size_t)-1 && best_r != (size_t)-1)
    {
        printf("\nbest Huffman: %12zu bytes  [%s]\n", best_h, best_hname);
        printf("best rANS:    %12zu bytes  [%s]\n", best_r, best_rname);
        printf("rANS vs Huffman: %+.2f%%\n",
               100.0 * ((double)best_r / (double)best_h - 1.0));
    }
    return all_ok;
}

/*--------------------------------------------------------------------------*/
static int self_test(const char *path, const uint8_t *S, size_t n)
{
    static const char *mnames[3] =
    {
        "huffman", "rans", "auto"
    };
    int                all_ok    = 1;
    int                m;

    for (m = 0; m < 3; ++m)
    {
        Buf  comp;
        Buf  dec;
        char desc[64];
        int  ok;

        buf_init(&comp);
        buf_init(&dec);
        ok = compress_buffer(S, n, m, &comp, desc, sizeof desc)
             && decompress_buffer(comp.p, comp.len, &dec)
             && dec.len == n
             && (n == 0 || memcmp(dec.p, S, n) == 0);
        fprintf(stderr, "%s [%-7s]: %zu -> %zu -> %zu  %-38s %s\n", path,
                mnames[m], n, comp.len, dec.len, desc,
                ok ? "ROUND-TRIP OK" : "MISMATCH");
        if (!ok)
        {
            all_ok = 0;
        }
        buf_free(&comp);
        buf_free(&dec);
    }
    return all_ok;
}

/*==========================================================================*/
/*                                                                          */
/*  CORPUS RUN: every regular file in each folder is compressed four ways   */
/*  and the results go to <folder>.csv, one row per file; averages.csv      */
/*  then gets one row per folder holding the per-folder means.              */
/*                                                                          */
/*      huffman      one canonical Huffman tree            (force_k = 1)    */
/*      huffman_mt   multi-table Huffman, k model-selected (as `c h`)       */
/*      rans         one static rANS table                 (ctx_bits = 0)   */
/*      rans_mt      multi-table rANS, best ctx_bits 0..8  (as `c r`)       */
/*                                                                          */
/*  Both _mt searches include their single-table case, exactly as the       */
/*  compressor does, so an _mt column can tie but never lose to its         */
/*  single-table neighbour.                                                 */
/*                                                                          */
/*  Sizes follow the bench convention: coder payload with its own headers   */
/*  (tables, trees, cmap, rANS state), without the container's framing.     */
/*  Set CORPUS_FRAMING_BYTES to 10 to get on-disk `mtcodec c` sizes         */
/*  instead (9-byte container header + 1 inner_mode byte; this does not     */
/*  model the container's `stored` fallback).                               */
/*                                                                          */
/*  Every figure written has been round-trip verified first.                */
/*==========================================================================*/
#define CORPUS_FRAMING_BYTES  0
#define CORPUS_NMETHODS       5
#define CORPUS_SUMMARY_NAME   "averages.csv"

enum
{
    CM_HUFF = 0, CM_HUFF_MT, CM_RANS, CM_RANS_HB, CM_RANS_MT
};

static const char *const CORPUS_METHOD[CORPUS_NMETHODS] =
{
    "huffman", "huffman_mt", "rans", "rans_hb", "rans_mt"
};

static const char *const CORPUS_DEFAULT_DIRS[] =
{
    "calgary", "canterbury", "large"
};
#define CORPUS_NDEFAULT ((int)(sizeof CORPUS_DEFAULT_DIRS / sizeof CORPUS_DEFAULT_DIRS[0]))

typedef struct
{
    size_t bytes[CORPUS_NMETHODS];
    int    huff_trees;                    /* k chosen by huffman_mt         */
    int    rans_hb_tables;                /* tables chosen by rans_hb       */
    int    rans_tables;                   /* k chosen by rans_mt            */
} CorpusResult;

typedef struct
{
    char   name[256];                     /* folder name without any path   */
    size_t files;
    double sum_bytes[CORPUS_NMETHODS];
    double sum_bpb[CORPUS_NMETHODS];
} CorpusSummary;

/*--------------------------------------------------------------------------*/
/* directory listing: regular, non-hidden files, sorted by name             */
/*--------------------------------------------------------------------------*/
typedef struct
{
    char  **name;
    size_t  n;
    size_t  cap;
} NameList;

static void names_free(NameList *l)
{
    size_t i;

    for (i = 0; i < l->n; ++i)
    {
        free(l->name[i]);
    }
    free(l->name);
    l->name = NULL;
    l->n    = 0;
    l->cap  = 0;
}

static int names_add(NameList *l, const char *s)
{
    size_t len = strlen(s) + 1;
    char  *copy;

    if (l->n == l->cap)
    {
        size_t  nc = l->cap ? l->cap * 2 : 32;
        char  **np = (char **)realloc(l->name, nc * sizeof *np);

        if (!np)
        {
            return 0;
        }
        l->name = np;
        l->cap  = nc;
    }
    copy = (char *)malloc(len);
    if (!copy)
    {
        return 0;
    }
    memcpy(copy, s, len);
    l->name[l->n++] = copy;
    return 1;
}

static int names_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static char *path_join(const char *dir, const char *name)
{
    size_t dl   = strlen(dir);
    size_t nl   = strlen(name);
    int    sep  = dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\';
    char  *path = (char *)malloc(dl + nl + 2);

    if (path)
    {
        memcpy(path, dir, dl);
        if (sep)
        {
            path[dl] = '/';
        }
        memcpy(path + dl + sep, name, nl + 1);
    }
    return path;
}

/* returns 0 if the folder cannot be read */
static int list_dir(const char *dir, NameList *out)
{
    int ok = 1;

    out->name = NULL;
    out->n    = 0;
    out->cap  = 0;

#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE           h;
        char            *pattern = path_join(dir, "*");

        if (!pattern)
        {
            return 0;
        }
        h = FindFirstFileA(pattern, &fd);
        free(pattern);
        if (h == INVALID_HANDLE_VALUE)
        {
            fprintf(stderr, "%s: cannot open folder\n", dir);
            return 0;
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            if (fd.cFileName[0] == '.')
            {
                continue;
            }
            if (!names_add(out, fd.cFileName))
            {
                ok = 0;
                break;
            }
        }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR           *d = opendir(dir);
        struct dirent *de;

        if (!d)
        {
            perror(dir);
            return 0;
        }
        while ((de = readdir(d)) != NULL)
        {
            struct stat st;
            char       *path;
            int         regular;

            if (de->d_name[0] == '.')
            {
                continue;
            }
            path = path_join(dir, de->d_name);
            if (!path)
            {
                ok = 0;
                break;
            }
            regular = stat(path, &st) == 0 && S_ISREG(st.st_mode);
            free(path);
            if (regular && !names_add(out, de->d_name))
            {
                ok = 0;
                break;
            }
        }
        closedir(d);
    }
#endif

    if (!ok)
    {
        fprintf(stderr, "out of memory\n");
        names_free(out);
        return 0;
    }
    if (out->n > 1)
    {
        qsort(out->name, out->n, sizeof out->name[0], names_cmp);
    }
    return 1;
}

/* "data/calgary/" -> "calgary" */
static void dir_basename(const char *dir, char *out, size_t cap)
{
    size_t end = strlen(dir);
    size_t beg;
    size_t len;

    while (end > 0 && (dir[end - 1] == '/' || dir[end - 1] == '\\'))
    {
        end--;
    }
    beg = end;
    while (beg > 0 && dir[beg - 1] != '/' && dir[beg - 1] != '\\' && dir[beg - 1] != ':')
    {
        beg--;
    }
    len = end - beg;
    if (len == 0 || (len == 1 && dir[beg] == '.'))
    {
        snprintf(out, cap, "corpus");
        return;
    }
    if (len >= cap)
    {
        len = cap - 1;
    }
    memcpy(out, dir + beg, len);
    out[len] = '\0';
}

/* RFC 4180 quoting, only where a field needs it */
static void csv_field(FILE *f, const char *s)
{
    if (strpbrk(s, ",\"\r\n") == NULL)
    {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (; *s; ++s)
    {
        if (*s == '"')
        {
            fputc('"', f);
        }
        fputc(*s, f);
    }
    fputc('"', f);
}

/*--------------------------------------------------------------------------*/
/* the four measurements for one file; returns 0 if anything fails to       */
/* encode or to round-trip                                                  */
/*--------------------------------------------------------------------------*/
static int corpus_verify(const uint8_t *S, size_t n, const Buf *enc,
                         DecodeFn dec, uint8_t *scratch)
{
    return dec(enc->p, enc->len, n, scratch) && memcmp(scratch, S, n) == 0;
}

static int corpus_measure(const uint8_t *S, size_t n, CorpusResult *r)
{
    uint8_t *scratch = (uint8_t *)malloc(n);
    Buf      e;
    Buf      best;
    int      best_bits = -1;
    int      ok        = 1;
    int      k         = 0;
    int      i;

    if (!scratch)
    {
        fprintf(stderr, "out of memory\n");
        return 0;
    }
    memset(r, 0, sizeof *r);

    /* normal Huffman: a single tree */
    buf_init(&e);
    ok = huff_encode(S, n, 1, &e, &k) && corpus_verify(S, n, &e, huff_decode, scratch);
    r->bytes[CM_HUFF] = e.len;
    buf_free(&e);

    /* multi-table Huffman: k chosen by exact total cost */
    if (ok)
    {
        buf_init(&e);
        ok = huff_encode(S, n, 0, &e, &k) && corpus_verify(S, n, &e, huff_decode, scratch);
        r->bytes[CM_HUFF_MT] = e.len;
        r->huff_trees        = k;
        buf_free(&e);
    }

    /* rANS: ctx_bits 0 is the normal coder; the smallest of all the        */
    /* choices is the multi-table result (ties keep the fewest tables)      */
    buf_init(&best);
    for (i = 0; i < RANS_NCHOICES && ok; ++i)
    {
        int bits = RANS_CTX_CHOICES[i];

        buf_init(&e);
        ok = rans_encode(S, n, bits, &e);
        if (ok && bits == 0)
        {
            ok = corpus_verify(S, n, &e, rans_decode, scratch);
            r->bytes[CM_RANS] = e.len;
        }
        if (ok && (best_bits < 0 || e.len < best.len))
        {
            buf_free(&best);
            best      = e;
            best_bits = bits;
        }
        else
        {
            buf_free(&e);
        }
    }
    if (ok && best_bits != 0)             /* ctx_bits 0 was verified above  */
    {
        ok = best_bits > 0 && corpus_verify(S, n, &best, rans_decode, scratch);
    }
    if (ok)
    {
        r->bytes[CM_RANS_HB] = best.len;
        r->rans_hb_tables    = 1 << best_bits;
    }
    buf_free(&best);

    /* clustered multi-table rANS: k chosen by exact encoded size */
    if (ok)
    {
        buf_init(&best);
        ok = rans_mt_encode(S, n, 0, &best, &k)
             && corpus_verify(S, n, &best, rans_mt_decode, scratch);
        r->bytes[CM_RANS_MT] = best.len;
        r->rans_tables       = k;
    }
    if (ok)
    {
        for (i = 0; i < CORPUS_NMETHODS; ++i)
        {
            r->bytes[i] += CORPUS_FRAMING_BYTES;
        }
    }
    buf_free(&best);
    free(scratch);
    return ok;
}

/*--------------------------------------------------------------------------*/
/* one folder -> <folder>.csv; fills *sum. Returns 0 on any problem.        */
/*--------------------------------------------------------------------------*/
static int corpus_run_dir(const char *dir, CorpusSummary *sum)
{
    NameList list;
    char     csv_name[300];
    FILE    *csv;
    int      all_ok = 1;
    int      m;
    size_t   i;

    memset(sum, 0, sizeof *sum);
    dir_basename(dir, sum->name, sizeof sum->name);
    snprintf(csv_name, sizeof csv_name, "%s.csv", sum->name);

    if (!list_dir(dir, &list))
    {
        return 0;
    }
    csv = fopen(csv_name, "w");
    if (!csv)
    {
        perror(csv_name);
        names_free(&list);
        return 0;
    }

    fputs("file,original_bytes", csv);
    for (m = 0; m < CORPUS_NMETHODS; ++m)
    {
        fprintf(csv, ",%s_bytes", CORPUS_METHOD[m]);
    }
    for (m = 0; m < CORPUS_NMETHODS; ++m)
    {
        fprintf(csv, ",%s_bpb", CORPUS_METHOD[m]);
    }
    fputs(",huffman_mt_trees,rans_hb_tables,rans_mt_tables\n", csv);

    fprintf(stderr, "%s/  (%zu file%s)\n", sum->name, list.n, list.n == 1 ? "" : "s");
    for (i = 0; i < list.n; ++i)
    {
        CorpusResult r;
        Buf          in;
        char        *path = path_join(dir, list.name[i]);

        if (!path || !read_file(path, &in))
        {
            free(path);
            all_ok = 0;
            continue;
        }
        free(path);
        if (in.len == 0)
        {
            fprintf(stderr, "  %-16s empty, skipped\n", list.name[i]);
            buf_free(&in);
            continue;
        }
        if (!corpus_measure(in.p, in.len, &r))
        {
            fprintf(stderr, "  %-16s ROUND-TRIP FAILED, left out of the results\n",
                    list.name[i]);
            buf_free(&in);
            all_ok = 0;
            continue;
        }

        csv_field(csv, list.name[i]);
        fprintf(csv, ",%zu", in.len);
        for (m = 0; m < CORPUS_NMETHODS; ++m)
        {
            fprintf(csv, ",%zu", r.bytes[m]);
        }
        for (m = 0; m < CORPUS_NMETHODS; ++m)
        {
            double bpb = 8.0 * (double)r.bytes[m] / (double)in.len;

            fprintf(csv, ",%.4f", bpb);
            sum->sum_bytes[m] += (double)r.bytes[m];
            sum->sum_bpb[m]   += bpb;
        }
        fprintf(csv, ",%d,%d,%d\n", r.huff_trees, r.rans_hb_tables, r.rans_tables);
        sum->files++;

        fprintf(stderr, "  %-14s %9zu -> H %8zu  H-mt %8zu (k=%-2d)  R %8zu  R-hb %8zu (%3d)  R-mt %8zu (k=%d)\n",
                list.name[i], in.len, r.bytes[CM_HUFF], r.bytes[CM_HUFF_MT],
                r.huff_trees, r.bytes[CM_RANS], r.bytes[CM_RANS_HB], r.rans_hb_tables,
                r.bytes[CM_RANS_MT], r.rans_tables);
        buf_free(&in);
    }
    names_free(&list);

    if (ferror(csv))
    {
        all_ok = 0;
    }
    if (fclose(csv) != 0)
    {
        all_ok = 0;
    }
    if (!all_ok)
    {
        fprintf(stderr, "%s: finished with errors\n", csv_name);
    }
    else
    {
        fprintf(stderr, "  wrote %s\n", csv_name);
    }
    return all_ok;
}

/* per-folder means over files: compressed bytes, and bits per input byte   */
/* (the unweighted per-file mean, as the corpus tables are usually quoted)  */
static int corpus_write_summary(const CorpusSummary *sums, int nsums)
{
    FILE *csv = fopen(CORPUS_SUMMARY_NAME, "w");
    int   ok  = 1;
    int   d;
    int   m;

    if (!csv)
    {
        perror(CORPUS_SUMMARY_NAME);
        return 0;
    }
    fputs("folder", csv);
    for (m = 0; m < CORPUS_NMETHODS; ++m)
    {
        fprintf(csv, ",%s_avg_bytes", CORPUS_METHOD[m]);
    }
    for (m = 0; m < CORPUS_NMETHODS; ++m)
    {
        fprintf(csv, ",%s_avg_bpb", CORPUS_METHOD[m]);
    }
    fputc('\n', csv);

    for (d = 0; d < nsums; ++d)
    {
        if (sums[d].files == 0)
        {
            continue;                     /* nothing measured: no row       */
        }
        csv_field(csv, sums[d].name);
        for (m = 0; m < CORPUS_NMETHODS; ++m)
        {
            fprintf(csv, ",%.1f", sums[d].sum_bytes[m] / (double)sums[d].files);
        }
        for (m = 0; m < CORPUS_NMETHODS; ++m)
        {
            fprintf(csv, ",%.4f", sums[d].sum_bpb[m] / (double)sums[d].files);
        }
        fputc('\n', csv);
    }
    if (ferror(csv))
    {
        ok = 0;
    }
    if (fclose(csv) != 0)
    {
        ok = 0;
    }
    if (ok)
    {
        fprintf(stderr, "wrote %s\n", CORPUS_SUMMARY_NAME);
    }
    else
    {
        fprintf(stderr, "%s: write failed\n", CORPUS_SUMMARY_NAME);
    }
    return ok;
}

static int corpus_run(const char *const *dirs, int ndirs)
{
    CorpusSummary *sums = (CorpusSummary *)calloc((size_t)ndirs, sizeof *sums);
    int            ok   = 1;
    int            d;

    if (!sums)
    {
        fprintf(stderr, "out of memory\n");
        return 0;
    }
    for (d = 0; d < ndirs; ++d)
    {
        if (!corpus_run_dir(dirs[d], &sums[d]))
        {
            ok = 0;
        }
    }
    if (!corpus_write_summary(sums, ndirs))
    {
        ok = 0;
    }
    free(sums);
    return ok;
}

static int usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s c h <in> <out>   compress, multi-table Huffman\n"
            "       %s c r <in> <out>   compress, multi-table rANS\n"
            "       %s c a <in> <out>   compress, auto (best of all candidates)\n"
            "       %s d <in> <out>     decompress (method read from file)\n"
            "       %s t <in>           self-test: round-trip every method\n"
            "       %s b <in>           bench: all configurations, verified\n"
            "       %s s [dir ...]      corpus run: <dir>.csv per folder + averages.csv\n"
            "                                (default: calgary canterbury large; also\n"
            "                                what a bare `%s` with no arguments does)\n",
            prog, prog, prog, prog, prog, prog, prog, prog);
    return 1;
}

int main(int argc, char **argv)
{
    Buf  in;
    Buf  out;
    char cmd;
    int  rc = 0;

    if (argc == 1 || (argv[1][0] == 's' && argv[1][1] == '\0'))
    {
        const char *const *dirs  = CORPUS_DEFAULT_DIRS;
        int                ndirs = CORPUS_NDEFAULT;

        if (argc > 2)
        {
            dirs  = (const char *const *)(argv + 2);
            ndirs = argc - 2;
        }
        return corpus_run(dirs, ndirs) ? 0 : 5;
    }
    if (argc < 3)
    {
        return usage(argv[0]);
    }
    cmd = argv[1][0];

    if (cmd == 'c')
    {
        char desc[64];
        int  method;

        if (argc != 5)
        {
            return usage(argv[0]);
        }
        switch (argv[2][0])
        {
            case 'h':
            {
                method = METHOD_HUFF;
                break;
            }
            case 'r':
            {
                method = METHOD_RANS;
                break;
            }
            case 'a':
            {
                method = METHOD_AUTO;
                break;
            }
            default:
            {
                fprintf(stderr, "unknown method '%s' (want h, r or a)\n", argv[2]);
                return 1;
            }
        }
        if (!read_file(argv[3], &in))
        {
            return 2;
        }
        buf_init(&out);
        if (!compress_buffer(in.p, in.len, method, &out, desc, sizeof desc)
            || !write_file(argv[4], out.p, out.len))
        {
            rc = 3;
        }
        else
        {
            fprintf(stderr, "%zu -> %zu bytes (%.4f)  [%s]\n", in.len, out.len,
                    in.len ? (double)out.len / (double)in.len : 0.0, desc);
        }
        buf_free(&out);
    }
    else if (cmd == 'd')
    {
        if (argc != 4)
        {
            return usage(argv[0]);
        }
        if (!read_file(argv[2], &in))
        {
            return 2;
        }
        buf_init(&out);
        if (!decompress_buffer(in.p, in.len, &out))
        {
            fprintf(stderr, "decode failed\n");
            rc = 3;
        }
        else if (!write_file(argv[3], out.p, out.len))
        {
            rc = 3;
        }
        else
        {
            fprintf(stderr, "%zu -> %zu bytes\n", in.len, out.len);
        }
        buf_free(&out);
    }
    else if (cmd == 't' || cmd == 'b')
    {
        if (!read_file(argv[2], &in))
        {
            return 2;
        }
        if (cmd == 't')
        {
            rc = self_test(argv[2], in.p, in.len) ? 0 : 4;
        }
        else
        {
            rc = bench(in.p, in.len) ? 0 : 4;
        }
    }
    else
    {
        return usage(argv[0]);
    }

    buf_free(&in);
    return rc;
}
