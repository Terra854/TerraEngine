// Conservative morphology with distributed Z-arm coverage and deferred
// integer coefficients. Reconstructed from Intel's published algorithm stages:
// https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html
// No reference source/assets are imported. Fitness/simple heuristics below are
// original; installed image and GPU validation govern their quality acceptance.
#include "CMAA2Common.hlsli"

// Each workgroup scans one contiguous row in parallel. Row admission stays
// deterministic because PrefixRows assigns exclusive row-major offsets first.
groupshared uint scanValues[64];
groupshared uint scanBase;
uint InclusiveScan(uint value,uint lane)
{
	scanValues[lane]=value;
	GroupMemoryBarrierWithGroupSync();
	for(uint offset=1;offset<64;offset<<=1)
	{
		uint previous=scanValues[lane>=offset?lane-offset:0];
		if(lane<offset)previous=0;
		// Everyone reads the previous iteration before any lane updates it.
		GroupMemoryBarrierWithGroupSync();
		scanValues[lane]+=previous;
		GroupMemoryBarrierWithGroupSync();
	}
	return scanValues[lane];
}
[numthreads(64,1,1)]
void CountRows(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
	uint y=group.x;if(y>=config.height)return;
	uint count=0;
	for(uint x=lane;x<config.width;x+=64)count+=edgeFlags[uint2(x,y)]!=0?1:0;
	InclusiveScan(count,lane);
	if(lane==0)rowCounts[y]=scanValues[63];
}
[numthreads(64,1,1)]
void PrefixRows(uint lane:SV_GroupIndex)
{
	if(lane==0)scanBase=0;
	GroupMemoryBarrierWithGroupSync();
	for(uint first=0;first<config.height;first+=64)
	{
		uint y=first+lane;
		uint value=rowCounts[min(y,config.height-1)];
		if(y>=config.height)value=0;
		uint prefix=InclusiveScan(value,lane);
		if(y<config.height)rowOffsets[y]=scanBase+prefix-value;
		GroupMemoryBarrierWithGroupSync();
		if(lane==0)scanBase+=scanValues[63];
		GroupMemoryBarrierWithGroupSync();
	}
	if(lane==0)
	{
		uint admitted=min(scanBase,config.capacity),groups=(admitted+63)/64;
		counters[0]=scanBase;counters[1]=admitted;counters[2]=scanBase-admitted;counters[3]=0;
		// Vulkan guarantees65535 groups per axis; dense images use a bounded2D grid.
		indirectArgs[0]=min(groups,65535u);indirectArgs[1]=(groups+65534u)/65535u;indirectArgs[2]=1;
	}
}
[numthreads(64,1,1)]
void CompactRows(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
	uint y=group.x;if(y>=config.height)return;
	if(lane==0)scanBase=rowOffsets[y];
	GroupMemoryBarrierWithGroupSync();
	for(uint first=0;first<config.width;first+=64)
	{
		uint x=first+lane;
		uint edge=edgeFlags[uint2(min(x,config.width-1),y)]!=0?1:0;
		if(x>=config.width)edge=0;
		uint prefix=InclusiveScan(edge,lane);
		if(edge!=0&&scanBase+prefix-1<config.capacity)candidates[scanBase+prefix-1]=y*config.width+x;
		GroupMemoryBarrierWithGroupSync();
		if(lane==0)scanBase+=scanValues[63];
		GroupMemoryBarrierWithGroupSync();
	}
}
uint ConnectingEdges(int2 p)
{
	// Eight distinct perpendicular connectors around the four center edges
	// complete the published12-edge simple-shape neighborhood.
	return countbits(Edges(p+int2(1,0))&(North|South))
		+countbits(Edges(p-int2(1,0))&(North|South))
		+countbits(Edges(p+int2(0,1))&(East|West))
		+countbits(Edges(p-int2(0,1))&(East|West));
}
uint Walk(int2 start,int2 direction,uint edge)
{
	uint length=0;
	for(uint step=0;step<min(config.maxSearch,64);++step)
	{
		int2 p=start+direction*int(step);
		if(!Valid(p)||(Edges(p)&edge)==0)break;
		++length;
	}
	return length;
}
// Integer coefficient additions are commutative. With maxSearch<=64 and
// one winning complex shape per candidate, at most 4*S+3<=259 candidates can
// affect any destination (same source row/column plus its own simple shape).
// Q=65536 and max priority10 bound every uint sum by169738240, below2^32.
static const uint Quantum=65536;
uint Direction(int2 normal)
{
	return normal.x>0?0:normal.y>0?1:normal.x<0?2:3;
}
void StoreContribution(int2 p,float4 neighborWeights,uint priority)
{
	if(!Valid(p))return;
	int2 offsets[4]={int2(1,0),int2(0,1),int2(-1,0),int2(0,-1)};
	uint budget=priority*Quantum;
	uint4 quantized=0;
	for(uint k=0;k<4;++k)
		if(Valid(p+offsets[k]))quantized[k]=uint(round(saturate(neighborWeights[k])*float(budget)));
	// At most four rounding errors exist; integer normalization makes every
	// proposal a convex center/four-neighbor combination even in that case.
	uint sum=quantized.x+quantized.y+quantized.z+quantized.w;
	if(sum>budget)
	{
		uint excess=sum-budget;
		for(uint k=0;k<4;++k){uint remove=min(quantized[k],excess);quantized[k]-=remove;excess-=remove;}
		sum=budget;
	}
	if(sum==0)return;
	uint address=(uint(p.y)*config.width+uint(p.x))*5;
	for(uint k=0;k<4;++k)if(quantized[k]!=0)InterlockedAdd(coefficients[address+k],quantized[k]);
	InterlockedAdd(coefficients[address+4],budget);
}
float Coverage(int pixelOffset,float leftLength,float rightLength)
{
	// In local oriented coordinates, raster boundary is y=0 left of the
	// one-pixel step and y=1 right of it. Reconstructed silhouette connects
	// (-leftLength/2,0) and (rightLength/2,1). Exact trapezoid integration
	// measures the missing triangle area within each destination footprint.
	float a=max(float(pixelOffset),-0.5f*leftLength);
	float b=min(float(pixelOffset+1),0.5f*rightLength);
	if(b<=a)return 0;
	float inverseSpan=2.0f/(leftLength+rightLength);
	float ya=(a+0.5f*leftLength)*inverseSpan;
	float yb=(b+0.5f*leftLength)*inverseSpan;
	float underLine=(b-a)*0.5f*(ya+yb);
	return saturate(pixelOffset<0?underLine:(b-a)-underLine);
}
void ScatterZ(int2 center,uint orientation,float leftLength,float rightLength)
{
	int2 axis=orientation<2?int2(1,0):int2(0,1);
	int2 normal=orientation<2?int2(0,1):int2(1,0);
	if((orientation&1)!=0)normal=-normal;
	int first=-int(ceil(0.5f*leftLength));
	int last=int(ceil(0.5f*rightLength))-1;
	// Every arm pixel contributes to its own orthogonal neighbor only; RGB is
	// read later by resolve, preserving arbitrary finite source HDR range.
	for(int offset=first;offset<=last;++offset)
	{
		float weight=Coverage(offset,leftLength,rightLength);
		float4 weights=0;
		weights[Direction(offset<0?-normal:normal)]=weight;
		StoreContribution(center+axis*offset,weights,10);
	}
}
[numthreads(64,1,1)]
void Classify(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
	uint index=(group.y*65535u+group.x)*64u+lane;
	if(index>=counters[1])return;
	uint pixel=candidates[min(index,config.capacity-1)];if(pixel>=config.width*config.height)return;
	int2 p=int2(pixel%config.width,pixel/config.width);
	uint center=Edges(p),bestOrientation=0;
	float bestFitness=0,bestLeft=0,bestRight=0;
	for(uint orientation=0;orientation<4;++orientation)
	{
		int2 axis=(orientation<2)?int2(1,0):int2(0,1);
		int2 normal=(orientation<2)?int2(0,1):int2(1,0);
		if((orientation&1)!=0)normal=-normal;
		uint edge=normal.y>0?South:normal.y<0?North:normal.x>0?East:West;
		uint connector=axis.x!=0?West:North;
		// A Z anchor owns a normal edge and a perpendicular connector. Its
		// incoming arm is one row/column back; outgoing arm starts at the anchor.
		if((center&connector)==0||(center&edge)==0)continue;
		uint left=Walk(p-axis-normal,-axis,edge),right=Walk(p,axis,edge);
		if(left==0||right==0)continue;
		// Stop extension at the arm-ratio boundary. This retains an admissible
		// symmetric subshape rather than dropping a valid shorter silhouette.
		float smaller=float(min(left,right));
		float limit=smaller*max(config.armRatio,1.0f);
		float measuredLeft=min(float(left),limit),measuredRight=min(float(right),limit);
		// Original conservative fitness: favor balanced arms and penalize branch
		// junctions in the12-edge neighborhood. The article does not publish the
		// exact reference score formula; this is not a source-equivalent claim.
		uint connections=ConnectingEdges(p);
		float branches=float(max(int(countbits(center))-2,0)+max(int(connections)-4,0));
		float fitness=min(measuredLeft,measuredRight)/max(measuredLeft,measuredRight)/(1.0f+branches);
		float trim=1.0f-fitness;
		measuredLeft=max(0,measuredLeft-trim);measuredRight=max(0,measuredRight-trim);
		if(measuredLeft+measuredRight<5.0f)continue;
		if(fitness>bestFitness)
		{bestFitness=fitness;bestOrientation=orientation;bestLeft=measuredLeft;bestRight=measuredRight;}
	}
	if(bestFitness>0)
	{
		ScatterZ(p,bestOrientation,bestLeft,bestRight);
		return;
	}
	// Simple fallback uses all12 distinct edges around the four neighbors.
	// It preserves opposite-edge thin features/straight boundaries and emits
	// one low-priority convex proposal; overlapping complex shapes dominate10:1.
	uint horizontal=center&(East|West),vertical=center&(North|South);
	uint connections=ConnectingEdges(p);
	if(countbits(horizontal)==1&&countbits(vertical)==1&&connections<=4)
	{
		float4 weights=0;
		for(uint k=0;k<4;++k)if((center&(1u<<k))!=0)weights[k]=0.125f;
		StoreContribution(p,weights,1);
	}
}
