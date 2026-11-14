/*
 * The Linux-isms Firebrand's port of amdgpu powerplay (pm/powerplay, the
 * smu7 hardware manager and the polaris10 SMU manager) needs, mapped onto
 * Firebrand's library: types, logging, PP_ASSERT_WITH_CODE, allocation, the
 * CGS register accessors, the SMC byte-order macros.
 *
 * It is included by the frb_ powerplay files only (frb_hwmgr.h pulls it in),
 * so the rest of the library keeps its own conventions.
 *
 * BYTE ORDER (docs/byte-order.md, "SMC tables"). The SMC is big-endian.
 * amdgpu fills the SMU74_* structures (include/pp/smu74_discrete.h, packed)
 * with PP_HOST_TO_SMC_UL/US = cpu_to_be32/16 and copies their bytes into SMC
 * RAM most significant byte first (smu7_copy_bytes_to_smc). Those macros are
 * real conversions here, so on either host a converted field holds the SMC's
 * order. A multi-byte field amdgpu stores WITHOUT a conversion holds the
 * little-endian host's order on Linux; each such place is ported with
 * FRB_LEHOST32/16 (cpu_to_le), which reproduces Linux's bytes, and says so.
 * VBIOS data (the PowerPlay table, ATOM data tables) is little-endian: every
 * multi-byte field is read with frb_get_le16/32 at its offsetof() position
 * (FRB_VB16/32 below), never by dereferencing a structure.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_PP_LINUX_H
#define FRB_PP_LINUX_H

#include <stdbool.h>
#include <string.h>
#include "frb_device.h"

/* also in shared/atom/atom_port.h: gcc 4.0 rejects a repeated typedef */
#ifndef FRB_LINUX_UTYPES
#define FRB_LINUX_UTYPES
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif
typedef uint64_t u64;
typedef int32_t s32;

#ifndef ETIME
#define ETIME 62
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 102
#endif
#ifndef UINT_MAX
#define UINT_MAX 0xffffffffu
#endif

/* --- logging: pr_* have no device in Linux; the powerplay port logs through the
 * OS layer of the device it was started on (frb_pp_set_log, frb_amd_powerplay.c) */
extern struct frb_os *frb_pp_log_os;
#define frb_pp_log(level, ...)						\
	do {								\
		if (frb_pp_log_os)					\
			frb_log(frb_pp_log_os, (level), __VA_ARGS__);	\
	} while (0)
#define pr_err(...)	frb_pp_log(FRB_LOG_ERROR, __VA_ARGS__)
#define pr_warn(...)	frb_pp_log(FRB_LOG_INFO, __VA_ARGS__)
#define pr_info(...)	frb_pp_log(FRB_LOG_INFO, __VA_ARGS__)
#define pr_debug(...)	frb_pp_log(FRB_LOG_DEBUG, __VA_ARGS__)

/* pp_debug.h */
#define PP_ASSERT_WITH_CODE(cond, msg, code)	\
	do {					\
		if (!(cond)) {			\
			pr_warn("%s", msg);	\
			code;			\
		}				\
	} while (0)

/* allocation: zeroed, through the OS layer of hwmgr->adev */
#define frb_pp_zalloc(hwmgr, size)	((hwmgr)->adev->os->alloc((hwmgr)->adev->os->cookie, (size)))
#define frb_pp_free(hwmgr, p)						\
	do {								\
		if (p)							\
			(hwmgr)->adev->os->free((hwmgr)->adev->os->cookie, (p)); \
	} while (0)

/* include/linux/minmax.h, the scalar cases used */
#define min_t(type, a, b)	((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b)	((type)(a) > (type)(b) ? (type)(a) : (type)(b))
static inline uint32_t frb_min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t frb_max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

/* asm/div64.h do_div: n = n / base, returns the remainder */
#define do_div(n, base)	({ uint32_t __base = (base); uint32_t __rem = (uint32_t)((n) % __base); \
			   (n) = (n) / __base; __rem; })

/* fls/ffs on 32 bits (1-based; 0 for 0) */
static inline int frb_fls(uint32_t x)
{
	int r = 0;

	while (x) {
		r++;
		x >>= 1;
	}
	return r;
}
static inline int frb_ffs(uint32_t x)
{
	int r = 1;

	if (!x)
		return 0;
	while (!(x & 1)) {
		r++;
		x >>= 1;
	}
	return r;
}
#define fls(x)	frb_fls(x)
#define ffs(x)	frb_ffs(x)

/* --- byte order ----------------------------------------------------------- */

static inline uint32_t frb_swab32(uint32_t v)
{
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}
static inline uint16_t frb_swab16(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}
#if FRB_BIG_ENDIAN
#define cpu_to_be32(x)	((uint32_t)(x))
#define cpu_to_be16(x)	((uint16_t)(x))
#define be32_to_cpu(x)	((uint32_t)(x))
#define be16_to_cpu(x)	((uint16_t)(x))
#define FRB_LEHOST32(x)	frb_swab32((uint32_t)(x))
#define FRB_LEHOST16(x)	frb_swab16((uint16_t)(x))
#else
#define cpu_to_be32(x)	frb_swab32((uint32_t)(x))
#define cpu_to_be16(x)	frb_swab16((uint16_t)(x))
#define be32_to_cpu(x)	frb_swab32((uint32_t)(x))
#define be16_to_cpu(x)	frb_swab16((uint16_t)(x))
#define FRB_LEHOST32(x)	((uint32_t)(x))
#define FRB_LEHOST16(x)	((uint16_t)(x))
#endif

/* pp_endian.h */
#define PP_HOST_TO_SMC_UL(X)		cpu_to_be32(X)
#define PP_SMC_TO_HOST_UL(X)		be32_to_cpu(X)
#define PP_HOST_TO_SMC_US(X)		cpu_to_be16(X)
#define PP_SMC_TO_HOST_US(X)		be16_to_cpu(X)
#define CONVERT_FROM_HOST_TO_SMC_UL(X)	((X) = PP_HOST_TO_SMC_UL(X))
#define CONVERT_FROM_SMC_TO_HOST_UL(X)	((X) = PP_SMC_TO_HOST_UL(X))
#define CONVERT_FROM_HOST_TO_SMC_US(X)	((X) = PP_HOST_TO_SMC_US(X))

/* VBIOS fields: little-endian at offsetof() of an ATOM (packed) structure */
#define FRB_VB8(p, T, f)	frb_get_u8((p), offsetof(T, f))
#define FRB_VB16(p, T, f)	frb_get_le16((p), offsetof(T, f))
#define FRB_VB32(p, T, f)	frb_get_le32((p), offsetof(T, f))
/* entry i of the array `f` of a table T whose entries are struct R */
#define FRB_VB_ENTRY(p, T, f, R, i)	((const uint8_t *)(p) + offsetof(T, f) + (uint32_t)(i) * sizeof(R))

/* include/pp/pptable_v1_0.h's flexible arrays carry Linux's bounds annotation */
#ifndef __counted_by
#define __counted_by(member)
#endif

#ifndef min
#define min(a, b)	({ __typeof__(a) _min_a = (a); __typeof__(b) _min_b = (b); _min_a < _min_b ? _min_a : _min_b; })
#endif
#ifndef max
#define max(a, b)	({ __typeof__(a) _max_a = (a); __typeof__(b) _max_b = (b); _max_a > _max_b ? _max_a : _max_b; })
#endif

/* compile-time check of a layout (no C11 here: gcc 4.0.1) */
#define FRB_STATIC_ASSERT(name, cond)	typedef char frb_static_assert_##name[(cond) ? 1 : -1]

/* --- registers: cgs_common.h on Firebrand's accessors --------------------- */

enum cgs_ind_reg {
	CGS_IND_REG__PCIE,
	CGS_IND_REG__SMC,
	CGS_IND_REG__UVD_CTX,
	CGS_IND_REG__DIDT,
	CGS_IND_REG_GC_CAC,
	CGS_IND_REG_SE_CAC,
	CGS_IND_REG__AUDIO_ENDPT
};


static inline uint32_t cgs_read_register(struct frb_device *dev, uint32_t reg)
{
	return frb_rreg(dev, reg);
}
static inline void cgs_write_register(struct frb_device *dev, uint32_t reg, uint32_t v)
{
	frb_wreg(dev, reg, v);
}
static inline uint32_t cgs_read_ind_register(struct frb_device *dev, enum cgs_ind_reg space, uint32_t reg)
{
	switch (space) {
	case CGS_IND_REG__PCIE:	return frb_pcie_rreg(dev, reg);
	case CGS_IND_REG__SMC:	return frb_smc_rreg(dev, reg);
	case CGS_IND_REG__DIDT:	return frb_didt_rreg(dev, reg);
	case CGS_IND_REG_GC_CAC:	return frb_gc_cac_rreg(dev, reg);
	default:		return 0;
	}
}
static inline void cgs_write_ind_register(struct frb_device *dev, enum cgs_ind_reg space, uint32_t reg, uint32_t v)
{
	switch (space) {
	case CGS_IND_REG__PCIE:	frb_pcie_wreg(dev, reg, v); break;
	case CGS_IND_REG__SMC:	frb_smc_wreg(dev, reg, v); break;
	case CGS_IND_REG__DIDT:	frb_didt_wreg(dev, reg, v); break;
	case CGS_IND_REG_GC_CAC:	frb_gc_cac_wreg(dev, reg, v); break;
	default:		break;
	}
}

/* cgs_common.h CGS_REG_SET_FIELD and friends (smu7_powertune.c uses the _IND one) */
#define CGS_REG_FIELD_SHIFT(reg, field)	reg##__##field##__SHIFT
#define CGS_REG_FIELD_MASK(reg, field)	reg##__##field##_MASK
#define CGS_REG_SET_FIELD(orig_val, reg, field, field_val)			\
	(((orig_val) & ~CGS_REG_FIELD_MASK(reg, field)) |			\
	 (CGS_REG_FIELD_MASK(reg, field) & ((field_val) << CGS_REG_FIELD_SHIFT(reg, field))))
#define CGS_REG_GET_FIELD(value, reg, field)				\
	(((value) & CGS_REG_FIELD_MASK(reg, field)) >> CGS_REG_FIELD_SHIFT(reg, field))
#define CGS_WREG32_FIELD_IND(device, space, reg, field, val)	\
	cgs_write_ind_register(device, space, ix##reg, (cgs_read_ind_register(device, space, ix##reg) & ~CGS_REG_FIELD_MASK(reg, field)) | (val) << CGS_REG_FIELD_SHIFT(reg, field))

#endif /* FRB_PP_LINUX_H */
