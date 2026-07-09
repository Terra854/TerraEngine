#include "TestHarness.h"
#include "Renderer/RendererRecovery.h"

#include <utility>

namespace TerraEngine::Tests
{
	void RegisterRendererRecoveryResourceRestorationTests(TestRegistry& registry)
	{
		registry.Add("RendererRecovery.ResourceRestoration.PlanStartsCpuOnly", [](TestContext& context) {
			TerraEngine::Renderer::ResourceRestorationPlanInput input;
			input.renderSessionGeneration = 3u;
			input.requiredShaders.push_back("Shaders/Renderer/Mesh.vert.spv");
			input.visibleMeshes.push_back("camera_uv");
			input.textureResidency.push_back("camera_albedo");
			const TerraEngine::Renderer::ResourceRestorationPlan plan = TerraEngine::Renderer::BuildResourceRestorationPlan(std::move(input));

			context.ExpectEqual(3u, plan.renderSessionGeneration, "Restoration plans should target a specific render session generation.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.requiredShaders.size(), "Restoration plans should name required shaders as CPU-side metadata.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.visibleMeshes.size(), "Restoration plans should retain visible mesh identities before GPU recreation.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.textureResidency.size(), "Restoration plans should retain visible texture residency intent before GPU recreation.");
		});
	}
}
