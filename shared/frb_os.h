/*
 * OS layer for Firebrand's hardware library (shared/ and amdgpu/).
 *
 * Everything below the kext reaches the outside world only through this
 * structure, so the same code runs in the kernel extension, in the trace
 * replay test on the host (little-endian) and on PowerPC (big-endian).
 * Taken from RadeonNI's hw/rdn_os.h with the frb_ prefix.
 *
 * The library is not thread-safe: the caller serialises all entry.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef FRB_OS_H
#define FRB_OS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__BIG_ENDIAN__) || \
	(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define FRB_BIG_ENDIAN 1
#else
#define FRB_BIG_ENDIAN 0
#endif

enum frb_log_level {
	FRB_LOG_ERROR,
	FRB_LOG_INFO,
	FRB_LOG_DEBUG
};

/*
 * Register values cross this interface in CPU byte order. The card's
 * registers are little-endian; the implementation does the swap on a
 * big-endian host. Offsets are byte offsets into the respective BAR.
 */
/* A GTT object: the CPU's view (cached, coherent through the snooped inbound window) and the GPU's */
struct frb_gtt {
	void *cpu;
	uint64_t gpu_addr;	/* in the GART aperture */
	uint32_t size;		/* bytes, whole pages */
	uint32_t first_page;	/* GART page index */
	void *handle;		/* the OS layer's */
};

struct frb_os {
	void *cookie;

	/* Register BAR (BAR5 on VI), 32-bit accesses at byte offsets. */
	uint32_t (*mmio_read32)(void *cookie, uint32_t offset);
	void (*mmio_write32)(void *cookie, uint32_t offset, uint32_t value);

	/* PCI configuration space of the VGA function. */
	uint32_t (*cfg_read32)(void *cookie, uint32_t offset);
	void (*cfg_write32)(void *cookie, uint32_t offset, uint32_t value);

	/* Busy-wait or sleep for at least this many microseconds. */
	void (*delay_us)(void *cookie, uint32_t usec);

	/* Monotonic milliseconds, for timeouts. */
	uint64_t (*time_ms)(void *cookie);

	/* Zeroed allocation; NULL on failure. */
	void *(*alloc)(void *cookie, size_t size);
	void (*free)(void *cookie, void *ptr);

	/*
	 * A firmware image by its linux-firmware name without the directory
	 * ("polaris12_mc.bin"): 0 and the bytes (owned by the OS layer, valid
	 * while the device lives), or a negative errno.
	 */
	int (*firmware)(void *cookie, const char *name, const uint8_t **data,
			uint32_t *size);

	/* One complete message, no trailing newline required. */
	void (*log)(void *cookie, enum frb_log_level level, const char *fmt,
		    va_list ap);

	/*
	 * GTT: wired, page-aligned system memory bound into the GART, as
	 * amdgpu's TTM GTT objects. NULL where there is none (the replay).
	 */
	int (*gtt_alloc)(void *cookie, uint32_t size, struct frb_gtt *gtt);
	void (*gtt_free)(void *cookie, struct frb_gtt *gtt);
};

static inline void frb_log(struct frb_os *os, enum frb_log_level level,
			   const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 3, 4)))
#endif
	;

static inline void frb_log(struct frb_os *os, enum frb_log_level level,
			   const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	os->log(os->cookie, level, fmt, ap);
	va_end(ap);
}

/*
 * Explicit little-endian accessors for byte buffers (VBIOS tables). These
 * make no assumption about alignment or host byte order.
 */
static inline uint8_t frb_get_u8(const void *base, uint32_t off)
{
	return ((const uint8_t *)base)[off];
}

static inline uint16_t frb_get_le16(const void *base, uint32_t off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t frb_get_le32(const void *base, uint32_t off)
{
	const uint8_t *p = (const uint8_t *)base + off;

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Byte-swap a CPU-order value to or from little-endian storage. */
static inline uint16_t frb_swap_le16(uint16_t v)
{
#if FRB_BIG_ENDIAN
	return (uint16_t)((v << 8) | (v >> 8));
#else
	return v;
#endif
}

static inline uint32_t frb_swap_le32(uint32_t v)
{
#if FRB_BIG_ENDIAN
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
#else
	return v;
#endif
}

/*
 * Little-endian stores into a byte buffer: every structure the GPU or the
 * SMU reads from memory (firmware TOC, page-table entries, ATOM parameter
 * blocks) is built with these, never by storing a CPU-order word.
 */
static inline void frb_put_le16(void *base, uint32_t off, uint16_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void frb_put_le32(void *base, uint32_t off, uint32_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

#endif /* FRB_OS_H */
