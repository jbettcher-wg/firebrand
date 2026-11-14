/*
 * The boot splash: the splash grey (0xBFBFBF, RadeonNI's) with the Firebrand
 * logo in the middle, as RadeonNI's rdn_splash_draw shows the Freescale one
 * (RadeonNI's hw/rdn_pattern.c). The logo is shared/frb_splash_logo.h,
 * made by tools/mk-splash-logo.py from the C-chip SVG in two sizes.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */

#ifndef FRB_SPLASH_H
#define FRB_SPLASH_H

#include <stdint.h>

struct frb_splash_logo {
	uint32_t width, height, colours;
	const uint32_t *palette;	/* 0xRRGGBB; entry 0 is the grey */
	const uint8_t *rle;		/* (count, palette index) pairs, row-major */
	uint32_t rle_len;
};

/*
 * The logo and its pixel multiplier for a width x height screen: 640 px wide
 * up to 3839 px screens, the 1280 px one from 3840 (4K), doubled again from
 * 7680; never taller than half the screen.
 */
const struct frb_splash_logo *frb_splash_pick(uint32_t width, uint32_t height, uint32_t *scale);

/*
 * Draw it into a linear surface, pitch in pixels, in the CPU's order (the
 * scanout swaps on a big-endian host): 32 bpp xRGB8888, 16 bpp xRGB1555,
 * 8 bpp the logo's palette indices (frb_splash_pick's palette goes in the
 * colour table; index 0 is the grey). Returns the multiplier drawn at.
 */
uint32_t frb_splash_draw(volatile void *base, uint32_t pitch_pixels, uint32_t width, uint32_t height,
			 uint32_t bpp);

#endif
