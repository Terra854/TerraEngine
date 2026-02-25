#pragma pack_matrix(column_major)

struct GPULight
{
	float4 positionAndType;
	float4 directionAndRange;
	float4 colorAndIntensity;
	float4 coneAnglesAndArea;
	float4 shadowAndPadding;
};

struct ClusterData
{
	uint offset;
	uint count;
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

static const uint kMaxLightsPerCluster = 64u;
static const uint kCullThreadCount = 64u;
static const uint kLightTypeDirectional = 0u;
static const uint kLightTypePoint = 1u;
static const uint kLightTypeSpot = 2u;
static const uint kLightTypeAreaRect = 3u;

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> globalLights;
[[vk::binding(5, 0)]] RWStructuredBuffer<ClusterData> clusterGrid;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> lightIndices;

groupshared uint gVisibleCount;
groupshared uint gSharedIndices[kMaxLightsPerCluster];

float3 BuildClusterCenter(uint clusterIndex, uint3 gridDim, FrameCullingData frame)
{
	const uint clustersPerSlice = gridDim.x * gridDim.y;
	const uint z = clusterIndex / clustersPerSlice;
	const uint xy = clusterIndex % clustersPerSlice;
	const uint y = xy / gridDim.x;
	const uint x = xy % gridDim.x;

	const float2 uv = (float2(x, y) + 0.5) / max(float2(gridDim.xy), float2(1.0, 1.0));
	const float nearPlane = frame.clusterDepthAndViewport.x;
	const float farPlane = max(frame.clusterDepthAndViewport.y, nearPlane + 0.001);
	const float depth = lerp(nearPlane, farPlane, (float(z) + 0.5) / max(float(gridDim.z), 1.0));

	return float3(uv * 2.0 - 1.0, depth);
}

bool SphereIntersectsCluster(const GPULight light, const float3 clusterCenter)
{
	const float3 lightPos = light.positionAndType.xyz;
	const float range = max(light.directionAndRange.w, 0.001);
	const float distanceToCluster = distance(lightPos, clusterCenter);
	return distanceToCluster <= range;
}

bool ConeIntersectsCluster(const GPULight light, const float3 clusterCenter)
{
	const float3 lightPos = light.positionAndType.xyz;
	const float3 lightDir = normalize(light.directionAndRange.xyz);
	const float3 toCluster = normalize(clusterCenter - lightPos);
	const float coneOuterCos = light.coneAnglesAndArea.y;
	return dot(lightDir, toCluster) >= coneOuterCos;
}

bool AreaIntersectsCluster(const GPULight light, const float3 clusterCenter)
{
	const float3 lightPos = light.positionAndType.xyz;
	const float extent = max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
	const float range = max(light.directionAndRange.w, extent);
	return distance(lightPos, clusterCenter) <= range;
}

// Per-cluster light culling using group-shared memory for cooperative writes.
// The current intersection tests are conservative to avoid missing lights while
// keeping the data flow compatible with future tighter AABB/frustum checks.
[numthreads(kCullThreadCount, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
	const FrameCullingData frame = frameCullingData[0];
	const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
	const uint clusterCount = gridDim.x * gridDim.y * gridDim.z;
	const uint clusterIndex = groupId.x;
	if (clusterIndex >= clusterCount)
	{
		return;
	}

	if (groupThreadId.x == 0u)
	{
		gVisibleCount = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	const float3 clusterCenter = BuildClusterCenter(clusterIndex, gridDim, frame);
	const uint lightCount = frame.clusterGridAndLightCount.w;

	for (uint lightIndex = groupThreadId.x; lightIndex < lightCount; lightIndex += kCullThreadCount)
	{
		const GPULight light = globalLights[lightIndex];
		const uint lightType = (uint)(light.positionAndType.w + 0.5);

		bool intersects = false;
		if (lightType == kLightTypeDirectional)
		{
			intersects = true;
		}
		else if (lightType == kLightTypePoint)
		{
			intersects = SphereIntersectsCluster(light, clusterCenter);
		}
		else if (lightType == kLightTypeSpot)
		{
			intersects = SphereIntersectsCluster(light, clusterCenter) && ConeIntersectsCluster(light, clusterCenter);
		}
		else if (lightType == kLightTypeAreaRect)
		{
			intersects = AreaIntersectsCluster(light, clusterCenter);
		}

		if (!intersects)
		{
			continue;
		}

		uint slot = 0u;
		InterlockedAdd(gVisibleCount, 1u, slot);
		if (slot < kMaxLightsPerCluster)
		{
			gSharedIndices[slot] = lightIndex;
		}
	}
	GroupMemoryBarrierWithGroupSync();

	const uint visibleCount = min(gVisibleCount, kMaxLightsPerCluster);
	const uint outputOffset = clusterIndex * kMaxLightsPerCluster;

	if (groupThreadId.x == 0u)
	{
		ClusterData cluster;
		cluster.offset = outputOffset;
		cluster.count = visibleCount;
		clusterGrid[clusterIndex] = cluster;
	}

	if (groupThreadId.x < visibleCount)
	{
		lightIndices[outputOffset + groupThreadId.x] = gSharedIndices[groupThreadId.x];
	}
}
