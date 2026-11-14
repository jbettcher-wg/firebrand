/*
 * FirebrandSurfaceClient: the accelerator's surface user client, the public
 * IOAccelSurface interface (IOAccelSurfaceConnect.h) that the window server
 * opens with IOAccelCreateSurface (type 0, kIOAccelSurfaceClientType).
 * RadeonNI's RadeonNISurfaceClient (RadeonNI's kext/RadeonNI/
 * RadeonNISurface.{h,cpp}), ported onto FirebrandAccel: the same methods,
 * gate, read locks, backing mode and Quartz GL ownership; the copies run on
 * SDMA0 (FirebrandAccel::blitLinear) and, where the engine cannot, on the
 * CPU through the kernel's BAR0 mapping a word at a time.
 *
 * Copyright (c) 2026 kouta-kun and Claude (RadeonNISurface)
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_SURFACE_H
#define FIREBRAND_SURFACE_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOAccelSurfaceConnect.h>

class FirebrandAccel;

class FirebrandSurfaceClient : public IOUserClient
{
	OSDeclareDefaultStructors(FirebrandSurfaceClient)

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
	virtual bool start(IOService *provider);
	virtual IOReturn clientClose(void);
	virtual IOReturn clientDied(void);
	virtual void free(void);
	virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
	/* the count properties brought up to date for whoever reads them */
	virtual bool serializeProperties(OSSerialize *s) const;

	/* the public methods, in the order of eIOAccelSurfaceMethods */
	IOReturn readLockOptions(UInt32 options, IOAccelSurfaceInformation *info, IOByteCount *size);
	IOReturn readUnlockOptions(UInt32 options);
	IOReturn getState(UInt32 *state);
	IOReturn writeLockOptions(UInt32 options, IOAccelSurfaceInformation *info, IOByteCount *size);
	IOReturn writeUnlockOptions(UInt32 options);
	IOReturn read(IOAccelSurfaceReadData *data, IOByteCount inSize);
	IOReturn setShapeBacking(UInt32 options, UInt32 fbIndex, UInt32 backing, UInt32 rowBytes,
				 IOAccelDeviceRegion *rgn, IOByteCount size);
	IOReturn setIDMode(UInt32 wid, UInt32 mode);
	IOReturn setScale(UInt32 options, IOAccelSurfaceScaling *scaling, IOByteCount size);
	IOReturn setShape(UInt32 options, UInt32 fbIndex, IOAccelDeviceRegion *rgn, IOByteCount size);
	IOReturn flush(UInt32 fbMask, UInt32 options);
	IOReturn queryLock(void);
	IOReturn readLock(IOAccelSurfaceInformation *info, IOByteCount *size);
	IOReturn readUnlock(void);
	IOReturn writeLock(IOAccelSurfaceInformation *info, IOByteCount *size);
	IOReturn writeUnlock(void);
	IOReturn control(UInt32 selector, UInt32 arg, UInt32 *result);
	IOReturn setShapeBackingAndLength(UInt32 options, UInt32 fbIndex, UInt32 backing, UInt32 rowBytes,
					  UInt32 length, IOAccelDeviceRegion *rgn, IOByteCount size);

	/* a program's GL takes the surface (FirebrandAccel::acquireSurfaceForGPU) */
	IOReturn acquireForGPU(bool *copied);

private:
	FirebrandAccel *fAccel;
	UInt32 fWid, fMode;
	/* whose client this is, and its view of the surface while read-locked */
	task_t fTask;
	bool fSetShape;
	/* setIDMode refused it (the surface gate): not ours */
	bool fRefused;
	IOMemoryMap *fReadMap;
	UInt32 fReadMapStart, fReadMapLength;
	bool fBackingMode;
	bool fBackingStale;
	/*
	 * The last transfer between the backing and the GPU store went from the
	 * store to the backing, and nothing has been sent up since (RadeonNI's
	 * writeLockOptions: a write lock does not read back over it).
	 */
	bool fBackingFromStore;
	/* Quartz GL ownership (RadeonNISurface.h): fCPUOwned while the window server has handed the backing to the CPU */
	bool fGPUAcquires;
	bool fCPUOwned;
	UInt32 fTransfers;
	IOLock *fBackingLock;
	IOMemoryDescriptor *fBackingMemory;
	IOMemoryMap *fBackingMap;
	UInt32 fBackingAddress, fBackingStride, fBackingWidth, fBackingHeight;
	/* the backing bound into the GART so SDMA0 moves it; size 0: the CPU copies */
	UInt32 fBackingGart, fBackingGartSize, fBackingGartSub;
	IOReturn configureBacking(UInt32 options, UInt32 fbIndex, UInt32 address, UInt32 stride, UInt32 length,
				  IOAccelDeviceRegion *region, IOByteCount size);
	IOReturn lockBacking(IOAccelSurfaceInformation *info, IOByteCount *size, bool syncToCPU = true,
			     bool keepCPUEdits = false);
	IOReturn syncBacking(bool toCPU);
	void releaseBacking(void);

	IOReturn lockForRead(IOAccelSurfaceInformation *info, IOByteCount *size, bool syncToCPU = true);
	void unlockRead(void);
	void dropReadMap(void);
	UInt32 fCalls;

	/* method: one of eIOAccelSurfaceMethods */
	void note(UInt32 method, UInt32 a, UInt32 b, UInt32 c, UInt32 d);
	void noteRegion(UInt32 method, IOAccelDeviceRegion *rgn, IOByteCount size);

	/*
	 * What is asked of this client, counted for its whole life (RadeonNI's
	 * counts): every method, and for control and flush each distinct set of
	 * arguments. Properties FirebrandSurfaceCalls, FirebrandSurfaceControl
	 * and FirebrandSurfaceFlush (ioreg -c FirebrandSurfaceClient -l -w0),
	 * and the log when the client closes.
	 */
	enum { kMaxNotes = 0, kControlSelectors = 8, kControlArgs = 4, kFlushKinds = 8 };
	UInt32 fMethodCalls[kIOAccelNumSurfaceMethods];
	UInt32 fPrivateAsked;
	struct {
		UInt32 selector, calls, argCount, otherArgs;
		struct {
			UInt32 arg, calls;
		} args[kControlArgs];
	} fControl[kControlSelectors];
	UInt32 fControlCount, fControlOther;
	struct {
		UInt32 mask, options, calls;
	} fFlush[kFlushKinds];
	UInt32 fFlushCount, fFlushOther;
	UInt32 fPublished;

	void countControl(UInt32 selector, UInt32 arg);
	void countFlush(UInt32 mask, UInt32 options);
	void publishCounts(bool log);
};

#endif
