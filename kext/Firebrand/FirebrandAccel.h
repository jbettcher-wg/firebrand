/*
 * FirebrandAccel: the accelerator service, and RadeonNI's legacy user
 * client protocol ('rdn1', 'rdn2'; RadeonNI's hw/rdn_user.h) on top of
 * it. Phases G0 and G1 of docs/accel-interface.md (section 4).
 *
 *   IOPCIDevice
 *    |- FirebrandFramebuffer : RadeonNI : IOFramebuffer   (or Firebrand, headless)
 *    |- FirebrandAGPShim : IOAGPDevice                    AGPShim bit 1 (registered, "model")
 *    `- [IOAGPDevice "FirebrandAGPAncestor"]             AGPShim bit 0
 *        `- FirebrandAccel : RadeonNIAccel : IOAccelerator
 *             |- FirebrandAccelClient ('rdn1', 'rdn2')
 *             `- FirebrandSurfaceClient (type 0, the window server's; FirebrandSurface.h)
 *
 * The owner (FirebrandFramebuffer or the control personality Firebrand)
 * creates it once the stages reached sdma, when FirebrandAccel / boot-arg
 * frb_accel=1 asks for it. Under the framebuffer it publishes what the
 * window server and OpenGL look for (IOGLBundleName, the framebuffer's
 * IOAccelTypes, the GA plug-in's IOCFPlugInTypes); under the control
 * personality it is headless (no IOAccelTypes) and can be unloaded, which a
 * framebuffer the window server has opened cannot (FirebrandFramebuffer.cpp
 * stop()).
 *
 * The legacy model (docs/accel-interface.md 1.9, 3.9): command buffers run
 * on the GFX ring in VMID 0 at MC addresses, little-endian (RadeonNI's
 * clients wrote them in the CPU's order and the CP swapped them; GFX8's CP
 * has no working swap, so on Firebrand the client swaps, and VRAM_INFO64
 * says so with RDN_VRAM_INFO64_LE_IBS); a client's VRAM is buffers it owns
 * in the visible or invisible heap (amdgpu/frb_bo.c); its system memory is
 * bound into VMID 0's GART, which the accelerator moves below 4 GB
 * (adev.gart_legacy); fences are the GFX ring's 32-bit sequence, mirrored
 * in a read-only status page. Clients are as trusted as under RadeonNI:
 * VMID 0 reaches all of VRAM and the GART. What Firebrand adds is
 * ownership (a client frees and submits only from its own buffers),
 * deferred freeing (memory goes back once the work submitted before the
 * free has retired) and the register allowlist for REG_READ.
 *
 * G2, the window server's: the surface client (type 0) and RadeonNI's
 * surface registry (RDN_UC_SURFACE_*), and the 2D work RadeonNI did with
 * its 3D engine or CP DMA: SCREEN_FILL/COPY/COPY_REGION, a surface's flush
 * to the screen, the backing copies. Here they run on SDMA0 (amdgpu/
 * frb_rect.c builds the IBs, in GTT), and the GFX ring's 32-bit sequence
 * stays the one timeline the clients see: SDMA waits on the GPU for the
 * clients' GFX work before it reads or writes (a POLL_REGMEM on the GFX
 * fence), and the fence a 2D call returns is the next GFX fence, emitted
 * lazily (a "join": a WAIT_REG_MEM on SDMA's fence, then the GFX fence)
 * when someone waits for it or submits after it.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_ACCEL_H
#define FIREBRAND_ACCEL_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/pci/IOAGPDevice.h>

#include "FirebrandCore.h"
#include "FirebrandCompat.h"

#include <IOKit/graphics/IOAccelSurfaceConnect.h>

extern "C" {
#include "frb_ring.h"
#include "frb_bo.h"
#include "frb_rect.h"
#include "rdn_user.h"
#include "rdn_video.h"
}

/*
 * A Firebrand selector beyond RadeonNI's set, for 'rdn2' clients run by an
 * administrator: SCREEN_* from this client go to a buffer of its own
 * instead of the screen (offset hi, lo, pitch in pixels, width, height;
 * width 0: the screen again). The tests use it to check the blits off
 * screen, on a machine whose desktop is up.
 */
#define FRB_UC_SCREEN_TARGET	0x3f

/*
 * What Tiger's and Leopard's window server want before they composite with
 * OpenGL (RadeonNI's docs/QUARTZ-EXTREME.md, gates 6 and 7): a
 * registered IOAGPDevice with a "model", and an IOAGPDevice among the
 * accelerator's ancestors. A PCI Express card has neither; RadeonNIAGPShim,
 * renamed. Off unless the personality's AGPShim asks (as RadeonNI's).
 */
class FirebrandAGPShim : public IOAGPDevice
{
	OSDeclareDefaultStructors(FirebrandAGPShim)

public:
	virtual bool matchPropertyTable(OSDictionary *table, SInt32 *score);
	virtual bool matchPropertyTable(OSDictionary *table);
	virtual bool compareName(OSString *name, OSString **matched = 0) const;
	virtual IOReturn getResources(void);
};

class FirebrandAccelClient;
class FirebrandSurfaceClient;
class FirebrandVideoClient;
class FirebrandVideoMemory;
struct FirebrandVideoSession;

class FirebrandAccel : public FRB_ACCEL_BASE
{
	OSDeclareDefaultStructors(FirebrandAccel)

public:
	/*
	 * Create, attach under `provider` (or the AGP ancestor below it) and
	 * register. owner: the service whose personality holds the switches,
	 * retained while this lives (its FirebrandCore is `core`); fb: the
	 * framebuffer to point at this accelerator, or 0 (headless); VRAM below
	 * scanoutEnd is the display's. 0 if the engines are not up.
	 */
	static FirebrandAccel *withOwner(IOService *owner, FirebrandCore *core, IOFramebuffer *fb,
					 IOPCIDevice *provider, UInt32 scanoutEnd);
	/* Before the owner stops its core: no new work, the ring drains, the framebuffer's keys go. */
	void prepareToRetire(void);
	/* After the core has stopped (engines halted, bus mastering off): clients, memory, registry. */
	void retire(void);
	virtual void free(void);
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
				       IOUserClient **handler);

	/* For FirebrandAccelClient. Each takes the core's lock. */
	IOReturn getInfo(struct rdn_user_info *info);
	IOReturn gfxInfo(struct rdn_user_gfx_info *info);
	IOReturn vramInfo64(struct rdn_user_vram_info64 *info);
	IOReturn hiddenInfo(UInt32 *offset, UInt32 *size);
	IOReturn allocVram(struct frb_bo_table *bos, UInt64 size, UInt32 align, UInt32 where, UInt64 *offset);
	IOReturn freeVram(struct frb_bo_table *bos, UInt64 offset);
	IOReturn submit(struct frb_bo_table *bos, UInt64 offset, UInt32 words, UInt32 *fence);
	IOReturn fenceWait(UInt32 fence, UInt32 timeoutMs, UInt32 *reached);
	IOReturn syncForCPU(void);
	IOReturn regRead(UInt32 byteOffset, bool admin, UInt32 *value);
	IOReturn regWrite(UInt32 byteOffset, UInt32 value);
	IOReturn gartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages);
	/* owner: the client (FirebrandAccelClient or FirebrandSurfaceClient) the binding belongs to */
	IOReturn gartBind(void *owner, task_t task, UInt64 address, UInt32 size, UInt32 *offset);
	IOReturn gartUnbind(void *owner, UInt32 offset);
	/* a client's end: its buffers, bindings and surfaces go once the GPU is done with them */
	void releaseClient(void *owner, struct frb_bo_table *bos);
	IOMemoryDescriptor *apertureMemory(void) { return fAperture; }
	UInt32 apertureBytes(void) { return fApertureBytes; }
	IOMemoryDescriptor *statusMemory(void) { return fStatusMem; }
	struct frb_os *os(void) { return fCore ? fCore->adev()->os : 0; }

	/*
	 * G2: the 2D work, on SDMA0 (frb_rect.c). target: a client's own
	 * buffer (FRB_UC_SCREEN_TARGET, checked against bos), or 0 for the
	 * screen. Queued, not waited for; *fence is the GFX fence to wait for.
	 */
	IOReturn screenTarget(struct frb_bo_table *bos, UInt64 offset, UInt32 pitch, UInt32 width, UInt32 height,
			      struct frb_rect_surface *out);
	IOReturn screenFill(struct frb_bo_table *bos, const struct frb_rect_surface *target, int32_t x, int32_t y,
			    int32_t w, int32_t h, UInt32 colour, UInt32 *fence);
	IOReturn screenCopy(struct frb_bo_table *bos, const struct frb_rect_surface *target, int32_t sx, int32_t sy,
			    int32_t dx, int32_t dy, int32_t w, int32_t h, UInt32 *fence);
	IOReturn screenCopyRegion(struct frb_bo_table *bos, const struct frb_rect_surface *target,
				  const struct rdn_user_region_move *move, UInt32 count, UInt32 *fence);

	/* G2: the window server's surfaces, by ID (RadeonNIAccel's registry) */
	bool surfacesOffered(void) { return fSurfaces; }
	bool surfaceGate(void) { return fSurfaceGate; }
	bool screenSurface(void) { return fScreenSurface; }
	bool backingSurfaces(void) { return fBackingSurfaces; }
	bool setSurfaceRegion(UInt32 wid, const IOAccelDeviceRegion *rgn, UInt32 rects);
	bool getSurfaceRegion(UInt32 wid, struct rdn_user_region *region);
	void forgetSurface(UInt32 wid);
	/* RDN_UC_SURFACE_BUFFER: inside one of the owner's visible buffers (bos), or width 0 */
	IOReturn setSurfaceBuffer(struct frb_bo_table *bos, task_t task, void *owner, UInt32 wid, UInt32 offset,
				  UInt32 rowBytes, UInt32 width, UInt32 height);
	bool getSurfaceBuffer(UInt32 wid, UInt32 *offset, UInt32 *rowBytes, UInt32 *width, UInt32 *height);
	IOReturn initSurfaceBuffer(struct frb_bo_table *bos, task_t task, void *owner,
				   const struct rdn_user_surface_init *in, UInt32 *synced);
	bool isGLSurface(UInt32 wid);
	bool markSurfaceGL(UInt32 wid);
	bool isGLProducer(UInt32 wid);
	bool isBackingOwner(UInt32 wid, task_t task);
	bool setSurfaceBacking(UInt32 wid, task_t task, const struct rdn_user_backing *backing);
	void setSurfaceBackingGart(UInt32 wid, task_t task, UInt64 gartAddr);
	bool getSurfaceBacking(UInt32 wid, task_t task, struct rdn_user_backing *backing);
	void setSurfaceBackingClient(UInt32 wid, FirebrandSurfaceClient *client, FirebrandSurfaceClient *only);
	IOReturn acquireSurfaceForGPU(UInt32 wid, UInt32 *copied);
	void listSurfaces(struct rdn_user_surfaces *list);
	void querySurface(UInt32 wid, struct rdn_user_surface_info *info);
	/* a surface's picture to the screen, inside its shape: queued */
	IOReturn flushSurface(UInt32 wid);
	void setReadLocked(UInt32 wid) { fReadLocked = wid; }
	UInt32 readLocked(void) { return fReadLocked; }
	/* everything the GPU has been given so far is done (GFX and 2D) */
	IOReturn waitForCPU(void);
	/*
	 * RDN_UC_CP_RECOVER (administrators): 0 the busy blocks, 1 and 2 the
	 * GFX block regardless (RadeonNI: the CP, then the graphics block too;
	 * gfx_v8_0's soft reset takes both), 3 GFX and SDMA. *result: errno-style.
	 */
	IOReturn cpRecover(UInt32 level, SInt32 *result);
	/* one rectangle copied (GPU addresses; pitches in pixels), waited for; for the backing and SURFACE_INIT */
	IOReturn blitLinear(UInt64 dst, UInt32 dstPitchPx, UInt64 src, UInt32 srcPitchPx, UInt32 width, UInt32 height);
	IOReturn fillLinear(UInt64 dst, UInt32 dstPitchPx, UInt32 width, UInt32 height, UInt32 colour);
	UInt64 vramAddr(UInt64 offset);
	UInt64 gartAddr(UInt32 gartOffset);
	/* BAR0 in the kernel (the CPU's way when the engine cannot), [0, apertureBytes()) */
	volatile UInt8 *vramKernel(void) { return fCore ? fCore->adev()->aper : 0; }
	/* {ScreenSurface: bool}, read at the window server's next display surface (RadeonNIAccel) */
	virtual IOReturn setProperties(OSObject *properties);
	virtual bool serializeProperties(OSSerialize *s) const;

	/* 'rdnv', the video client (FirebrandVideo.cpp): RadeonNIAccel's video* calls */
	void videoQuery(bool administrator, rdn_video_query *out);
	IOReturn videoCreate(FirebrandVideoClient *owner, bool administrator, const rdn_video_create *in,
			     rdn_video_session *out);
	IOReturn videoDecode(FirebrandVideoClient *owner, const rdn_video_decode *in, rdn_video_output *out);
	IOReturn videoRelease(FirebrandVideoClient *owner, const rdn_video_release *in);
	IOReturn videoDestroy(FirebrandVideoClient *owner, const rdn_video_destroy *in);
	void videoClose(FirebrandVideoClient *owner);
	IOReturn videoMemory(FirebrandVideoClient *owner, UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);
	/* the 20 MB of client-mapped video memory, RadeonNI's quota */
	bool videoCharge(UInt32 bytes);
	void videoUncharge(UInt32 bytes);

private:
	enum {
		kMaxInFlight = 6,	/* GFX ring: 2048 dwords, 256 a submission; two spare */
		kMaxBindings = 1024,
		kPendingVram = 1,
		kPendingGart = 2,
		/* the 2D IBs: GTT, one slot a submission, reused once SDMA0 has passed its fence */
		kIbSlots = 16,
		kIbSlotBytes = 64 << 10
	};
	struct Pending {
		Pending *next;
		UInt32 kind, seq;
		UInt32 sdmaSeq;			/* SDMA0's sequence when it was freed (2D work may read it) */
		UInt64 offset, size;		/* VRAM */
		UInt32 first, npages;		/* GART pages */
		IOMemoryDescriptor *memory;	/* the wired pages behind them */
	};
	struct Binding {
		IOMemoryDescriptor *memory;	/* 0: the slot is free */
		UInt32 first, npages;
		void *owner;
	};
	/* RadeonNIAccel::SurfaceRecord */
	struct SurfaceRecord {
		UInt32 wid;
		bool used;
		bool gl;			/* announced by its owner (RDN_UC_SURFACE_BUFFER) */
		bool glProducer;		/* rendered into by an OpenGL producer */
		struct rdn_user_region region;
		UInt32 bufOffset, bufRowBytes, bufWidth, bufHeight;	/* the picture, if its owner said */
		void *owner;
		task_t backingTask;
		struct rdn_user_backing backing;
		UInt64 backingGartAddr;
		/* the window server's client holding the backing; not retained (acquireSurfaceForGPU) */
		FirebrandSurfaceClient *backingClient;
	};

	bool setUp(IOService *owner, FirebrandCore *core, IOFramebuffer *fb, IOPCIDevice *provider,
		   UInt32 scanoutEnd);
	void publish(IOPCIDevice *provider);
	struct frb_ring *ringLocked(void);
	static UInt32 frbRingFrames(const struct frb_ring *ring);
	UInt32 completedLocked(void);
	bool signalledLocked(UInt32 seq);
	void updateStatusLocked(void);
	void deferLocked(UInt32 kind, UInt64 offset, UInt64 size, UInt32 first, UInt32 npages,
			 IOMemoryDescriptor *memory);
	void reapLocked(bool all);
	bool waitSeq(UInt32 seq, UInt32 timeoutMs, bool draining);
	void reportHang(const char *what, UInt32 seq);
	/* the lockup watchdog (amdgpu's job timeout, per ring) and the recovery it starts */
	static void watchdogFired(OSObject *owner, IOTimerEventSource *sender);
	void watchdog(void);
	void startWatchdog(void);
	void stopWatchdog(void);
	int recoverLocked(UInt32 force, UInt32 stalled, const char *why);
	/* G2 */
	struct frb_ring *sdmaLocked(void);
	bool sdmaSignalledLocked(UInt32 seq);
	bool waitSdma(UInt32 seq, UInt32 timeoutMs, bool draining);
	void softwareCursorSync(const struct frb_rect_surface *target);
	IOReturn gfxRoomLocked(void);
	IOReturn flushJoinLocked(void);
	UInt32 currentFenceLocked(void);
	IOReturn slotLocked(UInt32 count, UInt32 *slots, struct frb_rect_ib *ibs);
	void gfxWrapLocked(void);
	IOReturn sdmaSubmitLocked(UInt32 slot, struct frb_rect_ib *ib, UInt32 *seq);
	bool screenLocked(struct frb_bo_table *bos, const struct frb_rect_surface *target, struct frb_rect_surface *s);
	IOReturn scratchLocked(const struct frb_rect_surface *s, struct frb_rect_surface *scratch);
	IOReturn movesLocked(struct frb_bo_table *bos, const struct frb_rect_surface *target,
			     const struct frb_rect_move *m, UInt32 n);
	int surfaceSlotLocked(UInt32 wid, bool create);
	/* video (FirebrandVideo.cpp); fVideoLock before the core's lock, never the other way */
	IOReturn newVideoClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler);
	bool videoReady(bool administrator, bool validation);
	void videoPoison(int error, const char *why);
	int videoBindLocked(FirebrandVideoMemory *memory, UInt32 *first, UInt32 *pages);
	int videoCommand(UInt32 type, const rdn_uvd_h264_picture *picture, UInt32 bitstreamBytes);
	void videoCleanup(bool destroy, bool hardware);
	void videoRetire(void);
	void videoReleaseAll(void);
	void dropSurfaceBuffersLocked(void *owner, UInt64 offset, UInt64 size);
	bool bufferOwned(struct frb_bo_table *bos, UInt64 offset, UInt64 bytes);

	IOService *fOwner;
	FirebrandCore *fCore;
	IOFramebuffer *fFramebuffer;
	IOPCIDevice *fProvider;
	IOAGPDevice *fAncestor;
	FirebrandAGPShim *fShim;
	bool fRetired;
	bool fReleased;			/* retire() has run */
	struct frb_vram_mgr fVram;
	UInt32 fScanoutEnd;
	IODeviceMemory *fAperture;	/* BAR0 [0, the driver's region) */
	UInt32 fApertureBytes;
	IOBufferMemoryDescriptor *fStatusMem;
	volatile struct rdn_user_status *fStatus;
	UInt32 fCompleted;		/* the newest sequence seen signalled */
	UInt32 fHangReports;
	IOWorkLoop *fWatchLoop;
	IOTimerEventSource *fWatchdog;
	UInt32 fWatchSeen[2], fWatchStallMs[2];	/* GFX, SDMA0: the fence last seen, how long it stood */
	UInt32 fRecoveries;
	bool fWedged;			/* a recovery failed: only a reload resets the ASIC */
	/*
	 * FirebrandAccelHoldMs / frb_accel_hold: the first submission (2D or a
	 * client's IB) and every one that comes meanwhile waits until this long
	 * after the first asked (0: no hold). An experiment's switch: it moves
	 * the engines' first work away from the rest of the boot.
	 */
	UInt32 fHoldMs;
	UInt64 fHoldUntilNs;		/* 0 until the first submission asks */
	bool fHoldDone;
	void holdOffLocked(void);
	/*
	 * SOC15: before the first submission SDMA0 runs one empty 2D frame of
	 * the driver's own, and is reset if that frame does not come back
	 * (sdmaProbeLocked, docs/vega20-bring-up.md "SDMA0's first frame").
	 */
	bool fProbeDone;
	void sdmaProbeLocked(void);
	Pending *fPending;
	Binding *fBindings;

	/* G2: SDMA0's 2D work and the join into the GFX timeline */
	struct frb_gtt fIbArena;
	bool fIbArenaUp;
	UInt32 fSlotSeq[kIbSlots];
	bool fSlotUsed[kIbSlots];
	UInt32 fSlotNext;
	UInt32 fSdmaCompleted;
	UInt32 fSdmaMaxInFlight;	/* 2D frames SDMA0's ring holds (frbRingFrames - 2, at most kIbSlots) */
	bool fJoinPending;		/* the GFX fence fJoinSeq is promised to cover SDMA0 up to fJoinSdma */
	UInt32 fJoinSeq, fJoinSdma;
	UInt32 fJoinPromised, fJoinEmitted;	/* the last join: promised and emitted numbers */
	UInt32 fClientGfxSeq;		/* the newest client GFX submission (2D work waits for it) */
	UInt64 fScratchOffset, fScratchBytes;	/* region moves' scratch surface (VRAM) */
	UInt32 fScratchPitch;
	UInt64 fFills, fCopies, fMoves, fScratchMoves, fFlushes, fJoins;
	UInt64 fSoftCursorSyncs;	/* 2D calls waited for because the cursor was the CPU's */
	/* G2: surfaces */
	bool fSurfaces, fSurfaceGate, fScreenSurface, fBackingSurfaces;
	SurfaceRecord *fShapes;
	UInt32 fShapeCapacity;
	UInt32 fReadLocked;
	/* video: one session in the system */
	IOLock *fVideoLock;
	FirebrandVideoSession *fVideoSession;
	UInt64 fVideoEpoch;
	volatile UInt32 fVideoCpuBytes;
	bool fVideoOffered;		/* FirebrandVideo (default on) / frb_video */
	bool fVideoPoisoned;		/* a decode timed out: until a reload */
	bool fVideoHalted;		/* retire(): the engines are halted, BAR0 gone */
};

class FirebrandAccelClient : public IOUserClient
{
	OSDeclareDefaultStructors(FirebrandAccelClient)

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
	virtual bool start(IOService *provider);
	virtual IOReturn clientClose(void);
	virtual IOReturn clientDied(void);
	virtual void stop(IOService *provider);
	virtual void free(void);
	virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);

	/* hw/rdn_user.h, in its order */
	IOReturn methodGetInfo(struct rdn_user_info *info, IOByteCount *size);
	IOReturn methodGfxInfo(struct rdn_user_gfx_info *info, IOByteCount *size);
	IOReturn methodAlloc(UInt32 size, UInt32 align, UInt32 *offset);
	IOReturn methodFree(UInt32 offset);
	IOReturn methodSubmit(UInt32 offset, UInt32 words, UInt32 *fence);
	IOReturn methodFenceWait(UInt32 fence, UInt32 timeoutMs, UInt32 *reached);
	IOReturn methodSyncForCPU(void);
	IOReturn methodRegRead(UInt32 offset, UInt32 *value);
	IOReturn methodAllocHidden(UInt32 size, UInt32 align, UInt32 *offset);
	IOReturn methodHiddenInfo(UInt32 *offset, UInt32 *size);
	IOReturn methodGartInfo(UInt32 *on, UInt32 *gpuStart, UInt32 *pages);
	IOReturn methodGartBind(UInt32 address, UInt32 size, UInt32 *offset);
	IOReturn methodGartUnbind(UInt32 offset);
	IOReturn methodGartBind64(UInt32 low, UInt32 high, UInt32 size, UInt32 *offset);
	IOReturn methodRegWrite(UInt32 offset, UInt32 value);
	IOReturn methodCpRecover(UInt32 level, SInt32 *result);
	/* G2 */
	IOReturn methodSurfaceRegion(UInt32 wid, struct rdn_user_region *region, IOByteCount *size);
	IOReturn methodSurfaceBuffer(UInt32 wid, UInt32 offset, UInt32 rowBytes, UInt32 width, UInt32 height);
	IOReturn methodSurfaceList(struct rdn_user_surfaces *list, IOByteCount *size);
	IOReturn methodSurfaceLocked(UInt32 *wid);
	IOReturn methodScreenFill(UInt32 x, UInt32 y, UInt32 width, UInt32 height, UInt32 colour, UInt32 *fence);
	IOReturn methodScreenCopy(UInt32 sx, UInt32 sy, UInt32 dx, UInt32 dy, UInt32 size, UInt32 *fence);
	IOReturn methodSurfaceBacking(UInt32 wid, struct rdn_user_backing *backing, IOByteCount *size);
	IOReturn methodSurfaceQuery(UInt32 wid, struct rdn_user_surface_info *info, IOByteCount *size);
	IOReturn methodScreenCopyRegion(const void *in, struct rdn_user_fence *out, IOByteCount inSize,
					IOByteCount *outSize);
	IOReturn methodSurfaceGL(UInt32 wid);
	IOReturn methodSurfaceInit(const struct rdn_user_surface_init *in, struct rdn_user_surface_init_out *out,
				   IOByteCount inSize, IOByteCount *outSize);
	IOReturn methodSurfaceAcquire(UInt32 wid, UInt32 *copied);
	IOReturn methodScreenTarget(UInt32 offHi, UInt32 offLo, UInt32 pitch, UInt32 width, UInt32 height);
	IOReturn methodVramInfo64(struct rdn_user_vram_info64 *info, IOByteCount *size);
	IOReturn methodAlloc64(UInt32 sizeHi, UInt32 sizeLo, UInt32 align, UInt32 flags, UInt32 *offHi,
			       UInt32 *offLo);
	IOReturn methodFree64(UInt32 offHi, UInt32 offLo);
	IOReturn methodSubmit64(UInt32 offHi, UInt32 offLo, UInt32 words, UInt32 *fence);
	/* what Firebrand does not have yet: power (G3) */
	IOReturn methodUnsupported(void);
	/* UVD: RadeonNI's answer without FW_UVD, which its user space handles */
	IOReturn methodNotReady(void);
	/* the administrator's diagnostics */
	IOReturn methodAdminOnly(void);

private:
	void releaseAll(void);

	FirebrandAccel *fAccel;
	task_t fTask;
	bool fAdmin;
	bool fV2;
	bool fReleased;
	struct frb_bo_table fBos;
	/* FRB_UC_SCREEN_TARGET: SCREEN_* go here instead of the screen */
	bool fHasTarget;
	struct frb_rect_surface fTarget;
};

#endif
