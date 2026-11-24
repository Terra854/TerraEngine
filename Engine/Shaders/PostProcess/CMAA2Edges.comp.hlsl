// Conservative morphological AA: edge detection only. Display-linear source
// color and alpha are preserved for the later deferred resolve.
#include "CMAA2Common.hlsli"
#include "SpatialAALuminance.hlsli"

float PerceptualLuma(int2 p)
{
	p=clamp(p,int2(0,0),int2(config.width,config.height)-1);
	return SpatialPerceptualLuminance(sourceColor.Load(int3(p,0)).rgb);
}
float Difference(int2 a,int2 b) { return abs(PerceptualLuma(a)-PerceptualLuma(b)); }
// The same canonical pair rule is used from both incident pixels. LCA compares
// connecting perpendicular discontinuities, so reverse edges agree exactly.
bool HasEdge(int2 a,int2 b)
{
	if(!Valid(a)||!Valid(b))return false;
	int2 perpendicular=(a.y==b.y)?int2(0,1):int2(1,0);
	float competing=max(max(Difference(a,a+perpendicular),Difference(a,a-perpendicular)),
		max(Difference(b,b+perpendicular),Difference(b,b-perpendicular)));
	return Difference(a,b)>max(config.threshold,competing*config.localContrast);
}
[numthreads(8,8,1)]
void DetectEdges(uint3 tid:SV_DispatchThreadID)
{
	int2 p=int2(tid.xy); if(!Valid(p))return;
	edgeFlags[p]=(HasEdge(p,p+int2(1,0))?East:0)|(HasEdge(p,p+int2(0,1))?South:0)
		|(HasEdge(p,p-int2(1,0))?West:0)|(HasEdge(p,p-int2(0,1))?North:0);
	uint offset=(tid.y*config.width+tid.x)*5;
	for(uint k=0;k<5;++k)coefficients[offset+k]=0;
}
