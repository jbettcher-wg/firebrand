/*
 * Copyright (C) 2018  Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * Trimmed by tools/mk-soc15-headers.py from Linux's
 * drivers/gpu/drm/amd/include/asic_reg/nbio/nbio_7_4_sh_mask.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _nbio_7_4_SH_MASK_HEADER
#define _nbio_7_4_SH_MASK_HEADER

#define PCIE_INDEX2__PCIE_INDEX2__SHIFT                                                                       0x0
#define PCIE_INDEX2__PCIE_INDEX2_MASK                                                                         0xFFFFFFFFL
#define PCIE_DATA2__PCIE_DATA2__SHIFT                                                                         0x0
#define PCIE_DATA2__PCIE_DATA2_MASK                                                                           0xFFFFFFFFL
#define BIOS_SCRATCH_0__BIOS_SCRATCH_0__SHIFT                                                                 0x0
#define BIOS_SCRATCH_0__BIOS_SCRATCH_0_MASK                                                                   0xFFFFFFFFL
#define BIOS_SCRATCH_4__BIOS_SCRATCH_4__SHIFT                                                                 0x0
#define BIOS_SCRATCH_4__BIOS_SCRATCH_4_MASK                                                                   0xFFFFFFFFL
#define BIOS_SCRATCH_6__BIOS_SCRATCH_6__SHIFT                                                                 0x0
#define BIOS_SCRATCH_6__BIOS_SCRATCH_6_MASK                                                                   0xFFFFFFFFL
#define BIOS_SCRATCH_7__BIOS_SCRATCH_7__SHIFT                                                                 0x0
#define BIOS_SCRATCH_7__BIOS_SCRATCH_7_MASK                                                                   0xFFFFFFFFL
#define RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0__SHIFT                                                 0x18
#define RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0_MASK                                                   0x0F000000L
#define PCIEP_STRAP_MISC__STRAP_MULTI_FUNC_EN__SHIFT                                                          0xa
#define PCIEP_STRAP_MISC__STRAP_MULTI_FUNC_EN_MASK                                                            0x00000400L
#define RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN__SHIFT                                                     0x0
#define RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK                                                       0x00000001L
#define RCC_CONFIG_MEMSIZE__CONFIG_MEMSIZE__SHIFT                                                             0x0
#define RCC_CONFIG_MEMSIZE__CONFIG_MEMSIZE_MASK                                                               0xFFFFFFFFL
#define BIF_FB_EN__FB_READ_EN_MASK                                                                            0x00000001L
#define BIF_FB_EN__FB_WRITE_EN_MASK                                                                           0x00000002L
#define GPU_HDP_FLUSH_REQ__CP0__SHIFT                                                                         0x0
#define GPU_HDP_FLUSH_REQ__SDMA0__SHIFT                                                                       0xa
#define GPU_HDP_FLUSH_REQ__SDMA1__SHIFT                                                                       0xb
#define GPU_HDP_FLUSH_REQ__CP0_MASK                                                                           0x00000001L
#define GPU_HDP_FLUSH_REQ__SDMA0_MASK                                                                         0x00000400L
#define GPU_HDP_FLUSH_REQ__SDMA1_MASK                                                                         0x00000800L
#define GPU_HDP_FLUSH_DONE__CP0__SHIFT                                                                        0x0
#define GPU_HDP_FLUSH_DONE__SDMA0__SHIFT                                                                      0xa
#define GPU_HDP_FLUSH_DONE__SDMA1__SHIFT                                                                      0xb
#define GPU_HDP_FLUSH_DONE__CP0_MASK                                                                          0x00000001L
#define GPU_HDP_FLUSH_DONE__SDMA0_MASK                                                                        0x00000400L
#define GPU_HDP_FLUSH_DONE__SDMA1_MASK                                                                        0x00000800L
#define BIF_SDMA0_DOORBELL_RANGE__OFFSET__SHIFT                                                               0x2
#define BIF_SDMA0_DOORBELL_RANGE__SIZE__SHIFT                                                                 0x10
#define BIF_SDMA0_DOORBELL_RANGE__OFFSET_MASK                                                                 0x00000FFCL
#define BIF_SDMA0_DOORBELL_RANGE__SIZE_MASK                                                                   0x001F0000L
#define PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD__SHIFT                                                      0x4
#define PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD_MASK                                                        0x00000070L
#define PCIE_CNTL2__SLV_MEM_LS_EN_MASK                                                                        0x00010000L
#define PCIE_CNTL2__MST_MEM_LS_EN_MASK                                                                        0x00040000L
#define PCIE_CNTL2__REPLAY_MEM_LS_EN_MASK                                                                     0x00080000L

#endif
