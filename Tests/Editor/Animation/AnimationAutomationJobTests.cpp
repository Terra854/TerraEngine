// Durable retained animation job contract tests.

#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Automation/AnimationAutomationJobs.h"

#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterAnimationAutomationJobTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.Automation.Job.RetainedCancelAndReconnect", [](TestContext& context) {
			using namespace Editor::Automation;
			EditorAutomationJobService service(4U);
			AnimationAutomationJobs jobs(service);
			const auto created = jobs.Start({
				"import",
				"animation.asset.import",
				"request",
				"correlation",
				"key",
				true
			});
			const std::string id = created.data.value("jobId", "");
			context.Expect(
				created.ok && !id.empty(),
				"Long animation work gets a durable retained ID.");
			context.Expect(
				jobs.Get(id).ok
					&& jobs.Cancel(id).ok
					&& jobs.Get(id).data.value("cancelRequested", false),
				"A reconnect can read and cancel its retained animation job.");
		});

		registry.Add("Editor.Animation.Automation.Job.ForeignProviderIsNotVisible", [](TestContext& context) {
			using namespace Editor::Automation;
			EditorAutomationJobService service(4U);
			AnimationAutomationJobs jobs(service);
			const AutomationJobSnapshot foreign = service.Create({
				"export",
				"export.package",
				"request",
				"correlation",
				"key",
				true
			});
			context.Expect(!foreign.jobId.empty(), "The shared store should create the foreign control job.");
			const AutomationDispatchResult read = jobs.Get(foreign.jobId);
			const AutomationDispatchResult cancel = jobs.Cancel(foreign.jobId);
			context.Expect(
				!read.ok && read.error == ErrorCategory::MissingTarget
					&& !cancel.ok && cancel.error == ErrorCategory::MissingTarget,
				"Animation job tools must neither reveal nor cancel another provider's job.");
			context.Expect(
				!service.Get(foreign.jobId)->cancelRequested,
				"Rejecting the foreign cancellation must leave its shared-store state unchanged.");
		});

		registry.Add("Editor.Animation.Automation.Job.AllLongFamiliesRetainProgressAndTerminalResults", [](TestContext& context) {
			using namespace Editor::Automation;
			EditorAutomationJobService service(8U);
			AnimationAutomationJobs jobs(service);
			for (const std::string& capability : {
				"animation.asset.import", "animation.database.build",
				"animation.recording.finalize", "animation.controller.validate" })
			{
				const AutomationDispatchResult started = jobs.Start({
					"animation_long_operation", capability, "request-" + capability,
					"correlation-" + capability, "key-" + capability, true });
				const std::string jobId = started.data.value("jobId", std::string{});
				context.Expect(started.ok && !jobId.empty(),
					"Every long animation family must receive a retained job identity.");
				context.Expect(service.SetState(jobId, AutomationJobState::Running, "worker_preparation")
					&& service.PublishProgress(jobId, 0.5, "protected_publication")
					&& service.CompleteSucceeded(jobId, {{"status", "published"}}),
					"The shared durable store must retain copied progress and terminal results.");
				const AutomationDispatchResult reconnected = jobs.Get(jobId);
				context.Expect(reconnected.ok
					&& reconnected.data.value("progress", 0.0) == 1.0
					&& reconnected.data["result"].value("status", std::string{}) == "published",
					"A reconnect must receive the terminal result without re-running work.");
			}
		});
}
}
