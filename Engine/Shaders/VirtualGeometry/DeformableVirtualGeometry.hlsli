// DeformableVirtualGeometry.hlsli - Immutable deformable-cluster shader contract.
//
// A renderer packet owns this record.  It is intentionally value-only: shader
// code receives resource indices, byte offsets, generations, and conservative
// bounds rather than an animation-instance pointer or any mutable scene state.
// This lets Game, Scene, shadow, and capture passes make the same safety choice
// for one published deformation generation.

#ifndef TERRAENGINE_DEFORMABLE_VIRTUAL_GEOMETRY_HLSLI
#define TERRAENGINE_DEFORMABLE_VIRTUAL_GEOMETRY_HLSLI

#define TE_DEFORMABLE_VIRTUAL_GEOMETRY_ABI_VERSION 1u
#define TE_DEFORMABLE_VIRTUAL_GEOMETRY_INVALID_BUFFER_INDEX 0xffffffffu
#define TE_DEFORMABLE_VIRTUAL_GEOMETRY_INVALID_CLUSTER_INDEX 0xffffffffu

// The flags record proofs, never requests.  A missing proof disables the
// corresponding rejection path and retains visible conservative coverage.
#define TE_DEFORMABLE_VG_FLAG_ACTIVE 0x001u
#define TE_DEFORMABLE_VG_FLAG_DYNAMIC_BOUNDS_VALID 0x002u
#define TE_DEFORMABLE_VG_FLAG_CONSERVATIVE_ENVELOPE_VALID 0x004u
#define TE_DEFORMABLE_VG_FLAG_FRUSTUM_CULLING_SAFE 0x008u
#define TE_DEFORMABLE_VG_FLAG_CONE_CULLING_SAFE 0x010u
#define TE_DEFORMABLE_VG_FLAG_HIZ_CULLING_SAFE 0x020u
#define TE_DEFORMABLE_VG_FLAG_FALLBACK_COVERAGE_COMPLETE 0x040u
#define TE_DEFORMABLE_VG_FLAG_FALLBACK_ACTIVE 0x080u
#define TE_DEFORMABLE_VG_FLAG_REFINEMENT_SAFE 0x100u
#define TE_DEFORMABLE_VG_FLAG_ALL 0x1ffu

// The cooked cluster influence table is compact because the mesh shader only
// needs its stable address and generation.  The skinning stream itself stays
// in the immutable mesh payload; this record names the matching palette range.
struct TeDeformableVirtualGeometryClusterRecord
{
	uint abiVersion;
	uint flags;
	uint paletteBufferIndex;
	uint paletteOffsetBytes;
	uint morphBufferIndex;
	uint morphOffsetBytes;
	uint paletteEntryCount;
	uint morphTargetCount;
	uint2 paletteGeneration;
	uint2 morphGeneration;
	uint fallbackClusterId;
	uint refinementParentClusterId;
	uint refinementFirstChildClusterId;
	uint refinementChildCount;
	// xyz is the conservative bind/deformation-envelope centre and w is radius.
	float4 conservativeEnvelope;
	// Dynamic bounds are used only when the published generation proves them
	// finite and valid.  Otherwise the conservative envelope remains visible.
	float4 dynamicBoundsMin;
	float4 dynamicBoundsMax;
	// x = baked geometric error, y = deformation error allowance, z = observed
	// screen-space error, w = caller-selected refinement threshold in pixels.
	float4 refinementAndError;
};

bool TeDeformableVirtualGeometryFinite(float value)
{
	return !isnan(value) && !isinf(value);
}

bool TeDeformableVirtualGeometryFinite3(float3 value)
{
	return TeDeformableVirtualGeometryFinite(value.x)
		&& TeDeformableVirtualGeometryFinite(value.y)
		&& TeDeformableVirtualGeometryFinite(value.z);
}

bool TeDeformableVirtualGeometryHasFlag(
	TeDeformableVirtualGeometryClusterRecord cluster,
	uint flag)
{
	return (cluster.flags & flag) != 0u;
}

bool TeDeformableVirtualGeometryHasUsableDynamicBounds(
	TeDeformableVirtualGeometryClusterRecord cluster)
{
	return TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_DYNAMIC_BOUNDS_VALID)
		&& TeDeformableVirtualGeometryFinite3(cluster.dynamicBoundsMin.xyz)
		&& TeDeformableVirtualGeometryFinite3(cluster.dynamicBoundsMax.xyz)
		&& all(cluster.dynamicBoundsMin.xyz <= cluster.dynamicBoundsMax.xyz);
}

bool TeDeformableVirtualGeometryHasUsableEnvelope(
	TeDeformableVirtualGeometryClusterRecord cluster)
{
	return TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_CONSERVATIVE_ENVELOPE_VALID)
		&& TeDeformableVirtualGeometryFinite3(cluster.conservativeEnvelope.xyz)
		&& TeDeformableVirtualGeometryFinite(cluster.conservativeEnvelope.w)
		&& cluster.conservativeEnvelope.w >= 0.0f;
}

// Resolves the only bounds source allowed to reject a deformable cluster.  A
// false result means the caller must draw fallback coverage instead of culling.
bool TeResolveDeformableVirtualGeometryBounds(
	TeDeformableVirtualGeometryClusterRecord cluster,
	out float3 boundsMin,
	out float3 boundsMax)
{
	boundsMin = float3(0.0f, 0.0f, 0.0f);
	boundsMax = float3(0.0f, 0.0f, 0.0f);
	if (TeDeformableVirtualGeometryHasUsableDynamicBounds(cluster))
	{
		boundsMin = cluster.dynamicBoundsMin.xyz;
		boundsMax = cluster.dynamicBoundsMax.xyz;
		return true;
	}
	if (!TeDeformableVirtualGeometryHasUsableEnvelope(cluster))
	{
		return false;
	}
	const float radius = cluster.conservativeEnvelope.w;
	boundsMin = cluster.conservativeEnvelope.xyz - radius.xxx;
	boundsMax = cluster.conservativeEnvelope.xyz + radius.xxx;
	return true;
}

// Cone and Hi-Z rejection amplify small bound errors, so they require both a
// complete fallback chain and a cooked proof for this exact deformation record.
bool TeDeformableVirtualGeometryCanConeCull(TeDeformableVirtualGeometryClusterRecord cluster)
{
	return TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_FALLBACK_COVERAGE_COMPLETE)
		&& TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_CONE_CULLING_SAFE)
		&& (TeDeformableVirtualGeometryHasUsableDynamicBounds(cluster)
			|| TeDeformableVirtualGeometryHasUsableEnvelope(cluster));
}

bool TeDeformableVirtualGeometryCanHiZCull(TeDeformableVirtualGeometryClusterRecord cluster)
{
	return TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_FALLBACK_COVERAGE_COMPLETE)
		&& TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_HIZ_CULLING_SAFE)
		&& TeDeformableVirtualGeometryHasUsableDynamicBounds(cluster);
}

bool TeDeformableVirtualGeometryRequiresVisibleFallback(TeDeformableVirtualGeometryClusterRecord cluster)
{
	float3 boundsMin;
	float3 boundsMax;
	return !TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_ACTIVE)
		|| !TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_FALLBACK_COVERAGE_COMPLETE)
		|| !TeResolveDeformableVirtualGeometryBounds(cluster, boundsMin, boundsMax);
}

// Refinement is optional quality work.  Invalid error metadata therefore keeps
// the current covered cluster rather than risking an uncovered child cut.
bool TeDeformableVirtualGeometryShouldRefine(TeDeformableVirtualGeometryClusterRecord cluster)
{
	const float bakedError = cluster.refinementAndError.x;
	const float deformationError = cluster.refinementAndError.y;
	const float projectedError = cluster.refinementAndError.z;
	const float threshold = cluster.refinementAndError.w;
	return TeDeformableVirtualGeometryHasFlag(cluster, TE_DEFORMABLE_VG_FLAG_REFINEMENT_SAFE)
		&& cluster.refinementChildCount != 0u
		&& TeDeformableVirtualGeometryFinite(bakedError)
		&& TeDeformableVirtualGeometryFinite(deformationError)
		&& TeDeformableVirtualGeometryFinite(projectedError)
		&& TeDeformableVirtualGeometryFinite(threshold)
		&& bakedError >= 0.0f
		&& deformationError >= 0.0f
		&& threshold >= 0.0f
		&& projectedError + deformationError > threshold;
}

#endif // TERRAENGINE_DEFORMABLE_VIRTUAL_GEOMETRY_HLSLI
