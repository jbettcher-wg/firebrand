/*
 * What Mac OS X 10.4's C library lacks and current Mesa uses.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include "tiger_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>

int clock_gettime(clockid_t clock, struct timespec *ts)
{
	switch (clock) {
	case CLOCK_REALTIME: {
		struct timeval tv;

		gettimeofday(&tv, NULL);
		ts->tv_sec = tv.tv_sec;
		ts->tv_nsec = tv.tv_usec * 1000;
		return 0;
	}
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW: {
		static mach_timebase_info_data_t tb;
		uint64_t t = mach_absolute_time(), ns;

		if (!tb.denom)
			mach_timebase_info(&tb);
		/* Split to keep the multiplication inside 64 bits. */
		ns = (t / tb.denom) * tb.numer + (t % tb.denom) * tb.numer / tb.denom;
		ts->tv_sec = ns / 1000000000ull;
		ts->tv_nsec = ns % 1000000000ull;
		return 0;
	}
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID: {
		/* Tiger has no per-thread figure here; the process's will do. */
		struct rusage ru;

		getrusage(RUSAGE_SELF, &ru);
		ts->tv_sec = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec;
		ts->tv_nsec = (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000;
		if (ts->tv_nsec >= 1000000000) {
			ts->tv_sec++;
			ts->tv_nsec -= 1000000000;
		}
		return 0;
	}
	}
	errno = EINVAL;
	return -1;
}

int clock_getres(clockid_t clock, struct timespec *ts)
{
	(void)clock;
	ts->tv_sec = 0;
	ts->tv_nsec = 1000;
	return 0;
}

/*
 * The block is found again by free() because the pointer handed out is
 * what malloc() returned. Tiger's and Leopard's malloc align to 16 bytes;
 * a block of more than 1008 bytes comes from the allocator's "small"
 * regions, which are cut in units of 512 bytes, or (15 KB and more)
 * straight from the kernel in pages, so asking for at least 1024 bytes gets
 * a block aligned to 512. That is checked, and what is not aligned as
 * asked (another allocator in the program's zone), or wants more than 512,
 * goes to valloc(), which aligns to a page. More than a page is not
 * supported.
 *
 * Everything between 16 and a page used to go to valloc(): a page from the
 * kernel and back for every block. Mesa allocates each buffer and texture
 * object aligned to a cache line, and a program that makes and drops
 * buffers all the time (Doom 3's vertex caches) spent 3 % of its three GL
 * threads in mmap and munmap (T4240, 2026-11-15).
 */
#define SMALL_REGION_LEAST 1024
#define SMALL_REGION_ALIGN 512

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	void *p;

	if (alignment < sizeof(void *) || (alignment & (alignment - 1)))
		return EINVAL;
	if (alignment <= 16) {
		p = malloc(size ? size : 1);
	} else if (alignment <= 4096) {
		p = NULL;
		if (alignment <= SMALL_REGION_ALIGN) {
			p = malloc(size < SMALL_REGION_LEAST ? SMALL_REGION_LEAST : size);
			if (p && ((uintptr_t)p & (alignment - 1))) {
				free(p);
				p = NULL;
			}
		}
		if (!p)
			p = valloc(size ? size : 1);
	} else {
		return EINVAL;
	}
	if (!p)
		return ENOMEM;
	*memptr = p;
	return 0;
}

size_t strnlen(const char *s, size_t maxlen)
{
	const char *end = memchr(s, 0, maxlen);

	return end ? (size_t)(end - s) : maxlen;
}

char *strndup(const char *s, size_t n)
{
	size_t len = strnlen(s, n);
	char *copy = malloc(len + 1);

	if (copy) {
		memcpy(copy, s, len);
		copy[len] = 0;
	}
	return copy;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
	size_t len = 0;
	int c;

	if (!lineptr || !n) {
		errno = EINVAL;
		return -1;
	}
	while ((c = getc(stream)) != EOF) {
		if (len + 2 > *n) {
			size_t size = *n ? *n * 2 : 128;
			char *p = realloc(*lineptr, size);

			if (!p)
				return -1;
			*lineptr = p;
			*n = size;
		}
		(*lineptr)[len++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!len)
		return -1;
	(*lineptr)[len] = 0;
	return (ssize_t)len;
}

/* open_memstream() on top of funopen(), which Tiger does have. */
struct memstream {
	char **ptr;
	size_t *size;
	size_t capacity;
};

static int memstream_write(void *cookie, const char *data, int len)
{
	struct memstream *m = cookie;
	size_t need = *m->size + (size_t)len + 1;

	if (need > m->capacity) {
		size_t capacity = m->capacity ? m->capacity : 128;
		char *p;

		while (capacity < need)
			capacity *= 2;
		p = realloc(*m->ptr, capacity);
		if (!p) {
			errno = ENOMEM;
			return -1;
		}
		*m->ptr = p;
		m->capacity = capacity;
	}
	memcpy(*m->ptr + *m->size, data, (size_t)len);
	*m->size += (size_t)len;
	(*m->ptr)[*m->size] = 0;
	return len;
}

static int memstream_close(void *cookie)
{
	free(cookie);
	return 0;
}

FILE *open_memstream(char **ptr, size_t *size)
{
	struct memstream *m = calloc(1, sizeof(*m));
	FILE *f;

	if (!m)
		return NULL;
	*ptr = calloc(1, 1);
	*size = 0;
	m->ptr = ptr;
	m->size = size;
	m->capacity = 1;
	f = funopen(m, NULL, memstream_write, NULL, memstream_close);
	if (!f) {
		free(*ptr);
		free(m);
	}
	return f;
}

#undef sysconf

long tiger_sysconf(int name)
{
	if (name == _SC_PHYS_PAGES) {
		int mib[2] = { CTL_HW, HW_MEMSIZE };
		uint64_t bytes = 0;
		size_t len = sizeof(bytes);

		if (sysctl(mib, 2, &bytes, &len, NULL, 0) || !bytes)
			return -1;
		return (long)(bytes / (uint64_t)getpagesize());
	}
	return sysconf(name);
}

int pthread_setname_np(const char *name)
{
	(void)name;
	return 0;
}

int fstatat(int fd, const char *path, struct stat *buf, int flag)
{
	char dir[PATH_MAX], full[PATH_MAX];
	int n;

	if (!path || !buf || (flag & ~AT_SYMLINK_NOFOLLOW)) {
		errno = EINVAL;
		return -1;
	}
	if (path[0] == '/' || fd == AT_FDCWD)
		return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(path, buf) : stat(path, buf);
	if (fcntl(fd, F_GETPATH, dir) == -1)
		return -1;
	n = snprintf(full, sizeof(full), "%s/%s", dir, path);
	if (n < 0 || (size_t)n >= sizeof(full)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return (flag & AT_SYMLINK_NOFOLLOW) ? lstat(full, buf) : stat(full, buf);
}
