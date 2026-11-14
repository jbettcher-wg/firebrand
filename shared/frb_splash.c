/*
 * The boot splash (see frb_splash.h). RadeonNI's rdn_splash_draw, for three
 * depths and two logo sizes: fill the screen with the grey, then decode the
 * logo's runs and write every pixel that is not the grey, centred, each one
 * scale x scale.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */

#include "frb_splash.h"
#include "frb_splash_logo.h"

#define NLOGOS (sizeof(frb_splash_logos) / sizeof(frb_splash_logos[0]))

const struct frb_splash_logo *frb_splash_pick(uint32_t width, uint32_t height, uint32_t *scale)
{
	const struct frb_splash_logo *l = &frb_splash_logos[0];
	uint32_t want = width >= 1920 ? width / 1920 : 1, i, s;	/* 640 px per 1920 */

	/* the biggest logo that fits a whole number of times into the size wanted */
	for (i = 1; i < NLOGOS; i++)
		if (frb_splash_logos[i].width <= 640 * want)
			l = &frb_splash_logos[i];
	s = 640 * want / l->width;
	if (!s)
		s = 1;
	while (s > 1 && l->height * s > height / 2)
		s--;
	*scale = s;
	return l;
}

static uint32_t pixel(uint32_t rgb, uint32_t index, uint32_t bpp)
{
	if (bpp == 8)
		return index;
	if (bpp == 16)
		return ((rgb >> 9) & 0x7c00) | ((rgb >> 6) & 0x03e0) | ((rgb >> 3) & 0x001f);
	return rgb & 0xffffff;
}

static void put(volatile void *base, uint32_t pitch, uint32_t bpp, uint32_t x, uint32_t y, uint32_t v)
{
	uint32_t at = y * pitch + x;

	if (bpp == 32)
		((volatile uint32_t *)base)[at] = v;
	else if (bpp == 16)
		((volatile uint16_t *)base)[at] = (uint16_t)v;
	else
		((volatile uint8_t *)base)[at] = (uint8_t)v;
}

uint32_t frb_splash_draw(volatile void *base, uint32_t pitch_pixels, uint32_t width, uint32_t height,
			 uint32_t bpp)
{
	uint32_t s, i, x, y, pos = 0, grey = pixel(FRB_SPLASH_GREY, 0, bpp);
	const struct frb_splash_logo *l = frb_splash_pick(width, height, &s);
	int32_t ox = ((int32_t)width - (int32_t)(l->width * s)) / 2;	/* may be negative: clip */
	int32_t oy = ((int32_t)height - (int32_t)(l->height * s)) / 2;

	if (bpp != 8 && bpp != 16 && bpp != 32)
		return 0;
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			put(base, pitch_pixels, bpp, x, y, grey);
	for (i = 0; i + 1 < l->rle_len; i += 2) {
		uint32_t run = l->rle[i], index = l->rle[i + 1], v = pixel(l->palette[index], index, bpp);

		for (; run; run--, pos++) {
			uint32_t dx, dy;
			int32_t px, py;

			if (!index)
				continue;	/* the grey: already there */
			px = ox + (int32_t)((pos % l->width) * s);
			py = oy + (int32_t)((pos / l->width) * s);
			for (dy = 0; dy < s; dy++)
				for (dx = 0; dx < s; dx++)
					if (px + (int32_t)dx >= 0 && py + (int32_t)dy >= 0 &&
					    (uint32_t)px + dx < width && (uint32_t)py + dy < height)
						put(base, pitch_pixels, bpp, (uint32_t)px + dx, (uint32_t)py + dy, v);
		}
	}
	return s;
}
