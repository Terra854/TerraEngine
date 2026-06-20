#include "RendererThreadingTestFixtures.h"

#include <algorithm>

namespace TerraEngine::Tests {

	TerraEngine::Renderer::RenderFrameSnapshot MakeRendererThreadingSnapshot(
		std::uint64_t frameId,
		std::size_t renderableCount,
		std::size_t lightCount)
	{
		TerraEngine::Renderer::RenderFrameSnapshot snapshot;
		snapshot.frameId = frameId;
		snapshot.sceneRevision = frameId * 10u;
		snapshot.resourceRevision = frameId * 100u;
		snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 1u });
		for (std::size_t index = 0u; index < renderableCount; ++index)
		{
			const TerraEngine::UUID mesh = TerraEngine::GenerateUUID();
			const TerraEngine::UUID material = TerraEngine::GenerateUUID();
			snapshot.meshes.push_back({ mesh, "mesh", true, false });
			snapshot.materials.push_back({ material, "material", true, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "texture", true, false });
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), mesh, material });
		}
		for (std::size_t index = 0u; index < lightCount; ++index)
		{
			snapshot.lights.push_back({ TerraEngine::GenerateUUID(), index % 2u == 0u ? 1u : 0u, true, false });
		}
		TerraEngine::Renderer::SortSnapshotDeterministically(snapshot);
		return snapshot;
	}

	TerraEngine::Renderer::BackendCapabilityProfile MakeThreadedRendererProfile()
	{
		TerraEngine::Renderer::BackendCapabilityProfile profile;
		profile.backendId = "test-threaded";
		profile.supportsParallelRecording = true;
		profile.supportsAsyncUploadPreparation = true;
		profile.supportsTransientResources = true;
		profile.supportsSecondaryRecording = true;
		profile.supportsParallelShadowRecording = true;
		profile.supportsParallelPerViewRecording = true;
		profile.supportsParallelPipelineStatePreparation = true;
		profile.supportsParallelDescriptorPreparation = true;
		profile.supportsParallelUploadPlanning = true;
		profile.supportsParallelTransitionPlanning = true;
		profile.supportsParallelCulling = true;
		profile.supportsParallelLightPreparation = true;
		profile.supportsParallelUiPreparation = true;
		profile.supportsParallelPacketValidation = true;
		profile.supportsPacketLocalDiagnostics = true;
		profile.supportsRenderTaskGroups = true;
		profile.supportsFrameOverlap = true;
		profile.supportsRenderWorkBudgets = true;
		profile.supportsMultiQueueReadiness = true;
		profile.maxRecordingWorkers = 8u;
		profile.workerSchedulingThreshold = 4u;
		profile.requiredSerialBoundaries = { "final-submit", "present", "swapchain", "recovery", "resource-retire" };
		return profile;
	}

	TerraEngine::Renderer::RenderPassNode MakeRendererPass(
		std::string passId,
		std::uint64_t resourceId,
		TerraEngine::Renderer::RenderAccessMode outputAccess)
	{
		TerraEngine::Renderer::RenderPassNode pass;
		pass.passId = std::move(passId);
		pass.outputs.push_back({ resourceId, outputAccess, TerraEngine::Renderer::RenderStage::Geometry, pass.passId });
		pass.profilingLabel = pass.passId;
		return pass;
	}

	void RandomizePacketCompletionOrder(std::vector<TerraEngine::Renderer::RenderWorkPacket>& packets)
	{
		std::reverse(packets.begin(), packets.end());
		for (TerraEngine::Renderer::RenderWorkPacket& packet : packets)
		{
			packet.state = TerraEngine::Renderer::RenderPacketState::Completed;
		}
	}

	void InjectResourceDelay(TerraEngine::Renderer::RenderFrameSnapshot& snapshot, const char* reason)
	{
		if (snapshot.textures.empty())
		{
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "delayed-texture", false, true });
		}
		snapshot.textures.front().resident = false;
		snapshot.textures.front().usingFallback = true;
		snapshot.textures.front().readiness = TerraEngine::Renderer::RenderResourceReadinessState::Fallback;
		snapshot.textures.front().fallbackReason = reason == nullptr ? "synthetic delay" : reason;
	}

}
