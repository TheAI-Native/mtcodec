/* Deterministic, non-corpus histogram fixtures; defined before comparison.
 * Generates no calls to the code under test. All arithmetic is uint32_t.
 */
#ifndef DCC_ORACLE_POOLS_H
#define DCC_ORACLE_POOLS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define DCC_ORACLE_POOLS 12
static size_t dcc_oracle_pool(int id, uint32_t *p) {
    static const size_t rows_for_id[12]={1,4,4,8,16,64,65,128,256,256,256,17};
    uint32_t state=UINT32_C(0x51c0de01); size_t rows,i,s;
    if (id<0 || id>=12) return 0;
    rows=rows_for_id[id]; memset(p,0,256*256*sizeof(*p));
    for (i=0;i<rows;++i) {
        for (s=0;s<256;++s) {
            uint32_t v=0;
            switch(id) {
            case 0: if (s==17) v=1024; break;
            case 1: break; /* all-zero raw histograms */
            case 2: if (s<16) v=32; break;
            case 3: if (s==i*29) v=1024; break;
            case 4: if (s==i%2 || s==64+i%2) v=512; break;
            case 5: if (s/16==i%4) v=(uint32_t)(1+(s%16)); break;
            case 6: if ((s+i)%13<2) v=(uint32_t)(1+i%7); break;
            case 7:
                state=state*UINT32_C(1664525)+UINT32_C(1013904223);
                if ((state>>24)<24) v=1+((state>>8)&255);
                break;
            case 8: if (s==i || s==(i+1)%256) v=256; break;
            case 9:
                state=state*UINT32_C(1664525)+UINT32_C(1013904223);
                if ((s+i%7)%17<4) v=1+((state>>10)&2047);
                break;
            case 10: v=(uint32_t)(1+s%11); break;
            case 11: if (s==0) v=8192; else if(s==(1+i)) v=1; break;
            }
            p[i*256+s]=v;
        }
    }
    return rows;
}
#endif
