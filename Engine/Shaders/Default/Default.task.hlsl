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
};

// Task stage compacts visible meshlets for each task-group chunk.
static const uint kTaskBatchSize = 32u;
struct TaskPayload
{
	uint meshletIndices[kTaskBatchSize];
};

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(3, 0)]] RWStructuredBuffer<MeshletCullingCounters> meshletCounters;
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

bool TryGetHiZProjectionScale(const FrameCullingData frameData, out float2 projectionScale)
{
	projectionScale = float2(0.0f, 0.0f);

	// A pure projection matrix has stable X/Y scale on its diagonal. The old
	// path read the view-projection diagonal, which changes as the camera
	// rotates and can inflate or shrink the Hi-Z rectangle until occlusion
	// appears sticky. The inverse projection is already uploaded for clustered
	// lighting, so invert its diagonal back to projection scale here.
	const float2 inverseProjectionDiagonal = float2(frameData.invProjection[0][0], frameData.invProjection[1][1]);
	if (!all(isfinite(inverseProjectionDiagonal)) || any(abs(inverseProjectionDiagonal) <= 1e-6f))
	{
		return false;
	}

	projectionScale = rcp(abs(inverseProjectionDiagonal));
	return all(isfinite(projectionScale));
}

bool IsOccluded(const float3 centerWorld, const float radiusWorld, const FrameCullingData frameData, const uint hiZViewIndex, const float epsilon)
{
	// Project the meshlet sphere into screen space. Hi-Z must stay conservative:
	// if the sphere crosses the view edge or produces invalid math, draw it.
	float4 centerClip = mul(SceneUniforms.viewProj, float4(centerWorld, 1.0f));
	if (centerClip.w <= 0.0f) return false; // Near plane clipping

	float3 centerNdc = centerClip.xyz / centerClip.w;
	// Projection is already Y-flipped on CPU for Vulkan, so do not flip again here.
	float2 uvCenter = centerNdc.xy * 0.5f + 0.5f;
	if (any(uvCenter < 0.0f) || any(uvCenter > 1.0f)) return false;

	// Estimate a projected screen rectangle for the sphere. The rectangle is
	// padded below so tiny math differences, diagonal camera angles, and mip
	// quantization fail open instead of clipping a visible edge.
	float2 projectionScale = float2(0.0f, 0.0f);
	if (!TryGetHiZProjectionScale(frameData, projectionScale))
	{
		return false;
	}
	float2 uvRadius = 0.5f * (radiusWorld * projectionScale) / max(centerClip.w, 1e-6f);
	if (!all(isfinite(uvRadius)))
	{
		return false;
	}

	float2 viewportSize = max(frameData.clusterDepthAndViewport.zw, float2(1.0f, 1.0f));
	const float2 uvPadding = 2.0f / viewportSize;
	float2 uvMin = max(uvCenter - uvRadius - uvPadding, 0.0f);
	float2 uvMax = min(uvCenter + uvRadius + uvPadding, 1.0f);
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
	// texel budget separate from the per-axis cap so solid occluders do not get
	// collapsed into overly coarse mips during common wall-in-front tests.
	static const uint kMaxHiZRectTexelSamples = 1024u;
	static const uint kMaxHiZRectAxisSamples = 64u;
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
	
	// Project the point on the bounding sphere closest to the camera instead
	// of subtracting a linear radius from clip depth. That keeps the test
	// conservative under perspective and prevents meshlets from hiding
	// themselves when the previous frame's Hi-Z pyramid contains the same mesh.
	float3 cameraToCenter = centerWorld - frameData.cameraPositionAndEpsilon.xyz;
	float cameraDistanceSq = dot(cameraToCenter, cameraToCenter);
	if (cameraDistanceSq <= radiusWorld * radiusWorld)
	{
		return false;
	}
	float3 closestWorld = centerWorld - cameraToCenter * (radiusWorld * rsqrt(max(cameraDistanceSq, 1e-12f)));
	float4 closestClip = mul(SceneUniforms.viewProj, float4(closestWorld, 1.0f));
	if (closestClip.w <= 0.0f)
	{
		return false;
	}
	float meshletClosestDepth = saturate(closestClip.z / closestClip.w);
	// Depth-buffer values are non-linear. A world-space culling epsilon becomes
	// far too large when applied directly to normalized device depth, especially
	// with a 0.1-1000 camera where useful occluders commonly sit above 0.995.
	// Keep the bias in depth units and cap the epsilon contribution so distant
	// walls can still occlude distant meshlets without creating edge flicker.
	float occlusionBias = clamp(epsilon * 0.05f, 0.00001f, 0.0001f);
	
	// Occluded if the nearest point on the meshlet is still deeper than occluders in the Hi-Z region.
	if (meshletClosestDepth > (furthestDepth + occlusionBias))
	{
		return true;
	}

	// Do not fall back to center-only samples. A meshlet can have its center
	// behind a wall while a visible edge or silhouette remains outside the
	// occluder, so anything weaker than the full-bounds test can pop geometry.
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
				if (IsOccluded(worldCenter, worldRadius, frameData, hiZViewIndex, epsilon))
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
