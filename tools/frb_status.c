/*
 * frb_status.h: Firebrand's status from user space, without root.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include "frb_status.h"

/* --- the registry ------------------------------------------------------- */

static void copy_cstr(char *dst, size_t n, const char *src)
{
	if (!n)
		return;
	strncpy(dst, src ? src : "", n - 1);
	dst[n - 1] = 0;
}

/* a string property (CFString, or CFData holding text) */
static int reg_string(io_registry_entry_t e, const char *key, char *buf, size_t n)
{
	CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	CFTypeRef v = k ? IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0) : 0;
	int ok = 0;

	buf[0] = 0;
	if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
		ok = CFStringGetCString((CFStringRef)v, buf, (CFIndex)n, kCFStringEncodingUTF8);
	} else if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
		CFIndex len = CFDataGetLength((CFDataRef)v);

		if ((size_t)len >= n)
			len = (CFIndex)n - 1;
		memcpy(buf, CFDataGetBytePtr((CFDataRef)v), (size_t)len);
		buf[len] = 0;
		ok = 1;
	}
	if (v)
		CFRelease(v);
	if (k)
		CFRelease(k);
	return ok;
}

/* a number property (CFNumber, or a 4-byte CFData in host order, as IOPCIDevice's ids) */
static int reg_number(io_registry_entry_t e, const char *key, unsigned *out)
{
	CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	CFTypeRef v = k ? IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0) : 0;
	int ok = 0;

	if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
		long long x = 0;

		ok = CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &x);
		*out = (unsigned)x;
	} else if (v && CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)v) >= 4) {
		uint32_t x;

		memcpy(&x, CFDataGetBytePtr((CFDataRef)v), 4);
		*out = x;
		ok = 1;
	}
	if (v)
		CFRelease(v);
	if (k)
		CFRelease(k);
	return ok;
}

/* a boolean property */
static int reg_bool(io_registry_entry_t e, const char *key)
{
	CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
	CFTypeRef v = k ? IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0) : 0;
	int on = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);

	if (v)
		CFRelease(v);
	if (k)
		CFRelease(k);
	return on;
}

/* IOPCIDevice "reg": phys.hi = bus 23:16, device 15:11, function 10:8 */
static int pci_location(io_registry_entry_t pci, unsigned *bus, unsigned *dev, unsigned *fn)
{
	unsigned reg;

	if (!reg_number(pci, "reg", &reg))
		return 0;
	*bus = (reg >> 16) & 0xff;
	*dev = (reg >> 11) & 0x1f;
	*fn = (reg >> 8) & 7;
	return 1;
}

/* the card's HD Audio function: another function of the same device, class 0x0403 */
static void find_audio_function(struct frb_card *c)
{
	io_iterator_t it = 0;
	io_service_t s;

	if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOPCIDevice"), &it) != KERN_SUCCESS)
		return;
	while ((s = IOIteratorNext(it))) {
		unsigned bus, dev, fn, cc = 0;

		if (pci_location(s, &bus, &dev, &fn) && bus == c->bus && dev == c->dev && fn != c->fn &&
		    reg_number(s, "class-code", &cc) && (cc >> 8) == 0x0403) {
			io_iterator_t kids = 0;
			io_registry_entry_t k;

			reg_number(s, "vendor-id", &c->audio_fn_vendor);
			reg_number(s, "device-id", &c->audio_fn_device);
			c->audio_fn_fn = fn;
			c->audio_fn_driver[0] = 0;
			if (IORegistryEntryGetChildIterator(s, kIOServicePlane, &kids) == KERN_SUCCESS) {
				while (!c->audio_fn_driver[0] && (k = IOIteratorNext(kids))) {
					io_name_t cls;

					if (IOObjectGetClass(k, cls) == KERN_SUCCESS)
						copy_cstr(c->audio_fn_driver, sizeof(c->audio_fn_driver), cls);
					IOObjectRelease(k);
				}
				IOObjectRelease(kids);
			}
		}
		IOObjectRelease(s);
	}
	IOObjectRelease(it);
}

/* the PCI device's own properties: its identity, and the report the driver left there */
static void read_pci(struct frb_card *c, io_registry_entry_t pci)
{
	reg_string(pci, "FirebrandSupport", c->support, sizeof(c->support));
	reg_string(pci, "FirebrandStageCap", c->stage_cap, sizeof(c->stage_cap));
	reg_string(pci, "FirebrandStageFailedAt", c->failed_at, sizeof(c->failed_at));
	reg_string(pci, "FirebrandStageError", c->stage_error, sizeof(c->stage_error));
	reg_string(pci, "FirebrandVBIOS", c->vbios, sizeof(c->vbios));
	c->unlocked = reg_bool(pci, "FirebrandUnlocked");
	reg_number(pci, "vendor-id", &c->vendor);
	reg_number(pci, "device-id", &c->device);
	reg_number(pci, "subsystem-vendor-id", &c->subsys_vendor);
	reg_number(pci, "subsystem-id", &c->subsys_device);
	reg_number(pci, "revision-id", &c->revision);
	reg_number(pci, "VRAM,totalMB", &c->vram_mb);
	reg_number(pci, "VRAM,visibleMB", &c->visible_mb);
	reg_number(pci, "IOPCIExpressLinkStatus", &c->reg_link_status);
	reg_number(pci, "IOPCIExpressLinkCapabilities", &c->reg_link_caps);
	pci_location(pci, &c->bus, &c->dev, &c->fn);
}

static void read_registry(struct frb_card *c)
{
	io_registry_entry_t pci = 0;
	io_name_t cls;
	unsigned v;

	if (c->detached) {
		/* no driver on the card: everything is on the PCI device */
		copy_cstr(c->service_class, sizeof(c->service_class), "none");
		reg_string(c->service, "FirebrandASIC", c->asic, sizeof(c->asic));
		reg_string(c->service, "FirebrandBoard", c->board, sizeof(c->board));
		reg_string(c->service, "FirebrandStageDone", c->stage, sizeof(c->stage));
		reg_string(c->service, "FirebrandVersion", c->version, sizeof(c->version));
		if (reg_number(c->service, "FirebrandStageFailed", &v))
			c->stage_failed = (int)v;
		read_pci(c, c->service);
		find_audio_function(c);
		return;
	}
	if (IOObjectGetClass(c->service, cls) == KERN_SUCCESS)
		copy_cstr(c->service_class, sizeof(c->service_class), cls);
	reg_string(c->service, "FirebrandASIC", c->asic, sizeof(c->asic));
	reg_string(c->service, "FirebrandBoard", c->board, sizeof(c->board));
	reg_string(c->service, "FirebrandStageDone", c->stage, sizeof(c->stage));
	reg_string(c->service, "FirebrandVersion", c->version, sizeof(c->version));
	if (reg_number(c->service, "FirebrandStageFailed", &v))
		c->stage_failed = (int)v;
	if (IORegistryEntryGetParentEntry(c->service, kIOServicePlane, &pci) != KERN_SUCCESS)
		return;
	read_pci(c, pci);
	IOObjectRelease(pci);
	find_audio_function(c);
}

int frb_card_support(const struct frb_card *c)
{
	if (c->have_status && c->st.version >= 2)
		return (int)c->st.support;
	if (!strcmp(c->support, "verified"))
		return FRB_SUPPORT_VERIFIED;
	if (!strcmp(c->support, "untested"))
		return FRB_SUPPORT_UNTESTED;
	if (!strcmp(c->support, "not ported"))
		return FRB_SUPPORT_NOT_PORTED;
	return -1;
}

int frb_card_unlocked(const struct frb_card *c)
{
	if (c->have_status && c->st.version >= 2)
		return (c->st.flags & FRB_SF_UNLOCKED) != 0;
	return c->unlocked;
}

int frb_card_has(const struct frb_card *c, uint32_t bit)
{
	return c->have_status && (c->st.valid & bit) == bit;
}

kern_return_t frb_card_refresh(struct frb_card *c)
{
	struct frb_user_status st;
	IOByteCount size = sizeof(st);
	kern_return_t r;

	reg_string(c->service, "FirebrandStageDone", c->stage, sizeof(c->stage));
	if (c->detached) {
		io_registry_entry_t pci = c->service;

		reg_string(pci, "FirebrandStageCap", c->stage_cap, sizeof(c->stage_cap));
		c->unlocked = reg_bool(pci, "FirebrandUnlocked");
	}
	if (!c->conn) {
		c->have_status = 0;
		return c->open_error ? c->open_error : kIOReturnNotOpen;
	}
	memset(&st, 0, sizeof(st));
	r = IOConnectMethodScalarIStructureO(c->conn, FRB_US_STATUS, 0, &size, &st);
	if (r == kIOReturnUnsupported) {
		/* frb-monitor@ec27d6ecb, the one build before FRB_US_STATUS: the status as 'frb1''s method 25 */
		size = sizeof(st);
		memset(&st, 0, sizeof(st));
		r = IOConnectMethodScalarIStructureO(c->conn, 25, 0, &size, &st);
	}
	c->read_error = r;
	if (r == KERN_SUCCESS && st.version >= 1 && size >= offsetof(struct frb_user_status, valid) + 4) {
		/* a shorter answer (an older driver) leaves the rest zero: its bits are clear */
		c->st = st;
		c->have_status = 1;
	} else {
		c->have_status = 0;
	}
	return r;
}

int frb_cards_open(struct frb_card *cards, int max)
{
	static const char *classes[] = { FRB_UC_FB_CLASS, FRB_UC_SERVICE_CLASS };
	struct frb_card *c;
	int n = 0, k, i, j;

	for (k = 0; k < 2; k++) {
		io_iterator_t it = 0;
		io_service_t s;

		if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching(classes[k]), &it) != KERN_SUCCESS)
			continue;
		while ((s = IOIteratorNext(it))) {
			if (n >= max) {
				IOObjectRelease(s);
				continue;
			}
			c = &cards[n++];
			memset(c, 0, sizeof(*c));
			c->service = s;
			read_registry(c);
			c->open_error = IOServiceOpen(s, mach_task_self(), FRB_UC_TYPE_STATUS, &c->conn);
			if (c->open_error)
				c->conn = 0;
			frb_card_refresh(c);
		}
		IOObjectRelease(it);
	}
	/*
	 * Cards no Firebrand service stayed on (a start that failed, a Vega 10/12
	 * it only identifies): the report the driver left on the PCI device.
	 */
	{
		io_iterator_t it = 0;
		io_service_t s;

		if (IOServiceGetMatchingServices(kIOMasterPortDefault, IOServiceMatching("IOPCIDevice"), &it) == KERN_SUCCESS) {
			while ((s = IOIteratorNext(it))) {
				char support[16];
				unsigned bus, dev, fn;
				int known = 0;

				if (n >= max || !reg_string(s, "FirebrandSupport", support, sizeof(support)) ||
				    !pci_location(s, &bus, &dev, &fn)) {
					IOObjectRelease(s);
					continue;
				}
				for (i = 0; i < n; i++)
					if (cards[i].bus == bus && cards[i].dev == dev && cards[i].fn == fn)
						known = 1;
				if (known) {
					IOObjectRelease(s);
					continue;
				}
				c = &cards[n++];
				memset(c, 0, sizeof(*c));
				c->service = s;
				c->detached = 1;
				c->open_error = kIOReturnNotAttached;
				read_registry(c);
				frb_card_refresh(c);
			}
			IOObjectRelease(it);
		}
	}
	/* by PCI bus, so "card 1" stays the same card */
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && cards[j - 1].bus > cards[j].bus; j--) {
			struct frb_card t = cards[j];

			cards[j] = cards[j - 1];
			cards[j - 1] = t;
		}
	return n;
}

void frb_cards_close(struct frb_card *cards, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (cards[i].conn)
			IOServiceClose(cards[i].conn);
		if (cards[i].service)
			IOObjectRelease(cards[i].service);
		cards[i].conn = 0;
		cards[i].service = 0;
	}
}

/* --- words ------------------------------------------------------------------ */

const char *frb_stage_label(unsigned stage)
{
	static const char *n[] = { "survey", "post", "gmc", "smu", "dce", "gfx", "sdma", "uvd" };

	return stage < 8 ? n[stage] : "?";
}

const char *frb_card_name(const struct frb_card *c)
{
	/* the registry's first: the status call's copy is cut at 63 characters */
	if (c->board[0])
		return c->board;
	if (c->have_status && c->st.board[0])
		return c->st.board;
	if (c->have_status && c->st.asic[0])
		return c->st.asic;
	if (c->asic[0])
		return c->asic;
	return "AMD GPU";
}

const char *frb_card_short_name(const struct frb_card *c, char *buf, size_t n)
{
	const char *name = frb_card_name(c), *p = strchr(name, '(');
	size_t len = p ? (size_t)(p - name) : strlen(name);

	while (len && name[len - 1] == ' ')
		len--;
	if (len >= n)
		len = n - 1;
	memcpy(buf, name, len);
	buf[len] = 0;
	return buf;
}

static const char *asic_of(const struct frb_card *c)
{
	return c->have_status && c->st.asic[0] ? c->st.asic : c->asic;
}

/* the chip's own name, from amdgpu's ASIC name */
static const char *chip_label(const char *asic)
{
	static const struct { const char *asic, *chip, *family; } t[] = {
		{ "TAHITI", "Tahiti", "Southern Islands (GFX6)" },
		{ "PITCAIRN", "Pitcairn", "Southern Islands (GFX6)" },
		{ "VERDE", "Cape Verde", "Southern Islands (GFX6)" },
		{ "OLAND", "Oland", "Southern Islands (GFX6)" },
		{ "HAINAN", "Hainan", "Southern Islands (GFX6)" },
		{ "BONAIRE", "Bonaire", "Sea Islands (GFX7)" },
		{ "HAWAII", "Hawaii", "Sea Islands (GFX7)" },
		{ "TONGA", "Tonga", "Volcanic Islands (GFX8)" },
		{ "FIJI", "Fiji", "Volcanic Islands (GFX8)" },
		{ "POLARIS10", "Polaris 10", "Polaris (GFX8)" },
		{ "POLARIS11", "Polaris 11", "Polaris (GFX8)" },
		{ "POLARIS12", "Polaris 12", "Polaris (GFX8)" },
		{ "VEGAM", "Vega M", "Polaris (GFX8)" },
		{ "VEGA10", "Vega 10", "Vega (GFX9)" },
		{ "VEGA12", "Vega 12", "Vega (GFX9)" },
		{ "VEGA20", "Vega 20", "Vega (GFX9)" },
	};
	static char buf[96];
	unsigned i;

	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (asic && !strcmp(asic, t[i].asic)) {
			snprintf(buf, sizeof(buf), "%s, %s", t[i].chip, t[i].family);
			return buf;
		}
	return asic && asic[0] ? asic : FRB_NOT_AVAILABLE;
}

const char *frb_family_label(const struct frb_card *c)
{
	return chip_label(asic_of(c));
}

const char *frb_connector_label(unsigned type)
{
	switch (type) {
	case 0x13: return "DisplayPort";
	case 0x14: return "eDP";
	case 0x0c: return "HDMI";
	case 0x01: case 0x02: return "DVI-I";
	case 0x03: case 0x04: return "DVI-D";
	case 0x05: return "VGA";
	default: return "connector";
	}
}

const char *frb_link_speed_label(unsigned speed)
{
	static const char *n[] = { "?", "2.5 GT/s", "5.0 GT/s", "8.0 GT/s", "16.0 GT/s", "32.0 GT/s" };

	return speed < 6 ? n[speed] : "?";
}

/* ixFEATURE_STATUS (smu_7_1_3_sh_mask.h FEATURE_STATUS__*) */
static const char *smu7_features[32] = {
	"SCLK_DPM", "MCLK_DPM", "LCLK_DPM", "UVD_DPM", "VCE_DPM", "SAMU_DPM", "ACP_DPM", "PCIE_DPM",
	"BAPM", "LPMX", "NBDPM", "LHTC", "VPC", "VOLTAGE_CONTROLLER", "TDC_LIMIT", "GPU_CAC",
	"AVS", "SPMI", "SCLK_DPM_FORCED", "MCLK_DPM_FORCED", "LCLK_DPM_FORCED", "PCIE_DPM_FORCED",
};

/* vega20 FEATURE_*_BIT (smu11_driver_if.h; amdgpu's GNLD_* in this order) */
static const char *smu11_features[64] = {
	"DPM_PREFETCHER", "DPM_GFXCLK", "DPM_UCLK", "DPM_SOCCLK", "DPM_UVD", "DPM_VCE", "ULV", "DPM_MP0CLK",
	"DPM_LINK", "DPM_DCEFCLK", "DS_GFXCLK", "DS_SOCCLK", "DS_LCLK", "PPT", "TDC", "THERMAL",
	"GFX_PER_CU_CG", "RM", "DS_DCEFCLK", "ACDC", "VR0HOT", "VR1HOT", "FW_CTF", "LED_DISPLAY",
	"FAN_CONTROL", "GFX_EDC", "GFXOFF", "CG", "DPM_FCLK", "DS_FCLK", "DS_MP1CLK", "DS_MP0CLK",
	"XGMI", "ECC",
};

void frb_feature_names(unsigned kind, uint64_t mask, char *buf, size_t n)
{
	const char **names = kind == FRB_FEATURES_SMU7 ? smu7_features : kind == FRB_FEATURES_SMU11 ? smu11_features : 0;
	unsigned bits = kind == FRB_FEATURES_SMU7 ? 32 : 64, i;
	size_t len = 0;

	buf[0] = 0;
	for (i = 0; names && i < bits && len + 1 < n; i++) {
		char unk[16];
		const char *name;

		if (!(mask & ((uint64_t)1 << i)))
			continue;
		name = names[i];
		if (!name) {
			snprintf(unk, sizeof(unk), "bit%u", i);
			name = unk;
		}
		len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? " " : "", name);
	}
	if (len >= n)
		buf[n - 1] = 0;
}

void frb_throttle_names(uint32_t mask, char *buf, size_t n)
{
	static const char *t[16] = {
		"PADDING", "TEMP_EDGE", "TEMP_HOTSPOT", "TEMP_HBM", "TEMP_VR_GFX", "TEMP_VR_SOC", "TEMP_VR_MEM0",
		"TEMP_VR_MEM1", "TEMP_LIQUID", "TEMP_PLX", "TEMP_SKIN", "TDC_GFX", "TDC_SOC", "PPT", "FIT", "PPM",
	};
	size_t len = 0;
	unsigned i;

	buf[0] = 0;
	if (!mask) {
		copy_cstr(buf, n, "none");
		return;
	}
	for (i = 0; i < 32 && len + 1 < n; i++) {
		if (!(mask & (1u << i)))
			continue;
		if (i < 16)
			len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? " " : "", t[i]);
		else
			len += (size_t)snprintf(buf + len, n - len, "%sbit%u", len ? " " : "", i);
	}
	if (len >= n)
		buf[n - 1] = 0;
}

/* --- the rows ------------------------------------------------------------------ */

struct rows {
	struct frb_status_row *r;
	int n, max;
	const char *section;
};

static void row(struct rows *rs, const char *key, const char *fmt, ...)
{
	struct frb_status_row *r;
	va_list ap;

	if (rs->n >= rs->max)
		return;
	r = &rs->r[rs->n++];
	r->section = rs->section;
	copy_cstr(r->key, sizeof(r->key), key);
	va_start(ap, fmt);
	vsnprintf(r->value, sizeof(r->value), fmt, ap);
	va_end(ap);
}

static void degrees(char *buf, size_t n, int32_t mc)
{
	snprintf(buf, n, "%d \xc2\xb0""C", (int)(mc / 1000));
}

static void mb_or_gb(char *buf, size_t n, unsigned mb)
{
	if (mb >= 1024 && !(mb % 1024))
		snprintf(buf, n, "%u GB", mb / 1024);
	else
		snprintf(buf, n, "%u MB", mb);
}

static void rows_card(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;
	int have = c->have_status;

	rs->section = "Card";
	row(rs, "Name", "%s", frb_card_name(c));
	row(rs, "Chip", "%s", frb_family_label(c));
	row(rs, "PCI ID", "%04x:%04x rev %02x, subsystem %04x:%04x",
	    have ? st->vendor : c->vendor, have ? st->device : c->device, have ? st->revision : c->revision,
	    have ? st->subsys_vendor : c->subsys_vendor, have ? st->subsys_device : c->subsys_device);
	row(rs, "PCI location", "bus %u, device %u, function %u", c->bus, c->dev, c->fn);
	if (have && st->version >= 2 && st->vbios_pn[0])
		row(rs, "VBIOS", "%s%s%s%s%s", st->vbios_pn, st->vbios_ver[0] ? ", version " : "", st->vbios_ver,
		    st->vbios_date[0] ? ", " : "", st->vbios_date);
	else if (c->vbios[0])
		row(rs, "VBIOS", "%s", c->vbios);
	else
		row(rs, "VBIOS", FRB_NOT_AVAILABLE);
	switch (frb_card_support(c)) {
	case FRB_SUPPORT_VERIFIED:
		row(rs, "Support", "verified on hardware (this card's own row)");
		break;
	case FRB_SUPPORT_UNTESTED:
		row(rs, "Support", "UNTESTED card (no verified row): %s",
		    frb_card_unlocked(c) ? "unlocked, every stage the family has (sudo frbctl lock goes back)" :
		    "display-only (stage dce); sudo frbctl unlock tries the rest");
		break;
	case FRB_SUPPORT_NOT_PORTED:
		row(rs, "Support", "recognised, NOT PORTED yet: identified only, nothing written to the card");
		break;
	default:
		break;
	}
}

static void rows_driver(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;

	rs->section = "Driver";
	if (c->version[0])
		row(rs, "Version", "Firebrand %s", c->version);
	else if (c->have_status)
		row(rs, "Version", "Firebrand %s (%s)", st->driver_version, st->build[0] ? st->build : "build unknown");
	else
		row(rs, "Version", "Firebrand, version " FRB_NOT_AVAILABLE " (a driver older than the status call)");
	if (c->have_status)
		row(rs, "Status interface", "version %u, %u bytes", st->version, st->size);
	else
		row(rs, "Status interface", FRB_NOT_AVAILABLE " (IOServiceOpen 'frbs': 0x%x; the driver predates it)",
		    (unsigned)c->open_error);
	if (c->have_status) {
		char verified[48];

		if (st->row_max_stage == 0xff)
			snprintf(verified, sizeof(verified), "no family-table row");
		else if (st->version >= 2 && st->support != FRB_SUPPORT_VERIFIED)
			snprintf(verified, sizeof(verified), "allowed to %s", frb_stage_label(st->stage_cap));
		else
			snprintf(verified, sizeof(verified), "verified to %s", frb_stage_label(st->row_max_stage));
		if (st->stage_failed && st->version >= 2)
			row(rs, "Stage", "%s done, stage %s FAILED (error %d); card %s%s", frb_stage_label(st->stage_done),
			    frb_stage_label(st->stage_failed_at), (int)st->stage_failed, verified,
			    st->flags & FRB_SF_BRINGUP ? ", bring-up on" : "");
		else if (st->stage_failed)
			row(rs, "Stage", "%s done, the next one FAILED (error %d); card %s%s", frb_stage_label(st->stage_done),
			    (int)st->stage_failed, verified, st->flags & FRB_SF_BRINGUP ? ", bring-up on" : "");
		else
			row(rs, "Stage", "%s; card %s%s", frb_stage_label(st->stage_done), verified,
			    st->flags & FRB_SF_BRINGUP ? ", bring-up on (past the verified stage allowed)" : "");
		if (st->stage_failed && st->version >= 2 && st->stage_error[0])
			row(rs, "Stage failure", "%s", st->stage_error);
		row(rs, "Runs as", "%s%s%s", c->service_class[0] ? c->service_class : "?",
		    st->flags & FRB_SF_FRAMEBUFFER ? " (the desktop's display)" : " (headless control)",
		    !(st->flags & FRB_SF_ACCEL) ? ", no accelerator" :
		    frb_card_has(c, FRB_ST_VRAM_HEAP) ? ", accelerator on" :
		    st->stage_done >= 6 ? ", accelerator asked for, not started (it starts with the driver: restart)" :
		    ", accelerator asked for (it needs stage sdma)");
		if (st->flags & FRB_SF_STOPPED)
			row(rs, "State", "stopping: identity only");
	} else if (c->detached) {
		if (c->stage_failed)
			row(rs, "Stage", "%s done, stage %s FAILED (error %d); card allowed to %s", c->stage[0] ? c->stage : "?",
			    c->failed_at[0] ? c->failed_at : "?", c->stage_failed, c->stage_cap[0] ? c->stage_cap : "?");
		else
			row(rs, "Stage", "%s; card allowed to %s", c->stage[0] ? c->stage : "?", c->stage_cap[0] ? c->stage_cap : "?");
		if (c->stage_error[0])
			row(rs, "Stage failure", "%s", c->stage_error);
		row(rs, "Runs as", "nothing: Firebrand did not stay on the card (the report it left on the PCI device)");
	} else {
		row(rs, "Stage", "%s%s", c->stage[0] ? c->stage : FRB_NOT_AVAILABLE,
		    c->stage_failed ? " (the next one FAILED)" : "");
		row(rs, "Runs as", "%s", c->service_class[0] ? c->service_class : "?");
	}
}

static void rows_power(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;
	char buf[200];

	rs->section = "Power management";
	if (!c->have_status) {
		row(rs, "DPM", FRB_NOT_AVAILABLE);
		return;
	}
	switch (st->dpm_state) {
	case FRB_DPM_STATE_ON:
		row(rs, "DPM", "on");
		break;
	case FRB_DPM_STATE_FAILED:
		row(rs, "DPM", "FAILED to start (error %d)", (int)st->dpm_failed);
		break;
	case FRB_DPM_STATE_PENDING:
		row(rs, "DPM", "asked for; the stages have not reached it%s",
		    (st->flags & FRB_SF_UNTESTED) && !(st->flags & FRB_SF_UNLOCKED) ?
		    " (an untested card reaches it once unlocked)" : "");
		break;
	default:
		row(rs, "DPM", "off%s", st->flags & FRB_SF_DPM ? "" : " (FirebrandDPM off)");
		break;
	}
	if (st->version >= 2 && (st->boot_gfx_mhz || st->boot_mem_mhz))
		row(rs, "Boot clocks", "GPU %u MHz, memory %u MHz (the VBIOS's, without power management)",
		    st->boot_gfx_mhz, st->boot_mem_mhz);
	if (frb_card_has(c, FRB_ST_SMU)) {
		uint64_t f = (uint64_t)st->features_hi << 32 | st->features_lo;

		if (st->smu_version)
			row(rs, "SMU firmware", "%u.%u.%u", st->smu_version >> 16, (st->smu_version >> 8) & 0xff,
			    st->smu_version & 0xff);
		frb_feature_names(st->features_kind, f, buf, sizeof(buf));
		row(rs, "SMU features", "0x%08x%08x%s%s", (unsigned)st->features_hi, (unsigned)st->features_lo,
		    buf[0] ? ": " : "", buf);
	} else {
		row(rs, "SMU features", FRB_NOT_AVAILABLE);
	}
	if (frb_card_has(c, FRB_ST_GATING))
		row(rs, "Clock gating", "%s (cg_flags 0x%08x)",
		    st->gating & 2 ? "on, gated now" : st->gating & 1 ? "asked for, ungated now" :
		    st->family == 110 || st->family == 120 ? "off (frb_cg=0)" : "off (FirebrandGating off)",
		    (unsigned)st->cg_flags_lo);
	else if (st->family == 110)
		/* SI gates since 8bda848ce; a driver before 'the status call reports SI's gating' does not say */
		row(rs, "Clock gating", "on/off unknown (this driver build does not report SI's)");
	else
		row(rs, "Clock gating", FRB_NOT_AVAILABLE " (not ported for this family)");
	if (frb_card_has(c, FRB_ST_THROTTLE)) {
		frb_throttle_names(st->throttler, buf, sizeof(buf));
		row(rs, "Throttling", "%s", buf);
	} else {
		row(rs, "Throttling", FRB_NOT_AVAILABLE);
	}
}

static void levels(char *buf, size_t n, const uint16_t *l, unsigned count)
{
	if (!count)
		buf[0] = 0;
	else
		snprintf(buf, n, " (DPM %u..%u MHz, %u levels)", l[0], l[count - 1], count);
}

static void rows_sensors(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;
	int lv = frb_card_has(c, FRB_ST_LEVELS);
	char buf[96], t1[24], t2[24], t3[24];

	rs->section = "Sensors";
	if (frb_card_has(c, FRB_ST_CLOCKS)) {
		levels(buf, sizeof(buf), st->gfx_levels, lv ? st->gfx_level_count : 0);
		/* Vega 20's is the SMU's AverageGfxclkFrequency; Polaris's the clock now */
		row(rs, "GPU clock", "%u MHz%s%s", st->gfx_mhz, st->features_kind == FRB_FEATURES_SMU11 ? " average" : "", buf);
		levels(buf, sizeof(buf), st->mem_levels, lv ? st->mem_level_count : 0);
		row(rs, "Memory clock", "%u MHz%s", st->mem_mhz, buf);
	} else {
		row(rs, "GPU clock", FRB_NOT_AVAILABLE);
		row(rs, "Memory clock", FRB_NOT_AVAILABLE);
	}
	if (frb_card_has(c, FRB_ST_SOC_CLOCKS))
		row(rs, "SoC / fabric clock", "%u MHz / %u MHz", st->soc_mhz, st->fclk_mhz);
	if (frb_card_has(c, FRB_ST_TEMP) || frb_card_has(c, FRB_ST_TEMP_HOTSPOT)) {
		size_t len = 0;

		buf[0] = 0;
		if (frb_card_has(c, FRB_ST_TEMP)) {
			degrees(t1, sizeof(t1), st->temp_mc);
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s edge", t1);
		}
		if (frb_card_has(c, FRB_ST_TEMP_HOTSPOT)) {
			degrees(t2, sizeof(t2), st->temp_hotspot_mc);
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%s hotspot", len ? ", " : "", t2);
		}
		if (frb_card_has(c, FRB_ST_TEMP_MEM)) {
			degrees(t3, sizeof(t3), st->temp_mem_mc);
			snprintf(buf + len, sizeof(buf) - len, "%s%s memory", len ? ", " : "", t3);
		}
		row(rs, "Temperature", "%s", buf);
	} else {
		row(rs, "Temperature", FRB_NOT_AVAILABLE);
	}
	if (frb_card_has(c, FRB_ST_POWER)) {
		if (frb_card_has(c, FRB_ST_POWER_LIMIT))
			row(rs, "Power", "%u.%u W (limit %u W)", st->power_mw / 1000, (st->power_mw % 1000) / 100,
			    st->power_limit_w);
		else
			row(rs, "Power", "%u.%u W", st->power_mw / 1000, (st->power_mw % 1000) / 100);
	} else {
		row(rs, "Power", FRB_NOT_AVAILABLE);
	}
	if (frb_card_has(c, FRB_ST_LOAD))
		row(rs, "Load", "GPU %u %%, memory controller %u %% (the SMU's activity averages)", st->gpu_load, st->mem_load);
	else
		row(rs, "Load", FRB_NOT_AVAILABLE);
	if (frb_card_has(c, FRB_ST_VDDGFX))
		row(rs, "Voltage", "VDDGFX %u mV", st->vddgfx_mv);
	else
		row(rs, "Voltage", FRB_NOT_AVAILABLE);
	if (frb_card_has(c, FRB_ST_FAN))
		row(rs, "Fan", "%u RPM (%u %%)", st->fan_rpm, (st->fan_pwm * 100 + 127) / 255);
	else
		row(rs, "Fan", FRB_NOT_AVAILABLE "%s", c->have_status && st->dpm_state == FRB_DPM_STATE_ON ?
		    " (no fan, or no tachometer)" : "");
}

static void rows_uvd(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;

	rs->section = "Video decoder (UVD)";
	if (!frb_card_has(c, FRB_ST_UVD)) {
		row(rs, "State", FRB_NOT_AVAILABLE);
		return;
	}
	switch (st->uvd_state) {
	case FRB_UVD_STATE_RUNNING:
		row(rs, "State", "running (VCPU up%s)", st->dpm_state == FRB_DPM_STATE_ON ? ", UVD DPM on" : "");
		break;
	case FRB_UVD_STATE_IDLE:
		row(rs, "State", "idle (VCPU stopped, clocks down)");
		break;
	default:
		row(rs, "State", "not started (stage uvd %s)", st->stage_done < 7 ? "not run" : "has no decoder here");
		return;
	}
	if (st->uvd_flags & FRB_UVD_FLAG_POISONED)
		row(rs, "Health", "OUT OF SERVICE: a decode timed out (until the driver reloads)");
	row(rs, "Instances", "%u", st->uvd_instances);
	row(rs, "Sessions", "%u open", st->uvd_sessions);
	/* amdgpu_uvd_sw_init: Vega on keeps the header's ucode_version as it is, earlier families pack it */
	if (st->uvd_fw_version && st->family >= 141)
		row(rs, "Firmware", "ENC %u.%u, DEC .%u, family %u", st->uvd_fw_version >> 30, (st->uvd_fw_version >> 24) & 0x3f,
		    (st->uvd_fw_version >> 8) & 0xff, st->uvd_fw_version & 0xff);
	else if (st->uvd_fw_version)
		row(rs, "Firmware", "%u.%u, family %u", st->uvd_fw_version >> 24, (st->uvd_fw_version >> 16) & 0xff,
		    (st->uvd_fw_version >> 8) & 0xff);
}

static void rows_display(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;
	unsigned i;

	rs->section = "Displays";
	if (!frb_card_has(c, FRB_ST_DISPLAY)) {
		row(rs, "Display", FRB_NOT_AVAILABLE "%s", c->have_status ? " (stage dce not done)" : "");
		return;
	}
	for (i = 0; i < st->num_connectors && i < 6; i++) {
		const struct frb_user_status_conn *u = &st->conn[i];
		char key[24], name[40], link[96], max[64];

		snprintf(key, sizeof(key), "Connector %u", i);
		name[0] = link[0] = max[0] = 0;
		if (u->sink[0])
			snprintf(name, sizeof(name), " \"%s\"", u->sink);
		if (u->dp_max_rate_khz)
			snprintf(max, sizeof(max), "sink max %u.%u Gbps x%u", u->dp_max_rate_khz / 100000,
				 (u->dp_max_rate_khz / 10000) % 10, u->dp_max_lanes);
		if (!u->active) {
			row(rs, key, "%s: %s%s%s%s%s", frb_connector_label(u->type), u->connected ? "connected, not driven" : "empty",
			    name, max[0] ? " (" : "", max, max[0] ? ")" : "");
			continue;
		}
		if (u->dp_rate_khz)
			snprintf(link, sizeof(link), ", DP %u.%u Gbps x%u%s%s%s", u->dp_rate_khz / 100000,
				 (u->dp_rate_khz / 10000) % 10, u->dp_lanes, max[0] ? " (" : "", max, max[0] ? ")" : "");
		row(rs, key, "%s%s: %ux%u @ %u.%02u Hz, %u bpc%s%s", frb_connector_label(u->type), name, u->width, u->height,
		    u->refresh_chz / 100, u->refresh_chz % 100, u->bpc, link, u->connected ? "" : " (hot-plug sense says unplugged)");
	}
}

static void rows_pcie(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;

	rs->section = "PCI Express";
	if (!frb_card_has(c, FRB_ST_SLOT_LINK)) {
		/* an older driver: IOPCIFamily's snapshot from enumeration (the GPU function's own link) */
		if (c->reg_link_status)
			row(rs, "GPU link", "%s x%u (card max %s x%u), as enumerated at boot", frb_link_speed_label(c->reg_link_status & 0xf),
			    (c->reg_link_status >> 4) & 0x3f, frb_link_speed_label(c->reg_link_caps & 0xf), (c->reg_link_caps >> 4) & 0x3f);
		else
			row(rs, "Link", FRB_NOT_AVAILABLE);
		return;
	}
	row(rs, "Link to host", "%s x%u (card max %s x%u)%s", frb_link_speed_label(st->slot_speed), st->slot_width,
	    frb_link_speed_label(st->slot_max_speed), st->slot_max_width,
	    st->slot_vendor == st->vendor && st->slot_device == st->device ? "" : ", at the package's upstream port");
	if (frb_card_has(c, FRB_ST_LINK) && (st->slot_vendor != st->vendor || st->slot_device != st->device))
		row(rs, "GPU internal link", "%s x%u (%04x:%04x, the in-package switch)", frb_link_speed_label(st->link_speed),
		    st->link_width, st->slot_vendor, st->slot_device);
}

static void rows_audio(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;

	rs->section = "Audio";
	if (!frb_card_has(c, FRB_ST_AUDIO))
		row(rs, "DP audio", FRB_NOT_AVAILABLE "%s", c->have_status && !(st->flags & FRB_SF_AUDIO) ?
		    " (FirebrandAudio off)" : "");
	else if (!(st->flags & FRB_SF_AUDIO))
		row(rs, "DP audio", "off (FirebrandAudio off)");
	else if (st->audio_endpoint >= 0)
		row(rs, "DP audio", "%s: endpoint %d on DIG %d; the sink %s audio", st->audio_on ? "on" : "set up, stream off",
		    (int)st->audio_endpoint, (int)st->audio_dig, st->audio_sink ? "takes" : "does not take");
	else
		row(rs, "DP audio", "no stream with audio (the sink %s audio)", st->audio_sink ? "takes" : "does not take");
	if (c->audio_fn_device)
		row(rs, "HD Audio function", "%04x:%04x (function %u), %s", c->audio_fn_vendor, c->audio_fn_device,
		    c->audio_fn_fn, c->audio_fn_driver[0] ? c->audio_fn_driver : "no driver attached");
	else
		row(rs, "HD Audio function", "none found");
}

static void rows_memory(const struct frb_card *c, struct rows *rs)
{
	const struct frb_user_status *st = &c->st;
	unsigned vram = c->have_status ? st->vram_mb : c->vram_mb, vis = c->have_status ? st->vram_visible_mb : c->visible_mb;
	char a[24], b[24];

	rs->section = "Memory";
	if (vram) {
		mb_or_gb(a, sizeof(a), vram);
		mb_or_gb(b, sizeof(b), vis);
		if (c->have_status && st->vram_width)
			row(rs, "VRAM", "%s, %u-bit; %s CPU-visible", a, st->vram_width, b);
		else
			row(rs, "VRAM", "%s; %s CPU-visible", a, b);
	} else {
		row(rs, "VRAM", FRB_NOT_AVAILABLE);
	}
	if (frb_card_has(c, FRB_ST_VRAM_HEAP))
		row(rs, "VRAM in use", "%u MB of the %u MB heap (clients and surfaces)", (st->vram_heap_used_kb + 1023) / 1024,
		    st->vram_heap_kb / 1024);
	else
		row(rs, "VRAM in use", FRB_NOT_AVAILABLE "%s", c->have_status ? " (no accelerator)" : "");
	if (frb_card_has(c, FRB_ST_GART))
		row(rs, "GART in use", "%u MB of %u MB", (st->gart_used_kb + 1023) / 1024, st->gart_kb / 1024);
	else
		row(rs, "GART in use", FRB_NOT_AVAILABLE);
}

int frb_card_rows(const struct frb_card *c, struct frb_status_row *r, int max)
{
	struct rows rs;

	rs.r = r;
	rs.n = 0;
	rs.max = max;
	rs.section = "";
	rows_card(c, &rs);
	rows_driver(c, &rs);
	rows_power(c, &rs);
	rows_sensors(c, &rs);
	rows_uvd(c, &rs);
	rows_display(c, &rs);
	rows_pcie(c, &rs);
	rows_audio(c, &rs);
	rows_memory(c, &rs);
	return rs.n;
}

/* --- the report ----------------------------------------------------------------- */

struct out {
	char *buf;
	size_t cap, len;
};

static void put(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(o->len < o->cap ? o->buf + o->len : 0, o->len < o->cap ? o->cap - o->len : 0, fmt, ap);
	va_end(ap);
	if (n > 0)
		o->len += (size_t)n;
}

/* Mac OS X's version from SystemVersion.plist */
static void os_version(char *buf, size_t n)
{
	CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
		CFSTR("/System/Library/CoreServices/SystemVersion.plist"), kCFURLPOSIXPathStyle, false);
	CFDataRef data = 0;
	CFPropertyListRef plist = 0;
	char ver[32] = "", build[32] = "";

	if (url && CFURLCreateDataAndPropertiesFromResource(kCFAllocatorDefault, url, &data, 0, 0, 0) && data)
		plist = CFPropertyListCreateFromXMLData(kCFAllocatorDefault, data, kCFPropertyListImmutable, 0);
	if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		CFStringRef v = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("ProductVersion"));
		CFStringRef b = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("ProductBuildVersion"));

		if (v && CFGetTypeID(v) == CFStringGetTypeID())
			CFStringGetCString(v, ver, sizeof(ver), kCFStringEncodingUTF8);
		if (b && CFGetTypeID(b) == CFStringGetTypeID())
			CFStringGetCString(b, build, sizeof(build), kCFStringEncodingUTF8);
	}
	if (plist)
		CFRelease(plist);
	if (data)
		CFRelease(data);
	if (url)
		CFRelease(url);
	snprintf(buf, n, "Mac OS X %s (%s)", ver[0] ? ver : "?", build[0] ? build : "?");
}

size_t frb_report(struct frb_card *cards, int n, const char *generator, int fenced, char *buf, size_t cap)
{
	struct frb_status_row rows[FRB_MAX_ROWS];
	struct out o;
	struct utsname u;
	char os[64], model[64], when[32];
	size_t len = sizeof(model);
	time_t now = time(0);
	int i, k, nr;

	o.buf = buf;
	o.cap = cap;
	o.len = 0;
	if (cap)
		buf[0] = 0;
	os_version(os, sizeof(os));
	if (sysctlbyname("hw.model", model, &len, 0, 0))
		copy_cstr(model, sizeof(model), "?");
	if (uname(&u))
		copy_cstr(u.release, sizeof(u.release), "?");
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
	if (fenced)
		put(&o, "```text\n");
	put(&o, "Firebrand status report (%s, %s)\n", generator, when);
	put(&o, "System: %s, Darwin %s, %s\n", os, u.release, model);
	if (!n)
		put(&o, "\nNo Firebrand card found (Firebrand.kext not loaded, or no card it drives).\n");
	for (i = 0; i < n; i++) {
		const char *section = 0;

		put(&o, "\nCard %d of %d: %s\n", i + 1, n, frb_card_name(&cards[i]));
		switch (frb_card_support(&cards[i])) {
		case FRB_SUPPORT_UNTESTED:
			put(&o, "  ** UNTESTED CARD: Firebrand has not been verified on this card. It runs display-only\n"
				"  ** unless unlocked (sudo frbctl unlock, then restart). Please paste this report into an\n"
				"  ** issue at " FRB_ISSUES_URL ",\n"
				"  ** with what you tried and what worked or broke.\n");
			break;
		case FRB_SUPPORT_NOT_PORTED:
			put(&o, "  ** NOT PORTED: Firebrand recognises this chip but has no driver for it yet; it only\n"
				"  ** identified the card. A report at " FRB_ISSUES_URL "\n"
				"  ** helps decide what comes next.\n");
			break;
		default:
			break;
		}
		nr = frb_card_rows(&cards[i], rows, FRB_MAX_ROWS);
		for (k = 0; k < nr; k++) {
			if (!section || strcmp(section, rows[k].section)) {
				section = rows[k].section;
				put(&o, "  %s\n", section);
			}
			put(&o, "    %-20s %s\n", rows[k].key, rows[k].value);
		}
	}
	if (fenced)
		put(&o, "```\n");
	if (cap && o.len >= cap)
		buf[cap - 1] = 0;
	return o.len;
}
