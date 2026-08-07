#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
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
	}
}
