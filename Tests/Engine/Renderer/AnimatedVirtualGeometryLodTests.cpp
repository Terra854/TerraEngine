#include "TestHarness.h"

#include "Engine/Renderer/Deformation/GpuDeformationPlan.h"
#include "Engine/Renderer/Graph/RenderDiagnostics.h"
#include "Engine/Renderer/Graph/RenderFrameSnapshot.h"

namespace TerraEngine::Tests
{

	namespace
	{

		Renderer::DeformationPageDemand MakeLodDemand(std::uint32_t animationLod, std::uint64_t viewMask)
		{
			Renderer::DeformationPageDemand demand;
			demand.key.page.meshId = UUID{ 1u, 2u };
			demand.key.page.meshPayloadGeneration = 3u;
			demand.key.page.pageId = 4u;
			demand.key.page.pageContentGeneration = 5u;
			demand.key.page.residencyGeneration = 6u;
			demand.key.page.layoutVersion = Renderer::kAnimatedGeometryAbiVersion;
			demand.key.page.codecProfile = 1u;
			demand.key.page.codecVersion = 1u;
			demand.key.deformation = { 7u, 8u, 9u, 10u, 11u, 3u, 12u, 13u };
			demand.key.inputLayoutVersion = Renderer::kAnimatedGeometryAbiVersion;
			demand.key.animationLod = animationLod;
			demand.key.animationLodVersion = Renderer::kAnimatedGeometryAbiVersion;
			demand.sourceRecordIndex = 14u;
			demand.vertexCount = 64u;
			demand.meshletCount = 2u;
			demand.clusterCount = 1u;
			demand.requestedViewMask = viewMask;
			demand.priorHistoryCompatible = true;
			return demand;
		}

	} // namespace

	void RegisterAnimatedVirtualGeometryLodTests(TestRegistry &registry)
	{
		registry.Add("AnimatedVirtualGeometry.Lod.CompatibleViewsShareExactLod",
			[](TestContext &context)
			{
				const std::array demands{ MakeLodDemand(2u, 1u), MakeLodDemand(2u, 2u) };
				const Renderer::GpuDeformationPlan plan = Renderer::BuildGpuDeformationPlan(1u, 12u, demands);
				context.Expect(plan.IsComplete(), "Exact codec/profile/LOD identities must produce a complete plan.");
				context.Expect(plan.work.size() == 1u && plan.work.front().requestedViewMask == 3u && plan.reusedDemandCount == 1u,
					"Compatible view demands must merge into one deformation dispatch.");
			});

		registry.Add("AnimatedVirtualGeometry.Lod.DifferentLodsNeverAlias",
			[](TestContext &context)
			{
				const std::array demands{ MakeLodDemand(1u, 1u), MakeLodDemand(2u, 2u) };
				const Renderer::GpuDeformationPlan plan = Renderer::BuildGpuDeformationPlan(1u, 12u, demands);
				context.Expect(plan.IsComplete() && plan.work.size() == 2u, "Independent animation LODs must retain separate work/publication keys.");
			});

		registry.Add("AnimatedVirtualGeometry.Lod.UnknownCodecFailsClosed",
			[](TestContext &context)
			{
				Renderer::DeformationPageDemand demand = MakeLodDemand(0u, 1u);
				demand.key.page.codecVersion = 0u;
				const std::array demands{ demand };
				const Renderer::GpuDeformationPlan plan = Renderer::BuildGpuDeformationPlan(1u, 12u, demands);
				context.Expect(!plan.valid && plan.work.empty(), "Unknown compact stream versions must not reach device deformation.");
			});

		registry.Add("AnimatedVirtualGeometry.Lod.DiagnosticsDescribePlannedBoundsWithoutGuessingExecution",
			[](TestContext &context)
			{
				Renderer::RenderFrameSnapshot snapshot;
				snapshot.frameId = 1u;
				snapshot.viewRequests.push_back({
					RenderOutputKind::GameView,
					RenderViewType::Game,
					glm::uvec2(640u, 360u),
					1u,
					1u });
				snapshot.animatedGeometryPageDemands = {
					MakeLodDemand(3u, 1u), MakeLodDemand(3u, 2u) };
				const std::vector<Renderer::VirtualGeometryViewDiagnosticsSnapshot>
					diagnostics = Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
						snapshot, {}, VirtualGeometryInactiveReason::None);
				context.Expect(
					diagnostics.size() == 1u
						&& diagnostics.front().animatedAnimationLods
							== std::vector<std::uint32_t>{ 3u }
						&& diagnostics.front().animatedReuseCount == 1u
						&& diagnostics.front().animatedSkippedWorkCount == 0u
						&& diagnostics.front().animatedBoundsSource
							== "PlannedCurrentAndHistoryDynamicBounds",
					"Animation diagnostics must expose LOD, reuse, skipped work, and a planned bounds source without falsely claiming GPU completion.");
			});
	}

} // namespace TerraEngine::Tests
