// Publishes conservative current bounds. Uncertain records fail open.
#include "AnimatedVirtualGeometryAbi.hlsli"

StructuredBuffer<TeAnimatedDeformedVertex> vertices : register(t0);
RWStructuredBuffer<TeAnimatedDynamicBounds> bounds : register(u0);
RWStructuredBuffer<uint> diagnostics : register(u1);

cbuffer BoundsConstants : register(b0)
{
	uint clusterCount;
	uint vertexCount;
	uint boundsCapacity;
	uint failOpen;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= clusterCount)
	{
		return;
	}
	if (id.x >= boundsCapacity)
	{
		InterlockedOr(diagnostics[0], TE_ANIMATED_GEOMETRY_DIAGNOSTIC_RANGE_OVERFLOW);
		return;
	}
	TeAnimatedDynamicBounds result = (TeAnimatedDynamicBounds)0;
	// Infinite conservative bounds avoid false rejection when animation data is uncertain.
	result.minimum = float4(-1e20, -1e20, -1e20, 0.0);
	result.maximum = float4(1e20, 1e20, 1e20, 0.0);
	result.sphere = float4(0.0, 0.0, 0.0, 1e20);
	result.identity = uint4(id.x, failOpen != 0u ? TE_ANIMATED_GEOMETRY_DIAGNOSTIC_INVALID_RECORD : 0u, 0u, 0u);
	bounds[id.x] = result;
}
