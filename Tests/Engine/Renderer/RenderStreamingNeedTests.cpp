#include "TestHarness.h"

#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
	void RegisterRenderStreamingNeedTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Streaming.VisibleNeedMapsToActiveViewProtection", [](TestContext& context) {
			context.Expect(Resource::ProtectionForNeed(Resource::StreamingNeedType::Visible) == Resource::StreamingProtectionReason::ActiveView, "Visible renderer needs should protect resources as active-view content.");
		});

		registry.Add("Renderer.Streaming.FailedLoadsStayDiagnostic", [](TestContext& context) {
			context.Expect(Resource::StateAfterLoadResult(false, true) == Resource::StreamablePayloadState::Failed, "Renderer fallback paths need failed loads to stay separate from deferred loads.");
		});
	}
}
