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
 * drivers/gpu/drm/amd/include/asic_reg/sdma0/sdma0_4_2_sh_mask.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _sdma0_4_2_0_SH_MASK_HEADER
#define _sdma0_4_2_0_SH_MASK_HEADER

#define SDMA0_VIRT_RESET_REQ__VF__SHIFT                                                                       0x0
#define SDMA0_VIRT_RESET_REQ__PF__SHIFT                                                                       0x1f
#define SDMA0_VIRT_RESET_REQ__VF_MASK                                                                         0x0000FFFFL
#define SDMA0_VIRT_RESET_REQ__PF_MASK                                                                         0x80000000L
#define SDMA0_POWER_CNTL__MEM_POWER_OVERRIDE_MASK                                                             0x00000100L
#define SDMA0_CLK_CTRL__RESERVED__SHIFT                                                                       0xc
#define SDMA0_CLK_CTRL__RESERVED_MASK                                                                         0x00FFF000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE7_MASK                                                                   0x01000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE6_MASK                                                                   0x02000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE5_MASK                                                                   0x04000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE4_MASK                                                                   0x08000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE3_MASK                                                                   0x10000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE2_MASK                                                                   0x20000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE1_MASK                                                                   0x40000000L
#define SDMA0_CLK_CTRL__SOFT_OVERRIDE0_MASK                                                                   0x80000000L
#define SDMA0_CNTL__TRAP_ENABLE__SHIFT                                                                        0x0
#define SDMA0_CNTL__UTC_L1_ENABLE__SHIFT                                                                      0x1
#define SDMA0_CNTL__AUTO_CTXSW_ENABLE__SHIFT                                                                  0x12
#define SDMA0_CNTL__TRAP_ENABLE_MASK                                                                          0x00000001L
#define SDMA0_CNTL__UTC_L1_ENABLE_MASK                                                                        0x00000002L
#define SDMA0_CNTL__AUTO_CTXSW_ENABLE_MASK                                                                    0x00040000L
#define SDMA0_GB_ADDR_CONFIG__NUM_PIPES__SHIFT                                                                0x0
#define SDMA0_GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT                                                     0x3
#define SDMA0_GB_ADDR_CONFIG__NUM_BANKS__SHIFT                                                                0xc
#define SDMA0_GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT                                                       0x13
#define SDMA0_GB_ADDR_CONFIG__NUM_PIPES_MASK                                                                  0x00000007L
#define SDMA0_GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK                                                       0x00000038L
#define SDMA0_GB_ADDR_CONFIG__NUM_BANKS_MASK                                                                  0x00007000L
#define SDMA0_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK                                                         0x00180000L
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_PIPES__SHIFT                                                           0x0
#define SDMA0_GB_ADDR_CONFIG_READ__PIPE_INTERLEAVE_SIZE__SHIFT                                                0x3
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_BANKS__SHIFT                                                           0xc
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_SHADER_ENGINES__SHIFT                                                  0x13
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_PIPES_MASK                                                             0x00000007L
#define SDMA0_GB_ADDR_CONFIG_READ__PIPE_INTERLEAVE_SIZE_MASK                                                  0x00000038L
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_BANKS_MASK                                                             0x00007000L
#define SDMA0_GB_ADDR_CONFIG_READ__NUM_SHADER_ENGINES_MASK                                                    0x00180000L
#define SDMA0_STATUS_REG__IDLE_MASK                                                                           0x00000001L
#define SDMA0_F32_CNTL__HALT__SHIFT                                                                           0x0
#define SDMA0_F32_CNTL__HALT_MASK                                                                             0x00000001L
#define SDMA0_PHASE0_QUANTUM__UNIT__SHIFT                                                                     0x0
#define SDMA0_PHASE0_QUANTUM__VALUE__SHIFT                                                                    0x8
#define SDMA0_PHASE0_QUANTUM__UNIT_MASK                                                                       0x0000000FL
#define SDMA0_PHASE0_QUANTUM__VALUE_MASK                                                                      0x00FFFF00L
#define SDMA0_UTCL1_WR_STATUS__RPTR_DATA_FIFO_EMPTY__SHIFT                                                    0x1c
#define SDMA0_UTCL1_WR_STATUS__RPTR_DATA_FIFO_EMPTY_MASK                                                      0x10000000L
#define SDMA0_ULV_CNTL__HYSTERESIS__SHIFT                                                                     0x0
#define SDMA0_ULV_CNTL__HYSTERESIS_MASK                                                                       0x0000001FL
#define SDMA0_GFX_RB_CNTL__RB_ENABLE__SHIFT                                                                   0x0
#define SDMA0_GFX_RB_CNTL__RB_SIZE__SHIFT                                                                     0x1
#define SDMA0_GFX_RB_CNTL__RB_SWAP_ENABLE__SHIFT                                                              0x9
#define SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT                                                       0xc
#define SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_SWAP_ENABLE__SHIFT                                                  0xd
#define SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK                                                                     0x00000001L
#define SDMA0_GFX_RB_CNTL__RB_SIZE_MASK                                                                       0x0000003EL
#define SDMA0_GFX_RB_CNTL__RB_SWAP_ENABLE_MASK                                                                0x00000200L
#define SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK                                                         0x00001000L
#define SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_SWAP_ENABLE_MASK                                                    0x00002000L
#define SDMA0_GFX_RB_RPTR__OFFSET__SHIFT                                                                      0x0
#define SDMA0_GFX_RB_RPTR__OFFSET_MASK                                                                        0xFFFFFFFFL
#define SDMA0_GFX_RB_RPTR_HI__OFFSET__SHIFT                                                                   0x0
#define SDMA0_GFX_RB_RPTR_HI__OFFSET_MASK                                                                     0xFFFFFFFFL
#define SDMA0_GFX_RB_WPTR__OFFSET__SHIFT                                                                      0x0
#define SDMA0_GFX_RB_WPTR__OFFSET_MASK                                                                        0xFFFFFFFFL
#define SDMA0_GFX_RB_WPTR_HI__OFFSET__SHIFT                                                                   0x0
#define SDMA0_GFX_RB_WPTR_HI__OFFSET_MASK                                                                     0xFFFFFFFFL
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                            0x0
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                   0x2
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                   0x10
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                              0x00000001L
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                     0x00000004L
#define SDMA0_GFX_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                     0xFFFF0000L
#define SDMA0_GFX_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                        0x0
#define SDMA0_GFX_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                          0x00000001L
#define SDMA0_GFX_IB_CNTL__IB_ENABLE__SHIFT                                                                   0x0
#define SDMA0_GFX_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                              0x4
#define SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK                                                                     0x00000001L
#define SDMA0_GFX_IB_CNTL__IB_SWAP_ENABLE_MASK                                                                0x00000010L
#define SDMA0_GFX_IB_RPTR__OFFSET__SHIFT                                                                      0x2
#define SDMA0_GFX_IB_RPTR__OFFSET_MASK                                                                        0x003FFFFCL
#define SDMA0_GFX_IB_OFFSET__OFFSET__SHIFT                                                                    0x2
#define SDMA0_GFX_IB_OFFSET__OFFSET_MASK                                                                      0x003FFFFCL
#define SDMA0_GFX_DOORBELL__ENABLE__SHIFT                                                                     0x1c
#define SDMA0_GFX_DOORBELL__ENABLE_MASK                                                                       0x10000000L
#define SDMA0_GFX_DOORBELL_OFFSET__OFFSET__SHIFT                                                              0x2
#define SDMA0_GFX_DOORBELL_OFFSET__OFFSET_MASK                                                                0x0FFFFFFCL
#define SDMA0_GFX_MINOR_PTR_UPDATE__ENABLE__SHIFT                                                             0x0
#define SDMA0_GFX_MINOR_PTR_UPDATE__ENABLE_MASK                                                               0x00000001L
#define SDMA0_PAGE_RB_CNTL__RB_ENABLE__SHIFT                                                                  0x0
#define SDMA0_PAGE_RB_CNTL__RB_SIZE__SHIFT                                                                    0x1
#define SDMA0_PAGE_RB_CNTL__RB_SWAP_ENABLE__SHIFT                                                             0x9
#define SDMA0_PAGE_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT                                                      0xc
#define SDMA0_PAGE_RB_CNTL__RPTR_WRITEBACK_SWAP_ENABLE__SHIFT                                                 0xd
#define SDMA0_PAGE_RB_CNTL__RB_ENABLE_MASK                                                                    0x00000001L
#define SDMA0_PAGE_RB_CNTL__RB_SIZE_MASK                                                                      0x0000003EL
#define SDMA0_PAGE_RB_CNTL__RB_SWAP_ENABLE_MASK                                                               0x00000200L
#define SDMA0_PAGE_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK                                                        0x00001000L
#define SDMA0_PAGE_RB_CNTL__RPTR_WRITEBACK_SWAP_ENABLE_MASK                                                   0x00002000L
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_PAGE_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_PAGE_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_PAGE_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_PAGE_IB_CNTL__IB_ENABLE__SHIFT                                                                  0x0
#define SDMA0_PAGE_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                             0x4
#define SDMA0_PAGE_IB_CNTL__IB_ENABLE_MASK                                                                    0x00000001L
#define SDMA0_PAGE_IB_CNTL__IB_SWAP_ENABLE_MASK                                                               0x00000010L
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC0_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC0_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC0_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC0_IB_CNTL__IB_ENABLE__SHIFT                                                                  0x0
#define SDMA0_RLC0_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                             0x4
#define SDMA0_RLC0_IB_CNTL__IB_ENABLE_MASK                                                                    0x00000001L
#define SDMA0_RLC0_IB_CNTL__IB_SWAP_ENABLE_MASK                                                               0x00000010L
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC1_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC1_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC1_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC1_IB_CNTL__IB_ENABLE__SHIFT                                                                  0x0
#define SDMA0_RLC1_IB_CNTL__IB_SWAP_ENABLE__SHIFT                                                             0x4
#define SDMA0_RLC1_IB_CNTL__IB_ENABLE_MASK                                                                    0x00000001L
#define SDMA0_RLC1_IB_CNTL__IB_SWAP_ENABLE_MASK                                                               0x00000010L
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC2_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC2_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC2_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC3_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC3_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC3_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC4_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC4_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC4_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC5_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC5_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC5_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC6_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC6_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC6_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__ENABLE__SHIFT                                                           0x0
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE__SHIFT                                                  0x2
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT                                                  0x10
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__ENABLE_MASK                                                             0x00000001L
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__F32_POLL_ENABLE_MASK                                                    0x00000004L
#define SDMA0_RLC7_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK                                                    0xFFFF0000L
#define SDMA0_RLC7_RB_RPTR_ADDR_LO__RPTR_WB_IDLE__SHIFT                                                       0x0
#define SDMA0_RLC7_RB_RPTR_ADDR_LO__RPTR_WB_IDLE_MASK                                                         0x00000001L

#endif
