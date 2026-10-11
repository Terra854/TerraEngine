// RenderFramePublication.cpp - Validation and protected immutable-root exchange.

#include "RenderFramePublication.h"
#include "RenderFrameSnapshot.h"

#include <algorithm>
#include <new>
#include <unordered_set>

namespace TerraEngine::Renderer
{
	namespace
	{
		RenderPublicationReceipt Reject(
			const RenderFramePublication& publication,
			RenderPublicationOutcome outcome,
			const char* diagnostic) noexcept
		{
			RenderPublicationReceipt receipt = publication.receipt;
			receipt.outcome = outcome;
			receipt.publicationGeneration = publication.generation;
			receipt.expectedPreviousGeneration = publication.expectedPreviousGeneration;
			receipt.diagnostic = diagnostic;
			return receipt;
		}

		// A view carries slot lifetime keys rather than pointers into renderer state.
		// Seal-time validation must prove that every selected key still names the
		// occupied persistent slot and its matching dynamic lane.  Otherwise a slot
		// recycled after culling could publish successfully and only disappear when
		// the backend later tries to resolve it.
		bool IsCurrentSelectedSlotAfterRootValidation(
			const RenderFramePublication& publication,
			const RenderSlotGenerationKey& key) noexcept
		{
			if(!key.IsValid() || publication.persistent == nullptr
				|| publication.dynamic == nullptr) {
				return false;
			}

			const std::size_t pageIndex = key.ordinal / kRenderPublicationPageCapacity;
			const std::size_t slotIndex = key.ordinal % kRenderPublicationPageCapacity;
			if(pageIndex >= publication.persistent->pages.size()
				|| pageIndex >= publication.dynamic->pages.size()) {
				return false;
			}

			const std::shared_ptr<const RenderPersistentPage>& persistentPage =
				publication.persistent->pages[pageIndex];
			const std::shared_ptr<const RenderDynamicPage>& dynamicPage =
				publication.dynamic->pages[pageIndex];
			if(persistentPage == nullptr || dynamicPage == nullptr
				|| slotIndex >= persistentPage->slots.size()
				|| slotIndex >= dynamicPage->lanes.size()) {
				return false;
			}

			const RenderPersistentSlot& persistent = persistentPage->slots[slotIndex];
			const RenderDynamicLane& dynamic = dynamicPage->lanes[slotIndex];
			// The root scan immediately before view validation has already proved
			// completeness, occupancy parity, fixed addresses, and matching lifetime
			// keys for every persistent/dynamic lane. Selection therefore needs only
			// an exact key comparison; repeating UUID and resource completeness checks
			// for every visible object would make sealing scan the crowd twice.
			return persistent.occupied && dynamic.occupied
				&& persistent.key == key && dynamic.key == key;
		}
	}

	RenderPublicationReceipt ValidateRenderFramePublication(
		const RenderFramePublication& publication,
		std::uint64_t expectedSessionGeneration,
		const RenderPublicationFailureInjection& injection) noexcept
	{
		if(injection.failAllocation)
			return Reject(publication, RenderPublicationOutcome::RejectedAllocation,
				"A deterministic allocation failure rejected the complete candidate.");
		if(injection.cancelled)
			return Reject(publication, RenderPublicationOutcome::RejectedCancellation,
				"A cancelled worker result cannot become backend-visible.");
		if(injection.backendPreparationFailed)
			return Reject(publication, RenderPublicationOutcome::RejectedBackendPreparation,
				"Backend frame-slot preparation failed before publication.");
		if(publication.frameId == 0u || publication.generation == 0u
			|| publication.rendererSessionGeneration != expectedSessionGeneration
			|| publication.backendResourceGeneration == 0u
			|| publication.persistent == nullptr || publication.dynamic == nullptr
			|| publication.persistent->generation == 0u
			|| publication.dynamic->generation == 0u
			|| publication.persistent->rendererSessionGeneration != expectedSessionGeneration
			|| publication.dynamic->rendererSessionGeneration != expectedSessionGeneration)
			return Reject(publication, RenderPublicationOutcome::RejectedSession,
				"The candidate does not belong to the active renderer session.");
		if(!publication.referenceFrames.empty())
		{
			std::unordered_set<std::uint64_t> viewIdentities;
			for(const RenderReferenceFrame& referenceFrame : publication.referenceFrames)
			{
				if(!referenceFrame.IsValid() || referenceFrame.frameId != publication.frameId
					|| !viewIdentities.insert(referenceFrame.viewId).second)
					return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
						"A render reference frame is invalid, stale, or duplicated.");
			}
		}
		for (const RenderViewPresentationInput& presentation :
			publication.presentationInputs)
		{
			if (!presentation.referenceFrame.IsValid()
				|| presentation.referenceFrame.frameId != publication.frameId
				|| presentation.referenceFrame.outputId
					!= presentation.output.outputId)
			{
				return Reject(publication,
					RenderPublicationOutcome::RejectedStaleGeneration,
					"A presentation input does not match its render reference frame.");
			}
		}
		if(publication.backendWork == nullptr || !publication.backendWork->valid
			|| publication.backendWork->frameId != publication.frameId
			|| !publication.backendWork->animationWork.valid
			|| publication.backendWork->animationWork.frameId != publication.frameId
			|| (publication.backendWork->deformationWork != nullptr
				&& !publication.backendWork->deformationWork->IsComplete()))
			return Reject(publication, RenderPublicationOutcome::RejectedBackendPreparation,
				"The backend work root is incomplete or belongs to another frame.");
		if(publication.persistent->pages.size()
			!= publication.dynamic->pages.size())
			return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
				"Persistent and dynamic roots do not describe the same page set.");

		try
		{
			for(std::size_t pageIndex = 0u;
				pageIndex < publication.persistent->pages.size(); ++pageIndex)
			{
				const std::shared_ptr<const RenderPersistentPage>& page =
					publication.persistent->pages[pageIndex];
				if(page == nullptr || page->generation == 0u
					|| page->firstOrdinal != pageIndex * kRenderPublicationPageCapacity
					|| page->slots.empty()
					|| page->slots.size() > kRenderPublicationPageCapacity)
					return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
						"A persistent page is absent or belongs to no generation.");
				const bool producerValidated = page->producerValidatedSlotCount
					== page->slots.size();
				for(std::size_t slotIndex = 0u;
					!producerValidated && slotIndex < page->slots.size(); ++slotIndex)
				{
					const RenderPersistentSlot& slot = page->slots[slotIndex];
					if(!slot.occupied) continue;
					if(!slot.IsComplete())
						return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
							"A persistent slot is incomplete.");
					if(slot.key.ordinal != page->firstOrdinal + slotIndex)
						return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
							"A persistent slot does not match its fixed ordinal address.");
					// A slot's ordinal must equal its unique page address. Once this
					// invariant holds, two occupied slots cannot share a key without one
					// of them first failing the address check above. Avoid maintaining and
					// linearly searching a crowd-sized duplicate table on every frame.
				}
			}
			for(std::size_t pageIndex = 0u;
				pageIndex < publication.dynamic->pages.size(); ++pageIndex)
			{
				const std::shared_ptr<const RenderDynamicPage>& page =
					publication.dynamic->pages[pageIndex];
				const std::shared_ptr<const RenderPersistentPage>& persistentPage =
					publication.persistent->pages[pageIndex];
				if(page == nullptr || page->generation == 0u
					|| page->firstOrdinal != pageIndex * kRenderPublicationPageCapacity
					|| page->lanes.empty() || page->lanes.size() > kRenderPublicationPageCapacity
					|| page->changedLanes.size() != page->lanes.size()
					|| persistentPage == nullptr
					|| persistentPage->slots.size() != page->lanes.size())
					return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
						"A dynamic page is incomplete or has an invalid changed-lane mask.");
				const bool producerValidated = page->producerValidatedLaneCount
					== page->lanes.size()
					&& page->producerValidatedPersistentGeneration
						== persistentPage->generation;
				for(std::size_t laneIndex = 0u;
					!producerValidated && laneIndex < page->lanes.size(); ++laneIndex) {
					const RenderDynamicLane& lane = page->lanes[laneIndex];
					const RenderPersistentSlot& persistent =
						persistentPage->slots[laneIndex];
					if(lane.occupied && lane.key.ordinal != page->firstOrdinal + laneIndex)
						return Reject(publication, RenderPublicationOutcome::RejectedStaleGeneration,
							"A dynamic lane does not match its fixed ordinal address.");
					if(lane.occupied != persistent.occupied
						|| (lane.occupied && !lane.Matches(persistent)))
						return Reject(publication,
							RenderPublicationOutcome::RejectedStaleGeneration,
							"Persistent and dynamic slot lifetimes do not match.");
				}
			}
			for(const RenderViewSelection& view : publication.views)
			{
				if(view.generation == 0u || !view.HasParallelLengths())
					return Reject(publication, RenderPublicationOutcome::RejectedSelectorLength,
						"A view selector array does not match visible stable ordinals.");
				for(const RenderSlotGenerationKey& selectedKey : view.VisibleOrdinals())
				{
					if(!IsCurrentSelectedSlotAfterRootValidation(
						publication, selectedKey))
						return Reject(publication,
							RenderPublicationOutcome::RejectedStaleGeneration,
							"A view selection names a stale or recycled render slot.");
				}
				if(view.viewType == RenderViewType::Shadow && view.lightIdentity == 0u)
					return Reject(publication, RenderPublicationOutcome::RejectedRange,
						"A shadow selection requires an explicit light identity.");
			}
			if(publication.presentationInputs.size() != publication.views.size())
				return Reject(publication, RenderPublicationOutcome::RejectedSelectorLength,
					"Every compact view selection requires one immutable presentation input.");
			for(std::size_t viewIndex = 0u; viewIndex < publication.views.size(); ++viewIndex)
			{
				if(!publication.presentationInputs[viewIndex].Matches(
					publication.views[viewIndex]))
					return Reject(publication, RenderPublicationOutcome::RejectedRange,
						"A view presentation input does not match its compact selection.");
				const RenderViewPresentationInput& presentation =
					publication.presentationInputs[viewIndex];
				const RenderViewSelection& selection = publication.views[viewIndex];
				if(presentation.CommandLanes().size()
					!= presentation.CommandSelectionIndices().size()
					|| std::any_of(presentation.CommandSelectionIndices().begin(),
						presentation.CommandSelectionIndices().end(), [&selection](std::uint32_t index) {
							return index != RenderSlotGenerationKey::kInvalidOrdinal
								&& index >= selection.VisibleOrdinals().size();
						}))
					return Reject(publication, RenderPublicationOutcome::RejectedSelectorLength,
						"A compact command lane does not name a valid view selection.");
				for(const ShadowDrawList& shadow : presentation.shadowDrawLists)
				{
					if (shadow.instanceInputs && !ShadowPreparedInstanceInputsCurrent(publication, presentation, shadow))
						return Reject(publication, RenderPublicationOutcome::RejectedSelectorLength,
							"A shadow instance stream names a stale frame, light, view or reference frame.");
					if(shadow.lightId == TerraEngine::EMPTY_UUID
						|| !shadow.HasParallelLengths()
						|| shadow.topologyGeneration != publication.persistent->generation
						|| std::any_of(shadow.stableOrdinals.begin(),
							shadow.stableOrdinals.end(), [&publication](std::uint32_t ordinal) {
								const std::size_t pageIndex = ordinal
									/ kRenderPublicationPageCapacity;
								const std::size_t slotIndex = ordinal
									% kRenderPublicationPageCapacity;
								return pageIndex >= publication.persistent->pages.size()
									|| publication.persistent->pages[pageIndex] == nullptr
									|| slotIndex >= publication.persistent->pages[pageIndex]
										->slots.size();
							}))
					{
						return Reject(publication,
							RenderPublicationOutcome::RejectedSelectorLength,
							"A compact shadow selection is incomplete or stale.");
					}
				}
			}
			for(const RenderSubmissionRange& range : publication.submissionRanges)
			{
				if(!range.IsComplete() || range.viewIndex >= publication.views.size())
					return Reject(publication, RenderPublicationOutcome::RejectedRange,
						"A backend range is incomplete or refers to an absent view.");
				const RenderViewSelection& view = publication.views[range.viewIndex];
				if(range.firstSelection > view.VisibleOrdinals().size()
					|| range.selectionCount > view.VisibleOrdinals().size()
						- range.firstSelection)
					return Reject(publication, RenderPublicationOutcome::RejectedRange,
						"A backend range exceeds its compact view selection.");
				const auto templateBegin = view.PacketTemplateIdentities().begin()
					+ range.firstSelection;
				const auto templateEnd = templateBegin + range.selectionCount;
				if(std::any_of(templateBegin, templateEnd,
					[&range](std::uint64_t identity) {
						return identity != range.packetTemplateIdentity;
					}))
					return Reject(publication, RenderPublicationOutcome::RejectedRange,
						"A backend range mixes or misidentifies persistent packet templates.");
			}
			if(!publication.resourcePreparation.records.empty()
				&& publication.resourcePreparation.generation == 0u)
				return Reject(publication,
					RenderPublicationOutcome::RejectedBackendPreparation,
					"Resource preparation table has no generation.");
			for(std::size_t index = 0u;
				index < publication.resourcePreparation.records.size(); ++index)
			{
				const RenderResourcePreparationRecord& record =
					publication.resourcePreparation.records[index];
				if(!record.IsComplete() || record.preparationIndex != index)
					return Reject(publication,
						RenderPublicationOutcome::RejectedBackendPreparation,
						"Resource preparation records are incomplete or not densely indexed.");
				if(index != 0u
					&& publication.resourcePreparation.records[index - 1u].resourceId
						>= record.resourceId)
					return Reject(publication,
						RenderPublicationOutcome::RejectedBackendPreparation,
						"Resource preparation records are not deterministically ordered.");
			}
		}
		catch(const std::bad_alloc&)
		{
			return Reject(publication, RenderPublicationOutcome::RejectedAllocation,
				"Validation could not reserve its bounded duplicate-slot evidence.");
		}

		RenderPublicationReceipt receipt = publication.receipt;
		receipt.outcome = RenderPublicationOutcome::Published;
		receipt.publicationGeneration = publication.generation;
		receipt.expectedPreviousGeneration = publication.expectedPreviousGeneration;
		receipt.viewCount = static_cast<std::uint32_t>(publication.views.size());
		receipt.rangeCount = static_cast<std::uint32_t>(publication.submissionRanges.size());
		return receipt;
	}

	RenderViewCommandResolver MakeRenderViewCommandResolver(
		const RenderFramePublication& publication, std::size_t viewIndex) noexcept
	{
		RenderViewCommandResolver resolver;
		if(publication.persistent == nullptr || publication.dynamic == nullptr
			|| viewIndex >= publication.views.size()
			|| viewIndex >= publication.presentationInputs.size()) {
			return resolver;
		}
		const RenderViewSelection& selection = publication.views[viewIndex];
		const RenderViewPresentationInput& presentation = publication.presentationInputs[viewIndex];
		if(!selection.HasParallelLengths()
			|| presentation.CommandLanes().size()
				!= presentation.CommandSelectionIndices().size()
			|| !presentation.Matches(selection)) {
			return resolver;
		}
		resolver.publication_ = &publication;
		resolver.selection_ = &selection;
		resolver.presentation_ = &presentation;
		return resolver;
	}

	const RenderResourcePreparationRecord* RenderResourcePreparationTable::Find(
		TerraEngine::UUID resourceId) const noexcept
	{
		for(const RenderResourcePreparationRecord& record : records)
		{
			if(record.resourceId == resourceId)
				return &record;
		}
		return nullptr;
	}

	RenderResourcePreparationTable BuildRenderResourcePreparationTable(
		std::span<const RenderResourcePreparationRecord> records,
		std::uint64_t generation)
	{
		RenderResourcePreparationTable table;
		table.generation = generation;
		table.records.assign(records.begin(), records.end());
		std::sort(table.records.begin(), table.records.end(),
			[](const RenderResourcePreparationRecord& left,
				const RenderResourcePreparationRecord& right) {
				// UUID is a fixed 16-byte array. Its native lexicographic comparison
				// preserves the serialized hex order without allocating two strings
				// for every sort comparison on every frame.
				return left.resourceId < right.resourceId;
			});
		table.records.erase(std::unique(table.records.begin(), table.records.end(),
			[](const RenderResourcePreparationRecord& left,
				const RenderResourcePreparationRecord& right) {
				return left.resourceId == right.resourceId;
			}), table.records.end());
		for(std::size_t index = 0u; index < table.records.size(); ++index)
			table.records[index].preparationIndex = static_cast<std::uint32_t>(index);
		return table;
	}

	std::vector<RenderSubmissionRange> BuildRenderSubmissionRanges(
		const RenderViewSelection& selection, std::uint32_t viewIndex)
	{
		std::vector<RenderSubmissionRange> ranges;
		if(!selection.HasParallelLengths() || selection.VisibleOrdinals().empty())
			return ranges;

		ranges.reserve(selection.VisibleOrdinals().size());
		const auto resolveRoute = [&selection](std::size_t index) noexcept
		{
			const std::uint32_t selector = selection.RouteSelectors()[index];
			if(selector == static_cast<std::uint32_t>(
				GeometryRoute::VirtualGeometryHardware)
				|| selector == static_cast<std::uint32_t>(
					GeometryRoute::VirtualGeometryCompute))
			{
				return RenderSubmissionRoute::VirtualGeometry;
			}
			return selection.outputKind == RenderOutputKind::OffscreenTexture
				? RenderSubmissionRoute::Capture
				: RenderSubmissionRoute::Direct;
		};

		for(std::size_t selectionIndex = 0u;
			selectionIndex < selection.VisibleOrdinals().size(); ++selectionIndex)
		{
			const RenderSubmissionRoute route = resolveRoute(selectionIndex);
			const std::uint64_t packetTemplateIdentity =
				selection.PacketTemplateIdentities()[selectionIndex];
			if(!ranges.empty())
			{
				RenderSubmissionRange& tail = ranges.back();
				if(tail.route == route
					&& tail.packetTemplateIdentity == packetTemplateIdentity
					&& tail.firstSelection + tail.selectionCount == selectionIndex)
				{
					++tail.selectionCount;
					tail.singleton = false;
					continue;
				}
			}

			RenderSubmissionRange range{};
			range.route = route;
			range.viewIndex = viewIndex;
			range.firstSelection = static_cast<std::uint32_t>(selectionIndex);
			range.selectionCount = 1u;
			range.packetTemplateIdentity = packetTemplateIdentity;
			range.singleton = true;
			ranges.push_back(range);
		}
		return ranges;
	}

	RenderSlotAllocatorState CreateRenderSlotAllocatorState(
		std::uint64_t rendererSessionGeneration, std::uint64_t topologyGeneration)
	{
		RenderSlotAllocatorState state;
		if(rendererSessionGeneration == 0u || topologyGeneration == 0u)
			return state;
		state.rendererSessionGeneration = rendererSessionGeneration;
		state.topologyGeneration = topologyGeneration;
		auto topology = std::make_shared<RenderPersistentTopologyRoot>();
		topology->generation = topologyGeneration;
		topology->rendererSessionGeneration = rendererSessionGeneration;
		state.topology = std::move(topology);
		return state;
	}

	RenderSlotAllocatorTransaction BeginRenderSlotAllocatorTransaction(
		const RenderSlotAllocatorState& state, std::uint64_t nextTopologyGeneration)
	{
		RenderSlotAllocatorTransaction transaction;
		if(state.rendererSessionGeneration == 0u || state.topologyGeneration == 0u
			|| state.topology == nullptr || state.topology->generation != state.topologyGeneration
			|| state.topology->rendererSessionGeneration != state.rendererSessionGeneration
			|| nextTopologyGeneration <= state.topologyGeneration) {
			return transaction;
		}
		transaction.working_ = state;
		transaction.nextTopologyGeneration_ = nextTopologyGeneration;
		transaction.active_ = true;
		return transaction;
	}

	bool RenderSlotAllocatorTransaction::EnsureMutableTopology()
	{
		if(!active_ || working_.topology == nullptr)
			return false;
		if(mutableTopology_ == nullptr) {
			mutableTopology_ = std::make_shared<RenderPersistentTopologyRoot>(
				*working_.topology);
			mutableTopology_->generation = nextTopologyGeneration_;
		}
		return true;
	}

	bool RenderSlotAllocatorTransaction::ReplaceSlot(std::uint32_t ordinal,
		const RenderPersistentSlot& replacement)
	{
		if(!EnsureMutableTopology())
			return false;
		const std::size_t pageIndex = ordinal / kRenderPublicationPageCapacity;
		const std::size_t slotIndex = ordinal % kRenderPublicationPageCapacity;
		if(pageIndex >= mutableTopology_->pages.size())
			mutableTopology_->pages.resize(pageIndex + 1u);

		std::shared_ptr<RenderPersistentPage> page;
		if(mutableTopology_->pages[pageIndex] == nullptr) {
			page = std::make_shared<RenderPersistentPage>();
			page->firstOrdinal = static_cast<std::uint32_t>(
				pageIndex * kRenderPublicationPageCapacity);
		}
		else {
			page = std::make_shared<RenderPersistentPage>(
				*mutableTopology_->pages[pageIndex]);
		}
		page->generation = nextTopologyGeneration_;
		if(page->slots.size() <= slotIndex)
			page->slots.resize(slotIndex + 1u);
		page->slots[slotIndex] = replacement;
		// This transaction is the sole producer for allocator pages. Validate the
		// complete page before exposing it as const so frame sealing can trust one
		// immutable proof instead of revisiting stable topology every frame.
		page->producerValidatedSlotCount = page->slots.size();
		for(std::size_t candidateIndex = 0u;
			candidateIndex < page->slots.size(); ++candidateIndex) {
			const RenderPersistentSlot& candidate = page->slots[candidateIndex];
			if(candidate.occupied && (!candidate.IsComplete()
				|| candidate.key.ordinal != page->firstOrdinal + candidateIndex)) {
				page->producerValidatedSlotCount = 0u;
				break;
			}
		}
		mutableTopology_->pages[pageIndex] = std::move(page);
		return true;
	}

	std::optional<RenderSlotGenerationKey> RenderSlotAllocatorTransaction::Allocate(
		const RenderSlotAllocationRequest& request)
	{
		if(!active_ || !request.IsComplete())
			return std::nullopt;

		std::uint32_t ordinal = 0u;
		if(working_.freeOrdinals.empty()) {
			if(working_.nextOrdinal == RenderSlotGenerationKey::kInvalidOrdinal)
				return std::nullopt;
			ordinal = working_.nextOrdinal;
		}
		else {
			ordinal = working_.freeOrdinals.front();
		}
		const std::uint64_t priorGeneration = ordinal < working_.lifetimeGenerations.size()
			? working_.lifetimeGenerations[ordinal] : 0u;
		if(priorGeneration == std::numeric_limits<std::uint64_t>::max())
			return std::nullopt;
		const RenderSlotGenerationKey key{ ordinal, priorGeneration + 1u };

		RenderPersistentSlot slot{};
		slot.key = key;
		slot.objectId = request.objectId;
		slot.meshId = request.meshId;
		slot.materialId = request.materialId;
		slot.meshGeneration = request.meshGeneration;
		slot.materialGeneration = request.materialGeneration;
		slot.resourceGeneration = request.resourceGeneration;
		slot.packetTemplateIdentity = request.packetTemplateIdentity;
		slot.routeTemplate = request.routeTemplate;
		slot.occupied = true;
		if(!ReplaceSlot(ordinal, slot))
			return std::nullopt;

		if(ordinal >= working_.lifetimeGenerations.size())
			working_.lifetimeGenerations.resize(static_cast<std::size_t>(ordinal) + 1u, 0u);
		working_.lifetimeGenerations[ordinal] = key.lifetimeGeneration;
		if(working_.freeOrdinals.empty())
			++working_.nextOrdinal;
		else
			working_.freeOrdinals.erase(working_.freeOrdinals.begin());
		working_.topologyGeneration = nextTopologyGeneration_;
		changed_ = true;
		return key;
	}

	bool RenderSlotAllocatorTransaction::Release(RenderSlotGenerationKey key)
	{
		if(!active_ || !key.IsValid() || working_.topology == nullptr)
			return false;
		const std::size_t pageIndex = key.ordinal / kRenderPublicationPageCapacity;
		const std::size_t slotIndex = key.ordinal % kRenderPublicationPageCapacity;
		const RenderPersistentTopologyRoot* topology = mutableTopology_ != nullptr
			? mutableTopology_.get() : working_.topology.get();
		if(topology == nullptr || pageIndex >= topology->pages.size()
			|| topology->pages[pageIndex] == nullptr
			|| slotIndex >= topology->pages[pageIndex]->slots.size()) {
			return false;
		}
		const RenderPersistentSlot& existing =
			topology->pages[pageIndex]->slots[slotIndex];
		if(!existing.IsComplete() || existing.key != key)
			return false;
		const auto insertion = std::lower_bound(working_.freeOrdinals.begin(),
			working_.freeOrdinals.end(), key.ordinal);
		if(insertion != working_.freeOrdinals.end() && *insertion == key.ordinal)
			return false;
		if(!ReplaceSlot(key.ordinal, {}))
			return false;
		working_.freeOrdinals.insert(insertion, key.ordinal);
		working_.topologyGeneration = nextTopologyGeneration_;
		changed_ = true;
		return true;
	}

	std::optional<RenderSlotAllocatorState> RenderSlotAllocatorTransaction::Commit() const
	{
		if(!active_)
			return std::nullopt;
		RenderSlotAllocatorState committed = working_;
		if(changed_ && mutableTopology_ != nullptr)
			committed.topology = mutableTopology_;
		return committed;
	}

	std::size_t RenderViewCommandResolver::Count() const noexcept
	{
		return selection_ != nullptr && presentation_ != nullptr
			&& presentation_->CommandLanes().size()
				== presentation_->CommandSelectionIndices().size()
			? presentation_->CommandLanes().size() : 0u;
	}

	namespace
	{
		bool ResolvePublishedItem(
			const RenderFramePublication& publication,
			const RenderReferenceFrame& referenceFrame,
			RenderSlotGenerationKey key,
			GeometryRoute route,
			std::uint32_t selectedLod,
			const glm::vec4& editorTint,
			const RenderViewCommandLane& lane,
			RenderItemRecord& output) noexcept
		{
			if(publication.persistent == nullptr || publication.dynamic == nullptr)
				return false;
			const std::size_t pageIndex = key.ordinal / kRenderPublicationPageCapacity;
			const std::size_t laneIndex = key.ordinal % kRenderPublicationPageCapacity;
			if(pageIndex >= publication.persistent->pages.size()
				|| pageIndex >= publication.dynamic->pages.size()) {
				return false;
			}
			const std::shared_ptr<const RenderPersistentPage>& persistentPage =
				publication.persistent->pages[pageIndex];
			const std::shared_ptr<const RenderDynamicPage>& dynamicPage =
				publication.dynamic->pages[pageIndex];
			if(persistentPage == nullptr || dynamicPage == nullptr
				|| persistentPage->firstOrdinal != pageIndex * kRenderPublicationPageCapacity
				|| dynamicPage->firstOrdinal != pageIndex * kRenderPublicationPageCapacity
				|| laneIndex >= persistentPage->slots.size()
				|| laneIndex >= dynamicPage->lanes.size()) {
				return false;
			}
			const RenderPersistentSlot& persistent = persistentPage->slots[laneIndex];
			const RenderDynamicLane& dynamic = dynamicPage->lanes[laneIndex];
			if(!persistent.IsComplete() || persistent.key != key
				|| !dynamic.Matches(persistent)) {
				return false;
			}
			RenderItemRecord resolved{};
			resolved.meshId = persistent.meshId;
			resolved.materialId = persistent.materialId;
			resolved.objectId = persistent.objectId;
			resolved.shadowCasterInputs = dynamic.shadowCasterInputs;
			resolved.objectLifetimeGeneration = dynamic.shadowCasterInputs != nullptr
				&& dynamic.shadowCasterInputs->lifetimeGeneration != 0
				? dynamic.shadowCasterInputs->lifetimeGeneration : persistent.key.lifetimeGeneration;
			resolved.canonicalWorldPose = dynamic.canonicalWorldPose;
			if(dynamic.hasCanonicalWorldPose)
			{
				// Dynamic pages are shared by every camera. Localize their precise
				// canonical pose only when this view consumes the sealed command.
				const RenderLocalizedPoseMatrices localized =
					BuildLocalizedRenderPoseMatrices(
						dynamic.canonicalWorldPose, referenceFrame);
				if(!localized.valid)
					return false;
				resolved.modelMatrix = localized.currentMatrix;
			}
			else
			{
				// Compatibility and hand-authored test publications may still carry
				// a ready local matrix without canonical large-world pose metadata.
				resolved.modelMatrix = dynamic.instance.modelMatrix;
			}
			resolved.conventionalVisibilityIdentityIndex = dynamic.instance.objectIdentity.x;
			resolved.geometryRoute = route;
			resolved.animation = dynamic.animation;
			resolved.animationBinding = dynamic.animationBinding;
			if(publication.backendWork != nullptr
				&& publication.backendWork->animationWork.fullImmutablePlanCoverage
				&& publication.backendWork->animationWork.immutablePlan != nullptr) {
				// Dense animation advances through its own immutable publication root.
				// Geometry pages intentionally retain stable packet ordinals, so resolve
				// the changing generations from the current plan at consumption instead
				// of rebuilding every geometry lane for every pose.
				const auto& plan = *publication.backendWork->animationWork.immutablePlan;
				const std::uint32_t packetIndex =
					resolved.animation.firstGpuWorkPacket;
				if(packetIndex < plan.WorkPackets().size()) {
					const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
						plan.WorkPackets()[packetIndex];
					if(packet.generationRecordIndex < plan.GenerationRecordCount()
						&& packet.stableInstanceId
							== resolved.animation.stableInstanceId) {
						const TerraEngine::Animation::GpuAnimationGenerationRecord&
							generation = plan.GenerationRecord(
								packet.generationRecordIndex);
						resolved.animation.animationPlanGeneration = plan.generation;
						resolved.animation.paletteGeneration =
							publication.animationPresentationGeneration;
						resolved.animation.poseGeneration = generation.workGeneration;
						resolved.animation.morphGeneration = generation.workGeneration;
						// Current GPU work has no retained exact CPU output proof.
						resolved.animation.deformationContentIdLow = 0;
						resolved.animation.deformationContentIdHigh = 0;
						resolved.animation.programGeneration =
							generation.programGeneration;
						resolved.animation.rendererGeneration =
							generation.rendererGeneration;
						resolved.animation.gpuStageMask = packet.stageMask;
					}
				}
			}
			resolved.editorTint = editorTint;
			resolved.materialIndex = lane.materialIndex;
			resolved.meshDataIndex = lane.meshDataIndex;
			resolved.textureIndex = lane.textureIndex;
			resolved.virtualMeshletOffset = lane.virtualMeshletOffset;
			resolved.virtualMeshletCount = lane.virtualMeshletCount;
			resolved.selectedVirtualLod = selectedLod;
			resolved.virtualGeometryFlags = lane.virtualGeometryFlags;
			resolved.selectedVirtualClusterOffset = lane.selectedVirtualClusterOffset;
			resolved.selectedVirtualClusterCount = lane.selectedVirtualClusterCount;
			resolved.selectedVirtualPageNeedOffset = lane.selectedVirtualPageNeedOffset;
			resolved.selectedVirtualPageNeedCount = lane.selectedVirtualPageNeedCount;
			resolved.virtualGeometryPageId = lane.virtualGeometryPageId;
			resolved.virtualGeometryFallbackPageId = lane.virtualGeometryFallbackPageId;
			resolved.virtualGeometryPayloadSource = lane.virtualGeometryPayloadSource;
			resolved.virtualGeometryEmittedSource = lane.virtualGeometryEmittedSource;
			resolved.virtualGeometryPagePayloadByteOffset = lane.virtualGeometryPagePayloadByteOffset;
			resolved.virtualGeometryPagePayloadByteSize = lane.virtualGeometryPagePayloadByteSize;
			resolved.virtualGeometryPayloadBufferIndex = lane.virtualGeometryPayloadBufferIndex;
			resolved.virtualGeometryPayloadDescriptorIndex = lane.virtualGeometryPayloadDescriptorIndex;
			resolved.virtualGeometryPacketDescriptorIndex = lane.virtualGeometryPacketDescriptorIndex;
			resolved.taskShaderCullingEnabled = lane.taskShaderCullingEnabled;
			output = std::move(resolved);
			return true;
		}
	}

	bool RenderViewCommandResolver::Resolve(std::size_t selectionIndex,
		RenderItemRecord& output) const noexcept
	{
		if(publication_ == nullptr || selection_ == nullptr || presentation_ == nullptr
			|| selectionIndex >= Count() || publication_->persistent == nullptr
			|| publication_->dynamic == nullptr) {
			return false;
		}
		const std::uint32_t visibleSelection =
			presentation_->CommandSelectionIndices()[selectionIndex];
		if(visibleSelection == RenderSlotGenerationKey::kInvalidOrdinal
			|| visibleSelection >= selection_->VisibleOrdinals().size()) {
			return false;
		}
		const RenderViewCommandLane& lane =
			presentation_->CommandLanes()[selectionIndex];
		return ResolvePublishedItem(*publication_, presentation_->referenceFrame,
			selection_->VisibleOrdinals()[visibleSelection],
			static_cast<GeometryRoute>(selection_->RouteSelectors()[visibleSelection]),
			selection_->SelectedLods()[visibleSelection],
			selection_->EditorTints()[visibleSelection], lane, output);
	}

	bool ResolveShadowDrawListItem(const RenderFramePublication& publication,
		const RenderReferenceFrame& referenceFrame,
		const ShadowDrawList& shadow, std::size_t itemIndex,
		RenderItemRecord& output) noexcept
	{
		if(!shadow.HasParallelLengths() || shadow.topologyGeneration == 0u
			|| itemIndex >= shadow.stableOrdinals.size() || publication.persistent == nullptr
			|| publication.persistent->generation != shadow.topologyGeneration) {
			return false;
		}
		// The list's generation identifies the topology root, not a slot lifetime.
		// Recover the actual occupied slot key; the shared resolver still validates
		// its lifetime against the matching immutable dynamic lane before use.
		const auto ordinal = shadow.stableOrdinals[itemIndex];
		const std::size_t pageIndex = ordinal / kRenderPublicationPageCapacity;
		const std::size_t slotIndex = ordinal % kRenderPublicationPageCapacity;
		if (pageIndex >= publication.persistent->pages.size()) return false;
		const auto& page = publication.persistent->pages[pageIndex];
		if (!page || slotIndex >= page->slots.size()) return false;
		const auto key = page->slots[slotIndex].key;
		if (!key.IsValid() || key.ordinal != ordinal) return false;
		return ResolvePublishedItem(publication, referenceFrame,
			key,
			static_cast<GeometryRoute>(shadow.routeSelectors[itemIndex]),
			shadow.selectedLods[itemIndex], shadow.editorTints[itemIndex],
			shadow.commandLanes[itemIndex], output);
	}

	std::shared_ptr<const ShadowPreparedInstanceInputs> BuildShadowPreparedInstanceInputs(
		const RenderFramePublication& publication, const RenderViewPresentationInput& presentation,
		const ShadowDrawList& shadow)
	{
		auto inputs = std::make_shared<ShadowPreparedInstanceInputs>();
		inputs->frameSerial = publication.frameId;
		inputs->sessionGeneration = publication.rendererSessionGeneration;
		inputs->topologyGeneration = shadow.topologyGeneration;
		inputs->referenceFrame = presentation.referenceFrame;
		inputs->viewType = presentation.output.diagnosticsView; inputs->outputId = presentation.output.outputId;
		inputs->lightId = shadow.lightId; inputs->records.reserve(shadow.stableOrdinals.size());
		for (std::size_t index = 0; index < shadow.stableOrdinals.size(); ++index)
		{
			RenderItemRecord item;
			if (!ResolveShadowDrawListItem(publication, presentation.referenceFrame, shadow, index, item))
			{
				// Failed optional preparation cannot reject an otherwise valid view.
				// Absence selects the retained singleton path, whose ordinary resolver
				// still reports any genuinely unsupported source at BackendCommit.
				return nullptr;
			}
			RenderInstanceRecord record;
			record.modelMatrix = item.modelMatrix; record.editorTint = item.editorTint;
			record.objectIdentity = glm::uvec4(item.conventionalVisibilityIdentityIndex,
				static_cast<std::uint32_t>(item.geometryRoute), item.virtualGeometryFlags,
				RenderInstanceRecord::PackCompatibilityPolicy(false, static_cast<std::uint32_t>(item.animationBinding.source),
					item.animationBinding.RequiresConservativeCoarseVisibility()));
			record.materialAndTexture = glm::uvec4(item.materialIndex, item.textureIndex,
				shadow.stableOrdinals[index], item.conventionalVisibilityIdentityIndex);
			// These copied immutable selectors are lowered to accepted native bridge
			// selectors only at BackendCommit, after real uploads have succeeded.
			record.animationSelectors = glm::uvec4(item.animation.firstGpuWorkPacket,
				static_cast<std::uint32_t>(item.animation.animationPlanGeneration),
				static_cast<std::uint32_t>(item.animation.rendererGeneration),
				static_cast<std::uint32_t>(item.animation.programGeneration));
			record.geometrySelectors = glm::uvec4(item.selectedVirtualLod, item.virtualGeometryPacketDescriptorIndex,
				item.virtualMeshletOffset, item.virtualMeshletCount);
			record.virtualGeometrySelectors = glm::uvec4(item.selectedVirtualClusterOffset, item.selectedVirtualClusterCount,
				item.selectedVirtualPageNeedOffset, item.selectedVirtualPageNeedCount);
			record.virtualGeometryPayloadSelectors = glm::uvec4(static_cast<std::uint32_t>(item.virtualGeometryPayloadSource),
				static_cast<std::uint32_t>(item.virtualGeometryEmittedSource),
				static_cast<std::uint32_t>(item.virtualGeometryPagePayloadByteOffset),
				static_cast<std::uint32_t>(item.virtualGeometryPagePayloadByteSize));
			inputs->records.push_back(record);
		}
		inputs->complete = true; return inputs;
	}
	bool ShadowPreparedInstanceInputsCurrent(const RenderFramePublication& publication,
		const RenderViewPresentationInput& presentation, const ShadowDrawList& shadow) noexcept
	{
		const auto& owner = shadow.instanceInputs;
		return owner && owner->complete && publication.frameId && publication.rendererSessionGeneration
			&& shadow.lightId != TerraEngine::EMPTY_UUID && presentation.referenceFrame.IsValid() && shadow.HasParallelLengths()
			&& owner->records.size() == shadow.stableOrdinals.size() && owner->lightId == shadow.lightId
			&& owner->frameSerial == publication.frameId && owner->sessionGeneration == publication.rendererSessionGeneration
			&& owner->topologyGeneration == shadow.topologyGeneration && publication.persistent
			&& owner->topologyGeneration == publication.persistent->generation
			&& owner->referenceFrame.frameId == presentation.referenceFrame.frameId
			&& owner->referenceFrame.MatchesGeneration(presentation.referenceFrame)
			&& owner->referenceFrame.canonicalOrigin == presentation.referenceFrame.canonicalOrigin
			&& owner->viewType == presentation.output.diagnosticsView && owner->outputId == presentation.output.outputId;
	}

	std::shared_ptr<RenderDynamicPage> RenderDynamicPageRecyclePool::Acquire(
		std::size_t maximumRetainedPages)
	{
		std::lock_guard<std::mutex> lock(mutex);
		// The pool itself keeps one mutable owner. Any additional owner is a
		// current/retired immutable publication or a worker that already acquired
		// this page, so that candidate cannot be touched yet.
		for(const std::shared_ptr<RenderDynamicPage>& candidate : retainedPages)
		{
			if(candidate != nullptr && candidate.use_count() == 1u)
			{
				return candidate;
			}
		}

		auto page = std::make_shared<RenderDynamicPage>();
		// Keep the pool bounded if a backend temporarily retains more frames than
		// usual. The returned page remains fully valid; it simply becomes an
		// ordinary one-shot allocation after its publication retires.
		if(retainedPages.size() < maximumRetainedPages)
		{
			retainedPages.push_back(page);
		}
		return page;
	}

	void RenderDynamicPageRecyclePool::Clear() noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		retainedPages.clear();
	}

	std::shared_ptr<const RenderFramePublication> RenderFramePublicationStore::Load() const noexcept
	{
		return root.load(std::memory_order_acquire);
	}

	SealedRenderFramePublication SealRenderFramePublication(
		std::shared_ptr<const RenderFramePublication> candidate,
		std::uint64_t expectedSessionGeneration,
		const RenderPublicationFailureInjection& injection) noexcept
	{
		SealedRenderFramePublication sealed;
		sealed.sessionGeneration_ = expectedSessionGeneration;
		if(candidate == nullptr) {
			sealed.receipt.outcome = RenderPublicationOutcome::RejectedStaleGeneration;
			sealed.receipt.diagnostic = "A null publication candidate cannot be sealed.";
			return sealed;
		}
		// The caller transfers its only mutable reference before validation.  Even a
		// rejected candidate therefore remains diagnostic-only and cannot be altered
		// then presented as a previously sealed root.
		sealed.candidate_ = std::move(candidate);
		sealed.receipt = ValidateRenderFramePublication(*sealed.candidate_,
			expectedSessionGeneration, injection);
		return sealed;
	}

	RenderPublicationReceipt RenderFramePublicationStore::TryPublish(
		const SealedRenderFramePublication& candidate,
		std::uint64_t expectedSessionGeneration) noexcept
	{
		if(!candidate.IsValid() || candidate.sessionGeneration_ != expectedSessionGeneration) {
			RenderPublicationReceipt receipt = candidate.Receipt();
			if(receipt.IsSuccess()) {
				receipt.outcome = RenderPublicationOutcome::RejectedSession;
				receipt.diagnostic = "The sealed candidate does not belong to this renderer session.";
			}
			return receipt;
		}
		std::lock_guard<std::mutex> lock(publicationMutex);
		RenderPublicationReceipt receipt = candidate.Receipt();
		std::shared_ptr<const RenderFramePublication> previous = Load();
		const std::uint64_t observed = previous ? previous->generation : 0u;
		receipt.observedPreviousGeneration = observed;
		if(expectedSessionGeneration != SessionGeneration()
			|| candidate.candidate_->expectedPreviousGeneration != observed)
		{
			receipt.outcome = RenderPublicationOutcome::RejectedStaleGeneration;
			receipt.diagnostic = "The protected root changed before candidate exchange.";
			return receipt;
		}
		std::shared_ptr<const RenderFramePublication> expected = previous;
		if(!root.compare_exchange_strong(expected, candidate.candidate_,
			std::memory_order_release, std::memory_order_acquire))
		{
			receipt.outcome = RenderPublicationOutcome::RejectedStaleGeneration;
			receipt.observedPreviousGeneration = expected ? expected->generation : 0u;
			receipt.diagnostic = "Another protected publication won the root exchange.";
		}
		return receipt;
	}

	void RenderFramePublicationStore::ResetForSession(std::uint64_t nextSessionGeneration) noexcept
	{
		std::lock_guard<std::mutex> lock(publicationMutex);
		// Exchanging null first prevents an old backend packet retaining a root from
		// being mistaken for the new renderer session during recovery.
		root.store({}, std::memory_order_release);
		sessionGeneration.store(nextSessionGeneration, std::memory_order_release);
	}

	std::uint64_t RenderFramePublicationStore::SessionGeneration() const noexcept
	{
		return sessionGeneration.load(std::memory_order_acquire);
	}
}
