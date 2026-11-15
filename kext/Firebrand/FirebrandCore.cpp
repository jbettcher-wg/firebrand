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

#include "FirebrandCore.h"
extern "C" {
#include "frb_ring.h"
#include "frb_pm.h"
#include "frb_splash.h"
}

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

/* --- firmware from the personality ---------------------------------------- */

int FirebrandCore::firmware(const char *name, const uint8_t **data, uint32_t *size)
{
	OSData *d = fFirmware ? OSDynamicCast(OSData, fFirmware->getObject(name)) : 0;

	if (!d || !d->getLength()) {
		IOLog("Firebrand: firmware %s is not in the kext (scripts/fetch-firmware.sh, rebuild)\n", name);
		return -ENOENT;
	}
	*data = (const uint8_t *)d->getBytesNoCopy();
	*size = d->getLength();
	return 0;
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

bool FirebrandCore::mapAperture(void)
{
	if (fFbMap)
		return true;
	fFbMap = fDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!fFbMap) {
		IOLog("Firebrand: cannot map BAR0\n");
		return false;
	}
	fAdev.aper = (volatile uint8_t *)fFbMap->getVirtualAddress();
	fAdev.aper_size = (uint32_t)fFbMap->getLength();
	IOLog("Firebrand: BAR0 mapped, %lu MB\n", (unsigned long)(fAdev.aper_size >> 20));
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
	frb_gart_bind(&fAdev, first, npages, bus, FRB_PTE_GTT);
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
	int r;

	if (stage >= FRB_STAGE_COUNT)
		return -EINVAL;
	if (stage >= FRB_STAGE_GMC && (!mapAperture() || !allocDummyPage())) {
		IOLog("Firebrand: stages to %s: no aperture or dummy page\n", frb_stage_name((enum frb_stage)stage));
		return -ENOMEM;
	}
	frb_trace_start(&fAdev);
	r = frb_device_init(&fAdev, (enum frb_stage)stage);
	frb_trace_stop(&fAdev);
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
 * only the 256 MB BAR0. System Profiler reads VRAM,totalsize as a SIGNED
 * 32-bit byte count, so 2 GB and up is capped at 2047 MB there (and in
 * ATY,memsize); VRAM,totalMB carries the whole count, VRAM,totalsize64 the
 * bytes. Host-order numbers, as RadeonNI's. Also the model name.
 */
void FirebrandCore::publishMemory(void)
{
	UInt32 memoryMB = frb_vi_get_config_memsize(&fAdev), memoryBytes, visibleMB;
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

	/* the microcode leaves the property table (serializeProperties) */
	fw = OSDynamicCast(OSDictionary, fOwner->getProperty("Firmware"));
	if (fw) {
		fFirmware = fw;
		fFirmware->retain();
		fOwner->removeProperty("Firmware");
	}
	fAllowRegWrite = flag("FirebrandAllowRegWrite", "frb_regwrite");
	fBringUp = flag("FirebrandBringUp", "frb_bringup");
	fAdopt = flag("FirebrandAdopt", "frb_adopt");
	fDisplayAuto = flag("FirebrandDisplay", "frb_display");
	fAccelWanted = flag("FirebrandAccel", "frb_accel");
	fDpmWanted = flag("FirebrandDPM", "frb_dpm");
	fAudioWanted = flag("FirebrandAudio", "frb_audio");
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
	fAdev.asic_type = frb_asic_type_of(fAdev.device);
	fAdev.row = frb_asic_lookup(fAdev.device, fAdev.revision, fAdev.subsys_vendor, fAdev.subsys_device);
	fAdev.num_crtc = 6;	/* DCE 11.2 (dce_v11_0 mode_info.num_crtc for Polaris) */
	fAdev.allow_unverified = fBringUp;
	fAdev.adopt = fAdopt;
	/* the accelerator's legacy clients take the GART start as 32 bits */
	fAdev.gart_legacy = fAccelWanted;
	fAdev.trace = (struct frb_trace_ent *)fTraceMem->getBytesNoCopy();
	fAdev.trace_cap = FRB_TRACE_ENTRIES;
	fAdev.want_dpm = fDpmWanted;
	fAdev.want_audio = fAudioWanted;
	fAdev.audio_pin = -1;		/* RadeonNI's pin choice until frbctl audio pin N */
	readPcieCaps();

	IOLog("Firebrand: %04x:%04x rev %02x subsystem %04x:%04x, %s, command %04x; %s\n",
	      fAdev.vendor, fAdev.device, fAdev.revision, fAdev.subsys_vendor, fAdev.subsys_device,
	      frb_asic_name(fAdev.asic_type), fDevice->configRead16(kIOPCIConfigCommand),
	      fAdev.row ? fAdev.row->board : "no row in the family table: survey only");

	/* amdgpu_device_init: the register BAR is 5 on VI (256 KB) */
	fDevice->setMemoryEnable(true);
	fRegMap = fDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!fRegMap) {
		IOLog("Firebrand: cannot map the register BAR (BAR5)\n");
		goto fail;
	}
	fRegs = (volatile UInt8 *)fRegMap->getVirtualAddress();
	fAdev.rmmio_size = (uint32_t)fRegMap->getLength();
	IOLog("Firebrand: registers: BAR5, %lu KB\n", (unsigned long)(fAdev.rmmio_size >> 10));

	if (!loadBios()) {
		IOLog("Firebrand: no usable VBIOS; nothing done\n");
		goto fail;
	}

	/* the survey: reads only */
	frb_vi_common_early_init(&fAdev);
	frb_atombios_survey(&fAdev);
	fAdev.stage_done = FRB_STAGE_SURVEY;
	fOwner->setProperty("FirebrandASIC", frb_asic_name(fAdev.asic_type));
	if (fAdev.row)
		fOwner->setProperty("FirebrandBoard", fAdev.row->board);
	publishState();

	if (!fAdev.row) {
		/* a Polaris nobody has verified: the survey is all it gets */
		IOLog("Firebrand: survey done; not staying on a card without a row\n");
		goto fail;
	}

	stage = configuredStage();
	if (stage > fAdev.row->max_stage && !fBringUp) {
		IOLog("Firebrand: stage %lu asked, %s is verified to stage %s; running that far\n",
		      (unsigned long)stage, fAdev.row->board,
		      frb_stage_name((enum frb_stage)fAdev.row->max_stage));
		stage = fAdev.row->max_stage;
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
	}
	IOLog("Firebrand: ready (stage %s); frbctl talks to it\n", frb_stage_name(fAdev.stage_done));
	return true;

fail:
	stop();
	return false;
}

void FirebrandCore::stop(void)
{
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
	if (fLock)
		IOLockFree(fLock);
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
			u->dig_encoder = (int8_t)s->dig_encoder;
			u->sense = frb_dce_v11_0_hpd_sense(&fAdev, s->hpd);
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
		/* that codec pin's HOT_PLUG_CONTROL, CHANNEL_SPEAKER (AZF0ENDPOINTn index/data, 4 apart) */
		frb_wreg(&fAdev, 0x17a8 + 4 * out->audio_ep, 0x54);
		out->audio_hot_plug = frb_rreg(&fAdev, 0x17a9 + 4 * out->audio_ep);
		frb_wreg(&fAdev, 0x17a8 + 4 * out->audio_ep, 0x25);
		out->audio_speakers = frb_rreg(&fAdev, 0x17a9 + 4 * out->audio_ep);
		out->crtc_control = frb_rreg(&fAdev, 0x1b9c + c->crtc_offset);	/* mmCRTC_CONTROL */
		out->crtc_status_frame_count = frb_rreg(&fAdev, 0x1ba6 + c->crtc_offset);	/* mmCRTC_STATUS_FRAME_COUNT */
	}
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
	if (!d || !fAdev.want_audio) {
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
 * The cursor surface: FRB_CURSOR_MAX pixels a row (the plane's pitch on DCE
 * 10/11, whatever CUR_SIZE says: RadeonNI rdn_cursor.c, DCE8 on), the
 * image top left, the rest transparent; little-endian words, as the
 * display reads them. Then HDP flushed and the plane pointed at it, the
 * whole FRB_CURSOR_MAX square shown (amdgpu's 128x128 cursor bo).
 */
int FirebrandCore::loadCursorLocked(UInt32 offset, const UInt32 *argb, UInt32 w, UInt32 h)
{
	struct frb_display *d = display();
	UInt32 x, y;

	if (!d || !fAdev.aper || w > FRB_CURSOR_MAX || h > FRB_CURSOR_MAX ||
	    !frb_vram_ok(&fAdev, offset, kCursorBytes) || offset + kCursorBytes > fAdev.layout.reserved_start)
		return -EINVAL;
	for (y = 0; y < FRB_CURSOR_MAX; y++)
		for (x = 0; x < FRB_CURSOR_MAX; x++)
			frb_vram_write32(&fAdev, offset + 4 * (y * FRB_CURSOR_MAX + x),
					 x < w && y < h ? argb[y * w + x] : 0);
	frb_flush_hdp(&fAdev);
	return frb_display_cursor_set(&fAdev, d, offset, FRB_CURSOR_MAX, FRB_CURSOR_MAX);
}

/*
 * frbctl cursor test|move|hide|off [x y]: a known image (left half opaque
 * colour ramps, right half transparent) after the visible surface, for
 * the sink CRC. Not for use under the window server.
 */
IOReturn FirebrandCore::ucCursor(UInt32 op, SInt32 x, SInt32 y, SInt32 *result)
{
	struct frb_display *d;
	UInt32 *img, i, j;

	IOLockLock(fLock);
	d = display();
	if (!d || !d->crtc.conn) {
		*result = -ENODEV;
	} else if (op == FRB_CURSOR_OP_TEST) {
		img = (UInt32 *)IOMalloc(kCursorBytes);
		if (!img) {
			*result = -ENOMEM;
		} else {
			for (j = 0; j < FRB_CURSOR_MAX; j++)
				for (i = 0; i < FRB_CURSOR_MAX; i++)
					img[j * FRB_CURSOR_MAX + i] = i < FRB_CURSOR_MAX / 2 ?
						0xff000000u | ((i * 4) & 0xff) << 16 | ((j * 2) & 0xff) << 8 |
						(((i + j) * 3) & 0xff) : 0;
			*result = loadCursorLocked(cursorOffset(d->crtc.fb_pitch * 4 * d->crtc.fb_height), img,
						   FRB_CURSOR_MAX, FRB_CURSOR_MAX);
			if (!*result)
				*result = frb_display_cursor_move(&fAdev, d, x, y, 1);
			IOFree(img, kCursorBytes);
		}
	} else if (op == FRB_CURSOR_OP_SOLID) {
		img = (UInt32 *)IOMalloc(kCursorBytes);
		if (!img) {
			*result = -ENOMEM;
		} else {
			for (j = 0; j < FRB_CURSOR_MAX; j++)
				for (i = 0; i < FRB_CURSOR_MAX; i++)
					img[j * FRB_CURSOR_MAX + i] = j < (UInt32)y ? (UInt32)x : 0;
			*result = loadCursorLocked(cursorOffset(d->crtc.fb_pitch * 4 * d->crtc.fb_height), img,
						   FRB_CURSOR_MAX, FRB_CURSOR_MAX);
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
	if (!d || idx >= (UInt32)d->num_connectors)
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

IOReturn FirebrandCore::ucDpm(UInt32 op, struct frb_user_dpm *out)
{
	struct frb_pm_status st;
	int r = 0;
	uint32_t i;

	bzero(out, sizeof(*out));
	IOLockLock(fLock);
	/* a change is traced (frbctl trace dump); a status read keeps the last trace */
	if (op != FRB_DPM_OP_STATUS)
		frb_trace_start(&fAdev);
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
		else if (fAdev.stage_done < FRB_STAGE_SDMA)
			r = -EPERM;
		else {
			fAdev.dpm_failed = 0;
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
	default:
		r = -EINVAL;
		break;
	}
	if (op != FRB_DPM_OP_STATUS)
		frb_trace_stop(&fAdev);
	frb_pm_get_status(&fAdev, &st);
	IOLockUnlock(fLock);

	out->version = 1;
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

