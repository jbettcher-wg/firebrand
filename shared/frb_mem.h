/*
 * Range allocator for video memory. Taken from RadeonNI's hw/rdn_mem.[ch]
 * (frb_ names; nothing else changed).
 *
 * Hands out byte ranges of one contiguous region, first fit, with
 * power-of-two alignment. It knows nothing about the card; the caller
 * decides what the region is.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef FRB_MEM_H
#define FRB_MEM_H

#include "frb_os.h"

struct frb_mem_block;

struct frb_mem {
	struct frb_os *os;
	struct frb_mem_block *head;
	uint64_t base, size;
};

int frb_mem_init(struct frb_mem *mem, struct frb_os *os, uint64_t base,
		 uint64_t size);
void frb_mem_fini(struct frb_mem *mem);

/* Zero size/alignment mean 1; otherwise align is a power of two. */
int frb_mem_alloc(struct frb_mem *mem, uint64_t size, uint64_t align,
		  uint64_t *offset);
/* Inside [lo, hi). Failure returns -ENOMEM without changing offset or the heap. */
int frb_mem_alloc_range(struct frb_mem *mem, uint64_t size, uint64_t align,
			uint64_t lo, uint64_t hi, uint64_t *offset);
/* Strict variant: size and alignment must be nonzero. */
int frb_mem_alloc_in_range(struct frb_mem *mem, uint64_t size, uint64_t align,
			  uint64_t minimum, uint64_t maximum, uint64_t *offset);
/* Free the range that starts at `offset`; unknown offsets are ignored. */
void frb_mem_free(struct frb_mem *mem, uint64_t offset);
/* Bytes not handed out. */
uint64_t frb_mem_available(struct frb_mem *mem);
/* Bytes not handed out inside [lo, hi), and (if `largest` is not NULL) the largest free piece there. */
uint64_t frb_mem_available_range(struct frb_mem *mem, uint64_t lo, uint64_t hi,
				 uint64_t *largest);
uint64_t frb_mem_largest(struct frb_mem *mem);

#endif /* FRB_MEM_H */
