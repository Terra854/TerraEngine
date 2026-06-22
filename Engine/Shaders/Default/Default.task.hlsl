#pragma pack_matrix(column_major)

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
	uint4 counts;
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
	float4 renderSettings2; // x: indirectStrength, y: aoStrength, z: aoQuality, w: hiZHistoryValid
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
[[vk::binding(10, 0)]] Texture2D<float> hiZPyramid;
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
	float4 editorTint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

static const uint kMaxStatsSlots = 16u;

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

bool IsOccluded(const float3 centerWorld, const float radiusWorld, const FrameCullingData frameData, const float epsilon)
{
	// Project bounding sphere to NDC
	float4 centerClip = mul(SceneUniforms.viewProj, float4(centerWorld, 1.0f));
	if (centerClip.w <= 0.0f) return false; // Near plane clipping

	float3 centerNdc = centerClip.xyz / centerClip.w;
	// Projection is already Y-flipped on CPU for Vulkan, so do not flip again here.
	float2 uv = centerNdc.xy * 0.5f + 0.5f;

	// Clamp UV to avoid sampling border/background
	if (any(uv < 0.0) || any(uv > 1.0)) return false;

	// Conservative radius in screen space for mip selection.
	// Use absolute projection scale because Vulkan's Y-flip makes [1][1] negative.
	float2 projectionScale = abs(float2(SceneUniforms.viewProj[0][0], SceneUniforms.viewProj[1][1]));
	float screenRadius = (radiusWorld * max(projectionScale.x, projectionScale.y)) / max(centerClip.w, 1e-6f);
	
	float2 viewportSize = frameData.clusterDepthAndViewport.zw;
	float sizeInPixels = max(1.0f, screenRadius * max(viewportSize.x, viewportSize.y));
	if (!isfinite(sizeInPixels))
	{
		return false;
	}
	
	// Choose mip level where the sphere covers roughly 2x2 texels
	float mip = max(0.0f, ceil(log2(sizeInPixels / 2.0f)));
	// Clamp to the mip range derived from the current viewport to avoid invalid samples.
	float maxMip = max(0.0f, floor(log2(max(viewportSize.x, viewportSize.y))));
	mip = min(mip, maxMip);
	
	// Sample furthest depth in the region
	float furthestDepth = hiZPyramid.SampleLevel(hiZSampler, uv, mip).r;
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
	float occlusionBias = max(epsilon * 4.0f, 0.002f);
	
	// Occluded if the nearest point on the meshlet is still deeper than occluders in the Hi-Z region.
	return meshletClosestDepth > (furthestDepth + occlusionBias);
}

[numthreads(1, 1, 1)]
void main(uint3 groupId : SV_GroupID)
{
	const MeshDecodeInfo decode = meshDecodeInfo[0];
	const uint meshletCount = decode.counts.w & 0x7FFFFFFFu;
	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frameData = frameCullingData[statsIndex];
	const float epsilon = max(SceneUniforms.cullingEpsilon, frameData.cameraPositionAndEpsilon.w);
	const float3 cameraPosition = frameData.cameraPositionAndEpsilon.xyz;

	const uint meshletBase = groupId.x * kTaskBatchSize;
	uint visibleCount = 0u;
	uint ignored = 0u;

	const bool cullingEnabled = (SceneUniforms.taskCullingEnabled != 0u)
		&& (frameData.globalTaskCullingEnabled != 0u)
		&& (frameData.taskShaderSupported != 0u);

	for (uint localIndex = 0u; localIndex < kTaskBatchSize; ++localIndex)
	{
		const uint meshletIndex = meshletBase + localIndex;
		if (meshletIndex >= meshletCount)
		{
			break;
		}

		InterlockedAdd(meshletCounters[statsIndex].meshletsTotal, 1u, ignored);

		bool culled = false;
		if (cullingEnabled)
		{
			const MeshletBounds bounds = meshletBounds[meshletIndex];
			const float3 worldCenter = mul(SceneUniforms.model, float4(bounds.center, 1.0f)).xyz;
			const float3 worldConeApex = mul(SceneUniforms.model, float4(bounds.coneApex, 1.0f)).xyz;
			const float3 worldConeAxis = mul((float3x3)SceneUniforms.model, bounds.coneAxis);
			const float3 worldAxisX = mul((float3x3)SceneUniforms.model, float3(1.0f, 0.0f, 0.0f));
			const float3 worldAxisY = mul((float3x3)SceneUniforms.model, float3(0.0f, 1.0f, 0.0f));
			const float3 worldAxisZ = mul((float3x3)SceneUniforms.model, float3(0.0f, 0.0f, 1.0f));
			const float maxScale = max(length(worldAxisX), max(length(worldAxisY), length(worldAxisZ)));
			const float worldRadius = bounds.radius * max(maxScale, 1e-6f);

			culled = IsFrustumCulled(worldCenter, worldRadius, frameData, epsilon);
			if (!culled)
			{
				culled = IsConeBackfacing(worldConeApex, worldConeAxis, bounds.coneCutoff, worldRadius, cameraPosition, epsilon);
			}
			if (!culled && frameData.renderSettings2.w > 0.5f)
			{
				if (IsOccluded(worldCenter, worldRadius, frameData, epsilon))
				{
					InterlockedAdd(meshletCounters[statsIndex].meshletsCulledHiZ, 1u, ignored);
					culled = true;
				}
			}
		}

		if (culled)
		{
			InterlockedAdd(meshletCounters[statsIndex].meshletsCulled, 1u, ignored);
			continue;
		}

		gTaskPayload.meshletIndices[visibleCount] = meshletIndex;
		visibleCount++;
		InterlockedAdd(meshletCounters[statsIndex].meshletsDispatched, 1u, ignored);
	}

	DispatchMesh(visibleCount, 1u, 1u, gTaskPayload);
}
