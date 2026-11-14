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

# RadeonNI's user-client protocol (hw/rdn_user.h), which the accelerator
# speaks unchanged: radeonni/hw/rdn_user.h in this repository.
RDN_HW   = $(LIB)/radeonni/hw

CPPFLAGS = -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-nostdinc -Icompat -I$(KROOT)/PrivateHeaders -I$(KROOT)/Headers \
	-I$(LIB)/shared -I$(LIB)/amdgpu -I$(RDN_HW)
CFLAGS   = -arch ppc -Os -Wall -fno-builtin -fno-common -static \
	-mlong-branch -force_cpusubtype_ALL
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fapple-kext -fcheck-new
# Files taken from Linux do not pass -Wall cleanly with this compiler.
LIBCFLAGS = $(CFLAGS) -std=gnu99 -Wno-unused -Wno-sign-compare

LIB_SRCS = shared/atom/atom.c shared/frb_edid.c shared/frb_dmt.c shared/frb_mem.c shared/frb_splash.c \
	amdgpu/frb_drv.c amdgpu/frb_device.c amdgpu/frb_atombios.c \
	amdgpu/frb_vi.c amdgpu/frb_gmc_v8_0.c amdgpu/frb_gart.c amdgpu/frb_ucode.c amdgpu/frb_smu7.c \
	amdgpu/frb_atombios_conn.c amdgpu/frb_atombios_crtc.c amdgpu/frb_atombios_encoders.c \
	amdgpu/frb_atombios_dp.c amdgpu/frb_atombios_i2c.c amdgpu/frb_dce_v11_0.c amdgpu/frb_dce_audio.c \
	amdgpu/frb_ring.c amdgpu/frb_engines.c amdgpu/frb_gfx_v8_0.c amdgpu/frb_sdma_v3_0.c \
	amdgpu/frb_bo.c amdgpu/frb_rect.c amdgpu/frb_reset.c \
	amdgpu/frb_amd_powerplay.c amdgpu/frb_hwmgr.c amdgpu/frb_hardwaremanager.c amdgpu/frb_pp_psm.c \
	amdgpu/frb_smu_helper.c amdgpu/frb_ppatomctrl.c amdgpu/frb_process_pptables_v1_0.c \
	amdgpu/frb_smu7_hwmgr.c amdgpu/frb_smu7_powertune.c amdgpu/frb_smu7_thermal.c amdgpu/frb_smu7_clockpowergating.c \
	amdgpu/frb_polaris10_smumgr.c \
	amdgpu/frb_uvd.c amdgpu/frb_uvd_v6_0.c amdgpu/frb_uvd_h264.c
LIB_OBJS = $(addprefix $(BUILD)/lib/,$(LIB_SRCS:.c=.o))
OBJS     = $(BUILD)/Firebrand.o $(BUILD)/FirebrandCore.o $(BUILD)/FirebrandFramebuffer.o \
	$(BUILD)/FirebrandAccel.o $(BUILD)/FirebrandAccel2D.o $(BUILD)/FirebrandSurface.o $(BUILD)/FirebrandVideo.o \
	$(BUILD)/kmod_info.o $(BUILD)/leopard.o $(LIB_OBJS)
LIB_HDRS = $(wildcard $(LIB)/shared/*.h $(LIB)/shared/atom/*.h $(LIB)/amdgpu/*.h $(LIB)/amdgpu/include/pp/*.h)

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
$(BUNDLE): $(OBJS) Info.plist firmware.list embed-firmware.pl
	@mkdir -p $(BUNDLE)/Contents/MacOS
	g++ -arch ppc -static -nostdlib -r $(OBJS) -lkmodc++ -lkmod -lcc_kext \
		-o $(BUNDLE)/Contents/MacOS/$(KEXT)
	cp Info.plist $(BUNDLE)/Contents/Info.plist
	perl embed-firmware.pl $(BUNDLE) firmware.list $(FWDIR)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
