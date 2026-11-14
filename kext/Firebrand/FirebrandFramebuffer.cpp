/*
 * FirebrandFramebuffer: see FirebrandFramebuffer.h. The IOFramebuffer
 * methods follow RadeonNI's (RadeonNI.cpp), cut to one depth and one
 * connection; the hardware is FirebrandCore's.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <pexpert/pexpert.h>

#include "Firebrand.h"
#include "FirebrandFramebuffer.h"
#include "FirebrandAccel.h"
extern "C" {
#include "frb_splash.h"
}

#define super FRB_FB_BASE
OSDefineMetaClassAndStructors(FirebrandFramebuffer, FRB_FB_BASE)

enum { kFrbPowerSleep, kFrbPowerIdle, kFrbPowerOn, kFrbPowerCount };

/* the hot-plug poll is 500 ms: 5 s after a mode set before a lost HPD line counts */
enum { kUnplugGracePolls = 10 };
/* a line up with no sink answering (no DPCD on AUX): asked again every 3 s, not every poll */
enum { kNoSinkRetryPolls = 4 };

/* RadeonNI's depth indices: the lowest first, the window server starts at 32 */
enum { kDepth8 = 0, kDepth16 = 1, kDepth32 = 2, kDepthCount = 3 };
static const UInt32 kDepthBits[kDepthCount] = { 8, 16, 32 };

/* --- modes ---------------------------------------------------------------- */

const struct frb_mode *FirebrandFramebuffer::modeForID(IODisplayModeID id)
{
	return (id >= 1 && (UInt32)id <= fModeCount) ? &fModes[id - 1] : 0;
}

/*
 * What frb_display_set_mode will take: within the DCE's clock, carried by
 * the sink's DP link (24 bpp; FirebrandMaxLinkRate caps the rate, as in
 * frb_dp_set_link_config), and a surface that fits below the driver's VRAM.
 */
bool FirebrandFramebuffer::modeSettable(const struct frb_mode *mode)
{
	struct frb_display *d = fCore.display();
	UInt32 pitch;

	if (fHeadless && mode && mode->clock)
		return fCore.surfaceOffset() + ((mode->hdisplay + 63) & ~63u) * 4 * mode->vdisplay <=
			fCore.adev()->layout.reserved_start;
	if (!d || fConn < 0 || !linkCarries(fConn, mode))
		return false;
	pitch = (mode->hdisplay + 63) & ~63u;
	return fCore.surfaceOffset() + pitch * 4 * mode->vdisplay <= fCore.adev()->layout.reserved_start;
}

/* the sink on conn takes mode: its DP link at 24 bpp, or its TMDS link (frb_display_mode_valid) */
bool FirebrandFramebuffer::linkCarries(int conn, const struct frb_mode *mode)
{
	struct frb_display *d = fCore.display();

	if (!d || conn < 0 || conn >= d->num_connectors)
		return false;
	return frb_display_mode_valid(&d->conn[conn], mode, fCore.maxLinkRate()) != 0;
}

/*
 * Headless (no monitor; FirebrandHeadless, on unless frb_headless=0): a
 * framebuffer the window server can use with nothing scanning it out, for
 * the accelerator, OpenGL and screenshots on a machine without a monitor.
 * VESA DMT modes, 1920x1080 first.
 */
bool FirebrandFramebuffer::buildHeadlessModes(void)
{
	static const UInt16 sizes[][2] = { { 1920, 1080 }, { 1920, 1200 }, { 1600, 1200 }, { 1280, 1024 }, { 1024, 768 } };
	UInt32 i, k, bytes;

	fModeCount = 0;
	fSurfaceBytes = 0;
	for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]) && fModeCount < kMaxModes; k++)
		for (i = 0; i < frb_dmt_mode_count; i++) {
			const struct frb_mode *m = &frb_dmt_modes[i];

			if (m->hdisplay != sizes[k][0] || m->vdisplay != sizes[k][1] || frb_mode_hz(m) != 60)
				continue;
			fModes[fModeCount] = *m;
			if (!modeSettable(&fModes[fModeCount]))
				break;
			bytes = ((m->hdisplay + 63) & ~63u) * 4 * m->vdisplay;
			if (bytes > fSurfaceBytes)
				fSurfaceBytes = bytes;
			IOLog("FirebrandFramebuffer: headless mode %lu: %ux%u@60\n", (unsigned long)fModeCount + 1,
			      m->hdisplay, m->vdisplay);
			fModeCount++;
			break;
		}
	fPreferred = 1;
	/*
	 * Hot-plug: the window server maps the aperture once, so a monitor
	 * that comes later gets only the modes that fit it; keep room for
	 * 3840x2160.
	 */
	if (fHotplug) {
		bytes = 3840 * 4 * 2160;
		if (bytes > fSurfaceBytes &&
		    fCore.surfaceOffset() + bytes + FirebrandCore::kCursorBytes <= fCore.adev()->layout.reserved_start)
			fSurfaceBytes = bytes;
	}
	fSurfaceBytes = (fSurfaceBytes + 4095) & ~4095u;
	return fModeCount > 0;
}

bool FirebrandFramebuffer::buildModes(void)
{
	struct frb_connector *conn = &fCore.display()->conn[fConn];
	int pref = -1, n, i, firstSafe = -1;

	fModeCount = 0;
	fSurfaceBytes = 0;
	if (conn->edid_len < 128)
		return false;
	n = frb_edid_modes(conn->edid, conn->edid_len, fModes, kMaxModes, &pref);
	if (n <= 0)
		return false;
	fModeCount = n;
	for (i = 0; i < n; i++) {
		UInt32 bytes = ((fModes[i].hdisplay + 63) & ~63u) * 4 * fModes[i].vdisplay;
		bool safe = modeSettable(&fModes[i]);

		if (safe && firstSafe < 0)
			firstSafe = i;
		if (safe && bytes > fSurfaceBytes)
			fSurfaceBytes = bytes;
		IOLog("FirebrandFramebuffer: mode %d: %ux%u@%u, %u kHz%s%s\n", i + 1, fModes[i].hdisplay,
		      fModes[i].vdisplay, frb_mode_hz(&fModes[i]), fModes[i].clock, safe ? "" : " (not settable)",
		      i == pref ? " (EDID preferred)" : "");
	}
	if (firstSafe < 0)
		return false;
	fPreferred = (pref >= 0 && modeSettable(&fModes[pref]) ? pref : firstSafe) + 1;
	fSurfaceBytes = (fSurfaceBytes + 4095) & ~4095u;
	return true;
}

IOReturn FirebrandFramebuffer::programMode(IODisplayModeID id, IOIndex depth, int pattern)
{
	IOReturn r;

	fCore.lock();
	r = programModeLocked(id, depth, pattern);
	fCore.unlock();
	return r;
}

/*
 * With the core's lock held. Headless: the surface only. With the display
 * powered down (DPMS), the mode is remembered and set when it wakes.
 */
IOReturn FirebrandFramebuffer::programModeLocked(IODisplayModeID id, IOIndex depth, int pattern)
{
	const struct frb_mode *mode = modeForID(id);
	UInt32 bpp;
	int r;

	if (!mode || !modeSettable(mode) || depth < 0 || depth >= kDepthCount)
		return kIOReturnUnsupportedMode;
	bpp = kDepthBits[depth];
	if (pattern >= 0 && pattern != FRB_PATTERN_SPLASH && bpp != 32)
		pattern = -1;		/* the test patterns are 32 bpp; the splash is any depth */
	if (fHeadless || !fConnected) {
		fCore.setHeadlessScreen(fCore.surfaceOffset(), frb_display_align_pitch(mode->hdisplay, bpp),
					mode->hdisplay, mode->vdisplay, bpp);
		IOLog("FirebrandFramebuffer: mode %ld (%ux%u, %lu bpp) with no monitor: nothing scans it out\n",
		      (long)id, mode->hdisplay, mode->vdisplay, (unsigned long)bpp);
		fCurrentMode = id;
		fCurrentDepth = depth;
		fModeSet = true;
		loadColorsLocked();
		return kIOReturnSuccess;
	}
	if (fDpms && !(fFbPowerOn && fDisplayPowerOn)) {
		IOLog("FirebrandFramebuffer: mode %ld at %lu bpp asked while the display sleeps: set when it wakes\n",
		      (long)id, (unsigned long)bpp);
		fCurrentMode = id;
		fCurrentDepth = depth;
		fModeSet = true;
		return kIOReturnSuccess;
	}
	r = fCore.programMode(fConn, mode, pattern, fCore.maxLinkRate(), bpp);
	IOLog("FirebrandFramebuffer: mode %ld (%ux%u@%u, %lu bpp): %d\n", (long)id, mode->hdisplay, mode->vdisplay,
	      frb_mode_hz(mode), (unsigned long)bpp, r);
	if (r)
		return kIOReturnIOError;
	fCore.setHeadlessScreen(0, 0, 0, 0);
	/*
	 * A sink re-syncing to new timing may drop its HPD line for a second
	 * or two: the LG did after the window server's first mode set,
	 * 2026-11-14 00:44:53-55, and taking that for an unplug in the middle
	 * of the window server's start left its screen a solid fill. A lost
	 * line counts only once it outlasts kUnplugGracePolls.
	 */
	fUnplugGrace = kUnplugGracePolls;
	fGraceNoted = false;
	fCurrentMode = id;
	fCurrentDepth = depth;
	fModeSet = true;
	fOutputOn = true;
	if (pattern == FRB_PATTERN_SPLASH && bpp == 8)
		splashColorsLocked(mode);
	loadColorsLocked();
	return kIOReturnSuccess;
}

/*
 * At 8 bpp the splash is palette indices: its logo's palette goes in the
 * colour table (the window server loads its own when it starts).
 */
void FirebrandFramebuffer::splashColorsLocked(const struct frb_mode *mode)
{
	uint32_t scale, i;	/* not UInt32: unsigned long on this kernel */
	const struct frb_splash_logo *l = frb_splash_pick(mode->hdisplay, mode->vdisplay, &scale);

	for (i = 0; i < 256; i++) {
		UInt32 rgb = i < l->colours ? l->palette[i] : 0;

		fClut[0][i] = (UInt16)(((rgb >> 16) & 0xff) * 0x101);
		fClut[1][i] = (UInt16)(((rgb >> 8) & 0xff) * 0x101);
		fClut[2][i] = (UInt16)((rgb & 0xff) * 0x101);
	}
}

/*
 * The picture before the window server draws. FirebrandBootPattern:
 * "splash" (the grey with the Firebrand logo, shared/frb_splash.c), "bars"
 * (the colour bars), "black", "grey", a FRB_PATTERN_* number, or "auto",
 * the default: the splash on a card running within the stages its family
 * table row has verified, the bars on a card running past them
 * (FirebrandBringUp), so a card in bring-up shows the test screen and a
 * verified one what release shows.
 */
int FirebrandFramebuffer::bootPattern(void)
{
	OSObject *o = getProperty("FirebrandBootPattern");
	OSString *s = OSDynamicCast(OSString, o);
	OSNumber *n = OSDynamicCast(OSNumber, o);
	const struct frb_device *adev = fCore.adev();

	if (n)
		return (int)n->unsigned32BitValue();
	if (s && s->isEqualTo("splash"))
		return FRB_PATTERN_SPLASH;
	if (s && s->isEqualTo("bars"))
		return FRB_PATTERN_BARS;
	if (s && s->isEqualTo("black"))
		return FRB_PATTERN_BLACK;
	if (s && s->isEqualTo("grey"))
		return FRB_PATTERN_GREY;
	return adev->row && adev->stage_done <= adev->row->max_stage ? FRB_PATTERN_SPLASH : FRB_PATTERN_BARS;
}

/*
 * The first mode with the boot picture, from start() as RadeonNI lights its
 * splash there (a monitor present): on screen from the driver's start until
 * the window server sets its own mode. Headless, the surface only.
 */
IOReturn FirebrandFramebuffer::showBootPicture(void)
{
	int pattern = bootPattern();
	const struct frb_device *adev = fCore.adev();
	IOReturn r = programMode(fPreferred, kDepth32, pattern);

	IOLog("FirebrandFramebuffer: boot picture: %s (%s, verified to stage %s, running to %s): %s\n",
	      pattern == FRB_PATTERN_SPLASH ? "the splash" : pattern == FRB_PATTERN_BARS ? "the colour bars" :
	      "a test fill", adev->row ? adev->row->board : "no row",
	      adev->row ? frb_stage_name((enum frb_stage)adev->row->max_stage) : "-",
	      frb_stage_name(adev->stage_done), r == kIOReturnSuccess ? "set" : "not set");
	return r;
}

/*
 * The CRTC's LUT (core lock held): the gamma ramp at 16 and 32 bpp; at 8
 * bpp the colour table, each entry through the gamma ramp, as an NDRV's
 * SetEntries applies the gamma to the CLUT.
 */
void FirebrandFramebuffer::loadColorsLocked(void)
{
	struct frb_display *d = fCore.display();
	UInt16 (*t)[256];
	UInt32 ch, i;

	if (!d)
		return;	/* headless the store is still kept (frbctl lut); the hardware only takes it on a running CRTC */
	if (fCurrentDepth != kDepth8) {
		frb_display_set_gamma(fCore.adev(), d, fGamma[0], fGamma[1], fGamma[2]);
		return;
	}
	t = (UInt16 (*)[256])IOMalloc(3 * 256 * sizeof(UInt16));
	if (!t)
		return;
	for (ch = 0; ch < 3; ch++)
		for (i = 0; i < 256; i++)
			t[ch][i] = fGamma[ch][fClut[ch][i] >> 8];
	frb_display_set_gamma(fCore.adev(), d, t[0], t[1], t[2]);
	IOFree(t, 3 * 256 * sizeof(UInt16));
}

/* --- IOService ------------------------------------------------------------- */

IOService *FirebrandFramebuffer::probe(IOService *provider, SInt32 *score)
{
	if (!FirebrandProbeDevice(provider) || !FirebrandFramebufferWanted(this))
		return 0;
	return super::probe(provider, score);
}

/*
 * The hardware first, as RadeonNI does: the stages to dce (FirebrandStage in
 * this personality), the monitor and its modes; then IOFramebuffer.
 */
bool FirebrandFramebuffer::start(IOService *provider)
{
	IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, provider);
	const struct frb_mode *m;

	fConn = -1;
	fCurrentDepth = kDepth32;
	{
		UInt32 i;

		/* hw_init's ramp, i << 8 (frb_dce_v11_0_load_lut), and a grey colour table */
		for (i = 0; i < 256; i++)
			fGamma[0][i] = fGamma[1][i] = fGamma[2][i] = fClut[0][i] = fClut[1][i] = fClut[2][i] =
				(UInt16)(i << 8);
	}
	{
		int arg;

		fHotplug = !(PE_parse_boot_arg("frb_hotplug", &arg) && !arg) &&
			getProperty("FirebrandHotplug") != kOSBooleanFalse;
		fDpms = !(PE_parse_boot_arg("frb_dpms", &arg) && !arg) &&
			getProperty("FirebrandDPMS") != kOSBooleanFalse;
		fIdleDoze = PE_parse_boot_arg("frb_idledoze", &arg) ? arg != 0 :
			getProperty("FirebrandIdleDoze") == kOSBooleanTrue;
		fFbPowerOn = fDisplayPowerOn = true;
	}
	if (!dev || !fCore.start(this, dev))
		return false;
	fCoreStarted = true;
	fCore.lock();
	if (fCore.adev()->stage_done < FRB_STAGE_DCE)
		IOLog("FirebrandFramebuffer: the bring-up stopped at stage %s; a framebuffer needs dce\n",
		      frb_stage_name(fCore.adev()->stage_done));
	else if ((fConn = fCore.firstConnected()) < 0) {
		int arg;

		fHeadless = !(PE_parse_boot_arg("frb_headless", &arg) && !arg) &&
			getProperty("FirebrandHeadless") != kOSBooleanFalse;
		IOLog("FirebrandFramebuffer: no monitor%s\n",
		      fHeadless ? "; headless (FirebrandHeadless)" : "");
		if (fHeadless && !buildHeadlessModes())
			IOLog("FirebrandFramebuffer: no headless mode fits\n");
	} else if (!buildModes())
		IOLog("FirebrandFramebuffer: the monitor offers no mode this card can set\n");
	fCore.unlock();
	fConnected = fConn >= 0;
	if ((fConn < 0 && !fHeadless) || !fModeCount || !fSurfaceBytes) {
		fCore.stop();
		fCoreStarted = false;
		return false;
	}
	if (fConnected)
		showBootPicture();
	if (!super::start(provider)) {
		IOLog("FirebrandFramebuffer: IOFramebuffer::start failed\n");
		fCore.stop();
		fCoreStarted = false;
		return false;
	}
	m = modeForID(fPreferred);
	IOLog("FirebrandFramebuffer: started on connector %d%s: %lu modes, startup %ux%u@%u, "
	      "surface %lu KB at VRAM 0x%08lx\n", fConn, fHeadless ? " (headless)" : "", (unsigned long)fModeCount,
	      m->hdisplay, m->vdisplay,
	      frb_mode_hz(m), (unsigned long)(fSurfaceBytes >> 10), (unsigned long)fCore.surfaceOffset());
	/*
	 * The accelerator, as RadeonNI announces it (RadeonNI.cpp, after
	 * IOFramebuffer::start): only when asked for, since the window server
	 * and OpenGL act on it as soon as it is there. Its heap starts at the
	 * first megabyte boundary past the biggest settable surface.
	 */
	/*
	 * The hardware cursor's surface follows the biggest scanout surface
	 * (amdgpu pins a 128x128 cursor bo in VRAM); the window server's
	 * cursor then never touches the screen's pixels, which the GPU's 2D
	 * work moves at the same time (the software cursor's save-under is
	 * the CPU's).
	 */
	{
		int arg;

		fCursorOffset = fCore.cursorOffset(fSurfaceBytes);
		fHWCursor = !(PE_parse_boot_arg("frb_hwcursor", &arg) && !arg) &&
			getProperty("FirebrandHWCursor") != kOSBooleanFalse &&
			fCursorOffset + FirebrandCore::kCursorBytes <= fCore.adev()->layout.reserved_start;
		if (fHWCursor) {
			fCursorData = (UInt32 *)IOMalloc(FirebrandCore::kCursorBytes);
			fHWCursor = fCursorData != 0;
		}
		IOLog("FirebrandFramebuffer: hardware cursor %s (surface at VRAM 0x%08lx)\n",
		      fHWCursor ? "on" : "off (FirebrandHWCursor / frb_hwcursor=0)", (unsigned long)fCursorOffset);
	}
	if (fCore.accelWanted()) {
		UInt32 scanoutEnd = (fCursorOffset + FirebrandCore::kCursorBytes + 0xfffff) & ~0xfffffu;

		fAccel = FirebrandAccel::withOwner(this, &fCore, this, dev, scanoutEnd);
		if (fAccel)
			fCore.setScanoutLimit(scanoutEnd);
	}
	startHotplug();
	return true;
}

/*
 * kextunload terminates every instance first (IOCatalogue's
 * _terminateDrivers) and gives up when one refuses. Refuse while IOGraphics
 * has opened this framebuffer: IOGraphics 1.7.3 keeps it in its
 * gAllFramebuffers for good and holds references stop cannot drop, so the
 * kext could not go anyway, and the old way out (IOFramebuffer::
 * requestTerminate from stop, below) panicked a test machine on 2026-11-13: an
 * instruction fetch from 0x7D7343A4 out of requestTerminate's call to
 * IOEventSource::closeGate. Reboot to unload; after frbctl fb detach
 * the old path is still taken, for working on it.
 */
bool FirebrandFramebuffer::terminate(IOOptionBits options)
{
	if (opened && !fCore.detaching()) {
		IOLog("FirebrandFramebuffer: not terminating: IOGraphics opened this framebuffer and "
		      "cannot let it go; reboot to unload (frbctl fb detach first to try anyway)\n");
		return false;
	}
	return super::terminate(options);
}

void FirebrandFramebuffer::stop(IOService *provider)
{
	bool detaching = fCore.detaching();

	stopHotplug();
	/* the accelerator takes no more work and lets the ring drain; it goes once the engines are halted */
	if (fAccel)
		fAccel->prepareToRetire();
	super::stop(provider);
	if (detaching) {
		/*
		 * IOGraphics keeps every framebuffer it has opened in its
		 * gAllFramebuffers and lets one go only in close() once
		 * requestTerminate has put it on its runaway list
		 * (IOFramebuffer.cpp, IOGraphics-305); and it never releases
		 * vramMap or userAccessRanges, which hold our aperture range.
		 * After frbctl fb detach, walk its own path and drop those.
		 * Measured on 10.5.8 (IOGraphicsFamily 1.7.3): the aperture
		 * range is freed, but an opened framebuffer still holds ten
		 * references at stop that this path does not release, so the
		 * kext stays until a reboot (open item).
		 */
		IOFramebuffer::requestTerminate(provider, 0);
		IOFramebuffer::close();
		if (vramMap) {
			vramMap->release();
			vramMap = 0;
		}
		if (userAccessRanges)
			userAccessRanges->flushCollection();
	}
	if (fCoreStarted) {
		fCore.stop();
		fCoreStarted = false;
	}
	if (fAccel) {
		fAccel->retire();
		fAccel->release();
		fAccel = 0;
	}
	IOLog("FirebrandFramebuffer: stopped\n");
}

void FirebrandFramebuffer::free(void)
{
	IOLog("FirebrandFramebuffer: freed\n");
	if (fCursorData) {
		IOFree(fCursorData, FirebrandCore::kCursorBytes);
		fCursorData = 0;
	}
	fCore.free();
	super::free();
}

bool FirebrandFramebuffer::serializeProperties(OSSerialize *s) const
{
	bool ok;

	if (FirebrandSerializeWithoutFirmware(this, s, &ok))
		return ok;
	return super::serializeProperties(s);
}

IOReturn FirebrandFramebuffer::newUserClient(task_t task, void *security, UInt32 type,
					     IOUserClient **handler)
{
	if (type == FRB_UC_TYPE)
		return FirebrandNewControlClient(this, task, security, type, handler);
	if (fCore.detaching()) {
		IOLog("FirebrandFramebuffer: detached: refusing IOGraphics client type %lu\n", (unsigned long)type);
		return kIOReturnOffline;
	}
	return super::newUserClient(task, security, type, handler);
}

unsigned long FirebrandFramebuffer::maxCapabilityForDomainState(IOPMPowerFlags state)
{
	return (state & IOPMPowerOn) ? kFrbPowerOn : kFrbPowerSleep;
}

unsigned long FirebrandFramebuffer::initialPowerStateForDomainState(IOPMPowerFlags state)
{
	return maxCapabilityForDomainState(state);
}

unsigned long FirebrandFramebuffer::powerStateForDomainState(IOPMPowerFlags state)
{
	return maxCapabilityForDomainState(state);
}

/* --- IOFramebuffer --------------------------------------------------------- */

/*
 * The first mode, with the boot picture if start() did not already set it
 * (bootPattern: the splash on a verified card, the colour bars in
 * bring-up), and the power states
 * RadeonNI registers: no system sleep, since nothing restores the GPU yet.
 *
 * No idle doze either, unless FirebrandIdleDoze / frb_idledoze=1. With
 * system sleep prevented, the root domain's idle sleep becomes Doze
 * ("Sleep prevented by kIOPMPreventSystemSleep flag"), and Doze sends
 * IOGraphics kIOMessageSystemWillSleep: gIOFBSystemPower goes false and
 * IOFramebuffer::extEntry parks the window server, and every other
 * IOFramebuffer caller, in an uninterruptible sleepGate until a key press
 * wakes the system. A reboot from ssh in that state never finishes: launchd
 * signals the parked process and waits for it forever, before reboot(2),
 * so the kernel's halt/restart notices (PEHaltRestart, systemWillShutdown)
 * never come (2026-11-13 22:04 and 23:15). The CPUs keep running
 * in Doze on this platform, so it saves nothing that display sleep (DPMS)
 * does not; kIOPMPreventIdleSleep in the idle and on states holds the root
 * domain out of idle sleep. A demand sleep (the Apple menu) still dozes.
 */
IOReturn FirebrandFramebuffer::enableController(void)
{
	IOPMPowerFlags hold = fIdleDoze ? 0 : kIOPMPreventIdleSleep;
	IOReturn result;

	result = fModeSet ? kIOReturnSuccess : showBootPicture();
	if (result != kIOReturnSuccess || fPowerRegistered)
		return result;
	IOPMPowerState states[kFrbPowerCount] = {
		{ 1, kIOPMPreventSystemSleep, 0, 0, 0,0,0,0,0,0,0,0 },
		{ 1, kIOPMPreventSystemSleep | hold, 0, IOPMPowerOn, 0,0,0,0,0,0,0,0 },
		{ 1, kIOPMPreventSystemSleep | hold | IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0,0,0,0,0,0,0,0 }
	};
	result = registerPowerDriver(this, states, kFrbPowerCount);
	if (result != kIOReturnSuccess)
		return result;
	IOLog("FirebrandFramebuffer: %s\n", fIdleDoze ? "idle doze allowed (FirebrandIdleDoze)" :
	      "no idle doze (it parks the window server; FirebrandIdleDoze / frb_idledoze=1 allow it)");
	fPowerRegistered = true;
	fPowerState = kFrbPowerOn;
	temporaryPowerClampOn();
	changePowerStateTo(kFrbPowerIdle);
	setProperty("SystemSleepSupported", false);
	return kIOReturnSuccess;
}

/*
 * The visible surface: the same range in every mode, as big as the biggest
 * settable one (the window server maps it once and keeps the mapping across
 * mode changes). Unaccelerated, the VRAM range is the same.
 */
IODeviceMemory *FirebrandFramebuffer::getApertureRange(IOPixelAperture aperture)
{
	if (aperture != kIOFBSystemAperture)
		return 0;
	return FirebrandSubDeviceMemory::withDeviceSubRange(fCore.aperture(), fCore.surfaceOffset(),
							    fSurfaceBytes);
}

IODeviceMemory *FirebrandFramebuffer::getVRAMRange(void)
{
	return getApertureRange(kIOFBSystemAperture);
}

/* The card's memory as IOFBMemorySize (32-bit, so 2047 MB at most, as RadeonNI). */
IOReturn FirebrandFramebuffer::setupForCurrentConfig(void)
{
	IOReturn err = super::setupForCurrentConfig();
	UInt64 bytes = fCore.adev()->gmc.real_vram_size;

	if (bytes > 0x7ff00000ull)
		bytes = 0x7ff00000ull;
	if (bytes > fSurfaceBytes)
		setProperty(kIOFBMemorySizeKey, (UInt32)bytes, 32);
	return err;
}

const char *FirebrandFramebuffer::getPixelFormats(void)
{
	static const char formats[] = IO8BitIndexedPixels "\0" IO16BitDirectPixels "\0" IO32BitDirectPixels "\0";

	return formats;
}

IOItemCount FirebrandFramebuffer::getDisplayModeCount(void)
{
	return fModeCount;
}

IOReturn FirebrandFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
	UInt32 i;

	for (i = 0; i < fModeCount; i++)
		allDisplayModes[i] = i + 1;
	return kIOReturnSuccess;
}

/* Valid for every mode, Safe only for what modeSettable() takes (RadeonNI's rule). */
IOReturn FirebrandFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
							    IODisplayModeInformation *info)
{
	const struct frb_mode *mode = modeForID(displayMode);

	if (!mode)
		return kIOReturnUnsupportedMode;
	bzero(info, sizeof(*info));
	info->nominalWidth = mode->hdisplay;
	info->nominalHeight = mode->vdisplay;
	info->refreshRate = (IOFixed1616)(((UInt64)mode->clock * 1000ULL << 16) /
					  ((UInt64)mode->htotal * mode->vtotal));
	info->maxDepthIndex = kDepthCount - 1;
	info->flags = kDisplayModeValidFlag;
	if (modeSettable(mode))
		info->flags |= kDisplayModeSafeFlag;
	if (displayMode == fPreferred)
		info->flags |= kDisplayModeDefaultFlag;
	return kIOReturnSuccess;
}

UInt64 FirebrandFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
	return 0;
}

IOReturn FirebrandFramebuffer::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
						   IOPixelAperture aperture, IOPixelInformation *pixelInfo)
{
	const struct frb_mode *mode = modeForID(displayMode);
	UInt32 bpp;

	if (!mode || depth < 0 || depth >= kDepthCount || aperture != kIOFBSystemAperture)
		return kIOReturnUnsupportedMode;
	bpp = kDepthBits[depth];
	bzero(pixelInfo, sizeof(*pixelInfo));
	pixelInfo->bytesPerRow = frb_display_align_pitch(mode->hdisplay, bpp) * (bpp / 8);
	pixelInfo->bytesPerPlane = 0;
	pixelInfo->bitsPerPixel = bpp;
	pixelInfo->activeWidth = mode->hdisplay;
	pixelInfo->activeHeight = mode->vdisplay;
	switch (depth) {
	case kDepth8:
		pixelInfo->pixelType = kIOCLUTPixels;
		pixelInfo->componentCount = 1;
		pixelInfo->bitsPerComponent = 8;
		pixelInfo->componentMasks[0] = 0xff;
		strncpy(pixelInfo->pixelFormat, IO8BitIndexedPixels, sizeof(pixelInfo->pixelFormat));
		break;
	case kDepth16:
		pixelInfo->pixelType = kIORGBDirectPixels;
		pixelInfo->componentCount = 3;
		pixelInfo->bitsPerComponent = 5;
		pixelInfo->componentMasks[0] = 0x7c00;
		pixelInfo->componentMasks[1] = 0x03e0;
		pixelInfo->componentMasks[2] = 0x001f;
		strncpy(pixelInfo->pixelFormat, IO16BitDirectPixels, sizeof(pixelInfo->pixelFormat));
		break;
	default:
		pixelInfo->pixelType = kIORGBDirectPixels;
		pixelInfo->componentCount = 3;
		pixelInfo->bitsPerComponent = 8;
		pixelInfo->componentMasks[0] = 0x00ff0000;
		pixelInfo->componentMasks[1] = 0x0000ff00;
		pixelInfo->componentMasks[2] = 0x000000ff;
		strncpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
		break;
	}
	return kIOReturnSuccess;
}

IOReturn FirebrandFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
	if (displayMode)
		*displayMode = fCurrentMode;
	if (depth)
		*depth = fCurrentDepth;
	return kIOReturnSuccess;
}

IOReturn FirebrandFramebuffer::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
	if (depth < 0 || depth >= kDepthCount)
		return kIOReturnUnsupportedMode;
	if (fModeSet && displayMode == fCurrentMode && depth == fCurrentDepth)
		return kIOReturnSuccess;
	return programMode(displayMode, depth, -1);
}

IOReturn FirebrandFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
	if (displayMode)
		*displayMode = fPreferred;
	if (depth)
		*depth = kDepth32;
	return kIOReturnSuccess;
}

/* The colour table for 8 bpp (RadeonNI's): 16-bit components, by value or from index on. */
IOReturn FirebrandFramebuffer::setCLUTWithEntries(IOColorEntry *colors, UInt32 index,
						  UInt32 numEntries, IOOptionBits options)
{
	UInt32 i, slot;

	if (!colors)
		return kIOReturnBadArgument;
	fCore.lock();
	for (i = 0; i < numEntries; i++) {
		slot = (options & kSetCLUTByValue) ? colors[i].index : index + i;
		if (slot > 255)
			continue;
		fClut[0][slot] = colors[i].red;
		fClut[1][slot] = colors[i].green;
		fClut[2][slot] = colors[i].blue;
	}
	if (fModeSet && fCurrentDepth == kDepth8)
		loadColorsLocked();
	fCore.unlock();
	return kIOReturnSuccess;
}

/*
 * The gamma ramp: `channelCount` tables of `dataCount` entries, each
 * `dataWidth` bits wide, red first; kept 16-bit (the CRTC's gamma store,
 * which the LUT takes as dce_v11_0_crtc_load_lut does) and loaded at 16
 * and 32 bpp, and at 8 bpp under the colour table (loadColorsLocked).
 */
IOReturn FirebrandFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount,
					     UInt32 dataWidth, void *data)
{
	UInt32 i, ch, v;

	if (!data || !dataCount || (channelCount != 1 && channelCount != 3) || dataWidth < 8 || dataWidth > 16)
		return kIOReturnBadArgument;
	fCore.lock();
	for (i = 0; i < 256; i++) {
		UInt32 src = i * dataCount / 256;

		for (ch = 0; ch < 3; ch++) {
			UInt32 table = (channelCount == 3 ? ch : 0) * dataCount;

			v = dataWidth == 8 ? ((UInt8 *)data)[table + src] : ((UInt16 *)data)[table + src];
			fGamma[ch][i] = (UInt16)(v << (16 - dataWidth));
		}
	}
	if (fModeSet)
		loadColorsLocked();
	fCore.unlock();
	return kIOReturnSuccess;
}

IOReturn FirebrandFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID displayMode,
							   IOTimingInformation *info)
{
	const struct frb_mode *mode = modeForID(displayMode);
	IODetailedTimingInformationV2 *t;

	if (!mode)
		return kIOReturnUnsupportedMode;
	bzero(info, sizeof(*info));
	info->appleTimingID = kIOTimingIDInvalid;
	info->flags = kIODetailedTimingValid;
	t = &info->detailedInfo.v2;
	t->signalConfig = kIODigitalSignal;
	t->pixelClock = (UInt64)mode->clock * 1000ULL;
	t->minPixelClock = t->pixelClock;
	t->maxPixelClock = t->pixelClock;
	t->horizontalActive = mode->hdisplay;
	t->horizontalBlanking = mode->htotal - mode->hdisplay;
	t->horizontalSyncOffset = mode->hsync_start - mode->hdisplay;
	t->horizontalSyncPulseWidth = mode->hsync_end - mode->hsync_start;
	t->verticalActive = mode->vdisplay;
	t->verticalBlanking = mode->vtotal - mode->vdisplay;
	t->verticalSyncOffset = mode->vsync_start - mode->vdisplay;
	t->verticalSyncPulseWidth = mode->vsync_end - mode->vsync_start;
	t->horizontalSyncConfig = (mode->flags & FRB_MODE_NHSYNC) ? 0 : kIOSyncPositivePolarity;
	t->verticalSyncConfig = (mode->flags & FRB_MODE_NVSYNC) ? 0 : kIOSyncPositivePolarity;
	t->numLinks = 1;
	return kIOReturnSuccess;
}

IOItemCount FirebrandFramebuffer::getConnectionCount(void)
{
	return 1;
}

/* Display power (DPMS) is not carried yet: the requests are logged and accepted. */
IOReturn FirebrandFramebuffer::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, UInt32 value)
{
	if (connectIndex != 0)
		return kIOReturnBadArgument;
	switch (attribute) {
	case kConnectionSyncEnable:
		/* the bits name the syncs that are off (DPMS) */
		IOLog("FirebrandFramebuffer: connection syncs 0x%lx\n", (unsigned long)value);
		return setOutputPower(!(value & (kIOHSyncDisable | kIOVSyncDisable)), false);
	case kConnectionPower:
		IOLog("FirebrandFramebuffer: connection power 0x%lx\n", (unsigned long)value);
		return setOutputPower((value & kFBDisplayUsablePowerState) != 0, false);
	case kConnectionPostWake:
		/* the system woke: the monitor may have changed */
		fCore.lock();
		fSenseSteady = 0;
		fCore.unlock();
		return kIOReturnSuccess;
	default:
		return super::setAttributeForConnection(connectIndex, attribute, value);
	}
}

IOReturn FirebrandFramebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, UInt32 *value)
{
	if (connectIndex != 0)
		return kIOReturnBadArgument;
	switch (attribute) {
	case kConnectionSupportsHLDDCSense:
		/* IODisplay probes this capability with a null value pointer */
		return kIOReturnSuccess;
	case kConnectionChanged:
		/*
		 * IOGraphics, after our connect-change interrupt, before the
		 * window server rebuilds its modes (IOFBInterestCallback):
		 * as IONDRVFramebuffer::processConnectChange, the cached
		 * mode list goes, so the rebuild asks us.
		 */
		forgetModeConfig("connection changed");	/* a registry property: no core lock */
		if (value)
			*value = 0;
		return kIOReturnSuccess;
	case kConnectionEnable:
		/* headless: the surface is a display the window server can use */
		if (value)
			*value = (fConnected || fHeadless) ? 1 : 0;
		return kIOReturnSuccess;
	case kConnectionFlags:
		if (value)
			*value = 0;
		return kIOReturnSuccess;
	default:
		return super::getAttributeForConnection(connectIndex, attribute, value);
	}
}

IOReturn FirebrandFramebuffer::setAttribute(IOSelect attribute, UInt32 value)
{
	switch (attribute) {
	case kIOPowerAttribute:
		{
			bool on = value == kFrbPowerOn, wasOn = fPowerState == kFrbPowerOn;

			if (value >= kFrbPowerCount)
				return kIOReturnBadArgument;
			if (value == fPowerState)
				return kIOReturnSuccess;
			if (on != wasOn)
				handleEvent(on ? kIOFBNotifyWillPowerOn : kIOFBNotifyWillPowerOff);
			setOutputPower(on, true);
			fPowerState = value;
			if (on != wasOn)
				handleEvent(on ? kIOFBNotifyDidPowerOn : kIOFBNotifyDidPowerOff);
			IOLog("FirebrandFramebuffer: framebuffer power state %lu\n", (unsigned long)value);
		}
		return kIOReturnSuccess;
	case kIOCapturedAttribute:
		fCaptured = value != 0;
		return kIOReturnSuccess;
	default:
		return super::setAttribute(attribute, value);
	}
}

/* --- the hardware cursor (RadeonNI.cpp's, on dce_v11_0's cursor plane) --------- */

/* IOFramebuffer draws the cursor itself, with the CPU, unless the driver says it has one */
IOReturn FirebrandFramebuffer::getAttribute(IOSelect attribute, UInt32 *value)
{
	if (attribute == kIOHardwareCursorAttribute && fHWCursor) {
		if (value)
			*value = 1;
		return kIOReturnSuccess;
	}
	return super::getAttribute(attribute, value);
}

/*
 * A new cursor picture: IOGraphics converts it to 32-bit ARGB with the
 * colours premultiplied (its software cursor blends it that way), which is
 * the plane's CURSOR_MODE 2. Refused (the software cursor takes over):
 * bigger than FRB_CURSOR_MAX, or no conversion.
 */
IOReturn FirebrandFramebuffer::setCursorImage(void *cursorImage)
{
	IOHardwareCursorDescriptor desc;
	IOHardwareCursorInfo info;
	struct frb_display *d;
	int r = -ENODEV;

	if (!fHWCursor)
		return kIOReturnUnsupported;
	bzero(&desc, sizeof(desc));
	desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
	desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
	desc.height = FRB_CURSOR_MAX;
	desc.width = FRB_CURSOR_MAX;
	desc.bitDepth = 32;
	bzero(&info, sizeof(info));
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = (UInt8 *)fCursorData;
	if (convertCursorImage(cursorImage, &desc, &info) && info.cursorWidth <= FRB_CURSOR_MAX &&
	    info.cursorHeight <= FRB_CURSOR_MAX) {
		fCore.lock();
		r = fCore.loadCursorLocked(fCursorOffset, fCursorData, info.cursorWidth, info.cursorHeight);
		fCore.unlock();
	}
	if (!fCursorLogged || r) {
		fCursorLogged = !r;
		IOLog("FirebrandFramebuffer: cursor image %lux%lu: %s (%d)\n", (unsigned long)info.cursorWidth,
		      (unsigned long)info.cursorHeight, r ? "the software cursor draws it" : "hardware", r);
	}
	if (r) {
		fCore.lock();
		d = fCore.display();
		if (d)
			frb_display_cursor_set(fCore.adev(), d, ~0u, 0, 0);
		fCore.unlock();
		fCore.setHWCursorActive(false);
		return kIOReturnUnsupported;
	}
	fCore.setHWCursorActive(true);
	return kIOReturnSuccess;
}

/* x, y: the picture's top left on this screen (IOFramebuffer took the hot spot off) */
IOReturn FirebrandFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
	struct frb_display *d;

	if (!fHWCursor)
		return kIOReturnUnsupported;
	fCore.lock();
	d = fCore.display();
	if (d)
		frb_display_cursor_move(fCore.adev(), d, x, y, visible);
	fCore.unlock();
	return kIOReturnSuccess;
}

/* The same split as getInformationForDisplayMode(): IODisplay builds its list from this one. */
IOReturn FirebrandFramebuffer::connectFlags(IOIndex connectIndex, IODisplayModeID displayMode,
					    IOOptionBits *flags)
{
	const struct frb_mode *mode = modeForID(displayMode);

	if (connectIndex != 0 || !flags)
		return kIOReturnBadArgument;
	if (!mode)
		return kIOReturnUnsupportedMode;
	*flags = kDisplayModeValidFlag;
	if (modeSettable(mode))
		*flags |= kDisplayModeSafeFlag;
	return kIOReturnSuccess;
}

bool FirebrandFramebuffer::hasDDCConnect(IOIndex connectIndex)
{
	struct frb_display *d = fCore.display();

	return connectIndex == 0 && d && fConnected && fConn >= 0 && d->conn[fConn].edid_len >= 128;
}

/* Blocks are numbered from 1; the EDID was read by stage dce. */
IOReturn FirebrandFramebuffer::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType,
					   IOOptionBits options, UInt8 *data, IOByteCount *length)
{
	struct frb_display *d = fCore.display();
	IOByteCount n;

	if (connectIndex != 0 || !data || !length)
		return kIOReturnBadArgument;
	if (!d || fConn < 0 || blockType != kIODDCBlockTypeEDID || blockNumber < 1 ||
	    blockNumber > d->conn[fConn].edid_len / 128)
		return kIOReturnUnsupported;
	n = *length < 128 ? *length : 128;
	bcopy(d->conn[fConn].edid + (blockNumber - 1) * 128, data, n);
	*length = n;
	return kIOReturnSuccess;
}

/* --- display power (DPMS) and hot-plug: RadeonNI's, on FirebrandCore -------------- */

/*
 * The framebuffer's power state (kIOPowerAttribute) and the display's
 * (kConnectionPower, kConnectionSyncEnable) both have to be on for the
 * output to run. Off: frb_display_off (dce_v11_0's disable: the stream, the
 * transmitter, the CRTC); on: the mode set again, the link trained anew.
 * The surface stays, so the picture comes back as it was.
 */
IOReturn FirebrandFramebuffer::setOutputPower(bool on, bool framebuffer)
{
	struct frb_display *d;
	IOReturn result = kIOReturnSuccess;
	bool want;

	if (!fDpms)
		return kIOReturnSuccess;
	fCore.lock();
	if (framebuffer)
		fFbPowerOn = on;
	else
		fDisplayPowerOn = on;
	want = fFbPowerOn && fDisplayPowerOn;
	d = fCore.display();
	if (fModeSet && fConnected && !fHeadless && d && want != fOutputOn) {
		if (want) {
			result = programModeLocked(fCurrentMode, fCurrentDepth, -1);
			IOLog("FirebrandFramebuffer: output back on: %d\n", (int)result);
		} else {
			frb_display_off(fCore.adev(), d);
			fOutputOn = false;
			IOLog("FirebrandFramebuffer: output off (display sleep)\n");
		}
	}
	fCore.unlock();
	return result;
}

void FirebrandFramebuffer::pollTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
	FirebrandFramebuffer *self = OSDynamicCast(FirebrandFramebuffer, owner);

	if (!self)
		return;
	self->pollHotplug();
	sender->setTimeoutMS(500);
}

/*
 * Twice a second on a work loop of its own: the hot-plug sense lines
 * (stage dce enabled them, dce_v11_0_hpd_init). No interrupts yet: the
 * IH ring is not ported.
 */
void FirebrandFramebuffer::startHotplug(void)
{
	if (!fHotplug || fPollTimer)
		return;
	fCore.lock();
	fSenseLast = senseLocked();
	fSenseSteady = 0;
	fCore.unlock();
	fPollLoop = IOWorkLoop::workLoop();
	if (fPollLoop)
		fPollTimer = IOTimerEventSource::timerEventSource(this, pollTimerFired);
	if (!fPollTimer || fPollLoop->addEventSource(fPollTimer) != kIOReturnSuccess) {
		IOLog("FirebrandFramebuffer: no hot-plug polling (no work loop or timer)\n");
		if (fPollTimer) {
			fPollTimer->release();
			fPollTimer = 0;
		}
		if (fPollLoop) {
			fPollLoop->release();
			fPollLoop = 0;
		}
		return;
	}
	fPollTimer->setTimeoutMS(500);
	IOLog("FirebrandFramebuffer: hot-plug polling on (%s)\n", fConnected ? "a monitor on" : "no monitor yet");
}

void FirebrandFramebuffer::stopHotplug(void)
{
	if (fPollTimer) {
		fPollTimer->cancelTimeout();
		if (fPollLoop)
			fPollLoop->removeEventSource(fPollTimer);
		fPollTimer->release();
		fPollTimer = 0;
	}
	if (fPollLoop) {
		fPollLoop->release();
		fPollLoop = 0;
	}
}

/* With a monitor: its connector's line. Without one: any connector's (it may come on another). */
int FirebrandFramebuffer::senseLocked(void)
{
	struct frb_display *d = fCore.display();
	int i;

	if (!d)
		return 0;
	if (fConnected && fConn >= 0)
		return frb_dce_v11_0_hpd_sense(fCore.adev(), d->conn[fConn].hpd) ? 1 : 0;
	for (i = 0; i < d->num_connectors; i++)
		if (frb_dce_v11_0_hpd_sense(fCore.adev(), d->conn[i].hpd))
			return 1;
	return 0;
}

/*
 * A change has to hold for a second (a waking monitor pulls its line low
 * for a moment, and a plug makes contact in steps). Not while the display
 * sleeps: a monitor in standby may drop the line.
 */
void FirebrandFramebuffer::pollHotplug(void)
{
	bool changed = false;
	int sense;

	fCore.lock();
	if (fConnected && fDpms && !(fFbPowerOn && fDisplayPowerOn)) {
		fSenseSteady = 0;
		fCore.unlock();
		return;
	}
	sense = senseLocked();
	if (fUnplugGrace)
		fUnplugGrace--;
	if (sense != fSenseLast) {
		fSenseLast = sense;
		fSenseSteady = 0;
		fArriveHold = 0;
		fNoSinkNoted = false;
	} else if (sense && !fConnected && fArriveHold) {
		fArriveHold--;		/* a line up with no sink: not every poll */
	} else if (!sense && fConnected && fUnplugGrace) {
		fSenseSteady = 0;	/* the sink re-syncing after a mode set, not (yet) an unplug */
		if (!fGraceNoted) {
			fGraceNoted = true;
			IOLog("FirebrandFramebuffer: connector %d's HPD line is down %lu ms after a mode set: "
			      "the sink re-syncing, not an unplug unless it stays down\n", fConn,
			      (unsigned long)(kUnplugGracePolls - fUnplugGrace) * 500);
		}
	} else if (sense != (fConnected ? 1 : 0) && ++fSenseSteady >= 2) {
		fSenseSteady = 0;
		if (sense) {
			changed = monitorArrivedLocked();
			fArriveHold = changed ? 0 : kNoSinkRetryPolls;
		} else {
			monitorGoneLocked();
			changed = true;
		}
	}
	fCore.unlock();
	if (changed)
		notifyConnectionChange();
}

/*
 * A monitor on a line, the one before or another, maybe on another
 * connector: detect again (DPCD, EDID), the modes that its link carries and
 * that fit the surface the window server mapped at start, then the mode of
 * the size on screen (else the preferred one) with the link trained anew.
 * false: not yet (tried again on the next polls).
 */
bool FirebrandFramebuffer::monitorArrivedLocked(void)
{
	struct frb_display *d = fCore.display();
	struct frb_mode *modes, *old;
	struct frb_connector *c;
	IODisplayModeID id = 0, oldCurrent = fCurrentMode, oldPreferred = fPreferred;
	UInt32 keep = 0, oldCount = fModeCount, bytes;
	int conn, oldConn = fConn, n, i, pref = -1, prefId = 0;
	bool oldHeadless = fHeadless;

	if (!d)
		return false;
	frb_display_detect(fCore.adev(), d);
	conn = fCore.firstConnected();
	if (conn < 0) {
		if (!fNoSinkNoted)
			IOLog("FirebrandFramebuffer: a hot-plug line is up but no sink answers; asking again every %u ms\n",
			      (unsigned)(kNoSinkRetryPolls + 2) * 500);
		fNoSinkNoted = true;
		return false;
	}
	fNoSinkNoted = false;
	c = &d->conn[conn];
	if (c->edid_len < 128) {
		IOLog("FirebrandFramebuffer: connector %d: no EDID yet\n", conn);
		return false;
	}
	modes = (struct frb_mode *)IOMalloc(2 * kMaxModes * sizeof(*modes));
	if (!modes)
		return false;
	old = modes + kMaxModes;
	n = frb_edid_modes(c->edid, c->edid_len, modes, kMaxModes, &pref);
	for (i = 0; i < n; i++) {
		bytes = ((modes[i].hdisplay + 63) & ~63u) * 4 * modes[i].vdisplay;
		if (!linkCarries(conn, &modes[i]) || bytes > fSurfaceBytes)
			continue;
		if (i == pref)
			prefId = keep + 1;
		modes[keep++] = modes[i];
	}
	if (!keep) {
		IOLog("FirebrandFramebuffer: connector %d: no mode the link carries fits the mapped surface\n", conn);
		IOFree(modes, 2 * kMaxModes * sizeof(*modes));
		return false;
	}
	if (!prefId)
		prefId = 1;
	/* the size on screen now at the nearest refresh (the preferred one if it is that size), else the preferred */
	if (fModeSet && fCurrentMode) {
		const struct frb_mode *cur = &fModes[fCurrentMode - 1];
		int hz = (int)frb_mode_hz(cur), best = 1 << 30;

		for (i = 0; i < (int)keep; i++) {
			int dist = (int)frb_mode_hz(&modes[i]) - hz;

			if (modes[i].hdisplay != cur->hdisplay || modes[i].vdisplay != cur->vdisplay)
				continue;
			if (dist < 0)
				dist = -dist;
			if (dist < best || (dist == best && i + 1 == prefId)) {
				best = dist;
				id = i + 1;
			}
		}
	}
	if (!id)
		id = prefId;
	bcopy(fModes, old, sizeof(fModes[0]) * kMaxModes);
	bcopy(modes, fModes, keep * sizeof(modes[0]));
	fModeCount = keep;
	fConn = conn;
	fHeadless = false;
	fConnected = true;
	fPreferred = prefId;
	fOutputOn = false;
	/*
	 * With a window server (its IOFramebufferUserClient open) the screen is
	 * its to redraw once told of the change; with none, nothing would draw:
	 * the boot picture, as at start.
	 */
	if (programModeLocked(id, fCurrentDepth, serverConnect ? -1 : bootPattern()) != kIOReturnSuccess) {
		bcopy(old, fModes, sizeof(fModes[0]) * kMaxModes);
		fModeCount = oldCount;
		fConn = oldConn;
		fHeadless = oldHeadless;
		fConnected = false;
		fPreferred = oldPreferred;
		fCurrentMode = oldCurrent;
		IOLog("FirebrandFramebuffer: the mode set for the monitor on connector %d failed; trying again\n", conn);
		IOFree(modes, 2 * kMaxModes * sizeof(*modes));
		return false;
	}
	IOLog("FirebrandFramebuffer: a monitor on connector %d: %lu modes, %ux%u set\n", conn,
	      (unsigned long)keep, fModes[id - 1].hdisplay, fModes[id - 1].vdisplay);
	forgetModeConfig("a monitor arrived");
	IOFree(modes, 2 * kMaxModes * sizeof(*modes));
	return true;
}

/* Unplugged: the output blanked cleanly; the surface and its modes stay for the next monitor. */
void FirebrandFramebuffer::monitorGoneLocked(void)
{
	struct frb_display *d = fCore.display();

	fConnected = false;
	if (d && fOutputOn)
		frb_display_off(fCore.adev(), d);
	fOutputOn = false;
	if (fModeSet && fCurrentMode) {
		const struct frb_mode *m = modeForID(fCurrentMode);

		if (m)
			fCore.setHeadlessScreen(fCore.surfaceOffset(), (m->hdisplay + 63) & ~63u, m->hdisplay,
						m->vdisplay);
	}
	IOLog("FirebrandFramebuffer: the monitor on connector %d was unplugged\n", fConn);
	forgetModeConfig("the monitor went");
}

/*
 * The mode list the window server keeps on this service: IOGraphicsLib
 * stores its whole mode table (IOFBModes, with each mode's timing and
 * flags) as the framebuffer's IOFBConfig property, and IOFBBuildModeList
 * takes that copy instead of asking the driver whenever it is there: on a
 * connection change and on every window server launch. Apple's
 * IONDRVFramebuffer drops it when the connection changes
 * (processConnectChange: removeProperty(kIOFBConfigKey)); without that, a
 * monitor that arrives after a headless start keeps the headless list in
 * the window server (1920x1080 up to 1920x1200, mode IDs that now name
 * other modes here), even across a window server restart
 * (2026-11-14: 1920x1080 drawn over a 3840x2160@30 scanout).
 */
void FirebrandFramebuffer::forgetModeConfig(const char *why)
{
	if (getProperty(kIOFBConfigKey)) {
		removeProperty(kIOFBConfigKey);
		IOLog("FirebrandFramebuffer: %s: the window server's cached mode list (IOFBConfig) dropped\n", why);
	}
}

/* IOGraphics may come back into the framebuffer from its callback: outside the lock */
void FirebrandFramebuffer::notifyConnectionChange(void)
{
	IOFBInterruptProc proc = 0;
	OSObject *target = 0;
	void *ref = 0;

	fCore.lock();
	if (fConnectOn && !fCaptured) {
		proc = fConnectProc;
		target = fConnectTarget;
		ref = fConnectRef;
		if (target)
			target->retain();
	}
	fCore.unlock();
	if (proc)
		(*proc)(target, ref);
	if (target)
		target->release();
}

IOReturn FirebrandFramebuffer::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
							OSObject *target, void *ref, void **interruptRef)
{
	if (interruptType == kIOFBConnectInterruptType) {
		if (!proc || !interruptRef)
			return kIOReturnBadArgument;
		fCore.lock();
		fConnectProc = proc;
		fConnectTarget = target;
		fConnectRef = ref;
		fConnectOn = true;
		*interruptRef = (void *)&fConnectProc;
		fCore.unlock();
		IOLog("FirebrandFramebuffer: the OS wants connection changes\n");
		return kIOReturnSuccess;
	}
	return super::registerForInterruptType(interruptType, proc, target, ref, interruptRef);
}

IOReturn FirebrandFramebuffer::unregisterInterrupt(void *interruptRef)
{
	if (interruptRef == (void *)&fConnectProc) {
		fCore.lock();
		fConnectProc = 0;
		fConnectTarget = 0;
		fConnectRef = 0;
		fConnectOn = false;
		fCore.unlock();
		return kIOReturnSuccess;
	}
	return super::unregisterInterrupt(interruptRef);
}

IOReturn FirebrandFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
	if (interruptRef == (void *)&fConnectProc) {
		fCore.lock();
		fConnectOn = state == kEnabledInterruptState;
		fCore.unlock();
		return kIOReturnSuccess;
	}
	return super::setInterruptState(interruptRef, state);
}
