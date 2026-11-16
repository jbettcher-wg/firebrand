# Builds Firebrand.kext on Mac OS X 10.5.8 with Xcode 3.1 (Apple gcc 4.0.1);
# scripts/build-mac.sh runs it and checks the result. Same
# shape as RadeonNI's kext Makefile with LEOPARD=1.
#
# The hardware library (../../shared, ../../amdgpu) is compiled in unchanged.

KEXT     = Firebrand
KROOT    = /System/Library/Frameworks/Kernel.framework
LIB      = ../..
BUILD    = build
BUNDLE   = $(BUILD)/$(KEXT).kext
FWDIR    = $(LIB)/firmware

# IOPCIMatch: every device id of amdgpu/frb_drv.c's pciidlist (all of
# amdgpu's discrete SI, CIK, Polaris and Vega ids), written into both
# personalities by mk-pcimatch.pl, so the family table and the match cannot
# drift. A card with a verified row runs to that row's stage; any other card
# of a ported family is an untested card, display-only unless unlocked
# (FirebrandUnlock, `sudo frbctl unlock`); Vega 10 and 12 are identified only.
#
# BRINGUP=vega20: the vega20 microcode (firmware-vega20.list) is embedded
# beside firmware.list's. Every release build is one: without it a Vega 20
# card stops at stage smu (no PSP images).
# BRINGUP=hawaii is accepted and adds nothing.
#
# UNTESTED=0xDEVICE: a TEST build that ignores that device's verified rows,
# so a verified card runs as an untested one (the untested-card policy's
# test on hardware). Never installed for daily use.
BRINGUP  ?=
UNTESTED ?=
FW_LISTS = firmware.list $(if $(filter vega20,$(BRINGUP)),firmware-vega20.list)

# RadeonNI's user-client protocol (hw/rdn_user.h), which the accelerator
# speaks unchanged: radeonni/hw/rdn_user.h in this repository.
RDN_HW   = $(LIB)/radeonni/hw

# frb_build.h (optional): the git commit the build comes from, for FirebrandVersion
BUILD_H  = $(if $(wildcard frb_build.h),-DFRB_HAVE_BUILD_H)

CPPFLAGS = $(BUILD_H) $(if $(UNTESTED),-DFRB_FORCE_UNTESTED=$(UNTESTED)) -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-nostdinc -Icompat -I$(KROOT)/PrivateHeaders -I$(KROOT)/Headers \
	-I$(LIB)/shared -I$(LIB)/amdgpu -I$(RDN_HW)
CFLAGS   = -arch ppc -Os -Wall -fno-builtin -fno-common -static \
	-mlong-branch -force_cpusubtype_ALL
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fapple-kext -fcheck-new
# Files taken from Linux do not pass -Wall cleanly with this compiler.
LIBCFLAGS = $(CFLAGS) -std=gnu99 -Wno-unused -Wno-sign-compare

LIB_SRCS = shared/atom/atom.c shared/frb_edid.c shared/frb_dmt.c shared/frb_mem.c shared/frb_splash.c \
	amdgpu/frb_drv.c amdgpu/frb_device.c amdgpu/frb_atombios.c \
	amdgpu/frb_soc15.c amdgpu/frb_atomfirmware.c amdgpu/frb_gmc_v9_0.c amdgpu/frb_gfxhub_v1_0.c \
	amdgpu/frb_mmhub_v1_0.c amdgpu/frb_psp.c amdgpu/frb_psp_v11_0.c amdgpu/frb_vega20_smumgr.c \
	amdgpu/frb_dce_v12_0.c amdgpu/frb_dce12_aux.c amdgpu/frb_dce12_hwseq.c amdgpu/frb_dp_fallback.c \
	amdgpu/frb_vega20_hwmgr.c amdgpu/frb_gfx_v9_0.c amdgpu/frb_sdma_v4_0.c \
	amdgpu/frb_vi.c amdgpu/frb_gmc_v8_0.c amdgpu/frb_gart.c amdgpu/frb_ucode.c amdgpu/frb_smu7.c \
	amdgpu/frb_atombios_conn.c amdgpu/frb_atombios_crtc.c amdgpu/frb_atombios_encoders.c \
	amdgpu/frb_atombios_dp.c amdgpu/frb_atombios_i2c.c amdgpu/frb_dce_v11_0.c amdgpu/frb_dce_audio.c \
	amdgpu/frb_display.c amdgpu/frb_cik.c amdgpu/frb_gmc_v7_0.c amdgpu/frb_dce_v8_0.c amdgpu/frb_dce_v8_0_audio.c amdgpu/frb_si.c amdgpu/frb_gmc_v6_0.c amdgpu/frb_dce_v6_0.c amdgpu/frb_dce_v6_0_audio.c amdgpu/frb_pll.c \
	amdgpu/frb_ring.c amdgpu/frb_engines.c amdgpu/frb_gfx_v8_0.c amdgpu/frb_gfx_v6_0.c amdgpu/frb_si_dma.c amdgpu/frb_sdma_v3_0.c \
	amdgpu/frb_gfx_v7_0.c amdgpu/frb_cik_sdma.c \
	amdgpu/frb_bo.c amdgpu/frb_rect.c amdgpu/frb_reset.c \
	amdgpu/frb_amd_powerplay.c amdgpu/frb_hwmgr.c amdgpu/frb_hardwaremanager.c amdgpu/frb_pp_psm.c \
	amdgpu/frb_smu_helper.c amdgpu/frb_ppatomctrl.c amdgpu/frb_process_pptables_v1_0.c \
	amdgpu/frb_smu7_hwmgr.c amdgpu/frb_smu7_powertune.c amdgpu/frb_smu7_thermal.c amdgpu/frb_smu7_clockpowergating.c \
	amdgpu/frb_polaris10_smumgr.c \
	amdgpu/frb_uvd.c amdgpu/frb_uvd_v6_0.c amdgpu/frb_uvd_v7_0.c amdgpu/frb_uvd_v3_1.c amdgpu/frb_uvd_v4_2.c amdgpu/frb_uvd_h264.c \
	amdgpu/frb_si_dpm.c amdgpu/si_dpm/rdn_si_dpm.c amdgpu/si_dpm/rdn_ci_tables.c \
	amdgpu/frb_ci_dpm.c amdgpu/si_dpm/rdn_ci_dpm.c amdgpu/si_dpm/rdn_ci_smu.c amdgpu/si_dpm/rdn_cik_smc.c
LIB_OBJS = $(addprefix $(BUILD)/lib/,$(LIB_SRCS:.c=.o))
OBJS     = $(BUILD)/Firebrand.o $(BUILD)/FirebrandCore.o $(BUILD)/FirebrandFramebuffer.o \
	$(BUILD)/FirebrandAccel.o $(BUILD)/FirebrandAccel2D.o $(BUILD)/FirebrandSurface.o $(BUILD)/FirebrandVideo.o \
	$(BUILD)/kmod_info.o $(BUILD)/leopard.o $(LIB_OBJS)
LIB_HDRS = $(wildcard $(LIB)/shared/*.h $(LIB)/shared/atom/*.h $(LIB)/amdgpu/*.h $(LIB)/amdgpu/include/*.h $(LIB)/amdgpu/include/pp/*.h \
	$(LIB)/amdgpu/include/asic_reg/*/*.h $(LIB)/amdgpu/si_dpm/*.h $(LIB)/amdgpu/si_dpm/linux/*.h)

all: $(BUNDLE)

$(BUILD)/leopard.o: leopard.s
	@mkdir -p $(dir $@)
	gcc -arch ppc -c -o $@ $<

$(BUILD)/%.o: %.cpp Firebrand.h FirebrandCore.h FirebrandFramebuffer.h FirebrandAccel.h FirebrandSurface.h FirebrandCompat.h \
		FirebrandVideo.h frb_user.h $(RDN_HW)/rdn_user.h $(RDN_HW)/rdn_video.h $(LIB_HDRS)
	@mkdir -p $(dir $@)
	g++ $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD)/lib/%.o: $(LIB)/%.c $(LIB_HDRS)
	@mkdir -p $(dir $@)
	gcc $(CPPFLAGS) $(LIBCFLAGS) -c -o $@ $<

# Objects first: kld needs the symbols libkmodc++ defines for them.
$(BUNDLE): $(OBJS) Info.plist $(FW_LISTS) embed-firmware.pl mk-pcimatch.pl $(LIB)/amdgpu/frb_drv.c
	@mkdir -p $(BUNDLE)/Contents/MacOS
	g++ -arch ppc -static -nostdlib -r $(OBJS) -lkmodc++ -lkmod -lcc_kext \
		-o $(BUNDLE)/Contents/MacOS/$(KEXT)
	cp Info.plist $(BUNDLE)/Contents/Info.plist
	perl mk-pcimatch.pl $(LIB)/amdgpu/frb_drv.c $(BUNDLE)/Contents/Info.plist
	cat $(FW_LISTS) > $(BUILD)/firmware.all
	perl embed-firmware.pl $(BUNDLE) $(BUILD)/firmware.all $(FWDIR)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
