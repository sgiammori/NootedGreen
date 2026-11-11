#ifndef i915_lrc_reg_hpp
#define i915_lrc_reg_hpp

// From i915, drivers/gpu/drm/i915/gt/intel_lrc.h (the per-process HWSP page and the state page after it) and
// gt/intel_lrc_reg.h (the dword offsets inside the state page: each names the value slot of a register's LRI pair):
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_lrc.h#L23-L27
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_lrc_reg.h#L14-L28

#define LRC_PPHWSP_PN    (0)
#define LRC_PPHWSP_SZ    (1)
#define LRC_STATE_PN     (LRC_PPHWSP_PN + LRC_PPHWSP_SZ)
#define LRC_STATE_OFFSET (LRC_STATE_PN * PAGE_SIZE)

#define CTX_CONTEXT_CONTROL (0x02 + 1)
#define CTX_RING_HEAD       (0x04 + 1)
#define CTX_RING_TAIL       (0x06 + 1)
#define CTX_RING_START      (0x08 + 1)
#define CTX_RING_CTL        (0x0a + 1)
#define CTX_PDP0_UDW        (0x30 + 1)
#define CTX_PDP0_LDW        (0x32 + 1)

// The CSB pointer fields of RING_CONTEXT_STATUS_PTR:
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_lrc_reg.h#L69-L72
#define GEN11_CSB_PTR_MASK 0xf
#define GEN11_CSB_READ_PTR_MASK		(GEN11_CSB_PTR_MASK << 8)
#define GEN11_CSB_WRITE_PTR_MASK	(GEN11_CSB_PTR_MASK << 0)

// The CSB entries in the HWSP (gt/intel_engine.h) and the fields of a gen 12 entry's low dword (gt/intel_execlists_submission.c):
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_engine.h#L201-L203
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_execlists_submission.c#L160-L163
#define I915_HWS_CSB_BUF0_INDEX		0x10
#define ICL_HWS_CSB_WRITE_INDEX		0x2f
#define GEN12_CTX_STATUS_SWITCHED_TO_NEW_QUEUE	(0x1) /* lower csb dword */
#define GEN12_CSB_SW_CTX_ID_MASK		GENMASK(25, 15)
#define GEN12_IDLE_CTX_ID		0x7FF

#endif
