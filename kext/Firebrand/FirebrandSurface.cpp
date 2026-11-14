/*
 * The accelerator's surface user client: see FirebrandSurface.h. The body
 * is RadeonNISurface.cpp's, method for method, with these changes:
 *   - the copies between the backing and the GPU store run on SDMA0
 *     (FirebrandAccel::blitLinear, waited for); RadeonNI's 3D engine or CP
 *     DMA did them. The CPU's fallback reads and writes VRAM through the
 *     kernel's BAR0 mapping a word at a time (no bcopy on uncached memory);
 *   - GART bindings go through FirebrandAccel::gartBind with this client as
 *     their owner;
 *   - the surface profiler (RadeonNISurfaceProfile) is not ported.
 *
 * Copyright (c) 2026 kouta-kun and Claude (RadeonNISurface.cpp)
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <stddef.h>

#include "FirebrandAccel.h"
#include "FirebrandSurface.h"

#define super IOUserClient
OSDefineMetaClassAndStructors(FirebrandSurfaceClient, IOUserClient)

/* Enough of the log to see the pattern, not every frame for ever. */
#define MAX_NOTES FirebrandSurfaceClient::kMaxNotes

/* The counts as text: room for every table full. */
#define COUNTS_TEXT 2048

/* In the order of eIOAccelSurfaceMethods. */
static const char *const methodNames[kIOAccelNumSurfaceMethods] = {
	"readLockOptions", "readUnlockOptions", "getState", "writeLockOptions",
	"writeUnlockOptions", "read", "setShapeBacking", "setIDMode", "setScale",
	"setShape", "flush", "queryLock", "readLock", "readUnlock", "writeLock",
	"writeUnlock", "control", "setShapeBackingAndLength",
};

/* Add to the text at *len, never beyond size; what does not fit is left out. */
static void append(char *text, UInt32 size, UInt32 *len, const char *fmt, ...)
{
	va_list ap;

	if (*len + 1 >= size)
		return;
	va_start(ap, fmt);
	vsnprintf(text + *len, size - *len, fmt, ap);
	va_end(ap);
	while (text[*len])
		(*len)++;
}

bool FirebrandSurfaceClient::initWithTask(task_t owningTask, void *securityID,
					 UInt32 type)
{
	if (!super::initWithTask(owningTask, securityID, type))
		return false;
	fTask = owningTask;
	fBackingGartSub = 0;
	fBackingFromStore = false;
	fGPUAcquires = fCPUOwned = false;
	fTransfers = 0;
	fBackingLock = IOLockAlloc();
	return fBackingLock != NULL;
}

/*
 * Reading a surface: its owner has told the accelerator where the picture
 * is in video memory (RDN_UC_SURFACE_BUFFER); that part of the aperture is
 * mapped, read-only, into the task that asks. The window server does this
 * when it has to draw a program's OpenGL window itself.
 */

/*
 * VRAM through the kernel's BAR0 mapping (cache-inhibited): a word at a
 * time, never bcopy, whose vector or dcbz paths need cacheable memory.
 * Offsets and lengths are multiples of 4 (surfaces are 32 bits a pixel at
 * 4-byte offsets); the other side is ordinary memory, any alignment.
 */
static void fromVram(UInt8 *dst, const volatile UInt8 *src, UInt32 bytes)
{
	const volatile UInt32 *s = (const volatile UInt32 *)src;
	UInt32 i, v;

	for (i = 0; i < bytes / 4; i++) {
		v = s[i];
		bcopy(&v, dst + 4 * i, 4);
	}
}

static void toVram(volatile UInt8 *dst, const UInt8 *src, UInt32 bytes)
{
	volatile UInt32 *d = (volatile UInt32 *)dst;
	UInt32 i, v;

	for (i = 0; i < bytes / 4; i++) {
		bcopy(src + 4 * i, &v, 4);
		d[i] = v;
	}
}

IOReturn FirebrandSurfaceClient::lockForRead(IOAccelSurfaceInformation *info,
					    IOByteCount *size, bool syncToCPU)
{
	UInt32 offset, rowBytes, width, height, start, length;
	uint64_t end;
	IOReturn ret;
	IOMemoryDescriptor *aperture;

	if (!info || !size || *size < sizeof(*info))
		return kIOReturnBadArgument;
	if (!fAccel->getSurfaceBuffer(fWid, &offset, &rowBytes, &width, &height))
		return kIOReturnUnsupported;
	aperture = fAccel->apertureMemory();
	if (!aperture)
		return kIOReturnNotReady;
	end = (uint64_t)offset + (uint64_t)rowBytes * height;
	if (!width || !height || (uint64_t)width * 4 > rowBytes ||
	    end > aperture->getLength() || end > 0xffffffffULL - PAGE_SIZE + 1)
		return kIOReturnBadArgument;
	/*
	 * WindowServer passes kIOAccelSurfaceLockInAccel (syncToCPU == false)
	 * when compositing on the GPU; both producer and compositor submit to
	 * the same FIFO GFX ring, so only true CPU reads need waitForCPU().
	 */
	if (syncToCPU) {
		ret = fAccel->waitForCPU();
		if (ret != kIOReturnSuccess)
			return ret;
	}
	start = offset & ~(UInt32)(PAGE_SIZE - 1);
	length = ((UInt32)(end + PAGE_SIZE - 1) & ~(UInt32)(PAGE_SIZE - 1)) - start;
	if ((uint64_t)start + length > aperture->getLength())
		return kIOReturnBadArgument;
	if (!fReadMap || fReadMapStart != start || fReadMapLength != length) {
		dropReadMap();
		fReadMap = aperture->map(fTask, 0, kIOMapAnywhere | kIOMapReadOnly,
					 start, length);
		if (!fReadMap)
			return kIOReturnNoMemory;
		fReadMapStart = start;
		fReadMapLength = length;
	}
	bzero(info, sizeof(*info));
	info->address[0] = fReadMap->getVirtualAddress() + (offset - start);
	info->rowBytes = rowBytes;
	info->width = width;
	info->height = height;
	/*
	 * The window server reads this as a surface colour depth
	 * (kIOAccelSurfaceModeColorDepth8888); anything it does not know
	 * makes it give the surface up and draw white.
	 */
	info->pixelFormat = kIOAccelSurfaceModeColorDepth8888;
	fAccel->setReadLocked(fWid);
	*size = sizeof(*info);
	return kIOReturnSuccess;
}

void FirebrandSurfaceClient::unlockRead(void)
{
	if (fAccel && fAccel->readLocked() == fWid)
		fAccel->setReadLocked(0);
}

void FirebrandSurfaceClient::dropReadMap(void)
{
	if (fReadMap) {
		fReadMap->release();
		fReadMap = 0;
		fReadMapStart = 0;
		fReadMapLength = 0;
	}
}

bool FirebrandSurfaceClient::start(IOService *provider)
{
	fAccel = OSDynamicCast(FirebrandAccel, provider);
	if (!fAccel || !super::start(provider))
		return false;
	if (kMaxNotes > 0)
		IOLog("FirebrandSurface: surface client %p opened\n", this);
	return true;
}

IOReturn FirebrandSurfaceClient::clientClose(void)
{
	if (kMaxNotes > 0)
		IOLog("FirebrandSurface: surface client %p (window %lu) closed after %lu calls\n",
		      this, (unsigned long)fWid, (unsigned long)fCalls);
	publishCounts(kMaxNotes > 0);
	unlockRead();
	dropReadMap();
	IOLockLock(fBackingLock);
	releaseBacking();
	IOLockUnlock(fBackingLock);
	/*
	 * The window server's and the program's clients share the surface
	 * by its ID; only when the window server lets go is it forgotten.
	 */
	if (fAccel && fWid && (fSetShape || fBackingMode))
		fAccel->forgetSurface(fWid);
	terminate();
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::clientDied(void)
{
	return clientClose();
}

void FirebrandSurfaceClient::note(UInt32 method, UInt32 a, UInt32 b, UInt32 c,
				 UInt32 d)
{
	UInt32 n;

	fMethodCalls[method]++;
	n = ++fCalls;
	if (MAX_NOTES > 0 && n <= (UInt32)MAX_NOTES)
		IOLog("FirebrandSurface: surface %lu: %s(0x%lx, 0x%lx, 0x%lx, 0x%lx)\n",
		      (unsigned long)fWid, methodNames[method], (unsigned long)a,
		      (unsigned long)b, (unsigned long)c, (unsigned long)d);
	/*
	 * The properties are also written here, in case nothing reads them
	 * the way serializeProperties() sees: at every power of two calls up
	 * to 256, then every 256.
	 */
	if (n <= 256 ? !(n & (n - 1)) : !(n & 4095))
		publishCounts(false);
}

void FirebrandSurfaceClient::noteRegion(UInt32 method, IOAccelDeviceRegion *rgn,
				       IOByteCount size)
{
	if (MAX_NOTES > 0 && fCalls < (UInt32)MAX_NOTES && rgn &&
	    size >= sizeof(IOAccelDeviceRegion))
		IOLog("FirebrandSurface: surface %lu:   %s region: %lu rects, bounds %d,%d %dx%d\n",
		      (unsigned long)fWid, methodNames[method],
		      (unsigned long)rgn->num_rects,
		      rgn->bounds.x, rgn->bounds.y, rgn->bounds.w, rgn->bounds.h);
}

/*
 * The tables only grow, and a count is one word: two threads of a client
 * in here at once can lose a count or enter a value twice, nothing worse.
 */
void FirebrandSurfaceClient::countControl(UInt32 selector, UInt32 arg)
{
	UInt32 i, j, n = fControlCount;

	for (i = 0; i < n && fControl[i].selector != selector; i++)
		;
	if (i == n) {
		if (n >= kControlSelectors) {
			fControlOther++;
			return;
		}
		fControl[i].selector = selector;
		fControlCount = n + 1;
	}
	fControl[i].calls++;
	n = fControl[i].argCount;
	for (j = 0; j < n && fControl[i].args[j].arg != arg; j++)
		;
	if (j == n) {
		if (n >= kControlArgs) {
			fControl[i].otherArgs++;
			return;
		}
		fControl[i].args[j].arg = arg;
		fControl[i].argCount = n + 1;
	}
	fControl[i].args[j].calls++;
}

void FirebrandSurfaceClient::countFlush(UInt32 mask, UInt32 options)
{
	UInt32 i, n = fFlushCount;

	for (i = 0; i < n && (fFlush[i].mask != mask || fFlush[i].options != options); i++)
		;
	if (i == n) {
		if (n >= kFlushKinds) {
			fFlushOther++;
			return;
		}
		fFlush[i].mask = mask;
		fFlush[i].options = options;
		fFlushCount = n + 1;
	}
	fFlush[i].calls++;
}

/*
 * Write the counts as three strings, into the registry and, when the
 * client closes, into the log. Nothing is done when nothing was counted
 * since the last time.
 */
void FirebrandSurfaceClient::publishCounts(bool log)
{
	UInt32 all = fCalls + fPrivateAsked, len, i, j;
	char *text;

	if (!all || (!log && all == fPublished))
		return;
	text = (char *)IOMalloc(COUNTS_TEXT);
	if (!text)
		return;
	fPublished = all;

	len = 0;
	text[0] = 0;
	append(text, COUNTS_TEXT, &len, "window %lu: %lu in all",
	       (unsigned long)fWid, (unsigned long)fCalls);
	for (i = 0; i < kIOAccelNumSurfaceMethods; i++)
		if (fMethodCalls[i])
			append(text, COUNTS_TEXT, &len, ", %s %lu", methodNames[i],
			       (unsigned long)fMethodCalls[i]);
	if (fPrivateAsked)
		append(text, COUNTS_TEXT, &len, ", private methods asked for %lu",
		       (unsigned long)fPrivateAsked);
	setProperty("FirebrandSurfaceCalls", text);
	if (log)
		IOLog("FirebrandSurface: surface calls: %s\n", text);

	if (fControlCount) {
		len = 0;
		text[0] = 0;
		append(text, COUNTS_TEXT, &len, "window %lu:", (unsigned long)fWid);
		for (i = 0; i < fControlCount; i++) {
			append(text, COUNTS_TEXT, &len, "%s selector %lu (%lu):",
			       i ? ";" : "", (unsigned long)fControl[i].selector,
			       (unsigned long)fControl[i].calls);
			for (j = 0; j < fControl[i].argCount; j++)
				append(text, COUNTS_TEXT, &len, "%s 0x%lx %lu",
				       j ? "," : "",
				       (unsigned long)fControl[i].args[j].arg,
				       (unsigned long)fControl[i].args[j].calls);
			if (fControl[i].otherArgs)
				append(text, COUNTS_TEXT, &len, ", other arguments %lu",
				       (unsigned long)fControl[i].otherArgs);
		}
		if (fControlOther)
			append(text, COUNTS_TEXT, &len, "; other selectors %lu",
			       (unsigned long)fControlOther);
		setProperty("FirebrandSurfaceControl", text);
		if (log)
			IOLog("FirebrandSurface: surface control: %s\n", text);
	}

	if (fFlushCount) {
		len = 0;
		text[0] = 0;
		append(text, COUNTS_TEXT, &len, "window %lu:", (unsigned long)fWid);
		for (i = 0; i < fFlushCount; i++)
			append(text, COUNTS_TEXT, &len, "%s mask 0x%lx options 0x%lx: %lu",
			       i ? ";" : "", (unsigned long)fFlush[i].mask,
			       (unsigned long)fFlush[i].options,
			       (unsigned long)fFlush[i].calls);
		if (fFlushOther)
			append(text, COUNTS_TEXT, &len, "; other %lu",
			       (unsigned long)fFlushOther);
		setProperty("FirebrandSurfaceFlush", text);
		if (log)
			IOLog("FirebrandSurface: surface flush: %s\n", text);
	}
	IOFree(text, COUNTS_TEXT);
}

/*
 * ioreg and IORegistryEntryCreateCFProperties() come through here, so the
 * counts they show are the counts of that moment.
 */
bool FirebrandSurfaceClient::serializeProperties(OSSerialize *s) const
{
	((FirebrandSurfaceClient *)this)->publishCounts(false);
	return super::serializeProperties(s);
}

IOExternalMethod *FirebrandSurfaceClient::getTargetAndMethodForIndex(
	IOService **target, UInt32 index)
{
	static const IOExternalMethod methods[kIOAccelNumSurfaceMethods] = {
		/* kIOAccelSurfaceReadLockOptions */
		{ 0, (IOMethod)&FirebrandSurfaceClient::readLockOptions,
		  kIOUCScalarIStructO, 1, sizeof(IOAccelSurfaceInformation) },
		/* kIOAccelSurfaceReadUnlockOptions */
		{ 0, (IOMethod)&FirebrandSurfaceClient::readUnlockOptions,
		  kIOUCScalarIScalarO, 1, 0 },
		/* kIOAccelSurfaceGetState */
		{ 0, (IOMethod)&FirebrandSurfaceClient::getState,
		  kIOUCScalarIScalarO, 0, 1 },
		/* kIOAccelSurfaceWriteLockOptions */
		{ 0, (IOMethod)&FirebrandSurfaceClient::writeLockOptions,
		  kIOUCScalarIStructO, 1, sizeof(IOAccelSurfaceInformation) },
		/* kIOAccelSurfaceWriteUnlockOptions */
		{ 0, (IOMethod)&FirebrandSurfaceClient::writeUnlockOptions,
		  kIOUCScalarIScalarO, 1, 0 },
		/* kIOAccelSurfaceRead */
		{ 0, (IOMethod)&FirebrandSurfaceClient::read,
		  kIOUCStructIStructO, sizeof(IOAccelSurfaceReadData), 0 },
		/* kIOAccelSurfaceSetShapeBacking */
		{ 0, (IOMethod)&FirebrandSurfaceClient::setShapeBacking,
		  kIOUCScalarIStructI, 4, 0xffffffff },
		/* kIOAccelSurfaceSetIDMode */
		{ 0, (IOMethod)&FirebrandSurfaceClient::setIDMode,
		  kIOUCScalarIScalarO, 2, 0 },
		/* kIOAccelSurfaceSetScale */
		{ 0, (IOMethod)&FirebrandSurfaceClient::setScale,
		  kIOUCScalarIStructI, 1, 0xffffffff },
		/* kIOAccelSurfaceSetShape */
		{ 0, (IOMethod)&FirebrandSurfaceClient::setShape,
		  kIOUCScalarIStructI, 2, 0xffffffff },
		/* kIOAccelSurfaceFlush */
		{ 0, (IOMethod)&FirebrandSurfaceClient::flush,
		  kIOUCScalarIScalarO, 2, 0 },
		/* kIOAccelSurfaceQueryLock */
		{ 0, (IOMethod)&FirebrandSurfaceClient::queryLock,
		  kIOUCScalarIScalarO, 0, 0 },
		/* kIOAccelSurfaceReadLock */
		{ 0, (IOMethod)&FirebrandSurfaceClient::readLock,
		  kIOUCScalarIStructO, 0, sizeof(IOAccelSurfaceInformation) },
		/* kIOAccelSurfaceReadUnlock */
		{ 0, (IOMethod)&FirebrandSurfaceClient::readUnlock,
		  kIOUCScalarIScalarO, 0, 0 },
		/* kIOAccelSurfaceWriteLock */
		{ 0, (IOMethod)&FirebrandSurfaceClient::writeLock,
		  kIOUCScalarIStructO, 0, sizeof(IOAccelSurfaceInformation) },
		/* kIOAccelSurfaceWriteUnlock */
		{ 0, (IOMethod)&FirebrandSurfaceClient::writeUnlock,
		  kIOUCScalarIScalarO, 0, 0 },
		/* kIOAccelSurfaceControl */
		{ 0, (IOMethod)&FirebrandSurfaceClient::control,
		  kIOUCScalarIScalarO, 2, 1 },
		/* kIOAccelSurfaceSetShapeBackingAndLength */
		{ 0, (IOMethod)&FirebrandSurfaceClient::setShapeBackingAndLength,
		  kIOUCScalarIStructI, 5, 0xffffffff },
	};

	if (index >= kIOAccelNumSurfaceMethods) {
		fPrivateAsked++;
		IOLog("FirebrandSurface: surface %lu: private method %lu asked for\n",
		      (unsigned long)fWid, (unsigned long)index);
		return 0;
	}
	*target = this;
	return (IOExternalMethod *)&methods[index];
}

/* Backed surfaces synchronize the caller's buffer with GPU storage. */
IOReturn FirebrandSurfaceClient::readLockOptions(UInt32 options,
	IOAccelSurfaceInformation *info, IOByteCount *size)
{
	bool syncToCPU = (options & kIOAccelSurfaceLockInMask) == kIOAccelSurfaceLockInBacking;
	IOReturn ret = fBackingMode ? lockBacking(info, size, syncToCPU) : lockForRead(info, size, syncToCPU);

	note(kIOAccelSurfaceReadLockOptions, options, ret, 0, 0);
	return ret;
}

IOReturn FirebrandSurfaceClient::readUnlockOptions(UInt32 options)
{
	note(kIOAccelSurfaceReadUnlockOptions, options, 0, 0, 0);
	unlockRead();
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::getState(UInt32 *state)
{
	note(kIOAccelSurfaceGetState, 0, 0, 0, 0);
	*state = kIOAccelSurfaceStateIdleBit;
	return kIOReturnSuccess;
}

/*
 * A WRITE LOCK DOES NOT READ THE STORE BACK OVER A BACKING THAT WAS JUST
 * READ BACK.
 *
 * System Preferences (and any window that asks for
 * NSWindowBackingLocationVideoMemory) draws with Quartz GL: the program's
 * own GL context draws the window into this surface's store, and the CPU
 * backing is only a copy. When CoreGraphics has to work on the pixels with
 * the CPU -- -[NSView scrollRect:by:] -> CGContextCopyWindowContentsToRect,
 * which issues no GL at all -- the window server keeps the two in step with
 * locks. Recorded on a test machine (2026-11-10, a ring of this client's calls),
 * one scroll step of a VideoMemory test window is
 *
 *     WriteLock(DontCare)  [store -> backing]  WriteUnlock(DontCare)  [backing -> store]
 *     ... GL draws the exposed strip, flush ...
 *     ReadLock(InBacking)  [store -> backing]  ReadUnlock
 *
 * and the CPU copy of the rows happens between that ReadLock and the next
 * WriteLock. The WriteLock's read-back put the picture from before the copy
 * over it, WriteUnlock sent the old picture up, and the strip landed on it:
 * rows that do not move, the exposed row drawn over them. That is the
 * scrambled list. The read-back was gated only on isGLProducer, which a
 * Quartz GL window always is.
 *
 * So a write lock reads back only when the backing has not already been
 * brought down since the last upload. A stale backing, or a store that may
 * have been drawn into after the last upload (fBackingFromStore false),
 * keeps the old rule, which is what a write lock with no read lock before it
 * relies on. Between a read-back and the next upload only the CPU writes:
 * GL draws come after the upload and the next ReadLock brings them down.
 * A store the window server's own GL draws into (DRAWABLE_BACKING) is
 * written back into the backing by Mesa when that context flushes
 * (OSMesaStoreBacking), so it does not depend on this read-back either.
 */
IOReturn FirebrandSurfaceClient::writeLockOptions(UInt32 options,
	IOAccelSurfaceInformation *info, IOByteCount *size)
{
	bool syncToCPU = (options & kIOAccelSurfaceLockInMask) != kIOAccelSurfaceLockInAccel;

	note(kIOAccelSurfaceWriteLockOptions, options, 0, 0, 0);
	return fBackingMode ? lockBacking(info, size, syncToCPU, syncToCPU)
			    : kIOReturnUnsupported;
}

IOReturn FirebrandSurfaceClient::writeUnlockOptions(UInt32 options)
{
	note(kIOAccelSurfaceWriteUnlockOptions, options, 0, 0, 0);
	if (!fBackingMode) return kIOReturnSuccess;
	IOLockLock(fBackingLock);
	if (fBackingStale && fBackingMap) {
		struct rdn_user_backing b = {fBackingAddress, 0, fBackingStride,
			fBackingWidth, fBackingHeight};
		if (fAccel->setSurfaceBacking(fWid, fTask, &b))
			fBackingStale = false;
	}
	IOReturn ret = syncBacking(false);
	unlockRead();
	IOLockUnlock(fBackingLock);
	return ret;
}

IOReturn FirebrandSurfaceClient::read(IOAccelSurfaceReadData *data,
				     IOByteCount inSize)
{
	note(kIOAccelSurfaceRead, data ? (UInt32)data->x : 0,
	     data ? (UInt32)data->y : 0, data ? (UInt32)data->w : 0,
	     data ? (UInt32)data->h : 0);
	if (fRefused)
		return kIOReturnUnsupported;
	if (!data || inSize < sizeof(*data) || !data->client_addr)
		return kIOReturnBadArgument;

	SInt64 x = (SInt32)data->x;
	SInt64 y = (SInt32)data->y;
	SInt64 w = (SInt32)data->w;
	SInt64 h = (SInt32)data->h;
	UInt64 dstAddr = (UInt64)(vm_address_t)data->client_addr;
	UInt32 dstStride = (UInt32)data->client_row_bytes;

	if (fBackingMode) {
		IOLockLock(fBackingLock);
		if (!fBackingMap) {
			IOLockUnlock(fBackingLock);
			return kIOReturnNotReady;
		}
		UInt32 surfW = fBackingWidth, surfH = fBackingHeight;
		if (w <= 0 || h <= 0 || x + w <= 0 || y + h <= 0 ||
		    x >= (SInt64)surfW || y >= (SInt64)surfH) {
			IOLockUnlock(fBackingLock);
			return kIOReturnSuccess;
		}
		if (x < 0) {
			dstAddr += (UInt64)(-x) * 4ULL;
			w += x;
			x = 0;
		}
		if (y < 0) {
			dstAddr += (UInt64)(-y) * (UInt64)dstStride;
			h += y;
			y = 0;
		}
		if (x + w > (SInt64)surfW)
			w = (SInt64)surfW - x;
		if (y + h > (SInt64)surfH)
			h = (SInt64)surfH - y;
		UInt32 rowBytes = (UInt32)w * 4U;
		if (h > 1 && dstStride < rowBytes) {
			IOLockUnlock(fBackingLock);
			return kIOReturnBadArgument;
		}
		UInt64 mapBytes = (UInt64)(UInt32)(h - 1) * (UInt64)dstStride +
				  (UInt64)rowBytes;
		if (!dstAddr || mapBytes > 0x7fffffffULL ||
		    dstAddr + mapBytes > 0x100000000ULL) {
			IOLockUnlock(fBackingLock);
			return kIOReturnBadArgument;
		}
		IOReturn ret = syncBacking(true);
		if (ret == kIOReturnSuccess) {
			IOMemoryDescriptor *dstMem = IOMemoryDescriptor::withAddress(
				(vm_address_t)dstAddr, (IOByteCount)mapBytes,
				kIODirectionIn, fTask);
			if (!dstMem) {
				ret = kIOReturnNoMemory;
			} else {
				ret = dstMem->prepare();
				if (ret == kIOReturnSuccess) {
					IOMemoryMap *dstMap = dstMem->map();
					if (!dstMap) {
						ret = kIOReturnNoMemory;
					} else {
						const UInt8 *src = (const UInt8 *)fBackingMap->getVirtualAddress() +
								   (size_t)y * fBackingStride + (size_t)x * 4U;
						UInt8 *dst = (UInt8 *)dstMap->getVirtualAddress();
						for (UInt32 r = 0; r < (UInt32)h; ++r)
							bcopy(src + (size_t)r * fBackingStride,
							      dst + (size_t)r * dstStride, rowBytes);
						dstMap->release();
					}
					dstMem->complete();
				}
				dstMem->release();
			}
		}
		IOLockUnlock(fBackingLock);
		return ret;
	}

	UInt32 offset, srcStride, width, height;
	if (!fAccel->getSurfaceBuffer(fWid, &offset, &srcStride, &width, &height))
		return kIOReturnUnsupported;
	IOMemoryDescriptor *aperture = fAccel->apertureMemory();
	if (!aperture)
		return kIOReturnNotReady;
	UInt64 end = (UInt64)offset + (UInt64)srcStride * height;
	if (!width || !height || (UInt64)width * 4ULL > srcStride ||
	    end > aperture->getLength())
		return kIOReturnBadArgument;
	if (w <= 0 || h <= 0 || x + w <= 0 || y + h <= 0 ||
	    x >= (SInt64)width || y >= (SInt64)height)
		return kIOReturnSuccess;
	if (x < 0) {
		dstAddr += (UInt64)(-x) * 4ULL;
		w += x;
		x = 0;
	}
	if (y < 0) {
		dstAddr += (UInt64)(-y) * (UInt64)dstStride;
		h += y;
		y = 0;
	}
	if (x + w > (SInt64)width)
		w = (SInt64)width - x;
	if (y + h > (SInt64)height)
		h = (SInt64)height - y;
	UInt32 rowBytes = (UInt32)w * 4U;
	if (h > 1 && dstStride < rowBytes)
		return kIOReturnBadArgument;
	UInt64 mapBytes = (UInt64)(UInt32)(h - 1) * (UInt64)dstStride +
			  (UInt64)rowBytes;
	if (!dstAddr || mapBytes > 0x7fffffffULL ||
	    dstAddr + mapBytes > 0x100000000ULL)
		return kIOReturnBadArgument;
	IOReturn ret = fAccel->waitForCPU();
	if (ret != kIOReturnSuccess)
		return ret;
	volatile UInt8 *vram = fAccel->vramKernel();
	if (!vram)
		return kIOReturnNotReady;
	IOMemoryDescriptor *dstMem = IOMemoryDescriptor::withAddress(
		(vm_address_t)dstAddr, (IOByteCount)mapBytes,
		kIODirectionIn, fTask);
	if (!dstMem)
		return kIOReturnNoMemory;
	ret = dstMem->prepare();
	if (ret == kIOReturnSuccess) {
		IOMemoryMap *dstMap = dstMem->map();
		if (!dstMap) {
			ret = kIOReturnNoMemory;
		} else {
			const volatile UInt8 *src = vram + offset + (size_t)y * srcStride + (size_t)x * 4U;
			UInt8 *dst = (UInt8 *)dstMap->getVirtualAddress();
			for (UInt32 r = 0; r < (UInt32)h; ++r)
				fromVram(dst + (size_t)r * dstStride, src + (size_t)r * srcStride, rowBytes);
			dstMap->release();
		}
		dstMem->complete();
	}
	dstMem->release();
	return ret;
}

IOReturn FirebrandSurfaceClient::setShapeBacking(UInt32 options, UInt32 fbIndex,
	UInt32 backing, UInt32 rowBytes, IOAccelDeviceRegion *rgn, IOByteCount size)
{
	note(kIOAccelSurfaceSetShapeBacking, options, fbIndex, backing, rowBytes);
	noteRegion(kIOAccelSurfaceSetShapeBacking, rgn, size);
	return configureBacking(options, fbIndex, backing, rowBytes, 0, rgn, size);
}

IOReturn FirebrandSurfaceClient::setIDMode(UInt32 wid, UInt32 mode)
{
	bool backing = fAccel->backingSurfaces();

	/*
	 * Surface 1 is the window server's screen compositor, not an app buffer; it is let through
	 * the gate only with BackingSurfaces. Without that key (the installers' default) the gate
	 * refuses every surface that is not a GL one, surface 1 included, as it always did: letting
	 * it through alone made the window server composite GL windows (QuickTime Player's video)
	 * from the wrong data on 10.5, bisected on the G5 (2026-10-22).
	 */
	/*
	 * AN APP'S SURFACE IS NOT THE SCREEN'S, AND ONE KEY CANNOT SAY BOTH.
	 *
	 * The paragraph above is about surface 1 and stays true. What it did
	 * not separate is every OTHER surface, which the same `!backing' term
	 * refused along with it. Measured 2026-11-02, with
	 * BackingSurfaces absent (the installers' default) and the gate on:
	 *
	 *     RadeonNI: surface client 0x321cf000 opened
	 *     RadeonNI: surface 0: setIDMode(0x31bf9226, 0x24, 0x1, 0x0)
	 *     RadeonNI: surface client 0x321cf000 (window 0) closed after 1 calls
	 *
	 * over and over -- every GL surface an application asks for refused
	 * (the 0x1 is fRefused), the client closing at once and trying again.
	 * Quartz Extreme reports Supported while nothing can obtain an
	 * accelerated surface; the screen saver preview is black for this
	 * reason.
	 *
	 * That mode is 0x24 -- exactly
	 * kIOAccelSurfaceModeWindowedBit|kIOAccelSurfaceModeColorDepth8888,
	 * what the test below asks for. The ONLY failing term was `!backing'.
	 *
	 * And isGLSurface() cannot rescue it, because it is unreachable: the
	 * GL driver never calls setIDMode (CoreGraphics does) and only
	 * REGISTERS a buffer afterwards, through RDN_UC_SURFACE_BUFFER ->
	 * setSurfaceBuffer(), which is the one place `.gl' is set
	 * (gld/rdn_mesa.c:880,903; RadeonNIAccel.cpp:2783). Refusing at
	 * creation means the registration that would have permitted it can
	 * never happen.
	 *
	 * So the two settings of one key break opposite halves: with the key
	 * on, surface 1 is let through and the window server composites from
	 * the wrong data (the bisect above); with it off, the compositor is
	 * right and no application can draw. Neither is wanted, and the
	 * difference is not about backing memory at all -- it is about WHICH
	 * surface.
	 *
	 * Surface 1 therefore keeps the old rule exactly, `backing' and all.
	 * Every other surface is judged on the mode alone, and is NOT put in
	 * fBackingMode: with the key absent there is no backing store to
	 * synchronise, and upstream records that half as unfinished anyway
	 * ("setShapeBacking ... ignore the supplied backing memory while
	 * returning success", g5/LEOPARD-ACCEL-README.txt). It takes the
	 * ordinary path, and the gld registers its buffer immediately after.
	 *
	 * If an app surface does turn out to need the backing path, it shows
	 * as wrong CONTENT in that app's window -- not a hang, and not
	 * anything the compositor can catch.
	 */
	/*
	 * SURFACE 1 REFUSED IS THE SOFTWARE COMPOSITOR, FOR GOOD.
	 *
	 * Measured on the T4240 (R7 450, 10.5.8, 2026-11-11): with surface 1
	 * refused, Leopard's window server composites EVERY display update on
	 * the CPU. CGXAcquireDisplayDeviceSurface (CoreGraphics, read in
	 * disassembly) wants an IOAccelSurface for the display before it tries
	 * its GL compositor at all: MPAccessSurfaceForDisplayDevice calls
	 * IOAccelCreateSurface(accelerator, 1, 0x24), and when that fails it
	 * clears the display's hardware-surface bit (0x08000000 in the display
	 * record's flags; read live with gdb as 0xd7000000, the bit missing) and
	 * never asks again. From then on CGXUpdateDisplay runs CGLayerComposite
	 * with layer_blit_thread -- RLE decompress of backing stores, shadows
	 * blended on the CPU, the result memcpy'd into the framebuffer through
	 * the aperture -- at 11 to 19 ms of window-server CPU per drag frame and
	 * zero GL command buffers per frame. "Quartz Extreme: in use" and the
	 * "GLCompositor: GL renderer id" log line are both true and both idle.
	 *
	 * The "wrong data" bisect above was made with a bundle that could not
	 * yet draw here (before radeonsi); it bound the
	 * decision to the wrong cause. So surface 1 passes the gate like any
	 * other surface and takes the ordinary path -- setShape carries each
	 * update's region, the GL driver attaches it as the screen and copies
	 * the flushed update there, as on Tiger -- and BackingSurfaces keeps
	 * only what it was for: the opt-in backing mode of surface 1.
	 *
	 * The rule is a switch (FirebrandAccel::screenSurface()): personality key
	 * ScreenSurface, boot argument frb_screensurface=0/1 (RadeonNI's
	 * rdn_screensurface), or at run time
	 * {ScreenSurface: bool} through IORegistryEntrySetCFProperties
	 * (tools/guest/rdnprop.c), read at the window server's next start. The
	 * default is TRUE (Info.plist) since the window server learned to
	 * texture a program's surface from the program's own store
	 * (gld/rdn_mesa.c, rdn_mesa_cgls_set_integer); false is the software
	 * compositor, correct and slow, for a comparison or a retreat.
	 */
	if (fAccel->surfaceGate() && !fAccel->isGLSurface(wid)) {
		bool screen = (wid == 1);

		if (mode != (kIOAccelSurfaceModeWindowedBit | kIOAccelSurfaceModeColorDepth8888)
		    || (screen && !backing && !fAccel->screenSurface())) {
			fRefused = true;
			note(kIOAccelSurfaceSetIDMode, wid, mode, 1, 0);
			return kIOReturnUnsupported;
		}
		if (screen && backing)
			fBackingMode = true;
	}
	fWid = wid;
	fMode = mode;
	note(kIOAccelSurfaceSetIDMode, wid, mode, 0, 0);
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::setScale(UInt32 options,
	IOAccelSurfaceScaling *scaling, IOByteCount size)
{
	note(kIOAccelSurfaceSetScale, options, size, 0, 0);
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::setShape(UInt32 options, UInt32 fbIndex,
	IOAccelDeviceRegion *rgn, IOByteCount size)
{
	note(kIOAccelSurfaceSetShape, options, fbIndex, size, 0);
	noteRegion(kIOAccelSurfaceSetShape, rgn, size);
	if (fRefused)
		return kIOReturnUnsupported;
	const UInt32 header = offsetof(IOAccelDeviceRegion, rect);
	if (!rgn || size < header ||
	    rgn->num_rects > (size - header) / sizeof(IOAccelBounds))
		return kIOReturnBadArgument;
	if (!fAccel->setSurfaceRegion(fWid, rgn, rgn->num_rects))
		return kIOReturnNoResources;
	fSetShape = true;
	return kIOReturnSuccess;
}

/*
 * The window server's way of showing a surface's new picture when it need
 * not draw the window again itself. There is one screen: bit 0 of the mask.
 */
IOReturn FirebrandSurfaceClient::flush(UInt32 fbMask, UInt32 options)
{
	IOReturn ret = kIOReturnSuccess;

	if (fBackingMode) {
		IOLockLock(fBackingLock);
		ret = syncBacking(true);
		IOLockUnlock(fBackingLock);
	} else if (fAccel && fWid && (fbMask & 1))
		ret = fAccel->flushSurface(fWid);
	countFlush(fbMask, options);
	note(kIOAccelSurfaceFlush, fbMask, options, ret, 0);
	return ret;
}

IOReturn FirebrandSurfaceClient::queryLock(void)
{
	note(kIOAccelSurfaceQueryLock, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::readLock(IOAccelSurfaceInformation *info,
					 IOByteCount *size)
{
	IOReturn ret = fBackingMode ? lockBacking(info, size) : lockForRead(info, size);

	note(kIOAccelSurfaceReadLock, ret, 0, 0, 0);
	return ret;
}

IOReturn FirebrandSurfaceClient::readUnlock(void)
{
	note(kIOAccelSurfaceReadUnlock, 0, 0, 0, 0);
	unlockRead();
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::writeLock(IOAccelSurfaceInformation *info,
					  IOByteCount *size)
{
	return writeLockOptions(kIOAccelSurfaceLockInBacking, info, size);
}

IOReturn FirebrandSurfaceClient::writeUnlock(void)
{
	return writeUnlockOptions(kIOAccelSurfaceLockInBacking);
}

IOReturn FirebrandSurfaceClient::control(UInt32 selector, UInt32 arg,
					UInt32 *result)
{
	countControl(selector, arg);
	note(kIOAccelSurfaceControl, selector, arg, 0, 0);
	*result = 0;
	if (fBackingMode) {
		if (selector != 1 && selector != 4 && selector != 5)
			return kIOReturnUnsupported;
		/* The backing is pinned and retained; making it nonvolatile synchronizes it. */
		if (selector == 4 || selector == 5) *result = 1;
		if (selector == 1 || (selector == 5 && !arg)) {
			IOReturn ret = kIOReturnSuccess;

			IOLockLock(fBackingLock);
			/*
			 * synchronizeBackingSurface() opens with this (5, 0) when it
			 * had made the surface volatile, also before the write lock
			 * that follows a CPU edit: the rule of writeLockOptions().
			 */
			if (selector == 1 || !fBackingFromStore || fBackingStale)
				ret = syncBacking(true);
			IOLockUnlock(fBackingLock);
			return ret;
		}
		if (selector == 5 && arg && !fAccel->isGLProducer(fWid)) {
			IOLockLock(fBackingLock);
			IOReturn ret = syncBacking(false);
			IOLockUnlock(fBackingLock);
			return ret;
		}
	}
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::setShapeBackingAndLength(UInt32 options,
	UInt32 fbIndex, UInt32 backing, UInt32 rowBytes, UInt32 length,
	IOAccelDeviceRegion *rgn, IOByteCount size)
{
	note(kIOAccelSurfaceSetShapeBackingAndLength, options, fbIndex, backing, rowBytes);
	noteRegion(kIOAccelSurfaceSetShapeBackingAndLength, rgn, size);
	return configureBacking(options, fbIndex, backing, rowBytes, length, rgn, size);
}

void FirebrandSurfaceClient::releaseBacking(void)
{
	if (fBackingGartSize) {
		fAccel->setSurfaceBackingGart(fWid, fTask, 0);
		fAccel->gartUnbind(this, fBackingGart);
		fBackingGart = fBackingGartSize = fBackingGartSub = 0;
	}
	if (fBackingMap) { fBackingMap->release(); fBackingMap = NULL; }
	if (fBackingMemory) {
		fBackingMemory->complete();
		fBackingMemory->release();
		fBackingMemory = NULL;
	}
	fBackingAddress = fBackingStride = fBackingWidth = fBackingHeight = 0;
	fBackingStale = false;
	fBackingFromStore = false;
	fCPUOwned = false;
	if (fAccel && fWid)
		fAccel->setSurfaceBackingClient(fWid, NULL, this);
}

void FirebrandSurfaceClient::free(void)
{
	dropReadMap();
	releaseBacking();
	if (fBackingLock) IOLockFree(fBackingLock);
	super::free();
}

IOReturn FirebrandSurfaceClient::configureBacking(UInt32 options, UInt32 fbIndex,
	UInt32 address, UInt32 stride, UInt32 length,
	IOAccelDeviceRegion *region, IOByteCount size)
{
	if (fRefused || !fWid || fWid == 1 ||
	    fMode != (kIOAccelSurfaceModeWindowedBit | kIOAccelSurfaceModeColorDepth8888))
		return kIOReturnUnsupported;
	if (fbIndex) return kIOReturnBadArgument;
	const UInt32 header = offsetof(IOAccelDeviceRegion, rect);
	if (!region || size < header ||
	    region->num_rects > (size - header) / sizeof(IOAccelBounds) ||
	    !address || region->bounds.w <= 0 || region->bounds.h <= 0 ||
	    region->bounds.w > 16384 || region->bounds.h > 16384 ||
	    stride < (UInt32)region->bounds.w * 4)
		return kIOReturnBadArgument;
	UInt64 bytes = (UInt64)stride * region->bounds.h;
	if (bytes > 0x7fffffffU || (UInt64)address + bytes > 0x100000000ULL ||
	    (length && bytes > length)) return kIOReturnBadArgument;
	/* WindowServer also publishes direct CPU edits by rebinding the backing. */
	IOLockLock(fBackingLock);
	bool unchanged = fBackingMap && fBackingAddress == address &&
		fBackingStride == stride && fBackingWidth == (UInt32)region->bounds.w &&
		fBackingHeight == (UInt32)region->bounds.h;
	if (unchanged) {
		IOReturn ret = kIOReturnNoResources;
		bool stale = (options & kIOAccelSurfaceShapeStaleBackingBit) != 0;
		if (fBackingStale != stale) {
			struct rdn_user_backing b = {fBackingAddress,
				stale ? RDN_USER_BACKING_STALE : 0,
				fBackingStride, fBackingWidth, fBackingHeight};
			if (fAccel->setSurfaceBacking(fWid, fTask, &b))
				fBackingStale = stale;
		}
		if (fAccel->setSurfaceRegion(fWid, region, region->num_rects))
			ret = stale ? kIOReturnSuccess : syncBacking(false);
		IOLockUnlock(fBackingLock);
		return ret;
	}
	IOLockUnlock(fBackingLock);
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddress(
		(vm_address_t)address, (IOByteCount)bytes, kIODirectionOutIn, fTask);
	if (!memory) return kIOReturnNoMemory;
	IOReturn ret = memory->prepare();
	if (ret != kIOReturnSuccess) { memory->release(); return ret; }
	IOMemoryMap *mapping = memory->map();
	if (!mapping) { memory->complete(); memory->release(); return kIOReturnNoMemory; }
	bool stale = (options & kIOAccelSurfaceShapeStaleBackingBit) != 0;
	struct rdn_user_backing backing = {address,
		stale ? RDN_USER_BACKING_STALE : 0, stride,
		(UInt32)region->bounds.w, (UInt32)region->bounds.h};
	bool adopted = false;
	IOLockLock(fBackingLock);
	if (fBackingGartSize || fBackingMap)
		ret = fAccel->waitForCPU();
	if (ret == kIOReturnSuccess && !fAccel->setSurfaceBacking(fWid, fTask, &backing))
		ret = kIOReturnNoResources;
	if (ret == kIOReturnSuccess) {
		releaseBacking();
		adopted = true;
		fBackingMode = true;
		fBackingStale = stale;
		fBackingMemory = memory;
		fBackingMap = mapping;
		fBackingAddress = address;
		fBackingStride = stride;
		fBackingWidth = backing.width;
		fBackingHeight = backing.height;
		/*
		 * Bind it into the GART so syncBacking() can let the engine
		 * move it. gartBind() wants both ends page aligned, and the
		 * size here is stride*height, which is not (2688*464 is
		 * 304.5 pages), so the length is rounded up -- that page
		 * belongs to the caller's allocation and is normally mapped,
		 * but if it is not the bind fails and the copy simply stays
		 * on the CPU. Failure is never fatal here: fBackingGartSize
		 * zero means "use bcopy", which is what this did before.
		 */
		{
			const UInt32 pg = 4096;
			UInt32 sub = address & (pg - 1);
			UInt32 base = address - sub;
			UInt64 endAligned = ((UInt64)address + bytes + pg - 1) & ~(UInt64)(pg - 1);
			UInt32 want = (UInt32)(endAligned - base);
			UInt32 at = 0;

			if (fBackingGartSize) {
				fAccel->setSurfaceBackingGart(fWid, fTask, 0);
				fAccel->gartUnbind(this, fBackingGart);
				fBackingGart = fBackingGartSize = fBackingGartSub = 0;
			}
			if (!(address & 3) &&
			    endAligned <= 0x100000000ULL &&
			    endAligned - base <= 0x7fffffffULL &&
			    fAccel->gartBind(this, fTask, base, want, &at)
				== kIOReturnSuccess) {
				fBackingGart = at;
				fBackingGartSize = want;
				fBackingGartSub = sub;
				fAccel->setSurfaceBackingGart(fWid, fTask,
					fAccel->gartAddr(at) + sub);
			}
		}
		/* A new backing holds the window's picture: the CPU has it. */
		fCPUOwned = true;
		fSetShape = fAccel->setSurfaceRegion(fWid, region, region->num_rects);
		fAccel->setSurfaceBackingClient(fWid, this, NULL);
		ret = !fSetShape ? kIOReturnNoResources :
		      (options & kIOAccelSurfaceShapeStaleBackingBit)
			      ? kIOReturnSuccess : syncBacking(false);
	}
	IOLockUnlock(fBackingLock);
	if (!adopted) {
		mapping->release(); memory->complete(); memory->release();
	}
	return ret;
}

IOReturn FirebrandSurfaceClient::syncBacking(bool toCPU)
{
	UInt32 offset, stride, width, height;
	if (!fBackingMap) return kIOReturnNotReady;
	if (!fAccel->getSurfaceBuffer(fWid, &offset, &stride, &width, &height))
		return kIOReturnSuccess;
	/* Resizing replaces the GL store on the next drawable attachment. */
	if (width != fBackingWidth || height != fBackingHeight)
		return kIOReturnSuccess;
	IOMemoryDescriptor *aperture = fAccel->apertureMemory();
	UInt64 end = (UInt64)offset + (UInt64)stride * height;
	if (!aperture || stride < width * 4 || end > aperture->getLength())
		return kIOReturnBadArgument;
	if (toCPU && !fBackingStale && !fAccel->isGLProducer(fWid))
		return kIOReturnSuccess;
	/* Quartz GL: no GL has drawn since the CPU got the backing (acquireForGPU). */
	if (toCPU && !fBackingStale && fGPUAcquires && fCPUOwned)
		return kIOReturnSuccess;
	/*
	 * THE ENGINE NEEDS NO CPU-SIDE DRAIN; THE CPU DOES (RadeonNI's
	 * reasoning, measured on its single GFX ring). On Firebrand the copy
	 * runs on SDMA0, and its IB waits on the GPU for the clients' newest
	 * GFX work and for SDMA0's own before it (FirebrandAccel2D.cpp), so the
	 * ordering the drain gave is there without it; blitLinear() waits for
	 * its own fence, so the destination is readable when this returns.
	 * The CPU path keeps the drain, and must: there the CPU reads and
	 * writes video memory itself.
	 */
	const bool canBlit = fBackingGartSize && !(stride & 3) && !(fBackingStride & 3) && !(fBackingGartSub & 3);
	IOReturn ret;

	if (canBlit) {
		const UInt64 gart = fAccel->gartAddr(fBackingGart) + fBackingGartSub;
		const UInt64 vram = fAccel->vramAddr(offset);
		IOReturn br;

		if (toCPU)
			br = fAccel->blitLinear(gart, fBackingStride / 4,
						vram, stride / 4, width, height);
		else
			br = fAccel->blitLinear(vram, stride / 4,
						gart, fBackingStride / 4,
						width, height);
		if (br == kIOReturnSuccess) {
			fBackingFromStore = toCPU;
			fTransfers++;
			if (toCPU)
				fCPUOwned = true;
			if (toCPU && fBackingStale) {
				struct rdn_user_backing b = {fBackingAddress, 0,
					fBackingStride, fBackingWidth, fBackingHeight};
				if (fAccel->setSurfaceBacking(fWid, fTask, &b))
					fBackingStale = false;
			}
			return kIOReturnSuccess;
		}
		/* Busy, wedged or refused: the CPU does it, drain and all. */
	}

	ret = fAccel->waitForCPU();
	if (ret != kIOReturnSuccess)
		return ret;
	{
		volatile UInt8 *gpu = fAccel->vramKernel();
		UInt8 *cpu = (UInt8 *)fBackingMap->getVirtualAddress();

		if (!gpu)
			return kIOReturnNotReady;
		gpu += offset;
		for (UInt32 y = 0; y < height; ++y) {
			if (toCPU)
				fromVram(cpu + y * fBackingStride, gpu + y * stride, width * 4);
			else
				toVram(gpu + y * stride, cpu + y * fBackingStride, width * 4);
		}
		__asm__ volatile("sync" ::: "memory");
	}
	fBackingFromStore = toCPU;
	fTransfers++;
	if (toCPU)
		fCPUOwned = true;
	if (toCPU && fBackingStale) {
		struct rdn_user_backing b = {fBackingAddress, 0,
			fBackingStride, fBackingWidth, fBackingHeight};
		if (fAccel->setSurfaceBacking(fWid, fTask, &b))
			fBackingStale = false;
	}
	return kIOReturnSuccess;
}

IOReturn FirebrandSurfaceClient::lockBacking(IOAccelSurfaceInformation *info,
                                           IOByteCount *size, bool syncToCPU,
                                           bool keepCPUEdits)
{
	if (!info || !size || *size < sizeof(*info)) return kIOReturnBadArgument;
	IOLockLock(fBackingLock);
	/* Read back already, nothing sent up since: the CPU's edits are the newest picture. */
	if (keepCPUEdits && fBackingFromStore && !fBackingStale)
		syncToCPU = false;
	IOReturn ret = syncToCPU ? syncBacking(true) : kIOReturnSuccess;
	/* keepCPUEdits is the write lock: from here the CPU has the backing. */
	if (ret == kIOReturnSuccess && keepCPUEdits)
		fCPUOwned = true;
	if (ret == kIOReturnSuccess) {
		bzero(info, sizeof(*info));
		info->address[0] = fBackingAddress;
		info->rowBytes = fBackingStride;
		info->width = fBackingWidth;
		info->height = fBackingHeight;
		info->pixelFormat = kIOAccelSurfaceModeColorDepth8888;
		*size = sizeof(*info);
		fAccel->setReadLocked(fWid);
	}
	IOLockUnlock(fBackingLock);
	return ret;
}

/*
 * See FirebrandAccel::acquireSurfaceForGPU(): a program's GL is about to draw
 * into this surface. If the CPU has the backing, copy it up first -- under
 * fBackingLock, the lock releaseBacking() unbinds the GART pages under, and
 * syncBacking() returns only after the engine has finished with them.
 */
IOReturn FirebrandSurfaceClient::acquireForGPU(bool *copied)
{
	IOReturn ret = kIOReturnSuccess;

	*copied = false;
	IOLockLock(fBackingLock);
	if (!fBackingMap) {
		IOLockUnlock(fBackingLock);
		return kIOReturnNotReady;
	}
	fGPUAcquires = true;
	if (fBackingStale) {
		/* The store is the picture; the backing is not worth sending up. */
		fCPUOwned = false;
	} else if (fCPUOwned) {
		UInt32 before = fTransfers;

		ret = syncBacking(false);
		/* No store yet, or a resize under way: the CPU keeps it, try again later. */
		if (ret == kIOReturnSuccess && fTransfers != before) {
			fCPUOwned = false;
			*copied = true;
		}
	}
	IOLockUnlock(fBackingLock);
	return ret;
}
