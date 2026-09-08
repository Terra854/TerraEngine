// DetourGroundShard.cpp - private Detour assembly and immutable byte ownership.

#include "Navigation/Backend/DetourGroundShard.h"

// Detour types never cross this provider-private translation unit boundary.
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourAlloc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace TerraEngine::Navigation {
	namespace {
		struct NavMeshDeleter { void operator()(dtNavMesh* value) const noexcept { dtFreeNavMesh(value); } };
		struct PrivateDetourShard final {
			// Use Detour's allocation boundary so custom provider allocators remain
			// paired correctly with destruction on every rejected candidate path.
			std::unique_ptr<dtNavMesh, NavMeshDeleter> navMesh{dtAllocNavMesh()};
		};
		[[nodiscard]] bool Cancelled(const DetourGroundShardBuildInput& input) noexcept { try { return input.cancellationRequested && input.cancellationRequested(); } catch(...) { return true; } }
	}
	DetourGroundShardBuildResult AssembleDetourGroundShard(const DetourGroundShardBuildInput& input) {
		DetourGroundShardBuildResult result{};
		if(Cancelled(input)) { result.cancelled = true; result.reason = "CancelledBeforeDetourAssembly"; return result; }
		if(input.expectedGeneration == 0U || input.expectedGeneration == std::numeric_limits<NavigationGeneration>::max()
			|| input.tiles.empty() || input.maximumBytes == 0U || !std::isfinite(input.minimumClearanceMetres)
			|| input.minimumClearanceMetres < 0.0F) { result.reason = "ShardInputInvalid"; return result; }
		std::vector<GroundTileBlob> tiles = input.tiles;
		std::sort(tiles.begin(), tiles.end(), [](const GroundTileBlob& left, const GroundTileBlob& right) { return left.key < right.key; });
		std::set<std::pair<std::int32_t, std::int32_t>> horizontal;
		std::size_t bytes = 0U;
		for(const GroundTileBlob& tile : tiles) {
			if(Cancelled(input)) { result.cancelled = true; result.reason = "CancelledDuringDetourAssembly"; return result; }
			const std::int64_t shardMinimumX = static_cast<std::int64_t>(input.shard.x) * 8;
			const std::int64_t shardMinimumZ = static_cast<std::int64_t>(input.shard.z) * 8;
			if(tile.key.profileBuildKey != input.shard.profileBuildKey || tile.key.verticalBand != input.shard.verticalBand
				|| (tile.pristineBytes.empty() && !tile.empty)
				|| tile.key.x < shardMinimumX || tile.key.x >= shardMinimumX + 8
				|| tile.key.z < shardMinimumZ || tile.key.z >= shardMinimumZ + 8
				|| bytes > input.maximumBytes - tile.pristineBytes.size()) { result.reason = "ShardTileInvalidOrBudgetExceeded"; return result; }
			horizontal.emplace(tile.key.x, tile.key.z);
			if(horizontal.size() > 64U) { result.reason = "ShardHorizontalTileLimitExceeded"; return result; }
			bytes += tile.pristineBytes.size();
		}
		auto provider = std::make_shared<PrivateDetourShard>();
		if(!provider->navMesh) { result.reason = "DetourNavMeshAllocationFailed"; return result; }
		dtNavMeshParams params{};
		params.orig[0] = static_cast<float>(input.shard.x) * 256.0F;
		params.orig[1] = static_cast<float>(input.shard.verticalBand) * 128.0F;
		params.orig[2] = static_cast<float>(input.shard.z) * 256.0F;
		params.tileWidth = 32.0F; params.tileHeight = 32.0F;
		params.maxTiles = static_cast<int>(tiles.size()); params.maxPolys = 2048;
		if(dtStatusFailed(provider->navMesh->init(&params))) { result.reason = "DetourNavMeshInitializationFailed"; return result; }
		auto generation = std::make_shared<DetourGroundShardGeneration>();
		generation->reference = {input.shard, input.expectedGeneration + 1U};
		generation->minimumClearanceMetres = input.minimumClearanceMetres;
		generation->tileKeys.reserve(tiles.size()); generation->ownedTileBytes.reserve(tiles.size());
		for(const GroundTileBlob& tile : tiles) {
			generation->tileKeys.push_back(tile.key);
			generation->ownedTileBytes.push_back(tile.pristineBytes);
			if(tile.empty) {
				continue;
			}
			unsigned char* copied = static_cast<unsigned char*>(dtAlloc(static_cast<int>(tile.pristineBytes.size()), DT_ALLOC_PERM));
			if(copied == nullptr) { result.reason = "DetourTileAllocationFailed"; return result; }
			std::memcpy(copied, tile.pristineBytes.data(), tile.pristineBytes.size());
			dtTileRef ignored{};
			if(dtStatusFailed(provider->navMesh->addTile(copied, static_cast<int>(tile.pristineBytes.size()), DT_TILE_FREE_DATA, 0, &ignored))) { dtFree(copied); result.reason = "DetourAddTileFailed"; return result; }
		}
		generation->providerState = std::move(provider);
		if(input.budget != nullptr) {
			auto reservation = std::make_shared<NavigationBudgetReservation>(input.budget->TryReserve(NavigationBudgetCategory::PublishedNavigation, bytes, "detour shard tile copies"));
			if(!reservation->accepted) { result.reason = reservation->reason; return result; }
			generation->budgetReservation = std::move(reservation);
		}
		result.accepted = true; result.generation = std::move(generation); return result;
	}

	DetourGroundRouteResult QueryDetourGroundShard(
		const DetourGroundShardGeneration& generation, const DetourGroundRouteQuery& query) {
		DetourGroundRouteResult result{};
		if(!generation.reference.IsValid() || !generation.providerState
			|| query.maximumPoints == 0U || !std::isfinite(query.horizontalSearchRadius)
			|| !std::isfinite(query.verticalSearchRadius) || query.horizontalSearchRadius <= 0.0F
			|| query.verticalSearchRadius <= 0.0F) {
			result.outcome = NavigationOutcome::InvalidRequest;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}
		const auto finitePoint = [](const NavigationGeometryVector3& point) {
			return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
		};
		if(!finitePoint(query.start) || !finitePoint(query.destination)) {
			result.outcome = NavigationOutcome::InvalidEndpoint;
			result.reason = NavigationReason::InvalidDestination;
			return result;
		}

		// providerState is created only by AssembleDetourGroundShard in this
		// translation unit, so this cast cannot expose a Detour type publicly.
		const std::shared_ptr<PrivateDetourShard> provider =
			std::static_pointer_cast<PrivateDetourShard>(generation.providerState);
		if(!provider || !provider->navMesh) {
			result.outcome = NavigationOutcome::UnavailableCoverage;
			result.reason = NavigationReason::MissingNeighbor;
			return result;
		}
		dtNavMeshQuery navQuery{};
		const int maximumNodes = static_cast<int>(std::min<std::uint32_t>(query.maximumPoints, 4096U));
		if(dtStatusFailed(navQuery.init(provider->navMesh.get(), maximumNodes))) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		// Recast tiles retain the provider's global tile coordinates internally, but
		// the engine-facing query is always shard-local. Translate at this private
		// boundary so callers cannot accidentally round a canonical cell address to
		// a large global float before a Detour lookup.
		const NavigationGeometryVector3 origin{
			static_cast<float>(generation.reference.shard.x) * 256.0F,
			static_cast<float>(generation.reference.shard.verticalBand) * 128.0F,
			static_cast<float>(generation.reference.shard.z) * 256.0F};
		const float start[3] = {origin.x + query.start.x, origin.y + query.start.y, origin.z + query.start.z};
		const float destination[3] = {origin.x + query.destination.x, origin.y + query.destination.y, origin.z + query.destination.z};
		const float extents[3] = {query.horizontalSearchRadius, query.verticalSearchRadius, query.horizontalSearchRadius};
		dtQueryFilter filter{};
		dtPolyRef startReference = 0U;
		dtPolyRef destinationReference = 0U;
		float resolvedStart[3]{};
		float resolvedDestination[3]{};
		if(dtStatusFailed(navQuery.findNearestPoly(start, extents, &filter, &startReference, resolvedStart))
			|| startReference == 0U) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::MissingSupport;
			return result;
		}
		if(dtStatusFailed(navQuery.findNearestPoly(destination, extents, &filter, &destinationReference, resolvedDestination))
			|| destinationReference == 0U) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::MissingSupport;
			return result;
		}
		std::vector<dtPolyRef> corridor(static_cast<std::size_t>(maximumNodes));
		int corridorCount = 0;
		if(dtStatusFailed(navQuery.findPath(startReference, destinationReference, resolvedStart, resolvedDestination,
			&filter, corridor.data(), &corridorCount, maximumNodes)) || corridorCount <= 0) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::MissingSupport;
			return result;
		}
		std::vector<float> straightPoints(static_cast<std::size_t>(maximumNodes) * 3U);
		std::vector<unsigned char> straightFlags(static_cast<std::size_t>(maximumNodes));
		std::vector<dtPolyRef> straightReferences(static_cast<std::size_t>(maximumNodes));
		int straightCount = 0;
		if(dtStatusFailed(navQuery.findStraightPath(resolvedStart, resolvedDestination, corridor.data(), corridorCount,
			straightPoints.data(), straightFlags.data(), straightReferences.data(), &straightCount, maximumNodes))
			|| straightCount <= 0) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::ProviderFailure;
			return result;
		}
		result.points.reserve(static_cast<std::size_t>(straightCount));
		for(int index = 0; index < straightCount; ++index) {
			result.points.push_back({straightPoints[static_cast<std::size_t>(index) * 3U] - origin.x,
				straightPoints[static_cast<std::size_t>(index) * 3U + 1U] - origin.y,
				straightPoints[static_cast<std::size_t>(index) * 3U + 2U] - origin.z});
		}
		result.outcome = NavigationOutcome::Complete;
		result.reason = NavigationReason::None;
		return result;
	}

	std::vector<DetourGroundBoundarySpan> CollectDetourGroundBoundarySpans(
		const DetourGroundShardGeneration& generation) {
		std::vector<DetourGroundBoundarySpan> spans;
		if(!generation.reference.IsValid() || !generation.providerState) return spans;
		const std::shared_ptr<PrivateDetourShard> provider =
			std::static_pointer_cast<PrivateDetourShard>(generation.providerState);
		if(!provider || !provider->navMesh) return spans;
		const float shardMinimumX = static_cast<float>(generation.reference.shard.x) * 256.0F;
		const float shardMaximumX = shardMinimumX + 256.0F;
		const float shardMinimumZ = static_cast<float>(generation.reference.shard.z) * 256.0F;
		const float shardMaximumZ = shardMinimumZ + 256.0F;
		constexpr float kBoundaryEpsilon = 0.05F;
		const auto append = [&spans, &generation](std::uint8_t axis, bool positive, float start, float end) {
			if(!std::isfinite(start) || !std::isfinite(end) || end <= start) return;
			spans.push_back({axis, positive, 0U, start, end, generation.minimumClearanceMetres, true, true});
		};
		// Read tiles through Detour's public const inspection API. The mutable
		// overload is intentionally private because callers must not edit a live
		// published shard while route jobs may be reading it.
		const dtNavMesh& navMesh = *provider->navMesh;
		for(int tileIndex = 0; tileIndex < navMesh.getMaxTiles(); ++tileIndex) {
			const dtMeshTile* tile = navMesh.getTile(tileIndex);
			if(tile == nullptr || tile->header == nullptr || tile->polys == nullptr || tile->verts == nullptr) continue;
			for(int polygonIndex = 0; polygonIndex < tile->header->polyCount; ++polygonIndex) {
				const dtPoly& polygon = tile->polys[polygonIndex];
				if(polygon.getType() != DT_POLYTYPE_GROUND || polygon.vertCount < 3U) continue;
				float minimumX = std::numeric_limits<float>::max();
				float maximumX = std::numeric_limits<float>::lowest();
				float minimumZ = std::numeric_limits<float>::max();
				float maximumZ = std::numeric_limits<float>::lowest();
				for(unsigned int vertexIndex = 0U; vertexIndex < polygon.vertCount; ++vertexIndex) {
					const float* vertex = &tile->verts[static_cast<std::size_t>(polygon.verts[vertexIndex]) * 3U];
					minimumX = std::min(minimumX, vertex[0]); maximumX = std::max(maximumX, vertex[0]);
					minimumZ = std::min(minimumZ, vertex[2]); maximumZ = std::max(maximumZ, vertex[2]);
				}
				if(minimumX <= shardMinimumX + kBoundaryEpsilon) append(0U, false, minimumZ, maximumZ);
				if(maximumX >= shardMaximumX - kBoundaryEpsilon) append(0U, true, minimumZ, maximumZ);
				if(minimumZ <= shardMinimumZ + kBoundaryEpsilon) append(1U, false, minimumX, maximumX);
				if(maximumZ >= shardMaximumZ - kBoundaryEpsilon) append(1U, true, minimumX, maximumX);
			}
		}
		std::sort(spans.begin(), spans.end(), [](const DetourGroundBoundarySpan& first, const DetourGroundBoundarySpan& second) {
			if(first.boundaryAxis != second.boundaryAxis) return first.boundaryAxis < second.boundaryAxis;
			if(first.positiveSide != second.positiveSide) return first.positiveSide < second.positiveSide;
			if(first.intervalStart != second.intervalStart) return first.intervalStart < second.intervalStart;
			return first.intervalEnd < second.intervalEnd;
		});
		return spans;
	}
}
