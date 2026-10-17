#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Core/Logger/Logger.h"
#include "Profiling/Profiler.h"
#include "RenderPreparationTasks.h"
#include "Renderer/ShadowWorkPlan.h"
// View lowering creates immutable selector/presentation owners consumed by the sealing stage.
#include "RenderFrameSealing.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Profiling/ProfilerScope.h"
// Preserve typed output order and transfer existing owning arrays without a second frame copy.
TerraEngine::Renderer::RenderViewPresentationInput RenderSystem::MakeFramePresentationInput(
	RenderViewBuildState&& frame, const RenderOutputRequest& output)
{
		// Transfer only presentation state and backend-neutral evidence. Command
		// materialization intentionally stays out of this new contract: the
		// compact selection and typed submission ranges below describe geometry.
		// The caller has finished the view build, so moving its vectors preserves
		// single ownership and avoids a crowd-sized copy at the queue boundary.
		TerraEngine::Renderer::RenderViewPresentationInput input{};
		input.output = output;
		input.referenceFrame = frame.referenceFrame;
		input.viewMatrix = frame.viewMatrix;
		input.projectionMatrix = frame.projectionMatrix;
		input.cameraLocalPosition = frame.cameraLocalPosition;
		input.deltaTimeSeconds = frame.deltaTimeSeconds;
		input.globalTaskShaderCullingEnabled = frame.globalTaskShaderCullingEnabled;
		input.cullingEpsilon = frame.cullingEpsilon;
		input.activePreset = frame.activePreset;
		input.customOverrideActive = frame.customOverrideActive;
		input.settings = frame.settings;
		input.efficiencyOptions = frame.efficiencyOptions;
		input.efficiencyFixtureIntent = frame.efficiencyFixtureIntent;
		if (input.efficiencyFixtureIntent.controlsApplied && input.efficiencyFixtureIntent.depthControls
			&& input.efficiencyFixtureIntent.depthControls->oracleRecipe) {
			// This conversion already runs in the view preparation job (or the
			// explicitly requested inline reference). Only immutable owned controls
			// are read; no GPU mapping or live scene mutation happens on the worker.
			const auto& recipe=*input.efficiencyFixtureIntent.depthControls->oracleRecipe;
			auto pixels=std::make_shared<std::vector<std::uint32_t>>(std::size_t(recipe.extent.width)*recipe.extent.height);
			if (TerraEngine::Renderer::LightingDepthReduction::MaterializeDepthRecipe(recipe,*pixels))
				input.efficiencyDepthOracleInput=std::move(pixels);
			else {
				input.efficiencyFixtureIntent.controlsApplied=false;
				input.efficiencyFixtureIntent.controlReason="Captured isolated depth recipe could not materialize atomically.";
			}
		}
		input.renderOptimization = frame.renderOptimization;
		input.groupedInstanceRecords = std::move(frame.groupedInstanceRecords);
		input.groupedSubmissions = std::move(frame.groupedSubmissions);
		input.drawResourcePreparationCommandIndices =
			std::move(frame.drawResourcePreparationCommandIndices);
		input.sky = frame.sky;
		input.lightCandidates = std::move(frame.lightCandidates);
		input.editorContextTint = frame.editorContextTint;
		input.sceneViewGridVisible = frame.sceneViewGridVisible;
		input.sceneViewOrthographic = frame.sceneViewOrthographic;
		input.shadowDrawLists = std::move(frame.shadowDrawLists);
		input.uiBatch = std::move(frame.uiBatch);
		input.virtualGeometryPacketEvidenceSignature =
			frame.virtualGeometryPacketEvidenceSignature;
		input.virtualGeometryVisibilitySetupSignature =
			frame.virtualGeometryVisibilitySetupSignature;
		input.retainedVirtualGeometryGpuEvidence =
			std::move(frame.retainedVirtualGeometryGpuEvidence);
		if(input.retainedVirtualGeometryGpuEvidence == nullptr)
		{
			// Standalone frame data owns its packet vectors. Transfer them in this
			// path; cached evidence remains shared through the immutable root.
			input.virtualGeometryPackets = std::move(frame.virtualGeometryGpuPackets);
			input.virtualGeometryBatches = std::move(frame.virtualGeometryGpuBatches);
			input.virtualGeometryFrameSets = std::move(frame.virtualGeometryGpuFrameSets);
			input.virtualGeometryCompactionDispatches =
				std::move(frame.virtualGeometryGpuCompactionDispatches);
			input.virtualGeometryGeneratedSubmissions =
				std::move(frame.virtualGeometryGeneratedSubmissions);
			input.virtualGeometryCounterReadbacks =
				std::move(frame.virtualGeometryCounterReadbacks);
			input.virtualGeometryFallbackRecords =
				std::move(frame.virtualGeometryGpuFallbackRecords);
		}
		input.virtualGeometryVisibilityDiagnosticsRequested =
			frame.virtualGeometryVisibilityDiagnosticsRequested;
		input.virtualGeometryPageRetirements =
			std::move(frame.virtualGeometryPageRetirements);
		input.virtualGeometryDecodedPageUploads =
			std::move(frame.virtualGeometryDecodedPageUploads);
		return input;
	}

// Preserve typed output order and transfer existing owning arrays without a second frame copy.
void RenderSystem::QueueFramePreparedSubmission(const FrameVisibilityInputs& inputs,
	std::vector<PreparedRenderSubmission>& preparedRenderSubmissions, RenderViewBuildState&& preparedFrame,
	RenderOutputRequest output, const std::vector<RenderItemRecord>& commands,
	const TerraEngine::Renderer::Frame::VisibleCommandCacheRecord* cachedRecord)
{
	const auto& retainedRenderTopology = inputs.retainedRenderTopology;
	const auto& renderableDrawCommands = inputs.renderableDrawCommands;

		PreparedRenderSubmission prepared{};
		prepared.output = std::move(output);
		prepared.selection.outputKind = prepared.output.kind;
		prepared.selection.viewType = prepared.output.diagnosticsView;
		prepared.selection.outputId = prepared.output.outputId;
		prepared.selection.generation = std::max<std::uint64_t>(
			1u, activeRenderFrameId);
		prepared.presentation = MakeFramePresentationInput(
			std::move(preparedFrame), prepared.output);
		const bool reuseCachedViewLanes = cachedRecord != nullptr
			&& prepared.presentation.editorContextTint == glm::vec4(0.0f)
			&& cachedRecord->stableOrdinals.size() == commands.size()
			&& cachedRecord->selectedLods.size() == commands.size()
			&& cachedRecord->routeSelectors.size() == commands.size()
			&& cachedRecord->editorTints.size() == commands.size()
			&& cachedRecord->commandLanes != nullptr
			&& cachedRecord->commandLanes->size() == commands.size();
		if(reuseCachedViewLanes)
		{
			// A revision-validated cache record already owns the exact compact lanes
			// produced by visibility selection. The first hit resolves stable indices
			// into lifetime keys; later frames share both immutable arrays directly.
			prepared.presentation.retainedCommandLanes = cachedRecord->commandLanes;
			if(cachedRecord->retainedViewSelection == nullptr
				|| cachedRecord->retainedCommandSelectionIndices == nullptr)
			{
				auto retainedSelection = std::make_shared<TerraEngine::Renderer::
					RenderViewSelectionPayload>();
				auto commandSelectionIndices =
					std::make_shared<std::vector<std::uint32_t>>();
				retainedSelection->visibleOrdinals.reserve(commands.size());
				retainedSelection->selectedLods.reserve(commands.size());
				retainedSelection->routeSelectors.reserve(commands.size());
				retainedSelection->packetTemplateIdentities.reserve(commands.size());
				retainedSelection->tintSelectors.reserve(commands.size());
				retainedSelection->editorTints.reserve(commands.size());
				commandSelectionIndices->reserve(commands.size());
				for(std::size_t index = 0u; index < commands.size(); ++index)
				{
					std::uint32_t visibleSelection = TerraEngine::Renderer::
						RenderSlotGenerationKey::kInvalidOrdinal;
					const std::uint32_t renderableIndex =
						cachedRecord->stableOrdinals[index];
					const std::uint32_t identityIndex = renderableIndex
						< renderableDrawCommands.size()
							? renderableDrawCommands[renderableIndex].
								conventionalVisibilityIdentityIndex : 0u;
					if(identityIndex != 0u)
					{
						const std::uint32_t ordinal = identityIndex - 1u;
						const std::shared_ptr<const TerraEngine::Renderer::
							RenderPersistentTopologyRoot>& slotTopology =
								retainedRenderTopology.slotAllocator.topology;
						const std::size_t pageIndex = ordinal
							/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
						const std::size_t slotIndex = ordinal
							% TerraEngine::Renderer::kRenderPublicationPageCapacity;
						const TerraEngine::Renderer::RenderPersistentSlot* persistentSlot =
							slotTopology != nullptr && pageIndex < slotTopology->pages.size()
								&& slotTopology->pages[pageIndex] != nullptr
								&& slotIndex < slotTopology->pages[pageIndex]->slots.size()
									? &slotTopology->pages[pageIndex]->slots[slotIndex] : nullptr;
						if(persistentSlot != nullptr && persistentSlot->IsComplete())
						{
							visibleSelection = static_cast<std::uint32_t>(
								retainedSelection->visibleOrdinals.size());
							retainedSelection->visibleOrdinals.push_back(persistentSlot->key);
							retainedSelection->selectedLods.push_back(
								cachedRecord->selectedLods[index]);
							retainedSelection->routeSelectors.push_back(
								cachedRecord->routeSelectors[index]);
							retainedSelection->packetTemplateIdentities.push_back(
								persistentSlot->packetTemplateIdentity);
							const glm::vec4& editorTint = cachedRecord->editorTints[index];
							retainedSelection->tintSelectors.push_back(
								editorTint == glm::vec4(0.0f) ? 0u : 1u);
							retainedSelection->editorTints.push_back(editorTint);
						}
					}
					commandSelectionIndices->push_back(visibleSelection);
				}
				cachedRecord->retainedViewSelection = std::move(retainedSelection);
				cachedRecord->retainedCommandSelectionIndices =
					std::move(commandSelectionIndices);
			}
			prepared.selection.retainedPayload = cachedRecord->retainedViewSelection;
			prepared.presentation.retainedCommandSelectionIndices =
				cachedRecord->retainedCommandSelectionIndices;
		}
		else
		{
			prepared.presentation.commandLanes.reserve(commands.size());
			prepared.presentation.commandSelectionIndices.reserve(commands.size());
			for(const RenderItemRecord& command : commands)
			{
				prepared.presentation.commandLanes.push_back(
					MakeRenderViewCommandLane(command));
				std::uint32_t visibleSelection = TerraEngine::Renderer::
					RenderSlotGenerationKey::kInvalidOrdinal;
				if(command.conventionalVisibilityIdentityIndex != 0u)
				{
					const std::uint32_t ordinal =
						command.conventionalVisibilityIdentityIndex - 1u;
					const std::shared_ptr<const TerraEngine::Renderer::RenderPersistentTopologyRoot>&
						slotTopology = retainedRenderTopology.slotAllocator.topology;
					const std::size_t pageIndex = ordinal
						/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
					const std::size_t slotIndex = ordinal
						% TerraEngine::Renderer::kRenderPublicationPageCapacity;
					const TerraEngine::Renderer::RenderPersistentSlot* persistentSlot =
						slotTopology != nullptr && pageIndex < slotTopology->pages.size()
							&& slotTopology->pages[pageIndex] != nullptr
							&& slotIndex < slotTopology->pages[pageIndex]->slots.size()
								? &slotTopology->pages[pageIndex]->slots[slotIndex] : nullptr;
					if(persistentSlot != nullptr && persistentSlot->IsComplete())
					{
						visibleSelection = static_cast<std::uint32_t>(
							prepared.selection.visibleOrdinals.size());
						prepared.selection.visibleOrdinals.push_back(persistentSlot->key);
						prepared.selection.selectedLods.push_back(command.selectedVirtualLod);
						prepared.selection.routeSelectors.push_back(
							static_cast<std::uint32_t>(command.geometryRoute));
						prepared.selection.packetTemplateIdentities.push_back(
							persistentSlot->packetTemplateIdentity);
						prepared.selection.tintSelectors.push_back(
							command.editorTint == glm::vec4(0.0f) ? 0u : 1u);
						prepared.selection.editorTints.push_back(command.editorTint);
					}
				}
				prepared.presentation.commandSelectionIndices.push_back(visibleSelection);
			}
		}
		preparedRenderSubmissions.push_back(std::move(prepared));
	}

// Preserve typed output order and transfer existing owning arrays without a second frame copy.
void RenderSystem::CopyFrameVirtualGeometryPayload(RenderViewBuildState& target,
	const TerraEngine::Renderer::Frame::VisibleCommandBuildResult& source)
{
		target.virtualGeometryPacketEvidenceSignature = source.packetEvidenceSignature;
		if(source.cachedRecord != nullptr) {
			// The cache owns immutable evidence for at least this prepared view's
			// lifetime. Share it through backend submission instead of cloning several
			// packet tables that no stage is permitted to mutate.
			target.retainedVirtualGeometryGpuEvidence =
				source.cachedRecord->retainedGpuEvidence;
			target.virtualGeometryGpuPackets.clear();
			target.virtualGeometryGpuBatches.clear();
			target.virtualGeometryGpuFrameSets.clear();
			target.virtualGeometryGpuCompactionDispatches.clear();
			target.virtualGeometryGeneratedSubmissions.clear();
			target.virtualGeometryCounterReadbacks.clear();
			target.virtualGeometryGpuFallbackRecords.clear();
		}
		else {
			target.retainedVirtualGeometryGpuEvidence.reset();
			target.virtualGeometryGpuPackets = source.GpuPacketRecords();
			target.virtualGeometryGpuBatches = source.GpuBatchDescriptors();
			target.virtualGeometryGpuFrameSets = source.GpuPacketFrameSets();
			target.virtualGeometryGpuCompactionDispatches =
				source.GpuCompactionDispatches();
			target.virtualGeometryGeneratedSubmissions =
				source.GeneratedSubmissions();
			target.virtualGeometryCounterReadbacks = source.GpuCounterReadbacks();
			target.virtualGeometryGpuFallbackRecords = source.GpuFallbackRecords();
		}
		// The compute visibility mirror is intentionally opt-in. Ordinary dense VG
		// already renders through mesh tasks, so rebuilding CPU triangles and full
		// pixel buffers without a diagnostics consumer only duplicates frame work.
		target.virtualGeometryVisibilityDiagnosticsRequested =
			TerraEngine::Renderer::RenderDiagnosticsRequested()
			|| std::any_of(source.GpuPacketFrameSets().begin(),
				source.GpuPacketFrameSets().end(),
				[](const GpuVirtualGeometryPacketFrameSet& frameSet) {
					return frameSet.debugMode
						!= VirtualGeometryDebugVisualizationMode::None;
				});
	}

// Preserve typed output order and transfer existing owning arrays without a second frame copy.
void RenderSystem::ApplyFrameSceneContextTint(RenderViewBuildState& target, std::vector<RenderItemRecord>& commands)
{
		if (!sceneViewContextTintEnabled || sceneViewContextFocusedObjectIds.empty())
		{
			return;
		}

		// Prefab context mode keeps the opened prefab branch in normal material
		// color and gently washes every surrounding scene command toward white.
		// This runs only on the Scene View command list before submission.
		const glm::vec4 contextTint(1.0f, 1.0f, 1.0f, 0.25f);
		target.editorContextTint = contextTint;
		// Focus membership is editor state outside immutable scene visibility
		// evidence. Force compute-raster setup to rebuild when context tint is active.
		target.virtualGeometryVisibilitySetupSignature = 0u;
		for (RenderItemRecord& command : commands)
		{
			command.editorTint = sceneViewContextFocusedObjectIds.contains(command.objectId)
				? glm::vec4(0.0f)
				: contextTint;
		}
	}

// The caller owns each captured input through synchronous completion and admitted-child settlement.
std::shared_ptr<TerraEngine::Renderer::RenderFramePublication> RenderSystem::BeginFramePublication(
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot, const FrameCaptureRoots& roots,
	const TerraEngine::Renderer::Frame::RetainedRenderTopologyState& retainedRenderTopology,
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{
	const auto snapshotRendererGeneration = roots.protectedRendererGeneration;
	const auto protectedBackendResourceGeneration = roots.protectedBackendResourceGeneration;
	const auto protectedSceneGeneration = roots.protectedSceneGeneration;
	const auto& gpuPresentation = roots.gpuPresentation;
		// Renderer recovery creates a new publication session.  Resetting before
		// building the candidate guarantees a pre-loss root is never paired with
		// post-recovery Vulkan resources.
		const std::uint64_t publicationSessionGeneration =
			std::max<std::uint64_t>(1u, snapshotRendererGeneration);
		if(renderFramePublicationStore.SessionGeneration()
			!= publicationSessionGeneration)
		{
			renderFramePublicationStore.ResetForSession(
				publicationSessionGeneration);
		}
		const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
			previousPublication = renderFramePublicationStore.Load();
		auto publication = std::make_shared<TerraEngine::Renderer::RenderFramePublication>();
		publication->frameId = activeRenderFrameId;
		publication->referenceFrames.reserve(
			renderFrameSnapshot.viewRequests.size());
		for (const TerraEngine::Renderer::RenderFrameViewRequest& request :
			renderFrameSnapshot.viewRequests)
		{
			publication->referenceFrames.push_back(request.referenceFrame);
		}
		publication->rendererSessionGeneration = publicationSessionGeneration;
		publication->backendResourceGeneration = protectedBackendResourceGeneration;
		publication->sceneGeneration = protectedSceneGeneration;
		publication->animationPresentationGeneration = gpuPresentation != nullptr
			? gpuPresentation->presentationGeneration : 0u;
		publication->expectedPreviousGeneration = previousPublication != nullptr
			? previousPublication->generation : 0u;
		publication->generation = publication->expectedPreviousGeneration + 1u;
		renderGraphDiagnostics.publicationGeneration = publication->generation;
		renderGraphDiagnostics.rendererSessionGeneration = publicationSessionGeneration;
		const std::shared_ptr<const TerraEngine::Renderer::RenderPersistentTopologyRoot>
			allocatorTopology = retainedRenderTopology.slotAllocator.topology;
		if(allocatorTopology == nullptr
			|| allocatorTopology->rendererSessionGeneration != publicationSessionGeneration)
			throw std::runtime_error(
				"Renderer slot allocator has no topology root for the active session.");
		if(retainedPersistentTopologyRoot != nullptr)
		{
			const std::size_t sharedPageCount = std::min(
				retainedPersistentTopologyRoot->pages.size(), allocatorTopology->pages.size());
			for(std::size_t pageIndex = 0u; pageIndex < sharedPageCount; ++pageIndex)
			{
				if(retainedPersistentTopologyRoot->pages[pageIndex]
					== allocatorTopology->pages[pageIndex])
					++publication->receipt.reusedPersistentPages;
			}
		}
		retainedPersistentTopologyRoot = allocatorTopology;
		publication->persistent = allocatorTopology;

	return publication;
}

// The caller owns each captured input through synchronous completion and admitted-child settlement.
void RenderSystem::PrepareFrameBackendWork(const std::shared_ptr<TerraEngine::Renderer::RenderFramePublication>& publication,
	const FrameCaptureRoots& roots, std::shared_ptr<TerraEngine::Renderer::CpuAnimationDeformationPlan>& cpuDeformationPlan,
	TerraEngine::Renderer::DeformationBackendWorkPlan& selectedDeformation)
{
	const bool hasDenseAnimationPublication = roots.hasDenseAnimationTopologyCandidate;
	const auto denseAnimationOwnerCount = roots.denseAnimationOwnerCount;
	const auto& immutableGpuAnimationPlan = roots.immutableGpuAnimationPlan;
                TerraEngine::Renderer::AnimationBackendWorkPlan animationBackendWork;
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Animation Backend Work",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			animationBackendWork = hasDenseAnimationPublication
				? TerraEngine::Renderer::BuildDenseAnimationBackendWorkPlan(
					activeRenderFrameId, denseAnimationOwnerCount,
					immutableGpuAnimationPlan)
				: TerraEngine::Renderer::BuildAnimationBackendWorkPlan(
					activeRenderFrameId, *publication->dynamic,
					immutableGpuAnimationPlan);
		}
		if(cpuDeformationPlan != nullptr)
		{
			animationBackendWork.cpuOrFallbackAnimatorCount =
				cpuDeformationPlan->records.size();
			animationBackendWork.cpuDeformationPlan = std::move(cpuDeformationPlan);
		}
		const bool backendWorkValid = activeRenderFrameId != 0u
			&& animationBackendWork.frameId == activeRenderFrameId
			&& animationBackendWork.valid
			&& selectedDeformation.frameId == activeRenderFrameId
			&& selectedDeformation.valid
			&& (!selectedDeformation.immutablePlan
				|| selectedDeformation.immutablePlan->IsComplete());
		if(!backendWorkValid)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Renderer rejected selected animation or deformation work for frame "
					<< activeRenderFrameId << ".");
		}
		auto backendWork = std::make_shared<
			TerraEngine::Renderer::RenderBackendWorkPublication>();
		backendWork->frameId = activeRenderFrameId;
		backendWork->animationWork = std::move(animationBackendWork);
		backendWork->deformationWork = std::move(selectedDeformation.immutablePlan);
		backendWork->valid = backendWorkValid;
		publication->backendWork = std::move(backendWork);
}

// The caller owns each captured input through synchronous completion and admitted-child settlement.
void RenderSystem::LowerFrameViewPublications(const std::shared_ptr<TerraEngine::Renderer::RenderFramePublication>& publication,
	std::vector<PreparedRenderSubmission>& preparedRenderSubmissions,
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot)
{		for(PreparedRenderSubmission& prepared : preparedRenderSubmissions)
		{
			const std::uint32_t viewIndex = static_cast<std::uint32_t>(publication->views.size());
			std::vector<TerraEngine::Renderer::RenderSubmissionRange> ranges =
				TerraEngine::Renderer::BuildRenderSubmissionRanges(
					prepared.selection, viewIndex);
			publication->submissionRanges.insert(publication->submissionRanges.end(),
				std::make_move_iterator(ranges.begin()),
				std::make_move_iterator(ranges.end()));
			publication->views.push_back(std::move(prepared.selection));
			publication->presentationInputs.push_back(std::move(prepared.presentation));
		}
		// Resource readiness crosses the backend boundary once as compact identities.
		// Per-view ranges resolve these shared rows by stable resource UUID instead of
		// retaining the worker-only snapshot's mesh, material, or texture vectors.
		std::vector<TerraEngine::Renderer::RenderResourcePreparationRecord>
			resourcePreparationRecords;
		resourcePreparationRecords.reserve(
			renderFrameSnapshot.meshes.size()
			+ renderFrameSnapshot.materials.size()
			+ renderFrameSnapshot.textures.size());
		const auto appendResourcePreparation = [&resourcePreparationRecords,
			&renderFrameSnapshot](
			const std::vector<TerraEngine::Renderer::RenderFrameResourceRecord>& records)
		{
			for(const TerraEngine::Renderer::RenderFrameResourceRecord& record : records)
			{
				if(record.resourceId == TerraEngine::EMPTY_UUID)
				{
					continue;
				}
				resourcePreparationRecords.push_back({
					record.resourceId,
					std::max<std::uint64_t>(1u, renderFrameSnapshot.resourceRevision),
					0u,
					record.resident,
					record.usingFallback});
			}
		};
		appendResourcePreparation(renderFrameSnapshot.meshes);
		appendResourcePreparation(renderFrameSnapshot.materials);
		appendResourcePreparation(renderFrameSnapshot.textures);
		publication->resourcePreparation =
			TerraEngine::Renderer::BuildRenderResourcePreparationTable(
				resourcePreparationRecords,
				std::max<std::uint64_t>(1u, renderFrameSnapshot.resourceRevision));
}

// The caller owns each captured input through synchronous completion and admitted-child settlement.
TerraEngine::Profiling::ProfilerFramePublicationEvidence RenderSystem::FinalizeFramePublication(
	std::shared_ptr<TerraEngine::Renderer::RenderFramePublication> publication,
	TerraEngine::Renderer::Vulkan::VulkanRendererPreparationSummary& rendererPreparationSummary,
	const std::vector<TerraEngine::Renderer::RenderPassNode>& renderGraphPasses,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities,
	const FrameCaptureRoots& roots, const FramePublicationTimings& timings)
{
	const auto publicationSessionGeneration = publication->rendererSessionGeneration;
	auto& rendererPreparationCache = lifetimeState->rendererPreparationCache;
	const auto& animationPresentationFrame = roots.animationPresentationFrame;
	const auto& backendPreparationStart = timings.backendPreparationStart;
	const auto& backendPreparationEnd = timings.backendPreparationEnd;
	const auto& publicationSealStart = timings.publicationSealStart;
	const auto& persistentPublicationEnd = timings.persistentPublicationEnd;
	const auto& dynamicPublicationEnd = timings.dynamicPublicationEnd;
	const auto& viewPublicationEnd = timings.viewPublicationEnd;
		// Read only the completed immutable roots. Each task writes its own light
		// list, and settlement precedes publication; no new scene flush is needed.
		std::vector<std::future<void>> shadowStreamFutures;
		TerraEngine::Tasks::ScopedTaskSettlement<void> shadowStreamSettlement(shadowStreamFutures);
		try
		{
			for (auto& presentation : publication->presentationInputs)
				for (auto& shadow : presentation.shadowDrawLists)
				{
					const auto seal = [&publication, &presentation, &shadow]() {
						shadow.instanceInputs = TerraEngine::Renderer::BuildShadowPreparedInstanceInputs(
							*publication, presentation, shadow);
					};
					if (presentation.efficiencyOptions.options.validationPreparation
						== TerraEngine::Renderer::RendererEfficiencyPreparation::Concurrent)
						shadowStreamSettlement.CreateAndEnqueue(ThreadPool::instance(), seal);
					else shadowStreamSettlement.RunInline(seal);
				}
			shadowStreamSettlement.GetAll();
		}
		catch (...)
		{
			shadowStreamSettlement.Wait();
			for (auto& presentation : publication->presentationInputs)
				for (auto& shadow : presentation.shadowDrawLists) shadow.instanceInputs.reset();
		}
		const TerraEngine::Renderer::SealedRenderFramePublication sealedPublication =
			TerraEngine::Renderer::SealRenderFramePublication(
				std::move(publication), publicationSessionGeneration);
		const std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>&
			sealedRoot = sealedPublication.Publication();
		const TerraEngine::Renderer::RenderPublicationReceipt publicationReceipt =
			renderFramePublicationStore.TryPublish(sealedPublication,
				publicationSessionGeneration);
		if(sealedRoot != nullptr && publicationReceipt.IsSuccess())
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Prepare Backend Work Caches",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			rendererPreparationSummary = rendererPreparationCache.BuildFramePreparation(
				*sealedRoot,
				renderGraphPasses,
				backendCapabilities,
				TerraEngine::Renderer::RenderDiagnosticsRequested());
		}
		const auto protectedPublicationEnd = std::chrono::steady_clock::now();
		// Profiler evidence is copied only while recording is active. Missing
		// producers stay unavailable; successful and rejected publications both
		// retain their exact generation/outcome tuple for diagnosis.
		TerraEngine::Profiling::ProfilerFramePublicationEvidence evidence{};
		evidence.availabilityMask = 0x7fu;
		evidence.frameGeneration = sealedRoot != nullptr ? sealedRoot->generation : 0u;
		evidence.sceneGeneration = sealedRoot != nullptr ? sealedRoot->sceneGeneration : 0u;
		evidence.renderSessionGeneration = publicationSessionGeneration;
		evidence.topologyGeneration = sealedRoot != nullptr && sealedRoot->persistent != nullptr
			? sealedRoot->persistent->generation : 0u;
		evidence.dynamicGeneration = sealedRoot != nullptr && sealedRoot->dynamic != nullptr
			? sealedRoot->dynamic->generation : 0u;
		evidence.renderOutcome = static_cast<std::uint32_t>(
			publicationReceipt.outcome);
		evidence.rejectionReason = publicationReceipt.IsSuccess()
			? 0u : evidence.renderOutcome;
		evidence.persistentReuseCount = publicationReceipt.reusedPersistentPages;
		evidence.persistentRebuildCount = sealedRoot != nullptr && sealedRoot->persistent != nullptr
			? static_cast<std::uint32_t>(sealedRoot->persistent->pages.size()) - std::min<std::uint32_t>(
				publicationReceipt.reusedPersistentPages,
				static_cast<std::uint32_t>(sealedRoot->persistent->pages.size())) : 0u;
		evidence.dynamicRebuildCount = publicationReceipt.changedDynamicPages;
		evidence.dynamicReuseCount = sealedRoot != nullptr && sealedRoot->dynamic != nullptr
			? static_cast<std::uint32_t>(sealedRoot->dynamic->pages.size()) - std::min<std::uint32_t>(
				publicationReceipt.changedDynamicPages,
				static_cast<std::uint32_t>(sealedRoot->dynamic->pages.size())) : 0u;
		evidence.viewSelectionRebuildCount = sealedRoot != nullptr
			? static_cast<std::uint32_t>(sealedRoot->views.size()) : 0u;
		const auto elapsedNanoseconds = [](const auto begin, const auto end) {
			return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
				std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
		};
		evidence.disjointTimingNanoseconds[0] = elapsedNanoseconds(
			publicationSealStart, persistentPublicationEnd);
		evidence.disjointTimingNanoseconds[1] = elapsedNanoseconds(
			persistentPublicationEnd, dynamicPublicationEnd);
		evidence.disjointTimingNanoseconds[2] = elapsedNanoseconds(
			dynamicPublicationEnd, viewPublicationEnd);
		evidence.disjointTimingNanoseconds[4] = elapsedNanoseconds(
			viewPublicationEnd, protectedPublicationEnd);
		evidence.disjointTimingNanoseconds[5] = elapsedNanoseconds(
			backendPreparationStart, backendPreparationEnd);
		if(animationPresentationFrame != nullptr
			&& animationPresentationFrame->authority != nullptr)
		{
			const auto& authority = *animationPresentationFrame->authority;
			evidence.authorityGeneration = authority.authorityGeneration;
			evidence.presentationGeneration =
				animationPresentationFrame->presentationGeneration;
			evidence.animationOutcome = static_cast<std::uint32_t>(
				authority.receipt.outcome);
			evidence.authorityReuseCount = authority.receipt.reusedPages;
			evidence.authorityRebuildCount = authority.receipt.changedPages;
		}
		evidence.viewCount = sealedRoot != nullptr ? static_cast<std::uint32_t>(std::min<std::size_t>(
			sealedRoot->presentationInputs.size(),
			TerraEngine::Profiling::ProfilerFramePublicationEvidence::kMaximumViews)) : 0u;
		for(std::size_t index = 0u; index < evidence.viewCount; ++index)
		{
			const RenderOptimizationSummary& source =
				sealedRoot->presentationInputs[index].renderOptimization;
			auto& destination = evidence.views[index];
			destination.viewType = static_cast<std::uint32_t>(
				sealedRoot->views[index].viewType);
			destination.frameGeneration = sealedRoot->generation;
			destination.topologyGeneration = source.topologyGeneration;
			destination.viewGeneration = sealedRoot->views[index].generation;
			destination.rendererGeneration = source.rendererGeneration;
			destination.topologyBuildNanoseconds = source.topologyBuildNanoseconds;
			destination.dynamicMaterializationNanoseconds =
				source.dynamicMaterializationNanoseconds;
			destination.instanceStreamUploadNanoseconds =
				source.instanceStreamUploadNanoseconds;
			destination.groupedCommandRecordingNanoseconds =
				source.groupedCommandRecordingNanoseconds;
			destination.visibleInstanceCount = source.visibleInstanceCount;
			destination.groupedInstanceCount = source.groupedInstanceCount;
			destination.fallbackInstanceCount = source.fallbackInstanceCount;
			destination.groupedSubmissionCount = source.groupedSubmissionCount;
			destination.fallbackSubmissionCount = source.fallbackSubmissionCount;
			destination.avoidedSubmissionCount = source.avoidedSubmissionCount;
			destination.instanceStreamBytes = source.instanceStreamBytes;
			destination.instanceStreamCapacityBytes = source.instanceStreamCapacityBytes;
			destination.topologyReuseHits = source.topologyReuseHits;
			destination.topologyInvalidations = source.topologyInvalidations;
			destination.viewEvidenceReuseHits = source.viewEvidenceReuseHits;
			destination.viewEvidenceInvalidations = source.viewEvidenceInvalidations;
			destination.rejectedIncompleteKeyCount =
				source.rejectedIncompleteKeyCount;
			destination.rejectedUnsupportedVariantCount =
				source.rejectedUnsupportedVariantCount;
			destination.rejectedOutOfRangeCount = source.rejectedOutOfRangeCount;
			destination.rejectedStaleGenerationCount =
				source.rejectedStaleGenerationCount;
			destination.fallbackAllocationFailureCount =
				source.fallbackAllocationFailureCount;
			destination.fallbackRecoveryCount = source.fallbackRecoveryCount;
			destination.valid = source.valid != 0u;
		}

		if(!publicationReceipt.IsSuccess())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Render frame publication was rejected for frame " << activeRenderFrameId
					<< ": " << publicationReceipt.DiagnosticText());
		}
	return evidence;
}

// Preserve original ordered view evidence and exact immutable publication identity.
void RenderSystem::OrderFramePreparedViews(TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	std::vector<PreparedRenderSubmission>& preparedRenderSubmissions)
{	if (renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied && !renderFrameSnapshot.efficiencyFixtureIntent.viewOrder.empty())
	{
		std::vector<std::size_t> actualSlots;
		std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry> actual;
		for (std::size_t index = 0; index < preparedRenderSubmissions.size(); ++index)
		{
			const auto& output = preparedRenderSubmissions[index].output;
			if (output.kind == RenderOutputKind::SceneView || output.kind == RenderOutputKind::GameView || output.kind == RenderOutputKind::MainRuntimeView)
			{ actualSlots.push_back(index); actual.push_back({output.diagnosticsView, output.outputId}); }
		}
		std::vector<std::size_t> order; std::string reason;
		if (TerraEngine::Renderer::ResolveRendererEfficiencyFixtureViewOrder(renderFrameSnapshot.efficiencyFixtureIntent, actual, order, reason))
		{
			// Move the actual presentation owners before sealing ranges and snapshot
			// slots. RHI iteration and vkQueueSubmit now consume this exact order.
			std::vector<PreparedRenderSubmission> reordered; reordered.reserve(preparedRenderSubmissions.size());
			for (const auto index : order) reordered.push_back(std::move(preparedRenderSubmissions[actualSlots[index]]));
			for (std::size_t index = 0; index < preparedRenderSubmissions.size(); ++index)
				if (std::find(actualSlots.begin(), actualSlots.end(), index) == actualSlots.end()) reordered.push_back(std::move(preparedRenderSubmissions[index]));
			preparedRenderSubmissions = std::move(reordered);
		}
		else { renderFrameSnapshot.efficiencyFixtureIntent.controlsApplied = false; renderFrameSnapshot.efficiencyFixtureIntent.controlReason = reason; }
	}
}

// Preserve original ordered view evidence and exact immutable publication identity.
void RenderSystem::PublishFrameAnimatedVisibility(const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	std::span<const RenderableEntry> renderables, const std::vector<PreparedRenderSubmission>& preparedRenderSubmissions,
	std::vector<std::uint8_t>& visibleAnimatedRenderableFlags, std::uint64_t protectedSceneGeneration)
{	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Publish Animated Visibility",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Renderer::AnimatedObjectVisibilitySnapshot visibility;
		visibility.frameId = activeRenderFrameId;
		visibility.sceneGeneration = protectedSceneGeneration;
		visibility.hasActiveView = !preparedRenderSubmissions.empty();
		visibility.valid = true;
		// Camera commands marked the canonical mask while their current animation
		// bindings were attached. Scan only shadow lists here so shadow-only casters
		// cannot freeze while avoiding a second scan of every camera command.
		for(const PreparedRenderSubmission& prepared : preparedRenderSubmissions)
		{
			for(const ShadowDrawList& shadow : prepared.presentation.shadowDrawLists)
			{
				for(const std::uint32_t ordinal : shadow.stableOrdinals)
				{
					if(ordinal >= renderFrameSnapshot.renderables.size()
						|| renderFrameSnapshot.renderables[ordinal].animationBinding.status
						!= TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
					{
						continue;
					}
					if(ordinal < visibleAnimatedRenderableFlags.size())
					{
						visibleAnimatedRenderableFlags[ordinal] = 1u;
					}
				}
			}
		}
		visibility.visibleObjectIds.reserve(renderables.size());
		for(std::size_t index = 0u; index < renderables.size(); ++index)
		{
			if(visibleAnimatedRenderableFlags[index] != 0u)
			{
				visibility.visibleObjectIds.push_back(renderables[index].objectId);
			}
		}
		visibility.canonicalUnique = true;
		TerraEngine::Renderer::PublishAnimatedObjectVisibilitySnapshot(
			std::move(visibility));
	}
}

// Preserve original ordered view evidence and exact immutable publication identity.
RenderSystem::FrameViewObservations RenderSystem::ObserveFrameViewOutputs(
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const std::vector<PreparedRenderSubmission>& preparedRenderSubmissions, std::uint64_t snapshotRendererGeneration)
{
	FrameViewObservations result;
	auto& completedSceneVisibilityRecords = result.completedSceneVisibilityRecords;
	auto& completedGameVisibilityRecords = result.completedGameVisibilityRecords;
	auto& completedSceneCaptureVisibilityRecords = result.completedSceneCaptureVisibilityRecords;
	auto& completedGameCaptureVisibilityRecords = result.completedGameCaptureVisibilityRecords;
	auto& completedSceneCaptureIdentitySnapshot = result.completedSceneCaptureIdentitySnapshot;
	auto& completedGameCaptureIdentitySnapshot = result.completedGameCaptureIdentitySnapshot;
	auto& sceneCaptureProcessed = result.sceneCaptureProcessed;
	auto& gameCaptureProcessed = result.gameCaptureProcessed;
	for(const PreparedRenderSubmission& prepared : preparedRenderSubmissions)
	{
		if(prepared.output.kind == RenderOutputKind::SceneView)
		{
			completedSceneVisibilityRecords.assign(
				prepared.presentation.VirtualGeometryPackets().begin(),
				prepared.presentation.VirtualGeometryPackets().end());
		}
		else if(prepared.output.kind == RenderOutputKind::GameView
			|| prepared.output.kind == RenderOutputKind::MainRuntimeView)
		{
			completedGameVisibilityRecords.assign(
				prepared.presentation.VirtualGeometryPackets().begin(),
				prepared.presentation.VirtualGeometryPackets().end());
		}
		else if(prepared.output.kind == RenderOutputKind::OffscreenTexture)
		{
			if(prepared.output.diagnosticsView == RenderViewType::Scene) {
				completedSceneCaptureVisibilityRecords.assign(
					prepared.presentation.VirtualGeometryPackets().begin(),
					prepared.presentation.VirtualGeometryPackets().end());
				completedSceneCaptureIdentitySnapshot.frameId =
					prepared.selection.generation;
				completedSceneCaptureIdentitySnapshot.rendererGeneration = snapshotRendererGeneration;
				completedSceneCaptureIdentitySnapshot.outputId = prepared.output.outputId;
				completedSceneCaptureIdentitySnapshot.originGeneration =
					prepared.presentation.referenceFrame.originGeneration;
				completedSceneCaptureIdentitySnapshot.spatialGeneration =
					prepared.presentation.referenceFrame.spatialGeneration;
				completedSceneCaptureIdentitySnapshot.residencyGeneration =
					prepared.presentation.referenceFrame.residencyGeneration;
				completedSceneCaptureIdentitySnapshot.table =
					renderFrameSnapshot.conventionalVisibilityIdentityTable;
				sceneCaptureProcessed = true;
			}
			else
			{
				completedGameCaptureVisibilityRecords.assign(
					prepared.presentation.VirtualGeometryPackets().begin(),
					prepared.presentation.VirtualGeometryPackets().end());
				completedGameCaptureIdentitySnapshot.frameId =
					prepared.selection.generation;
				completedGameCaptureIdentitySnapshot.rendererGeneration = snapshotRendererGeneration;
				completedGameCaptureIdentitySnapshot.outputId = prepared.output.outputId;
				completedGameCaptureIdentitySnapshot.originGeneration =
					prepared.presentation.referenceFrame.originGeneration;
				completedGameCaptureIdentitySnapshot.spatialGeneration =
					prepared.presentation.referenceFrame.spatialGeneration;
				completedGameCaptureIdentitySnapshot.residencyGeneration =
					prepared.presentation.referenceFrame.residencyGeneration;
				completedGameCaptureIdentitySnapshot.table =
					renderFrameSnapshot.conventionalVisibilityIdentityTable;
				gameCaptureProcessed = true;
			}
		}
	}

	return result;
}

// Seal the all-view backend envelope only after every per-view coverage authority exists.
void RenderSystem::PrepareFrameVirtualGeometryEnvelope(
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const std::vector<PreparedRenderSubmission>& preparedRenderSubmissions,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities,
	std::uint64_t snapshotRendererGeneration,
	const std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback>& completedVirtualGeometryCounters,
	TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{	// Seal one immutable virtual-geometry envelope only after every view has
	// produced its own visibility packet list. The oracle normalizes this union
	// into bounded IDs; it does not replace the per-view CPU cut that remains the
	// coverage authority until Vulkan reports completed device-stage evidence.
	const TerraEngine::Renderer::VirtualGeometryBackendSelection backendSelection =
		TerraEngine::Renderer::SelectVirtualGeometryBackend(backendCapabilities);
	const bool hasVirtualGeometryRoute = std::any_of(
		renderFrameSnapshot.renderables.begin(),
		renderFrameSnapshot.renderables.end(),
		[](const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable) {
			return renderable.geometryRouteDecision.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryHardware
				|| renderable.geometryRouteDecision.selected
					== TerraEngine::Renderer::GeometryRoute::VirtualGeometryCompute;
		});
	if(backendSelection.deviceAuthoredWorkAvailable && hasVirtualGeometryRoute)
	{
		std::vector<TerraEngine::Renderer::VirtualGeometryOracleNode> oracleNodes;
		for(const PreparedRenderSubmission &prepared : preparedRenderSubmissions)
		{
			for(const GpuVirtualGeometryPacketRecord& packet
				: prepared.presentation.VirtualGeometryPackets())
			{
				TerraEngine::Renderer::VirtualGeometryOracleNode node{};
				// Packet identity is local to a view, so the sealed all-view envelope
				// uses an ordinal value. Its deterministic order follows the immutable
				// prepared-submission sequence, while packet metadata remains view-owned.
				node.id = static_cast<std::uint64_t>(oracleNodes.size()) + 1u;
				node.resident = packet.meshletCount != 0u
					&& (packet.flags & kVirtualGeometryFlagFailOpen) == 0u;
				node.error = 1.0f;
				oracleNodes.push_back(node);
			}
		}
		if(!oracleNodes.empty())
		{
			const std::uint32_t boundedCapacity = std::min<std::uint32_t>(
				backendSelection.commandCapacity,
				static_cast<std::uint32_t>(std::min<std::size_t>(
					oracleNodes.size(), std::numeric_limits<std::uint32_t>::max())));
			const TerraEngine::Renderer::VirtualGeometrySelectionResult oracleSelection =
				TerraEngine::Renderer::SelectVirtualGeometryOracle(
					oracleNodes, 0.0f, boundedCapacity);
			TerraEngine::Renderer::VirtualGeometryGpuPlanOptions planOptions{};
			planOptions.packetCapacity = boundedCapacity;
			planOptions.frameId = activeRenderFrameId;
			planOptions.generation = snapshotRendererGeneration;
			planOptions.generatedWorkKind = backendSelection.generatedWorkKind;
			planOptions.usesIndirectCount =
				backendCapabilities.virtualGeometryGeneratedWork.supportsIndirectCount;
			auto immutableVirtualGeometryPlan = std::make_shared<const TerraEngine::Renderer::VirtualGeometryGpuPlan>(
				TerraEngine::Renderer::BuildVirtualGeometryGpuPlan(
					oracleSelection, planOptions));
			if(immutableVirtualGeometryPlan->IsComplete())
			{
				// Existing Vulkan implementations may return no counter rows. In that
				// case the replay remains pending rather than inventing an authoritative
				// value from prepared CPU packet counts.
				latestVirtualGeometryReplayArtifacts =
					virtualGeometryReplayReadbacks.PublishAuthoritative(
						completedVirtualGeometryCounters);
				virtualGeometryReplayReadbacks.EnqueuePending(*immutableVirtualGeometryPlan);
			}
			else
			{
				renderGraphDiagnostics.backendFallbacks.push_back(
					"Virtual-geometry immutable device-work envelope was invalid; CPU oracle coverage remains authoritative.");
			}
		}
	}
	else if(!hasVirtualGeometryRoute)
	{
		// A direct-only frame intentionally has no VG backend envelope. Normal
		// resource uploads and conventional mesh-task draws continue separately.
		renderGraphDiagnostics.degradedWork.push_back(
			"Virtual-geometry feedback, decode, residency, compaction, and executor stages were suppressed for direct meshlet routes.");
	}
	else if(!backendSelection.fallbackReason.empty())
	{
		if(TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(backendSelection.fallbackReason);
		}
	}
}
