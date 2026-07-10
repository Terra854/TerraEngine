#include "TestHarness.h"
#include "Renderer/RendererRecovery.h"
#include "Renderer/RendererRecoveryPolicy.h"
#include "Renderer/RendererRecoveryTestFixtures.h"

namespace TerraEngine::Tests
{
	void RegisterRendererDeviceLossLatchTests(TestRegistry& registry)
	{
		registry.Add("RendererRecovery.DeviceLoss.LatchesFirstIncident", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeLatchedRendererDeviceLoss();

			context.Expect(snapshot.state == TerraEngine::Renderer::RendererRecoveryStateKind::DeviceLost, "A device-loss failure should move the renderer into DeviceLost.");
			context.Expect(snapshot.firstFailureStage == TerraEngine::Renderer::RendererDeviceFailureStage::Submit, "The first failure stage should classify the submit site.");
			context.Expect(!snapshot.incidentId.empty(), "The first device-loss failure should create an incident id.");
			context.Expect(!snapshot.canSaveProjectData, "Terminal device loss should not advertise an unreachable project-save action.");
			context.Expect(!TerraEngine::Renderer::CanSubmitNormalGpuWork(snapshot), "Normal GPU work should be blocked after a latched incident.");
		});

		registry.Add("RendererRecovery.DeviceLoss.EditorPolicyClosesWithoutSaveInstruction", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoveryPolicy policy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();

			context.Expect(policy.fatalMessage.find("Save") == std::string::npos, "The editor fatal message should not tell users to perform an unavailable save action.");
			context.Expect(policy.fatalMessage.find("close") != std::string::npos, "The editor fatal message should state that acknowledgement closes the editor.");
		});

		registry.Add("RendererRecovery.DeviceLoss.SuppressDuplicateFailures", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot first = MakeLatchedRendererDeviceLoss();
			const TerraEngine::Renderer::RendererRecoverySnapshot duplicate = TerraEngine::Renderer::RecordDeviceLossIncident(
				first,
				"VK_ERROR_DEVICE_LOST",
				"present",
				43u,
				first.renderSessionGeneration,
				TerraEngine::Renderer::RendererRecoveryAffectedView::GameView);

			context.ExpectEqual(first.incidentId, duplicate.incidentId, "Duplicate failures should stay grouped under the first incident.");
			context.Expect(duplicate.firstFailureStage == TerraEngine::Renderer::RendererDeviceFailureStage::Submit, "Duplicate failures must not overwrite the first failure stage.");
			context.ExpectEqual(1u, duplicate.duplicateFailureCount, "Duplicate failures should increment the suppressed failure count.");
		});
	}
}
