#pragma pack_matrix(column_major)

// Default.task.hlsl is the task-stage visibility filter for the default
// mesh-shader route. It consumes TEMF meshlet bounds plus the selected
// virtual-geometry cluster range chosen on the CPU, compacts visible meshlet indices
// into TaskPayload, and always fails open when culling data or the Hi-Z pyramid is
// not trustworthy.

// Sphere + cone bounds emitted per meshlet by meshoptimizer.
struct MeshletBounds
{
	float3 center;
	float radius;
	float3 coneApex;
	float coneCutoff;
	float3 coneAxis;
	float coneApexOffset;
};

// Meshlet table emitted by the mesh compiler. The task shader uses it only to
// build a tighter Hi-Z screen rectangle; actual triangle emission still lives
// in Default.mesh.hlsl and consumes the same descriptor set.
struct MeshletRecord
{
	uint vertexOffset;
	uint vertexCount;
	uint primitiveIndexOffset;
	uint primitiveIndexCount;
};

// Packed decode constants for direct GPU consumption of TEMF payloads.
struct MeshDecodeInfo
{
	float4 positionOffset;
	float4 positionScale;
	float4 uvOffsetScale;
	uint4 counts; // x/y/z are mesh stream counts; w is the total meshlet count with the high bit reserved for future flags.
};

struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	column_major float4x4 viewMatrix;
	column_major float4x4 invProjection;
	uint4 renderSettings0; // x: shadowRes, y: shadowFilter, z: retainedLightBudget, w: enableLTC
	uint4 renderSettings1; // x: enableIBL, y: aniso, z: shadowLightBudget, w: uploadedLightBudget
	float4 renderSettings2; // x: indirectStrength, y: aoStrength, z: aoQuality, w: hiZPyramidAvailable
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};

struct MeshletCullingCounters
{
	uint meshletsTotal;
	uint meshletsDispatched;
	uint meshletsCulled;
	uint meshletsCulledHiZ;
	uint hiZTests;
	uint hiZProjectionFailed;
	uint hiZFullRectCulled;
	uint hiZRectCoverageCulled;
	uint hiZCenterCulled;
	uint hiZVertexCoverageCulled;
	uint hiZNoOcclusionProof;
	uint hiZReserved;
};

MeshletCullingCounters MakeEmptyMeshletCullingCounters()
{
	MeshletCullingCounters counters;
	counters.meshletsTotal = 0u;
	counters.meshletsDispatched = 0u;
	counters.meshletsCulled = 0u;
	counters.meshletsCulledHiZ = 0u;
	counters.hiZTests = 0u;
	counters.hiZProjectionFailed = 0u;
	counters.hiZFullRectCulled = 0u;
	counters.hiZRectCoverageCulled = 0u;
	counters.hiZCenterCulled = 0u;
	counters.hiZVertexCoverageCulled = 0u;
	counters.hiZNoOcclusionProof = 0u;
	counters.hiZReserved = 0u;
	return counters;
}

// Task stage compacts visible meshlets for each task-group chunk.
static const uint kTaskBatchSize = 32u;
static const uint kMaxMeshletVertices = 64u;
static const uint kMaxMeshletTriangles = 124u;
static const uint kMaxCoverageTriangleSamples = 64u;
static const float kMaxVertexCoverageRectExtentPixels = 160.0f;
static const float kMaxTriangleCoverageRectExtentPixels = 512.0f;
static const float kMaxRectCoverageRectExtentPixels = 320.0f;
static const float kMaxCenterCoverageRectExtentPixels = 128.0f;
struct TaskPayload
{
	uint meshletIndices[kTaskBatchSize];
};

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(3, 0)]] RWStructuredBuffer<MeshletCullingCounters> meshletCounters;
[[vk::binding(0, 1)]] ByteAddressBuffer meshletVertexBytes;
[[vk::binding(1, 1)]] ByteAddressBuffer primitiveIndices;
[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;
[[vk::binding(3, 1)]] ByteAddressBuffer meshletVertexRemap;
[[vk::binding(4, 1)]] StructuredBuffer<MeshDecodeInfo> meshDecodeInfo;
[[vk::binding(5, 1)]] StructuredBuffer<MeshletBounds> meshletBounds;
[[vk::binding(9, 0)]] StructuredBuffer<uint> globalBuffers[];
[[vk::binding(10, 0)]] Texture2D<float> hiZPyramids[2];
[[vk::binding(11, 0)]] SamplerState hiZSampler;

void FlushMeshletCullingCounters(const uint statsIndex, const MeshletCullingCounters counters)
{
	uint ignored = 0u;
	// Task groups inspect up to 32 meshlets. Accumulating locally avoids several
	// global atomics per meshlet while preserving the exact diagnostic totals.
	if (counters.meshletsTotal != 0u) InterlockedAdd(meshletCounters[statsIndex].meshletsTotal, counters.meshletsTotal, ignored);
	if (counters.meshletsDispatched != 0u) InterlockedAdd(meshletCounters[statsIndex].meshletsDispatched, counters.meshletsDispatched, ignored);
	if (counters.meshletsCulled != 0u) InterlockedAdd(meshletCounters[statsIndex].meshletsCulled, counters.meshletsCulled, ignored);
	if (counters.meshletsCulledHiZ != 0u) InterlockedAdd(meshletCounters[statsIndex].meshletsCulledHiZ, counters.meshletsCulledHiZ, ignored);
	if (counters.hiZTests != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZTests, counters.hiZTests, ignored);
	if (counters.hiZProjectionFailed != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZProjectionFailed, counters.hiZProjectionFailed, ignored);
	if (counters.hiZFullRectCulled != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZFullRectCulled, counters.hiZFullRectCulled, ignored);
	if (counters.hiZRectCoverageCulled != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZRectCoverageCulled, counters.hiZRectCoverageCulled, ignored);
	if (counters.hiZCenterCulled != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZCenterCulled, counters.hiZCenterCulled, ignored);
	if (counters.hiZVertexCoverageCulled != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZVertexCoverageCulled, counters.hiZVertexCoverageCulled, ignored);
	if (counters.hiZNoOcclusionProof != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZNoOcclusionProof, counters.hiZNoOcclusionProof, ignored);
	if (counters.hiZReserved != 0u) InterlockedAdd(meshletCounters[statsIndex].hiZReserved, counters.hiZReserved, ignored);
}

groupshared TaskPayload gTaskPayload;

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint materialIndex;
	uint meshDataIndex;
	uint textureIndex;
	uint vrsActive;
	uint taskCullingEnabled;
	uint statsSlotIndex;
	float cullingEpsilon;
	uint lightDataOffset;
	uint clusterDataOffset;
	uint lightIndexOffset;
	uint indirectDrawOffset;
	uint virtualMeshletOffset; // First meshlet of the selected virtual-geometry cluster range.
	uint virtualMeshletCount; // Number of meshlets in the selected cluster; zero means consume the rest of the meshlet table.
	uint selectedVirtualLod; // CPU-selected LOD index for diagnostics and debug visualization.
	uint virtualGeometryFlags; // Debug/diagnostic bits authored by RenderSystem.
	// Cluster/page fields mirror the CPU's sparse adaptive selection packet.
	// RenderSystem may submit a compatible contiguous cluster packet batch, so
	// this draw references one record or a packet run without changing meshlet
	// output semantics.
	uint selectedVirtualClusterOffset; // First selected cluster record in the per-view packet.
	uint selectedVirtualClusterCount; // Number of selected clusters in this packet batch draw.
	uint selectedVirtualPageNeedOffset; // First missing-page request record for this draw.
	uint selectedVirtualPageNeedCount; // Number of desired pages that fell back to resident coverage.
	uint virtualGeometryPacketBufferIndex; // Bindless storage-buffer slot for the current view's GPU packet table, or 0xFFFFFFFF when unavailable.
	uint virtualGeometryBatchBufferIndex; // Bindless storage-buffer slot for the current view's GPU batch table, or 0xFFFFFFFF when unavailable.
	uint virtualGeometryPagePayloadBufferIndex; // Bindless storage-buffer slot for page-source metadata, or 0xFFFFFFFF when unavailable.
	uint virtualGeometryPayloadSource; // Page payload source label selected by RenderSystem for this draw.
	uint virtualGeometryEmittedSource; // Emitted source label after fallback substitution.
	uint virtualGeometryPagePayloadByteOffset; // Low 32 bits of the page-local payload byte offset.
	uint virtualGeometryPagePayloadByteSize; // Low 32 bits of the page-local payload byte size.
	uint virtualGeometryPacketDescriptorIndex; // Packet-local page descriptor generated for this submitted draw.
	float4 editorTint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

static const uint kMaxStatsSlots = 16u;
static const uint kHiZViewCount = 2u;
static const uint kVirtualGeometryFlagConeCullingSafe = 0x20u;
static const uint kVirtualGeometryDebugModeShift = 8u;
static const uint kVirtualGeometryDebugModeMask = 0xFFu;
static const uint kInvalidVirtualGeometryBufferIndex = 0xFFFFFFFFu;
static const uint kVirtualGeometryPayloadDescriptorStrideWords = 32u;
static const uint kVirtualGeometryPayloadDescriptorSourceWord = 2u;
static const uint kVirtualGeometryPayloadDescriptorBufferIndexWord = 8u;
static const uint kVirtualGeometryPayloadDescriptorIndexWord = 9u;
static const uint kVirtualGeometryPayloadDescriptorVertexWordOffset = 16u;
static const uint kVirtualGeometryPayloadDescriptorVertexWordSize = 17u;
static const uint kVirtualGeometryPayloadDescriptorPrimitiveWordOffset = 18u;
static const uint kVirtualGeometryPayloadDescriptorPrimitiveByteSize = 19u;
static const uint kVirtualGeometryPayloadDescriptorRemapWordOffset = 20u;
static const uint kVirtualGeometryPayloadDescriptorRemapCount = 21u;
static const uint kVirtualGeometryPayloadDescriptorMeshletWordOffset = 22u;
static const uint kVirtualGeometryPayloadDescriptorMeshletCount = 23u;
static const uint kVirtualGeometryPayloadDescriptorBoundsWordOffset = 24u;
static const uint kVirtualGeometryPayloadDescriptorBoundsCount = 25u;
static const uint kVirtualGeometryPayloadDescriptorDecodeWordOffset = 26u;
static const uint kVirtualGeometryPayloadDescriptorMaterialWordOffset = 27u;
static const uint kVirtualGeometryPayloadDescriptorMaterialCount = 28u;
static const uint kVirtualGeometryPayloadDescriptorLocalVertexCount = 29u;
static const uint kVirtualGeometryPayloadDescriptorLocalPrimitiveByteCount = 30u;
static const uint kVirtualGeometryPayloadDescriptorLocalRemapEncoding = 31u;
static const uint kVirtualGeometrySourceResidentPagePayload = 1u;
static const uint kVirtualGeometrySourceResidentFallbackPayload = 2u;

struct PageLocalPayloadDescriptor
{
	uint vertexWordOffset;
	uint vertexWordSize;
	uint primitiveWordOffset;
	uint primitiveByteSize;
	uint remapWordOffset;
	uint remapCount;
	uint meshletWordOffset;
	uint meshletCount;
	uint boundsWordOffset;
	uint boundsCount;
	uint decodeWordOffset;
	uint materialWordOffset;
	uint materialCount;
	uint localVertexCount;
	uint localPrimitiveByteCount;
	uint localRemapEncoding;
};

uint LoadVirtualGeometryBufferWord(uint bufferIndex, uint wordOffset, uint fallbackValue)
{
	if (bufferIndex == kInvalidVirtualGeometryBufferIndex)
	{
		return fallbackValue;
	}

	return globalBuffers[NonUniformResourceIndex(bufferIndex)][wordOffset];
}

uint LoadVirtualGeometryPagePayloadDescriptorWord(uint descriptorWord, uint fallbackValue)
{
	const uint packetOffset = SceneUniforms.virtualGeometryPacketDescriptorIndex == kInvalidVirtualGeometryBufferIndex
		? SceneUniforms.selectedVirtualClusterOffset
		: SceneUniforms.virtualGeometryPacketDescriptorIndex;
	const uint wordOffset = packetOffset * kVirtualGeometryPayloadDescriptorStrideWords + descriptorWord;
	return LoadVirtualGeometryBufferWord(
		SceneUniforms.virtualGeometryPagePayloadBufferIndex,
		wordOffset,
		fallbackValue);
}

bool IsVirtualGeometryPageLocalSource(uint payloadSource)
{
	return payloadSource == kVirtualGeometrySourceResidentPagePayload
		|| payloadSource == kVirtualGeometrySourceResidentFallbackPayload;
}

uint LoadPageLocalPayloadWord(uint wordOffset, uint fallbackValue)
{
	return LoadVirtualGeometryBufferWord(
		SceneUniforms.virtualGeometryPagePayloadBufferIndex,
		wordOffset,
		fallbackValue);
}

bool TryLoadPageLocalPayloadDescriptor(out PageLocalPayloadDescriptor descriptor)
{
	descriptor.vertexWordOffset = 0u;
	descriptor.vertexWordSize = 0u;
	descriptor.primitiveWordOffset = 0u;
	descriptor.primitiveByteSize = 0u;
	descriptor.remapWordOffset = 0u;
	descriptor.remapCount = 0u;
	descriptor.meshletWordOffset = 0u;
	descriptor.meshletCount = 0u;
	descriptor.boundsWordOffset = 0u;
	descriptor.boundsCount = 0u;
	descriptor.decodeWordOffset = 0u;
	descriptor.materialWordOffset = 0u;
	descriptor.materialCount = 0u;
	descriptor.localVertexCount = 0u;
	descriptor.localPrimitiveByteCount = 0u;
	descriptor.localRemapEncoding = 0u;
	if (SceneUniforms.virtualGeometryPagePayloadBufferIndex == kInvalidVirtualGeometryBufferIndex
		|| !IsVirtualGeometryPageLocalSource(SceneUniforms.virtualGeometryPayloadSource))
	{
		return false;
	}

	descriptor.vertexWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorVertexWordOffset, 0u);
	descriptor.vertexWordSize = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorVertexWordSize, 0u);
	descriptor.primitiveWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorPrimitiveWordOffset, 0u);
	descriptor.primitiveByteSize = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorPrimitiveByteSize, 0u);
	descriptor.remapWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorRemapWordOffset, 0u);
	descriptor.remapCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorRemapCount, 0u);
	descriptor.meshletWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorMeshletWordOffset, 0u);
	descriptor.meshletCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorMeshletCount, 0u);
	descriptor.boundsWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorBoundsWordOffset, 0u);
	descriptor.boundsCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorBoundsCount, 0u);
	descriptor.decodeWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorDecodeWordOffset, 0u);
	descriptor.materialWordOffset = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorMaterialWordOffset, 0u);
	descriptor.materialCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorMaterialCount, 0u);
	descriptor.localVertexCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorLocalVertexCount, 0u);
	descriptor.localPrimitiveByteCount = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorLocalPrimitiveByteCount, 0u);
	descriptor.localRemapEncoding = LoadVirtualGeometryPagePayloadDescriptorWord(kVirtualGeometryPayloadDescriptorLocalRemapEncoding, 0u);
	return descriptor.vertexWordSize != 0u
		&& descriptor.primitiveByteSize != 0u
		&& descriptor.remapCount != 0u
		&& descriptor.meshletCount != 0u
		&& descriptor.boundsCount >= descriptor.meshletCount
		&& descriptor.localVertexCount != 0u;
}

bool TryUsePageLocalGeometry(out PageLocalPayloadDescriptor descriptor)
{
	return TryLoadPageLocalPayloadDescriptor(descriptor);
}

uint ResolvePageLocalMeshletIndex(uint selectedMeshletOffset, uint groupMeshletOffset, uint localIndex, bool pageLocal)
{
	return pageLocal ? groupMeshletOffset + localIndex : selectedMeshletOffset + groupMeshletOffset + localIndex;
}

MeshletRecord LoadPageLocalMeshletRecord(PageLocalPayloadDescriptor descriptor, uint meshletIndex)
{
	MeshletRecord record;
	const uint baseWord = descriptor.meshletWordOffset + meshletIndex * 4u;
	record.vertexOffset = LoadPageLocalPayloadWord(baseWord + 0u, 0u);
	record.vertexCount = LoadPageLocalPayloadWord(baseWord + 1u, 0u);
	record.primitiveIndexOffset = LoadPageLocalPayloadWord(baseWord + 2u, 0u);
	record.primitiveIndexCount = LoadPageLocalPayloadWord(baseWord + 3u, 0u);
	return record;
}

MeshletBounds LoadPageLocalMeshletBounds(PageLocalPayloadDescriptor descriptor, uint meshletIndex)
{
	MeshletBounds bounds;
	const uint baseWord = descriptor.boundsWordOffset + meshletIndex * 12u;
	bounds.center = float3(
		asfloat(LoadPageLocalPayloadWord(baseWord + 0u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 1u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 2u, 0u)));
	bounds.radius = asfloat(LoadPageLocalPayloadWord(baseWord + 3u, 0u));
	bounds.coneApex = float3(
		asfloat(LoadPageLocalPayloadWord(baseWord + 4u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 5u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 6u, 0u)));
	bounds.coneCutoff = asfloat(LoadPageLocalPayloadWord(baseWord + 7u, 0u));
	bounds.coneAxis = float3(
		asfloat(LoadPageLocalPayloadWord(baseWord + 8u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 9u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 10u, 0u)));
	bounds.coneApexOffset = asfloat(LoadPageLocalPayloadWord(baseWord + 11u, 0u));
	return bounds;
}

MeshDecodeInfo LoadPageLocalDecodeInfo(PageLocalPayloadDescriptor descriptor)
{
	MeshDecodeInfo decode;
	const uint baseWord = descriptor.decodeWordOffset;
	decode.positionOffset = float4(
		asfloat(LoadPageLocalPayloadWord(baseWord + 0u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 1u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 2u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 3u, 0u)));
	decode.positionScale = float4(
		asfloat(LoadPageLocalPayloadWord(baseWord + 4u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 5u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 6u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 7u, 0u)));
	decode.uvOffsetScale = float4(
		asfloat(LoadPageLocalPayloadWord(baseWord + 8u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 9u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 10u, 0u)),
		asfloat(LoadPageLocalPayloadWord(baseWord + 11u, 0u)));
	decode.counts = uint4(
		LoadPageLocalPayloadWord(baseWord + 12u, 0u),
		LoadPageLocalPayloadWord(baseWord + 13u, 0u),
		LoadPageLocalPayloadWord(baseWord + 14u, 0u),
		LoadPageLocalPayloadWord(baseWord + 15u, 0u));
	return decode;
}

uint LoadVirtualGeometryPacketRunSeed()
{
	const uint packetSeed = LoadVirtualGeometryBufferWord(
		SceneUniforms.virtualGeometryPacketBufferIndex,
		0u,
		SceneUniforms.selectedVirtualClusterOffset);
	const uint batchSeed = LoadVirtualGeometryBufferWord(
		SceneUniforms.virtualGeometryBatchBufferIndex,
		0u,
		SceneUniforms.selectedVirtualClusterCount);
	const uint pagePayloadSeed = LoadVirtualGeometryPagePayloadDescriptorWord(
		kVirtualGeometryPayloadDescriptorSourceWord,
		SceneUniforms.virtualGeometryPayloadSource);
	const uint physicalPayloadBindingSeed = LoadVirtualGeometryPagePayloadDescriptorWord(
		kVirtualGeometryPayloadDescriptorBufferIndexWord,
		kInvalidVirtualGeometryBufferIndex)
		^ LoadVirtualGeometryPagePayloadDescriptorWord(
			kVirtualGeometryPayloadDescriptorIndexWord,
			kInvalidVirtualGeometryBufferIndex);
	// Page payload identity is consumed for packet ownership/debug validation
	// and does not alter the Hi-Z projection or accepted occlusion threshold.
	return packetSeed
		^ (batchSeed << 1u)
		^ (pagePayloadSeed << 8u)
		^ (physicalPayloadBindingSeed << 4u)
		^ SceneUniforms.virtualGeometryPagePayloadByteSize
		^ SceneUniforms.virtualGeometryPacketDescriptorIndex;
}

bool IsFrustumCulled(const float3 centerWorld, const float radiusWorld, const FrameCullingData frameData, const float epsilon)
{
	[unroll]
	for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
	{
		const float4 plane = frameData.frustumPlanes[planeIndex];
		const float signedDistance = dot(plane.xyz, centerWorld) + plane.w;
		if (signedDistance < -(radiusWorld + epsilon))
		{
			return true;
		}
	}
	return false;
}

bool IsConeBackfacing(const float3 coneApexWorld, const float3 coneAxisWorld, const float coneCutoff, const float radiusWorld, const float3 cameraPosition, const float epsilon)
{
	const float axisLengthSq = dot(coneAxisWorld, coneAxisWorld);
	if (axisLengthSq < 1e-6f || coneCutoff < -1.0f || coneCutoff > 1.0f)
	{
		return false;
	}

	const float3 axis = coneAxisWorld * rsqrt(axisLengthSq);
	const float3 cameraToApex = coneApexWorld - cameraPosition;
	const float apexDistanceSq = dot(cameraToApex, cameraToApex);
	if (apexDistanceSq < 1e-8f)
	{
		return false;
	}

	const float apexDistance = sqrt(apexDistanceSq);
	if (radiusWorld >= apexDistance)
	{
		return false;
	}

	const float3 viewDirection = cameraToApex / apexDistance;
	const float facing = dot(viewDirection, axis);
	return facing >= (coneCutoff + epsilon);
}

uint HiZViewIndexFromStatsSlot(const uint statsSlotIndex)
{
	// Stats slots are packed as frame * 2 + view, with Game at 0 and Scene at
	// 1. The Hi-Z descriptor array mirrors that view order so editor Scene/Game
	// command buffers can be recorded together without rebinding the texture.
	return min(statsSlotIndex % kHiZViewCount, kHiZViewCount - 1u);
}

uint LoadPrimitiveIndex(uint primitiveElementIndex)
{
	const uint alignedByteOffset = primitiveElementIndex & ~3u;
	const uint packed = primitiveIndices.Load(alignedByteOffset);
	const uint shift = (primitiveElementIndex & 3u) * 8u;
	return (packed >> shift) & 0xFFu;
}

float DecodeSnorm10(uint packedValue, uint bitOffset)
{
	int value = int((packedValue >> bitOffset) & 0x3FFu);
	if ((value & 0x200) != 0)
	{
		value |= ~0x3FF;
	}

	return clamp(float(value) / 511.0f, -1.0f, 1.0f);
}

bool TryLoadMeshletGlobalVertexIndex(const MeshDecodeInfo decode, const MeshletRecord record, const uint vertexIndex, out uint globalVertexIndex)
{
	globalVertexIndex = 0u;
	if (vertexIndex >= record.vertexCount)
	{
		return false;
	}

	const uint remapIndex = record.vertexOffset + vertexIndex;
	if (remapIndex < record.vertexOffset || remapIndex >= decode.counts.y)
	{
		return false;
	}

	const uint isU16 = (decode.counts.w >> 31u) & 1u;
	if (isU16 != 0u)
	{
		const uint byteOffset = remapIndex * 2u;
		const uint alignedByteOffset = byteOffset & ~3u;
		const uint packed = meshletVertexRemap.Load(alignedByteOffset);
		const uint shift = (byteOffset & 2u) * 8u;
		globalVertexIndex = (packed >> shift) & 0xFFFFu;
	}
	else
	{
		globalVertexIndex = meshletVertexRemap.Load(remapIndex * 4u);
	}

	return globalVertexIndex < decode.counts.x;
}

float3 DecodeMeshletPosition(const MeshDecodeInfo decode, const uint globalVertexIndex)
{
	const uint packedVertexByteOffset = globalVertexIndex * 16u;
	const uint packedPosition = meshletVertexBytes.Load(packedVertexByteOffset + 0u);
	const float3 qPosition = float3(
		DecodeSnorm10(packedPosition, 0u),
		DecodeSnorm10(packedPosition, 10u),
		DecodeSnorm10(packedPosition, 20u));
	return decode.positionOffset.xyz + qPosition * decode.positionScale.xyz;
}

bool TryLoadPageLocalMeshletGlobalVertexIndex(PageLocalPayloadDescriptor descriptor, const MeshDecodeInfo decode, const MeshletRecord record, const uint vertexIndex, out uint globalVertexIndex)
{
	globalVertexIndex = 0u;
	if (vertexIndex >= record.vertexCount)
	{
		return false;
	}

	const uint remapIndex = record.vertexOffset + vertexIndex;
	if (remapIndex < record.vertexOffset || remapIndex >= decode.counts.y || remapIndex >= descriptor.remapCount)
	{
		return false;
	}

	globalVertexIndex = LoadPageLocalPayloadWord(descriptor.remapWordOffset + remapIndex, 0u);
	return globalVertexIndex < decode.counts.x && globalVertexIndex < descriptor.localVertexCount;
}

float3 DecodePageLocalMeshletPosition(PageLocalPayloadDescriptor descriptor, const MeshDecodeInfo decode, const uint globalVertexIndex)
{
	const uint packedPosition = LoadPageLocalPayloadWord(descriptor.vertexWordOffset + globalVertexIndex * 4u + 0u, 0u);
	const float3 qPosition = float3(
		DecodeSnorm10(packedPosition, 0u),
		DecodeSnorm10(packedPosition, 10u),
		DecodeSnorm10(packedPosition, 20u));
	return decode.positionOffset.xyz + qPosition * decode.positionScale.xyz;
}

bool TryProjectMeshletVertex(const MeshDecodeInfo decode, const MeshletRecord record, const uint vertexIndex, out float2 uv, out float depth)
{
	uv = float2(0.0f, 0.0f);
	depth = 1.0f;

	uint globalVertexIndex = 0u;
	if (!TryLoadMeshletGlobalVertexIndex(decode, record, vertexIndex, globalVertexIndex))
	{
		return false;
	}

	const float3 localPosition = DecodeMeshletPosition(decode, globalVertexIndex);
	const float3 worldPosition = mul(SceneUniforms.model, float4(localPosition, 1.0f)).xyz;
	const float4 clipPosition = mul(SceneUniforms.viewProj, float4(worldPosition, 1.0f));
	if (clipPosition.w <= 0.0f)
	{
		return false;
	}

	const float3 ndcPosition = clipPosition.xyz / clipPosition.w;
	if (!all(isfinite(ndcPosition)))
	{
		return false;
	}

	// Projection is already Y-flipped on CPU for Vulkan, so do not flip again here.
	uv = ndcPosition.xy * 0.5f + 0.5f;
	depth = saturate(ndcPosition.z);
	return true;
}

bool TryProjectResolvedMeshletVertex(
	const MeshDecodeInfo decode,
	const MeshletRecord record,
	const uint vertexIndex,
	const PageLocalPayloadDescriptor pageDescriptor,
	const bool pageLocal,
	out float2 uv,
	out float depth)
{
	if (!pageLocal)
	{
		return TryProjectMeshletVertex(decode, record, vertexIndex, uv, depth);
	}

	uv = float2(0.0f, 0.0f);
	depth = 1.0f;
	uint globalVertexIndex = 0u;
	if (!TryLoadPageLocalMeshletGlobalVertexIndex(pageDescriptor, decode, record, vertexIndex, globalVertexIndex))
	{
		return false;
	}

	const float3 localPosition = DecodePageLocalMeshletPosition(pageDescriptor, decode, globalVertexIndex);
	const float3 worldPosition = mul(SceneUniforms.model, float4(localPosition, 1.0f)).xyz;
	const float4 clipPosition = mul(SceneUniforms.viewProj, float4(worldPosition, 1.0f));
	if (clipPosition.w <= 0.0f)
	{
		return false;
	}

	const float3 ndcPosition = clipPosition.xyz / clipPosition.w;
	if (!all(isfinite(ndcPosition)))
	{
		return false;
	}

	uv = ndcPosition.xy * 0.5f + 0.5f;
	depth = saturate(ndcPosition.z);
	return true;
}

bool TryBuildProjectedMeshletRect(const uint meshletIndex, const uint meshletCount, const MeshDecodeInfo decode, const FrameCullingData frameData, const PageLocalPayloadDescriptor pageDescriptor, const bool pageLocal, out float2 uvMin, out float2 uvMax, out float meshletClosestDepth)
{
	uvMin = float2(1.0f, 1.0f);
	uvMax = float2(0.0f, 0.0f);
	meshletClosestDepth = 1.0f;
	if (meshletIndex >= meshletCount)
	{
		return false;
	}

	MeshletRecord record = meshletRecords[meshletIndex];
	if (pageLocal)
	{
		record = LoadPageLocalMeshletRecord(pageDescriptor, meshletIndex);
	}
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	if (vertexCount == 0u || record.vertexOffset > decode.counts.y || vertexCount > (decode.counts.y - record.vertexOffset))
	{
		return false;
	}

	[loop]
	for (uint vertexIndex = 0u; vertexIndex < kMaxMeshletVertices; ++vertexIndex)
	{
		if (vertexIndex >= vertexCount)
		{
			break;
		}

		float2 projectedUv = float2(0.0f, 0.0f);
		float projectedDepth = 1.0f;
		if (!TryProjectResolvedMeshletVertex(decode, record, vertexIndex, pageDescriptor, pageLocal, projectedUv, projectedDepth))
		{
			return false;
		}

		uvMin = min(uvMin, projectedUv);
		uvMax = max(uvMax, projectedUv);
		meshletClosestDepth = min(meshletClosestDepth, projectedDepth);
	}

	// Keep edge-crossing meshlets visible. A tighter vertex rectangle is still
	// only safe when the entire projected meshlet is represented on screen.
	if (any(uvMin < 0.0f) || any(uvMax > 1.0f) || !isfinite(meshletClosestDepth))
	{
		return false;
	}

	float2 viewportSize = max(frameData.clusterDepthAndViewport.zw, float2(1.0f, 1.0f));
	// Give wall/object silhouettes a small screen-space escape hatch. Hi-Z is
	// only allowed to cull when the expanded projected meshlet footprint is
	// hidden, so peeking edges stay visible instead of disappearing early.
	const float2 uvPadding = 8.0f / viewportSize;
	uvMin = max(uvMin - uvPadding, 0.0f);
	uvMax = min(uvMax + uvPadding, 1.0f);
	if (any(uvMax < uvMin))
	{
		return false;
	}

	return true;
}

bool TrySampleHiZPoint(const uint hiZViewIndex, const uint baseWidth, const uint baseHeight, const float2 uv, out float sampledDepth)
{
	sampledDepth = 1.0f;
	if (baseWidth == 0u || baseHeight == 0u || any(uv < 0.0f) || any(uv > 1.0f))
	{
		return false;
	}

	const float2 baseSize = float2(baseWidth, baseHeight);
	const uint2 texel = min((uint2)floor(uv * baseSize), uint2(baseWidth - 1u, baseHeight - 1u));
	sampledDepth = hiZPyramids[hiZViewIndex].Load(int3(texel, 0u)).r;
	return isfinite(sampledDepth) && sampledDepth > 0.0f;
}

bool IsMeshletVertexCoverageOccluded(
	const uint meshletIndex,
	const uint meshletCount,
	const MeshDecodeInfo decode,
	const uint hiZViewIndex,
	const uint baseWidth,
	const uint baseHeight,
	const float rectExtentPixels,
	const float occlusionBias)
{
	if (meshletIndex >= meshletCount || rectExtentPixels > kMaxVertexCoverageRectExtentPixels)
	{
		return false;
	}

	const MeshletRecord record = meshletRecords[meshletIndex];
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	if (vertexCount < 3u || record.vertexOffset > decode.counts.y || vertexCount > (decode.counts.y - record.vertexOffset))
	{
		return false;
	}

	// The strict rectangle path above is fully conservative, but imported
	// meshlets often project to rectangles with empty/background pixels around
	// the actual triangles. For small far-away meshlets, require every decoded
	// vertex to be behind the current Hi-Z surface before accepting occlusion.
	// This keeps the fallback much stronger than the old center tap while still
	// preventing one unrelated background texel from forcing a fail-open draw.
	uint occludedVertexCount = 0u;
	[loop]
	for (uint vertexIndex = 0u; vertexIndex < kMaxMeshletVertices; ++vertexIndex)
	{
		if (vertexIndex >= vertexCount)
		{
			break;
		}

		float2 uv = float2(0.0f, 0.0f);
		float depth = 1.0f;
		if (!TryProjectMeshletVertex(decode, record, vertexIndex, uv, depth))
		{
			return false;
		}

		float sampledDepth = 1.0f;
		if (!TrySampleHiZPoint(hiZViewIndex, baseWidth, baseHeight, uv, sampledDepth))
		{
			return false;
		}

		if (depth > (sampledDepth + occlusionBias))
		{
			occludedVertexCount++;
		}
	}

	const uint requiredOccludedVertices = vertexCount;
	return occludedVertexCount >= requiredOccludedVertices;
}

bool IsMeshletTriangleCoverageOccluded(
	const uint meshletIndex,
	const uint meshletCount,
	const MeshDecodeInfo decode,
	const uint hiZViewIndex,
	const uint baseWidth,
	const uint baseHeight,
	const float rectExtentPixels,
	const float occlusionBias)
{
	if (meshletIndex >= meshletCount || rectExtentPixels > kMaxTriangleCoverageRectExtentPixels)
	{
		return false;
	}

	const MeshletRecord record = meshletRecords[meshletIndex];
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	const uint primitiveElementCount = min(record.primitiveIndexCount, kMaxMeshletTriangles * 3u);
	if (vertexCount < 3u || record.vertexOffset > decode.counts.y || vertexCount > (decode.counts.y - record.vertexOffset))
	{
		return false;
	}
	if (primitiveElementCount < 3u || record.primitiveIndexOffset > decode.counts.z || primitiveElementCount > (decode.counts.z - record.primitiveIndexOffset))
	{
		return false;
	}

	// Real meshlet coverage is triangle-shaped, not rectangle-shaped. Sampling
	// triangle centroids gives Hi-Z a chance to prove that the actual cluster
	// surface is hidden even when the projected bounding rectangle includes sky
	// or empty pixels around the meshlet. The high vote threshold keeps edge
	// meshlets visible unless almost all sampled surface area is behind depth.
	const uint primitiveCount = primitiveElementCount / 3u;
	const uint triangleStride = max(1u, (primitiveCount + kMaxCoverageTriangleSamples - 1u) / kMaxCoverageTriangleSamples);
	uint sampledTriangleCount = 0u;
	uint occludedTriangleCount = 0u;
	[loop]
	for (uint sampleIndex = 0u; sampleIndex < kMaxCoverageTriangleSamples; ++sampleIndex)
	{
		const uint primitiveIndex = sampleIndex * triangleStride;
		if (primitiveIndex >= primitiveCount)
		{
			break;
		}

		const uint primitiveOffset = record.primitiveIndexOffset + primitiveIndex * 3u;
		const uint index0 = LoadPrimitiveIndex(primitiveOffset + 0u);
		const uint index1 = LoadPrimitiveIndex(primitiveOffset + 1u);
		const uint index2 = LoadPrimitiveIndex(primitiveOffset + 2u);
		if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
		{
			return false;
		}

		float2 uv0 = float2(0.0f, 0.0f);
		float2 uv1 = float2(0.0f, 0.0f);
		float2 uv2 = float2(0.0f, 0.0f);
		float depth0 = 1.0f;
		float depth1 = 1.0f;
		float depth2 = 1.0f;
		if (!TryProjectMeshletVertex(decode, record, index0, uv0, depth0)
			|| !TryProjectMeshletVertex(decode, record, index1, uv1, depth1)
			|| !TryProjectMeshletVertex(decode, record, index2, uv2, depth2))
		{
			return false;
		}

		const float2 centroidUv = (uv0 + uv1 + uv2) / 3.0f;
		const float centroidDepth = (depth0 + depth1 + depth2) / 3.0f;
		float sampledDepth = 1.0f;
		if (!TrySampleHiZPoint(hiZViewIndex, baseWidth, baseHeight, centroidUv, sampledDepth))
		{
			return false;
		}

		sampledTriangleCount++;
		if (centroidDepth > (sampledDepth + occlusionBias))
		{
			occludedTriangleCount++;
		}
	}

	if (sampledTriangleCount < 4u)
	{
		return false;
	}

	const uint requiredOccludedTriangles = sampledTriangleCount;
	return occludedTriangleCount >= requiredOccludedTriangles;
}

bool IsMeshletRectCoverageMostlyOccluded(
	const uint hiZViewIndex,
	const uint baseWidth,
	const uint baseHeight,
	const float2 uvMin,
	const float2 uvMax,
	const float meshletClosestDepth,
	const float rectExtentPixels,
	const float occlusionBias)
{
	if (rectExtentPixels > kMaxRectCoverageRectExtentPixels || any(uvMax < uvMin) || !isfinite(meshletClosestDepth))
	{
		return false;
	}

	static const uint kRectCoverageGridSize = 5u;
	static const uint kRectCoverageSampleCount = kRectCoverageGridSize * kRectCoverageGridSize;
	static const uint kRequiredRectCoverageSamples = kRectCoverageSampleCount;
	uint occludedSamples = 0u;
	bool centerOccluded = false;

	// The full-rectangle path samples every texel and is maximally
	// conservative, but it fails open when a projected meshlet rectangle
	// includes a few background texels around real triangle coverage. This
	// coarse vote accepts occlusion only when the whole sample grid and the
	// center tap are behind Hi-Z. This is intentionally stricter than the older
	// "mostly covered" vote because small visible silhouettes near wall edges
	// must fail open.
	[loop]
	for (uint sampleY = 0u; sampleY < kRectCoverageGridSize; ++sampleY)
	{
		[loop]
		for (uint sampleX = 0u; sampleX < kRectCoverageGridSize; ++sampleX)
		{
			const float2 sampleT = (float2(sampleX, sampleY) + 0.5f) / float(kRectCoverageGridSize);
			const float2 sampleUv = lerp(uvMin, uvMax, sampleT);
			float sampledDepth = 1.0f;
			if (!TrySampleHiZPoint(hiZViewIndex, baseWidth, baseHeight, sampleUv, sampledDepth))
			{
				return false;
			}

			const bool sampleOccluded = meshletClosestDepth > (sampledDepth + occlusionBias);
			occludedSamples += sampleOccluded ? 1u : 0u;
			if (sampleX == 2u && sampleY == 2u)
			{
				centerOccluded = sampleOccluded;
			}
		}
	}

	return centerOccluded && occludedSamples >= min(kRequiredRectCoverageSamples, kRectCoverageSampleCount);
}

bool IsSmallMeshletCenterOccluded(
	const uint hiZViewIndex,
	const uint baseWidth,
	const uint baseHeight,
	const float2 uvMin,
	const float2 uvMax,
	const float meshletClosestDepth,
	const float rectExtentPixels,
	const float occlusionBias)
{
	if (rectExtentPixels > kMaxCenterCoverageRectExtentPixels || any(uvMax < uvMin) || !isfinite(meshletClosestDepth))
	{
		return false;
	}

	const float2 centerUv = (uvMin + uvMax) * 0.5f;
	float sampledDepth = 1.0f;
	if (!TrySampleHiZPoint(hiZViewIndex, baseWidth, baseHeight, centerUv, sampledDepth))
	{
		return false;
	}

	// This is intentionally limited to small projected meshlets. It catches
	// far-away clusters where full rectangle coverage is too strict for real
	// triangle shapes, while larger silhouette-crossing meshlets still need the
	// higher-confidence rectangle or vertex coverage paths above.
	return meshletClosestDepth > (sampledDepth + occlusionBias);
}

bool IsOccluded(const uint meshletIndex, const uint meshletCount, const MeshDecodeInfo decode, const FrameCullingData frameData, const uint hiZViewIndex, inout MeshletCullingCounters counters, const float epsilon, const PageLocalPayloadDescriptor pageDescriptor, const bool pageLocal)
{
	float2 uvMin = float2(0.0f, 0.0f);
	float2 uvMax = float2(0.0f, 0.0f);
	float meshletClosestDepth = 1.0f;
	if (!TryBuildProjectedMeshletRect(meshletIndex, meshletCount, decode, frameData, pageDescriptor, pageLocal, uvMin, uvMax, meshletClosestDepth))
	{
		counters.hiZProjectionFailed += 1u;
		return false;
	}
	counters.hiZTests += 1u;

	const float2 viewportSize = max(frameData.clusterDepthAndViewport.zw, float2(1.0f, 1.0f));
	float2 rectPixels = max((uvMax - uvMin) * viewportSize, float2(1.0f, 1.0f));
	float rectExtentPixels = max(rectPixels.x, rectPixels.y);
	if (!isfinite(rectExtentPixels))
	{
		return false;
	}

	// Query the full projected rectangle, not a fixed 3x3 pattern. We choose
	// the finest mip whose covered texel range fits in a bounded loop. Because
	// the pyramid stores max depth, sampling extra/coarser texels is conservative:
	// background or holes inside/near the rect increase furthestDepth and keep
	// the meshlet visible instead of producing a false occlusion. Keep the cap
	// explicit so Hi-Z cannot turn a large projected meshlet into thousands of
	// task-shader texture loads; when the cap forces a coarser mip, the culler
	// may reject fewer meshlets, but it still fails open instead of cutting
	// visible geometry.
	static const uint kMaxHiZRectAxisSamples = 32u;
	static const uint kMaxHiZRectTexelSamples = kMaxHiZRectAxisSamples * kMaxHiZRectAxisSamples;
	uint baseWidth = 1u;
	uint baseHeight = 1u;
	uint mipLevels = 1u;
	hiZPyramids[hiZViewIndex].GetDimensions(0u, baseWidth, baseHeight, mipLevels);
	if (baseWidth == 0u || baseHeight == 0u || mipLevels == 0u)
	{
		return false;
	}

	uint mipIndex = 0u;
	uint2 selectedMinTexel = uint2(0u, 0u);
	uint2 selectedMaxTexel = uint2(0u, 0u);
	uint selectedWidth = 1u;
	uint selectedHeight = 1u;
	const uint maxMipIndex = mipLevels - 1u;
	[loop]
	for (uint candidateMip = 0u; candidateMip <= maxMipIndex; ++candidateMip)
	{
		uint mipWidth = 1u;
		uint mipHeight = 1u;
		uint ignoredMipLevels = 1u;
		hiZPyramids[hiZViewIndex].GetDimensions(candidateMip, mipWidth, mipHeight, ignoredMipLevels);
		mipWidth = max(mipWidth, 1u);
		mipHeight = max(mipHeight, 1u);

		const float2 mipSize = float2(mipWidth, mipHeight);
		uint2 minTexel = min((uint2)floor(uvMin * mipSize), uint2(mipWidth - 1u, mipHeight - 1u));
		uint2 maxTexel = min((uint2)floor(uvMax * mipSize), uint2(mipWidth - 1u, mipHeight - 1u));
		if (any(maxTexel < minTexel))
		{
			return false;
		}

		const uint sampleWidth = maxTexel.x - minTexel.x + 1u;
		const uint sampleHeight = maxTexel.y - minTexel.y + 1u;
		const uint sampleCount = sampleWidth * sampleHeight;
		const bool fitsBoundedLoop =
			sampleWidth <= kMaxHiZRectAxisSamples
			&& sampleHeight <= kMaxHiZRectAxisSamples
			&& sampleCount <= kMaxHiZRectTexelSamples;
		if (fitsBoundedLoop || candidateMip == maxMipIndex)
		{
			mipIndex = candidateMip;
			selectedMinTexel = minTexel;
			selectedMaxTexel = maxTexel;
			selectedWidth = sampleWidth;
			selectedHeight = sampleHeight;
			break;
		}
	}

	float furthestDepth = 0.0f;
	[loop]
	for (uint sampleY = 0u; sampleY < kMaxHiZRectAxisSamples; ++sampleY)
	{
		if (sampleY >= selectedHeight)
		{
			break;
		}
		[loop]
		for (uint sampleX = 0u; sampleX < kMaxHiZRectAxisSamples; ++sampleX)
		{
			if (sampleX >= selectedWidth)
			{
				break;
			}
			const uint2 texel = selectedMinTexel + uint2(sampleX, sampleY);
			if (any(texel > selectedMaxTexel))
			{
				return false;
			}
			float sampledDepth = hiZPyramids[hiZViewIndex].Load(int3(texel, mipIndex)).r;
			if (!isfinite(sampledDepth))
			{
				return false;
			}
			furthestDepth = max(furthestDepth, sampledDepth);
		}
	}
	// Fail open when Hi-Z is invalid/uninitialized so visibility stays correct.
	if (!isfinite(furthestDepth) || furthestDepth <= 0.0f)
	{
		return false;
	}
	
	// Depth-buffer values are non-linear. A world-space culling epsilon becomes
	// far too large when applied directly to normalized device depth, especially
	// with a 0.1-1000 camera where useful occluders commonly sit above 0.995.
	// Keep the bias in depth units and cap the epsilon contribution so distant
	// walls can still occlude distant meshlets. The lower floor matters for
	// editor tests where a wall is only a few world units in front of a mesh at
	// long camera distance; that separation can be less than 1e-5 in depth.
	float occlusionBias = clamp(epsilon * 0.01f, 0.000001f, 0.00002f);
	
	// Occluded if every sampled texel in the projected meshlet rectangle is in
	// front of the nearest projected meshlet vertex.
	if (meshletClosestDepth > (furthestDepth + occlusionBias))
	{
		counters.hiZFullRectCulled += 1u;
		return true;
	}

	// Coverage-vote fallbacks are intentionally diagnostic-only for now. They
	// helped cull far meshlets with small background gaps, but they could still
	// eat visible wall-edge silhouettes. Until the shader has a stronger
	// per-triangle coverage proof, only the full expanded rectangle may cull and
	// every partial/uncertain case fails open.
	counters.hiZNoOcclusionProof += 1u;
	return false;
}

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	PageLocalPayloadDescriptor pageDescriptor;
	const bool pageLocalGeometry = TryUsePageLocalGeometry(pageDescriptor);
	MeshDecodeInfo decode = meshDecodeInfo[0];
	if (pageLocalGeometry)
	{
		// Resident page packets are self-contained. Their decode record describes
		// only the local vertex/remap/primitive/meshlet streams in the page
		// payload buffer, so task culling never has to read owner mesh buffers.
		decode = LoadPageLocalDecodeInfo(pageDescriptor);
	}
	const uint meshletCount = pageLocalGeometry
		? pageDescriptor.meshletCount
		: (decode.counts.w & 0x7FFFFFFFu);
	const uint packetRunSeed = LoadVirtualGeometryPacketRunSeed();
	const uint selectedMeshletOffset = pageLocalGeometry
		? 0u
		: min(SceneUniforms.virtualMeshletOffset + (packetRunSeed & 0u), meshletCount);
	const uint selectedMeshletCount = SceneUniforms.virtualMeshletCount == 0u
		? (meshletCount - selectedMeshletOffset)
		: min(SceneUniforms.virtualMeshletCount, meshletCount - selectedMeshletOffset);
	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frameData = frameCullingData[statsIndex];
	const float epsilon = max(SceneUniforms.cullingEpsilon, frameData.cameraPositionAndEpsilon.w);
	const float3 cameraPosition = frameData.cameraPositionAndEpsilon.xyz;
	const bool hiZPyramidAvailable = frameData.renderSettings2.w > 0.5f;
	const uint virtualGeometryDebugMode = (SceneUniforms.virtualGeometryFlags >> kVirtualGeometryDebugModeShift) & kVirtualGeometryDebugModeMask;
	const bool debugCulledMeshlets = virtualGeometryDebugMode == 4u;
	const bool coneCullingSafe = (SceneUniforms.virtualGeometryFlags & kVirtualGeometryFlagConeCullingSafe) != 0u;
	const uint hiZViewIndex = HiZViewIndexFromStatsSlot(statsIndex);

	uint visibleCount = 0u;
	MeshletCullingCounters localCounters = MakeEmptyMeshletCullingCounters();

	const bool cullingEnabled = (SceneUniforms.taskCullingEnabled != 0u)
		&& (frameData.globalTaskCullingEnabled != 0u)
		&& (frameData.taskShaderSupported != 0u);

	for (uint localIndex = 0u; localIndex < kTaskBatchSize; ++localIndex)
	{
		const uint selectedLocalIndex = groupId.x * kTaskBatchSize + localIndex;
		const uint meshletIndex = ResolvePageLocalMeshletIndex(selectedMeshletOffset, groupId.x * kTaskBatchSize, localIndex, pageLocalGeometry);
		if (selectedLocalIndex >= selectedMeshletCount || meshletIndex >= meshletCount)
		{
			break;
		}

		localCounters.meshletsTotal += 1u;

		bool culled = false;
		if (cullingEnabled)
		{
			MeshletBounds bounds = meshletBounds[meshletIndex];
			if (pageLocalGeometry)
			{
				bounds = LoadPageLocalMeshletBounds(pageDescriptor, meshletIndex);
			}
			const float3 worldCenter = mul(SceneUniforms.model, float4(bounds.center, 1.0f)).xyz;
			const float3 worldAxisX = mul((float3x3)SceneUniforms.model, float3(1.0f, 0.0f, 0.0f));
			const float3 worldAxisY = mul((float3x3)SceneUniforms.model, float3(0.0f, 1.0f, 0.0f));
			const float3 worldAxisZ = mul((float3x3)SceneUniforms.model, float3(0.0f, 0.0f, 1.0f));
			const float maxScale = max(length(worldAxisX), max(length(worldAxisY), length(worldAxisZ)));
			const float worldRadius = bounds.radius * max(maxScale, 1e-6f);

			culled = IsFrustumCulled(worldCenter, worldRadius, frameData, epsilon);
			// Cone backface culling is only armed by RenderSystem when every
			// selected cluster has cooked proof for one-sided opaque materials,
			// consistent winding, low thin-shell risk, and valid cone bounds.
			// Imported GLB/FBX package meshes currently leave this bit off until
			// per-cluster material proof exists, so risky content still fails open.
			if (!culled && coneCullingSafe)
			{
				const float3 worldConeApex = mul(SceneUniforms.model, float4(bounds.coneApex, 1.0f)).xyz;
				const float3 worldConeAxis = mul((float3x3)SceneUniforms.model, bounds.coneAxis);
				culled = IsConeBackfacing(worldConeApex, worldConeAxis, bounds.coneCutoff, worldRadius, cameraPosition, epsilon);
			}
			// Feature 030 preserves this single current-pyramid decision path:
			// no shader quality tiers are introduced while cluster batches are
			// being validated against the accepted Hi-Z baseline. Shadow passes
			// consume their light-view meshlet ranges with this gate off, because
			// camera Hi-Z cannot answer light visibility.
			if (!culled && hiZPyramidAvailable)
			{
				if (IsOccluded(meshletIndex, meshletCount, decode, frameData, hiZViewIndex, localCounters, epsilon, pageDescriptor, pageLocalGeometry))
				{
					localCounters.meshletsCulledHiZ += 1u;
					culled = true;
				}
			}
		}

		if (culled && !debugCulledMeshlets)
		{
			localCounters.meshletsCulled += 1u;
			continue;
		}
		if (culled)
		{
			// Culled debug mode deliberately keeps rejected meshlets in the
			// payload so the mesh/fragment stages can color them by the rejection
			// reason. Normal rendering still drops them above.
			localCounters.meshletsCulled += 1u;
		}

		gTaskPayload.meshletIndices[visibleCount] = meshletIndex;
		visibleCount++;
		localCounters.meshletsDispatched += 1u;
	}

	FlushMeshletCullingCounters(statsIndex, localCounters);
	DispatchMesh(visibleCount, 1u, 1u, gTaskPayload);
}
