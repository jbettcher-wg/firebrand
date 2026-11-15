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
 * drivers/gpu/drm/amd/include/asic_reg/mmhub/mmhub_1_0_sh_mask.h (7.1.1): only the
 * definitions Firebrand's amdgpu/ sources name. Regenerate, do not edit.
 */
#ifndef _mmhub_1_0_SH_MASK_HEADER
#define _mmhub_1_0_SH_MASK_HEADER

#define DAGB0_CNTL_MISC2__DISABLE_WRREQ_CG_MASK                                                               0x00000004L
#define DAGB0_CNTL_MISC2__DISABLE_WRRET_CG_MASK                                                               0x00000008L
#define DAGB0_CNTL_MISC2__DISABLE_RDREQ_CG_MASK                                                               0x00000010L
#define DAGB0_CNTL_MISC2__DISABLE_RDRET_CG_MASK                                                               0x00000020L
#define DAGB0_CNTL_MISC2__DISABLE_TLBWR_CG_MASK                                                               0x00000040L
#define DAGB0_CNTL_MISC2__DISABLE_TLBRD_CG_MASK                                                               0x00000080L
#define DAGB1_CNTL_MISC2__DISABLE_WRREQ_CG_MASK                                                               0x00000004L
#define DAGB1_CNTL_MISC2__DISABLE_WRRET_CG_MASK                                                               0x00000008L
#define DAGB1_CNTL_MISC2__DISABLE_RDREQ_CG_MASK                                                               0x00000010L
#define DAGB1_CNTL_MISC2__DISABLE_RDRET_CG_MASK                                                               0x00000020L
#define DAGB1_CNTL_MISC2__DISABLE_TLBWR_CG_MASK                                                               0x00000040L
#define DAGB1_CNTL_MISC2__DISABLE_TLBRD_CG_MASK                                                               0x00000080L
#define ATC_L2_MISC_CG__ENABLE__SHIFT                                                                         0x12
#define ATC_L2_MISC_CG__ENABLE_MASK                                                                           0x00040000L
#define ATC_L2_MISC_CG__MEM_LS_ENABLE_MASK                                                                    0x00080000L
#define VM_L2_CNTL__ENABLE_L2_CACHE__SHIFT                                                                    0x0
#define VM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING__SHIFT                                                      0x1
#define VM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE__SHIFT                                                  0x8
#define VM_L2_CNTL__ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE__SHIFT                                            0x9
#define VM_L2_CNTL__ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE__SHIFT                                           0xa
#define VM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY__SHIFT                                           0xb
#define VM_L2_CNTL__EFFECTIVE_L2_QUEUE_SIZE__SHIFT                                                            0xf
#define VM_L2_CNTL__PDE_FAULT_CLASSIFICATION__SHIFT                                                           0x12
#define VM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT                                                      0x13
#define VM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE__SHIFT                                                        0x15
#define VM_L2_CNTL__ENABLE_L2_CACHE_MASK                                                                      0x00000001L
#define VM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK                                                        0x00000002L
#define VM_L2_CNTL__L2_PDE0_CACHE_TAG_GENERATION_MODE_MASK                                                    0x00000100L
#define VM_L2_CNTL__ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE_MASK                                              0x00000200L
#define VM_L2_CNTL__ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE_MASK                                             0x00000400L
#define VM_L2_CNTL__ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY_MASK                                             0x00000800L
#define VM_L2_CNTL__EFFECTIVE_L2_QUEUE_SIZE_MASK                                                              0x00038000L
#define VM_L2_CNTL__PDE_FAULT_CLASSIFICATION_MASK                                                             0x00040000L
#define VM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE_MASK                                                        0x00180000L
#define VM_L2_CNTL__IDENTITY_MODE_FRAGMENT_SIZE_MASK                                                          0x03E00000L
#define VM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS__SHIFT                                                            0x0
#define VM_L2_CNTL2__INVALIDATE_L2_CACHE__SHIFT                                                               0x1
#define VM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK                                                              0x00000001L
#define VM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK                                                                 0x00000002L
#define VM_L2_CNTL3__BANK_SELECT__SHIFT                                                                       0x0
#define VM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE__SHIFT                                                       0xf
#define VM_L2_CNTL3__L2_CACHE_BIGK_ASSOCIATIVITY__SHIFT                                                       0x14
#define VM_L2_CNTL3__BANK_SELECT_MASK                                                                         0x0000003FL
#define VM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE_MASK                                                         0x000F8000L
#define VM_L2_CNTL3__L2_CACHE_BIGK_ASSOCIATIVITY_MASK                                                         0x00100000L
#define VM_L2_PROTECTION_FAULT_CNTL__CLEAR_PROTECTION_FAULT_STATUS_ADDR__SHIFT                                0x0
#define VM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                             0x2
#define VM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                              0x3
#define VM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                              0x4
#define VM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                              0x5
#define VM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                 0x6
#define VM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                              0x7
#define VM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                        0x8
#define VM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                             0x9
#define VM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                              0xa
#define VM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                             0xb
#define VM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                           0xc
#define VM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT__SHIFT                                           0x1e
#define VM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT__SHIFT                                              0x1f
#define VM_L2_PROTECTION_FAULT_CNTL__CLEAR_PROTECTION_FAULT_STATUS_ADDR_MASK                                  0x00000001L
#define VM_L2_PROTECTION_FAULT_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                               0x00000004L
#define VM_L2_PROTECTION_FAULT_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                0x00000008L
#define VM_L2_PROTECTION_FAULT_CNTL__PDE1_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                0x00000010L
#define VM_L2_PROTECTION_FAULT_CNTL__PDE2_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                0x00000020L
#define VM_L2_PROTECTION_FAULT_CNTL__TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                   0x00000040L
#define VM_L2_PROTECTION_FAULT_CNTL__NACK_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                0x00000080L
#define VM_L2_PROTECTION_FAULT_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                          0x00000100L
#define VM_L2_PROTECTION_FAULT_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                               0x00000200L
#define VM_L2_PROTECTION_FAULT_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                0x00000400L
#define VM_L2_PROTECTION_FAULT_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                               0x00000800L
#define VM_L2_PROTECTION_FAULT_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                             0x00001000L
#define VM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT_MASK                                             0x40000000L
#define VM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT_MASK                                                0x80000000L
#define VM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY__SHIFT                             0x12
#define VM_L2_PROTECTION_FAULT_CNTL2__ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY_MASK                               0x00040000L
#define VM_L2_PROTECTION_FAULT_STATUS__VMID__SHIFT                                                            0x14
#define VM_L2_PROTECTION_FAULT_STATUS__VF__SHIFT                                                              0x18
#define VM_L2_PROTECTION_FAULT_STATUS__VMID_MASK                                                              0x00F00000L
#define VM_L2_PROTECTION_FAULT_STATUS__VF_MASK                                                                0x01000000L
#define VM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL__SHIFT                                                      0x6
#define VM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL__SHIFT                                                      0x7
#define VM_L2_CNTL4__VMC_TAP_PDE_REQUEST_PHYSICAL_MASK                                                        0x00000040L
#define VM_L2_CNTL4__VMC_TAP_PTE_REQUEST_PHYSICAL_MASK                                                        0x00000080L
#define VM_CONTEXT0_CNTL__ENABLE_CONTEXT__SHIFT                                                               0x0
#define VM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH__SHIFT                                                             0x1
#define VM_CONTEXT0_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT                                                        0x3
#define VM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT                                       0x7
#define VM_CONTEXT0_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0xa
#define VM_CONTEXT0_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                   0xc
#define VM_CONTEXT0_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                         0xe
#define VM_CONTEXT0_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0x10
#define VM_CONTEXT0_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                         0x12
#define VM_CONTEXT0_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0x14
#define VM_CONTEXT0_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                      0x16
#define VM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK                                                                 0x00000001L
#define VM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH_MASK                                                               0x00000006L
#define VM_CONTEXT0_CNTL__PAGE_TABLE_BLOCK_SIZE_MASK                                                          0x00000078L
#define VM_CONTEXT0_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK                                         0x00000080L
#define VM_CONTEXT0_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00000400L
#define VM_CONTEXT0_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                     0x00001000L
#define VM_CONTEXT0_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                           0x00004000L
#define VM_CONTEXT0_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00010000L
#define VM_CONTEXT0_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                           0x00040000L
#define VM_CONTEXT0_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00100000L
#define VM_CONTEXT0_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                        0x00400000L
#define VM_CONTEXT1_CNTL__ENABLE_CONTEXT__SHIFT                                                               0x0
#define VM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT                                                             0x1
#define VM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT                                                        0x3
#define VM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT                                       0x7
#define VM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0xa
#define VM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                   0xc
#define VM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                         0xe
#define VM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0x10
#define VM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                         0x12
#define VM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                        0x14
#define VM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT                                      0x16
#define VM_CONTEXT1_CNTL__ENABLE_CONTEXT_MASK                                                                 0x00000001L
#define VM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK                                                               0x00000006L
#define VM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE_MASK                                                          0x00000078L
#define VM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK                                         0x00000080L
#define VM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00000400L
#define VM_CONTEXT1_CNTL__DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                     0x00001000L
#define VM_CONTEXT1_CNTL__PDE0_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                           0x00004000L
#define VM_CONTEXT1_CNTL__VALID_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00010000L
#define VM_CONTEXT1_CNTL__READ_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                           0x00040000L
#define VM_CONTEXT1_CNTL__WRITE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                          0x00100000L
#define VM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK                                        0x00400000L
#define VM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ__SHIFT                                                0x0
#define VM_INVALIDATE_ENG0_REQ__FLUSH_TYPE__SHIFT                                                             0x10
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES__SHIFT                                                     0x12
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0__SHIFT                                                     0x13
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1__SHIFT                                                     0x14
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2__SHIFT                                                     0x15
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES__SHIFT                                                     0x16
#define VM_INVALIDATE_ENG0_REQ__CLEAR_PROTECTION_FAULT_STATUS_ADDR__SHIFT                                     0x17
#define VM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ_MASK                                                  0x0000FFFFL
#define VM_INVALIDATE_ENG0_REQ__FLUSH_TYPE_MASK                                                               0x00030000L
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PTES_MASK                                                       0x00040000L
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE0_MASK                                                       0x00080000L
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE1_MASK                                                       0x00100000L
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L2_PDE2_MASK                                                       0x00200000L
#define VM_INVALIDATE_ENG0_REQ__INVALIDATE_L1_PTES_MASK                                                       0x00400000L
#define VM_INVALIDATE_ENG0_REQ__CLEAR_PROTECTION_FAULT_STATUS_ADDR_MASK                                       0x00800000L
#define VM_INVALIDATE_ENG1_REQ__PER_VMID_INVALIDATE_REQ__SHIFT                                                0x0
#define VM_INVALIDATE_ENG1_REQ__FLUSH_TYPE__SHIFT                                                             0x10
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PTES__SHIFT                                                     0x12
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE0__SHIFT                                                     0x13
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE1__SHIFT                                                     0x14
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE2__SHIFT                                                     0x15
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L1_PTES__SHIFT                                                     0x16
#define VM_INVALIDATE_ENG1_REQ__CLEAR_PROTECTION_FAULT_STATUS_ADDR__SHIFT                                     0x17
#define VM_INVALIDATE_ENG1_REQ__PER_VMID_INVALIDATE_REQ_MASK                                                  0x0000FFFFL
#define VM_INVALIDATE_ENG1_REQ__FLUSH_TYPE_MASK                                                               0x00030000L
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PTES_MASK                                                       0x00040000L
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE0_MASK                                                       0x00080000L
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE1_MASK                                                       0x00100000L
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L2_PDE2_MASK                                                       0x00200000L
#define VM_INVALIDATE_ENG1_REQ__INVALIDATE_L1_PTES_MASK                                                       0x00400000L
#define VM_INVALIDATE_ENG1_REQ__CLEAR_PROTECTION_FAULT_STATUS_ADDR_MASK                                       0x00800000L
#define MC_VM_FB_LOCATION_BASE__FB_BASE_MASK                                                                  0x00FFFFFFL
#define MC_VM_FB_LOCATION_TOP__FB_TOP_MASK                                                                    0x00FFFFFFL
#define MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB__SHIFT                                                            0x0
#define MC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE__SHIFT                                                       0x3
#define MC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS__SHIFT                                          0x5
#define MC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL__SHIFT                                             0x6
#define MC_VM_MX_L1_TLB_CNTL__MTYPE__SHIFT                                                                    0xb
#define MC_VM_MX_L1_TLB_CNTL__ATC_EN__SHIFT                                                                   0xd
#define MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK                                                              0x00000001L
#define MC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK                                                         0x00000018L
#define MC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS_MASK                                            0x00000020L
#define MC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK                                               0x00000040L
#define MC_VM_MX_L1_TLB_CNTL__MTYPE_MASK                                                                      0x00001800L
#define MC_VM_MX_L1_TLB_CNTL__ATC_EN_MASK                                                                     0x00002000L
#define MC_VM_XGMI_LFB_CNTL__PF_MAX_REGION__SHIFT                                                             0x4
#define MC_VM_XGMI_LFB_CNTL__PF_MAX_REGION_MASK                                                               0x00000070L

#endif
