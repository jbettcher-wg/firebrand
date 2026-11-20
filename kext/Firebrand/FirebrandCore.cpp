/*
 * FirebrandCore: see FirebrandCore.h. The hardware side of Firebrand.kext,
 * moved out of the Firebrand class unchanged except that the personality's
 * properties are reached through the owning service.
 *
 * Pieces taken from RadeonNI (RadeonNI's kext/RadeonNI/RadeonNI.cpp)
 * and renamed: the OS layer callbacks, register access with its ordering
 * (sync before a store, eieio after), the three VBIOS sources (the ROM BAR
 * through ml_probe_read, the bridge's rom-image property, a VBIOS property
 * in the personality), BAR0 above 4 GB through QorIQPCIBridge, and the GART
 * dummy page's bus address (RadeonNIAccel.cpp gartBusOf).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/IOCatalogue.h>

#include "FirebrandCore.h"
extern "C" {
#include "frb_ring.h"
#include "frb_pm.h"
#include "frb_splash.h"
#include "frb_bo.h"
#include "frb_uvd.h"
#include "frb_mode.h"
}
#include <mach/kmod.h>
#ifdef FRB_HAVE_BUILD_H
#include "frb_build.h"	/* FRB_BUILD_ID: the source the build came from, when the build writes it */
#endif
#ifndef FRB_BUILD_ID
#define FRB_BUILD_ID	""
#endif

/* kmod_info.c: the kext's version ("0.1.0") */
extern "C" kmod_info_t kmod_info;

extern "C" {
/* osfmk/ppc/machine_routines.h; exported, but not in Kernel.framework. */
boolean_t ml_probe_read(vm_offset_t paddr, unsigned int *val);
}


#define FRB_TRACE_ENTRIES	(64 * 1024)	/* 512 KB: survey + POST + GMC is about 23k */
#define FRB_LOG_BYTES		(64 * 1024)

/* RadeonNIAccel.cpp: inbound ATMU window 2, PCI 0x10_0000_0000 -> CPU 0 */
#define FRB_PCI_INBOUND_BASE	0x1000000000ULL
#define FRB_PCI_INBOUND_SIZE	(1ULL << 36)

/* --- a sub-range of BAR0 (FirebrandCore.h) --------------------------------- */

#define super IOSubMemoryDescriptor
OSDefineMetaClassAndStructors(FirebrandSubDeviceMemory, IOSubMemoryDescriptor)

IODeviceMemory *FirebrandSubDeviceMemory::withDeviceSubRange(IODeviceMemory *parent, IOByteCount offset,
							     IOByteCount length)
{
	FirebrandSubDeviceMemory *me;

	if (!parent)
		return 0;
	me = new FirebrandSubDeviceMemory;
	if (!me)
		return 0;
	if (!me->initSubRange(parent, offset, length, kIODirectionNone)) {
		me->release();
		return 0;
	}
	return (IODeviceMemory *)me;
}

IOPhysicalAddress FirebrandSubDeviceMemory::getPhysicalSegment(IOByteCount offset, IOByteCount *length)
{
	IOByteCount actualLength = 0, segLen;
	addr64_t pa;

	if (length)
		*length = 0;
	if (offset >= _length || !_parent)
		return 0;
	pa = _parent->getPhysicalSegment64(offset + _start, &actualLength);
	if (!pa)
		return 0;
	segLen = (_length - offset < actualLength) ? (_length - offset) : actualLength;
	if (length)
		*length = segLen;
	if (pa + segLen <= 0x100000000ULL)
		return (IOPhysicalAddress)pa;
	return (IOPhysicalAddress)(0x80000000u + _start + offset);
}

IOMemoryMap *FirebrandSubDeviceMemory::makeMapping(IOMemoryDescriptor *owner, task_t intoTask,
						   IOVirtualAddress atAddress, IOOptionBits options,
						   IOByteCount offset, IOByteCount length)
{
	if ((options & kIOMapUnique) && _parent) {
		IOByteCount segLen = 0;
		addr64_t pa = _parent->getPhysicalSegment64(_start + offset, &segLen);

		if (pa + length > 0x100000000ULL)
			options &= ~(IOOptionBits)(kIOMapUnique | kIOMapReference);
	}
	return super::makeMapping(owner, intoTask, atAddress, options, offset, length);
}

#undef super

/* --- the OS layer ---------------------------------------------------------- */

static uint32_t os_mmio_read32(void *cookie, uint32_t offset)
{
	return ((FirebrandCore *)cookie)->readReg(offset);
}

static void os_mmio_write32(void *cookie, uint32_t offset, uint32_t value)
{
	((FirebrandCore *)cookie)->writeReg(offset, value);
}

static void os_doorbell_write64(void *cookie, uint32_t index, uint64_t value)
{
	((FirebrandCore *)cookie)->writeDoorbell64(index, value);
}

static uint32_t os_cfg_read32(void *cookie, uint32_t offset)
{
	return ((FirebrandCore *)cookie)->device()->configRead32(offset);
}

static void os_cfg_write32(void *cookie, uint32_t offset, uint32_t value)
{
	((FirebrandCore *)cookie)->device()->configWrite32(offset, value);
}

static void os_delay_us(void *cookie, uint32_t usec)
{
	/* Short waits spin; long ones give up the CPU. */
	if (usec < 1000)
		IODelay(usec);
	else
		IOSleep((usec + 999) / 1000);
}

static uint64_t os_time_ms(void *cookie)
{
	uint64_t abstime, nanos;

	clock_get_uptime(&abstime);
	absolutetime_to_nanoseconds(abstime, &nanos);
	return nanos / 1000000ULL;
}

/* IOFree needs the size back, so it is kept in front of the block. */
static void *os_alloc(void *cookie, size_t size)
{
	UInt32 *block = (UInt32 *)IOMalloc(size + 8);

	if (!block)
		return 0;
	bzero(block, size + 8);
	block[0] = size + 8;
	return block + 2;
}

static void os_free(void *cookie, void *ptr)
{
	UInt32 *block = (UInt32 *)ptr - 2;

	IOFree(block, block[0]);
}

static int os_firmware(void *cookie, const char *name, const uint8_t **data, uint32_t *size)
{
	return ((FirebrandCore *)cookie)->firmware(name, data, size);
}

static int os_gtt_alloc(void *cookie, uint32_t size, struct frb_gtt *gtt)
{
	return ((FirebrandCore *)cookie)->gttAlloc(size, gtt);
}

static void os_gtt_free(void *cookie, struct frb_gtt *gtt)
{
	((FirebrandCore *)cookie)->gttFree(gtt);
}

static void os_log(void *cookie, enum frb_log_level level, const char *fmt, va_list ap)
{
	char line[256];

	if (level == FRB_LOG_DEBUG)
		return;
	vsnprintf(line, sizeof(line), fmt, ap);
	IOLog("Firebrand: %s\n", line);
	((FirebrandCore *)cookie)->logLine(line);
	if (level == FRB_LOG_ERROR)
		((FirebrandCore *)cookie)->errorLine(line);
}

void FirebrandCore::errorLine(const char *line)
{
	char *d = fErrLines[fErrCount % 4];

	strncpy(d, line, sizeof(fErrLines[0]) - 1);
	d[sizeof(fErrLines[0]) - 1] = 0;
	fErrCount++;
}

void FirebrandCore::logLine(const char *line)
{
	char *buf;
	UInt32 n, cap;

	if (!fLogMem)
		return;
	buf = (char *)fLogMem->getBytesNoCopy();
	cap = (UInt32)fLogMem->getLength();
	n = strlen(line);
	if (n + 2 > cap)
		return;
	if (fLogLen + n + 1 >= cap) {
		/* keep the newer half */
		bcopy(buf + cap / 2, buf, cap / 2);
		fLogLen = fLogLen > cap / 2 ? fLogLen - cap / 2 : 0;
	}
	bcopy(line, buf + fLogLen, n);
	fLogLen += n;
	buf[fLogLen++] = '\n';
	buf[fLogLen] = 0;
}

/*
 * The card's registers are little-endian and the CPU is not. A store is
 * preceded by sync (it must not pass earlier stores, VRAM included: the
 * GPU may act on it) and followed by eieio, which keeps a following load
 * from overtaking it: the SMC/PCIE/MC index-then-data reads depend on that
 * (the sipeek defect the E9173 capture found was exactly a missing barrier).
 */
UInt32 FirebrandCore::readReg(UInt32 offset)
{
	return OSReadLittleInt32(fRegs, offset);
}

void FirebrandCore::writeReg(UInt32 offset, UInt32 value)
{
	__asm__ __volatile__("sync" : : : "memory");
	OSWriteLittleInt32(fRegs, offset, value);
	OSSynchronizeIO();
}

/*
 * A SOC15 doorbell (amdgpu's WDOORBELL64): the ring's packets and its WPTR
 * writeback went out through BAR0 and the HDP was flushed before this
 * (frb_ring.c), so a sync first orders them ahead of the doorbell. The
 * 32-bit kernel has no 64-bit store to a device, so the value goes out as
 * two little-endian dwords, the low one first (the lo-hi order of Linux's
 * io-64-nonatomic-lo-hi): the engines' write pointers stay far below 4 GB,
 * so the high dword is 0 either way. eieio keeps a later register read from
 * passing them.
 */
void FirebrandCore::writeDoorbell64(UInt32 index, UInt64 value)
{
	if (!fDoorbells || (index + 2) * 4 > fDoorbellSize)
		return;
	__asm__ __volatile__("sync" : : : "memory");
	OSWriteLittleInt32(fDoorbells, index * 4, (UInt32)value);
	OSWriteLittleInt32(fDoorbells, index * 4 + 4, (UInt32)(value >> 32));
	OSSynchronizeIO();
}

/*
 * SOC15: BAR2 is the doorbell aperture (2 MB on the MI50; amdgpu_doorbell_init
 * maps it from pci_resource_start(pdev, 2)). soc15_common_hw_init enabled it
 * (BIF_DOORBELL_APER_EN) in stage gmc. Without it, or with FirebrandMMIOWptr,
 * the gfx and SDMA rings take amdgpu's use_doorbell = false arm.
 */
bool FirebrandCore::mapDoorbells(void)
{
	if (fDoorbellMap)
		return true;
	if (fMMIOWptr) {
		IOLog("Firebrand: doorbells off (FirebrandMMIOWptr): the rings write their WPTR registers\n");
		return false;
	}
	fDoorbellMap = fDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
	if (!fDoorbellMap || fDoorbellMap->getLength() < 4096) {
		IOLog("Firebrand: cannot map the doorbell BAR (BAR2); the rings write their WPTR registers\n");
		if (fDoorbellMap) {
			fDoorbellMap->release();
			fDoorbellMap = 0;
		}
		return false;
	}
	fDoorbells = (volatile UInt8 *)fDoorbellMap->getVirtualAddress();
	fDoorbellSize = (UInt32)fDoorbellMap->getLength();
	fOS.doorbell_write64 = os_doorbell_write64;
	IOLog("Firebrand: doorbells: BAR2, %lu KB\n", (unsigned long)(fDoorbellSize >> 10));
	return true;
}

/* --- firmware from the personality ---------------------------------------- */

/* a file of the store, through FirmwareAliases when its bytes are under another name */
OSData *FirebrandCore::firmwareData(const char *name)
{
	OSData *d = fFirmware ? OSDynamicCast(OSData, fFirmware->getObject(name)) : 0;
	OSString *canon;

	if (!d && fFirmware && fFirmwareAliases &&
	    (canon = OSDynamicCast(OSString, fFirmwareAliases->getObject(name))))
		d = OSDynamicCast(OSData, fFirmware->getObject(canon));
	return d && d->getLength() ? d : 0;
}

int FirebrandCore::firmware(const char *name, const uint8_t **data, uint32_t *size)
{
	OSData *d = firmwareData(name);

	if (!d) {
		IOLog("Firebrand: firmware %s is not in the kext (scripts/fetch-firmware.sh, rebuild)\n", name);
		return -ENOENT;
	}
	*data = (const uint8_t *)d->getBytesNoCopy();
	*size = d->getLength();
	return 0;
}

/*
 * The FirebrandFirmware personality: IOProviderClass FirebrandFirmwareStore
 * (no such class, so it never matches) and FirebrandFirmwareStore = this
 * kext's bundle identifier, holding the microcode once for both driver
 * personalities. kextd hands it to the IOCatalogue with the others; the
 * catalogue's findDrivers finds it by that key (IOCatalogue.h, in the 10.5
 * KPI). The dictionaries are retained, so they outlive the personality.
 */
bool FirebrandCore::findFirmwareStore(void)
{
	OSString *id = OSDynamicCast(OSString, fOwner ? fOwner->getProperty("CFBundleIdentifier") : 0);
	OSDictionary *match = OSDictionary::withCapacity(1);
	OSOrderedSet *found = 0;
	OSDictionary *store = 0;
	SInt32 generation = 0;

	if (match && id && match->setObject("FirebrandFirmwareStore", id))
		found = gIOCatalogue->findDrivers(match, &generation);
	if (match)
		match->release();
	if (found && found->getCount())
		store = OSDynamicCast(OSDictionary, found->getFirstObject());
	if (store && OSDynamicCast(OSDictionary, store->getObject("Firmware"))) {
		fFirmware = (OSDictionary *)store->getObject("Firmware");
		fFirmware->retain();
		fFirmwareAliases = OSDynamicCast(OSDictionary, store->getObject("FirmwareAliases"));
		if (fFirmwareAliases)
			fFirmwareAliases->retain();
		fFirmwareFrom = "the FirebrandFirmware personality";
	}
	if (found)
		found->release();
	return fFirmware != 0;
}

/*
 * frbctl firmware: for every device id the driver matches and every
 * revision amdgpu's kicker rules name, every file that card may ask for
 * (frb_fw_names_for: all register outcomes), looked up in the store as the
 * stages would. Reads the kext's own memory only; the card is not touched.
 */
IOReturn FirebrandCore::ucFirmwareCheck(struct frb_user_fwcheck *out)
{
	char names[40][32];
	int i, r, k, n, j;

	bzero(out, sizeof(*out));
	out->version = 1;
	out->store_files = fFirmware ? fFirmware->getCount() : 0;
	out->store_aliases = fFirmwareAliases ? fFirmwareAliases->getCount() : 0;
	strncpy(out->source, fFirmwareFrom ? fFirmwareFrom : "none", sizeof(out->source) - 1);
	for (i = 0; i < frb_pciid_count(); i++) {
		uint16_t device = frb_pciid_device(i);

		out->devices++;
		for (r = 0; r < frb_fw_revision_count; r++) {
			enum amd_asic_type t = frb_asic_type_of_rev(device, frb_fw_revisions[r]);

			if (!frb_asic_ported(t))
				continue;
			n = frb_fw_names_for(t, device, frb_fw_revisions[r], names, 40);
			out->lookups += n;
			for (k = 0; k < n; k++) {
				if (firmwareData(names[k]))
					continue;
				out->missing++;
				for (j = 0; j < (int)out->missing_listed; j++)
					if (!strncmp(out->missing_names[j], names[k], sizeof(out->missing_names[j])))
						break;
				if (j == (int)out->missing_listed && out->missing_listed < 8)
					strncpy(out->missing_names[out->missing_listed++], names[k],
						sizeof(out->missing_names[0]) - 1);
			}
		}
	}
	/* the verified cards' own files, by the choices their stages make */
	for (i = 0; i < frb_verified_row_count(); i++) {
		const struct frb_asic_row *row = frb_verified_row(i);

		n = frb_fw_names_for(row->asic_type, row->device, row->revision, names, 40);
		for (k = 0; k < n; k++)
			if (!firmwareData(names[k]))
				out->verified_missing++;
		out->verified_rows++;
	}
	return kIOReturnSuccess;
}

/* --- the VBIOS (RadeonNI's three sources) ---------------------------------- */

bool FirebrandCore::biosFromRom(void)
{
	IODeviceMemory *mem;
	const char *why = 0;
	UInt8 *image = 0;
	UInt32 bar, phys, size = 0, i;
	unsigned int word = 0;
	UInt8 sum = 0;

	bar = fDevice->configRead32(kIOPCIConfigExpansionROMBase);
	mem = fDevice->getDeviceMemoryWithRegister(kIOPCIConfigExpansionROMBase);
	if (!(bar & 0xfffff800) || !mem) {
		IOLog("Firebrand: the firmware gave the card's ROM no address\n");
		return false;
	}
	phys = mem->getPhysicalAddress();
	fDevice->configWrite32(kIOPCIConfigExpansionROMBase, bar | 1);
	IODelay(1000);

	if (!ml_probe_read(phys, &word)) {
		why = "does not answer";
	} else if ((word >> 16) != 0x55aa) {
		why = "has no option ROM signature";
	} else {
		size = ((word >> 8) & 0xff) * 512;
		if (!size || size > mem->getLength())
			why = "states a length that does not fit its BAR";
		else if (!(image = (UInt8 *)IOMalloc(size)))
			why = "could not be copied (no memory)";
	}
	for (i = 0; !why && i < size; i += 4) {
		if (!ml_probe_read(phys + i, &word)) {
			why = "stopped answering";
			break;
		}
		image[i] = word >> 24;
		image[i + 1] = word >> 16;
		image[i + 2] = word >> 8;
		image[i + 3] = word;
		sum += image[i] + image[i + 1] + image[i + 2] + image[i + 3];
	}
	if (!why && sum)
		why = "has a wrong checksum";
	fDevice->configWrite32(kIOPCIConfigExpansionROMBase, bar);
	if (why) {
		IOLog("Firebrand: the card's ROM at %08lx %s\n", (unsigned long)phys, why);
		if (image)
			IOFree(image, size);
		return false;
	}
	fBios = image;
	fBiosSize = size;
	IOLog("Firebrand: VBIOS, %lu bytes from the card's ROM\n", (unsigned long)size);
	return true;
}

static bool romImageOk(const UInt8 *bytes, UInt32 len)
{
	return len >= 0x200 && bytes[0] == 0x55 && bytes[1] == 0xaa && len >= (UInt32)bytes[2] * 512;
}

bool FirebrandCore::biosFromProvider(void)
{
	OSData *data = OSDynamicCast(OSData, fDevice->getProperty("rom-image"));

	if (!data || !romImageOk((const UInt8 *)data->getBytesNoCopy(), data->getLength())) {
		IOLog("Firebrand: no usable rom-image property on the provider\n");
		return false;
	}
	fBiosSize = data->getLength();
	fBios = IOMalloc(fBiosSize);
	if (!fBios)
		return false;
	bcopy(data->getBytesNoCopy(), fBios, fBiosSize);
	IOLog("Firebrand: VBIOS, %lu bytes from the provider's rom-image\n", (unsigned long)fBiosSize);
	return true;
}

bool FirebrandCore::biosFromPersonality(void)
{
	OSData *data = OSDynamicCast(OSData, fOwner->getProperty("VBIOS"));

	if (!data || !romImageOk((const UInt8 *)data->getBytesNoCopy(), data->getLength()))
		return false;
	fBiosSize = data->getLength();
	fBios = IOMalloc(fBiosSize);
	if (!fBios)
		return false;
	bcopy(data->getBytesNoCopy(), fBios, fBiosSize);
	IOLog("Firebrand: VBIOS, %lu bytes from the personality\n", (unsigned long)fBiosSize);
	return true;
}

/* Most authoritative first; an image the AtomBIOS parser refuses falls through. */
bool FirebrandCore::loadBios(void)
{
	int source;

	for (source = 0; source < 3; source++) {
		bool got;

		switch (source) {
		case 0:		got = biosFromRom();		break;
		case 1:		got = biosFromProvider();	break;
		default:	got = biosFromPersonality();	break;
		}
		if (!got)
			continue;
		fAdev.bios = (uint8_t *)fBios;
		fAdev.bios_size = fBiosSize;
		if (!frb_atombios_init(&fAdev))
			return true;
		IOLog("Firebrand: that image is not a usable AtomBIOS\n");
		frb_atombios_fini(&fAdev);
		IOFree(fBios, fBiosSize);
		fBios = 0;
		fAdev.bios = 0;
	}
	return false;
}

/* --- BAR0 and the dummy page (needed from stage GMC on) -------------------- */

#define FRB_APER_MAX	(256u << 20)	/* the CPU window into VRAM: frb_gmc_v9_0_layout's and the replays' */

bool FirebrandCore::mapAperture(void)
{
	IODeviceMemory *bar, *window;
	IOByteCount length;

	if (fFbMap)
		return true;
	/*
	 * The CPU window into VRAM is at most FRB_APER_MAX from BAR0's start,
	 * whatever size the BAR has (a resizable BAR may be 16 GB on Vega 20;
	 * the bridge publishes at most 256 MB of it today, but a 32-bit kernel
	 * map could not take the whole of a large one anyway). Nothing the GPU
	 * is programmed with depends on the BAR's size: offset 0 of BAR0 is
	 * VRAM offset 0, and gmc's layout sits at the top of this window.
	 */
	bar = fDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar || !(length = bar->getLength())) {
		IOLog("Firebrand: BAR0 has no device memory\n");
		return false;
	}
	if (length > FRB_APER_MAX) {
		window = IODeviceMemory::withSubRange(bar, 0, FRB_APER_MAX);
		fFbMap = window ? window->map() : 0;
		if (window)
			window->release();
	} else
		fFbMap = bar->map();
	if (!fFbMap) {
		IOLog("Firebrand: cannot map BAR0\n");
		return false;
	}
	fAdev.aper = (volatile uint8_t *)fFbMap->getVirtualAddress();
	fAdev.aper_size = (uint32_t)fFbMap->getLength();
	IOLog("Firebrand: BAR0 mapped, %lu MB%s\n", (unsigned long)(fAdev.aper_size >> 20),
	      length > FRB_APER_MAX ? " of a larger BAR (the CPU window's cap)" : "");
	return true;
}

/*
 * A page of system memory that stays put, as the GART's dummy page and the
 * VM fault default. Its bus address is the CPU address seen through
 * inbound window 2. Bus mastering stays OFF in this round, so the card
 * cannot actually reach it (or anything else in host memory) yet.
 */
bool FirebrandCore::allocDummyPage(void)
{
	IOByteCount length = 0;
	addr64_t phys;
	IOReturn r;

	if (fDummy)
		return true;
	/*
	 * One page-aligned page is one physical page, so this needs no
	 * kIOMemoryPhysicallyContiguous: that allocator stopped giving even one
	 * page after several loads in one boot (2026-11-13), while
	 * ordinary wired pages, as RadeonNI's other buffers use, kept coming.
	 */
	fDummy = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, 4096, 4096);
	if (!fDummy) {
		IOLog("Firebrand: no page for the dummy page\n");
		return false;
	}
	if ((r = fDummy->prepare()) != kIOReturnSuccess) {
		IOLog("Firebrand: dummy page: prepare 0x%x\n", r);
		fDummy->release();
		fDummy = 0;
		return false;
	}
	bzero(fDummy->getBytesNoCopy(), 4096);
	phys = fDummy->getPhysicalSegment64(0, &length);
	if (!phys || (!fDMACmd && phys >= FRB_PCI_INBOUND_SIZE) || length < 4096) {
		IOLog("Firebrand: dummy page at 0x%llx is outside the inbound window\n",
		      (unsigned long long)phys);
		fDummy->complete();
		fDummy->release();
		fDummy = 0;
		return false;
	}
	{
		QorIQDMAWalk *walk = dmaWalk(fDummy);

		fAdev.dummy_page_addr = busOfPage(walk, fDummy, 0);
		if (walk)
			delete walk;
	}
	if (!fAdev.dummy_page_addr) {
		IOLog("Firebrand: dummy page at 0x%llx has no bus address\n", (unsigned long long)phys);
		fDummy->complete();
		fDummy->release();
		fDummy = 0;
		return false;
	}
	IOLog("Firebrand: dummy page: CPU 0x%llx, bus 0x%llx\n", (unsigned long long)phys,
	      (unsigned long long)fAdev.dummy_page_addr);
	return true;
}

/* --- bus addresses (FirebrandCore.h) ---------------------------------------- */

/* The GPU's VM takes a 40-bit system address (RadeonNIAccel.cpp's too). */
#define FRB_BUS_ADDRESS_BITS	40

QorIQDMAWalk *FirebrandCore::dmaWalk(IOMemoryDescriptor *md)
{
	QorIQDMAWalk *walk;

	if (!fDMACmd)
		return 0;
	walk = new QorIQDMAWalk(md, true, FRB_BUS_ADDRESS_BITS, fDMAAdd, fDMALimit, 4096);
	if (walk && !walk->ready()) {
		delete walk;
		walk = 0;
	}
	return walk;
}

UInt64 FirebrandCore::busOfPage(QorIQDMAWalk *walk, IOMemoryDescriptor *md, UInt64 offset)
{
	IOByteCount len = 0;
	addr64_t phys;

	if (fDMACmd)
		return walk ? walk->page(offset) : 0;
	/* the pre-IODMACommand rule, unchanged */
	phys = md->getPhysicalSegment64((IOByteCount)offset, &len);
	if (!phys || (phys & 0xfff) || len < 4096 || phys >= FRB_PCI_INBOUND_SIZE)
		return 0;
	return (UInt64)phys + FRB_PCI_INBOUND_BASE;
}

void FirebrandCore::logBinding(const char *what, IOMemoryDescriptor *md, UInt64 bus, UInt32 pages)
{
	static SInt32 logged;

	if (OSIncrementAtomic(&logged) >= 8)
		return;
	IOLog("Firebrand: %s: %lu page(s), first CPU 0x%llx -> bus 0x%llx (%s)\n", what, (unsigned long)pages,
	      (unsigned long long)md->getPhysicalSegment64(0, 0), (unsigned long long)bus,
	      fDMACmd ? "IODMACommand" : "physical + window 2");
}

/* --- GTT ------------------------------------------------------------------- */

/*
 * A GTT object, as amdgpu's TTM gives one: wired system pages bound into
 * the GART (VMID 0). Each page is the CPU address the descriptor reports,
 * page by page, plus inbound window 2's base (FRB_PCI_INBOUND_BASE): a
 * snooped window, so the CPU's cached view and the GPU's agree without
 * cache maintenance (RadeonNIAccel.cpp gartBusOf). Page-aligned
 * whole pages, never kIOMemoryPhysicallyContiguous (that allocator runs dry
 * on this kernel). Bus mastering is switched on with the first one.
 */
int FirebrandCore::gttAlloc(uint32_t size, struct frb_gtt *gtt)
{
	uint32_t npages = (size + 4095) / 4096, first = 0, i;
	IOBufferMemoryDescriptor *mem;
	uint64_t *bus;
	int r = 0;

	if (!fAdev.gart.ready || !npages)
		return -ENODEV;
	mem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, npages * 4096, 4096);
	if (!mem)
		return -ENOMEM;
	if (mem->prepare() != kIOReturnSuccess) {
		mem->release();
		return -ENOMEM;
	}
	bus = (uint64_t *)IOMalloc(npages * sizeof(uint64_t));
	if (!bus) {
		mem->complete();
		mem->release();
		return -ENOMEM;
	}
	{
		QorIQDMAWalk *walk = dmaWalk(mem);

		for (i = 0; i < npages && !r; i++) {
			bus[i] = busOfPage(walk, mem, (UInt64)i * 4096);
			if (!bus[i])
				r = -EIO;
		}
		if (walk)
			delete walk;
		if (!r)
			logBinding("GTT", mem, bus[0], npages);
	}
	if (!r)
		r = frb_gart_alloc(&fAdev, npages, &first);
	if (r) {
		IOLog("Firebrand: GTT: %lu pages: %d\n", (unsigned long)npages, r);
		IOFree(bus, npages * sizeof(uint64_t));
		mem->complete();
		mem->release();
		return r;
	}
	busMaster();
	bzero(mem->getBytesNoCopy(), npages * 4096);
	frb_gart_bind(&fAdev, first, npages, bus, frb_gart_gtt_flags(&fAdev));
	IOFree(bus, npages * sizeof(uint64_t));
	gtt->cpu = mem->getBytesNoCopy();
	gtt->gpu_addr = frb_gart_gpu_addr(&fAdev, first);
	gtt->size = npages * 4096;
	gtt->first_page = first;
	gtt->handle = mem;
	return 0;
}

void FirebrandCore::busMaster(void)
{
	if (fBusMaster || !fDevice)
		return;
	fDevice->setBusMasterEnable(true);
	fBusMaster = true;
	IOLog("Firebrand: bus mastering on (the first GTT object or GART binding)\n");
}

void FirebrandCore::gttFree(struct frb_gtt *gtt)
{
	IOBufferMemoryDescriptor *mem = (IOBufferMemoryDescriptor *)gtt->handle;
	uint32_t npages = gtt->size / 4096;

	if (!mem)
		return;
	frb_gart_unbind(&fAdev, gtt->first_page, npages);
	frb_gart_free(&fAdev, gtt->first_page, npages);
	mem->complete();
	mem->release();
	gtt->handle = 0;
	gtt->cpu = 0;
}

/* --- stages ---------------------------------------------------------------- */

UInt32 FirebrandCore::configuredStage(void)
{
	OSNumber *n = OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandStage"));
	UInt32 stage = n ? n->unsigned32BitValue() : 0;
	int arg;

	if (PE_parse_boot_arg("frb_stage", &arg) && arg >= 0)
		stage = (UInt32)arg;
	return stage;
}

int FirebrandCore::runStages(UInt32 stage)
{
	UInt32 errs;
	int r;

	if (stage >= FRB_STAGE_COUNT)
		return -EINVAL;
	if (stage >= FRB_STAGE_GMC && (!mapAperture() || !allocDummyPage())) {
		IOLog("Firebrand: stages to %s: no aperture or dummy page\n", frb_stage_name((enum frb_stage)stage));
		return -ENOMEM;
	}
	errs = fErrCount;
	frb_trace_start(&fAdev);
	r = frb_device_init(&fAdev, (enum frb_stage)stage);
	frb_trace_stop(&fAdev);
	if (r && fAdev.stage_failed && fErrCount > errs) {
		/* the failed stage's own words, first cause first ("... / stage gmc: FAILED (-2)"), for the report */
		UInt32 first = fErrCount - errs > 3 ? fErrCount - 3 : errs, i, len = 0;

		fStageError[0] = 0;
		for (i = first; i < fErrCount && len + 4 < sizeof(fStageError); i++) {
			snprintf(fStageError + len, sizeof(fStageError) - len, "%s%s", len ? " / " : "", fErrLines[i % 4]);
			len = strlen(fStageError);
		}
	}
	IOLog("Firebrand: stages to %s: %d; %lu register accesses traced (%lu lost, %lu repeated reads not stored)\n",
	      frb_stage_name((enum frb_stage)stage), r, (unsigned long)fAdev.trace_len,
	      (unsigned long)fAdev.trace_lost, (unsigned long)fAdev.trace_squashed);
	publishState();
	if (fAdev.stage_done >= FRB_STAGE_POST)
		publishMemory();
	return r;
}

/*
 * The card's video memory as RadeonNI publishes it on the PCI device
 * (RadeonNI.cpp, KNOWN-ISSUES 16): what System Profiler and CGL's
 * kCGLRPVideoMemory read. The true size is CONFIG_MEMSIZE (MB); the CPU sees
 * only the 256 MB BAR0. 10.5.8's System Profiler cannot print more than
 * 2047 MB from any of these: SPDisplaysReporter sends VRAM,totalsize
 * -intValue, which SPSupport's NSData category answers with the FIRST FOUR
 * BYTES as a signed int, then prints that >> 20 -- an 8-byte value prints its
 * high word (8 GB -> 0 MB) and a 4-byte one past 0x7fffffff goes negative.
 * So VRAM,totalsize (and ATY,memsize) stays a 4-byte count capped at
 * 0x7ff00000 for any reader, and VRAM,totalMB carries the whole count in MB:
 * scripts/patch-system-profiler-vram.sh makes the reporter print that when
 * it is published (measured: an 8 GB card, 2047 MB -> 8192 MB).
 * VRAM,totalsize64 holds the bytes. Host-order numbers, as RadeonNI's. Also
 * the model name.
 */
void FirebrandCore::publishMemory(void)
{
	UInt32 memoryMB = frb_funcs(&fAdev)->get_config_memsize(&fAdev), memoryBytes, visibleMB;
	IODeviceMemory *bar = aperture();
	UInt64 totalBytes64, barMB;
	char model[64];
	UInt32 i;

	if (fAdev.row) {
		for (i = 0; i + 1 < sizeof(model) && fAdev.row->board[i] && fAdev.row->board[i] != '('; i++)
			model[i] = fAdev.row->board[i];
		while (i && model[i - 1] == ' ')
			i--;
		model[i] = 0;
		fDevice->setProperty("model", (void *)model, i + 1);
	}
	if (!memoryMB || memoryMB == 0xffffffff)
		return;
	totalBytes64 = (UInt64)memoryMB << 20;
	barMB = bar ? bar->getLength() >> 20 : 0;
	visibleMB = (UInt32)(barMB && barMB < memoryMB ? barMB : memoryMB);
	memoryBytes = memoryMB >= 2048 ? 0x7ff00000u : memoryMB << 20;
	fDevice->setProperty("VRAM,totalsize", &memoryBytes, sizeof(memoryBytes));
	fDevice->setProperty("VRAM,totalsize64", &totalBytes64, sizeof(totalBytes64));
	fDevice->setProperty("ATY,memsize", &memoryBytes, sizeof(memoryBytes));
	fDevice->setProperty("VRAM,totalMB", &memoryMB, sizeof(memoryMB));
	fDevice->setProperty("VRAM,visibleMB", &visibleMB, sizeof(visibleMB));
	IOLog("Firebrand: %lu MB of video memory, %lu MB visible through BAR0\n", (unsigned long)memoryMB,
	      (unsigned long)visibleMB);
}

void FirebrandCore::publishState(void)
{
	fOwner->setProperty("FirebrandStageDone", frb_stage_name(fAdev.stage_done));
	fOwner->setProperty("FirebrandStageFailed", (unsigned long long)(SInt64)fAdev.stage_failed, 32);
	publishReport();
}

/* the support level's word, as the report and the registry say it */
static const char *supportWord(const struct frb_device *adev)
{
	if (!adev->row)
		return "unknown";
	if (!adev->row->untested)
		return "verified";
	return frb_asic_ported(adev->asic_type) ? "untested" : "not ported";
}

void FirebrandCore::publishReport(void)
{
	char vbios[112];

	if (!fDevice)
		return;
	fDevice->setProperty("FirebrandSupport", supportWord(&fAdev));
	fDevice->setProperty("FirebrandUnlocked", fAdev.unlocked ? kOSBooleanTrue : kOSBooleanFalse);
	fDevice->setProperty("FirebrandStageCap", frb_stage_name(frb_stage_cap(&fAdev)));
	fDevice->setProperty("FirebrandStageDone", frb_stage_name(fAdev.stage_done));
	fDevice->setProperty("FirebrandStageFailed", (unsigned long long)(SInt64)fAdev.stage_failed, 32);
	fDevice->setProperty("FirebrandStageFailedAt", fAdev.stage_failed ? frb_stage_name(fAdev.stage_failed_at) : "");
	fDevice->setProperty("FirebrandStageError", fAdev.stage_failed ? fStageError : "");
	fDevice->setProperty("FirebrandASIC", frb_asic_name(fAdev.asic_type));
	if (fAdev.row)
		fDevice->setProperty("FirebrandBoard", fAdev.row->board);
	snprintf(vbios, sizeof(vbios), "%s%s%s%s%s", fAdev.vbios_pn[0] ? fAdev.vbios_pn : "unknown",
		 fAdev.vbios_ver[0] ? ", version " : "", fAdev.vbios_ver, fAdev.vbios_date[0] ? ", " : "", fAdev.vbios_date);
	fDevice->setProperty("FirebrandVBIOS", vbios);
	if (OSDynamicCast(OSString, fOwner ? fOwner->getProperty("FirebrandVersion") : 0))
		fDevice->setProperty("FirebrandVersion", fOwner->getProperty("FirebrandVersion"));
}

/*
 * amdgpu_device_get_pcie_info's inputs: the GPU's PCIe Link Capabilities
 * (pcie_get_speed_cap / pcie_get_width_cap on the device) from its config
 * space, and the upstream port's. The GPU hangs directly off QorIQPCIBridge
 * here, which publishes no IOPCIDevice for the root port, so the root
 * port's LnkCap comes from the personality's FirebrandUpstreamLinkCap (the
 * LnkCap low bits: speed | width << 4; 0x82 = 5 GT/s x8, what lspci reads
 * on the T4240's root port 0000:00:00.0). 0 = unknown: Linux's
 * defaults.
 */
void FirebrandCore::readPcieCaps(void)
{
	OSNumber *n = OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandUpstreamLinkCap"));
	UInt32 up = n ? n->unsigned32BitValue() : 0, lnkcap = 0;
	UInt8 ptr = 0;
	int guard = 48;

	if (fDevice->configRead16(kIOPCIConfigStatus) & 0x0010)	/* capabilities list */
		ptr = fDevice->configRead8(0x34) & 0xfc;
	while (ptr && guard-- > 0) {
		if (fDevice->configRead8(ptr) == 0x10) {	/* PCI Express */
			lnkcap = fDevice->configRead32(ptr + 0x0c);
			fPcieCap = ptr;
			break;
		}
		ptr = fDevice->configRead8(ptr + 1) & 0xfc;
	}
	fAdev.pcie_caps.asic_speed = lnkcap & 0xf;
	fAdev.pcie_caps.asic_width = (lnkcap >> 4) & 0x3f;
	fAdev.pcie_caps.platform_speed = up & 0xf;
	fAdev.pcie_caps.platform_width = (up >> 4) & 0x3f;
	IOLog("Firebrand: PCIe LnkCap: GPU %08x (speed %u, x%u); upstream (FirebrandUpstreamLinkCap) speed %u, x%u\n",
	      (unsigned)lnkcap, (unsigned)fAdev.pcie_caps.asic_speed, (unsigned)fAdev.pcie_caps.asic_width,
	      (unsigned)fAdev.pcie_caps.platform_speed, (unsigned)fAdev.pcie_caps.platform_width);
}

/* a personality switch, or its boot-arg */
bool FirebrandCore::flag(const char *key, const char *bootarg)
{
	int arg;

	return (fOwner && fOwner->getProperty(key) == kOSBooleanTrue) ||
		(bootarg && PE_parse_boot_arg(bootarg, &arg) && arg);
}

bool FirebrandCore::start(IOService *owner, IOPCIDevice *dev)
{
	OSDictionary *fw;
	UInt32 stage;

	fOwner = owner;
	fDevice = dev;
	if (!fDevice)
		return false;

	{
		const char *from = 0, *how = 0;

		fDMACmd = qoriqDMACommandSwitch(fOwner, &from);
		fDMAAdd = fDMALimit = 0;
		if (fDMACmd) {
			fDMAAdd = qoriqDMAAddFor(fDevice, &how, &fDMALimit);
			IOLog("Firebrand: DMA from IODMACommand (%s): %s, + 0x%llx\n", from, how,
			      (unsigned long long)fDMAAdd);
		} else
			IOLog("Firebrand: DMA: physical + inbound window 2 (QorIQDMACommand off, %s)\n", from);
	}

	fLock = IOLockAlloc();
	fLogMem = IOBufferMemoryDescriptor::withOptions(0, FRB_LOG_BYTES, 4096);
	fTraceMem = IOBufferMemoryDescriptor::withOptions(0, FRB_TRACE_ENTRIES * sizeof(struct frb_trace_ent), 4096);
	if (!fLock || !fLogMem || !fTraceMem)
		goto fail;
	bzero(fLogMem->getBytesNoCopy(), FRB_LOG_BYTES);

	/*
	 * The microcode: the FirebrandFirmware personality's, once for both
	 * driver personalities (embed-firmware.pl); a build that still embeds it
	 * in this personality is taken as it is, and leaves the property table
	 * (serializeProperties).
	 */
	fw = OSDynamicCast(OSDictionary, fOwner->getProperty("Firmware"));
	if (fw) {
		OSDictionary *aliases = OSDynamicCast(OSDictionary, fOwner->getProperty("FirmwareAliases"));

		fFirmware = fw;
		fFirmware->retain();
		if (aliases) {
			fFirmwareAliases = aliases;
			fFirmwareAliases->retain();
		}
		fOwner->removeProperty("Firmware");
		fOwner->removeProperty("FirmwareAliases");
		fFirmwareFrom = "this personality";
	} else if (!findFirmwareStore()) {
		IOLog("Firebrand: no FirebrandFirmware personality in the IOCatalogue: no microcode (stage post at most)\n");
	}
	if (fFirmware)
		IOLog("Firebrand: microcode from %s: %u files, %u aliases\n", fFirmwareFrom,
		      (unsigned)fFirmware->getCount(), fFirmwareAliases ? (unsigned)fFirmwareAliases->getCount() : 0);
	fAllowRegWrite = flag("FirebrandAllowRegWrite", "frb_regwrite");
	fBringUp = flag("FirebrandBringUp", "frb_bringup");
	{
		/* FirebrandUnlock (frbctl unlock writes it); boot-arg frb_unlock=1 on, frb_unlock=0 off over the personality */
		int arg;

		fUnlock = fOwner && fOwner->getProperty("FirebrandUnlock") == kOSBooleanTrue;
		if (PE_parse_boot_arg("frb_unlock", &arg))
			fUnlock = arg != 0;
	}
	fAdopt = flag("FirebrandAdopt", "frb_adopt");
	fDisplayAuto = flag("FirebrandDisplay", "frb_display");
	fAccelWanted = flag("FirebrandAccel", "frb_accel");
	fDpmWanted = flag("FirebrandDPM", "frb_dpm");
	fAudioWanted = flag("FirebrandAudio", "frb_audio");
	fPspOnly = flag("FirebrandPSPOnly", "frb_psp_only");
	fMMIOWptr = flag("FirebrandMMIOWptr", "frb_mmio_wptr");
	fUVDOneInstance = flag("FirebrandUVDOneInstance", "frb_uvd_one");
	{
		/* FirebrandGating (default on in the personality); boot-arg frb_cg=0 turns it off, frb_cg=1 on */
		int arg;

		fGating = fOwner && fOwner->getProperty("FirebrandGating") == kOSBooleanTrue;
		if (PE_parse_boot_arg("frb_cg", &arg))
			fGating = arg != 0;
	}
	{
		/* FirebrandGatingExclude: AMD_CG_SUPPORT_* bits to leave ungated; boot-arg frb_cg_exclude */
		OSNumber *n = fOwner ? OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandGatingExclude")) : 0;
		int arg;

		fGatingExclude = n ? n->unsigned32BitValue() : 0;
		if (PE_parse_boot_arg("frb_cg_exclude", &arg))
			fGatingExclude = (UInt32)arg;
	}
	fScanoutLimit = 0;

	bzero(&fOS, sizeof(fOS));
	fOS.cookie = this;
	fOS.mmio_read32 = os_mmio_read32;
	fOS.mmio_write32 = os_mmio_write32;
	fOS.cfg_read32 = os_cfg_read32;
	fOS.cfg_write32 = os_cfg_write32;
	fOS.delay_us = os_delay_us;
	fOS.time_ms = os_time_ms;
	fOS.alloc = os_alloc;
	fOS.free = os_free;
	fOS.firmware = os_firmware;
	fOS.log = os_log;
	fOS.gtt_alloc = os_gtt_alloc;
	fOS.gtt_free = os_gtt_free;

	bzero(&fAdev, sizeof(fAdev));
	fAdev.os = &fOS;
	fAdev.vendor = fDevice->configRead16(kIOPCIConfigVendorID);
	fAdev.device = fDevice->configRead16(kIOPCIConfigDeviceID);
	fAdev.revision = fDevice->configRead8(kIOPCIConfigRevisionID);
	fAdev.subsys_vendor = fDevice->configRead16(kIOPCIConfigSubSystemVendorID);
	fAdev.subsys_device = fDevice->configRead16(kIOPCIConfigSubSystemID);
	fAdev.asic_type = frb_asic_type_of_rev(fAdev.device, fAdev.revision);
	fAdev.row = frb_asic_lookup(fAdev.device, fAdev.revision, fAdev.subsys_vendor, fAdev.subsys_device);
	fAdev.unlocked = fUnlock;
	/* a card of a family Firebrand knows, with no verified row of its own: an untested card */
	if (!fAdev.row && fAdev.asic_type != CHIP_UNKNOWN_FRB)
		frb_asic_untested_row(&fAdev);
	/* the family's IP blocks (si_set_ip_blocks / vi_set_ip_blocks) */
	fAdev.funcs = frb_asic_funcs_of(fAdev.asic_type);
	if (!fAdev.funcs) {
		IOLog("Firebrand: %04x:%04x is no family this driver has\n", fAdev.vendor, fAdev.device);
		goto fail;
	}
	fAdev.num_crtc = fAdev.funcs->num_crtc;	/* the DCE's mode_info.num_crtc */
	fAdev.allow_unverified = fBringUp;
	fAdev.adopt = fAdopt;
	/* the accelerator's legacy clients take the GART start as 32 bits */
	fAdev.gart_legacy = fAccelWanted;
	fAdev.trace = (struct frb_trace_ent *)fTraceMem->getBytesNoCopy();
	fAdev.trace_cap = FRB_TRACE_ENTRIES;
	/*
	 * An untested card runs its family's own path to stage dce, power
	 * management included where the family has it there (CIK: CI DPM right
	 * after stage smu, for the heat; Vega 20: the SMU's DPM in stage smu,
	 * which the display and the thermal limits need). SI and Polaris start
	 * DPM after stage sdma, so an untested one gets it with the unlock.
	 */
	fAdev.want_dpm = fDpmWanted;
	fAdev.want_audio = fAudioWanted;
	fAdev.force_tmds = flag("FirebrandForceTMDS", "frb_force_tmds");
	{
		int cg = 1;

		/* frb_cg=0: SI's blocks keep the gating hw_init leaves (si_sdma_stage) */
		fAdev.no_cg = PE_parse_boot_arg("frb_cg", &cg) && !cg;
	}
	{
		OSNumber *q = fOwner ? OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandSIDPMQuirks")) : 0;
		int arg;

		/* SI DPM features to leave off (RadeonNI's SIDPMQuirks / rdn_sidpm_quirks) */
		fAdev.si_dpm_quirks = q ? q->unsigned32BitValue() : 0;
		if (PE_parse_boot_arg("frb_sidpm_quirks", &arg))
			fAdev.si_dpm_quirks = (uint32_t)arg;
		/*
		 * SI DPM with the rest (FirebrandDPM) since its tables reach the
		 * SMC in the SMC's byte order (docs/si-bring-up.md, "The cause");
		 * FirebrandSIDPM false or frb_sidpm=0 leaves SI at its boot clocks.
		 */
		fAdev.si_dpm_wanted = !(fOwner && fOwner->getProperty("FirebrandSIDPM") == kOSBooleanFalse);
		if (PE_parse_boot_arg("frb_sidpm", &arg))
			fAdev.si_dpm_wanted = arg != 0;
	}
	{
		/*
		 * CIK DPM (frb_ci_dpm.c), right after stage smu, with FirebrandDPM:
		 * FirebrandCIDPM / boot-arg frb_cidpm, 1 on (the default since the
		 * W8100's SMC tables were verified against Linux radeon's,
		 * docs/cik-bring-up.md), 0 off (boot clocks), 2 upload only (the SMC
		 * image and tables in its SRAM, the SMC held in reset)
		 */
		OSNumber *n = fOwner ? OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandCIDPM")) : 0;
		int arg, v = n ? (int)n->unsigned32BitValue() : 1;

		if (PE_parse_boot_arg("frb_cidpm", &arg))
			v = arg;
		fAdev.ci_dpm_wanted = v != 0;
		fAdev.ci_dpm_quirks = v == 2 ? FRB_CI_DPM_UPLOAD_ONLY : 0;
	}
	fAdev.audio_pin = -1;		/* RadeonNI's pin choice until frbctl audio pin N */
	fAdev.psp_only = fPspOnly;	/* SOC15: stage smu stops after the PSP's firmware loads */
	fAdev.mmio_wptr = fMMIOWptr;	/* SOC15: no doorbells, the rings write their WPTR registers */
	fAdev.uvd_one_inst = fUVDOneInstance;	/* Vega 20: stage uvd starts only the first UVD instance */
	fAdev.want_gating = fGating;	/* SOC15: clock gating after stage sdma (amdgpu's late_init) */
	fAdev.cg_exclude = fGatingExclude;
	readPcieCaps();
	findSlotPort();
	{
		/* the driver's version and source, for frbctl report and Firebrand Monitor (any user reads the registry) */
		char v[96];

		snprintf(v, sizeof(v), "%s%s%s", kmod_info.version, FRB_BUILD_ID[0] ? " " : "", FRB_BUILD_ID);
		fOwner->setProperty("FirebrandVersion", v);
	}

	IOLog("Firebrand: %04x:%04x rev %02x subsystem %04x:%04x, %s, command %04x; %s\n",
	      fAdev.vendor, fAdev.device, fAdev.revision, fAdev.subsys_vendor, fAdev.subsys_device,
	      frb_asic_name(fAdev.asic_type), fDevice->configRead16(kIOPCIConfigCommand),
	      fAdev.row ? fAdev.row->board : "no row in the family table: survey only");

	if (!frb_asic_ported(fAdev.asic_type)) {
		/*
		 * Vega 10 / Vega 12: recognised, not ported. The VBIOS (from the
		 * option ROM) for the report, and nothing else: the register BAR
		 * is not even mapped.
		 */
		fDevice->setMemoryEnable(true);
		if (loadBios())
			frb_atombios_survey_strings(&fAdev);
		fOwner->setProperty("FirebrandASIC", frb_asic_name(fAdev.asic_type));
		publishReport();
		IOLog("Firebrand: %s has no Firebrand port yet: identified for a report (frbctl report), "
		      "nothing written; please report it as an issue\n", frb_asic_name(fAdev.asic_type));
		goto fail;
	}

	/* amdgpu_device_init: the register BAR is 5 from Bonaire on (CIK, VI: 256 KB; Vega 20: 512 KB), 2 on SI */
	fDevice->setMemoryEnable(true);
	fRegMap = fDevice->mapDeviceMemoryWithRegister((UInt8)fAdev.funcs->rmmio_bar);
	if (!fRegMap) {
		IOLog("Firebrand: cannot map the register BAR (BAR%lu)\n",
		      (unsigned long)((fAdev.funcs->rmmio_bar - 0x10) / 4));
		goto fail;
	}
	fRegs = (volatile UInt8 *)fRegMap->getVirtualAddress();
	fAdev.rmmio_size = (uint32_t)fRegMap->getLength();
	IOLog("Firebrand: registers: BAR%lu, %lu KB (%s)\n", (unsigned long)((fAdev.funcs->rmmio_bar - 0x10) / 4),
	      (unsigned long)(fAdev.rmmio_size >> 10), fAdev.funcs->name);

	if (!loadBios()) {
		IOLog("Firebrand: no usable VBIOS; nothing done\n");
		snprintf(fStageError, sizeof(fStageError), "no usable VBIOS (the option ROM, the firmware's copy and the personality's all failed)");
		fAdev.stage_failed = -ENOENT;
		publishReport();
		goto fail;
	}
	if (frb_is_soc15(&fAdev))
		mapDoorbells();		/* the gfx and SDMA rings' WPTRs (stages gfx, sdma) */

	/* the survey: reads only */
	fAdev.funcs->early_init(&fAdev);
	frb_device_survey(&fAdev);	/* atomfirmware (SOC15) or atombios tables */
	fAdev.stage_done = FRB_STAGE_SURVEY;
	fOwner->setProperty("FirebrandASIC", frb_asic_name(fAdev.asic_type));
	if (fAdev.row)
		fOwner->setProperty("FirebrandBoard", fAdev.row->board);
	publishState();

	if (fAdev.row->untested)
		IOLog("Firebrand: %04x:%04x rev %02x subsystem %04x:%04x is an UNTESTED card: %s; "
		      "please send its report (frbctl report --md, or Firebrand Monitor's Copy Report) as a GitHub issue\n",
		      fAdev.vendor, fAdev.device, fAdev.revision, fAdev.subsys_vendor, fAdev.subsys_device,
		      fAdev.unlocked ? "unlocked (FirebrandUnlock / frb_unlock=1): every stage the family has" :
		      "display-only (stage dce); `sudo frbctl unlock` and a restart try the rest");

	stage = configuredStage();
	if (stage > (UInt32)frb_stage_cap(&fAdev) && !fBringUp) {
		IOLog("Firebrand: stage %lu asked, %s %s stage %s; running that far\n",
		      (unsigned long)stage, fAdev.row->board, fAdev.row->untested ? "may run to" : "is verified to",
		      frb_stage_name(frb_stage_cap(&fAdev)));
		stage = frb_stage_cap(&fAdev);
	}
	if (stage > FRB_STAGE_SURVEY) {
		IOLockLock(fLock);
		runStages(stage);
		if (fDisplayAuto && fAdev.stage_done >= FRB_STAGE_DCE && fAdev.display) {
			int found = firstConnected();

			if (found >= 0)
				setMode(found, 0, 0, 0, maxLinkRate());
			else
				IOLog("Firebrand: FirebrandDisplay: no monitor found\n");
		}
		IOLockUnlock(fLock);
		/* outside the lock: the bridge publishes a nub and its driver starts */
		if (fAdev.stage_done >= FRB_STAGE_DCE)
			audioFunction();
	}
	IOLog("Firebrand: ready (stage %s); frbctl talks to it\n", frb_stage_name(fAdev.stage_done));
	return true;

fail:
	stop();
	return false;
}

void FirebrandCore::stop(void)
{
	if (fLock) {
		/* the status call answers with the identity only from here on */
		IOLockLock(fLock);
		fStopped = true;
		IOLockUnlock(fLock);
	}
	if (fRegs) {
		/* the engines first: nothing may fetch from the driver's memory after this */
		IOLockLock(fLock);
		frb_engines_fini(&fAdev);
		if (fAdev.display)
			frb_display_fini(&fAdev);
		IOLockUnlock(fLock);
	}
	if (fBusMaster && fDevice) {
		/* the engines are halted: the card may not reach host memory any more */
		fDevice->setBusMasterEnable(false);
		fBusMaster = false;
	}
	if (fFbMap) {
		fFbMap->release();
		fFbMap = 0;
		fAdev.aper = 0;
	}
	if (fDoorbellMap) {
		fOS.doorbell_write64 = 0;
		fDoorbellMap->release();
		fDoorbellMap = 0;
		fDoorbells = 0;
	}
	if (fRegMap) {
		fRegMap->release();
		fRegMap = 0;
		fRegs = 0;
	}
	if (fDummy) {
		/* bus mastering was never enabled, so nothing can still be using it */
		fDummy->complete();
		fDummy->release();
		fDummy = 0;
	}
	frb_atombios_fini(&fAdev);
	if (fBios) {
		IOFree(fBios, fBiosSize);
		fBios = 0;
	}
}

void FirebrandCore::free(void)
{
	if (fTraceMem)
		fTraceMem->release();
	if (fLogMem)
		fLogMem->release();
	if (fFirmware)
		fFirmware->release();
	if (fFirmwareAliases)
		fFirmwareAliases->release();
	fFirmwareAliases = 0;
	if (fLutSaved)
		IOFree(fLutSaved, 3 * 256 * sizeof(uint16_t));
	fLutSaved = 0;
	if (fLock)
		IOLockFree(fLock);
	if (fSlotPort)
		fSlotPort->release();
	fSlotPort = 0;
	fTraceMem = 0;
	fLogMem = 0;
	fFirmware = 0;
	fLock = 0;
}

/* --- the control client's work, under the lock ----------------------------- */

IOReturn FirebrandCore::ucInfo(struct frb_user_info *in)
{
	IOLockLock(fLock);
	bzero(in, sizeof(*in));
	in->version = 1;
	in->vendor = fAdev.vendor;
	in->device = fAdev.device;
	in->subsys_vendor = fAdev.subsys_vendor;
	in->subsys_device = fAdev.subsys_device;
	in->revision = fAdev.revision;
	in->rev_id = fAdev.rev_id;
	in->external_rev_id = fAdev.external_rev_id;
	in->asic_type = fAdev.asic_type;
	in->row_max_stage = fAdev.row ? fAdev.row->max_stage : 0xff;
	in->stage_done = fAdev.stage_done;
	in->stage_failed = fAdev.stage_failed;
	in->asic_init_ms = fAdev.asic_init_ms;
	in->rmmio_size = fAdev.rmmio_size;
	in->aper_size = (uint32_t)fAdev.aper_size;
	in->vram_mb = (uint32_t)(fAdev.gmc.mc_vram_size >> 20);
	in->vram_width = fAdev.gmc.vram_width;
	in->vram_start_hi = (uint32_t)(fAdev.gmc.vram_start >> 32);
	in->vram_start_lo = (uint32_t)fAdev.gmc.vram_start;
	in->gart_start_hi = (uint32_t)(fAdev.gmc.gart_start >> 32);
	in->gart_start_lo = (uint32_t)fAdev.gmc.gart_start;
	in->gart_table_offset = fAdev.layout.gart_table;
	in->mem_scratch_offset = fAdev.layout.mem_scratch;
	in->toc_offset = fAdev.layout.toc;
	in->smu_buffer_offset = fAdev.layout.smu_buffer;
	in->fw_buf_offset = fAdev.layout.fw_buf;
	in->dummy_hi = (uint32_t)(fAdev.dummy_page_addr >> 32);
	in->dummy_lo = (uint32_t)fAdev.dummy_page_addr;
	in->trace_len = fAdev.trace_len;
	in->trace_cap = fAdev.trace_cap;
	in->trace_lost = fAdev.trace_lost;
	in->log_len = fLogLen;
	in->log_cap = fLogMem ? (uint32_t)fLogMem->getLength() : 0;
	in->mc_fw_version = fAdev.gmc.fw_version;
	if (fAdev.row)
		strncpy(in->board, fAdev.row->board, sizeof(in->board) - 1);
	strncpy(in->mc_fw, fAdev.gmc.fw_name, sizeof(in->mc_fw) - 1);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucRegRead(UInt32 reg, UInt32 *value)
{
	if (reg * 4 >= fAdev.rmmio_size)
		return kIOReturnBadArgument;
	IOLockLock(fLock);
	*value = frb_rreg(&fAdev, reg);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * FRB_QUIRK_AUDIO_FN_STRAP (frb_device.h): a card whose HD Audio function is
 * strapped on but hidden by the BIF's audio pin strap. The strap is card
 * knowledge, so it is set here; finding the new function and giving it a
 * nub is the PCI bridge's job, asked for by name through
 * callPlatformFunction("PCIScanFunctions", device, flags 0: no reset) so the
 * call is harmless on a bridge that does not know it (kIOReturnUnsupported).
 * Off with FirebrandAudioFunction false or boot-arg frb_audiofn=0. Called
 * once the stages have run: the audio driver that attaches to the new
 * function finds a posted card with its display engine up.
 */
#define FRB_PCI_SCAN_FUNCTIONS	"PCIScanFunctions"
#define FRB_PCI_SCAN_RESET_LINK	1u	/* a link reset first; the quirk does not use it (frbctl fn scan 1 can) */

void FirebrandCore::audioFunction(void)
{
	IOPCIAddressSpace sp;
	UInt32 hdr_before, hdr_after, id1, newNubs = 0;
	IOReturn ret;
	bool on;
	int arg, r;

	if (!fAdev.row || !(fAdev.row->quirks & FRB_QUIRK_AUDIO_FN_STRAP) || !frb_is_soc15(&fAdev))
		return;
	on = fOwner->getProperty("FirebrandAudioFunction") != kOSBooleanFalse;
	if (PE_parse_boot_arg("frb_audiofn", &arg))
		on = arg != 0;
	sp = fDevice->space;
	sp.s.functionNum = 1;
	hdr_before = fDevice->configRead32(kIOPCIConfigCacheLineSize);
	IOLockLock(fLock);
	r = frb_soc15_audio_function_strap(&fAdev, on);
	IOLockUnlock(fLock);
	hdr_after = fDevice->configRead32(kIOPCIConfigCacheLineSize);
	id1 = fDevice->configRead32(sp, kIOPCIConfigVendorID);
	IOLog("Firebrand: audio function (quirk %s): strap %s; header type %02x -> %02x; function 1 reads %08x\n",
	      on ? "on" : "off (FirebrandAudioFunction / frb_audiofn)",
	      r == FRB_AUDIOFN_NONE ? "not an HD Audio function, left alone" :
	      r == FRB_AUDIOFN_ALREADY ? "STRAP_AUD_PIN already on" :
	      r == FRB_AUDIOFN_LEFT ? "STRAP_AUD_PIN 0 (no audio function), left so" :
	      r == FRB_AUDIOFN_WRITTEN ? "STRAP_AUD_PIN set" : "STRAP_AUD_PIN did NOT read back",
	      (unsigned)((hdr_before >> 16) & 0xff), (unsigned)((hdr_after >> 16) & 0xff), (unsigned)id1);
	if (!on || (r != FRB_AUDIOFN_WRITTEN && r != FRB_AUDIOFN_ALREADY))
		return;
	if (!(hdr_after & 0x00800000) || id1 == 0xffffffff) {
		IOLog("Firebrand: audio function: the strap is on but function 1 did not appear; nothing asked of "
		      "the bridge\n");
		return;
	}
	/* a function the bridge already published (a second start) gets no second nub */
	ret = scanFunctions(0, &newNubs);
	IOLog("Firebrand: audio function: the bridge asked to scan %02x:%02x's functions: 0x%x%s; %u new nub(s)\n",
	      (unsigned)sp.s.busNum, (unsigned)sp.s.deviceNum, ret,
	      ret == kIOReturnUnsupported ? " (the bridge does not offer " FRB_PCI_SCAN_FUNCTIONS ")" : "",
	      (unsigned)newNubs);
}

extern "C" {
/* frb_soc15.h (its register headers are C only) */
uint32_t frb_soc15_smn_rreg(struct frb_device *adev, uint32_t reg);
void frb_soc15_smn_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
void frb_soc15_survey_registers(struct frb_device *adev);
}

/* the bridge's PCIScanFunctions for this device (kIOReturnUnsupported from a bridge without it) */
IOReturn FirebrandCore::scanFunctions(UInt32 flags, UInt32 *newNubs)
{
	IOService *bridge = OSDynamicCast(IOService, fDevice->getParentEntry(gIOServicePlane));
	const OSSymbol *name;
	IOReturn ret;

	*newNubs = 0;
	if (!bridge)
		return kIOReturnNoDevice;
	name = OSSymbol::withCString(FRB_PCI_SCAN_FUNCTIONS);
	ret = bridge->callPlatformFunction(name, false, fDevice, (void *)(unsigned long)flags, newNubs, 0);
	name->release();
	return ret;
}

IOReturn FirebrandCore::ucSmn(UInt32 addr, UInt32 write, UInt32 value, UInt32 *out)
{
	if (!frb_is_soc15(&fAdev))
		return kIOReturnUnsupported;
	if (write && !fAllowRegWrite)
		return kIOReturnNotPermitted;
	IOLockLock(fLock);
	if (write) {
		IOLog("Firebrand: frbctl smn %08lx = %08lx\n", (unsigned long)addr, (unsigned long)value);
		frb_soc15_smn_wreg(&fAdev, addr, value);
	}
	*out = frb_soc15_smn_rreg(&fAdev, addr);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucFunctions(UInt32 op, UInt32 arg, SInt32 *result, UInt32 *hdr, UInt32 *id1, UInt32 *nubs)
{
	IOPCIAddressSpace sp = fDevice->space;

	sp.s.functionNum = 1;
	*result = 0;
	*nubs = 0;
	if (!frb_is_soc15(&fAdev))
		return kIOReturnUnsupported;
	if (op != FRB_FN_OP_STATUS && !(op == FRB_FN_OP_SCAN && !(arg & FRB_PCI_SCAN_RESET_LINK)) && !fAllowRegWrite)
		return kIOReturnNotPermitted;
	IOLockLock(fLock);
	switch (op) {
	case FRB_FN_OP_STATUS:
		frb_soc15_survey_registers(&fAdev);
		frb_soc15_audio_function_strap(&fAdev, 0);
		break;
	case FRB_FN_OP_SCAN:
		*result = (SInt32)scanFunctions(arg, nubs);
		break;
	default:
		IOLockUnlock(fLock);
		return kIOReturnBadArgument;
	}
	*hdr = (fDevice->configRead32(kIOPCIConfigCacheLineSize) >> 16) & 0xff;
	*id1 = fDevice->configRead32(sp, kIOPCIConfigVendorID);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucConfig(UInt32 fn, UInt32 offset, UInt32 write, UInt32 value, UInt32 *out)
{
	IOPCIAddressSpace sp;

	if (offset > 0xffc || (offset & 3))
		return kIOReturnBadArgument;
	if (offset > 0xfc)
		return kIOReturnUnsupported;	/* IOPCIDevice's space calls take a UInt8 offset */
	if (fn & 0x01000000) {
		/*
		 * 0x01BBDDFF: another device of this host bridge (the card's
		 * switch ports, the root port), read only: the status registers
		 * of the path to the card
		 */
		IOPCIBridge *bridge = OSDynamicCast(IOPCIBridge, fDevice->getProvider());

		if (write || !bridge || ((fn >> 8) & 0xff) > 31 || (fn & 0xff) > 7)
			return kIOReturnBadArgument;
		sp = fDevice->space;
		sp.s.busNum = (fn >> 16) & 0xff;
		sp.s.deviceNum = (fn >> 8) & 0x1f;
		sp.s.functionNum = fn & 7;
		*out = bridge->configRead32(sp, (UInt8)offset);
		return kIOReturnSuccess;
	}
	if (fn > 7)
		return kIOReturnBadArgument;
	if (write && !fAllowRegWrite)
		return kIOReturnNotPermitted;
	sp = fDevice->space;
	sp.s.functionNum = fn;
	IOLockLock(fLock);
	if (write) {
		IOLog("Firebrand: frbctl cfg %u %02lx = %08lx\n", (unsigned)fn, (unsigned long)offset,
		      (unsigned long)value);
		fDevice->configWrite32(sp, (UInt8)offset, value);
	}
	*out = fDevice->configRead32(sp, (UInt8)offset);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucRegWrite(UInt32 reg, UInt32 value)
{
	if (!fAllowRegWrite)
		return kIOReturnNotPermitted;
	if (reg * 4 >= fAdev.rmmio_size)
		return kIOReturnBadArgument;
	IOLockLock(fLock);
	IOLog("Firebrand: frbctl wr %05lx = %08lx\n", (unsigned long)reg, (unsigned long)value);
	frb_wreg(&fAdev, reg, value);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucSmcRead(UInt32 addr, UInt32 *value)
{
	IOLockLock(fLock);
	*value = frb_smc_rreg(&fAdev, addr);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * frbctl unlock / lock on a loaded driver. Only an untested card's cap moves
 * (frb_stage_cap); a verified row keeps its own. Unlocking lets frbctl stage
 * N run the rest now (power management with stage sdma, as at a start);
 * locking stops further stages but leaves the ones already run until the
 * next start.
 */
IOReturn FirebrandCore::ucUnlock(UInt32 on, UInt32 *cap)
{
	IOLockLock(fLock);
	fAdev.unlocked = on ? 1 : 0;
	*cap = frb_stage_cap(&fAdev);
	IOLog("Firebrand: frbctl %s: %s may run to stage %s\n", on ? "unlock" : "lock",
	      fAdev.row ? fAdev.row->board : "the card", frb_stage_name(frb_stage_cap(&fAdev)));
	publishReport();
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucRunStage(UInt32 stage, SInt32 *result)
{
	IOLockLock(fLock);
	*result = runStages(stage);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucVramRead(UInt32 offset, UInt32 *value)
{
	IOReturn r = kIOReturnSuccess;

	IOLockLock(fLock);
	/* only once stage gmc has let the CPU in (BIF_FB_EN) */
	if (fAdev.stage_done < FRB_STAGE_GMC || !frb_vram_ok(&fAdev, offset, 4))
		r = kIOReturnNotReady;
	else
		*value = frb_vram_read32(&fAdev, offset);
	IOLockUnlock(fLock);
	return r;
}

IOReturn FirebrandCore::ucTrace(UInt32 on, UInt32 *entries)
{
	IOLockLock(fLock);
	if (on)
		frb_trace_start(&fAdev);
	else
		frb_trace_stop(&fAdev);
	*entries = fAdev.trace_len;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/* --- the display ------------------------------------------------------------ */

struct frb_display *FirebrandCore::display(void)
{
	if (fAdev.stage_done < FRB_STAGE_DCE || !fAdev.display || !fAdev.display->ready)
		return 0;
	return fAdev.display;
}

/* width 0: the EDID's preferred mode; else the EDID's, then the DMT table's */
bool FirebrandCore::pickMode(struct frb_connector *conn, UInt32 width, UInt32 height, UInt32 hz,
			 struct frb_mode *mode)
{
	const UInt32 max = 48;
	struct frb_mode *list = (struct frb_mode *)IOMalloc(max * sizeof(*list));
	int n = 0, pref = -1, i;
	bool found = false;

	if (!list)
		return false;
	if (conn->edid_len >= 128)
		n = frb_edid_modes(conn->edid, conn->edid_len, list, max, &pref);
	if (!width) {
		if (n) {
			*mode = list[pref >= 0 ? pref : 0];
			found = true;
		}
	} else {
		for (i = 0; i < n && !found; i++) {
			if (list[i].hdisplay == width && list[i].vdisplay == height &&
			    (!hz || frb_mode_hz(&list[i]) == hz)) {
				*mode = list[i];
				found = true;
			}
		}
		for (i = 0; i < (int)frb_dmt_mode_count && !found; i++) {
			if (frb_dmt_modes[i].hdisplay == width && frb_dmt_modes[i].vdisplay == height &&
			    (!hz || frb_mode_hz(&frb_dmt_modes[i]) == hz)) {
				*mode = frb_dmt_modes[i];
				found = true;
			}
		}
	}
	IOFree(list, max * sizeof(*list));
	return found;
}

/*
 * A test pattern, written as big-endian XRGB words: the scanout swaps
 * 8-in-32 (GRPH_SWAP_CNTL), so 0x00RRGGBB here is red RR on the screen.
 */
static UInt32 patternPixel(UInt32 pattern, UInt32 x, UInt32 y, UInt32 w, UInt32 h, UInt32 a0, UInt32 a1)
{
	static const UInt32 bars[8] = {
		0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000
	};
	UInt32 v;

	switch (pattern) {
	case FRB_PATTERN_BARS:
		if (x == 0 || y == 0 || x == w - 1 || y == h - 1)
			return 0xffffff;
		if (x >= 8 && x < 72 && y >= 8 && y < 72)
			return 0xff0000;
		if (y < h * 2 / 3)
			return bars[x * 8 / w];
		v = ((x * 4) % w) * 256 / w;
		switch (x * 4 / w) {
		case 0:		return v * 0x010101;
		case 1:		return v << 16;
		case 2:		return v << 8;
		default:	return v;
		}
	case FRB_PATTERN_GREY:
		return 0x9a9a9a;
	case FRB_PATTERN_SOLID:
		return a0 & 0xffffff;
	case FRB_PATTERN_DOT:
		return x == a0 && y == a1 ? 0xffffff : 0;
	case FRB_PATTERN_TOP:
		return y < a0 ? 0xffffff : 0;
	case FRB_PATTERN_LEFT:
		return x < a0 ? 0xffffff : 0;
	case FRB_PATTERN_SMPTE: {
		/* libdrm tests/util/pattern.c fill_smpte_rgb32, XR24 (no alpha bits) */
		static const UInt32 top[7] = { 0xc0c0c0, 0xc0c000, 0x00c0c0, 0x00c000, 0xc000c0, 0xc00000, 0x0000c0 };
		static const UInt32 middle[7] = { 0x0000c0, 0x131313, 0xc000c0, 0x131313, 0x00c0c0, 0x131313, 0xc0c0c0 };
		static const UInt32 bottom[8] = { 0x00214c, 0xffffff, 0x32006a, 0x131313, 0x090909, 0x131313, 0x1d1d1d,
						  0x131313 };

		if (y < h * 6 / 9)
			return top[x * 7 / w];
		if (y < h * 7 / 9)
			return middle[x * 7 / w];
		if (x < w * 5 / 7)
			return bottom[x * 4 / (w * 5 / 7)];
		if (x < w * 6 / 7)
			return bottom[(x - w * 5 / 7) * 3 / (w / 7) + 4];
		return bottom[7];
	}
	case FRB_PATTERN_SMPTE_CURSOR:
		/* ucCursor's FRB_CURSOR_OP_TEST image (left half opaque, its alpha 0xff) over the bars */
		if (x >= a0 && x < a0 + FRB_CURSOR_MAX / 2 && y >= a1 && y < a1 + FRB_CURSOR_MAX)
			return ((((x - a0) * 4) & 0xff) << 16) | ((((y - a1) * 2) & 0xff) << 8) |
			       ((((x - a0) + (y - a1)) * 3) & 0xff);
		return patternPixel(FRB_PATTERN_SMPTE, x, y, w, h, 0, 0);
	default:
		return 0;
	}
}

void FirebrandCore::fillPattern(UInt32 pattern, UInt32 arg0, UInt32 arg1)
{
	struct frb_crtc *c = &fAdev.display->crtc;
	UInt32 x, y;

	if (pattern == FRB_PATTERN_SPLASH) {
		/* the boot splash, at the surface's depth (shared/frb_splash.c) */
		frb_splash_draw(fAdev.aper + c->fb_offset, c->fb_pitch, c->fb_width, c->fb_height,
				c->fb_bpp ? c->fb_bpp : 32);
		__asm__ __volatile__("sync" : : : "memory");
		return;
	}
	for (y = 0; y < c->fb_height; y++) {
		volatile UInt32 *row = (volatile UInt32 *)(fAdev.aper + c->fb_offset + y * c->fb_pitch * 4);

		for (x = 0; x < c->fb_width; x++)
			row[x] = patternPixel(pattern, x, y, c->fb_width, c->fb_height, arg0, arg1);
	}
	__asm__ __volatile__("sync" : : : "memory");
}

/* the first connector with a DP sink, or -1 */
int FirebrandCore::firstConnected(void)
{
	struct frb_display *d = display();
	int i;

	for (i = 0; d && i < d->num_connectors; i++)
		if (frb_connector_connected(&d->conn[i]))
			return i;
	return -1;
}

/* FirebrandMaxLinkRate (kHz; 0 = the sink's) */
UInt32 FirebrandCore::maxLinkRate(void)
{
	OSNumber *n = fOwner ? OSDynamicCast(OSNumber, fOwner->getProperty("FirebrandMaxLinkRate")) : 0;

	return n ? n->unsigned32BitValue() : 0;
}

/*
 * The scanout surface: the first 1 MB boundary past what the pre-OS display
 * holds, the same for every mode (the window server maps the aperture once).
 */
UInt32 FirebrandCore::surfaceOffset(void)
{
	return (fAdev.gmc.stolen_vga_size + 0xfffff) & ~0xfffffu;
}

IODeviceMemory *FirebrandCore::aperture(void)
{
	return fDevice ? fDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0) : 0;
}

/*
 * A modeset on CRTC0 with amdgpu's 64-pixel pitch. A test pattern, if one
 * is asked for, goes in before the CRTC starts reading. The register trace
 * is restarted for it.
 */
int FirebrandCore::programMode(UInt32 idx, const struct frb_mode *mode, int pattern, UInt32 max_rate,
				UInt32 bpp)
{
	struct frb_display *d = display();
	UInt32 offset, pitch, bytes;
	int r;

	if (!d)
		return -ENODEV;
	if (idx >= (UInt32)d->num_connectors || (bpp != 8 && bpp != 16 && bpp != 32) ||
	    (pattern >= 0 && pattern != FRB_PATTERN_SPLASH && bpp != 32))
		return -EINVAL;		/* the test patterns are 32 bpp; the splash is any depth */
	pitch = frb_display_align_pitch(mode->hdisplay, bpp);
	bytes = pitch * (bpp / 8) * mode->vdisplay;
	offset = surfaceOffset();
	if (!frb_vram_ok(&fAdev, offset, bytes) || offset + bytes > fAdev.layout.reserved_start ||
	    (fScanoutLimit && offset + bytes > fScanoutLimit))
		return -ERANGE;
	if (pattern >= 0) {
		d->crtc.fb_offset = offset;
		d->crtc.fb_pitch = pitch;
		d->crtc.fb_width = mode->hdisplay;
		d->crtc.fb_height = mode->vdisplay;
		d->crtc.fb_bpp = bpp;
		fillPattern((UInt32)pattern);
	}
	frb_trace_start(&fAdev);
	r = frb_display_set_mode(&fAdev, d, &d->conn[idx], mode, offset, pitch, bpp, max_rate);
	frb_trace_stop(&fAdev);
	IOLog("Firebrand: display: %ux%u@%u, %lu bpp, on connector %lu: %d; %lu register accesses traced (%lu lost)\n",
	      mode->hdisplay, mode->vdisplay, frb_mode_hz(mode), (unsigned long)bpp, (unsigned long)idx, r,
	      (unsigned long)fAdev.trace_len, (unsigned long)fAdev.trace_lost);
	return r;
}

/* frbctl mode: a mode by size from the EDID or the DMT, colour bars */
int FirebrandCore::setMode(UInt32 idx, UInt32 width, UInt32 height, UInt32 hz, UInt32 max_rate)
{
	struct frb_display *d = display();
	struct frb_mode mode;

	if (!d)
		return -ENODEV;
	if (idx >= (UInt32)d->num_connectors)
		return -EINVAL;
	if (!pickMode(&d->conn[idx], width, height, hz, &mode)) {
		IOLog("Firebrand: display: no mode %lux%lu@%lu for connector %lu\n", (unsigned long)width,
		      (unsigned long)height, (unsigned long)hz, (unsigned long)idx);
		return -ENOENT;
	}
	return programMode(idx, &mode, FRB_PATTERN_BARS, max_rate);
}

IOReturn FirebrandCore::ucDisplayInfo(struct frb_user_display *out)
{
	struct frb_display *d;
	int i;

	IOLockLock(fLock);
	bzero(out, sizeof(*out));
	out->version = 1;
	out->active = -1;
	d = display();
	if (d) {
		struct frb_crtc *c = &d->crtc;

		out->ready = 1;
		out->num_connectors = d->num_connectors;
		out->num_crtc = d->num_crtc;
		out->default_dispclk = d->default_dispclk;
		out->dp_extclk = d->dp_extclk;
		if (frb_is_soc15(&fAdev))	/* DCE 12: DISPCLK as the VBIOS last set it */
			out->default_dispclk = d->dce12.dispclk_khz / 10;
		for (i = 0; i < d->num_connectors && i < 6; i++) {
			struct frb_connector *s = &d->conn[i];
			struct frb_user_connector *u = &out->conn[i];

			u->connector_object_id = s->connector_object_id;
			u->encoder_object_id = s->encoder_object_id;
			u->devices = s->devices;
			u->edid_len = s->edid_len;
			u->connector_type = s->connector_type;
			u->encoder_id = s->encoder_id;
			u->linkb = s->linkb;
			u->i2c_id = s->i2c_id;
			u->hpd = s->hpd;
			/* DCE 12: the link's DIG is its transmitter's (DC's link_enc_hw_inst = stream encoder) */
			u->dig_encoder = (int8_t)(frb_is_soc15(&fAdev) && c->conn == s ? s->transmitter : s->dig_encoder);
			u->sense = frb_display_hpd_sense(&fAdev, s);
			u->dp_sink_type = s->dp_sink_type;
			bcopy(s->dpcd, u->dpcd, sizeof(u->dpcd));
			u->dp_clock = s->dp_clock;
			u->dp_lane_count = s->dp_lane_count;
			if (c->conn == s)
				out->active = i;
		}
		if (c->conn) {
			out->clock = c->mode.clock;
			out->flags = c->mode.flags;
			out->hdisplay = c->mode.hdisplay;
			out->hsync_start = c->mode.hsync_start;
			out->hsync_end = c->mode.hsync_end;
			out->htotal = c->mode.htotal;
			out->vdisplay = c->mode.vdisplay;
			out->vsync_start = c->mode.vsync_start;
			out->vsync_end = c->mode.vsync_end;
			out->vtotal = c->mode.vtotal;
			out->adjusted_clock = c->adjusted_clock;
			out->fb_offset = c->fb_offset;
			out->fb_pitch = c->fb_pitch;
			out->line_time = c->line_time;
			out->watermark = c->wm_high;
			out->lb_lead_lines = c->lb_vblank_lead_lines;
		}
		out->audio_dig = d->afmt.enabled ? d->afmt.active_dig : -1;
		out->audio_ep = d->afmt.active_pin >= 0 ? d->afmt.active_pin : 0;
		if (frb_is_soc15(&fAdev)) {
			frb_dce_v12_0_readout(&fAdev, out->audio_ep, &out->crtc_control,
					      &out->crtc_status_frame_count, &out->audio_hot_plug,
					      &out->audio_speakers);
			goto done;
		}
		/*
		 * that codec pin's HOT_PLUG_CONTROL, CHANNEL_SPEAKER through its
		 * AZF0ENDPOINTn index/data pair, from the DCE's audio table: DCE
		 * 11.2's (0x17a8, 4 apart) and DCE 8's (0x1780, 6 apart; reads
		 * need no write-enable bit). DCE 6 is not read here, as before.
		 */
		if (frb_funcs(&fAdev)->display && (frb_funcs(&fAdev)->display->audio_regs == &frb_audio_regs_dce11 ||
						   frb_funcs(&fAdev)->display->audio_regs == &frb_audio_regs_dce8)) {
			const struct frb_audio_regs *ar = frb_funcs(&fAdev)->display->audio_regs;
			UInt32 pin = ar->pin_stride * (UInt32)out->audio_ep;

			frb_wreg(&fAdev, ar->ep_index + pin, 0x54);
			out->audio_hot_plug = frb_rreg(&fAdev, ar->ep_data + pin);
			frb_wreg(&fAdev, ar->ep_index + pin, 0x25);
			out->audio_speakers = frb_rreg(&fAdev, ar->ep_data + pin);
		}
		out->crtc_control = frb_rreg(&fAdev, 0x1b9c + c->crtc_offset);	/* mmCRTC_CONTROL */
		out->crtc_status_frame_count = frb_rreg(&fAdev, 0x1ba6 + c->crtc_offset);	/* mmCRTC_STATUS_FRAME_COUNT */
	}
done:
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * frbctl audio: DP audio's state, and for trying things on the running
 * stream: another codec pin (FRB_AUDIO_OP_PIN; -1 = RadeonNI's choice), the
 * audio set up again, or its packets off and on (frb_dce_audio.c).
 */
IOReturn FirebrandCore::ucAudio(UInt32 op, SInt32 arg, SInt32 *result, SInt32 *dig, SInt32 *pin, UInt32 *flags)
{
	struct frb_display *d;
	int r = 0;

	IOLockLock(fLock);
	d = display();
	if (!d || !fAdev.want_audio || frb_is_soc15(&fAdev)) {	/* DCE 12: DP audio is DC's (frb_dce_audio.c is DCE 6/8/11's) */
		r = -ENODEV;
	} else {
		switch (op) {
		case FRB_AUDIO_OP_STATUS:
			break;
		case FRB_AUDIO_OP_PIN:
			if (arg < -1 || arg >= FRB_AFMT_NUM_PINS) {
				r = -EINVAL;
				break;
			}
			fAdev.audio_pin = arg;
			r = frb_audio_rerun(&fAdev, d, true);
			break;
		case FRB_AUDIO_OP_RERUN:
			r = frb_audio_rerun(&fAdev, d, false);
			break;
		case FRB_AUDIO_OP_OFF:
		case FRB_AUDIO_OP_ON:
			if (!d->crtc.conn || d->crtc.conn->dig_encoder < 0)
				r = -ENODEV;
			else
				frb_audio_dp_enable(&fAdev, d, d->crtc.conn->dig_encoder, op == FRB_AUDIO_OP_ON);
			break;
		default:
			r = -EINVAL;
			break;
		}
	}
	*result = r;
	*dig = d ? d->afmt.active_dig : -1;
	*pin = d ? d->afmt.active_pin : -1;
	*flags = d ? (d->afmt.enabled ? 1u : 0u) | (d->afmt.has_audio ? 2u : 0u) |
		     ((UInt32)(fAdev.audio_pin & 0xff) << 8) : 0;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucEdid(UInt32 idx, void *out, IOByteCount *size)
{
	struct frb_display *d;
	IOReturn r = kIOReturnSuccess;
	UInt32 n;

	IOLockLock(fLock);
	d = display();
	if (!d || idx >= (UInt32)d->num_connectors) {
		r = d ? kIOReturnBadArgument : kIOReturnNotReady;
	} else {
		n = d->conn[idx].edid_len;
		if (n > *size)
			n = *size;
		bcopy(d->conn[idx].edid, out, n);
		*size = n;
	}
	IOLockUnlock(fLock);
	return r;
}

IOReturn FirebrandCore::ucDisplayMode(UInt32 idx, UInt32 width, UInt32 height, UInt32 hz, UInt32 max_rate,
				  SInt32 *result)
{
	IOLockLock(fLock);
	*result = setMode(idx, width, height, hz, max_rate);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucDisplayOff(SInt32 *result)
{
	struct frb_display *d;

	IOLockLock(fLock);
	d = display();
	if (d)
		frb_display_off(&fAdev, d);
	*result = d ? 0 : -ENODEV;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucDisplayFill(UInt32 pattern, UInt32 arg0, UInt32 arg1, SInt32 *result)
{
	struct frb_display *d;

	IOLockLock(fLock);
	d = display();
	if (!d || !d->crtc.conn) {
		*result = -ENODEV;
	} else if (pattern >= FRB_PATTERN_COUNT ||
		   (pattern != FRB_PATTERN_SPLASH && d->crtc.fb_bpp && d->crtc.fb_bpp != 32)) {
		*result = -EINVAL;	/* the test patterns are 32 bpp; the splash is any depth */
	} else {
		fillPattern(pattern, arg0, arg1);
		*result = 0;
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * The cursor surface: the DCE's cursor_max pixels a row (the plane's pitch,
 * whatever CUR_SIZE says: 128 on DCE 8-11, RadeonNI rdn_cursor.c; 64 on
 * DCE 6), the image top left, the rest transparent; little-endian words, as
 * the display reads them. Then HDP flushed and the plane pointed at it, the
 * whole square shown (amdgpu's 128x128 cursor bo; 64x64 on DCE 6).
 */
int FirebrandCore::loadCursorLocked(UInt32 offset, const UInt32 *argb, UInt32 w, UInt32 h)
{
	struct frb_display *d = display();
	UInt32 x, y, m = frb_display_cursor_max(&fAdev);

	if (!d || !fAdev.aper || w > m || h > m ||
	    !frb_vram_ok(&fAdev, offset, kCursorBytes) || offset + kCursorBytes > fAdev.layout.reserved_start)
		return -EINVAL;
	for (y = 0; y < m; y++)
		for (x = 0; x < m; x++)
			frb_vram_write32(&fAdev, offset + 4 * (y * m + x),
					 x < w && y < h ? argb[y * w + x] : 0);
	frb_flush_hdp(&fAdev);
	return frb_display_cursor_set(&fAdev, d, offset, m, m);
}

/*
 * frbctl cursor test|move|hide|off [x y]: a known image (left half opaque
 * colour ramps, right half transparent) after the visible surface, for
 * the sink CRC. Not for use under the window server.
 */
IOReturn FirebrandCore::ucCursor(UInt32 op, SInt32 x, SInt32 y, SInt32 *result)
{
	struct frb_display *d;
	UInt32 *img, i, j, m = frb_display_cursor_max(&fAdev);

	IOLockLock(fLock);
	d = display();
	if (!d || !d->crtc.conn) {
		*result = -ENODEV;
	} else if (op == FRB_CURSOR_OP_TEST) {
		img = (UInt32 *)IOMalloc(kCursorBytes);
		if (!img) {
			*result = -ENOMEM;
		} else {
			for (j = 0; j < m; j++)
				for (i = 0; i < m; i++)
					img[j * m + i] = i < m / 2 ?
						0xff000000u | ((i * 4) & 0xff) << 16 | ((j * 2) & 0xff) << 8 |
						(((i + j) * 3) & 0xff) : 0;
			*result = loadCursorLocked(cursorOffset(d->crtc.fb_pitch * 4 * d->crtc.fb_height), img,
						   m, m);
			if (!*result)
				*result = frb_display_cursor_move(&fAdev, d, x, y, 1);
			IOFree(img, kCursorBytes);
		}
	} else if (op == FRB_CURSOR_OP_SOLID) {
		img = (UInt32 *)IOMalloc(kCursorBytes);
		if (!img) {
			*result = -ENOMEM;
		} else {
			for (j = 0; j < m; j++)
				for (i = 0; i < m; i++)
					img[j * m + i] = j < (UInt32)y ? (UInt32)x : 0;
			*result = loadCursorLocked(cursorOffset(d->crtc.fb_pitch * 4 * d->crtc.fb_height), img,
						   m, m);
			if (!*result)
				*result = frb_display_cursor_move(&fAdev, d, d->crtc.cursor_x, d->crtc.cursor_y, 1);
			IOFree(img, kCursorBytes);
		}
	} else if (op == FRB_CURSOR_OP_MOVE || op == FRB_CURSOR_OP_HIDE) {
		*result = frb_display_cursor_move(&fAdev, d, x, y, op == FRB_CURSOR_OP_MOVE);
	} else if (op == FRB_CURSOR_OP_OFF) {
		*result = frb_display_cursor_set(&fAdev, d, ~0u, 0, 0);
	} else {
		*result = -EINVAL;
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

bool FirebrandCore::screenLocked(UInt32 *offset, UInt32 *pitch, UInt32 *width, UInt32 *height, UInt32 *bpp)
{
	struct frb_display *d = display();

	if (fHeadlessWidth) {
		*offset = fHeadlessOffset;
		*pitch = fHeadlessPitch;
		*width = fHeadlessWidth;
		*height = fHeadlessHeight;
		if (bpp)
			*bpp = fHeadlessBpp ? fHeadlessBpp : 32;
		return true;
	}
	if (!d || !d->crtc.conn || !d->crtc.fb_width || !d->crtc.fb_height)
		return false;
	*offset = d->crtc.fb_offset;
	*pitch = d->crtc.fb_pitch;
	*width = d->crtc.fb_width;
	*height = d->crtc.fb_height;
	if (bpp)
		*bpp = d->crtc.fb_bpp ? d->crtc.fb_bpp : 32;
	return true;
}

IOReturn FirebrandCore::ucDetect(SInt32 *found)
{
	struct frb_display *d;

	IOLockLock(fLock);
	d = display();
	*found = d ? frb_display_detect(&fAdev, d) : -ENODEV;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucDpcdRead(UInt32 idx, UInt32 addr, UInt32 *value)
{
	struct frb_display *d;
	IOReturn r = kIOReturnSuccess;
	uint8_t b[4] = { 0, 0, 0, 0 };

	IOLockLock(fLock);
	d = display();
	if (!d || idx >= (UInt32)d->num_connectors)
		r = d ? kIOReturnBadArgument : kIOReturnNotReady;
	else if (frb_dp_dpcd_read(&fAdev, &d->conn[idx], addr, b, 4) != 4)
		r = kIOReturnIOError;
	*value = ((UInt32)b[0] << 24) | ((UInt32)b[1] << 16) | ((UInt32)b[2] << 8) | b[3];
	IOLockUnlock(fLock);
	return r;
}

IOReturn FirebrandCore::ucSinkCrc(UInt32 idx, SInt32 *result, UInt32 *r, UInt32 *g, UInt32 *b)
{
	struct frb_display *d;
	uint16_t crc[3] = { 0, 0, 0 };

	IOLockLock(fLock);
	d = display();
	if (d && idx == FRB_PIPE_CRC)
		*result = frb_display_pipe_crc(&fAdev, d, crc);
	else if (!d || idx >= (UInt32)d->num_connectors)
		*result = -ENODEV;
	else
		*result = frb_dp_sink_crc(&fAdev, &d->conn[idx], crc);
	*r = crc[0];
	*g = crc[1];
	*b = crc[2];
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * frbctl dpm high|low|auto|on|off: power_dpm_force_performance_level
 * (pp_dpm_force_performance_level), the DPM init by hand, hw_fini; then the
 * readout (frb_pm_get_status: the SMC's clocks, VDDGFX, temperature, fan,
 * package power). Like amdgpu's sysfs file, a forced level does not wait
 * for the rings; "on" and "off" are for an idle card (bring-up by hand).
 */
/* frbctl lut: the LUT store (the framebuffer fills it at every depth, headless too) and the hardware's */
IOReturn FirebrandCore::ucLut(struct frb_user_lut *out)
{
	struct frb_display *d;
	UInt32 off, pitch, w, h, bpp = 0;

	bzero(out, sizeof(*out));
	IOLockLock(fLock);
	d = display();
	if (screenLocked(&off, &pitch, &w, &h, &bpp))
		out->bpp = bpp;
	if (d) {
		memcpy(out->store, d->crtc.gamma, sizeof(out->store));
		out->hw_read = frb_display_read_lut(&fAdev, d, out->hw);
	} else {
		out->hw_read = -ENODEV;
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * frbctl lut linear|restore: the sink CRC of a known frame (frbctl fill)
 * needs the pipe's identity, which the window server's ColorSync ramp is not.
 * linear keeps the ramp it replaces; restore puts it back. Administrator only
 * (the control client is).
 */
IOReturn FirebrandCore::ucLutSet(UInt32 op, SInt32 *result)
{
	struct frb_display *d;
	uint16_t ramp[256];
	UInt32 i;

	IOLockLock(fLock);
	d = display();
	if (!d) {
		*result = -ENODEV;
	} else if (op == FRB_LUT_OP_LINEAR) {
		if (!fLutSaved)
			fLutSaved = (uint16_t *)IOMalloc(sizeof(d->crtc.gamma));
		if (!fLutSaved) {
			*result = -ENOMEM;
		} else {
			memcpy(fLutSaved, d->crtc.gamma, sizeof(d->crtc.gamma));
			for (i = 0; i < 256; i++)
				ramp[i] = (uint16_t)(i << 8);
			frb_display_set_gamma(&fAdev, d, ramp, ramp, ramp);
			*result = 0;
		}
	} else if (op == FRB_LUT_OP_RESTORE) {
		if (!fLutSaved) {
			*result = -ENOENT;
		} else {
			frb_display_set_gamma(&fAdev, d, fLutSaved, fLutSaved + 256, fLutSaved + 512);
			IOFree(fLutSaved, sizeof(d->crtc.gamma));
			fLutSaved = 0;
			*result = 0;
		}
	} else {
		*result = -EINVAL;
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucDpm(UInt32 op, struct frb_user_dpm *out)
{
	struct frb_pm_status st;
	int r = 0;
	uint32_t i;

	bzero(out, sizeof(*out));
	bzero(&st, sizeof(st));
	IOLockLock(fLock);
	/* a change is traced (frbctl trace dump); a status read keeps the last trace */
	/*
	 * frbctl dpm on N: SI's DPM switches (RDN_SI_DPM_*, FRB_SI_DPM_*) for this
	 * try, above the op; on CIK the CI DPM switches (FRB_CI_DPM_*), cleared by
	 * a plain dpm on (which starts an SMC FRB_CI_DPM_UPLOAD_ONLY left in reset)
	 */
	if ((op & 0xff) == FRB_DPM_OP_ENABLE && frb_is_cik(&fAdev))
		fAdev.ci_dpm_quirks = op >> 8;
	else if ((op & 0xff) == FRB_DPM_OP_ENABLE && (op >> 8))
		fAdev.si_dpm_quirks = op >> 8;
	op &= 0xff;
	if (op != FRB_DPM_OP_STATUS)
		frb_trace_start(&fAdev);
	if (frb_is_soc15(&fAdev)) {
		/*
		 * Vega 20: the SMU11 readout; clock gating on and off live
		 * (amdgpu's set_clockgating_state gate/ungate); forcing levels
		 * and the DPM init by hand are not ported (DPM runs in stage smu)
		 */
		switch (op) {
		case FRB_DPM_OP_STATUS:
			break;
		case FRB_DPM_OP_GATE_ON:
			r = frb_soc15_set_cg_state(&fAdev, 1);
			if (!r)
				IOLog("Firebrand: clock gating on (frbctl dpm gating on)\n");
			break;
		case FRB_DPM_OP_GATE_OFF:
			r = frb_soc15_set_cg_state(&fAdev, 0);
			if (!r)
				IOLog("Firebrand: clock gating off (frbctl dpm gating off)\n");
			break;
		case FRB_DPM_OP_FCLK_LOW:
		case FRB_DPM_OP_FCLK_TOP:
			r = frb_vega20_set_fclk_soft_min(&fAdev, op == FRB_DPM_OP_FCLK_TOP);
			break;
		default:
			r = -ENOSYS;
			break;
		}
		if (op != FRB_DPM_OP_STATUS)
			frb_trace_stop(&fAdev);
		frb_vega20_get_status(&fAdev, &st);
		IOLockUnlock(fLock);
		goto fill;
	}
	switch (op) {
	case FRB_DPM_OP_STATUS:
		break;
	case FRB_DPM_OP_HIGH:
		r = frb_pm_force_performance_level(&fAdev, AMD_DPM_FORCED_LEVEL_HIGH);
		break;
	case FRB_DPM_OP_LOW:
		r = frb_pm_force_performance_level(&fAdev, AMD_DPM_FORCED_LEVEL_LOW);
		break;
	case FRB_DPM_OP_AUTO:
		r = frb_pm_force_performance_level(&fAdev, AMD_DPM_FORCED_LEVEL_AUTO);
		break;
	case FRB_DPM_OP_ENABLE:
		if (frb_pm_enabled(&fAdev))
			r = 0;
		else if (fAdev.stage_done < (frb_is_cik(&fAdev) ? FRB_STAGE_SMU : FRB_STAGE_SDMA))
			r = -EPERM;
		else {
			fAdev.dpm_failed = 0;
			fAdev.si_dpm_wanted = 1;	/* frbctl dpm on is the request SI's opt-in waits for */
			fAdev.ci_dpm_wanted = 1;	/* and CIK's */
			r = frb_pm_hw_init(&fAdev);
			fAdev.dpm_failed = r;
			/* as at load: the watermarks and powerplay's display configuration */
			if (!r)
				frb_display_dpm_changed(&fAdev);
		}
		break;
	case FRB_DPM_OP_DISABLE:
		frb_pm_fini(&fAdev);
		/* the watermarks back to the boot clocks */
		frb_display_dpm_changed(&fAdev);
		break;
	case FRB_DPM_OP_GATE_ON:
	case FRB_DPM_OP_GATE_OFF:
		r = -ENOSYS;	/* VI's clock gating is not ported */
		break;
	default:
		r = -EINVAL;
		break;
	}
	if (op != FRB_DPM_OP_STATUS)
		frb_trace_stop(&fAdev);
	frb_pm_get_status(&fAdev, &st);
	IOLockUnlock(fLock);

fill:
	out->version = 2;
	out->result = r;
	out->enabled = st.enabled;
	out->failed = fAdev.dpm_failed;
	out->forced_level = (uint32_t)st.forced_level;
	out->sclk_count = st.sclk_count;
	out->mclk_count = st.mclk_count;
	out->pcie_count = st.pcie_count;
	for (i = 0; i < 8; i++) {
		out->sclk_levels[i] = st.sclk_levels[i];
		out->mclk_levels[i] = st.mclk_levels[i];
	}
	out->sclk_mask = st.sclk_mask;
	out->mclk_mask = st.mclk_mask;
	out->pcie_mask = st.pcie_mask;
	out->cur_sclk = st.cur_sclk;
	out->cur_mclk = st.cur_mclk;
	out->cur_sclk_index = st.cur_sclk_index;
	out->cur_mclk_index = st.cur_mclk_index;
	out->vddgfx_mv = st.vddgfx_mv;
	out->temp_mc = st.temp_mc;
	out->fan_rpm = st.fan_rpm;
	out->fan_pwm = st.fan_pwm;
	out->power_mw = st.power_mw;
	out->gpu_load = st.gpu_load;
	out->mem_load = st.mem_load;
	out->pcie_gen = st.pcie_gen;
	out->pcie_lanes = st.pcie_lanes;
	out->smc_feature_status = st.smc_feature_status;
	out->num_display = st.num_display;
	out->vrefresh = st.vrefresh;
	out->min_vblank_time = st.min_vblank_time;
	out->soc15 = frb_is_soc15(&fAdev) ? 1 : 0;
	out->cur_socclk = st.cur_socclk;
	out->cur_fclk = st.cur_fclk;
	out->temp_edge = st.temp_edge;
	out->temp_hotspot = st.temp_hotspot;
	out->temp_hbm = st.temp_hbm;
	out->gfx_count = st.gfx_count;
	out->uclk_count = st.uclk_count;
	out->soc_count = st.soc_count;
	for (i = 0; i < 16; i++) {
		out->gfx_levels[i] = st.gfx_levels[i];
		out->uclk_levels[i] = st.uclk_levels[i];
		out->soc_levels[i] = st.soc_levels[i];
	}
	out->smu_version = st.smu_version;
	out->gating = st.gating;
	out->cg_flags_lo = (uint32_t)st.cg_flags;
	out->cg_flags_hi = (uint32_t)(st.cg_flags >> 32);
	return kIOReturnSuccess;
}

IOReturn FirebrandCore::ucEngineTest(UInt32 engine, UInt32 test, SInt32 *result)
{
	IOLockLock(fLock);
	frb_trace_start(&fAdev);
	*result = frb_engine_test(&fAdev, (int)engine, (int)test);
	frb_trace_stop(&fAdev);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

/*
 * frbctl recover [gfx|sdma|all]: frb_device_gpu_recover (frb_reset.c). force
 * names the blocks to reset whatever their status says; 0 resets the busy
 * ones. after: still busy | rings failed << 8.
 */
int FirebrandCore::recoverLocked(UInt32 force, struct frb_recover_report *rep)
{
	int r;

	frb_trace_start(&fAdev);
	r = frb_device_gpu_recover(&fAdev, force, rep);
	frb_trace_stop(&fAdev);
	return r;
}

IOReturn FirebrandCore::ucRecover(UInt32 force, SInt32 *result, UInt32 *hung, UInt32 *after, UInt32 *soft,
				  UInt32 *ms)
{
	struct frb_recover_report rep;

	IOLockLock(fLock);
	*result = recoverLocked(force & (FRB_RECOVER_GFX | FRB_RECOVER_SDMA), &rep);
	IOLockUnlock(fLock);
	*hung = rep.hung;
	*after = rep.still_hung | (rep.ring_failed << 8);
	*soft = rep.soft_recovered;
	*ms = rep.ms;
	return kIOReturnSuccess;
}

/*
 * frbctl fb detach: before an unload. The window server reopens the
 * framebuffer the moment it is restarted, and its IOFramebufferUserClient
 * keeps the framebuffer (and the aperture range it was given) alive after
 * kextunload has stopped it; then the kext can never unload. Detached, the
 * framebuffer refuses new IOGraphics clients: kill the window server, and
 * nothing holds it any more.
 */
IOReturn FirebrandCore::ucFbDetach(SInt32 *result)
{
	fDetaching = true;
	IOLog("Firebrand: detaching: the framebuffer takes no new clients; restart the window server, then unload\n");
	*result = 0;
	return kIOReturnSuccess;
}

IOMemoryDescriptor *FirebrandCore::ucMemory(UInt32 type)
{
	switch (type) {
	case FRB_UC_MEMORY_TRACE:	return fTraceMem;
	case FRB_UC_MEMORY_LOG:		return fLogMem;
	default:			return 0;
	}
}


/* --- the status call (FRB_UC_STATUS): what a monitor shows ------------------ */

/* the PCI Express capability of d, 0 none */
static UInt8 pcieCapOf(IOPCIDevice *d)
{
	UInt8 ptr;
	int guard = 48;

	if (!(d->configRead16(kIOPCIConfigStatus) & 0x0010))
		return 0;
	ptr = d->configRead8(0x34) & 0xfc;
	while (ptr && guard-- > 0) {
		if (d->configRead8(ptr) == 0x10)
			return ptr;
		ptr = d->configRead8(ptr + 1) & 0xfc;
	}
	return 0;
}

/* an AMD PCI-to-PCI bridge whose PCIe capability says Upstream Port (PCI_EXP_TYPE_UPSTREAM, 5) */
static bool amdUpstreamPort(IOPCIDevice *d, UInt8 *cap)
{
	UInt8 c;

	if (d->configRead16(kIOPCIConfigVendorID) != 0x1002 ||
	    (d->configRead32(kIOPCIConfigRevisionID) >> 8) != 0x060400)
		return false;
	c = pcieCapOf(d);
	if (!c || ((d->configRead16(c + 2) >> 4) & 0xf) != 5)
		return false;
	*cap = c;
	return true;
}

/*
 * The PCIe link the card reaches the host through. Vega 10 and 20 carry a
 * switch in the package (upstream port 1002:14a0, downstream 14a1): the GPU
 * function's own Link Status is the switch's internal link (16 GT/s x16 on
 * the MI50 whatever the slot), and the slot's is the upstream port's, where
 * lspci reads it. QorIQPCIBridge publishes the switch's ports as IOPCIDevice
 * siblings of the GPU; a stock IOPCIFamily has them as its ancestors, behind
 * IOPCI2PCIBridge. Either way: an AMD upstream port on a lower bus than the
 * GPU's, the nearest one. None (Polaris, SI): the GPU's own link is the slot's.
 */
void FirebrandCore::findSlotPort(void)
{
	IORegistryEntry *e, *parent = fDevice->getParentEntry(gIOServicePlane);
	IOPCIDevice *d, *best = 0;
	OSIterator *it;
	UInt8 bus = fDevice->getBusNumber(), cap, bestCap = 0;

	for (e = parent; e; e = e->getParentEntry(gIOServicePlane)) {
		d = OSDynamicCast(IOPCIDevice, e);
		if (d && d->getBusNumber() < bus && amdUpstreamPort(d, &cap) &&
		    (!best || d->getBusNumber() > best->getBusNumber())) {
			best = d;
			bestCap = cap;
		}
	}
	it = parent ? parent->getChildIterator(gIOServicePlane) : 0;
	while (it && (e = (IORegistryEntry *)it->getNextObject())) {
		d = OSDynamicCast(IOPCIDevice, e);
		if (d && d != fDevice && d->getBusNumber() < bus && amdUpstreamPort(d, &cap) &&
		    (!best || d->getBusNumber() > best->getBusNumber())) {
			best = d;
			bestCap = cap;
		}
	}
	if (it)
		it->release();
	if (best) {
		best->retain();
		fSlotPort = best;
		fSlotCap = bestCap;
		IOLog("Firebrand: the card's link to the host is %04x:%04x's (bus %u), an upstream port in the package\n",
		      best->configRead16(kIOPCIConfigVendorID), best->configRead16(kIOPCIConfigDeviceID),
		      (unsigned)best->getBusNumber());
	}
}

/*
 * The sensors (frb_pm_read_sensors): at most one sample every kSensorsMinMs
 * whoever asks, so any number of monitors costs the SMU one sample per
 * period; after a read the SMU did not acknowledge, none for kSensorsHoldMs
 * (each unanswered message waits out the SMU's timeout with the lock held).
 */
void FirebrandCore::sampleSensorsLocked(void)
{
	UInt64 now = os_time_ms(this);

	if (fSensorsAt && now - fSensorsAt < kSensorsMinMs)
		return;
	if (fSensorsHoldUntil && now < fSensorsHoldUntil)
		return;
	fSensorsResult = frb_pm_read_sensors(&fAdev, &fSensors);
	fSensorsAt = now;
	if (fSensorsResult && fSensorsResult != -ENODEV) {
		fSensorsHoldUntil = now + kSensorsHoldMs;
		IOLog("Firebrand: status: the sensor read failed (%d); the next in %u s\n", fSensorsResult,
		      (unsigned)(kSensorsHoldMs / 1000));
	}
}

static void statusCopy(char *dst, size_t cap, const char *src)
{
	strncpy(dst, src ? src : "", cap - 1);
	dst[cap - 1] = 0;
}

/* the connectors, CRTC0's mode, DP audio: the display's own state, the hot-plug sense read now */
void FirebrandCore::statusDisplayLocked(struct frb_user_status *out)
{
	struct frb_display *d = display();
	struct frb_crtc *c;
	int i;

	if (!d)
		return;
	c = &d->crtc;
	out->valid |= FRB_ST_DISPLAY;
	out->num_connectors = d->num_connectors < 6 ? d->num_connectors : 6;
	for (i = 0; i < (int)out->num_connectors; i++) {
		struct frb_connector *s = &d->conn[i];
		struct frb_user_status_conn *u = &out->conn[i];
		struct frb_edid_audio aud;

		u->type = s->connector_type;
		u->connected = frb_display_hpd_sense(&fAdev, s) ? 1 : 0;
		if (s->dpcd[0]) {
			u->dp_max_rate_khz = s->dpcd[1] * 27000u;
			u->dp_max_lanes = s->dpcd[2] & 0x1f;
		}
		if (s->edid_len && frb_edid_extract_audio(s->edid, s->edid_len, &aud))
			statusCopy(u->sink, sizeof(u->sink), aud.sink_name);
		if (c->conn != s)
			continue;
		u->active = 1;
		u->width = c->mode.hdisplay;
		u->height = c->mode.vdisplay;
		u->pixel_khz = c->mode.clock;
		if (c->mode.htotal && c->mode.vtotal)
			u->refresh_chz = (UInt16)(((c->mode.clock * 1000u) / c->mode.htotal * 100u) / c->mode.vtotal);
		u->bpc = (UInt8)(frb_is_soc15(&fAdev) ? d->dce12.bpc : c->bpc);
		if (s->dpcd[0]) {
			u->dp_rate_khz = s->dp_clock;
			u->dp_lanes = (UInt8)s->dp_lane_count;
		}
		if (s->edid_len && frb_edid_extract_audio(s->edid, s->edid_len, &aud))
			out->audio_sink = aud.has_audio ? 1 : 0;
	}
	out->valid |= FRB_ST_AUDIO;
	if (frb_is_soc15(&fAdev)) {
		/* DCE 12: the stream's Azalia endpoint and its DIG (the link's transmitter) */
		out->audio_endpoint = d->dce12.audio_ep;
		out->audio_on = d->dce12.audio_on ? 1 : 0;
		out->audio_dig = d->dce12.audio_ep >= 0 && c->conn ? c->conn->transmitter : -1;
	} else {
		out->audio_endpoint = d->afmt.enabled ? d->afmt.active_pin : -1;
		out->audio_on = d->afmt.enabled ? 1 : 0;
		out->audio_dig = d->afmt.enabled ? d->afmt.active_dig : -1;
	}
}

IOReturn FirebrandCore::ucStatus(struct frb_user_status *out)
{
	struct frb_device *adev = &fAdev;
	const struct frb_pm_sensors *s = &fSensors;
	UInt64 vram;
	UInt32 i, n;
	UInt16 v;

	bzero(out, sizeof(*out));
	out->version = FRB_STATUS_VERSION;
	out->size = sizeof(*out);
	IOLockLock(fLock);
	out->vendor = adev->vendor;
	out->device = adev->device;
	out->subsys_vendor = adev->subsys_vendor;
	out->subsys_device = adev->subsys_device;
	out->revision = adev->revision;
	out->asic_type = adev->asic_type;
	out->family = adev->family;
	out->stage_done = adev->stage_done;
	out->row_max_stage = adev->row ? adev->row->max_stage : 0xff;
	out->stage_failed = adev->stage_failed;
	out->flags = (fBringUp ? FRB_SF_BRINGUP : 0) | (fOwner && fOwner->metaCast("FirebrandFramebuffer") ? FRB_SF_FRAMEBUFFER : 0) |
		     (fAccelWanted ? FRB_SF_ACCEL : 0) | (fDpmWanted ? FRB_SF_DPM : 0) | (fAudioWanted ? FRB_SF_AUDIO : 0) |
		     (fGating ? FRB_SF_GATING : 0) | (adev->row && adev->row->untested ? FRB_SF_UNTESTED : 0) |
		     (adev->unlocked ? FRB_SF_UNLOCKED : 0);
	statusCopy(out->asic, sizeof(out->asic), frb_asic_name(adev->asic_type));
	statusCopy(out->board, sizeof(out->board), adev->row ? adev->row->board : "");
	statusCopy(out->driver_version, sizeof(out->driver_version), kmod_info.version);
	snprintf(out->build, sizeof(out->build), "%s%s%s", FRB_BUILD_ID, FRB_BUILD_ID[0] ? ", " : "", __DATE__);
	out->support = !adev->row || !adev->row->untested ? FRB_SUPPORT_VERIFIED :
		       frb_asic_ported(adev->asic_type) ? FRB_SUPPORT_UNTESTED : FRB_SUPPORT_NOT_PORTED;
	out->stage_cap = frb_stage_cap(adev);
	out->stage_failed_at = adev->stage_failed ? adev->stage_failed_at : 0;
	statusCopy(out->vbios_pn, sizeof(out->vbios_pn), adev->vbios_pn);
	statusCopy(out->vbios_ver, sizeof(out->vbios_ver), adev->vbios_ver);
	statusCopy(out->vbios_date, sizeof(out->vbios_date), adev->vbios_date);
	statusCopy(out->stage_error, sizeof(out->stage_error), adev->stage_failed ? fStageError : "");
	out->boot_gfx_mhz = adev->default_sclk / 100;
	out->boot_mem_mhz = adev->default_mclk / 100;
	vram = adev->gmc.real_vram_size ? adev->gmc.real_vram_size : adev->gmc.mc_vram_size;
	out->vram_mb = (UInt32)(vram >> 20);
	out->vram_visible_mb = (UInt32)((adev->aper_size && adev->aper_size < vram ? adev->aper_size : vram) >> 20);
	out->vram_width = adev->gmc.vram_width;
	if (fStopped || !fRegs) {
		out->flags |= FRB_SF_STOPPED;
		IOLockUnlock(fLock);
		return kIOReturnSuccess;
	}

	/* power management: the switch, the init's outcome, then the sample */
	if (!fDpmWanted)
		out->dpm_state = FRB_DPM_STATE_OFF;
	else if (adev->dpm_failed)
		out->dpm_state = FRB_DPM_STATE_FAILED;
	else if (frb_pm_dpm_running(adev))
		out->dpm_state = FRB_DPM_STATE_ON;
	else
		out->dpm_state = FRB_DPM_STATE_PENDING;
	out->dpm_failed = adev->dpm_failed;
	sampleSensorsLocked();
	out->sensor_age_ms = fSensorsAt ? (UInt32)(os_time_ms(this) - fSensorsAt) : 0;
	if (s->valid & FRB_PMS_CLOCKS) {
		out->gfx_mhz = s->gfx_mhz;
		out->mem_mhz = s->mem_mhz;
		out->valid |= FRB_ST_CLOCKS;
	}
	if (s->valid & FRB_PMS_SOC_CLOCKS) {
		out->soc_mhz = s->soc_mhz;
		out->fclk_mhz = s->fclk_mhz;
		out->valid |= FRB_ST_SOC_CLOCKS;
	}
	if (s->valid & FRB_PMS_LEVELS) {
		out->gfx_level_count = s->gfx_count;
		out->mem_level_count = s->mem_count;
		for (i = 0; i < 16; i++) {
			out->gfx_levels[i] = (UInt16)s->gfx_levels[i];
			out->mem_levels[i] = (UInt16)s->mem_levels[i];
		}
		out->valid |= FRB_ST_LEVELS;
	}
	if (s->valid & FRB_PMS_TEMP) {
		out->temp_mc = s->temp_mc;
		out->valid |= FRB_ST_TEMP;
	}
	if (s->valid & FRB_PMS_TEMP_HOTSPOT) {
		out->temp_hotspot_mc = s->temp_hotspot_mc;
		out->valid |= FRB_ST_TEMP_HOTSPOT;
	}
	if (s->valid & FRB_PMS_TEMP_MEM) {
		out->temp_mem_mc = s->temp_mem_mc;
		out->valid |= FRB_ST_TEMP_MEM;
	}
	if (s->valid & FRB_PMS_POWER) {
		out->power_mw = s->power_mw;
		out->valid |= FRB_ST_POWER;
	}
	if (s->valid & FRB_PMS_POWER_LIMIT) {
		out->power_limit_w = s->power_limit_w;
		out->valid |= FRB_ST_POWER_LIMIT;
	}
	if (s->valid & FRB_PMS_LOAD) {
		out->gpu_load = s->gpu_load;
		out->mem_load = s->mem_load;
		out->valid |= FRB_ST_LOAD;
	}
	if (s->valid & FRB_PMS_VDDGFX) {
		out->vddgfx_mv = s->vddgfx_mv;
		out->valid |= FRB_ST_VDDGFX;
	}
	if (s->valid & FRB_PMS_FAN) {
		out->fan_rpm = s->fan_rpm;
		out->fan_pwm = s->fan_pwm;
		out->valid |= FRB_ST_FAN;
	}
	if (s->valid & FRB_PMS_THROTTLE) {
		out->throttler = s->throttler;
		out->valid |= FRB_ST_THROTTLE;
	}
	if (s->valid & FRB_PMS_SMU) {
		out->smu_version = s->smu_version;
		out->features_kind = s->features_kind;
		out->features_lo = (UInt32)s->features;
		out->features_hi = (UInt32)(s->features >> 32);
		out->valid |= FRB_ST_SMU;
	}
	if (frb_is_soc15(adev)) {
		/* SOC15 clock gating (VI's is not ported: no bit, "not available") */
		out->gating = (adev->want_gating ? 1u : 0u) | (adev->cg_gated ? 2u : 0u);
		out->cg_flags_lo = (UInt32)adev->cg_flags;
		out->cg_flags_hi = (UInt32)(adev->cg_flags >> 32);
		out->valid |= FRB_ST_GATING;
	} else if (frb_is_si(adev) || frb_is_cik(adev)) {
		/* SI, CIK: gated at the end of stage sdma unless frb_cg=0 (si_sdma_stage, cik_sdma_stage) */
		out->gating = (adev->no_cg ? 0u : 1u) | (adev->cg_on ? 2u : 0u);
		out->cg_flags_lo = (UInt32)adev->cg_flags;
		out->cg_flags_hi = (UInt32)(adev->cg_flags >> 32);
		out->valid |= FRB_ST_GATING;
	}

	/* the video decoder: the driver's bookkeeping, never UVD's registers (a gated block) */
	out->valid |= FRB_ST_UVD;
	if (adev->uvd && adev->stage_done >= FRB_STAGE_UVD) {
		for (i = 0, n = 0; i < adev->uvd->num_uvd_inst && i < AMDGPU_MAX_UVD_INSTANCES; i++)
			if (!(adev->uvd->harvest_config & (1u << i)))
				n++;
		out->uvd_instances = n;
		out->uvd_state = adev->uvd->powered ? FRB_UVD_STATE_RUNNING : FRB_UVD_STATE_IDLE;
		out->uvd_fw_version = adev->uvd->fw_version;
		out->uvd_sessions = fVideoSessions;
		out->uvd_flags = fVideoPoisoned ? FRB_UVD_FLAG_POISONED : 0;
	}

	/* PCIe: Link Status / Link Capabilities from configuration space, read now */
	if (fPcieCap) {
		UInt32 cap = fDevice->configRead32(fPcieCap + 0x0c);

		v = fDevice->configRead16(fPcieCap + 0x12);
		if (v != 0xffff) {
			out->link_speed = v & 0xf;
			out->link_width = (v >> 4) & 0x3f;
			out->link_max_speed = cap & 0xf;
			out->link_max_width = (cap >> 4) & 0x3f;
			out->valid |= FRB_ST_LINK;
		}
	}
	if (fSlotPort && fSlotCap) {
		UInt32 cap = fSlotPort->configRead32(fSlotCap + 0x0c);

		v = fSlotPort->configRead16(fSlotCap + 0x12);
		if (v != 0xffff) {
			out->slot_speed = v & 0xf;
			out->slot_width = (v >> 4) & 0x3f;
			out->slot_max_speed = cap & 0xf;
			out->slot_max_width = (cap >> 4) & 0x3f;
			out->slot_vendor = fSlotPort->configRead16(kIOPCIConfigVendorID);
			out->slot_device = fSlotPort->configRead16(kIOPCIConfigDeviceID);
			out->valid |= FRB_ST_SLOT_LINK;
		}
	} else if (out->valid & FRB_ST_LINK) {
		out->slot_speed = out->link_speed;
		out->slot_width = out->link_width;
		out->slot_max_speed = out->link_max_speed;
		out->slot_max_width = out->link_max_width;
		out->slot_vendor = adev->vendor;
		out->slot_device = adev->device;
		out->valid |= FRB_ST_SLOT_LINK;
	}

	/* memory: the accelerator's heaps, the GART's pages */
	if (fVramMgr && fVramMgr->ready) {
		out->vram_heap_kb = (UInt32)(((fVramMgr->vis_end - fVramMgr->vis_start) +
					      (fVramMgr->invis_end - fVramMgr->invis_start)) >> 10);
		out->vram_heap_used_kb = (UInt32)((fVramMgr->vis_used + fVramMgr->invis_used) >> 10);
		out->valid |= FRB_ST_VRAM_HEAP;
	}
	if (adev->gart.ready) {
		out->gart_kb = adev->gart.num_gpu_pages * (AMDGPU_GPU_PAGE_SIZE >> 10);
		out->gart_used_kb = adev->gart.used_pages * (AMDGPU_GPU_PAGE_SIZE >> 10);
		out->valid |= FRB_ST_GART;
	}

	statusDisplayLocked(out);
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}
