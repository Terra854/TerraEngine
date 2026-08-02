// AnimatedVirtualGeometryAbiTests.cpp - byte and planning contract coverage.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
			demand.key.page.codecProfile = 1u;
			demand.key.page.codecVersion = 1u;
			demand.key.deformation = {
				100u, 101u, 102u, 103u, 104u, 10u, 105u, 106u
			};
			demand.key.inputLayoutVersion = kAnimatedGeometryAbiVersion;
			demand.key.animationLodVersion = kAnimatedGeometryAbiVersion;
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
		registry.Add("AnimatedVirtualGeometry.Abi.CanonicalDemandedPageLayoutsAreStable", [](TestContext& context)
		{
			context.ExpectEqual<std::size_t>(40u,
				sizeof(TemfDeformableDemandedPageDirectory),
				"Canonical demanded-page directories must remain byte exact.");
			context.ExpectEqual<std::size_t>(80u,
				sizeof(TemfDeformableDemandedPageEntry),
				"Canonical demanded-page entries must remain byte exact.");
			context.ExpectEqual<std::size_t>(56u,
				sizeof(TemfDeformableDemandedPagePayload),
				"Canonical demanded-page payload identities must remain byte exact.");
			context.ExpectEqual<std::size_t>(24u,
				sizeof(TemfDeformablePageSection),
				"Canonical demanded-page section records must remain byte exact.");
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

		registry.Add("AnimatedVirtualGeometry.Abi.ComputeRasterConsumesDemandedPageOutput", [](TestContext& context)
		{
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Shaders/VirtualGeometry/VirtualGeometryTriangleSetup.comp.hlsl";
			std::ifstream stream(shaderPath, std::ios::binary);
			const std::string shader{
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>() };
			context.Expect(
				shader.find("AnimatedDeformedGeometry.hlsli") != std::string::npos
					&& shader.find("TeTryResolveAnimatedGraphicsPage") != std::string::npos
					&& shader.find("TeTryLoadAnimatedVertex") != std::string::npos,
				"Compute raster setup must consume the same deduplicated demanded-page deformation bridge as mesh shading, not rebuild animation per view.");
		});

		registry.Add("AnimatedVirtualGeometry.Abi.PageSkinningKeepsPaletteLocalJointIndices", [](TestContext& context)
		{
			const std::filesystem::path shaderPath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Engine/Shaders/VirtualGeometry/AnimatedPageDeform.comp.hlsl";
			std::ifstream stream(shaderPath, std::ios::binary);
			const std::string shader{
				std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>() };
			context.Expect(
				shader.find("const uint paletteEntry = localJoint;") != std::string::npos
					&& shader.find("staticPageBytes.Load(work.jointPaletteByteOffset + localJoint * 4u)")
						== std::string::npos,
				"Demanded-page skin records already contain palette-local indices; the shader must not reinterpret source-skeleton joint identities as compact palette rows.");
		});
	}

}
