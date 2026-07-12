#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class TemporaryProjectWorkspace
		{
		public:
			explicit TemporaryProjectWorkspace(std::string name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineMcpProjectLifecycle" / std::move(name))
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TemporaryProjectWorkspace()
			{
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		struct LifecycleFixture
		{
			explicit LifecycleFixture(TemporaryProjectWorkspace& workspace)
			{
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
			}

			EditorContext context;
			EditorAutomationService service;
		};

	}

	void RegisterEditorMcpProjectLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.ProjectLifecycle.CreateOpenAndCloseAreNotExposed", [](TestContext& context) {
			TemporaryProjectWorkspace workspace("NativeWorkspaceOwnership");
			LifecycleFixture fixture(workspace);

			context.Expect(fixture.service.FindCapability("project.create") == nullptr,
				"MCP discovery must not advertise project creation.");
			context.Expect(fixture.service.FindCapability("project.open") == nullptr,
				"MCP discovery must not advertise project opening.");
			context.Expect(fixture.service.FindCapability("project.close") == nullptr,
				"MCP discovery must not advertise project closing.");
			for (const std::string tool : {std::string("project.create"), std::string("project.open"), std::string("project.close")})
			{
				const nlohmann::json rejected = fixture.service.HandleTool(tool, nlohmann::json::object(), WriteMode::WritesEnabled, "native-project-ownership");
				context.Expect(!rejected.value("ok", true), "Removed project lifecycle tools must reject direct dispatch.");
				context.ExpectEqual(std::string("unsupported_capability"), rejected["error"].value("category", std::string()),
					"Removed project lifecycle tools must report an unsupported capability.");
			}
		});

		registry.Add("Editor.MCP.ProjectLifecycle.HubReadsRemainAvailable", [](TestContext& context) {
			TemporaryProjectWorkspace workspace("HubReads");
			LifecycleFixture fixture(workspace);
			const nlohmann::json templates = fixture.service.HandleTool("project.templates.list", nlohmann::json::object(), WriteMode::ReadOnly, "project-templates-hub");
			const nlohmann::json state = fixture.service.HandleTool("project.state", nlohmann::json::object(), WriteMode::ReadOnly, "project-state-hub");
			context.Expect(templates.value("ok", false), "Template discovery should remain available at the project hub.");
			context.Expect(state.value("ok", false), "Project state should remain available at the project hub.");
			context.Expect(!state["data"].value("hasProject", true), "Hub project state should report that no project is open.");
		});
	}
}
