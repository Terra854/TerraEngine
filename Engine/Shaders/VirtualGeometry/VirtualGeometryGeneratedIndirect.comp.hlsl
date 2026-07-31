// Converts compact packets into bounded mesh/compute/indexed indirect records.
#include "VirtualGeometryAbi.hlsli"

struct Packet
{
	uint packetId; uint viewId; uint viewType; uint frameIdLow;
	uint4 objectIdWords; uint4 meshIdWords; uint4 materialIdWords;
	uint selectedLod; uint clusterId; uint meshletOffset; uint meshletCount;
	float3 boundsMin; float boundsMinPadding; float3 boundsMax; float boundsMaxPadding;
	float4 projectedBounds;
	uint pageId; uint fallbackPageId; uint payloadSource; uint emittedSource;
	uint2 pagePayloadByteOffset; uint2 pagePayloadByteSize;
	uint payloadBufferIndex; uint payloadDescriptorIndex; uint generatedCommandIndex;
	uint generatedCommandTaskGroupCount; uint debugIdentity; uint flags; uint2 tailPadding;
};

[[vk::binding(4, 0)]] StructuredBuffer<Packet> packets;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint4> args;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> diagnostics;

struct IndirectConstants
{
	uint packetCount;
	uint argumentCapacity;
	uint commandStride;
	uint routeMask;
};
[[vk::push_constant]] ConstantBuffer<IndirectConstants> Indirect;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Indirect.packetCount)
	{
		return;
	}
	Packet packet = packets[id.x];
	// Emitted source is an enum, while routeMask is a bit set. Zeroed or
	// rejected compacted slots therefore publish no command. The executor mask
	// admits only high-detail, fallback, and conservative embedded coverage.
	const uint routeBit = packet.emittedSource < 32u
		? (1u << packet.emittedSource)
		: 0u;
	if ((routeBit & Indirect.routeMask) == 0u)
	{
		return;
	}
	if (packet.generatedCommandIndex >= Indirect.argumentCapacity)
	{
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INDIRECT_OVERFLOW);
		return;
	}
	// The first three words are exactly VkDrawMeshTasksIndirectCommandEXT;
	// word four remains padding/debug identity at the required 16-byte stride.
	// Every packet in a compatible batch targets its first descriptor and carries
	// the conservative full-batch count. If compaction rejects only a middle
	// packet, a survivor still publishes the original contiguous descriptor run;
	// if it rejects the entire batch, the cleared command remains zero.
	InterlockedMax(
		args[packet.generatedCommandIndex].x,
		packet.generatedCommandTaskGroupCount);
	InterlockedOr(args[packet.generatedCommandIndex].y, 1u);
	InterlockedOr(args[packet.generatedCommandIndex].z, 1u);
	InterlockedOr(args[packet.generatedCommandIndex].w, packet.flags);
}
