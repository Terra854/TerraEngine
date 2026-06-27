#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/Graph/RenderSubmissionPlan.h"
#include "Renderer/Graph/RenderWorkerTaskGroup.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>

namespace TerraEngine::Tests {

	void RegisterRenderWorkerSafetyTests(TestRegistry& registry)
	{
		registry.Add("Renderer.WorkerSafety.LiveStateLeakInvalidatesSnapshot", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(16u, 16u);
			snapshot.containsLiveMutableState = true;
			context.Expect(TerraEngine::Renderer::SnapshotContainsLiveMutableState(snapshot), "Worker safety validation should flag live mutable state consumption.");
			context.Expect(!TerraEngine::Renderer::SnapshotUsesOnlyResolvedResources(snapshot), "Unsafe snapshots should not be worker-ready.");
		});

		registry.Add("Renderer.WorkerSafety.WorkerPacketsStopBeforeProtectedSubmit", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(17u, 192u, 8u);
			const auto profile = MakeThreadedRendererProfile();
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto packets = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			const auto submission = TerraEngine::Renderer::BuildRenderSubmissionPlan(snapshot, passes, packets, snapshot.frameId);
			const TerraEngine::Scheduling::SystemDeclaration declaration = RenderSystem::Instance().SchedulerDeclaration();

			context.Expect(packets.workerPacketCount > 0u, "Large render snapshots should produce worker preparation packets.");
			context.Expect(submission.valid, "Prepared render packets should produce a valid protected submission handoff.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "Render should stay classified as partially worker-safe.");
			context.Expect(declaration.protectedCommit.find("queue submit") != std::string::npos, "Live GPU queue submission should remain in the protected commit.");
			context.Expect(std::find(profile.requiredSerialBoundaries.begin(), profile.requiredSerialBoundaries.end(), "final-submit") != profile.requiredSerialBoundaries.end(), "Backend profile should keep final submit serial.");
		});
	}

}
