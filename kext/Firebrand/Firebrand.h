/*
 * Firebrand: the IOKit side of the driver for AMD GPUs of the amdgpu era on
 * Mac OS X 10.5.8 (PowerPC). The hardware library is in
 * ../../shared (taken from RadeonNI) and ../../amdgpu (ported from Linux
 * amdgpu, one IP block per file); FirebrandCore owns the card.
 *
 * This class is the default personality: an IOService in its own match
 * category, so it never keeps IONDRVSupport, RadeonNI or anything else off
 * the card. By default it only reads (stage "survey"). From stage "dce" it
 * can light a DisplayPort monitor with a test pattern (frbctl mode /
 * FirebrandDisplay). With FirebrandFramebuffer / frb_fb=1 it steps aside
 * for FirebrandFramebuffer, the IOFramebuffer. With FirebrandAccel /
 * frb_accel=1 it creates a headless accelerator (FirebrandAccel.h): the
 * clients' interface without a window server, and unloadable, unlike a
 * framebuffer the window server has opened. README.md has the switches.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FIREBRAND_H
#define FIREBRAND_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

#include "FirebrandCore.h"

class FirebrandAccel;

class Firebrand : public IOService
{
	OSDeclareDefaultStructors(Firebrand)

public:
	virtual IOService *probe(IOService *provider, SInt32 *score);
	virtual bool start(IOService *provider);
	virtual void stop(IOService *provider);
	virtual void free(void);
	virtual bool serializeProperties(OSSerialize *s) const;
	virtual IOReturn newUserClient(task_t task, void *security, UInt32 type,
				       IOUserClient **handler);

	FirebrandCore *core(void) { return &fCore; }

private:
	FirebrandCore fCore;
	FirebrandAccel *fAccel;		/* headless (FirebrandAccel / frb_accel=1), or 0 */
};

/* is this a Polaris the family table knows by id (any revision)? */
bool FirebrandProbeDevice(IOService *provider);
/* the framebuffer switch: FirebrandFramebuffer in `personality`, or frb_fb=1 */
bool FirebrandFramebufferWanted(IOService *personality);
/* the registry view of a personality without its microcode; false: not handled */
bool FirebrandSerializeWithoutFirmware(const IOService *service, OSSerialize *s, bool *ok);
/*
 * a control client on `owner` (Firebrand or FirebrandFramebuffer): type
 * FRB_UC_TYPE ('frb1', administrators only, every method) or
 * FRB_UC_TYPE_STATUS ('frbs', any user, FRB_UC_STATUS alone)
 */
IOReturn FirebrandNewControlClient(IOService *owner, task_t task, void *security, UInt32 type,
				   IOUserClient **handler);

class FirebrandUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(FirebrandUserClient)

public:
	virtual bool initWithTask(task_t owningTask, void *securityID, UInt32 type);
	virtual bool start(IOService *provider);
	virtual IOReturn clientClose(void);
	virtual IOReturn clientDied(void);
	virtual IOExternalMethod *getTargetAndMethodForIndex(IOService **target, UInt32 index);
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
					     IOMemoryDescriptor **memory);

	IOReturn methodInfo(void *out, IOByteCount *outSize);
	IOReturn methodRegRead(UInt32 reg, UInt32 *value);
	IOReturn methodRegWrite(UInt32 reg, UInt32 value);
	IOReturn methodSmcRead(UInt32 addr, UInt32 *value);
	IOReturn methodRunStage(UInt32 stage, SInt32 *result);
	IOReturn methodUnlock(UInt32 on, UInt32 *cap);
	IOReturn methodFirmwareCheck(void *out, IOByteCount *outSize);
	IOReturn methodVramRead(UInt32 offset, UInt32 *value);
	IOReturn methodTrace(UInt32 on, UInt32 *entries);
	IOReturn methodDisplayInfo(void *out, IOByteCount *outSize);
	IOReturn methodEdid(UInt32 conn, void *out, IOByteCount *outSize);
	IOReturn methodDisplayMode(UInt32 conn, UInt32 width, UInt32 height, UInt32 hz, UInt32 max_rate,
				   SInt32 *result);
	IOReturn methodDisplayOff(SInt32 *result);
	IOReturn methodDisplayFill(UInt32 pattern, UInt32 arg0, UInt32 arg1, SInt32 *result);
	IOReturn methodDetect(SInt32 *found);
	IOReturn methodDpcdRead(UInt32 conn, UInt32 addr, UInt32 *value);
	IOReturn methodSinkCrc(UInt32 conn, SInt32 *result, UInt32 *r, UInt32 *g, UInt32 *b);
	IOReturn methodEngineTest(UInt32 engine, UInt32 test, SInt32 *result);
	IOReturn methodFbDetach(SInt32 *result);
	IOReturn methodRecover(UInt32 force, SInt32 *result, UInt32 *hung, UInt32 *after, UInt32 *soft, UInt32 *ms);
	IOReturn methodCursor(UInt32 op, UInt32 x, UInt32 y, SInt32 *result);
	IOReturn methodDpm(UInt32 op, void *out, IOByteCount *outSize);
	IOReturn methodLut(void *out, IOByteCount *outSize);
	IOReturn methodAudio(UInt32 op, UInt32 arg, SInt32 *result, SInt32 *dig, SInt32 *pin, UInt32 *flags);
	IOReturn methodConfig(UInt32 fn, UInt32 offset, UInt32 write, UInt32 value, UInt32 *out);
	IOReturn methodSmn(UInt32 addr, UInt32 write, UInt32 value, UInt32 *out);
	IOReturn methodFunctions(UInt32 op, UInt32 arg, SInt32 *result, UInt32 *hdr, UInt32 *id1, UInt32 *nubs);
	IOReturn methodLutSet(UInt32 op, SInt32 *result);
	IOReturn methodStatus(void *out, IOByteCount *outSize);

private:
	FirebrandCore *fCore;
	bool fStatusOnly;		/* an 'frbs' client: FRB_US_STATUS (the status) and nothing else */
};

#endif
