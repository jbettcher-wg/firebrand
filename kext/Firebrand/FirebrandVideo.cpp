/*
 * The 'rdnv' video client (FirebrandVideo.h): RadeonNIVideo.cpp's user
 * client and RadeonNIVideoMemory.cpp's memory, ported; the session
 * (RadeonNIUvdSession.cpp's shape) on UVD 6.3, 7.2, SI's 3.1 and CIK's 4.2.
 *
 * One stream in the whole system, as on RadeonNI (max_sessions 1). Every
 * call that touches a session holds fVideoLock; the core's lock is taken
 * only around register and ring access, never across a wait for the
 * decoder.
 *
 * A session:
 *   VRAM   session context | macroblock context | DPB, one allocation
 *          (invisible heap first), cleared by SDMA1 before the create msg;
 *   GTT    the kernel's control pages (msg, feedback, IT scaling table, IB)
 *          and the bitstream, written by the CPU (snooped, no flush);
 *   slots  two FirebrandVideoMemory buffers bound into the GART: UVD
 *          writes each picture straight into one (no readback copy), the
 *          client maps them read-only. Each is a guard page, the NV12
 *          target, a guard page; the guards are checked after every picture.
 *   upload FirebrandVideoMemory the client writes the slice data into; the
 *          kernel copies it into the GTT bitstream before every decode, so
 *          the engine never reads memory the client can still change.
 *
 * UVD before 5.0 (SI's 3.1, CIK's 4.2; frb_uvd.address_64_bit false) addresses 28 bits
 * within a 256 MB segment that UVD_LMI_ADDR_EXT names per memory client
 * (uvd_v3_1_mc_resume: the VCPU bo's segment for the VCPU, 0 for the
 * decoder's own clients). Linux keeps every buffer a decode names in the
 * first 256 MB of its domain (amdgpu_uvd_cs_pass1's
 * amdgpu_uvd_force_into_uvd_segment, the msg and feedback forced into
 * VRAM); RadeonNI found on Turks's UVD that the decoder's reference and
 * output pictures follow UVD_LMI_ADDR_EXT's nibbles 1 and 2, not the
 * addresses' own bits 28-31 (its docs/UVD.md, "split mode"). The GART here
 * is at 0x4000_0000 (segments 4-7), so the slots and a GTT bitstream are out
 * of reach: everything the IB names is in VRAM inside the VCPU bo's
 * segment, VRAM's first 256 MB (the CPU window, the VCPU bo at its top):
 * the control pages (msg, feedback, IT scaling table, IB) written through
 * BAR0 (the msg built in host memory, only the dwords that changed copied;
 * the feedback read back after an HDP invalidate), the bitstream (copied in
 * by SDMA1 from a GTT staging buffer the CPU fills), one decode target
 * (copied out by SDMA1 into the client's slot) and the DPB. The IB in VRAM
 * is also in the ring's 4 GB (uvd_v3_1_start's LMI_EXT40_ADDR for the RBC).
 * The messages are the legacy ones (stream type H264, frb_uvd_h264.h).
 *
 * The create message goes out at CREATE with the decoder powered up
 * (begin_use); the destroy message at DESTROY or the client's end, then the
 * idle work powers it down. A decode that times out poisons the decoder
 * until the driver is reloaded (the VCPU's state is unknown); a picture
 * whose feedback reports an error ends the session's decoding (sticky, as
 * RadeonNI's decodeFailed: the library opens a new session at the next IDR).
 *
 * Copyright (c) 2026 kouta-kun and Claude (RadeonNIVideo.cpp, RadeonNIVideoMemory.cpp,
 * RadeonNIUvdSession.cpp, which this follows)
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <sys/errno.h>

#include "FirebrandAccel.h"
#include "FirebrandVideo.h"

extern "C" {
#include "frb_uvd.h"
}

/* Bus addresses: FirebrandCore::busOfPage() (qoriq_dma.h). */

enum {
	kVideoPage = 4096,
	kVideoTimeoutMs = 1000,
	kVideoQuota = 20u * 1024 * 1024,
	/* the control GTT object: one page each */
	kCtlMsg = 0, kCtlFeedback = 1 * kVideoPage, kCtlIts = 2 * kVideoPage, kCtlIb = 3 * kVideoPage,
	kCtlBytes = 4 * kVideoPage,
	/* UVD before 5.0: the VRAM in the VCPU's segment is control pages | bitstream | target */
	kSegBitstream = kCtlBytes, kSegTarget = kCtlBytes + RDN_VIDEO_MAX_UPLOAD_BYTES
};
#define kGuardWord	0xa5a5a5a5u
/* UVD before 5.0: a buffer's GPU address range stays inside one of these (amdgpu_uvd_cs_pass2) */
#define kUvdSegment	(256ull << 20)

struct FirebrandVideoSession {
	FirebrandVideoClient *owner;
	rdn_video_token token;
	rdn_uvd_h264_config config;
	struct frb_uvd_h264_layout layout;
	struct frb_uvd_h264_buffers buffers;
	UInt32 handle;
	/* VRAM: session context, macroblock context, DPB */
	UInt64 vramOffset, vramBytes;
	bool vramAllocated;
	/* GTT: the kernel's */
	struct frb_gtt control, bitstream;
	bool controlUp, bitstreamUp;
	/* UVD before 5.0 (segmented): control pages, bitstream and target in VRAM, in the VCPU's segment */
	bool segmented, ctlAllocated;
	UInt64 ctlOffset, ctlBytes;	/* BAR0 offset and size */
	UInt64 vramTarget;		/* GPU address of the decode target there (copied out to a slot) */
	UInt8 msg[FRB_UVD_H264_MSG_BYTES];	/* the next message, built here */
	UInt8 msgVram[FRB_UVD_H264_MSG_BYTES];	/* what the VRAM msg page holds now */
	/* the client's */
	FirebrandVideoMemory *upload, *slot[2];
	UInt32 slotFirst[2], slotPages[2];
	bool slotBound[2];
	UInt64 slotTarget[2];		/* GPU address of the NV12 target in each slot */
	UInt32 slotBytes;		/* guard + target + guard */
	UInt32 nextRequest, nextOutput, held[2];
	bool awake, created, destroyed, quarantined, validation, decodeFailed;
	UInt64 uvdUs, pictures;
};

static UInt64 videoTimeUs(void)
{
	UInt64 ticks, nanos;

	clock_get_uptime(&ticks);
	absolutetime_to_nanoseconds(ticks, &nanos);
	return nanos / 1000;
}

/*
 * A wait for the decoder sleeps in 100 us naps (RadeonNI found IOSleep(1)
 * rounds every wait up to a millisecond boundary): a real block, not a spin.
 */
static void videoNap(void)
{
	static int event;

	assert_wait_timeout((event_t)&event, THREAD_UNINT, 100, NSEC_PER_USEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static bool videoZero(const uint32_t *words, unsigned count)
{
	for (unsigned i = 0; i < count; ++i)
		if (words[i])
			return false;
	return true;
}

static bool videoToken(const rdn_video_token &a, const rdn_video_token &b)
{
	return a.high == b.high && a.low == b.low && (a.high || a.low);
}

static UInt32 videoProfile(UInt32 profile)
{
	return profile == 66 ? RDN_VIDEO_PROFILE_BASELINE : profile == 77 ? RDN_VIDEO_PROFILE_MAIN :
	       profile == 100 ? RDN_VIDEO_PROFILE_HIGH : 0;
}

/* --- FirebrandVideoMemory (RadeonNIVideoMemory) -------------------------------- */

#undef super
#define super IOGeneralMemoryDescriptor
OSDefineMetaClassAndStructors(FirebrandVideoMemory, IOGeneralMemoryDescriptor)

FirebrandVideoMemory *FirebrandVideoMemory::withBytes(FirebrandAccel *owner, UInt32 bytes, bool readOnly)
{
	FirebrandVideoMemory *memory;

	if (!bytes || bytes > kVideoQuota - 4095)
		return 0;
	bytes = (bytes + 4095) & ~4095u;
	memory = new FirebrandVideoMemory;
	if (!memory)
		return 0;
	memory->fOwner = 0;
	memory->fBuffer = 0;
	memory->fBytes = 0;
	memory->fReadOnly = readOnly;
	memory->fPrepared = false;
	if (!owner->videoCharge(bytes)) {
		memory->release();
		return 0;
	}
	owner->retain();
	memory->fOwner = owner;
	memory->fBytes = bytes;
	memory->fBuffer = IOMallocAligned(bytes, 4096);
	if (!memory->fBuffer) {
		memory->release();
		return 0;
	}
	bzero(memory->fBuffer, bytes);
	if (!memory->initWithAddress(memory->fBuffer, bytes, kIODirectionInOut)) {
		memory->release();
		return 0;
	}
	/* wired for the GART (the slots) and for the kernel's copies (the upload) */
	if (memory->prepare() != kIOReturnSuccess) {
		memory->release();
		return 0;
	}
	memory->fPrepared = true;
	return memory;
}

IOReturn FirebrandVideoMemory::doMap(vm_map_t map, IOVirtualAddress *address, IOOptionBits options,
				     IOByteCount offset, IOByteCount length)
{
	/* Leopard grants write access when a mapping requests cache-mode changes. */
	if ((options & kIOMapCacheMask) != kIOMapDefaultCache || (fReadOnly && !(options & kIOMapReadOnly)))
		return kIOReturnNotPermitted;
	return super::doMap(map, address, options, offset, length);
}

void FirebrandVideoMemory::free(void)
{
	FirebrandAccel *owner = fOwner;
	void *buffer = fBuffer;
	UInt32 bytes = fBytes;

	if (fPrepared) {
		complete();
		fPrepared = false;
	}
	/* Unwire and release VM entries before freeing the underlying pages. */
	super::free();
	if (buffer)
		IOFreeAligned(buffer, bytes);
	if (owner) {
		owner->videoUncharge(bytes);
		owner->release();
	}
}

/* --- FirebrandVideoClient (RadeonNIVideoUserClient) ---------------------------- */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(FirebrandVideoClient, IOUserClient)

bool FirebrandVideoClient::initWithTask(task_t task, void *securityID, UInt32 type)
{
	if (!super::initWithTask(task, securityID, type))
		return false;
	fAccel = 0;
	fClosed = false;
	fCreated = false;
	fAdministrator = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
	fClientLock = IOLockAlloc();
	return fClientLock != 0;
}

bool FirebrandVideoClient::start(IOService *provider)
{
	FirebrandAccel *accel = OSDynamicCast(FirebrandAccel, provider);

	if (!accel || !super::start(provider))
		return false;
	fAccel = accel;
	fAccel->retain();
	return true;
}

void FirebrandVideoClient::free(void)
{
	if (fClientLock) {
		IOLockFree(fClientLock);
		fClientLock = 0;
	}
	super::free();
}

IOReturn FirebrandVideoClient::clientClose(void)
{
	IOLockLock(fClientLock);
	if (!fClosed) {
		fClosed = true;
		if (fAccel)
			fAccel->videoClose(this);
	}
	IOLockUnlock(fClientLock);
	terminate();
	return kIOReturnSuccess;
}

IOReturn FirebrandVideoClient::clientDied(void)
{
	return clientClose();
}

void FirebrandVideoClient::stop(IOService *provider)
{
	FirebrandAccel *accel;

	IOLockLock(fClientLock);
	fClosed = true;
	accel = fAccel;
	if (accel)
		accel->videoClose(this);
	fAccel = 0;
	IOLockUnlock(fClientLock);
	if (accel)
		accel->release();
	super::stop(provider);
}

IOExternalMethod *FirebrandVideoClient::getTargetAndMethodForIndex(IOService **target, UInt32 index)
{
	static const IOExternalMethod methods[RDN_VIDEO_METHOD_COUNT] = {
		{ 0, (IOMethod)&FirebrandVideoClient::methodQuery, kIOUCScalarIStructO, 0, sizeof(rdn_video_query) },
		{ 0, (IOMethod)&FirebrandVideoClient::methodCreate, kIOUCStructIStructO, sizeof(rdn_video_create),
		  sizeof(rdn_video_session) },
		{ 0, (IOMethod)&FirebrandVideoClient::methodDecode, kIOUCStructIStructO, sizeof(rdn_video_decode),
		  sizeof(rdn_video_output) },
		{ 0, (IOMethod)&FirebrandVideoClient::methodRelease, kIOUCScalarIStructI, 0, sizeof(rdn_video_release) },
		{ 0, (IOMethod)&FirebrandVideoClient::methodDestroy, kIOUCScalarIStructI, 0, sizeof(rdn_video_destroy) },
		/* DECODE_GPU, ACQUIRE_GPU, RETIRE_GPU */
		{ 0, (IOMethod)&FirebrandVideoClient::methodUnsupported, kIOUCScalarIScalarO, 0, 0 },
		{ 0, (IOMethod)&FirebrandVideoClient::methodUnsupported, kIOUCScalarIScalarO, 0, 0 },
		{ 0, (IOMethod)&FirebrandVideoClient::methodUnsupported, kIOUCScalarIScalarO, 0, 0 },
	};

	if (index >= RDN_VIDEO_METHOD_COUNT)
		return 0;
	*target = this;
	return const_cast<IOExternalMethod *>(&methods[index]);
}

IOReturn FirebrandVideoClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	IOReturn result;

	if (!options || !memory)
		return kIOReturnBadArgument;
	*memory = 0;
	IOLockLock(fClientLock);
	result = !fClosed && fAccel ? fAccel->videoMemory(this, type, options, memory) : kIOReturnNotOpen;
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodQuery(rdn_video_query *out, IOByteCount *size)
{
	IOReturn result = kIOReturnNotOpen;

	if (!out || !size || *size != sizeof(*out))
		return kIOReturnBadArgument;
	bzero(out, sizeof(*out));
	IOLockLock(fClientLock);
	if (!fClosed && fAccel) {
		fAccel->videoQuery(fAdministrator, out);
		result = kIOReturnSuccess;
	}
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodCreate(const rdn_video_create *in, rdn_video_session *out, IOByteCount inSize,
					    IOByteCount *outSize)
{
	IOReturn result = kIOReturnNotOpen;

	if (!in || !out || !outSize || inSize != sizeof(*in) || *outSize != sizeof(*out))
		return kIOReturnBadArgument;
	bzero(out, sizeof(*out));
	IOLockLock(fClientLock);
	if (!fClosed && fAccel) {
		result = fCreated ? kIOReturnExclusiveAccess : fAccel->videoCreate(this, fAdministrator, in, out);
		if (result == kIOReturnSuccess)
			fCreated = true;
	}
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodDecode(const rdn_video_decode *in, rdn_video_output *out, IOByteCount inSize,
					    IOByteCount *outSize)
{
	IOReturn result;

	if (!in || !out || !outSize || inSize != sizeof(*in) || *outSize != sizeof(*out))
		return kIOReturnBadArgument;
	bzero(out, sizeof(*out));
	IOLockLock(fClientLock);
	result = !fClosed && fAccel ? fAccel->videoDecode(this, in, out) : kIOReturnNotOpen;
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodRelease(const rdn_video_release *in, IOByteCount size)
{
	IOReturn result;

	if (!in || size != sizeof(*in))
		return kIOReturnBadArgument;
	IOLockLock(fClientLock);
	result = !fClosed && fAccel ? fAccel->videoRelease(this, in) : kIOReturnNotOpen;
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodDestroy(const rdn_video_destroy *in, IOByteCount size)
{
	IOReturn result;

	if (!in || size != sizeof(*in))
		return kIOReturnBadArgument;
	IOLockLock(fClientLock);
	result = !fClosed && fAccel ? fAccel->videoDestroy(this, in) : kIOReturnNotOpen;
	IOLockUnlock(fClientLock);
	return result;
}

IOReturn FirebrandVideoClient::methodUnsupported(void)
{
	return kIOReturnUnsupported;
}

/* --- the accelerator's side ----------------------------------------------------- */

bool FirebrandAccel::videoCharge(UInt32 bytes)
{
	UInt32 old;

	do {
		old = fVideoCpuBytes;
		if (old > kVideoQuota || bytes > kVideoQuota - old)
			return false;
	} while (!OSCompareAndSwap(old, old + bytes, (UInt32 *)&fVideoCpuBytes));
	return true;
}

void FirebrandAccel::videoUncharge(UInt32 bytes)
{
	OSAddAtomic(-(SInt32)bytes, (SInt32 *)&fVideoCpuBytes);
}

/* the decoder is there: stage uvd done, its ring up, the accelerator not retiring, not poisoned */
bool FirebrandAccel::videoReady(bool administrator, bool validation)
{
	bool ready;

	fCore->lock();
	ready = fVideoOffered && !fRetired && !fVideoPoisoned && frb_uvd_ring(fCore->adev()) != 0;
	fCore->unlock();
	if (ready && validation)
		ready = administrator;
	return ready;
}

void FirebrandAccel::videoQuery(bool administrator, rdn_video_query *out)
{
	bool production, validation;

	bzero(out, sizeof(*out));
	out->version = RDN_VIDEO_API_VERSION;
	out->bytes = sizeof(*out);
	if (!fVideoLock || !IOLockTryLock(fVideoLock)) {
		out->flags = RDN_VIDEO_CAP_BUSY;
		out->unavailable_reason = 4;
		return;
	}
	production = videoReady(false, false);
	validation = videoReady(administrator, true);
	if (production || validation)
		out->flags |= RDN_VIDEO_CAP_READY;
	if (production)
		out->flags |= RDN_VIDEO_CAP_PRODUCTION;
	if (validation)
		out->flags |= RDN_VIDEO_CAP_VALIDATION;
	if (fVideoSession)
		out->flags |= RDN_VIDEO_CAP_BUSY;
	out->unavailable_reason = fRetired ? 1 : fVideoPoisoned ? 2 : !(production || validation) ? 3 : 0;
	if (production || validation) {
		out->max_width = RDN_UVD_H264_MAX_WIDTH;
		out->max_height = RDN_UVD_H264_MAX_HEIGHT;
		out->max_refs = RDN_UVD_H264_MAX_REFS;
		out->profile_mask = RDN_VIDEO_PROFILE_BASELINE | RDN_VIDEO_PROFILE_MAIN | RDN_VIDEO_PROFILE_HIGH;
		out->level_mask = 0x1fff;	/* levels 1.0, 1.1, 1.2, 1.3, 2.0, 2.1, 2.2, 3.0, 3.1, 3.2, 4.0, 4.1, 4.2 */
		out->max_upload_bytes = RDN_VIDEO_MAX_UPLOAD_BYTES;
		out->max_sessions = 1;
		out->max_inflight = 1;
		out->output_slots = RDN_VIDEO_OUTPUT_SLOTS;
		out->max_vram_bytes = 128u * 1024 * 1024;
		out->max_cpu_bytes = kVideoQuota;
	}
	IOLockUnlock(fVideoLock);
}

/* fVideoLock held. The decoder's state is unknown: no more video until a reload. */
void FirebrandAccel::videoPoison(int error, const char *why)
{
	fVideoPoisoned = true;
	fCore->setVideoState(fVideoSession ? 1 : 0, true);
	if (fVideoSession)
		fVideoSession->quarantined = true;
	if (fFramebuffer)
		fFramebuffer->setProperty("EnableUVDSessions", false);	/* FrostByte's probe: software from here */
	IOLog("FirebrandAccel: video: %s (%d); the decoder is out of service until the driver is reloaded\n",
	      why, error);
}

/*
 * The kernel pages of a FirebrandVideoMemory into the GART (FRB_PTE_GTT:
 * snooped system pages through inbound window 2, as gartBind does for a
 * client's own pages).
 */
int FirebrandAccel::videoBindLocked(FirebrandVideoMemory *memory, UInt32 *first, UInt32 *pages)
{
	struct frb_device *adev = fCore->adev();
	UInt32 n = memory->length() / kVideoPage, i;
	uint32_t at = 0;

	if (!adev->gart.ready || !n)
		return -ENODEV;
	if (frb_gart_alloc(adev, n, &at))
		return -ENOMEM;
	{
		QorIQDMAWalk *walk = fCore->dmaWalk(memory);

		for (i = 0; i < n; i++) {
			UInt64 bus = fCore->busOfPage(walk, memory, (UInt64)i * kVideoPage);

			if (!bus || !frb_gart_bus_addr_ok(adev, bus)) {
				IOLog("FirebrandAccel: video: page %lu at 0x%llx has no bus address\n",
				      (unsigned long)i,
				      (unsigned long long)memory->getPhysicalSegment64((IOByteCount)i * kVideoPage, 0));
				if (walk)
					delete walk;
				frb_gart_unbind(adev, at, n);
				frb_gart_free(adev, at, n);
				return -EIO;
			}
			frb_gart_set_pte(adev, at + i, bus, frb_gart_gtt_flags(adev));
			if (!i)
				fCore->logBinding("video", memory, bus, n);
		}
		if (walk)
			delete walk;
	}
	frb_gart_invalidate_tlb(adev);
	fCore->busMaster();
	*first = at;
	*pages = n;
	return 0;
}

/*
 * VRAM through BAR0, a dword at a time (little-endian words, as the GTT
 * pages are written); with a shadow only the dwords that differ from it,
 * and the shadow follows. fCore's lock held, the accelerator not retired.
 */
static void videoVramCopy(struct frb_device *adev, UInt64 offset, const UInt8 *src, UInt32 bytes, UInt8 *shadow)
{
	UInt32 i, v;

	for (i = 0; i + 4 <= bytes; i += 4) {
		v = frb_get_le32(src, i);
		if (shadow) {
			if (frb_get_le32(shadow, i) == v)
				continue;
			frb_put_le32(shadow, i, v);
		}
		frb_vram_write32(adev, (uint32_t)(offset + i), v);
	}
}

static void videoVramZero(struct frb_device *adev, UInt64 offset, UInt32 bytes)
{
	UInt32 i;

	for (i = 0; i + 4 <= bytes; i += 4)
		frb_vram_write32(adev, (uint32_t)(offset + i), 0);
}

/*
 * One message to the decoder and the wait for it: the msg (and for a
 * decode the IT scaling table) in the control pages, the IB after them in
 * the ring's byte order, submitted on the UVD ring with its fence; then
 * naps until the fence passes. With the control pages in VRAM (segmented)
 * the msg is built in the session and copied through BAR0 with the
 * feedback's reset and the IB, under the core's lock. 0, -ECANCELED
 * (retiring), -ETIMEDOUT (poisoned), or another errno.
 */
int FirebrandAccel::videoCommand(UInt32 type, const rdn_uvd_h264_picture *picture, UInt32 bitstreamBytes)
{
	FirebrandVideoSession *s = fVideoSession;
	struct frb_device *adev = fCore->adev();
	bool vram = s->ctlAllocated;
	UInt8 *ctl = vram ? 0 : (UInt8 *)s->control.cpu;
	UInt8 *msg = vram ? s->msg : ctl + kCtlMsg;
	UInt8 its[FRB_UVD_H264_ITS_BYTES];
	bool withIts = type == RDN_UVD_H264_MSG_DECODE && !s->layout.hw.legacy;
	uint32_t words[FRB_UVD_H264_IB_MAX_DW], n, i, seq = 0;
	struct frb_uvd_h264_buffers b = s->buffers;
	struct frb_ring *ring;
	struct frb_ib ib;
	UInt64 t0;
	int r;

	if (type == RDN_UVD_H264_MSG_CREATE)
		r = frb_uvd_h264_pack_create(&s->config, &s->layout, s->handle, msg);
	else if (type == RDN_UVD_H264_MSG_DECODE)
		r = frb_uvd_h264_pack_decode(&s->config, picture, &s->layout, s->handle, s->nextRequest,
					     bitstreamBytes, msg);
	else
		r = frb_uvd_h264_pack_destroy(s->handle, msg);
	if (r)
		return -EINVAL;
	if (withIts)
		frb_uvd_h264_pack_its(picture, vram ? its : ctl + kCtlIts);
	if (type == RDN_UVD_H264_MSG_DECODE && !vram) {
		/* the feedback buffer: zero, its size first (little-endian) */
		bzero(ctl + kCtlFeedback, FRB_UVD_H264_FEEDBACK_BYTES);
		frb_put_le32(ctl + kCtlFeedback, 0, FRB_UVD_H264_FEEDBACK_BYTES);
	}
	n = frb_uvd_h264_build_ib(type, &s->layout.hw, &b, words);
	if (!n)
		return -EINVAL;

	fCore->lock();
	ring = frb_uvd_ring(adev);
	if (fRetired || !ring) {
		fCore->unlock();
		return -ECANCELED;
	}
	bzero(&ib, sizeof(ib));
	ib.size_dw = kVideoPage / 4;
	if (vram) {
		/* BAR0: the msg's changed dwords, the feedback reset, the IT table; the IB below */
		videoVramCopy(adev, s->ctlOffset + kCtlMsg, msg, FRB_UVD_H264_MSG_BYTES, s->msgVram);
		if (type == RDN_UVD_H264_MSG_DECODE) {
			frb_vram_write32(adev, (uint32_t)(s->ctlOffset + kCtlFeedback), FRB_UVD_H264_FEEDBACK_BYTES);
			videoVramZero(adev, s->ctlOffset + kCtlFeedback + 4, FRB_UVD_H264_FEEDBACK_BYTES - 4);
		}
		if (withIts)
			videoVramCopy(adev, s->ctlOffset + kCtlIts, its, FRB_UVD_H264_ITS_BYTES, 0);
		ib.cpu = 0;
		ib.bar_offset = (uint32_t)(s->ctlOffset + kCtlIb);
		ib.gpu_addr = vramAddr(s->ctlOffset + kCtlIb);
	} else {
		ib.cpu = ctl + kCtlIb;
		ib.gpu_addr = s->control.gpu_addr + kCtlIb;
	}
	for (i = 0; i < n; i++)
		frb_uvd_ib_write(adev, &ib, words[i]);
	/*
	 * The CPU's stores into the snooped pages are ordered before the WPTR's
	 * MMIO store (sync); BAR0's go out of the HDP with frb_ib_schedule's flush.
	 */
	OSSynchronizeIO();
	frb_uvd_ring_begin_use(ring);
	r = frb_ib_schedule(ring, &ib, &seq);
	fCore->unlock();
	if (r)
		return r;

	t0 = videoTimeUs();
	for (;;) {
		bool done;

		fCore->lock();
		ring = frb_uvd_ring(adev);
		done = ring && (int32_t)(frb_fence_read(ring) - seq) >= 0;
		fCore->unlock();
		if (done)
			break;
		if (!ring)
			return -ECANCELED;
		if (videoTimeUs() - t0 > (UInt64)kVideoTimeoutMs * 1000) {
			char why[96];

			snprintf(why, sizeof(why), "the decoder's fence %u did not come (message type %u)",
				 (unsigned)seq, (unsigned)type);
			videoPoison(-ETIMEDOUT, why);
			return -ETIMEDOUT;
		}
		videoNap();
	}
	s->uvdUs += videoTimeUs() - t0;
	return 0;
}

/* the feedback's first dwords: from the GTT page, or VRAM after an HDP invalidate. false: retiring */
bool FirebrandAccel::videoFeedback(UInt32 *words, UInt32 count)
{
	FirebrandVideoSession *s = fVideoSession;
	struct frb_device *adev = fCore->adev();
	UInt32 i;

	if (!s->ctlAllocated) {
		for (i = 0; i < count; i++)
			words[i] = frb_get_le32((const UInt8 *)s->control.cpu + kCtlFeedback, 4 * i);
		return true;
	}
	fCore->lock();
	if (fRetired) {
		fCore->unlock();
		return false;
	}
	frb_invalidate_hdp(adev);
	for (i = 0; i < count; i++)
		words[i] = frb_vram_read32(adev, (uint32_t)(s->ctlOffset + kCtlFeedback + 4 * i));
	fCore->unlock();
	return true;
}

static bool videoGuards(const FirebrandVideoSession *s, UInt32 slot)
{
	const UInt32 *head = (const UInt32 *)s->slot[slot]->bytes();
	const UInt32 *tail = (const UInt32 *)((const UInt8 *)s->slot[slot]->bytes() + s->slotBytes - kVideoPage);
	UInt32 i;

	for (i = 0; i < kVideoPage / 4; i++)
		if (head[i] != kGuardWord || tail[i] != kGuardWord)
			return false;
	return true;
}

/* a GTT object's pages without its GART entries (the card is halted and BAR0 gone) */
static void videoGttDrop(struct frb_gtt *gtt)
{
	IOBufferMemoryDescriptor *mem = (IOBufferMemoryDescriptor *)gtt->handle;

	if (!mem)
		return;
	mem->complete();
	mem->release();
	gtt->handle = 0;
	gtt->cpu = 0;
}

/*
 * fVideoLock held. destroy: send the destroy message first (when the
 * session was created and the decoder is healthy). Memory the engine may
 * still write is kept (a quarantined session stays, holding the one slot,
 * until the accelerator retires). hardware false: the engines are halted
 * and BAR0 is unmapped (retire): the pages go, no GART entry or VRAM heap
 * is touched.
 */
void FirebrandAccel::videoCleanup(bool destroy, bool hardware)
{
	FirebrandVideoSession *s = fVideoSession;
	struct frb_device *adev = fCore->adev();
	unsigned i;

	if (!s)
		return;
	if (!hardware) {
		for (i = 0; i < 2; i++)
			s->slotBound[i] = false;
		if (s->controlUp)
			videoGttDrop(&s->control);
		if (s->bitstreamUp)
			videoGttDrop(&s->bitstream);
		s->controlUp = s->bitstreamUp = s->vramAllocated = s->ctlAllocated = s->awake = false;
		s->quarantined = false;
		destroy = false;
	}
	if (destroy && s->created && !s->destroyed && !s->quarantined && !fVideoPoisoned) {
		int r = videoCommand(RDN_UVD_H264_MSG_DESTROY, 0, 0);

		if (!r)
			s->destroyed = true;
		else if (r != -ECANCELED && r != -ETIMEDOUT)
			videoPoison(r, "the destroy message failed");
		/* -ECANCELED: retiring; the engines are halted next, the session's memory may go */
	}
	if (s->quarantined) {
		/* the engine may still write the slots or the VRAM: keep it all; retire() halts it first */
		s->owner = 0;
		return;
	}
	fCore->lock();
	if (s->awake) {
		/* no session left: the clocks down, the VCPU stopped (amdgpu's idle work) */
		if (frb_uvd_idle_work_handler(adev))
			IOLog("FirebrandAccel: video: the decoder did not go idle\n");
		s->awake = false;
	}
	for (i = 0; i < 2; i++) {
		if (s->slotBound[i]) {
			frb_gart_unbind(adev, s->slotFirst[i], s->slotPages[i]);
			frb_gart_free(adev, s->slotFirst[i], s->slotPages[i]);
		}
		s->slotBound[i] = false;
	}
	if (s->controlUp)
		fCore->gttFree(&s->control);
	if (s->bitstreamUp)
		fCore->gttFree(&s->bitstream);
	s->controlUp = s->bitstreamUp = false;
	if (s->vramAllocated)
		frb_vram_mgr_free(&fVram, s->vramOffset, s->vramBytes);
	if (s->ctlAllocated)
		frb_vram_mgr_free(&fVram, s->ctlOffset, s->ctlBytes);
	s->vramAllocated = s->ctlAllocated = false;
	fCore->unlock();
	for (i = 0; i < 2; i++) {
		if (s->slot[i])
			s->slot[i]->release();
		s->slot[i] = 0;
	}
	if (s->upload)
		s->upload->release();
	s->upload = 0;
	if (s->pictures)
		IOLog("FirebrandAccel: video: session %08x:%08x ended: %llu pictures, %llu us in the decoder "
		      "(%llu us a picture)\n", (unsigned)s->token.high, (unsigned)s->token.low,
		      (unsigned long long)s->pictures, (unsigned long long)s->uvdUs,
		      (unsigned long long)(s->uvdUs / s->pictures));
	IOFree(s, sizeof(*s));
	fVideoSession = 0;
	fCore->setVideoState(0, fVideoPoisoned);
}

IOReturn FirebrandAccel::videoCreate(FirebrandVideoClient *owner, bool administrator, const rdn_video_create *in,
				     rdn_video_session *out)
{
	struct frb_device *adev = fCore->adev();
	struct frb_uvd_h264_layout layout;
	struct frb_uvd_h264_hw hw;
	struct frb_ring *uvdRing;
	FirebrandVideoSession *s = 0;
	IOReturn result = kIOReturnNoMemory;
	const char *stage = "session";
	UInt64 ctxAt, dpbAt, bytes = 0, t0 = videoTimeUs();
	UInt64 vcpu = 0, ringGpu = 0, window = 0, segLo = 0, segHi = 0;
	bool segmented = false;
	unsigned i;
	int r = 0;

	if (in->version != RDN_VIDEO_API_VERSION || in->bytes != sizeof(*in) ||
	    (in->flags & ~(RDN_VIDEO_CREATE_VALIDATION | RDN_VIDEO_CREATE_GPU_OUTPUT)) || !videoZero(in->reserved, 7))
		return kIOReturnBadArgument;
	/* no GPU-output lease on Firebrand (QUERY never offers it) */
	if (in->flags & RDN_VIDEO_CREATE_GPU_OUTPUT)
		return kIOReturnUnsupported;
	/* RadeonNI: smaller coded frames completed with unchanged output on Turks */
	if (in->config.coded_width < 64 || in->config.coded_height < 64)
		return kIOReturnUnsupported;
	/*
	 * The decoder the session will run on: its GPCOM register base (the
	 * first UVD instance that is up) and Mesa's DPB alignment for the family
	 * (ac_uvd_dec_dpb_alignment: 32 from Vega 10 on).
	 */
	fCore->lock();
	uvdRing = frb_uvd_ring(adev);
	hw.reg_base = frb_uvd_reg_base(adev, uvdRing ? uvdRing->me : 0);
	/* UVD before 5.0 (SI): 32-bit addresses in 256 MB segments, the VCPU bo's the msg's */
	if (uvdRing && adev->uvd && !adev->uvd->address_64_bit) {
		segmented = true;
		vcpu = adev->uvd->inst[uvdRing->me].gpu_addr;
		ringGpu = uvdRing->gpu_addr;
	}
	fCore->unlock();
	hw.dpb_alignment = adev->asic_type >= CHIP_VEGA10 ? 32 : 16;
	/* ac_uvd_create_video_decoder: stream type H264_PERF from Tonga on, H264 and the legacy DPB before */
	hw.legacy = adev->asic_type < CHIP_TONGA;
	if (!videoProfile(in->config.profile_idc) || frb_uvd_h264_layout_init(&hw, &in->config, &layout))
		return kIOReturnUnsupported;
	if (segmented) {
		/* the VCPU's segment in VRAM offsets: the msg and feedback go there */
		UInt64 seg = vcpu & ~(kUvdSegment - 1), base = vramAddr(0);

		window = kUvdSegment;
		segLo = seg > base ? seg - base : 0;
		segHi = seg + kUvdSegment - base;
	}

	IOLockLock(fVideoLock);
	if (!videoReady(administrator, (in->flags & RDN_VIDEO_CREATE_VALIDATION) != 0)) {
		result = kIOReturnNotReady;
		goto done;
	}
	if (fVideoSession) {
		result = kIOReturnBusy;
		goto done;
	}
	s = (FirebrandVideoSession *)IOMalloc(sizeof(*s));
	if (!s)
		goto done;
	bzero(s, sizeof(*s));
	fVideoSession = s;
	fCore->setVideoState(1, fVideoPoisoned);
	s->owner = owner;
	s->config = in->config;
	s->layout = layout;
	s->validation = (in->flags & RDN_VIDEO_CREATE_VALIDATION) != 0;
	++fVideoEpoch;
	s->token.high = (UInt32)(fVideoEpoch >> 32);
	s->token.low = (UInt32)fVideoEpoch;
	/* a stream handle of its own each session (Mesa: a base XOR a counter), never 0 */
	s->handle = (0x46524256u ^ (UInt32)fVideoEpoch) | 0x80000000u;
	s->nextRequest = s->nextOutput = 1;
	s->segmented = segmented;

	/*
	 * VRAM: session context | macroblock context | DPB (before Polaris the
	 * DPB alone); segmented, in the VCPU's segment (the visible heap)
	 */
	stage = "vram";
	ctxAt = (UInt64)layout.session_bytes;
	dpbAt = ctxAt + ((layout.ctx_bytes + kVideoPage - 1) & ~(kVideoPage - 1));
	s->vramBytes = dpbAt + ((layout.dpb_bytes + kVideoPage - 1) & ~(kVideoPage - 1));
	fCore->lock();
	r = segmented ? -ENOMEM :
		frb_vram_mgr_alloc(&fVram, s->vramBytes, 64 * 1024, FRB_VRAM_INVISIBLE, &s->vramOffset, &bytes);
	if (r)
		r = frb_vram_mgr_alloc_windowed(&fVram, s->vramBytes, 64 * 1024, FRB_VRAM_VISIBLE, vramAddr(0), window,
						segLo, segHi, &s->vramOffset, &bytes);
	if (!r) {
		s->vramAllocated = true;
		s->vramBytes = bytes;
	}
	fCore->unlock();
	if (r)
		goto fail;
	s->buffers.session = layout.session_bytes ? vramAddr(s->vramOffset) : 0;
	s->buffers.ctx = layout.ctx_bytes ? vramAddr(s->vramOffset + ctxAt) : 0;
	s->buffers.dpb = vramAddr(s->vramOffset + dpbAt);

	if (segmented) {
		/*
		 * In the VCPU's segment: the control pages (zeroed through BAR0),
		 * the bitstream and the decode target; the bitstream's GTT
		 * staging buffer anywhere (SDMA1 copies it in).
		 */
		stage = "segment-vram";
		s->ctlBytes = kSegTarget + ((layout.target_bytes + kVideoPage - 1) & ~(kVideoPage - 1));
		fCore->lock();
		r = fRetired ? -ECANCELED :
			frb_vram_mgr_alloc_windowed(&fVram, s->ctlBytes, 64 * 1024, FRB_VRAM_VISIBLE, vramAddr(0),
						    kUvdSegment, segLo, segHi, &s->ctlOffset, &s->ctlBytes);
		if (!r) {
			s->ctlAllocated = true;
			videoVramZero(adev, s->ctlOffset, kCtlBytes);
			frb_flush_hdp(adev);
		}
		fCore->unlock();
		if (r)
			goto fail;
		s->buffers.msg = vramAddr(s->ctlOffset + kCtlMsg);
		s->buffers.feedback = vramAddr(s->ctlOffset + kCtlFeedback);
		s->buffers.its = vramAddr(s->ctlOffset + kCtlIts);
		s->vramTarget = vramAddr(s->ctlOffset + kSegTarget);
		stage = "gtt";
		fCore->lock();
		r = fCore->gttAlloc(RDN_VIDEO_MAX_UPLOAD_BYTES, &s->bitstream);
		if (!r)
			s->bitstreamUp = true;
		fCore->unlock();
		if (r)
			goto fail;
	} else {
		/* GTT: the control pages and the bitstream */
		stage = "gtt";
		fCore->lock();
		r = fCore->gttAlloc(kCtlBytes, &s->control);
		if (!r) {
			s->controlUp = true;
			r = fCore->gttAlloc(RDN_VIDEO_MAX_UPLOAD_BYTES, &s->bitstream);
			if (!r)
				s->bitstreamUp = true;
		}
		fCore->unlock();
		if (r)
			goto fail;
		s->buffers.msg = s->control.gpu_addr + kCtlMsg;
		s->buffers.feedback = s->control.gpu_addr + kCtlFeedback;
		s->buffers.its = s->control.gpu_addr + kCtlIts;
	}
	s->buffers.bitstream = segmented ? vramAddr(s->ctlOffset + kSegBitstream) : s->bitstream.gpu_addr;

	/* the client's: the upload, and the two slots bound into the GART */
	stage = "upload-ram";
	s->upload = FirebrandVideoMemory::withBytes(this, RDN_VIDEO_MAX_UPLOAD_BYTES, false);
	if (!s->upload)
		goto fail;
	s->slotBytes = kVideoPage + ((layout.target_bytes + kVideoPage - 1) & ~(kVideoPage - 1)) + kVideoPage;
	for (i = 0; i < 2; i++) {
		UInt32 *w, k;

		stage = i ? "slot1-ram" : "slot0-ram";
		s->slot[i] = FirebrandVideoMemory::withBytes(this, s->slotBytes, true);
		if (!s->slot[i])
			goto fail;
		w = (UInt32 *)s->slot[i]->bytes();
		for (k = 0; k < kVideoPage / 4; k++) {
			w[k] = kGuardWord;
			w[(s->slotBytes - kVideoPage) / 4 + k] = kGuardWord;
		}
		stage = i ? "slot1-gart" : "slot0-gart";
		fCore->lock();
		r = fRetired ? -ECANCELED : videoBindLocked(s->slot[i], &s->slotFirst[i], &s->slotPages[i]);
		fCore->unlock();
		if (r)
			goto fail;
		s->slotBound[i] = true;
		s->slotTarget[i] = gartAddr(s->slotFirst[i] * kVideoPage) + kVideoPage;
	}

	/*
	 * Every buffer the IB names, and the IB, in the VCPU bo's segment (the
	 * placement amdgpu_uvd_cs_pass1 makes; pass2's checks are weaker), and
	 * the IB in the ring's 4 GB (its LMI_EXT40_ADDR): refused here rather
	 * than found out by the decoder.
	 */
	if (segmented) {
		struct { const char *name; UInt64 start, bytes; } spans[] = {
			{ "msg", s->buffers.msg, FRB_UVD_H264_MSG_BYTES },
			{ "feedback", s->buffers.feedback, FRB_UVD_H264_FEEDBACK_BYTES },
			{ "DPB", s->buffers.dpb, layout.dpb_bytes },
			{ "bitstream", s->buffers.bitstream, RDN_VIDEO_MAX_UPLOAD_BYTES },
			{ "target", s->vramTarget, layout.target_bytes },
			{ "IB", vramAddr(s->ctlOffset + kCtlIb), kVideoPage },
		};

		stage = "segments";
		for (i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
			UInt64 a = spans[i].start, e = spans[i].start + spans[i].bytes - 1;

			if ((a >> 28) != (vcpu >> 28) || (e >> 28) != (vcpu >> 28) ||
			    (i == 5 && (a >> 32) != (ringGpu >> 32))) {
				IOLog("FirebrandAccel: video: the %s at 0x%llx-0x%llx breaks UVD's segments (VCPU 0x%llx, "
				      "ring 0x%llx)\n", spans[i].name, (unsigned long long)a, (unsigned long long)e,
				      (unsigned long long)vcpu, (unsigned long long)ringGpu);
				r = -ERANGE;
				goto fail;
			}
		}
	}

	/* the contexts and the DPB start out zero (Mesa clears them; nothing of another client shows) */
	stage = "vram-clear";
	fCore->lock();
	r = fRetired ? -ECANCELED : frb_sdma_fill_buffer(adev, 1, 0, vramAddr(s->vramOffset), (UInt32)s->vramBytes);
	fCore->unlock();
	if (r)
		goto fail;

	/* the decoder up (begin_use), and the create message */
	stage = "uvd-create";
	s->awake = true;
	r = videoCommand(RDN_UVD_H264_MSG_CREATE, 0, 0);
	if (r) {
		result = r == -ECANCELED ? kIOReturnNotReady : kIOReturnIOError;
		goto fail;
	}
	s->created = true;

	IOLog("FirebrandAccel: video: session %08x:%08x handle %08x, %ux%u profile %u level %u refs %u: "
	      "VRAM %llu KB at 0x%llx (ctx %u KB, DPB %u KB), slots %u KB at GART 0x%llx/0x%llx, %llu ms%s\n",
	      (unsigned)s->token.high, (unsigned)s->token.low, (unsigned)s->handle, (unsigned)layout.width,
	      (unsigned)layout.height, (unsigned)in->config.profile_idc, (unsigned)in->config.level_idc,
	      (unsigned)in->config.max_num_ref_frames, (unsigned long long)(s->vramBytes >> 10),
	      (unsigned long long)vramAddr(s->vramOffset), (unsigned)(layout.ctx_bytes >> 10),
	      (unsigned)(layout.dpb_bytes >> 10), (unsigned)(s->slotBytes >> 10),
	      (unsigned long long)s->slotTarget[0], (unsigned long long)s->slotTarget[1],
	      (unsigned long long)((videoTimeUs() - t0) / 1000), s->validation ? " (validation)" : "");
	if (segmented)
		IOLog("FirebrandAccel: video: UVD before 5.0, all in the VCPU bo's segment (bo at 0x%llx): msg/feedback/IB "
		      "0x%llx, bitstream 0x%llx, target 0x%llx, DPB 0x%llx; stream type H264\n",
		      (unsigned long long)vcpu, (unsigned long long)s->buffers.msg,
		      (unsigned long long)s->buffers.bitstream, (unsigned long long)s->vramTarget,
		      (unsigned long long)s->buffers.dpb);

	out->version = RDN_VIDEO_API_VERSION;
	out->bytes = sizeof(*out);
	out->session = s->token;
	out->coded_width = s->config.coded_width;
	out->coded_height = s->config.coded_height;
	out->format = RDN_VIDEO_FORMAT_NV12;
	out->y_stride = out->uv_stride = s->config.coded_width;
	out->uv_offset = s->config.coded_width * s->config.coded_height;
	out->output_bytes = layout.packed_nv12_bytes;
	out->upload_bytes = RDN_VIDEO_MAX_UPLOAD_BYTES;
	out->output_slots = RDN_VIDEO_OUTPUT_SLOTS;
	out->upload_memory_type = RDN_VIDEO_MEMORY_UPLOAD;
	out->output0_memory_type = RDN_VIDEO_MEMORY_OUTPUT0;
	out->output1_memory_type = RDN_VIDEO_MEMORY_OUTPUT1;
	result = kIOReturnSuccess;
	goto done;
fail:
	IOLog("FirebrandAccel: video: create failed at %s (%d, 0x%x): %ux%u, VRAM %llu KB, cpu %u KB charged\n",
	      stage, r, (unsigned)result, (unsigned)layout.width, (unsigned)layout.height,
	      (unsigned long long)(s ? s->vramBytes >> 10 : 0), (unsigned)(fVideoCpuBytes >> 10));
	if (result == kIOReturnSuccess)
		result = kIOReturnNoMemory;
	videoCleanup(false, true);
done:
	IOLockUnlock(fVideoLock);
	return result;
}

IOReturn FirebrandAccel::videoDecode(FirebrandVideoClient *owner, const rdn_video_decode *in, rdn_video_output *out)
{
	FirebrandVideoSession *s;
	IOReturn result = kIOReturnNotReady;
	UInt32 slot = 0, seqFb, status, fb[8];
	uint32_t padded = 0;
	UInt64 t0, t1, t2, t3;
	int r;

	if (in->version != RDN_VIDEO_API_VERSION || in->bytes != sizeof(*in) || !videoZero(in->reserved, 2) ||
	    !in->bitstream_bytes || in->bitstream_bytes > RDN_VIDEO_MAX_UPLOAD_BYTES)
		return kIOReturnBadArgument;
	IOLockLock(fVideoLock);
	s = fVideoSession;
	if (fRetired || !s || s->owner != owner || !s->created || s->quarantined || fVideoPoisoned)
		goto done;
	if (s->decodeFailed) {
		result = kIOReturnIOError;
		goto done;
	}
	if (!videoToken(in->session, s->token) || in->request_sequence != s->nextRequest ||
	    s->nextRequest == UINT32_MAX || s->nextOutput == UINT32_MAX) {
		result = kIOReturnBadArgument;
		goto done;
	}
	if (s->held[0])
		slot = 1;
	if (s->held[slot]) {
		result = kIOReturnBusy;
		goto done;
	}
	if (frb_uvd_h264_bitstream_size(in->bitstream_bytes, &padded) || padded > RDN_VIDEO_MAX_UPLOAD_BYTES ||
	    !frb_uvd_h264_valid_picture(&s->config, &in->picture)) {
		result = kIOReturnBadArgument;
		goto done;
	}
	t0 = videoTimeUs();
	/* the snapshot: the client's slice data into the GTT bitstream, zero to the 128-byte end */
	memcpy(s->bitstream.cpu, s->upload->bytes(), in->bitstream_bytes);
	bzero((UInt8 *)s->bitstream.cpu + in->bitstream_bytes, padded - in->bitstream_bytes);
	if (s->segmented) {
		/* into the VCPU's segment: SDMA1 from the snooped staging pages; the decode target is the VRAM one */
		fCore->lock();
		r = fRetired ? -ECANCELED :
			frb_sdma_copy_buffer(fCore->adev(), 1, s->bitstream.gpu_addr, s->buffers.bitstream, padded);
		fCore->unlock();
		if (r) {
			result = r == -ECANCELED ? kIOReturnNotReady : kIOReturnIOError;
			goto done;
		}
		s->buffers.target = s->vramTarget;
	} else {
		s->buffers.target = s->slotTarget[slot];
	}
	t1 = videoTimeUs();
	r = videoCommand(RDN_UVD_H264_MSG_DECODE, &in->picture, in->bitstream_bytes);
	if (r) {
		result = r == -ECANCELED ? kIOReturnNotReady : kIOReturnIOError;
		goto done;
	}
	t2 = videoTimeUs();
	if (!videoFeedback(fb, 8)) {
		result = kIOReturnNotReady;
		goto done;
	}
	seqFb = fb[1];
	status = fb[2];
	if (s->validation && s->nextRequest <= 8)
		IOLog("FirebrandAccel: video: feedback request %u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		      (unsigned)s->nextRequest, (unsigned)fb[0], (unsigned)seqFb, (unsigned)status, (unsigned)fb[3],
		      (unsigned)fb[4], (unsigned)fb[5], (unsigned)fb[6], (unsigned)fb[7]);
	/* completed malformed pictures report a nonzero status: no partial pixels (RadeonNI's rule) */
	if (seqFb != s->nextRequest || status) {
		IOLog("FirebrandAccel: video: picture %u failed: feedback sequence %u status 0x%x\n",
		      (unsigned)s->nextRequest, (unsigned)seqFb, (unsigned)status);
		s->decodeFailed = true;
		result = kIOReturnIOError;
		goto done;
	}
	if (s->segmented) {
		/* the picture out to the client's slot (SDMA1, VRAM to the slot's snooped pages) */
		fCore->lock();
		r = fRetired ? -ECANCELED :
			frb_sdma_copy_buffer(fCore->adev(), 1, s->vramTarget, s->slotTarget[slot], s->layout.target_bytes);
		fCore->unlock();
		if (r) {
			result = r == -ECANCELED ? kIOReturnNotReady : kIOReturnIOError;
			goto done;
		}
	}
	if (!videoGuards(s, slot)) {
		videoPoison(-EIO, "the decoder wrote past its target (a slot's guard page changed)");
		result = kIOReturnIOError;
		goto done;
	}
	t3 = videoTimeUs();
	s->pictures++;
	if (s->validation && s->nextRequest <= 8)
		IOLog("FirebrandAccel: video: decode timing request %u bitstream %u: snapshot_us %llu uvd_us %llu "
		      "check_us %llu\n", (unsigned)s->nextRequest, (unsigned)in->bitstream_bytes,
		      (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1), (unsigned long long)(t3 - t2));

	s->held[slot] = s->nextOutput++;
	out->version = RDN_VIDEO_API_VERSION;
	out->bytes = sizeof(*out);
	out->session = s->token;
	out->request_sequence = s->nextRequest++;
	out->output_sequence = s->held[slot];
	out->slot = slot;
	out->coded_width = s->config.coded_width;
	out->coded_height = s->config.coded_height;
	out->format = RDN_VIDEO_FORMAT_NV12;
	out->y_stride = out->uv_stride = s->layout.target_pitch;
	out->y_offset = kVideoPage;
	out->uv_offset = kVideoPage + s->layout.chroma_offset;
	out->output_bytes = s->layout.target_bytes;
	result = kIOReturnSuccess;
done:
	IOLockUnlock(fVideoLock);
	return result;
}

IOReturn FirebrandAccel::videoRelease(FirebrandVideoClient *owner, const rdn_video_release *in)
{
	FirebrandVideoSession *s;
	IOReturn result = kIOReturnBadArgument;

	if (in->version != RDN_VIDEO_API_VERSION || in->bytes != sizeof(*in) || !videoZero(in->reserved, 2) ||
	    in->slot >= 2 || !in->output_sequence)
		return kIOReturnBadArgument;
	IOLockLock(fVideoLock);
	s = fVideoSession;
	if (!fRetired && s && s->owner == owner && videoToken(s->token, in->session) &&
	    s->held[in->slot] == in->output_sequence) {
		s->held[in->slot] = 0;
		result = kIOReturnSuccess;
	}
	IOLockUnlock(fVideoLock);
	return result;
}

IOReturn FirebrandAccel::videoDestroy(FirebrandVideoClient *owner, const rdn_video_destroy *in)
{
	IOReturn result = kIOReturnBadArgument;

	if (in->version != RDN_VIDEO_API_VERSION || in->bytes != sizeof(*in) || !videoZero(in->reserved, 4))
		return kIOReturnBadArgument;
	IOLockLock(fVideoLock);
	if (fVideoSession && fVideoSession->owner == owner && videoToken(fVideoSession->token, in->session)) {
		videoCleanup(true, true);
		result = fVideoSession ? kIOReturnIOError : kIOReturnSuccess;
	}
	IOLockUnlock(fVideoLock);
	return result;
}

void FirebrandAccel::videoClose(FirebrandVideoClient *owner)
{
	if (!fVideoLock)
		return;
	IOLockLock(fVideoLock);
	if (fVideoSession && (!owner || fVideoSession->owner == owner))
		videoCleanup(true, !fVideoHalted);
	IOLockUnlock(fVideoLock);
}

IOReturn FirebrandAccel::videoMemory(FirebrandVideoClient *owner, UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory)
{
	FirebrandVideoSession *s;
	IOMemoryDescriptor *descriptor = 0;
	IOReturn result = kIOReturnNotReady;

	IOLockLock(fVideoLock);
	s = fVideoSession;
	if (!fRetired && s && s->owner == owner && s->created && !s->quarantined) {
		if (type == RDN_VIDEO_MEMORY_UPLOAD)
			descriptor = s->upload;
		else if (type == RDN_VIDEO_MEMORY_OUTPUT0 || type == RDN_VIDEO_MEMORY_OUTPUT1)
			descriptor = s->slot[type - RDN_VIDEO_MEMORY_OUTPUT0];
		if (descriptor) {
			*options = (*options & ~kIOMapCacheMask) | kIOMapDefaultCache;
			if (type != RDN_VIDEO_MEMORY_UPLOAD)
				*options |= kIOMapReadOnly;
			descriptor->retain();
			*memory = descriptor;
			result = kIOReturnSuccess;
		} else {
			result = kIOReturnBadArgument;
		}
	}
	IOLockUnlock(fVideoLock);
	return result;
}

/*
 * prepareToRetire, before anything else: no new sessions, and the one there
 * is ends while the decoder still runs (its destroy message, the idle work,
 * its GART entries and VRAM back).
 */
void FirebrandAccel::videoRetire(void)
{
	if (!fVideoLock)
		return;
	IOLockLock(fVideoLock);
	fVideoOffered = false;
	if (fVideoSession)
		videoCleanup(true, true);
	IOLockUnlock(fVideoLock);
}

/* retire(): the engines are halted, BAR0 unmapped; what a quarantined session kept can go now */
void FirebrandAccel::videoReleaseAll(void)
{
	if (!fVideoLock)
		return;
	IOLockLock(fVideoLock);
	fVideoHalted = true;
	if (fVideoSession)
		videoCleanup(false, false);
	IOLockUnlock(fVideoLock);
}

IOReturn FirebrandAccel::newVideoClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
	FirebrandVideoClient *client;

	if (fRetired)
		return kIOReturnNotReady;
	client = new FirebrandVideoClient;
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
