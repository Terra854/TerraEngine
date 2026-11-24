// Temporal color reconstruction prerequisite. No native temporal capability is
// implied until current/previous scene producers and history ownership qualify.
// UV origin is top-left, positive Y down. Motion.xy is previous RASTER UV minus
// current RASTER UV and already includes both actual projection jitters.
// Output/history grids are unjittered:
//   currentRasterUV = outputUV + currentJitterUV
//   previousHistoryUV = currentRasterUV + motion.xy - previousJitterUV
// Do not add or subtract another jitter delta in the native producer/consumer.
[[vk::binding(0,0)]] Texture2D<float4> currentColor;
[[vk::binding(1,0)]] Texture2D<float> currentDepth;
[[vk::binding(2,0)]] Texture2D<float4> currentMotion;
[[vk::binding(3,0)]] Texture2D<float4> historyColor;
[[vk::binding(4,0)]] Texture2D<float> historyDepth;
#if defined(TEMPORAL_OUTPUT_RGBA32F)
[[vk::binding(5,0),vk::image_format("rgba32f")]] RWTexture2D<float4> resolvedColor;
#else
[[vk::binding(5,0),vk::image_format("rgba16f")]] RWTexture2D<float4> resolvedColor;
#endif
[[vk::binding(6,0),vk::image_format("r32f")]] RWTexture2D<float> nextHistoryDepth;
struct TemporalResolveConstants
{
	uint2 internalExtent;
	uint2 outputExtent;
	uint historyValid;
	uint flags; // Bit0: reversed normalized depth (larger is nearer).
	float historyWeight; // Blend weight, bounded to0..0.98 during resolve.
	float absoluteDepthTolerance; // Absolute normalized-depth rejection threshold.
	float relativeDepthTolerance; // Additional fraction of expected/stored depth.
	float neighborhoodDepthTolerance; // Current normalized-depth clamp neighborhood.
	uint reserved0;
	uint reserved1;
	float2 currentJitterUV; // Actual current raster projection shift in normalizedUV.
	float2 previousJitterUV; // Actual predecessor shift, same top-left/down convention.
};
[[vk::push_constant]] ConstantBuffer<TemporalResolveConstants> temporal;

int2 BoundPixel(int2 pixel,uint2 extent) { return clamp(pixel,int2(0,0),int2(extent)-1); }
bool FiniteDepth(float depth) { return isfinite(depth)&&depth>=0.0f&&depth<=1.0f; }
float4 ReconstructCurrent(float2 rasterUV,uint2 extent,out bool valid)
{
	// The same reconstruction is used on reset, rejected history and the valid
	// temporal path, at both native1x and lower internal resolution. Clamp actual
	// addresses, not just results of eager HLSL conditional expressions.
	float2 grid=saturate(rasterUV)*float2(extent)-0.5f;
	int2 base=int2(floor(grid));float2 fraction=frac(grid);
	float4 sum=0;float total=0;
	for(uint y=0;y<2;++y)for(uint x=0;x<2;++x)
	{
		float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
		float4 value=currentColor.Load(int3(BoundPixel(base+int2(x,y),extent),0));
		if(weight>0&&all(isfinite(value))){sum+=value*weight;total+=weight;}
	}
	valid=total>0;
	if(valid)return sum/total;
	// All-invalid source data has no valid temporal solution. Keep the bounded
	// current sample rather than borrowing unrelated old pixels or inventing HDR.
	return currentColor.Load(int3(BoundPixel(int2(floor(saturate(rasterUV)*float2(extent))),extent),0));
}
bool SelectCurrentGeometry(float2 rasterUV,uint2 extent,out int2 selected,out float depth)
{
	float2 grid=saturate(rasterUV)*float2(extent)-0.5f;
	int2 base=int2(floor(grid));float2 fraction=frac(grid);
	selected=BoundPixel(int2(floor(saturate(rasterUV)*float2(extent))),extent);
	depth=-1;bool found=false;float bestWeight=-1;
	// At native resolution prefer the nearest surface so foreground edges
	// retain their motion. A magnified footprint spans several output pixels.
	// There, its highest-weight sample supplies depth and motion. A foreground
	// corner can contribute color without moving the whole footprint with it.
	const bool magnified=any(extent<temporal.outputExtent);
	for(uint y=0;y<2;++y)for(uint x=0;x<2;++x)
	{
		float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
		if(weight<=0)continue;
		int2 pixel=BoundPixel(base+int2(x,y),extent);
		float candidate=currentDepth.Load(int3(pixel,0));
		float4 color=currentColor.Load(int3(pixel,0));
		if(!FiniteDepth(candidate)||!all(isfinite(color)))continue;
		bool nearer=(temporal.flags&1u)!=0?candidate>depth:candidate<depth;
		const bool dominant=weight>bestWeight||(weight==bestWeight&&nearer);
		if(!found||(magnified?dominant:nearer||(candidate==depth&&weight>bestWeight)))
		{selected=pixel;depth=candidate;bestWeight=weight;found=true;}
	}
	return found;
}
bool ReconstructHistory(float2 uv,float expectedDepth,uint2 extent,out float3 color)
{
	color=0;
	if(!all(isfinite(uv))||any(uv<0)||any(uv>1)||!FiniteDepth(expectedDepth))return false;
	float2 grid=uv*float2(extent)-0.5f;int2 base=int2(floor(grid));float2 fraction=frac(grid);
	float3 sum=0;float total=0;
	for(uint y=0;y<2;++y)for(uint x=0;x<2;++x)
	{
		float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);
		if(weight<=0)continue;
		int2 pixel=BoundPixel(base+int2(x,y),extent);
		float sampleDepth=historyDepth.Load(int3(pixel,0));
		float4 sampleColor=historyColor.Load(int3(pixel,0));
		float tolerance=temporal.absoluteDepthTolerance+temporal.relativeDepthTolerance*max(abs(expectedDepth),abs(sampleDepth));
		// Disoccluded taps never contribute. A subpixel edge can still use
		// the part of its footprint covered by the same reprojected surface.
		if(!FiniteDepth(sampleDepth)||!all(isfinite(sampleColor))||abs(sampleDepth-expectedDepth)>tolerance)continue;
		sum+=sampleColor.rgb*weight;total+=weight;
	}
	// An incidental matching neighbor does not validate a new surface.
	if(total<0.25f)return false;
	color=sum/total;return all(isfinite(color));
}
[numthreads(8,8,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
	uint2 outputSize;resolvedColor.GetDimensions(outputSize.x,outputSize.y);
	if(any(tid.xy>=outputSize))return;
	uint2 sourceSize;currentColor.GetDimensions(sourceSize.x,sourceSize.y);
	if(any(sourceSize==0))return; // A native prepare must reject missing resources.
	uint2 depthSize,motionSize,historySize,historyDepthSize,nextDepthSize;
	currentDepth.GetDimensions(depthSize.x,depthSize.y);currentMotion.GetDimensions(motionSize.x,motionSize.y);
	historyColor.GetDimensions(historySize.x,historySize.y);historyDepth.GetDimensions(historyDepthSize.x,historyDepthSize.y);
	nextHistoryDepth.GetDimensions(nextDepthSize.x,nextDepthSize.y);
	float2 outputUV=(float2(tid.xy)+0.5f)/float2(outputSize);
	float2 currentJitter=all(isfinite(temporal.currentJitterUV))?temporal.currentJitterUV:float2(0,0);
	float2 currentRasterUV=outputUV+currentJitter;
	bool currentValid=false;float4 current=ReconstructCurrent(currentRasterUV,sourceSize,currentValid);
	bool currentDimensions=all(sourceSize==temporal.internalExtent)&&all(depthSize==sourceSize)&&all(motionSize==sourceSize)
		&&all(outputSize==temporal.outputExtent)&&all(nextDepthSize==outputSize);
	int2 selected=int2(0,0);float selectedDepth=-1;
	bool geometryValid=false;
	if(currentDimensions&&currentValid)geometryValid=SelectCurrentGeometry(currentRasterUV,sourceSize,selected,selectedDepth);
	if(geometryValid)
	{
		// A depth discontinuity will often receive a different subset of
		// subpixel samples on successive jitters. Integrate more current
		// coverage there while retaining the ordinary interior filter.
		const int2 offsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
		bool discontinuity=false;
		for(uint axis=0;axis<4u;++axis)
		{
			const float depth=currentDepth.Load(int3(BoundPixel(selected+offsets[axis],sourceSize),0));
			discontinuity=discontinuity||(FiniteDepth(depth)&&abs(depth-selectedDepth)>temporal.neighborhoodDepthTolerance);
		}
		if(discontinuity)
		{
			// Bound extra filtering in output-pixel units at 2×. A source-pixel
			// radius would spread edge colors twice as far after reconstruction.
			const bool upscale2x=all(outputSize==sourceSize*2u);
			const float2 subpixelSpan=upscale2x?0.75f/float2(outputSize):0.75f/float2(sourceSize);
			float4 accumulated=0;
			uint validTaps=0;
			for(uint axis=0;axis<4u;++axis)
			{
				bool valid;
				const float4 tap=ReconstructCurrent(currentRasterUV+float2(offsets[axis])*subpixelSpan,sourceSize,valid);
				if(valid){accumulated+=tap;validTaps+=1;}
			}
			if(validTaps>0)current=lerp(current,accumulated/validTaps,0.55f);
		}
	}
	// -1 is an explicit invalid history-depth sentinel, outside normalized depth.
	if(all(tid.xy<nextDepthSize))nextHistoryDepth[tid.xy]=geometryValid?selectedDepth:-1.0f;
	resolvedColor[tid.xy]=current;
	bool controlsValid=isfinite(temporal.historyWeight)&&isfinite(temporal.absoluteDepthTolerance)
		&&isfinite(temporal.relativeDepthTolerance)&&isfinite(temporal.neighborhoodDepthTolerance)
		&&temporal.absoluteDepthTolerance>=0&&temporal.relativeDepthTolerance>=0&&temporal.neighborhoodDepthTolerance>=0
		&&all(isfinite(temporal.currentJitterUV))&&all(isfinite(temporal.previousJitterUV))&&(temporal.flags&~1u)==0&&temporal.reserved0==0&&temporal.reserved1==0;
	// Every invalidation returns the exact already-computed current reconstruction,
	// including its alpha. Old extent/history pixels are never read as substitutes.
	if(!currentDimensions||!currentValid||!geometryValid||!controlsValid||temporal.historyValid==0
		||temporal.historyWeight<=0||any(historySize!=outputSize)||any(historyDepthSize!=outputSize))return;
	float4 motion=currentMotion.Load(int3(selected,0));
	if(!all(isfinite(motion))||motion.w<0.5f||!FiniteDepth(motion.z))return;
	float2 previousUV=currentRasterUV+motion.xy-temporal.previousJitterUV;
	float3 previous;
	if(!ReconstructHistory(previousUV,motion.z,outputSize,previous))return;
	float3 minimum=current.rgb,maximum=current.rgb;
	// Clamp against the current local HDR range. History has already passed
	// its separate depth gate. The range includes legitimate edge coverage.
	for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x)
	{
		int2 pixel=BoundPixel(selected+int2(x,y),sourceSize);
		float depth=currentDepth.Load(int3(pixel,0));float4 color=currentColor.Load(int3(pixel,0));
		if(FiniteDepth(depth)&&all(isfinite(color)))
		{minimum=min(minimum,color.rgb);maximum=max(maximum,color.rgb);}
	}
	previous=clamp(previous,minimum,maximum);
	float weight=clamp(temporal.historyWeight,0.0f,0.98f);
	resolvedColor[tid.xy]=float4(lerp(current.rgb,previous,weight),current.a);
}
