// Private CMAA2 resources shared by its compute stages. The source stays
// immutable; only edge decisions use the shared perceptual luminance metric.
#ifndef TERRAENGINE_CMAA2_COMMON_HLSLI
#define TERRAENGINE_CMAA2_COMMON_HLSLI

[[vk::binding(0,0)]] Texture2D<float4> sourceColor;
[[vk::binding(1,0), vk::image_format("r32ui")]] RWTexture2D<uint> edgeFlags;
[[vk::binding(2,0)]] RWStructuredBuffer<uint> candidates;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> rowCounts;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> rowOffsets;
[[vk::binding(5,0)]] RWStructuredBuffer<uint> counters; // requested, admitted, overflow, reserved
[[vk::binding(6,0)]] RWStructuredBuffer<uint> indirectArgs; // x,y,z dispatch groups
[[vk::binding(7,0)]] RWStructuredBuffer<uint> coefficients; // five words per destination: E,S,W,N,total priority
#if defined(CMAA2_OUTPUT_RGBA32F)
[[vk::binding(8,0), vk::image_format("rgba32f")]] RWTexture2D<float4> destination;
#else
[[vk::binding(8,0), vk::image_format("rgba16f")]] RWTexture2D<float4> destination;
#endif
struct Constants
{
	uint width;
	uint height;
	uint capacity;
	uint maxSearch;
	float threshold;
	float localContrast;
	float armRatio;
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<Constants> config;
static const uint East=1, South=2, West=4, North=8;
bool Valid(int2 p) { return all(p>=0) && p.x<int(config.width) && p.y<int(config.height); }
uint Edges(int2 p)
{
	// Clamp the actual address as well as masking its result. HLSL may evaluate
	// both sides of a conditional expression, so a mask alone cannot bound reads.
	int2 address=clamp(p,int2(0,0),int2(config.width,config.height)-1);
	uint value=edgeFlags[address];
	return Valid(p)?value:0;
}

#endif
