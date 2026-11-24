// Independent output-space sharpening. Temporal acceptance/history does not
// depend on this pass, and strength0 is an exact bounded source-load bypass.
[[vk::binding(0,0)]] Texture2D<float4> sharpenSource;
#if defined(TEMPORAL_OUTPUT_RGBA32F)
[[vk::binding(1,0),vk::image_format("rgba32f")]] RWTexture2D<float4> sharpenOutput;
#else
[[vk::binding(1,0),vk::image_format("rgba16f")]] RWTexture2D<float4> sharpenOutput;
#endif
struct TemporalSharpenConstants
{
	uint2 extent;
	float strength;
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<TemporalSharpenConstants> sharpen;
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
	uint2 sourceSize,outputSize;
	sharpenSource.GetDimensions(sourceSize.x,sourceSize.y);sharpenOutput.GetDimensions(outputSize.x,outputSize.y);
	if(any(tid.xy>=outputSize)||any(sourceSize==0))return;
	int2 pixel=clamp(int2(tid.xy),int2(0,0),int2(sourceSize)-1);
	float4 center=sharpenSource.Load(int3(pixel,0));
	// Native preparation requires equal trusted extents and matching formats.
	// A malformed dispatch still performs bounded reads and cannot sharpen old
	// extent pixels, change alpha or reuse history as an apparent fallback.
	if(any(sourceSize!=outputSize)||any(outputSize!=sharpen.extent)||!isfinite(sharpen.strength)
		||sharpen.strength<=0||sharpen.reserved!=0||!all(isfinite(center))){sharpenOutput[tid.xy]=center;return;}
	float3 minimum=center.rgb,maximum=center.rgb,sum=0;float count=0;
	int2 offsets[4]={int2(1,0),int2(0,1),int2(-1,0),int2(0,-1)};
	for(uint i=0;i<4;++i)
	{
		int2 neighbor=clamp(pixel+offsets[i],int2(0,0),int2(sourceSize)-1);
		float4 value=sharpenSource.Load(int3(neighbor,0));
		if(all(isfinite(value))){minimum=min(minimum,value.rgb);maximum=max(maximum,value.rgb);sum+=value.rgb;count+=1;}
	}
	float3 color=center.rgb;
	if(count>0)color=clamp(center.rgb+(center.rgb-sum/count)*clamp(sharpen.strength,0.0f,1.0f),minimum,maximum);
	sharpenOutput[tid.xy]=float4(color,center.a);
}
