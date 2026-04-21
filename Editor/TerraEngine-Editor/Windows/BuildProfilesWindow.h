// BuildProfilesWindow.h â€” Editor window for managing export build profiles.
//
// Provides the ImGui-based UI for creating, editing, validating, and running
// export builds. This window docks into the editor layout alongside other
// editor windows.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportValidator.h"
#include "../../Export/ExportPipeline.h"

#include <string>
#include <vector>

class MainEditorSubsystem;

class BuildProfilesWindow : public EditorWindow {
public:
	BuildProfilesWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	MainEditorSubsystem* m_Owner;

	// Currently loaded profiles.
	std::vector<BuildProfile> m_Profiles;
	int m_SelectedProfileIndex = -1;

	// Validation state.
	ExportValidationResult m_LastValidationResult;
	bool m_HasValidationResult = false;

	// Export state.
	bool m_ExportInProgress = false;
	bool m_RunAfterExport = false;
	ExportProgress m_CurrentProgress;
	ExportResult m_LastExportResult;
	bool m_HasExportResult = false;

	// Profile directory (where .json profiles are saved).
	std::filesystem::path m_ProfileDirectory;

	// --- UI sections ---
	void DrawProfileList();
	void DrawProfileEditor();
	void DrawValidationPanel();
	void DrawExportPanel();

	// --- Actions ---
	void CreateNewProfile();
	void DeleteSelectedProfile();
	void DuplicateSelectedProfile();
	void SaveSelectedProfile();
	void AutoSaveDirtyProfiles();
	void LoadProfiles();

	public:
	// Actions
	void OnProjectOpened();
	void RunValidation();
	void StartExport();
	void StartExportAndRun();
	};
