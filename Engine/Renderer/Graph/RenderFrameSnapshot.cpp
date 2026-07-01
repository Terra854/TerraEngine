#include "RenderFrameSnapshot.h"
#include "RenderDiagnostics.h"

#include <functional>

namespace TerraEngine::Renderer {

	namespace {
		void HashCombine(std::uint64_t& seed, std::uint64_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		std::uint64_t HashUuid(const TerraEngine::UUID& id)
		{
			return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
		}

		template<typename Record>
		void SortByUuid(std::vector<Record>& records, TerraEngine::UUID Record::* member)
		{
			std::sort(records.begin(), records.end(), [member](const Record& left, const Record& right) {
				return TerraEngine::ToHexString(left.*member) < TerraEngine::ToHexString(right.*member);
			});
		}

		void HashVirtualGeometryRange(std::uint64_t& hash, const VirtualGeometryDrawRange& range)
		{
			HashCombine(hash, range.selectedLodIndex);
			HashCombine(hash, range.meshletOffset);
			HashCombine(hash, range.meshletCount);
			HashCombine(hash, range.virtualGeometryActive ? 1u : 0u);
			HashCombine(hash, range.failOpen ? 1u : 0u);
			HashCombine(hash, range.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(range.hiZInactiveReason)));
			HashCombine(hash, range.selectedClusterOffset);
			HashCombine(hash, range.selectedClusterCount);
			HashCombine(hash, range.pageNeedOffset);
			HashCombine(hash, range.pageNeedCount);
		}
	}

	void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot)
	{
		// Stable sort points make repeated frames produce identical work packet
		// ids and pass ordering even when scene registry iteration order changes.
		std::sort(snapshot.viewRequests.begin(), snapshot.viewRequests.end(), [](const RenderFrameViewRequest& left, const RenderFrameViewRequest& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		SortByUuid(snapshot.renderables, &RenderFrameRenderableRecord::objectId);
		SortByUuid(snapshot.lights, &RenderFrameLightRecord::lightId);
		SortByUuid(snapshot.materials, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.meshes, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.textures, &RenderFrameResourceRecord::resourceId);
		std::sort(snapshot.virtualGeometryViewSelections.begin(), snapshot.virtualGeometryViewSelections.end(), [](const VirtualGeometryViewSelectionState& left, const VirtualGeometryViewSelectionState& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		for (VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			std::sort(state.draws.begin(), state.draws.end(), [](const VirtualGeometryViewDrawRecord& left, const VirtualGeometryViewDrawRecord& right) {
				return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
			});
			std::sort(state.selectedClusters.begin(), state.selectedClusters.end(), [](const VirtualGeometrySelectedClusterRecord& left, const VirtualGeometrySelectedClusterRecord& right) {
				if (left.clusterId != right.clusterId)
				{
					return left.clusterId < right.clusterId;
				}
				return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
			});
			std::sort(state.pageNeeds.begin(), state.pageNeeds.end(), [](const VirtualGeometryPageNeedRecord& left, const VirtualGeometryPageNeedRecord& right) {
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
			});
		}
	}

	bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot)
	{
		auto allResourcesResolved = [](const std::vector<RenderFrameResourceRecord>& records) {
			return std::all_of(records.begin(), records.end(), [](const RenderFrameResourceRecord& record) {
				return record.resourceId != TerraEngine::EMPTY_UUID && (record.resident || record.usingFallback);
			});
		};

		return snapshot.valid
			&& !SnapshotContainsLiveMutableState(snapshot)
			&& allResourcesResolved(snapshot.materials)
			&& allResourcesResolved(snapshot.meshes)
			&& allResourcesResolved(snapshot.textures);
	}

	bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot)
	{
		return snapshot.containsLiveMutableState;
	}

	std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t hash = snapshot.frameId;
		HashCombine(hash, snapshot.sceneRevision);
		HashCombine(hash, snapshot.resourceRevision);
		HashCombine(hash, snapshot.valid ? 1u : 0u);
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			HashCombine(hash, view.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(view.viewType));
			HashCombine(hash, view.targetSize.x);
			HashCombine(hash, view.targetSize.y);
		}
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			HashCombine(hash, HashUuid(renderable.objectId));
			HashCombine(hash, HashUuid(renderable.meshId));
			HashCombine(hash, HashUuid(renderable.materialId));
			HashVirtualGeometryRange(hash, renderable.virtualGeometry);
			HashCombine(hash, renderable.taskShaderCullingEnabled ? 1u : 0u);
			HashCombine(hash, renderable.usingFallback ? 1u : 0u);
		}
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			HashCombine(hash, state.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.targetSize.x);
			HashCombine(hash, state.targetSize.y);
			HashCombine(hash, state.shadowPass ? 1u : 0u);
			HashCombine(hash, state.exportRuntime ? 1u : 0u);
			HashCombine(hash, state.hiZOcclusionAllowed ? 1u : 0u);
			HashCombine(hash, state.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(state.hiZInactiveReason)));
			for (const VirtualGeometryViewDrawRecord& draw : state.draws)
			{
				HashCombine(hash, HashUuid(draw.objectId));
				HashCombine(hash, HashUuid(draw.meshId));
				HashVirtualGeometryRange(hash, draw.range);
				HashCombine(hash, draw.taskShaderCullingEnabled ? 1u : 0u);
			}
			for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
			{
				HashCombine(hash, HashUuid(cluster.objectId));
				HashCombine(hash, HashUuid(cluster.meshId));
				HashCombine(hash, cluster.clusterId);
				HashCombine(hash, cluster.lodIndex);
				HashCombine(hash, cluster.pageId);
				HashCombine(hash, cluster.fallbackClusterId);
				HashCombine(hash, cluster.usingFallback ? 1u : 0u);
			}
			for (const VirtualGeometryPageNeedRecord& need : state.pageNeeds)
			{
				HashCombine(hash, HashUuid(need.objectId));
				HashCombine(hash, HashUuid(need.meshId));
				HashCombine(hash, need.pageId);
				HashCombine(hash, need.clusterId);
				HashCombine(hash, need.desiredLodIndex);
				HashCombine(hash, need.fallbackRequired ? 1u : 0u);
			}
		}
		for (const RenderFrameLightRecord& light : snapshot.lights)
		{
			HashCombine(hash, HashUuid(light.lightId));
			HashCombine(hash, light.lightType);
			HashCombine(hash, light.castsShadows ? 1u : 0u);
		}
		return hash;
	}

	std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot)
	{
		// The estimate is intentionally backend-neutral. It reflects CPU work
		// units the graph can split, not GPU timings or API-specific commands.
		std::uint64_t virtualGeometryClusterWork = 0u;
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			virtualGeometryClusterWork += static_cast<std::uint64_t>(state.selectedClusters.size() + state.pageNeeds.size());
		}
		return static_cast<std::uint64_t>(snapshot.renderables.size())
			+ static_cast<std::uint64_t>(snapshot.lights.size())
			+ static_cast<std::uint64_t>(snapshot.viewRequests.size() * 8u)
			+ static_cast<std::uint64_t>(snapshot.virtualGeometryViewSelections.size() * 4u)
			+ virtualGeometryClusterWork;
	}

	VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables)
	{
		VirtualGeometryViewSelectionState state;
		state.outputKind = view.outputKind;
		state.viewType = view.outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : view.viewType;
		state.targetSize = view.targetSize;
		state.outputId = view.outputId;
		state.shadowPass = view.viewType == RenderViewType::Shadow;
		state.exportRuntime = view.viewType == RenderViewType::ExportRuntime || view.outputKind == RenderOutputKind::MainRuntimeView;
		state.hiZOcclusionAllowed = true;
		state.hiZInactiveReason.clear();
		state.draws.reserve(renderables.size());
		for (const RenderFrameRenderableRecord& renderable : renderables)
		{
			if (!renderable.virtualGeometry.virtualGeometryActive
				&& renderable.virtualGeometry.meshletCount == 0u
				&& !renderable.virtualGeometry.failOpen)
			{
				continue;
			}

			state.draws.push_back({
				renderable.objectId,
				renderable.meshId,
				renderable.virtualGeometry,
				renderable.taskShaderCullingEnabled
			});
		}
		return state;
	}

	std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot)
	{
		if (!snapshot.virtualGeometryViewSelections.empty())
		{
			// Explicit per-view selections are authored by RenderSystem after each
			// visibility pass. Returning a copy preserves immutability for editor
			// diagnostics and prevents one view from mutating another view's range.
			return snapshot.virtualGeometryViewSelections;
		}

		std::vector<VirtualGeometryViewSelectionState> states;
		states.reserve(snapshot.viewRequests.size());
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			states.push_back(BuildVirtualGeometryViewSelectionState(view, snapshot.renderables));
		}
		return states;
	}

	std::vector<VirtualGeometryViewDiagnosticsSnapshot> BuildVirtualGeometryViewDiagnosticsFromSnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryQualityProfile& qualityProfile,
		VirtualGeometryInactiveReason inactiveReason)
	{
		const VirtualGeometryQualityProfile quality = SanitizeVirtualGeometryQualityProfile(qualityProfile);
		std::vector<VirtualGeometryViewSelectionState> states = BuildVirtualGeometryViewSelectionStates(snapshot);
		std::vector<VirtualGeometryViewDiagnosticsSnapshot> diagnostics;
		diagnostics.reserve(states.size());
		for (const VirtualGeometryViewSelectionState& state : states)
		{
			VirtualGeometryViewDiagnosticsSnapshot viewDiagnostics;
			viewDiagnostics.viewType = state.viewType;
			viewDiagnostics.frameId = snapshot.frameId;
			viewDiagnostics.active = true;
			viewDiagnostics.inactiveReason = VirtualGeometryInactiveReason::None;
			viewDiagnostics.hiZActive = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZHistoryValid = viewDiagnostics.hiZActive;
			viewDiagnostics.effectiveLodErrorScale = quality.lodErrorScale;
			viewDiagnostics.configuredMaxLodDrop = quality.maxLodDrop;
			viewDiagnostics.geometryBudgetBytes = quality.geometryBudgetBytes;
			viewDiagnostics.clusterSelection.selectedClusters = static_cast<std::uint32_t>(state.selectedClusters.size());
			viewDiagnostics.requestedPages = static_cast<std::uint32_t>(state.pageNeeds.size());
			viewDiagnostics.missingPages = static_cast<std::uint32_t>(state.pageNeeds.size());
			for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
			{
				if (cluster.usingFallback)
				{
					++viewDiagnostics.clusterSelection.fallbackClusters;
				}
			}
			if (!state.pageNeeds.empty())
			{
				viewDiagnostics.warnings.push_back("Virtual geometry selected resident fallback clusters while requesting missing high-detail pages.");
			}
			viewDiagnostics.hiZValidation.ownerViewType = state.viewType;
			viewDiagnostics.hiZValidation.sameViewHistory = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZValidation.sameTargetHistory = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZValidation.projectionMatches = viewDiagnostics.hiZActive;
			viewDiagnostics.hiZValidation.failOpen = !viewDiagnostics.hiZActive;
			viewDiagnostics.hiZValidation.inactiveReason = viewDiagnostics.hiZInvalidReason;
			for (const VirtualGeometryViewDrawRecord& draw : state.draws)
			{
				viewDiagnostics.selectedMeshlets += draw.range.meshletCount;
				viewDiagnostics.dispatchedMeshlets += draw.range.meshletCount;
				if (viewDiagnostics.selectedLodHistogram.size() <= draw.range.selectedLodIndex)
				{
					viewDiagnostics.selectedLodHistogram.resize(static_cast<std::size_t>(draw.range.selectedLodIndex) + 1u, 0u);
				}
				viewDiagnostics.selectedLodHistogram[draw.range.selectedLodIndex] += 1u;
				if (draw.range.failOpen)
				{
					viewDiagnostics.warnings.push_back(draw.range.fallbackReason.empty()
						? "Virtual geometry failed open to preserve visibility."
						: draw.range.fallbackReason);
				}
			}
			viewDiagnostics.sourceMeshlets = std::max(viewDiagnostics.sourceMeshlets, viewDiagnostics.selectedMeshlets);
			if (state.shadowPass)
			{
				viewDiagnostics.warnings.push_back("Shadow virtual geometry uses conservative LOD; camera Hi-Z ownership is diagnostic-only.");
			}
			if (state.exportRuntime)
			{
				viewDiagnostics.warnings.push_back("Export runtime virtual geometry state is isolated from editor Scene View diagnostics.");
			}
			diagnostics.push_back(std::move(viewDiagnostics));
		}
		return diagnostics;
	}

}
