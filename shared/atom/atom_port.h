/*
 * Shim that lets the AtomBIOS interpreter taken from Linux build against the
 * OS layer in frb_os.h instead of the Linux kernel. It exists to keep the
 * ported files close to upstream. From RadeonNI's hw/atom/atom_port.h.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#ifndef ATOM_PORT_H
#define ATOM_PORT_H

#include <stdbool.h>
#include <string.h>

#include "../frb_os.h"

/* frb_atom.c: the interpreter's last register reads, logged on a loop abort. */
void frb_atom_dump_recent_reads(struct frb_os *os);

/* also in amdgpu/frb_pp_linux.h: gcc 4.0 rejects a repeated typedef */
#ifndef FRB_LINUX_UTYPES
#define FRB_LINUX_UTYPES
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif

/*
 * Selects the bitfield layout in atombios.h. Set from the compiler's own
 * byte order, never from __BIG_ENDIAN: glibc defines that constant on
 * little-endian hosts too, and atom-types.h tests only whether it exists.
 * (Firebrand builds every parameter block byte by byte and never relies on
 * these bitfields; this keeps the headers honest for the host tests.)
 */
#define ATOM_BIG_ENDIAN FRB_BIG_ENDIAN

#define get_unaligned_le16(p)	frb_get_le16((p), 0)
#define get_unaligned_le32(p)	frb_get_le32((p), 0)

#define cpu_to_le16(v)		frb_swap_le16(v)
#define cpu_to_le32(v)		frb_swap_le32(v)
#define le16_to_cpu(v)		frb_swap_le16(v)
#define le32_to_cpu(v)		frb_swap_le32(v)

#define atom_err(os, ...)	frb_log((os), FRB_LOG_ERROR, __VA_ARGS__)
#define atom_info(os, ...)	frb_log((os), FRB_LOG_INFO, __VA_ARGS__)
#define atom_dbg(os, ...)	frb_log((os), FRB_LOG_DEBUG, __VA_ARGS__)

#endif /* ATOM_PORT_H */
