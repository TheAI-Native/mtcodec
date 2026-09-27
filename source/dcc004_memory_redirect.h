/* Force-included ONLY in memory binary, never in the timed executable. */
#ifndef DCC004_MEMORY_REDIRECT_H
#define DCC004_MEMORY_REDIRECT_H
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include "dcc004_memory.h"
#define malloc dcc004_malloc
#define calloc dcc004_calloc
#define realloc dcc004_realloc
#define free dcc004_free
#endif
