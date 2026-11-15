/*
 * Firebrand's status as user space reads it: the cards in the IORegistry,
 * each one's status client ('frbs', FRB_UC_STATUS: any user, reads only;
 * kext/Firebrand/frb_user.h) and the words for what it says. Shared by
 * frbctl report and Firebrand Monitor (tools/FirebrandMonitor), so the two
 * say the same thing in the same words.
 *
 * Nothing here needs root, and nothing here parses frbctl's output. A driver
 * older than the status call still gives its identity, stage, version and
 * VRAM size through the registry; the live readings are then "not available".
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_STATUS_H
#define FRB_STATUS_H

#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include "frb_user.h"

#define FRB_MAX_CARDS		4
#define FRB_NOT_AVAILABLE	"not available"

struct frb_card {
	io_service_t service;		/* Firebrand or FirebrandFramebuffer (retained) */
	io_connect_t conn;		/* the status client; 0 none */
	kern_return_t open_error;	/* IOServiceOpen's answer for 'frbs' (an older driver: unsupported) */
	kern_return_t read_error;	/* the last FRB_UC_STATUS call's */
	int have_status;		/* st holds the last good answer */
	struct frb_user_status st;
	/* the registry: any driver version */
	char service_class[32];		/* "FirebrandFramebuffer", "Firebrand" */
	char asic[24];			/* FirebrandASIC */
	char board[96];			/* FirebrandBoard */
	char stage[16];			/* FirebrandStageDone */
	char version[64];		/* FirebrandVersion ("" before the status call existed) */
	int stage_failed;
	unsigned vendor, device, subsys_vendor, subsys_device, revision;
	unsigned bus, dev, fn;		/* the PCI function the driver sits on */
	unsigned vram_mb, visible_mb;	/* VRAM,totalMB / VRAM,visibleMB on the PCI device */
	unsigned reg_link_status, reg_link_caps;	/* IOPCIFamily's IOPCIExpressLink*, set at enumeration; 0 none */
	/* the card's HD Audio function (the same device, class 0x0403): 0 none */
	unsigned audio_fn_vendor, audio_fn_device, audio_fn_fn;
	char audio_fn_driver[48];	/* the class driving it ("HDAudioDevice"), "" none */
};

/* every Firebrand card, its status client opened where the driver has one; the count */
int frb_cards_open(struct frb_card *cards, int max);
void frb_cards_close(struct frb_card *cards, int n);
/* FRB_UC_STATUS again (and the registry's stage); 0, or the IOKit error */
kern_return_t frb_card_refresh(struct frb_card *c);
/* the field is there: the card answered the status call with that bit */
int frb_card_has(const struct frb_card *c, uint32_t bit);

/* words */
const char *frb_card_name(const struct frb_card *c);		/* the row's board, else the ASIC, else the ids */
const char *frb_card_short_name(const struct frb_card *c, char *buf, size_t n);	/* up to the first '(' */
const char *frb_family_label(const struct frb_card *c);	/* "Vega (GFX9)" ... */
const char *frb_stage_label(unsigned stage);			/* "survey" ... "uvd" */
const char *frb_connector_label(unsigned type);
const char *frb_link_speed_label(unsigned speed);		/* "5.0 GT/s" */
/* names of the bits set in a feature mask (FRB_FEATURES_*), space-separated */
void frb_feature_names(unsigned kind, uint64_t mask, char *buf, size_t n);
/* names of the SMU11 ThrottlerStatus bits set, "none" for 0 */
void frb_throttle_names(uint32_t mask, char *buf, size_t n);

/*
 * The card's status as lines: section, key, value. The report prints them;
 * the monitor's list shows them. Fields the card does not provide read
 * FRB_NOT_AVAILABLE (with the reason when there is one).
 */
struct frb_status_row {
	const char *section;
	char key[24];
	char value[200];
};
#define FRB_MAX_ROWS		64
int frb_card_rows(const struct frb_card *c, struct frb_status_row *rows, int max);

/*
 * The plain-text report (a GitHub issue's paste): the system, then every
 * card's rows. generator names the tool ("frbctl", "Firebrand Monitor 1.0");
 * fenced wraps it in a ``` block. Writes at most cap bytes (NUL included);
 * returns the length the whole report needs.
 */
size_t frb_report(struct frb_card *cards, int n, const char *generator, int fenced, char *buf, size_t cap);

#endif /* FRB_STATUS_H */
