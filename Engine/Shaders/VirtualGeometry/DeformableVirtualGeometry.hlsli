// DeformableVirtualGeometry.hlsli - fail-open dynamic proof helpers.

#ifndef TERRAENGINE_DEFORMABLE_VIRTUAL_GEOMETRY_HLSLI
#define TERRAENGINE_DEFORMABLE_VIRTUAL_GEOMETRY_HLSLI

#include "AnimatedVirtualGeometryAbi.hlsli"

bool TeAnimatedBoundsProofMatches(
	TeAnimatedDynamicBounds bounds,
	uint expectedIdentity,
	uint expectedWorkIndex)
{
	const uint required = TE_ANIMATED_GEOMETRY_VALID_CURRENT
		| TE_ANIMATED_GEOMETRY_VALID_BOUNDS
		| TE_ANIMATED_GEOMETRY_VALID_HIZ_PROOF;
	return bounds.identity.x == expectedIdentity
		&& bounds.identity.y == expectedWorkIndex
		&& bounds.identity.z == TE_ANIMATED_GEOMETRY_ABI_VERSION
		&& (bounds.identity.w & required) == required
		&& all(!isnan(bounds.minimum.xyz))
		&& all(!isnan(bounds.maximum.xyz))
		&& all(bounds.minimum.xyz <= bounds.maximum.xyz);
}

bool TeAnimatedConeProofMatches(TeAnimatedDynamicBounds bounds)
{
	return (bounds.identity.w & TE_ANIMATED_GEOMETRY_VALID_CONE_PROOF) != 0u;
}

#endif
