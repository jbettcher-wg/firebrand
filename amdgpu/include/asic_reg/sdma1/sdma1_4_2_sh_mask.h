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
 * drivers/gpu/drm/amd/include/asic_reg/sdma1/sdma1_4_2_sh_mask.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _sdma1_4_2_0_SH_MASK_HEADER
#define _sdma1_4_2_0_SH_MASK_HEADER

#define SDMA1_CLK_CTRL__RESERVED__SHIFT                                                                       0xc
#define SDMA1_CLK_CTRL__RESERVED_MASK                                                                         0x00FFF000L
#define SDMA1_GB_ADDR_CONFIG__NUM_PIPES__SHIFT                                                                0x0
#define SDMA1_GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT                                                     0x3
#define SDMA1_GB_ADDR_CONFIG__NUM_BANKS__SHIFT                                                                0xc
#define SDMA1_GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT                                                       0x13
#define SDMA1_GB_ADDR_CONFIG__NUM_PIPES_MASK                                                                  0x00000007L
#define SDMA1_GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK                                                       0x00000038L
#define SDMA1_GB_ADDR_CONFIG__NUM_BANKS_MASK                                                                  0x00007000L
#define SDMA1_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK                                                         0x00180000L
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_PIPES__SHIFT                                                           0x0
#define SDMA1_GB_ADDR_CONFIG_READ__PIPE_INTERLEAVE_SIZE__SHIFT                                                0x3
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_BANKS__SHIFT                                                           0xc
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_SHADER_ENGINES__SHIFT                                                  0x13
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_PIPES_MASK                                                             0x00000007L
#define SDMA1_GB_ADDR_CONFIG_READ__PIPE_INTERLEAVE_SIZE_MASK                                                  0x00000038L
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_BANKS_MASK                                                             0x00007000L
#define SDMA1_GB_ADDR_CONFIG_READ__NUM_SHADER_ENGINES_MASK                                                    0x00180000L
#define SDMA1_F32_CNTL__HALT__SHIFT                                                                           0x0
#define SDMA1_F32_CNTL__HALT_MASK                                                                             0x00000001L
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                            0x0
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                   0x2
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                   0x10
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                              0x00000001L
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                     0x00000004L
#define SDMA1_GFX_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                     0xFFFF0000L
#define SDMA1_GFX_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                        0x0
#define SDMA1_GFX_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                          0x00000001L
#define SDMA1_GFX_IB_CNTL__IB_ENABLE__SHIFT                                                                   0x0
#define SDMA1_GFX_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                              0x4
#define SDMA1_GFX_IB_CNTL__IB_ENABLE_MASK                                                                     0x00000001L
#define SDMA1_GFX_IB_CNTL__IB_SWAP_ENABLE_MASK                                                                0x00000010L
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_PAGE_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_PAGE_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_PAGE_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC0_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC0_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC0_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC0_IB_CNTL__IB_ENABLE__SHIFT                                                                  0x0
#define SDMA1_RLC0_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                             0x4
#define SDMA1_RLC0_IB_CNTL__IB_ENABLE_MASK                                                                    0x00000001L
#define SDMA1_RLC0_IB_CNTL__IB_SWAP_ENABLE_MASK                                                               0x00000010L
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC1_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC1_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC1_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC1_IB_CNTL__IB_ENABLE__SHIFT                                                                  0x0
#define SDMA1_RLC1_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                             0x4
#define SDMA1_RLC1_IB_CNTL__IB_ENABLE_MASK                                                                    0x00000001L
#define SDMA1_RLC1_IB_CNTL__IB_SWAP_ENABLE_MASK                                                               0x00000010L
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC2_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC2_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC2_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC3_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC3_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC3_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC4_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC4_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC4_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC5_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC5_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC5_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC6_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC6_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC6_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA1_RLC7_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA1_RLC7_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA1_RLC7_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L

#endif
