// ProjectHubSubsystem.cpp — Project hub implementation
//
// Implements the user interface for the editor's project selection screen. It manages the display of recent projects and the new project wizard, providing the initial entry point for users to open or create TerraEngine projects.

#include "ProjectHubSubsystem.h"
#include "ProjectManager.h"
#include "../Project/EditorPreferencesMetadata.h"
#include <imgui.h>
#include "Engine/Core/Logger/Logger.h"
#include <utility>

void ProjectHubSubsystem::Initialize(EditorContext& context) {
	ctx = &context;
	// Default path for new projects could be Documents/TerraProjects
	newProjectPath = std::filesystem::current_path().string();
}


void ProjectHubSubsystem::Shutdown() {
	pendingProjectAction.reset();
	openPreferences = {};
	ctx = nullptr;
}


void ProjectHubSubsystem::Update(float deltaTime) {
	ProcessPendingProjectAction();
}

void ProjectHubSubsystem::SetOnProjectLoadedCallback(std::function<void()> callback) {
	onProjectLoaded = callback;
}

void ProjectHubSubsystem::SetOpenPreferencesCallback(std::function<void()> callback) {
	openPreferences = std::move(callback);
}


void ProjectHubSubsystem::DrawUI() {
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->Pos);
	ImGui::SetNextWindowSize(viewport->Size);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;
	window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("ProjectHubDockSpace", nullptr, window_flags);
	ImGui::PopStyleVar(3);

	// Keep the normal editor-owned Preferences command available at the hub.
	// Project create/open approvals can otherwise wait forever because no project
	// workspace exists yet to expose the main editor menu bar.
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu(TerraEngine::Editor::Preferences::kPreferencesMenuName.data())) {
			if (ImGui::MenuItem(TerraEngine::Editor::Preferences::kPreferencesMenuLabel.data()) && openPreferences) {
				openPreferences();
			}
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}

	// Split window into Sidebar (Left) and Content (Right)
	ImGui::Columns(2, "HubColumns", false);
	ImGui::SetColumnWidth(0, 250.0f);

	DrawSidebar();
	
	ImGui::NextColumn();
	
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 20.0f));

	if (currentPage == Page::Recent) {
		DrawRecentProjects();
	} else if (currentPage == Page::NewProject) {
		DrawNewProjectWizard();
	}
	ImGui::PopStyleVar();

	ImGui::End(); // End Hub Window

	// Handle File Dialogs

	if (ImGuiFileDialog::Instance()->Display("ChooseProjectDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();

			if (filePathName.empty()) {
				filePathName = ImGuiFileDialog::Instance()->GetCurrentPath();
			}
			TERRAENGINE_LOG(LogLevel::Info, "[ProjectHub] Open Project Selected Path: " << filePathName);
			QueueProjectOpen(filePathName);
		}
		ImGuiFileDialog::Instance()->Close();
	}


	if (ImGuiFileDialog::Instance()->Display("ChooseNewProjectDir")) {
		if (ImGuiFileDialog::Instance()->IsOk()) {
			std::string selectedPath = ImGuiFileDialog::Instance()->GetFilePathName();

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


void ProjectHubSubsystem::DrawSidebar() {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
	ImGui::BeginChild("Sidebar", ImVec2(0, 0), true);
	
	ImGui::Spacing();
	ImGui::TextDisabled("  TERRA ENGINE");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();


	if (ImGui::Button("Open Project", ImVec2(-1, 40))) {
		errorMessage.clear();
		IGFD::FileDialogConfig config;
		config.path = ".";
		ImGuiFileDialog::Instance()->OpenDialog("ChooseProjectDir", "Select Project Folder", nullptr, config);
	}
	

	if (ImGui::Button("New Project", ImVec2(-1, 40))) {
		errorMessage.clear();
		currentPage = Page::NewProject;
	}


	if (ImGui::Button("Recent Projects", ImVec2(-1, 40))) {
		errorMessage.clear();
		currentPage = Page::Recent;
	}
	
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextDisabled("  Resources");

	if (ImGui::Button("Documentation", ImVec2(-1, 30))) {
		// Open URL logic
	}

	ImGui::EndChild();
	ImGui::PopStyleColor();
}


void ProjectHubSubsystem::DrawRecentProjects() {
	ImGui::Text("Recent Projects");
	ImGui::Separator();
	ImGui::Spacing();
	

	if (!errorMessage.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: %s", errorMessage.c_str());
		ImGui::Spacing();
	}

	const auto& recents = ctx->projectManager->GetRecentProjects();

	if (recents.empty()) {
		ImGui::TextDisabled("No recent projects found.");
	} else {
		ImGui::BeginChild("RecentList");

		for (const auto& proj : recents) {
			std::string label = proj.name + " (" + proj.path + ")";
			if (ImGui::Button(label.c_str(), ImVec2(-1, 50))) {
				QueueProjectOpen(proj.path);
			}
		}
		ImGui::EndChild();
	}
}


void ProjectHubSubsystem::DrawNewProjectWizard() {
	ImGui::Text("Create New Project");
	ImGui::Separator();
	ImGui::Spacing();
	

	if (!errorMessage.empty()) {
		ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Error: %s", errorMessage.c_str());
		ImGui::Spacing();
	}

	ImGui::InputText("Project Name", newProjectName, 64);
	
	ImGui::LabelText("Location", "%s", newProjectPath.c_str());
	ImGui::SameLine();

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


	if (ImGui::Button("Create", ImVec2(120, 40))) {
		std::filesystem::path finalPath = std::filesystem::path(newProjectPath) / newProjectName;
		QueueProjectCreate(finalPath, newProjectName, selectedTemplate);
	}
	ImGui::SameLine();

	if (ImGui::Button("Cancel", ImVec2(100, 40))) {
		errorMessage.clear();
		currentPage = Page::Recent;
	}
}

void ProjectHubSubsystem::QueueProjectOpen(const std::filesystem::path& path) {
	errorMessage.clear();
	// Project loading changes asset, script, and renderer-facing state, so run it from Update instead of the ImGui draw callback.
	pendingProjectAction = PendingProjectAction {
		.type = PendingProjectActionType::Open,
		.path = path
	};
}

void ProjectHubSubsystem::QueueProjectCreate(const std::filesystem::path& path, std::string name, int templateIndex) {
	errorMessage.clear();
	// Project creation ends by loading the new project, so it follows the same deferred path as opening an existing project.
	pendingProjectAction = PendingProjectAction {
		.type = PendingProjectActionType::Create,
		.path = path,
		.name = std::move(name),
		.templateIndex = templateIndex
	};
}

void ProjectHubSubsystem::ProcessPendingProjectAction() {
	// The editor can shut the hub down while dialogs are closing; keep this guard before touching shared context pointers.
	if (ctx == nullptr || ctx->projectManager == nullptr || !pendingProjectAction.has_value()) {
		return;
	}

	PendingProjectAction action = std::move(*pendingProjectAction);
	pendingProjectAction.reset();

	bool projectLoaded = false;
	if (action.type == PendingProjectActionType::Open) {
		TERRAENGINE_LOG(LogLevel::Info, "[ProjectHub] Opening queued project: " << action.path.string());
		projectLoaded = ctx->projectManager->LoadProject(action.path, &errorMessage);
	} else {
		TERRAENGINE_LOG(LogLevel::Info, "[ProjectHub] Creating queued project: " << action.path.string());
		projectLoaded = ctx->projectManager->CreateProject(
			action.path,
			action.name,
			static_cast<ProjectTemplate>(action.templateIndex),
			&errorMessage
		);
	}

	if (projectLoaded) {
		errorMessage.clear();
		if (onProjectLoaded) {
			onProjectLoaded();
		}
	}
}
