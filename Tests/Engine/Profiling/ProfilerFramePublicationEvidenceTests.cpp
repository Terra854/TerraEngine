#include "TestHarness.h"
#include "Profiling/ProfilerCapture.h"
#include "Profiling/Profiler.h"
#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterProfilerFramePublicationEvidenceTests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerFramePublicationEvidence.AvailabilityAndGenerationAreFailClosed", [](TestContext& context)
		{
			using namespace TerraEngine::Profiling;
			ProfilerFramePublicationEvidence evidence;
			evidence.frameGeneration = 12U;
			evidence.viewCount = 1U;
			evidence.views[0].valid = true;
			evidence.views[0].frameGeneration = 12U;
			context.Expect(ValidateProfilerFramePublicationEvidence(evidence), "Coherent optional evidence is accepted.");
			evidence.views[0].frameGeneration = 11U;
			context.Expect(!ValidateProfilerFramePublicationEvidence(evidence), "Mixed generations are rejected atomically.");
			evidence.views[0].frameGeneration = 12U;
			evidence.schemaVersion = 99U;
			context.Expect(!ValidateProfilerFramePublicationEvidence(evidence), "Unknown evidence schemas fail closed.");
			evidence.schemaVersion = ProfilerFramePublicationEvidence::kSchemaVersion;
			evidence.disjointTimingNanoseconds[2] = 77U;
			ProfilerCaptureDocument document;
			ProfilerFrameRecord frame;
			frame.framePublication = evidence;
			document.frames.push_back(frame);
			const auto path = std::filesystem::temp_directory_path() / "frame-publication-evidence.teprof";
			std::string error;
			context.Expect(SaveCaptureDocument(document, path, error), "Evidence capture should save: " + error);
			ProfilerCaptureDocument loaded;
			context.Expect(LoadCaptureDocument(path, loaded, error), "Evidence capture should load: " + error);
			context.Expect(loaded.frames.size() == 1U && loaded.frames[0].framePublication.has_value(), "Evidence should round-trip through v2.");
			if (!loaded.frames.empty() && loaded.frames[0].framePublication) context.ExpectEqual(77ULL, loaded.frames[0].framePublication->disjointTimingNanoseconds[2], "Timing evidence must survive round-trip.");
			std::array<std::uint64_t, 8U> buckets{20U, 30U, 10U, 15U, 5U, 0U, 0U, 0U};
			context.Expect(ValidateProfilerDisjointTimingBuckets(buckets, 80U, 1U), "Disjoint timing buckets should conserve within tolerance.");
			context.Expect(!ValidateProfilerDisjointTimingBuckets(buckets, 70U, 0U), "Timing overrun must be rejected.");
			context.Expect(Profiler::Instance().IsFrameRecordingActive() == false, "Inactive profiler should keep publication hook on its atomic fast path.");
		});

		registry.Add("Engine.ProfilerFramePublicationEvidence.DisjointTimingsReconcileWithoutNestedWorkerDoubleCounting", [](TestContext& context)
		{
			using namespace TerraEngine::Profiling;
			constexpr std::uint64_t frameNanoseconds = 10'000U;
			// These buckets are mutually exclusive wall-clock spans. The last lane is
			// the explicit uninstrumented remainder, so their sum describes the whole
			// frame without treating nested worker scopes as additional elapsed time.
			const std::array<std::uint64_t, 8U> reconciled{
				2'000U, 1'500U, 1'000U, 2'500U, 1'000U, 500U, 500U, 1'000U};
			context.Expect(
				ValidateProfilerDisjointTimingBuckets(
					reconciled, frameNanoseconds, frameNanoseconds / 100U),
				"Disjoint phases plus the explicit remainder must reconcile within one percent.");

			const std::array<std::uint64_t, 8U> missingRemainder{
				2'000U, 1'500U, 1'000U, 2'500U, 1'000U, 500U, 500U, 0U};
			context.Expect(
				!ValidateProfilerDisjointTimingBuckets(
					missingRemainder, frameNanoseconds, frameNanoseconds / 100U),
				"A missing uninstrumented remainder must not make an incomplete frame look complete.");

			const std::array<std::uint64_t, 8U> onePercentShort{
				2'000U, 1'500U, 1'000U, 2'500U, 1'000U, 500U, 500U, 900U};
			context.Expect(
				ValidateProfilerDisjointTimingBuckets(
					onePercentShort, frameNanoseconds, frameNanoseconds / 100U),
				"Exactly one percent of clock uncertainty is accepted by the reconciliation contract.");
			context.Expect(
				!ValidateProfilerDisjointTimingBuckets(
					onePercentShort, frameNanoseconds, frameNanoseconds / 100U - 1U),
				"The same evidence must fail when it exceeds the declared tolerance.");

			// Adding a nested 3 ms worker scope to its already-inclusive 8 ms parent
			// would claim 11 ms of a 10 ms frame. Rejecting that input freezes the UI
			// rule that nested inclusive durations are diagnostic, not additive.
			const std::array<std::uint64_t, 8U> doubleCountedNestedWorker{
				8'000U, 3'000U, 0U, 0U, 0U, 0U, 0U, 0U};
			context.Expect(
				!ValidateProfilerDisjointTimingBuckets(
					doubleCountedNestedWorker, frameNanoseconds, frameNanoseconds / 100U),
				"Nested worker inclusive time must never be added to its enclosing wall span.");
		});
	}
}
