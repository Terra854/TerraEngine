#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/GameViewWindow.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RHI/Vulkan/VulkanFrameDiagnostics.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadGameViewWindowSource()
		{
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			const std::string path = "Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp";
			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			const std::string thisFile = __FILE__;
			const auto marker = thisFile.find("Tests\\Editor\\Renderer\\GameViewStatsOverlayTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Renderer/GameViewStatsOverlayTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}
	}

	void RegisterGameViewStatsOverlayTests(TestRegistry& registry)
	{
		registry.Add("GameViewStatsOverlay.CompactRows.ExcludesRendererDiagnostics", [](TestContext& context) {
			const std::string source = ReadGameViewWindowSource();

			context.Expect(source.find("std::array<std::string, 5>") != std::string::npos, "Game View stats should be capped to a compact fixed row set.");
			context.Expect(source.find("FPS: %.1f") != std::string::npos, "Compact stats should keep the FPS row.");
			context.Expect(source.find("Frame: %.2f ms") != std::string::npos, "Compact stats should keep the frame-time row.");
			context.Expect(source.find("Draw Calls: %u") != std::string::npos, "Compact stats should keep the draw-call row.");
			context.Expect(source.find("Triangles: %u") != std::string::npos, "Compact stats should keep the triangle row.");
			context.Expect(source.find("\"Meshlets Selected\"") == std::string::npos, "Meshlet readbacks should not be drawn in Game View stats.");
			context.Expect(source.find("\"VG Cluster Packets\"") == std::string::npos, "VG packet counters should not be drawn in Game View stats.");
			context.Expect(source.find("\"VG Page Payload Bytes") == std::string::npos, "VG page byte counters should not be drawn in Game View stats.");
			context.Expect(source.find("\"Hi-Z Baseline\"") == std::string::npos, "Hi-Z validation counters should not be drawn in Game View stats.");
		});

		registry.Add("GameViewStatsOverlay.InactivePath.ContentFreeWorkIsBounded", [](TestContext& context) {
			const TerraEngine::Editor::GameViewUpdateDecision hidden =
				TerraEngine::Editor::BuildGameViewUpdateDecision(false, true, false, true);
			context.Expect(hidden.path == TerraEngine::Editor::GameViewUpdatePath::InactiveWindow, "Collapsed Game View should select its inactive path.");
			context.Expect(!hidden.updateRenderTarget && !hidden.queryRenderTexture && !hidden.publishRuntimePointer && !hidden.collectRenderStats, "Inactive Game View should not request target, texture, pointer, or stats work.");

			const TerraEngine::Editor::GameViewUpdateDecision contentFree =
				TerraEngine::Editor::BuildGameViewUpdateDecision(true, false, false, true);
			context.Expect(contentFree.path == TerraEngine::Editor::GameViewUpdatePath::ContentFree, "Camera-free Game View should select its content-free path.");
			context.Expect(!contentFree.updateRenderTarget && !contentFree.queryRenderTexture && !contentFree.collectRenderStats, "Content-free Game View should not create a render target or query texture/stats state.");

			TerraEngine::Editor::GameViewCameraActivityCache cache;
			context.Expect(cache.NeedsRefresh(7u), "The camera activity cache should request its initial membership snapshot.");
			cache.Store(7u, false);
			context.Expect(!cache.NeedsRefresh(7u) && !cache.hasActiveCamera, "An unchanged camera-free membership revision should reuse the empty result.");
			context.Expect(cache.NeedsRefresh(8u), "A new object/component membership revision should invalidate the empty-camera result.");

			const TerraEngine::Editor::GameViewSchedulerDeclaration declaration =
				TerraEngine::Editor::BuildGameViewSchedulerDeclaration();
			context.Expect(declaration.activationPredicate.find("expanded") != std::string::npos, "Game View scheduling should declare its expanded/content activation predicate.");
			context.Expect(declaration.workerSafePreparation.find("immutable") != std::string::npos, "Game View scheduling should declare worker-safe immutable camera preparation.");
			context.Expect(declaration.protectedBoundary.find("ImGui") != std::string::npos, "Game View scheduling should keep ImGui and live request publication protected.");
		});

		registry.Add("GameViewStatsOverlay.PresentationWaits.ClassifiesOnlyUnavoidableStages", [](TestContext& context) {
			TerraEngine::Renderer::BackendFrameBreakdown breakdown;
			breakdown.frameId = 43u;
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::ImageAcquire, RenderViewType::Game, 0.10f, 0u, "acquire");
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::FenceWait, RenderViewType::Game, 0.20f, 0u, "fence");
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0.30f, 0u, "submit");
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0.40f, 0u, "present");
			TerraEngine::Renderer::AddBackendStageTiming(breakdown, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, RenderViewType::Game, 1.00f, 0u, "preventable idle");

			TerraEngine::Renderer::RenderDiagnosticsSummary summary;
			TerraEngine::Renderer::ApplyBackendBreakdownToRenderDiagnostics(summary, breakdown);
			context.ExpectEqual(static_cast<std::size_t>(4u), summary.presentationWaits.size(), "Acquire, fence, submit, and present should produce typed presentation waits while forced idle stays separate.");
			context.Expect(summary.presentationWaits[0].kind == TerraEngine::Profiling::PresentationWaitKind::SwapchainAcquire, "Image acquire should retain its swapchain wait identity.");
			context.Expect(summary.presentationWaits[1].kind == TerraEngine::Profiling::PresentationWaitKind::GpuCompletion, "Frame fence wait should retain its GPU-completion identity.");
			context.Expect(summary.presentationWaits[2].kind == TerraEngine::Profiling::PresentationWaitKind::RenderQueueBackpressure, "Queue submission should retain its backpressure identity.");
			context.Expect(summary.presentationWaits[3].kind == TerraEngine::Profiling::PresentationWaitKind::UiComposition, "Present should retain its platform composition identity.");
			context.ExpectEqual(static_cast<std::uint64_t>(400000u), summary.presentationWaits[3].duration.nanoseconds, "Presentation wait duration should convert milliseconds to integer nanoseconds.");

			const auto vulkanPresent = TerraEngine::Renderer::Vulkan::ClassifyVulkanPresentationWait(
				{ TerraEngine::Renderer::BackendFrameCostCategory::Presentation, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0u, "present", true, false });
			const auto vulkanForcedIdle = TerraEngine::Renderer::Vulkan::ClassifyVulkanPresentationWait(
				{ TerraEngine::Renderer::BackendFrameCostCategory::DeviceIdleWait, TerraEngine::Renderer::BackendSubmitStage::ForcedIdle, RenderViewType::Game, 0u, "idle", true, false });
			context.Expect(vulkanPresent.has_value() && *vulkanPresent == TerraEngine::Profiling::PresentationWaitKind::UiComposition, "Vulkan present should map into the provider-neutral presentation category.");
			context.Expect(!vulkanForcedIdle.has_value(), "Preventable Vulkan forced idle must not be reported as unavoidable presentation time.");
		});
	}
}
