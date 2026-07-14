// SceneViewOverlayTypes.h - Scene View overlay state contracts.
//
// These small value types keep editor-only Scene View state separate from
// authored scene data and Game View rendering. They are intentionally plain so
// tests, diagnostics, and UI code can reason about the same toolbar, marker,
// preference, and interaction contracts without depending on Vulkan or ImGui.

#pragma once

#include "Engine/Utilities/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace TerraEngine::Editor::SceneViewOverlay
{
	enum class SceneViewTool
	{
		Hand,
		Move,
		Rotate,
		Scale,
		None
	};

	enum class PivotMode
	{
		Center,
		Pivot
	};

	enum class OrientationMode
	{
		Global,
		Local
	};

	enum class ProjectionMode
	{
		Perspective,
		Orthographic
	};

	enum class LastChangedBy
	{
		User,
		RestoredPreference,
		AutomationDiagnosticReplay,
		DefaultValue
	};

	enum class IconCategory
	{
		Toolbar,
		Camera,
		DirectionalLight,
		PointLight,
		SpotLight,
		EmptyObject,
		Selection,
		Orientation,
		Diagnostics,
		Grid
	};

	enum class MarkerCategory
	{
		Camera,
		DirectionalLight,
		PointLight,
		SpotLight,
		EmptyObject,
		SelectedObject,
		Unsupported
	};

	enum class InteractionRegion
	{
		Toolbar,
		OrientationWidget,
		Marker,
		Gizmo,
		FlyCamera,
		TransformDrag,
		Picking,
		Viewport,
		None
	};

	enum class TransformOperation
	{
		Move,
		Rotate,
		Scale
	};

	enum class TransformEditStatus
	{
		Previewing,
		Committed,
		Cancelled,
		Rejected,
		RolledBack
	};

	struct SceneViewToolState
	{
		SceneViewTool activeTool = SceneViewTool::Move;
		PivotMode pivotMode = PivotMode::Center;
		OrientationMode orientationMode = OrientationMode::Local;
		bool snapEnabled = false;
		std::array<float, 3> snapValues = { 0.5f, 15.0f, 0.1f };
		bool gridVisible = true;
		bool gizmosVisible = true;
		bool iconsVisible = true;
		ProjectionMode projectionMode = ProjectionMode::Perspective;
		LastChangedBy lastChangedBy = LastChangedBy::DefaultValue;
	};

	struct SceneOverlayPreference
	{
		bool iconsVisible = true;
		bool cameraIconsVisible = true;
		bool lightIconsVisible = true;
		bool emptyObjectIconsVisible = true;
		bool gridVisible = true;
		bool gizmosVisible = true;
		bool diagnosticsVisible = false;
		bool snapEnabled = false;
		float translationSnap = 0.5f;
		float rotationSnapDegrees = 15.0f;
		float scaleSnap = 0.1f;
		ProjectionMode projectionMode = ProjectionMode::Perspective;
		bool orientationWidgetVisible = true;
		std::string toolbarCompactMode = "auto";
	};

	struct OverlayIconDefinition
	{
		std::string iconId;
		std::string displayName;
		IconCategory category = IconCategory::Toolbar;
		std::string shapeKind;
		bool defaultVisible = true;
		float minimumHitSize = 24.0f;
		std::uint32_t visualPriority = 0u;
		std::string tooltip;
		std::string accessibilityName;
		std::string contrastStyle = "outlined";
	};

	struct SceneObjectMarker
	{
		std::string markerId;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		entt::entity entity = entt::null;
		std::string displayName;
		MarkerCategory category = MarkerCategory::Unsupported;
		glm::vec3 worldPosition = glm::vec3(0.0f);
		glm::vec2 screenPosition = glm::vec2(0.0f);
		float depth = 0.0f;
		bool visible = false;
		bool selected = false;
		bool hovered = false;
		bool occlusionHint = false;
		std::uint32_t visualPriority = 0u;
		std::uint32_t overlapGroup = 0u;
		std::string tooltip;
	};

	struct OrientationWidgetState
	{
		glm::vec3 cameraForward = glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 cameraRight = glm::vec3(1.0f, 0.0f, 0.0f);
		ProjectionMode projectionMode = ProjectionMode::Perspective;
		std::string hoveredAxis = "none";
		std::string activeAxis = "none";
		std::vector<std::string> availableAxisTargets;
		glm::vec4 screenBounds = glm::vec4(0.0f);
		std::string label = "Persp";
		bool visible = true;
	};

	struct ViewportInteractionContext
	{
		glm::vec2 pointerPosition = glm::vec2(0.0f);
		InteractionRegion hoveredRegion = InteractionRegion::None;
		InteractionRegion activeRegion = InteractionRegion::None;
		std::vector<std::string> consumedButtons;
		std::string activeObjectId;
		std::vector<std::string> candidateMarkers;
		bool gizmoActive = false;
		bool flyCameraActive = false;
		bool selectionChangePending = false;
		bool transformCommitPending = false;
	};

	struct SceneViewDiagnosticsSnapshot
	{
		SceneViewToolState toolState;
		SceneOverlayPreference overlayPreferences;
		std::uint32_t cameraMarkerCount = 0u;
		std::uint32_t directionalLightMarkerCount = 0u;
		std::uint32_t pointLightMarkerCount = 0u;
		std::uint32_t spotLightMarkerCount = 0u;
		std::uint32_t emptyObjectMarkerCount = 0u;
		std::uint32_t selectedObjectMarkerCount = 0u;
		std::string hoveredMarkerId;
		std::string selectedMarkerId;
		OrientationWidgetState orientationWidget;
		ProjectionMode projectionMode = ProjectionMode::Perspective;
		bool gameViewAffected = false;
		InteractionRegion lastInteractionOwner = InteractionRegion::None;
		std::string lastRejectedMutationReason;
		bool rendererAvailable = true;
		std::string disabledOverlayReason;
		std::size_t physicsDebugPacketCount = 0U;
		std::size_t physicsDebugPrimitiveCount = 0U;
		bool physicsDebugVisible = false;
	};

	struct TransformEditCommit
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TransformOperation operation = TransformOperation::Move;
		glm::vec3 oldPosition = glm::vec3(0.0f);
		glm::quat oldRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 oldScale = glm::vec3(1.0f);
		glm::vec3 newPosition = glm::vec3(0.0f);
		glm::quat newRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 newScale = glm::vec3(1.0f);
		bool snapApplied = false;
		std::uint64_t startedAtFrame = 0u;
		std::uint64_t committedAtFrame = 0u;
		std::string source = "SceneView";
		TransformEditStatus status = TransformEditStatus::Previewing;
		std::string rejectionReason;
	};

	// Physics handle edits stay local to the Scene View while dragging. The
	// caller converts the finished value into one typed authoring transaction;
	// preview data must never mutate the live component or physics world.
	struct PhysicsShapeEditPreview
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID colliderRecordId = TerraEngine::EMPTY_UUID;
		glm::vec3 localCenter = glm::vec3(0.0f);
		glm::vec3 localExtents = glm::vec3(0.5f);
		bool fitToRenderMeshRequested = false;
		bool active = false;
	};

	struct PhysicsShapeAuthoringCommit
	{
		PhysicsShapeEditPreview preview;
		std::string transactionLabel = "Scene View physics shape edit";
		bool valid = false;
	};

	struct SceneViewSchedulerDeclaration
	{
		std::string owningSystem = "SceneViewWindow";
		std::string schedulerPhase = "EditorUI";
		std::vector<std::string> readResources;
		std::vector<std::string> writeResources;
		std::string workerSafePreparation;
		std::string protectedBoundary;
		std::string visibilityBoundary;
	};

	[[nodiscard]] const char* ToString(SceneViewTool tool);
	[[nodiscard]] const char* ToString(PivotMode mode);
	[[nodiscard]] const char* ToString(OrientationMode mode);
	[[nodiscard]] const char* ToString(ProjectionMode mode);
	[[nodiscard]] const char* ToString(MarkerCategory category);
	[[nodiscard]] const char* ToString(InteractionRegion region);
	[[nodiscard]] const char* ToString(TransformEditStatus status);

	[[nodiscard]] bool IsPositiveFinite(float value);
	[[nodiscard]] float SanitizePositive(float value, float fallback);
	[[nodiscard]] SceneOverlayPreference SanitizePreference(SceneOverlayPreference preference);
	[[nodiscard]] SceneViewToolState BuildToolStateFromPreference(const SceneOverlayPreference& preference, SceneViewTool activeTool, PivotMode pivotMode, OrientationMode orientationMode);
	[[nodiscard]] SceneViewSchedulerDeclaration BuildSceneViewOverlaySchedulerDeclaration();
	[[nodiscard]] PhysicsShapeAuthoringCommit FinalizePhysicsShapeEdit(const PhysicsShapeEditPreview& preview);
	[[nodiscard]] std::size_t CountPhysicsDebugPrimitives(const std::vector<std::shared_ptr<const TerraEngine::Physics::PhysicsDebugPacket>>& packets, const TerraEngine::Physics::PhysicsDebugDisplaySelection& selection);
	[[nodiscard]] nlohmann::json SerializeDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot);

	void to_json(nlohmann::json& json, const SceneOverlayPreference& preference);
	void from_json(const nlohmann::json& json, SceneOverlayPreference& preference);

	void PublishLatestDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot);
	[[nodiscard]] SceneViewDiagnosticsSnapshot GetLatestDiagnosticsSnapshot();
}
