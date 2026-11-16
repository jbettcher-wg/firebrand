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
 *   frb_uvd_v7_0.c  uvd_v7_0.c         UVD 7.2 (Vega 20, two instances): start/stop, the decode rings
 *
 * Vega 20 (SOC15, frb_soc15.h): frb_soc15.c (soc15.c, nbio_v7_4.c, hdp_v4_0.c,
 * vega20_reg_init.c), frb_atomfirmware.c (amdgpu_atomfirmware.c),
 * frb_gmc_v9_0.c / frb_gfxhub_v1_0.c / frb_mmhub_v1_0.c, frb_psp.c /
 * frb_psp_v11_0.c (amdgpu_psp.c, psp_v11_0.c), frb_vega20_smumgr.c and
 * frb_vega20_hwmgr.c (pm/powerplay), frb_dce_v12_0.c (DC's dce120),
 * frb_gfx_v9_0.c (gfx_v9_0.c, GC 9.4.0), frb_sdma_v4_0.c (sdma_v4_0.c, 4.2.0).
 * The stages keep their numbers; on Vega stage smu is the PSP (every
 * firmware image, the SMC's included) and then the SMU, as
 * amdgpu_device_fw_loading and powerplay's hw_init run them.
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
#ifndef EPROTO
#define EPROTO 100	/* Darwin's value */
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

/* include/drm/amd_asic_type.h, the SI, CIK and VI parts (values as Linux numbers them) */
enum amd_asic_type {
	CHIP_TAHITI = 0,
	CHIP_PITCAIRN,
	CHIP_VERDE,
	CHIP_OLAND,
	CHIP_HAINAN,
	CHIP_BONAIRE,
	CHIP_KAVERI,
	CHIP_KABINI,
	CHIP_HAWAII,
	CHIP_MULLINS,
	CHIP_TOPAZ,
	CHIP_TONGA,
	CHIP_FIJI,
	CHIP_CARRIZO,
	CHIP_STONEY,
	CHIP_POLARIS10,
	CHIP_POLARIS11,
	CHIP_POLARIS12,
	CHIP_VEGAM,
	CHIP_VEGA10,
	CHIP_VEGA12,
	CHIP_VEGA20,
	CHIP_UNKNOWN_FRB = 0xff
};

#define AMDGPU_FAMILY_SI		110	/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_FAMILY_CI		120	/* Bonaire, Hawaii */
#define AMDGPU_FAMILY_VI		130
#define AMDGPU_FAMILY_AI		141	/* Vega (SOC15) */
#define AMDGPU_MAX_USEC_TIMEOUT		100000	/* amdgpu.h: 100 ms */
#define AMDGPU_GPU_PAGE_SIZE		4096
#define AMDGPU_GPU_PAGE_SHIFT		12
#define FRB_GART_LEGACY_START		0x40000000ULL	/* RadeonNI hw/rdn_gart.h RDN_GART_GPU_START */

/* amdgpu.h */
/* amdgpu.h: the Polaris 10 and 11 "kicker" parts (frb_fw_select.c's MC and SMC choices) */
#define ASICID_IS_P20(did, rid)		(((did == 0x67DF) && \
					 ((rid == 0xE3) || \
					  (rid == 0xE4) || \
					  (rid == 0xE5) || \
					  (rid == 0xE7) || \
					  (rid == 0xEF))) || \
					 ((did == 0x6FDF) && \
					 ((rid == 0xE7) || \
					  (rid == 0xEF) || \
					  (rid == 0xFF))))
#define ASICID_IS_P30(did, rid)		((did == 0x67DF) && \
					((rid == 0xE1) || \
					 (rid == 0xF7)))
#define ASICID_IS_P21(did, rid)		(((did == 0x67EF) && \
					 ((rid == 0xE0) || \
					  (rid == 0xE5))) || \
					 ((did == 0x67FF) && \
					 ((rid == 0xCF) || \
					  (rid == 0xEF) || \
					  (rid == 0xFF))))
#define ASICID_IS_P31(did, rid)		((did == 0x67EF) && \
					((rid == 0xE2)))
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
	FRB_STAGE_GMC,		/* vi_common hw_init, gmc_v8_0 sw/hw init, GART (F2); SOC15:
				   soc15_common hw_init, gmc_v9_0 (both hubs) */
	FRB_STAGE_SMU,		/* SMU7 protected-mode start, microcode TOC (F3); SOC15: the
				   PSP's firmware loads, then the SMU (powerplay hw_init) */
	FRB_STAGE_DCE,		/* dce_v11_0 hw_init, connectors, detect (display) */
	FRB_STAGE_GFX,		/* gfx_v8_0 hw_init: RLC, CP gfx ring, ring test; SOC15: gfx_v9_0 */
	FRB_STAGE_SDMA,		/* sdma_v3_0 hw_init: both SDMA rings, ring tests; SOC15: sdma_v4_0 */
	FRB_STAGE_UVD,		/* uvd_v6_0 sw/hw_init: firmware, VCPU start, ring and IB tests;
				   SOC15: uvd_v7_0 (both instances, the image from the PSP's TMR) */
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
	uint32_t quirks;		/* FRB_QUIRK_*: what this one card needs beyond its family */
	uint8_t untested;		/* frb_asic_untested_row's: a card of a supported family with no row */
};

/*
 * The untested-card policy. A card whose device id is in a family Firebrand
 * has ported (frb_asic_ported) but which has no row of its own runs under a
 * row frb_asic_untested_row makes for it: display-only (FRB_UNTESTED_STAGE,
 * stage dce) by default, and with the unlock (FirebrandUnlock in the
 * personality, which `sudo frbctl unlock` sets; boot-arg frb_unlock=1, or
 * frbctl unlock on a loaded driver) every stage the family has
 * (FRB_UNLOCKED_STAGE). A verified row keeps its own max_stage either way.
 */
#define FRB_UNTESTED_STAGE	FRB_STAGE_DCE
#define FRB_UNLOCKED_STAGE	FRB_STAGE_UVD

/*
 * FRB_QUIRK_AUDIO_FN_STRAP: the card's HD Audio function (function 1) is
 * strapped on, but the BIF's audio pin strap (RCC_BIF_STRAP0 STRAP_AUD_PIN)
 * is 0, "no audio function": function 0's header type says single-function
 * and function 1 does not answer, so no OS finds it. The MI50 (1002:66a1,
 * subsystem 1002:081e) is built that way, having no outputs of its own; with
 * a Radeon Pro VII VBIOS it has six. A Radeon VII (66af) presumably has the
 * strap set as shipped and needs nothing. Firebrand sets STRAP_AUD_PIN when,
 * and only when, the straps read that way (frb_soc15_audio_function_strap);
 * it takes effect at once, without a reset. Then it asks the PCI bridge to
 * scan the device's functions again (PCIScanFunctions, no reset), and the
 * HD Audio driver attaches to the new function. On by default; off with
 * FirebrandAudioFunction false or boot-arg
 * frb_audiofn=0. Verified on the MI50, 2026-11-15.
 */
#define FRB_QUIRK_AUDIO_FN_STRAP	(1u << 0)

/*
 * The HD Audio function's straps (FRB_QUIRK_AUDIO_FN_STRAP; Firebrand's own,
 * amdgpu never writes them). Reads and logs RCC_DEV0_EPF1_STRAP0 and
 * RCC_BIF_STRAP0; with write set, and only when function 1 is strapped on
 * as an HD Audio controller (device 0xab20) while STRAP_AUD_PIN is 0, sets
 * STRAP_AUD_PIN to 3. Returns FRB_AUDIOFN_*.
 */
enum {
	FRB_AUDIOFN_NONE = 0,	/* function 1 not strapped on as HD Audio: nothing to do */
	FRB_AUDIOFN_ALREADY,	/* the audio pin strap already on */
	FRB_AUDIOFN_LEFT,	/* would need the write; not asked to */
	FRB_AUDIOFN_WRITTEN,	/* STRAP_AUD_PIN written and read back on */
	FRB_AUDIOFN_FAILED	/* written, but it did not read back */
};
struct frb_device;
int frb_soc15_audio_function_strap(struct frb_device *adev, int write);

const struct frb_asic_row *frb_asic_lookup(uint16_t device, uint8_t revision,
					   uint16_t subsys_vendor, uint16_t subsys_device);
/* amdgpu_drv.c pciidlist: the device id's chip (any revision), CHIP_UNKNOWN_FRB for none */
enum amd_asic_type frb_asic_type_of(uint16_t device);
/* the same with amdgpu's asic_type_quirks (67ff: Polaris 10 at some revisions, Polaris 11 at the rest) */
enum amd_asic_type frb_asic_type_of_rev(uint16_t device, uint8_t revision);
/*
 * Whether Firebrand has the chip's code at all: 1 for the families it ports
 * (SI, CIK, Polaris, Vega 20), 0 for the ids it only recognises (Vega 10 and
 * 12: their register bases, PSP 3.1 and powerplay are not ported, so the
 * driver reads their VBIOS for a report and never touches their registers).
 */
int frb_asic_ported(enum amd_asic_type t);
struct frb_device;
/*
 * frb_fw_select.c: the microcode file names amdgpu asks for, per chip,
 * revision and kicker part (pure; tests/frb_fw_select_test.c). 0 with the
 * name in buf; frb_fw_mc_name returns 1 for a chip with no MC image (Vega M);
 * -EINVAL for a chip without one.
 */
#define FRB_FW_TOC_COUNT	7	/* SDMA0, SDMA1, CE, PFP, ME, MEC, RLC */
const char *frb_fw_chip_prefix(enum amd_asic_type t);
int frb_fw_mc_name(char *buf, uint32_t n, enum amd_asic_type t, uint16_t device, uint8_t revision,
		   uint32_t mc_seq_misc0, uint32_t mc_io_debug_up159);
int frb_fw_mc_needs_misc0(enum amd_asic_type t);			/* SI: MC_SEQ_MISC0 decides si58 */
int frb_fw_mc_needs_up159(enum amd_asic_type t, uint16_t device, uint8_t revision); /* Polaris 12's 32-bit part */
int frb_fw_smc_name(char *buf, uint32_t n, enum amd_asic_type t, uint16_t device, uint8_t revision, int soft_key);
int frb_fw_vi_toc_names(enum amd_asic_type t, char names[FRB_FW_TOC_COUNT][32]);
const char *frb_fw_uvd_name(enum amd_asic_type t);			/* 0: no UVD (Hainan) */
/* every file a card of this identity may ask for (all register outcomes); the count put */
int frb_fw_names_for(enum amd_asic_type t, uint16_t device, uint8_t revision, char out[][32], int max);
extern const uint8_t frb_fw_revisions[];	/* the revisions the kicker rules name, and 0 */
extern const int frb_fw_revision_count;
/* frb_drv.c's pciidlist, for walking every device id the driver matches */
int frb_pciid_count(void);
uint16_t frb_pciid_device(int i);
/* the family table's verified rows */
int frb_verified_row_count(void);
const struct frb_asic_row *frb_verified_row(int i);
/* an untested card's row (adev->untested_row, from the PCI identity and asic_type); adev->row points at it */
void frb_asic_untested_row(struct frb_device *adev);
/* the highest stage this card may run to now: its row's, or an untested card's by the unlock */
enum frb_stage frb_stage_cap(const struct frb_device *adev);
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
	uint64_t fb_start, fb_end;		/* gmc_v9_0: the FB aperture, = VRAM here */
	uint64_t gart_size, gart_start, gart_end;
	uint64_t agp_size, agp_start, agp_end;
	uint64_t mc_mask;
	uint32_t vram_width, vram_type, vram_vendor;
	int noretry, translate_further;		/* gmc_v9_0 */
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
	uint32_t used_pages;		/* bits set in it (page 0, kept out, counts) */
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
	uint32_t gart_table;		/* gmc_vX_0_gart_init's table */
	uint32_t gart_table_size;	/* the room kept for it (512 KB VI, 2 MB SI's 1 GB GART) */
	uint32_t objects;		/* rings, writeback, IB pool, CSB (frb_ring.c), until GTT */
	uint32_t objects_size;
	uint32_t uvd;			/* the UVD VCPU bo and the kernel's msg bo (frb_uvd.c) */
	uint32_t uvd_size;
	/* SOC15 (frb_gmc_v9_0.c frb_gmc_v9_0_layout): the PSP's and the SMU's buffers */
	uint32_t psp_ring, psp_cmd, psp_fence;	/* one page each */
	uint32_t psp_fw_pri;			/* 1 MB, 1 MB aligned: sOS/sysdrv, ASD, TA images */
	uint32_t psp_tmr, psp_tmr_size;		/* the PSP's trusted memory region */
	uint32_t smu_tables, smu_tables_size;	/* vega20_smu_init's table bos */
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
struct frb_psp;	/* frb_psp.c */
struct frb_smu11;	/* frb_vega20_smumgr.c */

/* SOC15 register bases (frb_soc15.h): [HWIP][instance][segment] */
#define FRB_HWIP_MAX	40
#define FRB_HWIP_INST	2
#define FRB_HWIP_SEG	6

/* frb_soc15.h's struct frb_vmhub, here for its size */
struct frb_vmhub_regs {
	uint32_t ctx0_ptb_addr_lo32, ctx0_ptb_addr_hi32;
	uint32_t vm_inv_eng0_sem, vm_inv_eng0_req, vm_inv_eng0_ack;
	uint32_t vm_context0_cntl, vm_l2_pro_fault_status, vm_l2_pro_fault_cntl;
	uint32_t ctx_distance, ctx_addr_distance, eng_distance, eng_addr_distance;
};

/* One register access, for the bring-up trace compared with Linux's. */
struct frb_trace_ent {
	uint32_t reg;			/* dword index; bit 31 = write */
	uint32_t val;
};
#define FRB_TRACE_WRITE		0x80000000u

/*
 * The family's IP blocks, as amdgpu's si_set_ip_blocks / vi_set_ip_blocks
 * pick them (frb_drv.c's frb_asic_funcs_of chooses by asic_type). Every
 * family-specific call the stages and the kext make goes through here; a
 * NULL engine means the family has no port of that block yet, and its
 * stage fails with -ENOSYS (a row never asks for it).
 */
struct frb_device;
struct frb_display_funcs;
struct frb_asic_funcs {
	const char *name;			/* "si", "cik", "vi", "soc15": the common block */
	uint32_t rmmio_bar;			/* config offset of the register BAR: 0x18 (BAR2) SI, 0x24 (BAR5) CIK and later */
	uint32_t family;			/* AMDGPU_FAMILY_* */
	uint32_t pcie_index, pcie_data;		/* the PCIE index/data pair: 0xe/0xf VI, 0xc/0xd SI */
	void (*early_init)(struct frb_device *adev);	/* the common block's early_init: rev ids, flags; reads only */
	uint32_t (*smc_rreg)(struct frb_device *adev, uint32_t reg);
	void (*smc_wreg)(struct frb_device *adev, uint32_t reg, uint32_t v);
	uint32_t (*get_config_memsize)(struct frb_device *adev);
	int (*need_reset_on_init)(struct frb_device *adev);
	/*
	 * amdgpu_asic_pre_asic_init: what the family needs before ASIC_Init
	 * (stage post, only when it posts). NULL on SI and VI as in amdgpu;
	 * CIK's is RadeonNI's measured SMC clock for the W8100's VBIOS
	 * (frb_cik.c).
	 */
	void (*pre_asic_init)(struct frb_device *adev);
	int (*asic_reset)(struct frb_device *adev);
	int (*common_hw_init)(struct frb_device *adev);	/* stage gmc, first */
	void (*gmc_early_init)(struct frb_device *adev);
	int (*gmc_sw_init)(struct frb_device *adev);
	int (*gmc_hw_init)(struct frb_device *adev);
	int (*smu_start)(struct frb_device *adev);	/* stage smu */
	const struct frb_display_funcs *display;	/* stage dce */
	int (*gfx_init)(struct frb_device *adev);	/* stage gfx */
	int (*sdma_init)(struct frb_device *adev);	/* stage sdma */
	int (*uvd_init)(struct frb_device *adev);	/* stage uvd */
	int (*read_register)(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			     uint32_t *value);
	uint32_t num_crtc;			/* the DCE's mode_info.num_crtc */
};
extern const struct frb_asic_funcs frb_vi_asic_funcs;	/* frb_vi.c */
extern const struct frb_asic_funcs frb_si_asic_funcs;	/* frb_si.c */
extern const struct frb_asic_funcs frb_cik_asic_funcs;	/* frb_cik.c */
extern const struct frb_asic_funcs frb_soc15_asic_funcs;	/* frb_soc15.c */
/* frb_drv.c: the family's table for an asic_type, NULL for none */
const struct frb_asic_funcs *frb_asic_funcs_of(enum amd_asic_type t);

struct frb_device {
	struct frb_os *os;
	const struct frb_asic_funcs *funcs;	/* the family's IP blocks (frb_asic_funcs_of) */

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

	/* SOC15: vega20_reg_base_init's bases, the two VM hubs (gfxhub 0, mmhub 8) */
	uint32_t reg_offset[FRB_HWIP_MAX][FRB_HWIP_INST][FRB_HWIP_SEG];
	struct frb_vmhub_regs vmhub[16];
	uint64_t vram_base_offset;		/* adev->vm_manager.vram_base_offset (MC_VM_FB_OFFSET) */
	struct frb_psp *psp;			/* frb_psp.c, from stage smu (SOC15) */
	struct frb_smu11 *smu11;		/* frb_vega20_smumgr.c, from stage smu (SOC15) */
	int psp_only;				/* FirebrandPSPOnly / frb_psp_only=1: stage smu stops after the PSP */

	/* VBIOS (amdgpu_bios.c / amdgpu_atombios.c) */
	int is_atom_fw;				/* atomfirmware (SOC15) rather than legacy ATOM tables */
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
		uint32_t num_level;		/* gmc_v9_0 */
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
	/*
	 * An untested card (row->untested): the unlock lets it past
	 * display-only (frb_stage_cap). FirebrandUnlock / boot-arg frb_unlock,
	 * or frbctl unlock on a loaded driver. Never affects a verified row.
	 */
	int unlocked;
	struct frb_asic_row untested_row;	/* frb_asic_untested_row's, when adev->row is it */

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
	struct frb_gfx6 *gfx6;			/* frb_gfx_v6_0.c (SI), from stage gfx */
	struct frb_si_dma *si_dma;		/* frb_si_dma.c (SI), from stage sdma */
	struct frb_si_dpm *si_dpm;		/* frb_si_dpm.c (SI), DPM after stage sdma */
	struct frb_ci_dpm *ci_dpm;		/* frb_ci_dpm.c (CIK), DPM after stage smu */
	struct frb_gfx7 *gfx7;			/* frb_gfx_v7_0.c (CIK), from stage gfx */
	struct frb_cik_sdma *cik_sdma;		/* frb_cik_sdma.c (CIK), from stage sdma */
	struct frb_gtt gtt_test;		/* frb_engines.c's GTT test buffer */
	uint32_t gtt_test_vram;
	struct frb_sdma *sdma;			/* frb_sdma_v3_0.c, from stage sdma */
	struct frb_uvd *uvd;			/* frb_uvd.c / frb_uvd_v6_0.c, from stage uvd */
	struct frb_gfx9 *gfx9;			/* SOC15: frb_gfx_v9_0.c, from stage gfx */
	struct frb_sdma4 *sdma4;		/* SOC15: frb_sdma_v4_0.c, from stage sdma */
	/*
	 * SOC15: the rings write their WPTR registers (amdgpu's use_doorbell =
	 * false arm) instead of ringing doorbells: FirebrandMMIOWptr /
	 * frb_mmio_wptr=1, or no BAR2 (os->doorbell_write64 NULL).
	 */
	int mmio_wptr;
	/*
	 * Vega 20: start only the first UVD instance and leave the second as a
	 * harvested one is left (FirebrandUVDOneInstance / frb_uvd_one=1). Off:
	 * both, as amdgpu.
	 */
	int uvd_one_inst;
	/*
	 * SOC15 clock gating (amdgpu_device_ip_late_init's set_cg_state GATE):
	 * want_gating from FirebrandGating (default on) / frb_cg=0;
	 * cg_gated: the blocks are gated now (frb_soc15_set_cg_state);
	 * sdma_late_done: sdma_v4_0_late_init (ULV) ran.
	 */
	int want_gating;
	int cg_gated;
	int sdma_late_done;
	/*
	 * AMD_CG_SUPPORT_* flags taken out of cg_flags (FirebrandGatingExclude /
	 * frb_cg_exclude): those blocks' set_clockgating_state takes its
	 * ungate arm. To find which gate a problem follows without giving up
	 * the rest.
	 */
	uint64_t cg_exclude;

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
	int no_cg;				/* frb_cg=0: SI and CIK leave their blocks' clock gating as hw_init left it */
	uint32_t si_dpm_quirks;			/* FirebrandSIDPMQuirks / frb_sidpm_quirks / frbctl dpm on N: RDN_SI_DPM_* (si_dpm/rdn_si_dpm.h) and FRB_SI_DPM_* */
/* a lone CRTC takes the whole line buffer and two DMIF buffers (RadeonNI's reading of dce6_line_buffer_adjust; amdgpu and radeon always halve it) */
#define FRB_SI_DPM_LB_WHOLE	(1u << 9)
#define FRB_SI_DPM_CG_KEEP	(1u << 10)	/* the blocks' clock gating stays on through si_dpm's enable and first switch (as before amdgpu's order was taken) */
#define FRB_SI_DPM_MC_UNGATED	(1u << 11)	/* the MC's MGCG and light sleep stay off once DPM runs */
	int si_dpm_wanted;			/* SI DPM (on by default; FirebrandSIDPM false / frb_sidpm=0: boot clocks) */
	int cg_on;				/* the SI / CIK blocks were gated (frb_si.c, frb_cik.c), so hw_fini ungates them */
	uint32_t cik_gb_addr_config;		/* CIK: gfx_v7_0_gpu_early_init's GB_ADDR_CONFIG (frb_cik_gfx_addr_config) */
	/*
	 * CIK DPM (frb_ci_dpm.c): ci_dpm_wanted from FirebrandCIDPM / frb_cidpm
	 * (DPM right after stage smu); ci_dpm_quirks the bring-up switches
	 * (frbctl dpm on <bits> on a CIK card).
	 */
	int ci_dpm_wanted;
	uint32_t ci_dpm_quirks;
#define FRB_CI_DPM_UPLOAD_ONLY	(1u << 0)	/* the image and tables into the SMC's SRAM, the SMC left in reset */
	int want_audio;				/* FirebrandAudio / frb_audio=1: DP audio (frb_dce_audio.c) */
	int force_tmds;				/* FirebrandForceTMDS / frb_force_tmds=1: a test aid, a silent DVI/HDMI connector taken as a DVI sink with no EDID (RadeonNI's FakeEDID) */
	int audio_pin;				/* frbctl audio pin N: the codec pin DP audio takes; -1 RadeonNI's choice */
	uint32_t pp_feature;
	struct frb_pm *pm;
	int dpm_failed;				/* errno of the DPM init, or 0 */
	int smu_hw_avfs;
	struct frb_pcie_caps pcie_caps;
	uint32_t pcie_gen_mask, pcie_mlw_mask;	/* adev->pm.pcie_gen_mask / pcie_mlw_mask (frb_device_get_pcie_info) */
	int pm_thermal_min, pm_thermal_max;	/* adev->pm.dpm.thermal, millidegrees */

	enum frb_stage stage_done;		/* highest stage completed */
	int stage_failed;			/* errno of the stage that failed, or 0 */
	enum frb_stage stage_failed_at;		/* the stage that failed (with stage_failed) */
	/* the VBIOS's identity (frb_atombios_survey_strings), for the status call and reports */
	char vbios_pn[48], vbios_ver[24], vbios_date[20];
	uint32_t asic_init_ms;

	/* register trace (frbctl trace; tools/trace-compare.py) */
	struct frb_trace_ent *trace;
	uint32_t trace_cap, trace_len, trace_lost;
	uint32_t trace_squashed;	/* repeated identical reads not stored (trace_add) */
	int trace_on;
};

/* Vega (SOC15) rather than VI: which IP-block files a stage runs */
static inline int frb_is_soc15(const struct frb_device *adev)
{
	return adev->asic_type >= CHIP_VEGA10 && adev->asic_type <= CHIP_VEGA20;
}

/* the device's table, VI when unset (the replays of the Polaris captures set none) */
static inline const struct frb_asic_funcs *frb_funcs(const struct frb_device *adev)
{
	return adev->funcs ? adev->funcs : &frb_vi_asic_funcs;
}

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
/* the family's early_init (vi_common / soc15_common) and the survey: reads only */
void frb_device_early_init(struct frb_device *adev);
void frb_device_survey(struct frb_device *adev);
/* RCC_CONFIG_MEMSIZE / CONFIG_MEMSIZE: VRAM in MB */
uint32_t frb_device_get_config_memsize(struct frb_device *adev);
/* amdgpu_device_get_pcie_info: pcie_gen_mask / pcie_mlw_mask from pcie_caps (frb_amd_powerplay.c) */
void frb_device_get_pcie_info(struct frb_device *adev);
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
struct frb_gfx6;
struct frb_si_dma;
struct frb_si_dpm;
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
	/*
	 * GFX9 (frb_gfx_v9_0_get_info; zero on GFX8): what amdgpu's
	 * AMDGPU_INFO_DEV_INFO hands Mesa from adev->gfx.config and cu_info.
	 * GFX9 has no tiling tables: addrlib works from gb_addr_config.
	 */
	uint32_t gfx_level;		/* 7 (CIK), 8 or 9; 0 from gfx_v6_0 */
	uint32_t cu_bitmap[4][4];	/* active CUs per SE (4) and SH (4), as amdgpu_cu_info.bitmap */
	uint32_t cu_ao_mask;		/* always-on CUs (amdgpu_cu_info.ao_cu_mask) */
	uint32_t simd_per_cu, max_waves_per_simd, wave_front_size, max_scratch_slots_per_cu, lds_size;
	uint32_t num_tcc_blocks;	/* max_texture_channel_caches */
	uint32_t max_gprs, max_gs_threads, gs_vgt_table_depth, gs_prim_buffer_depth;
	uint32_t sc_prim_fifo_size_frontend, sc_prim_fifo_size_backend;
	uint32_t sc_hiz_tile_fifo_size, sc_earlyz_tile_fifo_size;
	uint32_t double_offchip_lds_buf, db_debug2;
	/* the CP images' versions and features (the ucode headers; Mesa reads me/pfp/mec) */
	uint32_t me_fw_version, me_fw_feature, pfp_fw_version, pfp_fw_feature, mec_fw_version, mec_fw_feature;
	uint32_t csb_loaded;		/* the RLC has the clear-state buffer (CLEAR_STATE works) */
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
/* UVD 7.2 (Vega 20), both instances' decode rings; the tests run on every instance that is up */
int frb_uvd_v7_0_init(struct frb_device *adev);
void frb_uvd_v7_0_fini(struct frb_device *adev);
int frb_uvd_v7_0_test(struct frb_device *adev, int test);
/*
 * SOC15 (Vega 20): gfx v9 (GC 9.4.0) and sdma v4 (4.2.0), the same tests by
 * the same numbers; frb_engine_test and frb_engines_fini pick these on a
 * SOC15 card.
 */
struct frb_gfx9;
struct frb_sdma4;
int frb_gfx_v9_0_init(struct frb_device *adev);
void frb_gfx_v9_0_fini(struct frb_device *adev);
int frb_gfx_v9_0_test(struct frb_device *adev, int test);
struct frb_ring *frb_gfx_v9_0_ring(struct frb_device *adev);	/* NULL unless the ring is up */
int frb_sdma_v4_0_init(struct frb_device *adev);
void frb_sdma_v4_0_fini(struct frb_device *adev);
int frb_sdma_v4_0_test(struct frb_device *adev, int instance, int test);
struct frb_ring *frb_sdma_v4_0_ring(struct frb_device *adev, int instance);
void frb_sdma_v4_0_pad_ib(struct frb_ring *ring, struct frb_ib *ib);
int frb_sdma_v4_0_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset,
			      uint64_t dst_offset, uint32_t byte_count);
int frb_sdma_v4_0_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data,
			      uint64_t dst_addr, uint32_t byte_count);
/*
 * SOC15 power: amdgpu_device_set_cg_state over the blocks that are up, in
 * amdgpu's IP-block order to gate (soc15_common, gmc_v9_0, gfx_v9_0,
 * sdma_v4_0) and the reverse to ungate; Vega 20 has no power gating
 * (pg_flags 0). The pieces, by block: frb_soc15.c, frb_gmc_v9_0.c /
 * frb_mmhub_v1_0.c, frb_gfx_v9_0.c, frb_sdma_v4_0.c.
 */
int frb_soc15_set_cg_state(struct frb_device *adev, int gate);
void frb_soc15_common_set_clockgating_state(struct frb_device *adev, int gate);
void frb_gmc_v9_0_set_clockgating_state(struct frb_device *adev, int gate);
void frb_gfx_v9_0_set_clockgating_state(struct frb_device *adev, int gate);
void frb_sdma_v4_0_set_clockgating_state(struct frb_device *adev, int gate);
void frb_sdma_v4_0_late_init(struct frb_device *adev);	/* sdma_v4_0_setup_ulv */
void frb_sdma_v4_0_log_state(struct frb_device *adev, const char *where);
int frb_gfx_v9_0_up(struct frb_device *adev);		/* hw_init ran (the RLC/CP were touched) */
int frb_sdma_v4_0_up(struct frb_device *adev);
/* gfx_v6_0 (Southern Islands): the same calls as gfx_v8_0's (frb_gfx_v6_0.c) */
int frb_gfx_v6_0_init(struct frb_device *adev);
void frb_gfx_v6_0_fini(struct frb_device *adev);
int frb_gfx_v6_0_test(struct frb_device *adev, int test);
struct frb_ring *frb_gfx_v6_0_ring(struct frb_device *adev);
int frb_gfx_v6_0_get_info(struct frb_device *adev, struct frb_gfx_info *info);
int frb_gfx_v6_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value);
/* si_dma (Southern Islands): the same calls as sdma_v3_0's (frb_si_dma.c); its fill is whole dwords */
int frb_si_dma_init(struct frb_device *adev);
void frb_si_dma_fini(struct frb_device *adev);
int frb_si_dma_test(struct frb_device *adev, int instance, int test);
int frb_si_dma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset, uint64_t dst_offset,
			   uint32_t byte_count);
int frb_si_dma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data, uint64_t dst_addr,
			   uint32_t byte_count);
struct frb_ring *frb_si_dma_ring(struct frb_device *adev, int instance);
void frb_si_dma_pad_ib(struct frb_ring *ring, struct frb_ib *ib);
/* si_set_uvd_clocks (frb_si.c): UVD's UPLL, 10 kHz; 0 puts it in bypass */
int frb_si_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk);
/* si_get_xclk (frb_si.c): the GPU counter's reference clock, 10 kHz */
uint32_t frb_si_get_xclk(struct frb_device *adev);
int frb_uvd_v3_1_init(struct frb_device *adev);
int frb_uvd_v3_1_test(struct frb_device *adev, int test);
void frb_uvd_v3_1_fini(struct frb_device *adev);
int frb_uvd_v4_2_init(struct frb_device *adev);
int frb_uvd_v4_2_test(struct frb_device *adev, int test);
void frb_uvd_v4_2_fini(struct frb_device *adev);
int frb_uvd_v4_2_set_powergating_state(struct frb_device *adev, int gate);	/* stop / start */
/* cik_set_uvd_clocks (frb_cik.c): VCLK/DCLK by their DFS dividers, 10 kHz; 0 for the lowest */
int frb_cik_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk);
/* the SI blocks' clock gating, as amdgpu's late init (gate) and hw_fini (ungate) set it */
void frb_gmc_v6_0_set_clockgating_state(struct frb_device *adev, int gate);
void frb_gmc_v6_0_set_mc_clockgating(struct frb_device *adev, int gate);
void frb_gfx_v6_0_set_clockgating_state(struct frb_device *adev, int gate);
void frb_si_dma_set_clockgating_state(struct frb_device *adev, int gate);
/*
 * gfx_v7_0 and cik_sdma (Sea Islands): the same calls as gfx_v6_0's and
 * si_dma's (frb_gfx_v7_0.c, frb_cik_sdma.c); cik_sdma's packets are SDMA 3's
 * but for the sub-window's extent (frb_rect.h); its fill is amdgpu's
 * (FILLSIZE 0: a byte fill on SDMA 3, not yet measured on CIK)
 */
struct frb_gfx7;
struct frb_cik_sdma;
int frb_gfx_v7_0_init(struct frb_device *adev);
void frb_gfx_v7_0_fini(struct frb_device *adev);
int frb_gfx_v7_0_test(struct frb_device *adev, int test);
struct frb_ring *frb_gfx_v7_0_ring(struct frb_device *adev);
int frb_gfx_v7_0_get_info(struct frb_device *adev, struct frb_gfx_info *info);
int frb_gfx_v7_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value);
void frb_gfx_v7_0_set_clockgating_state(struct frb_device *adev, int gate);
int frb_cik_sdma_init(struct frb_device *adev);
void frb_cik_sdma_fini(struct frb_device *adev);
int frb_cik_sdma_test(struct frb_device *adev, int instance, int test);
int frb_cik_sdma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset, uint64_t dst_offset,
			     uint32_t byte_count);
int frb_cik_sdma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data, uint64_t dst_addr,
			     uint32_t byte_count);
struct frb_ring *frb_cik_sdma_ring(struct frb_device *adev, int instance);
void frb_cik_sdma_pad_ib(struct frb_ring *ring, struct frb_ib *ib);
void frb_cik_sdma_set_clockgating_state(struct frb_device *adev, int gate);
int frb_engine_test(struct frb_device *adev, int engine, int test);
void frb_engines_fini(struct frb_device *adev);
/* gfx_v9_0's counterpart of frb_gfx_v8_0_get_info (the GFX9 fields of frb_gfx_info filled in) */
int frb_gfx_v9_0_get_info(struct frb_device *adev, struct frb_gfx_info *info);

/*
 * The accelerator's engines by family (frb_engines.c), as amdgpu reaches
 * them through adev->gfx.gfx_ring[0], adev->mman.buffer_funcs_ring and
 * buffer_funcs: gfx_v8_0 / sdma_v3_0 on VI (Polaris), gfx_v6_0 / si_dma on
 * SI, gfx_v7_0 / cik_sdma on CIK (Hawaii), gfx_v9_0 / sdma_v4_0 on SOC15
 * (Vega 20). The kext calls only these;
 * every family choice it would otherwise make is here. UVD is uvd_v6_0 on
 * VI and uvd_v7_0 on Vega 20, behind frb_uvd.h's calls (frb_uvd_ring,
 * frb_uvd_ring_begin_use, the idle work), which the video sessions use on
 * either; uvd_v3_1 on SI is ported but not yet run on a card. A block not
 * ported on a family answers NULL / -ENODEV.
 */
static inline int frb_is_si(const struct frb_device *adev)
{
	return adev->family == AMDGPU_FAMILY_SI;
}
/* Sea Islands (CIK: Bonaire, Hawaii): gmc_v7_0, gfx_v7_0, cik_sdma, dce_v8_0, CI DPM */
static inline int frb_is_cik(const struct frb_device *adev)
{
	return adev->family == AMDGPU_FAMILY_CI;
}
struct frb_ring *frb_gfx_ring(struct frb_device *adev);			/* NULL unless the ring is up */
struct frb_ring *frb_sdma_ring(struct frb_device *adev, int instance);	/* NULL unless the ring is up */
/* the IB padding of the ring's engine (sdma_v3_0 / sdma_v4_0_ring_pad_ib: NOPs to 8 dwords) */
void frb_sdma_pad_ib(struct frb_ring *ring, struct frb_ib *ib);
/* amdgpu_copy_buffer / amdgpu_ttm_fill_mem on SDMA ring `instance`, polled; GPU addresses */
int frb_sdma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset, uint64_t dst_offset,
			 uint32_t byte_count);
int frb_sdma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data, uint64_t dst_addr,
			 uint32_t byte_count);
/* the GFX block's configuration for the clients (frb_gfx_v8_0_get_info / frb_gfx_v9_0_get_info) */
int frb_gfx_get_info(struct frb_device *adev, struct frb_gfx_info *info);
/*
 * amdgpu_asic_read_register: vi_read_register on VI, soc15_read_register on
 * SOC15 (soc15_allowed_read_registers, GB_ADDR_CONFIG and DB_DEBUG2 from
 * gfx_v9_0's copies). By dword index; 0 and the value, or -EINVAL.
 */
int frb_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
		      uint32_t *value);
/* amdgpu_asic_get_xclk: vi_get_xclk / soc15_get_xclk, 10 kHz (the GPU counter's clock) */
uint32_t frb_get_xclk(struct frb_device *adev);
/* GRBM_STATUS, CP_STAT and CP_STALLED_STAT1 for a hang report (VI or SOC15 offsets) */
void frb_gfx_hang_status(struct frb_device *adev, uint32_t *grbm_status, uint32_t *cp_stat,
			 uint32_t *cp_stalled_stat1);
/*
 * the UVD block this driver drives: uvd_v3_1 on SI, uvd_v6_0 on VI,
 * uvd_v7_0 on Vega 20; none on CIK yet (uvd_v4_2) or another SOC15 chip
 */
static inline int frb_has_uvd(const struct frb_device *adev)
{
	/* SI's uvd_v3_1, CIK's uvd_v4_2, VI's uvd_v6_0, Vega 20's uvd_v7_0 */
	return !frb_is_soc15(adev) || adev->asic_type == CHIP_VEGA20;
}

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
extern const struct frb_ip_reset_funcs frb_gfx_v9_0_reset_funcs;	/* SOC15 */
extern const struct frb_ip_reset_funcs frb_sdma_v4_0_reset_funcs;
void frb_gfx_v8_0_ring_soft_recovery(struct frb_ring *ring, unsigned vmid);
void frb_gfx_v9_0_ring_soft_recovery(struct frb_ring *ring, unsigned vmid);

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
void frb_soc15_recover_snapshot(struct frb_device *adev, struct frb_recover_report *rep);	/* frb_engines.c */

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
void frb_atombios_survey_strings(struct frb_device *adev);	/* part number, version, date */
/* amdgpu_atomfirmware.c (frb_atomfirmware.c; frb_soc15.h has the rest) */
void frb_atomfirmware_scratch_regs_init(struct frb_device *adev);
int frb_atomfirmware_allocate_fb_scratch(struct frb_device *adev);
int frb_device_asic_init(struct frb_device *adev);

/* vi.c (frb_vi.c) */
uint32_t frb_vi_smc_rreg(struct frb_device *adev, uint32_t reg);	/* vi_smc_rreg (frb_device.c) */
void frb_vi_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
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
/*
 * amd_shared.h: the clock- and power-gating flags (adev->cg_flags /
 * pg_flags): the ones vi_common_early_init sets for Polaris 12 that a ported
 * block reads, si_common_early_init's for SI, and soc15_common_early_init's
 * for Vega 20 (GC 9.4.0), which the SOC15 clock gating reads
 * (frb_soc15_set_cg_state)
 */
#define AMD_CG_SUPPORT_GFX_MGCG		(1ULL << 0)
#define AMD_CG_SUPPORT_GFX_MGLS		(1ULL << 1)
#define AMD_CG_SUPPORT_GFX_CGCG		(1ULL << 2)
#define AMD_CG_SUPPORT_GFX_CGLS		(1ULL << 3)
#define AMD_CG_SUPPORT_GFX_CGTS		(1ULL << 4)
#define AMD_CG_SUPPORT_GFX_CGTS_LS	(1ULL << 5)
#define AMD_CG_SUPPORT_GFX_CP_LS	(1ULL << 6)
#define AMD_CG_SUPPORT_GFX_RLC_LS	(1ULL << 7)
#define AMD_CG_SUPPORT_MC_LS		(1ULL << 8)
#define AMD_CG_SUPPORT_MC_MGCG		(1ULL << 9)
#define AMD_CG_SUPPORT_SDMA_LS		(1ULL << 10)
#define AMD_CG_SUPPORT_SDMA_MGCG	(1ULL << 11)
#define AMD_CG_SUPPORT_BIF_LS		(1ULL << 12)
#define AMD_CG_SUPPORT_UVD_MGCG		(1ULL << 13)
#define AMD_CG_SUPPORT_VCE_MGCG		(1ULL << 14)
#define AMD_CG_SUPPORT_HDP_LS		(1ULL << 15)
#define AMD_CG_SUPPORT_HDP_MGCG		(1ULL << 16)
#define AMD_CG_SUPPORT_ROM_MGCG		(1ULL << 17)
#define AMD_CG_SUPPORT_DRM_LS		(1ULL << 18)
#define AMD_CG_SUPPORT_BIF_MGCG		(1ULL << 19)
#define AMD_CG_SUPPORT_GFX_3D_CGCG	(1ULL << 20)
#define AMD_CG_SUPPORT_GFX_3D_CGLS	(1ULL << 21)
#define AMD_CG_SUPPORT_DRM_MGCG		(1ULL << 22)
#define AMD_CG_SUPPORT_DF_MGCG		(1ULL << 23)
#define AMD_PG_SUPPORT_GFX_PG		(1 << 0)
#define AMD_PG_SUPPORT_GFX_SMG		(1 << 1)
#define AMD_PG_SUPPORT_GFX_DMG		(1 << 2)
#define AMD_PG_SUPPORT_UVD		(1 << 3)
#define AMD_PG_SUPPORT_VCE		(1 << 4)
#define AMD_PG_SUPPORT_CP		(1 << 5)
#define AMD_PG_SUPPORT_GDS		(1 << 6)
#define AMD_PG_SUPPORT_RLC_SMU_HS	(1 << 7)

/* si.c (frb_si.c): the PCIE port window, and stage smu on SI */
uint32_t frb_si_pciep_rreg(struct frb_device *adev, uint32_t reg);
void frb_si_pciep_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
int frb_si_smc_stage(struct frb_device *adev);
/* gfx_v6_0_constants_init's GB_ADDR_CONFIG and its DMIF/HDP/DMA copies (stage dce runs it first) */
int frb_si_gfx_addr_config(struct frb_device *adev);

/* gmc_v6_0.c (frb_gmc_v6_0.c) */
int frb_gmc_v6_0_sw_init(struct frb_device *adev);
int frb_gmc_v6_0_hw_init(struct frb_device *adev);

/* cik.c (frb_cik.c): stage smu on CIK, the reference clock */
int frb_cik_smc_stage(struct frb_device *adev);
uint32_t frb_cik_get_xclk(struct frb_device *adev);	/* cik_get_xclk, 10 kHz */
/* gfx_v7_0_constants_init's GB_ADDR_CONFIG and its HDP/DMIF/SDMA copies (stage dce runs it first, as on SI) */
int frb_cik_gfx_addr_config(struct frb_device *adev);
/* cik_srbm_select: the ME/pipe/queue/VMID instance of the SRBM-indexed registers (SH_MEM_*, SDMA0_GFX_VIRTUAL_ADDR ...) */
void frb_cik_srbm_select(struct frb_device *adev, uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid);

/* gmc_v7_0.c (frb_gmc_v7_0.c) */
int frb_gmc_v7_0_sw_init(struct frb_device *adev);
int frb_gmc_v7_0_hw_init(struct frb_device *adev);
void frb_gmc_v7_0_set_clockgating_state(struct frb_device *adev, int gate);

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
/* GMC 9's memory type in bits 58:57 (AMDGPU_PTE_MTYPE_VG10); vega10_enum.h MTYPE_UC = 3 */
#define AMDGPU_PTE_MTYPE_VG10(mtype)	((uint64_t)(mtype) << 57)
/*
 * The flags a GTT page gets in the GART (amdgpu_ttm_tt_pte_flags): on SOC15
 * gmc_v9_0_gart_init's gart_pte_flags join them (MTYPE UC, EXECUTABLE). VI
 * keeps FRB_PTE_GTT as Polaris runs it.
 */
static inline uint64_t frb_gart_gtt_flags(const struct frb_device *adev)
{
	return frb_is_soc15(adev) ? FRB_PTE_GTT | AMDGPU_PTE_MTYPE_VG10(3) | AMDGPU_PTE_EXECUTABLE : FRB_PTE_GTT;
}
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
