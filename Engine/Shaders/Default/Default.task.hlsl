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
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint reserved0;
	uint reserved1;
};

struct MeshletCullingCounters
{
	uint meshletsTotal;
	uint meshletsDispatched;
	uint meshletsCulled;
	uint reserved;
};

// Task stage compacts visible meshlets for each task-group chunk.
static const uint kTaskBatchSize = 32u;
struct TaskPayload
{
	uint meshletIndices[kTaskBatchSize];
};

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(3, 0)]] RWStructuredBuffer<MeshletCullingCounters> meshletCounters;
[[vk::binding(4, 1)]] StructuredBuffer<MeshDecodeInfo> meshDecodeInfo;
[[vk::binding(5, 1)]] StructuredBuffer<MeshletBounds> meshletBounds;

groupshared TaskPayload gTaskPayload;

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint textureIndex;
	uint materialIndex;
	uint taskCullingEnabled;
	uint statsSlotIndex;
	float cullingEpsilon;
	float padding0;
	float padding1;
	float padding2;
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

	// Large meshlets should stay conservative to avoid silhouette popping.
	const float apexDistance = sqrt(apexDistanceSq);
	if (radiusWorld >= apexDistance)
	{
		return false;
	}

	// meshoptimizer perspective rejection uses normalize(cone_apex - camera_position).
	const float3 viewDirection = cameraToApex / apexDistance;
	const float facing = dot(viewDirection, axis);
	return facing >= (coneCutoff + epsilon);
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

	// Each task group scans a fixed-size meshlet chunk and emits only visible ones.
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
