#include "TestHarness.h"

#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryShadowTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Render.Shadow.UsesConservativeLodWithoutHiZ", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 128u, 0.02f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 2u;
			quality.hiZOcclusionEnabled = true;
			quality.hiZSelfOcclusionGuardEnabled = true;

			const TerraEngine::Renderer::VirtualGeometryDrawRange cameraRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 16.0f, true, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange shadowRange =
				RenderSystem::SelectVirtualGeometryShadowLodRange(asset, quality, true);

			context.Expect(cameraRange.selectedLodIndex > shadowRange.selectedLodIndex, "The camera view may choose lower detail at distance.");
			context.ExpectEqual(0u, shadowRange.selectedLodIndex, "Shadow visibility should keep LOD0 for conservative silhouettes.");
			context.ExpectEqual(16u, shadowRange.meshletCount, "Shadow visibility should dispatch the highest-detail meshlet range.");
			context.Expect(shadowRange.virtualGeometryActive, "Shadow virtual geometry should stay active when the quality profile enables it.");
			context.Expect(shadowRange.hiZSelfOcclusionGuardPassed, "Shadow views should keep Hi-Z guard metadata diagnostic-only instead of clearing it.");
			context.Expect(shadowRange.fallbackReason.find("shadow pass") != std::string::npos, "Shadow selection should explain its conservative policy.");
		});

		registry.Add("VirtualGeometry.Render.Shadow.FailsOpenForInvalidBounds", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 8u, 256u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();

			const TerraEngine::Renderer::VirtualGeometryDrawRange shadowRange =
				RenderSystem::SelectVirtualGeometryShadowLodRange(asset, quality, false);

			context.Expect(shadowRange.failOpen, "Invalid shadow meshlet bounds should fail open.");
			context.ExpectEqual(0u, shadowRange.selectedLodIndex, "Invalid shadow bounds should preserve highest-detail visibility.");
			context.Expect(shadowRange.hiZSelfOcclusionGuardPassed, "Fail-open shadow visibility should not clear camera Hi-Z diagnostics.");
		});
	}
}
