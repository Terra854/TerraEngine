// TerrainRenderPackets.cpp -- Converts cooked immutable terrain data to VG intents.

#include "Renderer/Terrain/TerrainRenderPackets.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Renderer::Terrain
{
	TerrainAreaRenderPacketResult PrepareTerrainAreaRenderPackets(
		const TerrainAssetRenderSnapshot& snapshot,
		std::span<const TerrainAreaRenderRequest> requests,
		std::span<const TerrainPageResidencyEntry> residency)
	{
		TerrainAreaRenderPacketResult result{};
		if(!snapshot.IsComplete() || requests.empty()) return result;
		std::vector<TerrainPageSelectionRequest> pageRequests;
		pageRequests.reserve(requests.size());
		for(const TerrainAreaRenderRequest& request : requests)
		{
			if(snapshot.FindArea(request.area) == nullptr) return result;
			pageRequests.push_back({request.frameId, request.viewId, snapshot.assetId,
				request.area, request.desiredPageId, request.priority});
		}
		const TerrainPageDemandResult pageSelection = BuildTerrainPageDemands(pageRequests, residency);
		if(!pageSelection.completeCoverage || pageSelection.resolutions.size() != requests.size()) return result;

		result.packets.reserve(requests.size());
		for(std::size_t index = 0u; index < requests.size(); ++index)
		{
			const TerrainAreaRenderRequest& request = requests[index];
			const TerrainAreaRenderBinding* area = snapshot.FindArea(request.area);
			const TerrainPageResolution& resolution = pageSelection.resolutions[index];
			TerrainAreaRenderPacket packet{};
			packet.route = request.route;
			packet.purpose = request.purpose;
			packet.stableAreaIdentity = area->stableAreaIdentity;
			packet.controlIdentity = area->controlIdentity;
			packet.holeIdentity = area->holeIdentity;
			packet.materialId = area->materialId;
			packet.desiredPageId = resolution.desiredPageId;
			packet.actualPageId = resolution.actualPageId;
			packet.usesAncestorPage = resolution.fallbackDepth != 0u;
			packet.castsShadow = request.purpose == TerrainRenderPurpose::Shadow;
			packet.pick.assetId = snapshot.assetId;
			packet.pick.area = area->coordinate;
			packet.pick.localCellX = request.localCellX;
			packet.pick.localCellZ = request.localCellZ;
			packet.pick.authoredHash = area->authoredHash;
			packet.pick.authoredGeneration = area->authoredGeneration;
			packet.pick.pageSourceIdentity = area->pageSourceIdentity;
			result.packets.push_back(std::move(packet));
		}
		result.demands = pageSelection.demands;
		result.complete = true;
		return result;
	}

	bool TerrainDrawMappingPacket::IsComplete() const noexcept
	{
		if(!std::isfinite(sampleSpacingMetres) || sampleSpacingMetres <= 0.0
			|| !std::isfinite(localSampleOrigin[0]) || !std::isfinite(localSampleOrigin[1])
			|| localSampleOrigin[0] < 0.0 || localSampleOrigin[1] < 0.0
			|| localSampleOrigin[0] > static_cast<double>(kTerrainControlExtentSamples)
			|| localSampleOrigin[1] > static_cast<double>(kTerrainControlExtentSamples)
			|| controlExtentSamples != kTerrainControlExtentSamples
			|| layerCount == 0u || layerCount > kTerrainGpuLayerLimit)
		{
			return false;
		}
		for(std::uint32_t layerIndex = 0u; layerIndex < layerCount; ++layerIndex)
		{
			const TerrainGpuLayerPhase& phase = layerPhases[layerIndex];
			if(phase.stableLayerId == 0u || !std::isfinite(phase.phaseMetres[0])
				|| !std::isfinite(phase.phaseMetres[1]) || phase.phaseMetres[0] < 0.0F
				|| phase.phaseMetres[1] < 0.0F) return false;
		}
		return true;
	}

	std::optional<TerrainDrawMappingPacket> PrepareTerrainDrawMappingPacket(
		const TerrainMaterialRuntimeBinding& material,
		TerraEngine::Terrain::TerrainAreaCoordinate area,
		std::array<double, 2u> localSampleOrigin)
	{
		if(!material.IsComplete() || !material.HasCompleteAreaMappings()) return std::nullopt;
		const auto found = std::find_if(material.areaMappings.begin(), material.areaMappings.end(),
			[area](const TerrainAreaMappingBinding& candidate) { return candidate.area == area; });
		if(found == material.areaMappings.end()) return std::nullopt;
		TerrainDrawMappingPacket packet{};
		packet.area = area;
		packet.sampleSpacingMetres = found->sampleSpacingMetres;
		packet.localSampleOrigin = localSampleOrigin;
		packet.controlExtentSamples = found->controlExtentSamples;
		packet.layerPhases = found->layerPhases;
		packet.layerCount = found->layerCount;
		if(!packet.IsComplete()) return std::nullopt;
		for(std::uint32_t layerIndex = 0u; layerIndex < packet.layerCount; ++layerIndex)
		{
			TerrainGpuLayerBinding gpuLayer{};
			if(!BuildTerrainGpuLayerBinding(material.layers[layerIndex], gpuLayer)
				|| gpuLayer.stableLayerId != material.layers[layerIndex].stableLayerId)
				return std::nullopt;
			if(packet.layerPhases[layerIndex].stableLayerId != material.layers[layerIndex].stableLayerId
				|| packet.layerPhases[layerIndex].phaseMetres[0]
					>= gpuLayer.tileSizeMetres[0]
				|| packet.layerPhases[layerIndex].phaseMetres[1]
					>= gpuLayer.tileSizeMetres[1]) return std::nullopt;
		}
		return packet;
	}

	bool TerrainPickIdentity::IsComplete() const noexcept
	{
		return assetId != TerraEngine::EMPTY_UUID && authoredHash.IsValid() && authoredGeneration != 0u
			&& pageSourceIdentity.IsValid() && localCellX < 256u && localCellZ < 256u;
	}

	std::optional<TerrainPreparedRenderPacket> PrepareTerrainRenderPacket(const TerrainViewPacket& view, const TerrainPageCookResult& cooked, const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication>& priorVisiblePages)
	{
		if (view.assetId == TerraEngine::EMPTY_UUID || !view.authoredHash.IsValid() || !view.pageSourceIdentity.IsValid() || view.authoredGeneration == 0u
			|| (view.surfaceControlIdentity.IsValid() && view.derivedGeneration == 0u)
			|| (view.mapping.layerCount != 0u
				&& (!view.mapping.IsComplete() || view.mapping.area != view.area))) return std::nullopt;
		// A complete cook is still unusable when it was prepared for an older edit.
		// Compare both immutable source values before making a page candidate so a
		// stale worker result can only select already-visible fallback coverage.
		const bool exactCookIdentity = cooked.complete
			&& cooked.assetId == view.assetId
			&& cooked.area == view.area
			&& cooked.authoredHash == view.authoredHash
			&& cooked.authoredGeneration == view.authoredGeneration
			&& cooked.sourceIdentity == view.pageSourceIdentity
			&& (!view.neighborDependencyHash.IsValid() || cooked.neighborDependencyHash == view.neighborDependencyHash)
			&& !cooked.pageRanges.empty()
			&& cooked.pageRanges.front().payloadIdentity == cooked.sourceIdentity;
		TerrainPreparedRenderPacket packet{};
		packet.view = view;
		packet.pick = {view.assetId, view.area, 0u, 0u, view.authoredHash, view.authoredGeneration, view.pageSourceIdentity};
		packet.mapping = view.mapping;
		if (!exactCookIdentity)
		{
			const auto prior = std::find_if(priorVisiblePages.begin(), priorVisiblePages.end(), [&](const auto& page) { return page.IsVisible() && page.key.asset.assetId == view.assetId && page.key.pageId == view.pageId && page.key.pageContentGeneration == view.authoredGeneration && page.key.asset.sourceHash == view.pageSourceIdentity.low64 && page.key.asset.buildRecipeHash == view.pageSourceIdentity.high64; });
			if (prior == priorVisiblePages.end()) return std::nullopt;
			packet.usesLastKnownGood = true;
			packet.view.fallback = true;
			packet.candidate.key = prior->key;
			return packet;
		}
		auto& candidate = packet.candidate;
		candidate.candidateId = (view.frameId << 32u) ^ (static_cast<std::uint64_t>(view.viewId) << 16u) ^ view.pageId;
		if (candidate.candidateId == 0u) candidate.candidateId = 1u;
		candidate.publicationGeneration = view.authoredGeneration;
		candidate.key.asset.assetId = view.assetId;
		candidate.key.asset.sourceHash = cooked.sourceIdentity.low64 == 0u ? 1u : cooked.sourceIdentity.low64;
		candidate.key.asset.buildRecipeHash = cooked.sourceIdentity.high64 == 0u ? 1u : cooked.sourceIdentity.high64;
		candidate.key.generations = {1u, 1u, 1u, view.authoredGeneration, 1u};
		candidate.key.pageId = view.pageId;
		candidate.key.pageContentGeneration = view.authoredGeneration;
		const std::uint64_t integrity = (cooked.sourceIdentity.low64 ^ cooked.sourceIdentity.high64) == 0u ? 1u : (cooked.sourceIdentity.low64 ^ cooked.sourceIdentity.high64);
		candidate.payload = {static_cast<std::uint64_t>(cooked.pagePayload.size()), static_cast<std::uint64_t>(cooked.pagePayload.size()), integrity, true, true};
		candidate.hierarchy.clusterIds = {0u}; candidate.hierarchy.fallbackPageId = view.pageId; candidate.hierarchy.fallbackCoverageVerified = true; candidate.hierarchy.hierarchyAcyclic = true;
		// Worker-side cooking can describe the page payload and hierarchy, but it
		// cannot prove that Vulkan has allocated descriptors or completed a GPU
		// upload. Leave the descriptor intentionally incomplete; the protected RHI
		// upload path creates the real allocation and flips uploadCompleted only
		// after its frame fence has completed.
		candidate.descriptor = {candidate.candidateId, 0u, 0u, false};
		candidate.dependenciesVerified = true;
		candidate.residencyTransitionVerified = true;
		return packet;
	}
}
