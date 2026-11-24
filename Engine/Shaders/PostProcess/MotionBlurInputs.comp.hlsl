// Real current raster motion -> unjittered blur inputs and depth disocclusion.
// Previous depth belongs to the exact accepted previous view, in its unjittered
// output grid. No zero-mask, guessed camera velocity, or synthetic depth fallback.
[[vk::binding(0,0)]] Texture2D<float> currentDepth;
[[vk::binding(1,0)]] Texture2D<float4> rawTotalMotion;
[[vk::binding(2,0)]] Texture2D<float4> rawCameraMotion;
[[vk::binding(3,0)]] Texture2D<float> previousDepth;
[[vk::binding(4,0),vk::image_format("r32f")]] RWTexture2D<float> linearDepth;
[[vk::binding(5,0),vk::image_format("rgba32f")]] RWTexture2D<float4> totalVelocity;
[[vk::binding(6,0),vk::image_format("rgba32f")]] RWTexture2D<float4> cameraVelocity;
[[vk::binding(7,0),vk::image_format("r32f")]] RWTexture2D<float> disocclusion;
struct MotionBlurInputConstants
{
	column_major float4x4 currentInverseProjection;
	column_major float4x4 previousInverseProjection;
	uint2 extent;
	float2 currentJitterUV;
	float2 previousJitterUV;
	float absoluteMetresTolerance;
	float relativeDepthTolerance;
	uint historyValid;
	uint flags; //Bit0: current HDR image is on an unjittered output grid.
	uint reserved0;
	uint reserved1;
};
[[vk::push_constant]] ConstantBuffer<MotionBlurInputConstants> config;

bool EyeMetres(column_major float4x4 inverseProjection,float2 rasterUV,float depth,out float metres)
{
	metres=0;
	if(!isfinite(depth)||depth<0.0f||depth>=1.0f||!all(isfinite(rasterUV)))return false;
	float4 eye=mul(inverseProjection,float4(rasterUV*2.0f-1.0f,depth,1.0f));
	if(!all(isfinite(eye))||abs(eye.w)<=1e-8f)return false;
	metres=-eye.z/eye.w;return isfinite(metres)&&metres>0.0f;
}
bool SelectCurrent(float2 rasterUV,uint2 size,out int2 selected,out float depth)
{
	selected=int2(0,0);depth=1.0f;
	if(!all(isfinite(rasterUV))||any(rasterUV<0)||any(rasterUV>1))return false;
	float2 grid=rasterUV*float2(size)-0.5f;int2 base=int2(floor(grid));float2 fraction=frac(grid);bool found=false;float bestWeight=-1;
	// Mirror temporal nearest-surface selection on its current reconstruction
	// footprint, so a foreground edge never borrows a background's velocity.
	for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){
		float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);if(weight<=0)continue;
		int2 pixel=clamp(base+int2(x,y),int2(0,0),int2(size)-1);float candidate=currentDepth.Load(int3(pixel,0));
		if(!isfinite(candidate)||candidate<0||candidate>=1)continue;
		if(!found||candidate<depth||(candidate==depth&&weight>bestWeight)){selected=pixel;depth=candidate;bestWeight=weight;found=true;}
	}
	return found;
}
bool PreviousSurface(float2 historyUV,float2 expectedRasterUV,float expectedDepth,uint2 size)
{
	if(!all(isfinite(historyUV))||any(historyUV<0)||any(historyUV>1))return false;
	float expectedMetres;if(!EyeMetres(config.previousInverseProjection,expectedRasterUV,expectedDepth,expectedMetres))return false;
	float2 grid=historyUV*float2(size)-0.5f;int2 base=int2(floor(grid));float2 fraction=frac(grid);bool sampled=false;
	for(uint y=0;y<2;++y)for(uint x=0;x<2;++x){
		float weight=(x==0?1-fraction.x:fraction.x)*(y==0?1-fraction.y:fraction.y);if(weight<=0)continue;
		int2 pixel=clamp(base+int2(x,y),int2(0,0),int2(size)-1);
		float stored=previousDepth.Load(int3(pixel,0));
		float2 previousRasterUV=(float2(pixel)+0.5f)/float2(size)+config.previousJitterUV;
		float storedMetres;if(!EyeMetres(config.previousInverseProjection,previousRasterUV,stored,storedMetres))return false;
		float tolerance=max(config.absoluteMetresTolerance,config.relativeDepthTolerance*max(expectedMetres,storedMetres));
		if(abs(expectedMetres-storedMetres)>tolerance)return false;sampled=true;
	}
	return sampled;
}
[numthreads(8,8,1)]
void CSMain(uint3 tid:SV_DispatchThreadID)
{
	uint2 size,totalOut,cameraOut,maskOut;linearDepth.GetDimensions(size.x,size.y);
	totalVelocity.GetDimensions(totalOut.x,totalOut.y);cameraVelocity.GetDimensions(cameraOut.x,cameraOut.y);disocclusion.GetDimensions(maskOut.x,maskOut.y);
	if(any(tid.xy>=size)||any(tid.xy>=totalOut)||any(tid.xy>=cameraOut)||any(tid.xy>=maskOut))return;
	linearDepth[tid.xy]=0;totalVelocity[tid.xy]=float4(0,0,0,0);cameraVelocity[tid.xy]=float4(0,0,0,0);disocclusion[tid.xy]=1;
	uint2 depthSize,totalSize,cameraSize,oldSize;
	currentDepth.GetDimensions(depthSize.x,depthSize.y);rawTotalMotion.GetDimensions(totalSize.x,totalSize.y);rawCameraMotion.GetDimensions(cameraSize.x,cameraSize.y);
	previousDepth.GetDimensions(oldSize.x,oldSize.y);
	if(any(size==0)||any(size!=config.extent)||any(depthSize!=size)||any(totalSize!=size)||any(cameraSize!=size)||any(oldSize!=size)
		||any(totalOut!=size)||any(cameraOut!=size)||any(maskOut!=size)||(config.flags&~1u)!=0||config.reserved0||config.reserved1
		||!all(isfinite(config.currentJitterUV))||!all(isfinite(config.previousJitterUV))
		||!isfinite(config.absoluteMetresTolerance)||config.absoluteMetresTolerance<0||!isfinite(config.relativeDepthTolerance)||config.relativeDepthTolerance<0)return;
	float2 outputUV=(float2(tid.xy)+0.5f)/float2(size);
	float2 requestedRasterUV=outputUV+((config.flags&1u)!=0?config.currentJitterUV:float2(0,0));
	int2 selected;float depth;
	if((config.flags&1u)==0u||all(config.currentJitterUV==float2(0,0))){
		// The raster grid already matches color exactly. Do not manufacture a
		// neighboring footprint from floating-point UV roundoff at pixel centers.
		selected=int2(tid.xy);depth=currentDepth.Load(int3(selected,0));
		if(!isfinite(depth)||depth<0||depth>=1)return;
	}else if(!SelectCurrent(requestedRasterUV,size,selected,depth))return;
	float2 rasterUV=(float2(selected)+0.5f)/float2(size);float metres;
	if(!EyeMetres(config.currentInverseProjection,rasterUV,depth,metres))return;linearDepth[tid.xy]=metres;
	if(config.historyValid==0)return;
	float4 total=rawTotalMotion.Load(int3(selected,0)),camera=rawCameraMotion.Load(int3(selected,0));
	if(!all(isfinite(total))||!all(isfinite(camera))||total.w<0.5f||camera.w<0.5f||camera.z<0||camera.z>=1)return;
	float2 previousRasterUV=rasterUV+total.xy;
	float2 previousHistoryUV=previousRasterUV-config.previousJitterUV;
	if(!PreviousSurface(previousHistoryUV,previousRasterUV,total.z,size))return;
	// Raw motion already contains previousJitter-currentJitter. Remove that
	// signed difference once; physical camera/object components remain separate.
	float2 correction=config.currentJitterUV-config.previousJitterUV;
	total.xy+=correction;camera.xy+=correction;
	if(!all(isfinite(total))||!all(isfinite(camera)))return;
	totalVelocity[tid.xy]=total;cameraVelocity[tid.xy]=camera;disocclusion[tid.xy]=0;
}
