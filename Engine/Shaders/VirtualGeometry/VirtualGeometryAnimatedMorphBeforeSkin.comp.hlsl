// Applies compact morph deltas before skinning and writes the shared vertex ABI.
#include "AnimatedVirtualGeometryAbi.hlsli"

StructuredBuffer<float4> source : register(t0);
StructuredBuffer<float4> morph : register(t1);
RWStructuredBuffer<TeAnimatedDeformedVertex> outputVertices : register(u0);
RWStructuredBuffer<uint> diagnostics : register(u1);

cbuffer MorphConstants : register(b0)
{
	uint vertexCount;
	uint outputCapacity;
	float morphWeight;
	uint flags;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= vertexCount)
	{
		return;
	}
	if (id.x >= outputCapacity)
	{
		InterlockedOr(diagnostics[0], TE_ANIMATED_GEOMETRY_DIAGNOSTIC_RANGE_OVERFLOW);
		return;
	}
	float4 position = source[id.x] + morph[id.x] * morphWeight;
	TeAnimatedDeformedVertex vertex = (TeAnimatedDeformedVertex)0;
	vertex.currentPosition = position;
	vertex.previousPosition = position;
	outputVertices[id.x] = vertex;
}
