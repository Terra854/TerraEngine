#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterProfilerPerformanceSummaryTests(TestRegistry& registry)
	{
		registry.Add("ProfilerPerformanceSummary.TimingPercentilesKeepUnitsAndAvailability", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			std::vector<ProfilerFrameRecord> frames;
			for (std::uint64_t index = 0u; index < 5u; ++index)
			{
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				AddOrReplaceTimingStreamSample(frame, {"physics.provider_reads", "Physics provider reads", ProfilerTimingStreamScope::EngineCpu, ProfilerTimingStreamSource::ProfilerClock, ProfilerTimingStreamStatus::Valid, static_cast<double>((index + 1u) * 10u), {}});
				frames.push_back(std::move(frame));
			}

			const std::vector<ProfilerTimingStreamAggregate> summaries = BuildTimingStreamAggregates(frames);
			context.ExpectEqual(std::size_t(1u), summaries.size(), "A typed timing identity should produce one bounded aggregate row.");
			if (!summaries.empty())
			{
				const ProfilerTimingStreamAggregate& summary = summaries.front();
				context.ExpectEqual(std::string("physics.provider_reads"), summary.id, "Summary identity should retain the stage name for regression comparison.");
				context.ExpectEqual(std::size_t(5u), summary.sampleCount, "Every valid stage sample should participate in percentiles.");
				context.ExpectEqual(30.0, summary.medianMs, "Median should retain the timing unit in milliseconds.");
				context.ExpectEqual(48.0, summary.p95Ms, "P95 should interpolate deterministically for a short capture.");
				context.ExpectEqual(49.6, summary.p99Ms, "P99 should identify near-maximum stage cost.");
				context.ExpectEqual(50.0, summary.maxMs, "Maximum should preserve the observed worst stage duration.");
			}
		});

		registry.Add("ProfilerPerformanceSummary.UnavailableStreamDoesNotBecomeZero", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerFrameRecord frame;
			frame.frameIndex = 1u;
			AddOrReplaceTimingStreamSample(frame, {"scene_view.projection", "Scene View projection", ProfilerTimingStreamScope::ViewportCpu, ProfilerTimingStreamSource::Unavailable, ProfilerTimingStreamStatus::Unavailable, 0.0, "Scene View hidden"});

			const std::vector<ProfilerTimingStreamAggregate> summaries = BuildTimingStreamAggregates({frame});
			context.ExpectEqual(std::size_t(1u), summaries.size(), "Unavailable streams should remain represented for availability diagnostics.");
			if (!summaries.empty())
			{
				context.ExpectEqual(std::size_t(0u), summaries.front().sampleCount, "Unavailable streams must not participate as a zero-duration percentile sample.");
				context.ExpectEqual(std::size_t(1u), summaries.front().missingCount, "Unavailable streams should report their missing observation count.");
				context.Expect(summaries.front().selectedStatus == ProfilerTimingStreamStatus::Unavailable, "The selected stream status should preserve unavailable semantics.");
			}
		});
	}
}
