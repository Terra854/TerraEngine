// AnimationDormantPerformanceTests.cpp - Zero-animator foundation contracts.

#include "TestHarness.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Profiling/Profiler.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		const TerraEngine::Profiling::ProfilerModuleSummary* FindProfilerModule(
			const std::vector<TerraEngine::Profiling::ProfilerModuleSummary>& modules,
			TerraEngine::Profiling::ProfilerModuleId id)
		{
			const auto found = std::find_if(modules.begin(), modules.end(), [id](const auto& module) {
				return module.moduleId == id;
			});
			return found == modules.end() ? nullptr : &(*found);
		}

		const TerraEngine::Profiling::ProfilerMetric* FindProfilerMetric(
			const TerraEngine::Profiling::ProfilerModuleSummary& module,
			const std::string& name)
		{
			const auto found = std::find_if(module.metrics.begin(), module.metrics.end(), [&name](const auto& metric) {
				return metric.name == name;
			});
			return found == module.metrics.end() ? nullptr : &(*found);
		}

		bool HasExactPublicDormantProfilerMetrics(const TerraEngine::Profiling::ProfilerFrameRecord& frame)
		{
			using namespace TerraEngine::Profiling;
			const ProfilerModuleSummary* cpu = FindProfilerModule(frame.moduleSummaries, ProfilerModuleId::CpuEngine);
			if (cpu == nullptr)
			{
				return false;
			}

			const ProfilerMetric* membership = FindProfilerMetric(*cpu, "Animation.SchedulerMembershipChecks");
			const ProfilerMetric* dormant = FindProfilerMetric(*cpu, "Animation.Dormant");
			const ProfilerMetric* baseline = FindProfilerMetric(*cpu, "Animation.BaselineIdentityMatched");
			const std::size_t animationMetricCount = static_cast<std::size_t>(std::count_if(cpu->metrics.begin(), cpu->metrics.end(), [](const ProfilerMetric& metric) {
				return metric.name.starts_with("Animation.");
			}));
			return membership != nullptr && membership->value == 1.0 && membership->unit == "count"
				&& dormant != nullptr && dormant->value == 1.0 && dormant->unit == "boolean"
				&& baseline != nullptr && baseline->value == 1.0 && baseline->unit == "boolean"
				&& animationMetricCount == 3U;
		}
	}

	void RegisterAnimationDormantPerformanceTests(TestRegistry& registry) {
		registry.Add("Animation.Dormant.EmptyStoreCreatesNoFrameInputsOrAllocation", [](TestContext& context) {
			AnimatorInstanceStore store;
			std::vector<AnimationFrameInput> inputs;
			const std::size_t capacityBefore = inputs.capacity();
			store.CaptureFrameInputs(17, inputs);

			context.ExpectEqual(static_cast<std::size_t>(0U), store.ActiveCount(),
				"A project without AnimatorComponent membership must keep the runtime store empty.");
			context.Expect(inputs.empty(),
				"An empty runtime store must not create placeholder evaluation records.");
			context.ExpectEqual(capacityBefore, inputs.capacity(),
				"Capturing an empty frame must not allocate reusable frame-input storage.");
		});

		registry.Add("Animation.Dormant.SnapshotProvesZeroSubsystemWork", [](TestContext& context) {
			const AnimationDiagnosticsSnapshot snapshot = MakeDormantAnimationDiagnostics(42U, 84, 1U);
			context.Expect(ValidateAnimationDiagnosticsSnapshot(snapshot),
				"The canonical dormant snapshot should satisfy every zero-work invariant.");
			context.Expect(snapshot.dormant && snapshot.detailLevel != AnimationDiagnosticsDetailLevel::Detailed,
				"Dormant animation may publish bounded summary attribution but no detailed records.");
			context.ExpectEqual(1ULL, snapshot.counters.schedulerMembershipChecks,
				"The dormant path is limited to one bounded scheduler membership check.");
			context.Expect(
				snapshot.counters.activeAnimators == 0U
				&& snapshot.counters.evaluatedAnimators == 0U
				&& snapshot.counters.workerJobs == 0U
				&& snapshot.counters.transientAllocations == 0U
				&& snapshot.counters.poseAllocations == 0U
				&& snapshot.counters.residencyRequests == 0U
				&& snapshot.counters.gpuDispatches == 0U
				&& snapshot.counters.gpuUploadBytes == 0U
				&& snapshot.counters.gpuReadbacks == 0U
				&& snapshot.counters.gpuWaits == 0U
				&& snapshot.counters.physicsHandoffs == 0U
				&& snapshot.counters.managedCallbacks == 0U
				&& snapshot.counters.detailedRecords == 0U,
				"Static projects must create no animation jobs, allocations, residency, GPU, physics, managed, or detailed diagnostic work.");
			context.Expect(std::is_trivially_copyable_v<AnimationDiagnosticsSnapshot>,
				"Foundation snapshots must remain fixed value records with no hidden allocation ownership.");
			context.Expect(sizeof(AnimationDiagnosticsSnapshot) <= 1024U,
				"The always-available summary snapshot must remain a bounded fixed-size record.");
		});

		registry.Add("Animation.Dormant.PublicationCopiesAndRejectsContradictoryWork", [](TestContext& context) {
			ClearPublishedAnimationDiagnostics();
			AnimationDiagnosticsSnapshot dormant = MakeDormantAnimationDiagnostics(7U, 11, 1U);
			std::string featureIdentity(kAnimationFeatureIdentity);
			std::string sourceCommit(kAnimationBaselineSourceCommit);
			std::string manifestHash(kAnimationBaselineManifestSha256);
			std::string evidenceHash(kAnimationBaselineEvidenceSha256);
			std::string workloadsHash(kAnimationPerformanceWorkloadsSha256);
			dormant.baseline = {
				featureIdentity,
				sourceCommit,
				manifestHash,
				evidenceHash,
				workloadsHash
			};
			context.Expect(PublishAnimationDiagnosticsSnapshot(dormant),
				"A valid dormant snapshot should publish at the protected animation boundary.");
			dormant.frameIndex = 99U;
			// Destroy the value of every producer-owned buffer before reading the
			// retained copy. Publication must have rebound each view to its canonical
			// program-lifetime identity rather than borrowing these strings.
			featureIdentity.assign(featureIdentity.size(), 'x');
			sourceCommit.assign(sourceCommit.size(), 'x');
			manifestHash.assign(manifestHash.size(), 'x');
			evidenceHash.assign(evidenceHash.size(), 'x');
			workloadsHash.assign(workloadsHash.size(), 'x');
			const auto copied = AcquireAnimationDiagnosticsSnapshot();
			context.Expect(copied.has_value() && copied->frameIndex == 7U,
				"Consumers must receive a copied immutable value, not the producer's mutable storage.");
			context.Expect(copied.has_value()
				&& copied->baseline.feature == kAnimationFeatureIdentity
				&& copied->baseline.sourceCommit == kAnimationBaselineSourceCommit
				&& copied->baseline.manifestSha256 == kAnimationBaselineManifestSha256
				&& copied->baseline.evidenceSha256 == kAnimationBaselineEvidenceSha256
				&& copied->baseline.workloadsSha256 == kAnimationPerformanceWorkloadsSha256,
				"Profiler evidence must retain every canonical identity after producer buffers are overwritten.");

			AnimationDiagnosticsSnapshot contradictory = MakeDormantAnimationDiagnostics(8U, 12, 1U);
			contradictory.counters.gpuDispatches = 1U;
			context.Expect(!PublishAnimationDiagnosticsSnapshot(contradictory),
				"A snapshot may not claim both dormancy and GPU work.");
			const auto retained = AcquireAnimationDiagnosticsSnapshot();
			context.Expect(retained.has_value() && retained->frameIndex == 7U,
				"Rejected diagnostics publication must retain the complete prior snapshot.");
			ClearPublishedAnimationDiagnostics();
		});

		registry.Add("Animation.Dormant.ProfilerRecordsOnlyBoundedSummaryWhenOptedIn", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			static_assert(std::is_trivially_copyable_v<ProfilerAnimationDiagnosticsFacts>,
				"Dormant profiler evidence must remain fixed typed storage.");
			Profiler& profiler = Profiler::Instance();
			profiler.Clear();
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Animation Dormant Contract";
			ProfilerCaptureSettings settings;
			settings.maxFrames = 2U;
			profiler.StartRecording(metadata, settings);
			profiler.BeginFrame();
			// The scheduler's static-project path records the already-proven dormant
			// facts directly. Public capture readers must still observe exactly the
			// same stable three-metric contract as the full diagnostics path.
			profiler.RecordDormantAnimationFacts(1U);
			profiler.EndFrame();
			profiler.BeginFrame();
			profiler.RecordDormantAnimationFacts(2U);
			profiler.EndFrame();

			const std::vector<ProfilerFrameRecord> frames = profiler.GetFramesSnapshot();
			if(!frames.empty()) {
				profiler.SelectFrame(frames.front().frameIndex);
			}
			const ProfilerCaptureDocument document = profiler.GetDocumentSnapshot();
			const std::optional<ProfilerFrameRecord> selected = profiler.GetSelectedFrameSnapshot();
			const ProfilerJournalFinalizationTicket finishTicket = profiler.FinishRecording();
			const ProfilerCaptureDocument finished = profiler.GetDocumentSnapshot();
			context.Expect(profiler.GetJournalFinalizationStatus(finishTicket).state
				== ProfilerJournalFinalizationState::Completed,
				"Dormant animation diagnostics should use the non-copying finish ticket contract.");
			context.Expect(!frames.empty(), "An opted-in profiler frame should copy the dormant animation summary.");
			context.Expect(frames.size() == 2U && !frames.back().animationDiagnostics.has_value(),
				"A contradictory multi-check dormant claim must be rejected without retaining typed evidence.");
			const std::optional<ProfilerAnimationDiagnosticsFacts> typedFacts = frames.empty()
				? std::optional<ProfilerAnimationDiagnosticsFacts>{}
				: frames.front().animationDiagnostics;
			const ProfilerModuleSummary* cpu = frames.empty()
				? nullptr
				: FindProfilerModule(frames.front().moduleSummaries, ProfilerModuleId::CpuEngine);
			const ProfilerMetric* dormant = cpu != nullptr ? FindProfilerMetric(*cpu, "Animation.Dormant") : nullptr;
			const ProfilerMetric* membership = cpu != nullptr ? FindProfilerMetric(*cpu, "Animation.SchedulerMembershipChecks") : nullptr;
			const ProfilerMetric* baseline = cpu != nullptr ? FindProfilerMetric(*cpu, "Animation.BaselineIdentityMatched") : nullptr;
			const std::size_t animationMetricCount = cpu == nullptr
				? 0U
				: static_cast<std::size_t>(std::count_if(cpu->metrics.begin(), cpu->metrics.end(), [](const ProfilerMetric& metric) {
					return metric.name.starts_with("Animation.");
				}));
			context.Expect(dormant != nullptr && dormant->value == 1.0,
				"The profiler should attribute a captured empty frame as dormant without detailed records.");
			context.Expect(membership != nullptr && membership->value == 1.0,
				"The opted-in summary must preserve the single bounded membership check.");
			context.Expect(baseline != nullptr && baseline->value == 1.0,
				"Profiler evidence should retain the validated frozen baseline identity marker.");
			context.Expect(typedFacts.has_value()
				&& typedFacts->schedulerMembershipChecks == 1U
				&& typedFacts->dormant
				&& typedFacts->baselineIdentityMatched,
				"A copied dormant frame must retain the fixed typed three-fact summary.");
			context.ExpectEqual(static_cast<std::size_t>(3U), animationMetricCount,
				"A dormant captured frame must retain only membership, dormancy, and baseline identity facts.");
			context.Expect(HasExactPublicDormantProfilerMetrics(frames.front()),
				"GetFramesSnapshot must materialize exactly the established public dormant metrics and units.");
			context.Expect(!document.frames.empty() && HasExactPublicDormantProfilerMetrics(document.frames.front()),
				"GetDocumentSnapshot must materialize the complete public dormant metric schema.");
			context.Expect(selected.has_value() && HasExactPublicDormantProfilerMetrics(*selected),
				"GetSelectedFrameSnapshot must materialize the complete public dormant metric schema.");
			context.Expect(!finished.frames.empty() && HasExactPublicDormantProfilerMetrics(finished.frames.front()),
				"FinishRecording must materialize the complete public dormant metric schema.");
			profiler.Clear();
		});
	}
}
