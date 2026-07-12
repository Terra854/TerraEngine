#include "../TestHarness.h"

#include "EditorAutomationJobService.h"

#include <string>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor::Automation;

	namespace
	{
		AutomationJobCreateRequest MakeJob(std::string requestId, bool cancellable = true)
		{
			return AutomationJobCreateRequest{
				"test_work",
				"test.job",
				std::move(requestId),
				"correlation-test",
				{},
				cancellable};
		}
	}

	void RegisterEditorAutomationJobTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Jobs.LifecyclePublishesImmutableSnapshots", [](TestContext& context) {
			EditorAutomationJobService jobs;
			const AutomationJobSnapshot created = jobs.Create(MakeJob("request-lifecycle"));
			context.Expect(created.state == AutomationJobState::Queued, "A new job should begin queued.");
			context.Expect(jobs.SetState(created.jobId, AutomationJobState::Running, "work"), "A queued job should enter running state.");
			context.Expect(jobs.PublishProgress(created.jobId, 0.5, "halfway"), "A running job should publish progress.");
			context.Expect(jobs.CompleteSucceeded(created.jobId, {{"artifact", "done"}}), "A running job should complete successfully.");

			const auto completed = jobs.Get(created.jobId);
			context.Expect(completed.has_value(), "A terminal job must remain queryable in the editor session.");
			context.Expect(completed->state == AutomationJobState::Succeeded, "Completion should publish succeeded state.");
			context.ExpectEqual(std::string("done"), completed->result.value("artifact", std::string()), "The terminal result should be retained.");
			context.Expect(created.state == AutomationJobState::Queued, "Previously returned snapshots must not change when the service updates.");
		});

		registry.Add("Editor.Automation.Jobs.ProgressIsMonotonicAndBounded", [](TestContext& context) {
			EditorAutomationJobService jobs;
			const auto job = jobs.Create(MakeJob("request-progress"));
			context.Expect(jobs.PublishProgress(job.jobId, 0.75, "advanced"), "Valid progress should be accepted.");
			context.Expect(!jobs.PublishProgress(job.jobId, 0.50, "regressed"), "Progress must never move backward.");
			context.Expect(!jobs.PublishProgress(job.jobId, 1.25, "invalid"), "Progress above one must be rejected.");
			context.ExpectEqual(0.75, jobs.Get(job.jobId)->progress, "Rejected progress must not mutate the retained snapshot.");
		});

		registry.Add("Editor.Automation.Jobs.ReconnectLookupRetainsTerminalResult", [](TestContext& context) {
			EditorAutomationJobService jobs;
			const auto accepted = jobs.Create(MakeJob("request-reconnect"));
			const std::string durableId = accepted.jobId;
			context.Expect(jobs.CompleteFailed(durableId, {{"error", "compile failed"}}), "The job should publish its terminal failure.");

			const auto reconnectedLookup = jobs.Get(durableId);
			context.Expect(reconnectedLookup.has_value(), "A reconnect should retrieve a job by its durable ID.");
			context.Expect(reconnectedLookup->state == AutomationJobState::Failed, "Reconnect lookup should preserve terminal state.");
		});

		registry.Add("Editor.Automation.Jobs.CancellationIsCooperative", [](TestContext& context) {
			EditorAutomationJobService jobs;
			const auto cancellable = jobs.Create(MakeJob("request-cancel", true));
			const auto fixed = jobs.Create(MakeJob("request-fixed", false));

			context.Expect(jobs.RequestCancellation(cancellable.jobId), "A cancellable active job should accept a cancellation request.");
			context.Expect(jobs.IsCancellationRequested(cancellable.jobId), "Workers should observe the cooperative cancellation flag.");
			context.Expect(jobs.Get(cancellable.jobId)->state == AutomationJobState::CancelRequested, "Cancellation should be visible without pretending work already stopped.");
			context.Expect(!jobs.RequestCancellation(fixed.jobId), "A non-cancellable job must reject cancellation.");
		});

		registry.Add("Editor.Automation.Jobs.ActiveJobsSurviveCapacityPressure", [](TestContext& context) {
			EditorAutomationJobService jobs(2);
			const auto first = jobs.Create(MakeJob("active-one"));
			const auto second = jobs.Create(MakeJob("active-two"));
			const auto third = jobs.Create(MakeJob("active-three"));

			context.Expect(jobs.Get(first.jobId).has_value(), "Capacity pressure must not evict the first active job.");
			context.Expect(jobs.Get(second.jobId).has_value(), "Capacity pressure must not evict the second active job.");
			context.Expect(third.jobId.empty(), "A bounded store should refuse admission when every retained job is active.");
			context.Expect(third.state == AutomationJobState::Failed, "Admission refusal should be returned as a structured failed snapshot.");
			context.ExpectEqual(std::size_t(2), jobs.Size(), "Refusing admission must preserve the bounded active set.");
		});

		registry.Add("Editor.Automation.Jobs.TerminalHistoryUsesBoundedEviction", [](TestContext& context) {
			EditorAutomationJobService jobs(2);
			const auto first = jobs.Create(MakeJob("terminal-one"));
			context.Expect(jobs.CompleteSucceeded(first.jobId), "The first job should complete.");
			const auto second = jobs.Create(MakeJob("terminal-two"));
			context.Expect(jobs.CompleteSucceeded(second.jobId), "The second job should complete.");
			const auto third = jobs.Create(MakeJob("terminal-three"));
			context.Expect(jobs.CompleteSucceeded(third.jobId), "The third job should complete.");

			context.Expect(jobs.Size() <= 2, "Terminal job retention must remain within capacity.");
			context.Expect(!jobs.Get(first.jobId).has_value(), "Bounded eviction should remove the oldest terminal job first.");
			context.Expect(jobs.OldestRetainedSequence() > first.retentionSequence, "Retention metadata should identify the oldest surviving sequence.");
		});
	}
}
