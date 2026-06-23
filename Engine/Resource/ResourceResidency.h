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
	TERRAENGINE_API std::uint64_t EstimateResidentBytes(const ResidentAsset& asset);
	TERRAENGINE_API ResidencyBlockedReason EvaluateReleaseBlocker(const ResidentAsset& asset, const ResidencyPolicy& policy, double nowSeconds);
	TERRAENGINE_API std::vector<ReleaseCandidate> BuildReleaseCandidates(const ResidencySnapshot& snapshot);
	TERRAENGINE_API ResidencyDiagnosticsSummary BuildResidencyDiagnostics(const ResidencySnapshot& snapshot, const std::vector<ReleaseRecord>& records, bool includeCandidates);
}
