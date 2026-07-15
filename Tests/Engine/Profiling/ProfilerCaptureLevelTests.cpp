#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterProfilerCaptureLevelTests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerCaptureLevels.RetainCriticalStreamsAtEveryLevel", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			for (const ProfilerCaptureDetailLevel level : {ProfilerCaptureDetailLevel::Detailed, ProfilerCaptureDetailLevel::Sampled, ProfilerCaptureDetailLevel::Aggregate})
			{
				ProfilerCaptureDocument document;
				document.metadata.settings.detailLevel = level;
				document.metadata.settings.sampleIntervalFrames = 4u;
				ProfilerFrameRecord frame;
				frame.frameIndex = 1u;
				frame.samples.push_back({1u, 0u, ProfilerModuleId::CpuEngine, "Optional scope", "main", {}, 0.0, 1.0, 0u, 1u, true});
				frame.performanceStages.push_back({PerformanceStageDomain::Physics, "Physics.ProviderRead", {TelemetryAvailability::Available, 1000u, {}}, "PhysicsSystem", 1u, false});
				frame.physicsWorkVolume = PhysicsWorkVolumeRecord{1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u};
				document.frames.push_back(std::move(frame));
				const std::filesystem::path path = std::filesystem::temp_directory_path() / ("TerraEngine-capture-level-" + std::to_string(static_cast<unsigned int>(level)) + ".teprof");
				std::string error;
				context.Expect(SaveCaptureDocument(document, path, error), "Each capture level should save: " + error);
				ProfilerCaptureDocument loaded;
				context.Expect(LoadCaptureDocument(path, loaded, error), "Each capture level should load: " + error);
				context.Expect(!loaded.frames.empty() && !loaded.frames.front().performanceStages.empty() && loaded.frames.front().physicsWorkVolume.has_value(), "Critical stages and work volumes must survive every capture level.");
				context.Expect(!loaded.frames.empty() && (level == ProfilerCaptureDetailLevel::Detailed || loaded.frames.front().samples.empty()), "Only detailed mode should retain every optional scope at this sampled frame.");
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		});
	}
}
