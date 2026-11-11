// The command-stream words this driver reads or writes, spelled as i915's drivers/gpu/drm/i915/gt/intel_gpu_commands.h spells
// them (MI_INSTR without the client field: MI commands carry client 0):
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L37-L42
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L62
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L155
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L188
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L287
// https://github.com/torvalds/linux/blob/65ba8f3/drivers/gpu/drm/i915/gt/intel_gpu_commands.h#L289
#ifndef intel_gpu_commands_hpp
#define intel_gpu_commands_hpp

#define MI_INSTR(opcode, flags) \
	((opcode) << 23 | (flags))

#define MI_BATCH_BUFFER_END	MI_INSTR(0x0a, 0)
#define MI_LOAD_REGISTER_IMM(x)	MI_INSTR(0x22, 2*(x)-1)
#define MI_BATCH_BUFFER_START_GEN8	MI_INSTR(0x31, 1)
#define GFX_OP_PIPE_CONTROL(len)	((0x3<<29)|(0x3<<27)|(0x2<<24)|((len)-2))
#define   PIPE_CONTROL_TILE_CACHE_FLUSH			(1<<28) /* gen11+ */

#endif
