/*
 * The interface between Firebrand.kext and its control tool (tools/frbctl.c):
 * the service class, the client type, the method numbers, the memory types
 * and the info structure. Administrator only, like RadeonNI's 'rdn2'.
 *
 * One exception: the status client ('frbs', FRB_UC_TYPE_STATUS), which any
 * user may open and which offers FRB_UC_STATUS alone: a read-only snapshot
 * for monitors (tools/frb_status.c: frbctl report, Firebrand Monitor).
 * Everything that changes the card stays on the administrator's 'frb1'.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_USER_H
#define FRB_USER_H

#include <stdint.h>

#define FRB_UC_SERVICE_CLASS	"Firebrand"
#define FRB_UC_FB_CLASS		"FirebrandFramebuffer"
#define FRB_UC_TYPE		0x66726231	/* 'frb1' */
#define FRB_UC_TYPE_STATUS	0x66726273	/* 'frbs': any user; one method, FRB_US_STATUS */

/*
 * The 'frbs' client's own method numbers: its public interface, which does
 * not move when 'frb1' gains methods (FRB_UC_STATUS's number does).
 */
#define FRB_US_STATUS		0	/* struct out: struct frb_user_status */

#define FRB_PIPE_CRC	0x80000000u

enum {
	FRB_UC_GET_INFO = 0,	/* struct out: struct frb_user_info */
	FRB_UC_REG_READ,	/* in: dword register index; out: value */
	FRB_UC_REG_WRITE,	/* in: index, value (only with frb_regwrite=1 / FirebrandAllowRegWrite) */
	FRB_UC_SMC_READ,	/* in: SMC address; out: value (through SMC_IND_INDEX_11) */
	FRB_UC_RUN_STAGE,	/* in: stage; out: errno-style result */
	FRB_UC_VRAM_READ,	/* in: BAR0 offset; out: the little-endian word, as a number */
	FRB_UC_TRACE,		/* in: 1 = start a fresh register trace, 0 = stop; out: entries */
	FRB_UC_DISPLAY_INFO,	/* struct out: struct frb_user_display (stage dce done) */
	FRB_UC_EDID,		/* in: connector; struct out: its EDID (up to 512 bytes) */
	FRB_UC_DISPLAY_MODE,	/* in: connector, width, height, refresh Hz, max link rate kHz; out: result.
				   width 0 = the EDID's preferred mode; rate 0 = the sink's */
	FRB_UC_DISPLAY_OFF,	/* out: result */
	FRB_UC_DISPLAY_FILL,	/* in: pattern (FRB_PATTERN_*), arg0, arg1; out: result */
	FRB_UC_DETECT,		/* out: the first connected DP connector, or -1 (re-runs detect) */
	FRB_UC_DPCD_READ,	/* in: connector, DPCD address; out: 4 bytes from there, first in the top byte */
	FRB_UC_SINK_CRC,	/* in: connector; out: result, CRC red/Cr, green/Y, blue/Cb.
				   connector FRB_PIPE_CRC: the CRTC's own CRC (CRTC0, DCE 12) instead of the sink's */
	FRB_UC_ENGINE_TEST,	/* in: engine (FRB_ENGINE_*), test (FRB_TEST_*); out: result */
	FRB_UC_FB_DETACH,	/* out: result. The framebuffer takes no new IOGraphics clients from here
				   on, so the window server cannot reopen it and the kext can unload */
	FRB_UC_RECOVER,		/* in: blocks to reset regardless (FRB_RECOVER_*: 1 gfx, 2 sdma; 0 the busy
				   ones); out: result, blocks hung, still hung | rings failed << 8,
				   soft-recovered, ms */
	FRB_UC_CURSOR,		/* in: FRB_CURSOR_OP_*, x, y; out: result (the hardware cursor, for tests) */
	FRB_UC_DPM,		/* in: FRB_DPM_OP_*; struct out: struct frb_user_dpm (after the op) */
	FRB_UC_LUT,		/* struct out: struct frb_user_lut (the CRTC's LUT, as stored and as the hardware holds it) */
	FRB_UC_AUDIO,		/* in: FRB_AUDIO_OP_*, arg; out: result, active DIG, active codec pin, flags
				   (bit 0 packets on, bit 1 the sink has audio, 15:8 the forced pin or 0xff) */
	FRB_UC_CONFIG,		/* in: function, offset, write, value; out: the dword read (after the write) */
	FRB_UC_SMN,		/* in: SMN address, write, value; out: the dword read (after the write). SOC15 only;
				   a write only with FirebrandAllowRegWrite */
	FRB_UC_FUNCTIONS,	/* in: FRB_FN_OP_*, arg; out: result (IOReturn or -errno), function 0's header
				   type, function 1's vendor/device dword, nubs the bridge published */
	FRB_UC_LUT_SET,		/* in: FRB_LUT_OP_*; out: result. A test aid: the sink CRC of a known
				   frame through an identity LUT, whatever ramp the window server loaded */
	FRB_UC_STATUS,		/* struct out: struct frb_user_status. Reads only; on an 'frbs' client
				   it is method FRB_US_STATUS (0) instead */
	FRB_UC_UNLOCK,		/* in: 1 unlock, 0 lock (an untested card past display-only, for frbctl stage N
				   now; frbctl unlock also writes FirebrandUnlock for the next start); out: the
				   stage the card may run to now */
	FRB_UC_FW_CHECK,	/* struct out: struct frb_user_fwcheck (the kext's microcode against every
				   matched device id's needs; reads only, the card untouched) */
	FRB_UC_METHOD_COUNT
};

/*
 * FRB_UC_STATUS: what a monitor shows of a card, one structure for every
 * family. A field means something only when its FRB_ST_* bit is set in
 * valid: a family (or a card, or a state) without the thing leaves the bit
 * clear, and a monitor shows "not available" rather than a number. The
 * identity, the stage, the flags and the memory sizes are always filled.
 *
 * The sensors are a sample the driver takes at most every 500 ms, whoever
 * asks (sensor_age_ms says how old), under the lock that serializes its own
 * SMU traffic, with cheap reads only (frb_pm_read_sensors in
 * amdgpu/frb_pm.h); after a read the SMU did not acknowledge it takes none
 * for 30 s. Nothing here wakes a gated block: UVD's state is the driver's
 * bookkeeping, not its registers. Only appended to in later versions; a
 * client checks size.
 */
#define FRB_STATUS_VERSION	2	/* 2: support, stage_cap, stage_failed_at, vbios_*, stage_error */

enum {
	FRB_ST_CLOCKS		= 1u << 0,	/* gfx_mhz, mem_mhz */
	FRB_ST_SOC_CLOCKS	= 1u << 1,	/* soc_mhz, fclk_mhz (Vega 20) */
	FRB_ST_LEVELS		= 1u << 2,	/* gfx_levels, mem_levels: the DPM ladders */
	FRB_ST_TEMP		= 1u << 3,	/* temp_mc: the GPU (amdgpu's edge / temp1) */
	FRB_ST_TEMP_HOTSPOT	= 1u << 4,	/* temp_hotspot_mc (junction) */
	FRB_ST_TEMP_MEM		= 1u << 5,	/* temp_mem_mc (HBM) */
	FRB_ST_POWER		= 1u << 6,	/* power_mw */
	FRB_ST_POWER_LIMIT	= 1u << 7,	/* power_limit_w */
	FRB_ST_LOAD		= 1u << 8,	/* gpu_load, mem_load */
	FRB_ST_VDDGFX		= 1u << 9,	/* vddgfx_mv */
	FRB_ST_FAN		= 1u << 10,	/* fan_rpm, fan_pwm (only a card with a fan) */
	FRB_ST_THROTTLE		= 1u << 11,	/* throttler: SMU11 ThrottlerStatus bits */
	FRB_ST_SMU		= 1u << 12,	/* smu_version (0: not read), features_kind, features_lo/hi */
	FRB_ST_GATING		= 1u << 13,	/* gating, cg_flags_lo/hi */
	FRB_ST_UVD		= 1u << 14,	/* uvd_* */
	FRB_ST_DISPLAY		= 1u << 15,	/* num_connectors, conn[] (stage dce done) */
	FRB_ST_AUDIO		= 1u << 16,	/* audio_* (DP audio) */
	FRB_ST_LINK		= 1u << 17,	/* link_*: the GPU function's own PCIe link */
	FRB_ST_SLOT_LINK	= 1u << 18,	/* slot_*: the card's link to the host (see slot_vendor) */
	FRB_ST_VRAM_HEAP	= 1u << 19,	/* vram_heap_kb, vram_heap_used_kb (the accelerator's heaps) */
	FRB_ST_GART		= 1u << 20	/* gart_kb, gart_used_kb */
};

/* flags: the switches the card runs with */
enum {
	FRB_SF_BRINGUP		= 1u << 0,	/* FirebrandBringUp: stages past the card's verified one allowed */
	FRB_SF_FRAMEBUFFER	= 1u << 1,	/* FirebrandFramebuffer owns the card (the desktop's display) */
	FRB_SF_ACCEL		= 1u << 2,	/* FirebrandAccel: the accelerator was asked for */
	FRB_SF_DPM		= 1u << 3,	/* FirebrandDPM: power management asked for */
	FRB_SF_AUDIO		= 1u << 4,	/* FirebrandAudio: DP audio asked for */
	FRB_SF_GATING		= 1u << 5,	/* FirebrandGating (SOC15 clock gating) asked for */
	FRB_SF_STOPPED		= 1u << 6,	/* the driver is stopping: identity only */
	FRB_SF_UNTESTED		= 1u << 7,	/* no verified row: an untested card (support FRB_SUPPORT_UNTESTED) */
	FRB_SF_UNLOCKED		= 1u << 8	/* the unlock is on (FirebrandUnlock, frb_unlock=1, frbctl unlock) */
};

/* support: what Firebrand knows of this card (the untested-card policy) */
enum {
	FRB_SUPPORT_VERIFIED = 0,	/* a family-table row: verified on this card, to row_max_stage */
	FRB_SUPPORT_UNTESTED,		/* a ported family, not this card: display-only unless unlocked */
	FRB_SUPPORT_NOT_PORTED		/* recognised (Vega 10/12), no port: identified only */
};

/* dpm_state */
enum {
	FRB_DPM_STATE_OFF = 0,		/* not asked for (FirebrandDPM false), or the family has none yet */
	FRB_DPM_STATE_ON,		/* running */
	FRB_DPM_STATE_FAILED,		/* its init failed (dpm_failed) */
	FRB_DPM_STATE_PENDING		/* asked for; the stages have not reached it */
};

/* features_kind: whose bits features_lo/hi are */
enum {
	FRB_FEATURES_NONE = 0,
	FRB_FEATURES_SMU7 = 7,		/* ixFEATURE_STATUS (Polaris): SCLK_DPM_ON 0x1, MCLK_DPM_ON 0x2, ... */
	FRB_FEATURES_SMU11 = 11		/* the SMU11 feature bits (Vega 20): FEATURE_DPM_PREFETCHER_BIT 0, ... */
};

/* uvd_state */
enum {
	FRB_UVD_STATE_NONE = 0,		/* no decoder: stage uvd not run, or the family has none here */
	FRB_UVD_STATE_IDLE,		/* up, VCPU stopped and clocks down (amdgpu's idle work ran) */
	FRB_UVD_STATE_RUNNING		/* VCPU running (begin_use; DPM_UVD on with DPM) */
};
#define FRB_UVD_FLAG_POISONED	1u	/* uvd_flags: a decode timed out; no sessions until a reload */

struct frb_user_status_conn {
	uint32_t dp_rate_khz;		/* active DP: the link rate as frbctl display gives it (162000, 270000 HBR,
					   540000 HBR2, 810000 HBR3); per lane */
	uint32_t dp_max_rate_khz;	/* a DP sink: DPCD MAX_LINK_RATE x 27000, 0 none */
	uint32_t pixel_khz;		/* active: the mode's pixel clock */
	uint16_t width, height;		/* active: the mode */
	uint16_t refresh_chz;		/* active: refresh in 1/100 Hz */
	uint8_t type;			/* the connector object type: 0x13 DisplayPort, 0x14 eDP, 0x0c HDMI-A,
					   0x01/0x02 DVI-I, 0x03/0x04 DVI-D */
	uint8_t connected;		/* hot-plug sense, read now */
	uint8_t active;			/* driven by CRTC0 (the mode fields) */
	uint8_t bpc;			/* active: bits per colour of the stream, 0 unknown */
	uint8_t dp_lanes;		/* active DP: lanes trained */
	uint8_t dp_max_lanes;		/* a DP sink: DPCD MAX_LANE_COUNT */
	char sink[14];			/* the EDID's monitor name, "" none */
	uint8_t reserved[2];
};

struct frb_user_status {
	uint32_t version;		/* FRB_STATUS_VERSION */
	uint32_t size;			/* bytes the driver filled: sizeof its struct frb_user_status */
	uint32_t valid;			/* FRB_ST_* */
	uint32_t sensor_age_ms;		/* how old the sensor sample is */
	/* identity, stage, switches: always */
	uint16_t vendor, device, subsys_vendor, subsys_device;
	uint32_t revision;
	uint32_t asic_type;		/* enum amd_asic_type */
	uint32_t family;		/* AMDGPU_FAMILY_*: 110 SI, 120 CI, 130 VI, 141 AI (Vega) */
	uint32_t stage_done;		/* 0 survey, 1 post, 2 gmc, 3 smu, 4 dce, 5 gfx, 6 sdma, 7 uvd */
	uint32_t row_max_stage;		/* the family table's verified stage for this card; 0xff: no row */
	int32_t stage_failed;		/* errno of the stage that failed, 0 */
	uint32_t flags;			/* FRB_SF_* */
	/* power management */
	uint32_t dpm_state;		/* FRB_DPM_STATE_* */
	int32_t dpm_failed;		/* the DPM init's errno */
	uint32_t smu_version;		/* FRB_ST_SMU: major << 16 | minor << 8 | patch, 0 not read */
	uint32_t features_kind;		/* FRB_FEATURES_* */
	uint32_t features_lo, features_hi;	/* the SMU features running now */
	uint32_t gating;		/* FRB_ST_GATING: bit 0 asked for, bit 1 the blocks are gated now */
	uint32_t cg_flags_lo, cg_flags_hi;	/* AMD_CG_SUPPORT_* in force */
	/* sensors */
	uint32_t gfx_mhz, mem_mhz;	/* FRB_ST_CLOCKS (gfx: the SMU's average on Vega 20) */
	uint32_t soc_mhz, fclk_mhz;	/* FRB_ST_SOC_CLOCKS */
	int32_t temp_mc, temp_hotspot_mc, temp_mem_mc;	/* millidegrees C */
	uint32_t power_mw;		/* FRB_ST_POWER: package (Polaris) / socket (Vega 20) power */
	uint32_t power_limit_w;		/* FRB_ST_POWER_LIMIT */
	uint32_t gpu_load, mem_load;	/* FRB_ST_LOAD, % */
	uint32_t vddgfx_mv;		/* FRB_ST_VDDGFX */
	uint32_t fan_rpm, fan_pwm;	/* FRB_ST_FAN; pwm 0-255 */
	uint32_t throttler;		/* FRB_ST_THROTTLE */
	uint32_t gfx_level_count, mem_level_count;	/* FRB_ST_LEVELS */
	uint16_t gfx_levels[16], mem_levels[16];	/* MHz, lowest first */
	/* the video decoder */
	uint32_t uvd_state;		/* FRB_ST_UVD: FRB_UVD_STATE_* */
	uint32_t uvd_instances;		/* started (not harvested) */
	uint32_t uvd_sessions;		/* decode sessions open ('rdnv' clients) */
	uint32_t uvd_fw_version;	/* (major << 24) | (minor << 16) | (family << 8) */
	uint32_t uvd_flags;		/* FRB_UVD_FLAG_* */
	/* PCIe: speed 1 = 2.5 GT/s, 2 = 5, 3 = 8, 4 = 16; width in lanes */
	uint16_t link_speed, link_width, link_max_speed, link_max_width;	/* FRB_ST_LINK */
	uint16_t slot_speed, slot_width, slot_max_speed, slot_max_width;	/* FRB_ST_SLOT_LINK */
	uint16_t slot_vendor, slot_device;	/* the port slot_* is read from: an in-package switch's
						   upstream port (Vega 10/20: 1002:14a0), else the GPU itself */
	/* memory */
	uint32_t vram_mb;		/* the card's */
	uint32_t vram_visible_mb;	/* the CPU's window (BAR0) */
	uint32_t vram_width;		/* bits */
	uint32_t vram_heap_kb, vram_heap_used_kb;	/* FRB_ST_VRAM_HEAP: what clients allocate from */
	uint32_t gart_kb, gart_used_kb;	/* FRB_ST_GART */
	/* displays (CRTC0 drives one) */
	uint32_t num_connectors;	/* FRB_ST_DISPLAY */
	struct frb_user_status_conn conn[6];
	/* DP audio */
	uint32_t audio_on;		/* FRB_ST_AUDIO: the active stream carries audio */
	int32_t audio_endpoint;		/* the Azalia endpoint (codec pin) it feeds, -1 none */
	int32_t audio_dig;		/* the DIG / stream encoder, -1 none */
	uint32_t audio_sink;		/* the sink takes audio (its EDID says so) */
	/* names */
	char asic[16];			/* "VEGA20", "POLARIS12", ... */
	char board[64];			/* the family table's row, "" none */
	char driver_version[16];	/* the kext's version */
	char build[48];			/* the build's source (git commit) and date, "" unknown */
	/* version 2: the untested-card policy, and what an issue report needs */
	uint32_t support;		/* FRB_SUPPORT_* */
	uint32_t stage_cap;		/* the stage the card may run to now: its row's, display-only (dce) for an
					   untested card, every stage when unlocked */
	uint32_t stage_failed_at;	/* the stage that failed (with stage_failed) */
	char vbios_pn[48];		/* the VBIOS part number ("113-C6750200-100") */
	char vbios_ver[24];		/* its version string, "" none */
	char vbios_date[20];		/* its build date ("20YY/MM/DD HH:MM") */
	char stage_error[200];		/* the failed stage's error lines, "" none */
	uint32_t boot_gfx_mhz, boot_mem_mhz;	/* the VBIOS's boot clocks (FirmwareInfo), 0 unknown: what the card
						   runs at without power management */
};

/* FRB_UC_FW_CHECK */
struct frb_user_fwcheck {
	uint32_t version;		/* 1 */
	uint32_t store_files;		/* distinct images in the store */
	uint32_t store_aliases;		/* names whose bytes are under another name */
	uint32_t devices;		/* device ids walked (pciidlist) */
	uint32_t lookups;		/* file lookups made (device x revision x file) */
	uint32_t missing;		/* lookups that found nothing */
	uint32_t missing_listed;
	uint32_t verified_rows, verified_missing;	/* the family table's verified cards, their misses */
	char source[40];		/* where the store came from */
	char missing_names[8][32];	/* the first distinct misses */
};

/* FRB_UC_FUNCTIONS: the device's PCI functions (the hidden HD Audio function; bring-up) */
enum {
	FRB_FN_OP_STATUS = 0,	/* the survey's registers and the straps to the log; nothing written */
	FRB_FN_OP_SCAN		/* the bridge's PCIScanFunctions; arg: its flags (bit 0: a link reset first,
				   only with FirebrandAllowRegWrite) */
};

/* FRB_UC_AUDIO (DP audio, FirebrandAudio) */
enum {
	FRB_AUDIO_OP_STATUS = 0,
	FRB_AUDIO_OP_PIN,	/* arg: the codec pin to use from now on, -1 = RadeonNI's choice; the stream's
				   audio is set up again on it */
	FRB_AUDIO_OP_RERUN,	/* the stream's audio set up again (off, mode set, on) */
	FRB_AUDIO_OP_OFF,	/* the DIG's audio packets and the pin's presence off */
	FRB_AUDIO_OP_ON
};

/* FRB_UC_LUT_SET */
enum {
	FRB_LUT_OP_LINEAR = 0,	/* the CRTC's LUT an identity ramp (hw_init's i << 8); the ramp it held kept */
	FRB_LUT_OP_RESTORE	/* the ramp kept by FRB_LUT_OP_LINEAR back */
};

/* FRB_UC_CURSOR */
enum {
	FRB_CURSOR_OP_OFF = 0,	/* no cursor */
	FRB_CURSOR_OP_TEST,	/* the test image at x, y: 128x128, left half opaque ramps
				   (A ff, R 4x, G 2y, B 3(x+y), low bytes), right half transparent */
	FRB_CURSOR_OP_MOVE,	/* shown at x, y */
	FRB_CURSOR_OP_HIDE,
	FRB_CURSOR_OP_SOLID	/* x = one ARGB word for the whole 128x128 image, y = how many rows of it
				   (the rest transparent); where it was, shown */
};

/* FRB_UC_LUT: what the LUT was last given (the gamma ramp, or at 8 bpp the colour table through it) */
struct frb_user_lut {
	uint32_t bpp;			/* the screen's depth: 8, 16, 32 (0: no screen) */
	int32_t hw_read;		/* 0: hw[] read back from DC_LUT_30_COLOR; else -errno (no CRTC running) */
	uint16_t store[3][256];		/* red, green, blue, 16-bit */
	uint32_t hw[256];		/* DC_LUT_30_COLOR: red 29:20, green 19:10, blue 9:0 */
};

/* FRB_UC_DPM: power_dpm_force_performance_level and the sensors */
enum {
	FRB_DPM_OP_STATUS = 0,	/* nothing; the readout */
	FRB_DPM_OP_HIGH,	/* smu7_force_dpm_highest */
	FRB_DPM_OP_LOW,		/* smu7_force_dpm_lowest */
	FRB_DPM_OP_AUTO,	/* smu7_unforce_dpm_levels */
	FRB_DPM_OP_ENABLE,	/* run the DPM init now (stage sdma done, DPM not yet on) */
	FRB_DPM_OP_DISABLE,	/* hw_fini: boot state, DPM off */
	FRB_DPM_OP_GATE_ON,	/* SOC15: amdgpu_device_set_cg_state(GATE) on the blocks that are up */
	FRB_DPM_OP_GATE_OFF,	/* SOC15: amdgpu_device_set_cg_state(UNGATE) */
	FRB_DPM_OP_FCLK_LOW,	/* Vega 20: FCLK's soft minimum at its lowest level (frb_vega20_set_fclk_soft_min) */
	FRB_DPM_OP_FCLK_TOP	/* ... at its highest, where a display configuration leaves it */
};

struct frb_user_dpm {
	uint32_t version;		/* 1 */
	int32_t result;			/* of the op */
	int32_t enabled, failed;	/* DPM running; the init's errno */
	uint32_t forced_level;		/* AMD_DPM_FORCED_LEVEL_*: 1 auto, 4 low, 8 high */
	uint32_t sclk_count, mclk_count, pcie_count;
	uint32_t sclk_levels[8], mclk_levels[8];	/* 10 kHz */
	uint32_t sclk_mask, mclk_mask, pcie_mask;
	uint32_t cur_sclk, cur_mclk;	/* 10 kHz, the SMC's answer */
	int32_t cur_sclk_index, cur_mclk_index;
	uint32_t vddgfx_mv;
	int32_t temp_mc;		/* millidegrees */
	uint32_t fan_rpm, fan_pwm;	/* pwm 0-255 */
	uint32_t power_mw;
	uint32_t gpu_load, mem_load;	/* % */
	uint32_t pcie_gen, pcie_lanes;
	uint32_t smc_feature_status;
	uint32_t num_display, vrefresh, min_vblank_time;
	/* version 2: SOC15 (Vega 20, the SMU11's readout) */
	uint32_t soc15;			/* 1: the fields below are filled */
	uint32_t cur_socclk, cur_fclk;	/* 10 kHz */
	int32_t temp_edge, temp_hotspot, temp_hbm;	/* millidegrees */
	uint32_t gfx_count, uclk_count, soc_count;
	uint32_t gfx_levels[16], uclk_levels[16], soc_levels[16];	/* MHz */
	uint32_t smu_version;
	uint32_t gating;		/* bit 0: FirebrandGating; bit 1: gated now */
	uint32_t cg_flags_lo, cg_flags_hi;
};

/* clientMemoryForType */
enum {
	FRB_UC_MEMORY_TRACE = 1,	/* struct { uint32 reg (bit 31 = write); uint32 val } [], CPU order */
	FRB_UC_MEMORY_LOG = 2		/* the driver's log ring, text */
};

#define FRB_EDID_BYTES		512	/* FRB_EDID_MAX in amdgpu/frb_display.h */

/* test patterns for the scanout surface */
enum {
	FRB_PATTERN_BLACK = 0,
	FRB_PATTERN_BARS,	/* colour bars, ramps, a 1-pixel white border, red top-left square */
	FRB_PATTERN_GREY,	/* plain 0x9a9a9a */
	FRB_PATTERN_SOLID,	/* arg0 = 0xRRGGBB everywhere */
	FRB_PATTERN_DOT,	/* one white pixel at (arg0, arg1) on black */
	FRB_PATTERN_TOP,	/* the top arg0 lines white, the rest black */
	FRB_PATTERN_LEFT,	/* the left arg0 columns white, the rest black */
	FRB_PATTERN_SPLASH,	/* the boot splash: grey 0xBFBFBF, the Firebrand logo (shared/frb_splash.c); any depth */
	FRB_PATTERN_SMPTE,	/* libdrm's modetest "smpte" for XR24 (tests/util/pattern.c fill_smpte_rgb32), pixel
				   for pixel: the pattern of the Vega 20 capture's pipe and sink CRCs */
	FRB_PATTERN_SMPTE_CURSOR,	/* SMPTE with the cursor test image's opaque half (frbctl cursor test) drawn at
				   (arg0, arg1): the software reference for the hardware cursor's pipe CRC */
	FRB_PATTERN_COUNT
};

struct frb_user_connector {
	uint16_t connector_object_id, encoder_object_id;
	uint16_t devices, edid_len;
	uint8_t connector_type, encoder_id, linkb, i2c_id;
	int8_t hpd, dig_encoder;
	uint8_t sense, dp_sink_type;
	uint8_t dpcd[16];
	uint32_t dp_clock, dp_lane_count;
};

struct frb_user_display {
	uint32_t version;		/* 1 */
	uint32_t ready, num_connectors, num_crtc;
	int32_t active;			/* connector on CRTC0, -1 none */
	uint32_t default_dispclk, dp_extclk;	/* 10 kHz */
	uint32_t clock, flags;		/* the mode on CRTC0 */
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t adjusted_clock, fb_offset, fb_pitch;
	uint32_t line_time, watermark, lb_lead_lines;
	uint32_t crtc_control, crtc_status_frame_count;
	struct frb_user_connector conn[6];
	int32_t audio_dig;		/* DP audio: the DIG audio_ep feeds, -1 off */
	uint32_t audio_hot_plug, audio_speakers;	/* audio_ep's HOT_PLUG_CONTROL, CHANNEL_SPEAKER */
	int32_t audio_ep;		/* the codec pin (AZF0ENDPOINTn) DP audio takes, 0 when none */
};

struct frb_user_info {
	uint32_t version;		/* 1 */
	uint16_t vendor, device, subsys_vendor, subsys_device;
	uint32_t revision, rev_id, external_rev_id;
	uint32_t asic_type;
	uint32_t row_max_stage;		/* 0xff: no row */
	uint32_t stage_done;
	int32_t stage_failed;
	uint32_t asic_init_ms;
	uint32_t rmmio_size, aper_size;
	uint32_t vram_mb, vram_width;
	uint32_t vram_start_hi, vram_start_lo;
	uint32_t gart_start_hi, gart_start_lo;
	uint32_t gart_table_offset, mem_scratch_offset, toc_offset, smu_buffer_offset, fw_buf_offset;
	uint32_t dummy_hi, dummy_lo;
	uint32_t trace_len, trace_cap, trace_lost;
	uint32_t log_len, log_cap;
	uint32_t mc_fw_version;
	char board[64];
	char mc_fw[32];
};

#endif
