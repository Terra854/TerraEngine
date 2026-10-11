// RendererEfficiencyContractTests.cpp - transient options and protected publication contracts.

#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Renderer;
		constexpr std::uint64_t kMaximumBudget = 1024ull * 1024ull * 1024ull;

		// Values deliberately change several fields together. Rejection must not
		// partially admit these otherwise-valid changes or erase a queued request.
		RendererEfficiencyOptions ReferenceOptions()
		{
			RendererEfficiencyOptions options;
			options.shadowReuse = RendererEfficiencyShadowReuse::AlwaysRefresh;
			options.lightingUploads = RendererEfficiencyLightingUploads::Full;
			options.lightingDepth = RendererEfficiencyLightingDepth::CompleteScan;
			options.shadowSubmission = RendererEfficiencyShadowSubmission::Singleton;
			options.staticShadowCacheBudgetBytes = 0u;
			options.validationPreparation = RendererEfficiencyPreparation::Serial;
			return options;
		}

		bool Contains(const std::vector<std::string>& resources, std::string_view value)
		{
			return std::find(resources.begin(), resources.end(), value) != resources.end();
		}

		// Each invalid field is exercised separately so a validator that checks
		// only its first enum cannot pass by rejecting a multiply-invalid request.
		template<typename Change>
		void ExpectAtomicRejection(TestContext& context, Change change)
		{
			RendererEfficiencyOptionsQueue queue;
			const RendererEfficiencyOptions reference = ReferenceOptions();
			const auto pending = queue.Queue(reference);
			context.Expect(pending.accepted, "The valid request preceding rejection must be admitted.");
			auto invalid = RendererEfficiencyOptions{};
			change(invalid);
			std::string reason;
			context.Expect(!ValidateRendererEfficiencyOptions(invalid, &reason), "An invalid enum or budget must be rejected.");
			context.Expect(!reason.empty(), "Invalid options must explain rejection.");
			const auto rejected = queue.Queue(invalid);
			context.Expect(!rejected.accepted && !rejected.error.empty(), "Queue rejection must return a descriptive error.");
			context.ExpectEqual(std::uint64_t{0}, queue.Snapshot().settingsGeneration, "Rejection cannot advance the applied generation.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Rejection must preserve the prior valid pending request.");
			const auto captured = queue.Snapshot();
			context.Expect(captured.options == reference, "Rejected values cannot replace any part of the pending request.");
			context.ExpectEqual(pending.requestedGeneration, captured.settingsGeneration, "Only the accepted request generation becomes visible.");
		}
	}

	void RegisterRendererEfficiencyContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Contract.DefaultsAndBudgetBounds", [](TestContext& context) {
			RendererEfficiencyOptions options;
			context.Expect(options.shadowReuse == RendererEfficiencyShadowReuse::Enabled, "Shadow reuse defaults to enabled.");
			context.Expect(options.lightingUploads == RendererEfficiencyLightingUploads::Dirty, "Lighting uploads default to dirty ranges.");
			context.Expect(options.lightingDepth == RendererEfficiencyLightingDepth::Cooperative, "Lighting depth defaults to cooperative processing.");
			context.Expect(options.shadowSubmission == RendererEfficiencyShadowSubmission::Grouped, "Shadow submission defaults to grouped.");
			context.Expect(options.validationPreparation == RendererEfficiencyPreparation::Concurrent, "Preparation defaults to concurrent.");
			context.ExpectEqual(std::uint64_t{67108864}, options.staticShadowCacheBudgetBytes, "Static cache defaults to 64 MiB.");
			context.Expect(ValidateRendererEfficiencyOptions(options), "Default options must be valid.");
			options.staticShadowCacheBudgetBytes = 0u;
			context.Expect(ValidateRendererEfficiencyOptions(options), "Zero is a supported cache-disable budget.");
			options.staticShadowCacheBudgetBytes = kMaximumBudget;
			context.Expect(ValidateRendererEfficiencyOptions(options), "The inclusive 1 GiB boundary must be accepted.");
		});

		registry.Add("Renderer.Efficiency.Contract.RejectsShadowReuseAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.shadowReuse = static_cast<RendererEfficiencyShadowReuse>(255u); });
		});
		registry.Add("Renderer.Efficiency.Contract.RejectsLightingUploadsAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.lightingUploads = static_cast<RendererEfficiencyLightingUploads>(255u); });
		});
		registry.Add("Renderer.Efficiency.Contract.RejectsLightingDepthAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.lightingDepth = static_cast<RendererEfficiencyLightingDepth>(255u); });
		});
		registry.Add("Renderer.Efficiency.Contract.RejectsShadowSubmissionAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.shadowSubmission = static_cast<RendererEfficiencyShadowSubmission>(255u); });
		});
		registry.Add("Renderer.Efficiency.Contract.RejectsPreparationAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.validationPreparation = static_cast<RendererEfficiencyPreparation>(255u); });
		});
		registry.Add("Renderer.Efficiency.Contract.RejectsExcessBudgetAtomically", [](TestContext& context) {
			ExpectAtomicRejection(context, [](auto& options) { options.staticShadowCacheBudgetBytes = kMaximumBudget + 1u; });
			ExpectAtomicRejection(context, [](auto& options) { options.staticShadowCacheBudgetBytes = std::numeric_limits<std::uint64_t>::max(); });
		});

		registry.Add("Renderer.Efficiency.Contract.VisibilityRequiresProtectedCapture", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto before = queue.Snapshot();
			context.ExpectEqual(std::uint64_t{1}, before.optionsSessionGeneration, "A new queue starts in its first options token epoch.");
			context.ExpectEqual(std::uint64_t{0}, before.rendererSessionGeneration, "An unbound queue cannot invent a backend session.");
			context.ExpectEqual(std::uint64_t{0}, before.settingsGeneration, "No request has been applied initially.");
			const auto receipt = queue.Queue(ReferenceOptions());
			context.Expect(receipt.accepted && receipt.requestedGeneration > 0u, "An admitted request receives a nonzero generation.");
			context.Expect(queue.Snapshot().options == before.options, "Worker ingress must not change current immutable options.");
			context.ExpectEqual(before.settingsGeneration, queue.Snapshot().settingsGeneration, "Worker ingress must not expose the requested generation early.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Protected capture applies a pending request.");
			const auto captured = queue.Snapshot();
			context.Expect(captured.options == ReferenceOptions(), "Protected capture publishes all fields together.");
			context.ExpectEqual(receipt.requestedGeneration, captured.settingsGeneration, "Captured evidence identifies the applied request.");
			context.Expect(!queue.CaptureAtProtectedBoundary(), "A second capture cannot apply the same request twice.");
			context.ExpectEqual(captured.settingsGeneration, queue.Snapshot().settingsGeneration, "An empty capture keeps its generation stable.");
		});

		registry.Add("Renderer.Efficiency.Contract.RestoresPriorOptionsThroughQueue", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto initial = queue.Snapshot();
			const auto receipt = queue.Queue(ReferenceOptions());
			context.Expect(receipt.accepted && receipt.priorToken.valid, "Accepted controls retain a restorable prior token.");
			context.Expect(receipt.priorToken.options == initial.options, "The token preserves prior option values.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Reference options apply at capture.");
			const auto restore = queue.Restore(receipt.priorToken);
			context.Expect(restore.accepted, "A token from the current renderer session is restorable.");
			context.Expect(queue.Snapshot().options == ReferenceOptions(), "Restoration also waits for the protected boundary.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Protected capture applies restoration.");
			context.Expect(queue.Snapshot().options == initial.options, "Restoration reproduces all prior fields.");
			context.ExpectEqual(restore.requestedGeneration, queue.Snapshot().settingsGeneration, "Restoration produces its own applied generation.");
		});

		registry.Add("Renderer.Efficiency.Contract.CoalescingPreservesEffectivePriorToken", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto first = queue.Queue(ReferenceOptions());
			auto secondOptions = ReferenceOptions();
			secondOptions.staticShadowCacheBudgetBytes = 1048576u;
			const auto second = queue.Queue(secondOptions);
			context.Expect(first.accepted && second.accepted, "Both valid requests are admitted.");
			context.Expect(second.requestedGeneration > first.requestedGeneration, "Queued generations remain ordered before capture.");
			context.Expect(second.priorToken.options == ReferenceOptions(), "The second request restores the effective prior pending options.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Latest-wins ingress publishes one complete request.");
			context.Expect(queue.Snapshot().options == secondOptions, "The newest request wins without mixing fields.");
			context.ExpectEqual(second.requestedGeneration, queue.Snapshot().settingsGeneration, "The latest receipt identifies capture.");
			context.Expect(queue.Restore(second.priorToken).accepted, "The second token can restore its actual prior request.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Prior pending values restore through capture.");
			context.Expect(queue.Snapshot().options == ReferenceOptions(), "Coalesced restoration retains the first request values.");
		});

		registry.Add("Renderer.Efficiency.Contract.RecoveryRejectsStaleTokensAndPendingWork", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto old = queue.Queue(ReferenceOptions());
			context.Expect(old.accepted, "The pre-recovery request is valid.");
			queue.InvalidateSession();
			context.ExpectEqual(std::uint64_t{2}, queue.Snapshot().optionsSessionGeneration, "Recovery advances the options token epoch.");
			context.ExpectEqual(std::uint64_t{0}, queue.Snapshot().rendererSessionGeneration, "Recovery clears the previous backend session binding.");
			context.Expect(!queue.CaptureAtProtectedBoundary(), "Recovery discards requests prepared for the previous session.");
			const auto newRequest = queue.Queue(ReferenceOptions());
			context.Expect(newRequest.accepted, "The recovered session admits new work.");
			const auto rejected = queue.Restore(old.priorToken);
			context.Expect(!rejected.accepted && !rejected.error.empty(), "A previous-session token must reject with a reason.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Rejected stale restoration preserves the new-session pending request.");
			context.ExpectEqual(newRequest.requestedGeneration, queue.Snapshot().settingsGeneration, "Only current-session work becomes visible.");
		});

		registry.Add("Renderer.Efficiency.Contract.InvalidTokenDoesNotErasePendingRequest", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto pending = queue.Queue(ReferenceOptions());
			const auto rejected = queue.Restore(RendererEfficiencyOptionsToken{});
			context.Expect(!rejected.accepted && !rejected.error.empty(), "An uninitialized token cannot restore state.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Invalid token rejection preserves pending options.");
			context.ExpectEqual(pending.requestedGeneration, queue.Snapshot().settingsGeneration, "Invalid tokens cannot alter generation ownership.");
		});

		registry.Add("Renderer.Efficiency.Contract.SchedulerDeclaresProtectedOptionsAndEvidence", [](TestContext& context) {
			const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0f);
			const auto find = [&workloads](std::string_view name) {
				return std::find_if(workloads.begin(), workloads.end(), [name](const auto& workload) { return workload.workloadName == name; });
			};
			const auto capture = find("RenderPublicationCapture");
			const auto commit = find("BackendCommit");
			const auto publication = find("RenderPublication");
			context.Expect(capture != workloads.end() && commit != workloads.end() && publication != workloads.end(), "Efficiency work must retain existing protected workload boundaries.");
			context.Expect(capture->mainThreadRequired && !capture->workerEligible, "Option application stays in protected capture.");
			context.Expect(Contains(capture->reads, "RendererEfficiencyOptionsQueue"), "Capture declares its transient ingress dependency.");
			context.Expect(Contains(capture->writes, "RendererEfficiencyOptions"), "Capture declares immutable options publication.");
			context.Expect(commit->mainThreadRequired && !commit->workerEligible, "Native backend mutation stays protected.");
			context.Expect(Contains(publication->writes, "RendererEfficiencyEvidence"), "Evidence publication declares its output resource.");
			context.Expect(publication->sameFrameVisibilityRequired, "Evidence becomes visible at the existing publication boundary.");
		});

		registry.Add("Renderer.Efficiency.Contract.MissingTimingIsExplicitlyUnavailable", [](TestContext& context) {
			RendererEfficiencyStageTiming timing;
			context.Expect(timing.status == RendererEfficiencyEvidenceStatus::Unavailable, "An absent GPU sample must be explicitly unavailable.");
			context.Expect(!timing.durationMilliseconds.has_value(), "Missing timing must not masquerade as a zero-duration sample.");
			context.Expect(!timing.unavailableReason.empty(), "Unavailable GPU timing must explain the missing evidence.");
			// Pending queries likewise retain no duration until completion supplies
			// a real sample; these states must remain distinguishable in exports.
			timing.status = RendererEfficiencyEvidenceStatus::Pending;
			context.Expect(timing.status != RendererEfficiencyEvidenceStatus::Valid, "A pending query is not valid performance evidence.");
			context.Expect(!timing.durationMilliseconds.has_value(), "Pending work has no completed duration.");
		});

		registry.Add("Renderer.Efficiency.Contract.ImmutableSnapshotHashesExactOptionsAndGenerations", [](TestContext& context) {
			RenderFrameSnapshot baseline;
			baseline.frameId = 42u;
			baseline.efficiencyOptions.settingsGeneration = 7u;
			const auto originalHash = BuildSnapshotDeterministicHash(baseline);
			const auto unchanged = baseline;
			context.ExpectEqual(originalHash, BuildSnapshotDeterministicHash(unchanged), "Copied immutable efficiency inputs retain their deterministic hash.");
			// Change one input at a time: bundling changes would fail to detect an
			// omitted field that happens to accompany another hashed generation.
			const auto expectChanged = [&context, &baseline, originalHash](auto change) {
				auto candidate = baseline;
				change(candidate.efficiencyOptions);
				context.Expect(originalHash != BuildSnapshotDeterministicHash(candidate), "Each exact mode or generation must invalidate prepared snapshot identity.");
				context.ExpectEqual(originalHash, BuildSnapshotDeterministicHash(baseline), "Editing a copy cannot alter the captured baseline.");
			};
			expectChanged([](auto& captured) { ++captured.settingsGeneration; });
			expectChanged([](auto& captured) { ++captured.optionsSessionGeneration; });
			expectChanged([](auto& captured) { ++captured.rendererSessionGeneration; });
			expectChanged([](auto& captured) { captured.options.shadowReuse = RendererEfficiencyShadowReuse::AlwaysRefresh; });
			expectChanged([](auto& captured) { captured.options.lightingUploads = RendererEfficiencyLightingUploads::Full; });
			expectChanged([](auto& captured) { captured.options.lightingDepth = RendererEfficiencyLightingDepth::CompleteScan; });
			expectChanged([](auto& captured) { captured.options.shadowSubmission = RendererEfficiencyShadowSubmission::Singleton; });
			expectChanged([](auto& captured) { ++captured.options.staticShadowCacheBudgetBytes; });
			expectChanged([](auto& captured) { captured.options.validationPreparation = RendererEfficiencyPreparation::Serial; });
		});

		registry.Add("Renderer.Efficiency.Contract.UnavailableViewEvidenceIsCopiedAndNeverMeasuredZero", [](TestContext& context) {
			auto& renderer = RenderSystem::Instance();
			// A deliberately nonexistent view exercises the public production getter
			// without starting Vulkan or changing pending options in this singleton.
			constexpr std::uint32_t missingView = 0xfffffff0u;
			auto evidence = renderer.GetLatestRendererEfficiencyEvidence(RenderViewType::Scene, missingView);
			context.Expect(evidence.status == RendererEfficiencyEvidenceStatus::Unavailable, "A view with no completed publication is unavailable.");
			context.Expect(!evidence.unavailableReason.empty(), "Unavailable view evidence must explain its absence.");
			context.Expect(evidence.viewType == RenderViewType::Scene, "Unavailable evidence preserves requested view type.");
			context.ExpectEqual(missingView, evidence.viewId, "Unavailable evidence preserves requested view identity.");
			const auto expectMissing = [&context](const RendererEfficiencyCounter& counter) {
				context.Expect(counter.status == RendererEfficiencyEvidenceStatus::Unavailable, "Unpublished counters must disclose unavailable status.");
				context.Expect(!counter.value.has_value(), "An unpublished counter must not become measured zero.");
				context.Expect(!counter.reason.empty(), "An unpublished counter needs an unavailable reason.");
			};
			expectMissing(evidence.lightingLogicalWriteBytes);
			expectMissing(evidence.lightingPhysicalFlushBytes);
			expectMissing(evidence.actualDrawSubmissions);
			expectMissing(evidence.staticActiveBytes);
			context.Expect(evidence.lightingDepthTiming.status == RendererEfficiencyEvidenceStatus::Unavailable, "Absent GPU timing remains unavailable in the copied getter.");
			context.Expect(!evidence.lightingDepthTiming.durationMilliseconds.has_value(), "No-backend timing cannot fabricate a duration.");
			context.Expect(!evidence.lightingDepthTiming.unavailableReason.empty(), "Absent GPU timing retains its reason.");
			const auto options = renderer.GetRendererEfficiencyOptionsSnapshot();
			context.Expect(evidence.effectiveOptions.options == options.options, "Unavailable evidence carries the current transient options.");
			context.ExpectEqual(options.settingsGeneration, evidence.effectiveOptions.settingsGeneration, "Unavailable evidence carries the applied generation.");
			// Mutating a returned value proves callers cannot edit renderer-owned
			// publication state through the diagnostics API.
			evidence.lightingLogicalWriteBytes.value = 999u;
			evidence.unavailableReason = "caller-local change";
			const auto repeated = renderer.GetLatestRendererEfficiencyEvidence(RenderViewType::Scene, missingView);
			context.Expect(!repeated.lightingLogicalWriteBytes.value.has_value(), "Getter returns an independent counter copy.");
			context.Expect(repeated.unavailableReason != evidence.unavailableReason, "Getter returns independent diagnostic strings.");
		});

		registry.Add("Renderer.Efficiency.Contract.BackendSessionBindingPreservesIngressAndTokenEpoch", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			const auto request = queue.Queue(ReferenceOptions());
			context.Expect(request.accepted, "Copied ingress can arrive before backend binding.");
			context.ExpectEqual(std::uint64_t{1}, request.priorToken.optionsSessionGeneration, "Restoration tokens identify the options epoch.");
			// Bind the actual backend session independently of the token epoch. A
			// recreated backend may reuse this number; old tokens must still expire.
			queue.BindRendererSession(2u);
			context.ExpectEqual(std::uint64_t{2}, queue.Snapshot().rendererSessionGeneration, "Protected binding records the actual backend session.");
			context.ExpectEqual(std::uint64_t{1}, queue.Snapshot().optionsSessionGeneration, "First backend binding does not expire this queue's token epoch.");
			context.ExpectEqual(std::uint64_t{0}, queue.Snapshot().settingsGeneration, "Binding cannot expose queued options prematurely.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Backend binding preserves the pending ingress request.");
			context.ExpectEqual(request.requestedGeneration, queue.Snapshot().settingsGeneration, "Preserved ingress retains its requested generation.");
			context.ExpectEqual(std::uint64_t{2}, queue.Snapshot().rendererSessionGeneration, "Applying ingress retains the authoritative backend binding.");
			queue.InvalidateSession();
			context.ExpectEqual(std::uint64_t{2}, queue.Snapshot().optionsSessionGeneration, "Session invalidation advances the independent token epoch.");
			context.ExpectEqual(std::uint64_t{0}, queue.Snapshot().rendererSessionGeneration, "Invalidation requires a fresh backend binding.");
			queue.BindRendererSession(2u);
			const auto next = queue.Queue(RendererEfficiencyOptions{});
			const auto stale = queue.Restore(request.priorToken);
			context.Expect(!stale.accepted && !stale.error.empty(), "An old options epoch stays invalid even if the backend session number is reused.");
			context.Expect(queue.CaptureAtProtectedBoundary(), "Rejected stale restoration cannot erase current ingress.");
			context.ExpectEqual(next.requestedGeneration, queue.Snapshot().settingsGeneration, "Current options retain generation ownership.");
		});

		registry.Add("Renderer.Efficiency.Contract.NewLifetimeRestoresDefaultsAndRejectsOldTokens", [](TestContext& context) {
			RendererEfficiencyOptionsQueue queue;
			queue.BindRendererSession(2u);
			const auto old = queue.Queue(ReferenceOptions());
			context.Expect(old.accepted && queue.CaptureAtProtectedBoundary(), "The old lifetime applies transient reference controls.");
			const auto oldSnapshot = queue.Snapshot();
			auto pendingOptions = ReferenceOptions();
			pendingOptions.staticShadowCacheBudgetBytes = 1048576u;
			context.Expect(queue.Queue(pendingOptions).accepted, "The old lifetime also has a valid pending request.");
			// Shutdown/new initialization must not inherit validation overrides from
			// an earlier renderer lifetime, including requests not yet captured.
			queue.ResetForNewLifetime();
			const auto reset = queue.Snapshot();
			context.Expect(reset.options == RendererEfficiencyOptions{}, "A new renderer lifetime starts with default transient options.");
			context.ExpectEqual(oldSnapshot.optionsSessionGeneration + 1u, reset.optionsSessionGeneration, "New lifetime advances the restoration-token epoch.");
			context.ExpectEqual(std::uint64_t{0}, reset.rendererSessionGeneration, "A new lifetime must bind its own backend session.");
			context.Expect(!queue.CaptureAtProtectedBoundary(), "New lifetime discards old pending requests.");
			context.Expect(queue.Snapshot().options == RendererEfficiencyOptions{}, "An empty capture cannot reapply old controls.");
			queue.BindRendererSession(2u);
			const auto restored = queue.Restore(old.priorToken);
			context.Expect(!restored.accepted && !restored.error.empty(), "Reused backend session numbers cannot restore prior-lifetime tokens.");
			context.Expect(!queue.CaptureAtProtectedBoundary(), "Rejected old tokens cannot create pending work in the new lifetime.");
		});

		registry.Add("Renderer.Efficiency.Contract.SchedulerCatalogRegistersReferencedResources", [](TestContext& context) {
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const auto declaration = RenderSystem::Instance().SchedulerDeclaration();
			const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0.0f);
			// Local workload metadata alone cannot prove executable scheduling: the
			// production scheduler rejects names absent from its resource catalog.
			for(const std::string_view name : {"RendererEfficiencyOptionsQueue", "RendererEfficiencyOptions", "RendererEfficiencyEvidence"})
			{
				const bool systemReferences = Contains(declaration.reads, name) || Contains(declaration.writes, name);
				const bool workloadReferences = std::any_of(workloads.begin(), workloads.end(), [name](const auto& workload) {
					return Contains(workload.reads, name) || Contains(workload.writes, name);
				});
				context.Expect(systemReferences && workloadReferences, "Efficiency resource must be declared by its system and executable workloads: " + std::string(name));
				const auto found = std::find_if(resources.begin(), resources.end(), [name](const auto& resource) { return resource.name == name; });
				context.Expect(found != resources.end(), "Production scheduler resource catalog must register: " + std::string(name));
				context.Expect(!found->description.empty(), "Registered efficiency resources must describe their ownership intent.");
				context.Expect(!found->futureComponentScope.empty(), "Registered efficiency resources must describe their protected or immutable access scope.");
			}
		});
	}
}
