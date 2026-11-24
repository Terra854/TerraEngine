#if defined(POSTPROCESS_SCREEN_SPACE_VIEW)
// Current-view owner AO. The legacy entry remains below the compile-time branch
// unchanged. These view inputs belong to one matching depth/normal publication.
#pragma pack_matrix(column_major)
[[vk::binding(0,0)]] Texture2D<float> viewDepth;
[[vk::binding(1,0)]] Texture2D<float4> viewNormal;
[[vk::binding(2,0),vk::image_format("r32f")]] RWTexture2D<float> viewOcclusion;
struct AoView{column_major float4x4 inverseProjection;};
[[vk::binding(3,0)]] ConstantBuffer<AoView> aoView;
struct AoConstants
{
	uint2 extent;
	float radius; // World/view units, independent of projection and distance.
	float strength;
	uint directions; // Bounded4..16 azimuth slices.
	uint steps; // Bounded2..16 radial samples per slice.
	float bias; // View-space self-occlusion rejection distance.
	float reserved;
};
[[vk::push_constant]] ConstantBuffer<AoConstants> ao;
bool AoPosition(int2 pixel,out float3 position)
{
	position=0;
	if(any(pixel<0)||any(pixel>=int2(ao.extent)))return false;
	float d=viewDepth.Load(int3(pixel,0));
	// Current renderer standardZ; reversedZ is deliberately not silently guessed.
	if(!isfinite(d)||d<0||d>=1)return false;
	float2 uv=(float2(pixel)+0.5f)/ao.extent;
	float4 p=mul(aoView.inverseProjection,float4(uv*2-1,d,1));
	if(!all(isfinite(p))||abs(p.w)<1e-8f)return false;
	position=p.xyz/p.w;return all(isfinite(position));
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
	uint2 pixel=id.xy;uint w,h;viewOcclusion.GetDimensions(w,h);
	if(any(pixel>=uint2(w,h))||any(pixel>=ao.extent))return;
	viewOcclusion[pixel]=1;
	viewDepth.GetDimensions(w,h);if(w!=ao.extent.x||h!=ao.extent.y)return;
	viewNormal.GetDimensions(w,h);if(w!=ao.extent.x||h!=ao.extent.y)return;
	if(!isfinite(ao.radius)||ao.radius<=0||!isfinite(ao.strength)||ao.strength<=0||!isfinite(ao.bias))return;
	float3 center;if(!AoPosition(int2(pixel),center))return;
	float3 normal=viewNormal.Load(int3(pixel,0)).xyz;
	float normalLength=dot(normal,normal);
	if(!all(isfinite(normal))||normalLength<0.25f||normalLength>2.25f)return;
	normal*=rsqrt(normalLength);
	// Reconstruct adjacent positions on the center depth plane to measure the
	// actual projection's pixel footprint, including orthographic cameras.
	float d=viewDepth.Load(int3(pixel,0));
	float2 uv=(float2(pixel)+0.5f)/ao.extent;
	float4 px=mul(aoView.inverseProjection,float4((uv+float2(1.0f/ao.extent.x,0))*2-1,d,1));
	float4 py=mul(aoView.inverseProjection,float4((uv+float2(0,1.0f/ao.extent.y))*2-1,d,1));
	if(!all(isfinite(px))||!all(isfinite(py))||abs(px.w)<1e-8f||abs(py.w)<1e-8f)return;
	float footprint=max(length(px.xyz/px.w-center),length(py.xyz/py.w-center));
	if(!isfinite(footprint)||footprint<1e-8f)return;
	float radiusPixels=min(ao.radius/footprint,float(max(ao.extent.x,ao.extent.y)));
	uint directions=clamp(ao.directions,4u,16u),steps=clamp(ao.steps,2u,16u);
	float occlusion=0;
	for(uint i=0;i<directions;++i)
	{
		float angle=(float(i)+0.5f)*6.283185307f/float(directions);
		float2 axis=float2(cos(angle),sin(angle));
		float horizon=0;
		for(uint j=1;j<=steps;++j)
		{
			float distancePixels=max(1.0f,radiusPixels*float(j*j)/float(steps*steps));
			float3 samplePosition;
			if(!AoPosition(int2(pixel)+int2(round(axis*distancePixels)),samplePosition))continue;
			float3 offset=samplePosition-center;float distance=length(offset);
			if(!isfinite(distance)||distance<=max(ao.bias,1e-5f)||distance>=ao.radius)continue;
			// Track the highest normal-relative blocker in each azimuth slice.
			// Smooth world-radius attenuation prevents a hard darkening halo.
			float elevation=saturate((dot(normal,offset)-max(ao.bias,0.0f))/distance);
			float attenuation=saturate(1-distance*distance/(ao.radius*ao.radius));
			horizon=max(horizon,elevation*attenuation);
		}
		occlusion+=horizon;
	}
	viewOcclusion[pixel]=saturate(1-occlusion/float(directions)*max(ao.strength,0.0f));
}

#else
#pragma pack_matrix(column_major)

// First-pass contact darkening shader. Runtime GTAO samples an owned
// full-resolution depth-history texture and writes one single-channel
// occlusion target for the blur and material passes.
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
// Dedicated AO storage binding. Binding 16 is reserved for Hi-Z output, and
// reusing it for AO made optimized command buffers depend on descriptor update
// ordering instead of the recorded command order.
[[vk::binding(23, 0)]] RWTexture2D<float> outputOcclusion;

struct PushConstants
{
	uint2 viewportSize;
	float radiusPixels;
	float strength;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

float LoadDepthClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputDepth.Load(uint3(uint2(clampedPixel), 0));
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

	const int sampleRadius = max(1, (int)round(config.radiusPixels));
	float occlusion = 0.0f;
	float weight = 0.0f;
	const float sameSurfaceBias = max(0.00035f, centerDepth * 0.00045f);

	// This depth-only approximation is intentionally conservative. Nearby
	// samples that are closer to the camera than the center pixel contribute
	// contact darkening, while far samples are ignored to avoid halos.
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			if (x == 0 && y == 0)
			{
				continue;
			}

			const int2 offset = int2(x, y) * sampleRadius;
			const float sampleDepth = LoadDepthClamped(int2(pixel) + offset);
			if (!IsUsableDepth(sampleDepth))
			{
				continue;
			}

			// Ignore tiny depth deltas from the same triangle or neighboring
			// coplanar triangles. Optimized/export builds run fast enough that
			// these sub-pixel depth differences were visible as dotted AO noise.
			const float depthDelta = centerDepth - sampleDepth - sameSurfaceBias;
			occlusion += saturate(depthDelta * 72.0f);
			weight += 1.0f;
		}
	}

	const float normalizedOcclusion = weight > 0.0f ? occlusion / weight : 0.0f;
	outputOcclusion[pixel] = saturate(1.0f - normalizedOcclusion * max(config.strength, 0.0f));
}

#endif
