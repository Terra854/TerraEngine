// TerrainResizeJobs.cpp -- Worker preparation and protected transaction handoff.

#include "TerrainResizeJobs.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		TerraEngine::Terrain::TerrainAreaDescriptor ResolveCapturedDescriptor(
			const TerraEngine::Terrain::TerrainAssetRoot& root,
			TerraEngine::Terrain::TerrainAreaCoordinate coordinate)
		{
			for(const auto& shard : root.directoryShards)
			{
				const auto found = shard.descriptors.find(coordinate);
				if(found != shard.descriptors.end()) return found->second;
			}
			TerraEngine::Terrain::TerrainAreaDescriptor implicit{};
			implicit.coordinate = coordinate;
			implicit.editGeneration = root.publicationGeneration;
			implicit.minHeightMm = root.defaultHeightMm;
			implicit.maxHeightMm = root.defaultHeightMm;
			implicit.layerPresence = {root.defaultLayerId};
			const std::string identity = TerraEngine::ToHexString(root.assetUuid) + ":implicit:"
				+ std::to_string(coordinate.x) + ":" + std::to_string(coordinate.z) + ":"
				+ std::to_string(root.defaultHeightMm) + ":" + std::to_string(root.defaultLayerId);
			implicit.authoredContentHash = TerraEngine::Terrain::HashTerrainBytes(
				std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
			return implicit;
		}

		void SortUnique(std::vector<TerraEngine::Terrain::TerrainAreaCoordinate>& coordinates)
		{
			std::sort(coordinates.begin(), coordinates.end());
			coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
		}

		void PopulateResizeMembership(const TerraEngine::Terrain::TerrainAssetRoot& root,
			TerraEngine::Terrain::TerrainResizeDelta& delta,
			TerraEngine::Terrain::TerrainResizePreview& preview,
			std::size_t maximumAreas)
		{
			const auto previous = TerraEngine::Terrain::EnumerateTerrainAreas(delta.previousBounds, maximumAreas);
			const auto proposed = TerraEngine::Terrain::EnumerateTerrainAreas(delta.proposedBounds, maximumAreas);
			if(!previous || !proposed) return;
			std::set<TerraEngine::Terrain::TerrainAreaCoordinate> previousSet(previous->begin(), previous->end());
			std::set<TerraEngine::Terrain::TerrainAreaCoordinate> proposedSet(proposed->begin(), proposed->end());
			preview.observations.clear();
			for(const auto coordinate : *previous)
			{
				const auto descriptor = ResolveCapturedDescriptor(root, coordinate);
				preview.observations.push_back({coordinate, descriptor.editGeneration, descriptor.authoredContentHash});
			}
			std::sort(preview.observations.begin(), preview.observations.end(), [](const auto& left, const auto& right) {
				return left.coordinate < right.coordinate;
			});

			delta.descriptorRemovals.clear();
			delta.forcedRetirements.clear();
			delta.partialAreaBounds.clear();
			std::vector<TerraEngine::Terrain::TerrainAreaCoordinate> changed;
			for(const auto coordinate : *previous)
			{
				if(!proposedSet.contains(coordinate))
				{
					changed.push_back(coordinate);
					delta.forcedRetirements.push_back(coordinate);
					for(const auto& shard : root.directoryShards)
						if(shard.descriptors.contains(coordinate)) delta.descriptorRemovals.push_back(coordinate);
				}
			}
			for(const auto coordinate : *proposed)
			{
				if(!previousSet.contains(coordinate)) changed.push_back(coordinate);
				const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(delta.proposedBounds, coordinate);
				if(active && (active->minX != 0u || active->minZ != 0u
					|| active->maxXExclusive != TerraEngine::Terrain::kTerrainAreaExtentSamples
					|| active->maxZExclusive != TerraEngine::Terrain::kTerrainAreaExtentSamples))
				{
					delta.partialAreaBounds.emplace(coordinate, *active);
					if(delta.kind == TerraEngine::Terrain::TerrainResizeKind::Crop)
						delta.forcedRetirements.push_back(coordinate);
					changed.push_back(coordinate);
				}
			}
			SortUnique(delta.descriptorRemovals);
			SortUnique(delta.forcedRetirements);
			SortUnique(changed);
			delta.affectedOneRing.clear();
			for(const auto coordinate : changed)
				for(std::int64_t dz = -1; dz <= 1; ++dz)
					for(std::int64_t dx = -1; dx <= 1; ++dx)
					{
						if((dx < 0 && coordinate.x == (std::numeric_limits<std::int64_t>::min)())
							|| (dx > 0 && coordinate.x == (std::numeric_limits<std::int64_t>::max)())
							|| (dz < 0 && coordinate.z == (std::numeric_limits<std::int64_t>::min)())
							|| (dz > 0 && coordinate.z == (std::numeric_limits<std::int64_t>::max)())) continue;
						delta.affectedOneRing.push_back({coordinate.x + dx, coordinate.z + dz});
					}
			SortUnique(delta.affectedOneRing);
			preview.cropLoss.removedDescriptors = delta.descriptorRemovals.size();
			preview.cropLoss.partialDescriptors = delta.partialAreaBounds.size();
			preview.estimatedWork.generatedAreas = delta.descriptorUpserts.size();
		}

		std::vector<std::pair<TerraEngine::Terrain::ContentHash, std::vector<std::uint8_t>>>
			MaterializeExpansionAreas(const TerraEngine::Terrain::TerrainAssetRoot& root,
				TerraEngine::Terrain::TerrainResizeDelta& delta,
				const TerraEngine::Terrain::TerrainHeightSampler& source,
				const std::map<TerraEngine::Terrain::TerrainAreaCoordinate, TerrainResizeJobRequest::AreaSource>& sources,
				std::size_t maximumAreas, const std::atomic_bool& cancelled)
		{
			std::vector<std::pair<TerraEngine::Terrain::ContentHash, std::vector<std::uint8_t>>> blobs;
			if(delta.kind != TerraEngine::Terrain::TerrainResizeKind::Expand) return blobs;
			const auto previous = TerraEngine::Terrain::EnumerateTerrainAreas(delta.previousBounds, maximumAreas);
			const auto proposed = TerraEngine::Terrain::EnumerateTerrainAreas(delta.proposedBounds, maximumAreas);
			if(!previous || !proposed) return blobs;
			for(const auto coordinate : *proposed)
			{
				if(cancelled.load(std::memory_order_acquire)) continue;
				const auto previousActive = TerraEngine::Terrain::TerrainActiveRectForArea(delta.previousBounds, coordinate);
				const auto active = TerraEngine::Terrain::TerrainActiveRectForArea(delta.proposedBounds, coordinate);
				if(!active) continue;
				// A coordinate can belong to both roots while its active rectangle grows.
				// Replacing that partial descriptor is required so inactive storage from a
				// prior crop can never become visible as newly authored terrain.
				if(previousActive && *previousActive == *active) continue;
				const auto captured = sources.find(coordinate);
				if(captured != sources.end())
				{
					// Replacing an existing partial boundary makes its prior channels part
					// of the exact expansion inverse. Publication retains these hashes until
					// the corresponding history command is pruned.
					if(captured->second.descriptor.channels.heightHash)
						delta.retainedBlobHashes.push_back(*captured->second.descriptor.channels.heightHash);
					if(captured->second.descriptor.channels.holeHash)
						delta.retainedBlobHashes.push_back(*captured->second.descriptor.channels.holeHash);
					for(const auto& [group, channelHash] : captured->second.descriptor.channels.weightGroupHashes)
					{
						(void)group;
						delta.retainedBlobHashes.push_back(channelHash);
					}
				}
				std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, root.defaultHeightMm);
				bool hasNonDefault = false;
				for(std::uint32_t z = 0u; z < 256u; ++z)
					for(std::uint32_t x = 0u; x < 256u; ++x)
					{
						// Inactive tails reuse the nearest active sample, which prevents a
						// later re-expansion from resurrecting uninitialized storage.
						const std::uint32_t canonicalX = std::clamp(x,
							static_cast<std::uint32_t>(active->minX), static_cast<std::uint32_t>(active->maxXExclusive - 1u));
						const std::uint32_t canonicalZ = std::clamp(z,
							static_cast<std::uint32_t>(active->minZ), static_cast<std::uint32_t>(active->maxZExclusive - 1u));
						const TerraEngine::Terrain::TerrainSampleCoordinate sample{
							coordinate.x * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalX,
							coordinate.z * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalZ};
						// Samples retained by the old root are copied byte-for-byte. Only newly
						// exposed coverage is initialized by the selected expansion policy.
						const std::int32_t value = delta.previousBounds.Contains(sample)
							? source(sample).value_or(root.defaultHeightMm)
							: TerraEngine::Terrain::InitializeExpandedTerrainHeight(root, delta, sample, source);
						heights[static_cast<std::size_t>(z) * 256u + x] = value;
						hasNonDefault = hasNonDefault || value != root.defaultHeightMm;
					}
				if(!hasNonDefault && captured == sources.end()) continue;
				std::vector<std::uint8_t> bytes(heights.size() * sizeof(std::int32_t));
				std::memcpy(bytes.data(), heights.data(), bytes.size());
				const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
				TerraEngine::Terrain::TerrainAreaDescriptor descriptor = captured == sources.end()
					? ResolveCapturedDescriptor(root, coordinate) : captured->second.descriptor;
				descriptor.coordinate = coordinate;
				descriptor.editGeneration = root.publicationGeneration + 1u;
				descriptor.channels.heightHash = hash;
				const auto [minimum, maximum] = std::minmax_element(heights.begin(), heights.end());
				descriptor.minHeightMm = *minimum;
				descriptor.maxHeightMm = *maximum;
				if(descriptor.layerPresence.empty()) descriptor.layerPresence = {root.defaultLayerId};
				std::string identity = "terrain-expand-area-v1:" + std::to_string(coordinate.x) + ":"
					+ std::to_string(coordinate.z) + ":h:" + TerraEngine::Terrain::ToHexString(hash);
				if(captured != sources.end() && captured->second.holeBits.size() == TerraEngine::Terrain::kTerrainAreaSampleCount / 8u)
				{
					std::vector<std::uint8_t> holes(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
					auto sourceHole = [&](TerraEngine::Terrain::TerrainSampleCoordinate queried) {
						const auto owner = TerraEngine::Terrain::OwnerArea(queried);
						const auto local = TerraEngine::Terrain::LocalSample(queried);
						if(!owner || !local || *owner != coordinate) return false;
						const std::size_t index = static_cast<std::size_t>(local->z) * 256u + local->x;
						return ((captured->second.holeBits[index / 8u] >> (index & 7u)) & 1u) != 0u;
					};
					for(std::uint32_t z = 0u; z < 256u; ++z)
						for(std::uint32_t x = 0u; x < 256u; ++x)
						{
							const std::uint32_t canonicalX = std::clamp(x,
								static_cast<std::uint32_t>(active->minX), static_cast<std::uint32_t>(active->maxXExclusive - 1u));
							const std::uint32_t canonicalZ = std::clamp(z,
								static_cast<std::uint32_t>(active->minZ), static_cast<std::uint32_t>(active->maxZExclusive - 1u));
							const TerraEngine::Terrain::TerrainSampleCoordinate sample{
								coordinate.x * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalX,
								coordinate.z * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalZ};
							const bool hole = delta.previousBounds.Contains(sample)
								? sourceHole(sample)
								: TerraEngine::Terrain::InitializeExpandedTerrainHole(delta, sample, sourceHole);
							const std::size_t index = static_cast<std::size_t>(z) * 256u + x;
							if(hole) holes[index / 8u] |= static_cast<std::uint8_t>(1u << (index & 7u));
						}
					const auto holeHash = TerraEngine::Terrain::HashTerrainBytes(holes);
					descriptor.channels.holeHash = holeHash;
					identity += ":o:" + TerraEngine::Terrain::ToHexString(holeHash);
					blobs.emplace_back(holeHash, std::move(holes));
				}
				else if(descriptor.channels.holeHash)
					identity += ":o:" + TerraEngine::Terrain::ToHexString(*descriptor.channels.holeHash);
				if(captured != sources.end())
					for(const auto& [group, sourceSamples] : captured->second.surfaceGroups)
					{
						if(sourceSamples.size() != TerraEngine::Terrain::kTerrainAreaSampleCount) continue;
						const std::array<TerraEngine::Terrain::TerrainWeightContribution, 1u> fallback{{{root.defaultLayerId, 255u}}};
						const auto defaultSample = TerraEngine::Terrain::NormalizeTerrainWeights(fallback);
						std::vector<TerraEngine::Terrain::TerrainWeightSample> expanded(
							TerraEngine::Terrain::kTerrainAreaSampleCount, defaultSample);
						auto sourceSurface = [&](TerraEngine::Terrain::TerrainSampleCoordinate queried)
							-> std::optional<TerraEngine::Terrain::TerrainWeightSample> {
							const auto owner = TerraEngine::Terrain::OwnerArea(queried);
							const auto local = TerraEngine::Terrain::LocalSample(queried);
							if(!owner || !local || *owner != coordinate) return std::nullopt;
							return sourceSamples[static_cast<std::size_t>(local->z) * 256u + local->x];
						};
						for(std::uint32_t z = 0u; z < 256u; ++z)
							for(std::uint32_t x = 0u; x < 256u; ++x)
							{
								const std::uint32_t canonicalX = std::clamp(x,
									static_cast<std::uint32_t>(active->minX), static_cast<std::uint32_t>(active->maxXExclusive - 1u));
								const std::uint32_t canonicalZ = std::clamp(z,
									static_cast<std::uint32_t>(active->minZ), static_cast<std::uint32_t>(active->maxZExclusive - 1u));
								const TerraEngine::Terrain::TerrainSampleCoordinate sample{
									coordinate.x * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalX,
									coordinate.z * TerraEngine::Terrain::kTerrainAreaExtentSamples + canonicalZ};
								expanded[static_cast<std::size_t>(z) * 256u + x] = delta.previousBounds.Contains(sample)
									? sourceSurface(sample).value_or(defaultSample)
									: TerraEngine::Terrain::InitializeExpandedTerrainSurface(root, delta, sample, sourceSurface);
							}
						std::vector<std::uint8_t> surfaceBytes;
						if(!TerraEngine::Terrain::MaterializeTerrainWeightGroup(expanded, surfaceBytes).Succeeded()) continue;
						const auto surfaceHash = TerraEngine::Terrain::HashTerrainBytes(surfaceBytes);
						descriptor.channels.weightGroupHashes[group] = surfaceHash;
						identity += ":w:" + std::to_string(group) + ":" + TerraEngine::Terrain::ToHexString(surfaceHash);
						blobs.emplace_back(surfaceHash, std::move(surfaceBytes));
					}
				for(const auto& [group, weightHash] : descriptor.channels.weightGroupHashes)
					if(captured == sources.end() || !captured->second.surfaceGroups.contains(group))
						identity += ":w:" + std::to_string(group) + ":" + TerraEngine::Terrain::ToHexString(weightHash);
				descriptor.authoredContentHash = TerraEngine::Terrain::HashTerrainBytes(
					std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
				delta.descriptorUpserts.push_back(descriptor);
				blobs.emplace_back(hash, std::move(bytes));
			}
			std::sort(delta.descriptorUpserts.begin(), delta.descriptorUpserts.end(), [](const auto& left, const auto& right) {
				return left.coordinate < right.coordinate;
			});
			std::sort(delta.retainedBlobHashes.begin(), delta.retainedBlobHashes.end(), [](const auto& left, const auto& right) {
				return left.low64 != right.low64 ? left.low64 < right.low64 : left.high64 < right.high64;
			});
			delta.retainedBlobHashes.erase(std::unique(delta.retainedBlobHashes.begin(), delta.retainedBlobHashes.end()),
				delta.retainedBlobHashes.end());
			return blobs;
		}

		std::vector<std::pair<TerraEngine::Terrain::ContentHash, std::vector<std::uint8_t>>>
			MaterializeCropPartialAreas(const TerraEngine::Terrain::TerrainAssetRoot& root,
				TerraEngine::Terrain::TerrainResizeDelta& delta,
				TerraEngine::Terrain::TerrainResizePreview& preview,
				const std::map<TerraEngine::Terrain::TerrainAreaCoordinate, TerrainResizeJobRequest::AreaSource>& sources,
				const std::atomic_bool& cancelled)
		{
			std::vector<std::pair<TerraEngine::Terrain::ContentHash, std::vector<std::uint8_t>>> blobs;
			if(delta.kind != TerraEngine::Terrain::TerrainResizeKind::Crop) return blobs;
			for(const auto& [coordinate, source] : sources)
			{
				const bool removed = std::binary_search(delta.descriptorRemovals.begin(), delta.descriptorRemovals.end(), coordinate);
				const auto partial = delta.partialAreaBounds.find(coordinate);
				auto retainSourceChannels = [&] {
					if(source.descriptor.channels.heightHash) delta.retainedBlobHashes.push_back(*source.descriptor.channels.heightHash);
					if(source.descriptor.channels.holeHash) delta.retainedBlobHashes.push_back(*source.descriptor.channels.holeHash);
					for(const auto& [group, hash] : source.descriptor.channels.weightGroupHashes)
					{
						(void)group;
						delta.retainedBlobHashes.push_back(hash);
					}
				};
				if(removed)
				{
					retainSourceChannels();
					preview.cropLoss.nonDefaultHeightSamples += static_cast<std::uint64_t>(std::count_if(
						source.heightMm.begin(), source.heightMm.end(), [&](std::int32_t value) { return value != root.defaultHeightMm; }));
					for(std::uint8_t byte : source.holeBits)
						preview.cropLoss.holeSamples += static_cast<std::uint64_t>(std::popcount(static_cast<unsigned>(byte)));
					for(const auto& [group, samples] : source.surfaceGroups)
					{
						(void)group;
						preview.cropLoss.paintedSamples += static_cast<std::uint64_t>(std::count_if(samples.begin(), samples.end(), [&](const auto& sample) {
							return sample.slots[0].layerId != root.defaultLayerId || sample.slots[0].weight != 255u;
						}));
					}
					preview.cropLoss.retainedHistoryBytes += source.heightMm.size() * sizeof(std::int32_t) + source.holeBits.size();
					for(const auto& [group, samples] : source.surfaceGroups)
					{
						(void)group;
						preview.cropLoss.retainedHistoryBytes += samples.size() * 12u;
					}
					continue;
				}
				if(partial == delta.partialAreaBounds.end() || cancelled.load(std::memory_order_acquire)) continue;
				retainSourceChannels();
				TerraEngine::Terrain::TerrainAreaDescriptor replacement = source.descriptor;
				if(replacement.editGeneration == (std::numeric_limits<std::uint64_t>::max)()) continue;
				++replacement.editGeneration;
				std::string identity = "terrain-crop-area-v1:" + std::to_string(coordinate.x) + ":" + std::to_string(coordinate.z);
				if(source.heightMm.size() == TerraEngine::Terrain::kTerrainAreaSampleCount)
				{
					auto heights = TerraEngine::Terrain::CanonicalizeCroppedTerrainHeights(source.heightMm, partial->second);
					for(std::uint32_t z = 0u; z < 256u; ++z)
						for(std::uint32_t x = 0u; x < 256u; ++x)
							if(x < partial->second.minX || x >= partial->second.maxXExclusive
								|| z < partial->second.minZ || z >= partial->second.maxZExclusive)
								preview.cropLoss.nonDefaultHeightSamples += source.heightMm[static_cast<std::size_t>(z) * 256u + x] != root.defaultHeightMm;
					std::vector<std::uint8_t> bytes(heights.size() * sizeof(std::int32_t));
					std::memcpy(bytes.data(), heights.data(), bytes.size());
					const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
					replacement.channels.heightHash = hash;
					identity += ":h:" + TerraEngine::Terrain::ToHexString(hash);
					const auto [minimum, maximum] = std::minmax_element(heights.begin(), heights.end());
					replacement.minHeightMm = *minimum;
					replacement.maxHeightMm = *maximum;
					blobs.emplace_back(hash, std::move(bytes));
				}
				if(source.holeBits.size() == TerraEngine::Terrain::kTerrainAreaSampleCount / 8u)
				{
					for(std::uint32_t z = 0u; z < 256u; ++z)
						for(std::uint32_t x = 0u; x < 256u; ++x)
							if((x < partial->second.minX || x >= partial->second.maxXExclusive
								|| z < partial->second.minZ || z >= partial->second.maxZExclusive)
								&& ((source.holeBits[(static_cast<std::size_t>(z) * 256u + x) / 8u] >> (x & 7u)) & 1u))
								++preview.cropLoss.holeSamples;
					auto holes = TerraEngine::Terrain::CanonicalizeCroppedTerrainHoles(source.holeBits, partial->second);
					const auto hash = TerraEngine::Terrain::HashTerrainBytes(holes);
					replacement.channels.holeHash = hash;
					identity += ":o:" + TerraEngine::Terrain::ToHexString(hash);
					blobs.emplace_back(hash, std::move(holes));
				}
				for(const auto& [group, samples] : source.surfaceGroups)
				{
					if(samples.size() != TerraEngine::Terrain::kTerrainAreaSampleCount) continue;
					for(std::uint32_t z = 0u; z < 256u; ++z)
						for(std::uint32_t x = 0u; x < 256u; ++x)
							if((x < partial->second.minX || x >= partial->second.maxXExclusive
								|| z < partial->second.minZ || z >= partial->second.maxZExclusive)
								&& (samples[static_cast<std::size_t>(z) * 256u + x].slots[0].layerId != root.defaultLayerId
									|| samples[static_cast<std::size_t>(z) * 256u + x].slots[0].weight != 255u))
								++preview.cropLoss.paintedSamples;
					auto canonical = TerraEngine::Terrain::CanonicalizeCroppedTerrainSurfaces(samples, partial->second, root.defaultLayerId);
					if(canonical.empty()) continue;
					std::vector<std::uint8_t> bytes;
					if(!TerraEngine::Terrain::MaterializeTerrainWeightGroup(canonical, bytes).Succeeded()) continue;
					const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
					replacement.channels.weightGroupHashes[group] = hash;
					identity += ":w:" + std::to_string(group) + ":" + TerraEngine::Terrain::ToHexString(hash);
					blobs.emplace_back(hash, std::move(bytes));
				}
				replacement.authoredContentHash = TerraEngine::Terrain::HashTerrainBytes(
					std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
				delta.descriptorUpserts.push_back(std::move(replacement));
			}
			std::sort(delta.descriptorUpserts.begin(), delta.descriptorUpserts.end(), [](const auto& left, const auto& right) {
				return left.coordinate < right.coordinate;
			});
			return blobs;
		}

		void InspectExpansionEdges(const TerraEngine::Terrain::TerrainAssetRoot& root,
			const TerraEngine::Terrain::TerrainResizeDelta& delta,
			const TerraEngine::Terrain::TerrainHeightSampler& source,
			TerraEngine::Terrain::TerrainResizePreview& preview)
		{
			if(delta.kind != TerraEngine::Terrain::TerrainResizeKind::Expand || !source) return;
			struct SideSamples
			{
				TerraEngine::Terrain::TerrainResizeSide side;
				bool active;
				bool xAxis;
				std::int64_t boundary;
				std::int64_t inward;
			};
			const auto& old = delta.previousBounds;
			const std::array<SideSamples, 4u> sides{{
				{TerraEngine::Terrain::TerrainResizeSide::MinimumX, delta.proposedBounds.minX < old.minX, true, old.minX, old.minX + 1},
				{TerraEngine::Terrain::TerrainResizeSide::MaximumX, delta.proposedBounds.maxXExclusive > old.maxXExclusive, true, old.maxXExclusive - 1, old.maxXExclusive - 2},
				{TerraEngine::Terrain::TerrainResizeSide::MinimumZ, delta.proposedBounds.minZ < old.minZ, false, old.minZ, old.minZ + 1},
				{TerraEngine::Terrain::TerrainResizeSide::MaximumZ, delta.proposedBounds.maxZExclusive > old.maxZExclusive, false, old.maxZExclusive - 1, old.maxZExclusive - 2}}};
			for(const auto& side : sides)
			{
				if(!side.active) continue;
				TerraEngine::Terrain::TerrainResizeEdgeRisk risk{};
				risk.side = side.side;
				risk.minimumDeltaMm = (std::numeric_limits<std::int32_t>::max)();
				risk.maximumDeltaMm = (std::numeric_limits<std::int32_t>::min)();
				std::int64_t sum = 0;
				std::size_t count = 0u;
				const std::int64_t begin = side.xAxis ? old.minZ : old.minX;
				const std::int64_t end = side.xAxis ? old.maxZExclusive : old.maxXExclusive;
				for(std::int64_t along = begin; along < end; ++along)
				{
					const TerraEngine::Terrain::TerrainSampleCoordinate boundary = side.xAxis
						? TerraEngine::Terrain::TerrainSampleCoordinate{side.boundary, along}
						: TerraEngine::Terrain::TerrainSampleCoordinate{along, side.boundary};
					const TerraEngine::Terrain::TerrainSampleCoordinate inward = side.xAxis
						? TerraEngine::Terrain::TerrainSampleCoordinate{side.inward, along}
						: TerraEngine::Terrain::TerrainSampleCoordinate{along, side.inward};
					const auto boundaryHeight = source(boundary);
					if(!boundaryHeight) continue;
					const std::int32_t deltaMm = *boundaryHeight - source(inward).value_or(*boundaryHeight);
					risk.minimumDeltaMm = std::min(risk.minimumDeltaMm, deltaMm);
					risk.maximumDeltaMm = std::max(risk.maximumDeltaMm, deltaMm);
					risk.maximumSlopePerMille = std::max(risk.maximumSlopePerMille,
						static_cast<std::uint32_t>(std::min<std::uint64_t>(1'000'000u,
							static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(deltaMm))) * 1000u
							/ std::max<std::uint64_t>(1u, static_cast<std::uint64_t>(root.sampleSpacingMetres * 1000.0)))));
					sum += deltaMm;
					++count;
					preview.discontinuityWarning = preview.discontinuityWarning
						|| (delta.expansionPolicy == TerraEngine::Terrain::TerrainExpansionPolicy::FlatDefault
							&& *boundaryHeight != root.defaultHeightMm);
				}
				if(count == 0u) risk.minimumDeltaMm = risk.maximumDeltaMm = 0;
				risk.averageDeltaMm = count == 0u ? 0 : sum / static_cast<std::int64_t>(count);
				risk.diagnosticCode = preview.discontinuityWarning ? "TerrainResizeFlatDefaultCliffRisk" : "TerrainResizeEdgeInspected";
				preview.edgeRiskBySide.push_back(std::move(risk));
			}
			if(preview.discontinuityWarning)
				preview.warnings.push_back("Flat Default may create a cliff where the authored edge differs from the asset default height.");
		}
	}

	bool TerrainResizeJobs::Start(TerrainResizeJobRequest request, std::string& outError)
	{
		if (!request.observedRoot.IsValid() || request.maximumAreas == 0u)
		{
			outError = "Terrain resize requires an immutable root capture and a positive area budget.";
			return false;
		}
		Cancel("Coalesced by a newer terrain resize preview.");
		const auto token = std::make_shared<std::atomic_bool>(false);
		const auto state = std::make_shared<WorkerState>();
		const TerraEngine::UUID jobId = TerraEngine::GenerateUUID();
		if(request.budget.maximumScannedAreas == 64u && request.maximumAreas != 64u)
			request.budget.maximumScannedAreas = request.maximumAreas;
		request.maximumAreas = request.budget.maximumScannedAreas;
		const std::size_t workWeight = request.maximumAreas;
		{
			std::scoped_lock lock(mutex);
			cancellation = token;
			runningState = state;
			progress = {jobId, TerrainResizeStage::Validating, 0.05F, 0u, request.maximumAreas, true,
				"Validating captured terrain resize bounds."};
		}
		{
			std::scoped_lock stateLock(state->mutex);
			state->progress = progress;
		}
		auto work = [request = std::move(request), token, state, jobId]() mutable -> Completed {
			Completed completed{};
			auto update = [&](TerrainResizeStage stage, float fraction, std::size_t completedAreas, std::string message) {
				std::scoped_lock stateLock(state->mutex);
				state->progress = {jobId, stage, fraction, completedAreas, request.maximumAreas,
					stage != TerrainResizeStage::AwaitingProtectedPublish && stage != TerrainResizeStage::Committed,
					std::move(message)};
				completed.progress = state->progress;
			};
			update(request.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Expand
				? TerrainResizeStage::InspectingBoundary : TerrainResizeStage::InspectingDiscardedContent,
				0.25F, 0u, "Inspecting immutable terrain resize inputs.");
			if (token->load(std::memory_order_acquire))
			{
				update(TerrainResizeStage::Cancelled, completed.progress.fraction, 0u,
					"Terrain resize cancelled before preview preparation.");
				return completed;
			}
			TerraEngine::Terrain::TerrainResizeRequest coreRequest{};
			coreRequest.transactionId = jobId;
			coreRequest.targetAssetUuid = request.observedRoot.root->assetUuid;
			coreRequest.expectedRootHash = request.observedRoot.root->rootContentHash;
			coreRequest.expectedRootGeneration = request.observedRoot.root->publicationGeneration;
			coreRequest.delta = request.delta;
			coreRequest.budget = request.budget;
			coreRequest.holePolicyExplicit = request.holePolicyExplicit;
			auto preview = TerraEngine::Terrain::PreviewTerrainResize(*request.observedRoot.root, coreRequest);
			if (!preview.Succeeded())
			{
				update(TerrainResizeStage::Failed, 1.0F, 0u, preview.message);
				return completed;
			}
			PopulateResizeMembership(*request.observedRoot.root, preview.delta, preview, request.maximumAreas);
			InspectExpansionEdges(*request.observedRoot.root, preview.delta, request.sourceHeight, preview);
			auto preparedBlobs = MaterializeExpansionAreas(*request.observedRoot.root,
				preview.delta, request.sourceHeight, request.sourceAreas, request.maximumAreas, *token);
			auto cropBlobs = MaterializeCropPartialAreas(*request.observedRoot.root,
				preview.delta, preview, request.sourceAreas, *token);
			preparedBlobs.insert(preparedBlobs.end(), std::make_move_iterator(cropBlobs.begin()),
				std::make_move_iterator(cropBlobs.end()));
			preview.estimatedWork.generatedAreas = preview.delta.descriptorUpserts.size();
			preview.estimatedWork.preparedBytes = 0u;
			for(const auto& [hash, bytes] : preparedBlobs)
			{
				(void)hash;
				preview.estimatedWork.preparedBytes += bytes.size();
			}
			if(preview.estimatedWork.generatedAreas > request.budget.maximumGeneratedAreas
				|| preview.estimatedWork.preparedBytes > request.budget.maximumPreparedBytes
				|| preview.cropLoss.retainedHistoryBytes > request.budget.maximumHistoryBytes)
			{
				update(TerrainResizeStage::Failed, 1.0F, preview.previousAreaCount,
					"Terrain resize materialization exceeds the configured preparation or history budget.");
				return completed;
			}
			coreRequest.delta = preview.delta;
			preview.proposalFingerprint = TerraEngine::Terrain::FingerprintTerrainResizeProposal(
				*request.observedRoot.root, coreRequest, preview);
			if (token->load(std::memory_order_acquire))
			{
				update(TerrainResizeStage::Cancelled, 0.75F, preview.previousAreaCount,
					"Terrain resize cancelled after bounded preview preparation.");
				return completed;
			}
			completed.prepared = TerrainPreparedResize{jobId, request.observedRoot, std::move(coreRequest),
				std::move(preview), request.observedRoot.root->publicationGeneration + 1u, std::move(preparedBlobs)};
			update(TerrainResizeStage::ReadyForConfirmation, 1.0F,
				completed.prepared->preview.previousAreaCount,
				"Detached terrain resize preview is ready for confirmation.");
			return completed;
		};
		if(ThreadPool::instance().getThreadCount() == 0u)
		{
			// Headless tools and focused tests can request a resize before the engine
			// scheduler starts its shared workers. Keep preparation off the caller and
			// protected publication threads by using one owned asynchronous worker.
			future = std::async(std::launch::async, std::move(work));
		}
		else
		{
			auto task = ThreadPool::instance().createTask(std::move(work));
			future = task->getFuture();
			ThreadPool::instance().enqueue(task, workWeight);
		}
		return true;
	}

	void TerrainResizeJobs::Cancel(std::string_view reason)
	{
		std::scoped_lock lock(mutex);
		if (cancellation) cancellation->store(true, std::memory_order_release);
		if(runningState)
		{
			std::scoped_lock stateLock(runningState->mutex);
			runningState->progress.stage = TerrainResizeStage::Cancelled;
			runningState->progress.cancellationAvailable = false;
			runningState->progress.message = std::string(reason);
		}
		if (progress.stage != TerrainResizeStage::Idle && progress.stage != TerrainResizeStage::Committed)
		{
			progress.stage = TerrainResizeStage::Cancelled;
			progress.message = std::string(reason);
		}
	}

	TerrainResizeProgress TerrainResizeJobs::Progress() const
	{
		std::scoped_lock lock(mutex);
		if(runningState)
		{
			std::scoped_lock stateLock(runningState->mutex);
			return runningState->progress;
		}
		return progress;
	}

	std::optional<TerrainPreparedResize> TerrainResizeJobs::TakeReadyPreview()
	{
		if (!future.valid() || future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
		Completed completed = future.get();
		std::scoped_lock lock(mutex);
		progress = completed.progress;
		runningState.reset();
		return std::move(completed.prepared);
	}

	std::optional<TerraEngine::Terrain::TerrainEditTransaction> TerrainResizeJobs::BuildTransaction(
		TerraEngine::Terrain::TerrainAssetStore& store, const TerrainPreparedResize& prepared,
		bool dataLossConfirmed, std::string& outError)
	{
		const auto current = store.CaptureRoot();
		if (!current.IsValid() || current.root != prepared.observedRoot.root)
		{
			outError = "Terrain resize preview is stale because the authored root changed.";
			return std::nullopt;
		}
		if (prepared.preview.requiresDataLossConfirmation && !dataLossConfirmed)
		{
			outError = "Terrain crop requires explicit data-loss confirmation from this preview.";
			return std::nullopt;
		}
		for(const auto& [hash, bytes] : prepared.preparedBlobs)
			if(!store.Blobs().PutVerified(hash, bytes))
			{
				outError = "A prepared terrain resize blob failed content verification.";
				return std::nullopt;
			}
		TerraEngine::Terrain::TerrainEditTransaction transaction{};
		transaction.transactionId = TerraEngine::GenerateUUID();
		transaction.targetAssetUuid = current.root->assetUuid;
		transaction.inputRootHash = current.root->rootContentHash;
		transaction.expectedLayerStackGeneration = current.root->layerStackGeneration;
		transaction.affectedBounds = prepared.preview.delta.proposedBounds;
		transaction.historyGroup = prepared.preview.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Expand
			? "Expand Terrain" : "Crop Terrain";
		transaction.resizeDelta = prepared.preview.delta;
		TerraEngine::Terrain::TerrainResizeDelta inverse = prepared.preview.delta;
		inverse.previousBounds = prepared.preview.delta.proposedBounds;
		inverse.proposedBounds = prepared.preview.delta.previousBounds;
		inverse.kind = prepared.preview.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Expand
			? TerraEngine::Terrain::TerrainResizeKind::Crop : TerraEngine::Terrain::TerrainResizeKind::Expand;
		inverse.descriptorUpserts.clear();
		inverse.descriptorRemovals.clear();
		if (prepared.preview.delta.kind == TerraEngine::Terrain::TerrainResizeKind::Crop)
		{
			// Whole removed descriptors are content-addressed value objects. Keeping
			// them in the inverse makes undo exact while their channel blobs remain
			// pinned by the history receipt/command lifetime.
			for (const auto coordinate : prepared.preview.delta.descriptorRemovals)
				if (const auto descriptor = store.FindDescriptor(*current.root, coordinate))
					inverse.descriptorUpserts.push_back(*descriptor);
			for(const auto& replacement : prepared.preview.delta.descriptorUpserts)
				if(const auto descriptor = store.FindDescriptor(*current.root, replacement.coordinate))
					inverse.descriptorUpserts.push_back(*descriptor);
			std::sort(inverse.descriptorUpserts.begin(), inverse.descriptorUpserts.end(), [](const auto& left, const auto& right) {
				return left.coordinate < right.coordinate;
			});
		}
		else
		{
			for (const auto& descriptor : prepared.preview.delta.descriptorUpserts)
			{
				// Expanding through an existing partial boundary replaces its descriptor.
				// Undo must restore that exact value object; only brand-new sparse areas
				// should be removed from the inverse root.
				if(const auto previous = store.FindDescriptor(*current.root, descriptor.coordinate))
					inverse.descriptorUpserts.push_back(*previous);
				else inverse.descriptorRemovals.push_back(descriptor.coordinate);
			}
		}
		transaction.inverseResizeDelta = std::move(inverse);
		transaction.operationFingerprint = prepared.preview.proposalFingerprint;
		for(const auto& observation : prepared.preview.observations)
			transaction.readSet.push_back({observation.coordinate, observation.editGeneration, observation.authoredContentHash});
		std::sort(transaction.readSet.begin(), transaction.readSet.end(), [](const auto& left, const auto& right) {
			return left.coordinate < right.coordinate;
		});
		transaction.budget.maximumAreas = std::max<std::size_t>(1u, prepared.preview.proposedAreaCount);
		transaction.budget.maximumAreas = std::max(transaction.budget.maximumAreas, transaction.readSet.size());
		transaction.budget.maximumSamples = transaction.budget.maximumAreas > (std::numeric_limits<std::size_t>::max)() / TerraEngine::Terrain::kTerrainAreaSampleCount
			? (std::numeric_limits<std::size_t>::max)()
			: transaction.budget.maximumAreas * TerraEngine::Terrain::kTerrainAreaSampleCount;
		return transaction;
	}
}
