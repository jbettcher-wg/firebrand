/*
 * RadeonNI's class names, as empty bases of Firebrand's classes
 * (docs/accel-interface.md, section 0 and 2.1).
 *
 * User space finds the card by concrete class: RadeonNI's GL bundle, its 2D
 * plug-in (GA) and FrostByte call IOServiceMatching("RadeonNIAccel") for the
 * accelerator and IOServiceMatching("RadeonNI") for the framebuffer, and
 * IOKit matches a class name through metaCast, i.e. the class and its
 * bases. So FirebrandAccel derives from RadeonNIAccel : IOAccelerator and
 * FirebrandFramebuffer from RadeonNI : IOFramebuffer, two abstract classes
 * with nothing in them. ioreg -c RadeonNIAccel / -c RadeonNI then list
 * Firebrand's objects, and that existing user space works unchanged.
 *
 * RadeonNI.kext AND Firebrand.kext ARE MUTUALLY EXCLUSIVE until RadeonNI's
 * SI/CIK support is folded into Firebrand: both define the classes RadeonNI
 * and RadeonNIAccel, and a kext whose class names are taken does not load.
 * That needs an SI/CIK card and a Polaris in one machine, which the project
 * does not plan for (decision of 2026-11-13). Build with
 * FRB_RADEONNI_COMPAT=0 to leave the names out; existing user space then
 * does not find Firebrand.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_COMPAT_H
#define FIREBRAND_COMPAT_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOAccelerator.h>

#ifndef FRB_RADEONNI_COMPAT
#define FRB_RADEONNI_COMPAT	1
#endif

#if FRB_RADEONNI_COMPAT
class RadeonNI : public IOFramebuffer
{
	OSDeclareAbstractStructors(RadeonNI)
};

class RadeonNIAccel : public IOAccelerator
{
	OSDeclareAbstractStructors(RadeonNIAccel)
};

#define FRB_FB_BASE	RadeonNI
#define FRB_ACCEL_BASE	RadeonNIAccel
#else
#define FRB_FB_BASE	IOFramebuffer
#define FRB_ACCEL_BASE	IOAccelerator
#endif

#endif
