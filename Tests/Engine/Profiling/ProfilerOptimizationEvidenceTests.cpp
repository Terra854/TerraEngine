#include "TestHarness.h"

#include "Profiling/ProfilerCapture.h"

#include <memory>

namespace TerraEngine::Tests
{
	void RegisterProfilerOptimizationEvidenceTests(TestRegistry& registry)
	{
		registry.Add("Engine.ProfilerOptimizationEvidence.AggregatesConserveAndShareGeneration", [](TestContext& context)
		{
			using namespace TerraEngine::Profiling;
			ProfilerRenderOptimizationEvidence evidence;
			ProfilerRenderOptimizationViewSummary& game = evidence.views[0];
			game.viewType = 1u;
			game.frameGeneration = 42u;
			game.topologyGeneration = 7u;
			game.viewGeneration = 9u;
			game.rendererGeneration = 3u;
			game.visibleInstanceCount = 100u;
			game.groupedInstanceCount = 80u;
			game.fallbackInstanceCount = 20u;
			game.groupedSubmissionCount = 4u;
			game.fallbackSubmissionCount = 20u;
			game.avoidedSubmissionCount = 76u;
			game.instanceStreamBytes = 6400u;
			game.instanceStreamCapacityBytes = 8192u;
			game.valid = true;
			ProfilerRenderOptimizationViewSummary& scene = evidence.views[1];
			scene = game;
			scene.viewType = 2u;
			context.Expect(ValidateProfilerRenderOptimizationEvidence(evidence), "Accepted optimization evidence must satisfy conservation and capacity rules.");
			context.ExpectEqual(42ULL, evidence.views[1].frameGeneration, "All view summaries in one publication must carry the same profiler frame generation.");

			ProfilerFrameRecord frame;
			frame.frameIndex = 42u;
			frame.renderOptimization = evidence;
			ProfilerCaptureDocument document;
			document.frames.push_back(frame);
			context.Expect(ValidateProfilerRenderOptimizationEvidence(*document.frames.front().renderOptimization), "Frame-bound evidence must remain valid after attachment to a capture.");
			ProfilerRenderOptimizationEvidence rejected = evidence;
			rejected.views[0].fallbackInstanceCount = 19u;
			context.Expect(!ValidateProfilerRenderOptimizationEvidence(rejected), "A conservation mismatch must reject the complete evidence candidate.");
		});

		registry.Add("Engine.ProfilerOptimizationEvidence.AnimationCountsRemainTyped", [](TestContext& context)
		{
			using namespace TerraEngine::Profiling;
			ProfilerAnimationFrameTelemetry telemetry;
			telemetry.summary.activeAnimators = 10u;
			telemetry.summary.evaluatedAnimators = 10u;
			telemetry.summary.cpuAnimators = 2u;
			telemetry.summary.gpuVisualAnimators = 8u;
			telemetry.summary.schedulerMembershipChecks = 10u;
			telemetry.details.push_back({ 100u, 1u, 0u, 4u, 3u, 0u, 0u, 0u, true });
			context.Expect(ValidateProfilerAnimationFrameTelemetry(telemetry), "Typed animation evidence must validate without string-derived counters.");
			telemetry.details.push_back(telemetry.details.front());
			context.Expect(!ValidateProfilerAnimationFrameTelemetry(telemetry), "Duplicate detail identities must reject atomically.");
		});

		registry.Add("Engine.ProfilerOptimizationEvidence.AnimationConservationRejectsPartialEvidence", [](TestContext& context)
		{
			using namespace TerraEngine::Profiling;
			ProfilerAnimationOptimizationEvidence evidence;
			evidence.frameIndex = 42U;
			evidence.candidateGeneration = 11U;
			evidence.acceptedGeneration = 11U;
			evidence.topologyGeneration = 7U;
			evidence.presentationGeneration = 11U;
			evidence.rendererGeneration = 3U;
			evidence.sceneGeneration = 5U;
			evidence.activeAnimators = 10U;
			evidence.compactEligibleAnimators = 8U;
			evidence.compactProcessedAnimators = 8U;
			evidence.completeCapturedAnimators = 2U;
			evidence.completeProcessedAnimators = 2U;
			evidence.compactPlanGroups = 1U;
			evidence.compactDispatches = 1U;
			evidence.valid = true;
			context.Expect(ValidateProfilerAnimationOptimizationEvidence(evidence),
				"Disjoint compact and complete ranges with coherent generations must validate.");
			evidence.completeProcessedAnimators = 3U;
			context.Expect(!ValidateProfilerAnimationOptimizationEvidence(evidence),
				"A partially or multiply published complete range must reject atomically.");
		});
	}
}
