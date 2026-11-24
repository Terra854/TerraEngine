// Current-view diffuse incident-radiance estimator. Input fallback must be the
// existing diffuse INDIRECT incident term in the same linear units. Material
// albedo/BRDF modulation remains downstream and is performed exactly once.
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
	float3 axis=abs(normal.z)<0.9f?float3(0,0,1):float3(0,1,0);
	float3 tangent=normalize(cross(axis,normal)),bitangent=cross(normal,tangent);
	float3 estimate=0;float confidence=0;
	// Eight deterministic cosine-weighted hemisphere strata. Each missing ray
	// contributes its original environment estimate; no division by only hits
	// that would brighten the image when sources leave the screen.
	for(uint i=0;i<8;++i)
	{
		float radial=sqrt((float(i)+0.5f)/8),angle=float(i)*2.39996323f;
		float3 direction=tangent*(radial*cos(angle))+bitangent*(radial*sin(angle))
			+normal*sqrt(1-radial*radial);
		ScreenTraceHit hit=TraceScreenRay(position+normal*trace.thickness*2,direction);
		float3 contribution=lerp(fallback.rgb,hit.radiance*trace.strength,hit.confidence);
		if(!all(isfinite(contribution)))return;
		// Normalize before accumulation to avoid overflowing eight valid HDR
		// samples merely because their unnormalized sum exceeds float range.
		estimate+=contribution/8;
		confidence+=hit.confidence;
	}
	if(confidence>0)
	{
		if(!all(isfinite(estimate)))return;
#if !defined(SCREEN_SPACE_OUTPUT_RGBA32F)
		if(any(abs(estimate)>65504.0f))return;
#endif
		screenConfidence[pixel]=confidence/8;
		screenResolved[pixel]=float4(estimate,fallback.a);
	}
}
