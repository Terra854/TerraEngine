#include "TestHarness.h"
#include <string_view>
#include <iterator>
#include "VulkanSourceTestUtils.h"

#include "RendererThreadingTestFixtures.h"

#include "Renderer/Graph/RenderWorkPacket.h"
#include "Renderer/RHI/Vulkan/VulkanCommandRecording.h"
#include "Renderer/RHI/Vulkan/VulkanDescriptorPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanPipelineStateKeys.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/RHI/Vulkan/VulkanTransitionPlanning.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace TerraEngine::Tests {
	namespace {

		// Required reads and lexical definition bounds fail before any token or
		// ordering assertion can accidentally accept an empty/missing region.
		std::string RequiredSource(TestContext& context, const char* path)
		{
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
			context.Expect(input.good(), "Required source-contract input must be readable.");
			context.Expect(input.good(), std::string("Required source is unreadable: ") + path);
			std::string text((std::istreambuf_iterator<char>(input)), {});
			context.Expect(!text.empty(), std::string("Required source is empty: ") + path);
			text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
			return text;
		}
		std::string RequiredDefinition(TestContext& context, const char* path, std::string_view signature)
		{
			const auto text = RequiredSource(context, path);
			const auto start = text.find(signature);
			context.Expect(start != std::string::npos, std::string("Required definition is absent: ") + std::string(signature));
			const auto open = text.find('{', start + signature.size());
			context.Expect(open != std::string::npos, "Required definition has no opening body.");
			std::size_t depth = 0;
			bool lineComment = false, blockComment = false, quoted = false, character = false, escape = false;
			for(std::size_t index = open; index < text.size(); ++index)
			{
				const char value = text[index], next = index + 1 < text.size() ? text[index + 1] : '\0';
				if(lineComment) { if(value == '\n') lineComment = false; continue; }
				if(blockComment) { if(value == '*' && next == '/') { blockComment = false; ++index; } continue; }
				if(quoted || character) {
					if(escape) { escape = false; continue; }
					if(value == '\\') { escape = true; continue; }
					if((quoted && value == '"') || (character && value == '\'')) quoted = character = false;
					continue;
				}
				if(value == '/' && next == '/') { lineComment = true; ++index; continue; }
				if(value == '/' && next == '*') { blockComment = true; ++index; continue; }
				if(value == '"') { quoted = true; continue; }
				if(value == '\'') { character = true; continue; }
				if(value == '{') ++depth;
				if(value == '}' && --depth == 0) return text.substr(start, index + 1 - start);
			}
			context.Expect(false, "Required definition has no balanced closing body.");
			return {};
		}

		std::string RequiredVulkanContext(TestContext& context)
		{
			for(const char* file : {"VulkanContext.cpp", "VulkanContextSource.h", "VulkanBuiltinPasses.cpp", "VulkanEditorRendering.cpp", "VulkanFrameRendering.cpp", "VulkanFrameSubmission.cpp", "VulkanPostProcessing.cpp", "VulkanReadback.cpp"})
				RequiredSource(context, (std::string("Engine/Renderer/RHI/Vulkan/") + file).c_str());
			return VulkanSourceContracts::ReadContextSources();
		}
		std::string RequiredVulkanResources(TestContext& context)
		{
			for(const char* file : {"VulkanContextResources.cpp", "VulkanResourceSource.h", "VulkanAnimationResources.cpp", "VulkanVirtualGeometryResources.cpp", "VulkanPipelineResources.cpp", "VulkanGlobalResources.cpp", "VulkanLightingResources.cpp", "VulkanGpuDiagnostics.cpp"})
				RequiredSource(context, (std::string("Engine/Renderer/RHI/Vulkan/") + file).c_str());
			return VulkanSourceContracts::ReadResourceSources();
		}
		bool SourceBefore(const std::string& text, std::string_view first, std::string_view second)
		{
			const auto a = text.find(first), b = text.find(second);
			return a != std::string::npos && b != std::string::npos && a < b;
		}

	}


	void RegisterVulkanRecordingSchedulingTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VulkanRecording.SecondaryReuseRequiresExactGenerationAndFence", [](TestContext& context) {
			TerraEngine::Renderer::Vulkan::SecondaryCommandReuseIdentity identity{};
			identity.rendererSessionGeneration = 11u;
			identity.backendResourceGeneration = 12u;
			identity.renderPassIdentity = 13u;
			identity.framebufferIdentity = 14u;
			identity.commandSignature = 15u;
			identity.width = 1920u;
			identity.height = 1080u;
			const std::uint64_t original = TerraEngine::Renderer::Vulkan::
				BuildSecondaryCommandReuseSignature(identity);
			context.Expect(TerraEngine::Renderer::Vulkan::CanReuseSecondaryCommands(
				original, original, true),
				"An exact secondary recording may be reused only after its frame-slot fence completes.");
			context.Expect(!TerraEngine::Renderer::Vulkan::CanReuseSecondaryCommands(
				original, original, false),
				"An in-flight frame slot must never expose its retained command buffer.");

			++identity.backendResourceGeneration;
			const std::uint64_t recovered = TerraEngine::Renderer::Vulkan::
				BuildSecondaryCommandReuseSignature(identity);
			context.Expect(!TerraEngine::Renderer::Vulkan::CanReuseSecondaryCommands(
				original, recovered, true),
				"Swapchain or backend recovery must invalidate inherited Vulkan commands.");
			--identity.backendResourceGeneration;
			++identity.commandSignature;
			const std::uint64_t changedDraws = TerraEngine::Renderer::Vulkan::
				BuildSecondaryCommandReuseSignature(identity);
			context.Expect(!TerraEngine::Renderer::Vulkan::CanReuseSecondaryCommands(
				original, changedDraws, true),
				"Changed pipelines, descriptors, push values, or indirect ranges must force rerecording.");
		});

		registry.Add("Renderer.VulkanRecording.ReusesOnlyExactResidentAnimationPayload", [](TestContext& context) {
			auto first = std::make_shared<TerraEngine::Animation::GpuAnimationResidentPayloads>();
			first->programWords = {1u, 2u};
			auto equalBytesDifferentOwner =
				std::make_shared<TerraEngine::Animation::GpuAnimationResidentPayloads>(*first);
			context.Expect(
				TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(first, first, true),
				"A retained frame-slot buffer may reuse the exact immutable cooked payload owner.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(
						first, equalBytesDifferentOwner, true)
				&& !TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload(first, first, false)
				&& !TerraEngine::Renderer::Vulkan::
					CanReuseVulkanAnimationResidentPayloadUpload({}, first, true),
				"A new publication, resized buffer, or empty prior identity must force a complete resident upload.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(true, false)
				&& TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(false, false)
				&& TerraEngine::Renderer::Vulkan::
					ShouldRefreshVulkanAnimationBindlessBuffer(true, true),
				"Stable frame-slot descriptors stay untouched, while first publication and buffer replacement refresh them.");
		});

		registry.Add("Renderer.VulkanRecording.FrameUploadsReusePersistentMappings", [](TestContext& context) {
			const std::filesystem::path implPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextImpl.h";
			std::ifstream implStream(implPath, std::ios::binary);
			context.Expect(implStream.good(), "Required source-contract input must be readable.");
			const std::string implementation{ std::istreambuf_iterator<char>(implStream), {} };
			const std::string resources = RequiredVulkanResources(context);
			const std::string contextSource = RequiredVulkanContext(context);

			// Host-visible frame resources are updated every frame. Keeping their VMA
			// allocations mapped removes repeated driver bookkeeping, while every path
			// must retain an explicit flush for non-coherent memory. The animation bridge
			// is authored in its final allocation so it does not build and copy a second
			// crowd-sized vector first.
			context.Expect(implementation.find("void* mappedData = nullptr;") != std::string::npos
				&& resources.find("VMA_ALLOCATION_CREATE_MAPPED_BIT") != std::string::npos
				&& resources.find("void* mapped = buffer.mappedData;") != std::string::npos
				&& resources.find("vmaFlushAllocation(allocator, buffer.allocation, offset, size);")
					!= std::string::npos
				&& resources.find("VulkanGpuAnimationGraphicsRecord* bridgeRecords")
					!= std::string::npos
				&& implementation.find(
					"std::vector<VulkanGpuAnimationGraphicsRecord> graphicsBridgeScratch")
					== std::string::npos
				&& contextSource.find("void* mapped = instanceBuffer.mappedData;")
					!= std::string::npos,
				"Animation and grouped-instance frame uploads must reuse persistent mappings and write the graphics bridge directly.");
		});

		registry.Add("Renderer.VulkanRecording.ComputeUploadKeepsOnlyDispatchedSources", [](TestContext& context) {
			TerraEngine::Animation::GpuAnimationPlan plan{};
			plan.generation = 7U;
			plan.generationRecords.resize(3U);
			plan.workPackets.resize(3U);
			for(std::uint32_t index = 0U; index < 3U; ++index) {
				plan.workPackets[index].stableInstanceId = 100U + index;
				plan.workPackets[index].generationRecordIndex = index;
				plan.workPackets[index].paletteOffsetBytes = 1024U * index;
			}
			const auto appendDispatch = [&plan](std::uint32_t packetIndex,
				std::uint32_t stage) {
				TerraEngine::Animation::GpuAnimationDispatchRecord record{};
				record.stableInstanceId = plan.workPackets[packetIndex].stableInstanceId;
				record.workPacketIndex = packetIndex;
				record.generationRecordIndex = packetIndex;
				record.stage = stage;
				record.elementCount = 1U;
				plan.dispatchRecords.push_back(record);
			};
			appendDispatch(0U, 0U);
			appendDispatch(0U, 1U);
			appendDispatch(2U, 0U);

			TerraEngine::Renderer::Vulkan::VulkanAnimationComputeUploadPlan upload{};
			const bool built = TerraEngine::Renderer::Vulkan::
				BuildVulkanAnimationComputeUploadPlan(plan, upload);
			const bool compactShapeValid = built && upload.valid
				&& upload.generationRecords.size() == 2U
				&& upload.workPackets.size() == 2U
				&& upload.dispatchRecords.size() == 3U;
			context.Expect(compactShapeValid,
				"Only packets that own actual compute dispatches should enter the frame upload.");
			if(compactShapeValid) {
				context.Expect(upload.workPackets[0U].stableInstanceId == 100U
					&& upload.workPackets[1U].stableInstanceId == 102U
					&& upload.workPackets[1U].paletteOffsetBytes == 2048U
					&& upload.dispatchRecords[0U].workPacketIndex == 0U
					&& upload.dispatchRecords[1U].workPacketIndex == 0U
					&& upload.dispatchRecords[2U].workPacketIndex == 1U
					&& upload.dispatchRecords[2U].generationRecordIndex == 1U,
					"Compaction must preserve source order and output offsets while remapping every compute index consistently.");
			}
		});

		registry.Add("Renderer.VulkanRecording.ComputeUploadReusesImmutableDispatchTopology", [](TestContext& context) {
			auto staticPlan = std::make_shared<TerraEngine::Animation::GpuAnimationPlan>();
			staticPlan->generation = 10U;
			staticPlan->generationRecords.resize(2U);
			staticPlan->workPackets.resize(2U);
			for(std::uint32_t index = 0U; index < 2U; ++index) {
				staticPlan->workPackets[index].stableInstanceId = 500U + index;
				staticPlan->workPackets[index].generationRecordIndex = index;
				TerraEngine::Animation::GpuAnimationDispatchRecord record{};
				record.stableInstanceId = staticPlan->workPackets[index].stableInstanceId;
				record.workPacketIndex = index;
				record.generationRecordIndex = index;
				record.stage = 0U;
				record.elementCount = 1U;
				staticPlan->dispatchRecords.push_back(record);
			}

			TerraEngine::Animation::GpuAnimationPlan frame{};
			frame.generation = 11U;
			frame.retainedStaticPlan = staticPlan;
			frame.dynamicGenerationRecords = staticPlan->generationRecords;
			frame.dynamicSampleTicks = {3U, 7U};
			frame.dynamicRenderIdentities.resize(2U);
			TerraEngine::Renderer::Vulkan::VulkanAnimationComputeUploadPlan upload{};
			context.Expect(TerraEngine::Renderer::Vulkan::
				BuildVulkanAnimationComputeUploadPlan(frame, upload)
				&& !upload.topologyReused,
				"A frame slot must build compact dispatch topology on first publication.");

			const auto* retainedDispatchBytes = upload.dispatchRecords.data();
			frame.generation = 12U;
			frame.dynamicSampleTicks = {4U, 8U};
			++frame.dynamicGenerationRecords[0U].workGeneration;
			context.Expect(TerraEngine::Renderer::Vulkan::
				BuildVulkanAnimationComputeUploadPlan(frame, upload)
				&& upload.topologyReused
				&& upload.dispatchRecords.data() == retainedDispatchBytes
				&& upload.sampleTicks[0U] == 4U
				&& upload.sampleTicks[1U] == 8U
				&& upload.workPackets[0U].sampleTick == 3U
				&& upload.workPackets[1U].sampleTick == 7U,
				"An exact retained static owner must refresh narrow dynamic rows without rewriting packet or dispatch topology.");
		});

		registry.Add("Renderer.VulkanRecording.HiZReusesSealedVisibilitySignature", [](TestContext& context) {
			const std::string source = RequiredVulkanContext(context);

			// A sealed visibility result has already visited every command and folded
			// world, resource, route, LOD, and view identity into one signature. Vulkan
			// should consume that proof directly; only legacy/incomplete publications
			// pay for a second full command-resolution scan.
			context.Expect(source.find(
				"publicationPresentation->virtualGeometryPacketEvidenceSignature != 0U")
					!= std::string::npos
				&& source.find(
					"? publicationPresentation->virtualGeometryPacketEvidenceSignature")
					!= std::string::npos
				&& source.find(": BuildVulkanHiZDepthSceneSignature(commandResolver);")
					!= std::string::npos,
				"Hi-Z diagnostics must reuse sealed visibility identity and keep only a fail-open legacy scan.");
		});

		registry.Add("Renderer.VulkanRecording.GrowsIndirectCommandRangesWithoutOverwritingViews", [](TestContext& context) {
			// A crowd can append a large shadow list after its main-pass commands.
			// The second reservation must start after the first instead of reusing a
			// fixed snapshot slice, even when their combined size exceeds 4,096.
			const auto mainPass = TerraEngine::Renderer::Vulkan::
				PlanIndirectDrawBufferCapacity(4096u, 0u, 2000u);
			const auto shadowPass = TerraEngine::Renderer::Vulkan::
				PlanIndirectDrawBufferCapacity(
					mainPass.targetCommandCapacity,
					mainPass.requiredCommandCount,
					6000u);

			context.Expect(mainPass.valid && shadowPass.valid,
				"Valid command counts should produce append reservations.");
			context.ExpectEqual(static_cast<std::size_t>(0u),
				mainPass.commandOffset,
				"The first view should begin at the frame-slot command origin.");
			context.ExpectEqual(static_cast<std::size_t>(2000u),
				shadowPass.commandOffset,
				"Shadow commands must append after the main-pass range.");
			context.Expect(shadowPass.requiresGrowth
				&& shadowPass.targetCommandCapacity >= 8000u,
				"Crowd command storage must grow beyond the former fixed 4,096-command slice.");

			const auto overflow = TerraEngine::Renderer::Vulkan::
				PlanIndirectDrawBufferCapacity(
					64u,
					std::numeric_limits<std::size_t>::max(),
					1u);
			context.Expect(!overflow.valid,
				"Overflowing command counts must be rejected before offsets can alias.");
		});

		registry.Add("Renderer.VulkanRecording.AnimationBackendReceiptRequiresRecordedSubmittedWork", [](TestContext& context) {
			TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot snapshot;
			context.Expect(!snapshot.HasSubmittedWork() && !snapshot.HasCompletedWork(),
				"An empty backend receipt must not be presented as evidence of GPU animation execution.");

			snapshot.latestSubmitted = {
				71U,
				3U,
				9U,
				4U,
				8U};
			context.Expect(snapshot.HasSubmittedWork() && !snapshot.HasCompletedWork(),
				"A submission receipt becomes valid only with frame, generation, serial, and nonzero recorded-dispatch identity.");

			snapshot.latestCompleted = snapshot.latestSubmitted;
			context.Expect(snapshot.HasCompletedWork(),
				"Fence-owned completion should preserve the exact submitted animation receipt without pose readback.");

			snapshot.latestSubmitted.recordedDispatchCount = 0U;
			context.Expect(!snapshot.HasSubmittedWork(),
				"A zero-dispatch plan must never satisfy truthful backend execution diagnostics.");
		});

		registry.Add("Renderer.VulkanRecording.AnimationPlanIsPublishedAfterBackendFrameReset", [](TestContext& context) {
			// Vulkan BeginFrame retires the reused slot and deliberately clears its
			// pending animation plan. Freeze the coordinator call order so a future
			// refactor cannot silently erase newly published compute work again.
			const auto source = RequiredDefinition(context, "Engine/Renderer/Frame/RenderBackendCommit.cpp", "RenderSystem::CommitFrameBackend(");
			const std::size_t beginFrame = source.find("rhi->BeginFrame();");
			const std::size_t consumePlan = source.find(
				"rhi->ConsumeRenderFramePublication(renderFramePublicationStore.Load())");
			context.Expect(beginFrame != std::string::npos
					&& consumePlan != std::string::npos
					&& beginFrame < consumePlan,
				"The backend frame slot must reset before it receives this frame's immutable animation plan.");
		});

		registry.Add("Renderer.VulkanRecording.BuildsWorkerBatchesFromPackets", [](TestContext& context) {
			TerraEngine::Renderer::RenderFramePublication publication;
			publication.frameId = 5u;
			publication.generation = 1u;
			TerraEngine::Renderer::RenderViewSelection view;
			view.outputId = 2u;
			view.generation = 1u;
			view.visibleOrdinals.push_back({ 0u, 1u });
			publication.views.push_back(view);
			publication.submissionRanges.push_back({
				TerraEngine::Renderer::RenderSubmissionRoute::Direct, 0u, 0u, 1u, 1u, true });
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;

			const auto vulkanPlan = TerraEngine::Renderer::Vulkan::BuildVulkanRecordingBatchPlan(
				publication, profile, TerraEngine::Renderer::RenderBackendWorkKind::Geometry);
			context.ExpectEqual(static_cast<std::size_t>(1u), vulkanPlan.workerBatchCount,
				"Typed publication ranges should become worker recording batches.");
			context.Expect(vulkanPlan.batches.front().batchId.find("geometry") != std::string::npos,
				"Batch ids should include deterministic work kind names.");
		});

		registry.Add("Renderer.VulkanRecording.PreparesImmutableBackendPlans", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>();
			publication->frameId = 6u;
			publication->resourcePreparation.generation = 77u;
			RenderResourcePreparationRecord resident{};
			resident.resourceId = TerraEngine::GenerateUUID();
			resident.resourceGeneration = 1u;
			resident.resident = true;
			RenderResourcePreparationRecord pending{};
			pending.resourceId = TerraEngine::GenerateUUID();
			pending.resourceGeneration = 1u;
			pending.resident = false;
			publication->resourcePreparation = BuildRenderResourcePreparationTable(
				std::span<const RenderResourcePreparationRecord>(&resident, 1u), 77u);
			publication->resourcePreparation.records.push_back(pending);
			std::sort(publication->resourcePreparation.records.begin(),
				publication->resourcePreparation.records.end(), [](const auto& left, const auto& right) {
					return TerraEngine::ToHexString(left.resourceId)
						< TerraEngine::ToHexString(right.resourceId);
				});
			for(std::size_t index = 0u; index < publication->resourcePreparation.records.size(); ++index)
				publication->resourcePreparation.records[index].preparationIndex =
					static_cast<std::uint32_t>(index);
			auto persistent = std::make_shared<RenderPersistentPage>();
			persistent->generation = 1u;
			RenderPersistentSlot slot{};
			slot.key = { 0u, 1u };
			slot.objectId = TerraEngine::GenerateUUID();
			slot.meshId = TerraEngine::GenerateUUID();
			slot.materialId = TerraEngine::GenerateUUID();
			slot.meshGeneration = slot.materialGeneration = slot.resourceGeneration = 1u;
			slot.packetTemplateIdentity = 1u;
			slot.occupied = true;
			persistent->slots.push_back(slot);
			auto persistentRoot = std::make_shared<RenderPersistentTopologyRoot>();
			persistentRoot->generation = 1u;
			persistentRoot->pages.push_back(std::move(persistent));
			publication->persistent = std::move(persistentRoot);

			const auto descriptors = TerraEngine::Renderer::Vulkan::BuildDescriptorBindingPlan(*publication, 2u);
			const auto uploads = TerraEngine::Renderer::Vulkan::BuildUploadPreparationPlan(*publication);
			const auto keys = TerraEngine::Renderer::Vulkan::BuildPipelineStateKeys(*publication);

			context.ExpectEqual(static_cast<std::uint64_t>(6u), descriptors.frameId, "Descriptor plans should carry immutable frame identity.");
			context.ExpectEqual(static_cast<std::uint64_t>(77u), uploads.resourceRevision, "Upload plans should preserve publication resource generations.");
			context.ExpectEqual(static_cast<std::size_t>(1u), uploads.payloads.size(), "Only nonresident publication resources require upload payloads.");
			context.ExpectEqual(static_cast<std::size_t>(1u), keys.size(), "Pipeline keys should be generated from retained publication topology.");
		});

		registry.Add("Renderer.VulkanRecording.CollapsesInstancedPipelineStateKeys", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>();
			auto page = std::make_shared<RenderPersistentPage>();
			page->generation = 1u;
			const TerraEngine::UUID sharedMesh = TerraEngine::GenerateUUID();
			const TerraEngine::UUID sharedMaterial = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondMaterial = TerraEngine::GenerateUUID();
			// Two instances share one complete pipeline identity. A third material
			// requires one additional key, independent of how many objects use it.
			for(std::size_t index = 0u; index < 3u; ++index) {
				RenderPersistentSlot slot{};
				slot.key = { static_cast<std::uint32_t>(index), 1u };
				slot.objectId = TerraEngine::GenerateUUID();
				slot.meshId = sharedMesh;
				slot.materialId = index == 2u ? secondMaterial : sharedMaterial;
				slot.meshGeneration = slot.materialGeneration = slot.resourceGeneration = 1u;
				slot.packetTemplateIdentity = index + 1u;
				slot.occupied = true;
				page->slots.push_back(slot);
			}
			auto persistentRoot = std::make_shared<RenderPersistentTopologyRoot>();
			persistentRoot->generation = 1u;
			persistentRoot->pages.push_back(std::move(page));
			publication->persistent = std::move(persistentRoot);

			const auto keys =
				TerraEngine::Renderer::Vulkan::BuildPipelineStateKeys(*publication);
			context.ExpectEqual(static_cast<std::size_t>(2u), keys.size(),
				"Instanced draws must share one immutable pipeline-state key.");
		});

		registry.Add("Renderer.VulkanRecording.BoundsVirtualGeometryMaterialPipelineKeys", [](TestContext& context) {
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			MaterialCapabilitySignature compatible{};
			compatible.requiredFeatureMask = ToVirtualGeometryMaterialFeatureMask(
				VirtualGeometryMaterialFeature::VisibilityBuffer);
			compatible.programmableFeatureMask = ToVirtualGeometryMaterialFeatureMask(
				VirtualGeometryMaterialFeature::ProgrammableShading);
			compatible.shadingBin = 3u;
			compatible.maximumShadingBins = 4u;
			compatible.compatibilitySignature = 0x45u;
			compatible.valid = true;
			const auto programmable = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, compatible);
			compatible.shadingBin = compatible.maximumShadingBins;
			const auto fallback = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, compatible);
			context.Expect(!programmable.usesIndexedFallback
				&& programmable.shadingBin == 3u
				&& fallback.usesIndexedFallback && fallback.shadingBin == 0u,
				"Only a valid bounded TMVG signature may select a programmable Vulkan pipeline key.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityVariantUsesDistinctPipelineKey", [](TestContext& context) {
			MaterialCapabilitySignature capability{};
			capability.maximumShadingBins = 1u;
			capability.compatibilitySignature = 0x92u;
			capability.valid = true;
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const auto target0Only = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, capability, false);
			const auto identityTarget = TerraEngine::Renderer::Vulkan::BuildPipelineStateKey(
				materialId, meshId, capability, true);
			context.Expect(
				!target0Only.usesConventionalVisibilityIdentity
					&& identityTarget.usesConventionalVisibilityIdentity
					&& target0Only.sortKey != identityTarget.sortKey,
				"The typed conventional visibility shader variant must never reuse a target0-only Vulkan pipeline key.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityReadbackIsTypedAndFailClosed", [](TestContext& context) {
			TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult empty{};
			empty.completed = true;
			empty.identity = glm::uvec4(0u);
			context.Expect(!empty.authoritative && empty.identity == glm::uvec4(0u),
				"A zero uint4 identity must never be presented as an authoritative GPU pick.");

			const std::string source = RequiredVulkanContext(context);
			context.Expect(source.find("inFlightConventionalVisibilityIdentityReadbacks") != std::string::npos
				&& source.find("vkGetFenceStatus") != std::string::npos
				&& source.find("Identity pixel was zero") != std::string::npos,
				"The Vulkan identity path must publish only fence-completed, nonzero typed pixels.");
			const auto renderSystem = RequiredDefinition(context, "Engine/Renderer/RenderSystemCapture.cpp", "RenderSystem::DrainConventionalVisibilityIdentityReadbacks(");
			context.Expect(renderSystem.find("candidate.payloadDescriptorIndex == result.identity.z") != std::string::npos
				&& renderSystem.find("latestSceneAuthoritativeIdentityPick") != std::string::npos,
				"RenderSystem must map a completed identity only to its immutable packet descriptor record.");
		});

		registry.Add("Renderer.VulkanRecording.CleanupDestroysGroupedInstanceStreams", [](TestContext& context) {
			const auto initialization = RequiredDefinition(context, "Engine/Renderer/RHI/Vulkan/VulkanContextInit.cpp", "VulkanContext::Impl::Cleanup(");


			// The reusable streams are allocated once per frame slot. This source
			// contract prevents shutdown from reaching vkDestroyDevice with those
			// VMA allocations still alive after repeated static-scene submissions.
			const std::size_t cleanup = initialization.find("VulkanContext::Impl::Cleanup(bool skipDeviceIdle)");
			const std::size_t groupedStreams = initialization.find("for (VulkanBuffer& buffer : groupedInstanceBuffers)", cleanup);
			const std::size_t vmaTeardown = initialization.find("vmaDestroyAllocator", cleanup);
			context.Expect(cleanup != std::string::npos && groupedStreams != std::string::npos
				&& vmaTeardown != std::string::npos && groupedStreams < vmaTeardown,
				"Cleanup must contain ordered grouped-stream and VMA teardown regions.");
			const auto grouped = initialization.substr(groupedStreams, vmaTeardown - groupedStreams);
			context.Expect(grouped.find("DestroyBufferImmediate(buffer);") != std::string::npos
				&& grouped.find("groupedInstanceCapacities.fill(0u);") != std::string::npos
				&& grouped.find("groupedInstanceBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;") != std::string::npos,
				"Vulkan cleanup must release every grouped instance stream before VMA/device teardown.");

		});

		registry.Add("Renderer.VulkanRecording.CaptureIdentityExcludesShadowOffscreenOutputs", [](TestContext& context) {
			const auto vulkanContext = RequiredDefinition(context, "Engine/Renderer/RHI/Vulkan/VulkanFrameRendering.cpp", "VulkanContext::RenderFrame(");


			// Shadow diagnostics share the OffscreenTexture classification, but they
			// do not own the dedicated MCP capture framebuffer or its identity image.
			const std::size_t captureSelection = vulkanContext.find(
				"const bool renderingDedicatedCaptureTarget =");
			const std::size_t captureSelectionEnd = vulkanContext.find(
				"VkExtent2D requestedCaptureExtent",
				captureSelection);
			const std::string capturePredicate = captureSelection != std::string::npos
				&& captureSelectionEnd != std::string::npos
				? vulkanContext.substr(captureSelection,
					captureSelectionEnd - captureSelection)
				: std::string{};
			context.Expect(!capturePredicate.empty()
				&& capturePredicate.find(
					"outputRequest.kind == RenderOutputKind::OffscreenTexture")
					!= std::string::npos
				&& capturePredicate.find(
					"outputRequest.diagnosticsView == RenderViewType::Game")
					!= std::string::npos
				&& capturePredicate.find(
					"outputRequest.diagnosticsView == RenderViewType::Scene")
					!= std::string::npos
				&& vulkanContext.find(
					"const VkImageView vrsColorView = renderingDedicatedCaptureTarget")
					!= std::string::npos,
				"Only Game/Scene offscreen outputs may select captureView; Shadow offscreen outputs must not clear capture identity.");
		});

		registry.Add("Renderer.VulkanRecording.ConventionalIdentityBlendRequiresIndependentBlend", [](TestContext& context) {
			const std::string vulkanContext =
				RequiredVulkanContext(context);
			const std::filesystem::path initializationSource = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Renderer" / "RHI" / "Vulkan" / "VulkanContextInit.cpp";
			std::ifstream initializationStream(initializationSource, std::ios::binary);
			context.Expect(initializationStream.good(), "Required source-contract input must be readable.");
			const std::string initialization{ std::istreambuf_iterator<char>(initializationStream), {} };

			// Target0-only pipelines deliberately differ from the integer identity
			// attachment, so the required core feature must be both checked and enabled.
			context.Expect(vulkanContext.find("identityBlendAttachment.colorWriteMask = 0u;")
					!= std::string::npos
				&& vulkanContext.find("identityBlendAttachment.blendEnable = VK_FALSE;")
					!= std::string::npos
				&& initialization.find("features2.features.independentBlend != VK_TRUE")
					!= std::string::npos
				&& initialization.find("deviceFeatures.independentBlend = VK_TRUE;")
					!= std::string::npos
				&& initialization.find("independentBlend is required for the conventional two-target visibility pass.")
					!= std::string::npos,
				"Conventional two-target pipelines must reject devices without and enable core independentBlend.");
		});

		registry.Add("Renderer.VulkanRecording.PrimaryCommandBuffersSubmitOncePerFrameSlot", [](TestContext& context) {
			const std::string vulkanContext =
				RequiredVulkanContext(context);
			const std::string resources =
				RequiredVulkanResources(context);
			// The marker is set after queue submission and cleared only when the
			// owning frame slot becomes reusable. Descriptor-mutation prefix
			// submissions must use the same ownership marker because asset
			// finalization can re-enter that path before the outer call unwinds.
			context.Expect(vulkanContext.find("pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] != 0u")
					!= std::string::npos
				&& resources.find("pendingPrimaryCommandBuffersSubmitted[currentFrame] = 1u;")
					!= std::string::npos
				&& resources.find("descriptor mutation editor render semaphore drain")
					!= std::string::npos
				&& resources.find("editorRenderSubmitted = false;")
					!= std::string::npos
				&& vulkanContext.find("BeginFrame") != std::string::npos,
				"EndFrame and descriptor mutation must reject a second primary submission and consume an editor render signal before reusing its binary semaphore.");
		});

		registry.Add("Renderer.VulkanRecording.ThreadPlanCoversAllThreadingStages", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 9u;
			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
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
			profile.supportsPacketLocalDiagnostics = true;
			profile.supportsRenderTaskGroups = true;

			auto publication = BuildRendererThreadingPublication(snapshot);
			publication.rendererSessionGeneration = 1u;
			publication.backendResourceGeneration = 1u;
			const auto plan = TerraEngine::Renderer::Vulkan::BuildVulkanRendererThreadingPlan(publication, profile);
			// ImplementationStage adds four explicit animation stages: two worker-safe planning
			// stages and two protected recording/completion stages. Keeping them in
			// this count makes scheduler ownership changes visible to diagnostics.
			context.ExpectEqual(static_cast<std::size_t>(24u), plan.stages.size(), "The Vulkan threading plan should cover all selected renderer and animation threading opportunities.");
			context.Expect(plan.workerEligibleStageCount >= 21u, "All planning stages should be worker eligible when backend capabilities allow it.");
			context.ExpectEqual(static_cast<std::size_t>(3u), plan.coordinatorStageCount, "Final submission plus animation recording and completion must remain protected coordinator stages.");
			context.Expect(plan.stages.back().stageId == "final-submit-present-retire", "The coordinator boundary should be explicit and easy to diagnose.");
		});

		registry.Add("Renderer.VulkanRecording.CachesStablePreparationPlans", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 10u;
			snapshot.resourceRevision = 44u;
			snapshot.sceneRevision = 91u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1280u, 720u), 2u });
			snapshot.materials.push_back({ TerraEngine::GenerateUUID(), "mat", true, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "tex", true, false });
			snapshot.meshes.push_back({ TerraEngine::GenerateUUID(), "mesh", true, false });
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), snapshot.meshes.front().resourceId, snapshot.materials.front().resourceId });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsParallelCulling = true;
			profile.supportsParallelDescriptorPreparation = true;
			profile.supportsParallelPipelineStatePreparation = true;
			profile.supportsParallelUploadPlanning = true;
			profile.supportsParallelUiPreparation = true;
			profile.maxRecordingWorkers = 4u;
			const std::vector<TerraEngine::Renderer::RenderPassNode> passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto publication = BuildRendererThreadingPublication(snapshot);

			TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
			const auto first = cache.BuildFramePreparation(publication, passes, profile);
			const auto second = cache.BuildFramePreparation(publication, passes, profile);
			const auto compact = cache.BuildFramePreparation(
				publication, passes, profile, false);
			auto recoveredPublication = publication;
			recoveredPublication.backendResourceGeneration = 2u;
			const auto recovered = cache.BuildFramePreparation(
				recoveredPublication, passes, profile);

			context.ExpectEqual(static_cast<std::size_t>(0u), first.cacheHitCount, "The first frame should populate preparation caches.");
			context.Expect(second.cacheHitCount >= 3u, "Stable frames should reuse descriptor, pipeline, and upload preparation caches.");
			context.ExpectEqual(static_cast<std::size_t>(1u), second.renderPacketCache.cacheHits, "Persistent draw packet signatures should hit for unchanged renderables.");
			context.ExpectEqual(static_cast<std::size_t>(0u), second.renderPacketCache.cacheMisses, "Unchanged renderables should not rebuild persistent draw packets.");
			context.ExpectEqual(static_cast<std::size_t>(1u), compact.renderPacketCache.packetCount,
				"Compact preparation should retain the packet population count.");
			context.ExpectEqual(static_cast<std::size_t>(0u), compact.renderPacketCache.cacheHits,
				"Compact preparation must skip diagnostic-only persistent packet scans.");
			context.ExpectEqual(static_cast<std::size_t>(0u), recovered.cacheHitCount,
				"A successful presentation recovery must rebuild value-only preparation caches.");
			context.ExpectEqual(static_cast<std::size_t>(1u), recovered.renderPacketCache.cacheMisses,
				"Persistent packet signatures must not survive a backend resource generation change.");
		});

		registry.Add("Renderer.VulkanRecording.StatsKeepPublicationAndCaptureProvenanceSeparate", [](TestContext& context) {
			const std::string source = RequiredVulkanContext(context);

			// Game and capture share a camera type but not an output target. Capture-
			// only tools still need renderer counters, so the delayed snapshot carries
			// both the sealed publication and exact output provenance.
			RenderStatsSnapshot stats{};
			stats.outputKind = RenderOutputKind::OffscreenTexture;
			stats.outputId = 9000u;
			stats.dedicatedCaptureOutput = true;
			stats.publicationFrameId = 17u;
			stats.publicationGeneration = 23u;
			stats.rendererSessionGeneration = 5u;
			context.Expect(stats.outputKind == RenderOutputKind::OffscreenTexture
				&& stats.outputId == 9000u && stats.dedicatedCaptureOutput
				&& stats.publicationFrameId == 17u
				&& stats.publicationGeneration == 23u
				&& stats.rendererSessionGeneration == 5u
				&& source.find("pendingStats.publicationFrameId = publication->frameId;") != std::string::npos
				&& source.find("pendingStats.dedicatedCaptureOutput = renderingDedicatedCaptureTarget;") != std::string::npos,
				"Delayed stats must retain publication and exact output identity for visible and dedicated capture targets.");
		});

		registry.Add("Renderer.VulkanRecording.GroupedInstancesUseWholeStreamBoundsAndSnapshotIdentity", [](TestContext& context) {
			const std::filesystem::path taskShaderPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Shaders" / "Default" / "Default.task.hlsl";
			const std::filesystem::path meshShaderPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Shaders" / "Default" / "Default.mesh.hlsl";
			const std::filesystem::path fragmentShaderPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine" / "Shaders" / "Default" / "Default.frag.hlsl";
			std::ifstream taskStream(taskShaderPath, std::ios::binary);
			context.Expect(taskStream.good(), "Required source-contract input must be readable.");
			std::ifstream meshStream(meshShaderPath, std::ios::binary);
			context.Expect(meshStream.good(), "Required source-contract input must be readable.");
			std::ifstream fragmentStream(fragmentShaderPath, std::ios::binary);
			context.Expect(fragmentStream.good(), "Required source-contract input must be readable.");
			const std::string taskShader{ std::istreambuf_iterator<char>(taskStream), {} };
			const std::string meshShader{ std::istreambuf_iterator<char>(meshStream), {} };
			const std::string fragmentShader{ std::istreambuf_iterator<char>(fragmentStream), {} };
			const auto renderSystem = RequiredDefinition(context, "Engine/Renderer/RenderSystem.cpp", "RenderSystem::RequestConventionalVisibilityIdentityPick(");
			const std::string resources = RequiredVulkanResources(context);
			const std::string vulkan = RequiredVulkanContext(context);

			// Later groups have a nonzero firstInstance. Their task payload must be
			// validated against the complete stream, not only the local group count,
			// or every rejected load reuses the representative transform. The same
			// loaded record must also carry its owner key through mesh output and the
			// dedicated identity fragment; otherwise an apparently valid capture
			// could report a neighbouring representative instead of the hit owner.
			context.Expect(taskShader.find("groupedStreamRecordCount") != std::string::npos
				&& taskShader.find("groupedFirstInstance + groupedInstanceRelative") != std::string::npos
				&& taskShader.find("SceneUniforms.model[1].w") != std::string::npos
				&& taskShader.find("SceneUniforms.model[2].w") != std::string::npos
				&& taskShader.find("SceneUniforms.model[3].y") == std::string::npos
				&& taskShader.find("SceneUniforms.model[3].z") == std::string::npos
				&& meshShader.find("asuint(SceneUniforms.model[0].w)") != std::string::npos
				&& meshShader.find("asuint(SceneUniforms.model[3].w), instanceRecord") != std::string::npos
				&& meshShader.find("gMeshShaderObjectIdentity = instanceRecord.objectIdentity;")
					!= std::string::npos
				&& meshShader.find("outVertex.objectIdentity = gMeshShaderObjectIdentity;")
					!= std::string::npos
				&& vulkan.find("push.model[3] = glm::vec4(") != std::string::npos
				&& fragmentShader.find("const uint identityIndex = objectIdentity.x != 0u") != std::string::npos
				&& fragmentShader.find("MakeConventionalVisibilityOutput(colorValue, primitiveId, input.objectIdentity)")
					!= std::string::npos
				&& fragmentShader.find("SceneUniforms.selectedVirtualPageNeedOffset") != std::string::npos
				&& vulkan.find("push.indirectDrawOffset = renderSnapshot.indirectDrawOffset;") != std::string::npos
				&& vulkan.find("push.selectedVirtualPageNeedOffset = command.conventionalVisibilityIdentityIndex;") != std::string::npos
				&& renderSystem.find("conventionalVisibilityIdentityTable") != std::string::npos
				&& renderSystem.find("latestRenderFrameSnapshot.frameId") != std::string::npos
				&& resources.find("pipeline.usesConventionalVisibilityIdentity = useConventionalIdentityVariant")
					!= std::string::npos,
				"Grouped transforms and direct typed picks must resolve through whole-stream bounds and the immutable frame identity table.");
		});

		registry.Add("Renderer.VulkanRecording.ComputeRasterPreservesPickingIdentity", [](TestContext& context) {
			// Source-contract coverage spans the CPU packet and shared shader ABI;
			// the installed-editor click check additionally verifies actual readback.
			const auto read = [&](const char* path) {
				std::ifstream stream(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path, std::ios::binary);
			context.Expect(stream.good(), "Required source-contract input must be readable.");
				return std::string(std::istreambuf_iterator<char>(stream), {});
			};
			const auto resources = read("Engine/Renderer/RHI/Vulkan/VulkanVirtualGeometryResources.cpp");
			const auto setup = read("Engine/Shaders/VirtualGeometry/VirtualGeometryTriangleSetup.comp.hlsl");
			const auto composite = read("Engine/Shaders/VirtualGeometry/VirtualGeometryComposite.frag.hlsl");
			context.Expect(resources.find("command.conventionalVisibilityIdentityIndex });") != std::string::npos
				&& resources.find("drawEvidence->visibilityIdentityIndex,") != std::string::npos,
				"Compute packet setup must retain the resolved command's immutable object key, not zero.");
			context.Expect(setup.find("output.debugState = packet.debugState;") != std::string::npos
				&& composite.find("source.debugState.y,") != std::string::npos,
				"Triangle setup and composite must carry the object key into the typed identity attachment.");
		});

		registry.Add("Renderer.VulkanRecording.DedicatedCaptureIdentityUsesRetainedSnapshot", [](TestContext& context) {
			const auto admission = RequiredDefinition(context, "Engine/Renderer/RenderSystem.cpp", "RenderSystem::RequestConventionalVisibilityIdentityPick(");
			const auto observations = RequiredDefinition(context, "Engine/Renderer/Frame/RenderFrameSealing.cpp", "RenderSystem::ObserveFrameViewOutputs(");
			const auto readback = RequiredDefinition(context, "Engine/Renderer/RenderSystemCapture.cpp", "RenderSystem::DrainConventionalVisibilityIdentityReadbacks(");
			const auto publication = RequiredDefinition(context, "Engine/Renderer/Frame/RenderDiagnosticsPublication.cpp", "RenderSystem::PublishFrameDiagnostics(");
			const auto vulkan = RequiredVulkanContext(context);
			context.Expect(admission.find("latestGameCaptureIdentitySnapshot") != std::string::npos
				&& observations.find("completedGameCaptureIdentitySnapshot.frameId =") != std::string::npos
				&& observations.find("prepared.selection.generation") != std::string::npos
				&& publication.find("latestGameCaptureIdentitySnapshot = std::move(completedGameCaptureIdentitySnapshot)") != std::string::npos
				&& readback.find("result.dedicatedCaptureTarget") != std::string::npos
				&& readback.find("identityTable.begin()") != std::string::npos
				&& vulkan.find("rendered target metadata does not match the request") != std::string::npos,
				"Dedicated capture identity picks must retain matching immutable frame data and diagnose target metadata rejection.");

		});

		registry.Add("Renderer.VulkanRecording.InvalidatesUploadPlansAndRetiresResourcesDeterministically", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.resourceRevision = 1u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(640u, 360u), 1u });
			snapshot.meshes.push_back({ TerraEngine::GenerateUUID(), "streaming-mesh", false, false });
			snapshot.textures.push_back({ TerraEngine::GenerateUUID(), "streaming-texture", false, false });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelUploadPlanning = true;
			profile.supportsParallelRecording = true;
			profile.maxRecordingWorkers = 2u;
			const std::vector<TerraEngine::Renderer::RenderPassNode> passes = TerraEngine::Renderer::BuildDefaultRenderPasses(snapshot, profile);
			const auto firstPublication = BuildRendererThreadingPublication(snapshot);

			TerraEngine::Renderer::Vulkan::VulkanRendererPreparationCache cache;
			const auto first = cache.BuildFramePreparation(firstPublication, passes, profile);
			snapshot.resourceRevision = 2u;
			const auto secondPublication = BuildRendererThreadingPublication(snapshot);
			const auto second = cache.BuildFramePreparation(secondPublication, passes, profile);

			cache.EnqueueResourceForRetirement({ 7u, 21u, "old-buffer" });
			const auto tooEarly = cache.RetireCompletedResources(20u);
			const auto retired = cache.RetireCompletedResources(21u);

			context.Expect(first.uploadPlan.queuedForUploadWorker, "Missing resources should create immutable upload packets for worker preparation.");
			context.Expect(!second.uploadPlan.cacheHit, "Changing the resource revision should invalidate upload preparation.");
			context.ExpectEqual(static_cast<std::size_t>(0u), tooEarly.size(), "Resource retirement must wait until the completed frame reaches the fence frame.");
			context.ExpectEqual(static_cast<std::size_t>(1u), retired.size(), "Completed frame retirement should release eligible resource records deterministically.");
		});

		registry.Add("Renderer.VulkanRecording.BuildsAllRemainingPreparationPlans", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 30u;
			snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1920u, 1080u), 1u });
			for (std::size_t index = 0u; index < 128u; ++index)
			{
				snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
				snapshot.materials.push_back({ snapshot.renderables.back().materialId, "mat", true, false });
			}
			snapshot.lights.push_back({ TerraEngine::GenerateUUID(), 1u, true, false });

			TerraEngine::Renderer::BackendCapabilityProfile profile;
			profile.supportsParallelRecording = true;
			profile.supportsSecondaryRecording = true;
			profile.supportsParallelShadowRecording = true;
			profile.supportsParallelCulling = true;
			profile.supportsParallelPipelineStatePreparation = true;
			profile.supportsParallelUiPreparation = true;
			profile.maxRecordingWorkers = 8u;

			const auto publication = BuildRendererThreadingPublication(snapshot);
			const auto visibility = TerraEngine::Renderer::Vulkan::BuildVisibilityPreparationPlan(publication, profile);
			const auto indirect = TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(publication, profile);
			const auto shadows = TerraEngine::Renderer::Vulkan::BuildShadowRecordingPreparationPlan(publication, profile);
			const auto shaderMaterials = TerraEngine::Renderer::Vulkan::BuildShaderMaterialPreparationPlan(publication, profile);
			const auto ui = TerraEngine::Renderer::Vulkan::BuildUiGeometryPreparationPlan(publication, profile);

			context.Expect(visibility.workerEligible, "Large visibility and light assignment work should be worker eligible.");
			context.Expect(indirect.gpuDrivenEligible, "Renderable snapshots should be convertible into GPU-driven indirect draw preparation.");
			context.ExpectEqual(static_cast<std::size_t>(6u), shadows.secondaryRecordingCount, "Point-light shadows should prepare one secondary recording per cube face.");
			context.Expect(shaderMaterials.workerEligible, "Large material sets should be prepared off the coordinator path.");
			context.ExpectEqual(static_cast<std::size_t>(6u), ui.estimatedVertexCount, "UI preparation should estimate a quad for each renderable UI view.");
		});
	}

}
