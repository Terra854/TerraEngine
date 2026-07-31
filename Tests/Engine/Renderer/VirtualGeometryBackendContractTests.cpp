#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
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
					"Non-Vulkan backends remain explicitly processor-oracle fallback-only for feature 045.");
			});
	}
} // namespace TerraEngine::Tests
