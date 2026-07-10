#include "TestHarness.h"
#include "Renderer/RendererRecoveryEditorTestFixtures.h"

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryWindowTests(TestRegistry& registry)
	{
		registry.Add("Editor.RendererRecovery.WindowsCanShowUnavailableState", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeEditorRendererRecoverySnapshot();

			context.Expect(snapshot.state == TerraEngine::Renderer::RendererRecoveryStateKind::DeviceLost, "Recovery windows should receive the unavailable renderer state.");
			context.Expect(!snapshot.message.empty(), "Recovery windows should have a user-facing status message.");
		});
	}
}
