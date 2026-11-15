/*
 * DC's link fallback after a failed DP training (link_dp_capability.c
 * decide_fallback_link_setting, 8b/10b), on its own so the unit test can
 * walk it: a monitor that fails training is not something a test rig has.
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_DP_FALLBACK_H
#define FRB_DP_FALLBACK_H

#include <stdint.h>

/*
 * dp_perform_link_training's result. LT_CR_FAIL is DC's CR_FAIL_LANE0
 * (dp_get_cr_failure: the lowest lane without CR_DONE).
 */
enum { LT_SUCCESS = 0, LT_CR_FAIL, LT_EQ_FAIL_CR, LT_EQ_FAIL_EQ, LT_LINK_LOSS, LT_CR_FAIL_LANE1, LT_CR_FAIL_LANE23 };

/*
 * The next lower setting after a failed training, within the maximum (which
 * an EQ failure lowers); 0 when there is none. Rates are DPCD link rate
 * codes (0x06 RBR, 0x0a HBR, 0x14 HBR2, 0x1e HBR3).
 */
int frb_dce12_dp_decide_fallback(uint32_t *max_rate, uint32_t max_lanes, uint32_t *rate, uint32_t *lanes, int status);

#endif
