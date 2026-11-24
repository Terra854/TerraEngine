// Scene-linear lighting composition before exposure/tone mapping. Every declared
// image must belong to one actually rendered view/frame/extent; native admission
// rejects stale or missing producers. Optional variants omit unused descriptors
// entirely instead of binding fabricated normals, fallback or confidence images.
// Source/output formats and extents must match to preserve exact bypass values.
[[vk::binding(0,0)]] Texture2D<float4> sceneHdr;
#if defined(SCREEN_COMPOSITE_AO)
[[vk::binding(1,0)]] Texture2D<float> enhancedAo;
#endif
#if defined(SCREEN_COMPOSITE_SSR)
[[vk::binding(2,0)]] Texture2D<float4> reflectionResolved;
[[vk::binding(10,0)]] Texture2D<float> reflectionConfidence;
#endif
#if defined(SCREEN_COMPOSITE_SSR) || defined(SCREEN_COMPOSITE_AO)
[[vk::binding(3,0)]] Texture2D<float4> environmentFallback;
[[vk::binding(4,0)]] Texture2D<float4> environmentWeight;
#endif
#if defined(SCREEN_COMPOSITE_SSGI)
[[vk::binding(5,0)]] Texture2D<float4> indirectResolved;
[[vk::binding(11,0)]] Texture2D<float> indirectConfidence;
#endif
#if defined(SCREEN_COMPOSITE_SSGI) || defined(SCREEN_COMPOSITE_AO)
[[vk::binding(6,0)]] Texture2D<float4> diffuseFallback;
[[vk::binding(7,0)]] Texture2D<float4> diffuseWeight;
#endif
#if defined(SCREEN_COMPOSITE_RGBA32F)
[[vk::binding(8,0),vk::image_format("rgba32f")]] RWTexture2D<float4> composedHdr;
#else
[[vk::binding(8,0),vk::image_format("rgba16f")]] RWTexture2D<float4> composedHdr;
#endif
#if defined(SCREEN_COMPOSITE_AO) || defined(SCREEN_COMPOSITE_SSR) || defined(SCREEN_COMPOSITE_SSGI)
[[vk::binding(9,0)]] Texture2D<float4> surfaceNormalRoughness;
#endif
struct ScreenCompositeConstants
{
	uint2 extent;
	uint enabledMask; // Reflection1, diffuse indirect2, enhanced AO4; no implicit enable.
	uint inputFlags; // Bit0: original incident weights omit LEGACY SCREEN AO.
};
[[vk::push_constant]] ConstantBuffer<ScreenCompositeConstants> composite;

bool CompositeExtent(uint width,uint height)
{
	return width==composite.extent.x&&height==composite.extent.y;
}

[numthreads(8,8,1)]
void main(uint3 dispatchId:SV_DispatchThreadID)
{
	uint2 pixel=dispatchId.xy;uint width,height;
	composedHdr.GetDimensions(width,height);
	if(any(pixel>=composite.extent)||any(pixel>=uint2(width,height)))return;
	sceneHdr.GetDimensions(width,height);
	if(any(pixel>=uint2(width,height)))return;
	float4 original=sceneHdr.Load(int3(pixel,0));
	// Store the original immediately: every invalid/missing geometric proposal
	// fails open to the current scene, never yesterday's screen-space contents.
	composedHdr[pixel]=original;
	if(!CompositeExtent(width,height)||!all(isfinite(original)))return;
	const uint compiledMask=0u
#if defined(SCREEN_COMPOSITE_SSR)
		|1u
#endif
#if defined(SCREEN_COMPOSITE_SSGI)
		|2u
#endif
#if defined(SCREEN_COMPOSITE_AO)
		|4u
#endif
		;
	if(composite.enabledMask==0u||(composite.enabledMask&~compiledMask)!=0u)return;
	bool reflection=false,indirect=false,ao=false;
#if defined(SCREEN_COMPOSITE_SSR)
	reflection=(composite.enabledMask&1u)!=0u;
#endif
#if defined(SCREEN_COMPOSITE_SSGI)
	indirect=(composite.enabledMask&2u)!=0u;
#endif
#if defined(SCREEN_COMPOSITE_AO)
	// Authored/material AO may remain in the weights. Enhanced screen AO is
	// eligible only when a protected producer explicitly omitted legacy SCREEN
	// AO from these exact original terms. No second strength is applied here.
	ao=(composite.enabledMask&4u)!=0u&&(composite.inputFlags&1u)!=0u;
#endif
	if(!reflection&&!indirect&&!ao)return;
#if defined(SCREEN_COMPOSITE_AO) || defined(SCREEN_COMPOSITE_SSR) || defined(SCREEN_COMPOSITE_SSGI)
	surfaceNormalRoughness.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
	float4 surface=surfaceNormalRoughness.Load(int3(pixel,0));
	float normalLengthSquared=dot(surface.xyz,surface.xyz);
	if(!all(isfinite(surface))||normalLengthSquared<0.25f||normalLengthSquared>2.25f
		||surface.w<0||surface.w>1)return;
#endif
	float3 environmentOriginal=0,diffuseOriginal=0,delta=0;
#if defined(SCREEN_COMPOSITE_SSR) || defined(SCREEN_COMPOSITE_AO)
	if(reflection||ao)
	{
		environmentFallback.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
		environmentWeight.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
		float3 fallback=environmentFallback.Load(int3(pixel,0)).rgb;
		float3 weight=environmentWeight.Load(int3(pixel,0)).rgb;
		if(!all(isfinite(fallback))||!all(isfinite(weight)))return;
		environmentOriginal=weight*fallback;
#if defined(SCREEN_COMPOSITE_SSR)
		if(reflection)
		{
			reflectionConfidence.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
			float confidence=reflectionConfidence.Load(int3(pixel,0));
			if(isfinite(confidence)&&confidence>0&&confidence<=1)
			{
				reflectionResolved.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
				float3 replacement=reflectionResolved.Load(int3(pixel,0)).rgb;
				if(!all(isfinite(replacement)))return;
				// Tracing already resolves hit confidence against its fallback.
				// Confidence is only a validity gate here, never a second blend.
				if(any(replacement!=fallback))delta+=weight*(replacement-fallback);
			}
		}
#endif
	}
#endif
#if defined(SCREEN_COMPOSITE_SSGI) || defined(SCREEN_COMPOSITE_AO)
	if(indirect||ao)
	{
		diffuseFallback.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
		diffuseWeight.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
		float3 fallback=diffuseFallback.Load(int3(pixel,0)).rgb;
		float3 weight=diffuseWeight.Load(int3(pixel,0)).rgb;
		if(!all(isfinite(fallback))||!all(isfinite(weight)))return;
		diffuseOriginal=weight*fallback;
#if defined(SCREEN_COMPOSITE_SSGI)
		if(indirect)
		{
			indirectConfidence.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
			float confidence=indirectConfidence.Load(int3(pixel,0));
			if(isfinite(confidence)&&confidence>0&&confidence<=1)
			{
				indirectResolved.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
				float3 replacement=indirectResolved.Load(int3(pixel,0)).rgb;
				if(!all(isfinite(replacement)))return;
				if(any(replacement!=fallback))delta+=weight*(replacement-fallback);
			}
		}
#endif
	}
#endif
	float visibility=1;
#if defined(SCREEN_COMPOSITE_AO)
	if(ao)
	{
		enhancedAo.GetDimensions(width,height);if(!CompositeExtent(width,height))return;
		float value=enhancedAo.Load(int3(pixel,0));
		if(!isfinite(value)||value<0||value>1)return;
		visibility=value;
	}
#endif
	float3 originalIndirect=environmentOriginal+diffuseOriginal;
	if(!all(isfinite(delta))||!all(isfinite(originalIndirect)))return;
	// Exact disabled/miss/unit-AO bypass: do not even add floating-point zero to
	// original HDR or clamp it. Negative zero, HDR range and alpha survive intact.
	if(all(delta==0)&&(visibility==1||all(originalIndirect==0)))return;
	float3 color;
	if(visibility!=1)
	{
		// Remove the old IBL once, then apply enhanced AO once to its replacement.
		// Direct/local lighting and emissive stay in the untouched scene remainder.
		color=original.rgb-originalIndirect+(originalIndirect+delta)*visibility;
	}
	else color=original.rgb+delta;
	if(!all(isfinite(color)))return;
#if !defined(SCREEN_COMPOSITE_RGBA32F)
	// Finite float arithmetic can still overflow the actual half-float image.
	// Reject that proposal instead of storing infinity or silently clipping HDR.
	if(any(abs(color)>65504.0f))return;
#endif
	composedHdr[pixel]=float4(color,original.a);
}
