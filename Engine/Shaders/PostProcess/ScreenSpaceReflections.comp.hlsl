// Replaces the declared environment reflection term using current valid hits.
// This is not a second additive environment pass; a miss writes exact fallback.
#include "ScreenSpaceTrace.hlsli"
[numthreads(8,8,1)]
void main(uint3 threadId:SV_DispatchThreadID)
{
	uint2 pixel=threadId.xy;if(!ScreenOutputPixel(pixel))return;
	float4 fallback=ScreenFallbackAt(pixel);
	screenResolved[pixel]=fallback;screenConfidence[pixel]=0;
	if(!ScreenInputsMatch()||!isfinite(trace.strength)||trace.strength<=0||trace.strength>4)return;
	float depth=screenDepth.Load(int3(pixel,0)),roughness;float3 normal,position;
	float2 uv=(float2(pixel)+0.5f)/trace.extent;
	if(!ScreenDepthValid(depth)||!ScreenNormal(pixel,normal,roughness)
		||!ScreenPosition(uv,depth,position))return;
	float3 nearPoint,farPoint;
	float nearDepth=(trace.flags&1u)!=0u?0.99999f:0.00001f;
	if(!ScreenPosition(uv,nearDepth,nearPoint)||!ScreenPosition(uv,1-nearDepth,farPoint))return;
	float3 incident=farPoint-nearPoint;
	if(dot(incident,incident)<1e-12f)return;
	float3 direction=reflect(normalize(incident),normal);
	ScreenTraceHit hit=TraceScreenRay(position+normal*trace.thickness*2,direction);
	// Rough unresolved microfacets cannot be represented by one perfect ray.
	// Gradually defer to the existing prefiltered environment instead of blurring
	// unrelated pixels or claiming full confidence for a rough mirror sample.
	float confidence=hit.confidence*(1-roughness)*(1-roughness);
	if(confidence>0)
	{
		// Intensity scales actual hit radiance, not confidence. The catalog's
		// 0..4 multiplier remains responsive above1 without extrapolating misses.
		float3 resolved=lerp(fallback.rgb,hit.radiance*trace.strength,confidence);
		if(!all(isfinite(resolved)))return;
#if !defined(SCREEN_SPACE_OUTPUT_RGBA32F)
		if(any(abs(resolved)>65504.0f))return;
#endif
		screenConfidence[pixel]=confidence;
		screenResolved[pixel]=float4(resolved,fallback.a);
	}
}
