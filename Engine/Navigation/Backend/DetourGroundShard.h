// DetourGroundShard.h - immutable provider-neutral ground shard generations.

#pragma once

#include "Navigation/Backend/RecastGroundBuilder.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	struct GroundShardKey {
		// Keep canonical shard identity exact; Detour receives only bounded local
		// coordinates after the owning shard has been selected.
		std::int64_t x = 0, z = 0;
		std::int32_t verticalBand = 0;
		std::uint64_t profileBuildKey = 0U;
		[[nodiscard]] constexpr bool operator==(const GroundShardKey&) const noexcept = default;
	};
	struct GroundShardReference {
		GroundShardKey shard{};
		NavigationGeneration generation = 0U;
		[[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0U; }
	};
	struct DetourGroundShardBuildInput {
		GroundShardKey shard{};
		NavigationGeneration expectedGeneration = 0U;
		std::vector<GroundTileBlob> tiles;
		NavigationBudget* budget = nullptr;
		std::function<bool()> cancellationRequested;
		std::size_t maximumBytes = 64U * 1024U * 1024U;
		// Recast validates this profile-derived minimum before a polygon reaches the
		// shard. Retaining it makes seam certificates explicit without exposing a
		// provider polygon or guessing clearance from a tile's byte length.
		float minimumClearanceMetres = 0.01F;
	};
	// This pack is a separately charged immutable owner for the full Recast proof.
	// A dynamic async packet may outlive the Detour shard that originally exposed
	// it, so copying only a vector of bytes would otherwise lose its budget lease.
	struct RetainedGroundTilePack {
		std::vector<GroundTileBlob> tiles;
		std::shared_ptr<NavigationBudgetReservation> budgetReservation;
	};
	struct DetourGroundShardGeneration {
		GroundShardReference reference{};
		std::vector<GroundTileKey> tileKeys;
		std::vector<std::vector<std::byte>> ownedTileBytes;
		// Keep the complete, validated Recast inputs beside the immutable Detour
		// generation. A later dynamic worker may reuse only blobs proven outside every
		// copied receipt halo; raw ownedTileBytes alone do not retain empty-tile,
		// checksum, provenance, or budget evidence.
		std::shared_ptr<const RetainedGroundTilePack> retainedTiles;
		// Opaque lifetime only: Detour objects never become a public API type.
		std::shared_ptr<void> providerState;
		std::shared_ptr<NavigationBudgetReservation> budgetReservation;
		float minimumClearanceMetres = 0.01F;
	};
	struct DetourGroundShardBuildResult {
		bool accepted = false, cancelled = false;
		std::string reason;
		std::shared_ptr<const DetourGroundShardGeneration> generation;
	};
	// This provider-neutral boundary keeps Detour references and allocations
	// private while requiring both endpoints to resolve on one immutable shard.
	// Points are expressed in that shard's local 256m/128m float frame, so public
	// canonical world addresses never lose their large-world precision at this API.
	struct DetourGroundRouteQuery {
		NavigationGeometryVector3 start{};
		NavigationGeometryVector3 destination{};
		float horizontalSearchRadius = 2.0F;
		float verticalSearchRadius = 4.0F;
		std::uint32_t maximumPoints = 4096U;
		// Detour's node pool bounds corridor search independently from the copied
		// straight-path output. A small guidance budget must never create an invalid
		// zero-sized provider hash table or silently reduce search coverage. Keep
		// this trailing so existing aggregate callers retain their point-budget slot.
		std::uint32_t maximumSearchNodes = 32768U;
		// NearestReachable callers may retain a bounded corridor that stops before
		// the requested endpoint. Exact routes keep the historical fail-closed
		// behavior and reject that same provider partial result.
		bool allowPartial = false;
	};
	struct DetourGroundRouteResult {
		NavigationOutcome outcome = NavigationOutcome::UnavailableCoverage;
		NavigationReason reason = NavigationReason::MissingNeighbor;
		std::vector<NavigationGeometryVector3> points;
	};
	struct DetourGroundBoundarySpan {
		std::uint8_t boundaryAxis = 0U;
		bool positiveSide = false;
		std::uint32_t layer = 0U;
		double intervalStart = 0.0;
		double intervalEnd = 0.0;
		float minimumClearanceMetres = 0.0F;
		bool supported = false;
		bool clearanceValid = false;
		// The two values describe the actual polygon edge at intervalStart/end in
		// the shard-local provider frame. Keeping the edge rather than a polygon
		// average prevents a portal from projecting an elevated floor to Y=0.
		float heightAtIntervalStart = 0.0F;
		float heightAtIntervalEnd = 0.0F;
		// Detour records the profile-derived climb limit in every tile header. The
		// portal graph uses the smaller side's limit before joining two floor edges.
		float maximumStepHeightMetres = 0.0F;
	};
	TERRAENGINE_API DetourGroundShardBuildResult AssembleDetourGroundShard(const DetourGroundShardBuildInput& input);
	TERRAENGINE_API DetourGroundRouteResult QueryDetourGroundShard(
		const DetourGroundShardGeneration& generation, const DetourGroundRouteQuery& query);
	// Emits only navmesh-polygon boundary spans from an immutable shard. The
	// portal layer later pairs adjacent opposite spans; it never assumes that a
	// ready coverage record implies a traversable seam.
	TERRAENGINE_API std::vector<DetourGroundBoundarySpan> CollectDetourGroundBoundarySpans(
		const DetourGroundShardGeneration& generation);
}
