#include "TestHarness.h"
#include "Renderer/RendererRecoveryEditorTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryEditorTests(TestRegistry& registry)
	{
		registry.Add("Editor.RendererRecovery.ProjectOpenSnapshotRemainsCpuReadable", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeEditorRendererRecoverySnapshot();

			context.Expect(snapshot.canSaveProjectData, "Editor recovery snapshots should advertise safe project-data saving.");
			context.Expect(snapshot.affectedView == TerraEngine::Renderer::RendererRecoveryAffectedView::SceneView, "Editor fixture should identify the affected view.");
			context.Expect(!TerraEngine::Renderer::CanSubmitNormalGpuWork(snapshot), "Editor recovery snapshots should block normal GPU work until rebuild.");
		});

		registry.Add("Editor.RendererRecovery.RestoredOutputAllowsGpuSubmission", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot rebuilding = TerraEngine::Renderer::BeginRecoveryRebuild(
				MakeEditorRendererRecoverySnapshot(),
				1u,
				2u,
				TerraEngine::Renderer::RendererRecoveryAction::AutomaticRebuild);
			const TerraEngine::Renderer::RendererRecoverySnapshot restored = TerraEngine::Renderer::CompleteRecoveryRebuild(rebuilding, true, 24u);

			context.Expect(restored.state == TerraEngine::Renderer::RendererRecoveryStateKind::RebuildSucceeded, "Editor recovery smoke tests should model a restored renderer session.");
			context.Expect(TerraEngine::Renderer::CanSubmitNormalGpuWork(restored), "Restored editor output should allow normal GPU submissions again.");
			context.ExpectEqual(24u, restored.renderSessionGeneration, "Restored editor output should publish the fresh renderer generation.");
		});

		registry.Add("Editor.RendererRecovery.ProjectOpenFailuresUseRealRecoverySnapshots", [](TestContext& context) {
			// Project-open coverage records the same CPU recovery state a real
			// Vulkan failure would publish, without exposing any runtime hook
			// that editor or export builds could accidentally call.
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = TerraEngine::Renderer::RecordDeviceLossIncident(
				TerraEngine::Renderer::MakeActiveRecoverySnapshot(),
				"VK_ERROR_DEVICE_LOST",
				"editor swapchain image acquire",
				7u,
				3u,
				TerraEngine::Renderer::RendererRecoveryAffectedView::SceneView);

			context.Expect(snapshot.state == TerraEngine::Renderer::RendererRecoveryStateKind::DeviceLost, "Editor project-open failures should latch recovery state from a real failure record.");
			context.Expect(snapshot.affectedView == TerraEngine::Renderer::RendererRecoveryAffectedView::SceneView, "Editor project-open failures should preserve the affected view for diagnostics.");
			context.Expect(!TerraEngine::Renderer::CanSubmitNormalGpuWork(snapshot), "Project-open recovery snapshots should stop normal GPU submissions until rebuild.");
		});
	}
}
