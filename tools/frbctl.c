/*
 * frbctl -- Firebrand.kext from userland, for bring-up and diagnosis.
 * Administrator only ('frb1' client, kext/Firebrand/frb_user.h). In the
 * spirit of RadeonNI's rdnctl: each experiment is a command, not a reboot.
 *
 *   frbctl info                  identity, stage, VRAM layout, trace and log sizes
 *   frbctl log                   the driver's log ring
 *   frbctl rd <reg>              read a register (dword index, as amdgpu numbers them)
 *   frbctl wr <reg> <value>      write one (needs frb_regwrite=1 or FirebrandAllowRegWrite)
 *   frbctl cfg <fn> <off> [val]  configuration dword of function fn of the card's device (a write
 *                                needs the same switch)
 *   frbctl smc <addr>            read SMC space through SMC_IND_INDEX_11
 *   frbctl smcdump <a> <n> <file> SMC RAM as the SMC holds it (smc-ram-smcorder.bin form)
 *   frbctl regfile <file>        read every "mmio NAME 0xBYTEOFFSET" / "smc NAME 0xADDR"
 *                                line of a capture register list; print it in the
 *                                capture's regs.txt format, for diffing
 *   frbctl stage <n>             run the bring-up up to stage n (1 post, 2 gmc, 3 smu, 4 dce)
 *   frbctl vram <offset> [n]     words of VRAM through BAR0 (after stage gmc)
 *   frbctl trace on|off          start a fresh register trace / stop it
 *   frbctl trace dump            the trace as "R|W reg value" lines (tests/mktrace.py's form)
 *   frbctl display               connectors, sinks, DPCD, the mode on CRTC0 (after stage dce)
 *   frbctl detect                hot-plug sense, DPCD and EDID again
 *   frbctl edid <n> [file]       connector n's EDID: hex, or raw to a file
 *   frbctl modes <n>             the modes its EDID offers (preferred first)
 *   frbctl mode <n> [W H [Hz]] [rate=MHz]
 *                                light connector n: the EDID's preferred mode, or W x H (@Hz);
 *                                rate caps the DP link (162, 270, 540); a colour-bar pattern
 *   frbctl off                   CRTC0 and its encoder off
 *   frbctl fill bars|grey|black|splash  a test pattern (or the boot splash) on the current mode
 *   frbctl fill solid <rrggbb> | dot <x> <y> | top <lines> | left <columns>
 *   frbctl dpcd <n> <addr> [len] the sink's DPCD registers
 *   frbctl crc <n> [count]       the sink's CRC of the frames it receives (DP test CRC)
 *   frbctl pipecrc [count]       the CRTC's own CRC of what it sends (DCE 12; no monitor needed)
 *   frbctl test gfx|sdma0|sdma1 ring|ib|copy|fill   an engine test (after stage gfx / sdma)
 *   frbctl test sdma0|sdma1 gtt  copies VRAM<->GTT and a fill into GTT (bus mastering)
 *   frbctl test gfx ibprobe      which IB swap bits run which IBs (diagnosis; docs/accel-interface.md Q4b)
 *   frbctl test uvd ring|ib|swapprobe|idle   UVD's decode ring (after stage uvd): the ring test, the
 *                                create/destroy msg IB test, both UVD_LMI_SWAP_CNTL settings, the idle work
 *   frbctl fb detach             before unloading FirebrandFramebuffer: it refuses new window
 *                                server clients (then killall WindowServer, kextunload)
 *   frbctl report [--md]         every card's status, as Firebrand Monitor's Copy Report
 *                                (frb_status.c; any user, through the read-only 'frbs' client);
 *                                --md wraps it in a ``` block for a GitHub issue
 *
 * Builds on Mac OS X 10.5 (IOKit only; scripts/build-mac.sh does this):
 *   gcc -std=gnu99 -I../kext/Firebrand -I../shared -o frbctl frbctl.c frb_status.c ../shared/frb_edid.c \
 *       ../shared/frb_dmt.c -framework IOKit -framework CoreFoundation
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "frb_user.h"
#include "frb_mode.h"
#include "frb_status.h"

static io_connect_t conn;

/* frbctl report [--md]: no root needed (the 'frbs' status client and the registry) */
static int report(int argc, char **argv)
{
	struct frb_card cards[FRB_MAX_CARDS];
	int n = frb_cards_open(cards, FRB_MAX_CARDS), md = argc > 2 && !strcmp(argv[2], "--md");
	size_t need = frb_report(cards, n, "frbctl", md, 0, 0);
	char *text = (char *)malloc(need + 1);

	if (text) {
		frb_report(cards, n, "frbctl", md, text, need + 1);
		fputs(text, stdout);
		free(text);
	}
	frb_cards_close(cards, n);
	return n ? 0 : 1;
}

static int scalar(int method, int nin, int a, int b, int *out)
{
	int dummy = 0;

	if (!out)
		out = &dummy;
	if (nin == 1)
		return IOConnectMethodScalarIScalarO(conn, method, 1, 1, a, out);
	return IOConnectMethodScalarIScalarO(conn, method, 2, 0, a, b);
}

static const char *stage_name(unsigned s)
{
	static const char *n[] = { "survey", "post", "gmc", "smu", "dce", "gfx", "sdma", "uvd" };
	return s < 8 ? n[s] : "?";
}

static const char *conn_type(unsigned t)
{
	switch (t) {
	case 0x13: return "DisplayPort";
	case 0x14: return "eDP";
	case 0x0c: return "HDMI-A";
	case 0x01: case 0x02: return "DVI-I";	/* single, dual link */
	case 0x03: case 0x04: return "DVI-D";
	default:   return "other";
	}
}

static int get_display(struct frb_user_display *d)
{
	IOByteCount sz = sizeof(*d);
	int r;

	memset(d, 0, sizeof(*d));
	r = IOConnectMethodScalarIStructureO(conn, FRB_UC_DISPLAY_INFO, 0, &sz, d);
	if (r)
		fprintf(stderr, "display info: 0x%x\n", r);
	else if (!d->ready)
		fprintf(stderr, "the display is not up (stage dce not done)\n");
	return r || !d->ready;
}

static int get_edid(int n, uint8_t *edid, IOByteCount *len)
{
	*len = FRB_EDID_BYTES;
	return IOConnectMethodScalarIStructureO(conn, FRB_UC_EDID, 1, len, n, edid);
}

static void edid_name(const uint8_t *e, unsigned len, char *name, int cap)
{
	int i, k;

	name[0] = 0;
	if (len < 128)
		return;
	for (i = 54; i < 126; i += 18) {
		if (e[i] || e[i + 1] || e[i + 3] != 0xfc)
			continue;
		for (k = 0; k < 13 && k + 1 < cap && e[i + 5 + k] != 0x0a; k++)
			name[k] = e[i + 5 + k];
		name[k] = 0;
	}
}

static int map(int type, vm_address_t *addr, vm_size_t *size)
{
	int r = IOConnectMapMemory(conn, type, mach_task_self(), addr, size, kIOMapAnywhere);

	if (r)
		fprintf(stderr, "map of memory type %d failed (0x%x)\n", type, r);
	return r;
}

int main(int argc, char **argv)
{
	io_service_t service;
	int r, v = 0, i;

	if (argc < 2) {
		fprintf(stderr, "usage: frbctl info|log|rd|wr|smc|regfile|stage|vram|trace|"
			"display|detect|edid|modes|mode|off|fill|report ...\n");
		return 2;
	}
	if (!strcmp(argv[1], "report"))
		return report(argc, argv);
	/* the control personality, or the framebuffer when it owns the card */
	service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(FRB_UC_SERVICE_CLASS));
	if (!service)
		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(FRB_UC_FB_CLASS));
	if (!service || IOServiceOpen(service, mach_task_self(), FRB_UC_TYPE, &conn)) {
		fprintf(stderr, "cannot reach Firebrand (loaded? root?)\n");
		return 1;
	}

	if (!strcmp(argv[1], "info")) {
		struct frb_user_info in;
		IOByteCount sz = sizeof(in);

		memset(&in, 0, sizeof(in));
		r = IOConnectMethodScalarIStructureO(conn, FRB_UC_GET_INFO, 0, &sz, &in);
		if (r) {
			fprintf(stderr, "info: 0x%x\n", r);
			return 1;
		}
		printf("device       %04x:%04x rev %02x subsystem %04x:%04x, rev_id %u, external rev 0x%x\n",
		       in.vendor, in.device, in.revision, in.subsys_vendor, in.subsys_device,
		       in.rev_id, in.external_rev_id);
		printf("board        %s (verified to stage %s)\n", in.board[0] ? in.board : "(no row)",
		       in.row_max_stage == 0xff ? "-" : stage_name(in.row_max_stage));
		printf("stage        %s done, failed %d, ASIC_Init %u ms\n", stage_name(in.stage_done),
		       in.stage_failed, in.asic_init_ms);
		printf("bars         registers %u KB, aperture %u MB\n", in.rmmio_size >> 10, in.aper_size >> 20);
		printf("vram         %u MB, %u-bit, at 0x%x%08x; GART at 0x%x%08x\n", in.vram_mb, in.vram_width,
		       in.vram_start_hi, in.vram_start_lo, in.gart_start_hi, in.gart_start_lo);
		printf("layout       fw_buf 0x%08x smu 0x%08x toc 0x%08x scratch 0x%08x gart 0x%08x (BAR0 offsets)\n",
		       in.fw_buf_offset, in.smu_buffer_offset, in.toc_offset, in.mem_scratch_offset,
		       in.gart_table_offset);
		printf("dummy page   bus 0x%x%08x\n", in.dummy_hi, in.dummy_lo);
		printf("mc ucode     %s 0x%08x\n", in.mc_fw[0] ? in.mc_fw : "-", in.mc_fw_version);
		printf("trace        %u of %u entries (%u lost); log %u of %u bytes\n", in.trace_len,
		       in.trace_cap, in.trace_lost, in.log_len, in.log_cap);
	} else if (!strcmp(argv[1], "log")) {
		vm_address_t addr = 0;
		vm_size_t size = 0;

		if (map(FRB_UC_MEMORY_LOG, &addr, &size))
			return 1;
		{
			const char *text = (const char *)addr;
			vm_size_t n = 0;

			while (n < size && text[n])	/* no strnlen before 10.7 */
				n++;
			fwrite(text, 1, n, stdout);
		}
	} else if (!strcmp(argv[1], "rd") && argc == 3) {
		uint32_t reg = strtoul(argv[2], 0, 0);

		r = scalar(FRB_UC_REG_READ, 1, (int)reg, 0, &v);
		printf("%05x %08x%s\n", reg, (unsigned)v, r ? " ERR" : "");
	} else if (!strcmp(argv[1], "wr") && argc == 4) {
		r = scalar(FRB_UC_REG_WRITE, 2, (int)strtoul(argv[2], 0, 0), (int)strtoul(argv[3], 0, 0), 0);
		printf("%s\n", r ? "refused" : "ok");
	} else if (!strcmp(argv[1], "smc") && argc == 3) {
		uint32_t a = strtoul(argv[2], 0, 0);

		r = scalar(FRB_UC_SMC_READ, 1, (int)a, 0, &v);
		printf("smc %08x %08x%s\n", a, (unsigned)v, r ? " ERR" : "");
	} else if (!strcmp(argv[1], "smcdump") && argc == 5) {
		/* SMC RAM words as the SMC holds them (big-endian bytes), like the
		 * capture's smc-ram-smcorder.bin */
		uint32_t a = strtoul(argv[2], 0, 0), n = strtoul(argv[3], 0, 0), k;
		FILE *f = fopen(argv[4], "wb");
		unsigned char b[4];

		if (!f) {
			perror(argv[4]);
			return 1;
		}
		for (k = 0; k < n; k += 4) {
			r = scalar(FRB_UC_SMC_READ, 1, (int)(a + k), 0, &v);
			b[0] = (unsigned)v >> 24; b[1] = (unsigned)v >> 16; b[2] = (unsigned)v >> 8; b[3] = (unsigned)v;
			fwrite(b, 1, 4, f);
		}
		fclose(f);
		printf("smcdump: %u bytes from 0x%x\n", n, a);
	} else if (!strcmp(argv[1], "regfile") && argc == 3) {
		FILE *f = fopen(argv[2], "r");
		char line[256], kind[16], name[64];
		unsigned long off;

		if (!f) {
			perror(argv[2]);
			return 1;
		}
		while (fgets(line, sizeof(line), f)) {
			if (line[0] == '#' || sscanf(line, "%15s %63s %lx", kind, name, &off) != 3)
				continue;
			if (!strcmp(kind, "mmio"))
				r = scalar(FRB_UC_REG_READ, 1, (int)(off / 4), 0, &v);
			else if (!strcmp(kind, "smc"))
				r = scalar(FRB_UC_SMC_READ, 1, (int)off, 0, &v);
			else
				continue;
			printf("%-4s %-40s 0x%08lx 0x%08x%s\n", kind, name, off, (unsigned)v, r ? " ERR" : "");
		}
		fclose(f);
	} else if (!strcmp(argv[1], "stage") && argc == 3) {
		r = scalar(FRB_UC_RUN_STAGE, 1, atoi(argv[2]), 0, &v);
		printf("stage %s: %s, result %d (frbctl log has the details)\n", argv[2], r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "vram") && argc >= 3) {
		uint32_t off = strtoul(argv[2], 0, 0), n = argc > 3 ? strtoul(argv[3], 0, 0) : 8;

		for (i = 0; i < (int)n; i++) {
			r = scalar(FRB_UC_VRAM_READ, 1, (int)(off + 4 * i), 0, &v);
			if (!(i & 7))
				printf("%08x:", off + 4 * i);
			printf(r ? " ????????" : " %08x", (unsigned)v);
			if ((i & 7) == 7 || i + 1 == (int)n)
				printf("\n");
		}
	} else if (!strcmp(argv[1], "trace") && argc == 3 && strcmp(argv[2], "dump")) {
		r = scalar(FRB_UC_TRACE, 1, !strcmp(argv[2], "on"), 0, &v);
		printf("trace %s: %d entries\n", argv[2], v);
	} else if (!strcmp(argv[1], "trace") && argc == 3) {
		struct frb_user_info in;
		IOByteCount sz = sizeof(in);
		vm_address_t addr = 0;
		vm_size_t size = 0;
		const uint32_t *t;

		if (IOConnectMethodScalarIStructureO(conn, FRB_UC_GET_INFO, 0, &sz, &in) ||
		    map(FRB_UC_MEMORY_TRACE, &addr, &size))
			return 1;
		t = (const uint32_t *)addr;
		for (i = 0; i < (int)in.trace_len && (vm_size_t)(8 * i + 8) <= size; i++)
			printf("%c %05x %08x\n", (t[2 * i] & 0x80000000u) ? 'W' : 'R',
			       (unsigned)(t[2 * i] & 0x7fffffffu), (unsigned)t[2 * i + 1]);
		if (in.trace_lost)
			fprintf(stderr, "trace: %u entries were lost (buffer full)\n", in.trace_lost);
	} else if (!strcmp(argv[1], "display")) {
		struct frb_user_display d;

		if (get_display(&d))
			return 1;
		printf("dce          %u CRTCs, DISPCLK %u kHz, DP ext clock %u kHz\n", d.num_crtc,
		       d.default_dispclk * 10, d.dp_extclk * 10);
		for (i = 0; i < (int)d.num_connectors && i < 6; i++) {
			const struct frb_user_connector *c = &d.conn[i];
			uint8_t edid[FRB_EDID_BYTES];
			IOByteCount len = 0;
			char name[16];

			name[0] = 0;
			if (c->edid_len && !get_edid(i, edid, &len))
				edid_name(edid, (unsigned)len, name, sizeof(name));
			printf("connector %d  %s (0x%04x), encoder 0x%04x link %c, aux 0x%02x, hpd %d: %s",
			       i, conn_type(c->connector_type), c->connector_object_id, c->encoder_object_id,
			       c->linkb ? 'B' : 'A', c->i2c_id, c->hpd, c->sense ? "PLUGGED" : "empty");
			if (c->dpcd[0])
				printf(", DPCD %x.%x max %u MHz x%u", c->dpcd[0] >> 4, c->dpcd[0] & 0xf,
				       c->dpcd[1] * 27, c->dpcd[2] & 0x1f);
			if (c->edid_len)
				printf(", EDID %u bytes \"%s\"", c->edid_len, name);
			if (i == d.active)
				printf(" [CRTC0, DIG%d, %u MHz x%u]", c->dig_encoder, c->dp_clock / 1000,
				       c->dp_lane_count);
			printf("\n");
		}
		if (d.active >= 0) {
			printf("crtc0        %ux%u (%u/%u/%u, %u/%u/%u) %u kHz, adjusted %u kHz\n",
			       d.hdisplay, d.vdisplay, d.hsync_start, d.hsync_end, d.htotal,
			       d.vsync_start, d.vsync_end, d.vtotal, d.clock, d.adjusted_clock);
			printf("scanout      VRAM 0x%08x, pitch %u px; line time %u ns, watermark %u ns, "
			       "lb lead %u lines\n", d.fb_offset, d.fb_pitch, d.line_time, d.watermark,
			       d.lb_lead_lines);
		}
		printf("crtc0 regs   CRTC_CONTROL %08x, frame count %u\n", d.crtc_control, d.crtc_status_frame_count);
		if (d.audio_dig >= 0)
			printf("audio        codec pin %d -> DIG%d, HOT_PLUG_CONTROL %08x (AUDIO_ENABLED %u), CHANNEL_SPEAKER %08x\n",
			       d.audio_ep, d.audio_dig, d.audio_hot_plug, d.audio_hot_plug >> 31, d.audio_speakers);
		else
			printf("audio        off (FirebrandAudio false, no sink audio, or frbctl audio off); pin %d HOT_PLUG_CONTROL %08x\n",
			       d.audio_ep, d.audio_hot_plug);
	} else if (!strcmp(argv[1], "detect")) {
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_DETECT, 0, 1, &v);
		printf("detect: %s, first DP monitor on connector %d\n", r ? "ERR" : "ran", v);
	} else if ((!strcmp(argv[1], "edid") || !strcmp(argv[1], "modes")) && argc >= 3) {
		uint8_t edid[FRB_EDID_BYTES];
		IOByteCount len = 0;

		if (get_edid(atoi(argv[2]), edid, &len) || !len) {
			fprintf(stderr, "no EDID on connector %s\n", argv[2]);
			return 1;
		}
		if (!strcmp(argv[1], "modes")) {
			struct frb_mode m[48];
			int pref = -1, n = frb_edid_modes(edid, (int)len, m, 48, &pref);

			for (i = 0; i < n; i++)
				printf("%c %4ux%-4u @%3u  %6u kHz  h %u %u %u  v %u %u %u  %c%c\n",
				       i == pref ? '*' : ' ', m[i].hdisplay, m[i].vdisplay, frb_mode_hz(&m[i]),
				       m[i].clock, m[i].hsync_start, m[i].hsync_end, m[i].htotal,
				       m[i].vsync_start, m[i].vsync_end, m[i].vtotal,
				       (m[i].flags & FRB_MODE_NHSYNC) ? '-' : '+', (m[i].flags & FRB_MODE_NVSYNC) ? '-' : '+');
		} else if (argc >= 4) {
			FILE *f = fopen(argv[3], "wb");

			if (!f) {
				perror(argv[3]);
				return 1;
			}
			fwrite(edid, 1, len, f);
			fclose(f);
			printf("edid: %u bytes to %s\n", (unsigned)len, argv[3]);
		} else {
			for (i = 0; i < (int)len; i++)
				printf("%02x%s", edid[i], (i & 15) == 15 ? "\n" : " ");
		}
	} else if (!strcmp(argv[1], "mode") && argc >= 3) {
		int args[4] = { 0, 0, 0, 0 }, na = 0, rate = 0, k;

		for (k = 3; k < argc; k++) {
			if (!strncmp(argv[k], "rate=", 5))
				rate = atoi(argv[k] + 5) * 1000;
			else if (na < 3)
				args[na++] = atoi(argv[k]);
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_DISPLAY_MODE, 5, 1, atoi(argv[2]),
						  args[0], args[1], args[2], rate, &v);
		printf("mode: %s, result %d (frbctl display / frbctl log for the details)\n", r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "off")) {
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_DISPLAY_OFF, 0, 1, &v);
		printf("off: %s, result %d\n", r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "fill") && argc >= 3) {
		const char *pn = argv[2];
		int p = !strcmp(pn, "bars") ? FRB_PATTERN_BARS : !strcmp(pn, "grey") ? FRB_PATTERN_GREY :
			!strcmp(pn, "black") ? FRB_PATTERN_BLACK : !strcmp(pn, "solid") ? FRB_PATTERN_SOLID :
			!strcmp(pn, "dot") ? FRB_PATTERN_DOT : !strcmp(pn, "top") ? FRB_PATTERN_TOP :
			!strcmp(pn, "left") ? FRB_PATTERN_LEFT : !strcmp(pn, "splash") ? FRB_PATTERN_SPLASH :
			!strcmp(pn, "smpte") ? FRB_PATTERN_SMPTE : !strcmp(pn, "cursorref") ? FRB_PATTERN_SMPTE_CURSOR :
			atoi(pn);
		int a0 = argc > 3 ? (int)strtoul(argv[3], 0, p == FRB_PATTERN_SOLID ? 16 : 0) : 0;
		int a1 = argc > 4 ? (int)strtoul(argv[4], 0, 0) : 0;

		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_DISPLAY_FILL, 3, 1, p, a0, a1, &v);
		printf("fill: %s, result %d\n", r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "dpcd") && argc >= 4) {
		uint32_t a = strtoul(argv[3], 0, 0), n = argc > 4 ? strtoul(argv[4], 0, 0) : 16, k;

		for (k = 0; k < n; k += 4) {
			r = IOConnectMethodScalarIScalarO(conn, FRB_UC_DPCD_READ, 2, 1, atoi(argv[2]), (int)(a + k), &v);
			if (!(k & 15))
				printf("%05x:", a + k);
			if (r)
				printf(" ?? ?? ?? ??");
			else
				printf(" %02x %02x %02x %02x", ((unsigned)v >> 24) & 0xff, ((unsigned)v >> 16) & 0xff,
				       ((unsigned)v >> 8) & 0xff, (unsigned)v & 0xff);
			if ((k & 15) == 12 || k + 4 >= n)
				printf("\n");
		}
	} else if (!strcmp(argv[1], "test") && argc == 4) {
		static const char *engines[] = { "gfx", "sdma0", "sdma1", "uvd" },
			*tests[] = { "ring", "ib", "copy", "fill", "gtt", "ibprobe", "hang", "swapprobe", "idle" };
		int e, t;

		for (e = 0; e < 4 && strcmp(argv[2], engines[e]); e++)
			;
		for (t = 0; t < 9 && strcmp(argv[3], tests[t]); t++)
			;
		if (e == 4 || t == 9) {
			fprintf(stderr, "usage: frbctl test gfx|sdma0|sdma1|uvd "
				"ring|ib|copy|fill|gtt|ibprobe|hang|swapprobe|idle\n");
			return 2;
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_ENGINE_TEST, 2, 1, e, t, &v);
		printf("test %s %s: %s, result %d (frbctl log has the details)\n", argv[2], argv[3],
		       r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "cursor") && argc >= 3) {
		/* the hardware cursor, for the sink CRC: test|move|hide x y, off */
		static const char *ops[] = { "off", "test", "move", "hide", "solid" };
		int op;

		for (op = 0; op < 5 && strcmp(argv[2], ops[op]); op++)
			;
		if (op == 5 || (op != 0 && argc != 5)) {
			fprintf(stderr, "usage: frbctl cursor test|move|hide x y | solid 0xAARRGGBB rows | off\n");
			return 2;
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_CURSOR, 3, 1, op, op ? (int)strtoul(argv[3], 0, 0) : 0,
						  op ? atoi(argv[4]) : 0, &v);
		printf("cursor %s: %s, result %d\n", argv[2], r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "recover") && argc <= 3) {
		/* the busy blocks, or these regardless: gfx 1, sdma 2, all 3 */
		uint32_t force = 0, hung = 0, after = 0, soft = 0, ms = 0;

		if (argc == 3) {
			if (!strcmp(argv[2], "gfx"))
				force = 1;
			else if (!strcmp(argv[2], "sdma"))
				force = 2;
			else if (!strcmp(argv[2], "all"))
				force = 3;
			else {
				fprintf(stderr, "usage: frbctl recover [gfx|sdma|all]\n");
				return 2;
			}
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_RECOVER, 1, 5, force, &v, &hung, &after, &soft, &ms);
		printf("recover: %s, result %d; hung %s%s%s, soft recovery %s, still busy %x, rings failed %x, %u ms\n",
		       r ? "ERR" : "ran", v, hung & 1 ? "gfx " : "", hung & 2 ? "sdma " : "", hung ? "" : "none",
		       soft ? "worked" : "no", after & 0xff, (after >> 8) & 0xff, ms);
	} else if (!strcmp(argv[1], "fb") && argc == 3 && !strcmp(argv[2], "detach")) {
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_FB_DETACH, 0, 1, &v);
		printf("fb detach: %s, result %d\n", r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "lut") && argc == 3 && (!strcmp(argv[2], "linear") || !strcmp(argv[2], "restore"))) {
		/* frbctl lut linear|restore: an identity LUT for a sink CRC, and the ramp back */
		r = scalar(FRB_UC_LUT_SET, 1, !strcmp(argv[2], "linear") ? FRB_LUT_OP_LINEAR : FRB_LUT_OP_RESTORE, 0, &v);
		printf("lut %s: %s, result %d\n", argv[2], r ? "ERR" : "ran", v);
	} else if (!strcmp(argv[1], "lut") && argc <= 3) {
		/* frbctl lut [all]: the LUT store and, on a running CRTC, the hardware's entries */
		struct frb_user_lut l;
		IOByteCount n = sizeof(l);
		int all = argc == 3 && !strcmp(argv[2], "all"), k, same = 0;

		r = IOConnectMethodScalarIStructureO(conn, FRB_UC_LUT, 0, &n, &l);
		if (r) {
			fprintf(stderr, "lut: ERR 0x%x\n", r);
			return 1;
		}
		for (k = 0; k < 256; k++) {
			uint32_t want = ((uint32_t)(l.store[0][k] & 0xffc0) << 14) | ((uint32_t)(l.store[1][k] & 0xffc0) << 4) |
					(l.store[2][k] >> 6);

			if (!l.hw_read && l.hw[k] == want)
				same++;
		}
		printf("lut: screen %u bpp; hardware %s", l.bpp, l.hw_read == -2 ? "LUT in bypass (the store is the identity)" :
		       l.hw_read ? "not read (no CRTC running)" : "read");
		if (!l.hw_read)
			printf(", %d of 256 entries equal the store at 10 bits", same);
		printf("\n");
		for (k = 0; k < 256; k += all ? 1 : 17) {
			printf("  %3d: %04x %04x %04x", k, l.store[0][k], l.store[1][k], l.store[2][k]);
			if (!l.hw_read)
				printf("  hw %03x %03x %03x", (l.hw[k] >> 20) & 0x3ff, (l.hw[k] >> 10) & 0x3ff, l.hw[k] & 0x3ff);
			printf("\n");
		}
	} else if (!strcmp(argv[1], "dpm") && argc <= 4) {
		/*
		 * frbctl dpm [high|low|auto|on|off] [-q]: power_dpm_force_performance_level
		 * (or the DPM init / hw_fini by hand), then the SMC's state.
		 * frbctl dpm gating on|off: SOC15 clock gating gated / ungated live.
		 */
		static const char *ops[] = { "status", "high", "low", "auto", "on", "off" };
		static const char *opname[] = { "status", "high", "low", "auto", "on", "off", "gating on", "gating off",
						"fclk low", "fclk top" };
		struct frb_user_dpm d;
		IOByteCount n = sizeof(d);
		int op = 0, quiet = argc == 4 && !strcmp(argv[3], "-q"), k;

		if (argc >= 3 && !strcmp(argv[2], "gating")) {
			if (argc != 4 || (strcmp(argv[3], "on") && strcmp(argv[3], "off"))) {
				fprintf(stderr, "usage: frbctl dpm gating on|off\n");
				return 2;
			}
			op = !strcmp(argv[3], "on") ? FRB_DPM_OP_GATE_ON : FRB_DPM_OP_GATE_OFF;
			quiet = 0;
		} else if (argc >= 3 && !strcmp(argv[2], "fclk")) {
			/* Vega 20: FCLK's soft minimum, which a display configuration leaves at the top */
			if (argc != 4 || (strcmp(argv[3], "low") && strcmp(argv[3], "top"))) {
				fprintf(stderr, "usage: frbctl dpm fclk low|top\n");
				return 2;
			}
			op = !strcmp(argv[3], "low") ? FRB_DPM_OP_FCLK_LOW : FRB_DPM_OP_FCLK_TOP;
			quiet = 0;
		} else if (argc >= 3) {
			for (op = 0; op < 6 && strcmp(argv[2], ops[op]); op++)
				;
			if (op == 6) {	/* not FRB_DPM_OP_GATE_ON (also 6): no name matched */
				fprintf(stderr, "usage: frbctl dpm [status|high|low|auto|on [switches]|off|gating on|gating off|"
					"fclk low|fclk top] [-q]\n");
				return 2;
			}
		}
		/* dpm on N: Southern Islands' DPM switches for this try (si_dpm/rdn_si_dpm.h), e.g. 0x110 */
		if (op == 4 && argc == 4 && strcmp(argv[3], "-q")) {
			op |= (int)(strtoul(argv[3], NULL, 0) << 8);
			quiet = 0;
		}
		r = IOConnectMethodScalarIStructureO(conn, FRB_UC_DPM, 1, &n, op, &d);
		if (r) {
			fprintf(stderr, "dpm: ERR 0x%x\n", r);
			return 1;
		}
		if (op)
			printf("dpm %s: result %d\n", opname[op & 0xff], d.result);
		if (d.version >= 2 && d.soc15) {
			/* Vega 20: the SMU11's readout */
			/* DPM_UVD is feature bit 4: on while the decoder is in use (vega20_power_gate_uvd) */
			printf("dpm: %s (SMU %u.%u.%u, features %08x: UVD DPM %s); clock gating %s%s (cg_flags %08x)\n",
			       d.enabled ? "on" : d.smu_version ? "off (the SMU runs, DPM not set up)" : "off (no SMU)",
			       d.smu_version >> 16, (d.smu_version >> 8) & 0xff, d.smu_version & 0xff, d.smc_feature_status,
			       d.smc_feature_status & 0x10 ? "on" : "off",
			       d.gating & 2 ? "on" : "off", d.gating & 1 ? "" : " (FirebrandGating off)", d.cg_flags_lo);
			if (quiet) {
				printf("dpm: gfx %u MHz uclk %u MHz soc %u MHz fclk %u MHz %u mV edge %d C hotspot %d C hbm %d C "
				       "%u W load %u%% mem %u%% gating %s\n", d.cur_sclk / 100, d.cur_mclk / 100,
				       d.cur_socclk / 100, d.cur_fclk / 100, d.vddgfx_mv, d.temp_edge / 1000,
				       d.temp_hotspot / 1000, d.temp_hbm / 1000, d.power_mw / 1000, d.gpu_load, d.mem_load,
				       d.gating & 2 ? "on" : "off");
			} else {
				printf("  now: gfx %u MHz (average), uclk %u MHz, soc %u MHz, fclk %u MHz; vddgfx %u mV\n",
				       d.cur_sclk / 100, d.cur_mclk / 100, d.cur_socclk / 100, d.cur_fclk / 100, d.vddgfx_mv);
				printf("  temperature: edge %d C, hotspot %d C (THM CTF), HBM %d C; socket power %u W; "
				       "load gfx %u%%, memory %u%%\n", d.temp_edge / 1000, d.temp_hotspot / 1000,
				       d.temp_hbm / 1000, d.power_mw / 1000, d.gpu_load, d.mem_load);
				printf("  gfx levels:");
				for (k = 0; k < (int)d.gfx_count && k < 16; k++)
					printf(" %u", d.gfx_levels[k]);
				printf(" MHz\n  uclk levels:");
				for (k = 0; k < (int)d.uclk_count && k < 16; k++)
					printf(" %u", d.uclk_levels[k]);
				printf(" MHz\n  soc levels:");
				for (k = 0; k < (int)d.soc_count && k < 16; k++)
					printf(" %u", d.soc_levels[k]);
				printf(" MHz\n");
			}
			return d.result ? 1 : 0;
		}
		if (!d.enabled) {
			printf("dpm: off%s (init result %d)\n", d.failed ? ", the init FAILED" : "", d.failed);
		} else if (quiet) {
			printf("dpm: %s sclk %u MHz (level %d) mclk %u MHz (level %d) %u mV %d.%01d C fan %u rpm/%u pwm "
			       "%u.%03u W load %u%% mem %u%% pcie gen%u x%u\n",
			       d.forced_level == 8 ? "high" : d.forced_level == 4 ? "low" : d.forced_level == 1 ? "auto" : "?",
			       d.cur_sclk / 100, d.cur_sclk_index, d.cur_mclk / 100, d.cur_mclk_index, d.vddgfx_mv,
			       d.temp_mc / 1000, (d.temp_mc % 1000) / 100, d.fan_rpm, d.fan_pwm,
			       d.power_mw / 1000, d.power_mw % 1000, d.gpu_load, d.mem_load, d.pcie_gen, d.pcie_lanes);
		} else {
			printf("dpm: on, forced level %s (%u)\n",
			       d.forced_level == 8 ? "high" : d.forced_level == 4 ? "low" : d.forced_level == 1 ? "auto" : "?",
			       d.forced_level);
			printf("  sclk levels (enabled mask %02x):", d.sclk_mask);
			for (k = 0; k < (int)d.sclk_count && k < 8; k++)
				printf(" %s%u%s", k == d.cur_sclk_index ? "[" : "", d.sclk_levels[k] / 100,
				       k == d.cur_sclk_index ? "]" : "");
			printf(" MHz\n  mclk levels (enabled mask %02x):", d.mclk_mask);
			for (k = 0; k < (int)d.mclk_count && k < 8; k++)
				printf(" %s%u%s", k == d.cur_mclk_index ? "[" : "", d.mclk_levels[k] / 100,
				       k == d.cur_mclk_index ? "]" : "");
			printf(" MHz\n  now: sclk %u MHz, mclk %u MHz (the SMC's answer)\n", d.cur_sclk / 100, d.cur_mclk / 100);
			printf("  vddgfx %u mV, temperature %d.%01d C, fan %u rpm (pwm %u/255), package power %u.%03u W\n",
			       d.vddgfx_mv, d.temp_mc / 1000, (d.temp_mc % 1000) / 100, d.fan_rpm, d.fan_pwm,
			       d.power_mw / 1000, d.power_mw % 1000);
			printf("  load: gfx %u%%, memory %u%%; PCIe gen %u x%u (%u levels, mask %x); "
			       "SMC FEATURE_STATUS %08x\n", d.gpu_load, d.mem_load, d.pcie_gen, d.pcie_lanes, d.pcie_count,
			       d.pcie_mask, d.smc_feature_status);
			printf("  display config: %u display(s), %u Hz, min vblank %u us\n", d.num_display, d.vrefresh,
			       d.min_vblank_time);
		}
	} else if (!strcmp(argv[1], "cfg") && (argc == 4 || argc == 5)) {
		/*
		 * frbctl cfg <function> <offset> [value]: this device's configuration space, any function;
		 * function 0x01BBDDFF reads another device of the same host bridge (bus BB, device DD,
		 * function FF: the card's switch ports, the root port)
		 */
		int v = 0;
		unsigned fn = strtoul(argv[2], 0, 0), off = strtoul(argv[3], 0, 0);

		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_CONFIG, 4, 1, fn, off, argc == 5,
						  argc == 5 ? (int)strtoul(argv[4], 0, 0) : 0, &v);
		if (r) {
			printf("cfg %x %02x: ERR 0x%x\n", fn, off, r);
			return 1;
		}
		printf("cfg %x %02x: %08x\n", fn, off, (unsigned)v);
	} else if (!strcmp(argv[1], "smn") && (argc == 3 || argc == 4)) {
		/* frbctl smn <address> [value]: an SMN register through PCIE_INDEX2/DATA2 (SOC15) */
		int v = 0;
		unsigned a = strtoul(argv[2], 0, 0);

		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_SMN, 3, 1, (int)a, argc == 4,
						  argc == 4 ? (int)strtoul(argv[3], 0, 0) : 0, &v);
		if (r) {
			printf("smn %08x: ERR 0x%x\n", a, r);
			return 1;
		}
		printf("smn %08x: %08x\n", a, (unsigned)v);
	} else if (!strcmp(argv[1], "fn") && argc >= 3) {
		/* frbctl fn status | scan [flags]: the device's PCI functions (the HD Audio function's straps) */
		int op, res = 0, hdr = 0, id1 = 0, nubs = 0;
		unsigned arg = argc >= 4 ? strtoul(argv[3], 0, 0) : 0;

		if (!strcmp(argv[2], "status"))
			op = FRB_FN_OP_STATUS;
		else if (!strcmp(argv[2], "scan"))
			op = FRB_FN_OP_SCAN;
		else {
			fprintf(stderr, "usage: frbctl fn status | scan [flags]\n");
			return 2;
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_FUNCTIONS, 2, 4, op, (int)arg, &res, &hdr, &id1, &nubs);
		if (r) {
			printf("fn %s: ERR 0x%x\n", argv[2], r);
			return 1;
		}
		printf("fn %s: result 0x%x, header type %02x, function 1 reads %08x, %d new nub(s)\n", argv[2],
		       (unsigned)res, (unsigned)hdr, (unsigned)id1, nubs);
	} else if (!strcmp(argv[1], "audio")) {
		/* frbctl audio [status | pin N | pin auto | rerun | off | on] */
		int op = FRB_AUDIO_OP_STATUS, arg = 0, res = 0, dig = -1, pin = -1, flags = 0;

		if (argc >= 3 && !strcmp(argv[2], "pin") && argc == 4) {
			op = FRB_AUDIO_OP_PIN;
			arg = !strcmp(argv[3], "auto") ? -1 : atoi(argv[3]);
		} else if (argc == 3 && !strcmp(argv[2], "rerun")) {
			op = FRB_AUDIO_OP_RERUN;
		} else if (argc == 3 && !strcmp(argv[2], "off")) {
			op = FRB_AUDIO_OP_OFF;
		} else if (argc == 3 && !strcmp(argv[2], "on")) {
			op = FRB_AUDIO_OP_ON;
		} else if (argc != 2 && !(argc == 3 && !strcmp(argv[2], "status"))) {
			fprintf(stderr, "usage: frbctl audio [status | pin N|auto | rerun | off | on]\n");
			return 2;
		}
		r = IOConnectMethodScalarIScalarO(conn, FRB_UC_AUDIO, 2, 4, op, arg, &res, &dig, &pin, &flags);
		if (r) {
			printf("audio: ERR 0x%x\n", r);
			return 1;
		}
		printf("audio: result %d; DIG %d <- codec pin %d, packets %s, sink audio %s, pin choice %s",
		       res, dig, pin, (flags & 1) ? "on" : "off", (flags & 2) ? "yes" : "no",
		       ((flags >> 8) & 0xff) == 0xff ? "RadeonNI's" : "forced");
		if (((flags >> 8) & 0xff) != 0xff)
			printf(" (%d)", (flags >> 8) & 0xff);
		printf("\n");
	} else if ((!strcmp(argv[1], "crc") && argc >= 3) || !strcmp(argv[1], "pipecrc")) {
		int pipe = !strcmp(argv[1], "pipecrc");
		int count = pipe ? (argc > 2 ? atoi(argv[2]) : 1) : (argc > 3 ? atoi(argv[3]) : 1);
		int res = 0, cr = 0, cg = 0, cb = 0;
		uint32_t target = pipe ? FRB_PIPE_CRC : (uint32_t)atoi(argv[2]);

		for (i = 0; i < count; i++) {
			r = IOConnectMethodScalarIScalarO(conn, FRB_UC_SINK_CRC, 1, 4, target, &res, &cr, &cg, &cb);
			if (r || res)
				printf("crc: %s, result %d\n", r ? "ERR" : "ran", res);
			else
				printf("crc: R/Cr %04x  G/Y %04x  B/Cb %04x\n", cr, cg, cb);
		}
	} else {
		fprintf(stderr, "usage: frbctl info|log|rd|wr|smc|regfile|stage|vram|trace|"
			"display|detect|edid|modes|mode|off|fill|dpcd|crc|pipecrc|test|recover|dpm|lut|audio|report ...\n");
		return 2;
	}
	IOServiceClose(conn);
	return 0;
}
