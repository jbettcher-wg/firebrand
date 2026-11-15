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
 * drivers/gpu/drm/amd/include/asic_reg/sdma0/sdma0_4_2_offset.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _sdma0_4_2_0_OFFSET_HEADER
#define _sdma0_4_2_0_OFFSET_HEADER

#define mmSDMA0_VIRT_RESET_REQ                                                                         0x0009
#define mmSDMA0_VIRT_RESET_REQ_BASE_IDX                                                                0
#define mmSDMA0_POWER_CNTL                                                                             0x001a
#define mmSDMA0_POWER_CNTL_BASE_IDX                                                                    0
#define mmSDMA0_CLK_CTRL                                                                               0x001b
#define mmSDMA0_CLK_CTRL_BASE_IDX                                                                      0
#define mmSDMA0_CNTL                                                                                   0x001c
#define mmSDMA0_CNTL_BASE_IDX                                                                          0
#define mmSDMA0_CHICKEN_BITS                                                                           0x001d
#define mmSDMA0_CHICKEN_BITS_BASE_IDX                                                                  0
#define mmSDMA0_GB_ADDR_CONFIG                                                                         0x001e
#define mmSDMA0_GB_ADDR_CONFIG_BASE_IDX                                                                0
#define mmSDMA0_GB_ADDR_CONFIG_READ                                                                    0x001f
#define mmSDMA0_GB_ADDR_CONFIG_READ_BASE_IDX                                                           0
#define mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL                                                               0x0021
#define mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL_BASE_IDX                                                      0
#define mmSDMA0_STATUS_REG                                                                             0x0025
#define mmSDMA0_STATUS_REG_BASE_IDX                                                                    0
#define mmSDMA0_RD_BURST_CNTL                                                                          0x0027
#define mmSDMA0_RD_BURST_CNTL_BASE_IDX                                                                 0
#define mmSDMA0_F32_CNTL                                                                               0x002a
#define mmSDMA0_F32_CNTL_BASE_IDX                                                                      0
#define mmSDMA0_PHASE0_QUANTUM                                                                         0x002c
#define mmSDMA0_PHASE0_QUANTUM_BASE_IDX                                                                0
#define mmSDMA0_PHASE1_QUANTUM                                                                         0x002d
#define mmSDMA0_PHASE1_QUANTUM_BASE_IDX                                                                0
#define mmSDMA0_UTCL1_WR_STATUS                                                                        0x003f
#define mmSDMA0_UTCL1_WR_STATUS_BASE_IDX                                                               0
#define mmSDMA0_UTCL1_TIMEOUT                                                                          0x0047
#define mmSDMA0_UTCL1_TIMEOUT_BASE_IDX                                                                 0
#define mmSDMA0_UTCL1_PAGE                                                                             0x0048
#define mmSDMA0_UTCL1_PAGE_BASE_IDX                                                                    0
#define mmSDMA0_PHASE2_QUANTUM                                                                         0x004f
#define mmSDMA0_PHASE2_QUANTUM_BASE_IDX                                                                0
#define mmSDMA0_PUB_DUMMY_REG2                                                                         0x0053
#define mmSDMA0_PUB_DUMMY_REG2_BASE_IDX                                                                0
#define mmSDMA0_ULV_CNTL                                                                               0x005e
#define mmSDMA0_ULV_CNTL_BASE_IDX                                                                      0
#define mmSDMA0_GFX_RB_CNTL                                                                            0x0080
#define mmSDMA0_GFX_RB_CNTL_BASE_IDX                                                                   0
#define mmSDMA0_GFX_RB_BASE                                                                            0x0081
#define mmSDMA0_GFX_RB_BASE_BASE_IDX                                                                   0
#define mmSDMA0_GFX_RB_BASE_HI                                                                         0x0082
#define mmSDMA0_GFX_RB_BASE_HI_BASE_IDX                                                                0
#define mmSDMA0_GFX_RB_RPTR                                                                            0x0083
#define mmSDMA0_GFX_RB_RPTR_BASE_IDX                                                                   0
#define mmSDMA0_GFX_RB_RPTR_HI                                                                         0x0084
#define mmSDMA0_GFX_RB_RPTR_HI_BASE_IDX                                                                0
#define mmSDMA0_GFX_RB_WPTR                                                                            0x0085
#define mmSDMA0_GFX_RB_WPTR_BASE_IDX                                                                   0
#define mmSDMA0_GFX_RB_WPTR_HI                                                                         0x0086
#define mmSDMA0_GFX_RB_WPTR_HI_BASE_IDX                                                                0
#define mmSDMA0_GFX_RB_WPTR_POLL_CNTL                                                                  0x0087
#define mmSDMA0_GFX_RB_WPTR_POLL_CNTL_BASE_IDX                                                         0
#define mmSDMA0_GFX_RB_RPTR_ADDR_HI                                                                    0x0088
#define mmSDMA0_GFX_RB_RPTR_ADDR_HI_BASE_IDX                                                           0
#define mmSDMA0_GFX_RB_RPTR_ADDR_LO                                                                    0x0089
#define mmSDMA0_GFX_RB_RPTR_ADDR_LO_BASE_IDX                                                           0
#define mmSDMA0_GFX_IB_CNTL                                                                            0x008a
#define mmSDMA0_GFX_IB_CNTL_BASE_IDX                                                                   0
#define mmSDMA0_GFX_IB_RPTR                                                                            0x008b
#define mmSDMA0_GFX_IB_RPTR_BASE_IDX                                                                   0
#define mmSDMA0_GFX_IB_OFFSET                                                                          0x008c
#define mmSDMA0_GFX_IB_OFFSET_BASE_IDX                                                                 0
#define mmSDMA0_GFX_CONTEXT_STATUS                                                                     0x0091
#define mmSDMA0_GFX_CONTEXT_STATUS_BASE_IDX                                                            0
#define mmSDMA0_GFX_DOORBELL                                                                           0x0092
#define mmSDMA0_GFX_DOORBELL_BASE_IDX                                                                  0
#define mmSDMA0_GFX_DOORBELL_OFFSET                                                                    0x00ab
#define mmSDMA0_GFX_DOORBELL_OFFSET_BASE_IDX                                                           0
#define mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI                                                               0x00b2
#define mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI_BASE_IDX                                                      0
#define mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO                                                               0x00b3
#define mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO_BASE_IDX                                                      0
#define mmSDMA0_GFX_MINOR_PTR_UPDATE                                                                   0x00b5
#define mmSDMA0_GFX_MINOR_PTR_UPDATE_BASE_IDX                                                          0
#define mmSDMA0_PAGE_RB_CNTL                                                                           0x00e0
#define mmSDMA0_PAGE_RB_CNTL_BASE_IDX                                                                  0
#define mmSDMA0_PAGE_RB_WPTR_POLL_CNTL                                                                 0x00e7
#define mmSDMA0_PAGE_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_PAGE_RB_RPTR_ADDR_LO                                                                   0x00e9
#define mmSDMA0_PAGE_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_PAGE_IB_CNTL                                                                           0x00ea
#define mmSDMA0_PAGE_IB_CNTL_BASE_IDX                                                                  0
#define mmSDMA0_RLC0_RB_WPTR_POLL_CNTL                                                                 0x0147
#define mmSDMA0_RLC0_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC0_RB_RPTR_ADDR_LO                                                                   0x0149
#define mmSDMA0_RLC0_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC0_IB_CNTL                                                                           0x014a
#define mmSDMA0_RLC0_IB_CNTL_BASE_IDX                                                                  0
#define mmSDMA0_RLC1_RB_WPTR_POLL_CNTL                                                                 0x01a7
#define mmSDMA0_RLC1_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC1_RB_RPTR_ADDR_LO                                                                   0x01a9
#define mmSDMA0_RLC1_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC1_IB_CNTL                                                                           0x01aa
#define mmSDMA0_RLC1_IB_CNTL_BASE_IDX                                                                  0
#define mmSDMA0_RLC2_RB_WPTR_POLL_CNTL                                                                 0x0207
#define mmSDMA0_RLC2_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC2_RB_RPTR_ADDR_LO                                                                   0x0209
#define mmSDMA0_RLC2_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC3_RB_WPTR_POLL_CNTL                                                                 0x0267
#define mmSDMA0_RLC3_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC3_RB_RPTR_ADDR_LO                                                                   0x0269
#define mmSDMA0_RLC3_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC4_RB_WPTR_POLL_CNTL                                                                 0x02c7
#define mmSDMA0_RLC4_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC4_RB_RPTR_ADDR_LO                                                                   0x02c9
#define mmSDMA0_RLC4_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC5_RB_WPTR_POLL_CNTL                                                                 0x0327
#define mmSDMA0_RLC5_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC5_RB_RPTR_ADDR_LO                                                                   0x0329
#define mmSDMA0_RLC5_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC6_RB_WPTR_POLL_CNTL                                                                 0x0387
#define mmSDMA0_RLC6_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC6_RB_RPTR_ADDR_LO                                                                   0x0389
#define mmSDMA0_RLC6_RB_RPTR_ADDR_LO_BASE_IDX                                                          0
#define mmSDMA0_RLC7_RB_WPTR_POLL_CNTL                                                                 0x03e7
#define mmSDMA0_RLC7_RB_WPTR_POLL_CNTL_BASE_IDX                                                        0
#define mmSDMA0_RLC7_RB_RPTR_ADDR_LO                                                                   0x03e9
#define mmSDMA0_RLC7_RB_RPTR_ADDR_LO_BASE_IDX                                                          0

#endif
