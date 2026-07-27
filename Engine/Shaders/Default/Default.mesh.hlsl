#pragma pack_matrix(column_major)
#include "DefaultDeformation.hlsli"

// Meshlet table emitted by the mesh compiler and mirrored from TerraEngineMeshFile.h.
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
	float4 positionOffset; // xyz = position offset
	float4 positionScale;  // xyz = position scale
	float4 uvOffsetScale;  // xy = uv offset, zw = uv scale
	uint4 counts;          // x = vertex count, y = remap count, z = primitive count, w = meshlet count
};

struct MeshletBounds
{
	float3 center;
	float radius;
	float3 coneApex;
	float coneCutoff;
	float3 coneAxis;
	float coneApexOffset;
};

struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	column_major float4x4 viewMatrix;
	column_major float4x4 invProjection;
	uint4 renderSettings0;
	uint4 renderSettings1;
	float4 renderSettings2;
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};

// Task stage writes a compacted visible meshlet index list into payload.
static const uint kTaskBatchSize = 32u;
struct TaskPayload
{
	uint meshletIndices[kTaskBatchSize];
	// The task shader resolves page ownership before culling and compaction.
	// Preserve that descriptor id with each visible meshlet so one draw can emit
	// a run of independently uploaded page packets safely.
	uint packetDescriptorIndices[kTaskBatchSize];
};

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(0, 1)]] ByteAddressBuffer meshletVertexBytes;
[[vk::binding(1, 1)]] ByteAddressBuffer primitiveIndices;
[[vk::binding(2, 1)]] StructuredBuffer<MeshletRecord> meshletRecords;
[[vk::binding(3, 1)]] ByteAddressBuffer meshletVertexRemap;
[[vk::binding(4, 1)]] StructuredBuffer<MeshDecodeInfo> meshDecodeInfo;
[[vk::binding(5, 1)]] StructuredBuffer<MeshletBounds> meshletBounds;
// The mesh compiler packs four uint8 local palette joints followed by four
// normalized uint8 weights for every deformable owner-mesh vertex.
[[vk::binding(6, 1)]] ByteAddressBuffer meshSkinningBytes;
// Target-major float4 position/normal/tangent deltas.  The descriptor is a
// safe sentinel for static meshes, which never enter the GPU bridge path.
[[vk::binding(7, 1)]] ByteAddressBuffer meshMorphDeltaBytes;
[[vk::binding(9, 0)]] StructuredBuffer<uint> globalBuffers[];

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
	uint virtualMeshletOffset;
	uint virtualMeshletCount;
	uint selectedVirtualLod;
	uint virtualGeometryFlags;
	uint selectedVirtualClusterOffset;
	uint selectedVirtualClusterCount;
	uint selectedVirtualPageNeedOffset;
	uint selectedVirtualPageNeedCount;
	uint virtualGeometryPacketBufferIndex;
	uint virtualGeometryBatchBufferIndex;
	uint virtualGeometryPagePayloadBufferIndex;
	uint virtualGeometryPayloadSource;
	uint virtualGeometryEmittedSource;
	uint virtualGeometryPagePayloadByteOffset;
	uint virtualGeometryPagePayloadByteSize;
	uint virtualGeometryPacketDescriptorIndex;
	float4 editorTint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	nointerpolation uint meshletIndex : TEXCOORD2;
	nointerpolation float meshletBoundsRadius : TEXCOORD3;
	nointerpolation uint meshletCullFlags : TEXCOORD4;
	nointerpolation uint virtualGeometryDebugData : TEXCOORD5;
};

static const uint kMaxMeshletVertices = 64;
static const uint kMaxMeshletTriangles = 124;
static const uint kMaxStatsSlots = 16u;
// Task stages may run without rejecting any work, but their payload still owns
// the exact child meshlet index for each emitted mesh-stage group.
static const uint kVirtualGeometryFlagTaskPayloadValid = 0x40u;
static const uint kMeshletCullFlagFrustum = 1u;
static const uint kMeshletCullFlagCone = 2u;
static const uint kMeshletCullFlagUnavailable = 4u;
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

bool TryLoadGpuAnimationGraphicsRecord(out DefaultGpuAnimationGraphicsRecord record)
{
	record = (DefaultGpuAnimationGraphicsRecord)0;
	if (SceneUniforms.meshDataIndex == kInvalidAnimationGraphicsBufferIndex
		|| SceneUniforms.textureIndex == kInvalidAnimationGraphicsBufferIndex)
	{
		return false;
	}

	uint elementCount;
	uint stride;
	globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)].GetDimensions(elementCount, stride);
	if (elementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint byteCount = elementCount * 4u;
	if (SceneUniforms.textureIndex > (0xFFFFFFFFu - kAnimationGraphicsRecordBytes) / kAnimationGraphicsRecordBytes)
	{
		return false;
	}
	const uint byteOffset = SceneUniforms.textureIndex * kAnimationGraphicsRecordBytes;
	if (byteOffset > byteCount || kAnimationGraphicsRecordBytes > byteCount - byteOffset)
	{
		return false;
	}

	const uint wordOffset = byteOffset >> 2u;
	record.planGeneration = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 0u];
	record.rendererGeneration = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 1u];
	record.workPacketIndex = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 2u];
	record.generationRecordIndex = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 3u];
	record.paletteBufferIndex = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 4u];
	record.morphWeightBufferIndex = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 5u];
	record.paletteOffsetBytes = uint2(
		globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 6u],
		globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 7u]);
	record.morphWeightOffsetBytes = uint2(
		globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 8u],
		globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 9u]);
	record.jointCount = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 10u];
	record.morphCount = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 11u];
	record.paletteEntryCount = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 12u];
	record.workFlags = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 13u];
	record.stageMask = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 14u];
	record.validMarker = globalBuffers[NonUniformResourceIndex(SceneUniforms.meshDataIndex)][wordOffset + 15u];
	return DefaultGpuAnimationRecordHeaderIsValid(record, SceneUniforms.textureIndex);
}

bool TryReadGpuMorphWeight(DefaultGpuAnimationGraphicsRecord record, uint targetIndex, out float weight)
{
	weight = 0.0f;
	if (targetIndex >= record.morphCount
		|| record.morphWeightBufferIndex == kInvalidAnimationGraphicsBufferIndex
		|| record.morphWeightOffsetBytes.y != 0u
		|| targetIndex > (0xFFFFFFFFu - record.morphWeightOffsetBytes.x) / 4u)
	{
		return false;
	}

	uint elementCount;
	uint stride;
	globalBuffers[NonUniformResourceIndex(record.morphWeightBufferIndex)].GetDimensions(elementCount, stride);
	if (elementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint byteCount = elementCount * 4u;
	const uint byteOffset = record.morphWeightOffsetBytes.x + targetIndex * 4u;
	if (byteOffset > byteCount || 4u > byteCount - byteOffset)
	{
		return false;
	}
	weight = asfloat(globalBuffers[NonUniformResourceIndex(record.morphWeightBufferIndex)][byteOffset >> 2u]);
	return DefaultGpuAnimationFinite(weight);
}

bool TryApplyGpuMorphs(
	DefaultGpuAnimationGraphicsRecord record,
	MeshDecodeInfo decode,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	if (record.morphCount == 0u)
	{
		return true;
	}
	uint deltaByteCount;
	meshMorphDeltaBytes.GetDimensions(deltaByteCount);
	float3 morphedPosition = position;
	float3 morphedNormal = normal;
	float3 morphedTangent = tangent;
	for (uint targetIndex = 0u; targetIndex < record.morphCount; ++targetIndex)
	{
		float weight;
		if (!TryReadGpuMorphWeight(record, targetIndex, weight))
		{
			return false;
		}
		if (weight == 0.0f)
		{
			continue;
		}
		if (decode.counts.x == 0u || targetIndex > (0xFFFFFFFFu - globalVertexIndex) / decode.counts.x)
		{
			return false;
		}
		const uint deltaIndex = targetIndex * decode.counts.x + globalVertexIndex;
		if (deltaIndex > (0xFFFFFFFFu - kAnimationMorphDeltaBytes) / kAnimationMorphDeltaBytes)
		{
			return false;
		}
		const uint byteOffset = deltaIndex * kAnimationMorphDeltaBytes;
		if (byteOffset > deltaByteCount || kAnimationMorphDeltaBytes > deltaByteCount - byteOffset)
		{
			return false;
		}
		const float3 positionDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 0u)).xyz;
		const float3 normalDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 16u)).xyz;
		const float3 tangentDelta = asfloat(meshMorphDeltaBytes.Load4(byteOffset + 32u)).xyz;
		if (!DefaultGpuAnimationFinite3(positionDelta)
			|| !DefaultGpuAnimationFinite3(normalDelta)
			|| !DefaultGpuAnimationFinite3(tangentDelta))
		{
			return false;
		}
		morphedPosition += positionDelta * weight;
		morphedNormal += normalDelta * weight;
		morphedTangent += tangentDelta * weight;
	}
	if (!DefaultGpuAnimationFinite3(morphedPosition)
		|| !DefaultGpuAnimationFinite3(morphedNormal)
		|| !DefaultGpuAnimationFinite3(morphedTangent))
	{
		return false;
	}
	position = morphedPosition;
	normal = morphedNormal;
	tangent = morphedTangent;
	return true;
}

bool TryApplyGpuSkinning(
	DefaultGpuAnimationGraphicsRecord record,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	uint skinningByteCount;
	meshSkinningBytes.GetDimensions(skinningByteCount);
	if (globalVertexIndex > (0xFFFFFFFFu - 8u) / 8u)
	{
		return false;
	}
	const uint skinningOffset = globalVertexIndex * 8u;
	if (skinningOffset > skinningByteCount || 8u > skinningByteCount - skinningOffset)
	{
		return false;
	}
	const uint packedJoints = meshSkinningBytes.Load(skinningOffset);
	const uint packedWeights = meshSkinningBytes.Load(skinningOffset + 4u);
	const uint weightSum = (packedWeights & 0xFFu)
		+ ((packedWeights >> 8u) & 0xFFu)
		+ ((packedWeights >> 16u) & 0xFFu)
		+ ((packedWeights >> 24u) & 0xFFu);
	// All-zero weights encode an intentional identity transform. Morphs have
	// already been applied, so this keeps unskinned morph-only vertices valid.
	if (weightSum == 0u)
	{
		return true;
	}
	if (record.paletteBufferIndex == kInvalidAnimationGraphicsBufferIndex
		|| record.paletteOffsetBytes.y != 0u
		|| record.jointCount == 0u
		|| record.paletteEntryCount == 0u)
	{
		return false;
	}

	uint paletteElementCount;
	uint paletteStride;
	globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)].GetDimensions(paletteElementCount, paletteStride);
	if (paletteElementCount > 0x3FFFFFFFu)
	{
		return false;
	}
	const uint paletteByteCount = paletteElementCount * 4u;
	float3 skinnedPosition = float3(0.0f, 0.0f, 0.0f);
	float3 skinnedNormal = float3(0.0f, 0.0f, 0.0f);
	float3 skinnedTangent = float3(0.0f, 0.0f, 0.0f);
	for (uint influence = 0u; influence < 4u; ++influence)
	{
		const uint weightByte = (packedWeights >> (influence * 8u)) & 0xFFu;
		if (weightByte == 0u)
		{
			continue;
		}
		const uint jointIndex = (packedJoints >> (influence * 8u)) & 0xFFu;
		// TDEF stores palette-local indices, not skeleton-joint indices. A sparse
		// cooked palette may contain fewer entries than the complete skeleton; the
		// palette compaction stage has already mapped each entry to its source joint.
		if (jointIndex >= record.paletteEntryCount
			|| jointIndex > (0xFFFFFFFFu - record.paletteOffsetBytes.x) / kAnimationPaletteAffineBytes)
		{
			return false;
		}
		const uint paletteOffset = record.paletteOffsetBytes.x + jointIndex * kAnimationPaletteAffineBytes;
		if (paletteOffset > paletteByteCount || kAnimationPaletteAffineBytes > paletteByteCount - paletteOffset)
		{
			return false;
		}
		const uint paletteWordOffset = paletteOffset >> 2u;
		const float4 row0 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 0u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 1u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 2u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 3u]));
		const float4 row1 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 4u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 5u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 6u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 7u]));
		const float4 row2 = asfloat(uint4(
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 8u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 9u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 10u],
			globalBuffers[NonUniformResourceIndex(record.paletteBufferIndex)][paletteWordOffset + 11u]));
		if (!DefaultGpuAnimationFinite3(row0.xyz) || !DefaultGpuAnimationFinite3(row1.xyz) || !DefaultGpuAnimationFinite3(row2.xyz)
			|| !DefaultGpuAnimationFinite(row0.w) || !DefaultGpuAnimationFinite(row1.w) || !DefaultGpuAnimationFinite(row2.w))
		{
			return false;
		}
		const float weight = float(weightByte) / float(weightSum);
		skinnedPosition += float3(dot(row0.xyz, position) + row0.w, dot(row1.xyz, position) + row1.w, dot(row2.xyz, position) + row2.w) * weight;
		skinnedNormal += float3(dot(row0.xyz, normal), dot(row1.xyz, normal), dot(row2.xyz, normal)) * weight;
		skinnedTangent += float3(dot(row0.xyz, tangent), dot(row1.xyz, tangent), dot(row2.xyz, tangent)) * weight;
	}
	if (!DefaultGpuAnimationFinite3(skinnedPosition)
		|| !DefaultGpuAnimationFinite3(skinnedNormal)
		|| !DefaultGpuAnimationFinite3(skinnedTangent)
		|| dot(skinnedNormal, skinnedNormal) <= 1e-12f
		|| dot(skinnedTangent, skinnedTangent) <= 1e-12f)
	{
		return false;
	}
	position = skinnedPosition;
	normal = normalize(skinnedNormal);
	tangent = normalize(skinnedTangent);
	return true;
}

void ApplyGpuDeformationIfValid(
	DefaultGpuAnimationGraphicsRecord record,
	MeshDecodeInfo decode,
	uint globalVertexIndex,
	inout float3 position,
	inout float3 normal,
	inout float3 tangent)
{
	// Work on local copies so one malformed morph/palette range atomically falls
	// back to the cooked bind pose instead of mixing deformed and static data.
	float3 deformedPosition = position;
	float3 deformedNormal = normal;
	float3 deformedTangent = tangent;
	if (TryApplyGpuMorphs(record, decode, globalVertexIndex, deformedPosition, deformedNormal, deformedTangent)
		&& TryApplyGpuSkinning(record, globalVertexIndex, deformedPosition, deformedNormal, deformedTangent))
	{
		position = deformedPosition;
		normal = deformedNormal;
		tangent = deformedTangent;
	}
}

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

uint LoadVirtualGeometryPagePayloadDescriptorWordAt(uint packetDescriptorIndex, uint descriptorWord, uint fallbackValue)
{
	const uint wordOffset = packetDescriptorIndex * kVirtualGeometryPayloadDescriptorStrideWords + descriptorWord;
	return LoadVirtualGeometryBufferWord(
		SceneUniforms.virtualGeometryPagePayloadBufferIndex,
		wordOffset,
		fallbackValue);
}

uint LoadVirtualGeometryPagePayloadDescriptorWord(uint descriptorWord, uint fallbackValue)
{
	const uint packetDescriptorIndex = SceneUniforms.virtualGeometryPacketDescriptorIndex == kInvalidVirtualGeometryBufferIndex
		? SceneUniforms.selectedVirtualClusterOffset
		: SceneUniforms.virtualGeometryPacketDescriptorIndex;
	return LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, descriptorWord, fallbackValue);
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

bool TryLoadPageLocalPayloadDescriptorAt(uint packetDescriptorIndex, out PageLocalPayloadDescriptor descriptor)
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

	descriptor.vertexWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorVertexWordOffset, 0u);
	descriptor.vertexWordSize = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorVertexWordSize, 0u);
	descriptor.primitiveWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorPrimitiveWordOffset, 0u);
	descriptor.primitiveByteSize = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorPrimitiveByteSize, 0u);
	descriptor.remapWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorRemapWordOffset, 0u);
	descriptor.remapCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorRemapCount, 0u);
	descriptor.meshletWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorMeshletWordOffset, 0u);
	descriptor.meshletCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorMeshletCount, 0u);
	descriptor.boundsWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorBoundsWordOffset, 0u);
	descriptor.boundsCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorBoundsCount, 0u);
	descriptor.decodeWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorDecodeWordOffset, 0u);
	descriptor.materialWordOffset = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorMaterialWordOffset, 0u);
	descriptor.materialCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorMaterialCount, 0u);
	descriptor.localVertexCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorLocalVertexCount, 0u);
	descriptor.localPrimitiveByteCount = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorLocalPrimitiveByteCount, 0u);
	descriptor.localRemapEncoding = LoadVirtualGeometryPagePayloadDescriptorWordAt(packetDescriptorIndex, kVirtualGeometryPayloadDescriptorLocalRemapEncoding, 0u);
	return descriptor.vertexWordSize != 0u
		&& descriptor.primitiveByteSize != 0u
		&& descriptor.remapCount != 0u
		&& descriptor.meshletCount != 0u
		&& descriptor.boundsCount >= descriptor.meshletCount
		&& descriptor.localVertexCount != 0u;
}

bool TryLoadPageLocalPayloadDescriptor(out PageLocalPayloadDescriptor descriptor)
{
	const uint packetDescriptorIndex = SceneUniforms.virtualGeometryPacketDescriptorIndex == kInvalidVirtualGeometryBufferIndex
		? SceneUniforms.selectedVirtualClusterOffset
		: SceneUniforms.virtualGeometryPacketDescriptorIndex;
	return TryLoadPageLocalPayloadDescriptorAt(packetDescriptorIndex, descriptor);
}

bool TryUsePageLocalGeometry(out PageLocalPayloadDescriptor descriptor)
{
	return TryLoadPageLocalPayloadDescriptor(descriptor);
}

uint LoadPageLocalPackedVertexWord(PageLocalPayloadDescriptor descriptor, uint localVertexIndex, uint vertexWordIndex)
{
	const uint wordIndex = localVertexIndex * 4u + vertexWordIndex;
	if (vertexWordIndex >= 4u || localVertexIndex >= descriptor.localVertexCount || wordIndex >= descriptor.vertexWordSize)
	{
		return 0u;
	}

	return LoadPageLocalPayloadWord(descriptor.vertexWordOffset + wordIndex, 0u);
}

uint LoadPageLocalPrimitiveIndex(PageLocalPayloadDescriptor descriptor, uint primitiveElementIndex)
{
	if (primitiveElementIndex >= descriptor.primitiveByteSize)
	{
		return 0u;
	}

	const uint packed = LoadPageLocalPayloadWord(descriptor.primitiveWordOffset + (primitiveElementIndex >> 2u), 0u);
	const uint shift = (primitiveElementIndex & 3u) * 8u;
	return (packed >> shift) & 0xFFu;
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

uint ResolveVirtualGeometryPacketRunDebugId()
{
	// Fold page-source identity into the visualizer seed so page/fallback
	// transitions are visible without changing the accepted meshlet decode path.
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
	return packetSeed
		^ (batchSeed << 1u)
		^ (pagePayloadSeed << 8u)
		^ (physicalPayloadBindingSeed << 4u)
		^ (SceneUniforms.virtualGeometryEmittedSource << 12u)
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

float MaxModelScale()
{
	const float3 worldAxisX = mul((float3x3)SceneUniforms.model, float3(1.0f, 0.0f, 0.0f));
	const float3 worldAxisY = mul((float3x3)SceneUniforms.model, float3(0.0f, 1.0f, 0.0f));
	const float3 worldAxisZ = mul((float3x3)SceneUniforms.model, float3(0.0f, 0.0f, 1.0f));
	return max(length(worldAxisX), max(length(worldAxisY), length(worldAxisZ)));
}

void BuildMeshletDebugData(uint meshletIndex, uint meshletCount, out float boundsRadius, out uint cullFlags)
{
	boundsRadius = 0.0f;
	cullFlags = 0u;
	if (meshletIndex >= meshletCount)
	{
		cullFlags = kMeshletCullFlagUnavailable;
		return;
	}

	const MeshletBounds bounds = meshletBounds[meshletIndex];
	const float maxScale = max(MaxModelScale(), 1e-6f);
	boundsRadius = bounds.radius * maxScale;
	if (!isfinite(boundsRadius) || boundsRadius < 0.0f)
	{
		cullFlags = kMeshletCullFlagUnavailable;
		return;
	}

	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frameData = frameCullingData[statsIndex];
	if (SceneUniforms.taskCullingEnabled == 0u || frameData.globalTaskCullingEnabled == 0u || frameData.taskShaderSupported == 0u)
	{
		return;
	}

	const float epsilon = max(SceneUniforms.cullingEpsilon, frameData.cameraPositionAndEpsilon.w);
	const float3 worldCenter = mul(SceneUniforms.model, float4(bounds.center, 1.0f)).xyz;
	if (IsFrustumCulled(worldCenter, boundsRadius, frameData, epsilon))
	{
		cullFlags |= kMeshletCullFlagFrustum;
		return;
	}

	// The task shader no longer rejects meshlets through cone backface culling
	// because imported GLB/FBX meshes do not yet carry enough sidedness and
	// winding validation metadata to make cone rejection conservative. Keep the
	// debug reason aligned with the runtime path so Culling does not mark
	// meshlets as cone-culled when they would actually be submitted.
}

void BuildPageLocalMeshletDebugData(PageLocalPayloadDescriptor descriptor, uint meshletIndex, uint meshletCount, out float boundsRadius, out uint cullFlags)
{
	boundsRadius = 0.0f;
	cullFlags = 0u;
	if (meshletIndex >= meshletCount)
	{
		cullFlags = kMeshletCullFlagUnavailable;
		return;
	}

	const MeshletBounds bounds = LoadPageLocalMeshletBounds(descriptor, meshletIndex);
	const float maxScale = max(MaxModelScale(), 1e-6f);
	boundsRadius = bounds.radius * maxScale;
	if (!isfinite(boundsRadius) || boundsRadius < 0.0f)
	{
		cullFlags = kMeshletCullFlagUnavailable;
		return;
	}

	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frameData = frameCullingData[statsIndex];
	if (SceneUniforms.taskCullingEnabled == 0u || frameData.globalTaskCullingEnabled == 0u || frameData.taskShaderSupported == 0u)
	{
		return;
	}

	const float epsilon = max(SceneUniforms.cullingEpsilon, frameData.cameraPositionAndEpsilon.w);
	const float3 worldCenter = mul(SceneUniforms.model, float4(bounds.center, 1.0f)).xyz;
	if (IsFrustumCulled(worldCenter, boundsRadius, frameData, epsilon))
	{
		cullFlags |= kMeshletCullFlagFrustum;
	}
}

float DecodeSnorm10(uint packedValue, uint bitOffset)
{
	int value = int((packedValue >> bitOffset) & 0x3FFu);
	if ((value & 0x200) != 0)
	{
		value |= ~0x3FF;
	}

	float normalized = float(value) / 511.0f;
	return clamp(normalized, -1.0f, 1.0f);
}

float DecodeUnorm16(uint packedValue, uint bitOffset)
{
	const uint value = (packedValue >> bitOffset) & 0xFFFFu;
	return float(value) / 65535.0f;
}

float3 DecodeOct32Normal(uint packedValue)
{
	float2 f = float2(
		float((int(packedValue) << 16) >> 16) / 32767.0,
		float(int(packedValue) >> 16) / 32767.0
	);

	float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.x += (n.x >= 0.0) ? -t : t;
	n.y += (n.y >= 0.0) ? -t : t;
	return normalize(n);
}

float4 DecodeOct32Tangent(uint packedValue)
{
	return float4(DecodeOct32Normal(packedValue), 1.0);
}

float3 TransformDirection(float3 localDirection)
{
	return normalize(mul((float3x3)SceneUniforms.model, localDirection));
}

float3 TransformNormal(float3 localNormal)
{
	const float3 worldAxisX = mul((float3x3)SceneUniforms.model, float3(1.0f, 0.0f, 0.0f));
	const float3 worldAxisY = mul((float3x3)SceneUniforms.model, float3(0.0f, 1.0f, 0.0f));
	const float3 worldAxisZ = mul((float3x3)SceneUniforms.model, float3(0.0f, 0.0f, 1.0f));

	// Normals use inverse scale while positions and tangents use forward scale.
	// This keeps lighting correct when an object has non-uniform scale without
	// spending push-constant space on a separate normal matrix.
	const float3 normalAxisX = worldAxisX / max(dot(worldAxisX, worldAxisX), 1e-8f);
	const float3 normalAxisY = worldAxisY / max(dot(worldAxisY, worldAxisY), 1e-8f);
	const float3 normalAxisZ = worldAxisZ / max(dot(worldAxisZ, worldAxisZ), 1e-8f);
	return normalize(localNormal.x * normalAxisX + localNormal.y * normalAxisY + localNormal.z * normalAxisZ);
}

uint LoadPrimitiveIndex(uint primitiveElementIndex)
{
	const uint alignedByteOffset = primitiveElementIndex & ~3u;
	const uint packed = primitiveIndices.Load(alignedByteOffset);
	const uint shift = (primitiveElementIndex & 3u) * 8u;
	return (packed >> shift) & 0xFFu;
}

void EmitMeshlet(
	uint meshletIndex,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const MeshDecodeInfo decode = meshDecodeInfo[0];
	DefaultGpuAnimationGraphicsRecord animationRecord;
	const bool gpuDeformationAvailable = TryLoadGpuAnimationGraphicsRecord(animationRecord);
	const uint meshletCount = decode.counts.w & 0x7FFFFFFFu;
	const uint isU16 = (decode.counts.w >> 31u) & 1u;

	if (meshletIndex >= meshletCount)
	{
		SetMeshOutputCounts(0, 0);
		return;
	}

	const MeshletRecord record = meshletRecords[meshletIndex];
	float meshletBoundsRadius = 0.0f;
	uint meshletCullFlags = 0u;
	BuildMeshletDebugData(meshletIndex, meshletCount, meshletBoundsRadius, meshletCullFlags);

	// Defensive clamps keep malformed payloads from writing outside shader-declared arrays.
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	const uint primitiveCount = min(record.primitiveIndexCount / 3u, kMaxMeshletTriangles);
	SetMeshOutputCounts(vertexCount, primitiveCount);

	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const uint remapIndex = record.vertexOffset + vertexIndex;
		uint globalVertexIndex = 0u;
		if (remapIndex < decode.counts.y)
		{
			if (isU16 != 0)
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
		}
		if (globalVertexIndex >= decode.counts.x)
		{
			globalVertexIndex = 0u;
		}

		const uint packedVertexByteOffset = globalVertexIndex * 16u;
		const uint packedPosition = meshletVertexBytes.Load(packedVertexByteOffset + 0u);
		const uint packedNormal   = meshletVertexBytes.Load(packedVertexByteOffset + 4u);
		const uint packedTangent  = meshletVertexBytes.Load(packedVertexByteOffset + 8u);
		const uint packedUv       = meshletVertexBytes.Load(packedVertexByteOffset + 12u);

		const float3 qPosition = float3(
			DecodeSnorm10(packedPosition, 0u),
			DecodeSnorm10(packedPosition, 10u),
			DecodeSnorm10(packedPosition, 20u));
		const float3 position = decode.positionOffset.xyz + qPosition * decode.positionScale.xyz;
		
		// Decode the spare packed-position bits as a bitangent sign. Older mesh
		// payloads can leave this field as zero, and the previous raw signed
		// decode could emit 0 or -2. The fragment shader expects only a handedness
		// sign, so publish a stable +1/-1 contract for all payload variants.
		const int rawTangentSign = (int(packedPosition >> 30) << 30) >> 30;
		const float tangentW = rawTangentSign < 0 ? -1.0f : 1.0f;
		
		const float3 normal = DecodeOct32Normal(packedNormal);
		const float3 tangent = DecodeOct32Normal(packedTangent);
		float3 deformedPosition = position;
		float3 deformedNormal = normal;
		float3 deformedTangent = tangent;
		if (gpuDeformationAvailable)
		{
			// Morph targets are accumulated before the four-weight palette blend,
			// so positions and their lighting directions share one deformation.
			ApplyGpuDeformationIfValid(
				animationRecord,
				decode,
				globalVertexIndex,
				deformedPosition,
				deformedNormal,
				deformedTangent);
		}
		
		const float2 qUv = float2(
			DecodeUnorm16(packedUv, 0u),
			DecodeUnorm16(packedUv, 16u));
		const float2 uv = decode.uvOffsetScale.xy + qUv * decode.uvOffsetScale.zw;

		const float4 worldPosition = mul(SceneUniforms.model, float4(deformedPosition, 1.0f));

		PSIn outVertex;
		outVertex.position = mul(SceneUniforms.viewProj, worldPosition);
		outVertex.uv = uv;
		outVertex.worldPosition = worldPosition.xyz;
		outVertex.normal = TransformNormal(deformedNormal);
		outVertex.tangent = float4(TransformDirection(deformedTangent), tangentW);
		// The fragment visualizer uses the true meshlet index so the Meshlet Id
		// mode shows selected LOD density instead of an unrelated world-space hash.
		outVertex.meshletIndex = meshletIndex;
		outVertex.meshletBoundsRadius = meshletBoundsRadius;
		outVertex.meshletCullFlags = meshletCullFlags;
		// Pack the per-draw adaptive state into a single interpolator. Batched
		// virtual geometry draws keep the selected packet count and offset here
		// so fragment visualizers can identify packet-run ownership.
		const uint packetRunDebugId = ResolveVirtualGeometryPacketRunDebugId();
		outVertex.virtualGeometryDebugData =
			(SceneUniforms.selectedVirtualLod & 0xFFu) |
			((SceneUniforms.selectedVirtualClusterCount & 0xFFu) << 8u) |
			((SceneUniforms.selectedVirtualPageNeedCount & 0xFFu) << 16u) |
			((packetRunDebugId & 0xFFu) << 24u);
		outputVertices[vertexIndex] = outVertex;
	}

	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const uint primitiveOffset = record.primitiveIndexOffset + primitiveIndex * 3u;
		const uint index0 = LoadPrimitiveIndex(primitiveOffset + 0u);
		const uint index1 = LoadPrimitiveIndex(primitiveOffset + 1u);
		const uint index2 = LoadPrimitiveIndex(primitiveOffset + 2u);

		// Invalid primitive references collapse to a degenerate triangle instead of OOB.
		if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
		{
			outputTriangles[primitiveIndex] = uint3(0, 0, 0);
			continue;
		}

		outputTriangles[primitiveIndex] = uint3(index0, index1, index2);
	}
}

void EmitPageLocalMeshlet(
	PageLocalPayloadDescriptor descriptor,
	uint meshletIndex,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const MeshDecodeInfo decode = LoadPageLocalDecodeInfo(descriptor);
	const uint meshletCount = descriptor.meshletCount;
	if (meshletIndex >= meshletCount)
	{
		SetMeshOutputCounts(0, 0);
		return;
	}

	const MeshletRecord record = LoadPageLocalMeshletRecord(descriptor, meshletIndex);
	float meshletBoundsRadius = 0.0f;
	uint meshletCullFlags = 0u;
	BuildPageLocalMeshletDebugData(descriptor, meshletIndex, meshletCount, meshletBoundsRadius, meshletCullFlags);

	// Page-local records are already rebased by the Vulkan upload planner. These
	// clamps are still kept here because malformed or stale page payloads should
	// fail closed into degenerate output instead of indexing unrelated pages.
	const uint vertexCount = min(record.vertexCount, kMaxMeshletVertices);
	const uint primitiveCount = min(record.primitiveIndexCount / 3u, kMaxMeshletTriangles);
	SetMeshOutputCounts(vertexCount, primitiveCount);

	for (uint vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		const uint remapIndex = record.vertexOffset + vertexIndex;
		uint localVertexIndex = 0u;
		if (remapIndex < descriptor.remapCount && remapIndex < decode.counts.y)
		{
			localVertexIndex = LoadPageLocalPayloadWord(descriptor.remapWordOffset + remapIndex, 0u);
		}
		if (localVertexIndex >= descriptor.localVertexCount || localVertexIndex >= decode.counts.x)
		{
			localVertexIndex = 0u;
		}

		const uint packedPosition = LoadPageLocalPackedVertexWord(descriptor, localVertexIndex, 0u);
		const uint packedNormal = LoadPageLocalPackedVertexWord(descriptor, localVertexIndex, 1u);
		const uint packedTangent = LoadPageLocalPackedVertexWord(descriptor, localVertexIndex, 2u);
		const uint packedUv = LoadPageLocalPackedVertexWord(descriptor, localVertexIndex, 3u);

		const float3 qPosition = float3(
			DecodeSnorm10(packedPosition, 0u),
			DecodeSnorm10(packedPosition, 10u),
			DecodeSnorm10(packedPosition, 20u));
		const float3 position = decode.positionOffset.xyz + qPosition * decode.positionScale.xyz;
		const int rawTangentSign = (int(packedPosition >> 30) << 30) >> 30;
		const float tangentW = rawTangentSign < 0 ? -1.0f : 1.0f;
		const float3 normal = DecodeOct32Normal(packedNormal);
		const float3 tangent = DecodeOct32Normal(packedTangent);
		const float2 qUv = float2(
			DecodeUnorm16(packedUv, 0u),
			DecodeUnorm16(packedUv, 16u));
		const float2 uv = decode.uvOffsetScale.xy + qUv * decode.uvOffsetScale.zw;

		const float4 worldPosition = mul(SceneUniforms.model, float4(position, 1.0f));

		PSIn outVertex;
		outVertex.position = mul(SceneUniforms.viewProj, worldPosition);
		outVertex.uv = uv;
		outVertex.worldPosition = worldPosition.xyz;
		outVertex.normal = TransformNormal(normal);
		outVertex.tangent = float4(TransformDirection(tangent), tangentW);
		outVertex.meshletIndex = meshletIndex;
		outVertex.meshletBoundsRadius = meshletBoundsRadius;
		outVertex.meshletCullFlags = meshletCullFlags;
		const uint packetRunDebugId = ResolveVirtualGeometryPacketRunDebugId();
		outVertex.virtualGeometryDebugData =
			(SceneUniforms.selectedVirtualLod & 0xFFu) |
			((SceneUniforms.selectedVirtualClusterCount & 0xFFu) << 8u) |
			((SceneUniforms.selectedVirtualPageNeedCount & 0xFFu) << 16u) |
			((packetRunDebugId & 0xFFu) << 24u);
		outputVertices[vertexIndex] = outVertex;
	}

	for (uint primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex)
	{
		const uint primitiveOffset = record.primitiveIndexOffset + primitiveIndex * 3u;
		const uint index0 = LoadPageLocalPrimitiveIndex(descriptor, primitiveOffset + 0u);
		const uint index1 = LoadPageLocalPrimitiveIndex(descriptor, primitiveOffset + 1u);
		const uint index2 = LoadPageLocalPrimitiveIndex(descriptor, primitiveOffset + 2u);

		if (index0 >= vertexCount || index1 >= vertexCount || index2 >= vertexCount)
		{
			outputTriangles[primitiveIndex] = uint3(0, 0, 0);
			continue;
		}

		outputTriangles[primitiveIndex] = uint3(index0, index1, index2);
	}
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(
	in payload TaskPayload taskPayload,
	uint3 groupId : SV_GroupID,
	out vertices PSIn outputVertices[kMaxMeshletVertices],
	out indices uint3 outputTriangles[kMaxMeshletTriangles])
{
	const uint visibleIndex = groupId.x;
	const uint packetDescriptorIndex = taskPayload.packetDescriptorIndices[visibleIndex];
	DefaultGpuAnimationGraphicsRecord animationRecord;
	const bool gpuDeformationAvailable = TryLoadGpuAnimationGraphicsRecord(animationRecord);
	PageLocalPayloadDescriptor pageDescriptor;
	const bool pageLocalGeometry = IsVirtualGeometryPageLocalSource(SceneUniforms.virtualGeometryPayloadSource)
		// Page packets do not yet carry their matching skin/morph streams. Use
		// the owner payload (or its CPU-selected fallback) for GPU-deformed work
		// rather than rendering a visually plausible but undeformed detail page.
		&& !gpuDeformationAvailable
		&& TryLoadPageLocalPayloadDescriptorAt(packetDescriptorIndex, pageDescriptor);
	// Choose the payload source before reading decode metadata. Resident page
	// packets are self-contained and must not require a valid full-owner decode
	// buffer merely because the shader also supports legacy owner-mesh draws.
	MeshDecodeInfo decode;
	if (pageLocalGeometry)
	{
		decode = LoadPageLocalDecodeInfo(pageDescriptor);
	}
	else
	{
		decode = meshDecodeInfo[0];
	}
	const uint meshletCount = pageLocalGeometry
		? pageDescriptor.meshletCount
		: (decode.counts.w & 0x7FFFFFFFu);
	const uint selectedMeshletOffset = pageLocalGeometry
		? 0u
		: min(SceneUniforms.virtualMeshletOffset, meshletCount);
	const uint selectedMeshletCount = SceneUniforms.virtualMeshletCount == 0u
		? (meshletCount - selectedMeshletOffset)
		: min(SceneUniforms.virtualMeshletCount, meshletCount - selectedMeshletOffset);
	const bool taskPayloadValid =
		(SceneUniforms.virtualGeometryFlags & kVirtualGeometryFlagTaskPayloadValid) != 0u;
	const uint meshletIndex = taskPayloadValid
		? taskPayload.meshletIndices[visibleIndex]
		: selectedMeshletOffset + visibleIndex;
	if (pageLocalGeometry)
	{
		// Page-local indices are always resolved by the task stage. This remains
		// true when culling is disabled because task compaction still establishes
		// the packet descriptor and local meshlet pair for the mesh stage.
		// Validate the resolved packet-local index, not visibleIndex. visibleIndex
		// is only the 0-31 output slot within this task group and can exceed a later
		// packet's meshlet count even when its resolved local index is valid.
		const uint pageLocalMeshletIndex = taskPayload.meshletIndices[visibleIndex];
		if (pageLocalMeshletIndex >= pageDescriptor.meshletCount)
		{
			SetMeshOutputCounts(0, 0);
			return;
		}
		EmitPageLocalMeshlet(pageDescriptor, pageLocalMeshletIndex, outputVertices, outputTriangles);
		return;
	}

	// Owner-mesh draws still use one continuous selected range, so their task
	// output slot must stay within the draw's selected meshlet count.
	if (visibleIndex >= selectedMeshletCount)
	{
		SetMeshOutputCounts(0, 0);
		return;
	}

	EmitMeshlet(meshletIndex, outputVertices, outputTriangles);
}
