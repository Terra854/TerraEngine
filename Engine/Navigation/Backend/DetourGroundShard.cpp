// DetourGroundShard.cpp - private Detour assembly and immutable byte ownership.

#include "Navigation/Backend/DetourGroundShard.h"

// Detour types never cross this provider-private translation unit boundary.
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#include <DetourAlloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
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
		constexpr std::size_t kMaximumCachedTileRecords = 1'000'000U;

		[[nodiscard]] bool CheckedAdd(std::size_t first, std::size_t second, std::size_t& result) noexcept {
			if(second > std::numeric_limits<std::size_t>::max() - first) return false;
			result = first + second;
			return true;
		}

		[[nodiscard]] bool CheckedMultiply(std::size_t first, std::size_t second, std::size_t& result) noexcept {
			if(first != 0U && second > std::numeric_limits<std::size_t>::max() / first) return false;
			result = first * second;
			return true;
		}

		[[nodiscard]] bool AppendAlignedSection(
			std::size_t count, std::size_t elementBytes, std::size_t& cursor, std::size_t total) noexcept {
			std::size_t bytes = 0U;
			if(!CheckedMultiply(count, elementBytes, bytes) || !CheckedAdd(cursor, bytes, cursor)) return false;
			const std::size_t alignment = cursor % 4U;
			if(alignment != 0U && !CheckedAdd(cursor, 4U - alignment, cursor)) return false;
			return cursor <= total;
		}

		template<typename Value>
		[[nodiscard]] bool ReadUntrustedValue(std::span<const std::byte> bytes, std::size_t offset, Value& value) noexcept {
			if(offset > bytes.size() || bytes.size() - offset < sizeof(Value)) return false;
			std::memcpy(&value, bytes.data() + offset, sizeof(Value));
			return true;
		}

		[[nodiscard]] bool IsFiniteOrderedBounds(const dtMeshHeader& header) noexcept {
			for(std::size_t axis = 0U; axis < 3U; ++axis) {
				if(!std::isfinite(header.bmin[axis]) || !std::isfinite(header.bmax[axis])
					|| header.bmin[axis] > header.bmax[axis]) return false;
			}
			return std::isfinite(header.walkableHeight) && std::isfinite(header.walkableRadius)
				&& std::isfinite(header.walkableClimb) && std::isfinite(header.bvQuantFactor)
				&& header.walkableHeight > 0.0F && header.walkableRadius >= 0.0F && header.walkableClimb >= 0.0F
				&& header.bvQuantFactor > 0.0F;
		}

		[[nodiscard]] bool LocalTileCoordinate(std::int64_t tile, std::int64_t shard,
			std::int32_t& local) noexcept {
			if(shard > std::numeric_limits<std::int64_t>::max() / 8LL
				|| shard < std::numeric_limits<std::int64_t>::min() / 8LL) return false;
			const std::int64_t start = shard * 8LL;
			if(tile < start) return false;
			const std::int64_t difference = tile - start;
			if(difference < 0LL || difference > 7LL) return false;
			local = static_cast<std::int32_t>(difference);
			return true;
		}

		[[nodiscard]] bool ValidateProviderTile(
			std::span<const std::byte> bytes, const GroundTileBlob& expected,
			const GroundShardKey& shard, std::string& reason) noexcept {
			// Cache checksums detect accidental corruption, not a malicious or malformed
			// Detour payload. Validate every section size and index before addTile gets
			// a pointer into untrusted persisted bytes.
			dtMeshHeader header{};
			std::int32_t localX = 0, localZ = 0;
			if(!LocalTileCoordinate(expected.key.x, shard.x, localX)
				|| !LocalTileCoordinate(expected.key.z, shard.z, localZ)
				|| bytes.size() < sizeof(header) || !ReadUntrustedValue(bytes, 0U, header)
				|| header.magic != DT_NAVMESH_MAGIC || header.version != DT_NAVMESH_VERSION
				// Provider headers are deliberately bounded shard-local addresses. The
				// GroundTileKey remains the authoritative canonical int64 address.
				|| header.x != localX || header.y != localZ || header.layer != 0
				|| header.polyCount <= 0 || header.vertCount <= 0 || header.maxLinkCount < 0
				|| header.detailMeshCount != header.polyCount || header.detailVertCount < 0
				|| header.detailTriCount < 0 || header.bvNodeCount < 0 || header.offMeshConCount != 0
				|| header.offMeshBase != header.polyCount || !IsFiniteOrderedBounds(header)) {
				reason = "DetourCachedTileHeaderInvalid";
				return false;
			}
			const auto withinRecordBound = [](int value) {
				return value >= 0 && static_cast<std::size_t>(value) <= kMaximumCachedTileRecords;
			};
			if(!withinRecordBound(header.polyCount) || !withinRecordBound(header.vertCount)
				|| !withinRecordBound(header.maxLinkCount) || !withinRecordBound(header.detailVertCount)
				|| !withinRecordBound(header.detailTriCount) || !withinRecordBound(header.bvNodeCount)) {
				reason = "DetourCachedTileRecordCountInvalid";
				return false;
			}
			if(header.bvNodeCount != header.polyCount * 2) {
				reason = "DetourCachedTileBvNodeCountInvalid";
				return false;
			}

			std::size_t cursor = (sizeof(dtMeshHeader) + 3U) & ~std::size_t{3U};
			const std::size_t verticesOffset = cursor;
			if(!AppendAlignedSection(static_cast<std::size_t>(header.vertCount), sizeof(float) * 3U, cursor, bytes.size())) {
				reason = "DetourCachedTileVertexSectionInvalid";
				return false;
			}
			const std::size_t polygonsOffset = cursor;
			if(!AppendAlignedSection(static_cast<std::size_t>(header.polyCount), sizeof(dtPoly), cursor, bytes.size())
				|| !AppendAlignedSection(static_cast<std::size_t>(header.maxLinkCount), sizeof(dtLink), cursor, bytes.size())) {
				reason = "DetourCachedTilePolygonSectionInvalid";
				return false;
			}
			const std::size_t detailMeshesOffset = cursor;
			if(!AppendAlignedSection(static_cast<std::size_t>(header.detailMeshCount), sizeof(dtPolyDetail), cursor, bytes.size())) {
				reason = "DetourCachedTileDetailMeshSectionInvalid";
				return false;
			}
			const std::size_t detailVerticesOffset = cursor;
			if(!AppendAlignedSection(static_cast<std::size_t>(header.detailVertCount), sizeof(float) * 3U, cursor, bytes.size())) {
				reason = "DetourCachedTileDetailVertexSectionInvalid";
				return false;
			}
			const std::size_t detailTrianglesOffset = cursor;
			if(!AppendAlignedSection(static_cast<std::size_t>(header.detailTriCount), 4U, cursor, bytes.size())
				|| !AppendAlignedSection(static_cast<std::size_t>(header.bvNodeCount), sizeof(dtBVNode), cursor, bytes.size())
				|| cursor != bytes.size()) {
				reason = "DetourCachedTileTrailingOrBoundsSectionInvalid";
				return false;
			}

			for(std::size_t index = 0U; index < static_cast<std::size_t>(header.vertCount) * 3U; ++index) {
				float coordinate = 0.0F;
				if(!ReadUntrustedValue(bytes, verticesOffset + index * sizeof(float), coordinate) || !std::isfinite(coordinate)) {
					reason = "DetourCachedTileVertexInvalid";
					return false;
				}
			}
			for(std::size_t index = 0U; index < static_cast<std::size_t>(header.polyCount); ++index) {
				dtPoly polygon{};
				if(!ReadUntrustedValue(bytes, polygonsOffset + index * sizeof(dtPoly), polygon)
					|| polygon.getType() != DT_POLYTYPE_GROUND || polygon.vertCount < 3U
					|| polygon.vertCount > DT_VERTS_PER_POLYGON) {
					reason = "DetourCachedTilePolygonInvalid";
					return false;
				}
				for(std::size_t vertex = 0U; vertex < polygon.vertCount; ++vertex) {
					if(polygon.verts[vertex] >= static_cast<unsigned int>(header.vertCount)) {
						reason = "DetourCachedTilePolygonIndexInvalid";
						return false;
					}
					const std::uint16_t neighbour = polygon.neis[vertex];
					if((neighbour & DT_EXT_LINK) != 0U) {
						const std::uint16_t side = neighbour & 0xffU;
						const std::uint16_t allowedBits = static_cast<std::uint16_t>(DT_EXT_LINK | 0xffU);
						if((neighbour & static_cast<std::uint16_t>(~allowedBits)) != 0U
							|| (side != 0U && side != 2U && side != 4U && side != 6U)) {
							reason = "DetourCachedTileExternalNeighbourInvalid";
							return false;
						}
					}
					else if(neighbour != 0U && neighbour > static_cast<std::uint16_t>(header.polyCount)) {
						reason = "DetourCachedTileInternalNeighbourInvalid";
						return false;
					}
				}
			}
			for(std::size_t index = 0U; index < static_cast<std::size_t>(header.detailMeshCount); ++index) {
				dtPolyDetail detail{};
				dtPoly polygon{};
				if(!ReadUntrustedValue(bytes, detailMeshesOffset + index * sizeof(dtPolyDetail), detail)
					|| !ReadUntrustedValue(bytes, polygonsOffset + index * sizeof(dtPoly), polygon)
					|| detail.vertBase > static_cast<std::size_t>(header.detailVertCount)
					|| detail.vertCount > static_cast<std::size_t>(header.detailVertCount) - detail.vertBase
					|| detail.triBase > static_cast<std::size_t>(header.detailTriCount)
					|| detail.triCount > static_cast<std::size_t>(header.detailTriCount) - detail.triBase) {
					reason = "DetourCachedTileDetailIndexInvalid";
					return false;
				}
				const std::size_t detailVertexLimit = static_cast<std::size_t>(polygon.vertCount) + detail.vertCount;
				for(std::size_t triangleIndex = 0U; triangleIndex < detail.triCount; ++triangleIndex) {
					std::array<unsigned char, 4U> triangle{};
					const std::size_t offset = detailTrianglesOffset
						+ (static_cast<std::size_t>(detail.triBase) + triangleIndex) * triangle.size();
					if(!ReadUntrustedValue(bytes, offset, triangle)
						|| triangle[0U] >= detailVertexLimit || triangle[1U] >= detailVertexLimit
						|| triangle[2U] >= detailVertexLimit) {
						reason = "DetourCachedTileDetailTriangleIndexInvalid";
						return false;
					}
				}
			}
			for(std::size_t index = 0U; index < static_cast<std::size_t>(header.detailVertCount) * 3U; ++index) {
				float coordinate = 0.0F;
				if(!ReadUntrustedValue(bytes, detailVerticesOffset + index * sizeof(float), coordinate) || !std::isfinite(coordinate)) {
					reason = "DetourCachedTileDetailVertexInvalid";
					return false;
				}
			}
			const std::size_t bvTreeOffset = detailTrianglesOffset
				+ ((static_cast<std::size_t>(header.detailTriCount) * 4U + 3U) & ~std::size_t{3U});
			for(std::size_t index = 0U; index < static_cast<std::size_t>(header.bvNodeCount); ++index) {
				dtBVNode node{};
				if(!ReadUntrustedValue(bytes, bvTreeOffset + index * sizeof(dtBVNode), node)) {
					reason = "DetourCachedTileBvNodeOutOfBounds";
					return false;
				}
				for(std::size_t axis = 0U; axis < 3U; ++axis) {
					if(node.bmin[axis] > node.bmax[axis]) {
						reason = "DetourCachedTileBvBoundsInvalid";
						return false;
					}
				}
				if(node.i >= 0 && node.i >= header.polyCount) {
					reason = "DetourCachedTileBvLeafIndexInvalid";
					return false;
				}
				if(node.i < 0 && (node.i == std::numeric_limits<int>::min()
					|| static_cast<std::size_t>(-node.i) > static_cast<std::size_t>(header.bvNodeCount) - index)) {
					reason = "DetourCachedTileBvEscapeIndexInvalid";
					return false;
				}
			}
			return true;
		}
	}
	DetourGroundShardBuildResult AssembleDetourGroundShard(const DetourGroundShardBuildInput& input) {
		DetourGroundShardBuildResult result{};
		if(Cancelled(input)) { result.cancelled = true; result.reason = "CancelledBeforeDetourAssembly"; return result; }
		if(input.expectedGeneration == 0U || input.expectedGeneration == std::numeric_limits<NavigationGeneration>::max()
			|| input.tiles.empty() || input.maximumBytes == 0U || !std::isfinite(input.minimumClearanceMetres)
			|| input.minimumClearanceMetres < 0.0F) { result.reason = "ShardInputInvalid"; return result; }
		std::vector<GroundTileBlob> tiles = input.tiles;
		std::sort(tiles.begin(), tiles.end(), [](const GroundTileBlob& left, const GroundTileBlob& right) { return left.key < right.key; });
		std::set<std::pair<std::int64_t, std::int64_t>> horizontal;
		std::size_t bytes = 0U;
		for(const GroundTileBlob& tile : tiles) {
			if(Cancelled(input)) { result.cancelled = true; result.reason = "CancelledDuringDetourAssembly"; return result; }
			std::int32_t localX = 0, localZ = 0;
			if(tile.key.profileBuildKey != input.shard.profileBuildKey || tile.key.verticalBand != input.shard.verticalBand
				|| (tile.pristineBytes.empty() && !tile.empty)
				|| !LocalTileCoordinate(tile.key.x, input.shard.x, localX)
				|| !LocalTileCoordinate(tile.key.z, input.shard.z, localZ)
				|| tile.pristineBytes.size() > input.maximumBytes
				|| bytes > input.maximumBytes - tile.pristineBytes.size()) { result.reason = "ShardTileInvalidOrBudgetExceeded"; return result; }
			horizontal.emplace(tile.key.x, tile.key.z);
			if(horizontal.size() > 64U) { result.reason = "ShardHorizontalTileLimitExceeded"; return result; }
			bytes += tile.pristineBytes.size();
		}
		std::shared_ptr<RetainedGroundTilePack> retainedTiles;
		try {
			// Preserve the complete Recast proof before provider assembly. Move the
			// already-sorted vector into the retained pack instead of copying it again;
			// the pack remains the immutable source for every later assembly step.
			retainedTiles = std::make_shared<RetainedGroundTilePack>();
			retainedTiles->tiles = std::move(tiles);
		}
		catch(...) {
			result.reason = "DetourRetainedTileProofAllocationFailed";
			return result;
		}
		std::size_t retainedBytes = bytes;
		for(const GroundTileBlob& tile : retainedTiles->tiles) {
			if(tile.sourceFingerprints.capacity() > (std::numeric_limits<std::size_t>::max() - retainedBytes)
				/ sizeof(std::uint64_t)) {
				result.reason = "DetourRetainedTileProofSizeOverflow";
				return result;
			}
			retainedBytes += tile.sourceFingerprints.capacity() * sizeof(std::uint64_t);
		}
		if(input.budget != nullptr && retainedBytes != 0U) {
			auto reservation = std::make_shared<NavigationBudgetReservation>(input.budget->TryReserve(
				NavigationBudgetCategory::PublishedNavigation, retainedBytes, "retained Recast ground tile proof"));
			if(!reservation->accepted) {
				result.reason = reservation->reason;
				return result;
			}
			retainedTiles->budgetReservation = std::move(reservation);
		}
		auto provider = std::make_shared<PrivateDetourShard>();
		if(!provider->navMesh) { result.reason = "DetourNavMeshAllocationFailed"; return result; }
		dtNavMeshParams params{};
		// Recast persists global signed tile coordinates in every Detour tile header.
		// Detour uses this origin to map a world-space query back to those header
		// coordinates before it searches. A per-shard origin would remap -62m in
		// shard -1 to tile +6 instead of the stored global tile -2, making every
		// non-zero shard appear to have no support. Keep the provider grid canonical;
		// QueryDetourGroundShard remains responsible for translating its public
		// shard-local inputs into this global provider frame.
		params.orig[0] = 0.0F;
		params.orig[1] = 0.0F;
		params.orig[2] = 0.0F;
		params.tileWidth = 32.0F; params.tileHeight = 32.0F;
		params.maxTiles = static_cast<int>(retainedTiles->tiles.size()); params.maxPolys = 2048;
		if(dtStatusFailed(provider->navMesh->init(&params))) { result.reason = "DetourNavMeshInitializationFailed"; return result; }
		auto generation = std::make_shared<DetourGroundShardGeneration>();
		generation->reference = {input.shard, input.expectedGeneration + 1U};
		generation->minimumClearanceMetres = input.minimumClearanceMetres;
		generation->retainedTiles = std::move(retainedTiles);
		// Keep the retained proof and provider-facing pristine byte copies separate:
		// Detour may mutate its own tile memory, while dynamic rebuilds still need the
		// original bytes, empty-tile markers, and source fingerprints untouched.
		const RetainedGroundTilePack& retainedPack = *generation->retainedTiles;
		generation->tileKeys.reserve(retainedPack.tiles.size());
		generation->ownedTileBytes.reserve(retainedPack.tiles.size());
		for(const GroundTileBlob& tile : retainedPack.tiles) {
			generation->tileKeys.push_back(tile.key);
			generation->ownedTileBytes.push_back(tile.pristineBytes);
			if(tile.empty) {
				continue;
			}
			std::string validationReason;
			if(!ValidateProviderTile(tile.pristineBytes, tile, input.shard, validationReason)) {
				result.reason = validationReason;
				return result;
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
			|| query.maximumSearchNodes == 0U || query.maximumPoints == 0U
			|| !std::isfinite(query.horizontalSearchRadius)
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
		// Detour accepts signed counts. Reject values it cannot represent instead of
		// narrowing a configured budget and producing a route with hidden limits.
		constexpr std::uint32_t kMinimumDetourSearchNodes = 4U;
		constexpr std::uint32_t kMaximumDetourCount =
			static_cast<std::uint32_t>(std::numeric_limits<int>::max());
		if(query.maximumSearchNodes < kMinimumDetourSearchNodes
			|| query.maximumSearchNodes > kMaximumDetourCount) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		if(query.maximumPoints > kMaximumDetourCount) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::OutputLimit;
			return result;
		}
		const int maximumSearchNodes = static_cast<int>(query.maximumSearchNodes);
		const int maximumOutputPoints = static_cast<int>(query.maximumPoints);
		dtNavMeshQuery navQuery{};
		// The node pool and the straight-path buffer are deliberately independent:
		// output capacity limits copied guidance while search capacity limits Detour
		// expansion/corridor work. This keeps a one-point output limit safe.
		if(dtStatusFailed(navQuery.init(provider->navMesh.get(), maximumSearchNodes))) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		// The shard owns a 256 m local provider frame. Never add canonical int64
		// coordinates here: callers already localized both endpoints precisely.
		const float start[3] = {query.start.x, query.start.y, query.start.z};
		const float destination[3] = {query.destination.x, query.destination.y, query.destination.z};
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
		std::vector<dtPolyRef> corridor(static_cast<std::size_t>(maximumSearchNodes));
		int corridorCount = 0;
		const dtStatus pathStatus = navQuery.findPath(startReference, destinationReference,
			resolvedStart, resolvedDestination, &filter, corridor.data(), &corridorCount, maximumSearchNodes);
		// A bounded internal search/corridor is a budget outcome, not an approximate
		// route. Check these details before DT_PARTIAL_RESULT because Detour sets
		// both flags when it stops at the best node reached so far.
		if(dtStatusDetail(pathStatus, DT_OUT_OF_NODES)) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		if(dtStatusDetail(pathStatus, DT_BUFFER_TOO_SMALL)) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		const bool partialCorridor = dtStatusDetail(pathStatus, DT_PARTIAL_RESULT)
			|| (corridorCount > 0
				&& corridor[static_cast<std::size_t>(corridorCount - 1)] != destinationReference);
		if(dtStatusFailed(pathStatus) || corridorCount <= 0 || (partialCorridor && !query.allowPartial)) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::MissingSupport;
			return result;
		}
		float straightDestination[3] = {resolvedDestination[0], resolvedDestination[1], resolvedDestination[2]};
		if(partialCorridor) {
			// Project the requested endpoint onto the last reachable polygon. This
			// gives NearestReachable a real provider point without ever exposing the
			// unreachable requested endpoint as if it had support.
			if(dtStatusFailed(navQuery.closestPointOnPoly(
				corridor[static_cast<std::size_t>(corridorCount - 1)], resolvedDestination,
				straightDestination, nullptr))) {
				result.outcome = NavigationOutcome::Unreachable;
				result.reason = NavigationReason::MissingSupport;
				return result;
			}
		}
		std::vector<float> straightPoints(static_cast<std::size_t>(maximumOutputPoints) * 3U);
		std::vector<unsigned char> straightFlags(static_cast<std::size_t>(maximumOutputPoints));
		std::vector<dtPolyRef> straightReferences(static_cast<std::size_t>(maximumOutputPoints));
		int straightCount = 0;
		const dtStatus straightStatus = navQuery.findStraightPath(resolvedStart, straightDestination,
			corridor.data(), corridorCount, straightPoints.data(), straightFlags.data(),
			straightReferences.data(), &straightCount, maximumOutputPoints);
		const bool partialStraight = dtStatusDetail(straightStatus, DT_PARTIAL_RESULT);
		if(dtStatusDetail(straightStatus, DT_OUT_OF_NODES)) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::NodeLimit;
			return result;
		}
		if(dtStatusDetail(straightStatus, DT_BUFFER_TOO_SMALL)) {
			result.outcome = NavigationOutcome::BudgetExhausted;
			result.reason = NavigationReason::OutputLimit;
			return result;
		}
		if(dtStatusFailed(straightStatus)
			|| (partialStraight && !(query.allowPartial && partialCorridor))
			|| straightCount <= 0) {
			result.outcome = NavigationOutcome::Unreachable;
			result.reason = NavigationReason::ProviderFailure;
			return result;
		}
		result.points.reserve(static_cast<std::size_t>(straightCount));
		for(int index = 0; index < straightCount; ++index) {
			result.points.push_back({straightPoints[static_cast<std::size_t>(index) * 3U],
				straightPoints[static_cast<std::size_t>(index) * 3U + 1U],
				straightPoints[static_cast<std::size_t>(index) * 3U + 2U]});
		}
		const bool exposedPartial = partialCorridor || partialStraight;
		result.outcome = exposedPartial ? NavigationOutcome::Partial : NavigationOutcome::Complete;
		result.reason = exposedPartial ? NavigationReason::DomainLimit : NavigationReason::None;
		return result;
	}

	std::vector<DetourGroundBoundarySpan> CollectDetourGroundBoundarySpans(
		const DetourGroundShardGeneration& generation) {
		std::vector<DetourGroundBoundarySpan> spans;
		if(!generation.reference.IsValid() || !generation.providerState) return spans;
		const std::shared_ptr<PrivateDetourShard> provider =
			std::static_pointer_cast<PrivateDetourShard>(generation.providerState);
		if(!provider || !provider->navMesh) return spans;
		const float shardMinimumX = 0.0F;
		const float shardMaximumX = 256.0F;
		const float shardMinimumZ = 0.0F;
		const float shardMaximumZ = 256.0F;
		constexpr float kBoundaryEpsilon = 0.05F;
		const auto append = [&spans, &generation](std::uint8_t axis, bool positive,
			float firstTransverse, float secondTransverse, float firstHeight, float secondHeight,
			float maximumStepHeight) {
			if(!std::isfinite(firstTransverse) || !std::isfinite(secondTransverse)
				|| !std::isfinite(firstHeight) || !std::isfinite(secondHeight)
				|| !std::isfinite(maximumStepHeight) || maximumStepHeight < 0.0F
				|| firstTransverse == secondTransverse) return;
			// Normalize transverse direction with the matching heights. A later portal
			// overlap can then interpolate this edge without relying on polygon order.
			float start = firstTransverse;
			float end = secondTransverse;
			float startHeight = firstHeight;
			float endHeight = secondHeight;
			if(end < start) {
				std::swap(start, end);
				std::swap(startHeight, endHeight);
			}
			// Adjacent coverage records share the same transverse shard, so their
			// bounded local interval compares exactly in GroundPortalGraph. Keep it
			// local: converting an int64 shard address to a global double would lose
			// seam identity at distant canonical cells.
			spans.push_back({axis, positive, 0U, start, end,
				generation.minimumClearanceMetres, true, true, startHeight, endHeight,
				maximumStepHeight});
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
				for(unsigned int vertexIndex = 0U; vertexIndex < polygon.vertCount; ++vertexIndex) {
					const unsigned int nextVertexIndex = (vertexIndex + 1U) % polygon.vertCount;
					const float* first = &tile->verts[static_cast<std::size_t>(polygon.verts[vertexIndex]) * 3U];
					const float* second = &tile->verts[static_cast<std::size_t>(polygon.verts[nextVertexIndex]) * 3U];
					// A polygon merely touching a shard boundary is not enough. Emit the
					// actual exterior edge so the portal has a physical Y coordinate and
					// cannot bridge another floor that happens to share its X/Z bounds.
					if(first[0] <= shardMinimumX + kBoundaryEpsilon
						&& second[0] <= shardMinimumX + kBoundaryEpsilon) {
						append(0U, false, first[2], second[2], first[1], second[1],
							tile->header->walkableClimb);
					}
					if(first[0] >= shardMaximumX - kBoundaryEpsilon
						&& second[0] >= shardMaximumX - kBoundaryEpsilon) {
						append(0U, true, first[2], second[2], first[1], second[1],
							tile->header->walkableClimb);
					}
					if(first[2] <= shardMinimumZ + kBoundaryEpsilon
						&& second[2] <= shardMinimumZ + kBoundaryEpsilon) {
						append(1U, false, first[0], second[0], first[1], second[1],
							tile->header->walkableClimb);
					}
					if(first[2] >= shardMaximumZ - kBoundaryEpsilon
						&& second[2] >= shardMaximumZ - kBoundaryEpsilon) {
						append(1U, true, first[0], second[0], first[1], second[1],
							tile->header->walkableClimb);
					}
				}
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
