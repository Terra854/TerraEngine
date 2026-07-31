#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Selection/GeometryRoutePolicy.h"
#include "TestHarness.h"
namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryBackendContractTests(TestRegistry &r)
	{
		r.Add("VirtualGeometry.BackendContract.DepthIdentityIsPortable",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				c.Expect(WinsVirtualGeometryDepthTie({ .2f, 1 }, { .2f, 2 }), "Generated work uses backend-neutral stable visibility identity.");
			});

		r.Add("VirtualGeometry.BackendContract.DeviceAuthoredRouteIsVulkanOnlyAndComplete",
			[](TestContext &c)
			{
				using namespace TerraEngine::Renderer;
				BackendCapabilityProfile profile{};
				profile.backendId = "vulkan";
				profile.supportsVirtualGeometryGpuPacketBuffers = true;
				profile.supportsVirtualGeometryGpuCompaction = true;
				profile.supportsVirtualGeometryGeneratedSubmissions = true;
				profile.virtualGeometryGeneratedWork.supportsDeviceAuthoredWork = true;
				profile.virtualGeometryGeneratedWork.maximumCommandCount = 128u;
				const VirtualGeometryBackendSelection live =
					SelectVirtualGeometryBackend(profile);
				c.Expect(live.deviceAuthoredWorkAvailable
					&& live.route == VirtualGeometrySubmissionRoute::VulkanDeviceAuthored,
					"Only a complete Vulkan capability set may select device-authored virtual geometry.");

				profile.backendId = "mock";
				const VirtualGeometryBackendSelection fallback =
					SelectVirtualGeometryBackend(profile);
				c.Expect(!fallback.deviceAuthoredWorkAvailable
					&& fallback.route == VirtualGeometrySubmissionRoute::ProcessorOracle
					&& !fallback.fallbackReason.empty(),
					"Non-Vulkan backends remain explicitly processor-oracle fallback-only for capability validation.");
			});

		r.Add("VirtualGeometry.BackendContract.BootstrapAvailabilityDoesNotClaimExecutionEvidence",
			[](TestContext& c)
			{
				using namespace TerraEngine::Renderer;
				BackendCapabilityProfile bootstrap{};
				bootstrap.backendId = "vulkan";
				bootstrap.supportsVirtualGeometryGpuPacketBuffers = true;

				// A live device may admit the first packet-producing frame, but it
				// cannot claim the device-authored route until fence-delayed stage
				// receipts promote compaction and generated-submission evidence.
				const VirtualGeometryBackendSelection beforeReceipt =
					SelectVirtualGeometryBackend(bootstrap);
				c.Expect(!beforeReceipt.deviceAuthoredWorkAvailable,
					"Bootstrap storage capability must not masquerade as completed GPU execution.");

				GeometryRoutePolicyInput route{};
				route.direct.valid = false;
				route.virtualGeometryHardware.valid = true;
				route.virtualGeometryHardware.capabilitiesAvailable =
					bootstrap.supportsVirtualGeometryGpuPacketBuffers;
				route.virtualGeometryHardware.estimatedCost = 1.0F;
				const GeometryRouteDecision admitted =
					SelectGeometryRoute(route, {});
				c.Expect(admitted.selected == GeometryRoute::VirtualGeometryHardware,
					"A fresh Vulkan session must admit one real virtual-geometry frame so execution evidence can be produced.");
			});
	}
} // namespace TerraEngine::Tests
