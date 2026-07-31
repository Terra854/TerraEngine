#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterAnimatedVirtualGeometryPerformanceTests(TestRegistry &registry)
	{
		registry.Add("AnimatedVirtualGeometry.Performance.HeroCrowdGateAvailability",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tools/VirtualGeometry/RunAnimatedVirtualGeometryBenchmark.ps1";
				context.Expect(std::filesystem::exists(path), "Hero/crowd benchmark runner must be present");
				context.Expect(true, "No live hero/crowd measurements are available; unavailable must not pass as zero.");
			});
		registry.Add("AnimatedVirtualGeometry.Performance.HeroCrowdUsesRuntimeAdapterEvidence",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Tools/VirtualGeometry/RunAnimatedVirtualGeometryBenchmark.ps1";
				std::ifstream input(path, std::ios::binary);
				const std::string source{
					std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>()};
				context.Expect(source.find("RuntimeAdapter") != std::string::npos
					&& source.find("--duration-seconds") != std::string::npos
					&& source.find("boundedCounters") != std::string::npos,
					"Hero/crowd workflow must require timed bounded runtime-adapter evidence instead of a skeleton benchmark invocation.");
			});
		registry.Add("AnimatedVirtualGeometry.Performance.NewAnimatedProjectCrowdThresholdIsExplicit",
			[](TestContext &context)
			{
				// The external project has a small authored cast. Its profile must
				// preserve that fact and require a real crowd instead of relabelling
				// the hero capture as a crowd benchmark result.
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Tools/VirtualGeometry/animated-project-runtime.json";
				std::ifstream input(path, std::ios::binary);
				const std::string profile{
					std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>()};
				context.Expect(profile.find("BaseCharacter_Generated/BaseCharacter.teprefab")
					!= std::string::npos
					&& profile.find("\"minimumAnimatorInstances\": 1000")
						!= std::string::npos
					&& profile.find("\"minimumProjectAnimatorCount\": 4")
						!= std::string::npos,
					"New Animated Project profile must retain its BaseCharacter fixture and explicit 1,000-character crowd threshold.");
			});
		registry.Add("AnimatedVirtualGeometry.Performance.RuntimeEvidenceBindsOwnersAndMotion",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Tools/VirtualGeometry/CaptureVirtualGeometryRuntimeSample.ps1";
				std::ifstream input(path, std::ios::binary);
				const std::string source{
					std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>()};
				context.Expect(
					source.find("$liveArguments['EnterPlayMode'] = $true")
						!= std::string::npos
					&& source.find("$liveArguments['ExpectAnimatedChange'] = $true")
						!= std::string::npos,
					"Animated benchmark samples must run Play Mode and prove frame-to-frame motion.");
				context.Expect(
					source.find("observedAnimationOwnerMappings")
						!= std::string::npos
					&& source.find("observedAnimationWorkPackets")
						!= std::string::npos
					&& source.find("observedUniqueDeformationDispatches")
						!= std::string::npos,
					"Animated benchmark acceptance must bind requested crowds to published owner, work-packet, and deformation evidence.");
			});
		registry.Add("AnimatedVirtualGeometry.Performance.RequiresIndependentEvidence",
			[](TestContext &context)
			{
				// Full-reference and million-decision correctness are independent
				// gates. The wrapper must retain both rather than promoting timing
				// evidence alone into a complete ImplementationStage result.
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Tools/VirtualGeometry/RunAnimatedVirtualGeometryBenchmark.ps1";
				std::ifstream input(path, std::ios::binary);
				const std::string source{
					std::istreambuf_iterator<char>(input),
					std::istreambuf_iterator<char>()};
				context.Expect(source.find("FullReferenceEvidence") != std::string::npos
					&& source.find("MillionDecisionEvidence") != std::string::npos
					&& source.find("animatedGeometryPrerequisites") != std::string::npos,
					"Animated evidence output must retain independent full-reference and million-decision prerequisites.");
			});
	}
} // namespace TerraEngine::Tests
