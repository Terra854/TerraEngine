// VirtualGeometryPacketCompaction.comp.hlsl
//
// Compacts virtual-geometry packet candidates into a generated packet stream.
// The shader intentionally mirrors the CPU fail-open contract: frustum and
// missing-resource rejection may remove work, but invalid or fallback packets
// are preserved so the mesh-shader path never loses coverage while the GPU
// driven path is being validated.

struct GpuVirtualGeometryPacketRecord
{
	uint packetId;
	uint viewId;
	uint viewType;
	uint frameIdLow;
	uint4 objectIdWords;
	uint4 meshIdWords;
	uint4 materialIdWords;
	uint selectedLod;
	uint clusterId;
	uint meshletOffset;
	uint meshletCount;
	float3 boundsMin;
	float boundsMinPadding;
	float3 boundsMax;
	float boundsMaxPadding;
	float4 projectedBounds;
	uint pageId;
	uint fallbackPageId;
	uint payloadSource;
	uint emittedSource;
	uint2 pagePayloadByteOffset;
	uint2 pagePayloadByteSize;
	uint payloadBufferIndex;
	uint payloadDescriptorIndex;
	uint debugIdentity;
	uint flags;
};

struct GpuPacketCompactionConstants
{
	uint candidatePacketCount;
	uint outputCapacity;
	uint counterOffset;
	uint failOpenOnInvalid;
};

struct GpuPacketCompactionCounters
{
	uint candidatePackets;
	uint compactedPackets;
	uint rejectedPackets;
	uint failOpenPackets;
};

[[vk::binding(0, 0)]] StructuredBuffer<GpuVirtualGeometryPacketRecord> candidatePackets;
[[vk::binding(1, 0)]] RWStructuredBuffer<GpuVirtualGeometryPacketRecord> compactedPackets;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> generatedSubmissions;
[[vk::binding(3, 0)]] RWStructuredBuffer<GpuPacketCompactionCounters> compactionCounters;

[[vk::push_constant]] ConstantBuffer<GpuPacketCompactionConstants> Compaction;

bool ProjectedBoundsOutsideView(float4 projectedBounds)
{
	const float minX = min(projectedBounds.x, projectedBounds.z);
	const float maxX = max(projectedBounds.x, projectedBounds.z);
	const float minY = min(projectedBounds.y, projectedBounds.w);
	const float maxY = max(projectedBounds.y, projectedBounds.w);
	return maxX < 0.0f || maxY < 0.0f || minX > 1.0f || minY > 1.0f;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint packetIndex = dispatchThreadId.x;
	if (packetIndex >= Compaction.candidatePacketCount)
	{
		return;
	}

	// The backend clears the counter buffer before dispatch. Each task only
	// contributes atomic increments, which keeps early returns legal and avoids
	// a divergent group barrier.
	InterlockedAdd(compactionCounters[Compaction.counterOffset].candidatePackets, 1u);
	const GpuVirtualGeometryPacketRecord packet = candidatePackets[packetIndex];
	const bool fallbackPacket = (packet.flags & 1u) != 0u;
	const bool missingResource = packet.meshletCount == 0u;
	const bool outsideView = ProjectedBoundsOutsideView(packet.projectedBounds);
	const bool failOpen = fallbackPacket || (missingResource && Compaction.failOpenOnInvalid != 0u);

	if (!failOpen && (missingResource || outsideView))
	{
		InterlockedAdd(compactionCounters[Compaction.counterOffset].rejectedPackets, 1u);
		return;
	}

	if (failOpen)
	{
		InterlockedAdd(compactionCounters[Compaction.counterOffset].failOpenPackets, 1u);
	}

	uint compactedIndex = 0u;
	InterlockedAdd(compactionCounters[Compaction.counterOffset].compactedPackets, 1u, compactedIndex);
	if (compactedIndex >= Compaction.outputCapacity)
	{
		InterlockedAdd(compactionCounters[Compaction.counterOffset].rejectedPackets, 1u);
		return;
	}

	compactedPackets[compactedIndex] = packet;
	generatedSubmissions[compactedIndex] = uint4(packet.packetId, packet.meshletOffset, packet.meshletCount, packet.selectedLod);
}
