/*
 *	HDAudio -- an Intel High Definition Audio driver for this port.
 *
 *	WHY. Leopard has no PowerPC HD Audio driver (AppleHDA and IOHDAFamily
 *	are i386-only; PowerPC Macs never had HDA), and the display cards this
 *	driver serves carry an HDA controller with an HDMI/DisplayPort codec. The
 *	controller and ring handling follow FreeBSD's sys/dev/sound/pci/hda
 *	(hdac.c), BSD-licensed; the IOAudio half follows Apple's
 *	SamplePCIAudioDriver template (IOAudioFamily-169.4.5, the family Mac OS X
 *	10.5.8 runs, Examples/Templates). Register offsets and encodings are the
 *	HDA 1.0a spec's, through hda_calc.h, which tools/hda-test.sh checks on
 *	the host (the float-to-int16 clip on every one of the 2^32 inputs).
 *
 *	SHAPE. HDAudioDevice (IOAudioDevice) owns the controller: reset, CORB/RIRB,
 *	the codec walk, one output stream descriptor and the DMA arena.
 *	HDAudioEngine (IOAudioEngine) owns the sample buffer -- which IS the DMA
 *	buffer the stream's BDL points at -- and the timestamps.
 *
 *	ENDPOINTS. Nothing is assumed about which display is attached: every
 *	output converter of every audio function group is put on the one stream
 *	tag and every output-capable pin is enabled. HDA lets several converters
 *	take the same stream (HDA 1.0a 7.3.3.11), so the sound reaches whichever
 *	endpoint the GPU side routes.
 *
 *	TIMESTAMPS. Polled, not interrupt-driven: an IOTimerEventSource reads
 *	LPIB every few ms and, when it has gone backwards, takes the wrap
 *	timestamp backdated by the frames already played. MSI has not been run on
 *	this controller; the poll is exact to the read, not to the period.
 *
 *	MEASURED 2026-11-04. Stage A1 (a
 *	one-shot stream test in start()) proved the DMA path both ways: BDL and
 *	PCM fetched through window 2, the position buffer written back snooped,
 *	no FIFO or descriptor error. Stage A2, this file: afplay through Core
 *	Audio, "102392 frames in ~2132 ms (~48026 frames/s at a nominal 48000)",
 *	STS clean.
 *	CORRECTED: A1's commit said the position ran at ONE SIXTH of the rate and
 *	blamed the GPU's unprogrammed audio DTO. Both wrong -- it was aliasing in
 *	the measurement. A1 sampled LPIB every 200 ms on a 32 KB ring that wraps
 *	every 170.7 ms at 48 kHz, so each sample saw one whole wrap plus 5632
 *	bytes: 192000 B/s x 0.200005 s = 38401 = 32768 + 5633, measured 5628;
 *	and at 2.042 s the prediction is 31701, measured 31664. Every A1 sample
 *	fits 48 kHz exactly. The rate report in performAudioEngineStop() counts
 *	wraps, so it cannot alias. Whether the GPU must still program a DTO for
 *	the *sink* to lock is the GPU half's question; the controller's DMA does
 *	not need it.
 *
 *	DMA: one physically contiguous arena, ordinary cacheable memory, at
 *	physical + 0x10_00000000 through inbound ATMU window 2, which is snooped.
 *	getPhysicalSegment64, never getPhysicalSegment. Everything the controller
 *	reads or writes is little-endian.
 *
 *	NO FLOATING POINT: kexts here are built -msoft-float with no libcc, so
 *	the clip is integer-only (hda_f2s16) and no 64-bit divide appears.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <libkern/OSByteOrder.h>
#include <kern/clock.h>
#include <mach/kmod.h>

#include "hda_calc.h"

/* Controller registers, HDA 1.0a 3.3 */
#define R_GCAP		0x00
#define R_GCTL		0x08
#define R_STATESTS	0x0e
#define R_CORBLBASE	0x40
#define R_CORBUBASE	0x44
#define R_CORBWP	0x48
#define R_CORBRP	0x4a	/* bit 15: CORBRPRST */
#define R_CORBCTL	0x4c	/* bit 1: CORBRUN */
#define R_CORBSIZE	0x4e
#define R_RIRBLBASE	0x50
#define R_RIRBUBASE	0x54
#define R_RIRBWP	0x58	/* bit 15: RIRBWPRST */
#define R_RINTCNT	0x5a
#define R_RIRBCTL	0x5c	/* bit 1: RIRBDMAEN */
#define R_RIRBSIZE	0x5e

/* Stream descriptor registers, offsets from hda_sd_base(), HDA 1.0a 3.3.35 */
#define SD_CTL		0x00	/* SRST 0, RUN 1; stream number 23:20 */
#define SD_STS		0x03	/* BCIS 2, FIFOE 3, DESE 4, FIFORDY 5 */
#define SD_LPIB		0x04
#define SD_CBL		0x08
#define SD_LVI		0x0c
#define SD_FIFOS	0x10
#define SD_FMT		0x12
#define SD_BDPL		0x18
#define SD_BDPU		0x1c

/* The engine's ring: 8192 frames of 16-bit stereo, four BDL entries of 8 KB.
 * 170 ms at 48 kHz, so a 5 ms poll sees every wrap with room to spare. */
#define NFRAMES		8192
#define FRAME_BYTES	4
#define PCM_BYTES	(NFRAMES * FRAME_BYTES)
#define NBDLE		4
#define POLL_MS		5

/* Arena layout, 128-byte aligned throughout (HDA 1.0a 3.3.18, 3.3.24, 3.6.2) */
#define A_CORB		0x0000	/* 256 x 4 */
#define A_RIRB		0x0400	/* 256 x 8 */
#define A_BDL		0x0c00	/* NBDLE x 16 */
#define A_PCM		0x1000
#define ARENA_SIZE	(A_PCM + PCM_BYTES)

#define STREAM_TAG	1
#define MAX_NODES	32

class HDAudioEngine;

class HDAudioDevice : public IOAudioDevice
{
    OSDeclareDefaultStructors(HDAudioDevice)
    friend class HDAudioEngine;

    IOPCIDevice *		fPCI;
    IOMemoryMap *		fMap;
    volatile UInt8 *		fR;
    IOBufferMemoryDescriptor *	fArena;
    volatile UInt8 *		fVA;
    UInt64			fDev;		/* the arena as the device sees it */
    unsigned			fCorbN, fRirbN, fRirbRP, fISS, fSD, fTimeouts;
    unsigned			fCad;
    UInt8			fConv[MAX_NODES], fPin[MAX_NODES], fPinMux[MAX_NODES];
    UInt8			fPinConv[MAX_NODES];	/* the converter fPinMux selects */
    UInt32			fConvWcap[MAX_NODES], fPinWcap[MAX_NODES];
    unsigned			fNConv, fNPin;
    UInt32			fPCM;		/* first converter's rate/size caps */
    UInt32			fVendor;
    UInt32			fRev;
    UInt32			fLastSinkMask;
    bool			fSinkProbeLogged;
    bool			fDigital;	/* first converter is digital (HDMI/DP) */
    HDAudioEngine *		fEngine;

    UInt32 rd32(unsigned o) { return OSReadLittleInt32((const volatile void *)fR, o); }
    UInt16 rd16(unsigned o) { return OSReadLittleInt16((const volatile void *)fR, o); }
    UInt8  rd8(unsigned o)  { return fR[o]; }
    void sync() { __asm__ volatile("sync" ::: "memory"); }
    void wr32(unsigned o, UInt32 v) { OSWriteLittleInt32((volatile void *)fR, o, v); sync(); }
    void wr16(unsigned o, UInt16 v) { OSWriteLittleInt16((volatile void *)fR, o, v); sync(); }
    void wr8(unsigned o, UInt8 v)   { fR[o] = v; sync(); }

    bool waitBit32(unsigned o, UInt32 mask, UInt32 want, unsigned us);
    bool waitBit16(unsigned o, UInt16 mask, UInt16 want, unsigned us);
    bool waitBit8(unsigned o, UInt8 mask, UInt8 want, unsigned us);
    bool allocArena();
    void freeArena();
    bool resetController();
    bool startRings();
    void stopRings();
    bool verb(UInt32 cmd, UInt32 *resp);
    UInt32 param(unsigned nid, unsigned p);
    UInt8 findPinMux(unsigned pinNid, unsigned pinIndex, UInt32 wcap, UInt8 *conv);
    bool walkCodec();
    void setCodecFormat(UInt16 fmt);
    void probeActiveSinks();
    void refreshEndpoints(UInt16 fmt);

    /* the stream, for the engine */
    bool streamSetup(UInt16 fmt);
    void streamRun(bool run);
    UInt32 streamLPIB() { return rd32(fSD + SD_LPIB); }
    UInt8 streamStatus() { return rd8(fSD + SD_STS); }
    UInt32 streamFIFO() { return rd16(fSD + SD_FIFOS); }
    void *pcmBuffer() { return (void *)(fVA + A_PCM); }

public:
    virtual bool initHardware(IOService *provider);
    virtual void free();
};

class HDAudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(HDAudioEngine)

    HDAudioDevice *		fHW;
    IOTimerEventSource *	fTimer;
    UInt32			fLastLPIB, fRate, fWraps;
    uint64_t			fT0;		/* uptime at RUN, for the rate report */
    UInt16			fFmt;
    SInt32			fVol;
    UInt32			fGainQ15;
    bool			fMuted;
    bool			fRunning;

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn volumeChanged(OSObject *target, IOAudioControl *ctl,
				  SInt32 oldVal, SInt32 newVal);
    static IOReturn muteChanged(OSObject *target, IOAudioControl *ctl,
				SInt32 oldVal, SInt32 newVal);
    void updateGain();
    void poll();

public:
    bool initWithDevice(HDAudioDevice *hw);
    virtual bool initHardware(IOService *provider);
    virtual void free();
    virtual void stop(IOService *provider);
    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    virtual UInt32 getCurrentSampleFrame();
    virtual IOReturn performFormatChange(IOAudioStream *audioStream,
		const IOAudioStreamFormat *newFormat, const IOAudioSampleRate *newSampleRate);
    virtual IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf,
		UInt32 firstSampleFrame, UInt32 numSampleFrames,
		const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
};

OSDefineMetaClassAndStructors(HDAudioDevice, IOAudioDevice)
OSDefineMetaClassAndStructors(HDAudioEngine, IOAudioEngine)

/* ======================================================================
 *	the controller
 * ==================================================================== */

/* Every wait is bounded and a timeout is a reported failure, never "proceed
 * as if" (the powerpc-bare-metal skill, section 6). */
bool HDAudioDevice::waitBit32(unsigned o, UInt32 mask, UInt32 want, unsigned us)
{
    for (unsigned i = 0; i < us / 10; i++) {
	if ((rd32(o) & mask) == want)
	    return true;
	IODelay(10);
    }
    return (rd32(o) & mask) == want;
}
bool HDAudioDevice::waitBit16(unsigned o, UInt16 mask, UInt16 want, unsigned us)
{
    for (unsigned i = 0; i < us / 10; i++) {
	if ((rd16(o) & mask) == want)
	    return true;
	IODelay(10);
    }
    return (rd16(o) & mask) == want;
}
bool HDAudioDevice::waitBit8(unsigned o, UInt8 mask, UInt8 want, unsigned us)
{
    for (unsigned i = 0; i < us / 10; i++) {
	if ((rd8(o) & mask) == want)
	    return true;
	IODelay(10);
    }
    return (rd8(o) & mask) == want;
}

bool HDAudioDevice::allocArena()
{
    IOByteCount len = 0;
    addr64_t pa;

    fArena = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		kIOMemoryPhysicallyContiguous | kIODirectionInOut, ARENA_SIZE, PAGE_SIZE);
    if (!fArena || fArena->prepare() != kIOReturnSuccess) {
	IOLog("HDAudio: could not allocate a %u-byte contiguous DMA arena\n", ARENA_SIZE);
	if (fArena) { fArena->release(); fArena = 0; }
	return false;
    }
    fVA = (volatile UInt8 *)fArena->getBytesNoCopy();
    bzero((void *)fVA, ARENA_SIZE);
    pa = fArena->getPhysicalSegment64(0, &len);	/* never getPhysicalSegment */
    fDev = hda_dma_addr((hda_u64)pa, ARENA_SIZE);
    if (!pa || len < ARENA_SIZE || !fDev) {
	IOLog("HDAudio: arena physical 0x%llx (%u of %u contiguous) is outside"
	      " inbound window 2; refusing\n", (unsigned long long)pa,
	      (unsigned)len, ARENA_SIZE);
	freeArena();
	return false;
    }
    return true;
}

void HDAudioDevice::freeArena()
{
    if (fArena) {
	fArena->complete();
	fArena->release();
	fArena = 0;
    }
    fVA = 0;
}

/* HDA 1.0a 4.2.2: CRST to 0, read back; to 1, read back; > 521 us. */
bool HDAudioDevice::resetController()
{
    wr32(R_GCTL, rd32(R_GCTL) & ~1U);
    if (!waitBit32(R_GCTL, 1, 0, 100000)) {
	IOLog("HDAudio: CRST would not clear\n");
	return false;
    }
    IODelay(100);
    wr32(R_GCTL, rd32(R_GCTL) | 1);
    if (!waitBit32(R_GCTL, 1, 1, 100000)) {
	IOLog("HDAudio: CRST would not set\n");
	return false;
    }
    IOSleep(2);
    return true;
}

/* CORB and RIRB, HDA 1.0a 4.4.1/4.4.2, in FreeBSD hdac_corb_init/rirb_init's
 * order: stop the engine, size it, point it at the ring, reset the pointers,
 * start it. Measured working first time in stage A1. */
bool HDAudioDevice::startRings()
{
    unsigned code;
    UInt64 a;

    wr8(R_CORBCTL, 0);
    wr8(R_RIRBCTL, 0);
    if (!waitBit8(R_CORBCTL, 2, 0, 10000) || !waitBit8(R_RIRBCTL, 2, 0, 10000)) {
	IOLog("HDAudio: CORB/RIRB would not stop\n");
	return false;
    }

    code = hda_ring_size(rd8(R_CORBSIZE), &fCorbN);
    wr8(R_CORBSIZE, (rd8(R_CORBSIZE) & ~3) | code);
    a = fDev + A_CORB;
    wr32(R_CORBLBASE, (UInt32)a);
    wr32(R_CORBUBASE, (UInt32)(a >> 32));
    wr16(R_CORBRP, 0x8000);		/* CORBRPRST, HDA 1.0a 3.3.21 */
    (void)waitBit16(R_CORBRP, 0x8000, 0x8000, 10000);
    wr16(R_CORBRP, 0);
    if (!waitBit16(R_CORBRP, 0x8000, 0, 10000)) {
	IOLog("HDAudio: CORBRPRST would not clear\n");
	return false;
    }
    wr16(R_CORBWP, 0);

    code = hda_ring_size(rd8(R_RIRBSIZE), &fRirbN);
    wr8(R_RIRBSIZE, (rd8(R_RIRBSIZE) & ~3) | code);
    a = fDev + A_RIRB;
    wr32(R_RIRBLBASE, (UInt32)a);
    wr32(R_RIRBUBASE, (UInt32)(a >> 32));
    wr16(R_RIRBWP, 0x8000);		/* RIRBWPRST */
    wr16(R_RINTCNT, 1);
    fRirbRP = 0;

    wr8(R_CORBCTL, 2);			/* CORBRUN */
    wr8(R_RIRBCTL, 2);			/* RIRBDMAEN, no interrupt: polled */
    if (!waitBit8(R_CORBCTL, 2, 2, 10000) || !waitBit8(R_RIRBCTL, 2, 2, 10000)) {
	IOLog("HDAudio: CORB/RIRB would not start\n");
	return false;
    }
    return true;
}

void HDAudioDevice::stopRings()
{
    if (!fR)
	return;
    wr8(R_CORBCTL, 0);
    wr8(R_RIRBCTL, 0);
}

/* One verb through the CORB, its response from the RIRB. Polled; an
 * unsolicited response (resp_ex bit 4) is skipped. */
bool HDAudioDevice::verb(UInt32 cmd, UInt32 *resp)
{
    unsigned wp = (rd16(R_CORBWP) + 1) % fCorbN;

    hda_st32(fVA + A_CORB + 4 * wp, cmd);
    sync();
    wr16(R_CORBWP, wp);
    for (unsigned i = 0; i < 10000; i++) {
	unsigned rwp = rd16(R_RIRBWP) & 0xff;

	while (fRirbRP != rwp) {
	    volatile UInt8 *e;

	    fRirbRP = (fRirbRP + 1) % fRirbN;
	    e = fVA + A_RIRB + 8 * fRirbRP;
	    if (hda_ld32(e + 4) & 0x10)
		continue;		/* unsolicited: not ours */
	    *resp = hda_ld32(e);
	    return true;
	}
	IODelay(10);
    }
    fTimeouts++;
    *resp = 0;
    return false;
}

UInt32 HDAudioDevice::param(unsigned nid, unsigned p)
{
    UInt32 r;

    verb(hda_verb12(fCad, nid, HDA_V_GETPARAM, p), &r);
    return r;
}

/* Pick a connection-list index on pinNid that feeds from one of our output
 * converters (preferring the paired converter fConv[pinIndex], else the first
 * output converter in the pin's mux list); *conv gets that converter's NID
 * (0 when the pin has no list or none of ours is in it). */
UInt8 HDAudioDevice::findPinMux(unsigned pinNid, unsigned pinIndex, UInt32 wcap, UInt8 *conv)
{
    UInt32 cl, word = 0;
    unsigned clen, k, j, firstMatch = 0, firstConv = 0;
    bool haveMatch = false;

    *conv = 0;
    if (!hda_wcap_connlist(wcap))
	return 0;
    cl = param(pinNid, HDA_P_CONNLIST);
    clen = hda_connlist_short_len(cl);
    for (k = 0; k < clen && k < 64; k++) {
	unsigned cnid;

	if ((k & 3u) == 0)
	    verb(hda_verb12(fCad, pinNid, HDA_V_GET_CONNLIST, k), &word);
	cnid = hda_connlist_nid(word, k);
	if (pinIndex < fNConv && cnid == fConv[pinIndex]) {
	    *conv = (UInt8)cnid;
	    return (UInt8)k;
	}
	if (!haveMatch) {
	    for (j = 0; j < fNConv; j++) {
		if (cnid == fConv[j]) {
		    firstMatch = k;
		    firstConv = cnid;
		    haveMatch = true;
		    break;
		}
	    }
	}
    }
    *conv = (UInt8)firstConv;
    return haveMatch ? (UInt8)firstMatch : 0;
}

/* Find every audio function group on the first codec present, and in each
 * the output converters and output-capable pins (FreeBSD hdaa_attach's walk,
 * reduced to what an output-only engine needs). Powers the groups to D0. */
bool HDAudioDevice::walkCodec()
{
    UInt16 st = rd16(R_STATESTS);
    UInt32 r, sub, w;
    unsigned fg, nid, i;

    for (fCad = 0; fCad < 15 && !(st & (1 << fCad)); fCad++)
	;
    if (fCad == 15) {
	IOLog("HDAudio: no codec present (STATESTS 0x%04x)\n", st);
	return false;
    }
    fVendor = param(0, HDA_P_VENDOR);
    fRev = param(0, HDA_P_REV);
    sub = param(0, HDA_P_NODECOUNT);
    fNConv = fNPin = 0;
    for (fg = hda_node_start(sub); fg < hda_node_start(sub) + hda_node_count(sub); fg++) {
	if (hda_fg_type(param(fg, HDA_P_FGTYPE)) != HDA_FG_AUDIO)
	    continue;
	verb(hda_verb12(fCad, fg, HDA_V_SET_POWER, 0), &r);
	r = param(fg, HDA_P_NODECOUNT);
	for (nid = hda_node_start(r); nid < hda_node_start(r) + hda_node_count(r); nid++) {
	    w = param(nid, HDA_P_WCAP);
	    if (hda_wcap_type(w) == HDA_WT_OUTPUT && fNConv < MAX_NODES) {
		if (!fNConv) {
		    fPCM = param(nid, HDA_P_PCM);
		    fDigital = hda_wcap_digital(w);
		}
		fConvWcap[fNConv] = w;
		fConv[fNConv++] = nid;
	    } else if (hda_wcap_type(w) == HDA_WT_PIN && fNPin < MAX_NODES &&
		       hda_pincap_out(param(nid, HDA_P_PINCAP))) {
		fPinWcap[fNPin] = w;
		fPin[fNPin++] = nid;
	    }
	}
    }
    for (i = 0; i < fNPin; i++)
	fPinMux[i] = findPinMux(fPin[i], i, fPinWcap[i], &fPinConv[i]);
    IOLog("HDAudio: codec %u vendor 0x%08x rev 0x%08x: %u output converter(s),"
	  " %u output pin(s), PCM caps 0x%08x; %u verb timeout(s)\n",
	  fCad, (unsigned)fVendor, (unsigned)fRev, fNConv, fNPin,
	  (unsigned)fPCM, fTimeouts);
    /*
     * What each pin's sink is. AMD codecs have no ELD buffer; the display
     * driver writes the sink's EDID audio fields into the codec endpoint and
     * the codec hands them back through vendor verbs, which Linux turns into
     * an ELD (get_eld_ati(), sound/hda/codecs/hdmi/atihdmi.c): speaker
     * allocation 0xf70 (bit 9: DisplayPort), sink info index 0x780 / data
     * 0xf81 -- 0 manufacturer, 1 product, 2 name length, 5.. the name.
     */
    for (i = 0; i < fNPin; i++) {
	UInt32 sense = 0, spk = 0, mfg = 0, prod = 0, len = 0, c;
	char name[19];
	unsigned k;

	verb(hda_verb12(fCad, fPin[i], HDA_V_GET_PINSENSE, 0), &sense);
	if (!hda_is_amd_hdmi(fVendor)) {
	    IOLog("HDAudio: pin 0x%02x sense 0x%08lx\n", fPin[i], (unsigned long)sense);
	    continue;
	}
	verb(hda_verb12(fCad, fPin[i], 0xf70, 0), &spk);
	verb(hda_verb12(fCad, fPin[i], 0x780, 0), &c);
	verb(hda_verb12(fCad, fPin[i], 0xf81, 0), &mfg);
	verb(hda_verb12(fCad, fPin[i], 0x780, 1), &c);
	verb(hda_verb12(fCad, fPin[i], 0xf81, 0), &prod);
	verb(hda_verb12(fCad, fPin[i], 0x780, 2), &c);
	verb(hda_verb12(fCad, fPin[i], 0xf81, 0), &len);
	if (len > 18)
	    len = 18;
	for (k = 0; k < len; k++) {
	    c = 0;
	    verb(hda_verb12(fCad, fPin[i], 0x780, 5 + k), &c);
	    verb(hda_verb12(fCad, fPin[i], 0xf81, 0), &c);
	    name[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	name[k] = 0;
	IOLog("HDAudio: pin 0x%02x sense 0x%08lx, sink %s: speakers 0x%02lx, manufacturer 0x%04lx"
	      " product 0x%04lx, name \"%s\"\n", fPin[i], (unsigned long)sense,
	      (spk & 0x200) ? "DisplayPort" : (spk & 0x100) ? "HDMI" : "none",
	      (unsigned long)(spk & 0x7f), (unsigned long)(mfg & 0xffff),
	      (unsigned long)(prod & 0xffff), name);
    }
    return fNConv && fNPin;
}

/* Put every converter on the stream and every pin on output (HDA 1.0a
 * 7.3.3.11 stream/channel, .8 format, .35 HDMI channel count, .9 digital
 * enable, .12 pin control, .10 power), plus AMD HDMI/DP (0x1002aa01) rev3+
 * ramp, single-channel ASP slot mapping and Audio InfoFrame CA (Linux
 * sound/hda/codecs/hdmi/atihdmi.c). Re-run on format change and engine start
 * so a display modeset after attach never leaves a pin unrouted. */
void HDAudioDevice::setCodecFormat(UInt16 fmt)
{
    UInt32 r, route = 0;
    unsigned i, j, nch = (fmt & 0xfu) + 1u;
    int amd = hda_is_amd_hdmi(fVendor);
    int rev3 = hda_is_amd_rev3(fVendor, fRev);
    /* AMD's DCE 11 codec: Linux's model, one converter per stream and its
     * digital output off while the stream and format change (below) */
    int linuxModel = hda_amd_route_active_only(fVendor, fRev);

    /*
     * Which converters carry the stream. Every one, except on AMD's DCE 11
     * codec with a sink present: there only the converters of the pins
     * that report one (fLastSinkMask, from probeActiveSinks), the others on
     * stream 0, as Linux leaves every converter no PCM has open.
     */
    if (linuxModel && fLastSinkMask) {
	for (i = 0; i < fNPin && i < 32; i++) {
	    if (!(fLastSinkMask & (1u << i)))
		continue;
	    for (j = 0; j < fNConv && j < 32; j++)
		if (fConv[j] == fPinConv[i])
		    route |= 1u << j;
	}
    }
    if (!route)
	route = 0xffffffffu;
    for (i = 0; i < fNConv; i++) {
	unsigned n = fConv[i];

	if (i < 32 && !(route & (1u << i))) {
	    verb(hda_verb12(fCad, n, HDA_V_SET_STREAM, 0), &r);	/* stream 0: idle */
	    continue;
	}
	verb(hda_verb12(fCad, n, HDA_V_SET_POWER, 0), &r);
	if (hda_wcap_out_amp(fConvWcap[i]))
	    verb(hda_verb4(fCad, n, HDA_V4_SET_AMP, HDA_AMP_OUT_UNMUTE), &r);
	if (rev3)
	    verb(hda_verb12(fCad, n, HDA_ATI_V_SET_RAMP, HDA_ATI_RAMP_PCM), &r);
	/*
	 * DCE 11 codec: the digital output off before the stream and format
	 * change and on again after (DigEn below), as Linux's spdif_status_reset
	 * does around a digital converter's set-up, so the IEC 60958 status is
	 * taken anew.
	 */
	if (linuxModel)
	    verb(hda_verb12(fCad, n, HDA_V_SET_DIGI1, 0), &r);
	verb(hda_verb12(fCad, n, HDA_V_SET_STREAM, (STREAM_TAG << 4) | 0), &r);
	verb(hda_verb4(fCad, n, HDA_V4_SET_FORMAT, fmt), &r);
	verb(hda_verb12(fCad, n, HDA_V_SET_CONVCHAN, (fmt & 0xf)), &r);
	verb(hda_verb12(fCad, n, HDA_V_SET_DIGI1, HDA_DIGI1_DIGEN), &r);
    }
    for (i = 0; i < fNPin; i++) {
	unsigned pn = fPin[i];

	verb(hda_verb12(fCad, pn, HDA_V_SET_POWER, 0), &r);
	if (hda_wcap_connlist(fPinWcap[i]))
	    verb(hda_verb12(fCad, pn, HDA_V_SET_CONNSEL, fPinMux[i]), &r);
	if (hda_wcap_out_amp(fPinWcap[i]))
	    verb(hda_verb4(fCad, pn, HDA_V4_SET_AMP, HDA_AMP_OUT_UNMUTE), &r);
	verb(hda_verb12(fCad, pn, HDA_V_SET_PINCTL, HDA_PINCTL_OUT), &r);
	if (amd) {
	    unsigned slot;

	    verb(hda_verb12(fCad, pn, HDA_ATI_V_SET_DOWNMIX, 0), &r);
	    if (rev3)
		verb(hda_verb12(fCad, pn, HDA_ATI_V_SET_MCH_MODE, HDA_ATI_MCH_MODE_SINGLE), &r);
	    for (slot = 0; slot < 8; slot++) {
		unsigned sv = hda_ati_slot_verb(slot, rev3);
		if (sv)
		    verb(hda_verb12(fCad, pn, sv, hda_ati_slot_payload(slot, slot < nch)), &r);
	    }
	    verb(hda_verb12(fCad, pn, HDA_ATI_V_SET_CHANALLOC, 0x00), &r);
	}
    }
}

/* Inspect pin sense (and on AMD HDMI/DP codecs, the GPU-populated speaker
 * allocation + sink description registers) and log/publish when the active
 * sink set changes. Before the GPU sets AUDIO_ENABLED on an endpoint, AMD
 * pins return 0x7fffffff, which hda_pinsense_present() filters out. */
void HDAudioDevice::probeActiveSinks()
{
    UInt32 mask = 0, ps = 0, spk = 0, r = 0;
    int amd = hda_is_amd_hdmi(fVendor);
    int rev3 = hda_is_amd_rev3(fVendor, fRev);
    char sinkName[HDA_ATI_SINK_MAX_LEN + 1];
    unsigned i;

    sinkName[0] = '\0';
    for (i = 0; i < fNPin && i < 32; i++) {
	unsigned pn = fPin[i];

	if (!verb(hda_verb12(fCad, pn, HDA_V_GET_PINSENSE, 0), &ps))
	    continue;
	if (!hda_pinsense_present(ps))
	    continue;
	mask |= (1u << i);
    }
    if (fSinkProbeLogged && mask == fLastSinkMask)
	return;
    fSinkProbeLogged = true;
    fLastSinkMask = mask;
    setProperty("HDAudioActivePinMask", (unsigned long long)mask, 32);

    if (!mask) {
	setProperty("HDAudioActiveSink", "none");
	return;
    }
    for (i = 0; i < fNPin && i < 32; i++) {
	unsigned pn = fPin[i];
	const char *conn = "Digital";

	if (!(mask & (1u << i)))
	    continue;
	spk = 0;
	sinkName[0] = '\0';
	if (amd) {
	    verb(hda_verb12(fCad, pn, HDA_ATI_V_GET_SPKALLOC, 0), &spk);
	    if (spk & HDA_ATI_SPK_DP)
		conn = "DisplayPort";
	    else if (spk & HDA_ATI_SPK_HDMI)
		conn = "HDMI";
	    if (rev3) {
		UInt32 slen = 0;
		verb(hda_verb12(fCad, pn, HDA_ATI_V_SET_SINK_IDX, HDA_ATI_SINK_IDX_LEN), &r);
		verb(hda_verb12(fCad, pn, HDA_ATI_V_GET_SINK_DATA, 0), &slen);
		slen &= 0xffu;
		if (slen > HDA_ATI_SINK_MAX_LEN)
		    slen = HDA_ATI_SINK_MAX_LEN;
		unsigned n = 0;
		for (unsigned k = 0; k < slen; k++) {
		    UInt32 ch = 0;
		    verb(hda_verb12(fCad, pn, HDA_ATI_V_SET_SINK_IDX,
				    HDA_ATI_SINK_IDX_FIRST + k), &r);
		    verb(hda_verb12(fCad, pn, HDA_ATI_V_GET_SINK_DATA, 0), &ch);
		    ch &= 0xffu;
		    if (ch >= 0x20 && ch <= 0x7e)
			sinkName[n++] = (char)ch;
		}
		while (n > 0 && sinkName[n - 1] == ' ')
		    n--;
		sinkName[n] = '\0';
	    }
	}
	if (sinkName[0])
	    setProperty("HDAudioActiveSink", sinkName);
	else
	    setProperty("HDAudioActiveSink", conn);
	IOLog("HDAudio: pin %u (index %u, mux %u) active: %s%s%s%s, spk 0x%04x\n",
	      pn, i, (unsigned)fPinMux[i], conn,
	      sinkName[0] ? " \"" : "", sinkName, sinkName[0] ? "\"" : "",
	      (unsigned)(spk & 0xffffu));
    }
}

/* On AMD's DCE 11 codec the sinks first: setCodecFormat routes by them.
 * Every other codec as before (the R7 450's and the W8100's order). */
void HDAudioDevice::refreshEndpoints(UInt16 fmt)
{
    if (hda_amd_route_active_only(fVendor, fRev)) {
	probeActiveSinks();
	setCodecFormat(fmt);
	return;
    }
    setCodecFormat(fmt);
    probeActiveSinks();
}

/* The stream: reset, then CBL, LVI, format, BDL, tag (HDA 1.0a 4.5.3;
 * FreeBSD hdac_stream_reset/setup). Leaves it stopped. */
bool HDAudioDevice::streamSetup(UInt16 fmt)
{
    unsigned i;
    UInt32 ctl;

    streamRun(false);
    for (i = 0; i < NBDLE; i++)
	hda_bdle(fVA + A_BDL + i * HDA_BDLE_SIZE, fDev + A_PCM + i * (PCM_BYTES / NBDLE),
		 PCM_BYTES / NBDLE, 0);
    sync();
    wr8(fSD + SD_CTL, rd8(fSD + SD_CTL) | 1);
    if (!waitBit8(fSD + SD_CTL, 1, 1, 10000)) { IOLog("HDAudio: SRST would not set\n"); return false; }
    wr8(fSD + SD_CTL, rd8(fSD + SD_CTL) & ~1);
    if (!waitBit8(fSD + SD_CTL, 1, 0, 10000)) { IOLog("HDAudio: SRST would not clear\n"); return false; }
    wr8(fSD + SD_STS, 0x1c);			/* clear BCIS/FIFOE/DESE */
    wr32(fSD + SD_CBL, PCM_BYTES);
    wr16(fSD + SD_LVI, NBDLE - 1);
    wr16(fSD + SD_FMT, fmt);
    wr32(fSD + SD_BDPL, (UInt32)(fDev + A_BDL));
    wr32(fSD + SD_BDPU, (UInt32)((fDev + A_BDL) >> 32));
    ctl = rd32(fSD + SD_CTL) & 0xff000000;	/* keep STS (byte 3) out of it */
    wr32(fSD + SD_CTL, ctl | (STREAM_TAG << 20));
    refreshEndpoints(fmt);
    return true;
}

void HDAudioDevice::streamRun(bool run)
{
    if (!fR)
	return;
    if (run) {
	wr8(fSD + SD_CTL, rd8(fSD + SD_CTL) | 2);
    } else {
	wr8(fSD + SD_CTL, rd8(fSD + SD_CTL) & ~2);
	(void)waitBit8(fSD + SD_CTL, 2, 0, 10000);
    }
}

static const char *vendorName(UInt32 v)
{
    switch (v >> 16) {
    case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x10ec: return "Realtek";
    }
    return "HD Audio";
}

bool HDAudioDevice::initHardware(IOService *provider)
{
    UInt16 g;

    if (!IOAudioDevice::initHardware(provider))
	return false;
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI)
	return false;
    /*
     * D0 first. Linux's pci_enable_device() puts a function in D0 before
     * anything reads its BARs; IOKit does not. The R7 450's audio function
     * (1002:aab0) read GCAP 0x0000 at every Leopard boot while Linux's
     * snd-hda-intel read the same controller (2026-11-11): a function left
     * in D3hot answers configuration cycles and reads its memory as zero.
     */
    {
	UInt8 pm = 0;

	if (fPCI->findPCICapability(kIOPCIPowerManagementCapability, &pm) && pm) {
	    UInt16 pmcsr = fPCI->configRead16(pm + 4);

	    if (pmcsr & 3) {
		fPCI->configWrite16(pm + 4, pmcsr & ~3);
		IOSleep(10);		/* D3hot -> D0: 10 ms (PCI PM 1.2, 5.6.1) */
	    }
	    IOLog("HDAudio: power state D%u -> D%u (PMCSR 0x%04x)\n", pmcsr & 3,
		  fPCI->configRead16(pm + 4) & 3, fPCI->configRead16(pm + 4));
	}
    }
    fPCI->setMemoryEnable(true);		/* BEFORE the first register read */
    fPCI->setBusMasterEnable(true);
    fMap = fPCI->mapDeviceMemoryWithIndex(0);
    if (!fMap)
	return false;
    fR = (volatile UInt8 *)fMap->getVirtualAddress();
    /*
     * OUT OF RESET BEFORE GCAP. AMD's GPU audio controllers read every
     * register as zero -- GCAP, VMIN/VMAJ, the payload capabilities, the
     * wall clock -- while GCTL.CRST is 0, and they come up with it at 0.
     * Measured on the R7 450 (1002:aab0, 2026-11-11): all zeros,
     * no error anywhere; set CRST and GCAP reads 0x6003 (six output
     * streams, 64OK), VMAJ 1, STATESTS 0x0001. Linux reads the same zero:
     * azx_first_init() reads GCAP before azx_init_chip() resets the link,
     * and for AZX_DRIVER_ATIHDMI(_NS) it falls back to ATIHDMI_NUM_PLAYBACK
     * (8) streams when GCAP "didn't give any info" (sound/hda/controllers/
     * intel.c). Leaving reset first is the HDA 1.0a 4.2.2 sequence anyway,
     * so it is done for every controller.
     */
    g = rd16(R_GCAP);
    if (!resetController())
	return false;
    IOLog("HDAudio: GCAP 0x%04x in reset, 0x%04x out of it (GCTL 0x%08lx)\n", g,
	  rd16(R_GCAP), (unsigned long)rd32(R_GCTL));
    g = rd16(R_GCAP);
    fISS = (g >> 8) & 0xf;
    if (!((g >> 12) & 0xf)) {
	IOLog("HDAudio: GCAP 0x%04x: no output streams (command 0x%04x, BAR0 0x%08x_%08x, mapped at phys 0x%08lx)\n",
	      g, fPCI->configRead16(kIOPCIConfigCommand), fPCI->configRead32(kIOPCIConfigBaseAddress0 + 4),
	      fPCI->configRead32(kIOPCIConfigBaseAddress0), (unsigned long)fMap->getPhysicalAddress());
	return false;
    }
    fSD = hda_sd_base(fISS, 0);
    if (!allocArena() || !startRings() || !walkCodec())
	return false;

    setDeviceName(fDigital ? "HDMI / DisplayPort Audio" : "HD Audio");
    setDeviceShortName("HDAudio");
    setManufacturerName(vendorName(fVendor));
    setDeviceTransportType(kIOAudioDeviceTransportTypePCI);

    fEngine = new HDAudioEngine;
    if (!fEngine || !fEngine->initWithDevice(this)) {
	if (fEngine) { fEngine->release(); fEngine = 0; }
	return false;
    }
    if (activateAudioEngine(fEngine) != kIOReturnSuccess) {
	fEngine->release();
	fEngine = 0;
	return false;
    }
    fEngine->release();		/* the device's engine list holds it now */
    IOLog("HDAudio: engine active: stream descriptor 0x%x, %u frames, %u BDL entries,"
	  " DMA at 0x%llx\n", fSD, NFRAMES, NBDLE, (unsigned long long)fDev);
    return true;
}

void HDAudioDevice::free()
{
    if (fR)
	streamRun(false);
    stopRings();
    freeArena();
    if (fMap) { fMap->release(); fMap = 0; }
    fR = 0;
    IOAudioDevice::free();
}

/* ======================================================================
 *	the engine
 * ==================================================================== */

bool HDAudioEngine::initWithDevice(HDAudioDevice *hw)
{
    if (!IOAudioEngine::init(NULL))
	return false;
    fHW = hw;
    fVol = 100;
    fMuted = false;
    fGainQ15 = HDA_GAIN_UNITY_Q15;
    return true;
}

void HDAudioEngine::updateGain()
{
    fGainQ15 = hda_vol_to_q15(fVol, 100, fMuted ? 1 : 0);
}

IOReturn HDAudioEngine::volumeChanged(OSObject *target, IOAudioControl *ctl,
				      SInt32 oldVal, SInt32 newVal)
{
    HDAudioEngine *e = OSDynamicCast(HDAudioEngine, target);
    (void)ctl;
    (void)oldVal;
    if (e) {
	e->fVol = newVal;
	e->updateGain();
    }
    return kIOReturnSuccess;
}

IOReturn HDAudioEngine::muteChanged(OSObject *target, IOAudioControl *ctl,
				    SInt32 oldVal, SInt32 newVal)
{
    HDAudioEngine *e = OSDynamicCast(HDAudioEngine, target);
    (void)ctl;
    (void)oldVal;
    if (e) {
	e->fMuted = (newVal != 0);
	e->updateGain();
    }
    return kIOReturnSuccess;
}

bool HDAudioEngine::initHardware(IOService *provider)
{
    IOAudioSampleRate rate;
    IOAudioStream *stream;
    IOAudioLevelControl *volCtl;
    IOAudioToggleControl *muteCtl;
    IOWorkLoop *wl;
    static const unsigned rates[3] = { 32000, 44100, 48000 };

    if (!IOAudioEngine::initHardware(provider))
	return false;

    fRate = 48000;
    fFmt = hda_format(fRate, 16, 2);
    rate.whole = fRate;
    rate.fraction = 0;
    setDescription("HDMI / DisplayPort");
    setSampleRate(&rate);
    setNumSampleFramesPerBuffer(NFRAMES);
    /* How far ahead of the DMA Core Audio must stay: the stream FIFO, which
     * the controller has already fetched (SDnFIFOS, HDA 1.0a 3.3.40), plus
     * one poll interval. */
    setSampleOffset(fHW->streamFIFO() / FRAME_BYTES + 48000 * POLL_MS / 1000);
    setSampleLatency(32);

    wl = getWorkLoop();
    if (!wl)
	return false;
    fTimer = IOTimerEventSource::timerEventSource(this, timerFired);
    if (!fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess)
	return false;

    if (!fHW->streamSetup(fFmt))
	return false;

    stream = new IOAudioStream;
    if (!stream)
	return false;
    if (!stream->initWithAudioEngine(this, kIOAudioStreamDirectionOutput, 1, "Digital Out")) {
	stream->release();
	return false;
    }
    {
	IOAudioStreamFormat format = {
	    2,						/* channels */
	    kIOAudioStreamSampleFormatLinearPCM,
	    kIOAudioStreamNumericRepresentationSignedInt,
	    16, 16,					/* bit depth, bit width */
	    kIOAudioStreamAlignmentHighByte,		/* unused: depth == width */
	    kIOAudioStreamByteOrderLittleEndian,	/* the controller's order */
	    true,					/* mixable */
	    0
	};
	stream->setSampleBuffer(fHW->pcmBuffer(), PCM_BYTES);
	for (unsigned i = 0; i < 3; i++) {
	    if (!hda_pcm_has_rate(fHW->fPCM, rates[i]))
		continue;
	    rate.whole = rates[i];
	    stream->addAvailableFormat(&format, &rate, &rate);
	}
	rate.whole = fRate;
	stream->setFormat(&format);
    }
    addAudioStream(stream);
    stream->release();

    volCtl = IOAudioLevelControl::createVolumeControl(100, 0, 100,
		(-40) << 16, 0, kIOAudioControlChannelIDAll,
		kIOAudioControlChannelNameAll, 1, kIOAudioControlUsageOutput);
    if (volCtl) {
	volCtl->setValueChangeHandler(volumeChanged, this);
	addDefaultAudioControl(volCtl);
	volCtl->release();
    }
    muteCtl = IOAudioToggleControl::createMuteControl(false,
		kIOAudioControlChannelIDAll, kIOAudioControlChannelNameAll,
		2, kIOAudioControlUsageOutput);
    if (muteCtl) {
	muteCtl->setValueChangeHandler(muteChanged, this);
	addDefaultAudioControl(muteCtl);
	muteCtl->release();
    }
    return true;
}

void HDAudioEngine::stop(IOService *provider)
{
    if (fTimer) {
	fTimer->cancelTimeout();
	IOWorkLoop *wl = getWorkLoop();
	if (wl)
	    wl->removeEventSource(fTimer);
	fTimer->release();
	fTimer = 0;
    }
    fHW->streamRun(false);
    IOAudioEngine::stop(provider);
}

void HDAudioEngine::free()
{
    if (fTimer) {
	fTimer->release();
	fTimer = 0;
    }
    IOAudioEngine::free();
}

/*
 * The clock calls below are written (clock_get_uptime)(...) on purpose. The
 * functions take uint64_t in both kernels, but xnu-1504's kern/clock.h
 * (lines 291-319) wraps them, for a 32-bit kext, in function-like macros that
 * force an AbsoluteTime struct, and a parenthesised name is not a macro
 * invocation. Defining ABSOLUTETIME_SCALAR_TYPE instead would retype
 * AbsoluteTime itself and change how takeTimeStamp(AbsoluteTime *) mangles
 * against IOAudioFamily.
 */
IOReturn HDAudioEngine::performAudioEngineStart()
{
    bzero(fHW->pcmBuffer(), PCM_BYTES);
    if (!fHW->streamSetup(fFmt))
	return kIOReturnError;
    fLastLPIB = 0;
    fWraps = 0;
    fRunning = true;
    takeTimeStamp(false);
    (clock_get_uptime)(&fT0);
    fHW->streamRun(true);
    fTimer->setTimeoutMS(POLL_MS);
    return kIOReturnSuccess;
}

IOReturn HDAudioEngine::performAudioEngineStop()
{
    uint64_t t1, ns;
    UInt32 frames, ms;

    fRunning = false;
    fTimer->cancelTimeout();
    /* The measured rate: frames the DMA consumed over the uptime it took.
     * Counts wraps, so unlike A1's 200 ms LPIB samples it cannot alias.
     * ms fits 32 bits for 49 days. */
    frames = fWraps * NFRAMES + (fHW->streamLPIB() % PCM_BYTES) / FRAME_BYTES;
    (clock_get_uptime)(&t1);
    fHW->streamRun(false);
    (absolutetime_to_nanoseconds)(t1 - fT0, &ns);
    /* ms = (ns >> 20) * 1.048576, without a 64-bit divide (no __udivdi3 in
     * a kext). The old (UInt32)(ns >> 20) * 1048576 / 1000000 overflowed 32
     * bits past ~4.1 s: a 10 s tone logged "~1607 ms". 0.048576 * 2^20 =
     * 50935. */
    {
	UInt32 x = (UInt32)(ns >> 20);

	ms = x + (UInt32)(((uint64_t)x * 50935u) >> 20);
    }
    IOLog("HDAudio: engine stopped: %u frames in ~%u ms (~%u frames/s at a"
	  " nominal %u), %u wraps, STS 0x%02x\n", (unsigned)frames, (unsigned)ms,
	  ms ? (unsigned)(frames / ms * 1000 + frames % ms * 1000 / ms) : 0,
	  (unsigned)fRate, (unsigned)fWraps, fHW->streamStatus());
    return kIOReturnSuccess;
}

UInt32 HDAudioEngine::getCurrentSampleFrame()
{
    return (fHW->streamLPIB() % PCM_BYTES) / FRAME_BYTES;
}

void HDAudioEngine::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    HDAudioEngine *e = OSDynamicCast(HDAudioEngine, owner);

    if (e)
	e->poll();
}

/* The wrap: LPIB went backwards. The buffer started over lpib bytes ago, so
 * backdate the timestamp by that many frames' worth of time. */
void HDAudioEngine::poll()
{
    UInt32 lpib, us;
    uint64_t now, back;
    AbsoluteTime ts;

    if (!fRunning)
	return;
    lpib = fHW->streamLPIB() % PCM_BYTES;
    if (lpib < fLastLPIB) {
	(clock_get_uptime)(&now);
	us = hda_frames_to_us(lpib / FRAME_BYTES, fRate);
	(nanoseconds_to_absolutetime)((UInt64)us * 1000, &back);
	now -= back;
	bcopy(&now, &ts, sizeof(ts));	/* AbsoluteTime is the same 64 bits, as a struct */
	takeTimeStamp(true, &ts);
	if (++fWraps == 1)
	    IOLog("HDAudio: first buffer wrap; STS 0x%02x\n", fHW->streamStatus());
    }
    fLastLPIB = lpib;
    fTimer->setTimeoutMS(POLL_MS);
}

IOReturn HDAudioEngine::performFormatChange(IOAudioStream *audioStream,
		const IOAudioStreamFormat *newFormat, const IOAudioSampleRate *newSampleRate)
{
    UInt16 fmt;
    bool was = fRunning;

    if (!newSampleRate)
	return kIOReturnSuccess;
    fmt = hda_format(newSampleRate->whole, 16, 2);
    if (!fmt || !hda_pcm_has_rate(fHW->fPCM, newSampleRate->whole))
	return kIOReturnUnsupported;
    if (was)
	performAudioEngineStop();
    fRate = newSampleRate->whole;
    fFmt = fmt;
    if (!fHW->streamSetup(fFmt))
	return kIOReturnError;
    IOLog("HDAudio: format 0x%04x (%u Hz)\n", fFmt, (unsigned)fRate);
    if (was)
	performAudioEngineStart();
    return kIOReturnSuccess;
}

/* Float mix buffer -> 16-bit LE DMA buffer, both indexed by sample (frame *
 * channels). Integer-only; see hda_f2s16 and hda_clip_s16le_gain. */
IOReturn HDAudioEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf,
		UInt32 firstSampleFrame, UInt32 numSampleFrames,
		const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
    UInt32 ch = streamFormat->fNumChannels;
    (void)audioStream;

    hda_clip_s16le_gain((const hda_u32 *)mixBuf, sampleBuf, firstSampleFrame * ch,
			numSampleFrames * ch, fGainQ15);
    return kIOReturnSuccess;
}

__asm__(".set .constructors_used, 0");
__asm__(".set .destructors_used, 0");

extern "C" {
extern kern_return_t OSRuntimeInitializeCPP(kmod_info_t *ki, void *data);
extern kern_return_t OSRuntimeFinalizeCPP(kmod_info_t *ki, void *data);

static kern_return_t hdaStart(kmod_info_t *ki, void *data) { return OSRuntimeInitializeCPP(ki, data); }
static kern_return_t hdaStop(kmod_info_t *ki, void *data) { return OSRuntimeFinalizeCPP(ki, data); }

KMOD_EXPLICIT_DECL(io.github.jbettcher-wg.HDAudio, "1.0.0", hdaStart, hdaStop)
}
