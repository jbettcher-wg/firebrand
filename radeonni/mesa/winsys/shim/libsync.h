/*
 * libdrm's libsync.h, for radeonsi's includes without libdrm: Mesa carries
 * the same inline sync-file helpers in util/libsync.h, and radeonsi calls
 * none of them on this winsys (no fence file descriptors here).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#ifndef RDN_LIBSYNC_SHIM_H
#define RDN_LIBSYNC_SHIM_H
#include "util/libsync.h"
#endif
