#include "RenderFrameSnapshot.h"
#include "RenderDiagnostics.h"

#include <cmath>
#include <functional>
#include <utility>

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

		void HashVirtualGeometryCounters(std::uint64_t& hash, const VirtualGeometryCounterSnapshot& counters)
		{
			HashCombine(hash, static_cast<std::uint64_t>(counters.source));
			HashCombine(hash, counters.preparedFrameId);
			HashCombine(hash, counters.readbackFrameId);
			HashCombine(hash, counters.readbackLatencyFrames);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(counters.unavailableReason)));
			HashCombine(hash, counters.toleranceMeshlets);
			HashCombine(hash, static_cast<std::uint32_t>(std::round(counters.tolerancePercent * 1000.0f)));
			HashCombine(hash, counters.sourceMeshlets);
			HashCombine(hash, counters.selectedMeshlets);
			HashCombine(hash, counters.dispatchedMeshlets);
			HashCombine(hash, counters.culledMeshlets);
			HashCombine(hash, counters.culledByFrustum);
			HashCombine(hash, counters.culledByCone);
			HashCombine(hash, counters.culledByHiZ);
			HashCombine(hash, counters.fallbackMeshlets);
			HashCombine(hash, counters.residentPages);
			HashCombine(hash, counters.requestedPages);
			HashCombine(hash, counters.missingPages);
			HashCombine(hash, counters.fallbackSubstitutions);
			HashCombine(hash, counters.authoritative ? 1u : 0u);
		}

		void HashVirtualGeometryHiZState(std::uint64_t& hash, const VirtualGeometryHiZValidationState& state)
		{
			HashCombine(hash, static_cast<std::uint64_t>(state.state));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(state.reason)));
			HashCombine(hash, state.pyramidSize.x);
			HashCombine(hash, state.pyramidSize.y);
			HashCombine(hash, state.renderSize.x);
			HashCombine(hash, state.renderSize.y);
			HashCombine(hash, state.cameraSignature);
			HashCombine(hash, state.depthSceneSignature);
			HashCombine(hash, state.warmupFrames);
			HashCombine(hash, state.pyramidReady ? 1u : 0u);
			HashCombine(hash, state.failOpen ? 1u : 0u);
		}

		void HashVirtualGeometryOcclusionSample(std::uint64_t& hash, const OcclusionDebugSample& sample)
		{
			HashCombine(hash, HashUuid(sample.objectId));
			HashCombine(hash, HashUuid(sample.meshId));
			HashCombine(hash, sample.clusterId);
			HashCombine(hash, sample.meshletId);
			HashCombine(hash, sample.sampledMip);
			HashCombine(hash, static_cast<std::uint64_t>(sample.outcome));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(sample.reason)));
		}

		void HashVirtualGeometryLODTransitionState(std::uint64_t& hash, const VirtualGeometryLODTransitionState& state)
		{
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.outputId);
			HashCombine(hash, HashUuid(state.objectId));
			HashCombine(hash, HashUuid(state.meshId));
			HashCombine(hash, state.viewStateKey);
			HashCombine(hash, state.lastStableSelectionSignature);
			HashCombine(hash, state.candidateSelectionSignature);
			HashCombine(hash, state.stableFrameCount);
			HashCombine(hash, state.heldFrameCount);
			HashCombine(hash, state.acceptedFrameCount);
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.hysteresisBandPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.transitionMarginPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.candidateMaxErrorPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, state.heldPreviousSelection ? 1u : 0u);
			HashCombine(hash, state.resetThisFrame ? 1u : 0u);
			HashCombine(hash, state.valid ? 1u : 0u);
		}

		std::uint64_t BuildVirtualGeometryCameraSignature(const RenderFrameSnapshot& snapshot, const VirtualGeometryViewSelectionState& state)
		{
			// The foundation stage has no live camera object in this value layer.
			// Hashing immutable view identity gives diagnostics a stable
			// placeholder until backend-specific camera signatures are published.
			std::uint64_t signature = snapshot.frameId;
			HashCombine(signature, snapshot.sceneRevision);
			HashCombine(signature, snapshot.resourceRevision);
			HashCombine(signature, state.outputId);
			HashCombine(signature, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(signature, static_cast<std::uint64_t>(state.viewType));
			HashCombine(signature, state.targetSize.x);
			HashCombine(signature, state.targetSize.y);
			return signature;
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
			std::sort(state.lodTransitionStates.begin(), state.lodTransitionStates.end(), [](const VirtualGeometryLODTransitionState& left, const VirtualGeometryLODTransitionState& right) {
				if (left.outputId != right.outputId)
				{
					return left.outputId < right.outputId;
				}
				return TerraEngine::ToHexString(left.objectId) < TerraEngine::ToHexString(right.objectId);
			});
		}
		std::sort(snapshot.virtualGeometryVisibilitySnapshots.begin(), snapshot.virtualGeometryVisibilitySnapshots.end(), [](const VirtualGeometryViewVisibilitySnapshot& left, const VirtualGeometryViewVisibilitySnapshot& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		for (VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			std::sort(state.drawRanges.begin(), state.drawRanges.end(), [](const VirtualGeometryViewDrawRecord& left, const VirtualGeometryViewDrawRecord& right) {
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
			std::sort(state.lodTransitionStates.begin(), state.lodTransitionStates.end(), [](const VirtualGeometryLODTransitionState& left, const VirtualGeometryLODTransitionState& right) {
				if (left.outputId != right.outputId)
				{
					return left.outputId < right.outputId;
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
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, state.shadowPass ? 1u : 0u);
			HashCombine(hash, state.exportRuntime ? 1u : 0u);
			HashCombine(hash, state.hiZOcclusionAllowed ? 1u : 0u);
			HashCombine(hash, state.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(state.hiZInactiveReason)));
			HashCombine(hash, state.hasHiZValidationState ? 1u : 0u);
			if (state.hasHiZValidationState)
			{
				HashVirtualGeometryHiZState(hash, state.hiZValidationState);
			}
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
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				HashVirtualGeometryLODTransitionState(hash, transitionState);
			}
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			HashCombine(hash, state.frameId);
			HashCombine(hash, state.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.renderSize.x);
			HashCombine(hash, state.renderSize.y);
			HashCombine(hash, state.cameraSignature);
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, static_cast<std::uint64_t>(state.source));
			HashCombine(hash, state.valid ? 1u : 0u);
			HashVirtualGeometryCounters(hash, state.counters);
			HashVirtualGeometryHiZState(hash, state.hiZValidationState);
			for (const OcclusionDebugSample& sample : state.occlusionDebugSamples)
			{
				HashVirtualGeometryOcclusionSample(hash, sample);
			}
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				HashVirtualGeometryLODTransitionState(hash, transitionState);
			}
			for (const VirtualGeometryViewDrawRecord& draw : state.drawRanges)
			{
				HashCombine(hash, HashUuid(draw.objectId));
				HashCombine(hash, HashUuid(draw.meshId));
				HashVirtualGeometryRange(hash, draw.range);
				HashCombine(hash, draw.taskShaderCullingEnabled ? 1u : 0u);
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
			virtualGeometryClusterWork += static_cast<std::uint64_t>(state.selectedClusters.size() + state.pageNeeds.size() + state.lodTransitionStates.size());
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			virtualGeometryClusterWork += static_cast<std::uint64_t>(state.drawRanges.size() + state.selectedClusters.size() + state.pageNeeds.size() + state.lodTransitionStates.size());
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

	VirtualGeometryCounterSnapshot BuildVirtualGeometryCounterSnapshot(const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source)
	{
		VirtualGeometryCounterSnapshot counters;
		counters.source = source;
		counters.authoritative = source == VirtualGeometryCounterSource::GPUReadback;
		switch (source)
		{
		case VirtualGeometryCounterSource::Prepared:
			counters.unavailableReason = "prepared CPU estimate before GPU readback";
			break;
		case VirtualGeometryCounterSource::GPUReadback:
			counters.unavailableReason.clear();
			break;
		case VirtualGeometryCounterSource::DelayedGPUReadback:
			counters.readbackLatencyFrames = 1u;
			counters.unavailableReason = "GPU readback is delayed; showing prepared estimate";
			break;
		case VirtualGeometryCounterSource::Unsupported:
			counters.unavailableReason = "backend does not support virtual geometry counter readback";
			break;
		case VirtualGeometryCounterSource::Unavailable:
			counters.unavailableReason = "virtual geometry counters are unavailable for this view";
			break;
		}
		for (const VirtualGeometryViewDrawRecord& draw : state.draws)
		{
			counters.sourceMeshlets += draw.range.meshletCount;
			counters.selectedMeshlets += draw.range.meshletCount;
			counters.dispatchedMeshlets += draw.range.meshletCount;
			if (draw.range.failOpen)
			{
				counters.fallbackMeshlets += draw.range.meshletCount;
			}
		}
		for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
		{
			if (cluster.usingFallback)
			{
				++counters.fallbackSubstitutions;
			}
		}
		counters.requestedPages = static_cast<std::uint32_t>(state.pageNeeds.size());
		counters.missingPages = counters.requestedPages;
		return counters;
	}

	std::vector<OcclusionDebugSample> BuildVirtualGeometryOcclusionDebugSamples(const VirtualGeometryViewSelectionState& state)
	{
		std::vector<OcclusionDebugSample> samples;
		samples.reserve(std::min<std::size_t>(state.draws.size(), kVirtualGeometryOcclusionDebugSampleCapacity));
		for (const VirtualGeometryViewDrawRecord& draw : state.draws)
		{
			if (samples.size() >= kVirtualGeometryOcclusionDebugSampleCapacity)
			{
				break;
			}
			if (!draw.range.virtualGeometryActive && !draw.range.failOpen)
			{
				continue;
			}

			OcclusionDebugSample sample;
			sample.objectId = draw.objectId;
			sample.meshId = draw.meshId;
			sample.meshletId = draw.range.meshletOffset;
			if (draw.range.selectedClusterCount > 0u
				&& draw.range.selectedClusterOffset < state.selectedClusters.size())
			{
				const VirtualGeometrySelectedClusterRecord& cluster = state.selectedClusters[draw.range.selectedClusterOffset];
				sample.clusterId = cluster.clusterId;
			}
			sample.outcome = draw.range.failOpen
				? VirtualGeometryCullOutcome::FailOpen
				: VirtualGeometryCullOutcome::Visible;
			sample.reason = draw.range.failOpen
				? (draw.range.fallbackReason.empty() ? "Virtual geometry failed open to preserve visibility." : draw.range.fallbackReason)
				: "Selected meshlet remained visible; Hi-Z may only cull after full projected-bounds proof.";
			samples.push_back(std::move(sample));
		}
		return samples;
	}

	VirtualGeometryHiZValidationState BuildDefaultVirtualGeometryHiZValidationState(const VirtualGeometryViewSelectionState& state, std::uint64_t cameraSignature)
	{
		VirtualGeometryHiZValidationState hiZ;
		hiZ.renderSize = state.targetSize;
		hiZ.cameraSignature = cameraSignature;
		hiZ.depthSceneSignature = cameraSignature ^ 0x517cc1b727220a95ull;
		hiZ.pyramidReady = false;
		hiZ.failOpen = true;
		if (state.shadowPass || state.viewType == RenderViewType::Shadow)
		{
			hiZ.state = VirtualGeometryHiZState::Unsupported;
			hiZ.reason = "Shadow virtual geometry does not sample camera Hi-Z.";
			return hiZ;
		}
		if (state.targetSize.x == 0u || state.targetSize.y == 0u)
		{
			hiZ.state = VirtualGeometryHiZState::MissingPyramid;
			hiZ.reason = "Render target size is unavailable, so no current Hi-Z pyramid can be sampled.";
			return hiZ;
		}
		if (!state.hiZOcclusionAllowed)
		{
			hiZ.state = VirtualGeometryHiZState::FailOpen;
			hiZ.reason = state.hiZInactiveReason.empty()
				? "Hi-Z occlusion is failing open for this view."
				: state.hiZInactiveReason;
			return hiZ;
		}

		hiZ.state = VirtualGeometryHiZState::Active;
		hiZ.pyramidSize = state.targetSize;
		hiZ.warmupFrames = 1u;
		hiZ.pyramidReady = true;
		hiZ.failOpen = false;
		return hiZ;
	}

	VirtualGeometryViewVisibilitySnapshot BuildVirtualGeometryViewVisibilitySnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryViewSelectionState& state,
		VirtualGeometryCounterSource source)
	{
		VirtualGeometryViewVisibilitySnapshot visibility;
		visibility.outputKind = state.outputKind;
		visibility.viewType = state.viewType;
		visibility.frameId = snapshot.frameId;
		visibility.renderSize = state.targetSize;
		visibility.outputId = state.outputId;
		visibility.cameraSignature = BuildVirtualGeometryCameraSignature(snapshot, state);
		visibility.debugVisualizationMode = state.debugVisualizationMode;
		visibility.counters = BuildVirtualGeometryCounterSnapshot(state, source);
		visibility.counters.preparedFrameId = snapshot.frameId;
		if (source == VirtualGeometryCounterSource::GPUReadback)
		{
			visibility.counters.readbackFrameId = snapshot.frameId;
		}
		else if (source == VirtualGeometryCounterSource::DelayedGPUReadback)
		{
			visibility.counters.readbackFrameId = snapshot.frameId > 0u ? snapshot.frameId - 1u : 0u;
			visibility.counters.readbackLatencyFrames = snapshot.frameId > 0u ? 1u : 0u;
		}
		visibility.hiZValidationState = state.hasHiZValidationState
			? state.hiZValidationState
			: BuildDefaultVirtualGeometryHiZValidationState(state, visibility.cameraSignature);
		if (visibility.hiZValidationState.cameraSignature == 0u)
		{
			visibility.hiZValidationState.cameraSignature = visibility.cameraSignature;
		}
		else
		{
			visibility.cameraSignature = visibility.hiZValidationState.cameraSignature;
		}
		if (visibility.hiZValidationState.renderSize == glm::uvec2(0u))
		{
			visibility.hiZValidationState.renderSize = state.targetSize;
		}
		visibility.drawRanges = state.draws;
		visibility.selectedClusters = state.selectedClusters;
		visibility.pageNeeds = state.pageNeeds;
		visibility.lodTransitionStates = state.lodTransitionStates;
		visibility.occlusionDebugSamples = BuildVirtualGeometryOcclusionDebugSamples(state);
		visibility.source = source;
		visibility.valid = snapshot.valid;
		return visibility;
	}

	std::vector<VirtualGeometryViewVisibilitySnapshot> BuildVirtualGeometryViewVisibilitySnapshots(const RenderFrameSnapshot& snapshot)
	{
		if (!snapshot.virtualGeometryVisibilitySnapshots.empty())
		{
			// RenderSystem can publish backend-populated visibility records. Tests
			// receive copies so they can verify independence without touching the
			// protected renderer-owned publication buffer.
			return snapshot.virtualGeometryVisibilitySnapshots;
		}

		std::vector<VirtualGeometryViewSelectionState> states = BuildVirtualGeometryViewSelectionStates(snapshot);
		std::vector<VirtualGeometryViewVisibilitySnapshot> visibility;
		visibility.reserve(states.size());
		for (const VirtualGeometryViewSelectionState& state : states)
		{
			visibility.push_back(BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::Prepared));
		}
		return visibility;
	}

	std::vector<VirtualGeometryViewDiagnosticsSnapshot> BuildVirtualGeometryViewDiagnosticsFromSnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryQualityProfile& qualityProfile,
		VirtualGeometryInactiveReason inactiveReason)
	{
		const VirtualGeometryQualityProfile quality = SanitizeVirtualGeometryQualityProfile(qualityProfile);
		if (!snapshot.virtualGeometryVisibilitySnapshots.empty())
		{
			std::vector<VirtualGeometryViewDiagnosticsSnapshot> diagnostics;
			diagnostics.reserve(snapshot.virtualGeometryVisibilitySnapshots.size());
			for (const VirtualGeometryViewVisibilitySnapshot& visibility : snapshot.virtualGeometryVisibilitySnapshots)
			{
				VirtualGeometryViewDiagnosticsSnapshot viewDiagnostics =
					BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);
				if (inactiveReason != VirtualGeometryInactiveReason::None)
				{
					viewDiagnostics.active = false;
					viewDiagnostics.inactiveReason = inactiveReason;
					viewDiagnostics.warnings.push_back("Virtual geometry counters are unavailable because the view is inactive: " + std::string(ToString(inactiveReason)));
				}
				diagnostics.push_back(std::move(viewDiagnostics));
			}
			return diagnostics;
		}

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
			viewDiagnostics.counterSource = VirtualGeometryCounterSource::Prepared;
			VirtualGeometryHiZValidationState hiZState = state.hasHiZValidationState
				? state.hiZValidationState
				: BuildDefaultVirtualGeometryHiZValidationState(
					state,
					BuildVirtualGeometryCameraSignature(snapshot, state));
			if (hiZState.cameraSignature == 0u)
			{
				hiZState.cameraSignature = BuildVirtualGeometryCameraSignature(snapshot, state);
			}
			viewDiagnostics.hiZState = hiZState.state;
			viewDiagnostics.hiZPyramidSize = hiZState.pyramidSize;
			viewDiagnostics.hiZRenderSize = hiZState.renderSize;
			viewDiagnostics.hiZCameraSignature = hiZState.cameraSignature;
			viewDiagnostics.hiZDepthSceneSignature = hiZState.depthSceneSignature;
			viewDiagnostics.hiZWarmupFrames = hiZState.warmupFrames;
			if (hiZState.state != VirtualGeometryHiZState::Active)
			{
				viewDiagnostics.hiZInvalidReason = hiZState.reason;
				viewDiagnostics.hiZActive = false;
				viewDiagnostics.hiZHistoryValid = false;
			}
			viewDiagnostics.effectiveLodErrorScale = quality.lodErrorScale;
			viewDiagnostics.configuredMaxLodDrop = quality.maxLodDrop;
			viewDiagnostics.lodTransitionHysteresisPixels = quality.lodTransitionHysteresisPixels;
			viewDiagnostics.lodTransitionStableFrames = quality.lodTransitionStableFrames;
			viewDiagnostics.geometryBudgetBytes = quality.geometryBudgetBytes;
			viewDiagnostics.clusterSelection.selectedClusters = static_cast<std::uint32_t>(state.selectedClusters.size());
			viewDiagnostics.requestedPages = static_cast<std::uint32_t>(state.pageNeeds.size());
			viewDiagnostics.missingPages = static_cast<std::uint32_t>(state.pageNeeds.size());
			viewDiagnostics.lodTransitionStates = static_cast<std::uint32_t>(state.lodTransitionStates.size());
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				if (transitionState.heldPreviousSelection)
				{
					++viewDiagnostics.lodTransitionHeldSelections;
				}
				if (!transitionState.heldPreviousSelection && transitionState.valid)
				{
					++viewDiagnostics.lodTransitionAcceptedSelections;
				}
				if (transitionState.resetThisFrame)
				{
					++viewDiagnostics.lodTransitionResets;
				}
				viewDiagnostics.lodTransitionHysteresisPixels = transitionState.hysteresisBandPixels;
			}
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
			viewDiagnostics.hiZValidation.failOpen = hiZState.failOpen || hiZState.state != VirtualGeometryHiZState::Active;
			viewDiagnostics.hiZValidation.inactiveReason = viewDiagnostics.hiZInvalidReason;
			viewDiagnostics.occlusionDebugSamples = BuildVirtualGeometryOcclusionDebugSamples(state);
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
