/*
 *	hda_calc.h -- the register-free half of the HD Audio driver.
 *
 *	NO KERNEL INCLUDES: tools/hda-test.c compiles this on the build host and
 *	checks it against the Intel High Definition Audio Specification 1.0a.
 *	QEMU's ppce500 machine has no HD Audio controller at all, and the hardware
 *	can only say "the stream ran" or "it did not" -- a wrong bit in a verb,
 *	a stream format or a buffer descriptor does not trap, it plays nothing or
 *	plays the wrong thing. So every encoding here is pinned by the host test
 *	against values worked from the spec's own tables.
 *
 *	BYTE ORDER: the controller is little-endian in everything -- registers,
 *	CORB and RIRB entries, buffer descriptor list entries, the DMA position
 *	buffer and the PCM samples themselves (HDA 1.0a 3.3, 3.6, 4.4). This
 *	header's store/load helpers are written byte by byte so the same code is
 *	right on big-endian PowerPC and on the little-endian host that tests it.
 */
#ifndef _HDA_CALC_H_
#define _HDA_CALC_H_

typedef unsigned char	hda_u8;
typedef unsigned short	hda_u16;
typedef unsigned int	hda_u32;
typedef unsigned long long hda_u64;

/* ---- little-endian memory, byte by byte (rings, BDL, posbuf, PCM) ------ */
static inline void hda_st32(volatile void *p, hda_u32 v)
{
	volatile hda_u8 *b = (volatile hda_u8 *)p;
	b[0] = (hda_u8)v; b[1] = (hda_u8)(v >> 8); b[2] = (hda_u8)(v >> 16); b[3] = (hda_u8)(v >> 24);
}
static inline hda_u32 hda_ld32(const volatile void *p)
{
	const volatile hda_u8 *b = (const volatile hda_u8 *)p;
	return (hda_u32)b[0] | ((hda_u32)b[1] << 8) | ((hda_u32)b[2] << 16) | ((hda_u32)b[3] << 24);
}
static inline void hda_st16(volatile void *p, hda_u16 v)
{
	volatile hda_u8 *b = (volatile hda_u8 *)p;
	b[0] = (hda_u8)v; b[1] = (hda_u8)(v >> 8);
}

/* ---- verbs, HDA 1.0a 7.3 -------------------------------------------------
 *	A command word is CAd 31:28, NID 27:20, then either a 12-bit verb with an
 *	8-bit payload or a 4-bit verb with a 16-bit payload (the "set converter
 *	format" and amplifier verbs). */
static inline hda_u32 hda_verb12(unsigned cad, unsigned nid, unsigned verb, unsigned payload)
{
	return ((hda_u32)(cad & 0xf) << 28) | ((hda_u32)(nid & 0xff) << 20) |
	       ((hda_u32)(verb & 0xfff) << 8) | (payload & 0xff);
}
static inline hda_u32 hda_verb4(unsigned cad, unsigned nid, unsigned verb, unsigned payload)
{
	return ((hda_u32)(cad & 0xf) << 28) | ((hda_u32)(nid & 0xff) << 20) |
	       ((hda_u32)(verb & 0xf) << 16) | (payload & 0xffff);
}

#define HDA_V_GETPARAM		0xf00
#define HDA_V_GET_CONNSEL	0xf01
#define HDA_V_GET_CONNLIST	0xf02
#define HDA_V_GET_PINSENSE	0xf09
#define HDA_V_SET_CONNSEL	0x701	/* payload: connection list index */
#define HDA_V_SET_POWER		0x705	/* payload: D-state 3:0 (0 = D0) */
#define HDA_V_SET_STREAM	0x706	/* payload: stream 7:4, channel 3:0 */
#define HDA_V_SET_PINCTL	0x707	/* payload: OUT enable bit 6 */
#define HDA_V_SET_DIGI1		0x70d	/* payload: DigEn bit 0 */
#define HDA_V_SET_CONVCHAN	0x72d	/* converter channel count - 1 (HDMI) */
#define HDA_V4_SET_FORMAT	0x2	/* 4-bit verb, 16-bit stream format payload */
#define HDA_V4_SET_AMP		0x3	/* 4-bit verb, 16-bit amp gain/mute payload */

#define HDA_PINCTL_OUT		0x40
#define HDA_DIGI1_DIGEN		0x01
#define HDA_AMP_OUT_UNMUTE	0xb000	/* Set Out (15) | L (13) | R (12), mute=0, gain=0 */

/* Pin sense response bits, HDA 1.0a 7.3.3.15. Note: an AMD HDMI/DP pin whose
 * display endpoint has AUDIO_ENABLED=0 returns 0x7fffffff (bit 31 clear, all
 * lower bits set), so bit 30 (ELDV) alone does NOT mean a sink is present. */
#define HDA_PINSENSE_PRESENCE	0x80000000u
#define HDA_PINSENSE_ELDV	0x40000000u
#define HDA_PINSENSE_AMD_DIS	0x7fffffffu

/* AMD/ATI HDMI/DP codec (0x1002aa01) vendor verbs (Linux sound/hda/codecs/hdmi/atihdmi.c).
 * ATI/AMD codecs do not implement standard HDA DIP InfoFrame or ELD verbs;
 * instead, function .0 writes AZ_F0_CODEC_PIN_CONTROL_* and function .1 reads
 * sink info via 0xf70/0x780/0xf81 and configures ASP slot mapping + Audio
 * InfoFrame CA via 0x771/0x772/0x777..0x789. */
#define HDA_VENDOR_AMD_HDMI	0x1002aa01u
#define HDA_ATI_V_SET_RAMP	0x770	/* converter: PCM ramp rate (180 default on rev3+) */
#define HDA_ATI_V_GET_SPKALLOC	0xf70	/* pin: 6:0 speaker alloc, 8 HDMI, 9 DisplayPort */
#define HDA_ATI_V_SET_CHANALLOC	0x771	/* pin: Audio InfoFrame CA (0x00 = stereo FL/FR) */
#define HDA_ATI_V_SET_DOWNMIX	0x772	/* pin: Audio InfoFrame downmix info */
#define HDA_ATI_V_SET_MCH_01	0x777	/* pin: slot 0 (or pair 0/1 on pre-rev3) */
#define HDA_ATI_V_SET_SINK_IDX	0x780	/* pin: sink info index (rev3+) */
#define HDA_ATI_V_GET_SINK_DATA	0xf81	/* pin: sink info byte/word (rev3+) */
#define HDA_ATI_V_SET_MCH_1	0x785	/* pin: odd slot 1 (rev3+ single mode) */
#define HDA_ATI_V_SET_MCH_MODE	0x789	/* pin: 0 = paired, 1 = single-channel remap (rev3+) */

#define HDA_ATI_RAMP_PCM	180u
#define HDA_ATI_MCH_MODE_SINGLE	1u
#define HDA_ATI_SPK_MASK	0x007fu
#define HDA_ATI_SPK_HDMI	0x0100u
#define HDA_ATI_SPK_DP		0x0200u
#define HDA_ATI_SINK_IDX_LEN	2u
#define HDA_ATI_SINK_IDX_FIRST	5u
#define HDA_ATI_SINK_MAX_LEN	18u

/* ---- parameters (GET_PARAMETER payloads) and their fields, HDA 1.0a 7.3.4 */
#define HDA_P_VENDOR		0x00
#define HDA_P_REV		0x02	/* revision ID; 15:8 MajRev:MinRev */
#define HDA_P_NODECOUNT		0x04	/* starting node 23:16, total 7:0 */
#define HDA_P_FGTYPE		0x05	/* node type 7:0; 0x01 = audio function group */
#define HDA_P_WCAP		0x09	/* audio widget capabilities */
#define HDA_P_PCM		0x0a	/* supported PCM sizes and rates */
#define HDA_P_PINCAP		0x0c	/* pin capabilities */
#define HDA_P_CONNLIST		0x0e	/* bit 7 long form, 6:0 connection list length */

#define HDA_FG_AUDIO		0x01
#define HDA_WT_OUTPUT		0x0	/* widget type, wcap 23:20 (7.3.4.6) */
#define HDA_WT_PIN		0x4

static inline unsigned hda_node_start(hda_u32 r) { return (r >> 16) & 0xff; }
static inline unsigned hda_node_count(hda_u32 r) { return r & 0xff; }
static inline unsigned hda_fg_type(hda_u32 r)    { return r & 0xff; }
static inline unsigned hda_wcap_type(hda_u32 w)  { return (w >> 20) & 0xf; }
static inline int hda_wcap_out_amp(hda_u32 w)    { return (w >> 2) & 1; }
static inline int hda_wcap_connlist(hda_u32 w)   { return (w >> 8) & 1; }
static inline int hda_wcap_digital(hda_u32 w)    { return (w >> 9) & 1; }
static inline int hda_pincap_out(hda_u32 p)      { return (p >> 4) & 1; }	/* 7.3.4.9 */
static inline unsigned hda_connlist_short_len(hda_u32 cl)
{
	return (cl & 0x80) ? 0u : (unsigned)(cl & 0x7f);
}
static inline unsigned hda_connlist_nid(hda_u32 word, unsigned idx)
{
	return (unsigned)((word >> (8 * (idx & 3))) & 0xff);
}
static inline int hda_pinsense_present(hda_u32 ps)
{
	return ps != HDA_PINSENSE_AMD_DIS && (ps & HDA_PINSENSE_PRESENCE) != 0;
}
static inline int hda_is_amd_hdmi(hda_u32 vendor)
{
	return vendor == HDA_VENDOR_AMD_HDMI;
}
static inline int hda_is_amd_rev3(hda_u32 vendor, hda_u32 rev)
{
	return vendor == HDA_VENDOR_AMD_HDMI && (rev & 0xff00u) >= 0x0300u;
}
/* AMD's DCE 11 codec (rev 7, Polaris): only the converters of pins with a
 * sink go on the stream, the rest stay on stream 0 -- Linux's state (one
 * converter per open PCM, hdmi_pcm_open). Older AMD codecs (rev 3 on the
 * R7 450, rev 5 on the W8100) keep every converter on the stream, as
 * measured working on the R7 450. */
static inline int hda_amd_route_active_only(hda_u32 vendor, hda_u32 rev)
{
	return vendor == HDA_VENDOR_AMD_HDMI && (rev & 0xff00u) >= 0x0700u;
}
/* AMD HDMI/DP ASP slot-to-stream-channel verb & payload (atihdmi.c:311-345).
 * Returns 0 if the slot has no independent verb (odd slots in pre-rev3 paired mode). */
static inline unsigned hda_ati_slot_verb(unsigned slot, int rev3)
{
	if (slot > 7)
		return 0;
	if (!rev3) {
		unsigned s = (slot == 2) ? 3u : (slot == 3) ? 2u : slot;
		if (s & 1u)
			return 0;
		return HDA_ATI_V_SET_MCH_01 + (s / 2u);
	}
	return HDA_ATI_V_SET_MCH_01 + (slot / 2u) + (slot % 2u) * 0x00eu;
}
static inline unsigned hda_ati_slot_payload(unsigned stream_chan, int enable)
{
	return enable ? (((stream_chan & 0xfu) << 4) | 1u) : 0u;
}

/* ---- the stream format word, HDA 1.0a 3.7.1 ----------------------------
 *	Bit 15 type (0 = PCM), 14 base (0 = 48 kHz, 1 = 44.1 kHz), 13:11 rate
 *	multiple - 1, 10:8 rate divisor - 1, 6:4 bits per sample (0 = 8, 1 = 16,
 *	2 = 20, 3 = 24, 4 = 32), 3:0 channels - 1. Returns 0 for a combination
 *	the encoding cannot express. */
static inline hda_u16 hda_format(unsigned rate, unsigned bits, unsigned channels)
{
	unsigned base, mult, div, b;

	switch (rate) {
	case 8000:   base = 0; mult = 1; div = 6; break;
	case 11025:  base = 1; mult = 1; div = 4; break;
	case 16000:  base = 0; mult = 1; div = 3; break;
	case 22050:  base = 1; mult = 1; div = 2; break;
	case 32000:  base = 0; mult = 2; div = 3; break;
	case 44100:  base = 1; mult = 1; div = 1; break;
	case 48000:  base = 0; mult = 1; div = 1; break;
	case 88200:  base = 1; mult = 2; div = 1; break;
	case 96000:  base = 0; mult = 2; div = 1; break;
	case 176400: base = 1; mult = 4; div = 1; break;
	case 192000: base = 0; mult = 4; div = 1; break;
	default:     return 0;
	}
	switch (bits) {
	case 8:  b = 0; break;
	case 16: b = 1; break;
	case 20: b = 2; break;
	case 24: b = 3; break;
	case 32: b = 4; break;
	default: return 0;
	}
	if (channels < 1 || channels > 16)
		return 0;
	return (hda_u16)((base << 14) | ((mult - 1) << 11) | ((div - 1) << 8) |
			 (b << 4) | (channels - 1));
}

/* The converter's supported-PCM parameter (0x0a): rate bits 11:0 in the order
 * 8k 11.025k 16k 22.05k 32k 44.1k 48k 88.2k 96k 176.4k 192k 384k, and size
 * bits 20:16 for 8/16/20/24/32 bits (HDA 1.0a 7.3.4.7). */
static inline int hda_pcm_has_rate(hda_u32 pcm, unsigned rate)
{
	static const unsigned r[12] = { 8000, 11025, 16000, 22050, 32000, 44100,
					48000, 88200, 96000, 176400, 192000, 384000 };
	unsigned i;

	for (i = 0; i < 12; i++)
		if (r[i] == rate)
			return (pcm >> i) & 1;
	return 0;
}
static inline int hda_pcm_has_bits(hda_u32 pcm, unsigned bits)
{
	switch (bits) {
	case 8:  return (pcm >> 16) & 1;
	case 16: return (pcm >> 17) & 1;
	case 20: return (pcm >> 18) & 1;
	case 24: return (pcm >> 19) & 1;
	case 32: return (pcm >> 20) & 1;
	}
	return 0;
}

/* ---- the buffer descriptor list entry, HDA 1.0a 3.6.3 -------------------
 *	16 bytes: address 63:0 (128-byte aligned for the list itself, the
 *	buffers need only be 128-byte aligned by convention), length 31:0 in
 *	bytes, then IOC in bit 0 of the last word. All little-endian. */
#define HDA_BDLE_SIZE	16
static inline void hda_bdle(volatile void *e, hda_u64 addr, hda_u32 len, int ioc)
{
	volatile hda_u8 *b = (volatile hda_u8 *)e;

	hda_st32(b + 0, (hda_u32)addr);
	hda_st32(b + 4, (hda_u32)(addr >> 32));
	hda_st32(b + 8, len);
	hda_st32(b + 12, ioc ? 1 : 0);
}

/* ---- stream descriptor offsets, HDA 1.0a 3.3.35 -------------------------
 *	Input streams first, then output, then bidirectional, 0x20 bytes each,
 *	from 0x80. */
static inline unsigned hda_sd_base(unsigned iss, unsigned out_index)
{
	return 0x80 + 0x20 * (iss + out_index);
}

/* ---- CORB/RIRB: entry counts from the size registers, HDA 1.0a 3.3.24 ---
 *	CORBSIZE/RIRBSIZE capability bits 7:4 say which of 2, 16, 256 entries the
 *	controller supports; this picks the largest. Returns the code for bits
 *	1:0 and stores the entry count. */
static inline unsigned hda_ring_size(hda_u8 sizereg, unsigned *entries)
{
	if (sizereg & 0x40) { *entries = 256; return 2; }
	if (sizereg & 0x20) { *entries = 16;  return 1; }
	*entries = 2;
	return 0;
}

/* The DMA address the device sees on the T4240: inbound ATMU window 2 maps
 * PCI 0x10_00000000 + 2^36 onto CPU physical 0, snooped (the platform's
 * inbound PCIe window). Returns 0 if out of reach. */
#define HDA_WIN2_BASE	0x1000000000ULL
#define HDA_WIN2_SIZE	(1ULL << 36)
static inline hda_u64 hda_dma_addr(hda_u64 phys, hda_u64 len)
{
	if (phys + len > HDA_WIN2_SIZE || phys + len < phys)
		return 0;
	return phys + HDA_WIN2_BASE;
}

/* ---- time: frames to microseconds, with no 64-bit divide ---------------
 *	Kexts here link without libcc, so a 64-bit '/' is an undefined
 *	__udivdi3 at load (measured 2026-11-04). Exact to 1 us for frames below
 *	4.29 million: q is whole milliseconds, r the remainder in frame-ms. */
static inline hda_u32 hda_frames_to_us(hda_u32 frames, hda_u32 rate)
{
	hda_u32 q = frames * 1000 / rate, r = frames * 1000 % rate;

	return q * 1000 + r * 1000 / rate;
}

/* ---- the output clip, float to 16-bit, in integers only -----------------
 *	The engine's mix buffer holds IEEE single floats in [-1, 1) written by
 *	Core Audio; the hardware wants signed 16-bit little-endian. Kexts here
 *	are built -msoft-float with no libcc to supply __mulsf3/__fixsfsi, and
 *	kernel-mode FP has not been exercised on this port, so the conversion
 *	is done on the float's bits: f * 32768 = mant24 * 2^(exp - 135), which
 *	is a right shift of the 24-bit significand by (135 - exp), truncating
 *	toward zero like a C (short) cast. |f| >= 1, infinities and NaN
 *	saturate by sign; denormals and anything below 2^-15 give 0.
 *	tools/hda-test.c checks this against the float expression it replaces. */
static inline short hda_f2s16(hda_u32 bits)
{
	unsigned e = (bits >> 23) & 0xff, sh;
	hda_u32 m;
	int neg = (int)(bits >> 31);

	if (e >= 127)
		return neg ? (short)-32768 : (short)32767;
	sh = 135 - e;			/* >= 9 */
	if (sh > 23)
		return 0;
	m = ((bits & 0x7fffff) | 0x800000) >> sh;
	return neg ? (short)-(int)m : (short)m;
}

/* Clip n samples from src[first..] into a 16-bit LE buffer at the same index. */
static inline void hda_clip_s16le(const hda_u32 *src, volatile void *dst,
				  hda_u32 first, hda_u32 n)
{
	volatile hda_u8 *d = (volatile hda_u8 *)dst + 2 * first;
	hda_u32 i;

	src += first;
	for (i = 0; i < n; i++) {
		hda_u16 v = (hda_u16)hda_f2s16(src[i]);

		d[2 * i] = (hda_u8)v;
		d[2 * i + 1] = (hda_u8)(v >> 8);
	}
}

/* ---- integer-only software volume and mute (Q15 gain, 32768 = unity) ----
 *	HDMI/DisplayPort digital widgets carry no hardware output amplifier, so
 *	exposing an IOAudioLevelControl/IOAudioToggleControl backed by Q15 gain
 *	gives Core Audio and the menu-bar slider working volume + mute with zero
 *	floating point. At vol == max_vol && !muted, gain_q15 == 32768 and
 *	hda_clip_s16le_gain is bit-exact pass-through of hda_clip_s16le. */
#define HDA_GAIN_UNITY_Q15	32768u

static inline hda_u32 hda_vol_to_q15(int vol, int max_vol, int muted)
{
	if (muted || max_vol <= 0 || vol <= 0)
		return 0;
	if (vol >= max_vol)
		return HDA_GAIN_UNITY_Q15;
	return ((hda_u32)vol * (hda_u32)vol * HDA_GAIN_UNITY_Q15) /
	       ((hda_u32)max_vol * (hda_u32)max_vol);
}

static inline short hda_scale_s16_q15(short s, hda_u32 gain_q15)
{
	int p;

	if (gain_q15 == 0)
		return 0;
	if (gain_q15 >= HDA_GAIN_UNITY_Q15)
		return s;
	p = (int)s * (int)gain_q15;
	return (short)((p >= 0 ? (p + 16384) : (p - 16384)) / 32768);
}

static inline void hda_clip_s16le_gain(const hda_u32 *src, volatile void *dst,
				       hda_u32 first, hda_u32 n, hda_u32 gain_q15)
{
	volatile hda_u8 *d;
	hda_u32 i;

	if (gain_q15 >= HDA_GAIN_UNITY_Q15) {
		hda_clip_s16le(src, dst, first, n);
		return;
	}
	d = (volatile hda_u8 *)dst + 2 * first;
	if (gain_q15 == 0) {
		for (i = 0; i < 2 * n; i++)
			d[i] = 0;
		return;
	}
	src += first;
	for (i = 0; i < n; i++) {
		hda_u16 v = (hda_u16)hda_scale_s16_q15(hda_f2s16(src[i]), gain_q15);

		d[2 * i] = (hda_u8)v;
		d[2 * i + 1] = (hda_u8)(v >> 8);
	}
}

#endif /* _HDA_CALC_H_ */
