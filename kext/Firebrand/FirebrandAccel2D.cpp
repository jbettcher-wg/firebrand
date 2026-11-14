/*
 * FirebrandAccel, phase G2 (docs/accel-interface.md, sections 4 and 7):
 * the window server's surface registry and the 2D work, on SDMA0.
 *
 * WHAT RUNS WHERE. RadeonNI drew its screen fills and copies with the 3D
 * engine (Evergreen) or CP DMA (GCN), on the one GFX ring, so its 32-bit
 * fence covered everything. Here a fill or copy is an SDMA0 IB (built by
 * amdgpu/frb_rect.c in GTT): CONSTANT_FILL rows, COPY_LINEAR_SUB_WINDOW
 * rectangles. The GFX ring's sequence stays the only one the clients see
 * (the 'rdn2' fences, the status page), joined to SDMA0 in both directions:
 *
 *   GFX -> SDMA   every 2D IB first waits, on the GPU, for the clients'
 *                 newest GFX submission (POLL_REGMEM >= on the GFX fence),
 *                 and for SDMA0's own previous IB (amdgpu's pipeline sync):
 *                 what GL drew, or a fill before a copy, is in memory.
 *   SDMA -> GFX   the fence a 2D call returns is the next GFX sequence,
 *                 promised; the frame that carries it (WAIT_REG_MEM >= on
 *                 SDMA0's fence, then the EOP fence: frb_ring_fence_after)
 *                 is emitted when someone waits for it (FENCE_WAIT) or
 *                 submits GFX work after it. A burst of fills costs one GFX
 *                 frame, not one each (a frame is 256 dwords of the 2048).
 *
 * MOVES. A region move whose destinations meet its sources goes through a
 * scratch surface the size of the screen, in two IBs (frb_rect.h): exact
 * in any order of rectangles, as RadeonNI's rdn_blit_move. Both IBs' slots
 * are taken before either is built, so no other move's scratch use comes
 * between them; the pipeline sync orders each move after the one before.
 *
 * The surface registry is RadeonNIAccel's (setSurfaceRegion ...
 * flushSurface), on the core's lock, with ownership: a surface's picture
 * must lie in a VRAM buffer of the client that names it, and is forgotten
 * when that buffer is freed or the client goes.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/clock.h>

/*
 * Where a 2D call's time goes (property Firebrand2DProfile): the CPU's part
 * of a submission, and the wait for an IB slot, which is SDMA0's pace.
 */
static UInt64 profSlotNs, profSubmitNs, profSubmits, profSlotWaits;
static inline UInt64 nowNs(void)
{
	uint64_t t = 0, ns = 0;

	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	return ns;
}

#include "FirebrandAccel.h"
#include "FirebrandSurface.h"

#define FRB_2D_WAIT_MS		2000

#define super FRB_ACCEL_BASE

/* What a coordinate or a size on a screen can be; sums of two cannot overflow (RadeonNI's screenRange) */
static bool screenRange(int32_t v)
{
	return v >= -32768 && v <= 32767;
}

/* --- SDMA0, its IB slots, the join ------------------------------------------ */

struct frb_ring *FirebrandAccel::sdmaLocked(void)
{
	return fCore && fIbArenaUp ? frb_sdma_v3_0_ring(fCore->adev(), 0) : 0;
}

/*
 * Has SDMA0 passed seq? (wrap-safe; true when there is no 2D path: nothing
 * runs there.) The fence is read only when what was seen before does not
 * answer: a read is an HDP invalidate and two PCIe round trips.
 */
bool FirebrandAccel::sdmaSignalledLocked(UInt32 seq)
{
	struct frb_ring *sdma = sdmaLocked();
	UInt32 now;

	if (!sdma)
		return true;
	if ((SInt32)(fSdmaCompleted - seq) >= 0)
		return true;
	now = frb_fence_read(sdma);
	if ((SInt32)(now - fSdmaCompleted) > 0 && (SInt32)(sdma->sync_seq - now) >= 0)
		fSdmaCompleted = now;
	return (SInt32)(fSdmaCompleted - seq) >= 0;
}

bool FirebrandAccel::waitSdma(UInt32 seq, UInt32 timeoutMs, bool draining)
{
	UInt32 waited = 0, spin;
	bool done = false, gone;

	for (spin = 0;; spin++) {
		fCore->lock();
		gone = fReleased || (fRetired && !draining);
		if (!gone)
			done = sdmaSignalledLocked(seq);
		fCore->unlock();
		if (gone || done)
			return done;
		if (spin < 100) {
			IODelay(10);
			continue;
		}
		if (waited >= timeoutMs)
			return false;
		IOSleep(1);
		waited++;
	}
}

/* room on the GFX ring for one more frame; lets go of the lock while it waits */
IOReturn FirebrandAccel::gfxRoomLocked(void)
{
	struct frb_ring *ring;
	UInt32 waited = 0, spin = 0;

	for (;;) {
		if (fRetired || !(ring = ringLocked()))
			return kIOReturnNoDevice;
		if ((UInt32)(ring->sync_seq - fCompleted) < kMaxInFlight)
			return kIOReturnSuccess;	/* known without reading the fence */
		completedLocked();
		if ((UInt32)(ring->sync_seq - fCompleted) < kMaxInFlight)
			return kIOReturnSuccess;
		fCore->unlock();
		if (spin < 200) {
			spin++;
			IODelay(10);
		} else if (waited++ < FRB_2D_WAIT_MS) {
			IOSleep(1);
		} else {
			reportHang("the GFX ring stays full", ring->sync_seq - kMaxInFlight + 1);
			fCore->lock();
			return kIOReturnTimeout;
		}
		fCore->lock();
	}
}

/*
 * Emit the promised GFX fence: WAIT_REG_MEM until SDMA0's fence reaches
 * fJoinSdma, then the fence. If frbctl's GFX tests took the promised
 * number meanwhile, the join gets the next one and fenceWait maps the
 * promise to it (fJoinPromised, fJoinEmitted).
 */
IOReturn FirebrandAccel::flushJoinLocked(void)
{
	struct frb_ring *ring, *sdma;
	UInt32 wait;
	uint32_t seq = 0;
	IOReturn r;

	if (!fJoinPending)
		return kIOReturnSuccess;
	r = gfxRoomLocked();
	if (r != kIOReturnSuccess || !fJoinPending)	/* another thread emitted it meanwhile */
		return r;
	ring = ringLocked();
	sdma = sdmaLocked();
	wait = fJoinSdma;
	if (!sdma || sdmaSignalledLocked(wait)) {
		wait = 0;			/* done already: >= 0 holds at once */
	} else if (fSdmaCompleted > wait) {
		/* SDMA0's sequence wrapped between them: the GPU's unsigned compare would pass at once */
		fCore->unlock();
		waitSdma(wait, FRB_2D_WAIT_MS, false);
		fCore->lock();
		if (!fJoinPending)
			return kIOReturnSuccess;
		if (fRetired || !(ring = ringLocked()))
			return kIOReturnNoDevice;
		wait = 0;
	}
	if (frb_ring_fence_after(ring, sdma ? sdma->fence_gpu_addr : ring->fence_gpu_addr, wait, &seq))
		return kIOReturnIOError;
	if (seq != fJoinSeq)
		frb_info(fCore->adev(), "accel: the promised 2D fence %u went to other GFX work; the join is %u",
			 (unsigned)fJoinSeq, (unsigned)seq);
	fJoinPromised = fJoinSeq;
	fJoinEmitted = seq;
	fJoinPending = false;
	fJoins++;
	updateStatusLocked();
	return kIOReturnSuccess;
}

/*
 * The software cursor (no hardware cursor: FirebrandHWCursor off, or a
 * picture the plane cannot show): IOFramebuffer, on every mouse move, and
 * the window server, when it lifts its shield, save the pixels under the
 * cursor and draw it with the CPU, without asking the plug-in to
 * synchronize first. A screen operation still queued on SDMA0 then lands
 * after the save-under: the cursor is overwritten and its stale save-under
 * comes back when it moves on (the window drag the user saw on the RX 550,
 * 2026-11-13). So while the cursor is the CPU's, a 2D call on the screen
 * returns only once SDMA0 has done it, and the HDP is invalidated for the
 * CPU's reads. With the hardware cursor the calls stay queued.
 */
void FirebrandAccel::softwareCursorSync(const struct frb_rect_surface *target)
{
	struct frb_ring *sdma;
	UInt32 seq = 0;

	if (target || !fFramebuffer || fCore->hwCursorActive())
		return;
	fCore->lock();
	sdma = fRetired ? 0 : sdmaLocked();
	if (sdma)
		seq = sdma->sync_seq;
	fCore->unlock();
	if (!sdma)
		return;
	if (!waitSdma(seq, FRB_2D_WAIT_MS, false))
		return;
	fCore->lock();
	if (!fRetired)
		frb_invalidate_hdp(fCore->adev());
	fSoftCursorSyncs++;
	fCore->unlock();
}

/* what a 2D call that drew nothing returns: everything queued so far */
UInt32 FirebrandAccel::currentFenceLocked(void)
{
	struct frb_ring *ring = ringLocked();

	return fJoinPending ? fJoinSeq : ring ? ring->sync_seq : fCompleted;
}

/*
 * `count` IB slots in a row, each once SDMA0 has passed its last use; lets
 * go of the lock while it waits, and takes them all before it returns (a
 * move's two IBs share the scratch: nothing else may come between them).
 */
IOReturn FirebrandAccel::slotLocked(UInt32 count, UInt32 *slots, struct frb_rect_ib *ibs)
{
	UInt32 waited = 0, spin = 0, i, s = 0;

	UInt64 t0 = nowNs();

	if (!count || count > 2)
		return kIOReturnBadArgument;
	for (;;) {
		bool idle = true;

		if (fRetired || !sdmaLocked())
			return kIOReturnNoDevice;
		for (i = 0; i < count && idle; i++) {
			s = (fSlotNext + i) % kIbSlots;
			idle = !fSlotUsed[s] || sdmaSignalledLocked(fSlotSeq[s]);
		}
		if (idle)
			break;
		profSlotWaits++;
		fCore->unlock();
		if (spin < 200) {
			spin++;
			IODelay(10);
		} else if (waited++ < FRB_2D_WAIT_MS) {
			IOSleep(1);
		} else {
			fCore->lock();
			frb_err(fCore->adev(), "accel: sdma0: IB slot %u still busy (fence %u, signalled %u, emitted %u)",
				(unsigned)s, (unsigned)fSlotSeq[s], (unsigned)fSdmaCompleted,
				sdmaLocked() ? (unsigned)sdmaLocked()->sync_seq : 0);
			return kIOReturnTimeout;
		}
		fCore->lock();
	}
	for (i = 0; i < count; i++) {
		s = (fSlotNext + i) % kIbSlots;
		fSlotUsed[s] = false;
		slots[i] = s;
		ibs[i].buf = (uint8_t *)fIbArena.cpu + s * kIbSlotBytes;
		ibs[i].len_dw = 0;
		ibs[i].cap_dw = kIbSlotBytes / 4 - 8;	/* sdma_v3_0_ring_pad_ib's room */
	}
	fSlotNext = (fSlotNext + count) % kIbSlots;
	profSlotNs += nowNs() - t0;
	return kIOReturnSuccess;
}

/*
 * Before taking slots: if the GFX sequence wrapped since the clients'
 * newest submission and it is still running, the GPU's unsigned compare
 * cannot wait for it; the CPU does. (Once in 2^32 submissions.)
 */
void FirebrandAccel::gfxWrapLocked(void)
{
	UInt32 g = fClientGfxSeq;

	if ((SInt32)(fCompleted - g) >= 0)
		return;			/* done, as last seen */
	completedLocked();
	if ((SInt32)(fCompleted - g) < 0 && fCompleted > g) {
		fCore->unlock();
		waitSeq(g, FRB_2D_WAIT_MS, false);
		fCore->lock();
	}
}

/* one 2D IB on SDMA0 (see the top of this file), and the promise of a GFX fence covering it */
IOReturn FirebrandAccel::sdmaSubmitLocked(UInt32 slot, struct frb_rect_ib *rib, UInt32 *seqOut)
{
	struct frb_ring *sdma = sdmaLocked(), *gfx = ringLocked();
	struct frb_ib_deps deps;
	struct frb_ib ib;
	uint32_t seq = 0;
	int r;

	UInt64 t0 = nowNs();

	if (fRetired || !sdma || !gfx)
		return kIOReturnNoDevice;
	if (!rib->len_dw)
		return kIOReturnSuccess;
	bzero(&ib, sizeof(ib));
	ib.cpu = (uint8_t *)fIbArena.cpu + slot * kIbSlotBytes;
	ib.gpu_addr = fIbArena.gpu_addr + (UInt64)slot * kIbSlotBytes;
	ib.size_dw = kIbSlotBytes / 4;
	ib.length_dw = rib->len_dw;
	frb_sdma_v3_0_pad_ib(sdma, &ib);
	bzero(&deps, sizeof(deps));
	deps.pipeline_sync = 1;
	deps.no_hdp_flush = 1;		/* frb_ring_commit's MMIO flush comes before the WPTR moves */
	if ((SInt32)(fCompleted - fClientGfxSeq) < 0)
		completedLocked();
	if ((SInt32)(fCompleted - fClientGfxSeq) < 0 && fCompleted <= fClientGfxSeq) {
		deps.wait_addr = gfx->fence_gpu_addr;
		deps.wait_seq = fClientGfxSeq;
	}
	/* the IB is in cached memory the card snoops: ordered before the WPTR write */
	__asm__ __volatile__("sync" : : : "memory");
	r = frb_ib_schedule_deps(sdma, &ib, &deps, &seq);
	if (r) {
		frb_err(fCore->adev(), "accel: sdma0: 2D IB of %u dwords not scheduled (%d)", (unsigned)ib.length_dw, r);
		return kIOReturnIOError;
	}
	fSlotSeq[slot] = seq;
	fSlotUsed[slot] = true;
	if (!fJoinPending) {
		fJoinPending = true;
		fJoinSeq = gfx->sync_seq + 1;
	}
	fJoinSdma = seq;
	if (seqOut)
		*seqOut = seq;
	profSubmits++;
	profSubmitNs += nowNs() - t0;
	return kIOReturnSuccess;
}

/* the screen, or a client's buffer it named (still its own: it may have freed it since) */
bool FirebrandAccel::screenLocked(struct frb_bo_table *bos, const struct frb_rect_surface *target,
				  struct frb_rect_surface *s)
{
	struct frb_device *adev = fCore->adev();

	if (target) {
		if (!bos || target->gpu_addr < adev->gmc.vram_start ||
		    !bufferOwned(bos, target->gpu_addr - adev->gmc.vram_start,
				 (UInt64)target->pitch * 4 * target->height))
			return false;
		*s = *target;
		return true;
	}
	{
		UInt32 off, pitch, w, h, bpp;

		/* the 2D path is 32 bpp; at 8 and 16 the GA plug-in draws with the CPU (RadeonNIGA.c) */
		if (!fFramebuffer || !fCore->screenLocked(&off, &pitch, &w, &h, &bpp) || bpp != 32)
			return false;
		s->gpu_addr = adev->gmc.vram_start + off;
		s->pitch = pitch;
		s->width = w;
		s->height = h;
	}
	return frb_rect_surface_ok(s);
}

bool FirebrandAccel::bufferOwned(struct frb_bo_table *bos, UInt64 offset, UInt64 bytes)
{
	return bos && bytes && frb_bo_table_contains(bos, offset, bytes);
}

/*
 * The moves' scratch surface: VRAM the size of the screen, beyond the CPU
 * window when the card has memory there (only the GPU touches it).
 * Allocated at the first move, again when the screen has grown; the old one
 * goes once the work queued before has retired.
 */
IOReturn FirebrandAccel::scratchLocked(const struct frb_rect_surface *s, struct frb_rect_surface *scratch)
{
	UInt64 need = (UInt64)s->pitch * 4 * s->height, off = 0, bytes = 0;

	if (fScratchBytes < need || fScratchPitch < s->pitch) {
		if (fScratchBytes)
			deferLocked(kPendingVram, fScratchOffset, fScratchBytes, 0, 0, 0);
		fScratchBytes = 0;
		if (frb_vram_mgr_alloc(&fVram, need, 4096, FRB_VRAM_INVISIBLE, &off, &bytes) &&
		    frb_vram_mgr_alloc(&fVram, need, 4096, FRB_VRAM_VISIBLE, &off, &bytes))
			return kIOReturnNoMemory;
		fScratchOffset = off;
		fScratchBytes = bytes;
		fScratchPitch = s->pitch;
		frb_info(fCore->adev(), "accel: region moves go through %lu KB of VRAM at 0x%llx",
			 (unsigned long)(bytes >> 10), (unsigned long long)off);
	}
	scratch->gpu_addr = fCore->adev()->gmc.vram_start + fScratchOffset;
	scratch->pitch = s->pitch;
	scratch->width = s->width;
	scratch->height = s->height;
	return kIOReturnSuccess;
}

/* --- the screen: SCREEN_FILL, SCREEN_COPY, SCREEN_COPY_REGION --------------- */

IOReturn FirebrandAccel::screenTarget(struct frb_bo_table *bos, UInt64 offset, UInt32 pitch, UInt32 width,
				      UInt32 height, struct frb_rect_surface *out)
{
	bool owned;

	if (!pitch || !width || !height || pitch < width || (offset & 3))
		return kIOReturnBadArgument;
	out->gpu_addr = vramAddr(offset);
	out->pitch = pitch;
	out->width = width;
	out->height = height;
	if (!frb_rect_surface_ok(out))
		return kIOReturnBadArgument;
	fCore->lock();
	owned = bufferOwned(bos, offset, (UInt64)pitch * 4 * height);
	fCore->unlock();
	return owned ? kIOReturnSuccess : kIOReturnBadArgument;
}

/*
 * RadeonNIAccel::screenFill on SDMA0: the colour's bytes as this machine's
 * programs store a pixel; rows in as many IBs as they need.
 */
IOReturn FirebrandAccel::screenFill(struct frb_bo_table *bos, const struct frb_rect_surface *target, int32_t x,
				    int32_t y, int32_t w, int32_t h, UInt32 colour, UInt32 *fence)
{
	struct frb_rect_surface s;
	struct frb_rect_ib ib;
	UInt32 slot = 0, done = 0;
	uint32_t rows = 0;
	IOReturn ret = kIOReturnSuccess;

	if (!screenRange(x) || !screenRange(y) || !screenRange(w) || !screenRange(h))
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired || !sdmaLocked()) {
		fCore->unlock();
		return fRetired ? kIOReturnNoDevice : kIOReturnUnsupported;
	}
	if (!screenLocked(bos, target, &s)) {
		fCore->unlock();
		return target ? kIOReturnBadArgument : kIOReturnUnsupported;
	}
	if (!frb_rect_clip(&s, &x, &y, &w, &h)) {
		/* nothing to draw: what is queued already is all there is to wait for */
		*fence = currentFenceLocked();
		fCore->unlock();
		return kIOReturnSuccess;
	}
	gfxWrapLocked();
	while (done < (UInt32)h) {
		ret = slotLocked(1, &slot, &ib);
		if (ret != kIOReturnSuccess)
			break;
		/* the lock may have gone meanwhile, and a client's buffer with it */
		if (target && !screenLocked(bos, target, &s)) {
			ret = kIOReturnBadArgument;
			break;
		}
		if (frb_rect_emit_fill(&ib, &s, x, y + done, w, h - done, colour, &rows) || !rows) {
			ret = kIOReturnInternalError;
			break;
		}
		ret = sdmaSubmitLocked(slot, &ib, 0);
		if (ret != kIOReturnSuccess)
			break;
		done += rows;
	}
	fFills++;
	*fence = currentFenceLocked();
	fCore->unlock();
	softwareCursorSync(target);
	return ret;
}

/*
 * Moves inside the screen (or the client's target): straight across when
 * no destination meets a source, else through the scratch in two IBs.
 */
IOReturn FirebrandAccel::movesLocked(struct frb_bo_table *bos, const struct frb_rect_surface *target,
				     const struct frb_rect_move *m, UInt32 n)
{
	struct frb_rect_surface s, scratch;
	struct frb_rect_ib ib[2];
	UInt32 slot[2], k;
	IOReturn ret;
	bool through = frb_rect_moves_overlap(m, n);

	gfxWrapLocked();
	ret = slotLocked(through ? 2 : 1, slot, ib);
	if (ret != kIOReturnSuccess)
		return ret;
	/* from here on the lock stays: the surface, the scratch and both IBs are this move's */
	if (!screenLocked(bos, target, &s))
		return target ? kIOReturnBadArgument : kIOReturnUnsupported;
	if (!through) {
		if (frb_rect_emit_moves(&ib[0], &s, 0, m, n, 0))
			return kIOReturnBadArgument;
		fCopies++;
		return sdmaSubmitLocked(slot[0], &ib[0], 0);
	}
	ret = scratchLocked(&s, &scratch);
	if (ret != kIOReturnSuccess)
		return ret;
	for (k = 0; k < 2; k++)
		if (frb_rect_emit_moves(&ib[k], &s, &scratch, m, n, (int)k + 1))
			return kIOReturnBadArgument;
	fScratchMoves++;
	ret = sdmaSubmitLocked(slot[0], &ib[0], 0);
	if (ret == kIOReturnSuccess)
		ret = sdmaSubmitLocked(slot[1], &ib[1], 0);
	return ret;
}

/* RadeonNIAccel::screenCopy: cut the destination and carry the source along, then the source */
IOReturn FirebrandAccel::screenCopy(struct frb_bo_table *bos, const struct frb_rect_surface *target, int32_t sx,
				    int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h, UInt32 *fence)
{
	struct frb_rect_surface s;
	struct frb_rect_move m;
	IOReturn ret;

	if (!screenRange(sx) || !screenRange(sy) || !screenRange(dx) || !screenRange(dy) || !screenRange(w) ||
	    !screenRange(h))
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired || !sdmaLocked()) {
		fCore->unlock();
		return fRetired ? kIOReturnNoDevice : kIOReturnUnsupported;
	}
	if (!screenLocked(bos, target, &s)) {
		fCore->unlock();
		return target ? kIOReturnBadArgument : kIOReturnUnsupported;
	}
	if (!frb_rect_clip_copy(&s, &sx, &sy, &dx, &dy, &w, &h) || (sx == dx && sy == dy)) {
		*fence = currentFenceLocked();
		fCore->unlock();
		return kIOReturnSuccess;
	}
	m.sx = sx;
	m.sy = sy;
	m.dx = dx;
	m.dy = dy;
	m.w = w;
	m.h = h;
	ret = movesLocked(bos, target, &m, 1);
	fMoves++;
	*fence = currentFenceLocked();
	fCore->unlock();
	softwareCursorSync(target);
	return ret;
}

/*
 * RadeonNIAccel::screenCopyRegion: each rectangle cut to the screen as
 * screenCopy cuts it, then all of them in one move, exact in any order
 * (the window server's order is not top to bottom: RadeonNI KNOWN-ISSUES 14).
 */
IOReturn FirebrandAccel::screenCopyRegion(struct frb_bo_table *bos, const struct frb_rect_surface *target,
					  const struct rdn_user_region_move *move, UInt32 count, UInt32 *fence)
{
	struct frb_rect_surface s;
	struct frb_rect_move *m;
	UInt32 i, n = 0;
	IOReturn ret = kIOReturnSuccess;

	if (!screenRange(move->dx) || !screenRange(move->dy) || !count || count > RDN_USER_REGION_MAX_RECTS)
		return kIOReturnBadArgument;
	m = (struct frb_rect_move *)IOMalloc(count * sizeof(*m));
	if (!m)
		return kIOReturnNoMemory;
	fCore->lock();
	if (fRetired || !sdmaLocked()) {
		ret = fRetired ? kIOReturnNoDevice : kIOReturnUnsupported;
		goto out;
	}
	if (!screenLocked(bos, target, &s)) {
		ret = target ? kIOReturnBadArgument : kIOReturnUnsupported;
		goto out;
	}
	for (i = 0; i < count; i++) {
		int32_t sx = move->rect[i].x, sy = move->rect[i].y, w = move->rect[i].w, h = move->rect[i].h;
		int32_t dx = sx + move->dx, dy = sy + move->dy;

		if (!screenRange(sx) || !screenRange(sy) || !screenRange(w) || !screenRange(h)) {
			ret = kIOReturnBadArgument;
			goto out;
		}
		if (!frb_rect_clip_copy(&s, &sx, &sy, &dx, &dy, &w, &h))
			continue;
		m[n].sx = sx;
		m[n].sy = sy;
		m[n].dx = dx;
		m[n].dy = dy;
		m[n].w = w;
		m[n].h = h;
		n++;
	}
	if (n && (move->dx || move->dy)) {
		ret = movesLocked(bos, target, m, n);
		fMoves++;
	}
out:
	*fence = currentFenceLocked();
	fCore->unlock();
	IOFree(m, count * sizeof(*m));
	if (n)
		softwareCursorSync(target);
	return ret;
}

/* --- copies the CPU waits for: the backing, SURFACE_INIT ---------------------- */

IOReturn FirebrandAccel::blitLinear(UInt64 dst, UInt32 dstPitchPx, UInt64 src, UInt32 srcPitchPx, UInt32 width,
				    UInt32 height)
{
	struct frb_rect_surface d, s;
	struct frb_rect_ib ib;
	UInt32 slot = 0, seq = 0;
	IOReturn ret;

	d.gpu_addr = dst;
	d.pitch = dstPitchPx;
	d.width = width;
	d.height = height;
	s.gpu_addr = src;
	s.pitch = srcPitchPx;
	s.width = width;
	s.height = height;
	if (!frb_rect_surface_ok(&d) || !frb_rect_surface_ok(&s))
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired || !sdmaLocked()) {
		fCore->unlock();
		return kIOReturnNotReady;
	}
	gfxWrapLocked();
	ret = slotLocked(1, &slot, &ib);
	if (ret == kIOReturnSuccess)
		ret = frb_rect_emit_copy(&ib, &d, 0, 0, &s, 0, 0, width, height) ? kIOReturnBadArgument :
			sdmaSubmitLocked(slot, &ib, &seq);
	fCore->unlock();
	if (ret != kIOReturnSuccess)
		return ret;
	/* the caller hands the destination straight on: arrived, not issued */
	return waitSdma(seq, FRB_2D_WAIT_MS, false) ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn FirebrandAccel::fillLinear(UInt64 dst, UInt32 dstPitchPx, UInt32 width, UInt32 height, UInt32 colour)
{
	struct frb_rect_surface d;
	struct frb_rect_ib ib;
	UInt32 slot = 0, seq = 0, done = 0;
	uint32_t rows = 0;
	IOReturn ret = kIOReturnSuccess;

	d.gpu_addr = dst;
	d.pitch = dstPitchPx;
	d.width = width;
	d.height = height;
	if (!frb_rect_surface_ok(&d))
		return kIOReturnBadArgument;
	fCore->lock();
	if (fRetired || !sdmaLocked()) {
		fCore->unlock();
		return kIOReturnNotReady;
	}
	gfxWrapLocked();
	while (done < height && ret == kIOReturnSuccess) {
		ret = slotLocked(1, &slot, &ib);
		if (ret == kIOReturnSuccess)
			ret = frb_rect_emit_fill(&ib, &d, 0, done, width, height - done, colour, &rows) || !rows ?
				kIOReturnInternalError : sdmaSubmitLocked(slot, &ib, &seq);
		done += rows;
	}
	fCore->unlock();
	if (ret != kIOReturnSuccess)
		return ret;
	return waitSdma(seq, FRB_2D_WAIT_MS, false) ? kIOReturnSuccess : kIOReturnTimeout;
}

/* RadeonNIAccel::waitForCPU, both engines: surface imports carry no producer fence of their own */
IOReturn FirebrandAccel::waitForCPU(void)
{
	struct frb_ring *ring, *sdma;
	UInt32 g, sd;

	fCore->lock();
	ring = ringLocked();
	sdma = sdmaLocked();
	if (fRetired || !ring) {
		fCore->unlock();
		return kIOReturnNotReady;
	}
	g = ring->sync_seq;
	sd = sdma ? sdma->sync_seq : 0;
	fCore->unlock();
	if (!waitSeq(g, FRB_2D_WAIT_MS, false) || !waitSdma(sd, FRB_2D_WAIT_MS, false))
		return kIOReturnTimeout;
	fCore->lock();
	if (!fRetired)
		frb_invalidate_hdp(fCore->adev());
	fCore->unlock();
	return kIOReturnSuccess;
}

UInt64 FirebrandAccel::vramAddr(UInt64 offset)
{
	return fCore->adev()->gmc.vram_start + offset;
}

UInt64 FirebrandAccel::gartAddr(UInt32 gartOffset)
{
	return fCore->adev()->gmc.gart_start + gartOffset;
}

/* --- the surface registry (RadeonNIAccel's) ------------------------------------- */

/* the lock held; existing records survive a failed expansion */
int FirebrandAccel::surfaceSlotLocked(UInt32 wid, bool create)
{
	SurfaceRecord *records;
	UInt32 i, capacity;
	int empty = -1;

	for (i = 0; i < fShapeCapacity; i++) {
		if (fShapes[i].used && fShapes[i].wid == wid)
			return (int)i;
		if (!fShapes[i].used && empty < 0)
			empty = (int)i;
	}
	if (!create || empty >= 0)
		return create ? empty : -1;
	capacity = fShapeCapacity ? fShapeCapacity * 2 : 32;
	if (capacity <= fShapeCapacity || capacity > 4096)
		return -1;
	records = (SurfaceRecord *)IOMalloc(capacity * sizeof(*records));
	if (!records)
		return -1;
	bzero(records, capacity * sizeof(*records));
	if (fShapes) {
		bcopy(fShapes, records, fShapeCapacity * sizeof(*records));
		IOFree(fShapes, fShapeCapacity * sizeof(*fShapes));
	}
	empty = (int)fShapeCapacity;
	fShapes = records;
	fShapeCapacity = capacity;
	return empty;
}

/*
 * Forget the surfaces an owner registered (owner != 0), or whose picture
 * lies in [offset, offset + size) (a freed buffer), as RadeonNI's
 * releaseClientMemory clears them: no flush reads freed memory.
 */
void FirebrandAccel::dropSurfaceBuffersLocked(void *owner, UInt64 offset, UInt64 size)
{
	UInt32 i;

	for (i = 0; i < fShapeCapacity; i++) {
		if (!fShapes[i].used)
			continue;
		if ((owner && fShapes[i].owner == owner) ||
		    (size && fShapes[i].bufWidth && fShapes[i].bufOffset >= offset &&
		     fShapes[i].bufOffset < offset + size))
			bzero(&fShapes[i], sizeof(fShapes[i]));
	}
}

void FirebrandAccel::querySurface(UInt32 wid, struct rdn_user_surface_info *info)
{
	int slot;

	bzero(info, sizeof(*info));
	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0) {
		info->id = wid;
		info->offset = fShapes[slot].bufOffset;
		info->row_bytes = fShapes[slot].bufRowBytes;
		info->width = fShapes[slot].bufWidth;
		info->height = fShapes[slot].bufHeight;
	}
	fCore->unlock();
}

bool FirebrandAccel::setSurfaceRegion(UInt32 wid, const IOAccelDeviceRegion *rgn, UInt32 rects)
{
	int slot;
	UInt32 r;

	fCore->lock();
	slot = surfaceSlotLocked(wid, true);
	if (slot >= 0) {
		struct rdn_user_region *region = &fShapes[slot].region;

		if (!fShapes[slot].used)
			bzero(&fShapes[slot], sizeof(fShapes[slot]));
		fShapes[slot].used = true;
		fShapes[slot].wid = wid;
		region->count = rects;
		region->bounds[0] = rgn->bounds.x;
		region->bounds[1] = rgn->bounds.y;
		region->bounds[2] = rgn->bounds.w;
		region->bounds[3] = rgn->bounds.h;
		for (r = 0; r < rects && r < RDN_USER_REGION_RECTS; r++) {
			region->rects[r][0] = rgn->rect[r].x;
			region->rects[r][1] = rgn->rect[r].y;
			region->rects[r][2] = rgn->rect[r].w;
			region->rects[r][3] = rgn->rect[r].h;
		}
	}
	fCore->unlock();
	return slot >= 0;
}

bool FirebrandAccel::getSurfaceRegion(UInt32 wid, struct rdn_user_region *region)
{
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0)
		*region = fShapes[slot].region;
	fCore->unlock();
	return slot >= 0;
}

/*
 * RDN_UC_SURFACE_BUFFER (RadeonNIAccel::setSurfaceBuffer and the user
 * client's checks): the picture inside one of the owner's buffers in the
 * CPU window (a read lock maps it for the window server), or width 0 (a
 * reservation: the owner announces the surface before it has a picture).
 */
IOReturn FirebrandAccel::setSurfaceBuffer(struct frb_bo_table *bos, task_t task, void *owner, UInt32 wid,
					  UInt32 offset, UInt32 rowBytes, UInt32 width, UInt32 height)
{
	UInt64 bytes = (UInt64)rowBytes * height;
	int slot;

	if (width && (!height || width > 16384 || height > 16384 || rowBytes < width * 4 || (rowBytes & 3) ||
		      (offset & 3) || (UInt64)offset + bytes > fApertureBytes))
		return kIOReturnBadArgument;
	fCore->lock();
	if (width && !bufferOwned(bos, offset, bytes)) {
		fCore->unlock();
		return kIOReturnBadArgument;
	}
	slot = surfaceSlotLocked(wid, true);
	if (slot >= 0) {
		/* the owner may be first: the shape then comes later */
		if (!fShapes[slot].used) {
			bzero(&fShapes[slot], sizeof(fShapes[slot]));
			fShapes[slot].used = true;
			fShapes[slot].wid = wid;
		}
		fShapes[slot].gl = true;
		if (!fShapes[slot].owner || width)
			fShapes[slot].owner = owner;
		fShapes[slot].bufOffset = offset;
		fShapes[slot].bufRowBytes = rowBytes;
		fShapes[slot].bufWidth = width;
		fShapes[slot].bufHeight = height;
	}
	fCore->unlock();
	if (slot < 0)
		return kIOReturnNoResources;
	if (width && !isBackingOwner(wid, task))
		markSurfaceGL(wid);
	return kIOReturnSuccess;
}

bool FirebrandAccel::getSurfaceBuffer(UInt32 wid, UInt32 *offset, UInt32 *rowBytes, UInt32 *width, UInt32 *height)
{
	bool found = false;
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0 && fShapes[slot].bufWidth) {
		*offset = fShapes[slot].bufOffset;
		*rowBytes = fShapes[slot].bufRowBytes;
		*width = fShapes[slot].bufWidth;
		*height = fShapes[slot].bufHeight;
		found = true;
	}
	fCore->unlock();
	return found;
}

bool FirebrandAccel::setSurfaceBacking(UInt32 wid, task_t task, const struct rdn_user_backing *backing)
{
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, true);
	if (slot >= 0) {
		if (!fShapes[slot].used)
			bzero(&fShapes[slot], sizeof(fShapes[slot]));
		if (fShapes[slot].backingTask != task || fShapes[slot].backing.address_lo != backing->address_lo ||
		    fShapes[slot].backing.row_bytes != backing->row_bytes ||
		    fShapes[slot].backing.width != backing->width || fShapes[slot].backing.height != backing->height)
			fShapes[slot].backingGartAddr = 0;
		fShapes[slot].used = true;
		fShapes[slot].wid = wid;
		fShapes[slot].backingTask = task;
		fShapes[slot].backing = *backing;
	}
	fCore->unlock();
	return slot >= 0;
}

void FirebrandAccel::setSurfaceBackingGart(UInt32 wid, task_t task, UInt64 gartAddr)
{
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0 && fShapes[slot].backingTask == task)
		fShapes[slot].backingGartAddr = gartAddr;
	fCore->unlock();
}

void FirebrandAccel::setSurfaceBackingClient(UInt32 wid, FirebrandSurfaceClient *client, FirebrandSurfaceClient *only)
{
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0 && (!only || fShapes[slot].backingClient == only))
		fShapes[slot].backingClient = client;
	fCore->unlock();
}

/*
 * RadeonNIAccel::acquireSurfaceForGPU: a program's GL is about to draw into
 * a window whose backing the CPU may have changed; the window server's
 * client copies it up first, under its backing lock. The client is taken
 * and retained under the lock; it clears itself from the record in
 * releaseBacking(), which runs while IOKit still holds it.
 */
IOReturn FirebrandAccel::acquireSurfaceForGPU(UInt32 wid, UInt32 *copied)
{
	FirebrandSurfaceClient *client = 0;
	IOReturn ret;
	bool up = false;
	int slot;

	*copied = 0;
	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0)
		client = fShapes[slot].backingClient;
	if (client)
		client->retain();
	fCore->unlock();
	if (!client)
		return kIOReturnSuccess;
	ret = client->acquireForGPU(&up);
	client->release();
	*copied = up;
	return ret;
}

bool FirebrandAccel::getSurfaceBacking(UInt32 wid, task_t task, struct rdn_user_backing *backing)
{
	bool found = false;
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	if (slot >= 0 && fShapes[slot].backingTask == task && fShapes[slot].backing.width) {
		*backing = fShapes[slot].backing;
		found = true;
	}
	fCore->unlock();
	return found;
}

bool FirebrandAccel::isGLSurface(UInt32 wid)
{
	bool found;
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	found = slot >= 0 && fShapes[slot].gl;
	fCore->unlock();
	return found;
}

bool FirebrandAccel::markSurfaceGL(UInt32 wid)
{
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, true);
	if (slot >= 0) {
		if (!fShapes[slot].used) {
			bzero(&fShapes[slot], sizeof(fShapes[slot]));
			fShapes[slot].used = true;
			fShapes[slot].wid = wid;
		}
		fShapes[slot].glProducer = true;
	}
	fCore->unlock();
	return slot >= 0;
}

bool FirebrandAccel::isGLProducer(UInt32 wid)
{
	bool found;
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	found = slot >= 0 && fShapes[slot].glProducer;
	fCore->unlock();
	return found;
}

bool FirebrandAccel::isBackingOwner(UInt32 wid, task_t task)
{
	bool match;
	int slot;

	fCore->lock();
	slot = surfaceSlotLocked(wid, false);
	match = slot >= 0 && fShapes[slot].backing.width && fShapes[slot].backingTask == task;
	fCore->unlock();
	return match;
}

void FirebrandAccel::listSurfaces(struct rdn_user_surfaces *list)
{
	UInt32 i;

	list->count = 0;
	fCore->lock();
	for (i = 0; i < fShapeCapacity && list->count < RDN_USER_SURFACES; i++) {
		if (!fShapes[i].used || !fShapes[i].bufWidth)
			continue;
		list->surface[list->count].id = fShapes[i].wid;
		list->surface[list->count].offset = fShapes[i].bufOffset;
		list->surface[list->count].row_bytes = fShapes[i].bufRowBytes;
		list->surface[list->count].width = fShapes[i].bufWidth;
		list->surface[list->count].height = fShapes[i].bufHeight;
		list->count++;
	}
	fCore->unlock();
}

void FirebrandAccel::forgetSurface(UInt32 wid)
{
	UInt32 i;

	fCore->lock();
	for (i = 0; i < fShapeCapacity; i++)
		if (fShapes[i].used && fShapes[i].wid == wid)
			bzero(&fShapes[i], sizeof(fShapes[i]));
	fCore->unlock();
}

/*
 * RadeonNIAccel::flushSurface: the surface's picture to the screen, inside
 * the shape the window server gave it (what other windows cover is not in
 * it); the shape's box has the surface's top left corner. Queued.
 */
IOReturn FirebrandAccel::flushSurface(UInt32 wid)
{
	struct frb_rect_surface screen, src;
	struct frb_rect_ib ib;
	struct rdn_user_region *region;
	UInt32 slot = 0, r, n = 0;
	IOReturn ret;
	int k;

	fCore->lock();
	if (fRetired || !sdmaLocked() || !screenLocked(0, 0, &screen)) {
		fCore->unlock();
		return kIOReturnSuccess;
	}
	gfxWrapLocked();
	ret = slotLocked(1, &slot, &ib);
	if (ret != kIOReturnSuccess) {
		fCore->unlock();
		return ret;
	}
	/* after the wait: the record and the screen as they are now */
	k = surfaceSlotLocked(wid, false);
	if (k < 0 || !fShapes[k].bufWidth || !screenLocked(0, 0, &screen)) {
		fCore->unlock();
		return kIOReturnSuccess;
	}
	src.gpu_addr = vramAddr(fShapes[k].bufOffset);
	src.pitch = fShapes[k].bufRowBytes / 4;
	src.width = fShapes[k].bufWidth;
	src.height = fShapes[k].bufHeight;
	region = &fShapes[k].region;
	for (r = 0; frb_rect_surface_ok(&src) && r < region->count && r < RDN_USER_REGION_RECTS; r++) {
		SInt32 bx = region->bounds[0], by = region->bounds[1];
		SInt32 x0 = region->rects[r][0], y0 = region->rects[r][1];
		SInt32 x1 = x0 + region->rects[r][2], y1 = y0 + region->rects[r][3];

		if (x0 < bx)
			x0 = bx;
		if (y0 < by)
			y0 = by;
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > bx + (SInt32)src.width)
			x1 = bx + (SInt32)src.width;
		if (y1 > by + (SInt32)src.height)
			y1 = by + (SInt32)src.height;
		if (x1 > (SInt32)screen.width)
			x1 = (SInt32)screen.width;
		if (y1 > (SInt32)screen.height)
			y1 = (SInt32)screen.height;
		if (x1 <= x0 || y1 <= y0)
			continue;
		if (frb_rect_emit_copy(&ib, &screen, x0, y0, &src, x0 - bx, y0 - by, x1 - x0, y1 - y0))
			break;
		n++;
	}
	if (n) {
		ret = sdmaSubmitLocked(slot, &ib, 0);
		fFlushes++;
	}
	fCore->unlock();
	if (n)
		softwareCursorSync(0);
	return ret;
}

/*
 * RDN_UC_SURFACE_INIT (RadeonNIAccel::initSurfaceBuffer): a GL store's
 * first picture, from the old store on a resize (KEEP_OLD: the overlap
 * copied, the rest cleared) or from the window server's backing when its
 * size matches and it is in the GART, else cleared. Both stores must be
 * the caller's buffers.
 */
IOReturn FirebrandAccel::initSurfaceBuffer(struct frb_bo_table *bos, task_t task, void *owner,
					   const struct rdn_user_surface_init *in, UInt32 *synced)
{
	bool keepOld = (in->flags & RDN_USER_SURFACE_KEEP_OLD) != 0, owned;
	UInt64 bgAddr = 0;
	UInt32 bgPitchPx = 0, i;
	IOReturn r;

	*synced = 0;
	if (!in->id || !in->width || !in->height || in->width > 16384 || in->height > 16384 || (in->offset & 3) ||
	    (in->row_bytes & 3) || in->row_bytes < in->width * 4 ||
	    (UInt64)in->offset + (UInt64)in->row_bytes * in->height > fApertureBytes)
		return kIOReturnBadArgument;
	if (keepOld && (!in->old_width || !in->old_height || in->old_width > 16384 || in->old_height > 16384 ||
			(in->old_offset & 3) || (in->old_row_bytes & 3) || in->old_row_bytes < in->old_width * 4 ||
			(UInt64)in->old_offset + (UInt64)in->old_row_bytes * in->old_height > fApertureBytes))
		return kIOReturnBadArgument;
	fCore->lock();
	owned = bufferOwned(bos, in->offset, (UInt64)in->row_bytes * in->height) &&
		(!keepOld || bufferOwned(bos, in->old_offset, (UInt64)in->old_row_bytes * in->old_height));
	if (owned && !keepOld) {
		for (i = 0; i < fShapeCapacity; i++) {
			const SurfaceRecord *s = &fShapes[i];

			if (s->used && s->wid == in->id && s->backingTask == task && s->backing.width == in->width &&
			    s->backing.height == in->height && !(s->backing.address_hi & RDN_USER_BACKING_STALE) &&
			    s->backingGartAddr && !(s->backing.row_bytes & 3) &&
			    s->backing.row_bytes >= in->width * 4) {
				bgAddr = s->backingGartAddr;
				bgPitchPx = s->backing.row_bytes / 4;
				break;
			}
		}
	}
	fCore->unlock();
	if (!owned)
		return kIOReturnBadArgument;
	if (keepOld) {
		UInt32 cw = in->old_width < in->width ? in->old_width : in->width;
		UInt32 ch = in->old_height < in->height ? in->old_height : in->height;

		if (in->old_width < in->width || in->old_height < in->height) {
			r = fillLinear(vramAddr(in->offset), in->row_bytes / 4, in->width, in->height, 0);
			if (r != kIOReturnSuccess)
				return r;
		}
		r = blitLinear(vramAddr(in->offset), in->row_bytes / 4, vramAddr(in->old_offset), in->old_row_bytes / 4,
			       cw, ch);
		if (r != kIOReturnSuccess)
			return r;
	} else if (bgAddr && blitLinear(vramAddr(in->offset), in->row_bytes / 4, bgAddr, bgPitchPx, in->width,
					in->height) == kIOReturnSuccess) {
		*synced = 1;
	} else {
		r = fillLinear(vramAddr(in->offset), in->row_bytes / 4, in->width, in->height, 0);
		if (r != kIOReturnSuccess)
			return r;
	}
	return setSurfaceBuffer(bos, task, owner, in->id, in->offset, in->row_bytes, in->width, in->height);
}

/*
 * RadeonNIAccel::setProperties: {ScreenSurface: bool} through
 * IORegistryEntrySetCFProperties; the window server reads it when it next
 * creates its display surface.
 */
IOReturn FirebrandAccel::setProperties(OSObject *properties)
{
	OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
	OSObject *o;

	if (!dict)
		return kIOReturnBadArgument;
	o = dict->getObject("ScreenSurface");
	if (!o)
		return kIOReturnUnsupported;
	fScreenSurface = (o == kOSBooleanTrue);
	setProperty("ScreenSurface", fScreenSurface);
	IOLog("FirebrandAccel: the display surface is %s\n", fScreenSurface ? "let through" : "refused");
	return kIOReturnSuccess;
}

/* the 2D counts, as of when they are read (ioreg -c FirebrandAccel -l) */
bool FirebrandAccel::serializeProperties(OSSerialize *s) const
{
	FirebrandAccel *self = (FirebrandAccel *)this;
	char text[256];

	if (fCore) {
		snprintf(text, sizeof(text), "fills %llu, moves %llu (straight %llu, through the scratch %llu), "
			 "surface flushes %llu, joins %llu; waited for the software cursor %llu, cursor %s",
			 (unsigned long long)fFills, (unsigned long long)fMoves,
			 (unsigned long long)fCopies, (unsigned long long)fScratchMoves, (unsigned long long)fFlushes,
			 (unsigned long long)fJoins, (unsigned long long)fSoftCursorSyncs,
			 fCore->hwCursorActive() ? "hardware" : "software");
		self->setProperty("Firebrand2DStats", text);
		snprintf(text, sizeof(text), "submits %llu: %llu ns each in submit, slot %llu ns each, slot waits %llu",
			 profSubmits, profSubmits ? profSubmitNs / profSubmits : 0, profSubmits ? profSlotNs / profSubmits : 0,
			 profSlotWaits);
		self->setProperty("Firebrand2DProfile", text);
	}
	return super::serializeProperties(s);
}
