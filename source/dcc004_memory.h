#ifndef DCC004_MEMORY_H
#define DCC004_MEMORY_H
#include <stddef.h>
#include <stdint.h>
void *dcc004_malloc(size_t);
void *dcc004_calloc(size_t,size_t);
void *dcc004_realloc(void*,size_t);
void dcc004_free(void*);
void dcc004_memory_begin(void);
uint64_t dcc004_memory_peak(void);
uint64_t dcc004_memory_live_delta(void);
#endif
