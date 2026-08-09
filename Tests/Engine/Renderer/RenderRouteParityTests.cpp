#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

namespace TerraEngine::Tests
{
	void RegisterRenderRouteParityTests(TestRegistry& registry)
	{
		registry.Add("Renderer.RouteParity.PreservesDirectAndVirtualRouteIdentity", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			PreparedRenderRouteEnvelope envelope;
			envelope.frameId = 11u;
			envelope.snapshotGeneration = 22u;
			envelope.backendCapabilityIdentity = 33u;
			envelope.preparationHash = 44u;
			for (const GeometryRoute route : { GeometryRoute::DirectMeshlet, GeometryRoute::VirtualGeometryHardware, GeometryRoute::VirtualGeometryCompute, GeometryRoute::VirtualGeometryFallback })
			{
				GeometryRouteDecision decision;
				decision.decisionIdentity = static_cast<std::uint64_t>(route) + 1u;
				decision.frameId = envelope.frameId;
				decision.objectIdentity = decision.decisionIdentity;
				decision.selected = route;
				decision.routeGeneration = envelope.snapshotGeneration;
				envelope.routeDecisions.push_back(decision);
			}
			DirectMeshletBatch direct;
			direct.geometryIdentity = 9u;
			direct.geometryGeneration = envelope.snapshotGeneration;
			direct.materialIdentity = 10u;
			direct.instanceIdentities = { 100u, 101u };
			direct.firstMeshlet = 0u;
			direct.meshletCount = 12u;
			envelope.directBatches.push_back(direct);
			context.Expect(envelope.IsComplete(), "Route envelope must carry frame, snapshot, and preparation identities.");
			context.ExpectEqual(static_cast<std::size_t>(4), envelope.routeDecisions.size(), "Mixed route parity must retain every selected route independently.");
			context.ExpectEqual(100ULL, envelope.directBatches.front().instanceIdentities.front(), "Direct route batches must retain per-instance identity.");
		});

		registry.Add("Renderer.RouteParity.PartialOwnerLodRemainsIndependent", [](TestContext& context)
		{
			TerraEngine::Renderer::VirtualGeometryDrawRange first;
			TerraEngine::Renderer::VirtualGeometryDrawRange second;
			first.selectedLodIndex = 1u;
			second.selectedLodIndex = 4u;
			first.virtualGeometryActive = true;
			second.virtualGeometryActive = true;
			context.Expect(first.selectedLodIndex != second.selectedLodIndex,
				"Per-owner LOD transitions must not snap as a shared group.");
		});

		registry.Add("Renderer.RouteParity.SealedRangesRetainDirectDenseAndAnimatedRoutes",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFramePublication publication{};
			publication.generation = 9u;
			publication.views.push_back({});
			RenderViewSelection& view = publication.views.front();
			view.generation = 3u;
			for(std::uint32_t ordinal = 0u; ordinal < 4u; ++ordinal)
			{
				view.visibleOrdinals.push_back({ ordinal, 2u });
				view.selectedLods.push_back(ordinal);
				view.routeSelectors.push_back(ordinal);
				view.packetTemplateIdentities.push_back(100u + ordinal);
				view.tintSelectors.push_back(0u);
				view.editorTints.push_back(glm::vec4(0.0f));
			}
			// Route type is publication data. Vulkan must not infer or collapse it
			// while lowering compact ranges into backend recording work.
			publication.submissionRanges = {
				{ RenderSubmissionRoute::Direct, 0u, 0u, 1u, 100u, true },
				{ RenderSubmissionRoute::VirtualGeometry, 0u, 1u, 1u, 101u, true },
				{ RenderSubmissionRoute::Skinned, 0u, 2u, 1u, 102u, true },
				{ RenderSubmissionRoute::Capture, 0u, 3u, 1u, 103u, true }
			};
			const Vulkan::VulkanPublishedRangePlan plan =
				Vulkan::BuildVulkanPublishedRangePlan(publication, 0u);
			context.Expect(plan.valid && plan.ranges.size() == 4u
				&& plan.ranges[0].route == RenderSubmissionRoute::Direct
				&& plan.ranges[1].route == RenderSubmissionRoute::VirtualGeometry
				&& plan.ranges[2].route == RenderSubmissionRoute::Skinned
				&& plan.ranges[3].route == RenderSubmissionRoute::Capture,
				"Direct, dense-VG, animated, and capture routes must remain typed and ordered through backend lowering.");
		});
	}
}
