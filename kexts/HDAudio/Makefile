# Builds HDAudio.kext on Mac OS X 10.5.8 (PowerPC) with Xcode 3.1 (Apple gcc 4.0.1);
# scripts/build-mac.sh runs it and checks the relocations.
#
# One source and no link step: the object IS the kext's executable. -mlong-branch makes every
# call a JBSR (a kext loads about 2.3 GB away from the kernel's text, out of a 24-bit branch's
# reach), and an `ld -r` through ld64 would turn those back into 24-bit branches.
#
# Copyright (c) 2026 Jordan Bettcher
# SPDX-License-Identifier: MIT

NAME   = HDAudio
K     ?= /System/Library/Frameworks/Kernel.framework/Headers
BUILD  = build
CC     = gcc-4.0
CFLAGS = -arch ppc -static -mlong-branch -fno-common -msoft-float \
	-DAPPLE -DKERNEL -DKERNEL_PRIVATE -DXNU_KERNEL_PRIVATE -DPRIVATE \
	-D__MACHO__=1 -DPPC -Dppc -D__PPC__ -I$(K) -I. -O2 -Wall

all: $(BUILD)/$(NAME).kext

$(BUILD)/$(NAME).o: $(NAME).cpp hda_calc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fapple-kext -fno-exceptions -fno-rtti -c $(NAME).cpp -o $@

# a flat bundle: Info.plist and the executable side by side
$(BUILD)/$(NAME).kext: $(BUILD)/$(NAME).o Info.plist
	rm -rf $@
	mkdir -p $@
	cp Info.plist $@/Info.plist
	cp $(BUILD)/$(NAME).o $@/$(NAME)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
