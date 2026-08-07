#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{
	namespace
	{
		// Stable identities make this scale test reproducible and prove that a
		// unique material is preserved by renderer grouping rather than masked by
		// random UUID generation.
		TerraEngine::UUID MakeDeterministicIdentity(
			const std::uint64_t role,
			const std::uint32_t ordinal)
		{
			TerraEngine::UUID id{};
			for(std::uint32_t byte = 0u; byte < 8u; ++byte)
			{
				id[byte] = static_cast<std::uint8_t>(role >> (byte * 8u));
			}
			for(std::uint32_t byte = 0u; byte < 4u; ++byte)
			{
				id[8u + byte] = static_cast<std::uint8_t>(ordinal >> (byte * 8u));
			}
			id[12u] = 0x54u;
			id[13u] = 0x45u;
			id[14u] = 0x52u;
			id[15u] = 0x41u;
			return id;
		}
	}

	void RegisterRendererScalePerformanceTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ScalePerformance.TypedCountsScaleWithoutWallClockClaims", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			const UUID mesh = GenerateUUID();
			const UUID material = GenerateUUID();
			for (std::uint32_t index = 0u; index < 4000u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = GenerateUUID();
				record.meshId = mesh;
				record.materialId = material;
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
				snapshot.renderables.push_back(record);
			}
			const auto topology = BuildPersistentRenderTopology(snapshot, 1u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 1u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 2u);
			context.Expect(dynamic.valid, "A large repeated-instance workload must build a complete typed sidecar.");
			context.ExpectEqual(static_cast<std::size_t>(4000), dynamic.instances.size(), "Typed instance count must preserve all repeated models.");
			context.Expect(!dynamic.groups.empty(), "Repeated models must produce grouped work without relying on timing thresholds.");
			context.ExpectEqual(static_cast<std::size_t>(4000), topology.records.size(), "Persistent topology must retain deterministic owner membership.");
		});

		registry.Add("Renderer.ScalePerformance.SmallAndNoPeerWorkRemainCheapFallbacks", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			for (std::uint32_t index = 0u; index < 10u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = GenerateUUID();
				record.meshId = GenerateUUID();
				record.materialId = GenerateUUID();
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
				snapshot.renderables.push_back(record);
			}
			const auto topology = BuildPersistentRenderTopology(snapshot, 2u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 480u), 1u, 1u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 3u);
			context.Expect(dynamic.valid, "No-peer workloads must remain valid through complete fallback preparation.");
			context.ExpectEqual(static_cast<std::size_t>(10), dynamic.fallbackCommands.size(), "Incompatible small scenes must preserve one complete command per model.");
		});

		registry.Add("Renderer.ScalePerformance.HeterogeneousStaticPopulationRemainsDeterministic", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			const UUID mesh = MakeDeterministicIdentity(0x4d455348u, 1u);
			for (std::uint32_t index = 0u; index < 4000u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = MakeDeterministicIdentity(0x4f424a45u, index + 1u);
				record.meshId = mesh;
				record.materialId = MakeDeterministicIdentity(0x4d41544cu, index + 1u);
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
				snapshot.renderables.push_back(record);
			}
			const auto topology = BuildPersistentRenderTopology(snapshot, 4u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 1u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 5u);
			context.Expect(dynamic.valid, "Heterogeneous static populations must remain valid without grouping.");
			context.ExpectEqual(static_cast<std::size_t>(4000), topology.groupTemplates.size(), "Every deterministic material identity must remain distinct in persistent topology.");
			context.Expect(dynamic.groups.empty(), "Heterogeneous static populations must not form synthetic groups.");
			context.ExpectEqual(static_cast<std::size_t>(4000), dynamic.fallbackCommands.size(), "Every heterogeneous static owner must retain one fallback command.");
			context.ExpectEqual(static_cast<std::size_t>(0), dynamic.instances.size(), "Static fallback work must not allocate synthetic animation instance records.");
		});

		registry.Add("Renderer.ScalePerformance.NoCompatiblePeerPopulationConservesFallbacks", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			const UUID sharedMesh = GenerateUUID();
			for (std::uint32_t index = 0u; index < 4000u; ++index)
			{
				RenderFrameRenderableRecord record;
				record.objectId = GenerateUUID();
				record.meshId = sharedMesh;
				record.materialId = GenerateUUID();
				record.invalidation = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u };
				snapshot.renderables.push_back(record);
			}
			const auto topology = BuildPersistentRenderTopology(snapshot, 6u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u, 1u };
			const auto dynamic = BuildDynamicRenderFrameState(snapshot, topology, view, 7u);
			context.Expect(dynamic.valid && dynamic.groups.empty(), "A population with no compatible peers must use complete fallback work.");
			context.ExpectEqual(static_cast<std::size_t>(4000), dynamic.fallbackCommands.size(), "No-peer fallback count must conserve the visible population.");
			context.ExpectEqual(static_cast<std::size_t>(0), dynamic.instances.size(), "No-peer static work must not fabricate animation records.");
		});
	}
}
