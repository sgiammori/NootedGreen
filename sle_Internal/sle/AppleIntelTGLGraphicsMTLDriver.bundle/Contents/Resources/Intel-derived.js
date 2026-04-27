// Formulae to compute derived counters from raw counter values

function GPUUtilization()
{
	return (GPU_BusyTics * 100.0) / GPU_CoreClocks;
}

function ShaderCoreActive()
{
	return ((EuActive_Flex0 * 100.0) / EUCoreCount) / GPU_BusyTics;
}

function ShaderCoreStall()
{
	return ((EuStall_Flex1 * 100.0) / EUCoreCount)/ GPU_BusyTics;
}

function VertexCost()
{
	return (VSEuActive_Flex6 * 100.0) / EuActive_Flex0;
}

function PixelCost()
{
	return (PSEuActive_Flex12 * 100.0) / EuActive_Flex0;
}

function GTRingBW()
{
	return (gam_total_reads2ring + gam_total_writes2ring ) * 64 / ( GPU_delta_timestamp * 8333e-11 ) / 1e9
}

function DRAMBW()
{
	return (DRAMReads + DRAMWrites ) * 64 / ( GPU_delta_timestamp * 8333e-11 ) / 1e9
}

function PixelShaderInvocations()
{
	return PsInvocations;
}

function VertexShaderInvocations()
{
	return VsInvocations;
}

function ComputeShaderInvocations()
{
	return CsInvocations;
}

function PixelToVertexRatio()
{
    return VsInvocations > 0 ? (PsInvocations / VsInvocations) : 0;
}

function PixelsPerPrimitive()
{
    return IAPrimitives > 0 ? (PsInvocations / IAPrimitives) : 0;
}

function VerticesSubmitted()
{
	return IAVertices;
}

function VerticesPerClock()
{
	return IAVertices / GPU_BusyTics;
}

function VerticesRendered()
{
	return (ClipperPrimitives * IAVertices)/IAPrimitives;
}

function PrimitivesSubmitted()
{
	return IAPrimitives;
}

function PrimitivesClipped()
{
	return (IAPrimitives - ClipperPrimitives);
}

function PrimitivesRendered()
{
    return ClipperPrimitives;
}

function PixelsFailingHiZ()
{
	return (Pixels_2x2_Fail_HiZ_PrePS + Pixels_2x2_Fail_Early_PrePS) * 4;
}

function PixelsFailingPostPS()
{
	return Pixels_2x2_Fail_PostPS * 4;
}

function PixelsWrittenToMemory()
{
	return (Samples_2x2_Written + Samples_2x2_Blended_Written) * 4;
}

function PixelsProcessed()
{
	return (Pixels_2x2_Rasterized * 4) - PixelsFailingHiZ();
}

function PixelsDiscarded()
{
	return Samples_2x2_Killed_PS * 4;
}

function PixelsWrittenPerClock()
{
	return (PixelsWrittenToMemory() / GPU_BusyTics);
}


function TextureUnitBusy()
{
	var samplerBusy = s0_ss0_sampler_is_busy > s0_ss1_sampler_is_busy ? s0_ss0_sampler_is_busy :s0_ss1_sampler_is_busy;
	return (samplerBusy * 100.0) / GPU_BusyTics;
}

function TextureUnitStalled()
{
	var samplerBottleneck = s0_ss0_sampler_is_bottleneck > s0_ss1_sampler_is_bottleneck ? s0_ss0_sampler_is_bottleneck :s0_ss1_sampler_is_bottleneck;
	return (samplerBottleneck * 100.0) / GPU_BusyTics;
}

function SamplerL3Throughput()
{
	return (s0_ss0_sampler_cache_miss + s0_ss1_sampler_cache_miss) * 8 * 64;// bytes
}

function L3GtiThroughput()
{
	return (gam_l3_tlb_hit + gam_l3_tlb_miss) * 64;
}

function L3HitRate()
{
	var L3LookUps = Shader_HDC_MemoryAccess + (SamplerL3Throughput() / 64);
	var L3Misses = L3GtiThroughput() / 64;
	return (1 - (L3Misses/L3LookUps)) * 100.0;
}

