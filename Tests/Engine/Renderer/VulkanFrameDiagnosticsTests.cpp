#include "TestHarness.h"

#include "Renderer/RHI/Vulkan/VulkanFrameDiagnostics.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::Tests {

	void RegisterVulkanFrameDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanFrameDiagnostics.RecordsBackendStages", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown = TerraEngine::Renderer::Vulkan::CreateVulkanFrameBreakdown(61u);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
				breakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 2u, "queue submit", true, true },
				0.33f);

			context.ExpectEqual(static_cast<std::size_t>(1u), breakdown.stageTimings.size(), "Vulkan stage helpers should append one timing row.");
			context.ExpectEqual(std::string("vulkan"), breakdown.stageTimings.front().backendId, "Vulkan timing rows should name the backend.");
			context.ExpectEqual(static_cast<std::uint32_t>(2u), breakdown.stageTimings.front().viewId, "Vulkan timing rows should preserve view identifiers.");
		});

		registry.Add("Renderer.VulkanFrameDiagnostics.RecordsForcedIdleStalls", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown = TerraEngine::Renderer::Vulkan::CreateVulkanFrameBreakdown(62u);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				breakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::DeviceIdleWait, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, RenderViewType::Game, 0u, "wait idle", true, false },
				0.01f,
				0.25f);

			context.ExpectEqual(static_cast<std::size_t>(1u), breakdown.stallDiagnostics.size(), "Forced idle should be reported even when the measured wait is short.");
			context.Expect(breakdown.stallDiagnostics.front().waitType == TerraEngine::Renderer::BackendStallType::DeviceIdle, "Forced idle should map to device-idle stalls.");
		});

		registry.Add("Renderer.VulkanFrameDiagnostics.BuildsViewSubmitItems", [](TestContext& context) {
			const TerraEngine::Renderer::ViewSubmitWorkItem item = TerraEngine::Renderer::Vulkan::BuildVulkanViewSubmitWorkItem(
				63u,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ViewSubmission, TerraEngine::Renderer::BackendSubmitStage::ViewSubmit, RenderViewType::Scene, 9u, "scene submit", false, true },
				4u,
				5u,
				0.2f,
				0.1f,
				"scene-view");

			context.ExpectEqual(static_cast<std::uint64_t>(63u), item.frameId, "Vulkan view submit helper should preserve the frame id.");
			context.ExpectEqual(static_cast<std::uint32_t>(9u), item.viewId, "Vulkan view submit helper should preserve the view id.");
			context.ExpectEqual(std::string("scene-view"), item.sharedWorkKey, "Vulkan view submit helper should preserve the reuse key.");
		});

		registry.Add("Renderer.VulkanFrameDiagnostics.RetainsLatestValidViewStats", [](TestContext& context) {
			std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp");
			const std::string text((std::istreambuf_iterator<char>(source)), {});
			const std::size_t publish = text.find("std::lock_guard<std::mutex> lock(statsMutex);",
				text.find("void VulkanContext::Impl::PublishCompletedFrameStats"));
			context.Expect(publish != std::string::npos,
				"Completed Vulkan statistics must publish under the diagnostics lock.");
			if(publish == std::string::npos) return;
			const std::string region = text.substr(publish, 1400U);
			context.Expect(region.find("if(gameSnapshot.valid)") != std::string::npos
				&& region.find("if(sceneSnapshot.valid)") != std::string::npos,
				"An empty completed slot must not erase the latest valid Game or Scene statistics.");
		});
	}

}
