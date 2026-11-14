/*
 * The powerplay hardware manager's shared definitions, for Firebrand's port
 * of amdgpu's pm/powerplay on Polaris 12 (Linux 7.1.1, the kernel the E9173
 * capture ran; 7.0.2 differs only in the display-voltage request, see
 * frb_smu7_hwmgr.c). Taken from, in this order:
 *
 *   inc/hwmgr.h, inc/hardwaremanager.h, inc/power_state.h, hwmgr/hwmgr_ppt.h,
 *   inc/smumgr.h, hwmgr/smu_helper.h, hwmgr/process_pptables_v1_0.h,
 *   hwmgr/pp_psm.h, include/amd_shared.h (PP_FEATURE_MASK),
 *   include/kgd_pp_interface.h and include/dm_pp_interface.h (the enums and
 *   amd_pp_display_configuration).
 *
 * Firebrand trims, it does not change: struct pp_hwmgr keeps only the
 * members the Polaris paths use, and the two function tables
 * (pp_hwmgr_func, pp_smumgr_func) only the entries Polaris fills and
 * something calls. hwmgr->device and hwmgr->adev are both the frb_device.
 * There is no mutex (Firebrand's library is single-threaded by contract).
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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
#ifndef FRB_HWMGR_H
#define FRB_HWMGR_H

#include "frb_pp_linux.h"
#include "frb_pm.h"

struct pp_hwmgr;
struct pp_power_state;
struct pp_hw_power_state;

/* include/amd_shared.h */
#define AMD_MAX_USEC_TIMEOUT		1000000	/* 1000 ms */
enum PP_FEATURE_MASK {
	PP_SCLK_DPM_MASK = 0x1,
	PP_MCLK_DPM_MASK = 0x2,
	PP_PCIE_DPM_MASK = 0x4,
	PP_SCLK_DEEP_SLEEP_MASK = 0x8,
	PP_POWER_CONTAINMENT_MASK = 0x10,
	PP_UVD_HANDSHAKE_MASK = 0x20,
	PP_SMC_VOLTAGE_CONTROL_MASK = 0x40,
	PP_VBI_TIME_SUPPORT_MASK = 0x80,
	PP_ULV_MASK = 0x100,
	PP_ENABLE_GFX_CG_THRU_SMU = 0x200,
	PP_CLOCK_STRETCH_MASK = 0x400,
	PP_OD_FUZZY_FAN_CONTROL_MASK = 0x800,
	PP_SOCCLK_DPM_MASK = 0x1000,
	PP_DCEFCLK_DPM_MASK = 0x2000,
	PP_OVERDRIVE_MASK = 0x4000,
	PP_GFXOFF_MASK = 0x8000,
	PP_ACG_MASK = 0x10000,
	PP_STUTTER_MODE = 0x20000,
	PP_AVFS_MASK = 0x40000,
	PP_GFX_DCS_MASK = 0x80000,
};
/* amdgpu_drv.c amdgpu_pp_feature_mask's default (the capture's ppfeaturemask) */
#define FRB_PP_FEATURE_MASK_DEFAULT	0xfff7bfffu

/* include/uapi/drm/amdgpu_drm.h, amd_asic_type.h: the families and chips the
 * smu7 code names in its other arms */
#define AMDGPU_FAMILY_CI	120
#define AMDGPU_FAMILY_CZ	135
#define CHIP_BONAIRE		5
#define CHIP_HAWAII		8

/* include/linux/compiler_attributes.h */
#define fallthrough	do {} while (0)

/* include/kgd_pp_interface.h (amd_dpm_forced_level is in frb_pm.h) */
enum amd_pp_task {
	AMD_PP_TASK_DISPLAY_CONFIG_CHANGE,
	AMD_PP_TASK_ENABLE_USER_STATE,
	AMD_PP_TASK_READJUST_POWER_STATE,
	AMD_PP_TASK_COMPLETE_INIT,
	AMD_PP_TASK_MAX
};
enum PP_SMC_POWER_PROFILE {
	PP_SMC_POWER_PROFILE_UNKNOWN = -1,
	PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT = 0x0,
	PP_SMC_POWER_PROFILE_FULLSCREEN3D = 0x1,
	PP_SMC_POWER_PROFILE_POWERSAVING  = 0x2,
	PP_SMC_POWER_PROFILE_VIDEO        = 0x3,
	PP_SMC_POWER_PROFILE_VR           = 0x4,
	PP_SMC_POWER_PROFILE_COMPUTE      = 0x5,
	PP_SMC_POWER_PROFILE_CUSTOM       = 0x6,
};
/* include/kgd_pp_interface.h amd_pp_sensors, the ones smu7_read_sensor answers */
enum amd_pp_sensors {
	AMDGPU_PP_SENSOR_GFX_SCLK = 0,
	AMDGPU_PP_SENSOR_GFX_MCLK,
	AMDGPU_PP_SENSOR_GPU_LOAD,
	AMDGPU_PP_SENSOR_MEM_LOAD,
	AMDGPU_PP_SENSOR_GPU_TEMP,
	AMDGPU_PP_SENSOR_UVD_POWER,
	AMDGPU_PP_SENSOR_VCE_POWER,
	AMDGPU_PP_SENSOR_GPU_INPUT_POWER,
	AMDGPU_PP_SENSOR_GPU_AVG_POWER,
	AMDGPU_PP_SENSOR_VDDGFX,
};
enum pp_clock_type {
	PP_SCLK,
	PP_MCLK,
	PP_PCIE,
};
enum amd_pm_state_type {
	POWER_STATE_TYPE_DEFAULT,
	POWER_STATE_TYPE_POWERSAVE,
	POWER_STATE_TYPE_BATTERY,
	POWER_STATE_TYPE_BALANCED,
	POWER_STATE_TYPE_PERFORMANCE,
};

/* --- power_state.h ------------------------------------------------------- */

struct pp_hw_power_state {
	unsigned int magic;
};

enum PP_StateUILabel {
	PP_StateUILabel_None,
	PP_StateUILabel_Battery,
	PP_StateUILabel_MiddleLow,
	PP_StateUILabel_Balanced,
	PP_StateUILabel_MiddleHigh,
	PP_StateUILabel_Performance,
	PP_StateUILabel_BACO
};

enum PP_StateClassificationFlag {
	PP_StateClassificationFlag_Boot                = 0x0001,
	PP_StateClassificationFlag_Thermal             = 0x0002,
	PP_StateClassificationFlag_LimitedPowerSource  = 0x0004,
	PP_StateClassificationFlag_Rest                = 0x0008,
	PP_StateClassificationFlag_Forced              = 0x0010,
	PP_StateClassificationFlag_User3DPerformance   = 0x0020,
	PP_StateClassificationFlag_User2DPerformance   = 0x0040,
	PP_StateClassificationFlag_3DPerformance       = 0x0080,
	PP_StateClassificationFlag_ACOverdriveTemplate   = 0x0100,
	PP_StateClassificationFlag_Uvd                 = 0x0200,
	PP_StateClassificationFlag_3DPerformanceLow    = 0x0400,
	PP_StateClassificationFlag_ACPI                = 0x0800,
	PP_StateClassificationFlag_HD2                 = 0x1000,
	PP_StateClassificationFlag_UvdHD               = 0x2000,
	PP_StateClassificationFlag_UvdSD               = 0x4000,
	PP_StateClassificationFlag_UserDCPerformance    = 0x8000,
	PP_StateClassificationFlag_DCOverdriveTemplate   = 0x10000,
	PP_StateClassificationFlag_BACO                  = 0x20000,
	PP_StateClassificationFlag_LimitedPowerSource_2  = 0x40000,
	PP_StateClassificationFlag_ULV                   = 0x80000,
	PP_StateClassificationFlag_UvdMVC               = 0x100000,
};

struct PP_StateClassificationBlock {
	enum PP_StateUILabel         ui_label;
	enum PP_StateClassificationFlag  flags;
	int                          bios_index;
	bool                      temporary_state;
	bool                      to_be_deleted;
};

struct PP_StatePcieBlock {
	unsigned int lanes;
};

struct PP_StateDisplayBlock {
	bool              disableFrameModulation;
	bool              limitRefreshrate;
	int                  explicitRefreshrate;
	int                  edidRefreshrateIndex;
	bool              enableVariBright;
};

#define PP_TEMPERATURE_UNITS_PER_CENTIGRADES 1000

struct PP_TemperatureRange {
	int min;
	int max;
	int edge_emergency_max;
	int hotspot_min;
	int hotspot_crit_max;
	int hotspot_emergency_max;
	int mem_min;
	int mem_crit_max;
	int mem_emergency_max;
	int sw_ctf_threshold;
};

struct PP_StateValidationBlock {
	bool singleDisplayOnly;
	bool disallowOnDC;
	uint8_t supportedPowerLevels;
};

struct PP_UVD_CLOCKS {
	uint32_t VCLK;
	uint32_t DCLK;
};

struct pp_power_state {
	uint32_t                            id;
	struct PP_StateClassificationBlock         classification;
	struct PP_StateValidationBlock             validation;
	struct PP_StatePcieBlock                   pcie;
	struct PP_StateDisplayBlock                display;
	struct PP_TemperatureRange                 temperatures;
	struct PP_UVD_CLOCKS                       uvd_clocks;
	struct pp_hw_power_state  hardware;
};

/* --- hardwaremanager.h ---------------------------------------------------- */

struct phm_fan_speed_info {
	uint32_t min_percent;
	uint32_t max_percent;
	uint32_t min_rpm;
	uint32_t max_rpm;
	bool supports_percent_read;
	bool supports_percent_write;
	bool supports_rpm_read;
	bool supports_rpm_write;
};

enum PHM_AutoThrottleSource {
	PHM_AutoThrottleSource_Thermal,
	PHM_AutoThrottleSource_External
};
typedef enum PHM_AutoThrottleSource PHM_AutoThrottleSource;

/* the capability bits, in Linux's order (only names and distinctness matter) */
enum phm_platform_caps {
	PHM_PlatformCaps_AtomBiosPpV1 = 0,
	PHM_PlatformCaps_PowerPlaySupport,
	PHM_PlatformCaps_ACOverdriveSupport,
	PHM_PlatformCaps_BacklightSupport,
	PHM_PlatformCaps_ThermalController,
	PHM_PlatformCaps_BiosPowerSourceControl,
	PHM_PlatformCaps_DisableVoltageTransition,
	PHM_PlatformCaps_DisableEngineTransition,
	PHM_PlatformCaps_DisableMemoryTransition,
	PHM_PlatformCaps_DynamicPowerManagement,
	PHM_PlatformCaps_EnableASPML0s,
	PHM_PlatformCaps_EnableASPML1,
	PHM_PlatformCaps_OD5inACSupport,
	PHM_PlatformCaps_OD5inDCSupport,
	PHM_PlatformCaps_SoftStateOD5,
	PHM_PlatformCaps_NoOD5Support,
	PHM_PlatformCaps_ContinuousHardwarePerformanceRange,
	PHM_PlatformCaps_ActivityReporting,
	PHM_PlatformCaps_EnableBackbias,
	PHM_PlatformCaps_OverdriveDisabledByPowerBudget,
	PHM_PlatformCaps_ShowPowerBudgetWarning,
	PHM_PlatformCaps_PowerBudgetWaiverAvailable,
	PHM_PlatformCaps_GFXClockGatingSupport,
	PHM_PlatformCaps_MMClockGatingSupport,
	PHM_PlatformCaps_AutomaticDCTransition,
	PHM_PlatformCaps_GeminiPrimary,
	PHM_PlatformCaps_MemorySpreadSpectrumSupport,
	PHM_PlatformCaps_EngineSpreadSpectrumSupport,
	PHM_PlatformCaps_StepVddc,
	PHM_PlatformCaps_DynamicPCIEGen2Support,
	PHM_PlatformCaps_SMC,
	PHM_PlatformCaps_FaultyInternalThermalReading,
	PHM_PlatformCaps_EnableVoltageControl,
	PHM_PlatformCaps_EnableSideportControl,
	PHM_PlatformCaps_VideoPlaybackEEUNotification,
	PHM_PlatformCaps_TurnOffPll_ASPML1,
	PHM_PlatformCaps_EnableHTLinkControl,
	PHM_PlatformCaps_PerformanceStateOnly,
	PHM_PlatformCaps_ExclusiveModeAlwaysHigh,
	PHM_PlatformCaps_DisableMGClockGating,
	PHM_PlatformCaps_DisableMGCGTSSM,
	PHM_PlatformCaps_UVDAlwaysHigh,
	PHM_PlatformCaps_DisablePowerGating,
	PHM_PlatformCaps_CustomThermalPolicy,
	PHM_PlatformCaps_StayInBootState,
	PHM_PlatformCaps_SMCAllowSeparateSWThermalState,
	PHM_PlatformCaps_MultiUVDStateSupport,
	PHM_PlatformCaps_EnableSCLKDeepSleepForUVD,
	PHM_PlatformCaps_EnableMCUHTLinkControl,
	PHM_PlatformCaps_ABM,
	PHM_PlatformCaps_KongThermalPolicy,
	PHM_PlatformCaps_SwitchVDDNB,
	PHM_PlatformCaps_ULPS,
	PHM_PlatformCaps_NativeULPS,
	PHM_PlatformCaps_EnableMVDDControl,
	PHM_PlatformCaps_ControlVDDCI,
	PHM_PlatformCaps_DisableDCODT,
	PHM_PlatformCaps_DynamicACTiming,
	PHM_PlatformCaps_EnableThermalIntByGPIO,
	PHM_PlatformCaps_BootStateOnAlert,
	PHM_PlatformCaps_DontWaitForVBlankOnAlert,
	PHM_PlatformCaps_Force3DClockSupport,
	PHM_PlatformCaps_MicrocodeFanControl,
	PHM_PlatformCaps_AdjustUVDPriorityForSP,
	PHM_PlatformCaps_DisableLightSleep,
	PHM_PlatformCaps_DisableMCLS,
	PHM_PlatformCaps_RegulatorHot,
	PHM_PlatformCaps_BACO,
	PHM_PlatformCaps_DisableDPM,
	PHM_PlatformCaps_DynamicM3Arbiter,
	PHM_PlatformCaps_SclkDeepSleep,
	PHM_PlatformCaps_DynamicPatchPowerState,
	PHM_PlatformCaps_ThermalAutoThrottling,
	PHM_PlatformCaps_SumoThermalPolicy,
	PHM_PlatformCaps_PCIEPerformanceRequest,
	PHM_PlatformCaps_BLControlledByGPU,
	PHM_PlatformCaps_PowerContainment,
	PHM_PlatformCaps_SQRamping,
	PHM_PlatformCaps_CAC,
	PHM_PlatformCaps_NIChipsets,
	PHM_PlatformCaps_TrinityChipsets,
	PHM_PlatformCaps_EvergreenChipsets,
	PHM_PlatformCaps_PowerControl,
	PHM_PlatformCaps_DisableLSClockGating,
	PHM_PlatformCaps_BoostState,
	PHM_PlatformCaps_UserMaxClockForMultiDisplays,
	PHM_PlatformCaps_RegWriteDelay,
	PHM_PlatformCaps_NonABMSupportInPPLib,
	PHM_PlatformCaps_GFXDynamicMGPowerGating,
	PHM_PlatformCaps_DisableSMUUVDHandshake,
	PHM_PlatformCaps_DTE,
	PHM_PlatformCaps_W5100Specifc_SmuSkipMsgDTE,
	PHM_PlatformCaps_UVDPowerGating,
	PHM_PlatformCaps_UVDDynamicPowerGating,
	PHM_PlatformCaps_VCEPowerGating,
	PHM_PlatformCaps_SamuPowerGating,
	PHM_PlatformCaps_UVDDPM,
	PHM_PlatformCaps_VCEDPM,
	PHM_PlatformCaps_SamuDPM,
	PHM_PlatformCaps_AcpDPM,
	PHM_PlatformCaps_SclkDeepSleepAboveLow,
	PHM_PlatformCaps_DynamicUVDState,
	PHM_PlatformCaps_WantSAMClkWithDummyBackEnd,
	PHM_PlatformCaps_WantUVDClkWithDummyBackEnd,
	PHM_PlatformCaps_WantVCEClkWithDummyBackEnd,
	PHM_PlatformCaps_WantACPClkWithDummyBackEnd,
	PHM_PlatformCaps_OD6inACSupport,
	PHM_PlatformCaps_OD6inDCSupport,
	PHM_PlatformCaps_EnablePlatformPowerManagement,
	PHM_PlatformCaps_SurpriseRemoval,
	PHM_PlatformCaps_NewCACVoltage,
	PHM_PlatformCaps_DiDtSupport,
	PHM_PlatformCaps_DBRamping,
	PHM_PlatformCaps_TDRamping,
	PHM_PlatformCaps_TCPRamping,
	PHM_PlatformCaps_DBRRamping,
	PHM_PlatformCaps_DiDtEDCEnable,
	PHM_PlatformCaps_GCEDC,
	PHM_PlatformCaps_PSM,
	PHM_PlatformCaps_EnableSMU7ThermalManagement,
	PHM_PlatformCaps_FPS,
	PHM_PlatformCaps_ACP,
	PHM_PlatformCaps_SclkThrottleLowNotification,
	PHM_PlatformCaps_XDMAEnabled,
	PHM_PlatformCaps_UseDummyBackEnd,
	PHM_PlatformCaps_EnableDFSBypass,
	PHM_PlatformCaps_VddNBDirectRequest,
	PHM_PlatformCaps_PauseMMSessions,
	PHM_PlatformCaps_UnTabledHardwareInterface,
	PHM_PlatformCaps_SMU7,
	PHM_PlatformCaps_RevertGPIO5Polarity,
	PHM_PlatformCaps_Thermal2GPIO17,
	PHM_PlatformCaps_ThermalOutGPIO,
	PHM_PlatformCaps_DisableMclkSwitchingForFrameLock,
	PHM_PlatformCaps_ForceMclkHigh,
	PHM_PlatformCaps_VRHotGPIOConfigurable,
	PHM_PlatformCaps_TempInversion,
	PHM_PlatformCaps_IOIC3,
	PHM_PlatformCaps_ConnectedStandby,
	PHM_PlatformCaps_EVV,
	PHM_PlatformCaps_EnableLongIdleBACOSupport,
	PHM_PlatformCaps_CombinePCCWithThermalSignal,
	PHM_PlatformCaps_DisableUsingActualTemperatureForPowerCalc,
	PHM_PlatformCaps_StablePState,
	PHM_PlatformCaps_OD6PlusinACSupport,
	PHM_PlatformCaps_OD6PlusinDCSupport,
	PHM_PlatformCaps_ODThermalLimitUnlock,
	PHM_PlatformCaps_ReducePowerLimit,
	PHM_PlatformCaps_ODFuzzyFanControlSupport,
	PHM_PlatformCaps_GeminiRegulatorFanControlSupport,
	PHM_PlatformCaps_ControlVDDGFX,
	PHM_PlatformCaps_BBBSupported,
	PHM_PlatformCaps_DisableVoltageIsland,
	PHM_PlatformCaps_FanSpeedInTableIsRPM,
	PHM_PlatformCaps_GFXClockGatingManagedInCAIL,
	PHM_PlatformCaps_IcelandULPSSWWorkAround,
	PHM_PlatformCaps_FPSEnhancement,
	PHM_PlatformCaps_LoadPostProductionFirmware,
	PHM_PlatformCaps_VpuRecoveryInProgress,
	PHM_PlatformCaps_Falcon_QuickTransition,
	PHM_PlatformCaps_AVFS,
	PHM_PlatformCaps_ClockStretcher,
	PHM_PlatformCaps_TablelessHardwareInterface,
	PHM_PlatformCaps_EnableDriverEVV,
	PHM_PlatformCaps_SPLLShutdownSupport,
	PHM_PlatformCaps_VirtualBatteryState,
	PHM_PlatformCaps_IgnoreForceHighClockRequestsInAPUs,
	PHM_PlatformCaps_DisableMclkSwitchForVR,
	PHM_PlatformCaps_SMU8,
	PHM_PlatformCaps_VRHotPolarityHigh,
	PHM_PlatformCaps_IPS_UlpsExclusive,
	PHM_PlatformCaps_SMCtoPPLIBAcdcGpioScheme,
	PHM_PlatformCaps_GeminiAsymmetricPower,
	PHM_PlatformCaps_OCLPowerOptimization,
	PHM_PlatformCaps_MaxPCIEBandWidth,
	PHM_PlatformCaps_PerfPerWattOptimizationSupport,
	PHM_PlatformCaps_UVDClientMCTuning,
	PHM_PlatformCaps_ODNinACSupport,
	PHM_PlatformCaps_ODNinDCSupport,
	PHM_PlatformCaps_OD8inACSupport,
	PHM_PlatformCaps_OD8inDCSupport,
	PHM_PlatformCaps_UMDPState,
	PHM_PlatformCaps_AutoWattmanSupport,
	PHM_PlatformCaps_AutoWattmanEnable_CCCState,
	PHM_PlatformCaps_FreeSyncActive,
	PHM_PlatformCaps_EnableShadowPstate,
	PHM_PlatformCaps_customThermalManagement,
	PHM_PlatformCaps_staticFanControl,
	PHM_PlatformCaps_Virtual_System,
	PHM_PlatformCaps_LowestUclkReservedForUlv,
	PHM_PlatformCaps_EnableBoostState,
	PHM_PlatformCaps_AVFSSupport,
	PHM_PlatformCaps_ThermalPolicyDelay,
	PHM_PlatformCaps_CustomFanControlSupport,
	PHM_PlatformCaps_BAMACO,
	PHM_PlatformCaps_Max
};

#define PHM_MAX_NUM_CAPS_BITS_PER_FIELD (sizeof(uint32_t)*8)
#define PHM_MAX_NUM_CAPS_ULONG_ENTRIES \
	((PHM_PlatformCaps_Max + ((PHM_MAX_NUM_CAPS_BITS_PER_FIELD) - 1)) / (PHM_MAX_NUM_CAPS_BITS_PER_FIELD))

enum PHM_PerformanceLevelDesignation {
	PHM_PerformanceLevelDesignation_Activity,
	PHM_PerformanceLevelDesignation_PowerContainment
};
typedef enum PHM_PerformanceLevelDesignation PHM_PerformanceLevelDesignation;

static inline void phm_cap_set(uint32_t *caps, enum phm_platform_caps c)
{
	caps[c / PHM_MAX_NUM_CAPS_BITS_PER_FIELD] |= (1UL << (c & (PHM_MAX_NUM_CAPS_BITS_PER_FIELD - 1)));
}

static inline void phm_cap_unset(uint32_t *caps, enum phm_platform_caps c)
{
	caps[c / PHM_MAX_NUM_CAPS_BITS_PER_FIELD] &= ~(1UL << (c & (PHM_MAX_NUM_CAPS_BITS_PER_FIELD - 1)));
}

static inline bool phm_cap_enabled(const uint32_t *caps, enum phm_platform_caps c)
{
	return (0 != (caps[c / PHM_MAX_NUM_CAPS_BITS_PER_FIELD] &
		  (1UL << (c & (PHM_MAX_NUM_CAPS_BITS_PER_FIELD - 1)))));
}

#define PP_CAP(c) phm_cap_enabled(hwmgr->platform_descriptor.platformCaps, (c))

#define PP_PCIEGenInvalid  0xffff
enum PP_PCIEGen {
	PP_PCIEGen1 = 0,
	PP_PCIEGen2,
	PP_PCIEGen3
};
typedef enum PP_PCIEGen PP_PCIEGen;

#define PP_Min_PCIEGen     PP_PCIEGen1
#define PP_Max_PCIEGen     PP_PCIEGen3
#define PP_Min_PCIELane    1
#define PP_Max_PCIELane    16

struct PP_Clocks {
	uint32_t engineClock;
	uint32_t memoryClock;
	uint32_t BusBandwidth;
	uint32_t engineClockInSR;
	uint32_t dcefClock;
	uint32_t dcefClockInSR;
};

struct phm_platform_descriptor {
	uint32_t platformCaps[PHM_MAX_NUM_CAPS_ULONG_ENTRIES];
	uint32_t vbiosInterruptId;
	struct PP_Clocks overdriveLimit;
	struct PP_Clocks clockStep;
	uint32_t hardwareActivityPerformanceLevels;
	uint32_t minimumClocksReductionPercentage;
	uint32_t minOverdriveVDDC;
	uint32_t maxOverdriveVDDC;
	uint32_t overdriveVDDCStep;
	uint32_t hardwarePerformanceLevels;
	uint16_t powerBudget;
	uint32_t TDPLimit;
	uint32_t nearTDPLimit;
	uint32_t nearTDPLimitAdjusted;
	uint32_t SQRampingThreshold;
	uint32_t CACLeakage;
	uint16_t TDPODLimit;
	uint32_t TDPAdjustment;
	bool TDPAdjustmentPolarity;
	uint16_t LoadLineSlope;
	uint32_t  VidMinLimit;
	uint32_t  VidMaxLimit;
	uint32_t  VidStep;
	uint32_t  VidAdjustment;
	bool VidAdjustmentPolarity;
};

#define DPMTABLE_OD_UPDATE_SCLK     0x00000001
#define DPMTABLE_OD_UPDATE_MCLK     0x00000002
#define DPMTABLE_UPDATE_SCLK        0x00000004
#define DPMTABLE_UPDATE_MCLK        0x00000008
#define DPMTABLE_OD_UPDATE_VDDC     0x00000010
#define DPMTABLE_UPDATE_SOCCLK      0x00000020

struct phm_odn_performance_level {
	uint32_t clock;
	uint32_t vddc;
	bool enabled;
};

struct phm_odn_clock_levels {
	uint32_t size;
	uint32_t options;
	uint32_t flags;
	uint32_t num_of_pl;
	struct phm_odn_performance_level entries[8];
};

/* --- hwmgr_ppt.h ----------------------------------------------------------- */

struct phm_ppt_v1_clock_voltage_dependency_record {
	uint32_t clk;
	uint8_t  vddInd;
	uint8_t  vddciInd;
	uint8_t  mvddInd;
	uint16_t vdd_offset;
	uint16_t vddc;
	uint16_t vddgfx;
	uint16_t vddci;
	uint16_t mvdd;
	uint8_t  phases;
	uint8_t  cks_enable;
	uint8_t  cks_voffset;
	uint32_t sclk_offset;
};
typedef struct phm_ppt_v1_clock_voltage_dependency_record phm_ppt_v1_clock_voltage_dependency_record;

struct phm_ppt_v1_clock_voltage_dependency_table {
	uint32_t count;
	phm_ppt_v1_clock_voltage_dependency_record entries[];
};
typedef struct phm_ppt_v1_clock_voltage_dependency_table phm_ppt_v1_clock_voltage_dependency_table;

struct phm_ppt_v1_mm_clock_voltage_dependency_record {
	uint32_t  dclk;
	uint32_t  vclk;
	uint32_t  eclk;
	uint32_t  aclk;
	uint32_t  samclock;
	uint8_t	vddcInd;
	uint16_t vddgfx_offset;
	uint16_t vddc;
	uint16_t vddgfx;
	uint8_t phases;
};
typedef struct phm_ppt_v1_mm_clock_voltage_dependency_record phm_ppt_v1_mm_clock_voltage_dependency_record;

struct phm_ppt_v1_mm_clock_voltage_dependency_table {
	uint32_t count;
	phm_ppt_v1_mm_clock_voltage_dependency_record entries[];
};
typedef struct phm_ppt_v1_mm_clock_voltage_dependency_table phm_ppt_v1_mm_clock_voltage_dependency_table;

struct phm_ppt_v1_voltage_lookup_record {
	uint16_t us_calculated;
	uint16_t us_vdd;
	uint16_t us_cac_low;
	uint16_t us_cac_mid;
	uint16_t us_cac_high;
};
typedef struct phm_ppt_v1_voltage_lookup_record phm_ppt_v1_voltage_lookup_record;

struct phm_ppt_v1_voltage_lookup_table {
	uint32_t count;
	phm_ppt_v1_voltage_lookup_record entries[];
};
typedef struct phm_ppt_v1_voltage_lookup_table phm_ppt_v1_voltage_lookup_table;

struct phm_ppt_v1_pcie_record {
	uint8_t gen_speed;
	uint8_t lane_width;
	uint16_t usreserved;
	uint16_t reserved;
	uint32_t pcie_sclk;
};
typedef struct phm_ppt_v1_pcie_record phm_ppt_v1_pcie_record;

struct phm_ppt_v1_pcie_table {
	uint32_t count;
	phm_ppt_v1_pcie_record entries[];
};
typedef struct phm_ppt_v1_pcie_table phm_ppt_v1_pcie_table;

struct phm_ppt_v1_gpio_table {
	uint8_t vrhot_triggered_sclk_dpm_index;
};
typedef struct phm_ppt_v1_gpio_table phm_ppt_v1_gpio_table;

/* --- hwmgr.h --------------------------------------------------------------- */

#define VOLTAGE_SCALE 4
#define VOLTAGE_VID_OFFSET_SCALE1   625
#define VOLTAGE_VID_OFFSET_SCALE2   100

enum DISPLAY_GAP {
	DISPLAY_GAP_VBLANK_OR_WM = 0,
	DISPLAY_GAP_VBLANK       = 1,
	DISPLAY_GAP_WATERMARK    = 2,
	DISPLAY_GAP_IGNORE       = 3
};
typedef enum DISPLAY_GAP DISPLAY_GAP;

struct vi_dpm_level {
	bool enabled;
	uint32_t value;
	uint32_t param1;
};

struct vi_dpm_table {
	uint32_t count;
	struct vi_dpm_level dpm_level[];
};

#define PCIE_PERF_REQ_REMOVE_REGISTRY   0
#define PCIE_PERF_REQ_FORCE_LOWPOWER    1
#define PCIE_PERF_REQ_GEN1         2
#define PCIE_PERF_REQ_GEN2         3
#define PCIE_PERF_REQ_GEN3         4

enum PHM_BackEnd_Magic {
	PHM_VIslands_Magic    = 0x20130307,
};

struct phm_set_power_state_input {
	const struct pp_hw_power_state *pcurrent_state;
	const struct pp_hw_power_state *pnew_state;
};

struct phm_clock_array {
	uint32_t count;
	uint32_t values[];
};

struct phm_clock_voltage_dependency_record {
	uint32_t clk;
	uint32_t v;
};

struct phm_clock_voltage_dependency_table {
	uint32_t count;
	struct phm_clock_voltage_dependency_record entries[];
};

struct phm_cac_tdp_table {
	uint16_t usTDP;
	uint16_t usConfigurableTDP;
	uint16_t usTDC;
	uint16_t usBatteryPowerLimit;
	uint16_t usSmallPowerLimit;
	uint16_t usLowCACLeakage;
	uint16_t usHighCACLeakage;
	uint16_t usMaximumPowerDeliveryLimit;
	uint16_t usEDCLimit;
	uint16_t usOperatingTempMinLimit;
	uint16_t usOperatingTempMaxLimit;
	uint16_t usOperatingTempStep;
	uint16_t usOperatingTempHyst;
	uint16_t usDefaultTargetOperatingTemp;
	uint16_t usTargetOperatingTemp;
	uint16_t usPowerTuneDataSetID;
	uint16_t usSoftwareShutdownTemp;
	uint16_t usClockStretchAmount;
	uint16_t usTemperatureLimitHotspot;
	uint16_t usTemperatureLimitLiquid1;
	uint16_t usTemperatureLimitLiquid2;
	uint16_t usTemperatureLimitVrVddc;
	uint16_t usTemperatureLimitVrMvdd;
	uint16_t usTemperatureLimitPlx;
	uint8_t  ucLiquid1_I2C_address;
	uint8_t  ucLiquid2_I2C_address;
	uint8_t  ucLiquid_I2C_Line;
	uint8_t  ucVr_I2C_address;
	uint8_t  ucVr_I2C_Line;
	uint8_t  ucPlx_I2C_address;
	uint8_t  ucPlx_I2C_Line;
	uint32_t usBoostPowerLimit;
	uint8_t  ucCKS_LDO_REFSEL;
	uint8_t  ucHotSpotOnly;
};

struct phm_ppm_table {
	uint8_t   ppm_design;
	uint16_t  cpu_core_number;
	uint32_t  platform_tdp;
	uint32_t  small_ac_platform_tdp;
	uint32_t  platform_tdc;
	uint32_t  small_ac_platform_tdc;
	uint32_t  apu_tdp;
	uint32_t  dgpu_tdp;
	uint32_t  dgpu_ulv_power;
	uint32_t  tj_max;
};

struct phm_clock_and_voltage_limits {
	uint32_t sclk;
	uint32_t mclk;
	uint32_t gfxclk;
	uint16_t vddc;
	uint16_t vddci;
	uint16_t vddgfx;
	uint16_t vddmem;
};

struct phm_ppt_v1_information {
	struct phm_ppt_v1_clock_voltage_dependency_table *vdd_dep_on_sclk;
	struct phm_ppt_v1_clock_voltage_dependency_table *vdd_dep_on_mclk;
	struct phm_clock_array *valid_sclk_values;
	struct phm_clock_array *valid_mclk_values;
	struct phm_clock_and_voltage_limits max_clock_voltage_on_dc;
	struct phm_clock_and_voltage_limits max_clock_voltage_on_ac;
	struct phm_ppm_table *ppm_parameter_table;
	struct phm_cac_tdp_table *cac_dtp_table;
	struct phm_ppt_v1_mm_clock_voltage_dependency_table *mm_dep_table;
	struct phm_ppt_v1_voltage_lookup_table *vddc_lookup_table;
	struct phm_ppt_v1_voltage_lookup_table *vddgfx_lookup_table;
	struct phm_ppt_v1_pcie_table *pcie_table;
	struct phm_ppt_v1_gpio_table *gpio_table;
	uint16_t us_ulv_voltage_offset;
};

struct phm_dynamic_state_info {
	struct phm_clock_voltage_dependency_table *vddc_dependency_on_display_clock;
	struct phm_clock_and_voltage_limits       max_clock_voltage_on_dc;
	struct phm_clock_and_voltage_limits       max_clock_voltage_on_ac;
	struct phm_cac_tdp_table                  *cac_dtp_table;
};

struct pp_fan_info {
	bool bNoFan;
	uint8_t   ucTachometerPulsesPerRevolution;
	uint32_t   ulMinRPM;
	uint32_t   ulMaxRPM;
};

struct pp_advance_fan_control_parameters {
	uint16_t  usTMin;
	uint16_t  usTMed;
	uint16_t  usTHigh;
	uint16_t  usPWMMin;
	uint16_t  usPWMMed;
	uint16_t  usPWMHigh;
	uint8_t   ucTHyst;
	uint32_t   ulCycleDelay;
	uint16_t  usTMax;
	uint8_t   ucFanControlMode;
	uint16_t  usFanPWMMinLimit;
	uint16_t  usFanPWMMaxLimit;
	uint16_t  usFanPWMStep;
	uint16_t  usDefaultMaxFanPWM;
	uint16_t  usFanOutputSensitivity;
	uint16_t  usDefaultFanOutputSensitivity;
	uint16_t  usMaxFanPWM;
	uint16_t  usFanRPMMinLimit;
	uint16_t  usFanRPMMaxLimit;
	uint16_t  usFanRPMStep;
	uint16_t  usDefaultMaxFanRPM;
	uint16_t  usMaxFanRPM;
	uint16_t  usFanCurrentLow;
	uint16_t  usFanCurrentHigh;
	uint16_t  usFanRPMLow;
	uint16_t  usFanRPMHigh;
	uint32_t   ulMinFanSCLKAcousticLimit;
	uint8_t   ucTargetTemperature;
	uint8_t   ucMinimumPWMLimit;
	uint16_t  usFanGainEdge;
	uint16_t  usFanGainHotspot;
	uint16_t  usFanGainLiquid;
	uint16_t  usFanGainVrVddc;
	uint16_t  usFanGainVrMvdd;
	uint16_t  usFanGainPlx;
	uint16_t  usFanGainHbm;
	uint8_t   ucEnableZeroRPM;
	uint8_t   ucFanStopTemperature;
	uint8_t   ucFanStartTemperature;
	uint32_t  ulMaxFanSCLKAcousticLimit;
	uint32_t  ulTargetGfxClk;
	uint16_t  usZeroRPMStartTemperature;
	uint16_t  usZeroRPMStopTemperature;
	uint16_t  usMGpuThrottlingRPMLimit;
};

struct pp_thermal_controller_info {
	uint8_t ucType;
	uint8_t ucI2cLine;
	uint8_t ucI2cAddress;
	uint8_t use_hw_fan_control;
	struct pp_fan_info fanInfo;
	struct pp_advance_fan_control_parameters advanceFanControlParameters;
};

struct phm_microcode_version_info {
	uint32_t SMC;
	uint32_t DMCU;
	uint32_t MC;
	uint32_t NB;
};

enum PP_TABLE_VERSION {
	PP_TABLE_V0 = 0,
	PP_TABLE_V1,
	PP_TABLE_V2,
	PP_TABLE_MAX
};

/* the two function tables, trimmed to what the Polaris paths fill and call */
struct pp_smumgr_func {
	const char *name;
	int (*smu_init)(struct pp_hwmgr *hwmgr);
	int (*smu_fini)(struct pp_hwmgr *hwmgr);
	int (*update_smc_table)(struct pp_hwmgr *hwmgr, uint32_t type);
	int (*process_firmware_header)(struct pp_hwmgr *hwmgr);
	int (*update_sclk_threshold)(struct pp_hwmgr *hwmgr);
	int (*thermal_setup_fan_table)(struct pp_hwmgr *hwmgr);
	int (*thermal_avfs_enable)(struct pp_hwmgr *hwmgr);
	int (*init_smc_table)(struct pp_hwmgr *hwmgr);
	int (*populate_all_graphic_levels)(struct pp_hwmgr *hwmgr);
	int (*populate_all_memory_levels)(struct pp_hwmgr *hwmgr);
	int (*initialize_mc_reg_table)(struct pp_hwmgr *hwmgr);
	uint32_t (*get_offsetof)(uint32_t type, uint32_t member);
	uint32_t (*get_mac_definition)(uint32_t value);
	bool (*is_dpm_running)(struct pp_hwmgr *hwmgr);
	bool (*is_hw_avfs_present)(struct pp_hwmgr *hwmgr);
};

struct pp_hwmgr_func {
	int (*backend_init)(struct pp_hwmgr *hw_mgr);
	int (*backend_fini)(struct pp_hwmgr *hw_mgr);
	int (*asic_setup)(struct pp_hwmgr *hw_mgr);
	int (*get_power_state_size)(struct pp_hwmgr *hw_mgr);
	int (*apply_state_adjust_rules)(struct pp_hwmgr *hwmgr,
				struct pp_power_state  *prequest_ps,
			const struct pp_power_state *pcurrent_ps);
	int (*force_dpm_level)(struct pp_hwmgr *hw_mgr, enum amd_dpm_forced_level level);
	int (*dynamic_state_management_enable)(struct pp_hwmgr *hw_mgr);
	int (*dynamic_state_management_disable)(struct pp_hwmgr *hw_mgr);
	int (*patch_boot_state)(struct pp_hwmgr *hwmgr, struct pp_hw_power_state *hw_ps);
	int (*get_pp_table_entry)(struct pp_hwmgr *hwmgr, unsigned long, struct pp_power_state *);
	int (*get_num_of_pp_table_entries)(struct pp_hwmgr *hwmgr);
	uint32_t (*get_mclk)(struct pp_hwmgr *hwmgr, bool low);
	uint32_t (*get_sclk)(struct pp_hwmgr *hwmgr, bool low);
	int (*power_state_set)(struct pp_hwmgr *hwmgr, const void *state);
	int (*notify_smc_display_config_after_ps_adjustment)(struct pp_hwmgr *hwmgr);
	int (*display_config_changed)(struct pp_hwmgr *hwmgr);
	int (*set_max_fan_rpm_output)(struct pp_hwmgr *hwmgr, uint16_t us_max_fan_pwm);
	int (*set_max_fan_pwm_output)(struct pp_hwmgr *hwmgr, uint16_t us_max_fan_pwm);
	int (*stop_thermal_controller)(struct pp_hwmgr *hwmgr);
	int (*get_fan_speed_pwm)(struct pp_hwmgr *hwmgr, uint32_t *speed);
	int (*get_fan_speed_rpm)(struct pp_hwmgr *hwmgr, uint32_t *speed);
	bool (*check_smc_update_required_for_display_configuration)(struct pp_hwmgr *hwmgr);
	int (*check_states_equal)(struct pp_hwmgr *hwmgr,
					const struct pp_hw_power_state *pstate1,
					const struct pp_hw_power_state *pstate2,
					bool *equal);
	int (*force_clock_level)(struct pp_hwmgr *hwmgr, enum pp_clock_type type, uint32_t mask);
	int (*read_sensor)(struct pp_hwmgr *hwmgr, int idx, void *value, int *size);
	int (*avfs_control)(struct pp_hwmgr *hwmgr, bool enable);
	int (*start_thermal_controller)(struct pp_hwmgr *hwmgr, struct PP_TemperatureRange *range);
	int (*get_thermal_temperature_range)(struct pp_hwmgr *hwmgr, struct PP_TemperatureRange *range);
	int (*set_power_limit)(struct pp_hwmgr *hwmgr, uint32_t n);
	void (*powergate_uvd)(struct pp_hwmgr *hwmgr, bool bgate);
};

struct pp_table_func {
	int (*pptable_init)(struct pp_hwmgr *hw_mgr);
	int (*pptable_fini)(struct pp_hwmgr *hw_mgr);
};

#define Workload_Policy_Max 6

struct pp_hwmgr {
	struct frb_device *adev;
	uint32_t chip_family;
	uint32_t chip_id;
	bool not_vf;
	bool pm_en;
	bool pp_one_vf;

	uint32_t pp_table_version;
	struct frb_device *device;
	const uint8_t *soft_pp_table;
	uint32_t soft_pp_table_size;
	void *hardcode_pp_table;	/* always NULL: no user pp_table on Firebrand */

	enum amd_dpm_forced_level dpm_level;
	enum amd_dpm_forced_level saved_dpm_level;
	enum amd_dpm_forced_level request_dpm_level;
	uint32_t usec_timeout;
	void *pptable;
	struct phm_platform_descriptor platform_descriptor;
	void *backend;

	void *smu_backend;
	const struct pp_smumgr_func *smumgr_funcs;
	bool is_kicker;

	struct phm_dynamic_state_info dyn_state;
	const struct pp_hwmgr_func *hwmgr_func;
	const struct pp_table_func *pptable_func;

	struct pp_power_state    *ps;
	uint32_t num_ps;
	struct pp_thermal_controller_info thermal_controller;
	bool fan_ctrl_is_in_default_mode;
	uint32_t fan_ctrl_default_mode;
	bool fan_ctrl_enabled;
	uint32_t tmin;
	struct phm_microcode_version_info microcode_version_info;
	uint32_t ps_size;
	struct pp_power_state    *current_ps;
	struct pp_power_state    *request_ps;
	struct pp_power_state    *boot_ps;
	struct pp_power_state    *uvd_ps;
	const struct amd_pp_display_configuration *display_config;
	uint32_t feature_mask;
	bool avfs_supported;
	bool en_umd_pstate;
	uint32_t power_profile_mode;
	uint32_t default_power_profile_mode;
	uint32_t pstate_sclk;
	uint32_t pstate_mclk;
	bool od_enabled;
	uint32_t power_limit;
	uint32_t default_power_limit;
	uint32_t workload_mask;
	uint32_t workload_prority[Workload_Policy_Max];
	uint32_t workload_setting[Workload_Policy_Max];
	bool gfxoff_state_changed_by_workload;
	uint32_t pstate_sclk_peak;
	uint32_t pstate_mclk_peak;
};

#define PHM_ENTIRE_REGISTER_MASK 0xFFFFFFFFU

/* hwmgr.c (frb_hwmgr.c) */
int hwmgr_early_init(struct pp_hwmgr *hwmgr);
int hwmgr_sw_init(struct pp_hwmgr *hwmgr);
int hwmgr_sw_fini(struct pp_hwmgr *hwmgr);
int hwmgr_hw_init(struct pp_hwmgr *hwmgr);
int hwmgr_hw_fini(struct pp_hwmgr *hwmgr);
int hwmgr_handle_task(struct pp_hwmgr *hwmgr, enum amd_pp_task task_id,
		      enum amd_pm_state_type *user_state);
int smu7_init_function_pointers(struct pp_hwmgr *hwmgr);

/* hardwaremanager.c (frb_hardwaremanager.c) */
int phm_setup_asic(struct pp_hwmgr *hwmgr);
int phm_enable_dynamic_state_management(struct pp_hwmgr *hwmgr);
int phm_disable_dynamic_state_management(struct pp_hwmgr *hwmgr);
int phm_set_power_state(struct pp_hwmgr *hwmgr,
			const struct pp_hw_power_state *pcurrent_state,
			const struct pp_hw_power_state *pnew_power_state);
int phm_apply_state_adjust_rules(struct pp_hwmgr *hwmgr,
				 struct pp_power_state *adjusted_ps,
				 const struct pp_power_state *current_ps);
int phm_force_dpm_levels(struct pp_hwmgr *hwmgr, enum amd_dpm_forced_level level);
int phm_pre_display_configuration_changed(struct pp_hwmgr *hwmgr);
int phm_display_configuration_changed(struct pp_hwmgr *hwmgr);
int phm_notify_smc_display_config_after_ps_adjustment(struct pp_hwmgr *hwmgr);
int phm_start_thermal_controller(struct pp_hwmgr *hwmgr);
int phm_stop_thermal_controller(struct pp_hwmgr *hwmgr);
bool phm_check_smc_update_required_for_display_configuration(struct pp_hwmgr *hwmgr);
int phm_check_states_equal(struct pp_hwmgr *hwmgr,
			   const struct pp_hw_power_state *pstate1,
			   const struct pp_hw_power_state *pstate2,
			   bool *equal);
int phm_store_dal_configuration_data(struct pp_hwmgr *hwmgr,
				     const struct amd_pp_display_configuration *display_config);
int phm_set_cpu_power_state(struct pp_hwmgr *hwmgr);
int phm_apply_clock_adjust_rules(struct pp_hwmgr *hwmgr);

/* pp_psm.c (frb_pp_psm.c) */
int psm_init_power_state_table(struct pp_hwmgr *hwmgr);
int psm_fini_power_state_table(struct pp_hwmgr *hwmgr);
int psm_set_boot_states(struct pp_hwmgr *hwmgr);
int psm_set_performance_states(struct pp_hwmgr *hwmgr);
int psm_adjust_power_state_dynamic(struct pp_hwmgr *hwmgr, bool skip_display_settings,
				   struct pp_power_state *new_ps);

/* smumgr.h (smumgr.c's wrappers, in frb_polaris10_smumgr.c) */
enum SMU_TABLE {
	SMU_UVD_TABLE = 0,
	SMU_VCE_TABLE,
	SMU_BIF_TABLE,
};

enum SMU_TYPE {
	SMU_SoftRegisters = 0,
	SMU_Discrete_DpmTable,
};

enum SMU_MEMBER {
	HandshakeDisables = 0,
	VoltageChangeTimeout,
	AverageGraphicsActivity,
	AverageMemoryActivity,
	PreVBlankGap,
	VBlankTimeout,
	UcodeLoadStatus,
	UvdBootLevel,
	VceBootLevel,
	LowSclkInterruptThreshold,
	DRAM_LOG_ADDR_H,
	DRAM_LOG_ADDR_L,
	DRAM_LOG_PHY_ADDR_H,
	DRAM_LOG_PHY_ADDR_L,
	DRAM_LOG_BUFF_SIZE,
};

enum SMU_MAC_DEFINITION {
	SMU_MAX_LEVELS_GRAPHICS = 0,
	SMU_MAX_LEVELS_MEMORY,
	SMU_MAX_LEVELS_LINK,
	SMU_MAX_ENTRIES_SMIO,
	SMU_MAX_LEVELS_VDDC,
	SMU_MAX_LEVELS_VDDGFX,
	SMU_MAX_LEVELS_VDDCI,
	SMU_MAX_LEVELS_MVDD,
	SMU_UVD_MCLK_HANDSHAKE_DISABLE,
};

int smum_send_msg_to_smc(struct pp_hwmgr *hwmgr, uint16_t msg, uint32_t *resp);
int smum_send_msg_to_smc_with_parameter(struct pp_hwmgr *hwmgr, uint16_t msg, uint32_t parameter,
					uint32_t *resp);
int smum_update_sclk_threshold(struct pp_hwmgr *hwmgr);
int smum_update_smc_table(struct pp_hwmgr *hwmgr, uint32_t type);
int smum_process_firmware_header(struct pp_hwmgr *hwmgr);
int smum_thermal_avfs_enable(struct pp_hwmgr *hwmgr);
int smum_thermal_setup_fan_table(struct pp_hwmgr *hwmgr);
int smum_init_smc_table(struct pp_hwmgr *hwmgr);
int smum_populate_all_graphic_levels(struct pp_hwmgr *hwmgr);
int smum_populate_all_memory_levels(struct pp_hwmgr *hwmgr);
int smum_initialize_mc_reg_table(struct pp_hwmgr *hwmgr);
uint32_t smum_get_offsetof(struct pp_hwmgr *hwmgr, uint32_t type, uint32_t member);
uint32_t smum_get_mac_definition(struct pp_hwmgr *hwmgr, uint32_t value);
bool smum_is_dpm_running(struct pp_hwmgr *hwmgr);
bool smum_is_hw_avfs_present(struct pp_hwmgr *hwmgr);
int smum_stop_smc(struct pp_hwmgr *hwmgr);

/* --- smu_helper.h (frb_smu_helper.c) -------------------------------------- */

struct pp_atomctrl_voltage_table;

uint8_t convert_to_vid(uint16_t vddc);
uint16_t convert_to_vddc(uint8_t vid);
int phm_wait_on_register(struct pp_hwmgr *hwmgr, uint32_t index, uint32_t value, uint32_t mask);
int phm_wait_on_indirect_register(struct pp_hwmgr *hwmgr, uint32_t indirect_port, uint32_t index,
				  uint32_t value, uint32_t mask);
int phm_wait_for_register_unequal(struct pp_hwmgr *hwmgr, uint32_t index, uint32_t value, uint32_t mask);
int phm_wait_for_indirect_register_unequal(struct pp_hwmgr *hwmgr, uint32_t indirect_port,
					   uint32_t index, uint32_t value, uint32_t mask);
bool phm_cf_want_uvd_power_gating(struct pp_hwmgr *hwmgr);
bool phm_cf_want_vce_power_gating(struct pp_hwmgr *hwmgr);
int phm_trim_voltage_table(struct pp_hwmgr *hwmgr, struct pp_atomctrl_voltage_table *vol_table);
int phm_get_svi2_mvdd_voltage_table(struct pp_hwmgr *hwmgr, struct pp_atomctrl_voltage_table *vol_table,
				    phm_ppt_v1_clock_voltage_dependency_table *dep_table);
int phm_get_svi2_vddci_voltage_table(struct pp_hwmgr *hwmgr, struct pp_atomctrl_voltage_table *vol_table,
				     phm_ppt_v1_clock_voltage_dependency_table *dep_table);
int phm_get_svi2_vdd_voltage_table(struct pp_atomctrl_voltage_table *vol_table,
				   phm_ppt_v1_voltage_lookup_table *lookup_table);
void phm_trim_voltage_table_to_fit_state_table(uint32_t max_vol_steps, struct pp_atomctrl_voltage_table *vol_table);
int phm_reset_single_dpm_table(void *table, uint32_t count, int max);
void phm_setup_pcie_table_entry(void *table, uint32_t index, uint32_t pcie_gen, uint32_t pcie_lanes);
int32_t phm_get_dpm_level_enable_mask_value(void *table);
uint8_t phm_get_voltage_id(struct pp_atomctrl_voltage_table *voltage_table, uint32_t voltage);
uint8_t phm_get_voltage_index(struct phm_ppt_v1_voltage_lookup_table *lookup_table, uint16_t voltage);
uint16_t phm_find_closest_vddci(struct pp_atomctrl_voltage_table *vddci_table, uint16_t vddci);
int phm_find_boot_level(void *table, uint32_t value, uint32_t *boot_level);
int phm_get_sclk_for_voltage_evv(struct pp_hwmgr *hwmgr, phm_ppt_v1_voltage_lookup_table *lookup_table,
				 uint16_t virtual_voltage_id, int32_t *sclk);
uint32_t phm_get_lowest_enabled_level(struct pp_hwmgr *hwmgr, uint32_t mask);
int phm_get_voltage_evv_on_sclk(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
				uint32_t sclk, uint16_t id, uint16_t *voltage);
uint32_t phm_set_field_to_u32(u32 offset, u32 original_data, u32 field, u32 size);
/* smu_atom_get_data_table: the table's offset in the VBIOS image (0: none) */
const uint8_t *smu_atom_get_data_table(struct frb_device *adev, uint32_t table, uint16_t *size,
				       uint8_t *frev, uint8_t *crev);
int smu_get_voltage_dependency_table_ppt_v1(
	const struct phm_ppt_v1_clock_voltage_dependency_table *allowed_dep_table,
	struct phm_ppt_v1_clock_voltage_dependency_table *dep_table);

#define PHM_FIELD_SHIFT(reg, field) reg##__##field##__SHIFT
#define PHM_FIELD_MASK(reg, field) reg##__##field##_MASK

#define PHM_SET_FIELD(origval, reg, field, fieldval)	\
	(((origval) & ~PHM_FIELD_MASK(reg, field)) |	\
	 (PHM_FIELD_MASK(reg, field) & ((fieldval) << PHM_FIELD_SHIFT(reg, field))))

#define PHM_GET_FIELD(value, reg, field)	\
	(((value) & PHM_FIELD_MASK(reg, field)) >>	\
	 PHM_FIELD_SHIFT(reg, field))

#define PHM_READ_FIELD(device, reg, field)	\
	PHM_GET_FIELD(cgs_read_register(device, mm##reg), reg, field)

#define PHM_READ_INDIRECT_FIELD(device, port, reg, field)	\
	PHM_GET_FIELD(cgs_read_ind_register(device, port, ix##reg),	\
			reg, field)

#define PHM_READ_VFPF_INDIRECT_FIELD(device, port, reg, field)	\
	PHM_GET_FIELD(cgs_read_ind_register(device, port, ix##reg),	\
			reg, field)

#define PHM_WRITE_FIELD(device, reg, field, fieldval)	\
	cgs_write_register(device, mm##reg, PHM_SET_FIELD(	\
				cgs_read_register(device, mm##reg), reg, field, fieldval))

#define PHM_WRITE_INDIRECT_FIELD(device, port, reg, field, fieldval)	\
	cgs_write_ind_register(device, port, ix##reg,	\
			PHM_SET_FIELD(cgs_read_ind_register(device, port, ix##reg),	\
				reg, field, fieldval))

#define PHM_WRITE_VFPF_INDIRECT_FIELD(device, port, reg, field, fieldval)	\
	cgs_write_ind_register(device, port, ix##reg,	\
			PHM_SET_FIELD(cgs_read_ind_register(device, port, ix##reg),	\
				reg, field, fieldval))

#define PHM_WAIT_REGISTER_UNEQUAL_GIVEN_INDEX(hwmgr, index, value, mask) \
		phm_wait_for_register_unequal(hwmgr, index, value, mask)

#define PHM_WAIT_REGISTER_UNEQUAL(hwmgr, reg, value, mask)		\
	PHM_WAIT_REGISTER_UNEQUAL_GIVEN_INDEX(hwmgr, mm##reg, value, mask)

#define PHM_WAIT_FIELD_UNEQUAL(hwmgr, reg, field, fieldval)		\
	PHM_WAIT_REGISTER_UNEQUAL(hwmgr, reg,				\
		(fieldval) << PHM_FIELD_SHIFT(reg, field),		\
		PHM_FIELD_MASK(reg, field))

/* --- process_pptables_v1_0.h (frb_process_pptables_v1_0.c) ---------------- */

extern const struct pp_table_func pptable_v1_0_funcs;
int get_number_of_powerplay_table_entries_v1_0(struct pp_hwmgr *hwmgr);
int get_powerplay_table_entry_v1_0(struct pp_hwmgr *hwmgr, uint32_t entry_index,
		struct pp_power_state *power_state, int (*call_back_func)(struct pp_hwmgr *, const uint8_t *,
				struct pp_power_state *, const uint8_t *, uint32_t));


/* --- include/amd_pcie.h, amd_pcie_helpers.h; hwmgr/pppcielanes.c -------- */

#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1        0x00010000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2        0x00020000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3        0x00040000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN4        0x00080000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN5        0x00100000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_MASK        0xFFFF0000
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1   0x00000001
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2   0x00000002
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3   0x00000004
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN4   0x00000008
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN5   0x00000010
#define CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_MASK   0x0000FFFF
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X1          0x00000001
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X2          0x00000002
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X4          0x00000004
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X8          0x00000008
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X12         0x00000010
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X16         0x00000020
#define CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X32         0x00000040
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X1          0x00010000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X2          0x00020000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X4          0x00040000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X8          0x00080000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X12         0x00100000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X16         0x00200000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X32         0x00400000
#define AMDGPU_DEFAULT_PCIE_GEN_MASK (CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1 \
				      | CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2 \
				      | CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3 \
				      | CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1 \
				      | CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2 \
				      | CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3)
#define AMDGPU_DEFAULT_PCIE_MLW_MASK (CAIL_PCIE_LINK_WIDTH_SUPPORT_X1 \
				      | CAIL_PCIE_LINK_WIDTH_SUPPORT_X2 \
				      | CAIL_PCIE_LINK_WIDTH_SUPPORT_X4 \
				      | CAIL_PCIE_LINK_WIDTH_SUPPORT_X8 \
				      | CAIL_PCIE_LINK_WIDTH_SUPPORT_X12 \
				      | CAIL_PCIE_LINK_WIDTH_SUPPORT_X16)
#define AMDGPU_DEFAULT_ASIC_PCIE_MLW_MASK (CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X1 \
					   | CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X2 \
					   | CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X4 \
					   | CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X8 \
					   | CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X12 \
					   | CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X16)

static inline bool is_pcie_gen3_supported(uint32_t pcie_link_speed_cap)
{
	if (pcie_link_speed_cap & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3)
		return true;

	return false;
}

static inline bool is_pcie_gen2_supported(uint32_t pcie_link_speed_cap)
{
	if (pcie_link_speed_cap & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2)
		return true;

	return false;
}

/* Get the new PCIE speed given the ASIC PCIE Cap and the NewState's requested PCIE speed*/
static inline uint16_t get_pcie_gen_support(uint32_t pcie_link_speed_cap,
					    uint16_t ns_pcie_gen)
{
	uint32_t asic_pcie_link_speed_cap = (pcie_link_speed_cap &
		CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_MASK);
	uint32_t sys_pcie_link_speed_cap  = (pcie_link_speed_cap &
		CAIL_PCIE_LINK_SPEED_SUPPORT_MASK);

	switch (asic_pcie_link_speed_cap) {
	case CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1:
		return PP_PCIEGen1;

	case CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2:
		return PP_PCIEGen2;

	case CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3:
		return PP_PCIEGen3;

	default:
		if (is_pcie_gen3_supported(sys_pcie_link_speed_cap) &&
			(ns_pcie_gen == PP_PCIEGen3)) {
			return PP_PCIEGen3;
		} else if (is_pcie_gen2_supported(sys_pcie_link_speed_cap) &&
			((ns_pcie_gen == PP_PCIEGen3) || (ns_pcie_gen == PP_PCIEGen2))) {
			return PP_PCIEGen2;
		}
	}

	return PP_PCIEGen1;
}

static inline uint16_t get_pcie_lane_support(uint32_t pcie_lane_width_cap,
					     uint16_t ns_pcie_lanes)
{
	int i, j;
	uint16_t new_pcie_lanes = ns_pcie_lanes;
	uint16_t pcie_lanes[7] = {1, 2, 4, 8, 12, 16, 32};

	switch (pcie_lane_width_cap) {
	case 0:
		pr_err("No valid PCIE lane width reported\n");
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X1:
		new_pcie_lanes = 1;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X2:
		new_pcie_lanes = 2;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X4:
		new_pcie_lanes = 4;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X8:
		new_pcie_lanes = 8;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X12:
		new_pcie_lanes = 12;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X16:
		new_pcie_lanes = 16;
		break;
	case CAIL_PCIE_LINK_WIDTH_SUPPORT_X32:
		new_pcie_lanes = 32;
		break;
	default:
		for (i = 0; i < 7; i++) {
			if (ns_pcie_lanes == pcie_lanes[i]) {
				if (pcie_lane_width_cap & (0x10000 << i)) {
					break;
				} else {
					for (j = i - 1; j >= 0; j--) {
						if (pcie_lane_width_cap & (0x10000 << j)) {
							new_pcie_lanes = pcie_lanes[j];
							break;
						}
					}

					if (j < 0) {
						for (j = i + 1; j < 7; j++) {
							if (pcie_lane_width_cap & (0x10000 << j)) {
								new_pcie_lanes = pcie_lanes[j];
								break;
							}
						}
						if (j > 7)
							pr_err("Cannot find a valid PCIE lane width!\n");
					}
				}
				break;
			}
		}
		break;
	}

	return new_pcie_lanes;
}

uint8_t encode_pcie_lane_width(uint32_t num_lanes);
uint8_t decode_pcie_lane_width(uint32_t num_lanes);

/* amdgpu.h: the Polaris "kicker" parts */
#define ASICID_IS_P20(did, rid)	(((did == 0x67DF) && \
				 ((rid == 0xE3) || \
				  (rid == 0xE4) || \
				  (rid == 0xE5) || \
				  (rid == 0xE7) || \
				  (rid == 0xEF))) || \
				 ((did == 0x6FDF) && \
				 ((rid == 0xE7) || \
				  (rid == 0xEF) || \
				  (rid == 0xFF))))
#define ASICID_IS_P30(did, rid)	((did == 0x67DF) && \
				((rid == 0xE1) || \
				 (rid == 0xF7)))
#define ASICID_IS_P21(did, rid)	(((did == 0x67EF) && \
				 ((rid == 0xE0) || \
				  (rid == 0xE5))) || \
				 ((did == 0x67FF) && \
				 ((rid == 0xCF) || \
				  (rid == 0xEF) || \
				  (rid == 0xFF))))
#define ASICID_IS_P31(did, rid)	((did == 0x67EF) && \
				((rid == 0xE2)))

#endif /* FRB_HWMGR_H */
