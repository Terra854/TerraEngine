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

		registry.Add("VirtualGeometry.Visibility.MicrotrianglesPreferComputeRaster",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryRasterCandidate microtrianglePacket{};
				microtrianglePacket.stableId = 1u;
				microtrianglePacket.projectedPixelArea = 48.0f;
				microtrianglePacket.triangleCount = 96u;
				const VirtualGeometryRasterBinPlan plan =
					BuildVirtualGeometryRasterBinPlan(
						{ microtrianglePacket }, 64u);
				context.Expect(
					plan.computeCount == 1u
						&& plan.hardwareCount == 0u
						&& plan.routes.front()
							== VirtualGeometryRasterRoute::Compute,
					"Subpixel triangles should avoid fixed-function 2x2 quad allocation.");
			});

		registry.Add("VirtualGeometry.Visibility.LargeOrUnboundedPacketsStayHardware",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryRasterCandidate ordinaryPacket{};
				ordinaryPacket.stableId = 1u;
				ordinaryPacket.projectedPixelArea = 4096.0f;
				ordinaryPacket.triangleCount = 32u;
				VirtualGeometryRasterCandidate unboundedPacket = ordinaryPacket;
				unboundedPacket.stableId = 2u;
				unboundedPacket.projectedPixelArea =
					kVirtualGeometryComputeMaximumPacketPixelArea + 1.0f;
				unboundedPacket.preferCompute = true;
				const VirtualGeometryRasterBinPlan plan =
					BuildVirtualGeometryRasterBinPlan(
						{ ordinaryPacket, unboundedPacket }, 64u);
				context.Expect(
					plan.hardwareCount == 2u
						&& plan.computeCount == 0u,
					"Large packet rectangles must remain on bounded hardware raster work even when compute is preferred.");
			});

		registry.Add("VirtualGeometry.Visibility.CalibrationAddsHysteresisAndContentionGuard",
			[](TestContext& context)
			{
				using namespace TerraEngine::Renderer;
				VirtualGeometryRasterCandidate boundary{};
				boundary.stableId = 7u;
				boundary.projectedPixelArea = 100.0f;
				boundary.triangleCount = 100u;
				const auto fresh = BuildVirtualGeometryRasterBinPlan({ boundary }, 64u);
				boundary.priorRoute = VirtualGeometryRasterRoute::Compute;
				boundary.priorRouteValid = true;
				const auto retained = BuildVirtualGeometryRasterBinPlan({ boundary }, 64u);
				context.Expect(
					fresh.hardwareCount == 1u && retained.computeCount == 1u
						&& fresh.routeSwitchCount == 0u
						&& retained.routeSwitchCount == 0u,
					"Asymmetric measured thresholds must retain a prior compute route without pulling a fresh boundary packet off hardware.");

				VirtualGeometryRasterCandidate contended{};
				contended.stableId = 8u;
				contended.projectedPixelArea = 10.0f;
				contended.triangleCount = 200u;
				const auto guarded = BuildVirtualGeometryRasterBinPlan({ contended }, 64u);
				context.Expect(
					guarded.hardwareCount == 1u
						&& guarded.peakEstimatedAtomicContention > 12.0f,
					"Dense overlap must remain on fixed-function raster when the portable atomic-contention estimate exceeds calibration.");
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
				context.Expect(shader.find("Raster.phase == 1u") != std::string::npos
					&& shader.find("InterlockedMin(") != std::string::npos
					&& shader.find("source.identity.x") != std::string::npos,
					"Mixed routes require deterministic stable-identity tie arbitration");
				context.Expect(shader.find("Raster.phase == 0u") != std::string::npos
					&& shader.find("visibility[pixel].x != source.identity.x") != std::string::npos,
					"Companion visibility fields must publish only after depth and stable ownership are final.");
			});
	}
} // namespace TerraEngine::Tests
