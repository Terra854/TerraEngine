#include "TestHarness.h"

#include "Renderer/Graph/RenderFramePublication.h"
#include "RendererThreadingTestFixtures.h"

#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <type_traits>

namespace TerraEngine::Tests
{
	namespace
	{
		template <typename Binding>
		constexpr bool HasWorkerAnimationEnvelope()
		{
			// Check the actual omitted worker fields instead of comparing padded
			// structure sizes: semantic scalar provenance may legitimately grow.
			return requires(Binding value) { value.meshId; }
				|| requires(Binding value) { value.animatorObjectId; }
				|| requires(Binding value) { value.skeletonAssetId; }
				|| requires(Binding value) { value.animatorSlotIndex; }
				|| requires(Binding value) { value.presentationFrame; }
				|| requires(Binding value) { value.authorityRow; }
				|| requires(Binding value) { value.gpu; };
		}

		std::shared_ptr<const TerraEngine::Renderer::RenderFramePublication>
			BuildPublication(std::uint64_t session, std::uint64_t previous)
		{
			using namespace TerraEngine::Renderer;
			auto persistentPage = std::make_shared<RenderPersistentPage>();
			persistentPage->generation = 1u;
			RenderPersistentSlot slot{};
			slot.key = { 0u, 1u };
			slot.objectId = GenerateUUID();
			slot.meshId = GenerateUUID();
			slot.materialId = GenerateUUID();
			slot.meshGeneration = 1u;
			slot.materialGeneration = 1u;
			slot.resourceGeneration = 1u;
			slot.packetTemplateIdentity = 1u;
			slot.occupied = true;
			persistentPage->slots.push_back(slot);
			auto dynamicPage = std::make_shared<RenderDynamicPage>();
			dynamicPage->generation = 1u;
			dynamicPage->changedLanes.push_back(true);
			RenderDynamicLane lane{};
			lane.key = slot.key;
			// Poison the float matrix so the resolver test proves the canonical
			// large-world pose is localized against the view reference frame.
			lane.instance.modelMatrix[3][0] = 900.0f;
			lane.canonicalWorldPose.position.offsetMetres.x = 107.0;
			lane.hasCanonicalWorldPose = true;
			lane.instance.objectIdentity.x = 1u;
			lane.occupied = true;
			dynamicPage->lanes.push_back(lane);
			auto publication = std::make_shared<RenderFramePublication>();
			publication->frameId = 1u;
			publication->rendererSessionGeneration = session;
			publication->sceneGeneration = 1u;
			publication->expectedPreviousGeneration = previous;
			publication->generation = previous + 1u;
			publication->persistent = std::make_shared<RenderPersistentTopologyRoot>(
				RenderPersistentTopologyRoot{ 1u, session, { persistentPage } });
			publication->dynamic = std::make_shared<RenderDynamicStateRoot>(
				RenderDynamicStateRoot{ 1u, session, { dynamicPage } });
			auto backendWork = std::make_shared<RenderBackendWorkPublication>();
			backendWork->frameId = publication->frameId;
			backendWork->animationWork.frameId = publication->frameId;
			backendWork->animationWork.valid = true;
			backendWork->valid = true;
			publication->backendWork = std::move(backendWork);
			RenderViewSelection view{};
			view.outputId = 1u;
			view.generation = 1u;
			view.visibleOrdinals.push_back(slot.key);
			view.selectedLods.push_back(0u);
			view.routeSelectors.push_back(0u);
			view.packetTemplateIdentities.push_back(slot.packetTemplateIdentity);
			view.tintSelectors.push_back(0u);
			view.editorTints.push_back(glm::vec4(0.0f));
			publication->views.push_back(view);
			RenderViewPresentationInput presentation{};
			presentation.output.kind = view.outputKind;
			presentation.output.diagnosticsView = view.viewType;
			presentation.output.outputId = view.outputId;
			RenderReferenceFrame referenceFrame =
				MakeRendererTestReferenceFrame(publication->frameId, 1u, view.outputId);
			referenceFrame.canonicalOrigin.offsetMetres.x = 100.0;
			presentation.referenceFrame = referenceFrame;
			publication->referenceFrames.push_back(referenceFrame);
			RenderViewCommandLane commandLane{};
			commandLane.materialIndex = 3u;
			commandLane.virtualMeshletOffset = 5u;
			commandLane.virtualMeshletCount = 11u;
			presentation.commandLanes.push_back(std::move(commandLane));
			presentation.commandSelectionIndices.push_back(0u);
			publication->presentationInputs.push_back(std::move(presentation));
			publication->submissionRanges.push_back(
				{ RenderSubmissionRoute::Direct, 0u, 0u, 1u,
					slot.packetTemplateIdentity, true });
			return publication;
		}
	}

	void RegisterRenderFramePublicationContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.FramePublication.PublishesCompactAnimationBinding",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderItemRecord::PublishedAnimationGeneration generation{};
			generation.stableInstanceId = 11u;
			generation.poseGeneration = 13u;
			generation.morphGeneration = 47u;
			generation.deformationContentIdLow = 53u;
			generation.deformationContentIdHigh = 59u;
			generation.paletteGeneration = 17u;
			generation.deformablePayloadGeneration = 19u;
			generation.programGeneration = 23u;
			generation.animationPlanGeneration = 29u;
			generation.rendererGeneration = 31u;
			generation.firstGpuWorkPacket = 37u;
			generation.gpuWorkPacketCount = 41u;
			generation.gpuStageMask = 43u;
			generation.source = static_cast<std::uint8_t>(
				AnimationRenderEvaluationSource::GpuVisual);
			generation.complete = true;
			context.Expect(std::is_trivially_copyable_v<RenderItemRecord::PublishedAnimationGeneration>
				&& std::is_standard_layout_v<RenderItemRecord::PublishedAnimationGeneration>
				&& !HasWorkerAnimationEnvelope<RenderItemRecord::PublishedAnimationGeneration>(),
				"Published animation provenance must remain scalar-only without worker runtime identities or owners.");
			// Copy through the real dynamic lane so semantic provenance is explicitly
			// preserved rather than accidentally omitted to meet a size threshold.
			RenderDynamicLane semanticLane{};
			semanticLane.animation = generation;
			context.Expect(semanticLane.animation.morphGeneration == 47u
				&& semanticLane.animation.deformationContentIdLow == 53u
				&& semanticLane.animation.deformationContentIdHigh == 59u,
				"The sealed lane must retain morph generation and both exact deformation content identity halves.");
			context.Expect(generation.stableInstanceId == 11u
				&& generation.poseGeneration == 13u
				&& generation.paletteGeneration == 17u
				&& generation.deformablePayloadGeneration == 19u
				&& generation.programGeneration == 23u
				&& generation.animationPlanGeneration == 29u
				&& generation.rendererGeneration == 31u
				&& generation.firstGpuWorkPacket == 37u
				&& generation.gpuWorkPacketCount == 41u
				&& generation.gpuStageMask == 43u
				&& generation.source == static_cast<std::uint8_t>(
					AnimationRenderEvaluationSource::GpuVisual)
				&& generation.complete,
				"The compact generation must preserve every value required by backend animation work.");
			AnimationRenderBinding resolved{};
			resolved.animationAuthorityGeneration = 17u;
			resolved.posePublicationGeneration = 19u;
			resolved.deformablePayloadGeneration = 23u;
			resolved.skeletonCompatibilityFingerprint = 29u;
			resolved.source = AnimationRenderPoseSource::GpuVisual;
			resolved.status = AnimationRenderBindingStatus::Ready;
			resolved.safePose = false;
			resolved.pageLocalDeformationAvailable = true;
			const PublishedAnimationRenderBinding published(resolved);
			context.Expect(sizeof(PublishedAnimationRenderBinding)
				< sizeof(AnimationRenderBinding)
				&& HasWorkerAnimationEnvelope<AnimationRenderBinding>()
				&& !HasWorkerAnimationEnvelope<PublishedAnimationRenderBinding>(),
				"Protected publication must not duplicate the worker-facing animation identity envelope for every object.");
			context.Expect(published.animationAuthorityGeneration == 17u
				&& published.posePublicationGeneration == 19u
				&& published.deformablePayloadGeneration == 23u
				&& published.skeletonCompatibilityFingerprint == 29u
				&& published.source == AnimationRenderPoseSource::GpuVisual
				&& published.status == AnimationRenderBindingStatus::Ready
				&& !published.safePose
				&& published.pageLocalDeformationAvailable,
				"The compact binding must preserve every fallback and compatibility value consumed after publication.");
		});

		registry.Add("Renderer.FramePublication.PublishesImmutableGenerationRoot", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFramePublicationStore store;
			const auto candidate = BuildPublication(store.SessionGeneration(), 0u);
			const SealedRenderFramePublication sealed = SealRenderFramePublication(
				candidate, store.SessionGeneration());
			const RenderPublicationReceipt receipt = store.TryPublish(
				sealed, store.SessionGeneration());
			context.Expect(receipt.IsSuccess(), "A complete candidate must publish atomically.");
			context.Expect(store.Load() == candidate,
				"Readers must acquire the exact sealed root selected at the protected boundary.");
		});

		registry.Add("Renderer.FramePublication.StageContractRetainsExactRootsAcrossExchangeAndSessionReset", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFramePublicationStore store;
			auto first = std::make_shared<RenderFramePublication>(*BuildPublication(store.SessionGeneration(), 0u));
			auto payload = std::make_shared<RenderViewSelectionPayload>();
			auto& selection = first->views.front();
			payload->visibleOrdinals.swap(selection.visibleOrdinals);
			payload->selectedLods.swap(selection.selectedLods);
			payload->routeSelectors.swap(selection.routeSelectors);
			payload->packetTemplateIdentities.swap(selection.packetTemplateIdentities);
			payload->tintSelectors.swap(selection.tintSelectors);
			payload->editorTints.swap(selection.editorTints);
			selection.retainedPayload = payload;
			const auto expectedObject = first->persistent->pages.front()->slots.front().objectId;
			std::weak_ptr<const RenderPersistentTopologyRoot> persistent = first->persistent;
			std::weak_ptr<const RenderDynamicStateRoot> dynamic = first->dynamic;
			std::weak_ptr<const RenderBackendWorkPublication> backend = first->backendWork;
			std::weak_ptr<const RenderViewSelectionPayload> view = payload;
			context.Expect(store.TryPublish(SealRenderFramePublication(first, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(),
				"The production publication store must accept the first complete owned root.");
			auto heldReader = store.Load();
			const auto second = BuildPublication(store.SessionGeneration(), first->generation);
			context.Expect(store.TryPublish(SealRenderFramePublication(second, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(),
				"A later complete generation must exchange the live root.");
			first.reset(); payload.reset();
			store.ResetForSession(store.SessionGeneration() + 1u);
			context.Expect(!store.Load() && !persistent.expired() && !dynamic.expired() && !backend.expired() && !view.expired(),
				"Exchange and session retirement must not destroy exact roots retained by an earlier immutable reader.");
			{
				// The real resolver borrows its reader; finish the last typed command
				// consumption before releasing that reader's strong page/root owners.
				const auto resolver = MakeRenderViewCommandResolver(*heldReader, 0u);
				RenderItemRecord command;
				context.Expect(resolver.Count() == 1u && resolver.Resolve(0u, command) && command.objectId == expectedObject
					&& command.modelMatrix[3][0] == 7.0f && command.materialIndex == 3u,
					"The held generation must still resolve its original identity, transform and lane after a new session starts.");
			}
			heldReader.reset();
			context.Expect(persistent.expired() && dynamic.expired() && backend.expired() && view.expired(),
				"Exact old-generation roots must retire once their final real reader finishes, without a hidden global retention owner.");
		});

		registry.Add("Renderer.FramePublication.StageContractPreservesTypedOutputOrderAtSealAndResolve", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>(*BuildPublication(1u, 0u));
			const auto sourceView = publication->views.front();
			const auto sourcePresentation = publication->presentationInputs.front();
			publication->views.clear(); publication->presentationInputs.clear(); publication->referenceFrames.clear(); publication->submissionRanges.clear();
			// Deliberately non-sorted IDs make an accidental stage-local sort visible.
			const std::array<std::uint32_t,3> ids{31u,7u,19u};
			const std::array<RenderOutputKind,3> kinds{RenderOutputKind::SceneView,RenderOutputKind::GameView,RenderOutputKind::MainRuntimeView};
			for (std::size_t index = 0; index < ids.size(); ++index) {
				auto view = sourceView; view.outputId = ids[index]; view.outputKind = kinds[index]; view.viewType = index == 0 ? RenderViewType::Scene : RenderViewType::Game; view.selectedLods.front() = static_cast<std::uint32_t>(index);
				auto presentation = sourcePresentation;
				presentation.output.outputId = view.outputId; presentation.output.kind = view.outputKind; presentation.output.diagnosticsView = view.viewType;
				presentation.referenceFrame = MakeRendererTestReferenceFrame(publication->frameId, index + 1u, ids[index]);
				presentation.referenceFrame.canonicalOrigin.offsetMetres.x = 100.0;
				presentation.commandLanes.front().materialIndex = static_cast<std::uint32_t>(40u + index);
				publication->referenceFrames.push_back(presentation.referenceFrame);
				publication->views.push_back(std::move(view));
				publication->presentationInputs.push_back(std::move(presentation));
				const auto ranges = BuildRenderSubmissionRanges(publication->views.back(), static_cast<std::uint32_t>(index));
				publication->submissionRanges.insert(publication->submissionRanges.end(), ranges.begin(), ranges.end());
			}
			const auto sealed = SealRenderFramePublication(publication, 1u);
			context.Expect(sealed.IsValid(), "Multi-output selectors and presentation inputs must pass the actual production seal.");
			const auto& actual = *sealed.Publication();
			context.Expect(actual.views.size() == ids.size() && actual.submissionRanges.size() == ids.size(), "Sealing must preserve every typed output and its submission range.");
			for (std::size_t index = 0; index < ids.size(); ++index) {
				const auto resolver = MakeRenderViewCommandResolver(actual, index); RenderItemRecord command;
				context.Expect(actual.views[index].outputId == ids[index] && actual.views[index].outputKind == kinds[index]
					&& actual.presentationInputs[index].output.outputId == ids[index] && actual.submissionRanges[index].viewIndex == index
					&& resolver.Resolve(0u, command) && command.materialIndex == 40u + index
					&& actual.views[index].SelectedLods().front() == index,
					"Protected sealing and real command resolution must retain authored output order and each output's exact parallel selectors.");
			}
		});

		registry.Add("Renderer.FramePublication.StageContractRejectsStaleAndFailedCandidatesAtomically", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFramePublicationStore store;
			const auto first = BuildPublication(store.SessionGeneration(), 0u);
			context.Expect(store.TryPublish(SealRenderFramePublication(first, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(), "Publish a valid predecessor before worker candidate preparation.");
			const auto stalePrepared = SealRenderFramePublication(BuildPublication(store.SessionGeneration(), first->generation), store.SessionGeneration());
			const auto winner = BuildPublication(store.SessionGeneration(), first->generation);
			context.Expect(store.TryPublish(SealRenderFramePublication(winner, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(), "A different prepared candidate must be able to win the protected exchange first.");
			context.Expect(store.TryPublish(stalePrepared, store.SessionGeneration()).outcome == RenderPublicationOutcome::RejectedStaleGeneration
				&& store.Load() == winner, "A previously sealed worker candidate cannot overwrite its changed predecessor or partially exchange root owners.");
			const std::array<RenderPublicationFailureInjection,3> faults{{{true,false,false},{false,true,false},{false,false,true}}};
			const std::array<RenderPublicationOutcome,3> expected{RenderPublicationOutcome::RejectedAllocation,RenderPublicationOutcome::RejectedCancellation,RenderPublicationOutcome::RejectedBackendPreparation};
			for (std::size_t index = 0; index < faults.size(); ++index) {
				const auto candidate = BuildPublication(store.SessionGeneration(), winner->generation);
				const auto rejected = store.TryPublish(SealRenderFramePublication(candidate, store.SessionGeneration(), faults[index]), store.SessionGeneration());
				context.Expect(rejected.outcome == expected[index] && store.Load() == winner
					&& store.Load()->persistent == winner->persistent && store.Load()->dynamic == winner->dynamic && store.Load()->backendWork == winner->backendWork,
					"Allocation, cancellation and backend rejection must preserve the complete prior root and its exact constituent owners.");
			}
			const auto oldSessionCandidate = SealRenderFramePublication(BuildPublication(store.SessionGeneration(), winner->generation), store.SessionGeneration());
			const auto oldSession = store.SessionGeneration(); store.ResetForSession(oldSession + 1u);
			context.Expect(!store.TryPublish(oldSessionCandidate, oldSession).IsSuccess() && !store.Load(), "A prepared old-session candidate cannot resurrect roots after a protected lifecycle reset.");
		});

		registry.Add("Renderer.FramePublication.StaleProducerProofFallsBackToFullValidation",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>(
				*BuildPublication(1u, 0u));
			auto persistent = std::make_shared<RenderPersistentTopologyRoot>(
				*publication->persistent);
			auto persistentPage = std::make_shared<RenderPersistentPage>(
				*persistent->pages.front());
			persistentPage->producerValidatedSlotCount =
				persistentPage->slots.size();
			persistent->pages.front() = persistentPage;
			publication->persistent = persistent;

			auto dynamic = std::make_shared<RenderDynamicStateRoot>(
				*publication->dynamic);
			auto dynamicPage = std::make_shared<RenderDynamicPage>(
				*dynamic->pages.front());
			dynamicPage->producerValidatedLaneCount = dynamicPage->lanes.size();
			// A proof for another topology generation cannot suppress exact-key checks.
			dynamicPage->producerValidatedPersistentGeneration =
				persistentPage->generation + 1u;
			dynamicPage->lanes.front().key.lifetimeGeneration += 1u;
			dynamic->pages.front() = dynamicPage;
			publication->dynamic = dynamic;
			context.Expect(!ValidateRenderFramePublication(*publication, 1u).IsSuccess(),
				"A stale producer proof must fall back to full lane validation and reject a recycled lifetime key.");
		});

		registry.Add("Renderer.FramePublication.RejectsSelectorAndBackendFailuresWithoutExchange", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFramePublicationStore store;
			const auto first = BuildPublication(store.SessionGeneration(), 0u);
			context.Expect(store.TryPublish(SealRenderFramePublication(
				first, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(),
				"The baseline root must publish before rejection rollback is tested.");
			auto invalid = std::make_shared<RenderFramePublication>(*BuildPublication(
				store.SessionGeneration(), first->generation));
			invalid->views.front().selectedLods.clear();
			context.Expect(!store.TryPublish(SealRenderFramePublication(
				invalid, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(),
				"Mismatched parallel selectors must reject the entire candidate.");
			context.Expect(store.Load() == first,
				"A rejected selector candidate must retain the previous immutable root.");
			auto missingBackend = std::make_shared<RenderFramePublication>(
				*BuildPublication(store.SessionGeneration(), first->generation));
			missingBackend->backendWork.reset();
			context.Expect(!store.TryPublish(SealRenderFramePublication(
				missingBackend, store.SessionGeneration()),
				store.SessionGeneration()).IsSuccess(),
				"A frame without sealed backend work must reject before root exchange.");
			context.Expect(store.Load() == first,
				"Missing backend work must leave the prior root visible.");
			const auto second = BuildPublication(store.SessionGeneration(), first->generation);
			context.Expect(!store.TryPublish(SealRenderFramePublication(second,
				store.SessionGeneration(), { false, false, true }),
				store.SessionGeneration()).IsSuccess(),
				"Backend preparation failure must reject before visible publication.");
			context.Expect(store.Load() == first,
				"Backend preparation rejection must not submit a partial root.");
		});

		registry.Add("Renderer.FramePublication.ResolverReconstructsLocalCommand", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const auto publication = BuildPublication(1u, 0u);
			const RenderViewCommandResolver resolver =
				MakeRenderViewCommandResolver(*publication, 0u);
			RenderItemRecord command{};
			const RenderPersistentSlot& persistent =
				publication->persistent->pages.front()->slots.front();
			context.Expect(resolver.Count() == 1u && resolver.Resolve(0u, command)
				&& command.meshId == persistent.meshId
				&& command.materialId == persistent.materialId
				&& command.objectId == persistent.objectId
				&& command.modelMatrix[3][0] == 7.0f
				&& command.materialIndex == 3u
				&& command.virtualMeshletOffset == 5u
				&& command.virtualMeshletCount == 11u,
				"The no-allocation resolver must compose persistent identity, dynamic transform, and view lane selectors into one local command value.");
		});

		registry.Add("Renderer.FramePublication.ShadowResolverUsesCompactOrdinalSelection",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>(
				*BuildPublication(1u, 0u));
			// Topology rebuilds and slot recycling have independent clocks. Keep
			// lifetime 1 on the real slot/lane while advancing only the root to 23.
			auto topology = std::make_shared<RenderPersistentTopologyRoot>(*publication->persistent);
			topology->generation = 23u;
			publication->persistent = topology;
			ShadowDrawList shadow{};
			shadow.lightId = GenerateUUID();
			shadow.topologyGeneration = publication->persistent->generation;
			shadow.stableOrdinals.push_back(0u);
			shadow.selectedLods.push_back(2u);
			shadow.routeSelectors.push_back(static_cast<std::uint32_t>(
				GeometryRoute::VirtualGeometryCompute));
			shadow.editorTints.push_back(glm::vec4(0.25f));
			RenderViewCommandLane shadowLane{};
			shadowLane.virtualMeshletOffset = 17u;
			shadowLane.virtualMeshletCount = 4u;
			shadow.commandLanes.push_back(shadowLane);
			publication->presentationInputs.front().shadowDrawLists.push_back(shadow);
			RenderItemRecord resolved{};
			const RenderPublicationReceipt validation = ValidateRenderFramePublication(
				*publication, 1u);
			context.Expect(validation.IsSuccess()
				&& ResolveShadowDrawListItem(*publication,
					publication->presentationInputs.front().referenceFrame,
					shadow, 0u, resolved)
				&& resolved.selectedVirtualLod == 2u
				&& resolved.geometryRoute == GeometryRoute::VirtualGeometryCompute
				&& resolved.virtualMeshletOffset == 17u
				&& resolved.virtualMeshletCount == 4u,
				"A shadow pass must resolve one local item from sealed roots and compact light-view selectors without retaining a full command vector.");
			shadow.topologyGeneration = 1u;
			context.Expect(!ResolveShadowDrawListItem(*publication,
				publication->presentationInputs.front().referenceFrame, shadow, 0u, resolved),
				"A stale topology must reject even when its generation equals the current slot lifetime.");

			publication->presentationInputs.front().shadowDrawLists.front()
				.topologyGeneration += 1u;
			context.Expect(!ValidateRenderFramePublication(*publication, 1u).IsSuccess(),
				"A shadow selection from a stale topology generation must reject the complete publication.");
		});

		registry.Add("Renderer.FramePublication.ResourcePreparationIsCompactAndDeterministic",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			std::vector<RenderResourcePreparationRecord> input(2u);
			input[0].resourceId = GenerateUUID();
			input[0].resourceGeneration = 4u;
			input[1].resourceId = GenerateUUID();
			input[1].resourceGeneration = 7u;
			const RenderResourcePreparationTable table =
				BuildRenderResourcePreparationTable(input, 9u);
			context.Expect(table.generation == 9u && table.records.size() == 2u
				&& table.records[0].preparationIndex == 0u
				&& table.records[1].preparationIndex == 1u
				&& table.Find(table.records[0].resourceId) != nullptr,
				"Resource preparation must be sorted, densely indexed, and directly searchable.");
		});

		registry.Add("Renderer.FramePublication.CoalescesCompatibleSubmissionRanges",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderViewSelection selection{};
			selection.generation = 1u;
			for(std::uint32_t ordinal = 0u; ordinal < 6u; ++ordinal)
			{
				selection.visibleOrdinals.push_back({ ordinal, 3u });
				selection.selectedLods.push_back(0u);
				selection.routeSelectors.push_back(ordinal >= 4u
					? static_cast<std::uint32_t>(GeometryRoute::VirtualGeometryCompute)
					: 0u);
				selection.packetTemplateIdentities.push_back(
					ordinal < 3u ? 11u : ordinal < 4u ? 12u : 13u);
				selection.tintSelectors.push_back(0u);
				selection.editorTints.push_back(glm::vec4(0.0f));
			}
			const std::vector<RenderSubmissionRange> ranges =
				BuildRenderSubmissionRanges(selection, 2u);
			const std::uint32_t selectedCount = std::accumulate(ranges.begin(),
				ranges.end(), 0u, [](std::uint32_t total,
					const RenderSubmissionRange& range)
				{
					return total + range.selectionCount;
				});
			context.Expect(ranges.size() == 3u && selectedCount == 6u,
				"Compatible adjacent selections must collapse without losing work.");
			context.Expect(ranges[0].viewIndex == 2u
				&& ranges[0].firstSelection == 0u
				&& ranges[0].selectionCount == 3u && !ranges[0].singleton
				&& ranges[1].firstSelection == 3u && ranges[1].singleton
				&& ranges[2].firstSelection == 4u
				&& ranges[2].selectionCount == 2u
				&& ranges[2].route == RenderSubmissionRoute::VirtualGeometry,
				"Route or packet-template boundaries must split otherwise contiguous work.");
		});

		registry.Add("Renderer.FramePublication.ResolverUsesFixedOrdinalPagesForCrowds",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			constexpr std::size_t kCrowdCount = 10000u;
			const TerraEngine::UUID objectId = GenerateUUID();
			const TerraEngine::UUID meshId = GenerateUUID();
			const TerraEngine::UUID materialId = GenerateUUID();
			auto publication = std::make_shared<RenderFramePublication>();
			publication->frameId = 1u;
			publication->rendererSessionGeneration = 1u;
			publication->sceneGeneration = 1u;
			publication->generation = 1u;
			auto persistent = std::make_shared<RenderPersistentTopologyRoot>();
			persistent->generation = 1u;
			persistent->rendererSessionGeneration = 1u;
			auto dynamic = std::make_shared<RenderDynamicStateRoot>();
			dynamic->generation = 1u;
			dynamic->rendererSessionGeneration = 1u;
			for(std::size_t first = 0u; first < kCrowdCount;
				first += kRenderPublicationPageCapacity) {
				auto persistentPage = std::make_shared<RenderPersistentPage>();
				persistentPage->generation = 1u;
				persistentPage->firstOrdinal = static_cast<std::uint32_t>(first);
				auto dynamicPage = std::make_shared<RenderDynamicPage>();
				dynamicPage->generation = 1u;
				dynamicPage->firstOrdinal = static_cast<std::uint32_t>(first);
				const std::size_t pageEnd = std::min(first + kRenderPublicationPageCapacity,
					kCrowdCount);
				for(std::size_t ordinal = first; ordinal < pageEnd; ++ordinal) {
					RenderPersistentSlot slot{};
					slot.key = { static_cast<std::uint32_t>(ordinal), 1u };
					slot.objectId = objectId;
					slot.meshId = meshId;
					slot.materialId = materialId;
					slot.meshGeneration = 1u;
					slot.materialGeneration = 1u;
					slot.resourceGeneration = 1u;
					slot.packetTemplateIdentity = ordinal + 1u;
					slot.occupied = true;
					persistentPage->slots.push_back(slot);
					RenderDynamicLane lane{};
					lane.key = slot.key;
					lane.instance.modelMatrix[3][0] = static_cast<float>(ordinal);
					lane.occupied = true;
					dynamicPage->lanes.push_back(lane);
					dynamicPage->changedLanes.push_back(true);
				}
				persistent->pages.push_back(std::move(persistentPage));
				dynamic->pages.push_back(std::move(dynamicPage));
			}
			publication->persistent = std::move(persistent);
			publication->dynamic = std::move(dynamic);
			RenderViewSelection selection{};
			selection.generation = 1u;
			RenderViewPresentationInput presentation{};
			presentation.output.kind = selection.outputKind;
			presentation.output.diagnosticsView = selection.viewType;
			presentation.output.outputId = selection.outputId;
			for(std::size_t ordinal = 0u; ordinal < kCrowdCount; ++ordinal) {
				selection.visibleOrdinals.push_back({ static_cast<std::uint32_t>(ordinal), 1u });
				selection.selectedLods.push_back(0u);
				selection.routeSelectors.push_back(0u);
				selection.packetTemplateIdentities.push_back(ordinal + 1u);
				selection.tintSelectors.push_back(0u);
				selection.editorTints.push_back(glm::vec4(0.0f));
				presentation.commandLanes.emplace_back();
				presentation.commandSelectionIndices.push_back(static_cast<std::uint32_t>(ordinal));
			}
			publication->views.push_back(std::move(selection));
			publication->presentationInputs.push_back(std::move(presentation));
			const RenderViewCommandResolver resolver =
				MakeRenderViewCommandResolver(*publication, 0u);
			std::size_t resolvedCount = 0u;
			RenderItemRecord command{};
			for(std::size_t ordinal = 0u; ordinal < resolver.Count(); ++ordinal) {
				resolvedCount += resolver.Resolve(ordinal, command) ? 1u : 0u;
			}
			const std::size_t expectedPages =
				(kCrowdCount + kRenderPublicationPageCapacity - 1u)
				/ kRenderPublicationPageCapacity;
			context.Expect(resolver.Count() == kCrowdCount && resolvedCount == kCrowdCount
				&& publication->persistent->pages.size() == expectedPages
				&& publication->dynamic->pages.size() == expectedPages
				&& resolver.Resolve(kCrowdCount - 1u, command)
				&& command.modelMatrix[3][0] == static_cast<float>(kCrowdCount - 1u),
				"A 10k crowd must conserve every command through fixed 256-slot ordinal pages without per-selection page scans.");
		});

		registry.Add("Renderer.FramePublication.RetainedViewArraysResolveWithoutCopies",
			[](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			auto publication = std::make_shared<RenderFramePublication>(
				*BuildPublication(1u, 0u));
			RenderViewSelection& selection = publication->views.front();
			auto retainedSelection = std::make_shared<RenderViewSelectionPayload>();
			retainedSelection->visibleOrdinals = std::move(selection.visibleOrdinals);
			retainedSelection->selectedLods = std::move(selection.selectedLods);
			retainedSelection->routeSelectors = std::move(selection.routeSelectors);
			retainedSelection->packetTemplateIdentities =
				std::move(selection.packetTemplateIdentities);
			retainedSelection->tintSelectors = std::move(selection.tintSelectors);
			retainedSelection->editorTints = std::move(selection.editorTints);
			selection.retainedPayload = retainedSelection;
			RenderViewPresentationInput& presentation =
				publication->presentationInputs.front();
			presentation.retainedCommandLanes =
				std::make_shared<const std::vector<RenderViewCommandLane>>(
					std::move(presentation.commandLanes));
			presentation.retainedCommandSelectionIndices =
				std::make_shared<const std::vector<std::uint32_t>>(
					std::move(presentation.commandSelectionIndices));

			const RenderPublicationReceipt validation =
				ValidateRenderFramePublication(*publication, 1u);
			const RenderViewCommandResolver resolver =
				MakeRenderViewCommandResolver(*publication, 0u);
			RenderItemRecord resolved{};
			context.Expect(validation.IsSuccess() && resolver.Count() == 1u
				&& resolver.Resolve(0u, resolved)
				&& selection.visibleOrdinals.empty()
				&& presentation.commandLanes.empty()
				&& presentation.commandSelectionIndices.empty(),
				"A sealed view must validate and resolve exclusively through retained immutable selection, lane, and mapping arrays.");
		});
	}
}
