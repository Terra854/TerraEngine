#if defined(POSTPROCESS_SCREEN_SPACE_VIEW)
// Stateless normal/depth bilateral filter; no same-pixel history approximation.
#pragma pack_matrix(column_major)
[[vk::binding(0,0)]] Texture2D<float> viewDepth;
[[vk::binding(1,0)]] Texture2D<float4> viewNormal;
[[vk::binding(2,0)]] Texture2D<float> rawOcclusion;
[[vk::binding(3,0),vk::image_format("r32f")]] RWTexture2D<float> filteredOcclusion;
struct AoBlurView{column_major float4x4 inverseProjection;};
[[vk::binding(4,0)]] ConstantBuffer<AoBlurView> aoView;
struct AoBlurConstants{uint2 extent;float depthSigma;float normalPower;};
[[vk::push_constant]] ConstantBuffer<AoBlurConstants> ao;
bool AoBlurSurface(int2 pixel,out float3 p,out float3 n)
{
	p=0;n=0;if(any(pixel<0)||any(pixel>=int2(ao.extent)))return false;
	float d=viewDepth.Load(int3(pixel,0));float3 v=viewNormal.Load(int3(pixel,0)).xyz;
	float magnitude=dot(v,v);
	if(!isfinite(d)||d<0||d>=1||!all(isfinite(v))||magnitude<0.25f||magnitude>2.25f)return false;
	float2 uv=(float2(pixel)+0.5f)/ao.extent;
	float4 h=mul(aoView.inverseProjection,float4(uv*2-1,d,1));
	if(!all(isfinite(h))||abs(h.w)<1e-8f)return false;
	p=h.xyz/h.w;n=v*rsqrt(magnitude);return all(isfinite(p));
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
	uint2 pixel=id.xy;uint w,h;filteredOcclusion.GetDimensions(w,h);
	if(any(pixel>=uint2(w,h))||any(pixel>=ao.extent))return;
	filteredOcclusion[pixel]=1;
	viewDepth.GetDimensions(w,h);if(w!=ao.extent.x||h!=ao.extent.y)return;
	viewNormal.GetDimensions(w,h);if(w!=ao.extent.x||h!=ao.extent.y)return;
	rawOcclusion.GetDimensions(w,h);if(w!=ao.extent.x||h!=ao.extent.y)return;
	float3 center,normal;if(!AoBlurSurface(int2(pixel),center,normal))return;
	float fallback=rawOcclusion.Load(int3(pixel,0));fallback=isfinite(fallback)?saturate(fallback):1;
	if(!isfinite(ao.depthSigma)||ao.depthSigma<=0||!isfinite(ao.normalPower))
	{
		filteredOcclusion[pixel]=fallback;return;
	}
	float sum=0,total=0;
	for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x)
	{
		int2 q=int2(pixel)+int2(x,y);float3 p,n;if(!AoBlurSurface(q,p,n))continue;
		float value=rawOcclusion.Load(int3(q,0));if(!isfinite(value))continue;
		// View-space depth and geometric normal agreement stop background AO
		// from crossing silhouettes or sharp normal discontinuities.
		float weight=exp(-abs(p.z-center.z)/ao.depthSigma)
			*pow(saturate(dot(normal,n)),max(ao.normalPower,1.0f))*exp(-float(x*x+y*y)*0.25f);
		sum+=saturate(value)*weight;total+=weight;
	}
	filteredOcclusion[pixel]=total>0?sum/total:fallback;
}

#else
#pragma pack_matrix(column_major)

// Bilateral blur for the contact-darkening target. It uses depth agreement to
// keep occlusion from bleeding across object edges.
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
// Raw AO uses its own sampled binding so material shading can keep binding 19
// pointed at the final blurred AO image for the whole draw pass.
[[vk::binding(22, 0)]] Texture2D<float> inputOcclusion;
// Blur output uses a dedicated storage binding instead of the Hi-Z output
// binding. This keeps all compute passes stable under optimized recording.
[[vk::binding(24, 0)]] RWTexture2D<float> outputOcclusion;
[[vk::binding(21, 0)]] Texture2D<float> inputHistory;

struct PushConstants
{
	uint2 viewportSize;
	float depthSigma;
	float historyBlend;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

float LoadDepthClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputDepth.Load(uint3(uint2(clampedPixel), 0));
}

float LoadOcclusionClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputOcclusion.Load(uint3(uint2(clampedPixel), 0));
}

float LoadHistoryClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputHistory.Load(uint3(uint2(clampedPixel), 0));
}

bool IsUsableDepth(float depth)
{
	return isfinite(depth) && depth > 0.0f && depth < 0.999999f;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= config.viewportSize))
	{
		return;
	}

	const float centerDepth = LoadDepthClamped(int2(pixel));
	if (!IsUsableDepth(centerDepth))
	{
		outputOcclusion[pixel] = 1.0f;
		return;
	}

	float weightedOcclusion = 0.0f;
	float totalWeight = 0.0f;

	[unroll]
	for (int y = -2; y <= 2; ++y)
	{
		[unroll]
		for (int x = -2; x <= 2; ++x)
		{
			const int2 samplePixel = int2(pixel) + int2(x, y);
			const float sampleDepth = LoadDepthClamped(samplePixel);
			if (!IsUsableDepth(sampleDepth))
			{
				continue;
			}
			const float depthWeight = exp(-abs(sampleDepth - centerDepth) / max(config.depthSigma, 0.0001f));
			const float spatialWeight = exp(-dot(float2(x, y), float2(x, y)) * 0.25f);
			const float weight = depthWeight * spatialWeight;
			const float sampleOcclusion = LoadOcclusionClamped(samplePixel);
			weightedOcclusion += (isfinite(sampleOcclusion) ? saturate(sampleOcclusion) : 1.0f) * weight;
			totalWeight += weight;
		}
	}

	const float fallbackOcclusion = LoadOcclusionClamped(int2(pixel));
	const float blurred = totalWeight > 0.0f
		? weightedOcclusion / totalWeight
		: (isfinite(fallbackOcclusion) ? saturate(fallbackOcclusion) : 1.0f);
	// Blend only after the spatial blur so history smooths residual AO noise.
	// Avoid sampling history while the CPU says it is invalid; optimized builds
	// may still propagate undefined texture contents through a zero-weight lerp.
	const float historyBlend = saturate(config.historyBlend);
	float resolvedOcclusion = blurred;
	if (historyBlend > 0.001f)
	{
		const float history = LoadHistoryClamped(int2(pixel));
		resolvedOcclusion = lerp(blurred, isfinite(history) ? saturate(history) : 1.0f, historyBlend);
	}
	outputOcclusion[pixel] = saturate(resolvedOcclusion);
}

#endif
