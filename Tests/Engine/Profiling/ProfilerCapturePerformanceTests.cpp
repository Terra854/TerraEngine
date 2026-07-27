#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <chrono>
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

		registry.Add("Engine.ProfilerCapturePerformance.TenThousandFramePersistenceStaysLinear", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			ProfilerCaptureDocument document;
			document.metadata.settings.detailLevel = ProfilerCaptureDetailLevel::Aggregate;
			document.metadata.settings.maximumChunkFrames = 1024u;
			document.metadata.settings.maximumChunkBytes = 8u * 1024u * 1024u;
			document.frames.reserve(10000u);
			for (std::uint64_t index = 0u; index < 10000u; ++index)
			{
				// Repeated typed rows reproduce the shape of a high-frame-rate live
				// aggregate capture without depending on a renderer or wall clock.
				ProfilerFrameRecord frame;
				frame.frameIndex = index;
				frame.durationMs = 1.0;
				frame.performanceStages.push_back({
					PerformanceStageDomain::Physics,
					"Animation.Evaluate",
					{TelemetryAvailability::Available, 250000u, {}},
					"AnimationSystem",
					4u,
					false});
				frame.physicsWorkVolume = PhysicsWorkVolumeRecord{
					index, 10u, 10u, 10u, 10u, 10u, 10u, 10u};
				document.frames.push_back(std::move(frame));
			}

			const std::filesystem::path path =
				std::filesystem::temp_directory_path() / "TerraEngine-ten-thousand-frame-linear.teprof";
			std::string error;
			const auto started = std::chrono::steady_clock::now();
			const bool saved = SaveCaptureDocument(document, path, error);
			const auto elapsed = std::chrono::steady_clock::now() - started;
			context.Expect(saved, "The 10,000-frame aggregate capture should save: " + error);
			context.Expect(elapsed < std::chrono::seconds(5),
				"Chunk persistence must remain linear rather than repeatedly serializing the growing chunk.");

			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		});
	}
}
