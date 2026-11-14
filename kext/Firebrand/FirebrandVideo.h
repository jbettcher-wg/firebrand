/*
 * The 'rdnv' video client on FirebrandAccel: RadeonNI's bounded H.264
 * decode protocol (RadeonNI's hw/rdn_video.h, RadeonNIVideo.cpp,
 * RadeonNIUvdSession.cpp), unchanged at the interface, so the client
 * library FrostByte compiles in (RadeonNI's video/rdnvd_rdnv.c) and
 * its rdnvdh264dec element work as they do on RadeonNI. Underneath, UVD 6.3
 * (amdgpu/frb_uvd_v6_0.c) and the Polaris messages (amdgpu/frb_uvd_h264.c).
 *
 * What changes under the protocol (FirebrandVideo.cpp has the details):
 *  - the decode target is the output slot itself: two buffers in system
 *    memory, bound into VMID 0's GART, which UVD writes and the client maps
 *    read-only (RadeonNI decoded into VRAM and copied each picture out with
 *    CP DMA). A page of guard bytes on either side, checked every picture.
 *  - messages, feedback, the IT scaling table, the IB and the bitstream are
 *    in GTT too, written by the CPU through its cache (snooped);
 *  - the session context, the macroblock context and the DPB are one VRAM
 *    allocation (the invisible heap when it has room), cleared by SDMA1
 *    before the create message.
 *  - no GPU-output lease (DECODE_GPU/ACQUIRE_GPU/RETIRE_GPU: unsupported,
 *    and QUERY never offers RDN_VIDEO_CAP_GPU_OUTPUT).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_VIDEO_H
#define FIREBRAND_VIDEO_H

#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
#include "rdn_video.h"
#include "frb_uvd_h264.h"
}

class FirebrandAccel;

/*
 * Kernel pages a client maps: RadeonNIVideoMemory. Owns its buffer (freed
 * once every mapping is gone, so a client that outlives its session never
 * maps freed pages), charged against the accelerator's 20 MB video quota,
 * mappable only with the default cache mode and, when read-only, only
 * read-only (Leopard grants write access to a mapping that asks for a
 * cache-mode change).
 */
class FirebrandVideoMemory : public IOGeneralMemoryDescriptor
{
	OSDeclareDefaultStructors(FirebrandVideoMemory)

public:
	static FirebrandVideoMemory *withBytes(FirebrandAccel *owner, UInt32 bytes, bool readOnly);
	void *bytes(void) { return fBuffer; }
	UInt32 length(void) { return fBytes; }
	virtual IOReturn doMap(vm_map_t map, IOVirtualAddress *address, IOOptionBits options,
			       IOByteCount offset, IOByteCount length);
	virtual void free(void);

private:
	FirebrandAccel *fOwner;
	void *fBuffer;
	UInt32 fBytes;
	bool fReadOnly;
	bool fPrepared;
};

class FirebrandVideoClient : public IOUserClient
{
	OSDeclareDefaultStructors(FirebrandVideoClient)

public:
	virtual bool initWithTask(task_t task, void *securityID, UInt32 type);
	virtual bool start(IOService *provider);
	virtual IOReturn clientClose(void);
	virtual IOReturn clientDied(void);
	virtual void stop(IOService *provider);
	virtual void free(void);
	virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory);

	/* hw/rdn_video.h, in its order */
	IOReturn methodQuery(rdn_video_query *out, IOByteCount *size);
	IOReturn methodCreate(const rdn_video_create *in, rdn_video_session *out, IOByteCount inSize,
			      IOByteCount *outSize);
	IOReturn methodDecode(const rdn_video_decode *in, rdn_video_output *out, IOByteCount inSize,
			      IOByteCount *outSize);
	IOReturn methodRelease(const rdn_video_release *in, IOByteCount size);
	IOReturn methodDestroy(const rdn_video_destroy *in, IOByteCount size);
	/* the GPU-output lease (DECODE_GPU, ACQUIRE_GPU, RETIRE_GPU): not on Firebrand */
	IOReturn methodUnsupported(void);

private:
	FirebrandAccel *fAccel;
	IOLock *fClientLock;
	bool fClosed, fCreated, fAdministrator;
};

#endif
