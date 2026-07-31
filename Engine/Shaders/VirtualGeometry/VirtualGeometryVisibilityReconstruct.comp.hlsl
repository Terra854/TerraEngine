// Copies validated visibility records while enforcing output capacity.
#include "VirtualGeometryAbi.hlsli"

StructuredBuffer<uint4> inputRecords : register(t0);
RWStructuredBuffer<uint4> outputRecords : register(u0);
RWStructuredBuffer<uint> diagnostics : register(u1);
cbuffer VisibilityConstants : register(b0)
{
	uint recordCount;
	uint outputCapacity;
	uint generation;
	uint reserved;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= recordCount) return;
	if (id.x >= outputCapacity)
	{
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW);
		return;
	}
	outputRecords[id.x] = inputRecords[id.x];
}
