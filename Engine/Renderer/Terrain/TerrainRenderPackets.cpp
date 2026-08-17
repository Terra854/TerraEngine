// TerrainRenderPackets.cpp -- Converts cooked immutable terrain data to VG intents.

#include "Renderer/Terrain/TerrainRenderPackets.h"

#include <algorithm>

namespace TerraEngine::Renderer::Terrain
{
	bool TerrainPickIdentity::IsComplete() const noexcept
	{
		return assetId != TerraEngine::EMPTY_UUID && authoredHash.IsValid() && authoredGeneration != 0u
			&& pageSourceIdentity.IsValid() && localCellX < 256u && localCellZ < 256u;
	}

	std::optional<TerrainPreparedRenderPacket> PrepareTerrainRenderPacket(const TerrainViewPacket& view, const TerrainPageCookResult& cooked, const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication>& priorVisiblePages)
	{
		if (view.assetId == TerraEngine::EMPTY_UUID || !view.authoredHash.IsValid() || !view.pageSourceIdentity.IsValid() || view.authoredGeneration == 0u
			|| (view.surfaceControlIdentity.IsValid() && view.derivedGeneration == 0u)) return std::nullopt;
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
		candidate.descriptor = {candidate.candidateId, 0u, 0u, true}; candidate.dependenciesVerified = true; candidate.residencyTransitionVerified = true;
		return packet;
	}
}
