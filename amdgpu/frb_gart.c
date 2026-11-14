/*
 * amdgpu_gart.c / gmc_v8_0_gart_init / amdgpu_gmc_set_pte_pde: the GART
 * table for VMID 0, a single-level table of 64-bit entries in VRAM.
 *
 * BYTE ORDER: an entry is a little-endian 64-bit word (amdgpu writes it
 * with writeq). Through BAR0 it is stored as two little-endian 32-bit
 * words, the low one at the lower address.
 *
 * ADDRESSES: the GPU's DMA is not translated on this platform (the PAMU is
 * bypassed on the T4240), so an entry is a raw bus
 * address and a wrong one writes anywhere in RAM. System pages reach the
 * card through inbound ATMU window 2, PCI 0x10_0000_0000 -> CPU 0 over
 * 2^36 bytes (RadeonNI RadeonNIAccel.cpp gartBusOf, measured;
 * Linux on the same machine gave the card the same 0x10_xxxx_xxxx DMA
 * addresses: VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR 0x01169f3c in the
 * E9173 capture). frb_gart_bus_addr_ok refuses anything outside it.
 *
 * Copyright 2008 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
 * Copyright 2009 Jerome Glisse.
 * Copyright (c) 2026 Jordan Bettcher
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "frb_device.h"
#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/bif/bif_5_0_d.h"

#define FRB_PCI_INBOUND_BASE	0x1000000000ULL		/* ATMU inbound window 2 */
#define FRB_PCI_INBOUND_SIZE	(1ULL << 36)

int frb_gart_bus_addr_ok(struct frb_device *adev, uint64_t bus)
{
	(void)adev;
	return bus >= FRB_PCI_INBOUND_BASE &&
	       bus < FRB_PCI_INBOUND_BASE + FRB_PCI_INBOUND_SIZE &&
	       !(bus & (AMDGPU_GPU_PAGE_SIZE - 1)) && bus < (1ULL << 40);
}

/* amdgpu_gart_init + gmc_v8_0_gart_init + amdgpu_gart_table_vram_alloc */
int frb_gart_init(struct frb_device *adev)
{
	adev->gart.num_gpu_pages = (uint32_t)(adev->gmc.gart_size / AMDGPU_GPU_PAGE_SIZE);
	adev->gart.table_size = adev->gart.num_gpu_pages * 8;
	adev->gart.table_offset = adev->layout.gart_table;
	adev->gart.table_addr = adev->gmc.vram_start + adev->layout.gart_table;
	frb_info(adev, "[drm] GART: num gpu pages %u, table %u KB at 0x%016llX (BAR0 offset 0x%08x)",
		 adev->gart.num_gpu_pages, adev->gart.table_size >> 10,
		 (unsigned long long)adev->gart.table_addr, adev->gart.table_offset);
	adev->gart.ready = adev->gart.table_size <= (512u << 10);
	return adev->gart.ready ? 0 : -ERANGE;
}

/*
 * amdgpu_gmc_set_pte_pde: value = addr & 0x0000FFFFFFFFF000 | flags.
 * Unbound entries (amdgpu_gart_unbind) are the dummy page with flags 0.
 */
void frb_gart_set_pte(struct frb_device *adev, uint32_t page, uint64_t addr, uint64_t flags)
{
	uint64_t value;
	uint32_t off;

	if (page >= adev->gart.num_gpu_pages)
		return;
	if ((flags & 2 /* AMDGPU_PTE_SYSTEM */) || !flags) {
		/* a system page, or an unbound entry that names the dummy */
		if (!frb_gart_bus_addr_ok(adev, addr)) {
			frb_err(adev, "gart: refusing entry %u -> 0x%llx (not a bus address the card may use)",
				page, (unsigned long long)addr);
			return;
		}
	}
	value = (addr & 0x0000FFFFFFFFF000ULL) | flags;
	off = adev->gart.table_offset + page * 8;
	frb_vram_write32(adev, off, (uint32_t)value);
	frb_vram_write32(adev, off + 4, (uint32_t)(value >> 32));
}

/* --- GTT: binding system pages (amdgpu_gart_bind / _unbind / _invalidate_tlb) --- */

/*
 * The GART aperture's pages. Page 0 is never handed out, so a zero GPU
 * address offset never means a live mapping.
 */
int frb_gart_alloc(struct frb_device *adev, uint32_t npages, uint32_t *first)
{
	uint32_t n = adev->gart.num_gpu_pages, run = 0, i;

	if (!adev->gart.ready || !npages)
		return -EINVAL;
	if (!adev->gart.used) {
		adev->gart.used = (uint32_t *)adev->os->alloc(adev->os->cookie, (n + 31) / 32 * 4);
		if (!adev->gart.used)
			return -ENOMEM;
		adev->gart.used[0] |= 1;
	}
	for (i = 1; i < n; i++) {
		if (adev->gart.used[i / 32] & (1u << (i % 32))) {
			run = 0;
			continue;
		}
		if (++run == npages) {
			uint32_t p, start = i + 1 - npages;

			for (p = start; p <= i; p++)
				adev->gart.used[p / 32] |= 1u << (p % 32);
			*first = start;
			return 0;
		}
	}
	return -ENOMEM;
}

void frb_gart_free(struct frb_device *adev, uint32_t first, uint32_t npages)
{
	uint32_t p;

	if (!adev->gart.used)
		return;
	for (p = first; p < first + npages && p < adev->gart.num_gpu_pages; p++)
		adev->gart.used[p / 32] &= ~(1u << (p % 32));
}

/* amdgpu_gart_invalidate_tlb: the HDP flush (the table is in VRAM), then VMID 0's TLB */
void frb_gart_invalidate_tlb(struct frb_device *adev)
{
	WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
	/* gmc_v8_0_flush_gpu_tlb(adev, 0, 0, 0): bits 0-15 are the VM contexts */
	WREG32(mmVM_INVALIDATE_REQUEST, 1 << 0);
}

/* amdgpu_gart_bind (amdgpu_gart_map) + amdgpu_gart_invalidate_tlb; bus[] one address per page */
void frb_gart_bind(struct frb_device *adev, uint32_t first, uint32_t npages, const uint64_t *bus,
		   uint64_t flags)
{
	uint32_t i;

	for (i = 0; i < npages; i++)
		frb_gart_set_pte(adev, first + i, bus[i], flags);
	frb_gart_invalidate_tlb(adev);
}

/* amdgpu_gart_unbind: the dummy page, flags 0 */
void frb_gart_unbind(struct frb_device *adev, uint32_t first, uint32_t npages)
{
	uint32_t i;

	for (i = 0; i < npages; i++)
		frb_gart_set_pte(adev, first + i, adev->dummy_page_addr, 0);
	frb_gart_invalidate_tlb(adev);
}
