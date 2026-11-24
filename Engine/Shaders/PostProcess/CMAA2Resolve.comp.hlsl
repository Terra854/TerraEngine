// Resolve commutative deferred coefficients against the preserved linear image.
// Each invocation owns one output pixel and copies its original alpha directly.
#include "CMAA2Common.hlsli"

[numthreads(8,8,1)]
void Resolve(uint3 tid:SV_DispatchThreadID)
{
	int2 p=int2(tid.xy);if(!Valid(p))return;
	float4 center=sourceColor.Load(int3(p,0));
	// Never publish a partially processed image if a bounded queue overflows.
	if(counters[2]!=0){destination[p]=center;return;}
	uint address=(tid.y*config.width+tid.x)*5;
	uint total=coefficients[address+4];
	if(total==0){destination[p]=center;return;}
	uint4 accumulated=uint4(coefficients[address],coefficients[address+1],coefficients[address+2],coefficients[address+3]);
	uint sum=accumulated.x+accumulated.y+accumulated.z+accumulated.w;
	// Proven convex sums preserve finite input HDR values without quantizing
	// color. This explicit order gives bit-stable resolve for the same input.
	float inverseTotal=1.0f/float(total);
	float3 rgb=center.rgb*(float(total-sum)*inverseTotal);
	int2 offsets[4]={int2(1,0),int2(0,1),int2(-1,0),int2(0,-1)};
	for(uint k=0;k<4;++k)if(accumulated[k]!=0&&Valid(p+offsets[k]))
		rgb+=sourceColor.Load(int3(clamp(p+offsets[k],int2(0,0),int2(config.width,config.height)-1),0)).rgb*(float(accumulated[k])*inverseTotal);
	destination[p]=float4(rgb,center.a);
}
