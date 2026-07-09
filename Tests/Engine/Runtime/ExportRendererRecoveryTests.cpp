#include "TestHarness.h"
#include "Renderer/RendererRecoveryPolicy.h"
#include "Renderer/RendererRecoveryTestFixtures.h"

namespace TerraEngine::Tests
{
	void RegisterExportRendererRecoveryTests(TestRegistry& registry)
	{
		registry.Add("RendererRecovery.Export.PolicyFailsClearlyAfterBudget", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoveryPolicy policy =
				TerraEngine::Renderer::MakeExportRendererRecoveryPolicy(true, true, 1u, "Export renderer recovery failed.");
			const TerraEngine::Renderer::RendererRecoveryPolicyDecision decision =
				TerraEngine::Renderer::EvaluateRendererRecoveryPolicy(MakeLatchedRendererDeviceLoss(), policy, 1u);

			context.Expect(policy.fatalOnExhaustedAttempts, "Export recovery policy should fail fatally once bounded attempts are exhausted.");
			context.Expect(!decision.canAttempt, "A completed one-attempt export policy should reject another rebuild attempt.");
			context.Expect(decision.budgetExhausted, "The rejected export rebuild should report exhausted budget.");
		});

		registry.Add("RendererRecovery.Export.FatalDialogReportAttachesDumpMetadata", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot failed = TerraEngine::Renderer::CompleteRecoveryRebuild(
				TerraEngine::Renderer::BeginRecoveryRebuild(
					MakeLatchedRendererDeviceLoss(),
					1u,
					1u,
					TerraEngine::Renderer::RendererRecoveryAction::ExportPolicyRebuild),
				false,
				1u,
				"Export renderer rebuild failed.");
			const TerraEngine::Renderer::RendererRecoverySnapshot fatal = TerraEngine::Renderer::AttachFatalDialogReport(
				failed,
				"fatal-export-graphics",
				"C:/Temp/TerraEngineCrash.dmp",
				"C:/Temp/TerraEngineCrash.txt",
				"Written");

			context.Expect(fatal.selectedAction == TerraEngine::Renderer::RendererRecoveryAction::FatalDialog, "Export fatal recovery should record that a system dialog is required.");
			context.ExpectEqual(std::string("fatal-export-graphics"), fatal.fatalDialogReportId, "Export fatal recovery should preserve the dialog report id.");
			context.ExpectEqual(std::string("Written"), fatal.crashDumpStatus, "Export fatal recovery should publish the crash dump status for logs and automation.");
		});

		registry.Add("RendererRecovery.Export.RebuildFailureRecordsRealFatalState", [](TestContext& context) {
			// Export tests model the recovery result directly; production builds
			// must not keep a callable hook that can manufacture device loss.
			const TerraEngine::Renderer::RendererRecoverySnapshot failed = TerraEngine::Renderer::CompleteRecoveryRebuild(
				TerraEngine::Renderer::BeginRecoveryRebuild(
					MakeLatchedRendererDeviceLoss(),
					1u,
					1u,
					TerraEngine::Renderer::RendererRecoveryAction::ExportPolicyRebuild),
				false,
				1u,
				"Vulkan renderer rebuild could not create a fresh session.");

			context.Expect(failed.state == TerraEngine::Renderer::RendererRecoveryStateKind::RendererRecoveryFailed, "Export rebuild failure should exhaust the bounded recovery budget.");
			context.Expect(failed.finalOutcome == TerraEngine::Renderer::RendererRecoveryOutcome::RecoveryFailed, "Export rebuild failure should publish the fatal recovery outcome.");
			context.Expect(!failed.lastError.empty(), "Export rebuild failure should preserve the real backend error for logs and dialogs.");
		});
	}
}
