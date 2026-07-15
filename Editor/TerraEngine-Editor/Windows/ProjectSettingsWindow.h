// ProjectSettingsWindow.h - Unified editor project settings surface.
//
// Declares the native Project Settings window that consolidates project-wide
// settings categories into one menu destination under Edit.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../Project/ProjectSettingsMetadata.h"

#include <cstddef>
#include <string>
#include <optional>

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
	const std::optional<TerraEngine::Editor::ProjectSettings::PhysicsSettingsPublicationRequest>& GetPendingPhysicsSettingsRequestForTests() const { return m_PendingPhysicsSettingsRequest; }
	// MainEditorSubsystem acknowledges only the sequence it successfully queued.
	// A newer UI edit therefore cannot be erased by a delayed boundary response.
	bool AcknowledgePhysicsSettingsRequest(std::uint64_t sequence)
	{
		if (!m_PendingPhysicsSettingsRequest || m_PendingPhysicsSettingsRequest->requestSequence != sequence)
		{
			return false;
		}
		m_PendingPhysicsSettingsRequest.reset();
		return true;
	}

protected:
	void OnGui() override;

private:
	void DrawCategorySidebar();
	void DrawSelectedCategory();
	void DrawProjectCategory();
	void DrawGraphicsCategory();
	void DrawResourcesCategory();
	void DrawPhysicsCategory();
	void DrawUnavailableState(const char* message);
	void DrawValidationMessage();

	MainEditorSubsystem* m_Owner = nullptr;
	TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId m_SelectedCategory =
		TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Project;
	std::string m_SearchText;
	std::string m_ValidationMessage;
	// The settings window persists validated authoring values, then exposes this
	// request for the editor lifecycle coordinator's protected publication step.
	std::optional<TerraEngine::Editor::ProjectSettings::PhysicsSettingsPublicationRequest> m_PendingPhysicsSettingsRequest;
	std::uint64_t m_NextPhysicsSettingsRequestSequence = 0;
	bool m_PhysicsSettingsLoaded = false;
	// The fixed 32-slot runtime layout stays internal while the editor presents
	// one compact, readable layer editor instead of expanding every raw record.
	std::size_t m_SelectedPhysicsLayerIndex = 0;
	TerraEngine::Physics::PhysicsSettings m_PhysicsSettings = TerraEngine::Physics::PhysicsSettings::Defaults();
};
