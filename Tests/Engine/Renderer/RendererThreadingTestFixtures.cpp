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

	TerraEngine::Renderer::RenderFramePublication BuildRendererThreadingPublication(
		const TerraEngine::Renderer::RenderFrameSnapshot& snapshot)
	{
		using namespace TerraEngine::Renderer;
		RenderFramePublication publication;
		publication.frameId = snapshot.frameId;
		publication.generation = std::max<std::uint64_t>(1u, snapshot.frameId);
		publication.rendererSessionGeneration = 1u;
		publication.sceneGeneration = std::max<std::uint64_t>(1u, snapshot.sceneRevision);

		auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
		persistent->generation = publication.generation;
		persistent->rendererSessionGeneration = publication.rendererSessionGeneration;
		auto persistentPage = std::make_shared<RenderPersistentPage>();
		persistentPage->generation = publication.generation;
		auto dynamic = std::make_shared<RenderDynamicStateRoot>();
		dynamic->generation = publication.generation;
		dynamic->rendererSessionGeneration = publication.rendererSessionGeneration;
		auto dynamicPage = std::make_shared<RenderDynamicPage>();
		dynamicPage->generation = publication.generation;

		for (std::size_t index = 0u; index < snapshot.renderables.size(); ++index)
		{
			const RenderFrameRenderableRecord& renderable = snapshot.renderables[index];
			RenderPersistentSlot slot;
			slot.key = { static_cast<std::uint32_t>(index), 1u };
			slot.objectId = renderable.objectId;
			slot.meshId = renderable.meshId;
			slot.materialId = renderable.materialId;
			slot.meshGeneration = 1u;
			slot.materialGeneration = 1u;
			slot.resourceGeneration = std::max<std::uint64_t>(1u, snapshot.resourceRevision);
			slot.packetTemplateIdentity = static_cast<std::uint64_t>(index) + 1u;
			slot.occupied = true;
			persistentPage->slots.push_back(slot);

			RenderDynamicLane lane;
			lane.key = slot.key;
			lane.occupied = true;
			dynamicPage->lanes.push_back(lane);
			dynamicPage->changedLanes.push_back(true);
		}
		persistent->pages.push_back(std::move(persistentPage));
		dynamic->pages.push_back(std::move(dynamicPage));
		publication.persistent = std::move(persistent);
		publication.dynamic = std::move(dynamic);

		for (const RenderFrameViewRequest& request : snapshot.viewRequests)
		{
			RenderViewSelection selection;
			selection.outputKind = request.outputKind;
			selection.viewType = request.viewType;
			selection.outputId = request.outputId;
			selection.generation = std::max<std::uint64_t>(1u, request.viewGeneration);
			RenderViewPresentationInput input;
			input.output.kind = request.outputKind;
			input.output.diagnosticsView = request.viewType;
			input.output.outputId = request.outputId;
			for (std::size_t index = 0u; index < snapshot.renderables.size(); ++index)
			{
				selection.visibleOrdinals.push_back({ static_cast<std::uint32_t>(index), 1u });
				selection.selectedLods.push_back(0u);
				selection.routeSelectors.push_back(0u);
				selection.packetTemplateIdentities.push_back(index + 1u);
				selection.tintSelectors.push_back(0u);
				selection.editorTints.push_back(glm::vec4(0.0f));
			}
			for (const RenderFrameLightRecord& light : snapshot.lights)
			{
				FrameLightCandidate candidate;
				candidate.lightId = light.lightId;
				input.lightCandidates.push_back(candidate);
				if (light.castsShadows)
				{
					const std::size_t faceCount = light.lightType == 1u ? 6u : 1u;
					for (std::size_t face = 0u; face < faceCount; ++face)
					{
						ShadowDrawList shadow;
						shadow.lightId = light.lightId;
						shadow.stableOrdinals = { 0u };
						shadow.selectedLods = { 0u };
						shadow.routeSelectors = { 0u };
						shadow.editorTints = { glm::vec4(0.0f) };
						shadow.commandLanes.resize(1u);
						input.shadowDrawLists.push_back(std::move(shadow));
					}
				}
			}
			publication.views.push_back(std::move(selection));
			publication.presentationInputs.push_back(std::move(input));
		}

		std::vector<RenderResourcePreparationRecord> resources;
		const auto appendResources = [&resources, &snapshot](const auto& source)
		{
			for (const RenderFrameResourceRecord& resource : source)
			{
				resources.push_back({ resource.resourceId,
					std::max<std::uint64_t>(1u, snapshot.resourceRevision), 0u,
					resource.resident, resource.usingFallback });
			}
		};
		appendResources(snapshot.meshes);
		appendResources(snapshot.materials);
		appendResources(snapshot.textures);
		publication.resourcePreparation = BuildRenderResourcePreparationTable(resources,
			std::max<std::uint64_t>(1u, snapshot.resourceRevision));
		return publication;
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
