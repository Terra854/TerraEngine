#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterProfilerCapturePerformanceTests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerCapturePerformance.FiveSecondAggregateStaysUnderTenMiB", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerCaptureDocument document;
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			document.metadata.settings.maximumChunkFrames = 120u;
			for (std::uint64_t index = 0u; index < 300u; ++index)
			{
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				frame.durationMs = 16.0;
				frame.performanceStages.push_back({PerformanceStageDomain::Physics, "Physics.ProviderRead", {TelemetryAvailability::Available, 1000u, {}}, "PhysicsSystem", 10000u, false});
				frame.physicsWorkVolume = PhysicsWorkVolumeRecord{index, 10000u, 1000u, 10000u, 1000u, 1000u, 10u, 10u};
				document.frames.push_back(std::move(frame));
			}
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngine-five-second-aggregate.teprof";
			std::string error;
			context.Expect(SaveCaptureDocument(document, path, error), "The deterministic five-second gate capture should save: " + error);
			std::error_code sizeError;
			const std::uintmax_t bytes = std::filesystem::file_size(path, sizeError);
			context.Expect(!sizeError && bytes <= 10u * 1024u * 1024u, "The five-second aggregate capture must stay at or below the 10 MiB acceptance gate.");
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		});
	}
}
