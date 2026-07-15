// SceneViewOverlayTypes.cpp - Scene View overlay state helpers.

#include "SceneViewOverlayTypes.h"

#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace TerraEngine::Editor::SceneViewOverlay
{
	namespace
	{
		std::mutex g_DiagnosticsMutex;
		SceneViewDiagnosticsSnapshot g_LatestDiagnosticsSnapshot;

		std::uint32_t ReadUnsignedPreference(const nlohmann::json& json, const char* key, std::uint32_t fallback)
		{
			const auto iterator = json.find(key);
			if (iterator == json.end())
			{
				return fallback;
			}
			if (iterator->is_number_unsigned())
			{
				const std::uint64_t value = iterator->get<std::uint64_t>();
				return value <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(value) : fallback;
			}
			if (iterator->is_number_integer())
			{
				const std::int64_t value = iterator->get<std::int64_t>();
				return value >= 0 && static_cast<std::uint64_t>(value) <= std::numeric_limits<std::uint32_t>::max()
					? static_cast<std::uint32_t>(value)
					: fallback;
			}
			return fallback;
		}

		float ReadFloatPreference(const nlohmann::json& json, const char* key, float fallback)
		{
			const auto iterator = json.find(key);
			return iterator != json.end() && iterator->is_number()
				? iterator->get<float>()
				: fallback;
		}

		std::string ReadStringPreference(const nlohmann::json& json, const char* key, std::string fallback)
		{
			const auto iterator = json.find(key);
			return iterator != json.end() && iterator->is_string()
				? iterator->get<std::string>()
				: std::move(fallback);
		}
	}

	const char* ToString(SceneViewTool tool)
	{
		switch (tool)
		{
		case SceneViewTool::Hand: return "hand";
		case SceneViewTool::Move: return "move";
		case SceneViewTool::Rotate: return "rotate";
		case SceneViewTool::Scale: return "scale";
		case SceneViewTool::None: return "none";
		default: return "unknown";
		}
	}

	const char* ToString(PivotMode mode)
	{
		switch (mode)
		{
		case PivotMode::Center: return "center";
		case PivotMode::Pivot: return "pivot";
		default: return "unknown";
		}
	}

	const char* ToString(OrientationMode mode)
	{
		switch (mode)
		{
		case OrientationMode::Global: return "global";
		case OrientationMode::Local: return "local";
		default: return "unknown";
		}
	}

	const char* ToString(ProjectionMode mode)
	{
		switch (mode)
		{
		case ProjectionMode::Perspective: return "perspective";
		case ProjectionMode::Orthographic: return "orthographic";
		default: return "unknown";
		}
	}

	const char* ToString(MarkerCategory category)
	{
		switch (category)
		{
		case MarkerCategory::Camera: return "camera";
		case MarkerCategory::DirectionalLight: return "directionalLight";
		case MarkerCategory::PointLight: return "pointLight";
		case MarkerCategory::SpotLight: return "spotLight";
		case MarkerCategory::Physics: return "physics";
		case MarkerCategory::EmptyObject: return "emptyObject";
		case MarkerCategory::SelectedObject: return "selectedObject";
		case MarkerCategory::Unsupported: return "unsupported";
		default: return "unknown";
		}
	}

	const char* ToString(LargeSceneMode mode)
	{
		switch (mode)
		{
		case LargeSceneMode::Automatic: return "auto";
		case LargeSceneMode::On: return "on";
		case LargeSceneMode::Off: return "off";
		default: return "unknown";
		}
	}

	const char* ToString(PhysicsMarkerMode mode)
	{
		switch (mode)
		{
		case PhysicsMarkerMode::SelectedOnly: return "selected_only";
		case PhysicsMarkerMode::Clustered: return "clustered";
		case PhysicsMarkerMode::All: return "all";
		default: return "unknown";
		}
	}

	const char* ToString(DensePrimitiveTier tier)
	{
		switch (tier)
		{
		case DensePrimitiveTier::Full: return "full";
		case DensePrimitiveTier::Simple: return "simple";
		case DensePrimitiveTier::Point: return "point";
		default: return "unknown";
		}
	}

	const char* ToString(InteractionRegion region)
	{
		switch (region)
		{
		case InteractionRegion::Toolbar: return "toolbar";
		case InteractionRegion::OrientationWidget: return "orientationWidget";
		case InteractionRegion::Marker: return "marker";
		case InteractionRegion::Gizmo: return "gizmo";
		case InteractionRegion::FlyCamera: return "flyCamera";
		case InteractionRegion::TransformDrag: return "transformDrag";
		case InteractionRegion::Picking: return "picking";
		case InteractionRegion::Viewport: return "viewport";
		case InteractionRegion::None: return "none";
		default: return "unknown";
		}
	}

	const char* ToString(TransformEditStatus status)
	{
		switch (status)
		{
		case TransformEditStatus::Previewing: return "previewing";
		case TransformEditStatus::Committed: return "committed";
		case TransformEditStatus::Cancelled: return "cancelled";
		case TransformEditStatus::Rejected: return "rejected";
		case TransformEditStatus::RolledBack: return "rolledBack";
		default: return "unknown";
		}
	}

	bool IsPositiveFinite(float value)
	{
		return std::isfinite(value) && value > 0.0f;
	}

	float SanitizePositive(float value, float fallback)
	{
		return IsPositiveFinite(value) ? value : fallback;
	}

	bool ValidatePreference(const SceneOverlayPreference& preference, std::string* outError)
	{
		const auto reject = [outError](const char* message) {
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		};

		if (preference.largeSceneMode != LargeSceneMode::Automatic
			&& preference.largeSceneMode != LargeSceneMode::On
			&& preference.largeSceneMode != LargeSceneMode::Off)
		{
			return reject("Large-scene mode must be auto, on, or off.");
		}
		if (preference.largeSceneObjectThreshold < 1u || preference.largeSceneObjectThreshold > 10000000u)
		{
			return reject("Large-scene object threshold must be from 1 through 10000000.");
		}
		if (preference.visibleMarkerBudget < 64u || preference.visibleMarkerBudget > 100000u)
		{
			return reject("Visible marker budget must be from 64 through 100000.");
		}
		if (preference.markerPresentationBatchSize < 32u || preference.markerPresentationBatchSize > 8192u)
		{
			return reject("Marker presentation batch size must be from 32 through 8192.");
		}
		if (!std::isfinite(preference.densityRadiusPixels)
			|| preference.densityRadiusPixels < 4.0f || preference.densityRadiusPixels > 128.0f)
		{
			return reject("Marker density radius must be from 4 through 128 pixels.");
		}
		if (preference.densityThreshold < 2u || preference.densityThreshold > 256u)
		{
			return reject("Marker density threshold must be from 2 through 256.");
		}
		if (!std::isfinite(preference.maximumMarkerDistance)
			|| preference.maximumMarkerDistance < 0.0f || preference.maximumMarkerDistance > 1000000000.0f)
		{
			return reject("Maximum marker distance must be from 0 through 1000000000.");
		}
		if (!std::isfinite(preference.minimumProjectedSizePixels)
			|| preference.minimumProjectedSizePixels < 0.0f || preference.minimumProjectedSizePixels > 64.0f)
		{
			return reject("Minimum projected marker size must be from 0 through 64 pixels.");
		}
		if (preference.physicsMarkerMode != PhysicsMarkerMode::SelectedOnly
			&& preference.physicsMarkerMode != PhysicsMarkerMode::Clustered
			&& preference.physicsMarkerMode != PhysicsMarkerMode::All)
		{
			return reject("Physics marker mode must be selected_only, clustered, or all.");
		}
		if (preference.densePrimitiveTier != DensePrimitiveTier::Full
			&& preference.densePrimitiveTier != DensePrimitiveTier::Simple
			&& preference.densePrimitiveTier != DensePrimitiveTier::Point)
		{
			return reject("Dense primitive tier must be full, simple, or point.");
		}
		if (!IsPositiveFinite(preference.translationSnap)
			|| !IsPositiveFinite(preference.rotationSnapDegrees)
			|| !IsPositiveFinite(preference.scaleSnap))
		{
			return reject("Scene View snap values must be positive finite numbers.");
		}
		if (preference.toolbarCompactMode != "auto"
			&& preference.toolbarCompactMode != "compact"
			&& preference.toolbarCompactMode != "expanded")
		{
			return reject("Toolbar compact mode must be auto, compact, or expanded.");
		}
		if (outError != nullptr)
		{
			outError->clear();
		}
		return true;
	}

	SceneOverlayPreference SanitizePreference(SceneOverlayPreference preference)
	{
		preference.translationSnap = SanitizePositive(preference.translationSnap, 0.5f);
		preference.rotationSnapDegrees = SanitizePositive(preference.rotationSnapDegrees, 15.0f);
		preference.scaleSnap = SanitizePositive(preference.scaleSnap, 0.1f);
		if (preference.toolbarCompactMode != "auto" && preference.toolbarCompactMode != "compact" && preference.toolbarCompactMode != "expanded")
		{
			preference.toolbarCompactMode = "auto";
		}

		// Preference loading is deliberately tolerant: one malformed field falls
		// back to its documented default instead of discarding all unrelated user
		// settings. Explicit updates use ValidatePreference and reject atomically.
		if (preference.largeSceneMode != LargeSceneMode::Automatic
			&& preference.largeSceneMode != LargeSceneMode::On
			&& preference.largeSceneMode != LargeSceneMode::Off)
		{
			preference.largeSceneMode = LargeSceneMode::Automatic;
		}
		if (preference.largeSceneObjectThreshold < 1u || preference.largeSceneObjectThreshold > 10000000u)
		{
			preference.largeSceneObjectThreshold = SceneOverlayPreference::kDefaultLargeSceneObjectThreshold;
		}
		if (preference.visibleMarkerBudget < 64u || preference.visibleMarkerBudget > 100000u)
		{
			preference.visibleMarkerBudget = SceneOverlayPreference::kDefaultVisibleMarkerBudget;
		}
		if (preference.markerPresentationBatchSize < 32u || preference.markerPresentationBatchSize > 8192u)
		{
			preference.markerPresentationBatchSize = SceneOverlayPreference::kDefaultMarkerPresentationBatchSize;
		}
		if (!std::isfinite(preference.densityRadiusPixels)
			|| preference.densityRadiusPixels < 4.0f || preference.densityRadiusPixels > 128.0f)
		{
			preference.densityRadiusPixels = SceneOverlayPreference::kDefaultDensityRadiusPixels;
		}
		if (preference.densityThreshold < 2u || preference.densityThreshold > 256u)
		{
			preference.densityThreshold = SceneOverlayPreference::kDefaultDensityThreshold;
		}
		if (!std::isfinite(preference.maximumMarkerDistance)
			|| preference.maximumMarkerDistance < 0.0f || preference.maximumMarkerDistance > 1000000000.0f)
		{
			preference.maximumMarkerDistance = SceneOverlayPreference::kDefaultMaximumMarkerDistance;
		}
		if (!std::isfinite(preference.minimumProjectedSizePixels)
			|| preference.minimumProjectedSizePixels < 0.0f || preference.minimumProjectedSizePixels > 64.0f)
		{
			preference.minimumProjectedSizePixels = SceneOverlayPreference::kDefaultMinimumProjectedSizePixels;
		}
		if (preference.physicsMarkerMode != PhysicsMarkerMode::SelectedOnly
			&& preference.physicsMarkerMode != PhysicsMarkerMode::Clustered
			&& preference.physicsMarkerMode != PhysicsMarkerMode::All)
		{
			preference.physicsMarkerMode = PhysicsMarkerMode::Clustered;
		}
		if (preference.densePrimitiveTier != DensePrimitiveTier::Full
			&& preference.densePrimitiveTier != DensePrimitiveTier::Simple
			&& preference.densePrimitiveTier != DensePrimitiveTier::Point)
		{
			preference.densePrimitiveTier = DensePrimitiveTier::Simple;
		}
		return preference;
	}

	SceneViewLargeSceneState ResolveLargeSceneState(const SceneOverlayPreference& preference, std::uint32_t objectCount)
	{
		const SceneOverlayPreference sanitized = SanitizePreference(preference);
		SceneViewLargeSceneState state;
		state.configuredMode = sanitized.largeSceneMode;
		state.objectCount = objectCount;
		state.activationThreshold = sanitized.largeSceneObjectThreshold;
		state.effective = sanitized.largeSceneMode == LargeSceneMode::On
			|| (sanitized.largeSceneMode == LargeSceneMode::Automatic && objectCount >= sanitized.largeSceneObjectThreshold);
		return state;
	}

	SceneViewToolState BuildToolStateFromPreference(const SceneOverlayPreference& preference, SceneViewTool activeTool, PivotMode pivotMode, OrientationMode orientationMode)
	{
		const SceneOverlayPreference sanitized = SanitizePreference(preference);
		SceneViewToolState state;
		state.activeTool = activeTool;
		state.pivotMode = pivotMode;
		state.orientationMode = orientationMode;
		state.snapEnabled = sanitized.snapEnabled;
		state.snapValues = { sanitized.translationSnap, sanitized.rotationSnapDegrees, sanitized.scaleSnap };
		state.gridVisible = sanitized.gridVisible;
		state.gizmosVisible = sanitized.gizmosVisible;
		state.iconsVisible = sanitized.iconsVisible;
		state.projectionMode = sanitized.projectionMode;
		state.lastChangedBy = LastChangedBy::RestoredPreference;
		return state;
	}

	SceneViewSchedulerDeclaration BuildSceneViewOverlaySchedulerDeclaration()
	{
		SceneViewSchedulerDeclaration declaration;
		declaration.readResources = {
			"EditorSelection",
			"EditorPreferences.SceneView",
			"TransformSnapshot",
			"CameraComponentSnapshot",
			"LightComponentSnapshot",
			"ColliderSetComponentSnapshot",
			"MeshBoundsSnapshot",
			"RendererRecoveryStateSnapshot",
			"PhysicsDebugPacketHistory",
			"PhysicsDiagnosticsSnapshot"
		};
		declaration.writeResources = {
			"EditorPreferences.SceneView",
			"SceneViewDiagnosticsSnapshot",
			"UndoRedoTransformCommand",
			"PhysicsShapeAuthoringTransaction",
			"SceneMutationQueue"
		};
		declaration.workerSafePreparation = "Prepare immutable marker, toolbar, orientation, physics debug packet, collider-record, and mesh-bound selections from editor-owned read state.";
		declaration.protectedBoundary = "Draw ImGui overlays, own pointer input, keep collider drags local, and enqueue one observed typed physics authoring transaction only when Apply is pressed on the editor UI boundary.";
		declaration.visibilityBoundary = "Display-only state is visible immediately in Scene View; durable transform and collider mutations become visible after the declared SceneMutationQueue transactional flush.";
		return declaration;
	}

	PhysicsShapeAuthoringCommit FinalizePhysicsShapeEdit(const PhysicsShapeEditPreview& preview)
	{
		PhysicsShapeAuthoringCommit commit;
		commit.preview = preview;
		// A zero UUID or non-positive extent cannot become a transaction because it
		// would make a rejected edit look partially applied in the Scene View.
		commit.valid = preview.active
			&& preview.objectId != TerraEngine::EMPTY_UUID
			&& preview.colliderRecordId != TerraEngine::EMPTY_UUID
			&& IsPositiveFinite(preview.localExtents.x)
			&& IsPositiveFinite(preview.localExtents.y)
			&& IsPositiveFinite(preview.localExtents.z);
		return commit;
	}

	std::size_t CountPhysicsDebugPrimitives(const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>>& packets, const TerraEngine::Physics::PhysicsDebugDisplaySelection& selection)
	{
		std::size_t count = 0U;
		for (const std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>& packet : packets)
		{
			if (!packet)
			{
				continue;
			}
			if (selection.showLines && packet->lines) { count += packet->lines->size(); }
			if (selection.showTriangles && packet->triangles) { count += packet->triangles->size(); }
			if (selection.showContacts && packet->contacts) { count += packet->contacts->size(); }
			if (selection.showConstraints && packet->constraints) { count += packet->constraints->size(); }
			if (selection.showCharacterSupport && packet->characters) { count += packet->characters->size(); }
			if (selection.showRecentQueries && packet->queries) { count += packet->queries->size(); }
		}
		return count;
	}

	void to_json(nlohmann::json& json, const SceneOverlayPreference& preference)
	{
		const SceneOverlayPreference sanitized = SanitizePreference(preference);
		json = nlohmann::json{
			{"iconsVisible", sanitized.iconsVisible},
			{"cameraIconsVisible", sanitized.cameraIconsVisible},
			{"lightIconsVisible", sanitized.lightIconsVisible},
			{"emptyObjectIconsVisible", sanitized.emptyObjectIconsVisible},
			{"gridVisible", sanitized.gridVisible},
			{"gizmosVisible", sanitized.gizmosVisible},
			{"diagnosticsVisible", sanitized.diagnosticsVisible},
			{"snapEnabled", sanitized.snapEnabled},
			{"translationSnap", sanitized.translationSnap},
			{"rotationSnapDegrees", sanitized.rotationSnapDegrees},
			{"scaleSnap", sanitized.scaleSnap},
			{"projectionMode", ToString(sanitized.projectionMode)},
			{"orientationWidgetVisible", sanitized.orientationWidgetVisible},
			{"toolbarCompactMode", sanitized.toolbarCompactMode},
			{"largeSceneMode", ToString(sanitized.largeSceneMode)},
			{"largeSceneObjectThreshold", sanitized.largeSceneObjectThreshold},
			{"visibleMarkerBudget", sanitized.visibleMarkerBudget},
			{"markerPresentationBatchSize", sanitized.markerPresentationBatchSize},
			{"densityRadiusPixels", sanitized.densityRadiusPixels},
			{"densityThreshold", sanitized.densityThreshold},
			{"maximumMarkerDistance", sanitized.maximumMarkerDistance},
			{"minimumProjectedSizePixels", sanitized.minimumProjectedSizePixels},
			{"physicsMarkerMode", ToString(sanitized.physicsMarkerMode)},
			{"densePrimitiveTier", ToString(sanitized.densePrimitiveTier)}
		};
	}

	void from_json(const nlohmann::json& json, SceneOverlayPreference& preference)
	{
		SceneOverlayPreference restored;
		restored.iconsVisible = json.value("iconsVisible", restored.iconsVisible);
		restored.cameraIconsVisible = json.value("cameraIconsVisible", restored.cameraIconsVisible);
		restored.lightIconsVisible = json.value("lightIconsVisible", restored.lightIconsVisible);
		restored.emptyObjectIconsVisible = json.value("emptyObjectIconsVisible", restored.emptyObjectIconsVisible);
		restored.gridVisible = json.value("gridVisible", restored.gridVisible);
		restored.gizmosVisible = json.value("gizmosVisible", restored.gizmosVisible);
		restored.diagnosticsVisible = json.value("diagnosticsVisible", restored.diagnosticsVisible);
		restored.snapEnabled = json.value("snapEnabled", restored.snapEnabled);
		restored.translationSnap = json.value("translationSnap", restored.translationSnap);
		restored.rotationSnapDegrees = json.value("rotationSnapDegrees", restored.rotationSnapDegrees);
		restored.scaleSnap = json.value("scaleSnap", restored.scaleSnap);
		restored.orientationWidgetVisible = json.value("orientationWidgetVisible", restored.orientationWidgetVisible);
		restored.toolbarCompactMode = json.value("toolbarCompactMode", restored.toolbarCompactMode);
		restored.largeSceneObjectThreshold = ReadUnsignedPreference(json, "largeSceneObjectThreshold", restored.largeSceneObjectThreshold);
		restored.visibleMarkerBudget = ReadUnsignedPreference(json, "visibleMarkerBudget", restored.visibleMarkerBudget);
		restored.markerPresentationBatchSize = ReadUnsignedPreference(json, "markerPresentationBatchSize", restored.markerPresentationBatchSize);
		restored.densityRadiusPixels = ReadFloatPreference(json, "densityRadiusPixels", restored.densityRadiusPixels);
		restored.densityThreshold = ReadUnsignedPreference(json, "densityThreshold", restored.densityThreshold);
		restored.maximumMarkerDistance = ReadFloatPreference(json, "maximumMarkerDistance", restored.maximumMarkerDistance);
		restored.minimumProjectedSizePixels = ReadFloatPreference(json, "minimumProjectedSizePixels", restored.minimumProjectedSizePixels);

		const std::string projectionMode = json.value("projectionMode", std::string(ToString(restored.projectionMode)));
		restored.projectionMode = projectionMode == "orthographic" ? ProjectionMode::Orthographic : ProjectionMode::Perspective;
		const std::string largeSceneMode = ReadStringPreference(json, "largeSceneMode", ToString(restored.largeSceneMode));
		if (largeSceneMode == "on")
		{
			restored.largeSceneMode = LargeSceneMode::On;
		}
		else if (largeSceneMode == "off")
		{
			restored.largeSceneMode = LargeSceneMode::Off;
		}
		else
		{
			restored.largeSceneMode = LargeSceneMode::Automatic;
		}
		const std::string physicsMarkerMode = ReadStringPreference(json, "physicsMarkerMode", ToString(restored.physicsMarkerMode));
		if (physicsMarkerMode == "selected_only")
		{
			restored.physicsMarkerMode = PhysicsMarkerMode::SelectedOnly;
		}
		else if (physicsMarkerMode == "all")
		{
			restored.physicsMarkerMode = PhysicsMarkerMode::All;
		}
		else
		{
			restored.physicsMarkerMode = PhysicsMarkerMode::Clustered;
		}
		const std::string densePrimitiveTier = ReadStringPreference(json, "densePrimitiveTier", ToString(restored.densePrimitiveTier));
		if (densePrimitiveTier == "full")
		{
			restored.densePrimitiveTier = DensePrimitiveTier::Full;
		}
		else if (densePrimitiveTier == "point")
		{
			restored.densePrimitiveTier = DensePrimitiveTier::Point;
		}
		else
		{
			restored.densePrimitiveTier = DensePrimitiveTier::Simple;
		}
		preference = SanitizePreference(restored);
	}

	nlohmann::json SerializeDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot)
	{
		nlohmann::json stageTimings = nlohmann::json::array();
		for (const TerraEngine::Profiling::PerformanceStageRecord& stage : snapshot.stageTimings)
		{
			stageTimings.push_back({
				{"stage", stage.stageName},
				{"durationNanoseconds", stage.duration.availability == TerraEngine::Profiling::TelemetryAvailability::Available
					? nlohmann::json(stage.duration.nanoseconds)
					: nlohmann::json(nullptr)},
				{"unavailableReason", stage.duration.unavailableReason},
				{"workItems", stage.workItemCount},
				{"sampled", stage.sampled}
			});
		}
		return {
			{"surface", "SceneView"},
			{"rendererAvailable", snapshot.rendererAvailable},
			{"activeTool", ToString(snapshot.toolState.activeTool)},
			{"pivotMode", ToString(snapshot.toolState.pivotMode)},
			{"orientationMode", ToString(snapshot.toolState.orientationMode)},
			{"snapEnabled", snapshot.toolState.snapEnabled},
			{"snapValues", { snapshot.toolState.snapValues[0], snapshot.toolState.snapValues[1], snapshot.toolState.snapValues[2] }},
			{"gridVisible", snapshot.toolState.gridVisible},
			{"gizmosVisible", snapshot.toolState.gizmosVisible},
			{"iconsVisible", snapshot.toolState.iconsVisible},
			{"projectionMode", ToString(snapshot.projectionMode)},
			{"largeScene", {
				{"configuredMode", ToString(snapshot.largeSceneState.configuredMode)},
				{"effective", snapshot.largeSceneState.effective},
				{"objectCount", snapshot.largeSceneState.objectCount},
				{"activationThreshold", snapshot.largeSceneState.activationThreshold}
				,{"reason", snapshot.largeSceneState.effective ? "object_threshold" : "below_threshold"}
				,{"reductions", snapshot.activeReductions}
			}},
			{"markers", {
				{"source", snapshot.sourceMarkerCount},
				{"spatialRecords", snapshot.spatialRecordCount},
				{"spatialQueryRejected", snapshot.spatialQueryRejectedCount},
				{"spatialQueryFallback", snapshot.spatialQueryFallbackCount},
				{"candidates", snapshot.candidateMarkerCount},
				{"viewRejected", snapshot.viewRejectedMarkerCount},
				{"densityConsolidated", snapshot.densityConsolidatedMarkerCount},
				{"budgetRejected", snapshot.budgetRejectedMarkerCount},
				{"presented", snapshot.presentedMarkerCount},
				{"presentationBatches", snapshot.presentationBatchCount},
				{"selectedExemptions", snapshot.selectedExemptionCount},
				{"cameraExemptions", snapshot.cameraExemptionCount},
				{"lightExemptions", snapshot.lightExemptionCount}
			}},
			{"stageTimings", std::move(stageTimings)},
			{"orientationWidgetVisible", snapshot.orientationWidget.visible},
			{"orientationHoveredAxis", snapshot.orientationWidget.hoveredAxis},
			{"visibleMarkerCountsByCategory", {
				{"camera", snapshot.cameraMarkerCount},
				{"directionalLight", snapshot.directionalLightMarkerCount},
				{"pointLight", snapshot.pointLightMarkerCount},
				{"spotLight", snapshot.spotLightMarkerCount},
				{"emptyObject", snapshot.emptyObjectMarkerCount},
				{"selectedObject", snapshot.selectedObjectMarkerCount}
			}},
			{"hoveredMarkerId", snapshot.hoveredMarkerId},
			{"selectedMarkerId", snapshot.selectedMarkerId},
			{"lastInteractionOwner", ToString(snapshot.lastInteractionOwner)},
			{"lastRejectedMutationReason", snapshot.lastRejectedMutationReason},
			{"disabledOverlayReason", snapshot.disabledOverlayReason},
			{"physicsDebugPacketCount", snapshot.physicsDebugPacketCount},
			{"physicsDebugPrimitiveCount", snapshot.physicsDebugPrimitiveCount},
			{"physicsDebugVisible", snapshot.physicsDebugVisible},
			{"gameViewAffected", snapshot.gameViewAffected}
		};
	}

	void PublishLatestDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
		g_LatestDiagnosticsSnapshot = snapshot;
	}

	SceneViewDiagnosticsSnapshot GetLatestDiagnosticsSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
		return g_LatestDiagnosticsSnapshot;
	}
}
