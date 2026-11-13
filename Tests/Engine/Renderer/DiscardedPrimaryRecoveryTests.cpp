// A failed first view must not donate its discarded compute prefix to a
// later view. All plans originate from real cooked assets + authored playback.
#include "TestHarness.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include "Tests/Support/ReviewRemediation/ExistingRendererFixtureSession.h"
#include "Tests/Support/ReviewRemediation/DiscardedPrimaryReviewHooks.h"
// Impl contains validation-gated fields before every native owner. This TU
// must use the Engine's matching validation ABI; otherwise even a read is unsafe.
#if !defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION) || !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
#error Native review Impl access requires the matching Engine validation ABI.
#endif
#include "Renderer/RHI/Vulkan/VulkanContextImpl.h"
#include "Animation/AnimationSystem.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Scene/SceneManager.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Core/Object/ObjectManager.h"
#include "Terrain/TerrainMaterial.h"
#include "Utilities/PlatformUtils.h"
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <cmath>
#include <cstring>
#include <span>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests::Review
{
	// Compile-time protocol control: equal numeric IDs in Scene and Game cannot
	// alias, so the missing-dispatch safety gate recognizes the second view.
	static_assert(PrimaryReviewViewIdentity{ RenderOutputKind::SceneView, RenderViewType::Scene, 0 }
		!= PrimaryReviewViewIdentity{ RenderOutputKind::GameView, RenderViewType::Game, 0 });
	struct AnimatedDemandDrawFact
	{
		UUID objectId{};
		std::uint32_t ordinal = 0, meshlets = 0;
		bool active = false, failOpen = false, taskCulling = false;
		std::string fallbackReason;
	};
	struct AnimatedDemandViewFact
	{
		PrimaryReviewViewIdentity identity{};
		std::size_t drawCount = 0, clusterCount = 0, packetCount = 0;
		std::vector<AnimatedDemandDrawFact> draws;
		std::vector<std::uint32_t> selectedPageIds, packetPageIds;
	};
	struct AnimatedDemandRenderableFact
	{
		UUID objectId{};
		Renderer::AnimationRenderGenerationRecord animation{};
		bool schedulable = false, bindingReady = false;
	};
	struct AnimatedDemandProducerFacts
	{
		std::uint64_t frameId = 0;
		std::size_t renderableCount = 0, viewCount = 0;
		bool truncated = false;
		bool fixtureControlsApplied = false;
		std::uint64_t fixtureControlGeneration = 0;
		std::string fixtureControlReason;
		std::vector<AnimatedDemandRenderableFact> renderables;
		std::vector<AnimatedDemandViewFact> views;
	};
	struct RendererAnimatedDemandReviewAccess
	{
		static bool HasExactEmptyIntent(RenderSystem& renderer)
		{
			std::lock_guard lock(renderer.runtimeRenderSettingsMutex);
			const auto& i = renderer.capturedRendererEfficiencyFixtureIntent;
			// This fresh child may not displace another ingress owner. Empty means
			// the complete supported default value, not only an empty operation ID.
			return !renderer.pendingRendererEfficiencyFixtureIntent
				&& i.correlation == Renderer::RendererEfficiencyFixtureCorrelation{}
				&& i.holdFixedTick && i.sampleTick == 0 && i.injection == Renderer::RendererEfficiencyFixtureInjection::None
				&& !i.referenceOriginDelta && !i.shadowSelection && i.viewOrder.empty() && i.viewOrigins.empty()
				&& i.cameras.empty() && i.casterSelections.empty() && !i.lightingControls && !i.depthControls
				&& !i.shadowBatchControls && !i.staticShadowControls && i.controlGeneration == 0
				&& !i.controlsApplied && i.controlReason.empty();
		}
		// Copy at most 32 renderables, 8 views and 64 rows per view from the
		// protected published snapshot. No live scene, mesh blobs or plans escape.
		static AnimatedDemandProducerFacts Capture(RenderSystem& renderer)
		{
			std::lock_guard lock(renderer.statsMutex);
			const auto& snapshot = renderer.latestRenderFrameSnapshot;
			AnimatedDemandProducerFacts facts; facts.frameId = snapshot.frameId;
			facts.fixtureControlsApplied = snapshot.efficiencyFixtureIntent.controlsApplied;
			facts.fixtureControlGeneration = snapshot.efficiencyFixtureIntent.controlGeneration;
			facts.fixtureControlReason = snapshot.efficiencyFixtureIntent.controlReason.substr(0,256);
			facts.renderableCount = snapshot.renderables.size(); facts.viewCount = snapshot.virtualGeometryViewSelections.size();
			facts.truncated = facts.renderableCount > 32 || facts.viewCount > 8;
			for (std::size_t i = 0; i < std::min<std::size_t>(32, facts.renderableCount); ++i)
			{
				const auto& row = snapshot.renderables[i];
				facts.renderables.push_back({row.objectId, row.animation, Renderer::CanScheduleGpuAnimatedPageDeformation(row.animation), row.animationBinding.status == Renderer::AnimationRenderBindingStatus::Ready});
			}
			for (std::size_t i = 0; i < std::min<std::size_t>(8, facts.viewCount); ++i)
			{
				const auto& view = snapshot.virtualGeometryViewSelections[i];
				AnimatedDemandViewFact copied; copied.identity = {view.outputKind, view.viewType, view.outputId};
				copied.drawCount = view.DrawRecords().size(); copied.clusterCount = view.SelectedClusters().size(); copied.packetCount = view.GpuPacketRecords().size();
				facts.truncated |= copied.drawCount > 64 || copied.clusterCount > 64 || copied.packetCount > 64;
				for (std::size_t j = 0; j < std::min<std::size_t>(64, copied.drawCount); ++j)
				{
					const auto& draw = view.DrawRecords()[j];
					copied.draws.push_back({draw.objectId, draw.renderableIndex, draw.range.meshletCount, draw.range.virtualGeometryActive, draw.range.failOpen, draw.taskShaderCullingEnabled, draw.range.fallbackReason.substr(0,256)});
					facts.truncated |= draw.range.fallbackReason.size() > 256;
				}
				for (std::size_t j = 0; j < std::min<std::size_t>(64, copied.clusterCount); ++j) copied.selectedPageIds.push_back(view.SelectedClusters()[j].pageId);
				for (std::size_t j = 0; j < std::min<std::size_t>(64, copied.packetCount); ++j) copied.packetPageIds.push_back(view.GpuPacketRecords()[j].pageId);
				facts.views.push_back(std::move(copied));
			}
			return facts;
		}
	};
	struct VulkanDiscardedPrimaryReviewAccess
	{
		// Observe only the exact completed production frame. These are the real
		// frame-owned storage buffers, not replacement work packets or an oracle
		// dispatch. The small observer submission changes memory visibility only.
		static nlohmann::json ReadCompletedResults(VulkanContext& context, std::uint64_t frame,
			std::uint64_t productionFence, const UUID& mesh, const std::filesystem::path& artifacts)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
				throw TestFailure("Production result observation cannot submit an already-invalid native cohort.");
			std::size_t selected = state.animationFrameResources.size();
			for (std::size_t index = 0; index < state.animationFrameResources.size()
				&& index < state.deformationFrameResources.size(); ++index)
			{
				const auto& animation = state.animationFrameResources[index];
				const auto& deformation = state.deformationFrameResources[index];
				if (!animation.live || !deformation.live || animation.frameId != frame || deformation.frameId != frame) continue;
				if (selected != state.animationFrameResources.size()) throw TestFailure("Completed production frame has ambiguous resource owners.");
				selected = index;
			}
			if (selected == state.animationFrameResources.size()) throw TestFailure("Completed production frame resources are unavailable.");
			const auto& animation = state.animationFrameResources[selected];
			const auto& deformation = state.deformationFrameResources[selected];
			if (animation.rendererGeneration != deformation.rendererGeneration || !animation.submissionSerial
				|| !animation.recordedDispatchCount || deformation.workKeys.empty() || !productionFence
				|| deformation.workKeys.front().page.meshId != mesh
				|| deformation.workKeys.front().deformation.rendererGeneration != animation.rendererGeneration)
				throw TestFailure("Production result owners must match the submitted renderer/frame generation.");
			VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
			const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
			const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(state.instance, "vkGetDeviceProcAddr"));
			const auto createPool = reinterpret_cast<PFN_vkCreateCommandPool>(getDevice(state.device, "vkCreateCommandPool"));
			const auto destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(getDevice(state.device, "vkDestroyCommandPool"));
			const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(getDevice(state.device, "vkAllocateCommandBuffers"));
			const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(getDevice(state.device, "vkBeginCommandBuffer"));
			const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(getDevice(state.device, "vkCmdPipelineBarrier"));
			const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(getDevice(state.device, "vkEndCommandBuffer"));
			const auto createFence = reinterpret_cast<PFN_vkCreateFence>(getDevice(state.device, "vkCreateFence"));
			const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(getDevice(state.device, "vkDestroyFence"));
			const auto wait = reinterpret_cast<PFN_vkWaitForFences>(getDevice(state.device, "vkWaitForFences"));
			if (!createPool || !destroyPool || !allocate || !begin || !barrier || !end
				|| !createFence || !destroyFence || !wait)
				throw TestFailure("Actual device observer entry points are unavailable.");
			const auto families = state.FindQueueFamilies(state.physicalDevice);
			if (!families.graphicsFamily) throw TestFailure("Actual graphics queue family is unavailable.");
			struct ObserverOwner final
			{
				VkDevice device; PFN_vkDestroyCommandPool destroyPool; PFN_vkDestroyFence destroyFence;
				VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; bool pending = false;
				~ObserverOwner()
				{
					// An uncertain submission retains both owners until terminal device
					// teardown; exception cleanup must never free pending native work.
					if (pending) return;
					if (fence) destroyFence(device, fence, nullptr);
					if (pool) destroyPool(device, pool, nullptr);
				}
			} observer{state.device, destroyPool, destroyFence};
			const auto check = [](VkResult result) {
				if (result != VK_SUCCESS) throw TestFailure("Production result observer failed: " + std::to_string(result));
			};
			VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			poolInfo.queueFamilyIndex = *families.graphicsFamily;
			check(createPool(state.device, &poolInfo, nullptr, &observer.pool));
			VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocation.commandPool = observer.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
			VkCommandBuffer command = VK_NULL_HANDLE; check(allocate(state.device, &allocation, &command));
			VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; check(begin(command, &beginInfo));
			// Fence completion alone does not provide the device-to-host memory
			// dependency. This later command is ordered on the same protected queue.
			VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			visibility.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; visibility.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			barrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
				1, &visibility, 0, nullptr, 0, nullptr); check(end(command));
			if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
				throw TestFailure("Production host-visibility barrier failed validation before submission.");
			VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(createFence(state.device, &fenceInfo, nullptr, &observer.fence));
			VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submission.commandBufferCount = 1; submission.pCommandBuffers = &command;
			const auto quarantine = [&](VkResult result, const char* point) noexcept {
				// This runs only in the existing watched native child. An uncertain
				// submit cannot unwind through Engine shutdown or native owner cleanup.
				// Even diagnostic IO failure must preserve that fail-closed policy.
				try {
					std::ofstream receipt(artifacts / "production-result-quarantine.json");
					receipt << nlohmann::json{{"point",point},{"result",int(result)},{"frame",frame},
						{"context",reinterpret_cast<std::uintptr_t>(&context)},{"device",reinterpret_cast<std::uintptr_t>(state.device)},
						{"queue",reinterpret_cast<std::uintptr_t>(state.graphicsQueue)},{"command",reinterpret_cast<std::uintptr_t>(command)},
						{"fence",reinterpret_cast<std::uintptr_t>(observer.fence)},{"completionProven",false}}.dump(2);
					receipt.flush();
				} catch (...) {}
				std::_Exit(2);
			};
			observer.pending = true;
			VkResult submitted = VK_ERROR_UNKNOWN;
			// Use the production queue-wide gate as well as the device-state gate;
			// uploads and editor callbacks share this actual graphics queue.
			try { submitted = state.SubmitGraphicsQueue(1, &submission, observer.fence); }
			catch (...) { quarantine(VK_ERROR_UNKNOWN, "ObserverSubmitException"); }
			if (submitted != VK_SUCCESS) quarantine(submitted, "ObserverSubmitUncertain");
			const auto completed = wait(state.device, 1, &observer.fence, VK_TRUE, 10000000000ULL);
			if (completed != VK_SUCCESS) quarantine(completed, "ObserverCompletionUnproven");
			observer.pending = false;
			const auto read = [&](const VulkanBuffer& buffer, VkDeviceSize offset, std::size_t bytes, const char* name) {
				if (!buffer.buffer || !buffer.allocation || !buffer.mappedData || offset > buffer.size || bytes > buffer.size - offset)
					throw TestFailure("Production result slice exceeds its mapped owner.");
				VkMemoryPropertyFlags properties = 0; vmaGetAllocationMemoryProperties(state.allocator, buffer.allocation, &properties);
				if (!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) throw TestFailure("Production result allocation is not host visible.");
				check(vmaInvalidateAllocation(state.allocator, buffer.allocation, offset, bytes));
				std::vector<std::uint8_t> result(bytes); std::memcpy(result.data(), static_cast<const std::uint8_t*>(buffer.mappedData) + offset, bytes);
				std::ofstream output(artifacts / name, std::ios::binary); output.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()));
				output.flush();
				if (!output) throw TestFailure("Cannot persist actual completed production result bytes.");
				return result;
			};
			using Binding = Renderer::Vulkan::VulkanDeformationDescriptorBinding;
			const auto& buffers = deformation.buffers;
			const auto workBytes = read(buffers[static_cast<std::size_t>(Binding::Work)], 0, sizeof(Renderer::GpuDeformationShaderWorkRecord), "actual-work.bin");
			Renderer::GpuDeformationShaderWorkRecord work; std::memcpy(&work, workBytes.data(), sizeof(work));
			if (work.vertexCount == 0 || work.jointCount != 1 || work.morphTargetCount != 1 || work.generationRecordIndex != 0)
				throw TestFailure("Independent companion oracle requires its actual one-joint, one-morph first work record.");
			const auto generationBytes = read(buffers[static_cast<std::size_t>(Binding::Generations)], 0,
				sizeof(Renderer::GpuDeformationGenerationRecord), "actual-generation.bin");
			Renderer::GpuDeformationGenerationRecord generation; std::memcpy(&generation,generationBytes.data(),sizeof(generation));
			const auto& key = deformation.workKeys.front();
			if (generation.stableInstanceId != key.deformation.stableInstanceId
				|| generation.animationPublicationGeneration != key.deformation.animationPublicationGeneration
				|| generation.pageContentGeneration != key.page.pageContentGeneration || generation.residencyGeneration != key.page.residencyGeneration
				|| generation.rendererGeneration != key.deformation.rendererGeneration || generation.sceneGeneration != key.deformation.sceneGeneration
				|| generation.meshPayloadGeneration != key.deformation.meshPayloadGeneration)
				throw TestFailure("Actual generation upload must match the retained exact production work owner.");
			const auto paletteBytes = read(animation.buffers[static_cast<std::size_t>(Animation::GpuAnimationDescriptorBinding::PaletteMatrices)],
				VkDeviceSize(work.paletteRowOffset) * 16, sizeof(Animation::GpuAnimationPaletteRecord), "actual-palette.bin");
			const auto morphBytes = read(animation.buffers[static_cast<std::size_t>(Animation::GpuAnimationDescriptorBinding::MorphOutputs)],
				VkDeviceSize(work.morphWeightOffset) * sizeof(float), sizeof(float), "actual-morph.bin");
			const auto vertexBytes = read(buffers[static_cast<std::size_t>(Binding::CurrentVertices)],
				VkDeviceSize(work.currentOutputOffset) * sizeof(Renderer::GpuDeformedVertexRecord), sizeof(Renderer::GpuDeformedVertexRecord), "actual-current-vertex.bin");
			const auto packedBytes = read(buffers[static_cast<std::size_t>(Binding::StaticPageBytes)], work.inputVertexByteOffset, 16, "actual-packed-vertex.bin");
			const auto deltaBytes = read(buffers[static_cast<std::size_t>(Binding::StaticPageBytes)], work.morphDeltaByteOffset, 48, "actual-morph-delta.bin");
			const auto skinBytes = read(buffers[static_cast<std::size_t>(Binding::StaticPageBytes)],work.skinRecordByteOffset,8,"actual-skin.bin");
			const auto boundsBytes = read(buffers[static_cast<std::size_t>(Binding::MeshletBounds)], 0, sizeof(Renderer::GpuDynamicBoundsRecord), "actual-bounds.bin");
			Animation::GpuAnimationPaletteRecord palette; Renderer::GpuDeformedVertexRecord vertex; Renderer::GpuDynamicBoundsRecord bounds;
			float morph; std::array<std::uint32_t, 4> packed{}; std::array<float, 12> delta{};
			std::memcpy(&palette, paletteBytes.data(), sizeof(palette)); std::memcpy(&morph, morphBytes.data(), sizeof(morph));
			std::memcpy(&vertex, vertexBytes.data(), sizeof(vertex)); std::memcpy(&bounds, boundsBytes.data(), sizeof(bounds));
			std::memcpy(packed.data(), packedBytes.data(), 16); std::memcpy(delta.data(), deltaBytes.data(), 48);
			const auto close = [](float actual, float expected) { return std::isfinite(actual) && std::abs(actual - expected) <= 0.0002F; };
			std::array<std::uint32_t,2> skin{}; std::memcpy(skin.data(),skinBytes.data(),8);
			bool matches = close(morph, 0.25F) && skin[0] == 0 && skin[1] == 255;
			const std::array<std::array<float, 4>, 3> expectedPalette{{{1,0,0,0.125F},{0,1,0,0.25F},{0,0,1,0.5F}}};
			const std::array<std::array<float, 4>, 3> actualPalette{palette.row0,palette.row1,palette.row2};
			for (std::size_t row = 0; row < 3; ++row) for (std::size_t lane = 0; lane < 4; ++lane)
				matches &= close(actualPalette[row][lane], expectedPalette[row][lane]);
			// The installed baker authors a constant translation and constant morph.
			// Decode only the documented packed XYZ16 fields; do not call a CPU
			// animation/deformation evaluator to manufacture expected GPU output.
			const std::array<std::uint32_t, 3> codes{packed[0] & 65535U, packed[0] >> 16, packed[1] & 65535U};
			std::array<float, 3> expectedPosition{};
			for (std::size_t axis = 0; axis < 3; ++axis)
			{
				const float expectedDelta = axis == 2 ? 0.01F : 0.0F;
				matches &= close(delta[axis], expectedDelta);
				expectedPosition[axis] = work.positionOffset[axis] + (float(codes[axis]) / 65535.0F * 2.0F - 1.0F)
					* work.positionScale[axis] + expectedDelta * 0.25F + expectedPalette[axis][3];
				matches &= close(vertex.currentPosition[axis], expectedPosition[axis])
					&& std::isfinite(bounds.minimum[axis]) && std::isfinite(bounds.maximum[axis])
					&& bounds.minimum[axis] <= vertex.currentPosition[axis] + 0.0002F
					&& bounds.maximum[axis] >= vertex.currentPosition[axis] - 0.0002F;
			}
			matches &= (bounds.identity[3] & 13U) == 13U && bounds.identity[0] == work.reserved[0];
			return {{"scope","ActualCompletedProductionAnimationAndDeformationSlices"},{"frame",frame},{"frameSlot",selected},
				{"rendererGeneration",animation.rendererGeneration},{"planGeneration",animation.planGeneration},{"submissionSerial",animation.submissionSerial},
				{"productionFence",productionFence},{"context",reinterpret_cast<std::uintptr_t>(&context)},
				{"device",reinterpret_cast<std::uintptr_t>(state.device)},{"queue",reinterpret_cast<std::uintptr_t>(state.graphicsQueue)},
				{"rendererSession",state.renderSessionGeneration},{"backendGeneration",state.backendResourceGeneration},
				{"stableInstanceId",generation.stableInstanceId},{"animationPublicationGeneration",generation.animationPublicationGeneration},
				{"meshPayloadGeneration",generation.meshPayloadGeneration},{"pageId",key.page.pageId},
				{"hostBarrierSubmittedAndCompleted",true},{"observerFence",reinterpret_cast<std::uintptr_t>(observer.fence)},
				{"actualPalette",actualPalette},{"actualMorph",morph},{"actualPosition",vertex.currentPosition},{"independentPosition",expectedPosition},
				{"boundsIdentity",bounds.identity},{"independentOraclePassed",matches}};
		}
		// Only a real successful production submit supplies this fence. A bounded
		// native wait qualifies completion; timeout remains failure in this child.
		static bool WaitSubmittedFence(VulkanContext& context, const PrimaryReviewEvent& submitted,
			const std::filesystem::path& artifacts)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
			const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
			const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(state.instance, "vkGetDeviceProcAddr"));
			const auto wait = reinterpret_cast<PFN_vkWaitForFences>(getDevice(state.device, "vkWaitForFences"));
			const auto actual = reinterpret_cast<VkFence>(std::uintptr_t(submitted.fence));
			const auto result = wait && actual ? wait(state.device, 1, &actual, VK_TRUE, 10000000000ULL) : VK_ERROR_INITIALIZATION_FAILED;
			if (result != VK_SUCCESS)
			{
				// A genuinely accepted production submission needs the same no-unwind
				// quarantine as the small observer command if completion is unproven.
				try {
					std::ofstream receipt(artifacts / "production-fence-quarantine.json");
					receipt << nlohmann::json{{"point","ProductionCompletionUnproven"},{"result",int(result)},
						{"frame",submitted.frame},{"command",submitted.command},{"fence",submitted.fence},
						{"context",reinterpret_cast<std::uintptr_t>(&context)},{"device",reinterpret_cast<std::uintptr_t>(state.device)},
						{"queue",reinterpret_cast<std::uintptr_t>(state.graphicsQueue)},{"completionProven",false}}.dump(2);
					receipt.flush();
				} catch (...) {}
				std::_Exit(2);
			}
			return true;
		}
	};
}
namespace TerraEngine::Tests
{
	void RegisterDiscardedPrimaryRecoveryTests(TestRegistry& registry)
	{
		using namespace Review;
		if (!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		registry.Add("Native.ReviewSimulation.Lifecycle.DiscardedPrimaryRerecordsCompute", [](TestContext& context) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.DiscardedPrimaryRerecordsCompute";
			if (!VulkanReviewFixture::IsWatchedChild(name))
			{
				const auto child = VulkanReviewFixture::RunWatchedSelf(name, std::chrono::seconds(120));
			Review::RecordNativeReviewChild(name, child);
				context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Actual discarded-primary child must settle: " + child.output); return;
			}
			ExistingRendererFixtureSession session(name);
			// The maintained build installs this coherent double-baked cohort beside
			// the runner. Never silently fall back to another checkout/build's mesh.
			const auto cohort = std::filesystem::path(PlatformUtils::GetExecutablePath()).parent_path();
			const auto base = cohort / "TerraEngineProductionShapeFixture";
			std::uint64_t fingerprint = 0; std::uint32_t joints = 0; std::string key, skeletonFile, clipFile;
			std::ifstream identity(base.string()+".identity");
			while (identity >> key)
			{
				if (key == "skeletonCompatibilityFingerprint") identity >> fingerprint;
				else if (key == "sourceSkeletonJointCount") identity >> joints;
				else if (key == "skeleton") identity >> skeletonFile;
				else if (key == "clip") identity >> clipFile;
				else throw std::runtime_error("Unknown actual baked identity field.");
			}
			context.Expect(fingerprint && joints == 1 && !skeletonFile.empty() && !clipFile.empty()
				&& std::filesystem::is_regular_file(base.string()+".manifest"), "Actual coherent cooked mesh/skeleton/clip identity + manifest must be installed beside runner.");
			auto& resources = ResourceManager::Instance();
			const auto mesh = resources.LoadDeformableMesh(base.string()+".temf", joints, fingerprint);
			const auto skeleton = resources.LoadSkeleton(cohort/skeletonFile, fingerprint);
			const auto clip = resources.LoadAnimationClip(cohort/clipFile, fingerprint);
			const auto resident = resources.AcquireMeshSnapshot(mesh);
			context.Expect(mesh != EMPTY_UUID && skeleton != EMPTY_UUID && clip != EMPTY_UUID && resident
				&& resident->hasDeformableDemandedPages && !resident->deformableDemandedPageEntries.empty(), "Actual public loader must publish coherent immutable demanded-page residency.");
			const auto scenePath = session.Artifacts()/"Assets/Scenes/primary.tescene";
			std::ofstream(scenePath) << R"({"format":"TerraSceneUnboundedV1","objects":[]})";
			context.Expect(SceneManager::Instance().SetActiveScene(SceneManager::Instance().LoadScene(scenePath)), "Owned real scene must activate before protected object authoring.");
			auto& objects = ObjectManager::instance(); auto object = objects.createObject("Real cooked discarded-prefix owner");
			auto* render = objects.AddComponent<MeshRenderComponent>(object);
			render->meshId = mesh; render->materialId = resources.RegisterMaterial("discarded-prefix-material", TerraEngine::Terrain::CreateTerrainMaterial(resources, {}));
			// Use the supported normal VG selector path. Disabling this component
			// explicitly returns an inactive highest-detail fallback before clusters.
			const bool priorComponentTaskCulling = render->taskShaderCullingEnabled;
			render->taskShaderCullingEnabled = true;
			context.Expect(render->materialId != EMPTY_UUID, "Actual generated opaque PBR material must publish.");
			auto* animator = objects.AddComponent<AnimatorComponent>(object);
			animator->defaultClipAssetId = clip; animator->skeletonOverrideAssetId = skeleton;
			animator->requestedMode = Animation::AnimationExecutionPolicy::ForceGpuVisual;
			animator->rootMotionPolicy = Animation::AnimatorRootMotionPolicy::Ignore;
			animator->eventPolicy = Animation::AnimatorEventPolicy::Suppress;
			Animation::AnimationSystem::Instance().SetAuthoredComponentPlaybackEnabled(true);
			Animation::AnimationSystem::Instance().SetAuthoredComponentPlaybackAdvancing(true);
			auto& renderer = RenderSystem::Instance();
			struct RestoreCullingSettings final
			{
				RenderSystem& renderer; ObjectManager& objects; Object object;
				bool priorGlobal = true, priorComponent = true, active = true, restored = false;
				bool Restore()
				{
					if (!active) return restored;
					active = false; renderer.SetTaskShaderCullingEnabled(priorGlobal);
					auto* component = objects.GetComponent<MeshRenderComponent>(object);
					if (component) component->taskShaderCullingEnabled = priorComponent;
					restored = component && component->taskShaderCullingEnabled == priorComponent
						&& renderer.IsTaskShaderCullingEnabled() == priorGlobal;
					return restored;
				}
				~RestoreCullingSettings() noexcept { try { Restore(); } catch (...) {} }
			} cullingLease{renderer, objects, object, renderer.IsTaskShaderCullingEnabled(), priorComponentTaskCulling};
			renderer.SetTaskShaderCullingEnabled(true);
			const auto priorFixtureIntent = renderer.GetRendererEfficiencyFixtureControlSnapshot();
			// This existing typed validation API requests real route selection. It
			// rejects unsupported adapters; it never authors a plan or capability.
			// RendererEfficiencyFixtureIntent is a separate API and stays untouched.
			struct RestorePriorRoute final
			{
				RenderSystem& renderer;
				std::optional<Renderer::VirtualGeometryRasterRoute> prior;
				bool priorHybrid = false, changed = false, restored = false;
				std::string reason;
				bool Restore()
				{
					if (!changed) return restored;
					changed = false;
					if (prior) restored = renderer.SetVirtualGeometryValidationRouteOverride(*prior, &reason);
					else { renderer.SetVirtualGeometryHybridValidationSceneRequested(priorHybrid); restored = true; }
					restored = restored && renderer.GetVirtualGeometryValidationRouteOverride() == prior
						&& renderer.IsVirtualGeometryHybridValidationSceneRequested() == priorHybrid;
					return restored;
				}
				~RestorePriorRoute() noexcept { try { Restore(); } catch (...) {} }
			} routeLease{renderer, renderer.GetVirtualGeometryValidationRouteOverride(), renderer.IsVirtualGeometryHybridValidationSceneRequested()};
			std::string requestedRouteReason;
			const bool hardwareRouteAccepted = renderer.SetVirtualGeometryValidationRouteOverride(
				Renderer::VirtualGeometryRasterRoute::Hardware, &requestedRouteReason);
			routeLease.changed = hardwareRouteAccepted;
			context.Expect(hardwareRouteAccepted, "Actual adapter must admit the existing Hardware validation route: " + requestedRouteReason);
			renderer.SetSceneViewTargetSize({96,96}); renderer.SetGameViewTargetSize({96,96});
			// Legacy matrices are relative to their canonical camera origin. Rotation
			// is identity for this real camera at z4 looking toward the mesh at0.
			const glm::mat4 view(1.0f);
			auto projection = glm::perspective(glm::radians(60.0f), 1.0f, 0.01f, 100.0f); projection[1][1] *= -1.0f;
			Scene::WorldPosition camera; camera.offsetMetres = {0,0,4};
			const bool priorExactEmptyIntent = RendererAnimatedDemandReviewAccess::HasExactEmptyIntent(renderer);
			context.Expect(priorExactEmptyIntent, "Fresh watched child requires an exact empty captured/pending fixture intent; another owner is never displaced.");
			// A normal warmup binds the actual renderer/settings generation. No
			// fault hook is installed and no synthetic publication is constructed.
			SetDiscardedPrimaryReviewHook(nullptr);
			renderer.SetSceneViewCamera(view, projection, camera); renderer.SetGameViewCamera(view, projection, camera);
			const bool warmupCompleted = session.Update(1.0f/60.0f);
			const auto boundSession = renderer.GetRendererEfficiencyOptionsSnapshot();
			context.Expect(warmupCompleted && boundSession.rendererSessionGeneration != 0, "Normal warmup must bind a real renderer session before fixture ingress.");
			Renderer::RendererEfficiencyFixtureIntent fixedIntent;
			fixedIntent.holdFixedTick = false;
			fixedIntent.correlation.operationId = name + ".FixedReferenceOrigin";
			fixedIntent.correlation.fixtureStepId = "ActualSpatialQueryAnimationBinding";
			fixedIntent.correlation.stepFrame = 1;
			fixedIntent.correlation.settingsGeneration = boundSession.settingsGeneration;
			fixedIntent.correlation.rendererSessionGeneration = boundSession.rendererSessionGeneration;
			for (const auto [type, id] : {std::pair{RenderViewType::Scene,1U}, std::pair{RenderViewType::Game,2U}})
			{
				Renderer::RendererEfficiencyFixtureCamera source;
				source.viewType = type; source.outputId = id; source.cameraPosition = camera;
				source.lookAtTarget = {}; source.referenceOrigin = {};
				source.nearPlane = 0.01; source.farPlane = 100; source.verticalFovDegrees = 60; source.nativeExtent = {96,96};
				fixedIntent.cameras.push_back(source);
			}
			struct RestoreEmptyIntent final
			{
				RenderSystem& renderer; std::string operation; bool active = false, restored = false;
				bool Restore()
				{
					if (!active) return restored;
					renderer.ClearRendererEfficiencyFixtureIntent(operation); active = false;
					restored = RendererAnimatedDemandReviewAccess::HasExactEmptyIntent(renderer);
					return restored;
				}
				~RestoreEmptyIntent() noexcept { try { Restore(); } catch (...) {} }
			} intentLease{renderer,fixedIntent.correlation.operationId};
			const auto intentReceipt = renderer.QueueRendererEfficiencyFixtureIntent(fixedIntent);
			intentLease.active = intentReceipt.accepted;
			context.Expect(intentReceipt.accepted, "Actual supported fixed-camera ingress must accept real bound generation: " + intentReceipt.reason);
			DiscardedPrimaryReviewHook hook;
			context.Expect(SetDiscardedPrimaryReviewHook(&hook), "Exact actual-primary observation seam must be enabled.");
			struct Disarm { ~Disarm() { SetDiscardedPrimaryReviewHook(nullptr); } } disarm;
			// Normal Engine.Update owns coordinator sync, immutable publication,
			// generation sealing, actual upload/recording and the queue submission.
			for (unsigned attempt = 0; attempt < 8 && hook.refusals == 0; ++attempt)
			{
				hook.count = 0; hook.overflow = false;
				renderer.SetSceneViewCamera(view, projection, camera); renderer.SetGameViewCamera(view, projection, camera);
				if (!session.Update(1.0f/60.0f)) break;
			}
			SetDiscardedPrimaryReviewHook(nullptr);
			nlohmann::json rows = nlohmann::json::array();
			std::optional<PrimaryReviewEvent> refused, reset, begun, admitted, submitted;
			bool animationRerecorded = false, deformationRerecorded = false;
			for (std::uint32_t i = 0; i < hook.count; ++i)
			{
				const auto& event = hook.events[i];
				rows.push_back({{"point",int(event.point)},{"frame",event.frame},{"command",event.command},{"fence",event.fence},{"view",{{"outputKind",int(event.view.outputKind)},{"viewType",int(event.view.viewType)},{"outputId",event.view.outputId}}},
					{"animationDispatches",event.animationDispatches},{"deformationDispatches",event.deformationDispatches},{"result",event.result}});
				if (!refused && hook.refusals == 1 && i == hook.refusedEventIndex && event.point == PrimaryReviewPoint::LightingRefusal && event.animationDispatches && event.deformationDispatches) refused = event;
				else if (refused && event.frame == refused->frame && event.view != refused->view)
				{
					if (event.point == PrimaryReviewPoint::Reset && event.result == VK_SUCCESS) reset = event;
					if (event.point == PrimaryReviewPoint::Begin && event.result == VK_SUCCESS)
					{
						begun = event; animationRerecorded = false; deformationRerecorded = false;
					}
					if (begun && event.command == begun->command && event.view == begun->view)
					{
						if (event.point == PrimaryReviewPoint::AnimationRecorded && event.animationDispatches) animationRerecorded = true;
						if (event.point == PrimaryReviewPoint::DeformationRecorded && event.deformationDispatches) deformationRerecorded = true;
						if (event.point == PrimaryReviewPoint::Admitted) admitted = event;
					}
				}
				if (admitted && event.point == PrimaryReviewPoint::Submit && event.command == admitted->command && event.frame == admitted->frame && event.result == VK_SUCCESS) submitted = event;
			}
			const bool completed = submitted && VulkanDiscardedPrimaryReviewAccess::WaitSubmittedFence(session.Native(), *submitted, session.Artifacts());
			nlohmann::json gpuResult{{"independentOraclePassed",false}};
			if (completed) gpuResult = VulkanDiscardedPrimaryReviewAccess::ReadCompletedResults(session.Native(), submitted->frame, submitted->fence, mesh, session.Artifacts());
			const auto actualDemandCount = renderer.GetLatestAnimatedGeometryPageDemandCount();
			const auto producer = RendererAnimatedDemandReviewAccess::Capture(renderer);
			const auto actualAnimatedVisibility = Renderer::LoadAnimatedObjectVisibilitySnapshot();
			const bool actualObjectPublishedVisible = actualAnimatedVisibility && actualAnimatedVisibility->Contains(objects.getUUID(object));
			nlohmann::json producerBindings = nlohmann::json::array(), producerViews = nlohmann::json::array(), publicViews = nlohmann::json::array(), pageSections = nlohmann::json::array();
			for (const auto& row : producer.renderables)
			{
				const auto& a = row.animation;
				producerBindings.push_back({{"objectId",ToHexString(row.objectId)},{"isComplete",a.IsComplete()},{"canScheduleGpuDeformation",row.schedulable},{"bindingStatusReady",row.bindingReady},
					{"source",int(a.source)},{"stableInstanceId",a.stableInstanceId},{"rendererGeneration",a.rendererGeneration},{"payloadGeneration",a.deformablePayloadGeneration},
					{"programGeneration",a.programGeneration},{"poseGeneration",a.poseGeneration},{"morphGeneration",a.morphGeneration},{"paletteGeneration",a.paletteGeneration},
					{"planGeneration",a.animationPlanGeneration},{"gpuStageMask",a.gpuStageMask},{"contentIdLow",a.deformationContentIdLow},{"contentIdHigh",a.deformationContentIdHigh}});
			}
			for (const auto& view : producer.views)
			{
				nlohmann::json draws = nlohmann::json::array();
				for (const auto& draw : view.draws)
					draws.push_back({{"objectId",ToHexString(draw.objectId)},{"renderableIndex",draw.ordinal},{"meshletCount",draw.meshlets},{"active",draw.active},{"failOpen",draw.failOpen},{"taskShaderCullingEnabled",draw.taskCulling},{"fallbackReason",draw.fallbackReason}});
				producerViews.push_back({{"outputKind",int(view.identity.outputKind)},{"viewType",int(view.identity.viewType)},{"outputId",view.identity.outputId},
					{"drawCount",view.drawCount},{"clusterCount",view.clusterCount},{"packetCount",view.packetCount},{"draws",draws},{"selectedPageIds",view.selectedPageIds},{"packetPageIds",view.packetPageIds}});
			}
			for (const auto type : {RenderViewType::Scene, RenderViewType::Game})
			{
				const auto stats = renderer.GetLatestRenderStats(type); const auto spatial = renderer.GetLatestSpatialDiagnostics(type);
				publicViews.push_back({{"viewType",int(type)},{"statsPublicationFrame",stats.publicationFrameId},{"drawCalls",stats.drawCalls},{"triangles",stats.triangleCount},
					{"meshletsTotal",stats.meshletsTotal},{"meshletsDispatched",stats.meshletsDispatched},{"meshletsCulled",stats.meshletsCulled},
					{"spatialCandidates",spatial.visibleCandidateCount},{"spatialRejected",spatial.rejectedObjectCount},{"spatialFallback",spatial.fallbackObjectCount},
					{"publicVisibilityPacketCount",renderer.GetLatestVirtualGeometryVisibilityRecords(type).size()}});
			}
			const bool sectionRowsTruncated = resident->deformableDemandedPageEntries.size() > 64;
			for (std::size_t i = 0; i < std::min<std::size_t>(64, resident->deformableDemandedPageEntries.size()); ++i)
			{
				nlohmann::json sections = nlohmann::json::array(); const auto& row = resident->deformableDemandedPageSections.at(i);
				for (std::size_t j = 0; j < std::min<std::size_t>(16, row.size()); ++j) sections.push_back({{"kind",int(row[j].kind)},{"elements",row[j].elementCount}});
				pageSections.push_back({{"pageId",resident->deformableDemandedPageEntries[i].pageId},{"sectionCount",row.size()},{"sectionsTruncated",row.size() > 16},{"sections",sections}});
			}
			nlohmann::json actualBindings = nlohmann::json::array(); bool actualHardwareSelection = false;
			for (const auto& renderable : renderer.GetLatestRenderablesSnapshot())
			{
				actualHardwareSelection |= renderable.geometryRouteDecision.selected == Renderer::GeometryRoute::VirtualGeometryHardware;
				actualBindings.push_back({{"complete",renderable.animation.complete},{"safeFallback",renderable.animation.safeFallback},
					{"gpuWorkPacketCount",renderable.animation.gpuWorkPacketCount},
					{"actualSelectedRoute",int(renderable.geometryRouteDecision.selected)},
					{"actualRouteFallbackReason",renderable.geometryRouteDecision.fallbackReason}});
			}
			// Restore the exact prior route namespace while this actual renderer
			// lifetime is still alive. The destructor covers early test exceptions.
			const auto actualConfiguredIntent = renderer.GetRendererEfficiencyFixtureControlSnapshot();
			const bool priorExactIntentRestored = intentLease.Restore();
			const bool actualGlobalTaskCulling = renderer.IsTaskShaderCullingEnabled();
			const bool priorCullingSettingsRestored = cullingLease.Restore();
			const bool priorRouteRestored = routeLease.Restore();
			const auto afterFixtureIntent = renderer.GetRendererEfficiencyFixtureControlSnapshot();
			const bool intentIdentityUnchanged = priorFixtureIntent.correlation == afterFixtureIntent.correlation
				&& priorFixtureIntent.controlGeneration == afterFixtureIntent.controlGeneration;
			session.CloseRenderer();
			const auto cameraRows = [](const auto& values) {
				nlohmann::json rows = nlohmann::json::array();
				for (const auto& c : values)
					rows.push_back({{"viewType",int(c.viewType)},{"outputId",c.outputId},
						{"cameraCell",{c.cameraPosition.cell.x,c.cameraPosition.cell.y,c.cameraPosition.cell.z}},
						{"targetCell",{c.lookAtTarget.cell.x,c.lookAtTarget.cell.y,c.lookAtTarget.cell.z}},
						{"referenceCell",{c.referenceOrigin.cell.x,c.referenceOrigin.cell.y,c.referenceOrigin.cell.z}},
						{"cameraOffset",{c.cameraPosition.offsetMetres.x,c.cameraPosition.offsetMetres.y,c.cameraPosition.offsetMetres.z}},
						{"targetOffset",{c.lookAtTarget.offsetMetres.x,c.lookAtTarget.offsetMetres.y,c.lookAtTarget.offsetMetres.z}},
						{"referenceOffset",{c.referenceOrigin.offsetMetres.x,c.referenceOrigin.offsetMetres.y,c.referenceOrigin.offsetMetres.z}},
						{"up",{c.up.x,c.up.y,c.up.z}},{"fovDegrees",c.verticalFovDegrees},{"nearPlane",c.nearPlane},{"farPlane",c.farPlane},
						{"extent",{c.nativeExtent.x,c.nativeExtent.y}}});
				return rows;
			};
			std::ifstream manifestInput(base.string()+".manifest"); const std::string manifest((std::istreambuf_iterator<char>(manifestInput)),{});
			std::ofstream evidence(session.Artifacts()/"discarded-primary.json");
			evidence << nlohmann::json{{"scope","ActualCookedAuthoredPlayback_TwoViews_ProductionPrimary"},{"primaryReviewProtocolVersion",2},{"refusedEventIndex",hook.refusedEventIndex},{"laterPrimaryBeginObserved",bool(begun)},{"laterPrimaryResetObserved",bool(reset)},
				
				
				
				{"priorExactEmptyIntent",priorExactEmptyIntent},{"priorExactIntentRestored",priorExactIntentRestored},{"warmupCompletedWithoutFaultHook",warmupCompleted},
				{"warmupRendererSessionGeneration",boundSession.rendererSessionGeneration},{"warmupSettingsGeneration",boundSession.settingsGeneration},
				{"configuredIntentOperation",fixedIntent.correlation.operationId},{"configuredIntentStep",fixedIntent.correlation.fixtureStepId},
				{"configuredIntentControlGeneration",intentReceipt.controlGeneration},{"configuredHoldFixedTick",fixedIntent.holdFixedTick},
				{"actualConfiguredOperation",actualConfiguredIntent.correlation.operationId},{"actualConfiguredStep",actualConfiguredIntent.correlation.fixtureStepId},
				{"actualConfiguredStepFrame",actualConfiguredIntent.correlation.stepFrame},{"actualConfiguredMutationGeneration",actualConfiguredIntent.correlation.mutationGeneration},
				{"actualConfiguredSettingsGeneration",actualConfiguredIntent.correlation.settingsGeneration},{"actualConfiguredRendererSessionGeneration",actualConfiguredIntent.correlation.rendererSessionGeneration},
				{"restoredIntentControlGeneration",afterFixtureIntent.controlGeneration},{"configuredFixedCameras",cameraRows(fixedIntent.cameras)},
				{"actualCapturedFixedCameras",cameraRows(actualConfiguredIntent.cameras)},
				{"producerFixtureControlsApplied",producer.fixtureControlsApplied},{"producerFixtureControlGeneration",producer.fixtureControlGeneration},{"producerFixtureControlReason",producer.fixtureControlReason},
				{"configuredGlobalTaskCullingEnabled",actualGlobalTaskCulling},{"priorGlobalTaskCullingEnabled",cullingLease.priorGlobal},
				{"priorComponentTaskCullingEnabled",cullingLease.priorComponent},{"priorCullingSettingsRestored",priorCullingSettingsRestored},
				{"hardwareRouteRequestAccepted",hardwareRouteAccepted},{"hardwareRouteRequestReason",requestedRouteReason},
				{"actualHardwareSelection",actualHardwareSelection},{"priorRouteRestored",priorRouteRestored},
				{"priorRouteRestoreReason",routeLease.reason},{"fixtureIntentIdentityRestored",intentIdentityUnchanged},
				
				
				{"animatedVisibilityFrame",actualAnimatedVisibility ? actualAnimatedVisibility->frameId : 0},
				{"animatedVisibilityValid",actualAnimatedVisibility && actualAnimatedVisibility->valid},
				{"animatedVisibilityHasActiveView",actualAnimatedVisibility && actualAnimatedVisibility->hasActiveView},
				{"fixtureObjectPublishedVisible",actualObjectPublishedVisible},
				{"demandProducerFrame",producer.frameId},{"demandProducerTruncated",producer.truncated},{"demandProducerRenderableCount",producer.renderableCount},{"demandProducerViewCount",producer.viewCount},
				{"demandProducerBindings",producerBindings},{"demandProducerViews",producerViews},{"completionDelayedPublicViewStats",publicViews},
				{"immutablePageSectionRows",pageSections},{"pageSectionRowsTruncated",sectionRowsTruncated},
				{"actualPageDemandCount",actualDemandCount},{"actualAnimationBindings",actualBindings},
				{"meshHasDemandedPages",resident->hasDeformableDemandedPages},{"meshPageStreamingReady",resident->virtualGeometryPageStreamingReady},
				{"meshHasVirtualGeometry",resident->hasVirtualGeometry},{"meshOwnerFallbackReason",resident->virtualGeometryOwnerFallbackReason},
				{"fixtureManifest",manifest},{"fingerprint",fingerprint},{"sourceJointCount",joints},{"faultReached",hook.refusals},{"overflow",hook.overflow},{"unsafeDependentViewBlockedBeforeAdmission",hook.unsafeDependentViewBlocked},
				{"events",rows},{"animationActuallyRerecorded",animationRerecorded},{"deformationActuallyRerecorded",deformationRerecorded},
				{"submittedFenceCompleted",completed},{"actualProductionGpuResult",gpuResult},{"allLifetimeValidationErrors",session.ValidationErrors()}}.dump(2);
			evidence.flush(); context.Expect(static_cast<bool>(evidence), "Actual production dispatch, raw-result and retirement evidence must persist completely.");
			context.Expect(priorExactIntentRestored && priorRouteRestored && priorCullingSettingsRestored && intentIdentityUnchanged, "Route, culling and the exact prior supported fixture intent must be restored before teardown.");
			context.Expect(producer.fixtureControlsApplied && producer.fixtureControlGeneration == intentReceipt.controlGeneration
				&& actualConfiguredIntent.cameras == fixedIntent.cameras && actualConfiguredIntent.correlation == fixedIntent.correlation, "Protected capture must apply the actual queued camera/reference-origin recipe before producer qualification.");
			context.Expect(actualHardwareSelection && actualDemandCount != 0, "The real protected producer must select supported hardware VG and publish actual page demand; override acceptance alone is insufficient.");
			context.Expect(hook.refusals == 1 && refused && !hook.overflow, "A complete real animation+deformation prefix must reach exactly one later lighting refusal.");
			context.Expect(animationRerecorded && deformationRerecorded && hook.unsafeDependentViewBlocked == 0, "Both actual production recorders must emit current compute again after the discarded prefix; the safety guard blocks unsafe dependent submission before a normal pre-fix failure.");
			context.Expect(begun && admitted && submitted && completed, "The later same-frame view must actually begin its primary, admit and submit re-recorded compute to a completed real fence; either a reused or fresh command is valid.");
			context.Expect(gpuResult.at("independentOraclePassed").get<bool>(), "Completed production palette, morph, deformed vertex and bounds must match the independent installed companion oracle.");
			context.Expect(session.ValidationErrors() == 0, "All actual frame and teardown Vulkan validation must remain clean.");
		});
	}
}
