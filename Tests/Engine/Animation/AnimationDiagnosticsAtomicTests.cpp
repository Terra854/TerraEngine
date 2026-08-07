// AnimationDiagnosticsAtomicTests.cpp - Fixed compact evidence publication contracts.

#include "TestHarness.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"

#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	void RegisterAnimationDiagnosticsAtomicTests(TestRegistry& registry)
	{
		registry.Add("Animation.Diagnostics.GpuCountersUsePublishedVulkanPlan", [](TestContext& context) {
			// Keep this integration contract close to the snapshot validator: GPU
			// counters are meaningful only when they name the exact immutable plan
			// that the Vulkan command recorder can consume.
			const std::string path = std::string(TERRAENGINE_SOURCE_DIR)
				+ "/Engine/Animation/AnimationSystem.cpp";
			std::ifstream sourceFile(path, std::ios::binary);
			const std::string source{
				std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>()};
			const std::size_t diagnostics = source.find("BuildActiveDiagnosticsLocked");
			const std::size_t presentation = source.find(
				"presentation->plan.dispatchCommands.size()", diagnostics);
			context.Expect(diagnostics != std::string::npos
				&& presentation != std::string::npos
				&& source.find("presentation->plan.batches.size()", diagnostics) != std::string::npos
				&& source.find("presentation->safeFallback", diagnostics) != std::string::npos
				&& source.find("presentation->sourceGeneration == publicationGeneration", diagnostics)
					!= std::string::npos,
				"GPU diagnostics must count only the current accepted presentation plan.");
			const std::size_t perItemAttempt = source.find("++counters.gpuDispatches", diagnostics);
			context.Expect(perItemAttempt == std::string::npos
				&& source.find("counters.gpuDispatches = presentation->plan.dispatchCommands.size()",
					diagnostics) != std::string::npos,
				"GPU dispatches must reflect Vulkan command-plan entries, not per-item lowering attempts.");
		});

		registry.Add("Animation.Presentation.CapturesOwnerMappingBeforeMovingLoweringInput", [](TestContext& context) {
			// The presentation handoff is fail-closed: one incomplete owner mapping
			// returns no frame and the presentation system safely publishes an empty
			// fallback. Keep the stable owner identity capture ahead of the move so a
			// valid GPU visual input cannot be rejected through moved-from state.
			const std::string path = std::string(TERRAENGINE_SOURCE_DIR)
				+ "/Engine/Animation/AnimationSystem.cpp";
			std::ifstream sourceFile(path, std::ios::binary);
			const std::string source{
				std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>()};
			const std::size_t handoff = source.find("BuildGpuPresentationFrame");
			const std::size_t stableIdentity = source.find(
				"mapping.stableInstanceId = item.gpuLowering->visual.stableInstanceId", handoff);
			const std::size_t transfer = source.find(
				"frame->inputs.push_back(std::move(*item.gpuLowering))", handoff);
			context.Expect(handoff != std::string::npos
				&& stableIdentity != std::string::npos
				&& transfer != std::string::npos
				&& stableIdentity < transfer,
				"GPU presentation must prove the owner mapping before moving its lowering input.");
		});

		registry.Add("Animation.Diagnostics.FixedCompactCountersValidateAtomically", [](TestContext& context) {
			AnimationDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = 11U;
			snapshot.candidateGeneration = 11U;
			snapshot.acceptedGeneration = 11U;
			snapshot.dormant = false;
			snapshot.counters.activeAnimators = 2U;
			snapshot.counters.compactEligibleAnimators = 1U;
			snapshot.counters.compactProcessedAnimators = 1U;
			snapshot.counters.completeCapturedAnimators = 1U;
			snapshot.counters.completeProcessedAnimators = 1U;
			snapshot.counters.fallbackReasonCounts[
				static_cast<std::size_t>(AnimationFallbackReason::None)] = 1U;
			context.Expect(ValidateAnimationDiagnosticsSnapshot(snapshot),
				"Fixed-size compact evidence should publish when every processed animator is accounted for.");
			snapshot.counters.completeProcessedAnimators = 2U;
			context.Expect(!ValidateAnimationDiagnosticsSnapshot(snapshot),
				"An over-counted mixed candidate must be rejected before replacing diagnostics.");
		});
	}
}
