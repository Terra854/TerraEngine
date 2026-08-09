#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>

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
			context.ExpectEqual(static_cast<std::size_t>(10), dynamic.singletonOrdinals.size(), "Incompatible small scenes must preserve one stable ordinal per model.");
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
			context.ExpectEqual(static_cast<std::size_t>(4000), dynamic.singletonOrdinals.size(), "Every heterogeneous static owner must retain one stable ordinal.");
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
			context.ExpectEqual(static_cast<std::size_t>(4000), dynamic.singletonOrdinals.size(), "No-peer ordinal count must conserve the visible population.");
			context.ExpectEqual(static_cast<std::size_t>(0), dynamic.instances.size(), "No-peer static work must not fabricate animation records.");
		});

		registry.Add("Renderer.ScalePerformance.DirectStaticCrowdKeepsEveryPersistentSlotComplete", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			constexpr std::uint32_t kCrowdCount = 10000u;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 1u;
			snapshot.valid = true;
			const UUID mesh = GenerateUUID();
			for(std::uint32_t index = 0u; index < kCrowdCount; ++index)
			{
				RenderFrameRenderableRecord record{};
				record.objectId = GenerateUUID();
				record.meshId = mesh;
				record.materialId = GenerateUUID();
				record.geometryRouteDecision.selected = GeometryRoute::DirectMeshlet;
				// These are the four baseline lifetimes production snapshot assembly
				// assigns to static content before it attempts persistent publication.
				record.invalidation.asset = index + 1u;
				record.invalidation.material = index + 1u;
				record.invalidation.renderer = 1u;
				record.invalidation.residency = 1u;
				snapshot.renderables.push_back(record);
			}
			const PersistentRenderTopology topology =
				BuildPersistentRenderTopology(snapshot, 1u);
			const std::size_t completeSlots = static_cast<std::size_t>(std::count_if(
				topology.records.begin(), topology.records.end(),
				[](const PersistentRenderTopologyRecord& record) {
					return record.groupKey.IsComplete();
				}));
			context.Expect(topology.valid && completeSlots == kCrowdCount,
				"A 10k direct-only static crowd with baseline resource lifetimes must retain every persistent slot instead of publishing an empty draw selection.");
		});

		registry.Add("Renderer.ScalePerformance.StaticReuseTenThousandConservesPreparedWork", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			constexpr std::uint32_t kPopulation = 10000u;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 17u;
			snapshot.valid = true;
			const UUID mesh = MakeDeterministicIdentity(0x4d455348u, 17u);
			const UUID material = MakeDeterministicIdentity(0x4d41544cu, 17u);
			for(std::uint32_t ordinal = 0u; ordinal < kPopulation; ++ordinal) {
				RenderFrameRenderableRecord record{};
				record.objectId = MakeDeterministicIdentity(0x4f424a45u, ordinal);
				record.meshId = mesh;
				record.materialId = material;
				record.geometryRouteDecision.selected = GeometryRoute::DirectMeshlet;
				record.invalidation = { 5u, 5u, 5u, 5u, 5u, 5u, 5u, 5u };
				snapshot.renderables.push_back(record);
			}
			const PersistentRenderTopology first = BuildPersistentRenderTopology(snapshot, 5u);
			const PersistentRenderTopology reused = BuildPersistentRenderTopology(snapshot, 5u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game,
				glm::uvec2(1280u, 720u), 1u, 5u };
			const DynamicRenderFrameState dynamic = BuildDynamicRenderFrameState(
				snapshot, reused, view, 18u);
			context.Expect(first.valid && reused.valid && first.records.size() == kPopulation
				&& reused.records.size() == kPopulation && first.groupTemplates.size() == 1u
				&& reused.groupTemplates.size() == 1u,
				"A deterministic 10k static workload must retain one reusable topology template across publications.");
			context.Expect(dynamic.valid && dynamic.instances.size() == kPopulation
				&& dynamic.groups.size() == 1u && dynamic.singletonOrdinals.empty(),
				"Static reuse must conserve all 10k instances in one grouped prepared lane without fallback records.");
		});

		registry.Add("Renderer.ScalePerformance.MixedDirectDensePopulationConservesCpuPreparation", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			constexpr std::uint32_t kPopulation = 4000u;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 23u;
			snapshot.valid = true;
			const UUID mesh = MakeDeterministicIdentity(0x4d455348u, 23u);
			const UUID material = MakeDeterministicIdentity(0x4d41544cu, 23u);
			for(std::uint32_t ordinal = 0u; ordinal < kPopulation; ++ordinal) {
				RenderFrameRenderableRecord record{};
				record.objectId = MakeDeterministicIdentity(0x4f424a45u, ordinal + 1u);
				record.meshId = mesh;
				record.materialId = material;
				record.geometryRouteDecision.selected = ordinal < 2000u
					? GeometryRoute::DirectMeshlet : GeometryRoute::VirtualGeometryCompute;
				record.invalidation = { 9u, 9u, 9u, 9u, 9u, 9u, 9u, 9u };
				snapshot.renderables.push_back(record);
			}
			const PersistentRenderTopology topology = BuildPersistentRenderTopology(snapshot, 9u);
			const RenderFrameViewRequest view{ RenderOutputKind::GameView, RenderViewType::Game,
				glm::uvec2(1280u, 720u), 2u, 9u };
			const DynamicRenderFrameState dynamic = BuildDynamicRenderFrameState(
				snapshot, topology, view, 10u);
			context.Expect(dynamic.valid && dynamic.instances.size() == kPopulation,
				"Mixed direct/dense-VG preparation must retain every CPU instance record.");
			context.Expect(dynamic.groups.size() == 2u && dynamic.singletonOrdinals.empty(),
				"The two route variants must form exactly two grouped submissions with no dropped fallback ordinals.");
		});
	}
}
