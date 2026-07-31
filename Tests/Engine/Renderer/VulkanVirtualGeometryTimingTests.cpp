#include "TestHarness.h"

#include "Renderer/RHI/Vulkan/VulkanFrameDiagnostics.h"

namespace TerraEngine::Tests
{

	void RegisterVulkanVirtualGeometryTimingTests(TestRegistry &registry)
	{
		registry.Add("Renderer.VulkanVirtualGeometryTiming.EnumeratesEveryRequiredStage",
			[](TestContext &context)
			{
				const auto markers = TerraEngine::Renderer::Vulkan::CreateVulkanVirtualGeometryStageMarkers();

				context.ExpectEqual(static_cast<std::size_t>(13u),
					markers.size(),
					"Vulkan timing must explicitly own every virtual-geometry stage, including unsupported routes.");
				for (const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryStageMarker &marker : markers)
				{
					context.Expect(marker.availability == TerraEngine::Renderer::BackendMeasurementAvailability::Unavailable,
						"Stages without a live Vulkan command must remain unavailable rather than report a zero duration.");
					context.Expect(!marker.label.empty(), "Every virtual-geometry stage marker needs a stable diagnostic label.");
					context.Expect(!marker.reason.empty(), "Unavailable stages need an explicit ownership reason.");
				}
			});

		registry.Add("Renderer.VulkanVirtualGeometryTiming.KeepsLiveTimestampPendingUntilReadback",
			[](TestContext &context)
			{
				const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryStageMarker marker =
					TerraEngine::Renderer::Vulkan::MakeVulkanVirtualGeometryStageMarker(TerraEngine::Renderer::VirtualGeometryBackendStage::HardwareRaster,
						TerraEngine::Renderer::VirtualGeometryBackendRoute::MeshShader,
						true,
						true);

				context.Expect(marker.availability == TerraEngine::Renderer::BackendMeasurementAvailability::Pending,
					"A recorded Vulkan stage marker must stay pending until its delayed query readback completes.");
				context.Expect(marker.ownsLiveCommand, "Only a live Vulkan command may own a pending virtual-geometry timing marker.");
			});

		registry.Add("Renderer.VulkanVirtualGeometryTiming.RejectsUnmarkedLiveCommand",
			[](TestContext &context)
			{
				const TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryStageMarker marker =
					TerraEngine::Renderer::Vulkan::MakeVulkanVirtualGeometryStageMarker(TerraEngine::Renderer::VirtualGeometryBackendStage::Compaction,
						TerraEngine::Renderer::VirtualGeometryBackendRoute::Compute,
						true,
						false);

				context.Expect(marker.availability == TerraEngine::Renderer::BackendMeasurementAvailability::Invalid,
					"A live command without both Vulkan timestamp writes must be invalid, never a measured zero.");
				context.Expect(!marker.reason.empty(), "Invalid timing ownership needs an explanation for diagnostics and automation.");
			});
	}

} // namespace TerraEngine::Tests
