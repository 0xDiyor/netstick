#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define MALLOC_CAP_DMA 8
static inline size_t heap_caps_get_free_size(int caps) { return caps & MALLOC_CAP_SPIRAM ? 8000 * 1024 : 210 * 1024; }
static inline void *heap_caps_malloc(size_t n, int c) { (void)c; return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t b, int c) { (void)c; return calloc(a, b); }
