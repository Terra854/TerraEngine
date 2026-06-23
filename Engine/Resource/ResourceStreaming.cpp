// ResourceStreaming.cpp - deterministic visibility streaming helper logic.
//
// These helpers are deliberately pure or close to pure. ResourceManager owns
// synchronized cache mutation, while renderer/scene systems can build
// candidate lists on worker tasks and feed compact needs back to the manager.

#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <charconv>

namespace TerraEngine::Resource
{
	namespace
	{
		constexpr double kDefaultPreloadDistance = 75.0;
		constexpr double kDefaultVisibleRetentionSeconds = 2.0;
		constexpr double kDefaultNeededRetentionSeconds = 5.0;

		bool UuidLess(const TerraEngine::UUID& left, const TerraEngine::UUID& right)
		{
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		bool NeedLess(const StreamingNeed& left, const StreamingNeed& right)
		{
			if (left.priority != right.priority)
			{
				return left.priority > right.priority;
			}
			if (left.assetId != right.assetId)
			{
				return UuidLess(left.assetId, right.assetId);
			}
			if (left.needType != right.needType)
			{
				return static_cast<std::uint8_t>(left.needType) < static_cast<std::uint8_t>(right.needType);
			}
			if (left.needId != right.needId)
			{
				return left.needId < right.needId;
			}
			return left.reason < right.reason;
		}

		bool NeedEquivalent(const StreamingNeed& left, const StreamingNeed& right)
		{
			return left.assetId == right.assetId
				&& left.needType == right.needType
				&& left.needId == right.needId
				&& left.viewId == right.viewId
				&& left.objectId == right.objectId;
		}

		bool ParseNonNegativeDouble(std::string_view text)
		{
			if (text.empty())
			{
				return false;
			}
			double value = 0.0;
			const char* begin = text.data();
			const char* end = text.data() + text.size();
			const std::from_chars_result result = std::from_chars(begin, end, value);
			return result.ec == std::errc{} && result.ptr == end && value >= 0.0;
		}
	}

	StreamingPolicy MakeDefaultStreamingPolicy()
	{
		StreamingPolicy policy;
		policy.enabled = true;
		policy.deferScenePayloadsOnLoad = true;
		policy.preloadDistance = kDefaultPreloadDistance;
		policy.visibleRetentionSeconds = kDefaultVisibleRetentionSeconds;
		policy.neededRetentionSeconds = kDefaultNeededRetentionSeconds;
		policy.maxLoadsPerUpdate = 0u;
		policy.textureMipStreamingEnabled = false;
		policy.textureMinStreamMip = 0u;
		policy.textureMaxStreamMip = 0u;
		policy.memoryBudgetInteraction = MemoryBudgetInteraction::UseResidencyPolicy;
		policy.diagnosticsEnabled = false;
		policy.protectedNeedTypes = {
			StreamingNeedType::Visible,
			StreamingNeedType::EditorSelection,
			StreamingNeedType::InspectorPreview,
			StreamingNeedType::PrefabEditing,
			StreamingNeedType::AssetPreview,
			StreamingNeedType::ActiveUi,
			StreamingNeedType::RuntimeExplicit,
			StreamingNeedType::FallbackDefault,
			StreamingNeedType::BackendInFlight
		};
		return policy;
	}

	StreamingPolicy SanitizeStreamingPolicy(const StreamingPolicy& policy)
	{
		StreamingPolicy sanitized = policy;
		if (sanitized.preloadDistance < 0.0)
		{
			sanitized.preloadDistance = kDefaultPreloadDistance;
		}
		if (sanitized.visibleRetentionSeconds < 0.0)
		{
			sanitized.visibleRetentionSeconds = kDefaultVisibleRetentionSeconds;
		}
		if (sanitized.neededRetentionSeconds < 0.0)
		{
			sanitized.neededRetentionSeconds = kDefaultNeededRetentionSeconds;
		}
		if (sanitized.protectedNeedTypes.empty())
		{
			sanitized.protectedNeedTypes = MakeDefaultStreamingPolicy().protectedNeedTypes;
		}
		if (sanitized.textureMaxStreamMip < sanitized.textureMinStreamMip)
		{
			sanitized.textureMaxStreamMip = sanitized.textureMinStreamMip;
		}
		return sanitized;
	}

	StreamingAssetPolicy MakeDefaultStreamingAssetPolicy(StreamableAssetType type)
	{
		StreamingAssetPolicy policy;
		switch (type)
		{
		case StreamableAssetType::ShaderCollection:
			policy.priorityClass = StreamingPriorityClass::Critical;
			policy.residencyTier = StreamingResidencyTier::Persistent;
			policy.priorityBias = 1000;
			break;
		case StreamableAssetType::Material:
			policy.priorityClass = StreamingPriorityClass::Important;
			policy.residencyTier = StreamingResidencyTier::Warm;
			policy.priorityBias = 100;
			break;
		case StreamableAssetType::Mesh:
		case StreamableAssetType::Texture:
		case StreamableAssetType::Other:
			policy.priorityClass = StreamingPriorityClass::Normal;
			policy.residencyTier = StreamingResidencyTier::Streamed;
			policy.priorityBias = 0;
			break;
		}
		return policy;
	}

	StreamingAssetPolicy SanitizeStreamingAssetPolicy(const StreamingAssetPolicy& policy)
	{
		StreamingAssetPolicy sanitized = policy;
		if (sanitized.priorityBias < -10000)
		{
			sanitized.priorityBias = -10000;
		}
		if (sanitized.priorityBias > 10000)
		{
			sanitized.priorityBias = 10000;
		}
		return sanitized;
	}

	TextureMipResidency ResolveTextureMipResidency(const StreamingPolicy& policy, std::uint32_t sourceMipCount)
	{
		TextureMipResidency residency;
		residency.sourceMipCount = sourceMipCount;
		if (sourceMipCount == 0u)
		{
			return residency;
		}

		const StreamingPolicy sanitized = SanitizeStreamingPolicy(policy);
		if (!sanitized.textureMipStreamingEnabled
			|| sanitized.textureMaxStreamMip == 0u)
		{
			residency.sourceMipOffset = 0u;
			residency.residentMipCount = sourceMipCount;
			residency.partial = false;
			return residency;
		}

		// Mip streaming policy is expressed in source mip indices: mip 0 is
		// full resolution, and larger indices are progressively cheaper lower
		// detail levels. The backend currently uploads a contiguous tail that
		// starts at the chosen source mip, so sampling never touches missing
		// higher-detail levels.
		const std::uint32_t maxSourceMip = sourceMipCount - 1u;
		const std::uint32_t requestedSourceMip = std::clamp(
			sanitized.textureMinStreamMip,
			0u,
			std::min(sanitized.textureMaxStreamMip, maxSourceMip));
		residency.sourceMipOffset = requestedSourceMip;
		residency.residentMipCount = sourceMipCount - requestedSourceMip;
		residency.partial = residency.sourceMipOffset > 0u;
		return residency;
	}

	std::int32_t PriorityClassBias(StreamingPriorityClass priorityClass)
	{
		switch (priorityClass)
		{
		case StreamingPriorityClass::Background:
			return -100;
		case StreamingPriorityClass::Normal:
			return 0;
		case StreamingPriorityClass::Important:
			return 100;
		case StreamingPriorityClass::Critical:
			return 1000;
		}
		return 0;
	}

	bool ValidatePreloadRule(const PreloadRule& rule, std::string* reason)
	{
		if (!rule.enabled)
		{
			return true;
		}
		if (rule.ruleId.empty())
		{
			if (reason)
			{
				*reason = "ruleId is required";
			}
			return false;
		}
		if (rule.assetScope.empty())
		{
			if (reason)
			{
				*reason = "assetScope must contain at least one asset";
			}
			return false;
		}
		if (rule.criteriaType == PreloadCriteriaType::Distance)
		{
			if (!ParseNonNegativeDouble(rule.criteriaValue))
			{
				if (reason)
				{
					*reason = "distance criteria must be a non-negative number";
				}
				return false;
			}
			return true;
		}
		if (rule.criteriaValue.empty())
		{
			if (reason)
			{
				*reason = "criteriaValue is required";
			}
			return false;
		}
		return true;
	}

	std::vector<StreamingNeed> EvaluatePreloadRules(const std::vector<PreloadRule>& rules, double nowSeconds)
	{
		std::vector<StreamingNeed> needs;
		for (const PreloadRule& rule : rules)
		{
			if (!ValidatePreloadRule(rule))
			{
				continue;
			}

			// Rule evaluation returns needs only; ResourceManager still owns the
			// synchronized cache mutation and actual load scheduling. This keeps
			// region/tag/path scans worker-friendly as the world grows.
			for (const StreamableAssetId& assetId : rule.assetScope)
			{
				if (assetId == TerraEngine::EMPTY_UUID)
				{
					continue;
				}

				StreamingNeed need;
				need.needId = rule.ruleId;
				need.assetId = assetId;
				need.needType = rule.criteriaType == PreloadCriteriaType::Distance
					? StreamingNeedType::DistancePreload
					: StreamingNeedType::RulePreload;
				need.viewId = std::string(ToString(rule.criteriaType));
				need.expiresAtSeconds = rule.graceSeconds > 0.0 && nowSeconds > 0.0
					? nowSeconds + rule.graceSeconds
					: 0.0;
				need.priority = rule.priority;
				need.reason = rule.diagnosticName.empty()
					? std::string(ToString(rule.criteriaType)) + ":" + rule.criteriaValue
					: rule.diagnosticName;
				needs.push_back(std::move(need));
			}
		}

		return MergeStreamingNeeds(std::move(needs), nowSeconds);
	}

	std::vector<StreamingNeed> MergeStreamingNeeds(std::vector<StreamingNeed> needs, double nowSeconds)
	{
		needs.erase(std::remove_if(needs.begin(), needs.end(), [nowSeconds](const StreamingNeed& need) {
			return need.assetId == TerraEngine::EMPTY_UUID
				|| (need.expiresAtSeconds > 0.0 && nowSeconds > 0.0 && need.expiresAtSeconds <= nowSeconds);
		}), needs.end());
		std::sort(needs.begin(), needs.end(), NeedLess);
		needs.erase(std::unique(needs.begin(), needs.end(), NeedEquivalent), needs.end());
		return needs;
	}

	StreamingProtectionReason ProtectionForNeed(StreamingNeedType needType)
	{
		switch (needType)
		{
		case StreamingNeedType::Visible: return StreamingProtectionReason::ActiveView;
		case StreamingNeedType::NearVisible: return StreamingProtectionReason::PreloadRule;
		case StreamingNeedType::DistancePreload: return StreamingProtectionReason::PreloadRule;
		case StreamingNeedType::RulePreload: return StreamingProtectionReason::PreloadRule;
		case StreamingNeedType::EditorSelection: return StreamingProtectionReason::EditorSelection;
		case StreamingNeedType::InspectorPreview: return StreamingProtectionReason::Preview;
		case StreamingNeedType::PrefabEditing: return StreamingProtectionReason::PrefabEditing;
		case StreamingNeedType::AssetPreview: return StreamingProtectionReason::Preview;
		case StreamingNeedType::ActiveUi: return StreamingProtectionReason::ActiveUi;
		case StreamingNeedType::RuntimeExplicit: return StreamingProtectionReason::ExplicitPreload;
		case StreamingNeedType::FallbackDefault: return StreamingProtectionReason::FallbackResource;
		case StreamingNeedType::BackendInFlight: return StreamingProtectionReason::BackendInFlight;
		}
		return StreamingProtectionReason::None;
	}

	bool IsProtectedNeed(StreamingNeedType needType, const StreamingPolicy& policy)
	{
		const StreamingPolicy sanitized = SanitizeStreamingPolicy(policy);
		return sanitized.protectedNeedTypes.find(needType) != sanitized.protectedNeedTypes.end();
	}

	StreamablePayloadState StateAfterRegistration(bool deferScenePayloads)
	{
		return deferScenePayloads ? StreamablePayloadState::Deferred : StreamablePayloadState::Registered;
	}

	StreamablePayloadState StateAfterLoadResult(bool success, bool requiresGpuUpload)
	{
		if (!success)
		{
			return StreamablePayloadState::Failed;
		}
		return requiresGpuUpload ? StreamablePayloadState::UploadingVram : StreamablePayloadState::RamResident;
	}

	std::string_view ToString(StreamableAssetType type)
	{
		switch (type)
		{
		case StreamableAssetType::Mesh: return "mesh";
		case StreamableAssetType::Material: return "material";
		case StreamableAssetType::Texture: return "texture";
		case StreamableAssetType::ShaderCollection: return "shader-collection";
		case StreamableAssetType::Other: return "other";
		}
		return "other";
	}

	std::string_view ToString(StreamablePayloadState state)
	{
		switch (state)
		{
		case StreamablePayloadState::Registered: return "registered";
		case StreamablePayloadState::Deferred: return "deferred";
		case StreamablePayloadState::LoadingRam: return "loading-ram";
		case StreamablePayloadState::RamResident: return "ram-resident";
		case StreamablePayloadState::UploadingVram: return "uploading-vram";
		case StreamablePayloadState::VramResident: return "vram-resident";
		case StreamablePayloadState::PendingRelease: return "pending-release";
		case StreamablePayloadState::Released: return "released";
		case StreamablePayloadState::Failed: return "failed";
		}
		return "registered";
	}

	std::string_view ToString(StreamingNeedType type)
	{
		switch (type)
		{
		case StreamingNeedType::Visible: return "visible";
		case StreamingNeedType::NearVisible: return "near-visible";
		case StreamingNeedType::DistancePreload: return "distance-preload";
		case StreamingNeedType::RulePreload: return "rule-preload";
		case StreamingNeedType::EditorSelection: return "editor-selection";
		case StreamingNeedType::InspectorPreview: return "inspector-preview";
		case StreamingNeedType::PrefabEditing: return "prefab-editing";
		case StreamingNeedType::AssetPreview: return "asset-preview";
		case StreamingNeedType::ActiveUi: return "active-ui";
		case StreamingNeedType::RuntimeExplicit: return "runtime-explicit";
		case StreamingNeedType::FallbackDefault: return "fallback-default";
		case StreamingNeedType::BackendInFlight: return "backend-in-flight";
		}
		return "visible";
	}

	std::string_view ToString(StreamingProtectionReason reason)
	{
		switch (reason)
		{
		case StreamingProtectionReason::None: return "none";
		case StreamingProtectionReason::DefaultResource: return "default-resource";
		case StreamingProtectionReason::FallbackResource: return "fallback-resource";
		case StreamingProtectionReason::EditorSelection: return "editor-selection";
		case StreamingProtectionReason::PrefabEditing: return "prefab-editing";
		case StreamingProtectionReason::Preview: return "preview";
		case StreamingProtectionReason::ActiveUi: return "active-ui";
		case StreamingProtectionReason::ExplicitPreload: return "explicit-preload";
		case StreamingProtectionReason::InFlightLoad: return "in-flight-load";
		case StreamingProtectionReason::BackendInFlight: return "backend-in-flight";
		case StreamingProtectionReason::SharedUse: return "shared-use";
		case StreamingProtectionReason::ActiveView: return "active-view";
		case StreamingProtectionReason::PreloadRule: return "preload-rule";
		}
		return "none";
	}

	std::string_view ToString(PreloadCriteriaType type)
	{
		switch (type)
		{
		case PreloadCriteriaType::Distance: return "distance";
		case PreloadCriteriaType::Region: return "region";
		case PreloadCriteriaType::ObjectGroup: return "object-group";
		case PreloadCriteriaType::AssetTag: return "asset-tag";
		case PreloadCriteriaType::PriorityBand: return "priority-band";
		case PreloadCriteriaType::TraversalPath: return "traversal-path";
		case PreloadCriteriaType::ExplicitRequest: return "explicit-request";
		}
		return "distance";
	}

	std::string_view ToString(MemoryBudgetInteraction interaction)
	{
		switch (interaction)
		{
		case MemoryBudgetInteraction::UseResidencyPolicy: return "use-residency-policy";
		case MemoryBudgetInteraction::PreferStreamingRelease: return "prefer-streaming-release";
		case MemoryBudgetInteraction::Conservative: return "conservative";
		}
		return "use-residency-policy";
	}

	std::string_view ToString(StreamingPriorityClass priorityClass)
	{
		switch (priorityClass)
		{
		case StreamingPriorityClass::Background: return "background";
		case StreamingPriorityClass::Normal: return "normal";
		case StreamingPriorityClass::Important: return "important";
		case StreamingPriorityClass::Critical: return "critical";
		}
		return "normal";
	}

	std::string_view ToString(StreamingResidencyTier residencyTier)
	{
		switch (residencyTier)
		{
		case StreamingResidencyTier::Streamed: return "streamed";
		case StreamingResidencyTier::Warm: return "warm";
		case StreamingResidencyTier::Persistent: return "persistent";
		}
		return "streamed";
	}
}
