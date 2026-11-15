/*
 * Copyright (C) 2017  Advanced Micro Devices, Inc.
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
 * drivers/gpu/drm/amd/include/asic_reg/uvd/uvd_7_0_sh_mask.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _uvd_7_0_SH_MASK_HEADER
#define _uvd_7_0_SH_MASK_HEADER

#define UVD_POWER_STATUS__UVD_PG_MODE_MASK                                                                    0x00000004L
#define UVD_POWER_STATUS__UVD_PG_EN_MASK                                                                      0x00000100L
#define UVD_GPCOM_VCPU_CMD__CMD__SHIFT                                                                        0x1
#define UVD_GPCOM_VCPU_CMD__CMD_MASK                                                                          0x7FFFFFFEL
#define UVD_GPCOM_VCPU_DATA0__DATA0__SHIFT                                                                    0x0
#define UVD_GPCOM_VCPU_DATA0__DATA0_MASK                                                                      0xFFFFFFFFL
#define UVD_GPCOM_VCPU_DATA1__DATA1__SHIFT                                                                    0x0
#define UVD_GPCOM_VCPU_DATA1__DATA1_MASK                                                                      0xFFFFFFFFL
#define UVD_UDEC_ADDR_CONFIG__NUM_PIPES__SHIFT                                                                0x0
#define UVD_UDEC_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT                                                     0x3
#define UVD_UDEC_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT                                                     0x6
#define UVD_UDEC_ADDR_CONFIG__NUM_BANKS__SHIFT                                                                0xc
#define UVD_UDEC_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT                                                       0x13
#define UVD_UDEC_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT                                                            0x1a
#define UVD_UDEC_ADDR_CONFIG__ROW_SIZE__SHIFT                                                                 0x1c
#define UVD_UDEC_ADDR_CONFIG__NUM_PIPES_MASK                                                                  0x00000007L
#define UVD_UDEC_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK                                                       0x00000038L
#define UVD_UDEC_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK                                                       0x000000C0L
#define UVD_UDEC_ADDR_CONFIG__NUM_BANKS_MASK                                                                  0x00007000L
#define UVD_UDEC_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK                                                         0x00180000L
#define UVD_UDEC_ADDR_CONFIG__NUM_RB_PER_SE_MASK                                                              0x0C000000L
#define UVD_UDEC_ADDR_CONFIG__ROW_SIZE_MASK                                                                   0x30000000L
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_PIPES__SHIFT                                                             0x0
#define UVD_UDEC_DB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT                                                  0x3
#define UVD_UDEC_DB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT                                                  0x6
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_BANKS__SHIFT                                                             0xc
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT                                                    0x13
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT                                                         0x1a
#define UVD_UDEC_DB_ADDR_CONFIG__ROW_SIZE__SHIFT                                                              0x1c
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_PIPES_MASK                                                               0x00000007L
#define UVD_UDEC_DB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK                                                    0x00000038L
#define UVD_UDEC_DB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK                                                    0x000000C0L
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_BANKS_MASK                                                               0x00007000L
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK                                                      0x00180000L
#define UVD_UDEC_DB_ADDR_CONFIG__NUM_RB_PER_SE_MASK                                                           0x0C000000L
#define UVD_UDEC_DB_ADDR_CONFIG__ROW_SIZE_MASK                                                                0x30000000L
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_PIPES__SHIFT                                                            0x0
#define UVD_UDEC_DBW_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT                                                 0x3
#define UVD_UDEC_DBW_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT                                                 0x6
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_BANKS__SHIFT                                                            0xc
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT                                                   0x13
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT                                                        0x1a
#define UVD_UDEC_DBW_ADDR_CONFIG__ROW_SIZE__SHIFT                                                             0x1c
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_PIPES_MASK                                                              0x00000007L
#define UVD_UDEC_DBW_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK                                                   0x00000038L
#define UVD_UDEC_DBW_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK                                                   0x000000C0L
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_BANKS_MASK                                                              0x00007000L
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK                                                     0x00180000L
#define UVD_UDEC_DBW_ADDR_CONFIG__NUM_RB_PER_SE_MASK                                                          0x0C000000L
#define UVD_UDEC_DBW_ADDR_CONFIG__ROW_SIZE_MASK                                                               0x30000000L
#define UVD_SUVD_CGC_GATE__SRE_MASK                                                                           0x00000001L
#define UVD_SUVD_CGC_GATE__SIT_MASK                                                                           0x00000002L
#define UVD_SUVD_CGC_GATE__SMP_MASK                                                                           0x00000004L
#define UVD_SUVD_CGC_GATE__SCM_MASK                                                                           0x00000008L
#define UVD_SUVD_CGC_GATE__SDB_MASK                                                                           0x00000010L
#define UVD_SUVD_CGC_GATE__SRE_H264_MASK                                                                      0x00000020L
#define UVD_SUVD_CGC_GATE__SRE_HEVC_MASK                                                                      0x00000040L
#define UVD_SUVD_CGC_GATE__SIT_H264_MASK                                                                      0x00000080L
#define UVD_SUVD_CGC_GATE__SIT_HEVC_MASK                                                                      0x00000100L
#define UVD_SUVD_CGC_GATE__SCM_H264_MASK                                                                      0x00000200L
#define UVD_SUVD_CGC_GATE__SCM_HEVC_MASK                                                                      0x00000400L
#define UVD_SUVD_CGC_GATE__SDB_H264_MASK                                                                      0x00000800L
#define UVD_SUVD_CGC_GATE__SDB_HEVC_MASK                                                                      0x00001000L
#define UVD_SUVD_CGC_CTRL__SRE_MODE_MASK                                                                      0x00000001L
#define UVD_SUVD_CGC_CTRL__SIT_MODE_MASK                                                                      0x00000002L
#define UVD_SUVD_CGC_CTRL__SMP_MODE_MASK                                                                      0x00000004L
#define UVD_SUVD_CGC_CTRL__SCM_MODE_MASK                                                                      0x00000008L
#define UVD_SUVD_CGC_CTRL__SDB_MODE_MASK                                                                      0x00000010L
#define UVD_NO_OP__NO_OP__SHIFT                                                                               0x0
#define UVD_NO_OP__NO_OP_MASK                                                                                 0xFFFFFFFFL
#define UVD_GP_SCRATCH8__DATA__SHIFT                                                                          0x0
#define UVD_GP_SCRATCH8__DATA_MASK                                                                            0xFFFFFFFFL
#define UVD_CTX_DATA__DATA__SHIFT                                                                             0x0
#define UVD_CTX_DATA__DATA_MASK                                                                               0xFFFFFFFFL
#define UVD_CGC_GATE__RBC__SHIFT                                                                              0x4
#define UVD_CGC_GATE__VCPU__SHIFT                                                                             0x12
#define UVD_CGC_GATE__SYS_MASK                                                                                0x00000001L
#define UVD_CGC_GATE__UDEC_MASK                                                                               0x00000002L
#define UVD_CGC_GATE__MPEG2_MASK                                                                              0x00000004L
#define UVD_CGC_GATE__REGS_MASK                                                                               0x00000008L
#define UVD_CGC_GATE__RBC_MASK                                                                                0x00000010L
#define UVD_CGC_GATE__LMI_MC_MASK                                                                             0x00000020L
#define UVD_CGC_GATE__LMI_UMC_MASK                                                                            0x00000040L
#define UVD_CGC_GATE__IDCT_MASK                                                                               0x00000080L
#define UVD_CGC_GATE__MPRD_MASK                                                                               0x00000100L
#define UVD_CGC_GATE__MPC_MASK                                                                                0x00000200L
#define UVD_CGC_GATE__LBSI_MASK                                                                               0x00000400L
#define UVD_CGC_GATE__LRBBM_MASK                                                                              0x00000800L
#define UVD_CGC_GATE__UDEC_RE_MASK                                                                            0x00001000L
#define UVD_CGC_GATE__UDEC_CM_MASK                                                                            0x00002000L
#define UVD_CGC_GATE__UDEC_IT_MASK                                                                            0x00004000L
#define UVD_CGC_GATE__UDEC_DB_MASK                                                                            0x00008000L
#define UVD_CGC_GATE__UDEC_MP_MASK                                                                            0x00010000L
#define UVD_CGC_GATE__WCB_MASK                                                                                0x00020000L
#define UVD_CGC_GATE__VCPU_MASK                                                                               0x00040000L
#define UVD_CGC_GATE__SCPU_MASK                                                                               0x00080000L
#define UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT                                                               0x2
#define UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT                                                                    0x6
#define UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK                                                                     0x00000001L
#define UVD_CGC_CTRL__CLK_GATE_DLY_TIMER_MASK                                                                 0x0000003CL
#define UVD_CGC_CTRL__CLK_OFF_DELAY_MASK                                                                      0x000007C0L
#define UVD_CGC_CTRL__UDEC_RE_MODE_MASK                                                                       0x00000800L
#define UVD_CGC_CTRL__UDEC_CM_MODE_MASK                                                                       0x00001000L
#define UVD_CGC_CTRL__UDEC_IT_MODE_MASK                                                                       0x00002000L
#define UVD_CGC_CTRL__UDEC_DB_MODE_MASK                                                                       0x00004000L
#define UVD_CGC_CTRL__UDEC_MP_MODE_MASK                                                                       0x00008000L
#define UVD_CGC_CTRL__SYS_MODE_MASK                                                                           0x00010000L
#define UVD_CGC_CTRL__UDEC_MODE_MASK                                                                          0x00020000L
#define UVD_CGC_CTRL__MPEG2_MODE_MASK                                                                         0x00040000L
#define UVD_CGC_CTRL__REGS_MODE_MASK                                                                          0x00080000L
#define UVD_CGC_CTRL__RBC_MODE_MASK                                                                           0x00100000L
#define UVD_CGC_CTRL__LMI_MC_MODE_MASK                                                                        0x00200000L
#define UVD_CGC_CTRL__LMI_UMC_MODE_MASK                                                                       0x00400000L
#define UVD_CGC_CTRL__IDCT_MODE_MASK                                                                          0x00800000L
#define UVD_CGC_CTRL__MPRD_MODE_MASK                                                                          0x01000000L
#define UVD_CGC_CTRL__MPC_MODE_MASK                                                                           0x02000000L
#define UVD_CGC_CTRL__LBSI_MODE_MASK                                                                          0x04000000L
#define UVD_CGC_CTRL__LRBBM_MODE_MASK                                                                         0x08000000L
#define UVD_CGC_CTRL__WCB_MODE_MASK                                                                           0x10000000L
#define UVD_CGC_CTRL__VCPU_MODE_MASK                                                                          0x20000000L
#define UVD_CGC_CTRL__SCPU_MODE_MASK                                                                          0x40000000L
#define UVD_GP_SCRATCH4__DATA__SHIFT                                                                          0x0
#define UVD_GP_SCRATCH4__DATA_MASK                                                                            0xFFFFFFFFL
#define UVD_LMI_CTRL2__STALL_ARB_UMC__SHIFT                                                                   0x8
#define UVD_LMI_CTRL2__STALL_ARB_UMC_MASK                                                                     0x00000100L
#define UVD_MASTINT_EN__VCPU_EN__SHIFT                                                                        0x1
#define UVD_MASTINT_EN__SYS_EN__SHIFT                                                                         0x2
#define UVD_MASTINT_EN__VCPU_EN_MASK                                                                          0x00000002L
#define UVD_MASTINT_EN__SYS_EN_MASK                                                                           0x00000004L
#define UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT                                                                0x0
#define UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK                                                               0x00000100L
#define UVD_LMI_CTRL__REQ_MODE_MASK                                                                           0x00000200L
#define UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK                                                                  0x00002000L
#define UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK                                                             0x00200000L
#define UVD_VCPU_CNTL__CLK_EN_MASK                                                                            0x00000200L
#define UVD_SOFT_RESET__VCPU_SOFT_RESET__SHIFT                                                                0x3
#define UVD_SOFT_RESET__RBC_SOFT_RESET_MASK                                                                   0x00000001L
#define UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK                                                                  0x00000002L
#define UVD_SOFT_RESET__LMI_SOFT_RESET_MASK                                                                   0x00000004L
#define UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK                                                                  0x00000008L
#define UVD_SOFT_RESET__CSM_SOFT_RESET_MASK                                                                   0x00000020L
#define UVD_SOFT_RESET__CXW_SOFT_RESET_MASK                                                                   0x00000040L
#define UVD_SOFT_RESET__TAP_SOFT_RESET_MASK                                                                   0x00000080L
#define UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK                                                               0x00002000L
#define UVD_RBC_RB_CNTL__RB_BUFSZ__SHIFT                                                                      0x0
#define UVD_RBC_RB_CNTL__RB_BLKSZ__SHIFT                                                                      0x8
#define UVD_RBC_RB_CNTL__RB_NO_FETCH__SHIFT                                                                   0x10
#define UVD_RBC_RB_CNTL__RB_WPTR_POLL_EN__SHIFT                                                               0x14
#define UVD_RBC_RB_CNTL__RB_NO_UPDATE__SHIFT                                                                  0x18
#define UVD_RBC_RB_CNTL__RB_RPTR_WR_EN__SHIFT                                                                 0x1c
#define UVD_RBC_RB_CNTL__RB_BUFSZ_MASK                                                                        0x0000001FL
#define UVD_RBC_RB_CNTL__RB_BLKSZ_MASK                                                                        0x00001F00L
#define UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK                                                                     0x00010000L
#define UVD_RBC_RB_CNTL__RB_WPTR_POLL_EN_MASK                                                                 0x00100000L
#define UVD_RBC_RB_CNTL__RB_NO_UPDATE_MASK                                                                    0x01000000L
#define UVD_RBC_RB_CNTL__RB_RPTR_WR_EN_MASK                                                                   0x10000000L
#define UVD_STATUS__VCPU_REPORT__SHIFT                                                                        0x1

#endif
