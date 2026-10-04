#include "../TestHarness.h"
#include "EditorAutomationService.h"

#include "BuildProfile.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include "Editor/Export/EditorExportOperationController.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"

#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::Editor::Automation
{
	// This fixture-only access models a corrupt consumer reference without adding
	// a runtime command that could rewrite controller identities or launch intent.
	struct EditorExportRetentionTestAccess
	{
		static void Track(EditorAutomationService& service, const std::string& jobId,
			std::shared_ptr<EditorExportOperationController> controller, std::uint64_t id)
		{
			service.activeExportOperations[jobId] = {std::move(controller), id, 0};
		}
		static std::size_t Pending(const EditorAutomationService& service) { return service.activeExportOperations.size(); }
	};
}

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
		for (const bool expired : {true, false}) {
			registry.Add(expired ? "Editor.ExportRetention.AutomationExpiredTerminates" : "Editor.ExportRetention.AutomationUnknownTerminates", [expired](TestContext& context) {
				using TerraEngine::Editor::Automation::EditorExportRetentionTestAccess;
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2);
				// Declare pool cleanup first so controller/service ownership retires
				// before the global worker pool is shut down, even after an assertion.
				struct PoolCleanup { ~PoolCleanup() { ThreadPool::instance().shutdown(); } } poolCleanup;
				ExportJobFixture fixture;
				auto controller = std::make_shared<EditorExportOperationController>([](const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
					ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
				});
				std::uint64_t operationId = 1234;
				if (expired) {
					EditorExportOperationRequest request; request.projectGeneration = controller->Generation();
					request.request.runId = "expiry-fixture";
					const auto admission = controller->Admit(request);
					context.Expect(admission.accepted, "The native expiry fixture should admit its owned export.");
					operationId = admission.operationId;
					controller->BeginProject();
				}
				const auto job = fixture.service.GetJobService().Create(AutomationJobCreateRequest{"export", "export.start", "retention-request", "retention-correlation", "", true});
				EditorExportRetentionTestAccess::Track(fixture.service, job.jobId, controller, operationId);
				fixture.service.PumpExportJobsAtProtectedFrame();
				const auto polled = fixture.service.HandleTool("jobs.get", {{"jobId", job.jobId}}, WriteMode::ReadOnly, "retention-poll");
				context.Expect(polled.value("ok", false) && polled["data"]["job"].value("state", std::string{}) == "failed", "Expired or unknown export references must become explicit terminal jobs.");
				context.Expect(polled["data"]["job"]["result"].value("category", std::string{}) == (expired ? "export.expired" : "export.unknown"), "Expiry and unknown references must expose distinguishable terminal diagnostics.");
				context.Expect(EditorExportRetentionTestAccess::Pending(fixture.service) == 0, "A terminal missing reference must relinquish ongoing polling ownership.");
				const auto retained = polled["data"]["job"];
				controller->BeginProject(); fixture.service.PumpExportJobsAtProtectedFrame();
				const auto again = fixture.service.HandleTool("jobs.get", {{"jobId", job.jobId}}, WriteMode::ReadOnly, "retention-reconnect");
				context.Expect(again["data"]["job"]["result"] == retained["result"], "The service's copied terminal outcome must survive controller/project retirement.");
				// A separately adopted successful outcome is copied into durable job
				// state before project expiry, so it must never be relabeled as expired.
				EditorExportOperationRequest successRequest; successRequest.projectGeneration = controller->Generation(); successRequest.request.runId = "copied-success";
				const auto successAdmission = controller->Admit(successRequest);
				const auto successJob = fixture.service.GetJobService().Create(AutomationJobCreateRequest{"export", "export.start", "copied-request", "copied-correlation", "", true});
				EditorExportRetentionTestAccess::Track(fixture.service, successJob.jobId, controller, successAdmission.operationId);
				bool copied = false; const auto deadline = std::chrono::steady_clock::now() + TerraEngine::Tests::Correctness::kDefaultTimeout;
				while (!copied && std::chrono::steady_clock::now() < deadline) { fixture.service.PumpExportJobsAtProtectedFrame(); copied = EditorExportRetentionTestAccess::Pending(fixture.service) == 0; std::this_thread::yield(); }
				context.Expect(successAdmission.accepted && copied, "A real controller success should copy into its automation job.");
				const auto successBefore = fixture.service.HandleTool("jobs.get", {{"jobId", successJob.jobId}}, WriteMode::ReadOnly, "success-before-expiry");
				controller->BeginProject(); fixture.service.PumpExportJobsAtProtectedFrame();
				const auto successAfter = fixture.service.HandleTool("jobs.get", {{"jobId", successJob.jobId}}, WriteMode::ReadOnly, "success-after-expiry");
				context.Expect(successAfter["data"]["job"].value("state", std::string{}) == "succeeded" && successBefore["data"]["job"]["result"] == successAfter["data"]["job"]["result"], "Copied successful job results must persist after project history expires.");
			});
		}
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
