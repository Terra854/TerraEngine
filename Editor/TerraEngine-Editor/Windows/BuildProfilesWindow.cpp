// BuildProfilesWindow.cpp — Export build profiles window implementation
//
// Implements the ImGui-based interface for build profile management. It handles profile lifecycle actions like creation, duplication, and deletion, and integrates with the ThreadPool to run the export process in the background.

// BuildProfilesWindow.cpp — ImGui implementation for the build profiles editor window.

#define IMGUI_DEFINE_MATH_OPERATORS
#include "BuildProfilesWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Core/EditorContext.h"
#include "../Project/ProjectManager.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace
{
	constexpr float kProfileListWidth = 220.0f;

	// CollectProjectScenePaths is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::vector<std::string> CollectProjectScenePaths(const std::filesystem::path& projectRoot)
	{
		std::vector<std::string> scenePaths;
		const std::filesystem::path assetsRoot = projectRoot / "Assets";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::filesystem::exists(assetsRoot)) {
			return scenePaths;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".tescene") {
				continue;
			}

			std::error_code ec;
			std::string scenePath = std::filesystem::relative(entry.path(), assetsRoot, ec).generic_string();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!ec && !scenePath.empty()) {
				scenePaths.push_back(std::move(scenePath));
			}
		}

		std::sort(scenePaths.begin(), scenePaths.end());
		return scenePaths;
	}

	// DrawSectionHeader is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void DrawSectionHeader(const char* title)
	{
		ImGui::TextUnformatted(title);
		ImGui::Spacing();
	}

	void DrawHoverTooltip(const char* text)
	{
		// Build profile controls are dense, so short hover text keeps rarely
		// used export-only switches discoverable without adding permanent copy.
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", text);
		}
	}

}

BuildProfilesWindow::BuildProfilesWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Build Profiles", false), m_Owner(owner)
{
	// Default profile directory is relative to the editor working directory.
	m_ProfileDirectory = std::filesystem::current_path() / "BuildProfiles";
	LoadProfiles();
}

// BuildProfilesWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::OnGui() {
	constexpr ImGuiTableFlags layoutFlags =
		ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_NoSavedSettings;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (profile.isDirty) {
				label += " *";
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("New", ImVec2(0, 0))) {
		CreateNewProfile();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Duplicate", ImVec2(0, 0))) {
		DuplicateSelectedProfile();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Delete", ImVec2(0, 0))) {
		DeleteSelectedProfile();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::InputText("Output Dir", outputBuf, sizeof(outputBuf))) {
		profile.outputDirectory = outputBuf;
		profile.isDirty = true;
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Checkbox("Enable Managed Compilation", &profile.enableManagedCompilation)) {
		profile.isDirty = true;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Checkbox("Include Debug Symbols", &profile.includeDebugSymbols)) {
		profile.isDirty = true;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Checkbox("Strip NativeAOT Output", &profile.stripNativeAotOutput)) {
		profile.isDirty = true;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Checkbox("Export Vulkan Validation Layers", &profile.enableVulkanValidationLayers)) {
		profile.isDirty = true;
	}
	DrawHoverTooltip("Requests Vulkan validation layers when this exported build starts. Editor validation remains controlled by Project Settings.");

	ImGui::Spacing();

	// TEPAK settings
	DrawSectionHeader("TEPAK Settings");
	const char* presetItems[] = { "None", "Fast", "Normal", "Best" };
	int presetIndex = static_cast<int>(profile.compressionPreset);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Combo("Compression", &presetIndex, presetItems, IM_ARRAYSIZE(presetItems))) {
		profile.compressionPreset = static_cast<CompressionPreset>(presetIndex);
		profile.isDirty = true;
	}

	int workers = static_cast<int>(profile.tepakWorkerCount);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::InputInt("Workers (0=auto)", &workers)) {
			profile.tepakWorkerCount = static_cast<uint32_t>(std::max(0, workers));
			profile.isDirty = true;
		}

	ImGui::Spacing();

	ImGui::Spacing();

	// Save button is optional now due to auto-save, but keeping it for explicit saves
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Save Profile")) {
		SaveSelectedProfile();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.isDirty) {
		ImGui::SameLine();
		ImGui::TextDisabled("Unsaved changes (auto-saves on change)");
	}

	// Handle File Dialog
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGuiFileDialog::Instance()->Display("ChooseOutputDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string selectedPath = ImGuiFileDialog::Instance()->GetFilePathName();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Validate")) {
		RunValidation();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!m_LastValidationResult.diagnostics.empty()) {
			if (ImGui::BeginChild("ValidationDiagnostics", ImVec2(0, 120), true)) {
				for (const auto& diag : m_LastValidationResult.diagnostics) {
					ImVec4 color;
					const char* prefix;
					// Dispatch by explicit engine state so each case documents one supported behavior.
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
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_ExportInProgress) {
		// Show progress bar and status.
		ImGui::ProgressBar(m_CurrentProgress.overallProgress,
			ImVec2(-1, 0),
			m_CurrentProgress.statusMessage.c_str());
	}
	else {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Export Build", ImVec2(-1, 30))) {
			StartExport();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Build And Run", ImVec2(-1, 30))) {
			StartExportAndRun();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_HasExportResult && !m_ExportInProgress) {
		ImGui::Spacing();
		if (m_LastExportResult.success) {
			ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f),
				"Export succeeded in %.1fs", m_LastExportResult.totalElapsedSeconds);
			ImGui::TextDisabled("Output: %s", m_LastExportResult.outputDirectory.string().c_str());
			
			if (m_RunAfterExport) {
				m_RunAfterExport = false;
				const std::filesystem::path projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
				const bool hasSelectedProfile = m_SelectedProfileIndex >= 0 && m_SelectedProfileIndex < static_cast<int>(m_Profiles.size());
				const BuildProfile* selectedProfile = hasSelectedProfile ? &m_Profiles[m_SelectedProfileIndex] : nullptr;
				const std::string fallbackName = selectedProfile ? selectedProfile->name : "TerraEngine";
				const ExportPlatform platform = selectedProfile ? selectedProfile->platform : GetHostExportPlatform();
				std::string projectName = projectRoot.filename().string();
				if (projectName.empty()) {
					projectName = fallbackName;
				}
				const std::string launcherPath = (m_LastExportResult.outputDirectory / GetExportLauncherFileName(projectName, platform)).string();
				// Hand the built launcher to the OS after export completes; this mirrors
				// the editor's lightweight external-tool launch behavior.
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
	profile.platform = GetHostExportPlatform();
	profile.isDirty = true;
	m_Profiles.push_back(std::move(profile));
	m_SelectedProfileIndex = static_cast<int>(m_Profiles.size()) - 1;
}

// BuildProfilesWindow::DuplicateSelectedProfile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// BuildProfilesWindow::DeleteSelectedProfile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::DeleteSelectedProfile() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	const auto& profile = m_Profiles[m_SelectedProfileIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!profile.filePath.empty() && std::filesystem::exists(profile.filePath)) {
		std::filesystem::remove(profile.filePath);
	}

	m_Profiles.erase(m_Profiles.begin() + m_SelectedProfileIndex);
	m_SelectedProfileIndex = std::min(m_SelectedProfileIndex,
		static_cast<int>(m_Profiles.size()) - 1);
	m_HasValidationResult = false;
	m_HasExportResult = false;
}

// BuildProfilesWindow::SaveSelectedProfile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::SaveSelectedProfile() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	auto& profile = m_Profiles[m_SelectedProfileIndex];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.id.empty()) {
		profile.id = GenerateBuildProfileId();
	}

	const auto path = m_ProfileDirectory / (profile.id + ".json");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (SaveBuildProfile(profile, path)) {
		profile.filePath = path;
		profile.isDirty = false;
	}
}

// BuildProfilesWindow::AutoSaveDirtyProfiles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// BuildProfilesWindow::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::OnProjectOpened() {
	if (m_Owner && m_Owner->GetContext() && m_Owner->GetContext()->projectManager->HasActiveProject()) {
		m_ProfileDirectory = m_Owner->GetContext()->projectManager->GetProjectRoot() / "BuildProfiles";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::filesystem::exists(m_ProfileDirectory)) {
			std::filesystem::create_directories(m_ProfileDirectory);
		}
		LoadProfiles();
	}
}

// BuildProfilesWindow::RunValidation is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::RunValidation() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}

	std::filesystem::path projectRoot = m_Owner->GetContext()->projectManager->GetProjectRoot();
	ExportValidator validator;
	m_LastValidationResult = validator.ValidateAll(m_Profiles[m_SelectedProfileIndex], projectRoot);
	m_HasValidationResult = true;
}

// BuildProfilesWindow::StartExport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::StartExport() {
	if (m_SelectedProfileIndex < 0 || m_SelectedProfileIndex >= static_cast<int>(m_Profiles.size())) {
		return;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	auto exportTask = ThreadPool::instance().createTask([this, profile, projectRoot]() {
		// Unload the gameplay assembly before starting managed compilation to
		// avoid file locks on Assembly-CSharp.dll.
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (profile.enableManagedCompilation) {
			ScriptingSystem::Instance().ClearGameplayAssembly();
		}

		ExportPipeline pipeline;
		m_LastExportResult = pipeline.Execute(profile, projectRoot,
			[this](const ExportProgress& progress) {
				m_CurrentProgress = progress;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (profile.enableManagedCompilation) {
			ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
		}
	});
	ThreadPool::instance().enqueue(exportTask);
}

// BuildProfilesWindow::StartExportAndRun is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void BuildProfilesWindow::StartExportAndRun() {
	StartExport();
	if (m_ExportInProgress) {
		m_RunAfterExport = true;
	}
}
