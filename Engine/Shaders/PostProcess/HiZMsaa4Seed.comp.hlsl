// Populate level zero from the furthest of four coverage samples in standard
// Z. A background sample must keep the pixel open to later occlusion queries.
[[vk::binding(0, 0)]] Texture2DMS<float, 4> rasterDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> outputMip;

// Match HiZGenerate's descriptor and constants layout. Later pyramid levels
// continue to use the original single-sample shader.
struct PushConstants
{
	uint2 inputSize;
	uint2 outputSize;
	uint occlusionState;
	uint invalidReason;
	uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> config;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	uint2 pixel = dispatchId.xy;
	if (any(pixel >= config.outputSize)) return;
	if (any(config.inputSize != config.outputSize))
	{
		outputMip[pixel] = 1.0f;
		return;
	}
	int2 coordinate = int2(pixel);
	float4 coveredDepth = float4(rasterDepth.Load(coordinate, 0), rasterDepth.Load(coordinate, 1),
		rasterDepth.Load(coordinate, 2), rasterDepth.Load(coordinate, 3));
	outputMip[pixel] = all(isfinite(coveredDepth)) ? max(max(coveredDepth.x, coveredDepth.y),
		max(coveredDepth.z, coveredDepth.w)) : 1.0f;
}
