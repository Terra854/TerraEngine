// NavigationRebuildPipeline.cpp - detached dynamic Recast/Detour preparation.

#include "Navigation/Dynamic/NavigationRebuildPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] std::int64_t FloorShard(std::int64_t tile) noexcept {
			// C++ division truncates toward zero. Correct a negative remainder without
			// negating INT64_MIN so canonical tile-to-shard identity stays exact.
			std::int64_t quotient = tile / 8LL;
			if(tile % 8LL < 0LL) --quotient;
			return quotient;
		}

		[[nodiscard]] std::optional<std::int64_t> MultiplyShardTile(std::int64_t shard) noexcept {
			if(shard > std::numeric_limits<std::int64_t>::max() / 8LL
				|| shard < std::numeric_limits<std::int64_t>::min() / 8LL) return std::nullopt;
			return shard * 8LL;
		}

		[[nodiscard]] std::size_t EstimateShardBytes(const DetourGroundShardGeneration& shard) noexcept {
			std::size_t bytes = 0U;
			for(const std::vector<std::byte>& tile : shard.ownedTileBytes) bytes += tile.size();
			return bytes;
		}

		[[nodiscard]] NavigationGeneration ChangeSequence(const NavigationDynamicSourceChange& change) noexcept {
			return change.changeSequence != 0U ? change.changeSequence : change.sourceRevision;
		}

		using ShardAddress = std::tuple<std::int64_t, std::int64_t, std::int32_t>;
		using TileSetByShard = std::map<ShardAddress, std::set<GroundTileKey>>;
		constexpr std::size_t kMaximumIncrementalReceipts = 64U;

		[[nodiscard]] bool UsesPreviousBounds(NavigationDynamicChangeKind kind) noexcept {
			return kind != NavigationDynamicChangeKind::Created && kind != NavigationDynamicChangeKind::Enabled;
		}

		[[nodiscard]] bool UsesCurrentBounds(NavigationDynamicChangeKind kind) noexcept {
			return kind != NavigationDynamicChangeKind::Removed && kind != NavigationDynamicChangeKind::Disabled;
		}

		[[nodiscard]] bool IsFiniteBounds(const NavigationGeometryBounds& bounds) noexcept {
			return std::isfinite(bounds.minimum.x) && std::isfinite(bounds.minimum.y)
				&& std::isfinite(bounds.minimum.z) && std::isfinite(bounds.maximum.x)
				&& std::isfinite(bounds.maximum.y) && std::isfinite(bounds.maximum.z)
				&& bounds.minimum.x <= bounds.maximum.x && bounds.minimum.y <= bounds.maximum.y
				&& bounds.minimum.z <= bounds.maximum.z;
		}

		[[nodiscard]] std::optional<std::int64_t> AddExact(std::int64_t first, std::int64_t second) noexcept {
			if((second > 0 && first > std::numeric_limits<std::int64_t>::max() - second)
				|| (second < 0 && first < std::numeric_limits<std::int64_t>::min() - second)) return std::nullopt;
			return first + second;
		}

		[[nodiscard]] std::optional<std::int64_t> CanonicalCoordinate(double local, std::int64_t origin,
			double edge) noexcept {
			if(!std::isfinite(local) || !std::isfinite(edge) || edge <= 0.0) return std::nullopt;
			const double cell = std::floor(local / edge);
			// INT64_MAX rounds up to +2^63 as a double. Use exact power-of-two bounds
			// so an apparently in-range far-world coordinate can never narrow through
			// an overflowing signed cast.
			constexpr double kMinimumInt64 = -9223372036854775808.0;
			constexpr double kOnePastMaximumInt64 = 9223372036854775808.0;
			if(cell < kMinimumInt64 || cell >= kOnePastMaximumInt64) return std::nullopt;
			return AddExact(origin, static_cast<std::int64_t>(cell));
		}

		[[nodiscard]] bool AppendHaloBounds(const NavigationDynamicSourceChange& change,
			const NavigationGeometryBounds& bounds, const GroundTileBuildHalo& halo,
			const std::uint64_t profileBuildKey, const std::set<ShardAddress>& affected,
			const std::size_t maximumTiles, TileSetByShard& closure, std::string& reason) {
			if(!IsFiniteBounds(bounds)) { reason = "IncrementalReuseReceiptBoundsInvalid"; return false; }
			const double minimumX = static_cast<double>(bounds.minimum.x) - halo.horizontalMetres;
			const double maximumX = static_cast<double>(bounds.maximum.x) + halo.horizontalMetres;
			const double minimumY = static_cast<double>(bounds.minimum.y) - halo.verticalMetres;
			const double maximumY = static_cast<double>(bounds.maximum.y) + halo.verticalMetres;
			const double minimumZ = static_cast<double>(bounds.minimum.z) - halo.horizontalMetres;
			const double maximumZ = static_cast<double>(bounds.maximum.z) + halo.horizontalMetres;
			const auto firstX = CanonicalCoordinate(minimumX, change.canonicalTileOriginX, kGroundTileEdgeMetres);
			const auto lastX = CanonicalCoordinate(maximumX, change.canonicalTileOriginX, kGroundTileEdgeMetres);
			const auto firstZ = CanonicalCoordinate(minimumZ, change.canonicalTileOriginZ, kGroundTileEdgeMetres);
			const auto lastZ = CanonicalCoordinate(maximumZ, change.canonicalTileOriginZ, kGroundTileEdgeMetres);
			const auto firstBand = CanonicalCoordinate(minimumY, change.canonicalVerticalBandOrigin, kGroundVerticalBandMetres);
			const auto lastBand = CanonicalCoordinate(maximumY, change.canonicalVerticalBandOrigin, kGroundVerticalBandMetres);
			if(!firstX || !lastX || !firstZ || !lastZ || !firstBand || !lastBand
				|| *firstBand < std::numeric_limits<std::int32_t>::min()
				|| *lastBand > std::numeric_limits<std::int32_t>::max()) {
				reason = "IncrementalReuseHaloAddressOverflow";
				return false;
			}
			const auto exceeds = [](std::int64_t minimum, std::int64_t maximum, std::size_t limit) {
				if(minimum > maximum || limit == 0U) return true;
				// Unsigned subtraction is defined modulo 2^64, so this remains safe even
				// when the signed endpoints span opposite extremes of the world range.
				const std::uint64_t distance = static_cast<std::uint64_t>(maximum)
					- static_cast<std::uint64_t>(minimum);
				return distance >= limit;
			};
			if(exceeds(*firstX, *lastX, maximumTiles) || exceeds(*firstZ, *lastZ, maximumTiles)
				|| exceeds(*firstBand, *lastBand, maximumTiles)) {
				reason = "IncrementalReuseHaloExceedsBound";
				return false;
			}
			for(std::int32_t band = static_cast<std::int32_t>(*firstBand);; ++band) {
				for(std::int64_t z = *firstZ;; ++z) {
					for(std::int64_t x = *firstX;; ++x) {
						const ShardAddress shard{FloorShard(x), FloorShard(z), band};
						// Reuse never broadens today's dynamic invalidation topology. A halo
						// escaping its already selected shard is a full-build fallback.
						if(!affected.contains(shard)) { reason = "IncrementalReuseHaloEscapesDirtyShards"; return false; }
						TileSetByShard::mapped_type& tiles = closure[shard];
						tiles.insert({x, z, band, profileBuildKey});
						if(tiles.size() > 64U || closure.size() > maximumTiles) {
							reason = "IncrementalReuseClosureExceedsShardBound";
							return false;
						}
						if(x == *lastX) break;
					}
					if(z == *lastZ) break;
				}
				if(band == static_cast<std::int32_t>(*lastBand)) break;
			}
			return true;
		}

		[[nodiscard]] bool BuildIncrementalClosure(const NavigationDynamicRebuildRequest& request,
			const std::set<ShardAddress>& affected, TileSetByShard& closure, std::string& reason) {
			if(request.pendingChanges.empty() || request.pendingChanges.size() > kMaximumIncrementalReceipts) {
				reason = "IncrementalReuseReceiptHistoryUnavailable";
				return false;
			}
			const std::optional<GroundTileBuildHalo> halo = ComputeGroundTileBuildHalo(request.profile);
			if(!halo) { reason = "IncrementalReuseProfileHaloUnavailable"; return false; }
			const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
			const std::size_t maximumTiles = affected.size() * 64U;
			for(const NavigationDynamicSourceChange& change : request.pendingChanges) {
				if(change.source.value == EMPTY_UUID || ChangeSequence(change) == 0U) {
					reason = "IncrementalReuseReceiptInvalid";
					return false;
				}
				if(UsesPreviousBounds(change.kind) && !AppendHaloBounds(change, change.previousBounds, *halo,
					profileBuildKey, affected, maximumTiles, closure, reason)) return false;
				if(UsesCurrentBounds(change.kind) && !AppendHaloBounds(change, change.currentBounds, *halo,
					profileBuildKey, affected, maximumTiles, closure, reason)) return false;
			}
			return true;
		}

		[[nodiscard]] const NavigationDynamicShardReuseBaseline* FindReuseBaseline(
			const NavigationDynamicRebuildRequest& request, const ShardAddress& address,
			std::uint64_t profileBuildKey) noexcept {
			return [&request, &address, profileBuildKey]() -> const NavigationDynamicShardReuseBaseline* {
				const auto found = std::find_if(request.reuseBaselines.begin(), request.reuseBaselines.end(),
					[&request, &address, profileBuildKey](const NavigationDynamicShardReuseBaseline& baseline) {
						return baseline.reference.IsValid() && baseline.retainedTiles && !baseline.retainedTiles->tiles.empty()
							&& baseline.rootGeneration == request.expectedRootGeneration
							&& baseline.terrainPublicationGeneration == request.expectedTerrainPublicationGeneration
							&& baseline.sourceIndexRevision == request.expectedSourceIndexRevision
							&& baseline.reference.shard.x == std::get<0>(address)
							&& baseline.reference.shard.z == std::get<1>(address)
							&& baseline.reference.shard.verticalBand == std::get<2>(address)
							&& baseline.reference.shard.profileBuildKey == profileBuildKey;
					});
				return found == request.reuseBaselines.end() ? nullptr : &*found;
			}();
		}

		[[nodiscard]] bool HasCoherentAutomaticSourceIndex(
			const std::vector<NavigationSourceIndexRecord>& sourceIndex) noexcept {
			for(std::size_t left = 0U; left < sourceIndex.size(); ++left) {
				const NavigationSourceIndexRecord& record = sourceIndex[left];
				if(record.sourceId == EMPTY_UUID || record.sourceRevision == 0U
					|| record.geometryRevision == 0U) return false;
				if(record.identity.IsValid()) {
					// Automatic records carry the complete source identity/evidence tuple.
					// Recomputing it here keeps a worker from assembling a shard from a
					// stale resource, terrain, or streamed physical publication.
					if(!record.revisionEvidence.IsComplete()
						|| record.identity.sourceId != record.sourceId
						|| ComputeNavigationEligibleSourceRevision(record.identity, record.revisionEvidence)
							!= record.sourceRevision) return false;
				}
				for(std::size_t right = left + 1U; right < sourceIndex.size(); ++right)
					if(record.sourceId == sourceIndex[right].sourceId) return false;
			}
			return true;
		}

		void SetPlanFailure(NavigationDynamicRebuildPlan& plan, NavigationOutcome outcome,
			NavigationReason reason, bool permanent) noexcept {
			plan.failureOutcome = outcome;
			plan.failureReason = reason;
			plan.failurePermanent = permanent;
		}

		void SetRecastPlanFailure(NavigationDynamicRebuildPlan& plan,
			RecastGroundBuildStatus status) noexcept {
			// Dynamic input was already copied and source-index validated before this
			// worker runs. A Recast geometry/configuration rejection is therefore a
			// real provider failure for the current request, not a missing live object.
			switch(status) {
			case RecastGroundBuildStatus::BudgetExceeded:
				SetPlanFailure(plan, NavigationOutcome::BudgetExhausted, NavigationReason::OutputLimit, true);
				return;
			case RecastGroundBuildStatus::LocalizationFailed:
				SetPlanFailure(plan, NavigationOutcome::InvalidRequest, NavigationReason::DomainLimit, true);
				return;
			case RecastGroundBuildStatus::Cancelled:
				SetPlanFailure(plan, NavigationOutcome::Stale, NavigationReason::DirtyCoverage, false);
				return;
			case RecastGroundBuildStatus::InvalidProfile:
				SetPlanFailure(plan, NavigationOutcome::InvalidRequest, NavigationReason::ProviderFailure, true);
				return;
			case RecastGroundBuildStatus::InvalidGeometry:
			case RecastGroundBuildStatus::RecastConfigurationFailed:
			case RecastGroundBuildStatus::RecastHeightfieldFailed:
			case RecastGroundBuildStatus::RecastRasterizationFailed:
			case RecastGroundBuildStatus::RecastCompactHeightfieldFailed:
			case RecastGroundBuildStatus::ProviderFailure:
				SetPlanFailure(plan, NavigationOutcome::Failed, NavigationReason::ProviderFailure, true);
				return;
			case RecastGroundBuildStatus::Success:
				return;
			}
		}
	}

	NavigationDynamicRebuildPlan PrepareDynamicGroundReplacement(
		const NavigationDynamicRebuildRequest& request) {
		NavigationDynamicRebuildPlan result{};
		if(request.change.source.value == TerraEngine::EMPTY_UUID || request.change.sourceRevision == 0U
			|| request.change.physicalReadyRevision == 0U || request.expectedRootGeneration == 0U
			|| request.maximumShards == 0U || request.dirtyCoverage.empty()
			|| !request.profile.Validate().Succeeded()) {
			result.reason = "Dynamic rebuild request is missing a valid source, profile, root generation, or dirty coverage.";
			return result;
		}
		if(!HasCoherentAutomaticSourceIndex(request.sourceIndex)) {
			result.reason = "Dynamic rebuild source index has duplicate, incomplete, or stale automatic source evidence.";
			return result;
		}
		for(const auto& source : request.sources) {
			if(!source) {
				result.reason = "Dynamic rebuild request contains a null immutable source snapshot.";
				return result;
			}
			const auto indexed = std::find_if(request.sourceIndex.begin(), request.sourceIndex.end(),
				[&source](const NavigationSourceIndexRecord& entry) {
					return entry.enabled && entry.sourceId == source->Descriptor().sourceUuid.value
						&& entry.sourceRevision == source->Descriptor().sourceRevision;
				});
			if(!request.sourceIndex.empty() && indexed == request.sourceIndex.end()) {
				result.reason = "Dynamic rebuild source snapshot does not match its immutable eligible-source index.";
				return result;
			}
		}

		// Removals intentionally publish their tombstone/dirty state without trying
		// to reconstruct deleted geometry. A later source set may rebuild reopened
		// coverage, but cannot make the removed revision traversable again.
		if(request.sources.empty()) {
			result.candidate.expectedRootGeneration = request.expectedRootGeneration;
			result.candidate.expectedSourceRevision = ChangeSequence(request.change);
			result.candidate.tombstones = request.tombstones;
			result.candidate.sourceReplacements = request.sourceIndex;
			result.candidate.sourceReplacements.push_back({request.change.source.value,
				request.change.sourceRevision, request.change.sourceRevision, false});
			result.accepted = true;
			result.reason = "Prepared tombstone-only dynamic navigation replacement.";
			return result;
		}

		RecastGroundBuildInput buildInput{};
		buildInput.profile = request.profile;
		buildInput.sources = request.sources;
		buildInput.budget = request.budget;

		std::set<ShardAddress> affected;
		for(const NavigationDirtyCoverageDependency& dirty : request.dirtyCoverage)
			affected.emplace(FloorShard(dirty.tileX), FloorShard(dirty.tileZ), dirty.verticalBand);
		if(affected.size() > request.maximumShards) {
			result.reason = "Dynamic rebuild exceeds the configured bounded shard count.";
			SetPlanFailure(result, NavigationOutcome::BudgetExhausted, NavigationReason::OutputLimit, true);
			return result;
		}
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
		std::vector<GroundTileKey> fullShardTiles;
		fullShardTiles.reserve(affected.size() * 64U);
		for(const ShardAddress& shard : affected) {
			const auto firstX = MultiplyShardTile(std::get<0>(shard));
			const auto firstZ = MultiplyShardTile(std::get<1>(shard));
			if(!firstX || !firstZ || *firstX > std::numeric_limits<std::int64_t>::max() - 7LL
				|| *firstZ > std::numeric_limits<std::int64_t>::max() - 7LL) {
				result.reason = "Dynamic rebuild shard tile expansion overflowed canonical int64 addressing.";
				SetPlanFailure(result, NavigationOutcome::InvalidRequest, NavigationReason::DomainLimit, true);
				return result;
			}
			for(std::int64_t localZ = 0LL; localZ < 8LL; ++localZ)
				for(std::int64_t localX = 0LL; localX < 8LL; ++localX)
					fullShardTiles.push_back({*firstX + localX, *firstZ + localZ,
						std::get<2>(shard), profileBuildKey});
		}
		std::map<ShardAddress, std::vector<GroundTileBlob>> grouped;
		TileSetByShard incrementalClosure;
		std::string reuseFailure;
		result.incrementalReuseAttempted = !request.reuseBaselines.empty();
		if(result.incrementalReuseAttempted
			&& request.expectedTerrainPublicationGeneration != 0U
			&& request.expectedSourceIndexRevision != 0U
			&& BuildIncrementalClosure(request, affected, incrementalClosure, reuseFailure)) {
			RecastGroundBuildInput incrementalInput = buildInput;
			for(const auto& [_, tiles] : incrementalClosure)
				incrementalInput.requestedTiles.insert(incrementalInput.requestedTiles.end(), tiles.begin(), tiles.end());
			incrementalInput.maximumTiles = incrementalInput.requestedTiles.size();
			const RecastGroundBuildResult rebuilt = BuildRecastGroundTiles(incrementalInput);
			if(rebuilt.Succeeded()) {
				// Requested tiles are sparse: absence means no current source selected that
				// address, not an evaluated empty tile. Re-enumerate the current expected
				// key set before merging so an old byte sequence is removed only when the
				// immutable source packet proves it disappeared from this closure.
				const GroundCoverageEvidence expected = CaptureGroundCoverageEvidence(incrementalInput);
				if(!expected.accepted) {
					reuseFailure = "IncrementalReuseExpectedTileEvidenceUnavailable:" + expected.reason;
				}
				std::map<ShardAddress, std::map<GroundTileKey, GroundTileBlob>> rebuiltByShard;
				bool merged = expected.accepted;
				std::set<GroundTileKey> expectedKeys(expected.tileKeys.begin(), expected.tileKeys.end());
				for(const GroundTileBlob& tile : rebuilt.tiles) {
					const ShardAddress address{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand};
					if(!affected.contains(address) || !incrementalClosure.contains(address)
						|| !incrementalClosure.at(address).contains(tile.key)
						|| !rebuiltByShard[address].emplace(tile.key, tile).second) {
						merged = false;
						reuseFailure = "IncrementalReuseRebuiltTileSetInvalid";
						break;
					}
				}
				if(merged) {
					for(const auto& [shardAddress, tiles] : rebuiltByShard) {
						(void)shardAddress;
						for(const auto& [key, rebuiltTile] : tiles) {
							(void)rebuiltTile;
							if(!expectedKeys.erase(key)) {
								merged = false;
								reuseFailure = "IncrementalReuseExpectedTileSetMismatch";
								break;
							}
						}
					}
					if(merged && !expectedKeys.empty()) {
						merged = false;
						reuseFailure = "IncrementalReuseExpectedTileSetMismatch";
					}
				}
				for(const ShardAddress& address : affected) {
					const NavigationDynamicShardReuseBaseline* baseline = FindReuseBaseline(request, address, profileBuildKey);
					// The full path skips sparse/no-source shards. Preserve that behavior:
					// only a shard with either prior or freshly emitted tiles needs a pack.
					if(!baseline && rebuiltByShard[address].empty()) continue;
					if(!baseline || !baseline->retainedTiles) {
						merged = false;
						reuseFailure = "IncrementalReuseBaselineUnavailable";
						break;
					}
					std::map<GroundTileKey, GroundTileBlob> mergedTiles;
					for(const GroundTileBlob& tile : baseline->retainedTiles->tiles) {
						const ShardAddress tileAddress{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand};
						if(tileAddress != address || tile.key.profileBuildKey != profileBuildKey
							|| !tile.supportComplete || !tile.clearanceComplete
							|| (tile.empty ? !tile.pristineBytes.empty() : tile.pristineBytes.empty())
							|| !mergedTiles.emplace(tile.key, tile).second) {
							merged = false;
							reuseFailure = "IncrementalReuseBaselineTileInvalid";
							break;
						}
					}
					if(!merged) break;
					const TileSetByShard::mapped_type& changedTiles = incrementalClosure[address];
					for(const GroundTileKey& key : changedTiles) mergedTiles.erase(key);
					for(const auto& [key, tile] : rebuiltByShard[address]) mergedTiles.emplace(key, tile);
					if(mergedTiles.empty()) {
						merged = false;
						reuseFailure = "IncrementalReuseMergedShardEmpty";
						break;
					}
					std::vector<GroundTileBlob>& output = grouped[address];
					output.reserve(mergedTiles.size());
					for(auto& [_, tile] : mergedTiles) output.push_back(std::move(tile));
					result.reusedTileCount += output.size() - rebuiltByShard[address].size();
					result.rebuiltTileCount += rebuiltByShard[address].size();
				}
				if(!merged) grouped.clear();
			}
			else reuseFailure = "IncrementalReuseRecastFailed:" + rebuilt.reason;
		}
		else if(result.incrementalReuseAttempted && reuseFailure.empty()) {
			reuseFailure = "IncrementalReuseBaselineTupleUnavailable";
		}
		if(grouped.empty()) {
			// A failed proof never publishes a partial merge. Rebuild the established
			// complete shard set from the exact same immutable packet instead.
			if(result.incrementalReuseAttempted) {
				// A partially merged map was discarded. Do not report its provisional
				// reuse as selected work; only the fallback's rebuilt tiles remain real.
				result.reusedTileCount = 0U;
				result.fullShardFallbackCount = affected.size();
				result.incrementalReuseFallbackReason = std::move(reuseFailure);
			}
			buildInput.requestedTiles = std::move(fullShardTiles);
			buildInput.maximumTiles = buildInput.requestedTiles.size();
			const RecastGroundBuildResult tiles = BuildRecastGroundTiles(buildInput);
			if(!tiles.Succeeded()) {
				result.reason = "Dynamic Recast tile preparation failed: " + tiles.reason;
				SetRecastPlanFailure(result, tiles.status);
				return result;
			}
			for(const GroundTileBlob& tile : tiles.tiles) {
				const ShardAddress address{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand};
				if(affected.contains(address)) grouped[address].push_back(tile);
			}
			result.rebuiltTileCount += tiles.tiles.size();
		}
		if(grouped.empty()) {
			result.reason = "Dynamic rebuild found no detached tiles for the committed dirty coverage.";
			return result;
		}

		result.candidate.expectedRootGeneration = request.expectedRootGeneration;
		result.candidate.expectedSourceRevision = ChangeSequence(request.change);
		result.candidate.tombstones = request.tombstones;
		result.candidate.sourceReplacements = request.sourceIndex;
		for(auto& [address, shardTiles] : grouped) {
			const GroundShardKey key{std::get<0>(address), std::get<1>(address), std::get<2>(address), profileBuildKey};
			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = key;
			// Source revisions are monotonic at the publication boundary, making the
			// generated shard reference reject an older worker result deterministically.
			// Shard generations order protected publications, not FNV source content
			// identities. A newer content hash may be numerically lower.
			shardInput.expectedGeneration = ChangeSequence(request.change);
			shardInput.tiles = std::move(shardTiles);
			// The assembled generation retains both provider bytes and the copied Recast
			// proof. Use the worker's leased budget so either lifetime remains charged
			// through protected publication and retirement.
			shardInput.budget = request.budget;
			shardInput.minimumClearanceMetres = static_cast<float>(request.profile.clearanceMarginMetres);
			const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
			if(!assembled.accepted || !assembled.generation) {
				result.reason = "Dynamic Detour shard assembly failed: " + assembled.reason;
				SetPlanFailure(result, NavigationOutcome::Failed, NavigationReason::ProviderFailure, true);
				result.shards.clear();
				result.candidate.coverage.clear();
				result.boundaryIntervals.clear();
				return result;
			}
			const auto& generation = assembled.generation;
			const NavigationCoverageKey coverageKey{key.x, key.z, key.verticalBand, key.profileBuildKey};
			result.candidate.coverage.push_back({coverageKey, generation->reference.generation,
				NavigationCoverageState::Ready, EstimateShardBytes(*generation), true});
			for(const DetourGroundBoundarySpan& span : CollectDetourGroundBoundarySpans(*generation)) {
				result.boundaryIntervals.push_back({coverageKey, generation->reference.generation,
					span.boundaryAxis, span.positiveSide, span.layer, span.intervalStart, span.intervalEnd,
					span.minimumClearanceMetres, span.supported, span.clearanceValid});
			}
			result.shards.push_back(generation);
		}
		result.accepted = true;
		result.reason = "Prepared bounded dynamic navigation shard replacement.";
		return result;
	}
}
