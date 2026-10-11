// Pure fixture controls consume copied cooked ranges and canonical positions.
#pragma once
#include "Renderer/RenderTypes.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include <unordered_map>

namespace TerraEngine::Renderer
{
	// Lighting controls are checked without accessing ECS storage. The protected
	// capture supplies the exact active membership before any budgets select it.
	inline bool ValidateRendererEfficiencyFixtureLightingControls(const RendererEfficiencyFixtureIntent& intent,
		std::string& reason)
	{
		if (!intent.lightingControls) { reason.clear(); return true; }
		const auto& controls = *intent.lightingControls;
		if (intent.correlation.operationId.empty() || controls.candidateOrder.size() > 4096U)
		{ reason = "Lighting controls require an owned bounded validation session."; return false; }
		if (controls.maxUploadedLights > 1024U || controls.maxDynamicLights > controls.maxUploadedLights
			|| controls.maxShadowLights > controls.maxDynamicLights)
		{ reason = "Lighting budgets exceed physical capacity or retained membership."; return false; }
		// This selects an existing capture slot; it never admits writes without
		// the backend's independent fence and physical-owner validation.
		if (controls.frameSlot && *controls.frameSlot >= 3U)
		{ reason = "Lighting capture targets an unavailable physical frame slot."; return false; }
		if (controls.snapshotSlot && (*controls.snapshotSlot >= 8U
			|| (controls.snapshotViewType != RenderViewType::Scene && controls.snapshotViewType != RenderViewType::Game)))
		{ reason = "Lighting snapshot control targets an unavailable physical slice or view."; return false; }
		auto identities = controls.candidateOrder;
		std::sort(identities.begin(), identities.end());
		if ((!identities.empty() && identities.front() == EMPTY_UUID)
			|| std::adjacent_find(identities.begin(), identities.end()) != identities.end())
		{ reason = "Lighting order contains unavailable or duplicate identities."; return false; }
		reason.clear(); return true;
	}
	inline bool ValidateRendererEfficiencyFixtureLightingControls(const RendererEfficiencyFixtureIntent& intent,
		std::span<const TerraEngine::UUID> actualActiveLightIds, std::string& reason)
	{
		if (!ValidateRendererEfficiencyFixtureLightingControls(intent, reason)) return false;
		if (!intent.lightingControls) return true;
		auto requested = intent.lightingControls->candidateOrder;
		std::vector<TerraEngine::UUID> actual(actualActiveLightIds.begin(), actualActiveLightIds.end());
		std::sort(requested.begin(), requested.end()); std::sort(actual.begin(), actual.end());
		if (requested != actual || (!actual.empty() && actual.front() == EMPTY_UUID)
			|| std::adjacent_find(actual.begin(), actual.end()) != actual.end())
		{ reason = "Lighting order is not the complete current active candidate membership."; return false; }
		reason.clear(); return true;
	}
	inline bool ApplyRendererEfficiencyFixtureLightingControls(const RendererEfficiencyFixtureIntent& intent,
		std::vector<FrameLightCandidate>& candidates, RenderSettings& settings, std::string& reason)
	{
		if (!intent.lightingControls) { reason.clear(); return true; }
		if (!intent.controlsApplied) { reason = "Lighting controls have not passed protected capture."; return false; }
		std::vector<TerraEngine::UUID> actual; actual.reserve(candidates.size());
		for (const auto& candidate : candidates) actual.push_back(candidate.lightId);
		if (!ValidateRendererEfficiencyFixtureLightingControls(intent, actual, reason)) return false;
		// Build the entire replacement before publishing either order or settings.
		// All allocations are worker-safe and failure leaves the caller's copies intact.
		std::unordered_map<TerraEngine::UUID, std::size_t> indices; indices.reserve(candidates.size());
		for (std::size_t index = 0; index < candidates.size(); ++index) indices.emplace(candidates[index].lightId, index);
		std::vector<FrameLightCandidate> ordered; ordered.reserve(candidates.size());
		for (const auto& id : intent.lightingControls->candidateOrder) ordered.push_back(candidates[indices.at(id)]);
		candidates.swap(ordered);
		settings.maxUploadedLights = intent.lightingControls->maxUploadedLights;
		settings.maxDynamicLights = intent.lightingControls->maxDynamicLights;
		settings.maxShadowLights = intent.lightingControls->maxShadowLights;
		reason.clear(); return true;
	}
	// Ingress validates copied identities without reading scene storage. Capture
	// later proves every requested identity against its protected target snapshot.
	inline bool ValidateRendererEfficiencyFixtureCasterSelections(const RendererEfficiencyFixtureIntent& intent, std::string& reason)
	{
		if (intent.casterSelections.empty()) { reason.clear(); return true; }
		if (intent.correlation.operationId.empty() || intent.casterSelections.size() > 2U)
		{ reason = "Caster controls require one owned bounded validation session."; return false; }
		for (std::size_t index = 0; index < intent.casterSelections.size(); ++index)
		{
			const auto& selected = intent.casterSelections[index];
			if (!((selected.viewType == RenderViewType::Scene && selected.outputId == 1U)
				|| (selected.viewType == RenderViewType::Game && selected.outputId == 2U))
				|| selected.objectIds.empty() || selected.objectIds.size() > 4096U)
			{ reason = "Caster allowlist targets an unsupported output or exceeds its bounded nonempty membership."; return false; }
			for (std::size_t previous = 0; previous < index; ++previous)
				if (intent.casterSelections[previous].viewType == selected.viewType && intent.casterSelections[previous].outputId == selected.outputId)
				{ reason = "Duplicate caster allowlists are ambiguous for one output."; return false; }
			auto identities = selected.objectIds;
			std::sort(identities.begin(), identities.end());
			if (identities.front() == EMPTY_UUID || std::adjacent_find(identities.begin(), identities.end()) != identities.end())
			{ reason = "Caster allowlist contains unavailable or duplicate object identities."; return false; }
		}
		reason.clear(); return true;
	}
	inline bool ValidateRendererEfficiencyFixtureCasterSelections(const RendererEfficiencyFixtureIntent& intent,
		std::span<const RendererEfficiencyFixtureTargetProof> targets, std::string& reason)
	{
		if (!ValidateRendererEfficiencyFixtureCasterSelections(intent, reason)) return false;
		for (const auto& selected : intent.casterSelections) for (const auto& id : selected.objectIds)
			if (std::none_of(targets.begin(), targets.end(), [&](const auto& target) { return target.objectId == id && target.lifetimeGeneration != 0; }))
			{ reason = "Caster allowlist identity is absent from the protected current target snapshot."; return false; }
		reason.clear(); return true;
	}
	inline const RendererEfficiencyFixtureCasterSelection* ResolveRendererEfficiencyFixtureCasterSelection(
		const RendererEfficiencyFixtureIntent& intent, RenderViewType viewType, std::uint32_t outputId)
	{
		if (!intent.controlsApplied) return nullptr;
		const auto selected = std::find_if(intent.casterSelections.begin(), intent.casterSelections.end(), [&](const auto& row) {
			return row.viewType == viewType && row.outputId == outputId;
		});
		return selected == intent.casterSelections.end() ? nullptr : std::addressof(*selected);
	}
	inline void FilterRendererEfficiencyFixtureShadowCasters(std::vector<RenderItemRecord>& commands,
		const RendererEfficiencyFixtureCasterSelection* selected)
	{
		// Filter only the already-qualified light caster commands. Never create a
		// command for an allowlisted object that the real spatial query excluded.
		if (!selected) return;
		std::erase_if(commands, [&](const auto& command) {
			return std::find(selected->objectIds.begin(), selected->objectIds.end(), command.objectId) == selected->objectIds.end();
		});
	}
	inline bool ResolveRendererEfficiencyFixtureViewOrder(const RendererEfficiencyFixtureIntent& intent,
		std::span<const RendererEfficiencyFixtureViewOrderEntry> actual, std::vector<std::size_t>& output, std::string& reason)
	{
		const auto same = [](const auto& a, const auto& b) { return a.viewType == b.viewType && a.outputId == b.outputId; };
		const bool controlled = !intent.viewOrder.empty() || !intent.viewOrigins.empty() || !intent.casterSelections.empty();
		if (controlled && intent.correlation.operationId.empty()) { reason = "View controls require an owned validation session."; return false; }
		if (intent.referenceOriginDelta && !intent.viewOrigins.empty()) { reason = "Shared and per-view reference origins are ambiguous."; return false; }
		if (controlled) for (std::size_t i = 0; i < actual.size(); ++i)
			for (std::size_t j = 0; j < i; ++j) if (same(actual[i], actual[j])) { reason = "Actual output identity is not unique."; return false; }
		for (const auto& selection : intent.casterSelections)
			if (std::none_of(actual.begin(), actual.end(), [&](const auto& row) { return same(selection, row); }))
			{ reason = "Caster allowlist targets an unavailable actual output."; return false; }
		for (std::size_t i = 0; i < intent.viewOrigins.size(); ++i)
		{
			const auto& origin = intent.viewOrigins[i];
			if (std::none_of(actual.begin(), actual.end(), [&](const auto& row) { return same(origin, row); })) { reason = "Per-view origin targets an unavailable actual output."; return false; }
			for (std::size_t j = 0; j < i; ++j) if (same(origin, intent.viewOrigins[j])) { reason = "Duplicate per-view origin controls."; return false; }
		}
		std::vector<std::size_t> indices;
		if (intent.viewOrder.empty()) for (std::size_t i = 0; i < actual.size(); ++i) indices.push_back(i);
		else
		{
			if (intent.viewOrder.size() != actual.size()) { reason = "View order is not a complete actual output permutation."; return false; }
			for (const auto& requested : intent.viewOrder)
			{
				const auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& row) { return same(requested, row); });
				if (found == actual.end()) { reason = "View order references an unavailable output."; return false; }
				const auto index = static_cast<std::size_t>(found - actual.begin());
				if (std::find(indices.begin(), indices.end(), index) != indices.end()) { reason = "View order duplicates an actual output."; return false; }
				indices.push_back(index);
			}
		}
		output = std::move(indices); reason.clear(); return true;
	}
	inline std::optional<TerraEngine::Scene::WorldDelta> RendererEfficiencyFixtureOriginDelta(
		const RendererEfficiencyFixtureIntent& intent, RenderViewType viewType, std::uint32_t outputId)
	{
		for (const auto& origin : intent.viewOrigins) if (origin.viewType == viewType && origin.outputId == outputId) return origin.delta;
		return intent.referenceOriginDelta;
	}
	inline bool IsCurrentRendererEfficiencyFixtureEvidence(const RendererEfficiencyEvidence& row,
		const RendererEfficiencyOptionsSnapshot& options, const RendererEfficiencyFixtureIntent& intent)
	{
		return row.effectiveOptions.rendererSessionGeneration == options.rendererSessionGeneration
			&& row.effectiveOptions.optionsSessionGeneration == options.optionsSessionGeneration
			&& row.fixtureCorrelation.operationId == intent.correlation.operationId;
	}
	inline bool HasRendererEfficiencyFixtureControlAcknowledgment(std::span<const RendererEfficiencyEvidence> rows,
		const RendererEfficiencyFixtureIntent& intent)
	{
		return intent.controlsApplied && intent.controlGeneration != 0 && std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
			return row.status != RendererEfficiencyEvidenceStatus::Pending && row.commandSerial != 0
				&& row.fixtureControlGeneration == intent.controlGeneration
				&& row.fixtureCorrelation.operationId == intent.correlation.operationId
				&& row.fixtureCorrelation.fixtureStepId == intent.correlation.fixtureStepId
				&& row.fixtureCorrelation.rendererSessionGeneration == intent.correlation.rendererSessionGeneration
				&& row.fixtureCorrelation.settingsGeneration == intent.correlation.settingsGeneration;
		});
	}
	inline void ResetRendererEfficiencyFixtureEvidenceBatch(RendererEfficiencyEvidenceBatch& batch)
	{
		batch.rows.clear(); ++batch.batchSequence;
	}
	inline bool ResolveRendererEfficiencyFixtureSelection(const RendererEfficiencyFixtureShadowSelection& selection,
		const RendererEfficiencyFixtureTargetProof& proof, RendererEfficiencyFixtureDetailRange& result, std::string& reason)
	{
		if (!proof.ownerGeometrySupported || !proof.meshletCapacity || !proof.lifetimeGeneration || !proof.resourceGeneration
			|| selection.objectId != proof.objectId || selection.meshId != proof.meshId
			|| selection.lifetimeGeneration != proof.lifetimeGeneration || selection.resourceGeneration != proof.resourceGeneration)
		{ reason = "Fixture target identity, cooked owner route or publication is unavailable."; return false; }
		const auto detail = std::find_if(proof.detailLevels.begin(), proof.detailLevels.end(), [&](const auto& row) { return row.lodIndex == selection.lodIndex; });
		if (detail == proof.detailLevels.end() || !detail->meshletCount || detail->meshletOffset > proof.meshletCapacity
			|| detail->meshletCount > proof.meshletCapacity - detail->meshletOffset)
		{ reason = "Requested cooked detail level is unavailable."; return false; }
		auto range = *detail;
		if (selection.kind == RendererEfficiencyFixtureSelectionKind::MeshletRange)
		{
			if (!selection.meshletCount || selection.meshletOffset < detail->meshletOffset
				|| selection.meshletOffset - detail->meshletOffset >= detail->meshletCount
				|| selection.meshletCount > detail->meshletCount - (selection.meshletOffset - detail->meshletOffset))
			{ reason = "Requested meshlet subset exceeds its actual cooked detail range."; return false; }
			range.meshletOffset = selection.meshletOffset; range.meshletCount = selection.meshletCount;
		}
		else if (selection.kind != RendererEfficiencyFixtureSelectionKind::DetailLevel)
		{ reason = "Unknown fixture shadow selection kind."; return false; }
		result = range; reason.clear(); return true;
	}
	inline bool ApplyRendererEfficiencyFixtureSelection(RenderItemRecord& command,
		const RendererEfficiencyFixtureShadowSelection& selection, const RendererEfficiencyFixtureTargetProof& proof)
	{
		if (command.objectId != selection.objectId || command.meshId != selection.meshId
			|| command.objectLifetimeGeneration != selection.lifetimeGeneration || command.geometryRoute != GeometryRoute::DirectMeshlet) return false;
		RendererEfficiencyFixtureDetailRange range; std::string reason;
		if (!ResolveRendererEfficiencyFixtureSelection(selection, proof, range, reason)) return false;
		command.selectedVirtualLod = range.lodIndex; command.virtualMeshletOffset = range.meshletOffset;
		command.virtualMeshletCount = range.meshletCount; return true;
	}
	inline bool RebaseRendererEfficiencyFixtureView(const TerraEngine::Scene::WorldPosition& camera,
		const TerraEngine::Scene::WorldDelta& delta, RenderReferenceFrame& frame, glm::mat4& view, glm::vec3& localCamera)
	{
		const auto changed = TerraEngine::Scene::ApplyWorldDelta(frame.canonicalOrigin, delta);
		if (!changed.Succeeded()) return false;
		auto newFrame = frame; newFrame.canonicalOrigin = changed.position;
		const auto localized = TerraEngine::Scene::LocalizeWorldPosition(camera, newFrame.canonicalOrigin,
			TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
		if (!localized.Succeeded()) return false;
		const glm::vec3 newCamera(localized.offsetMetres);
		// Coordinate re-expression moves both geometry and camera by the same
		// amount. The canonical camera/object poses are never changed here.
		const glm::mat4 newView = view * glm::translate(glm::mat4(1), localCamera - newCamera);
		frame = newFrame; view = newView; localCamera = newCamera; return true;
	}
}
