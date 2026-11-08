#ifndef MEM_H
#define MEM_H

#include "types.h"

#define MEM_TEMP_OBJECTS_MAX 8
#define MEM_HUNK_BYTES (16 * 1024 * 1024)

typedef struct {
	uint32_t bump, temp, bump_peak, temp_peak, total_peak, hunk_size;
} mem_stats_t;

void mem_init(void *base);
void mem_get_stats(mem_stats_t *s);

void *mem_bump(uint32_t size);
void *mem_mark(void);
void mem_reset(void *p);

void *mem_temp_alloc(uint32_t size);
void mem_temp_free(void *p);
void mem_temp_check(void);

#endif
