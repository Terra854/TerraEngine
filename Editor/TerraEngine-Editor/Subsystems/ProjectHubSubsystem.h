// ProjectHubSubsystem.h — Project hub and wizard system
//
// Defines the subsystem responsible for the initial project selection and creation interface. it manages the recent projects list and provides a step-by-step wizard for creating new projects from engine templates.

#pragma once

#include "IEditorSubsystem.h"
#include <ImGuiFileDialog.h>
#include <functional>

class ProjectHubSubsystem : public IEditorSubsystem {
public:
	virtual void Initialize(EditorContext& context) override;
	virtual void Update(float deltaTime) override;
	virtual void DrawUI() override;
	virtual void Shutdown() override;
	// GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual std::string GetName() const override { return "Project Hub"; }

	// Callback to notify the system that a project has been loaded
	void SetOnProjectLoadedCallback(std::function<void()> callback);

private:
	void DrawSidebar();
	void DrawRecentProjects();
	void DrawNewProjectWizard();

	EditorContext* ctx = nullptr;
	std::function<void()> onProjectLoaded;
	
	// Page names the supported states explicitly so serialization and switches stay readable.
	enum class Page {
		Recent,
		NewProject
	};
	Page currentPage = Page::Recent;

	// Wizard State
	char newProjectName[64] = "New Project";
	std::string newProjectPath;
	int selectedTemplate = 0;
	std::string errorMessage;
};
