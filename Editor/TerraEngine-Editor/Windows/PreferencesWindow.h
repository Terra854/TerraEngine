// PreferencesWindow.h - Unified editor Preferences surface.
//
// Declares the native Preferences window for editor-wide user settings. It uses
// the same category/sidebar layout as Project Settings so users do not need to
// learn a second settings navigation pattern.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Project/EditorPreferencesMetadata.h"

#include <string>

class MainEditorSubsystem;

class PreferencesWindow : public EditorWindow
{
public:
	explicit PreferencesWindow(MainEditorSubsystem* owner);

	void OpenToCategory(TerraEngine::Editor::Preferences::PreferencesCategoryId categoryId);
	void ApplyStoredPreferencesToRenderSystem();

protected:
	void OnBeforeBegin() override;
	void OnGui() override;

private:
	void DrawCategorySidebar();
	void DrawSelectedCategory();
	void DrawRendererDiagnosticsCategory();
	void DrawSceneViewCategory();
	void DrawMcpBridgeCategory();
	void DrawUnavailableState(const char* message);

	MainEditorSubsystem* m_Owner = nullptr;
	TerraEngine::Editor::Preferences::PreferencesCategoryId m_SelectedCategory =
		TerraEngine::Editor::Preferences::PreferencesCategoryId::Diagnostics;
	std::string m_SearchText;
	int m_McpListenPortDraft = 0;
	bool m_McpListenPortDraftInitialized = false;
	std::string m_SceneViewPreferenceError;
};
