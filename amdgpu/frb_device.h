/*
 * Firebrand's amdgpu_device: the state the ported IP blocks share, and the
 * register accessors they use.
 *
 * The IP-block files in this directory are ports of Linux amdgpu
 * (drivers/gpu/drm/amd/amdgpu, Linux 7.0.2), one file for one file:
 *
 *   frb_drv.c       amdgpu_drv.c       PCI id rows (the family table)
 *   frb_device.c    amdgpu_device.c    init order, need_post, register sequences
 *   frb_atombios.c  amdgpu_atombios.c  scratch registers, ASIC_Init, VBIOS survey
 *   frb_vi.c        vi.c               vi_common: rev ids, SMC/PCIE windows, doorbells
 *   frb_gmc_v8_0.c  gmc_v8_0.c         memory controller, MC microcode, GART
 *   frb_gart.c      amdgpu_gart.c      the GART table (with RadeonNI's address checks)
 *   frb_ucode.c     amdgpu_ucode.c     firmware headers
 *   frb_smu7.c      pm/powerplay/smumgr/smu7_smumgr.c + polaris10_smumgr.c (start)
 *   frb_uvd.c       amdgpu_uvd.c       UVD firmware, VCPU bo, the create/destroy msg IB test
 *   frb_uvd_v6_0.c  uvd_v6_0.c         UVD 6.3: start/stop, the decode ring, clock gating
 *
 * Functions that are static in Linux keep Linux's names, so a port can be
 * diffed against its source; anything visible outside a file is frb_.
 * Inside a ported file `adev` is a struct frb_device *, and RREG32/WREG32 and
 * friends take dword register indices exactly as in amdgpu.
 *
 * BYTE ORDER. The host is big-endian and amdgpu never was. Registers cross
 * the OS layer in CPU order (the kext swaps). Everything the GPU or the SMU
 * reads from MEMORY is little-endian and is written with explicit
 * little-endian stores (frb_put_le32, frb_vram_write32); docs/byte-order.md
 * lists each such structure.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_DEVICE_H
#define FRB_DEVICE_H

#include "../shared/frb_os.h"
#include "../shared/atom/atom.h"

#ifndef ETIMEDOUT
#define ETIMEDOUT 60
#endif
#ifndef ENODEV
#define ENODEV 19
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EIO
#define EIO 5
#endif
#ifndef ERANGE
#define ERANGE 34
#endif
#ifndef EPERM
#define EPERM 1
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif
#ifndef EBUSY
#define EBUSY 16
#endif
#ifndef ENOSYS
#define ENOSYS 78
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

/* include/drm/amd_asic_type.h, the VI part (values as Linux numbers them) */
enum amd_asic_type {
	CHIP_TOPAZ = 10,
	CHIP_TONGA,
	CHIP_FIJI,
	CHIP_CARRIZO,
	CHIP_STONEY,
	CHIP_POLARIS10,
	CHIP_POLARIS11,
	CHIP_POLARIS12,
	CHIP_VEGAM,
	CHIP_UNKNOWN_FRB = 0xff
};

#define AMDGPU_FAMILY_VI		130	/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_MAX_USEC_TIMEOUT		100000	/* amdgpu.h: 100 ms */
#define AMDGPU_GPU_PAGE_SIZE		4096
#define AMDGPU_GPU_PAGE_SHIFT		12
#define FRB_GART_LEGACY_START		0x40000000ULL	/* RadeonNI hw/rdn_gart.h RDN_GART_GPU_START */

/* amdgpu.h */
#define ASICID_IS_P23(did, rid)		(((did == 0x6987) && \
					 ((rid == 0xC0) || \
					  (rid == 0xC1) || \
					  (rid == 0xC3) || \
					  (rid == 0xC7))) || \
					 ((did == 0x6981) && \
					 ((rid == 0x00) || \
					  (rid == 0x01) || \
					  (rid == 0x10))))

/*
 * Bring-up stages. Each is a set of register writes the amdgpu init sequence
 * makes, in its order; a stage runs only if every earlier one succeeded.
 * The default is FRB_STAGE_SURVEY: VBIOS and register READS only.
 */
enum frb_stage {
	FRB_STAGE_SURVEY = 0,	/* identity, VBIOS, command-table revisions; no writes */
	FRB_STAGE_POST,		/* scratch registers, need_post, ASIC_Init (F1) */
	FRB_STAGE_GMC,		/* vi_common hw_init, gmc_v8_0 sw/hw init, GART (F2) */
	FRB_STAGE_SMU,		/* SMU7 protected-mode start, microcode TOC (F3) */
	FRB_STAGE_DCE,		/* dce_v11_0 hw_init, connectors, detect (display) */
	FRB_STAGE_GFX,		/* gfx_v8_0 hw_init: RLC, CP gfx ring, ring test */
	FRB_STAGE_SDMA,		/* sdma_v3_0 hw_init: both SDMA rings, ring tests */
	FRB_STAGE_UVD,		/* uvd_v6_0 sw/hw_init: firmware, VCPU start, ring and IB tests */
	FRB_STAGE_COUNT
};

/*
 * One row of the family table (frb_drv.c). A card is driven only past the
 * stages its row says were verified on that card; a 699f with no row, or a
 * row with max_stage 0, gets the survey and nothing else.
 */
struct frb_asic_row {
	uint16_t device;
	uint16_t subsys_vendor, subsys_device;	/* 0 = any */
	uint8_t revision;
	enum amd_asic_type asic_type;
	const char *asic_name;		/* amdgpu_asic_name[] */
	const char *fw_prefix;		/* linux-firmware amdgpu/ prefix */
	const char *board;		/* what the card is */
	uint8_t max_stage;		/* highest enum frb_stage verified on this card */
};

const struct frb_asic_row *frb_asic_lookup(uint16_t device, uint8_t revision,
					   uint16_t subsys_vendor, uint16_t subsys_device);
/* amdgpu_drv.c pciidlist: is the device id a Polaris at all (any revision)? */
enum amd_asic_type frb_asic_type_of(uint16_t device);
const char *frb_asic_name(enum amd_asic_type t);

struct frb_fw {
	const uint8_t *data;
	uint32_t size;
};

/* amdgpu_gmc.h, the fields gmc_v8_0 uses */
struct frb_gmc {
	uint64_t mc_vram_size, real_vram_size, visible_vram_size;
	uint64_t aper_base, aper_size;
	uint64_t vram_start, vram_end;
	uint64_t gart_size, gart_start, gart_end;
	uint64_t agp_size, agp_start, agp_end;
	uint64_t mc_mask;
	uint32_t vram_width, vram_type;
	uint32_t stolen_vga_size;	/* amdgpu_gmc_get_vbios_allocations: kept at VRAM 0 */
	uint32_t fw_version;
	struct frb_fw fw;		/* <fw_prefix>_mc.bin, chosen in init_microcode */
	char fw_name[32];
};

/* amdgpu_gart.h */
struct frb_gart {
	uint64_t table_addr;		/* MC address of the table (in VRAM) */
	uint32_t table_offset;		/* the same, as a BAR0 offset */
	uint32_t num_gpu_pages;
	uint32_t table_size;
	int ready;
	uint32_t *used;			/* frb_gart_alloc's bitmap, one bit per GPU page */
};

/*
 * VRAM the driver keeps for itself. amdgpu allocates these top-down from
 * the end of VRAM; the CPU reaches only the 256 MB BAR0 here, so they live
 * at the top of the CPU-visible window instead (frb_gmc_v8_0.c,
 * "placement"). Offsets are from the start of VRAM, which BAR0 maps.
 */
struct frb_vram_layout {
	uint32_t reserved_start;	/* everything from here to the window top is ours */
	uint32_t smu_buffer;		/* SMU private scratch (SMU_DRAM_ADDR) */
	uint32_t smu_buffer_size;
	uint32_t fw_buf;		/* microcode images for the SMU to load */
	uint32_t fw_buf_size;
	uint32_t toc;			/* SMU_DRAMData_TOC (DRV_DRAM_ADDR) */
	uint32_t mem_scratch;		/* amdgpu_device_mem_scratch_init's page */
	uint32_t gart_table;		/* gmc_v8_0_gart_init's table */
	uint32_t objects;		/* rings, writeback, IB pool, CSB (frb_ring.c), until GTT */
	uint32_t objects_size;
	uint32_t uvd;			/* the UVD VCPU bo and the kernel's msg bo (frb_uvd.c) */
	uint32_t uvd_size;
};

/*
 * The PCIe capabilities amdgpu_device_get_pcie_info reads from the GPU's and
 * the upstream port's Link Capabilities (pcie_get_speed_cap /
 * pcie_get_width_cap): speed as the LnkCap encoding (1 = 2.5, 2 = 5, 3 = 8
 * GT/s), width in lanes; 0 = unknown, which takes Linux's defaults. The OS
 * layer fills them (the kext from config space, the replay from options).
 */
struct frb_pcie_caps {
	uint32_t asic_speed, asic_width;
	uint32_t platform_speed, platform_width;
};

struct frb_pm;	/* frb_amd_powerplay.c */

/* One register access, for the bring-up trace compared with Linux's. */
struct frb_trace_ent {
	uint32_t reg;			/* dword index; bit 31 = write */
	uint32_t val;
};
#define FRB_TRACE_WRITE		0x80000000u

struct frb_device {
	struct frb_os *os;

	/* PCI identity */
	uint16_t vendor, device, subsys_vendor, subsys_device;
	uint8_t revision;
	const struct frb_asic_row *row;		/* NULL: survey only */
	enum amd_asic_type asic_type;
	uint32_t family;
	uint32_t rev_id, external_rev_id;
	uint32_t rmmio_size;
	uint32_t usec_timeout;
	uint64_t cg_flags;			/* vi_common_early_init: AMD_CG_SUPPORT_* */
	uint32_t pg_flags;			/* AMD_PG_SUPPORT_* */

	/* VBIOS (amdgpu_bios.c / amdgpu_atombios.c) */
	uint8_t *bios;
	uint32_t bios_size;
	struct card_info atom_card;
	struct atom_context *atom;		/* adev->mode_info.atom_context */
	uint32_t bios_scratch_reg_offset;
	uint32_t default_sclk, default_mclk;	/* FirmwareInfo, 10 kHz units */
	uint32_t spll_reference_freq;		/* FirmwareInfo usCoreReferenceClock, 10 kHz */
	uint32_t num_crtc;			/* adev->mode_info.num_crtc */

	/* memory */
	struct frb_gmc gmc;
	struct frb_gart gart;
	struct frb_vram_layout layout;
	uint64_t dummy_page_addr;		/* bus address of a system page, 0 = none */
	uint64_t mem_scratch_gpu_addr;
	struct {
		uint64_t max_pfn;
		uint32_t block_size, fragment_size;
	} vm_manager;
	/*
	 * VMID 0's GART at FRB_GART_LEGACY_START instead of amdgpu's BEST_FIT
	 * (0xFF_0000_0000 on both cards): RadeonNI's legacy accelerator clients
	 * get the GART start as 32 bits (RDN_UC_GART_INFO). Set by the kext
	 * before stage gmc, only when the accelerator is wanted
	 * (FirebrandAccel / frb_accel=1); the default keeps amdgpu's placement.
	 */
	int gart_legacy;

	/* CPU view of VRAM: BAR0, mapped by the OS layer (NULL until F2) */
	volatile uint8_t *aper;
	uint64_t aper_size;

	/*
	 * Bring-up of a card whose row is not yet verified that far: lets a
	 * stage past row->max_stage run, never on a card without a row.
	 * Off unless the personality's FirebrandBringUp / boot-arg frb_bringup=1.
	 */
	int allow_unverified;

	/* progress */
	/*
	 * adopt: continue on a card an earlier Firebrand load already brought
	 * up in this boot (SMC running) as it is, without the ASIC reset that
	 * amdgpu, and Firebrand otherwise, does there (stage post).
	 */
	int adopt;
	struct frb_display *display;		/* frb_display.h; from stage dce on */

	/* frb_ring.c: VRAM objects, writeback, the IB pool */
	uint32_t obj_used;
	uint32_t wb_offset;
	uint64_t wb_used;
	uint32_t ib_pool_offset, ib_pool_used;
	struct frb_gfx *gfx;			/* frb_gfx_v8_0.c, from stage gfx */
	struct frb_gtt gtt_test;		/* frb_engines.c's GTT test buffer */
	uint32_t gtt_test_vram;
	struct frb_sdma *sdma;			/* frb_sdma_v3_0.c, from stage sdma */
	struct frb_uvd *uvd;			/* frb_uvd.c / frb_uvd_v6_0.c, from stage uvd */

	/* resets (frb_vi.c, frb_reset.c) */
	int has_hw_reset;			/* the ASIC was reset: post it again */
	uint32_t gpu_reset_counter;		/* resets and recoveries that worked */
	uint32_t hang_wb;			/* FRB_TEST_HANG's writeback dword, + 1 (0: none yet) */

	/*
	 * DPM (frb_pm.h): want_dpm from FirebrandDPM / frb_dpm=1 (off by
	 * default); run after stage sdma. pp_feature: amdgpu_pp_feature_mask
	 * (0: amdgpu's default). smu_hw_avfs: the AVFS fuse the SMU start read.
	 */
	int want_dpm;
	int want_audio;				/* FirebrandAudio / frb_audio=1: DP audio (frb_dce_audio.c) */
	int audio_pin;				/* frbctl audio pin N: the codec pin DP audio takes; -1 RadeonNI's choice */
	uint32_t pp_feature;
	struct frb_pm *pm;
	int dpm_failed;				/* errno of the DPM init, or 0 */
	int smu_hw_avfs;
	struct frb_pcie_caps pcie_caps;
	uint32_t pcie_gen_mask, pcie_mlw_mask;	/* adev->pm.pcie_gen_mask / pcie_mlw_mask */
	int pm_thermal_min, pm_thermal_max;	/* adev->pm.dpm.thermal, millidegrees */

	enum frb_stage stage_done;		/* highest stage completed */
	int stage_failed;			/* errno of the stage that failed, or 0 */
	uint32_t asic_init_ms;

	/* register trace (frbctl trace; tools/trace-compare.py) */
	struct frb_trace_ent *trace;
	uint32_t trace_cap, trace_len, trace_lost;
	uint32_t trace_squashed;	/* repeated identical reads not stored (trace_add) */
	int trace_on;
};

/* --- register access (amdgpu_device.c amdgpu_device_rreg/wreg, vi.c) ------ */
uint32_t frb_rreg(struct frb_device *adev, uint32_t reg);
void frb_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
uint32_t frb_smc_rreg(struct frb_device *adev, uint32_t reg);	/* vi_smc_rreg */
void frb_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
uint32_t frb_pcie_rreg(struct frb_device *adev, uint32_t reg);	/* vi_pcie_rreg */
void frb_pcie_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);

#define RREG32(reg)		frb_rreg(adev, (reg))
#define WREG32(reg, v)		frb_wreg(adev, (reg), (v))
#define RREG32_NO_KIQ(reg)	frb_rreg(adev, (reg))
#define WREG32_NO_KIQ(reg, v)	frb_wreg(adev, (reg), (v))
#define RREG32_SMC(reg)		frb_smc_rreg(adev, (reg))
#define WREG32_SMC(reg, v)	frb_smc_wreg(adev, (reg), (v))
#define RREG32_PCIE(reg)	frb_pcie_rreg(adev, (reg))
#define WREG32_PCIE(reg, v)	frb_pcie_wreg(adev, (reg), (v))
#define WREG32_P(reg, val, mask)				\
	do {							\
		uint32_t tmp_ = RREG32(reg);			\
		tmp_ &= (mask);					\
		tmp_ |= ((val) & ~(mask));			\
		WREG32(reg, tmp_);				\
	} while (0)

/* amdgpu.h */
#define REG_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define REG_FIELD_MASK(reg, field) reg##__##field##_MASK
#define REG_SET_FIELD(orig_val, reg, field, field_val)			\
	(((orig_val) & ~REG_FIELD_MASK(reg, field)) |			\
	 (REG_FIELD_MASK(reg, field) & ((field_val) << REG_FIELD_SHIFT(reg, field))))
#define REG_GET_FIELD(value, reg, field)				\
	(((value) & REG_FIELD_MASK(reg, field)) >> REG_FIELD_SHIFT(reg, field))
#define WREG32_FIELD(reg, field, val)	\
	WREG32(mm##reg, (RREG32(mm##reg) & ~REG_FIELD_MASK(reg, field)) | (val) << REG_FIELD_SHIFT(reg, field))

#define udelay(us)	adev->os->delay_us(adev->os->cookie, (us))
#define mdelay(ms)	adev->os->delay_us(adev->os->cookie, (ms) * 1000)
#define frb_info(adev, ...)	frb_log((adev)->os, FRB_LOG_INFO, __VA_ARGS__)
#define frb_err(adev, ...)	frb_log((adev)->os, FRB_LOG_ERROR, __VA_ARGS__)
#define frb_dbg(adev, ...)	frb_log((adev)->os, FRB_LOG_DEBUG, __VA_ARGS__)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#endif

/* "<prefix><suffix>" into dst, truncated (no stdio in the library) */
static inline void frb_fw_name(char *dst, uint32_t cap, const char *prefix, const char *suffix)
{
	uint32_t n = 0;

	while (*prefix && n + 1 < cap)
		dst[n++] = *prefix++;
	while (*suffix && n + 1 < cap)
		dst[n++] = *suffix++;
	if (cap)
		dst[n] = 0;
}

/* amdgpu_device.c */
void frb_device_program_register_sequence(struct frb_device *adev,
					  const uint32_t *registers, uint32_t array_size);
int frb_device_need_post(struct frb_device *adev);
/* the whole bring-up, as amdgpu_device_init orders it, up to `stage` */
int frb_device_init(struct frb_device *adev, enum frb_stage stage);
const char *frb_stage_name(enum frb_stage stage);
void frb_trace_start(struct frb_device *adev);
void frb_trace_stop(struct frb_device *adev);

/*
 * The engines (frb_ring.h has the submission core). Stage gfx and stage
 * sdma run the IP block's hw_init including Linux's ring tests; the tests
 * can be run again by hand (frbctl test). frb_engines_fini halts them
 * (hw_fini) before the driver lets go of its memory.
 */
struct frb_gfx;
struct frb_sdma;
struct frb_uvd;
enum { FRB_ENGINE_GFX = 0, FRB_ENGINE_SDMA0, FRB_ENGINE_SDMA1, FRB_ENGINE_UVD, FRB_ENGINE_COUNT };
/*
 * FRB_TEST_IB_PROBE (GFX): which fetch-swap settings run which IBs (frb_gfx_v8_0.c, diagnosis).
 * FRB_TEST_HANG: leave the engine waiting for a writeback dword that never
 * changes, with a fence behind the wait (frb_engines.c), for the recovery.
 */
/*
 * FRB_TEST_SWAP_PROBE (UVD): the decode ring and the msg IB test under both
 * UVD_LMI_SWAP_CNTL settings (0 with little-endian rings and IBs, amdgpu's
 * __BIG_ENDIAN 0xa with host-order ones), the default restored after
 * (frb_uvd_v6_0.c, diagnosis). FRB_TEST_IDLE (UVD): the idle work now.
 */
enum { FRB_TEST_RING = 0, FRB_TEST_IB, FRB_TEST_COPY, FRB_TEST_FILL, FRB_TEST_GTT, FRB_TEST_IB_PROBE,
       FRB_TEST_HANG, FRB_TEST_SWAP_PROBE, FRB_TEST_IDLE, FRB_TEST_COUNT };
int frb_gfx_v8_0_init(struct frb_device *adev);
void frb_gfx_v8_0_fini(struct frb_device *adev);
int frb_gfx_v8_0_test(struct frb_device *adev, int test);
/* the accelerator's view of the GFX block (frb_gfx_v8_0.c) */
struct frb_ring;
struct frb_gfx_info {
	uint32_t num_se, num_sh_per_se, max_cu_per_sh, num_cu;
	uint32_t num_rbs, backend_enable_mask, max_backends_per_se;
	uint32_t max_tile_pipes, gb_addr_config, mc_arb_ramcfg;
	uint32_t max_hw_contexts, ce_ram_size;
};
struct frb_ring *frb_gfx_v8_0_ring(struct frb_device *adev);	/* NULL unless the ring is up */
int frb_gfx_v8_0_get_info(struct frb_device *adev, struct frb_gfx_info *info);
/* amdgpu_gfx_rlc_enter/exit_safe_mode (gfx_v8_0's set/unset_safe_mode under it) */
void frb_gfx_v8_0_rlc_enter_safe_mode(struct frb_device *adev);
void frb_gfx_v8_0_rlc_exit_safe_mode(struct frb_device *adev);
int frb_gfx_v8_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value);
int frb_sdma_v3_0_init(struct frb_device *adev);
void frb_sdma_v3_0_fini(struct frb_device *adev);
int frb_sdma_v3_0_test(struct frb_device *adev, int instance, int test);
/* amdgpu_copy_buffer / amdgpu_ttm_fill_mem on SDMA ring `instance`, polled; GPU addresses */
int frb_sdma_v3_0_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset,
			      uint64_t dst_offset, uint32_t byte_count);
int frb_sdma_v3_0_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data,
			      uint64_t dst_addr, uint32_t byte_count);
/* the accelerator's 2D path: SDMA ring `instance` once it is up (else NULL), and its IB padding */
struct frb_ib;
struct frb_ring *frb_sdma_v3_0_ring(struct frb_device *adev, int instance);
void frb_sdma_v3_0_pad_ib(struct frb_ring *ring, struct frb_ib *ib);
/* UVD 6.3, the decode ring (frb_uvd.h has the rest) */
int frb_uvd_v6_0_init(struct frb_device *adev);
void frb_uvd_v6_0_fini(struct frb_device *adev);
int frb_uvd_v6_0_test(struct frb_device *adev, int test);
int frb_engine_test(struct frb_device *adev, int engine, int test);
void frb_engines_fini(struct frb_device *adev);

/*
 * Recovery (frb_reset.c): amdgpu_ip_funcs' soft-reset hooks, per block, and
 * amdgpu_device_gpu_recover's soft arm over them.
 */
struct frb_ip_reset_funcs {
	const char *name;
	int (*check_soft_reset)(struct frb_device *adev);	/* nonzero: busy, so hung */
	void (*force_soft_reset)(struct frb_device *adev);	/* not amdgpu: mark it hung */
	int (*pre_soft_reset)(struct frb_device *adev);
	int (*soft_reset)(struct frb_device *adev);
	int (*post_soft_reset)(struct frb_device *adev);	/* the ring tests' result */
};
extern const struct frb_ip_reset_funcs frb_gfx_v8_0_reset_funcs;
extern const struct frb_ip_reset_funcs frb_sdma_v3_0_reset_funcs;
void frb_gfx_v8_0_ring_soft_recovery(struct frb_ring *ring, unsigned vmid);

/* which blocks: bit per FRB_RECOVER_* */
#define FRB_RECOVER_GFX		(1u << 0)
#define FRB_RECOVER_SDMA	(1u << 1)
struct frb_recover_report {
	uint32_t grbm_status, grbm_status2, srbm_status, srbm_status2, cp_stat;
	uint32_t sdma_status[2];
	uint32_t hung;			/* FRB_RECOVER_* bits found (or forced) hung */
	uint32_t still_hung;		/* ... and after the reset */
	uint32_t ring_failed;		/* ... whose rings failed their tests after it */
	int soft_recovered;		/* the waves were killed and the fence came (no reset) */
	uint32_t ms;
};
/*
 * force: FRB_RECOVER_* blocks to reset whatever their status says (0: only
 * the ones check_soft_reset finds busy, after the soft recovery). 0 when
 * nothing is hung or the reset worked, -EIO when a block is still hung or
 * a ring failed (a full reset is the only way left: reload the driver).
 */
int frb_device_gpu_recover(struct frb_device *adev, uint32_t force, struct frb_recover_report *rep);

/* frb_dce_v11_0.c: stage dce, and its teardown (frb_display.h has the rest) */
int frb_display_init(struct frb_device *adev);
void frb_display_fini(struct frb_device *adev);

/* VRAM through BAR0, little-endian words (the GPU's order) */
int frb_vram_ok(struct frb_device *adev, uint32_t offset, uint32_t bytes);
void frb_vram_write32(struct frb_device *adev, uint32_t offset, uint32_t v);
uint32_t frb_vram_read32(struct frb_device *adev, uint32_t offset);

/* amdgpu_atombios.c (frb_atombios.c) */
int frb_atombios_init(struct frb_device *adev);
void frb_atombios_fini(struct frb_device *adev);
void frb_atombios_scratch_regs_init(struct frb_device *adev);
int frb_atombios_scratch_need_asic_init(struct frb_device *adev);
void frb_atombios_scratch_regs_engine_hung(struct frb_device *adev, int hung);
int frb_atombios_allocate_fb_scratch(struct frb_device *adev);
int frb_atombios_get_clock_info(struct frb_device *adev);
/* amdgpu_atombios.h struct atom_clock_dividers, the fields the CI/VI arm fills */
struct frb_atom_clock_dividers {
	uint32_t post_div;
	uint32_t whole_fb_div, frac_fb_div;
	uint32_t ref_div;
	uint32_t flags;
	uint32_t real_clock;		/* 10 kHz */
	uint32_t post_divider;		/* the DFS divider (v1.6 ulClock.ucPostDiv) */
};
#define FRB_COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK	0x00
int frb_atombios_get_clock_dividers(struct frb_device *adev, uint8_t clock_type, uint32_t clock,
				    int strobe_mode, struct frb_atom_clock_dividers *dividers);
void frb_atombios_survey(struct frb_device *adev);
int frb_device_asic_init(struct frb_device *adev);

/* vi.c (frb_vi.c) */
void frb_vi_common_early_init(struct frb_device *adev);
uint32_t frb_vi_get_xclk(struct frb_device *adev);	/* vi_get_xclk, 10 kHz */
uint32_t frb_didt_rreg(struct frb_device *adev, uint32_t reg);	/* vi_didt_rreg */
void frb_didt_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
uint32_t frb_gc_cac_rreg(struct frb_device *adev, uint32_t reg);	/* vi_gc_cac_rreg */
void frb_gc_cac_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
int frb_vi_need_reset_on_init(struct frb_device *adev);
uint32_t frb_vi_get_config_memsize(struct frb_device *adev);
/* amd_reset_method, the two VI has */
enum frb_reset_method { FRB_RESET_METHOD_LEGACY = 0, FRB_RESET_METHOD_BACO = 5 };
enum frb_reset_method frb_vi_asic_reset_method(struct frb_device *adev);
/* vi_asic_reset: the whole ASIC; it comes back unposted (has_hw_reset) */
int frb_vi_asic_reset(struct frb_device *adev);
/*
 * vi_read_register: a register user space may read (vi_allowed_read_registers),
 * by dword index; se/sh 0xffffffff = broadcast. 0 and the value, or -EINVAL.
 */
int frb_vi_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			 uint32_t *value);
int frb_vi_common_hw_init(struct frb_device *adev);
/* vi_set_uvd_clocks (10 kHz; 0 turns them off), vi_uvd_ctx_rreg/wreg (UVD_CTX_INDEX/DATA) */
int frb_vi_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk);
uint32_t frb_uvd_ctx_rreg(struct frb_device *adev, uint32_t reg);
void frb_uvd_ctx_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
#define RREG32_UVD_CTX(reg)	frb_uvd_ctx_rreg(adev, (reg))
#define WREG32_UVD_CTX(reg, v)	frb_uvd_ctx_wreg(adev, (reg), (v))
/* amd_shared.h, the flags vi_common_early_init sets for Polaris 12 that a ported block reads */
#define AMD_CG_SUPPORT_UVD_MGCG		(1ULL << 13)
#define AMD_CG_SUPPORT_VCE_MGCG		(1ULL << 14)
#define AMD_PG_SUPPORT_UVD		(1 << 3)

/* gmc_v8_0.c (frb_gmc_v8_0.c) */
void frb_gmc_v8_0_early_init(struct frb_device *adev);
int frb_gmc_v8_0_sw_init(struct frb_device *adev);
int frb_gmc_v8_0_hw_init(struct frb_device *adev);
void frb_gmc_v8_0_report(struct frb_device *adev);

/* amdgpu_gart.c (frb_gart.c) */
int frb_gart_init(struct frb_device *adev);
void frb_gart_set_pte(struct frb_device *adev, uint32_t page, uint64_t addr, uint64_t flags);
/* amdgpu_vm.h's PTE bits (VI) and amdgpu_ttm_tt_pte_flags for cached, snooped system pages */
#define AMDGPU_PTE_VALID	(1ULL << 0)
#define AMDGPU_PTE_SYSTEM	(1ULL << 1)
#define AMDGPU_PTE_SNOOPED	(1ULL << 2)
#define AMDGPU_PTE_EXECUTABLE	(1ULL << 4)
#define AMDGPU_PTE_READABLE	(1ULL << 5)
#define AMDGPU_PTE_WRITEABLE	(1ULL << 6)
#define FRB_PTE_GTT		(AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM | AMDGPU_PTE_SNOOPED | \
				 AMDGPU_PTE_READABLE | AMDGPU_PTE_WRITEABLE)
/* the GART aperture's pages (amdgpu_gtt_mgr, first fit) and amdgpu_gart_bind/unbind */
int frb_gart_alloc(struct frb_device *adev, uint32_t npages, uint32_t *first);
void frb_gart_free(struct frb_device *adev, uint32_t first, uint32_t npages);
void frb_gart_bind(struct frb_device *adev, uint32_t first, uint32_t npages, const uint64_t *bus,
		   uint64_t flags);
void frb_gart_unbind(struct frb_device *adev, uint32_t first, uint32_t npages);
void frb_gart_invalidate_tlb(struct frb_device *adev);
static inline uint64_t frb_gart_gpu_addr(struct frb_device *adev, uint32_t page)
{
	return adev->gmc.gart_start + (uint64_t)page * AMDGPU_GPU_PAGE_SIZE;
}
int frb_gart_bus_addr_ok(struct frb_device *adev, uint64_t bus);

/* smu7 (frb_smu7.c) */
int frb_smu7_start(struct frb_device *adev);
void frb_smu7_report(struct frb_device *adev);
/* smu7_smumgr.c's helpers for the DPM code. The message pair keeps Linux's
 * semantics: the answer is logged and 0 returned. */
int frb_smu7_copy_bytes_to_smc(struct frb_device *adev, uint32_t smc_start_address,
			       const uint8_t *src, uint32_t byte_count, uint32_t limit);
int frb_smu7_read_smc_sram_dword(struct frb_device *adev, uint32_t smc_addr, uint32_t *value, uint32_t limit);
int frb_smu7_write_smc_sram_dword(struct frb_device *adev, uint32_t smc_addr, uint32_t value, uint32_t limit);
int frb_smu7_send_msg_to_smc(struct frb_device *adev, uint16_t msg, uint32_t timeout_us);
int frb_smu7_send_msg_to_smc_with_parameter(struct frb_device *adev, uint16_t msg, uint32_t parameter,
					    uint32_t timeout_us);
uint32_t frb_smu7_get_argument(struct frb_device *adev);

#endif /* FRB_DEVICE_H */
