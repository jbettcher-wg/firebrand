/*
 * FirebrandFramebuffer: the card as an unaccelerated IOFramebuffer, after
 * RadeonNI's (RadeonNI's kext/RadeonNI/RadeonNI.cpp), on FirebrandCore.
 *
 * It matches the PCI device in the IOFramebuffer category and only when
 * FirebrandFramebuffer / frb_fb=1 asks for it; Firebrand (the control
 * personality) then stands aside, and this class offers the control client
 * too. The bring-up runs to stage dce; the monitor is the first connector
 * with a DisplayPort sink. One CRTC, one connection, 32 bits per pixel
 * (big-endian XRGB in memory; the scanout swaps 8-in-32), or 16 (xRGB1555,
 * swapped 8-in-16) or 8 (indexed through the LUT), as RadeonNI offers. The modes are the
 * EDID's (frb_edid_modes, in the order Quartz wants), Safe when the DP link
 * carries them. No monitor: headless (a surface nothing scans out). Hot-plug
 * by polling the HPD sense lines, display sleep by turning the output off
 * (RadeonNI's); no interrupts yet. The cursor: the CRTC's cursor plane with
 * FirebrandHWCursor, else IOGraphics' software one.
 * With FirebrandAccel / frb_accel=1 (and the stages to sdma) it creates the
 * accelerator (FirebrandAccel.h) and points the window server at it.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_FRAMEBUFFER_H
#define FIREBRAND_FRAMEBUFFER_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>

#include "FirebrandCore.h"
#include "FirebrandCompat.h"

class FirebrandAccel;

/* FRB_FB_BASE: RadeonNI, the compatibility name (FirebrandCompat.h), or IOFramebuffer */
class FirebrandFramebuffer : public FRB_FB_BASE
{
	OSDeclareDefaultStructors(FirebrandFramebuffer)

	enum { kMaxModes = 32 };

public:
	virtual IOService *probe(IOService *provider, SInt32 *score);
	virtual bool start(IOService *provider);
	virtual void stop(IOService *provider);
	virtual bool terminate(IOOptionBits options = 0);
	virtual void free(void);
	virtual bool serializeProperties(OSSerialize *s) const;
	virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
				       IOUserClient **handler);
	virtual unsigned long maxCapabilityForDomainState(IOPMPowerFlags state);
	virtual unsigned long initialPowerStateForDomainState(IOPMPowerFlags state);
	virtual unsigned long powerStateForDomainState(IOPMPowerFlags state);

	/* IOFramebuffer */
	virtual IOReturn enableController(void);
	virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture);
	virtual IODeviceMemory *getVRAMRange(void);
	virtual IOReturn setupForCurrentConfig(void);
	virtual const char *getPixelFormats(void);
	virtual IOItemCount getDisplayModeCount(void);
	virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes);
	virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
		IODisplayModeInformation *info);
	virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth);
	virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
		IOPixelAperture aperture, IOPixelInformation *pixelInfo);
	virtual IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth);
	virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth);
	virtual IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth);
	virtual IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index,
		UInt32 numEntries, IOOptionBits options);
	virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
		UInt32 dataWidth, void *data);
	virtual IOReturn getTimingInfoForDisplayMode(IODisplayModeID displayMode,
		IOTimingInformation *info);
	virtual IOItemCount getConnectionCount(void);
	virtual IOReturn setAttributeForConnection(IOIndex connectIndex,
		IOSelect attribute, UInt32 value);
	virtual IOReturn getAttributeForConnection(IOIndex connectIndex,
		IOSelect attribute, UInt32 *value);
	virtual IOReturn setAttribute(IOSelect attribute, UInt32 value);
	virtual IOReturn getAttribute(IOSelect attribute, UInt32 *value);
	virtual IOReturn setCursorImage(void *cursorImage);
	virtual IOReturn setCursorState(SInt32 x, SInt32 y, bool visible);
	/* hot-plug: the connection-change callback IOFramebuffer registers */
	virtual IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
						  OSObject *target, void *ref, void **interruptRef);
	virtual IOReturn unregisterInterrupt(void *interruptRef);
	virtual IOReturn setInterruptState(void *interruptRef, UInt32 state);
	virtual IOReturn connectFlags(IOIndex connectIndex, IODisplayModeID displayMode,
		IOOptionBits *flags);
	virtual bool hasDDCConnect(IOIndex connectIndex);
	virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
		IOSelect blockType, IOOptionBits options, UInt8 *data, IOByteCount *length);

	FirebrandCore *core(void) { return &fCore; }

private:
	const struct frb_mode *modeForID(IODisplayModeID id);
	bool modeSettable(const struct frb_mode *mode);
	bool buildModes(void);
	bool buildHeadlessModes(void);
	IOReturn programMode(IODisplayModeID id, IOIndex depth, int pattern);
	IOReturn programModeLocked(IODisplayModeID id, IOIndex depth, int pattern);
	void loadColorsLocked(void);
	bool linkCarries(int conn, const struct frb_mode *mode);
	/* hot-plug (RadeonNI's pollHotplug / monitorReturned) and display power */
	void startHotplug(void);
	void stopHotplug(void);
	static void pollTimerFired(OSObject *owner, IOTimerEventSource *sender);
	void pollHotplug(void);
	int senseLocked(void);
	bool monitorArrivedLocked(void);
	void monitorGoneLocked(void);
	void forgetModeConfig(const char *why);
	int bootPattern(void);
	IOReturn showBootPicture(void);
	void splashColorsLocked(const struct frb_mode *mode);
	void notifyConnectionChange(void);
	IOReturn setOutputPower(bool on, bool framebuffer);

	FirebrandCore fCore;
	FirebrandAccel *fAccel;
	bool fCoreStarted;
	int fConn;			/* the connector the monitor is on (-1: headless) */
	bool fHeadless;			/* no monitor: nothing scans the surface out */
	struct frb_mode fModes[kMaxModes];
	UInt32 fModeCount;
	IODisplayModeID fPreferred;
	UInt32 fSurfaceBytes;		/* the aperture: the biggest settable mode */
	IODisplayModeID fCurrentMode;
	IOIndex fCurrentDepth;		/* kDepth8, 16 or 32 (FirebrandFramebuffer.cpp) */
	bool fModeSet;
	/* the LUT: the gamma ramp (16 and 32 bpp) and the colour table (8 bpp, through the gamma) */
	UInt16 fGamma[3][256];
	UInt16 fClut[3][256];
	bool fPowerRegistered;
	UInt32 fPowerState;
	/* the hardware cursor (FirebrandHWCursor, on unless frb_hwcursor=0) */
	bool fHWCursor;
	UInt32 fCursorOffset;		/* its surface, after the biggest scanout surface */
	UInt32 *fCursorData;		/* convertCursorImage's output, FRB_CURSOR_MAX squared */
	bool fCursorLogged;
	/* hot-plug (FirebrandHotplug, on unless frb_hotplug=0) */
	bool fHotplug;
	IOWorkLoop *fPollLoop;
	IOTimerEventSource *fPollTimer;
	int fSenseLast;			/* what the hot-plug lines said last time */
	UInt32 fSenseSteady;		/* polls it has said so */
	UInt32 fUnplugGrace;		/* polls left in which a lost HPD line is a sink re-syncing (after a mode set) */
	bool fGraceNoted;		/* logged once per grace period */
	UInt32 fArriveHold;		/* polls to wait before asking a line that is up with no sink again */
	bool fNoSinkNoted;		/* "a line is up but no sink answers" logged (once until it changes) */
	bool fConnected;		/* a monitor is on fConn */
	IOFBInterruptProc fConnectProc;
	OSObject *fConnectTarget;
	void *fConnectRef;
	bool fConnectOn;
	bool fCaptured;
	/* display power: the framebuffer's power state and the display's (DPMS) */
	bool fDpms;			/* FirebrandDPMS, on unless frb_dpms=0 */
	bool fIdleDoze;			/* FirebrandIdleDoze, off unless frb_idledoze=1 */
	bool fFbPowerOn, fDisplayPowerOn;
	bool fOutputOn;			/* the CRTC and the link are running */
};

#endif
