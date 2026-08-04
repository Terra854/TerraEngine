#include "TestHarness.h"

#include "Renderer/Graph/RenderSubmissionPlan.h"

namespace TerraEngine::Tests {

	void RegisterRenderSubmissionPlanTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Submission.FailedPacketsBlockSubmission", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 4u;
			plan.orderedPasses.push_back({ "draw" });
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "draw.0";
			packet.passId = "draw";
			packet.state = TerraEngine::Renderer::RenderPacketState::Failed;
			plan.recordedWork.push_back(packet);
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 4u);
			context.Expect(!plan.valid, "Failed required packets should prevent unsafe submission.");
		});

		registry.Add("Renderer.Submission.WorkersCannotOwnPresentation", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 8u;
			plan.orderedPasses.push_back({ "present" });
			TerraEngine::Renderer::RenderWorkPacket packet;
			packet.packetId = "present.0";
			packet.passId = "present";
			packet.executionPolicy = TerraEngine::Renderer::RenderPacketExecutionPolicy::Worker;
			packet.outputs.push_back({ 9u, TerraEngine::Renderer::RenderAccessMode::Present, TerraEngine::Renderer::RenderStage::Present, "present" });
			plan.recordedWork.push_back(packet);
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 8u);
			context.Expect(!plan.valid, "Presentation ownership should remain coordinator-owned.");
		});

		registry.Add("Renderer.Submission.FrameOverlapIsDisabledByDefault", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 13u;
			plan.orderedPasses.push_back({ "draw" });
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 13u);
			context.Expect(plan.valid, "Ordinary same-frame submission should remain valid.");
			context.Expect(!plan.frameOverlapAllowed, "Frame overlap should stay disabled until lifecycle validation enables it.");
		});

		registry.Add("Renderer.Submission.NoRequestedViewsCanSubmitNoopFrame", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 21u;
			plan.requestedViewCount = 0u;
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 21u);
			context.Expect(plan.valid, "Editor frames without scene or game views should be valid no-op renderer submissions.");
		});

		registry.Add("Renderer.Submission.RequestedViewsRequireGraphPasses", [](TestContext& context) {
			TerraEngine::Renderer::RenderSubmissionPlan plan;
			plan.frameId = 22u;
			plan.requestedViewCount = 1u;
			TerraEngine::Renderer::ValidateRenderSubmissionPlan(plan, 22u);
			context.Expect(!plan.valid, "Requested render views still need graph passes before backend submission.");
		});

		registry.Add("Animation.RenderSubmission.AcceptsCompleteCpuDeformationPlan", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto deformation = std::make_shared<CpuAnimationDeformationData>();
			deformation->paletteMatrices.resize(1U);
			deformation->jointCount = 1U;
			deformation->contentIdLow = 1U;
			deformation->contentIdHigh = 2U;
			auto cpuPlan = std::make_shared<CpuAnimationDeformationPlan>();
			cpuPlan->frameId = 31U;
			cpuPlan->rendererGeneration = 4U;
			TerraEngine::UUID meshId{};
			meshId[0U] = 1U;
			cpuPlan->records.push_back({
				9U,
				meshId,
				7U,
				6U,
				4U,
				5U,
				deformation});

			RenderSubmissionPlan plan;
			plan.frameId = 31U;
			plan.animationWork.frameId = 31U;
			plan.animationWork.cpuOrFallbackAnimatorCount = 1U;
			plan.animationWork.cpuDeformationPlan = std::move(cpuPlan);
			ValidateRenderSubmissionPlan(plan, 31U);

			context.Expect(plan.valid,
				"A complete same-frame CPU palette plan must reach the protected backend upload boundary.");
		});

		registry.Add("Animation.RenderSubmission.RejectsStaleCpuDeformationAtomically", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto deformation = std::make_shared<CpuAnimationDeformationData>();
			deformation->paletteMatrices.resize(1U);
			deformation->jointCount = 1U;
			deformation->contentIdLow = 1U;
			deformation->contentIdHigh = 2U;
			auto cpuPlan = std::make_shared<CpuAnimationDeformationPlan>();
			cpuPlan->frameId = 40U;
			cpuPlan->rendererGeneration = 3U;
			TerraEngine::UUID meshId{};
			meshId[0U] = 1U;
			cpuPlan->records.push_back({
				8U,
				meshId,
				6U,
				5U,
				3U,
				4U,
				deformation});

			RenderSubmissionPlan plan;
			plan.frameId = 41U;
			plan.animationWork.frameId = 41U;
			plan.animationWork.cpuOrFallbackAnimatorCount = 1U;
			plan.animationWork.cpuDeformationPlan = std::move(cpuPlan);
			ValidateRenderSubmissionPlan(plan, 41U);

			context.Expect(!plan.valid,
				"A stale CPU palette plan must reject the complete frame handoff rather than mix pose generations.");
		});
	}

}
