#pragma once
#include <cstdint>
namespace TerraEngine::Renderer
{
	enum class VirtualGeometryAggregateKind : std::uint8_t
	{
		Assembly,
		Foliage,
		Curve,
		Voxel,
		Impostor,
		Terrain,
		MicroInstance,
		DistantRepresentation,
		Fallback
	};
	struct VirtualGeometryAggregateRepresentation
	{
		VirtualGeometryAggregateKind kind = VirtualGeometryAggregateKind::Fallback;
		std::uint64_t stableId = 0;
		float canonicalError = 0;
		bool completeFallbackCoverage = false;
		// Value-only provenance used by cooker tests and transition planning.
		std::uint32_t sourceElementCount = 0;
		float coverageRatio = 0.0f;
		std::uint64_t fallbackStableId = 0;
		std::uint64_t sharedStorageIdentity = 0;
		std::uint64_t generationIdentity = 0;
		bool productionValidated = false;
		bool atomicTransitionReady = false;
		[[nodiscard]] bool Valid() const noexcept
		{
			return stableId != 0 && canonicalError >= 0.0f && coverageRatio >= 0.0f && coverageRatio <= 1.0f
				&& completeFallbackCoverage && (fallbackStableId == 0 || fallbackStableId == stableId)
				&& sharedStorageIdentity != 0 && generationIdentity != 0
				&& productionValidated && atomicTransitionReady;
		}
	};
	struct VirtualGeometryAggregateTransition
	{
		std::uint64_t fromId = 0, toId = 0;
		float error = 0;
		bool failOpen = false;
	};

	[[nodiscard]] inline VirtualGeometryAggregateTransition BuildVirtualGeometryAggregateTransition(
		const VirtualGeometryAggregateRepresentation& from,
		const VirtualGeometryAggregateRepresentation& to) noexcept
	{
		const bool valid = from.Valid() && to.Valid() && to.canonicalError >= from.canonicalError;
		return { from.stableId, to.stableId, to.canonicalError, !valid };
	}
} // namespace TerraEngine::Renderer
