/*
 * Thread-local storage for Mac OS X 10.4, which has none of its own.
 *
 * GCC then emulates `__thread`: every access calls
 * __emutls_get_address() with a small control object of the variable, and
 * libgcc's routine finds the thread's copy through pthread_getspecific().
 * Mesa looks up the current context and dispatch table that way on every
 * OpenGL call, twice with glthread, so in a game this routine runs a
 * million times a second.
 *
 * This replaces libgcc's routine with the same interface and a short cut
 * that takes no call at all (pthread_self() is three jumps away through
 * the C library, and asking it cost more than everything else here) and no
 * stack frame. A thread is recognised by where its stack is: every thread
 * that comes here gets a record with its stack's range and its table of
 * copies, and a small table (two records for each 64 KB step of the address
 * space, taken modulo 64 MB) says which threads' stacks were last seen at an
 * address. The routine looks at the two records for its stack pointer and
 * takes the one whose range the stack pointer is in. Any number of threads
 * have the short cut that way; until 2026-11-15 only the first four to come
 * here had it, which in a program with glthread are Mesa's own, and a
 * program that draws from a thread it made later (Doom 3 with a render
 * thread) took the long way at every OpenGL call.
 *
 * A thread whose stack pointer is found in neither record (first time at
 * that depth of its stack, pushed out by two other threads at the same
 * place in the table, or running on a stack that is not its own: fibres)
 * is found by pthread_getspecific() (emutls_mid), put in the table if it
 * is on its own stack, and is still right if it is not. Only a thread's
 * first use of a variable goes through emutls_slow, with a lock on the
 * first use of a variable by anyone.
 *
 * RDN_EMUTLS_STATS=1 in the environment prints, when the program ends, how
 * many calls took each of the steps after the first (the first is the
 * normal one and is not counted: counting would cost more than it tells).
 *
 * The control object's layout is libgcc's (emutls.c, struct
 * __emutls_object); the code is written for this project.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

struct emutls_object {
	uintptr_t size;
	uintptr_t align;
	union {
		uintptr_t offset;	/* 1-based slot; 0 until first use */
		void *ptr;
	} loc;
	void *templ;
};

/*
 * A thread: its stack and its copies, indexed by slot - 1.
 *
 * `low` and `len` are written once, before any other thread can see the
 * record, and never again: records are never freed, and one whose thread
 * has ended is taken again only by a thread with exactly the same stack
 * (the C library hands stacks out again). So whatever thread finds its
 * stack pointer in a record's range is the one thread living on that stack
 * now, and the rest of the record is that thread's alone to read and
 * write. A thread that ends leaves `count` at 0, so that nothing is found
 * through its record until the next owner has filled it.
 */
struct emutls_thread {
	uintptr_t low, len;		/* the stack: [low, low + len); len 0: not known */
	uintptr_t count;		/* of `slot` */
	void **slot;
	int alive;			/* under emutls_lock */
	struct emutls_thread *next;	/* all of them, under emutls_lock */
};

static pthread_mutex_t emutls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t emutls_once = PTHREAD_ONCE_INIT;
static pthread_key_t emutls_key;	/* the thread's struct emutls_thread */
static volatile int emutls_ready;	/* the key exists */
static uintptr_t emutls_slots;
static struct emutls_thread *emutls_threads;

/*
 * The short cut's table. A set is read by every thread and written by any
 * (one pointer at a time, each a whole record's address at all times); see
 * struct emutls_thread for why a reader needs no more than that.
 */
#define CACHE_SHIFT 16
#define CACHE_SETS 1024
static struct emutls_thread emutls_nobody;	/* len 0: no stack pointer is in it */
static struct emutls_thread *emutls_cache[CACHE_SETS][2] = {
	[0 ... CACHE_SETS - 1] = { &emutls_nobody, &emutls_nobody }
};

/* `t` is running with its stack pointer at `sp`, on its own stack: found there next time. */
static void emutls_cache_put(struct emutls_thread *t, uintptr_t sp)
{
	struct emutls_thread **set = emutls_cache[(sp >> CACHE_SHIFT) & (CACHE_SETS - 1)];

	if (set[0] == t)
		return;
	set[1] = set[0];
	set[0] = t;
}

/*
 * The program's GL thread, without a lookup (mesa/patches/0025).
 *
 * Mesa reads its current context and dispatch table on every call (twice
 * with glthread). Here the first thread that makes a context current in
 * Mesa (and whose stack is known) keeps copies of both,
 * with its stack's range. Mesa's reader (glapi.h) takes the copies when
 * the stack pointer is in the range, which only that thread's can be, and
 * the thread-local variable otherwise (the glthread worker, a second
 * context's thread, code on stacks of its own). The owner is the only
 * writer of ctx and disp, so the reader never sees a mix. Given up when
 * the owner makes no context current, or ends.
 *
 * On for The Sims 2, and for any program with RDN_TLS_FAST=1 (not the
 * window server); RDN_TLS_FAST=0 off.
 */
struct rdn_tls_primary {
	uintptr_t low, len;
	void *ctx, *disp;
};
struct rdn_tls_primary rdn_tls_primary __attribute__((visibility("hidden")));
static struct emutls_thread *primary_thread;	/* the owner, under emutls_lock */

static int primary_enabled(void)
{
	static int on = -1;

	if (on < 0) {
		const char *name = getprogname(), *env = getenv("RDN_TLS_FAST");

		on = name && strcmp(name, "WindowServer") &&
		     (env ? *env != '0' : !strcmp(name, "The Sims 2"));
	}
	return on;
}

static int in_primary(void)
{
	uintptr_t sp;

	__asm__("mr %0,r1" : "=r"(sp));
	return sp - rdn_tls_primary.low < rdn_tls_primary.len;
}

/* The owner's own context changed (Mesa: _mesa_glapi_set_context). */
void rdn_tls_set_context(void *ctx)
{
	if (in_primary()) {
		if (ctx)
			rdn_tls_primary.ctx = ctx;
		else {
			/* No context: given up, any thread may take it. */
			rdn_tls_primary.len = 0;
			__sync_synchronize();
			pthread_mutex_lock(&emutls_lock);
			primary_thread = NULL;
			pthread_mutex_unlock(&emutls_lock);
		}
	}
}

/* The same for the dispatch table (_mesa_glapi_set_dispatch). */
void rdn_tls_set_dispatch(void *disp)
{
	if (in_primary())
		rdn_tls_primary.disp = disp;
}

/*
 * A context has been made current in this thread (_mesa_make_current),
 * with the thread's variables `ctx` and `disp` as they are now. Takes the
 * place of owner if there is none and the thread has a short-cut slot.
 */
void rdn_tls_claim(void *ctx, void *disp)
{
	struct emutls_thread *t;

	if (!ctx || rdn_tls_primary.len || !primary_enabled() || !emutls_ready)
		return;
	t = pthread_getspecific(emutls_key);
	if (!t || !t->len)
		return;
	pthread_mutex_lock(&emutls_lock);
	if (!rdn_tls_primary.len) {
		primary_thread = t;
		rdn_tls_primary.ctx = ctx;
		rdn_tls_primary.disp = disp;
		rdn_tls_primary.low = t->low;
		__sync_synchronize();
		/* Last: from here on the thread's lookups use the copies. */
		rdn_tls_primary.len = t->len;
	}
	pthread_mutex_unlock(&emutls_lock);
}

/* The largest stack the kernel gives the first thread (MAXSSIZ). */
#define MAIN_STACK_MOST (64u << 20)

/*
 * Where the calling thread's stack is. For the first thread the C library
 * of 10.4 answers with the size other threads get by default; its real
 * limit is the process's, below which the kernel keeps the addresses free
 * for it. Nothing (a range nothing is in) if the answer does not hold the
 * stack pointer: then the thread is not on the stack it was made with.
 */
static void emutls_stack(uintptr_t *low, uintptr_t *high)
{
	pthread_t self = pthread_self();
	uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
	uintptr_t size = pthread_get_stacksize_np(self);
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
	struct rlimit limit;

	if (pthread_main_np() && !getrlimit(RLIMIT_STACK, &limit) &&
	    limit.rlim_cur > size && limit.rlim_cur <= MAIN_STACK_MOST)
		size = (uintptr_t)limit.rlim_cur;
	if (size <= top && sp >= top - size && sp < top) {
		*low = top - size;
		*high = top;
	} else {
		*low = ~(uintptr_t)0;
		*high = 0;
	}
}

static void emutls_thread_exit(void *arg)
{
	struct emutls_thread *t = arg;
	uintptr_t i, count = t->count;
	void **slot = t->slot;

	pthread_mutex_lock(&emutls_lock);
	if (primary_thread == t) {
		/* The owner ends: its stack may be another thread's next. */
		rdn_tls_primary.len = 0;
		__sync_synchronize();
		primary_thread = NULL;
	}
	/* Nothing is found through the record until its next owner fills it. */
	t->count = 0;
	t->slot = NULL;
	t->alive = 0;
	pthread_mutex_unlock(&emutls_lock);
	for (i = 0; i < count; i++)
		if (slot[i])
			/* The allocation starts one pointer before the copy. */
			free(((void **)slot[i])[-1]);
	free(slot);
}

static void emutls_init(void)
{
	pthread_key_create(&emutls_key, emutls_thread_exit);
	__sync_synchronize();
	emutls_ready = 1;
}

static void *emutls_new_copy(struct emutls_object *obj)
{
	uintptr_t align = obj->align < sizeof(void *) ? sizeof(void *) : obj->align;
	void *raw = malloc(obj->size + align + sizeof(void *));
	void *copy;

	if (!raw)
		abort();
	copy = (void *)(((uintptr_t)raw + sizeof(void *) + align - 1) & ~(align - 1));
	((void **)copy)[-1] = raw;
	if (obj->templ)
		memcpy(copy, obj->templ, obj->size);
	else
		memset(copy, 0, obj->size);
	return copy;
}

/* The calling thread's record: one left by a thread that had the same stack, or a new one. */
static struct emutls_thread *emutls_new_thread(void)
{
	struct emutls_thread *t;
	uintptr_t low, high, len;

	emutls_stack(&low, &high);
	len = high > low ? high - low : 0;
	if (!len)
		low = 0;
	pthread_mutex_lock(&emutls_lock);
	for (t = emutls_threads; t; t = t->next)
		if (!t->alive && t->low == low && t->len == len)
			break;
	if (t) {
		t->alive = 1;
		pthread_mutex_unlock(&emutls_lock);
		return t;
	}
	pthread_mutex_unlock(&emutls_lock);
	t = calloc(1, sizeof(*t));
	if (!t)
		abort();
	t->low = low;
	t->len = len;
	t->alive = 1;
	pthread_mutex_lock(&emutls_lock);
	t->next = emutls_threads;
	/* Whole before any other thread can come by it. */
	__sync_synchronize();
	emutls_threads = t;
	pthread_mutex_unlock(&emutls_lock);
	return t;
}

/* Calls that missed the short cut: found by thread identity, and the long way. */
static unsigned long emutls_mid_calls, emutls_slow_calls;

/* The long way: a thread's first use of a variable (and the first use of anything by a thread). */
static void *emutls_slow(struct emutls_object *obj, uintptr_t sp) __attribute__((noinline));

static void *emutls_slow(struct emutls_object *obj, uintptr_t sp)
{
	struct emutls_thread *t;
	uintptr_t offset;

	emutls_slow_calls++;
	pthread_once(&emutls_once, emutls_init);

	offset = obj->loc.offset;
	if (!offset) {
		pthread_mutex_lock(&emutls_lock);
		offset = obj->loc.offset;
		if (!offset)
			obj->loc.offset = offset = ++emutls_slots;
		pthread_mutex_unlock(&emutls_lock);
	}

	t = pthread_getspecific(emutls_key);
	if (!t) {
		t = emutls_new_thread();
		pthread_setspecific(emutls_key, t);
	}
	if (offset > t->count) {
		uintptr_t count = offset + 32;
		void **slot = realloc(t->slot, count * sizeof(*slot));

		if (!slot)
			abort();
		memset(slot + t->count, 0, (count - t->count) * sizeof(*slot));
		/* The table first: `count` says how much of it there is. */
		t->slot = slot;
		t->count = count;
	}
	if (!t->slot[offset - 1])
		t->slot[offset - 1] = emutls_new_copy(obj);
	if (sp - t->low < t->len)
		emutls_cache_put(t, sp);
	return t->slot[offset - 1];
}

/*
 * The second step: a thread whose stack pointer the table did not lead to
 * its record, found by who it is. On its own stack it is put in the table
 * for the next time; on another (fibres) it comes here every time.
 */
static void *emutls_mid(struct emutls_object *obj, uintptr_t sp) __attribute__((noinline));

static void *emutls_mid(struct emutls_object *obj, uintptr_t sp)
{
	uintptr_t index = obj->loc.offset - 1;
	struct emutls_thread *t;

	emutls_mid_calls++;
	if (emutls_ready && (t = pthread_getspecific(emutls_key)) != NULL) {
		if (sp - t->low < t->len)
			emutls_cache_put(t, sp);
		if (index < t->count) {
			void *copy = t->slot[index];

			if (copy)
				return copy;
		}
	}
	return emutls_slow(obj, sp);
}

static void emutls_report(void) __attribute__((destructor));

static void emutls_report(void)
{
	if (getenv("RDN_EMUTLS_STATS"))
		fprintf(stderr, "emutls: %lu calls by thread identity, %lu the long way\n",
			emutls_mid_calls, emutls_slow_calls);
}

void *__emutls_get_address(void *object)
{
	struct emutls_object *obj = object;
	/* No variable has slot 0, and no table that many entries. */
	uintptr_t index = obj->loc.offset - 1;
	/* Read as it is: asking the compiler for it costs a stack frame. */
	register uintptr_t sp __asm__("r1");
	struct emutls_thread *const *set = emutls_cache[(sp >> CACHE_SHIFT) & (CACHE_SETS - 1)];
	const struct emutls_thread *t = set[0];

	if (sp - t->low >= t->len) {
		t = set[1];
		if (sp - t->low >= t->len)
			return emutls_mid(obj, sp);
	}
	if (index < t->count) {
		void *copy = t->slot[index];

		if (copy)
			return copy;
	}
	return emutls_slow(obj, sp);
}

/* libgcc's other entry point: a common variable seen in several objects. */
void __emutls_register_common(void *object, uintptr_t size,
			      uintptr_t align, void *templ)
{
	struct emutls_object *obj = object;

	if (obj->size < size) {
		obj->size = size;
		obj->templ = NULL;
	}
	if (obj->align < align)
		obj->align = align;
	if (templ && size == obj->size)
		obj->templ = templ;
}
