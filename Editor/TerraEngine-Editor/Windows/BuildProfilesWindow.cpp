// BuildProfilesWindow.cpp — ImGui implementation for the build profiles editor window.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "BuildProfilesWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <thread>

namespace
{
	constexpr float kProfileListWidth = 220.0f;

	std::vector<std::string> CollectProjectScenePaths(const std::filesystem::path& projectRoot)
	{
		std::vector<std::string> scenePaths;
		const std::filesystem::path assetsRoot = projectRoot / "Assets";
		if (!std::filesystem::exists(assetsRoot)) {
			return scenePaths;
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".tescene") {
				continue;
			}

			std::error_code ec;
			std::string scenePath = std::filesystem::relative(entry.path(), assetsRoot, ec).generic_string();
			if (!ec && !scenePath.empty()) {
				scenePaths.push_back(std::move(scenePath));
			}
		}

		std::sort(scenePaths.begin(), scenePaths.end());
		return scenePaths;
	}

	void DrawSectionHeader(const char* title)
	{
		ImGui::TextUnformatted(title);
		ImGui::Spacing();
	}
}

BuildProfilesWindow::BuildProfilesWindow(MainEditorSubsystem* owner)
	: EditorWindow("Build Profiles", false), m_Owner(owner)
{
	// Default profile directory is relative to the editor working directory.
	m_ProfileDirectory = std::filesystem::current_path() / "BuildProfiles";
	LoadProfiles();
}

void BuildProfilesWindow::OnGui() {
	constexpr ImGuiTableFlags layoutFlags =
		ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_NoSavedSettings;

	if (ImGui::BeginTable("BuildProfileLayout", 2, layoutFlags, ImGui::GetContentRegionAvail())) {
		ImGui::TableSetupColumn("Profiles", ImGuiTableColumnFlags_WidthFixed, kProfileListWidth);
		ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		ImGui::TableSetColumnIndex(0);
		ImGui::BeginChild("BuildProfilesSidebar", ImVec2(0, 0), true);
		DrawProfileList();
		ImGui::EndChild();

		ImGui::TableSetColumnIndex(1);
		ImGui::BeginChild("BuildProfilesContent", ImVec2(0, 0), true);
		if (m_SelectedProfileIndex >= 0 && m_SelectedProfileIndex < static_cast<int>(m_Profiles.size())) {
			DrawProfileEditor();
			ImGui::Spacing();
			DrawValidationPanel();
			ImGui::Spacing();
			DrawExportPanel();
		}
		else {
			ImGui::TextDisabled("Select or create a build profile.");
		}
		ImGui::EndChild();

		ImGui::EndTable();
	}
	
	AutoSaveDirtyProfiles();
}

// --- Profile List ---

void BuildProfilesWindow::DrawProfileList() {
	DrawSectionHeader("Profiles");

	// Profile selection list.
	const float buttonRowHeight = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
	if (ImGui::BeginChild("ProfileList", ImVec2(0, -buttonRowHeight * 2.0f), true)) {
		for (int i = 0; i < static_cast<int>(m_Profiles.size()); i++) {
			const auto& profile = m_Profiles[i];
			std::string label = profile.name;
			if (profile.isDirty) {
				label += " *";
			}

			if (ImGui::Selectable(label.c_str(), m_SelectedProfileIndex == i)) {
				// Auto-save the previously selected profile if dirty
				if (m_SelectedProfileIndex >= 0 && m_SelectedProfileIndex < static_cast<int>(m_Profiles.size()) && m_Profiles[m_SelectedProfileIndex].isDirty) {
					SaveSelectedProfile();
				}
				m_SelectedProfileIndex = i;
				m_HasValidationResult = false;
				m_HasExportResult = false;
			}
		}
	}
	ImGui::EndChild();

	ImGui::Spacing();

	// Action buttons below the list.
	if (ImGui::Button("New", ImVec2(0, 0))) {
		CreateNewProfile();
	}
	if (ImGui::Button("Duplicate", ImVec2(0, 0))) {
		DuplicateSelectedProfile();
	}
	if (ImGui::Button("Delete", ImVec2(0, 0))) {
		DeleteSelectedProfile();
	}
	if (ImGui::Button("Refresh", ImVec2(0, 0))) {
		LoadProfiles();
	}
}

// --- Profile Editor ---

void BuildProfilesWindow::DrawProfileEditor() {
	auto& profile = m_Profiles[m_SelectedProfileIndex];

	DrawSectionHeader("Profile Settings");

	// Name
	char nameBuf[256] = {};
	std::copy_n(profile.name.begin(), std::min<size_t>(profile.name.size(), sizeof(nameBuf) - 1), nameBuf);
	if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
		profile.name = nameBuf;
		profile.isDirty = true;
	}

	// Platform
	const char* platformItems[] = { "Windows", "Linux" };
	int platformIndex = static_cast<int>(profile.platform);
	if (ImGui::Combo("Platform", &platformIndex, platformItems, IM_ARRAYSIZE(platformItems))) {
		profile.platform = static_cast<ExportPlatform>(platformIndex);
		profile.isDirty = true;
	}

	// Output directory
	char outputBuf[512] = {};
	std::string outputStr = profile.outputDirectory.string();
	std::copy_n(outputStr.begin(), std::min<size_t>(outputStr.size(), sizeof(outputBuf) - 1), outputBuf);
	if (ImGui::InputText("Output Dir", outputBuf, sizeof(outputBuf))) {
		profile.outputDirectory = outputBuf;
		profile.isDirty = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("...##OutputDir")) {
		IGFD::FileDialogConfig config;
		std::filesystem::path projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
		config.path = outputStr.empty() ? projectRoot.string() : outputStr;
		ImGuiFileDialog::Instance()->OpenDialog("ChooseOutputDir", "Select Output Directory", nullptr, config);
	}

	// Startup Scene
	const std::filesystem::path projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
	const std::vector<std::string> scenePaths = CollectProjectScenePaths(projectRoot);
	if (profile.startupScene.empty() && !scenePaths.empty()) {
		// Exports now require a startup scene, so default to the first authored
		// scene instead of exposing an empty "None" state in the UI.
		profile.startupScene = scenePaths.front();
		profile.isDirty = true;
	}

	if (ImGui::BeginCombo("Startup Scene", profile.startupScene.empty() ? "<No scenes found>" : profile.startupScene.c_str())) {
		for (const auto& scenePath : scenePaths) {
			if (ImGui::Selectable(scenePath.c_str(), profile.startupScene == scenePath)) {
				profile.startupScene = scenePath;
				profile.isDirty = true;
			}
		}
		ImGui::EndCombo();
	}

	ImGui::Spacing();

	// Compilation settings
	if (ImGui::Checkbox("Enable Managed Compilation", &profile.enableManagedCompilation)) {
		profile.isDirty = true;
	}
	if (ImGui::Checkbox("Include Debug Symbols", &profile.includeDebugSymbols)) {
		profile.isDirty = true;
	}
	if (ImGui::Checkbox("Strip NativeAOT Output", &profile.stripNativeAotOutput)) {
		profile.isDirty = true;
	}

	ImGui::Spacing();

	ImGui::Spacing();

	// TEPAK settings
	DrawSectionHeader("TEPAK Settings");
	const char* presetItems[] = { "None", "Fast", "Normal", "Best" };
	int presetIndex = static_cast<int>(profile.compressionPreset);
	if (ImGui::Combo("Compression", &presetIndex, presetItems, IM_ARRAYSIZE(presetItems))) {
		profile.compressionPreset = static_cast<CompressionPreset>(presetIndex);
		profile.isDirty = true;
	}

	int workers = static_cast<int>(profile.tepakWorkerCount);
		if (ImGui::InputInt("Workers (0=auto)", &workers)) {
			profile.tepakWorkerCount = static_cast<uint32_t>(std::max(0, workers));
			profile.isDirty = true;
		}

	ImGui::Spacing();

	ImGui::Spacing();

	// Save button is optional now due to auto-save, but keeping it for explicit saves
	if (ImGui::Button("Save Profile")) {
		SaveSelectedProfile();
	}
	if (profile.isDirty) {
		ImGui::SameLine();
		ImGui::TextDisabled("Unsaved changes (auto-saves on change)");
	}

	// Handle File Dialog
	if (ImGuiFileDialog::Instance()->Display("ChooseOutputDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string selectedPath = ImGuiFileDialog::Instance()->GetFilePathName();
			if (selectedPath.empty()) {
				selectedPath = ImGuiFileDialog::Instance()->GetCurrentPath();
			}
			profile.outputDirectory = selectedPath;
			profile.isDirty = true;
		}
		ImGuiFileDialog::Instance()->Close();
	}
}

// --- Validation Panel ---

void BuildProfilesWindow::DrawValidationPanel() {
	DrawSectionHeader("Validation");

	if (ImGui::Button("Validate")) {
		RunValidation();
	}

	if (m_HasValidationResult) {
		ImGui::SameLine();
		if (m_LastValidationResult.passed) {
			ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "PASSED");
		}
		else {
			ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
				"FAILED (%u errors, %u warnings)",
				m_LastValidationResult.errorCount,
				m_LastValidationResult.warningCount);
		}

		// Show diagnostic list.
		if (!m_LastValidationResult.diagnostics.empty()) {
			if (ImGui::BeginChild("ValidationDiagnostics", ImVec2(0, 120), true)) {
				for (const auto& diag : m_LastValidationResult.diagnostics) {
					ImVec4 color;
					const char* prefix;
					switch (diag.severity) {
					case ExportDiagnosticSeverity::Error:
						color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
						prefix = "[ERROR]";
						break;
					case ExportDiagnosticSeverity::Warning:
						color = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
						prefix = "[WARN]";
						break;
					default:
						color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
						prefix = "[INFO]";
						break;
					}
					ImGui::TextColored(color, "%s %s: %s", prefix,
						diag.category.c_str(), diag.message.c_str());
					if (!diag.filePath.empty()) {
						ImGui::SameLine();
						ImGui::TextDisabled("(%s:%d)", diag.filePath.c_str(), diag.line);
					}
				}
			}
			ImGui::EndChild();
		}
	}
}

// --- Export Panel ---

void BuildProfilesWindow::DrawExportPanel() {
	DrawSectionHeader("Export");

	if (m_ExportInProgress) {
		// Show progress bar and status.
		ImGui::ProgressBar(m_CurrentProgress.overallProgress,
			ImVec2(-1, 0),
			m_CurrentProgress.statusMessage.c_str());
	}
	else {
		if (ImGui::Button("Export Build", ImVec2(-1, 30))) {
			StartExport();
		}
		if (ImGui::Button("Build And Run", ImVec2(-1, 30))) {
			StartExportAndRun();
		}
	}

	if (m_HasExportResult && !m_ExportInProgress) {
		ImGui::Spacing();
		if (m_LastExportResult.success) {
			ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
				"Export succeeded in %.1fs", m_LastExportResult.totalElapsedSeconds);
			ImGui::TextDisabled("Output: %s", m_LastExportResult.outputDirectory.string().c_str());
			
			if (m_RunAfterExport) {
				m_RunAfterExport = false;
				std::string launcherPath = (m_LastExportResult.outputDirectory / "TerraEngine-Launcher.exe").string();
				// Use std::system or platform-specific equivalent to launch the process asynchronously
#ifdef _WIN32
				std::string cmd = "start \"\" \"" + launcherPath + "\"";
				std::system(cmd.c_str());
#else
				std::string cmd = "\"" + launcherPath + "\" &";
				std::system(cmd.c_str());
#endif
			}
		}
		else {
			ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f),
				"Export failed: %s", m_LastExportResult.errorMessage.c_str());
		}
	}
}

// --- Actions ---

void BuildProfilesWindow::CreateNewProfile() {
	BuildProfile profile;
	profile.id = GenerateBuildProfileId();
	profile.name = "New Profile";
	profile.isDirty = true;
	m_Profiles.push_back(std::move(profile));
	m_SelectedProfileIndex = static_cast<int>(m_Profiles.size()) - 1;
}

void BuildProfilesWindow::DuplicateSelectedProfile() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}
	
	BuildProfile duplicated = m_Profiles[m_SelectedProfileIndex];
	duplicated.id = GenerateBuildProfileId();
	duplicated.name += " (Copy)";
	duplicated.isDirty = true;
	duplicated.filePath = ""; // Must be re-saved
	m_Profiles.push_back(std::move(duplicated));
	m_SelectedProfileIndex = static_cast<int>(m_Profiles.size()) - 1;
}

void BuildProfilesWindow::DeleteSelectedProfile() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	const auto& profile = m_Profiles[m_SelectedProfileIndex];
	if (!profile.filePath.empty() && std::filesystem::exists(profile.filePath)) {
		std::filesystem::remove(profile.filePath);
	}

	m_Profiles.erase(m_Profiles.begin() + m_SelectedProfileIndex);
	m_SelectedProfileIndex = std::min(m_SelectedProfileIndex,
		static_cast<int>(m_Profiles.size()) - 1);
	m_HasValidationResult = false;
	m_HasExportResult = false;
}

void BuildProfilesWindow::SaveSelectedProfile() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	auto& profile = m_Profiles[m_SelectedProfileIndex];
	if (profile.id.empty()) {
		profile.id = GenerateBuildProfileId();
	}

	const auto path = m_ProfileDirectory / (profile.id + ".json");
	if (SaveBuildProfile(profile, path)) {
		profile.filePath = path;
		profile.isDirty = false;
	}
}

void BuildProfilesWindow::AutoSaveDirtyProfiles() {
	// A simple timer-based or full list validation approach could be here.
	// For now, if the currently selected profile is dirty, keep a flag or save occasionally.
	// We'll trust the explicit save added to selection-change.
}

void BuildProfilesWindow::LoadProfiles() {
	m_Profiles = LoadAllBuildProfiles(m_ProfileDirectory);
	m_SelectedProfileIndex = m_Profiles.empty() ? -1 : 0;
	m_HasValidationResult = false;
	m_HasExportResult = false;
}

void BuildProfilesWindow::OnProjectOpened() {
	if (m_Owner && m_Owner->GetContext() && m_Owner->GetContext()->projectManager->HasActiveProject()) {
		m_ProfileDirectory = m_Owner->GetContext()->projectManager->GetProjectRoot() / "BuildProfiles";
		if (!std::filesystem::exists(m_ProfileDirectory)) {
			std::filesystem::create_directories(m_ProfileDirectory);
		}
		LoadProfiles();
	}
}

void BuildProfilesWindow::RunValidation() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	std::filesystem::path projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
	ExportValidator validator;
	m_LastValidationResult = validator.ValidateAll(m_Profiles[m_SelectedProfileIndex], projectRoot);
	m_HasValidationResult = true;
}

void BuildProfilesWindow::StartExport() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}
	if (m_ExportInProgress) {
		return;
	}

	// Save the profile before exporting to ensure consistency.
	SaveSelectedProfile();

	m_ExportInProgress = true;
	m_HasExportResult = false;
	m_RunAfterExport = false;

	// Run the export pipeline on a background thread so the editor stays
	// responsive during lengthy compilation and packaging steps.
	const auto profile = m_Profiles[m_SelectedProfileIndex];
	const auto projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();

	std::thread exportThread([this, profile, projectRoot]() {
		// Unload the gameplay assembly before starting managed compilation to
		// avoid file locks on Assembly-CSharp.dll.
		if (profile.enableManagedCompilation) {
			ScriptingSystem::Instance().ClearGameplayAssembly();
		}

		ExportPipeline pipeline;
		m_LastExportResult = pipeline.Execute(profile, projectRoot,
			[this](const ExportProgress& progress) {
				m_CurrentProgress = progress;
				if (progress.finished) {
					m_ExportInProgress = false;
					m_HasExportResult = true;
				}
			});

		// Ensure validation results from the pipeline are displayed in the validation panel
		// if we halted at validation or successfully completed validation during export.
		m_LastValidationResult = m_LastExportResult.validationResult;
		m_HasValidationResult = true;

		m_ExportInProgress = false;
		m_HasExportResult = true;

		// Re-trigger a script build/reload to restore editor functionality.
		if (profile.enableManagedCompilation) {
			ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
		}
	});
	exportThread.detach();
}

void BuildProfilesWindow::StartExportAndRun() {
	StartExport();
	if (m_ExportInProgress) {
		m_RunAfterExport = true;
	}
}
