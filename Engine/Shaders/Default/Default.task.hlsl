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
	// RenderSystem expands one draw per selected cluster, so this draw normally
	// references one record and one page-backed meshlet range.
	uint selectedVirtualClusterOffset; // First selected cluster record in the per-view packet.
	uint selectedVirtualClusterCount; // Number of selected clusters for this draw; normally one for sparse VG dispatch.
	uint selectedVirtualPageNeedOffset; // First missing-page request record for this draw.
	uint selectedVirtualPageNeedCount; // Number of desired pages that fell back to resident coverage.
	float4 editorTint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

static const uint kMaxStatsSlots = 16u;
static const uint kHiZViewCount = 2u;
static const uint kVirtualGeometryDebugModeShift = 8u;
static const uint kVirtualGeometryDebugModeMask = 0xFFu;

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

bool TryBuildProjectedMeshletRect(const uint meshletIndex, const uint meshletCount, const MeshDecodeInfo decode, const FrameCullingData frameData, out float2 uvMin, out float2 uvMax, out float meshletClosestDepth)
{
	uvMin = float2(1.0f, 1.0f);
	uvMax = float2(0.0f, 0.0f);
	meshletClosestDepth = 1.0f;
	if (meshletIndex >= meshletCount)
	{
		return false;
	}

	const MeshletRecord record = meshletRecords[meshletIndex];
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
		if (!TryProjectMeshletVertex(decode, record, vertexIndex, projectedUv, projectedDepth))
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
	const float2 uvPadding = 2.0f / viewportSize;
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

	if (occludedVertexCount == vertexCount)
	{
		return true;
	}

	const uint requiredOccludedVertices = max(3u, (vertexCount * 92u + 99u) / 100u);
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

	const uint requiredOccludedTriangles = max(1u, (sampledTriangleCount * 92u + 99u) / 100u);
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
	static const uint kRequiredRectCoverageSamples = 22u;
	uint occludedSamples = 0u;
	bool centerOccluded = false;

	// The full-rectangle path samples every texel and is maximally
	// conservative, but it fails open when a projected meshlet rectangle
	// includes a few background texels around real triangle coverage. This
	// coarse vote accepts occlusion only when nearly the whole rectangle and the
	// center tap are behind Hi-Z, which improves wall coverage while preserving
	// a visible-margin escape hatch.
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

bool IsOccluded(const uint meshletIndex, const uint meshletCount, const MeshDecodeInfo decode, const FrameCullingData frameData, const uint hiZViewIndex, const uint statsIndex, const float epsilon)
{
	uint ignored = 0u;
	float2 uvMin = float2(0.0f, 0.0f);
	float2 uvMax = float2(0.0f, 0.0f);
	float meshletClosestDepth = 1.0f;
	if (!TryBuildProjectedMeshletRect(meshletIndex, meshletCount, decode, frameData, uvMin, uvMax, meshletClosestDepth))
	{
		InterlockedAdd(meshletCounters[statsIndex].hiZProjectionFailed, 1u, ignored);
		return false;
	}
	InterlockedAdd(meshletCounters[statsIndex].hiZTests, 1u, ignored);

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
	// the meshlet visible instead of producing a false occlusion. Keep the total
	// texel budget matches the full per-axis cap so solid occluders do not get
	// collapsed into overly coarse mips during common wall-in-front tests. That
	// keeps a farther/smaller wall useful as an occluder while still proving
	// occlusion with the whole projected meshlet sphere, not a risky center tap.
	static const uint kMaxHiZRectAxisSamples = 64u;
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
		InterlockedAdd(meshletCounters[statsIndex].hiZFullRectCulled, 1u, ignored);
		return true;
	}

	if (IsMeshletRectCoverageMostlyOccluded(hiZViewIndex, baseWidth, baseHeight, uvMin, uvMax, meshletClosestDepth, rectExtentPixels, occlusionBias))
	{
		InterlockedAdd(meshletCounters[statsIndex].hiZRectCoverageCulled, 1u, ignored);
		return true;
	}

	if (IsSmallMeshletCenterOccluded(hiZViewIndex, baseWidth, baseHeight, uvMin, uvMax, meshletClosestDepth, rectExtentPixels, occlusionBias))
	{
		InterlockedAdd(meshletCounters[statsIndex].hiZCenterCulled, 1u, ignored);
		return true;
	}

	if (IsMeshletTriangleCoverageOccluded(meshletIndex, meshletCount, decode, hiZViewIndex, baseWidth, baseHeight, rectExtentPixels, occlusionBias))
	{
		InterlockedAdd(meshletCounters[statsIndex].hiZVertexCoverageCulled, 1u, ignored);
		return true;
	}

	if (IsMeshletVertexCoverageOccluded(meshletIndex, meshletCount, decode, hiZViewIndex, baseWidth, baseHeight, rectExtentPixels, occlusionBias))
	{
		InterlockedAdd(meshletCounters[statsIndex].hiZVertexCoverageCulled, 1u, ignored);
		return true;
	}

	// Larger meshlets still fail open here. A meshlet can have its center behind
	// a wall while a visible edge or silhouette remains outside the occluder, so
	// the center fallback above is capped to small projected rectangles only.
	InterlockedAdd(meshletCounters[statsIndex].hiZNoOcclusionProof, 1u, ignored);
	return false;
}

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	const MeshDecodeInfo decode = meshDecodeInfo[0];
	const uint meshletCount = decode.counts.w & 0x7FFFFFFFu;
	const uint selectedMeshletOffset = min(SceneUniforms.virtualMeshletOffset, meshletCount);
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
	const uint hiZViewIndex = HiZViewIndexFromStatsSlot(statsIndex);

	const uint meshletBase = selectedMeshletOffset + groupId.x * kTaskBatchSize;
	uint visibleCount = 0u;
	uint ignored = 0u;

	const bool cullingEnabled = (SceneUniforms.taskCullingEnabled != 0u)
		&& (frameData.globalTaskCullingEnabled != 0u)
		&& (frameData.taskShaderSupported != 0u);

	for (uint localIndex = 0u; localIndex < kTaskBatchSize; ++localIndex)
	{
		const uint selectedLocalIndex = groupId.x * kTaskBatchSize + localIndex;
		const uint meshletIndex = meshletBase + localIndex;
		if (selectedLocalIndex >= selectedMeshletCount || meshletIndex >= meshletCount)
		{
			break;
		}

		InterlockedAdd(meshletCounters[statsIndex].meshletsTotal, 1u, ignored);

		bool culled = false;
		if (cullingEnabled)
		{
			const MeshletBounds bounds = meshletBounds[meshletIndex];
			const float3 worldCenter = mul(SceneUniforms.model, float4(bounds.center, 1.0f)).xyz;
			const float3 worldAxisX = mul((float3x3)SceneUniforms.model, float3(1.0f, 0.0f, 0.0f));
			const float3 worldAxisY = mul((float3x3)SceneUniforms.model, float3(0.0f, 1.0f, 0.0f));
			const float3 worldAxisZ = mul((float3x3)SceneUniforms.model, float3(0.0f, 0.0f, 1.0f));
			const float maxScale = max(length(worldAxisX), max(length(worldAxisY), length(worldAxisZ)));
			const float worldRadius = bounds.radius * max(maxScale, 1e-6f);

			culled = IsFrustumCulled(worldCenter, worldRadius, frameData, epsilon);
			// Cone backface culling is intentionally disabled for the always-on
			// virtual-geometry path until material sidedness, thin-shell hints,
			// and imported winding validation are available in this shader. A
			// valid meshoptimizer cone can still be unsafe for GLB/FBX content
			// that expects two-sided or mixed-winding visibility, so frustum and
			// Hi-Z remain the only meshlet rejection paths here.
			if (!culled && hiZPyramidAvailable)
			{
				if (IsOccluded(meshletIndex, meshletCount, decode, frameData, hiZViewIndex, statsIndex, epsilon))
				{
					InterlockedAdd(meshletCounters[statsIndex].meshletsCulledHiZ, 1u, ignored);
					culled = true;
				}
			}
		}

		if (culled && !debugCulledMeshlets)
		{
			InterlockedAdd(meshletCounters[statsIndex].meshletsCulled, 1u, ignored);
			continue;
		}
		if (culled)
		{
			// Culled debug mode deliberately keeps rejected meshlets in the
			// payload so the mesh/fragment stages can color them by the rejection
			// reason. Normal rendering still drops them above.
			InterlockedAdd(meshletCounters[statsIndex].meshletsCulled, 1u, ignored);
		}

		gTaskPayload.meshletIndices[visibleCount] = meshletIndex;
		visibleCount++;
		InterlockedAdd(meshletCounters[statsIndex].meshletsDispatched, 1u, ignored);
	}

	DispatchMesh(visibleCount, 1u, 1u, gTaskPayload);
}
