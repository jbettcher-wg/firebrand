/*
 * The driver bundle's Mesa side: one Mesa context per CGL context.
 *
 * Apple's engine stays in charge of the context's life (through the
 * software renderer the bundle forwards to); what changes is where the GL
 * calls go. When the engine asks the driver to set up dispatch, the
 * application's whole table is filled with entry points that call Mesa's
 * r600. Off-screen drawables (CGLSetOffScreen) and windows: the picture is
 * rendered by the card and copied into the buffer Apple's code expects it
 * in, when the application flushes or presents. Slow for being a copy, but
 * it needs nothing from the window server.
 *
 * Mesa context binding is local to each thread.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/osmesa.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdn_engine.h"
#include "rdn_glue.h"
#include "rdn_target.h"

/* gldAttachDrawable's type for CGLSetOffScreen: kCGLPFAOffScreen. */
#define DRAWABLE_OFFSCREEN	0x35

/*
 * The engine's context (10.4.11's GLEngine): the driver's own table is at
 * this offset in it, and six words before that table is a pointer to the
 * application's GLIFunctionDispatch. See docs/GLD-INTERFACE.md.
 */
/*
 * Where the engine keeps its driver table and the pointer to the
 * application's table, and how long that table is, depend on the system:
 * gld/rdn_engine.h. On a system that is not known the engine is left alone.
 *
 * On 10.4.11 the application's table has 684 entries; the 10.4u SDK's
 * header declares 686. The CGL context's private fields follow the table,
 * so writing the last two entries corrupts them (seen as a crash in
 * CGLDestroyContext). On 10.5.9 it has 759, as its header does.
 */
#define ENGINE_CTX_TABLE_OFFSET	(engine->table_off)
#define APP_DISPATCH_BACK	(engine->disp_back)
#define APP_DISPATCH_ENTRIES	(engine->disp_entries)

/* gldAttachDrawable's type for a window: kCGLPFAWindow. */
#define DRAWABLE_WINDOW		0x50
/* gldAttachDrawable's type for CGLSetPBuffer: kCGLPFAPBuffer. */
#define DRAWABLE_PBUFFER	0x5a
/* Not Apple's: the card's own screen, for the window server's context. */
#define DRAWABLE_SCREEN		0x7570
/*
 * Not Apple's either: a program's window as a surface of the window
 * server's, shown by the card itself at the place the kext is told.
 */
#define DRAWABLE_SURFACE	0x7571
#define DRAWABLE_BACKING	0x7572
#define MAX_SCREEN_RECTS	256

/*
 * What gldAttachDrawable's record says, as far as it is understood, for
 * off-screen drawables and windows alike (docs/GLD-INTERFACE.md): words
 * 4 and 5 are width and height, word 11 the base address of the buffer to
 * draw into, word 27 the row length in pixels (the width rounded up to 16
 * for a window) in its high half and the bytes per pixel in its low half.
 * The engine owns the record and changes it in place when a window is
 * resized, so it is read again before every use.
 */
enum {
	REC_WIDTH = 4,
	REC_HEIGHT = 5,
	REC_BASE = 11,
	REC_ROW = 27
};

struct drawable {
	uint32_t width;
	uint32_t height;
	uint32_t rowbytes;
	void *base;
};

/*
 * Everything rdn_make_current binds Mesa from, as it was when it last did, so
 * that a call that would bind the same again can be left out (below).
 */
struct bind_sig {
	struct drawable d;
	OSMesaContext mesa;
	const uint32_t *record;
	long type;
	unsigned long connection, window, surface;
	int origin_x, origin_y, placed, direct, stored, backing_extent_set;
	uint32_t screen_width, screen_height, screen_pitch, screen_rects, rect_hash;
	uint32_t store_offset, store_row_bytes, store_width, store_height;
	/* From here on what the binding left in the globals; not compared. */
	int global_origin_x, global_origin_y, watch, flush_waits;
};

struct context {
	void *gld_ctx;
	/* The thread Mesa's context was last made current in. */
	pthread_t thread;
	int thread_set;
	int unpack_row_bytes;
	/* The context it shares objects with (gldCreateContext's fourth argument). */
	void *share;
	void *rend;
	OSMesaContext mesa;
	long type;
	const uint32_t *record;
	/* What Mesa is bound to now; compared with the record. */
	struct drawable drawable;
	/*
	 * For DRAWABLE_SCREEN: the part of the screen to draw on, and the
	 * rectangles of it (relative to it) that are copied there. None
	 * means all of it.
	 */
	struct drawable screen;
	uint32_t screen_rects;
	GLint screen_rect[MAX_SCREEN_RECTS][4];
	/*
	 * Or, for DRAWABLE_SCREEN, Mesa draws on the whole screen's own
	 * surface and the context's window coordinates are moved by this
	 * much, to the bottom left corner of that part (rdn_origin_x).
	 */
	int direct;
	uint32_t screen_width, screen_height, screen_pitch;
	int origin_x, origin_y;
	/*
	 * For DRAWABLE_SURFACE: the window server's names for it; origin_*
	 * is its top left on the screen once the kext knows (placed).
	 */
	unsigned long connection, window, surface;
	int placed;
	unsigned surface_width, surface_height;
	void *surface_base;
	int surface_size_valid;
	/* Its picture: video memory of our own, registered with the kext. */
	uint32_t store_offset, store_row_bytes, store_width, store_height;
	int stored, store_new, backing_extent_set;
	int bound;
	/* The window server's Core Image context has had its start state set. */
	int ws_state;
	/*
	 * The engine's context as gldCreateContext was told, so that the
	 * program's GL calls can be given to Mesa before the first drawable
	 * is attached; and whether they have been.
	 */
	void *early_rend;
	int early_ready;
	int dispatched;
	/* Bound to nothing that is shown (no drawable yet). */
	int nowhere;
	unsigned swaps;
	/* The pixel format asked for a double-buffered context (OSMesaDoubleBuffer). */
	int dbl;
	int core_profile;
	int has_shared_buffer;
	/* What Mesa was last bound from, when it can be told again (rdn_make_current). */
	struct bind_sig sig;
	int sig_valid;
	unsigned sig_age;
	/* Flushed since its last drawing call: acquire the surface first (rdn_mesa_surface_arm). */
	int acquire_armed;
};

#define MAX_CONTEXTS 64
static struct context contexts[MAX_CONTEXTS];
static void surface_arm(struct context *c);
static int resolved, usable = -1;

/* Core Animation can bind one context to several independently live surfaces. */
struct surface_store {
	uint32_t id, offset, row_bytes, width, height;
	int gl_attached;
	struct surface_store *next;
};
static struct surface_store *surface_stores;
static pthread_mutex_t surface_store_lock = PTHREAD_MUTEX_INITIALIZER;


static int surface_direct(void);

/*
 * Pbuffers. CGLCreatePBuffer's object is the record gldAttachDrawable
 * gets for type 0x5a: word 2 is its surface ID, word 3 the texture
 * target, word 4 the format, words 6 and 7 the size. Its pixels are video
 * memory of ours, 64 pixels to the row's multiple, bottom row first (a
 * texture's order), made when a context first draws into it or takes it
 * as a texture. CGLDestroyPBuffer tells the driver nothing, so the memory
 * is given back when the object's address comes back with another ID, and
 * when the program ends.
 */
#define MAX_PBUFFERS 64
struct pbuffer {
	const void *key;
	uint32_t id;
	uint32_t offset, row_bytes, width, height;
	/* OSMESA_STORE_* of the pixel format, from CGLCreatePBuffer's. */
	uint32_t flags, bytes_per_pixel;
	int cleared;
};
static struct pbuffer pbuffers[MAX_PBUFFERS];
/* The window server's records have the size in words 8 and 9, not 6 and 7. */
static int pbuffer_ws;

void rdn_mesa_pbuffer_layout(int window_server)
{
	pbuffer_ws = window_server;
}

__thread void *rdn_current_rend;
__thread int rdn_current_core_profile;

static void update_current_core_profile(void *rend)
{
	int i, cp = 0;

	if (rend) {
		for (i = 0; i < MAX_CONTEXTS; i++)
			if (contexts[i].gld_ctx && contexts[i].rend == rend) {
				cp = contexts[i].core_profile;
				break;
			}
	}
	rdn_current_core_profile = cp;
}

/*
 * The thread that draws, found without the thread-local lookup (which on
 * Tiger is a call to __emutls_get_address for every GL call of the program:
 * rdn_current_rend is read by every entry, RDN_ENTER). The first thread to
 * make a context current takes this, with the range of its stack; its
 * entries compare the stack pointer to the range (no other thread's is in
 * it) and the context to .rend, which is what the thread's variable holds:
 * every write to rdn_current_rend goes through rdn_set_current_rend, which
 * updates both. A thread that is not in the range, or a context that is not
 * .rend, takes the variable. The owner gives the place up by making no
 * context current. A thread that ends while it owns it leaves its range
 * behind; a thread made later with the same stack sets its own contexts
 * through the same function, and has the same answers as its variable.
 * On for The Sims 2, or RDN_FAST_REND=1; RDN_FAST_REND=0 off.
 */
struct rdn_fast_rend rdn_fast_rend;
static pthread_mutex_t fast_rend_lock = PTHREAD_MUTEX_INITIALIZER;

static void invalidate_surface_sizes(void)
{
	int i;

	for (i = 0; i < MAX_CONTEXTS; i++)
		contexts[i].surface_size_valid = 0;
}

#if defined(__APPLE__) && defined(__ppc__)
#include <sys/resource.h>

static int fast_rend_enabled(void)
{
	static int on = -1;

	if (on < 0) {
		const char *name = getprogname(), *env = getenv("RDN_FAST_REND");

		on = name && strcmp(name, "WindowServer") &&
		     (env ? *env != '0' : !strcmp(name, "The Sims 2"));
	}
	return on;
}

static int fast_rend_mine(void)
{
	uintptr_t sp;

	__asm__("mr %0,r1" : "=r"(sp));
	return sp - rdn_fast_rend.low < rdn_fast_rend.len;
}

/* The calling thread's stack as [*low, *high); nothing if the stack pointer is not in what the C library says. */
static int fast_rend_stack(uintptr_t *low, uintptr_t *high)
{
	pthread_t self = pthread_self();
	uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
	uintptr_t size = pthread_get_stacksize_np(self);
	uintptr_t sp;
	struct rlimit limit;

	__asm__("mr %0,r1" : "=r"(sp));
	/* The first thread's real limit is the process's (at most 64 MB, the kernel's). */
	if (pthread_main_np() && !getrlimit(RLIMIT_STACK, &limit) &&
	    limit.rlim_cur > size && limit.rlim_cur <= (64u << 20))
		size = (uintptr_t)limit.rlim_cur;
	if (size > top || sp < top - size || sp >= top)
		return 0;
	*low = top - size;
	*high = top;
	return 1;
}

void rdn_set_current_rend(void *rend)
{
	rdn_lazy_flush();
	uintptr_t low, high;

	if (!rend)
		invalidate_surface_sizes();
	rdn_current_rend = rend;
	update_current_core_profile(rend);
	if (fast_rend_mine()) {
		if (rend)
			rdn_fast_rend.rend = rend;
		else {
			rdn_fast_rend.len = 0;
			rdn_fast_rend.rend = NULL;
		}
		return;
	}
	if (!rend || rdn_fast_rend.len || !fast_rend_enabled())
		return;
	pthread_mutex_lock(&fast_rend_lock);
	if (!rdn_fast_rend.len && fast_rend_stack(&low, &high)) {
		rdn_fast_rend.rend = rend;
		rdn_fast_rend.low = low;
		__sync_synchronize();
		/* Last: from here on this thread's entries use it. */
		rdn_fast_rend.len = high - low;
	}
	pthread_mutex_unlock(&fast_rend_lock);
}
#else
void rdn_set_current_rend(void *rend)
{
	rdn_lazy_flush();
	if (!rend)
		invalidate_surface_sizes();
	rdn_current_rend = rend;
	update_current_core_profile(rend);
}
#endif
int rdn_origin_x, rdn_origin_y;
long rdn_async_limit, rdn_async_piece;
int rdn_flush_waits;
int rdn_kept_now;
/* The context being made current runs with glthread. */
static int glthread;

/*
 * How the entry points hand large buffer data to this context (see
 * rdn_glue.h). Two pieces fill one of glthread's batches. RDN_GLD_NO_SPLIT
 * in the environment leaves the data whole.
 */
static void async_data(OSMesaContext mesa)
{
	static int split = -1;
	long limit = OSMesaAsyncDataLimit(mesa);

	if (split < 0)
		split = getenv("RDN_GLD_NO_SPLIT") == NULL;
	glthread = limit > 0;
	if (!split)
		limit = 0;
	rdn_async_limit = limit;
	rdn_async_piece = limit / 2;
	/* Until the drawable turns out to be memory (rdn_make_current). */
	rdn_flush_waits = 0;
}

/*
 * Draw on the screen's surface itself, unless /tmp/rdngld.copy exists:
 * then, as for windows, off-screen with a copy for every flush.
 */
static int screen_direct(void)
{
	static int known = -1;

	if (known < 0)
		known = access("/tmp/rdngld.copy", F_OK) != 0;
	return known;
}
static void (*mesa_finish)(void);
static void (*mesa_flush)(void);

static struct context *find(void *gld_ctx)
{
	int i;

	for (i = 0; i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx == gld_ctx)
			return gld_ctx ? &contexts[i] : NULL;
	return NULL;
}

/*
 * Mesa's context for ours, made now if it is not yet. A context that
 * shares (CGLCreateContext's second argument) is made with the other's
 * Mesa context as its share list, so that textures, programs and buffers
 * are the same objects in both; the other's is made first when it does
 * not exist yet (contexts are made on first use). Core Image does this:
 * its two contexts for pbuffers share with the program's.
 */
/* The last pixel format chosen asked for double buffering (rdn_mesa_double). */
static int pending_double;
int rdn_modern_contexts;
static int rdn_modern_major = 4, rdn_modern_minor = 6;

static OSMesaContext create_modern_attribs(OSMesaContext share, int major, int minor)
{
	const int attribs[] = { OSMESA_FORMAT, OSMESA_BGRA,
		OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_COMPAT_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, major,
		OSMESA_CONTEXT_MINOR_VERSION, minor, 0 };
	return OSMesaCreateContextAttribs(attribs, share);
}

static OSMesaContext modern_context(OSMesaContext share)
{
	OSMesaContext ctx = create_modern_attribs(share, rdn_modern_major, rdn_modern_minor);
	if (!ctx && (rdn_modern_major > 4 || (rdn_modern_major == 4 && rdn_modern_minor > 5)))
		ctx = create_modern_attribs(share, 4, 5);
	if (!ctx && rdn_modern_major >= 4)
		ctx = create_modern_attribs(share, 3, 3);
	return ctx;
}

/* Opt in before creating any CGL contexts. The desktop keeps its legacy path. */
__attribute__((visibility("default")))
int RDNGLRequestCompatibilityContext(int major, int minor)
{
	OSMesaContext probe;
	int i;
	if (major < 3 || (major == 3 && minor < 2) ||
	    major > 4 || (major == 4 && minor > 6) || rdn_window_server)
		return 0;
	for (i = 0; i < MAX_CONTEXTS; ++i)
		if (contexts[i].gld_ctx)
			return 0;
	rdn_modern_major = major;
	rdn_modern_minor = minor;
	probe = modern_context(NULL);
	if (!probe)
		return 0;
	OSMesaDestroyContext(probe);
	rdn_modern_contexts = 1;
	return 1;
}

__attribute__((visibility("default")))
int RDNGLMakeCurrent(void *cgl_context)
{
	GLint major = 0, minor = 0;
	void (*get)(GLenum, GLint *);
	if (!rdn_modern_contexts)
		return 0;
	rdn_mesa_early_all(cgl_context);
	if (!cgl_context)
		return OSMesaGetCurrentContext() == NULL;
	if (!OSMesaGetCurrentContext())
		return 0;
	get = (void (*)(GLenum, GLint *))OSMesaGetProcAddress("glGetIntegerv");
	if (!get)
		return 0;
	get(GL_MAJOR_VERSION, &major);
	get(GL_MINOR_VERSION, &minor);
	return major > 3 || (major == 3 && minor >= 2);
}

__attribute__((visibility("default")))
void *RDNGLGetProcAddress(const char *name)
{
	if (!rdn_modern_contexts || !name || name[0] != 'g' || name[1] != 'l' ||
					!OSMesaGetCurrentContext())
		return NULL;
	if (!strcmp(name, "glFinish") && !getenv("RDN_GLD_SWAP_FINISH"))
		return (void *)OSMesaGetProcAddress("glFlush");
	return (void *)OSMesaGetProcAddress(name);
}

static void legacy_version_cap(void);

static OSMesaContext mesa_for(struct context *c)
{
	OSMesaContext list = NULL;

	if (c->mesa)
		return c->mesa;
	if (c->share && c->share != c->gld_ctx) {
		struct context *o = find(c->share);

		if (o)
			list = mesa_for(o);
	}
	OSMesaDoubleBuffer(c->dbl ? GL_TRUE : GL_FALSE);

	if (c->core_profile) {
		const int attribs[] = {
			OSMESA_FORMAT, OSMESA_BGRA,
			OSMESA_DEPTH_BITS, 24,
			OSMESA_STENCIL_BITS, 8,
			OSMESA_PROFILE, OSMESA_CORE_PROFILE,
			OSMESA_CONTEXT_MAJOR_VERSION, 3,
			OSMESA_CONTEXT_MINOR_VERSION, 3,
			0
		};

		c->mesa = OSMesaCreateContextAttribs(attribs, list);
	} else if (rdn_modern_contexts) {
		c->mesa = modern_context(list);
	} else {
		legacy_version_cap();
		c->mesa = OSMesaCreateContextExt(OSMESA_BGRA, 24, 8, 0, list);
	}
	OSMesaDoubleBuffer(GL_FALSE);
	if (c->mesa && list && rdn_trace)
		rdn_log("context %p shares with %p", c->gld_ctx, c->share);
	return c->mesa;
}

void rdn_mesa_context_created(void *gld_ctx, void *share, int core_profile)
{
	struct context *existing;
	int i;

	if (!gld_ctx)
		return;
	existing = find(gld_ctx);
	if (existing) {
		existing->core_profile = core_profile != 0;
		return;
	}
	for (i = 0; i < MAX_CONTEXTS; i++)
		if (!contexts[i].gld_ctx) {
			memset(&contexts[i], 0, sizeof(contexts[i]));
			contexts[i].gld_ctx = gld_ctx;
			contexts[i].share = share;
			contexts[i].dbl = pending_double;
			contexts[i].core_profile = core_profile != 0;
			return;
		}
	rdn_log("no room for another context; it stays with the software renderer");
}

void rdn_mesa_context_destroyed(void *gld_ctx)
{
	struct context *c = find(gld_ctx);
	void *group;
	int i, group_alive = 0;

	if (!c)
		return;
	group = c->rend ? rdn_mesa_share_group(c->rend) : c->gld_ctx;
	if (c->rend && c->rend == rdn_current_rend) {
		OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
		rdn_set_current_rend(NULL);
	}
	if (c->mesa)
		OSMesaDestroyContext(c->mesa);
	rdn_dispatch_forget(c->rend);
	if (c->acquire_armed)
		__sync_fetch_and_sub(&rdn_surface_acquire_pending, 1);
	/* WindowServer owns the surface lifetime, independently of this context. */
	memset(c, 0, sizeof(*c));
	for (i = 0; group && i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx &&
		    (contexts[i].gld_ctx == group || contexts[i].share == group ||
		     (contexts[i].rend && rdn_mesa_share_group(contexts[i].rend) == group))) {
			group_alive = 1;
			break;
		}
	if (group && !group_alive)
		rdn_client_tex_forget_group(group);
}


/* Imported Mesa resources retain storage until their final GPU use. */
void rdn_mesa_pbuffer_destroyed(void *record)
{
	int i, found = 0;

	if (record)
		rdn_target_surface_buffer(((const uint32_t *)record)[2],
			0xffffffffU, 0, 0, 0);

	for (i = 0; i < MAX_CONTEXTS; i++) {
		struct context *c = &contexts[i];

		if (c->gld_ctx && c->type == DRAWABLE_PBUFFER && c->record == record) {
			c->type = 0;
			c->record = NULL;
			c->bound = 0;
			if (c->rend && c->rend == rdn_current_rend)
				rdn_set_current_rend(NULL);
		}
	}
	for (i = 0; i < MAX_PBUFFERS; i++) {
		struct pbuffer *p = &pbuffers[i];

		if (p->key != record)
			continue;
		found = 1;
		rdn_target_vram_free(p->offset);
		memset(p, 0, sizeof(*p));
	}
	if (rdn_logging) {
		unsigned live = 0, bytes = 0;

		for (i = 0; i < MAX_PBUFFERS; i++)
			if (pbuffers[i].key) {
				live++;
				bytes += pbuffers[i].row_bytes * pbuffers[i].height;
			}
		rdn_log("pbuffer %p destroyed (ours: %d); %u live, %u MB", record, found, live, bytes >> 20);
	}
}

static struct pbuffer *pbuffer_find(uint32_t id)
{
	int i;

	for (i = 0; i < MAX_PBUFFERS; i++)
		if (pbuffers[i].key && pbuffers[i].id == id)
			return &pbuffers[i];
	return NULL;
}

static int mesa_ready(void);

static int reserve_surface(uint32_t surface)
{
	uint32_t info[5];
	bool found;

	if (!rdn_target_surface_query(surface, info, &found))
		return 0;
	if (found) {
		rdn_target_surface_gl(surface);
		return 1;
	}
	return rdn_target_surface_buffer(surface, 0, 0, 0, 0);
}

void rdn_mesa_prepare_pbuffer(void *cgl_ctx, void *record)
{
	void *rend = cgl_ctx ? *(void **)cgl_ctx : NULL;
	const uint32_t *r = record;
	int i;

	if (!r)
		return;
	if (!cgl_ctx) {
		if (rdn_window_server) {
			if (!mesa_ready())
				return;
			rdn_log("WindowServer pbuffer 0x%x registered before attach: %d", r[2],
				reserve_surface(r[2]));
			return;
		}
		for (i = 0; i < MAX_CONTEXTS; ++i) {
			if (contexts[i].gld_ctx && mesa_for(&contexts[i])) {
				rdn_log("cgls pbuffer 0x%x registered before attach: %d", r[2],
					reserve_surface(r[2]));
				return;
			}
		}
		if (usable > 0)
			rdn_log("cgls pbuffer 0x%x registered before attach: %d", r[2],
				reserve_surface(r[2]));
		return;
	}
	if (!rend)
		return;
	for (i = 0; i < MAX_CONTEXTS; ++i) {
		struct context *c = &contexts[i];
		if (c->gld_ctx && (c->rend == rend || c->early_rend == rend)) {
			/* Leopard creates the accelerator surface before attaching GL. */
			if (mesa_for(c))
				rdn_log("pbuffer 0x%x registered before attach: %d", r[2],
					reserve_surface(r[2]));
			return;
		}
	}
}

/*
 * The pixels of a pbuffer by its internal format (CGLCreatePBuffer's
 * fourth argument, word 4 of the record): Core Image asks for GL_RGBA16
 * (0x805b) for its intermediate pictures, and for floating point when a
 * filter wants it. Everything else is 8 bits a channel.
 */
static void pbuffer_pixels(uint32_t format, uint32_t *flags, uint32_t *bytes)
{
	switch (format) {
	case 0x8054:	/* GL_RGB16 */
	case 0x805b:	/* GL_RGBA16 */
		*flags = OSMESA_STORE_RGBA16;
		*bytes = 8;
		break;
	case 0x881b:	/* GL_RGB16F_ARB, GL_RGB_FLOAT16_APPLE */
	case 0x881a:	/* GL_RGBA16F_ARB, GL_RGBA_FLOAT16_APPLE */
		*flags = OSMESA_STORE_FLOAT16;
		*bytes = 8;
		break;
	case 0x8815:	/* GL_RGB32F_ARB, GL_RGB_FLOAT32_APPLE */
	case 0x8814:	/* GL_RGBA32F_ARB, GL_RGBA_FLOAT32_APPLE */
		*flags = OSMESA_STORE_FLOAT32;
		*bytes = 16;
		break;
	default:
		*flags = 0;
		*bytes = 4;
	}
}

static uint32_t surface_tex_flags(uint32_t format)
{
	uint32_t flags, bytes;

	pbuffer_pixels(format, &flags, &bytes);
	switch (format) {
	case 0x1907:	/* GL_RGB */
	case 0x8050:	/* GL_RGB5 */
	case 0x8051:	/* GL_RGB8 */
	case 0x8054:	/* GL_RGB16 */
	case 0x881b:	/* GL_RGB16F_ARB */
	case 0x8815:	/* GL_RGB32F_ARB */
		return flags;
	default:
		return flags | OSMESA_STORE_ALPHA;
	}
}

/* The pbuffer a record describes, its memory made if it is not yet. */
static struct pbuffer *pbuffer_for(const uint32_t *r)
{
	struct pbuffer *p = NULL, *free_slot = NULL;
	uint32_t width = r[6], height = r[7];
	uint32_t row_bytes, bytes, flags, per_pixel;
	int in_aperture = 0;
	uint8_t *gpu;
	int i;

	if (pbuffer_ws || (!width && !height)) {
#if defined(__LP64__) && defined(__APPLE__)
		if (!pbuffer_ws && !r[8]) {
			width = r[9];
			height = r[10];
		} else
#endif
		{
			width = r[8];
			height = r[9];
		}
	}

	if (!width || !height)
		return NULL;
	pbuffer_pixels(r[4], &flags, &per_pixel);
	for (i = 0; i < MAX_PBUFFERS; i++) {
		struct pbuffer *q = &pbuffers[i];

		if (!q->key) {
			if (!free_slot)
				free_slot = q;
			continue;
		}
		if (q->key == r && q->id != r[2]) {
			/* The object's address is another pbuffer's now. */
			if (!pbuffer_ws)
				rdn_target_surface_buffer(q->id, 0xffffffffU, 0, 0, 0);
			rdn_target_vram_free(q->offset);
			memset(q, 0, sizeof(*q));
			if (!free_slot)
				free_slot = q;
		} else if (q->id == r[2]) {
			p = q;
		}
	}
	if (p && p->width == width && p->height == height && p->flags == flags)
		return p;
	if (p) {
		if (!pbuffer_ws)
			rdn_target_surface_buffer(p->id, 0, 0, 0, 0);
		rdn_target_vram_free(p->offset);
		memset(p, 0, sizeof(*p));
		free_slot = p;
	}
	if (!free_slot)
		return NULL;
	/* Nothing bigger than the card draws (16384 a side) or than fits 32 bits. */
	if (width > 16384 || height > 16384 ||
	    (unsigned long long)((width + 63) & ~63u) * per_pixel *
	    ((height + 63) & ~63u) > 0x7ffff000ull) {
		rdn_log("pbuffer %ux%u: too big", (unsigned)width, (unsigned)height);
		return NULL;
	}
	row_bytes = ((width + 63) & ~63u) * per_pixel;
	bytes = row_bytes * ((height + 63) & ~63u);
	bytes = (bytes + 4095) & ~4095u;
	/*
	 * WindowServer Core Image pbuffers stay in hidden VRAM beyond the 256 MB
	 * aperture; client CGLPBuffers may be locked or imported cross-process by
	 * CGSSurfaceID, which RadeonNIAccel requires to live in the aperture.
	 */
	if (pbuffer_ws && !getenv("RDN_PBUFFER_APERTURE")) {
		if (!rdn_target_vram_alloc_hidden(bytes, &free_slot->offset)) {
			if (!rdn_target_vram_alloc(bytes, &free_slot->offset)) {
				rdn_log("pbuffer %ux%u: no video memory", (unsigned)width, (unsigned)height);
				return NULL;
			}
			in_aperture = 1;
		}
	} else {
		if (rdn_target_vram_alloc(bytes, &free_slot->offset)) {
			in_aperture = 1;
		} else if (!rdn_target_vram_alloc_hidden(bytes, &free_slot->offset)) {
			rdn_log("pbuffer %ux%u: no video memory", (unsigned)width, (unsigned)height);
			return NULL;
		}
	}
	gpu = in_aperture ? rdn_target_vram_map(free_slot->offset) : NULL;
	free_slot->cleared = 0;
	if (gpu) {
		uint32_t y;

		for (y = 0; y < height; y++)
			memset(gpu + (size_t)y * row_bytes, 0, (size_t)width * per_pixel);
		__sync_synchronize();
		free_slot->cleared = 1;
	}
	free_slot->key = r;
	free_slot->id = r[2];
	free_slot->row_bytes = row_bytes;
	free_slot->width = width;
	free_slot->height = height;
	free_slot->flags = flags;
	free_slot->bytes_per_pixel = per_pixel;
	if (!pbuffer_ws && in_aperture) {
		rdn_target_surface_buffer(free_slot->id, free_slot->offset,
					  free_slot->row_bytes, free_slot->width,
					  free_slot->height);
		rdn_target_surface_gl(free_slot->id);
	}

	if (rdn_trace)
		rdn_log("pbuffer 0x%x (record %p): %ux%u, format 0x%x, %u bytes a pixel, in video memory at 0x%x",
			(unsigned)r[2], (const void *)r, (unsigned)width,
			(unsigned)height, (unsigned)r[4], (unsigned)per_pixel,
			(unsigned)free_slot->offset);
	return free_slot;
}

static void ensure_pbuffer_cleared(struct context *c, struct pbuffer *p)
{
	void (*push_attrib)(GLbitfield);
	void (*pop_attrib)(void);
	void (*disable)(GLenum);
	void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
	void (*clear_color)(GLclampf, GLclampf, GLclampf, GLclampf);
	void (*clear_fn)(GLbitfield);

	if (p->cleared)
		return;
	push_attrib = (void (*)(GLbitfield))OSMesaGetProcAddress("glPushAttrib");
	pop_attrib = (void (*)(void))OSMesaGetProcAddress("glPopAttrib");
	disable = (void (*)(GLenum))OSMesaGetProcAddress("glDisable");
	color_mask = (void (*)(GLboolean, GLboolean, GLboolean, GLboolean))
		OSMesaGetProcAddress("glColorMask");
	clear_color = (void (*)(GLclampf, GLclampf, GLclampf, GLclampf))
		OSMesaGetProcAddress("glClearColor");
	clear_fn = (void (*)(GLbitfield))OSMesaGetProcAddress("glClear");
	if (push_attrib && pop_attrib && disable && color_mask &&
	    clear_color && clear_fn &&
	    OSMesaMakeCurrentStore(c->mesa, RDN_TARGET_VRAM_HANDLE,
				   (GLsizei)p->row_bytes, p->offset,
				   (GLsizei)p->width, (GLsizei)p->height,
				   p->flags | OSMESA_STORE_BOTTOM_UP)) {
		OSMesaReadbackRects(c->mesa, 0, NULL);
		push_attrib(GL_COLOR_BUFFER_BIT | GL_SCISSOR_BIT);
		disable(GL_SCISSOR_TEST);
		color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		clear_color(0.0f, 0.0f, 0.0f, 0.0f);
		clear_fn(GL_COLOR_BUFFER_BIT);
		pop_attrib();
		OSMesaFlushRender(c->mesa, GL_TRUE);
		p->cleared = 1;
		c->sig_valid = 0;
		c->bound = 0;
		c->nowhere = 0;
		rdn_set_current_rend(NULL);
	}
}

/*
 * CGLTexImagePBuffer (hooked, rdn_hook.c): the texture bound to the
 * pbuffer's target in this context then shows the pbuffer's memory, the
 * way a texture does that is bound to a pbuffer. `pbuffer` is the record
 * gldAttachDrawable got. Returns 1 if the call is ours, with the CGL
 * error (0 if it worked) in *result.
 */
int rdn_mesa_tex_image_pbuffer(void *cgl_ctx, void *pbuffer, int source, int *result)
{
	const uint32_t *r = pbuffer;
	struct context *c = NULL;
	struct pbuffer *p;
	void *rend = cgl_ctx ? *(void **)cgl_ctx : NULL;
	int i;

	(void)source;
	for (i = 0; rend && i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend)
			c = &contexts[i];
	if (!c || !r || !c->mesa)
		return 0;
	p = pbuffer_find(r[2]);
	if (!p && !pbuffer_ws) {
		uint32_t info[5];
		bool found = false;

		if (rdn_target_surface_query(r[2], info, &found) &&
		    found && info[3] && info[4]) {
			rdn_make_current(c->rend);
			rdn_client_tex_drop_bound(c->rend, (unsigned)r[3]);
			*result = OSMesaTexStoreImage(c->mesa, (GLenum)r[3],
						      RDN_TARGET_VRAM_HANDLE,
						      (GLsizei)info[2], info[1],
						      (GLsizei)info[3], (GLsizei)info[4],
						      surface_tex_flags(r[4]) |
						      OSMESA_STORE_BOTTOM_UP)
				  ? 0 : 0x2717;
			return 1;
		}
	}
	if (!p) {
		/* Not one of ours, or never drawn into or attached. */
		p = pbuffer_for(r);
		if (!p)
			return 0;
	}
	ensure_pbuffer_cleared(c, p);
	rdn_make_current(c->rend);
	rdn_client_tex_drop_bound(c->rend, (unsigned)r[3]);
	if (!OSMesaTexStoreImage(c->mesa, (GLenum)r[3], RDN_TARGET_VRAM_HANDLE,
				 (GLsizei)p->row_bytes, p->offset,
				 (GLsizei)p->width, (GLsizei)p->height,
				 p->flags | OSMESA_STORE_BOTTOM_UP | OSMESA_STORE_ALPHA)) {
		rdn_log("pbuffer 0x%x as a texture: Mesa refuses (target 0x%x)",
			(unsigned)r[2], (unsigned)r[3]);
		*result = 0x2717;
		return 1;
	}
	if (rdn_trace)
		rdn_log("pbuffer 0x%x is the texture bound to 0x%x of context %p",
			(unsigned)r[2], (unsigned)r[3], c->gld_ctx);
	*result = 0;
	return 1;
}

/* The window server's cglsTexImagePBuffer: the same, with its record. */
int rdn_mesa_tex_image_pbuffer_ws(void *cgls_ctx, void *pbuffer, int source, int *result)
{
	int ret;

	rdn_mesa_pbuffer_layout(1);
	ret = rdn_mesa_tex_image_pbuffer(cgls_ctx, pbuffer, source, result);
	if (rdn_trace)
		rdn_log("cglsTexImagePBuffer(%p, %p) -> %s, %ld", cgls_ctx, pbuffer,
			ret ? "ours" : "left to OpenGL", ret ? *result : 0L);
	return ret;
}

/*
 * The window server's cglsSetInteger. Parameter 0x3e6 makes the texture
 * bound in the context (its own, in the engine, which Mesa's glBindTexture
 * never reaches) the image of a surface by ID: a pbuffer's (this is also
 * what cglsTexImagePBuffer does), or the screen's, which a layer's filter
 * wants as its backdrop. The values are the ID, the target, the format,
 * the width and height, 0x8367, the buffer (0x400 and 0x404 seen) and a
 * word.
 */
static int backing_store(struct context *c, void *backing, uint32_t stride,
                         uint32_t width, uint32_t height);
static int own_surface_store(uint32_t id);
static struct context *find_rend(void *rend);
static void probe_backing(const char *why, uint32_t surface, const void *backing,
			  uint32_t stride, uint32_t width, uint32_t height,
			  uint32_t store_offset, uint32_t store_row_bytes,
			  uint32_t store_height);

int rdn_surface_texture_hook;

/*
 * THE WINDOW SERVER'S TEXTURE OF A PROGRAM'S SURFACE FOLLOWS ITS STORE.
 *
 * The window server binds a program's surface as a texture once
 * (cglsSetInteger below) and composites from it from then on; the texture
 * is the producer's store in video memory (OSMesaTexStoreImage). When the
 * program resizes its window its GLD replaces the store (surface_store:
 * a new offset, row bytes and size, the old memory freed), and the window
 * server knew nothing of it: it went on drawing the old store, scaled into
 * the window's new bounds while the picture lasted and whatever the memory
 * held next after that. Seen 2026-11-12: FrostByte's page
 * squeezed into a smaller window, and after a resize a band of blue
 * scan-line garbage where its old store had been reused.
 *
 * So every surface texture the window server binds is remembered here with
 * the store it was made from, and each time the texture is bound again
 * (once a frame: the client-storage epoch gates it) the kext is asked for
 * the surface's current store; when that moved or changed size the texture
 * is made over it. The window server's own stores (backing_store) never
 * move, so they are not kept here.
 */
struct surface_tex {
	void *rend;
	GLenum target;
	GLuint name;
	uint32_t id;
	uint32_t info[5];
	uint32_t flags;
	uint32_t clip_w, clip_h;	/* the size the window server asked for, 0: the store's */
	unsigned epoch;
	uint64_t checked;		/* mach_absolute_time() of the last look at the store */
};
#define MAX_SURFACE_TEX 64
static struct surface_tex surface_texs[MAX_SURFACE_TEX];
static pthread_mutex_t surface_tex_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int rdn_surface_tex_live;

/* The texture bound to a target in the current Mesa context. */
static GLuint bound_texture(GLenum target)
{
	static void (*get_int)(GLenum, GLint *);
	GLint v = 0;

	if (!get_int)
		get_int = (void (*)(GLenum, GLint *))OSMesaGetProcAddress("glGetIntegerv");
	if (!get_int)
		return 0;
	switch (target) {
	case GL_TEXTURE_2D: get_int(0x8069, &v); break;		/* GL_TEXTURE_BINDING_2D */
	case 0x84f5: get_int(0x84f6, &v); break;		/* GL_TEXTURE_BINDING_RECTANGLE */
	default: return 0;
	}
	return (GLuint)v;
}

static struct surface_tex *surface_tex_find_locked(void *rend, GLenum target, GLuint name)
{
	int i;

	for (i = 0; i < MAX_SURFACE_TEX; i++)
		if (surface_texs[i].name == name && surface_texs[i].rend == rend &&
		    (!target || surface_texs[i].target == target))
			return &surface_texs[i];
	return NULL;
}

void rdn_mesa_surface_tex_forget(void *rend, unsigned target, unsigned name)
{
	struct surface_tex *t;
	int i;

	if (!rdn_surface_tex_live || !name)
		return;
	pthread_mutex_lock(&surface_tex_lock);
	while ((t = surface_tex_find_locked(rend, (GLenum)target, (GLuint)name)) != NULL)
		memset(t, 0, sizeof(*t));
	for (i = 0; i < MAX_SURFACE_TEX && !surface_texs[i].name; i++)
		;
	rdn_surface_tex_live = i < MAX_SURFACE_TEX;
	pthread_mutex_unlock(&surface_tex_lock);
}

static void surface_tex_note(void *rend, GLenum target, uint32_t id, const uint32_t info[5],
			     uint32_t flags, uint32_t clip_w, uint32_t clip_h)
{
	GLuint name = bound_texture(target);
	struct surface_tex *t;

	if (!name)
		return;
	pthread_mutex_lock(&surface_tex_lock);
	t = surface_tex_find_locked(rend, target, name);
	if (!t)
		t = surface_tex_find_locked(NULL, 0, 0);
	if (t) {
		t->rend = rend;
		t->target = target;
		t->name = name;
		t->id = id;
		memcpy(t->info, info, sizeof(t->info));
		t->flags = flags;
		t->clip_w = clip_w;
		t->clip_h = clip_h;
		t->epoch = rdn_client_tex_epoch_now();
		t->checked = mach_absolute_time();
		rdn_surface_tex_live = 1;
	}
	pthread_mutex_unlock(&surface_tex_lock);
}

/*
 * How often a bound surface texture looks at its store when the epoch has
 * not moved (8 ms, in mach_absolute_time units). The epoch only moves at
 * the window server's cglsSetInteger 0x1f96, and after a live resize it can
 * stand still while the window server composites the window again (an
 * overlapping window, the link bubble, a menu): the texture then kept the
 * store from before the drag's last step until something else rebound it
 * (FrostByte re-attached its drawable 150 ms after the drag to get past
 * this). A store query is one call into the kext.
 */
static uint64_t surface_tex_interval(void)
{
	static uint64_t interval;

	if (!interval) {
		mach_timebase_info_data_t tb;

		mach_timebase_info(&tb);
		interval = tb.numer ? 8000000ULL * tb.denom / tb.numer : 8000000ULL;
		if (!interval)
			interval = 1;
	}
	return interval;
}

/* glBindTexture in the window server: the surface's store may have moved. */
void rdn_mesa_surface_tex_bound(void *rend, unsigned target, unsigned name)
{
	struct surface_tex snap, *t;
	struct context *c;
	uint32_t info[5];
	bool found = false;
	uint32_t tex_w, tex_h;
	unsigned ep = rdn_client_tex_epoch_now();
	uint64_t now = mach_absolute_time();
	int resized;

	pthread_mutex_lock(&surface_tex_lock);
	t = surface_tex_find_locked(rend, (GLenum)target, (GLuint)name);
	if (!t || (t->epoch == ep && now - t->checked < surface_tex_interval())) {
		pthread_mutex_unlock(&surface_tex_lock);
		return;
	}
	t->epoch = ep;
	t->checked = now;
	snap = *t;
	pthread_mutex_unlock(&surface_tex_lock);

	if (!rdn_target_surface_query(snap.id, info, &found) || !found ||
	    !info[3] || !info[4] || own_surface_store(snap.id))
		return;
	if (info[1] == snap.info[1] && info[2] == snap.info[2] &&
	    info[3] == snap.info[3] && info[4] == snap.info[4])
		return;
	c = find_rend(rend);
	if (!c || !c->mesa)
		return;
	/*
	 * The size the window server asked for at its bind belongs to the store
	 * of that moment. When the program has since resized the store, that
	 * size is the window's from before the resize (a live resize shrinks
	 * and grows the store many times under one bind), and clipping the new
	 * store to it textured the window at its old size, clamped at the edges.
	 * The program sizes its store to its view, so the new store's size is
	 * the window's; a store that only moved keeps the asked-for size.
	 */
	resized = info[3] != snap.info[3] || info[4] != snap.info[4];
	tex_w = info[3];
	tex_h = info[4];
	if (!resized && snap.clip_w && snap.clip_h && snap.clip_w <= tex_w && snap.clip_h <= tex_h) {
		tex_w = snap.clip_w;
		tex_h = snap.clip_h;
	}
	if (!OSMesaTexStoreImage(c->mesa, (GLenum)target, RDN_TARGET_VRAM_HANDLE,
				 (GLsizei)info[2], info[1], (GLsizei)tex_w, (GLsizei)tex_h,
				 snap.flags)) {
		rdn_log("surface 0x%x: its store moved to 0x%x (%ux%u, row bytes %u) but texture %u could not follow",
			(unsigned)snap.id, (unsigned)info[1], (unsigned)info[3], (unsigned)info[4],
			(unsigned)info[2], (unsigned)name);
		return;
	}
	if (rdn_logging)
		rdn_log("surface 0x%x: its store moved from 0x%x (%ux%u) to 0x%x (%ux%u, row bytes %u): texture %u follows at %ux%u",
			(unsigned)snap.id, (unsigned)snap.info[1], (unsigned)snap.info[3], (unsigned)snap.info[4],
			(unsigned)info[1], (unsigned)info[3], (unsigned)info[4], (unsigned)info[2], (unsigned)name,
			(unsigned)tex_w, (unsigned)tex_h);
	pthread_mutex_lock(&surface_tex_lock);
	t = surface_tex_find_locked(rend, (GLenum)target, (GLuint)name);
	if (t && t->id == snap.id) {
		memcpy(t->info, info, sizeof(t->info));
		if (resized)
			t->clip_w = t->clip_h = 0;
	}
	pthread_mutex_unlock(&surface_tex_lock);
}

int rdn_mesa_cgls_set_integer(void *cgls_ctx, int pname, int *vals, int *result)
{
	struct context *c = NULL;
	void *rend = cgls_ctx ? *(void **)cgls_ctx : NULL;
	struct pbuffer *p;
	void *backing;
	uint32_t backing_stride, backing_width, backing_height;
	uint32_t info[5];
	bool found = false;
	int is_backing;
	int i;

	if (rdn_trace)
		rdn_log("cglsSetInteger(%p, 0x%lx): first value 0x%lx", cgls_ctx,
			pname, vals ? vals[0] : 0);
	if ((pname != 0x3e5 && pname != 0x3e6 && pname != 999 &&
	     pname != 0x1f96) || !vals)
		return 0;
	for (i = 0; rend && i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend)
			c = &contexts[i];
	if (!c || !c->mesa)
		return 0;
	if (pname == 0x1f96) {
		rdn_client_tex_bump_epoch();
		if (vals[0] == 3 &&
		    (c->type == DRAWABLE_BACKING || c->type == DRAWABLE_SCREEN) &&
		    (unsigned long)vals[1] == c->surface) {
			rdn_make_current(c->rend);
			if (mesa_flush)
				mesa_flush();
			*result = 0;
			return 1;
		}
		return 0;
	}
	if (rdn_trace)
		rdn_log("cglsSetInteger(%p, 0x%lx): %lx %lx %lx %lx %lx", cgls_ctx,
			pname, vals[0], vals[1], vals[2], vals[3], vals[4]);

	p = pbuffer_find((uint32_t)vals[0]);
	if (p) {
		/* A pbuffer: its memory is the texture. */
		rdn_make_current(c->rend);
		rdn_client_tex_drop_bound(c->rend, (unsigned)vals[1]);
		rdn_mesa_surface_tex_forget(c->rend, (unsigned)vals[1], bound_texture((GLenum)vals[1]));
		if (!OSMesaTexStoreImage(c->mesa, (GLenum)vals[1], RDN_TARGET_VRAM_HANDLE,
					 (GLsizei)p->row_bytes, p->offset,
					 (GLsizei)p->width, (GLsizei)p->height,
					 p->flags | OSMESA_STORE_BOTTOM_UP | OSMESA_STORE_ALPHA)) {
			*result = 0x2717;
			return 1;
		}
		*result = 0;
		return 1;
	}

	if (pname != 0x3e5 &&
	    (c->type == DRAWABLE_SCREEN || c->type == DRAWABLE_BACKING) &&
	    (unsigned long)vals[0] == c->surface) {
		/*
		 * The screen's or backing's surface: what is drawn so far, as a
		 * texture. Leopard's __CGXGLCompositeLayers requests the full
		 * screen (vals[3] == screen_width && vals[4] == screen_height)
		 * even on partial-region redraws where origin_x/origin_y are
		 * non-zero; DRAWABLE_BACKING always starts at (0, 0) top-down.
		 */
		GLint x = c->origin_x;
		GLint y = (GLint)c->screen_height - c->origin_y - (GLint)vals[4];

		if (c->type == DRAWABLE_BACKING ||
		    (c->direct && (uint32_t)vals[3] == c->screen_width &&
		     (uint32_t)vals[4] == c->screen_height)) {
			x = 0;
			y = 0;
		}
		if (c->type == DRAWABLE_BACKING)
			probe_backing("screen as texture", c->surface, c->screen.base,
				      (uint32_t)c->screen.rowbytes, c->store_width,
				      c->store_height, c->store_offset,
				      c->store_row_bytes, c->store_height);
		rdn_make_current(c->rend);
		rdn_client_tex_drop_bound(c->rend, (unsigned)vals[1]);
		rdn_mesa_surface_tex_forget(c->rend, (unsigned)vals[1], bound_texture((GLenum)vals[1]));
		*result = OSMesaTexCopyDrawable(c->mesa, (GLenum)vals[1], x, y,
						(GLsizei)vals[3], (GLsizei)vals[4]) ? 0 : 0x2717;
		if (rdn_logging)
			rdn_log("the screen as a texture of context %p (%ldx%ld at %d,%d -> %d,%d of %ux%u): %s",
				c->gld_ctx, vals[3], vals[4], c->origin_x, c->origin_y,
				(int)x, (int)y,
				(unsigned)c->screen_width, (unsigned)c->screen_height,
				*result ? "failed" : "done");
		return 1;
	}

	/*
	 * A PROGRAM'S STORE IS THE TEXTURE, NOT A COPY OF ITS BACKING.
	 *
	 * A Quartz GL window (NSWindowBackingLocationVideoMemory: System
	 * Preferences) has both: the program's GLD draws it into a store it
	 * registered with the kext, and the window server keeps a CPU backing
	 * that the kext brings up to date at its locks. Under Leopard's GL
	 * compositor the window server binds the surface as a texture once
	 * (this call) and composites from it. Taking the backing path here
	 * made a store of our own from a copy of the backing, registered it
	 * over the program's, and the window then showed that stale copy for
	 * good: a white band where the store had grown and the old pane after
	 * a pane change (2026-11-11). So a surface
	 * whose registered buffer is not a store of ours is textured from
	 * that buffer, which the program keeps drawing; only a surface with no
	 * producer (the Dock's, the menu bar's) gets a store made from its
	 * backing.
	 */
	if (rdn_target_surface_query((uint32_t)vals[0], info, &found) &&
	    found && info[3] && info[4] && !own_surface_store((uint32_t)vals[0])) {
		if (rdn_logging)
			rdn_log("surface 0x%lx: its producer's store (%ux%u, row bytes %u at 0x%x) is the texture",
				vals[0], (unsigned)info[3], (unsigned)info[4],
				(unsigned)info[2], (unsigned)info[1]);
		is_backing = 0;
	} else {
		found = false;
		is_backing = rdn_target_surface_backing((uint32_t)vals[0], &backing,
							&backing_stride, &backing_width,
							&backing_height);
		if (is_backing) {
			struct context storage = {0};

			storage.surface = (uint32_t)vals[0];
			if (!backing_store(&storage, backing, backing_stride,
					   backing_width, backing_height)) {
				*result = 0x2717;
				return 1;
			}
		}
	}
	if ((found || rdn_target_surface_query((uint32_t)vals[0], info, &found)) &&
	    found && info[3] && info[4]) {
		uint32_t tex_w = info[3], tex_h = info[4];

		if (pname != 0x3e5 && vals[3] > 0 && vals[4] > 0 &&
		    (uint32_t)vals[3] <= tex_w && (uint32_t)vals[4] <= tex_h) {
			tex_w = (uint32_t)vals[3];
			tex_h = (uint32_t)vals[4];
		}
		rdn_make_current(c->rend);
		rdn_client_tex_drop_bound(c->rend, (unsigned)vals[1]);
		*result = OSMesaTexStoreImage(c->mesa, (GLenum)vals[1],
					      RDN_TARGET_VRAM_HANDLE,
					      (GLsizei)info[2], info[1],
					      (GLsizei)tex_w, (GLsizei)tex_h,
					      surface_tex_flags((uint32_t)vals[2]))
			  ? 0 : 0x2717;
		if (rdn_logging)
			rdn_log("surface 0x%lx explicitly bound to texture target 0x%lx: %ld",
				vals[0], vals[1], *result);
		if (!*result && !is_backing)
			surface_tex_note(c->rend, (GLenum)vals[1], (uint32_t)vals[0], info,
					 surface_tex_flags((uint32_t)vals[2]),
					 tex_w != info[3] || tex_h != info[4] ? tex_w : 0,
					 tex_w != info[3] || tex_h != info[4] ? tex_h : 0);
		else
			rdn_mesa_surface_tex_forget(c->rend, (unsigned)vals[1], bound_texture((GLenum)vals[1]));
		return 1;
	}
	if (is_backing || pname == 0x3e5 || found) {
		*result = 0x2717;
		return 1;
	}
	return 0;
}

/* Read the engine's record; false if there is nothing to draw into. */
/*
 * A surface's picture is kept in a linear buffer in video memory, 64
 * pixels to the row's multiple as the card wants of a linear render
 * target. WindowServer reads it until the surface is destroyed; resizing
 * replaces the backing without tying its lifetime to the current context.
 */
/* Is this surface's registered buffer a store this process made? */
static int own_surface_store(uint32_t id)
{
	struct surface_store *s;
	int own = 0;

	pthread_mutex_lock(&surface_store_lock);
	for (s = surface_stores; s; s = s->next)
		if (s->id == id) {
			own = 1;
			break;
		}
	pthread_mutex_unlock(&surface_store_lock);
	return own;
}

static void clear_surface_store(struct surface_store *store)
{
	store->id = store->offset = store->row_bytes = store->width = store->height = 0;
	store->gl_attached = 0;
}

static void surface_store(struct context *c, uint32_t width, uint32_t height)
{
	uint32_t row_bytes = ((width + 63) & ~63u) * 4;
	uint32_t rows = (height + 63) & ~63u;
	uint32_t info[5];
	struct surface_store *store = NULL, *empty = NULL;
	bool found;
	/* A resized surface's previous store, kept until its picture is carried over. */
	uint32_t old_offset = 0, old_row_bytes = 0, old_width = 0, old_height = 0;
	int old_gl_attached = 0;
	int have_old = 0, allocated = 0, gpu_inited = 0;

	c->stored = 0;
	c->store_new = 0;
	if (!c->surface || !width || !height || width > 16384 || height > 16384)
		return;
	pthread_mutex_lock(&surface_store_lock);
	/* Reclaim only after WindowServer has removed the surface. */
	for (struct surface_store *s = surface_stores; s; s = s->next) {
		if (s->id && s->id != c->surface &&
		    rdn_target_surface_query(s->id, info, &found) && !found) {
			rdn_log("surface 0x%x is gone: its store (%ux%u at 0x%x) is freed",
				(unsigned)s->id, (unsigned)s->width, (unsigned)s->height, (unsigned)s->offset);
			rdn_target_vram_free(s->offset);
			clear_surface_store(s);
		}
		if (s->id == c->surface)
			store = s;
		else if (!s->id && !empty)
			empty = s;
	}
	if (store && (store->width != width || store->height != height)) {
		if (c->mesa && OSMesaGetCurrentContext() == c->mesa)
			OSMesaFlushRender(c->mesa, GL_TRUE);
		if (!rdn_target_surface_buffer(store->id, 0, 0, 0, 0))
			goto out;
		rdn_log("surface 0x%x resized %ux%u -> %ux%u: its store at 0x%x is replaced",
			(unsigned)store->id, (unsigned)store->width, (unsigned)store->height,
			(unsigned)width, (unsigned)height, (unsigned)store->offset);
		old_offset = store->offset;
		old_row_bytes = store->row_bytes;
		old_width = store->width;
		old_height = store->height;
		old_gl_attached = store->gl_attached;
		have_old = 1;
		clear_surface_store(store);
	}
	if (!store)
		store = empty;
	if (!store) {
		store = calloc(1, sizeof(*store));
		if (!store)
			goto out;
		store->next = surface_stores;
		surface_stores = store;
	}
	if (!store->id) {
		int keep_old = have_old && (old_gl_attached || c->mesa != NULL);

		if (!rdn_target_vram_alloc((row_bytes * rows + 4095) & ~4095u, &store->offset))
			goto out;
		allocated = 1;
		c->store_new = !keep_old;
		store->id = (uint32_t)c->surface;
		store->row_bytes = row_bytes;
		store->width = width;
		store->height = height;
		store->gl_attached = old_gl_attached;
		rdn_log("surface 0x%x: new store %ux%u at 0x%x%s", (unsigned)store->id,
			(unsigned)width, (unsigned)height, (unsigned)store->offset,
			keep_old ? ", keeping the old picture" : "");
		/*
		 * A surface keeps its pixels. The window server draws into the
		 * strips it keeps for title bars and the menu bar a part at a
		 * time and expects the rest to stay, across a resize too, as an
		 * IOAccelSurface does. So a new store starts clear, a resized
		 * GL-attached surface carries the previous store's overlap
		 * without letting backing_store() overwrite it from stale CPU
		 * backing memory, and a purely CPU-drawn surface (!keep_old)
		 * sets store_new so backing_store() copies its new CPU backing.
		 * Measured on the W8100 under Leopard, 2026-11-04: a new store
		 * that started with what its memory held showed another window's
		 * pixels (or a test's clear colour) where the server had not
		 * drawn.
		 */
		{
			bool synced = false;

			if (rdn_target_surface_init(store->id, store->offset,
						    store->row_bytes, width, height,
						    keep_old ? old_offset : 0,
						    keep_old ? old_row_bytes : 0,
						    keep_old ? old_width : 0,
						    keep_old ? old_height : 0,
						    keep_old != 0, &synced)) {
				c->store_new = !keep_old && !synced;
				gpu_inited = 1;
			} else {
				uint8_t *gpu = rdn_target_vram_map(store->offset);
				const uint8_t *old = keep_old ? rdn_target_vram_map(old_offset) : NULL;
				uint32_t y, keep_w = old && old_width < width ? old_width : width;
				uint32_t keep_h = old && old_height < height ? old_height : height;

				if (gpu) {
					for (y = 0; y < height; y++) {
						uint8_t *row = gpu + (size_t)y * row_bytes;

						/*
						 * Both DRAWABLE_BACKING (flags == 0, FlipY == true) and
						 * DRAWABLE_SURFACE store row 0 at the top of the image.
						 * Carry the top keep_h rows anchored at row 0 and clear
						 * any newly exposed rows or columns.
						 */
						if (old && y < keep_h) {
							memcpy(row, old + (size_t)y * old_row_bytes, keep_w * 4);
							if (keep_w < width)
								memset(row + keep_w * 4, 0, (width - keep_w) * 4);
						} else {
							memset(row, 0, width * 4);
						}
					}
					__sync_synchronize();
				}
			}
		}
	}
	if (have_old) {
		rdn_target_vram_free(old_offset);
		have_old = 0;
	}
	if (!gpu_inited &&
	    !rdn_target_surface_buffer(store->id, store->offset,
				       store->row_bytes, width, height)) {
		if (allocated) {
			rdn_target_vram_free(store->offset);
			clear_surface_store(store);
		}
		goto out;
	}
	if (c->mesa)
		store->gl_attached = 1;
	if (store->gl_attached)
		rdn_target_surface_gl(store->id);
	c->store_offset = store->offset;
	c->store_row_bytes = store->row_bytes;
	c->store_width = width;
	c->store_height = height;
	c->stored = 1;
out:
	if (have_old)
		rdn_target_vram_free(old_offset);
	pthread_mutex_unlock(&surface_store_lock);
}


/*
 * Diagnostic for KNOWN-ISSUES 23: compare the window server's CPU backing
 * (its own pointer, read in its own process) with the surface's video-
 * memory store, so the noise can be placed: noise already in the backing
 * (the backing is not what we think it is), or a good backing and a store
 * that disagrees (the transfer or its arithmetic). Only while logging,
 * at most twice a second per surface. For a sampled set of backing rows it
 * reports the row's edge count (adjacent pixels that differ: a flat grey
 * row has ~0, noise has ~width), and the store row that matches it best
 * with that row's mismatch count, which shows a flip or a shift directly.
 */
static unsigned row_edges(const uint32_t *row, uint32_t width)
{
	unsigned n = 0;
	uint32_t x;
	for (x = 1; x < width; x++)
		n += row[x] != row[x - 1];
	return n;
}

static unsigned row_diff(const uint32_t *a, const volatile uint32_t *b, uint32_t width)
{
	unsigned n = 0;
	uint32_t x;
	for (x = 0; x < width; x++)
		n += a[x] != b[x];
	return n;
}

static void probe_backing(const char *why, uint32_t surface, const void *backing,
			  uint32_t stride, uint32_t width, uint32_t height,
			  uint32_t store_offset, uint32_t store_row_bytes,
			  uint32_t store_height)
{
	static struct { uint32_t surface; uint64_t at; } seen[16];
	static uint32_t *line;
	static uint32_t line_cap;
	const volatile uint8_t *gpu;
	uint64_t now = mach_absolute_time();
	mach_timebase_info_data_t tb;
	unsigned i, slot = 0, total = 0, flip = 0;
	uint32_t y, ys[6];

	if (!rdn_logging || !backing || !width || !height)
		return;
	mach_timebase_info(&tb);
	for (i = 0; i < 16; i++) {
		if (seen[i].surface == surface) { slot = i; break; }
		if (seen[i].at < seen[slot].at) slot = i;
	}
	if (seen[slot].surface == surface && strcmp(why, "new store") &&
	    (now - seen[slot].at) * tb.numer / tb.denom < 500000000ULL)
		return;
	seen[slot].surface = surface;
	seen[slot].at = now;
	gpu = rdn_target_vram_map(store_offset);
	rdn_log("probe %s: surface 0x%lx backing %p stride %lu %lux%lu, store 0x%lx row_bytes %lu height %lu (gpu %p)",
		why, (unsigned long)surface, backing, (unsigned long)stride,
		(unsigned long)width, (unsigned long)height,
		(unsigned long)store_offset, (unsigned long)store_row_bytes,
		(unsigned long)store_height, (const void *)gpu);
	if (!gpu || store_row_bytes < width * 4 || store_height < height)
		return;
	if (line_cap < width) {
		free(line);
		line = malloc((size_t)width * 4);
		line_cap = line ? width : 0;
		if (!line) return;
	}
	for (y = 0; y < height; y++) {
		const uint32_t *b = (const uint32_t *)((const uint8_t *)backing + (size_t)y * stride);
		total += row_diff(b, (const volatile uint32_t *)(gpu + (size_t)y * store_row_bytes), width);
		flip += row_diff(b, (const volatile uint32_t *)(gpu + (size_t)(height - 1 - y) * store_row_bytes), width);
	}
	rdn_log("probe   pixels differing: as-is %u, flipped %u, of %lu", total, flip,
		(unsigned long)(width * height));
	ys[0] = 0; ys[1] = 1; ys[2] = height / 4; ys[3] = height / 2;
	ys[4] = height * 3 / 4; ys[5] = height - 1;
	for (i = 0; i < 6; i++) {
		const uint32_t *b = (const uint32_t *)((const uint8_t *)backing + (size_t)ys[i] * stride);
		uint32_t r, best = 0, best_n = ~0u, x;
		for (r = 0; r < store_height; r++) {
			unsigned n = row_diff(b, (const volatile uint32_t *)(gpu + (size_t)r * store_row_bytes), width);
			if (n < best_n) { best_n = n; best = r; }
		}
		for (x = 0; x < width; x++)
			line[x] = ((const volatile uint32_t *)(gpu + (size_t)ys[i] * store_row_bytes))[x];
		rdn_log("probe   row %2lu: backing edges %4u px0 %08lx mid %08lx | store edges %4u px0 %08lx mid %08lx | best store row %lu (%u differ)",
			(unsigned long)ys[i], row_edges(b, width), (unsigned long)b[0],
			(unsigned long)b[width / 2], row_edges(line, width),
			(unsigned long)line[0], (unsigned long)line[width / 2],
			(unsigned long)best, best_n);
	}
}

/* CPU-drawn backings also need storage when first imported as textures. */
static int backing_store(struct context *c, void *backing, uint32_t stride,
                         uint32_t width, uint32_t height)
{
	surface_store(c, width, height);
	if (!c->stored)
		return 0;
	/*
	 * Fill a freshly-created store from the CPU backing. The kext syncs
	 * the backing into the store (syncBacking, RadeonNISurface.cpp) at
	 * setShapeBackingAndLength and at every writeUnlock -- but the store
	 * does not exist at setShapeBackingAndLength time, because the bundle
	 * allocates it here, at the first import. So that first sync misses
	 * the new store and, without this copy, the first composite of a
	 * backing surface reads whatever the new store's video memory held:
	 * the menu bar and title strips showing other surfaces' pixels before
	 * the server next draws into them (backingsurfacetest, 2026-11-04:
	 * "CPU-only backing import" and "initial CPU -> GPU" both failed,
	 * the import reading 0,0,0,0 instead of the backing's colour). The
	 * kext keeps the store current after this, so only a new store needs
	 * the copy. No vertical flip: the kext's bcopy maps backing row y to
	 * store row y, so this does the same.
	 */
	probe_backing(c->store_new ? "new store" : "import", c->surface, backing,
		      stride, width, height, c->store_offset, c->store_row_bytes,
		      c->store_height);
	if (c->store_new && backing &&
	    !rdn_target_surface_backing_stale((uint32_t)c->surface)) {
		uint8_t *gpu = rdn_target_vram_map(c->store_offset);
		uint32_t y;

		if (gpu) {
			for (y = 0; y < height; y++)
				memcpy(gpu + (size_t)y * c->store_row_bytes,
				       (const uint8_t *)backing + (size_t)y * stride,
				       (size_t)width * 4);
			__sync_synchronize();
		}
	}
	return 1;
}

/*
 * Where the window server has the surface now, from the kext: size as the
 * drawable, place and rectangles kept in the context. The base only says
 * that there is one.
 */
static int surface_drawable(struct context *c, struct drawable *d)
{
	static int16_t rects[MAX_SCREEN_RECTS][4];
	volatile uint32_t *pixels;
	uint32_t count = 0, i;
	unsigned width, height;
	int32_t b[4];

	memset(d, 0, sizeof(*d));
	if (!c->mesa)
		return 0;
	if (c->window && !surface_direct() && c->surface_size_valid &&
	    c->surface_base && c->screen_width && c->screen_height) {
		d->width = c->surface_width;
		d->height = c->surface_height;
		d->base = c->surface_base;
		c->screen_rects = 0;
		c->placed = 0;
		return 1;
	}
	if (!rdn_target_screen(&pixels, &c->screen_width, &c->screen_height,
			       &c->screen_pitch))
		return 0;
	if (!c->window) {
		/* Quartz-owned surfaces have no CGS window to query. */
		if (!rdn_target_surface_region((uint32_t)c->surface, b, rects,
		                              MAX_SCREEN_RECTS, &count) ||
		    b[2] <= 0 || b[3] <= 0)
			return 0;
		width = b[2];
		height = b[3];
	} else if (c->surface_size_valid) {
		width = c->surface_width;
		height = c->surface_height;
	} else if (!rdn_surface_size(c->connection, c->window, c->surface,
	                             &width, &height)) {
		return 0;
	} else {
		c->surface_width = width;
		c->surface_height = height;
		c->surface_base = (void *)pixels;
		c->surface_size_valid = 1;
	}
	d->width = !c->window && c->backing_extent_set ? c->screen.width : width;
	d->height = !c->window && c->backing_extent_set ? c->screen.height : height;
	d->base = (void *)pixels;

	/*
	 * The place and the visible part come from the kext, once the
	 * window server has told it. Until then the program can draw; nothing
	 * is shown.
	 */
	c->screen_rects = 0;
	c->placed = 0;
	if (!c->window || !surface_direct())
		return 1;
	if (!rdn_target_surface_region((uint32_t)c->surface, b, rects,
				       MAX_SCREEN_RECTS, &count) ||
	    b[2] <= 0 || b[3] <= 0 || count > MAX_SCREEN_RECTS)
		return 1;
	c->origin_x = b[0];
	c->origin_y = b[1];
	/* Experiment: RDN_GLD_SURFACE_AT=x shows the surface x pixels further right. */
	if (getenv("RDN_GLD_SURFACE_AT"))
		c->origin_x += atoi(getenv("RDN_GLD_SURFACE_AT"));
	for (i = 0; i < count; i++) {
		c->screen_rect[i][0] = rects[i][0] - b[0];
		c->screen_rect[i][1] = rects[i][1] - b[1];
		c->screen_rect[i][2] = rects[i][2];
		c->screen_rect[i][3] = rects[i][3];
	}
	c->screen_rects = count;
	c->placed = 1;
	return 1;
}

static int read_record(const struct context *c, struct drawable *d)
{
	const uint32_t *r = c->record;

	if (c->type == DRAWABLE_SCREEN || c->type == DRAWABLE_BACKING) {
		*d = c->screen;
		return d->base != NULL;
	}
	if (c->type == DRAWABLE_SURFACE)
		return surface_drawable((struct context *)c, d);
	memset(d, 0, sizeof(*d));
	if (c->type == DRAWABLE_PBUFFER) {
		struct pbuffer *p = r ? pbuffer_for(r) : NULL;

		if (!p)
			return 0;
		d->width = p->width;
		d->height = p->height;
		d->rowbytes = p->row_bytes;
		d->base = p;
		return 1;
	}
	if (!r || (c->type != DRAWABLE_OFFSCREEN && c->type != DRAWABLE_WINDOW))
		return 0;
	if (c->type == DRAWABLE_OFFSCREEN) {
		/*
		 * CGLSetOffScreen's record is the program's own: width,
		 * height, row bytes and base in its first four words. The
		 * engine's later words (REC_*) are not filled for it.
		 */
		d->width = r[0];
		d->height = r[1];
		d->rowbytes = r[2];
		d->base = *(void *const *)((const char *)r + (sizeof(void *) == 8 ? 16 : 12));
		return d->base && d->width && d->height &&
		       d->rowbytes >= d->width * 4;
	}
	d->width = r[REC_WIDTH];
	d->height = r[REC_HEIGHT];
	d->rowbytes = (r[REC_ROW] >> 16) * (r[REC_ROW] & 0xffff);
	d->base = (void *)(uintptr_t)r[REC_BASE];
	return d->base && d->width && d->height && (r[REC_ROW] & 0xffff) == 4 &&
	       d->rowbytes >= d->width * 4;
}

int rdn_mesa_attach(void *gld_ctx, long type, const void *drawable)
{
	struct context *c = find(gld_ctx);
	struct drawable d;

	if (!c)
		return 1;
	if (c->type == DRAWABLE_PBUFFER && c->mesa && c->bound &&
	    (type != DRAWABLE_PBUFFER || c->record != drawable)) {
		if (OSMesaAdoptContext(c->mesa)) {
			OSMesaFlushRender(c->mesa, GL_FALSE);
			rdn_set_current_rend(NULL);
		}
	}
	c->type = type;
	c->record = drawable;
	c->bound = 0;
	c->surface_size_valid = 0;
	/* The next GL call binds the new drawable. */
	if (c->rend && c->rend == rdn_current_rend)
		rdn_set_current_rend(NULL);
	read_record(c, &d);
	rdn_log("attach: context %p, type 0x%lx, %ux%u, rowbytes %u, base %p",
		gld_ctx, type, (unsigned)d.width, (unsigned)d.height,
		(unsigned)d.rowbytes, d.base);
	if (type == DRAWABLE_PBUFFER && !d.base) {
		/*
		 * No memory for it (or a size no card has): the call fails
		 * (kCGLBadAlloc) and the context keeps what it had, instead
		 * of drawing into a dummy where nothing is ever seen.
		 */
		c->type = 0;
		c->record = NULL;
		return 0;
	}
	return 1;
}

int rdn_mesa_expect_surface(void *gld_ctx, unsigned long surface)
{
	struct context *c = find(gld_ctx);

	if (!c) {
		rdn_log("surface 0x%lx: no Mesa context for %p", surface, gld_ctx);
		return 0;
	}
	if (!c->mesa && !mesa_for(c)) {
		rdn_log("surface 0x%lx: Mesa context creation failed", surface);
		return 0;
	}
	/* Rebinding must preserve the image used by the kernel's direct flush. */
	if (!reserve_surface((uint32_t)surface)) {
		rdn_log("surface 0x%lx: buffer announcement failed", surface);
		return 0;
	}
	rdn_target_surface_gl((uint32_t)surface);
	return 1;
}

int rdn_mesa_attach_surface(void *gld_ctx, unsigned long connection,
			    unsigned long window, unsigned long surface)
{
	struct context *c = find(gld_ctx);

	if (!c)
		return 0;
	if (c->type == DRAWABLE_PBUFFER && c->mesa && c->bound) {
		if (OSMesaAdoptContext(c->mesa)) {
			OSMesaFlushRender(c->mesa, GL_FALSE);
			rdn_set_current_rend(NULL);
		}
	}
	/* The device is opened with the first Mesa context. */
	if (!c->mesa)
		mesa_for(c);
	if (!c->mesa)
		return 0;
	/* Windowless Quartz contexts draw into their registered CPU backing. */
	if (!window && surface > 1 && rdn_mesa_attach_screen(gld_ctx, surface))
		return 1;
	if (c->surface != surface)
		c->stored = 0;
	c->type = DRAWABLE_SURFACE;
	c->backing_extent_set = 0;
	c->connection = connection;
	c->window = window;
	c->surface = surface;
	c->record = NULL;
	c->bound = 0;
	c->surface_size_valid = 0;
	if (c->rend && c->rend == rdn_current_rend)
		rdn_set_current_rend(NULL);
	rdn_log("attach: context %p draws on surface 0x%lx", gld_ctx, surface);
	surface_arm(c);
	return 1;
}

int rdn_mesa_is_pbuffer(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	return c && c->type == DRAWABLE_PBUFFER;
}

int rdn_mesa_is_dispatched(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	return c && c->dispatched;
}

int rdn_mesa_is_surface(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	/* A surface, or the whole screen in a program's hands. */
	return c && (c->type == DRAWABLE_SURFACE ||
		     ((c->type == DRAWABLE_SCREEN || c->type == DRAWABLE_BACKING) &&
		      !rdn_window_server));
}

int rdn_mesa_is_ws_drawable(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	return c && (c->type == DRAWABLE_SCREEN || c->type == DRAWABLE_BACKING);
}

void rdn_mesa_detach(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	if (!c)
		return;
	if (c->type == DRAWABLE_PBUFFER && c->mesa && c->bound) {
		/* An imported pbuffer can outlive its producer's drawable binding. */
		if (OSMesaAdoptContext(c->mesa)) {
			OSMesaFlushRender(c->mesa, GL_FALSE);
			rdn_set_current_rend(NULL);
		}
	}
	if (c->type == DRAWABLE_SURFACE && c->stored) {
		/* The compositor retains this surface across context detach/reattach. */
		if (c->mesa && OSMesaGetCurrentContext() == c->mesa)
			OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
	}
	c->type = 0;
	c->record = NULL;
	c->bound = 0;
	c->surface_size_valid = 0;
	if (c->rend && c->rend == rdn_current_rend)
		rdn_set_current_rend(NULL);
}

/*
 * The window server composes a frame in several passes, each ended by a
 * glFlush that copies to the screen, and the frame is only whole after the
 * last: the pass with the filter's backdrop is shown without the widget
 * that is drawn over it next, for as long as that pass takes (a few
 * milliseconds: a flicker). So the copies are put off and merged
 * (OSMesaDeferPresent) and made when a few milliseconds have passed
 * with the window server's thread waiting for a message (mach_msg with a
 * timeout, rdn_hook_mach_msg; it has no run loop for a timer), or when it
 * calls for a message and the copies are older than that. Until that
 * thread has been seen to wait many times, every flush copies at once as
 * before.
 */
#include <pthread.h>

#include <sys/time.h>

#define DEFER_MS 2

static pthread_t defer_thread;
static int defer_set, defer_on, defer_pending;
static unsigned defer_idle_calls;
static struct timeval defer_first;
static OSMesaContext defer_ctx;

static void defer_hook(OSMesaContext m)
{
	defer_ctx = m;
	if (!defer_pending) {
		defer_pending = 1;
		gettimeofday(&defer_first, NULL);
	}
}

static void defer_now(void)
{
	int i;

	defer_pending = 0;
	for (i = 0; defer_ctx && i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].mesa == defer_ctx) {
			rdn_make_current(contexts[i].rend);
			OSMesaPresentPending(defer_ctx);
			break;
		}
}

/* The window server's thread is about to wait for a message. */
static unsigned defer_idle(void)
{
	struct timeval now;
	long age_us;

	if (!defer_set || !pthread_equal(pthread_self(), defer_thread))
		return 0;
	if (!defer_on) {
		/* Seen to go idle between frames, often enough. */
		if (++defer_idle_calls == 10) {
			defer_on = 1;
			OSMesaDeferPresent(GL_TRUE, defer_hook);
			rdn_log("the window server's thread goes idle: copies to the screen are put off for up to %d ms", DEFER_MS);
		}
		return 0;
	}
	if (!defer_pending)
		return 0;
	gettimeofday(&now, NULL);
	age_us = (now.tv_sec - defer_first.tv_sec) * 1000000L + (now.tv_usec - defer_first.tv_usec);
	if (age_us >= DEFER_MS * 1000L) {
		defer_now();
		return 0;
	}
	return (unsigned)((DEFER_MS * 1000L - age_us + 999) / 1000);
}

/* The time ran out with no message: the frame is whole. */
static void defer_timeout(void)
{
	if (defer_pending)
		defer_now();
}

static void defer_setup(void)
{
	if (defer_set || access("/Library/Application Support/RadeonNI/nodefer", F_OK) == 0)
		return;
	defer_thread = pthread_self();
	defer_set = 1;
	rdn_hook_mach_msg(defer_idle, defer_timeout);
}

/* Quartz's first viewport gives the unpadded backing extent. */
void rdn_mesa_backing_viewport(void *rend, int x, int y, int width, int height)
{
	int i;
	if (x || y || width <= 0 || height <= 0) return;
	for (i = 0; i < MAX_CONTEXTS; ++i) {
		struct context *c = &contexts[i];
		if (!c->gld_ctx || c->rend != rend)
			continue;
		if (c->type == DRAWABLE_SURFACE && c->window && c->surface_size_valid &&
		    ((uint32_t)width != c->surface_width || (uint32_t)height != c->surface_height)) {
			c->surface_size_valid = 0;
			c->sig_valid = 0;
			rdn_set_current_rend(NULL);
			rdn_make_current(rend);
			return;
		}
		if ((c->type != DRAWABLE_BACKING &&
		     (c->type != DRAWABLE_SURFACE || c->window)) ||
		    c->backing_extent_set || (uint32_t)width > c->store_width ||
		    (uint32_t)height > c->store_height)
			continue;
		c->backing_extent_set = 1;
		if ((uint32_t)width == c->screen.width && (uint32_t)height == c->screen.height)
			return;
		c->screen.width = width;
		c->screen.height = height;
		if (c->type == DRAWABLE_BACKING) {
			c->screen_width = width;
			c->screen_height = height;
		}
		rdn_set_current_rend(NULL);
		rdn_make_current(rend);
		return;
	}
}

int rdn_mesa_attach_screen(void *gld_ctx, unsigned long surface)
{
	static int16_t rects[MAX_SCREEN_RECTS][4];
	uint32_t count = 0, i;
	int32_t b[4];
	struct context *c = find(gld_ctx);
	volatile uint32_t *pixels;
	uint32_t width, height, pitch;

	if (!c)
		return 0;
	if (c->type == DRAWABLE_PBUFFER && c->mesa && c->bound) {
		if (OSMesaAdoptContext(c->mesa)) {
			OSMesaFlushRender(c->mesa, GL_FALSE);
			rdn_set_current_rend(NULL);
		}
	}
	/* The device is opened with the first Mesa context. */
	if (!c->mesa)
		mesa_for(c);
	if (!c->mesa || !rdn_target_screen(&pixels, &width, &height, &pitch)) {
		rdn_log("attach: context %p cannot reach the screen", gld_ctx);
		return 0;
	}
	void *backing;
	uint32_t backing_stride, backing_width, backing_height;
	if (surface != 1 && rdn_target_surface_backing((uint32_t)surface, &backing,
	                                              &backing_stride, &backing_width,
	                                              &backing_height)) {
		c->surface = surface;
		if (!backing_store(c, backing, backing_stride, backing_width, backing_height))
			return 0;
		c->type = DRAWABLE_BACKING;
		c->backing_extent_set = 0;
		c->record = NULL;
		c->bound = 0;
		c->origin_x = c->origin_y = 0;
		c->screen_rects = 0;
		c->screen.width = c->screen_width = backing_width;
		c->screen.height = c->screen_height = backing_height;
		c->screen.rowbytes = backing_stride;
		c->screen.base = backing;
		c->screen_pitch = c->store_row_bytes / 4;
		if (c->rend == rdn_current_rend) rdn_set_current_rend(NULL);
		rdn_log("attach: context %p draws into backing surface %lu, %ux%u",
		        gld_ctx, surface, backing_width, backing_height);
		return 1;
	}
	/* Unknown surfaces must never alias the display's scanout. */
	if (surface != 1 && (surface || rdn_window_server)) return 0;
	c->type = DRAWABLE_SCREEN;
	c->surface = surface;
	if (rdn_window_server && rdn_ws_core_image())
		defer_setup();
	c->record = NULL;
	c->bound = 0;
	c->screen_rects = 0;
	if (!rdn_target_surface_region((uint32_t)surface, b, rects,
				       MAX_SCREEN_RECTS, &count) ||
	    b[0] < 0 || b[1] < 0 || b[2] <= 0 || b[3] <= 0 ||
	    (uint32_t)(b[0] + b[2]) > width || (uint32_t)(b[1] + b[3]) > height) {
		b[0] = b[1] = 0;
		b[2] = (int32_t)width;
		b[3] = (int32_t)height;
	} else if (count <= MAX_SCREEN_RECTS) {
		/*
		 * The window server draws only inside the shape; the rest
		 * of the box around it must stay as it is on the screen.
		 */
		for (i = 0; i < count; i++) {
			/* Screen coordinates when drawing direct. */
			c->screen_rect[i][0] = rects[i][0] - (screen_direct() ? 0 : b[0]);
			c->screen_rect[i][1] = rects[i][1] - (screen_direct() ? 0 : b[1]);
			c->screen_rect[i][2] = rects[i][2];
			c->screen_rect[i][3] = rects[i][3];
		}
		c->screen_rects = count;
	}
	c->direct = screen_direct();
	c->screen_width = width;
	c->screen_height = height;
	c->screen_pitch = pitch;
	c->origin_x = b[0];
	c->origin_y = (int)height - (b[1] + b[3]);
	c->screen.width = (uint32_t)b[2];
	c->screen.height = (uint32_t)b[3];
	c->screen.rowbytes = pitch * 4;
	c->screen.base = (void *)(pixels + (uint32_t)b[1] * pitch + (uint32_t)b[0]);
	if (c->rend && c->rend == rdn_current_rend)
		rdn_set_current_rend(NULL);
	rdn_log("attach: context %p draws on the screen at %d,%d %dx%d (surface %lu, %u rectangles, %s)",
		gld_ctx, (int)b[0], (int)b[1], (int)b[2], (int)b[3], surface,
		(unsigned)count, c->direct ? "direct" : "copied");
	return 1;
}

static void *lookup(const char *name)
{
	return (void *)OSMesaGetProcAddress(name);
}

void rdn_mesa_ensure_current(void *cgl_ctx)
{
	int i;

	if (!cgl_ctx)
		return;
	for (i = 0; i < MAX_CONTEXTS; ++i) {
		struct context *c = &contexts[i];
		if (c->gld_ctx && !c->rend && c->early_rend == *(void **)cgl_ctx) {
			rdn_mesa_early_all(cgl_ctx);
			return;
		}
	}
}

void *rdn_mesa_lookup_missing(const char *name, void *cgl_ctx)
{
	void *function;
	int i;

	if (!name || name[0] != 'g' || name[1] != 'l')
		return NULL;
	if (!cgl_ctx)
		return mesa_ready() ? lookup(name) : NULL;
	rdn_mesa_ensure_current(cgl_ctx);
	function = lookup(name);
	if (!function)
		return NULL;
	for (i = 0; i < MAX_CONTEXTS; ++i) {
		struct context *c = &contexts[i];
		if (c->gld_ctx && c->rend == *(void **)cgl_ctx) {
			rdn_make_current(c->rend);
			return OSMesaGetCurrentContext() == c->mesa ? function : NULL;
		}
	}
	return NULL;
}

static void missing(const char *name)
{
	rdn_log("Mesa has no %s; the engine keeps that entry", name);
}

/* Mesa's functions looked up, once. False if there is no card to use. */
extern void (*osmesa_present_log)(int rects, unsigned long sum, const uint8_t *rgb, int w, int h);
extern int osmesa_present_area[4];
static void present_log(int rects, unsigned long sum, const uint8_t *rgb, int w, int h)
{
	static int count = -1;
	char name[64];
	FILE *f;
	int i;

	if (count < 0)
		count = access("/tmp/rdngld.dump", F_OK) == 0 ? 0 : -2;
	if (rects < 0) {
		rdn_log("FLUSH of the screen context, %d rectangles", -rects);
		return;
	}
	rdn_log("PRESENT to the screen, %d copies, sum %lu, frame %d", rects, sum, count >= 0 ? count : -1);
	if (count < 0 || !rgb || w <= 0 || h <= 0)
		return;
	snprintf(name, sizeof(name), "/tmp/rdnframes/%05d.ppm", count++);
	f = fopen(name, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < h; i++)
		fwrite(rgb + i * 160 * 3, 3, (size_t)w, f);
	fclose(f);
}

/*
 * Programs written for Apple's OpenGL 2.1 that need more of it when Mesa
 * says 4.6: Gecko (PowerFox) asks for glGetStringi from a 3.0 or newer
 * context, and Leopard's OpenGL framework does not export it, so the
 * browser stops itself at start. A program listed in the file
 * RDN_GLVERSION_LIST (a line each: the program's name as the system has
 * it; "-name" and "*" as for glthread) sees 2.1 and GLSL 1.20, which is
 * what Apple's engine reports there. RDN_GL_VERSION21=1 or 0 in a
 * program's environment decides for it. No other program is changed.
 */
#define RDN_GLVERSION_LIST "/Library/Application Support/RadeonNI/glversion21"

/* Is this program named in the file, or has the variable (1 or 0) been set? */
static int program_listed(const char *var, const char *path)
{
	const char *env = getenv(var);
	const char *name = getprogname();
	int named = -1, all = -1;
	char line[256];
	FILE *f;

	if (env)
		return strcmp(env, "0") != 0;
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		const char *who = line;
		int on = 1;

		line[strcspn(line, "\r\n")] = 0;
		if (*who == '-') {
			on = 0;
			who++;
		}
		if (!strcmp(who, "*"))
			all = on;
		else if (name && !strcmp(who, name))
			named = on;
	}
	fclose(f);
	return named >= 0 ? named : all > 0;
}

static int want_version21(void)
{
	return program_listed("RDN_GL_VERSION21", RDN_GLVERSION_LIST);
}

/*
 * A legacy context (renderer 0x00021a00, Apple's 2.1 engine) reports GL 2.1
 * and GLSL 1.20 even where Mesa computes more for the card (3.3 on Southern
 * and Sea Islands since the packed formats work on a big-endian host):
 * Leopard's OpenGL framework dispatches 2.1 and exports no 3.x entry point,
 * and a program that sees 3.x goes looking for them (PowerFox, LWJGL's
 * glGetStringi). Every extension Mesa has is still listed. A program named
 * in RDN_GLVERSIONFULL_LIST (lines as for the 2.1 list above), or started
 * with RDN_GL_VERSION_FULL=1, sees Mesa's own version instead. The modern
 * bridge (RDNGLRequestCompatibilityContext) and the 3.3 core renderer ask
 * for their version and are not affected.
 */
#define RDN_GLVERSIONFULL_LIST "/Library/Application Support/RadeonNI/glversionfull"

static int want_version_full(void)
{
	return program_listed("RDN_GL_VERSION_FULL", RDN_GLVERSIONFULL_LIST);
}

/* Before the first legacy context; once. */
static void legacy_version_cap(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	if (want_version_full())
		rdn_log("legacy contexts report Mesa's own GL version (%s)", RDN_GLVERSIONFULL_LIST);
	else
		OSMesaLegacyVersionCap(21);
}

/*
 * A program named in RDN_SOFTWARE_LIST (same lines as the list above), or
 * started with RDN_SOFTWARE=1, is left to Apple's software renderer: Mesa
 * and the card are not used for it. To tell a program's trouble from ours.
 */
#define RDN_SOFTWARE_LIST "/Library/Application Support/RadeonNI/software"

static int mesa_ready(void)
{
	unsigned n;

	if (usable < 0) {
		if (program_listed("RDN_SOFTWARE", RDN_SOFTWARE_LIST)) {
			usable = 0;
			rdn_log("Mesa is not used for this program (RDN_SOFTWARE): Apple's software renderer");
		}
	}
	if (usable < 0) {
		/* Before Mesa makes its first context: it reads the override once. */
		if (want_version21()) {
			setenv("MESA_GL_VERSION_OVERRIDE", "2.1", 0);
			setenv("MESA_GLSL_VERSION_OVERRIDE", "120", 0);
			rdn_log("GL 2.1 and GLSL 1.20 for this program (%s)", RDN_GLVERSION_LIST);
		}
		legacy_version_cap();
		/* A context to find out whether the card is there at all. */
		OSMesaContext probe = OSMesaCreateContextExt(OSMESA_BGRA, 24, 8, 0, NULL);

		usable = probe != NULL;
		if (probe)
			OSMesaDestroyContext(probe);
		rdn_log("Mesa on the card: %s", usable ? "available" : "not available");
	}
	if (!usable)
		return 0;
	if (!resolved) {
		mesa_finish = (void (*)(void))OSMesaGetProcAddress("glFinish");
		if (rdn_logging) {
			osmesa_present_log = present_log;
			/* Summing a part of every frame (and dumping it) slows a present down: only with /tmp/rdngld.dump. */
			if (access("/tmp/rdngld.dump", F_OK) == 0) {
				osmesa_present_area[0] = 560;
				osmesa_present_area[1] = 380;
				osmesa_present_area[2] = 640;
				osmesa_present_area[3] = 330;
			}
		}
		if (rdn_ws_core_image())
			OSMesaPresentOnSwitch(GL_FALSE);
		mesa_flush = (void (*)(void))OSMesaGetProcAddress("glFlush");
		n = rdn_dispatch_resolve(lookup, missing);
		rdn_log("Mesa provides %u of the %u GL entry points", n,
			rdn_dispatch_entries);
		resolved = 1;
	}
	return 1;
}

static int install(struct context *c, void *engine_table, const char *when)
{
	const struct rdn_engine_layout *engine = rdn_engine_layout();
	void *app_table;
	unsigned n;

	if (!engine)
		return 0;	/* a system whose engine is not known: leave it alone */
	app_table = *(void **)((char *)engine_table - APP_DISPATCH_BACK);
	if (!app_table || !mesa_ready())
		return 0;
	c->rend = (char *)engine_table - ENGINE_CTX_TABLE_OFFSET;
	if (getenv("RDN_GLD_MASTER")) {
		/*
		 * Experiment: does the engine keep, inside its context, the
		 * table it fills the program's from? Look for the program's
		 * first entries there.
		 */
		void **t = app_table, **r = (void **)c->rend;
		unsigned i;

		rdn_log("table %p starts %p %p %p %p (%s)", app_table, t[0], t[1],
			t[2], t[3], when);
		for (i = 0; i + 4 <= (ENGINE_CTX_TABLE_OFFSET + 0x80) / 4; i++)
			if (r[i] == t[0] && r[i + 1] == t[1] && r[i + 2] == t[2] &&
			    r[i + 3] == t[3] && (void **)&r[i] != t)
				rdn_log("  the same four at engine context + 0x%x", i * 4);
	}
	if (c->dispatched && getenv("RDN_GLD_MASTER")) {
		/* Experiment: which entries has the engine taken back? */
		static void *before[RDN_MAX_DISP_ENTRIES];
		void **t = app_table;
		unsigned i, changed = 0, first = 0;

		memcpy(before, t, APP_DISPATCH_ENTRIES * sizeof(void *));
		rdn_dispatch_install(app_table, APP_DISPATCH_ENTRIES);
		for (i = 0; i < APP_DISPATCH_ENTRIES; i++)
			if (before[i] != t[i] && !changed++)
				first = i;
		if (changed)
			rdn_log("  the engine had taken back %u entries, the first at index %u (%s)",
				changed, first, when);
	}
	/* Not before the context has a drawable: see rdn_glue.h. */
	rdn_kept_now = c->type != 0;
	n = rdn_dispatch_install(app_table, APP_DISPATCH_ENTRIES);
	rdn_kept_now = 0;
	if (!c->dispatched || rdn_trace)
		rdn_log("context %p (engine %p): %u entries of table %p are Mesa's (%s)",
			c->gld_ctx, c->rend, n, app_table, when);
	c->dispatched = 1;
	return 1;
}

int rdn_mesa_dispatch(void *gld_ctx, void *engine_table)
{
	struct context *c = find(gld_ctx);
	struct drawable d;

	if (c) {
		c->surface_size_valid = 0;
		c->sig_valid = 0;
		if (c->bound && c->type == DRAWABLE_SURFACE && c->window &&
		    c->rend && c->rend == rdn_current_rend)
			rdn_set_current_rend(NULL);
	}
	/*
	 * A context taken over early stays Mesa's whatever its drawable,
	 * and a surface is always ours.
	 */
	if (!c || (!c->dispatched && c->type != DRAWABLE_SURFACE &&
		   !read_record(c, &d)))
		return 0;
	return install(c, engine_table, "dispatch set-up");
}

int rdn_mesa_unpack_row_bytes(void *rend, int value)
{
	int i;
	for (i = 0; i < MAX_CONTEXTS; ++i)
		if (contexts[i].gld_ctx && contexts[i].rend == rend) {
			if (value >= 0) contexts[i].unpack_row_bytes = value;
			return contexts[i].unpack_row_bytes;
		}
	return 0;
}

/*
 * QUARTZ GL: THE CPU'S EDITS REACH THE STORE BEFORE GL DRAWS OVER THEM.
 *
 * A program whose window draws with Quartz GL (System Preferences, any
 * window asking for NSWindowBackingLocationVideoMemory) draws into the
 * window's surface through this context, but CoreGraphics also changes the
 * window's CPU backing directly -- -[NSView scrollRect:by:] copies the rows
 * there with the CPU and no GL. The window server hands the backing to the
 * CPU with its surface locks; the kext keeps that ownership
 * (RadeonNISurfaceClient::fCPUOwned) and, when asked, copies a CPU-owned
 * backing up into the store (RDN_UC_SURFACE_ACQUIRE).
 *
 * This asks before the first drawing call after every flush of the context
 * (and after it is attached): the CPU's work for the next frame comes before
 * GL's, and the kext answers at once when GL already owns the surface. When
 * it did copy, the context's own colour buffer (which every flush copies
 * over the store) is refreshed from the store before the draw. Seen
 * on 2026-11-10: without it a scrolled list kept its old rows with
 * the newly exposed row drawn over them.
 */
volatile int rdn_surface_acquire_pending;

static int surface_acquires(const struct context *c)
{
	return c && c->type == DRAWABLE_SURFACE && c->surface > 1 && !rdn_window_server;
}

static void surface_arm(struct context *c)
{
	if (surface_acquires(c) && !c->acquire_armed) {
		c->acquire_armed = 1;
		__sync_fetch_and_add(&rdn_surface_acquire_pending, 1);
	}
}

static struct context *find_rend(void *rend)
{
	int i;

	for (i = 0; rend && i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend)
			return &contexts[i];
	return NULL;
}

void rdn_mesa_surface_arm(void *rend)
{
	surface_arm(find_rend(rend));
}

void rdn_mesa_surface_before_draw(void *rend)
{
	struct context *c = find_rend(rend);

	if (!c || !c->acquire_armed)
		return;
	c->acquire_armed = 0;
	__sync_fetch_and_sub(&rdn_surface_acquire_pending, 1);
	/*
	 * The CPU's picture is in the store now; Mesa draws on a colour buffer
	 * of its own and copies it over the store at every flush, so that
	 * buffer has to start from the store again.
	 */
	if (surface_acquires(c) && rdn_target_surface_acquire((uint32_t)c->surface) &&
	    c->mesa && !OSMesaSurfaceReseed(c->mesa))
		rdn_log("surface 0x%lx: copied up, but the colour buffer could not be refreshed",
			(unsigned long)c->surface);
}

void *rdn_mesa_share_group(void *rend)
{
	struct context *c = NULL;
	int i, depth;

	for (i = 0; i < MAX_CONTEXTS; ++i)
		if (contexts[i].gld_ctx && contexts[i].rend == rend) {
			c = &contexts[i];
			break;
		}
	if (!c)
		return rend;
	for (depth = 0; depth < MAX_CONTEXTS && c->share && c->share != c->gld_ctx; ++depth) {
		struct context *s = find(c->share);
		if (!s)
			break;
		c = s;
	}
	return c->gld_ctx;
}

void rdn_mesa_context_engine(void *gld_ctx, void *rend)
{
	struct context *c = find(gld_ctx);

	if (c && !c->early_rend)
		c->early_rend = rend;
}

void rdn_mesa_cgls_context_created(void *cgls_ctx)
{
	void *rend = cgls_ctx ? *(void **)cgls_ctx : NULL;
	int i;

	if (!rend || !rdn_engine_layout())
		return;
	for (i = 0; i < MAX_CONTEXTS; ++i) {
		struct context *c = &contexts[i];
		if (c->gld_ctx && !c->type && c->early_rend == rend) {
			c->early_ready = 1;
			install(c, (char *)c->early_rend + rdn_engine_layout()->table_off,
				"cgls before a drawable");
			return;
		}
	}
}

void rdn_mesa_early_all(void *cgl_ctx)
{
	int i;

	/* What the glue holds back for the context that is current now is made first. */
	rdn_dispatch_flush_pending();
	if (!cgl_ctx) {
		OSMesaContext m = OSMesaGetCurrentContext();

		/*
		 * The thread lets go of its context. Mesa's current context
		 * is per thread: let go of it here too, which makes glthread
		 * run what this thread recorded while it is still the
		 * context's thread (a program that goes on in another
		 * thread, like Quake 3 with r_smp, would else have the
		 * second thread run it, with no current context there).
		 * Not when another thread has taken the context since: Mesa
		 * would unbind it there.
		 */
		for (i = 0; m && i < MAX_CONTEXTS; i++)
			if (contexts[i].gld_ctx && contexts[i].mesa == m) {
				if (pthread_equal(contexts[i].thread, pthread_self()))
					OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
				break;
			}
		rdn_set_current_rend(NULL);
		return;
	}
	for (i = 0; i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx) {
			if (contexts[i].early_rend == *(void **)cgl_ctx)
				contexts[i].early_ready = 1;
			rdn_mesa_early(contexts[i].gld_ctx);
		}
	/* By-name entrypoints bypass the engine's per-call context wrapper. */
	if (!rdn_window_server) {
		for (i = 0; i < MAX_CONTEXTS; ++i)
			if (contexts[i].gld_ctx && contexts[i].rend == *(void **)cgl_ctx) {
				rdn_make_current(contexts[i].rend);
				return;
			}
		OSMesaMakeCurrent(NULL, NULL, GL_UNSIGNED_BYTE, 0, 0);
		rdn_set_current_rend(NULL);
	}
}

void rdn_mesa_early(void *gld_ctx)
{
	struct context *c = find(gld_ctx);

	/*
	 * Again at every call until a drawable is attached: the engine is
	 * still filling the program's table while the context is being
	 * made.
	 */
	if (!c || !c->early_rend || c->type)
		return;
	/* Leopard 10.5.8 publishes the GL table after gldCreateContext returns. */
	if (rdn_engine_layout() && !strcmp(rdn_engine_layout()->os, "10.5.8") &&
	    !c->early_ready) {
		static void *(*get_current)(void);
		void *current;

		/* Clients may cache CGLSetCurrentContext before this bundle loads. */
		if (!get_current)
			get_current = (void *(*)(void))dlsym(RTLD_DEFAULT, "CGLGetCurrentContext");
		current = get_current ? get_current() : NULL;
		if (!current || *(void **)current != c->early_rend)
			return;
		c->early_ready = 1;
	}
	if (rdn_engine_layout())
		install(c, (char *)c->early_rend + rdn_engine_layout()->table_off,
			"before a drawable");
}

static void make_current_slow(void *rend)
{
	struct context *c = NULL;
	struct drawable d;
	int i;

	for (i = 0; i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend)
			c = &contexts[i];
	/*
	 * A mode set changes the screen the window server's contexts draw
	 * on (size and pitch; the device asks the kext at most every 20 ms):
	 * attach it again, or the picture goes into the old shape.
	 */
	if (c && c->type == DRAWABLE_SCREEN && rdn_window_server) {
		volatile uint32_t *px;
		uint32_t w, h, p;

		if (rdn_target_screen(&px, &w, &h, &p) &&
		    (w != c->screen_width || h != c->screen_height || p != c->screen_pitch)) {
			rdn_log("the screen is now %ux%u, pitch %u: attached again", (unsigned)w,
				(unsigned)h, (unsigned)p);
			rdn_mesa_attach_screen(c->gld_ctx, c->surface);
		}
	}
	if (!c) {
		rdn_log("GL call on engine context %p, which is not ours", rend);
		return;
	}
	if (!c->mesa)
		mesa_for(c);
	if (!c->mesa)
		return;
	async_data(c->mesa);
	/*
	 * Another thread of the program takes the context over while it is
	 * attached already (Quake 3's render thread; its main thread draws
	 * the intro): this thread only becomes Mesa's current one.
	 */
	if (c->bound && !c->nowhere && c->thread_set && !pthread_equal(c->thread, pthread_self()) &&
	    !rdn_window_server && !getenv("RDN_GLD_NO_ADOPT")) {
		if (OSMesaAdoptContext(c->mesa)) {
			c->thread = pthread_self();
			if (rdn_trace)
				rdn_log("Mesa adopted in this thread for context %p", c->gld_ctx);
			rdn_set_current_rend(rend);
			return;
		}
	}
	c->thread = pthread_self();
	c->thread_set = 1;
	if (rdn_trace)
		rdn_log("Mesa made current in this thread for context %p", c->gld_ctx);
	if (rdn_trace) {
		int ok = read_record(c, &d);

		rdn_log("make current: context %p type 0x%lx record %p: %s, %ux%u base %p row %u",
			c->gld_ctx, c->type, (const void *)c->record,
			ok ? "drawable" : "no drawable", (unsigned)d.width,
			(unsigned)d.height, d.base, (unsigned)d.rowbytes);
		if (c->record) {
			int k;

			for (k = 0; k < 32; k += 8)
				rdn_log("  record+%02x: %08x %08x %08x %08x %08x %08x %08x %08x",
					k * 4, c->record[k], c->record[k + 1],
					c->record[k + 2], c->record[k + 3],
					c->record[k + 4], c->record[k + 5],
					c->record[k + 6], c->record[k + 7]);
		}
	}
	if (!read_record(c, &d)) {
		/*
		 * No drawable yet: programs load textures and build display
		 * lists before their window exists. Mesa needs something
		 * to be bound to; nothing drawn now is ever shown.
		 */
		static uint32_t *nowhere;
		static int side = 16;

		if (!nowhere) {
			/* Experiment: RDN_GLD_DUMMY=n makes it n pixels a side. */
			if (getenv("RDN_GLD_DUMMY"))
				side = atoi(getenv("RDN_GLD_DUMMY"));
			nowhere = calloc((size_t)side * side, 4);
			if (!nowhere)
				return;
		}

		/*
		 * rdn_current_rend stays as it is, so that every GL call
		 * comes back here and finds the drawable as soon as there
		 * is one (the engine fills an off-screen record only after
		 * gldAttachDrawable has returned).
		 */
		if (c->nowhere && OSMesaGetCurrentContext() == c->mesa) {
			if (!c->type)
				rdn_set_current_rend(rend);
			return;
		}
		rdn_origin_x = rdn_origin_y = 0;
		rdn_set_current_rend(NULL);
		if (!OSMesaMakeCurrent(c->mesa, nowhere, GL_UNSIGNED_BYTE, side, side))
			return;
		OSMesaPixelStore(OSMESA_ROW_LENGTH, side);
		OSMesaReadbackRects(c->mesa, 0, NULL);
		c->bound = 0;
		c->nowhere = 1;
		if (!c->type)
			rdn_set_current_rend(rend);
		return;
	}
	rdn_origin_x = rdn_origin_y = 0;
	if (c->type == DRAWABLE_BACKING) {
		if (!OSMesaMakeCurrentStore(c->mesa, RDN_TARGET_VRAM_HANDLE,
		                            c->store_row_bytes, c->store_offset,
		                            d.width, d.height, 0)) return;
		if (!OSMesaStoreBacking(c->mesa, d.base, d.rowbytes)) return;
		OSMesaReadbackRects(c->mesa, 0, NULL);
		c->drawable = d;
		c->bound = 1;
		c->nowhere = 0;
		rdn_watch = 0;
		rdn_set_current_rend(rend);
		return;
	}
	if (c->type == DRAWABLE_SURFACE) {
		if (!c->window && c->backing_extent_set)
			surface_store(c, c->store_width, c->store_height);
		else
			surface_store(c, d.width, d.height);
		OSMesaSurfaceStorage(c->mesa, c->stored ? RDN_TARGET_VRAM_HANDLE : 0,
				     (GLsizei)c->store_row_bytes, c->store_offset);
		if (!OSMesaMakeCurrentSurface(c->mesa, RDN_TARGET_SCREEN_HANDLE,
					      (GLsizei)c->screen_width,
					      (GLsizei)c->screen_height,
					      (GLsizei)(c->screen_pitch * 4), 0,
					      c->origin_x, c->origin_y,
					      (GLsizei)d.width, (GLsizei)d.height)) {
			rdn_log("OSMesaMakeCurrentSurface failed for context %p",
				c->gld_ctx);
			return;
		}
		/* Only what the window server lets show of the window. */
		if (surface_direct() && c->placed && c->screen_rects) {
			OSMesaReadbackRects(c->mesa, (GLint)c->screen_rects,
					    &c->screen_rect[0][0]);
		} else {
			GLint none[4] = { 0, 0, 0, 0 };

			OSMesaReadbackRects(c->mesa, 1, none);
		}
		c->drawable = d;
		c->bound = 1;
		c->nowhere = 0;
		rdn_set_current_rend(rend);
		return;
	}
	if (c->type == DRAWABLE_SCREEN && c->direct) {
		if (!OSMesaMakeCurrentDirect(c->mesa, RDN_TARGET_SCREEN_HANDLE,
					     (GLsizei)c->screen_width,
					     (GLsizei)c->screen_height,
					     (GLsizei)(c->screen_pitch * 4), 0)) {
			rdn_log("OSMesaMakeCurrentDirect failed for context %p",
				c->gld_ctx);
			return;
		}
		rdn_origin_x = c->origin_x;
		rdn_origin_y = c->origin_y;
		rdn_watch = rdn_window_server;
		/* What is shown when the context is flushed: the region. */
		if (c->screen_rects) {
			OSMesaReadbackRects(c->mesa, (GLint)c->screen_rects,
					    &c->screen_rect[0][0]);
		} else {
			GLint box[4];

			box[0] = c->origin_x;
			box[1] = (GLint)c->screen_height - c->origin_y -
				 (GLint)c->screen.height;
			box[2] = (GLint)c->screen.width;
			box[3] = (GLint)c->screen.height;
			OSMesaReadbackRects(c->mesa, 1, box);
		}
		c->drawable = d;
		c->bound = 1;
		c->nowhere = 0;
		rdn_set_current_rend(rend);
		return;
	}
	if (c->type == DRAWABLE_PBUFFER) {
		struct pbuffer *p = d.base;

		/*
		 * The window server's first calls on its Core Image context
		 * (bind a texture, set its parameters) come before the pbuffer
		 * is attached and carry the screen context's engine context, so
		 * they land in the context that is current, not here. The new
		 * context takes over the rectangle texture bound there.
		 */
		GLint inherit = 0;

		if (pbuffer_ws && !c->ws_state && !c->early_ready &&
		    OSMesaGetCurrentContext() && OSMesaGetCurrentContext() != c->mesa) {
			void (*get_int)(GLenum, GLint *) = (void (*)(GLenum, GLint *))
				OSMesaGetProcAddress("glGetIntegerv");

			if (get_int)
				get_int(0x84f6, &inherit);	/* GL_TEXTURE_BINDING_RECTANGLE */
		}
		if (!OSMesaMakeCurrentStore(c->mesa, RDN_TARGET_VRAM_HANDLE,
					    (GLsizei)p->row_bytes, p->offset,
					    (GLsizei)p->width, (GLsizei)p->height,
					    p->flags | OSMESA_STORE_BOTTOM_UP)) {
			rdn_log("OSMesaMakeCurrentStore failed for context %p",
				c->gld_ctx);
			return;
		}
		p->cleared = 1;
		OSMesaReadbackRects(c->mesa, 0, NULL);
		c->drawable = d;
		c->bound = 1;
		c->nowhere = 0;
		rdn_set_current_rend(rend);
		if (pbuffer_ws && !c->ws_state) {
			/*
			 * The window server's contexts start with rectangle
			 * texturing on: it draws the backdrop copy into its
			 * pbuffer without ever enabling it.
			 */
			void (*enable)(GLenum) = (void (*)(GLenum))OSMesaGetProcAddress("glEnable");
			void (*bind)(GLenum, GLuint) = (void (*)(GLenum, GLuint))
				OSMesaGetProcAddress("glBindTexture");

			c->ws_state = 1;
			if (enable)
				enable(0x84f5);
			if (inherit > 0 && bind)
				bind(0x84f5, (GLuint)inherit);
			if (rdn_logging)
				rdn_log("window server context %p: rectangle texturing on, texture %d bound",
					c->gld_ctx, (int)inherit);
		}
		return;
	}
	/*
	 * 32-bit ARGB words in the host's byte order, which is what both
	 * CGL's buffers and OSMESA_BGRA with GL_UNSIGNED_BYTE are.
	 */
	if (!OSMesaMakeCurrent(c->mesa, d.base, GL_UNSIGNED_BYTE,
			       (GLsizei)d.width, (GLsizei)d.height)) {
		rdn_log("OSMesaMakeCurrent failed for context %p", c->gld_ctx);
		return;
	}
	OSMesaPixelStore(OSMESA_ROW_LENGTH, (GLint)(d.rowbytes / 4));
	/* CGL's buffers hold the bottom row first; the screen the top row. */
	OSMesaPixelStore(OSMESA_Y_UP, c->type == DRAWABLE_OFFSCREEN ||
				      (c->type == DRAWABLE_WINDOW &&
				       !rdn_windows_top_down()));
	OSMesaReadbackRects(c->mesa,
			    c->type == DRAWABLE_SCREEN ? (GLint)c->screen_rects : 0,
			    &c->screen_rect[0][0]);
	c->drawable = d;
	c->bound = 1;
	c->nowhere = 0;
	rdn_flush_waits = glthread;
	rdn_set_current_rend(rend);
}

/*
 * The bindings that can be told again: a program's window, off-screen buffer
 * or surface. The others (the screen, pbuffers, backings) are bound anew.
 */
static int bind_sig_type(const struct context *c)
{
	return c->type == DRAWABLE_WINDOW || c->type == DRAWABLE_OFFSCREEN ||
	       c->type == DRAWABLE_SURFACE;
}

/* What the binding of `c` to `d` is made from, in the context's present state. */
static void bind_sig_of(const struct context *c, const struct drawable *d, struct bind_sig *sig)
{
	uint32_t hash = 2166136261u, i;
	const uint32_t *words = (const uint32_t *)c->screen_rect;

	memset(sig, 0, sizeof(*sig));
	sig->d = *d;
	sig->mesa = c->mesa;
	sig->record = c->record;
	sig->type = c->type;
	sig->connection = c->connection;
	sig->window = c->window;
	sig->surface = c->surface;
	if (c->type != DRAWABLE_SURFACE || !c->window || surface_direct()) {
		sig->origin_x = c->origin_x;
		sig->origin_y = c->origin_y;
		sig->placed = c->placed;
		sig->screen_rects = c->screen_rects;
		for (i = 0; i < c->screen_rects && i < MAX_SCREEN_RECTS * 4; i++)
			hash = (hash ^ words[i]) * 16777619u;
	}
	sig->direct = c->direct;
	sig->stored = c->stored;
	sig->backing_extent_set = c->backing_extent_set;
	sig->screen_width = c->screen_width;
	sig->screen_height = c->screen_height;
	sig->screen_pitch = c->screen_pitch;
	sig->rect_hash = hash;
	sig->store_offset = c->store_offset;
	sig->store_row_bytes = c->store_row_bytes;
	sig->store_width = c->store_width;
	sig->store_height = c->store_height;
}

/*
 * Make the engine context's Mesa context current in this thread, bound to the
 * program's drawable.
 *
 * Programs call aglSetCurrentContext (and for a surface the swap comes
 * here) every frame, with the same context and drawable as in the last. Doing
 * it anew each time is not free: OSMesaMakeCurrent* and OSMesaSurfaceStorage
 * wait for glthread's thread to run everything the program has called (the
 * calls that follow cannot be queued while it does), and the surface's
 * storage is registered with the kext again. When the context is still the
 * one Mesa has current in this thread and nothing it is bound from has
 * changed (the drawable's size, base and row length, a surface's place,
 * visible rectangles and storage, the screen's mode), only what the slow
 * way leaves in the globals is put back. Every 128th call takes the slow way
 * anyway. RDN_GLD_NO_REBIND_SKIP=1 in the environment always does.
 */
void rdn_make_current(void *rend)
{
	rdn_lazy_flush();
	struct context *c = NULL;
	struct drawable d;
	struct bind_sig now;
	static int never = -1;
	int i;

	if (never < 0)
		never = getenv("RDN_GLD_NO_REBIND_SKIP") != NULL;
	for (i = 0; i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend)
			c = &contexts[i];
	if (c && c->sig_valid && !never && !rdn_trace && c->bound && !c->nowhere && c->mesa &&
	    (++c->sig_age & 127) && pthread_equal(c->thread, pthread_self()) &&
	    OSMesaGetCurrentContext() == c->mesa && read_record(c, &d)) {
		bind_sig_of(c, &d, &now);
		if (!memcmp(&now, &c->sig, offsetof(struct bind_sig, global_origin_x))) {
			async_data(c->mesa);
			rdn_origin_x = c->sig.global_origin_x;
			rdn_origin_y = c->sig.global_origin_y;
			rdn_watch = c->sig.watch;
			rdn_flush_waits = c->sig.flush_waits;
			rdn_set_current_rend(rend);
			return;
		}
	}
	if (c) {
		if (c->sig_valid && !(c->sig_age & 127))
			c->surface_size_valid = 0;
		c->sig_valid = 0;
	}
	make_current_slow(rend);
	if (c && !never && c->mesa && c->bound && !c->nowhere && bind_sig_type(c) &&
	    rdn_current_rend == rend) {
		bind_sig_of(c, &c->drawable, &c->sig);
		c->sig.global_origin_x = rdn_origin_x;
		c->sig.global_origin_y = rdn_origin_y;
		c->sig.watch = rdn_watch;
		c->sig.flush_waits = rdn_flush_waits;
		c->sig_valid = 1;
	}
}

/* True if the engine's record no longer says what Mesa is bound to. */
static int drawable_changed(const struct context *c)
{
	struct drawable d;

	return !read_record(c, &d) || memcmp(&d, &c->drawable, sizeof(d)) != 0;
}

/*
 * A program's surface inside the window server's picture.
 *
 * Where a program's OpenGL surface is, the window server draws a plain
 * white rectangle: one quad, with texturing off, white, whose texture
 * coordinates are nevertheless the part of the surface it stands for, and
 * leaves the surface itself to the driver. The calls are watched as they
 * go by (gen_dispatch.py, WATCHED); when such a quad has been drawn, the
 * surface's picture is copied over it in the window server's own drawing
 * buffer, there and then, so that whatever the window server draws next
 * (windows in front, shadows) comes out on top of it and the whole goes to
 * the screen in one piece. The window server's own coordinates are used,
 * not the shape the kext was told, which lags behind while a window is
 * dragged.
 */
int rdn_watch;
int rdn_window_server;

static struct {
	/* glOrtho's left and top: the desktop position of the drawable's corner. */
	double left, top;
	int unit, rect_on[2];
	int white, in_quads, vertices, texcoords;
	float v[4][2], t[4][2], s_now, t_now;
} watch;

void rdn_watch_ortho(double left, double right, double bottom, double top,
		     double z_near, double z_far)
{
	watch.left = left;
	watch.top = top;
}

void rdn_watch_active_texture(unsigned unit)
{
	watch.unit = unit == GL_TEXTURE1;
}

void rdn_watch_enable(unsigned cap)
{
	if (cap == GL_TEXTURE_RECTANGLE_ARB)
		watch.rect_on[watch.unit] = 1;
}

void rdn_watch_disable(unsigned cap)
{
	if (cap == GL_TEXTURE_RECTANGLE_ARB)
		watch.rect_on[watch.unit] = 0;
}

void rdn_watch_color4ub(unsigned char r, unsigned char g, unsigned char b,
			unsigned char a)
{
	watch.white = (r & g & b & a) == 0xff;
}

void rdn_watch_begin(unsigned mode)
{
	watch.in_quads = mode == GL_QUADS;
	watch.vertices = 0;
	watch.texcoords = 0;
}

void rdn_watch_tex_coord2f(float s, float t)
{
	watch.s_now = s;
	watch.t_now = t;
	watch.texcoords++;
}

void rdn_watch_vertex2f(float x, float y)
{
	if (watch.in_quads && watch.vertices < 4) {
		watch.v[watch.vertices][0] = x;
		watch.v[watch.vertices][1] = y;
		watch.t[watch.vertices][0] = watch.s_now;
		watch.t[watch.vertices][1] = watch.t_now;
	}
	watch.vertices++;
}

/*
 * A surface as a texture.
 *
 * The better case: the window server makes a rectangle texture for a
 * program's surface, sets its filters and gives it no image, because it
 * has told Apple's engine (a private context parameter, 997) that the
 * texture is the surface; it then draws the surface as an ordinary
 * textured quad. The engine keeps that to itself, but the window server
 * does all of it while it holds the surface locked for reading, and the
 * kext knows which surface that is. So: a rectangle texture whose
 * anisotropy is set before it has an image, while a surface is locked,
 * gets that surface's picture in video memory as its image, uncopied.
 */
static int watch_texture_has_image;

/* Opt-in, bounded snapshots of CPU uploads before compositor drawing. */
static void watch_upload(unsigned target, int width, int height,
			 unsigned format, unsigned type, const void *pixels)
{
	static unsigned count;
	void (*get)(GLenum, GLint *);
	GLint pbo = 0, row = 0, skip_x = 0, skip_y = 0, texture = 0;
	char path[128];
	FILE *fp;
	int y;

	if (count >= 24 || target != GL_TEXTURE_RECTANGLE_ARB ||
	    width < 128 || width > 512 || height < 128 || height > 512 ||
	    format != GL_BGRA || type != GL_UNSIGNED_INT_8_8_8_8_REV || !pixels ||
	    access("/tmp/rdngld.uploads", F_OK))
		return;
	get = (void (*)(GLenum, GLint *))OSMesaGetProcAddress("glGetIntegerv");
	if (!get)
		return;
	get(0x88EF, &pbo); /* GL_PIXEL_UNPACK_BUFFER_BINDING */
	if (pbo)
		return;
	get(GL_UNPACK_ROW_LENGTH, &row);
	get(GL_UNPACK_SKIP_PIXELS, &skip_x);
	get(GL_UNPACK_SKIP_ROWS, &skip_y);
	get(GL_TEXTURE_BINDING_RECTANGLE_ARB, &texture);
	if (!row)
		row = width;
	if (row < width || row > 16384 || skip_x < 0 || skip_y < 0)
		return;
	snprintf(path, sizeof(path), "/tmp/rdn-upload-%d-%02u-%x-%dx%d.argb",
		 (int)getpid(), count++, (unsigned)texture, width, height);
	fp = fopen(path, "wb");
	if (!fp)
		return;
	for (y = 0; y < height; ++y)
		if (fwrite((const char *)pixels + ((size_t)(skip_y + y) * row + skip_x) * 4,
			   4, width, fp) != (size_t)width)
			break;
	fclose(fp);
}

void rdn_watch_bind_texture(unsigned target, unsigned texture)
{
	if (target == GL_TEXTURE_RECTANGLE_ARB)
		watch_texture_has_image = 0;
}

void rdn_watch_tex_image2D(unsigned target, int level, int internalformat,
			   int width, int height, int border, unsigned format,
			   unsigned type, const void *pixels)
{
	if (target == GL_TEXTURE_RECTANGLE_ARB)
		watch_texture_has_image = 1;
	watch_upload(target, width, height, format, type, pixels);
}

void rdn_watch_tex_sub_image2D(unsigned target, int level, int xoffset,
			       int yoffset, int width, int height,
			       unsigned format, unsigned type, const void *pixels)
{
	if (target == GL_TEXTURE_RECTANGLE_ARB)
		watch_texture_has_image = 1;
	watch_upload(target, width, height, format, type, pixels);
}

void rdn_watch_tex_parameterf(unsigned target, unsigned pname, float param)
{
	uint32_t info[5], id;
	bool found;
	struct context *c = NULL;
	static unsigned noted;
	int j;

	if (rdn_surface_texture_hook || target != GL_TEXTURE_RECTANGLE_ARB || watch_texture_has_image ||
	    pname != 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */)
		return;
	id = rdn_target_surface_locked();
	if (!id)
		return;
	for (j = 0; j < MAX_CONTEXTS; j++)
		if (contexts[j].gld_ctx && contexts[j].rend == rdn_current_rend)
			c = &contexts[j];
	if (!c || !c->mesa)
		return;
	if (rdn_target_surface_query(id, info, &found) && found && info[3] && info[4]) {
		rdn_client_tex_drop_bound(c->rend, GL_TEXTURE_RECTANGLE_ARB);
		if (OSMesaTexStore(c->mesa, GL_TEXTURE_RECTANGLE_ARB,
				   RDN_TARGET_VRAM_HANDLE, (GLsizei)info[2],
				   info[1], (GLsizei)info[3], (GLsizei)info[4]))
			watch_texture_has_image = 1;
		if (noted++ < 20)
			rdn_log("surface 0x%x is the image of a texture of the window server's: %s",
				(unsigned)id, watch_texture_has_image ? "done" : "failed");
		return;
	}
	if (noted++ < 20)
		rdn_log("surface 0x%x is locked for a texture but has no buffer", (unsigned)id);
}

void rdn_watch_end(void)
{
	uint32_t list[32][5], n, i, best = 32;
	struct context *c = NULL;
	int sx, sy, sw, sh, dx, dy, j;
	static unsigned noted;
	/* Where each surface's corner was when it was drawn last. */
	enum { MAX_DRAWN = 32 };
	static struct { uint32_t id; long x, y; } drawn[MAX_DRAWN];
	long best_d = 0;

	if (rdn_surface_texture_hook || !watch.in_quads || watch.vertices != 4 || watch.texcoords != 4 ||
	    !watch.white || watch.rect_on[0] || watch.rect_on[1])
		return;
	for (j = 0; j < MAX_CONTEXTS; j++)
		if (contexts[j].gld_ctx && contexts[j].rend == rdn_current_rend)
			c = &contexts[j];
	if (!c || c->type != DRAWABLE_SCREEN || !c->direct || !c->bound)
		return;
	/* Corners go round from the top left: 0 and 2 are opposite. */
	sx = (int)(watch.t[0][0] + 0.5f);
	sy = (int)(watch.t[0][1] + 0.5f);
	sw = (int)(watch.t[2][0] + 0.5f) - sx;
	sh = (int)(watch.t[2][1] + 0.5f) - sy;
	if (sx < 0 || sy < 0 || sw <= 0 || sh <= 0 ||
	    (int)(watch.v[2][0] - watch.v[0][0] + 0.5f) != sw ||
	    (int)(watch.v[2][1] - watch.v[0][1] + 0.5f) != sh)
		return;
	/*
	 * The drawable's corner is at origin_x from the screen's left and
	 * origin_y from its bottom, and at glOrtho's left, top on the
	 * desktop.
	 */
	dx = (int)(watch.v[0][0] - watch.left) + c->origin_x;
	dy = (int)(watch.v[0][1] - watch.top) +
	     ((int)c->screen_height - c->origin_y - (int)c->screen.height);

	/*
	 * Whose is it? Nothing in the quad says. Of the surfaces it fits,
	 * the one whose corner would be nearest to where that surface was
	 * drawn last or to where the kext has it (which is right except for
	 * a step's lag during a drag), whichever is nearer.
	 */
	n = rdn_target_surface_list(list, 32);
	for (i = 0; i < n; i++) {
		static int16_t rects[MAX_SCREEN_RECTS][4];
		long ox = dx - sx, oy = dy - sy, d, bx, by;
		uint32_t count, k;
		int32_t b[4];

		if ((uint32_t)(sx + sw) > list[i][3] || (uint32_t)(sy + sh) > list[i][4])
			continue;
		for (k = 0; k < MAX_DRAWN; k++)
			if (drawn[k].id == list[i][0])
				break;
		d = -1;
		if (k < MAX_DRAWN) {
			bx = drawn[k].x;
			by = drawn[k].y;
			d = labs(ox - bx) + labs(oy - by);
		}
		/* The nearer of the two counts. */
		if (rdn_target_surface_region(list[i][0], b, rects,
					      MAX_SCREEN_RECTS, &count)) {
			long e = labs(ox - b[0]) + labs(oy - b[1]);

			if (d < 0 || e < d)
				d = e;
		}
		if (d < 0)
			continue;
		if (best == 32 || d < best_d) {
			best = i;
			best_d = d;
		}
	}
	if (best != 32) {
		uint32_t k, slot = 0;

		for (k = 0; k < MAX_DRAWN; k++) {
			if (drawn[k].id == list[best][0]) {
				slot = k;
				break;
			}
			/* Otherwise a free place, or the one of a surface that is gone. */
			if (!drawn[k].id)
				slot = k;
		}
		if (drawn[slot].id != list[best][0]) {
			for (k = 0; k < MAX_DRAWN; k++) {
				uint32_t m;

				for (m = 0; m < n; m++)
					if (list[m][0] == drawn[k].id)
						break;
				if (m == n) {
					slot = k;
					break;
				}
			}
		}
		drawn[slot].id = list[best][0];
		drawn[slot].x = dx - sx;
		drawn[slot].y = dy - sy;
	}
	if (best == 32) {
		if (noted++ < 40)
			rdn_log("a white quad %dx%d (from %d,%d) fits none of %u surfaces",
				sw, sh, sx, sy, (unsigned)n);
		return;
	}
	if (rdn_trace || noted++ < 40)
		rdn_log("surface 0x%x drawn into the window server's picture: %d,%d %dx%d of it at %d,%d",
			(unsigned)list[best][0], sx, sy, sw, sh, dx, dy);
	OSMesaDrawStore(c->mesa, RDN_TARGET_VRAM_HANDLE, (GLsizei)list[best][2],
			list[best][1], (GLsizei)list[best][3], (GLsizei)list[best][4],
			sx, sy, sw, sh, dx, dy);
}

static int surface_direct(void)
{
	static int known = -1;

	if (known < 0)
		known = getenv("RDN_GLD_DIRECT_SWAP") != NULL;
	return known;
}

void rdn_flush_surface(void *rend)
{
	int i;

	for (i = 0; i < MAX_CONTEXTS; i++) {
		if (!contexts[i].gld_ctx || contexts[i].rend != rend)
			continue;
		if (contexts[i].has_shared_buffer && contexts[i].mesa)
			OSMesaFlushRender(contexts[i].mesa, GL_FALSE);
		if (contexts[i].type == DRAWABLE_SURFACE) {
			if (contexts[i].dbl)
				return;	/* only a swap shows the back buffer */
			if (contexts[i].swaps == 0 && contexts[i].bound) {
				/* As for a swap: the picture is complete, then the window server knows. */
				rdn_mesa_present(contexts[i].gld_ctx);
				rdn_surface_flush(contexts[i].connection, contexts[i].window,
						  contexts[i].surface);
			}
			return;
		}
	}
}

int rdn_swap(void *rend)
{
	int i, tries;

	for (i = 0; i < MAX_CONTEXTS; i++) {
		if (contexts[i].gld_ctx && contexts[i].rend == rend &&
		    contexts[i].type == DRAWABLE_SCREEN && !rdn_window_server) {
			/* The whole screen: the copy is all there is to do. */
			rdn_mesa_present(contexts[i].gld_ctx);
			return 1;
		}
		if (contexts[i].gld_ctx && contexts[i].rend == rend &&
		    contexts[i].type == DRAWABLE_SURFACE) {
			if ((contexts[i].swaps & 63) == 0)
				contexts[i].surface_size_valid = 0;
			if (contexts[i].swaps++ % 100 < 2)
				rdn_log("swap: surface 0x%lx at %d,%d, %u rectangles, bound %d",
					contexts[i].surface, contexts[i].origin_x,
					contexts[i].origin_y,
					(unsigned)contexts[i].screen_rects,
					contexts[i].bound);
			/*
			 * The picture goes to the surface's buffer, and the
			 * window server is told. It decides how the picture
			 * gets to the screen: if nothing else changed and
			 * nothing translucent lies over the window, it has
			 * the kext copy the buffer there
			 * (RadeonNIAccel::flushSurface); otherwise it draws
			 * the window's area again with the buffer as a
			 * texture. RDN_GLD_DIRECT_SWAP=1 also puts it on
			 * the screen from here, which is wrong when the
			 * window is moving or partly covered.
			 */
			if (surface_direct()) {
				for (tries = 0; tries < 50; tries++) {
					struct drawable d;

					if (surface_drawable(&contexts[i], &d) &&
					    contexts[i].placed)
						break;
					usleep(10000);
				}
			}
			rdn_mesa_present(contexts[i].gld_ctx);
			rdn_surface_flush(contexts[i].connection, contexts[i].window,
					  contexts[i].surface);
			return 1;
		}
	}
	return 0;
}

static int ws_finish(void)
{
	static int known = -1;

	if (known < 0)
		known = getenv("RDN_GLD_WS_FINISH") != NULL;
	return known;
}

static int swap_finish(void)
{
	static int known = -1;

	if (known < 0)
		known = getenv("RDN_GLD_SWAP_FINISH") != NULL;
	return known;
}

int rdn_finish_surface(void *rend)
{
	int i;

	if (swap_finish())
		return 0;
	for (i = 0; i < MAX_CONTEXTS; i++)
		if (contexts[i].gld_ctx && contexts[i].rend == rend &&
		    contexts[i].type == DRAWABLE_SURFACE &&
		    contexts[i].stored && !surface_direct()) {
			if (mesa_flush)
				mesa_flush();
			rdn_flush_surface(rend);
			return 1;
		}
	return 0;
}

/*
 * A program asked for a multisampled pixel format. Contexts made after
 * this get that many samples; RDN_GLD_NO_MSAA in the environment keeps
 * them at one.
 */
/*
 * A program chose a double-buffered pixel format (kCGLPFADoubleBuffer). The
 * contexts made with it draw on a back buffer that a swap shows; with
 * RDN_GLD_NO_BACKBUFFER=1 in the program's environment they do not, and
 * GL_BACK is GL_FRONT, which is shown at every flush, as before.
 */
void rdn_mesa_double(int yes)
{
	if (getenv("RDN_GLD_NO_BACKBUFFER"))
		yes = 0;
	pending_double = yes != 0;
	if (yes)
		rdn_log("pixel format with a back buffer asked for");
}

void rdn_mesa_samples(int samples)
{
	if (getenv("RDN_GLD_NO_MSAA"))
		samples = 1;
	if (samples > 1)
		rdn_log("pixel format with %d samples asked for", samples);
	OSMesaSetSamples(samples);
}

/*
 * RDN_FPS=1 in the environment, or the file /tmp/rdngld.fps (for programs
 * whose environment is not ours to set): frames a second, once a second, in
 * /tmp/rdn-fps.log. A frame is one present of the program's own drawable.
 */
static void fps_count(void)
{
	static int on = -1;
	static unsigned frames;
	static double last, first;
	struct timeval tv;
	double now;

	if (on < 0)
		on = getenv("RDN_FPS") != NULL || access("/tmp/rdngld.fps", F_OK) == 0;
	if (!on)
		return;
	gettimeofday(&tv, NULL);
	now = tv.tv_sec + tv.tv_usec / 1e6;
	if (last == 0) {
		last = first = now;
		return;
	}
	frames++;
	if (now - last >= 1.0) {
		FILE *f = fopen("/tmp/rdn-fps.log", "a");

		if (f) {
			fprintf(f, "rdn: fps %.1f (frame %.2f ms) at %.0f s\n",
				frames / (now - last), 1000.0 * (now - last) / frames,
				now - first);
			fclose(f);
		}
		frames = 0;
		last = now;
	}
}

void rdn_mesa_present(void *gld_ctx)
{
	struct context *c = find(gld_ctx);
	struct drawable d;

	surface_arm(c);
	if (!c || !c->rend || !mesa_finish || !read_record(c, &d))
		return;
	fps_count();
	/* A direct surface may have moved or been covered; stored surfaces only resize. */
	if (c->rend != rdn_current_rend || !c->bound ||
	    memcmp(&d, &c->drawable, sizeof(d)) != 0 ||
	    (c->type == DRAWABLE_SURFACE && surface_direct()))
		rdn_make_current(c->rend);
	if (c->rend != rdn_current_rend || !c->bound)
		return;
	if (c->dbl) {
		/*
		 * A full-screen program does not wait for its picture: the swap
		 * is queued behind the frame in glthread's thread. A surface is
		 * submitted to the single GFX CP ring before returning.
		 */
		if (c->type == DRAWABLE_SCREEN && !rdn_window_server && !swap_finish())
			OSMesaSwapBuffersAsync(c->mesa);
		else
			OSMesaSwapBuffers(c->mesa);
	}
	/*
	 * A program that has the whole screen, a pbuffer, or a stored surface
	 * whose copy is already ordered ahead of WindowServer on the single
	 * GFX CP ring (and guarded by waitForCPU() if CPU readback ever locks
	 * it), need not stall on a synchronous fence wait. RDN_GLD_SWAP_FINISH=1
	 * waits.
	 */
	if (((c->type == DRAWABLE_SURFACE && c->stored && !surface_direct()) ||
	     c->type == DRAWABLE_PBUFFER) && !swap_finish()) {
		if (!c->dbl && mesa_flush)
			mesa_flush();
	} else if (c->type == DRAWABLE_SCREEN && !rdn_window_server &&
		   mesa_flush && !swap_finish()) {
		mesa_flush();
	} else if (c->type == DRAWABLE_SCREEN && rdn_window_server &&
		   mesa_flush && !ws_finish()) {
		/*
		 * The window server's display updates: the copy to the screen
		 * is queued behind the frame on the one ring, and what the
		 * window server does next with the GPU is queued behind that.
		 * Waiting here for the frame to finish held its event loop
		 * for the GPU's time every update (T4240, 2026-11-11: 27 ms
		 * a drag step at 11 ms of CPU). RDN_GLD_WS_FINISH=1 waits.
		 */
		mesa_flush();
	} else {
		mesa_finish();
	}
}

/*
 * Cross-process shared GPU buffers (LeopardGPU_* C API).
 *
 * Allocates linear VRAM regions from the kernel's shared store pool and wraps
 * their metadata + cross-process reference counts in an unlinked 4 KiB
 * shared-memory file descriptor passable via SCM_RIGHTS.
 */
#define LEOPARD_FOURCC_ARGB8888 0x34325241u
#define LEOPARD_FOURCC_XRGB8888 0x34325258u
#define LEOPARD_MODIFIER_LINEAR 0ULL
#define LEOPARD_MODIFIER_INVALID 0x00ffffffffffffffULL
#define LGPU_META_MAGIC 0x4c475055u
#define LGPU_META_VERSION 1u
#define LGPU_MAX_BUFFERS 256
#define LGPU_MAX_DEFERRED 256
#define LGPU_MAX_HOLDERS 32

struct lgpu_holder_slot {
	volatile int32_t pid;
	volatile int32_t refs;
	volatile int32_t exported;
};

struct lgpu_shared_meta {
	uint32_t magic;
	uint32_t version;
	uint32_t gpu_offset;
	uint32_t size;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t offset;
	uint32_t stride;
	uint64_t modifier;
	volatile int32_t ref_handles;
	volatile int32_t exported_fds;
	struct lgpu_holder_slot holders[LGPU_MAX_HOLDERS];
};

struct lgpu_buffer_entry {
	uint32_t id;
	int shm_fd;
	int owner;
	struct lgpu_shared_meta *meta;
};

struct lgpu_deferred_entry {
	int shm_fd;
	struct lgpu_shared_meta *meta;
};

static pthread_mutex_t lgpu_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lgpu_buffer_entry lgpu_buffers[LGPU_MAX_BUFFERS];
static struct lgpu_deferred_entry lgpu_deferred[LGPU_MAX_DEFERRED];
static uint32_t lgpu_next_id = 1;

static void lgpu_holder_adjust(struct lgpu_shared_meta *m, int32_t d_refs,
			       int32_t d_exp)
{
	int32_t my_pid = (int32_t)getpid();
	int k, slot = -1;

	if (!m)
		return;
	for (k = 0; k < LGPU_MAX_HOLDERS; k++) {
		if (m->holders[k].pid == my_pid) {
			slot = k;
			break;
		}
	}
	if (slot < 0 && (d_refs > 0 || d_exp > 0)) {
		for (k = 0; k < LGPU_MAX_HOLDERS; k++) {
			if (__sync_bool_compare_and_swap(&m->holders[k].pid, 0,
							 my_pid)) {
				slot = k;
				break;
			}
		}
	}
	if (slot >= 0) {
		if (d_refs != 0)
			__sync_add_and_fetch(&m->holders[slot].refs, d_refs);
		if (d_exp != 0)
			__sync_add_and_fetch(&m->holders[slot].exported, d_exp);
	}
}

static void lgpu_holder_consume_export(struct lgpu_shared_meta *m)
{
	int k;

	if (!m)
		return;
	for (k = 0; k < LGPU_MAX_HOLDERS; k++) {
		int32_t cur = m->holders[k].exported;

		while (cur > 0) {
			if (__sync_bool_compare_and_swap(&m->holders[k].exported,
							 cur, cur - 1))
				return;
			cur = m->holders[k].exported;
		}
	}
}

static void lgpu_reap_dead_holders(struct lgpu_shared_meta *m)
{
	int32_t my_pid = (int32_t)getpid();
	int k;

	if (!m)
		return;
	for (k = 0; k < LGPU_MAX_HOLDERS; k++) {
		int32_t hpid = m->holders[k].pid;
		int32_t hrefs = m->holders[k].refs;
		int32_t hexp = m->holders[k].exported;

		if (hpid <= 0 || hpid == my_pid || (hrefs <= 0 && hexp <= 0))
			continue;
		if (kill((pid_t)hpid, 0) != 0 && errno == ESRCH) {
			int32_t leaked_refs =
				__sync_fetch_and_and(&m->holders[k].refs, 0);
			int32_t leaked_exp =
				__sync_fetch_and_and(&m->holders[k].exported, 0);

			if (leaked_refs > 0)
				__sync_sub_and_fetch(&m->ref_handles,
						     leaked_refs);
			if (leaked_exp > 0)
				__sync_sub_and_fetch(&m->exported_fds,
						     leaked_exp);
			__sync_bool_compare_and_swap(&m->holders[k].pid, hpid,
						     0);
		}
	}
}

static void lgpu_reap_deferred_locked(void)
{
	int i;

	for (i = 0; i < LGPU_MAX_DEFERRED; i++) {
		struct lgpu_shared_meta *m = lgpu_deferred[i].meta;

		if (!m)
			continue;
		__sync_synchronize();
		lgpu_reap_dead_holders(m);
		if (m->ref_handles <= 0 && m->exported_fds <= 0) {
			if (m->gpu_offset)
				rdn_target_vram_free(m->gpu_offset);
			munmap(m, sizeof(*m));
			close(lgpu_deferred[i].shm_fd);
			lgpu_deferred[i].meta = NULL;
			lgpu_deferred[i].shm_fd = -1;
		}
	}
}

static struct context *lgpu_context_for(void *cgl_ctx)
{
	void *rend;
	int i;

	if (!cgl_ctx)
		return NULL;
	rdn_mesa_ensure_current(cgl_ctx);
	rend = *(void **)cgl_ctx;
	if (!rend)
		return NULL;
	for (i = 0; i < MAX_CONTEXTS; i++) {
		if (contexts[i].gld_ctx &&
		    (contexts[i].rend == rend || contexts[i].early_rend == rend))
			return &contexts[i];
	}
	return NULL;
}

uint32_t LeopardGPU_SharedBufferVersion(void *cgl_ctx)
{
	if (!mesa_ready())
		return 0;
	if (cgl_ctx && !lgpu_context_for(cgl_ctx))
		return 0;
	return 1;
}

int LeopardGPU_BufferCreate(void *cgl_ctx, uint32_t width, uint32_t height,
			    uint32_t fourcc, uint64_t modifier_hint,
			    uint32_t *buffer_id)
{
	struct context *c;
	uint32_t stride, rows, alloc_bytes, gpu_offset = 0, id;
	uint64_t total_bytes;
	char tmpl[] = "/tmp/lgpu.XXXXXX";
	int fd, slot = -1, i;
	struct lgpu_shared_meta *meta;
	uint8_t *gpu;

	if (!cgl_ctx || !buffer_id || width == 0 || height == 0 ||
	    width > 16384 || height > 16384)
		return -1;
	if (fourcc != LEOPARD_FOURCC_ARGB8888 &&
	    fourcc != LEOPARD_FOURCC_XRGB8888)
		return -1;
	if (modifier_hint != LEOPARD_MODIFIER_LINEAR &&
	    modifier_hint != LEOPARD_MODIFIER_INVALID)
		return -1;
	c = lgpu_context_for(cgl_ctx);
	if (!c)
		return -1;
	if (!c->mesa && !mesa_for(c))
		return -1;

	/* 64-pixel pitch/height alignment matches linear CB/DB & texture rules. */
	stride = ((width + 63u) & ~63u) * 4u;
	rows = (height + 63u) & ~63u;
	total_bytes = (uint64_t)stride * (uint64_t)rows;
	if (total_bytes == 0 || total_bytes > 0x0ffff000ULL)
		return -1;
	alloc_bytes = ((uint32_t)total_bytes + 4095u) & ~4095u;

	pthread_mutex_lock(&lgpu_lock);
	lgpu_reap_deferred_locked();
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (!lgpu_buffers[i].meta) {
			slot = i;
			break;
		}
	}
	pthread_mutex_unlock(&lgpu_lock);
	if (slot < 0)
		return -1;

	if (!rdn_target_vram_alloc(alloc_bytes, &gpu_offset) || !gpu_offset)
		return -1;

	gpu = rdn_target_vram_map(gpu_offset);
	if (gpu) {
		uint32_t y;

		for (y = 0; y < height; y++)
			memset(gpu + (size_t)y * stride, 0, (size_t)width * 4u);
		__sync_synchronize();
	}

	fd = mkstemp(tmpl);
	if (fd < 0) {
		rdn_target_vram_free(gpu_offset);
		return -1;
	}
	unlink(tmpl);
	if (ftruncate(fd, 4096) != 0) {
		close(fd);
		rdn_target_vram_free(gpu_offset);
		return -1;
	}
	meta = (struct lgpu_shared_meta *)mmap(NULL, sizeof(*meta),
					       PROT_READ | PROT_WRITE,
					       MAP_SHARED, fd, 0);
	if (meta == MAP_FAILED) {
		close(fd);
		rdn_target_vram_free(gpu_offset);
		return -1;
	}

	memset(meta, 0, sizeof(*meta));
	meta->magic = LGPU_META_MAGIC;
	meta->version = LGPU_META_VERSION;
	meta->gpu_offset = gpu_offset;
	meta->size = alloc_bytes;
	meta->width = width;
	meta->height = height;
	meta->fourcc = fourcc;
	meta->offset = 0;
	meta->stride = stride;
	meta->modifier = LEOPARD_MODIFIER_LINEAR;
	meta->ref_handles = 1;
	meta->exported_fds = 0;
	meta->holders[0].pid = (int32_t)getpid();
	meta->holders[0].refs = 1;
	__sync_synchronize();

	pthread_mutex_lock(&lgpu_lock);
	slot = -1;
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (!lgpu_buffers[i].meta) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&lgpu_lock);
		munmap(meta, sizeof(*meta));
		close(fd);
		rdn_target_vram_free(gpu_offset);
		return -1;
	}
	id = lgpu_next_id++;
	if (id == 0)
		id = lgpu_next_id++;
	lgpu_buffers[slot].id = id;
	lgpu_buffers[slot].shm_fd = fd;
	lgpu_buffers[slot].owner = 1;
	lgpu_buffers[slot].meta = meta;
	c->has_shared_buffer = 1;
	pthread_mutex_unlock(&lgpu_lock);

	*buffer_id = id;
	return 0;
}

int LeopardGPU_BufferBindTexture(void *cgl_ctx, uint32_t buffer_id,
				 unsigned int texture)
{
	struct context *c;
	struct lgpu_shared_meta *meta = NULL;
	uint32_t gpu_offset, stride, width, height, fourcc;
	GLuint flags;
	void (*bind_tex)(GLenum, GLuint);
	void (*get_tex_param)(GLenum, GLenum, GLint *);
	void (*tex_param_i)(GLenum, GLenum, GLint);
	int i;

	if (!cgl_ctx || !buffer_id || !texture)
		return -1;
	c = lgpu_context_for(cgl_ctx);
	if (!c)
		return -1;

	pthread_mutex_lock(&lgpu_lock);
	lgpu_reap_deferred_locked();
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (lgpu_buffers[i].meta && lgpu_buffers[i].id == buffer_id) {
			meta = lgpu_buffers[i].meta;
			gpu_offset = meta->gpu_offset;
			stride = meta->stride;
			width = meta->width;
			height = meta->height;
			fourcc = meta->fourcc;
			break;
		}
	}
	pthread_mutex_unlock(&lgpu_lock);
	if (!meta)
		return -1;

	if (c->rend)
		rdn_make_current(c->rend);
	if (!c->mesa || OSMesaGetCurrentContext() != c->mesa)
		return -1;

	bind_tex = (void (*)(GLenum, GLuint))OSMesaGetProcAddress("glBindTexture");
	if (!bind_tex)
		return -1;
	bind_tex(GL_TEXTURE_2D, texture);
	if (c->rend)
		rdn_client_tex_drop_bound(c->rend, GL_TEXTURE_2D);

	flags = (fourcc == LEOPARD_FOURCC_ARGB8888) ? OSMESA_STORE_ALPHA : 0;
	if (!OSMesaTexStoreImage(c->mesa, GL_TEXTURE_2D, RDN_TARGET_VRAM_HANDLE,
				 (GLsizei)stride, gpu_offset,
				 (GLsizei)width, (GLsizei)height, flags))
		return -1;

	get_tex_param = (void (*)(GLenum, GLenum, GLint *))
		OSMesaGetProcAddress("glGetTexParameteriv");
	tex_param_i = (void (*)(GLenum, GLenum, GLint))
		OSMesaGetProcAddress("glTexParameteri");
	if (get_tex_param && tex_param_i) {
		GLint min_filter = 0;

		get_tex_param(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
		if (min_filter == GL_NEAREST_MIPMAP_LINEAR ||
		    min_filter == GL_LINEAR_MIPMAP_LINEAR ||
		    min_filter == GL_NEAREST_MIPMAP_NEAREST ||
		    min_filter == GL_LINEAR_MIPMAP_NEAREST) {
			tex_param_i(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			tex_param_i(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			tex_param_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			tex_param_i(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
	}

	c->has_shared_buffer = 1;
	return 0;
}

int LeopardGPU_BufferExport(void *cgl_ctx, uint32_t buffer_id, int *fd,
			    uint32_t *fourcc, uint32_t *offset,
			    uint32_t *stride, uint64_t *modifier)
{
	struct context *c;
	struct lgpu_shared_meta *meta = NULL;
	int shm_fd = -1, dup_fd, i;

	if (!cgl_ctx || !buffer_id || !fd || !fourcc || !offset ||
	    !stride || !modifier)
		return -1;
	c = lgpu_context_for(cgl_ctx);
	if (!c)
		return -1;

	pthread_mutex_lock(&lgpu_lock);
	lgpu_reap_deferred_locked();
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (lgpu_buffers[i].meta && lgpu_buffers[i].id == buffer_id) {
			meta = lgpu_buffers[i].meta;
			shm_fd = lgpu_buffers[i].shm_fd;
			break;
		}
	}
	if (!meta || shm_fd < 0) {
		pthread_mutex_unlock(&lgpu_lock);
		return -1;
	}
	dup_fd = dup(shm_fd);
	if (dup_fd < 0) {
		pthread_mutex_unlock(&lgpu_lock);
		return -1;
	}
	__sync_fetch_and_add(&meta->exported_fds, 1);
	lgpu_holder_adjust(meta, 0, 1);
	*fd = dup_fd;
	*fourcc = meta->fourcc;
	*offset = meta->offset;
	*stride = meta->stride;
	*modifier = meta->modifier;
	pthread_mutex_unlock(&lgpu_lock);

	if (c->mesa)
		OSMesaFlushRender(c->mesa, GL_FALSE);
	return 0;
}

int LeopardGPU_BufferImport(void *cgl_ctx, int fd, uint32_t width,
			    uint32_t height, uint32_t fourcc, uint32_t offset,
			    uint32_t stride, uint64_t modifier,
			    uint32_t *buffer_id)
{
	struct context *c;
	struct stat st;
	struct lgpu_shared_meta *meta;
	int dup_fd, slot = -1, i;
	uint32_t id;

	if (!cgl_ctx || fd < 0 || !buffer_id || width == 0 || height == 0)
		return -1;
	if (fourcc != LEOPARD_FOURCC_ARGB8888 &&
	    fourcc != LEOPARD_FOURCC_XRGB8888)
		return -1;
	if (modifier != LEOPARD_MODIFIER_LINEAR)
		return -1;
	c = lgpu_context_for(cgl_ctx);
	if (!c)
		return -1;
	if (!c->mesa && !mesa_for(c))
		return -1;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(*meta))
		return -1;

	dup_fd = dup(fd);
	if (dup_fd < 0)
		return -1;
	meta = (struct lgpu_shared_meta *)mmap(NULL, sizeof(*meta),
					       PROT_READ | PROT_WRITE,
					       MAP_SHARED, dup_fd, 0);
	if (meta == MAP_FAILED) {
		close(dup_fd);
		return -1;
	}
	__sync_synchronize();
	if (meta->magic != LGPU_META_MAGIC ||
	    meta->version != LGPU_META_VERSION ||
	    meta->width != width || meta->height != height ||
	    meta->fourcc != fourcc || meta->offset != offset ||
	    meta->stride != stride || meta->modifier != modifier ||
	    !meta->gpu_offset) {
		munmap(meta, sizeof(*meta));
		close(dup_fd);
		return -1;
	}

	pthread_mutex_lock(&lgpu_lock);
	lgpu_reap_deferred_locked();
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (!lgpu_buffers[i].meta) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		pthread_mutex_unlock(&lgpu_lock);
		munmap(meta, sizeof(*meta));
		close(dup_fd);
		return -1;
	}
	__sync_fetch_and_add(&meta->ref_handles, 1);
	lgpu_holder_adjust(meta, 1, 0);
	if (meta->exported_fds > 0) {
		__sync_fetch_and_sub(&meta->exported_fds, 1);
		lgpu_holder_consume_export(meta);
	}

	id = lgpu_next_id++;
	if (id == 0)
		id = lgpu_next_id++;
	lgpu_buffers[slot].id = id;
	lgpu_buffers[slot].shm_fd = dup_fd;
	lgpu_buffers[slot].owner = 0;
	lgpu_buffers[slot].meta = meta;
	c->has_shared_buffer = 1;
	pthread_mutex_unlock(&lgpu_lock);

	*buffer_id = id;
	return 0;
}

int LeopardGPU_BufferRelease(void *cgl_ctx, uint32_t buffer_id)
{
	struct lgpu_shared_meta *meta = NULL;
	int shm_fd = -1, owner = 0, i, j;
	int32_t remaining;

	if (!cgl_ctx || !buffer_id)
		return -1;
	if (!lgpu_context_for(cgl_ctx))
		return -1;

	pthread_mutex_lock(&lgpu_lock);
	for (i = 0; i < LGPU_MAX_BUFFERS; i++) {
		if (lgpu_buffers[i].meta && lgpu_buffers[i].id == buffer_id) {
			meta = lgpu_buffers[i].meta;
			shm_fd = lgpu_buffers[i].shm_fd;
			owner = lgpu_buffers[i].owner;
			lgpu_buffers[i].meta = NULL;
			lgpu_buffers[i].shm_fd = -1;
			lgpu_buffers[i].id = 0;
			lgpu_buffers[i].owner = 0;
			break;
		}
	}
	if (!meta) {
		pthread_mutex_unlock(&lgpu_lock);
		return -1;
	}
	if (!owner && meta->gpu_offset)
		rdn_target_vram_free(meta->gpu_offset);
	lgpu_holder_adjust(meta, -1, 0);
	remaining = __sync_sub_and_fetch(&meta->ref_handles, 1);
	lgpu_reap_dead_holders(meta);
	remaining = meta->ref_handles;
	if (owner) {
		if (remaining <= 0 && meta->exported_fds <= 0) {
			if (meta->gpu_offset)
				rdn_target_vram_free(meta->gpu_offset);
			munmap(meta, sizeof(*meta));
			close(shm_fd);
		} else {
			/*
			 * Defer kernel store destruction until all imported
			 * handles and in-flight exported FDs are done.
			 */
			int deferred = 0;

			for (j = 0; j < LGPU_MAX_DEFERRED; j++) {
				if (!lgpu_deferred[j].meta) {
					lgpu_deferred[j].meta = meta;
					lgpu_deferred[j].shm_fd = shm_fd;
					deferred = 1;
					break;
				}
			}
			if (!deferred) {
				munmap(meta, sizeof(*meta));
				close(shm_fd);
			}
		}
	} else {
		munmap(meta, sizeof(*meta));
		close(shm_fd);
	}
	lgpu_reap_deferred_locked();
	pthread_mutex_unlock(&lgpu_lock);
	return 0;
}
