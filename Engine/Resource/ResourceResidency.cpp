// ResourceResidency.cpp - deterministic loaded-resource residency decisions.
//
// The helpers here are intentionally pure where practical. ResourceManager can
// feed snapshots into them from any safe synchronization point, and tests can
// verify policy decisions without creating renderer or editor state.

#include "Resource/ResourceResidency.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "Resource/ResourceManager.h"

namespace TerraEngine::Resource
{
	namespace
	{
		constexpr double kDefaultIdleSeconds = 10.0;
		constexpr double kDefaultCleanupIntervalSeconds = 1.0;
		constexpr double kDefaultReloadGraceSeconds = 0.5;

		bool ContainsUuid(const std::vector<TerraEngine::UUID>& values, const TerraEngine::UUID& value)
		{
			return std::find(values.begin(), values.end(), value) != values.end();
		}
	}

	ResidencyPolicy MakeDefaultResidencyPolicy()
	{
		ResidencyPolicy policy;
		policy.enabled = true;
		policy.idleSecondsThreshold = kDefaultIdleSeconds;
		policy.memoryBudgetBytes = 0;
		policy.cleanupIntervalSeconds = kDefaultCleanupIntervalSeconds;
		policy.reloadGraceSeconds = kDefaultReloadGraceSeconds;
		policy.strictBudgetMode = false;
		policy.diagnosticsEnabled = false;
		policy.protectedGroups = { "default", "fallback", "editor-selection", "preview", "active-scene", "prefab-editing", "loading" };
		return policy;
	}

	ResidencyPolicy SanitizeResidencyPolicy(const ResidencyPolicy& policy)
	{
		ResidencyPolicy sanitized = policy;
		if (sanitized.idleSecondsThreshold < 0.0)
		{
			sanitized.idleSecondsThreshold = kDefaultIdleSeconds;
		}
		if (sanitized.cleanupIntervalSeconds <= 0.0)
		{
			sanitized.cleanupIntervalSeconds = kDefaultCleanupIntervalSeconds;
		}
		if (sanitized.reloadGraceSeconds < 0.0)
		{
			sanitized.reloadGraceSeconds = 0.0;
		}
		return sanitized;
	}

	ResidencyAssetType ToResidencyAssetType(AssetType type)
	{
		switch (type)
		{
		case AssetType::Mesh: return ResidencyAssetType::Mesh;
		case AssetType::Texture: return ResidencyAssetType::Texture;
		case AssetType::Material: return ResidencyAssetType::Material;
		case AssetType::ShaderCollection: return ResidencyAssetType::ShaderCollection;
		}
		return ResidencyAssetType::Other;
	}

	std::string_view ToString(ResidencyAssetType type)
	{
		switch (type)
		{
		case ResidencyAssetType::Mesh: return "mesh";
		case ResidencyAssetType::Texture: return "texture";
		case ResidencyAssetType::Material: return "material";
		case ResidencyAssetType::Shader: return "shader";
		case ResidencyAssetType::ShaderCollection: return "shader-collection";
		case ResidencyAssetType::FontAtlas: return "font-atlas";
		case ResidencyAssetType::Other: return "other";
		}
		return "other";
	}

	std::string_view ToString(ResidencyState state)
	{
		switch (state)
		{
		case ResidencyState::Loading: return "loading";
		case ResidencyState::Active: return "active";
		case ResidencyState::Idle: return "idle";
		case ResidencyState::Pinned: return "pinned";
		case ResidencyState::PendingRelease: return "pending-release";
		case ResidencyState::Released: return "released";
		case ResidencyState::Failed: return "failed";
		}
		return "idle";
	}

	std::string_view ToString(ResidencyPinReason reason)
	{
		switch (reason)
		{
		case ResidencyPinReason::None: return "none";
		case ResidencyPinReason::DefaultResource: return "default-resource";
		case ResidencyPinReason::FallbackResource: return "fallback-resource";
		case ResidencyPinReason::EditorSelection: return "editor-selection";
		case ResidencyPinReason::Preview: return "preview";
		case ResidencyPinReason::ActiveScene: return "active-scene";
		case ResidencyPinReason::PrefabEditing: return "prefab-editing";
		case ResidencyPinReason::ExportStartup: return "export-startup";
		case ResidencyPinReason::Loading: return "loading";
		case ResidencyPinReason::BackendInFlight: return "backend-in-flight";
		case ResidencyPinReason::PolicyProtected: return "policy-protected";
		}
		return "none";
	}

	std::string_view ToString(ResidencyReleaseReason reason)
	{
		switch (reason)
		{
		case ResidencyReleaseReason::None: return "none";
		case ResidencyReleaseReason::IdleExpiry: return "idle-expiry";
		case ResidencyReleaseReason::MemoryPressure: return "memory-pressure";
		case ResidencyReleaseReason::SceneUnload: return "scene-unload";
		case ResidencyReleaseReason::ExplicitRuntimeUnload: return "explicit-runtime-unload";
		case ResidencyReleaseReason::Disabled: return "disabled";
		}
		return "none";
	}

	std::string_view ToString(ResidencyReleaseResult result)
	{
		switch (result)
		{
		case ResidencyReleaseResult::Released: return "released";
		case ResidencyReleaseResult::Deferred: return "deferred";
		case ResidencyReleaseResult::Skipped: return "skipped";
		case ResidencyReleaseResult::Failed: return "failed";
		}
		return "skipped";
	}

	std::string_view ToString(ResidencyBlockedReason reason)
	{
		switch (reason)
		{
		case ResidencyBlockedReason::None: return "none";
		case ResidencyBlockedReason::Active: return "active";
		case ResidencyBlockedReason::Pinned: return "pinned";
		case ResidencyBlockedReason::Loading: return "loading";
		case ResidencyBlockedReason::Failed: return "failed";
		case ResidencyBlockedReason::NotReloadable: return "not-reloadable";
		case ResidencyBlockedReason::InFlight: return "in-flight";
		case ResidencyBlockedReason::RecentlyUsed: return "recently-used";
		}
		return "none";
	}

	std::uint64_t EstimateResidentBytes(const ResidentAsset& asset)
	{
		constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
		if (asset.estimatedCpuBytes > max - asset.estimatedGpuBytes)
		{
			return max;
		}
		return asset.estimatedCpuBytes + asset.estimatedGpuBytes;
	}

	ResidencyBlockedReason EvaluateReleaseBlocker(const ResidentAsset& asset, const ResidencyPolicy& policy, double nowSeconds)
	{
		if (asset.state == ResidencyState::Active)
		{
			return ResidencyBlockedReason::Active;
		}
		if (asset.state == ResidencyState::Loading)
		{
			return ResidencyBlockedReason::Loading;
		}
		if (asset.state == ResidencyState::Failed)
		{
			return ResidencyBlockedReason::Failed;
		}
		if (asset.pinReason != ResidencyPinReason::None || asset.state == ResidencyState::Pinned)
		{
			return ResidencyBlockedReason::Pinned;
		}
		if (!asset.reloadable || asset.sourceKey.empty())
		{
			return ResidencyBlockedReason::NotReloadable;
		}
		if (asset.inFlight)
		{
			return ResidencyBlockedReason::InFlight;
		}
		const double idleSeconds = nowSeconds >= asset.lastUsedSeconds ? nowSeconds - asset.lastUsedSeconds : 0.0;
		if (idleSeconds < policy.reloadGraceSeconds)
		{
			return ResidencyBlockedReason::RecentlyUsed;
		}
		return ResidencyBlockedReason::None;
	}

	std::vector<ReleaseCandidate> BuildReleaseCandidates(const ResidencySnapshot& snapshot)
	{
		const ResidencyPolicy policy = SanitizeResidencyPolicy(snapshot.policy);
		if (!policy.enabled)
		{
			return {};
		}

		std::uint64_t residentBytes = 0;
		for (const ResidentAsset& asset : snapshot.residentAssets)
		{
			residentBytes += EstimateResidentBytes(asset);
		}
		const bool overBudget = policy.memoryBudgetBytes > 0 && residentBytes > policy.memoryBudgetBytes;
		std::uint64_t requiredReclaim = overBudget ? residentBytes - policy.memoryBudgetBytes : 0;

		std::vector<ReleaseCandidate> candidates;
		for (const ResidentAsset& asset : snapshot.residentAssets)
		{
			if (asset.state == ResidencyState::Released || asset.state == ResidencyState::PendingRelease)
			{
				continue;
			}

			const bool markedActive = ContainsUuid(snapshot.activeAssetIds, asset.assetId);
			const bool markedPinned = ContainsUuid(snapshot.pinnedAssetIds, asset.assetId);
			ResidentAsset effectiveAsset = asset;
			if (markedActive)
			{
				effectiveAsset.state = ResidencyState::Active;
			}
			if (markedPinned && effectiveAsset.pinReason == ResidencyPinReason::None)
			{
				effectiveAsset.pinReason = ResidencyPinReason::PolicyProtected;
			}

			const double idleSeconds = snapshot.nowSeconds >= effectiveAsset.lastUsedSeconds ? snapshot.nowSeconds - effectiveAsset.lastUsedSeconds : 0.0;
			const bool idleExpired = idleSeconds >= policy.idleSecondsThreshold;
			const ResidencyBlockedReason blocker = EvaluateReleaseBlocker(effectiveAsset, policy, snapshot.nowSeconds);
			if (blocker != ResidencyBlockedReason::None)
			{
				continue;
			}
			if (!idleExpired && !overBudget)
			{
				continue;
			}

			ReleaseCandidate candidate;
			candidate.assetId = effectiveAsset.assetId;
			candidate.assetType = effectiveAsset.assetType;
			candidate.sourceKey = effectiveAsset.sourceKey;
			candidate.idleSeconds = idleSeconds;
			candidate.estimatedReclaimableBytes = EstimateResidentBytes(effectiveAsset);
			candidate.releaseReason = idleExpired ? ResidencyReleaseReason::IdleExpiry : ResidencyReleaseReason::MemoryPressure;
			candidate.blockedReason = ResidencyBlockedReason::None;
			candidates.push_back(std::move(candidate));
		}

		std::sort(candidates.begin(), candidates.end(), [](const ReleaseCandidate& lhs, const ReleaseCandidate& rhs) {
			if (lhs.releaseReason != rhs.releaseReason)
			{
				return lhs.releaseReason == ResidencyReleaseReason::IdleExpiry;
			}
			if (lhs.idleSeconds != rhs.idleSeconds)
			{
				return lhs.idleSeconds > rhs.idleSeconds;
			}
			if (lhs.estimatedReclaimableBytes != rhs.estimatedReclaimableBytes)
			{
				return lhs.estimatedReclaimableBytes > rhs.estimatedReclaimableBytes;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});

		if (overBudget)
		{
			std::uint64_t reclaimed = 0;
			std::vector<ReleaseCandidate> trimmed;
			for (ReleaseCandidate candidate : candidates)
			{
				if (candidate.releaseReason == ResidencyReleaseReason::IdleExpiry || reclaimed < requiredReclaim)
				{
					if (candidate.releaseReason != ResidencyReleaseReason::IdleExpiry)
					{
						reclaimed += candidate.estimatedReclaimableBytes;
					}
					trimmed.push_back(std::move(candidate));
				}
			}
			return trimmed;
		}

		return candidates;
	}

	ResidencyDiagnosticsSummary BuildResidencyDiagnostics(const ResidencySnapshot& snapshot, const std::vector<ReleaseRecord>& records, bool includeCandidates)
	{
		ResidencyDiagnosticsSummary summary;
		summary.frameId = snapshot.frameId;
		summary.diagnosticsEnabled = snapshot.policy.diagnosticsEnabled;
		summary.residentAssetCount = snapshot.residentAssets.size();
		summary.activeAssetCount = snapshot.activeAssetIds.size();
		summary.pinnedAssetCount = snapshot.pinnedAssetIds.size();

		for (const ResidentAsset& asset : snapshot.residentAssets)
		{
			summary.estimatedResidentBytes += EstimateResidentBytes(asset);
			if (asset.state == ResidencyState::PendingRelease)
			{
				++summary.pendingReleaseCount;
			}
		}

		for (const ReleaseRecord& record : records)
		{
			if (record.result == ResidencyReleaseResult::Released)
			{
				++summary.releasedAssetCount;
				summary.estimatedReclaimedBytes += record.bytesReclaimed;
			}
			if (summary.cleanupReason == ResidencyReleaseReason::None)
			{
				summary.cleanupReason = record.releaseReason;
			}
		}

		if (includeCandidates)
		{
			summary.candidates = BuildReleaseCandidates(snapshot);
			summary.idleCandidateCount = summary.candidates.size();
			summary.records = records;
		}
		return summary;
	}
}
