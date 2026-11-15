/* DMA addresses through IOKit, shared by Firebrand and HDAudio. */
/*
 * qoriq_dma.h -- a device's bus address for memory, the way IOKit intends:
 * from an IODMACommand over the prepared descriptor, kMapped, with the system
 * mapper (NULL). Header only, no symbols beyond the IOKit KPI set, so a kext
 * that includes it links on every kernel this port has booted.
 *
 * WHERE THE BUS ADDRESS COMES FROM, by machine:
 *
 *   a Power Mac G5   IOMapper::gSystem is Apple's DART and IODMACommand
 *                    returns the DART's I/O address. Nothing is added.
 *
 *   the T4240, kernel translating
 *                    the QorIQ PCI bridge registered its inbound windows with
 *                    the kernel ("QorIQPCIe-dma-linear"), IODMACommand adds
 *                    the window's offset itself, and the bridge publishes
 *                    PCIInboundDMATranslated = true and PCIInboundDMAOffset = 0.
 *                    Nothing is added.
 *
 *   the T4240, kernel not translating
 *                    IODMACommand returns the CPU physical address, and the
 *                    first PCIInboundDMAOffset up the service plane from the
 *                    device is added (0x10_00000000 for u-boot's window 2).
 *
 *   anything else    no mapper, no bridge property: the CPU physical address.
 *
 * So a driver has one code path, and nothing is ever translated twice: the
 * offset is added only where nothing else adds it.
 *
 * NOT FOR ON-CHIP MASTERS. The DPAA's FMan and the SoC's EHCI are CoreNet
 * masters that see CPU physical addresses; they never sit under a PCI bridge
 * and must ask for physical addresses (kIOMapperNone, getPhysicalSegment64).
 */
#ifndef QORIQ_DMA_H
#define QORIQ_DMA_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>

#define kQorIQDMAOffsetKey	"PCIInboundDMAOffset"
#define kQorIQDMATranslatedKey	"PCIInboundDMATranslated"

/*
 * The switch every migrated driver reads: boot-arg qdmacmd=0|1 wins, then the
 * personality's QorIQDMACommand (boolean or number), else OFF. Off means the
 * driver's own pre-migration code, unchanged, so a boot with it off issues
 * exactly the writes it always did.
 */
static inline bool
qoriqDMACommandSwitch( IOService * driver, const char ** from )
{
    unsigned int v = 0;
    OSObject *	 o;

    if (PE_parse_boot_arg("qdmacmd", &v)) {
	if (from) *from = "boot-arg qdmacmd";
	return (0 != v);
    }
    o = driver ? driver->getProperty("QorIQDMACommand") : 0;
    if (OSDynamicCast(OSBoolean, o)) {
	if (from) *from = "personality QorIQDMACommand";
	return ((OSBoolean *) o)->isTrue();
    }
    if (OSDynamicCast(OSNumber, o)) {
	if (from) *from = "personality QorIQDMACommand";
	return (0 != ((OSNumber *) o)->unsigned32BitValue());
    }
    if (from) *from = "default";
    return false;
}

/*
 * What to add to IODMACommand's addresses for `device' (its IOPCIDevice):
 * 0 under a system mapper or a translating kernel, else the bridge's offset.
 * `how' says which, for the driver's log line. When an offset is added,
 * *cpuLimit (if asked) is where that window ends in CPU addresses -- the
 * bridge's PCIInboundDMABase + PCIInboundDMASize -- and 0 otherwise (the
 * mapper or the kernel limits what IODMACommand returns itself).
 */
static inline UInt64
qoriqDMAAddFor( IORegistryEntry * device, const char ** how,
		UInt64 * cpuLimit = 0 )
{
    IORegistryEntry * e;

    if (cpuLimit)
	*cpuLimit = 0;

    IOMapper::checkForSystemMapper();
    if (IOMapper::gSystem) {
	if (how) *how = "the system mapper's addresses";
	return 0;
    }
    for (e = device; e; e = e->getParentEntry(gIOServicePlane)) {
	OSBoolean * t = OSDynamicCast(OSBoolean, e->getProperty(kQorIQDMATranslatedKey));
	OSNumber *  n = OSDynamicCast(OSNumber, e->getProperty(kQorIQDMAOffsetKey));

	if (t && t->isTrue()) {
	    if (how) *how = "translated by the kernel (" kQorIQDMATranslatedKey ")";
	    return 0;
	}
	if (n) {
	    OSNumber * b = OSDynamicCast(OSNumber, e->getProperty("PCIInboundDMABase"));
	    OSNumber * z = OSDynamicCast(OSNumber, e->getProperty("PCIInboundDMASize"));

	    if (cpuLimit && b && z)
		*cpuLimit = b->unsigned64BitValue() + z->unsigned64BitValue();
	    if (how) *how = "physical + the bridge's " kQorIQDMAOffsetKey;
	    return n->unsigned64BitValue();
	}
    }
    if (how) *how = "physical (no mapper, no " kQorIQDMAOffsetKey " above the device)";
    return 0;
}

/*
 * The bus address of `md' at `offset', and in *length how far it runs
 * contiguously on the bus. md must already be prepared (an
 * IOBufferMemoryDescriptor is). kIterateOnly: the descriptor's own prepare()
 * holds the pages, so the command only walks and is gone on return. 0 if
 * there is no address, or the device could not reach it in `bits'.
 */
static inline UInt64
qoriqDMABusAddress( IOMemoryDescriptor * md, UInt64 offset, UInt64 * length,
		    UInt8 bits, UInt64 add, UInt64 cpuLimit = 0 )
{
    IODMACommand *	     cmd;
    IODMACommand::Segment64 seg;
    UInt32		     n = 1;
    UInt64		     at = offset, bus = 0;

    if (length)
	*length = 0;
    if (!md)
	return 0;
    cmd = IODMACommand::withSpecification(IODMACommand::OutputHost64, bits, 0,
		(IODMACommand::MappingOptions)
		    (IODMACommand::kMapped | IODMACommand::kIterateOnly),
		0, 1);
    if (!cmd)
	return 0;
    if (kIOReturnSuccess == cmd->setMemoryDescriptor(md, true)) {
	seg.fIOVMAddr = 0;
	seg.fLength = 0;
	if (kIOReturnSuccess == cmd->gen64IOVMSegments(&at, &seg, &n)
	    && 1 == n && seg.fIOVMAddr) {
	    bus = seg.fIOVMAddr + add;
	    if (bus < seg.fIOVMAddr
		|| (bits < 64 && ((bus + seg.fLength - 1) >> bits))
		|| (cpuLimit && seg.fIOVMAddr + seg.fLength > cpuLimit))
		bus = 0;
	    else if (length)
		*length = seg.fLength;
	}
	cmd->clearMemoryDescriptor();
    }
    cmd->release();
    return bus;
}

/*
 * Page by page over one descriptor, for a GART or any per-page table: one
 * IODMACommand for the whole walk (kIterateOnly, segments cut at `page'),
 * so a 256 MB binding does not make 65536 commands. `active' false makes it
 * do nothing at all -- the driver's switch-off path never touches IOKit's
 * DMA code. page() is the bus address of the whole page at `offset' (which
 * must be page aligned), or 0 when there is none, it is not a whole aligned
 * page, or the device could not reach it.
 */
class QorIQDMAWalk {
public:
    QorIQDMAWalk( IOMemoryDescriptor * md, bool active, UInt8 bits, UInt64 add,
		  UInt64 cpuLimit, UInt32 page )
	: fCmd(0), fBits(bits), fAdd(add), fLimit(cpuLimit), fPage(page)
    {
	if (!active || !md || !page)
	    return;
	fCmd = IODMACommand::withSpecification(IODMACommand::OutputHost64,
		    bits, page,
		    (IODMACommand::MappingOptions)
			(IODMACommand::kMapped | IODMACommand::kIterateOnly),
		    0, 1);
	if (fCmd && kIOReturnSuccess != fCmd->setMemoryDescriptor(md, true)) {
	    fCmd->release();
	    fCmd = 0;
	}
    }
    ~QorIQDMAWalk()
    {
	if (fCmd) {
	    fCmd->clearMemoryDescriptor();
	    fCmd->release();
	}
    }
    bool   ready( void ) const { return 0 != fCmd; }
    UInt64 page( UInt64 offset )
    {
	IODMACommand::Segment64 seg;
	UInt32			 n = 1;
	UInt64			 at = offset, bus;

	if (!fCmd)
	    return 0;
	seg.fIOVMAddr = 0;
	seg.fLength = 0;
	if (kIOReturnSuccess != fCmd->gen64IOVMSegments(&at, &seg, &n) || 1 != n
	    || seg.fLength != fPage || !seg.fIOVMAddr
	    || (seg.fIOVMAddr & (fPage - 1)))
	    return 0;
	if (fLimit && seg.fIOVMAddr + fPage > fLimit)
	    return 0;
	bus = seg.fIOVMAddr + fAdd;
	if (bus < seg.fIOVMAddr || (fBits < 64 && ((bus + fPage - 1) >> fBits)))
	    return 0;
	return bus;
    }
private:
    IODMACommand * fCmd;
    UInt8	   fBits;
    UInt64	   fAdd, fLimit;
    UInt32	   fPage;
};

#endif /* QORIQ_DMA_H */
