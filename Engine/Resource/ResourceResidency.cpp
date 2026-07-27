// ResourceResidency.cpp - deterministic loaded-resource residency decisions.
//
// The helpers here are intentionally pure where practical. ResourceManager can
// feed snapshots into them from any safe synchronization point, and tests can
// verify policy decisions without creating renderer or editor state.

#include "Resource/ResourceResidency.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

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
		case AssetType::Skeleton: return ResidencyAssetType::Skeleton;
		case AssetType::AnimationClip: return ResidencyAssetType::AnimationClip;
		case AssetType::AnimationController: return ResidencyAssetType::AnimationController;
		case AssetType::AnimationMask: return ResidencyAssetType::AnimationMask;
		case AssetType::RetargetProfile: return ResidencyAssetType::RetargetProfile;
		case AssetType::MotionDatabase: return ResidencyAssetType::MotionDatabase;
		case AssetType::RagdollProfile: return ResidencyAssetType::RagdollProfile;
		// Physics assets remain provider-owned payloads until their dedicated
		// residency accounting is introduced, so they retain the legacy fallback.
		case AssetType::PhysicsMaterial:
		case AssetType::CookedCollider:
			return ResidencyAssetType::Other;
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
		case ResidencyAssetType::Skeleton: return "skeleton";
		case ResidencyAssetType::AnimationClip: return "animation-clip";
		case ResidencyAssetType::AnimationController: return "animation-controller";
		case ResidencyAssetType::AnimationMask: return "animation-mask";
		case ResidencyAssetType::RetargetProfile: return "retarget-profile";
		case ResidencyAssetType::MotionDatabase: return "motion-database";
		case ResidencyAssetType::RagdollProfile: return "ragdoll-profile";
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

	std::string_view ToString(VirtualGeometryPageResidencyState state)
	{
		switch (state)
		{
		case VirtualGeometryPageResidencyState::NotResident: return "not-resident";
		case VirtualGeometryPageResidencyState::Requested: return "requested";
		case VirtualGeometryPageResidencyState::Loading: return "loading";
		case VirtualGeometryPageResidencyState::Resident: return "resident";
		case VirtualGeometryPageResidencyState::Failed: return "failed";
		case VirtualGeometryPageResidencyState::Evicting: return "evicting";
		case VirtualGeometryPageResidencyState::Evicted: return "evicted";
		case VirtualGeometryPageResidencyState::PendingIO: return "pending-io";
		case VirtualGeometryPageResidencyState::PendingUpload: return "pending-upload";
		case VirtualGeometryPageResidencyState::Pinned: return "pinned";
		case VirtualGeometryPageResidencyState::PendingRetirement: return "pending-retirement";
		case VirtualGeometryPageResidencyState::Retired: return "retired";
		}
		return "not-resident";
	}

	std::string_view ToString(CpuPayloadOwnershipKind kind)
	{
		switch (kind)
		{
		case CpuPayloadOwnershipKind::DecodedMesh: return "decoded-mesh";
		case CpuPayloadOwnershipKind::TexturePixels: return "texture-pixels";
		case CpuPayloadOwnershipKind::MaterialMetadata: return "material-metadata";
		case CpuPayloadOwnershipKind::ShaderCollection: return "shader-collection";
		case CpuPayloadOwnershipKind::VirtualGeometryPage: return "virtual-geometry-page";
		case CpuPayloadOwnershipKind::VirtualGeometryFallbackPage: return "virtual-geometry-fallback-page";
		case CpuPayloadOwnershipKind::VirtualGeometryMetadata: return "virtual-geometry-metadata";
		case CpuPayloadOwnershipKind::Skeleton: return "skeleton";
		case CpuPayloadOwnershipKind::AnimationClip: return "animation-clip";
		case CpuPayloadOwnershipKind::AnimationController: return "animation-controller";
		case CpuPayloadOwnershipKind::AnimationMask: return "animation-mask";
		case CpuPayloadOwnershipKind::RetargetProfile: return "retarget-profile";
		case CpuPayloadOwnershipKind::MotionDatabase: return "motion-database";
		case CpuPayloadOwnershipKind::RagdollProfile: return "ragdoll-profile";
		case CpuPayloadOwnershipKind::Unknown: return "unknown";
		}
		return "unknown";
	}

	std::string_view ToString(CpuPayloadOwnershipState state)
	{
		switch (state)
		{
		case CpuPayloadOwnershipState::Loaded: return "loaded";
		case CpuPayloadOwnershipState::Retained: return "retained";
		case CpuPayloadOwnershipState::PendingRelease: return "pending-release";
		case CpuPayloadOwnershipState::Released: return "released";
		case CpuPayloadOwnershipState::Failed: return "failed";
		case CpuPayloadOwnershipState::Unavailable: return "unavailable";
		}
		return "unavailable";
	}

	std::string_view ToString(GpuAllocationOwnershipKind kind)
	{
		switch (kind)
		{
		case GpuAllocationOwnershipKind::MeshBuffer: return "mesh-buffer";
		case GpuAllocationOwnershipKind::TextureImage: return "texture-image";
		case GpuAllocationOwnershipKind::MaterialBuffer: return "material-buffer";
		case GpuAllocationOwnershipKind::ShaderCollection: return "shader-collection";
		case GpuAllocationOwnershipKind::VirtualGeometryPageBuffer: return "virtual-geometry-page-buffer";
		case GpuAllocationOwnershipKind::VirtualGeometryFallbackPageBuffer: return "virtual-geometry-fallback-page-buffer";
		case GpuAllocationOwnershipKind::VirtualGeometryMetadataBuffer: return "virtual-geometry-metadata-buffer";
		case GpuAllocationOwnershipKind::DiagnosticBuffer: return "diagnostic-buffer";
		case GpuAllocationOwnershipKind::Unknown: return "unknown";
		}
		return "unknown";
	}

	std::string_view ToString(GpuAllocationOwnershipState state)
	{
		switch (state)
		{
		case GpuAllocationOwnershipState::Published: return "published";
		case GpuAllocationOwnershipState::InFlight: return "in-flight";
		case GpuAllocationOwnershipState::PendingRetirement: return "pending-retirement";
		case GpuAllocationOwnershipState::Retired: return "retired";
		case GpuAllocationOwnershipState::Unavailable: return "unavailable";
		}
		return "unavailable";
	}

	ExactResidencyByteValue MakeExactResidencyBytes(std::uint64_t bytes)
	{
		ExactResidencyByteValue value;
		value.bytes = bytes;
		value.status = ExactResidencyByteStatus::Exact;
		return value;
	}

	ExactResidencyByteValue MakeZeroByStateResidencyBytes()
	{
		ExactResidencyByteValue value;
		value.bytes = 0u;
		value.status = ExactResidencyByteStatus::ZeroByState;
		return value;
	}

	ExactResidencyByteValue MakeUnavailableResidencyBytes(std::string_view reason)
	{
		ExactResidencyByteValue value;
		value.bytes = 0u;
		value.status = ExactResidencyByteStatus::Unavailable;
		value.unavailableReason = reason.empty()
			? "exact residency accounting is unavailable"
			: std::string(reason);
		return value;
	}

	bool IsExactResidencyNumeric(const ExactResidencyByteValue& value)
	{
		return value.status == ExactResidencyByteStatus::Exact
			|| value.status == ExactResidencyByteStatus::ZeroByState;
	}

	std::uint64_t ExactResidencyBytesOrZero(const ExactResidencyByteValue& value)
	{
		return IsExactResidencyNumeric(value) ? value.bytes : 0u;
	}

	bool CpuPayloadContributesCurrentBytes(const CpuPayloadOwnershipRecord& record)
	{
		return record.byteSize > 0u
			&& (record.state == CpuPayloadOwnershipState::Loaded
				|| record.state == CpuPayloadOwnershipState::Retained);
	}

	bool GpuAllocationContributesCurrentBytes(const GpuAllocationOwnershipRecord& record)
	{
		return record.byteSize > 0u
			&& (record.state == GpuAllocationOwnershipState::Published
				|| record.state == GpuAllocationOwnershipState::InFlight
				|| record.state == GpuAllocationOwnershipState::PendingRetirement);
	}

	ExactResidencyByteValue AggregateCpuPayloadBytes(const std::vector<CpuPayloadOwnershipRecord>& records, const TerraEngine::UUID& assetId)
	{
		bool sawMatchingRecord = false;
		bool sawUnavailable = false;
		std::uint64_t bytes = 0u;
		for (const CpuPayloadOwnershipRecord& record : records)
		{
			if (record.ownerAssetId != assetId)
			{
				continue;
			}
			sawMatchingRecord = true;
			if (record.state == CpuPayloadOwnershipState::Unavailable)
			{
				sawUnavailable = true;
				continue;
			}
			if (CpuPayloadContributesCurrentBytes(record))
			{
				bytes = AddSaturated(bytes, record.byteSize);
			}
		}

		if (bytes > 0u)
		{
			return MakeExactResidencyBytes(bytes);
		}
		if (sawUnavailable)
		{
			return MakeUnavailableResidencyBytes("CPU payload ownership producer did not prove exact bytes");
		}
		return sawMatchingRecord ? MakeZeroByStateResidencyBytes() : MakeUnavailableResidencyBytes("no CPU payload ownership records were published");
	}

	ExactResidencyByteValue AggregateGpuAllocationBytes(const std::vector<GpuAllocationOwnershipRecord>& records, const TerraEngine::UUID& assetId)
	{
		bool sawMatchingRecord = false;
		bool sawUnavailable = false;
		std::uint64_t bytes = 0u;
		for (const GpuAllocationOwnershipRecord& record : records)
		{
			if (record.ownerAssetId != assetId)
			{
				continue;
			}
			sawMatchingRecord = true;
			if (record.state == GpuAllocationOwnershipState::Unavailable)
			{
				sawUnavailable = true;
				continue;
			}
			if (GpuAllocationContributesCurrentBytes(record))
			{
				bytes = AddSaturated(bytes, record.byteSize);
			}
		}

		if (bytes > 0u)
		{
			return MakeExactResidencyBytes(bytes);
		}
		if (sawUnavailable)
		{
			return MakeUnavailableResidencyBytes("GPU allocation ownership producer did not prove exact bytes");
		}
		return sawMatchingRecord ? MakeZeroByStateResidencyBytes() : MakeUnavailableResidencyBytes("no GPU allocation ownership records were published");
	}

	std::vector<ExactResidencyUnavailableDiagnostic> BuildExactResidencyUnavailableDiagnostics(const std::vector<ResidentAsset>& assets)
	{
		std::vector<ExactResidencyUnavailableDiagnostic> diagnostics;
		for (const ResidentAsset& asset : assets)
		{
			if (asset.exactCpuBytes.status == ExactResidencyByteStatus::Unavailable)
			{
				ExactResidencyUnavailableDiagnostic diagnostic;
				diagnostic.assetId = asset.assetId;
				diagnostic.domain = "cpu";
				diagnostic.missingProducer = "ResourceManager CPU payload ownership";
				diagnostic.reason = asset.exactCpuBytes.unavailableReason;
				diagnostics.push_back(std::move(diagnostic));
			}
			if (asset.exactGpuBytes.status == ExactResidencyByteStatus::Unavailable)
			{
				ExactResidencyUnavailableDiagnostic diagnostic;
				diagnostic.assetId = asset.assetId;
				diagnostic.domain = "gpu";
				diagnostic.missingProducer = "Renderer backend GPU allocation ownership";
				diagnostic.reason = asset.exactGpuBytes.unavailableReason;
				diagnostics.push_back(std::move(diagnostic));
			}
		}
		return diagnostics;
	}

	bool IsVirtualGeometryPagePinnedFallback(const VirtualGeometryResidentPage& page)
	{
		return page.fallbackPage
			|| page.pinnedFallback
			|| page.protectedFallback
			|| page.state == VirtualGeometryPageResidencyState::Pinned;
	}

	bool IsVirtualGeometryPageResidentForRendering(const VirtualGeometryResidentPage& page)
	{
		return page.state == VirtualGeometryPageResidencyState::Resident
			|| page.state == VirtualGeometryPageResidencyState::Pinned
			|| page.state == VirtualGeometryPageResidencyState::PendingRetirement;
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

	VirtualGeometryPageTransitionResult ApplyVirtualGeometryPageTransition(VirtualGeometryResidentPage& page, VirtualGeometryPageResidencyEvent event, std::uint64_t frameId, std::string_view reason)
	{
		VirtualGeometryPageTransitionResult result;
		result.previousState = page.state;
		result.newState = page.state;

		const auto apply = [&](VirtualGeometryPageResidencyState state, std::string text) {
			page.state = state;
			page.lastUsedFrame = frameId;
			page.lastStateChangeFrame = frameId;
			result.applied = true;
			result.newState = state;
			result.reason = std::move(text);
		};

		switch (event)
		{
		case VirtualGeometryPageResidencyEvent::Request:
			if (page.state == VirtualGeometryPageResidencyState::NotResident
				|| page.state == VirtualGeometryPageResidencyState::Evicted
				|| page.state == VirtualGeometryPageResidencyState::Failed)
			{
				page.failureReason.clear();
				page.lastRequestedFrame = frameId;
				++page.requestCount;
				apply(VirtualGeometryPageResidencyState::Requested, "page requested");
			}
			break;
		case VirtualGeometryPageResidencyEvent::BeginLoad:
			if (page.state == VirtualGeometryPageResidencyState::Requested)
			{
				apply(VirtualGeometryPageResidencyState::Loading, "page load started");
			}
			break;
		case VirtualGeometryPageResidencyEvent::BeginIO:
			if (page.state == VirtualGeometryPageResidencyState::Requested
				|| page.state == VirtualGeometryPageResidencyState::Loading)
			{
				apply(VirtualGeometryPageResidencyState::PendingIO, "page IO started");
			}
			break;
		case VirtualGeometryPageResidencyEvent::BeginUpload:
			if (page.state == VirtualGeometryPageResidencyState::PendingIO
				|| page.state == VirtualGeometryPageResidencyState::Loading)
			{
				apply(VirtualGeometryPageResidencyState::PendingUpload, "page upload queued");
			}
			break;
		case VirtualGeometryPageResidencyEvent::MarkResident:
			if (page.state == VirtualGeometryPageResidencyState::Requested
				|| page.state == VirtualGeometryPageResidencyState::Loading
				|| page.state == VirtualGeometryPageResidencyState::PendingIO
				|| page.state == VirtualGeometryPageResidencyState::PendingUpload
				|| page.state == VirtualGeometryPageResidencyState::Pinned)
			{
				page.failureReason.clear();
				page.lastResidentFrame = frameId;
				apply(IsVirtualGeometryPagePinnedFallback(page) ? VirtualGeometryPageResidencyState::Pinned : VirtualGeometryPageResidencyState::Resident, "page became resident");
			}
			break;
		case VirtualGeometryPageResidencyEvent::MarkFailed:
			if (page.state == VirtualGeometryPageResidencyState::Requested
				|| page.state == VirtualGeometryPageResidencyState::Loading
				|| page.state == VirtualGeometryPageResidencyState::PendingIO
				|| page.state == VirtualGeometryPageResidencyState::PendingUpload)
			{
				page.failureReason = reason.empty() ? "page load failed" : std::string(reason);
				apply(VirtualGeometryPageResidencyState::Failed, page.failureReason);
			}
			break;
		case VirtualGeometryPageResidencyEvent::BeginEvict:
			if (IsVirtualGeometryPagePinnedFallback(page))
			{
				result.reason = "fallback page preserved";
				break;
			}
			if (page.state == VirtualGeometryPageResidencyState::Resident)
			{
				apply(VirtualGeometryPageResidencyState::Evicting, "page eviction started");
			}
			break;
		case VirtualGeometryPageResidencyEvent::MarkEvicted:
			if (page.state == VirtualGeometryPageResidencyState::Evicting)
			{
				apply(VirtualGeometryPageResidencyState::Evicted, "page eviction completed");
			}
			break;
		case VirtualGeometryPageResidencyEvent::PinFallback:
			page.fallbackPage = true;
			page.pinnedFallback = true;
			if (page.state == VirtualGeometryPageResidencyState::Resident
				|| page.state == VirtualGeometryPageResidencyState::Pinned)
			{
				apply(VirtualGeometryPageResidencyState::Pinned, reason.empty() ? "fallback page pinned" : std::string(reason));
			}
			else
			{
				result.reason = "fallback pin recorded until the page becomes resident";
			}
			break;
		case VirtualGeometryPageResidencyEvent::UnpinFallback:
			page.pinnedFallback = false;
			if (page.state == VirtualGeometryPageResidencyState::Pinned)
			{
				apply(VirtualGeometryPageResidencyState::Resident, reason.empty() ? "fallback page unpinned" : std::string(reason));
			}
			break;
		case VirtualGeometryPageResidencyEvent::BeginRetire:
			if (IsVirtualGeometryPagePinnedFallback(page))
			{
				result.reason = "fallback page preserved";
				break;
			}
			if (page.state == VirtualGeometryPageResidencyState::Resident)
			{
				page.retirementFrame = frameId;
				apply(VirtualGeometryPageResidencyState::PendingRetirement, reason.empty() ? "page retirement queued" : std::string(reason));
			}
			break;
		case VirtualGeometryPageResidencyEvent::MarkRetired:
			if (page.state == VirtualGeometryPageResidencyState::PendingRetirement)
			{
				page.retirementFrame = frameId;
				apply(VirtualGeometryPageResidencyState::Retired, reason.empty() ? "page retirement completed" : std::string(reason));
			}
			break;
		}

		return result;
	}

	std::uint32_t ProtectVirtualGeometryFallbackPages(std::vector<VirtualGeometryResidentPage>& pages, std::uint64_t frameId, std::string_view reason)
	{
		std::uint32_t protectedCount = 0u;
		for (VirtualGeometryResidentPage& page : pages)
		{
			const bool fallbackIdentity = page.fallbackPage
				|| page.pinnedFallback
				|| page.protectedFallback
				|| page.pageId == page.fallbackPageId;
			if (!fallbackIdentity)
			{
				continue;
			}

			// Fallback pages are the visibility safety net for page streaming.
			// If budget or retirement work touched one earlier in the frame,
			// restore the protected renderable state before eviction candidates
			// are built so missing detail never becomes missing coverage.
			page.fallbackPage = true;
			page.pinnedFallback = true;
			page.protectedFallback = true;
			page.lastUsedFrame = frameId;
			page.lastStateChangeFrame = frameId;
			if (!reason.empty())
			{
				page.requestReason = std::string(reason);
			}
			if (page.state == VirtualGeometryPageResidencyState::Resident
				|| page.state == VirtualGeometryPageResidencyState::Pinned
				|| page.state == VirtualGeometryPageResidencyState::Evicting
				|| page.state == VirtualGeometryPageResidencyState::Evicted
				|| page.state == VirtualGeometryPageResidencyState::PendingRetirement)
			{
				page.state = VirtualGeometryPageResidencyState::Pinned;
			}
			++protectedCount;
		}
		return protectedCount;
	}

	std::vector<VirtualGeometryPageEvictionCandidate> BuildVirtualGeometryPageEvictionCandidates(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget)
	{
		if (budget.memoryBudgetBytes == 0u)
		{
			return {};
		}

		std::uint64_t residentBytes = 0u;
		for (const VirtualGeometryResidentPage& page : pages)
		{
			if (IsVirtualGeometryPageResidentForRendering(page))
			{
				residentBytes = AddSaturated(residentBytes, page.byteSize);
			}
		}
		if (residentBytes <= budget.memoryBudgetBytes)
		{
			return {};
		}

		const std::uint64_t requiredReclaim = residentBytes - budget.memoryBudgetBytes;
		std::vector<VirtualGeometryPageEvictionCandidate> candidates;
		for (const VirtualGeometryResidentPage& page : pages)
		{
			if (!IsVirtualGeometryPageResidentForRendering(page)
				|| page.state == VirtualGeometryPageResidencyState::PendingRetirement
				|| IsVirtualGeometryPagePinnedFallback(page)
				|| page.byteSize == 0u)
			{
				continue;
			}

			VirtualGeometryPageEvictionCandidate candidate;
			candidate.assetId = page.assetId;
			candidate.pageId = page.pageId;
			candidate.reclaimableBytes = page.byteSize;
			candidate.priority = page.priority;
			candidate.lastUsedFrame = page.lastUsedFrame;
			candidates.push_back(candidate);
		}

		std::sort(candidates.begin(), candidates.end(), [](const VirtualGeometryPageEvictionCandidate& lhs, const VirtualGeometryPageEvictionCandidate& rhs) {
			if (lhs.priority != rhs.priority)
			{
				return lhs.priority < rhs.priority;
			}
			if (lhs.lastUsedFrame != rhs.lastUsedFrame)
			{
				return lhs.lastUsedFrame < rhs.lastUsedFrame;
			}
			if (lhs.reclaimableBytes != rhs.reclaimableBytes)
			{
				return lhs.reclaimableBytes > rhs.reclaimableBytes;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});

		std::uint64_t plannedReclaim = 0u;
		std::vector<VirtualGeometryPageEvictionCandidate> trimmed;
		for (VirtualGeometryPageEvictionCandidate candidate : candidates)
		{
			if (plannedReclaim >= requiredReclaim)
			{
				break;
			}
			plannedReclaim = AddSaturated(plannedReclaim, candidate.reclaimableBytes);
			trimmed.push_back(std::move(candidate));
		}
		return trimmed;
	}

	VirtualGeometryPageBudgetPressureSummary BuildVirtualGeometryPageBudgetPressureSummary(const std::vector<VirtualGeometryResidentPage>& pages, const VirtualGeometryPageBudget& budget)
	{
		VirtualGeometryPageBudgetPressureSummary summary;
		summary.budgetBytes = budget.memoryBudgetBytes;
		std::unordered_map<std::string, std::size_t> contributorByAsset;

		const auto contributorFor = [&](const VirtualGeometryResidentPage& page) -> VirtualGeometryPageBudgetPressureContributor& {
			const std::string key = TerraEngine::ToHexString(page.assetId);
			if (const auto existing = contributorByAsset.find(key); existing != contributorByAsset.end())
			{
				return summary.contributors[existing->second];
			}
			VirtualGeometryPageBudgetPressureContributor contributor;
			contributor.assetId = page.assetId;
			summary.contributors.push_back(contributor);
			const std::size_t index = summary.contributors.size() - 1u;
			contributorByAsset.emplace(key, index);
			return summary.contributors[index];
		};

		for (const VirtualGeometryResidentPage& page : pages)
		{
			VirtualGeometryPageBudgetPressureContributor& contributor = contributorFor(page);
			switch (page.state)
			{
			case VirtualGeometryPageResidencyState::Resident:
			case VirtualGeometryPageResidencyState::Pinned:
				++summary.residentPages;
				++contributor.residentPages;
				summary.residentBytes = AddSaturated(summary.residentBytes, page.byteSize);
				contributor.residentBytes = AddSaturated(contributor.residentBytes, page.byteSize);
				if (IsVirtualGeometryPagePinnedFallback(page))
				{
					++summary.pinnedFallbackPages;
					++contributor.pinnedFallbackPages;
					summary.pinnedFallbackBytes = AddSaturated(summary.pinnedFallbackBytes, page.byteSize);
					contributor.pinnedFallbackBytes = AddSaturated(contributor.pinnedFallbackBytes, page.byteSize);
					summary.embeddedFallbackBytes = AddSaturated(summary.embeddedFallbackBytes, page.byteSize);
					contributor.embeddedFallbackBytes = AddSaturated(contributor.embeddedFallbackBytes, page.byteSize);
				}
				if (!IsVirtualGeometryPagePinnedFallback(page))
				{
					++summary.residentHighDetailPages;
					++contributor.residentHighDetailPages;
					summary.residentHighDetailBytes = AddSaturated(summary.residentHighDetailBytes, page.byteSize);
					contributor.residentHighDetailBytes = AddSaturated(contributor.residentHighDetailBytes, page.byteSize);
					summary.reclaimableBytes = AddSaturated(summary.reclaimableBytes, page.byteSize);
					contributor.reclaimableBytes = AddSaturated(contributor.reclaimableBytes, page.byteSize);
				}
				break;
			case VirtualGeometryPageResidencyState::Requested:
			case VirtualGeometryPageResidencyState::Loading:
				++summary.requestedPages;
				++contributor.requestedPages;
				summary.requestedBytes = AddSaturated(summary.requestedBytes, page.byteSize);
				contributor.requestedBytes = AddSaturated(contributor.requestedBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::PendingIO:
				++summary.requestedPages;
				++contributor.requestedPages;
				summary.requestedBytes = AddSaturated(summary.requestedBytes, page.byteSize);
				contributor.requestedBytes = AddSaturated(contributor.requestedBytes, page.byteSize);
				summary.pendingIoBytes = AddSaturated(summary.pendingIoBytes, page.byteSize);
				contributor.pendingIoBytes = AddSaturated(contributor.pendingIoBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::PendingUpload:
				++summary.requestedPages;
				++contributor.requestedPages;
				summary.requestedBytes = AddSaturated(summary.requestedBytes, page.byteSize);
				contributor.requestedBytes = AddSaturated(contributor.requestedBytes, page.byteSize);
				summary.pendingUploadBytes = AddSaturated(summary.pendingUploadBytes, page.byteSize);
				contributor.pendingUploadBytes = AddSaturated(contributor.pendingUploadBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::Failed:
				++summary.failedPages;
				++summary.missingPages;
				++contributor.failedPages;
				++contributor.missingPages;
				summary.failedBytes = AddSaturated(summary.failedBytes, page.byteSize);
				contributor.failedBytes = AddSaturated(contributor.failedBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::Evicted:
				++summary.evictedPages;
				++summary.missingPages;
				++contributor.evictedPages;
				++contributor.missingPages;
				summary.evictedBytes = AddSaturated(summary.evictedBytes, page.byteSize);
				contributor.evictedBytes = AddSaturated(contributor.evictedBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::Evicting:
				++summary.missingPages;
				++contributor.missingPages;
				summary.evictingBytes = AddSaturated(summary.evictingBytes, page.byteSize);
				contributor.evictingBytes = AddSaturated(contributor.evictingBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::PendingRetirement:
				++summary.residentPages;
				++contributor.residentPages;
				++summary.retiringPages;
				++contributor.retiringPages;
				summary.residentBytes = AddSaturated(summary.residentBytes, page.byteSize);
				contributor.residentBytes = AddSaturated(contributor.residentBytes, page.byteSize);
				summary.retiringBytes = AddSaturated(summary.retiringBytes, page.byteSize);
				contributor.retiringBytes = AddSaturated(contributor.retiringBytes, page.byteSize);
				break;
			case VirtualGeometryPageResidencyState::Retired:
				++summary.evictedPages;
				++summary.missingPages;
				++contributor.evictedPages;
				++contributor.missingPages;
				break;
			case VirtualGeometryPageResidencyState::NotResident:
				++summary.missingPages;
				++contributor.missingPages;
				break;
			}
		}

		if (summary.budgetBytes > 0u && summary.residentBytes > summary.budgetBytes)
		{
			summary.overBudgetBytes = summary.residentBytes - summary.budgetBytes;
			// Keep both names: pressureBytes matches the public streaming
			// contract, while overBudgetBytes preserves the older C++ API.
			summary.pressureBytes = summary.overBudgetBytes;
		}

		std::sort(summary.contributors.begin(), summary.contributors.end(), [](const VirtualGeometryPageBudgetPressureContributor& lhs, const VirtualGeometryPageBudgetPressureContributor& rhs) {
			if (lhs.reclaimableBytes != rhs.reclaimableBytes)
			{
				return lhs.reclaimableBytes > rhs.reclaimableBytes;
			}
			if (lhs.residentBytes != rhs.residentBytes)
			{
				return lhs.residentBytes > rhs.residentBytes;
			}
			return TerraEngine::ToHexString(lhs.assetId) < TerraEngine::ToHexString(rhs.assetId);
		});

		return summary;
	}

	AnimationResidencyTracker::AnimationResidencyTracker(std::size_t maximumTrackedGenerations)
		: m_maximumTrackedGenerations(std::max<std::size_t>(1u, maximumTrackedGenerations))
	{
	}

	std::string AnimationResidencyTracker::MakeKey(
		const TerraEngine::UUID& assetId,
		std::uint64_t requestGeneration)
	{
		return TerraEngine::ToHexString(assetId) + ":" + std::to_string(requestGeneration);
	}

	bool AnimationResidencyTracker::IsAnimationAssetType(ResidencyAssetType type) noexcept
	{
		return type >= ResidencyAssetType::Skeleton && type <= ResidencyAssetType::RagdollProfile;
	}

	bool AnimationResidencyTracker::RequiresAuthorityRetention(const AnimationResidencyRequest& request) noexcept
	{
		return request.gameplayCritical || request.offscreenAuthority;
	}

	bool AnimationResidencyTracker::IsResidentState(ResidencyState state) noexcept
	{
		return state == ResidencyState::Active || state == ResidencyState::Idle || state == ResidencyState::Pinned;
	}

	bool AnimationResidencyTracker::HasRetains(const TrackedGeneration& generation) noexcept
	{
		return std::any_of(generation.usageRetainCounts.begin(), generation.usageRetainCounts.end(),
			[](std::uint32_t count) { return count != 0u; });
	}

	AnimationResidencyUsage AnimationResidencyTracker::BuildRetainedUsage(const TrackedGeneration& generation) noexcept
	{
		AnimationResidencyUsage usages = AnimationResidencyUsage::None;
		for (std::size_t index = 0u; index < generation.usageRetainCounts.size(); ++index)
		{
			if (generation.usageRetainCounts[index] != 0u)
			{
				usages = usages | static_cast<AnimationResidencyUsage>(1u << index);
			}
		}
		return usages;
	}

	AnimationResidencyDecision AnimationResidencyTracker::MakeDecisionLocked(const TrackedGeneration& generation) const
	{
		AnimationResidencyDecision decision;
		decision.assetId = generation.record.request.assetId;
		decision.requestGeneration = generation.record.request.requestGeneration;
		decision.state = generation.record.state;
		decision.accepted = generation.record.state != ResidencyState::Failed
			&& generation.record.state != ResidencyState::Released;
		decision.retainedForAuthority = RequiresAuthorityRetention(generation.record.request) || HasRetains(generation);
		decision.safeFallbackRequired = !IsResidentState(generation.record.state);
		decision.residentBytes = IsResidentState(generation.record.state) ? generation.record.request.byteSize : 0u;
		const std::string assetKey = TerraEngine::ToHexString(generation.record.request.assetId);
		if (const auto active = m_activeGeneration.find(assetKey); active != m_activeGeneration.end())
		{
			decision.activeGeneration = active->second;
		}
		decision.reason = generation.record.lastDiagnostic;
		return decision;
	}

	void AnimationResidencyTracker::RefreshRetentionStateLocked(TrackedGeneration& generation)
	{
		generation.record.retainedUsages = BuildRetainedUsage(generation);
		generation.record.retainCount = 0u;
		for (std::uint32_t count : generation.usageRetainCounts)
		{
			generation.record.retainCount = count > std::numeric_limits<std::uint32_t>::max() - generation.record.retainCount
				? std::numeric_limits<std::uint32_t>::max()
				: generation.record.retainCount + count;
		}
		if (IsResidentState(generation.record.state))
		{
			const bool retained = HasRetains(generation)
				|| (generation.record.activeGeneration && RequiresAuthorityRetention(generation.record.request));
			generation.record.state = retained ? ResidencyState::Pinned : ResidencyState::Idle;
		}
	}

	void AnimationResidencyTracker::PruneReleasedLocked()
	{
		if (m_generations.size() < m_maximumTrackedGenerations)
		{
			return;
		}
		std::vector<std::pair<std::string, std::uint64_t>> released;
		for (const auto& [key, generation] : m_generations)
		{
			if (generation.record.state == ResidencyState::Released)
			{
				released.emplace_back(key, generation.record.lastTouchedSequence);
			}
		}
		std::sort(released.begin(), released.end(), [](const auto& left, const auto& right) {
			return left.second != right.second ? left.second < right.second : left.first < right.first;
		});
		for (const auto& [key, sequence] : released)
		{
			(void)sequence;
			if (m_generations.size() < m_maximumTrackedGenerations)
			{
				break;
			}
			const auto found = m_generations.find(key);
			if (found == m_generations.end())
			{
				continue;
			}
			const std::string assetKey = TerraEngine::ToHexString(found->second.record.request.assetId);
			m_generations.erase(found);
			const bool assetStillTracked = std::any_of(m_generations.begin(), m_generations.end(),
				[&](const auto& pair) {
					return TerraEngine::ToHexString(pair.second.record.request.assetId) == assetKey;
				});
			if (!assetStillTracked)
			{
				m_latestRequestedGeneration.erase(assetKey);
				m_activeGeneration.erase(assetKey);
			}
		}
	}

	AnimationResidencyDecision AnimationResidencyTracker::Request(const AnimationResidencyRequest& request)
	{
		AnimationResidencyDecision decision;
		decision.assetId = request.assetId;
		decision.requestGeneration = request.requestGeneration;
		if (request.assetId == TerraEngine::EMPTY_UUID || request.requestGeneration == 0u
			|| request.byteSize == 0u || !IsAnimationAssetType(request.assetType))
		{
			decision.reason = "A request requires a typed animation asset, nonzero bytes, and a nonzero generation.";
			return decision;
		}

		std::scoped_lock lock(m_mutex);
		const std::string assetKey = TerraEngine::ToHexString(request.assetId);
		const std::string generationKey = MakeKey(request.assetId, request.requestGeneration);
		if (const auto latest = m_latestRequestedGeneration.find(assetKey);
			latest != m_latestRequestedGeneration.end() && request.requestGeneration < latest->second)
		{
			++m_rejectedRequests;
			decision.reason = "Stale animation residency generation rejected before IO was scheduled.";
			return decision;
		}
		if (const auto existing = m_generations.find(generationKey); existing != m_generations.end())
		{
			const AnimationResidencyRequest& prior = existing->second.record.request;
			const bool identical = prior.assetType == request.assetType && prior.byteSize == request.byteSize
				&& prior.priority == request.priority && prior.gameplayCritical == request.gameplayCritical
				&& prior.offscreenAuthority == request.offscreenAuthority;
			if (!identical)
			{
				++m_rejectedRequests;
				decision.reason = "The same generation was reused with different immutable request data.";
				return decision;
			}
			decision = MakeDecisionLocked(existing->second);
			decision.reason = decision.accepted
				? "Idempotent request joined the existing generation."
				: "A terminal generation cannot be reused; submit a newer request generation.";
			return decision;
		}

		// A newer generation supersedes unfinished IO for this asset. Marking the
		// older request terminal prevents a completion that can no longer publish
		// from consuming the bounded request table forever.
		for (auto& [key, generation] : m_generations)
		{
			(void)key;
			if (generation.record.request.assetId == request.assetId
				&& generation.record.request.requestGeneration < request.requestGeneration
				&& generation.record.state == ResidencyState::Loading)
			{
				generation.record.state = ResidencyState::Released;
				generation.record.cancellationRequested = true;
				generation.record.lastTouchedSequence = ++m_touchSequence;
				generation.record.lastDiagnostic = "Superseded by a newer animation residency generation.";
				++m_cancelledRequests;
			}
		}

		PruneReleasedLocked();
		if (m_generations.size() >= m_maximumTrackedGenerations)
		{
			++m_rejectedRequests;
			decision.reason = "The bounded animation residency generation table is full.";
			return decision;
		}

		TrackedGeneration tracked;
		tracked.record.request = request;
		tracked.record.state = ResidencyState::Loading;
		tracked.record.lastTouchedSequence = ++m_touchSequence;
		tracked.record.lastDiagnostic = "Queued asynchronously; the prior generation or reference pose remains safe.";
		m_generations.emplace(generationKey, std::move(tracked));
		m_latestRequestedGeneration[assetKey] = request.requestGeneration;
		decision = MakeDecisionLocked(m_generations.at(generationKey));
		decision.accepted = true;
		return decision;
	}

	bool AnimationResidencyTracker::Retain(
		const TerraEngine::UUID& assetId,
		std::uint64_t requestGeneration,
		AnimationResidencyUsage usage)
	{
		const std::uint32_t bits = static_cast<std::uint32_t>(usage);
		if (bits == 0u || (bits & ~0x1ffu) != 0u)
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		if (found == m_generations.end() || found->second.record.state == ResidencyState::Released
			|| found->second.record.state == ResidencyState::Failed)
		{
			return false;
		}
		for (std::size_t index = 0u; index < found->second.usageRetainCounts.size(); ++index)
		{
			if ((bits & (1u << index)) != 0u
				&& found->second.usageRetainCounts[index] == std::numeric_limits<std::uint32_t>::max())
			{
				return false;
			}
		}
		for (std::size_t index = 0u; index < found->second.usageRetainCounts.size(); ++index)
		{
			if ((bits & (1u << index)) != 0u)
			{
				++found->second.usageRetainCounts[index];
			}
		}
		found->second.record.lastTouchedSequence = ++m_touchSequence;
		found->second.record.lastDiagnostic = "Retained by an explicit animation consumer.";
		RefreshRetentionStateLocked(found->second);
		return true;
	}

	bool AnimationResidencyTracker::Release(
		const TerraEngine::UUID& assetId,
		std::uint64_t requestGeneration,
		AnimationResidencyUsage usage)
	{
		const std::uint32_t bits = static_cast<std::uint32_t>(usage);
		if (bits == 0u || (bits & ~0x1ffu) != 0u)
		{
			return false;
		}
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		if (found == m_generations.end())
		{
			return false;
		}
		for (std::size_t index = 0u; index < found->second.usageRetainCounts.size(); ++index)
		{
			if ((bits & (1u << index)) != 0u && found->second.usageRetainCounts[index] == 0u)
			{
				return false;
			}
		}
		for (std::size_t index = 0u; index < found->second.usageRetainCounts.size(); ++index)
		{
			if ((bits & (1u << index)) != 0u)
			{
				--found->second.usageRetainCounts[index];
			}
		}
		found->second.record.lastTouchedSequence = ++m_touchSequence;
		found->second.record.lastDiagnostic = "An animation consumer released its generation retain.";
		RefreshRetentionStateLocked(found->second);
		return true;
	}

	bool AnimationResidencyTracker::Touch(const TerraEngine::UUID& assetId, std::uint64_t requestGeneration)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		if (found == m_generations.end() || found->second.record.state == ResidencyState::Released)
		{
			return false;
		}
		found->second.record.lastTouchedSequence = ++m_touchSequence;
		return true;
	}

	bool AnimationResidencyTracker::Cancel(const TerraEngine::UUID& assetId, std::uint64_t requestGeneration)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		if (found == m_generations.end() || found->second.record.state != ResidencyState::Loading)
		{
			return false;
		}
		found->second.record.state = ResidencyState::Released;
		found->second.record.cancellationRequested = true;
		found->second.record.lastTouchedSequence = ++m_touchSequence;
		found->second.record.lastDiagnostic = "Cancelled before protected resource publication.";
		++m_cancelledRequests;
		return true;
	}

	bool AnimationResidencyTracker::Complete(
		const TerraEngine::UUID& assetId,
		std::uint64_t requestGeneration,
		bool success)
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		const std::string assetKey = TerraEngine::ToHexString(assetId);
		const auto latest = m_latestRequestedGeneration.find(assetKey);
		if (found == m_generations.end() || found->second.record.state != ResidencyState::Loading
			|| found->second.record.cancellationRequested
			|| latest == m_latestRequestedGeneration.end() || latest->second != requestGeneration)
		{
			++m_staleCompletions;
			return false;
		}
		found->second.record.lastTouchedSequence = ++m_touchSequence;
		if (!success)
		{
			found->second.record.state = ResidencyState::Failed;
			found->second.record.lastDiagnostic = "Load failed; last-known-good or reference-pose fallback remains authoritative.";
			return true;
		}

		if (const auto previous = m_activeGeneration.find(assetKey); previous != m_activeGeneration.end())
		{
			if (const auto old = m_generations.find(MakeKey(assetId, previous->second)); old != m_generations.end())
			{
				old->second.record.activeGeneration = false;
				RefreshRetentionStateLocked(old->second);
			}
		}
		m_activeGeneration[assetKey] = requestGeneration;
		found->second.record.activeGeneration = true;
		found->second.record.state = ResidencyState::Idle;
		found->second.record.lastDiagnostic = "Complete generation published at the protected residency boundary.";
		RefreshRetentionStateLocked(found->second);
		return true;
	}

	std::vector<AnimationResidencyDecision> AnimationResidencyTracker::ApplyBudget(std::uint64_t budgetBytes)
	{
		std::scoped_lock lock(m_mutex);
		std::uint64_t residentBytes = 0u;
		std::vector<std::pair<std::string, TrackedGeneration*>> candidates;
		for (auto& [key, generation] : m_generations)
		{
			if (!IsResidentState(generation.record.state))
			{
				continue;
			}
			residentBytes = AddSaturated(residentBytes, generation.record.request.byteSize);
			const bool retained = HasRetains(generation)
				|| (generation.record.activeGeneration && RequiresAuthorityRetention(generation.record.request));
			if (!retained)
			{
				candidates.emplace_back(key, &generation);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
			const AnimationResidencyRecord& lhs = left.second->record;
			const AnimationResidencyRecord& rhs = right.second->record;
			const std::uint8_t leftRank = PriorityReleaseRank(lhs.request.priority);
			const std::uint8_t rightRank = PriorityReleaseRank(rhs.request.priority);
			if (leftRank != rightRank) return leftRank < rightRank;
			if (lhs.activeGeneration != rhs.activeGeneration) return !lhs.activeGeneration;
			if (lhs.lastTouchedSequence != rhs.lastTouchedSequence) return lhs.lastTouchedSequence < rhs.lastTouchedSequence;
			return left.first < right.first;
		});
		std::vector<AnimationResidencyDecision> decisions;
		for (const auto& [key, generation] : candidates)
		{
			(void)key;
			if (residentBytes <= budgetBytes) break;
			const std::uint64_t bytes = generation->record.request.byteSize;
			generation->record.state = ResidencyState::Released;
			generation->record.lastTouchedSequence = ++m_touchSequence;
			generation->record.lastDiagnostic = "Evicted under pressure; a safe fallback is required.";
			if (generation->record.activeGeneration)
			{
				m_activeGeneration.erase(TerraEngine::ToHexString(generation->record.request.assetId));
				generation->record.activeGeneration = false;
			}
			residentBytes = residentBytes > bytes ? residentBytes - bytes : 0u;
			AnimationResidencyDecision decision = MakeDecisionLocked(*generation);
			decision.accepted = true;
			decisions.push_back(std::move(decision));
		}
		return decisions;
	}

	std::optional<AnimationResidencyDecision> AnimationResidencyTracker::Query(
		const TerraEngine::UUID& assetId,
		std::uint64_t requestGeneration) const
	{
		std::scoped_lock lock(m_mutex);
		const auto found = m_generations.find(MakeKey(assetId, requestGeneration));
		if (found == m_generations.end())
		{
			return std::nullopt;
		}
		return MakeDecisionLocked(found->second);
	}

	std::vector<AnimationResidencyRequest> AnimationResidencyTracker::Snapshot() const
	{
		const std::vector<AnimationResidencyRecord> records = SnapshotRecords();
		std::vector<AnimationResidencyRequest> requests;
		requests.reserve(records.size());
		for (const AnimationResidencyRecord& record : records)
		{
			requests.push_back(record.request);
		}
		return requests;
	}

	std::vector<AnimationResidencyRecord> AnimationResidencyTracker::SnapshotRecords() const
	{
		std::scoped_lock lock(m_mutex);
		std::vector<AnimationResidencyRecord> records;
		records.reserve(m_generations.size());
		for (const auto& [key, generation] : m_generations)
		{
			(void)key;
			records.push_back(generation.record);
		}
		std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
			const std::string leftId = TerraEngine::ToHexString(left.request.assetId);
			const std::string rightId = TerraEngine::ToHexString(right.request.assetId);
			return leftId != rightId ? leftId < rightId
				: left.request.requestGeneration < right.request.requestGeneration;
		});
		return records;
	}

	AnimationResidencyDiagnostics AnimationResidencyTracker::BuildDiagnostics(std::uint64_t budgetBytes) const
	{
		std::scoped_lock lock(m_mutex);
		AnimationResidencyDiagnostics diagnostics;
		diagnostics.budgetBytes = budgetBytes;
		diagnostics.rejectedRequests = m_rejectedRequests;
		diagnostics.cancelledRequests = m_cancelledRequests;
		diagnostics.staleCompletions = m_staleCompletions;
		constexpr std::array<ResidencyAssetType, 7u> types = {
			ResidencyAssetType::Skeleton, ResidencyAssetType::AnimationClip,
			ResidencyAssetType::AnimationController, ResidencyAssetType::AnimationMask,
			ResidencyAssetType::RetargetProfile, ResidencyAssetType::MotionDatabase,
			ResidencyAssetType::RagdollProfile
		};
		for (ResidencyAssetType type : types)
		{
			diagnostics.types.push_back(AnimationResidencyTypeDiagnostics{type});
		}
		for (const auto& [key, generation] : m_generations)
		{
			(void)key;
			auto type = std::find_if(diagnostics.types.begin(), diagnostics.types.end(),
				[&generation](const auto& item) { return item.assetType == generation.record.request.assetType; });
			if (type == diagnostics.types.end()) continue;
			++type->trackedGenerations;
			if (generation.record.state == ResidencyState::Loading) ++type->loadingGenerations;
			if (generation.record.state == ResidencyState::Failed) ++type->failedGenerations;
			if (!IsResidentState(generation.record.state)) continue;
			++type->residentGenerations;
			type->residentBytes = AddSaturated(type->residentBytes, generation.record.request.byteSize);
			diagnostics.residentBytes = AddSaturated(diagnostics.residentBytes, generation.record.request.byteSize);
			const bool retained = HasRetains(generation)
				|| (generation.record.activeGeneration && RequiresAuthorityRetention(generation.record.request));
			if (retained)
			{
				++type->retainedGenerations;
				diagnostics.protectedBytes = AddSaturated(diagnostics.protectedBytes, generation.record.request.byteSize);
			}
			else
			{
				diagnostics.evictableBytes = AddSaturated(diagnostics.evictableBytes, generation.record.request.byteSize);
			}
		}
		diagnostics.overBudgetBytes = diagnostics.residentBytes > budgetBytes
			? diagnostics.residentBytes - budgetBytes : 0u;
		return diagnostics;
	}
}
