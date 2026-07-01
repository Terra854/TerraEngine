// GameViewWindow.h — Editor game view window interface
//
// Defines the editor window responsible for presenting the live game output. it provides controls for aspect ratio selection, display scaling, and toggling runtime statistics or gizmos within the editor workspace.

#pragma once
#include "../Framework/EditorWindow.h"
#include "Engine/Renderer/RenderTypes.h"
#include <vector>
#include <string>
#include <imgui.h>

class MainEditorSubsystem;

// class GameViewWindow groups related state and operations so callers have one documented contract.
class GameViewWindow : public EditorWindow {
public:
	GameViewWindow(MainEditorSubsystem* owner);

protected:
	virtual void OnGui() override;

private:
	MainEditorSubsystem* m_Owner;

	// struct AspectRatio groups related state and operations so callers have one documented contract.
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
