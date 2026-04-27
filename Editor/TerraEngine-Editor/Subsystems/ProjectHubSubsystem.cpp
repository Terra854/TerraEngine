// ProjectHubSubsystem.cpp — Project hub implementation
//
// Implements the user interface for the editor's project selection screen. It manages the display of recent projects and the new project wizard, providing the initial entry point for users to open or create TerraEngine projects.

#include "ProjectHubSubsystem.h"
#include "ProjectManager.h"
#include <imgui.h>
#include "Engine/Core/Logger/Logger.h"

void ProjectHubSubsystem::Initialize(EditorContext& context) {
	ctx = &context;
	// Default path for new projects could be Documents/TerraProjects
	newProjectPath = std::filesystem::current_path().string();
}

// ProjectHubSubsystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::Shutdown() {
	ctx = nullptr;
}

// ProjectHubSubsystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::Update(float deltaTime) {
	// Logic updates if needed
}

void ProjectHubSubsystem::SetOnProjectLoadedCallback(std::function<void()> callback) {
	onProjectLoaded = callback;
}

// ProjectHubSubsystem::DrawUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::DrawUI() {
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
	window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("ProjectHubDockSpace", nullptr, window_flags);
	ImGui::PopStyleVar(3);

	// Split window into Sidebar (Left) and Content (Right)
	ImGui::Columns(2, "HubColumns", false);
	ImGui::SetColumnWidth(0, 250.0f);

	DrawSidebar();
	
	ImGui::NextColumn();
	
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 20.0f));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentPage == Page::Recent) {
		DrawRecentProjects();
	} else if (currentPage == Page::NewProject) {
		DrawNewProjectWizard();
	}
	ImGui::PopStyleVar();

	ImGui::End(); // End Hub Window

	// Handle File Dialogs
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGuiFileDialog::Instance()->Display("ChooseProjectDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (filePathName.empty()) {
				filePathName = ImGuiFileDialog::Instance()->GetCurrentPath();
			}
			TERRAENGINE_LOG(LogLevel::Info, "[ProjectHub] Open Project Selected Path: " << filePathName);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx->projectManager->LoadProject(filePathName, &errorMessage)) {
				errorMessage.clear();
				if (onProjectLoaded) onProjectLoaded();
			}
		}
		ImGuiFileDialog::Instance()->Close();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGuiFileDialog::Instance()->Display("ChooseNewProjectDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string selectedPath = ImGuiFileDialog::Instance()->GetFilePathName();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (selectedPath.empty()) {
				selectedPath = ImGuiFileDialog::Instance()->GetCurrentPath();
			}
			// Keep file-dialog path traces in logger output to simplify debugging project creation flows.
			TERRAENGINE_LOG(LogLevel::Info, "Selected Project Path: " << selectedPath);
			newProjectPath = selectedPath;
		}
		ImGuiFileDialog::Instance()->Close();
	}
}

// ProjectHubSubsystem::DrawSidebar is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::DrawSidebar() {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
	ImGui::BeginChild("Sidebar", ImVec2(0, 0), true);
	
	ImGui::Spacing();
	ImGui::TextDisabled("  TERRA ENGINE");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Open Project", ImVec2(-1, 40))) {
		errorMessage.clear();
		IGFD::FileDialogConfig config;
		config.path = ".";
		ImGuiFileDialog::Instance()->OpenDialog("ChooseProjectDir", "Select Project Folder", nullptr, config);
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("New Project", ImVec2(-1, 40))) {
		errorMessage.clear();
		currentPage = Page::NewProject;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Recent Projects", ImVec2(-1, 40))) {
		errorMessage.clear();
		currentPage = Page::Recent;
	}
	
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextDisabled("  Resources");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Documentation", ImVec2(-1, 30))) {
		// Open URL logic
	}

	ImGui::EndChild();
	ImGui::PopStyleColor();
}

// ProjectHubSubsystem::DrawRecentProjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::DrawRecentProjects() {
	ImGui::Text("Recent Projects");
	ImGui::Separator();
	ImGui::Spacing();
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!errorMessage.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: %s", errorMessage.c_str());
		ImGui::Spacing();
	}

	const auto& recents = ctx->projectManager->GetRecentProjects();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (recents.empty()) {
		ImGui::TextDisabled("No recent projects found.");
	} else {
		ImGui::BeginChild("RecentList");
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& proj : recents) {
			std::string label = proj.name + " (" + proj.path + ")";
			if (ImGui::Button(label.c_str(), ImVec2(-1, 50))) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ctx->projectManager->LoadProject(proj.path, &errorMessage)) {
					errorMessage.clear();
					if (onProjectLoaded) onProjectLoaded();
				}
			}
		}
		ImGui::EndChild();
	}
}

// ProjectHubSubsystem::DrawNewProjectWizard is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectHubSubsystem::DrawNewProjectWizard() {
	ImGui::Text("Create New Project");
	ImGui::Separator();
	ImGui::Spacing();
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!errorMessage.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: %s", errorMessage.c_str());
		ImGui::Spacing();
	}

	ImGui::InputText("Project Name", newProjectName, 64);
	
	ImGui::LabelText("Location", newProjectPath.c_str());
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("...")) {
		IGFD::FileDialogConfig config;
		config.path = ".";
		ImGuiFileDialog::Instance()->OpenDialog("ChooseNewProjectDir", "Select Directory", nullptr, config);
	}

	std::filesystem::path finalPath = std::filesystem::path(newProjectPath) / newProjectName;
	ImGui::TextDisabled("Target: %s", finalPath.string().c_str());

	ImGui::Spacing();
	ImGui::Text("Template");
	const char* templates[] = { "Blank", "2D Core", "3D Core" };
	ImGui::ListBox("##Template", &selectedTemplate, templates, 3);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Create", ImVec2(120, 40))) {
		std::filesystem::path finalPath = std::filesystem::path(newProjectPath) / newProjectName;
		ProjectTemplate tmpl = (ProjectTemplate)selectedTemplate;
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ctx->projectManager->CreateProject(finalPath, newProjectName, tmpl, &errorMessage)) {
			errorMessage.clear();
			if (onProjectLoaded) onProjectLoaded();
		}
	}
	ImGui::SameLine();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Cancel", ImVec2(100, 40))) {
		errorMessage.clear();
		currentPage = Page::Recent;
	}
}
