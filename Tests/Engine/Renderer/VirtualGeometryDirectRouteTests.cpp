#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Frame/RenderVirtualGeometrySelection.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/VirtualGeometry/Selection/GeometryRoutePolicy.h"
#include "Resource/VirtualGeometryStreamingDiagnostics.h"

#include <glm/gtc/matrix_transform.hpp>

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
				RenderItemRecord direct{};
				direct.geometryRoute = Renderer::GeometryRoute::DirectMeshlet;
				context.Expect(!direct.UsesVirtualGeometryRoute(),
					"Page-less direct-resident meshes must remain conventional indexed draws.");

				RenderItemRecord hardware{};
				hardware.geometryRoute = Renderer::GeometryRoute::VirtualGeometryHardware;
				context.Expect(hardware.UsesVirtualGeometryRoute(),
					"Hardware virtual geometry must enter view-local VG selection.");

				RenderItemRecord compute{};
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

		registry.Add("VirtualGeometry.DirectRoute.OrthographicLodFollowsZoomAndPerspectiveKeepsDistance",
			[](TestContext& context)
			{
				Mesh mesh{};
				mesh.hasVirtualGeometry = true;
				mesh.hasLocalBounds = true;
				for (int axis = 0; axis < 3; ++axis)
				{
					mesh.localBoundsMin[axis] = -1.0f;
					mesh.localBoundsMax[axis] = 1.0f;
				}
				mesh.virtualGeometryDetailLevels = {
					{ .lodIndex = 0u, .meshletCount = 8u },
					{ .lodIndex = 1u, .meshletOffset = 8u, .meshletCount = 4u, .geometricError = 0.02f },
					{ .lodIndex = 2u, .meshletOffset = 12u, .meshletCount = 2u, .geometricError = 0.04f }
				};
				auto quality = MakeDefaultVirtualGeometryQualityProfile();
				quality.maxLodDrop = 2u;
				const glm::uvec2 viewport(1280u, 640u);
				const glm::vec3 nearCamera(0.0f, 0.0f, 3.0f);
				const glm::vec3 farCamera(0.0f, 0.0f, 40.0f);
				const auto select = [&](const glm::mat4& projection, const glm::vec3& camera) {
					const auto view = glm::lookAtRH(camera, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
					return Renderer::Frame::SelectCookedDirectLodRange(
						mesh, quality, glm::mat4(1.0f), projection * view, camera, viewport);
				};

				// The box still spans the viewport after the orthographic camera
				// dollies back. Increasing the projection size shrinks its footprint.
				const auto ortho = glm::orthoRH_ZO(-1.0f, 1.0f, -0.5f, 0.5f, 0.1f, 100.0f);
				const auto widerOrtho = glm::orthoRH_ZO(-8.0f, 8.0f, -4.0f, 4.0f, 0.1f, 100.0f);
				context.ExpectEqual(0u, select(ortho, nearCamera).selectedLodIndex,
					"A large orthographic footprint must retain full detail.");
				context.ExpectEqual(0u, select(ortho, farCamera).selectedLodIndex,
					"Changing orthographic camera depth must preserve detail.");
				const auto zoomedOut = select(widerOrtho, farCamera);
				context.Expect(zoomedOut.selectedLodIndex == 2u && zoomedOut.meshletOffset == 12u
					&& zoomedOut.meshletCount == 2u,
					"Orthographic zoom must select the appropriate cooked meshlet range.");

				const auto perspective = glm::perspectiveRH_ZO(glm::radians(60.0f), 2.0f, 0.1f, 100.0f);
				context.ExpectEqual(0u, select(perspective, nearCamera).selectedLodIndex,
					"The perspective selector must still retain near detail.");
				context.ExpectEqual(2u, select(perspective, farCamera).selectedLodIndex,
					"The perspective selector must still reduce detail with distance.");
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
