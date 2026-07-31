#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/VirtualGeometry/Selection/GeometryRoutePolicy.h"
#include "Resource/VirtualGeometryStreamingDiagnostics.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryDirectRouteTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.DirectRoute.SuppressesAllVirtualGeometryWork",
			[](TestContext &context)
			{
				Renderer::GeometryRouteFramePlan plan{};
				plan.selectedRoute = Renderer::GeometryRoute::DirectMeshlet;
				plan.instances = 4096U;
				Renderer::BuildGeometryRouteFramePlan(plan);

				context.Expect(plan.vgHierarchyTraversalCount == 0U, "DirectMeshlet must not traverse a VG hierarchy.");
				context.Expect(plan.vgPageRequestCount == 0U, "DirectMeshlet must not emit VG page requests.");
				context.Expect(plan.vgDecodeCount == 0U, "DirectMeshlet must not schedule VG decode work.");
				context.Expect(plan.vgResidencyPublicationCount == 0U, "DirectMeshlet must not publish VG residency records.");
				context.Expect(plan.vgCompactionCount == 0U, "DirectMeshlet must not compact VG work.");
				context.Expect(plan.generatedVirtualGeometryPacketCount == 0U, "DirectMeshlet must not generate VG packets.");
				context.Expect(plan.vgExecutorAllocationCount == 0U, "DirectMeshlet must not allocate a VG executor.");
				context.Expect(plan.directBatchCount == 1U, "Direct instances should be submitted as one shared geometry batch.");
				context.Expect(plan.sharedGeometryInstanceCount == 4096U, "Repeated direct instances must share owner-packed geometry.");
			});

		registry.Add("VirtualGeometry.DirectRoute.PreservesCrossViewIdentity",
			[](TestContext &context)
			{
				Renderer::GeometryRouteFramePlan plan{};
				plan.selectedRoute = Renderer::GeometryRoute::DirectMeshlet;
				plan.viewKinds = { Renderer::GeometryViewKind::Camera, Renderer::GeometryViewKind::Shadow,
					Renderer::GeometryViewKind::Capture, Renderer::GeometryViewKind::Editor };
				Renderer::BuildGeometryRouteFramePlan(plan);
				context.Expect(plan.identityStableAcrossViews, "Direct route must preserve object, mesh, primitive, and picking identity across views.");
			});

		registry.Add("VirtualGeometry.DirectRoute.DrawCommandsBypassVirtualSelection",
			[](TestContext& context)
			{
				DrawCommand direct{};
				direct.geometryRoute = Renderer::GeometryRoute::DirectMeshlet;
				context.Expect(!direct.UsesVirtualGeometryRoute(),
					"Page-less direct-resident meshes must remain conventional indexed draws.");

				DrawCommand hardware{};
				hardware.geometryRoute = Renderer::GeometryRoute::VirtualGeometryHardware;
				context.Expect(hardware.UsesVirtualGeometryRoute(),
					"Hardware virtual geometry must enter view-local VG selection.");

				DrawCommand compute{};
				compute.geometryRoute = Renderer::GeometryRoute::VirtualGeometryCompute;
				context.Expect(compute.UsesVirtualGeometryRoute(),
					"Compute virtual geometry must enter view-local VG selection.");
			});

		registry.Add("VirtualGeometry.DirectRoute.SelectsOneResidentLodWithoutVirtualWork",
			[](TestContext& context)
			{
				VirtualGeometryAssetRuntime asset{};
				asset.detailLevels = {
					{ 0u, 0u, 8u, 512u, 0.0f, 1.0f },
					{ 1u, 8u, 4u, 256u, 0.02f, 0.5f },
					{ 2u, 12u, 2u, 128u, 0.04f, 0.25f }
				};
				VirtualGeometryQualityProfile quality =
					MakeDefaultVirtualGeometryQualityProfile();
				quality.maxLodDrop = 2u;

				const Renderer::VirtualGeometryDrawRange nearRange =
					RenderSystem::SelectDirectResidentLodRange(asset, quality, 640.0f);
				const Renderer::VirtualGeometryDrawRange farRange =
					RenderSystem::SelectDirectResidentLodRange(asset, quality, 80.0f);

				context.ExpectEqual(0u, nearRange.selectedLodIndex,
					"A nearby direct mesh must retain its source-detail range.");
				context.ExpectEqual(2u, farRange.selectedLodIndex,
					"A distant direct mesh must select the coarse resident range.");
				context.ExpectEqual(12u, farRange.meshletOffset,
					"Direct LOD selection must preserve the cooked range offset.");
				context.ExpectEqual(2u, farRange.meshletCount,
					"Direct LOD selection must submit only the selected range.");
				context.Expect(!farRange.virtualGeometryActive && !farRange.failOpen,
					"Direct LOD selection must not manufacture virtual hierarchy or fallback work.");
			});

		registry.Add("VirtualGeometry.DirectRoute.SuppressionDiagnosticsRemainExplicit",
			[](TestContext& context)
			{
				Resource::VirtualGeometryStreamingDiagnostics diagnostics{};
				Resource::RecordVirtualGeometryDirectRouteSuppression(
					diagnostics, 42u, 3u, 3u, 3u);
				context.Expect(diagnostics.suppressedDirectRouteFeedback == 3u
					&& diagnostics.suppressedDirectRouteDecodeIntents == 3u
					&& diagnostics.suppressedDirectRouteResidencyPublications == 3u,
					"Direct-route diagnostics must distinguish intentionally suppressed VG work from unavailable streaming.");
				context.Expect(!diagnostics.recentEvents.empty()
					&& diagnostics.recentEvents.back().stage == "direct-route-suppressed",
					"Direct-route suppression must leave a bounded observable event.");
			});
	}
}
