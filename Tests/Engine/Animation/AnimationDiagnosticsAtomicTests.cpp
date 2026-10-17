// Supplemental source integration checks accompany actual diagnostics validation below.
#include "TestHarness.h"
#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;
	namespace {
		// A missing file or definition must fail before any negative substring assertion.
		std::string ReadDiagnosticsDefinition(TestContext& context, const char* relative,
			const char* declaration, const char* nextDeclaration) {
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + relative, std::ios::binary);
			context.Expect(input.is_open(), "Supplemental diagnostics contract must open its actual production file.");
			const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			context.Expect(!input.bad() && !source.empty(), "Diagnostics source must be readable and nonempty.");
			const auto begin = source.find(declaration);
			context.Expect(begin != std::string::npos, "The compiled diagnostics/presentation definition must exist.");
			const auto end = source.find(nextDeclaration, begin + std::char_traits<char>::length(declaration));
			context.Expect(end != std::string::npos && end > begin, "The actual definition region requires nonempty ordered bounds.");
			return source.substr(begin, end - begin);
		}
	}

	void RegisterAnimationDiagnosticsAtomicTests(TestRegistry& registry) {
		registry.Add("Animation.Diagnostics.GpuCountersUsePublishedVulkanPlan", [](TestContext& context) {
			const auto source = ReadDiagnosticsDefinition(context, "/Engine/Animation/AnimationSystemDiagnostics.cpp",
				"AnimationSystem::Impl::BuildActiveDiagnosticsLocked(", "AnimationSystem::Impl::BuildActiveDetailedDiagnosticsLocked(");
			context.Expect(source.find("presentation->plan.DispatchCommands().size()") != std::string::npos
				&& source.find("presentation->plan.Batches().size()") != std::string::npos
				&& source.find("presentation->safeFallback") != std::string::npos
				&& source.find("presentation->sourceGeneration == publicationGeneration") != std::string::npos,
				"GPU diagnostics must count only the current accepted immutable presentation plan.");
			context.Expect(source.find("++counters.gpuDispatches") == std::string::npos
				&& source.find("counters.gpuDispatches = presentation->plan.DispatchCommands().size()") != std::string::npos,
				"GPU dispatches reflect command-plan entries rather than per-item lowering attempts.");
		});

		registry.Add("Animation.Presentation.CapturesOwnerMappingBeforeMovingLoweringInput", [](TestContext& context) {
			const auto source = ReadDiagnosticsDefinition(context, "/Engine/Animation/Frame/AnimationAuthorityPublication.cpp",
				"AnimationSystem::Impl::BuildGpuPresentationFrame(", "AnimationSystem::Impl::FindPreEvaluationPublication(");
			const auto stableIdentity = source.find("mapping.stableInstanceId");
			const auto transfer = source.find("frame->inputs.push_back(std::move(*item.gpuLowering))");
			context.Expect(stableIdentity != std::string::npos && transfer != std::string::npos && stableIdentity < transfer,
				"The same compiled presentation definition must capture owner identity before moving its lowering input.");
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
