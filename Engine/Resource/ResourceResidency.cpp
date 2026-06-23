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
		constexpr double kDefaultCpuIdleSeconds = 30.0;
		constexpr double kDefaultGpuIdleSeconds = 10.0;
		constexpr double kDefaultCleanupIntervalSeconds = 1.0;
		constexpr double kDefaultCpuReloadGraceSeconds = 5.0;
		constexpr double kDefaultGpuReloadGraceSeconds = 0.5;

		bool ContainsUuid(const std::vector<TerraEngine::UUID>& values, const TerraEngine::UUID& value)
		{
			return std::find(values.begin(), values.end(), value) != values.end();
		}

		std::uint64_t AddSaturated(std::uint64_t lhs, std::uint64_t rhs)
		{
			constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
			return lhs > max - rhs ? max : lhs + rhs;
		}

		ResidencyBlockedReason EvaluateCommonReleaseBlocker(const ResidentAsset& asset)
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
			return ResidencyBlockedReason::None;
		}

		ResidencyBlockedReason EvaluateDomainReleaseBlocker(const ResidentAsset& asset, double reloadGraceSeconds, double nowSeconds)
		{
			const ResidencyBlockedReason common = EvaluateCommonReleaseBlocker(asset);
			if (common != ResidencyBlockedReason::None)
			{
				return common;
			}
			const double idleSeconds = nowSeconds >= asset.lastUsedSeconds ? nowSeconds - asset.lastUsedSeconds : 0.0;
			if (idleSeconds < reloadGraceSeconds)
			{
				return ResidencyBlockedReason::RecentlyUsed;
			}
			return ResidencyBlockedReason::None;
		}

		std::uint8_t PriorityReleaseRank(StreamingPriorityClass priorityClass)
		{
			switch (priorityClass)
			{
			case StreamingPriorityClass::Background:
				return 0u;
			case StreamingPriorityClass::Normal:
				return 1u;
			case StreamingPriorityClass::Important:
				return 2u;
			case StreamingPriorityClass::Critical:
				return 3u;
			}
			return 1u;
		}

		std::uint8_t TierReleaseRank(StreamingResidencyTier tier)
		{
			switch (tier)
			{
			case StreamingResidencyTier::Streamed:
				return 0u;
			case StreamingResidencyTier::Warm:
				return 1u;
			case StreamingResidencyTier::Persistent:
				return 2u;
			}
			return 0u;
		}

		double IdleMultiplierForTier(StreamingResidencyTier tier)
		{
			switch (tier)
			{
			case StreamingResidencyTier::Streamed:
				return 1.0;
			case StreamingResidencyTier::Warm:
				return 3.0;
			case StreamingResidencyTier::Persistent:
				return std::numeric_limits<double>::infinity();
			}
			return 1.0;
		}

		bool IdleExpiredForTier(double idleSeconds, double baseThreshold, StreamingResidencyTier tier)
		{
			if (tier == StreamingResidencyTier::Persistent)
			{
				return false;
			}
			return idleSeconds >= baseThreshold * IdleMultiplierForTier(tier);
		}

	}

	ResidencyPolicy MakeDefaultResidencyPolicy()
	{
		ResidencyPolicy policy;
		policy.enabled = true;
		policy.cleanupIntervalSeconds = kDefaultCleanupIntervalSeconds;
		policy.cpu.idleSecondsThreshold = kDefaultCpuIdleSeconds;
		policy.cpu.memoryBudgetBytes = 0;
		policy.cpu.reloadGraceSeconds = kDefaultCpuReloadGraceSeconds;
		policy.gpu.idleSecondsThreshold = kDefaultGpuIdleSeconds;
		policy.gpu.memoryBudgetBytes = 0;
		policy.gpu.reloadGraceSeconds = kDefaultGpuReloadGraceSeconds;
		policy.gpu.strictBudgetMode = false;
		policy.diagnosticsEnabled = false;
		policy.protectedGroups = { "default", "fallback", "editor-selection", "preview", "active-scene", "prefab-editing", "loading" };
		return policy;
	}

	ResidencyPolicy SanitizeResidencyPolicy(const ResidencyPolicy& policy)
	{
		ResidencyPolicy sanitized = policy;
		if (sanitized.cpu.idleSecondsThreshold < 0.0)
		{
			sanitized.cpu.idleSecondsThreshold = kDefaultCpuIdleSeconds;
		}
		if (sanitized.gpu.idleSecondsThreshold < 0.0)
		{
			sanitized.gpu.idleSecondsThreshold = kDefaultGpuIdleSeconds;
		}
		if (sanitized.cleanupIntervalSeconds <= 0.0)
		{
			sanitized.cleanupIntervalSeconds = kDefaultCleanupIntervalSeconds;
		}
		if (sanitized.cpu.reloadGraceSeconds < 0.0)
		{
			sanitized.cpu.reloadGraceSeconds = 0.0;
		}
		if (sanitized.gpu.reloadGraceSeconds < 0.0)
		{
			sanitized.gpu.reloadGraceSeconds = 0.0;
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

	std::string_view ToString(ResidencyReleaseDomain domain)
	{
		switch (domain)
		{
		case ResidencyReleaseDomain::Cpu: return "cpu";
		case ResidencyReleaseDomain::Gpu: return "gpu";
		case ResidencyReleaseDomain::CpuAndGpu: return "cpu+gpu";
		}
		return "cpu+gpu";
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
		const ResidencyPolicy sanitized = SanitizeResidencyPolicy(policy);
		const double graceSeconds = std::min(sanitized.cpu.reloadGraceSeconds, sanitized.gpu.reloadGraceSeconds);
		return EvaluateDomainReleaseBlocker(asset, graceSeconds, nowSeconds);
	}

	std::vector<ReleaseCandidate> BuildReleaseCandidates(const ResidencySnapshot& snapshot)
	{
		const ResidencyPolicy policy = SanitizeResidencyPolicy(snapshot.policy);
		if (!policy.enabled)
		{
			return {};
		}

		std::uint64_t residentCpuBytes = 0;
		std::uint64_t residentGpuBytes = 0;
		for (const ResidentAsset& asset : snapshot.residentAssets)
		{
			residentCpuBytes = AddSaturated(residentCpuBytes, asset.estimatedCpuBytes);
			residentGpuBytes = AddSaturated(residentGpuBytes, asset.estimatedGpuBytes);
		}
		const bool cpuOverBudget = policy.cpu.memoryBudgetBytes > 0 && residentCpuBytes > policy.cpu.memoryBudgetBytes;
		const bool gpuOverBudget = policy.gpu.memoryBudgetBytes > 0 && residentGpuBytes > policy.gpu.memoryBudgetBytes;
		const std::uint64_t requiredCpuReclaim = cpuOverBudget ? residentCpuBytes - policy.cpu.memoryBudgetBytes : 0;
		const std::uint64_t requiredGpuReclaim = gpuOverBudget ? residentGpuBytes - policy.gpu.memoryBudgetBytes : 0;

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
			const auto appendCandidate = [&](ResidencyReleaseDomain domain,
				std::uint64_t reclaimableBytes,
				bool idleExpired,
				bool overBudget,
				double reloadGraceSeconds)
			{
				if (reclaimableBytes == 0u || (!idleExpired && !overBudget))
				{
					return;
				}
				const ResidencyBlockedReason blocker = EvaluateDomainReleaseBlocker(effectiveAsset, reloadGraceSeconds, snapshot.nowSeconds);
				if (blocker != ResidencyBlockedReason::None)
				{
					return;
				}

				ReleaseCandidate candidate;
				candidate.assetId = effectiveAsset.assetId;
				candidate.assetType = effectiveAsset.assetType;
				candidate.sourceKey = effectiveAsset.sourceKey;
				candidate.idleSeconds = idleSeconds;
				candidate.estimatedReclaimableBytes = reclaimableBytes;
				candidate.releaseReason = idleExpired ? ResidencyReleaseReason::IdleExpiry : ResidencyReleaseReason::MemoryPressure;
				candidate.releaseDomain = domain;
				candidate.priorityClass = effectiveAsset.priorityClass;
				candidate.residencyTier = effectiveAsset.residencyTier;
				candidate.blockedReason = ResidencyBlockedReason::None;
				candidates.push_back(std::move(candidate));
			};

			appendCandidate(ResidencyReleaseDomain::Gpu,
				effectiveAsset.estimatedGpuBytes,
				IdleExpiredForTier(idleSeconds, policy.gpu.idleSecondsThreshold, effectiveAsset.residencyTier),
				gpuOverBudget,
				policy.gpu.reloadGraceSeconds);
			appendCandidate(ResidencyReleaseDomain::Cpu,
				effectiveAsset.estimatedCpuBytes,
				IdleExpiredForTier(idleSeconds, policy.cpu.idleSecondsThreshold, effectiveAsset.residencyTier),
				cpuOverBudget,
				policy.cpu.reloadGraceSeconds);
		}

		std::sort(candidates.begin(), candidates.end(), [](const ReleaseCandidate& lhs, const ReleaseCandidate& rhs) {
			if (lhs.releaseReason != rhs.releaseReason)
			{
				return lhs.releaseReason == ResidencyReleaseReason::IdleExpiry;
			}
			if (lhs.priorityClass != rhs.priorityClass)
			{
				return PriorityReleaseRank(lhs.priorityClass) < PriorityReleaseRank(rhs.priorityClass);
			}
			if (lhs.residencyTier != rhs.residencyTier)
			{
				return TierReleaseRank(lhs.residencyTier) < TierReleaseRank(rhs.residencyTier);
			}
			if (lhs.idleSeconds != rhs.idleSeconds)
			{
				return lhs.idleSeconds > rhs.idleSeconds;
			}
			if (lhs.estimatedReclaimableBytes != rhs.estimatedReclaimableBytes)
			{
				return lhs.estimatedReclaimableBytes > rhs.estimatedReclaimableBytes;
			}
			if (lhs.releaseDomain != rhs.releaseDomain)
			{
				return lhs.releaseDomain == ResidencyReleaseDomain::Gpu;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});

		if (cpuOverBudget || gpuOverBudget)
		{
			std::uint64_t reclaimedCpu = 0;
			std::uint64_t reclaimedGpu = 0;
			std::vector<ReleaseCandidate> trimmed;
			for (ReleaseCandidate candidate : candidates)
			{
				if (candidate.releaseReason == ResidencyReleaseReason::IdleExpiry)
				{
					trimmed.push_back(std::move(candidate));
					continue;
				}

				const bool helpsCpu = candidate.releaseDomain == ResidencyReleaseDomain::Cpu || candidate.releaseDomain == ResidencyReleaseDomain::CpuAndGpu;
				const bool helpsGpu = candidate.releaseDomain == ResidencyReleaseDomain::Gpu || candidate.releaseDomain == ResidencyReleaseDomain::CpuAndGpu;
				const bool needsCpu = helpsCpu && reclaimedCpu < requiredCpuReclaim;
				const bool needsGpu = helpsGpu && reclaimedGpu < requiredGpuReclaim;
				if (!needsCpu && !needsGpu)
				{
					continue;
				}
				if (helpsCpu)
				{
					reclaimedCpu = AddSaturated(reclaimedCpu, candidate.estimatedReclaimableBytes);
				}
				if (helpsGpu)
				{
					reclaimedGpu = AddSaturated(reclaimedGpu, candidate.estimatedReclaimableBytes);
				}
				trimmed.push_back(std::move(candidate));
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
