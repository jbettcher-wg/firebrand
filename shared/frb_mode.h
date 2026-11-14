/*
 * Display modes and EDID, shared by the display code and the framebuffer.
 * From RadeonNI's hw/rdn_mode.h (the mode and EDID parts).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_MODE_H
#define FRB_MODE_H

#include <stdbool.h>
#include <string.h>
#include "frb_os.h"

/* DisplayPort HBR2 x4 carries 4K60 at 24 bpp (533 MHz); the DCE takes 600 */
#define FRB_MODE_MAX_CLOCK	600000

#define FRB_MODE_NHSYNC		(1 << 0)	/* horizontal sync active low */
#define FRB_MODE_NVSYNC		(1 << 1)	/* vertical sync active low */

struct frb_mode {
	uint32_t clock;		/* pixel clock in kHz */
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t flags;
};

bool frb_edid_detailed_mode(const uint8_t *edid, int index, struct frb_mode *mode);
bool frb_edid_preferred_mode(const uint8_t *edid, struct frb_mode *mode);
int frb_edid_modes(const uint8_t *edid, int len, struct frb_mode *out, int max,
		   int *preferred);
bool frb_edid_is_hdmi(const uint8_t *edid, int len);

/*
 * The sink's audio, as RadeonNI's DP audio reads it (hw/rdn_dce8_afmt.h's
 * struct rdn_edid_audio): the CEA-861 short audio descriptors as the EDID
 * has them (3 bytes each), the speaker allocation, the HDMI VSDB latencies,
 * the identity and the monitor name without its newline or trailing spaces.
 */
#define FRB_MAX_CEA_SADS	16
#define FRB_SINK_NAME_MAX	18
struct frb_edid_audio {
	bool has_audio;
	bool is_hdmi;
	bool has_speaker_alloc;
	uint8_t speaker_alloc;
	uint16_t mfg_id;
	uint16_t prod_id;
	char sink_name[FRB_SINK_NAME_MAX + 1];
	uint8_t sink_name_len;
	uint8_t sad_count;
	uint8_t sads[FRB_MAX_CEA_SADS][3];
	bool latency_present[2];
	uint8_t video_latency[2];
	uint8_t audio_latency[2];
};
/* RadeonNI's rdn_edid_extract_audio: false for no EDID; has_audio says whether the sink takes audio */
bool frb_edid_extract_audio(const uint8_t *edid, size_t len, struct frb_edid_audio *out);

/* frb_dmt.c: the VESA DMT modes the EDID's established/standard timings name */
extern const struct frb_mode frb_dmt_modes[];
extern const unsigned frb_dmt_mode_count;

static inline unsigned frb_mode_hz(const struct frb_mode *m)
{
	uint32_t total = (uint32_t)m->htotal * m->vtotal;

	return total ? (unsigned)(((uint64_t)m->clock * 1000 + total / 2) / total) : 0;
}

#endif
