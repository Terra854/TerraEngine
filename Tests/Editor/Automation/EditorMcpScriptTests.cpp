#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::AutomationJobDiagnostic;
	using TerraEngine::Editor::Automation::AutomationLogSeverity;
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class ScriptAutomationFixture
		{
		public:
			explicit ScriptAutomationFixture(std::string name)
				: workspace(std::filesystem::temp_directory_path() / "TerraEngineMcpScripts" / std::move(name)),
				projectRoot(workspace / "Project")
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
				std::filesystem::create_directories(workspace);
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
				// Script tools operate inside an owner-opened workspace and never
				// choose or create that workspace on the client's behalf.
				ready = context.projectManager->CreateProject(projectRoot, "ScriptProject", ProjectTemplate::Blank);
			}

			~ScriptAutomationFixture()
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace, ignored);
			}

			std::filesystem::path workspace;
			std::filesystem::path projectRoot;
			EditorContext context;
			EditorAutomationService service;
			bool ready = false;
		};

		std::string ErrorCategory(const nlohmann::json& result)
		{
			return result.value("error", nlohmann::json::object()).value("category", std::string());
		}
	}

	void RegisterEditorMcpScriptTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.Scripts.ListsOnlyApprovedTemplates", [](TestContext& context) {
			ScriptAutomationFixture fixture("Templates");
			context.Expect(fixture.ready, "Script template fixture should create its project.");
			const nlohmann::json status = fixture.service.HandleTool("script.status", nlohmann::json::object(), WriteMode::ReadOnly, "script-status");
			context.Expect(status.value("ok", false), "Managed readiness should be observable through a read-only script.status capability.");
			context.Expect(status["data"].contains("readiness") && status["data"].contains("fingerprint") && status["data"].contains("loadedOutput"),
				"Script status must expose additive cache, input-fingerprint, and loaded-output identity evidence.");
			context.Expect(status["data"]["readiness"].contains("cache"), "Script status readiness must identify cache hit or miss evidence.");
			const nlohmann::json listed = fixture.service.HandleTool("script.templates.list", nlohmann::json::object(), WriteMode::ReadOnly, "script-templates-list");
			context.Expect(listed.value("ok", false), "Script template discovery should succeed for an open project.");
			const nlohmann::json templates = listed["data"].value("templates", nlohmann::json::array());
			context.Expect(!templates.empty(), "The editor should expose at least one approved script template.");
			for (const nlohmann::json& scriptTemplate : templates)
			{
				context.Expect(!scriptTemplate.value("id", std::string()).empty(), "Approved templates need stable semantic IDs.");
				context.Expect(!scriptTemplate.value("displayName", std::string()).empty(), "Approved templates need user-facing names.");
				context.Expect(!scriptTemplate.contains("absolutePath") && !scriptTemplate.contains("contents"), "Template discovery must not disclose arbitrary source paths or raw file-write payloads.");
			}
		});

		registry.Add("Editor.MCP.Scripts.ValidatesTemplateNameNamespaceAndDestination", [](TestContext& context) {
			ScriptAutomationFixture fixture("Validation");
			context.Expect(fixture.ready, "Script validation fixture should create its project.");
			const nlohmann::json badTemplate = fixture.service.HandleTool(
				"script.create",
				{{"template", "../../ArbitraryFile"}, {"typeName", "PlayerController"}, {"destination", "Assets/Scripts/PlayerController.cs"}},
				WriteMode::WritesEnabled,
				"script-create-template-invalid");
			context.Expect(!badTemplate.value("ok", true), "Only IDs returned by script.templates.list may be used.");
			context.ExpectEqual(std::string("validation"), ErrorCategory(badTemplate), "Unknown templates should return validation errors.");

			const nlohmann::json badName = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "Bad-Type;"}, {"namespace", "Game.Valid"}, {"destination", "Assets/Scripts/BadType.cs"}},
				WriteMode::WritesEnabled,
				"script-create-name-invalid");
			context.Expect(!badName.value("ok", true), "Script type names must be valid managed identifiers.");

			const nlohmann::json badNamespace = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "PlayerController"}, {"namespace", "Game/Invalid"}, {"destination", "Assets/Scripts/PlayerController.cs"}},
				WriteMode::WritesEnabled,
				"script-create-namespace-invalid");
			context.Expect(!badNamespace.value("ok", true), "Script namespaces must be dotted managed identifiers.");

			const nlohmann::json traversal = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "PlayerController"}, {"destination", "../Outside.cs"}},
				WriteMode::WritesEnabled,
				"script-create-path-traversal");
			context.Expect(!traversal.value("ok", true), "Script creation must reject traversal outside project script roots.");
			context.ExpectEqual(std::string("validation"), ErrorCategory(traversal), "Traversal should be a validation error.");

			const nlohmann::json absolute = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "PlayerController"}, {"destination", (fixture.workspace / "Outside.cs").string()}},
				WriteMode::WritesEnabled,
				"script-create-path-absolute");
			context.Expect(!absolute.value("ok", true), "Script creation must reject absolute client-selected destinations.");
			context.ExpectEqual(std::string("validation"), ErrorCategory(absolute), "Absolute script destinations should be validation errors.");
		});

		registry.Add("Editor.MCP.Scripts.CreateSynchronizeAndDiscoverTypes", [](TestContext& context) {
			ScriptAutomationFixture fixture("CreateAndSync");
			context.Expect(fixture.ready, "Script creation fixture should create its project.");
			const nlohmann::json created = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "PlayerController"}, {"namespace", "Game.Play"}, {"destination", "Assets/Scripts/Gameplay/PlayerController.cs"}},
				WriteMode::WritesEnabled,
				"script-create-valid");
			context.Expect(created.value("ok", false), "An approved template and contained destination should create a script.");
			context.ExpectEqual(std::string("Game.Play.PlayerController"), created["data"].value("qualifiedTypeName", std::string()), "Script creation should report the generated managed type.");
			context.Expect(std::filesystem::exists(fixture.projectRoot / "Assets/Scripts/Gameplay/PlayerController.cs"), "The script should be published only at the validated project-relative destination.");

			const nlohmann::json synchronized = fixture.service.HandleTool("script.workspace.sync", nlohmann::json::object(), WriteMode::WritesEnabled, "script-workspace-sync");
			context.Expect(synchronized.value("ok", false), "Workspace synchronization should use the editor-owned workspace service.");
			context.ExpectEqual(std::string("healthy"), synchronized["data"].value("health", std::string()), "Successful synchronization should report semantic workspace health.");
			context.Expect(!synchronized["data"].value("projectPath", std::string()).empty(), "Workspace status should identify its generated project without exposing a command.");

			const nlohmann::json types = fixture.service.HandleTool("script.types.list", nlohmann::json::object(), WriteMode::ReadOnly, "script-types-list");
			context.Expect(types.value("ok", false), "Attachable script type discovery should be queryable independently of Inspector UI.");
			for (const nlohmann::json& type : types["data"].value("types", nlohmann::json::array()))
			{
				context.Expect(!type.value("qualifiedName", std::string()).empty(), "Every attachable script type should have a stable qualified name.");
			}
		});

		registry.Add("Editor.MCP.Scripts.CompilationUsesDurableCancelableJob", [](TestContext& context) {
			ScriptAutomationFixture fixture("CompileJob");
			context.Expect(fixture.ready, "Script compile fixture should create its project.");
			const nlohmann::json accepted = fixture.service.HandleTool(
				"script.compile",
				{{"profile", "Debug"}, {"correlationId", "script-compile-correlation"}},
				WriteMode::WritesEnabled,
				"script-compile-start");
			context.Expect(accepted.value("ok", false), "Script compilation should be accepted without holding the request open.");
			context.ExpectEqual(std::string("accepted"), accepted.value("status", std::string()), "Compilation should return the durable-job envelope.");
			const std::string jobId = accepted["data"].value("jobId", std::string());
			context.Expect(!jobId.empty(), "Accepted compilation should return a job ID.");
			context.ExpectEqual(std::string("jobs.get"), accepted["data"].value("pollTool", std::string()), "Compilation should advertise the common job polling tool.");
			context.Expect(accepted["data"].value("forcedBuild", false), "An explicit script.compile request must state that it bypasses managed-output reuse.");
			context.Expect(accepted["data"].contains("readiness"), "Explicit compilation should append readiness/cache evidence without changing the durable job fields.");
			context.ExpectEqual(std::string("explicit_compile_requested"), accepted["data"]["readiness"].value("cache", nlohmann::json::object()).value("reason", std::string()), "The cache miss reason should explain the user-requested rebuild.");

			const nlohmann::json canceled = fixture.service.HandleTool("jobs.cancel", {{"jobId", jobId}}, WriteMode::WritesEnabled, "script-compile-cancel");
			context.Expect(canceled.value("ok", false), "Queued or running script compilation should accept cooperative cancellation.");
			const nlohmann::json polled = fixture.service.HandleTool("jobs.get", {{"jobId", jobId}}, WriteMode::ReadOnly, "script-compile-cancel-poll");
			context.Expect(polled["data"]["job"].value("cancelRequested", false), "Cancellation state should remain observable after client reconnect.");
		});

		registry.Add("Editor.MCP.Scripts.CompilerDiagnosticsRemainStructuredAndCorrelated", [](TestContext& context) {
			ScriptAutomationFixture fixture("Diagnostics");
			context.Expect(fixture.ready, "Script diagnostic fixture should create its project.");
			const nlohmann::json accepted = fixture.service.HandleTool(
				"script.compile",
				{{"profile", "Debug"}, {"correlationId", "compile-with-error"}},
				WriteMode::WritesEnabled,
				"script-compile-diagnostics");
			context.Expect(accepted.value("ok", false), "Diagnostic fixture should receive a compile job.");
			const std::string jobId = accepted["data"].value("jobId", std::string());
			context.Expect(fixture.service.GetJobService().AddDiagnostic(jobId, AutomationJobDiagnostic{
				AutomationLogSeverity::Error,
				"compiler",
				"; expected",
				{{"code", "CS1002"}, {"file", "Assets/Scripts/Broken.cs"}, {"line", 7}, {"column", 18}}
			}), "The owning compilation subsystem should be able to publish a structured diagnostic.");
			context.Expect(fixture.service.GetJobService().CompleteFailed(jobId, {{"exitCode", 1}}), "The fixture should publish a terminal compiler failure.");

			const nlohmann::json polled = fixture.service.HandleTool("jobs.get", {{"jobId", jobId}}, WriteMode::ReadOnly, "script-compile-diagnostics-poll");
			context.ExpectEqual(std::string("failed"), polled["data"]["job"].value("state", std::string()), "Compiler failure should remain a durable terminal job result.");
			const nlohmann::json diagnostics = polled["data"]["job"].value("diagnostics", nlohmann::json::array());
			context.ExpectEqual(static_cast<std::size_t>(1), diagnostics.size(), "Compiler diagnostics should survive job serialization.");
			context.ExpectEqual(std::string("CS1002"), diagnostics.front().value("details", nlohmann::json::object()).value("code", std::string()), "Diagnostic details should preserve compiler code.");
			context.ExpectEqual(std::string("compile-with-error"), polled["data"]["job"].value("correlationId", std::string()), "Compilation diagnostics should remain attached to the request correlation ID.");
		});

		registry.Add("Editor.MCP.Scripts.ExposesNoArbitraryWriteProcessOrCommandSurface", [](TestContext& context) {
			ScriptAutomationFixture fixture("NoArbitraryCommands");
			context.Expect(fixture.ready, "Script safety fixture should create its project.");
			const nlohmann::json arbitraryPayload = fixture.service.HandleTool(
				"script.create",
				{{"template", "component"}, {"typeName", "Injected"}, {"destination", "Assets/Scripts/Injected.cs"}, {"contents", "arbitrary source"}, {"command", "whoami"}},
				WriteMode::WritesEnabled,
				"script-arbitrary-payload");
			context.Expect(!arbitraryPayload.value("ok", true), "script.create must reject raw contents and command fields outside its typed schema.");

			for (const std::string tool : {"script.execute", "filesystem.write", "process.start", "terminal.run"})
			{
				const nlohmann::json result = fixture.service.HandleTool(tool, nlohmann::json::object(), WriteMode::WritesEnabled, "script-forbidden-" + tool);
				context.Expect(!result.value("ok", true), "Automation must not expose arbitrary execution or filesystem tools.");
				context.ExpectEqual(std::string("unsupported_capability"), ErrorCategory(result), "Forbidden generic tools should remain unsupported capabilities.");
			}
		});
	}
}
