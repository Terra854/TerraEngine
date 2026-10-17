// GameViewWindow.h — Editor game view window interface
//
// Defines the editor window responsible for presenting the live game output. it provides controls for aspect ratio selection, display scaling, and toggling runtime statistics or gizmos within the editor workspace.

#pragma once
#include "../Framework/EditorWindow.h"
#include "Engine/Renderer/RenderTypes.h"
#include <cstdint>
#include <limits>
#include <vector>
#include <string>
#include <imgui.h>

class MainEditorSubsystem;

namespace TerraEngine::Editor
{
	// GameViewUpdatePath makes the per-frame activation decision observable to
	// tests and diagnostics without exposing ImGui or renderer-owned resources.
	enum class GameViewUpdatePath
	{
		Active,
		InactiveWindow,
		ContentFree,
		RendererRecovery
	};

	struct GameViewUpdateDecision
	{
		GameViewUpdatePath path = GameViewUpdatePath::InactiveWindow;
		bool updateRenderTarget = false;
		bool queryRenderTexture = false;
		bool publishRuntimePointer = false;
		bool collectRenderStats = false;
	};

	// BuildGameViewUpdateDecision keeps inactive and content-free frames on a
	// bounded path. Active frames retain the existing camera, pointer, and stats
	// behavior; recovery frames only present the recovery message.
	[[nodiscard]] inline GameViewUpdateDecision BuildGameViewUpdateDecision(
		bool windowExpanded,
		bool hasCameraContent,
		bool rendererRecoveryActive,
		bool statsRequested)
	{
		GameViewUpdateDecision decision;
		if (!windowExpanded)
		{
			decision.path = GameViewUpdatePath::InactiveWindow;
			return decision;
		}
		if (!hasCameraContent)
		{
			decision.path = GameViewUpdatePath::ContentFree;
			return decision;
		}
		if (rendererRecoveryActive)
		{
			decision.path = GameViewUpdatePath::RendererRecovery;
			return decision;
		}

		decision.path = GameViewUpdatePath::Active;
		decision.updateRenderTarget = true;
		decision.queryRenderTexture = true;
		decision.publishRuntimePointer = true;
		decision.collectRenderStats = statsRequested;
		return decision;
	}

	// GameViewCameraActivityCache remembers an empty camera query until active
	// object/component membership changes. Large camera-free scenes therefore
	// avoid rebuilding the same vector every editor frame.
	struct GameViewCameraActivityCache
	{
		std::uint64_t sourceMembershipRevision = std::numeric_limits<std::uint64_t>::max();
		bool hasActiveCamera = false;

		[[nodiscard]] bool NeedsRefresh(std::uint64_t currentMembershipRevision) const
		{
			return sourceMembershipRevision != currentMembershipRevision;
		}

		void Store(std::uint64_t currentMembershipRevision, bool currentHasActiveCamera)
		{
			sourceMembershipRevision = currentMembershipRevision;
			hasActiveCamera = currentHasActiveCamera;
		}
	};

	struct GameViewSchedulerDeclaration
	{
		std::string owningSystem = "GameViewWindow";
		std::string schedulerPhase = "EditorUI";
		std::vector<std::string> readResources;
		std::vector<std::string> writeResources;
		std::string activationPredicate;
		std::string workerSafePreparation;
		std::string protectedBoundary;
		std::string visibilityBoundary;
	};

	// The declaration documents the precise worker/protected split even though
	// the current Game View window performs its small active-camera preparation
	// inline. Future scheduler integration can reuse the same resource contract.
	[[nodiscard]] inline GameViewSchedulerDeclaration BuildGameViewSchedulerDeclaration()
	{
		GameViewSchedulerDeclaration declaration;
		declaration.readResources = {
			"EditorWindowVisibility",
			"RuntimeActiveMembershipSnapshot",
			"CameraComponents",
			"RendererRecoverySnapshot",
			"GameViewRenderTexture"
		};
		declaration.writeResources = {
			"GameViewTargetRequest",
			"GameViewCameraRequest",
			"EditorGameViewPointer"
		};
		declaration.activationPredicate = "Window is expanded and has active camera content.";
		declaration.workerSafePreparation = "Read immutable camera membership and prepare camera matrices; no scene mutation is permitted.";
		declaration.protectedBoundary = "ImGui drawing, runtime pointer publication, and RenderSystem request publication.";
		declaration.visibilityBoundary = "Game View requests become visible to rendering after the EditorUI boundary.";
		return declaration;
	}
}


class GameViewWindow : public EditorWindow {
public:
	GameViewWindow(MainEditorSubsystem* owner);

protected:
	virtual void OnBeforeBegin() override;
	virtual void OnGui() override;

private:
	MainEditorSubsystem* m_Owner;


	struct AspectRatio {
		std::string Name;
		float Width;
		float Height;
		bool IsFixedResolution; // If true, Width/Height are pixels. If false, they form a ratio.
	};

	std::vector<AspectRatio> m_AspectRatios;
	int m_SelectedAspectRatioIndex = 0; // Default to Free Aspect

	float m_Scale = 1.0f;
	bool m_MaximizeOnPlay = false;
	bool m_MuteAudio = false;
	bool m_Stats = false;
	bool m_Gizmos = false;
	// The cache is used only for the camera-free fast path. Active Game View
	// frames still obtain current camera transforms so moving cameras stay exact.
	TerraEngine::Editor::GameViewCameraActivityCache m_CameraActivityCache;
	// Game View owns its visualizer preference so Scene View can inspect a
	// different mode at the same time without changing project render settings.
	VirtualGeometryDebugVisualizationMode m_VirtualGeometryDebugMode = VirtualGeometryDebugVisualizationMode::None;
	bool m_VirtualGeometryDebugModeInitialized = false;
	ImVec4 m_StatsToggleInactiveBg = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
	ImVec4 m_StatsToggleHoverBg = ImVec4(0.36f, 0.36f, 0.36f, 0.65f);
	ImVec4 m_StatsToggleActiveBg = ImVec4(0.41f, 0.41f, 0.41f, 0.95f);
	ImVec4 m_StatsToggleText = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);
	float m_StatsToggleRounding = 3.0f;
	ImVec2 m_StatsTogglePadding = ImVec2(10.0f, 4.0f);
};
