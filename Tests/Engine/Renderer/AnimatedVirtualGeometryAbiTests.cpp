// AnimatedVirtualGeometryAbiTests.cpp - byte and planning contract coverage.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"

#include <array>
#include <cstdint>

namespace TerraEngine::Tests {

	namespace {

		TerraEngine::UUID MakeAnimatedGeometryUuid(std::uint8_t seed)
		{
			TerraEngine::UUID id{};
			id.back() = seed;
			return id;
		}

		TerraEngine::Renderer::DeformationPageDemand MakeDemand(
			std::uint32_t pageId,
			std::uint64_t viewMask)
		{
			using namespace TerraEngine::Renderer;

			DeformationPageDemand demand;
			demand.key.page.meshId = MakeAnimatedGeometryUuid(1u);
			demand.key.page.meshPayloadGeneration = 10u;
			demand.key.page.pageId = pageId;
			demand.key.page.pageContentGeneration = 20u + pageId;
			demand.key.page.residencyGeneration = 30u;
			demand.key.page.layoutVersion = kAnimatedGeometryAbiVersion;
			demand.key.deformation = {
				100u, 101u, 102u, 103u, 104u, 10u, 105u, 106u
			};
			demand.key.inputLayoutVersion = kAnimatedGeometryAbiVersion;
			demand.sourceRecordIndex = 4u;
			demand.vertexCount = 64u;
			demand.meshletCount = 2u;
			demand.clusterCount = 1u;
			demand.requestedViewMask = viewMask;
			demand.priorHistoryCompatible = true;
			return demand;
		}

	}

	void RegisterAnimatedVirtualGeometryAbiTests(TestRegistry& registry)
	{
		registry.Add("AnimatedVirtualGeometry.Abi.PhysicalPageLayoutsAreFrozen", [](TestContext& context)
		{
			context.ExpectEqual<std::size_t>(52u, sizeof(AnimatedVirtualPageDirectoryHeader),
				"AVPD headers must remain byte exact.");
			context.ExpectEqual<std::size_t>(100u, sizeof(AnimatedVirtualPageDirectoryEntry),
				"AVPD entries must remain byte exact.");
			context.ExpectEqual<std::size_t>(64u, sizeof(AnimatedVirtualPageBlobHeader),
				"AVPB headers must remain byte exact.");
			context.ExpectEqual<std::size_t>(32u, sizeof(AnimatedVirtualPageSectionRecord),
				"AVPB section records must remain byte exact.");
			context.ExpectEqual<std::size_t>(64u,
				sizeof(TerraEngine::Renderer::GpuDeformedVertexRecord),
				"Graphics-facing animated vertices must remain 64 bytes.");
		});

		registry.Add("AnimatedVirtualGeometry.Abi.DeduplicatesCompatibleViewDemands", [](TestContext& context)
		{
			const std::array demands = { MakeDemand(7u, 1u), MakeDemand(7u, 4u) };
			const auto plan = TerraEngine::Renderer::BuildGpuDeformationPlan(9u, 105u, demands);

			context.Expect(plan.IsComplete(), "Compatible view demands should produce a complete plan.");
			context.ExpectEqual<std::size_t>(1u, plan.work.size(),
				"One instance/page/generation must dispatch exactly once.");
			context.ExpectEqual<std::uint32_t>(1u, plan.reusedDemandCount,
				"The duplicate view demand should be counted as reuse.");
			context.ExpectEqual<std::uint64_t>(5u, plan.work.front().requestedViewMask,
				"Deduplication must preserve every consuming view.");
		});

		registry.Add("AnimatedVirtualGeometry.Abi.RejectsConflictsAtomically", [](TestContext& context)
		{
			auto first = MakeDemand(7u, 1u);
			auto conflicting = first;
			conflicting.vertexCount = 65u;
			const std::array demands = { first, conflicting };
			const auto plan = TerraEngine::Renderer::BuildGpuDeformationPlan(9u, 105u, demands);

			context.Expect(!plan.valid && plan.work.empty(),
				"Conflicting shapes for one exact key must reject the whole plan.");
			context.ExpectEqual<std::uint32_t>(0u, plan.totalVertexCount,
				"Rejected plans must not expose partial prefix sums.");
		});

		registry.Add("AnimatedVirtualGeometry.Abi.PreservesHighGenerationBits", [](TestContext& context)
		{
			auto first = MakeDemand(7u, 1u);
			auto second = first;
			second.key.page.pageContentGeneration += (std::uint64_t{1u} << 32u);
			second.requestedViewMask = 2u;
			const std::array demands = { first, second };
			const auto plan = TerraEngine::Renderer::BuildGpuDeformationPlan(9u, 105u, demands);

			context.Expect(plan.IsComplete(), "Distinct full-width generations remain valid.");
			context.ExpectEqual<std::size_t>(2u, plan.work.size(),
				"High-bit generation differences must never alias one cache entry.");
		});
	}

}
