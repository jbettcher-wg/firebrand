/*
 * Firebrand.kext: see Firebrand.h, FirebrandCore.h, FirebrandFramebuffer.h.
 *
 * SWITCHES (all default-safe):
 *   boot-arg frb_enable=0          do not match at all
 *   FirebrandStage (personality)   stage run at start; 0 = survey (reads only)
 *   boot-arg frb_stage=N           the same, overriding the personality
 *   FirebrandAllowRegWrite / boot-arg frb_regwrite=1   frbctl wr is refused without it
 *   FirebrandBringUp / boot-arg frb_bringup=1  stages past the card row's verified one (new cards)
 *   FirebrandAdopt / boot-arg frb_adopt=1      go on with a card an earlier load already brought
 *                                  up in this boot (SMC running) instead of asking for a reboot
 *   FirebrandDisplay / boot-arg frb_display=1  after stage dce, light the first DP monitor at its
 *                                  preferred mode with a test pattern (FirebrandMaxLinkRate caps it)
 *   FirebrandFramebuffer / boot-arg frb_fb=1   be the card's IOFramebuffer (FirebrandFramebuffer
 *                                  personality); the control personality then steps aside
 *   FirebrandAccel / boot-arg frb_accel=1      the accelerator (FirebrandAccel.h) once the stages
 *                                  reached sdma: under FirebrandFramebuffer for the window server and
 *                                  OpenGL, under this personality headless; moves VMID 0's GART
 *                                  below 4 GB (RadeonNI's legacy clients take its start as 32 bits)
 *   frbctl stage N                 run the stages up to N on a loaded driver
 *   frbctl mode/off/fill/detect    the display, by hand (stage dce done)
 * A card's family-table row caps every stage request (frb_drv.c).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#include "Firebrand.h"
#include "FirebrandFramebuffer.h"
#include "FirebrandAccel.h"

#define super IOService
OSDefineMetaClassAndStructors(Firebrand, IOService)

/* --- shared by both personalities ------------------------------------------ */

bool FirebrandProbeDevice(IOService *provider)
{
	IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, provider);
	int arg;

	if (PE_parse_boot_arg("frb_enable", &arg) && !arg)
		return false;
	return dev && dev->configRead16(kIOPCIConfigVendorID) == 0x1002 &&
	       frb_asic_type_of(dev->configRead16(kIOPCIConfigDeviceID)) != CHIP_UNKNOWN_FRB;
}

bool FirebrandFramebufferWanted(IOService *personality)
{
	int arg;

	if (PE_parse_boot_arg("frb_fb", &arg))
		return arg != 0;
	return personality && personality->getProperty("FirebrandFramebuffer") == kOSBooleanTrue;
}

/* RadeonNI measured 3 s per ioreg with the microcode in the registry. */
bool FirebrandSerializeWithoutFirmware(const IOService *service, OSSerialize *s, bool *ok)
{
	OSDictionary *table = service->getPropertyTable(), *copy;

	if (!table || !table->getObject("Firmware"))
		return false;
	copy = OSDictionary::withDictionary(table, 0);
	if (!copy)
		return false;
	copy->removeObject("Firmware");
	*ok = copy->serialize(s);
	copy->release();
	return true;
}

IOReturn FirebrandNewControlClient(IOService *owner, task_t task, void *security, UInt32 type,
				   IOUserClient **handler)
{
	FirebrandUserClient *client;

	if (type != FRB_UC_TYPE)
		return kIOReturnUnsupported;
	if (IOUserClient::clientHasPrivilege(security, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
		return kIOReturnNotPrivileged;
	client = new FirebrandUserClient;
	if (!client)
		return kIOReturnNoMemory;
	if (!client->initWithTask(task, security, type) || !client->attach(owner)) {
		client->release();
		return kIOReturnError;
	}
	if (!client->start(owner)) {
		client->detach(owner);
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}

/* --- the control personality ----------------------------------------------- */

IOService *Firebrand::probe(IOService *provider, SInt32 *score)
{
	if (!FirebrandProbeDevice(provider))
		return 0;
	/* the framebuffer personality owns the card instead */
	if (FirebrandFramebufferWanted(this)) {
		IOLog("Firebrand: FirebrandFramebuffer asked for; the control personality stands aside\n");
		return 0;
	}
	return super::probe(provider, score);
}

/* No framebuffer here: the headless accelerator keeps this much VRAM above the surface's start for frbctl mode. */
#define FRB_HEADLESS_SCANOUT_BYTES	(36u << 20)	/* 4096x2160, 32 bpp */

bool Firebrand::start(IOService *provider)
{
	IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, provider);

	if (!super::start(provider))
		return false;
	if (!fCore.start(this, dev)) {
		super::stop(provider);
		return false;
	}
	registerService();
	if (fCore.accelWanted()) {
		UInt32 scanoutEnd = (fCore.surfaceOffset() + FRB_HEADLESS_SCANOUT_BYTES + 0xfffff) & ~0xfffffu;

		fAccel = FirebrandAccel::withOwner(this, &fCore, 0, dev, scanoutEnd);
		if (fAccel)
			fCore.setScanoutLimit(scanoutEnd);
	}
	return true;
}

/* The accelerator drains and unpublishes, the engines halt, then the accelerator lets go of its memory. */
void Firebrand::stop(IOService *provider)
{
	if (fAccel)
		fAccel->prepareToRetire();
	fCore.stop();
	if (fAccel) {
		fAccel->retire();
		fAccel->release();
		fAccel = 0;
	}
	super::stop(provider);
}

void Firebrand::free(void)
{
	fCore.free();
	super::free();
}

bool Firebrand::serializeProperties(OSSerialize *s) const
{
	bool ok;

	if (FirebrandSerializeWithoutFirmware(this, s, &ok))
		return ok;
	return super::serializeProperties(s);
}

IOReturn Firebrand::newUserClient(task_t task, void *security, UInt32 type, IOUserClient **handler)
{
	return FirebrandNewControlClient(this, task, security, type, handler);
}

/* --- the user client ------------------------------------------------------- */

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(FirebrandUserClient, IOUserClient)

bool FirebrandUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	return super::initWithTask(owningTask, securityID, type);
}

bool FirebrandUserClient::start(IOService *provider)
{
	Firebrand *control = OSDynamicCast(Firebrand, provider);
	FirebrandFramebuffer *fb = OSDynamicCast(FirebrandFramebuffer, provider);

	fCore = control ? control->core() : fb ? fb->core() : 0;
	return fCore && super::start(provider);
}

IOReturn FirebrandUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn FirebrandUserClient::clientDied(void)
{
	return clientClose();
}

IOExternalMethod *FirebrandUserClient::getTargetAndMethodForIndex(IOService **target, UInt32 index)
{
	static const IOExternalMethod methods[FRB_UC_METHOD_COUNT] = {
		{ 0, (IOMethod)&FirebrandUserClient::methodInfo,
		  kIOUCScalarIStructO, 0, sizeof(struct frb_user_info) },
		{ 0, (IOMethod)&FirebrandUserClient::methodRegRead, kIOUCScalarIScalarO, 1, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodRegWrite, kIOUCScalarIScalarO, 2, 0 },
		{ 0, (IOMethod)&FirebrandUserClient::methodSmcRead, kIOUCScalarIScalarO, 1, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodRunStage, kIOUCScalarIScalarO, 1, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodVramRead, kIOUCScalarIScalarO, 1, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodTrace, kIOUCScalarIScalarO, 1, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDisplayInfo,
		  kIOUCScalarIStructO, 0, sizeof(struct frb_user_display) },
		{ 0, (IOMethod)&FirebrandUserClient::methodEdid, kIOUCScalarIStructO, 1, FRB_EDID_BYTES },
		{ 0, (IOMethod)&FirebrandUserClient::methodDisplayMode, kIOUCScalarIScalarO, 5, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDisplayOff, kIOUCScalarIScalarO, 0, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDisplayFill, kIOUCScalarIScalarO, 3, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDetect, kIOUCScalarIScalarO, 0, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDpcdRead, kIOUCScalarIScalarO, 2, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodSinkCrc, kIOUCScalarIScalarO, 1, 4 },
		{ 0, (IOMethod)&FirebrandUserClient::methodEngineTest, kIOUCScalarIScalarO, 2, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodFbDetach, kIOUCScalarIScalarO, 0, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodRecover, kIOUCScalarIScalarO, 1, 5 },
		{ 0, (IOMethod)&FirebrandUserClient::methodCursor, kIOUCScalarIScalarO, 3, 1 },
		{ 0, (IOMethod)&FirebrandUserClient::methodDpm, kIOUCScalarIStructO, 1, sizeof(struct frb_user_dpm) },
		{ 0, (IOMethod)&FirebrandUserClient::methodLut, kIOUCScalarIStructO, 0, sizeof(struct frb_user_lut) },
		{ 0, (IOMethod)&FirebrandUserClient::methodAudio, kIOUCScalarIScalarO, 2, 4 },
	};

	if (index >= FRB_UC_METHOD_COUNT)
		return 0;
	*target = this;
	return (IOExternalMethod *)&methods[index];
}

IOReturn FirebrandUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
						  IOMemoryDescriptor **memory)
{
	IOMemoryDescriptor *desc = fCore ? fCore->ucMemory(type) : 0;

	if (!desc)
		return kIOReturnBadArgument;
	desc->retain();
	if (options)
		*options |= kIOMapReadOnly;
	*memory = desc;
	return kIOReturnSuccess;
}

IOReturn FirebrandUserClient::methodInfo(void *out, IOByteCount *outSize)
{
	if (*outSize < sizeof(struct frb_user_info))
		return kIOReturnBadArgument;
	*outSize = sizeof(struct frb_user_info);
	return fCore->ucInfo((struct frb_user_info *)out);
}

IOReturn FirebrandUserClient::methodRegRead(UInt32 reg, UInt32 *value)
{
	return fCore->ucRegRead(reg, value);
}

IOReturn FirebrandUserClient::methodRegWrite(UInt32 reg, UInt32 value)
{
	return fCore->ucRegWrite(reg, value);
}

IOReturn FirebrandUserClient::methodSmcRead(UInt32 addr, UInt32 *value)
{
	return fCore->ucSmcRead(addr, value);
}

IOReturn FirebrandUserClient::methodRunStage(UInt32 stage, SInt32 *result)
{
	return fCore->ucRunStage(stage, result);
}

IOReturn FirebrandUserClient::methodVramRead(UInt32 offset, UInt32 *value)
{
	return fCore->ucVramRead(offset, value);
}

IOReturn FirebrandUserClient::methodTrace(UInt32 on, UInt32 *entries)
{
	return fCore->ucTrace(on, entries);
}

IOReturn FirebrandUserClient::methodDisplayInfo(void *out, IOByteCount *outSize)
{
	if (*outSize < sizeof(struct frb_user_display))
		return kIOReturnBadArgument;
	*outSize = sizeof(struct frb_user_display);
	return fCore->ucDisplayInfo((struct frb_user_display *)out);
}

IOReturn FirebrandUserClient::methodEdid(UInt32 conn, void *out, IOByteCount *outSize)
{
	return fCore->ucEdid(conn, out, outSize);
}

IOReturn FirebrandUserClient::methodDisplayMode(UInt32 conn, UInt32 width, UInt32 height, UInt32 hz,
						UInt32 max_rate, SInt32 *result)
{
	return fCore->ucDisplayMode(conn, width, height, hz, max_rate, result);
}

IOReturn FirebrandUserClient::methodDisplayOff(SInt32 *result)
{
	return fCore->ucDisplayOff(result);
}

IOReturn FirebrandUserClient::methodDisplayFill(UInt32 pattern, UInt32 arg0, UInt32 arg1, SInt32 *result)
{
	return fCore->ucDisplayFill(pattern, arg0, arg1, result);
}

IOReturn FirebrandUserClient::methodDetect(SInt32 *found)
{
	return fCore->ucDetect(found);
}

IOReturn FirebrandUserClient::methodDpcdRead(UInt32 conn, UInt32 addr, UInt32 *value)
{
	return fCore->ucDpcdRead(conn, addr, value);
}

IOReturn FirebrandUserClient::methodSinkCrc(UInt32 conn, SInt32 *result, UInt32 *r, UInt32 *g, UInt32 *b)
{
	return fCore->ucSinkCrc(conn, result, r, g, b);
}

IOReturn FirebrandUserClient::methodEngineTest(UInt32 engine, UInt32 test, SInt32 *result)
{
	return fCore->ucEngineTest(engine, test, result);
}

IOReturn FirebrandUserClient::methodFbDetach(SInt32 *result)
{
	return fCore->ucFbDetach(result);
}

IOReturn FirebrandUserClient::methodCursor(UInt32 op, UInt32 x, UInt32 y, SInt32 *result)
{
	return fCore->ucCursor(op, (SInt32)x, (SInt32)y, result);
}

IOReturn FirebrandUserClient::methodAudio(UInt32 op, UInt32 arg, SInt32 *result, SInt32 *dig, SInt32 *pin,
					   UInt32 *flags)
{
	return fCore->ucAudio(op, (SInt32)arg, result, dig, pin, flags);
}

IOReturn FirebrandUserClient::methodLut(void *out, IOByteCount *outSize)
{
	if (*outSize < sizeof(struct frb_user_lut))
		return kIOReturnBadArgument;
	*outSize = sizeof(struct frb_user_lut);
	return fCore->ucLut((struct frb_user_lut *)out);
}

IOReturn FirebrandUserClient::methodDpm(UInt32 op, void *out, IOByteCount *outSize)
{
	if (*outSize < sizeof(struct frb_user_dpm))
		return kIOReturnBadArgument;
	*outSize = sizeof(struct frb_user_dpm);
	return fCore->ucDpm(op, (struct frb_user_dpm *)out);
}

IOReturn FirebrandUserClient::methodRecover(UInt32 force, SInt32 *result, UInt32 *hung, UInt32 *after,
					    UInt32 *soft, UInt32 *ms)
{
	return fCore->ucRecover(force, result, hung, after, soft, ms);
}
