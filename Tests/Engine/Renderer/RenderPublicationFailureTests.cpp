#include "TestHarness.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "RendererThreadingTestFixtures.h"

#include <memory>
#include <cstring>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		std::shared_ptr<const Renderer::RenderFramePublication> BuildPublication(
			std::uint64_t session, std::uint64_t previous)
		{
			using namespace Renderer;
			auto persistentPage = std::make_shared<RenderPersistentPage>();
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
			persistentPage->generation = 1u;
			persistentPage->slots.push_back(slot);

			auto dynamicPage = std::make_shared<RenderDynamicPage>();
			RenderDynamicLane lane{};
			lane.key = slot.key;
			lane.occupied = true;
			dynamicPage->generation = 1u;
			dynamicPage->lanes.push_back(lane);
			dynamicPage->changedLanes.push_back(true);

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

			RenderViewSelection selection{};
			selection.outputId = 1u;
			selection.generation = 1u;
			selection.visibleOrdinals.push_back(slot.key);
			selection.selectedLods.push_back(0u);
			selection.routeSelectors.push_back(0u);
			selection.packetTemplateIdentities.push_back(slot.packetTemplateIdentity);
			selection.tintSelectors.push_back(0u);
			selection.editorTints.push_back(glm::vec4(0.0f));
			publication->views.push_back(selection);
			RenderViewPresentationInput presentation{};
			presentation.output.kind = selection.outputKind;
			presentation.output.diagnosticsView = selection.viewType;
			presentation.output.outputId = selection.outputId;
			const RenderReferenceFrame referenceFrame =
				MakeRendererTestReferenceFrame(publication->frameId, 1u, selection.outputId);
			presentation.referenceFrame = referenceFrame;
			publication->referenceFrames.push_back(referenceFrame);
			presentation.commandLanes.emplace_back();
			presentation.commandSelectionIndices.push_back(0u);
			publication->presentationInputs.push_back(std::move(presentation));
			publication->submissionRanges.push_back(
				{ RenderSubmissionRoute::Direct, 0u, 0u, 1u,
					slot.packetTemplateIdentity, true });
			return publication;
		}

		std::shared_ptr<Renderer::RenderFramePublication> BuildSlotReusedCandidate(
			std::uint64_t session, std::uint64_t previous)
		{
			using namespace Renderer;
			auto candidate = std::make_shared<RenderFramePublication>(
				*BuildPublication(session, previous));
			auto persistent = std::make_shared<RenderPersistentTopologyRoot>(
				*candidate->persistent);
			auto persistentPage = std::make_shared<RenderPersistentPage>(
				*persistent->pages.front());
			persistentPage->slots.front().key.lifetimeGeneration = 2u;
			persistent->pages.front() = std::move(persistentPage);
			candidate->persistent = std::move(persistent);

			auto dynamic = std::make_shared<RenderDynamicStateRoot>(*candidate->dynamic);
			auto dynamicPage = std::make_shared<RenderDynamicPage>(*dynamic->pages.front());
			dynamicPage->lanes.front().key.lifetimeGeneration = 2u;
			dynamic->pages.front() = std::move(dynamicPage);
			candidate->dynamic = std::move(dynamic);
			return candidate;
		}

		Renderer::RenderSlotAllocationRequest MakeAllocationRequest()
		{
			Renderer::RenderSlotAllocationRequest request{};
			request.objectId = GenerateUUID();
			request.meshId = GenerateUUID();
			request.materialId = GenerateUUID();
			request.meshGeneration = 1u;
			request.materialGeneration = 1u;
			request.resourceGeneration = 1u;
			request.packetTemplateIdentity = 11u;
			return request;
		}
	}

	void RegisterRenderPublicationFailureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PublicationFailure.ReceiptOwnsBoundedTextAndNumericIdentity", [](TestContext& context) {
			// Long and short replacements exercise bounded termination and ownership;
			// mutating the original afterward must never alter a copied result.
			Renderer::RenderPublicationReceipt source;
			source.publicationGeneration = 41;
			source.expectedPreviousGeneration = 39;
			source.observedPreviousGeneration = 40;
			source.SetDiagnostic(std::string(600, 'x'));
			context.Expect(std::strlen(source.DiagnosticText()) == 255,
				"Oversized diagnostics must be bounded and NUL terminated.");
			auto copied = source;
			source.SetDiagnostic("short");
			context.Expect(std::strlen(copied.DiagnosticText()) == 255
				&& std::string(source.DiagnosticText()) == "short",
				"Receipt text must be owned independently, including shorter replacements.");
			Renderer::RenderPublicationReceipt assigned;
			assigned = std::move(copied);
			context.Expect(assigned.publicationGeneration == 41
				&& assigned.expectedPreviousGeneration == 39
				&& assigned.observedPreviousGeneration == 40,
				"No-fail receipt moves must preserve every numeric generation.");
			source.SetDiagnostic({});
			context.Expect(source.DiagnosticText()[0] == '\0',
				"Clearing diagnostics must report empty text without stale suffixes.");
		});
		registry.Add("Renderer.PublicationFailure.RejectionKeepsOwnedDiagnosticAfterCandidateRelease", [](TestContext& context) {
			Renderer::RenderPublicationReceipt result;
			{
				auto candidate = BuildPublication(2, 31);
				result = Renderer::ValidateRenderFramePublication(*candidate, 1);
			}
			context.Expect(result.outcome == Renderer::RenderPublicationOutcome::RejectedSession
				&& result.publicationGeneration == 32 && result.expectedPreviousGeneration == 31
				&& std::string(result.DiagnosticText()).find("active renderer session") != std::string::npos,
				"A rejected receipt must preserve identities and owned text after its input retires.");
		});
		registry.Add("Renderer.PublicationFailure.NullCandidateIsAtomic", [](TestContext& context) {
			Renderer::RenderFramePublicationStore store;
			store.ResetForSession(2U);
			const auto receipt = store.TryPublish(
				Renderer::SealRenderFramePublication(nullptr, 2U), 2U);
			context.Expect(!receipt.IsSuccess(), "A null publication must be rejected.");
			context.Expect(store.Load() == nullptr,
				"A rejected candidate must leave the protected root unchanged.");
			context.ExpectEqual<std::uint64_t>(2U, store.SessionGeneration(),
				"Rejection must not advance the renderer session generation.");
		});

		registry.Add("Renderer.PublicationFailure.RejectsStaleVisibleKeysAtomically",
			[](TestContext& context) {
			using namespace Renderer;
			RenderFramePublicationStore store;
			const auto baseline = BuildPublication(store.SessionGeneration(), 0u);
			context.Expect(store.TryPublish(SealRenderFramePublication(
				baseline, store.SessionGeneration()), store.SessionGeneration()).IsSuccess(),
				"The baseline root must publish before atomic stale-key rejection is tested.");

			auto staleSelection = std::make_shared<RenderFramePublication>(
				*BuildPublication(store.SessionGeneration(), baseline->generation));
			staleSelection->views.front().visibleOrdinals.front().lifetimeGeneration = 2u;
			const SealedRenderFramePublication staleSelectionSeal =
				SealRenderFramePublication(staleSelection, store.SessionGeneration());
			const RenderPublicationReceipt staleSelectionReceipt = store.TryPublish(
				staleSelectionSeal, store.SessionGeneration());
			context.Expect(staleSelectionReceipt.outcome
				== RenderPublicationOutcome::RejectedStaleGeneration
				&& store.Load() == baseline,
				"A stale visible lifetime key must reject before it can replace the root.");

			const auto slotReused = BuildSlotReusedCandidate(store.SessionGeneration(),
				baseline->generation);
			const SealedRenderFramePublication slotReusedSeal =
				SealRenderFramePublication(slotReused, store.SessionGeneration());
			const RenderPublicationReceipt slotReusedReceipt = store.TryPublish(
				slotReusedSeal, store.SessionGeneration());
			context.Expect(slotReusedReceipt.outcome
				== RenderPublicationOutcome::RejectedStaleGeneration
				&& store.Load() == baseline,
				"A reused slot must reject an old visible key instead of submitting a partial view.");
		});

		registry.Add("Renderer.PublicationFailure.ReusesReleasedSlotWithNewLifetimeAtomically",
			[](TestContext& context) {
			using namespace Renderer;
			const RenderSlotAllocatorState initial = CreateRenderSlotAllocatorState(7u, 1u);
			auto allocateFirst = BeginRenderSlotAllocatorTransaction(initial, 2u);
			const std::optional<RenderSlotGenerationKey> first =
				allocateFirst.Allocate(MakeAllocationRequest());
			const std::optional<RenderSlotAllocatorState> firstCommitted =
				allocateFirst.Commit();
			context.Expect(first.has_value() && firstCommitted.has_value()
				&& first->ordinal == 0u && first->lifetimeGeneration == 1u
				&& firstCommitted->topology->pages.front()->slots.front()
					.packetTemplateIdentity == 11u
				&& firstCommitted->topology->pages.front()
					->producerValidatedSlotCount == 1u,
				"Allocation must publish a complete packet-template slot with its first lifetime key.");

			auto release = BeginRenderSlotAllocatorTransaction(*firstCommitted, 3u);
			const bool released = first.has_value() && release.Release(*first);
			const std::optional<RenderSlotAllocatorState> releasedCommitted = release.Commit();
			auto reuse = releasedCommitted
				? BeginRenderSlotAllocatorTransaction(*releasedCommitted, 4u)
				: RenderSlotAllocatorTransaction{};
			const std::optional<RenderSlotGenerationKey> replacement =
				reuse.Allocate(MakeAllocationRequest());
			const std::optional<RenderSlotAllocatorState> replacementCommitted = reuse.Commit();
			context.Expect(released && replacement.has_value()
				&& replacementCommitted.has_value()
				&& replacement->ordinal == first->ordinal
				&& replacement->lifetimeGeneration == first->lifetimeGeneration + 1u
				&& firstCommitted->topology->pages.front().get()
					!= replacementCommitted->topology->pages.front().get()
				&& firstCommitted->topology->pages.front()->slots.front().key == *first,
				"Release and reuse must copy only the touched page, retain old readers, and advance the slot lifetime generation.");

			auto abandoned = BeginRenderSlotAllocatorTransaction(*replacementCommitted, 5u);
			const std::optional<RenderSlotGenerationKey> abandonedKey =
				abandoned.Allocate(MakeAllocationRequest());
			abandoned.Rollback();
			context.Expect(abandonedKey.has_value() && !abandoned.Commit().has_value()
				&& replacementCommitted->topology->pages.front()->slots.front().key
					== *replacement,
				"Rollback must discard free-list and page edits without changing the committed root.");
		});

		registry.Add("Renderer.PublicationFailure.RetainsUntouchedTopologyPages",
			[](TestContext& context) {
			using namespace Renderer;
			const RenderSlotAllocatorState initial = CreateRenderSlotAllocatorState(9u, 1u);
			auto fill = BeginRenderSlotAllocatorTransaction(initial, 2u);
			std::vector<RenderSlotGenerationKey> keys;
			keys.reserve(kRenderPublicationPageCapacity + 1u);
			for(std::size_t index = 0u; index <= kRenderPublicationPageCapacity; ++index)
			{
				const std::optional<RenderSlotGenerationKey> key =
					fill.Allocate(MakeAllocationRequest());
				if(key.has_value()) keys.push_back(*key);
			}
			const std::optional<RenderSlotAllocatorState> populated = fill.Commit();
			auto release = populated
				? BeginRenderSlotAllocatorTransaction(*populated, 3u)
				: RenderSlotAllocatorTransaction{};
			const bool released = !keys.empty() && release.Release(keys.front());
			const std::optional<RenderSlotAllocatorState> changed = release.Commit();
			context.Expect(populated.has_value() && changed.has_value()
				&& keys.size() == kRenderPublicationPageCapacity + 1u
				&& released && populated->topology->pages.size() == 2u
				&& changed->topology->pages.size() == 2u
				&& populated->topology->pages[0u].get()
					!= changed->topology->pages[0u].get()
				&& populated->topology->pages[1u].get()
					== changed->topology->pages[1u].get(),
				"A release must copy the affected fixed page while retaining immutable pages that contain no changed slots.");
		});
	}
}
