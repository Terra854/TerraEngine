// Current-view lighting inputs only: no history, jitter subtraction or hidden
// temporal reuse. Matrices must be the exact raster projection for these images.
#ifndef TERRAENGINE_SCREEN_SPACE_TRACE
#define TERRAENGINE_SCREEN_SPACE_TRACE
#pragma pack_matrix(column_major)
[[vk::binding(0,0)]] Texture2D<float> screenDepth;
// Signed unit view-space normal, perceptual roughness in W. Zero normal invalid.
[[vk::binding(1,0)]] Texture2D<float4> screenNormalRoughness;
[[vk::binding(2,0)]] Texture2D<float4> screenRadiance;
[[vk::binding(3,0)]] Texture2D<float> screenHierarchy;
[[vk::binding(4,0)]] Texture2D<float4> screenFallback;
#if defined(SCREEN_SPACE_OUTPUT_RGBA32F)
[[vk::binding(5,0),vk::image_format("rgba32f")]] RWTexture2D<float4> screenResolved;
#else
[[vk::binding(5,0),vk::image_format("rgba16f")]] RWTexture2D<float4> screenResolved;
#endif
[[vk::binding(6,0),vk::image_format("r32f")]] RWTexture2D<float> screenConfidence;
struct ScreenSpaceViewData
{
	column_major float4x4 projection;
	column_major float4x4 inverseProjection;
};
[[vk::binding(7,0)]] ConstantBuffer<ScreenSpaceViewData> screenView;
struct ScreenSpaceTraceConstants
{
	uint2 extent;
	uint maxSteps; // Hard bounded to256; exhaustion is a declared miss.
	uint maxMip;
	float maxDistance; // View-space world units, not normalized depth.
	float thickness; // View-space hit tolerance and origin bias.
	float strength; // Resolve weight0..1, never adds a second environment term.
	uint flags; // Bit0 reversedZ; bit1 valid HiZ; bit2 HiZ is NEAREST, else FARTHEST.
};
[[vk::push_constant]] ConstantBuffer<ScreenSpaceTraceConstants> trace;
bool ScreenDepthValid(float d)
{
	return isfinite(d)&&d>=0&&d<=1&&((trace.flags&1u)!=0u?d>0:d<1);
}
float OrderedDepth(float d){return (trace.flags&1u)!=0u?1-d:d;}
bool ScreenPosition(float2 uv,float depth,out float3 p)
{
	float4 h=mul(screenView.inverseProjection,float4(uv*2-1,depth,1));
	p=0;if(!all(isfinite(h))||abs(h.w)<1e-8f)return false;
	p=h.xyz/h.w;return all(isfinite(p));
}
bool ScreenNormal(uint2 pixel,out float3 normal,out float roughness)
{
	float4 v=screenNormalRoughness.Load(int3(pixel,0));normal=0;roughness=1;
	float lengthSquared=dot(v.xyz,v.xyz);
	if(!all(isfinite(v))||lengthSquared<0.25f||lengthSquared>2.25f)return false;
	normal=v.xyz*rsqrt(lengthSquared);roughness=saturate(v.w);return true;
}
bool ScreenInputsMatch()
{
	uint w,h,levels;screenDepth.GetDimensions(w,h);
	if(any(trace.extent==0)||w!=trace.extent.x||h!=trace.extent.y)return false;
	screenNormalRoughness.GetDimensions(w,h);if(w!=trace.extent.x||h!=trace.extent.y)return false;
	screenRadiance.GetDimensions(w,h);if(w!=trace.extent.x||h!=trace.extent.y)return false;
	screenFallback.GetDimensions(w,h);if(w!=trace.extent.x||h!=trace.extent.y)return false;
	if((trace.flags&2u)!=0u){screenHierarchy.GetDimensions(0,w,h,levels);if(w!=trace.extent.x||h!=trace.extent.y||levels==0)return false;}
	return isfinite(trace.maxDistance)&&trace.maxDistance>0&&isfinite(trace.thickness)&&trace.thickness>0;
}
bool ScreenOutputPixel(uint2 pixel)
{
	uint w,h;screenResolved.GetDimensions(w,h);if(any(pixel>=uint2(w,h)))return false;
	screenConfidence.GetDimensions(w,h);return all(pixel<uint2(w,h))&&all(pixel<trace.extent);
}
float4 ScreenFallbackAt(uint2 pixel)
{
	uint w,h;screenFallback.GetDimensions(w,h);
	if(w==0||h==0)return 0;
	return screenFallback.Load(int3(min(pixel,uint2(w,h)-1),0));
}
// Clip the actual homogeneous segment to Vulkan's0..W depth clip volume.
// This supports perspective and orthographic projections without assuming -Z.
bool ClipScreenPlane(float a,float b,inout float lower,inout float upper)
{
	if(a<0&&b<0)return false;
	if(a<0)lower=max(lower,a/(a-b));
	if(b<0)upper=min(upper,a/(a-b));
	return lower<=upper;
}
bool ScreenSegment(float3 start,float3 finish,out float3 first,out float3 last)
{
	first=0;last=0;
	float4 a=mul(screenView.projection,float4(start,1)),b=mul(screenView.projection,float4(finish,1));
	if(!all(isfinite(a))||!all(isfinite(b)))return false;
	float lo=0,hi=1;
	if(!ClipScreenPlane(a.w-1e-6f,b.w-1e-6f,lo,hi)
		||!ClipScreenPlane(a.x+a.w,b.x+b.w,lo,hi)||!ClipScreenPlane(a.w-a.x,b.w-b.x,lo,hi)
		||!ClipScreenPlane(a.y+a.w,b.y+b.w,lo,hi)||!ClipScreenPlane(a.w-a.y,b.w-b.y,lo,hi)
		||!ClipScreenPlane(a.z,b.z,lo,hi)||!ClipScreenPlane(a.w-a.z,b.w-b.z,lo,hi))return false;
	float4 ca=lerp(a,b,lo),cb=lerp(a,b,hi);
	first=float3(ca.xy/ca.w*0.5f+0.5f,ca.z/ca.w);
	last=float3(cb.xy/cb.w*0.5f+0.5f,cb.z/cb.w);
	return all(isfinite(first))&&all(isfinite(last));
}
struct ScreenTraceHit{float3 radiance;float confidence;};
bool ScreenBoundSeparated(float3 a,float3 b,float bound)
{
	// Constant normalized depth is a plane in view space. A whole straight
	// segment must be farther than the WORLD-space hit tolerance from that
	// plane before a hierarchy rejection is safe. A fixed normalized epsilon
	// alone would incorrectly discard near hits at distant perspective depths.
	float3 pa,pb;
	if(!ScreenPosition(a.xy,a.z,pa)||!ScreenPosition(b.xy,b.z,pb))return false;
	float4 plane=screenView.projection[2]-bound*screenView.projection[3];
	float magnitude=length(plane.xyz);
	if(!all(isfinite(plane))||magnitude<1e-8f)return false;
	float da=dot(plane,float4(pa,1)),db=dot(plane,float4(pb,1));
	return da*db>0&&min(abs(da),abs(db))/magnitude>trace.thickness;
}
ScreenTraceHit TraceScreenRay(float3 origin,float3 direction)
{
	ScreenTraceHit hit=(ScreenTraceHit)0;
	float3 a,b;
	if(!ScreenSegment(origin,origin+direction*trace.maxDistance,a,b))return hit;
	float2 delta=b.xy-a.xy;
	float travelPixels=max(abs(delta.x)*trace.extent.x,abs(delta.y)*trace.extent.y);
	if(travelPixels<2)return hit;
	float epsilon=0.01f/travelPixels;
	float s=min(1.0f,1.5f/travelPixels); // Never hit the originating pixel.
	uint levels=1,w=0,h=0;
	if((trace.flags&2u)!=0u)screenHierarchy.GetDimensions(0,w,h,levels);
	uint highestMip=min(min(trace.maxMip,levels-1),15u);
	// Existing floor-sized reduction mips do not describe the dropped odd edge
	// texels. Restrict acceleration to exactly divisible footprints; odd views
	// still trace correctly at mip0 instead of borrowing an unrelated bound.
	while(highestMip>0&&any((trace.extent&((1u<<highestMip)-1u))!=0u))--highestMip;
	uint mip=highestMip;
	const uint maximumSamples=min(trace.maxSteps,256u);
	uint samples=0;
	// A hierarchy descent revisits the same ray interval. Charge the sample
	// budget only when traversal advances to another interval, so useful
	// fine-depth tests survive the optional hierarchy. Bound both loops.
	for(uint traversal=0;traversal<maximumSamples*(highestMip+1u)&&samples<maximumSamples&&s<1;++traversal)
	{
		float3 p=lerp(a,b,s);
		if(any(p.xy<0)||any(p.xy>=1))break;
		uint2 cellExtent=trace.extent;
		if((trace.flags&2u)!=0u)screenHierarchy.GetDimensions(mip,cellExtent.x,cellExtent.y,levels);
		uint2 cell=min(uint2(p.xy*cellExtent),cellExtent-1);
		// Jump only to this actual mip cell's next boundary. Odd mip extents
		// use queried dimensions; no power-of-two allocation assumption is made.
		float2 boundary=(float2(cell)+float2(delta.x>=0?1:0,delta.y>=0?1:0))/cellExtent;
		float sx=abs(delta.x)>1e-10f?(boundary.x-a.x)/delta.x:2;
		float sy=abs(delta.y)>1e-10f?(boundary.y-a.y)/delta.y:2;
		float exitS=min(1.0f,max(s+epsilon,min(sx,sy)));
		float exitDepth=lerp(a.z,b.z,exitS);
		bool skip=false;
		if((trace.flags&2u)!=0u)
		{
			float bound=screenHierarchy.Load(int3(cell,mip));
			if(isfinite(bound)&&bound>=0&&bound<=1)
			{
				float low=min(OrderedDepth(p.z),OrderedDepth(exitDepth));
				float high=max(OrderedDepth(p.z),OrderedDepth(exitDepth));
				// Existing standardZ HiZ is MAX/farthest: it can reject only a
				// segment wholly BEHIND that cell, never one in front of its max.
				skip=((trace.flags&4u)!=0u?high<OrderedDepth(bound)-1e-6f:low>OrderedDepth(bound)+1e-6f)
					&&ScreenBoundSeparated(p,lerp(a,b,exitS),bound);
			}
		}
		if(skip){++samples;s=exitS+epsilon;mip=min(mip+1,highestMip);continue;}
		if(mip>0){--mip;continue;}
		uint2 pixel=min(uint2(p.xy*trace.extent),trace.extent-1);
		float depth=screenDepth.Load(int3(pixel,0));
		if(ScreenDepthValid(depth))
		{
			float dz=b.z-a.z;
			float candidate=abs(dz)>1e-10f?(depth-a.z)/dz:s;
			candidate=clamp(candidate,s,exitS);
			float3 ray=lerp(a,b,candidate),rayView,surfaceView;
			float3 normal;float roughness;
			if(ScreenPosition(ray.xy,ray.z,rayView)&&ScreenPosition(ray.xy,depth,surfaceView)
				&&length(rayView-surfaceView)<=trace.thickness
				&&length(surfaceView-origin)>trace.thickness*2
				&&ScreenNormal(pixel,normal,roughness)&&dot(normal,-direction)>0.01f)
			{
				float4 color=screenRadiance.Load(int3(pixel,0));
				if(all(isfinite(color)))
				{
					float edge=min(min(ray.x,ray.y),min(1-ray.x,1-ray.y));
					float distanceFade=saturate(1-length(surfaceView-origin)/trace.maxDistance);
					hit.radiance=color.rgb;
					hit.confidence=saturate(edge*20)*distanceFade;
					return hit;
				}
			}
		}
		++samples;s=exitS+epsilon;mip=min(mip+1,highestMip);
	}
	return hit;
}
#endif
