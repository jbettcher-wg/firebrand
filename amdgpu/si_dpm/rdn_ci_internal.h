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
 */

/*
 * Shared between rdn_ci_dpm.c, rdn_ci_smu.c and rdn_ci_tables.c only: the
 * register access idioms of ci_dpm.c, the byte-order helpers, the
 * radeon_mode.h structures the ATOM helpers fill, and the prototypes of the
 * functions that Linux has in r600_dpm.c, radeon_atombios.c, ci_smc.c,
 * si_dpm.c, ni_dpm.c, rv770_dpm.c and cik.c.
 */
#ifndef RDN_CI_INTERNAL_H
#define RDN_CI_INTERNAL_H

#include <string.h>

#include "rdn_ci_dpm.h"
#include "rdn_cik_smc.h"
#include "linux/cikd.h"
#include "rdn_dpm_atom.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 110
#endif

/* rdev->usec_timeout */
#define CI_USEC_TIMEOUT 100000

/* cik_reg.h: the DIDT index/data pair (cik_didt_rreg / cik_didt_wreg) */
#define CIK_DIDT_IND_INDEX	0xca00
#define CIK_DIDT_IND_DATA	0xca04

/* nid.h: the arbiter's other three timing sets (ni_copy_and_switch_arb_sets) */
#define MC_ARB_DRAM_TIMING_1	0x27f0
#define MC_ARB_DRAM_TIMING_2	0x27f4
#define MC_ARB_DRAM_TIMING_3	0x27f8
#define MC_ARB_DRAM_TIMING2_1	0x27fc
#define MC_ARB_DRAM_TIMING2_2	0x2800
#define MC_ARB_DRAM_TIMING2_3	0x2804
#define MC_CG_CONFIG		0x25bc
#define MC_ARB_CG		0x27e8
#define CG_ARB_REQ(x)		((x) << 0)
#define CG_ARB_REQ_MASK		(0xff << 0)

/* rv770d.h: the VBIOS's memory module index lives in BIOS_SCRATCH_4[23:16] */
#define BIOS_SCRATCH_4		0x1734

/* r600_dpm.h */
#define R600_SSTU_DFLT		0
#define R600_SST_DFLT		0x00C8
#define R600_TEMP_RANGE_MIN	(90 * 1000)
#define R600_TEMP_RANGE_MAX	(120 * 1000)
#define FDO_PWM_MODE_STATIC	1
#define FDO_PWM_MODE_STATIC_RPM	5

enum r600_display_gap {
	R600_PM_DISPLAY_GAP_VBLANK_OR_WM = 0,
	R600_PM_DISPLAY_GAP_VBLANK       = 1,
	R600_PM_DISPLAY_GAP_WATERMARK    = 2,
	R600_PM_DISPLAY_GAP_IGNORE       = 3
};

/* radeon.h's enum radeon_dpm_auto_throttle_src */
enum radeon_dpm_auto_throttle_src {
	RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL,
	RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL
};

/* ---- register access: RREG32/WREG32 and the three indirect spaces ---- */

static inline uint32_t RREG32(struct rdn_card *card, uint32_t reg)
{
	return rdn_rreg(card, reg);
}

static inline void WREG32(struct rdn_card *card, uint32_t reg, uint32_t v)
{
	rdn_wreg(card, reg, v);
}

/* WREG32_P(reg, val, mask): keep the bits of `mask`, replace the rest */
static inline void WREG32_P(struct rdn_card *card, uint32_t reg, uint32_t val, uint32_t mask)
{
	uint32_t tmp = rdn_rreg(card, reg);

	tmp &= mask;
	tmp |= val;
	rdn_wreg(card, reg, tmp);
}

/* RREG32_SMC / WREG32_SMC: Linux takes smc_idx_lock here; this library runs under the caller's lock */
static inline uint32_t RREG32_SMC(struct rdn_card *card, uint32_t reg)
{
	return rdn_cik_smc_rreg(card, reg);
}

static inline void WREG32_SMC(struct rdn_card *card, uint32_t reg, uint32_t v)
{
	rdn_cik_smc_wreg(card, reg, v);
}

/* cik_didt_rreg / cik_didt_wreg */
static inline uint32_t RREG32_DIDT(struct rdn_card *card, uint32_t reg)
{
	rdn_wreg(card, CIK_DIDT_IND_INDEX, reg);
	return rdn_rreg(card, CIK_DIDT_IND_DATA);
}

static inline void WREG32_DIDT(struct rdn_card *card, uint32_t reg, uint32_t v)
{
	rdn_wreg(card, CIK_DIDT_IND_INDEX, reg);
	rdn_wreg(card, CIK_DIDT_IND_DATA, v);
}

/*
 * cik_pciep_rreg(): the PCIe port registers through PCIE_INDEX / PCIE_DATA
 * with the WHOLE register number written to the index (0x100100A2 and
 * 0x100100A4 for the link width and speed). r600_pciep_rreg() masked the
 * index to 0xff and this used to copy that; measured on the W8100
 * 2026-11-04: index 0xA4 reads 0, index 0x100100A4 reads 0x044d0d27, so the
 * masked form fed the DPM tables a gen1 x16 link that never existed.
 */
static inline uint32_t RREG32_PCIE_PORT(struct rdn_card *card, uint32_t reg)
{
	rdn_wreg(card, PCIE_INDEX, reg);
	(void)rdn_rreg(card, PCIE_INDEX);
	return rdn_rreg(card, PCIE_DATA);
}

static inline void udelay(struct rdn_card *card, uint32_t us)
{
	card->os->delay_us(card->os->cookie, us);
}

/* ---- byte order ---- */

/*
 * The SMU's SRAM tables are big-endian: Linux wraps every field in
 * cpu_to_be16/32 before ci_copy_bytes_to_smc() sends the bytes in memory
 * order. These are those wrappers, explicit about the host's byte order
 * rather than relying on PowerPC being big-endian.
 */
#ifndef RDN_BIG_ENDIAN
#error "RDN_BIG_ENDIAN is not defined (rdn_card.h): the SMC tables would be byte-reversed on a big-endian host"
#endif
static inline uint16_t cpu_to_be16(uint16_t v)
{
#if RDN_BIG_ENDIAN
	return v;
#else
	return (uint16_t)((v << 8) | (v >> 8));
#endif
}

static inline uint32_t cpu_to_be32(uint32_t v)
{
#if RDN_BIG_ENDIAN
	return v;
#else
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
#endif
}

#define be16_to_cpu(v) cpu_to_be16(v)
#define be32_to_cpu(v) cpu_to_be32(v)

/*
 * The two fields Linux forgets to wrap (DefaultTdp, TargetTdp) reach the SMU
 * as a little-endian host leaves them: 133 W * 256 = 0x8500 lands as bytes
 * 00 85, which the firmware reads as 133. Measured against the Linux
 * session's SRAM dump 2026-11-04. cpu_to_le16() from
 * atom_port.h (rdn_swap_le16) gives the same bytes from a big-endian host.
 */

/* VBIOS data and ATOM parameter spaces are little-endian; read and write them as bytes. */
#define le16_at(base, off)	rdn_get_le16((base), (uint32_t)(off))
#define le32_at(base, off)	rdn_get_le32((base), (uint32_t)(off))
#define u8_at(base, off)	rdn_get_u8((base), (uint32_t)(off))

static inline void put_le16(void *base, uint32_t off, uint16_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(void *base, uint32_t off, uint32_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- rdn_ci_smu.c: ci_smc.c and the SMU table half of ci_dpm.c ---- */

#define MC_CG_ARB_FREQ_F0           0x0a
#define MC_CG_ARB_FREQ_F1           0x0b
#define MC_CG_ARB_FREQ_F2           0x0c
#define MC_CG_ARB_FREQ_F3           0x0d

#define SMC_RAM_END 0x40000

#define VOLTAGE_SCALE               4
#define VOLTAGE_VID_OFFSET_SCALE1    625
#define VOLTAGE_VID_OFFSET_SCALE2    100

int ci_copy_bytes_to_smc(struct rdn_ci_dpm *dpm, uint32_t smc_start_address, const uint8_t *src,
			 uint32_t byte_count, uint32_t limit);
bool ci_is_smc_running(struct rdn_card *card);
int ci_read_smc_sram_dword(struct rdn_ci_dpm *dpm, uint32_t smc_address, uint32_t *value, uint32_t limit);
int ci_write_smc_sram_dword(struct rdn_ci_dpm *dpm, uint32_t smc_address, uint32_t value, uint32_t limit);
PPSMC_Result ci_send_msg_to_smc(struct rdn_ci_dpm *dpm, PPSMC_Msg msg);
PPSMC_Result ci_send_msg_to_smc_with_parameter(struct rdn_ci_dpm *dpm, PPSMC_Msg msg, uint32_t parameter);
PPSMC_Result ci_send_msg_to_smc_return_parameter(struct rdn_ci_dpm *dpm, PPSMC_Msg msg, uint32_t *parameter);
int ci_write_smc_soft_register(struct rdn_ci_dpm *dpm, uint16_t reg_offset, uint32_t value);

int ci_process_firmware_header(struct rdn_ci_dpm *dpm);
int ci_construct_voltage_tables(struct rdn_ci_dpm *dpm);
int ci_initialize_mc_reg_table(struct rdn_ci_dpm *dpm);
int ci_populate_initial_mc_reg_table(struct rdn_ci_dpm *dpm);
int ci_update_and_upload_mc_reg_table(struct rdn_ci_dpm *dpm);
int ci_init_smc_table(struct rdn_ci_dpm *dpm);
int ci_init_arb_table_index(struct rdn_ci_dpm *dpm);
int ci_initial_switch_from_arb_f0_to_f1(struct rdn_ci_dpm *dpm);
int ci_force_switch_to_arb_f0(struct rdn_ci_dpm *dpm);
int ci_program_memory_timing_parameters(struct rdn_ci_dpm *dpm);
int ci_populate_pm_base(struct rdn_ci_dpm *dpm);
int ci_populate_all_graphic_levels(struct rdn_ci_dpm *dpm);
int ci_populate_all_memory_levels(struct rdn_ci_dpm *dpm);
int ci_populate_bapm_parameters_in_dpm_table(struct rdn_ci_dpm *dpm);
int ci_setup_default_dpm_tables(struct rdn_ci_dpm *dpm);
uint32_t ci_get_dpm_level_enable_mask_value(struct ci_single_dpm_table *dpm_table);
int ci_update_sclk_t(struct rdn_ci_dpm *dpm);
int ci_thermal_setup_fan_table(struct rdn_ci_dpm *dpm);
void ci_initialize_powertune_defaults(struct rdn_ci_dpm *dpm);

#endif /* RDN_CI_INTERNAL_H */
