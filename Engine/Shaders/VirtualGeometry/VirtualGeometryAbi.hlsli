// VirtualGeometryAbi.hlsli - shared static virtual-geometry device ABI.
//
// Keep policy out of this file. C++ preparation and every backend shader use
// these identities, capacity rules, and layouts so work from different frame,
// geometry, residency, or descriptor generations can never be combined.

#ifndef TERRAENGINE_VIRTUAL_GEOMETRY_ABI_HLSLI
#define TERRAENGINE_VIRTUAL_GEOMETRY_ABI_HLSLI

#define TE_VIRTUAL_GEOMETRY_ABI_VERSION 1u
#define TE_VIRTUAL_GEOMETRY_INVALID_INDEX 0xffffffffu

#define TE_VIRTUAL_GEOMETRY_DESCRIPTOR_SET 3u
#define TE_VIRTUAL_GEOMETRY_BINDING_GENERATIONS 0u
#define TE_VIRTUAL_GEOMETRY_BINDING_WORLD_ROOTS 1u
#define TE_VIRTUAL_GEOMETRY_BINDING_INSTANCES 2u
#define TE_VIRTUAL_GEOMETRY_BINDING_CLUSTERS 3u
#define TE_VIRTUAL_GEOMETRY_BINDING_PAGE_DIRECTORY 4u
#define TE_VIRTUAL_GEOMETRY_BINDING_PAGE_PAYLOADS 5u
#define TE_VIRTUAL_GEOMETRY_BINDING_CANDIDATE_PACKETS 6u
#define TE_VIRTUAL_GEOMETRY_BINDING_VISIBLE_PACKETS 7u
#define TE_VIRTUAL_GEOMETRY_BINDING_REJECTED_PACKETS 8u
#define TE_VIRTUAL_GEOMETRY_BINDING_FAIL_OPEN_PACKETS 9u
#define TE_VIRTUAL_GEOMETRY_BINDING_PAGE_FEEDBACK 10u
#define TE_VIRTUAL_GEOMETRY_BINDING_GENERATED_WORK 11u
#define TE_VIRTUAL_GEOMETRY_BINDING_COUNTERS 12u
#define TE_VIRTUAL_GEOMETRY_BINDING_DIAGNOSTICS 13u
#define TE_VIRTUAL_GEOMETRY_BINDING_COUNT 14u

#define TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FALLBACK 0x00000001u
#define TE_VIRTUAL_GEOMETRY_PACKET_FLAG_FAIL_OPEN 0x00000002u
#define TE_VIRTUAL_GEOMETRY_PACKET_FLAG_HISTORY_VALID 0x00000004u
#define TE_VIRTUAL_GEOMETRY_PACKET_FLAG_DEFORMED 0x00000008u

#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION 0x00000001u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_QUEUE_OVERFLOW 0x00000002u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW 0x00000004u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_FEEDBACK_OVERFLOW 0x00000008u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INDIRECT_OVERFLOW 0x00000010u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_MISSING_PAGE 0x00000020u
#define TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD 0x00000040u

struct TeVirtualGeometryGenerationRecord
{
	uint2 frameNumber;
	uint2 rendererGeneration;
	uint2 geometryGeneration;
	uint2 hierarchyGeneration;
	uint2 pageDirectoryGeneration;
	uint2 residencyGeneration;
	uint2 descriptorGeneration;
	uint viewId;
	uint abiVersion;
};

struct TeVirtualGeometryCapacityRecord
{
	uint instanceQueueCapacity;
	uint clusterQueueCapacity;
	uint candidatePacketCapacity;
	uint visiblePacketCapacity;
	uint rejectedPacketCapacity;
	uint failOpenPacketCapacity;
	uint pageFeedbackCapacity;
	uint generatedWorkCapacity;
};

struct TeVirtualGeometryCounterRecord
{
	uint visitedInstances;
	uint visitedClusters;
	uint candidatePackets;
	uint visiblePackets;
	uint rejectedPackets;
	uint failOpenPackets;
	uint pageFeedbackRequests;
	uint generatedCommands;
	uint diagnosticFlags;
	uint3 reserved;
};

struct TeVirtualGeometryGeneratedWorkRecord
{
	uint route;
	uint commandOffset;
	uint commandCount;
	uint maximumCommandCount;
	uint commandStride;
	uint commandAlignment;
	uint generationRecordIndex;
	uint flags;
};

bool TeVirtualGeometryGenerationMatches(
	TeVirtualGeometryGenerationRecord left,
	TeVirtualGeometryGenerationRecord right)
{
	return all(left.frameNumber == right.frameNumber)
		&& all(left.rendererGeneration == right.rendererGeneration)
		&& all(left.geometryGeneration == right.geometryGeneration)
		&& all(left.hierarchyGeneration == right.hierarchyGeneration)
		&& all(left.pageDirectoryGeneration == right.pageDirectoryGeneration)
		&& all(left.residencyGeneration == right.residencyGeneration)
		&& all(left.descriptorGeneration == right.descriptorGeneration)
		&& left.viewId == right.viewId
		&& left.abiVersion == TE_VIRTUAL_GEOMETRY_ABI_VERSION
		&& right.abiVersion == TE_VIRTUAL_GEOMETRY_ABI_VERSION;
}

bool TeVirtualGeometryWithinCapacity(uint index, uint capacity)
{
	return capacity != 0u && index < capacity;
}

#endif
