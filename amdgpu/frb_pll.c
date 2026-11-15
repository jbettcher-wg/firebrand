/*
 * amdgpu_pll.c (Linux 7.0.2): the pixel PLL divider search, and
 * amdgpu_atombios_get_clock_info's pixel PLL limits. SetPixelClock 1.5 and
 * 1.6 (DCE 5/6/8) take the reference, feedback and post dividers from the
 * driver; 1.7 (DCE 11) works them out itself.
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stddef.h>
#include "frb_display.h"
#include "../shared/atom/atombios.h"

#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, d)	(((x) + ((d) / 2)) / (d))

static unsigned int gcd_u(unsigned int a, unsigned int b)
{
	while (b) {
		unsigned int t = a % b;

		a = b;
		b = t;
	}
	return a;
}

static unsigned int min_u(unsigned int a, unsigned int b) { return a < b ? a : b; }
static unsigned int max_u(unsigned int a, unsigned int b) { return a > b ? a : b; }
static unsigned int clamp_u(unsigned int v, unsigned int lo, unsigned int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * amdgpu_atombios_get_clock_info, the pixel PLLs: FirmwareInfo's reference
 * clock and PLL ranges, the same for PPLL1, PPLL2 and PPLL0.
 */
int frb_atombios_get_ppll_info(struct frb_device *adev, struct frb_display *d)
{
	const uint8_t *bios = adev->bios;
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	struct frb_pll *ppll = &d->ppll[0];
	uint8_t frev, crev;
	uint16_t off;
	int i;

	if (!frb_atom_parse_data_header(adev->atom, index, NULL, &frev, &crev, &off))
		return -EINVAL;
	memset(ppll, 0, sizeof(*ppll));
	/* pixel clocks */
	ppll->reference_freq = frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO, usReferenceClock));
	ppll->reference_div = 0;
	ppll->pll_out_min = frb_get_le32(bios, off + offsetof(ATOM_FIRMWARE_INFO_V1_2, ulMinPixelClockPLL_Output));
	ppll->pll_out_max = frb_get_le32(bios, off + offsetof(ATOM_FIRMWARE_INFO, ulMaxPixelClockPLL_Output));
	ppll->lcd_pll_out_min =
		frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO_V1_4, usLcdMinPixelClockPLL_Output)) * 100;
	if (ppll->lcd_pll_out_min == 0)
		ppll->lcd_pll_out_min = ppll->pll_out_min;
	ppll->lcd_pll_out_max =
		frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO_V1_4, usLcdMaxPixelClockPLL_Output)) * 100;
	if (ppll->lcd_pll_out_max == 0)
		ppll->lcd_pll_out_max = ppll->pll_out_max;
	if (ppll->pll_out_min == 0)
		ppll->pll_out_min = 64800;
	ppll->pll_in_min = frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO, usMinPixelClockPLL_Input));
	ppll->pll_in_max = frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO, usMaxPixelClockPLL_Input));
	ppll->min_post_div = 2;
	ppll->max_post_div = 0x7f;
	ppll->min_frac_feedback_div = 0;
	ppll->max_frac_feedback_div = 9;
	ppll->min_ref_div = 2;
	ppll->max_ref_div = 0x3ff;
	ppll->min_feedback_div = 4;
	ppll->max_feedback_div = 0xfff;
	for (i = 1; i < 3; i++)
		d->ppll[i] = *ppll;
	frb_info(adev, "display: pixel PLLs: reference %u kHz, output %u..%u kHz, input %u..%u kHz (FirmwareInfo %u.%u)",
		 ppll->reference_freq * 10, ppll->pll_out_min * 10, ppll->pll_out_max * 10,
		 ppll->pll_in_min * 10, ppll->pll_in_max * 10, frev, crev);
	return 0;
}

/* amdgpu_pll_reduce_ratio */
static void amdgpu_pll_reduce_ratio(unsigned *nom, unsigned *den, unsigned nom_min, unsigned den_min)
{
	unsigned tmp;

	/* reduce the numbers to a simpler ratio */
	tmp = gcd_u(*nom, *den);
	*nom /= tmp;
	*den /= tmp;

	/* make sure nominator is large enough */
	if (*nom < nom_min) {
		tmp = DIV_ROUND_UP(nom_min, *nom);
		*nom *= tmp;
		*den *= tmp;
	}

	/* make sure the denominator is large enough */
	if (*den < den_min) {
		tmp = DIV_ROUND_UP(den_min, *den);
		*nom *= tmp;
		*den *= tmp;
	}
}

/* amdgpu_pll_get_fb_ref_div */
static void amdgpu_pll_get_fb_ref_div(struct frb_device *adev, unsigned int nom, unsigned int den,
				      unsigned int post_div, unsigned int fb_div_max, unsigned int ref_div_max,
				      unsigned int *fb_div, unsigned int *ref_div)
{
	/* limit reference * post divider to a maximum */
	if (frb_funcs(adev)->family == AMDGPU_FAMILY_SI)
		ref_div_max = min_u(100 / post_div, ref_div_max);
	else
		ref_div_max = min_u(128 / post_div, ref_div_max);

	/* get matching reference and feedback divider */
	*ref_div = clamp_u(DIV_ROUND_CLOSEST(den, post_div), 1u, ref_div_max);
	*fb_div = DIV_ROUND_CLOSEST(nom * *ref_div * post_div, den);

	/* limit fb divider to its maximum */
	if (*fb_div > fb_div_max) {
		*ref_div = DIV_ROUND_CLOSEST(*ref_div * fb_div_max, *fb_div);
		*fb_div = fb_div_max;
	}
}

/* amdgpu_pll_compute: freq in kHz (10 kHz without USE_FRAC_FB_DIV, as amdgpu) */
void frb_pll_compute(struct frb_device *adev, const struct frb_pll *pll, uint32_t freq, uint32_t *dot_clock_p,
		     uint32_t *fb_div_p, uint32_t *frac_fb_div_p, uint32_t *ref_div_p, uint32_t *post_div_p)
{
	unsigned target_clock = pll->flags & FRB_PLL_USE_FRAC_FB_DIV ? freq : freq / 10;
	unsigned fb_div_min, fb_div_max, fb_div;
	unsigned post_div_min, post_div_max, post_div;
	unsigned ref_div_min, ref_div_max, ref_div;
	unsigned post_div_best, diff_best;
	unsigned nom, den;

	/* determine allowed feedback divider range */
	fb_div_min = pll->min_feedback_div;
	fb_div_max = pll->max_feedback_div;

	if (pll->flags & FRB_PLL_USE_FRAC_FB_DIV) {
		fb_div_min *= 10;
		fb_div_max *= 10;
	}

	/* determine allowed ref divider range */
	if (pll->flags & FRB_PLL_USE_REF_DIV)
		ref_div_min = pll->reference_div;
	else
		ref_div_min = pll->min_ref_div;

	if (pll->flags & FRB_PLL_USE_FRAC_FB_DIV &&
	    pll->flags & FRB_PLL_USE_REF_DIV)
		ref_div_max = pll->reference_div;
	else
		ref_div_max = pll->max_ref_div;

	/* determine allowed post divider range */
	if (pll->flags & FRB_PLL_USE_POST_DIV) {
		post_div_min = pll->post_div;
		post_div_max = pll->post_div;
	} else {
		unsigned vco_min, vco_max;

		if (pll->flags & FRB_PLL_IS_LCD) {
			vco_min = pll->lcd_pll_out_min;
			vco_max = pll->lcd_pll_out_max;
		} else {
			vco_min = pll->pll_out_min;
			vco_max = pll->pll_out_max;
		}

		if (pll->flags & FRB_PLL_USE_FRAC_FB_DIV) {
			vco_min *= 10;
			vco_max *= 10;
		}

		post_div_min = vco_min / target_clock;
		if ((target_clock * post_div_min) < vco_min)
			++post_div_min;
		if (post_div_min < pll->min_post_div)
			post_div_min = pll->min_post_div;

		post_div_max = vco_max / target_clock;
		if ((target_clock * post_div_max) > vco_max)
			--post_div_max;
		if (post_div_max > pll->max_post_div)
			post_div_max = pll->max_post_div;
	}

	/* represent the searched ratio as fractional number */
	nom = target_clock;
	den = pll->reference_freq;

	/* reduce the numbers to a simpler ratio */
	amdgpu_pll_reduce_ratio(&nom, &den, fb_div_min, post_div_min);

	/* now search for a post divider */
	if (pll->flags & FRB_PLL_PREFER_MINM_OVER_MAXP)
		post_div_best = post_div_min;
	else
		post_div_best = post_div_max;
	diff_best = ~0u;

	for (post_div = post_div_min; post_div <= post_div_max; ++post_div) {
		unsigned diff, got;

		amdgpu_pll_get_fb_ref_div(adev, nom, den, post_div, fb_div_max,
					  ref_div_max, &fb_div, &ref_div);
		got = (pll->reference_freq * fb_div) / (ref_div * post_div);
		diff = target_clock > got ? target_clock - got : got - target_clock;

		if (diff < diff_best || (diff == diff_best &&
		    !(pll->flags & FRB_PLL_PREFER_MINM_OVER_MAXP))) {
			post_div_best = post_div;
			diff_best = diff;
		}
	}
	post_div = post_div_best;

	/* get the feedback and reference divider for the optimal value */
	amdgpu_pll_get_fb_ref_div(adev, nom, den, post_div, fb_div_max, ref_div_max,
				  &fb_div, &ref_div);

	/* reduce the numbers to a simpler ratio once more */
	/* this also makes sure that the reference divider is large enough */
	amdgpu_pll_reduce_ratio(&fb_div, &ref_div, fb_div_min, ref_div_min);

	/* avoid high jitter with small fractional dividers */
	if (pll->flags & FRB_PLL_USE_FRAC_FB_DIV && (fb_div % 10)) {
		fb_div_min = max_u(fb_div_min, (9 - (fb_div % 10)) * 20 + 60);
		if (fb_div < fb_div_min) {
			unsigned tmp = DIV_ROUND_UP(fb_div_min, fb_div);

			fb_div *= tmp;
			ref_div *= tmp;
		}
	}

	/* and finally save the result */
	if (pll->flags & FRB_PLL_USE_FRAC_FB_DIV) {
		*fb_div_p = fb_div / 10;
		*frac_fb_div_p = fb_div % 10;
	} else {
		*fb_div_p = fb_div;
		*frac_fb_div_p = 0;
	}

	*dot_clock_p = ((pll->reference_freq * *fb_div_p * 10) +
			(pll->reference_freq * *frac_fb_div_p)) /
		       (ref_div * post_div * 10);
	*ref_div_p = ref_div;
	*post_div_p = post_div;

	frb_info(adev, "display: pll %u kHz -> %u kHz, dividers fb %u.%u ref %u post %u (flags 0x%x)",
		 freq, *dot_clock_p * 10, *fb_div_p, *frac_fb_div_p, ref_div, post_div, pll->flags);
}
