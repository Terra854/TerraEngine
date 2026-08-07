#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{
	void RegisterSharedRenderSubmissionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.SharedSubmission.ExactKeyGroupsOnlyCompatibleInstances", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			for (std::uint32_t index = 0u; index < 3u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = GenerateUUID();
				record.meshId = GenerateUUID();
				record.materialId = GenerateUUID();
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 4u, 1u };
				snapshot.renderables.push_back(record);
			}
			// Two records intentionally share stable mesh/material identity; the third
			// differs and must remain a singleton fallback.
			snapshot.renderables[1].meshId = snapshot.renderables[0].meshId;
			snapshot.renderables[1].materialId = snapshot.renderables[0].materialId;
			snapshot.renderables[2].usingFallback = true;
			const auto topology = BuildPersistentRenderTopology(snapshot, 5u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 6u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 8u);
			context.Expect(dynamic.valid, "Shared submission preparation must publish a complete dynamic state.");
			context.Expect(!dynamic.groups.empty(), "Compatible repeated instances must form an executable group.");
			context.Expect(!dynamic.fallbackCommands.empty(), "An isolated fallback member must retain its complete command.");
			context.ExpectEqual(2u, dynamic.groups.front().instanceCount, "Grouped instance count must exclude the incompatible member.");
			context.Expect(snapshot.renderables[2].objectId == dynamic.fallbackCommands.front().objectId, "Fallback command must retain the isolated object's identity.");
		});
	}
}
