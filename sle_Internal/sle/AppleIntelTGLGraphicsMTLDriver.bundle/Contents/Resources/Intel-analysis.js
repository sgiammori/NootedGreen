// Bottleneck analysis functions
// They can reference both derived counters and dependent raw counters 

function is_short_draw()
{
	return (GPU_BusyTics < 15000)
}

function analysis_memory_bound()
{
	// No Analysis for very short draws
	if(is_short_draw())
	{
		report_problem(1.0,
						"very short draw",
						"no analysis")
		return
	}
	// see if there is a problem
	if (DRAMBW > 15.0)
	{
		var confidence = DRAMBW > 18.0 ? 0.8 : 0.5; 
		// call report_problem to report found issues
		report_problem(confidence,
						"memory bound(DDR)" /* problem */,
						"reduce texture/resource accesses to memory" /* suggested solution */)
	}
}

function analysis_pbe_bound()
{
	if(is_short_draw())
	{
		report_problem(1.0,
						"very short draw",
						"no analysis")
		return
	}
	if (PixelsWrittenPerClock > 4.0)
	{
		var confidence = PixelsWrittenPerClock > 8.0 ? 1.0 : 0.5; 
		// call report_problem to report found issues
		report_problem(confidence,
						"pixel backend bound" /* problem */,
						"reduce sizes of surfaces being rendered to" /* suggested solution */)
	}
}

function analysis_shader_bound()
{
	if(is_short_draw())
	{
		report_problem(1.0,
						"very short draw",
						"no analysis")
		return
	}
	// see if there is a problem
	if (ShaderUtilization > 90.0)
	{
		var outputStr = "", guideStr
		if (ShaderCoreActive > ShaderCoreStall)
		{
			// Either VS or PS bound
			outputStr = PixelCost > VertexCost ? "Pixel Shader Bound" : "Vertex Shader Bound";
			guideStr = "Reduce work in your shader"
		}
		else if (ShaderCoreStall > 50)
		{
			if(TextureUnitStalled > 75)
				outputStr = "Texture Unit bound"
				guideStr = "Consider reducing your texture sizes or take fewer samples"
		}
		report_problem(0.8,
						outputStr,
						guideStr)
	}
}

function analysis_vertex_bound()
{
	if(is_short_draw())
	{
		report_problem(1.0,
						"very short draw",
						"no analysis")
		return
	}
	if (VerticesPerClock > 1.0)
	{
		var confidence = VerticesPerClock > 2.0 ? 0.8 : 0.5; 
		// call report_problem to report found issues
		report_problem(confidence,
						"vertex rate bound" /* problem */,
						"reduce geometry use" /* suggested solution */)
	}
}
