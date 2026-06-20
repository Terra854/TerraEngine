#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Renderer/Graph/RenderWorkPacket.h"

namespace TerraEngine::Tests {

	void RegisterRenderPreparationCacheTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PreparationCache.StableDrawSignaturesMatch", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(4u, 1u);
			const auto first = TerraEngine::Renderer::BuildPreparedDrawPacketSignature(snapshot.renderables.front());
			const auto second = TerraEngine::Renderer::BuildPreparedDrawPacketSignature(snapshot.renderables.front());
			context.ExpectEqual(TerraEngine::Renderer::HashPreparedDrawPacketSignature(first), TerraEngine::Renderer::HashPreparedDrawPacketSignature(second), "Stable draw packet signatures should be reused across unchanged frames.");
		});

		registry.Add("Renderer.PreparationCache.ChangedMaterialInvalidatesSignature", [](TestContext& context) {
			auto snapshot = MakeRendererThreadingSnapshot(5u, 1u);
			const auto first = TerraEngine::Renderer::HashPreparedDrawPacketSignature(TerraEngine::Renderer::BuildPreparedDrawPacketSignature(snapshot.renderables.front()));
			snapshot.renderables.front().materialId = TerraEngine::GenerateUUID();
			const auto second = TerraEngine::Renderer::HashPreparedDrawPacketSignature(TerraEngine::Renderer::BuildPreparedDrawPacketSignature(snapshot.renderables.front()));
			context.Expect(first != second, "Changing material identity should invalidate cached draw preparation.");
		});
	}

}
