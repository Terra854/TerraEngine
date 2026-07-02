// ResourceResidency.h - loaded resource residency policy and diagnostics.
//
// This module describes when a loaded engine resource may leave CPU/GPU
// memory. It intentionally does not manage persistent disk caches; callers use
// these data structures to reason about live runtime payloads only.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "Resource/ResourceStreaming.h"
#include "Core/Export.h"
#include "Utilities/UUID.h"

enum class AssetType;

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
		Other
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
		Pinned
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
		UnpinFallback
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
	};

	struct VirtualGeometryPageBudgetPressureContributor
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
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
	};

	struct VirtualGeometryPageBudgetPressureSummary
	{
		std::uint64_t budgetBytes = 0u;
		std::uint64_t residentBytes = 0u;
		std::uint64_t requestedBytes = 0u;
		std::uint64_t reclaimableBytes = 0u;
		std::uint64_t pinnedFallbackBytes = 0u;
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
		std::vector<VirtualGeometryPageBudgetPressureContributor> contributors;
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
	TERRAENGINE_API bool IsVirtualGeometryPagePinnedFallback(const VirtualGeometryResidentPage& page);
	TERRAENGINE_API bool IsVirtualGeometryPageResidentForRendering(const VirtualGeometryResidentPage& page);
	TERRAENGINE_API std::uint64_t EstimateResidentBytes(const ResidentAsset& asset);
	TERRAENGINE_API ResidencyBlockedReason EvaluateReleaseBlocker(const ResidentAsset& asset, const ResidencyPolicy& policy, double nowSeconds);
	TERRAENGINE_API std::vector<ReleaseCandidate> BuildReleaseCandidates(const ResidencySnapshot& snapshot);
	TERRAENGINE_API ResidencyDiagnosticsSummary BuildResidencyDiagnostics(const ResidencySnapshot& snapshot, const std::vector<ReleaseRecord>& records, bool includeCandidates);
	TERRAENGINE_API VirtualGeometryPageTransitionResult ApplyVirtualGeometryPageTransition(VirtualGeometryResidentPage& page, VirtualGeometryPageResidencyEvent event, std::uint64_t frameId, std::string_view reason = {});
	TERRAENGINE_API std::vector<VirtualGeometryPageEvictionCandidate> BuildVirtualGeometryPageEvictionCandidates(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget);
	TERRAENGINE_API VirtualGeometryPageBudgetPressureSummary BuildVirtualGeometryPageBudgetPressureSummary(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget);
}
