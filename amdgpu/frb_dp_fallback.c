/*
 * DC's link fallback after a failed DP training (link_dp_capability.c
 * decide_fallback_link_setting, 8b/10b), on its own so the unit test can
 * walk it: a monitor that fails training is not something a test rig has.
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_dp_fallback.h"

/* reduce_link_rate / reduce_lane_count (8b/10b: HBR3, HBR2, HBR, RBR; 4, 2, 1) */
static uint32_t reduce_link_rate(uint32_t rate)
{
	return rate > 0x14 ? 0x14 : rate > 0x0a ? 0x0a : 0x06;
}

static uint32_t reduce_lane_count(uint32_t lanes)
{
	return lanes > 2 ? 2 : 1;
}

int frb_dce12_dp_decide_fallback(uint32_t *max_rate, uint32_t max_lanes, uint32_t *rate, uint32_t *lanes, int status)
{
	/*
	 * Not DC: a lane that the setting does not have cannot be the one that
	 * failed (dp_get_cr_failure never says so); taken as the highest lane
	 * there is, so that no status can make the walk go round.
	 */
	if (status == LT_CR_FAIL_LANE23 && *lanes <= 2)
		status = LT_CR_FAIL_LANE1;
	if (status == LT_CR_FAIL_LANE1 && *lanes <= 1)
		status = LT_CR_FAIL;
	switch (status) {
	case LT_CR_FAIL:		/* LINK_TRAINING_CR_FAIL_LANE0 */
	case LT_CR_FAIL_LANE1:
	case LT_CR_FAIL_LANE23:
		if (*rate > 0x06) {
			*rate = reduce_link_rate(*rate);
		} else if (*lanes > 1) {
			*rate = *max_rate;
			if (status == LT_CR_FAIL)
				return 0;
			else if (status == LT_CR_FAIL_LANE1)
				*lanes = 1;
			else
				*lanes = 2;
		} else {
			return 0;
		}
		break;
	case LT_EQ_FAIL_EQ:
		if (*lanes > 1) {
			*lanes = reduce_lane_count(*lanes);
		} else if (*rate > 0x06) {
			*rate = reduce_link_rate(*rate);
			/* the maximum comes down too: a later CR fallback must not raise the rate again */
			*max_rate = *rate;
			*lanes = max_lanes;
		} else {
			return 0;
		}
		break;
	case LT_EQ_FAIL_CR:
		if (*rate > 0x06) {
			*rate = reduce_link_rate(*rate);
			*max_rate = *rate;
			*lanes = max_lanes;
		} else {
			return 0;
		}
		break;
	default:
		return 0;
	}
	return 1;
}
