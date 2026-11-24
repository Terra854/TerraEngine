// Convert actual standard-Z sampled scene depth to positive linear eye metres.
// Projection is the exact accepted current raster projection (including jitter
// and the engine's VulkanY convention), not a reconstructed near/far heuristic.
[[vk::binding(0,0)]] Texture2D<float> sceneDepth;
[[vk::binding(1,0),vk::image_format("r32f")]] RWTexture2D<float> linearEyeDepth;
struct LinearViewDepthConstants
{
	column_major float4x4 inverseProjection;
	uint2 extent;
	uint reserved0;
	uint reserved1;
};
[[vk::push_constant]] ConstantBuffer<LinearViewDepthConstants> config;
[numthreads(8,8,1)]
void CSMain(uint3 tid:SV_DispatchThreadID)
{
	uint2 outputSize;linearEyeDepth.GetDimensions(outputSize.x,outputSize.y);
	if(any(tid.xy>=outputSize))return;
	linearEyeDepth[tid.xy]=0.0f; // Explicit invalid/background sentinel, never fake far depth.
	uint2 depthSize;sceneDepth.GetDimensions(depthSize.x,depthSize.y);
	if(any(depthSize!=outputSize)||any(outputSize!=config.extent)||any(depthSize==0u)
		||config.reserved0!=0u||config.reserved1!=0u)return;
	float depth=sceneDepth.Load(int3(tid.xy,0));
	// Native qualification supports standardZ with a clear value of1. ReversedZ
	// and MSAA require their own declared producer/resolve and cannot reuse this.
	if(!isfinite(depth)||depth<0.0f||depth>=1.0f)return;
	float2 ndc=((float2(tid.xy)+0.5f)/float2(depthSize))*2.0f-1.0f;
	// Positive-height Vulkan raster coordinates already match the Y-flipped
	// accepted projection. Do not insert another image-space Y inversion.
	float4 eye=mul(config.inverseProjection,float4(ndc,depth,1.0f));
	if(!all(isfinite(eye))||abs(eye.w)<=1e-8f)return;
	float metres=-eye.z/eye.w; // Engine cameras look along negative viewZ.
	if(isfinite(metres)&&metres>0.0f)linearEyeDepth[tid.xy]=metres;
}
