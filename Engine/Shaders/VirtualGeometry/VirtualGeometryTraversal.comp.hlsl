// Bounded instance/cluster traversal. The traversal buffer starts with the
// immutable instance roots selected by worker preparation, followed by the
// cluster hierarchy records addressed by root/firstChild indices. Every path
// emits a conservative request on malformed generation or queue pressure.
#include "VirtualGeometryAbi.hlsli"

struct TraversalInstance
{
	uint root;
	uint generation;
	uint firstChild;
	uint childCount;
};

struct TraversalRequest
{
	uint instance;
	uint cluster;
	uint flags;
};

struct TraversalConstants
{
	uint instanceCount;
	uint requestCapacity;
	uint expectedGeneration;
	uint failOpen;
};

// Set 0 is shared by the executor's traversal, compaction, and generated-work
// stages. The same structured buffer supplies immutable roots and hierarchy
// records, so no worker writes descriptor-backed state while command recording.
[[vk::binding(0, 0)]] StructuredBuffer<TraversalInstance> traversalInstances;
[[vk::binding(1, 0)]] RWStructuredBuffer<TraversalRequest> traversalRequests;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> traversalCounters;
[[vk::push_constant]] ConstantBuffer<TraversalConstants> Traversal;

static const uint kMaximumTraversalStack = 32u;

void EmitTraversalRequest(uint instanceIndex, uint clusterIndex, uint flags)
{
	uint requestIndex = 0u;
	InterlockedAdd(traversalCounters[0], 1u, requestIndex);
	if(!TeVirtualGeometryWithinCapacity(requestIndex, Traversal.requestCapacity))
	{
		InterlockedOr(traversalCounters[1], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_QUEUE_OVERFLOW);
		return;
	}
	TraversalRequest request;
	request.instance = instanceIndex;
	request.cluster = clusterIndex;
	request.flags = flags;
	traversalRequests[requestIndex] = request;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= Traversal.instanceCount)
	{
		return;
	}

	uint hierarchyCount = 0u;
	uint hierarchyStride = 0u;
	traversalInstances.GetDimensions(hierarchyCount, hierarchyStride);
	const TraversalInstance rootInstance = traversalInstances[id.x];
	if(rootInstance.generation != Traversal.expectedGeneration
		|| rootInstance.root >= hierarchyCount)
	{
		if(Traversal.failOpen != 0u)
		{
			EmitTraversalRequest(id.x, rootInstance.root,
				TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FAIL_OPEN);
		}
		else
		{
			InterlockedOr(traversalCounters[1], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
		}
		return;
	}

	uint pendingClusters[kMaximumTraversalStack];
	uint pendingCount = 1u;
	pendingClusters[0] = rootInstance.root;
	while(pendingCount > 0u)
	{
		const uint clusterIndex = pendingClusters[--pendingCount];
		if(clusterIndex >= hierarchyCount)
		{
			EmitTraversalRequest(id.x, rootInstance.root,
				TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FAIL_OPEN);
			continue;
		}

		const TraversalInstance cluster = traversalInstances[clusterIndex];
		if(cluster.generation != Traversal.expectedGeneration)
		{
			if(Traversal.failOpen != 0u)
			{
				EmitTraversalRequest(id.x, clusterIndex,
					TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FAIL_OPEN);
			}
			else
			{
				InterlockedOr(traversalCounters[1], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
			}
			continue;
		}

		if(cluster.childCount == 0u)
		{
			EmitTraversalRequest(id.x, clusterIndex, 0u);
			continue;
		}

		const uint availableChildren = hierarchyCount - min(cluster.firstChild, hierarchyCount);
		const uint childCount = min(cluster.childCount, availableChildren);
		if(childCount != cluster.childCount || childCount > kMaximumTraversalStack - pendingCount)
		{
			InterlockedOr(traversalCounters[1], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_QUEUE_OVERFLOW);
			EmitTraversalRequest(id.x, clusterIndex,
				TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FAIL_OPEN);
			continue;
		}
		for(uint childOffset = 0u; childOffset < childCount; ++childOffset)
		{
			pendingClusters[pendingCount++] = cluster.firstChild + childOffset;
		}
	}
}
