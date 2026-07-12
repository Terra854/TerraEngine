#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::AutomationArtifact;
	using TerraEngine::Editor::Automation::AutomationJobCreateRequest;
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		class ExportJobFixture
		{
		public:
			ExportJobFixture()
				: workspace(std::filesystem::temp_directory_path() / "TerraEngineMcpExportJobs" / "Project")
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace.parent_path(), ignored);
				std::filesystem::create_directories(workspace.parent_path());
				context.projectManager = std::make_shared<ProjectManager>();
				service.SetEditorContext(&context);
				// MCP begins after the owner has opened a workspace in the editor.
				projectReady = context.projectManager->CreateProject(workspace, "ExportJobProject", ProjectTemplate::Blank);

				BuildProfile profile;
				profile.name = "Automation";
				profile.id = "automation";
				profile.outputDirectory = workspace / "Exports" / "Automation";
				profile.startupScene = "Scenes/Scene.tescene";
				profile.enableManagedCompilation = false;
				std::filesystem::create_directories(workspace / "BuildProfiles");
				profileReady = SaveBuildProfile(profile, workspace / "BuildProfiles" / "Automation.json");
			}

			~ExportJobFixture()
			{
				std::error_code ignored;
				std::filesystem::remove_all(workspace.parent_path(), ignored);
			}

			std::filesystem::path workspace;
			EditorContext context;
			EditorAutomationService service;
			bool projectReady = false;
			bool profileReady = false;
		};
	}

	void RegisterEditorMcpExportJobTests(TestRegistry& registry)
	{
		registry.Add("Editor.MCP.ExportJobs.AcceptPollAndReconnect", [](TestContext& context) {
			ExportJobFixture fixture;
			context.Expect(fixture.projectReady && fixture.profileReady, "Export job fixture should create its project and profile.");
			const nlohmann::json accepted = fixture.service.HandleTool(
				"export.start",
				{{"profile", "Automation"}, {"correlationId", "export-correlation"}},
				WriteMode::WritesEnabled,
				"export-start");
			context.Expect(accepted.value("ok", false), "Export start should return promptly with a durable job.");
			context.ExpectEqual(std::string("accepted"), accepted.value("status", std::string()), "Long-running export should return accepted rather than hold the call open.");
			const std::string jobId = accepted["data"].value("jobId", std::string());
			context.Expect(!jobId.empty(), "Accepted export should include a durable job identifier.");

			const nlohmann::json firstPoll = fixture.service.HandleTool("jobs.get", {{"jobId", jobId}}, WriteMode::ReadOnly, "jobs-poll-first");
			context.Expect(firstPoll.value("ok", false), "The accepted export should be pollable immediately.");
			// A later request models a reconnected client in the same editor session;
			// job ownership belongs to the session service, not a socket lifetime.
			const nlohmann::json reconnectPoll = fixture.service.HandleTool("jobs.get", {{"jobId", jobId}}, WriteMode::ReadOnly, "jobs-poll-reconnected");
			context.Expect(reconnectPoll.value("ok", false), "A reconnect should retain access to the export job.");
			context.ExpectEqual(jobId, reconnectPoll["data"]["job"].value("jobId", std::string()), "Reconnect polling should resolve the same job identity.");
		});

		registry.Add("Editor.MCP.ExportJobs.EvidenceAndTerminalResultRemainQueryable", [](TestContext& context) {
			ExportJobFixture fixture;
			context.Expect(fixture.projectReady, "Export job fixture should create its project.");
			const auto job = fixture.service.GetJobService().Create(AutomationJobCreateRequest{
				"export", "export.start", "export-evidence-request", "export-evidence-correlation", "", true
			});
			context.Expect(fixture.service.GetJobService().AddArtifact(job.jobId, AutomationArtifact{
				"artifact-export-evidence", "export_evidence", "Diagnostics/Export/evidence.json", "application/json", {{"validated", true}}
			}), "Fixture should attach export evidence before completion.");
			context.Expect(fixture.service.GetJobService().CompleteSucceeded(job.jobId, {{"output", "Exports/Automation"}, {"evidenceId", "artifact-export-evidence"}}), "Fixture should complete the export job.");

			const nlohmann::json result = fixture.service.HandleTool("jobs.get", {{"jobId", job.jobId}}, WriteMode::ReadOnly, "jobs-terminal-result");
			context.Expect(result.value("ok", false), "Completed export should remain queryable.");
			context.ExpectEqual(std::string("succeeded"), result["data"]["job"].value("state", std::string()), "Terminal job state should be serialized.");
			context.Expect(!result["data"]["job"].value("artifacts", nlohmann::json::array()).empty(), "Terminal export should retain evidence metadata.");
			context.ExpectEqual(std::string("artifact-export-evidence"), result["data"]["job"]["result"].value("evidenceId", std::string()), "Terminal result should reference retained evidence.");
		});

		registry.Add("Editor.MCP.ExportJobs.CancellationIsCooperativeAndObservable", [](TestContext& context) {
			ExportJobFixture fixture;
			context.Expect(fixture.projectReady, "Export job fixture should create its project.");
			const auto job = fixture.service.GetJobService().Create(AutomationJobCreateRequest{
				"export", "export.start", "export-cancel-request", "export-cancel-correlation", "", true
			});
			const nlohmann::json canceled = fixture.service.HandleTool("jobs.cancel", {{"jobId", job.jobId}}, WriteMode::WritesEnabled, "jobs-cancel");
			context.Expect(canceled.value("ok", false), "A cancellable export job should accept cancellation.");
			const nlohmann::json polled = fixture.service.HandleTool("jobs.get", {{"jobId", job.jobId}}, WriteMode::ReadOnly, "jobs-cancel-poll");
			context.Expect(polled["data"]["job"].value("cancelRequested", false), "Cancellation request should be visible to the export worker.");
			context.Expect(polled["data"]["job"].value("state", std::string()) == "cancel_requested" || polled["data"]["job"].value("state", std::string()) == "canceled", "Job state should expose cooperative cancellation progress.");
		});
	}
}
