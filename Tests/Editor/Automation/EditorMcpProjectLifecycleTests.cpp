#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
			const auto capability = fixture.service.GetRegistry().Find("project.state", 1, fixture.service.BuildAutomationStateSnapshot());
			context.Expect(capability.has_value(), "Project state should remain discoverable while derived-cache evidence is unavailable at the hub.");
			if (capability.has_value())
			{
				const auto& reads = capability->descriptor.execution.preparationReads;
				context.Expect(std::find(reads.begin(), reads.end(), "DerivedSceneCache") != reads.end(), "Project-state diagnostics must declare the derived-scene cache they report.");
			}
		});

		registry.Add("Editor.MCP.ProjectLifecycle.ProjectStateReportsSourceAndDerivedReadiness", [](TestContext& context) {
			TemporaryProjectWorkspace workspace("DerivedReadiness");
			LifecycleFixture fixture(workspace);
			std::string error;
			context.Expect(fixture.context.projectManager->CreateProject(workspace.root, "DerivedReadiness", ProjectTemplate::Blank, &error), error);
			const std::filesystem::path scenePath = workspace.root / "Assets" / "Scenes" / "Automation.tescene";
			std::filesystem::create_directories(scenePath.parent_path());
			{
				std::ofstream scene(scenePath, std::ios::binary | std::ios::trunc);
				scene << R"({"name":"Automation","objects":[{"name":"Root","components":{},"children":[]}]})";
			}

			ProjectSceneLoadDecision cold;
			context.Expect(fixture.context.projectManager->PrepareSceneForLoading("Scenes/Automation.tescene", cold, &error), error);
			const nlohmann::json coldState = fixture.service.HandleTool("project.state", nlohmann::json::object(), WriteMode::ReadOnly, "project-state-cold");
			context.Expect(coldState.value("ok", false), "Project state should expose the cold source decision additively.");
			context.Expect(coldState["data"]["derivedScene"].value("usedAuthoritativeSource", false), "A cold project state should identify authored-source fallback.");
			context.Expect(coldState["data"]["derivedScene"].value("regenerationPublished", false), "A cold project state should report successful atomic regeneration.");
			context.Expect(coldState["data"].contains("readiness"), "Project state should expose explicit simulation/editor readiness without removing legacy fields.");

			ProjectSceneLoadDecision warm;
			context.Expect(fixture.context.projectManager->PrepareSceneForLoading("Scenes/Automation.tescene", warm, &error), error);
			const nlohmann::json warmState = fixture.service.HandleTool("project.state", nlohmann::json::object(), WriteMode::ReadOnly, "project-state-warm");
			context.Expect(warmState["data"]["derivedScene"].value("cacheHit", false), "An unchanged second preparation should be visible as a derived-scene hit.");
			context.ExpectEqual(std::string("derived-hit"), warmState["data"]["derivedScene"].value("state", std::string()), "Project state should use a stable semantic derived-hit state.");
		});
	}
}
