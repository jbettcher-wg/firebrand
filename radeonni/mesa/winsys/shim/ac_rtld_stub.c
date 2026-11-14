/*
 * ac_rtld without libelf: the five entry points refuse. radeonsi reaches
 * them only for ELF shader binaries, which the LLVM backend produces and
 * ACO never does; this build has no LLVM (meson -Damd-use-llvm=false), so
 * nothing calls these at run time. Linked into libamd_common by
 * mesa/patches/0030-rdn-radeonsi-without-drm.patch when libelf is absent,
 * instead of cross-building libelf for the target.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "ac_rtld.h"

bool ac_rtld_open(struct ac_rtld_binary *binary, struct ac_rtld_open_info i)
{
   (void)binary;
   (void)i;
   fprintf(stderr, "rdn: ELF shader binaries are not supported in this build (no libelf)\n");
   return false;
}

void ac_rtld_close(struct ac_rtld_binary *binary)
{
   (void)binary;
}

bool ac_rtld_get_section_by_name(struct ac_rtld_binary *binary, const char *name, const char **data,
                                 size_t *nbytes)
{
   (void)binary;
   (void)name;
   *data = NULL;
   *nbytes = 0;
   return false;
}

bool ac_rtld_read_config(const struct ac_compiler_info *compiler_info,
                         struct ac_rtld_binary *binary, struct ac_shader_config *config)
{
   (void)compiler_info;
   (void)binary;
   (void)config;
   return false;
}

int ac_rtld_upload(struct ac_rtld_upload_info *u)
{
   (void)u;
   return -1;
}
