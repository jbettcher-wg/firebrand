/*
 * The si_dpm port's "atom/atom.h": Firebrand's interpreter (shared/atom, the
 * same Linux atom.c with frb_ names), under the names RadeonNI's code calls.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_SI_DPM_ATOM_SHIM_H
#define FRB_SI_DPM_ATOM_SHIM_H
#include "../../../shared/atom/atom.h"
#define atom_execute_table	frb_atom_execute_table
#define atom_parse_data_header	frb_atom_parse_data_header
#define atom_parse_cmd_header	frb_atom_parse_cmd_header
#endif
