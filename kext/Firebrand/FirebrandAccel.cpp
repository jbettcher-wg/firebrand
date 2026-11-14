/*
 * FirebrandAccel: see FirebrandAccel.h. The service follows RadeonNI's
 * (RadeonNI's kext/RadeonNI/RadeonNIAccel.cpp: withFramebuffer,
 * retire, the AGP shim, the user client's table and its UVD/diagnostic
 * answers); the engine, memory and fences are Firebrand's (amdgpu/).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#include "FirebrandAccel.h"
#include "FirebrandSurface.h"
extern "C" {
#include "frb_uvd.h"
}

extern "C" {
#include "include/asic_reg/gca/gfx_8_0_d.h"
#include "include/asic_reg/gca/gfx_8_0_sh_mask.h"
#include "include/asic_reg/gmc/gmc_8_1_sh_mask.h"
}

/* RadeonNIAccel.cpp gartBusOf: inbound ATMU window 2, PCI 0x10_0000_0000 -> CPU 0, snooped */
#define FRB_PCI_INBOUND_BASE	0x1000000000ULL
#define FRB_PCI_INBOUND_SIZE	(1ULL << 36)

#define FRB_GL_BUNDLE_NAME	"RadeonNIGLDriver"	/* one bundle; FrostByte checks its renderer ID 0x00021a00 */
#define FRB_GA_PLUGIN_TYPE	"ACCF0000-0000-0000-0000-000a2789904e"
#define FRB_GA_PLUGIN_NAME	"RadeonNIGA.plugin"
#define FRB_ACCEL_IFACE_VERSION	1
#define FRB_MAX_IB_WORDS	0xFFFFFu	/* INDIRECT_BUFFER's IB_SIZE: 20 bits */
#define FRB_RETIRE_WAIT_MS	2000
#define FRB_RING_WAIT_MS	2000
/*
 * The lockup watchdog: a ring whose fence stands still with work behind it
 * for amdgpu's default lockup_timeout (2000 ms, every queue) is hung. It
 * looks every FRB_WATCH_MS.
 */
#define FRB_WATCH_MS		500
#define FRB_LOCKUP_MS		2000

/* --- RadeonNI's class names (FirebrandCompat.h) ------------------------------ */

#if FRB_RADEONNI_COMPAT
OSDefineMetaClassAndAbstractStructors(RadeonNI, IOFramebuffer)
OSDefineMetaClassAndAbstractStructors(RadeonNIAccel, IOAccelerator)
#endif

/* --- the AGP shim (RadeonNIAGPShim) ------------------------------------------ */

OSDefineMetaClassAndStructors(FirebrandAGPShim, IOAGPDevice)

/* No driver is to be matched against the shim, and it has no bridge to ask. */
bool FirebrandAGPShim::matchPropertyTable(OSDictionary *table, SInt32 *score)
{
	return false;
}

bool FirebrandAGPShim::matchPropertyTable(OSDictionary *table)
{
	return false;
}

bool FirebrandAGPShim::compareName(OSString *name, OSString **matched) const
{
	return false;
}

IOReturn FirebrandAGPShim::getResources(void)
{
	return kIOReturnSuccess;
}

/* --- the service -------------------------------------------------------------- */

#define super FRB_ACCEL_BASE
OSDefineMetaClassAndStructors(FirebrandAccel, FRB_ACCEL_BASE)

FirebrandAccel *FirebrandAccel::withOwner(IOService *owner, FirebrandCore *core, IOFramebuffer *fb,
					  IOPCIDevice *provider, UInt32 scanoutEnd)
{
	FirebrandAccel *accel = new FirebrandAccel;

	if (!accel)
		return 0;
	if (!accel->init() || !accel->setUp(owner, core, fb, provider, scanoutEnd)) {
		accel->release();
		return 0;
	}
	return accel;
}

bool FirebrandAccel::setUp(IOService *owner, FirebrandCore *core, IOFramebuffer *fb, IOPCIDevice *provider,
			   UInt32 scanoutEnd)
{
	struct frb_device *adev = core->adev();
	struct frb_ring *ring;
	OSNumber *shim;
	IOService *parent = provider;
	UInt64 window = 0, vram = 0;
	UInt32 which, reserved = 0;
	const char *why = 0;

	fOwner = owner;
	fOwner->retain();
	fCore = core;
	fFramebuffer = fb;
	fProvider = provider;
	fScanoutEnd = scanoutEnd;

	core->lock();
	ring = frb_gfx_v8_0_ring(adev);
	if (adev->stage_done < FRB_STAGE_SDMA)
		why = "the stages did not reach sdma";
	else if (!ring)
		why = "the GFX ring is not up";
	else if (!adev->aper || !adev->gart.ready)
		why = "no VRAM window or no GART";
	if (!why) {
		window = adev->aper_size < adev->gmc.real_vram_size ? adev->aper_size : adev->gmc.real_vram_size;
		vram = adev->gmc.real_vram_size;
		reserved = adev->layout.reserved_start;
		if (scanoutEnd < adev->gmc.stolen_vga_size || scanoutEnd + (16u << 20) > reserved)
			why = "no room for a heap between the scanout and the driver's region";
		else if (frb_vram_mgr_init(&fVram, adev->os, scanoutEnd, reserved, window, vram))
			why = "no memory for the heaps";
	}
	if (!why) {
		struct frb_ring *sdma = frb_sdma_v3_0_ring(adev, 0);

		fCompleted = frb_fence_read(ring);
		fSdmaCompleted = sdma ? frb_fence_read(sdma) : 0;
		/*
		 * The 2D path's IBs: GTT, snooped, written by the CPU through its
		 * cache (frb_rect.c). Measured on the RX 550: IBs in VRAM made no
		 * difference to SDMA0's pace (~11 us a submission, ~2 us a packet).
		 */
		if (sdma && !core->gttAlloc(kIbSlots * kIbSlotBytes, &fIbArena))
			fIbArenaUp = true;
	}
	core->unlock();
	if (why) {
		IOLog("FirebrandAccel: no accelerator: %s (stage %s)\n", why, frb_stage_name(adev->stage_done));
		return false;
	}

	/*
	 * Video ('rdnv', FirebrandVideo.cpp): offered unless the owner's
	 * FirebrandVideo is false or boot-arg frb_video=0; a session also needs
	 * the decoder up (stage uvd), which QUERY looks at each time.
	 */
	{
		int arg = 1;

		fVideoLock = IOLockAlloc();
		fVideoOffered = fVideoLock && owner->getProperty("FirebrandVideo") != kOSBooleanFalse &&
			!(PE_parse_boot_arg("frb_video", &arg) && !arg);
	}

	/* the status page: rdn_user_status in the CPU's order, read-only to the clients */
	fStatusMem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, 4096, 4096);
	fAperture = FirebrandSubDeviceMemory::withDeviceSubRange(core->aperture(), 0, reserved);
	fBindings = (Binding *)IOMalloc(kMaxBindings * sizeof(Binding));
	if (!fStatusMem || !fAperture || !fBindings) {
		IOLog("FirebrandAccel: no memory for the status page, aperture or binding table\n");
		return false;
	}
	fApertureBytes = reserved;
	bzero(fBindings, kMaxBindings * sizeof(Binding));
	bzero(fStatusMem->getBytesNoCopy(), 4096);
	fStatus = (volatile struct rdn_user_status *)fStatusMem->getBytesNoCopy();
	fStatus->version = RDN_STATUS_VERSION;
	core->lock();
	updateStatusLocked();
	core->unlock();

	/*
	 * The window server's surfaces (RadeonNI's personality keys, read from
	 * the owner): only for a framebuffer, and only with the 2D path, which
	 * flushes them to the screen.
	 */
	{
		int arg = 1;
		OSObject *key = owner->getProperty("ScreenSurface");

		fSurfaces = fb && fIbArenaUp && owner->getProperty("Surfaces") == kOSBooleanTrue;
		fSurfaceGate = owner->getProperty("SurfaceGate") == kOSBooleanTrue;
		fBackingSurfaces = owner->getProperty("BackingSurfaces") == kOSBooleanTrue;
		fScreenSurface = (!key || key == kOSBooleanTrue) &&
			!(PE_parse_boot_arg("frb_screensurface", &arg) && !arg);
	}

	/* Bit 0: the ancestor. Bit 1: the registered shim (RadeonNIAccel::withFramebuffer). */
	shim = OSDynamicCast(OSNumber, owner->getProperty("AGPShim"));
	which = shim ? shim->unsigned32BitValue() : 0;
	if (which & 1) {
		OSDictionary *none = OSDictionary::withCapacity(1);

		fAncestor = new IOAGPDevice;
		/* into the registry only: IOPCIDevice::attach() expects a PCI bridge for a provider */
		if (fAncestor && none && fAncestor->init(none) && fAncestor->attachToParent(provider, gIOServicePlane)) {
			fAncestor->setName("FirebrandAGPAncestor");
			parent = fAncestor;
		} else if (fAncestor) {
			fAncestor->release();
			fAncestor = 0;
		}
		if (none)
			none->release();
	}
	if (which & 2) {
		static const char model[] = "Firebrand";
		static const UInt32 noClass = 0;
		OSDictionary *none = OSDictionary::withCapacity(1);

		fShim = new FirebrandAGPShim;
		if (fShim && none && fShim->init(none) && fShim->attachToParent(provider, gIOServicePlane)) {
			static const char *const ids[] = { "vendor-id", "device-id" };
			unsigned i;

			fShim->setName("FirebrandAGPShim");
			fShim->setProperty("model", (void *)model, sizeof(model));
			/* not a display class, so nothing takes it for a second card; the card's ids */
			fShim->setProperty("class-code", (void *)&noClass, sizeof(noClass));
			for (i = 0; i < 2; i++) {
				OSData *id = OSDynamicCast(OSData, provider->getProperty(ids[i]));

				if (id)
					fShim->setProperty(ids[i], id);
			}
			fShim->registerService();
		} else if (fShim) {
			fShim->release();
			fShim = 0;
		}
		if (none)
			none->release();
	}
	if (!attach(parent)) {
		IOLog("FirebrandAccel: cannot attach\n");
		return false;
	}
	publish(provider);
	startWatchdog();
	frb_info(adev, "accel: 2D on sdma0 %s (IBs %u x %u KB in GTT at 0x%llx); surface clients %s",
		 fIbArenaUp ? "up" : "NOT up", (unsigned)kIbSlots, (unsigned)(kIbSlotBytes >> 10),
		 (unsigned long long)fIbArena.gpu_addr, fSurfaces ? "offered" : "not offered");
	frb_info(adev, "accel: up: VRAM heap %lu MB visible at 0x%08lx, %lu MB beyond the window at 0x%08lx; "
		 "GART %lu MB at 0x%llx%s; GFX fence %lu; %s",
		 (unsigned long)((reserved - scanoutEnd) >> 20), (unsigned long)scanoutEnd,
		 (unsigned long)((vram - window) >> 20), (unsigned long)window,
		 (unsigned long)(adev->gmc.gart_size >> 20), (unsigned long long)adev->gmc.gart_start,
		 adev->gmc.gart_start + adev->gmc.gart_size <= 0x100000000ULL ? "" : " (above 4 GB: not offered)",
		 (unsigned long)fCompleted, fb ? "for the window server" : "headless");
	return true;
}

/*
 * The registry keys RadeonNI publishes (docs/accel-interface.md 1.7, 2.1).
 * With a framebuffer and the surface clients (G2): AccelCaps (the
 * personality's, RadeonNI's 3), ScreenSurface and BackingSurfaces, as
 * RadeonNIAccel::withFramebuffer sets them. OpenGL's announcement (the
 * framebuffer's IOAccelTypes) stays behind FirebrandAccelGL: without it
 * the window server finds no GL renderer for the display, whatever
 * AccelCaps says, and composites on the CPU with the 2D plug-in's blits.
 */
void FirebrandAccel::publish(IOPCIDevice *provider)
{
	OSArray *types = OSArray::withCapacity(2);
	char path[512];
	int len = sizeof(path), arg = 0;
	/*
	 * Announced to OpenGL (the framebuffer's IOAccelTypes) only when asked:
	 * FirebrandAccelGL / boot-arg frb_gl=1. With it, CGL lists the installed
	 * RadeonNIGLDriver as accelerated renderer 0x00021a00 (so FrostByte
	 * takes caps.radeon), and that bundle, SI/CIK only, kills the program
	 * that asks it for a context on a Polaris (SIGBUS after "rdn: no surface
	 * layout for device 699f", 2026-11-13). RadeonNI's own gate
	 * withholds IOAccelTypes the same way where GL is not validated.
	 */
	bool gl = fOwner->getProperty("FirebrandAccelGL") == kOSBooleanTrue ||
		(PE_parse_boot_arg("frb_gl", &arg) && arg);

	setName("FirebrandAccel");
	setProperty("IOMatchCategory", "IOAccelerator");
	setProperty("IOGLBundleName", FRB_GL_BUNDLE_NAME);
	setProperty(kIOAccelRevisionKey, (UInt64)kCurrentGraphicsInterfaceRevision, 32);
	setProperty("RadeonNIEngine", true);
	setProperty("FirebrandEngine", true);
	setProperty("FirebrandAccelInterface", (UInt64)FRB_ACCEL_IFACE_VERSION, 32);
	setProperty("FirebrandAccelHeadless", fFramebuffer == 0);
	setProperty("FirebrandAccelGL", fFramebuffer && gl);
	setProperty("FirebrandAccel2D", fIbArenaUp);
	if (fSurfaces) {
		OSNumber *caps = OSDynamicCast(OSNumber, fOwner->getProperty("AccelCaps"));

		if (caps)
			setProperty("AccelCaps", caps);
		setProperty("ScreenSurface", fScreenSurface);
		setProperty("BackingSurfaces", fBackingSurfaces);
	}
	if (types) {
		OSString *t1 = OSString::withCString("rdn1"), *t2 = OSString::withCString("rdn2");
		OSString *tv = fVideoOffered ? OSString::withCString("rdnv") : 0;

		if (t1)
			types->setObject(t1);
		if (t2)
			types->setObject(t2);
		if (tv)
			types->setObject(tv);
		setProperty("FirebrandClientTypes", types);
		if (t1)
			t1->release();
		if (t2)
			t2->release();
		if (tv)
			tv->release();
		types->release();
	}
	setProperty("FirebrandVideo", fVideoOffered);

	/*
	 * IOAccelFindAccelerator resolves the path below through the bridges:
	 * each QorIQ PCIe bridge ancestor needs its controller location
	 * (RadeonNIAccel.cpp: an older QorIQPCIBridge in the boot mkext lacks it).
	 */
	for (IORegistryEntry *anc = getParentEntry(gIOServicePlane); anc; anc = anc->getParentEntry(gIOServicePlane)) {
		OSNumber *ctrl = OSDynamicCast(OSNumber, anc->getProperty("QorIQPCIeController"));

		if (ctrl && !anc->getLocation(gIOServicePlane)) {
			char loc[12];

			snprintf(loc, sizeof(loc), "%x", (unsigned)ctrl->unsigned32BitValue());
			anc->setLocation(loc);
		}
	}

	if (fFramebuffer) {
		/* the 2D plug-in the window server loads for the framebuffer (IOPSAllocateBlitEngine) */
		if (fOwner->getProperty("GAPlugin") != kOSBooleanFalse) {
			OSDictionary *plugins = OSDictionary::withCapacity(1);
			OSString *plugin = OSString::withCString(FRB_GA_PLUGIN_NAME);

			if (plugins && plugin) {
				plugins->setObject(FRB_GA_PLUGIN_TYPE, plugin);
				fFramebuffer->setProperty("IOCFPlugInTypes", plugins);
			}
			if (plugin)
				plugin->release();
			if (plugins)
				plugins->release();
		}
		/*
		 * RadeonNI's UVD properties, which FrostByte's hardware probe reads
		 * from the "RadeonNI" service (fb-hardware.m detectVideoDecoder):
		 * FW_UVD non-empty and EnableUVDSessions true when decode sessions
		 * can be had. RadeonNI keeps the decoder firmware itself in FW_UVD;
		 * Firebrand embeds it, so FW_UVD names it instead (a few bytes, not
		 * 375 KB in every ioreg). Only when 'rdnv' is offered and stage uvd
		 * brought the ring up; videoPoison sets EnableUVDSessions false.
		 */
		if (fVideoOffered && frb_uvd_ring(fCore->adev())) {
			struct frb_uvd *uvd = fCore->adev()->uvd;
			char fw[64];
			OSData *d;

			snprintf(fw, sizeof(fw), "polaris12_uvd.bin %u.%u family %u (embedded)",
				 (unsigned)(uvd->fw_version >> 24) & 0xff, (unsigned)(uvd->fw_version >> 16) & 0xff,
				 (unsigned)(uvd->fw_version >> 8) & 0xff);
			d = OSData::withBytes(fw, strlen(fw));
			if (d) {
				fFramebuffer->setProperty("FW_UVD", d);
				d->release();
			}
			fFramebuffer->setProperty("EnableUVDSessions", true);
		}
		/* the framebuffer points at its accelerator by registry path (CGL, the window server) */
		if (!gl) {
			frb_info(fCore->adev(), "accel: engine and 2D plug-in only, not announced to OpenGL "
				 "(FirebrandAccelGL / frb_gl=1)");
		} else if (getPath(path, &len, gIOServicePlane)) {
			fFramebuffer->setProperty(kIOAccelTypesKey, path);
			fFramebuffer->setProperty(kIOAccelIndexKey, (UInt64)0, 32);
			fFramebuffer->setProperty(kIOAccelRevisionKey, (UInt64)kCurrentGraphicsInterfaceRevision, 32);
			frb_info(fCore->adev(), "accel: the framebuffer's accelerator is %s", path);
		}
	}
	registerService();
}

void FirebrandAccel::prepareToRetire(void)
{
	struct frb_ring *ring, *sdma;
	UInt32 seq = 0, sseq = 0;
	bool drained = true;

	if (!fCore)
		return;
	/* the video session first, while the decoder runs (its destroy message, the idle work) */
	videoRetire();
	fCore->lock();
	fRetired = true;
	ring = ringLocked();
	if (ring)
		seq = ring->sync_seq;
	sdma = sdmaLocked();
	if (sdma)
		sseq = sdma->sync_seq;
	fCore->unlock();
	if (ring && !waitSeq(seq, FRB_RETIRE_WAIT_MS, true))
		IOLog("FirebrandAccel: the GFX ring did not drain (fence %lu) before the engines stop\n",
		      (unsigned long)seq);
	if (sdma && !waitSdma(sseq, FRB_RETIRE_WAIT_MS, true)) {
		IOLog("FirebrandAccel: SDMA0 did not drain (fence %lu) before the engines stop; its IBs stay\n",
		      (unsigned long)sseq);
		drained = false;
	}
	/* the watchdog ran through the drain (a hung ring is recovered there too); it stops now */
	stopWatchdog();
	/* the IBs' GTT pages, while the GART can still be written (FirebrandCore::stop unmaps BAR0) */
	fCore->lock();
	if (fIbArenaUp && drained) {
		fCore->gttFree(&fIbArena);
		fIbArenaUp = false;
	}
	fCore->unlock();
	if (fFramebuffer) {
		fFramebuffer->removeProperty(kIOAccelTypesKey);
		fFramebuffer->removeProperty(kIOAccelIndexKey);
		fFramebuffer->removeProperty(kIOAccelRevisionKey);
		fFramebuffer->removeProperty("IOCFPlugInTypes");
		fFramebuffer->removeProperty("FW_UVD");
		fFramebuffer->removeProperty("EnableUVDSessions");
	}
}

/*
 * The engines are halted and bus mastering is off (FirebrandCore::stop), so
 * the card can no longer reach the clients' pages: unwire them all, give up
 * the heaps, leave the registry. BAR0 is unmapped by now, so nothing here
 * touches the card (no GART entries are written).
 */
void FirebrandAccel::retire(void)
{
	Pending *p;
	UInt32 i;

	if (!fCore || fReleased)
		return;
	if (!fRetired)
		prepareToRetire();
	/* a session kept for a decoder that stopped answering: its pages, now the engines are halted */
	videoReleaseAll();
	terminate(kIOServiceRequired | kIOServiceSynchronous);
	fCore->lock();
	fReleased = true;
	for (i = 0; fBindings && i < kMaxBindings; i++) {
		if (!fBindings[i].memory)
			continue;
		fBindings[i].memory->complete();
		fBindings[i].memory->release();
		fBindings[i].memory = 0;
	}
	while ((p = fPending)) {
		fPending = p->next;
		if (p->memory) {
			p->memory->complete();
			p->memory->release();
		}
		IOFree(p, sizeof(*p));
	}
	frb_vram_mgr_fini(&fVram);
	fStatus = 0;
	if (fShapes) {
		IOFree(fShapes, fShapeCapacity * sizeof(*fShapes));
		fShapes = 0;
		fShapeCapacity = 0;
	}
	fCore->unlock();
	if (fAncestor) {
		detach(fAncestor);
		fAncestor->detachFromParent(fProvider, gIOServicePlane);
		fAncestor->release();
		fAncestor = 0;
	} else {
		detach(fProvider);
	}
	if (fShim) {
		fShim->terminate(kIOServiceRequired | kIOServiceSynchronous);
		fShim->detachFromParent(fProvider, gIOServicePlane);
		fShim->release();
		fShim = 0;
	}
	IOLog("FirebrandAccel: retired\n");
}

void FirebrandAccel::free(void)
{
	stopWatchdog();
	if (fCore && !fReleased && fOwner) {
		/* setUp failed part way: nothing was submitted, nothing is bound; the core still runs */
		frb_vram_mgr_fini(&fVram);
		if (fIbArenaUp) {
			fCore->lock();
			fCore->gttFree(&fIbArena);
			fCore->unlock();
			fIbArenaUp = false;
		}
		if (fAncestor) {
			fAncestor->detachFromParent(fProvider, gIOServicePlane);
			fAncestor->release();
		}
		if (fShim) {
			fShim->detachFromParent(fProvider, gIOServicePlane);
			fShim->release();
		}
	}
	if (fBindings)
		IOFree(fBindings, kMaxBindings * sizeof(Binding));
	if (fShapes)
		IOFree(fShapes, fShapeCapacity * sizeof(*fShapes));
	fShapes = 0;
	if (fStatusMem)
		fStatusMem->release();
	if (fAperture)
		fAperture->release();
	if (fOwner)
		fOwner->release();
	if (fVideoLock)
		IOLockFree(fVideoLock);
	fVideoLock = 0;
	fBindings = 0;
	fStatusMem = 0;
	fAperture = 0;
	fOwner = 0;
	super::free();
}

IOReturn FirebrandAccel::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
	FirebrandAccelClient *client;

	/* the window server's surface client (FirebrandSurface.h), as RadeonNI hands it out */
	if (type == kIOAccelSurfaceClientType && fSurfaces) {
		FirebrandSurfaceClient *surface;

		if (fRetired)
			return kIOReturnNotReady;
		surface = new FirebrandSurfaceClient;
		if (!surface)
			return kIOReturnNoMemory;
		if (!surface->initWithTask(owningTask, securityID, type) || !surface->attach(this)) {
			surface->release();
			return kIOReturnError;
		}
		if (!surface->start(this)) {
			surface->detach(this);
			surface->release();
			return kIOReturnError;
		}
		*handler = surface;
		return kIOReturnSuccess;
	}
	/* RadeonNI's video client: H.264 on UVD (FirebrandVideo.cpp), once offered */
	if (type == RDN_VIDEO_UC_TYPE && fVideoOffered)
		return newVideoClient(owningTask, securityID, type, handler);
	if (type != RDN_UC_TYPE && type != RDN_UC_TYPE_V2) {
		/*
		 * 'frbg' comes with the VM clients (R phases); type 0 only with
		 * Surfaces on a framebuffer; 'rdnv' unless FirebrandVideo is false.
		 */
		IOLog("FirebrandAccel: user client type 0x%08lx requested: not offered\n", (unsigned long)type);
		return kIOReturnUnsupported;
	}
	if (fRetired)
		return kIOReturnNotReady;
	client = new FirebrandAccelClient;
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/* --- the ring, fences, the status page ----------------------------------------- */

struct frb_ring *FirebrandAccel::ringLocked(void)
{
	return fCore ? frb_gfx_v8_0_ring(fCore->adev()) : 0;
}

/* the newest signalled sequence (wrap-safe), with the status page and the deferred frees brought up to it */
UInt32 FirebrandAccel::completedLocked(void)
{
	struct frb_ring *ring = ringLocked();
	UInt32 now;

	if (!ring)
		return fCompleted;
	now = frb_fence_read(ring);
	if ((SInt32)(now - fCompleted) > 0 && (SInt32)(ring->sync_seq - now) >= 0)
		fCompleted = now;
	updateStatusLocked();
	reapLocked(false);
	return fCompleted;
}

bool FirebrandAccel::signalledLocked(UInt32 seq)
{
	completedLocked();
	return (SInt32)(fCompleted - seq) >= 0;
}

void FirebrandAccel::updateStatusLocked(void)
{
	struct frb_ring *ring = ringLocked();

	if (!fStatus || !ring)
		return;
	fStatus->fence_emitted = ring->sync_seq;
	if ((SInt32)(fCompleted - fStatus->fence_completed) > 0) {
		/* what the fence covers is visible to the CPU before the fence is */
		frb_invalidate_hdp(fCore->adev());
		__asm__ __volatile__("sync" : : : "memory");
		fStatus->fence_completed = fCompleted;
	}
}

/*
 * Wait (polling, no lock held while asleep) for seq. draining: called by
 * prepareToRetire, before the engines stop, so fRetired does not end it.
 */
bool FirebrandAccel::waitSeq(UInt32 seq, UInt32 timeoutMs, bool draining)
{
	UInt32 waited = 0, spin;
	bool done = false, gone;

	for (spin = 0;; spin++) {
		fCore->lock();
		gone = fReleased || (fRetired && !draining) || !ringLocked();
		if (!gone)
			done = signalledLocked(seq);
		fCore->unlock();
		if (gone || done)
			return done;
		if (spin < 100) {
			IODelay(10);
			continue;
		}
		if (waited >= timeoutMs)
			return false;
		IOSleep(1);
		waited++;
	}
}

/* --- the lockup watchdog and recovery ---------------------------------------- */

/*
 * Its own work loop: the timer's action takes the core's lock, and nothing
 * that holds that lock waits on this loop's gate.
 */
void FirebrandAccel::startWatchdog(void)
{
	fWatchLoop = IOWorkLoop::workLoop();
	if (fWatchLoop)
		fWatchdog = IOTimerEventSource::timerEventSource(this, &FirebrandAccel::watchdogFired);
	if (!fWatchdog || fWatchLoop->addEventSource(fWatchdog) != kIOReturnSuccess) {
		IOLog("FirebrandAccel: no lockup watchdog (no work loop or timer); hangs need frbctl recover\n");
		if (fWatchdog)
			fWatchdog->release();
		fWatchdog = 0;
		return;
	}
	fWatchdog->setTimeoutMS(FRB_WATCH_MS);
}

/* once removeEventSource has returned, the action is not running and will not run */
void FirebrandAccel::stopWatchdog(void)
{
	if (fWatchdog) {
		fWatchdog->cancelTimeout();
		if (fWatchLoop)
			fWatchLoop->removeEventSource(fWatchdog);
		fWatchdog->release();
		fWatchdog = 0;
	}
	if (fWatchLoop) {
		fWatchLoop->release();
		fWatchLoop = 0;
	}
}

void FirebrandAccel::watchdogFired(OSObject *owner, IOTimerEventSource *sender)
{
	FirebrandAccel *self = OSDynamicCast(FirebrandAccel, owner);

	if (self)
		self->watchdog();
	(void)sender;
}

/*
 * Each ring the accelerator drives (GFX, SDMA0): with work behind its fence
 * and the fence not moving for FRB_LOCKUP_MS, it is hung (amdgpu_job_
 * timedout), and frb_device_gpu_recover runs. Read only when something is
 * outstanding: a fence read is an HDP invalidate and two PCIe round trips.
 */
void FirebrandAccel::watchdog(void)
{
	struct frb_ring *rings[2];
	UInt32 known[2], stalled = 0, seen;
	unsigned i;

	fCore->lock();
	if (fReleased || fWedged) {
		fCore->unlock();
		return;
	}
	rings[0] = ringLocked();
	rings[1] = sdmaLocked();
	known[0] = fCompleted;
	known[1] = fSdmaCompleted;
	for (i = 0; i < 2; i++) {
		if (!rings[i] || known[i] == rings[i]->sync_seq) {
			fWatchStallMs[i] = 0;
			continue;
		}
		seen = frb_fence_read(rings[i]);
		if (seen == rings[i]->sync_seq || seen != fWatchSeen[i]) {
			fWatchSeen[i] = seen;
			fWatchStallMs[i] = 0;
			continue;
		}
		fWatchStallMs[i] += FRB_WATCH_MS;
		if (fWatchStallMs[i] >= FRB_LOCKUP_MS)
			stalled |= i ? FRB_RECOVER_SDMA : FRB_RECOVER_GFX;
	}
	if (stalled)
		recoverLocked(0, stalled, "lockup");
	fCore->unlock();
	if (fWatchdog)
		fWatchdog->setTimeoutMS(FRB_WATCH_MS);
}

/*
 * frb_device_gpu_recover (frb_reset.c), then the accelerator's view brought
 * up to the forced fences. force 0: the soft recovery, then the busy
 * blocks; when nothing is busy but a ring stands still (stalled), its block
 * is reset regardless, as amdgpu resets on any job timeout.
 */
int FirebrandAccel::recoverLocked(UInt32 force, UInt32 stalled, const char *why)
{
	struct frb_device *adev = fCore->adev();
	struct frb_recover_report rep;
	struct frb_ring *sdma;
	int r;

	frb_err(adev, "accel: %s: GFX fence %u of %u, SDMA0 fence %u of %u; recovering",
		why, (unsigned)fCompleted, ringLocked() ? (unsigned)ringLocked()->sync_seq : 0,
		(unsigned)fSdmaCompleted, sdmaLocked() ? (unsigned)sdmaLocked()->sync_seq : 0);
	r = fCore->recoverLocked(force, &rep);
	if (!r && !force && !rep.soft_recovered && !rep.hung && stalled) {
		frb_info(adev, "accel: %s: no block reads busy, yet the ring stands still: resetting it", why);
		r = fCore->recoverLocked(stalled, &rep);
	}
	fWatchStallMs[0] = fWatchStallMs[1] = 0;
	completedLocked();
	sdma = sdmaLocked();
	if (sdma)
		sdmaSignalledLocked(sdma->sync_seq);
	if (r) {
		fWedged = true;
		frb_err(adev, "accel: %s: recovery failed (%d); the watchdog stops. Reload the driver "
			"(a load resets the ASIC)", why, r);
	} else {
		fRecoveries++;
		frb_info(adev, "accel: %s: recovered (%s, %u ms; recovery %u)", why,
			 rep.soft_recovered ? "the waves were killed" : rep.hung ? "soft reset" : "nothing was hung",
			 (unsigned)rep.ms, (unsigned)fRecoveries);
	}
	return r;
}

IOReturn FirebrandAccel::cpRecover(UInt32 level, SInt32 *result)
{
	static const UInt32 force[4] = { 0, FRB_RECOVER_GFX, FRB_RECOVER_GFX, FRB_RECOVER_GFX | FRB_RECOVER_SDMA };

	if (level > 3)
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	*result = recoverLocked(force[level], 0, "CP_RECOVER");
	if (!*result)
		fWedged = false;
	fCore->unlock();
	return kIOReturnSuccess;
}

/* rate-limited: the first and every 64th, as RadeonNI's fenceWait */
void FirebrandAccel::reportHang(const char *what, UInt32 seq)
{
	struct frb_device *adev = fCore->adev();
	struct frb_ring *ring;

	if (fHangReports++ % 64)
		return;
	fCore->lock();
	ring = ringLocked();
	if (ring && !fRetired)
		frb_err(adev, "accel: %s: fence %u not reached; signalled %u, emitted %u, rptr %u wptr %u, "
			"GRBM_STATUS %08x CP_STAT %08x CP_STALLED_STAT1 %08x (%u reports)",
			what, (unsigned)seq, (unsigned)frb_fence_read(ring), (unsigned)ring->sync_seq,
			(unsigned)ring->funcs->get_rptr(ring), (unsigned)ring->funcs->get_wptr(ring),
			frb_rreg(adev, mmGRBM_STATUS), frb_rreg(adev, mmCP_STAT), frb_rreg(adev, mmCP_STALLED_STAT1),
			(unsigned)fHangReports);
	fCore->unlock();
}

/* --- deferred frees: back once the work submitted before the free has retired --- */

void FirebrandAccel::deferLocked(UInt32 kind, UInt64 offset, UInt64 size, UInt32 first, UInt32 npages,
				 IOMemoryDescriptor *memory)
{
	struct frb_ring *ring = ringLocked();
	Pending *p = (Pending *)IOMalloc(sizeof(Pending));

	if (!p) {
		/* keep it rather than give it back early */
		IOLog("FirebrandAccel: no memory to defer a free; leaking it\n");
		return;
	}
	bzero(p, sizeof(*p));
	p->kind = kind;
	p->seq = ring ? ring->sync_seq : fCompleted;
	p->sdmaSeq = sdmaLocked() ? sdmaLocked()->sync_seq : fSdmaCompleted;
	p->offset = offset;
	p->size = size;
	p->first = first;
	p->npages = npages;
	p->memory = memory;
	p->next = fPending;
	fPending = p;
}

void FirebrandAccel::reapLocked(bool all)
{
	Pending **pp = &fPending, *p;

	while ((p = *pp)) {
		if (!all && ((SInt32)(fCompleted - p->seq) < 0 || !sdmaSignalledLocked(p->sdmaSeq))) {
			pp = &p->next;
			continue;
		}
		*pp = p->next;
		if (p->kind == kPendingVram)
			frb_vram_mgr_free(&fVram, p->offset, p->size);
		if (p->kind == kPendingGart)
			frb_gart_free(fCore->adev(), p->first, p->npages);
		if (p->memory) {
			p->memory->complete();
			p->memory->release();
		}
		IOFree(p, sizeof(*p));
	}
}

/* --- what the clients call --------------------------------------------------- */

IOReturn FirebrandAccel::getInfo(struct rdn_user_info *info)
{
	struct frb_device *adev = fCore->adev();
	struct frb_gfx_info gfx;

	bzero(info, sizeof(*info));
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	frb_gfx_v8_0_get_info(adev, &gfx);
	info->version = RDN_USER_VERSION;
	info->pci_device_id = adev->device;
	info->vram_gpu_base_hi = (UInt32)(adev->gmc.vram_start >> 32);
	info->vram_gpu_base_lo = (UInt32)adev->gmc.vram_start;
	info->aperture_size = fApertureBytes;
	info->heap_offset = (UInt32)fVram.vis_start;
	info->heap_size = (UInt32)(fVram.vis_end - fVram.vis_start);
	/*
	 * radeon's packed tiling dword (cik.c cik_gpu_init, RadeonNI's
	 * rdn_cik_accel.c): pipes in 3:0, banks 7:4, interleave 11:8, row size
	 * 15:12. The GL bundle hands it to Mesa's radeon surface code as
	 * RADEON_INFO_TILING_CONFIG; GMC 8.1 keeps NOOFBANK in bits 1:0.
	 */
	switch (gfx.max_tile_pipes) {
	case 1:		info->tile_config = 0; break;
	case 2:		info->tile_config = 1; break;
	case 4:		info->tile_config = 2; break;
	default:	info->tile_config = 3; break;
	}
	info->tile_config |= ((gfx.mc_arb_ramcfg & MC_ARB_RAMCFG__NOOFBANK_MASK) >> MC_ARB_RAMCFG__NOOFBANK__SHIFT) << 4;
	info->tile_config |= ((gfx.gb_addr_config & GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK) >>
			      GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT) << 8;
	info->tile_config |= ((gfx.gb_addr_config & GB_ADDR_CONFIG__ROW_SIZE_MASK) >> GB_ADDR_CONFIG__ROW_SIZE__SHIFT) << 12;
	info->backend_map = gfx.backend_enable_mask;
	info->max_backends = gfx.max_backends_per_se * gfx.num_se;
	info->max_tile_pipes = gfx.max_tile_pipes;
	info->max_pipes = gfx.max_tile_pipes;
	info->num_ses = gfx.num_se;
	{
		UInt32 off, pitch, w, h, bpp;

		if (fCore->screenLocked(&off, &pitch, &w, &h, &bpp)) {
			info->fb_offset = off;
			info->fb_width = w;
			info->fb_height = h;
			info->fb_pitch_pixels = pitch;
			info->fb_bits_per_pixel = bpp;
		}
	}
	fCore->unlock();
	return kIOReturnSuccess;
}

IOReturn FirebrandAccel::vramInfo64(struct rdn_user_vram_info64 *info)
{
	struct frb_device *adev = fCore->adev();
	UInt64 hidden = fVram.invis_end - fVram.invis_start;

	bzero(info, sizeof(*info));
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	info->version = 1;
	info->slice_count = 0;		/* no resizable BAR: the aperture is the whole CPU view */
	info->slice_bytes = RDN_UC_VRAM_SLICE_BYTES;
	/* GFX8's CP has no working fetch swap: command buffers are little-endian (frb_gfx_v8_0.c ib probe) */
	info->flags = (fStatus ? RDN_VRAM_INFO64_STATUS : 0) | RDN_VRAM_INFO64_LE_IBS;
	info->total_vram_hi = (UInt32)(adev->gmc.real_vram_size >> 32);
	info->total_vram_lo = (UInt32)adev->gmc.real_vram_size;
	info->bar0_size_hi = (UInt32)(adev->aper_size >> 32);
	info->bar0_size_lo = (UInt32)adev->aper_size;
	info->hidden_offset_hi = (UInt32)(fVram.invis_start >> 32);
	info->hidden_offset_lo = (UInt32)fVram.invis_start;
	info->hidden_size_hi = (UInt32)(hidden >> 32);
	info->hidden_size_lo = (UInt32)hidden;
	fCore->unlock();
	return kIOReturnSuccess;
}

/* 32 bits, clamped as RadeonNIAccel::hiddenInfo */
IOReturn FirebrandAccel::hiddenInfo(UInt32 *offset, UInt32 *size)
{
	UInt64 start = fVram.invis_start, bytes = fVram.invis_end - fVram.invis_start;
	UInt64 most = start < 0xff000000ULL ? 0xff000000ULL - start : 0;

	if (fRetired)
		return kIOReturnNoDevice;
	*offset = (UInt32)start;
	*size = (UInt32)(bytes > most ? most : bytes);
	return kIOReturnSuccess;
}

IOReturn FirebrandAccel::allocVram(struct frb_bo_table *bos, UInt64 size, UInt32 align, UInt32 where,
				   UInt64 *offset)
{
	UInt64 off = 0, bytes = 0;
	UInt32 seq = 0;
	int r, tries;

	for (tries = 0; tries < 2; tries++) {
		fCore->lock();
		if (fRetired) {
			fCore->unlock();
			return kIOReturnNoDevice;
		}
		completedLocked();
		r = frb_vram_mgr_alloc(&fVram, size, align, where, &off, &bytes);
		if (r == -EINVAL) {
			fCore->unlock();
			return kIOReturnBadArgument;
		}
		if (!r) {
			if (!frb_bo_table_add(bos, off, bytes)) {
				fCore->unlock();
				*offset = off;
				return kIOReturnSuccess;
			}
			frb_vram_mgr_free(&fVram, off, bytes);
			fCore->unlock();
			return kIOReturnNoMemory;
		}
		/* full: what is waiting to be freed may be what it needs (RadeonNI freed at once) */
		seq = fPending && ringLocked() ? ringLocked()->sync_seq : 0;
		fCore->unlock();
		if (!seq || !waitSeq(seq, 1000, false))
			break;
	}
	return kIOReturnNoMemory;
}

IOReturn FirebrandAccel::freeVram(struct frb_bo_table *bos, UInt64 offset)
{
	UInt64 size = 0;

	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	if (frb_bo_table_remove(bos, offset, &size)) {
		fCore->unlock();
		return kIOReturnBadArgument;
	}
	/* no flush reads a surface's picture from it any more */
	dropSurfaceBuffersLocked(0, offset, size);
	deferLocked(kPendingVram, offset, size, 0, 0, 0);
	completedLocked();
	fCore->unlock();
	return kIOReturnSuccess;
}

/*
 * RDN_UC_SUBMIT: the IB must lie inside one VRAM buffer this client owns
 * (RadeonNI checked only offset >= its heap). On the GFX ring, VMID 0,
 * little-endian (no swap: GFX8's CP has none that works, so the client
 * stores the words as the GPU reads them, RDN_VRAM_INFO64_LE_IBS), after
 * an HDP flush; then the ring's fence. At most kMaxInFlight submissions
 * wait on the ring: a frame is 256 dwords there (gfx_v8_0's align_mask)
 * and the ring 2048. A promised 2D fence (a pending join) is emitted
 * first, so the client's fence comes after it.
 */
IOReturn FirebrandAccel::submit(struct frb_bo_table *bos, UInt64 offset, UInt32 words, UInt32 *fence)
{
	struct frb_ring *ring;
	struct frb_ib ib;
	uint32_t seq = 0;
	int r;

	if (!words || words > FRB_MAX_IB_WORDS || (offset & 3))
		return kIOReturnBadArgument;
	fCore->lock();
	for (;;) {
		IOReturn room;

		if (fRetired || !(ring = ringLocked())) {
			fCore->unlock();
			return kIOReturnNoDevice;
		}
		if (!frb_bo_table_contains(bos, offset, (UInt64)words * 4)) {
			fCore->unlock();
			return kIOReturnBadArgument;
		}
		/* 2D work's promised fence first: the client's comes after it on the GFX timeline */
		room = fJoinPending ? flushJoinLocked() : gfxRoomLocked();
		if (room != kIOReturnSuccess) {
			fCore->unlock();
			return room;
		}
		/* both may have let go of the lock: look again */
		if (!fJoinPending && !fRetired && ringLocked() &&
		    (UInt32)(ring->sync_seq - fCompleted) < kMaxInFlight)
			break;
	}
	bzero(&ib, sizeof(ib));
	ib.gpu_addr = fCore->adev()->gmc.vram_start + offset;
	ib.length_dw = words;
	ib.size_dw = words;
	ib.flags = 0;
	r = frb_ib_schedule(ring, &ib, &seq);
	if (!r)
		fClientGfxSeq = seq;	/* 2D work from now on waits for it */
	updateStatusLocked();
	fCore->unlock();
	if (r)
		return kIOReturnIOError;
	*fence = seq;
	return kIOReturnSuccess;
}

/*
 * RDN_UC_FENCE_WAIT: spin 2 ms, then look every millisecond (RadeonNI's
 * fenceWait: 200 x 10 us, then 1 ms sleeps); 0 ms only looks. A sequence
 * the ring never emitted is not reached, without waiting.
 */
IOReturn FirebrandAccel::fenceWait(UInt32 fence, UInt32 timeoutMs, UInt32 *reached)
{
	struct frb_ring *ring;
	UInt32 waited = 0, spin;
	bool done;

	*reached = 0;
	for (spin = 0;; spin++) {
		fCore->lock();
		if (fRetired || !(ring = ringLocked())) {
			fCore->unlock();
			return kIOReturnNoDevice;
		}
		/* a 2D call's fence: the join is emitted now (it covers SDMA0's work) */
		if (fJoinPending && fence == fJoinSeq) {
			IOReturn r = flushJoinLocked();

			if (r != kIOReturnSuccess) {
				fCore->unlock();
				return r == kIOReturnTimeout ? kIOReturnSuccess : r;
			}
		}
		/* frbctl's GFX tests took the promised number: the join came after it */
		if (fence == fJoinPromised && fJoinEmitted != fJoinPromised && !fJoinPending)
			fence = fJoinEmitted;
		if ((SInt32)(fence - ring->sync_seq) > 0) {
			fCore->unlock();
			return kIOReturnSuccess;
		}
		done = signalledLocked(fence);
		fCore->unlock();
		if (done) {
			*reached = 1;
			return kIOReturnSuccess;
		}
		if (!timeoutMs)
			return kIOReturnSuccess;
		if (spin < 200) {
			IODelay(10);
			continue;
		}
		if (waited >= timeoutMs) {
			reportHang("fence wait", fence);
			return kIOReturnSuccess;
		}
		IOSleep(1);
		waited++;
	}
}

/* RDN_UC_SYNC_FOR_CPU: what the GPU wrote, the CPU sees (vi_flush_hdp, vi_invalidate_hdp) */
IOReturn FirebrandAccel::syncForCPU(void)
{
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	frb_flush_hdp(fCore->adev());
	frb_invalidate_hdp(fCore->adev());
	completedLocked();
	fCore->unlock();
	return kIOReturnSuccess;
}

/*
 * RDN_UC_REG_READ, a BYTE offset: vi_read_register's allowlist for
 * everyone (gfx_v8_0's copies of the tiling tables and RB registers), the
 * register itself for an administrator otherwise (docs/accel-interface.md 2.4).
 */
IOReturn FirebrandAccel::regRead(UInt32 byteOffset, bool admin, UInt32 *value)
{
	struct frb_device *adev = fCore->adev();
	IOReturn ret = kIOReturnSuccess;
	uint32_t v = 0;

	*value = 0;
	if ((byteOffset & 3) || byteOffset >= adev->rmmio_size)
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired)
		ret = kIOReturnNoDevice;
	else if (!frb_vi_read_register(adev, 0xffffffff, 0xffffffff, byteOffset / 4, &v))
		*value = v;
	else if (admin)
		*value = frb_rreg(adev, byteOffset / 4);
	else
		ret = kIOReturnNotPrivileged;
	fCore->unlock();
	return ret;
}

/* RDN_UC_REG_WRITE: the administrator, and only with FirebrandAllowRegWrite / frb_regwrite=1 (as frbctl wr) */
IOReturn FirebrandAccel::regWrite(UInt32 byteOffset, UInt32 value)
{
	struct frb_device *adev = fCore->adev();
	IOReturn ret = kIOReturnSuccess;

	if ((byteOffset & 3) || byteOffset >= adev->rmmio_size)
		return kIOReturnBadArgument;
	if (!fCore->allowRegWrite())
		return kIOReturnNotPermitted;
	fCore->lock();
	if (fRetired) {
		ret = kIOReturnNoDevice;
	} else {
		IOLog("FirebrandAccel: REG_WRITE %05lx = %08lx\n", (unsigned long)(byteOffset / 4), (unsigned long)value);
		frb_wreg(adev, byteOffset / 4, value);
	}
	fCore->unlock();
	return ret;
}

/* RDN_UC_GART_INFO: on only where the GART start fits the 32 bits the call has (adev.gart_legacy) */
IOReturn FirebrandAccel::gartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages)
{
	struct frb_device *adev = fCore->adev();

	if (fRetired)
		return kIOReturnNoDevice;
	*on = adev->gart.ready && adev->gmc.gart_start + adev->gmc.gart_size <= 0x100000000ULL;
	*gpuStart = (UInt32)adev->gmc.gart_start;
	*pages = adev->gart.num_gpu_pages;
	return kIOReturnSuccess;
}

/*
 * RDN_UC_GART_BIND(64): wire a piece of the program's memory (it stays the
 * program's: no kernel address space is spent on it) and enter its pages
 * in VMID 0's GART, cached and snooped (FRB_PTE_GTT), at their bus
 * addresses through inbound window 2. Returns the byte offset in the GART.
 */
IOReturn FirebrandAccel::gartBind(void *owner, task_t task, UInt64 address, UInt32 size, UInt32 *offset)
{
	struct frb_device *adev = fCore->adev();
	IOMemoryDescriptor *memory;
	uint32_t first = 0;
	UInt32 npages, slot, i;
	UInt32 on = 0, start = 0, pages = 0;
	IOReturn ret = kIOReturnSuccess;

	if (!size || (address & 4095) || (size & 4095))
		return kIOReturnBadArgument;
	if (gartInfo(&on, &start, &pages) || !on)
		return kIOReturnNotReady;
	npages = size / 4096;
	if (npages >= pages)
		return kIOReturnNoResources;
	memory = IOMemoryDescriptor::withAddressRange(address, size, kIODirectionOutIn, task);
	if (!memory)
		return kIOReturnNoMemory;
	if (memory->prepare(kIODirectionOutIn) != kIOReturnSuccess) {
		memory->release();
		return kIOReturnVMError;
	}
	fCore->lock();
	for (slot = 0; slot < kMaxBindings && fBindings[slot].memory; slot++)
		;
	if (fRetired)
		ret = kIOReturnNoDevice;
	else if (slot == kMaxBindings)
		ret = kIOReturnNoResources;
	else if (frb_gart_alloc(adev, npages, &first)) {
		/* what was unbound a moment ago may be what it needs */
		completedLocked();
		if (frb_gart_alloc(adev, npages, &first))
			ret = kIOReturnNoResources;
	}
	for (i = 0; ret == kIOReturnSuccess && i < npages; i++) {
		IOByteCount len = 0;
		addr64_t phys = memory->getPhysicalSegment64((IOByteCount)i * 4096, &len);
		UInt64 bus = (UInt64)phys + FRB_PCI_INBOUND_BASE;

		if (!phys || (phys & 4095) || len < 4096 || phys >= FRB_PCI_INBOUND_SIZE ||
		    !frb_gart_bus_addr_ok(adev, bus)) {
			IOLog("FirebrandAccel: GART_BIND: page %lu at 0x%llx has no bus address\n", (unsigned long)i,
			      (unsigned long long)phys);
			ret = kIOReturnVMError;
			break;
		}
		frb_gart_set_pte(adev, first + i, bus, FRB_PTE_GTT);
	}
	if (ret == kIOReturnSuccess) {
		frb_gart_invalidate_tlb(adev);
		fCore->busMaster();
		fBindings[slot].memory = memory;
		fBindings[slot].first = first;
		fBindings[slot].npages = npages;
		fBindings[slot].owner = owner;
		*offset = first * 4096;
	} else if (first && !fRetired) {
		/* take the entries back (the dummy page) and the pages */
		frb_gart_unbind(adev, first, npages);
		frb_gart_free(adev, first, npages);
	}
	fCore->unlock();
	if (ret != kIOReturnSuccess) {
		memory->complete();
		memory->release();
	}
	return ret;
}

/*
 * RDN_UC_GART_UNBIND: the entries point at the dummy page at once (the
 * GPU reads harmless zeroes from here on); the pages stay wired, and the
 * GART range taken, until the work submitted before the unbind has retired.
 */
IOReturn FirebrandAccel::gartUnbind(void *owner, UInt32 offset)
{
	struct frb_device *adev = fCore->adev();
	UInt32 slot;

	if (offset & 4095)
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return kIOReturnNoDevice;
	}
	for (slot = 0; slot < kMaxBindings; slot++)
		if (fBindings[slot].memory && fBindings[slot].owner == owner && fBindings[slot].first == offset / 4096)
			break;
	if (slot == kMaxBindings) {
		fCore->unlock();
		return kIOReturnBadArgument;
	}
	frb_gart_unbind(adev, fBindings[slot].first, fBindings[slot].npages);
	deferLocked(kPendingGart, 0, 0, fBindings[slot].first, fBindings[slot].npages, fBindings[slot].memory);
	bzero(&fBindings[slot], sizeof(fBindings[slot]));
	completedLocked();
	fCore->unlock();
	return kIOReturnSuccess;
}

/*
 * A client's end (close or death): its buffers and bindings go once what
 * was submitted before has retired. Waits up to 2 s for that, as RadeonNI's
 * releaseClientMemory waited; if the GPU is stuck, they stay deferred and
 * go when it moves again, or at retire() once the engines are halted. The
 * surfaces whose pictures it registered are forgotten first (RadeonNI's
 * releaseClientMemory), so no flush reads them afterwards.
 */
void FirebrandAccel::releaseClient(void *owner, struct frb_bo_table *bos)
{
	struct frb_device *adev = fCore->adev();
	struct frb_ring *ring;
	UInt32 i, slot, seq = 0, sseq = 0, freed = 0, unbound = 0;

	fCore->lock();
	ring = ringLocked();
	dropSurfaceBuffersLocked(owner, 0, 0);
	for (i = 0; bos && i < bos->count; i++)
		dropSurfaceBuffersLocked(0, bos->e[i].offset, bos->e[i].size);
	if (fRetired || !ring) {
		/* retire() takes back everything once the engines are halted */
		fCore->unlock();
		return;
	}
	for (i = 0; bos && i < bos->count; i++, freed++)
		deferLocked(kPendingVram, bos->e[i].offset, bos->e[i].size, 0, 0, 0);
	if (bos)
		bos->count = 0;
	for (slot = 0; slot < kMaxBindings; slot++) {
		if (!fBindings[slot].memory || fBindings[slot].owner != owner)
			continue;
		frb_gart_unbind(adev, fBindings[slot].first, fBindings[slot].npages);
		deferLocked(kPendingGart, 0, 0, fBindings[slot].first, fBindings[slot].npages, fBindings[slot].memory);
		bzero(&fBindings[slot], sizeof(fBindings[slot]));
		unbound++;
	}
	seq = ring->sync_seq;
	sseq = sdmaLocked() ? sdmaLocked()->sync_seq : 0;
	completedLocked();
	fCore->unlock();
	if ((freed || unbound) && (!waitSeq(seq, FRB_RETIRE_WAIT_MS, false) ||
				   (sdmaLocked() && !waitSdma(sseq, FRB_RETIRE_WAIT_MS, false))))
		IOLog("FirebrandAccel: a client's %lu buffers and %lu bindings wait for fence %lu (sdma0 %lu)\n",
		      (unsigned long)freed, (unsigned long)unbound, (unsigned long)seq, (unsigned long)sseq);
}

/* --- the user client ----------------------------------------------------------- */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(FirebrandAccelClient, IOUserClient)

bool FirebrandAccelClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!super::initWithTask(owningTask, securityID, type))
		return false;
	fTask = owningTask;
	fV2 = type == RDN_UC_TYPE_V2;
	fAdmin = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
	return true;
}

bool FirebrandAccelClient::start(IOService *provider)
{
	fAccel = OSDynamicCast(FirebrandAccel, provider);
	if (!fAccel || !fAccel->os() || !super::start(provider))
		return false;
	fAccel->retain();
	frb_bo_table_init(&fBos, fAccel->os());
	return true;
}

void FirebrandAccelClient::releaseAll(void)
{
	if (fReleased || !fAccel)
		return;
	fReleased = true;
	fAccel->releaseClient(this, &fBos);
}

IOReturn FirebrandAccelClient::clientClose(void)
{
	releaseAll();
	terminate();
	return kIOReturnSuccess;
}

IOReturn FirebrandAccelClient::clientDied(void)
{
	return clientClose();
}

void FirebrandAccelClient::stop(IOService *provider)
{
	releaseAll();
	super::stop(provider);
}

void FirebrandAccelClient::free(void)
{
	if (fAccel) {
		releaseAll();
		frb_bo_table_fini(&fBos);
		fAccel->release();
		fAccel = 0;
	}
	super::free();
}

/*
 * RadeonNI's table (RadeonNIUserClient::getTargetAndMethodForIndex), with
 * its argument counts: 'rdn1' gets 0-31, 'rdn2' 0-52 and Firebrand's
 * FRB_UC_SCREEN_TARGET (0x3f, an administrator's). G1 implements 0-5,
 * 11-16, 32 and 47-50 (and REG_WRITE for the administrator); G2 the
 * surfaces and screen blits (6-9, 17, 18, 33, 34, 44-46, 51); power
 * answers kIOReturnUnsupported until G3, the decoder kIOReturnNotReady
 * (RadeonNI without FW_UVD), the diagnostics kIOReturnNotPrivileged
 * unless the caller is an administrator.
 */
IOExternalMethod *FirebrandAccelClient::getTargetAndMethodForIndex(IOService **target_, UInt32 index)
{
#define FRB_M(fn, kind, in, out)	{ 0, (IOMethod)&FirebrandAccelClient::fn, kind, in, out }
#define FRB_SISO(fn, in, out)		FRB_M(fn, kIOUCScalarIScalarO, in, out)
	static const IOExternalMethod methods[RDN_UC_METHOD_COUNT] = {
		FRB_M(methodGetInfo, kIOUCScalarIStructO, 0, sizeof(struct rdn_user_info)),	/* 0 */
		FRB_SISO(methodAlloc, 2, 1),
		FRB_SISO(methodFree, 1, 0),
		FRB_SISO(methodSubmit, 2, 1),
		FRB_SISO(methodFenceWait, 2, 1),
		FRB_SISO(methodSyncForCPU, 0, 0),						/* 5 */
		FRB_M(methodSurfaceRegion, kIOUCScalarIStructO, 1, sizeof(struct rdn_user_region)),
		FRB_SISO(methodSurfaceBuffer, 5, 0),
		FRB_M(methodSurfaceList, kIOUCScalarIStructO, 0, sizeof(struct rdn_user_surfaces)),
		FRB_SISO(methodSurfaceLocked, 0, 1),
		FRB_SISO(methodUnsupported, 2, 3),						/* 10 POWER */
		FRB_SISO(methodRegRead, 1, 1),
		FRB_SISO(methodAllocHidden, 2, 1),
		FRB_SISO(methodHiddenInfo, 0, 2),
		FRB_SISO(methodGartInfo, 0, 3),
		FRB_SISO(methodGartBind, 2, 1),							/* 15 */
		FRB_SISO(methodGartUnbind, 1, 0),
		FRB_SISO(methodScreenFill, 5, 1),
		FRB_SISO(methodScreenCopy, 5, 1),
		FRB_SISO(methodNotReady, 1, 1),							/* UVD_INIT */
		FRB_SISO(methodNotReady, 2, 1),							/* 20 UVD_SUBMIT */
		FRB_SISO(methodNotReady, 2, 1),							/* UVD_FENCE_WAIT */
		FRB_SISO(methodNotReady, 0, 3),							/* UVD_INFO */
		FRB_SISO(methodNotReady, 0, 1),							/* UVD_RESET */
		FRB_SISO(methodNotReady, 0, 1),							/* UVD_EPOCH */
		FRB_SISO(methodAdminOnly, 1, 1),						/* 25 IBLOG */
		FRB_SISO(methodAdminOnly, 1, 1),						/* DEBUG */
		FRB_SISO(methodNotReady, 0, 3),							/* UVD_SEGMENT */
		FRB_SISO(methodNotReady, 2, 1),							/* ALLOC_UVD */
		FRB_SISO(methodNotReady, 3, 0),							/* VRAM_PUT */
		FRB_SISO(methodNotReady, 3, 0),							/* 30 VRAM_GET */
		FRB_SISO(methodNotReady, 2, 1),							/* UVD_SUBMIT_WORDS */
		FRB_SISO(methodGartBind64, 3, 1),
		FRB_M(methodSurfaceBacking, kIOUCScalarIStructO, 1, sizeof(struct rdn_user_backing)),
		FRB_M(methodSurfaceQuery, kIOUCScalarIStructO, 1, sizeof(struct rdn_user_surface_info)),
		FRB_M(methodNotReady, kIOUCScalarIStructO, 0, sizeof(struct rdn_user_uvd_diagnostic)),	/* 35 */
		FRB_SISO(methodNotReady, 1, 1),							/* UVD_DIAGNOSTIC_INIT */
		FRB_M(methodNotReady, kIOUCScalarIStructO, 1, sizeof(struct rdn_user_uvd_fixture)),
		FRB_M(methodNotReady, kIOUCScalarIStructO, 2, RDN_UVD_FIXTURE_CHUNK_BYTES),
		FRB_SISO(methodRegWrite, 2, 0),
		FRB_M(methodAdminOnly, kIOUCScalarIStructI, 0, 0xffffffff),			/* 40 RING_WORDS */
		FRB_SISO(methodCpRecover, 1, 1),						/* CP_RECOVER */
		FRB_SISO(methodAdminOnly, 1, 1),						/* CLOCK_GATING */
		FRB_SISO(methodAdminOnly, 2, 2),						/* FENCE_PROBE */
		FRB_M(methodScreenCopyRegion, kIOUCStructIStructO, 0xffffffff, sizeof(struct rdn_user_fence)),
		FRB_SISO(methodSurfaceGL, 1, 0),						/* 45 */
		FRB_M(methodSurfaceInit, kIOUCStructIStructO, 0xffffffff, sizeof(struct rdn_user_surface_init_out)),
		FRB_M(methodVramInfo64, kIOUCScalarIStructO, 0, sizeof(struct rdn_user_vram_info64)),
		FRB_SISO(methodAlloc64, 4, 2),
		FRB_SISO(methodFree64, 2, 0),
		FRB_SISO(methodSubmit64, 3, 1),							/* 50 */
		FRB_SISO(methodSurfaceAcquire, 1, 1),
		FRB_SISO(methodUnsupported, 0, 4),						/* POWER_DETAIL */
	};
	static const IOExternalMethod target = FRB_SISO(methodScreenTarget, 5, 0);
#undef FRB_SISO
#undef FRB_M

	if (fV2 && index == FRB_UC_SCREEN_TARGET) {
		*target_ = this;
		return (IOExternalMethod *)&target;
	}
	if (index >= (fV2 ? (UInt32)RDN_UC_METHOD_COUNT : (UInt32)RDN_UC_V1_METHOD_COUNT))
		return 0;
	*target_ = this;
	return (IOExternalMethod *)&methods[index];
}

IOReturn FirebrandAccelClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOMemoryDescriptor *desc = 0;

	if (!fAccel)
		return kIOReturnBadArgument;
	if (type == RDN_UC_MEMORY_APERTURE) {
		desc = fAccel->apertureMemory();
	} else if (type == RDN_UC_MEMORY_STATUS) {
		desc = fAccel->statusMemory();
		if (desc && options)
			*options |= kIOMapReadOnly;
	} else if (type >= RDN_UC_MEMORY_VRAM_SLICE_BASE &&
		   type < RDN_UC_MEMORY_VRAM_SLICE_BASE + RDN_UC_VRAM_MAX_SLICES) {
		return kIOReturnNotReady;	/* no resizable BAR: VRAM_INFO64 reports no slices */
	} else {
		return kIOReturnBadArgument;
	}
	if (!desc)
		return kIOReturnNotReady;
	desc->retain();
	*memory = desc;
	return kIOReturnSuccess;
}

IOReturn FirebrandAccelClient::methodGetInfo(struct rdn_user_info *info, IOByteCount *size)
{
	if (*size < sizeof(*info))
		return kIOReturnBadArgument;
	*size = sizeof(*info);
	return fAccel->getInfo(info);
}

IOReturn FirebrandAccelClient::methodAlloc(UInt32 size, UInt32 align, UInt32 *offset)
{
	UInt64 off = 0;
	IOReturn r = fAccel->allocVram(&fBos, size, align, FRB_VRAM_VISIBLE | FRB_VRAM_BELOW_4G, &off);

	*offset = (UInt32)off;
	return r;
}

IOReturn FirebrandAccelClient::methodAllocHidden(UInt32 size, UInt32 align, UInt32 *offset)
{
	UInt64 off = 0;
	IOReturn r = fAccel->allocVram(&fBos, size, align, FRB_VRAM_INVISIBLE | FRB_VRAM_BELOW_4G, &off);

	*offset = (UInt32)off;
	return r;
}

/* RDN_ALLOC64_HIDDEN beyond the window, unless APERTURE256 (RadeonNIAccel::allocVram64) */
IOReturn FirebrandAccelClient::methodAlloc64(UInt32 sizeHi, UInt32 sizeLo, UInt32 align, UInt32 flags,
					     UInt32 *offHi, UInt32 *offLo)
{
	UInt64 off = 0;
	UInt32 where = ((flags & RDN_ALLOC64_HIDDEN) && !(flags & RDN_ALLOC64_APERTURE256)) ?
		FRB_VRAM_INVISIBLE : FRB_VRAM_VISIBLE;
	IOReturn r;

	if (flags & RDN_ALLOC64_BELOW_4G)
		where |= FRB_VRAM_BELOW_4G;
	r = fAccel->allocVram(&fBos, ((UInt64)sizeHi << 32) | sizeLo, align, where, &off);
	*offHi = (UInt32)(off >> 32);
	*offLo = (UInt32)off;
	return r;
}

IOReturn FirebrandAccelClient::methodFree(UInt32 offset)
{
	return fAccel->freeVram(&fBos, offset);
}

IOReturn FirebrandAccelClient::methodFree64(UInt32 offHi, UInt32 offLo)
{
	return fAccel->freeVram(&fBos, ((UInt64)offHi << 32) | offLo);
}

IOReturn FirebrandAccelClient::methodSubmit(UInt32 offset, UInt32 words, UInt32 *fence)
{
	*fence = 0;
	return fAccel->submit(&fBos, offset, words, fence);
}

IOReturn FirebrandAccelClient::methodSubmit64(UInt32 offHi, UInt32 offLo, UInt32 words, UInt32 *fence)
{
	*fence = 0;
	return fAccel->submit(&fBos, ((UInt64)offHi << 32) | offLo, words, fence);
}

IOReturn FirebrandAccelClient::methodFenceWait(UInt32 fence, UInt32 timeoutMs, UInt32 *reached)
{
	return fAccel->fenceWait(fence, timeoutMs, reached);
}

IOReturn FirebrandAccelClient::methodSyncForCPU(void)
{
	return fAccel->syncForCPU();
}

IOReturn FirebrandAccelClient::methodRegRead(UInt32 offset, UInt32 *value)
{
	return fAccel->regRead(offset, fAdmin, value);
}

IOReturn FirebrandAccelClient::methodRegWrite(UInt32 offset, UInt32 value)
{
	if (!fAdmin)
		return kIOReturnNotPrivileged;
	return fAccel->regWrite(offset, value);
}

IOReturn FirebrandAccelClient::methodHiddenInfo(UInt32 *offset, UInt32 *size)
{
	return fAccel->hiddenInfo(offset, size);
}

IOReturn FirebrandAccelClient::methodVramInfo64(struct rdn_user_vram_info64 *info, IOByteCount *size)
{
	if (*size < sizeof(*info))
		return kIOReturnBadArgument;
	*size = sizeof(*info);
	return fAccel->vramInfo64(info);
}

IOReturn FirebrandAccelClient::methodGartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages)
{
	return fAccel->gartInfo(on, gpuStart, pages);
}

/* a 32-bit address: what a ppc program has (a ppc64 one calls GART_BIND64) */
IOReturn FirebrandAccelClient::methodGartBind(UInt32 address, UInt32 size, UInt32 *offset)
{
	*offset = 0;
	return fAccel->gartBind(this, fTask, address, size, offset);
}

IOReturn FirebrandAccelClient::methodGartBind64(UInt32 low, UInt32 high, UInt32 size, UInt32 *offset)
{
	*offset = 0;
	return fAccel->gartBind(this, fTask, ((UInt64)high << 32) | low, size, offset);
}

IOReturn FirebrandAccelClient::methodGartUnbind(UInt32 offset)
{
	return fAccel->gartUnbind(this, offset);
}

/* --- G2: the surfaces and the screen ------------------------------------------ */

IOReturn FirebrandAccelClient::methodSurfaceRegion(UInt32 wid, struct rdn_user_region *region, IOByteCount *size)
{
	if (*size < sizeof(*region))
		return kIOReturnBadArgument;
	if (!fAccel->getSurfaceRegion(wid, region))
		return kIOReturnNotFound;
	*size = sizeof(*region);
	return kIOReturnSuccess;
}

/* RadeonNIUserClient::methodSurfaceBuffer: offset ~0 and nothing else releases the reservation */
IOReturn FirebrandAccelClient::methodSurfaceBuffer(UInt32 wid, UInt32 offset, UInt32 rowBytes, UInt32 width,
						   UInt32 height)
{
	if (offset == 0xffffffffU && !rowBytes && !width && !height) {
		fAccel->forgetSurface(wid);
		return kIOReturnSuccess;
	}
	return fAccel->setSurfaceBuffer(&fBos, fTask, this, wid, offset, rowBytes, width, height);
}

IOReturn FirebrandAccelClient::methodSurfaceList(struct rdn_user_surfaces *list, IOByteCount *size)
{
	if (*size < sizeof(*list))
		return kIOReturnBadArgument;
	fAccel->listSurfaces(list);
	*size = sizeof(*list);
	return kIOReturnSuccess;
}

IOReturn FirebrandAccelClient::methodSurfaceLocked(UInt32 *wid)
{
	*wid = fAccel->readLocked();
	return kIOReturnSuccess;
}

IOReturn FirebrandAccelClient::methodScreenFill(UInt32 x, UInt32 y, UInt32 width, UInt32 height, UInt32 colour,
						UInt32 *fence)
{
	*fence = 0;
	return fAccel->screenFill(&fBos, fHasTarget ? &fTarget : 0, (SInt32)x, (SInt32)y, (SInt32)width,
				  (SInt32)height, colour, fence);
}

/* width and height share an argument: a method has six in all */
IOReturn FirebrandAccelClient::methodScreenCopy(UInt32 sx, UInt32 sy, UInt32 dx, UInt32 dy, UInt32 size,
						UInt32 *fence)
{
	*fence = 0;
	return fAccel->screenCopy(&fBos, fHasTarget ? &fTarget : 0, (SInt32)sx, (SInt32)sy, (SInt32)dx, (SInt32)dy,
				  (SInt32)(size & 0xffff), (SInt32)(size >> 16), fence);
}

IOReturn FirebrandAccelClient::methodSurfaceBacking(UInt32 wid, struct rdn_user_backing *backing, IOByteCount *size)
{
	if (*size < sizeof(*backing))
		return kIOReturnBadArgument;
	if (!fAccel->getSurfaceBacking(wid, fTask, backing))
		return kIOReturnNotFound;
	*size = sizeof(*backing);
	return kIOReturnSuccess;
}

IOReturn FirebrandAccelClient::methodSurfaceQuery(UInt32 wid, struct rdn_user_surface_info *info, IOByteCount *size)
{
	if (*size < sizeof(*info))
		return kIOReturnBadArgument;
	fAccel->querySurface(wid, info);
	*size = sizeof(*info);
	return kIOReturnSuccess;
}

/* 12 + 16 a rectangle, 1 to RDN_USER_REGION_MAX_RECTS of them (255 arrive inline: rdn_user.h) */
IOReturn FirebrandAccelClient::methodScreenCopyRegion(const void *in, struct rdn_user_fence *out, IOByteCount inSize,
						      IOByteCount *outSize)
{
	const struct rdn_user_region_move *move = (const struct rdn_user_region_move *)in;
	UInt32 fence = 0;
	IOReturn ret;

	if (!in || !out || !outSize || *outSize != sizeof(*out) ||
	    inSize < offsetof(struct rdn_user_region_move, rect))
		return kIOReturnBadArgument;
	out->fence = 0;
	if (!move->count || move->count > RDN_USER_REGION_MAX_RECTS ||
	    inSize < offsetof(struct rdn_user_region_move, rect) + move->count * sizeof(move->rect[0]))
		return kIOReturnBadArgument;
	ret = fAccel->screenCopyRegion(&fBos, fHasTarget ? &fTarget : 0, move, move->count, &fence);
	out->fence = fence;
	return ret;
}

IOReturn FirebrandAccelClient::methodSurfaceGL(UInt32 wid)
{
	if (!wid)
		return kIOReturnBadArgument;
	return fAccel->markSurfaceGL(wid) ? kIOReturnSuccess : kIOReturnNoResources;
}

IOReturn FirebrandAccelClient::methodSurfaceInit(const struct rdn_user_surface_init *in,
						 struct rdn_user_surface_init_out *out, IOByteCount inSize,
						 IOByteCount *outSize)
{
	UInt32 synced = 0;
	IOReturn ret;

	if (!in || !out || !outSize || inSize != sizeof(*in) || *outSize < sizeof(*out))
		return kIOReturnBadArgument;
	bzero(out, sizeof(*out));
	ret = fAccel->initSurfaceBuffer(&fBos, fTask, this, in, &synced);
	out->synced = synced;
	*outSize = sizeof(*out);
	return ret;
}

IOReturn FirebrandAccelClient::methodSurfaceAcquire(UInt32 wid, UInt32 *copied)
{
	*copied = 0;
	if (!wid)
		return kIOReturnBadArgument;
	return fAccel->acquireSurfaceForGPU(wid, copied);
}

/* FRB_UC_SCREEN_TARGET: an administrator's buffer as this client's screen (the tests); width 0 undoes it */
IOReturn FirebrandAccelClient::methodScreenTarget(UInt32 offHi, UInt32 offLo, UInt32 pitch, UInt32 width,
						  UInt32 height)
{
	struct frb_rect_surface t;
	IOReturn ret;

	if (!fAdmin)
		return kIOReturnNotPrivileged;
	if (!width) {
		fHasTarget = false;
		return kIOReturnSuccess;
	}
	ret = fAccel->screenTarget(&fBos, ((UInt64)offHi << 32) | offLo, pitch, width, height, &t);
	if (ret == kIOReturnSuccess) {
		fTarget = t;
		fHasTarget = true;
	}
	return ret;
}

IOReturn FirebrandAccelClient::methodUnsupported(void)
{
	return kIOReturnUnsupported;
}

IOReturn FirebrandAccelClient::methodNotReady(void)
{
	return kIOReturnNotReady;
}

IOReturn FirebrandAccelClient::methodCpRecover(UInt32 level, SInt32 *result)
{
	if (!fAdmin)
		return kIOReturnNotPrivileged;
	return fAccel->cpRecover(level, result);
}

IOReturn FirebrandAccelClient::methodAdminOnly(void)
{
	return fAdmin ? kIOReturnUnsupported : kIOReturnNotPrivileged;
}
