#include "TestHarness.h"
#include "Renderer/RendererRecovery.h"
#include "Renderer/RendererRecoveryPolicy.h"
#include "Renderer/RendererRecoveryTestFixtures.h"

namespace TerraEngine::Tests
{
	void RegisterRendererRecoveryRebuildTests(TestRegistry& registry)
	{
		registry.Add("RendererRecovery.Rebuild.PolicyAllowsBoundedAttempts", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeLatchedRendererDeviceLoss();
			const TerraEngine::Renderer::RendererRecoveryPolicy policy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();
			const TerraEngine::Renderer::RendererRecoveryPolicyDecision firstAttempt = TerraEngine::Renderer::EvaluateRendererRecoveryPolicy(snapshot, policy, 0u);

			context.Expect(firstAttempt.canAttempt, "The editor policy should allow the first renderer rebuild attempt.");
			context.ExpectEqual(1u, firstAttempt.nextAttemptNumber, "The next attempt number should be one-based.");
			context.Expect(!firstAttempt.budgetExhausted, "The first attempt should be within the bounded recovery budget.");
		});

		registry.Add("RendererRecovery.Rebuild.SuccessPublishesFreshSession", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot rebuilding = TerraEngine::Renderer::BeginRecoveryRebuild(
				MakeLatchedRendererDeviceLoss(),
				1u,
				2u,
				TerraEngine::Renderer::RendererRecoveryAction::AutomaticRebuild);
			const TerraEngine::Renderer::RendererRecoverySnapshot completed = TerraEngine::Renderer::CompleteRecoveryRebuild(rebuilding, true, 12u);

			context.Expect(completed.state == TerraEngine::Renderer::RendererRecoveryStateKind::RebuildSucceeded, "Successful rebuild should publish RebuildSucceeded.");
			context.ExpectEqual(12u, completed.renderSessionGeneration, "Successful rebuild should publish the fresh render session generation.");
			context.Expect(TerraEngine::Renderer::CanSubmitNormalGpuWork(completed), "Normal GPU work may resume after a successful rebuild.");
		});
	}
}
