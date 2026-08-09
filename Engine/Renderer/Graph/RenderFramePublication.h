// RenderFramePublication.h - Sealed, backend-neutral renderer frame hand-off.
//
// A publication owns the immutable generation tuple consumed by every render
// view.  Worker jobs may build the pages and selections below, but only the
// protected renderer boundary is allowed to exchange the root in
// RenderFramePublicationStore.

#pragma once

#include "Renderer/RenderTypes.h"
#include "Renderer/VirtualGeometry/Selection/RenderViewSelectionData.h"
#include "Renderer/Backend/RenderBackendWork.h"
#include "Renderer/Deformation/GpuDeformationPlan.h"
#include "Core/Export.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer
{
	inline constexpr std::size_t kRenderPublicationPageCapacity = 256u;
	// Only backend compute/deformation ownership crosses the sealed publication
	// boundary. Render-graph diagnostics, pass strings, and validation scratch stay
	// with worker preparation instead of being copied into every visible frame.
	struct RenderBackendWorkPublication
	{
		std::uint64_t frameId = 0u;
		AnimationBackendWorkPlan animationWork{};
		std::shared_ptr<const GpuDeformationPlan> deformationWork;
		bool valid = false;
	};
	// A stable ordinal is never sufficient on its own: a released slot can be
	// reused by another object, so consumers must carry the lifetime generation.
	struct RenderSlotGenerationKey
	{
		static constexpr std::uint32_t kInvalidOrdinal =
			std::numeric_limits<std::uint32_t>::max();

		std::uint32_t ordinal = kInvalidOrdinal;
		std::uint64_t lifetimeGeneration = 0u;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return ordinal != kInvalidOrdinal && lifetimeGeneration != 0u;
		}

		auto operator<=>(const RenderSlotGenerationKey&) const = default;
	};

	// Persistent identities deliberately omit matrices, pose data, and view LOD.
	// That lets static crowds retain one topology root across many frame roots.
	struct RenderPersistentSlot
	{
		RenderSlotGenerationKey key{};
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		std::uint64_t meshGeneration = 0u;
		std::uint64_t materialGeneration = 0u;
		std::uint64_t resourceGeneration = 0u;
		std::uint64_t packetTemplateIdentity = 0u;
		std::uint32_t routeTemplate = 0u;
		bool occupied = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return occupied && key.IsValid() && objectId != TerraEngine::EMPTY_UUID
				&& meshId != TerraEngine::EMPTY_UUID
				&& materialId != TerraEngine::EMPTY_UUID
				&& meshGeneration != 0u && materialGeneration != 0u
				&& resourceGeneration != 0u && packetTemplateIdentity != 0u;
		}
	};

	// Pages are shared by immutable roots.  A changed lane replaces its page;
	// unmodified pages are retained without a crowd-wide copy.
	struct RenderPersistentPage
	{
		std::uint64_t generation = 0u;
		std::uint32_t firstOrdinal = 0u;
		std::vector<RenderPersistentSlot> slots;
		// The slot allocator sets this only after validating the complete immutable
		// page. Hand-authored/test pages leave zero and take the defensive seal scan.
		std::size_t producerValidatedSlotCount = 0u;
	};

	struct RenderDynamicLane
	{
		RenderSlotGenerationKey key{};
		RenderInstanceRecord instance{};
		// Motion-vector and temporal consumers need the transform that produced the
		// preceding sealed frame.  It stays in the dynamic lane because changing it
		// must copy only this page, never the stable mesh/material topology.
		glm::mat4 previousModelMatrix = glm::mat4(1.0f);
		// This compact record is the complete backend animation contract. Keeping it
		// beside the binding lets backend preparation consume the sealed dynamic root
		// without borrowing the worker-only full renderable collection.
		RenderItemRecord::PublishedAnimationGeneration animation{};
		// Animation presentation is frame-dynamic but view-independent. Keeping one
		// immutable binding here prevents every camera from retaining the same pose,
		// palette, and fallback ownership again in its compact selector lanes.
		PublishedAnimationRenderBinding animationBinding{};
		std::uint64_t transformGeneration = 0u;
		std::uint64_t animationPresentationGeneration = 0u;
		std::uint64_t materialOverrideGeneration = 0u;
		std::uint64_t boundsGeneration = 0u;
		std::uint64_t lifecycleGeneration = 0u;
		// Resource reloads can preserve object transforms while replacing mesh,
		// material, or texture payloads. Retain their generation in the dynamic
		// lane so COW pages cannot silently reuse pre-reload draw selectors.
		std::uint64_t resourceGeneration = 0u;
		bool occupied = false;

		[[nodiscard]] bool Matches(const RenderPersistentSlot& persistent) const noexcept
		{
			return occupied && key == persistent.key;
		}
	};

	struct RenderDynamicPage
	{
		std::uint64_t generation = 0u;
		std::uint32_t firstOrdinal = 0u;
		// changedLanes mirrors lanes and records exactly what the producer replaced.
		std::vector<bool> changedLanes;
		std::vector<RenderDynamicLane> lanes;
		// Dynamic-page workers validate every lane against this exact persistent-page
		// generation before publishing the immutable value. These proofs remove the
		// duplicate serial crowd scan while untrusted/test inputs still validate fully.
		std::size_t producerValidatedLaneCount = 0u;
		std::uint64_t producerValidatedPersistentGeneration = 0u;
	};

	struct RenderPersistentTopologyRoot
	{
		std::uint64_t generation = 0u;
		std::uint64_t rendererSessionGeneration = 0u;
		std::vector<std::shared_ptr<const RenderPersistentPage>> pages;
	};

	struct RenderDynamicStateRoot
	{
		std::uint64_t generation = 0u;
		std::uint64_t rendererSessionGeneration = 0u;
		std::vector<std::shared_ptr<const RenderDynamicPage>> pages;
	};

	// Dynamic pages are immutable while a sealed publication or the backend owns
	// them. This bounded recycler retains only allocation storage whose page has
	// no other owner, so the next worker can rebuild its lanes without growing
	// the heap during a stable animated crowd. A page is never reset while a
	// shared immutable publication can still observe it.
	class TERRAENGINE_API RenderDynamicPageRecyclePool
	{
	public:
		[[nodiscard]] std::shared_ptr<RenderDynamicPage> Acquire(
			std::size_t maximumRetainedPages);
		void Clear() noexcept;

	private:
		mutable std::mutex mutex;
		std::vector<std::shared_ptr<RenderDynamicPage>> retainedPages;
	};

	// Allocation keeps persistent identities separate from per-frame dynamic data.
	// The caller supplies every stable input required to build a complete slot, so
	// a newly allocated key is immediately safe for a later compact selection.
	struct RenderSlotAllocationRequest
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID materialId = TerraEngine::EMPTY_UUID;
		std::uint64_t meshGeneration = 0u;
		std::uint64_t materialGeneration = 0u;
		std::uint64_t resourceGeneration = 0u;
		std::uint64_t packetTemplateIdentity = 0u;
		std::uint32_t routeTemplate = 0u;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return objectId != TerraEngine::EMPTY_UUID
				&& meshId != TerraEngine::EMPTY_UUID
				&& materialId != TerraEngine::EMPTY_UUID
				&& meshGeneration != 0u && materialGeneration != 0u
				&& resourceGeneration != 0u && packetTemplateIdentity != 0u;
		}
	};

	// This value owns allocator bookkeeping alongside the immutable topology root.
	// Transactions copy the small free-list and generation vectors, then copy only
	// changed topology pages; abandoning a transaction therefore leaves this state
	// and all readers of its root untouched.
	struct RenderSlotAllocatorState
	{
		std::uint64_t rendererSessionGeneration = 0u;
		std::uint64_t topologyGeneration = 0u;
		std::uint32_t nextOrdinal = 0u;
		std::vector<std::uint64_t> lifetimeGenerations;
		std::vector<std::uint32_t> freeOrdinals;
		std::shared_ptr<const RenderPersistentTopologyRoot> topology;
	};

	class TERRAENGINE_API RenderSlotAllocatorTransaction
	{
	public:
		[[nodiscard]] bool IsActive() const noexcept { return active_; }
		[[nodiscard]] std::optional<RenderSlotGenerationKey> Allocate(
			const RenderSlotAllocationRequest& request);
		[[nodiscard]] bool Release(RenderSlotGenerationKey key);
		// Commit returns a replacement value rather than mutating its source.  The
		// protected renderer boundary decides whether to install that result.
		[[nodiscard]] std::optional<RenderSlotAllocatorState> Commit() const;
		void Rollback() noexcept { active_ = false; }

	private:
		friend TERRAENGINE_API RenderSlotAllocatorTransaction
			BeginRenderSlotAllocatorTransaction(const RenderSlotAllocatorState& state,
				std::uint64_t nextTopologyGeneration);
		[[nodiscard]] bool EnsureMutableTopology();
		[[nodiscard]] bool ReplaceSlot(std::uint32_t ordinal,
			const RenderPersistentSlot& replacement);

		RenderSlotAllocatorState working_{};
		std::uint64_t nextTopologyGeneration_ = 0u;
		std::shared_ptr<RenderPersistentTopologyRoot> mutableTopology_;
		bool active_ = false;
		bool changed_ = false;
	};

	[[nodiscard]] TERRAENGINE_API RenderSlotAllocatorState
		CreateRenderSlotAllocatorState(std::uint64_t rendererSessionGeneration,
			std::uint64_t topologyGeneration);
	[[nodiscard]] TERRAENGINE_API RenderSlotAllocatorTransaction
		BeginRenderSlotAllocatorTransaction(const RenderSlotAllocatorState& state,
			std::uint64_t nextTopologyGeneration);

	// Resource preparation is a compact, backend-neutral table shared by every
	// view in a publication.  It carries identity and readiness only; descriptor
	// handles, upload objects, and command buffers remain owned by the backend.
	struct RenderResourcePreparationRecord
	{
		TerraEngine::UUID resourceId = TerraEngine::EMPTY_UUID;
		std::uint64_t resourceGeneration = 0u;
		std::uint32_t preparationIndex = 0u;
		bool resident = false;
		bool fallbackAllowed = true;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return resourceId != TerraEngine::EMPTY_UUID
				&& resourceGeneration != 0u;
		}
	};

	struct RenderResourcePreparationTable
	{
		std::uint64_t generation = 0u;
		std::vector<RenderResourcePreparationRecord> records;

		[[nodiscard]] const RenderResourcePreparationRecord* Find(
			TerraEngine::UUID resourceId) const noexcept;
	};

	// A prepared span addresses compact ordinal/resource tables by offset and
	// count.  Singleton ranges use count==1 and never need a copied command.
	struct RenderPreparedSpan
	{
		std::uint32_t first = 0u;
		std::uint32_t count = 0u;

		[[nodiscard]] bool IsComplete() const noexcept { return count != 0u; }
	};

	// A stable view owns these parallel arrays once. Publications may retain this
	// immutable payload across frames while their scalar frame/output identity keeps
	// advancing independently.
	struct RenderViewSelectionPayload
	{
		std::vector<RenderSlotGenerationKey> visibleOrdinals;
		std::vector<std::uint32_t> selectedLods;
		std::vector<std::uint32_t> routeSelectors;
		std::vector<std::uint64_t> packetTemplateIdentities;
		std::vector<std::uint32_t> tintSelectors;
		std::vector<glm::vec4> editorTints;

		[[nodiscard]] bool HasParallelLengths() const noexcept
		{
			const std::size_t length = visibleOrdinals.size();
			return selectedLods.size() == length && routeSelectors.size() == length
				&& packetTemplateIdentities.size() == length
				&& tintSelectors.size() == length && editorTints.size() == length;
		}
	};

	// Every selector is indexed by VisibleOrdinals(). Seal-time validation resolves
	// each key against both immutable page roots, so a recycled fixed ordinal cannot
	// turn into a partial backend submission. The explicit layout avoids rebuilding
	// one complete mesh/material command per view.
	struct RenderViewSelection
	{
		RenderOutputKind outputKind = RenderOutputKind::GameView;
		RenderViewType viewType = RenderViewType::Game;
		std::uint32_t outputId = 0u;
		std::uint64_t generation = 0u;
		std::uint64_t lightIdentity = 0u;
		std::vector<RenderSlotGenerationKey> visibleOrdinals;
		std::vector<std::uint32_t> selectedLods;
		std::vector<std::uint32_t> routeSelectors;
		std::vector<std::uint64_t> packetTemplateIdentities;
		std::vector<std::uint32_t> tintSelectors;
		std::vector<glm::vec4> editorTints;
		std::shared_ptr<const RenderViewSelectionPayload> retainedPayload;

		[[nodiscard]] const std::vector<RenderSlotGenerationKey>&
			VisibleOrdinals() const noexcept
		{
			return retainedPayload ? retainedPayload->visibleOrdinals : visibleOrdinals;
		}
		[[nodiscard]] const std::vector<std::uint32_t>& SelectedLods() const noexcept
		{
			return retainedPayload ? retainedPayload->selectedLods : selectedLods;
		}
		[[nodiscard]] const std::vector<std::uint32_t>& RouteSelectors() const noexcept
		{
			return retainedPayload ? retainedPayload->routeSelectors : routeSelectors;
		}
		[[nodiscard]] const std::vector<std::uint64_t>&
			PacketTemplateIdentities() const noexcept
		{
			return retainedPayload
				? retainedPayload->packetTemplateIdentities : packetTemplateIdentities;
		}
		[[nodiscard]] const std::vector<std::uint32_t>& TintSelectors() const noexcept
		{
			return retainedPayload ? retainedPayload->tintSelectors : tintSelectors;
		}
		[[nodiscard]] const std::vector<glm::vec4>& EditorTints() const noexcept
		{
			return retainedPayload ? retainedPayload->editorTints : editorTints;
		}

		[[nodiscard]] bool HasParallelLengths() const noexcept
		{
			if(retainedPayload) return retainedPayload->HasParallelLengths();
			const std::size_t length = visibleOrdinals.size();
			return selectedLods.size() == length && routeSelectors.size() == length
				&& packetTemplateIdentities.size() == length
				&& tintSelectors.size() == length && editorTints.size() == length;
		}
	};

	// These immutable per-view values replace the non-command portion of the old
	// frame payload.  Keeping them beside the compact ordinal selection gives
	// Vulkan all presentation inputs without re-reading camera, UI, lighting, or
	// virtual-geometry state from live editor and scene objects.
	struct RenderViewPresentationInput
	{
		RenderOutputRequest output{};
		glm::mat4 viewMatrix = glm::mat4(1.0f);
		glm::mat4 projectionMatrix = glm::mat4(1.0f);
		glm::vec3 cameraWorldPosition = glm::vec3(0.0f);
		float deltaTimeSeconds = 0.0f;
		bool globalTaskShaderCullingEnabled = true;
		float cullingEpsilon = 0.001f;
		QualityPreset activePreset = QualityPreset::High;
		bool customOverrideActive = false;
		RenderSettings settings{};
		RenderOptimizationSummary renderOptimization{};
		// One compact lane is retained for each prepared command. The parallel map
		// names its visible selection, or kInvalidOrdinal when diagnostics/grouping
		// retained a non-visible source command. This preserves stable group indices
		// without copying full command records into the view publication.
		std::vector<RenderViewCommandLane> commandLanes;
		// Stable cache hits retain the immutable lane array instead of copying it into
		// every frame publication. The shared owner keeps the exact revision-validated
		// cut alive even when its bounded cache entry is retired on a later frame.
		std::shared_ptr<const std::vector<RenderViewCommandLane>> retainedCommandLanes;
		std::vector<std::uint32_t> commandSelectionIndices;
		std::shared_ptr<const std::vector<std::uint32_t>>
			retainedCommandSelectionIndices;
		std::vector<RenderInstanceRecord> groupedInstanceRecords;
		std::vector<GroupedRenderSubmission> groupedSubmissions;
		std::vector<std::uint32_t> drawResourcePreparationCommandIndices;
		FrameSkyData sky{};
		std::vector<FrameLightCandidate> lightCandidates;
		glm::vec4 editorContextTint = glm::vec4(0.0f);
		bool sceneViewGridVisible = true;
		bool sceneViewOrthographic = false;
		std::vector<ShadowDrawList> shadowDrawLists;
		UIRenderBatch uiBatch{};
		std::uint64_t virtualGeometryPacketEvidenceSignature = 0u;
		std::uint64_t virtualGeometryVisibilitySetupSignature = 0u;
		std::vector<GpuVirtualGeometryPacketRecord> virtualGeometryPackets;
		std::vector<GpuVirtualGeometryBatchDescriptor> virtualGeometryBatches;
		std::vector<GpuVirtualGeometryPacketFrameSet> virtualGeometryFrameSets;
		bool virtualGeometryVisibilityDiagnosticsRequested = false;
		std::vector<GpuPacketCompactionDispatch> virtualGeometryCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord>
			virtualGeometryGeneratedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> virtualGeometryCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> virtualGeometryFallbackRecords;
		std::shared_ptr<const VirtualGeometryRetainedGpuEvidence>
			retainedVirtualGeometryGpuEvidence;
		std::vector<VirtualGeometryPageRetirementRecord> virtualGeometryPageRetirements;
		std::vector<VirtualGeometryDecodedPageUploadRecord> virtualGeometryDecodedPageUploads;

		[[nodiscard]] const std::vector<RenderViewCommandLane>&
			CommandLanes() const noexcept
		{
			return retainedCommandLanes ? *retainedCommandLanes : commandLanes;
		}
		[[nodiscard]] const std::vector<std::uint32_t>&
			CommandSelectionIndices() const noexcept
		{
			return retainedCommandSelectionIndices
				? *retainedCommandSelectionIndices : commandSelectionIndices;
		}

		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketRecord>&
			VirtualGeometryPackets() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->packetRecords
				: virtualGeometryPackets;
		}

		[[nodiscard]] const std::vector<GpuVirtualGeometryBatchDescriptor>&
			VirtualGeometryBatches() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->batchDescriptors
				: virtualGeometryBatches;
		}

		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketFrameSet>&
			VirtualGeometryFrameSets() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->packetFrameSets
				: virtualGeometryFrameSets;
		}

		[[nodiscard]] const std::vector<GpuPacketCompactionDispatch>&
			VirtualGeometryCompactionDispatches() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->compactionDispatches
				: virtualGeometryCompactionDispatches;
		}

		[[nodiscard]] const std::vector<GeneratedVirtualGeometrySubmissionRecord>&
			VirtualGeometryGeneratedSubmissions() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->generatedSubmissions
				: virtualGeometryGeneratedSubmissions;
		}

		[[nodiscard]] const std::vector<VirtualGeometryCounterReadbackRecord>&
			VirtualGeometryCounterReadbacks() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->counterReadbacks
				: virtualGeometryCounterReadbacks;
		}

		[[nodiscard]] const std::vector<VirtualGeometryGpuFallbackRecord>&
			VirtualGeometryFallbackRecords() const noexcept
		{
			return retainedVirtualGeometryGpuEvidence
				? retainedVirtualGeometryGpuEvidence->fallbackRecords
				: virtualGeometryFallbackRecords;
		}

		[[nodiscard]] bool Matches(const RenderViewSelection& selection) const noexcept
		{
			return output.outputId == selection.outputId
				&& output.kind == selection.outputKind
				&& output.diagnosticsView == selection.viewType;
		}
	};

	enum class RenderSubmissionRoute : std::uint8_t
	{
		Direct,
		Skinned,
		VirtualGeometry,
		Shadow,
		Picking,
		Capture,
		Visualizer
	};

	// Grouped and singleton work share this type.  A singleton is simply a range
	// of one, so unsupported combinations never require a legacy fallback list.
	struct RenderSubmissionRange
	{
		RenderSubmissionRoute route = RenderSubmissionRoute::Direct;
		std::uint32_t viewIndex = 0u;
		std::uint32_t firstSelection = 0u;
		std::uint32_t selectionCount = 0u;
		std::uint64_t packetTemplateIdentity = 0u;
		bool singleton = false;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return selectionCount != 0u && packetTemplateIdentity != 0u
				&& (!singleton || selectionCount == 1u);
		}
	};

	enum class RenderPublicationOutcome : std::uint8_t
	{
		Published,
		Reused,
		RejectedAllocation,
		RejectedCancellation,
		RejectedStaleGeneration,
		RejectedDuplicateSlot,
		RejectedSelectorLength,
		RejectedRange,
		RejectedBackendPreparation,
		RejectedSession
	};

	struct RenderPublicationReceipt
	{
		RenderPublicationOutcome outcome = RenderPublicationOutcome::RejectedStaleGeneration;
		std::uint64_t publicationGeneration = 0u;
		std::uint64_t expectedPreviousGeneration = 0u;
		std::uint64_t observedPreviousGeneration = 0u;
		std::uint32_t reusedPersistentPages = 0u;
		std::uint32_t changedDynamicPages = 0u;
		std::uint32_t viewCount = 0u;
		std::uint32_t rangeCount = 0u;
		std::string diagnostic;

		[[nodiscard]] bool IsSuccess() const noexcept
		{
			return outcome == RenderPublicationOutcome::Published
				|| outcome == RenderPublicationOutcome::Reused;
		}
	};

	struct RenderFramePublication
	{
		std::uint64_t frameId = 0u;
		std::uint64_t rendererSessionGeneration = 0u;
		// Presentation recovery can replace swapchain-compatible Vulkan objects
		// without starting a new renderer session. This monotonically increasing
		// identity prevents an already sealed root from binding retired objects.
		std::uint64_t backendResourceGeneration = 1u;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t animationPresentationGeneration = 0u;
		std::uint64_t expectedPreviousGeneration = 0u;
		std::uint64_t generation = 0u;
		std::shared_ptr<const RenderPersistentTopologyRoot> persistent;
		std::shared_ptr<const RenderDynamicStateRoot> dynamic;
		std::vector<RenderViewSelection> views;
		std::vector<RenderViewPresentationInput> presentationInputs;
		std::vector<RenderSubmissionRange> submissionRanges;
		// Shared compact tables are prepared once and referenced by typed ranges;
		// no per-view full command/materialization arrays are retained here.
		RenderResourcePreparationTable resourcePreparation;
		std::vector<std::uint32_t> selectedOrdinals;
		// Compute and deformation plans are sealed with the same root as the views.
		// The backend consumes this owner once at the protected frame boundary.
		std::shared_ptr<const RenderBackendWorkPublication> backendWork;
		RenderPublicationReceipt receipt{};
	};

	// A resolver borrows only sealed publication values.  Resolve writes a caller-
	// owned value, so Vulkan cannot retain a pointer into mutable renderer state.
	class RenderViewCommandResolver
	{
	public:
		[[nodiscard]] std::size_t Count() const noexcept;
		[[nodiscard]] bool Resolve(std::size_t selectionIndex,
			RenderItemRecord& output) const noexcept;

	private:
		friend TERRAENGINE_API RenderViewCommandResolver MakeRenderViewCommandResolver(
			const RenderFramePublication& publication, std::size_t viewIndex) noexcept;
		const RenderFramePublication* publication_ = nullptr;
		const RenderViewSelection* selection_ = nullptr;
		const RenderViewPresentationInput* presentation_ = nullptr;
	};

	[[nodiscard]] TERRAENGINE_API RenderViewCommandResolver MakeRenderViewCommandResolver(
		const RenderFramePublication& publication, std::size_t viewIndex) noexcept;

	[[nodiscard]] TERRAENGINE_API RenderResourcePreparationTable
		BuildRenderResourcePreparationTable(
			std::span<const RenderResourcePreparationRecord> records,
			std::uint64_t generation);

	// Convert one compact view selection into the fewest contiguous backend ranges.
	// Adjacent items can share a range only when both their route and immutable
	// packet-template identity match. The function never reorders selections, so
	// picking, diagnostics, and stable ordinal ownership retain their exact indices.
	[[nodiscard]] TERRAENGINE_API std::vector<RenderSubmissionRange>
		BuildRenderSubmissionRanges(const RenderViewSelection& selection,
			std::uint32_t viewIndex);

	// Shadow selections are nested beneath a camera presentation because their
	// render target is owned by that camera pass. Resolve one ordinal directly
	// from the same sealed roots without retaining a second full command array.
	[[nodiscard]] TERRAENGINE_API bool ResolveShadowDrawListItem(
		const RenderFramePublication& publication,
		const ShadowDrawList& shadow,
		std::size_t itemIndex,
		RenderItemRecord& output) noexcept;

	// This test-only input allows deterministic validation of rejection rollback.
	// It is never a renderer setting and production callers leave it defaulted.
	struct RenderPublicationFailureInjection
	{
		bool failAllocation = false;
		bool cancelled = false;
		bool backendPreparationFailed = false;
	};

	[[nodiscard]] TERRAENGINE_API RenderPublicationReceipt ValidateRenderFramePublication(
		const RenderFramePublication& publication,
		std::uint64_t expectedSessionGeneration,
		const RenderPublicationFailureInjection& injection = {}) noexcept;

	// Only SealRenderFramePublication can create a publishable token.  This keeps
	// crowd-sized validation and deterministic failure injection outside the
	// protected root-exchange mutex.
	class SealedRenderFramePublication
	{
	public:
		[[nodiscard]] bool IsValid() const noexcept
		{
			return candidate_ != nullptr && receipt.IsSuccess();
		}
		[[nodiscard]] RenderPublicationReceipt Receipt() const noexcept { return receipt; }
		[[nodiscard]] const std::shared_ptr<const RenderFramePublication>& Publication() const noexcept
		{
			return candidate_;
		}

	private:
		friend TERRAENGINE_API SealedRenderFramePublication SealRenderFramePublication(
			std::shared_ptr<const RenderFramePublication> candidate,
			std::uint64_t expectedSessionGeneration,
			const RenderPublicationFailureInjection& injection) noexcept;
		friend class RenderFramePublicationStore;
		std::shared_ptr<const RenderFramePublication> candidate_;
		RenderPublicationReceipt receipt{};
		std::uint64_t sessionGeneration_ = 0u;
	};

	[[nodiscard]] TERRAENGINE_API SealedRenderFramePublication SealRenderFramePublication(
		std::shared_ptr<const RenderFramePublication> candidate,
		std::uint64_t expectedSessionGeneration,
		const RenderPublicationFailureInjection& injection = {}) noexcept;

	// The store owns the one protected root exchange.  Workers receive neither a
	// mutable root nor the ability to publish partial per-view state.
	class TERRAENGINE_API RenderFramePublicationStore
	{
	public:
		[[nodiscard]] std::shared_ptr<const RenderFramePublication> Load() const noexcept;
		[[nodiscard]] RenderPublicationReceipt TryPublish(
			const SealedRenderFramePublication& candidate,
			std::uint64_t expectedSessionGeneration) noexcept;
		void ResetForSession(std::uint64_t nextSessionGeneration) noexcept;
		[[nodiscard]] std::uint64_t SessionGeneration() const noexcept;

	private:
		std::atomic<std::shared_ptr<const RenderFramePublication>> root{};
		std::atomic<std::uint64_t> sessionGeneration{ 1u };
		// Reset and root exchange share this narrow protected boundary.  Readers use
		// the atomic root and never take this lock while rendering a sealed frame.
		mutable std::mutex publicationMutex;
	};
}
