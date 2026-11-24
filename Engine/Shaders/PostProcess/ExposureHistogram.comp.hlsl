#include "ExposureCommon.hlsli"
// The source is the unexposed linear scene before bloom, flare, grading or UI.
[[vk::binding(0, 0)]] Texture2D<float4> unexposedScene;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> histogram;
struct HistogramConstants
{
	uint width;
	uint height;
	float minimumLogLuminance;
	float maximumLogLuminance;
};
[[vk::push_constant]] ConstantBuffer<HistogramConstants> parameters;
groupshared uint localHistogram[256];

// A transfer vkCmdFillBuffer(0) is an equivalent native clear. This entry makes
// the shader independently dispatchable when a native transfer clear is unavailable.
[numthreads(256, 1, 1)]
void ClearHistogram(uint lane : SV_GroupIndex)
{
	histogram[lane] = 0;
}

[numthreads(16, 16, 1)]
void BuildHistogram(uint3 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
	localHistogram[lane] = 0;
	GroupMemoryBarrierWithGroupSync();
	if(pixel.x < parameters.width && pixel.y < parameters.height)
	{
		const float3 color = unexposedScene.Load(int3(pixel.xy, 0)).rgb;
		uint bin = 0;
		if(all(isfinite(color)))
		{
			const float3 positiveColor = max(color, 0.0f);
			const float peak = max(positiveColor.x, max(positiveColor.y, positiveColor.z));
			if(peak > 0.0f)
			{
				// Normalize before summing to keep finite full-range HDR inputs from
				// overflowing the weighted luminance dot product.
				const float luminance = peak * saturate(dot(positiveColor / peak, float3(0.2126f, 0.7152f, 0.0722f)));
				if(luminance > 0.0f)
				{
					float minimumLog, maximumLog;
					MeterRange(parameters.minimumLogLuminance, parameters.maximumLogLuminance, minimumLog, maximumLog);
					const float normalized = saturate((log2(luminance) - minimumLog) / (maximumLog - minimumLog));
					// Bin0 is black/invalid. The other255 bins cover the declared
					// range; extremes saturate instead of indexing outside storage.
					bin = 1u + min(254u, (uint)(normalized * 255.0f));
				}
			}
		}
		InterlockedAdd(localHistogram[bin], 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	// At most256 global atomics per tile, rather than one contested global
	// atomic per pixel. Out-of-bounds lanes still reach both group barriers.
	if(localHistogram[lane] != 0u) InterlockedAdd(histogram[lane], localHistogram[lane]);
}
