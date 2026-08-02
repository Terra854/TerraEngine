// Groups visible records into bounded material bins.
#include "VirtualGeometryAbi.hlsli"

struct BinConstants { uint visibleCount; uint binCapacity; uint binCount; uint reserved; };
[[vk::binding(0, 1)]] StructuredBuffer<uint4> visible;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint> bins;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint> diagnostics;
[[vk::push_constant]] ConstantBuffer<BinConstants> Bin;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Bin.visibleCount || Bin.binCount == 0u) return;
	if(visible[id.x].x == TE_VIRTUAL_GEOMETRY_INVALID_INDEX) return;
	// Resolve stores material bin in the low byte and route identity in the next
	// byte. Derivatives remain in word Z for later material reconstruction.
	uint bin = (visible[id.x].w & 0xffu) % Bin.binCount;
	uint slot;
	InterlockedAdd(bins[bin], 1u, slot);
	if (slot >= Bin.binCapacity) InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW);
}
