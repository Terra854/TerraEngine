#include "TestHarness.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryHybridRasterTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Visibility.HybridUsesThreeRealRoutesDeterministically",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				const std::vector<VirtualGeometryRasterCandidate> firstOrder{
					{30u, 0u, true, true},
					{10u, 0u, true, true},
					{20u, 0u, true, true}};
				const std::vector<VirtualGeometryRasterCandidate> secondOrder{
					firstOrder[1],
					firstOrder[2],
					firstOrder[0]};
				const VirtualGeometryRasterBinPlan first =
					BuildVirtualGeometryHybridRasterBinPlan(firstOrder, 64u);
				const VirtualGeometryRasterBinPlan second =
					BuildVirtualGeometryHybridRasterBinPlan(secondOrder, 64u);
				context.Expect(
					first.routes == second.routes
						&& first.routeStableIds == second.routeStableIds,
					"Hybrid routing must depend on stable packet identity, not worker completion order.");
				context.Expect(
					first.hybridCoverageComplete
						&& first.hardwareCount >= 1u
						&& first.computeCount >= 1u
						&& first.indexedCount >= 1u,
					"Three compatible packets must cover the real Hardware, Compute, and Indexed routes.");
				context.Expect(
					std::find(
						first.routes.begin(),
						first.routes.end(),
						VirtualGeometryRasterRoute::Fallback)
						== first.routes.end(),
					"Hybrid validation must not invent or emit a fourth backend route.");
			});

		registry.Add("VirtualGeometry.Visibility.HybridMatchesDistinctSupportedCandidates",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				const VirtualGeometryRasterBinPlan plan =
					BuildVirtualGeometryHybridRasterBinPlan({
						{1u, 0u, true, true},
						{2u, 0u, true, false},
						{3u, 0u, false, false}},
						64u);
				context.Expect(
					plan.hybridCoverageComplete
						&& plan.routes.size() == 3u
						&& plan.routes[0] == VirtualGeometryRasterRoute::Compute
						&& plan.routes[1] == VirtualGeometryRasterRoute::Hardware
						&& plan.routes[2] == VirtualGeometryRasterRoute::Indexed,
					"Hybrid matching must reserve distinct supported packets instead of greedily losing a route.");
			});

		registry.Add("VirtualGeometry.Visibility.HybridFailsOpenToSupportedAndIndexedRoutes",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				const VirtualGeometryRasterBinPlan plan =
					BuildVirtualGeometryHybridRasterBinPlan({
						{1u, 0u, false, true},
						{2u, 0u, false, true},
						{3u, 99u, true, true}},
						64u);
				context.Expect(
					plan.failOpen
						&& !plan.hybridCoverageComplete
						&& plan.hardwareCount == 0u
						&& plan.computeCount == 1u
						&& plan.indexedCount == 2u
						&& plan.overflowCount == 1u,
					"Unavailable hardware and overflowing material bins must retain supported compute work and indexed coverage.");
				context.Expect(
					std::find(
						plan.routes.begin(),
						plan.routes.end(),
						VirtualGeometryRasterRoute::Fallback)
						== plan.routes.end(),
					"Fail-open hybrid routing must remain visible through Indexed, never a synthetic fallback route.");
			});

		registry.Add("VirtualGeometry.Visibility.MixedRouteCrackAndImageParityContract",
			[](TestContext &context)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.hlsl";
				std::ifstream file(path);
				const std::string shader{ std::istreambuf_iterator<char>(file), {} };
				// Equal-depth hardware/compute overlap is resolved by the smallest stable
				// primitive identity. Check the executable atomic contract instead of a
				// cosmetic variable name so this test follows the shader's actual behavior.
				context.Expect(shader.find("depthKey == previousDepth") != std::string::npos
					&& shader.find("InterlockedMin(visibility[pixel].x, source.stablePrimitiveId") != std::string::npos,
					"Mixed routes require deterministic stable-identity tie arbitration");
				context.Expect(shader.find("failOpen") != std::string::npos, "Uncertain mixed-route coverage must fail open");
			});
	}
} // namespace TerraEngine::Tests
