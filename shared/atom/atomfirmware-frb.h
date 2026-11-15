/*
 * atomfirmware.h (Linux 7.1.1, SoC15 VBIOS interface, copied verbatim) as
 * Firebrand includes it: the stdint types first, the kernel's __counted_by
 * annotation as nothing, and BEFORE atom.h / atombios.h. amdgpu includes them
 * in that order too: atombios.h #defines many names atomfirmware.h declares
 * as enum constants, which only works with the enums seen first.
 *
 * VBIOS data tables are read with frb_get_le16/32 at offsetof() positions in
 * these packed structures; no structure is dereferenced whole (byte-order.md).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_ATOMFIRMWARE_FRB_H
#define FRB_ATOMFIRMWARE_FRB_H

#include <stddef.h>
#include "../frb_os.h"
#ifndef __counted_by
#define __counted_by(member)
#endif
#include "atomfirmware.h"

#endif
