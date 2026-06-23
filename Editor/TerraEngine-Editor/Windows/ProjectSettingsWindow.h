// ProjectSettingsWindow.h - Unified editor project settings surface.
//
// Declares the native Project Settings window that consolidates project-wide
// settings categories into one menu destination under Edit.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Project/ProjectSettingsMetadata.h"

#include <string>

class MainEditorSubsystem;

class ProjectSettingsWindow : public EditorWindow
{
public:
	explicit ProjectSettingsWindow(MainEditorSubsystem* owner);

	void OpenToCategory(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId);
	TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId GetSelectedCategoryForTests() const { return m_SelectedCategory; }
	void SetSearchTextForTests(std::string searchText) { m_SearchText = std::move(searchText); }
	std::size_t GetFilteredCategoryCountForTests() const;
	bool HasCategoryForTests(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId) const;

protected:
	void OnGui() override;

private:
	void DrawCategorySidebar();
	void DrawSelectedCategory();
	void DrawProjectCategory();
	void DrawGraphicsCategory();
	void DrawSkyCategory();
	void DrawUnavailableState(const char* message);
	void DrawValidationMessage();

	MainEditorSubsystem* m_Owner = nullptr;
	TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId m_SelectedCategory =
		TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Project;
	std::string m_SearchText;
	std::string m_ValidationMessage;
};
