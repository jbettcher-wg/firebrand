/*
 * The device on Mac OS X: the kext's accelerator user client
 * (hw/rdn_user.h). The kext must be loaded with its 3D engine started
 * (RDN_ACCEL=1 scripts/kext.sh up).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include "rdn_iokit.h"

#include "rdn_device.h"
#include "hw/rdn_user.h"
#include "hw/rdn_mem.h"

/*
 * Memory behind the GART comes in chunks: a piece of this program's own
 * address space, wired and entered in the page table by the kext with one
 * call, and divided up here. A buffer larger than a chunk gets a chunk of
 * its own size, which goes back to the kext when the buffer does: kept, it
 * would hold a slot that only a buffer of at most that size can use again,
 * and a program with many large buffers of different sizes (Call of Duty
 * 2, 17 to 22 MB each) ran out of slots with most of the GART free.
 */
#define GART_CHUNK_BYTES	(16u << 20)
#define GART_CHUNKS		256
/*
 * What one program may have bound at a time. Above some amount the kext's
 * call to bind never returns (Call of Duty 2 on the G5, about 860 MB
 * wired: the thread sleeps in the kernel and cannot be killed), so the
 * program is refused long before.
 */
#define GART_MOST_BYTES		(512u << 20)

struct gart_chunk {
	uint8_t *cpu;
	uint32_t offset, size;		/* in the GART's range */
	struct rdn_mem mem;
};

struct darwin_device {
	struct rdn_device base;
	io_connect_t conn;
	struct rdn_os mem_os;
	struct gart_chunk gart[GART_CHUNKS];
	uint32_t gart_bytes;
	bool has_vram64;
	const volatile struct rdn_user_status *status;
#ifdef __LP64__
	mach_vm_address_t status_va;
	mach_vm_address_t slice_va[RDN_UC_VRAM_MAX_SLICES];
	uint32_t slice_count;
#else
	vm_address_t status_va;
#endif
};

static bool has_u4_pcie_parent(io_registry_entry_t service)
{
	io_registry_entry_t entry = service, parent;
	bool found = false;
	unsigned depth;

	IOObjectRetain(entry);
	for (depth = 0; depth < 32; depth++) {
		CFTypeRef value = IORegistryEntryCreateCFProperty(entry,
			CFSTR("compatible"), kCFAllocatorDefault, 0);
		if (value && CFGetTypeID(value) == CFDataGetTypeID()) {
			const UInt8 *data = CFDataGetBytePtr((CFDataRef)value);
			CFIndex length = CFDataGetLength((CFDataRef)value), at;
			for (at = 0; at + 8 <= length; at++) {
				if ((!at || !data[at - 1]) && !memcmp(data + at, "u4-pcie", 8))
					found = true;
			}
		}
		if (value)
			CFRelease(value);
		if (found || IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent))
			break;
		IOObjectRelease(entry);
		entry = parent;
	}
	IOObjectRelease(entry);
	return found;
}

static int dev_alloc(struct rdn_device *dev, uint64_t size, uint64_t align,
		     uint64_t *offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int off = 0;

	if (d->has_vram64) {
		int hi = 0, lo = 0;

		if (align > 0x10000000ull ||
		    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_ALLOC64, 4, 2,
						  (int)(size >> 32), (int)(uint32_t)size,
						  (int)(uint32_t)align,
						  (int)RDN_ALLOC64_APERTURE256,
						  &hi, &lo))
			return -1;
		*offset = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
		return 0;
	}
	if (size > 0xffffffffull || align > 0x10000000ull ||
	    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_ALLOC, 2, 1,
					  (int)size, (int)align, &off))
		return -1;
	*offset = (uint32_t)off;
	return 0;
}

static int dev_alloc_hidden(struct rdn_device *dev, uint64_t size, uint64_t align,
			    uint64_t *offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int off = 0;

	if (d->has_vram64) {
		int hi = 0, lo = 0;

		if (align > 0x10000000ull ||
		    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_ALLOC64, 4, 2,
						  (int)(size >> 32), (int)(uint32_t)size,
						  (int)(uint32_t)align,
						  (int)RDN_ALLOC64_HIDDEN,
						  &hi, &lo))
			return -1;
		*offset = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
		return 0;
	}
	if (size > 0xffffffffull || align > 0x10000000ull ||
	    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_ALLOC_HIDDEN, 2, 1,
					  (int)size, (int)align, &off))
		return -1;
	*offset = (uint32_t)off;
	return 0;
}

static void *mem_os_alloc(void *cookie, size_t size)
{
	(void)cookie;
	return calloc(1, size);
}

static void mem_os_free(void *cookie, void *ptr)
{
	(void)cookie;
	free(ptr);
}

static int bind_address(struct darwin_device *d, uintptr_t address,
                        uint32_t bytes, int *offset)
{
#ifdef __LP64__
	return IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_BIND64, 3, 1,
		(uint32_t)address, (uint32_t)(address >> 32), bytes, offset);
#else
	return IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_BIND, 2, 1,
		(uint32_t)address, bytes, offset);
#endif
}

/* A new chunk of at least `size` bytes in a free slot; NULL if there is none. */
static struct gart_chunk *gart_new_chunk(struct darwin_device *d, uint64_t size,
					 uint64_t align)
{
	vm_address_t addr = 0;
	/*
	 * A chunk for one large buffer is a little larger than it: the
	 * chunk's start in the GART is only page aligned, and the buffer's
	 * own alignment may move it, which an exact fit cannot take (a
	 * 256 MB buffer was refused every time and its chunk stayed bound).
	 */
	vm_size_t bytes = size > GART_CHUNK_BYTES ?
		(vm_size_t)((size + align + 4095) & ~4095ull) : GART_CHUNK_BYTES;
	struct gart_chunk *c = NULL;
	int i, offset = 0;

	for (i = 0; i < GART_CHUNKS && !c; i++)
		if (!d->gart[i].cpu)
			c = &d->gart[i];
	if (!c || bytes > GART_MOST_BYTES - d->gart_bytes || vm_allocate(mach_task_self(), &addr, bytes, VM_FLAGS_ANYWHERE)) {
		if (getenv("RDN_STATS"))
			fprintf(stderr, "rdn: GART chunk of %lu bytes: %s (%u bound)\n", (unsigned long)bytes,
				!c ? "no free slot" : bytes > GART_MOST_BYTES - d->gart_bytes ? "over the limit" : "vm_allocate failed",
				(unsigned)d->gart_bytes);
		if (getenv("RDN_STATS") && bytes > GART_MOST_BYTES - d->gart_bytes) {
			int k;

			for (k = 0; k < GART_CHUNKS; k++)
				if (d->gart[k].cpu)
					fprintf(stderr, "rdn:   chunk %d: %u KB at %u, %llu KB free\n", k,
						(unsigned)(d->gart[k].size >> 10), (unsigned)d->gart[k].offset,
						(unsigned long long)(rdn_mem_available(&d->gart[k].mem) >> 10));
		}
		return NULL;
	}
	{
		int br = bind_address(d, addr, bytes, &offset);

		if (br || rdn_mem_init(&c->mem, &d->mem_os, (uint32_t)offset, bytes)) {
			if (getenv("RDN_STATS"))
				fprintf(stderr, "rdn: GART chunk of %lu bytes: bind returned 0x%x\n",
					(unsigned long)bytes, (unsigned)br);
			vm_deallocate(mach_task_self(), addr, bytes);
			return NULL;
		}
	}
	c->cpu = (uint8_t *)addr;
	c->offset = (uint32_t)offset;
	c->size = (uint32_t)bytes;
	d->gart_bytes += (uint32_t)bytes;
	return c;
}

/*
 * Chunks of the ordinary size stay bound for the buffers to come, which
 * counts against what one program may have bound (GART_MOST_BYTES). Give
 * back those with nothing in them when the room is wanted: piglit's
 * large-tex had 272 MB of them idle and was refused a 256 MB buffer.
 */
static void gart_release_idle(struct darwin_device *d)
{
	int i;

	for (i = 0; i < GART_CHUNKS; i++) {
		struct gart_chunk *c = &d->gart[i];

		if (!c->cpu || c->size != GART_CHUNK_BYTES ||
		    rdn_mem_available(&c->mem) != c->size)
			continue;
		rdn_mem_fini(&c->mem);
		IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_UNBIND, 1, 0,
					      (int)c->offset);
		vm_deallocate(mach_task_self(), (vm_address_t)c->cpu, c->size);
		c->cpu = NULL;
		d->gart_bytes -= c->size;
	}
}

/*
 * A mode set changes the screen's size and pitch. The window server's
 * program does not open the device again, so ask the kext, at most once
 * in 20 ms (this is called at every GL context switch of the window
 * server).
 */
static void dev_screen_refresh(struct rdn_device *dev)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_info info;
	IOByteCount size = sizeof(info);
	static uint64_t last;
	uint64_t now = mach_absolute_time();
	static mach_timebase_info_data_t tb;

	if (!tb.denom)
		mach_timebase_info(&tb);
	if (last && (now - last) * tb.numer / tb.denom < 20000000ull)
		return;
	last = now;
	if (IOConnectMethodScalarIStructureO(d->conn, RDN_UC_GET_INFO, 0, &size, &info) ||
	    info.version != RDN_USER_VERSION)
		return;
	if (info.fb_bits_per_pixel == 32) {
		d->base.screen.offset = info.fb_offset;
		d->base.screen.width = info.fb_width;
		d->base.screen.height = info.fb_height;
		d->base.screen.pitch_pixels = info.fb_pitch_pixels;
	} else {
		d->base.screen.width = 0;
	}
}

static int dev_gart_alloc(struct rdn_device *dev, uint64_t size, uint64_t align,
			  uint64_t *offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct gart_chunk *c;
	int i;

	if (size > (256u << 20))
		return -1;
	for (i = 0; i < GART_CHUNKS && size <= GART_CHUNK_BYTES; i++)
		if (d->gart[i].cpu && d->gart[i].size == GART_CHUNK_BYTES &&
		    !rdn_mem_alloc(&d->gart[i].mem, size, align, offset))
			return 0;
	c = gart_new_chunk(d, size, align);
	if (!c && size > GART_CHUNK_BYTES) {
		gart_release_idle(d);
		c = gart_new_chunk(d, size, align);
	}
	if (!c)
		return -1;
	if (rdn_mem_alloc(&c->mem, size, align, offset)) {
		/* Nothing came of it: do not keep the chunk. */
		if (c->size > GART_CHUNK_BYTES) {
			rdn_mem_fini(&c->mem);
			IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_UNBIND, 1, 0,
						      (int)c->offset);
			vm_deallocate(mach_task_self(), (vm_address_t)c->cpu, c->size);
			c->cpu = NULL;
			d->gart_bytes -= c->size;
		}
		return -1;
	}
	return 0;
}

static struct gart_chunk *gart_chunk_of(struct darwin_device *d, uint64_t offset)
{
	int i;

	for (i = 0; i < GART_CHUNKS; i++)
		if (d->gart[i].cpu && offset >= d->gart[i].offset &&
		    offset - d->gart[i].offset < d->gart[i].size)
			return &d->gart[i];
	return NULL;
}

static void dev_gart_free(struct rdn_device *dev, uint64_t offset)
{
	struct gart_chunk *c = gart_chunk_of((struct darwin_device *)dev, offset);

	struct darwin_device *d = (struct darwin_device *)dev;

	if (!c)
		return;
	rdn_mem_free(&c->mem, offset);
	/*
	 * A chunk of the ordinary size stays bound: the next buffers will
	 * want it. One made for a single large buffer goes back.
	 */
	if (c->size > GART_CHUNK_BYTES) {
		rdn_mem_fini(&c->mem);
		IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_UNBIND, 1, 0,
					      (int)c->offset);
		vm_deallocate(mach_task_self(), (vm_address_t)c->cpu, c->size);
		c->cpu = NULL;
		d->gart_bytes -= c->size;
	}
}

static int dev_gart_bind_user(struct rdn_device *dev, void *address, uint64_t size,
			      uint64_t *offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int at = 0;

	if (((uintptr_t)address & 4095) || (size & 4095) || !size ||
	    size > GART_MOST_BYTES - d->gart_bytes ||
	    bind_address(d, (uintptr_t)address, (uint32_t)size, &at))
		return -1;
	d->gart_bytes += (uint32_t)size;
	*offset = (uint32_t)at;
	return 0;
}

static void dev_gart_unbind_user(struct rdn_device *dev, uint64_t offset, uint64_t size)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_UNBIND, 1, 0, (int)offset);
	d->gart_bytes -= (uint32_t)size;
}

static void *dev_gart_cpu(struct rdn_device *dev, uint64_t offset)
{
	struct gart_chunk *c = gart_chunk_of((struct darwin_device *)dev, offset);

	return c ? c->cpu + (offset - c->offset) : NULL;
}

static void dev_free(struct rdn_device *dev, uint64_t offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	if (d->has_vram64) {
		IOConnectMethodScalarIScalarO(d->conn, RDN_UC_FREE64, 2, 0,
					      (int)(offset >> 32), (int)(uint32_t)offset);
		return;
	}
	IOConnectMethodScalarIScalarO(d->conn, RDN_UC_FREE, 1, 0, (int)offset);
}

static int dev_submit(struct rdn_device *dev, uint64_t offset, uint32_t words,
		      uint32_t *fence)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int seq = 0;

	if (d->has_vram64) {
		if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SUBMIT64, 3, 1,
						  (int)(offset >> 32), (int)(uint32_t)offset,
						  (int)words, &seq))
			return -1;
		*fence = (uint32_t)seq;
		return 0;
	}
	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SUBMIT, 2, 1,
					  (int)offset, (int)words, &seq))
		return -1;
	*fence = (uint32_t)seq;
	return 0;
}

static inline bool status_fence_done(struct darwin_device *d, uint32_t fence)
{
	if (!d->status || d->status->version != RDN_STATUS_VERSION)
		return false;
	uint32_t done = d->status->fence_completed;
#if defined(__ppc__) || defined(__ppc64__)
	__asm__ __volatile__("lwsync" : : : "memory");
#endif
	return (int32_t)(done - fence) >= 0;
}

static bool fence_wait_ms(struct darwin_device *d, uint32_t fence, uint32_t ms)
{
	int reached = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_FENCE_WAIT, 2, 1,
					  (int)fence, (int)ms, &reached))
		return false;
	return reached != 0;
}

/*
 * The video decoder (UVD). The kext has the methods only when its
 * personality has the firmware; the decoder is started by the first
 * command stream that wants it.
 */
static int dev_uvd_init(struct rdn_device *dev)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int up = 0, vcpu = 0, ring = 0, status = 0;

	if (!IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring) &&
	    up)
		return 0;
	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_INIT, 1, 1, 0, &status)) {
		fprintf(stderr, "rdn: the video decoder did not start\n");
		return -1;
	}
	return 0;
}

static int dev_uvd_epoch(struct rdn_device *dev, uint32_t *epoch)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int e = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_EPOCH, 0, 1, &e))
		return -1;
	*epoch = (uint32_t)e;
	return 0;
}

static int dev_uvd_reset(struct rdn_device *dev, uint32_t *epoch)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int e = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_RESET, 0, 1, &e))
		return -1;
	*epoch = (uint32_t)e;
	return 0;
}

static int dev_uvd_submit(struct rdn_device *dev, uint64_t offset, uint32_t words,
			  uint32_t *fence)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int seq = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_SUBMIT, 2, 1,
					  (int)offset, (int)words, &seq))
		return -1;
	*fence = (uint32_t)seq;
	return 0;
}

/*
 * The decoder in a segment (docs/UVD.md). An older kext lacks RDN_UC_UVD_SEGMENT: then the flag is not sent (it would
 * select a start variant) and the decoder is in the aperture.
 */
static int dev_uvd_start(struct rdn_device *dev, int want, uint64_t *first, uint64_t *size, bool *split)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int up = 0, vcpu = 0, ring = 0, status = 0, mode = 0, f = 0, s = 0;
	bool known = !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_SEGMENT, 0, 3, &mode, &f, &s);

	*first = *size = 0;
	*split = false;
	if (!(!IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring) && up) &&
	    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_INIT, 1, 1,
					  known && want == 1 ? (int)RDN_UC_UVD_F_SEGMENT :
					  known && want == 2 ? (int)RDN_UC_UVD_F_SPLIT : 0, &status)) {
		fprintf(stderr, "rdn: the video decoder did not start\n");
		return -1;
	}
	if (known && !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_SEGMENT, 0, 3, &mode, &f, &s) && mode) {
		*first = (uint32_t)f;
		*size = (uint32_t)s;
		*split = mode == 2;
	}
	return 0;
}

static int dev_uvd_alloc(struct rdn_device *dev, uint64_t size, uint64_t align, uint64_t *offset)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int off = 0;

	if (size > 0xffffffffull || align > 0x10000000ull ||
	    IOConnectMethodScalarIScalarO(d->conn, RDN_UC_ALLOC_UVD, 2, 1, (int)size, (int)align, &off))
		return -1;
	*offset = (uint32_t)off;
	return 0;
}

/* The kext takes at most 16 MB at a time. */
static int dev_vram_put(struct rdn_device *dev, uint64_t offset, const void *src, uint64_t bytes)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	const uint8_t *p = (const uint8_t *)src;

	while (bytes) {
		uint64_t n = bytes < (4u << 20) ? bytes : (4u << 20);

		if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_VRAM_PUT, 3, 0, (int)offset, (int)(uintptr_t)p, (int)n))
			return -1;
		offset += n;
		p += n;
		bytes -= n;
	}
	return 0;
}

static int dev_vram_get(struct rdn_device *dev, uint64_t offset, void *dst, uint64_t bytes)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	uint8_t *p = (uint8_t *)dst;

	while (bytes) {
		uint64_t n = bytes < (4u << 20) ? bytes : (4u << 20);

		if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_VRAM_GET, 3, 0, (int)offset, (int)(uintptr_t)p, (int)n))
			return -1;
		offset += n;
		p += n;
		bytes -= n;
	}
	return 0;
}

static int dev_uvd_submit_words(struct rdn_device *dev, const uint32_t *words, uint32_t count, uint32_t *fence)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int seq = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_SUBMIT_WORDS, 2, 1, (int)(uintptr_t)words, (int)count, &seq))
		return -1;
	*fence = (uint32_t)seq;
	return 0;
}

static long env_number(const char *name);
static uint64_t now_us(void);

static int dev_uvd_fence_wait(struct rdn_device *dev, uint32_t fence, uint64_t timeout_ns)
{
	static long spin_us = -1;
	struct darwin_device *d = (struct darwin_device *)dev;
	uint64_t ms = timeout_ns / 1000000;
	int reached = 0;

	/*
	 * Look for a while before the kext's sleeping wait, whose polls are a millisecond or more
	 * apart: a small picture is decoded in well under that. RDN_FENCE_SPIN (microseconds) as for
	 * the 3D engine; 2 ms without it.
	 */
	if (spin_us < 0)
		spin_us = getenv("RDN_FENCE_SPIN") ? env_number("RDN_FENCE_SPIN") : 2000;
	if (spin_us && timeout_ns) {
		uint64_t start = now_us();

		do
			if (!IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_FENCE_WAIT, 2, 1, (int)fence, 0,
							   &reached) && reached)
				return 0;
		while (now_us() - start < (uint64_t)spin_us);
	}

	/* As for the 3D engine: "forever" still ends. */
	if (ms > 10000)
		ms = 10000;
	if (timeout_ns && !ms)
		ms = 1;
	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_FENCE_WAIT, 2, 1,
					  (int)fence, (int)ms, &reached))
		return -1;
	return reached ? 0 : -1;
}

static bool dev_fence_done(struct rdn_device *dev, uint32_t fence)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	if (status_fence_done(d, fence))
		return true;
	return fence_wait_ms(d, fence, 0);
}

/*
 * RDN_FENCE_SPIN=microseconds: look that long without sleeping before the
 * kext's wait, which sleeps between looks. RDN_STATS=1: say at exit how
 * many waits there were and how long they took.
 */
static long env_number(const char *name)
{
	const char *s = getenv(name);

	return s ? atol(s) : 0;
}

static uint64_t now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/*
 * How the CPU's view of video memory is mapped: write combining, which on
 * PowerPC is uncached without the guard bit, so that the processor may
 * gather stores. With the kernel's default for device memory (uncached
 * and guarded) copying vertices and textures in was an eighth of a
 * Quake 3 frame. RDN_APERTURE_CACHE=inhibit asks for that again.
 */
static IOOptionBits aperture_cache_mode(void)
{
	const char *s = getenv("RDN_APERTURE_CACHE");

	if (s && !strcmp(s, "inhibit"))
		return kIOMapInhibitCache;
	if (s && !strcmp(s, "default"))
		return kIOMapDefaultCache;
	return kIOMapWriteCombineCache;
}

static struct {
	unsigned waits, spun, slept;
	uint64_t us, max_us;
	unsigned hist[8];	/* under 0.25, 0.5, 1, 2, 4, 8, 16 ms, more */
} wait_stats;

static void wait_stats_print(void)
{
	fprintf(stderr, "rdn stats: %u fence waits (%u ended while spinning, %u slept), "
		"%llu ms in all, longest %llu us; under 0.25/0.5/1/2/4/8/16 ms and over: "
		"%u %u %u %u %u %u %u %u\n",
		wait_stats.waits, wait_stats.spun, wait_stats.slept,
		(unsigned long long)(wait_stats.us / 1000),
		(unsigned long long)wait_stats.max_us,
		wait_stats.hist[0], wait_stats.hist[1], wait_stats.hist[2],
		wait_stats.hist[3], wait_stats.hist[4], wait_stats.hist[5],
		wait_stats.hist[6], wait_stats.hist[7]);
}

static int dev_fence_wait(struct rdn_device *dev, uint32_t fence,
			  uint64_t timeout_ns)
{
	static long spin_us = -1, stats;
	struct darwin_device *d = (struct darwin_device *)dev;
	uint64_t ms = timeout_ns / 1000000;
	uint64_t start, us, limit;
	bool reached = false;
	unsigned i;

	if (status_fence_done(d, fence))
		return 0;
	if (spin_us < 0) {
		spin_us = getenv("RDN_FENCE_SPIN") ? env_number("RDN_FENCE_SPIN") : 2000;
		stats = env_number("RDN_STATS");
		if (stats)
			atexit(wait_stats_print);
	}
	/* "Forever" still ends: a hung GPU must not hang the process. */
	if (ms > 10000)
		ms = 10000;
	if (!ms)
		ms = 1;
	start = (spin_us || stats) ? now_us() : 0;
	if (spin_us) {
		do
			reached = status_fence_done(d, fence) || fence_wait_ms(d, fence, 0);
		while (!reached && now_us() - start < (uint64_t)spin_us);
		if (reached)
			wait_stats.spun++;
	}
	if (!reached) {
		reached = fence_wait_ms(d, fence, (uint32_t)ms);
		wait_stats.slept++;
	}
	if (stats) {
		us = now_us() - start;
		wait_stats.waits++;
		wait_stats.us += us;
		if (us > wait_stats.max_us)
			wait_stats.max_us = us;
		for (i = 0, limit = 250; i < 7 && us >= limit; i++, limit *= 2)
			;
		wait_stats.hist[i]++;
	}
	return reached ? 0 : -1;
}

static int dev_reg_read(struct rdn_device *dev, uint32_t reg, uint32_t *value)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int v = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_REG_READ, 1, 1, (int)reg, &v))
		return -1;
	*value = (uint32_t)v;
	return 0;
}

static void dev_sync_for_cpu(struct rdn_device *dev)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SYNC_FOR_CPU, 0, 0);
}

static bool dev_surface_region(struct rdn_device *dev, uint32_t id,
			       struct rdn_region *region)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	static struct rdn_user_region r;
	IOByteCount size = sizeof(r);
	uint32_t n;

	if (IOConnectMethodScalarIStructureO(d->conn, RDN_UC_SURFACE_REGION, 1,
					     &size, (int)id, &r))
		return false;
	region->count = r.count;
	memcpy(region->bounds, r.bounds, sizeof(region->bounds));
	n = r.count < RDN_USER_REGION_RECTS ? r.count : RDN_USER_REGION_RECTS;
	if (n > RDN_REGION_RECTS)
		n = RDN_REGION_RECTS;
	memcpy(region->rects, r.rects, n * sizeof(r.rects[0]));
	return true;
}

static bool dev_surface_backing(struct rdn_device *dev, uint32_t id,
                                void **address, uint32_t *stride,
                                uint32_t *width, uint32_t *height)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_backing b;
	IOByteCount size = sizeof(b);
	if (IOConnectMethodScalarIStructureO(d->conn, RDN_UC_SURFACE_BACKING, 1,
	                                    &size, (int)id, &b) || size != sizeof(b))
		return false;
	uint64_t ptr = ((uint64_t)(b.address_hi & ~RDN_USER_BACKING_STALE) << 32) | b.address_lo;
	if (!ptr || ptr > UINTPTR_MAX || !b.width || !b.height ||
	    b.width > 16384 || b.height > 16384 || b.row_bytes < b.width * 4)
		return false;
	*address = (void *)(uintptr_t)ptr;
	*stride = b.row_bytes;
	*width = b.width;
	*height = b.height;
	return true;
}

static bool dev_surface_backing_stale(struct rdn_device *dev, uint32_t id)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_backing b;
	IOByteCount size = sizeof(b);
	if (IOConnectMethodScalarIStructureO(d->conn, RDN_UC_SURFACE_BACKING, 1,
	                                    &size, (int)id, &b) || size != sizeof(b))
		return false;
	return (b.address_hi & RDN_USER_BACKING_STALE) != 0;
}

static bool dev_surface_buffer(struct rdn_device *dev, uint32_t id,
			       uint64_t offset, uint32_t row_bytes,
			       uint32_t width, uint32_t height)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	return IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SURFACE_BUFFER, 5, 0,
					     (int)id, (int)offset, (int)row_bytes,
					     (int)width, (int)height) == 0;
}

static void dev_surface_gl(struct rdn_device *dev, uint32_t id)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	(void)IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SURFACE_GL, 1, 0, (int)id);
}

static bool dev_surface_acquire(struct rdn_device *dev, uint32_t id)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int copied = 0;

	/* An older kext has no such method and answers with an error: nothing to do then. */
	return IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SURFACE_ACQUIRE, 1, 1,
					     (int)id, &copied) == 0 && copied;
}

static bool dev_surface_init(struct rdn_device *dev, uint32_t id,
			     uint64_t offset, uint32_t row_bytes,
			     uint32_t width, uint32_t height,
			     uint64_t old_offset, uint32_t old_row_bytes,
			     uint32_t old_width, uint32_t old_height,
			     bool keep_old, bool *synced)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_surface_init in;
	struct rdn_user_surface_init_out out;
	IOByteCount out_size = sizeof(out);

	if (synced)
		*synced = false;
	if (offset > 0xffffffffULL || old_offset > 0xffffffffULL)
		return false;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));
	in.id = id;
	in.offset = (uint32_t)offset;
	in.row_bytes = row_bytes;
	in.width = width;
	in.height = height;
	if (keep_old) {
		in.old_offset = (uint32_t)old_offset;
		in.old_row_bytes = old_row_bytes;
		in.old_width = old_width;
		in.old_height = old_height;
		in.flags = RDN_USER_SURFACE_KEEP_OLD;
	}
	if (IOConnectMethodStructureIStructureO(d->conn, RDN_UC_SURFACE_INIT,
						sizeof(in), &out_size,
						&in, &out) ||
	    out_size != sizeof(out))
		return false;
	if (synced)
		*synced = out.synced != 0;
	return true;
}

static int dev_surface_list(struct rdn_device *dev, struct rdn_surface *list,
				 uint32_t max)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_surfaces s;
	IOByteCount size = sizeof(s);
	uint32_t i;

	if (IOConnectMethodScalarIStructureO(d->conn, RDN_UC_SURFACE_LIST, 0,
					     &size, &s))
		return -1;
	for (i = 0; i < s.count && i < max && i < RDN_USER_SURFACES; i++) {
		list[i].id = s.surface[i].id;
		list[i].offset = s.surface[i].offset;
		list[i].row_bytes = s.surface[i].row_bytes;
		list[i].width = s.surface[i].width;
		list[i].height = s.surface[i].height;
	}
	return i;
}

static bool dev_surface_query(struct rdn_device *dev, uint32_t id,
                              struct rdn_surface *surface, bool *found)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	struct rdn_user_surface_info info;
	IOByteCount size = sizeof(info);
	kern_return_t ret;

	*found = false;
	memset(surface, 0, sizeof(*surface));
	ret = IOConnectMethodScalarIStructureO(d->conn, RDN_UC_SURFACE_QUERY, 1,
	                                      &size, (int)id, &info);
	if (ret == kIOReturnUnsupported) {
		/* Older kernels have only the original 32-slot registry. */
		struct rdn_surface list[RDN_USER_SURFACES];
		int n = dev_surface_list(dev, list, RDN_USER_SURFACES);
		if (n < 0) return false;
		for (int i = 0; i < n; ++i) {
			if (list[i].id == id) {
				*surface = list[i];
				*found = true;
				break;
			}
		}
		return true;
	}
	if (ret || size != sizeof(info) || (info.id && info.id != id))
		return false;
	surface->id = info.id;
	surface->offset = info.offset;
	surface->row_bytes = info.row_bytes;
	surface->width = info.width;
	surface->height = info.height;
	*found = info.id != 0;
	return true;
}

static uint32_t dev_surface_locked(struct rdn_device *dev)
{
	struct darwin_device *d = (struct darwin_device *)dev;
	int id = 0;

	if (IOConnectMethodScalarIScalarO(d->conn, RDN_UC_SURFACE_LOCKED, 0, 1, &id))
		return 0;
	return (uint32_t)id;
}

static void dev_destroy(struct rdn_device *dev)
{
	struct darwin_device *d = (struct darwin_device *)dev;

	/* Closing frees what this process allocated and unmaps the aperture. */
	IOServiceClose(d->conn);
	free(d);
}

struct rdn_device *rdn_device_open(void)
{
	struct darwin_device *d = calloc(1, sizeof(*d));
	struct rdn_user_info info;
	IOByteCount size = sizeof(info);
	io_service_t service;
#ifdef __LP64__
	mach_vm_address_t addr = 0;
	mach_vm_size_t len = 0;
#else
	vm_address_t addr = 0;
	vm_size_t len = 0;
#endif
	kern_return_t kr;

	if (!d)
		return NULL;
	service = IOServiceGetMatchingService(kIOMasterPortDefault,
			IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service) {
		fprintf(stderr, "rdn: no %s service\n", RDN_UC_SERVICE_CLASS);
		goto fail;
	}
	kr = IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE_V2, &d->conn);
	if (has_u4_pcie_parent(service))
		d->base.info.cp_dma_gtt_copy_max_bytes = 128;
	IOObjectRelease(service);
	if (kr) {
		fprintf(stderr, "rdn: cannot open the accelerator (0x%x)\n", kr);
		goto fail;
	}
	kr = IOConnectMethodScalarIStructureO(d->conn, RDN_UC_GET_INFO, 0, &size,
					      &info);
	if (kr || info.version != RDN_USER_VERSION) {
		fprintf(stderr, "rdn: accelerator interface mismatch (0x%x, version %u)\n",
			kr, (unsigned)info.version);
		goto fail_close;
	}
	kr = IOConnectMapMemory(d->conn, RDN_UC_MEMORY_APERTURE, mach_task_self(),
				&addr, &len, kIOMapAnywhere | aperture_cache_mode());
	if (kr) {
		fprintf(stderr, "rdn: cannot map video memory (0x%x)\n", kr);
		goto fail_close;
	}

	d->base.info.pci_device_id = info.pci_device_id;
	d->base.info.vram_gpu_base = ((uint64_t)info.vram_gpu_base_hi << 32) |
				     info.vram_gpu_base_lo;
	d->base.info.vram_size = info.heap_size;
	/* The GART, if the kext has it on (boot argument rdn_gart=1). */
	{
		int on = 0, gpu_start = 0, pages = 0;

		d->mem_os.alloc = mem_os_alloc;
		d->mem_os.free = mem_os_free;
		if (!getenv("RDN_NO_GART") &&
		    !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_GART_INFO, 0, 3,
						   &on, &gpu_start, &pages) && on) {
			d->base.info.gart_gpu_base = (uint32_t)gpu_start;
			d->base.info.gart_size = (uint64_t)(uint32_t)pages * 4096;
			d->base.gart_alloc = dev_gart_alloc;
			d->base.gart_free = dev_gart_free;
			d->base.gart_cpu = dev_gart_cpu;
			if (!getenv("RDN_NO_USERPTR")) {
				d->base.gart_bind_user = dev_gart_bind_user;
				d->base.gart_unbind_user = dev_gart_unbind_user;
			}
		}
	}
	/* Map shared read-only status page for zero-syscall fence completion checks. */
	{
#ifdef __LP64__
		mach_vm_address_t st_addr = 0;
		mach_vm_size_t st_len = 0;
#else
		vm_address_t st_addr = 0;
		vm_size_t st_len = 0;
#endif
		if (!IOConnectMapMemory(d->conn, RDN_UC_MEMORY_STATUS, mach_task_self(),
					&st_addr, &st_len, kIOMapAnywhere | kIOMapReadOnly) &&
		    st_len >= sizeof(struct rdn_user_status)) {
			d->status_va = st_addr;
			d->status = (const volatile struct rdn_user_status *)(uintptr_t)st_addr;
		}
	}
	/* Prefer V2 64-bit VRAM info + ReBAR 8x1GB slice mapping when supported. */
	{
		struct rdn_user_vram_info64 v64;
		IOByteCount v64_size = sizeof(v64);

		memset(&v64, 0, sizeof(v64));
		if (!IOConnectMethodScalarIStructureO(d->conn, RDN_UC_VRAM_INFO64, 0,
						      &v64_size, &v64) &&
		    v64_size == sizeof(v64) && v64.version == 1) {
			uint64_t total_vram = ((uint64_t)v64.total_vram_hi << 32) | v64.total_vram_lo;
			uint64_t hidden_off = ((uint64_t)v64.hidden_offset_hi << 32) | v64.hidden_offset_lo;
			uint64_t hidden_sz = ((uint64_t)v64.hidden_size_hi << 32) | v64.hidden_size_lo;

			d->has_vram64 = true;
			d->base.info.total_vram_size = total_vram;
			d->base.info.le_ibs = (v64.flags & RDN_VRAM_INFO64_LE_IBS) != 0;
			if (!getenv("RDN_NO_HIDDEN_VRAM") && hidden_sz) {
				d->base.info.hidden_offset = hidden_off;
				d->base.info.hidden_size = hidden_sz;
				d->base.alloc_hidden = dev_alloc_hidden;
			}
#ifdef __LP64__
			if (v64.slice_count > 0 && v64.slice_count <= RDN_UC_VRAM_MAX_SLICES &&
			    v64.slice_bytes >= (16u << 20) && !getenv("RDN_NO_REBAR")) {
				uint64_t total_map = (uint64_t)v64.slice_count * v64.slice_bytes;
				vm_address_t base_va = 0;

				if (total_vram && total_map > total_vram)
					total_map = total_vram;
				if (!vm_allocate(mach_task_self(), &base_va,
						 (vm_size_t)total_map, VM_FLAGS_ANYWHERE)) {
					uint32_t i, mapped = 0;
					bool ok = true;

					for (i = 0; i < v64.slice_count; i++) {
						uint64_t off = (uint64_t)i * v64.slice_bytes;
						uint64_t want_len;
						mach_vm_address_t slice_va;
						mach_vm_size_t slice_len = 0;

						if (off >= total_map)
							break;
						want_len = total_map - off;
						if (want_len > v64.slice_bytes)
							want_len = v64.slice_bytes;
						slice_va = (mach_vm_address_t)base_va + off;
						vm_deallocate(mach_task_self(), (vm_address_t)slice_va,
							      (vm_size_t)want_len);
						if (IOConnectMapMemory(d->conn, RDN_UC_MEMORY_VRAM_SLICE(i),
								       mach_task_self(), &slice_va, &slice_len,
								       aperture_cache_mode()) ||
						    slice_va != (mach_vm_address_t)base_va + off ||
						    slice_len < want_len) {
							ok = false;
							if (off + want_len < total_map)
								vm_deallocate(mach_task_self(),
									      (vm_address_t)(base_va + off + want_len),
									      (vm_size_t)(total_map - (off + want_len)));
							break;
						}
						d->slice_va[i] = slice_va;
						mapped = i + 1;
					}
					if (ok && mapped > 0) {
						IOConnectUnmapMemory(d->conn, RDN_UC_MEMORY_APERTURE,
								     mach_task_self(), addr);
						addr = (mach_vm_address_t)base_va;
						len = (mach_vm_size_t)total_map;
						d->slice_count = mapped;
					} else {
						for (i = 0; i < mapped; i++)
							IOConnectUnmapMemory(d->conn, RDN_UC_MEMORY_VRAM_SLICE(i),
									     mach_task_self(), d->slice_va[i]);
					}
				}
			}
#endif
		} else {
			/* Older kext without RDN_UC_VRAM_INFO64: 32-bit hidden query. */
			int hidden_offset = 0, hidden_size = 0;

			if (!getenv("RDN_NO_HIDDEN_VRAM") &&
			    !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_HIDDEN_INFO, 0, 2,
							   &hidden_offset, &hidden_size) &&
			    hidden_size) {
				d->base.info.hidden_offset = (uint32_t)hidden_offset;
				d->base.info.hidden_size = (uint32_t)hidden_size;
				d->base.alloc_hidden = dev_alloc_hidden;
			}
		}
	}
	/* GFX9: the chip as amdgpu's device info describes it (Firebrand on SOC15); other kexts refuse the method. */
	{
		struct rdn_user_gfx_info g;
		IOByteCount g_size = sizeof(g);

		memset(&g, 0, sizeof(g));
		if (!IOConnectMethodScalarIStructureO(d->conn, RDN_UC_GFX_INFO, 0, &g_size, &g) &&
		    g.version == RDN_USER_GFX_INFO_VERSION && g_size >= sizeof(g)) {
			d->base.info.has_gfx = true;
			d->base.info.gfx.family = g.family;
			d->base.info.gfx.chip_external_rev = g.chip_external_rev;
			d->base.info.gfx.chip_rev = g.chip_rev;
			d->base.info.gfx.pci_rev = g.pci_rev;
			d->base.info.gfx.ip_major = (g.gfx_ip_version >> 16) & 0xff;
			d->base.info.gfx.ip_minor = (g.gfx_ip_version >> 8) & 0xff;
			d->base.info.gfx.ip_rev = g.gfx_ip_version & 0xff;
			d->base.info.gfx.me_fw_version = g.me_fw_version;
			d->base.info.gfx.me_fw_feature = g.me_fw_feature;
			d->base.info.gfx.pfp_fw_version = g.pfp_fw_version;
			d->base.info.gfx.pfp_fw_feature = g.pfp_fw_feature;
			d->base.info.gfx.mec_fw_version = g.mec_fw_version;
			d->base.info.gfx.mec_fw_feature = g.mec_fw_feature;
			d->base.info.gfx.gb_addr_config = g.gb_addr_config;
			d->base.info.gfx.num_shader_engines = g.num_shader_engines;
			d->base.info.gfx.num_sh_per_se = g.num_sh_per_se;
			d->base.info.gfx.num_cu_per_sh = g.num_cu_per_sh;
			d->base.info.gfx.num_cu = g.num_cu;
			memcpy(d->base.info.gfx.cu_bitmap, g.cu_bitmap, sizeof(g.cu_bitmap));
			d->base.info.gfx.enabled_rb_mask = g.enabled_rb_mask;
			d->base.info.gfx.num_rb_pipes = g.num_rb_pipes;
			d->base.info.gfx.num_tcc_blocks = g.num_tcc_blocks;
			d->base.info.gfx.num_shader_visible_vgprs = g.num_shader_visible_vgprs;
			d->base.info.gfx.gpu_counter_freq_khz = g.gpu_counter_freq_khz;
			d->base.info.gfx.engine_clock_khz = g.engine_clock_khz;
			d->base.info.gfx.memory_clock_khz = g.memory_clock_khz;
			d->base.info.gfx.clear_state = (g.flags & RDN_GFX_INFO_CLEAR_STATE) != 0;
		}
	}
	d->base.info.tile_config = info.tile_config;
	d->base.info.backend_map = info.backend_map;
	d->base.info.max_backends = info.max_backends;
	d->base.info.max_tile_pipes = info.max_tile_pipes;
	d->base.info.max_pipes = info.max_pipes;
	d->base.info.num_ses = info.num_ses;
	d->base.aperture = (void *)addr;
	d->base.aperture_size = len;
	if (info.fb_bits_per_pixel == 32) {
		d->base.screen.offset = info.fb_offset;
		d->base.screen.width = info.fb_width;
		d->base.screen.height = info.fb_height;
		d->base.screen.pitch_pixels = info.fb_pitch_pixels;
	}
	d->base.alloc = dev_alloc;
	d->base.free = dev_free;
	d->base.submit = dev_submit;
	d->base.fence_done = dev_fence_done;
	d->base.fence_wait = dev_fence_wait;
	d->base.sync_for_cpu = dev_sync_for_cpu;
	d->base.reg_read = dev_reg_read;
	d->base.destroy = dev_destroy;
	/* A kext without the decoder's firmware refuses the method. */
	{
		int up = 0, vcpu = 0, ring = 0;

		if (!getenv("RDN_NO_UVD") &&
		    !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_INFO, 0, 3, &up, &vcpu, &ring)) {
			d->base.uvd_init = dev_uvd_init;
			/* an older kext lacks these: the library then cannot recover a stalled decoder */
			if (!IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_EPOCH, 0, 1, &up)) {
				d->base.uvd_epoch = dev_uvd_epoch;
				d->base.uvd_reset = dev_uvd_reset;
			}
			d->base.uvd_submit = dev_uvd_submit;
			d->base.uvd_fence_wait = dev_uvd_fence_wait;
			/* the decoder in a segment: the kext tells by answering the method */
			if (!getenv("RDN_NO_UVD_SEGMENT") &&
			    !IOConnectMethodScalarIScalarO(d->conn, RDN_UC_UVD_SEGMENT, 0, 3, &up, &vcpu, &ring)) {
				d->base.uvd_start = dev_uvd_start;
				d->base.uvd_alloc = dev_uvd_alloc;
				d->base.vram_put = dev_vram_put;
				d->base.vram_get = dev_vram_get;
				d->base.uvd_submit_words = dev_uvd_submit_words;
			}
		}
	}
	d->base.surface_region = dev_surface_region;
	d->base.surface_backing = dev_surface_backing;
	d->base.surface_backing_stale = dev_surface_backing_stale;
	d->base.screen_refresh = dev_screen_refresh;
	d->base.surface_buffer = dev_surface_buffer;
	d->base.surface_gl = dev_surface_gl;
	d->base.surface_acquire = dev_surface_acquire;
	d->base.surface_init = dev_surface_init;
	d->base.surface_list = dev_surface_list;
	d->base.surface_query = dev_surface_query;
	d->base.surface_locked = dev_surface_locked;
	return &d->base;

fail_close:
	IOServiceClose(d->conn);
fail:
	free(d);
	return NULL;
}
