/* Test-only access shim shared by separate reference/experimental objects.
 * Never edit the frozen source to expose its static functions.
 */
#define DCC_JOIN_(a,b) a##b
#define DCC_JOIN(a,b) DCC_JOIN_(a,b)
#define DCC_FN(n) DCC_JOIN(DCC_PREFIX,n)

int DCC_FN(_encode)(int method, int param, const uint8_t *s, size_t n,
                    uint8_t **p, size_t *len, int *k)
{
    Buf b;
    int ok = 0;
    char desc[128];
    *p = NULL; *len = 0; *k = 0;
    buf_init(&b);
    switch (method) {
    case 0: ok = huff_encode(s, n, param, &b, k); break;
    case 1: ok = rans_mt_encode(s, n, param, &b, k); break;
    case 2: ok = rans_encode(s, n, param, &b); *k = 1 << param; break;
    case 3: ok = adapt_encode(s, n, param, &b); break;
    case 4: ok = compress_buffer(s, n, param, &b, desc, sizeof desc); break;
    default: break;
    }
    if (!ok) { buf_free(&b); return 0; }
    *p = b.p; *len = b.len;
    return 1;
}

int DCC_FN(_decode)(int method, const uint8_t *p, size_t len,
                    size_t n, uint8_t *out)
{
    switch (method) {
    case 0: return huff_decode(p, len, n, out);
    case 1: return rans_mt_decode(p, len, n, out);
    case 2: return rans_decode(p, len, n, out);
    case 3: return adapt_decode(p, len, n, out);
    case 4: {
        Buf b;
        int ok;
        buf_init(&b);
        ok = decompress_buffer(p, len, &b) && b.len == n;
        if (ok && n) memcpy(out, b.p, n);
        buf_free(&b);
        return ok;
    }
    default: return 0;
    }
}

int DCC_FN(_stats)(int method, const uint8_t *p, size_t len,
                   const uint8_t *s, size_t n, DccStats *st)
{
    BitR r;
    uint8_t lens[HUFF_MAX_TREES][256];
    uint16_t freq[256];
    uint64_t before, total;
    int c, t, prev = 0;
    size_t i;
    memset(st, 0, sizeof *st);
    if (method >= 3) return 1; /* No invented adaptive/container breakdown. */
    if (!p || !len) return 0;
    br_init(&r, p, len);
    t = (int)br_bits(&r, 8);
    st->count_bits = 8;
    if (method == 2) {
        if (t > 8) return 0;
        st->k = 1 << t;
        for (c = 0; c < 256; ++c)
            st->cmap[c] = (uint8_t)rans_ctx_of(c, t);
    } else {
        if (t < 1 || t > HUFF_MAX_TREES) return 0;
        st->k = t;
        if (t > 1) {
            int bits = huff_cmap_bits(t);
            for (c = 0; c < 256; ++c) {
                uint32_t v = br_bits(&r, bits);
                if (v >= (uint32_t)t) return 0;
                st->cmap[c] = (uint8_t)v;
            }
            st->map_bits = (uint64_t)256 * (uint64_t)bits;
        }
    }
    before = r.used;
    if (method == 0) {
        for (t = 0; t < st->k; ++t) {
            uint64_t start = r.used;
            huff_read_lengths(&r, lens[t]);
            if (r.used - start != huff_lengths_cost_bits(lens[t])) return 0;
        }
        st->table_bits = r.used - before;
        for (i = 0; i < n; ++i) {
            int l = lens[st->cmap[prev]][s[i]];
            if (!l) return 0;
            st->payload_bits += (uint64_t)l;
            prev = s[i];
        }
        total = r.used + st->payload_bits;
        st->padding_bits = (8 - (total & 7)) & 7;
        if (br_overrun(&r) || (total + st->padding_bits) / 8 != len) return 0;
    } else {
        for (t = 0; t < st->k; ++t) {
            uint64_t start = r.used;
            if (!rans_read_table(&r, freq)) return 0;
            if (r.used - start != rans_table_cost_bits(freq)) return 0;
        }
        st->table_bits = r.used - before;
        st->padding_bits = (8 - (r.used & 7)) & 7;
        br_align(&r);
        if (br_overrun(&r) || br_bytepos(&r) + 4 > len) return 0;
        st->state_bytes = 4;
        st->payload_bits = ((uint64_t)len - br_bytepos(&r) - 4) * 8;
    }
    total = st->count_bits + st->map_bits + st->table_bits +
            st->padding_bits + st->payload_bits + 8 * st->state_bytes;
    if (total != (uint64_t)len * 8) return 0;
    st->available = 1;
    return 1;
}
#undef DCC_FN
#undef DCC_JOIN
#undef DCC_JOIN_
