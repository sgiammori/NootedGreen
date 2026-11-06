//  Copyright © 2026 Stezza @ inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE for
//  details.
#include "kern_gen11.hpp"
#include "i915_lrc_reg.hpp"
#include "drm_dp.hpp"
#include "intel_gpu_commands.hpp"
#include "AppleIntelParams.hpp"
#include <Headers/kern_api.hpp>
#include "kern_genx.hpp"
#include "kern_green.hpp"
#include "IntelDPLinkTraining.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOLib.h>
#include <i386/proc_reg.h>
#include <kern/thread_call.h>

// ==== 6 kextInfos: ICL fallback + dual TGL identities (com.xxxxx and com.apple) from /Library/Extensions ====
//trivial
// ICL FB — com.apple (fallback path)
static const char *pathsICLFB[] = {
    "/System/Library/Extensions/AppleIntelICLLPGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelICLLPGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FB {"com.apple.driver.AppleIntelICLLPGraphicsFramebuffer", pathsICLFB, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// ICL HW — com.apple (fallback path)
static const char *pathsICLHW[] = {
    "/System/Library/Extensions/AppleIntelICLGraphics.kext/Contents/MacOS/AppleIntelICLGraphics",
};
static KernelPatcher::KextInfo kextG11HW {"com.apple.driver.AppleIntelICLGraphics", pathsICLHW, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLFB[] = {
    "/Library/Extensions/AppleIntelTGLGraphicsFramebuffer.kext/Contents/MacOS/AppleIntelTGLGraphicsFramebuffer",
};
static KernelPatcher::KextInfo kextG11FBT {"com.xxxxx.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
    {true, false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL FB — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11FBTA {"com.apple.driver.AppleIntelTGLGraphicsFramebuffer", pathsTGLFB, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.xxxxx (loaded from /Library/Extensions/)
static const char *pathsTGLHW[] = {
    "/Library/Extensions/AppleIntelTGLGraphics.kext/Contents/MacOS/AppleIntelTGLGraphics",
};
static KernelPatcher::KextInfo kextG11HWT {"com.xxxxx.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
    {true, false, false, false, true}, {},
    KernelPatcher::KextInfo::Unloaded};

// TGL HW — com.apple (loaded from /Library/Extensions/)
static KernelPatcher::KextInfo kextG11HWTA {"com.apple.driver.AppleIntelTGLGraphics", pathsTGLHW, 1,
	{true, false, false, false, true}, {},
	KernelPatcher::KextInfo::Unloaded};

// IOAcceleratorFamily2 symbols are resolved inside NGreen::processKext via
// kextIOAcceleratorFamily2 (kern_green.cpp) which already has the valid path.
// No separate kextIOAF2 registration needed here.

Gen11 *Gen11::callback = nullptr;

// From i915, drivers/gpu/drm/i915/gt/intel_lrc_reg.h: the pointer fields of RING_CONTEXT_STATUS_PTR. Moving the read pointer onto the
// write pointer, as a masked write (the form reset_csb_pointers writes the register in), consumes every pending status entry.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_lrc_reg.h#L69-L72
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_execlists_submission.c#L2797-L2837
static uint32_t csbConsumeWritePtr(uint32_t value) {
	const uint32_t write = value & GEN11_CSB_WRITE_PTR_MASK;
	return _MASKED_FIELD(GEN11_CSB_READ_PTR_MASK, write << 8);
}

static volatile UInt32 gIrqWatchdogStop = 1;
static volatile UInt32 gIrqWatchdogActive = 0;
static volatile UInt32 gIrqWatchdogPasses = 0;

static volatile UInt32 gEmrEnforcerArmed = 0;
static int gEmrCount = 0;

static constexpr size_t   kCsOffSlot0     = 0x30;
static constexpr size_t   kCsOffCur       = 0x50;
static constexpr size_t   kCsOffHwsp      = 0x68;
static constexpr size_t   kCsOffDesc      = 0x28;
static constexpr size_t   kCsOffSwRead    = 0x1c04;
static constexpr size_t   kCsOffGate      = 0x1c3c;

static inline bool isKernelPointer(uint64_t p) {
	return p >= 0xffffff8000000000ull && p < 0xffffffff00000000ull;
}

static constexpr uint64_t kScratchSizeOriginal = 0xd240;
static constexpr uint64_t kScratchSizeRounded = 0xe000;

static void roundScratchSize(void *params) {
	auto *sz = reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(params) + 0x10);
	const uint64_t was = *sz;
	if (was == kScratchSizeOriginal) {
		if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
			*sz = kScratchSizeRounded;
			MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
		}
	}
}

// From i915, drivers/gpu/drm/i915/gt/intel_workarounds.c: the render-engine rows of rcs_engine_wa_init that apply to ADL-S
// (Wa_22010271021 FF_DOP_CLOCK_GATE_DISABLE, Wa_1606931601 GEN12_DISABLE_EARLY_READ, Wa_14010919138 GEN12_FF_TESSELATION_DOP_GATE_DISABLE,
// Wa_1406941453 ENABLE_SMALLPL, Wa_1409804808 GEN12_PUSH_CONST_DEREF_HOLD_DIS, Wa_14010229206 GEN12_DISABLE_TDL_PUSH), the tuning row of
// add_render_compute_tuning_settings (GEN12_BUS_HASH_CTL_BIT_EXC cleared in GEN8_GARBCNTL) and icl_wa_init_mcr's steering (reads steered
// to the lowest subslice that is not fused off, __set_mcr_steering). i915 keeps them in a list its reset path applies; here they are
// written by MMIO at the engine probe and before a stop, a masked row as REG_MASKED_FIELD_ENABLE. The GEN8_CS_CHICKEN1 preemption level
// is a context workaround in i915 (gen12_ctx_workarounds_init: the context-image table above), so it is not written here any more; the
// GEN11_INDIRECT_STATE_BASE_ADDR_OVERRIDE row of general_render_compute_wa_init is a contract with Mesa's sampler states and is not
// applied under Apple's Metal driver. The multicast bit of GEN8_MCR_SELECTOR is forced on around the MCR rows: i915 owns the selector and
// relies on its default, Apple's driver leaves it in either state. The registers: gt/intel_gt_regs.h.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L1271-L1332
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L2279-L2320
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L2724-L2743
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gt_regs.h#L72-L82
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gt_regs.h#L629
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gt_regs.h#L941-L943
static constexpr uint32_t GEN11_MCR_MULTICAST     = 1u << 31;
#define GEN11_MCR_SLICE(slice)		(((slice) & 0xf) << 27)
#define GEN11_MCR_SLICE_MASK			GEN11_MCR_SLICE(0xf)
#define GEN11_MCR_SUBSLICE(subslice)		(((subslice) & 0x7) << 24)
#define GEN11_MCR_SUBSLICE_MASK		GEN11_MCR_SUBSLICE(0x7)
static constexpr uint32_t GEN12_GT_GEOMETRY_DSS_ENABLE = 0x913cu;
static constexpr uint32_t GEN8_GARBCNTL                = 0xb004u;
static constexpr uint32_t GEN12_BUS_HASH_CTL_BIT_EXC   = 1u << 7;

static void __set_mcr_steering(NGreen *cb, uint32_t steering_reg, uint32_t slice, uint32_t subslice) {
	const uint32_t mcr = GEN11_MCR_SLICE(slice) | GEN11_MCR_SUBSLICE(subslice);
	const uint32_t mcr_mask = GEN11_MCR_SLICE_MASK | GEN11_MCR_SUBSLICE_MASK;
	cb->writeReg32(steering_reg, (cb->readReg32(steering_reg) & ~mcr_mask) | mcr);
}

static void icl_wa_init_mcr(NGreen *cb) {
	/*
	 * Although a platform may have subslices, we need to always steer
	 * reads to the lowest instance that isn't fused off.  When Render
	 * Power Gating is enabled, grabbing forcewake will only power up a
	 * single subslice (the "minconfig") if there isn't a real workload
	 * that needs to be run; this means that if we steer register reads to
	 * one of the higher subslices, we run the risk of reading back 0's or
	 * random garbage.
	 */
	const uint32_t dss = cb->readReg32(GEN12_GT_GEOMETRY_DSS_ENABLE);
	const uint32_t subslice = (dss != 0 && dss != 0xffffffffu) ? static_cast<uint32_t>(__builtin_ctz(dss)) : 0u;
	__set_mcr_steering(cb, GEN8_MCR_SELECTOR, 0, subslice);
}

static void add_render_compute_tuning_settings(NGreen *cb) {
	const uint32_t garb = cb->readReg32(GEN8_GARBCNTL);
	if (garb != 0 && garb != 0xffffffffu) cb->writeReg32(GEN8_GARBCNTL, garb & ~GEN12_BUS_HASH_CTL_BIT_EXC);
}

// The rows are written in two passes, the MCR rows under the multicast selector: mcr_rows picks the pass.
static void rcs_engine_wa_init(NGreen *cb, bool mcr_rows) {
	if (!mcr_rows) {
		/*
		 * Wa_1606700617:tgl,dg1,adl-p
		 * Wa_22010271021:tgl,rkl,dg1,adl-s,adl-p
		 * Wa_14010826681:tgl,dg1,rkl,adl-p
		 * Wa_18019627453:dg2
		 */
		cb->writeReg32(GEN9_CS_DEBUG_MODE1, _MASKED_BIT_ENABLE(FF_DOP_CLOCK_GATE_DISABLE));
		/*
		 * Wa_1407928979:tgl A*
		 * Wa_18011464164:tgl[B0+],dg1[B0+]
		 * Wa_22010931296:tgl[B0+],dg1[B0+]
		 * Wa_14010919138:rkl,dg1,adl-s,adl-p
		 */
		const uint32_t ffThr = cb->readReg32(GEN7_FF_THREAD_MODE);
		if (ffThr != 0xffffffffu) cb->writeReg32(GEN7_FF_THREAD_MODE, ffThr | GEN12_FF_TESSELATION_DOP_GATE_DISABLE);
		return;
	}
	/* Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p and Wa_1409804808 */
	cb->writeReg32(GEN8_ROW_CHICKEN2, _MASKED_BIT_ENABLE(GEN12_DISABLE_EARLY_READ | GEN12_PUSH_CONST_DEREF_HOLD_DIS));
	/* Wa_14010229206 */
	cb->writeReg32(GEN9_ROW_CHICKEN4, _MASKED_BIT_ENABLE(GEN12_DISABLE_TDL_PUSH));
	/* Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p */
	cb->writeReg32(GEN10_SAMPLER_MODE, _MASKED_BIT_ENABLE(ENABLE_SMALLPL));
}

static void applyRenderWa(NGreen *cb, bool steer) {
	if (cb == nullptr) return;
	const uint32_t mcr = cb->readReg32(GEN8_MCR_SELECTOR);
	const uint32_t ffThr = cb->readReg32(GEN7_FF_THREAD_MODE);
	const uint32_t garb = cb->readReg32(GEN8_GARBCNTL);
	const bool awake = ffThr != 0 && ffThr != 0xffffffffu && mcr != 0xffffffffu && garb != 0xffffffffu;
	if (steer && awake) icl_wa_init_mcr(cb);
	rcs_engine_wa_init(cb, false);
	if (awake) add_render_compute_tuning_settings(cb);
	const uint32_t selNow = cb->readReg32(GEN8_MCR_SELECTOR);
	const bool mcOn = awake && selNow != 0xffffffffu && (selNow & GEN11_MCR_MULTICAST) != 0;
	if (mcOn || (steer && awake && selNow != 0xffffffffu)) {
		if (!mcOn) cb->writeReg32(GEN8_MCR_SELECTOR, selNow | GEN11_MCR_MULTICAST);
		rcs_engine_wa_init(cb, true);
		if (!mcOn) cb->writeReg32(GEN8_MCR_SELECTOR, selNow);
	}
	static volatile uint32_t rows = 0;
	if (__sync_add_and_fetch(&rows, 1u) <= 4u)
		SYSLOG("ngreen", "render workarounds: awake %u steer %u selector 0x%x -> 0x%x (multicast %u); CS_DEBUG_MODE1 0x%x FF_THREAD_MODE 0x%x GARBCNTL 0x%x ROW_CHICKEN2 0x%x ROW_CHICKEN4 0x%x SAMPLER_MODE 0x%x",
		       awake ? 1u : 0u, steer ? 1u : 0u, mcr, cb->readReg32(GEN8_MCR_SELECTOR), mcOn ? 1u : 0u, cb->readReg32(GEN9_CS_DEBUG_MODE1),
		       cb->readReg32(GEN7_FF_THREAD_MODE), cb->readReg32(GEN8_GARBCNTL), cb->readReg32(GEN8_ROW_CHICKEN2), cb->readReg32(GEN9_ROW_CHICKEN4),
		       cb->readReg32(GEN10_SAMPLER_MODE));
}

static constexpr uint64_t kScratchSizeApple = 0xd240;

static void copyScratchUngated(void *mappedBuf, mach_vm_address_t origScratchInit, mach_vm_address_t origGetMemory) {
	if (!isKernelPointer(reinterpret_cast<uint64_t>(mappedBuf))) return;
	const uint64_t size = getMember<uint64_t>(mappedBuf, 0x20);
	const uint64_t hold = getMember<uint64_t>(mappedBuf, 0x30);
	const uint64_t memH = isKernelPointer(hold) ? getMember<uint64_t>(reinterpret_cast<void *>(hold), 0x18) : 0;
	uint64_t mem = 0;
	if (origGetMemory != 0) mem = reinterpret_cast<uint64_t>(reinterpret_cast<void *(*)(void *)>(origGetMemory)(mappedBuf));
	if (!isKernelPointer(mem) && isKernelPointer(memH)) mem = memH;
	if (size < kScratchSizeApple || size > 0x20000 || (size & 0xfffu) != 0) return;
	if (!isKernelPointer(mem) || origScratchInit == 0) return;
	reinterpret_cast<void (*)(void *)>(origScratchInit)(reinterpret_cast<void *>(mem));
}

static volatile uint32_t gBurstLeft = 0;
static uint32_t gBurstPrevExec   = 0xffffffffu;
static uint32_t gBurstPrevHead   = 0xffffffffu;

static constexpr size_t kElBase     = 0x30;
static constexpr size_t kCurIdxOff  = 0x50;
static constexpr size_t kCtxIdOff   = 0x7c;
static constexpr size_t kCsDescOff  = 0x28;
static constexpr size_t kDescSqOff  = 0x19;
static constexpr size_t kDescElcOff = 0x1d;

static uint32_t streamerSq(void *cs) {
	if (cs == nullptr) return 0;
	auto *desc = *reinterpret_cast<uint8_t *volatile *>(static_cast<volatile uint8_t *>(cs) + kCsDescOff);
	if (desc == nullptr) return 0;
	const uint32_t sq = *reinterpret_cast<volatile const uint32_t *>(desc + kDescSqOff);
	const uint32_t el = *reinterpret_cast<volatile const uint32_t *>(desc + kDescElcOff);
	if ((sq & 0xfffu) != 0x510u || sq >= 0x400000u || el != sq + 0x40u) return 0;
	return sq;
}

static constexpr size_t   kCtxLrcBufOff   = 0x98;
static constexpr size_t   kSharedBufVaOff = 0x38;
static constexpr uint32_t kPoshSlot        = 0x35 + 1;

// The state page of a render context opens with the LRI pairs i915 lays out in gen12_rcs_offsets (gt/intel_lrc.c): the register
// of each pair names RING_CONTEXT_CONTROL, RING_TAIL and RING_START of the render engine (gt/intel_engine_regs.h), which is the
// check applied before an image is touched.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_engine_regs.h#L12-L19
static volatile uint32_t *lrcImageOf(void *ctx) {
	if (ctx == nullptr) return nullptr;
	auto *buf = *reinterpret_cast<void *volatile *>(
	    static_cast<volatile uint8_t *>(ctx) + kCtxLrcBufOff);
	if (buf == nullptr) return nullptr;
	const uint64_t va = *reinterpret_cast<volatile const uint64_t *>(
	    static_cast<volatile uint8_t *>(buf) + kSharedBufVaOff);
	if ((va >> 47) != 0x1ffffull || (va & 0xfffull) != 0) return nullptr;
	auto *img = reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(va) + LRC_STATE_OFFSET);
	if (img[CTX_CONTEXT_CONTROL - 1] != RING_CONTEXT_CONTROL(RENDER_RING_BASE) || img[CTX_RING_TAIL - 1] != RING_TAIL(RENDER_RING_BASE) || img[CTX_RING_START - 1] != RING_START(RENDER_RING_BASE))
		return nullptr;
	return img;
}

static constexpr uint32_t kL3AllocReg  = 0xb134u;
static constexpr uint32_t kAppleL3     = 0x88000020u;
static constexpr uint32_t kMesaL3      = 0xB0000040u;
static constexpr uint32_t kL3ScanDw      = 64;
static constexpr uint32_t kL3RingDw      = 512;
static constexpr uint64_t kL3AddrMask    = 0x0000FFFFFFFFF000ull;
static uint64_t *volatile gPhysmapBase = nullptr;
static uint64_t *volatile gPhysmapMax  = nullptr;

static uint32_t uptimeMs();

extern volatile uint64_t gStolenLo, gStolenHi;
extern volatile uint32_t gStolenState;
static volatile uint32_t *physToVirt(uint64_t pa, uint32_t bytes) {
	uint64_t *pb = gPhysmapBase, *pm = gPhysmapMax;
	if (pb == nullptr || pm == nullptr) return nullptr;
	const uint64_t base = *pb, max = *pm;
	if (base == 0 || max <= base || pa < 0x1000ull || pa >= (1ull << 46)) return nullptr;
	if ((pa & 0xfffull) + bytes > 0x1000ull) return nullptr;
	if (gStolenState == 1 && pa < gStolenHi && pa + bytes > gStolenLo) {
		return nullptr;
	}
	const uint64_t va = base + pa;
	if (va < base || va + bytes > max) return nullptr;
	return reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(va));
}
static inline void flushLines(volatile const void *p, uint32_t bytes) {
	uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~static_cast<uintptr_t>(63);
	const uintptr_t end = reinterpret_cast<uintptr_t>(p) + bytes;
	for (; a < end; a += 64) asm volatile("clflush (%0)" :: "r"(a) : "memory");
	asm volatile("mfence" ::: "memory");
}
static bool readPhys32(uint64_t pa, uint32_t &out) {
	volatile uint32_t *p = physToVirt(pa, 4);
	if (p == nullptr) return false;
	flushLines(p, 4);
	out = *p;
	return true;
}
static uint64_t pageWalk(uint64_t pml4, uint64_t va) {
	uint64_t tbl = pml4 & kL3AddrMask;
	if (tbl == 0) return 0;
	const uint32_t idx[4] = {
	    static_cast<uint32_t>((va >> 39) & 0x1ff), static_cast<uint32_t>((va >> 30) & 0x1ff),
	    static_cast<uint32_t>((va >> 21) & 0x1ff), static_cast<uint32_t>((va >> 12) & 0x1ff)};
	for (int lvl = 0; lvl < 4; lvl++) {
		uint32_t lo = 0, hi = 0;
		const uint64_t ea = tbl + static_cast<uint64_t>(idx[lvl]) * 8ull;
		if (!readPhys32(ea, lo) || !readPhys32(ea + 4, hi)) return 0;
		const uint64_t ent = (static_cast<uint64_t>(hi) << 32) | lo;
		if ((ent & 1ull) == 0) return 0;
		if (lvl == 3) return (ent & kL3AddrMask) | (va & 0xfffull);
		if (lvl >= 1 && (ent & (1ull << 7))) {
			if (lvl == 1) return (ent & 0x0000FFFFC0000000ull) | (va & 0x3FFFFFFFull);
			return (ent & 0x0000FFFFFFE00000ull) | (va & 0x1FFFFFull);
		}
		tbl = ent & kL3AddrMask;
	}
	return 0;
}
// From i915, drivers/gpu/drm/i915/gt/intel_ggtt.c, gen8_ggtt_pte_decode: a GGTT entry's address is its bits 45:12, its presence bit 0.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_ggtt.c#L293-L299
static inline uint64_t gen8_ggtt_pte_decode(uint64_t pte, bool *is_present) {
	*is_present = (pte & GEN8_PAGE_PRESENT) != 0;
	return pte & GEN12_GGTT_PTE_ADDR_MASK;
}

static bool ringDw(NGreen *cb, uint32_t start, uint32_t off, uint32_t &out, uint32_t &cPage, uint64_t &cPa) {
	const uint32_t ga = start + off;
	const uint32_t page = ga >> 12;
	if (page != cPage) {
		const uint64_t pte = (static_cast<uint64_t>(cb->readReg32(GGTT_PTE_HI(page))) << 32) | cb->readReg32(GGTT_PTE_LO(page));
		bool is_present = false;
		cPa = gen8_ggtt_pte_decode(pte, &is_present);
		if (!is_present) return false;
		cPage = page;
	}
	return readPhys32(cPa | (ga & 0xfffu), out);
}
static void patchL3Batch(uint64_t pml4, uint64_t bva) {
	const uint64_t pa = pageWalk(pml4, bva);
	if (pa == 0) return;
	uint32_t n = static_cast<uint32_t>((0x1000ull - (pa & 0xfffull)) / 4ull);
	if (n > kL3ScanDw) n = kL3ScanDw;
	volatile uint32_t *p = physToVirt(pa, n * 4u);
	if (p == nullptr) return;
	flushLines(p, n * 4u);
	for (uint32_t i = 0; i + 2 < n; i++) {
		if (p[i] != MI_LOAD_REGISTER_IMM(1) || p[i + 1] != kL3AllocReg) continue;
		const uint32_t was = p[i + 2];
		if (was == kAppleL3) {
			p[i + 2] = kMesaL3;
			flushLines(&p[i + 2], 4);
		}
		return;
	}
}

static uint32_t cmdLen(uint32_t dw) {
	const uint32_t type = dw >> 29;
	if (type == 0) {
		const uint32_t op = (dw >> 23) & 0x3fu;
		return op < 0x10u ? 1u : (dw & 0xffu) + 2u;
	}
	if (type == 3) {
		const uint32_t sub = (dw >> 27) & 3u, opc = (dw >> 24) & 7u;
		if (sub == 1 && opc <= 1) return 1u;
		return (dw & 0xffu) + 2u;
	}
	return 0;
}

static constexpr uint32_t kPoshBit  = 0x00000010u;
static constexpr uint32_t kPoshMask = 0x00100000u;
static constexpr uint32_t kPoshReg  = 0x21b0u;

static void applyPoshContextControl(NGreen *cb, void *ctx) {
	if (cb == nullptr || ctx == nullptr) return;
	volatile uint32_t *img = lrcImageOf(ctx);
	if (img == nullptr) return;
	const uint32_t cc = img[CTX_CONTEXT_CONTROL], reg = img[kPoshSlot - 1], posh = img[kPoshSlot];
	if ((cc & kPoshBit) != 0 && reg == kPoshReg && posh != 0) {
		img[CTX_CONTEXT_CONTROL] = (cc & 0xffefffefu) | kPoshMask;
		__sync_synchronize();
		flushLines(&img[CTX_CONTEXT_CONTROL], 4u);
	}
}

// From i915, drivers/gpu/drm/i915/gt/intel_workarounds.c, gen12_ctx_workarounds_init: the context workarounds of a display-12 GT
// (ADL-S is not DG1, so the HIZ_CHICKEN and COMMON_SLICE_CHICKEN4 rows apply), in its order; a row's kind is the helper it calls
// there (wa_masked_en, wa_masked_field_set, wa_add with ~0 as the clear mask, wa_mcr_write_or). The last row is rcs_engine_wa_init's
// ENABLE_SMALLPL, an engine workaround there, carried by the context image here as well. The registers: gt/intel_gt_regs.h.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L715-L778
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L301-L335
enum : uint32_t { kWaMaskedEn = 0, kWaMaskedFieldSet = 1, kWaMcrWriteOr = 2, kWaAdd = 3 };
struct i915_wa { uint32_t reg, kind, clr, set; const char *name; };
static constexpr uint32_t kWaCount = 7;
static const i915_wa kContextWa[kWaCount] = {
	{GEN11_COMMON_SLICE_CHICKEN3, kWaMaskedEn,       0u,                            GEN12_DISABLE_CPS_AWARE_COLOR_PIPE,             "CSC3"},
	{GEN8_CS_CHICKEN1,            kWaMaskedFieldSet, GEN9_PREEMPT_GPGPU_LEVEL_MASK, GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL,          "CSCHK1"},
	{GEN12_FF_MODE2,              kWaAdd,            ~0u,                           FF_MODE2_TDS_TIMER_128 | FF_MODE2_GS_TIMER_224, "FFMODE2"},
	{HIZ_CHICKEN,                 kWaMaskedEn,       0u,                            HZ_DEPTH_TEST_LE_GE_OPT_DISABLE,                "HIZ"},
	{COMMON_SLICE_CHICKEN4,       kWaMaskedEn,       0u,                            DISABLE_TDC_LOAD_BALANCING_CALC,                "CSC4"},
	{GEN8_WM_CHICKEN2,            kWaMcrWriteOr,     0u,                            WAIT_ON_DEPTH_STALL_DONE_DISABLE,               "WMCHK2"},
	{GEN10_SAMPLER_MODE,          kWaMaskedEn,       0u,                            ENABLE_SMALLPL,                                 "SMP"},
};
// The value a context-image slot takes for a workaround row: i915 appends its own MI_LOAD_REGISTER_IMM after the golden context
// (intel_engine_emit_ctx_wa: a masked row's value is REG_MASKED_FIELD_ENABLE(set) or REG_MASKED_FIELD(clr, set), an OR row reads the
// live register), so the golden image's value is applied first and i915's masked write after it. This driver patches the one slot the
// golden image has, so the slot takes the merge of the two: the golden value with i915's bits and their mask bits added. Dropping the
// golden bits would be the change.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_workarounds.c#L992-L1065
static uint32_t waNewValue(const i915_wa &w, uint32_t old) {
	switch (w.kind) {
	case kWaMaskedEn:       return old | w.set | (w.set << 16);
	case kWaMaskedFieldSet: return (old & ~w.clr) | w.set | (w.clr << 16);
	case kWaMcrWriteOr:     return old | w.set;
	default:                return w.set;
	}
}
static void patchImageWa(volatile uint32_t *base, uint32_t ndw) {
	uint32_t i = 0;
	while (i < ndw) {
		const uint32_t dw = base[i];
		const uint32_t len = (dw & 0xffu) + 1u;
		if ((dw >> 23) != 0x22u || len < 2u || (len & 1u) != 0 || i + 1u + len > ndw) { i++; continue; }
		bool ok = true;
		for (uint32_t j = 0; j < len && ok; j += 2) {
			const uint32_t r = base[i + 1u + j];
			ok = r != 0 && (r & 3u) == 0 && r < 0x40000u;
		}
		if (!ok) { i++; continue; }
		for (uint32_t j = 0; j < len; j += 2) {
			const uint32_t r = base[i + 1u + j];
			for (uint32_t t = 0; t < kWaCount; t++) {
				if (r != kContextWa[t].reg) continue;
				const uint32_t vi = i + 2u + j;
				const uint32_t nv = waNewValue(kContextWa[t], base[vi]);
				if (nv != base[vi]) { base[vi] = nv; flushLines(&base[vi], 4u); }
			}
		}
		i += 1u + len;
	}
}
static void *volatile gSafeImagePtr = nullptr;

static void applySafeImageWa(void *ctx) {
	auto *accel = *reinterpret_cast<void *volatile *>(static_cast<volatile uint8_t *>(ctx) + 0x50);
	auto *obj = accel ? *reinterpret_cast<OSObject *volatile *>(static_cast<volatile uint8_t *>(accel) + 0x1268) : nullptr;
	if (obj == nullptr || obj == gSafeImagePtr) return;
	gSafeImagePtr = obj;
	auto *md = OSDynamicCast(IOBufferMemoryDescriptor, obj);
	const uint8_t *bytes = md ? static_cast<const uint8_t *>(md->getBytesNoCopy()) : nullptr;
	const size_t len = md ? md->getLength() : 0;
	if (bytes == nullptr || len < LRC_STATE_OFFSET + 0x1000u) return;
	auto *img = reinterpret_cast<volatile uint32_t *>(const_cast<uint8_t *>(bytes) + LRC_STATE_OFFSET);
	if (img[CTX_CONTEXT_CONTROL - 1] != RING_CONTEXT_CONTROL(RENDER_RING_BASE) || img[CTX_RING_TAIL - 1] != RING_TAIL(RENDER_RING_BASE) || img[CTX_RING_START - 1] != RING_START(RENDER_RING_BASE)) return;
	patchImageWa(img, static_cast<uint32_t>((len - LRC_STATE_OFFSET) / 4u));
}
static void applyContextWa(NGreen *cb, void *ctx) {
	if (cb == nullptr || ctx == nullptr) return;
	applySafeImageWa(ctx);
	volatile uint32_t *img = lrcImageOf(ctx);
	auto *buf = *reinterpret_cast<void *volatile *>(static_cast<volatile uint8_t *>(ctx) + kCtxLrcBufOff);
	const uint64_t size = buf ? getMember<uint64_t>(buf, 0x20) : 0;
	if (img == nullptr || size < LRC_STATE_OFFSET + 0x1000u || size > 0x100000u) return;
	patchImageWa(img, static_cast<uint32_t>((size - LRC_STATE_OFFSET) / 4u));
}

static constexpr uint32_t kDsbSizePatchAt = 10;
static const uint8_t kDsbSizePre[] = {0x49, 0x8d, 0x44, 0x0c, 0xff, 0x31, 0xd2, 0x48, 0xf7, 0xf1,
                                   0x25, 0xff, 0xff, 0x0f, 0x00, 0x49, 0x09, 0xf3};
static uint32_t uptimeMs();

static void patchDsbSize(mach_vm_address_t address, size_t size) {
	uint8_t post[sizeof(kDsbSizePre)];
	memcpy(post, kDsbSizePre, sizeof(post));
	post[kDsbSizePatchAt] = 0xb8;
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kDsbSizePre) <= size; i++) {
		if (img[i] != 0x49) continue;
		if (memcmp(img + i, kDsbSizePre, sizeof(kDsbSizePre)) == 0) { if (n++ == 0) at = i; }
		else if (memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	*const_cast<uint8_t *>(img + at + kDsbSizePatchAt) = 0xb8;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static constexpr uint32_t kResolveEopPatchAt = 10, kResolveEopPatchLen = 8;
static const uint8_t kResolveEopPre[] = {0x4c, 0x8b, 0xa5, 0xb8, 0xfe, 0xff, 0xff,
                                   0x45, 0x89, 0xe5,
                                   0x41, 0xc1, 0xed, 0x02, 0x49, 0xc1, 0xe5, 0x02,
                                   0x49, 0xc1, 0xec, 0x20,
                                   0x48, 0xb8, 0x04, 0x00, 0x00, 0x7a, 0x04, 0x14, 0x10, 0x00};
static const uint8_t kResolveEopPost[kResolveEopPatchLen] = {0x41, 0x81, 0xc5, 0x00, 0x4f, 0x00, 0x00, 0x90};

static void patchResolveEop(mach_vm_address_t address, size_t size) {
	uint8_t post[sizeof(kResolveEopPre)];
	memcpy(post, kResolveEopPre, sizeof(post));
	memcpy(post + kResolveEopPatchAt, kResolveEopPost, kResolveEopPatchLen);
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kResolveEopPre) <= size; i++) {
		if (img[i] != 0x4c) continue;
		if (memcmp(img + i, kResolveEopPre, sizeof(kResolveEopPre)) == 0) { if (n++ == 0) at = i; }
		else if (memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	memcpy(const_cast<uint8_t *>(img + at + kResolveEopPatchAt), kResolveEopPost, kResolveEopPatchLen);
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static const uint8_t kTileFlushPre[] = {0x48, 0xb8, 0x04, 0x00, 0x00, 0x7a, 0x00, 0x50, 0x10, 0x00};
static constexpr uint32_t kTileFlushPatchAt = 9;
static constexpr uint8_t kTileFlushPost = PIPE_CONTROL_TILE_CACHE_FLUSH >> 24;

static void patchTileFlush(mach_vm_address_t address, size_t size) {
	uint8_t post[sizeof(kTileFlushPre)];
	memcpy(post, kTileFlushPre, sizeof(post));
	post[kTileFlushPatchAt] = kTileFlushPost;
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kTileFlushPre) <= size; i++) {
		if (img[i] != 0x48) continue;
		if (memcmp(img + i, kTileFlushPre, sizeof(kTileFlushPre)) == 0) { if (n++ == 0) at = i; }
		else if (memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	const_cast<uint8_t *>(img)[at + kTileFlushPatchAt] = kTileFlushPost;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static constexpr uint32_t kAuxFlushAccelBits = 0x1380;
static constexpr uint32_t kAuxFlushSites = 3;
static const uint8_t kAuxFlushPre[14] = {0x48, 0xf7, 0xd6,
                                     0x48, 0x21, 0xf1,
                                     0x48, 0x89, 0x8c, 0xd0, 0x80, 0x13, 0x00, 0x00};
static const uint8_t kAuxFlushMid[6] = {0x48, 0x31, 0xf1,
                                    0x80, 0xc9, 0x01};
static const uint8_t kAuxFlushStore[8] = {0x48, 0x89, 0x8c, 0xd0, 0x80, 0x13, 0x00, 0x00};
static volatile bool gAuxFlushPatched = false;

static void patchAuxFlush(mach_vm_address_t address, size_t size) {
	gAuxFlushPatched = false;
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	struct Site { size_t at; uint32_t len; bool r14, post; int64_t target; };
	Site s[kAuxFlushSites + 1];
	uint32_t n = 0, nPost = 0;
	for (size_t i = 0; i + 27 <= size && n <= kAuxFlushSites; i++) {
		const uint8_t b = img[i];
		if (b == 0x48 && memcmp(img + i, kAuxFlushPre, sizeof(kAuxFlushPre)) == 0) {
			const bool rbx = img[i + 14] == 0x48 && img[i + 15] == 0x89 && img[i + 16] == 0xdf &&
			                 img[i + 22] == 0xc6 && img[i + 23] == 0x43 && img[i + 24] == 0x6e && img[i + 25] == 0x00;
			const bool r14 = img[i + 14] == 0x4c && img[i + 15] == 0x89 && img[i + 16] == 0xf7 && img[i + 22] == 0x41 &&
			                 img[i + 23] == 0xc6 && img[i + 24] == 0x46 && img[i + 25] == 0x6e && img[i + 26] == 0x00;
			if ((!rbx && !r14) || img[i + 17] != 0xe8) continue;
			int32_t rel;
			memcpy(&rel, img + i + 18, 4);
			if (n < kAuxFlushSites) s[n] = Site {i, r14 ? 27u : 26u, r14, false, static_cast<int64_t>(i) + 22 + rel};
			n++;
		} else if ((b == 0xc6 || b == 0x41) && (memcmp(img + i + (b == 0x41 ? 5 : 4), kAuxFlushMid, sizeof(kAuxFlushMid)) == 0)) {
			const bool r14 = b == 0x41;
			const uint32_t c = r14 ? 5u : 4u;
			const bool lead = r14 ? (img[i + 1] == 0xc6 && img[i + 2] == 0x46 && img[i + 3] == 0x6e && img[i + 4] == 0x00)
			                      : (img[i + 1] == 0x43 && img[i + 2] == 0x6e && img[i + 3] == 0x00);
			if (!lead || memcmp(img + i + c + 6, kAuxFlushStore, sizeof(kAuxFlushStore)) != 0) continue;
			const uint32_t mv = c + 14;
			if (img[i + mv + 3] != 0xe8) continue;
			int32_t rel;
			memcpy(&rel, img + i + mv + 4, 4);
			if (n < kAuxFlushSites) s[n] = Site {i, r14 ? 27u : 26u, r14, true, static_cast<int64_t>(i) + mv + 8 + rel};
			n++; nPost++;
		}
	}
	if (n != kAuxFlushSites) return;
	if (s[1].target != s[0].target || s[2].target != s[0].target) return;
	if (nPost == kAuxFlushSites) { gAuxFlushPatched = true; return; }
	if (nPost != 0) return;
	uint8_t nw[kAuxFlushSites][27];
	for (uint32_t k = 0; k < kAuxFlushSites; k++) {
		uint8_t *p = nw[k];
		uint32_t o = 0;
		if (s[k].r14) { const uint8_t m[5] = {0x41, 0xc6, 0x46, 0x6e, 0x00}; memcpy(p, m, 5); o = 5; }
		else { const uint8_t m[4] = {0xc6, 0x43, 0x6e, 0x00}; memcpy(p, m, 4); o = 4; }
		memcpy(p + o, kAuxFlushMid, sizeof(kAuxFlushMid)); o += sizeof(kAuxFlushMid);
		memcpy(p + o, kAuxFlushStore, sizeof(kAuxFlushStore)); o += sizeof(kAuxFlushStore);
		if (s[k].r14) { p[o] = 0x4c; p[o + 1] = 0x89; p[o + 2] = 0xf7; }
		else { p[o] = 0x48; p[o + 1] = 0x89; p[o + 2] = 0xdf; }
		o += 3;
		p[o] = 0xe8;
		const int32_t rel = static_cast<int32_t>(s[k].target - static_cast<int64_t>(s[k].at + o + 5));
		memcpy(p + o + 1, &rel, 4);
		o += 5;
		if (o != s[k].len) return;
	}
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	for (uint32_t k = 0; k < kAuxFlushSites; k++) memcpy(const_cast<uint8_t *>(img + s[k].at), nw[k], s[k].len);
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	bool stuck = true;
	for (uint32_t k = 0; k < kAuxFlushSites; k++) stuck = stuck && memcmp(img + s[k].at, nw[k], s[k].len) == 0;
	gAuxFlushPatched = stuck;
}

static void armAuxFlushBit(void *accel) {
	if (accel == nullptr) return;
	auto *bits = reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(accel) + kAuxFlushAccelBits);
	if (gAuxFlushPatched) __sync_fetch_and_or(bits, 1ull);
}

static bool readPpgtt64(uint64_t pa, uint64_t &out) {
	volatile uint32_t *p = physToVirt(pa, 8);
	if (p == nullptr) return false;
	out = (static_cast<uint64_t>(p[1]) << 32) | p[0];
	return true;
}
static uint64_t walkPpgtt(uint64_t pml4, uint64_t va) {
	uint64_t tbl = pml4 & kL3AddrMask;
	if (tbl == 0) return 0;
	const uint32_t idx[4] = {
	    static_cast<uint32_t>((va >> 39) & 0x1ff), static_cast<uint32_t>((va >> 30) & 0x1ff),
	    static_cast<uint32_t>((va >> 21) & 0x1ff), static_cast<uint32_t>((va >> 12) & 0x1ff)};
	for (int lvl = 0; lvl < 4; lvl++) {
		uint64_t ent = 0;
		if (!readPpgtt64(tbl + static_cast<uint64_t>(idx[lvl]) * 8ull, ent)) return 0;
		if ((ent & 1ull) == 0) return 0;
		if (lvl == 3) return (ent & kL3AddrMask) | (va & 0xfffull);
		if (lvl >= 1 && (ent & (1ull << 7))) {
			if (lvl == 1) return (ent & 0x0000FFFFC0000000ull) | (va & 0x3FFFFFFFull);
			return (ent & 0x0000FFFFFFE00000ull) | (va & 0x1FFFFFull);
		}
		tbl = ent & kL3AddrMask;
	}
	return 0;
}
static uint32_t readPpgtt(uint64_t pml4, uint64_t va, uint32_t n, uint32_t *out) {
	uint32_t got = 0;
	while (got < n) {
		const uint64_t pa = walkPpgtt(pml4, va + static_cast<uint64_t>(got) * 4ull);
		if (pa == 0) break;
		uint32_t take = static_cast<uint32_t>((0x1000ull - (pa & 0xfffull)) / 4ull);
		if (take > n - got) take = n - got;
		volatile uint32_t *p = physToVirt(pa, take * 4u);
		if (p == nullptr) break;
		for (uint32_t k = 0; k < take; k++) out[got + k] = p[k];
		got += take;
	}
	return got;
}

static constexpr uint32_t kTimerSlots = 16;
struct TimerSource { void *src; int32_t net; uint32_t en, dis, skip; };
static TimerSource gTimerSources[kTimerSlots];
static IOSimpleLock *gTimerLock = nullptr;
static IOSimpleLock *gCtxFreeLock = nullptr;
static volatile uint32_t gCtxFreeRouted = 0;

static inline void ctxFreeLock() { if (gCtxFreeLock != nullptr) IOSimpleLockLock(gCtxFreeLock); }
static inline void ctxFreeUnlock() { if (gCtxFreeLock != nullptr) IOSimpleLockUnlock(gCtxFreeLock); }

static constexpr mach_vm_address_t kPmNotifyWrapperOff = 0x648fc;
static const uint8_t kGtClockPrologue[14] = {0x55, 0x48, 0x89, 0xe5, 0x49, 0x89, 0xc8, 0x48, 0x89, 0xd1, 0x89, 0xf2, 0x89, 0xfe};
static volatile uint32_t gGtClockActive = 0;
static volatile uint32_t gRp0Ratio = 0;
// From i915, drivers/gpu/drm/i915/gt/intel_rps.c, __gen6_rps_get_freq_caps: rp0_freq is the low byte of GEN6_RP_STATE_CAP, times
// GEN9_FREQ_SCALER on gen 11+ (the ratio in 16.67 MHz units); gen6_rps_set requests a frequency as GEN9_FREQUENCY(freq).
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_rps.c#L1136-L1170
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_rps.c#L803-L825
static uint32_t rp0Ratio() {
	uint32_t r = gRp0Ratio;
	if (r != 0) return r;
	auto *cb = NGreen::callback;
	if (cb == nullptr || !cb->mmioMapped()) return 0;
	r = (cb->readReg32(GEN6_RP_STATE_CAP) & 0xffu) * GEN9_FREQ_SCALER;
	if (r == 0 || r > 0x1ffu) return 0;
	gRp0Ratio = r;
	return r;
}
static void gtClockSetActive(uint32_t on) {
	gGtClockActive = on != 0 ? 1u : 0u;
}
static int answerPmNotify(unsigned int cls, unsigned int busy, unsigned long long *hint, unsigned int *freq) {
	(void)cls; (void)busy; (void)hint;
	if (gGtClockActive == 0 || freq == nullptr) return static_cast<int>(kIOReturnUnsupported);
	const uint32_t r = rp0Ratio();
	if (r == 0) return static_cast<int>(kIOReturnUnsupported);
	*freq = GEN9_FREQUENCY(r);
	return 0;
}
static void routeGtClock(KernelPatcher &patcher, mach_vm_address_t address, size_t size);

static bool setFastClearEnable(uint64_t pml4, uint64_t va, uint32_t expect) {
	const uint64_t pa = walkPpgtt(pml4, va);
	volatile uint32_t *p = (pa != 0 && (pa & 3ull) == 0) ? physToVirt(pa, 4) : nullptr;
	if (p == nullptr) return false;
	flushLines(p, 4);
	const uint32_t cur = *p;
	if (cur != expect) return false;
	*p = cur | 0x100u;
	flushLines(p, 4);
	return *p == (cur | 0x100u);
}

static constexpr uint32_t kResolveMaxDw = 1024;
static void setScopedFastClear(uint64_t pml4, uint64_t bva) {
	uint32_t w[16];
	if (readPpgtt(pml4, bva & ~3ull, 8, w) < 8) return;
	if (w[0] != GFX_OP_PIPE_CONTROL(6) || w[6] != 0x69041310u) return;
	uint64_t ssb = 0;
	uint32_t btp = 0, dw6 = 0, off = 0, psOff = 0;
	bool ssbSet = false, btpSet = false, ps = false, ended = false;
	while (off < kResolveMaxDw) {
		const uint32_t got = readPpgtt(pml4, (bva & ~3ull) + off * 4ull, 16, w);
		if (got == 0) break;
		const uint32_t dw = w[0];
		if (dw == MI_BATCH_BUFFER_END) { ended = true; break; }
		if (dw == 0) { off++; continue; }
		const uint32_t len = cmdLen(dw);
		if (len == 0) break;
		const uint32_t op = dw >> 16;
		if (op == 0x6101u && len >= 22 && got >= 6 && (w[4] & 1u)) {
			ssb = ((static_cast<uint64_t>(w[5] & 0xffffu) << 32) | w[4]) & ~0xfffull;
			ssbSet = true;
		} else if (op == 0x782au && got >= 2) {
			btp = w[1] & 0xffe0u;
			btpSet = true;
		} else if (op == 0x7820u && len >= 12 && got >= 12 && w[1] == 0 && w[2] == 0 && ((w[6] >> 6) & 3u) != 0) {
			ps = true;
			dw6 = w[6];
			psOff = off;
		}
		off += len;
	}
	if (!ended || !ps || ((dw6 >> 6) & 3u) != 2u || (dw6 & 0x100u) != 0 || !ssbSet || !btpSet) return;
	uint32_t bt0 = 0, s[16];
	if (readPpgtt(pml4, ssb + btp, 1, &bt0) != 1 || bt0 == 0) return;
	if (readPpgtt(pml4, ssb + (bt0 & ~0x3fu), 16, s) != 16) return;
	const uint64_t surf = ((static_cast<uint64_t>(s[9] & 0xffffu) << 32) | s[8]) & 0x0000fffffffff000ull;
	if ((s[6] & 7u) != 5u || surf == 0) return;
	setFastClearEnable(pml4, (bva & ~3ull) + (psOff + 6u) * 4ull, dw6);
}

static void patchPendingBatches(void *cs) {
	auto *cb = NGreen::callback;
	if (cs == nullptr || cb == nullptr || !cb->mmioMapped()) return;
	if (streamerSq(cs) != RENDER_RING_BASE + 0x510) return;
	if (gPhysmapBase == nullptr || gPhysmapMax == nullptr) return;
	auto *p = static_cast<volatile uint8_t *>(cs);
	const uint32_t cur = *reinterpret_cast<volatile const uint32_t *>(p + kCurIdxOff);
	if (cur > 1) return;
	const uint32_t pend = 1u - cur;
	for (uint32_t e = 0; e < 2; e++) {
		void *item = *reinterpret_cast<void *volatile *>(p + kElBase + pend * 16 + e * 8);
		void *ctx  = item ? *static_cast<void *volatile *>(item) : nullptr;
		volatile uint32_t *img = lrcImageOf(ctx);
		if (img == nullptr) continue;
		const uint32_t start = img[CTX_RING_START] & 0xFFFFF000u;
		const uint32_t ctl   = img[CTX_RING_CTL];
		const uint64_t pml4  = (static_cast<uint64_t>(img[CTX_PDP0_UDW]) << 32) | img[CTX_PDP0_LDW];
		if (start == 0 || (ctl & 1u) == 0 || (pml4 & kL3AddrMask) == 0) continue;
		const uint32_t size = (((ctl >> 12) & 0x1ffu) + 1u) << 12;
		uint32_t off = img[CTX_RING_HEAD] & (size - 1u) & ~3u;
		uint32_t cPage = 0xffffffffu;
		uint64_t cPa = 0;
		for (uint32_t k = 0; k < kL3RingDw; k++) {
			uint32_t dw = 0;
			if (!ringDw(cb, start, off, dw, cPage, cPa)) break;
			if (dw == MI_LOAD_REGISTER_IMM(1)) {
				uint32_t reg = 0, val = 0;
				if (ringDw(cb, start, (off + 4u) & (size - 1u), reg, cPage, cPa) && ringDw(cb, start, (off + 8u) & (size - 1u), val, cPage, cPa) && reg == GEN12_CCS_AUX_INV && val == 1u) {
					off = (off + 12u) & (size - 1u);
					k += 2;
					continue;
				}
			}
			if ((dw & 0xFF8001FFu) == (MI_BATCH_BUFFER_START_GEN8 | (1u << 8))) {
				uint32_t lo = 0, hi = 0;
				if (ringDw(cb, start, (off + 4u) & (size - 1u), lo, cPage, cPa) && ringDw(cb, start, (off + 8u) & (size - 1u), hi, cPage, cPa)) {
					const uint64_t bva = (static_cast<uint64_t>(hi & 0xffffu) << 32) | (lo & ~3u);
					patchL3Batch(pml4, bva);
					setScopedFastClear(pml4, bva);
				}
				off = (off + 12u) & (size - 1u);
				k += 2;
				continue;
			}
			if ((dw & 0xFF8001FFu) == MI_BATCH_BUFFER_START_GEN8) {
				off = (off + 12u) & (size - 1u);
				k += 2;
				continue;
			}
			off = (off + 4u) & (size - 1u);
		}
	}
}

static constexpr uint32_t kResubmitHoldMaxUs = 3000;
static void holdResubmitIfCompleting(void *cs) {
	auto *cb = NGreen::callback;
	if (cs == nullptr || cb == nullptr || !cb->mmioMapped()) return;
	if (streamerSq(cs) != RENDER_RING_BASE + 0x510) return;
	auto *p = static_cast<volatile uint8_t *>(cs);
	const uint32_t cur = *reinterpret_cast<volatile const uint32_t *>(p + kCurIdxOff);
	if (cur > 1) return;
	void *item = *reinterpret_cast<void *volatile *>(p + kElBase + (1u - cur) * 16);
	void *ctx  = item ? *static_cast<void *volatile *>(item) : nullptr;
	if (ctx == nullptr) return;
	const uint32_t pendId = *reinterpret_cast<volatile const uint32_t *>(static_cast<volatile uint8_t *>(ctx) + kCtxIdOff) & 0x7ffu;
	const uint32_t exLo0 = cb->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
	const uint32_t exHi0 = cb->readReg32(RING_EXECLIST_STATUS_HI(RENDER_RING_BASE));
	const uint32_t head0 = cb->readReg32(RING_HEAD(RENDER_RING_BASE));
	const uint32_t tail0 = cb->readReg32(RING_TAIL(RENDER_RING_BASE));
	const uint32_t csp0  = cb->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	const uint32_t curId = (exHi0 >> 5) & 0x7ffu;
	const bool active    = (exLo0 & 0x18u) != 0;
	const bool drained   = head0 == tail0 && head0 != 0;
	if (!(active && drained && curId == pendId)) return;
	for (uint32_t waited = 0; waited < kResubmitHoldMaxUs; waited += 10) {
		IODelay(10);
		const uint32_t csp  = cb->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
		const uint32_t exLo = cb->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
		const uint32_t mi   = cb->readReg32(RING_MI_MODE(RENDER_RING_BASE));
		if ((csp & 0xfu) != (csp0 & 0xfu) || (exLo & 0x18u) == 0 || (mi & MODE_IDLE) != 0) break;
	}
}

volatile uint64_t gStolenLo = 0, gStolenHi = 0;
volatile uint32_t gStolenState = 0;

static void initStolenRange() {
	auto *cb = NGreen::callback;
	if (cb == nullptr || gStolenState != 0) return;
	const uint64_t size = cb->stolen_size != 0 ? cb->stolen_size : (128ull << 20);
	const uint32_t g1 = cb->readReg32(GGTT_PTE_LO(1)), g2 = cb->readReg32(GGTT_PTE_LO(2)), g3 = cb->readReg32(GGTT_PTE_LO(3));
	const uint32_t g1hi = cb->readReg32(GGTT_PTE_HI(1));
	uint64_t ggttBase = 0;
	if ((g1 & g2 & g3 & 1u) != 0 && g1hi == 0 && (g1 & 0xFFFFF000u) >= 0x101000u && (g2 & 0xFFFFF000u) == (g1 & 0xFFFFF000u) + 0x1000u && (g3 & 0xFFFFF000u) == (g1 & 0xFFFFF000u) + 0x2000u)
		ggttBase = static_cast<uint64_t>(g1 & 0xFFFFF000u) - 0x1000ull;
	if ((ggttBase & 0xFFFFFull) != 0) ggttBase = 0;
	uint32_t dw0 = 0, dw1 = 0;
	// The BDSM register as the kernel's early-quirks stolen-base reader names it (include/drm/intel/i915_drm.h).
	if (cb->gpuDevice() != nullptr) { dw0 = cb->gpuDevice()->configRead32(INTEL_GEN11_BSM_DW0); dw1 = cb->gpuDevice()->configRead32(INTEL_GEN11_BSM_DW1); }
	const uint64_t bdsm = (dw0 != 0 && dw0 != 0xFFFFFFFFu && dw1 == 0) ? static_cast<uint64_t>(dw0 & INTEL_BSM_MASK) : 0;
	uint64_t lo = ggttBase, hi = ggttBase != 0 ? ggttBase + size : 0;
	if (bdsm != 0) {
		if (lo == 0 || bdsm < lo) lo = bdsm;
		if (bdsm + size > hi) hi = bdsm + size;
	}
	gStolenLo = lo;
	gStolenHi = hi;
	gStolenState = lo != 0 ? 1u : 2u;
}

static bool isStolen(uint64_t pa) {
	if (gStolenState == 0) initStolenRange();
	return gStolenState == 1 ? (pa >= gStolenLo && pa < gStolenHi) : pa < (1ull << 32);
}

static bool isExpectedTGLDebugFramebuffer(mach_vm_address_t address, size_t size) {
	struct Anchor {
		mach_vm_address_t offset;
		const uint8_t *bytes;
		size_t length;
	};

	static const uint8_t controllerStart[] =
		{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
	static const uint8_t getOSInformation[] =
		{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x10, 0x49};
	static const uint8_t scalerInit[] =
		{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x30};
	static const Anchor anchors[] = {
		{0x5A8A0, controllerStart, arrsize(controllerStart)},
		{0x5D1A0, getOSInformation, arrsize(getOSInformation)},
		{0x98C48, scalerInit, arrsize(scalerInit)},
	};

	for (const auto &anchor : anchors) {
		if (anchor.offset > size || anchor.length > size - anchor.offset || memcmp(reinterpret_cast<const void *>(address + anchor.offset),
				anchor.bytes, anchor.length) != 0) {
			return false;
		}
	}
	return true;
}

static mach_vm_address_t gTGLFbBase = 0;

struct EdpTranscoderSite {
	uint32_t    cmpOff;
	uint32_t    jccOff;
	uint8_t     pre[24];
	uint8_t     preLen;
};

static const EdpTranscoderSite kEdpTranscoderSites[] = {
	{ 0x67de2, 0x67de6, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0xa1, 0xd9, 0x0e, 0x00, 0x41, 0xbc, 0x0f, 0x00, 0x00, 0x00 }, 19 },
	{ 0x683dc, 0x683e4, { 0x83, 0x78, 0x08, 0x00, 0x48, 0x8b, 0x55, 0xd0, 0x75, 0x13, 0x48, 0xff, 0x05, 0xa3, 0xd3, 0x0e, 0x00, 0x41, 0xbc, 0x0f, 0x00, 0x00, 0x00 }, 23 },
	{ 0x685e1, 0x685e5, { 0x83, 0x7a, 0x08, 0x00, 0x75, 0x0c, 0x48, 0xff, 0x05, 0xa2, 0xd1, 0x0e, 0x00, 0xb8, 0x0f, 0x00, 0x00, 0x00 }, 18 },
	{ 0x71b46, 0x71b4a, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0x3d, 0x3c, 0x0e, 0x00, 0x41, 0xbe, 0x0f, 0x00, 0x00, 0x00 }, 19 },
	{ 0x74704, 0x74708, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0x7f, 0x10, 0x0e, 0x00, 0x41, 0xbf, 0x0f, 0x00, 0x00, 0x00 }, 19 },
	{ 0x78e8d, 0x78e91, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0xf6, 0xc8, 0x0d, 0x00, 0x41, 0xbe, 0x0f, 0x00, 0x00, 0x00 }, 19 },
	{ 0x805f7, 0x805ff, { 0x83, 0x78, 0x08, 0x00, 0x48, 0x8b, 0x5d, 0xd0, 0x75, 0x1a, 0x48, 0xff, 0x05, 0x88, 0x51, 0x0d, 0x00, 0x41, 0xbc, 0x0f, 0x00, 0x00, 0x00 }, 23 },
	{ 0x80a0e, 0x80a12, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0x75, 0x4d, 0x0d, 0x00, 0x41, 0xbe, 0x0f, 0x00, 0x00, 0x00 }, 19 },
	{ 0x80db1, 0x80db5, { 0x83, 0x78, 0x08, 0x00, 0x75, 0x0d, 0x48, 0xff, 0x05, 0xd2, 0x49, 0x0d, 0x00, 0x41, 0xbd, 0x0f, 0x00, 0x00, 0x00 }, 19 },
};

static void patchEdpTranscoder(mach_vm_address_t address, size_t size) {
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	for (size_t i = 0; i < arrsize(kEdpTranscoderSites); ++i) {
		const EdpTranscoderSite &st = kEdpTranscoderSites[i];
		if (st.cmpOff + st.preLen > size || st.jccOff >= size) {
			continue;
		}
		const uint8_t cur = img[st.jccOff];
		if (cur == 0xeb) {
			continue;
		}
		bool match = (cur == 0x75);
		for (uint8_t k = 0; match && k < st.preLen; ++k)
			if (img[st.cmpOff + k] != st.pre[k]) match = false;
		if (!match) {
			continue;
		}
		auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
		if (status != KERN_SUCCESS) {
			continue;
		}
		*const_cast<uint8_t *>(img + st.jccOff) = 0xeb;
		MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
	}
}

static thread_call_t     gScanoutLateCall   = nullptr;
static volatile uint32_t gScanoutLateFires  = 0;
static volatile uint32_t gImrFixDone    = 0;

static constexpr uint32_t kPlaneCountPatchAt = 2;
static constexpr uint32_t kPlaneCountFallbackDisp = 0x98;
static const uint8_t kPlaneCountPre[] = {0x8b, 0x83, 0xb0, 0x00, 0x00, 0x00,
                                   0x85, 0xc0, 0xba, 0x01, 0x00, 0x00, 0x00};

static void patchPlaneCount(mach_vm_address_t address, size_t size, mach_vm_address_t getter) {
	uint32_t disp = kPlaneCountFallbackDisp;
	if (getter != 0) {
		static const uint8_t kHead[] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x87};
		const uint8_t *g = reinterpret_cast<const uint8_t *>(getter);
		if (memcmp(g, kHead, sizeof(kHead)) != 0 || g[10] != 0x5d || g[11] != 0xc3) return;
		memcpy(&disp, g + 6, sizeof(disp));
	}
	uint8_t post[sizeof(kPlaneCountPre)];
	memcpy(post, kPlaneCountPre, sizeof(post));
	memcpy(post + kPlaneCountPatchAt, &disp, sizeof(disp));
	const bool same = memcmp(post, kPlaneCountPre, sizeof(post)) == 0;
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kPlaneCountPre) <= size; i++) {
		if (img[i] != 0x8b) continue;
		if (memcmp(img + i, kPlaneCountPre, sizeof(kPlaneCountPre)) == 0) { if (n++ == 0) at = i; }
		else if (!same && memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1 || same) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	memcpy(const_cast<uint8_t *>(img + at + kPlaneCountPatchAt), &disp, sizeof(disp));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static constexpr uint32_t kPixelFormatPatchAt = 2;
static constexpr uint32_t kPixelFormatFallbackDisp = 0x80;
static const uint8_t kPixelFormatPre[] = {0x8b, 0x83, 0xa4, 0x00, 0x00, 0x00,
                                   0x89, 0x85, 0xf4, 0xfe, 0xff, 0xff};

static void patchPixelFormat(mach_vm_address_t address, size_t size, mach_vm_address_t getter) {
	uint32_t disp = kPixelFormatFallbackDisp;
	if (getter != 0) {
		static const uint8_t kHead[] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x87};
		const uint8_t *g = reinterpret_cast<const uint8_t *>(getter);
		if (memcmp(g, kHead, sizeof(kHead)) != 0 || g[10] != 0x5d || g[11] != 0xc3) return;
		memcpy(&disp, g + 6, sizeof(disp));
	}
	uint8_t post[sizeof(kPixelFormatPre)];
	memcpy(post, kPixelFormatPre, sizeof(post));
	memcpy(post + kPixelFormatPatchAt, &disp, sizeof(disp));
	const bool same = memcmp(post, kPixelFormatPre, sizeof(post)) == 0;
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kPixelFormatPre) <= size; i++) {
		if (img[i] != 0x8b) continue;
		if (memcmp(img + i, kPixelFormatPre, sizeof(kPixelFormatPre)) == 0) { if (n++ == 0) at = i; }
		else if (!same && memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1 || same) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	memcpy(const_cast<uint8_t *>(img + at + kPixelFormatPatchAt), &disp, sizeof(disp));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static constexpr uint32_t kValidatePixelFormatPatchAt = 2;
static constexpr uint32_t kValidatePixelFormatDisp = 0x80;
static const uint8_t kValidatePixelFormatPre[] = {0x8b, 0x80, 0xa4, 0x00, 0x00, 0x00,
                                   0x89, 0x45, 0xb4,
                                   0x48, 0x8d, 0x4d, 0x80};

static void patchValidatePixelFormat(mach_vm_address_t address, size_t size) {
	uint8_t post[sizeof(kValidatePixelFormatPre)];
	memcpy(post, kValidatePixelFormatPre, sizeof(post));
	memcpy(post + kValidatePixelFormatPatchAt, &kValidatePixelFormatDisp, sizeof(kValidatePixelFormatDisp));
	const uint8_t *img = reinterpret_cast<const uint8_t *>(address);
	uint32_t n = 0, nPost = 0;
	size_t at = 0;
	for (size_t i = 0; i + sizeof(kValidatePixelFormatPre) <= size; i++) {
		if (img[i] != 0x8b) continue;
		if (memcmp(img + i, kValidatePixelFormatPre, sizeof(kValidatePixelFormatPre)) == 0) { if (n++ == 0) at = i; }
		else if (memcmp(img + i, post, sizeof(post)) == 0) { if (nPost++ == 0) at = i; }
	}
	if (n + nPost == 0) return;
	if (n + nPost > 1) return;
	if (nPost == 1) return;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) return;
	memcpy(const_cast<uint8_t *>(img + at + kValidatePixelFormatPatchAt), &kValidatePixelFormatDisp, sizeof(kValidatePixelFormatDisp));
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
}

static mach_vm_address_t gBlit2DSupported = 0;
static mach_vm_address_t gBlit3DSupported = 0;

static constexpr uint32_t kPageonSlotTestEvent = 0x190, kPageonStepUs = 5, kPageonMaxUs = 30000, kPageonAutoOffRun = 8;
enum : uint32_t { kPageonNotRouted = 0, kPageonRouted, kPageonRouteFailed };
static mach_vm_address_t gEventMachineVtbl = 0;
static mach_vm_address_t gAccelPavpCommand = 0;  // V429: IntelAccelerator::PAVPCommandCallback
static mach_vm_address_t gOrgWaitDrmRestore = 0;  // V429: IntelAccelerator::waitForDRMSessionRestore
static volatile SInt32 gV429Imei = 0, gV429Rows = 0;  // IMEI: 0 not looked up, 1 absent, 2 present
static volatile uint32_t gPageonState = kPageonNotRouted, gPageonActive = 0, gPageonAutoOff = 0;
static volatile uint32_t gPageonTimeoutRun = 0;

static void pageonOnWake() {
	if (gPageonState != kPageonRouted) return;
	if (!__sync_bool_compare_and_swap(&gPageonAutoOff, 1u, 0u)) return;
	gPageonTimeoutRun = 0;
	gPageonActive = 1u;
}

static void *blit3dInitBefore(void *ctx) {
	auto *cb = Gen11::callback;
	auto *pool = ctx ? getMember<uint8_t *>(ctx, 0xe0) : nullptr;
	auto *ch = pool ? getMember<uint8_t *>(pool, 0x20) : nullptr;
	if (cb == nullptr || cb->oIOAF2_submitBuffer == 0 || ch == nullptr) return nullptr;
	using IncrementStamp = void (*)(void *);
	const auto vt = *reinterpret_cast<uintptr_t *>(ch);
	reinterpret_cast<IncrementStamp>(*reinterpret_cast<uintptr_t *>(vt + 0x148))(ch);
	return pool;
}
static void blit3dInitAfter(void *pool) {
	if (pool == nullptr) return;
	using SubmitBuffer = void (*)(void *);
	reinterpret_cast<SubmitBuffer>(Gen11::callback->oIOAF2_submitBuffer)(pool);
}

static mach_vm_address_t gOrgResolveInit = 0;

static bool wrapResolveInit(void *pool, void *buffer, uint64_t arg, bool flag) {
	auto *cb = Gen11::callback;
	auto *p = static_cast<uint8_t *>(pool);
	auto *ch = p != nullptr ? getMember<uint8_t *>(p, 0x20) : nullptr;
	const bool own = cb != nullptr && cb->oIOAF2_submitBuffer != 0 && ch != nullptr;
	if (own) {
		using IncrementStamp = void (*)(void *);
		const auto vt = *reinterpret_cast<uintptr_t *>(ch);
		reinterpret_cast<IncrementStamp>(*reinterpret_cast<uintptr_t *>(vt + 0x148))(ch);
	}
	const bool rc = FunctionCast(wrapResolveInit, gOrgResolveInit)(pool, buffer, arg, flag);
	if (own) {
		const uint64_t seg = *reinterpret_cast<volatile uint64_t *>(p + 0x1848);
		const uint64_t ptr = *reinterpret_cast<volatile uint64_t *>(p + 0x1858);
		if (ptr >= seg) {
			auto *end = reinterpret_cast<volatile uint32_t *>(ptr);
			end[0] = 0x05000000u;
			end[1] = 0u;
			*reinterpret_cast<volatile uint64_t *>(p + 0x1858) = ptr + 8;
			using SubmitBuffer = void (*)(void *);
			reinterpret_cast<SubmitBuffer>(cb->oIOAF2_submitBuffer)(pool);
		}
	}
	return rc;
}

static volatile uint32_t gBarrierArmed = 1u;

static volatile uint32_t gHaveBiosRc6 = 0;
static volatile uint32_t gBiosRc = 0, gBiosPg = 0;
// From i915, drivers/gpu/drm/i915/intel_uncore.c: a forcewake domain (fw_domain_init: the request and ack registers of RENDER and GT
// on gen 9+), fw_domain_get after fw_domain_wait_ack_clear, fw_domain_wait_ack_set for FORCEWAKE_ACK_TIMEOUT_MS, the fallback that toggles
// FORCEWAKE_KERNEL_FALLBACK up to ten times when an ack does not come (fw_domain_wait_ack_with_fallback), and fw_domain_put. i915
// requests with FORCEWAKE_KERNEL (bit 0); this driver's own requesters take bits of their own (4 for the engine probe, 5 for the RC6
// writes) so that Apple's driver's requester bits (1 and 2) and its reference count are untouched.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/intel_uncore.c#L167-L322
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/intel_uncore.c#L2181-L2317
struct intel_uncore_forcewake_domain { uint32_t reg_set, reg_ack; };
static constexpr intel_uncore_forcewake_domain fw_domain_render = { FORCEWAKE_RENDER_GEN9, FORCEWAKE_ACK_RENDER_GEN9 };
static constexpr intel_uncore_forcewake_domain fw_domain_gt     = { FORCEWAKE_GT_GEN9, FORCEWAKE_ACK_GT_GEN9 };
static volatile uint32_t gFwFallbacks = 0;

static inline uint32_t fw_ack(const intel_uncore_forcewake_domain &d) { return NGreen::callback->readReg32(d.reg_ack); }
static inline void fw_set(const intel_uncore_forcewake_domain &d, uint32_t val) { NGreen::callback->writeReg32(d.reg_set, _MASKED_BIT_ENABLE(val)); }
static inline void fw_clear(const intel_uncore_forcewake_domain &d, uint32_t val) { NGreen::callback->writeReg32(d.reg_set, _MASKED_BIT_DISABLE(val)); }

static bool __wait_for_ack(const intel_uncore_forcewake_domain &d, uint32_t ack, uint32_t value) {
	for (uint32_t us = 0; us < FORCEWAKE_ACK_TIMEOUT_MS * 1000u; us += 10u) {
		if ((fw_ack(d) & ack) == value) return true;
		IODelay(10);
	}
	return (fw_ack(d) & ack) == value;
}
static inline bool wait_ack_clear(const intel_uncore_forcewake_domain &d, uint32_t ack) { return __wait_for_ack(d, ack, 0); }
static inline bool wait_ack_set(const intel_uncore_forcewake_domain &d, uint32_t ack) { return __wait_for_ack(d, ack, ack); }

static bool fw_domain_wait_ack_with_fallback(const intel_uncore_forcewake_domain &d, uint32_t ack_bit, bool set) {
	const uint32_t value = set ? ack_bit : 0;
	unsigned int pass = 1;
	bool ack_detected;

	__sync_add_and_fetch(&gFwFallbacks, 1u);
	do {
		wait_ack_clear(d, FORCEWAKE_KERNEL_FALLBACK);
		fw_set(d, FORCEWAKE_KERNEL_FALLBACK);
		IODelay(10 * pass);
		wait_ack_set(d, FORCEWAKE_KERNEL_FALLBACK);
		ack_detected = (fw_ack(d) & ack_bit) == value;
		fw_clear(d, FORCEWAKE_KERNEL_FALLBACK);
	} while (!ack_detected && pass++ < 10);
	return ack_detected;
}

static inline void fw_domain_wait_ack_clear_fallback(const intel_uncore_forcewake_domain &d, uint32_t bit) {
	if (wait_ack_clear(d, bit)) return;
	fw_domain_wait_ack_with_fallback(d, bit, false);
}
static inline bool fw_domain_wait_ack_set_fallback(const intel_uncore_forcewake_domain &d, uint32_t bit) {
	if (wait_ack_set(d, bit)) return true;
	return fw_domain_wait_ack_with_fallback(d, bit, true);
}
static inline void fw_domain_get(const intel_uncore_forcewake_domain &d, uint32_t bit) { fw_set(d, bit); }
static inline void fw_domain_put(const intel_uncore_forcewake_domain &d, uint32_t bit) { fw_clear(d, bit); }

static constexpr uint32_t kRc6FwBit = 1u << 5;
static constexpr uint32_t kRc6AckNeed = 3u;
// fw_domains_get_with_fallback for the RC6 requester on RENDER and GT; the acks as a mask (1 = render, 2 = gt).
static uint32_t rc6FwGet() {
	fw_domain_wait_ack_clear_fallback(fw_domain_render, kRc6FwBit);
	fw_domain_get(fw_domain_render, kRc6FwBit);
	fw_domain_wait_ack_clear_fallback(fw_domain_gt, kRc6FwBit);
	fw_domain_get(fw_domain_gt, kRc6FwBit);
	uint32_t acks = 0;
	if (fw_domain_wait_ack_set_fallback(fw_domain_render, kRc6FwBit)) acks |= 1u;
	if (fw_domain_wait_ack_set_fallback(fw_domain_gt, kRc6FwBit)) acks |= 2u;
	return acks;
}
static inline bool rc6Acked(uint32_t acks) { return (acks & kRc6AckNeed) == kRc6AckNeed; }
static void rc6FwPut() {
	fw_domain_put(fw_domain_render, kRc6FwBit);
	fw_domain_put(fw_domain_gt, kRc6FwBit);
}
// From i915, drivers/gpu/drm/i915/gt/intel_rc6.c: of gen11_rc6_enable's rows only GEN9_RENDER_PG_ENABLE is applied here, on top of
// the BIOS's GEN9_PG_ENABLE (mode 4, the render power gating the BIOS leaves off and Apple leaves to the absent GuC: A22's fix); the
// thresholds and hysteresis, the media power-gating bits and GEN6_RC_CONTROL's RC6 enable stay the BIOS's and Apple's. Mode 2 is
// __intel_rc6_disable's GEN6_RC_CONTROL = 0; mode 1 clears the render bit again. The registers: gt/intel_gt_regs.h.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_rc6.c#L61-L138
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_rc6.c#L559-L574
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gt_regs.h#L835
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gt_regs.h#L870-L875
static constexpr uint32_t GEN6_RC_CONTROL       = 0xa090u;
static constexpr uint32_t GEN9_PG_ENABLE        = 0xa210u;
static constexpr uint32_t GEN9_RENDER_PG_ENABLE = 1u << 0;
static void applyRc6(uint32_t mode) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || !cb->mmioMapped()) return;
	const uint32_t acks = rc6FwGet();
	const uint32_t biosRc = cb->readReg32(GEN6_RC_CONTROL), biosPg = cb->readReg32(GEN9_PG_ENABLE);
	if (rc6Acked(acks) && __sync_bool_compare_and_swap(&gHaveBiosRc6, 0u, 1u)) { gBiosRc = biosRc; gBiosPg = biosPg; }
	uint32_t rc = gHaveBiosRc6 ? gBiosRc : biosRc, pg = gHaveBiosRc6 ? gBiosPg : biosPg;
	if (mode & 1u) pg &= ~GEN9_RENDER_PG_ENABLE;
	if (mode & 4u) pg |= GEN9_RENDER_PG_ENABLE;
	if (mode & 2u) rc = 0;
	if (rc6Acked(acks) && biosPg != pg) cb->writeReg32(GEN9_PG_ENABLE, pg);
	if (rc6Acked(acks) && biosRc != rc) cb->writeReg32(GEN6_RC_CONTROL, rc);
	SYSLOG("ngreen", "RC6: forcewake acks 0x%x (fallbacks %u), PG_ENABLE 0x%x -> 0x%x, RC_CONTROL 0x%x -> 0x%x",
	       acks, gFwFallbacks, biosPg, cb->readReg32(GEN9_PG_ENABLE), biosRc, cb->readReg32(GEN6_RC_CONTROL));
	rc6FwPut();
}
static void rc6ProbeTick() {
	static volatile uint32_t once = 0;
	if (!__sync_bool_compare_and_swap(&once, 0u, 1u)) return;
	applyRc6(RAPTOR_LAKE_HX_OR(4u, 0u));
}

static bool barrierWants() {
	return gBarrierArmed != 0;
}
static void barrierHangSeen() {
	gBarrierArmed = 0;
}

static volatile uint32_t gCsbT1Uses = 0;
static constexpr uint32_t kCsbT1Cap = 32;

static inline uint64_t csbElement(const uint8_t *cs, uint32_t slot, uint32_t el) {
	return *reinterpret_cast<const uint64_t *>(cs + kCsOffSlot0 + 16u * slot + 8u * el);
}
static inline uint32_t csbContextId(uint64_t wi) {
	if (wi == 0) return 0xffffffffu;
	const uint8_t *ctx = *reinterpret_cast<const uint8_t *const *>(wi);
	return ctx != nullptr ? *reinterpret_cast<const uint32_t *>(ctx + kCtxIdOff) : 0xfffffffeu;
}
static bool csbRawEntry(const uint8_t *cs, uint32_t &lo, uint32_t &hi, uint32_t &ptr) {
	lo = hi = ptr = 0;
	const uint8_t *accel = *reinterpret_cast<const uint8_t *const *>(cs + 0x10);
	const uint8_t *desc = *reinterpret_cast<const uint8_t *const *>(cs + kCsOffDesc);
	const uint32_t rd = *reinterpret_cast<const uint32_t *>(cs + kCsOffSwRead);
	if (accel == nullptr || desc == nullptr || rd > 5u) return false;
	if (*reinterpret_cast<const int8_t *>(accel + 0x1190) < 0) {
		const uint8_t *mmio = *reinterpret_cast<const uint8_t *const *>(accel + 0x1240);
		uint32_t bank = 0, reg = 0;
		memcpy(&bank, desc + 0x29, sizeof(bank));
		memcpy(&reg, desc + 0x25, sizeof(reg));
		if (mmio == nullptr || bank < 0x2000u || bank > 0x1ffff0u || reg < 0x2000u || reg > 0x1ffff0u) return false;
		lo = *reinterpret_cast<const volatile uint32_t *>(mmio + bank + rd * 8u);
		hi = *reinterpret_cast<const volatile uint32_t *>(mmio + bank + rd * 8u + 4u);
		ptr = *reinterpret_cast<const volatile uint32_t *>(mmio + reg);
	} else {
		const uint8_t *hwsp = *reinterpret_cast<const uint8_t *const *>(cs + kCsOffHwsp);
		if (hwsp == nullptr) return false;
		// The CSB ring and its write pointer sit in the HWSP where i915 reads them (I915_HWS_CSB_BUF0_INDEX, ICL_HWS_CSB_WRITE_INDEX).
		lo = *reinterpret_cast<const volatile uint32_t *>(hwsp + I915_HWS_CSB_BUF0_INDEX * 4u + rd * 8u);
		hi = *reinterpret_cast<const volatile uint32_t *>(hwsp + I915_HWS_CSB_BUF0_INDEX * 4u + rd * 8u + 4u);
		ptr = *reinterpret_cast<const volatile uint32_t *>(hwsp + ICL_HWS_CSB_WRITE_INDEX * 4u);
	}
	return true;
}
static inline bool csbT1Room() {
	return gCsbT1Uses < kCsbT1Cap;
}

static const uint32_t kClearColorSrgb8[255] = {
	0x391f22b4, 0x39eeb40e, 0x3a46eb61, 0x3a8b3e5e, 0x3ab3070b, 0x3adacfb8, 0x3b014c33, 0x3b153089,
	0x3b2914df, 0x3b3cf936, 0x3b50f2d1, 0x3b65fb9b, 0x3b7c3403, 0x3b89d060, 0x3b962333, 0x3ba314bd,
	0x3bb0a731, 0x3bbedcb7, 0x3bcdb76d, 0x3bdd3967, 0x3bed64af, 0x3bfe3b46, 0x3c07df91, 0x3c10f91b,
	0x3c1a6b32, 0x3c2436c8, 0x3c2e5cc7, 0x3c38de1a, 0x3c43bba4, 0x3c4ef648, 0x3c5a8ee4, 0x3c668654,
	0x3c72dd71, 0x3c7f950f, 0x3c865702, 0x3c8d148f, 0x3c940396, 0x3c9b247c, 0x3ca277a6, 0x3ca9fd78,
	0x3cb1b653, 0x3cb9a298, 0x3cc1c2a9, 0x3cca16e3, 0x3cd29fa4, 0x3cdb5d4b, 0x3ce45032, 0x3ced78b5,
	0x3cf6d72e, 0x3d0035fc, 0x3d051bb4, 0x3d0a1cec, 0x3d0f39d0, 0x3d14728a, 0x3d19c745, 0x3d1f382c,
	0x3d24c567, 0x3d2a6f22, 0x3d303584, 0x3d3618b7, 0x3d3c18e4, 0x3d423632, 0x3d4870ca, 0x3d4ec8d2,
	0x3d553e73, 0x3d5bd1d3, 0x3d628318, 0x3d69526a, 0x3d703fee, 0x3d774bca, 0x3d7e7624, 0x3d82df90,
	0x3d869372, 0x3d8a56cb, 0x3d8e29ab, 0x3d920c27, 0x3d95fe4f, 0x3d9a0035, 0x3d9e11ec, 0x3da23384,
	0x3da66510, 0x3daaa6a0, 0x3daef847, 0x3db35a15, 0x3db7cc1b, 0x3dbc4e6b, 0x3dc0e114, 0x3dc58429,
	0x3dca37b9, 0x3dcefbd6, 0x3dd3d08f, 0x3dd8b5f5, 0x3dddac19, 0x3de2b30a, 0x3de7cad9, 0x3decf395,
	0x3df22d50, 0x3df77817, 0x3dfcd3fc, 0x3e012087, 0x3e03dfae, 0x3e06a77b, 0x3e0977f6, 0x3e0c5126,
	0x3e0f3314, 0x3e121dc5, 0x3e151143, 0x3e180d95, 0x3e1b12c2, 0x3e1e20d1, 0x3e2137cb, 0x3e2457b6,
	0x3e278099, 0x3e2ab27d, 0x3e2ded68, 0x3e313161, 0x3e347e70, 0x3e37d49c, 0x3e3b33ec, 0x3e3e9c67,
	0x3e420e15, 0x3e4588fb, 0x3e490d22, 0x3e4c9a90, 0x3e50314c, 0x3e53d15d, 0x3e577aca, 0x3e5b2d9a,
	0x3e5ee9d4, 0x3e62af7e, 0x3e667e9f, 0x3e6a573e, 0x3e6e3962, 0x3e722511, 0x3e761a52, 0x3e7a192c,
	0x3e7e21a5, 0x3e8119e2, 0x3e8327c7, 0x3e853a86, 0x3e875222, 0x3e896e9d, 0x3e8b8ffc, 0x3e8db641,
	0x3e8fe170, 0x3e92118b, 0x3e944696, 0x3e968095, 0x3e98bf89, 0x3e9b0377, 0x3e9d4c62, 0x3e9f9a4c,
	0x3ea1ed38, 0x3ea4452b, 0x3ea6a226, 0x3ea9042e, 0x3eab6b44, 0x3eadd76d, 0x3eb048aa, 0x3eb2bf00,
	0x3eb53a71, 0x3eb7bb00, 0x3eba40b1, 0x3ebccb85, 0x3ebf5b81, 0x3ec1f0a7, 0x3ec48af9, 0x3ec72a7c,
	0x3ec9cf32, 0x3ecc791e, 0x3ecf2842, 0x3ed1dca2, 0x3ed49641, 0x3ed75521, 0x3eda1946, 0x3edce2b2,
	0x3edfb168, 0x3ee2856a, 0x3ee55ebd, 0x3ee83d63, 0x3eeb215d, 0x3eee0ab1, 0x3ef0f95f, 0x3ef3ed6b,
	0x3ef6e6d8, 0x3ef9e5a8, 0x3efce9de, 0x3efff37e, 0x3f018145, 0x3f030b82, 0x3f049877, 0x3f062827,
	0x3f07ba92, 0x3f094fb9, 0x3f0ae79f, 0x3f0c8244, 0x3f0e1faa, 0x3f0fbfd2, 0x3f1162be, 0x3f13086e,
	0x3f14b0e4, 0x3f165c22, 0x3f180a29, 0x3f19bafa, 0x3f1b6e96, 0x3f1d24ff, 0x3f1ede36, 0x3f209a3c,
	0x3f225913, 0x3f241abc, 0x3f25df38, 0x3f27a689, 0x3f2970af, 0x3f2b3dad, 0x3f2d0d83, 0x3f2ee032,
	0x3f30b5bd, 0x3f328e24, 0x3f346968, 0x3f36478b, 0x3f38288f, 0x3f3a0c73, 0x3f3bf33a, 0x3f3ddce5,
	0x3f3fc975, 0x3f41b8eb, 0x3f43ab48, 0x3f45a08f, 0x3f4798bf, 0x3f4993db, 0x3f4b91e3, 0x3f4d92d8,
	0x3f4f96bd, 0x3f519d92, 0x3f53a758, 0x3f55b411, 0x3f57c3be, 0x3f59d65f, 0x3f5bebf7, 0x3f5e0486,
	0x3f60200e, 0x3f623e90, 0x3f64600c, 0x3f668485, 0x3f68abfb, 0x3f6ad670, 0x3f6d03e5, 0x3f6f345a,
	0x3f7167d2, 0x3f739e4d, 0x3f75d7cc, 0x3f781451, 0x3f7a53dd, 0x3f7c9671, 0x3f7edc0e,
};
static uint32_t clearColorUnorm(uint32_t b, uint32_t n) {
	const uint64_t mx = (1ull << n) - 1ull;
	if (b >> 31) return 0;
	const uint32_t e = (b >> 23) & 0xffu, m = b & 0x7fffffu;
	if (e == 0xffu) return m != 0 ? 0u : static_cast<uint32_t>(mx);
	if (e >= 127u) return static_cast<uint32_t>(mx);
	if (e == 0) return 0;
	const uint32_t sh = 150u - e;
	if (sh > 63u) return 0;
	return static_cast<uint32_t>((static_cast<uint64_t>(m | 0x800000u) * mx + (1ull << (sh - 1u))) >> sh);
}
static uint32_t clearColorSnorm(uint32_t b, uint32_t n) {
	const uint32_t mag = clearColorUnorm(b & 0x7fffffffu, n - 1u);
	return ((b >> 31) ? (0u - mag) : mag) & ((1u << n) - 1u);
}
static uint32_t clearColorHalf(uint32_t b) {
	const uint32_t s = (b >> 16) & 0x8000u, m = b & 0x7fffffu;
	const int e = static_cast<int>((b >> 23) & 0xffu);
	if (e == 0xff) return s | 0x7c00u | (m != 0 ? 0x200u : 0u);
	const int ee = e - 112;
	if (ee >= 31) return s | 0x7c00u;
	if (ee <= 0) {
		if (ee < -10) return s;
		const uint32_t mm = m | 0x800000u, sh = static_cast<uint32_t>(14 - ee);
		uint32_t h = mm >> sh;
		const uint32_t rem = mm & ((1u << sh) - 1u), half = 1u << (sh - 1u);
		if (rem > half || (rem == half && (h & 1u))) h++;
		return s | h;
	}
	uint32_t h = (static_cast<uint32_t>(ee) << 10) | (m >> 13);
	const uint32_t rem = m & 0x1fffu;
	if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) h++;
	return s | h;
}
static uint32_t clearColorSrgb8(uint32_t b) {
	if ((b >> 31) || b == 0) return 0;
	if (((b >> 23) & 0xffu) == 0xffu && (b & 0x7fffffu) != 0) return 0;
	uint32_t lo = 0, hi = 255;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) / 2u;
		if (kClearColorSrgb8[mid] <= b) lo = mid + 1u; else hi = mid;
	}
	return lo;
}
static uint32_t clearColorUfloat(uint32_t b, uint32_t mb) {
	const int e = static_cast<int>((b >> 23) & 0xffu) - 127;
	const uint32_t m = b & 0x7fffffu, top = 0x1fu << mb;
	if (e == 128) return m != 0 ? (top | 1u) : ((b >> 31) ? 0u : top);
	if ((b >> 31) || e <= -15) return 0;
	if (e > 15) return top - 1u;
	const uint32_t sh = 23u - mb, v = (static_cast<uint32_t>(e + 15) << mb) + ((m + (1u << (sh - 1u))) >> sh);
	return v >= top ? top - 1u : v;
}
static bool clearColorPack(uint32_t fmt, const uint32_t c[4], uint32_t px[2]) {
	const uint32_t r = c[0], g = c[1], b = c[2], a = c[3];
	px[0] = px[1] = 0;
	switch (fmt) {
	case 0x140: px[0] = clearColorUnorm(r, 8); return true;
	case 0x144: px[0] = clearColorUnorm(a, 8); return true;
	case 0x106: px[0] = clearColorUnorm(r, 8) | clearColorUnorm(g, 8) << 8; return true;
	case 0x0c7: case 0x0eb:
		px[0] = clearColorUnorm(r, 8) | clearColorUnorm(g, 8) << 8 | clearColorUnorm(b, 8) << 16 | clearColorUnorm(a, 8) << 24; return true;
	case 0x0c8: case 0x0ec:
		px[0] = clearColorSrgb8(r) | clearColorSrgb8(g) << 8 | clearColorSrgb8(b) << 16 | clearColorUnorm(a, 8) << 24; return true;
	case 0x0c0: case 0x0e9:
		px[0] = clearColorUnorm(b, 8) | clearColorUnorm(g, 8) << 8 | clearColorUnorm(r, 8) << 16 | clearColorUnorm(a, 8) << 24; return true;
	case 0x0c1: case 0x0ea:
		px[0] = clearColorSrgb8(b) | clearColorSrgb8(g) << 8 | clearColorSrgb8(r) << 16 | clearColorUnorm(a, 8) << 24; return true;
	case 0x0c2: px[0] = clearColorUnorm(r, 10) | clearColorUnorm(g, 10) << 10 | clearColorUnorm(b, 10) << 20 | clearColorUnorm(a, 2) << 30; return true;
	case 0x0d1: px[0] = clearColorUnorm(b, 10) | clearColorUnorm(g, 10) << 10 | clearColorUnorm(r, 10) << 20 | clearColorUnorm(a, 2) << 30; return true;
	case 0x10a: px[0] = clearColorUnorm(r, 16); return true;
	case 0x0cc: px[0] = clearColorUnorm(r, 16) | clearColorUnorm(g, 16) << 16; return true;
	case 0x080: px[0] = clearColorUnorm(r, 16) | clearColorUnorm(g, 16) << 16; px[1] = clearColorUnorm(b, 16) | clearColorUnorm(a, 16) << 16; return true;
	case 0x10e: px[0] = clearColorHalf(r); return true;
	case 0x0d0: px[0] = clearColorHalf(r) | clearColorHalf(g) << 16; return true;
	case 0x084: px[0] = clearColorHalf(r) | clearColorHalf(g) << 16; px[1] = clearColorHalf(b) | clearColorHalf(a) << 16; return true;
	case 0x0d3: px[0] = clearColorUfloat(r, 6) | clearColorUfloat(g, 6) << 11 | clearColorUfloat(b, 5) << 22; return true;
	case 0x141: px[0] = clearColorSnorm(r, 8); return true;
	case 0x107: px[0] = clearColorSnorm(r, 8) | clearColorSnorm(g, 8) << 8; return true;
	case 0x0c9: px[0] = clearColorSnorm(r, 8) | clearColorSnorm(g, 8) << 8 | clearColorSnorm(b, 8) << 16 | clearColorSnorm(a, 8) << 24; return true;
	case 0x10b: px[0] = clearColorSnorm(r, 16); return true;
	case 0x0cd: px[0] = clearColorSnorm(r, 16) | clearColorSnorm(g, 16) << 16; return true;
	case 0x081: px[0] = clearColorSnorm(r, 16) | clearColorSnorm(g, 16) << 16; px[1] = clearColorSnorm(b, 16) | clearColorSnorm(a, 16) << 16; return true;
	case 0x142: case 0x143: px[0] = r & 0xffu; return true;
	case 0x108: case 0x109: px[0] = (r & 0xffu) | (g & 0xffu) << 8; return true;
	case 0x0ca: case 0x0cb: px[0] = (r & 0xffu) | (g & 0xffu) << 8 | (b & 0xffu) << 16 | (a & 0xffu) << 24; return true;
	case 0x10c: case 0x10d: px[0] = r & 0xffffu; return true;
	case 0x0ce: case 0x0cf: px[0] = (r & 0xffffu) | (g & 0xffffu) << 16; return true;
	case 0x082: case 0x083: px[0] = (r & 0xffffu) | (g & 0xffffu) << 16; px[1] = (b & 0xffffu) | (a & 0xffffu) << 16; return true;
	case 0x0c4: px[0] = (r & 0x3ffu) | (g & 0x3ffu) << 10 | (b & 0x3ffu) << 20 | (a & 3u) << 30; return true;
	default: return false;
	}
}
static inline uint32_t clearColorR32(const void *p, uint32_t off) {
	return *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(p) + off);
}
static void fillClearColor(const void *cmd, const void *desc, const void *heaps) {
	const uint32_t count = *reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(cmd) + 0xe);
	if (clearColorR32(cmd, 0x4) > 0x118u || count == 0 || count > 8u) return;
	volatile uint32_t *batch = *reinterpret_cast<volatile uint32_t *const *>(static_cast<const uint8_t *>(desc) + 0x20);
	const uint8_t *ssh = *reinterpret_cast<const uint8_t *const *>(static_cast<const uint8_t *>(heaps) + 0x58);
	if (batch == nullptr || ssh == nullptr) return;
	bool any = false;
	for (uint32_t i = 0; i < count && !any; i++) any = clearColorR32(cmd, 0x18u + i * 0x20u + 0x14u) != 0;
	if (!any) return;
	volatile uint32_t *s = batch + clearColorR32(cmd, 0x8);
	const uint32_t hdr = s[0];
	if (hdr != 0x10200407u) return;
	if (s[7] != 0 || s[8] != 0) return;
	const uint64_t addr = (static_cast<uint64_t>(s[2] & 0xffffu) << 32) | (s[1] & ~3u);
	if (addr == 0) return;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t eo = 0x18u + i * 0x20u;
		if (clearColorR32(cmd, eo + 0x14u) == 0) continue;
		const uint32_t *rss = reinterpret_cast<const uint32_t *>(ssh + clearColorR32(cmd, eo + 0xcu));
		if ((rss[6] & 7u) != 5u || (rss[10] & 0x400u) == 0) continue;
		if (((static_cast<uint64_t>(rss[13] & 0xffffu) << 32) | (rss[12] & ~0x3fu)) != addr) continue;
		const uint32_t fmt = (rss[0] >> 18) & 0x1ffu;
		const uint32_t raw[4] = {s[3], s[4], s[5], s[6]};
		uint32_t px[2];
		if (!clearColorPack(fmt, raw, px)) return;
		s[7] = px[0];
		s[8] = px[1];
		return;
	}
}

static void waitPageon(void *that, void *event) {
	if (event == nullptr) return;
	auto *accel = getMember<uint8_t *>(that, 0x18);
	if (accel == nullptr) return;
	auto *em = getMember<uint8_t *>(accel, 0x380);
	if (em == nullptr) return;
	const auto vptr = getMember<mach_vm_address_t>(em, 0);
	if (gEventMachineVtbl == 0 || vptr != gEventMachineVtbl + 0x10) return;
	const auto slot = *reinterpret_cast<const mach_vm_address_t *>(vptr + kPageonSlotTestEvent);
	if (slot == 0) return;
	auto test = reinterpret_cast<bool (*)(void *, void *)>(slot);
	if (test(em, event)) return;
	uint64_t deadline = 0;
	clock_interval_to_deadline(kPageonMaxUs, kMicrosecondScale, &deadline);
	bool done = false;
	for (;;) {
		IODelay(kPageonStepUs);
		if (test(em, event)) { done = true; break; }
		if (mach_absolute_time() >= deadline) break;
	}
	if (done) {
		gPageonTimeoutRun = 0;
	} else if (__sync_add_and_fetch(&gPageonTimeoutRun, 1u) >= kPageonAutoOffRun) {
		gPageonActive = 0;
		gPageonAutoOff = 1;
	}
}

static volatile uint32_t gNotReadyRun = 0;
static uint32_t surfaceAddr(const void *res) {
	if (res == nullptr) return 0;
	auto *r = static_cast<const uint8_t *>(res);
	auto *map = *reinterpret_cast<const uint8_t *const *>(r + 0x40);
	if (map == nullptr) return 0;
	uint64_t a = *reinterpret_cast<const uint64_t *>(map + 0x98);
	const uint8_t t = r[0x14];
	if (t == 0 || t == 0x80) a += *reinterpret_cast<const uint64_t *>(r + 0xF8);
	else if (t == 0x82) a += *reinterpret_cast<const uint32_t *>(r + 0xFC);
	return static_cast<uint32_t>(a);
}
uint64_t wrapSubmitFlipBuffer(void *pipe, void *ev, uint32_t flags, void *res, void *res2);
uint64_t wrapGenerateFlip(void *pipe, void *ev, uint32_t flags, void *res, void *res2);
uint64_t wrapBackBufferReady(void *surf, uint32_t fb);

static constexpr uint32_t kFlipSettleMs = 50, kNotReadyRunPolls = 8, kReflipGapMs = 250, kMaxReflips = 16;
static constexpr uint32_t kMaxFlipRequests = 256, kStuckFlipFbs = 4;
static void *volatile gFlipPipe[kStuckFlipFbs] = {};
static volatile uint32_t gFlipCacheMs[3] = {};
static volatile uint32_t gFlipRequests = 0, gReflips = 0;
static volatile uint32_t gNotReadyRunStartMs = 0, gLastReflipMs = 0, gInReflip = 0;

// The primary plane's surface base of a pipe, as i915's PLANE_SURF(pipe, PLANE_1) reads it (skl_universal_plane_regs.h).
static uint32_t readPlaneSurf(int32_t idx) {
	if (idx < 0 || idx > 2 || NGreen::callback == nullptr || !NGreen::callback->mmioMapped()) return 0;
	return NGreen::callback->readReg32(PLANE_SURF(static_cast<uint32_t>(idx), PLANE_1)) & PLANE_SURF_ADDR_MASK;
}
static bool displayPipeIdle(const uint8_t *p) {
	return *reinterpret_cast<const int32_t *>(p + 0x238) == *reinterpret_cast<const int32_t *>(p + 0x23c) &&
	       *reinterpret_cast<const uint64_t *>(p + 0x248) == 0;
}
static uint32_t fixDuplicateFlip(void *pipe, uint32_t est, uint32_t cache) {
	if (pipe == nullptr) return 0;
	auto *p = static_cast<uint8_t *>(pipe);
	const uint32_t fb = *reinterpret_cast<const uint32_t *>(p + 0x284);
	if (fb < kStuckFlipFbs) gFlipPipe[fb] = pipe;
	const int32_t idx = *reinterpret_cast<const int32_t *>(p + 0x1334);
	if (est == 0 || est != cache || idx < 0 || idx > 2 || p[0x298] == 0) return 0;
	const uint32_t surf = readPlaneSurf(idx);
	if (surf == 0 || surf == est) return 0;
	const uint32_t now = uptimeMs(), age = now - gFlipCacheMs[idx];
	const bool idle = displayPipeIdle(p) && age >= kFlipSettleMs;
	if (idle && gFlipRequests < kMaxFlipRequests) {
		*reinterpret_cast<uint64_t *>(p + 0x12c8) = surf;
		__sync_add_and_fetch(&gFlipRequests, 1u);
		return 1;
	}
	return 0;
}
extern mach_vm_address_t gOrgSubmitFlipBuffer, gOrgGenerateFlip, gOrgBackBufferReady;
static void reflipIfStuck(void *surf, uint32_t fb, uint32_t n) {
	if (gInReflip != 0 || n < kNotReadyRunPolls || gOrgSubmitFlipBuffer == 0) return;
	const uint32_t now = uptimeMs(), run = now - gNotReadyRunStartMs;
	if (run < kFlipSettleMs || now - gLastReflipMs < kReflipGapMs) return;
	void *pipe = fb < kStuckFlipFbs ? gFlipPipe[fb] : nullptr;
	if (pipe == nullptr) return;
	auto *pp = static_cast<uint8_t *>(pipe);
	auto *s = static_cast<const uint8_t *>(surf);
	void *front = *reinterpret_cast<void *const *>(s + 0x11c0);
	const uint32_t fa = surfaceAddr(front), ba = surfaceAddr(*reinterpret_cast<void *const *>(s + 0x11c8));
	const int32_t idx = *reinterpret_cast<const int32_t *>(pp + 0x1334);
	const uint32_t reg = readPlaneSurf(idx);
	if (pp[0x298] == 0 || fa == 0 || ba == 0 || fa == ba || reg != ba) return;
	if (!displayPipeIdle(pp) || gReflips >= kMaxReflips) return;
	gLastReflipMs = now;
	const uint32_t cache = static_cast<uint32_t>(*reinterpret_cast<const uint64_t *>(pp + 0x12c8));
	if (cache == fa) *reinterpret_cast<uint64_t *>(pp + 0x12c8) = reg;
	__sync_add_and_fetch(&gReflips, 1u);
	gInReflip = 1;
	FunctionCast(wrapSubmitFlipBuffer, gOrgSubmitFlipBuffer)(pipe, nullptr, 0, front, nullptr);
	gInReflip = 0;
	gFlipCacheMs[idx] = uptimeMs();
}

// From i915, drivers/gpu/drm/i915/display/intel_ddi.c, intel_ddi_prepare_link_retrain: an SST link with enhanced framing gets
// DP_TP_CTL_ENHANCED_FRAME_ENABLE in DP_TP_CTL before training, with a posting read. i915 sets it once from the CRTC state; here it
// is set after each pattern write of Apple's trainer, which never sets it (the sink's capability is in the DPCD Apple reads).
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L3814-L3844
IOReturn Gen11::wrapSetLinkTrainingPattern(void *that, uint32_t pattern) {
	const IOReturn ret = FunctionCast(wrapSetLinkTrainingPattern,
	    callback->oSetLinkTrainingPattern)(that, pattern);
	auto *cb = NGreen::callback;
	if (cb == nullptr || that == nullptr) return ret;

	const uint32_t trans = getMember<uint32_t>(that, 0x3640);
	if (trans > 3u) return ret;
	const uint32_t reg = _MMIO_TRANS2(trans, TGL_DP_TP_CTL_A);
	const uint32_t dp_tp_ctl = cb->readReg32(reg);
	if (!cb->mmioMapped()) return ret;
	if ((dp_tp_ctl & DP_TP_CTL_ENHANCED_FRAME_ENABLE) == 0) cb->writeReg32(reg, dp_tp_ctl | DP_TP_CTL_ENHANCED_FRAME_ENABLE);
	cb->readReg32(reg);
	return ret;
}

static bool gAuxSeen = false;

// From i915, drivers/gpu/drm/i915/display/intel_vdsc.c: intel_dsc_pps_write (one value to every VDSC engine of the pipe),
// intel_dsc_pps_configure (PPS 0-10 and 16, the RC buffer thresholds, the RC range parameters, in that order), intel_dsc_enable (the
// configuration, then DSS_CTL1 and DSS_CTL2) and intel_dsc_disable (DSS_CTL1 and DSS_CTL2 cleared). The values are Apple's, latched from
// its CRTC parameters (i915 computes them from drm_dsc_config); the engine count comes off Apple's DSS_CTL2 as i915 takes it off the
// state (vdsc_per_pipe). The registers: display/intel_vdsc_regs.h (pipe A's block sits 0x200 below pipe B's, DSC1's 0x100 above
// DSC0's); the pipe and DDI enables: display/intel_display_regs.h.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc.c#L505-L521
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc.c#L523-L778
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc.c#L875-L926
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc_regs.h#L14-L81
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc_regs.h#L216-L224
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_vdsc_regs.h#L267-L275
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L869-L873
enum pipe { PIPE_A = 0, PIPE_B, PIPE_C, PIPE_D };	/* display/intel_display_limits.h */
static constexpr uint32_t _ICL_PIPE_DSS_CTL1_PB = 0x78200u, _ICL_PIPE_DSS_CTL1_PC = 0x78400u;
static constexpr uint32_t _ICL_PIPE_DSS_CTL2_PB = 0x78204u, _ICL_PIPE_DSS_CTL2_PC = 0x78404u;
#define ICL_PIPE_DSS_CTL1(pipe)	_PICK_EVEN((pipe) - PIPE_B, _ICL_PIPE_DSS_CTL1_PB, _ICL_PIPE_DSS_CTL1_PC)
#define ICL_PIPE_DSS_CTL2(pipe)	_PICK_EVEN((pipe) - PIPE_B, _ICL_PIPE_DSS_CTL2_PB, _ICL_PIPE_DSS_CTL2_PC)
static constexpr uint32_t JOINER_ENABLE = 1u << 30;
static constexpr uint32_t VDSC0_ENABLE  = 1u << 31;
static constexpr uint32_t VDSC1_ENABLE  = 1u << 15;
static constexpr uint32_t _ICL_DSC0_PICTURE_PARAMETER_SET_0_PB = 0x78270u, _ICL_DSC0_PICTURE_PARAMETER_SET_0_PC = 0x78470u;
static constexpr uint32_t _ICL_DSC1_PICTURE_PARAMETER_SET_0_PB = 0x78370u;
#define _ICL_DSC0_PPS_0(pipe)	_PICK_EVEN((pipe) - PIPE_B, _ICL_DSC0_PICTURE_PARAMETER_SET_0_PB, _ICL_DSC0_PICTURE_PARAMETER_SET_0_PC)
#define ICL_DSC0_PPS(pipe, pps)	(_ICL_DSC0_PPS_0(pipe) + ((pps) * 4))
static constexpr uint32_t _ICL_DSC0_RC_BUF_THRESH_0_PB = 0x78254u, _ICL_DSC0_RC_BUF_THRESH_0_PC = 0x78454u;
#define ICL_DSC0_RC_BUF_THRESH(pipe, n)	(_PICK_EVEN((pipe) - PIPE_B, _ICL_DSC0_RC_BUF_THRESH_0_PB, _ICL_DSC0_RC_BUF_THRESH_0_PC) + (n) * 4)
static constexpr uint32_t _ICL_DSC0_RC_RANGE_PARAMETERS_0_PB = 0x78208u, _ICL_DSC0_RC_RANGE_PARAMETERS_0_PC = 0x78408u;
#define ICL_DSC0_RC_RANGE_PARAMETERS(pipe, n)	(_PICK_EVEN((pipe) - PIPE_B, _ICL_DSC0_RC_RANGE_PARAMETERS_0_PB, _ICL_DSC0_RC_RANGE_PARAMETERS_0_PC) + (n) * 4)
static constexpr uint32_t kVdscDsc1Offset = _ICL_DSC1_PICTURE_PARAMETER_SET_0_PB - _ICL_DSC0_PICTURE_PARAMETER_SET_0_PB;
static constexpr uint32_t _TRANSACONF = 0x70008u;
static constexpr uint32_t TRANSCONF_ENABLE = 1u << 31, TRANSCONF_STATE_ENABLE = 1u << 30;

static volatile uint32_t gDssClearDone     = 0;

static bool vdscFlushInForce();

static void intel_dsc_disable() {
	auto *cb = NGreen::callback;
	cb->writeReg32(ICL_PIPE_DSS_CTL1(PIPE_A), 0);
	cb->writeReg32(ICL_PIPE_DSS_CTL2(PIPE_A), 0);
}

static void clearDssOnce() {
	if (vdscFlushInForce()) return;
	if (__sync_val_compare_and_swap(&gDssClearDone, 0u, 1u) != 0u) return;
	if (NGreen::callback == nullptr) return;
	intel_dsc_disable();
}

static void clearVdscGatePipeOff() {
	auto *cb = NGreen::callback;
	if (cb == nullptr || !cb->mmioMapped()) return;
	const uint32_t conf = cb->readReg32(_TRANSACONF);
	const uint32_t func = cb->readReg32(_TRANS_DDI_FUNC_CTL_A);
	const uint32_t p1   = cb->readReg32(ICL_PIPE_DSS_CTL1(PIPE_A));
	const uint32_t p2   = cb->readReg32(ICL_PIPE_DSS_CTL2(PIPE_A));
	const bool pipeOff  = (conf & (TRANSCONF_ENABLE | TRANSCONF_STATE_ENABLE)) == 0u && (func & TRANS_DDI_FUNC_ENABLE) == 0u;
	const bool gateSet  = (p1 & JOINER_ENABLE) != 0u || (p2 & (VDSC0_ENABLE | VDSC1_ENABLE)) != 0u;
	if (gateSet && pipeOff) intel_dsc_disable();
}

static constexpr uint32_t kVdscRegs = 24, kVdscDwords = 26, kVdscPpsRegs = 12;
static constexpr size_t kVdscParamsDsc = 0x88, kVdscPathPipe = 0x3640, kVdscPathPort = 0x3648;
static constexpr size_t kVdscPortDsc = 0x11c;
// Apple's 26 DSC dwords, in the order intel_dsc_pps_configure writes them: PPS 0-10 and 16, the four RC buffer thresholds, the eight RC
// range parameter words, DSS_CTL1, DSS_CTL2.
static const uint32_t kVdscReg[kVdscRegs] = {
	ICL_DSC0_PPS(PIPE_A, 0), ICL_DSC0_PPS(PIPE_A, 1), ICL_DSC0_PPS(PIPE_A, 2), ICL_DSC0_PPS(PIPE_A, 3), ICL_DSC0_PPS(PIPE_A, 4),
	ICL_DSC0_PPS(PIPE_A, 5), ICL_DSC0_PPS(PIPE_A, 6), ICL_DSC0_PPS(PIPE_A, 7), ICL_DSC0_PPS(PIPE_A, 8), ICL_DSC0_PPS(PIPE_A, 9),
	ICL_DSC0_PPS(PIPE_A, 10), ICL_DSC0_PPS(PIPE_A, 16),
	ICL_DSC0_RC_BUF_THRESH(PIPE_A, 0), ICL_DSC0_RC_BUF_THRESH(PIPE_A, 1), ICL_DSC0_RC_BUF_THRESH(PIPE_A, 2), ICL_DSC0_RC_BUF_THRESH(PIPE_A, 3),
	ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 0), ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 1), ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 2),
	ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 3), ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 4), ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 5),
	ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 6), ICL_DSC0_RC_RANGE_PARAMETERS(PIPE_A, 7),
};
static volatile uint32_t gVdscRouted = 0, gVdscFlushed = 0;
static volatile uint32_t gVdscLatchOn = 0;
static uint32_t gVdscLatchDw[kVdscDwords];
static const void *gVdscLatchPath = nullptr, *gVdscFlushPath = nullptr;

static bool vdscFlushInForce() {
	return gVdscFlushed != 0u;
}

static void vdscLatch(const void *params, const void *path) {
	const uint32_t *src = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(params) + kVdscParamsDsc);
	for (uint32_t i = 0; i < kVdscDwords; i++) gVdscLatchDw[i] = src[i];
	const uint32_t on = (gVdscLatchDw[24] | gVdscLatchDw[25]) != 0u ? 1u : 0u;
	gVdscLatchPath = path;
	gVdscLatchOn = on;
}

static void vdscClearDss() {
	intel_dsc_disable();
	gVdscFlushed = 0;
	gVdscFlushPath = nullptr;
}

static int intel_dsc_get_vdsc_per_pipe(uint32_t dss_ctl2) {
	return (dss_ctl2 & VDSC1_ENABLE) != 0u ? 2 : 1;
}

static void intel_dsc_pps_write(uint32_t reg, uint32_t pps_val, int vdsc_per_pipe) {
	for (int i = 0; i < vdsc_per_pipe; i++)
		NGreen::callback->writeReg32(reg + static_cast<uint32_t>(i) * kVdscDsc1Offset, pps_val);
}

static void intel_dsc_pps_configure(const uint32_t *dw, int vdsc_per_pipe) {
	// Apple hands RC rows of zero for some modes; they are then left to the hardware (i915 always computes and writes its own).
	bool rcZero = true;
	for (uint32_t i = kVdscPpsRegs; i < kVdscRegs; i++) rcZero = rcZero && dw[i] == 0u;
	for (uint32_t i = 0; i < kVdscRegs; i++) {
		if (i >= kVdscPpsRegs && rcZero) continue;
		intel_dsc_pps_write(kVdscReg[i], dw[i], vdsc_per_pipe);
	}
}

static void intel_dsc_enable(const uint32_t *dw) {
	auto *cb = NGreen::callback;
	const int vdsc_instances_per_pipe = intel_dsc_get_vdsc_per_pipe(dw[25]);
	intel_dsc_pps_configure(dw, vdsc_instances_per_pipe);
	cb->writeReg32(ICL_PIPE_DSS_CTL1(PIPE_A), dw[24]);
	cb->writeReg32(ICL_PIPE_DSS_CTL2(PIPE_A), dw[25]);
	static volatile uint32_t rows = 0;
	if (__sync_add_and_fetch(&rows, 1u) <= 2u)
		SYSLOG("ngreen", "VDSC pipe A enabled as intel_dsc_enable: %d engine(s), PPS0 0x%x, RC rows %s, DSS_CTL1 0x%x DSS_CTL2 0x%x",
		       vdsc_instances_per_pipe, dw[0], dw[kVdscPpsRegs] == 0u && dw[kVdscPpsRegs + 1] == 0u ? "left to the hardware" : "written",
		       cb->readReg32(ICL_PIPE_DSS_CTL1(PIPE_A)), cb->readReg32(ICL_PIPE_DSS_CTL2(PIPE_A)));
}

static void vdscEnable(void *path) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || cb->getIsRealTGL() || path == nullptr || gVdscRouted != 3u || !cb->mmioMapped()) return;
	void *port = getMember<void *>(path, kVdscPathPort);
	const uint32_t pipe = getMember<uint32_t>(path, kVdscPathPipe);
	const bool portOn = port != nullptr && getMember<uint8_t>(port, kVdscPortDsc) != 0;
	const bool latchOn = gVdscLatchOn != 0u && gVdscLatchPath == path;
	if (!latchOn && !portOn) {
		if (gVdscFlushed != 0u && gVdscFlushPath == path) vdscClearDss();
		return;
	}
	if (!latchOn || !portOn || pipe != 0u) return;
	intel_dsc_enable(gVdscLatchDw);
	gVdscFlushPath = path;
	gVdscFlushed = 1;
}

static void vdscDisable(void *path) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || gVdscFlushed == 0u || path != gVdscFlushPath || !cb->mmioMapped()) return;
	vdscClearDss();
}

static constexpr mach_vm_address_t kAgdcNubDoAttributeOff = 0xe6db4;
static const uint8_t kAgdcNubPrologue[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48};
static constexpr uint32_t kAgdcNubRegisterCallback = 0x923;
enum : uint32_t { kAgdcNubOff = 0, kAgdcNubWaiting = 1, kAgdcNubDone = 2 };
static volatile uint32_t gAgdcNubState = kAgdcNubOff;
static volatile bool gAgdcNubRouted = false;
static void *gAgdcNubController = nullptr;
static mach_vm_address_t gAgdcNubOrgCreate = 0, gAgdcNubOrgDoAttribute = 0;
static uint32_t agdcCallbackStub(void *ctx, uint64_t arg, uint32_t flags, uint32_t code);

static IOReturn wrapAgdcDoAttribute(void *nub, uint32_t cmd, unsigned long *in, unsigned long inSize, unsigned long *out,
                                unsigned long *outSize, void *args) {
	if (cmd == kAgdcNubRegisterCallback) return kIOReturnUnsupported;
	return FunctionCast(wrapAgdcDoAttribute, gAgdcNubOrgDoAttribute)(nub, cmd, in, inSize, out, outSize, args);
}

static bool agdcWranglerMatched(void *, void *, IOService *, IONotifier *) {
	if (__sync_bool_compare_and_swap(&gAgdcNubState, kAgdcNubWaiting, kAgdcNubDone))
		reinterpret_cast<uint64_t (*)(void *)>(gAgdcNubOrgCreate)(gAgdcNubController);
	return true;
}

static void armAgdcNub(void *controller, mach_vm_address_t org) {
	if (!__sync_bool_compare_and_swap(&gAgdcNubState, kAgdcNubOff, kAgdcNubWaiting)) return;
	gAgdcNubController = controller;
	gAgdcNubOrgCreate = org;
	if (controller == nullptr || org == 0 || !gAgdcNubRouted) {
		gAgdcNubState = kAgdcNubDone;
		return;
	}
	OSDictionary *match = IOService::serviceMatching("AppleGPUWrangler");
	IONotifier *notifier = match != nullptr
	    ? IOService::addMatchingNotification(gIOMatchedNotification, match, agdcWranglerMatched, nullptr, nullptr, 0)
	    : nullptr;
	OSSafeReleaseNULL(match);
	if (notifier == nullptr) __sync_bool_compare_and_swap(&gAgdcNubState, kAgdcNubWaiting, kAgdcNubDone);
}


static bool ensureA788DisplayAWells(bool requireDDI = true);
static void scanoutSnapshot(bool late) {
	auto *cb = NGreen::callback;
	if (cb == nullptr) return;
	if (__sync_bool_compare_and_swap(&gImrFixDone, 0u, 1u)) {
		const uint32_t imrPre = cb->readReg32(GEN8_DE_PIPE_IMR_A);
		if (imrPre & GEN8_PIPE_FIFO_UNDERRUN) cb->writeReg32(GEN8_DE_PIPE_IMR_A, imrPre & ~GEN8_PIPE_FIFO_UNDERRUN);
	}
	if (late && cb->isRaptorLakeHX() && gAuxSeen)
		ensureA788DisplayAWells(false);
	clearDssOnce();
}

static uint32_t uptimeMs() {
	uint64_t abs = 0, ns = 0;
	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return (uint32_t)(ns / 1000000ull);
}

static constexpr uint64_t kResolvePoolWindow = 0xff8;
static constexpr uint64_t kResolvePoolBufApple = 0x1000, kResolvePoolBufBlit3D = 0x10000;
static void bumpResolveBufferSize(void *params) {
	auto *sz = reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(params) + 0x20);
	if (*sz == kResolvePoolBufApple) *sz = kResolvePoolBufBlit3D;
}
static void reserveResolveWindow(void *ctx) {
	auto *pool = getMember<uint8_t *>(ctx, 0xe0);
	if (pool == nullptr) return;
	const uint64_t seg = *reinterpret_cast<volatile uint64_t *>(pool + 0x1848);
	const uint64_t lim = *reinterpret_cast<volatile uint64_t *>(pool + 0x1850);
	const uint64_t ptr = *reinterpret_cast<volatile uint64_t *>(pool + 0x1858);
	if (ptr == 0 || lim == 0) return;
	const uint64_t roomPre = lim > ptr ? lim - ptr : 0;
	if (roomPre >= kResolvePoolWindow) return;
	const bool pending = ptr > seg;
	auto *cb = Gen11::callback;
	if (cb == nullptr || cb->oIOAF2_getBufferPtrNoInc == 0) return;
	if (pending) {
		auto *ch = getMember<uint8_t *>(pool, 0x20);
		if (ch != nullptr) {
			using IncrementStamp = void (*)(void *);
			const auto vt = *reinterpret_cast<uintptr_t *>(ch);
			reinterpret_cast<IncrementStamp>(*reinterpret_cast<uintptr_t *>(vt + 0x148))(ch);
		}
	}
	using GetBufferPtrNoInc = uint32_t *(*)(void *, uint32_t);
	reinterpret_cast<GetBufferPtrNoInc>(cb->oIOAF2_getBufferPtrNoInc)(pool, static_cast<uint32_t>(kResolvePoolWindow / 4));
}

static void scanoutLate(thread_call_param_t, thread_call_param_t) {
	const uint32_t n = __sync_add_and_fetch(&gScanoutLateFires, 1u);
	if (n <= 3) scanoutSnapshot(true);
	if (n < 3 && gScanoutLateCall != nullptr) {
		uint64_t deadline = 0;
		clock_interval_to_deadline(10000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gScanoutLateCall, deadline);
	}
}

static void scanoutArmLate() {
	if (gScanoutLateCall == nullptr)
		gScanoutLateCall = thread_call_allocate_with_priority(scanoutLate, nullptr,
		                                                  THREAD_CALL_PRIORITY_KERNEL);
	if (gScanoutLateCall == nullptr) return;
	uint64_t deadline = 0;
	clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	thread_call_enter_delayed(gScanoutLateCall, deadline);
}

static bool isKnownTGLImage(mach_vm_address_t address, size_t size) {
	static const uint8_t kKnown[][16] = {
		{0xb9, 0x6a, 0x84, 0x48, 0x9d, 0x17, 0x3f, 0x78, 0xb7, 0x6d, 0x15, 0xa0, 0xb3, 0x59, 0xa9, 0xa0},
		{0xba, 0x3a, 0xa1, 0xc0, 0xfe, 0x6b, 0x33, 0xb3, 0x9d, 0x85, 0x73, 0xf8, 0x48, 0x39, 0x4e, 0x3d},
	};
	if (address == 0 || size < sizeof(mach_header_64)) return false;
	const auto *mh = reinterpret_cast<const mach_header_64 *>(address);
	if (mh->magic != MH_MAGIC_64 || sizeof(*mh) + mh->sizeofcmds > size) return false;
	const auto *lc = reinterpret_cast<const uint8_t *>(mh + 1), *end = lc + mh->sizeofcmds;
	for (uint32_t i = 0; i < mh->ncmds && lc + sizeof(load_command) <= end; i++) {
		const auto *cmd = reinterpret_cast<const load_command *>(lc);
		if (cmd->cmdsize < sizeof(load_command) || lc + cmd->cmdsize > end) return false;
		if (cmd->cmd == LC_UUID && cmd->cmdsize >= sizeof(uuid_command)) {
			const auto *u = reinterpret_cast<const uuid_command *>(lc);
			for (uint32_t j = 0; j < arrsize(kKnown); j++)
				if (memcmp(u->uuid, kKnown[j], sizeof(kKnown[j])) == 0) return true;
			return false;
		}
		lc += cmd->cmdsize;
	}
	return false;
}

template <size_t N>
static bool routeTGLDebugOffsets(KernelPatcher &patcher, RouteRequestPlus (&requests)[N],
	const mach_vm_address_t (&offsets)[N], mach_vm_address_t address, size_t size) {
	if (!isKnownTGLImage(address, size)) return false;
	for (size_t i = 0; i < N; i++) {
		if (offsets[i] >= size) return false;
		const auto from = address + offsets[i];
		patcher.eraseCoverageInstPrefix(from, 5, 14);
		const bool needsWrapper = requests[i].org != nullptr;
		auto original = patcher.routeFunction(from, requests[i].to, needsWrapper);
		if (patcher.getError() != KernelPatcher::Error::NoError || (needsWrapper && original == 0)) {
			patcher.clearError();
			return false;
		}

		if (needsWrapper)
			*requests[i].org = original;
	}
	return true;
}

static void routeGtClock(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	if (kPmNotifyWrapperOff + sizeof(kGtClockPrologue) > size || memcmp(reinterpret_cast<const void *>(address + kPmNotifyWrapperOff), kGtClockPrologue, sizeof(kGtClockPrologue)) != 0) return;
	RouteRequestPlus requests[] = {{"__ZL15pmNotifyWrapperjjPyPj", answerPmNotify}};
	static constexpr mach_vm_address_t offsets[] = {kPmNotifyWrapperOff};
	if (!routeTGLDebugOffsets(patcher, requests, offsets, address, size)) return;
	gtClockSetActive(1);
}

static void routeAgdcDoAttribute(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	if (kAgdcNubDoAttributeOff + sizeof(kAgdcNubPrologue) > size || memcmp(reinterpret_cast<const void *>(address + kAgdcNubDoAttributeOff), kAgdcNubPrologue, sizeof(kAgdcNubPrologue)) != 0) return;
	RouteRequestPlus requests[] = {
		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", wrapAgdcDoAttribute, gAgdcNubOrgDoAttribute},
	};
	static constexpr mach_vm_address_t offsets[] = {kAgdcNubDoAttributeOff};
	gAgdcNubRouted = routeTGLDebugOffsets(patcher, requests, offsets, address, size);
}

static constexpr mach_vm_address_t kReadDpbResidencyOff = 0xbc8d4;
static const uint8_t kDpbPrologue[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x10, 0x41, 0x89, 0xf6};
static mach_vm_address_t gOrgReadDpbResidency = 0;

static uint64_t wrapReadDpbResidency(void *tcon, uint32_t type) {
	if (tcon == nullptr) return 0;
	return FunctionCast(wrapReadDpbResidency, gOrgReadDpbResidency)(tcon, type);
}

static void routeDpbResidency(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	if (!gAgdcNubRouted) return;
	if (kReadDpbResidencyOff + sizeof(kDpbPrologue) > size || memcmp(reinterpret_cast<const void *>(address + kReadDpbResidencyOff), kDpbPrologue, sizeof(kDpbPrologue)) != 0) return;
	RouteRequestPlus requests[] = {
		{"__ZN12CamelliaBase16ReadDpbResidencyE10kDpbType_t", wrapReadDpbResidency, gOrgReadDpbResidency},
	};
	static constexpr mach_vm_address_t offsets[] = {kReadDpbResidencyOff};
	routeTGLDebugOffsets(patcher, requests, offsets, address, size);
}

void Gen11::routeAUXTransactionGuard(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	RouteRequestPlus requests[] = {
		{"__ZN17AppleIntelPortHAL13runAUXCommandEN14AppleIntelPort11AUX_CommandEPjjb", wrapRunAUXCommand, callback->orgRunAUXCommand},
	};
	static constexpr mach_vm_address_t requestOffsets[] = {
		0x91B64,
	};
	PANIC_COND(!routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size),
		"ngreen", "Failed to route TGL AUX transaction guard");
}

void Gen11::routeEnhancedFraming(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	RouteRequestPlus requests[] = {
		{"__ZN21AppleIntelDisplayPath22setLinkTrainingPatternE19LinkTrainingPattern",
		 wrapSetLinkTrainingPattern, callback->oSetLinkTrainingPattern},
	};
	static constexpr mach_vm_address_t requestOffsets[] = {
		0xEE060,
	};
	PANIC_COND(!routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size),
		"ngreen", "Failed to route TGL enhanced framing");
}

void Gen11::routePixelInformation(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
	RouteRequestPlus requests[] = {
		{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation", fbGetPixelInformation, callback->ofbGetPixelInformation},
	};
	static constexpr mach_vm_address_t requestOffsets[] = {
		0x498BC,
	};
	routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size);
}

void Gen11::init() {
	callback = this;

	if (checkKernelArgument("-ngreentglfb") || checkKernelArgument("-ngreentglwithgfx")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → TGL (ICL FB skipped)");
		lilu.onKextLoadForce(&kextG11FBT);
		lilu.onKextLoadForce(&kextG11FBTA);
		if (checkKernelArgument("-ngreentglwithgfx")) {
			SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
			lilu.onKextLoadForce(&kextG11HWT);
			lilu.onKextLoadForce(&kextG11HWTA);
		}
	} else if (checkKernelArgument("-ngreentglgfx")) {
		SYSLOG("ngreen", "Gen11::init: HW tier → TGL (ICL HW skipped)");
		lilu.onKextLoadForce(&kextG11HWT);
		lilu.onKextLoadForce(&kextG11HWTA);
	} else if (checkKernelArgument("-ngreenicl")) {
		SYSLOG("ngreen", "Gen11::init: FB tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11FB);
		SYSLOG("ngreen", "Gen11::init: HW tier → ICL fallback");
		lilu.onKextLoadForce(&kextG11HW);
	}
}

static bool isWEGCoexistMode() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngwegcoex", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngwegcoex");
}

static bool isExperimentalMonitorEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenexp", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenexp");
}

static bool isV60MonitorEnabled() {
	int forceOff = 0;
	if (PE_parse_boot_argn("ngreenv60off", &forceOff, sizeof(forceOff))) {
		if (forceOff != 0)
			return false;
	}

	if (checkKernelArgument("-ngreenv60off")) {
		return false;
	}

	int forceOn = 0;
	if (PE_parse_boot_argn("ngreenv60", &forceOn, sizeof(forceOn))) {
		if (forceOn != 0)
			return true;
	}

	if (checkKernelArgument("-ngreenv60")) {
		return true;
	}

	return isExperimentalMonitorEnabled();
}

static bool isDisplayPipeForceDisabled() {
	// NOTE: isLegacyOwnershipModeEnabled() (V80 plane-linearization) is intentionally
	// NOT checked here. V80 writes are self-limiting to the first 3 ticks (≤150ms) and
	// must not prevent WindowServer from opening the display pipe after that window.
	// Coupling these two features caused DisplayPipeSupported=0 → black screen forever.

	// Stage-3 baseline now has DYLD-side NULL guards for DisplayPipe path.
	// Keep native DisplayPipe ON by default and use ngreendp0 only as an explicit
	// fallback switch when troubleshooting.
	int nativeDisplayPipe = 0;
	if (PE_parse_boot_argn("ngreendp1", &nativeDisplayPipe, sizeof(nativeDisplayPipe))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp1=%d", nativeDisplayPipe);
		if (nativeDisplayPipe != 0)
			return false;
	}

	if (checkKernelArgument("-ngreendp1")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp1");
		return false;
	}

	int enabled = 0;
	if (PE_parse_boot_argn("ngreendp0", &enabled, sizeof(enabled))) {
		SYSLOG("ngreen", "V78A: parsed ngreendp0=%d", enabled);
		return enabled != 0;
	}

	if (checkKernelArgument("-ngreendp0")) {
		SYSLOG("ngreen", "V78A: detected -ngreendp0");
		return true;
	}

	static bool v78aLogged = false;
	if (!v78aLogged) {
		v78aLogged = true;
		SYSLOG("ngreen", "V78A (capped): default native DisplayPipeSupported ON");
	}
	return false;
}

static bool isV93PlaneGuardEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv93", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv93");
}

static bool shouldForceFullMetalPath() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenfullmtlcore", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	if (checkKernelArgument("-ngreenfullmtlcore")) {
		return true;
	}
	// Legacy: unified arg still accepted as fallback.
	if (PE_parse_boot_argn("ngreenfullmtl", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}
	return checkKernelArgument("-ngreenfullmtl");
}

static int getV142SubmitBlitMode() {
	int parsed = 0;
	if (PE_parse_boot_argn("ngreenV142", &parsed, sizeof(parsed))) {
		return parsed;
	}

	if (checkKernelArgument("-ngreenV142orig"))
		return 3;
	if (checkKernelArgument("-ngreenV142pass"))
		return 2;
	if (checkKernelArgument("-ngreenV142ok"))
		return 1;
	if (checkKernelArgument("-ngreenV142hardunsupported"))
		return 0;
	if (checkKernelArgument("-ngreenV142unsupported"))
		return 1;

	// V170: RPL (and any non-TGL spoof) cannot execute the TGL-format 3D blit commands that
	// the original IGAccelBlit::submitBlit generates — the RCS EU stalls indefinitely
	// (INSTDONE=0xfffffffe, bit-0 stuck) causing an infinite hangcheck→reset→retry loop
	// that kills WindowServer within 120 s. Default to mode 1 (return 0 = success, no-op)
	// so the compositor initialises without hanging. Real TGL hardware still uses mode 3.
	// Override with -ngreenV142orig or ngreenV142=3 to restore original blit submission.
	return 1;
}

static bool isV142Diag2DBypassEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenV142diag2d", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenV142diag2d");
}

static bool isV88ScanoutFillEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv88", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv88");
}

static bool isV60AggressiveMonitorEnabled() {
	int enabled = 0;
	if (PE_parse_boot_argn("ngreenv60rw", &enabled, sizeof(enabled))) {
		return enabled != 0;
	}

	return checkKernelArgument("-ngreenv60rw");
}

static bool isV80LEnforcerEnabled() {
	return checkKernelArgument("-ngreenv80l");
}

static uint32_t getV65Tier1WantBits(bool isRealTGL) {
	// Real TGL: always use native behavior (RCS + BCS).
	if (isRealTGL) {
		return (1u << GEN11_RCS0) | (1u << GEN11_BCS);
	}

	// RPL/ADL spoof path: BCS must be enabled when the original submitBlit path
	// (V142 mode 3) is active — that path submits directly to BCS ring. Leaving
	// BCS out of the want bits means RING_CTRL never gets the enable bit set,
	// commands queue up and the ring stalls (gpuRestart Signature 801).
	// For other V142 modes (0/1/2 = bypass/return0) BCS is idle, keep RCS-only.
	if (getV142SubmitBlitMode() == 3 || checkKernelArgument("-ngreenbcsirq")) {
		return (1u << GEN11_RCS0) | (1u << GEN11_BCS);
	}

	return (1u << GEN11_RCS0);
}

void Gen11::routeResolveInit(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	RouteRequestPlus route[] = {
		{"__Z19resolve_init_ctx_g7P25IOAccelCommandBufferPool2P14IGMappedBufferyb", wrapResolveInit, gOrgResolveInit},
	};
	if (!RouteRequestPlus::routeAll(patcher, index, route, address, size)) patcher.clearError();
}

// After a restart, beginReset gives the hung RCS context the accelerator's safe image, so a resolve context loses the state its
// preamble set once, at creation (resolve_init_ctx_g7: URB and L3 allocation, SIP, the state pointers and the disabled stages); its
// next resolves then stall in VF / VS. Such a context is initialised again before its next use, as at creation.
static mach_vm_address_t gOrgBeginReset = 0, gResolveCtxInitialize = 0, gSchedPriority = 0;
static constexpr mach_vm_address_t kSchedPrioritySize = 0xfa;
static constexpr uint64_t kResolvePreamble = 0x9c0;	// resolve_init_ctx_g7's 0x9b8 bytes + the batch end the init wrapper adds
static void *volatile gResolveCtxReset[16];
// The safe image beginReset restores was saved from one of the accelerator's own POCS contexts, and beginReset writes back only
// the page-table pointers: the per-context writes initWithOptions makes after the same restore (0x7c15e-0x7c343) are left at the golden
// context's values — CONTEXT_CONTROL bit 4 (the POCS enable, set for the golden, cleared for a resolve / Blit3D context), the POCS image
// pointer at +0x10d8 (the golden's, a buffer released at start-up; a POCS context's own otherwise), the 0x80008 bits, the task's AUX table
// base, +0x110c, the telemetry patch and ctx+0xd0. They are repeated here, in Apple's order, for every RCS context the restore touched.
static mach_vm_address_t gSharedBufferVA = 0, gMappedGpuVA = 0, gPatchCtxImageRCS = 0;
static void reapplyContextInit(void *ctx) {
	if (gSharedBufferVA == 0 || gMappedGpuVA == 0 || getMember<uint8_t>(ctx, 0x6c) != 0) return;
	using VA = uint8_t *(*)(void *);
	using GpuVA = uint64_t (*)(void *);
	void *imageBuf = getMember<void *>(ctx, 0x98);
	uint8_t *img = imageBuf != nullptr ? reinterpret_cast<VA>(gSharedBufferVA)(imageBuf) : nullptr;
	if (img == nullptr) return;
	auto r32 = [](uint8_t *p) { return *reinterpret_cast<volatile uint32_t *>(p); };
	auto w32 = [](uint8_t *p, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(p) = v; };
	const uint16_t flags = getMember<uint16_t>(ctx, 0x6e);
	auto *accel = getMember<uint8_t *>(ctx, 0x50);
	void *task = getMember<void *>(ctx, 0x58);
	void *pocsBuf = (flags & 0x10) != 0 ? getMember<void *>(ctx, 0xa0) : nullptr;
	if ((flags & 0x10) == 0) w32(img + 0x100c, (r32(img + 0x100c) & 0xffefffefu) | 0x100000u);
	else if (pocsBuf != nullptr) w32(img + 0x10d8, static_cast<uint32_t>(reinterpret_cast<GpuVA>(gMappedGpuVA)(pocsBuf)));
	const uint32_t accelFlags = accel != nullptr ? getMember<uint32_t>(accel, 0x1190) : 0;
	if ((accelFlags & 0x18000u) != 0) {
		w32(img + 0x100c, r32(img + 0x100c) | 0x80008u);
		uint8_t *pocs = pocsBuf != nullptr ? reinterpret_cast<VA>(gSharedBufferVA)(pocsBuf) : nullptr;
		if (pocs != nullptr) w32(pocs + 0x100c, r32(pocs + 0x100c) | 0x80008u);
	}
	void *aux = task != nullptr ? getMember<void *>(task, 0x278) : nullptr;
	void *l3 = aux != nullptr ? getMember<void *>(aux, 0x10) : nullptr;
	if (l3 != nullptr) {
		const uint64_t base = reinterpret_cast<GpuVA>((*reinterpret_cast<void ***>(l3))[0x130 / sizeof(void *)])(l3);
		w32(img + 0x1134, static_cast<uint32_t>(base >> 32));
		w32(img + 0x113c, static_cast<uint32_t>(base));
	}
	w32(img + 0x110c, 0x80000000u);
	auto *telemetry = accel != nullptr ? getMember<uint8_t *>(accel, 0xf10) : nullptr;
	if (telemetry != nullptr) {
		if (gPatchCtxImageRCS != 0) {
			using Patch = void (*)(void *, void *);
			reinterpret_cast<Patch>(gPatchCtxImageRCS)(telemetry, img);
		}
		getMember<uint64_t>(ctx, 0xd0) = getMember<uint64_t>(telemetry, 0x268);
	}
}
static bool wrapBeginReset(void *ctx) {
	const bool ok = FunctionCast(wrapBeginReset, gOrgBeginReset)(ctx);
	if (ok && ctx != nullptr) reapplyContextInit(ctx);
	if (!ok || ctx == nullptr || reinterpret_cast<OSObject *>(ctx)->metaCast("IGHardwareResolveContext") == nullptr) return ok;
	for (auto &slot : gResolveCtxReset)
		if (slot == ctx) return ok;
	for (auto &slot : gResolveCtxReset)
		if (__sync_bool_compare_and_swap(&slot, nullptr, ctx)) break;
	return ok;
}
static void forgetResetResolveContext(void *ctx) {
	for (auto &slot : gResolveCtxReset)
		if (slot == ctx) __sync_bool_compare_and_swap(&slot, ctx, nullptr);
}
// Not from updateSchedPriority: the one getter caller from which Apple never creates (so never initialises) a context.
static void initResolveContextAfterReset(void *ctx, const void *caller) {
	const auto from = reinterpret_cast<mach_vm_address_t>(caller);
	if (ctx == nullptr || gResolveCtxInitialize == 0 || (from >= gSchedPriority && from < gSchedPriority + kSchedPrioritySize))
		return;
	for (auto &slot : gResolveCtxReset) {
		if (slot != ctx) continue;
		// As at creation: room for the preamble in the buffer, and the two dwords it never writes (+0x5b0) zero. The mark goes
		// only once the re-init is certain to run: a failed room check keeps it for the next getter call.
		reserveResolveWindow(ctx);
		auto *pool = getMember<uint8_t *>(ctx, 0xe0);
		const uint64_t ptr = pool != nullptr ? *reinterpret_cast<volatile uint64_t *>(pool + 0x1858) : 0;
		const uint64_t lim = pool != nullptr ? *reinterpret_cast<volatile uint64_t *>(pool + 0x1850) : 0;
		if (ptr == 0 || ptr + kResolvePreamble > lim) return;
		if (!__sync_bool_compare_and_swap(&slot, ctx, nullptr)) return;
		bzero(reinterpret_cast<void *>(ptr), kResolvePreamble);
		using Initialize = bool (*)(void *);
		reinterpret_cast<Initialize>(gResolveCtxInitialize)(ctx);
		return;
	}
}

static void routeContextReset(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	// The marks are cleared when a context is freed: without that route a freed context's address could be re-initialised.
	if (!isKnownTGLImage(address, size) || gCtxFreeRouted == 0) return;
	gResolveCtxInitialize = patcher.solveSymbol(index, "__ZN24IGHardwareResolveContext10initializeEv", address, size);
	gSchedPriority = patcher.solveSymbol(index, "__ZN17IGHardwareContext19updateSchedPriorityEh", address, size);
	if (gResolveCtxInitialize == 0 || gSchedPriority == 0) {
		patcher.clearError();
		gResolveCtxInitialize = 0;
		return;
	}
	// The image's virtual address, a buffer's GPU address, the telemetry patch (optional).
	gSharedBufferVA = patcher.solveSymbol(index, "__ZNK20IGSharedMappedBuffer17getVirtualAddressEv", address, size);
	gMappedGpuVA = patcher.solveSymbol(index, "__ZNK14IGMappedBuffer20getGPUVirtualAddressEv", address, size);
	gPatchCtxImageRCS = patcher.solveSymbol(index, "__ZN18IGTelemetryManager20patchContextImageRCSEP27SGfxHardwareContextImageRCS", address, size);
	if (gSharedBufferVA == 0 || gMappedGpuVA == 0 || gPatchCtxImageRCS == 0) {
		patcher.clearError();
		if (gSharedBufferVA == 0 || gMappedGpuVA == 0) gSharedBufferVA = gMappedGpuVA = 0;
	}
	RouteRequestPlus route[] = {
		{"__ZN17IGHardwareContext10beginResetEv", wrapBeginReset, gOrgBeginReset},
	};
	if (!RouteRequestPlus::routeAll(patcher, index, route, address, size)) patcher.clearError();
}

static int getV77KillDelayIterations() {
	int delay = 0;
	if (PE_parse_boot_argn("ngreenV77DelayKill", &delay, sizeof(delay))) {
		if (delay < 0)
			return 0;
		if (delay > 60)
			return 60;
		return delay;
	}

	// Final/default behavior: do not kill DisplayPipe clients unless explicitly requested.
	return 60;
}

bool Gen11::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if (kextG11FB.loadIndex == index) {
		if (this->tglFBLoaded) {
			DBGLOG("ngreen", "Skipping ICL FB — TGL FB already loaded");
			return true;
		}
		auto *activeKext = &kextG11FB;
		DBGLOG("ngreen", "init AppleIntelICLLPGraphicsFramebuffer!");
		//NGreen::callback->igfxGen = iGFXGen::Gen11;
		NGreen::callback->setRMMIOIfNecessary();
		
		const bool wegCoexist = isWEGCoexistMode();
		if (wegCoexist) {
			SYSLOG("nblue", "WEG coexist mode enabled: skipping NootedBlue CDCLK route overlap");
		}

		if (wegCoexist) {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		} else {
			SolveRequestPlus solveRequests[] = {
			//		{"__ZN31AppleIntelFramebufferController14disableCDClockEv", this->orgDisableCDClock},
			//		{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments", this->orgFBClientDoAttribute},
			//		{"__ZN31AppleIntelFramebufferController5startEP9IOService",	this->ostart},
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",	this->oreadRegister32},
		 		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb", this->ohwConfigureCustomAUX},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv", this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "nblue",	"Failed to resolve symbols");
		}
		
		if (wegCoexist) {
			RouteRequestPlus requests[] = {
			// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
			// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
			//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
			// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		} else {
			RouteRequestPlus requests[] = {
			// Keep stock ReadRegister32 while stabilizing display pipeline behavior.
			//		{"__ZN31AppleIntelFramebufferController14ReadRegister32Em",wrapReadRegister32,	this->owrapReadRegister32},
			//		{"__ZN21AppleIntelFramebuffer13SaveNVRAMModeEv",handleLinkIntegrityCheck},
			// Keep stock wake/sleep lifecycle handlers to avoid broken restore paths.
			//{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv",dovoid},
			//{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv",dovoid},
			// Keep stock doAttribute while chasing UI stalls / high WindowServer CPU.
			//		{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute,	this->orgFBClientDoAttribute},
			//ADDED
			//{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
			//		{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
			//		{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
			//	{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock,this->oinitCDClock}
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "nblue","Failed to route symbols");
		}

		//static const uint8_t f15[]= {0x00,0x02, 0x00, 0x5c, 0x8a};
		//static const uint8_t r15[]= {0x00,0x00, 0x00, 0x49, 0x9a};
		
		
		// AppleIntelFramebufferController::hwSetMode skip hwRegsNeedUpdate
		static const uint8_t f2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x3D};
		static const uint8_t r2[] = {0xE8, 0x31, 0xE5, 0xFF, 0xFF, 0x84, 0xC0, 0xEB, 0x3D};
		
		//sonoma
		static const uint8_t f2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2b[] = {0xE8, 0x54, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		//sequoia
		static const uint8_t f2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x5C};
		static const uint8_t r2c[] = {0xE8, 0x74, 0xEA, 0xFF, 0xFF, 0x84, 0xC0, 0xeb, 0x5C};
		
		/*if (getKernelVersion() <= KernelVersion::Ventura) {
			KernelPatcher::LookupPatch patch { &kextG11FB, f2, r2, sizeof(f2), 1 };
			patcher.applyLookupPatch(&patch);
		}
		
		if (getKernelVersion() == KernelVersion::Sonoma) {
			KernelPatcher::LookupPatch patchb { &kextG11FB, f2b, r2b, sizeof(f2b), 1 };
			patcher.applyLookupPatch(&patchb);
		}
		
		if (getKernelVersion() >= KernelVersion::Sequoia) {
			KernelPatcher::LookupPatch patchc { &kextG11FB, f2c, r2c, sizeof(f2c), 1 };
			patcher.applyLookupPatch(&patchc);
		}*/
		
		

		// Variant-consistent remap for constructor entries:
		// B8 xx 00 5C 8A -> B8 xx 00 49 9A and exact C7 05 ... 02 00 5C 8A site.
		static const uint8_t kPatchPlatformRemapMovEaxFind0[] = {0xB8, 0x00, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace0[] = {0xB8, 0x00, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind1[] = {0xB8, 0x01, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace1[] = {0xB8, 0x01, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapMovEaxFind2[] = {0xB8, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapMovEaxReplace2[] = {0xB8, 0x02, 0x00, 0x49, 0x9A};
		static const uint8_t kPatchPlatformRemapC705Find2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x5C, 0x8A};
		static const uint8_t kPatchPlatformRemapC705Replace2[] = {0xC7, 0x05, 0xE9, 0x9B, 0x05, 0x00, 0x02, 0x00, 0x49, 0x9A};

		// hwSetMode: bypass hwRegsNeedUpdate result (CALL hwRegsNeedUpdate; TEST AL,AL: JE+0x62 → JMP+0x62)
		// Verified unique (1 match at 0x94055) in ICL LP le binary. Forces register reprogram unconditionally. [ICL-LP]
		static const uint8_t kPatchHwRegsNeedUpdateBypassFind[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x62};
		static const uint8_t kPatchHwRegsNeedUpdateBypassReplace[] = {0xe8, 0xe2, 0xcc, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x62};

		LookupPatchPlus const minPatches[] = {
			{&kextG11FB, kPatchPlatformRemapMovEaxFind0, kPatchPlatformRemapMovEaxReplace0, arrsize(kPatchPlatformRemapMovEaxFind0), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind1, kPatchPlatformRemapMovEaxReplace1, arrsize(kPatchPlatformRemapMovEaxFind1), 1},
			{&kextG11FB, kPatchPlatformRemapMovEaxFind2, kPatchPlatformRemapMovEaxReplace2, arrsize(kPatchPlatformRemapMovEaxFind2), 1},
			{&kextG11FB, kPatchPlatformRemapC705Find2, kPatchPlatformRemapC705Replace2, arrsize(kPatchPlatformRemapC705Find2), 1},
			{&kextG11FB, kPatchHwRegsNeedUpdateBypassFind, kPatchHwRegsNeedUpdateBypassReplace, arrsize(kPatchHwRegsNeedUpdateBypassFind), 1},  // hwSetMode always reprogram [ICL-LP]
		};
		
		PANIC_COND(!LookupPatchPlus::applyAll(patcher, minPatches , address, size), "ngreen", "kextG11FB Failed to apply patches!");
		//PANIC_COND
		
		DBGLOG("ngreen", "Loaded AppleIntelICLLPGraphicsFramebuffer!");
		return true;
		
		
	}	else if (kextG11FBT.loadIndex == index || kextG11FBTA.loadIndex == index) {
		return [&, this]() __attribute__((noinline)) -> bool {
		this->tglFBLoaded = true;
		auto *activeKext = (kextG11FBTA.loadIndex == index) ? &kextG11FBTA : &kextG11FBT;
		NGreen::callback->setRMMIOIfNecessary();
		SYSLOG("ngreen", "init AppleIntelTGLGraphicsFramebuffer");
		
		if (!isExpectedTGLDebugFramebuffer(address, size)) {
			return false;
		}
		constexpr bool isprod = false;
		
		if constexpr (isprod) {
			
			SolveRequestPlus solveRequests[] = {
				{"__ZN31AppleIntelFramebufferController19setCDClockFrequencyEy", this->orgSetCDClockFrequency},
				{"_gPlatformInformationList", this->gPlatformInformationList},
			};
			PANIC_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",	"Failed to resolve symbols");
		}
		else
		{
			constexpr mach_vm_address_t setCDClockFrequencyOffset = 0xA41E0;
			constexpr mach_vm_address_t platformInformationListOffset = 0x1C2FA0;
			PANIC_COND(setCDClockFrequencyOffset >= size || platformInformationListOffset >= size,
				"ngreen", "TGL fixed symbols are outside the loaded image");
			this->orgSetCDClockFrequency = reinterpret_cast<decltype(this->orgSetCDClockFrequency)>(
				address + setCDClockFrequencyOffset);
			this->gPlatformInformationList = address + platformInformationListOffset;
			
		}

		[&, this]() __attribute__((noinline)) {
		RouteRequestPlus requests[] = {
			// ...existing routes...
			//{"__ZN24AppleIntelBaseController17registerWithAICPMEPv", alwaysReturnSuccess, this->oalwaysReturnSuccess},
			// ...existing routes...
			/*{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.1",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.2",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.3",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.4",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.5",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.6",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.7",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.8",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.9",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.10",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.11",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj.cold.12",releaseDoorbell},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj.cold.1",releaseDoorbell},*/
			// V204: re-enable AppleIntelScaler/Plane init constructors so ccont is set
			// once at object construction. Friend's working version routes only these and
			// keeps the per-method patches commented; we keep both as belt-and-suspenders.
			{"__ZN16AppleIntelScaler4initE10IGScalerID", AppleIntelScalerinit, this->oAppleIntelScalerinit},
			{"__ZN15AppleIntelPlane4initE9IGPlaneID",     AppleIntelPlaneinit,  this->oAppleIntelPlaneinit},
			// AppleIntelPlaneinit/Scalerinit hooks are disabled in kern_genx.cpp, so fields
			// 0x90 (plane RAM) and 0x28/0x10 (scaler RAM) are never set at object-init time.
			// Every entry point into Plane/Scaler that calls ReadRegister32 will crash with
			// this=NULL unless we patch ccont in here.  These hooks must stay until the init
			// hooks are re-enabled (see kern_genx.cpp lines 75-76).
			{"__ZN16AppleIntelScaler13disableScalerEb",disableScaler, this->odisableScaler},
			{"__ZN15AppleIntelPlane11enablePlaneEb",enablePlane, this->oenablePlane},
			{"__ZN16AppleIntelScaler17programPipeScalerEP21AppleIntelDisplayPath",programPipeScaler, this->oprogramPipeScaler},
			// V400: read-only logger for setupPipeScaler. Confirms whether Apple's
			// pipe scaler is downscaling (PIPE_SRCSZ vs PS_PS_WIN_SZ mismatch) — if
			// yes, that's a direct cause of the fragmented/repeated scanout.
			{"__ZN16AppleIntelScaler15setupPipeScalerEP21AppleIntelDisplayPathP10CRTCParams", setupPipeScaler, this->osetupPipeScaler},
			// V401: paramsSurfCompare — read-only. Fires per flip. Logs PLANE_CTL
			// tiling bits (mask 0xF800000), PLANE_STRIDE (+0x18), PLANE_SURF (+0x20)
			// for both old and new PLANEPARAMS, plus CRTCParams PIPE_SRCSZ.
			{"__ZN24AppleIntelBaseController17paramsSurfCompareEP10CRTCParamsS1_PN15AppleIntelPlane11PLANEPARAMSES4_", paramsSurfCompare, this->oparamsSurfCompare},
			// setupDSCEngineParams: latches the DSC parameters for the VDSC flush.
			{"__ZN24AppleIntelBaseController20setupDSCEngineParamsEP21AppleIntelFramebufferP10CRTCParamsP21AppleIntelDisplayPathP29IODetailedTimingInformationV2", setupDSCEngineParams, this->osetupDSCEngineParams},
			// V403: SetupParams — master CRTCParams builder. Read-only logger of all
			// key fields after Apple populates the struct. Single chokepoint where
			// future origin-level overrides should land.
			{"__ZN24AppleIntelBaseController11SetupParamsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2", setupParams, this->osetupParams},
			// V404: setupPipeWatermarks — runs INSIDE SetupParams before setupPipeScaler.
			// Per-pipe DBUF/watermark allocator. Prime suspect for setting PIPE_SEAM_EXCESS=0x1.
			{"__ZN24AppleIntelBaseController19setupPipeWatermarksEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", setupPipeWatermarks, this->osetupPipeWatermarks},
			// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
			// Dispatches 8/10/12/12SEG gamma pipeline based on FlipTransactionArgs BPC mode.
			// Hook logs GAMMA_MODE (0x4A480) and PIPE_MISC (0x70030) before/after the call
			// to confirm Apple writes correct values for ADL-P Display 13 on our 8bpc eDP.
			{"__ZN15AppleIntelPlane22configureColorPipeLineEP19FlipTransactionArgsb", configureColorPipeLine, this->oConfigureColorPipeLine},
			// V406: AppleIntelPlane::configurePlane(FlipTransactionArgs*)
			// Patches FlipTransactionArgs+0x3c tiling enum to 2 (linear) so Apple builds
			// PLANE_CTL with bits[12:10]=000 and PLANE_STRIDE=pitch/512 natively.
			{"__ZN15AppleIntelPlane14configurePlaneEP19FlipTransactionArgs", configurePlane, this->oConfigurePlane},
			{"__ZN15AppleIntelPlane19updateRegisterCacheEv",AppleIntelPlaneupdateRegisterCache, this->oAppleIntelPlaneupdateRegisterCache},
			{"__ZN16AppleIntelScaler19updateRegisterCacheEv",AppleIntelScalerupdateRegisterCache, this->oAppleIntelScalerupdateRegisterCache},
			{"__ZN19AppleIntelPowerWell20disableDisplayEngineEv",disableDisplayEngine, this->odisableDisplayEngine},
			{"__ZN19AppleIntelPowerWell19enableDisplayEngineEv",enableDisplayEngine, this->oenableDisplayEngine},
		};
		static constexpr mach_vm_address_t requestOffsets[] = {
			0x98C48,
			0x03758,
			0x98F8C,
			0x03B5A,
			0x99112,
			0x99388,
			0x73FA2,
			0x3189E,
			0x72528,
			0x731F8,
			0x06DA8,
			0x0511E,
			0x092D6,
			0x9A73C,
			0x8A3F2,
			0x881EC,
		};
		PANIC_COND(!routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size),
			"ngreen", "Failed to route TGL debug common offsets batch 1");
		}();

		[&, this]() __attribute__((noinline)) {
			static const uint8_t prologue[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xff, 0x05};
			static constexpr mach_vm_address_t enableOffset[] = {0x31520}, disableOffset[] = {0x3172E};
			RouteRequestPlus enableRoute[] = {
				{"__ZN24AppleIntelBaseController15enableDSCEngineEP21AppleIntelFramebufferP21AppleIntelDisplayPath", wrapEnableDSCEngine, this->oEnableDSCEngine},
			};
			RouteRequestPlus disableRoute[] = {
				{"__ZN24AppleIntelBaseController16disableDSCEngineEP21AppleIntelFramebufferP21AppleIntelDisplayPath", wrapDisableDSCEngine, this->oDisableDSCEngine},
			};
			const bool same = disableOffset[0] + sizeof(prologue) <= size &&
			    memcmp(reinterpret_cast<const void *>(address + enableOffset[0]), prologue, sizeof(prologue)) == 0 &&
			    memcmp(reinterpret_cast<const void *>(address + disableOffset[0]), prologue, sizeof(prologue)) == 0;
			uint32_t routed = 0;
			if (same) {
				if (routeTGLDebugOffsets(patcher, disableRoute, disableOffset, address, size)) routed |= 2u;
				if (routed != 0u && routeTGLDebugOffsets(patcher, enableRoute, enableOffset, address, size)) routed |= 1u;
			}
			patcher.clearError();
			gVdscRouted = routed;
		}();

		[&, this]() __attribute__((noinline)) {
		RouteRequestPlus requests[] = {
			// V35: Removed ComboPhyEv hook — causes MCE on RPL/ADL. Firmware calibration sufficient.
			{"__ZN14AppleIntelPort16computeLaneCountEPK29IODetailedTimingInformationV2jjPj",computeLaneCount, this->ocomputeLaneCount},
			{"__ZN14AppleIntelPort21setupOptimalLaneCountEPK29IODetailedTimingInformationV2j",setupOptimalLaneCount, this->osetupOptimalLaneCount},
			// V97: Log AUX transactions to diagnose eDP link training failures on RPL
			{"__ZN14AppleIntelPort7readAUXEjPvj", wrapICLReadAUX, this->orgICLReadAUX},
			{"__ZN14AppleIntelPort8writeAUXEjPvj", wrapICLWriteAUX, this->orgICLWriteAUX},
			// V96: Force display online — WEG's getDisplayStatus hook (FOD) fails with
			// "err 2" on TGL kext because that symbol doesn't exist. TGL uses getOnlineInfo.
			{"__ZN21AppleIntelFramebuffer13getOnlineInfoEP21AppleIntelDisplayPathPhS2_", getOnlineInfo, this->ogetOnlineInfo},
			// V208/V209/V210 routes DISABLED at 3/3/3 baseline. They actively cause
			// the IOFramebuffer::getAttributeExt+0x25 NULL deref when fb1/fb2 start()
			// is refused but IGAccelDisplayPipe still tracks them. Re-enable only
			// alongside pinfo 1/1/1 if pursuing FB-count-reduction direction.
			//{"__ZN21AppleIntelFramebuffer5startEP9IOService", wrapAppleIntelFramebufferStart, this->oAppleIntelFramebufferStart},
			//{"__ZN21AppleIntelFramebuffer16enableControllerEv", wrapEnableController, this->oEnableController},
			{"__ZN21AppleIntelFramebuffer14setDisplayModeEii", setDisplayMode, this->osetDisplayMode},
			{"__ZN21AppleIntelFramebuffer18setupDisplayTimingEPK29IODetailedTimingInformationV2PS0_", setupDisplayTiming, this->osetupDisplayTiming},
			//{"__ZN21AppleIntelDisplayPath24getFreeJoinablePathCountEv", wrapGetFreeJoinablePathCount, this->oGetFreeJoinablePathCount},
			// Path B: Force aperture memory under dp0 mode to prevent WS=0x3 migration to
			// non-aperture, which leaves the display scanning empty pages and triggers
			// the 0x3→0x1 WS degradation.
			{"__ZN21AppleIntelFramebuffer24isApertureMemoryRequiredEv", wrapIsApertureMemoryRequired, this->oIsApertureMemoryRequired},
			// Path C: Coerce kIOWindowServerActiveAttribute=0x1 (WS degrade) to 0x3 (stay-active)
			// under dp0 mode so kernel-tracked fWSAAState never drops below 3 once WS goes active.
			{"__ZN21AppleIntelFramebuffer12setAttributeEjm", wrapSetAttribute, this->oSetAttribute},
			// V183: write-only ADL-P power well handler; no callthrough (TGL poll loop hangs on RPL).
			// Real TGL falls through to original via ohwSetPowerWellStatePGE.
			{"__ZN19AppleIntelPowerWell21hwSetPowerWellStatePGEbj", hwSetPowerWellStatePGE, this->ohwSetPowerWellStatePGE},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateAuxEbj",hwSetPowerWellStateAux, this->ohwSetPowerWellStateAux},
			{"__ZN19AppleIntelPowerWell22hwSetPowerWellStateDDIEbj",hwSetPowerWellStateDDI, this->ohwSetPowerWellStateDDI},
			{"__ZN24AppleIntelBaseController14hwSetBacklightEj", hwSetBacklight, this->ohwSetBacklight},
			{"__ZN31AppleIntelRegisterAccessManager19FastWriteRegister32Emj",FastWriteRegister32, this->oFastWriteRegister32},
			// V60: ReadRegister32 hooks DISABLED — V59 proved they cause 0-children regression
			// (display driver loops in forceWake power-well cycling, never completes init)
			/*{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32Em",raReadRegister32, this->oraReadRegister32},
			{"__ZN31AppleIntelRegisterAccessManager14ReadRegister32EPVvm",raReadRegister32b},*/
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32Emj",raWriteRegister32, this->oraWriteRegister32},
			{"__ZN31AppleIntelRegisterAccessManager15WriteRegister32EPVvmj",raWriteRegister32b},
			// V410: hook sleep/wake transition methods to intercept panel state tracking.
			// Apple's implementations are known to corrupt WS state on RPL/ADL on resume.
			{"__ZN21AppleIntelFramebuffer17prepareToExitWakeEv",   prepareToExitWake,   this->oprepareToExitWake},
			{"__ZN21AppleIntelFramebuffer18prepareToEnterWakeEv",  prepareToEnterWake,  this->oprepareToEnterWake},
			{"__ZN14AppleIntelPort12linkTrainingEP18AGDCDPPortConfig_t", wrapLinkTraining, this->oLinkTraining},
			{"__ZN14AppleIntelPort11getDPCDInfoEv", wrapGetDPCDInfo, this->oGetDPCDInfo},
			{"__ZN17AppleIntelPortHAL29setVoltageSwingAndPreEmphasisEjjP15PortDriveConfig",
			 wrapSetVoltageSwingAndPreEmphasis, this->oSetVoltageSwing},
			// Keep native SST timing setup; forcing custom clocks can break CoreDisplay validation.
			//{"__ZN24AppleIntelBaseController17SetupDPSSTTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParams", SetupDPSSTTimings, this->oSetupDPSSTTimings},
			//{"__ZN24AppleIntelBaseController12SetupTimingsEP21AppleIntelFramebufferP21AppleIntelDisplayPathPK29IODetailedTimingInformationV2P10CRTCParams", SetupTimings, this->oSetupTimings},
			// Keep native detailed timing validation; avoid overriding pixel clock fields.
			//{"__ZN21AppleIntelFramebuffer22validateDetailedTimingEPvy", validateDetailedTiming, this->ovalidateDetailedTiming},
			//{"__ZN21AppleIntelFramebuffer19validateDisplayModeEiPPKNS_15ModeDescriptionEPPK29IODetailedTimingInformationV2", validateDisplayMode, this->ovalidateDisplayMode},
	   		//{"__ZN21AppleIntelFramebuffer18setupDisplayTimingEPK29IODetailedTimingInformationV2PS0_", setupDisplayTiming, this->osetupDisplayTiming},
			//{"__ZN21AppleIntelFramebuffer18maxSupportedDepthsEPK29IODetailedTimingInformationV2", maxSupportedDepths, this->omaxSupportedDepths},
			//{"__ZN21AppleIntelFramebuffer17validateModeDepthEPK29IODetailedTimingInformationV2j", validateModeDepth, this->ovalidateModeDepth},
			//*****
			//{"__ZN21AppleIntelFramebuffer19getPixelInformationEiiiP18IOPixelInformation", getPixelInformation, this->ogetPixelInformation},
			//{"__ZN20IntelFBClientControl11doAttributeEjPmmS0_S0_P25IOExternalMethodArguments",wrapFBClientDoAttribute, this->orgFBClientDoAttribute},
			//{"__ZN20IntelFBClientControl24vendor_doDeviceAttributeEjPmmS0_S0_P25IOExternalMethodArguments", releaseDoorbell},
			//{"__ZN21AppleIntelFramebuffer16enableControllerEv", isPanelPowerOn},
		};
		static constexpr mach_vm_address_t requestOffsets[] = {
			0x2F032,
			0x2EEBE,
			0x28D9E,
			0x290BE,
			0x42874,
			0x4762C,
			0x9CF24,
			0x9E54E,
			0x4AD5E,
			0x89A36,
			0x8B7DC,
			0x8AD3A,
			0x6C67C,
			0x8DFB2,
			0x8DCE6,
			0x8DE14,
			0x3788C,
			0x383A8,
			0x26C54,
			0x2AA3C,
			0x90588,
		};
		gTGLFbBase = isKnownTGLImage(address, size) ? address : 0;
		PANIC_COND(!routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size),
			"ngreen", "Failed to route TGL debug common offsets batch 2");
		}();
		
		[&, this]() __attribute__((noinline)) {
			RouteRequestPlus sleepRoutes[] = {
				{"__ZN21AppleIntelFramebuffer18prepareToExitSleepEv", prepareToExitSleep, this->oprepareToExitSleep},
				{"__ZN21AppleIntelFramebuffer19prepareToEnterSleepEv", prepareToEnterSleep, this->oprepareToEnterSleep},
			};
			static constexpr mach_vm_address_t sleepOffsets[] = {0x396AE, 0x37E44};
			PANIC_COND(!routeTGLDebugOffsets(patcher, sleepRoutes, sleepOffsets, address, size),
				"ngreen", "Failed to route TGL debug sleep offsets");
		}();
		routeAUXTransactionGuard(patcher, address, size);
		routeEnhancedFraming(patcher, address, size);
		routePixelInformation(patcher, address, size);

		[&, this]() __attribute__((noinline)) {
		if constexpr (isprod) {
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP31AppleIntelFramebufferControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN31AppleIntelFramebufferController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN31AppleIntelFramebufferController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN31AppleIntelFramebufferController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN31AppleIntelFramebufferController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN31AppleIntelFramebufferController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP31AppleIntelFramebufferController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN31AppleIntelFramebufferController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN31AppleIntelFramebufferController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN31AppleIntelFramebufferController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN31AppleIntelFramebufferController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN31AppleIntelFramebufferController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN31AppleIntelFramebufferController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
				{"__ZN31AppleIntelFramebufferController35createAppleGraphicsDeviceControlnubEv", tglCreateAppleGraphicsDeviceControlnub, this->otglCreateAppleGraphicsDeviceControlnub},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route p symbols");
			
		} else
		{
			RouteRequestPlus requests[] = {
				{"__ZN21AppleIntelFramebuffer4initEP24AppleIntelBaseControllerj",AppleIntelFramebufferinit, this->oAppleIntelFramebufferinit},
				{"__ZN24AppleIntelBaseController23initPlatformWorkaroundsEv", initPlatformWorkarounds, this->oinitPlatformWorkarounds},
				{"__ZN24AppleIntelBaseController16getOSInformationEv", getOSInformation, this->ogetOSInformation},
				{"__ZN24AppleIntelBaseController10hwShutdownEP21AppleIntelFramebuffer",handleLinkIntegrityCheck},
				{"__ZN24AppleIntelBaseController18hwInitializeCStateEv",hwInitializeCState, this->ohwInitializeCState},
				{"__ZN24AppleIntelBaseController20hwConfigureCustomAUXEb",hwConfigureCustomAUX, this->ohwConfigureCustomAUX},
				{"__ZN19AppleIntelPowerWell4initEP24AppleIntelBaseController",AppleIntelPowerWellinit, this->oAppleIntelPowerWellinit},
				{"__ZN24AppleIntelBaseController5startEP9IOService",AppleIntelBaseControllerstart, this->oAppleIntelBaseControllerstart},
				{"__ZN24AppleIntelBaseController21probeCDClockFrequencyEv",wrapProbeCDClockFrequency,	this->orgProbeCDClockFrequency},
				{"__ZN24AppleIntelBaseController11initCDClockEv",initCDClock, this->oinitCDClock},
				{"__ZN24AppleIntelBaseController28setCDClockFrequencyOnHotplugEv",setCDClockFrequencyOnHotplug, this->osetCDClockFrequencyOnHotplug},
				{"__ZN24AppleIntelBaseController14disableCDClockEv",disableCDClock,this->odisableCDClock},
				{"__ZN24AppleIntelBaseController16hwRegsNeedUpdateEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsPK29IODetailedTimingInformationV2PN16AppleIntelScaler12SCALERPARAMSE",hwRegsNeedUpdate, this->ohwRegsNeedUpdate},
				// V201 diagnostic: read scanout buffer right after hwSetupMemory returns to
				// detect what fills it (zeros vs wallpaper pixels). FB-only scope.
				{"__ZN24AppleIntelBaseController13hwSetupMemoryEP21AppleIntelFramebufferP21AppleIntelDisplayPathP10CRTCParamsb", wrapHwSetupMemory, this->ohwSetupMemory},
				{"__ZN24AppleIntelBaseController35createAppleGraphicsDeviceControlnubEv", tglCreateAppleGraphicsDeviceControlnub, this->otglCreateAppleGraphicsDeviceControlnub},
				{"__ZN24AppleIntelBaseController10metricsLogE14kAGDCCommand_tjjz", metricsLogStub},
				{"__ZN24AppleIntelBaseController11hwSaveNVRAMEv", hwSaveNVRAMStub},
			};
			static constexpr mach_vm_address_t requestOffsets[] = {
				0x3CFE8,
				0x60502,
				0x5D1A0,
				0x773D0,
				0x5F7E2,
				0x65DA2,
				0x867C8,
				0x5A8A0,
				0xA4F90,
				0xA5192,
				0xA45B8,
				0xA4D46,
				0x732DC,
				0x6DF9E,
				0xE7A58,
				0xA284C,
				0x77A5A,
			};
			PANIC_COND(!routeTGLDebugOffsets(patcher, requests, requestOffsets, address, size),
				"ngreen", "Failed to route TGL debug controller offsets");
			routeGtClock(patcher, address, size);
			routeAgdcDoAttribute(patcher, address, size);
			routeDpbResidency(patcher, address, size);
			
			static const uint8_t forceWakeGTAckLoopFind[]    = {0x45, 0x38, 0xe7, 0x75, 0x9d, 0x45, 0x84, 0xe4};
			static const uint8_t forceWakeGTAckLoopMask[]    = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
			static const uint8_t forceWakeGTAckLoopReplace[] = {0x45, 0x38, 0xe7, 0x90, 0x90, 0x45, 0x84, 0xe4};
			static const uint8_t forceWakeGTAckLoopReplMask[]= {0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00};

			const LookupPatchPlus forceWakeGTAckPatch {&kextG11FBT, forceWakeGTAckLoopFind, forceWakeGTAckLoopMask,
				forceWakeGTAckLoopReplace, forceWakeGTAckLoopReplMask, 1};

			forceWakeGTAckPatch.apply(patcher, address, size);
			patcher.clearError();

			static const uint8_t controllerRegSvc_f[]  = {0x49, 0x8b, 0x45, 0x00, 0x4c, 0x89, 0xef, 0x31, 0xf6, 0xff, 0x90, 0xb0, 0x05, 0x00, 0x00};
			static const uint8_t controllerRegSvc_m[]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
			static const uint8_t controllerRegSvc_r[]  = {0x49, 0x8b, 0x45, 0x00, 0x4c, 0x89, 0xef, 0x31, 0xf6, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			static const uint8_t controllerRegSvc_rm[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
			const LookupPatchPlus controllerRegSvcPatch {&kextG11FBT, controllerRegSvc_f, controllerRegSvc_m,
				controllerRegSvc_r, controllerRegSvc_rm, 1};
			controllerRegSvcPatch.apply(patcher, address, size);
			patcher.clearError();
		}
		}();
		
		//powerwell
		static const uint8_t f1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x78};
		static const uint8_t r1[]= {0xe8, 0x99, 0x9f, 0xfd, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x78};
		
		static const uint8_t f1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0x74, 0x45};
		static const uint8_t r1p[]= {0xe8, 0x66, 0xb0, 0xfe, 0xff, 0x89, 0x45, 0xc8, 0x3d, 0xff, 0xff, 0x00, 0x00, 0xeb, 0x45};
		
		//osinfo
		/*fInfoHasLid                  : 1
		fInfoPipeCount               : 3
		fInfoPortCount               : 3
		fInfoFramebufferCount        : 3*/

		static const uint8_t f2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x03, 0x09, 0x03, 0xb8, 0x00, 0x00, 0x00, 0x04};
		static const uint8_t r2[]= {0xc7, 0x05, 0x07, 0x81, 0x10, 0x00, 0x01, 0x04, 0x03, 0x02, 0xb8, 0x00, 0x00, 0x00, 0x04};

		static const uint8_t f2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x03, 0x09, 0x03};
		static const uint8_t r2p[]= {0xc7, 0x05, 0x57, 0xe5, 0x0b, 0x00, 0x01, 0x04, 0x03, 0x02};
		
		static const uint8_t f2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		static const uint8_t r2b[]= {0x49, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0x35, 0x17, 0x81, 0x10, 0x00, 0xb8, 0x08, 0x00, 0x00, 0x00};
		
		static const uint8_t f2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		static const uint8_t r2c[]= {0x48, 0x89, 0x1d, 0xc0, 0x81, 0x10, 0x00, 0x4c, 0x89, 0x35, 0xc1, 0x81, 0x10, 0x00, 0x48, 0xb8, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
		
		//mem
		static const uint8_t f2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00};
		static const uint8_t r2d[]= {0x0f, 0x94, 0xc0, 0xb9, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00};
		
		//mem
		static const uint8_t f2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x80, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		static const uint8_t r2dp[]= {0xb8, 0x00, 0x00, 0x10, 0x00, 0xba, 0x00, 0x00, 0x40, 0x00, 0x0f, 0x44, 0xd0, 0x0f, 0x94, 0xc1, 0x48, 0x01, 0x0d, 0xa2, 0xd0, 0x09, 0x00};
		
		//cdclock
		static const uint8_t f2e[]= {0x48, 0xc7, 0x83, 0x60, 0x43, 0x00, 0x00, 0x00, 0x2d, 0x31, 0x01, 0x48, 0xc7, 0x83, 0x68, 0x43, 0x00, 0x00, 0x00, 0x54, 0xea, 0x2a, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		static const uint8_t r2e[]= {0x48, 0xc7, 0x83, 0x60, 0x4a, 0x00, 0x00, 0x00, 0xa3, 0x02, 0x00, 0x48, 0xc7, 0x83, 0x68, 0x4a, 0x00, 0x00, 0x00, 0xf6, 0x09, 0x00, 0xc6, 0x83, 0xb4, 0x45, 0x00, 0x00, 0x00};
		


		//conn
		static const uint8_t f3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x04, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x05, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x06, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x07, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x08, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00};
		
		static const uint8_t r3[]= {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
			0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
		
		
		//lcd power reg
		static const uint8_t f4[]= {0x00, 0x72, 0x0c, 0x00};
		static const uint8_t r4[]= {0x00, 0x12, 0x06, 0x00};
		
		static const uint8_t f4a[]= {0x04, 0x72, 0x0c, 0x00};
		static const uint8_t r4a[]= {0x04, 0x12, 0x06, 0x00};
		
		static const uint8_t f4b[]= {0x08, 0x72, 0x0c, 0x00};
		static const uint8_t r4b[]= {0x08, 0x12, 0x06, 0x00};
		
		static const uint8_t f4c[]= {0x0c, 0x72, 0x0c, 0x00};
		static const uint8_t r4c[]= {0x0c, 0x12, 0x06, 0x00};
		
		
		//jalavoui
		static const uint8_t f6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0xe8, 0x8c, 0xac, 0x04, 0x00};
		static const uint8_t r6a[]= { 0xbe, 0x04, 0x00, 0x00, 0x00, 0x48, 0x89, 0xda, 0x31, 0xc9, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		//ReadRegister64
		static const uint8_t f7[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		static const uint8_t r7[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0xca, 0xf5, 0x0c, 0x00};
		
		static const uint8_t f7p[]= {0x83, 0xc0, 0xfc, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};
		static const uint8_t r7p[]= {0x83, 0xc0, 0xf8, 0x48, 0x39, 0xf0, 0x76, 0x11, 0x48, 0x8b, 0x47, 0x50, 0x48, 0xff, 0x05, 0x84, 0x40, 0x08, 0x00};

		
		//hwreg
		static const uint8_t f10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x5b};
		static const uint8_t r10[]= {0xe8, 0xaf, 0xe2, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x5b};
		
		static const uint8_t f10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0x74, 0x3d};
		static const uint8_t r10p[]= {0xe8, 0x9e, 0xf3, 0xff, 0xff, 0x84, 0xc0, 0xeb, 0x3d};
		
		//probeportmode
		static const uint8_t f13b[]= {0xff, 0x90, 0x90, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13b[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		static const uint8_t f13[]= {0xff, 0x91, 0x90, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x0f, 0x84, 0xec, 0x00, 0x00, 0x00};
		static const uint8_t r13[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13p[]= {0xff, 0x91, 0x78, 0x01, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x74, 0x64};
		static const uint8_t r13p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
		
		static const uint8_t f13pb[]= {0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};
		static const uint8_t r13pb[]= {0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x0e, 0x4c, 0x89, 0xf7, 0x89, 0xc6, 0xff, 0x91, 0x38, 0x01, 0x00, 0x00};

		
		//getPathByPipe logs
		static const uint8_t f15[]= {0x74, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		static const uint8_t r15[]= {0xeb, 0x36, 0x48, 0xff, 0x05, 0x7e, 0x51, 0x08, 0x00, 0x44, 0x89, 0x3c, 0x24, 0x48, 0x8d, 0x15, 0x4d, 0x88, 0x03, 0x00, 0x4c, 0x8d, 0x05, 0x28, 0x8a, 0x03, 0x00};
		
		//getBuiltInPor
		static const uint8_t f16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0d};
		static const uint8_t r16[]= {0x48, 0x89, 0x05, 0xfc, 0x39, 0x12, 0x00, 0x48, 0x8b, 0x83, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};
		
		static const uint8_t f16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x75, 0x0a};
		static const uint8_t r16p[]= {0x48, 0x8b, 0x80, 0x48, 0x05, 0x00, 0x00, 0xf6, 0x40, 0x14, 0x08, 0x90, 0x90};

		//getHPDState
		static const uint8_t f19[]= {0xbe, 0x70, 0x44, 0x04, 0x00};
		static const uint8_t r19[]= {0xbe, 0xa0, 0x38, 0x16, 0x00};
		
		//savenvram
		static const uint8_t f20[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		static const uint8_t f20p[]= {0xff, 0x90, 0xf8, 0x09, 0x00, 0x00, 0x41, 0x89, 0xc6, 0x48, 0x85, 0xdb, 0x74, 0x17};
		static const uint8_t r20p[]= {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x85, 0xdb, 0x74, 0x17};
		
		//SafeForceWake
		static const uint8_t f21[]= {0x0f, 0x84, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		static const uint8_t r21[]= {0x48, 0xe9, 0x96, 0x00, 0x00, 0x00, 0x48, 0xff, 0x05, 0xbc, 0x05, 0x0f, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7};
		
		static const uint8_t f21p[]= {0x74, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
		static const uint8_t r21p[]= {0xeb, 0x3c, 0x48, 0xff, 0x05, 0x7a, 0x73, 0x09, 0x00, 0xbe, 0x44, 0x00, 0x13, 0x00, 0x4c, 0x89, 0xf7, 0xe8, 0x97, 0x80, 0x01, 0x00};
        
        //pixel hwcrt
        static const uint8_t f22[]= {0x48, 0x69, 0xc2, 0x50, 0xc3, 0x00, 0x00, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x74, 0x3a};
        
        static const uint8_t r22[]= {0x48, 0xc7, 0xc0, 0xc0, 0x40, 0xd0, 0x2e, 0x49, 0x89, 0x47, 0x28, 0xbf, 0x08, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, 0x9e, 0x81, 0x01, 0x00, 0x84, 0xc0, 0x90, 0x90};
		

		// force eDP panel detection regardless of pipe number (from NootedBlue)
		// NOPs two JE and one JNE that would skip eDP init when pipe != 1.
		// Required because our pinfo sets eDP on pipe=0, not pipe=1.
		static const uint8_t f6nb[]= {0x74, 0x2a, 0x83, 0xf8, 0x01, 0x74, 0x43, 0x85, 0xc0, 0x75, 0x60};
		static const uint8_t r6nb[]= {0x90, 0x90, 0x83, 0xf8, 0x01, 0x90, 0x90, 0x85, 0xc0, 0x90, 0x90};

		// fix register addresses if pipe=0 (jne→jmp to always use pipe-0 register offsets)
		static const uint8_t f24bp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24bp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24cp[]= {0x00, 0x4c, 0x89, 0xea, 0x75, 0x12};
		static const uint8_t r24cp[]= {0x00, 0x4c, 0x89, 0xea, 0xeb, 0x12};
		static const uint8_t f24dp[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24dp[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		static const uint8_t f24b[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0c};
		static const uint8_t r24b[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0c};
		static const uint8_t f24c[]= {0x48, 0x8b, 0x55, 0xd0, 0x75, 0x13};
		static const uint8_t r24c[]= {0x48, 0x8b, 0x55, 0xd0, 0xeb, 0x13};
		static const uint8_t f24d[]= {0x83, 0x78, 0x08, 0x00, 0x75, 0x0d};
		static const uint8_t r24d[]= {0x83, 0x78, 0x08, 0x00, 0xeb, 0x0d};
		// link training speed constant fix
		static const uint8_t f25[]= {0x77, 0x77, 0x00, 0x00};
		static const uint8_t r25[]= {0x33, 0x00, 0x00, 0x00};

		// Path E (TCON ID): guarded by -ngreentglwithgfx boot-arg (requires GFX kext present).
		// Rewrites CamelliaTcon2/BanksiaTcon DPCD ID comparisons so they match this panel's
		// DPCD bytes [14 1e c4 c1] → 0xc1c41e14. Also sets cameliav=2 in getOSInformation.
		// DO NOT enable on FB-only boot (no GFX): calls into AGDC services → hang.
		static const uint8_t f_tcon_camellia[]= {0x3d, 0x11, 0x0a, 0x84, 0x41};
		static const uint8_t r_tcon_camellia[]= {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		static const uint8_t f_tcon_banksia[] = {0x3d, 0x12, 0x14, 0xc4, 0x41};
		static const uint8_t r_tcon_banksia[] = {0x3d, 0x14, 0x1e, 0xc4, 0xc1};
		const bool enableTcon = checkKernelArgument("-ngreentglwithgfx");

		if constexpr (isprod) {
			LookupPatchPlus const patchesp[] = {// tgl production kext

				// f1p (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT prod patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1p, r1p, arrsize(f1p),	1},
				{activeKext, f2p, r2p, arrsize(f2p),	1},
				{activeKext, f2dp, r2dp, arrsize(f2dp),	1},
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT present in NootedBlue's working TGL FBT prod patch list. These rewrite
				// what appears to be LCD power register access patterns; on Display 13 hardware
				// the original Apple code paths may be correct without the rewrite.
				//{activeKext, f4, r4, arrsize(f4),	11},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
				{activeKext, f7p, r7p, arrsize(f7p),	1},
				// f10p ("hwreg" CALL+JZ→JMP bypass) commented out — NOT in NootedBlue.
				//{activeKext, f10p, r10p, arrsize(f10p),	1},
				// Keep native probe-port mode flow for compatibility.
				//{activeKext, f13p, r13p, arrsize(f13p),	1},
				//{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				//{activeKext, f16p, r16p, arrsize(f16p),	1},
				{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
				{activeKext, f13p, r13p, arrsize(f13p),	1},
				{activeKext, f13pb, r13pb, arrsize(f13pb),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
				{activeKext, f20p, r20p, arrsize(f20p),	1},
				{activeKext, f24bp, r24bp, arrsize(f24bp),	14},
				{activeKext, f24cp, r24cp, arrsize(f24cp),	1},
				{activeKext, f24dp, r24dp, arrsize(f24dp),	4},
				{activeKext, f25,  r25,  arrsize(f25),	6},
			};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesp , address, size), "ngreen", "kextG11FBT Failed to apply production patches!");
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				LookupPatchPlus::applyAll(patcher, tconPatches, address, size);
				SYSLOG("ngreen", "Path E: TCON ID patches applied (prod)");
			}
		}
		else {
			LookupPatchPlus const patches[] = {// tgl debug kext
				// f1 (powerwell JZ→JMP) commented out — not present in NootedBlue's
				// working TGL FBT debug patch list. Was an extra NootedGreen accumulated.
				//{activeKext, f1, r1, arrsize(f1),	1},
				// f2/f2d: osinfo pipe/port/fb counts now set via getOSInformation hook — no binary patch needed.
				// f3 (connector data table rewrite) commented out — not in NootedBlue.
				//{activeKext, f3, r3, arrsize(f3),	1},
				// f4 family ("lcd power reg" 0x72→0x12, 4 variants ~27 binary mods) commented out
				// — NOT in NootedBlue's working TGL FBT debug patch list. Suspected contributor
				// to the fragmentation/repetition symptom: rewriting LCD power register access
				// patterns may corrupt panel-side state on Display 13 (ADL-P) hardware.
				//{activeKext, f4, r4, arrsize(f4),	12},
				//{activeKext, f4a, r4a, arrsize(f4a),	11},
				//{activeKext, f4b, r4b, arrsize(f4b),	2},
				//{activeKext, f4c, r4c, arrsize(f4c),	2},
		//		{activeKext, f6a, r6a, arrsize(f6a),	1},
		//		{activeKext, f7, r7, arrsize(f7),	1},
				// f13: mandatory. Without this port-probe bypass the spoofed TGL path freezes during boot.
				{activeKext, f13, r13, arrsize(f13),	1},
				// f13b: mandatory. Without this bypass AppleIntelPort::probePortStateEv hits
				// a pure-virtual call and panics in WindowServer during enableController.
		//		{activeKext, f13b, r13b, arrsize(f13b),	1},
				// f15: suppress getPathByPipe log flood at IGLogLevel=8.
				// On platform 0x9a490000 all paths are on pipe 0; every scan cycle logs
				// "pipe = 0" many times per second, flooding the log unreadably.
				// The je→jmp makes the branch unconditionally skip the IGFB log emit.
				// Purely cosmetic: no behavioral change, no display-pipe impact.
				{activeKext, f15, r15, arrsize(f15),	1},
				//{activeKext, f16, r16, arrsize(f16),	1},
				{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				//{activeKext, f21, r21, arrsize(f21),	1},
				//{activeKext, f22, r22, arrsize(f22),    1},
		//		{activeKext, f6nb, r6nb, arrsize(f6nb),	1},
		//		{activeKext, f19, r19, arrsize(f19),	1},
		//		{activeKext, f20, r20, arrsize(f20),	1},
				{activeKext, f24b, r24b, arrsize(f24b),	11},
		//		{activeKext, f24c, r24c, arrsize(f24c),	1},
		//		{activeKext, f24d, r24d, arrsize(f24d),	6},
		//		{activeKext, f25,  r25,  arrsize(f25),	6},
				};

			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patches , address, size), "ngreen", "kextG11FBT Failed to apply dbg patches!");
			if (enableTcon) {
				LookupPatchPlus const tconPatches[] = {
					{activeKext, f_tcon_camellia, r_tcon_camellia, arrsize(f_tcon_camellia), 1},
					{activeKext, f_tcon_banksia,  r_tcon_banksia,  arrsize(f_tcon_banksia),  1},
				};
				LookupPatchPlus::applyAll(patcher, tconPatches, address, size);
				SYSLOG("ngreen", "Path E: TCON ID patches applied (dbg)");
			}
		}
		
		patchEdpTranscoder(address, size);
		patchValidatePixelFormat(address, size);

		return true;
		
		}();
	}else if (kextG11HW.loadIndex == index) {
		if (this->tglHWLoaded) {
			DBGLOG("ngreen", "Skipping ICL HW — TGL HW already loaded");
			return true;
		}
		auto *activeKext = &kextG11HW;
		DBGLOG("ngreen", "init AppleIntelICLGraphics!");
		injectAcceleratorPersonality(false);
		NGreen::callback->setRMMIOIfNecessary();
		const bool wegCoexist = isWEGCoexistMode();

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route loadGuCBinary (ICL)");
		}

		if (!wegCoexist) {
			RouteRequestPlus requests[] = {
				// PAVP/DRM: intercept session command callback (ICL hardware path, shared hook with TGL)
				{"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb", wrapPavpSessionCallback, this->orgPavpSessionCallback},
				// initHardwareCaps NOT routed: NBlue's wrapper reads TGL offset 0x1120 for SKU,
				// but ICL stores SKU at 0x1150. Let the original ICL code run — SKU gates are patched.
				// IGScheduler5resume NOT routed: kIGHwCsDesc is only resolved for kextG11HWT.
				// With -disablegfxfirmware, Host Preemptive scheduler is selected (not IGScheduler5).
				//last	 {"__ZN12IGScheduler56resumeEv", IGScheduler5resume, this->oIGScheduler5resume},
				// resetGraphicsEngine NOT routed: NBlue wrapper applies TGL GT workarounds which
				// target TGL MMIO offsets. Hardware is RPL-P (adlp/raptorlake) — using TGL workarounds
				// on RPL MMIO could corrupt the command streamer. Let the ICL original run unmodified.
			//symbol absent in kext	{"__ZN20IGHardwareRingBuffer19resetGraphicsEngineEP17IGHardwareContext", resetGraphicsEngine, this->oresetGraphicsEngine},
				//last	 {"__ZN13IGHardwareGuC18checkWOPCMSettingsEmR14IOVirtualRange", checkWOPCMSettings, this->ocheckWOPCMSettings},
				//last	 {"__ZN11IGScheduler15canLoadFirmwareEP16IntelAccelerator", canLoadFirmware, this->ocanLoadFirmware},
				 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
				 // Same implementation as TGL path — Gen11 IRQ registers are identical for ICL/TGL.
				 // V37: DISABLED — caused boot hang on TGL path; disabling ICL too for safety.
				 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},
			};

			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route dp symbols");

			RouteRequestPlus gpuInfoRoute[] = {
				// getGPUInfo: override topology at ICL object offsets (different from TGL offsets)
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfoICL, this->ogetGPUInfoICL},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfoICL");
		}
		
		// SKU gate 1+2: NOP JNZ/JA + XOR eax,eax (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x0F, 0x85, 0x0B, 0x01, 0x00, 0x00,
			0xFF, 0xC8,
			0x83, 0xF8, 0x07,
			0x0F, 0x87, 0x00, 0x01, 0x00, 0x00,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};
		static const uint8_t rSKUGates12[] = {
			0x83, 0xF9, 0x01,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x31, 0xC0,
			0x83, 0xF8, 0x07,
			0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
			0x48, 0x8D, 0x0D, 0x77, 0x02, 0x00, 0x00, 0x48
		};

		// SKU gate 3: NOP JNZ (Sonoma AppleIntelICLGraphics, verified in KC)
		static const uint8_t fSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x0F, 0x85, 0xC2, 0x00, 0x00, 0x00, 0xC7
		};
		static const uint8_t rSKUGate3[] = {
			0x83, 0xF8, 0x08, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC7
		};

		// SKU bypass: TEST rax,rax; JZ->JMP (Sonoma, f2Long verified in KC at 0x14152bcd)
		static const uint8_t fSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0x74, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};
		static const uint8_t rSkuBypassLong[] = {
			0x48, 0x85, 0xC0, 0xEB, 0x72, 0x48, 0x0F, 0xBC, 0xC0, 0x48, 0xFF, 0xC0, 0x48,
			0x8D, 0x15, 0x00, 0xCD, 0x0F, 0x00, 0x48, 0x8D, 0x48, 0xFF, 0x48, 0xF7, 0xC1,
			0xFD, 0xFF, 0xFF, 0xFF, 0x74, 0x27, 0x48, 0x6B, 0xC9, 0x79
		};

		LookupPatchPlus const patches[] = {
			{&kextG11HW, fSKUGates12,    rSKUGates12,    arrsize(fSKUGates12),    1},
			{&kextG11HW, fSKUGate3,      rSKUGate3,      arrsize(fSKUGate3),      1},
			{&kextG11HW, fSkuBypassLong, rSkuBypassLong, arrsize(fSkuBypassLong), 1},
		};
		
		/*auto catalina = getKernelVersion() == KernelVersion::Catalina;
		if (catalina)
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesc , address, size), "ngreen", "cata Failed to apply patches!");
		else*/
		for (size_t i = 0; i < sizeof(patches)/sizeof(patches[0]); ++i) {
			//IOSleep(delay);
			PANIC_COND(!patches[i].apply(patcher, address, size), "ngreen", "kextG11HW Failed to apply patch %zu", i);
		}
		DBGLOG("ngreen", "Loaded AppleIntelICLGraphics!");

		return true;

	} else if (kextG11HWT.loadIndex == index || kextG11HWTA.loadIndex == index) {
		return [&, this]() __attribute__((noinline)) -> bool {
		this->tglHWLoaded = true;
		auto *activeKext = (kextG11HWTA.loadIndex == index) ? &kextG11HWTA : &kextG11HWT;
		SYSLOG("ngreen", "init AppleIntelTGLGraphics (HW accelerator)");
		injectAcceleratorPersonality(true);
		NGreen::callback->setRMMIOIfNecessary();
		SYSLOG("ngreen", "V165: setRMMIO done, starting symbol resolve");

		// V144: Resolve the Blit3D context params struct and the ExtendedContext initWithOptions.
		// Blit3DExtendedCtxParams is a static data symbol — address passed as param_2 to initWithOptions.
		// Without these, IGHardwareExtendedContextinitWithOptions is called with param_2=0
		// causing it to return 0 (ok=0) and leave ctx+0xb8=NULL → submitBlit crash at +0x28e.
		{
			SolveRequestPlus solveRequests[] = {
				{"__ZN23IGHardwareBlit3DContext17ExtendedCtxParamsE", this->Blit3DExtendedCtxParams},
			};
			SYSLOG_COND(!SolveRequestPlus::solveAll(patcher, index, solveRequests, address, size), "ngreen",
			            "V144: Failed to resolve Blit3DExtendedCtxParams");
		}

		const bool wegCoexist = isWEGCoexistMode();
		const bool forceFullMTL = shouldForceFullMetalPath();

		{
			SolveRequestPlus resolvePoolParams[] = {
				{"__ZN24IGHardwareResolveContext17ExtendedCtxParamsE", this->ResolveExtendedCtxParams},
			};
			if (!SolveRequestPlus::solveAll(patcher, index, resolvePoolParams, address, size)) {
				patcher.clearError();
			}
		}
		{
			SolveRequestPlus blitSyms[] = {
				{"__Z16blit2d_supportedPK15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP9_WA_TABLE",
				 gBlit2DSupported},
				{"__Z16blit3d_supportedPK15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyE",
				 gBlit3DSupported},
			};
			if (!SolveRequestPlus::solveAll(patcher, index, blitSyms, address, size)) {
				patcher.clearError();
			}
		}

		{
			auto pb = patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_base");
			if (pb == 0) patcher.clearError();
			auto pm = patcher.solveSymbol(KernelPatcher::KernelID, "_physmap_max");
			if (pm == 0) patcher.clearError();
			gPhysmapBase = reinterpret_cast<uint64_t *>(pb);
			gPhysmapMax  = reinterpret_cast<uint64_t *>(pm);
		}

		{
			SolveRequestPlus enableSolve[] = {
				{"__ZN11IGScheduler33enablePeriodicEventTimerInterruptEP22IOInterruptEventSource",
				 this->oIGSchedEnablePeriodic},
			};
			SolveRequestPlus::solveAll(patcher, index, enableSolve, address, size);

			RouteRequestPlus finishTimerRoute[] = {
				{"__ZN26IGHardwareCommandStreamer521registerForInterruptsEv",
				 wrapHWCS5RegisterForInterrupts, this->oHWCS5RegisterForInterrupts},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, finishTimerRoute, address, size)) {
				patcher.clearError();
			}
		}

		{
			RouteRequestPlus submitRoute[] = {
				{"__ZN26IGHardwareCommandStreamer514submitExecListEj",
				 wrapHWCS5SubmitExecList, this->oHWCS5SubmitExecList},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, submitRoute, address, size)) {
				patcher.clearError();
			}
		}

		{
			RouteRequestPlus i2aRoute[] = {
				{"__ZN26IGHardwareCommandStreamer522csbProcessIdleToActiveEPK28SGfxContextStatusBufferEntry",
				 wrapIdleToActive, this->oIdleToActive},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, i2aRoute, address, size)) patcher.clearError();
			RouteRequestPlus preRoute[] = {
				{"__ZN26IGHardwareCommandStreamer519csbProcessPreemptedEPK28SGfxContextStatusBufferEntry",
				 wrapPreempted, this->oPreempted},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, preRoute, address, size)) patcher.clearError();
			RouteRequestPlus esRoute[] = {
				{"__ZN26IGHardwareCommandStreamer523csbProcessElementSwitchEPK28SGfxContextStatusBufferEntry",
				 wrapElementSwitch, this->oElementSwitch},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, esRoute, address, size)) patcher.clearError();
			RouteRequestPlus a2iRoute[] = {
				{"__ZN26IGHardwareCommandStreamer522csbProcessActiveToIdleEPK28SGfxContextStatusBufferEntry",
				 wrapActiveToIdle, this->oActiveToIdle},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, a2iRoute, address, size)) patcher.clearError();
			RouteRequestPlus wakeRoute[] = {
				{"__ZN26IGHardwareCommandStreamer511resetOnWakeEv", wrapResetOnWake, this->oResetOnWake},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, wakeRoute, address, size)) patcher.clearError();
		}

		{
			if (gTimerLock == nullptr) gTimerLock = IOSimpleLockAlloc();
			struct { const char *sym; mach_vm_address_t wrap; mach_vm_address_t &org; } timerRoutes[] = {
				{"__ZN11IGScheduler33enablePeriodicEventTimerInterruptEP22IOInterruptEventSource",
				 reinterpret_cast<mach_vm_address_t>(wrapEnablePeriodic), this->oEnablePeriodic},
				{"__ZN11IGScheduler34disablePeriodicEventTimerInterruptEP22IOInterruptEventSource",
				 reinterpret_cast<mach_vm_address_t>(wrapDisablePeriodic), this->oDisablePeriodic},
			};
			for (auto &e : timerRoutes) {
				RouteRequestPlus one[] = {{e.sym, e.wrap, e.org}};
				if (gTimerLock == nullptr || !RouteRequestPlus::routeAll(patcher, index, one, address, size)) patcher.clearError();
			}
		}

		{
			struct { const char *sym; mach_vm_address_t wrap; mach_vm_address_t &org; } flipRoutes[] = {
				{"__ZN18IGAccelDisplayPipe16submitFlipBufferEP12IOAccelEventjP16IOAccelResource2S3_",
				 reinterpret_cast<mach_vm_address_t>(wrapSubmitFlipBuffer), gOrgSubmitFlipBuffer},
				{"__ZN18IGAccelDisplayPipe12generateFlipEP12IOAccelEventjP16IOAccelResource2S3_",
				 reinterpret_cast<mach_vm_address_t>(wrapGenerateFlip), gOrgGenerateFlip},
				{"__ZN14IGAccelSurface17isBackBufferReadyEj",
				 reinterpret_cast<mach_vm_address_t>(wrapBackBufferReady), gOrgBackBufferReady},
			};
			for (auto &e : flipRoutes) {
				RouteRequestPlus one[] = {{e.sym, e.wrap, e.org}};
				if (!RouteRequestPlus::routeAll(patcher, index, one, address, size)) patcher.clearError();
			}
		}

		[&, this]() __attribute__((noinline)) {
		RouteRequestPlus requests[] = {
			
			 {"__ZN16IntelAccelerator20_PAVPCommandCallbackEP8OSObject22PAVPSessionCommandID_tjPj", wrapPavpCommandCallbackObj, this->orgPavpCommandCallbackObj},
			
			// V163: Hook startGraphicsEngine to clear PERCTX_PREEMPT_CTRL (FF_SLICE_CS_CHICKEN1 bit 14)
			// immediately after the TGL kext enables it. The TGL kext writes 0x40004000 to reg 0x20E0
			// (mask=bit14, value=bit14=1). On RPL this causes the EU thread dispatcher to freeze on
			// first context switch because hardware snapshots the register into the context image DMA
			// buffer — clearing it here, before any execlist context is created, prevents the bad
			// value from ever reaching the DMA buffer.
		 	{"__ZN16IntelAccelerator19startGraphicsEngineEv", startGraphicsEngine, this->ostartGraphicsEngine},
			{"__ZN16IntelAccelerator18stopGraphicsEngineEv",  stopGraphicsEngine,  this->ostopGraphicsEngine},

			// V212: Hook IGScheduler{4,5}::isGpuIdle — the bool watchdog query called post-startup.
			// startGraphicsEngine SUCCEEDS (returns non-zero = success path in decomp). But the GPU
			// watchdog polls isGpuIdle() after init; INSTDONE bit0 stuck at 0 makes it return false
			// → watchdog declares GPU hung → resets → startGraphicsEngine retry loop forever.
			// Fix: when INSTDONE == 0xfffffffe (RPL-P idle with stuck bit0), return true.
			{"__ZNK12IGScheduler59isGpuIdleEv", wrapIGScheduler5IsGpuIdle, this->oIGScheduler5IsGpuIdle},
			{"__ZNK12IGScheduler49isGpuIdleEv", wrapIGScheduler4IsGpuIdle, this->oIGScheduler4IsGpuIdle},

			// V164: Hook populateResetRegisterList which reads live MMIO values into the per-context
			// replay list (a batch of MI_LRI commands executed before every context switch).
			// startGraphicsEngine enables PERCTX_PREEMPT_CTRL then calls populateResetRegisterList,
			// which snapshots the live 0x4000 into the list. Hardware then replays 0x4000 back to
			// 0x20E0 on every context switch, overriding any post-hoc MMIO clear.
			// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000.
			{"__ZN16IntelAccelerator25populateResetRegisterListEv", populateResetRegisterList, this->opopulateResetRegisterList},


			 // V132: Hook task producers so submitBlit never sees a null IGAccelTask on spoofed RPL.
			 {"__ZN16IntelAccelerator17createUserGPUTaskEv", createUserGPUTask, this->ocreateUserGPUTask},
			 {"__ZN11IGAccelTask11withOptionsEP16IntelAccelerator", igAccelTaskWithOptions, this->oigAccelTaskWithOptions},

			 // V36: Hook readAndClearInterrupts to initialize Gen11 multi-engine GT interrupts.
			 // Without this, RCS/BCS user interrupts and context-switch notifications may not
			 // be properly enabled, preventing IOAccelF2 from seeing stamp completions.
			 // V37: DISABLED — caused boot hang (symbol may not exist in TGL kext, or
			 // interrupt reprogramming too early causes deadlock/panic).
			 // {"__ZN16IntelAccelerator23readAndClearInterruptsEPv", readAndClearInterrupts, this->oreadAndClearInterrupts},

			// V119: Route getBlit3DContext so our guard checks context+0xb8 before storing.
			 // Without this, Apple's original runs, stores a context with +0xb8=NULL to
			 // that+0x298 (when init path leaves +0xb8 unset), and submitBlit+0x28e crashes.
			 {"__ZN11IGAccelTask16getBlit3DContextEb", getBlit3DContext, this->ogetBlit3DContext},
		
			 // V112: Resolve the NootedBlue Blit3D helpers so the fallback path can
			 // initialize scratch/context state instead of leaving the blit object empty.
			 {"__Z31blit3d_initialize_scratch_spaceP16IGAccelSysMemory", blit3d_initialize_scratch_space, this->oblit3d_initialize_scratch_space},
			 {"__Z15blit3d_init_ctxP23IGHardwareBlit3DContext", blit3d_init_ctx, this->oblit3d_init_ctx},
			 // V69: ENABLED — original crashes at +0x4c memcpy'ing 68 bytes to unmapped GPU buffer
			 // at VA 0xfffffff034136000 (page 0xD of context buffer). Our replacement skips the
			 // dangerous memcpy and logs diagnostic info about the mapping.
			 {"__ZN23IGHardwareBlit3DContext10initializeEv", IGHardwareBlit3DContextinitialize, this->oIGHardwareBlit3DContextinitialize},
			 // V143: Register operator new for IGHardwareBlit3DContext so getBlit3DContext can
			 // actually allocate the context object. Without this, oIGHardwareBlit3DContextoperatornew
			 // is always null → alloc always returns nullptr → ctx=0 forever → NULL-task submitBlit
			 // flood → BCS ring WAIT_ON_SCANLINE stall.
			 {"__ZN23IGHardwareBlit3DContextnwEm", IGHardwareBlit3DContextoperatornew, this->oIGHardwareBlit3DContextoperatornew},
			 // V508: Hook base-class IGHardwareContext::withOptions to inspect ctx+0xb8 (FIFO
			 // channel / ring buffer allocation) and dump LRCA page1 via wbinvd+aperture right
			 // after initWithOptions writes it — determines if RING_START=0 is real or LLC artifact.
			 {"__ZN17IGHardwareContext11withOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh", IGHardwareContextwithOptions, this->oIGHardwareContextwithOptions},
			 // V509: Hook base-class IGHardwareContext::initWithOptions — repair LRCA page1 when
			 // restoreFromSafeImage skips g_cInitGfxRingContextRCS memcpy (leaves DW1=0x00ffffff).
			 // Writes Gen12 ring context MI_LRI block so ExecList context-restore works on RPL.
			 {"__ZN17IGHardwareContext15initWithOptionsEP11IGAccelTaskRK23IGHardwareContextParamsh", IGHardwareContextinitWithOptions, this->oIGHardwareContextinitWithOptions},

			 // V144: Hook IGHardwareExtendedContext::initWithOptions so it actually runs with
			 // the resolved Blit3DExtendedCtxParams. Without this, oIGHardwareExtendedContextinitWithOptions
			 // is null → init returns 0 → ctx+0xb8 stays NULL → submitBlit+0x28e crash.
			 {"__ZN25IGHardwareExtendedContext15initWithOptionsEP11IGAccelTaskRK31IGHardwareExtendedContextParams", IGHardwareExtendedContextinitWithOptions, this->oIGHardwareExtendedContextinitWithOptions},
			 {"__ZNK14IGMappedBuffer9getMemoryEv", IGMappedBuffergetMemory, this->oIGMappedBuffergetMemory},
			 
			 // V120: Hook submitBlit to protect against nullptr context.
			 // Even with getBlit3DContext hooked and returning null, Apple's blitCopy
			 // doesn't check the return value and passes it straight to submitBlit.
			 // submitBlit+0x28e unconditionally dereferences param_5+0xb8 (where param_5
			 // is the Blit3D or Blit2D context) -> page fault. 
			 {"__ZN16IntelAccelerator10submitBlitEP15blit3d_params_tRK8IGVectorI11rect_pair_t25IGIOMallocAllocatorPolicyEP11IGAccelTaskb", submitBlit, this->osubmitBlit},

			 // V121: Hook IGAccelSegmentResourceList::initBlitUsage which also crashes at +0x17
			 // dereferencing [member+0xb8] when the Blit3D context was never populated
			 // (same root cause: incomplete Blit context init leaves context+0xb8 null).
			 // The crash arrives via: coalesceSegment -> prepare -> initBlitUsage.
			 {"__ZN26IGAccelSegmentResourceList13initBlitUsageEv", initBlitUsage, this->oinitBlitUsage},

			 // V122: markBlitUsage+0x17 has the identical [ctx+0xb8] dereference and is called
			 // from prepare+0x2a immediately after initBlitUsage. Must be guarded too.
			 {"__ZN26IGAccelSegmentResourceList13markBlitUsageEv", markBlitUsage, this->omarkBlitUsage},

			 // V123b: Register the prepare hook — the implementation (returning 0 early) existed
			 // but was never routed, so Apple's original kept calling initBlitUsage/markBlitUsage.
			 {"__ZN26IGAccelSegmentResourceList7prepareEv", IGAccelSegmentResourceListprepare, this->oIGAccelSegmentResourceListprepare},

			 // V124: IGAccelCommandQueue::beginCoalescedSegment+0x2f dereferences [member+0xb8]
			 // (same null as all previous crashes — context never initialized on RPL).
			 // Called from processAndSubmitCoalescedSegments even when prepare returns 0.
			 // NOTE: processCommandBuffer (the caller) calls [off_C8178+0xA30] which IS the
			 // actual render submission — hooking that would break display. So we guard here
			 // at the lowest safe level instead.
			 {"__ZN19IGAccelCommandQueue21beginCoalescedSegmentEv", beginCoalescedSegment, this->obeginCoalescedSegment},

			 // V126b: barrierSubmission itself must keep original side effects/contract.
			 // We route it only to scope temporary getter fallback (no broad bypass).
			 {"__Z17barrierSubmissionR19IGAccelCommandQueueR16IntelAcceleratorR24IGAccelCommandDescriptorR12IOAccelEventtPKt", barrierSubmission, this->obarrierSubmission},
			 
			 // V117: Hook getBlit2DContext for null-guarding only.
			 // submitBlit expects a real Blit2D context layout and unconditionally reads
			 // [ctx+0xb8], so we must never substitute depth/color/3D context objects here.
			 {"__ZN11IGAccelTask16getBlit2DContextEb", getBlit2DContext, this->ogetBlit2DContext},

			 // Resolve-context getters are used by barrierSubmission and are generally safe.
			 {"__ZN11IGAccelTask22getDepthResolveContextEb", getDepthResolveContext, this->ogetDepthResolveContext},
			 {"__ZN11IGAccelTask22getColorResolveContextEb", getColorResolveContext, this->ogetColorResolveContext},
			 
		 };
		SYSLOG("ngreen", "V165: routing %zu HW accelerator symbols", sizeof(requests)/sizeof(requests[0]));
		PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, requests, address, size), "ngreen","Failed to route symbols");
		SYSLOG("ngreen", "V165: HW accelerator symbols routed OK");
		}();
		{
			SolveRequestPlus pageonSyms[] = {{"__ZTV19IGAccelEventMachine", gEventMachineVtbl}};
			if (!SolveRequestPlus::solveAll(patcher, index, pageonSyms, address, size)) {
				patcher.clearError();
				gEventMachineVtbl = 0;
			}
			RouteRequestPlus routePageon[] = {
				{"__ZN15IGAccelResource6pageonEP12IOAccelEventby", wrapPageon, this->oPageon},
			};
			if (RouteRequestPlus::routeAll(patcher, index, routePageon, address, size)) {
				gPageonState = kPageonRouted;
				gPageonActive = 1u;
			} else {
				patcher.clearError();
				gPageonState = kPageonRouteFailed;
			}
		}
		{
			if (gCtxFreeLock == nullptr) gCtxFreeLock = IOSimpleLockAlloc();
			RouteRequestPlus ctxFreeRoute[] = {
				{"__ZN17IGHardwareContext4freeEv", wrapContextFree, this->oContextFree},
			};
			if (gCtxFreeLock != nullptr && RouteRequestPlus::routeAll(patcher, index, ctxFreeRoute, address, size))
				gCtxFreeRouted = 1;
			else
				patcher.clearError();
		}
		// V429: see wrapWaitForDRMSessionRestore (its own frame: processKext's code stays as it was).
		routeV429(patcher, index, address, size);
		{
			RouteRequestPlus renderExecuteRoute[] = {
				{"__ZN23IntelMTLRenderFunctions7executeEN14IntelMTLRender7eTokensER19IGAccelCommandQueueR26IGAccelSegmentResourceListRK20IOAccelKernelCommandR24IGAccelCommandDescriptorR13IGHeapsAccessR22IOGraphicsAccelerator2R17IGHardwareContextR12IOAccelEvent",
				 wrapRenderExecute, this->oRenderExecute},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, renderExecuteRoute, address, size)) patcher.clearError();
		}
		{
			RouteRequestPlus hangRoute[] = {
				{"__ZN20IGHardwareRingBuffer14doHangAnalysisEv", wrapDoHangAnalysis, this->oDoHangAnalysis},
			};
			if (!RouteRequestPlus::routeAll(patcher, index, hangRoute, address, size)) patcher.clearError();
		}
		patchDsbSize(address, size);
		patchResolveEop(address, size);
		patchTileFlush(address, size);
		patchAuxFlush(address, size);
		{
			const mach_vm_address_t planeCount = patcher.solveSymbol(KernelPatcher::KernelID, "__ZNK9IOSurface13getPlaneCountEv");
			if (planeCount == 0) patcher.clearError();
			patchPlaneCount(address, size, planeCount);
			const mach_vm_address_t pixelFormat = patcher.solveSymbol(KernelPatcher::KernelID, "__ZNK9IOSurface14getPixelFormatEv");
			if (pixelFormat == 0) patcher.clearError();
			patchPixelFormat(address, size, pixelFormat);
		}
		routeResolveInit(patcher, index, address, size);
		routeContextReset(patcher, index, address, size);

		{
			RouteRequestPlus startRoute[] = {
				{"__ZN16IntelAccelerator5startEP9IOService", start, this->ostart},
			};
			if (RouteRequestPlus::routeAll(patcher, index, startRoute, address, size)) {
				SYSLOG("ngreen", "V44: Hooked IntelAccelerator::start");
			} else {
				SYSLOG("ngreen", "V44: IntelAccelerator::start symbol not found; Gen11::start logs unavailable on this build");
			}
		}

		{
			// loadGuCBinary: always route — WEG's firmware path is Mojave-gated and dead on Sonoma.
			// Without this hook, no GuC binary loads at all in coexist mode → ring dead.
			RouteRequestPlus firmwareRoute[] = {
				{"__ZN13IGHardwareGuC13loadGuCBinaryEv", loadGuCBinary, this->oloadGuCBinary},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, firmwareRoute, address, size), "ngreen", "Failed to route loadGuCBinary");
		}

		if (!NGreen::callback->isRealTGL) {
			// V111: Hook IGAccelDevice::deviceStart to force success on RPL.
			// Binary pattern (f_devstart) may not match every Sonoma build variant.
			// This symbol-based hook guarantees deviceStart returns true regardless of
			// BCS ring state, so the accelerator device is always registered with IOKit.
			RouteRequestPlus devStartRoute[] = {
				{"__ZN13IGAccelDevice11deviceStartEv", deviceStart, this->odeviceStart},
			};
			if (RouteRequestPlus::routeAll(patcher, index, devStartRoute, address, size)) {
				SYSLOG("ngreen", "V111: Hooked IGAccelDevice::deviceStart for RPL force-success");
			} else {
				SYSLOG("ngreen", "V111: IGAccelDevice::deviceStart symbol not found — relying on binary patch");
			}
		}

		{
			RouteRequestPlus fwReentRoute[] = {
				{"__ZN16IntelAccelerator13SafeForceWakeEbj", wrapSafeForceWakeReent, this->oSafeForceWakeReent},
			};
			RouteRequestPlus::routeAll(patcher, index, fwReentRoute, address, size);
		}

		if (!wegCoexist || forceFullMTL) {
			RouteRequestPlus coexistOffRoutes[] = {
				// ForceWake: replace Apple's SafeForceWakeMultithreaded with i915-ported version.
				// Apple's code uses 90ms timeouts and no fallback; ours uses 50ms + reserve-bit fallback.
				// The original domain mapping was broken (d<<1 loop misaligned Apple's 3-bit dom bitmap).
				{"__ZN16IntelAccelerator26SafeForceWakeMultithreadedEbjj", forceWake, this->oforceWake},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, coexistOffRoutes, address, size), "ngreen", "Failed to route coexist-off symbols");
			if (forceFullMTL && wegCoexist) {
				SYSLOG("ngreen", "FULL_MTL: forcing SafeForceWakeMultithreaded route despite coexist mode");
			}
		}

		if (!wegCoexist) {
			RouteRequestPlus gpuInfoRoute[] = {
				{"__ZN16IntelAccelerator10getGPUInfoEv", getGPUInfo, this->ogetGPUInfo},
			};
			PANIC_COND(!RouteRequestPlus::routeAll(patcher, index, gpuInfoRoute, address, size), "ngreen", "Failed to route getGPUInfo");
		}

		// SKU/device-ID panic bypass (verified @ 0x23c1d in LE binary)
		// Original: mov edi,[rsi]; cmp edi,0xDEAFBEEE; jg sentinel; cmp edi,0x9A408086; je ok; cmp edi,0x9A488086; je ok; call panic
		// Patch:    nop the sentinel-jg + change last "je ok" → "jmp ok" → always jumps to GT2 init path.
		// Necessary: our spoofed 0x9A498086 is not in the whitelist (0x9A408086 / 0x9A488086).
		static const uint8_t f3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x7f, 0x15, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0x74, 0x2d
		};
		static const uint8_t r3[] = {
			0x8b, 0x3e, 0x81, 0xff, 0xee, 0xbe, 0xaf, 0xde, 0x90, 0x90, 0x81, 0xff, 0x86, 0x80, 0x40, 0x9a, 0xeb, 0x2d
		};
		// GT tier override: stores GT1 (0x1) instead of GT2 (0x2) at IGAccelDevice+0x1120.
		// Disabled – 0x9A49 is GT2 so the default path (which writes 0x2) is correct.
		static const uint8_t f3a[] = {//gt1
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		static const uint8_t r3a[] = {
			0x41, 0xc7, 0x86, 0x20, 0x11, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe9, 0xda, 0xfc, 0xff, 0xff
		};
		
		// L3BankCount bypass (verified @ 0x28776 in LE binary)
		// Original: topology-gated conditionals (cmp slices/eu/threads) → only set L3BankCount=8 for a specific config.
		// Patch:    NOP all conditional branches → always store L3BankCount=4 @ IGAccelDevice+0x1164.
		static const uint8_t f3b[] = {// jmp L3BankCount
			0x74, 0x23, 0x83, 0xf9, 0x02, 0x0f, 0x85, 0x89, 0x01, 0x00, 0x00, 0x83, 0xfe, 0x01, 0x75, 0x59, 0x83, 0xfa, 0x0c, 0x75, 0x54, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
		};
		static const uint8_t r3b[] = {
			0x90, 0x90, 0x83, 0xf9, 0x02, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x83, 0xfe, 0x01, 0x90, 0x90, 0x83, 0xfa, 0x0c, 0x90, 0x90, 0x41, 0xc7, 0x87, 0x64, 0x11, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00
		};
		
		// MaxEUPerSubSlice override (verified @ 0x28692 in LE binary)
		// Original: MaxEUPerSubSlice = 8 - popcount(EUDisableFuse)  → stores result at IGAccelDevice+0x116c
		// Patch:    hardcodes MaxEUPerSubSlice=8 (8 EU per traditional sub-slice).
		//           TGL binary counts sub-slices (SS), not dual sub-slices (DSS).
		//           Linux shows 16 EU/DSS = 8 EU/SS since each DSS has 2 SS.
		static const uint8_t f3bb[] = {//MaxEUPerSubSlice
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x29, 0xde, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bb[] = {
			0xbe, 0x08, 0x00, 0x00, 0x00, 0x90, 0x90, 0x41, 0x89, 0xb7, 0x6c, 0x11, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x58, 0x11, 0x00, 0x00
		};
		
		// NumSubSlices override (verified @ 0x28654 in LE binary)
		// Original: mov ebx,[rbp-0x30]; popcnt esi,ebx; add esi,esi; mov [r15+0x1158],esi
		//           → NumSubSlices = popcount(subsliceMask) * 2  (hardware-detected)
		// Patch:    hardcodes NumSubSlices=4 (2 DSS × 2 SS/DSS).
		static const uint8_t f3bbb[] = {//NumSubSlices
			0x8b, 0x5d, 0xd0, 0xf3, 0x0f, 0xb8, 0xf3, 0x01, 0xf6, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		static const uint8_t r3bbb[] = {
			0x8b, 0x5d, 0xd0, 0xbe, 0x04, 0x00, 0x00, 0x00, 0x90, 0x41, 0x89, 0xb7, 0x58, 0x11, 0x00, 0x00
		};
		
		// GPU caps override (disabled) – would change MaxSlices 6→5, SARation 2→1, MaxEU/SS 6→5.
		// Leave disabled unless acceleration shows wrong Metal tier/feature set.
		static const uint8_t f4[] = {// CAPS
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0xd1, 0xe8, 0xba, 0x02, 0x00, 0x00, 0x00, 0xbe, 0x06, 0x00, 0x00, 0x00
		};
		static const uint8_t r4[] = {
			0xc7, 0x83, 0x48, 0x11, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x8b, 0x83, 0x58, 0x11, 0x00, 0x00, 0x90, 0x90, 0xba, 0x01, 0x00, 0x00, 0x00, 0xbe, 0x05, 0x00, 0x00, 0x00
		};

		// V46: IGAccelDevice::deviceStart bypass — NOP the BCS failure gate
		// IGAccelDevice::deviceStart() makes one vtable check (at vtable+0x970); if it
		// returns 0, the whole device start aborts → IOServiceOpen fails → MTLDevice=nil.
		// Root cause: IGHardwareCommandStreamer5::init for BCS fails (BCS CTL=0x0, ring
		// not running), sets encodeFailureStack[1]=1, and the readiness check reads that.
		// The RCS ring IS operational (CTL=0x7000, HWS_PGA valid, 5 CSB events confirmed).
		// NOPing the je lets deviceStart always take the success path so IOServiceOpen
		// succeeds, MTLDevice is non-nil, and WindowServer stops hanging.
		//
		// Pattern (masked, Sonoma-variant tolerant):
		//   ff 90 70 09 00 00  callq *0x970(%rax)   ← readiness vtable call
		//   84 c0              testb %al,%al
		//   74 xx              je failure_path       ← PATCH: 74 xx → 90 90
		//   48 8d xx           lea ...               ← allow minor compiler/reg variance
		static const uint8_t f_devstart[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x74, 0x00, 0x48, 0x8d, 0x00
		};
		static const uint8_t m_devstart[] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0x00
		};
		static const uint8_t r_devstart[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x90, 0x90, 0x48, 0x8d, 0x00
		};
		static const uint8_t rm_devstart[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00
		};

		// Some Sonoma builds encode the same readiness failure jump as long form:
		//   ff 90 70 09 00 00; 84 c0; 0f 84 xx xx xx xx
		// Patch 0f 84 rel32 -> 6x NOP to force success path.
		// Keep this optional (non-fatal) to avoid boot regressions when pattern differs.
		static const uint8_t f_devstart_long[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x0f, 0x84, 0x00, 0x00, 0x00, 0x00
		};
		static const uint8_t m_devstart_long[] = {
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00
		};
		static const uint8_t r_devstart_long[] = {
			0xff, 0x90, 0x70, 0x09, 0x00, 0x00,
			0x84, 0xc0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
		};
		static const uint8_t rm_devstart_long[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
		};

		// V139: RPL-only mitigation for GP faults inside blit3d_submit_rectlist.
		// Some command-buffer pointers on spoofed paths are 8-byte aligned; Apple emits
		// aligned SSE stores (movaps [r9+...], xmmN), which faults on unaligned targets.
		// Convert the hot-path stores to movups to tolerate unaligned command pointers.
		static const uint8_t f_v139_movaps_10[] = {
			0x41, 0x0f, 0x29, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t r_v139_movaps_10[] = {
			0x41, 0x0f, 0x11, 0x51, 0x10, 0x0f, 0x28, 0xd4
		};
		static const uint8_t f_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x29, 0x51, 0x30
		};
		static const uint8_t r_v139_movaps_30[] = {
			0x66, 0x0f, 0x3a, 0x21, 0xd4, 0x23, 0x41, 0x0f, 0x11, 0x51, 0x30
		};
		static const uint8_t f_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x29, 0x51, 0x50
		};
		static const uint8_t r_v139_movaps_50[] = {
			0x0f, 0x57, 0xd2, 0x0f, 0x16, 0xd4, 0x41, 0x0f, 0x11, 0x51, 0x50
		};
		static const uint8_t f_v139_movaps_00[] = {
			0x41, 0x0f, 0x29, 0x09
		};
		static const uint8_t r_v139_movaps_00[] = {
			0x41, 0x0f, 0x11, 0x09
		};
		static const uint8_t f_v139_movaps_20[] = {
			0x41, 0x0f, 0x29, 0x49, 0x20
		};
		static const uint8_t r_v139_movaps_20[] = {
			0x41, 0x0f, 0x11, 0x49, 0x20
		};
		static const uint8_t f_v139_movaps_40[] = {
			0x41, 0x0f, 0x29, 0x59, 0x40
		};
		static const uint8_t r_v139_movaps_40[] = {
			0x41, 0x0f, 0x11, 0x59, 0x40
		};

		[&]() __attribute__((noinline)) {

		{
			// V52: Split patches into always-apply and RPL-only groups.
			// Real TGL reads topology from fuses correctly; RPL must hardcode
			// because fuse layout differs and BCS ring doesn't start.
			LookupPatchPlus const patchesAlways[] = {
				// SKU/device-ID bypass — needed for both (0x9A49 not in whitelist)
				{activeKext, f3, r3, arrsize(f3),	1},
			};
			PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesAlways, address, size), "ngreen",
				"kextG11HWT Failed to apply base patches!");
			
			if (!NGreen::callback->isRealTGL) {
				// RPL-only: hardcode topology and bypass BCS readiness check
				LookupPatchPlus const patchesRPL[] = {
					{activeKext, f3b, r3b, arrsize(f3b),	1},      // L3BankCount=4
					{activeKext, f3bb, r3bb, arrsize(f3bb),	1},    // MaxEU/SS=8
					{activeKext, f3bbb, r3bbb, arrsize(f3bbb),	1},// NumSubSlices=4
					{activeKext, f_devstart, m_devstart, r_devstart, rm_devstart, arrsize(f_devstart), 1}, // BCS bypass
				};
				PANIC_COND(!LookupPatchPlus::applyAll(patcher, patchesRPL, address, size), "ngreen",
					"kextG11HWT Failed to apply RPL-specific patches!");

				// Optional secondary signature for Sonoma variants with long JE encoding.
				/*LookupPatchPlus const patchRPLDevstartLong {
					activeKext,
					f_devstart_long,
					m_devstart_long,
					r_devstart_long,
					rm_devstart_long,
					arrsize(f_devstart_long),
					1
				};
				if (patchRPLDevstartLong.apply(patcher, address, size)) {
					SYSLOG("ngreen", "V52: Applied optional long-form deviceStart readiness bypass");
				} else {
					SYSLOG("ngreen", "V52: Optional long-form deviceStart bypass not found on this build");
				}*/

				// Optional SSE unaligned-store mitigation for blit3d_submit_rectlist.
				LookupPatchPlus const patchV139Store10 {
					activeKext,
					f_v139_movaps_10,
					r_v139_movaps_10,
					arrsize(f_v139_movaps_10),
					1
				};
				LookupPatchPlus const patchV139Store30 {
					activeKext,
					f_v139_movaps_30,
					r_v139_movaps_30,
					arrsize(f_v139_movaps_30),
					1
				};
				LookupPatchPlus const patchV139Store50 {
					activeKext,
					f_v139_movaps_50,
					r_v139_movaps_50,
					arrsize(f_v139_movaps_50),
					1
				};
				LookupPatchPlus const patchV139Store00 {
					activeKext,
					f_v139_movaps_00,
					r_v139_movaps_00,
					arrsize(f_v139_movaps_00),
					6
				};
				LookupPatchPlus const patchV139Store20 {
					activeKext,
					f_v139_movaps_20,
					r_v139_movaps_20,
					arrsize(f_v139_movaps_20),
					4
				};
				LookupPatchPlus const patchV139Store40 {
					activeKext,
					f_v139_movaps_40,
					r_v139_movaps_40,
					arrsize(f_v139_movaps_40),
					1
				};
				if (!patchV139Store10.apply(patcher, address, size)) patcher.clearError();
				if (!patchV139Store30.apply(patcher, address, size)) patcher.clearError();
				if (!patchV139Store50.apply(patcher, address, size)) patcher.clearError();
				if (!patchV139Store00.apply(patcher, address, size)) patcher.clearError();
				if (!patchV139Store20.apply(patcher, address, size)) patcher.clearError();
				if (!patchV139Store40.apply(patcher, address, size)) patcher.clearError();

				SYSLOG("ngreen", "V52: Applied RPL-specific patches (topology hardcode + BCS bypass)");
			} else {
				SYSLOG("ngreen", "V52: Real TGL — skipping topology hardcodes and BCS bypass");
			}
		}
		}();

		SYSLOG("ngreen", "Loaded AppleIntelTGLGraphics! %s",
			   NGreen::callback->isRealTGL ? "Real TGL — native topology" :
			   "RPL spoofed — slices=1 subslices=4(2DSS) maxEU/SS=8 totalEU=32 L3=4");

		return true;
		}();
	}

    return false;
}

void *ccont;
void *ccont2;  // AppleIntelBaseController pointer (captured in FBMemMgr_Init)

//FB Hooks

uint64_t Gen11::AppleIntelScalerinit(AppleIntel::AppleIntelScaler *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelScalerinit, callback->oAppleIntelScalerinit)(that, pipeIndex);
	that->fWriteAccessor = ccont;
	that->fController    = reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont2);
	return ret;
}

uint64_t Gen11::AppleIntelPlaneinit(AppleIntel::AppleIntelPlane *that, uint32_t pipeIndex)
{
	auto ret = FunctionCast(AppleIntelPlaneinit, callback->oAppleIntelPlaneinit)(that, pipeIndex);
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88

	return ret;
}

void Gen11::disableScaler(AppleIntel::AppleIntelScaler *that, bool disable)
{
	that->fWriteAccessor = ccont;
	FunctionCast(disableScaler, callback->odisableScaler)(that, disable);
}

void Gen11::enablePlane(AppleIntel::AppleIntelPlane *that, bool enable)
{
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(enablePlane, callback->oenablePlane)(that, enable);

}

void Gen11::programPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *displayPath)
{
	that->fWriteAccessor = ccont;
	FunctionCast(programPipeScaler, callback->oprogramPipeScaler)(that, displayPath);
}

// V400: AppleIntelScaler::setupPipeScaler(AppleIntelDisplayPath *, CRTCParams *)
// ORIGIN-LEVEL FIX for seam-joining scaler wrongly enabled on single-pipe panels.
//
// Apple's setupPipeScaler enters the "Seam joining scaler enabled" branch when
// a byte gate is non-zero — disasm reveals:
//   rax = this->[+0x10]                     (AppleIntelScaler's controller ptr)
//   if [rax+0x3FD8] == 0xFFFFFFFF (device-tree absent on spoofed RPL)
//       rax += 0x1E5
//   else
//       rax += 0x1E3
//   if [rax] != 0:  enable seam joining
//   else:           skip seam joining
//
// Linux i915 on this exact hardware confirms `bigjoiner: no, pipes: 0x0` —
// dual-pipe joining must NOT be active for this single-pipe 2560x1600 eDP panel.
// Apple is doing it anyway, producing the per-row offset / fragmented pattern.
//
// Fix: before calling original, zero BOTH gate bytes (+0x1E3 and +0x1E5) at
// this->[+0x10]. Belt-and-suspenders covers both device-tree-present and absent
// branches. Origin-level — modifies Apple's own decision input, not MMIO. The
// rest of Apple's code paths run unchanged with seam joining naturally skipped.
//
// Gated on !isRealTGL since real TGL doesn't need this.
//
// Post-call still logs CRTCParams scaler fields to verify SEAM_EXCESS==0.
void Gen11::setupPipeScaler(AppleIntel::AppleIntelScaler *that, AppleIntel::AppleIntelDisplayPath *path, AppleIntel::CRTCParams *params)
{
	// ccont fixup (same as programPipeScaler) — needed because V204 init hooks
	// don't always populate ccont; original would crash on this=NULL ccont path.
	that->fWriteAccessor = ccont;


	// Pre-call snapshot — captures values BEFORE setupPipeScaler runs so we can
	// tell whether THIS function sets PIPE_SEAM_EXCESS=0x1 or whether something
	// upstream did. Also captures the gate bytes our zero-the-gate attempt targeted.
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0, pre_hphase = 0;
	uint8_t pre_gate1E3 = 0xFF, pre_gate1E5 = 0xFF;
	bool have_base = false;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		if (params != nullptr) {
			pre_seam   = params->PIPE_SEAM_EXCESS;
			pre_winsz  = params->PS_PS_WIN_SZ;
			pre_winpos = params->PS_PS_WIN_POS;
			pre_hphase = params->PS_HPHASE;
		}
		if (that != nullptr) {
			auto *base = reinterpret_cast<uint8_t *>(that->fController);
			if (base != nullptr) {
				have_base    = true;
				pre_gate1E3  = base[0x1E3];
				pre_gate1E5  = base[0x1E5];
				if (!NGreen::callback->isRaptorLakeHX()) {
				base[0x1E3]  = 0;
				base[0x1E5]  = 0;
				}
			}
		}
	}

	FunctionCast(setupPipeScaler, callback->osetupPipeScaler)(that, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr)
		return;

	static int v400Count = 0;
	if (v400Count >= 12) return;
	++v400Count;

	// PIPE_SRCSZ per Intel spec: high 16 = horizontal-1, low 16 = vertical-1.
	const uint32_t src_w = ((params->PIPE_SRCSZ >> 16) & 0xFFFF) + 1;
	const uint32_t src_h = (params->PIPE_SRCSZ & 0xFFFF) + 1;
	SYSLOG("ngreen", "V400[%d]: setupPipeScaler %s gates[+0x1E3,+0x1E5]=(0x%02x,0x%02x) "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x | "
		   "POST: SRC=%ux%u SEAM=0x%x WINSZ=0x%x WINPOS=0x%x HPHASE=0x%x "
		   "HTOTAL=0x%x VTOTAL=0x%x TRANS_CONF=0x%x",
		   v400Count, have_base ? "base-ok" : "NO-base",
		   pre_gate1E3, pre_gate1E5,
		   pre_seam, pre_winsz, pre_winpos, pre_hphase,
		   src_w, src_h, params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS, params->PS_HPHASE,
		   params->TRANS_HTOTAL, params->TRANS_VTOTAL, params->TRANS_CONF);
}

// V401: AppleIntelBaseController::paramsSurfCompare — READ-ONLY logger.
// Fires per flip. Apple uses the return value to decide whether to fully reprogram
// the plane. We just log the inputs.
bool Gen11::paramsSurfCompare(AppleIntel::AppleIntelBaseController *that,
                              AppleIntel::CRTCParams *p1, AppleIntel::CRTCParams *p2,
                              AppleIntel::PLANEPARAMS *pl1, AppleIntel::PLANEPARAMS *pl2)
{
	// V408: force linear tiling in the NEW (pl2) PLANEPARAMS before the comparison.
	// hwRegsNeedUpdate has no PLANEPARAMS argument — it cannot fix tiling there.
	// paramsSurfCompare is the last point where pl2 can be patched before its values
	// trigger raWriteRegister32 writes for PLANE_CTL and PLANE_STRIDE.
	// Old (pl1/hardware) is already linear from UEFI; patching pl2 to match suppresses
	// the X-tiled commit without any MMIO intercept.
	/*if (NGreen::callback && !NGreen::callback->isRealTGL && pl2) {
		pl2->PLANE_CTL    = (pl2->PLANE_CTL & ~(0x7u << 10));  // bits[12:10] = 000 → linear
		pl2->PLANE_STRIDE = 0xa0;                               // 2560×4 / 64 = 160 = 0xa0
	}*/

	// Capture Apple's natural values BEFORE any force, for V401 logging.
	uint32_t nat_ctl_pl1    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t nat_stride_pl1 = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl1   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t nat_ctl_pl2    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t nat_stride_pl2 = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t nat_surf_pl2   = pl2 ? pl2->PLANE_SURF   : 0;

	bool ret = FunctionCast(paramsSurfCompare, callback->oparamsSurfCompare)(that, p1, p2, pl1, pl2);

	SYSLOG("ngreen", "V401, original ret [%d]", ret);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return ret;

	static int v401Count = 0;
	if (v401Count >= 20) return ret;
	++v401Count;

	uint32_t old_ctl    = pl1 ? pl1->PLANE_CTL    : 0;
	uint32_t new_ctl    = pl2 ? pl2->PLANE_CTL    : 0;
	uint32_t old_stride = pl1 ? pl1->PLANE_STRIDE : 0;
	uint32_t new_stride = pl2 ? pl2->PLANE_STRIDE : 0;
	uint32_t old_surf   = pl1 ? pl1->PLANE_SURF   : 0;
	uint32_t new_surf   = pl2 ? pl2->PLANE_SURF   : 0;
	uint32_t old_src    = p1  ? p1->PIPE_SRCSZ    : 0;
	uint32_t new_src    = p2  ? p2->PIPE_SRCSZ    : 0;

	// PLANE_CTL tiling is bits[12:10]: 000=linear, 001=X-tile, 100=Tile4.
	uint32_t old_tile     = (old_ctl     >> 10) & 0x7;
	uint32_t new_tile     = (new_ctl     >> 10) & 0x7;
	uint32_t nat_tile_pl1 = (nat_ctl_pl1 >> 10) & 0x7;
	uint32_t nat_tile_pl2 = (nat_ctl_pl2 >> 10) & 0x7;

	SYSLOG("ngreen", "V401[%d]: paramsSurfCompare ret=%d | "
		   "OLD: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NEW: CTL=0x%x tile=%u STRIDE=0x%x SURF=0x%x SRC=0x%x | "
		   "NAT plold: tile_pl1=%u stride_pl1=0x%x surf_pl1=0x%x | "
		   "NAT: plnew tile_pl2=%u stride_pl2=0x%x surf_pl2=0x%x",
		   v401Count, ret,
		   old_ctl, old_tile, old_stride, old_surf, old_src,
		   new_ctl, new_tile, new_stride, new_surf, new_src,
		   nat_tile_pl1, nat_stride_pl1, nat_surf_pl1,
		   nat_tile_pl2, nat_stride_pl2, nat_surf_pl2);

	return ret;
}

// AppleIntelBaseController::setupDSCEngineParams: latches the DSC parameters for the VDSC flush.
void Gen11::setupDSCEngineParams(AppleIntel::AppleIntelBaseController *that,
                                 AppleIntel::AppleIntelFramebuffer *fb,
                                 AppleIntel::CRTCParams *params,
								 AppleIntel::AppleIntelDisplayPath *path,
                                 IODetailedTimingInformationV2 *timing)
{
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX() && gTGLFbBase != 0)
		*(volatile uint64_t *)(gTGLFbBase + 0x1c2b90) = 1;

	FunctionCast(setupDSCEngineParams, callback->osetupDSCEngineParams)(that, fb, params, path, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	vdscLatch(params, path);
}

void Gen11::wrapEnableDSCEngine(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb,
                                AppleIntel::AppleIntelDisplayPath *path)
{
	vdscEnable(path);
	FunctionCast(wrapEnableDSCEngine, callback->oEnableDSCEngine)(that, fb, path);
}

void Gen11::wrapDisableDSCEngine(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb,
                                 AppleIntel::AppleIntelDisplayPath *path)
{
	FunctionCast(wrapDisableDSCEngine, callback->oDisableDSCEngine)(that, fb, path);
	vdscDisable(path);
}

static volatile uint32_t gTransFuncCtl = 0;

// From i915, drivers/gpu/drm/i915/display/intel_ddi.c, intel_ddi_enable_transcoder_clock: on display 12 TRANS_CLK_SEL takes
// TGL_TRANS_CLK_SEL_PORT(port). Here the port is the one a TRANS_DDI_FUNC_CTL value selects (its field holds port + 1); no port
// selected leaves the clock TGL_TRANS_CLK_SEL_DISABLED.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L1043-L1062
static inline uint32_t clkSelFromFuncCtl(uint32_t funcCtl) {
	const uint32_t sel = (funcCtl & TGL_TRANS_DDI_PORT_MASK) >> TGL_TRANS_DDI_PORT_SHIFT;
	return sel == 0 ? TGL_TRANS_CLK_SEL_DISABLED : TGL_TRANS_CLK_SEL_PORT(sel - 1u);
}

// V403: AppleIntelBaseController::SetupParams — post-call.
// Logs full CRTCParams + causation test: force PIPE_SEAM_EXCESS=0 and PS_PS_WIN_SZ=0
// so any seam-join config Apple set up earlier in the modeset is wiped after the
// master builder finishes. If fragmentation disappears with these zeroed, seam was
// the cause. If unchanged, seam fields are irrelevant and we look elsewhere.
void Gen11::setupParams(AppleIntel::AppleIntelBaseController *that,
                        AppleIntel::AppleIntelFramebuffer *fb,
						AppleIntel::AppleIntelDisplayPath *path,
                        AppleIntel::CRTCParams *params,
                        const IODetailedTimingInformationV2 *timing)
{
	FunctionCast(setupParams, callback->osetupParams)(that, fb, path, params, timing);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	if (NGreen::callback->isRaptorLakeHX()) {
		const uint32_t func = params->TRANS_DDI_FUNC_CTL;
		const uint32_t clk  = params->TRANS_CLK_SEL;
		const uint32_t want = clkSelFromFuncCtl(func);
		gTransFuncCtl = func;
		if (want != 0 && clk == 0) params->TRANS_CLK_SEL = want;
	}

	uint32_t pre_seam  = params->PIPE_SEAM_EXCESS;
	uint32_t pre_winsz = params->PS_PS_WIN_SZ;
	uint32_t pre_hphase = params->PS_HPHASE;
	if (!NGreen::callback->isRaptorLakeHX() && (pre_seam != 0 || pre_winsz != 0 || pre_hphase != 0)) {
		params->PIPE_SEAM_EXCESS = 0;
		params->PS_PS_WIN_SZ     = 0;
		params->PS_HPHASE        = 0;
	}

	static int v403Count = 0;
	if (v403Count >= 6) return;
	++v403Count;

	SYSLOG("ngreen", "V403[%d]: SetupParams post-call CRTCParams: "
		   "CLK_SEL=0x%x DDI_FUNC_CTL=0x%x DDI_FUNC_CTL2=0x%x MSA_MISC=0x%x "
		   "HTOTAL=0x%x HBLANK=0x%x HSYNC=0x%x VTOTAL=0x%x VBLANK=0x%x VSYNC=0x%x "
		   "PIPE_SRCSZ=0x%x TRANS_CONF=0x%x | "
		   "PS_WIN_POS=0x%x | PRE: SEAM=0x%x WINSZ=0x%x HPHASE=0x%x | "
		   "POST: SEAM=0x%x WINSZ=0x%x HPHASE=0x%x | "
		   "DSC_ENGINE=0x%x DSC_JOINER=0x%x PPS_0=0x%x PPS_16=0x%x",
		   v403Count,
		   params->TRANS_CLK_SEL, params->TRANS_DDI_FUNC_CTL, params->TRANS_DDI_FUNC_CTL2, params->TRANS_MSA_MISC,
		   params->TRANS_HTOTAL, params->TRANS_HBLANK, params->TRANS_HSYNC,
		   params->TRANS_VTOTAL, params->TRANS_VBLANK, params->TRANS_VSYNC,
		   params->PIPE_SRCSZ, params->TRANS_CONF,
		   params->PS_PS_WIN_POS,
		   pre_seam, pre_winsz, pre_hphase,
		   params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_HPHASE,
		   params->DSC_ENGINE_SEL, params->DSC_JOINER_CTL, params->PPS_0, params->PPS_16);
}

// V404: AppleIntelBaseController::setupPipeWatermarks — READ-ONLY pre/post.
// Tells us if setupPipeWatermarks is what sets PIPE_SEAM_EXCESS=0x1. Called
// from inside SetupParams before setupPipeScaler — if pre=0 post=1 here, this
// is our seam-join origin (rather than setupPipeScaler).
void Gen11::setupPipeWatermarks(AppleIntel::AppleIntelBaseController *that,
                                AppleIntel::AppleIntelFramebuffer *fb,
								AppleIntel::AppleIntelDisplayPath *path,
                                AppleIntel::CRTCParams *params)
{
	uint32_t pre_seam = 0, pre_winsz = 0, pre_winpos = 0;
	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL && params != nullptr) {
		pre_seam   = params->PIPE_SEAM_EXCESS;
		pre_winsz  = params->PS_PS_WIN_SZ;
		pre_winpos = params->PS_PS_WIN_POS;
	}

	FunctionCast(setupPipeWatermarks, callback->osetupPipeWatermarks)(that, fb, path, params);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || params == nullptr) return;

	static int v404Count = 0;
	if (v404Count >= 4) return;
	++v404Count;

	SYSLOG("ngreen", "V404[%d]: setupPipeWatermarks | "
		   "PRE: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x | "
		   "POST: SEAM=0x%x WINSZ=0x%x WINPOS=0x%x %s",
		   v404Count,
		   pre_seam, pre_winsz, pre_winpos,
		   params->PIPE_SEAM_EXCESS, params->PS_PS_WIN_SZ, params->PS_PS_WIN_POS,
		   (pre_seam == 0 && params->PIPE_SEAM_EXCESS != 0)
			   ? "*** SEAM SET HERE ***"
			   : (pre_seam != 0 ? "(seam pre-existed)" : "(no seam)"));
}

// V405: AppleIntelPlane::configureColorPipeLine(FlipTransactionArgs*, bool)
//
// Dispatches to configurePipePostCSCGamma_{8,10,12,12SEG}Bit based on a BPC/gamma
// mode selector field within FlipTransactionArgs.  The field is at byte offset 0x1C
// (IDA's "param_1[7].Tiling" — element 7 of a dword array inside the struct).
//
// For our 2560×1600 eDP at 8bpc this always hits case 0 → 8Bit, which writes:
//   GAMMA_MODE   (0x4A480, Pipe A) = 0x0  (LEGACY_8BIT)
//   PIPE_MISC    (0x70030, Pipe A) bits[5:3] = 0b000 (8 bpc)
//
// Display 13 (ADL-P) uses the same register addresses as Display 12 (TGL), so
// Apple's writes are structurally correct.  The hook logs pre/post snapshots so
// we can confirm the hardware actually sees the right values each flip cycle.
void Gen11::configureColorPipeLine(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs, bool param_2)
{
	static int v405Count = 0;

	uint32_t pre_gamma = 0, pre_misc = 0;
	uint8_t  sel = 0xFF;

	if (NGreen::callback != nullptr && !NGreen::callback->isRealTGL) {
		pre_gamma = NGreen::callback->readReg32(0x4A480);  // GAMMA_MODE Pipe A
		pre_misc  = NGreen::callback->readReg32(0x70030);  // PIPE_MISC  Pipe A
		if (flipArgs != nullptr)
			sel = static_cast<uint8_t>(*reinterpret_cast<const uint32_t *>(&flipArgs->flt_001C) >> 24);
	}

	FunctionCast(configureColorPipeLine, callback->oConfigureColorPipeLine)(that, flipArgs, param_2);

	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL) return;

	if (v405Count >= 20) return;
	++v405Count;

	const uint32_t post_gamma = NGreen::callback->readReg32(0x4A480);
	const uint32_t post_misc  = NGreen::callback->readReg32(0x70030);
	// PIPE_MISC BPC field: bits[5:3].  0=8bpc, 1=10bpc, 2=6bpc, 3=12bpc.
	const uint8_t bpc_pre  = (pre_misc  >> 3) & 0x7u;
	const uint8_t bpc_post = (post_misc >> 3) & 0x7u;
	// GAMMA_MODE encoding: 0=legacy8bit, 1=prec10bit, 2=prec12interp, 3=split
	const char *gammaName[] = {"LEGACY_8BIT", "PREC_10BIT", "PREC_12INTERP", "SPLIT"};
	const char *gname = (post_gamma <= 3) ? gammaName[post_gamma] : "unknown";

	SYSLOG("ngreen", "V405[%d]: configureColorPipeLine sel=0x%02x param2=%d | "
		   "GAMMA_MODE: 0x%x→0x%x(%s) | PIPE_MISC: 0x%x→0x%x bpc[5:3]=%u→%u",
		   v405Count, sel, (int)param_2,
		   pre_gamma, post_gamma, gname,
		   pre_misc, post_misc, bpc_pre, bpc_post);
}

// V406: CORE origin-level plane tiling/stride manipulation for ADL-P/RPL spoofed as TGL.
// Physical framebuffer pages are CPU-written in linear order. Apple's IOSurface allocates
// with tiling enum=0 (X-tiled) → configurePlane ORs PLANE_CTL bit10 → display engine
// fetches X-tile geometry → black screen.
//
// Linux i915 on this exact ADL-P hardware: modifier=0x0 (LINEAR), stride=0x2800 bytes (0x14 units)
//
// FlipTransactionArgs+0x3c tiling enum (from disasm VA 0x59ad-0x59c3):
//   0x0 → X-tiled:  ORs 0x400 into PLANE_CTL bits[12:10] = 001
//   0x1 → Y-tiled:  ORs 0x1000 into bits[12:10] = 100
//   else → linear:  ANDs 0xffe7e7ff (clears bits[12:10] = 000)
//
// AppleIntelPlane shadow offsets (from configurePlane disasm):
//   +0x100 = PLANE_CTL shadow (tiling bits built here, read @ VA 0x5a41)
//   +0x104 = PLANE_STRIDE shadow (read @ VA 0x6867)
//   +0x154 = PLANE_COLOR_CTL shadow (post-OR write @ VA 0x6687)
//
// ============================================================================
// === CHOOSE ONE: Main approach (uncomment your choice below) ===
// ============================================================================

// ✓ APPROACH 1: PRE-CALL TILING PATCH (origin-level, Apple's natural path)
// #define V406_APPROACH_TILING_PATCH 1

// Alternative approaches below (comment out APPROACH 1 if trying these):
// #define V406_APPROACH_POST_SHADOW_PATCH 1      // Post-call shadow field write
// #define V406_APPROACH_STRIDE_DIVISOR 1          // Stride divisor manipulation
// #define V406_APPROACH_DUAL_TILING_STRIDE 1      // Tiling + stride combined
#define V406_APPROACH_DIAGNOSTIC_ONLY 1         // Pass-through with logging

// ============================================================================
// === TILING ENUM SELECTOR (active for approaches using pre-call patch) ===
// ============================================================================

// #define V406_TILING_VALUE 2       // linear — CPU BAR2 compositor path
// #define V406_TILING_VALUE 1       // Y-tiled
#define V406_TILING_VALUE 0          // X-tiled — matches Apple IOSurface allocator

// ============================================================================
// === STRIDE FORCE (active for stride-manipulation approaches) ===
// ============================================================================

#define V406_STRIDE_FORCE_ENABLE 0              // stride handled by paramsSurfCompare V408
#define V406_STRIDE_VALUE 0x14                  // X-tile: 2560×4 / 512 = 20 = 0x14

// ============================================================================
// === PLANE_CTL BITS MANIPULATION (advanced—leave as 0 unless testing) ===
// ============================================================================

#define V406_PLANE_CTL_CLEAR_MASK 0x0           // Bits to clear (0xFFE7E7FF = tiling bits)
#define V406_PLANE_CTL_SET_MASK 0x0             // Bits to set (0x400=X-tiled, 0x1000=Y-tiled)

// ============================================================================
// === SHADOW FIELD OFFSET OVERRIDES (for post-call patching) ===
// ============================================================================

#define V406_STRIDE_SHADOW_OFFSET 0x104         // AppleIntelPlane+0x104 = PLANE_STRIDE
#define V406_CTL_SHADOW_OFFSET 0x100            // AppleIntelPlane+0x100 = PLANE_CTL

// ============================================================================

void Gen11::configurePlane(AppleIntel::AppleIntelPlane *that, AppleIntel::FlipTransactionArgs *flipArgs)
{
	if (NGreen::callback == nullptr || NGreen::callback->isRealTGL || flipArgs == nullptr) {
		FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);
		return;
	}

	// Pre-call snapshot
	uint32_t incomingTiling = 0;
	if (flipArgs != nullptr)
		incomingTiling = flipArgs->TilingEnum;

	static int v406CallCount = 0;
	static int v406LogCount = 0;
	++v406CallCount;

#ifdef V406_APPROACH_TILING_PATCH
	// ===== APPROACH 1: PRE-CALL TILING PATCH =====
	// Patch FlipTransactionArgs+0x3c before calling original
	// Apple's configurePlane reads this, takes the corresponding disasm branch,
	// builds PLANE_CTL with correct tiling bits natively
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: TILING_PATCH call#%d | "
			   "Incoming tiling=0x%x → Patched=0x%x (%s)",
			   v406LogCount, v406CallCount, incomingTiling, V406_TILING_VALUE,
			   (V406_TILING_VALUE == 0) ? "X-tiled" :
			   (V406_TILING_VALUE == 1) ? "Y-tiled" : "linear/else");
	}

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;   // restore

#elif defined(V406_APPROACH_POST_SHADOW_PATCH)
	// ===== APPROACH 2: POST-CALL SHADOW PATCH =====
	// Call original, then modify AppleIntelPlane shadow fields directly
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (that != nullptr) {
		// Patch PLANE_CTL shadow at +0x100 (or custom offset)
		uint32_t &ctlShadow = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		const uint32_t ctlBefore = ctlShadow;
		
		// Clear tiling bits if needed
		if (V406_PLANE_CTL_CLEAR_MASK != 0)
			ctlShadow &= ~V406_PLANE_CTL_CLEAR_MASK;
		
		// Set specific bits if needed
		if (V406_PLANE_CTL_SET_MASK != 0)
			ctlShadow |= V406_PLANE_CTL_SET_MASK;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: POST_SHADOW_PATCH call#%d | "
				   "PLANE_CTL shadow @+0x%x: 0x%x → 0x%x (clear=0x%x set=0x%x)",
				   v406LogCount, v406CallCount, V406_CTL_SHADOW_OFFSET,
				   ctlBefore, ctlShadow, V406_PLANE_CTL_CLEAR_MASK, V406_PLANE_CTL_SET_MASK);
		}

		// Optionally patch STRIDE shadow
		if (V406_STRIDE_FORCE_ENABLE) {
			uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
			const uint32_t strideBefore = strideShadow;
			strideShadow = V406_STRIDE_VALUE;
			if (v406LogCount < 20) {
				SYSLOG("ngreen", "V406[%d]: STRIDE shadow @+0x%x: 0x%x → 0x%x",
					   v406LogCount, V406_STRIDE_SHADOW_OFFSET, strideBefore, V406_STRIDE_VALUE);
			}
		}
	}

#elif defined(V406_APPROACH_STRIDE_DIVISOR)
	// ===== APPROACH 3: STRIDE DIVISOR (experimental) =====
	// Patch stride divisor used in configurePlane's pitch/divisor calculation
	// WARNING: This requires knowing the exact location where stride divisor is stored
	// (likely in a local stack variable or controller object). This is highly experimental.
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: STRIDE_DIVISOR call#%d | "
			   "Attempting stride divisor override (experimental) | "
			   "Incoming tiling=0x%x",
			   v406LogCount, v406CallCount, incomingTiling);
	}

	// NOTE: Actual stride divisor patching would require identifying the controller
	// field or register that holds 0x200 (stride divisor for X-tiled).
	// This approach is a placeholder—override if you find the divisor location.

#elif defined(V406_APPROACH_DUAL_TILING_STRIDE)
	// ===== APPROACH 4: DUAL TILING + STRIDE =====
	// Combine pre-call tiling patch + post-call stride shadow patch
	
	uint32_t *tilingField = &flipArgs->TilingEnum;
	const uint32_t savedTiling = *tilingField;
	*tilingField = V406_TILING_VALUE;

	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	*tilingField = savedTiling;

	// Post-call: patch STRIDE shadow
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr) {
		uint32_t &strideShadow = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		const uint32_t strideBefore = strideShadow;
		strideShadow = V406_STRIDE_VALUE;
		
		if (v406LogCount < 20) {
			++v406LogCount;
			SYSLOG("ngreen", "V406[%d]: DUAL_TILING_STRIDE call#%d | "
				   "Tiling=0x%x STRIDE shadow: 0x%x → 0x%x",
				   v406LogCount, v406CallCount, V406_TILING_VALUE,
				   strideBefore, V406_STRIDE_VALUE);
		}
	}

#else
	// ===== APPROACH 5: DIAGNOSTIC PASS-THROUGH =====
	// No changes—pure logging to see original behavior
	
	FunctionCast(configurePlane, callback->oConfigurePlane)(that, flipArgs);

	if (v406LogCount < 20) {
		++v406LogCount;
		SYSLOG("ngreen", "V406[%d]: DIAGNOSTIC_ONLY call#%d | "
			   "Incoming tiling=0x%x (unchanged)",
			   v406LogCount, v406CallCount, incomingTiling);
	}
#endif

	// Always log final state if STRIDE monitoring enabled
	if (V406_STRIDE_FORCE_ENABLE && that != nullptr && v406LogCount < 20) {
		uint32_t ctlFinal = getMember<uint32_t>(that, V406_CTL_SHADOW_OFFSET);
		uint32_t strideFinal = getMember<uint32_t>(that, V406_STRIDE_SHADOW_OFFSET);
		SYSLOG("ngreen", "V406-final[%d]: CTL@+0x%x=0x%x STRIDE@+0x%x=0x%x",
			   v406LogCount, V406_CTL_SHADOW_OFFSET, ctlFinal,
			   V406_STRIDE_SHADOW_OFFSET, strideFinal);
	}
}

void Gen11::AppleIntelPlaneupdateRegisterCache(AppleIntel::AppleIntelPlane *that)
{
	getMember<void *>(that, 0x90) = ccont; // fWriteAccessor — ccont must NOT go to real fRegCache at +0x88
	FunctionCast(AppleIntelPlaneupdateRegisterCache, callback->oAppleIntelPlaneupdateRegisterCache)(that);
}

void Gen11::AppleIntelScalerupdateRegisterCache(AppleIntel::AppleIntelScaler *that)
{
	that->fWriteAccessor = ccont;
	FunctionCast(AppleIntelScalerupdateRegisterCache, callback->oAppleIntelScalerupdateRegisterCache)(that);
}

void Gen11::disableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disableDisplayEngine, callback->odisableDisplayEngine)(that );
}

void Gen11::enableDisplayEngine(AppleIntel::AppleIntelBaseController *that)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enableDisplayEngine, callback->oenableDisplayEngine)(that );
}

void Gen11::computeLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int linkRate, unsigned int bpp, unsigned int *laneCount) {
	if (!laneCount) return;

	// Call original first — handles all standard DP rates on both real TGL and spoofed paths.
	FunctionCast(computeLaneCount, callback->ocomputeLaneCount)(that, timing, linkRate, bpp, laneCount);

	}

// From i915, drivers/gpu/drm/i915/display/intel_dpll_mgr.c: the combo PLL parameters of a DP link (icl_dp_combo_pll_19_2MHz_values, the
// table of a 38.4 MHz reference too; icl_calc_dp_combo_pll picks the row by port clock), the register values icl_calc_dpll_state makes of
// them (on display 12 with a 38.4 MHz reference the DCO fraction is halved: ehl_combo_pll_div_frac_wa_needed), and the sequences
// icl_pll_disable and combo_pll_enable (icl_pll_power_enable, icl_dpll_write, icl_pll_enable; each wait is 1 ms). The reference clock is
// read as icl_readout_refclk reads it (intel_cdclk.c, SKL_DSSM). The registers: display/intel_display_regs.h; ADL-S's DPLL0 has the TGL
// layout (_DPLL0_ENABLE, _TGL_DPLL0_CFGCR0 / CFGCR1).
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L1606-L1614
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L2628-L2700
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L2915-L2940
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L3812-L3852
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L3960-L4034
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dpll_mgr.c#L4082-L4111
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_cdclk.c#L1766-L1782
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L1781-L1785
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L2936-L2943
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L2992-L3038
struct skl_wrpll_params { uint32_t dco_fraction, dco_integer, qdiv_ratio, qdiv_mode, kdiv, pdiv; };
struct icl_combo_pll_params { int clock; skl_wrpll_params wrpll; };

/* Also used for 38.4 MHz values. */
static const icl_combo_pll_params icl_dp_combo_pll_19_2MHz_values[] = {
	{ 540000, { 0x7000, 0x1A5, 0, 0, 1, 0x2 /* 3 */ } },	/* [0]: 5.4 */
	{ 270000, { 0x7000, 0x1A5, 0, 0, 2, 0x2 /* 3 */ } },	/* [1]: 2.7 */
	{ 162000, { 0x7000, 0x1A5, 0, 0, 2, 0x4 /* 5 */ } },	/* [2]: 1.62 */
	{ 324000, { 0x7000, 0x1A5, 0, 0, 1, 0x4 /* 5 */ } },	/* [3]: 3.24 */
	{ 216000, { 0x0000, 0x1C2, 2, 1, 2, 0x1 /* 2 */ } },	/* [4]: 2.16 */
	{ 432000, { 0x0000, 0x1C2, 0, 0, 2, 0x1 /* 2 */ } },	/* [5]: 4.32 */
	{ 648000, { 0x2000, 0x1FA, 0, 0, 1, 0x2 /* 3 */ } },	/* [6]: 6.48 */
	{ 810000, { 0x7000, 0x1A5, 0, 0, 1, 0x1 /* 2 */ } },	/* [7]: 8.1 */
};

static constexpr uint32_t _DPLL0_ENABLE     = 0x46010u;
static constexpr uint32_t PLL_ENABLE        = 1u << 31;
static constexpr uint32_t PLL_LOCK          = 1u << 30;
static constexpr uint32_t PLL_POWER_ENABLE  = 1u << 27;
static constexpr uint32_t PLL_POWER_STATE   = 1u << 26;
static constexpr uint32_t _TGL_DPLL0_CFGCR0 = 0x164284u;
static constexpr uint32_t _TGL_DPLL0_CFGCR1 = 0x164288u;
#define DPLL_CFGCR0_DCO_FRACTION(x)	((x) << 10)
#define DPLL_CFGCR1_QDIV_RATIO(x)	((x) << 10)
#define DPLL_CFGCR1_QDIV_MODE(x)	((x) << 9)
#define DPLL_CFGCR1_KDIV(x)		((x) << 6)
#define DPLL_CFGCR1_PDIV(x)		((x) << 2)
#define TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL	(0 << 0)

// drm_dp_bw_code_to_link_rate (drm_dp_helper.c): a DP_LINK_BW_SET code counts the link rate in 27 MHz units.
static inline int drm_dp_bw_code_to_link_rate(uint8_t link_bw) { return link_bw * 27000; }

static bool icl_calc_dp_combo_pll(int clock, skl_wrpll_params &pll_params) {
	for (const auto &row : icl_dp_combo_pll_19_2MHz_values) {
		if (clock == row.clock) {
			pll_params = row.wrpll;
			return true;
		}
	}
	return false;
}

static void icl_calc_dpll_state(uint32_t ref_clk, const skl_wrpll_params &pll_params, uint32_t &cfgcr0, uint32_t &cfgcr1) {
	uint32_t dco_fraction = pll_params.dco_fraction;
	if (ref_clk == 38400)	/* ehl_combo_pll_div_frac_wa_needed: display 12 with a 38.4 MHz reference */
		dco_fraction = (dco_fraction + 1u) / 2u;	/* DIV_ROUND_CLOSEST(dco_fraction, 2) */
	cfgcr0 = DPLL_CFGCR0_DCO_FRACTION(dco_fraction) | pll_params.dco_integer;
	cfgcr1 = DPLL_CFGCR1_QDIV_RATIO(pll_params.qdiv_ratio) | DPLL_CFGCR1_QDIV_MODE(pll_params.qdiv_mode) |
	         DPLL_CFGCR1_KDIV(pll_params.kdiv) | DPLL_CFGCR1_PDIV(pll_params.pdiv) | TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL;
}

static uint32_t icl_readout_refclk() {
	switch (NGreen::callback->readReg32(SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz: return 19200;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz: return 38400;
	default:                                return 24000;
	}
}

/* intel_de_wait_for_set_ms / intel_de_wait_for_clear_ms, polled every microsecond */
static bool intel_de_wait_ms(uint32_t reg, uint32_t mask, bool set, uint32_t ms) {
	auto *cb = NGreen::callback;
	for (uint32_t us = 0; us < ms * 1000u; us++) {
		if (((cb->readReg32(reg) & mask) != 0) == set) return true;
		IODelay(1);
	}
	return ((cb->readReg32(reg) & mask) != 0) == set;
}

static void icl_pll_power_enable(uint32_t enable_reg) {
	NGreen::callback->intel_de_rmw(enable_reg, 0, PLL_POWER_ENABLE);
	/* The spec says we need to "wait" but it also says it should be immediate. */
	intel_de_wait_ms(enable_reg, PLL_POWER_STATE, true, 1);
}

static void icl_pll_enable(uint32_t enable_reg) {
	NGreen::callback->intel_de_rmw(enable_reg, 0, PLL_ENABLE);
	/* Timeout is actually 600us. */
	intel_de_wait_ms(enable_reg, PLL_LOCK, true, 1);
}

static void icl_dpll_write(uint32_t cfgcr0, uint32_t cfgcr1) {
	auto *cb = NGreen::callback;
	cb->writeReg32(_TGL_DPLL0_CFGCR0, cfgcr0);
	cb->writeReg32(_TGL_DPLL0_CFGCR1, cfgcr1);
	(void)cb->readReg32(_TGL_DPLL0_CFGCR1);
}

static void combo_pll_enable(uint32_t enable_reg, uint32_t cfgcr0, uint32_t cfgcr1) {
	icl_pll_power_enable(enable_reg);
	icl_dpll_write(cfgcr0, cfgcr1);
	/* DVFS pre sequence would be here, but the cdclk code paths should already be setting the voltage: nothing here. */
	icl_pll_enable(enable_reg);
	/* DVFS post sequence would be here. See the comment above. */
}

static void icl_pll_disable(uint32_t enable_reg) {
	auto *cb = NGreen::callback;
	/* The first steps are done by intel_ddi_post_disable(). */
	cb->intel_de_rmw(enable_reg, PLL_ENABLE, 0);
	/* Timeout is actually 1us. */
	intel_de_wait_ms(enable_reg, PLL_LOCK, false, 1);
	cb->intel_de_rmw(enable_reg, PLL_POWER_ENABLE, 0);
	/* The spec says we need to "wait" but it also says it should be immediate. */
	intel_de_wait_ms(enable_reg, PLL_POWER_STATE, false, 1);
}

// Apple's framebuffer leaves DPLL0 set for a lower rate than the one it then trains (boot #66: 3.24 Gbps under a 5.4 link), so before
// its link training the PLL is set for the rate the sink will be asked for, in i915's order (icl_pll_disable, combo_pll_enable). i915
// touches a PLL only after intel_ddi_post_disable has turned the port off; when Apple's port is still on the PLL is left alone. The 24 MHz
// table is not carried: this platform's reference is 38.4 MHz (the BIOS's CFGCR0 0x00E001A5 holds the halved fraction).
static void programDPLL0(uint8_t rateCode) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || !cb->mmioMapped()) return;

	skl_wrpll_params pll_params = {};
	const int port_clock = drm_dp_bw_code_to_link_rate(rateCode);
	if (!icl_calc_dp_combo_pll(port_clock, pll_params)) return;
	const uint32_t ref_clk = icl_readout_refclk();
	if (ref_clk == 24000) return;
	uint32_t cfgcr0 = 0, cfgcr1 = 0;
	icl_calc_dpll_state(ref_clk, pll_params, cfgcr0, cfgcr1);

	const uint32_t curC0 = cb->readReg32(_TGL_DPLL0_CFGCR0);
	const uint32_t curC1 = cb->readReg32(_TGL_DPLL0_CFGCR1);
	if (curC0 == cfgcr0 && curC1 == cfgcr1) return;
	if ((cb->readReg32(DDI_BUF_CTL_A) & DDI_BUF_CTL_ENABLE) != 0) return;

	icl_pll_disable(_DPLL0_ENABLE);
	combo_pll_enable(_DPLL0_ENABLE, cfgcr0, cfgcr1);
	const uint32_t en = cb->readReg32(_DPLL0_ENABLE);
	SYSLOG("ngreen", "DPLL0 set for link rate code 0x%x (%d kHz, ref %u kHz): CFGCR0 0x%x -> 0x%x, CFGCR1 0x%x -> 0x%x, DPLL0_ENABLE 0x%x (power %u, lock %u)",
	       rateCode, port_clock, ref_clk, curC0, cfgcr0, curC1, cfgcr1, en, (en & PLL_POWER_STATE) != 0 ? 1u : 0u, (en & PLL_LOCK) != 0 ? 1u : 0u);
}



static constexpr uint32_t kTxDw4UpperMask   = 0x7FFC0000u;
static volatile uint32_t  gTxDw4Upper       = 0;
static volatile uint32_t  gTxDw4UpperValid  = 0;

static volatile uint32_t gSinkPowerDone = 0;

static constexpr mach_vm_address_t kWriteAUXOffset = 0x290BEu;
using AUXFn = IOReturn (*)(void *, uint32_t, void *, uint32_t);

// From i915, drivers/gpu/drm/i915/display/intel_pps_regs.h and intel_pps.c: the panel power sequencer at the PCH's PPS base (this
// platform's mmio_base is PCH_PPS_BASE, 0xC7200), the fields of PP_STATUS and PP_CONTROL, wait_panel_status's masks and its 10 ms / 5 s
// poll, and the cycle delay intel_pps_readout_hw_state takes from BXT_POWER_CYCLE_DELAY_MASK ("<delay>+1 in 100ms units"; pps_init_delays
// uses the larger of the hardware's and the VBT's, which this driver does not read, and the eDP spec's T11+T12 when both are 0).
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps_regs.h#L14-L55
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L594-L643
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L1368-L1400
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L1497-L1575
static constexpr uint32_t PCH_PPS_BASE = 0xC7200u;
static constexpr uint32_t PP_STATUS    = PCH_PPS_BASE + 0x0u;
static constexpr uint32_t PP_CONTROL   = PCH_PPS_BASE + 0x4u;
static constexpr uint32_t PP_ON                      = 1u << 31;
static constexpr uint32_t PP_SEQUENCE_MASK           = 3u << 28;
static constexpr uint32_t PP_SEQUENCE_NONE           = 0u << 28;
static constexpr uint32_t PP_CYCLE_DELAY_ACTIVE      = 1u << 27;
static constexpr uint32_t PP_SEQUENCE_STATE_MASK     = 0xFu;
static constexpr uint32_t PP_SEQUENCE_STATE_OFF_IDLE = 0x0u;
static constexpr uint32_t PP_SEQUENCE_STATE_ON_IDLE  = 0x8u;
static constexpr uint32_t BXT_POWER_CYCLE_DELAY_MASK = 0x1Fu << 4;
static constexpr uint32_t EDP_FORCE_VDD     = 1u << 3;
static constexpr uint32_t EDP_BLC_ENABLE    = 1u << 2;
static constexpr uint32_t PANEL_POWER_RESET = 1u << 1;
static constexpr uint32_t PANEL_POWER_ON    = 1u << 0;
static constexpr uint32_t IDLE_ON_MASK     = PP_ON | PP_SEQUENCE_MASK | 0                     | PP_SEQUENCE_STATE_MASK;
static constexpr uint32_t IDLE_ON_VALUE    = PP_ON | PP_SEQUENCE_NONE | 0                     | PP_SEQUENCE_STATE_ON_IDLE;
static constexpr uint32_t IDLE_OFF_MASK    = PP_ON | PP_SEQUENCE_MASK | 0                     | 0;
static constexpr uint32_t IDLE_OFF_VALUE   = 0     | PP_SEQUENCE_NONE | 0                     | 0;
static constexpr uint32_t IDLE_CYCLE_MASK  = PP_ON | PP_SEQUENCE_MASK | PP_CYCLE_DELAY_ACTIVE | PP_SEQUENCE_STATE_MASK;
static constexpr uint32_t IDLE_CYCLE_VALUE = 0     | PP_SEQUENCE_NONE | 0                     | PP_SEQUENCE_STATE_OFF_IDLE;
static constexpr uint32_t kPpsAuxTries     = 10u;  // drm_dp_dpcd_access retries a transfer that fails right after the panel comes up

static void wait_panel_status(uint32_t mask, uint32_t value) {
	auto *cb = NGreen::callback;
	for (uint32_t elapsed = 0; (cb->readReg32(PP_STATUS) & mask) != value && elapsed < 5000u; elapsed += 10u)
		IOSleep(10);
}

// pps_init_delays: the hardware's cycle delay, or the eDP spec's 510 ms (T11+T12) when the register holds none.
static uint32_t panel_power_cycle_delay(uint32_t pp_ctl) {
	const uint32_t power_cycle_delay = (pp_ctl & BXT_POWER_CYCLE_DELAY_MASK) >> 4;
	const uint32_t hw = power_cycle_delay != 0 ? (power_cycle_delay - 1u) * 100u : 0u;
	return hw != 0 ? hw : 510u;
}

static constexpr uint32_t kPpsSinkAddrs[] = {
	0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00D, 0x00E, 0x00F,
	0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109, 0x10A, 0x10B, 0x10C, 0x10D, 0x10E, 0x10F,
	0x160, 0x170, 0x171, 0x172, 0x300, 0x301, 0x302, 0x600,
	0x700, 0x701, 0x702, 0x703, 0x704, 0x705, 0x706, 0x707, 0x720, 0x721, 0x722, 0x723, 0x724, 0x725, 0x726, 0x727,
};


static volatile uint32_t gPhyTraining = 0;
static volatile uint32_t gLinkRateCode = 0;	// the DP_LINK_BW_SET code of the training in progress (the loadgen rule's port clock)
static volatile uint32_t gPhyProbed   = 0;

static constexpr uint32_t DP_TP_STATUS_IDLE_DONE = 1u << 25;

// From i915, drivers/gpu/drm/i915/display/intel_ddi.c, intel_ddi_set_idle_link_train: the idle pattern selected in DP_TP_CTL, then
// DP_TP_STATUS's idle-done bit awaited for 2 ms (display 12 waits on port A too). The registers: IntelDPLinkTraining.hpp's names.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L3878-L3903
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L2587-L2591
static void intel_ddi_set_idle_link_train() {
	NGreen::callback->intel_de_rmw(TGL_DP_TP_CTL_A, DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_IDLE);
	intel_de_wait_ms(TGL_DP_TP_STATUS_A, DP_TP_STATUS_IDLE_DONE, true, 2);
}

// A probe at the first LANE0_1_STATUS read of a training: the sink's rate select and rate table are read, the link is put through one
// idle-pattern period as intel_ddi_set_idle_link_train does, and the pattern Apple's trainer had selected is restored (i915 sends the
// idle pattern only after a training; the restore is this probe's).
static void linkTrainingIdleProbe(void *that, AUXFn aux) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || !cb->mmioMapped()) return;

	uint8_t rs = 0xFF, tbl[16];
	for (unsigned i = 0; i < sizeof(tbl); i++) tbl[i] = 0xFF;
	aux(that, DP_LINK_RATE_SET, &rs, 1);
	aux(that, DP_SUPPORTED_LINK_RATES, tbl, sizeof(tbl));

	const uint32_t cur = cb->readReg32(TGL_DP_TP_CTL_A);
	intel_ddi_set_idle_link_train();
	cb->writeReg32(TGL_DP_TP_CTL_A, cur);
	(void)cb->readReg32(TGL_DP_TP_STATUS_A);
}

// From i915, drivers/gpu/drm/i915/display/intel_dp.c: intel_dp_init_source_oui (the source OUI read first and written only when it
// differs, "so as to avoid clearing any state by accident") and intel_dp_set_power (D3 in one write; D0 after the OUI, retried three
// times a millisecond apart "to give the sink time to wake up"). Each write here retries three times, as the DRM AUX helper retries.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dp.c#L3910-L3943
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dp.c#L3965-L4008
static IOReturn intel_dp_init_source_oui(void *that, AUXFn readAUX, AUXFn writeAUX, bool *written) {
	uint8_t oui[] = { 0x00, 0xaa, 0x01 };
	uint8_t buf[3] = {};

	*written = false;
	const IOReturn ret = readAUX(that, DP_SOURCE_OUI, buf, sizeof(buf));
	if (ret != kIOReturnSuccess) return ret;
	if (memcmp(oui, buf, sizeof(oui)) == 0) return kIOReturnSuccess;
	*written = true;
	return writeAUX(that, DP_SOURCE_OUI, oui, sizeof(oui));
}

static IOReturn intel_dp_set_power(void *that, AUXFn readAUX, AUXFn writeAUX, uint8_t mode, bool *oui_written) {
	IOReturn ret = kIOReturnError;
	if (mode != DP_SET_POWER_D0) {
		for (int i = 0; i < 3; i++) {
			ret = writeAUX(that, DP_SET_POWER, &mode, 1);
			if (ret == kIOReturnSuccess) break;
			IOSleep(1);
		}
	} else {
		/* Write the source OUI as early as possible */
		intel_dp_init_source_oui(that, readAUX, writeAUX, oui_written);
		/*
		 * When turning on, we need to retry for 1ms to give the sink
		 * time to wake up.
		 */
		for (int i = 0; i < 3; i++) {
			ret = writeAUX(that, DP_SET_POWER, &mode, 1);
			if (ret == kIOReturnSuccess) break;
			IOSleep(1);
		}
	}
	return ret;
}

IOReturn Gen11::wrapLinkTraining(void *that, void *config) {
	clearVdscGatePipeOff();
	if (callback->orgICLReadAUX != 0 && gTGLFbBase != 0 && __sync_val_compare_and_swap(&gSinkPowerDone, 0u, 1u) == 0u) {
		auto readAUX = FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX);
		auto writeAUX = reinterpret_cast<AUXFn>(gTGLFbBase + kWriteAUXOffset);
		uint8_t value = 0xFF;
		// intel_ddi_post_disable's order: the sink to D3, then the panel off (the power cycle below); no hold between them.
		const IOReturn d3ret = intel_dp_set_power(that, readAUX, writeAUX, DP_SET_POWER_D3, nullptr);
		auto *cb = NGreen::callback;
		if (cb != nullptr && cb->mmioMapped()) {
			// From i915, drivers/gpu/drm/i915/display/intel_pps.c: one panel power cycle as intel_pps_off_unlocked and
			// intel_pps_on_unlocked run it (the sink put in D3 first, as intel_ddi_post_disable orders them): PANEL_POWER_ON,
			// PANEL_POWER_RESET, EDP_FORCE_VDD and EDP_BLC_ENABLE cleared together ("otherwise some panels get very unhappy"), wait_panel_off;
			// wait_panel_power_cycle (the remainder of panel_power_cycle_delay since the off, then the sequencer's own cycle flag);
			// PANEL_POWER_ON | PANEL_POWER_RESET set, wait_panel_on. VDD and the backlight are not restored here: i915 sets them again
			// through intel_pps_vdd_on and intel_pps_backlight_on when they are wanted, and Apple's own PP_CONTROL writes do the same later
			// (at link training the register reads PANEL_POWER_ON | PANEL_POWER_RESET on every boot on record).
			// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L1026-L1069
			// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L669-L694
			// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_pps.c#L954-L1015
			const uint32_t status = cb->readReg32(PP_STATUS);
			uint32_t pp = cb->readReg32(PP_CONTROL);
			if ((status & PP_ON) != 0u && (pp & PANEL_POWER_ON) != 0u) {
				const uint32_t pp_entry = pp;
				const uint32_t t_off = uptimeMs();
				pp &= ~(PANEL_POWER_ON | PANEL_POWER_RESET | EDP_FORCE_VDD | EDP_BLC_ENABLE);
				cb->writeReg32(PP_CONTROL, pp);
				(void)cb->readReg32(PP_CONTROL);
				wait_panel_status(IDLE_OFF_MASK, IDLE_OFF_VALUE);
				const uint32_t panel_power_off_time = uptimeMs();
				const uint32_t cycle = panel_power_cycle_delay(pp_entry);
				const uint32_t panel_power_off_duration = uptimeMs() - panel_power_off_time;
				const uint32_t remaining = cycle > panel_power_off_duration ? cycle - panel_power_off_duration : 0u;
				if (remaining != 0) IOSleep(remaining);
				wait_panel_status(IDLE_CYCLE_MASK, IDLE_CYCLE_VALUE);
				const uint32_t t_on = uptimeMs();
				pp |= PANEL_POWER_ON | PANEL_POWER_RESET;
				cb->writeReg32(PP_CONTROL, pp);
				(void)cb->readReg32(PP_CONTROL);
				wait_panel_status(IDLE_ON_MASK, IDLE_ON_VALUE);
				const uint32_t t_ready = uptimeMs();
				uint32_t tries = 0;
				for (; tries < kPpsAuxTries; tries++) {
					if (readAUX(that, 0x0600, &value, 1) == kIOReturnSuccess) break;
					IOSleep(10);
				}
				SYSLOG("ngreen", "panel power cycle: T12 %u ms from PP_CONTROL 0x%x, off %u ms, on %u ms, now PP_CONTROL 0x%x PP_STATUS 0x%x, aux ready at try %u",
				       cycle, pp_entry, panel_power_off_time - t_off, t_ready - t_on, cb->readReg32(PP_CONTROL), cb->readReg32(PP_STATUS),
				       tries < kPpsAuxTries ? tries + 1u : 0u);
				if (that != nullptr)
					for (uint32_t addr : kPpsSinkAddrs) readAUX(that, addr, &value, 1);
			}
		}
		// intel_ddi_pre_enable's order: the panel on (above), the source OUI, the sink to D0.
		bool oui_written = false;
		const IOReturn d0ret = intel_dp_set_power(that, readAUX, writeAUX, DP_SET_POWER_D0, &oui_written);
		SYSLOG("ngreen", "sink power: D3 write 0x%x, the panel power cycle, source OUI %s, D0 write 0x%x",
		       d3ret, oui_written ? "written" : "already set", d0ret);
	}
	auto *cb = NGreen::callback;
	if (cb != nullptr && cb->mmioMapped()) {
		// From i915, drivers/gpu/drm/i915/display/intel_ddi.c, in tgl_ddi_pre_enable_dp's order: intel_ddi_enable_transcoder_clock
		// (TRANS_CLK_SEL from the port), then intel_ddi_config_transcoder_func (TRANS_DDI_FUNC_CTL with TRANS_DDI_FUNC_ENABLE clear).
		// The value is the one Apple's SetupParams built, or port A / DP SST over the live register when Apple selected no port.
		// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L662-L675
		// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L2834-L2840
		const uint32_t funcBefore = cb->readReg32(_TRANS_DDI_FUNC_CTL_A);
		const uint32_t apple = gTransFuncCtl;
		uint32_t ctl;
		if (apple != 0 && (apple & TGL_TRANS_DDI_PORT_MASK) != 0)
			ctl = apple & ~TRANS_DDI_FUNC_ENABLE;
		else
			ctl = (funcBefore & ~(TRANS_DDI_FUNC_ENABLE | TGL_TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK)) | TGL_TRANS_DDI_SELECT_PORT(PORT_A) | TRANS_DDI_MODE_SELECT_DP_SST;
		cb->writeReg32(_TRANS_CLK_SEL_A, clkSelFromFuncCtl(ctl));
		(void)cb->readReg32(_TRANS_CLK_SEL_A);
		cb->writeReg32(_TRANS_DDI_FUNC_CTL_A, ctl);
		(void)cb->readReg32(_TRANS_CLK_SEL_A);
	}
	gLinkRateCode = getMember<uint8_t>(that, 0xf);
	programDPLL0(getMember<uint8_t>(that, 0xf));
	__sync_add_and_fetch(&gPhyTraining, 1u);
	const IOReturn ret = FunctionCast(wrapLinkTraining, callback->oLinkTraining)(that, config);
	__sync_sub_and_fetch(&gPhyTraining, 1u);
	return ret;
}

// From i915, drivers/gpu/drm/i915/display/intel_ddi_buf_trans.h (struct icl_ddi_buf_trans) and intel_ddi_buf_trans.c
// (_adls_combo_phy_trans_edp_hbr2: the ADL-S combo PHY's eDP HBR2 low-vswing table, the one this panel's link uses):
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi_buf_trans.h#L27-L33
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c#L845-L857
struct icl_ddi_buf_trans { uint8_t dw2_swing_sel, dw7_n_scalar, dw4_cursor_coeff, dw4_post_cursor_2, dw4_post_cursor_1; };
static const icl_ddi_buf_trans _adls_combo_phy_trans_edp_hbr2[10] = {
	                                /* NT mV Trans mV db    */
	{0x9, 0x73, 0x3D, 0x00, 0x02},  /* 200   200      0.0   */
	{0x9, 0x7A, 0x3C, 0x00, 0x03},  /* 200   250      1.9   */
	{0x9, 0x7F, 0x3B, 0x00, 0x04},  /* 200   300      3.5   */
	{0x4, 0x6C, 0x33, 0x00, 0x0C},  /* 200   350      4.9   */
	{0x2, 0x73, 0x3A, 0x00, 0x05},  /* 250   250      0.0   */
	{0x2, 0x7C, 0x38, 0x00, 0x07},  /* 250   300      1.6   */
	{0x4, 0x5A, 0x36, 0x00, 0x09},  /* 250   350      2.9   */
	{0x4, 0x57, 0x3D, 0x00, 0x02},  /* 300   300      0.0   */
	{0x4, 0x65, 0x38, 0x00, 0x07},  /* 300   350      1.3   */
	{0x4, 0x6C, 0x3A, 0x00, 0x05},  /* 350   350      0.0   */
};

// From i915, drivers/gpu/drm/i915/display/intel_ddi.c, translate_signal_level: the row of index_to_dp_signal_levels that holds a
// swing / pre-emphasis pair. The grid is that table inverted (row = swing, column = pre-emphasis); a pair outside it answers -1
// where i915 warns and answers 0.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L95-L106
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L1470-L1491
static int translate_signal_level(uint32_t swing, uint32_t preEmph) {
	static const int8_t kGrid[4][4] = {
		{ 0,  1,  2,  3},
		{ 4,  5,  6, -1},
		{ 7,  8, -1, -1},
		{ 9, -1, -1, -1},
	};
	if (swing > 3 || preEmph > 3) return -1;
	return kGrid[swing][preEmph];
}

static constexpr uint32_t EDP4K2K_MODE_OVRD_EN        = 1u << 3;
static constexpr uint32_t EDP4K2K_MODE_OVRD_OPTIMIZED = 1u << 2;
#define _ICL_PORT_CL_DW10(phy)	(_ICL_COMBOPHY(phy) + 0x028u)
enum phy { PHY_A = 0 };

// From i915, drivers/gpu/drm/i915/display/intel_ddi.c: icl_combo_phy_loadgen_select, icl_ddi_combo_vswing_program and
// icl_combo_phy_set_signal_levels, run after each swing / pre-emphasis step of Apple's trainer with the ADL-S eDP HBR2 table above
// (adls_get_combo_buf_trans_edp's low-vswing choice for this 3.24 Gbps link). The registers are IntelDPLinkTraining.hpp's
// (intel_combo_phy_regs.h's names). Not i915's: PORT_CL_DW10's eDP 4K2K override is cleared outright (ADL-S has no hobl table), and
// the upper bits 30:18 of PORT_TX_DW4, which Apple's driver damages on one lane (boots #73-#74), are restored from a lane that still
// holds them.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi.c#L1160-L1286
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c#L1620-L1631
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_combo_phy_regs.h#L24-L44
static uint32_t icl_combo_phy_loadgen_select(int port_clock, uint32_t lane_count, uint32_t lane) {
	if (port_clock > 600000)
		return 0;

	if (lane_count == 4)
		return lane >= 1 ? LOADGEN_SELECT : 0;
	else
		return lane == 1 || lane == 2 ? LOADGEN_SELECT : 0;
}

static void captureTxDw4Upper(uint8_t phy) {
	if (gTxDw4UpperValid) return;
	for (uint32_t ln = 0; ln < 4; ln++) {
		const uint32_t u = NGreen::callback->readReg32(ICL_PORT_TX_DW4_LN(ln, phy)) & kTxDw4UpperMask;
		if (u != 0) {
			gTxDw4Upper = u;
			gTxDw4UpperValid = 1;
			return;
		}
	}
}

static void icl_ddi_combo_vswing_program(uint8_t phy, int level) {
	auto *cb = NGreen::callback;
	const icl_ddi_buf_trans &e = _adls_combo_phy_trans_edp_hbr2[level];
	uint32_t val;

	/* eDP: the 4K2K override follows the table; ADL-S has no hobl table, so it is off */
	cb->intel_de_rmw(_ICL_PORT_CL_DW10(phy), EDP4K2K_MODE_OVRD_EN | EDP4K2K_MODE_OVRD_OPTIMIZED, 0);

	/* Set PORT_TX_DW5 */
	val = cb->readReg32(ICL_PORT_TX_DW5_LN(0, phy));
	val &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK |
		 COEFF_POLARITY | CURSOR_PROGRAM |
		 TAP2_DISABLE | TAP3_DISABLE);
	val |= SCALING_MODE_SEL(0x2);
	val |= RTERM_SELECT(0x6);
	val |= TAP3_DISABLE;
	cb->writeReg32(ICL_PORT_TX_DW5_GRP(phy), val);

	/* Program PORT_TX_DW2 */
	for (uint32_t ln = 0; ln < 4; ln++) {
		cb->intel_de_rmw(ICL_PORT_TX_DW2_LN(ln, phy),
			     SWING_SEL_UPPER_MASK | SWING_SEL_LOWER_MASK | RCOMP_SCALAR_MASK,
			     SWING_SEL_UPPER(e.dw2_swing_sel) |
			     SWING_SEL_LOWER(e.dw2_swing_sel) |
			     RCOMP_SCALAR_VAL);
	}

	/* Program PORT_TX_DW4 */
	/* We cannot write to GRP. It would overwrite individual loadgen. */
	for (uint32_t ln = 0; ln < 4; ln++) {
		// the one read-modify-write i915 does, with the restore of the upper bits folded in
		const uint32_t upperClr = gTxDw4UpperValid ? kTxDw4UpperMask : 0u;
		cb->intel_de_rmw(ICL_PORT_TX_DW4_LN(ln, phy),
			     POST_CURSOR_1_MASK | POST_CURSOR_2_MASK | CURSOR_COEFF_MASK | upperClr,
			     POST_CURSOR_1(e.dw4_post_cursor_1) |
			     POST_CURSOR_2(e.dw4_post_cursor_2) |
			     CURSOR_COEFF(e.dw4_cursor_coeff) |
			     (gTxDw4UpperValid ? gTxDw4Upper : 0u));
	}

	/* Program PORT_TX_DW7 */
	for (uint32_t ln = 0; ln < 4; ln++) {
		cb->intel_de_rmw(ICL_PORT_TX_DW7_LN(ln, phy),
			     N_SCALAR_MASK,
			     N_SCALAR(e.dw7_n_scalar));
	}
}

static void icl_combo_phy_set_signal_levels(uint8_t phy, int port_clock, uint32_t lane_count, int level) {
	auto *cb = NGreen::callback;
	uint32_t val;

	/*
	 * 1. If port type is eDP or DP,
	 * set PORT_PCS_DW1 cmnkeeper_enable to 1b,
	 * else clear to 0b.
	 */
	val = cb->readReg32(ICL_PORT_PCS_DW1_LN(0, phy));
	val |= COMMON_KEEPER_EN;
	cb->writeReg32(ICL_PORT_PCS_DW1_GRP(phy), val);

	/* 2. Program loadgen select */
	/*
	 * Program PORT_TX_DW4 depending on Bit rate and used lanes
	 * <= 6 GHz and 4 lanes (LN0=0, LN1=1, LN2=1, LN3=1)
	 * <= 6 GHz and 1,2 lanes (LN0=0, LN1=1, LN2=1, LN3=0)
	 * > 6 GHz (LN0=0, LN1=0, LN2=0, LN3=0)
	 */
	for (uint32_t ln = 0; ln < 4; ln++) {
		cb->intel_de_rmw(ICL_PORT_TX_DW4_LN(ln, phy),
			     LOADGEN_SELECT,
			     icl_combo_phy_loadgen_select(port_clock, lane_count, ln));
	}

	/* 3. Set PORT_CL_DW5 SUS Clock Config to 11b */
	cb->intel_de_rmw(_ICL_PORT_CL_DW5(phy),
		     0, SUS_CLOCK_CONFIG);

	/* 4. Clear training enable to change swing values */
	val = cb->readReg32(ICL_PORT_TX_DW5_LN(0, phy));
	val &= ~TX_TRAINING_EN;
	cb->writeReg32(ICL_PORT_TX_DW5_GRP(phy), val);

	/* 5. Program swing and de-emphasis */
	icl_ddi_combo_vswing_program(phy, level);

	/* 6. Set training enable to trigger update */
	val = cb->readReg32(ICL_PORT_TX_DW5_LN(0, phy));
	val |= TX_TRAINING_EN;
	cb->writeReg32(ICL_PORT_TX_DW5_GRP(phy), val);
}

IOReturn Gen11::wrapSetVoltageSwingAndPreEmphasis(void *that, uint32_t swing, uint32_t preEmphasis,
                                                 void *driveConfig) {
	const IOReturn ret = FunctionCast(wrapSetVoltageSwingAndPreEmphasis, callback->oSetVoltageSwing)(
		that, swing, preEmphasis, driveConfig);

	if (gPhyTraining != 0) {
		uint32_t lanes = getMember<uint32_t>(that, 0x124);
		if (lanes != 1 && lanes != 2 && lanes != 4) lanes = 2;
		auto *cb = NGreen::callback;
		const int level = translate_signal_level(swing, preEmphasis);
		if (cb != nullptr && cb->mmioMapped() && level >= 0) {
			const int port_clock = drm_dp_bw_code_to_link_rate(static_cast<uint8_t>(gLinkRateCode));
			static volatile uint32_t rows = 0;
			const uint32_t n = __sync_add_and_fetch(&rows, 1u);
			uint32_t pcs0 = 0, cl5_0 = 0, cl10_0 = 0, lg0 = 0, lg1 = 0;
			if (n <= 4) {
				pcs0 = cb->readReg32(ICL_PORT_PCS_DW1_LN(0, PHY_A));
				cl5_0 = cb->readReg32(_ICL_PORT_CL_DW5(PHY_A));
				cl10_0 = cb->readReg32(_ICL_PORT_CL_DW10(PHY_A));
				for (uint32_t ln = 0; ln < 4; ln++) lg0 |= ((cb->readReg32(ICL_PORT_TX_DW4_LN(ln, PHY_A)) >> 31) & 1u) << ln;
			}
			captureTxDw4Upper(PHY_A);
			icl_combo_phy_set_signal_levels(PHY_A, port_clock, lanes, level);
			if (n <= 4) {
				for (uint32_t ln = 0; ln < 4; ln++) lg1 |= ((cb->readReg32(ICL_PORT_TX_DW4_LN(ln, PHY_A)) >> 31) & 1u) << ln;
				SYSLOG("ngreen", "combo PHY A signal levels [%u]: level %d (swing %u pre-emphasis %u) lanes %u clock %d kHz; before PCS_DW1 0x%x CL_DW5 0x%x CL_DW10 0x%x loadgen 0x%x, after 0x%x 0x%x 0x%x loadgen 0x%x, DW5 0x%x, DW4 upper 0x%x",
				       n, level, swing, preEmphasis, lanes, port_clock, pcs0, cl5_0, cl10_0, lg0,
				       cb->readReg32(ICL_PORT_PCS_DW1_LN(0, PHY_A)), cb->readReg32(_ICL_PORT_CL_DW5(PHY_A)), cb->readReg32(_ICL_PORT_CL_DW10(PHY_A)), lg1,
				       cb->readReg32(ICL_PORT_TX_DW5_LN(0, PHY_A)), gTxDw4Upper);
			}
		}
	}
	return ret;
}

IOReturn Gen11::wrapGetDPCDInfo(void *that) {
	const IOReturn ret = FunctionCast(wrapGetDPCDInfo, callback->oGetDPCDInfo)(that);
	if (ret == 0 && getMember<uint8_t>(that, 0xf) != 12u) getMember<uint8_t>(that, 0xf) = 12u;
	return ret;
}

// setupOptimalLaneCount is called by hwSetMode to pick the lane count that gets
// stored in the port's cached LinkConfig (and ultimately into TRANS_DDI_FUNC_CTL).
// Apple's implementation runs: optimal = computeLaneCount(...); then caps it to
// port->maxLaneCount (from DPCD MAX_LANE_COUNT, which is 2 on this panel).
// On !isRealTGL we instead snap the cached count to match DDI_BUF_CTL_A so that
// SetupParams builds TRANS_DDI_FUNC_CTL with the correct HW-trained lane field.
void Gen11::setupOptimalLaneCount(AppleIntel::AppleIntelBaseController *that, const IODetailedTimingInformationV2 *timing, unsigned int bpp) {
	// Always run Apple's original first to populate all other LinkConfig fields.
	FunctionCast(setupOptimalLaneCount, callback->osetupOptimalLaneCount)(that, timing, bpp);

	if (NGreen::callback->isRealTGL || NGreen::callback->isRaptorLakeHX())
		return;

	// Read HW-trained lane count from DDI_BUF_CTL_A bits[3:1].
	// PORT_WIDTH: 0=x1, 1=x2, 3=x4.
	const uint32_t ddiA    = NGreen::callback->readReg32(0x64000);
	const unsigned int width   = (ddiA >> 1) & 0x7u;
	const unsigned int hwLanes = (width >= 3) ? 4u : (width >= 1) ? 2u : 1u;

	unsigned int &cached = getMember<unsigned int>(that, 0x124);

	static int v90L5Logs = 0;
	if (v90L5Logs < 10) {
		v90L5Logs++;
		SYSLOG("ngreen", "V90L5[%d]: setupOptimalLC: was=%u DDI_BUF_CTL_A=0x%x hwLanes=%u",
			   v90L5Logs, cached, ddiA, hwLanes);
	}

	if (hwLanes > cached)
		cached = hwLanes;
}

IOReturn Gen11::wrapICLReadAUX(void *that, uint32_t address, void *buffer, uint32_t length) {
	if (NGreen::callback && NGreen::callback->isRaptorLakeHX() && !ensureA788DisplayAWells(false))
		return kIOReturnNotReady;

	IOReturn retVal = FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that, address, buffer, length);

	if (that != nullptr) gAuxSeen = true;

	// V97AUX: log first ~40 AUX reads to diagnose eDP link training failures.
	static int auxLogCount = 0;
	if (auxLogCount < 40) {
		auxLogCount++;
		uint8_t *b = reinterpret_cast<uint8_t *>(buffer);
		if (length >= 2)
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x [0]=0x%02x [1]=0x%02x",
				   auxLogCount, address, length, retVal, b ? b[0] : 0xFF, (b && length >= 2) ? b[1] : 0xFF);
		else
			SYSLOG("ngreen", "V97AUX[%d]: addr=0x%04x len=%u ret=0x%x",
				   auxLogCount, address, length, retVal);
	}

	if (gPhyTraining != 0 && address == 0x0202 && __sync_val_compare_and_swap(&gPhyProbed, 0u, 1u) == 0u) {
		auto readAUX = FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX);
		uint8_t lc[2] = {0xFF, 0xFF};
		uint8_t sw[4] = {0xFF, 0xFF, 0xFF, 0xFF};
		readAUX(that, 0x0100, lc, sizeof(lc));
		readAUX(that, 0x0103, sw, sizeof(sw));
		linkTrainingIdleProbe(that, readAUX);
	}

	// V98T removed: do NOT clamp DPCD[0x0100-0x0101] (LINK_BW_SET / LANE_COUNT_SET).
	// Capping these to HBR2/2-lanes caused Apple to train the link at HBR2×2 lanes.
	// V97P then wrote a 4-lane DDI_FUNC_CTL value to a 2-lane trained link → black screen.
	// UEFI already trained the link at HBR3×4 lanes; we must let Apple see those values
	// so it (re-)trains consistently and V97P's bit16-only correction stays coherent.
	if (NGreen::callback && !NGreen::callback->isRealTGL && address == 0x0100 && buffer && length >= 1) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		static int v98tLogs = 0;
		if (v98tLogs < 5) {
			v98tLogs++;
			if (length >= 2)
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x lanes=0x%02x",
					   v98tLogs, raw[0], raw[1]);
			else
				SYSLOG("ngreen", "V98T[%d]: DPCD 0x0100 passthrough bw=0x%02x (len=1)",
					   v98tLogs, raw[0]);
		}
	}

	// V99: Suppress spurious LINK_STATUS_UPDATED (DPCD[0x204] bit7) on RPL-P.
	// HDCP probing reads DPCD 0x6921d, which causes the eDP panel to assert IRQ_HPD,
	// setting LINK_STATUS_UPDATED=1. Apple's checkLinkStatus then sees
	// INTERLANE_ALIGN_DONE=0 and tears down the display (~10s after boot).
	// The physical link is healthy; only the IRQ flag is spurious.
	// Clearing bit7 of DPCD[0x204] prevents the driver from acting on the IRQ.
	if (NGreen::callback && !NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX() && address == 0x0202 && buffer && length >= 3) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		if (raw[2] & 0x80) {
			static int v99Logs = 0;
			if (v99Logs < 10) {
				v99Logs++;
				SYSLOG("ngreen", "V99[%d]: suppressed DPCD 0x204 LINK_STATUS_UPDATED "
					   "(was 0x%02x, lanes=[0x%02x 0x%02x])",
					   v99Logs, raw[2], raw[0], raw[1]);
			}
			raw[2] &= ~0x80u; // clear LINK_STATUS_UPDATED
		}
	}

	if (address != 0x0000 && address != 0x2200) return retVal;

	if (length < sizeof(DPCDCap16) || buffer == nullptr)
		return retVal;

	auto caps = reinterpret_cast<DPCDCap16 *>(buffer);

	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		// V98: Do NOT cap maxLaneCount or maxLinkRate.
		// Previous versions capped maxLaneCount to 2, which caused Apple to train at
		// 2 lanes while V97P subsequently wrote a 4-lane DDI_FUNC_CTL value → black screen.
		// The hardware is trained at HBR3×4 lanes by UEFI; let Apple see those real caps
		// so its LightUpEDP (re-)trains consistently at HBR3×4 lanes.
		static int v98Logs = 0;
		if (v98Logs < 5) {
			v98Logs++;
			SYSLOG("ngreen", "V98[%d]: DPCD caps @0x%04x maxLinkRate=0x%02x maxLane=0x%02x (passthrough)",
				   v98Logs, address, caps->maxLinkRate, caps->maxLaneCount);
		}
	}

	if (caps->revision < 0x03) {
		caps->maxLinkRate = 0;
	}

	return retVal;
}

IOReturn Gen11::wrapICLWriteAUX(void *that, uint32_t address, void *buffer, uint32_t length) {
	if (callback == nullptr || callback->orgICLWriteAUX == 0)
		return kIOReturnNotReady;

	auto writeAUX = FunctionCast(wrapICLWriteAUX, callback->orgICLWriteAUX);

	// DP_EDP_CONFIGURATION_SET is written as 0 whatever Apple sends (i915 never writes this DPCD).
	if (address == DP_EDP_CONFIGURATION_SET && length == 1 && buffer != nullptr && that != nullptr) {
		uint8_t zero = 0;
		return writeAUX(that, DP_EDP_CONFIGURATION_SET, &zero, 1u);
	}

	const int32_t idx = RAPTOR_LAKE_HX_OR(4, -1);

	// From i915, drivers/gpu/drm/i915/display/intel_dp_link_training.c: Apple's two-byte DP_LINK_BW_SET write becomes
	// intel_dp_link_training_set_bw's eDP 1.4 branch (DP_LANE_COUNT_SET, then DP_LINK_RATE_SET with the rate index, and no
	// DP_LINK_BW_SET), after intel_dp_prepare_link_train's WaEdpLinkRateDataReload (a re-read of DP_SUPPORTED_LINK_RATES that the
	// Parade MUX of TGL+ laptops needs to snoop). The lane byte is Apple's (i915 ORs the enhanced-framing bit in itself); the
	// rate index is this panel's HBR3 entry (i915 takes it from intel_dp_compute_rate).
	// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dp_link_training.c#L1154-L1183
	// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_dp_link_training.c#L1264-L1311
	if (idx >= 0 && address == DP_LINK_BW_SET && length == 2 && buffer != nullptr && that != nullptr) {
		auto *raw = reinterpret_cast<uint8_t *>(buffer);
		uint8_t       lane_count  = raw[1];
		uint8_t       rate_select = static_cast<uint8_t>(idx);

		if (callback->orgICLReadAUX != 0) {
			uint16_t sink_rates[DP_MAX_SUPPORTED_RATES] = {};
			FunctionCast(wrapICLReadAUX, callback->orgICLReadAUX)(that, DP_SUPPORTED_LINK_RATES, sink_rates, sizeof(sink_rates));
		}

		const IOReturn rLane = writeAUX(that, DP_LANE_COUNT_SET, &lane_count, 1);
		const IOReturn rRate = writeAUX(that, DP_LINK_RATE_SET, &rate_select, 1);

		return (rRate != kIOReturnSuccess) ? rRate : rLane;
	}

	return writeAUX(that, address, buffer, length);
}

IOReturn Gen11::wrapRunAUXCommand(void *that, uint32_t command, uint32_t *data, uint32_t length, bool flag) {
	if (NGreen::callback && NGreen::callback->isRaptorLakeHX() && !ensureA788DisplayAWells(false)) return kIOReturnNotReady;
	return FunctionCast(wrapRunAUXCommand, callback->orgRunAUXCommand)(that, command, data, length, flag);
}

void Gen11::getOnlineInfo(AppleIntel::AppleIntelFramebuffer *that, AppleIntel::AppleIntelDisplayPath *displayPath, unsigned char *online, unsigned char *changed) {
	// V96 removed (was: force *online=1 for fbId==0). Confirmed no-op on this hardware:
	// baseline log shows Apple's getOnlineInfo natively reports orig=1 for FB0, so the
	// V96 forcing was already redundant. Keeping the wrapper as a logging shell so we
	// can observe original online behavior across boots — if any orig!=1 case shows up
	// we'll know V96 was masking a real status bug, not just being redundant.
	FunctionCast(getOnlineInfo, callback->ogetOnlineInfo)(that, displayPath, online, changed);
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	unsigned char origOnline = online ? *online : 0xFF;
	static int v96PassLogs = 0;
	if (v96PassLogs < 12) {
		v96PassLogs++;
		SYSLOG("ngreen", "V96p: fb%u getOnlineInfo: orig=%d passthrough (V96 hack removed)",
			   fbId, origOnline);
	}
}

// Path B: hook AppleIntelFramebuffer::isApertureMemoryRequired().
// On real TGL: pass-through (preserve native behavior).
// On RPL/ADL with -ngreendp0: force return true so setupScanoutMemory never migrates
// from aperture → non-aperture. setupScanoutMemory's logic is:
//   if (isApertureMemoryRequired() && nonAperSurf!=0) → migrate-TO-aperture
//   else if (!isApertureMemoryRequired() && nonAperSurf==0) → migrate-FROM-aperture (the bad path)
//   else → no migration; if nonAperSurf==0 → "Using aperture memory"
// Forcing true with nonAperSurf==0 (the boot state) keeps us on the aperture path forever,
// matching what V99S+V99G already do at the hardware level — but cleanly, at the driver level,
// so WS sees a coherent fWSAAState→memory mapping and shouldn't degrade 0x3→0x1.
bool Gen11::wrapIsApertureMemoryRequired(AppleIntel::AppleIntelFramebuffer *that) {
	bool orig = FunctionCast(wrapIsApertureMemoryRequired, callback->oIsApertureMemoryRequired)(that);
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;

	// Panel-freeze probe: the pipe, plane and cursor registers on the first calls.
	if (NGreen::callback) {
		static uint32_t v205Calls = 0;
		v205Calls++;

		// V99Z dirty-rect test removed: wipe ran but visually no change because WS
		// rewrites the whole buffer every frame. The "frozen images" symptom is
		// actually the X-tile-as-linear scanout artifact appearing more pronounced
		// on low-frequency content (text, solid blocks) than high-frequency content
		// (wallpaper texture). Single root cause = scanout-vs-buffer tile mismatch.
		// (Wipe proved BAR2 writes reach scanout in earlier magenta test, but WS's
		// per-frame rewriting makes the wipe invisible.)

		// Periodic state snapshot for freeze diagnosis. Reduced thresholds since
		// wrapIsApertureMemoryRequired only fires ~44 times per boot in FB-only mode.
		const bool sample = (v205Calls == 1  || v205Calls == 2  || v205Calls == 5  ||
							 v205Calls == 10 || v205Calls == 20 || v205Calls == 30 ||
							 v205Calls == 40 || v205Calls == 44);
		if (sample) {
			uint32_t frm   = NGreen::callback->readReg32(0x70040);
			uint32_t pstat = NGreen::callback->readReg32(0x70024);
			uint32_t pcfg  = NGreen::callback->readReg32(0x70008);
			uint32_t dcst  = NGreen::callback->readReg32(0x45504);
			// V211: also probe Plane 2 (overlay) and Plane 3 (sprite) on Pipe A.
			// The visible "frozen overlay over animating background" symptom suggests
			// these planes hold stale content because WS's dp0 path only writes Plane 1.
			uint32_t p1ctl = NGreen::callback->readReg32(0x70180);
			uint32_t p1surf = NGreen::callback->readReg32(0x7019C);
			uint32_t p1liv = NGreen::callback->readReg32(0x701AC);
			uint32_t p2ctl = NGreen::callback->readReg32(0x71180);
			uint32_t p2surf = NGreen::callback->readReg32(0x7119C);
			uint32_t p2liv = NGreen::callback->readReg32(0x711AC);
			uint32_t p3ctl = NGreen::callback->readReg32(0x72180);
			uint32_t p3surf = NGreen::callback->readReg32(0x7219C);
			uint32_t p3liv = NGreen::callback->readReg32(0x721AC);
			uint32_t curctl = NGreen::callback->readReg32(0x70080);
			uint32_t curbase = NGreen::callback->readReg32(0x70084);
			uint32_t curpos = NGreen::callback->readReg32(0x70088);
			SYSLOG("ngreen", "V205[c=%u]: FRM=%u STAT=%08x CONF=%08x DC=%08x | P1 CTL=%08x SURF=%08x LIVE=%08x | P2 CTL=%08x SURF=%08x LIVE=%08x | P3 CTL=%08x SURF=%08x LIVE=%08x | CUR CTL=%08x BASE=%08x POS=%08x | PSR1=%08x PSR2=%08x",
				   v205Calls, frm, pstat, pcfg, dcst,
				   p1ctl, p1surf, p1liv,
				   p2ctl, p2surf, p2liv,
				   p3ctl, p3surf, p3liv,
				   curctl, curbase, curpos,
				   NGreen::callback->readReg32(0x60800), NGreen::callback->readReg32(0x60900));
		}
	}

	if (isRealTGL || !isDisplayPipeForceDisabled()) {
		return orig;
	}
	static int logCount = 0;
	if (logCount < 8 && orig != true) {
		logCount++;
		SYSLOG("ngreen", "PathB: isApertureMemoryRequired forced true (orig=%d) fb=%p", orig, that);
	}
	return true;
}

// Path C: hook AppleIntelFramebuffer::setAttribute(IOSelect, uintptr_t).
// We intercept exactly one path: kIOWindowServerActiveAttribute ('wsrv' = 0x77737276).
// When WindowServer writes 0x1 (degrade-from-active), coerce the value to 0x3 before
// calling the original — keeping the driver's fWSAAState pinned at the active value so
// it never takes the hwDeferFeatures / degraded path.  Real TGL is unaffected.
IOReturn Gen11::wrapSetAttribute(void *that, uint32_t attr, uintptr_t value) {
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
	if (attr == 0x77737276u /* 'wsrv' */ && !isRealTGL && isDisplayPipeForceDisabled()) {
		static int logCount = 0;
		uintptr_t newValue = value;
		bool coerced = false;
		if ((value & 0xFFu) == 0x1u) {
			newValue = (value & ~uintptr_t(0xFF)) | 0x3u;
			coerced = true;
		}
		if (logCount < 16) {
			logCount++;
			SYSLOG("ngreen", "PathC: wsrv setAttribute fb=%p value=0x%llx%s",
				   that, (unsigned long long)value, coerced ? " → coerced 0x3" : "");
		}
		return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, newValue);
	}
	return FunctionCast(wrapSetAttribute, callback->oSetAttribute)(that, attr, value);
}

// The hwInitializeCState paths of the other DMC choices (ngreen-dmc=tgl, the ADL-P default) request their wells through the BIOS and
// DRIVER registers at once and poll 2 ms; i915 never writes a BIOS request register. Neither path is Raptor Lake-HX's (#223 ran
// neither), so they keep this routine.
static bool pwrWellRequestAndWait(uint32_t reg, uint32_t requestMask) {
	requestMask &= 0xAAAAAAAAu;
	const uint32_t requested = NGreen::callback->readReg32(reg) | requestMask;
	NGreen::callback->writeReg32(reg, requested);
	const uint32_t stateMask = requestMask >> 1;
	if (stateMask == 0) return true;

	for (int i = 0; i < 200; i++) {
		if ((NGreen::callback->readReg32(reg) & stateMask) == stateMask) return true;
		IODelay(10);
	}
	return false;
}

// From i915, drivers/gpu/drm/i915/display/intel_display_power_well.c: the ICL register sets of the AUX and DDI wells (BIOS, DRIVER and
// DEBUG request registers), hsw_power_well_enable with hsw_wait_for_power_well_enable (the REQ bit set in the driver register, the
// STATE bit awaited for 1 ms), hsw_power_well_disable with hsw_wait_for_power_well_disable (the REQ bit cleared, the STATE bit awaited
// clear for 1 ms, or not at all when another requester still holds the well: hsw_power_well_requesters) and hsw_power_well_enabled.
// ADL-S's AUX A takes icl_aux_power_well_enable's hsw_power_well_enable branch. The registers: display/intel_display_regs.h.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_power_well.c#L343-L421
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_power_well.c#L432-L485
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_power_well.c#L635-L645
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_power_well.c#L2101-L2119
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L2374-L2379
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_display_regs.h#L2410-L2454
struct i915_power_well_regs { uint32_t bios, driver, debug; };
static constexpr uint32_t ICL_PWR_WELL_CTL_AUX1 = 0x45440u;
static constexpr uint32_t ICL_PWR_WELL_CTL_AUX2 = 0x45444u;
static constexpr uint32_t ICL_PWR_WELL_CTL_AUX4 = 0x4544Cu;
static constexpr uint32_t ICL_PWR_WELL_CTL_DDI1 = 0x45450u;
static constexpr uint32_t ICL_PWR_WELL_CTL_DDI2 = 0x45454u;
static constexpr uint32_t ICL_PWR_WELL_CTL_DDI4 = 0x4545Cu;
static constexpr int ICL_PW_CTL_IDX_AUX_A = 0;
static constexpr int ICL_PW_CTL_IDX_DDI_A = 0;
#define HSW_PWR_WELL_CTL_REQ(pw_idx)		(0x2 << ((pw_idx) * 2))
#define HSW_PWR_WELL_CTL_STATE(pw_idx)	(0x1 << ((pw_idx) * 2))
static constexpr i915_power_well_regs icl_aux_power_well_regs = { ICL_PWR_WELL_CTL_AUX1, ICL_PWR_WELL_CTL_AUX2, ICL_PWR_WELL_CTL_AUX4 };
static constexpr i915_power_well_regs icl_ddi_power_well_regs = { ICL_PWR_WELL_CTL_DDI1, ICL_PWR_WELL_CTL_DDI2, ICL_PWR_WELL_CTL_DDI4 };

static bool hsw_wait_for_power_well_enable(const i915_power_well_regs &regs, int pw_idx) {
	/* Timeout for PW1:10 us, AUX:not specified, other PWs:20 us. */
	return intel_de_wait_ms(regs.driver, HSW_PWR_WELL_CTL_STATE(pw_idx), true, 1);
}

static uint32_t hsw_power_well_requesters(const i915_power_well_regs &regs, int pw_idx) {
	auto *cb = NGreen::callback;
	const uint32_t req_mask = HSW_PWR_WELL_CTL_REQ(pw_idx);
	uint32_t ret;

	ret = (cb->readReg32(regs.bios) & req_mask) ? 1 : 0;
	ret |= (cb->readReg32(regs.driver) & req_mask) ? 2 : 0;
	ret |= (cb->readReg32(regs.debug) & req_mask) ? 8 : 0;

	return ret;
}

static bool hsw_wait_for_power_well_disable(const i915_power_well_regs &regs, int pw_idx) {
	/*
	 * Bspec doesn't require waiting for PWs to get disabled, but still do
	 * this for paranoia. The known cases where a PW will be forced on:
	 * - a KVMR request on any power well via the KVMR request register
	 * - a DMC request on PW1 and MISC_IO power wells via the BIOS and
	 *   DEBUG request registers
	 * Skip the wait in case any of the request bits are set and print a
	 * diagnostic message.
	 */
	const uint32_t reqs = hsw_power_well_requesters(regs, pw_idx);
	return intel_de_wait_ms(regs.driver, HSW_PWR_WELL_CTL_STATE(pw_idx), false, reqs ? 0 : 1);
}

static bool hsw_power_well_enable(const i915_power_well_regs &regs, int pw_idx) {
	NGreen::callback->intel_de_rmw(regs.driver, 0, HSW_PWR_WELL_CTL_REQ(pw_idx));
	return hsw_wait_for_power_well_enable(regs, pw_idx);
}

static bool hsw_power_well_disable(const i915_power_well_regs &regs, int pw_idx) {
	NGreen::callback->intel_de_rmw(regs.driver, HSW_PWR_WELL_CTL_REQ(pw_idx), 0);
	return hsw_wait_for_power_well_disable(regs, pw_idx);
}

static bool hsw_power_well_enabled(const i915_power_well_regs &regs, int pw_idx) {
	const uint32_t mask = HSW_PWR_WELL_CTL_REQ(pw_idx) | HSW_PWR_WELL_CTL_STATE(pw_idx);
	return (NGreen::callback->readReg32(regs.driver) & mask) == mask;
}

// Apple's hwSetPowerWellStateAux / DDI hand the whole request mask of a register: every well whose request changes is enabled or
// disabled on its own, as intel_display_power_get / put would do it, in index order.
static void icl_power_wells_request(const i915_power_well_regs &regs, uint32_t request_mask, const char *name) {
	auto *cb = NGreen::callback;
	const uint32_t current = cb->readReg32(regs.driver);
	uint32_t enabled = 0, disabled = 0, timeouts = 0;
	for (int pw_idx = 0; pw_idx < 16; pw_idx++) {
		const uint32_t req = HSW_PWR_WELL_CTL_REQ(pw_idx);
		if ((request_mask & req) != 0 && (current & req) == 0) {
			enabled++;
			if (!hsw_power_well_enable(regs, pw_idx)) timeouts++;
		} else if ((request_mask & req) == 0 && (current & req) != 0) {
			disabled++;
			if (!hsw_power_well_disable(regs, pw_idx)) timeouts++;
		}
	}
	static volatile uint32_t rows = 0;
	if (__sync_add_and_fetch(&rows, 1u) <= 8u)
		SYSLOG("ngreen", "power wells %s: request 0x%x over driver 0x%x: %u enabled, %u disabled, %u timeouts; now driver 0x%x bios 0x%x debug 0x%x",
		       name, request_mask, current, enabled, disabled, timeouts, cb->readReg32(regs.driver), cb->readReg32(regs.bios), cb->readReg32(regs.debug));
}

static bool ensureA788DisplayAWells(bool requireDDI) {
	if (!NGreen::callback || !NGreen::callback->isRaptorLakeHX())
		return true;

	if (!hsw_power_well_enabled(icl_aux_power_well_regs, ICL_PW_CTL_IDX_AUX_A))
		hsw_power_well_enable(icl_aux_power_well_regs, ICL_PW_CTL_IDX_AUX_A);
	if (requireDDI && !hsw_power_well_enabled(icl_ddi_power_well_regs, ICL_PW_CTL_IDX_DDI_A))
		hsw_power_well_enable(icl_ddi_power_well_regs, ICL_PW_CTL_IDX_DDI_A);

	return hsw_power_well_enabled(icl_aux_power_well_regs, ICL_PW_CTL_IDX_AUX_A) &&
	       (!requireDDI || hsw_power_well_enabled(icl_ddi_power_well_regs, ICL_PW_CTL_IDX_DDI_A));
}

// V183: ADL-P/RPL write-only power well handler.
// HSW_PWR_WELL_CTL1 (0x45400): REQ bits = odd bits (mask 0xAA: bits 1,3,5,7,...);
//                               STATE bits = even bits (mask 0x55: bits 0,2,4,6,...).
// TGL original polls STATE bits after writing REQ — on ADL-P those ACKs
// never arrive within the 20-iteration timeout, spinning the CPU.
// Fix: write REQ bits directly to CTL1, skip polling entirely.
// Previously this wrote to CTL2 (0x45404) — wrong register. CTL2 controls DDI/AUX
// wells; PG display wells (PG1/PG2) are in CTL1. Writing to CTL2 left CTL1 at the
// DMC-reset value (0x405, no REQ bits) → hardware never enabled display power domains
// → vsync interrupts never delivered → GPU ring idle → framebuffer stayed black.
void Gen11::hwSetPowerWellStatePGE(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	if (!NGreen::callback->isRealTGL) {
		uint32_t ctl1 = NGreen::callback->readReg32(0x45400);
		uint32_t newVal;
		if (param_1) {
			// Enable: clear all REQ bits then set the requested ones.
			newVal = (ctl1 & 0xFFFFFF55U) | (param_2 & 0xAAU);
		} else {
			// Disable: clear the requested REQ bits.
			newVal = ctl1 & ~(param_2 & 0xAAU);
		}
		SYSLOG("ngreen", "V183.PGE: en=%u mask=0x%x ctl1: 0x%x->0x%x",
			   (unsigned)param_1, param_2, ctl1, newVal);
		NGreen::callback->writeReg32(0x45400, newVal);
		return;
	}
	// Real TGL: use original with ccont fixup.
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePGE, callback->ohwSetPowerWellStatePGE)(that, param_1, param_2);
}

void Gen11::hwSetPowerWellStateAux(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	if (NGreen::callback && NGreen::callback->isRaptorLakeHX()) {
		icl_power_wells_request(icl_aux_power_well_regs, param_2, "AUX");
		return;
	}
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateAux, callback->ohwSetPowerWellStateAux)(that,param_1,param_2);
}

void Gen11::hwSetPowerWellStateDDI(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	if (NGreen::callback && NGreen::callback->isRaptorLakeHX()) {
		icl_power_wells_request(icl_ddi_power_well_regs, param_2, "DDI");
		return;
	}
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStateDDI, callback->ohwSetPowerWellStateDDI)(that,param_1,param_2);
}

// From Linux, lib/uuid.c: guid_parse, the first three fields of a GUID string stored little-endian, as _WDG stores them.
// https://github.com/torvalds/linux/blob/65ba8f3/lib/uuid.c#L22
// https://github.com/torvalds/linux/blob/65ba8f3/lib/uuid.c#L89-L127
#define UUID_STRING_LEN 36
static const uint8_t guid_index[16] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};

static int hex_to_bin(char ch)
{
	if ((ch >= '0') && (ch <= '9'))
		return ch - '0';
	if ((ch >= 'a') && (ch <= 'f'))
		return ch - 'a' + 10;
	if ((ch >= 'A') && (ch <= 'F'))
		return ch - 'A' + 10;
	return -1;
}

static bool uuid_is_valid(const char *uuid)
{
	unsigned int i;

	for (i = 0; i < UUID_STRING_LEN; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (uuid[i] != '-')
				return false;
		} else if (hex_to_bin(uuid[i]) < 0) {
			return false;
		}
	}

	return true;
}

static int __uuid_parse(const char *uuid, uint8_t b[16], const uint8_t ei[16])
{
	static const uint8_t si[16] = {0,2,4,6,9,11,14,16,19,21,24,26,28,30,32,34};
	unsigned int i;

	if (!uuid_is_valid(uuid))
		return -EINVAL;

	for (i = 0; i < 16; i++) {
		int hi = hex_to_bin(uuid[si[i] + 0]);
		int lo = hex_to_bin(uuid[si[i] + 1]);

		b[ei[i]] = (hi << 4) | lo;
	}

	return 0;
}

static int guid_parse(const char *uuid, uint8_t b[16])
{
	return __uuid_parse(uuid, b, guid_index);
}

// From Linux, drivers/platform/wmi/core.c: a PNP0C14 device's _WDG lists its WMI blocks (parse_wdg); a method block is evaluated as
// WM + the block's object id with the instance, the method id and the input buffer (wmidev_evaluate_method). Only what
// nvidia-wmi-ec-backlight needs is here: the block is looked up by GUID directly (wmi_find_device_by_guid without the bus), and an
// integer result is laid into the output buffer as ACPICA lays out an integer object (the type, then the 64-bit value at offset 8).
// IOReturn takes acpi_status's place.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L43-L54
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L88
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L134-L145
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L211-L226
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L319-L364
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/wmi/core.c#L1291-L1323
#define ACPI_WMI_METHOD	0x2	/* GUID is a method */
#define WMI_ACPI_METHOD_NAME_SIZE 5

struct PACKED guid_block {
	uint8_t guid[16];
	char object_id[2];
	uint8_t instance_count;
	uint8_t flags;
};
static_assert(sizeof(struct guid_block) == 20, "guid_block");

struct wmi_block {
	struct guid_block gblock;
	IOACPIPlatformDevice *acpi_device;
};

struct acpi_buffer {
	size_t length;
	void *pointer;
};

static inline void get_acpi_method_name(const struct wmi_block *wblock, const char method, char buffer[WMI_ACPI_METHOD_NAME_SIZE])
{
	buffer[0] = 'W';
	buffer[1] = method;
	buffer[2] = wblock->gblock.object_id[0];
	buffer[3] = wblock->gblock.object_id[1];
	buffer[4] = '\0';
}

static int wmi_find_device_by_guid(const char *guid_string, struct wmi_block *wblock)
{
	uint8_t guid[16];
	OSDictionary *matching;
	OSIterator *it;
	OSObject *obj;
	int ret;

	ret = guid_parse(guid_string, guid);
	if (ret < 0)
		return ret;

	matching = IOService::nameMatching("PNP0C14");
	it = matching ? IOService::getMatchingServices(matching) : nullptr;
	OSSafeReleaseNULL(matching);
	if (!it)
		return -ENODEV;

	ret = -ENODEV;
	while (ret && (obj = it->getNextObject())) {
		IOACPIPlatformDevice *device = OSDynamicCast(IOACPIPlatformDevice, obj);
		const struct guid_block *gblock;
		OSObject *out = nullptr;
		OSData *buffer;
		uint32_t i, total;

		if (!device || device->evaluateObject("_WDG", &out) != kIOReturnSuccess)
			continue;

		buffer = OSDynamicCast(OSData, out);
		gblock = buffer ? static_cast<const struct guid_block *>(buffer->getBytesNoCopy()) : nullptr;
		total = buffer ? buffer->getLength() / sizeof(struct guid_block) : 0;

		for (i = 0; i < total; i++) {
			if (!gblock[i].instance_count || memcmp(gblock[i].guid, guid, sizeof(guid)))
				continue;

			device->retain();
			wblock->acpi_device = device;
			wblock->gblock = gblock[i];
			ret = 0;
			break;
		}
		OSSafeReleaseNULL(out);
	}
	it->release();

	return ret;
}

static IOReturn wmidev_evaluate_method(struct wmi_block *wblock, uint8_t instance, uint32_t method_id,
				       const struct acpi_buffer *in, struct acpi_buffer *out)
{
	const struct guid_block *block = &wblock->gblock;
	char method[WMI_ACPI_METHOD_NAME_SIZE];
	OSObject *params[3];
	OSObject *result = nullptr;
	OSNumber *integer;
	IOReturn status;

	if (!in)
		return kIOReturnBadArgument;

	if (!(block->flags & ACPI_WMI_METHOD))
		return kIOReturnBadArgument;

	if (block->instance_count <= instance)
		return kIOReturnBadArgument;

	params[0] = OSNumber::withNumber(instance, 32);
	params[1] = OSNumber::withNumber(method_id, 32);
	params[2] = OSData::withBytes(in->pointer, static_cast<unsigned int>(in->length));

	get_acpi_method_name(wblock, 'M', method);

	status = params[0] && params[1] && params[2] ? wblock->acpi_device->evaluateObject(method, &result, params, 3) : kIOReturnNoMemory;
	for (OSObject *param : params)
		OSSafeReleaseNULL(param);

	integer = OSDynamicCast(OSNumber, result);
	if (status == kIOReturnSuccess && out && integer && out->length >= 16) {
		const uint32_t type = 1;	/* ACPI_TYPE_INTEGER */
		const uint64_t value = integer->unsigned64BitValue();

		memcpy(out->pointer, &type, sizeof(type));
		memcpy(static_cast<uint8_t *>(out->pointer) + 8, &value, sizeof(value));
	}
	OSSafeReleaseNULL(result);

	return status;
}

// From Linux, include/linux/platform_data/x86/nvidia-wmi-ec-backlight.h.
// https://github.com/torvalds/linux/blob/65ba8f3/include/linux/platform_data/x86/nvidia-wmi-ec-backlight.h#L9
// https://github.com/torvalds/linux/blob/65ba8f3/include/linux/platform_data/x86/nvidia-wmi-ec-backlight.h#L16-L74
#define WMI_BRIGHTNESS_GUID "603E9613-EF25-4338-A3D0-C46177516DB7"

enum wmi_brightness_method {
	WMI_BRIGHTNESS_METHOD_LEVEL = 1,
	WMI_BRIGHTNESS_METHOD_SOURCE = 2,
	WMI_BRIGHTNESS_METHOD_MAX
};

enum wmi_brightness_mode {
	WMI_BRIGHTNESS_MODE_GET = 0,
	WMI_BRIGHTNESS_MODE_SET = 1,
	WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL = 2,
	WMI_BRIGHTNESS_MODE_MAX
};

enum wmi_brightness_source {
	WMI_BRIGHTNESS_SOURCE_GPU = 1,
	WMI_BRIGHTNESS_SOURCE_EC = 2,
	WMI_BRIGHTNESS_SOURCE_AUX = 3,
	WMI_BRIGHTNESS_SOURCE_MAX
};

struct wmi_brightness_args {
	uint32_t mode;
	uint32_t val;
	uint32_t ret;
	uint32_t ignored[3];
};

// From Linux, drivers/acpi/video_detect.c: the EC drives the backlight when the WMI method's source answers EC; Linux then uses
// nvidia-wmi-ec-backlight instead of the GPU's backlight, as it does on this laptop in hybrid mode (intel_backlight is not registered).
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/acpi/video_detect.c#L82-L102
static bool nvidia_wmi_ec_supported(struct wmi_block *w)
{
	struct wmi_brightness_args args = {};
	struct acpi_buffer buf = { (size_t)sizeof(args), &args };
	IOReturn status;

	args.mode = WMI_BRIGHTNESS_MODE_GET;
	args.val = 0;
	args.ret = 0;

	status = wmidev_evaluate_method(w, 0, WMI_BRIGHTNESS_METHOD_SOURCE, &buf, &buf);
	if (status != kIOReturnSuccess)
		return false;

	/*
	 * If brightness is handled by the EC then nvidia-wmi-ec-backlight
	 * should be used, else the GPU driver(s) should be used.
	 */
	return args.ret == WMI_BRIGHTNESS_SOURCE_EC;
}

// From Linux, drivers/platform/x86/nvidia-wmi-ec-backlight.c. The backlight class device is not there: the probe reads the maximum and
// the current level into the two properties update_status uses.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/x86/nvidia-wmi-ec-backlight.c#L31-L68
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/platform/x86/nvidia-wmi-ec-backlight.c#L90-L123
struct backlight_properties {
	uint32_t brightness;
	uint32_t max_brightness;
};

static int wmi_brightness_notify(struct wmi_block *w, enum wmi_brightness_method id, enum wmi_brightness_mode mode, uint32_t *val)
{
	struct wmi_brightness_args args = {};
	struct acpi_buffer buf = { (size_t)sizeof(args), &args };
	IOReturn status;

	args.mode = mode;
	args.val = 0;
	args.ret = 0;

	if (id < WMI_BRIGHTNESS_METHOD_LEVEL ||
	    id >= WMI_BRIGHTNESS_METHOD_MAX ||
	    mode < WMI_BRIGHTNESS_MODE_GET || mode >= WMI_BRIGHTNESS_MODE_MAX)
		return -EINVAL;

	if (mode == WMI_BRIGHTNESS_MODE_SET)
		args.val = *val;

	status = wmidev_evaluate_method(w, 0, id, &buf, &buf);
	if (status != kIOReturnSuccess) {
		static volatile uint32_t rows = 0;
		if (__sync_add_and_fetch(&rows, 1u) <= 8u)
			SYSLOG("ngreen", "EC backlight control failed: 0x%x", status);
		return -EIO;
	}

	if (mode != WMI_BRIGHTNESS_MODE_SET)
		*val = args.ret;

	return 0;
}

static int nvidia_wmi_ec_backlight_update_status(struct wmi_block *wdev, struct backlight_properties *props)
{
	return wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
				     WMI_BRIGHTNESS_MODE_SET,
				     &props->brightness);
}

static int nvidia_wmi_ec_backlight_probe(struct wmi_block *wdev, struct backlight_properties *props)
{
	int ret;

	/* drivers/acpi/video_detect.c also checks that SOURCE == EC */
	if (!nvidia_wmi_ec_supported(wdev))
		return -ENODEV;

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
				    WMI_BRIGHTNESS_MODE_GET_MAX_LEVEL,
				    &props->max_brightness);
	if (ret)
		return ret;

	ret = wmi_brightness_notify(wdev, WMI_BRIGHTNESS_METHOD_LEVEL,
				    WMI_BRIGHTNESS_MODE_GET, &props->brightness);
	if (ret)
		return ret;

	return props->max_brightness ? 0 : -ENODEV;
}

// Apple's hwSetBacklight writes only the PWM duty, which the EC does not follow. After it, the same level (IODisplayParameters' bklt,
// 0..0xffff) is scaled to max_brightness and sent through update_status from a thread call, outside Apple's caller; the newest level
// wins. The WMI block is looked up and probed once, at the first level.
static struct wmi_block nvidia_wmi_ec_backlight;
static struct backlight_properties nvidia_wmi_ec_backlight_props;
static volatile int nvidia_wmi_ec_backlight_state;	// 0 not probed yet, 1 the level follows Apple's, -1 not used
static volatile uint32_t nvidia_wmi_ec_backlight_level;
static thread_call_t nvidia_wmi_ec_backlight_call;

static void nvidia_wmi_ec_backlight_update(thread_call_param_t lock, thread_call_param_t)
{
	IOLockLock(static_cast<IOLock *>(lock));
	if (nvidia_wmi_ec_backlight_state == 0) {
		const int err = wmi_find_device_by_guid(WMI_BRIGHTNESS_GUID, &nvidia_wmi_ec_backlight);
		char method[WMI_ACPI_METHOD_NAME_SIZE] = "none";
		uint32_t source = 0;

		if (!err) {
			get_acpi_method_name(&nvidia_wmi_ec_backlight, 'M', method);
			wmi_brightness_notify(&nvidia_wmi_ec_backlight, WMI_BRIGHTNESS_METHOD_SOURCE, WMI_BRIGHTNESS_MODE_GET, &source);
		}
		const int ret = err ? err : nvidia_wmi_ec_backlight_probe(&nvidia_wmi_ec_backlight, &nvidia_wmi_ec_backlight_props);
		nvidia_wmi_ec_backlight_state = ret ? -1 : 1;
		SYSLOG("ngreen", "EC backlight: %s (%d); WMI method %s, source %u (EC = 2), max %u, level %u",
		       ret ? "not used" : "the level follows Apple's", ret, method, source, nvidia_wmi_ec_backlight_props.max_brightness,
		       nvidia_wmi_ec_backlight_props.brightness);
	}
	if (nvidia_wmi_ec_backlight_state > 0) {
		const uint32_t level = nvidia_wmi_ec_backlight_level > 0xffff ? 0xffff : nvidia_wmi_ec_backlight_level;
		auto &props = nvidia_wmi_ec_backlight_props;
		props.brightness = static_cast<uint32_t>((static_cast<uint64_t>(level) * props.max_brightness + 0x7fff) / 0xffff);
		const int ret = nvidia_wmi_ec_backlight_update_status(&nvidia_wmi_ec_backlight, &props);
		static uint32_t rows = 0;
		if (++rows <= 8)
			SYSLOG("ngreen", "EC backlight: level 0x%x of 0xffff -> %u of %u (%d)", level, props.brightness, props.max_brightness, ret);
	}
	IOLockUnlock(static_cast<IOLock *>(lock));
}

IOReturn Gen11::hwSetBacklight(AppleIntel::AppleIntelBaseController *that, uint32_t level)
{
	const IOReturn ret = FunctionCast(hwSetBacklight, callback->ohwSetBacklight)(that, level);
	if (IS_RAPTOR_LAKE_HX() && nvidia_wmi_ec_backlight_state >= 0) {
		if (!nvidia_wmi_ec_backlight_call) {
			IOLock *lock = IOLockAlloc();
			thread_call_t call = lock ? thread_call_allocate(nvidia_wmi_ec_backlight_update, lock) : nullptr;
			if (!call || !__sync_bool_compare_and_swap(&nvidia_wmi_ec_backlight_call, nullptr, call)) {
				if (call)
					thread_call_free(call);
				if (lock)
					IOLockFree(lock);
			}
		}
		nvidia_wmi_ec_backlight_level = level;
		if (nvidia_wmi_ec_backlight_call)
			thread_call_enter(nvidia_wmi_ec_backlight_call);
	}
	return ret;
}

// From i915, drivers/gpu/drm/i915/display/intel_fifo_underrun.c, bdw_set_fifo_underrun_reporting: GEN8_PIPE_FIFO_UNDERRUN is
// unmasked in GEN8_DE_PIPE_IMR while underrun reporting is on. Apple masks it on every write of pipe A's IMR; this keeps it unmasked.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/display/intel_fifo_underrun.c#L283-L290
static uint32_t fixPipeAImr(uint32_t val) {
	return IS_RAPTOR_LAKE_HX() ? val & ~GEN8_PIPE_FIFO_UNDERRUN : val;
}

void Gen11::FastWriteRegister32(AppleIntel::AppleIntelBaseController *that, unsigned long param_1, uint32_t param_2)
{
	if (NGreen::callback && NGreen::callback->dmcDisabled &&
	    ((param_1 & 0xFFFFF) == DC_STATE_EN || (param_1 & 0xFFFFF) == 0x60800 || (param_1 & 0xFFFFF) == 0x60900))
		param_2 = 0;

	// V99D: Diagnose — log all FastWrite calls near display engine range on first boot
	// to understand what addresses/values flow through this path.
	{
		static int v99DCount = 0;
		if (v99DCount < 30) {
			v99DCount++;
			SYSLOG("ngreen", "V99D[%d]: FastWrite addr=0x%lx val=0x%x",
				   v99DCount, param_1, param_2);
		}
	}

	// V72F removed (was: force RCS/BCS RING_EMR FastWrite path to 0xFFFFFFFF — same
	// blanket-mask hack as V72R/V72W, on the FastWriteRegister32 entry). Last of the
	// "raWriteRegister32-side" EMR mask trio. The V74 50ms enforcer still re-forces
	// EMR via direct writeReg32 from a polling thread, so this passthrough alone is
	// not yet a full "no blanket EMR mask" test — V74's EMR portion is stripped
	// separately in v71EmrEnforcer.
	if (param_1 == 0x20b4 || param_1 == 0x220b4) {
		static int v72FPassCount = 0;
		if (v72FPassCount < 6) {
			++v72FPassCount;
			SYSLOG("ngreen", "V72Fp[%d]: EMR @ 0x%lx val=0x%x passthrough (V72F hack removed)",
				   v72FPassCount, param_1, param_2);
		}
	}

	// V99F[S] removed (was: PLANE_STRIDE *= 8 — same X-tile-units → 64B-cacheline-units
	// rewrite as V99R[S], on the FastWriteRegister32 entry). Pair-mate to V99R[Sp].
	// V99S downstream still re-forces STRIDE=0xa0 at SURF arm via direct MMIO, so
	// this passthrough does not affect what scans out.
	if ((param_1 & 0xFFFFF) == 0x70188) {
		static int v99FSpCount = 0;
		if (v99FSpCount < 3) {
			++v99FSpCount;
			SYSLOG("ngreen", "V99F[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99F[S] hack removed)",
				   v99FSpCount, param_2);
		}
	}
	// V99F[C] removed (was: PLANE_CTL X-tiled (001) → Y-tiled-legacy (100) rewrite,
	// FastWriteRegister32 twin of V99R[C]). V99S at SURF arm still re-forces CTL
	// downstream, so this is a no-op for actually-displayed values.
	if ((param_1 & 0xFFFFF) == 0x70180) {
		static int v99FCpCount = 0;
		if (v99FCpCount < 3) {
			++v99FCpCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99F[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99F[C] hack removed)",
				   v99FCpCount, param_2, tiling);
		}
	}
	if ((param_1 & 0xFFFFF) == GEN8_DE_PIPE_IMR_A) {
		param_2 = fixPipeAImr(param_2);
	}
	// V103F removed (was: FastWriteRegister32 twin of V103 — block DC_STATE_EN
	// non-zero writes). Pair-mate to V103/V103P; all three sites now passthrough.
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103FpCount = 0;
		if (v103FpCount < 6) {
			++v103FpCount;
			SYSLOG("ngreen", "V103Fp[%d]: DC_STATE_EN FastWrite 0x%x passthrough (V103F hack removed)",
				   v103FpCount, param_2);
		}
	}

	// V195F removed (FastWriteRegister32 site of V195 — same hack pile).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195FpCount = 0;
		if (v195FpCount < 6) {
			++v195FpCount;
			SYSLOG("ngreen", "V195Fp[%d]: CTL1 FastWrite 0x%x passthrough (V195F hack removed)",
				   v195FpCount, param_2);
		}
	}

	// V99F[SURF] removed (was: at SURF arm via FastWriteRegister32, force PLANE_STRIDE=0xa0
	// — FastWrite-path twin of V99S non-dp0 STRIDE=0xa0 force in raWriteRegister32). The
	// raWriteRegister32 V99S still re-forces STRIDE at SURF arm via direct MMIO, so this
	// passthrough doesn't change what scans out.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		static int v99FSurfPassCount = 0;
		if (v99FSurfPassCount < 3) {
			++v99FSurfPassCount;
			SYSLOG("ngreen", "V99F[SURFp%d]: SURF arm 0x%x STRIDE=0x%x passthrough (V99F[SURF] hack removed)",
				   v99FSurfPassCount, param_2, hwStride);
		}
	}

	return FunctionCast(FastWriteRegister32, callback->oFastWriteRegister32)(that,param_1,param_2 );
}

void Gen11::raWriteRegister32b(void *that,void *param_1,unsigned long param_2, UInt32 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return;
	raWriteRegister32(that, reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::raWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	if (NGreen::callback && NGreen::callback->dmcDisabled &&
	    ((param_1 & 0xFFFFF) == DC_STATE_EN || (param_1 & 0xFFFFF) == 0x60800 || (param_1 & 0xFFFFF) == 0x60900))
		param_2 = 0;

	// V93: optional plane SURF zero-write guard.
	// Disabled by default due black-screen regressions; enable only with -ngreenv93.
	struct V93PlaneSurfState {
		uint32_t surfReg;
		uint32_t lastNonZeroSurf;
	};
	static V93PlaneSurfState v93States[8] {};
	static int v93LogCount = 0;

	auto getV93State = [](uint32_t surfReg) -> V93PlaneSurfState * {
		for (auto &state : v93States) {
			if (state.surfReg == surfReg)
				return &state;
		}
		for (auto &state : v93States) {
			if (state.surfReg == 0) {
				state.surfReg = surfReg;
				state.lastNonZeroSurf = 0;
				return &state;
			}
		}
		return nullptr;
	};

	if (NGreen::callback && isV93PlaneGuardEnabled()) {
		const uint32_t reg = static_cast<uint32_t>(param_1 & 0xFFFFF);
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);

		if (looksLikePlaneSurf) {
			auto *state = getV93State(reg);
			if (param_2 != 0) {
				if (state)
					state->lastNonZeroSurf = param_2;
			} else {
				const uint32_t ctlReg = reg - 0x1C; // PLANE_CTL is SURF - 0x1C on Gen11 paths.
				uint32_t planCtl = NGreen::callback->readReg32(ctlReg);
				if (planCtl & 0x80000000u) {
					if (state && state->lastNonZeroSurf != 0) {
						if (v93LogCount < 32) {
							SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping last 0x%x", reg, state->lastNonZeroSurf);
							v93LogCount++;
						}
						param_2 = state->lastNonZeroSurf;
					} else {
						uint32_t currentSurf = NGreen::callback->readReg32(reg);
						if (currentSurf != 0) {
							if (state)
								state->lastNonZeroSurf = currentSurf;
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: blocked zero SURF@0x%x while enabled; keeping current 0x%x", reg, currentSurf);
								v93LogCount++;
							}
							param_2 = currentSurf;
						} else {
							// Last resort: disable plane before allowing SURF=0 write.
							NGreen::callback->writeReg32(ctlReg, planCtl & ~0x80000000u);
							if (v93LogCount < 32) {
								SYSLOG("ngreen", "V93: forced plane disable for SURF@0x%x before zero write", reg);
								v93LogCount++;
							}
						}
					}
				}
			}
		}
	}

	// V72R removed (was: force RING_EMR (0x20b4 / 0x220b4) writes to 0xFFFFFFFF —
	// blanket-mask-all-GT-engine-errors). Hack that hides root cause; Linux i915
	// doesn't do blanket EMR masking. Remove and observe what real engine errors
	// emerge so the actual cause can be fixed.
	// NOTE: V72W (in wrapWriteRegister32 helper) and V72F (in FastWriteRegister32)
	// and V74 50ms permanent enforcer still mask RING_EMR. To fully test "no EMR
	// blanket mask" they need to be disabled in subsequent steps.

	// V99R[S] removed (was: PLANE_STRIDE *= 8 — convert X-tiled tile-units 0x14 to
	// Y-tiled/linear cacheline-units 0xa0). Hack guessing the right scanout stride
	// without knowing the actual buffer layout. Linux i915 picks stride based on
	// the IOSurface/buffer's documented tiling, not by rewriting Apple's value.
	// Removing this lets Apple's natural PLANE_STRIDE (0x14) reach hardware. If
	// scanout shows X-tiled bytes correctly → buffer was X-tiled. If garbled →
	// real layer is buffer/renderer side, not register-write.
	// NOTE: V99S (SURF arm) and V99f (fast-write variant) and V99F (FastWrite) still
	// have their own PLANE_STRIDE rewrites — to fully test, those need removal too.
	// For now keep the address-match shell so future legitimate intercepts can use it.
	if ((param_1 & 0xFFFFF) == 0x70188) { // PLANE_STRIDE Pipe A Plane 1
		static int v99SPassCount = 0;
		if (v99SPassCount < 3) {
			++v99SPassCount;
			SYSLOG("ngreen", "V99R[Sp%d]: PLANE_STRIDE 0x%x passthrough (V99R hack removed)", v99SPassCount, param_2);
		}
	}
	// V99R[C] / V99R[Cdp] removed (was: PLANE_CTL tiling rewrites — X-tiled (001) →
	// Y-tiled legacy (100) on default path, force linear under -ngreendp0). Same
	// "guess the tiling" antipattern as V99R[S]; pairs with the stride-rewrite hack
	// just removed. Linux i915 picks PLANE_CTL tiling bits from the IOSurface/buffer's
	// declared tiling, never rewrites Apple's value. With V99R[S] passthrough, leaving
	// the corresponding CTL field rewritten is contradictory: STRIDE is now 0x14
	// (X-tiled tile-units) but CTL would still be forced to Y-tiled or linear, which
	// is guaranteed-wrong scanout. Removing both lets Apple's natural CTL reach HW so
	// we can observe what tiling Apple's allocator actually chose.
	if ((param_1 & 0xFFFFF) == 0x70180) { // PLANE_CTL Pipe A Plane 1
		static int v99CPassCount = 0;
		if (v99CPassCount < 3) {
			++v99CPassCount;
			uint32_t tiling = (param_2 >> 10) & 0x7;
			SYSLOG("ngreen", "V99R[Cp%d]: PLANE_CTL 0x%x passthrough tiling=%d (V99R hack removed)",
				   v99CPassCount, param_2, tiling);
		}
	}
	if ((param_1 & 0xFFFFF) == GEN8_DE_PIPE_IMR_A) {
		param_2 = fixPipeAImr(param_2);
	}

	// V97 removed (was: clear bit[16] PORT_SYNC_MODE_MASTER_SELECT[0] from
	// TRANS_DDI_FUNC_CTL_A on !isRealTGL — Apple's SetupParams sets it but UEFI GOP
	// doesn't, allegedly disrupting trained eDP link → "InterLane Alignment is lost"
	// ~10s later). Symptom-hiding hack: real fix should match Linux i915's Display 13
	// transcoder programming, not strip a bit Apple's stack expects to set. Removing
	// reveals whether the InterLane loss still happens; if yes, look at Linux's
	// TRANS_DDI_FUNC_CTL field layout for ADL-P (Display 13) since PORT_SYNC fields
	// changed across display generations.
	if ((param_1 & 0xFFFFF) == 0x60400 && NGreen::callback && !NGreen::callback->isRealTGL) {
		static int v97RpCount = 0;
		if (v97RpCount < 6) {
			++v97RpCount;
			SYSLOG("ngreen", "V97Rp[%d]: TRANS_DDI_FUNC_CTL_A 0x%x passthrough bit16=%d (V97 hack removed)",
				   v97RpCount, param_2, !!(param_2 & (1u << 16)));
		}
	}

	// V103 removed (was: block all non-zero DC_STATE_EN writes when ADL-P DMC is
	// loaded — keeps the DMC from entering DC3/DC5/DC6 because Apple's ICL-targeted
	// driver has no ADL-P DC exit recovery, all eDP lanes drop ~70s after a write).
	// Symptom-hiding hack: Linux i915 has proper DC enter/exit for Display 13 via
	// the DMC. Permanently disabling DC states masks Apple's broken DC exit instead
	// of providing one. Removing reveals when DC_STATE_EN gets written and what
	// value Apple wants set; the fix is to ensure DC exit is properly handled (or
	// to align with what Linux ADL-P DMC expects).
	if ((param_1 & 0xFFFFF) == 0x45504 && NGreen::callback && NGreen::callback->dmcIsAdlp) {
		static int v103PassCount = 0;
		if (v103PassCount < 10) {
			++v103PassCount;
			SYSLOG("ngreen", "V103p[%d]: DC_STATE_EN 0x%x passthrough (V103 hack removed)",
				   v103PassCount, param_2);
		}
	}

	// V195 removed (was: same OR-in bits 14,12 hack as V195W, on raWriteRegister32).
	if ((param_1 & 0xFFFFF) == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
		&& NGreen::callback->uefiCtl1 != 0) {
		static int v195pCount = 0;
		if (v195pCount < 6) {
			++v195pCount;
			SYSLOG("ngreen", "V195p[%d]: CTL1 ra 0x%x passthrough (V195 hack removed)",
				   v195pCount, param_2);
		}
	}

	// V99S: PLANE_SURF arm — force correct STRIDE/CTL immediately before latching.
	// raWriteRegister32/WriteRegister32 is a CACHE-ONLY update; hardware MMIO for
	// double-buffered PLANE_STRIDE is written via a volatile* path not caught by
	// FastWriteRegister32. Forcing writeReg32 here ensures the hardware shadow is
	// correct right before SURF arms the double-buffer flip.
	//
	// dp0 path: redirect non-aperture SURF to 0x0 AND remap GGTT[0..] to the same
	// physical pages (V99G).  setupScanoutMemory migrates SURF from aperture to
	// ≥0x10000000 (non-aperture stolen RAM, phys ~0x7f…) when WindowServer sets
	// kIOWindowServerActiveAttribute=3. After migration WS writes to the non-aperture
	// physical pages; PLANE_SURF must also scan them. V99G copies the GGTT PTEs from the
	// non-aperture range down to GGTT[0..3999] so SURF=0x0 scans the same pages.
	if ((param_1 & 0xFFFFF) == 0x7019C && NGreen::callback) {
		uint32_t hwStride = NGreen::callback->readReg32(0x70188);
		uint32_t hwCtl    = NGreen::callback->readReg32(0x70180);
		static int v99SCount = 0;
		if (v99SCount < 8) {
			++v99SCount;
			// V201B: read TOP-LEFT (gray border) AND CENTER (Apple logo / loading bar
			// area) of the buffer. Linear byte offset of pixel (x,y) = y*10240 + x*4.
			// Sample points (BGRA, 2560×1600):
			//  - (0,0)         offset 0          → top-left, gray bg
			//  - (1280,800)    offset 0x7D2800   → screen center, Apple logo
			//  - (1280,1000)   offset 0x9C7800   → loading-bar row
			//  - (640,800)     offset 0x7D1A00   → mid-left of logo area
			NGreen::callback->setApertureIfNecessary();
			uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
			if (NGreen::callback->aperturePtr && NGreen::callback->apertureLen >= 0xA00000) {
				volatile uint32_t *fb32 = NGreen::callback->aperturePtr;
				tlCtr = fb32[0];             // top-left
				ctr   = fb32[0x7D2800 / 4];  // center (Apple logo)
				bar   = fb32[0x9C7800 / 4];  // loading bar row
				mid   = fb32[0x7D1A00 / 4];  // mid-left of logo area
			}
			SYSLOG("ngreen", "V99S[%d]: SURF arm 0x%x STRIDE=0x%x CTL=0x%x | tl=%08x ctr(1280,800)=%08x bar(1280,1000)=%08x mid(640,800)=%08x",
				   v99SCount, (uint32_t)param_2, hwStride, hwCtl, tlCtr, ctr, bar, mid);

			// V203: on the LAST sampled SURF arm, dump every pipe/transcoder/DSC/scaler
			// register relevant to the duplicated-content symptom. We're hunting an
			// off-by-2 in stride / src-vs-active / DSC bpp / pipe-bpc / M-N.
			if (v99SCount == 8) {
				#define R(addr) NGreen::callback->readReg32(addr)
				SYSLOG("ngreen", "V203: --- SCANOUT REGISTER DUMP ---");
				// Plane A
				SYSLOG("ngreen", "V203 PLANE_A: CTL=%08x STRIDE=%08x POS=%08x SIZE=%08x OFFSET=%08x SURF=%08x SURFLIVE=%08x AUX_DIST=%08x AUX_OFFSET=%08x KEYVAL=%08x KEYMSK=%08x KEYMAX=%08x COLOR_CTL=%08x",
					   R(0x70180), R(0x70188), R(0x7018C), R(0x70190), R(0x701A4),
					   R(0x7019C), R(0x701AC), R(0x701C0), R(0x701C4),
					   R(0x70194), R(0x70198), R(0x701A0), R(0x701CC));
				// Pipe A general
				SYSLOG("ngreen", "V203 PIPE_A: SRCSZ=%08x CONF=%08x MISC=%08x MISC2=%08x STAT=%08x",
					   R(_PIPEASRC), R(0x70008), R(0x70030), R(0x7002C), R(0x70024));
				// Transcoder A timings
				SYSLOG("ngreen", "V203 TRANS_A: HTOTAL=%08x HBLANK=%08x HSYNC=%08x VTOTAL=%08x VBLANK=%08x VSYNC=%08x VSYNCSHIFT=%08x",
					   R(0x60000), R(0x60004), R(0x60008), R(0x6000C),
					   R(0x60010), R(0x60014), R(0x60028));
				// DDI function control + MSA
				SYSLOG("ngreen", "V203 TRANS_A_DDI: DDI_FUNC_CTL=%08x DDI_FUNC_CTL2=%08x MSA_MISC=%08x CONF=%08x CLK_SEL=%08x",
					   R(0x60400), R(0x60404), R(0x60410), R(0x70008), R(0x46140));
				// DP M/N values for Pipe A
				SYSLOG("ngreen", "V203 TRANS_A_DPMN: DATAM1=%08x DATAN1=%08x DATAM2=%08x DATAN2=%08x LINKM1=%08x LINKN1=%08x LINKM2=%08x LINKN2=%08x",
					   R(0x60030), R(0x60034), R(0x60038), R(0x6003C),
					   R(0x60040), R(0x60044), R(0x60048), R(0x6004C));
				// Pipe A scaler 1
				SYSLOG("ngreen", "V203 PIPE_A_PS1: CTRL=%08x WIN_POS=%08x WIN_SZ=%08x VPHASE=%08x HPHASE=%08x",
					   R(0x68180), R(0x68170), R(0x68174), R(0x68188), R(0x68194));
				// DDI buf / port
				#undef R
				scanoutSnapshot(false);
				scanoutArmLate();
			}
		}
		/*.  ------ MAN IN THE MIDDLE CHIP HACK -------
		// V99R[P] + V99G + linear CTL/STRIDE forces — CORE scanout coherence (!isRealTGL).
		// Confirmed load-bearing for visible scanout on spoofed RPL/ADL-P in dp0, dp1, AND
		// without any -ngreendp* boot arg. Real TGL hardware programs these correctly via
		// Apple's native code path — gating the entire triad on !isRealTGL.
		//
		// Friend's architectural feedback (Visual Ehrmanntraut, NootedBlue lineage):
		// the right long-term fix is intercepting CRTCParams / PLANEPARAMS / SCALERPARAMS
		// at hwSetupMemory / paramsSurfCompare / hwRegsNeedUpdate (already partial via
		// V97P / V97C) instead of hooking MMIO writes after-the-fact. "Hacking regs only
		// won't work — leave WS alone, problem is in apple code."
		//
		//   1. SURF redirect: non-aperture writes (>=0x10000000) → 0, so the display engine
		//      always scans from the same GGTT page range (GGTT[0..3999]) no matter where
		//      Apple's setupScanoutMemory chose to migrate the surface.
		//   2. V99G: per-flip GGTT remap — copies the PTEs at the migrated surface pages
		//      down to GGTT[0..3999] so SURF=0 fetches the same physical memory WS's CPU
		//      compositor is writing into. Re-runs whenever Apple's SURF address changes
		//      (handles double/triple buffering — WS rotates between 2-3 IOSurfaces per flip).
		//   3. Linear CTL/STRIDE forces (tiling→0, STRIDE=0xa0): required for visible
		//      output. Removing them produces a black screen with no scanout activity,
		//      confirmed empirically. Side effect: produces the fragmented/repeated
		//      pattern when Apple's allocator stores buffer in non-linear physical layout
		//      — known cost of this path, removable only by struct-level fix.
		if (NGreen::callback && !NGreen::callback->isRealTGL && param_2 >= 0x10000000u) {
			static int v99PCount = 0;
			if (v99PCount < 8)
				SYSLOG("ngreen", "V99R[P%d]: SURF 0x%x->aperture (non-aperture redirect)",
					   ++v99PCount, (uint32_t)param_2);

			// ngreen-buf=N: 1=single, 2=double (default), 3=triple buffering.
			// Each buffer slot occupies 4000 GGTT pages = 0xFA0000 bytes of aperture:
			//   slot 0 → GGTT[0..3999],      SURF=0x0
			//   slot 1 → GGTT[4000..7999],   SURF=0xFA0000
			//   slot 2 → GGTT[8000..11999],  SURF=0x1F40000
			// Each unique non-aperture srcPage Apple presents is assigned a fixed slot.
			// The GGTT PTEs for that slot are remapped to point at the IOSurface's
			// physical pages. SURF is rewritten to the matching aperture slot address
			// so the display engine scans the correct physical memory.
			// With single buffering all flips always land on slot 0 (SURF=0x0).
			static int  bufCount       = -1;
			static uint32_t slotPages[3] = {0, 0, 0}; // srcPage assigned to each slot
			static int  slotCount      = 0;
			static int  v99GCount      = 0;

			if (bufCount < 0) {
				int val = 2;
				PE_parse_boot_argn("ngreen-buf", &val, sizeof(val));
				bufCount = (val >= 1 && val <= 3) ? val : 2;
				SYSLOG("ngreen", "V99G: ngreen-buf=%d (buffering slots)", bufCount);
			}

			uint32_t srcPage = (uint32_t)param_2 >> 12;

			// Find existing slot or assign a new one.
			int slot = -1;
			for (int s = 0; s < slotCount; s++) {
				if (slotPages[s] == srcPage) { slot = s; break; }
			}
			if (slot < 0) {
				if (slotCount < bufCount) {
					slot = slotCount++;
				} else {
					// All slots occupied — evict oldest (slot 0), shift down.
					for (int s = 0; s < bufCount - 1; s++) slotPages[s] = slotPages[s + 1];
					slot = bufCount - 1;
				}
				slotPages[slot] = srcPage;
			}

			// Remap GGTT[slot*4000 .. slot*4000+3999] from srcPage..srcPage+3999.
			{
				int base = slot * 4000;
				int remapped = 0, remapSkipped = 0;
				for (int i = 0; i < 4000; i++) {
					uint32_t lo = NGreen::callback->readReg32(GGTT_PTE_LO(srcPage + i));
					uint32_t hi = NGreen::callback->readReg32(GGTT_PTE_HI(srcPage + i));
					if (!(lo & 1)) { remapSkipped++; continue; }
					NGreen::callback->writeReg32(GGTT_PTE_LO(base + i), lo);
					NGreen::callback->writeReg32(GGTT_PTE_HI(base + i), hi);
					remapped++;
				}
				NGreen::callback->writeReg32(0x101008, 0x1); // flush GGTT TLB
				if (++v99GCount <= 8 || (v99GCount & 0x3F) == 0)
					SYSLOG("ngreen", "V99G[%d]: GGTT[%d..%d] <- srcPage=0x%x slot=%d remapped=%d skip=%d",
						   v99GCount, base, base + 3999, srcPage, slot, remapped, remapSkipped);
			}

			// Redirect SURF to the aperture address of the assigned slot.
			// slot 0 → 0x0, slot 1 → 0xFA0000, slot 2 → 0x1F40000.
			param_2 = (uint32_t)slot * 0xFA0000u;
		}
		// CTL/STRIDE forces — MATCH APPLE'S NATURAL INTENT.
		// V401 paramsSurfCompare logs prove Apple wants: CTL bits[12:10]=001 (X-tiled),
		// STRIDE=0x14 (20 X-tile units = 10240B/row = 2560*4bpp). Apple's IOSurface
		// allocator produces X-tiled physical buffers — Y-tile and linear forces both
		// scan wrong bytes from an X-tile buffer. Match Apple = same tile mode as the
		// buffer = correct scanout, IF the SURF address reaches the right pages
		// (V99R[P]+V99G handle the SURF redirect / GGTT remap unconditionally).
		//
		// Gated on !isRealTGL. Real TGL programs natively.
		if (NGreen::callback && !NGreen::callback->isRealTGL) {
			// force CTL linear and STRIDE=0xa0 (CPU compositor writes linearly via BAR2).
			uint32_t hwTiling = (hwCtl >> 10) & 0x7;
			if (hwTiling != 0)
				NGreen::callback->writeReg32(0x70180, hwCtl & ~(0x7u << 10));
			if (hwStride != 0xa0)
				NGreen::callback->writeReg32(0x70188, 0xa0);
		}*/
	}

	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->writeReg32(param_1,param_2);
	if (!callback->oraWriteRegister32) return NGreen::callback->writeReg32(param_1,param_2);
	FunctionCast(raWriteRegister32, callback->oraWriteRegister32)( that,param_1,param_2);
};

uint32_t Gen11::AppleIntelFramebufferinit(AppleIntel::AppleIntelFramebuffer *frame,
                                          AppleIntel::AppleIntelBaseController *cont,
                                          uint32_t pipeIndex)
{
	// Offsets into the full IOFramebuffer subclass hierarchy (much larger than the
	// tail fields captured in AppleIntelParams::AppleIntelFramebuffer).
	getMember<void *>(frame, 0x4a40) = ccont;
	getMember<void *>(frame, 0xc40)  = ccont;
	auto ret = FunctionCast(AppleIntelFramebufferinit, callback->oAppleIntelFramebufferinit)(frame, cont, pipeIndex);
	getMember<void *>(frame, 0x4a40) = ccont;
	getMember<void *>(frame, 0xc40)  = ccont;
	return ret;
}



void Gen11::initPlatformWorkarounds(AppleIntel::AppleIntelBaseController *that)
{
	// Platform workaround flags for ADL-P (RPL-P) running under TGL driver.
	// flags_ig (+0xC58): boot info flags — checked by PowerWell::init to set fAlwaysOn.
	//   Must be set BEFORE PowerWell::init runs if we want Apple's native fAlwaysOn path.
	//   We also force fAlwaysOn=1 in our PowerWell::init hook as belt-and-suspenders.
	// fInfoFlags2 (+0xC5C): display feature flags.
	//   ADL-P uses PCH PWM for backlight (cnp_setup_backlight confirmed in Linux syslog).
	//   Do NOT set FB_FLAG_ENABLE_BACKLIGHT_REG_CONTROL (that forces CPU-register backlight).
	that->flags_ig    = FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT;
	that->fInfoFlags2 =
		FB_FLAG_ALTERNATE_PWM_INCREMENT1 |
		FB_FLAG_ALTERNATE_PWM_INCREMENT2 |
		FB_FLAG_ENABLE_SLICE_FEATURES    |
		FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
		FB_FLAG_AVOID_FAST_LINK_TRAINING;

	FunctionCast(initPlatformWorkarounds, callback->oinitPlatformWorkarounds)(that);

	// V212: ADL-P (Display 13) specific display workarounds, ported from Linux i915.
	// Apple's TGL kext targets Display 12 and doesn't apply these chicken bits / clock-
	// gating / error masks on the spoofed setup. Linux marks them as REQUIRED for
	// Display 13+; their absence can manifest as display engine misbehavior
	if (NGreen::callback && !NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX()) {
		// Wa_22011091694:adlp — DPCE_GATING_DIS = REG_BIT(17) in GEN9_CLKGATE_DIS_5 (0x46540)
		NGreen::callback->intel_de_rmw(0x46540, 0, 1u << 17);

		// Bspec/49189 ADL-P init — CLEAR DDI_CLOCK_REG_ACCESS = REG_BIT(7) in GEN8_CHICKEN_DCPR_1 (0x46430)
		NGreen::callback->intel_de_rmw(0x46430, 1u << 7, 0);

		// PIPE_CHICKEN Pipe A (0x70038):
		//   bit 30 = UNDERRUN_RECOVERY_DISABLE_ADLP — required on Display 13+
		//   bit 7  = PER_PIXEL_ALPHA_BYPASS_EN     — Display WA #1153
		//   bit 15 = PIXEL_ROUNDING_TRUNC_FB_PASSTHRU — Display WA #1605353570
		NGreen::callback->intel_de_rmw(0x70038, 0, (1u << 30) | (1u << 15) | (1u << 7));

		// XELPD_DISPLAY_ERR_FATAL_MASK (0x4421C) ← mask all fatal display errors on
		// Display 13 (per icl_display_core_init in Linux i915). Without this, fatal
		// error events can trigger pipeline restart loops.
		NGreen::callback->writeReg32(0x4421C, 0xFFFFFFFFu);

		uint32_t pipeChicken    = NGreen::callback->readReg32(0x70038);
		uint32_t clkGateDis5    = NGreen::callback->readReg32(0x46540);
		uint32_t chickenDcpr1   = NGreen::callback->readReg32(0x46430);
		uint32_t errFatalMask   = NGreen::callback->readReg32(0x4421C);
		SYSLOG("ngreen", "V212: ADL-P Display 13+ workarounds applied — PIPE_CHICKEN(A)=0x%x CLKGATE_DIS_5=0x%x CHICKEN_DCPR_1=0x%x ERR_FATAL_MASK=0x%x",
			   pipeChicken, clkGateDis5, chickenDcpr1, errFatalMask);
	}
}

static constexpr size_t kAGDCCallbackFnOffset  = 0x1c00;
static constexpr size_t kAGDCCallbackCtxOffset = 0x1c08;

static uint32_t agdcCallbackStub(void *, uint64_t, uint32_t, uint32_t)
{
	return 1;
}

uint64_t Gen11::tglCreateAppleGraphicsDeviceControlnub(void *that)
{
	if (NGreen::callback && !NGreen::callback->isRealTGL) {
		if (that != nullptr) {
			auto *base = static_cast<uint8_t *>(that);
			auto **fn  = reinterpret_cast<void **>(base + kAGDCCallbackFnOffset);
			auto **ctx = reinterpret_cast<void **>(base + kAGDCCallbackCtxOffset);
			if (*fn == nullptr && *ctx == nullptr) {
				*ctx = that;
				*fn  = reinterpret_cast<void *>(agdcCallbackStub);
			}
		}

		armAgdcNub(that, callback->otglCreateAppleGraphicsDeviceControlnub);

		return kIOReturnSuccess;
	}
	return FunctionCast(tglCreateAppleGraphicsDeviceControlnub, callback->otglCreateAppleGraphicsDeviceControlnub)(that);
}

uint64_t Gen11::metricsLogStub(void *, uint32_t, uint32_t, uint32_t)
{
	return 0;
}

uint32_t Gen11::hwSaveNVRAMStub(void *)
{
	return 0;
}

uint64_t Gen11::getOSInformation(AppleIntel::AppleIntelBaseController *that)
{
	auto *pinfo = reinterpret_cast<PlatformInfo *>(callback->gPlatformInformationList);
	if (pinfo) {
		// Index 1 = the mobile TGL/ADL-P platform entry (0x9A490000 and variants).
		pinfo[1].fInfoFlags =
			FB_FLAG_DISABLE_PIPE_SCRAMBLE      |
			FB_FLAG_FRAMEBUFFER_COMPRESSION    |
			FB_FLAG_ALLOW_CONNECTOR_RECOVER    |
			FB_FLAG_FORCE_POWER_ALWAYS_CONNECTED |
			FB_FLAG_AVOID_FAST_LINK_TRAINING;

		// cameliav=2 (CamelliaTcon2) requires GFX kext present — gate on -ngreentglwithgfx.
		pinfo[1].cameliav = checkKernelArgument("-ngreentglwithgfx") ? 2 : 0;
		pinfo[1].fMobile  = 1;
		// 3/3/3 baseline restored. Multi-pipe reduction is whack-a-mole — every count
		// reduction reveals new cross-pipe NULL-deref sites in TGL FB internals
		// (enableController +0x1356, getFreeJoinablePathCount +0xa7, etc).
		// Best known config: 3/3/3 + -ngreendp0 → reaches login with banded display.
		pinfo[1].fPipeCount            = 3;
		pinfo[1].fInfoPortCount        = 3;
		pinfo[1].fInfoFramebufferCount = 3;
		pinfo[1].fSliceCount  = 1;
		pinfo[1].fmaxEuCount  = 8;
		pinfo[1].fsubslices   = 4;

		// Connector 0: built-in eDP (LVDS), DDI-A, pipe 0
		pinfo[1].connectors[0].index = 0;
		pinfo[1].connectors[0].busId = 0;
		pinfo[1].connectors[0].pipe  = 0;
		pinfo[1].connectors[0].pad   = 0;
		pinfo[1].connectors[0].type  = ConnectorLVDS;
		pinfo[1].connectors[0].flags = 0x8 | 0x10;

		// Connector 1: external USB-C/Thunderbolt DP (TC1/DDI-D), pipe 2
		pinfo[1].connectors[1].index = 1;
		pinfo[1].connectors[1].busId = 1;
		pinfo[1].connectors[1].pipe  = 2;
		pinfo[1].connectors[1].pad   = 0;
		pinfo[1].connectors[1].type  = ConnectorDP;
		pinfo[1].connectors[1].flags = 0x1 | 0x400;

		// Connectors 2-3: Dummy
		pinfo[1].connectors[2] = { 2, 2, 2, 0, ConnectorDummy, 0 };
		pinfo[1].connectors[3] = { 3, 3, 3, 0, ConnectorDummy, 0 };

		SYSLOG("ngreen", "getOSInformation: patched pinfo[1] for ADL-P (LVDS+HDMI, mobile)");
	}
	return FunctionCast(getOSInformation, callback->ogetOSInformation)(that);
}

int Gen11::handleLinkIntegrityCheck()
{
	return 0;
};

void Gen11::hwInitializeCState(AppleIntel::AppleIntelBaseController *that)
{
	SYSLOG("ngreen", "NB-BUILD-V50-ALLOW-METAL");

	int origB48 = getMember<int>(that, 0xB48);
	int origCE4 = getMember<int>(that, 0xCE4);
	SYSLOG("ngreen", "hwInitCState B48=%d CE4=%d", origB48, origCE4);

	// Boot-arg "ngreen-dmc":
	//   not set or "skip" (0xA788: any value) → no DMC: DC states and PSR off, AUX only
	//   "tgl"             → load TGL DMC v2.12 blob + TGL display engine registers
	//                       + ICL/TGL combo PHY signal levels (PHY_A eDP, PHY_B DP)
	//   "adlp"            → load ADL-P DMC v2.16 blob + ADL-P display engine registers
	//                       + combo PHY signal levels (PHY_A eDP)
	//   "icl"             → passthrough original ICL DMC load + ICL combo PHY signal levels
	char dmcArg[16] = {};
	PE_parse_boot_argn("ngreen-dmc", dmcArg, sizeof(dmcArg));
	NGreen::callback->dmcDisabled = false;
	const bool raptorLakeHX = NGreen::callback->isRaptorLakeHX();

	if (!raptorLakeHX && (dmcArg[0] == 't' || dmcArg[0] == 'T')) {
		// ── TGL DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=tgl, loading TGL DMC v2.12 (%u dwords)", tgl_dmc_ver2_12_bin_s / 4);
		// Write TGL DMC blob to MMIO 0x80000+
		for (unsigned long off = 0; off < tgl_dmc_ver2_12_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				*(const uint32_t *)((const char *)tgl_dmc_ver2_12_bin + off));

		// Disable DC states before touching display engine registers (same as ADL-P path).
		// DC_STATE_EN = 0x45504
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Use UEFI CTL1 as base (same pattern as ADL-P path).
		uint32_t tglUefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1 = tglUefiCtl1 | 0x00000401u;
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2
			SYSLOG("ngreen", "V101T: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03", tglUefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		pwrWellRequestAndWait(0x45440, 0x00000003);
		pwrWellRequestAndWait(0x45444, 0x00000003);
		pwrWellRequestAndWait(0x45450, 0x00000003);
		pwrWellRequestAndWait(0x45454, 0x00000003);

		// TGL display engine registers (DMC trigger/context regs in 0x8Fxxx range)
		// Values from IDA of original hwInitializeCState (TGL-native values)
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;        // reuse flag: also protects TGL path via V103/V104P
		NGreen::callback->uefiCtl1  = tglUefiCtl1;
		SYSLOG("ngreen", "hwInitCState: TGL DMC loaded");
		// setSignalLevels block REMOVED for the same reason as the ADL-P branch:
		// Linux i915 (per /Volumes/EFI/syslog.txt drm trace) only calls setSignalLevels
		// during DP link training, where swing=0/0/0/0 is the negotiation STARTING point
		// before DPRX adjust-request bumps it to vswing=1/1/1/1. Calling it once at
		// hwInitCState with all-zero levels freezes the combo PHY at the lowest drive
		// strength forever → DP receivers read garbage bitRates, link silently fails.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevels(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		// }
		// Let original run with B48=1 (ICL CSR blob loads to SRAM).
		// Same rationale as ADL-P: the TGL DMC firmware also causes lane drops because its
		// DC state management runs on ADL-P hardware; the ICL DMC is safer on this silicon.
		// TGL context regs (8Fxxx) are re-applied after so the display engine sees TGL values.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply TGL context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00006FC0);
		NGreen::callback->writeReg32(0x8F004, 0x00A40088);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		SYSLOG("ngreen", "V104T: TGL context regs re-applied after ICL blob load");
		// V102T: restore CTL1 after original (ICL DMC save/restore table may write 0x401).
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = tglUefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102T: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}

	} else if (!raptorLakeHX && (dmcArg[0] == 'a' || dmcArg[0] == 'A')) {
		// ── ADL-P DMC ──
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=adlp, loading ADL-P DMC v2.16 (%u dwords)", adlp_dmc_ver2_16_bin_s / 4);
		// adlp_dmc_ver2_16_bin is the raw firmware payload extracted from the v3 blob
		// (adlp_dmc_ver2_16.bin: CSS+package header stripped, pipe-A payload at file offset 0x310).
		// Write directly to SRAM starting at 0x80000.
		for (unsigned long off = 0; off < adlp_dmc_ver2_16_bin_s; off += 4)
			FastWriteRegister32(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(ccont), off + 0x80000,
				*(const uint32_t *)((const char *)adlp_dmc_ver2_16_bin + off));

		// Disable DC states before touching display engine registers.
		// If DC5/DC6 is active when we write, the clock-gated blocks won't latch the writes.
		// DC_STATE_EN = 0x45504 (confirmed: Archive HIGH, linux display/intel_display_regs.h)
		NGreen::callback->writeReg32(DC_STATE_EN, 0);

		// Power wells — Gen12 ICL-style DDI + AUX power well enable.
		// HSW_PWR_WELL_CTL1/2 (0x45400/45404): PG1/PG2 enable+state — values read from
		// Linux intel_reg dump on same hardware (reg_dump.txt):
		//   CTL1=0x00000401 (PG1 req+enabled), CTL2=0x00000C03 (PG1+PG2 req+enabled)
		//   CTL3=0x40000000 (PG3 state only), CTL4=0x00000401
		// V100: preserve UEFI CTL1 state bits (bits 12,14 = display power wells enabled).
		// Writing the hardcoded 0x401 clears those bits, breaking vsync interrupt delivery
		// → WindowServer crash at ~60s ("Display not ready").
		// Fix: read UEFI CTL1 and OR in our minimum bits; keep CTL2 hardcoded at 0x0C03.
		// DO NOT OR CTL2 with UEFI: UEFI leaves 0xfc00 (TC port "state" bits) set in CTL2.
		// Asserting TC bits without IOM handshake disrupts the display domain during mode
		// change, preventing the WSA un-blank (stays at 0x1, no cursor, no desktop).
		uint32_t uefiCtl1 = NGreen::callback->readReg32(0x45400);
		{
			uint32_t newCtl1  = uefiCtl1 | 0x00000401u;  // ensure PG1 req+state bits set
			NGreen::callback->writeReg32(0x45400, newCtl1); // HSW_PWR_WELL_CTL1
			NGreen::callback->writeReg32(0x45404, 0x00000C03); // HSW_PWR_WELL_CTL2 — hardcoded, no TC bits
			SYSLOG("ngreen", "V101: PWR_WELL CTL1 uefi=0x%x->0x%x CTL2=0xc03",
				   uefiCtl1, newCtl1);
		}
		NGreen::callback->writeReg32(0x45408, 0x40000000); // HSW_PWR_WELL_CTL3
		NGreen::callback->writeReg32(0x4540C, 0x00000401); // HSW_PWR_WELL_CTL4
		// ICL_PWR_WELL_CTL_AUX1/2 (0x45440/45444): enable AUX power wells A+B
		// bit1=req, bit0=enabled per ICL DDI power well HW spec (intel_display_regs.h)
		pwrWellRequestAndWait(0x45440, 0x00000003);
		pwrWellRequestAndWait(0x45444, 0x00000003);
		// ICL_PWR_WELL_CTL_DDI1/2 (0x45450/45454): enable DDI power wells A+B
		pwrWellRequestAndWait(0x45450, 0x00000003);
		pwrWellRequestAndWait(0x45454, 0x00000003);

		// ADL-P / RPL-P display engine registers — exact MMIO init pairs from v3 blob header
		// (adlp_dmc_ver2_16.bin header @ 0x0210, MMIO[0..6], stepping='A' variant)
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200); // was missing
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98); // corrected from 0x512050D4
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0); // corrected from 0x584C57FC
		// DDI C-F (ADL-P TC port registers — DKL PHY, 0x5Fxxx range)
		NGreen::callback->writeReg32(0x5F074, 0x00096FC0);
		NGreen::callback->writeReg32(0x5F034, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F004, 0x214C2114);
		NGreen::callback->writeReg32(0x5F038, 0xC003E000);
		NGreen::callback->writeReg32(0x5F008, 0x22402208);
		NGreen::callback->writeReg32(0x5F03C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F00C, 0x241422FC);
		NGreen::callback->writeReg32(0x5F040, 0xC0033100);
		NGreen::callback->writeReg32(0x5F010, 0x26F826CC);
		NGreen::callback->writeReg32(0x5F474, 0x0009EFC0);
		NGreen::callback->writeReg32(0x5F434, 0xC003DF00);
		NGreen::callback->writeReg32(0x5F404, 0xA968A930);
		NGreen::callback->writeReg32(0x5F438, 0xC003E000);
		NGreen::callback->writeReg32(0x5F408, 0xAA5CAA24);
		NGreen::callback->writeReg32(0x5F43C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F40C, 0xAC30AB18);
		NGreen::callback->writeReg32(0x5F440, 0xC0033100);
		NGreen::callback->writeReg32(0x5F410, 0xAF14AEE8);
		NGreen::callback->writeReg32(0x5F874, 0x00053FC0);
		NGreen::callback->writeReg32(0x5F83C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5F80C, 0x25202408);
		NGreen::callback->writeReg32(0x5F840, 0xC0033100);
		NGreen::callback->writeReg32(0x5F810, 0x280427D8);
		NGreen::callback->writeReg32(0x5FC3C, 0xC0032C00);
		NGreen::callback->writeReg32(0x5FC0C, 0x95209408);
		NGreen::callback->writeReg32(0x5FC40, 0xC0033100);
		NGreen::callback->writeReg32(0x5FC10, 0x980497D8);

		// Transcoder A DDI function + timing registers (re-imported from EFI reference for safety).
		// Values from Linux intel_reg dump on this hardware (reg_dump.txt).
		//   0x60400 = TRANS_DDI_FUNC_CTL_A  — DDI A enabled, DP SST, 8bpc, 2 lanes
		//   0x60000 = TRANS_HTOTAL_A        — 2560 active, 2720 total
		//   0x60004 = TRANS_HBLANK_A        — same as HTOTAL on eDP
		//   0x60008 = TRANS_HSYNC_A         — sync positions
		//   0x6000C = TRANS_VTOTAL_A        — 1600 active, 1750 total
		//   0x60010 = TRANS_VBLANK_A
		//   0x60014 = TRANS_VSYNC_A
		//   0x60028 = TRANS_VSYNCSHIFT_A
		//   0x60030/34 TRANS_DATA_M1/N1_A   — DP M/N values (TU 64, link rate)
		//   0x60040/44 TRANS_LINK_M1/N1_A
		// 0x8A000106: bit31=enable, [27:24]=DDI_A, [19:16]=2lanes, [3:1]=DP_SST(0x01)
		// Pre-matches Apple's target so paramsFbCompare sees no lane-count change; changing
		// lane count in TRANS_DDI_FUNC_CTL while transcoder is live resets the DDI buffer
		// and drops the trained link.
		NGreen::callback->writeReg32(0x60400, 0x8A000106); // TRANS_DDI_FUNC_CTL_A
		NGreen::callback->writeReg32(0x60000, 0x0A9F09FF); // TRANS_HTOTAL_A
		NGreen::callback->writeReg32(0x60004, 0x0A9F09FF); // TRANS_HBLANK_A
		NGreen::callback->writeReg32(0x60008, 0x0A4F0A2F); // TRANS_HSYNC_A
		NGreen::callback->writeReg32(0x6000C, 0x06D5063F); // TRANS_VTOTAL_A
		NGreen::callback->writeReg32(0x60010, 0x06D50000); // TRANS_VBLANK_A
		NGreen::callback->writeReg32(0x60014, 0x06480642); // TRANS_VSYNC_A
		NGreen::callback->writeReg32(0x60028, 0x00000000); // TRANS_VSYNCSHIFT_A
		NGreen::callback->writeReg32(0x60030, 0x7E5D159E); // TRANS_DATA_M1_A  (TU 64, M=0x5d159e)
		NGreen::callback->writeReg32(0x60034, 0x00800000); // TRANS_DATA_N1_A  (N=0x800000)
		NGreen::callback->writeReg32(0x60040, 0x0007C1CD); // TRANS_LINK_M1_A  (M=0x7c1cd)
		NGreen::callback->writeReg32(0x60044, 0x00080000); // TRANS_LINK_N1_A  (N=0x80000)

		// Panel power sequencer (re-imported from EFI reference).
		// TGL/ADL-P both have PCH_SPLIT (ICP/TGP PCH) → intel_pps_setup sets
		// mmio_base = PCH_PPS_BASE = 0xC7200 (not 0x61200 which is BXT/APL).
		// Values from Linux intel_reg dump on this hardware:
		//   0xC7204 (PP_CONTROL)   = 0x00000067 (panel on, VDD on, power-on target)
		//   0xC7208 (PP_ON_DELAYS) = 0x07D00001 (T1=1, T3=2000ms power-on delays)
		NGreen::callback->writeReg32(0xC7204, 0x00000067); // PP_CONTROL
		NGreen::callback->writeReg32(0xC7208, 0x07D00001); // PP_ON_DELAYS

		// PIPE_CLK_SEL_A (0x46140 = 0x10000000) REMOVED after the EFI re-import caused
		// "CD Clock PLL is locked" line to disappear from the FB log and link bitRates to
		// turn into garbage (27/40/63 instead of 179/204/206). Empirical: writing this
		// value to 0x46140 here either selects a clock source that prevents CD PLL lock,
		// or 0x46140 isn't actually PIPE_CLK_SEL on Display 13 (ADL-P) — Linux i915
		// names it differently for ADL-P. Leave for Apple's later mode-setup to program.
		// NGreen::callback->writeReg32(0x46140, 0x10000000); // PIPE_CLK_SEL_A

		// Enable DMC — DC_STATE_DEBUG (0x45520) = 2
		NGreen::callback->writeReg32(0x45520, 2); // DC_STATE_DEBUG
		NGreen::callback->dmcIsAdlp = true;
		NGreen::callback->uefiCtl1  = uefiCtl1;  // save for V60 re-enforcement
		SYSLOG("ngreen", "hwInitCState: ADL-P DMC loaded");
		// setSignalLevelsADLP block REMOVED after the EFI re-import caused link bitRates
		// to turn into garbage (27/40/63 instead of 179/204/206). Likely cause: calling
		// it with swing[]=preEmph[]={0,0,0,0} BEFORE Apple's link training overwrites
		// the UEFI-trained combo PHY DW2/4/5/7 with zero levels → DP link reads back
		// nonsense bitrates. The setSignalLevels function should only be invoked DURING
		// link training when swing/preEmph are properly populated, not as init scaffolding.
		// Left as a comment so we can re-enable surgically once real swing values are known.
		// {
		// 	uint8_t swing[4]   = {0, 0, 0, 0};
		// 	uint8_t preEmph[4] = {0, 0, 0, 0};
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/true,  swing, preEmph);
		// 	IntelDPLinkTraining::setSignalLevelsADLP(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isEDP=*/false, swing, preEmph);
		// }
		// Let original run with B48=1 so the ICL CSR blob is loaded to SRAM.
		// The ADL-P DMC firmware (even correct binary) autonomously drops all eDP lanes
		// at ~10s because its DC state management code runs on ADL-P hardware and performs
		// link maintenance that the ICL driver can't recover from. The ICL DMC running on
		// ADL-P hardware does NOT cause lane drops (it doesn't know ADL-P DC3CO/PSR2).
		// Our ADL-P display engine context regs (8Fxxx) are re-applied after the original
		// so the display engine still sees ADL-P-correct values despite ICL SRAM content.
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Re-apply ADL-P context regs overwritten by original's ICL blob load.
		NGreen::callback->writeReg32(0x8F074, 0x00086FC0);
		NGreen::callback->writeReg32(0x8F034, 0xC003B400);
		NGreen::callback->writeReg32(0x8F004, 0x01240108);
		NGreen::callback->writeReg32(0x8F038, 0xC003B200);
		NGreen::callback->writeReg32(0x8F008, 0x4FE44F98);
		NGreen::callback->writeReg32(0x8F03C, 0xC003B300);
		NGreen::callback->writeReg32(0x8F00C, 0x571056C0);
		SYSLOG("ngreen", "V104: ADL-P context regs re-applied after ICL blob load");
		// V102: CTL1 restore — ICL DMC save/restore table writes CTL1=0x401, clearing
		{
			uint32_t postCtl1 = NGreen::callback->readReg32(0x45400);
			uint32_t fixCtl1  = uefiCtl1 | 0x00000401u;
			if (postCtl1 != fixCtl1) {
				NGreen::callback->writeReg32(0x45400, fixCtl1);
				SYSLOG("ngreen", "V102: restore PWR_WELL CTL1 0x%x->0x%x", postCtl1, fixCtl1);
			}
		}
		// V105: disable PSR1+PSR2 — the ICL DMC initializer (original hwInitializeCState)
		// enables PSR2 (EDP_PSR2_CTL bit 0 = 1) for the eDP panel. On ADL-P hardware with
		// the ICL driver the PSR2 selective-update path is non-functional: the panel locks
		// into self-refresh showing the initial black frame; only the cursor plane (separate
		// SU path) updates. Clearing both registers before the first frame is displayed
		// restores normal scanout. Confirmed root cause via V76: PSR2_CTL=0x4811, V88
		// direct physical writes to SURF invisible despite valid GGTT PTEs.
		{
			// V105 ADDRESS FIX: PSR1 control on TGL is at 0x60800 (transcoder EDP space),
			// NOT 0x64800 (older Gen ICL/SKL layout). Pre-fix V105 was writing to a wrong
			// register; PSR1 stayed enabled at 0x60800 = 0x00100001, panel kept refreshing
			// from its own cache, screen frozen on first frame even while PIPE_FRMCOUNT
			// kept advancing. Probe V205 caught this. Now writes to both addresses for
			// safety (0x60800 = TGL, 0x64800 = legacy/ICL — covers both spoof paths).
			uint32_t psr2ctl     = NGreen::callback->readReg32(0x60900);  // EDP_PSR2_CTL TGL
			uint32_t psr1ctlTgl  = NGreen::callback->readReg32(0x60800);  // EDP_PSR_CTL  TGL
			uint32_t psr1ctlIcl  = NGreen::callback->readReg32(0x64800);  // EDP_PSR_CTL  ICL
			uint32_t psr2ctlIcl  = NGreen::callback->readReg32(0x60900);  // EDP_PSR2_CTL ICL
			NGreen::callback->writeReg32(0x60900, 0);  // PSR2 TGL
			NGreen::callback->writeReg32(0x60800, 0);  // PSR1 TGL  ← THE ACTUAL FIX
			NGreen::callback->writeReg32(0x60900, 0);  // PSR2 legacy
			NGreen::callback->writeReg32(0x64800, 0);  // PSR1 legacy
			SYSLOG("ngreen", "V105: PSR disabled — TGL(PSR1=0x%x PSR2=0x%x) legacy(PSR1=0x%x PSR2=0x%x)",
				   psr1ctlTgl, psr2ctl, psr1ctlIcl, psr2ctlIcl);
		}

	} else if (!raptorLakeHX && (dmcArg[0] == 'i' || dmcArg[0] == 'I')) {
		// ── ICL ──
		// ICL is the native target: let original hwInitializeCState load the ICL DMC blob,
		// then program ICL combo PHY signal levels (same DW2/4/5/7 layout as TGL).
		SYSLOG("ngreen", "hwInitCState: ngreen-dmc=icl, passthrough + ICL combo PHY signal levels");
		FunctionCast(hwInitializeCState, callback->ohwInitializeCState)(that);
		// Program combo PHY signal levels — PHY_A (eDP, 4 lanes, HBR) + PHY_B (DP-B, 4 lanes, HBR)
		{
			uint8_t swing[4]   = {0, 0, 0, 0};
			uint8_t preEmph[4] = {0, 0, 0, 0};
			IntelDPLinkTraining::setSignalLevels(/*phy=*/0, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
			IntelDPLinkTraining::setSignalLevels(/*phy=*/1, /*lanes=*/4, /*isHBR2=*/false, /*isDP=*/true, swing, preEmph);
		}
		SYSLOG("ngreen", "hwInitCState: ICL done");

	} else {
		NGreen::callback->dmcIsAdlp = false;
		NGreen::callback->dmcDisabled = true;
		NGreen::callback->writeReg32(DC_STATE_EN, 0);
		NGreen::callback->writeReg32(0x60800, 0);
		NGreen::callback->writeReg32(0x60900, 0);
	}

	if (raptorLakeHX && !ensureA788DisplayAWells(false)) {
		return;
	}
	hwConfigureCustomAUX(that, true);
	SYSLOG("ngreen", "hwInitCState: done");
}

void NGreen::adlpDcExit(const char *caller) {
	if (isRaptorLakeHX())
		ensureA788DisplayAWells();
	if (!dmcIsAdlp) return;
	const uint32_t dcState = readReg32(0x45504); // DC_STATE_EN
	if (dcState == 0) return;
	static int adlpDcExitCount = 0;
	if (adlpDcExitCount < 48) {
		adlpDcExitCount++;
		SYSLOG("ngreen", "adlpDcExit[%s/%d]: DC_STATE_EN=0x%x — restoring ADL-P display state", caller, adlpDcExitCount, dcState);
	}
	// 1. Disable DC states so clock-gated blocks latch the writes below.
	writeReg32(0x45504, 0);
	// 2. Restore power wells (request enable on CTL1/2/3/4 + AUX A/B + DDI A/B).
	writeReg32(0x45400, uefiCtl1 | 0x401u);  // PWR_WELL_CTL1
	writeReg32(0x45404, 0x0C03u);             // PWR_WELL_CTL2
	writeReg32(0x45408, 0x40000000u);         // PWR_WELL_CTL3
	writeReg32(0x4540C, 0x401u);              // PWR_WELL_CTL4
	writeReg32(0x45440, 0x3u);               // ICL_PWR_WELL_CTL_AUX1 — AUX A
	writeReg32(0x45444, 0x3u);               // ICL_PWR_WELL_CTL_AUX2 — AUX B
	writeReg32(0x45450, 0x3u);               // ICL_PWR_WELL_CTL_DDI1 — DDI A
	writeReg32(0x45454, 0x3u);               // ICL_PWR_WELL_CTL_DDI2 — DDI B
	// 3. Restore ADL-P display engine context registers (saved by DMC on DC entry).
	writeReg32(0x8F074, 0x00086FC0u);
	writeReg32(0x8F034, 0xC003B400u);
	writeReg32(0x8F004, 0x01240108u);
	writeReg32(0x8F038, 0xC003B200u);
	writeReg32(0x8F008, 0x4FE44F98u);
	writeReg32(0x8F03C, 0xC003B300u);
	writeReg32(0x8F00C, 0x571056C0u);
	// 4. Restore panel power sequencer.
	writeReg32(0xC7204, 0x67u);  // PP_CONTROL
	// 5. Disable PSR — DMC may have re-enabled it on DC exit.
	writeReg32(0x60800, 0u);   // EDP_PSR_CTL (TGL)
	writeReg32(0x60900, 0u);   // EDP_PSR2_CTL (TGL)
	if (adlpDcExitCount <= 48) {
		SYSLOG("ngreen", "adlpDcExit[%s]: restore complete", caller);
	}
}

void Gen11::AppleIntelPowerWellinit(AppleIntel::AppleIntelPowerWell *that, AppleIntel::AppleIntelBaseController *param_1)
{
	ccont = param_1->fRegCachePool;

	FunctionCast(AppleIntelPowerWellinit, callback->oAppleIntelPowerWellinit)(that, param_1);

	// After callthrough the south display domain is clocked; direct BAR read is safe.
	uint32_t pg  = NGreen::callback->readReg32(0x45404);
	ensureA788DisplayAWells();

	// Apple's PowerWell::init checks fController->flags_ig & FB_FLAG_BOOST_PIXEL_FREQUENCY_LIMIT
	// (+0xC58) before setting fAlwaysOn=1. On RPL/ADL that flag isn't set when the kext first
	// calls PowerWell::init (initPlatformWorkarounds runs later), so fAlwaysOn stays 0 and
	// Apple can gate power wells off. We force fAlwaysOn=1 unconditionally on non-real-TGL.
	// Also stamp fMMIO in case Apple's TGL-path init skipped it on ADL-P hardware.
	SYSLOG("ngreen", "PowerWell::init — flags_ig=0x%x fAlwaysOn(before)=%u fMMIO=%p",
		   param_1->flags_ig, that->fAlwaysOn, that->fMMIO);
	if (!NGreen::callback->isRealTGL) {
		that->fAlwaysOn = 1;
		if (!that->fMMIO) that->fMMIO = reinterpret_cast<AppleIntel::AppleIntelMMIO *>(ccont);
		SYSLOG("ngreen", "PowerWell::init forced fAlwaysOn=1, fMMIO=%p fMMIOBase=%p",
			   that->fMMIO, that->fMMIO ? that->fMMIO->fMMIOBase : nullptr);
	}
	SYSLOG("ngreen", "PowerWell::init done — fAlwaysOn=%u fPGBase=%u PG1=%u PG2=%u PG3=%u PG4=%u",
		   that->fAlwaysOn, that->fPGBase, that->fPG1, that->fPG2, that->fPG3, that->fPG4);
	SYSLOG("ngreen", "PowerWell::init DDI — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fDDI[0], that->fDDI[1], that->fDDI[2], that->fDDI[3], that->fDDI[4],
		   that->fDDI[5], that->fDDI[6], that->fDDI[7], that->fDDI[8]);
	SYSLOG("ngreen", "PowerWell::init AUX — [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u [5]=%u [6]=%u [7]=%u [8]=%u",
		   that->fAUX[0], that->fAUX[1], that->fAUX[2], that->fAUX[3], that->fAUX[4],
		   that->fAUX[5], that->fAUX[6], that->fAUX[7], that->fAUX[8]);
}

// ─── Sleep/wake lifecycle hooks ──────────────────────────────────────────────
// All four Apple implementations set this+0x49e0=4 as their final operation.
// If Apple's code exits early (null Camellia ptr, lock contention, etc.) that
// write never happens and the FB state machine is stuck.  We callthrough and
// then enforce state=4 regardless, plus track panel power for IOKit properties.
//
// Lifecycle order:
//   Sleep: prepareToEnterSleep → prepareToExitWake
//   Wake:  prepareToExitSleep  → prepareToEnterWake

static void logFBCtrlState(const char *tag, AppleIntel::AppleIntelFramebuffer *that)
{
	auto *ctrl = that->fController;
	uint32_t fbState = getMember<uint32_t>(that, 0x49e0);
	if (ctrl) {
		SYSLOG("ngreen", "%s fb=%p fbState=%u flags_ig=0x%x fInfoFlags2=0x%x fGPUIsAwake=%u pipe=%u",
			   tag, that, fbState,
			   ctrl->flags_ig, ctrl->fInfoFlags2, getMember<uint32_t>(ctrl, 0x1A00),
			   that->fPipeIndex);
	} else {
		SYSLOG("ngreen", "%s fb=%p fbState=%u ctrl=NULL pipe=%u", tag, that, fbState, that->fPipeIndex);
	}
}

// Wake stage 2: confirm panel is on, log state.
// Apple does: panel-power property, some vtable dispatch, then state=4.
void Gen11::prepareToEnterWake(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToEnterWake>>", that);
	bool panelOn = isPanelPowerOn(getMember<void *>(that, 0x1d0));
	if (panelOn) {
		reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", true);
	}
	FunctionCast(prepareToEnterWake, callback->oprepareToEnterWake)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToEnterWake: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToEnterWake<<", that);
}

// Sleep stage 2: NVRAM save, Camellia backlight off, StopTransactions,
// disableDisplay, hwSetPanelPower(0), two handleEvent calls, then state=4.
void Gen11::prepareToExitWake(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToExitWake>>", that);
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", false);
	FunctionCast(prepareToExitWake, callback->oprepareToExitWake)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToExitWake: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToExitWake<<", that);
}

// Sleep stage 1: fSleeping=1, cancel timers, clientNotify(2,0), state=4,
// handleEvent(sleep), hwSaveState, hwDisableInterrupts, fGPUIsAwake=0.
void Gen11::prepareToEnterSleep(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToEnterSleep>>", that);
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", false);
	FunctionCast(prepareToEnterSleep, callback->oprepareToEnterSleep)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToEnterSleep: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	logFBCtrlState("prepareToEnterSleep<<", that);
}

// Wake stage 1: restorePowerWellsState, hwEnableInterrupts, hwInitializeCState,
// hwRestoreState, probePortState per port, gamma/reg-cache restore,
// clientNotify(2,1), handleEvent(wake), fGPUIsAwake=1, setDPPowerState, state=4.
void Gen11::prepareToExitSleep(AppleIntel::AppleIntelFramebuffer *that)
{
	logFBCtrlState("prepareToExitSleep>>", that);
	FunctionCast(prepareToExitSleep, callback->oprepareToExitSleep)(that);
	uint32_t state_after = getMember<uint32_t>(that, 0x49e0);
	if (state_after != 4) {
		SYSLOG("ngreen", "prepareToExitSleep: state=0x%x (expected 4), forcing", state_after);
		getMember<uint32_t>(that, 0x49e0) = 4;
	}
	reinterpret_cast<IORegistryEntry *>(that)->setProperty("AAPL,LCD-PowerState-ON", true);
	logFBCtrlState("prepareToExitSleep<<", that);
}

bool Gen11::AppleIntelBaseControllerstart(AppleIntel::AppleIntelBaseController *that, IOService *param_1)
{
	// V25: Display workarounds BEFORE start (no ForceWake needed for display regs 0x4xxxx+).
	// GT workarounds moved AFTER start (ForceWake must be held for GT regs 0x0-0x7FFF).
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: applying display workarounds");

	// Disable DC states during init to prevent power domain conflicts
	NGreen::callback->writeReg32(DC_STATE_EN, 0);
	
	/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
	NGreen::callback->intel_de_rmw(SOUTH_DSPCLK_GATE_D, 0,
				PCH_DPMGUNIT_CLOCK_GATE_DISABLE);
	
	// PCH reset handshake
	NGreen::callback->intel_de_rmw(HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE,
				RESET_PCH_HANDSHAKE_ENABLE);
	
	/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
	NGreen::callback->intel_de_rmw(GEN11_CHICKEN_DCPR_2, 0,
				DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
				DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);
	
	/* Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl (Wa_14010480278) */
	NGreen::callback->intel_de_rmw(GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);
	
	/* Wa_14013723622 */
	NGreen::callback->intel_de_rmw(CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);
	
	SYSLOG("ngreen", "AppleIntelBaseControllerstart: display workarounds applied");

	SYSLOG("ngreen", "FBController::start() entering...");
	auto ret=FunctionCast(AppleIntelBaseControllerstart, callback->oAppleIntelBaseControllerstart)(that,param_1 );
	SYSLOG("ngreen", "FBController::start() returned %d", ret);
	
	if (ret) {
		// The personality dict itself was registered in IOCatalogue from the HW-kext
		// processKext branch (see Gen11::injectAcceleratorPersonality). All this wrapper
		// does — and all an FB-tier route should do — is poke the FBController service
		// so IOKit re-runs matching now that the personality is present.
		auto *service = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(that));
		if (service) {
			SYSLOG("ngreen", "FBController: calling registerService() to trigger accelerator matching");
			service->registerService();
		}
	}

	return ret;
}

uint32_t Gen11::wrapProbeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	// Sonoma probeCDClockFrequency checks reg 0x46070 (BXT_DE_PLL_ENABLE) bit 31 first.
	// If bit 31 is CLEAR it panics immediately: "Wrong CD clock frequency set by EFI".
	// EFI on Hackintosh may leave this clear, so we ensure it is set before calling the original.
	auto squash = NGreen::callback->readReg32(BXT_DE_PLL_ENABLE);  // byte addr OK — readReg32 divides internally
	if (!(squash & BXT_DE_PLL_PLL_ENABLE)) {
		DBGLOG("ngreen", "wrapProbeCDClockFrequency: BXT_DE_PLL_PLL_ENABLE (0x46070 bit31) was clear (0x%x), setting it", squash);
		NGreen::callback->writeReg32(BXT_DE_PLL_ENABLE, squash | BXT_DE_PLL_PLL_ENABLE);
	}

	// Always sanitize (disable + reprogram PLL) regardless of current CDCLK value.
	//
	// When BIOS leaves CDCLK already at >= 648 MHz (threshold), skipping sanitize causes
	// orgProbeCDClockFrequency to take the "cdclk already at target" path, which reads PCU
	// mailbox command 0x6 (PCODE_CDCLK_CONFIG verify). On ADL-P this returns 0x9b9b9b9b
	// (PCU not responding / command not supported), causing initCDClock to return an error.
	// start() then skips port allocation and boot display setup entirely — no hwUpdateCursorMemory,
	// no cursor GGTT entries → full black screen even when the display link is trained.
	//
	// Always calling sanitize takes the "frequency changed" success path in initCDClock,
	// which writes mailbox(7,2) ACK and skips the failing PCU 0x6 verify read.
	auto cdclk = NGreen::callback->readReg32(ICL_REG_CDCLK_CTL) & CDCLK_FREQ_DECIMAL_MASK;  // byte addr OK
	SYSLOG("ngreen", "wrapProbeCDClockFrequency: cdclk=0x%x, force-sanitizing to bypass PCU mailbox failure", cdclk);
	sanitizeCDClockFrequency(that);

	auto retVal = callback->orgProbeCDClockFrequency(that);
	return retVal;
}

void Gen11::sanitizeCDClockFrequency(AppleIntel::AppleIntelBaseController *that) {

	//auto referenceFrequency = callback->wrapReadRegister32(that, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	auto referenceFrequency =NGreen::callback->readReg32(ICL_REG_DSSM)>> 29;
	//auto referenceFrequency = callback->wrapReadRegister32(that, ICL_REG_DSSM) >> 29;
	uint32_t newCdclkFrequency = 0;
	uint32_t newPLLFrequency = 0;
	switch (referenceFrequency) {
		case ICL_REF_CLOCK_FREQ_19_2:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_19_2;
			break;
			
		case ICL_REF_CLOCK_FREQ_24_0:
			newCdclkFrequency = ICL_CDCLK_FREQ_648_0;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_24_0;
			break;
			
		case ICL_REF_CLOCK_FREQ_38_4:
			newCdclkFrequency = ICL_CDCLK_FREQ_652_8;
			newPLLFrequency = ICL_CDCLK_PLL_FREQ_REF_38_4;
			break;
			
		default:
			return;
	}

	DBGLOG("ngreen", "sanitizeCDClockFrequency: ref=%u targetCdclk=0x%x targetPll=0x%x", referenceFrequency, newCdclkFrequency, newPLLFrequency);

	// Use solved original directly so sanitize remains safe even when disableCDClock route is toggled off.
	if (callback->orgDisableCDClock) {
		callback->orgDisableCDClock(that);
	} else {
		disableCDClock(that);
	}

	callback->orgSetCDClockFrequency(that, newPLLFrequency);

}

/*
void Gen11::initCDClock(void *that)
{
	// Mirrors AppleIntelFramebufferController::initCDClock decompiled flow.
	// DSSM bits [31:29] encode the reference clock frequency (0=24MHz, 1=19.2MHz, 2=38.4MHz).
	// The check < 0x60000000 ensures bits[31:29] < 3 (i.e. a valid reference).
	auto dssm = NGreen::callback->readReg32(ICL_REG_DSSM);
	if (dssm >= 0x60000000) {
		// Invalid / unsupported reference clock — fall back to original which will panic/reset.
		return FunctionCast(initCDClock, callback->oinitCDClock)(that);
	}

	uint32_t refFreqIdx = dssm >> 0x1d;                       // bits [31:29]
	getMember<uint32_t>(that, 0xe98) = refFreqIdx;

	// Call through our safe wrapper so BXT_DE_PLL_ENABLE / CDCLK sanity checks are applied.
	auto probedFreq = static_cast<uint64_t>(wrapProbeCDClockFrequency(that));
	getMember<uint64_t>(that, 0xe88) = probedFreq;

	if (probedFreq == 0) {
		// Fallback table — mirrors DAT_00081140[refFreqIdx * 0x24], first uint32 per entry.
		static const uint32_t kDefaultCDClkFreq[] = {
			ICL_CDCLK_FREQ_648_0,   // index 0: 24.0 MHz ref → 648 MHz
			ICL_CDCLK_FREQ_652_8,   // index 1: 19.2 MHz ref → 652.8 MHz
			ICL_CDCLK_FREQ_652_8,   // index 2: 38.4 MHz ref → 652.8 MHz
		};
		probedFreq = (refFreqIdx < arrsize(kDefaultCDClkFreq))
			? kDefaultCDClkFreq[refFreqIdx]
			: ICL_CDCLK_FREQ_648_0;
	}

	getMember<uint64_t>(that, 0xe90) = probedFreq;
}
*/
void Gen11::initCDClock(AppleIntel::AppleIntelBaseController *that)
{
	return FunctionCast(initCDClock, callback->oinitCDClock)(that);
}

void Gen11::setCDClockFrequencyOnHotplug(AppleIntel::AppleIntelBaseController *that)
{
	return FunctionCast(setCDClockFrequencyOnHotplug, callback->osetCDClockFrequencyOnHotplug)(that );
}


void Gen11::disableCDClock(AppleIntel::AppleIntelBaseController *that)
{
	FunctionCast(disableCDClock, callback->odisableCDClock)(that );
}

uint8_t Gen11::hwRegsNeedUpdate
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3,
		   const IODetailedTimingInformationV2 *param_4,
		   AppleIntel::SCALERPARAMS *param_5)
{
	// ADL-P DC exit: restore power wells and context before Apple writes registers.
	if (NGreen::callback->dmcIsAdlp)
		NGreen::callback->adlpDcExit("hwRNU");

	// param_3 is the pending CRTCParams built by SetupParams.
	if (!NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX() && param_3) {
		auto *params = param_3;

		// V97P: clear bit[16] in TRANS_DDI_FUNC_CTL.
		// Apple's SetupParams sets bit16 as a port-type flag; UEFI/HW never sets it.
		// Without this the DDI FUNC_CTL compare fires and triggers a full modeset
		// that disrupts the already-trained 4-lane eDP link → black screen.
		if (params->TRANS_DDI_FUNC_CTL & (1u << 16)) {
			static int v97PCount = 0;
			if (v97PCount < 12) {
				v97PCount++;
				SYSLOG("ngreen", "V97P[%d]: CRTCParams TRANS_DDI_FUNC_CTL 0x%x -> 0x%x",
					   v97PCount, params->TRANS_DDI_FUNC_CTL,
					   params->TRANS_DDI_FUNC_CTL & ~(1u << 16));
			}
			params->TRANS_DDI_FUNC_CTL &= ~(1u << 16);
		}

		// V97C: align pending TRANS_CONF with the live HW value.
		// paramsFbCompare logs "TRANS_CONF 0xc0000000->0xc0000024": HW has bits[5,2]
		// clear (UEFI default), Apple wants to set them (interlace/depth config).
		// Writing those bits to an active pipe causes a transient signal disruption
		// that sets the panel's DPCD InterLane Alignment Lost bit (0x202=0x80),
		// which checkLinkStatus detects ~10 s later and tears down the display.
		// Fix: replace the pending TRANS_CONF with the current HW register value so
		// paramsFbCompare sees no change and the partial pipe update is suppressed.
		// PIPE_CONF_A (= TRANS_CONF in ICL+) = 0x70008.
		// NOTE: 0x60008 is TRANS_HSYNC_A (horizontal sync timing) — do NOT use that.
		const uint32_t hwTransConf = NGreen::callback->readReg32(0x70008);
		if (params->TRANS_CONF != hwTransConf) {
			static int v97CCount = 0;
			if (v97CCount < 12) {
				v97CCount++;
				SYSLOG("ngreen", "V97C[%d]: CRTCParams TRANS_CONF 0x%x -> HW 0x%x (suppressed pipe update)",
					   v97CCount, params->TRANS_CONF, hwTransConf);
			}
			params->TRANS_CONF = hwTransConf;
		}

		// V300 REVERTED — was a memory-write hack zeroing CRTCParams[+0xE8]/[+0xEC] to
		// kill DSC engine select bits. Per jalavoui's feedback ("stop hacking memory
		// writes, fix the origin not the destination"), DSC should be controlled at
		// getDPCDInfo / Info.plist FeatureControl level, not patched after-the-fact in
		// the CRTCParams struct. Our Info.plist already sets DSCSupport=0/DSCCapReporting=0
		// (confirmed by fb.log "DSC supported: 0" / "DSC Caps Reporting enabled: 0"),
		// so if Apple still calls setupDSCEngineParams the path is somewhere downstream
		// that ignores FeatureControl — needs investigation at the origin function, not
		// here. Linux on this hardware: VBT Port A DSC:0, link runs "DSC off" → DSC was
		// likely a wrong hypothesis for the fragmentation symptom in the first place.
	}

	static int CCount = 0;
	CCount++;
	SYSLOG("ngreen", "V97C[%d]: hwRegsNeedUpdate called", CCount);
	// Return the original result so that register reprogramming proceeds normally.
	// The lane count mismatch (4→2) that previously broke the display is now fixed
	// by the computeLaneCount hook forcing 4 lanes.  Without register updates, the
	// plane surface address and stride never get written, leaving the stale BIOS
	// framebuffer on screen (grey/black vertical bars with only cursor visible).
	return FunctionCast(hwRegsNeedUpdate, callback->ohwRegsNeedUpdate)(that, param_1, param_2, param_3, param_4, param_5);
}

uint8_t *gIOFBVerbosePtr = nullptr, *gIOFBBlackPtr = nullptr;
mach_vm_address_t gOrgInitFB = 0;
volatile uint32_t gBootFillRouted = 0;
static volatile uint32_t gBootFillCanary = 0;

void wrapInitFB(void *fb) {
	uint8_t *vp = gIOFBVerbosePtr, *bp = gIOFBBlackPtr;
	const uint8_t v0 = vp != nullptr ? *vp : 0xffu, b0 = bp != nullptr ? *bp : 0xffu;
	uint32_t canary = gBootFillCanary;
	if (canary == 0 && vp != nullptr) {
		canary = (v0 == 0 && b0 <= 1u) ? 1u : 2u;
		gBootFillCanary = canary;
	}
	const bool force = vp != nullptr && bp != nullptr && canary == 1 && (gBootFillRouted & 0x7u) == 0x7u;
	if (force) { *vp = 1; *bp = 1; }
	FunctionCast(wrapInitFB, gOrgInitFB)(fb);
	if (force) { *vp = v0; *bp = b0; }
}

static void bootFillZero(uint32_t surf, uint64_t len) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || cb->gpuDevice() == nullptr || len == 0) return;
	IODeviceMemory *bar = cb->gpuDevice()->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
	if (bar == nullptr || static_cast<uint64_t>(surf) + len > bar->getLength()) return;
	IODeviceMemory *sub = IODeviceMemory::withSubRange(bar, surf, static_cast<IOPhysicalLength>(len));
	if (sub == nullptr) return;
	IOMemoryMap *map = sub->map(kIOMapWriteCombineCache);
	if (map != nullptr) {
		bzero(reinterpret_cast<void *>(static_cast<uintptr_t>(map->getVirtualAddress())), static_cast<size_t>(len));
		asm volatile("sfence" ::: "memory");
		map->release();
	}
	sub->release();
}

// The fence of the boot surface is read where i915's i965_write_fence_reg writes it (FENCE_REG_GEN6_LO / HI: the base and the
// valid bit low, the pitch in 128-byte units minus one high); the height is pipe A's source size, as PIPESRC_HEIGHT_MASK holds it.
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_ggtt_fencing.c#L62-L110
static void bootFillAfterSetup(void *fb, bool isAperture, bool required, int ret) {
	auto *cb = NGreen::callback;
	if (cb == nullptr || fb == nullptr || !isAperture || !required || ret != 0) return;
	const uint32_t surf = getMember<uint32_t>(fb, 0x4330), stride = getMember<uint32_t>(fb, 0x4334);
	const uint32_t idx = getMember<uint8_t>(fb, 0x4288);
	if (idx > 31u) return;
	const uint32_t lo = cb->readReg32(FENCE_REG_GEN6_LO(idx)), hi = cb->readReg32(FENCE_REG_GEN6_HI(idx));
	const bool fenced = (lo & 1u) != 0 && (lo & 0xfffff000u) == (surf & 0xfffff000u) && stride >= 512u
	                    && (hi & 0x7ffu) == (stride >> 7) - 1u;
	const uint32_t h = (cb->readReg32(_PIPEASRC) & 0x1fffu) + 1u;
	const uint64_t len = static_cast<uint64_t>(stride) * h;
	cb->setApertureIfNecessary();
	const bool sane = fenced && h >= 480u && h <= 4320u && stride <= 65536u && len <= (64ull << 20)
	                  && static_cast<uint64_t>(surf) + len <= cb->bootApertureLen();
	if (sane) bootFillZero(surf, len);
}

int Gen11::wrapHwSetupMemory(AppleIntel::AppleIntelBaseController *that, AppleIntel::AppleIntelFramebuffer *fb, AppleIntel::AppleIntelDisplayPath *displayPath, AppleIntel::CRTCParams *params, bool isAperture)
{
	int ret = FunctionCast(wrapHwSetupMemory, callback->ohwSetupMemory)(that, fb, displayPath, params, isAperture);

	// V201: post-hwSetupMemory probe — read 32 bytes at the SURF GTT offset through BAR2.
	// SURF address is at fb+0x4330 (per hwSetupMemory decomp at offset 0x6E046).
	// If buffer is all zeros → freshly-allocated IOBufferMemoryDescriptor pages, nothing
	// has rendered into them yet. If non-zero → some path filled them (EFI GOP carry-over,
	// CoreDisplay handoff blit, or other).
	static int v201Count = 0;
	if (v201Count < 8 && NGreen::callback) {
		uint32_t surfAddr = getMember<uint32_t>(fb, 0x4330);
		uint32_t fbSize   = getMember<uint32_t>(fb, 0x4334);
		uint8_t  fbIdx    = getMember<uint8_t>(fb,  0x4288);
		uint8_t  yTileFlg = getMember<uint8_t>(fb,  0x4A18);

		NGreen::callback->setApertureIfNecessary();
		// Sample top-left and screen-center for 2560×1600 BGRA layout.
		uint32_t tlCtr = 0xDEADBEEF, ctr = 0xDEADBEEF, bar = 0xDEADBEEF, mid = 0xDEADBEEF;
		if (NGreen::callback->aperturePtr && surfAddr + 0xA00000 <= NGreen::callback->apertureLen) {
			volatile uint32_t *fb32 = NGreen::callback->aperturePtr + (surfAddr / sizeof(uint32_t));
			tlCtr = fb32[0];
			ctr   = fb32[0x7D2800 / 4];  // (1280,800) Apple logo center
			bar   = fb32[0x9C7800 / 4];  // (1280,1000) loading-bar row
			mid   = fb32[0x7D1A00 / 4];  // (640,800) mid-left of logo area
		}
		// V201P: read GGTT PTE for the surface page — verifies display controller can access it.
		// GGTT PTE LO/HI at GEN8_GGTT_PTE_BASE(0x800000) + page*8. Valid: bit0=present, bit3=LLC.
		uint32_t pteLo = 0, pteHi = 0;
		if (surfAddr && NGreen::callback->mmioValid()) {
			uint32_t surfPage = surfAddr >> 12;
			pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage));
			pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage));
		}
		v201Count++;
		SYSLOG("ngreen", "V201[%d]: hwSetupMemory ret=0x%x fb=%p surf=0x%x size=0x%x idx=%u tile=%u | tl=%08x ctr=%08x bar=%08x mid=%08x | PTE=%08x:%08x(present=%d llc=%d)",
			   v201Count, ret, fb, surfAddr, fbSize, fbIdx, yTileFlg, tlCtr, ctr, bar, mid,
			   pteHi, pteLo, pteLo & 1, (pteLo >> 3) & 1);
	}

	bootFillAfterSetup(fb, isAperture, callback != nullptr && callback->oIsApertureMemoryRequired != 0
	               && FunctionCast(wrapIsApertureMemoryRequired, callback->oIsApertureMemoryRequired)(fb), ret);

	return ret;
}

// The IOVARendererID the TGL VA driver reports for itself (AppleIntelTGLGraphicsVADriver _AVD_CreateAVDAccelerator); AppleGVA's
// allowlist takes it, not the personality's 17301568 (0x1080040).
static constexpr uint32_t kVARendererIDTGLDriver = 0x1080080u;

// HW hooks

static void v44ScheduleBundleLog(void *accelInstance, unsigned delayMs);
static void v45ScheduleDelayedCheck(void *accelInstance, unsigned delayMs);

struct DelayedServiceCall {
	IOService *service;
	thread_call_t call;
	unsigned delayMs;
};

unsigned long Gen11::start(void *that,void  *param_1)
{
	static uint32_t startCount = 0;
	const uint32_t startIndex = __sync_fetch_and_add(&startCount, 1);
	const bool irqWatchdogQuiesceOwner = startIndex == 0 && !NGreen::callback->isRealTGL;
	if (irqWatchdogQuiesceOwner) {
		__sync_lock_test_and_set(&gIrqWatchdogStop, 0u);
		__sync_lock_test_and_set(&gIrqWatchdogActive, 0u);
		__sync_lock_test_and_set(&gIrqWatchdogPasses, 0u);
		Gen11::gGfxAccelStartDone = false;
	}
	// V44: Configurable scheduler type.
	// populateAccelConfig reads "GraphicsSchedulerSelect" from the IORegistry.
	// Types: 3=IGGuC (firmware), 4=IGScheduler4, 5=IGScheduler5 (host preemptive).
	// Priority: boot-arg "ngreenSched=N" > Info.plist "SchedulerType" > default.
	// V52: Default depends on platform — real TGL uses GuC (3), RPL uses Host (5).
	auto *service = static_cast<IOService *>(that);
	{
		int schedType = NGreen::callback->isRealTGL ? 3 : 5;
		
		// 1. Check boot-arg first (highest priority)
		int bootArgSched = 0;
		if (PE_parse_boot_argn("ngreenSched", &bootArgSched, sizeof(bootArgSched)) && bootArgSched >= 3 && bootArgSched <= 5) {
			schedType = bootArgSched;
			SYSLOG("ngreen", "V44: scheduler type %d from boot-arg ngreenSched", schedType);
		} else {
			// 2. Check NootedGreen's own IORegistry properties (from Info.plist)
			auto *myService = OSDynamicCast(IOService, reinterpret_cast<OSObject *>(param_1));
			if (myService) {
				auto *stProp = OSDynamicCast(OSNumber, myService->getProperty("SchedulerType"));
				if (stProp) {
					int val = (int)stProp->unsigned32BitValue();
					if (val >= 3 && val <= 5) {
						schedType = val;
						SYSLOG("ngreen", "V44: scheduler type %d from Info.plist SchedulerType", schedType);
					}
				}
			}
			if (schedType == 5) {
				SYSLOG("ngreen", "V44: using default scheduler type 5 (host preemptive)");
			}
		}
		
		auto *schedNum = OSNumber::withNumber(static_cast<unsigned long long>(schedType), 32);
		if (schedNum) {
			service->setProperty("GraphicsSchedulerSelect", schedNum);
			schedNum->release();
			SYSLOG("ngreen", "V44: injected GraphicsSchedulerSelect=%d", schedType);
		}
	}

	// Inject MultiForceWakeSelect=1 and CsbProcModeSelect=1, directly and into the Development dictionary.
	// This tells the accelerator to use SafeForceWakeMultithreaded (which we hook)
	// instead of the framebuffer's SafeForceWake (which fails on RPL-P with ACK=0).
	// V52: Only needed on RPL — real TGL's native ForceWake works fine.
	if (!NGreen::callback->isRealTGL) {
		auto *directNum = OSNumber::withNumber(1ULL, 32);
		if (directNum) {
			service->setProperty("MultiForceWakeSelect", directNum);
			service->setProperty("CsbProcModeSelect", directNum);
			directNum->release();
		}

	auto *devDict = OSDynamicCast(OSDictionary, service->getProperty("Development"));
	if (devDict) {
		auto *newDevDict = OSDictionary::withDictionary(devDict);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
					newDevDict->setObject("CsbProcModeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
		}
	} else {
		auto *newDevDict = OSDictionary::withCapacity(4);
		if (newDevDict) {
			auto *num = OSNumber::withNumber(1ULL, 32);
			if (num) {
				newDevDict->setObject("MultiForceWakeSelect", num);
					newDevDict->setObject("CsbProcModeSelect", num);
				num->release();
			}
			service->setProperty("Development", newDevDict);
			newDevDict->release();
		}
	}
	}

	if (IS_RAPTOR_LAKE_HX()) {
		auto *zero = OSNumber::withNumber(0ULL, 32);
		if (zero != nullptr) service->setProperty("EnableMultiContext", zero);
		OSSafeReleaseNULL(zero);
	}

	// ── V29: Apply GT workarounds + GGTT PTE diagnostics ──
	SYSLOG("ngreen", "Pre-start: acquiring ForceWake for GT workarounds");
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	uint32_t fwAck = 0;
	uint64_t fwNow = 0, fwDeadline = 0;
	clock_interval_to_deadline(50, kMillisecondScale, &fwDeadline);
	for (clock_get_uptime(&fwNow); fwNow < fwDeadline; clock_get_uptime(&fwNow)) {
		fwAck = NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9);
		if (fwAck & 1) break;
	}
	SYSLOG("ngreen", "Pre-start ForceWake ACK: 0x%x %s", fwAck, (fwAck & 1) ? "OK" : "TIMEOUT");
	
	// ── V29 NEW: Pre-start GGTT PTE dump ──
	// Dump first few GGTT PTEs to verify format
	initStolenRange();
	SYSLOG("ngreen", "GGTT PTE format check (first 4 pages):");
	for (int i = 0; i < 4; i++) {
		SYSLOG("ngreen", "  GGTT[%d]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	// Dump PTEs around the HWS page area (GGTT offset 0x40004000 → page 0x40004)
	// But that PTE would be at 0x800000 + 0x40004*8 = 0xA00020 — check if within BAR
	// Instead, dump some PTEs in the driver's active range
	SYSLOG("ngreen", "GGTT PTE near page 0x100:");
	for (int i = 0x100; i < 0x104; i++) {
		SYSLOG("ngreen", "  GGTT[0x%x]=0x%08x:%08x", i,
			NGreen::callback->readReg32(GGTT_PTE_HI(i)),
			NGreen::callback->readReg32(GGTT_PTE_LO(i)));
	}
	
	// ── V29 NEW: Context and engine config ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	{
		uint32_t errPreStart = NGreen::callback->readReg32(ERROR_GEN6);
		SYSLOG("ngreen", "Pre-start ERROR_GEN6=0x%x", errPreStart);

		// V55: Full TLB flush + 5x clear BEFORE start().
		// Simple W1C (0x37 -> 0x37 confirmed in log) cannot clear sticky TLB-fault bits.
		// Post-start V55 proved: TLB invalidation via 0xcee8 + 5x clear succeeds (0x37 -> 0x0).
		// Moving the same sequence here so Apple's getBlit3DContext sees a clean ERROR_GEN6.
		// ForceWake is already held at this point (Pre-start ForceWake ACK logged above).
		if (errPreStart && !NGreen::callback->isRealTGL) {
			uint32_t ccidBefore = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));

			// 1. GDRST render domain — evicts the stale EFI/firmware context (CCID=0x80)
			// that keeps re-asserting ERROR_GEN6 into memory invalid in macOS address space.
			// TLB flush + W1C alone can't win: the broken context re-fires errors immediately.
			NGreen::callback->writeReg32(GEN6_GDRST, GEN6_GRDOM_RENDER);
			uint32_t gdrstVal = GEN6_GRDOM_RENDER;
			int gdrstPoll = 0;
			while ((gdrstVal & GEN6_GRDOM_RENDER) && gdrstPoll < 2000) {
				gdrstVal = NGreen::callback->readReg32(GEN6_GDRST);
				gdrstPoll++;
			}
			uint32_t ccidAfter = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));
			SYSLOG("ngreen", "V55G: pre-start GDRST poll=%d gdrst=0x%x CCID 0x%x->0x%x ERR->0x%x",
				gdrstPoll, gdrstVal, ccidBefore, ccidAfter,
				NGreen::callback->readReg32(ERROR_GEN6));

			// Re-acquire ForceWake — GDRST resets the render engine's FW tracking
			NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
			IODelay(1000);

			// 2. Invalidate RCS + BCS TLBs (GEN12_GUC_TLB_INV_CR = 0xcee8)
			NGreen::callback->writeReg32(0xcee8, 0x1);   // RCS TLBs
			IODelay(500);
			NGreen::callback->writeReg32(0xcee8, 0x4);   // BCS TLBs
			IODelay(500);
			SYSLOG("ngreen", "V55: Pre-start TLB invalidation requested (RCS+BCS)");

			// 3. 5x clear loop — should now succeed with stale context evicted
			uint32_t errLoop = NGreen::callback->readReg32(ERROR_GEN6);
			for (int i = 0; i < 5 && errLoop; i++) {
				NGreen::callback->writeReg32(ERROR_GEN6, errLoop);
				IODelay(200);
				errLoop = NGreen::callback->readReg32(ERROR_GEN6);
			}
			SYSLOG("ngreen", "V55: Pre-start ERROR_GEN6 after GDRST + TLB flush + 5x clear = 0x%x", errLoop);

			// 4. Clear per-engine EIR
			uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			uint32_t bcsEir = NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE));
			if (rcsEir) NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
			if (bcsEir) NGreen::callback->writeReg32(RING_EIR(BLT_RING_BASE), bcsEir);

			uint32_t tlbStatus = NGreen::callback->readReg32(0xceec);
			SYSLOG("ngreen", "V55: Pre-start TLB_INV done status = 0x%x", tlbStatus);
		}
	}
	
	// GT workarounds
	uint32_t misccpctl = NGreen::callback->readReg32(GEN7_MISCCPCTL);
	misccpctl &= ~GEN12_DOP_CLOCK_GATE_RENDER_ENABLE;
	NGreen::callback->writeReg32(GEN7_MISCCPCTL, misccpctl);
	applyRenderWa(NGreen::callback, true);
	NGreen::callback->wa_mcr_write_or(GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3,
			GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	// V162: PERCTX_PREEMPT_CTRL (bit 14) is a TGL-specific workaround that bakes
	// into the execlist context image template.  On RPL the bit freezes the EU
	// thread dispatcher on the first Metal context-image DMA restore.  Only enable
	// it on genuine TGL hardware.
	if (NGreen::callback->isRealTGL) {
		NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1,
				GEN9_FFSC_PERCTX_PREEMPT_CTRL);
	}
	
	SYSLOG("ngreen", "Pre-start: GT workarounds applied, calling original start()");
	
	// Release Render ForceWake
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	
	// V42: Save accelerator instance for child enumeration in hangcheck
	callback->accelInstance = that;
	
	// V54: Pre-enable Master IRQ BEFORE calling original start().
	// V53 proved that the TGL driver never enables GFX_MSTR_IRQ bit 31 on RPL.
	// Without it, completion interrupts never reach the CPU and start() hangs
	// for ~15s until our hangcheck accidentally re-enabled it.
	if (!NGreen::callback->isRealTGL) {
		// Enable Master IRQ (bit 31)
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		IODelay(100);
		SYSLOG("ngreen", "V54: Pre-enabled Master IRQ: GFX_MSTR_IRQ=0x%x",
			NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ));
		
		// V65: Pre-enable RCS0+BCS tier-1 interrupt routing BEFORE ring init.
		// V64 proved GEN11_RENDER_COPY_INTR_ENABLE had RCS0 (bit 0) DISABLED,
		// causing scheduler to never receive completion interrupts → timeout → dead engine.
		// Must be set BEFORE ring activates (T+8-10s) to prevent scheduler timeout.
		uint32_t rcIntrPre = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		uint32_t newEn = rcIntrPre | wantBits;
		NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
		// Also unmask RCS context-switch + user interrupts at tier-2
		uint32_t rcsMaskPre = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, rcsMaskPre & ~wantUnmasked);
		// Pre-clear any stale ERROR_GEN6 to prevent error handler interference
		uint32_t errPre = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPre) NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V65: Pre-start interrupt fix: RC_INTR_EN 0x%x->0x%x, RCS_MASK 0x%x->0x%x, ERR=0x%x",
			rcIntrPre, NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
			rcsMaskPre, NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK), errPre);
	}
	
	// V54: Schedule IRQ watchdog timer — re-enables Master IRQ every 2s during start().
	if (irqWatchdogQuiesceOwner) {
		auto timerCall = thread_call_allocate(v54IrqWatchdog, nullptr);
		if (timerCall) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter1_delayed(timerCall, timerCall, deadline);
			SYSLOG("ngreen", "V54: IRQ watchdog armed — will re-enable Master IRQ every 2s");
		}
	}
	
	// V116: Remap GGTT page 0 to a safe dummy page BEFORE original start().
	// Root cause of package-wide MCE (Banks 9/11/15, all CPUs, addr 0x3e800000):
	// GGTT[0] PTE points to stolen memory base. Any GPU access to VA=0
	// (ring buffer at addr 0, PLANE_SURF=0, etc.) reads stolen mem → LLC UCE → MCE.
	// Fix: redirect GGTT[0] to a wired zero-page so stray VA-0 reads are harmless.
	// Real TGL keeps native GGTT mapping; spoof path needs this MCE guard.
	if (!NGreen::callback->isRealTGL) {
		if (!v116DummyBuf) {
			v116DummyBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
				kernel_task,
				kIOMemoryPhysicallyContiguous | kIODirectionInOut,
				4096,
				0x00000000FFFFF000ULL);  // keep below 4GB for 32-bit GGTT addressing
			if (v116DummyBuf && v116DummyBuf->prepare() == kIOReturnSuccess) {
				v116DummyPhys = v116DummyBuf->getPhysicalAddress();
				auto *m = v116DummyBuf->createMappingInTask(kernel_task, 0, kIOMapAnywhere, 0, 4096);
				if (m) { bzero((void *)m->getVirtualAddress(), 4096); m->release(); }
				SYSLOG("ngreen", "V116: dummy page allocated phys=0x%llx",
					   (unsigned long long)v116DummyPhys);
			} else {
				OSSafeReleaseNULL(v116DummyBuf);
				SYSLOG("ngreen", "V116: dummy page alloc/prepare FAILED");
			}
		}
		if (v116DummyPhys && NGreen::callback->mmioValid()) {
			// PTE format (Gen8+): bits[47:12]=phys PFN, bit[3]=LLC-coherent, bit[0]=present
			uint32_t pteLo = (uint32_t)(v116DummyPhys & 0xFFFFF000ULL) | 0x9;  // LLC | present
			uint32_t pteHi = (uint32_t)(v116DummyPhys >> 32);
			uint32_t oldLo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
			uint32_t oldHi = NGreen::callback->readReg32(GGTT_PTE_HI(0));
			NGreen::callback->writeReg32(GGTT_PTE_LO(0), pteLo);
			NGreen::callback->writeReg32(GGTT_PTE_HI(0), pteHi);
			NGreen::callback->writeReg32(0x101008, 0x1);  // GGTT TLB flush
			SYSLOG("ngreen", "V116: GGTT[0] 0x%08x:%08x -> 0x%08x:%08x (dummy phys=0x%llx)",
				   oldHi, oldLo, pteHi, pteLo, (unsigned long long)v116DummyPhys);
		}
	}

	auto ret= FunctionCast(start, callback->ostart)(that,param_1);
	if (irqWatchdogQuiesceOwner) {
		__sync_lock_test_and_set(&gIrqWatchdogStop, 1u);
		__sync_synchronize();
		while (__sync_fetch_and_or(&gIrqWatchdogActive, 0u) != 0) {
			IOSleep(1);
		}
	}

	// V221: Signal waitForStamp hook that the GFX interrupt handler is now installed.
	Gen11::gGfxAccelStartDone = ret != 0;
	if (!NGreen::callback->isRealTGL && ret) {
		armAuxFlushBit(that);
		Gen11::armEngineProbe(that);
	}

	// V65: IMMEDIATELY after original start() returns, re-enable RCS0 interrupts.
	// Apple's init code may have overwritten our pre-start settings.
	// This is our earliest opportunity after ring activation.
	if (!NGreen::callback->isRealTGL) {
		uint32_t rcIntrPost = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		uint32_t newEn = rcIntrPost | wantBits;
		NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
		// Re-unmask tier-2
		uint32_t rcsMaskPost = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, rcsMaskPost & ~wantUnmasked);
		// Re-enable Master IRQ (may have been toggled during start)
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		// Clear any ERROR_GEN6 that fired during ring init
		uint32_t errPost = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPost) NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V65: Post-start interrupt fix: RC_INTR_EN 0x%x->0x%x, RCS_MASK 0x%x->0x%x, ERR=0x%x",
			rcIntrPost, NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
			rcsMaskPost, NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK), errPost);
		// V507: GFX_RUN_LIST_ENABLE must be set in the live RING_MODE register before any
		// ExecList submission — Apple's init does not set it on RPL-P (snapshots 0x0 into
		// the MI_LRI replay list, leaving ExecList mode off permanently).
		// RING_MODE uses the Intel masked-write convention: hi16=write-enable mask, lo16=values.
		// Writing 0x80008000 sets bit15 (GFX_RUN_LIST_ENABLE) without touching other bits.
		{
			NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
			IODelay(1000);
			uint64_t forceWakeNow = 0, forceWakeDeadline = 0;
			clock_interval_to_deadline(50, kMillisecondScale, &forceWakeDeadline);
			for (clock_get_uptime(&forceWakeNow); forceWakeNow < forceWakeDeadline; clock_get_uptime(&forceWakeNow))
				if (NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9) & 1) break;
		}
	}
	
	// ── V29: Post-start diagnostics ──
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 1);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 1);
	IODelay(1000);
	
	
	// V43: Scheduler type diagnostic — read field_1190 bits 23-25
	{
		uint64_t caps1190 = getMember<uint64_t>(that, 0x1190);
		uint32_t schedType = (caps1190 >> 23) & 7;
		uint32_t devType1120 = getMember<uint32_t>(that, 0x1120);
		SYSLOG("ngreen", "V43: field_1190=0x%llx schedType=%u (3=GuC,4=Sched4,5=Host) devType_1120=%u",
			   (unsigned long long)caps1190, schedType, devType1120);
		
		// Read encodeFailureStack entries (offsets 0x1c30-0x1c4c, count at 0x1c50)
		uint32_t failCount = getMember<uint32_t>(that, 0x1c50);
		SYSLOG("ngreen", "V43: encodeFailureStack count=%u", failCount);
		for (uint32_t i = 0; i < failCount && i < 8; i++) {
			uint32_t code = getMember<uint32_t>(that, 0x1c30 + i * 4);
			SYSLOG("ngreen", "V43:   failureStack[%u] = 0x%x", i, code);
		}
		
		// Read the scheduler pointer — check if scheduler was actually created
		// From start() disassembly, the scheduler is stored via virtual call;
		// try reading field at 0xe00 (common IOAccelF2 scheduler offset)
		uint64_t schedPtr = getMember<uint64_t>(that, 0xe00);
		SYSLOG("ngreen", "V43: scheduler ptr (0xe00) = 0x%llx %s",
			   (unsigned long long)schedPtr, schedPtr ? "EXISTS" : "NULL");
		
		// V44: Check if accelerator registered with framebuffer controller
		// Field 0x128c: set to 1 by registerWithFramebufferController() on success
		// Field 0xde8: callback handler pointer set during FB registration
		uint8_t fbRegistered = getMember<uint8_t>(that, 0x128c);
		uint64_t fbCallbackPtr = getMember<uint64_t>(that, 0xde8);
		SYSLOG("ngreen", "V44: fbRegistered(0x128c)=%u fbCallbackPtr(0xde8)=0x%llx",
			   fbRegistered, (unsigned long long)fbCallbackPtr);
		
		// V44: Dump key IORegistry properties of the accelerator service
		auto *accelSvc = static_cast<IOService *>(that);
		auto *metalProp = OSDynamicCast(OSString, accelSvc->getProperty("MetalPluginName"));
		auto *glProp    = OSDynamicCast(OSString, accelSvc->getProperty("IOGLBundleName"));
		auto *dvdProp   = OSDynamicCast(OSString, accelSvc->getProperty("IODVDBundleName"));
		auto *vaRIDProp = OSDynamicCast(OSNumber, accelSvc->getProperty("IOVARendererID"));
		const char *metalStr = (metalProp && metalProp->getCStringNoCopy()) ? metalProp->getCStringNoCopy() : "MISSING";
		const char *glStr = (glProp && glProp->getCStringNoCopy()) ? glProp->getCStringNoCopy() : "MISSING";
		const char *dvdStr = (dvdProp && dvdProp->getCStringNoCopy()) ? dvdProp->getCStringNoCopy() : "MISSING";
		SYSLOG("ngreen", "V44: MetalPlugin=%s GL=%s VARendID=0x%x",
			   metalStr,
			   glStr,
			   vaRIDProp ? (uint32_t)vaRIDProp->unsigned32BitValue() : 0);
		SYSLOG("ngreen", "V44: DVD=%s MTL_TGL=%d GL_TGL=%d VA_TGL=%d",
			   dvdStr,
			   !strcmp(metalStr, "AppleIntelTGLGraphicsMTLDriver"),
			   !strcmp(glStr, "AppleIntelTGLGraphicsGLDriver"),
			   !strcmp(dvdStr, "AppleIntelTGLGraphicsVADriver"));
	}
	
	// Ring buffer state
	uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
	uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
	uint32_t rcsCtl  = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
	uint32_t rcsStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
	uint32_t rcsHws  = NGreen::callback->readReg32(RING_HWS_PGA(RENDER_RING_BASE));
	SYSLOG("ngreen", "RCS0 HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		rcsHead, rcsTail, rcsCtl, rcsStart);
	SYSLOG("ngreen", "RCS0 HWS_PGA=0x%x HWSTAM=0x%x",
		rcsHws, NGreen::callback->readReg32(RING_HWSTAM(RENDER_RING_BASE)));
	
	SYSLOG("ngreen", "RCS0 ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 INSTDONE=0x%x INSTPM=0x%x DMA_FADD=0x%x:%08x",
		NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_INSTPM(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD_UDW(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_DMA_FADD(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS0 EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE)));
	
	// Per-engine error
	SYSLOG("ngreen", "RCS0 EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE)));
	
	// Global errors
	SYSLOG("ngreen", "ERROR_GEN6=0x%x RING_FAULT(global)=0x%x",
		NGreen::callback->readReg32(ERROR_GEN6),
		NGreen::callback->readReg32(GEN12_RING_FAULT_REG));
	SYSLOG("ngreen", "FAULT_TLB_DATA0=0x%x FAULT_TLB_DATA1=0x%x",
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0),
		NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1));
	SYSLOG("ngreen", "GT_INTR_DW0=0x%x DW1=0x%x",
		NGreen::callback->readReg32(GEN11_GT_INTR_DW0),
		NGreen::callback->readReg32(GEN11_GT_INTR_DW1));
	
	// ── V29 NEW: Context and engine config after start ──
	SYSLOG("ngreen", "RCS CTX_SIZE=0x%x CCID=0x%x CTX_CTRL=0x%x",
		NGreen::callback->readReg32(RING_CTX_SIZE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_CTX_CTRL(RENDER_RING_BASE)));
	SYSLOG("ngreen", "RCS MI_MODE=0x%x RING_MODE=0x%x",
		NGreen::callback->readReg32(RING_MI_MODE(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_MODE(RENDER_RING_BASE)));
	
	// ── V29 NEW: GGTT PTEs for HWS page and ring buffer ──
	// HWS_PGA is a GGTT address; read its PTE to check format
	if (rcsHws) {
		uint32_t hwsPage = rcsHws >> 12;
		SYSLOG("ngreen", "GGTT PTE for HWS (page 0x%x)=0x%08x:%08x", hwsPage,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage)));
		// Also check adjacent pages
		SYSLOG("ngreen", "GGTT PTE for HWS+1 (page 0x%x)=0x%08x:%08x", hwsPage+1,
			NGreen::callback->readReg32(GGTT_PTE_HI(hwsPage+1)),
			NGreen::callback->readReg32(GGTT_PTE_LO(hwsPage+1)));
	}
	if (rcsStart) {
		uint32_t ringPage = rcsStart >> 12;
		uint32_t rpteHi = NGreen::callback->readReg32(GGTT_PTE_HI(ringPage));
		uint32_t rpteLo = NGreen::callback->readReg32(GGTT_PTE_LO(ringPage));
		SYSLOG("ngreen", "GGTT PTE for RING (page 0x%x)=0x%08x:%08x", ringPage, rpteHi, rpteLo);

	}

	// CSB entries
	uint32_t csp = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	for (int i = 0; i < 6; i++) {
		SYSLOG("ngreen", "CSB[%d]=0x%x:%08x", i,
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i)),
			NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i)));
	}
	
	SYSLOG("ngreen", "ForceWake ACK: Render=0x%x Blitter=0x%x",
		NGreen::callback->readReg32(FORCEWAKE_ACK_RENDER_GEN9),
		NGreen::callback->readReg32(FORCEWAKE_ACK_BLITTER_GEN9));
	
	// BCS full state (now with Blitter ForceWake held!)
	uint32_t bcsHead = NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE));
	uint32_t bcsTail = NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE));
	uint32_t bcsCtl  = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
	uint32_t bcsStart = NGreen::callback->readReg32(RING_START(BLT_RING_BASE));
	SYSLOG("ngreen", "BCS HEAD=0x%x TAIL=0x%x CTL=0x%x START=0x%x",
		bcsHead, bcsTail, bcsCtl, bcsStart);
	SYSLOG("ngreen", "BCS HWS_PGA=0x%x MI_MODE=0x%x",
		NGreen::callback->readReg32(RING_HWS_PGA(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_MI_MODE(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS ACTHD=0x%x:%08x IPEHR=0x%x IPEIR=0x%x",
		NGreen::callback->readReg32(RING_ACTHD_UDW(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ACTHD(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEHR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_IPEIR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS INSTDONE=0x%x EIR=0x%x ESR=0x%x EMR=0x%x",
		NGreen::callback->readReg32(RING_INSTDONE(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE)));
	SYSLOG("ngreen", "BCS EXECLIST_STATUS=0x%x CTX_STATUS_PTR=0x%x",
		NGreen::callback->readReg32(RING_EXECLIST_STATUS(BLT_RING_BASE)),
		NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(BLT_RING_BASE)));
	
	// V36: Dump interrupt enable/mask state to verify GT interrupts are configured
	SYSLOG("ngreen", "IRQ: GFX_MSTR_IRQ=0x%x DISPLAY_INT_CTL=0x%x",
		NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ),
		NGreen::callback->readReg32(GEN11_DISPLAY_INT_CTL));
	SYSLOG("ngreen", "IRQ: RENDER_COPY_INTR_EN=0x%x VCS_VECS_INTR_EN=0x%x",
		NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE),
		NGreen::callback->readReg32(GEN11_VCS_VECS_INTR_ENABLE));
	SYSLOG("ngreen", "IRQ: RCS0_RSVD_MASK=0x%x BCS_RSVD_MASK=0x%x",
		NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK),
		NGreen::callback->readReg32(GEN11_BCS_RSVD_INTR_MASK));
	
	// V42: Log pending GT interrupts — do NOT clear (W1C would discard the tier-2 IIR
	// user-interrupt for the stamp(8,3) that V221 faked; the now-installed handler needs
	// to service it naturally so the cpu-side stamp counter is actually bumped).
	uint32_t gt0 = NGreen::callback->readReg32(0x190018);
	if (gt0) {
		SYSLOG("ngreen", "V42: GT_INTR_DW0 pending=0x%x (leaving for handler)", gt0);
	}
	
	// V42: Enumerate accelerator children to check if IOAccelDevice was created
	{
		auto *service = static_cast<IOService *>(that);
		OSIterator *iter = service->getClientIterator();
		int childCount = 0;
		if (iter) {
			OSObject *obj;
			while ((obj = iter->getNextObject())) {
				auto *child = OSDynamicCast(IOService, obj);
				if (child) {
					SYSLOG("ngreen", "V42: accel child[%d]: %s", childCount, child->getName());
					childCount++;
				}
			}
			iter->release();
		}
		SYSLOG("ngreen", "V42: accelerator has %d children after start()", childCount);
	}
	
	// ── V45: IORegistry state & service visibility diagnostics ──
	{
		auto *accelSvc = static_cast<IOService *>(that);
		
		// 1. IOService state bitmask — verify the service is registered, matched, published
		uint64_t svcState = accelSvc->getState();
		SYSLOG("ngreen", "V45: getState()=0x%llx (reg=%d match=%d pub=%d fmatch=%d inact=%d)",
			   (unsigned long long)svcState,
			   !!(svcState & 0x02),  // kIOServiceRegisteredState
			   !!(svcState & 0x04),  // kIOServiceMatchedState
			   !!(svcState & 0x08),  // kIOServiceFirstPublishState
			   !!(svcState & 0x10),  // kIOServiceFirstMatchState
			   !!(svcState & 0x01)); // kIOServiceInactiveState
		
		// 2. Provider chain — verify accelerator is attached to IGPU PCI device
		auto *provider = accelSvc->getProvider();
		if (provider) {
			SYSLOG("ngreen", "V45: provider name=%s class=%s state=0x%llx",
				   provider->getName(),
				   provider->getMetaClass()->getClassName(),
				   (unsigned long long)provider->getState());
			
			// 3. Enumerate ALL siblings on the same provider to find IOServiceCompatibility
			OSIterator *siblings = provider->getClientIterator();
			if (siblings) {
				OSObject *sib;
				int sibIdx = 0;
				while ((sib = siblings->getNextObject())) {
					auto *sibSvc = OSDynamicCast(IOService, sib);
					if (sibSvc) {
						SYSLOG("ngreen", "V45: provider child[%d]: name=%s class=%s state=0x%llx",
							   sibIdx, sibSvc->getName(),
							   sibSvc->getMetaClass()->getClassName(),
							   (unsigned long long)sibSvc->getState());
						sibIdx++;
					}
				}
				siblings->release();
			}
		} else {
			SYSLOG("ngreen", "V45: WARNING: accelerator has no provider!");
		}
		
		// 4. Check if key properties are present on the service itself
		bool hasMetal = (accelSvc->getProperty("MetalPluginName") != nullptr);
		bool hasGL    = (accelSvc->getProperty("IOGLBundleName") != nullptr);
		bool hasCFPI  = (accelSvc->getProperty("IOCFPlugInTypes") != nullptr);
		bool hasVARID = (accelSvc->getProperty("IOVARendererID") != nullptr);
		SYSLOG("ngreen", "V45: properties present: MetalPlugin=%d GL=%d CFPlugIn=%d VARend=%d",
			   hasMetal, hasGL, hasCFPI, hasVARID);

		// AppleGVA connects a VA plug-in only when the accelerator's IOVARendererID is on its allowlist (0x1080000 + {0, 1, 2, 4,
		// 8, 0x10, 0x20}, 0x1080080, ...); Apple's TGL personality publishes 0x1080040, which is not, so no hardware decoder is ever
		// created (VideoToolbox still advertises H.264: it only selects). Apple's own TGL VA driver reports itself as 0x1080080 (ICL's),
		// and that is the value published here.
		if (NGreen::callback != nullptr && NGreen::callback->isRaptorLakeHX()) {
			auto *rid = OSNumber::withNumber(static_cast<unsigned long long>(kVARendererIDTGLDriver), 32);
			if (rid != nullptr) {
				accelSvc->setProperty("IOVARendererID", rid);
				rid->release();
			}
			// VideoToolbox keeps a GVA decoder plug-in only when an entry under the accelerator carries the key its plist names:
			// IOGVAHEVCDecode for the HEVC decoder (Apple's ICL / KBL personalities publish "2"; the TGL one publishes none). The TGL VA
			// driver decodes HEVC and AVC only, so IOGVAXDecode (VP9) and IOGVAHEVCEncode stay absent.
			accelSvc->setProperty("IOGVAHEVCDecode", "2");
		}
			   
		//v44ScheduleBundleLog(that, 500);
		v44ScheduleBundleLog(that, 3000);

		// 5. If MetalPluginName is missing, set GPU properties directly on the service
		//    (normally they come from the matched personality, but if IOKit didn't merge them...)
		if (!hasMetal) {
			const bool useTglNames = callback && callback->tglHWLoaded;
			const char *mtlName = useTglNames ? "AppleIntelTGLGraphicsMTLDriver" : "AppleIntelICLGraphicsMTLDriver";
			const char *glName = useTglNames ? "AppleIntelTGLGraphicsGLDriver" : "AppleIntelICLGraphicsGLDriver";
			const char *vaName = useTglNames ? "AppleIntelTGLGraphicsVADriver" : "AppleIntelICLGraphicsVADriver";
			SYSLOG("ngreen", "V45: MetalPluginName MISSING — injecting %s GPU props directly on service",
			       useTglNames ? "TGL" : "ICL");
			accelSvc->setProperty("MetalPluginName", mtlName);
			accelSvc->setProperty("IOGLBundleName", glName);
			accelSvc->setProperty("IODVDBundleName", vaName);
			accelSvc->setProperty("IOGVACodec", "Gen10");
			accelSvc->setProperty("IOGVAScaler", "Gen10");
			accelSvc->setProperty("IOGVABGRAEnc", "Gen10");
			accelSvc->setProperty("IOSourceVersion", "0.0.0.0.0");
			auto *vaRendID = OSNumber::withNumber(static_cast<unsigned long long>(kVARendererIDTGLDriver), 32);
			if (vaRendID) { accelSvc->setProperty("IOVARendererID", vaRendID); vaRendID->release(); }
			// V77: DisplayPipeSupported=0 — disable GPU display pipe compositing.
			// WindowServer crash-loops (consecutiveCrashCount=11) in
			// CoreDisplay::DisplayPipe::RunFullDisplayPipe because GPU can't render
			// surfaces (RCS ring idle). Force CPU compositing via FB aperture.
			auto *dpCaps = OSDictionary::withCapacity(2);
			if (dpCaps) {
				auto *v1 = OSNumber::withNumber(0ULL, 32);
				auto *v2 = OSNumber::withNumber(0ULL, 32);
				if (v1) { dpCaps->setObject("DisplayPipeSupported", v1); v1->release(); }
				if (v2) { dpCaps->setObject("TransactionsSupported", v2); v2->release(); }
				accelSvc->setProperty("IOAccelDisplayPipeCapabilities", dpCaps);
				dpCaps->release();
				SYSLOG("ngreen", "V77: DisplayPipeSupported=0 set on accelerator");
			}
			auto *cfpi = OSDictionary::withCapacity(1);
			if (cfpi) {
				auto *pn = OSString::withCString("IOAccelerator2D.plugin");
				if (pn) { cfpi->setObject("ACCF0000-0000-0000-0000-000a2789904e", pn); pn->release(); }
				accelSvc->setProperty("IOCFPlugInTypes", cfpi);
				cfpi->release();
			}
		}
		
		// V78: For spoofed (non-TGL) hardware, always force DisplayPipeSupported=0.
		// AccessComplete (V187) is stubbed to return 0 on spoofed hw to prevent a
		// crash at +1928 (null MTL device hash-table lookup). That stub silences the
		// crash but prevents the "FB Ready" surface-flip signal from ever being sent.
		// With DisplayPipeSupported=1 (GPU display pipe), WindowServer waits for
		// that signal indefinitely → watchdogd kills it twice → KP.
		// With DisplayPipeSupported=0, WindowServer uses CPU compositing via the
		// linear framebuffer aperture — bypassing the GPU pipe entirely — so the
		// FB Ready wait is never entered and WindowServer initialises successfully.
		// Real TGL hardware uses the native value (never enters this branch).
		// Explicit -ngreendp1 boot-arg restores native behavior for experimentation.
		if (isDisplayPipeForceDisabled()) {
			auto *dpCaps = OSDictionary::withCapacity(2);
			if (dpCaps) {
				auto *dpSupp = OSNumber::withNumber(0ULL, 32);
				auto *trSupp = OSNumber::withNumber(0ULL, 32);
				if (dpSupp) { dpCaps->setObject("DisplayPipeSupported", dpSupp); dpSupp->release(); }
				if (trSupp) { dpCaps->setObject("TransactionsSupported", trSupp); trSupp->release(); }
				accelSvc->setProperty("IOAccelDisplayPipeCapabilities", dpCaps);
				dpCaps->release();
				SYSLOG("ngreen", "V78: DisplayPipeSupported=0 forced by boot-arg");
			}
		} else {
			SYSLOG("ngreen", "V78: keeping native DisplayPipeSupported (real TGL)");
		}
		
		// 6. Explicit registerService() — ensures the service is visible to IOKit matching
		//    even if IntelAccelerator::start() didn't call it or the internal call failed.
		SYSLOG("ngreen", "V45: calling explicit registerService(kIOServiceAsynchronous) on accelerator");
		accelSvc->registerService(kIOServiceAsynchronous);
		
		// 7. Post-registerService state
		svcState = accelSvc->getState();
		SYSLOG("ngreen", "V45: post-registerService state=0x%llx (reg=%d match=%d)",
			   (unsigned long long)svcState, !!(svcState & 0x02), !!(svcState & 0x04));
		
		if (!NGreen::callback->isRealTGL) {
			// 8. V59: Schedule delayed child checks to rescue stuck IGAccelDevice children.
			v45ScheduleDelayedCheck(that, 3000);
			v45ScheduleDelayedCheck(that, 10000);
			v45ScheduleDelayedCheck(that, 15000);
			v45ScheduleDelayedCheck(that, 20000);
			v45ScheduleDelayedCheck(that, 30000);
			v45ScheduleDelayedCheck(that, 60000);
			v45ScheduleDelayedCheck(that, 120000);
			SYSLOG("ngreen", "V59: scheduled delayed child checks at T+3,10,15,20,30,60,120s");
			if (__sync_bool_compare_and_swap(&gEmrEnforcerArmed, 0u, 1u)) {
				auto *emrSvc = static_cast<IOService *>(that);
				emrSvc->retain();
				gEmrCount = 0;
				auto emrTimer = thread_call_allocate(v71EmrEnforcer,
								 static_cast<thread_call_param_t>(emrSvc));
				if (emrTimer) {
					uint64_t deadline;
					clock_interval_to_deadline(50, kMillisecondScale, &deadline);
					thread_call_enter1_delayed(emrTimer, emrTimer, deadline);
					SYSLOG("ngreen", "V74: EMR enforcer armed — 50ms interval, PERMANENT");
				} else {
					emrSvc->release(); // alloc failed — drop our retain
					__sync_lock_test_and_set(&gEmrEnforcerArmed, 0u);
				}
			}

			// V60: GPU health monitor (contains V77 DisplayPipe terminator) — opt-in only.
			// Without this, IOAccelDisplayPipeUserClient2 is NOT killed after V59 opens it.
			// Retain the IOService so it stays alive while a timer is pending (UAF fix).
			if (isV60MonitorEnabled()) {
				auto *monSvc = static_cast<IOService *>(that);
				monSvc->retain();
				auto monTimer = thread_call_allocate(v60GpuHealthMonitor,
										 static_cast<thread_call_param_t>(monSvc));
				if (monTimer) {
					uint64_t deadline;
					clock_interval_to_deadline(2, kSecondScale, &deadline);
					thread_call_enter_delayed(monTimer, deadline);
					SYSLOG("ngreen", "V60: GPU health monitor armed — 2s interval, 60 iterations (120s)");
				} else {
					monSvc->release(); // alloc failed — drop our retain
				}
			} else {
				SYSLOG("ngreen", "V110: V60 health monitor disabled (use -ngreenexp/-ngreenv60 to enable, -ngreenv60off to force disable)");
			}
		}
	}
	
	// ── V47: Enable Master Interrupt (GFX_MSTR_IRQ bit 31) ──
	// Without this, the GPU can't deliver interrupts to the CPU, and the
	// scheduler never gets completion notifications. i915 does this in
	// gen11_master_intr_enable() — just write (1 << 31) to 0x190010.
	{
		uint32_t mstrIrq = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V47: GFX_MSTR_IRQ before enable = 0x%x (bit31=%d)", mstrIrq, !!(mstrIrq & GEN11_MASTER_IRQ));
		if (!(mstrIrq & GEN11_MASTER_IRQ)) {
			NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
			IODelay(100);
			uint32_t after = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
			SYSLOG("ngreen", "V47: GFX_MSTR_IRQ after enable  = 0x%x (bit31=%d)", after, !!(after & GEN11_MASTER_IRQ));
		} else {
			SYSLOG("ngreen", "V47: Master IRQ already enabled");
		}
	}
	
	// ── V47: Log initial RING_TIMESTAMP for RCS and BCS ──
	SYSLOG("ngreen", "V47: RCS RING_TIMESTAMP=0x%x BCS RING_TIMESTAMP=0x%x",
		NGreen::callback->readReg32(RENDER_RING_BASE + 0x358),
		NGreen::callback->readReg32(BLT_RING_BASE + 0x358));
	
	// ── V50: Log Metal-readiness summary ──
	SYSLOG("ngreen", "V50: Metal ON. ICL f2 mask-based (fallback). Use -ngreenNoMetal for display-only.");
	SYSLOG("ngreen", "V50: active GPU plugin track = %s (TGL and ICL both supported)",
	       (callback && callback->tglHWLoaded) ? "TGL" : "ICL");
	
	// ── V51: Clear GPU errors — give Metal a clean slate ──
	// ERROR_GEN6 is W1C (write-1-to-clear). Stale errors from init may cause
	// the Metal plugin to reject the device during MTLDevice creation.
	// Per-engine EIR/ESR registers are also W1C.
	{
		// 1. Clear global error register — try multiple times with W1C
		uint32_t errPre = NGreen::callback->readReg32(ERROR_GEN6);
		if (errPre) {
			// V55: Loop clear — stale errors may re-generate from in-flight ops
			for (int i = 0; i < 3; i++) {
				NGreen::callback->writeReg32(ERROR_GEN6, errPre);
				IODelay(200);
				errPre = NGreen::callback->readReg32(ERROR_GEN6);
				if (!errPre) break;
			}
			SYSLOG("ngreen", "V51: cleared ERROR_GEN6 → 0x%x (after loop)", errPre);
		} else {
			SYSLOG("ngreen", "V51: ERROR_GEN6 already clean");
		}
		
		// 2. Clear per-engine error interrupt registers
		uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
		uint32_t bcsEir = NGreen::callback->readReg32(RING_EIR(BLT_RING_BASE));
		uint32_t rcsEsr = NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE));
		uint32_t bcsEsr = NGreen::callback->readReg32(RING_ESR(BLT_RING_BASE));
		if (rcsEir) NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
		if (bcsEir) NGreen::callback->writeReg32(RING_EIR(BLT_RING_BASE), bcsEir);
		if (rcsEsr) NGreen::callback->writeReg32(RING_ESR(RENDER_RING_BASE), rcsEsr);
		if (bcsEsr) NGreen::callback->writeReg32(RING_ESR(BLT_RING_BASE), bcsEsr);
		SYSLOG("ngreen", "V51: cleared RCS EIR=0x%x ESR=0x%x, BCS EIR=0x%x ESR=0x%x",
			   rcsEir, rcsEsr, bcsEir, bcsEsr);
		
		// 3. Clear ring fault register
		uint32_t ringFault = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
		if (ringFault) {
			NGreen::callback->writeReg32(GEN12_RING_FAULT_REG, 0);
			SYSLOG("ngreen", "V51: cleared RING_FAULT=0x%x", ringFault);
		}
		
		// 4. Clear TLB fault data
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		if (tlb0 || tlb1) {
			NGreen::callback->writeReg32(GEN8_FAULT_TLB_DATA0, 0);
			NGreen::callback->writeReg32(GEN8_FAULT_TLB_DATA1, 0);
			SYSLOG("ngreen", "V51: cleared TLB_FAULT data0=0x%x data1=0x%x", tlb0, tlb1);
		}
		
		uint32_t gtIntr0 = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
		uint32_t gtIntr1 = NGreen::callback->readReg32(GEN11_GT_INTR_DW1);
	}
	
	// ── V51: BCS engine stop+clear ──
	// V52: Only on RPL — real TGL's BCS works natively.
	if (!NGreen::callback->isRealTGL) {
	// BCS ring is dead (CTL=0x0, EXECLIST_STATUS=0x1). The Host scheduler
	// attempted context submission but the engine never started. Clear the
	// engine state so it can be retried by the scheduler on first Metal blit.
	{
		uint32_t bcsCtlNow = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
		if (bcsCtlNow == 0) {
			SYSLOG("ngreen", "V51: BCS ring dead (CTL=0x0) — clearing engine state");
			
			// Request engine stop via masked write (set STOP_RING with mask bit)
			NGreen::callback->writeReg32(RING_MI_MODE(BLT_RING_BASE), 
				(uint32_t)STOP_RING | ((uint32_t)STOP_RING << 16));
			IODelay(500);
			
			// Clear STOP request
			NGreen::callback->writeReg32(RING_MI_MODE(BLT_RING_BASE),
				(uint32_t)STOP_RING << 16);
			IODelay(100);
			
			// Reset HEAD and TAIL
			NGreen::callback->writeReg32(RING_HEAD(BLT_RING_BASE), 0);
			NGreen::callback->writeReg32(RING_TAIL(BLT_RING_BASE), 0);
			
			uint32_t bcsCtlAfter = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
			uint32_t bcsExec = NGreen::callback->readReg32(RING_EXECLIST_STATUS(BLT_RING_BASE));
			SYSLOG("ngreen", "V51: BCS after clear: CTL=0x%x EXECLIST=0x%x", bcsCtlAfter, bcsExec);
		} else {
			SYSLOG("ngreen", "V51: BCS ring active (CTL=0x%x) — no reset needed", bcsCtlNow);
		}
	}
	} else {
		SYSLOG("ngreen", "V52: Real TGL — skipping BCS engine reset");
	}
	
	// ── V55: TLB invalidation + aggressive error clearing ──
	// V54 showed ERROR_GEN6=0x7b cannot be cleared via simple W1C because the GPU
	// continuously re-generates errors (TLB_MISS + PAGE_TABLE_ERROR). The root cause
	// is stale TLB entries from the context init path. Flush TLBs first, then re-clear.
	if (!NGreen::callback->isRealTGL) {
		// 1. Invalidate GPU TLBs via GEN12 MMIO (no GuC needed)
		//    GEN12_GUC_TLB_INV_CR (0xcee8): write to trigger TLB invalidation
		//    Each bit corresponds to an engine: bit0=RCS, bit1=BCS, etc.
		NGreen::callback->writeReg32(0xcee8, 0x1);   // Invalidate RCS TLBs
		IODelay(500);
		NGreen::callback->writeReg32(0xcee8, 0x4);   // Invalidate BCS TLBs
		IODelay(500);
		SYSLOG("ngreen", "V55: TLB invalidation requested (RCS+BCS)");
		
		// 2. Try clearing ERROR_GEN6 multiple times after TLB flush
		uint32_t errLoop = NGreen::callback->readReg32(ERROR_GEN6);
		for (int attempt = 0; attempt < 5 && errLoop; attempt++) {
			NGreen::callback->writeReg32(ERROR_GEN6, errLoop);  // W1C
			IODelay(200);
			errLoop = NGreen::callback->readReg32(ERROR_GEN6);
		}
		SYSLOG("ngreen", "V55: ERROR_GEN6 after TLB flush + 5x clear = 0x%x", errLoop);
		
		// 3. Read back TLB invalidation status
		uint32_t tlbInvDone = NGreen::callback->readReg32(0xceec);
		SYSLOG("ngreen", "V55: TLB_INV done status = 0x%x", tlbInvDone);
		
		// ── V57: ERROR_GEN6 R/W clear + error masking ──
		// V56 DISCOVERY: Writing 0xFFFFFFFF to ERROR_GEN6 SET reserved bits (0x7b→0x7ffff).
		//   This proves ERROR_GEN6 is R/W (not W1C) on RPL-P Gen12.5!
		// V56 DISCOVERY: GDRST killed the ring permanently (CTL→0x0, HEAD→0x0).
		// V56 DISCOVERY: RING_RESET_CTL always reads 0x0 — not functional on RPL-P.
		// V57 approach: Write 0x0 to ERROR_GEN6 (R/W clear), mask all error interrupts.
		//   NO engine resets, NO GDRST — those are destructive.
		{
			uint32_t preErr57 = NGreen::callback->readReg32(ERROR_GEN6);
			uint32_t preEMR = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
			uint32_t preEIR = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			uint32_t preESR = NGreen::callback->readReg32(RING_ESR(RENDER_RING_BASE));
			SYSLOG("ngreen", "V57: Pre-clear ERROR_GEN6=0x%x EMR=0x%x EIR=0x%x ESR=0x%x",
				   preErr57, preEMR, preEIR, preESR);
			
			// 1. Write 0x0 to ERROR_GEN6 — test R/W clear hypothesis
			//    If register is R/W (as V56 proved), writing 0 should clear all bits.
			//    If register is truly W1C, writing 0 has no effect (harmless test).
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			IODelay(100);
			uint32_t afterZero = NGreen::callback->readReg32(ERROR_GEN6);
			SYSLOG("ngreen", "V57: ERROR_GEN6 after 0x0 write = 0x%x (was 0x%x)", afterZero, preErr57);
			
			// 2. If 0x0 didn't work, try writing just the set bits (proper W1C)
			if (afterZero == preErr57 && preErr57 != 0) {
				NGreen::callback->writeReg32(ERROR_GEN6, preErr57);
				IODelay(100);
				uint32_t afterW1C = NGreen::callback->readReg32(ERROR_GEN6);
				SYSLOG("ngreen", "V57: ERROR_GEN6 after W1C(0x%x) = 0x%x", preErr57, afterW1C);
			}
			
			// 3. Mask ALL per-ring error interrupts via EMR
			//    EMR bits: 0 = error enabled, 1 = error masked
			//    V56 showed EMR=0xfffffffa (bits 0,2 unmasked). Mask everything.
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
			NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE), 0xFFFFFFFF);
			IODelay(100);
			uint32_t postEMR_rcs = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
			uint32_t postEMR_bcs = NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE));
			SYSLOG("ngreen", "V57: EMR after mask-all: RCS=0x%x BCS=0x%x", postEMR_rcs, postEMR_bcs);
			
			// 4. Clear per-ring EIR if set
			uint32_t rcsEir = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
			if (rcsEir) {
				NGreen::callback->writeReg32(RING_EIR(RENDER_RING_BASE), rcsEir);
				IODelay(50);
				uint32_t eir2 = NGreen::callback->readReg32(RING_EIR(RENDER_RING_BASE));
				SYSLOG("ngreen", "V57: RCS EIR cleared 0x%x → 0x%x", rcsEir, eir2);
			}
			
			// 5. Log ring state to confirm it's still alive (V56 GDRST killed it!)
			uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
			uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
			uint32_t rcsCtl = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
			SYSLOG("ngreen", "V57: Ring preserved — HEAD=0x%x TAIL=0x%x CTL=0x%x (alive=%d)",
				   rcsHead, rcsTail, rcsCtl, !!(rcsCtl & 0x1000));
			
			// 6. Error forensics: IPEIR/IPEHR show which instruction caused the error
			SYSLOG("ngreen", "V57: IPEIR=0x%x IPEHR=0x%x INSTDONE=0x%x",
				   NGreen::callback->readReg32(RING_IPEIR(RENDER_RING_BASE)),
				   NGreen::callback->readReg32(RING_IPEHR(RENDER_RING_BASE)),
				   NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE)));
			
			// 7. Final ERROR_GEN6 snapshot
			uint32_t finalErr57 = NGreen::callback->readReg32(ERROR_GEN6);
			SYSLOG("ngreen", "V57: Final ERROR_GEN6=0x%x (goal: 0x0)", finalErr57);
		}
	}
	
	SYSLOG("ngreen", "V51: GPU error clearing complete — Metal should see a clean GPU");
	
	// Release both ForceWake domains
	NGreen::callback->writeReg32(FORCEWAKE_RENDER_GEN9, (1 << 16) | 0);
	NGreen::callback->writeReg32(FORCEWAKE_BLITTER_GEN9, (1 << 16) | 0);
	
	return ret;
}

uint8_t Gen11::deviceStart(void *that)
{
	// V111: Force IGAccelDevice::deviceStart to succeed on spoofed RPL platform.
	// The original checks encodeFailureStack[1] which is set when BCS ring fails to
	// start. On RPL hardware with TGL driver, BCS init fails (RPL uses different
	// ring programming). We allow the original to run and then force success if it
	// returned false, preventing CoreDisplay from seeing a null accelerator device.
	auto ret = FunctionCast(deviceStart, callback->odeviceStart)(that);
	const bool isRealTGL = NGreen::callback && NGreen::callback->isRealTGL;
	if (!isRealTGL && !ret) {
		SYSLOG("ngreen", "V111: IGAccelDevice::deviceStart returned false on RPL — forcing true");
		return true;
	}
	DBGLOG("ngreen", "V111: IGAccelDevice::deviceStart returned %d", ret);
	return ret;
}

IOReturn Gen11::wrapPavpSessionCallback( void *intelAccelerator, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4, bool flag) {

	//void* pPavpContext = *getMember<void**>(intelAccelerator, 0x1278);
	//void* pStampTrackingStruct = *(void**)getMember<char*>(pPavpContext, 0xb8);
	
	if (sessionCommand == 4) {
		//return kIOReturnTimeout;
		return kIOReturnSuccess;
	}

	return FunctionCast(wrapPavpSessionCallback, callback->orgPavpSessionCallback)(intelAccelerator, sessionCommand, sessionAppId, a4, flag);
}

IOReturn Gen11::wrapPavpCommandCallbackObj(void *intelAccelerator, OSObject *owner, int32_t sessionCommand, uint32_t sessionAppId, uint32_t *a4) {
	if (sessionCommand == 4) {
		return kIOReturnSuccess;
	}

	return FunctionCast(wrapPavpCommandCallbackObj, callback->orgPavpCommandCallbackObj)(intelAccelerator, owner, sessionCommand, sessionAppId, a4);
}

// V429: a system sleep clears the accelerator's DRM-restore flag (+0x1C54) and every video-context submit then waits in
// waitForDRMSessionRestore (its one caller: IGAccelVideoContext::getTargetAndMethodForIndex, selector 2, before any lock)
// until PAVP command 6 sets it again. With displays the flag is cleared by systemWillSleep (IOAcceleratorFamily2's
// display_change_handler, framebuffer event will-sleep), headless by setPowerState(0); command 6 comes only from the
// framebuffer's ME client (restoreSession), which exists only when a service named IMEI does. Without it no PAVP session
// can exist and nothing ever sends the command, so the submit sleeps for ever. Complete the restore here, as the ME
// client would, through the accelerator's own PAVPCommandCallback; an IMEI service keeps Apple's behaviour.
void Gen11::wrapWaitForDRMSessionRestore(void *that) {
	if (getMember<uint8_t>(that, 0x1C54) == 0) {
		if (gV429Imei == 0) {
			OSDictionary *matching = IOService::nameMatching("IMEI");
			auto *it = matching ? IOService::getMatchingServices(matching) : nullptr;
			OSSafeReleaseNULL(matching);
			const bool imei = it != nullptr && it->getNextObject() != nullptr;
			OSSafeReleaseNULL(it);
			gV429Imei = imei ? 2 : 1;
			SYSLOG("ngreen", "V429: DRM restore pending at a submit; IMEI service %s", imei ? "present: Apple's ME client restores" : "absent");
		}
		if (gV429Imei == 1) {
			using PavpCommandCallback = IOReturn (*)(void *, int32_t, uint32_t, uint32_t *, bool);
			IOReturn r = reinterpret_cast<PavpCommandCallback>(gAccelPavpCommand)(that, 6, 0, nullptr, true);
			if (__atomic_fetch_add(&gV429Rows, 1, __ATOMIC_RELAXED) < 16)
				SYSLOG("ngreen", "V429: no ME client: PAVP command 6 -> 0x%x, restore %s", r,
					getMember<uint8_t>(that, 0x1C54) ? "done" : "STILL PENDING");
		}
	}
	FunctionCast(wrapWaitForDRMSessionRestore, gOrgWaitDrmRestore)(that);
}

__attribute__((noinline)) void Gen11::routeV429(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	SolveRequestPlus pavpSyms[] = {{"__ZN16IntelAccelerator19PAVPCommandCallbackE22PAVPSessionCommandID_tjPjb", gAccelPavpCommand}};
	RouteRequestPlus waitRoute[] = {{"__ZN16IntelAccelerator24waitForDRMSessionRestoreEv", wrapWaitForDRMSessionRestore, gOrgWaitDrmRestore}};
	if (!SolveRequestPlus::solveAll(patcher, index, pavpSyms, address, size)) {
		patcher.clearError();
		gAccelPavpCommand = 0;
		SYSLOG("ngreen", "V429: PAVPCommandCallback not found: waitForDRMSessionRestore not routed");
	} else if (!RouteRequestPlus::routeAll(patcher, index, waitRoute, address, size)) {
		patcher.clearError();
		SYSLOG("ngreen", "V429: waitForDRMSessionRestore not routed: after a sleep every video submit waits");
	} else {
		SYSLOG("ngreen", "V429: waitForDRMSessionRestore routed");
	}
}

void Gen11::getGPUInfoICL(void *that)
{
	FunctionCast(getGPUInfoICL, callback->ogetGPUInfoICL)(that);
	
	// --- GPU topology override for ICL HW binary ---
	// ICL object layout (verified from AppleIntelICLGraphics.sonoma.bin disassembly):
	//   0x1190 = NumSlices          0x12cc = NumSlices mirror
	//   0x1188 = NumSubSlices       0x12d0 = NumSubSlices mirror
	//   0x11a0 = MaxEUPerSubSlice
	//   0x1154 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1198 = L3BankCount
	//   0x1150 = GPU Sku
	// ICL counts traditional sub-slices (same as TGL binary).
	// Use ICL GT2 LP config (1×8×8 = 64 EU) to stay within ICL-valid topology.
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 8;   // ICL GT2 LP max (8 SS)
	unsigned int maxEUPerSubSlice = 8;
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices; // = 64
	
	getMember<UInt32>(that, 0x1190) = numSlices;
	getMember<UInt32>(that, 0x1188) = numSubSlices;
	getMember<UInt32>(that, 0x11a0) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1154) = totalEU;
	getMember<UInt32>(that, 0x12cc) = numSlices;     // NumSlices mirror
	getMember<UInt32>(that, 0x12d0) = numSubSlices;  // NumSubSlices mirror
	getMember<UInt32>(that, 0x1198) = 8;             // L3BankCount
	
	SYSLOG("ngreen", "getGPUInfoICL: overridden topology → slices=%u subslices=%u maxEU/SS=%u totalEU=%u L3Banks=8",
		   numSlices, numSubSlices, maxEUPerSubSlice, totalEU);
}

void Gen11::getGPUInfo(void *that)
{

#define RPM_CONFIG0				(0xd00)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(1 << GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	0
#define   GEN9_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT	3
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK	(0x7 << GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_SHIFT)
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ	0
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ	1
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_38_4_MHZ	2
#define   GEN11_RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_25_MHZ	3
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT	1
#define   GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK	(0x3 << GEN10_RPM_CONFIG0_CTC_SHIFT_PARAMETER_SHIFT)
	
	FunctionCast(getGPUInfo, callback->ogetGPUInfo)(that);

	// --- GPU topology override for the Raptor Lake-HX iGPU: 1 slice, 2 DSS, 16 EU/DSS, 32 EU total ---
	// TGL binary uses traditional sub-slices (SS), not dual sub-slices (DSS):
	//   2 DSS × 2 SS/DSS = 4 SS,  16 EU/DSS / 2 = 8 EU/SS, 4 × 8 = 32 EU.
	// Object layout (byte offsets from `this`, verified via disassembly):
	//   0x115c = NumSlices          0x0dd8 = NumSlices mirror
	//   0x1158 = NumSubSlices       0x0ddc = NumSubSlices mirror
	//   0x116c = MaxEUPerSubSlice
	//   0x1124 = ExecutionUnitCount (= MaxEUPerSubSlice × NumSubSlices)
	//   0x1150 = Frequency pair (low32=fMaxMHz, high32=fMinMHz)
	//   0x1164 = L3BankCount
	unsigned int numSlices        = 1;
	unsigned int numSubSlices     = 4;
	unsigned int maxEUPerSubSlice = 8;   // 16 EU/DSS ÷ 2 SS/DSS = 8 EU/SS
	unsigned int totalEU          = maxEUPerSubSlice * numSubSlices;
	
	getMember<UInt32>(that, 0x115c) = numSlices;
	getMember<UInt32>(that, 0x1158) = numSubSlices;
	getMember<UInt32>(that, 0x116c) = maxEUPerSubSlice;
	getMember<UInt32>(that, 0x1124) = totalEU;
	getMember<UInt32>(that, 0x0dd8) = numSlices;
	getMember<UInt32>(that, 0x0ddc) = numSubSlices;
	getMember<UInt32>(that, 0x1164) = 4;
	
	// Frequency: fMaxFrequencyInMhz=1000, fMinFrequencyInMhz=450 (TGL defaults)
	getMember<uint64_t>(that, 0x1150) = 0x1C2000003E8ULL;
	

}

long Gen11::wrapRenderExecute(uint32_t token, void *queue, void *resList, const void *cmd, void *desc, void *heaps,
                              void *accel, void *hwCtx, void *event)
{
	const long r = FunctionCast(wrapRenderExecute, callback->oRenderExecute)(token, queue, resList, cmd, desc, heaps,
	                                                                           accel, hwCtx, event);
	if (token == 0x10004u && r == 0 && cmd != nullptr && desc != nullptr && heaps != nullptr) fillClearColor(cmd, desc, heaps);
	return r;
}

void Gen11::wrapContextFree(void *that)
{
	auto *cb = NGreen::callback;
	ctxFreeLock();
	if (cb != nullptr && that != nullptr && that == cb->lastRCSCtx) cb->lastRCSCtx = nullptr;
	ctxFreeUnlock();
	forgetResetResolveContext(that);
	FunctionCast(wrapContextFree, callback->oContextFree)(that);
}

// V164: Hook populateResetRegisterList to clear PERCTX_PREEMPT_CTRL before snapshot.
//
// IntelAccelerator::startGraphicsEngine writes 0x40004000 to 0x20E0, then calls
// populateResetRegisterList which reads the live MMIO value via [rax+20E0h] and stores
// it into a per-context replay list (a batch of MI_LRI commands replayed before every
// context switch). If 0x4000 is snapshotted into that list, hardware perpetually
// replays it back, overriding any post-hoc MMIO clear (V162/V163 arrive too late).
//
// Fix: clear bit 14 BEFORE calling original so the snapshot captures 0x0000,
// making the replay batch write 0x0000 to 0x20E0 on every context switch instead.
void Gen11::populateResetRegisterList(void *that)
{
	FunctionCast(populateResetRegisterList, callback->opopulateResetRegisterList)(that);

	// V503: Dump the exact values Apple just baked into the context reset register list.
	// Register set decoded from Ghidra decompile of IntelAccelerator::populateResetRegisterList.
	static bool v503Logged = false;
	if (!v503Logged && !NGreen::callback->isRealTGL) {
		v503Logged = true;
		auto *cb = NGreen::callback;
		SYSLOG("ngreen", "V503: context register snapshot (19 regs):");
		SYSLOG("ngreen", "V503:  0x2080 HWS_PGA           = 0x%08x", cb->readReg32(0x2080));
		SYSLOG("ngreen", "V503:  0x2134 RING_BUFFER_UHPTR = 0x%08x", cb->readReg32(0x2134));
		SYSLOG("ngreen", "V503:  0x20c0 INSTPM            = 0x%08x", cb->readReg32(0x20c0));
		SYSLOG("ngreen", "V503:  0x7000 CACHE_MODE_0      = 0x%08x", cb->readReg32(0x7000));
		SYSLOG("ngreen", "V503:  0x7004 CACHE_MODE_1      = 0x%08x", cb->readReg32(0x7004));
		SYSLOG("ngreen", "V503:  0x209c MI_MODE           = 0x%08x", cb->readReg32(0x209c));
		SYSLOG("ngreen", "V503:  0x2090 3D_CHICKEN3       = 0x%08x", cb->readReg32(0x2090));
		SYSLOG("ngreen", "V503:  0x4090 ECOCHK            = 0x%08x", cb->readReg32(0x4090));
		SYSLOG("ngreen", "V503:  0x20a0 FF_THREAD_MODE    = 0x%08x", cb->readReg32(0x20a0));
		SYSLOG("ngreen", "V503:  0x20e4 FF_SLICE_CS_CHKN2 = 0x%08x", cb->readReg32(0x20e4));
		SYSLOG("ngreen", "V503:  0x9430 UCGCTL6           = 0x%08x", cb->readReg32(0x9430));
		SYSLOG("ngreen", "V503:  0x7010 CMN_SLICE_CHKN1   = 0x%08x", cb->readReg32(0x7010));
		SYSLOG("ngreen", "V503:  0x0d08 RCPCONFIG         = 0x%08x", cb->readReg32(0x0d08));
		SYSLOG("ngreen", "V503:  0xe194 HALF_SLICE_CHKN7  = 0x%08x", cb->readReg32(0xe194));
		SYSLOG("ngreen", "V503:  0xb004 GARBCNTLREG       = 0x%08x", cb->readReg32(0xb004));
		SYSLOG("ngreen", "V503:  0x20ec CS_DEBUG_MODE1    = 0x%08x", cb->readReg32(0x20ec));
		SYSLOG("ngreen", "V503:  0x2580 CS_CHICKEN1       = 0x%08x", cb->readReg32(0x2580));
		SYSLOG("ngreen", "V503:  0x20e0 FF_SLICE_CS_CHKN1 = 0x%08x", cb->readReg32(0x20e0));
		SYSLOG("ngreen", "V503:  0x229c RCS_GFX_MODE      = 0x%08x", cb->readReg32(0x229c));
	}
	// V504: patch reset register list before context image bakes
	// Walk IGVector: each entry = {uint32_t mmio_addr, uint32_t value, char name[0x1c]}
	if (!NGreen::callback->isRealTGL) {
		auto *accel = reinterpret_cast<AppleIntel::IntelAccelerator*>(that);
		uint64_t count = accel->fResetRegCount;
		uint8_t *data  = reinterpret_cast<uint8_t*>(accel->fResetRegData);
		static int v504CallCount = 0;
		++v504CallCount;
		bool v504Verbose = (v504CallCount <= 3);
		if (v504Verbose)
			SYSLOG("ngreen", "V504[%d]: IGVector count=%llu data=%p", v504CallCount,
				   (unsigned long long)count, data);
		for (uint64_t i = 0; i < count; i++) {
			uint32_t *entry = reinterpret_cast<uint32_t*>(data + i * 0x24);
			uint32_t  addr  = entry[0];
			// V504-TEST: 0x20ec CS_DEBUG_MODE1 patch commented out to test if clearing
			// Apple's 0x11 (bits 0+4) is needed or harmful on RPL.
			// if (addr == 0x20ec) {
			//     entry[1] = 0x00000000;
			//     if (v504Verbose) SYSLOG("ngreen", "V504[%d]: patched CS_DEBUG_MODE1 → 0", v504CallCount);
			// }
			if (addr == 0x2580) {
				entry[1] = 0x00000002;
				if (v504Verbose) SYSLOG("ngreen", "V504[%d]: patched CS_CHICKEN1 → 0x2", v504CallCount);
			}
			}
	}

	// V507 LRCA repair: re-copy the ring-LRI from the saved RCS context buffer to the
	// ExecList submission slot (GGTT[0x19]) after every populateResetRegisterList.
	// The display retry loop reuses the same context without calling withOptions again,
	// so the slot gets wiped by stopGraphicsEngine and never repaired by V508.
	auto *cb = NGreen::callback;
	if (!cb->isRealTGL) cb->setApertureIfNecessary();
	ctxFreeLock();
	void *ctx = cb->lastRCSCtx;
	if (!gCtxFreeRouted) ctx = nullptr;
	if (!cb->isRealTGL && ctx) {
		uint32_t lrcaGpuVa = getMember<uint32_t>(ctx, 0x89) & 0xFFFFF000;
		if (lrcaGpuVa) {
			uint32_t ctxPage1Idx = (lrcaGpuVa >> 12) + 1;
			cb->setApertureIfNecessary();
			if (cb->aperturePtr && cb->apertureLen >= 0x1000) {
				uint32_t slotLo = cb->readReg32(GGTT_PTE_LO(0x19));
				uint32_t slotHi = cb->readReg32(GGTT_PTE_HI(0x19));
				uint32_t ctxLo  = cb->readReg32(GGTT_PTE_LO(ctxPage1Idx));
				uint32_t ctxHi  = cb->readReg32(GGTT_PTE_HI(ctxPage1Idx));
				if ((slotLo & 1) && (ctxLo & 1)) {
					asm volatile("wbinvd" ::: "memory");
					volatile uint32_t *ap = cb->aperturePtr;
					uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
					uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));

					uint32_t buf[96];
					cb->writeReg32(GGTT_PTE_LO(0), ctxLo);
					cb->writeReg32(GGTT_PTE_HI(0), ctxHi);
					cb->writeReg32(0x101008, 0x1);
					for (int i = 0; i < 96; i++) buf[i] = ap[i];

					cb->writeReg32(GGTT_PTE_LO(0), slotLo);
					cb->writeReg32(GGTT_PTE_HI(0), slotHi);
					cb->writeReg32(0x101008, 0x1);

					if ((ap[1] & 0x1F801000) != 0x11001000 &&
					    (buf[1] & 0x1F801000) == 0x11001000) {
						for (int i = 0; i < 96; i++) ap[i] = buf[i];
						asm volatile("sfence" ::: "memory");
						SYSLOG("ngreen", "V507: LRCA re-repair slot←ctx DW1=%08x",
						       buf[1]);
					}

					cb->writeReg32(GGTT_PTE_LO(0), saveLo);
					cb->writeReg32(GGTT_PTE_HI(0), saveHi);
					cb->writeReg32(0x101008, 0x1);
				}
			}
		}
	}
	ctxFreeUnlock();
}

void *Gen11::createUserGPUTask(void *that)
{
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0xb8) : nullptr;

		void *taskCtx = getMember<void *>(task, 0xb8);
		if (taskCtx)
			return taskCtx;

		// Read task+0x298 directly — that's where getBlit3DContext(task,true) stores the
		// allocated context when Apple's driver naturally calls it. Calling getBlit3DContext
		// with true ourselves triggers initWithOptions too early (before GPU memory is ready)
		// causing a boot hang. Apple's call happens later; null here is safe — all callers
		// of ensureTaskContext that dereference +0xb8 already have null guards.
		void *ctx = getMember<void *>(task, 0x298);
		if (!ctx)
			return nullptr;

		auto *ctxSlot = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(task) + 0xb8);
		*ctxSlot = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v138Count = 0;
			if (v138Count < 32) {
				v138Count++;
				SYSLOG("ngreen", "V138[%d]: %s task=%p initialized ctx=%p", v138Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	auto *task = FunctionCast(createUserGPUTask, callback->ocreateUserGPUTask)(that);
	if (isExperimentalMonitorEnabled()) {
		static int v137CreateTaskCount = 0;
		if (v137CreateTaskCount < 24) {
			v137CreateTaskCount++;
			void *taskVtable = task ? getMember<void *>(task, 0x0) : nullptr;
			void *taskCtx = task ? getMember<void *>(task, 0xb8) : nullptr;
			void *kernelTask = getMember<void *>(that, 0x150);
			void *kernelTaskVtable = kernelTask ? getMember<void *>(kernelTask, 0x0) : nullptr;
			void *kernelTaskCtx = kernelTask ? getMember<void *>(kernelTask, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.createUserGPUTask[%d]: acc=%p task=%p vtbl=%p ctx=%p ktask=%p kvtbl=%p kctx=%p cached=%p",
				   v137CreateTaskCount, that, task, taskVtable, taskCtx,
				   kernelTask, kernelTaskVtable, kernelTaskCtx, callback->v132CachedTask);
		}
	}
	if (NGreen::callback->isRealTGL || task) {
		if (task)
			ensureTaskContext(task, "createUserGPUTask-user");
		return task;
	}

	void *kernelTask = getMember<void *>(that, 0x150);
	if (kernelTask) {
		callback->v132CachedTask = kernelTask;
		ensureTaskContext(kernelTask, "createUserGPUTask-kernel");
		SYSLOG("ngreen", "V132: createUserGPUTask returned null, using kernel task fallback=%p", kernelTask);
		return kernelTask;
	}

	if (callback->v132CachedTask) {
		ensureTaskContext(callback->v132CachedTask, "createUserGPUTask-cached");
		SYSLOG("ngreen", "V132: createUserGPUTask returned null, using cached task=%p", callback->v132CachedTask);
		return callback->v132CachedTask;
	}

	SYSLOG("ngreen", "V132: createUserGPUTask returned null, no fallback task available");
	return nullptr;
}

void *Gen11::igAccelTaskWithOptions(void *that)
{
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0xb8) : nullptr;

		void *taskCtx = getMember<void *>(task, 0xb8);
		if (taskCtx)
			return taskCtx;

		// Read task+0x298 directly — that's where getBlit3DContext(task,true) stores the
		// allocated context when Apple's driver naturally calls it. Calling getBlit3DContext
		// with true ourselves triggers initWithOptions too early (before GPU memory is ready)
		// causing a boot hang. Apple's call happens later; null here is safe — all callers
		// of ensureTaskContext that dereference +0xb8 already have null guards.
		void *ctx = getMember<void *>(task, 0x298);
		if (!ctx)
			return nullptr;

		auto *ctxSlot = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(task) + 0xb8);
		*ctxSlot = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v138Count = 0;
			if (v138Count < 32) {
				v138Count++;
				SYSLOG("ngreen", "V138[%d]: %s task=%p initialized ctx=%p", v138Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	auto *task = FunctionCast(igAccelTaskWithOptions, callback->oigAccelTaskWithOptions)(that);
	if (isExperimentalMonitorEnabled()) {
		static int v137WithOptionsCount = 0;
		if (v137WithOptionsCount < 24) {
			v137WithOptionsCount++;
			void *taskVtable = task ? getMember<void *>(task, 0x0) : nullptr;
			void *taskCtx = task ? getMember<void *>(task, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.withOptions[%d]: acc=%p task=%p vtbl=%p ctx=%p cached=%p",
				   v137WithOptionsCount, that, task, taskVtable, taskCtx, callback->v132CachedTask);
		}
	}
	if (NGreen::callback->isRealTGL)
		return task;

	if (task) {
		callback->v132CachedTask = task;
		ensureTaskContext(task, "withOptions");
		DBGLOG("ngreen", "V132: IGAccelTask::withOptions created task=%p", task);
		return task;
	}

	if (callback->v132CachedTask) {
		ensureTaskContext(callback->v132CachedTask, "withOptions-cached");
		SYSLOG("ngreen", "V132: IGAccelTask::withOptions returned null, reusing cached task=%p", callback->v132CachedTask);
		return callback->v132CachedTask;
	}

	return nullptr;
}

void * Gen11::getBlit3DContext(void *that,bool param_1)
{
	if (!that)
		return nullptr;

	// Preserve real TGL behavior.
	if (NGreen::callback->isRealTGL) {
		if (!callback->ogetBlit3DContext)
			return nullptr;
		return FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that, param_1);
	}

	// V165: On RPL, ctx+0xb8 is NULL — base-class initWithOptions doesn't set it for RPL.
	// All callers that dereference ctx+0xb8 are guarded with !isRealTGL early-returns
	// (initBlitUsage V121, markBlitUsage V122, beginCoalescedSegment V124, barrierSubmission V130).
	// Accept any non-null ctx on RPL.
	//
	// V194: Always call Apple's original first. After gpuRestart all context objects are
	// freed/reallocated; original returns the live context for the task. If it returns a
	// different non-null ctx than our cache, a restart happened — update the cache.
	// Fall back to cache only when original returns null (early-init window).
	//
	// Tier-1 interrupt: ring init disables RENDER_COPY_INTR_ENABLE. Ensure it's set before
	// each call so the GPU context-creation completion interrupt can fire.
	uint32_t rcIntrPre = 0;
	if (!NGreen::callback->isRealTGL) {
		rcIntrPre = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(false);
		if (!(rcIntrPre & wantBits)) {
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, rcIntrPre | wantBits);
			SYSLOG("ngreen", "V148: tier-1 was 0x%x — enabled (0x%x) before getBlit3DContext",
				   rcIntrPre, rcIntrPre | wantBits);
		}
	}

	if (callback->ogetBlit3DContext) {
		// V502: log param_1 and task+0x298 before calling original — tells us whether
		// getBlit3DContext will attempt context creation (param_1=true) or just read cache.
		void *cached298 = getMember<void *>(that, 0x298);
		static int v502Count = 0;
		if (v502Count < 12) {
			v502Count++;
			SYSLOG("ngreen", "V502[%d]: getBlit3DContext task=%p param_1=%d cached298=%p",
				   v502Count, that, (int)param_1, cached298);
		}
		void *ctx = FunctionCast(getBlit3DContext, callback->ogetBlit3DContext)(that, param_1);
		void *b8 = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
		if (ctx && (b8 || !NGreen::callback->isRealTGL)) {
			if (ctx != callback->v131CachedBlit3DCtx) {
				// Each task has its own Blit3D context, so the cache changes whenever two tasks blit (tens of times a second at
				// the desktop): log the first changes and then powers of two.
				static volatile uint32_t changes = 0;
				const uint32_t n = __sync_add_and_fetch(&changes, 1u);
				if (n <= 16 || (n & (n - 1u)) == 0)
					SYSLOG("ngreen", "V194: getBlit3DContext ctx changed %p -> %p (change #%u), updating cache",
						   callback->v131CachedBlit3DCtx, ctx, n);
				callback->v131CachedBlit3DCtx = ctx;
			} else if (isExperimentalMonitorEnabled()) {
				SYSLOG("ngreen", "V148: getBlit3DContext using cached ctx=%p ctx+0xb8=%p",
					   ctx, b8);
			}
			return ctx;
		}
		uint32_t errReg     = NGreen::callback->readReg32(ERROR_GEN6);
		uint32_t rcIntrPost = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		SYSLOG("ngreen", "V148: getBlit3DContext original returned invalid ctx=%p ctx+0xb8=%p "
			   "tier1_pre=0x%x tier1_post=0x%x ERROR_GEN6=0x%x param_1=%d cached298_pre=%p",
			   ctx, b8, rcIntrPre, rcIntrPost, errReg, (int)param_1, cached298);

	}

	// Apple's original returned null (early init) — fall back to previously cached ctx.
	if (callback->v131CachedBlit3DCtx) {
		if (isExperimentalMonitorEnabled())
			SYSLOG("ngreen", "V148: getBlit3DContext using cached ctx=%p (original returned null)",
				   callback->v131CachedBlit3DCtx);
		return callback->v131CachedBlit3DCtx;
	}

	// Fallbacks: depth/color resolve contexts share the same +0xb8 layout.
	// V165: On RPL accept fallback ctx regardless of +0xb8.
	void *depth = getDepthResolveContext(that, param_1);
	void *depthB8 = depth ? getMember<void *>(depth, 0xb8) : nullptr;
	if (depth && (depthB8 || !NGreen::callback->isRealTGL)) {
		SYSLOG("ngreen", "V148: getBlit3DContext using depth-resolve fallback ctx=%p ctx+0xb8=%p", depth, depthB8);
		return depth;
	}
	void *color = getColorResolveContext(that, param_1);
	void *colorB8 = color ? getMember<void *>(color, 0xb8) : nullptr;
	if (color && (colorB8 || !NGreen::callback->isRealTGL)) {
		SYSLOG("ngreen", "V148: getBlit3DContext using color-resolve fallback ctx=%p ctx+0xb8=%p", color, colorB8);
		return color;
	}

	SYSLOG("ngreen", "V148: getBlit3DContext all paths exhausted");
	return nullptr;
}

uint64_t Gen11::blit3d_init_ctx(void *that)
{
	// V128 rollback: allow original Blit3D context init on spoofed paths.
	void *pool = blit3dInitBefore(that);
	const uint64_t rc = FunctionCast(blit3d_init_ctx, callback->oblit3d_init_ctx)(that);
	blit3dInitAfter(pool);
	return rc;
}

void Gen11::blit3d_initialize_scratch_space(void *that)
{
	// Real TGL: always use Apple's original path unchanged.
	if (NGreen::callback->isRealTGL) {
		FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(that);
		return;
	}

	// V181: On spoofed paths, allow scratch init now that IOAF2 lock/unlock symbols
	// are resolved. The scratch buffer must be populated for RCS (routeSel=3) blits
	// to execute correctly; without it the GPU stalls on null shader code (Sig 813).
	// Guard: only proceed if IOAF2 symbols resolved — if they're missing, suppressing
	// is safer than a GPU hang.
	if (!callback->oIOAF2_lockForCPUAccess || !callback->oIOAF2_unlockForCPUAccess) {
		SYSLOG("ngreen", "V181: suppressing scratch init on RPL — IOAF2 symbols unresolved lock=%p unlock=%p",
			   reinterpret_cast<void *>(callback->oIOAF2_lockForCPUAccess),
			   reinterpret_cast<void *>(callback->oIOAF2_unlockForCPUAccess));
		return;
	}

	static bool v181InitLogged = false;
	if (!v181InitLogged) {
		v181InitLogged = true;
		SYSLOG("ngreen", "V181: blit3d_initialize_scratch_space running on RPL (IOAF2 lock=%p unlock=%p)",
			   reinterpret_cast<void *>(callback->oIOAF2_lockForCPUAccess),
			   reinterpret_cast<void *>(callback->oIOAF2_unlockForCPUAccess));
	}
	FunctionCast(blit3d_initialize_scratch_space, callback->oblit3d_initialize_scratch_space)(that);
}

void Gen11::IGHardwareBlit3DContextinitialize(void *that)
{
	// V69: Diagnostic hook — original crashes at +0x4c writing 0x44 bytes to buffer+0xD000.
	// That page is unmapped (CPU page fault, type 14). We replace the entire function body
	// to prevent the panic and log the buffer state for root-cause analysis.
	// V73: Fixed double-dereference bug — *getMember<T*> reads a pointer then follows it.
	//      Use getMember<T> to write to object fields directly.
	//      Added NULL guard for `that` (crash seen with RDI=0x0).

	if (!that) {
		SYSLOG("ngreen", "V73: IGHardwareBlit3DContext::initialize called with NULL this!");
		return;
	}

	static int v69CallCount = 0;
	v69CallCount++;
	const bool v69Verbose = v69CallCount <= 6;

	// Preserve real TGL behavior: always use Apple's original initializer.
	if (NGreen::callback->isRealTGL) {
		FunctionCast(IGHardwareBlit3DContextinitialize, callback->oIGHardwareBlit3DContextinitialize)(that);
		return;
	}

	if (v69Verbose)
		SYSLOG("ngreen", "V69: IGHardwareBlit3DContext::initialize[%d](%p)", v69CallCount, that);

	// Dump context object internals — look for the GPU buffer mapping info
	void *mappedBufPtr = getMember<void *>(that, 0xd8);
	if (v69Verbose)
		SYSLOG("ngreen", "V69: ctx->0xd8(IGMappedBuffer)=%p", mappedBufPtr);

	// Probe IGMappedBuffer internals: vtable, size, IOMemoryDescriptor*, base VA, etc.
	if (mappedBufPtr && v69Verbose) {
		for (int i = 0; i < 12; i++) {
			uint64_t val = getMember<uint64_t>(mappedBufPtr, i * 8);
			SYSLOG("ngreen", "V69: mappedBuf[0x%02x]=0x%016llx", i * 8, (unsigned long long)val);
		}
	}

	// V106: On spoofed paths, default to skip Apple's original init.
	// The original still faults at +0x4c on some boots (page fault in SecurityAgent path).
	// In full-MTL mode, keep skip-by-default for stability; allow original only via opt-in arg.
	// Use -ngreenV69AllowOriginal to test original init, and -ngreenV69SkipOriginal to hard-disable it.
	const bool allowOriginal = checkKernelArgument("-ngreenV69AllowOriginal");
	const bool forceFullMTL = shouldForceFullMetalPath();
	const bool skipOriginal = checkKernelArgument("-ngreenV69SkipOriginal");
	uint64_t gpuBufBase = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x18) : 0;
	uint64_t gpuBufSize = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x20) : 0;
	// Calling the original Blit3D initialize on spoofed RPL/ADL is still unsafe on Sonoma:
	// crashes observed at initialize+0x4c with a write fault to the mapped scratch page.
	// Keep original path blocked by default even when -ngreenV69AllowOriginal is set.
	// Only allow with explicit unsafe override for debugging.
	// GPU VA is at mappedBuf[0x38], size at [0x20]. [0x18] is the task pointer, not GPU VA.
	uint64_t gpuVA   = mappedBufPtr ? getMember<uint64_t>(mappedBufPtr, 0x38) : 0;
	bool safeForOriginal = mappedBufPtr && gpuVA != 0 && gpuBufSize >= 0xD040;
	const bool forceUnsafeOriginal = checkKernelArgument("-ngreenV69ForceOriginalUnsafe");
	const bool shouldTryOriginal = allowOriginal && !skipOriginal && safeForOriginal &&
		(NGreen::callback->isRealTGL || forceUnsafeOriginal);
	if (shouldTryOriginal) {
		SYSLOG("ngreen", "V111B: calling original Blit3D init (opt-in, fullMTL=%d allow=%d skip=%d base=0x%llx size=0x%llx)",
			   forceFullMTL, allowOriginal, skipOriginal,
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
		FunctionCast(IGHardwareBlit3DContextinitialize, callback->oIGHardwareBlit3DContextinitialize)(that);
		SYSLOG("ngreen", "V111B: original Blit3D init completed");
		return;
	}
	if (!NGreen::callback->isRealTGL && allowOriginal && !forceUnsafeOriginal) {
		SYSLOG("ngreen", "V111B: -ngreenV69AllowOriginal blocked on spoofed RPL/ADL (use -ngreenV69ForceOriginalUnsafe only for crash debugging)");
	}
	if (allowOriginal && !safeForOriginal) {
		SYSLOG("ngreen", "V106: -ngreenV69AllowOriginal requested but rejected (base=0x%llx size=0x%llx)",
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
	} else if (!NGreen::callback->isRealTGL && forceFullMTL && skipOriginal) {
		SYSLOG("ngreen", "V111B: full-MTL active and original Blit3D init disabled by -ngreenV69SkipOriginal");
	} else if (!NGreen::callback->isRealTGL && forceFullMTL && !allowOriginal) {
		SYSLOG("ngreen", "V111B: full-MTL active; original Blit3D init remains disabled by default (use -ngreenV69AllowOriginal to test)");
	} else if (v69Verbose || v69CallCount == 16 || v69CallCount == 64 || v69CallCount == 256) {
		SYSLOG("ngreen", "V106: skip original Blit3D init on non-real TGL (base=0x%llx size=0x%llx)",
			   (unsigned long long)gpuBufBase, (unsigned long long)gpuBufSize);
	}

	// Dump context fields 0xb0-0x120 (includes buffer pointers, sizes, GPU addresses)
	for (int i = 0; v69Verbose && i < 16; i++) {
		uint64_t val = getMember<uint64_t>(that, 0xb0 + i * 8);
		if (val != 0) {
			SYSLOG("ngreen", "V69: ctx[0x%03x]=0x%016llx", 0xb0 + i * 8, (unsigned long long)val);
		}
	}

	// V148: Replicate initialize() ourselves, skipping only blit3d_initialize_scratch_space.
	//
	// initWithOptions IDA (0x7CEBC) clarifies the full field layout when initialize() runs:
	//   ctx+0xB8 — set by BASE CLASS IGHardwareContext::initWithOptions before we are called
	//   ctx+0xD8 — IGSharedMappedBuffer* scratch buf, allocated by initWithOptions from params+0x10
	//   ctx+0xE0 — optional extra IGMappedBuffer, only if params+0x18 != 0 (Blit3D: not used)
	//   initialize() is called LAST (vtable[0x130]) after all fields are set.
	//
	// Original initialize() body (from disassembly at 0x7d8b8):
	//   1. Zero ctx+0xe8, 0xf0, 0xf8, 0x100, 0x108, 0x110
	//   2. ctx+0xd8 -> getMemory() -> blit3d_initialize_scratch_space()
	//      ← CRASH on RPL: memcpy writes static GPU blit shader kernels to write-protected page
	//      (allocation succeeds, GPU VA valid, but CPU-side lockForCPUAccess ptr is read-only)
	//   3. tail-call blit3d_init_ctx(ctx)
	//      ← reads ctx+0xD8 for getGPUVirtualAddress(), reads ctx+0xE0 for getBufferPtrNoInc()
	//      ← writes GPU 3D state init commands into the command buffer (ctx+0xE0 pool slot)
	//
	// We zero the fields (step 1), skip scratch-space init (step 2), then call
	// oblit3d_init_ctx directly (step 3). ctx+0xD8 must be non-null for blit3d_init_ctx
	// to safely call getGPUVirtualAddress() — guard before calling.
	auto *base = reinterpret_cast<uint8_t *>(that);
	*reinterpret_cast<uint64_t *>(base + 0xe8)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0xf0)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0xf8)  = 0;
	*reinterpret_cast<uint64_t *>(base + 0x100) = 0;
	*reinterpret_cast<uint64_t *>(base + 0x108) = 0;
	*reinterpret_cast<uint32_t *>(base + 0x110) = 0;

	if (!callback->oblit3d_init_ctx) {
		SYSLOG("ngreen", "V148: oblit3d_init_ctx is null, cannot init GPU command buffer");
		return;
	}
	// ctx+0xD8 must be valid before calling blit3d_init_ctx (it reads it for GPU VA).
	// In practice it is always set by initWithOptions from params+0x10; guard for safety.
	if (!mappedBufPtr) {
		SYSLOG("ngreen", "V148: ctx+0xD8 is null (scratch buf not allocated), skipping blit3d_init_ctx");
		return;
	}

	copyScratchUngated(mappedBufPtr, callback->oblit3d_initialize_scratch_space, callback->oIGMappedBuffergetMemory);

	void *pool = blit3dInitBefore(that);
	uint64_t rc = FunctionCast(blit3d_init_ctx, callback->oblit3d_init_ctx)(that);
	blit3dInitAfter(pool);
	if (v69Verbose)
		SYSLOG("ngreen", "V148: blit3d_init_ctx returned %llu, ctx+0xb8=0x%llx ctx+0xd8=%p ctx+0xe0=%p",
			   (unsigned long long)rc,
			   (unsigned long long)getMember<uint64_t>(that, 0xb8),
			   mappedBufPtr,
			   getMember<void *>(that, 0xe0));

}

// V163: Hook startGraphicsEngine to clear PERCTX_PREEMPT_CTRL before first execlist context snapshot.
//
// Root cause: IntelAccelerator::startGraphicsEngine writes 0x40004000 to MMIO 0x20E0
// (FF_SLICE_CS_CHICKEN1). That masked write enables bit 14 (PERCTX_PREEMPT_CTRL).
// When the GPU loads its first execlist context it snapshots all GPU registers into
// the context image DMA buffer in GGTT memory. From that point every context-switch
// restore re-writes 0x4000 back to 0x20E0 from the DMA buffer — overriding any MMIO
// clear we do after the fact.
//
// Fix: call the original (must keep its side-effects), then immediately clear bit 14
// using a masked write (upper 16 = mask, lower 16 = 0). This fires before any context
// is ever submitted, so the DMA buffer snapshot captures 0x0000 instead of 0x4000.
unsigned long Gen11::startGraphicsEngine(void *that)
{
	static int startCount = 0;

	if (!NGreen::callback->isRealTGL) {
		startCount++;
		applyPreStartEngineWorkarounds(startCount);
	}

	unsigned long ret = FunctionCast(startGraphicsEngine, callback->ostartGraphicsEngine)(that);

	return ret;
}

// Minimal pre-start safety: clear ERROR_GEN6 + mask EMR only.
// GT chicken/clock-gate WAs must NOT run before Apple's startGraphicsEngine — they
// interfere with ring initialization and leave CTL=0 (ring never enabled).
// GT WAs are applied pre-stop via applyPreStopEngineWorkarounds; they persist in
// hardware state and the context image (V504) across the stop→start cycle.
void Gen11::applyPreStartEngineWorkarounds(int callCount)
{
	uint32_t preErr = NGreen::callback->readReg32(ERROR_GEN6);
	if (preErr) {
		NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		SYSLOG("ngreen", "V71S[%d]: pre-start ERR=0x%x cleared", callCount, preErr);
	}
	NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
	NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
}

// Full GT workarounds + pre-engine error clearing + BCS drain.
// Called before stopGraphicsEngine only — these WAs are safe to apply to a running
// ring and need to be in place before the context image is captured on stop.
void Gen11::applyPreStopEngineWorkarounds(int callCount)
{
	// ── GT workarounds (gen12_gt_workarounds_init / rcs_engine_wa_init) ──
	// Applied on every reset — same set as Linux i915 for TGL/ADL-P.
	applyRenderWa(NGreen::callback, true);
	/* Wa_14011059788:tgl,rkl,adl-s,dg1,adl-p */
	NGreen::callback->wa_mcr_write_or(GEN10_DFR_RATIO_EN_AND_CHICKEN, DFR_DISABLE);
	/* Wa_14015795083 */
	NGreen::callback->wa_add(GEN7_MISCCPCTL, GEN12_DOP_CLOCK_GATE_RENDER_ENABLE, 0, 0, false);
	/* Wa_1409142259:tgl,dg1,adl-p */
	NGreen::callback->wa_masked_en(GEN11_COMMON_SLICE_CHICKEN3, GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
	/* rcs_engine_wa_init */
	NGreen::callback->wa_masked_en(GEN7_FF_SLICE_CS_CHICKEN1, GEN9_FFSC_PERCTX_PREEMPT_CTRL);

	// V71 PRE: clear ERROR_GEN6 and mask EMR
	{
		uint32_t preErr = NGreen::callback->readReg32(ERROR_GEN6);
		if (preErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			SYSLOG("ngreen", "V71[%d]: pre-engine ERR=0x%x cleared", callCount, preErr);
		}
		NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
	}

	// V152: drain and disable BCS ring so Apple sees a clean BCS state
	{
		uint32_t bcsTail = NGreen::callback->readReg32(RING_TAIL(BLT_RING_BASE));
		uint32_t bcsHead = NGreen::callback->readReg32(RING_HEAD(BLT_RING_BASE));
		uint32_t bcsCtl  = NGreen::callback->readReg32(RING_CTL(BLT_RING_BASE));
		if (bcsCtl != 0 || bcsHead != bcsTail) {
			if (bcsHead != bcsTail) {
				NGreen::callback->writeReg32(RING_TAIL(BLT_RING_BASE), bcsHead);
				SYSLOG("ngreen", "V152[%d]: BCS drain TAIL 0x%x→0x%x HEAD=0x%x CTL=0x%x",
					   callCount, bcsTail, bcsHead, bcsHead, bcsCtl);
			}
			NGreen::callback->writeReg32(RING_CTL(BLT_RING_BASE), 0x0);
			SYSLOG("ngreen", "V152[%d]: BCS disabled (CTL was 0x%x)", callCount, bcsCtl);
		}
	}
}

// stopGraphicsEngine is the real GPU reset entry point on this binary (resetGraphicsEngine
// symbol is absent). Stop-specific: ring snapshot, V155 save/restore, V162 PERCTX clear,
// V71 POST CSB drain. GT workarounds are in applyPreStopEngineWorkarounds() above.
bool Gen11::stopGraphicsEngine(void *that)
{
	static int   v63ResetCount            = 0;
	static uint32_t v155SavedRcsCtl       = 0;
	static int   v505Count                = 0;

	if (!NGreen::callback->isRealTGL) {
		v63ResetCount++;

		applyPreStopEngineWorkarounds(v63ResetCount);

		// V155: save RCS CTL while ring is still intact
		{
			uint32_t ctlNow = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
			if (ctlNow != 0)
				v155SavedRcsCtl = ctlNow | 1;
		}

		// V53/V161 PRE: snapshot ring and EU state before stop
		uint32_t rcsHead  = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
		uint32_t rcsTail  = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
		uint32_t rcsStart = NGreen::callback->readReg32(RING_START(RENDER_RING_BASE));
		SYSLOG("ngreen", "V53[%d] stop PRE: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x START=0x%x EXEC=0x%x",
			v63ResetCount,
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			rcsHead, rcsTail, rcsStart,
			NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)));
		SYSLOG("ngreen", "V161[%d] stop PRE: INSTDONE_1=0x%08x SLICE_DONE=0x%08x CS_CHKN1=0x%08x FF_CHKN1=0x%08x",
			v63ResetCount,
			NGreen::callback->readReg32(0x206c),
			NGreen::callback->readReg32(0x2090),
			NGreen::callback->readReg32(GEN8_CS_CHICKEN1),
			NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1));

		// V505: dump ring preamble contents — ring still running at this point
		if (v505Count < 3 && rcsStart) {
			uint32_t ringPage = rcsStart >> 12;
			uint32_t rpteHi   = NGreen::callback->readReg32(GGTT_PTE_HI(ringPage));
			uint32_t rpteLo   = NGreen::callback->readReg32(GGTT_PTE_LO(ringPage));
			if (rpteLo & 1) {
				v505Count++;
				uint64_t physBase = ((uint64_t)rpteHi << 32) | (rpteLo & 0xFFFFF000u);
				SYSLOG("ngreen", "V505[%d]: ring phys=0x%llx HEAD=0x%x TAIL=0x%x — 32 dwords:",
					   v505Count, (unsigned long long)physBase, rcsHead, rcsTail);
				const bool stolen = isStolen(physBase);
				for (int i = 0; i < 32 && !stolen; i++) {
					uint32_t dw = IOMappedRead32(physBase + (uint64_t)(i * 4));
					SYSLOG("ngreen", "V505[%d]:  [%02d] +0x%02x = 0x%08x", v505Count, i, i * 4, dw);
				}
			} else {
				SYSLOG("ngreen", "V505: PTE invalid rpteHi=0x%x rpteLo=0x%x", rpteHi, rpteLo);
			}
		}

	}

	bool ret = FunctionCast(stopGraphicsEngine, callback->ostopGraphicsEngine)(that);

	if (!NGreen::callback->isRealTGL) {
		// V53/V161 POST: snapshot after stop
		SYSLOG("ngreen", "V53[%d] stop POST: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x EXEC=0x%x",
			v63ResetCount,
			NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
			NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)));

		// V155: restore RCS ring enable so Apple's startGraphicsEngine can take over
		if (v155SavedRcsCtl != 0)
			NGreen::callback->writeReg32(RING_CTL(RENDER_RING_BASE), v155SavedRcsCtl | 1);

		// V71 POST: clear any errors generated by stop + drain CSB
		{
			uint32_t postErr = NGreen::callback->readReg32(ERROR_GEN6);
			if (postErr) {
				NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
				SYSLOG("ngreen", "V71[%d]: post-stop ERR=0x%x cleared", v63ResetCount, postErr);
			}
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
			NGreen::callback->writeReg32(RING_EMR(BLT_RING_BASE),    0xFFFFFFFF);
			// Drain CSB: advance read pointer to write pointer
			uint32_t csbReg   = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
			uint32_t csbWrite = csbReg & GEN11_CSB_WRITE_PTR_MASK;
			uint32_t csbRead  = (csbReg & GEN11_CSB_READ_PTR_MASK) >> 8;
			if (csbRead != csbWrite)
				NGreen::callback->writeReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE),
											 csbConsumeWritePtr(csbReg));
			SYSLOG("ngreen", "V160[%d]: CSB rp %d→%d EXEC=0x%x FF_CS_CHKN1=0x%08x",
				v63ResetCount, csbRead, csbWrite,
				NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE)),
				NGreen::callback->readReg32(GEN7_FF_SLICE_CS_CHICKEN1));
		}
	}
	return ret;
}

void *  Gen11::IGHardwareBlit3DContextoperatornew(unsigned long size)
{
	if (!callback->oIGHardwareBlit3DContextoperatornew) {
		// V143: symbol was never registered — this is the root-cause of context alloc always
		// returning null, leading to the perpetual NULL-task submitBlit flood and BCS stall.
		if (isExperimentalMonitorEnabled()) {
			static bool v143NewNullLogged = false;
			if (!v143NewNullLogged) {
				v143NewNullLogged = true;
				SYSLOG("ngreen", "V143: oIGHardwareBlit3DContextoperatornew is null (RouteRequest missing?)");
			}
		}
		return nullptr;
	}
	auto ret = FunctionCast(IGHardwareBlit3DContextoperatornew, callback->oIGHardwareBlit3DContextoperatornew)(size);
	return ret;
}

// V509: Hook IGHardwareContext::initWithOptions (base class) — CPU-side LRCA page1 repair.
// Apple's initWithOptions calls restoreFromSafeImage() before copying g_cInitGfxRingContextRCS.
// On RPL that call returns true (skips the memcpy), leaving MI_LRI headers at DW1/DW8 as
// 0x00ffffff GEM-fill.  Hardware sees 0x00ffffff=MI_NOOP at DW1, skips register restore entirely,
// ring state is undefined, context posts IDLE immediately → startGraphicsEngine ret=3022544385.
//
// Fix: after original returns, if DW1 is still 0x00ffffff (only for RCS, this[0x6c]==0), write
// the minimal Gen12 ring context MI_LRI block via aperture (BAR2 remap of GGTT page1 PTE).
// LRCA GPU VA is in this+0x89 (set by initWithOptions on success path per Ghidra decompile).
// RING_START and RING_CTL are read from live MMIO so they match what Apple's ring-setup wrote.
uint64_t Gen11::IGHardwareContextinitWithOptions(void *that, void *task, const void *params, uint8_t arg)
{
	uint64_t ret = FunctionCast(IGHardwareContextinitWithOptions,
	                             callback->oIGHardwareContextinitWithOptions)(that, task, params, arg);
	auto *cb = NGreen::callback;
	if (cb->isRealTGL || !ret) return ret;

	uint8_t engType = getMember<uint8_t>(that, 0x6c);
	if (engType == 0) applyPoshContextControl(cb, that);
	if (engType == 0) applyContextWa(cb, that);
	if (engType != 0) return ret;  // only repair RCS (case 0)

	// LRCA GPU VA is stored at this+0x89 (4-byte field, written by initWithOptions success path).
	// Bits [31:12] = 4K-aligned GPU physical address of LRCA page0.  Page1 = page0 + 0x1000.
	uint32_t lrcaGpuVa = getMember<uint32_t>(that, 0x89) & 0xFFFFF000;
	if (!lrcaGpuVa) {
		SYSLOG("ngreen", "V509: LRCA GPU VA zero, skipping repair");
		return ret;
	}
	uint32_t page1Idx = (lrcaGpuVa >> 12) + 1;

	// Flush CPU caches so aperture (UC) sees true DRAM content written by initWithOptions.
	asm volatile("wbinvd" ::: "memory");
	cb->setApertureIfNecessary();
	uint32_t p1Lo = cb->readReg32(GGTT_PTE_LO(page1Idx));
	uint32_t p1Hi = cb->readReg32(GGTT_PTE_HI(page1Idx));
	if (!(p1Lo & 1) || !cb->aperturePtr || cb->apertureLen < 0x1000) {
		SYSLOG("ngreen", "V509: LRCA page1 PTE not present (idx=%u lo=%08x), skipping", page1Idx, p1Lo);
		return ret;
	}

	volatile uint32_t *ap = cb->aperturePtr;
	uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
	uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));
	cb->writeReg32(GGTT_PTE_LO(0), p1Lo);
	cb->writeReg32(GGTT_PTE_HI(0), p1Hi);
	cb->writeReg32(0x101008, 0x1);

	if (ap[1] != 0x00ffffff) {
		// Already has a valid LRI header — restoreFromSafeImage gave us a good image.
		SYSLOG("ngreen", "V509: DW1=%08x (valid), no repair needed", (uint32_t)ap[1]);
	} else {
		// Read ring setup from live MMIO — Apple's ring-setup code runs before context creation.
		uint32_t rcsStart = cb->readReg32(0x2038);   // RCS RING_START
		uint32_t rcsCtl   = cb->readReg32(0x2040);   // RCS RING_CTL (bit0=0 when stopped)
		if (!rcsStart) rcsStart = 0x402eb000;         // fallback from HANGCHECK

		SYSLOG("ngreen", "V509: repairing LRCA page1 lrcaGpuVa=%08x RING_START=%08x RING_CTL=%08x",
		       lrcaGpuVa, rcsStart, rcsCtl);

		// Zero DW0-DW79 (replaces 0x00ffffff GEM-fill with MI_NOOPs).
		for (int i = 0; i < 80; i++) ap[i] = 0;

		// LRI block 0 (DW0-DW7): CTX_CTRL | RING_HEAD | RING_TAIL  — 3 pairs, force-posted.
		// Gen12 MI_LRI_FORCE_POSTED = bit 12 (confirmed from else-branch header 0x11001003).
		ap[1]  = 0x11001005;   // MI_LRI(3) | FORCE_POSTED
		ap[2]  = 0x00002090;   // RING_CONTEXT_CONTROL addr (RCS)
		ap[3]  = 0xffff0001;   // CTX_CTRL: inhibit sync ctx switch (from initWithOptions decompile)
		ap[4]  = 0x00002034;   // RING_HEAD addr
		ap[5]  = 0x00000000;   // RING_HEAD = 0
		ap[6]  = 0x00002030;   // RING_TAIL addr
		ap[7]  = 0x00000000;   // RING_TAIL = 0

		// LRI block 1 (DW8-DW12): RING_START | RING_CTL  — 2 pairs, force-posted.
		ap[8]  = 0x11001003;   // MI_LRI(2) | FORCE_POSTED
		ap[9]  = 0x00002038;   // RING_START addr
		ap[10] = rcsStart;     // RING_START value (from live MMIO)
		ap[11] = 0x00002040;   // RING_CTL addr
		ap[12] = rcsCtl | 1;   // RING_CTL | RING_VALID

		// Apple context-image valid marker at DW80 (from initWithOptions decompile:
		// *(pSVar10 + 0x1140) = 0x5000001, where 0x1140 - 0x1000 = 0x140 = 80*4).
		ap[80] = 0x05000001;

		asm volatile("sfence" ::: "memory");
		SYSLOG("ngreen", "V509: repair done DW1=%08x DW10(RING_START)=%08x DW12(CTL)=%08x",
		       (uint32_t)ap[1], rcsStart, rcsCtl | 1);
	}

	cb->writeReg32(GGTT_PTE_LO(0), saveLo);
	cb->writeReg32(GGTT_PTE_HI(0), saveHi);
	cb->writeReg32(0x101008, 0x1);
	return ret;
}

// V508/V509: Hook IGHardwareContext::withOptions.
// V508 (diagnostic, first 4 calls): logs FIFO/ring-buffer fields and page1 raw content.
// V509 (repair, every RCS call): wbinvd flushes any stale cache write that re-stamped DW1 with
//   0x00ffffff after initWithOptions, then writes the correct Gen12 ring context MI_LRI block to
//   GGTT[0x19] (ExecList submission slot for page1) via aperture.  Without this, hardware sees
//   MI_NOOP at DW1, skips all register restore, ring state is undefined, context posts IDLE.
void *Gen11::IGHardwareContextwithOptions(void *task, const void *params, uint8_t arg)
{
	void *ctx = FunctionCast(IGHardwareContextwithOptions,
	                         callback->oIGHardwareContextwithOptions)(task, params, arg);
	auto *cb = NGreen::callback;
	if (cb->isRealTGL || !ctx) return ctx;

	uint8_t engType = getMember<uint8_t>(ctx, 0x6c);

	// V508: diagnostic dump (limited to first 4 withOptions calls).
	static int v508Count = 0;
	if (v508Count < 4) {
		v508Count++;
		void *fifo = getMember<void *>(ctx, 0xb8);
		SYSLOG("ngreen", "V508[%d]: ctx=%p fifo=%p arg=%u engType=%u",
		       v508Count, ctx, fifo, (unsigned)arg, (unsigned)engType);
		if (fifo) {
			for (int i = 0; i < 12; i++) {
				uint64_t v = getMember<uint64_t>(fifo, i * 8);
				SYSLOG("ngreen", "V508[%d]: fifo[0x%02x]=0x%016llx", v508Count, i*8, (unsigned long long)v);
			}
			void *ringBuf = getMember<void *>(fifo, 0x18);
			if (ringBuf && v508Count == 1) {
				SYSLOG("ngreen", "V508[%d]: ring_buf=%p:", v508Count, ringBuf);
				for (int i = 0; i < 20; i++) {
					uint64_t v = getMember<uint64_t>(ringBuf, i * 8);
					SYSLOG("ngreen", "V508[%d]:   rb[0x%03x]=0x%016llx", v508Count, i*8, (unsigned long long)v);
				}
			}
		}
	}

	// V509 repair: RCS contexts only.
	// The ExecList submission slot (GGTT[0x19]) is the physical page hardware reads on
	// context-restore. It starts as GEM fill (0x00ffffff / 0x00bfbfbf) because Apple never
	// copies the valid LRI block that restoreFromSafeImage wrote into the actual context
	// buffer (GGTT[ctxPage1Idx]).  Fix: read context buffer → copy to submission slot.
	// Also save the ctx pointer so V507 can re-run the repair on every populateResetRegisterList
	// call (the display retry loop reuses the same context without calling withOptions again).
	if (engType != 0) return ctx;
	cb->lastRCSCtx = ctx;

	uint32_t lrcaGpuVa = getMember<uint32_t>(ctx, 0x89) & 0xFFFFF000;
	if (!lrcaGpuVa) { SYSLOG("ngreen", "V509: LRCA GPU VA zero"); return ctx; }
	uint32_t ctxPage1Idx = (lrcaGpuVa >> 12) + 1;

	cb->setApertureIfNecessary();
	if (!cb->aperturePtr || cb->apertureLen < 0x1000) return ctx;

	uint32_t slotLo = cb->readReg32(GGTT_PTE_LO(0x19));
	uint32_t slotHi = cb->readReg32(GGTT_PTE_HI(0x19));
	uint32_t ctxLo  = cb->readReg32(GGTT_PTE_LO(ctxPage1Idx));
	uint32_t ctxHi  = cb->readReg32(GGTT_PTE_HI(ctxPage1Idx));
	if (!(slotLo & 1) || !(ctxLo & 1)) return ctx;

	asm volatile("wbinvd" ::: "memory");

	volatile uint32_t *ap = cb->aperturePtr;
	uint32_t saveLo = cb->readReg32(GGTT_PTE_LO(0));
	uint32_t saveHi = cb->readReg32(GGTT_PTE_HI(0));

	// Pass 1: read DW[0..95] from actual context buffer page1 into stack buffer.
	uint32_t buf[96];
	cb->writeReg32(GGTT_PTE_LO(0), ctxLo);
	cb->writeReg32(GGTT_PTE_HI(0), ctxHi);
	cb->writeReg32(0x101008, 0x1);
	for (int i = 0; i < 96; i++) buf[i] = ap[i];

	static uint32_t pageDumpRows = 0;
	const bool pageDumpLog = ++pageDumpRows <= 4;
	if (pageDumpLog) {
		SYSLOG("ngreen", "V508: ctx  pg1 DW0..7: %08x %08x %08x %08x  %08x %08x %08x %08x",
		       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
	}

	// Pass 2: map submission slot, check validity, copy if needed.
	cb->writeReg32(GGTT_PTE_LO(0), slotLo);
	cb->writeReg32(GGTT_PTE_HI(0), slotHi);
	cb->writeReg32(0x101008, 0x1);

	if (pageDumpLog) {
		SYSLOG("ngreen", "V508: slot pg1 DW0..7: %08x %08x %08x %08x  %08x %08x %08x %08x",
		       ap[0], ap[1], ap[2], ap[3], ap[4], ap[5], ap[6], ap[7]);
	}

	if ((ap[1] & 0x1F801000) != 0x11001000) {
		if ((buf[1] & 0x1F801000) == 0x11001000) {
			for (int i = 0; i < 96; i++) ap[i] = buf[i];
			asm volatile("sfence" ::: "memory");
			SYSLOG("ngreen", "V509: ctx→slot copy done  DW1=%08x DW10=%08x DW12=%08x",
			       buf[1], buf[10], buf[12]);
		} else {
			SYSLOG("ngreen", "V509: both invalid  slot=%08x ctx=%08x — skipping",
			       (uint32_t)ap[1], buf[1]);
		}
	} else if (v508Count <= 4) {
		SYSLOG("ngreen", "V509: slot DW1=%08x already valid — no repair", (uint32_t)ap[1]);
	}

	cb->writeReg32(GGTT_PTE_LO(0), saveLo);
	cb->writeReg32(GGTT_PTE_HI(0), saveHi);
	cb->writeReg32(0x101008, 0x1);
	return ctx;
}

uint64_t Gen11::IGHardwareExtendedContextinitWithOptions(void *that,void *param_1,void *param_2)
{
	if (!callback->oIGHardwareExtendedContextinitWithOptions) {
		static bool v136ExtInitLogged = false;
		if (!v136ExtInitLogged) {
			v136ExtInitLogged = true;
			SYSLOG("ngreen", "V136: oIGHardwareExtendedContextinitWithOptions is null");
		}
		return 0;
	}
	void *b8pre = getMember<void *>(that, 0xb8);
	// V502: log ExtendedCtxParams+0x10 (buffer size) and +0x18 (scheduler flag).
	// From Ghidra: if param_2+0x18==0 → simple buffer alloc path (no GPU cmd).
	// If param_2+0x18!=0 → GPU command submission via vtable+0x118 (hang source on RPL).
	uint64_t p2f10 = param_2 ? getMember<uint64_t>(param_2, 0x10) : 0;
	uint64_t p2f18 = param_2 ? getMember<uint64_t>(param_2, 0x18) : 0;
	static bool v502iwoLogged = false;
	if (!v502iwoLogged) {
		v502iwoLogged = true;
		SYSLOG("ngreen", "V502: initWithOptions ExtCtxParams=%p +0x10=0x%llx +0x18=0x%llx (0=simple,!=0=GPU-cmd)",
			   param_2, (unsigned long long)p2f10, (unsigned long long)p2f18);
	}
	if (param_2 != nullptr && callback->Blit3DExtendedCtxParams != 0 && reinterpret_cast<mach_vm_address_t>(param_2) == callback->Blit3DExtendedCtxParams)
		roundScratchSize(param_2);
	if (param_2 != nullptr && callback->ResolveExtendedCtxParams != 0 && reinterpret_cast<mach_vm_address_t>(param_2) == callback->ResolveExtendedCtxParams)
		bumpResolveBufferSize(param_2);
	uint64_t ret = FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
	void *b8post = getMember<void *>(that, 0xb8);
	// V115 REMOVED: V115 suppressed initWithOptions to prevent MCE from GGTT[0]→stolen mem.
	// V116 now remaps GGTT[0] to a safe dummy page, so the MCE can't happen.
	// V115 was causing Apple's (un-hooked) getBlit3DContext to return nullptr, which then
	// made submitBlit+0x28e dereference nullptr+0xb8 → page fault. Pass through real result.
	static int v501Count = 0;
	if (v501Count < 8) {
		v501Count++;
		SYSLOG("ngreen", "V501[%d]: initWithOptions(%p) ret=%llu b8_pre=%p b8_post=%p p1=%p p2=%p",
			   v501Count, that, (unsigned long long)ret, b8pre, b8post, param_1, param_2);
	}
	return ret;
	
	uint8_t ctx=getMember<uint8_t>(that, 0x6c);
	
	//intel_engine_init_workarounds(engine);
		//engine_fake_wa_init
		/*if (engine->class == COMPUTE_CLASS)
		ccs_engine_wa_init(engine, wal);
		 else if (engine->class == RENDER_CLASS)
		rcs_engine_wa_init(engine, wal);
		 else
		xcs_engine_wa_init(engine, wal);*/
	
	//intel_engine_init_whitelist(engine);
	//intel_engine_init_ctx_wa(engine);
	
	if (ctx==0)// RCS
	{
		
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(RENDER_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		
		
		
		//	rcs_engine_wa_init(engine, wal);
		//rcs_engine_wa_init
		//Wa_1606700617:tgl,dg1,adl-p
		NGreen::callback->wa_masked_en(
				 GEN9_CS_DEBUG_MODE1,
				 FF_DOP_CLOCK_GATE_DISABLE);
		
		
		// Wa_1606931601:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2, GEN12_DISABLE_EARLY_READ);


		 // Wa_1407928979:tgl A
		NGreen::callback->wa_write_or( GEN7_FF_THREAD_MODE,
				GEN12_FF_TESSELATION_DOP_GATE_DISABLE);

		//general_render_compute_wa_init
		// Wa_1406941453:tgl,rkl,dg1,adl-s,adl-p
		NGreen::callback->wa_mcr_masked_en(
				 GEN10_SAMPLER_MODE,
				 ENABLE_SMALLPL);
		
		
		// Wa_1409804808
		NGreen::callback->wa_mcr_masked_en( GEN8_ROW_CHICKEN2,
				 GEN12_PUSH_CONST_DEREF_HOLD_DIS);

		// Wa_14010229206 /
		NGreen::callback->wa_mcr_masked_en( GEN9_ROW_CHICKEN4, GEN12_DISABLE_TDL_PUSH);
		
		// Wa_1607297627
		NGreen::callback->wa_masked_en(
				RING_PSMI_CTL(RENDER_RING_BASE),
				GEN12_WAIT_FOR_EVENT_POWER_DOWN_DISABLE |
				GEN8_RC_SEMA_IDLE_MSG_DISABLE);
		
		
		//if (GRAPHICS_VER(i915) >= 9)
		NGreen::callback->wa_masked_en(
				 GEN7_FF_SLICE_CS_CHICKEN1,
				 GEN9_FFSC_PERCTX_PREEMPT_CTRL);
		
		
		
		//tgl_whitelist_build
		//WaAllowPMDepthAndInvocationCountAccessFromUMD
		NGreen::callback->whitelist_reg_ext( PS_INVOCATION_COUNT,
				  RING_FORCE_TO_NONPRIV_ACCESS_RD |
				  RING_FORCE_TO_NONPRIV_RANGE_4);

		
		 // Wa_1808121037:tgl

		NGreen::callback->whitelist_reg( GEN7_COMMON_SLICE_CHICKEN1);

		// Wa_1806527549:tgl
		NGreen::callback->whitelist_reg( HIZ_CHICKEN);

		// Required by recommended tuning setting (not a workaround)
		NGreen::callback->whitelist_reg( GEN11_COMMON_SLICE_CHICKEN3);
		
		
		
		//intel_engine_init_ctx_wa
		//gen12_ctx_workarounds_init
		// * Wa_1409142259:tgl,dg1,adl-p
		
		NGreen::callback->wa_masked_en( GEN11_COMMON_SLICE_CHICKEN3,
									  GEN12_DISABLE_CPS_AWARE_COLOR_PIPE);
		
		/* WaDisableGPGPUMidThreadPreemption:gen12 */
		NGreen::callback->wa_masked_field_set( GEN8_CS_CHICKEN1,
											 GEN9_PREEMPT_GPGPU_LEVEL_MASK,
											 GEN9_PREEMPT_GPGPU_THREAD_GROUP_LEVEL);
		
		//* Wa_16011163337 - GS_TIMER
		NGreen::callback->wa_add(
								GEN12_FF_MODE2,
								~0,
								FF_MODE2_TDS_TIMER_128 | FF_MODE2_GS_TIMER_224,
								0, false);
		
						
	}
	
	if (ctx==1)//CCS
	{
		//return 0;
		//engine_fake_wa_init
		uint8_t mocs= 3;
		NGreen::callback->wa_masked_field_set(
					RING_CMD_CCTL(GEN12_COMPUTE0_RING_BASE),
					CMD_CCTL_MOCS_MASK,
					CMD_CCTL_MOCS_OVERRIDE(mocs, mocs));
		
		//panic("x");
	}
	
	if (ctx==2)//BCS
	{

		//gen12_ctx_gt_mocs_init
		/*table->size  = ARRAY_SIZE(tgl_mocs_table);
		table->table = tgl_mocs_table;
		table->n_entries = GEN9_NUM_MOCS_ENTRIES;
		table->uc_index = 3;*/
		uint8_t mocs;
		//if (engine->class == COPY_ENGINE_CLASS) {
			mocs = 3;
			NGreen::callback->wa_write_clr_set(
					 BLIT_CCTL(BLT_RING_BASE),//engine->mmio_base
					 BLIT_CCTL_MASK,
					 BLIT_CCTL_MOCS(mocs, mocs));
		
	}
	
	if (ctx==3)//VCS
	{
	}
	
	if (ctx==4)//VCS2
	{
	}
	
	if (ctx==5)//VECS
	{
	}
	
	
	
	return FunctionCast(IGHardwareExtendedContextinitWithOptions, callback->oIGHardwareExtendedContextinitWithOptions)(that,param_1,param_2);
}

void  Gen11::initBlitUsage(void *that)
{
	// V121: initBlitUsage reads [ctx+0xb8]+0x178 without null checks.
	// Guard: skip if ctx+0xb8 is not yet populated (early init paths before
	// blit3d_init_ctx has run). In the render phase ctx+0xb8 is always set.
	if (!NGreen::callback->isRealTGL) {
		if (!getMember<void *>(that, 0xb8)) {
			DBGLOG("ngreen", "V121: initBlitUsage deferred — ctx+0xb8 null");
			return;
		}
	}
	FunctionCast(initBlitUsage, callback->oinitBlitUsage)(that);
}

unsigned long Gen11::submitBlit(void *that, void *param_1, void *param_2, void *param_3, bool param_4) {
	if (!NGreen::callback->isRealTGL && param_1) {
		auto *blit = reinterpret_cast<uint8_t *>(param_1);
		const uint32_t route = (blit[0xA2] >> 3U) & 0x3U;
		void *taskVt = param_3 ? getMember<void *>(param_3, 0x0) : nullptr;
		const bool hasTask = param_3 && taskVt && callback->osubmitBlit;
		if (route == 3) {
			if (hasTask)
				return FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
		} else if (route == 0 && hasTask) {
			bool pass = ((blit[0x3C] & 1U) != 0) || ((blit[0x84] & 1U) != 0) ||
			            (*reinterpret_cast<uint64_t *>(blit + 0x20) != 0) ||
			            (*reinterpret_cast<uint64_t *>(blit + 0x68) != 0);
			bool steer = false;
			if (!pass && gBlit2DSupported)
				pass = !reinterpret_cast<bool (*)(void *, void *, void *)>(gBlit2DSupported)(
					param_1, param_2, reinterpret_cast<uint8_t *>(that) + 0xF2E);
			if (!pass && gBlit3DSupported)
				pass = steer = reinterpret_cast<bool (*)(void *, void *)>(gBlit3DSupported)(param_1, param_2);
			if (pass) {
				const uint64_t dst = *reinterpret_cast<uint64_t *>(blit + 0x48) + *reinterpret_cast<uint32_t *>(blit + 0x74);
				const uint64_t src = *reinterpret_cast<uint64_t *>(blit + 0x00) + *reinterpret_cast<uint32_t *>(blit + 0x2C);
				if (dst == 0 || src == 0)
					return 1;
				const uint8_t saved = blit[0xA2];
				if (steer) blit[0xA2] = static_cast<uint8_t>(saved | 0x18U);
				const unsigned long ret =
					FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
				blit[0xA2] = saved;
				return ret;
			}
		}
	}

	// V186: For spoofed non-TGL, if V142 is configured to bypass submitBlit,
	// return before any task/context touching logic. The V149/V171 path writes
	// task+0x298 and can poison task lifetime on some boots, later crashing in
	// IGAccelTask::release from the garbage collector interrupt path.
	if (!NGreen::callback->isRealTGL) {
		static int v186Mode = -1;
		if (v186Mode < 0) {
			v186Mode = getV142SubmitBlitMode();
			SYSLOG("ngreen", "V186: early submitBlit spoof mode=%d (0=unsupported,1=ret0,2=ret1,3=orig)", v186Mode);
		}

		if (v186Mode != 3) {
			if (isV142Diag2DBypassEnabled() && param_1) {
				auto *blit = reinterpret_cast<uint8_t *>(param_1);
				const bool has3DFlags =
					((blit[0x3C] & 1U) != 0) ||
					((blit[0x84] & 1U) != 0) ||
					(*reinterpret_cast<uint64_t *>(blit + 0x20) != 0) ||
					(*reinterpret_cast<uint64_t *>(blit + 0x68) != 0);
				const uint32_t routeSel = (blit[0xA2] >> 3U) & 0x3U;
				static int v186DiagCount = 0;
				if (v186DiagCount < 64) {
					v186DiagCount++;
					SYSLOG("ngreen", "V186D[%d]: mode=%d routeSel=%u has3D=%d task=%p",
						   v186DiagCount, v186Mode, routeSel, (int)has3DFlags, param_3);
				}
			}
			if (v186Mode == 2)
				return 1;
			if (v186Mode == 1)
				return 0;
			return static_cast<uint32_t>(kIOReturnUnsupported);
		}
	}

	// V149: blit3D context is cached at task+0x298 per IGAccelTask::getBlit3DContext IDA.
	// Previous code used 0xb8 — that overwrote an unrelated IGAccelTask member.
	auto ensureTaskContext = [&](void *task, const char *origin) -> void * {
		if (!task || NGreen::callback->isRealTGL)
			return task ? getMember<void *>(task, 0x298) : nullptr;

		void *taskCtx = getMember<void *>(task, 0x298);
		if (taskCtx)
			return taskCtx;

		// param_1=true triggers allocation; Apple then stores result at task+0x298 internally.
		// V171: Our getBlit3DContext hook returns the global cached ctx but does NOT write
		// task+0x298. Apple's original would store it per-task. Do it explicitly here so
		// the invalidTask check (getMember<void*>(task,0x298) == nullptr) passes.
		void *ctx = callback->getBlit3DContext(task, true);
		if (!ctx)
			return nullptr;

		getMember<void *>(task, 0x298) = ctx;

		if (isExperimentalMonitorEnabled()) {
			static int v149Count = 0;
			if (v149Count < 32) {
				v149Count++;
				SYSLOG("ngreen", "V149[%d]: %s task=%p stored ctx@298=%p", v149Count, origin, task, ctx);
			}
		}

		return ctx;
	};

	bool invalidTask = (param_3 == nullptr);
	if (isExperimentalMonitorEnabled()) {
		static int v137SubmitEntryCount = 0;
		if (v137SubmitEntryCount < 40) {
			v137SubmitEntryCount++;
			void *entryVtable = param_3 ? getMember<void *>(param_3, 0x0) : nullptr;
			void *entryCtx = param_3 ? getMember<void *>(param_3, 0x298) : nullptr;
			SYSLOG("ngreen", "V149.submitBlit.entry[%d]: acc=%p p1=%p p2=%p task=%p vtbl=%p ctx@298=%p b=%u c3D=%p c2D=%p cTask=%p",
				   v137SubmitEntryCount, that, param_1, param_2, param_3, entryVtable, entryCtx,
				   static_cast<unsigned>(param_4), callback->v131CachedBlit3DCtx,
				   callback->v131CachedBlit2DCtx, callback->v132CachedTask);
		}
	}
	if (!invalidTask) {
		// V135: Some panics still reached Apple submitBlit with a non-null but broken task object
		// (null vtable / null ctx field), which can lead to RIP=0 indirect calls in Apple code.
		// Validate minimal task invariants before calling through.
		ensureTaskContext(param_3, "submitBlit-incoming");
		void *taskVtable = getMember<void *>(param_3, 0x0);
		void *taskCtx = getMember<void *>(param_3, 0x298);
		invalidTask = (taskVtable == nullptr) || (taskCtx == nullptr);
		if (invalidTask) {
			static int v135Count = 0;
			if (v135Count < 16) {
				v135Count++;
				SYSLOG("ngreen", "V149[%d]: submitBlit invalid task=%p vtbl=%p ctx@298=%p", v135Count, param_3, taskVtable, taskCtx);
			}
		}
	}

	if (invalidTask && callback->v132CachedTask && callback->v132CachedTask != param_3) {
		void *cachedCtx = ensureTaskContext(callback->v132CachedTask, "submitBlit-cached");
		void *cachedVtable = getMember<void *>(callback->v132CachedTask, 0x0);
		if (cachedVtable && cachedCtx) {
			if (isExperimentalMonitorEnabled()) {
				static int v138SwapCount = 0;
				if (v138SwapCount < 24) {
					v138SwapCount++;
					SYSLOG("ngreen", "V138.swap[%d]: replacing task=%p with cached=%p ctx=%p",
						   v138SwapCount, param_3, callback->v132CachedTask, cachedCtx);
				}
			}
			param_3 = callback->v132CachedTask;
			invalidTask = false;
		}
	}

	if (invalidTask) {
		static int v120Mode = -1;
		if (v120Mode < 0) {
			int parsed = 0;
			if (!NGreen::callback->isRealTGL) {
				// V120 modes for spoofed path:
				//   ngreenV120=0 or -ngreenV120ok   -> return success (legacy behavior)
				//   ngreenV120=1 or -ngreenV120fail -> return unsupported (default)
				//   ngreenV120=2 or -ngreenV120pass -> return 1
				if (PE_parse_boot_argn("ngreenV120", &parsed, sizeof(parsed))) {
					v120Mode = parsed;
				} else if (checkKernelArgument("-ngreenV120pass")) {
					v120Mode = 2;
				} else if (checkKernelArgument("-ngreenV120ok")) {
					v120Mode = 0;
				} else if (checkKernelArgument("-ngreenV120fail")) {
					v120Mode = 1;
				} else {
					v120Mode = 1;
				}
			} else {
				v120Mode = 0;
			}
			SYSLOG("ngreen", "V120: submitBlit NULL-task mode=%d (0=ret0,1=unsupported,2=ret1)", v120Mode);
		}

		static int v120NullCount = 0;
		if (v120NullCount < 32) {
			v120NullCount++;
			SYSLOG("ngreen", "V120[%d]: submitBlit NULL task that=%p p1=%p p2=%p bool=%d mode=%d",
				   v120NullCount, that, param_1, param_2, param_4, v120Mode);
		}

		if (isExperimentalMonitorEnabled()) {
			static int v137SubmitInvalidCount = 0;
			if (v137SubmitInvalidCount < 40) {
				v137SubmitInvalidCount++;
				SYSLOG("ngreen", "V137.submitBlit.invalid[%d]: task=%p cTask=%p c3D=%p c2D=%p mode=%d",
					   v137SubmitInvalidCount, param_3, callback->v132CachedTask,
					   callback->v131CachedBlit3DCtx, callback->v131CachedBlit2DCtx, v120Mode);
			}
		}

		if (v120Mode == 2)
			return 1;
		if (v120Mode == 1)
			return static_cast<uint32_t>(kIOReturnUnsupported);
		return 0;
	}

	if (!NGreen::callback->isRealTGL) {
		// V142: after the movaps/movapd fixes, the remaining watchdog still points at
		// Apple's original blit submit path wedging BCS. Keep original behavior by default
		// to avoid breaking the renderer, but provide an explicit no-log test switch for
		// spoofed RPL boots when we need to prove the hang is inside submitBlit itself.
		static int v142Mode = -1;
		if (v142Mode < 0) {
			v142Mode = getV142SubmitBlitMode();
			SYSLOG("ngreen", "V142: submitBlit spoof mode=%d (0=hard-unsupported,1=ret0,2=ret1,3=orig)", v142Mode);
		}

		if (v142Mode != 3) {
			static int v142Count = 0;
			if (v142Count < 24) {
				v142Count++;
				SYSLOG("ngreen", "V142[%d]: bypass submitBlit acc=%p p1=%p p2=%p task=%p b=%u mode=%d",
					   v142Count, that, param_1, param_2, param_3, static_cast<unsigned>(param_4), v142Mode);
			}

			if (v142Mode == 2)
				return 1;
			if (v142Mode == 1)
				return 0;
			// Mode 0 is intentionally harsh and should only be used for explicit diagnostics.
			return static_cast<uint32_t>(kIOReturnUnsupported);
		}
	}

	// V193: On !isRealTGL (RPL spoof), block routeSel=3 before calling Apple's submitBlit.
	// routeSel=3 routes to blit3d_submit_commands which executes TGL-compiled EU shaders
	// from the scratch buffer. RPL has a different EU topology — the shader unit stalls
	// (INSTDONE_1 bit clear), RCS hangs inside the batch at BB_ADDR, BCS then deadlocks
	// on a MI_SEMAPHORE_WAIT for the value RCS never writes → Sig 803 GPU reset loop.
	// routeSel=0..2 (2D blits, Apple logo, compositor) pass through safely.
	// Side effect: cursor sprite (uses routeSel=3) becomes invisible — acceptable vs. GPU hang.
	if (!NGreen::callback->isRealTGL && param_1) {
		auto *blit193 = reinterpret_cast<uint8_t *>(param_1);
		const uint32_t routeSel193 = (blit193[0xA2] >> 3U) & 0x3U;
		if (routeSel193 == 3) {
			static int v193Count = 0;
			if (v193Count < 32) {
				v193Count++;
				SYSLOG("ngreen", "V193[%d]: blocked routeSel=3 on RPL (TGL EU shader → RCS hang)", v193Count);
			}
			return 1;
		}
	}

	// Safety guard: avoid null indirect call if route capture failed.
	if (!callback->osubmitBlit) {
		static bool v134Logged = false;
		if (!v134Logged) {
			v134Logged = true;
			SYSLOG("ngreen", "V134: osubmitBlit is null, preventing call-through crash");
		}
		if (!NGreen::callback->isRealTGL)
			return 0;
		return static_cast<unsigned long>(kIOReturnUnsupported);
	}

	return FunctionCast(submitBlit, callback->osubmitBlit)(that, param_1, param_2, param_3, param_4);
}

void  Gen11::markBlitUsage(void *that)
{
	// V122: markBlitUsage dereferences [ctx+0xb8] without null checks.
	// Same guard as V121: skip only when ctx+0xb8 is not yet set.
	if (!NGreen::callback->isRealTGL) {
		if (!getMember<void *>(that, 0xb8)) {
			DBGLOG("ngreen", "V122: markBlitUsage deferred — ctx+0xb8 null");
			return;
		}
	}
	FunctionCast(markBlitUsage, callback->omarkBlitUsage)(that);
}

// still to order..

void * Gen11::IGMappedBuffergetMemory(void *that)
{
	auto ret=FunctionCast(IGMappedBuffergetMemory, callback->oIGMappedBuffergetMemory)(that);
	return ret;
}

uint32_t  Gen11::IGAccelSegmentResourceListprepare(void *that)
{
	// V123c: keep original prepare flow. initBlitUsage/markBlitUsage are already
	// individually guarded for !isRealTGL, so short-circuiting prepare is no longer needed
	// and can leave command-queue state incomplete, leading to stalls.
	return FunctionCast(IGAccelSegmentResourceListprepare, callback->oIGAccelSegmentResourceListprepare)(that);
}

uint32_t Gen11::beginCoalescedSegment(void *that) {
	// V124/IDA verified: beginCoalescedSegment is pure DTrace observability.
	// Full disassembly:
	//   [queue+0x830] = 0xFFFFFFFF           ← segment marker
	//   task = *(*(queue+0x5C0)+0x150)
	//   ctx  = getBlit3DContext(task, true)   ← may return null in early init
	//   r15  = ctx+0xB8                       ← CRASH if ctx null or ctx+0xB8 null
	//   r14  = r15+0x178                      ← usage counter
	//   dtracePushUnique(queue+0x5C0+0xE08, queue+0x838)
	//   dtHookSubmitQueueKMD(queue+0x838, 4, r14+1, r15+0x20, 0, 0)
	// No GPU ring submission whatsoever.
	//
	// Always write queue+0x830 first (matches Apple's implementation).
	reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(that) + 0x830)[0] = 0xFFFFFFFF;

	if (!NGreen::callback->isRealTGL) {
		// Guard: ctx+0xB8 must be non-null before calling through (DTrace reads it).
		// In early init getBlit3DContext returns null → skip DTrace, return 1.
		// In render phase ctx+0xB8 is always populated → call through for correct tracing.
		void *cached = callback->v131CachedBlit3DCtx;
		if (!cached || !getMember<void *>(cached, 0xb8)) {
			static int v124EarlyCount = 0;
			if (v124EarlyCount < 8) {
				v124EarlyCount++;
				DBGLOG("ngreen", "V124[%d]: beginCoalescedSegment deferred — ctx+0xb8 null", v124EarlyCount);
			}
			return 1;
		}
	}
	return FunctionCast(beginCoalescedSegment, callback->obeginCoalescedSegment)(that);
}

uint8_t Gen11::barrierSubmission(void *queue, void *accelerator, void *cmdDesc,
								 void *event, uint16_t count, const uint16_t *list) {
	if (!NGreen::callback->isRealTGL) {
		if (barrierWants()) {
			void *task = cmdDesc ? getMember<void *>(cmdDesc, 0x8) : nullptr;
			auto ready = [](void *c) { return c != nullptr && getMember<void *>(c, 0xb8) != nullptr; };
			if (callback->obarrierSubmission != 0 && task != nullptr && ready(getBlit2DContext(task, true)) &&
			    ready(getBlit3DContext(task, true)) && ready(getDepthResolveContext(task, true)) &&
			    ready(getColorResolveContext(task, true)))
				return FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator, cmdDesc,
				                                                                      event, count, list);
		}
		// V130: spoof-path guard for deterministic barrierSubmission+0x198 crash.
		// Boot-arg control (value or flags):
		//   ngreenV130=0 or -ngreenV130fail -> bypass and return 0 (default)
		//   ngreenV130=1 or -ngreenV130pass -> bypass and return 1
		//   ngreenV130=2 or -ngreenV130orig -> call original implementation
		//   ngreenV130=3 or -ngreenV130hybrid -> bypass during init, original during render
		// NOTE: on spoofed RPL this is unsafe unless explicitly forced because
		// the original path submits Blit2D/Blit3D barriers through BCS.
		// Use -ngreenV130forceorig only when intentionally validating that path.
		static int v130Mode = -1;
		if (v130Mode < 0) {
			int parsed = 0;
			const bool forceOrig = checkKernelArgument("-ngreenV130forceorig");
			if (PE_parse_boot_argn("ngreenV130", &parsed, sizeof(parsed))) {
				v130Mode = parsed;
			} else if (checkKernelArgument("-ngreenV130orig")) {
				v130Mode = 2;
			} else if (checkKernelArgument("-ngreenV130hybrid")) {
				v130Mode = 3;
			} else if (checkKernelArgument("-ngreenV130pass")) {
				v130Mode = 1;
			} else if (checkKernelArgument("-ngreenV130fail")) {
				v130Mode = 0;
			} else {
				// Default: bypass+ret1 so GPU scheduler treats barrier as succeeded.
				// ret0 causes a spin/wait loop in the scheduler → hard freeze.
				v130Mode = 1;
			}

			if (v130Mode == 2 && !forceOrig) {
				SYSLOG("ngreen", "V150: ngreenV130orig requested on spoofed RPL; forcing mode=1 to avoid BCS barrier stall (use -ngreenV130forceorig to override)");
				v130Mode = 1;
			}
			SYSLOG("ngreen", "V130: INIT barrierSubmission spoof mode=%d (0=bypass-ret0, 1=bypass-ret1, 2=call-original, 3=hybrid init-bypass/render-orig) forceOrig=%d",
				   v130Mode, forceOrig ? 1 : 0);
		}

		static int v130CallCount = 0;
		static int v130CallInitPhase = 0;
		static int v130CallRenderPhase = 0;
		static int v130HybridWarmup = -1;
		if (v130HybridWarmup < 0) {
			int parsedWarmup = 12;
			if (PE_parse_boot_argn("ngreenV130warmup", &parsedWarmup, sizeof(parsedWarmup))) {
				if (parsedWarmup < 0) parsedWarmup = 0;
				if (parsedWarmup > 200) parsedWarmup = 200;
			}
			v130HybridWarmup = parsedWarmup;
			if (v130Mode == 3) {
				SYSLOG("ngreen", "V130: hybrid warmup=%d calls (override with ngreenV130warmup=<0..200>)", v130HybridWarmup);
			}
		}
		
		v130CallCount++;
		// V151 DIAGNOSTIC: Enhanced logging to trace call patterns through init vs render phases
		// For hybrid mode, keep bypass window short to avoid WindowServer init starvation.
		bool isInitPhase = v130CallCount <= v130HybridWarmup;
		if (isInitPhase) v130CallInitPhase++;
		else v130CallRenderPhase++;
		
		if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124) ||
			(v130CallCount > 1000 && v130CallCount % 100 == 0)) {
			SYSLOG("ngreen", "V130[%d|init=%d|render=%d]: phase=%s q=%p acc=%p cmd=%p evt=%p count=%u mode=%d",
				   v130CallCount, v130CallInitPhase, v130CallRenderPhase,
				   isInitPhase ? "INIT" : "RENDER", queue, accelerator, cmdDesc, event,
				   static_cast<unsigned>(count), v130Mode);
		}

		uint8_t retVal = 0;
		if (v130Mode == 3 && !isInitPhase) {
			if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124)) {
				SYSLOG("ngreen", "V130[%d]: HYBRID render phase -> ORIGINAL path", v130CallCount);
			}
			retVal = FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																				 cmdDesc, event,
																				 count, list);
			if (v130CallCount <= 24 || (v130CallCount > 100 && v130CallCount <= 124)) {
				SYSLOG("ngreen", "V130[%d]: HYBRID original returned %u", v130CallCount, static_cast<unsigned>(retVal));
			}
			return retVal;
		}

		// Log the return path for first few calls to capture mode decision
		if (v130Mode == 2) {
			if (v130CallCount <= 24) {
				SYSLOG("ngreen", "V130[%d]: RETURNING ORIGINAL path (mode=2)", v130CallCount);
			}
			retVal = FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																				 cmdDesc, event,
																				 count, list);
			if (v130CallCount <= 24) {
				SYSLOG("ngreen", "V130[%d]: ORIGINAL returned %u", v130CallCount, static_cast<unsigned>(retVal));
			}
			return retVal;
		}

		retVal = static_cast<uint8_t>((v130Mode == 1 || v130Mode == 3) ? 1 : 0);
		if (v130CallCount <= 24) {
			SYSLOG("ngreen", "V130[%d]: RETURNING BYPASS path (mode=%d, ret=%u)", v130CallCount, v130Mode, static_cast<unsigned>(retVal));
		}
		return retVal;
	}
	return FunctionCast(barrierSubmission, callback->obarrierSubmission)(queue, accelerator,
																		 cmdDesc, event,
																		 count, list);
}

void * Gen11::getBlit2DContext(void *that,bool param_1)
{
	// V151 DIAGNOSTIC: Track call frequency to understand init vs render phase
	if (!NGreen::callback->isRealTGL) {
		static int v127CallCount = 0;
		v127CallCount++;
		bool isInitPhase = v127CallCount < 100;
		if (v127CallCount <= 20 || (v127CallCount > 100 && v127CallCount <= 120)) {
			SYSLOG("ngreen", "V127[%d]: getBlit2DContext called (phase=%s) task=%p force=%d",
				   v127CallCount, isInitPhase ? "INIT" : "RENDER", that, param_1 ? 1 : 0);
		}
	}
	
	// V127: On non-real-TGL (RPL spoof), call the original getter and enforce only
	// Blit2D-compatible fallbacks. submitBlit dereferences [ctx+0xb8] and then uses the
	// resulting object as Blit2D FIFO/ring state, so returning depth/color/3D contexts here
	// can corrupt renderer behavior (black output / wrong ring routing).
	if (!NGreen::callback->isRealTGL) {
		auto cachedValid2D = [&]() -> void * {
			void *cached = callback->v131CachedBlit2DCtx;
			if (cached && getMember<void *>(cached, 0xb8))
				return cached;
			return nullptr;
		};

		if (!callback->ogetBlit2DContext) {
			static bool v136Logged = false;
			if (!v136Logged) {
				v136Logged = true;
				SYSLOG("ngreen", "V136: ogetBlit2DContext is null");
			}
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			return nullptr;
		}
		void *ctx = FunctionCast(getBlit2DContext, callback->ogetBlit2DContext)(that, param_1);
		if (isExperimentalMonitorEnabled()) {
			static int v137Blit2DOrigCount = 0;
			if (v137Blit2DOrigCount < 24) {
				v137Blit2DOrigCount++;
				void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
				SYSLOG("ngreen", "V137.getBlit2DContext.orig[%d]: task=%p ctx=%p ctx+0xb8=%p", v137Blit2DOrigCount, that, ctx, ctxField);
			}
		}
		if (!ctx) {
			DBGLOG("ngreen", "V127: getBlit2DContext original returned null on RPL");
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			// No cross-context fallback allowed here (depth/color/3D are incompatible layouts).
			return nullptr;
		}
		void *ctxField = getMember<void *>(ctx, 0xb8);
		if (!ctxField) {
			DBGLOG("ngreen", "V127: getBlit2DContext ctx+0xb8 NULL on RPL, trying cached Blit2D fallback");
			void *fallback = cachedValid2D();
			if (fallback)
				return fallback;
			return nullptr;
		}
		DBGLOG("ngreen", "V127: getBlit2DContext ctx=%p ctx+0xb8=%p OK on RPL", ctx, ctxField);
		// V131: Cache valid context for future NULL fallback
		callback->v131CachedBlit2DCtx = ctx;
		return ctx;
	}
	if (!callback->ogetBlit2DContext)
		return nullptr;
	return FunctionCast(getBlit2DContext, callback->ogetBlit2DContext)(that,param_1);
}

void * __attribute__((noinline)) Gen11::getDepthResolveContext(void *that,bool param_1)
{
	if (!callback->ogetDepthResolveContext) {
		static bool v136DepthLogged = false;
		if (!v136DepthLogged) {
			v136DepthLogged = true;
			SYSLOG("ngreen", "V136: ogetDepthResolveContext is null");
		}
		return nullptr;
	}
	void *ctx = FunctionCast(getDepthResolveContext, callback->ogetDepthResolveContext)(that,param_1);
	initResolveContextAfterReset(ctx, __builtin_return_address(0));
	if (isExperimentalMonitorEnabled()) {
		static int v137DepthCount = 0;
		if (v137DepthCount < 32) {
			v137DepthCount++;
			void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.depthResolve[%d]: task=%p force=%u ctx=%p ctx+0xb8=%p", v137DepthCount, that,
				   static_cast<unsigned>(param_1), ctx, ctxField);
		}
	}
	return ctx;
}

void * __attribute__((noinline)) Gen11::getColorResolveContext(void *that,bool param_1)
{
	if (!callback->ogetColorResolveContext) {
		static bool v136ColorLogged = false;
		if (!v136ColorLogged) {
			v136ColorLogged = true;
			SYSLOG("ngreen", "V136: ogetColorResolveContext is null");
		}
		return nullptr;
	}
	void *ctx = FunctionCast(getColorResolveContext, callback->ogetColorResolveContext)(that,param_1);
	initResolveContextAfterReset(ctx, __builtin_return_address(0));
	if (isExperimentalMonitorEnabled()) {
		static int v137ColorCount = 0;
		if (v137ColorCount < 32) {
			v137ColorCount++;
			void *ctxField = ctx ? getMember<void *>(ctx, 0xb8) : nullptr;
			SYSLOG("ngreen", "V137.colorResolve[%d]: task=%p force=%u ctx=%p ctx+0xb8=%p", v137ColorCount, that,
				   static_cast<unsigned>(param_1), ctx, ctxField);
		}
	}
	if (ctx != nullptr)
		reserveResolveWindow(ctx);
	return ctx;
}

void Gen11::wrapPageon(void *that, void *event, bool flag, uint64_t opt) {
	using Pageon = void (*)(void *, void *, bool, uint64_t);
	reinterpret_cast<Pageon>(callback->oPageon)(that, event, flag, opt);
	if (gPageonActive == 0 || that == nullptr) return;
	waitPageon(that, event);
}

uint32_t Gen11::wrapDoHangAnalysis(void *that)
{
	barrierHangSeen();
	using Fn = uint32_t (*)(void *);
	return reinterpret_cast<Fn>(callback->oDoHangAnalysis)(that);
}

int Gen11::blit3d_supported()
{
	return 0;
}

void  Gen11::setAsyncSliceCount(void *that,uint32_t configRaw)
{
		uint32_t sliceCount     = (configRaw >> 0) & 0xFF;
		uint32_t subsliceCount  = (configRaw >> 8) & 0xFF;
		uint32_t euCount        = (configRaw >> 16) & 0xFF;

	uint32_t sliceField = 0;
		switch (sliceCount) {
			case 1: sliceField = 1; break;
			case 2: sliceField = 2; break;
			default:
				panic("IGPU: setAsyncSliceCount - Invalid slice count: %u\n", sliceCount);
				break;
		}
		
		uint32_t subsliceField = 0;
		switch (subsliceCount) {
			case 2: subsliceField = 0x20; break;
			case 4: subsliceField = 0x40; break;
			case 5: subsliceField = 0x50; break;
			case 6: subsliceField = 0x60; break;
			case 8: subsliceField = 0x80; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid subsliceCount: %u\n", subsliceCount);
						break;
		}

		uint32_t euField = 0;
		switch (euCount) {
			case 1: euField = 0x100; break;
			case 2: euField = 0x200; break;
			case 3: euField = 0x300; break;
			case 4: euField = 0x400; break;
			case 5: euField = 0x500; break;
			case 6: euField = 0x600; break;
			case 8: euField = 0x800; break;
			default:
						panic("IGPU: setAsyncSliceCount - Invalid EU count/power mode: %u\n", euCount);
						break;
		}

		uint32_t hwRegisterValue = sliceField | subsliceField | euField;

		getMember<uint32_t>(that, 0x12a0) = configRaw;


		//SafeForceWake(that,true, 4);
		volatile uint32_t* mmioBase = getMember<volatile uint32_t*>(that, 0x1240);
		mmioBase[0xa204 / 4] = hwRegisterValue;
		//SafeForceWake(that,false, 4);
	
}

static const uint8_t DAT_000b0bb0[] = {
	0x00, 0x36, 0x6e, 0x01, 0x00, 0xf8, 0x24, 0x01,
	0x00, 0xf0, 0x49, 0x02, 0x40, 0x78, 0x7d, 0x01
};

bool Gen11::initHardwareCaps(void *this_ptr) {
		uint32_t gpuSku = getMember<uint32_t>(this_ptr, 0x1120);
		bool result = false;
		
		uint32_t uVar1;
		int iVar2;
		int iVar3;
		int iVar4;

		if (gpuSku == 2) {
			// --- SKU 2 (TGLLP) - Original TGL values for 6 Dual SubSlices (12 SubSlices) ---
					
					// Buffer sizes for 12 SubSlices (6 DSS × 2 SS/DSS)
					// 0xc0 (192) = 16 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x112c) = 0x222000000c0ULL;
					
					// 0x150 (336) = 28 bytes × 12 SS
					getMember<uint64_t>(this_ptr, 0x1134) = 0x22200000150ULL;
					getMember<uint32_t>(this_ptr, 0x113c) = 0x150;
					
					getMember<uint64_t>(this_ptr, 0x1174) = 0x200000007ULL;
					getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000080ULL;
					
					getMember<uint32_t>(this_ptr, 0x1160) = 0xf00;
					
					// Max Dual SubSlices = 6 (matches hardware: 6 DSS)
					getMember<uint32_t>(this_ptr, 0x1148) = 0x6;
					
					// Calculate actual Dual SubSlices (SubSlices / 2)
					uVar1 = getMember<uint32_t>(this_ptr, 0x1158) >> 1;
					iVar3 = 2;
					
					// Reference Dual SubSlices count = 6 (must match max to avoid underflow)
					iVar4 = 0x6;
					iVar2 = 0x80;
		}
		else {
			if (gpuSku != 1) {
				result = false;
				return result;
			}
			
			// --- SKU 1 (TGLHP) - Modified for 5 DSS ---
			
			// Sizes for 10 SubSlices
			getMember<uint64_t>(this_ptr, 0x112c) = 0x2d800000140ULL;
			getMember<uint64_t>(this_ptr, 0x1134) = 0x27000000168ULL;
			getMember<uint32_t>(this_ptr, 0x113c) = 0x168;
			
			getMember<uint64_t>(this_ptr, 0x1174) = 0x100000007ULL;
			getMember<uint64_t>(this_ptr, 0x117c) = 0x1000000040ULL;
			
			getMember<uint32_t>(this_ptr, 0x1160) = 0x800;
			
			// Max SubSlices set to 10
			getMember<uint32_t>(this_ptr, 0x1148) = 0xA;
			
			uVar1 = getMember<uint32_t>(this_ptr, 0x1158);
			iVar3 = 1;
			
			// Reference count set to 10
			iVar4 = 0xA;
			iVar2 = 0x40;
		}

		// Final calculations
		getMember<uint32_t>(this_ptr, 0x114c) = uVar1;
		
		uint8_t &byteRef = getMember<uint8_t>(this_ptr, 0x1184);
		byteRef = byteRef & 0xFB;
		
		getMember<uint32_t>(this_ptr, 0x1128) = iVar3 * uVar1;
		getMember<uint32_t>(this_ptr, 0x1144) = (iVar4 - uVar1) * iVar3;
		getMember<uint32_t>(this_ptr, 0x1140) = iVar2 * uVar1;
		
		getMember<uint32_t>(this_ptr, 0x1168) = getMember<uint32_t>(this_ptr, 0x115c) << 4;
		
		result = true;
		return result;
}

void Gen11::checkWOPCMSettings(void *that,unsigned long param_1,void *param_2)
{
	const uint32_t GUC_WOPCM_OFFSET = 1 * 1024 * 1024;
	const uint32_t GUC_WOPCM_SIZE = 1 * 1024 * 1024;
	
	typedef struct {
		uint64_t address;
		uint64_t length;
	} IOVirtualRange;
	
	IOVirtualRange *wopcm_range = (IOVirtualRange *)param_2;
		
	wopcm_range->address = GUC_WOPCM_OFFSET;
	wopcm_range->length = GUC_WOPCM_SIZE;
	
}

IOReturn Gen11::wrapFBClientDoAttribute(void *fbclient, uint32_t attribute, unsigned long *unk1, unsigned long unk2, unsigned long *unk3, unsigned long *unk4,  void *externalMethodArguments) {
	if (attribute == 0x923) {
		return kIOReturnUnsupported;
	}
	return FunctionCast(wrapFBClientDoAttribute, callback->orgFBClientDoAttribute)(fbclient, attribute, unk1, unk2, unk3, unk4,  externalMethodArguments);
}

static constexpr uint32_t kFwAckLockedUs  = 500;
static volatile void    *gFwOwnerThread   = nullptr;

void Gen11::wrapSafeForceWakeReent(void *that, bool wake, uint32_t domain) {
	void *self = reinterpret_cast<void *>(IOThreadSelf());
	if (gFwOwnerThread == self) {
		forceWake(that, wake, domain, 2);
		return;
	}
	const bool claimed = __sync_bool_compare_and_swap(&gFwOwnerThread, nullptr, self);
	FunctionCast(wrapSafeForceWakeReent, callback->oSafeForceWakeReent)(that, wake, domain);
	if (claimed) gFwOwnerThread = nullptr;
}

unsigned long Gen11::loadGuCBinary(void *that) {
	// V52: Real TGL can authenticate and load GuC firmware natively.
	// RPL cannot — stub to return 1 and use host scheduling instead.
	if (NGreen::callback->isRealTGL) {
		SYSLOG("ngreen", "loadGuCBinary: real TGL — calling original for GuC firmware load");
		return FunctionCast(loadGuCBinary, callback->oloadGuCBinary)(that);
	}
	SYSLOG("ngreen", "loadGuCBinary: RPL — stubbed to return 1 (host scheduling)");
	return 1;
}

UInt8 Gen11::wrapLoadGuCBinary(void *that) {

	if (callback->firmwareSizePointer)
		callback->performingFirmwareLoad = true;

	auto r = FunctionCast(wrapLoadGuCBinary, callback->orgLoadGuCBinary)(that);
	DBGLOG("ngreen", "loadGuCBinary returned %d", r);

	callback->performingFirmwareLoad = false;

	return r;
}

bool Gen11::wrapLoadFirmware(void *that) {

	//(*reinterpret_cast<uintptr_t **>(that))[35] = reinterpret_cast<uintptr_t>(wrapSystemWillSleep);
	//(*reinterpret_cast<uintptr_t **>(that))[36] = reinterpret_cast<uintptr_t>(wrapSystemDidWake);
	return FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

void Gen11::wrapSystemWillSleep(void *that) {
	DBGLOG("ngreen", "systemWillSleep GuC callback");
}

void Gen11::wrapSystemDidWake(void *that) {
	DBGLOG("ngreen", "systemDidWake GuC callback");

	// This is IGHardwareGuC class instance.
	auto &GuC = (reinterpret_cast<OSObject **>(that))[76];

	if (GuC)
	if (GuC->metaCast("IGHardwareGuC")) {
		DBGLOG("igfx", "reloading firmware on wake; discovered IGHardwareGuC - releasing");
		GuC->release();
		GuC = nullptr;
	}

	FunctionCast(wrapLoadFirmware, callback->orgLoadFirmware)(that);
}

bool Gen11::wrapInitSchedControl(void *that) {
	DBGLOG("ngreen", "attempting to init sched control with load %d", callback->performingFirmwareLoad);
	bool perfLoad = callback->performingFirmwareLoad;
	callback->performingFirmwareLoad = false;
	bool r = FunctionCast(wrapInitSchedControl, callback->orgInitSchedControl)(that);

	callback->performingFirmwareLoad = perfLoad;
	return r;
}

void *Gen11::wrapIgBufferWithOptions(void *accelTask, void* size, unsigned int type, unsigned int flags) {
	void *r = nullptr;

	if (callback->performingFirmwareLoad) {
		callback->dummyFirmwareBuffer = Buffer::create<uint8_t>(*(unsigned long*)size);

		const void *fw = nullptr;
		const void *fwsig = nullptr;
		size_t fwsize = 0;
		size_t fwsigsize = 0;


		/*fw = GuCFirmwareKBL;
		fwsig = GuCFirmwareKBLSignature;
		fwsize = GuCFirmwareKBLSize;
		fwsigsize = GuCFirmwareSignatureSize;*/

		unsigned long newsize = fwsize > *(unsigned long*)size ? ((fwsize + 0xFFFF) & (~0xFFFF)) : *(unsigned long*)size;
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, (void*)newsize,type,flags);
		if (r && callback->dummyFirmwareBuffer) {
			auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
			if (status == KERN_SUCCESS) {
				callback->realFirmwareBuffer = static_cast<uint8_t **>(r)[7];
				static_cast<uint8_t **>(r)[7] = callback->dummyFirmwareBuffer;
				lilu_os_memcpy(callback->realFirmwareBuffer, fw, fwsize);
				lilu_os_memcpy(callback->signaturePointer, fwsig, fwsigsize);
				callback->realBinarySize = static_cast<uint32_t>(fwsize);
				*callback->firmwareSizePointer = static_cast<uint32_t>(fwsize);
				MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			} else {
				//SYSLOG("igfx", "ig buffer protection upgrade failure %d", status);
			}
		} else if (callback->dummyFirmwareBuffer) {
			//SYSLOG("igfx", "ig shared buffer allocation failure");
			Buffer::deleter(callback->dummyFirmwareBuffer);
			callback->dummyFirmwareBuffer = nullptr;
		} else {
			//SYSLOG("igfx", "dummy buffer allocation failure");
		}
	} else {
		r = FunctionCast(wrapIgBufferWithOptions, callback->orgIgBufferWithOptions)(accelTask, size,type,flags);
	}

	return r;
}

UInt64 Gen11::wrapIgBufferGetGpuVirtualAddress(void *that) {
	if (callback->performingFirmwareLoad && callback->realFirmwareBuffer) {
		static_cast<uint8_t **>(that)[7] = callback->realFirmwareBuffer;
		callback->realFirmwareBuffer = nullptr;
		Buffer::deleter(callback->dummyFirmwareBuffer);
		callback->dummyFirmwareBuffer = nullptr;
	}

	return FunctionCast(wrapIgBufferGetGpuVirtualAddress, callback->orgIgBufferGetGpuVirtualAddress)(that);
}


uint32_t Gen11::wrapReadRegister32(void *controller, uint32_t address) {
	if (controller == nullptr)
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets

	// Mirror Apple bounds logic, but keep a fallback path for the 2D-only dropped window.
	auto partInfo = getMember<uint8_t *>(controller, 0xCF8);
	auto mmioBase = getMember<uint8_t *>(controller, 0x9B8);
	auto mmioSize = static_cast<uint32_t>(getMember<int>(controller, 0xC38));

	bool twoDOnlyPart = partInfo && ((partInfo[0xB2] & 0x1) != 0);
	bool dropped2DWindow = (address >= 0x2000U && address <= 0x23FFFFU);

	if (twoDOnlyPart && dropped2DWindow) {
		return NGreen::callback->readReg32(address);  // readReg32 now takes byte offsets
	}

	if (mmioBase && mmioSize >= 4U && address < (mmioSize - 4U)) {
		return *reinterpret_cast<volatile uint32_t *>(mmioBase + address);
	}

	return FunctionCast(wrapReadRegister32, callback->owrapReadRegister32)(controller, address);
}

void Gen11::wrapWriteRegister32(void *controller, uint32_t address, uint32_t value) {
	if (controller == nullptr) {
		NGreen::callback->writeReg32(address, value);
		return;
	}

	// V195W removed (was: OR-in bits 14,12 from UEFI's HSW_PWR_WELL_CTL1 onto every
	// 0x45400 write because the ICL DMC save/restore table allegedly clears them
	// periodically, supposedly stalling WindowServer's vsync). Pair-mate to V195 in
	// raWriteRegister32 + V195F in FastWriteRegister32 — all three sites stripped
	// together. The DMC save/restore behavior on Display 13 (ADL-P) differs from
	// Display 11 (ICL); enforcing UEFI's specific bits hides what Apple's stack
	// actually wants and what the ADL-P DMC table actually contains. If vsync stalls
	// re-emerge, fix should come from a correct DMC table or proper power-well init,
	// not bit OR-in.
	if (address == 0x45400 && NGreen::callback && !NGreen::callback->isRealTGL
	    && NGreen::callback->uefiCtl1 != 0) {
		static int v195WpCount = 0;
		if (v195WpCount < 6) {
			++v195WpCount;
			SYSLOG("ngreen", "V195Wp[%d]: CTL1 0x%x passthrough uefiCtl1=0x%x (V195W hack removed)",
			       v195WpCount, value, NGreen::callback->uefiCtl1);
		}
	}

	// V72W removed (was: force RCS/BCS RING_EMR writes to 0xFFFFFFFF — same blanket
	// error-mask hack as V72R, on the wrapWriteRegister32 helper path). Pair-mate to
	// the V72R passthrough already in place. Keeping the address-match shell so a
	// future legitimate intercept can use this hook point.
	if (address == 0x20b4 || address == 0x220b4) {
		static int v72WPassCount = 0;
		if (v72WPassCount < 6) {
			++v72WPassCount;
			SYSLOG("ngreen", "V72Wp[%d]: EMR @ 0x%x val=0x%x passthrough (V72W hack removed)",
			       v72WPassCount, address, value);
		}
	}

	// ── V63: Broad RCS register write intercept (diagnostic only, no modification) ──
	// Catches ANY write to RCS control range: ELSP, EXECLIST, CTX_CTRL, CCID, TAIL, etc.
	// Rate-limited to first 100 writes to avoid flooding Lilu buffer.
	if (!NGreen::callback->isRealTGL) {
		static int v63WriteCount = 0;
		// RCS engine MMIO range: 0x2000-0x2FFF covers all ring control registers
		if (address >= 0x2000 && address <= 0x2FFF) {
			if (v63WriteCount < 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W[%d]: RCS reg 0x%x = 0x%x", v63WriteCount, address, value);
			} else if (v63WriteCount == 100) {
				v63WriteCount++;
				SYSLOG("ngreen", "V63W: rate limit reached (100 RCS writes logged)");
			}
		}
	}

	FunctionCast(wrapWriteRegister32, callback->owrapWriteRegister32)(controller,address,value);
}

// V45: Delayed child check callback — runs on a kernel thread after a configurable delay.
// Logs the IOService state and child count at the specified time after start().
static void v45DelayedChildCheck(thread_call_param_t p0, thread_call_param_t p1) {
	if (!isV60MonitorEnabled())
		return;

	auto *svc = static_cast<IOService *>(p0);
	unsigned delayMs = (unsigned)(uintptr_t)p1;
	
	uint64_t state = svc->getState();
	OSIterator *iter = svc->getClientIterator();
	int count = 0;
	if (iter) {
		OSObject *obj;
		while ((obj = iter->getNextObject())) {
			auto *child = OSDynamicCast(IOService, obj);
			if (child) {
				uint64_t childState = child->getState();
				SYSLOG("ngreen", "V45: T+%ums child[%d]: %s class=%s state=0x%llx",
					   delayMs, count, child->getName(),
					   child->getMetaClass()->getClassName(),
					   (unsigned long long)childState);
				
				// V55: Enhanced IGAccelDevice diagnostics + force-start
				if (delayMs >= 3000) {
					const char *childName = child->getName();
					if (childName && (strcmp(childName, "IGAccelDevice") == 0 ||
									  strcmp(childName, "IGAccelSharedUserClient") == 0 ||
									  strcmp(childName, "IOAccelDisplayPipeUserClient2") == 0 ||
									  strcmp(childName, "IGAccelCommandQueue") == 0)) {
						// Dump child's provider and property state
						auto *childProvider = child->getProvider();
						SYSLOG("ngreen", "V55: %s state=0x%llx provider=%s isOpen=%d",
							   childName, (unsigned long long)childState,
							   childProvider ? childProvider->getName() : "NULL",
							   child->isOpen());
						
						// Check if the accelerator (our parent) is open
						SYSLOG("ngreen", "V55: accelerator isOpen=%d", svc->isOpen());
						
						if (childState == 0) {
							if (strcmp(childName, "IOAccelDisplayPipeUserClient2") == 0) {
								// Never call registerService on IOAccelDisplayPipeUserClient2.
								// Starting it triggers stamp-9 timeouts every ~5.4s:
								// AccessComplete is stubbed → stamp never advances →
								// GPURestartSignaled → WS compositor interrupted per cycle.
								// This applies in all modes, not just dp0.
								SYSLOG("ngreen", "V55: %s at state=0x0 — skip registerService (prevent stamp-9 timeout loop)", childName);
							} else {
								SYSLOG("ngreen", "V55: %s at state=0x0 — calling registerService()", childName);
								child->registerService(kIOServiceAsynchronous);
								IODelay(500);
								uint64_t newState = child->getState();
								SYSLOG("ngreen", "V55: %s after registerService → state=0x%llx",
									   childName, (unsigned long long)newState);
							}
						}
					}
				}
				count++;
			}
		}
		iter->release();
	}
	
	// Also check if the service is open (someone called IOServiceOpen on it)
	bool isOpen = svc->isOpen();
	
	SYSLOG("ngreen", "V45: T+%ums: %d children, state=0x%llx (reg=%d match=%d pub=%d fmatch=%d inact=%d), isOpen=%d",
		   delayMs, count,
		   (unsigned long long)state,
		   !!(state & 0x02), !!(state & 0x04), !!(state & 0x08), !!(state & 0x10), !!(state & 0x01),
		   isOpen);
	
	// Check provider's children too (siblings in IOAccelerator match category)
	auto *provider = svc->getProvider();
	if (provider) {
		OSIterator *sibs = provider->getClientIterator();
		if (sibs) {
			int sibIdx = 0;
			OSObject *s;
			while ((s = sibs->getNextObject())) {
				auto *sibSvc = OSDynamicCast(IOService, s);
				if (sibSvc) {
					SYSLOG("ngreen", "V45: T+%ums provider child[%d]: %s class=%s",
						   delayMs, sibIdx, sibSvc->getName(),
						   sibSvc->getMetaClass()->getClassName());
					sibIdx++;
				}
			}
			sibs->release();
		}
	}
}

// V45: Helper to schedule a delayed child check
static void v45ScheduleDelayedCheck(void *accelInstance, unsigned delayMs) {
	if (!isV60MonitorEnabled())
		return;

	thread_call_t tc = thread_call_allocate(v45DelayedChildCheck,
											static_cast<thread_call_param_t>(accelInstance));
	if (tc) {
		uint64_t deadline;
		clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
		thread_call_enter1_delayed(tc,
								   (thread_call_param_t)(uintptr_t)delayMs,
								   deadline);
	}
}

// V60: GPU health monitor — 2s interval with V57-proven R/W ERROR_GEN6 clear.
// V59 ReadRegister32 intercept caused regression (0 children). Back to timer-based.
// Also applies EMR mask-all every cycle to prevent Apple from unmasking errors.
void Gen11::v60GpuHealthMonitor(thread_call_param_t param0, thread_call_param_t param1) {
	if (!isV60MonitorEnabled())
		return;

	static int v60Count = 0;
	static uint32_t v60LastHead = 0xDEAD;
	static uint32_t v60LastTail = 0xDEAD;
	v60Count++;
	
	auto *svc = static_cast<IOService *>(param0);
	if (!svc) {
		SYSLOG("ngreen", "v60[%d]: null IOService, stopping health monitor", v60Count);
		return;
	}

	const bool v60Aggressive = isV60AggressiveMonitorEnabled();

	// Guard: bail if MMIO mapping has been released (GPU torn down before timer fired).
	// Release the retain we hold — stop the timer loop.
	if (!NGreen::callback->mmioValid()) {
		SYSLOG("ngreen", "v60[%d]: MMIO not ready, stopping health monitor", v60Count);
		svc->release();
		return;
	}

	// 1. Read GPU state via direct MMIO
	uint32_t realErr = NGreen::callback->readReg32(ERROR_GEN6);
	uint32_t rcsHead = NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE));
	uint32_t rcsTail = NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE));
	uint32_t rcsCtl  = NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE));
	uint32_t emr     = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
	
	// 2. ExecList state
	uint32_t execStatus = NGreen::callback->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
	uint32_t csbPtr = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
	
	// V63: Additional scheduler/context diagnostics
	uint32_t acthd    = NGreen::callback->readReg32(RING_ACTHD(RENDER_RING_BASE));
	uint32_t ccid     = NGreen::callback->readReg32(RING_CCID(RENDER_RING_BASE));
	uint32_t ringFault = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
	
	// 3. Track ring HEAD/TAIL movement
	bool headChanged = (rcsHead != v60LastHead);
	bool tailChanged = (rcsTail != v60LastTail);
	
	// 4. Count children + find IGAccelDevice state
	int childCount = 0;
	uint64_t accelDevState = 0xDEAD;
	/*if (v60Aggressive) {
		OSIterator *iter = svc->getClientIterator();
		if (iter) {
			OSObject *obj;
			while ((obj = iter->getNextObject())) {
				auto *child = OSDynamicCast(IOService, obj);
				if (child) {
					const char *cn = child->getName();
					if (cn && strcmp(cn, "IGAccelDevice") == 0) {
						accelDevState = child->getState();
					}
					childCount++;
				}
			}
			iter->release();
		}
	}*/
	
	// V70: Track child transitions — dump all children when count or accelDev state changes
	/*if (v60Aggressive) {
		static int lastChildCount = -1;
		static uint64_t lastAccelDevState = 0xBEEF;
		if (childCount != lastChildCount || accelDevState != lastAccelDevState) {
			SYSLOG("ngreen", "V70: TRANSITION ch %d->%d dev 0x%llx->0x%llx [iter %d]",
				   lastChildCount, childCount,
				   (unsigned long long)lastAccelDevState, (unsigned long long)accelDevState,
				   v60Count);
			// Dump ALL children at the transition point
			OSIterator *iter2 = svc->getClientIterator();
			if (iter2) {
				int idx = 0;
				OSObject *obj2;
				while ((obj2 = iter2->getNextObject())) {
					auto *child2 = OSDynamicCast(IOService, obj2);
					if (child2) {
						SYSLOG("ngreen", "V70: ch[%d]: %s cls=%s st=0x%llx",
							   idx, child2->getName(),
							   child2->getMetaClass()->getClassName(),
							   (unsigned long long)child2->getState());
						idx++;
					}
				}
				iter2->release();
			}
			// If IGAccelDevice just disappeared, dump GPU error state at that moment
			if (accelDevState == 0xDEAD && lastAccelDevState != 0xDEAD && lastAccelDevState != 0xBEEF) {
				SYSLOG("ngreen", "V70: *** IGAccelDevice LOST! Was dev=0x%llx ***",
					   (unsigned long long)lastAccelDevState);
				uint32_t errAtLoss = NGreen::callback->readReg32(ERROR_GEN6);
				uint32_t tlb0Loss  = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
				uint32_t tlb1Loss  = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
				uint32_t faultLoss = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
				SYSLOG("ngreen", "V70: at loss: ERR=0x%x TLB0=0x%x TLB1=0x%x FAULT=0x%x",
					   errAtLoss, tlb0Loss, tlb1Loss, faultLoss);
				// Dump GGTT PTEs 0-3 to check general GGTT health at this moment
				for (int pg = 0; pg < 4; pg++) {
					uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
					uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
					SYSLOG("ngreen", "V70: loss GGTT[%d]=0x%08x:%08x %s",
						   pg, pteHi, pteLo, (pteLo & 1) ? "V" : "INV");
				}
			}
			lastChildCount = childCount;
			lastAccelDevState = accelDevState;
		}
	}*/
	
	// V78/V77: DisplayPipe terminator — DISABLED.
	// V115+V116 now prevent the Blit3D MCE that caused GPU rendering failure.
	// Killing IOAccelDisplayPipeUserClient2 every 2s blocks WindowServer's main
	// thread mid-IOKit-call → userspace watchdog fires at T+40s and T+80s.
	// Re-enable only if GPU rendering is still broken after V115/V116 testing.
	/*
	{
		const int v77KillDelay = getV77KillDelayIterations();
		if (v77KillDelay > 0 && v60Count <= v77KillDelay) {
			SYSLOG("ngreen", "V77: kill delayed at iter %d (ngreenV77DelayKill=%d)",
			       v60Count, v77KillDelay);
		} else {
		OSIterator *dpIter = svc->getClientIterator();
		if (dpIter) {
			OSObject *dpObj;
			int dpKilled = 0;
			while ((dpObj = dpIter->getNextObject())) {
				auto *dpChild = OSDynamicCast(IOService, dpObj);
				if (dpChild) {
					const char *dpCn = dpChild->getMetaClass()->getClassName();
					if (dpCn && strstr(dpCn, "DisplayPipe")) {
						SYSLOG("ngreen", "V77: TERMINATING %s (st=0x%llx) to prevent WS GPU compositing crash",
							   dpCn, (unsigned long long)dpChild->getState());
						dpChild->terminate(kIOServiceRequired);
						dpKilled++;
					}
				}
			}
			dpIter->release();
			if (dpKilled > 0) {
				SYSLOG("ngreen", "V77: Killed %d display pipe client(s) at iter %d", dpKilled, v60Count);
			}
		}
		}
	}
	*/

	// 5. V60: error suppression path. Default is read-only for stability.
	// Use -ngreenv60rw to enable active register writes during monitor ticks.
	/*if (v60Aggressive) {
		if (realErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		}
		// Re-mask EMR every cycle — Apple may unmask during error recovery attempts
		if (emr != 0xFFFFFFFF) {
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		}
		
		// ── V79: Plane linearization (brief Apple logo flash) — aggressive mode only ──
		// Boot log shows SURF=0x0 for iters 1–6; SURF becomes non-zero (tiled) at iter 7 (~14s).
		// Old cadence (<=5, 10, 20, 30) skipped iter 7/8/9 — the first 6s window where the plane
		// is tiled. Writing at iter 10 (~20s) is too late: the login screen is already up and the
		// Apple logo phase has ended. Fix: fire every tick from 1–12 so we catch iter 7 immediately
		// (first time SURF is mapped and tiling != 0) while the Apple logo may still be on screen.
		if (v60Count <= 5 || v60Count == 10 || v60Count == 20 || v60Count == 30) {
			uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			if (tiling != 0) {
				uint32_t newCtl = planCtl & ~(0x7u << 10); // clear tiling -> linear
				uint32_t newStrd = planStrd;
				if (tiling == 1) {
					newStrd = planStrd * 8;
				} else if (tiling == 4) {
					newStrd = planStrd * 16;
				}
				NGreen::callback->writeReg32(0x70180, newCtl);
				NGreen::callback->writeReg32(0x70188, newStrd);
				uint32_t planSurf = NGreen::callback->readReg32(0x7019C);
				NGreen::callback->writeReg32(0x7019C, planSurf);
				SYSLOG("ngreen", "V79[%d]: tiling %d->linear CTL 0x%x->0x%x STRIDE 0x%x->0x%x SURF=0x%x",
					   v60Count, tiling, planCtl, newCtl, planStrd, newStrd, planSurf);
			} else {
				SYSLOG("ngreen", "V79[%d]: already linear CTL=0x%x STRIDE=0x%x", v60Count, planCtl, planStrd);
			}
		}
	}*/

	// 5. V60: ACTIVE error suppression (V57 proven approach)
	//    Keep this write path strictly opt-in via -ngreenv60rw.
	/*if (v60Aggressive) {
		if (realErr) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
		}
		// Re-mask EMR every cycle — Apple may unmask during error recovery attempts
		if (emr != 0xFFFFFFFF) {
			NGreen::callback->writeReg32(RING_EMR(RENDER_RING_BASE), 0xFFFFFFFF);
		}
	}*/
	
	// 6. Log with ring + CSB focus
	uint32_t postErr = NGreen::callback->readReg32(ERROR_GEN6);
	uint32_t gtDw0   = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
	uint32_t rcIntr  = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
	SYSLOG("ngreen", "V60M[%d]: ERR=0x%x->0x%x HEAD=0x%x TAIL=0x%x CTL=0x%x EMR=0x%x EXEC=0x%x CSB=0x%x ch=%d dev=0x%llx%s%s",
		   v60Count, realErr, postErr, rcsHead, rcsTail, rcsCtl, emr,
		   execStatus, csbPtr, childCount, (unsigned long long)accelDevState,
		   headChanged ? " HEAD_MOVED!" : "",
		   tailChanged ? " TAIL_MOVED!" : "");
	SYSLOG("ngreen", "V66M[%d]: ACTHD=0x%x CCID=0x%x FAULT=0x%x GT_DW0=0x%x RC_EN=0x%x",
		   v60Count, acthd, ccid, ringFault, gtDw0, rcIntr);
	// V68: Log TLB fault data when ERROR_GEN6 fires — shows faulting address
	if (realErr) {
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		SYSLOG("ngreen", "V68F[%d]: ERR=0x%x TLB0=0x%x TLB1=0x%x", v60Count, realErr, tlb0, tlb1);
	}
	
	// ── V64: CSB buffer dump + interrupt mask diagnostics + CSB read pointer advance ──
	if (v60Count == 1) {
		// Dump all 6 CSB entries (context status buffer) — GPU writes these during context switches
		for (int i = 0; i < 6; i++) {
			uint32_t csbHi = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF_HI(RENDER_RING_BASE, i));
			uint32_t csbLo = NGreen::callback->readReg32(RING_CONTEXT_STATUS_BUF(RENDER_RING_BASE, i));
			SYSLOG("ngreen", "V64: CSB[%d] = 0x%x:0x%x", i, csbHi, csbLo);
		}
		// Dump interrupt enable + mask registers for RCS
		uint32_t rcIntrEn   = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsIntrMask = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t gtIntrDw0  = NGreen::callback->readReg32(GEN11_GT_INTR_DW0);
		uint32_t mstrIrq    = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V64: RC_INTR_EN=0x%x RCS_INTR_MASK=0x%x GT_DW0=0x%x MSTR=0x%x",
			   rcIntrEn, rcsIntrMask, gtIntrDw0, mstrIrq);
		// Dump identity registers
		uint32_t ident0 = NGreen::callback->readReg32(GEN11_INTR_IDENTITY_REG0);
		uint32_t ident1 = NGreen::callback->readReg32(GEN11_INTR_IDENTITY_REG1);
		SYSLOG("ngreen", "V64: INTR_IDENT0=0x%x INTR_IDENT1=0x%x", ident0, ident1);
		
		// ── V67: Enhanced scheduler + workloop + event source diagnostics ──
		// ── V68: + fault TLB data, e08obj class, early sched clear ──
		{
			// V68: Read fault detail registers (TLB data gives faulting address)
			uint32_t tlbData0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
			uint32_t tlbData1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
			uint32_t gucStatus = NGreen::callback->readReg32(0xC000); // GUC_STATUS
			uint32_t rcsEir = NGreen::callback->readReg32(RENDER_RING_BASE + 0xB0); // RING_EIR
			SYSLOG("ngreen", "V68: TLB_DATA0=0x%x TLB_DATA1=0x%x GUC=0x%x EIR=0x%x",
				tlbData0, tlbData1, gucStatus, rcsEir);

			uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
			if (schedPtr) {
				SYSLOG("ngreen", "V67: scheduler ptr=0x%llx", (unsigned long long)schedPtr);
				auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
				// V67: Expanded dump — 0x000-0x400 (128 QWORDs)
				for (int i = 0; i < 128; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(schedBase + i * 8);
					if (val != 0) {  // Only log non-zero to reduce noise
						SYSLOG("ngreen", "V67: sched[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
					}
				}
				
				// V68: Clear sched error EARLY (don't wait for iteration 5).
				// Clearing an error flag is safe regardless of V60 mode — the scheduler
				// error 0x7b blocks all GPU submissions and must be cleared on boot.
				uint32_t schedErr = *reinterpret_cast<uint32_t *>(schedBase + 0x0C);
				if (schedErr) {
					*reinterpret_cast<uint32_t *>(schedBase + 0x0C) = 0;
					*reinterpret_cast<uint32_t *>(schedBase + 0x00) = 0;
					SYSLOG("ngreen", "V68: early sched clear err=0x%x->0x%x flag=0x0",
						schedErr, *reinterpret_cast<uint32_t *>(schedBase + 0x0C));
				}
			} else {
				SYSLOG("ngreen", "V67: scheduler ptr is NULL!");
			}
			
			// V67: Dump object at accel[0xe08] — may be the actual scheduler C++ object
			uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
			if (schedObj) {
				auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
				// First 8 bytes should be vtable pointer for a C++ object
				uint64_t vtable = *reinterpret_cast<uint64_t *>(objBase);
				// V68: Identify class via getMetaClass()
				auto *e08Obj = reinterpret_cast<OSObject *>(schedObj);
				const char *e08Class = "?";
				if (e08Obj->getMetaClass()) {
					e08Class = e08Obj->getMetaClass()->getClassName();
				}
				SYSLOG("ngreen", "V68: accel[0xe08] obj=0x%llx vtable=0x%llx class=%s",
					(unsigned long long)schedObj, (unsigned long long)vtable, e08Class);
				// Dump first 256 bytes, non-zero only
				for (int i = 0; i < 32; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(objBase + i * 8);
					if (val != 0) {
						SYSLOG("ngreen", "V68: e08obj[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
					}
				}
			}
			
			// V67: Dump objects at accel[0xe10..0xe38] — identify by vtable class
			for (int idx = 0; idx < 6; idx++) {
				uint64_t objPtr = getMember<uint64_t>(svc, 0xe10 + idx * 8);
				if (objPtr) {
					auto *obj = reinterpret_cast<OSObject *>(objPtr);
					const char *clsName = obj->getMetaClass() ? obj->getMetaClass()->getClassName() : "?";
					SYSLOG("ngreen", "V67: accel[0x%03x]=0x%llx class=%s",
						0xe10 + idx * 8, (unsigned long long)objPtr, clsName);
				}
			}
			
			// V67: Workloop event source chain enumeration
			auto *wl = svc->getWorkLoop();
			SYSLOG("ngreen", "V67: workLoop=%s", wl ? "EXISTS" : "NULL");
			if (wl) {
				SYSLOG("ngreen", "V67: workLoop class=%s ptr=0x%llx",
					wl->getMetaClass()->getClassName(), (unsigned long long)wl);
				// Dump workloop object — first 128 bytes to find event source chain
				auto *wlBase = reinterpret_cast<uint8_t *>(wl);
				for (int i = 0; i < 16; i++) {
					uint64_t val = *reinterpret_cast<uint64_t *>(wlBase + i * 8);
					SYSLOG("ngreen", "V67: wl[0x%02x]=0x%016llx", i * 8, (unsigned long long)val);
				}
				
				// V67: Walk event source chain from workloop dump.
				// IOWorkLoop layout: [0x00]=vtable [0x08-0x10]=OSObject fields
				// [0x18]=gateLock [0x20]=eventChain [0x28]=controlG [0x30]=workToDo etc.
				// Read eventChain pointer (offset ~0x20 or thereabouts in the dump above)
				// and follow the chain manually.
				uint64_t eventChain = *reinterpret_cast<uint64_t *>(wlBase + 0x20);
				SYSLOG("ngreen", "V67: wl eventChain=0x%016llx", (unsigned long long)eventChain);
				int esIdx = 0;
				uint64_t esPtr = eventChain;
				while (esPtr && esIdx < 16) {
					auto *esObj = reinterpret_cast<OSObject *>(esPtr);
					const char *esClass = "?";
					if (esObj->getMetaClass()) {
						esClass = esObj->getMetaClass()->getClassName();
					}
					SYSLOG("ngreen", "V67: eventSource[%d]: class=%s ptr=0x%016llx",
						esIdx, esClass, (unsigned long long)esPtr);
					// IOEventSource::eventChainNext is typically at offset 0x10
					auto *esBase = reinterpret_cast<uint8_t *>(esPtr);
					esPtr = *reinterpret_cast<uint64_t *>(esBase + 0x10);
					esIdx++;
				}
				SYSLOG("ngreen", "V67: workLoop has %d event sources in chain", esIdx);
			}
			
			// V67: Key accelerator fields
			for (int i = 0; i < 8; i++) {
				uint64_t val = getMember<uint64_t>(svc, 0xe00 + i * 8);
				SYSLOG("ngreen", "V67: accel[0x%03x]=0x%016llx", 0xe00 + i * 8, (unsigned long long)val);
			}
			
			// V70: Read ACTUAL GGTT PTEs from correct base (0x800000, not the 0x100000
			// fence registers that V68 used). Each PTE is 8 bytes (64-bit).
			// PTE format: phys_addr[47:12] | flags[11:0], bit 0 = VALID/PRESENT.
			SYSLOG("ngreen", "V70: GGTT PTE dump (base=0x%x):", GEN8_GGTT_PTE_BASE);
			for (int pg = 0; pg < 16; pg++) {
				uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
				uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
				SYSLOG("ngreen", "V70: GGTT[%2d] = 0x%08x:%08x %s",
					   pg, pteHi, pteLo, (pteLo & 1) ? "VALID" : "INVALID");
			}
			// Check pages around stolen memory boundary (128MB = 0x8000 pages)
			for (int i = 0; i < 4; i++) {
				int pg = 0x7FFE + i;
				uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(pg));
				uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(pg));
				SYSLOG("ngreen", "V70: GGTT[0x%x] = 0x%08x:%08x %s",
					   pg, pteHi, pteLo, (pteLo & 1) ? "VALID" : "INVALID");
			}
			// MMIO BAR length — tells us if GGTT PTE access is direct or indirect
			SYSLOG("ngreen", "V70: rmmio length=0x%llx (GGTT direct=%s)",
				   (unsigned long long)NGreen::callback->rmmio->getLength(),
				   (GEN8_GGTT_PTE_BASE + 16*8 <= NGreen::callback->rmmio->getLength()) ? "yes" : "no(indirect)");
		}
	}
	
	// V64: On iteration 3, advance CSB read pointer to match write pointer
	// CSB ptr format: bits[10:8]=write ptr (GPU), bits[2:0]=read ptr (software)
	// If write > read, scheduler is stuck waiting for CSB processing
	if (v60Count == 3) {
		uint32_t csbPtrNow = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
		uint32_t writeIdx = (csbPtrNow >> 8) & 0x7;
		uint32_t readIdx  = csbPtrNow & 0x7;
		SYSLOG("ngreen", "V64: CSB ptr=0x%x write=%d read=%d", csbPtrNow, writeIdx, readIdx);
		if (writeIdx != readIdx && v60Aggressive) {
			// Advance read pointer to match write pointer
			uint32_t newPtr = (csbPtrNow & ~0x7) | (writeIdx & 0x7);
			NGreen::callback->writeReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE), newPtr);
			uint32_t verify = NGreen::callback->readReg32(RING_CONTEXT_STATUS_PTR(RENDER_RING_BASE));
			SYSLOG("ngreen", "V64: CSB read ptr advanced %d->%d (wrote 0x%x, readback 0x%x)",
				   readIdx, writeIdx, newPtr, verify);
		} else if (writeIdx != readIdx) {
			SYSLOG("ngreen", "V64: read-only mode (use -ngreenv60rw to advance CSB read ptr)");
		} else {
			SYSLOG("ngreen", "V64: CSB ptrs already matched (write=read=%d)", writeIdx);
		}
	}
	
	// V65: Enforce RCS0 interrupt enable on EVERY iteration (moved from V64 iteration 5).
	// V64 proved the fix at T+10s was too late — scheduler already gave up.
	// Now combined with V65 pre-start + V65W watchdog fixes for full coverage.
	/*if (v60Aggressive) {
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsMask  = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		
		// Tier-1 IRQ policy: RCS only on spoofed non-TGL by default.
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		if ((rcIntrEn & wantBits) != wantBits) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-1 fix 0x%x->0x%x", v60Count, rcIntrEn,
					NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
			}
		}
		// Unmask context-switch + user interrupt for RCS (0 = unmasked, 1 = masked)
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-2 fix mask 0x%x->0x%x", v60Count, rcsMask, newMask);
			}
		}
	}*/

	// V65: Enforce RCS0 interrupt enable on EVERY iteration (moved from V64 iteration 5).
	// V64 proved the fix at T+10s was too late — scheduler already gave up.
	// Now combined with V65 pre-start + V65W watchdog fixes for full coverage.
	/*if (v60Aggressive) {
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t rcsMask  = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		
		// Tier-1 IRQ policy follows V65 helper (RCS-only on spoof path unless V142 orig/BCS requested).
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		if ((rcIntrEn & wantBits) != wantBits) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-1 fix 0x%x->0x%x", v60Count, rcIntrEn,
					NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
			}
		}
		// Unmask context-switch + user interrupt for RCS (0 = unmasked, 1 = masked)
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			if (v60Count <= 3) {
				SYSLOG("ngreen", "V65M[%d]: tier-2 fix mask 0x%x->0x%x", v60Count, rcsMask, newMask);
			}
		}
	}*/
	
	// ── V79: Plane monitor (strictly read-only) ──
	// Any plane-format writes here have caused either WS crash loops or WS hangs.
	// Keep this path logging-only; never modify PLANE_CTL/STRIDE/SURF in V60.
	/*{
		uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos  = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t pipeConf = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]

		// Always log plane state for diagnostics
		SYSLOG("ngreen", "V79[%d]: PIPE=0x%x CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x tiling=%d",
			   v60Count, pipeConf, planCtl, planStrd, planSurf, planSize, planPos, tiling);
	}*/

	// ── V79: Plane monitor / experimental linearization ──
	// The old copy only reached brief first-light when the experimental path forced
	// the scanout plane to linear and re-armed PLANE_SURF. Keep the default path
	// read-only; restore the old behavior only with -ngreenexp.
	/*if (isExperimentalMonitorEnabled() && v60Aggressive ) {
		if (v60Count <= 5 || v60Count == 10 || v60Count == 20 || v60Count == 30) {
			uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C);
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			// V99S already ensures STRIDE=0xa0 and CTL=Y-tiled before every SURF arm.
			// Do NOT re-convert tiling or recalculate stride (those are now correct).
			// Just enforce correct values and re-arm SURF to keep the display engine live.
			// Skip if SURF=0 (plane disabled — arming a null surface causes garbage).
			if (planSurf != 0) {
				uint32_t neededStrd = 0xa0;
				uint32_t neededCtl  = (planCtl & ~(0x7u << 10)) | (0x4u << 10); // Y-tiled
				bool strdWrong  = (planStrd != neededStrd);
				bool tilingWrong = (tiling != 4);
				if (strdWrong)   NGreen::callback->writeReg32(0x70188, neededStrd);
				if (tilingWrong) NGreen::callback->writeReg32(0x70180, neededCtl);
				NGreen::callback->writeReg32(0x7019C, planSurf); // re-arm
				SYSLOG("ngreen", "V79RW[%d]: CTL=0x%x->0x%x STRIDE=0x%x->0x%x SURF=0x%x",
					   v60Count,
					   planCtl,  (tilingWrong ? neededCtl  : planCtl),
					   planStrd, (strdWrong   ? neededStrd : planStrd),
					   planSurf);
			} else {
				SYSLOG("ngreen", "V79RW[%d]: SURF=0 skip. CTL=0x%x STRIDE=0x%x tiling=%d",
					   v60Count, planCtl, planStrd, tiling);
			}
		}
	} else {
		uint32_t planCtl  = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos  = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t pipeConf = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
		SYSLOG("ngreen", "V79[%d]: PIPE=0x%x CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x tiling=%d",
			   v60Count, pipeConf, planCtl, planStrd, planSurf, planSize, planPos, tiling);

		// V105B: same blanking-pedestal check as V105, applied here after V79 reads SURF.
		if (planSurf != 0) {
			NGreen::callback->writeReg32(0x4A400, 0);    // index 0
			uint32_t g0b  = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, 128);
			uint32_t g128 = NGreen::callback->readReg32(0x4A404);
			if (g0b == g128 || g128 == 0) {
				static int v105bCount = 0;
				if (v105bCount < 10) {
					v105bCount++;
					SYSLOG("ngreen", "V105B[%d]: Gamma bad (iter %d, SURF=0x%x g0=0x%x g128=0x%x) — linear LUT",
					       v105bCount, v60Count, planSurf, g0b, g128);
				}
				NGreen::callback->writeReg32(0x4A400, 0x8000);
				for (int i = 0; i < 256; i++) {
					uint32_t v10 = (uint32_t)i * 4;
					NGreen::callback->writeReg32(0x4A404, (v10 << 20) | (v10 << 10) | v10);
				}
			}
		}
	}*/

	// ── V75: Display pipeline register dump — diagnose black screen ──
	// System stays alive but display is black. Read pipe/plane/transcoder/backlight
	// registers to understand what state the display hardware is in.
	if (v60Count == 3 || v60Count == 30) {
		// Pipe A config
		uint32_t pipeConf  = NGreen::callback->readReg32(0x70008); // PIPE_CONF_A
		uint32_t pipeSrc   = NGreen::callback->readReg32(_PIPEASRC);
		// Plane 1 on Pipe A
		uint32_t planCtl   = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		uint32_t planStrd  = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
		uint32_t planSurf  = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t planSize  = NGreen::callback->readReg32(0x70190); // PLANE_SIZE
		uint32_t planPos   = NGreen::callback->readReg32(0x7018C); // PLANE_POS
		uint32_t planOff   = NGreen::callback->readReg32(0x70194); // PLANE_OFFSET
		// Transcoder A
		uint32_t transConf = NGreen::callback->readReg32(0x60008); // TRANS_CONF_A (or PIPE_CONF for ICL+)
		uint32_t transH    = NGreen::callback->readReg32(0x60000); // HTOTAL_A
		uint32_t transV    = NGreen::callback->readReg32(0x6000C); // VTOTAL_A
		// DDI / port
		uint32_t ddiFuncA  = NGreen::callback->readReg32(0x60400); // DDI_FUNC_CTL_A (trans EDP on TGL)
		uint32_t ddiFunc1  = NGreen::callback->readReg32(0x60100); // DDI_FUNC_CTL_1 (trans A)
		// Backlight
		uint32_t blcPwm    = NGreen::callback->readReg32(0xC8250); // BLC_PWM_CTL2
		uint32_t blcDuty   = NGreen::callback->readReg32(0xC8254); // BLC_PWM_DATA / duty cycle
		uint32_t sblcPwm   = NGreen::callback->readReg32(0xC8254); // South BLC_PWM_CTL
		// Power wells
		uint32_t pwrWell   = NGreen::callback->readReg32(0x45400); // PWR_WELL_CTL2 (ICL+)
		uint32_t dcState   = NGreen::callback->readReg32(0x45504); // DC_STATE_EN

		SYSLOG("ngreen", "V75[%d]: PIPE_CONF=0x%x PIPE_SRC=0x%x", v60Count, pipeConf, pipeSrc);
		SYSLOG("ngreen", "V75[%d]: PLANE_CTL=0x%x STRIDE=0x%x SURF=0x%x SIZE=0x%x POS=0x%x OFF=0x%x",
			   v60Count, planCtl, planStrd, planSurf, planSize, planPos, planOff);
		SYSLOG("ngreen", "V75[%d]: TRANS_CONF=0x%x HTOTAL=0x%x VTOTAL=0x%x",
			   v60Count, transConf, transH, transV);
		SYSLOG("ngreen", "V75[%d]: DDI_FUNC_EDP=0x%x DDI_FUNC_A=0x%x",
			   v60Count, ddiFuncA, ddiFunc1);
		SYSLOG("ngreen", "V75[%d]: BLC_PWM=0x%x BLC_DUTY=0x%x SBLC=0x%x",
			   v60Count, blcPwm, blcDuty, sblcPwm);
		SYSLOG("ngreen", "V75[%d]: PWR_WELL=0x%x DC_STATE=0x%x",
			   v60Count, pwrWell, dcState);

		// V103P removed (was: every V60 tick, force DC_STATE_EN back to 0 when ADL-P
		// DMC is active — third belt of the V103 trio). Now read-only: log the value
		// when non-zero so we can see when Apple's DMC actually wants to enter a DC
		// state. With V103 raWriteRegister32-side and V103F FastWrite-side also off,
		// this is the full "no DC_STATE_EN block" experiment.
		if (NGreen::callback->dmcIsAdlp && dcState != 0)
			NGreen::callback->adlpDcExit("V60");
		// V105: Pipe-A gamma LUT enforcement — write linear pass-through when LUT is bad.
		// The Apple driver enables precision gamma mode before WindowServer writes the actual
		// LUT.  Two bad states are observed:
		//   zero LUT (g[128]=0): all pixels map to black
		//   blanking pedestal (g[128]=0x8020080, R=G=B=128/1023): uniform dim grey —
		//   the Apple IGFB power-management code sets this during display blanking and
		//   does not clear it before WindowServer takes over.  Both states cause the
		//   display to appear black/dark regardless of framebuffer content.
		// Detect: sample entries 0 and 128; if they are equal (constant LUT = blanking)
		// or both are zero, write a linear 8→10-bit pass-through until WS initialises.
		// PAL_PREC_DATA (0x4A404) format: bits[29:20]=red, [19:10]=green, [9:0]=blue.
		{
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF_A
			if (planSurf != 0) {
				NGreen::callback->writeReg32(0x4A400, 0);    // PAL_PREC_INDEX = 0
				uint32_t g0   = NGreen::callback->readReg32(0x4A404);
				NGreen::callback->writeReg32(0x4A400, 128);  // PAL_PREC_INDEX = 128
				uint32_t g128 = NGreen::callback->readReg32(0x4A404);
				// Constant LUT (all entries equal) = blanking or zero = bad state
				if (g0 == g128 || g128 == 0) {
					static int v105Count = 0;
					if (v105Count < 10) {
						v105Count++;
						SYSLOG("ngreen", "V105[%d]: Gamma bad at iter %d (g0=0x%x g128=0x%x) — linear pass-through",
						       v105Count, v60Count, g0, g128);
					}
					// Write linear LUT: set index=0 + AUTO_INCREMENT (bit 15 = 0x8000)
					NGreen::callback->writeReg32(0x4A400, 0x8000);
					for (int i = 0; i < 256; i++) {
						uint32_t v10 = (uint32_t)i * 4;
						NGreen::callback->writeReg32(0x4A404, (v10 << 20) | (v10 << 10) | v10);
					}
				}
			}
		}

		// V104P removed (was: V60-tick log "V195 miss?" if CTL1 != uefiCtl1 | 0x401).
		// V195 is gone (passthrough), so the "miss" framing is meaningless. The V60 V75
		// SYSLOG above already prints PWR_WELL=0x%x every tick, so CTL1 drift is observable
		// without this duplicate log line.
	}

	// ── V76: Deep display diagnostic + framebuffer physical write test ──
	// V75 proved: ALL display regs are correctly configured (pipe, plane, DDI, backlight ON).
	// Black screen must be because SURF=0x412ad000 points to unwritten (zeroed) memory.
	// Verify GGTT mapping, check WM/PSR, and write a white test pattern directly
	// to the framebuffer surface physical memory to prove the display pipeline works.
	if (v60Count == 5) {
		// 1. Read PLANE_SURF to get current surface GGTT address
		uint32_t surfAddr = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
		uint32_t surfPage = surfAddr >> 12;  // GGTT page number

		// 2. Read GGTT PTE for this page — verify mapping is valid
		uint32_t pteLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage));
		uint32_t pteHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage));
		uint64_t pte = ((uint64_t)pteHi << 32) | pteLo;
		uint64_t physAddr = pte & 0x0000FFFFFFFFF000ULL;
		bool pteValid = (pteLo & 0x1) != 0;
		SYSLOG("ngreen", "V76[%d]: SURF=0x%x page=0x%x PTE=0x%x:%08x phys=0x%llx valid=%d",
			   v60Count, surfAddr, surfPage, pteHi, pteLo,
			   (unsigned long long)physAddr, pteValid);

		// Also check a few neighboring GGTT pages for the surface
		for (int i = 1; i <= 3; i++) {
			uint32_t nLo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage + i));
			uint32_t nHi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage + i));
			uint64_t nPhys = (((uint64_t)nHi << 32) | nLo) & 0x0000FFFFFFFFF000ULL;
			SYSLOG("ngreen", "V76[%d]: SURF+%d PTE=0x%x:%08x phys=0x%llx v=%d",
				   v60Count, i, nHi, nLo, (unsigned long long)nPhys, nLo & 1);
		}

		// 3. Watermark registers — if wrong, the plane gets no FIFO bandwidth → blank
		uint32_t wm0 = NGreen::callback->readReg32(0x70240); // CUR_WM_A_0 (WM level 0)
		uint32_t wm1 = NGreen::callback->readReg32(0x70244); // CUR_WM_A_1
		uint32_t planWm0 = NGreen::callback->readReg32(0x70268); // PLANE_WM_A_0 (plane 1 WM level 0)
		uint32_t planWm1 = NGreen::callback->readReg32(0x7026C); // PLANE_WM_A_1
		uint32_t planWmT = NGreen::callback->readReg32(0x70278); // PLANE_WM_TRANS (transition WM)
		SYSLOG("ngreen", "V76[%d]: WM cur0=0x%x cur1=0x%x plan0=0x%x plan1=0x%x planT=0x%x",
			   v60Count, wm0, wm1, planWm0, planWm1, planWmT);

		// 4. PSR—Panel Self-Refresh: if active and confused, panel may show stale/black
		uint32_t psrCtl  = NGreen::callback->readReg32(0x64800); // EDP_PSR_CTL
		uint32_t psrSts  = NGreen::callback->readReg32(0x64840); // EDP_PSR_STATUS
		uint32_t psrCtl2 = NGreen::callback->readReg32(0x60900); // EDP_PSR2_CTL
		SYSLOG("ngreen", "V76[%d]: PSR_CTL=0x%x PSR_STATUS=0x%x PSR2_CTL=0x%x",
			   v60Count, psrCtl, psrSts, psrCtl2);

		// 5. Gamma LUT sample — if gamma table is zeroed, all output = black.
		// In read-only mode keep current index untouched and only sample current entry.
		uint32_t gammaIdx = NGreen::callback->readReg32(0x4A400); // PREC_PAL_INDEX_A
		uint32_t gamma0 = 0;
		uint32_t gamma128 = 0;
		uint32_t gamma255 = 0;
		if (v60Aggressive) {
			NGreen::callback->writeReg32(0x4A400, 0);                 // Set index to 0
			gamma0 = NGreen::callback->readReg32(0x4A404);            // PREC_PAL_DATA_A
			NGreen::callback->writeReg32(0x4A400, 128);               // Mid-range index
			gamma128 = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, 255);               // Near max index
			gamma255 = NGreen::callback->readReg32(0x4A404);
			NGreen::callback->writeReg32(0x4A400, gammaIdx);          // Restore original index
		} else {
			gamma0 = NGreen::callback->readReg32(0x4A404);
			gamma128 = gamma0;
			gamma255 = gamma0;
		}
		SYSLOG("ngreen", "V76[%d]: GAMMA idx_was=0x%x g[0]=0x%x g[128]=0x%x g[255]=0x%x",
			   v60Count, gammaIdx, gamma0, gamma128, gamma255);

		// (V105 now runs every tick in the periodic section above)

		// 6. Framebuffer physical read test — read pixels from the display surface
		// to check current content. V83 handles filling; V76 only reads now.
		if (pteValid && physAddr != 0) {
			auto *desc = IOMemoryDescriptor::withPhysicalAddress(
				(IOPhysicalAddress)physAddr, 4096, kIODirectionIn);
			if (desc) {
				auto *map = desc->createMappingInTask(kernel_task, 0,
					kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
				if (map) {
					volatile uint32_t *fb = (volatile uint32_t *)map->getVirtualAddress();
					SYSLOG("ngreen", "V76[%d]: READ px[0]=0x%x px[512]=0x%x px[1023]=0x%x phys=0x%llx",
						   v60Count, fb[0], fb[512], fb[1023],
						   (unsigned long long)physAddr);
					map->release();
				}
				desc->release();
			}
		} else {
			SYSLOG("ngreen", "V76[%d]: SURF PTE invalid — cannot read framebuffer", v60Count);
		}
	}

	// ── V84: Page-by-page framebuffer fill — persistent, no GGTT modification ──
	// V82 proved: display engine reads from PLANE_SURF via GGTT (magenta visible).
	// V83 bug: SURF redirect to GGTT page 0 → MCE at phys 0x3e800000 (stolen mem base).
	// MC Bank 11 (GPU) uncorrected error on all 20 CPUs. NEVER touch stolen base.
	// V88: Color band diagnostic + GGTT TLB flush + plane toggle + transcoder probe
	// V87 PROVED: SURF==SURFLIVE always MATCH. Only Plane 1 active. PipeB OFF.
	// 4000 pages filled, magenta persists. Display still shows bars.
	// PARADOX: buffer is correct, plane reads it, yet bars appear.
	// V88: Fill 4 color bands (RED/GREEN/BLUE/WHITE) to test if display reads our
	// data at all. Flush GGTT TLB. Toggle plane off/on. Probe transcoders.

	// V88 scanout fill is intentionally opt-in: it paints debug bars/colors and can
	// override normal Apple UI composition. Enable only for diagnostics with -ngreenv88.
	// In dp0 mode V88 is harmful: readReg32(PLANE_SURF) returns the hardware echo of
	// setupScanoutMemory's 0x412be000 write (before V99S redirects it), causing V88[5]'s
	// plane toggle to arm with the wrong SURF and fill non-aperture pages with color bands.
	if (isExperimentalMonitorEnabled() && isV88ScanoutFillEnabled() && v60Count >= 1 && v60Count <= 30) {
		uint32_t surfAddr = NGreen::callback->readReg32(0x7019C);
		uint32_t surfPage = surfAddr >> 12;

		// 1. Iteration 1-3: Deep transcoder & pipe diagnostic
		if (v60Count <= 3) {
			// Check ALL transcoder configs (A, B, C, D, EDP)
			uint32_t transAconf  = NGreen::callback->readReg32(0x60008);
			uint32_t transBconf  = NGreen::callback->readReg32(0x61008);
			uint32_t transEDPconf = NGreen::callback->readReg32(0x6F008);
			SYSLOG("ngreen", "V88[%d]: TransA=0x%x TransB=0x%x TransEDP=0x%x",
				   v60Count, transAconf, transBconf, transEDPconf);

			// DDI function control for each transcoder
			uint32_t transA_ddi = NGreen::callback->readReg32(0x60400);  // TRANS_DDI_FUNC_CTL_A
			uint32_t transEDP_ddi = NGreen::callback->readReg32(0x6F400); // TRANS_DDI_FUNC_CTL_EDP
			SYSLOG("ngreen", "V88[%d]: TransA_DDI=0x%x TransEDP_DDI=0x%x",
				   v60Count, transA_ddi, transEDP_ddi);

			// Pipe status — check for underruns
			uint32_t pipeStatA = NGreen::callback->readReg32(0x70024);  // PIPE_STATUS_A (or PIPEASTAT)
			// DSB control
			uint32_t dsbCtl = NGreen::callback->readReg32(0x70840);  // DSB_CTRL pipe A
			// PLANE_COLOR_CTL
			uint32_t planeColorCtl = NGreen::callback->readReg32(0x701CC);
			SYSLOG("ngreen", "V88[%d]: PIPE_STAT=0x%x DSB_CTL=0x%x COLOR_CTL=0x%x",
				   v60Count, pipeStatA, dsbCtl, planeColorCtl);

			// Plane 1 KEY registers
			uint32_t plKeyVal = NGreen::callback->readReg32(0x70194);   // PLANE_KEYVAL
			uint32_t plKeyMax = NGreen::callback->readReg32(0x70198);   // PLANE_KEYMSK (or MAX)
			uint32_t plOffset = NGreen::callback->readReg32(0x701A4);   // PLANE_OFFSET
			uint32_t plAuxSurf = NGreen::callback->readReg32(0x701A0);  // PLANE_AUX_SURF
			SYSLOG("ngreen", "V88[%d]: KEYVAL=0x%x KEYMSK=0x%x OFFSET=0x%x AUX=0x%x",
				   v60Count, plKeyVal, plKeyMax, plOffset, plAuxSurf);
		}

		// 2. Flush GGTT TLB (GFX_FLSH_CNTL_GEN6 = 0x101008)
		NGreen::callback->writeReg32(0x101008, 0x1);
		// Wait for flush to complete (bit 0 clears when done)
		for (int i = 0; i < 100; i++) {
			if (!(NGreen::callback->readReg32(0x101008) & 1)) break;
		}

		// 3. Fill with 4 COLOR BANDS (different color per screen quarter)
		//    2560x1600 @ 4BPP, stride=10240B, 1600 lines total
		//    Each page = 4096B. Lines/page = 4096/10240 ≈ 0.4
		//    Pages per quarter = 4000/4 = 1000
		int filled = 0, failed = 0, skipped = 0;
		uint32_t firstPx = 0;
		uint64_t firstPhys = 0;

		for (int p = 0; p < 4000; p++) {
			uint32_t lo = NGreen::callback->readReg32(GGTT_PTE_LO(surfPage + p));
			uint32_t hi = NGreen::callback->readReg32(GGTT_PTE_HI(surfPage + p));
			uint64_t phys = (((uint64_t)hi << 32) | lo) & 0x0000FFFFFFFFF000ULL;
			if (!(lo & 1) || phys == 0) { failed++; continue; }
			if (phys < 0x40000000ULL) { skipped++; continue; }

			// Choose color by screen quarter
			uint32_t color;
			if (p < 1000)      color = 0xFFFF0000;  // RED    (top)
			else if (p < 2000) color = 0xFF00FF00;  // GREEN  (2nd)
			else if (p < 3000) color = 0xFF0000FF;  // BLUE   (3rd)
			else               color = 0xFFFFFFFF;  // WHITE  (bottom)

			auto *desc = IOMemoryDescriptor::withPhysicalAddress(
				(IOPhysicalAddress)phys, 4096, kIODirectionInOut);
			if (!desc) { failed++; continue; }
			auto *map = desc->createMappingInTask(kernel_task, 0,
				kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
			if (map) {
				volatile uint32_t *fb = (volatile uint32_t *)map->getVirtualAddress();
				if (p == 0) {
					firstPx = fb[0];
					firstPhys = phys;
					if (!v85PersistMap && v60Count >= 3) {
						v85PersistMap = desc->createMappingInTask(kernel_task, 0,
							kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
						v85SurfAddr = surfAddr;
					}
				}
				for (int px = 0; px < 1024; px++)
					fb[px] = color;
				filled++;
				map->release();
			} else { failed++; }
			desc->release();
		}

		// 4. Re-arm PLANE_SURF (guard: zero -> GGTT[0] -> stolen mem -> MCE)
		if (filled > 0 && surfAddr != 0)
			NGreen::callback->writeReg32(0x7019C, surfAddr);

		// 5. On iteration 5: toggle plane OFF then ON (force re-init)
		if (v60Count == 5 && filled > 0 && surfAddr != 0) {
			uint32_t planCtl = NGreen::callback->readReg32(0x70180);
			// Disable plane (clear bit 31)
			NGreen::callback->writeReg32(0x70180, planCtl & ~0x80000000u);
			NGreen::callback->writeReg32(0x7019C, surfAddr); // commit disable
			// Small delay — ~1000 register reads as delay
			for (volatile int d = 0; d < 1000; d++)
				NGreen::callback->readReg32(0x70180);
			// Re-enable plane with same settings
			NGreen::callback->writeReg32(0x70180, planCtl);
			NGreen::callback->writeReg32(0x7019C, surfAddr); // commit enable
			SYSLOG("ngreen", "V88[%d]: plane toggle OFF->ON CTL=0x%x", v60Count, planCtl);
		}

		if (v60Count <= 5 || v60Count == 10 || v60Count == 20) {
			SYSLOG("ngreen", "V88[%d]: SURF=0x%x filled=%d fail=%d skip=%d px0=0x%x phys=0x%llx",
				   v60Count, surfAddr, filled, failed, skipped, firstPx,
				   (unsigned long long)firstPhys);
		}
	}

	// ── V68: Iteration 5 — deep probe e08obj + GGTT PTE check ──
	// V67 proved: sched error clear sticks, but hardware ERROR_GEN6=0x7b recurs
	// every ~10s independently. Need to find WHAT generates the error.
	if (v60Count == 5) {
		uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
		if (schedObj) {
			auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
			// V68: Dump e08obj bytes 0x100-0x200 — look for engine dead/reset flags
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + 0x100 + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68: e08deep[0x%03x]=0x%016llx", 0x100 + i * 8, (unsigned long long)val);
				}
			}
			// V68: Check e08obj+0x200-0x300 — possible per-engine state arrays
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + 0x200 + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68: e08deep[0x%03x]=0x%016llx", 0x200 + i * 8, (unsigned long long)val);
				}
			}
		}
		// V68: Re-check scheduler error after V68 early clear at iter 1
		uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
		if (schedPtr) {
			auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
			uint32_t schedFlag = *reinterpret_cast<uint32_t *>(schedBase + 0x00);
			uint32_t schedErr = *reinterpret_cast<uint32_t *>(schedBase + 0x0C);
			SYSLOG("ngreen", "V68: iter5 sched[0x00]=0x%x sched[0x0C]=0x%x (cleared at iter1)",
				schedFlag, schedErr);
		}
		// V68: Read GGTT PTE for ring buffer page (ring base = acthd area)
		// GGTT is accessed via aperture at BAR offset or via registers
		// Read the first few GGTT PTEs to check format
		uint32_t ringBase44k = 0x98 >> 12; // ring HEAD offset → page 0
		// Read fence regs to check for GGTT format clues
		uint32_t fence0 = NGreen::callback->readReg32(0x100000); // First GGTT PTE
		uint32_t fence1 = NGreen::callback->readReg32(0x100004);
		uint32_t fence2 = NGreen::callback->readReg32(0x100008);
		uint32_t fence3 = NGreen::callback->readReg32(0x10000C);
		SYSLOG("ngreen", "V68: GGTT[0]=0x%x:%x GGTT[1]=0x%x:%x", fence1, fence0, fence3, fence2);
		// Read TLB data again
		uint32_t tlb0 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA0);
		uint32_t tlb1 = NGreen::callback->readReg32(GEN8_FAULT_TLB_DATA1);
		uint32_t faultReg = NGreen::callback->readReg32(GEN12_RING_FAULT_REG);
		SYSLOG("ngreen", "V68: FAULT=0x%x TLB0=0x%x TLB1=0x%x", faultReg, tlb0, tlb1);
		// Check encodeFailureStack
		uint32_t failCount = getMember<uint32_t>(svc, 0x1c50);
		SYSLOG("ngreen", "V68: encodeFailureStack count=%u", failCount);
	}
	
	// V68: Iteration 10 — re-check e08obj + scheduler after error suppression
	if (v60Count == 10) {
		uint64_t schedObj = getMember<uint64_t>(svc, 0xe08);
		if (schedObj) {
			auto *objBase = reinterpret_cast<uint8_t *>(schedObj);
			// Re-dump first 256 bytes to see if any state shifted
			for (int i = 0; i < 32; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(objBase + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68B: e08obj[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
				}
			}
		}
		uint64_t schedPtr = getMember<uint64_t>(svc, 0xe00);
		if (schedPtr) {
			auto *schedBase = reinterpret_cast<uint8_t *>(schedPtr);
			// Dump non-zero fields in 0x000-0x400
			for (int i = 0; i < 128; i++) {
				uint64_t val = *reinterpret_cast<uint64_t *>(schedBase + i * 8);
				if (val != 0) {
					SYSLOG("ngreen", "V68B: sched[0x%03x]=0x%016llx", i * 8, (unsigned long long)val);
				}
			}
		}
	}
	
	v60LastHead = rcsHead;
	v60LastTail = rcsTail;
	
	// Re-arm: 60 iterations x 2s = 120s
	// Each timer holds a retain on the IOService (param0 = svc).
	auto *monSvc = static_cast<IOService *>(param0);
	if (v60Count < 60) {
		auto nextTimer = thread_call_allocate(v60GpuHealthMonitor, param0);
		if (nextTimer) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
			thread_call_enter_delayed(nextTimer, deadline);
			// retain is transferred to the next timer — do NOT release here
		} else {
			// alloc failed — release the retain we hold
			monSvc->release();
		}
	} else {
		SYSLOG("ngreen", "V60M: complete — %d iterations, final HEAD=0x%x EXEC=0x%x ch=%d",
			   v60Count, rcsHead, execStatus, childCount);
		monSvc->release(); // last iteration — release the retain
	}
}

// V74: Permanent EMR + GGTT enforcer — fires every 50ms for the lifetime of the GPU.
//
// WHY: On RPL/ADL spoofed as TGL, Apple's AppleIntelTGLGraphics continuously re-writes
// RING_EMR (Error Mask Register) for RCS and BCS back to a non-full-mask value, and also
// re-maps GGTT[0] to the stolen-memory base. Both are fatal:
//   - Un-masked EMR causes the GPU error interrupt to fire on every submitted command,
//     preventing the display pipe from ever activating (all-black screen).
//   - GGTT[0] → stolen mem causes a package-wide MCE if any GPU VA-0 access occurs.
//
// HOW: Apple writes EMR via direct GT MMIO (not through the hookable WriteRegister32
// vtable), so we cannot intercept it. Instead we poll every 50ms and correct any
// deviation. The timer re-enters its own thread call indefinitely; when MMIO becomes
// invalid (GPU teardown) or the service goes inactive it frees the call and drops the
// one retain it holds on the IOService.
//
// WHAT it does each tick:
//   1. Force RING_EMR(RCS) and RING_EMR(BCS) to 0xFFFFFFFF (all errors masked).
//   2. Clear ERROR_GEN6 if non-zero.
//   3. Re-enforce GGTT[0] → safe wired dummy page (V116, see below).
void Gen11::v71EmrEnforcer(thread_call_param_t param0, thread_call_param_t param1) {
	auto *emrSvc = static_cast<IOService *>(param0);
	auto self = static_cast<thread_call_t>(param1);
	const int v71Count = ++gEmrCount;
	if (!emrSvc || emrSvc->isInactive() || !NGreen::callback || !NGreen::callback->mmioValid()) {
		if (self) thread_call_free(self);
		if (emrSvc) emrSvc->release();
		__sync_lock_test_and_set(&gEmrEnforcerArmed, 0u);
		return;
	}

	// V74 EMR-mask portion REMOVED (was: every 50ms force RING_EMR(RCS) and RING_EMR(BCS)
	// to 0xFFFFFFFF, clear ERROR_GEN6). Pair-mate to V72R/V72W/V72F. With those three
	// passthrough on the write paths but V74 still polling and rewriting EMR back to
	// all-ones from a thread, the "no blanket EMR mask" experiment was incomplete.
	// Now reads-only: log what Apple's actual EMR/ERROR values look like so we can
	// see WHICH bits Apple unmasks and WHICH errors actually fire (instead of hiding
	// them under a blanket 0xFFFFFFFF). The V116 GGTT[0] re-enforcer below is left
	// intact — that prevents real MCE on stolen-mem GVA-0 access, not a hack.
	uint32_t emrRcs = NGreen::callback->readReg32(RING_EMR(RENDER_RING_BASE));
	uint32_t emrBcs = NGreen::callback->readReg32(RING_EMR(BLT_RING_BASE));
	uint32_t err    = NGreen::callback->readReg32(ERROR_GEN6);
	if (v71Count <= 3 || (err != 0 && v71Count <= 50) ||
	    ((emrRcs != 0xFFFFFFFF || emrBcs != 0xFFFFFFFF) && v71Count <= 50)) {
		SYSLOG("ngreen", "V74Ep[%d]: EMR_RCS=0x%x EMR_BCS=0x%x ERR=0x%x (V74 EMR mask removed)",
		       v71Count, emrRcs, emrBcs, err);
	}

	// V80 legacy ownership mode: one-shot plane linearization at early boot.
	// Runs only during the first 3 ticks (≤150ms after start) — this produces the
	// brief display flash that was present before the start hook was active.
	// After tick 3 WS has composited its first frame and owns those registers;
	// continuing to write them causes WS crash-loop → watchdog KP at T+120s.
	// -ngreenv80l overrides to run continuously (for isolated plane-write testing only).
	/*if (!NGreen::callback->isRealTGL && isV60MonitorEnabled() &&
	    (v71Count <= 3 || isV80LEnforcerEnabled())) {
		uint32_t planCtl = NGreen::callback->readReg32(0x70180); // PLANE_CTL
		bool planeEnabled = !!(planCtl & 0x80000000u);
		if (planeEnabled) {
			uint32_t planStrd = NGreen::callback->readReg32(0x70188); // PLANE_STRIDE
			uint32_t planSurf = NGreen::callback->readReg32(0x7019C); // PLANE_SURF
			uint32_t tiling = (planCtl >> 10) & 0x7; // bits[12:10]
			if (planSurf != 0 && tiling != 0) {
				uint32_t newCtl = planCtl & ~(0x7u << 10); // clear tiling -> linear
				uint32_t newStrd = planStrd;
				if (tiling == 1) {
					newStrd = planStrd * 8;
				} else if (tiling == 4) {
					newStrd = planStrd * 16;
				}
				NGreen::callback->writeReg32(0x70180, newCtl);
				NGreen::callback->writeReg32(0x70188, newStrd);
				NGreen::callback->writeReg32(0x7019C, planSurf);
				if (v71Count <= 20 || (v71Count % 20) == 0) {
					SYSLOG("ngreen", "V80L[%d]: tiling %u->linear CTL 0x%x->0x%x STRIDE 0x%x->0x%x SURF=0x%x",
						   v71Count, tiling, planCtl, newCtl, planStrd, newStrd, planSurf);
				}
			} else if (planSurf != 0) {
				NGreen::callback->writeReg32(0x7019C, planSurf);
				if (v71Count <= 20 || (v71Count % 20) == 0) {
					SYSLOG("ngreen", "V80L[%d]: already linear, SURF re-arm=0x%x", v71Count, planSurf);
				}
			}
		}
	}*/

	// V116: Re-enforce GGTT[0] → safe wired dummy page every 50ms.
	// Apple's TGL framebuffer driver re-writes GGTT[0] to the stolen-memory physical base
	// during its internal GTT setup. Any GPU command that dereferences VA 0x0 then hits
	// stolen memory, triggering an uncorrectable MCE (machine check exception) that kills
	// the whole package. We restore GGTT[0] to a wired zero-page (allocated at start) on
	// every enforcer tick so the stolen-mem mapping can never persist.
	// PTE flags: bits[11:0]=0x9 → present | global (TGL/ICL GTT format, no cache attrs).
	if (v116DummyPhys) {
		uint32_t wantLo = (uint32_t)(v116DummyPhys & 0xFFFFF000ULL) | 0x9;
		uint32_t cur0Lo = NGreen::callback->readReg32(GGTT_PTE_LO(0));
		if (cur0Lo != wantLo) {
			uint32_t wantHi = (uint32_t)(v116DummyPhys >> 32);
			NGreen::callback->writeReg32(GGTT_PTE_LO(0), wantLo);
			NGreen::callback->writeReg32(GGTT_PTE_HI(0), wantHi);
			NGreen::callback->writeReg32(0x101008, 0x1);  // GGTT TLB flush
			if (v71Count <= 10)
				SYSLOG("ngreen", "V116E[%d]: GGTT[0] restored (was 0x%x → 0x%x)",
					   v71Count, cur0Lo, wantLo);
		}
	}

		uint64_t deadline;
		clock_interval_to_deadline(50, kMillisecondScale, &deadline);
	thread_call_enter1_delayed(self, self, deadline);
}

// V85: Static member definitions for persistent FB mapping
IOMemoryMap *Gen11::v85PersistMap = nullptr;
uint32_t Gen11::v85SurfAddr = 0;

// V116: Safe dummy page for GGTT[0] remap (prevents stolen-mem MCE on GPU VA 0 access)
IOBufferMemoryDescriptor *Gen11::v116DummyBuf = nullptr;
uint64_t Gen11::v116DummyPhys = 0;

// V221: Becomes true when IntelAccelerator::start() returns. Used by wrapWaitForStamp
// to block CoreDisplay's stamp-3 wait until the GFX interrupt handler is installed.
volatile bool Gen11::gGfxAccelStartDone = false;

static constexpr size_t   kHWCS5HwspPtrOffset   = 0x68;
static constexpr uint32_t kEngineProbeFwBit  = 1u << 4;

static constexpr uint32_t kEngineProbeIntervalMs   = 250;
static constexpr uint32_t kEngineProbeMaxSamples   = 480;
static IOService        *gEngineProbeAccelService = nullptr;
static thread_call_t     gEngineProbeCall    = nullptr;
static volatile uint32_t gEngineProbeStop         = 0;
static uint32_t          gEngineProbeSamples      = 0;

static constexpr uint32_t kRcsMaxStreamers = 8;
static constexpr uint32_t kRcsMaxAttempts  = 24;
static constexpr size_t   kRcsSchedulerOffset = 0x20;

struct RcsStreamer {
	void     *cs;
	void     *hwsp;
	uint64_t  hwspPhys;
};
static RcsStreamer      gRcsStreamers[kRcsMaxStreamers] = {};
static volatile uint32_t gRcsCount     = 0;
static void * volatile   gRcsSubmitCs  = nullptr;
static uint32_t          gRcsSelected  = 0;
static uint32_t          gRcsAttempts  = 0;

void *gFinishTimerCommandStreamer = nullptr;
void *gFinishTimerScheduler       = nullptr;

static bool readPhys64(uint64_t pa, uint64_t &out) {
	if (isStolen(pa)) return false;
	auto *desc = IOMemoryDescriptor::withPhysicalAddress(
	    static_cast<IOPhysicalAddress>(pa & ~0xFFFull), 4096, kIODirectionIn);
	if (desc == nullptr) return false;
	auto *map = desc->createMappingInTask(kernel_task, 0,
	    kIOMapAnywhere | kIOMapInhibitCache, 0, 4096);
	if (map == nullptr) {
		desc->release();
		return false;
	}
	out = *reinterpret_cast<volatile const uint64_t *>(
	    static_cast<uintptr_t>(map->getVirtualAddress()) + (pa & 0xFFFull));
	map->release();
	desc->release();
	return true;
}

static uint64_t physOfKernelVA(const volatile void *vap) {
	const uint64_t va = reinterpret_cast<uint64_t>(vap);
	uint64_t tbl = get_cr3_base();
	const uint32_t idx[4] = {
	    static_cast<uint32_t>((va >> 39) & 0x1FF),
	    static_cast<uint32_t>((va >> 30) & 0x1FF),
	    static_cast<uint32_t>((va >> 21) & 0x1FF),
	    static_cast<uint32_t>((va >> 12) & 0x1FF)};
	for (int lvl = 0; lvl < 4; lvl++) {
		uint64_t e = 0;
		if (!readPhys64(tbl + (static_cast<uint64_t>(idx[lvl]) << 3), e)) return 0;
		if ((e & 1ull) == 0) return 0;
		if (lvl == 3) return (e & 0x000FFFFFFFFFF000ull) | (va & 0xFFFull);
		if (e & (1ull << 7)) {
			if (lvl == 1) return (e & 0x000FFFFFC0000000ull) | (va & 0x3FFFFFFFull);
			return (e & 0x000FFFFFFFE00000ull) | (va & 0x1FFFFFull);
		}
		tbl = e & 0x000FFFFFFFFFF000ull;
	}
	return 0;
}

static void rcsRetarget(uint64_t want) {
	if (gRcsSelected) return;
	gRcsAttempts++;

	const uint32_t seen = __sync_fetch_and_or(&gRcsCount, 0u);
	const uint32_t lim  = seen < kRcsMaxStreamers ? seen : kRcsMaxStreamers;
	void    *pick = nullptr;

	for (uint32_t i = 0; i < lim; i++) {
		if (gRcsStreamers[i].cs == nullptr) continue;
		if (gRcsStreamers[i].hwsp == nullptr)
			gRcsStreamers[i].hwsp = *reinterpret_cast<void **>(
			    static_cast<uint8_t *>(gRcsStreamers[i].cs) + kHWCS5HwspPtrOffset);
		if (gRcsStreamers[i].hwspPhys == 0 && gRcsStreamers[i].hwsp != nullptr)
			gRcsStreamers[i].hwspPhys = physOfKernelVA(gRcsStreamers[i].hwsp);

		if (pick == nullptr && want != 0 && gRcsStreamers[i].hwspPhys == want) pick = gRcsStreamers[i].cs;
	}

	if (pick == nullptr) pick = gRcsSubmitCs;

	if (pick == nullptr) {
		if (gRcsAttempts >= kRcsMaxAttempts) gRcsSelected = 1;
		return;
	}

	gRcsSelected = 1;
	if (pick != gFinishTimerCommandStreamer) {
		gFinishTimerCommandStreamer = pick;
		void *sc = *reinterpret_cast<void **>(static_cast<uint8_t *>(pick) + kRcsSchedulerOffset);
		if (sc != nullptr) gFinishTimerScheduler = sc;
	}
}

static void stopEngineProbe() {
	__sync_lock_test_and_set(&gEngineProbeStop, 1u);
	auto call = gEngineProbeCall;
	auto *service = gEngineProbeAccelService;
	gEngineProbeCall = nullptr;
	gEngineProbeAccelService = nullptr;
	if (call) thread_call_free(call);
	if (service) service->release();
}

void Gen11::engineProbeTick(thread_call_param_t, thread_call_param_t) {
	auto *service = gEngineProbeAccelService;
	if (__sync_fetch_and_or(&gEngineProbeStop, 0u) != 0 || !service || service->isInactive() || !NGreen::callback || !NGreen::callback->mmioValid()) {
		stopEngineProbe();
		return;
	}
	auto *cb = NGreen::callback;
	fw_domain_wait_ack_clear_fallback(fw_domain_render, kEngineProbeFwBit);
	fw_domain_get(fw_domain_render, kEngineProbeFwBit);
	fw_domain_wait_ack_set_fallback(fw_domain_render, kEngineProbeFwBit);
	rc6ProbeTick();
	const uint32_t index = ++gEngineProbeSamples;
	const uint32_t execStatus = cb->readReg32(RING_EXECLIST_STATUS(RENDER_RING_BASE));
	const uint32_t head = cb->readReg32(RING_HEAD(RENDER_RING_BASE));
	const uint32_t tail = cb->readReg32(RING_TAIL(RENDER_RING_BASE));
	const uint32_t gt0 = cb->readReg32(GEN11_GT_INTR_DW0);
	uint64_t hwsPgaPhys = 0;
	const uint32_t hwsPga = cb->readReg32(RING_HWS_PGA(RENDER_RING_BASE));
	if (hwsPga != 0 && hwsPga != 0xffffffffu) {
		const uint32_t hwsPteLo = cb->readReg32(GGTT_PTE_LO(hwsPga >> 12));
		const uint32_t hwsPteHi = cb->readReg32(GGTT_PTE_HI(hwsPga >> 12));
		bool is_present = false;
		const uint64_t pa = gen8_ggtt_pte_decode((static_cast<uint64_t>(hwsPteHi) << 32) | hwsPteLo, &is_present);
		if (is_present) hwsPgaPhys = pa;
	}
	const uint32_t csDebug = cb->readReg32(GEN9_CS_DEBUG_MODE1);
	const uint32_t ffMode = cb->readReg32(GEN7_FF_THREAD_MODE);
	if (csDebug != 0xffffffffu && ffMode != 0xffffffffu &&
	    ((csDebug & FF_DOP_CLOCK_GATE_DISABLE) == 0 || (ffMode & GEN12_FF_TESSELATION_DOP_GATE_DISABLE) == 0))
		applyRenderWa(cb, false);
	fw_domain_put(fw_domain_render, kEngineProbeFwBit);
	rcsRetarget(hwsPgaPhys);
	if (gBurstPrevExec != 0xffffffffu && gBurstLeft == 0) {
		const bool wasRunning = (gBurstPrevExec & 0x18u) != 0 || gBurstPrevExec == 0x18001u;
		const bool nowLoaded  = (execStatus & 0x18u) != 0;
		const bool bit30RoseOnStaticRing =
		    (gBurstPrevExec & 0x40000000u) == 0 && (execStatus & 0x40000000u) != 0 &&
		    (execStatus & 0x18u) != 0 && (execStatus & 0x60000u) == 0 &&
		    head == tail && head == gBurstPrevHead;
		if (((!wasRunning && nowLoaded) || bit30RoseOnStaticRing) && head == tail && gt0 == 0)
			gBurstLeft = 24;
	}
	gBurstPrevExec = execStatus;
	gBurstPrevHead = head;
	if (index >= kEngineProbeMaxSamples) {
		stopEngineProbe();
		return;
	}
	if (__sync_fetch_and_or(&gEngineProbeStop, 0u) == 0 && gEngineProbeCall != nullptr) {
		uint64_t deadline;
		if (gBurstLeft > 0) {
			clock_interval_to_deadline(10, kMillisecondScale, &deadline);
			gBurstLeft--;
		} else {
			clock_interval_to_deadline(kEngineProbeIntervalMs, kMillisecondScale, &deadline);
		}
		thread_call_enter_delayed(gEngineProbeCall, deadline);
	}
}

void Gen11::armEngineProbe(void *accel) {
	if (gEngineProbeCall != nullptr || accel == nullptr || callback == nullptr) return;
	gEngineProbeSamples = 0;
	gRcsSelected = 0;
	gRcsAttempts = 0;
	if (!NGreen::callback || !NGreen::callback->mmioValid()) return;
	gEngineProbeCall = thread_call_allocate(engineProbeTick, nullptr);
	if (gEngineProbeCall == nullptr) return;
	auto *service = static_cast<IOService *>(accel);
	service->retain();
	gEngineProbeAccelService = service;
	__sync_lock_test_and_set(&gEngineProbeStop, 0u);
	uint64_t deadline;
	clock_interval_to_deadline(kEngineProbeIntervalMs, kMillisecondScale, &deadline);
	thread_call_enter_delayed(gEngineProbeCall, deadline);
}

static constexpr size_t kHWCS5SchedulerOffset   = 0x20;
static constexpr size_t kHWCS5FinishSourceOffset = 0x1c08;

void Gen11::wrapHWCS5RegisterForInterrupts(void *that) {
	FunctionCast(wrapHWCS5RegisterForInterrupts,
	             callback->oHWCS5RegisterForInterrupts)(that);

	if (that == nullptr)
		return;

	if (gFinishTimerCommandStreamer == nullptr) gFinishTimerCommandStreamer = that;
	{
		auto *b   = static_cast<uint8_t *>(that);
		void *sc  = *reinterpret_cast<void **>(b + kHWCS5SchedulerOffset);
		if (gFinishTimerScheduler == nullptr && sc != nullptr) gFinishTimerScheduler = sc;
	}

	{
		const uint32_t n = __sync_add_and_fetch(&gRcsCount, 1u) - 1u;
		void *hwsp = *reinterpret_cast<void **>(
		    static_cast<uint8_t *>(that) + kHWCS5HwspPtrOffset);
		if (n < kRcsMaxStreamers) {
			gRcsStreamers[n].cs   = that;
			gRcsStreamers[n].hwsp = hwsp;
		}
	}

	if (!IS_RAPTOR_LAKE_HX())
		return;

	if (callback->oIGSchedEnablePeriodic == 0) {
		return;
	}

	auto *base  = static_cast<uint8_t *>(that);
	void *sched = *reinterpret_cast<void **>(base + kHWCS5SchedulerOffset);
	void *src   = *reinterpret_cast<void **>(base + kHWCS5FinishSourceOffset);

	if (sched == nullptr || src == nullptr) {
		return;
	}

	using EnableFn = void (*)(void *, void *);
	reinterpret_cast<EnableFn>(callback->oIGSchedEnablePeriodic)(sched, src);
}

void Gen11::wrapIdleToActive(void *that, const uint32_t *e) {
	auto *cs = static_cast<uint8_t *>(that);
	const uint32_t cur = *reinterpret_cast<const uint32_t *>(cs + kCsOffCur) & 1u;
	if (csbElement(cs, 1u - cur, 0) == 0) {
		volatile uint8_t *gate = cs + kCsOffGate;
		if (*gate == 0) *gate = 1;
		return;
	}
	FunctionCast(wrapIdleToActive, callback->oIdleToActive)(that, e);
}

void Gen11::wrapPreempted(void *that, const uint32_t *e) {
	auto *cs = static_cast<uint8_t *>(that);
	if (e != nullptr) {
		const uint32_t cur = *reinterpret_cast<const uint32_t *>(cs + kCsOffCur) & 1u;
		const uint64_t cur0 = csbElement(cs, cur, 0), pend0 = csbElement(cs, 1u - cur, 0);
		if (cur0 == 0 || pend0 == 0) {
			uint32_t lo = 0, hi = 0, ptr = 0;
			const bool ok = csbRawEntry(cs, lo, hi, ptr);
			const bool newq = ok ? (lo & GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE) != 0 : (e[0] & 0x1f0u) != 0;
			if (pend0 == 0 && newq) {
				return;
			}
			if (cur0 == 0 && csbT1Room()) {
				__sync_fetch_and_add(&gCsbT1Uses, 1u);
				if (ok && pend0 != 0 && callback->oIdleToActive != 0 && csbContextId(pend0) == FIELD_GET(GEN12_CSB_SW_CTX_ID_MASK, lo))
					FunctionCast(wrapIdleToActive, callback->oIdleToActive)(that, e);
				return;
			}
		}
	}
	FunctionCast(wrapPreempted, callback->oPreempted)(that, e);
}

void Gen11::wrapElementSwitch(void *that, const uint32_t *e) {
	auto *cs = static_cast<uint8_t *>(that);
	if (e != nullptr) {
		const uint32_t cur = *reinterpret_cast<const uint32_t *>(cs + kCsOffCur) & 1u;
		const uint64_t cur0 = csbElement(cs, cur, 0);
		if (cur0 == 0) {
			if (csbT1Room()) {
				const uint64_t pend0 = csbElement(cs, 1u - cur, 0), pend1 = csbElement(cs, 1u - cur, 1);
				__sync_fetch_and_add(&gCsbT1Uses, 1u);
				if (pend0 != 0 && pend1 != 0 && callback->oIdleToActive != 0 && csbContextId(pend0) == ((e[1] >> 5) & 0x7ffu)) {
					FunctionCast(wrapIdleToActive, callback->oIdleToActive)(that, e);
					FunctionCast(wrapElementSwitch, callback->oElementSwitch)(that, e);
				}
				return;
			}
		} else if (csbElement(cs, cur, 1) == 0) {
			return;
		}
	}
	FunctionCast(wrapElementSwitch, callback->oElementSwitch)(that, e);
}

void Gen11::wrapActiveToIdle(void *that, const uint32_t *e) {
	auto *cs = static_cast<uint8_t *>(that);
	if (e != nullptr) {
		const uint32_t cur = *reinterpret_cast<const uint32_t *>(cs + kCsOffCur) & 1u;
		if (csbElement(cs, cur, 0) == 0 && csbT1Room()) {
			const uint64_t pend0 = csbElement(cs, 1u - cur, 0);
			__sync_fetch_and_add(&gCsbT1Uses, 1u);
			if (pend0 != 0 && callback->oIdleToActive != 0 && csbContextId(pend0) == ((e[1] >> 5) & 0x7ffu)) {
				FunctionCast(wrapIdleToActive, callback->oIdleToActive)(that, e);
				FunctionCast(wrapActiveToIdle, callback->oActiveToIdle)(that, e);
			}
			return;
		}
	}
	FunctionCast(wrapActiveToIdle, callback->oActiveToIdle)(that, e);
}

void Gen11::wrapResetOnWake(void *that) {
	FunctionCast(wrapResetOnWake, callback->oResetOnWake)(that);
	pageonOnWake();
}

void Gen11::wrapHWCS5SubmitExecList(void *that, uint32_t flags) {
	if (gRcsSubmitCs == nullptr) gRcsSubmitCs = that;
	holdResubmitIfCompleting(that);
	patchPendingBatches(that);
	FunctionCast(wrapHWCS5SubmitExecList, callback->oHWCS5SubmitExecList)(that, flags);
}

static TimerSource *timerSlot(void *src, bool create) {
	TimerSource *empty = nullptr;
	for (uint32_t i = 0; i < kTimerSlots; i++) {
		if (gTimerSources[i].src == src) return &gTimerSources[i];
		if (empty == nullptr && gTimerSources[i].src == nullptr) empty = &gTimerSources[i];
	}
	if (!create || empty == nullptr) return nullptr;
	*empty = TimerSource {src, 0, 0, 0, 0};
	return empty;
}

uint64_t Gen11::wrapEnablePeriodic(void *sched, void *src) {
	if (gTimerLock != nullptr) {
		const IOInterruptState is = IOSimpleLockLockDisableInterrupt(gTimerLock);
		TimerSource *e = timerSlot(src, true);
		if (e != nullptr) e->net++;
		IOSimpleLockUnlockEnableInterrupt(gTimerLock, is);
	}
	return FunctionCast(wrapEnablePeriodic, callback->oEnablePeriodic)(sched, src);
}

uint64_t Gen11::wrapDisablePeriodic(void *sched, void *src) {
	bool unmatched = false;
	if (gTimerLock != nullptr) {
		const IOInterruptState is = IOSimpleLockLockDisableInterrupt(gTimerLock);
		TimerSource *e = timerSlot(src, true);
		if (e != nullptr) {
			if (e->net <= 0) unmatched = true;
			else e->net--;
		}
		IOSimpleLockUnlockEnableInterrupt(gTimerLock, is);
	}
	if (unmatched) return 0;
	return FunctionCast(wrapDisablePeriodic, callback->oDisablePeriodic)(sched, src);
}

mach_vm_address_t gOrgSubmitFlipBuffer = 0, gOrgGenerateFlip = 0, gOrgBackBufferReady = 0;
static volatile uint32_t gFlipGenSeen = 0;
uint64_t wrapGenerateFlip(void *pipe, void *ev, uint32_t flags, void *res, void *res2) {
	gFlipGenSeen = 1;
	return FunctionCast(wrapGenerateFlip, gOrgGenerateFlip)(pipe, ev, flags, res, res2);
}
uint64_t wrapSubmitFlipBuffer(void *pipe, void *ev, uint32_t flags, void *res, void *res2) {
	auto *p = static_cast<const uint8_t *>(pipe);
	fixDuplicateFlip(pipe, surfaceAddr(res), p ? static_cast<uint32_t>(*reinterpret_cast<const uint64_t *>(p + 0x12c8)) : 0);
	gFlipGenSeen = 0;
	const uint64_t r = FunctionCast(wrapSubmitFlipBuffer, gOrgSubmitFlipBuffer)(pipe, ev, flags, res, res2);
	if (gFlipGenSeen != 0 && p != nullptr) {
		const int32_t idx = *reinterpret_cast<const int32_t *>(p + 0x1334);
		if (idx >= 0 && idx <= 2) gFlipCacheMs[idx] = uptimeMs();
	}
	return r;
}
uint64_t wrapBackBufferReady(void *surf, uint32_t fb) {
	const uint64_t r = FunctionCast(wrapBackBufferReady, gOrgBackBufferReady)(surf, fb);
	if ((r & 0xffu) == 0) {
		const uint32_t n = __sync_add_and_fetch(&gNotReadyRun, 1u);
		if (n == 1) gNotReadyRunStartMs = uptimeMs();
		if (surf != nullptr) reflipIfStuck(surf, fb, n);
	} else {
		gNotReadyRun = 0;
	}
	return r;
}

// V54: IRQ watchdog — re-enables Master IRQ if the driver disables it during init.
// Fires every 2s, up to 5 times (10s total), then stops.
void Gen11::v54IrqWatchdog(thread_call_param_t, thread_call_param_t param1) {
	auto self = static_cast<thread_call_t>(param1);
	if (__sync_fetch_and_or(&gIrqWatchdogStop, 0u) != 0) {
		if (self) thread_call_free(self);
		return;
	}
	__sync_add_and_fetch(&gIrqWatchdogActive, 1u);
	if (__sync_fetch_and_or(&gIrqWatchdogStop, 0u) != 0 || !NGreen::callback || !NGreen::callback->mmioValid()) {
		__sync_sub_and_fetch(&gIrqWatchdogActive, 1u);
		if (self) thread_call_free(self);
		return;
	}
	const int v54WatchdogCount = static_cast<int>(__sync_add_and_fetch(&gIrqWatchdogPasses, 1u));
	
	uint32_t mstrIrq = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
	bool enabled = !!(mstrIrq & GEN11_MASTER_IRQ);
	
	if (!enabled) {
		// Re-enable Master IRQ
		NGreen::callback->writeReg32(GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
		IODelay(100);
		uint32_t after = NGreen::callback->readReg32(GEN11_GFX_MSTR_IRQ);
		SYSLOG("ngreen", "V54W[%d]: Master IRQ was DISABLED (0x%x) — re-enabled → 0x%x",
			v54WatchdogCount, mstrIrq, after);
	} else {
		SYSLOG("ngreen", "V54W[%d]: Master IRQ OK (0x%x)", v54WatchdogCount, mstrIrq);
	}
	
	// V65: Continuously enforce RCS0 tier-1 interrupt enable during ring init window.
	// V64 proved Apple's code leaves RCS0 DISABLED in RENDER_COPY_INTR_ENABLE.
	// The ring activates between V54W[4]-V54W[5] (T+8-10s) — we must have this set BEFORE.
	{
		uint32_t rcIntrEn = NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE);
		uint32_t wantBits = getV65Tier1WantBits(NGreen::callback->isRealTGL);
		bool rcsOk = !!(rcIntrEn & (1 << GEN11_RCS0));
		if (!rcsOk) {
			uint32_t newEn = rcIntrEn | wantBits;
			NGreen::callback->writeReg32(GEN11_RENDER_COPY_INTR_ENABLE, newEn);
			SYSLOG("ngreen", "V65W[%d]: RCS0 tier-1 DISABLED (0x%x) — enabled → 0x%x",
				v54WatchdogCount, rcIntrEn,
				NGreen::callback->readReg32(GEN11_RENDER_COPY_INTR_ENABLE));
		} else {
			SYSLOG("ngreen", "V65W[%d]: RCS0 tier-1 OK (0x%x)", v54WatchdogCount, rcIntrEn);
		}
		// Also enforce tier-2 unmask
		uint32_t rcsMask = NGreen::callback->readReg32(GEN11_RCS0_RSVD_INTR_MASK);
		uint32_t wantUnmasked = GT_CONTEXT_SWITCH_INTERRUPT | GT_RENDER_USER_INTERRUPT;
		if (rcsMask & wantUnmasked) {
			uint32_t newMask = rcsMask & ~wantUnmasked;
			NGreen::callback->writeReg32(GEN11_RCS0_RSVD_INTR_MASK, newMask);
			SYSLOG("ngreen", "V65W[%d]: RCS mask fix 0x%x->0x%x", v54WatchdogCount, rcsMask, newMask);
		}
		// V65: Proactively clear ERROR_GEN6 to prevent error handler from killing engine
		uint32_t errNow = NGreen::callback->readReg32(ERROR_GEN6);
		if (errNow) {
			NGreen::callback->writeReg32(ERROR_GEN6, 0x0);
			SYSLOG("ngreen", "V65W[%d]: cleared ERROR_GEN6=0x%x", v54WatchdogCount, errNow);
		}
	}
	
	// Also dump quick GPU state for diagnostics
	SYSLOG("ngreen", "V54W[%d]: RCS CTL=0x%x HEAD=0x%x TAIL=0x%x ERROR_GEN6=0x%x",
		v54WatchdogCount,
		NGreen::callback->readReg32(RING_CTL(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_HEAD(RENDER_RING_BASE)),
		NGreen::callback->readReg32(RING_TAIL(RENDER_RING_BASE)),
		NGreen::callback->readReg32(ERROR_GEN6));
	
	// Re-arm for up to 5 iterations
	if (v54WatchdogCount < 5 && __sync_fetch_and_or(&gIrqWatchdogStop, 0u) == 0) {
			uint64_t deadline;
			clock_interval_to_deadline(2, kSecondScale, &deadline);
		thread_call_enter1_delayed(self, self, deadline);
	} else {
		SYSLOG("ngreen", "V54W: watchdog complete after %d iterations", v54WatchdogCount);
		if (self) thread_call_free(self);
	}
	__sync_sub_and_fetch(&gIrqWatchdogActive, 1u);
}
// V44: Bundle/property logging helper — logs Metal/GL/VA bundle names after a delay.
static const char *v44SafeCString(OSString *str) {
    if (!str) {
        return "MISSING";
    }
    auto *cstr = str->getCStringNoCopy();
    return cstr ? cstr : "<null-cstr>";
}

static void v44DelayedBundleLog(thread_call_param_t p0, thread_call_param_t) {
    auto *context = static_cast<DelayedServiceCall *>(p0);
    auto *svc = context->service;
    unsigned delayMs = context->delayMs;
    thread_call_free(context->call);
    IODelete(context, DelayedServiceCall, 1);
    if (svc->isInactive()) {
        svc->release();
        return;
    }

    auto *metalProp = OSDynamicCast(OSString, svc->getProperty("MetalPluginName"));
    auto *glProp    = OSDynamicCast(OSString, svc->getProperty("IOGLBundleName"));
    auto *dvdProp   = OSDynamicCast(OSString, svc->getProperty("IODVDBundleName"));
    auto *accelBid  = OSDynamicCast(OSString, svc->getProperty("AcceleratorBundleIdentifier"));
    auto *fbBid     = OSDynamicCast(OSString, svc->getProperty("FramebufferBundleIdentifier"));
    auto *vaRIDProp = OSDynamicCast(OSNumber, svc->getProperty("IOVARendererID"));

    SYSLOG("ngreen", "V44: T+%ums MetalPlugin=%s GL=%s DVD=%s VARendID=0x%x",
           delayMs,
           v44SafeCString(metalProp),
           v44SafeCString(glProp),
           v44SafeCString(dvdProp),
           vaRIDProp ? (uint32_t)vaRIDProp->unsigned32BitValue() : 0);
    SYSLOG("ngreen", "V44: T+%ums AcceleratorBundleIdentifier=%s FramebufferBundleIdentifier=%s",
           delayMs,
           v44SafeCString(accelBid),
           v44SafeCString(fbBid));

    // Release retain acquired by scheduler helper.
    svc->release();
}

static void v44ScheduleBundleLog(void *accelInstance, unsigned delayMs) {
    auto *svc = static_cast<IOService *>(accelInstance);
    auto *context = svc ? IONewZero(DelayedServiceCall, 1) : nullptr;
    if (!context)
        return;
    svc->retain();

    context->service = svc;
    context->delayMs = delayMs;
    context->call = thread_call_allocate(v44DelayedBundleLog, context);
    if (context->call) {
        uint64_t deadline;
        clock_interval_to_deadline(delayMs, kMillisecondScale, &deadline);
        thread_call_enter_delayed(context->call, deadline);
    } else {
        svc->release();
        IODelete(context, DelayedServiceCall, 1);
    }
}

int Gen11::wrapPmNotifyWrapper(unsigned int a0, unsigned int a1, unsigned long long *a2, unsigned int *freq) {
	
	return answerPmNotify(a0, a1, a2, freq);
}

bool Gen11::patchRCSCheck(mach_vm_address_t& start) {
	constexpr unsigned ninsts_max {256};
	
	hde64s dis;
	
	bool found_cmp = false;
	bool found_jmp = false;

	for (size_t i = 0; i < ninsts_max; i++) {
		auto sz = Disassembler::hdeDisasm(start, &dis);

		if (dis.flags & F_ERROR) {
			break;
		}

		/* cmp byte ptr [rcx], 0 */
		if (!found_cmp && dis.opcode == 0x80 && dis.modrm_reg == 7 && dis.modrm_rm == 1)
			found_cmp = true;
		/* jnz rel32 */
		if (found_cmp && dis.opcode == 0x0f && dis.opcode2 == 0x85) {
			found_jmp = true;
			break;
		}

		start += sz;
	}
	
	if (found_jmp) {
		auto status = MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
		if (status == KERN_SUCCESS) {
			constexpr uint8_t nop6[] {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
			lilu_os_memcpy(reinterpret_cast<void*>(start), nop6, arrsize(nop6));
			MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
			return true;
		} else {
			return false;
		}
	} else {
		return false;
	}
}

/**
 * Port of i915 force wake for Gen12 (TGL/ADL/RPL).
 * Replaces IntelAccelerator::SafeForceWakeMultithreaded.
 *
 * Differences from Apple's code:
 * 1. 50 ms ACK timeouts (Apple uses 90 ms) — https://patchwork.kernel.org/patch/7057561/
 * 2. Reserve-bit fallback on primary ACK timeout — https://patchwork.kernel.org/patch/10029821/
 * 3. Correct Gen9-style 3-domain iteration matching Apple's dom bitmask
 *    (dom: bit0=Render, bit1=Media, bit2=Blitter/GT)
 *
 * NOTE: The header's regForDom/ackForDom use Gen11+ domain bitmask IDs
 * (FORCEWAKE_RENDER=1=Render, FORCEWAKE_GT=2, FORCEWAKE_MEDIA=4) which does NOT
 * match Apple's 3-bit dom (1=Render, 2=Media, 4=Blitter). We use direct register
 * mapping here to match Apple's convention, same as WEG's ForceWakeWorkaround.
 */

// Map Apple's 3-bit domain to MMIO request register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwReqReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_RENDER_GEN9;   // 0xa278
	if (d == DOM_BLITTER) return FORCEWAKE_BLITTER_GEN9;  // 0xa188
	return 0;  // Media uses Gen11+ per-engine registers, handled separately
}

// Map Apple's 3-bit domain to MMIO ACK register (Render + GT/Blitter same Gen9-Gen12)
static uint32_t fwAckReg(unsigned d) {
	if (d == DOM_RENDER)  return FORCEWAKE_ACK_RENDER_GEN9;   // 0x0D84
	if (d == DOM_BLITTER) return FORCEWAKE_ACK_BLITTER_GEN9;  // 0x130044
	return 0;
}

// Gen12 Media ForceWake: per-engine register pairs (VDBOX + VEBOX)
struct FwMediaEngine {
	uint8_t engineBit;
	uint32_t req;
	uint32_t ack;
	const char *name;
};
static const FwMediaEngine fwMediaEngines[] = {
	{ 3, FORCEWAKE_MEDIA_VDBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0), "VDBOX0" },  // 0xa540 / 0x0D50
	{ 4, FORCEWAKE_MEDIA_VDBOX_GEN11(2), FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(2), "VDBOX2" },
	{ 5, FORCEWAKE_MEDIA_VEBOX_GEN11(0), FORCEWAKE_ACK_MEDIA_VEBOX_GEN11(0), "VEBOX0" },  // 0xa560 / 0x0D70
};

void Gen11::wrapSafeForceWake(void *that, bool set, uint32_t dom) {
	forceWake(that, set, dom, 1);  // forward to our forceWake with ctx=1 (normal, non-IRQ)
}

void Gen11::forceWake(void *that, bool set, uint32_t dom, uint32_t ctx) {
	if (!that || !callback || !NGreen::callback)
		return;

	const bool fwLockHeld = ctx == 1;

	// Per-call logging stays off here: it flooded Lilu's circular buffer (384+ rows a boot)
	// and pushed the health-monitor and delayed-child-check rows out.
	
	// ctx 2: IRQ, ctx 1: normal
	uint32_t ack_exp = set << ctx;
	uint32_t mask = 1 << ctx;
	uint32_t wr = ack_exp | (1 << ctx << 16);

	for (unsigned d = DOM_FIRST; d <= DOM_LAST; d <<= 1) {
		if (!(dom & d)) continue;

		if (d == DOM_MEDIA) {
			const uint32_t mediaAckExp = set ? 1u : 0u;
			const uint32_t mediaMask = 1u;
			const uint32_t mediaWrite = 0x10000u | mediaAckExp;
			const auto *accelerator = reinterpret_cast<const volatile uint8_t *>(that);
			const uint64_t activeEngineMask =
				*reinterpret_cast<const volatile uint64_t *>(accelerator + 0x1300);
			for (const auto &eng : fwMediaEngines) {
				if ((activeEngineMask & (1ULL << eng.engineBit)) == 0)
					continue;
				wrapWriteRegister32(callback->framecont, eng.req, mediaWrite);
				IOPause(100);
				const bool mediaAcked = fwLockHeld
					? pollRegisterFast(eng.ack, mediaAckExp, mediaMask, kFwAckLockedUs)
					: (pollRegister(eng.ack, mediaAckExp, mediaMask, FORCEWAKE_ACK_TIMEOUT_MS) ||
					   forceWakeWaitAckFallback(eng.req, eng.ack, mediaAckExp, mediaMask) ||
					   pollRegister(eng.ack, mediaAckExp, mediaMask, FORCEWAKE_ACK_TIMEOUT_MS));
				if (mediaAcked)
					DBGLOG("ngreen", "ForceWake OK %s set=%d", eng.name, set);
			}
		} else {
			if (d == DOM_BLITTER && set) {
				const uint32_t writeVal = (1u | 0x10000u) << ctx;
				wrapWriteRegister32(callback->framecont, fwReqReg(d), writeVal);
				IODelay(500);
				continue;
			}
			wrapWriteRegister32(callback->framecont, fwReqReg(d), wr);
			IOPause(100);
			const bool domAcked = fwLockHeld
				? pollRegisterFast(fwAckReg(d), ack_exp, mask, kFwAckLockedUs)
				: (pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS) ||
				   forceWakeWaitAckFallback(fwReqReg(d), fwAckReg(d), ack_exp, mask) ||
				   pollRegister(fwAckReg(d), ack_exp, mask, FORCEWAKE_ACK_TIMEOUT_MS));
			if (!domAcked)
				SYSLOG("ngreen", "ForceWake timeout for domain %s (dom=0x%x), expected 0x%x", strForDom(d), dom, ack_exp);
			else
				DBGLOG("ngreen", "ForceWake OK domain=%s set=%d ack=0x%x", strForDom(d), set, wrapReadRegister32(callback->framecont, fwAckReg(d)));
		}
	}
	// V61: silenced — see above
}

bool Gen11::pollRegisterFast(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeoutUs) {
    uint64_t now = 0, deadline = 0;
    clock_interval_to_deadline(timeoutUs, kMicrosecondScale, &deadline);
    for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
        auto rd = wrapReadRegister32(callback->framecont, reg);
        if ((rd & mask) == val)
            return true;
    }
    return false;
}

bool Gen11::pollRegister(uint32_t reg, uint32_t val, uint32_t mask, uint32_t timeout) {
    uint64_t now = 0, deadline = 0;
    clock_interval_to_deadline(timeout, kMillisecondScale, &deadline);
    for (clock_get_uptime(&now); now < deadline; clock_get_uptime(&now)) {
        auto rd = wrapReadRegister32(callback->framecont, reg);
        if ((rd & mask) == val)
            return true;
    }
    return false;
}

bool Gen11::forceWakeWaitAckFallback(uint32_t reqReg, uint32_t ackReg, uint32_t val, uint32_t mask) {
	unsigned pass = 1;
	bool ack = false;
	auto controller = callback->framecont;
	
	do {
		pollRegister(ackReg, 0, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);
		wrapWriteRegister32(controller, reqReg, fw_set(FORCEWAKE_KERNEL_FALLBACK));
		
		IODelay(10 * pass);
		pollRegister(ackReg, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_KERNEL_FALLBACK, FORCEWAKE_ACK_TIMEOUT_MS);

		ack = (wrapReadRegister32(controller, ackReg) & mask) == val;

		wrapWriteRegister32(controller, reqReg, fw_clear(FORCEWAKE_KERNEL_FALLBACK));
	} while (!ack && pass++ < 10);
	
	return ack;
}

void Gen11::releaseDoorbell()
{}

bool Gen11::dotrue()
{
	return true;
}

int iniin=1;
void  Gen11::readAndClearInterrupts(AppleIntel::AppleIntelBaseController *that, void *param_1)
{
	
	if (iniin){
		iniin=0;
		SYSLOG("ngreen", "readAndClearInterrupts: first call — initializing Gen11 GT interrupts");
		
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, 0);
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, 0);
		uint32_t master_ctl=wrapReadRegister32(callback->framecont,GEN11_GFX_MSTR_IRQ);
		
		//Disable RCS, BCS, VCS and VECS class engines.
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE,	  0);
		
		// Restore masks irqs on RCS, BCS, VCS and VECS engines.
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK,	~0);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK,	~0);
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		
		
		uint32_t irqs = GT_RENDER_USER_INTERRUPT;
		uint32_t guc_mask = /*intel_uc_wants_guc(&gt->uc) ? GUC_INTR_GUC2HOST :*/ 0;
		uint32_t gsc_mask = 0;
		uint32_t heci_mask = 0;
		uint32_t dmask;
		uint32_t smask;
		
		irqs |= GT_CS_MASTER_ERROR_INTERRUPT |
		GT_CONTEXT_SWITCH_INTERRUPT |
		GT_WAIT_SEMAPHORE_INTERRUPT;
		
		dmask = irqs << 16 | irqs;
		smask = irqs << 16;
		
		
		/* Enable RCS, BCS, VCS and VECS class interrupts. */
		wrapWriteRegister32(callback->framecont, GEN11_RENDER_COPY_INTR_ENABLE, dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS_VECS_INTR_ENABLE, dmask);
		
		/* Unmask irqs on RCS, BCS, VCS and VECS engines. */
		wrapWriteRegister32(callback->framecont, GEN11_RCS0_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_BCS_RSVD_INTR_MASK, ~smask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
		wrapWriteRegister32(callback->framecont, GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
		
		/*
		 * RPS interrupts will get enabled/disabled on demand when RPS itself
		 * is enabled/disabled.
		 */
		
		
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
		
		/* Same thing for GuC interrupts */
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_ENABLE, 0);
		wrapWriteRegister32(callback->framecont, GEN11_GUC_SG_INTR_MASK,  ~0);
		
		wrapWriteRegister32(callback->framecont,GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE);
		wrapWriteRegister32(callback->framecont, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	}
	
	
	FunctionCast(readAndClearInterrupts, callback->oreadAndClearInterrupts)(that,param_1);
}

void * Gen11::serviceInterrupts(void *param_1)
{
	return FunctionCast(serviceInterrupts, callback->oserviceInterrupts)(param_1);
	
}

void * Gen11::wprobe(void *that,void *param_1,int *param_2)
{
	//FunctionCast(wprobe, callback->owprobe)(that, param_1,param_2);
	//logStateInRegistry(that,0x56);
	initializeLogging(reinterpret_cast<AppleIntel::AppleIntelBaseController *>(that));
	return that;
	
}

void *contr;
bool  Gen11::tgstart(void *that,void *param_1)
{
	contr=that;
	FunctionCast(tgstart, callback->otgstart)(that, param_1);
	return true;
	
}

void Gen11::FBMemMgr_Init(void *that)
{
	ccont  = getMember<void *>(that, 0xc40);  // MMIO register access manager
	ccont2 = that;                             // AppleIntelBaseController itself

	FunctionCast(FBMemMgr_Init, callback->oFBMemMgr_Init)(that);
	
	

	/*IODeviceMemory * m= NGreen::callback->iGPU->getDeviceMemoryWithIndex(0);
	IODeviceMemory *dm;
	m->withSubRange(dm,0x4180000,0x12000);//fDSBBufferBytes = 73728, fDSBBufferBaseOffset = 68681728
	IOMemoryMap *dsb=dm->map();
	
	IODeviceMemory *dm2;
	m->withSubRange(dm2,0x4192000,0x3000);//fConnectionStatusBytes = 12288, fConnectionStatusOffset = 68755456
	IOMemoryMap *dsb2=dm2->map();*/
	
}

uint32_t Gen11::probePortMode()
{
	auto ret=FunctionCast(probePortMode, callback->oprobePortMode)();
	return ret;
};


uint32_t Gen11::wdepthFromAttribute(void *that,uint param_1)
{
	return 0x1e;
};

uint32_t Gen11::raReadRegister32(void *that,unsigned long param_1)
{
	if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return NGreen::callback->readReg32(param_1);
	
	// V60: ReadRegister32 intercept REMOVED — V59 proved hooking all register reads
	// causes display driver to loop in power-well cycling, producing 0 children.
	// Error suppression done via timer-based R/W clear (V57 approach).
	
	auto ret=FunctionCast(raReadRegister32, callback->oraReadRegister32)(that,param_1);
	return ret;
};

unsigned long Gen11::raReadRegister32b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	//if (reinterpret_cast<volatile uint64_t*>(param_1)==nullptr) return 0;
	return  raReadRegister32(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};

uint64_t Gen11::raReadRegister64(void *that,unsigned long param_1)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	
	return FunctionCast(raReadRegister64, callback->oraReadRegister64)(that,param_1);
};
uint64_t Gen11::raReadRegister64b(void *that,void *param_1,unsigned long param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return 0;
	return  raReadRegister64(that,reinterpret_cast<uint64_t>(param_1) + param_2);
};

void Gen11::radWriteRegister32(void *that,unsigned long param_1, UInt32 param_2)
{
	radWriteRegister32f( that,param_1,param_2);
};

void Gen11::radWriteRegister32f(void *that,unsigned long param_1, UInt32 param_2)
{
	//FunctionCast(radWriteRegister32f, callback->oradWriteRegister32f)( that,param_1,param_2);
};

void Gen11::raWriteRegister64(void *that,unsigned long param_1,UInt64 param_2)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;

	if (NGreen::callback) {
		const uint32_t reg = static_cast<uint32_t>(param_1 & 0xFFFFF);
		const bool looksLikePlaneSurf =
			(reg >= 0x60000 && reg <= 0xBFFFF) &&
			((reg & 0xFFF) == 0x19C);

		if (looksLikePlaneSurf && param_2 == 0) {
			const uint32_t ctlReg = reg - 0x1C;
			const uint32_t planCtl = NGreen::callback->readReg32(ctlReg);
			if (planCtl & 0x80000000U) {
				const uint32_t currentSurf = NGreen::callback->readReg32(reg);
				if (currentSurf != 0) {
					param_2 = static_cast<UInt64>(currentSurf);
				} else {
					NGreen::callback->writeReg32(ctlReg, planCtl & ~0x80000000U);
				}
			}
		}
	}

	FunctionCast(raWriteRegister64, callback->oraWriteRegister64)(that, param_1, param_2);
};

void Gen11::raWriteRegister64b(void *that,void *param_1,unsigned long param_2,UInt64 param_3)
{
	//if (reinterpret_cast<volatile uint64_t*>(that)==nullptr) return;
	raWriteRegister64( that,reinterpret_cast<uint64_t>(param_1) + param_2,param_3);
};

void Gen11::setupPlanarSurfaceDBUF()
{
	//FunctionCast(setupPlanarSurfaceDBUF, callback->osetupPlanarSurfaceDBUF)();
};

void Gen11::updateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	//setupPlanarSurfaceDBUF();
};

int Gen11::LightUpEDP(void *that,void *param_1, void *param_2,void *param_3)
{
	FunctionCast(LightUpEDP, callback->oLightUpEDP)(that,param_1,param_2,param_3);
	return 0;
};

uint8_t Gen11::disableVDDForAux(void *that)
{
	uint32_t iVar3 = raReadRegister32(ccont,0xc7200);
	if (-1 < iVar3) {
		IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
		r->setProperty("AAPL,LCD-PowerState-ON", false);
	}
	return FunctionCast(disableVDDForAux, callback->odisableVDDForAux)(that);
};

void Gen11::setPanelPowerState(void *that,bool param_1)
{
	FunctionCast(setPanelPowerState, callback->osetPanelPowerState)(that,param_1);
	
	IORegistryEntry *r= (IORegistryEntry *)getMember<long *>(that, 0xd60);
	r->setProperty("AAPL,LCD-PowerState-ON", param_1);
};

unsigned long Gen11::fastLinkTraining()
{
	
	FunctionCast(fastLinkTraining, callback->ofastLinkTraining)();
	return 1;
};

void Gen11::logStateInRegistry(void *that,uint param_1)
{
 FunctionCast(logStateInRegistry, callback->ologStateInRegistry)(that,param_1 );
}

void Gen11::initializeLogging(AppleIntel::AppleIntelBaseController *that)
{
	FunctionCast(initializeLogging, callback->oinitializeLogging)(that );
}

int Gen11::getPlatformID()
{
 return FunctionCast(getPlatformID, callback->ogetPlatformID)( );
}

uint32_t Gen11::tprobePortMode(void * that)
{
 return Genx::callback->tprobePortMode(that );
}

void  Gen11::AppleIntelPlanec1(AppleIntel::AppleIntelPlane *that)
{
	Genx::callback->AppleIntelPlanec1(that );
}

void  Gen11::AppleIntelScalerc1(AppleIntel::AppleIntelScaler *that)
{
	Genx::callback->AppleIntelScalerc1(that );
}

void * Gen11::AppleIntelScalernew(unsigned long param_1)
{
	return Genx::callback->AppleIntelScalernew(param_1 );
}

void * Gen11::AppleIntelPlanenew(unsigned long param_1)
{
	return Genx::callback->AppleIntelPlanenew(param_1 );
}

void Gen11::uupdateDBUF(void *that,uint param_1,uint param_2,bool param_3)
{
	Genx::callback->uupdateDBUF(that,param_1,param_2 );
}

bool inpwell=false;
void Gen11::PowerWellinit(void *that,void *param_1)
{
	inpwell=true;
  FunctionCast(PowerWellinit, callback->oPowerWellinit)(that,param_1 );
	inpwell=false;
}

long Gen11::getPortByDDI(uint param_1)
{
 auto ret= FunctionCast(getPortByDDI, callback->ogetPortByDDI)(param_1 );
	if (inpwell) setPortMode((void*)ret,1);
 return ret;
}

uint8_t  Gen11::setPortMode(void *that,uint32_t param_1)
{
 auto ret= FunctionCast(setPortMode, callback->osetPortMode)(that,param_1 );
	
 return ret;
}

int smo=0;

int Gen11::isConflictRegister()
{
	return -1;

}

int Gen11::wrapGetFreeJoinablePathCount(void *that)
{
	// Always return 0 — with 1/1/1 pinfo config, only pipe 0 has a path. Original
	// would call getPathByPipe(1) and (2), get NULL back, dereference [+0x3644] →
	// kernel panic at setupBootDisplay->allocateBootDisplayResources chain.
	// Returning 0 means "no joinable paths" which is correct for single-pipe.
	static int v210Count = 0;
	if (v210Count < 4) {
		v210Count++;
		SYSLOG("ngreen", "V210[%d]: getFreeJoinablePathCount → 0 (1/1/1 single-pipe safe)", v210Count);
	}
	return 0;
}

int Gen11::wrapEnableController(void *that)
{
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	if (fbId != 0) {
		// Short-circuit for phantom FB1/FB2: don't call original, return success
		// without doing the per-fb setup (gController ref-count bumps,
		// AppleIntelBaseController::enableController, AAPL0n,IgnoreConnection
		// property lookup, etc). Without this, the per-fb setup chain leaves
		// fb1/fb2 partially-initialized → IGAccelDisplayPipe iterates them in
		// displayModeDidChange → IOFramebuffer::getAttributeExt+0x25 NULL deref.
		// Returning 0 (kIOReturnSuccess) so callers don't error-handle.
		SYSLOG("ngreen", "V209: enableController short-circuited for fb=%u", fbId);
		return 0;
	}
	int ret = FunctionCast(wrapEnableController, callback->oEnableController)(that);
	SYSLOG("ngreen", "V209: enableController fb=0 ret=0x%x", ret);
	return ret;
}

IOReturn Gen11::fbGetPixelInformation(void *that, int mode, int depth, int aperture,
	IOPixelInformation *info)
{
	IOReturn result = FunctionCast(fbGetPixelInformation, callback->ofbGetPixelInformation)(
		that, mode, depth, aperture, info);
	static constexpr IODisplayModeID kNativeModeId = static_cast<IODisplayModeID>(0x80001000);
	if (static_cast<IODisplayModeID>(mode) == kIODisplayModeIDBootProgrammable && result != kIOReturnSuccess) {
		IOReturn retry = kIOReturnSuccess;
		uint32_t w = 0, h = 0;
		if (info != nullptr) {
			retry = FunctionCast(fbGetPixelInformation, callback->ofbGetPixelInformation)(
				that, static_cast<int>(kNativeModeId), depth, aperture, info);
			if (retry == kIOReturnSuccess) {
				result = kIOReturnSuccess;
			} else if (depth == 0 && callback != nullptr) {
				const uint32_t src = NGreen::callback ? NGreen::callback->readReg32(_PIPEASRC) : 0;
				w = ((src >> 16) & 0x1FFFu) + 1u;
				h = (src & 0x1FFFu) + 1u;
				if (src != 0 && w >= 640u && w <= 8192u && h >= 480u && h <= 8192u) {
					bzero(info, sizeof(*info));
					info->bytesPerRow       = w * 4u;
					info->bitsPerPixel      = 32;
					info->pixelType         = kIORGBDirectPixels;
					info->componentCount    = 3;
					info->bitsPerComponent  = 8;
					info->componentMasks[0] = 0x00FF0000u;
					info->componentMasks[1] = 0x0000FF00u;
					info->componentMasks[2] = 0x000000FFu;
					strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
					info->activeWidth       = w;
					info->activeHeight      = h;
					result = kIOReturnSuccess;
				}
			}
		}
	}
	return result;
}

bool Gen11::wrapAppleIntelFramebufferStart(void *that, void *provider)
{
	uint32_t fbId = getMember<uint32_t>(that, 0x1DC);
	if (fbId != 0) {
		// Refuse to start phantom FB1/FB2 — they have no display path assigned and
		// trip CoreDisplay_CreateDisplayForCGXDisplayDevice's __assert_rtn during WS init.
		// Returning false from start() means IOService::registerService is never called,
		// so CoreDisplay never sees this FB. Apple's controller still has 3 FBs internally
		// (per pinfo counts) — we just block IOFramebufferShared registration for !FB0.
		SYSLOG("ngreen", "V208: AppleIntelFramebuffer::start refused for fb=%u (only FB0 starts)",
		       fbId);
		return false;
	}
	bool ret = FunctionCast(wrapAppleIntelFramebufferStart, callback->oAppleIntelFramebufferStart)(that, provider);
	SYSLOG("ngreen", "V208: AppleIntelFramebuffer::start fb=0 ret=%d", ret);
	return ret;
}

void Gen11::injectAcceleratorPersonality(bool useTglNames)
{
	if (this->acceleratorPersonalityInjected) {
		DBGLOG("ngreen", "injectAcceleratorPersonality: already injected, skipping");
		return;
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: registering IntelAccelerator (%s) into IOCatalogue",
	       useTglNames ? "TGL" : "ICL");

	auto *dict = OSDictionary::withCapacity(24);
	if (!dict) return;

	const char *bundleId = useTglNames ? "com.xxxxx.driver.AppleIntelTGLGraphics" : "com.apple.driver.AppleIntelICLGraphics";
	const char *mtlName  = useTglNames ? "AppleIntelTGLGraphicsMTLDriver"        : "AppleIntelICLGraphicsMTLDriver";
	const char *glName   = useTglNames ? "AppleIntelTGLGraphicsGLDriver"         : "AppleIntelICLGraphicsGLDriver";
	const char *vaName   = useTglNames ? "AppleIntelTGLGraphicsVADriver"         : "AppleIntelICLGraphicsVADriver";

	// Basic matching properties
	auto *bi  = OSString::withCString(bundleId);
	auto *cls = OSString::withCString("IntelAccelerator");
	auto *mc  = OSString::withCString("IOAccelerator");
	auto *pv  = OSString::withCString("IOPCIDevice");
	auto *pcm = OSString::withCString("0x03000000&0xff000000");
	auto *primaryMatch = useTglNames
		? OSString::withCString("0xff208086 0x9a498086 0xa7888086")
		: nullptr;
	auto *ps  = OSNumber::withNumber(static_cast<unsigned long long>(1000), 32);

	dict->setObject("CFBundleIdentifier", bi);
	dict->setObject("IOClass", cls);
	dict->setObject("IOMatchCategory", mc);
	dict->setObject("IOProviderClass", pv);
	dict->setObject("IOPCIClassMatch", pcm);
	if (primaryMatch)
		dict->setObject("IOPCIPrimaryMatch", primaryMatch);
	dict->setObject("IOProbeScore", ps);

	OSSafeReleaseNULL(bi);
	OSSafeReleaseNULL(cls);
	OSSafeReleaseNULL(mc);
	OSSafeReleaseNULL(pv);
	OSSafeReleaseNULL(pcm);
	OSSafeReleaseNULL(primaryMatch);
	OSSafeReleaseNULL(ps);

	// V44: GPU driver bundle names — required by IOAcceleratorFamily2 and WindowServer
	auto *mtl = OSString::withCString(mtlName);
	auto *gl  = OSString::withCString(glName);
	auto *dvd = OSString::withCString(vaName);
	auto *src = OSString::withCString("0.0.0.0.0");
	auto *vaCodec  = OSString::withCString("Gen10");
	auto *vaScaler = OSString::withCString("Gen10");
	auto *vaBGRA   = OSString::withCString("Gen10");
	auto *vaRendID = OSNumber::withNumber(static_cast<unsigned long long>(kVARendererIDTGLDriver), 32);

	dict->setObject("MetalPluginName", mtl);
	dict->setObject("IOGLBundleName", gl);
	dict->setObject("IODVDBundleName", dvd);
	dict->setObject("IOSourceVersion", src);
	dict->setObject("IOGVACodec", vaCodec);
	dict->setObject("IOGVAScaler", vaScaler);
	dict->setObject("IOGVABGRAEnc", vaBGRA);
	dict->setObject("IOVARendererID", vaRendID);

	OSSafeReleaseNULL(mtl);
	OSSafeReleaseNULL(gl);
	OSSafeReleaseNULL(dvd);
	OSSafeReleaseNULL(src);
	OSSafeReleaseNULL(vaCodec);
	OSSafeReleaseNULL(vaScaler);
	OSSafeReleaseNULL(vaBGRA);
	OSSafeReleaseNULL(vaRendID);

	// IOAccelerator2D plugin type (required for 2D acceleration matching)
	auto *pluginDict = OSDictionary::withCapacity(1);
	if (pluginDict) {
		auto *pluginName = OSString::withCString("IOAccelerator2D.plugin");
		pluginDict->setObject("ACCF0000-0000-0000-0000-000a2789904e", pluginName);
		OSSafeReleaseNULL(pluginName);
		dict->setObject("IOCFPlugInTypes", pluginDict);
		pluginDict->release();
	}

	// V77: Display pipe capabilities forced to 0 to prevent WS GPU compositing crash.
	auto *dpCaps = OSDictionary::withCapacity(2);
	if (dpCaps) {
		auto *dpSupp = OSNumber::withNumber(static_cast<unsigned long long>(0), 32);
		auto *trSupp = OSNumber::withNumber(static_cast<unsigned long long>(0), 32);
		dpCaps->setObject("DisplayPipeSupported", dpSupp);
		dpCaps->setObject("TransactionsSupported", trSupp);
		OSSafeReleaseNULL(dpSupp);
		OSSafeReleaseNULL(trSupp);
		dict->setObject("IOAccelDisplayPipeCapabilities", dpCaps);
		dpCaps->release();
	}

	SYSLOG("ngreen", "injectAcceleratorPersonality: personality has %u properties", dict->getCount());

	auto *array = OSArray::withCapacity(1);
	if (array) {
		array->setObject(dict);
		if (gIOCatalogue) {
			bool ok = gIOCatalogue->addDrivers(array, false);
			SYSLOG("ngreen", "injectAcceleratorPersonality: addDrivers returned %d", ok);
			this->acceleratorPersonalityInjected = ok;
		} else {
			SYSLOG("ngreen", "injectAcceleratorPersonality: gIOCatalogue is null!");
		}
		array->release();
	}
	dict->release();
}

void  Gen11::disablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(disablePowerWellPG, callback->odisablePowerWellPG)(that, param_1);
}
void  Gen11::enablePowerWellPG(AppleIntel::AppleIntelBaseController *that, uint param_1)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(enablePowerWellPG, callback->oenablePowerWellPG)(that, param_1);
}

void Gen11::hwSetPowerWellStatePG(AppleIntel::AppleIntelBaseController *that, bool param_1, uint param_2)
{
	getMember<void *>(that, 0x78) = ccont;
	FunctionCast(hwSetPowerWellStatePG, callback->ohwSetPowerWellStatePG)(that,param_1,param_2);
}

void Gen11::hwConfigureCustomAUX(AppleIntel::AppleIntelBaseController *that, bool param_1)
{
	SYSLOG("ngreen", "hwAUX p1=%d CE4=%d",
		(int)param_1, getMember<int>(that, 0xCE4));

	// Pure passthrough — V12 showed that the native "Custom AUX enable" logic works
	// correctly on ADL-P hardware. The 0x863xx PHY writes added in V12 broke EDID
	// (56283 µs failure). Native-only: EDID succeeded in 3663 µs on same hardware.
	if (callback->ohwConfigureCustomAUX)
		FunctionCast(hwConfigureCustomAUX, callback->ohwConfigureCustomAUX)(that, param_1);
}

int Gen11::hasExternalDispla()
{

	return 1;
}

long blti=0;

uint8_t Gen11::isPanelPowerOn()
{
	return 1;
}

uint8_t Gen11::SetupDPSSTTimings(void *that,void *param_1,void *param_2,void *param_3){
	return FunctionCast(SetupDPSSTTimings, callback->oSetupDPSSTTimings)(that,param_1,param_2,param_3);
}

uint32_t Gen11::validateDetailedTiming(void *that,void *param_1,unsigned long param_2) {
	return FunctionCast(validateDetailedTiming, callback->ovalidateDetailedTiming)(that,param_1,param_2);
}

uint8_t Gen11::maxSupportedDepths(void *param_1) {
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
    auto ret= FunctionCast(maxSupportedDepths, callback->omaxSupportedDepths)(param_1);
	return ret;
}

uint8_t Gen11::validateModeDepth(void *that,void *param_1,uint param_2)
{
    auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_1));
    if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	auto ret= FunctionCast(validateModeDepth, callback->ovalidateModeDepth)(that, param_1, param_2);
	return ret;
}

void Gen11::SetupTimings(void *that, void *param_1, void *param_2, void *param_3, void *param_4){
	FunctionCast(SetupTimings, callback->oSetupTimings)(that,param_1,param_2, param_3, param_4);
}

uint8_t Gen11::validateDisplayMode(void *that, int param_1,void *param_2, void *param_3){
	auto displayTimingInfo = const_cast<IODetailedTimingInformationV2 *>(reinterpret_cast<const IODetailedTimingInformationV2 *>(param_3));
	if (displayTimingInfo!=nullptr) displayTimingInfo->pixelClock = 785400000;
	auto ret= FunctionCast(validateDisplayMode, callback->ovalidateDisplayMode)(that,param_1,param_2,param_3);
	return ret;
}

void Gen11::setupDisplayTiming (void *that,void *param_1,
                         void *param_2){
	FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //auto ret= FunctionCast(setupDisplayTiming, callback->osetupDisplayTiming)(that,param_1,param_2);
    //return ret;
}

unsigned long Gen11::getPixelInformation (void *that, uint param_1,int param_2,int param_3, void *param_4){
	return FunctionCast(getPixelInformation, callback->ogetPixelInformation)(that,param_1,param_2, param_3, param_4);
}

int Gen11::blit3d_supported(void *param_1,void *param_2)
{
	return 0;
}

uint8_t Gen11::isPanelPowerOn(void *that)
{
	return 1;//FunctionCast(isPanelPowerOn, callback->oisPanelPowerOn)(that);
}

void * Gen11::ExtendedContextWithOptions(void *param_1)
{
	return FunctionCast(ExtendedContextWithOptions, callback->oExtendedContextWithOptions)(param_1);
}

uint8_t Gen11::enableController(AppleIntel::AppleIntelFramebuffer *that)
{
	if (getMember<uint32_t>(that, 0x1dc)==0) getMember<uint8_t>(that, 0x1e0)=1;
	auto ret= FunctionCast(enableController, callback->oenableController)(that);
	return ret;
}


int Gen11::setDisplayMode(AppleIntel::AppleIntelFramebuffer *that, int param_1, int param_2)
{
	return FunctionCast(setDisplayMode, callback->osetDisplayMode)(that,param_1,param_2 );

}

uint8_t Gen11::connectionChanged(void *that)
{
	
	auto ret= FunctionCast(connectionChanged, callback->oconnectionChanged)(that);
	//getMember<uint8_t>(that, 0x1e0)=1;
	return ret;
}

unsigned long  Gen11::allocateDisplayResources(void *that)
{
	auto ret=FunctionCast(allocateDisplayResources, callback->oallocateDisplayResources)(that);

	if (!NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX()) {
		// ADL-P / RPL-P / RPL-U (XE_LPD, Display v13).
		// ADL-P IS XE_LPD — same display engine, same register layout.
		// Apple's TGL driver (AppleIntelTGLGraphicsFramebuffer) does not program this
		// hardware correctly when spoofed, so we replicate icl_display_core_init here.

		//icl_display_core_init
		NGreen::callback->writeReg32(DC_STATE_EN,0);

		/* Wa_14011294188:ehl,jsl,tgl,rkl,adl-s */
		NGreen::callback->intel_de_rmw( SOUTH_DSPCLK_GATE_D, 0,
					 PCH_DPMGUNIT_CLOCK_GATE_DISABLE);

		uint32_t reg,reset_bits;
		reg = HSW_NDE_RSTWRN_OPT;
		reset_bits = RESET_PCH_HANDSHAKE_ENABLE;
		NGreen::callback->intel_de_rmw( reg, reset_bits, reset_bits);

		//intel_cdclk_init_hw(dev_priv);
		//gen12_dbuf_slices_config — XE_LPD: 4 slices (Linux syslog: dbuf slices=0xf)
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S0,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S1,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S2,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));
		NGreen::callback->intel_de_rmw(_DBUF_CTL_S3,
					 DBUF_TRACKER_STATE_SERVICE_MASK,
					 DBUF_TRACKER_STATE_SERVICE(8));

		//gen9_dbuf_enable(dev_priv); — enable all 4 slices
		reg = _DBUF_CTL_S0;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S1;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S2;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);
		reg = _DBUF_CTL_S3;
		NGreen::callback->intel_de_rmw(reg, DBUF_POWER_REQUEST,
				  DBUF_POWER_REQUEST);
		NGreen::callback->readReg32(reg);
		IODelay(10);


		//icl_mbus_init
		// XE_LPD_FEATURES (Linux intel_display_device.c): .abox_mask = GENMASK(1, 0)
		// = ABOX0 (0x45038) + ABOX1 (0x45048)
		unsigned long abox_mask = GENMASK(1, 0);//DISPLAY_INFO(dev_priv)->abox_mask;
		int config, i;

		unsigned long abox_regs=abox_mask;
		uint32_t mask = MBUS_ABOX_BT_CREDIT_POOL1_MASK |
			MBUS_ABOX_BT_CREDIT_POOL2_MASK |
			MBUS_ABOX_B_CREDIT_MASK |
			MBUS_ABOX_BW_CREDIT_MASK;
		uint32_t val = MBUS_ABOX_BT_CREDIT_POOL1(16) |
			MBUS_ABOX_BT_CREDIT_POOL2(16) |
			MBUS_ABOX_B_CREDIT(1) |
			MBUS_ABOX_BW_CREDIT(1);

		for_each_set_bit(i, &abox_regs, sizeof(abox_regs))
		NGreen::callback->intel_de_rmw( MBUS_ABOX_CTL(i), mask, val);


		//tgl_bw_buddy_init
		// Linux syslog: DRAM channels=4, memory type 0x23=LPDDR5
		// tgl_buddy_page_masks: {4ch, LPDDR4, 0x38} and {4ch, LPDDR5, 0x38} — same page_mask
		// Use LPDDR4 bucket (LPDDR5 maps identically in the table)
		enum intel_dram_type type = INTEL_DRAM_LPDDR4;
		uint8_t num_channels = 4; //4ch LPDDR — tgl_buddy_page_masks → page_mask=0x38
		const struct buddy_page_mask *table=tgl_buddy_page_masks;

		for (config = 0; table[config].page_mask != 0; config++)
			if (table[config].num_channels == num_channels &&
				table[config].type == type)
				break;

		for_each_set_bit(i, &abox_mask, sizeof(abox_mask)) {
			NGreen::callback->writeReg32( BW_BUDDY_PAGE_MASK(i),
						   table[config].page_mask);

			/* Wa_22010178259:tgl,dg1,rkl,adl-s */
			NGreen::callback->intel_de_rmw( BW_BUDDY_CTL(i),
							 BW_BUDDY_TLB_REQ_TIMER_MASK,
							 BW_BUDDY_TLB_REQ_TIMER(0x8));
		}

		/* Wa_14011508470:tgl,dg1,rkl,adl-s,adl-p,dg2 */
		//if (IS_DISPLAY_VER_FULL(dev_priv, IP_VER(12, 0), IP_VER(13, 0)))
		/*NGreen::callback->intel_de_rmw( GEN11_CHICKEN_DCPR_2, 0,
					 DCPR_CLEAR_MEMSTAT_DIS | DCPR_SEND_RESP_IMM |
					 DCPR_MASK_LPMODE | DCPR_MASK_MAXLATENCY_MEMUP_CLR);*/

		/* * Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl
		 * Also known as Wa_14010480278.
		 */
		//if (IS_DISPLAY_VER(i915, 10, 12))
		//NGreen::callback->intel_de_rmw( GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);

		/* Wa_14013723622 */
		NGreen::callback->intel_de_rmw( CLKREQ_POLICY, CLKREQ_POLICY_MEM_UP_OVRD, 0);

	}
	// isRealTGL: Apple's AppleIntelTGLGraphicsFramebuffer handles all of the above
	// natively (2 DBUF slices, XE_D abox_mask=GENMASK(2,1), correct DRAM config).
	// No additional writes needed.

	return ret;
}
	
void Gen11::IGScheduler5resume(void *that) {
		
		void *accelerator = getMember<void *>(that, 0x10);
		struct IGHwCsDesc *descArray = (struct IGHwCsDesc *)callback->kIGHwCsDesc;
		uint32_t mode = _MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE);

		for (int i = 0; i < 6; i++) {
			struct IGHwCsDesc *desc = &descArray[i];

			// --- FILTER: ONLY RCS AND BCS ---
			if (desc->type == kIGHwCsTypeRCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 0);
			} else if (desc->type == kIGHwCsTypeBCS) {
				//FunctionCast(SafeForceWake, callback->oSafeForceWake)(accelerator, true, 1);
			} else {
				continue;
			}

					uint32_t ringBase = desc->mmioExecListControl - 0x3c;
					
					// USE THE CORRECT STRUCT MEMBER FOR HWS
					// The dump proves mmioGlobalStatusPage holds the offset 0x2080
					uint32_t hwsRegisterOffset = desc->mmioGlobalStatusPage;
					
					// READ SHADOW REGISTER
					// We use the offset from the struct (0x2080) to index the shadow map
					uint32_t hwsAddr = getMember<uint32_t>(accelerator, 0x1240 + hwsRegisterOffset);
					
					// WRITE TO HARDWARE
					NGreen::callback->writeReg32(hwsRegisterOffset, hwsAddr);
					NGreen::callback->readReg32(hwsRegisterOffset);

					// REST OF INIT (Using explicit offsets from struct where possible)
					NGreen::callback->writeReg32(ringBase + 0x98, ~0u); // HWSTAM
					NGreen::callback->writeReg32(desc->mmioGfxMode, mode); // GFX Mode
					NGreen::callback->writeReg32(ringBase + 0x9c, _MASKED_BIT_DISABLE(STOP_RING)); // MI Mode
					
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0u); // EMR
					NGreen::callback->writeReg32(desc->mmioErrorMask, ~0u);    // EIR
					NGreen::callback->writeReg32(desc->mmioErrorIdentity, ~0x1); // I915_ERROR_INSTRUCTION
		}
	
	FunctionCast(IGScheduler5resume, callback->oIGScheduler5resume)(that);

}

// V212: The GPU watchdog calls isGpuIdle() after engine init to decide if the GPU is healthy.
// On RPL-P, INSTDONE bit0 is permanently 0 → original returns false → watchdog resets the GPU
// → startGraphicsEngine retry loop. startGraphicsEngine itself SUCCEEDS (return value is non-zero
// = success path); the reset is triggered entirely by this watchdog query.
bool Gen11::wrapIGScheduler5IsGpuIdle(const void *that) {
	bool idle = FunctionCast(wrapIGScheduler5IsGpuIdle, callback->oIGScheduler5IsGpuIdle)(that);
	if (!idle) {
		uint32_t instdone = NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE));
		if ((instdone & ~1U) == 0xfffffffe) {
			SYSLOG("ngreen", "V212[5]: isGpuIdle override INSTDONE=0x%08x → idle", instdone);
			return true;
		}
	}
	return idle;
}

bool Gen11::wrapIGScheduler4IsGpuIdle(const void *that) {
	bool idle = FunctionCast(wrapIGScheduler4IsGpuIdle, callback->oIGScheduler4IsGpuIdle)(that);
	if (!idle) {
		uint32_t instdone = NGreen::callback->readReg32(RING_INSTDONE(RENDER_RING_BASE));
		if ((instdone & ~1U) == 0xfffffffe) {
			SYSLOG("ngreen", "V212[4]: isGpuIdle override INSTDONE=0x%08x → idle", instdone);
			return true;
		}
	}
	return idle;
}

typedef enum AGDCVendorClass {
	kAGDCVendorClassReserved,
	kAGDCVendorClassIntegratedGPU,
	kAGDCVendorClassDiscreteGPU,
	kAGDCVendorClassOtherHW,
	kAGDCVendorClassOtherSW,
	kAGDCVendorClassAppleGPUPolicyManager,
	kAGDCVendorClassAppleGPUPowerManager,
	kAGDCVendorClassGPURoot,
	kAGDCVendorClassAppleGPUWrangler,
	kAGDCVendorClassAppleMuxControl,
} AGDCVendorClass_t;

typedef struct AGDCVendorInfo {
	union {
		struct {
			UInt16 Minor;
			UInt16 Major;
		};
		UInt32 Raw;
	} Version;
	char VendorString[32];
	UInt32 VendorID;
	AGDCVendorClass_t VendorClass;
} AGDCVendorInfo_t;

uint32_t
Gen11::IntelFBClientControldoAttribute
		  (void *that,uint param_1,unsigned long *param_2,unsigned long param_3,unsigned long *param_4,
		   unsigned long *param_5,void *param_6)
{

	/*if (param_1 == 0x923) {
		return kIOReturnUnsupported;
	}*/
	
	auto ret=FunctionCast(IntelFBClientControldoAttribute, callback->oIntelFBClientControldoAttribute)(that,param_1,param_2,param_3,param_4,param_5,param_6);
	
	if (param_1 == 1)//0x2001)
	if (param_5 != (unsigned long *)0x0)
		if (0x2b < *param_5) {
			//memset(param_4,0,0x2c);
			AGDCVendorInfo *v=(AGDCVendorInfo*)param_4;
			v->Version.Raw=0;
			v->Version.Major=0;
			v->Version.Minor=0;
			v->VendorID=0x8086;
			*v->VendorString=*(char*)"INTEL";
			v->VendorClass=kAGDCVendorClassIntegratedGPU;
			//*(mach_vm_address_t*)v=callback->IntelFBClientControl11doAttribut;
		return 0;
	}
	return ret;
}

int hw=1;
int Gen11::hwSetMode
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3)
{
	// On RPL-P (spoofed TGL), setPortMode ran just before us and restored TRANS_DDI_FUNC_CTL_A
	// to 0x8A000106 (4-lane eDP, matching UEFI DDI_BUF_CTL_A). paramsFbCompare inside the
	// original hwSetMode reads TRANS_DDI_FUNC_CTL_A and compares it with the target 0x8A010102
	// (2-lane DP SST). Write the correct value NOW so paramsFbCompare sees no lane-count change
	// needed and leaves DDI_BUF_CTL_A alone — preserving the UEFI-trained 4-lane eDP link.
	if (!NGreen::callback->isRealTGL && !NGreen::callback->isRaptorLakeHX()) {
		NGreen::callback->writeReg32(0x60400, 0x8A000106);  // TRANS_DDI_FUNC_CTL_A
		DBGLOG("ngreen", "hwSetMode: pre-write TRANS_DDI_FUNC_CTL_A=0x8A000106 to prevent paramsFbCompare lane reprog");
	}
	auto ret= FunctionCast(hwSetMode, callback->ohwSetMode)(that, param_1, param_2, param_3);
	if (hw)
		enablePipe(that, param_1, param_2, param_3);
	hw=0;
	return ret;
}

void Gen11::enablePipe
		  (AppleIntel::AppleIntelBaseController *that,
		   AppleIntel::AppleIntelFramebuffer *param_1,
		   AppleIntel::AppleIntelDisplayPath *param_2,
		   AppleIntel::CRTCParams *param_3)
{
	return FunctionCast(enablePipe, callback->oenablePipe)(that, param_1, param_2, param_3);
}

uint8_t Gen11::beginReset(void *that)
{
	auto ret= FunctionCast(beginReset, callback->obeginReset)(that);
	
	/*static const struct intel_device_info tgl_info = {
		GEN12_FEATURES,
		PLATFORM(INTEL_TIGERLAKE),
		.platform_engine_mask =
			BIT(RCS0) | BIT(BCS0) | BIT(VECS0) | BIT(VCS0) | BIT(VCS2),
	};*/
	
	
	
	/*if (GRAPHICS_VER_FULL(dev_priv) >= IP_VER(12, 10))
		dg1_irq_reset(dev_priv);
	else if (GRAPHICS_VER(dev_priv) >= 11)
		gen11_irq_reset(dev_priv);*/
	
	//dg1_irq_reset(struct drm_i915_private *dev_priv)
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, 0);
	
	//gen11_irq_reset
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, 0);
	
	//gen11_gt_irq_reset(gt);
	// Disable RCS, BCS, VCS and VECS class engines.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE,	  0);

	// Restore masks irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK,	~0);
	
	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK,	~0);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK,	~0);
	
	//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~0);
	
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GPM_WGBOXPERF_INTR_MASK,  ~0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_GUC_SG_INTR_MASK,  ~0);

	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_ENABLE, 0);
	NGreen::callback->writeReg32( GEN11_CRYPTO_RSVD_INTR_MASK,  ~0);
	
	//gen11_display_irq_reset(dev_priv);
	
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, ~0);

	return ret;
}
											
void Gen11::endReset(void *that)
{
	FunctionCast(endReset, callback->oendReset)(that);
	
	//void gen11_gt_irq_postinstall(struct intel_gt *gt)
	uint32_t irqs = GT_RENDER_USER_INTERRUPT;
	uint32_t dmask,smask;
	
	dmask = irqs << 16 | irqs;
	smask = irqs << 16;
	
	
	
	// Enable RCS, BCS, VCS and VECS class interrupts.
	NGreen::callback->writeReg32( GEN11_RENDER_COPY_INTR_ENABLE, dmask);
	NGreen::callback->writeReg32( GEN11_VCS_VECS_INTR_ENABLE, dmask);

		// Unmask irqs on RCS, BCS, VCS and VECS engines.
	NGreen::callback->writeReg32( GEN11_RCS0_RSVD_INTR_MASK, ~smask);
	NGreen::callback->writeReg32( GEN11_BCS_RSVD_INTR_MASK, ~smask);

	NGreen::callback->writeReg32( GEN11_VCS0_VCS1_INTR_MASK, ~dmask);
	NGreen::callback->writeReg32( GEN11_VCS2_VCS3_INTR_MASK, ~dmask);
	
	NGreen::callback->writeReg32( GEN11_VECS0_VECS1_INTR_MASK, ~dmask);
	
		//if (HAS_ENGINE(gt, VECS2) || HAS_ENGINE(gt, VECS3))
	NGreen::callback->writeReg32( GEN12_VECS2_VECS3_INTR_MASK, ~dmask);
	
	
	//gen11_de_irq_postinstall(dev_priv);
	NGreen::callback->writeReg32( GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
	
	NGreen::callback->writeReg32( DG1_MSTR_TILE_INTR, DG1_MSTR_IRQ);
}

//SIGNATURES GFX
//ulong __thiscall IntelAccelerator::start(IntelAccelerator *this,IOService *param_1)
//undefined8 __thiscall IGAccelDevice::deviceStart(IGAccelDevice *this)
//void __thiscall IntelAccelerator::getGPUInfo(IntelAccelerator *this)
//void IntelAccelerator::getGPUInfo(void)
//void __thiscall IntelAccelerator::populateResetRegisterList(IntelAccelerator *this)
//undefined8 __thiscall IntelAccelerator::createUserGPUTask(IntelAccelerator *this)
//IGAccelTask * IGAccelTask::withOptions(IntelAccelerator *param_1)
//IGHardwareExtendedContext * __thiscall IGAccelTask::getBlit3DContext(IGAccelTask *this,bool param_1)
//undefined8 __thiscall IGHardwareExtendedContext::initWithOptions (IGHardwareExtendedContext *this,IGAccelTask *param_1, IGHardwareExtendedContextParams *param_2)
//undefined8 blit3d_init_ctx(IGHardwareBlit3DContext *param_1)
//void blit3d_initialize_scratch_space(IGAccelSysMemory *param_1)
//void __thiscall IGHardwareBlit3DContext::initialize(IGHardwareBlit3DContext *this)
//ulong __thiscall IntelAccelerator::startGraphicsEngine(IntelAccelerator *this)
//undefined8 __thiscall IntelAccelerator::stopGraphicsEngine(IntelAccelerator *this)
//void __thiscall IGAccelSegmentResourceList::initBlitUsage(IGAccelSegmentResourceList *this)
//ulong IntelAccelerator::submitBlit (blit3d_params_t *param_1,IGVector *param_2,IGAccelTask *param_3,bool param_4)
