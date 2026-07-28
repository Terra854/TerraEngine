// ResourceResidency.h - loaded resource residency policy and diagnostics.
//
// This module describes when a loaded engine resource may leave CPU/GPU
// memory. It intentionally does not manage persistent disk caches; callers use
// these data structures to reason about live runtime payloads only.

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

#include "Resource/ResourceStreaming.h"
#include "Core/Export.h"
#include "Utilities/UUID.h"

// Keep the forward declaration's storage width aligned with ResourceManager.
// Physics appended IDs are serialized through the same stable enum contract.
enum class AssetType : std::uint32_t;

namespace TerraEngine::Resource
{
	enum class ResidencyAssetType : std::uint8_t
	{
		Mesh,
		Texture,
		Material,
		Shader,
		ShaderCollection,
		FontAtlas,
		Other,
		// Keep the original 0-6 identities unchanged. The animation residency
		// buckets are distinct so later budgets and diagnostics can account for
		// every dependency type without guessing from a filename.
		Skeleton = 7U,
		AnimationClip = 8U,
		AnimationController = 9U,
		AnimationMask = 10U,
		RetargetProfile = 11U,
		MotionDatabase = 12U,
		RagdollProfile = 13U
	};

	enum class ResidencyState : std::uint8_t
	{
		Loading,
		Active,
		Idle,
		Pinned,
		PendingRelease,
		Released,
		Failed
	};

	enum class ResidencyPinReason : std::uint8_t
	{
		None,
		DefaultResource,
		FallbackResource,
		EditorSelection,
		Preview,
		ActiveScene,
		PrefabEditing,
		ExportStartup,
		Loading,
		BackendInFlight,
		PolicyProtected
	};

	enum class ResidencyReleaseReason : std::uint8_t
	{
		None,
		IdleExpiry,
		MemoryPressure,
		SceneUnload,
		ExplicitRuntimeUnload,
		Disabled
	};

	enum class ResidencyReleaseResult : std::uint8_t
	{
		Released,
		Deferred,
		Skipped,
		Failed
	};

	enum class ResidencyBlockedReason : std::uint8_t
	{
		None,
		Active,
		Pinned,
		Loading,
		Failed,
		NotReloadable,
		InFlight,
		RecentlyUsed
	};

	enum class ResidencyReleaseDomain : std::uint8_t
	{
		Cpu,
		Gpu,
		CpuAndGpu
	};

	enum class VirtualGeometryPageResidencyState : std::uint8_t
	{
		NotResident,
		Requested,
		Loading,
		Resident,
		Failed,
		Evicting,
		Evicted,
		PendingIO,
		PendingUpload,
		Pinned,
		PendingRetirement,
		Retired
	};

	enum class VirtualGeometryPageResidencyEvent : std::uint8_t
	{
		Request,
		BeginLoad,
		BeginIO,
		BeginUpload,
		MarkResident,
		MarkFailed,
		BeginEvict,
		MarkEvicted,
		PinFallback,
		UnpinFallback,
		BeginRetire,
		MarkRetired
	};

	enum class CpuPayloadOwnershipKind : std::uint8_t
	{
		Unknown,
		DecodedMesh,
		TexturePixels,
		MaterialMetadata,
		ShaderCollection,
		VirtualGeometryPage,
		VirtualGeometryFallbackPage,
		VirtualGeometryMetadata,
		// Animation payload kinds are append-only diagnostic identities. Keeping
		// each class distinct prevents typed cooked data from being reported as a
		// decoded render mesh merely because both live in CPU memory.
		Skeleton,
		AnimationClip,
		AnimationController,
		AnimationMask,
		RetargetProfile,
		MotionDatabase,
		RagdollProfile
	};

	enum class CpuPayloadOwnershipState : std::uint8_t
	{
		Loaded,
		Retained,
		PendingRelease,
		Released,
		Failed,
		Unavailable
	};

	enum class GpuAllocationOwnershipKind : std::uint8_t
	{
		Unknown,
		MeshBuffer,
		TextureImage,
		MaterialBuffer,
		ShaderCollection,
		VirtualGeometryPageBuffer,
		VirtualGeometryFallbackPageBuffer,
		VirtualGeometryMetadataBuffer,
		DiagnosticBuffer
	};

	enum class GpuAllocationOwnershipState : std::uint8_t
	{
		Published,
		InFlight,
		PendingRetirement,
		Retired,
		Unavailable
	};

	struct CpuPayloadOwnershipRecord
	{
		TerraEngine::UUID ownerAssetId = TerraEngine::EMPTY_UUID;
		CpuPayloadOwnershipKind payloadKind = CpuPayloadOwnershipKind::Unknown;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t byteSize = 0u;
		CpuPayloadOwnershipState state = CpuPayloadOwnershipState::Unavailable;
		std::uint64_t publishFrame = 0u;
		std::uint64_t releaseFrame = 0u;
		std::string detail;
	};

	struct GpuAllocationOwnershipRecord
	{
		TerraEngine::UUID ownerAssetId = TerraEngine::EMPTY_UUID;
		GpuAllocationOwnershipKind resourceKind = GpuAllocationOwnershipKind::Unknown;
		std::uint64_t allocationId = 0u;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t byteSize = 0u;
		GpuAllocationOwnershipState state = GpuAllocationOwnershipState::Unavailable;
		std::uint64_t firstVisibleFrame = 0u;
		std::uint64_t safeRetireFrame = 0u;
		std::string detail;
	};

	struct ExactResidencyUnavailableDiagnostic
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::string domain;
		std::string missingProducer;
		std::string reason;
	};

	struct ResidencyMemoryPolicy
	{
		double idleSecondsThreshold = 10.0;
		std::uint64_t memoryBudgetBytes = 0;
		double reloadGraceSeconds = 0.5;
	};

	struct GpuResidencyPolicy : ResidencyMemoryPolicy
	{
		bool strictBudgetMode = false;
	};

	struct ResidencyPolicy
	{
		bool enabled = true;
		double cleanupIntervalSeconds = 1.0;
		ResidencyMemoryPolicy cpu;
		GpuResidencyPolicy gpu;
		bool diagnosticsEnabled = false;
		std::unordered_set<std::string> protectedGroups;
	};

	struct ResidentAsset
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		ResidencyAssetType assetType = ResidencyAssetType::Other;
		std::string sourceKey;
		ResidencyState state = ResidencyState::Idle;
		std::uint64_t lastUsedFrame = 0;
		double lastUsedSeconds = 0.0;
		std::string lastTouchedReason;
		std::uint64_t estimatedCpuBytes = 0;
		std::uint64_t estimatedGpuBytes = 0;
		ExactResidencyByteValue exactCpuBytes;
		ExactResidencyByteValue exactGpuBytes;
		std::vector<CpuPayloadOwnershipRecord> cpuPayloads;
		std::vector<GpuAllocationOwnershipRecord> gpuAllocations;
		ResidencyPinReason pinReason = ResidencyPinReason::None;
		bool reloadable = true;
		bool inFlight = false;
		StreamingPriorityClass priorityClass = StreamingPriorityClass::Normal;
		StreamingResidencyTier residencyTier = StreamingResidencyTier::Streamed;
		std::uint64_t releaseGeneration = 0;
	};

	struct ResidencySnapshot
	{
		std::uint64_t frameId = 0;
		double nowSeconds = 0.0;
		std::vector<TerraEngine::UUID> activeAssetIds;
		std::vector<TerraEngine::UUID> pinnedAssetIds;
		std::vector<ResidentAsset> residentAssets;
		ResidencyPolicy policy;
	};

	struct ReleaseCandidate
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		ResidencyAssetType assetType = ResidencyAssetType::Other;
		std::string sourceKey;
		double idleSeconds = 0.0;
		std::uint64_t estimatedReclaimableBytes = 0;
		ResidencyReleaseReason releaseReason = ResidencyReleaseReason::None;
		ResidencyReleaseDomain releaseDomain = ResidencyReleaseDomain::CpuAndGpu;
		StreamingPriorityClass priorityClass = StreamingPriorityClass::Normal;
		StreamingResidencyTier residencyTier = StreamingResidencyTier::Streamed;
		ResidencyBlockedReason blockedReason = ResidencyBlockedReason::None;
	};

	struct ReleaseRecord
	{
		std::uint64_t frameId = 0;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		ResidencyAssetType assetType = ResidencyAssetType::Other;
		ResidencyReleaseReason releaseReason = ResidencyReleaseReason::None;
		ResidencyReleaseDomain releaseDomain = ResidencyReleaseDomain::CpuAndGpu;
		ResidencyReleaseResult result = ResidencyReleaseResult::Skipped;
		std::uint64_t bytesReclaimed = 0;
		std::string explanation;
	};

	struct ResidencyDiagnosticsSummary
	{
		std::uint64_t frameId = 0;
		std::size_t residentAssetCount = 0;
		std::size_t activeAssetCount = 0;
		std::size_t pinnedAssetCount = 0;
		std::size_t idleCandidateCount = 0;
		std::size_t pendingReleaseCount = 0;
		std::size_t releasedAssetCount = 0;
		std::uint64_t estimatedResidentBytes = 0;
		std::uint64_t estimatedReclaimedBytes = 0;
		ResidencyReleaseReason cleanupReason = ResidencyReleaseReason::None;
		bool diagnosticsEnabled = false;
		std::vector<ReleaseCandidate> candidates;
		std::vector<ReleaseRecord> records;
	};

	struct VirtualGeometryResidentPage
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0u;
		std::uint32_t fallbackPageId = 0u;
		VirtualGeometryPageResidencyState state = VirtualGeometryPageResidencyState::NotResident;
		std::uint64_t byteSize = 0u;
		std::int32_t priority = 0;
		std::uint64_t lastUsedFrame = 0u;
		std::uint64_t lastRequestedFrame = 0u;
		std::uint64_t lastResidentFrame = 0u;
		std::uint64_t lastStateChangeFrame = 0u;
		std::uint32_t requestCount = 0u;
		bool fallbackPage = false;
		bool pinnedFallback = false;
		float screenImportance = 0.0f;
		float predictedNeed = 0.0f;
		std::uint64_t payloadByteOffset = 0u;
		std::uint64_t payloadByteSize = 0u;
		std::uint64_t retirementFrame = 0u;
		bool protectedFallback = false;
		std::string requestReason;
		std::string failureReason;
	};

	struct VirtualGeometryPageTransitionResult
	{
		bool applied = false;
		VirtualGeometryPageResidencyState previousState = VirtualGeometryPageResidencyState::NotResident;
		VirtualGeometryPageResidencyState newState = VirtualGeometryPageResidencyState::NotResident;
		std::string reason;
	};

	struct AnimatedVirtualPageResidencyRow
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0xffffffffu;
		std::uint64_t contentGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t staticAllocationId = 0u;
		std::uint64_t byteSize = 0u;
		std::uint64_t completionFence = 0u;
		bool resident = false;
		bool pinnedFallback = false;
		bool eligibleForNewWork = false;

		auto operator<=>(const AnimatedVirtualPageResidencyRow&) const =
			default;
	};

	struct AnimatedVirtualPageResidencySnapshot
	{
		std::uint64_t publicationGeneration = 0u;
		std::uint64_t catalogGeneration = 0u;
		std::vector<AnimatedVirtualPageResidencyRow> pages;
		bool valid = false;
	};

	struct AnimatedVirtualPageResidencyPublication
	{
		AnimatedVirtualPageResidencySnapshot visible;
		bool accepted = false;
		std::string rejectionReason;
	};

	struct VirtualGeometryPageEvictionCandidate
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::int32_t priority = 0;
		std::uint64_t lastUsedFrame = 0u;
	};

	struct VirtualGeometryPageBudget
	{
		std::uint64_t memoryBudgetBytes = 0u;
		std::uint64_t uploadBudgetBytes = 0u;
		std::uint64_t retirementBudgetBytes = 0u;
	};

	struct VirtualGeometryPageBudgetPressureContributor
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint64_t residentHighDetailBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
		std::uint64_t retiringBytes = 0u;
		std::uint64_t pendingIoBytes = 0u;
		std::uint64_t pendingUploadBytes = 0u;
		std::uint64_t failedBytes = 0u;
		std::uint64_t evictingBytes = 0u;
		std::uint64_t evictedBytes = 0u;
		std::uint32_t residentPages = 0u;
		std::uint32_t requestedPages = 0u;
		std::uint32_t missingPages = 0u;
		std::uint32_t failedPages = 0u;
		std::uint32_t evictedPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t residentHighDetailPages = 0u;
		std::uint32_t retiringPages = 0u;
	};

	struct VirtualGeometryPageBudgetPressureSummary
	{
		std::uint64_t budgetBytes = 0u;
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
		std::uint64_t residentHighDetailBytes = 0u;
		std::uint64_t embeddedFallbackBytes = 0u;
		std::uint64_t retiringBytes = 0u;
		std::uint64_t pendingIoBytes = 0u;
		std::uint64_t pendingUploadBytes = 0u;
		std::uint64_t failedBytes = 0u;
		std::uint64_t evictingBytes = 0u;
		std::uint64_t evictedBytes = 0u;
		std::uint64_t overBudgetBytes = 0u;
		std::uint64_t pressureBytes = 0u;
		std::uint32_t residentPages = 0u;
		std::uint32_t requestedPages = 0u;
		std::uint32_t missingPages = 0u;
		std::uint32_t failedPages = 0u;
		std::uint32_t evictedPages = 0u;
		std::uint32_t pinnedFallbackPages = 0u;
		std::uint32_t residentHighDetailPages = 0u;
		std::uint32_t retiringPages = 0u;
		std::vector<VirtualGeometryPageBudgetPressureContributor> contributors;
	};

	// AnimationResidencyTracker owns only request state. ResourceManager remains
	// responsible for IO and GPU work, so callers can exercise this policy on a
	// worker without mutating live resource payloads.
	struct AnimationResidencyRequest
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		ResidencyAssetType assetType = ResidencyAssetType::AnimationClip;
		std::uint64_t byteSize = 0u;
		StreamingPriorityClass priority = StreamingPriorityClass::Normal;
		bool gameplayCritical = false;
		bool offscreenAuthority = false;
		std::uint64_t requestGeneration = 0u;
	};

	// AnimationResidencyUsage records why a generation must remain available.
	// Visibility is intentionally separate from authoritative consumers: an
	// offscreen animator may stop visual work, but it must keep the data needed
	// for events, root motion, physics, recording, replication, and rollback.
	enum class AnimationResidencyUsage : std::uint32_t
	{
		None = 0u,
		Visible = 1u << 0u,
		Events = 1u << 1u,
		RootMotion = 1u << 2u,
		Physics = 1u << 3u,
		Recording = 1u << 4u,
		Replication = 1u << 5u,
		Rollback = 1u << 6u,
		Validation = 1u << 7u,
		Preview = 1u << 8u
	};

	constexpr AnimationResidencyUsage operator|(AnimationResidencyUsage left, AnimationResidencyUsage right) noexcept
	{
		return static_cast<AnimationResidencyUsage>(
			static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
	}

	constexpr AnimationResidencyUsage operator&(AnimationResidencyUsage left, AnimationResidencyUsage right) noexcept
	{
		return static_cast<AnimationResidencyUsage>(
			static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
	}

	constexpr bool Any(AnimationResidencyUsage usage) noexcept
	{
		return static_cast<std::uint32_t>(usage) != 0u;
	}

	struct AnimationResidencyDecision
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t requestGeneration = 0u;
		ResidencyState state = ResidencyState::Loading;
		bool accepted = false;
		bool retainedForAuthority = false;
		bool safeFallbackRequired = true;
		std::uint64_t activeGeneration = 0u;
		std::uint64_t residentBytes = 0u;
		std::string reason;
	};

	struct AnimationResidencyRecord
	{
		AnimationResidencyRequest request;
		ResidencyState state = ResidencyState::Loading;
		AnimationResidencyUsage retainedUsages = AnimationResidencyUsage::None;
		std::uint32_t retainCount = 0u;
		std::uint64_t lastTouchedSequence = 0u;
		bool activeGeneration = false;
		bool cancellationRequested = false;
		std::string lastDiagnostic;
	};

	struct AnimationResidencyTypeDiagnostics
	{
		ResidencyAssetType assetType = ResidencyAssetType::AnimationClip;
		std::uint32_t trackedGenerations = 0u;
		std::uint32_t loadingGenerations = 0u;
		std::uint32_t residentGenerations = 0u;
		std::uint32_t retainedGenerations = 0u;
		std::uint32_t failedGenerations = 0u;
		std::uint64_t residentBytes = 0u;
	};

	struct AnimationResidencyDiagnostics
	{
		std::uint64_t budgetBytes = 0u;
		std::uint64_t residentBytes = 0u;
		std::uint64_t evictableBytes = 0u;
		std::uint64_t protectedBytes = 0u;
		std::uint64_t overBudgetBytes = 0u;
		std::uint64_t rejectedRequests = 0u;
		std::uint64_t cancelledRequests = 0u;
		std::uint64_t staleCompletions = 0u;
		std::vector<AnimationResidencyTypeDiagnostics> types;
	};

	class TERRAENGINE_API AnimationResidencyTracker final
	{
	public:
		explicit AnimationResidencyTracker(std::size_t maximumTrackedGenerations = 4096u);
		AnimationResidencyDecision Request(const AnimationResidencyRequest& request);
		bool Retain(
			const TerraEngine::UUID& assetId,
			std::uint64_t requestGeneration,
			AnimationResidencyUsage usage);
		bool Release(
			const TerraEngine::UUID& assetId,
			std::uint64_t requestGeneration,
			AnimationResidencyUsage usage);
		bool Touch(const TerraEngine::UUID& assetId, std::uint64_t requestGeneration);
		bool Cancel(const TerraEngine::UUID& assetId, std::uint64_t requestGeneration);
		bool Complete(const TerraEngine::UUID& assetId, std::uint64_t requestGeneration, bool success);
		std::vector<AnimationResidencyDecision> ApplyBudget(std::uint64_t budgetBytes);
		[[nodiscard]] std::optional<AnimationResidencyDecision> Query(
			const TerraEngine::UUID& assetId,
			std::uint64_t requestGeneration) const;
		[[nodiscard]] std::vector<AnimationResidencyRequest> Snapshot() const;
		[[nodiscard]] std::vector<AnimationResidencyRecord> SnapshotRecords() const;
		[[nodiscard]] AnimationResidencyDiagnostics BuildDiagnostics(std::uint64_t budgetBytes) const;

	private:
		struct TrackedGeneration
		{
			AnimationResidencyRecord record;
			std::array<std::uint32_t, 9u> usageRetainCounts{};
		};

		[[nodiscard]] static std::string MakeKey(
			const TerraEngine::UUID& assetId,
			std::uint64_t requestGeneration);
		[[nodiscard]] static bool IsAnimationAssetType(ResidencyAssetType type) noexcept;
		[[nodiscard]] static bool RequiresAuthorityRetention(const AnimationResidencyRequest& request) noexcept;
		[[nodiscard]] static bool IsResidentState(ResidencyState state) noexcept;
		[[nodiscard]] static bool HasRetains(const TrackedGeneration& generation) noexcept;
		[[nodiscard]] static AnimationResidencyUsage BuildRetainedUsage(const TrackedGeneration& generation) noexcept;
		[[nodiscard]] AnimationResidencyDecision MakeDecisionLocked(const TrackedGeneration& generation) const;
		void PruneReleasedLocked();
		void RefreshRetentionStateLocked(TrackedGeneration& generation);

		std::size_t m_maximumTrackedGenerations = 4096u;
		std::uint64_t m_touchSequence = 0u;
		std::uint64_t m_rejectedRequests = 0u;
		std::uint64_t m_cancelledRequests = 0u;
		std::uint64_t m_staleCompletions = 0u;
		mutable std::mutex m_mutex;
		std::unordered_map<std::string, TrackedGeneration> m_generations;
		std::unordered_map<std::string, std::uint64_t> m_latestRequestedGeneration;
		std::unordered_map<std::string, std::uint64_t> m_activeGeneration;
	};

	TERRAENGINE_API ResidencyPolicy MakeDefaultResidencyPolicy();
	TERRAENGINE_API ResidencyPolicy SanitizeResidencyPolicy(const ResidencyPolicy& policy);
	TERRAENGINE_API ResidencyAssetType ToResidencyAssetType(AssetType type);
	TERRAENGINE_API std::string_view ToString(ResidencyAssetType type);
	TERRAENGINE_API std::string_view ToString(ResidencyState state);
	TERRAENGINE_API std::string_view ToString(ResidencyPinReason reason);
	TERRAENGINE_API std::string_view ToString(ResidencyReleaseReason reason);
	TERRAENGINE_API std::string_view ToString(ResidencyReleaseResult result);
	TERRAENGINE_API std::string_view ToString(ResidencyBlockedReason reason);
	TERRAENGINE_API std::string_view ToString(ResidencyReleaseDomain domain);
	TERRAENGINE_API std::string_view ToString(VirtualGeometryPageResidencyState state);
	TERRAENGINE_API std::string_view ToString(CpuPayloadOwnershipKind kind);
	TERRAENGINE_API std::string_view ToString(CpuPayloadOwnershipState state);
	TERRAENGINE_API std::string_view ToString(GpuAllocationOwnershipKind kind);
	TERRAENGINE_API std::string_view ToString(GpuAllocationOwnershipState state);
	TERRAENGINE_API ExactResidencyByteValue MakeExactResidencyBytes(std::uint64_t bytes);
	TERRAENGINE_API ExactResidencyByteValue MakeZeroByStateResidencyBytes();
	TERRAENGINE_API ExactResidencyByteValue MakeUnavailableResidencyBytes(std::string_view reason);
	TERRAENGINE_API bool IsExactResidencyNumeric(const ExactResidencyByteValue& value);
	TERRAENGINE_API std::uint64_t ExactResidencyBytesOrZero(const ExactResidencyByteValue& value);
	TERRAENGINE_API bool CpuPayloadContributesCurrentBytes(const CpuPayloadOwnershipRecord& record);
	TERRAENGINE_API bool GpuAllocationContributesCurrentBytes(const GpuAllocationOwnershipRecord& record);
	TERRAENGINE_API ExactResidencyByteValue AggregateCpuPayloadBytes(const std::vector<CpuPayloadOwnershipRecord>& records, const TerraEngine::UUID& assetId);
	TERRAENGINE_API ExactResidencyByteValue AggregateGpuAllocationBytes(const std::vector<GpuAllocationOwnershipRecord>& records, const TerraEngine::UUID& assetId);
	TERRAENGINE_API std::vector<ExactResidencyUnavailableDiagnostic> BuildExactResidencyUnavailableDiagnostics(const std::vector<ResidentAsset>& assets);
	TERRAENGINE_API bool IsVirtualGeometryPagePinnedFallback(const VirtualGeometryResidentPage& page);
	TERRAENGINE_API bool IsVirtualGeometryPageResidentForRendering(const VirtualGeometryResidentPage& page);
	TERRAENGINE_API std::uint64_t EstimateResidentBytes(const ResidentAsset& asset);
	TERRAENGINE_API ResidencyBlockedReason EvaluateReleaseBlocker(const ResidentAsset& asset, const ResidencyPolicy& policy, double nowSeconds);
	TERRAENGINE_API std::vector<ReleaseCandidate> BuildReleaseCandidates(const ResidencySnapshot& snapshot);
	TERRAENGINE_API ResidencyDiagnosticsSummary BuildResidencyDiagnostics(const ResidencySnapshot& snapshot, const std::vector<ReleaseRecord>& records, bool includeCandidates);
	TERRAENGINE_API VirtualGeometryPageTransitionResult ApplyVirtualGeometryPageTransition(VirtualGeometryResidentPage& page, VirtualGeometryPageResidencyEvent event, std::uint64_t frameId, std::string_view reason = {});
	TERRAENGINE_API std::uint32_t ProtectVirtualGeometryFallbackPages(std::vector<VirtualGeometryResidentPage>& pages, std::uint64_t frameId, std::string_view reason = {});
	TERRAENGINE_API std::vector<VirtualGeometryPageEvictionCandidate> BuildVirtualGeometryPageEvictionCandidates(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget);
	TERRAENGINE_API VirtualGeometryPageBudgetPressureSummary BuildVirtualGeometryPageBudgetPressureSummary(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget);
	TERRAENGINE_API AnimatedVirtualPageResidencyPublication
		PublishAnimatedVirtualPageResidencySnapshot(
			const AnimatedVirtualPageResidencySnapshot& prior,
			AnimatedVirtualPageResidencySnapshot candidate,
			std::uint64_t completedFence);
}
