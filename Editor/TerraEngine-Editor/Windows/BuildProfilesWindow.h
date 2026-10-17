// BuildProfilesWindow.h — Export build profiles editor window
//
// Defines the UI for managing project export configurations. it provides an interface for editing build settings, running pre-export validation, and initiating the full export pipeline for different target platforms.

#pragma once

#include "../Framework/EditorWindow.h"
#include "../../Export/BuildProfile.h"
#include "../../Export/ExportValidator.h"
#include "../../Export/ExportPipeline.h"

#include <string>
#include <vector>

class MainEditorSubsystem;
struct EditorContext;
class EditorExportOperationController;


class BuildProfilesWindow : public EditorWindow {
public:
	BuildProfilesWindow(MainEditorSubsystem* owner);
	// Explicit owner-thread context also supports non-ImGui editor hosts that
	// invoke the same frontend actions without initializing an entire UI shell.
	explicit BuildProfilesWindow(EditorContext& context);

protected:
	void OnGui() override;

private:
	MainEditorSubsystem* m_Owner;
	EditorContext* m_ExplicitContext = nullptr;
	EditorContext* Context() const;
	void StartCapturedExport(bool launchAfterSuccess);
	std::shared_ptr<EditorExportOperationController> m_ExportController;
	std::uint64_t m_OperationId = 0;

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
	void PollExportAtProtectedFrame();
	// The operation handle supports owner diagnostics and frontend correlation.
	std::uint64_t GetExportOperationId() const { return m_OperationId; }
	};
