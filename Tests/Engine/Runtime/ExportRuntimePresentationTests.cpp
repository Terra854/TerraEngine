#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanPresentationLifecycle.h"

namespace TerraEngine::Tests
{
	void RegisterExportRuntimePresentationTests(TestRegistry& registry)
	{
		registry.Add("ExportRuntime.Presentation.StickyCandidatePublication", [](TestContext& context) {
			using namespace TerraEngine::Renderer::Vulkan;
			PresentationLifecycle lifecycle;
			lifecycle.Configure(3, 2, 2, 3);
			const auto originalGeneration = lifecycle.Generation();
			lifecycle.RequestRecreation();
			lifecycle.PublishRecreation(PresentationRecreationOutcome::RecoverableFailure, 2, 2, 4);
			context.Expect(lifecycle.State() == PresentationLifecycleState::RecreationRequested, "Failed candidates must leave recreation sticky.");
			context.ExpectEqual(originalGeneration, lifecycle.Generation(), "Failed candidates must not publish a generation.");
			lifecycle.PublishRecreation(PresentationRecreationOutcome::Success, 2, 2, 4);
			context.Expect(lifecycle.Generation() > originalGeneration, "Only complete candidates publish a generation.");
		});
	}
}
