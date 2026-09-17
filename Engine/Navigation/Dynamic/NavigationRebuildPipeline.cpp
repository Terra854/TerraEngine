// NavigationRebuildPipeline.cpp - detached dynamic Recast/Detour preparation.

#include "Navigation/Dynamic/NavigationRebuildPipeline.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>

namespace TerraEngine::Navigation {
	namespace {
		[[nodiscard]] std::int32_t FloorShard(std::int32_t tile) noexcept {
			return tile >= 0 ? tile / 8 : -static_cast<std::int32_t>((-static_cast<std::int64_t>(tile) + 7) / 8);
		}

		[[nodiscard]] std::size_t EstimateShardBytes(const DetourGroundShardGeneration& shard) noexcept {
			std::size_t bytes = 0U;
			for(const std::vector<std::byte>& tile : shard.ownedTileBytes) bytes += tile.size();
			return bytes;
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
			result.candidate.expectedSourceRevision = request.change.sourceRevision;
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
		const RecastGroundBuildResult tiles = BuildRecastGroundTiles(buildInput);
		if(!tiles.Succeeded()) {
			result.reason = "Dynamic Recast tile preparation failed: " + tiles.reason;
			return result;
		}

		using ShardAddress = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
		std::set<ShardAddress> affected;
		for(const NavigationDirtyCoverageDependency& dirty : request.dirtyCoverage)
			affected.emplace(FloorShard(dirty.tileX), FloorShard(dirty.tileZ), dirty.verticalBand);
		std::map<ShardAddress, std::vector<GroundTileBlob>> grouped;
		for(const GroundTileBlob& tile : tiles.tiles) {
			const ShardAddress address{FloorShard(tile.key.x), FloorShard(tile.key.z), tile.key.verticalBand};
			if(affected.contains(address)) grouped[address].push_back(tile);
		}
		if(grouped.empty()) {
			result.reason = "Dynamic rebuild found no detached tiles for the committed dirty coverage.";
			return result;
		}
		if(grouped.size() > request.maximumShards) {
			result.reason = "Dynamic rebuild exceeds the configured bounded shard count.";
			return result;
		}

		result.candidate.expectedRootGeneration = request.expectedRootGeneration;
		result.candidate.expectedSourceRevision = request.change.sourceRevision;
		result.candidate.tombstones = request.tombstones;
		result.candidate.sourceReplacements = request.sourceIndex;
		const std::uint64_t profileBuildKey = MakeGroundProfileBuildKey(request.profile);
		for(auto& [address, shardTiles] : grouped) {
			const GroundShardKey key{std::get<0>(address), std::get<1>(address), std::get<2>(address), profileBuildKey};
			DetourGroundShardBuildInput shardInput{};
			shardInput.shard = key;
			// Source revisions are monotonic at the publication boundary, making the
			// generated shard reference reject an older worker result deterministically.
			shardInput.expectedGeneration = request.change.sourceRevision;
			shardInput.tiles = std::move(shardTiles);
			shardInput.minimumClearanceMetres = static_cast<float>(request.profile.clearanceMarginMetres);
			const DetourGroundShardBuildResult assembled = AssembleDetourGroundShard(shardInput);
			if(!assembled.accepted || !assembled.generation) {
				result.reason = "Dynamic Detour shard assembly failed: " + assembled.reason;
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
