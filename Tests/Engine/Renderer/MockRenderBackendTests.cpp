#include "TestHarness.h"

#include "Renderer/MockRenderBackend.h"
#include "Renderer/Graph/RenderGraph.h"

namespace TerraEngine::Tests {

	void RegisterMockRenderBackendTests(TestRegistry& registry)
	{
		registry.Add("Renderer.MockBackend.ConsumesValidSubmissionPlan", [](TestContext& context) {
			MockRenderBackend backend;
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 1u;
			plan.orderedPasses.push_back({ "draw" });
			context.Expect(backend.Consume(plan), "Mock backend should consume valid backend-neutral plans.");
			context.ExpectEqual(std::string("draw"), backend.lastPlan.orderedPasses.front().passId, "Mock backend should preserve pass ids.");
		});

		registry.Add("Renderer.MockBackend.SerialFallbackWhenParallelUnsupported", [](TestContext& context) {
			auto profile = MockRenderBackend().profile;
			profile.supportsParallelRecording = false;
			profile.maxRecordingWorkers = 1u;
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 2u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1u), 1u });
			snapshot.renderables.resize(100u);
			const auto passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto packetPlan = TerraEngine::Renderer::BuildRenderWorkPacketPlan(snapshot, passes, profile);
			context.ExpectEqual(static_cast<std::size_t>(0u), packetPlan.workerPacketCount, "Unsupported backends should not receive worker packets.");
			context.Expect(packetPlan.serialPacketCount > 0u, "Unsupported backends should receive serial fallback packets.");
		});

		registry.Add("Renderer.MockBackend.ExposesPortableCapabilities", [](TestContext& context) {
			const auto profile = MockRenderBackend().profile;
			context.Expect(profile.supportsParallelRecording, "Mock backend should expose backend-neutral parallel recording support.");
			context.Expect(profile.supportsRenderTaskGroups, "Mock backend should expose render task group support without Vulkan symbols.");
		});

		registry.Add("Renderer.MockBackend.ExposesQueueReadinessWithoutBackendHandles", [](TestContext& context) {
			MockRenderBackend backend;
			backend.profile.supportsMultiQueueReadiness = true;
			backend.profile.queueCapabilities.push_back({ "graphics", TerraEngine::Renderer::RenderBackendQueueMode::Serial, false, {} });
			context.Expect(backend.profile.supportsMultiQueueReadiness, "Mock backend should opt into backend-neutral queue readiness.");
			context.ExpectEqual(std::string("graphics"), backend.profile.queueCapabilities.front().category, "Queue readiness should use portable category names.");
		});
	}

}
