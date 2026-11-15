/*
 * FirebrandCore: everything that owns the card, shared by the two classes
 * that can sit on it:
 *
 *   Firebrand             IOService in its own match category ("Firebrand"):
 *                         bring-up, diagnosis, the control client. Default.
 *   FirebrandFramebuffer  IOFramebuffer (match category "IOFramebuffer"),
 *                         only with FirebrandFramebuffer / frb_fb=1; then the
 *                         control personality steps aside and the
 *                         framebuffer offers the control client itself.
 *
 * The core is a plain member of either (no IOService of its own): the OS
 * layer for the hardware library, the VBIOS, BAR maps, the stages, the
 * display, the log and trace, and the control client's operations, all
 * under one lock.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_CORE_H
#define FIREBRAND_CORE_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>

extern "C" {
#include "frb_device.h"
#include "frb_display.h"
#include "frb_pm.h"
}
struct frb_vram_mgr;
#include "frb_user.h"
#include "qoriq_dma.h"

/*
 * RadeonNISubDeviceMemory, renamed: a sub-range of BAR0, for IOFramebuffer
 * (the visible surface) and the accelerator's clients (the aperture).
 * QorIQPCIBridge puts BAR0 above 4 GB; the VM mapping path uses
 * getPhysicalSegment64() cleanly, but IOFramebuffer::doSetup() computes
 * vramMapOffset from getPhysicalAddress() (getPhysicalSegment(0, 0)), which
 * panics in IOGeneralMemoryDescriptor above 4 GB. This answers it with an
 * offset-preserving stand-in instead.
 */
class FirebrandSubDeviceMemory : public IOSubMemoryDescriptor
{
	OSDeclareDefaultStructors(FirebrandSubDeviceMemory)

public:
	static IODeviceMemory *withDeviceSubRange(IODeviceMemory *parent, IOByteCount offset,
						  IOByteCount length);
	virtual IOPhysicalAddress getPhysicalSegment(IOByteCount offset, IOByteCount *length);

protected:
	virtual IOMemoryMap *makeMapping(IOMemoryDescriptor *owner, task_t intoTask,
					 IOVirtualAddress atAddress, IOOptionBits options,
					 IOByteCount offset, IOByteCount length);
};

class FirebrandCore
{
public:
	/* owner: the service whose personality holds the switches and the firmware */
	bool start(IOService *owner, IOPCIDevice *dev);
	void stop(void);
	void free(void);
	/* the personality and boot-arg switches */
	bool flag(const char *key, const char *bootarg);

	/* the OS layer's callbacks */
	UInt32 readReg(UInt32 offset);
	void writeReg(UInt32 offset, UInt32 value);
	void writeDoorbell64(UInt32 index, UInt64 value);	/* SOC15: BAR2, os->doorbell_write64 */
	IOPCIDevice *device(void) { return fDevice; }
	int firmware(const char *name, const uint8_t **data, uint32_t *size);
	void logLine(const char *line);

	void lock(void) { if (fLock) IOLockLock(fLock); }
	void unlock(void) { if (fLock) IOLockUnlock(fLock); }
	struct frb_device *adev(void) { return &fAdev; }

	/*
	 * System memory as the card sees it (qoriq_dma.h). Switch
	 * QorIQDMACommand / qdmacmd= off: CPU physical + inbound window 2, as
	 * always. On: an IODMACommand walk -- the DART's address on a G5, the
	 * kernel's translation on a T4240 that has one, else physical + the
	 * bridge's PCIInboundDMAOffset. dmaWalk() makes the walker for a
	 * prepared descriptor (inert when the switch is off) and busOfPage()
	 * answers one page through it; 0 means no bus address.
	 */
	bool dmaCommand(void) const { return fDMACmd; }
	QorIQDMAWalk *dmaWalk(IOMemoryDescriptor *md);
	UInt64 busOfPage(QorIQDMAWalk *walk, IOMemoryDescriptor *md, UInt64 offset);
	/* the first few bindings say what their first page became, either way */
	void logBinding(const char *what, IOMemoryDescriptor *md, UInt64 bus, UInt32 pages);
	/* the display once stage dce is done, else 0 */
	struct frb_display *display(void);

	/* the control client's work (each takes the lock) */
	IOReturn ucInfo(struct frb_user_info *info);
	IOReturn ucRegRead(UInt32 reg, UInt32 *value);
	IOReturn ucRegWrite(UInt32 reg, UInt32 value);
	IOReturn ucSmcRead(UInt32 addr, UInt32 *value);
	IOReturn ucRunStage(UInt32 stage, SInt32 *result);
	IOReturn ucVramRead(UInt32 offset, UInt32 *value);
	IOReturn ucTrace(UInt32 on, UInt32 *entries);
	IOReturn ucDisplayInfo(struct frb_user_display *out);
	IOReturn ucEdid(UInt32 conn, void *out, IOByteCount *size);
	IOReturn ucDisplayMode(UInt32 conn, UInt32 width, UInt32 height, UInt32 hz, UInt32 max_rate, SInt32 *result);
	IOReturn ucDisplayOff(SInt32 *result);
	IOReturn ucDisplayFill(UInt32 pattern, UInt32 arg0, UInt32 arg1, SInt32 *result);
	IOReturn ucDetect(SInt32 *found);
	IOReturn ucDpcdRead(UInt32 conn, UInt32 addr, UInt32 *value);
	IOReturn ucSinkCrc(UInt32 conn, SInt32 *result, UInt32 *r, UInt32 *g, UInt32 *b);
	IOReturn ucEngineTest(UInt32 engine, UInt32 test, SInt32 *result);
	IOReturn ucRecover(UInt32 force, SInt32 *result, UInt32 *hung, UInt32 *after, UInt32 *soft, UInt32 *ms);
	/* frb_device_gpu_recover, with the lock held (frbctl recover, the accelerator's watchdog) */
	int recoverLocked(UInt32 force, struct frb_recover_report *rep);
	IOReturn ucFbDetach(SInt32 *result);
	/* frbctl cursor: op FRB_CURSOR_OP_* at x, y */
	IOReturn ucCursor(UInt32 op, SInt32 x, SInt32 y, SInt32 *result);
	/* frbctl dpm: FRB_DPM_OP_*, then the readout */
	void readPcieCaps(void);
	IOReturn ucDpm(UInt32 op, struct frb_user_dpm *out);
	IOReturn ucLut(struct frb_user_lut *out);
	IOReturn ucLutSet(UInt32 op, SInt32 *result);
	IOReturn ucAudio(UInt32 op, SInt32 arg, SInt32 *result, SInt32 *dig, SInt32 *pin, UInt32 *flags);
	/* frbctl cfg: configuration space of function fn of this device (writes need FirebrandAllowRegWrite) */
	IOReturn ucConfig(UInt32 fn, UInt32 offset, UInt32 write, UInt32 value, UInt32 *out);
	/* FRB_QUIRK_AUDIO_FN_STRAP at start, after the stages: the audio pin strap, then the bridge's scan */
	void audioFunction(void);
	IOReturn ucSmn(UInt32 addr, UInt32 write, UInt32 value, UInt32 *out);
	IOReturn ucFunctions(UInt32 op, UInt32 arg, SInt32 *result, UInt32 *hdr, UInt32 *id1, UInt32 *nubs);
	/*
	 * FRB_UC_STATUS (frb_user.h): a monitor's snapshot. The sensors come
	 * from a sample at most kSensorsMinMs old, taken under the lock; after
	 * a failed read none for kSensorsHoldMs.
	 */
	IOReturn ucStatus(struct frb_user_status *out);
	enum { kSensorsMinMs = 500, kSensorsHoldMs = 30000 };
	/* the accelerator's VRAM heaps, for the status (lock held; 0 when it retires) */
	void setVramMgr(const struct frb_vram_mgr *m) { fVramMgr = m; }
	/* the video sessions open now and whether a timeout poisoned the decoder (FirebrandVideo) */
	void setVideoState(UInt32 sessions, bool poisoned) { fVideoSessions = sessions; fVideoPoisoned = poisoned; }
	IOReturn scanFunctions(UInt32 flags, UInt32 *newNubs);
	bool detaching(void) { return fDetaching; }
	IOMemoryDescriptor *ucMemory(UInt32 type);

	/* the display, for the framebuffer (call with the lock held) */
	int firstConnected(void);
	bool pickMode(struct frb_connector *conn, UInt32 width, UInt32 height, UInt32 hz, struct frb_mode *mode);
	/* where the scanout surface lives: the same offset for every mode */
	UInt32 surfaceOffset(void);
	/* light conn with mode; pattern: FRB_PATTERN_* written first, or -1 to leave VRAM alone */
	/* bpp 8, 16 or 32 (the test patterns only at 32) */
	int programMode(UInt32 conn, const struct frb_mode *mode, int pattern, UInt32 max_rate, UInt32 bpp = 32);
	void fillPattern(UInt32 pattern, UInt32 arg0 = 0, UInt32 arg1 = 0);
	/* BAR0, for the framebuffer's aperture */
	IODeviceMemory *aperture(void);
	UInt32 maxLinkRate(void);
	/* GTT objects for the hardware library (frb_os gtt_alloc / gtt_free) */
	int gttAlloc(uint32_t size, struct frb_gtt *gtt);
	void gttFree(struct frb_gtt *gtt);
	/* the card may reach host memory from now on (the first GTT object or GART binding) */
	void busMaster(void);

	/*
	 * The accelerator (FirebrandAccel / boot-arg frb_accel=1), decided at
	 * start: it moves VMID 0's GART below 4 GB before stage gmc
	 * (adev.gart_legacy), and the owner creates the accelerator once the
	 * stages reached sdma.
	 */
	bool accelWanted(void) { return fAccelWanted; }
	/* FirebrandAllowRegWrite / frb_regwrite=1 */
	bool allowRegWrite(void) { return fAllowRegWrite; }
	/*
	 * VRAM from the scanout surface's start up to `end` is the display's;
	 * above it the accelerator's heap begins, so programMode refuses a
	 * surface that would reach past it. 0: no limit (no accelerator).
	 */
	void setScanoutLimit(UInt32 end) { fScanoutLimit = end; }
	/*
	 * The hardware cursor's surface: FRB_CURSOR_MAX x FRB_CURSOR_MAX ARGB
	 * words, 4 KB aligned, after a scanout surface of surfaceBytes.
	 */
	UInt32 cursorOffset(UInt32 surfaceBytes) { return (surfaceOffset() + surfaceBytes + 4095) & ~4095u; }
	enum { kCursorBytes = FRB_CURSOR_MAX * FRB_CURSOR_MAX * 4 };
	/* a w x h premultiplied ARGB image (host order) into the surface at offset, shown there (lock held) */
	int loadCursorLocked(UInt32 offset, const UInt32 *argb, UInt32 w, UInt32 h);
	/* the framebuffer's cursor is the hardware's; else IOFramebuffer and the window server draw it with the CPU */
	void setHWCursorActive(bool on) { fHWCursorActive = on; }
	bool hwCursorActive(void) { return fHWCursorActive; }
	/*
	 * The screen the 2D path and GET_INFO describe (lock held): the CRTC's
	 * scanout surface, or a headless framebuffer's (no monitor: a surface
	 * in VRAM that nothing scans out). false: none. bpp: 8, 16 or 32.
	 */
	bool screenLocked(UInt32 *offset, UInt32 *pitch, UInt32 *width, UInt32 *height, UInt32 *bpp = 0);
	void setHeadlessScreen(UInt32 offset, UInt32 pitch, UInt32 width, UInt32 height, UInt32 bpp = 32)
	{
		fHeadlessOffset = offset;
		fHeadlessPitch = pitch;
		fHeadlessWidth = width;
		fHeadlessHeight = height;
		fHeadlessBpp = bpp;
	}

private:
	bool biosFromRom(void);
	bool biosFromProvider(void);
	bool biosFromPersonality(void);
	bool loadBios(void);
	bool mapAperture(void);
	bool mapDoorbells(void);
	bool allocDummyPage(void);
	UInt32 configuredStage(void);
	void publishState(void);
	void publishMemory(void);
	/* the PCIe link the card reaches the host through (FRB_ST_SLOT_LINK), found at start */
	void findSlotPort(void);
	void sampleSensorsLocked(void);
	void statusDisplayLocked(struct frb_user_status *out);
	int runStages(UInt32 stage);
	int setMode(UInt32 conn, UInt32 width, UInt32 height, UInt32 hz, UInt32 max_rate);

	IOService *fOwner;
	IOPCIDevice *fDevice;
	bool fDMACmd;
	UInt64 fDMAAdd, fDMALimit;
	IOMemoryMap *fRegMap;
	volatile UInt8 *fRegs;
	IOMemoryMap *fFbMap;
	IOMemoryMap *fDoorbellMap;	/* SOC15: BAR2, the doorbells (NULL: the rings write their WPTRs) */
	volatile UInt8 *fDoorbells;
	UInt32 fDoorbellSize;
	IOBufferMemoryDescriptor *fDummy;
	IOBufferMemoryDescriptor *fTraceMem;
	IOBufferMemoryDescriptor *fLogMem;
	UInt32 fLogLen;
	OSDictionary *fFirmware;
	IOLock *fLock;
	void *fBios;
	UInt32 fBiosSize;
	bool fAllowRegWrite;
	bool fBringUp;
	bool fAdopt;
	bool fDisplayAuto;
	bool fBusMaster;		/* set at the first GTT object, cleared at stop */
	uint16_t *fLutSaved;		/* frbctl lut linear: the ramp it replaced (3 x 256), until restore */
	bool fDetaching;		/* frbctl fb detach: the framebuffer refuses new clients */
	bool fAccelWanted;
	bool fDpmWanted;
	bool fAudioWanted;
	bool fPspOnly;		/* FirebrandPSPOnly / frb_psp_only=1 (SOC15) */
	bool fMMIOWptr;		/* FirebrandMMIOWptr / frb_mmio_wptr=1 (SOC15: no doorbells) */
	bool fUVDOneInstance;	/* FirebrandUVDOneInstance / frb_uvd_one=1 (Vega 20: UVD(1) not started) */
	bool fGating;		/* FirebrandGating / frb_cg (SOC15 clock gating, default on) */
	UInt32 fGatingExclude;	/* FirebrandGatingExclude / frb_cg_exclude: AMD_CG_SUPPORT_* bits left ungated */
	UInt32 fScanoutLimit;
	bool fHWCursorActive;
	UInt32 fHeadlessOffset, fHeadlessPitch, fHeadlessWidth, fHeadlessHeight;	/* width 0: none */
	UInt32 fHeadlessBpp;
	struct frb_os fOS;
	struct frb_device fAdev;
	/* the status call (ucStatus) */
	UInt8 fPcieCap;			/* the GPU's PCI Express capability, 0 none */
	IOPCIDevice *fSlotPort;		/* an in-package switch's upstream port, retained; 0: the GPU's own link */
	UInt8 fSlotCap;
	struct frb_pm_sensors fSensors;	/* the last sample */
	int fSensorsResult;
	UInt64 fSensorsAt, fSensorsHoldUntil;	/* ms (os_time_ms); 0: never sampled */
	const struct frb_vram_mgr *fVramMgr;
	UInt32 fVideoSessions;
	bool fVideoPoisoned;
	bool fStopped;			/* stop() has begun */
};

#endif
