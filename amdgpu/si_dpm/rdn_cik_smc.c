/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 * Copyright 2026 kouta-kun and Claude
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
 *
 * Authors: Alex Deucher (the Linux radeon driver's ci_smc.c, which this follows)
 */
#include "rdn_cik_smc.h"
#include "linux/cikd.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

#define HAWAII_SMC_UCODE_START	0x20000
#define HAWAII_SMC_UCODE_SIZE	0x1FDEC
#define SMC_RAM_END		0x40000
/* the second index/data pair, which the VBIOS's tables use (0x200/0x204 is pair 0) */
#define SMC_IND_INDEX_1		0x208
#define SMC_IND_DATA_1		0x20c

/* tn_smc_rreg()/wreg(): the SMC's address space through index/data pair 0 */
uint32_t rdn_cik_smc_rreg(struct rdn_card *card, uint32_t reg)
{
	rdn_wreg(card, SMC_IND_INDEX_0, reg);
	return rdn_rreg(card, SMC_IND_DATA_0);
}

void rdn_cik_smc_wreg(struct rdn_card *card, uint32_t reg, uint32_t v)
{
	rdn_wreg(card, SMC_IND_INDEX_0, reg);
	rdn_wreg(card, SMC_IND_DATA_0, v);
}

#define smc_rreg rdn_cik_smc_rreg
#define smc_wreg rdn_cik_smc_wreg

static uint32_t smc_rreg1(struct rdn_card *card, uint32_t reg)
{
	rdn_wreg(card, SMC_IND_INDEX_1, reg);
	return rdn_rreg(card, SMC_IND_DATA_1);
}

void rdn_cik_smc_probe(struct rdn_card *card, const char *when)
{
	rdn_log(card->os, RDN_LOG_INFO,
		"cik: smc %s: RESET_CNTL 0x%08x CLOCK_CNTL_0 0x%08x MISC_CNTL 0x%08x PC_C 0x%08x FIRMWARE_FLAGS 0x%08x; "
		"0xC0200000 0x%08x (pair 1 0x%08x), CG_SPLL_FUNC_CNTL 0x%08x, CG_SPLL_STATUS 0x%08x; ACCESS_CNTL 0x%08x",
		when,
		smc_rreg(card, SMC_SYSCON_RESET_CNTL), smc_rreg(card, SMC_SYSCON_CLOCK_CNTL_0),
		smc_rreg(card, SMC_SYSCON_MISC_CNTL), smc_rreg(card, SMC_PC_C),
		smc_rreg(card, FIRMWARE_FLAGS),
		smc_rreg(card, 0xC0200000), smc_rreg1(card, 0xC0200000),
		smc_rreg(card, 0xC0500140), smc_rreg(card, 0xC0500150),
		rdn_rreg(card, SMC_IND_ACCESS_CNTL));
}

void rdn_cik_smc_clock_on(struct rdn_card *card)
{
	uint32_t tmp = smc_rreg(card, SMC_SYSCON_CLOCK_CNTL_0);

	smc_wreg(card, SMC_SYSCON_CLOCK_CNTL_0, tmp & ~CK_DISABLE);
}

int rdn_cik_smc_load(struct rdn_card *card, const void *fw, size_t size)
{
	const uint8_t *src = fw;
	uint32_t n, tmp, start = HAWAII_SMC_UCODE_START, version = 0;

	/*
	 * Two shapes (ci_load_smc_ucode()): the raw 2013 file (HAWAII_smc.bin,
	 * 0x1FDEC bytes at 0x20000, version 15.04 in its own header once
	 * loaded) and the headered one Linux prefers when present
	 * (hawaii_smc.bin: struct smc_firmware_header_v1_0, little-endian:
	 * size, header size, versions, ucode_version at +16, ucode size at
	 * +20, array offset at +24, crc32 at +28, ucode_start_addr at +32;
	 * 15.15 on this
	 * host, 2026-11-03). Bonaire and Hawaii share the start and the size.
	 */
	if (fw && size > 0x100 && size != HAWAII_SMC_UCODE_SIZE) {
		uint32_t total = src[0] | (src[1] << 8) | (src[2] << 16) | ((uint32_t)src[3] << 24);
		uint32_t usize = src[20] | (src[21] << 8) | (src[22] << 16) | ((uint32_t)src[23] << 24);
		uint32_t off = src[24] | (src[25] << 8) | (src[26] << 16) | ((uint32_t)src[27] << 24);

		version = src[16] | (src[17] << 8) | (src[18] << 16) | ((uint32_t)src[19] << 24);
		start = src[32] | (src[33] << 8) | (src[34] << 16) | ((uint32_t)src[35] << 24);
		if (total != size || (usize & 3) || off + usize > size || usize != HAWAII_SMC_UCODE_SIZE) {
			rdn_log(card->os, RDN_LOG_ERROR, "cik: smc: headered firmware not understood (%u bytes, ucode %u at %u, start 0x%x)",
				(unsigned)size, (unsigned)usize, (unsigned)off, (unsigned)start);
			return -EINVAL;
		}
		src += off;
		size = usize;
	} else if (!fw || size != HAWAII_SMC_UCODE_SIZE) {
		rdn_log(card->os, RDN_LOG_ERROR, "cik: smc: firmware of %u bytes, want %u (HAWAII_smc.bin / BONAIRE_smc.bin) or a headered hawaii_smc.bin",
			(unsigned)size, (unsigned)HAWAII_SMC_UCODE_SIZE);
		return -EINVAL;
	}
	/* ci_stop_smc_clock(), ci_reset_smc(): hold it while loading (ci_upload_firmware()'s order) */
	smc_wreg(card, SMC_SYSCON_CLOCK_CNTL_0, smc_rreg(card, SMC_SYSCON_CLOCK_CNTL_0) | CK_DISABLE);
	smc_wreg(card, SMC_SYSCON_RESET_CNTL, smc_rreg(card, SMC_SYSCON_RESET_CNTL) | RST_REG);

	/* ci_load_smc_ucode(): the SMC address space is big-endian, auto-incrementing through pair 0 */
	rdn_wreg(card, SMC_IND_INDEX_0, start);
	tmp = rdn_rreg(card, SMC_IND_ACCESS_CNTL);
	rdn_wreg(card, SMC_IND_ACCESS_CNTL, tmp | AUTO_INCREMENT_IND_0);
	for (n = 0; n + 4 <= size; n += 4, src += 4)
		rdn_wreg(card, SMC_IND_DATA_0,
			 ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3]);
	rdn_wreg(card, SMC_IND_ACCESS_CNTL, rdn_rreg(card, SMC_IND_ACCESS_CNTL) & ~AUTO_INCREMENT_IND_0);
	rdn_log(card->os, RDN_LOG_INFO, "cik: smc: %u words loaded at 0x%x (ucode version 0x%08x from the file's header, 0 for the raw file), SMC held in reset",
		n / 4, start, version);
	return 0;
}

int rdn_cik_smc_run(struct rdn_card *card)
{
	static const uint8_t jump[4] = { 0xE0, 0x00, 0x80, 0x40 };
	uint32_t i;

	/* ci_program_jump_on_start(): the four bytes at 0 */
	smc_wreg(card, 0x0, ((uint32_t)jump[0] << 24) | ((uint32_t)jump[1] << 16) | ((uint32_t)jump[2] << 8) | jump[3]);

	/* ci_dpm_start_smc(): clock, then release the reset */
	smc_wreg(card, SMC_SYSCON_CLOCK_CNTL_0, smc_rreg(card, SMC_SYSCON_CLOCK_CNTL_0) & ~CK_DISABLE);
	smc_wreg(card, SMC_SYSCON_RESET_CNTL, smc_rreg(card, SMC_SYSCON_RESET_CNTL) & ~RST_REG);
	for (i = 0; i < 200000; i++) {
		if (smc_rreg(card, FIRMWARE_FLAGS) & INTERRUPTS_ENABLED)
			break;
		card->os->delay_us(card->os->cookie, 1);
	}
	rdn_log(card->os, i < 200000 ? RDN_LOG_INFO : RDN_LOG_ERROR,
		"cik: smc: firmware %s after %u us (FIRMWARE_FLAGS 0x%08x, PC_C 0x%08x)",
		i < 200000 ? "running" : "NOT running", i,
		smc_rreg(card, FIRMWARE_FLAGS), smc_rreg(card, SMC_PC_C));
	return i < 200000 ? 0 : -ETIMEDOUT;
}

int rdn_cik_smc_start(struct rdn_card *card, const void *fw, size_t size)
{
	int r = rdn_cik_smc_load(card, fw, size);

	if (r)
		return r;
	return rdn_cik_smc_run(card);
}
