// Virtual geometry selection operates on captured immutable resources and typed packets.
#include "RenderFrameState.h"
#include "RenderVirtualGeometrySelection.h"
#include "Resource/ResourceManager.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Profiling/ProfilerScope.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include <glm/geometric.hpp>

namespace TerraEngine::Renderer::Frame {
	bool GeometryPageCoversLod(const GeometryPageRuntime& page, std::uint32_t lodIndex)
	{
		if (page.lodCount == 0u)
		{
			return false;
		}
		const std::uint64_t first = page.firstLodIndex;
		const std::uint64_t lastExclusive = first + page.lodCount;
		return static_cast<std::uint64_t>(lodIndex) >= first && static_cast<std::uint64_t>(lodIndex) < lastExclusive;
	}

	bool GeometryPageIsResident(const GeometryPageRuntime& page)
	{
		return page.state == GeometryPageState::Resident
			|| page.state == GeometryPageState::Pinned
			|| page.state == GeometryPageState::PendingRetirement;
	}

	const GeometryPageRuntime* FindGeometryPageForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (GeometryPageCoversLod(page, lodIndex))
			{
				return &page;
			}
		}
		return nullptr;
	}

	const GeometryPageRuntime* FindGeometryPageById(const VirtualGeometryAssetRuntime& asset, std::uint32_t pageId)
	{
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.pageId == pageId)
			{
				return &page;
			}
		}
		return nullptr;
	}

	const VirtualGeometryClusterRuntime* FindVirtualGeometryClusterById(const VirtualGeometryAssetRuntime& asset, std::uint32_t clusterId)
	{
		for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
		{
			if (cluster.clusterId == clusterId)
			{
				return &cluster;
			}
		}
		return nullptr;
	}

	bool VirtualGeometryClusterPageIsResident(const VirtualGeometryAssetRuntime& asset, const VirtualGeometryClusterRuntime& cluster)
	{
		if (asset.pages.empty())
		{
			return true;
		}
		const GeometryPageRuntime* page = FindGeometryPageById(asset, cluster.pageId);
		return page != nullptr && GeometryPageIsResident(*page);
	}

	VirtualGeometryPagePayloadSource ResolveVirtualGeometryPayloadSource(const GeometryPageRuntime* page, bool fallbackPacket)
	{
		if (page == nullptr)
		{
			return VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
		}
		if (!GeometryPageIsResident(*page))
		{
			return VirtualGeometryPagePayloadSource::Missing;
		}
			if (fallbackPacket || page->fallbackPage || page->pinnedFallback || page->protectedFallback
				|| page->state == GeometryPageState::Pinned)
		{
			return VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		}
		return VirtualGeometryPagePayloadSource::ResidentPagePayload;
	}

	VirtualGeometryEmittedPageSource ResolveVirtualGeometryEmittedSource(VirtualGeometryPagePayloadSource source)
	{
		switch (source)
		{
		case VirtualGeometryPagePayloadSource::ResidentPagePayload:
			return VirtualGeometryEmittedPageSource::HighDetailPage;
		case VirtualGeometryPagePayloadSource::ResidentFallbackPayload:
			return VirtualGeometryEmittedPageSource::FallbackPage;
		case VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback:
		case VirtualGeometryPagePayloadSource::EmbeddedMeshPayload:
			return VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
		case VirtualGeometryPagePayloadSource::Missing:
		case VirtualGeometryPagePayloadSource::Invalid:
			return VirtualGeometryEmittedPageSource::Invalid;
		case VirtualGeometryPagePayloadSource::Unknown:
			break;
		}
		return VirtualGeometryEmittedPageSource::None;
	}

	std::uint64_t ResolveVirtualGeometryPayloadBytes(const GeometryPageRuntime* page, const VirtualGeometryClusterMeshletRange& meshlets)
	{
		if (page != nullptr)
		{
			return page->payloadByteSize == 0u ? page->byteSize : page->payloadByteSize;
		}
		return static_cast<std::uint64_t>(meshlets.meshletCount);
	}

	std::pair<glm::vec3, glm::vec3> ResolveVirtualGeometryHiZPacketBounds(const VirtualGeometryClusterRuntime& cluster)
	{
		// Page streaming changes payload residency, not the conservative shape
		// tested by Hi-Z. Keep packet bounds tied to the selected cluster so a
		// broad fallback page cannot make the culler believe hidden coverage is
		// larger or smaller than the meshlet range being submitted.
		const glm::vec3 radius(cluster.boundsRadius);
		return {
			cluster.boundsCenter - radius,
			cluster.boundsCenter + radius
		};
	}

	void HashVirtualGeometryPageSourceKey(std::uint64_t& key, std::uint64_t value)
	{
		key ^= value;
		key *= kRenderHashPrime;
	}

	void PopulateVirtualGeometryPageSourceRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state, std::uint64_t frameId)
	{
		// Page payload records describe the resident source that the shader may
		// read. Emitted source records describe the visible result after fallback
		// substitution. Keeping both lets streaming request missing detail while
		// diagnostics still explain why the current frame remained visible.
		std::unordered_set<std::uint64_t> seenPayloads;
		seenPayloads.reserve(state.clusterPackets.size());
		for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
		{
			if (packet.payloadSource == VirtualGeometryPagePayloadSource::Missing
				|| packet.payloadSource == VirtualGeometryPagePayloadSource::Invalid
				|| packet.payloadSource == VirtualGeometryPagePayloadSource::Unknown)
			{
				continue;
			}
			std::uint64_t key = kRenderHashOffset;
			HashVirtualGeometryPageSourceKey(key, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.meshId)));
			HashVirtualGeometryPageSourceKey(key, packet.pageId);
			HashVirtualGeometryPageSourceKey(key, static_cast<std::uint64_t>(packet.payloadSource));
			HashVirtualGeometryPageSourceKey(key, packet.pagePayloadByteOffset);
			HashVirtualGeometryPageSourceKey(key, packet.pagePayloadByteSize);
			HashVirtualGeometryPageSourceKey(key, packet.payloadBufferIndex);
			HashVirtualGeometryPageSourceKey(key, packet.payloadDescriptorIndex);
			if (seenPayloads.insert(key).second)
			{
				VirtualGeometryPagePayloadRecord payload;
				payload.meshId = packet.meshId;
				payload.pageId = packet.pageId;
				payload.fallbackPageId = packet.fallbackPageId;
				payload.source = packet.payloadSource;
				payload.selectedLod = packet.selectedLod;
				payload.meshletOffset = packet.meshletOffset;
				payload.meshletCount = packet.meshletCount;
				payload.payloadByteOffset = packet.pagePayloadByteOffset;
				payload.payloadByteSize = packet.pagePayloadByteSize;
				payload.payloadBufferIndex = packet.payloadBufferIndex;
				payload.payloadDescriptorIndex = packet.payloadDescriptorIndex;
				payload.resident = packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload
					|| packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.pinnedFallback = packet.fallbackPacket || packet.payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				payload.valid = true;
				payload.owningMeshFallbackReason = packet.fallbackReason;
				payload.diagnostic = packet.payloadSource == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
					? "Virtual geometry emitted conservative embedded mesh payload instead of a resident streamed page."
					: std::string();
				state.residentPagePayloads.push_back(std::move(payload));
			}

			VirtualGeometryEmittedPageSourceRecord emitted;
			emitted.viewId = state.outputId;
			emitted.viewType = state.viewType;
			emitted.frameId = frameId;
			emitted.objectId = packet.objectId;
			emitted.meshId = packet.meshId;
			emitted.materialId = packet.materialId;
			emitted.clusterId = packet.clusterId;
			emitted.selectedLod = packet.selectedLod;
			emitted.pageId = packet.pageId;
			emitted.fallbackPageId = packet.fallbackPageId;
			emitted.payloadSource = packet.payloadSource;
			emitted.emittedSource = packet.emittedSource;
			emitted.meshletOffset = packet.meshletOffset;
			emitted.meshletCount = packet.meshletCount;
			emitted.payloadByteOffset = packet.pagePayloadByteOffset;
			emitted.payloadByteSize = packet.pagePayloadByteSize;
			emitted.payloadBufferIndex = packet.payloadBufferIndex;
			emitted.payloadDescriptorIndex = packet.payloadDescriptorIndex;
			emitted.fallbackSubstitution = packet.fallbackPacket;
			emitted.conservativeEmbeddedFallback = packet.payloadSource == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			emitted.reason = packet.fallbackReason.empty() ? ToString(packet.emittedSource) : packet.fallbackReason;
			state.emittedPageSources.push_back(std::move(emitted));
		}
	}

	void PopulateVirtualGeometryPageRequestRecords(TerraEngine::Renderer::VirtualGeometryViewSelectionState& state)
	{
		// Requests are deduplicated per view/object/page before crossing into
		// ResourceStreaming. The renderer never mutates residency here; it only
		// publishes immutable need packets that protected resource code consumes.
		for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : state.pageNeeds)
		{
			if (need.meshId == TerraEngine::EMPTY_UUID)
			{
				continue;
			}

			const auto appendRequest = [&](std::uint32_t pageId, bool fallbackRequest, VirtualGeometryPagePayloadSource source, VirtualGeometryEmittedPageSource emission, std::string reason) {
				if (pageId == std::numeric_limits<std::uint32_t>::max())
				{
					return;
				}
				const auto duplicate = std::find_if(state.pageRequests.begin(), state.pageRequests.end(), [&](const TerraEngine::Renderer::VirtualGeometryPageRequestRecord& existing) {
					return existing.meshId == need.meshId
						&& existing.objectId == need.objectId
						&& existing.pageId == pageId
						&& existing.fallbackRequest == fallbackRequest;
				});
				if (duplicate != state.pageRequests.end())
				{
					duplicate->priority = std::max(duplicate->priority, static_cast<std::int32_t>(need.screenImportance));
					return;
				}

				TerraEngine::Renderer::VirtualGeometryPageRequestRecord request;
				request.meshId = need.meshId;
				request.objectId = need.objectId;
				request.pageId = pageId;
				request.fallbackPageId = need.fallbackPageId;
				request.clusterId = need.clusterId;
				request.desiredLodIndex = need.desiredLodIndex;
				request.priority = static_cast<std::int32_t>(std::clamp(need.screenImportance, 0.0f, 100000.0f));
				request.screenImportance = need.screenImportance;
				request.fallbackRequired = need.fallbackRequired;
				request.fallbackRequest = fallbackRequest;
				request.payloadSource = source;
				request.expectedEmission = emission;
				request.reason = std::move(reason);
				state.pageRequests.push_back(std::move(request));
			};

			if (need.desiredPayloadSource == VirtualGeometryPagePayloadSource::Missing
				|| need.desiredPayloadSource == VirtualGeometryPagePayloadSource::Unknown)
			{
				appendRequest(
					need.pageId,
					false,
					need.desiredPayloadSource,
					VirtualGeometryEmittedPageSource::HighDetailPage,
					"visible virtual geometry cluster requested a non-resident high-detail page");
			}
			if (need.fallbackRequired
				&& (need.fallbackPayloadSource == VirtualGeometryPagePayloadSource::Missing
					|| need.fallbackPayloadSource == VirtualGeometryPagePayloadSource::Unknown))
			{
				appendRequest(
					need.fallbackPageId,
					true,
					need.fallbackPayloadSource,
					VirtualGeometryEmittedPageSource::FallbackPage,
					"visible virtual geometry cluster requested fallback coverage");
			}
		}
	}

	VirtualGeometryClusterMeshletRange ResolveVirtualGeometryClusterMeshletRange(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryClusterRuntime& cluster)
	{
		VirtualGeometryClusterMeshletRange range{};
		if (cluster.groupCount == 0u || cluster.firstGroupIndex >= asset.cullableGroups.size())
		{
			return range;
		}

		const std::uint32_t firstGroup = cluster.firstGroupIndex;
		const std::uint32_t lastGroupExclusive = std::min<std::uint32_t>(
			static_cast<std::uint32_t>(asset.cullableGroups.size()),
			firstGroup + cluster.groupCount);
		std::uint32_t minMeshlet = std::numeric_limits<std::uint32_t>::max();
		std::uint32_t maxMeshlet = 0u;
		for (std::uint32_t groupIndex = firstGroup; groupIndex < lastGroupExclusive; ++groupIndex)
		{
			const VirtualGeometryCullableGroupRuntime& group = asset.cullableGroups[groupIndex];
			minMeshlet = std::min(minMeshlet, group.meshletIndex);
			maxMeshlet = std::max(maxMeshlet, group.meshletIndex);
		}

		if (minMeshlet == std::numeric_limits<std::uint32_t>::max() || maxMeshlet < minMeshlet)
		{
			return range;
		}

		// The current task shader accepts one contiguous meshlet range per draw.
		// If a future page layout stores non-contiguous group coverage, dispatch
		// the conservative envelope instead of dropping visible geometry; the
		// selected-cluster packet still records the sparse identity for tooling.
		range.meshletOffset = minMeshlet;
		range.meshletCount = (maxMeshlet - minMeshlet) + 1u;
		range.valid = true;
		return range;
	}

	std::size_t DetailIndexForLod(const VirtualGeometryAssetRuntime& asset, std::uint32_t lodIndex)
	{
		for (std::size_t index = 0u; index < asset.detailLevels.size(); ++index)
		{
			if (asset.detailLevels[index].lodIndex == lodIndex)
			{
				return index;
			}
		}
		return 0u;
	}

	std::size_t ResolveResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::size_t desiredIndex,
		std::string& outFallbackReason)
	{
		if (asset.pages.empty() || desiredIndex >= asset.detailLevels.size())
		{
			return desiredIndex;
		}

		const std::uint32_t desiredLod = asset.detailLevels[desiredIndex].lodIndex;
		const GeometryPageRuntime* desiredPage = FindGeometryPageForLod(asset, desiredLod);
		if (desiredPage != nullptr && GeometryPageIsResident(*desiredPage))
		{
			return desiredIndex;
		}

		const std::uint32_t fallbackPageId = desiredPage != nullptr ? desiredPage->fallbackPageId : 0u;
		if (const GeometryPageRuntime* fallbackPage = FindGeometryPageById(asset, fallbackPageId);
			fallbackPage != nullptr && GeometryPageIsResident(*fallbackPage))
		{
			// Missing high-detail pages must fail open to a resident fallback.
			// Returning the fallback detail here preserves visibility while the
			// streaming system catches up or reports a page failure.
			outFallbackReason = "desired virtual geometry page is not resident; using resident fallback page";
			return DetailIndexForLod(asset, fallbackPage->firstLodIndex);
		}

		const GeometryPageRuntime* bestResidentFallback = nullptr;
		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (!GeometryPageIsResident(page) || page.firstLodIndex > desiredLod)
			{
				continue;
			}
			if (bestResidentFallback == nullptr || page.firstLodIndex > bestResidentFallback->firstLodIndex)
			{
				bestResidentFallback = &page;
			}
		}
		if (bestResidentFallback != nullptr)
		{
			outFallbackReason = "desired virtual geometry page is not resident; using best resident fallback page";
			return DetailIndexForLod(asset, bestResidentFallback->firstLodIndex);
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "desired virtual geometry page is not resident; using first resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "desired virtual geometry page is missing and no resident fallback page is published";
		return 0u;
	}

	std::size_t ResolveBestResidentVirtualGeometryDetailIndex(
		const VirtualGeometryAssetRuntime& asset,
		std::string& outFallbackReason)
	{
		if (asset.detailLevels.empty() || asset.pages.empty())
		{
			return 0u;
		}

		// Walk details in cooked order so the highest usable detail wins when
		// virtual geometry is disabled for debugging but resident page metadata
		// is still available. This avoids selecting a non-resident high-detail
		// range that would immediately need another fallback later.
		for (std::size_t detailIndex = 0u; detailIndex < asset.detailLevels.size(); ++detailIndex)
		{
			const std::uint32_t lodIndex = asset.detailLevels[detailIndex].lodIndex;
			if (const GeometryPageRuntime* page = FindGeometryPageForLod(asset, lodIndex);
				page != nullptr && GeometryPageIsResident(*page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using best resident detail";
				return detailIndex;
			}
		}

		for (const GeometryPageRuntime& page : asset.pages)
		{
			if (page.fallbackPage && GeometryPageIsResident(page))
			{
				outFallbackReason = "virtual geometry disabled by quality profile; using resident fallback page";
				return DetailIndexForLod(asset, page.firstLodIndex);
			}
		}

		outFallbackReason = "virtual geometry disabled by quality profile; no resident fallback page is published";
		return 0u;
	}

	float EstimateProjectedMeshRadiusPixels(const Mesh& mesh, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize)
	{
		if (!mesh.hasLocalBounds || targetSize.x == 0u || targetSize.y == 0u)
		{
			return 512.0f;
		}

		const glm::vec3 localMin(mesh.localBoundsMin[0], mesh.localBoundsMin[1], mesh.localBoundsMin[2]);
		const glm::vec3 localMax(mesh.localBoundsMax[0], mesh.localBoundsMax[1], mesh.localBoundsMax[2]);
		const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
		const float localRadius = glm::length(localMax - localMin) * 0.5f;
		const glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(localCenter, 1.0f));
		const float worldScale = std::max(
			glm::length(glm::vec3(modelMatrix[0])),
			std::max(glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))));
		const float distance = std::max(glm::length(worldCenter - cameraPosition), 1e-3f);
		const float viewportMax = static_cast<float>(std::max(targetSize.x, targetSize.y));
		const float projectedRadius = (localRadius * std::max(worldScale, 1e-6f) / distance) * viewportMax;
		return std::isfinite(projectedRadius) ? projectedRadius : 512.0f;
	}

	float EstimateProjectedClusterRadiusPixels(const VirtualGeometryClusterRuntime& cluster, const glm::mat4& modelMatrix, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, float fallbackProjectedRadiusPixels)
	{
		// Cluster LOD is decided from the cluster's own conservative sphere.
		// When tests call the legacy overload without camera data, fall back to
		// the already-computed object radius so selection remains deterministic.
		if (targetSize.x == 0u || targetSize.y == 0u || cluster.boundsRadius <= 0.0f)
		{
			return fallbackProjectedRadiusPixels;
		}

		const glm::vec3 worldCenter = glm::vec3(modelMatrix * glm::vec4(cluster.boundsCenter, 1.0f));
		const float worldScale = std::max(
			glm::length(glm::vec3(modelMatrix[0])),
			std::max(glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))));
		const float distance = std::max(glm::length(worldCenter - cameraPosition), 1e-3f);
		const float viewportMax = static_cast<float>(std::max(targetSize.x, targetSize.y));
		const float projectedRadius = (cluster.boundsRadius * std::max(worldScale, 1e-6f) / distance) * viewportMax;
		return std::isfinite(projectedRadius) ? projectedRadius : fallbackProjectedRadiusPixels;
	}

	std::uint64_t BuildVirtualGeometryTransitionKey(
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId,
		const glm::uvec2& targetSize,
		const TerraEngine::UUID& objectId,
		const TerraEngine::UUID& meshId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		HashCombine(seed, (static_cast<std::uint64_t>(targetSize.x) << 32u)
			| targetSize.y);
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(objectId)));
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		return seed;
	}

	std::uint64_t BuildVirtualGeometryDebugModeKey(RenderViewType viewType, std::uint32_t outputId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		return seed;
	}

	std::uint64_t BuildVirtualGeometrySelectionSignature(const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult& selection)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, selection.range.selectedLodIndex);
		HashCombine(seed, selection.range.meshletOffset);
		HashCombine(seed, selection.range.meshletCount);
		HashCombine(seed, selection.range.virtualGeometryActive ? 1u : 0u);
		HashCombine(seed, selection.range.failOpen ? 1u : 0u);
		HashCombine(seed, selection.range.pageId);
		HashCombine(seed, selection.range.fallbackPageId);
		HashCombine(seed, static_cast<std::uint64_t>(selection.range.payloadSource));
		HashCombine(seed, static_cast<std::uint64_t>(selection.range.emittedSource));
		HashCombine(seed, selection.range.pagePayloadByteOffset);
		HashCombine(seed, selection.range.pagePayloadByteSize);
		HashCombine(seed, selection.invalidSelectionReasons);
		HashCombine(seed, static_cast<std::uint64_t>(selection.selectedClusters.size()));
		for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& cluster : selection.selectedClusters)
		{
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(cluster.objectId)));
			HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(cluster.meshId)));
			HashCombine(seed, cluster.clusterId);
			HashCombine(seed, cluster.lodIndex);
			HashCombine(seed, cluster.pageId);
			HashCombine(seed, cluster.fallbackClusterId);
			HashCombine(seed, cluster.usingFallback ? 1u : 0u);
		}
		HashCombine(seed, static_cast<std::uint64_t>(selection.pageNeeds.size()));
		for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : selection.pageNeeds)
		{
			HashCombine(seed, need.pageId);
			HashCombine(seed, need.clusterId);
			HashCombine(seed, need.desiredLodIndex);
			HashCombine(seed, need.fallbackRequired ? 1u : 0u);
		}
		return seed;
	}

	VirtualGeometryTransitionMetrics EvaluateVirtualGeometryTransitionMetrics(
		const VirtualGeometryAssetRuntime& asset,
		const VirtualGeometryQualityProfile& quality,
		float projectedRadiusPixels,
		const glm::mat4& modelMatrix,
		const glm::vec3& cameraPosition,
		const glm::uvec2& targetSize)
	{
		VirtualGeometryTransitionMetrics metrics;
		const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
			? std::max(projectedRadiusPixels, 0.0f)
			: 512.0f;
		for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
		{
			if (cluster.childCount == 0u)
			{
				continue;
			}
			const float clusterProjectedRadiusPixels = EstimateProjectedClusterRadiusPixels(cluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels);
			const float errorPixels = clusterProjectedRadiusPixels
				* std::max(cluster.geometricError, 0.0f)
				* quality.lodErrorScale;
			if (!std::isfinite(errorPixels))
			{
				continue;
			}
			metrics.candidateMaxErrorPixels = std::max(metrics.candidateMaxErrorPixels, errorPixels);
			metrics.transitionMarginPixels = std::min(metrics.transitionMarginPixels, std::abs(errorPixels - kVirtualGeometryClusterRefinementErrorPixels));
			++metrics.transitionCandidateCount;
		}
		if (metrics.transitionCandidateCount == 0u)
		{
			metrics.transitionMarginPixels = 0.0f;
		}
		return metrics;
	}

	std::uint64_t BuildVirtualGeometryTransitionStateSignature(
		const std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		std::uint32_t outputId)
	{
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(outputKind));
		HashCombine(seed, static_cast<std::uint64_t>(viewType));
		HashCombine(seed, outputId);
		for (const auto& [key, selection] : transitionCache)
		{
			const VirtualGeometryLODTransitionState& state = selection.transitionState;
			if (!state.valid || state.viewType != viewType || state.outputId != outputId)
			{
				continue;
			}
			HashCombine(seed, key);
			HashCombine(seed, state.lastStableSelectionSignature);
			HashCombine(seed, state.candidateSelectionSignature);
			HashCombine(seed, state.stableFrameCount);
			HashCombine(seed, state.heldFrameCount);
			HashCombine(seed, state.heldPreviousSelection ? 1u : 0u);
			HashCombine(seed, state.resetThisFrame ? 1u : 0u);
		}
		return seed;
	}

	void PruneStaleVirtualGeometryTransitionCache(
		std::unordered_map<std::uint64_t, TerraEngine::Renderer::VirtualGeometryClusterSelectionResult>& transitionCache,
		const std::unordered_set<TerraEngine::UUID>& aliveObjectIds)
	{
		for (auto iterator = transitionCache.begin(); iterator != transitionCache.end();)
		{
			const VirtualGeometryLODTransitionState& state = iterator->second.transitionState;
			if (!state.valid || !aliveObjectIds.contains(state.objectId))
			{
				iterator = transitionCache.erase(iterator);
			}
			else
			{
				++iterator;
			}
		}
	}
}

// This selection operation borrows captured resources and mutates only typed frame-owned outputs.
void RenderSystem::CollectFrameVirtualGeometryDiagnostics(const FrameVirtualGeometryInputs& inputs,
	FrameVirtualGeometryPreparation& preparation, const std::vector<RenderItemRecord>& commands, RenderViewType viewType)
{
	auto& frameMeshResources = inputs.frameMeshResources;
	auto& geometryInputs = inputs.geometryInputs;
	auto& gameVirtualGeometryDiagnosticsAssets = preparation.gameVirtualGeometryDiagnosticsAssets;
	auto& sceneVirtualGeometryDiagnosticsAssets = preparation.sceneVirtualGeometryDiagnosticsAssets;
	auto& shadowVirtualGeometryDiagnosticsAssets = preparation.shadowVirtualGeometryDiagnosticsAssets;

	std::vector<VirtualGeometryAssetRuntime>& target = viewType == RenderViewType::Scene
		? sceneVirtualGeometryDiagnosticsAssets
		: (viewType == RenderViewType::Shadow ? shadowVirtualGeometryDiagnosticsAssets : gameVirtualGeometryDiagnosticsAssets);
	std::unordered_set<TerraEngine::UUID> seenMeshes;
	for (const RenderItemRecord& command : commands)
	{
		if (seenMeshes.contains(command.meshId))
		{
			continue;
		}
		const auto capturedMesh = frameMeshResources.find(command.meshId);
		if (const Mesh* mesh = capturedMesh != frameMeshResources.end()
				? capturedMesh->second : nullptr;
			mesh != nullptr && mesh->hasVirtualGeometry)
		{
			target.push_back(geometryInputs.Runtime(command.meshId, *mesh));
			seenMeshes.insert(command.meshId);
		}
	}

}

// This selection operation borrows captured resources and mutates only typed frame-owned outputs.
void RenderSystem::RecordFrameVirtualGeometrySelection(const FrameVirtualGeometryInputs& inputs,
	FrameVirtualGeometryPreparation& preparation, const std::vector<RenderItemRecord>& commands,
		const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
		const std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord>& pageNeeds,
		const std::vector<VirtualGeometryClusterPacket>& clusterPackets,
		const std::vector<ClusterWorkBatch>& clusterBatches,
		const std::vector<CompactedVisibilitySet>& compactionResults,
		const std::vector<FallbackCoverageChain>& fallbackCoverage,
		const std::vector<VirtualGeometryLODTransitionState>& lodTransitionStates,
		VirtualGeometryDebugVisualizationMode debugVisualizationMode,
		RenderOutputKind outputKind,
		RenderViewType viewType,
		const glm::uvec2& targetSize,
		std::uint32_t outputId,
		bool shadowPass,
		bool exportRuntime,
		const std::vector<std::uint32_t>* virtualGeometryCommandIndices,
		bool reuseValidatedSelection,
		const TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket* preparedSelection,
		const TerraEngine::Renderer::Frame::VisibleCommandCacheRecord* cachedSelectionEvidence)
{
	auto& renderFrameSnapshot = inputs.renderFrameSnapshot;
	auto& capturedVirtualGeometryResidencyGeneration = inputs.capturedVirtualGeometryResidencyGeneration;
	auto& activeVirtualGeometryViewPolicyKeys = preparation.activeVirtualGeometryViewPolicyKeys;
	auto& activeVirtualGeometryDebugModeKeys = preparation.activeVirtualGeometryDebugModeKeys;

	TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
	state.outputKind = outputKind;
	state.viewType = viewType;
	state.targetSize = targetSize;
	state.outputId = outputId;
	state.debugVisualizationMode = NormalizeVirtualGeometryDebugVisualizationMode(debugVisualizationMode);
	state.shadowPass = shadowPass;
	state.exportRuntime = exportRuntime;
	state.viewGeneration = activeRenderFrameId;
	const auto makeViewKind = [shadowPass, outputKind]() {
		if(shadowPass) return TerraEngine::Renderer::VirtualGeometryViewKind::Shadow;
		if(outputKind == RenderOutputKind::SceneView) return TerraEngine::Renderer::VirtualGeometryViewKind::Editor;
		if(outputKind == RenderOutputKind::OffscreenTexture) return TerraEngine::Renderer::VirtualGeometryViewKind::Capture;
		return TerraEngine::Renderer::VirtualGeometryViewKind::Camera;
	};
	const std::uint64_t policyKey = (static_cast<std::uint64_t>(
		static_cast<std::uint32_t>(viewType)) << 32u) | outputId;
	activeVirtualGeometryViewPolicyKeys.insert(policyKey);
	activeVirtualGeometryDebugModeKeys.insert(
		BuildVirtualGeometryDebugModeKey(viewType, outputId));
	TerraEngine::Renderer::VirtualGeometryViewPolicyState policyState{};
	policyState.kind = makeViewKind();
	policyState.viewId = policyKey;
	policyState.targetGeneration = (static_cast<std::uint64_t>(targetSize.x) << 32u)
		| targetSize.y;
	policyState.projectionGeneration = policyState.targetGeneration;
	// Geometry selection history must not span a reloaded mesh payload even
	// when the renderer session itself remains alive.
	policyState.geometryGeneration = renderFrameSnapshot.resourceRevision;
	policyState.residencyGeneration =
		capturedVirtualGeometryResidencyGeneration;
	if(const auto previous = virtualGeometryViewPolicies.find(policyKey);
		previous != virtualGeometryViewPolicies.end())
	{
		policyState.resized = previous->second.targetGeneration != policyState.targetGeneration;
	}
	TerraEngine::Renderer::VirtualGeometryViewPolicyState historyOwner = policyState;
	if(policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Shadow
		|| policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Capture)
	{
		const auto compatibleOwner = std::find_if(
			virtualGeometryViewPolicies.begin(), virtualGeometryViewPolicies.end(),
			[&policyState](const auto &entry) {
				return (entry.second.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Camera
					|| entry.second.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Editor)
					&& entry.second.geometryGeneration == policyState.geometryGeneration
					&& entry.second.residencyGeneration == policyState.residencyGeneration;
			});
		if(compatibleOwner != virtualGeometryViewPolicies.end())
		{
			historyOwner = compatibleOwner->second;
		}
	}
	policyState.historyOwnerViewId = policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Camera
		|| policyState.kind == TerraEngine::Renderer::VirtualGeometryViewKind::Editor
		? policyState.viewId : historyOwner.viewId;
	const TerraEngine::Renderer::VirtualGeometryViewPolicyDecision viewPolicy =
		TerraEngine::Renderer::EvaluateVirtualGeometryViewPolicy(policyState, historyOwner);
	virtualGeometryViewPolicies[policyKey] = policyState;
	// Geometry/residency reuse is allowed only as immutable inputs. Capture and
	// shadow views deliberately retain a main-pass/fail-open visibility policy
	// until they own a compatible history themselves.
	state.hiZOcclusionAllowed = viewPolicy.historyValid;
	state.hiZInactiveReason.clear();
	if(!viewPolicy.historyValid && !shadowPass)
	{
		state.hiZInactiveReason = "View-owned virtual-geometry history is not reusable; visibility remains conservative.";
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy VG Selection Inputs",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(cachedSelectionEvidence != nullptr
			&& cachedSelectionEvidence->retainedSelectionEvidence)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached VG Selection Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.retainedSelectionEvidence =
				cachedSelectionEvidence->retainedSelectionEvidence;
		}
		else
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Copy Unretained VG Selection Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.selectedClusters = selectedClusters;
			state.pageNeeds = pageNeeds;
			state.clusterPackets = clusterPackets;
			state.clusterBatches = clusterBatches;
			state.compactionResults = compactionResults;
			state.fallbackCoverage = fallbackCoverage;
			state.lodTransitionStates = lodTransitionStates;
		}
		if(preparedSelection != nullptr)
		{
			// Fresh selection already built and validated these current-frame
			// records. The view snapshot needs its own immutable copies, but must not
			// rebuild and revalidate the same packet graph a second time.
			state.gpuPacketRecords = preparedSelection->gpuPacketRecords;
			state.gpuBatchDescriptors = preparedSelection->gpuBatchDescriptors;
			state.gpuPacketFrameSets = preparedSelection->gpuPacketFrameSets;
			state.gpuCompactionDispatches = preparedSelection->gpuCompactionDispatches;
			state.generatedSubmissions = preparedSelection->generatedSubmissions;
			state.gpuCounterReadbacks = preparedSelection->gpuCounterReadbacks;
			state.gpuFallbackRecords = preparedSelection->gpuFallbackRecords;
		}
		else if(cachedSelectionEvidence != nullptr)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Reference Cached VG GPU Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// The cache signature validates every input that shapes these immutable
			// tables. The enclosing view carries the current publication identity,
			// so retaining the table owner avoids two diagnostic-only deep copies.
			state.retainedGpuEvidence = cachedSelectionEvidence->retainedGpuEvidence;
		}
	}
	const auto appendVirtualGeometryDraw = [&](const RenderItemRecord& command) {
		TerraEngine::Renderer::VirtualGeometryDrawRange range{};
		range.selectedLodIndex = command.selectedVirtualLod;
		range.meshletOffset = command.virtualMeshletOffset;
		range.meshletCount = command.virtualMeshletCount;
		range.virtualGeometryActive = (command.virtualGeometryFlags & kVirtualGeometryFlagActive) != 0u;
		range.failOpen = (command.virtualGeometryFlags & kVirtualGeometryFlagFailOpen) != 0u;
		range.hiZSelfOcclusionGuardPassed = true;
		range.hiZNearPlaneGuardPassed = true;
		range.hiZThinGeometryGuardPassed = true;
		range.selectedClusterOffset = command.selectedVirtualClusterOffset;
		range.selectedClusterCount = command.selectedVirtualClusterCount;
		range.pageNeedOffset = command.selectedVirtualPageNeedOffset;
		range.pageNeedCount = command.selectedVirtualPageNeedCount;
		range.pageId = command.virtualGeometryPageId;
		range.fallbackPageId = command.virtualGeometryFallbackPageId;
		range.payloadSource = command.virtualGeometryPayloadSource;
		range.emittedSource = command.virtualGeometryEmittedSource;
		range.pagePayloadByteOffset = command.virtualGeometryPagePayloadByteOffset;
		range.pagePayloadByteSize = command.virtualGeometryPagePayloadByteSize;
		range.payloadBufferIndex = command.virtualGeometryPayloadBufferIndex;
		range.payloadDescriptorIndex = command.virtualGeometryPayloadDescriptorIndex;
		state.hiZSelfOcclusionGuardPassed = state.hiZSelfOcclusionGuardPassed && range.hiZSelfOcclusionGuardPassed;
		state.hiZNearPlaneGuardPassed = state.hiZNearPlaneGuardPassed && range.hiZNearPlaneGuardPassed;
		state.hiZThinGeometryGuardPassed = state.hiZThinGeometryGuardPassed && range.hiZThinGeometryGuardPassed;
		if (state.hiZInactiveReason.empty() && !range.hiZInactiveReason.empty())
		{
			state.hiZInactiveReason = range.hiZInactiveReason;
		}
		if (range.failOpen)
		{
			range.fallbackReason = shadowPass
				? "shadow virtual geometry kept conservative visibility"
				: "view virtual geometry failed open; see fallback diagnostics";
		}
		if (!range.virtualGeometryActive && range.meshletCount == 0u && !range.failOpen)
		{
			return;
		}
		state.draws.push_back({
			command.objectId,
			command.meshId,
			std::move(range),
			command.taskShaderCullingEnabled,
			command.conventionalVisibilityIdentityIndex == 0u
				? 0xffffffffu
				: command.conventionalVisibilityIdentityIndex - 1u
		});
	};
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG View Draw Records",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if(cachedSelectionEvidence != nullptr
			&& cachedSelectionEvidence->hasViewDrawEvidence
			&& cachedSelectionEvidence->viewDraws)
		{
			state.retainedDraws = cachedSelectionEvidence->viewDraws;
		}
		else
		{
			// A cache hit already owns the exact contributing command indices. Reserve
			// their upper bound once so dense crowds do not repeatedly relocate records
			// while rebuilding the current frame's immutable view publication.
			state.draws.reserve(virtualGeometryCommandIndices != nullptr
				? virtualGeometryCommandIndices->size() : commands.size());
			if(virtualGeometryCommandIndices != nullptr) {
				for(const std::uint32_t commandIndex : *virtualGeometryCommandIndices) {
					if(commandIndex < commands.size()) {
						appendVirtualGeometryDraw(commands[commandIndex]);
					}
				}
			} else {
				for(const RenderItemRecord& command : commands) {
					appendVirtualGeometryDraw(command);
				}
			}
		}
	}
	state.hasHiZValidationState = true;
	state.hiZValidationState = {};
	state.hiZValidationState.renderSize = targetSize;
	state.hiZValidationState.depthSceneSignature = static_cast<std::uint64_t>(selectedClusters.size())
		^ (static_cast<std::uint64_t>(pageNeeds.size()) << 32u)
		^ static_cast<std::uint64_t>(state.DrawRecords().size());
	FinalizeFrameVirtualGeometryEvidence(state, cachedSelectionEvidence, preparedSelection, reuseValidatedSelection);
		renderFrameSnapshot.virtualGeometryViewSelections.push_back(std::move(state));

}

// This selection operation borrows captured resources and mutates only typed frame-owned outputs.
TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket RenderSystem::ApplyFrameVirtualGeometrySelection(const FrameVirtualGeometryInputs& inputs,
	FrameVirtualGeometryPreparation& preparation, std::vector<RenderItemRecord>& commands, const glm::mat4& viewProjection, RenderOutputKind outputKind, RenderViewType viewType, std::uint32_t outputId, const glm::vec3& cameraPosition, const glm::uvec2& targetSize, const VirtualGeometryQualityProfile& viewVirtualGeometryQuality)
{
	const auto& frameMaterialResources = inputs.frameMaterialResources;
	const auto& capturedDefaultShaderCollectionId = inputs.resourceCohort.capturedDefaultShaderCollectionId;
	const auto& capturedDefaultTaskShaderCollectionId = inputs.resourceCohort.capturedDefaultTaskShaderCollectionId;
	auto& frameMeshResources = inputs.frameMeshResources;
	auto& geometryInputs = inputs.geometryInputs;
	auto& renderGraphDiagnostics = inputs.renderGraphDiagnostics;
	auto& hardwareVirtualGeometryRouteAvailable = inputs.backendCapture.hardwareVirtualGeometryRouteAvailable;
	auto& computeVirtualGeometryRouteAvailable = inputs.backendCapture.computeVirtualGeometryRouteAvailable;
	auto& validationRouteOverride = inputs.backendCapture.validationRouteOverride;
	auto& hybridValidationSceneRequested = inputs.backendCapture.hybridValidationSceneRequested;
	auto& deviceAuthoredVirtualGeometryAvailable = inputs.backendCapture.deviceAuthoredVirtualGeometryAvailable;
	auto& activeVirtualGeometryTransitionKeys = preparation.activeVirtualGeometryTransitionKeys;

	VirtualGeometryViewSelectionPacket packet;
	std::vector<RenderItemRecord> expandedCommands;
	expandedCommands.reserve(commands.size());
	for (RenderItemRecord& command : commands)
	{
		// Visualizer ownership belongs to the view, not the geometry route. Stamp
		// the selected mode before routing so low-poly direct draws, fail-open
		// fallbacks, and adaptive virtual geometry all reach Vulkan consistently.
		const std::uint32_t debugMode =
			static_cast<std::uint32_t>(viewVirtualGeometryQuality.debugVisualizationMode)
			& kVirtualGeometryDebugModeMask;
		command.virtualGeometryFlags &=
			~(kVirtualGeometryDebugModeMask << kVirtualGeometryDebugModeShift);
		command.virtualGeometryFlags |= debugMode << kVirtualGeometryDebugModeShift;
		const auto capturedMesh = frameMeshResources.find(command.meshId);
		const Mesh* mesh = capturedMesh != frameMeshResources.end()
			? capturedMesh->second : nullptr;
		if (mesh != nullptr
			&& !mesh->hasVirtualGeometry
			&& command.geometryRoute == TerraEngine::Renderer::GeometryRoute::DirectMeshlet
			&& !mesh->virtualGeometryDetailLevels.empty())
		{
			// Low-poly assets select one compact owner-mesh range per camera. This is
			// conventional resident work: no virtual flags, packets, feedback, or
			// page requests are manufactured for the direct path.
			const VirtualGeometryAssetRuntime& runtime =
				geometryInputs.Runtime(command.meshId, *mesh);
			const float projectedRadiusPixels = EstimateProjectedMeshRadiusPixels(
				*mesh,
				command.modelMatrix,
				cameraPosition,
				targetSize);
			const TerraEngine::Renderer::VirtualGeometryDrawRange range =
				RenderSystem::SelectDirectResidentLodRange(
					runtime,
					viewVirtualGeometryQuality,
					projectedRadiusPixels);
			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			expandedCommands.push_back(command);
			continue;
		}
		if (mesh == nullptr || !mesh->hasVirtualGeometry
			|| !command.UsesVirtualGeometryRoute())
		{
			// Conventional low-poly work remains an ordinary indexed draw.
			// Do not synthesize virtual cluster/page metadata for it.
			expandedCommands.push_back(command);
			continue;
		}

		const VirtualGeometryAssetRuntime& runtime =
			geometryInputs.Runtime(command.meshId, *mesh);
		const float projectedRadiusPixels = EstimateProjectedMeshRadiusPixels(*mesh, command.modelMatrix, cameraPosition, targetSize);
		const std::uint64_t transitionKey = BuildVirtualGeometryTransitionKey(
			outputKind, viewType, outputId, targetSize,
			command.objectId, command.meshId);
		activeVirtualGeometryTransitionKeys.insert(transitionKey);
		const auto previousTransition = virtualGeometryTransitionCache.find(transitionKey);
		const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult* previousSelection =
			previousTransition != virtualGeometryTransitionCache.end()
				? &previousTransition->second
				: nullptr;
		TerraEngine::Renderer::VirtualGeometryClusterSelectionResult selection = SelectVirtualGeometryStableClusterCut(
			runtime,
			command.meshId,
			command.objectId,
			viewVirtualGeometryQuality,
			projectedRadiusPixels,
			command.modelMatrix,
			cameraPosition,
			targetSize,
			command.taskShaderCullingEnabled && taskShaderCullingEnabled,
			!mesh->meshletBounds.empty(),
			previousSelection,
			viewType,
			outputId);
		virtualGeometryTransitionCache[transitionKey] = selection;
		if (selection.transitionState.valid)
		{
			packet.lodTransitionStates.push_back(selection.transitionState);
		}
		TerraEngine::Renderer::VirtualGeometryDrawRange range = selection.range;
		range.selectedClusterOffset = static_cast<std::uint32_t>(packet.selectedClusters.size());
		range.pageNeedOffset = static_cast<std::uint32_t>(packet.pageNeeds.size());
		range.selectedClusterCount = static_cast<std::uint32_t>(selection.selectedClusters.size());
		range.pageNeedCount = static_cast<std::uint32_t>(selection.pageNeeds.size());
		std::string coneDisabledReason;
		const bool coneCullingSafe = AreVirtualGeometryClustersConeCullingEligible(runtime, selection.selectedClusters, &coneDisabledReason);
		const std::uint32_t virtualGeometryFlags = (range.virtualGeometryActive ? kVirtualGeometryFlagActive : 0u)
			| (range.failOpen ? kVirtualGeometryFlagFailOpen : 0u)
			| (range.hiZSelfOcclusionGuardPassed ? kVirtualGeometryFlagHiZSelfOcclusionGuard : 0u)
			| (range.hiZNearPlaneGuardPassed ? kVirtualGeometryFlagHiZNearPlaneGuard : 0u)
			| (range.hiZThinGeometryGuardPassed ? kVirtualGeometryFlagHiZThinGeometryGuard : 0u)
			| (coneCullingSafe ? kVirtualGeometryFlagConeCullingSafe : 0u)
			// Processor-expanded VG still keeps the selected LOD range, but its
			// animated vertices must use the proven owner palette bridge until
			// device-authored page work has completed on this backend.
			| (command.animationBinding.RequiresConservativeCoarseVisibility()
				&& !deviceAuthoredVirtualGeometryAvailable
				? kVirtualGeometryFlagOwnerPaletteDeformation : 0u)
			| (debugMode << kVirtualGeometryDebugModeShift);
		if (!coneCullingSafe && TerraEngine::Renderer::RenderDiagnosticsRequested() && !coneDisabledReason.empty())
		{
			renderGraphDiagnostics.backendFallbacks.push_back("Virtual geometry cone culling disabled: " + coneDisabledReason);
		}
		if (range.failOpen && TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			TerraEngine::Renderer::AddVirtualGeometryFailOpenDiagnostic(renderGraphDiagnostics, command.meshId, range);
		}
		std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> annotatedPageNeeds = selection.pageNeeds;

		if (command.animationBinding.RequiresOwnerMeshDeformationFallback())
		{
			// Legacy/unsupported pages have no accepted compute publication.
			// Do not reinterpret their page-local cluster range as an owner
			// range; retain complete conservative coverage instead.
			command.virtualMeshletOffset = 0U;
			command.virtualMeshletCount = mesh->runtimeLayout.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			command.selectedVirtualClusterOffset = range.selectedClusterOffset;
			command.selectedVirtualClusterCount = range.selectedClusterCount;
			command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
			command.selectedVirtualPageNeedCount = range.pageNeedCount;
			command.virtualGeometryPageId = range.pageId;
			command.virtualGeometryFallbackPageId = range.fallbackPageId;
			command.virtualGeometryPayloadSource =
				VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			command.virtualGeometryEmittedSource =
				VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
			command.virtualGeometryPagePayloadByteOffset = 0U;
			command.virtualGeometryPagePayloadByteSize = 0U;
			command.virtualGeometryPayloadBufferIndex = 0xffffffffU;
			command.virtualGeometryPayloadDescriptorIndex = 0xffffffffU;
			command.virtualGeometryPacketDescriptorIndex = 0xffffffffU;
			command.virtualGeometryFlags =
				(virtualGeometryFlags | kVirtualGeometryFlagFailOpen)
				& ~kVirtualGeometryFlagConeCullingSafe;
			expandedCommands.push_back(command);
			packet.selectedClusters.insert(
				packet.selectedClusters.end(),
				selection.selectedClusters.begin(),
				selection.selectedClusters.end());
			packet.pageNeeds.insert(
				packet.pageNeeds.end(),
				annotatedPageNeeds.begin(),
				annotatedPageNeeds.end());
			continue;
		}

		if (selection.selectedClusters.empty())
		{
			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			command.selectedVirtualClusterOffset = range.selectedClusterOffset;
			command.selectedVirtualClusterCount = range.selectedClusterCount;
			command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
			command.selectedVirtualPageNeedCount = range.pageNeedCount;
			command.virtualGeometryPageId = range.pageId;
			command.virtualGeometryFallbackPageId = range.fallbackPageId;
			command.virtualGeometryPayloadSource = range.failOpen
				? VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
				: VirtualGeometryPagePayloadSource::Unknown;
			command.virtualGeometryEmittedSource = ResolveVirtualGeometryEmittedSource(command.virtualGeometryPayloadSource);
			command.virtualGeometryFlags = virtualGeometryFlags;
			expandedCommands.push_back(command);
			packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
			packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());
			continue;
		}

		const std::uint32_t clusterPacketBase = static_cast<std::uint32_t>(packet.clusterPackets.size());
		std::vector<VirtualGeometryClusterPacket> localPackets = BuildVirtualGeometryClusterPackets(
			runtime,
			selection.selectedClusters,
			command.materialId,
			viewType,
			outputId,
			clusterPacketBase);
		const FrameVirtualRasterInputs rasterInputs{inputs.frameMaterialResources, inputs.resourceCohort, inputs.backendCapture};
		RouteFrameVirtualGeometryPackets(rasterInputs, command, mesh, localPackets, viewProjection, targetSize);
		if (localPackets.size() == selection.selectedClusters.size())
		{
			std::vector<VirtualGeometryClusterPacket> compactedPackets;
			CompactedVisibilitySet compaction = CompactVirtualGeometryClusterPackets(
				localPackets,
				compactedPackets,
				viewType,
				outputId);
			std::vector<ClusterWorkBatch> localBatches = BuildVirtualGeometryClusterBatches(
				compactedPackets,
				clusterPacketBase,
				static_cast<std::uint32_t>(packet.clusterBatches.size()));
			compaction.submittedBatches = static_cast<std::uint32_t>(localBatches.size());

			// Page-need records are authored by selection before batching.
			// Patch them with the packet/batch that currently preserves
			// coverage so streaming diagnostics can follow the same work item
			// that Vulkan receives.
			for (TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : annotatedPageNeeds)
			{
				std::size_t matchedPacketIndex = 0u;
				bool matchedPacket = false;
				for (std::size_t packetIndex = 0u; packetIndex < compactedPackets.size(); ++packetIndex)
				{
					const VirtualGeometryClusterPacket& localPacket = compactedPackets[packetIndex];
					if (localPacket.fallbackPacket || localPacket.clusterId == need.clusterId)
					{
						matchedPacketIndex = packetIndex;
						matchedPacket = true;
						if (localPacket.clusterId == need.clusterId)
						{
							break;
						}
					}
				}
				if (!matchedPacket)
				{
					continue;
				}
				const std::uint32_t globalPacketIndex = clusterPacketBase + static_cast<std::uint32_t>(matchedPacketIndex);
				need.packetId = compactedPackets[matchedPacketIndex].packetId;
				for (const ClusterWorkBatch& batch : localBatches)
				{
					if (globalPacketIndex >= batch.packetOffset && globalPacketIndex < batch.packetOffset + batch.packetCount)
					{
						need.batchId = batch.batchId;
						need.packetOffset = batch.packetOffset;
						need.packetCount = batch.packetCount;
						break;
					}
				}
			}
			for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : annotatedPageNeeds)
			{
				if (!need.fallbackRequired)
				{
					continue;
				}
				FallbackCoverageChain chain;
				chain.meshId = need.meshId;
				chain.desiredClusterId = need.clusterId;
				chain.fallbackPageId = 0u;
				chain.coverageStatus = VirtualGeometryFallbackCoverageStatus::Complete;
				chain.reason = "resident fallback coverage preserved while desired page streams";
				for (const VirtualGeometryClusterPacket& fallbackPacket : compactedPackets)
				{
					if (!fallbackPacket.fallbackPacket)
					{
						continue;
					}
					chain.fallbackClusterId = fallbackPacket.clusterId;
					chain.fallbackPageId = fallbackPacket.pageId;
					chain.substitutedMeshlets = fallbackPacket.meshletCount;
					break;
				}
				if (chain.substitutedMeshlets == 0u)
				{
					chain.coverageStatus = VirtualGeometryFallbackCoverageStatus::Partial;
					chain.reason = "fallback request is pending; compacted packets kept fail-open coverage";
				}
				packet.fallbackCoverage.push_back(std::move(chain));
			}

			packet.clusterPackets.insert(packet.clusterPackets.end(), compactedPackets.begin(), compactedPackets.end());
			packet.clusterBatches.insert(packet.clusterBatches.end(), localBatches.begin(), localBatches.end());
			packet.compactionResults.push_back(compaction);
			packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
			packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());

			for (const ClusterWorkBatch& batch : localBatches)
			{
				const std::uint32_t localPacketOffset = batch.packetOffset - clusterPacketBase;
				const VirtualGeometryClusterPacket& firstPacket = compactedPackets[localPacketOffset];
				const VirtualGeometryClusterPacket& lastPacket = compactedPackets[localPacketOffset + batch.packetCount - 1u];
				RenderItemRecord batchCommand = command;
				batchCommand.virtualMeshletOffset = firstPacket.meshletOffset;
				batchCommand.virtualMeshletCount = (lastPacket.meshletOffset + lastPacket.meshletCount) - firstPacket.meshletOffset;
				batchCommand.selectedVirtualLod = firstPacket.selectedLod;
				batchCommand.selectedVirtualClusterOffset = range.selectedClusterOffset + (firstPacket.packetId - clusterPacketBase);
				batchCommand.selectedVirtualClusterCount = batch.packetCount;
				batchCommand.selectedVirtualPageNeedOffset = range.pageNeedOffset;
				batchCommand.selectedVirtualPageNeedCount = 0u;
				batchCommand.virtualGeometryPageId = firstPacket.pageId;
				batchCommand.virtualGeometryFallbackPageId = firstPacket.fallbackPageId;
				batchCommand.virtualGeometryPayloadSource = firstPacket.payloadSource;
				batchCommand.virtualGeometryEmittedSource = firstPacket.emittedSource;
				batchCommand.virtualGeometryPagePayloadByteOffset = firstPacket.pagePayloadByteOffset;
				batchCommand.virtualGeometryPagePayloadByteSize = firstPacket.pagePayloadByteSize;
				batchCommand.virtualGeometryPayloadBufferIndex = firstPacket.payloadBufferIndex;
				batchCommand.virtualGeometryPayloadDescriptorIndex = firstPacket.payloadDescriptorIndex;
				// Page-local payload uploads are keyed by the submitted packet id.
				// Do not reuse selected-cluster offsets here: package imports and
				// fallback compaction can make selection rows and packet rows drift.
				batchCommand.virtualGeometryPacketDescriptorIndex = firstPacket.packetId;
				for (std::uint32_t packetIndex = localPacketOffset; packetIndex < localPacketOffset + batch.packetCount; ++packetIndex)
				{
					if (compactedPackets[packetIndex].fallbackPacket)
					{
						batchCommand.selectedVirtualPageNeedCount = range.pageNeedCount;
						break;
					}
				}
				// The route belongs to this homogeneous packet batch. Carry it in
				// the established flags word so Vulkan can omit compute-owned main
				// draws only after the visibility composite is proven recordable.
				batchCommand.virtualGeometryFlags = virtualGeometryFlags
					| ((firstPacket.rasterRoute
						& kVirtualGeometryRasterRouteMask)
						<< kVirtualGeometryRasterRouteShift);
				expandedCommands.push_back(batchCommand);
			}
			continue;
		}

		{
			// If corrupt metadata made every selected cluster unresolvable,
			// keep the preselection range visible and let validation/diagnostic
			// paths report the bad cluster ids instead of dropping the object.
			command.virtualMeshletOffset = range.meshletOffset;
			command.virtualMeshletCount = range.meshletCount;
			command.selectedVirtualLod = range.selectedLodIndex;
			command.selectedVirtualClusterOffset = range.selectedClusterOffset;
			command.selectedVirtualClusterCount = range.selectedClusterCount;
			command.selectedVirtualPageNeedOffset = range.pageNeedOffset;
			command.selectedVirtualPageNeedCount = range.pageNeedCount;
			command.virtualGeometryPageId = range.pageId;
			command.virtualGeometryFallbackPageId = range.fallbackPageId;
			command.virtualGeometryPayloadSource = VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			command.virtualGeometryEmittedSource = VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
			command.virtualGeometryFlags = virtualGeometryFlags | kVirtualGeometryFlagFailOpen;
			expandedCommands.push_back(command);
		}
		packet.selectedClusters.insert(packet.selectedClusters.end(), selection.selectedClusters.begin(), selection.selectedClusters.end());
		packet.pageNeeds.insert(packet.pageNeeds.end(), annotatedPageNeeds.begin(), annotatedPageNeeds.end());
	}
	ValidateFrameVirtualSelectionPacket(packet, viewType, outputId, targetSize,
		viewVirtualGeometryQuality.debugVisualizationMode);
	commands = std::move(expandedCommands);
	CollectFrameVirtualGeometryDiagnostics(inputs, preparation, commands, viewType);
	return packet;

}

// This selection operation borrows captured resources and mutates only typed frame-owned outputs.
void RenderSystem::ApplyFrameShadowVirtualGeometrySelection(const FrameVirtualGeometryInputs& inputs,
	FrameVirtualGeometryPreparation& preparation, std::vector<RenderItemRecord>& commands)
{
	const auto& fixtureTargets = inputs.fixtureTargets;
	 auto& fixtureShadowSelectionApplied = preparation.fixtureShadowSelectionApplied;
	auto& renderFrameSnapshot = inputs.renderFrameSnapshot;
	auto& frameMeshResources = inputs.frameMeshResources;
	auto& geometryInputs = inputs.geometryInputs;

	for (RenderItemRecord& command : commands)
	{
		const auto capturedMesh = frameMeshResources.find(command.meshId);
		const Mesh* mesh = capturedMesh != frameMeshResources.end()
			? capturedMesh->second : nullptr;
		if (mesh != nullptr
			&& !mesh->hasVirtualGeometry
			&& command.geometryRoute == TerraEngine::Renderer::GeometryRoute::DirectMeshlet
			&& !mesh->virtualGeometryDetailLevels.empty())
		{
			// Shadow silhouettes use direct LOD0 conservatively while camera views
			// remain free to choose a cheaper resident range.
			const VirtualGeometryDetailLevelRecord& baseDetail =
				mesh->virtualGeometryDetailLevels.front();
			command.virtualMeshletOffset = baseDetail.meshletOffset;
			command.virtualMeshletCount = baseDetail.meshletCount;
			command.selectedVirtualLod = baseDetail.lodIndex;
			continue;
		}
		if (mesh == nullptr || !mesh->hasVirtualGeometry
			|| !command.UsesVirtualGeometryRoute())
		{
			// Shadow views consume the same representation decision as the
			// camera view, preserving identity without hidden VG work.
			continue;
		}

		const VirtualGeometryAssetRuntime& runtime =
			geometryInputs.Runtime(command.meshId, *mesh);
		const TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryShadowLodRange(
			runtime,
			renderFrameSnapshot.renderSettings.virtualGeometry,
			!mesh->meshletBounds.empty());

		command.virtualMeshletOffset = range.meshletOffset;
		command.virtualMeshletCount = range.meshletCount;
		command.selectedVirtualLod = range.selectedLodIndex;
		command.virtualGeometryFlags = (range.virtualGeometryActive ? kVirtualGeometryFlagActive : 0u)
			| (range.failOpen ? kVirtualGeometryFlagFailOpen : 0u);
	}
	const auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
	if (intent.controlsApplied && intent.shadowSelection)
	{
		const auto target = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
		if (target != fixtureTargets.end())
			for (auto& command : commands)
				if (TerraEngine::Renderer::ApplyRendererEfficiencyFixtureSelection(command, *intent.shadowSelection, *target)) fixtureShadowSelectionApplied.store(true, std::memory_order_relaxed);
	}
	CollectFrameVirtualGeometryDiagnostics(inputs, preparation, commands, RenderViewType::Shadow);

}

// Finish owned shadow/HiZ/page evidence before exposing the complete view selection.
void RenderSystem::FinalizeFrameVirtualGeometryEvidence(
	TerraEngine::Renderer::VirtualGeometryViewSelectionState& state,
	const TerraEngine::Renderer::Frame::VisibleCommandCacheRecord* cachedSelectionEvidence,
	const TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket* preparedSelection,
	bool reuseValidatedSelection)
{
	const auto targetSize = state.targetSize;
	const bool shadowPass = state.shadowPass;
	const auto viewType = state.viewType;
	const auto outputId = state.outputId;
	if (shadowPass || viewType == RenderViewType::Shadow)
	{
		state.hiZValidationState.state = VirtualGeometryHiZState::Unsupported;
		state.hiZValidationState.reason = "Shadow virtual geometry uses conservative visibility and does not sample camera Hi-Z.";
		state.hiZValidationState.failOpen = true;
	}
	else if (targetSize.x == 0u || targetSize.y == 0u)
	{
		state.hiZValidationState.state = VirtualGeometryHiZState::MissingPyramid;
		state.hiZValidationState.reason = "Render target size is unavailable, so no current Hi-Z pyramid can be sampled.";
		state.hiZValidationState.failOpen = true;
	}
	else if (!state.hiZOcclusionAllowed)
	{
		state.hiZValidationState.state = VirtualGeometryHiZState::FailOpen;
		state.hiZValidationState.reason = state.hiZInactiveReason.empty()
			? "Hi-Z occlusion is failing open for this view."
			: state.hiZInactiveReason;
		state.hiZValidationState.failOpen = true;
	}
	else
	{
		state.hiZValidationState.state = VirtualGeometryHiZState::Active;
		state.hiZValidationState.pyramidSize = targetSize;
		state.hiZValidationState.pyramidReady = true;
		state.hiZValidationState.warmupFrames = 1u;
		state.hiZValidationState.failOpen = false;
	}
	if ((shadowPass || viewType == RenderViewType::Shadow)
		&& state.ClusterPackets().empty())
	{
		for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
			state.DrawRecords())
		{
			if (!draw.range.virtualGeometryActive || draw.range.meshletCount == 0u)
			{
				continue;
			}
			VirtualGeometryClusterPacket packet;
			packet.packetId = static_cast<std::uint32_t>(state.clusterPackets.size());
			packet.viewId = outputId;
			packet.viewType = RenderViewType::Shadow;
			packet.objectId = draw.objectId;
			packet.renderableIndex = draw.renderableIndex;
			packet.meshId = draw.meshId;
			packet.selectedLod = draw.range.selectedLodIndex;
			packet.clusterId = draw.range.selectedClusterOffset;
			packet.meshletOffset = draw.range.meshletOffset;
			packet.meshletCount = draw.range.meshletCount;
			packet.fallbackPacket = draw.range.failOpen;
			packet.payloadSource = draw.range.failOpen
				? VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback
				: VirtualGeometryPagePayloadSource::EmbeddedMeshPayload;
			packet.emittedSource = ResolveVirtualGeometryEmittedSource(packet.payloadSource);
			packet.pagePayloadByteSize = static_cast<std::uint64_t>(packet.meshletCount);
			packet.payloadBufferIndex = draw.range.payloadBufferIndex;
			packet.payloadDescriptorIndex = draw.range.payloadDescriptorIndex;
			packet.debugIdentity = packet.packetId ^ 0x5a5a0000u;
			state.clusterPackets.push_back(packet);
		}
		state.clusterBatches = BuildVirtualGeometryClusterBatches(state.clusterPackets);
	}
	if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Packet Upload Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		state.gpuPacketRecords = BuildGpuVirtualGeometryPacketRecords(
			state.ClusterPackets(),
			activeRenderFrameId,
			state.debugVisualizationMode);
		state.gpuBatchDescriptors = BuildGpuVirtualGeometryBatchDescriptors(
			state.gpuPacketRecords,
			activeRenderFrameId,
			state.debugVisualizationMode);
	}
	std::string gpuPacketValidationReason;
	bool gpuPacketRecordsValid = true;
	// A visible-command cache hit reuses selection inputs that passed these
	// exact CPU-oracle checks when the cache record was created. The signature
	// includes view, geometry, residency, settings, and renderer generations;
	// current-frame packet builders only restamp the frame identity. Repeating
	// the same linear validation would add work proportional to every visible
	// packet without discovering a new mutable input.
	if(preparedSelection == nullptr && !reuseValidatedSelection) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Validate VG Packet Records",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		gpuPacketRecordsValid = ValidateGpuVirtualGeometryPacketRecords(
			state.ClusterPackets(),
			state.SelectedClusters(),
			state.gpuPacketRecords,
			activeRenderFrameId,
			state.debugVisualizationMode,
			&gpuPacketValidationReason);
	}
	if (preparedSelection == nullptr && cachedSelectionEvidence == nullptr
		&& (!state.ClusterPackets().empty() || !state.gpuBatchDescriptors.empty()))
	{
		GpuVirtualGeometryPacketFrameSet frameSet = BuildGpuVirtualGeometryPacketFrameSet(
			state.viewType,
			state.outputId,
			activeRenderFrameId,
			state.targetSize,
			state.debugVisualizationMode,
			0u,
			static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
			0u,
			static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()));
		if (!gpuPacketRecordsValid)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			frameSet.validity = VirtualGeometryGpuFrameSetValidity::FailOpen;
			frameSet.packetCount = 0u;
			frameSet.batchCount = 0u;
			frameSet.compactedPacketCount = 0u;
			frameSet.generatedSubmissionCount = 0u;
			frameSet.fallbackReason = gpuPacketValidationReason;
			state.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
				state.viewType,
				state.outputId,
				activeRenderFrameId,
				gpuPacketValidationReason,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()),
				"GPU packet records failed CPU-oracle validation; existing CPU-selected draw coverage remains authoritative."));
			state.gpuPacketRecords.clear();
			state.gpuBatchDescriptors.clear();
		}
		state.gpuPacketFrameSets.push_back(std::move(frameSet));
	}
	if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Generated Submission Validation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		state.generatedSubmissions = BuildGeneratedVirtualGeometrySubmissionRecords(
			state.gpuBatchDescriptors,
			state.gpuPacketRecords,
			activeRenderFrameId);
		std::string generatedSubmissionValidationReason;
		const bool generatedSubmissionsValid = reuseValidatedSelection
			|| ValidateGeneratedVirtualGeometrySubmissionRecords(
				state.generatedSubmissions,
				state.gpuBatchDescriptors,
				activeRenderFrameId,
				state.viewType,
				state.outputId,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				&generatedSubmissionValidationReason);
		if (!generatedSubmissionsValid)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			state.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
				state.viewType,
				state.outputId,
				activeRenderFrameId,
				generatedSubmissionValidationReason,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size()),
				"Generated virtual geometry submissions failed validation; CPU-expanded packet batches remain authoritative."));
			for (GeneratedVirtualGeometrySubmissionRecord& submission : state.generatedSubmissions)
			{
				submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
				submission.fallbackReason = generatedSubmissionValidationReason;
			}
		}
	}
	if(preparedSelection == nullptr && cachedSelectionEvidence == nullptr) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Compaction Dispatch Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		state.gpuCompactionDispatches = BuildGpuPacketCompactionDispatches(
			state.gpuPacketFrameSets,
			state.hiZValidationState,
			state.debugVisualizationMode);
		for (const CompactedVisibilitySet& compaction : state.CompactionResults())
		{
			state.gpuCounterReadbacks.push_back(BuildVirtualGeometryCounterReadbackRecord(
				compaction,
				activeRenderFrameId,
				VirtualGeometryCounterSource::Prepared,
				0u,
				"Prepared CPU compaction counters are available; GPU readback is populated after backend support is active."));
		}
	}
	if (shadowPass || viewType == RenderViewType::Shadow)
	{
		ShadowVisibilitySelection shadowSelection{};
		shadowSelection.shadowViewId = outputId;
		shadowSelection.frameIndex = activeRenderFrameId;
		shadowSelection.candidatePackets = static_cast<std::uint32_t>(
			state.ClusterPackets().size());
		shadowSelection.selectedPackets = static_cast<std::uint32_t>(
			state.ClusterPackets().size());
		for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
			state.DrawRecords())
		{
			if (!draw.range.virtualGeometryActive)
			{
				continue;
			}
			shadowSelection.selectedMeshlets += draw.range.meshletCount;
			shadowSelection.dispatchedMeshlets += draw.range.meshletCount;
			if (draw.range.failOpen)
			{
				shadowSelection.fallbackReasons.push_back(draw.range.fallbackReason.empty()
					? "shadow virtual geometry failed open"
					: draw.range.fallbackReason);
			}
		}
		state.shadowVisibilitySelections.push_back(std::move(shadowSelection));
	}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG HiZ Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			HiZRegressionSnapshot hiZRegression{};
			hiZRegression.viewId = outputId;
			hiZRegression.viewType = viewType;
			hiZRegression.frameIndex = activeRenderFrameId;
			hiZRegression.state = state.hiZValidationState.state;
			hiZRegression.reason = state.hiZValidationState.reason;
			hiZRegression.recoveryFrames = state.hiZValidationState.warmupFrames;
			for (const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
				state.DrawRecords())
			{
				// This snapshot is a prepared, view-local copy of the current Hi-Z
				// contract. GPU readback still owns exact culled counts; prepared
				// counters only prove that the new batched path did not introduce a
				// new hard-disable or false-occlusion policy before submission.
				if (!draw.range.virtualGeometryActive)
				{
					continue;
				}
				hiZRegression.visibleMeshlets += draw.range.meshletCount;
				if (draw.range.failOpen)
				{
					hiZRegression.failOpenMeshlets += draw.range.meshletCount;
					hiZRegression.invalidDepthSamples += draw.range.meshletCount;
				}
				else if (state.hiZValidationState.state == VirtualGeometryHiZState::Active)
				{
					hiZRegression.testedMeshlets += draw.range.meshletCount;
				}
			}
			hiZRegression.baselineMatched =
				state.hiZValidationState.state == VirtualGeometryHiZState::Active
				|| state.hiZValidationState.state == VirtualGeometryHiZState::Unsupported
				|| state.hiZValidationState.failOpen;
			state.hiZRegressionSnapshots.push_back(std::move(hiZRegression));
		}
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build VG Page Evidence",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(cachedSelectionEvidence != nullptr)
			{
				// Requests and payloads are already shared by the retained selection
				// owner. The view state carries the current frame identity; individual
				// source rows describe signature-stable selection evidence.
			}
			else
			{
				PopulateVirtualGeometryPageRequestRecords(state);
				PopulateVirtualGeometryPageSourceRecords(state, activeRenderFrameId);
			}
		}
}

// Resolve projected draw inputs and material/bin routing from captured immutable owners.
void RenderSystem::RouteFrameVirtualGeometryPackets(const FrameVirtualRasterInputs& inputs,
	const RenderItemRecord& command, const Mesh* mesh, std::vector<VirtualGeometryClusterPacket>& localPackets,
	const glm::mat4& viewProjection, const glm::uvec2& targetSize)
{
	const auto& frameMaterialResources = inputs.frameMaterialResources;
	const auto& capturedDefaultShaderCollectionId = inputs.resourceCohort.capturedDefaultShaderCollectionId;
	const auto& capturedDefaultTaskShaderCollectionId = inputs.resourceCohort.capturedDefaultTaskShaderCollectionId;
	const auto hardwareVirtualGeometryRouteAvailable = inputs.backendCapture.hardwareVirtualGeometryRouteAvailable;
	const auto computeVirtualGeometryRouteAvailable = inputs.backendCapture.computeVirtualGeometryRouteAvailable;
	const auto hybridValidationSceneRequested = inputs.backendCapture.hybridValidationSceneRequested;
	const auto& validationRouteOverride = inputs.backendCapture.validationRouteOverride;
	// Project the packet AABB once during view preparation. The same bounds
	// drive editor picking, CPU compaction, and the microtriangle route split,
	// keeping all three consumers on one immutable estimate.
	const glm::mat4 packetClipTransform =
		viewProjection * command.modelMatrix;
	for(VirtualGeometryClusterPacket& localPacket : localPackets)
	{
		localPacket.renderableIndex =
			command.conventionalVisibilityIdentityIndex == 0u
				? std::numeric_limits<std::uint32_t>::max()
				: command.conventionalVisibilityIdentityIndex - 1u;
		glm::vec2 minimumUv(
			std::numeric_limits<float>::max());
		glm::vec2 maximumUv(
			std::numeric_limits<float>::lowest());
		bool projectionValid = true;
		for(std::uint32_t corner = 0u; corner < 8u; ++corner)
		{
			const glm::vec3 localCorner(
				(corner & 1u) != 0u
					? localPacket.boundsMax.x
					: localPacket.boundsMin.x,
				(corner & 2u) != 0u
					? localPacket.boundsMax.y
					: localPacket.boundsMin.y,
				(corner & 4u) != 0u
					? localPacket.boundsMax.z
					: localPacket.boundsMin.z);
			const glm::vec4 clip = packetClipTransform
				* glm::vec4(localCorner, 1.0f);
			if(!std::isfinite(clip.w) || clip.w <= 1.0e-5f)
			{
				projectionValid = false;
				break;
			}
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			// Camera projections already include Terra's Vulkan Y flip, and
			// the renderer uses a positive-height viewport. Match the actual
			// framebuffer transform used by hardware and compute raster.
			const glm::vec2 uv = ndc * 0.5f + glm::vec2(0.5f);
			minimumUv = glm::min(minimumUv, uv);
			maximumUv = glm::max(maximumUv, uv);
		}
		if(projectionValid)
		{
			localPacket.projectedBounds = glm::vec4(
				minimumUv, maximumUv);
		}
	}
	// Resolve route policy while this immutable command still owns the
	// exact material identity. Numeric route/bin values are copied into
	// packet flags, so protected Vulkan recording never reopens mutable
	// material state.
	const auto capturedMaterial = frameMaterialResources.find(command.materialId);
	const Material* packetMaterial = capturedMaterial != frameMaterialResources.end()
		? capturedMaterial->second : nullptr;
	const VirtualGeometryMaterialExtension* materialPolicy =
		packetMaterial != nullptr
			&& packetMaterial->virtualGeometryExtension.has_value()
		? &packetMaterial->virtualGeometryExtension.value()
		: nullptr;
	const std::uint32_t materialBin =
		materialPolicy != nullptr ? materialPolicy->shadingBin : 0u;
	const std::uint32_t materialBinCapacity =
		materialPolicy != nullptr
			&& materialPolicy->maximumShadingBins != 0u
		? std::min(materialPolicy->maximumShadingBins, 256u)
		: 64u;
	const bool materialForcesCompute =
		materialPolicy != nullptr
		&& materialPolicy->fallbackPolicy
			== VirtualGeometryMaterialFallbackPolicy::Compute;
	const bool materialDisablesVirtualGeometry =
		materialPolicy != nullptr
		&& materialPolicy->fallbackPolicy
			== VirtualGeometryMaterialFallbackPolicy::DisableVirtualGeometry;
	const bool alphaMasked = packetMaterial != nullptr
		&& packetMaterial->header.alphaMode
			== static_cast<std::uint32_t>(AlphaMode::Mask);
	const bool defaultSurfaceShader = packetMaterial != nullptr
		&& (packetMaterial->header.shaderCollectionId
				== TerraEngine::EMPTY_UUID
			|| packetMaterial->header.shaderCollectionId
				== capturedDefaultShaderCollectionId
			|| packetMaterial->header.shaderCollectionId
				== capturedDefaultTaskShaderCollectionId);
	const bool opaqueComputeSurface = packetMaterial != nullptr
		&& packetMaterial->header.alphaMode
			!= static_cast<std::uint32_t>(AlphaMode::Blend);
	const bool deformationSafeComputeSurface =
		command.animationBinding
			.IsComputeRasterDeformationSafe();
	std::vector<TerraEngine::Renderer::VirtualGeometryRasterCandidate>
		rasterCandidates;
	rasterCandidates.reserve(localPackets.size());
	for(std::size_t packetIndex = 0u;
		packetIndex < localPackets.size();
		++packetIndex)
	{
		const VirtualGeometryClusterPacket& localPacket =
			localPackets[packetIndex];
		TerraEngine::Renderer::VirtualGeometryRasterCandidate candidate;
		candidate.stableId = localPacket.debugIdentity;
		candidate.materialBin = materialBin;
		candidate.hardwareSupported =
			hardwareVirtualGeometryRouteAvailable
			&& !materialForcesCompute
			&& !materialDisablesVirtualGeometry;
		candidate.computeSupported =
			computeVirtualGeometryRouteAvailable
			&& !materialDisablesVirtualGeometry
			&& defaultSurfaceShader
			&& opaqueComputeSurface
			&& deformationSafeComputeSurface
			&& localPacket.projectedBounds != glm::vec4(0.0f);
		candidate.alphaMasked = alphaMasked;
		candidate.requiresStableIdentity = true;
		const glm::vec2 clampedMinimum = glm::clamp(
			glm::vec2(localPacket.projectedBounds),
			glm::vec2(0.0f),
			glm::vec2(1.0f));
		const glm::vec2 clampedMaximum = glm::clamp(
			glm::vec2(
				localPacket.projectedBounds.z,
				localPacket.projectedBounds.w),
			glm::vec2(0.0f),
			glm::vec2(1.0f));
		const glm::vec2 projectedExtent = glm::max(
			clampedMaximum - clampedMinimum,
			glm::vec2(0.0f));
		candidate.projectedPixelArea =
			projectedExtent.x * static_cast<float>(targetSize.x)
			* projectedExtent.y * static_cast<float>(targetSize.y);
		for(std::uint32_t meshletIndex = localPacket.meshletOffset;
			meshletIndex < localPacket.meshletOffset
				+ localPacket.meshletCount
				&& meshletIndex < mesh->meshletRecords.size();
			++meshletIndex)
		{
			candidate.triangleCount +=
				mesh->meshletRecords[meshletIndex]
					.primitiveIndexCount / 3u;
		}
		candidate.preferCompute = materialForcesCompute;
		candidate.sourceOrdinal =
			static_cast<std::uint32_t>(packetIndex);
		rasterCandidates.push_back(candidate);
	}
	const TerraEngine::Renderer::VirtualGeometryRasterBinPlan rasterPlan =
		hybridValidationSceneRequested
		? TerraEngine::Renderer::BuildVirtualGeometryHybridRasterBinPlan(
			rasterCandidates,
			materialBinCapacity)
		: TerraEngine::Renderer::BuildVirtualGeometryRasterBinPlan(
			rasterCandidates,
			materialBinCapacity);
	for(std::size_t packetIndex = 0u;
		packetIndex < localPackets.size();
		++packetIndex)
	{
		VirtualGeometryClusterPacket& localPacket =
			localPackets[packetIndex];
		const TerraEngine::Renderer::VirtualGeometryRasterCandidate&
			candidate = rasterCandidates[packetIndex];
		// Raster plans are stable-ID ordered, but retain each packet's
		// source ordinal. Resolve through that ordinal so duplicate debug
		// hashes cannot reorder ownership, page needs, or cluster records.
		const auto plannedOrdinal = std::find(
			rasterPlan.routeSourceOrdinals.begin(),
			rasterPlan.routeSourceOrdinals.end(),
			static_cast<std::uint32_t>(packetIndex));
		const std::size_t plannedIndex =
			static_cast<std::size_t>(std::distance(
				rasterPlan.routeSourceOrdinals.begin(),
				plannedOrdinal));
		TerraEngine::Renderer::VirtualGeometryRasterRoute rasterRoute =
			plannedOrdinal == rasterPlan.routeSourceOrdinals.end()
				|| plannedIndex >= rasterPlan.routes.size()
			? TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed
			: rasterPlan.routes[plannedIndex];
		if(hybridValidationSceneRequested
			&& std::find(
				rasterPlan.fallbackStableIds.begin(),
				rasterPlan.fallbackStableIds.end(),
				candidate.stableId)
				!= rasterPlan.fallbackStableIds.end())
		{
			if(!localPacket.fallbackReason.empty())
			{
				localPacket.fallbackReason += "; ";
			}
			localPacket.fallbackReason +=
				"hybrid validation kept an unsupported candidate visible through indexed fail-open";
		}
		if (validationRouteOverride.has_value())
		{
			const TerraEngine::Renderer::VirtualGeometryRasterRoute requestedRoute =
				*validationRouteOverride;
			const bool packetSupportsRequestedRoute =
				(requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Hardware
					&& candidate.hardwareSupported)
				|| (requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Compute
					&& candidate.computeSupported)
				|| requestedRoute == TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
			if (packetSupportsRequestedRoute) {
				rasterRoute = requestedRoute;
			} else {
				rasterRoute = TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
				if (!localPacket.fallbackReason.empty()) localPacket.fallbackReason += "; ";
				localPacket.fallbackReason +=
					"requested validation route is unsupported by this packet/material; used visible indexed fallback";
			}
		}
		if(rasterRoute
			== TerraEngine::Renderer::VirtualGeometryRasterRoute::Fallback)
		{
			rasterRoute =
				TerraEngine::Renderer::VirtualGeometryRasterRoute::Indexed;
			if(!localPacket.fallbackReason.empty())
			{
				localPacket.fallbackReason += "; ";
			}
			localPacket.fallbackReason +=
				"material shading-bin capacity overflow used visible indexed fallback";
		}
		localPacket.rasterRoute =
			static_cast<std::uint32_t>(rasterRoute);
		localPacket.materialBin = std::min(materialBin, 255u);
	}
}

// Validate typed packets and generate submission/fallback evidence before returning selection.
void RenderSystem::ValidateFrameVirtualSelectionPacket(
	TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket& packet,
	RenderViewType viewType, std::uint32_t outputId, const glm::uvec2& targetSize,
	VirtualGeometryDebugVisualizationMode debugVisualizationMode)
{
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Packet Upload Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		packet.gpuPacketRecords = BuildGpuVirtualGeometryPacketRecords(
			packet.clusterPackets,
			activeRenderFrameId,
			debugVisualizationMode);
		packet.gpuBatchDescriptors = BuildGpuVirtualGeometryBatchDescriptors(
			packet.gpuPacketRecords,
			activeRenderFrameId,
			debugVisualizationMode);
	}
	std::string gpuPacketValidationReason;
	const bool gpuPacketRecordsValid = ValidateGpuVirtualGeometryPacketRecords(
		packet.clusterPackets,
		packet.selectedClusters,
		packet.gpuPacketRecords,
		activeRenderFrameId,
		debugVisualizationMode,
		&gpuPacketValidationReason);
	if (!packet.gpuPacketRecords.empty() || !packet.gpuBatchDescriptors.empty())
	{
		GpuVirtualGeometryPacketFrameSet frameSet = BuildGpuVirtualGeometryPacketFrameSet(
			viewType,
			outputId,
			activeRenderFrameId,
			targetSize,
			debugVisualizationMode,
			0u,
			static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
			0u,
			static_cast<std::uint32_t>(packet.gpuBatchDescriptors.size()));
		if(!gpuPacketRecordsValid) {
			FailOpenFrameVirtualPacketRecords(packet, frameSet, gpuPacketValidationReason);
		}
		packet.gpuPacketFrameSets.push_back(std::move(frameSet));
	}
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Generated Submission Validation", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		packet.generatedSubmissions = BuildGeneratedVirtualGeometrySubmissionRecords(
			packet.gpuBatchDescriptors,
			packet.gpuPacketRecords,
			activeRenderFrameId);
		std::string generatedSubmissionValidationReason;
		if (!ValidateGeneratedVirtualGeometrySubmissionRecords(
			packet.generatedSubmissions,
			packet.gpuBatchDescriptors,
			activeRenderFrameId,
			viewType,
			outputId,
			static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
			&generatedSubmissionValidationReason))
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			packet.gpuFallbackRecords.push_back(BuildVirtualGeometryGpuFallbackRecord(
				viewType,
				outputId,
				activeRenderFrameId,
				generatedSubmissionValidationReason,
				static_cast<std::uint32_t>(packet.gpuPacketRecords.size()),
				static_cast<std::uint32_t>(packet.gpuBatchDescriptors.size()),
				"Generated virtual geometry submissions failed validation; CPU-expanded packet batches remain authoritative."));
			for (GeneratedVirtualGeometrySubmissionRecord& submission : packet.generatedSubmissions)
			{
				submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
				submission.fallbackReason = generatedSubmissionValidationReason;
			}
		}
	}
	VirtualGeometryHiZValidationState packetHiZState;
	packetHiZState.state = targetSize.x == 0u || targetSize.y == 0u
		? VirtualGeometryHiZState::MissingPyramid
		: VirtualGeometryHiZState::Active;
	packetHiZState.renderSize = targetSize;
	packetHiZState.pyramidSize = targetSize;
	packetHiZState.pyramidReady = packetHiZState.state == VirtualGeometryHiZState::Active;
	packetHiZState.failOpen = packetHiZState.state != VirtualGeometryHiZState::Active;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Compaction Dispatch Planning", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		packet.gpuCompactionDispatches = BuildGpuPacketCompactionDispatches(
			packet.gpuPacketFrameSets,
			packetHiZState,
			debugVisualizationMode);
		for (const CompactedVisibilitySet& compaction : packet.compactionResults)
		{
			packet.gpuCounterReadbacks.push_back(BuildVirtualGeometryCounterReadbackRecord(
				compaction,
				activeRenderFrameId,
				VirtualGeometryCounterSource::Prepared,
				0u,
				"Prepared CPU compaction counters are available; GPU readback is populated after backend support is active."));
		}
	}
}

// Failed validation preserves visible CPU coverage while clearing invalid device packet work.
void RenderSystem::FailOpenFrameVirtualPacketRecords(
	TerraEngine::Renderer::Frame::VirtualGeometryViewSelectionPacket& packet,
	GpuVirtualGeometryPacketFrameSet& frameSet, const std::string& gpuPacketValidationReason)
{

	TERRAENGINE_PROFILE_SCOPE("RenderSystem: VG Fallback Submission", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	frameSet.validity = VirtualGeometryGpuFrameSetValidity::FailOpen;
	frameSet.packetCount = 0u;
	frameSet.batchCount = 0u;
	frameSet.compactedPacketCount = 0u;
	frameSet.generatedSubmissionCount = 0u;
	frameSet.fallbackReason = gpuPacketValidationReason;
	packet.gpuPacketRecords.clear();
	packet.gpuBatchDescriptors.clear();

}

// Retire only histories absent from this exact frame request set.
void RenderSystem::RetireFrameViewHistories(const FrameVirtualGeometryPreparation& preparation)
{
	const auto& activeVirtualGeometryViewPolicyKeys = preparation.activeVirtualGeometryViewPolicyKeys;
	const auto& activeVirtualGeometryTransitionKeys = preparation.activeVirtualGeometryTransitionKeys;
	const auto& activeVirtualGeometryDebugModeKeys = preparation.activeVirtualGeometryDebugModeKeys;
	// View-local history is valid only while its exact output identity remains in
	// the protected request set. Removing a view retires its Hi-Z/LOD/debug state;
	// recreating an output ID or resizing it starts from conservative visibility.
	std::erase_if(virtualGeometryViewPolicies,
		[&activeVirtualGeometryViewPolicyKeys](const auto& entry) {
			return !activeVirtualGeometryViewPolicyKeys.contains(entry.first);
		});
	std::erase_if(virtualGeometryTransitionCache,
		[&activeVirtualGeometryTransitionKeys](const auto& entry) {
			return !activeVirtualGeometryTransitionKeys.contains(entry.first);
		});
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		std::erase_if(virtualGeometryDebugModes,
			[&activeVirtualGeometryDebugModeKeys](const auto& entry) {
				return !activeVirtualGeometryDebugModeKeys.contains(entry.first);
			});
	}
}
