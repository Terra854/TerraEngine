// SceneViewWindow.h — Scene View Window Definition
//
// Defines the SceneViewWindow class, which provides the primary 3D viewport for scene editing and navigation.
// It manages the camera used for editor rendering and handles input for spatial manipulation of entities.

#pragma once
#include "../Framework/EditorWindow.h"
#include "SceneViewOrientationWidget.h"
#include "SceneViewCameraState.h"
#include "SceneViewMarkerPipeline.h"
#include "SceneViewOverlayIcons.h"
#include "SceneViewOverlayMarkers.h"
#include "../Animation/AnimationOverlays.h"
#include "../Animation/AnimationSceneHandles.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Scene/SpatialIndex.h"
#include "Engine/Scene/WorldCoordinate.h"
#include "Engine/Utilities/UUID.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class MainEditorSubsystem;

// class SceneViewWindow groups related state and operations so callers have one documented contract.
class SceneViewWindow : public EditorWindow {
public:
	SceneViewWindow(MainEditorSubsystem* owner);

	// Scene View automation uses semantic values instead of screen coordinates.
	// These methods run on the editor's protected update boundary, so they may
	// safely share the same state and validation rules as the visible toolbar.
	[[nodiscard]] nlohmann::json GetAutomationState() const;
	// Project workspace persistence captures the viewport camera independently of
	// scene objects. Restore validates the whole record before changing live state.
	[[nodiscard]] nlohmann::json SerializeWorkspaceState() const;
	bool RestoreWorkspaceState(const nlohmann::json& value, std::string& outError);
	// Automation submits exact canonical addresses. Screen-space vectors are
	// deliberately not accepted here because they cannot name distant worlds.
	bool SetAutomationCamera(
		const TerraEngine::Scene::WorldPosition& position,
		const TerraEngine::Scene::WorldPosition& focus,
		std::string& outError);
	bool FrameAutomationObject(entt::entity entity, std::string& outError);
	bool SetAutomationTool(std::string_view tool, std::string& outError);
	bool SetAutomationOrientation(std::string_view orientation, std::string& outError);
	bool SetAutomationSnapping(bool enabled, const std::optional<glm::vec3>& values, std::string& outError);
	bool SetAutomationVisualization(std::string_view mode, std::string& outError);
	// Terrain diagnostics supply an already normalized canonical focus. Scene View
	// owns camera placement and keeps the focus exact across distant world cells.
	void FrameTerrainDiagnostic(const TerraEngine::Scene::WorldPosition& focus);
	// Terrain authoring claims the pointer before ordinary object selection only
	// while a real brush stroke is active. Releasing it restores normal picking.
	void SetTerrainBrushCapture(bool active) noexcept { m_TerrainBrushCapture = active; }
	[[nodiscard]] bool HasTerrainBrushCapture() const noexcept { return m_TerrainBrushCapture; }

	// The ColliderSet Inspector owns the visible authoring controls. Scene View
	// owns only the detached 3D preview and its transactional Apply boundary so
	// viewport handles never mutate a live component while the user is dragging.
	void BeginPhysicsColliderPreview(entt::entity entity, const TerraEngine::UUID& recordId);
	void RequestPhysicsColliderFitToRenderMesh(entt::entity entity);
	void CommitPhysicsColliderPreview();
	void CancelPhysicsColliderPreview();
	[[nodiscard]] bool ReadPhysicsColliderPreview(
		const TerraEngine::UUID& objectId,
		const TerraEngine::UUID& recordId,
		glm::vec3& outCenter,
		glm::vec3& outHalfExtents) const;
	void UpdatePhysicsColliderPreview(
		const TerraEngine::UUID& objectId,
		const TerraEngine::UUID& recordId,
		const glm::vec3& center,
		const glm::vec3& halfExtents);
	[[nodiscard]] bool IsPhysicsColliderPreviewSelection(const TerraEngine::UUID& recordId) const;
	[[nodiscard]] const std::string& PhysicsColliderPreviewStatus() const { return m_PhysicsPreviewStatus; }

protected:
	virtual void OnGui() override;

private:
	void LoadOverlayPreferencesIfNeeded();
	void PersistOverlayPreferences();
	void DrawSceneToolbar();
	void BuildSceneObjectMarkers(const glm::mat4& viewProjection, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawSceneObjectMarkers();
	bool DrawOrientationWidget(glm::mat4& view, const glm::mat4& projection, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void PublishSceneViewDiagnostics(bool rendererAvailable, const std::string& disabledOverlayReason = std::string());
	glm::vec3 GetCameraForward() const;
	glm::vec3 GetCameraUp() const;
	TerraEngine::Scene::WorldPosition ResolveSceneViewFocus(float fallbackDistance) const;
	float ResolveSceneViewFocusDistance(const TerraEngine::Scene::WorldPosition& focus, float fallbackDistance) const;
	void RepositionCameraAroundFocus(const TerraEngine::Scene::WorldPosition& focus, float distance);
	[[nodiscard]] glm::vec3 LocalizeForCamera(const TerraEngine::Scene::WorldPosition& position) const;
	[[nodiscard]] bool TranslateCamera(const glm::dvec3& deltaMetres);
	void SetSceneViewProjectionMode(TerraEngine::Editor::SceneViewOverlay::ProjectionMode projectionMode);
	bool IsPointerInsideOrientationWidget(const ImVec2& viewportPos, const ImVec2& viewportSize) const;
	bool HandleSceneViewScrollZoom();
	bool HandleSceneViewPan(const ImVec2& viewportSize);
	void HandleCameraInput(float deltaTime, const ImVec2& viewportSize);
	void DrawGizmos(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawPhysicsColliderEditHandles(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawAnimationSceneHandles(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawPhysicsDebugOverlay(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawNavigationDebugOverlay(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void ApplyPrefabContextTintForNextRender();
	void CollectFocusedPrefabObjectIds(entt::entity entity, std::unordered_set<TerraEngine::UUID>& objectIds);

	MainEditorSubsystem* m_Owner;

	// Camera State
	// Canonical state survives arbitrary world traversal. Render matrices always
	// use a bounded frame localized around this position.
	TerraEngine::Scene::WorldPosition m_CameraPosition{{}, {0.0, 5.0, 10.0}};
	glm::vec3 m_CameraRot{ -0.35f, 0.0f, 0.0f }; // ~20 degrees down in radians
	float m_Fov = 60.0f;
	float m_CameraSpeed = 10.0f;
	float m_MouseSensitivity = 0.1f;
	bool m_IsFlyCamActive = false;
	bool m_IsSceneViewPanning = false;
	bool m_TerrainBrushCapture = false;
	std::pair<double, double> m_StoredCursorPos;
	// Scene View camera snaps orbit around this authoring focus so orthographic
	// axis clicks rotate the scene instead of spinning the camera in place.
	TerraEngine::Scene::WorldPosition m_CameraFocus{};
	float m_CameraFocusDistance = 18.0f;
	bool m_CameraFocusInitialized = false;
	// Orthographic navigation is intentionally independent from perspective
	// navigation. Preserve every value that defines the perspective composition
	// so switching back restores the exact position, look direction, and pivot.
	struct PerspectiveCameraState
	{
		TerraEngine::Scene::WorldPosition position{};
		glm::vec3 rotation{ 0.0f };
		TerraEngine::Scene::WorldPosition focus{};
		float focusDistance = 18.0f;
		float fieldOfView = 60.0f;
		bool focusInitialized = false;
		bool valid = false;
	};
	PerspectiveCameraState m_PerspectiveCameraState;

	// Gizmo State
	int m_CurrentGizmoOperation = -1; // -1 for none, or ImGuizmo::TRANSLATE etc.
	bool m_UsingSnap = false;
	float m_SnapValue[3] = { 0.5f, 0.5f, 0.5f };
	float m_RotationSnapValue[3] = { 15.0f, 15.0f, 15.0f };
	float m_ScaleSnapValue[3] = { 0.1f, 0.1f, 0.1f };

	bool m_WasUsingGizmo = false;
	TerraEngine::Scene::WorldPose m_GizmoStartPose{};
	// Scene View keeps its debug mode separate from Game View so artists can
	// inspect editor-only culling while gameplay preview stays in another mode.
	VirtualGeometryDebugVisualizationMode m_VirtualGeometryDebugMode = VirtualGeometryDebugVisualizationMode::None;
	bool m_VirtualGeometryDebugModeInitialized = false;
	// Keep the temporary virtual-geometry counter panel opt-in until Scene View
	// gets a fuller diagnostics toolbar that can present this data cleanly.
	bool m_ShowVirtualGeometryDiagnostics = false;
	bool m_OverlayPreferencesLoaded = false;
	bool m_OverlayPreferenceDirty = false;
	float m_OrthographicHeight = 10.0f;
	TerraEngine::Editor::SceneViewOverlay::SceneViewToolState m_ToolState;
	TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference m_OverlayPreference;
	TerraEngine::Editor::SceneViewOverlay::ViewportInteractionContext m_InteractionContext;
	TerraEngine::Editor::SceneViewOverlay::OrientationWidgetState m_OrientationWidgetState;
	TerraEngine::Editor::SceneViewOverlay::OrientationWidgetCache m_OrientationWidgetCache;
	std::vector<TerraEngine::Editor::SceneViewOverlay::OverlayIconDefinition> m_IconDefinitions;
	std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> m_ObjectMarkers;
	// Marker preparation is split from the legacy draw records. The pipeline and
	// spatial snapshot retain immutable data across frames; m_ObjectMarkers is
	// now only the bounded presentation batch consumed by ImGui.
	TerraEngine::Editor::SceneViewOverlay::MarkerPipeline m_MarkerPipeline;
	TerraEngine::Editor::SceneViewOverlay::MarkerFrameResult m_MarkerFrame;
	TerraEngine::Scene::EditorSelectableSpatialSnapshotPublisher m_MarkerSpatialPublisher;
	std::shared_ptr<const TerraEngine::Scene::EditorSelectableSpatialSnapshot> m_MarkerSpatialSnapshot;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialObjectRecord> m_MarkerSpatialRecords;
	std::uint64_t m_MarkerSceneGeneration = 0u;
	std::uint64_t m_MarkerSpatialPublicationGeneration = 0u;
	std::uint64_t m_MarkerViewGeneration = 0u;
	std::uint64_t m_MarkerPreferenceGeneration = 0u;
	TerraEngine::Scene::WorldPosition m_MarkerWorkingFrameOrigin{};
	std::uint32_t m_MarkerSpatialQueryRejectedCount = 0u;
	std::uint32_t m_MarkerSpatialQueryFallbackCount = 0u;
	std::size_t m_LastMarkerPreferenceHash = 0u;
	glm::mat4 m_LastMarkerViewProjection = glm::mat4(0.0f);
	glm::ivec4 m_LastMarkerViewport = glm::ivec4(0);
	// Marker source data changes with scene structure, transforms, metadata,
	// selection, or icon preferences. Projection still runs per frame so camera
	// movement and viewport resize remain immediately visible without rescanning.
	std::uint64_t m_MarkerSpatialRevision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t m_MarkerHierarchyRevision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t m_MarkerMetadataSignature = std::numeric_limits<std::uint64_t>::max();
	std::uint8_t m_MarkerPreferenceSignature = 0U;
	std::optional<entt::entity> m_MarkerSelection;
	TerraEngine::Physics::PhysicsDebugDisplaySelection m_PhysicsDebugSelection;
	std::size_t m_PhysicsDebugPacketCount = 0U;
	std::size_t m_PhysicsDebugPrimitiveCount = 0U;
	bool m_PhysicsDebugVisible = false;
	// Collider handles edit this copy only. It deliberately outlives mouse
	// movement but never aliases a live ColliderSetComponent record.
	std::optional<TerraEngine::Editor::SceneViewOverlay::PhysicsShapeEditPreview> m_PhysicsShapePreview;
	TerraEngine::UUID m_SelectedColliderRecordId = TerraEngine::EMPTY_UUID;
	// These values track only the last live authored collider state. Undo, redo,
	// or another Inspector transaction can therefore refresh the detached preview
	// without mistaking the user's uncommitted handle drag for an external edit.
	glm::vec3 m_PhysicsLastAuthoredCenter{0.0f};
	glm::quat m_PhysicsLastAuthoredRotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::vec3 m_PhysicsLastAuthoredExtents{0.5f};
	std::string m_PhysicsPreviewStatus;
	std::uint64_t m_PhysicsShapeCommitSequence = 0U;

	// Animation overlays are opt-in and collect only the selected animator's
	// immutable publication. Procedural edits remain detached until Apply submits
	// a generation-safe animation command at the normal protected boundary.
	TerraEngine::Editor::Animation::AnimationOverlays m_AnimationOverlays;
	TerraEngine::Editor::Animation::AnimationSceneHandles m_AnimationSceneHandles;
	std::uint32_t m_AnimationOverlayKinds =
		TerraEngine::Editor::Animation::kAllAnimationOverlayKinds;
	bool m_ShowAnimationOverlays = false;
	bool m_AnimationOverlayConfigured = false;
	std::uint64_t m_SelectedAnimationGoalId = 0U;
	std::uint64_t m_AnimationHandleCommandSequence = 0U;
	std::string m_AnimationHandleStatus;

	void HandlePicking(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
};
