/*
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
 */

/*
 * The Sea Islands SMU (ci_smc.c): what a card needs before its VBIOS's
 * ASIC_Init can run here. On a PC the VBIOS's x86 POST brings the SMU up
 * before ASIC_Init; without it the W8100's SetEngineClock polls SMC-space
 * registers that read zero and spins (2026-11-03).
 */
#ifndef RDN_CIK_SMC_H
#define RDN_CIK_SMC_H

#include "rdn_card.h"

/*
 * The SMC's own address space through index/data pair 0 (SMC_IND_INDEX_0 /
 * SMC_IND_DATA_0), Linux's RREG32_SMC / WREG32_SMC (tn_smc_rreg/wreg). Linux
 * holds a spinlock around the pair; this library is single-threaded under the
 * caller's lock, so there is none here.
 */
uint32_t rdn_cik_smc_rreg(struct rdn_card *card, uint32_t reg);
void rdn_cik_smc_wreg(struct rdn_card *card, uint32_t reg, uint32_t v);

/* Log the SMC's control registers, the firmware flags and the two words SetEngineClock polls. */
void rdn_cik_smc_probe(struct rdn_card *card, const char *when);

/* ci_start_smc_clock(): clear CK_DISABLE so the SMC-space registers answer. */
void rdn_cik_smc_clock_on(struct rdn_card *card);

/*
 * ci_load_smc_ucode() + ci_program_jump_on_start() + the clock + the reset
 * release, then wait for FIRMWARE_FLAGS to report the firmware running.
 * `fw` is HAWAII_smc.bin (raw, 0x1FDEC bytes, loaded at 0x20000). 0, or an errno.
 */
int rdn_cik_smc_start(struct rdn_card *card, const void *fw, size_t size);

/*
 * The two halves of rdn_cik_smc_start(), for ci_dpm_enable(), which fills the
 * SMU's SRAM tables between them while the SMC is held in reset:
 * rdn_cik_smc_load() is ci_stop_smc_clock() + ci_reset_smc() +
 * ci_load_smc_ucode(); rdn_cik_smc_run() is ci_dpm_start_smc():
 * ci_program_jump_on_start(), ci_start_smc_clock(), ci_start_smc() and the
 * wait for FIRMWARE_FLAGS.INTERRUPTS_ENABLED. Both return 0 or an errno.
 */
int rdn_cik_smc_load(struct rdn_card *card, const void *fw, size_t size);
int rdn_cik_smc_run(struct rdn_card *card);

#endif /* RDN_CIK_SMC_H */
