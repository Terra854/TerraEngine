// Actual default uploads and ordinary view failure, one watched fault per
// real renderer lifetime. Caller ambiguity is injected after a proven fence;
// no genuinely in-flight resource is freed to manufacture a pre-fix failure.
#include "TestHarness.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include "Tests/Support/ReviewRemediation/ExistingRendererFixtureSession.h"
#include "Tests/Support/ReviewRemediation/TextureUploadFailureHooks.h"
#include "Tests/Support/ReviewRemediation/AnimationUploadProductionProbe.h"
#include "Tests/Engine/Resource/TextureTestFixtures.h"
// Impl contains validation-gated fields before every native owner. This TU
// must use the Engine's matching validation ABI; otherwise even a read is unsafe.
#if !defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION) || !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
#error Native review Impl access requires the matching Engine validation ABI.
#endif
#include "Renderer/RHI/Vulkan/VulkanContextImpl.h"
#include <fstream>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests::Review
{
	struct TextureViewFailureObservation
	{
		std::uint32_t allocationsBefore = 0, allocationsAfter = 0;
		bool exceptionObserved = false, observerOnlyReclaimedLeakedImage = false, subsequentViewCreated = false;
	};
	struct VulkanTextureReviewAccess
	{
		// Consume the actual protected publication before observing any owner.
		// No pending-plan flag, descriptor or packed upload is installed by this
		// test. The recorder below consumes the same production resources as views.
		static AnimationUploadProductionObservation ExerciseAnimation(VulkanContext& context,
			const Renderer::RenderBackendWorkPublication& publication, const std::filesystem::path& artifacts)
		{
			using namespace Renderer;
			using namespace Renderer::Vulkan;
			if (!publication.animationWork.cpuDeformationPlan || !publication.deformationWork)
				throw TestFailure("Production A/A/B probe requires owned CPU and deformation plans.");
			const auto& supplied = *publication.animationWork.cpuDeformationPlan;
			if (supplied.records.size() != 3) throw TestFailure("Production A/A/B probe requires exactly three input owners.");
			for (std::size_t index = 0; index < 3; ++index)
				if (!supplied.records[index].data || supplied.records[index].stableInstanceId != index+1
					|| supplied.records[index].data->paletteMatrices.size() != (index < 2 ? 1 : 2)
					|| supplied.records[index].data->morphWeights.size() != (index < 2 ? 1 : 2))
					throw TestFailure("Production A/A/B probe requires the bounded authored palette/morph recipe.");
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
				throw TestFailure("Production A/A/B cannot record an already-invalid native cohort.");
			AnimationUploadProductionObservation result;
			result.published = context.ConsumePublishedWorkPlans(publication);
			if (!result.published) throw TestFailure("Real CPU animation/deformation publication rejected the coherent A/A/B plan.");
			const auto slot = state.currentFrame;
			if (slot >= state.cpuAnimationFrameResources.size() || slot >= state.deformationFrameResources.size()
				|| slot >= state.deformationDescriptorSets.size() || !state.pendingDeformationImmutablePlan)
				throw TestFailure("Published frame owners are unavailable.");
			const auto& cpu = state.cpuAnimationFrameResources[slot];
			const auto& deformation = state.deformationFrameResources[slot];
			const auto& plan = *state.pendingDeformationImmutablePlan;
			const auto& input = *publication.animationWork.cpuDeformationPlan;
			if (!cpu.live || !deformation.live || cpu.frameId != publication.frameId
				|| deformation.frameId != publication.frameId || cpu.rendererGeneration != input.rendererGeneration
				|| deformation.rendererGeneration != input.rendererGeneration || input.records.size() != 3 || plan.work.size() != 2)
				throw TestFailure("A/A/B must own one current frame and exactly two reusable production work records.");
			result.workCount = static_cast<std::uint32_t>(plan.work.size());
			nlohmann::json receipt{{"scope","ActualProductionCpuUploadAndDeformationDispatch"},
				{"context",reinterpret_cast<std::uintptr_t>(&context)},{"device",reinterpret_cast<std::uintptr_t>(state.device)},
				{"queue",reinterpret_cast<std::uintptr_t>(state.graphicsQueue)},
				{"physicalDevice",reinterpret_cast<std::uintptr_t>(state.physicalDevice)},{"frame",publication.frameId},
				{"descriptorSet",reinterpret_cast<std::uintptr_t>(state.deformationDescriptorSets[slot])},
				{"slot",slot},{"rendererGeneration",input.rendererGeneration},{"published",true},
				{"completed",false},{"allocations",nlohmann::json::array()},{"work",nlohmann::json::array()},
				{"bridges",nlohmann::json::array()}};
			const auto persist = [&]() {
				std::ofstream output(artifacts / "production-aab.json"); output << receipt.dump(2); output.flush();
				if (!output) throw TestFailure("Cannot retain production A/A/B owner receipt.");
			};
			// A fresh frame has only host-written inputs here. Read actual mapped
			// allocations to reject an unsafe offset BEFORE recording GPU work.
			// After completion the same helper invalidates device-written results.
			const auto read = [&](const VulkanBuffer& buffer, VkDeviceSize offset, std::size_t bytes, const char* name) {
				VmaAllocationInfo info{}; VkMemoryPropertyFlags properties = 0;
				if (!buffer.buffer || !buffer.allocation || !buffer.mappedData || !bytes
					|| offset > buffer.size || bytes > buffer.size - offset)
					throw TestFailure("Actual production slice is outside its mapped buffer.");
				vmaGetAllocationInfo(state.allocator, buffer.allocation, &info);
				vmaGetAllocationMemoryProperties(state.allocator, buffer.allocation, &properties);
				if (!(properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || buffer.size > info.size
					|| offset > info.size || bytes > info.size - offset)
					throw TestFailure("Actual production slice lacks bounded HOST_VISIBLE allocation ownership.");
				if (vmaInvalidateAllocation(state.allocator, buffer.allocation, offset, bytes) != VK_SUCCESS)
					throw TestFailure("Actual production allocation invalidation failed.");
				std::vector<std::uint8_t> value(bytes);
				std::memcpy(value.data(), static_cast<const std::uint8_t*>(buffer.mappedData) + offset, bytes);
				std::ofstream raw(artifacts/name, std::ios::binary); raw.write(reinterpret_cast<const char*>(value.data()), bytes);
				raw.flush();
				if (!raw) throw TestFailure("Cannot retain production allocation bytes.");
				receipt["allocations"].push_back({{"name",name},{"buffer",reinterpret_cast<std::uintptr_t>(buffer.buffer)},
					{"allocation",reinterpret_cast<std::uintptr_t>(buffer.allocation)},{"bufferBytes",buffer.size},
					{"allocationBytes",info.size},{"offset",offset},{"bytes",bytes},{"memoryProperties",properties}});
				return value;
			};
			const auto binding = [&](VulkanDeformationDescriptorBinding b) -> const VulkanBuffer& {
				return deformation.buffers[static_cast<std::size_t>(b)];
			};
			const auto workBytes = read(binding(VulkanDeformationDescriptorBinding::Work), 0,
				plan.work.size()*sizeof(GpuDeformationShaderWorkRecord), "production-work.bin");
			std::vector<GpuDeformationShaderWorkRecord> works(plan.work.size());
			std::memcpy(works.data(), workBytes.data(), workBytes.size());
			// The oracle is authored A(1 matrix/1 weight), A again, then
			// B(2 matrices/2 weights): expected B starts at 48 palette bytes and
			// 4 morph bytes. No upload/deformation layout function supplies these.
			const auto palettes = read(cpu.paletteBuffer, 0, 3*sizeof(Animation::GpuAnimationPaletteRecord), "production-palettes.bin");
			const auto morphs = read(cpu.morphWeightBuffer, 0, 3*sizeof(float), "production-morphs.bin");
			const auto bridges = read(cpu.graphicsBridgeBuffer, 0, 3*sizeof(VulkanGpuAnimationGraphicsRecord), "production-bridges.bin");
			bool correct = true;
			for (std::size_t index = 0; index < 3; ++index)
			{
				VulkanGpuAnimationGraphicsRecord bridge{}; std::memcpy(&bridge, bridges.data()+index*sizeof(bridge), sizeof(bridge));
				const auto& data = *input.records[index].data;
				const std::size_t paletteOffset = index < 2 ? 0 : sizeof(Animation::GpuAnimationPaletteRecord);
				const std::size_t morphOffset = index < 2 ? 0 : sizeof(float);
				const bool matches = bridge.paletteOffsetBytes == paletteOffset && bridge.morphOutputOffsetBytes == morphOffset
					&& bridge.paletteEntryCount == data.paletteMatrices.size() && bridge.morphCount == data.morphWeights.size()
					&& bridge.paletteMatricesBindlessIndex == cpu.paletteBindlessIndex && bridge.morphOutputsBindlessIndex == cpu.morphWeightBindlessIndex
					&& std::memcmp(palettes.data()+paletteOffset, data.paletteMatrices.data(), data.paletteMatrices.size()*sizeof(Animation::GpuAnimationPaletteRecord)) == 0
					&& std::memcmp(morphs.data()+morphOffset, data.morphWeights.data(), data.morphWeights.size()*sizeof(float)) == 0;
				correct &= matches;
				receipt["bridges"].push_back({{"instance",input.records[index].stableInstanceId},
					{"paletteOffset",bridge.paletteOffsetBytes},{"morphOffset",bridge.morphOutputOffsetBytes},{"valuesMatch",matches}});
			}
			for (std::size_t index = 0; index < works.size(); ++index)
			{
				const auto& work = works[index]; const auto& source = plan.work[index];
				if (source.sourceStableInstanceId < 1 || source.sourceStableInstanceId > 3)
					throw TestFailure("Production work refers to an unowned source instance.");
				const bool a = source.sourceStableInstanceId < 3;
				const auto paletteOffset = std::uint64_t(work.paletteRowOffset)*16;
				const auto morphOffset = std::uint64_t(work.morphWeightOffset)*sizeof(float);
				const bool bounded = paletteOffset <= palettes.size() && work.jointCount*sizeof(Animation::GpuAnimationPaletteRecord) <= palettes.size()-paletteOffset
					&& morphOffset <= morphs.size() && work.morphTargetCount*sizeof(float) <= morphs.size()-morphOffset;
				const bool matches = bounded && paletteOffset == (a ? 0 : sizeof(Animation::GpuAnimationPaletteRecord))
					&& morphOffset == (a ? 0 : sizeof(float)) && work.jointCount == 1 && work.morphTargetCount == 1
					&& work.generationRecordIndex == index && work.vertexCount == source.vertexCount && work.vertexCount != 0
					&& work.currentOutputOffset == source.vertexOffset;
				correct &= matches;
				receipt["work"].push_back({{"index",index},{"sourceInstance",source.sourceStableInstanceId},
					{"mesh",source.key.page.meshId},{"page",source.key.page.pageId},
					{"pageContentGeneration",source.key.page.pageContentGeneration},{"meshPayloadGeneration",source.key.page.meshPayloadGeneration},
					{"paletteRowOffset",work.paletteRowOffset},{"morphWeightOffset",work.morphWeightOffset},
					{"vertexCount",work.vertexCount},{"currentOutputOffset",work.currentOutputOffset},{"bounded",bounded},{"valuesMatch",matches}});
			}
			result.uploadAndWorkValuesMatch = correct; receipt["uploadAndWorkValuesMatch"] = correct; persist();
			if (!correct) throw TestFailure("Production A/A/B upload/work offsets or bytes differ; unsafe dispatch was not attempted.");
			VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
			const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
			if (!getInstance) throw TestFailure("Actual production instance lookup unavailable.");
			const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(state.instance,"vkGetDeviceProcAddr"));
			if (!getDevice) throw TestFailure("Actual production device lookup unavailable.");
			const auto createPool = reinterpret_cast<PFN_vkCreateCommandPool>(getDevice(state.device,"vkCreateCommandPool"));
			const auto destroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(getDevice(state.device,"vkDestroyCommandPool"));
			const auto allocate = reinterpret_cast<PFN_vkAllocateCommandBuffers>(getDevice(state.device,"vkAllocateCommandBuffers"));
			const auto begin = reinterpret_cast<PFN_vkBeginCommandBuffer>(getDevice(state.device,"vkBeginCommandBuffer"));
			const auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(getDevice(state.device,"vkCmdPipelineBarrier"));
			const auto end = reinterpret_cast<PFN_vkEndCommandBuffer>(getDevice(state.device,"vkEndCommandBuffer"));
			const auto createFence = reinterpret_cast<PFN_vkCreateFence>(getDevice(state.device,"vkCreateFence"));
			const auto destroyFence = reinterpret_cast<PFN_vkDestroyFence>(getDevice(state.device,"vkDestroyFence"));
			const auto wait = reinterpret_cast<PFN_vkWaitForFences>(getDevice(state.device,"vkWaitForFences"));
			if (!createPool || !destroyPool || !allocate || !begin || !barrier || !end || !createFence || !destroyFence || !wait)
				throw TestFailure("Actual production observer device functions unavailable.");
			const auto families = state.FindQueueFamilies(state.physicalDevice);
			if (!families.graphicsFamily) throw TestFailure("Actual graphics queue family unavailable.");
			struct Owner final
			{
				VkDevice device; PFN_vkDestroyCommandPool destroyPool; PFN_vkDestroyFence destroyFence;
				VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
				~Owner() { if (fence) destroyFence(device,fence,nullptr); if (pool) destroyPool(device,pool,nullptr); }
			} owner{state.device,destroyPool,destroyFence};
			const auto check = [](VkResult value) { if (value != VK_SUCCESS) throw TestFailure("Production A/A/B native call failed: "+std::to_string(value)); };
			VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = *families.graphicsFamily;
			check(createPool(state.device,&poolInfo,nullptr,&owner.pool));
			VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			commandInfo.commandPool=owner.pool; commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount=1;
			VkCommandBuffer command=VK_NULL_HANDLE; check(allocate(state.device,&commandInfo,&command));
			VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; beginInfo.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			check(begin(command,&beginInfo));
			const auto recorded = RecordVulkanDeformationCommands(command,state.pendingDeformationCommandPlan,plan,
				state.deformationComputePipelines,state.deformationDescriptorSets[slot]);
			result.recorded = recorded.recorded && recorded.deformationDispatchCount != 0 && recorded.boundsDispatchCount != 0;
			if (!result.recorded) throw TestFailure("Actual production deformation/bounds commands were not recorded.");
			// A real device-to-host dependency is required even for coherent mapped
			// memory. Fence completion alone is not a host visibility guarantee.
			VkMemoryBarrier visibility{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			visibility.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; visibility.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
			barrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&visibility,0,nullptr,0,nullptr);
			check(end(command));
			if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
				throw TestFailure("Production A/A/B recording failed validation before native submission.");
			VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(createFence(state.device,&fenceInfo,nullptr,&owner.fence));
			VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submission.commandBufferCount=1; submission.pCommandBuffers=&command;
			const auto quarantine = [&](VkResult value, const char* point) noexcept {
				// Only the watched native child reaches here. Never unwind Engine or
				// allocation owners if a potentially live submission is unproven.
				try { receipt["quarantine"]={{"point",point},{"result",int(value)},
					{"command",reinterpret_cast<std::uintptr_t>(command)},{"fence",reinterpret_cast<std::uintptr_t>(owner.fence)}}; persist(); } catch (...) {}
				std::_Exit(2);
			};
			VkResult submitted = VK_ERROR_UNKNOWN;
			try { submitted = state.SubmitGraphicsQueue(1,&submission,owner.fence); }
			catch (...) { quarantine(VK_ERROR_UNKNOWN,"ProductionSubmitException"); }
			if (submitted != VK_SUCCESS) quarantine(submitted,"ProductionSubmitUncertain");
			const auto completed = wait(state.device,1,&owner.fence,VK_TRUE,10000000000ULL);
			if (completed != VK_SUCCESS) quarantine(completed,"ProductionCompletionUnproven");
			result.completed=true; receipt["completed"]=true;
			// Re-observe upload inputs after the real dispatch/fence too. These files
			// preserve the completed allocation bytes separately from preflight proof.
			const auto completedPalettes=read(cpu.paletteBuffer,0,palettes.size(),"completed-production-palettes.bin");
			const auto completedMorphs=read(cpu.morphWeightBuffer,0,morphs.size(),"completed-production-morphs.bin");
			const auto completedWork=read(binding(VulkanDeformationDescriptorBinding::Work),0,workBytes.size(),"completed-production-work.bin");
			result.uploadAndWorkValuesMatch &= completedPalettes==palettes && completedMorphs==morphs && completedWork==workBytes;
			receipt["uploadAndWorkValuesMatch"]=result.uploadAndWorkValuesMatch;
			receipt["command"]=reinterpret_cast<std::uintptr_t>(command); receipt["fence"]=reinterpret_cast<std::uintptr_t>(owner.fence);
			receipt["deformationDispatches"]=recorded.deformationDispatchCount; receipt["boundsDispatches"]=recorded.boundsDispatchCount;
			const auto close = [](float a,float b) { return std::isfinite(a) && std::abs(a-b)<=0.0002F; };
			bool outputMatches=true;
			for (std::size_t index=0; index<works.size(); ++index)
			{
				const auto& work=works[index]; const auto& source=plan.work[index]; const bool a=source.sourceStableInstanceId<3;
				const auto suffix=std::to_string(index);
				const auto vertexBytes=read(binding(VulkanDeformationDescriptorBinding::CurrentVertices),
					VkDeviceSize(work.currentOutputOffset)*sizeof(GpuDeformedVertexRecord),sizeof(GpuDeformedVertexRecord),("production-vertex-"+suffix+".bin").c_str());
				const auto packed=read(binding(VulkanDeformationDescriptorBinding::StaticPageBytes),work.inputVertexByteOffset,16,("production-packed-"+suffix+".bin").c_str());
				const auto deltas=read(binding(VulkanDeformationDescriptorBinding::StaticPageBytes),work.morphDeltaByteOffset,48,("production-delta-"+suffix+".bin").c_str());
				const auto boundBytes=read(binding(VulkanDeformationDescriptorBinding::MeshletBounds),
					// The production bounds stage publishes one conservative page row
					// per work record, not one row per source meshlet in this ABI.
					VkDeviceSize(index)*sizeof(GpuDynamicBoundsRecord),sizeof(GpuDynamicBoundsRecord),("production-bounds-"+suffix+".bin").c_str());
				GpuDeformedVertexRecord vertex{}; GpuDynamicBoundsRecord bounds{}; std::array<std::uint32_t,4> words{}; std::array<float,12> delta{};
				std::memcpy(&vertex,vertexBytes.data(),sizeof(vertex)); std::memcpy(&bounds,boundBytes.data(),sizeof(bounds));
				std::memcpy(words.data(),packed.data(),16); std::memcpy(delta.data(),deltas.data(),48);
				const std::array<std::uint32_t,3> xyz{words[0]&65535U,words[0]>>16,words[1]&65535U};
				std::array<float,3> expected{}; bool matches=true;
				for (std::size_t axis=0; axis<3; ++axis)
				{
					const float translation=float((a?1:4)+axis), weight=a?0.25F:0.5F;
					const float authoredDelta=axis==2?0.01F:0.0F;
					expected[axis]=work.positionOffset[axis]+(float(xyz[axis])/65535.0F*2-1)*work.positionScale[axis]+authoredDelta*weight+translation;
					matches &= close(delta[axis],authoredDelta) && close(vertex.currentPosition[axis],expected[axis])
						&& std::isfinite(bounds.minimum[axis]) && std::isfinite(bounds.maximum[axis])
						&& bounds.minimum[axis]<=vertex.currentPosition[axis]+0.0002F && bounds.maximum[axis]>=vertex.currentPosition[axis]-0.0002F;
				}
				matches &= bounds.identity[0]==work.reserved[0] && bounds.identity[1]==index
					&& bounds.identity[2]==kAnimatedGeometryAbiVersion && (bounds.identity[3]&13U)==13U;
				outputMatches &= matches;
				receipt["work"][index]["gpuPosition"]=vertex.currentPosition;
				receipt["work"][index]["expectedPosition"]=expected;
				receipt["work"][index]["boundsIdentity"]=bounds.identity;
				receipt["work"][index]["deformedValuesMatch"]=matches;
			}
			result.deformedValuesMatch=outputMatches; receipt["deformedValuesMatch"]=outputMatches; persist();
			return result;
		}
		static TextureViewFailureObservation ExerciseOrdinary(VulkanContext& context, const UUID& id, TextureUploadFailureHook& hook)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			TextureViewFailureObservation result; VmaTotalStatistics before{}, after{};
			vmaCalculateStatistics(state.allocator, &before); result.allocationsBefore = before.total.statistics.allocationCount;
			try { state.EnsureTexture(id); } catch (const std::exception&) { result.exceptionObserved = true; }
			vmaCalculateStatistics(state.allocator, &after); result.allocationsAfter = after.total.statistics.allocationCount;
			// Observe the real leak BEFORE optional test cleanup. This cleanup cannot
			// turn the red allocation delta into a passing ownership observation.
			if (result.allocationsAfter > result.allocationsBefore && hook.imageHandle && hook.imageAllocation
				&& !state.textureCache.contains(id) && hook.actualCopySubmitAndFenceSucceeded)
			{
				const bool retained = std::any_of(state.ambiguousUploadImages.begin(), state.ambiguousUploadImages.end(),
					[&](const auto& image) { return reinterpret_cast<std::uintptr_t>(image.image) == hook.imageHandle; });
				if (!retained && state.WaitDeviceIdle() == VK_SUCCESS)
				{
					vmaDestroyImage(state.allocator, reinterpret_cast<VkImage>(hook.imageHandle), static_cast<VmaAllocation>(hook.imageAllocation));
					result.observerOnlyReclaimedLeakedImage = true;
				}
			}
			SetTextureUploadFailureHook(nullptr);
			// Resume a real owner operation on the known-good default image. This
			// does not claim that a failed ordinary upload published a descriptor.
			const auto view = state.CreateImageView(state.defaultTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
			result.subsequentViewCreated = view != VK_NULL_HANDLE;
			if (view)
			{
				VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
				const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
				const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(state.instance, "vkGetDeviceProcAddr"));
				const auto destroy = reinterpret_cast<PFN_vkDestroyImageView>(getDevice(state.device, "vkDestroyImageView"));
				if (!destroy) throw std::runtime_error("Actual device image-view retirement entry point is unavailable.");
				destroy(state.device, view, nullptr);
			}
			return result;
		}

		static void ExerciseMipPromotion(VulkanContext& context, const UUID& id, TestContext& test)
		{
			auto& state = *context.impl;
			auto& resources = ResourceManager::Instance();
			std::lock_guard lock(state.deviceStateMutex);
			resources.MarkTextureStreamingDistance(id, 100.0, 1, "far texture demand");
			state.EnsureTexture(id);
			const auto original = state.textureCache.at(id);
			const auto alias = state.textureIndexMap.at(id);
			test.Expect(original.residentMipOffset == 3U && alias > 2U, "Far demand initially publishes a valid coarse mip tail.");
			VmaTotalStatistics before{}, after{};
			vmaCalculateStatistics(state.allocator, &before);

			// A genuine copy succeeds before native image-view creation fails.
			// Existing readers must keep the old image and material alias.
			resources.MarkTextureStreamingDistance(id, 0.0, 2, "near texture demand");
			TextureUploadFailureHook hook;
			hook.role = ReviewTextureRole::Ordinary;
			hook.failure = ReviewTextureFailure::ImageView;
			test.Expect(SetTextureUploadFailureHook(&hook), "Arm the real replacement fault hook.");
			{
				struct Disarm { ~Disarm() { SetTextureUploadFailureHook(nullptr); } } disarm;
				state.EnsureTexture(id);
			}
			vmaCalculateStatistics(state.allocator, &after);
			test.Expect(hook.actualCopySubmitAndFenceSucceeded && hook.reached == 1
				&& before.total.statistics.allocationCount == after.total.statistics.allocationCount,
				"The failed promotion destroys only the new unpublished allocation.");
			test.Expect(state.textureCache.at(id).view == original.view && state.textureIndexMap.at(id) == alias,
				"The previous image and bindless material alias survive the failed promotion.");

			state.EnsureTexture(id);
			const auto promoted = state.textureCache.at(id);
			test.Expect(promoted.view != original.view && promoted.residentMipOffset == 0U && state.textureIndexMap.at(id) == alias,
				"A successful retry switches the same material alias to the finer image.");
			// Advance beyond the nearest-view aggregation window so the
			// backend receives an actual coarser demand on the same owner.
			std::this_thread::sleep_for(std::chrono::milliseconds(275));
			resources.MarkTextureStreamingDistance(id, 100.0, 3, "far texture demand again");
			test.Expect(resources.ResolveTextureMipResidencyForTexture(id, 5).sourceMipOffset == 3U,
				"The distance publisher has selected the coarser tail again.");
			state.EnsureTexture(id);
			test.Expect(state.textureCache.at(id).view == promoted.view && state.textureCache.at(id).residentMipOffset == 0U,
				"Moving away retains the sharper image and avoids a replacement copy.");
		}

		static void ExerciseReloadDuringMipPromotion(VulkanContext& context, const UUID& id,
			const std::filesystem::path& path, TestContext& test)
		{
			auto& state = *context.impl;
			auto& resources = ResourceManager::Instance();
			std::lock_guard lock(state.deviceStateMutex);
			resources.MarkTextureStreamingDistance(id, 100.0, 1, "far texture demand");
			state.EnsureTexture(id);
			const auto original = state.textureCache.at(id);
			const auto alias = state.textureIndexMap.at(id);
			const auto aliasGeneration = state.textureViewGenerations.at(id);
			test.Expect(original.sourceOwner && original.residentMipOffset == 3U && alias > 2U,
				"The original coarse tail and its real alias must be resident.");

			// Change the cooked bytes before the upload starts. The old immutable
			// owner remains current until the rendezvous reloads the new file.
			TextureFixtures::Make(8, 8, 4, [](unsigned, unsigned, unsigned) { return TextureFixtures::Blue; }).Save(path);
			resources.MarkTextureStreamingDistance(id, 0.0, 2, "near texture demand");
			struct Replacement {
				UUID id;
				std::shared_ptr<const Texture> owner;
				std::optional<Resource::StreamableAssetRecord> accounting;
			} replacement{id, {}, {}};
			TextureUploadFailureHook hook;
			hook.role = ReviewTextureRole::Ordinary;
			hook.remaining = 0;
			hook.commitContext = &replacement;
			hook.beforeTextureCommit = [](void* data) {
				auto& replacement = *static_cast<Replacement*>(data);
				auto& resources = ResourceManager::Instance();
				// Wait for a real loader worker to replace the CPU owner after
				// the old upload has finished its descriptor-idle wait.
				if (resources.ReloadTexturePublicationAsync(replacement.id).get() != replacement.id)
					throw std::runtime_error("Replacement texture failed to publish.");
				replacement.owner = resources.AcquireTextureSnapshot(replacement.id);
				replacement.accounting = resources.GetStreamableAsset(replacement.id);
			};
			VmaTotalStatistics before{}, after{};
			vmaCalculateStatistics(state.allocator, &before);
			test.Expect(SetTextureUploadFailureHook(&hook), "Install the real upload rendezvous.");
			{
				struct Disarm { ~Disarm() { SetTextureUploadFailureHook(nullptr); } } disarm;
				state.EnsureTexture(id);
			}
			vmaCalculateStatistics(state.allocator, &after);
			test.Expect(hook.actualCopySubmitAndFenceSucceeded && replacement.owner
				&& replacement.owner != original.sourceOwner,
				"The reload must supersede the CPU owner after a real completed copy.");
			test.Expect(state.textureCache.at(id).view == original.view && state.textureIndexMap.at(id) == alias
				&& state.textureViewGenerations.at(id) == aliasGeneration
				&& after.total.statistics.allocationCount == before.total.statistics.allocationCount,
				"Reject the stale upload without changing the bound image or leaking its allocation.");
			const auto accounting = resources.GetStreamableAsset(id);
			test.Expect(replacement.accounting && accounting
				&& accounting->residencyGeneration == replacement.accounting->residencyGeneration
				&& accounting->textureResidentMipCount == replacement.accounting->textureResidentMipCount
				&& accounting->estimatedGpuBytes == replacement.accounting->estimatedGpuBytes,
				"Reject the stale upload before it can overwrite the replacement owner's accounting.");

			state.EnsureTexture(id);
			const auto& current = state.textureCache.at(id);
			test.Expect(current.sourceOwner == replacement.owner && current.sourceMipCount == 4U
				&& current.residentMipOffset == 0U && state.textureIndexMap.at(id) == alias
				&& state.textureViewGenerations.at(id) == aliasGeneration + 1U,
				"A later ensure publishes the replacement owner to the same descriptor alias.");
		}
	};
	AnimationUploadProductionObservation ExerciseAnimationUploadProduction(VulkanContext& context,
		const Renderer::RenderBackendWorkPublication& publication, const std::filesystem::path& artifacts)
	{
		return VulkanTextureReviewAccess::ExerciseAnimation(context,publication,artifacts);
	}
}
namespace TerraEngine::Tests
{
	namespace
	{
		bool EnterTextureChild(TestContext& context, const std::string& name)
		{
			if (Review::VulkanReviewFixture::IsWatchedChild(name)) return true;
			const auto child = Review::VulkanReviewFixture::RunWatchedSelf(name, std::chrono::seconds(120));
			Review::RecordNativeReviewChild(name, child);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Actual texture ownership child must settle: " + child.output); return false;
		}
		void DefaultFailure(TestContext& context, const std::string& name, Review::ReviewTextureRole role, Review::ReviewTextureFailure failure)
		{
			using namespace Review;
			if (!EnterTextureChild(context, name)) return;
			ExistingRendererFixtureSession session(name); auto& renderer = RenderSystem::Instance();
			// Keep the real Engine/managed host alive, replacing only the renderer,
			// exactly like the maintained renderer lifetime fixture's iterations.
			renderer.Shutdown(); TextureUploadFailureHook hook; hook.role = role; hook.failure = failure;
			context.Expect(SetTextureUploadFailureHook(&hook), "One-shot real upload fault hook must be compiled and admitted.");
			struct Disarm { ~Disarm() { SetTextureUploadFailureHook(nullptr); } } disarm;
			bool escaped = false, observationStop = false;
			try { renderer.Initialize(); }
			catch (const TextureReviewObservationStop&) { observationStop = true; }
			catch (const std::exception&) { escaped = true; }
			SetTextureUploadFailureHook(nullptr);
			const bool retirementOrderSafe = !hook.ownershipEventsOverflowed
				&& (hook.stagingDestroyedAfterRetirementProof == 0
					|| (hook.retainedSequence && hook.retirementProofSequence > hook.retainedSequence
						&& hook.lastDestroySequence > hook.retirementProofSequence));
			const bool callerOwnershipSafe = retirementOrderSafe && hook.nullImageUseBlocked == 0 && hook.observerOnlyStagingReclaimed == 0 && hook.publicationAttempts == 0
				&& (failure != ReviewTextureFailure::CompletedCopyCallerAmbiguity
					|| (hook.actualCopySubmitAndFenceSucceeded && hook.callerAmbiguityInjected && hook.stagingRetained != 0 && hook.ambiguousStagingDestroyAttempts == 0));
			renderer.Shutdown(); renderer.Initialize();
			auto* recovered = dynamic_cast<VulkanContext*>(renderer.GetContext());
			const bool resumed = recovered && recovered->GetEmptyFrameValidationReceipt().initialized && recovered->GetEmptyFrameValidationReceipt().validationActuallyEnabled;
			session.CloseRenderer();
			nlohmann::json ownershipRows = nlohmann::json::array();
			for (std::uint32_t i = 0; i < hook.ownershipEventCount; ++i)
				ownershipRows.push_back({{"point",int(hook.ownershipEvents[i].point)},{"sequence",hook.ownershipEvents[i].sequence}});
			std::ofstream(session.Artifacts()/"default-upload-failure.json") << nlohmann::json{{"scope","ActualDefaultUpload_ControlledFailure"},
				{"role",int(role)},{"failure",int(failure)},{"faultReached",hook.reached},{"imageCreated",hook.imageCreated},{"stagingCreated",hook.stagingCreated},
				{"stagingDestroyed",hook.stagingDestroyed},{"stagingRetained",hook.stagingRetained},{"publicationAttempts",hook.publicationAttempts},
				{"nullImageUseBlockedBeforeNativeCall",hook.nullImageUseBlocked},{"observerOnlyStagingReclaimed",hook.observerOnlyStagingReclaimed},
				{"actualCopySubmitAndFenceSucceeded",hook.actualCopySubmitAndFenceSucceeded},{"commandHandle",hook.commandHandle},{"fenceHandle",hook.fenceHandle},
				
				{"injectedCallerAmbiguityAfterRealCompletion",hook.callerAmbiguityInjected},{"ambiguousStagingDestroyBeforeRetirementProof",hook.ambiguousStagingDestroyAttempts},
				{"actualOwnershipEvents",ownershipRows},{"retainedSequence",hook.retainedSequence},{"retirementProofSequence",hook.retirementProofSequence},
				{"lastDestroySequence",hook.lastDestroySequence},{"stagingDestroyedAfterRetirementProof",hook.stagingDestroyedAfterRetirementProof},
				{"ownershipEventsOverflowed",hook.ownershipEventsOverflowed},
				{"ownerExceptionObserved",escaped},{"validationOnlyObservationStopBeforeNativeCall",observationStop},{"healthyRendererResumed",resumed},{"allLifetimeValidationErrors",session.ValidationErrors()}}.dump(2);
			context.Expect(hook.reached == 1 && hook.stagingCreated == 1, "The real targeted default upload must reach one bounded fault.");
			context.Expect(callerOwnershipSafe, "A reported failed/unproven upload must not publish or destroy unproven staging; null native uses must never be attempted.");
			context.Expect(escaped && !observationStop, "Checked upload failure must propagate an explicit production startup failure without reaching the null-use observer stop.");
			context.Expect(resumed, "The next actual renderer lifetime must initialize after the controlled upload failure.");
			context.Expect(session.ValidationErrors() == 0, "Actual failed and resumed renderer lifetimes, including teardown, must introduce no Vulkan validation error.");
		}
	}
	void RegisterTextureUploadFailureOwnershipTests(TestRegistry& registry)
	{
		using namespace Review;
		if (!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		registry.Add("Native.ReviewSimulation.Lifecycle.OrdinaryTexture.MipPromotionPreservesResidentImage", [](TestContext& test) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.OrdinaryTexture.MipPromotionPreservesResidentImage";
			if (!EnterTextureChild(test, name)) return;
			ExistingRendererFixtureSession session(name);
			auto& resources = ResourceManager::Instance();
			auto policy = resources.GetStreamingPolicy();
			policy.textureMipStreamingEnabled = true;
			policy.textureMinStreamMip = 0;
			policy.textureMaxStreamMip = 3;
			policy.streamingDistance = 100.0;
			resources.SetStreamingPolicy(policy);
			const auto path = session.Artifacts() / "ordinary-mip-promotion.tetf";
			TextureFixtures::Make(16, 16, 5).Save(path);
			const auto id = resources.LoadTexture(path);
			test.Expect(id != EMPTY_UUID, "The owned mip-chain fixture must load.");
			VulkanTextureReviewAccess::ExerciseMipPromotion(session.Native(), id, test);
			session.CloseRenderer();
			test.Expect(session.ValidationErrors() == 0, "Mip replacement and teardown must pass Vulkan validation.");
		});
		registry.Add("Native.ReviewSimulation.Lifecycle.OrdinaryTexture.ReloadSupersedesCompletedMipUpload", [](TestContext& test) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.OrdinaryTexture.ReloadSupersedesCompletedMipUpload";
			if (!EnterTextureChild(test, name)) return;
			ExistingRendererFixtureSession session(name);
			auto& resources = ResourceManager::Instance();
			auto policy = resources.GetStreamingPolicy();
			policy.textureMipStreamingEnabled = true;
			policy.textureMinStreamMip = 0;
			policy.textureMaxStreamMip = 3;
			policy.streamingDistance = 100.0;
			resources.SetStreamingPolicy(policy);
			const auto path = session.Artifacts() / "ordinary-mip-reload.tetf";
			TextureFixtures::Make(16, 16, 5).Save(path);
			const auto id = resources.LoadTexture(path);
			test.Expect(id != EMPTY_UUID, "The owned texture fixture must load.");
			VulkanTextureReviewAccess::ExerciseReloadDuringMipPromotion(session.Native(), id, path, test);
			session.CloseRenderer();
			test.Expect(session.ValidationErrors() == 0, "Concurrent reload and teardown must pass Vulkan validation.");
		});
		for (const auto role : {ReviewTextureRole::Normal, ReviewTextureRole::Mask})
			for (const auto failure : {ReviewTextureFailure::ImageAllocation, ReviewTextureFailure::CopyRefusal, ReviewTextureFailure::CompletedCopyCallerAmbiguity})
			{
				const std::string name = "Native.ReviewSimulation.Lifecycle.DefaultTexture." + std::string(role == ReviewTextureRole::Normal ? "Normal." : "Mask.")
					+ (failure == ReviewTextureFailure::ImageAllocation ? "ImageAllocationFailure" : failure == ReviewTextureFailure::CopyRefusal ? "CopyRefusal" : "CallerAmbiguityAfterCompletedFence");
				registry.Add(name, [name, role, failure](TestContext& context) { DefaultFailure(context, name, role, failure); });
			}
		registry.Add("Native.ReviewSimulation.Lifecycle.OrdinaryTexture.ViewFailureReleasesImage", [](TestContext& context) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.OrdinaryTexture.ViewFailureReleasesImage";
			if (!EnterTextureChild(context, name)) return;
			ExistingRendererFixtureSession session(name); const auto path = session.Artifacts()/"ordinary-view.tetf";
			TextureFixtures::Make().Save(path); const auto id = ResourceManager::Instance().LoadTexture(path);
			context.Expect(id != EMPTY_UUID && ResourceManager::Instance().AcquireTextureSnapshot(id) != nullptr, "Actual validated ordinary texture bytes must publish before native upload.");
			TextureUploadFailureHook hook; hook.role = ReviewTextureRole::Ordinary; hook.failure = ReviewTextureFailure::ImageView;
			context.Expect(SetTextureUploadFailureHook(&hook), "Actual image-view fault hook must be compiled and admitted.");
			struct Disarm { ~Disarm() { SetTextureUploadFailureHook(nullptr); } } disarm;
			const auto before = session.Native().GetEmptyFrameValidationReceipt();
			const auto observed = VulkanTextureReviewAccess::ExerciseOrdinary(session.Native(), id, hook);
			const auto after = session.Native().GetEmptyFrameValidationReceipt();
			session.CloseRenderer();
			std::ofstream(session.Artifacts()/"ordinary-view-failure.json") << nlohmann::json{{"scope","ActualEnsureTextureAndVmaAllocationCounts"},
				{"faultReached",hook.reached},{"actualCopySubmitAndFenceSucceeded",hook.actualCopySubmitAndFenceSucceeded},{"allocationsBefore",observed.allocationsBefore},
				{"allocationsAfterSettledAttempt",observed.allocationsAfter},{"exceptionObserved",observed.exceptionObserved},
				{"observerOnlyReclaimedLeakAfterObservation",observed.observerOnlyReclaimedLeakedImage},{"subsequentActualViewCreated",observed.subsequentViewCreated},{"allLifetimeValidationErrors",session.ValidationErrors()}}.dump(2);
			context.Expect(hook.reached == 1 && hook.imageCreated == 1 && hook.actualCopySubmitAndFenceSucceeded, "Ordinary view failure must follow a real completed texture upload.");
			context.Expect(observed.allocationsAfter == observed.allocationsBefore && !observed.observerOnlyReclaimedLeakedImage, "Failed view creation must release its actual newly allocated image before caller settlement.");
			context.Expect(observed.subsequentViewCreated && after.validationErrorsAfter == before.validationErrorsAfter && session.ValidationErrors() == 0, "A later actual view operation and teardown must work without native validation errors.");
		});
	}
}
