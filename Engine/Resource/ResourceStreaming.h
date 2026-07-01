// ResourceStreaming.h - visibility-driven resource streaming contracts.
//
// The types in this file describe why an asset should load, stay resident, or
// become eligible for the existing residency cleanup system. The streaming
// layer intentionally does not destroy CPU or GPU payloads directly; it only
// publishes deterministic state that ResourceManager, ResourceResidency, and
// renderer backends can consume at safe synchronization points.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "Core/Export.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Utilities/UUID.h"

namespace TerraEngine::Resource
{
	using StreamableAssetId = TerraEngine::UUID;

	enum class StreamableAssetType : std::uint8_t
	{
		Mesh,
		Material,
		Texture,
		ShaderCollection,
		Other
	};

	enum class StreamablePayloadState : std::uint8_t
	{
		Registered,
		Deferred,
		LoadingRam,
		RamResident,
		UploadingVram,
		VramResident,
		PendingRelease,
		Released,
		Failed
	};

	enum class StreamingNeedType : std::uint8_t
	{
		Visible,
		NearVisible,
		DistancePreload,
		RulePreload,
		EditorSelection,
		InspectorPreview,
		PrefabEditing,
		AssetPreview,
		ActiveUi,
		RuntimeExplicit,
		FallbackDefault,
		BackendInFlight
	};

	enum class StreamingProtectionReason : std::uint8_t
	{
		None,
		DefaultResource,
		FallbackResource,
		EditorSelection,
		PrefabEditing,
		Preview,
		ActiveUi,
		ExplicitPreload,
		InFlightLoad,
		BackendInFlight,
		SharedUse,
		ActiveView,
		PreloadRule
	};

	enum class StreamingViewType : std::uint8_t
	{
		Scene,
		Game,
		Runtime,
		Prefab,
		AssetPreview,
		Ui
	};

	enum class PreloadCriteriaType : std::uint8_t
	{
		Distance,
		Region,
		ObjectGroup,
		AssetTag,
		PriorityBand,
		TraversalPath,
		ExplicitRequest
	};

	enum class MemoryBudgetInteraction : std::uint8_t
	{
		UseResidencyPolicy,
		PreferStreamingRelease,
		Conservative
	};

	enum class StreamingPriorityClass : std::uint8_t
	{
		Background,
		Normal,
		Important,
		Critical
	};

	enum class StreamingResidencyTier : std::uint8_t
	{
		Streamed,
		Warm,
		Persistent
	};

	struct StreamingAssetPolicy
	{
		StreamingPriorityClass priorityClass = StreamingPriorityClass::Normal;
		StreamingResidencyTier residencyTier = StreamingResidencyTier::Streamed;
		std::int32_t priorityBias = 0;
	};

	struct StreamingNeed
	{
		std::string needId;
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		StreamingNeedType needType = StreamingNeedType::Visible;
		std::string viewId;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		double expiresAtSeconds = 0.0;
		std::int32_t priority = 0;
		std::string reason;
	};

	struct VirtualGeometryStreamingPage
	{
		std::uint32_t pageId = 0u;
		std::uint32_t firstLodIndex = 0u;
		std::uint32_t lodCount = 0u;
		std::uint32_t fallbackPageId = 0u;
		std::uint64_t byteSize = 0u;
		std::int32_t priority = 0;
		bool resident = false;
		bool failed = false;
		std::uint32_t clusterId = 0u;
	};

	struct VirtualGeometryPageNeedInput
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string viewId;
		std::uint32_t desiredLodIndex = 0u;
		std::uint32_t desiredClusterId = 0u;
		double retentionSeconds = 0.0;
		float screenImportance = 0.0f;
		float predictedNeed = 0.0f;
		std::int32_t assetPriority = 0;
		std::uint64_t budgetPressureBytes = 0u;
		bool fallbackRequired = true;
		std::vector<VirtualGeometryStreamingPage> pages;
	};

	struct VirtualGeometryPageRequestPacket
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string viewId;
		std::uint32_t pageId = 0u;
		std::uint32_t clusterId = 0u;
		std::uint32_t desiredLodIndex = 0u;
		std::int32_t priority = 0;
		bool fallbackRequest = false;
		std::string reason;
	};

	struct VirtualGeometryPageRequestFailureRecord
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string viewId;
		std::uint32_t pageId = 0xffffffffu;
		std::uint32_t fallbackPageId = 0xffffffffu;
		std::uint32_t clusterId = 0u;
		std::uint32_t desiredLodIndex = 0u;
		bool fallbackAvailable = false;
		bool fallbackResident = false;
		std::string reason;
	};

	struct VirtualGeometryPageRequestPlan
	{
		std::vector<VirtualGeometryPageRequestPacket> packets;
		std::vector<VirtualGeometryPageRequestFailureRecord> failures;
		std::uint32_t missingDesiredPages = 0u;
		std::uint32_t unavailableFallbackPages = 0u;
	};

	struct VirtualGeometryPageCatalogEntry
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		std::vector<VirtualGeometryStreamingPage> pages;
		std::int32_t assetPriority = 0;
		std::uint64_t budgetPressureBytes = 0u;
	};

	struct StreamingView
	{
		std::string viewId;
		StreamingViewType viewType = StreamingViewType::Scene;
		glm::vec3 position{ 0.0f };
		float preloadDistance = 0.0f;
		bool active = true;
	};

	struct StreamingPolicy
	{
		bool enabled = true;
		bool deferScenePayloadsOnLoad = true;
		double preloadDistance = 75.0;
		double visibleRetentionSeconds = 2.0;
		double neededRetentionSeconds = 5.0;
		std::uint32_t maxLoadsPerUpdate = 0;
		bool textureMipStreamingEnabled = false;
		// Texture streaming uses source mip indices. Mip 0 is the authored
		// full-resolution image; larger indices are lower-detail tail mips.
		// Near visible textures start at textureMinStreamMip, while far visible
		// textures can start at textureMaxStreamMip.
		std::uint32_t textureMinStreamMip = 0;
		std::uint32_t textureMaxStreamMip = 0;
		MemoryBudgetInteraction memoryBudgetInteraction = MemoryBudgetInteraction::UseResidencyPolicy;
		bool diagnosticsEnabled = false;
		std::unordered_set<StreamingNeedType> protectedNeedTypes;
	};

	struct TextureMipResidency
	{
		std::uint32_t sourceMipOffset = 0;
		std::uint32_t residentMipCount = 0;
		std::uint32_t sourceMipCount = 0;
		bool partial = false;
	};

	struct PreloadRule
	{
		std::string ruleId;
		bool enabled = true;
		PreloadCriteriaType criteriaType = PreloadCriteriaType::Distance;
		std::string criteriaValue;
		std::vector<StreamableAssetId> assetScope;
		std::int32_t priority = 0;
		double graceSeconds = 0.0;
		std::string diagnosticName;
	};

	struct StreamableAssetRecord
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		StreamableAssetType assetType = StreamableAssetType::Other;
		std::string sourceKey;
		StreamablePayloadState payloadState = StreamablePayloadState::Registered;
		double lastReferencedSeconds = 0.0;
		double lastVisibleSeconds = 0.0;
		double lastNeededSeconds = 0.0;
		double lastReleaseEligibleSeconds = 0.0;
		std::vector<StreamingNeed> needSources;
		StreamingProtectionReason protectionReason = StreamingProtectionReason::None;
		StreamingAssetPolicy assetPolicy;
		bool assetPolicyExplicit = false;
		std::uint32_t textureSourceMipCount = 0;
		std::uint32_t textureResidentMipCount = 0;
		std::uint32_t textureResidentMipOffset = 0;
		double textureClosestViewDistance = 0.0;
		double textureDistanceUpdatedSeconds = 0.0;
		std::uint64_t estimatedCpuBytes = 0;
		std::uint64_t estimatedGpuBytes = 0;
		std::string failureReason;
		std::uint64_t residencyGeneration = 0;
	};

	struct StreamingDiagnosticRecord
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		StreamableAssetType assetType = StreamableAssetType::Other;
		StreamablePayloadState payloadState = StreamablePayloadState::Registered;
		double lastVisibleSeconds = 0.0;
		double lastNeededSeconds = 0.0;
		std::vector<StreamingNeed> needSources;
		StreamingProtectionReason protectionReason = StreamingProtectionReason::None;
		StreamingAssetPolicy assetPolicy;
		std::uint32_t textureSourceMipCount = 0;
		std::uint32_t textureResidentMipCount = 0;
		std::uint32_t textureResidentMipOffset = 0;
		double textureClosestViewDistance = 0.0;
		std::string releaseEligibility;
		std::string failureReason;
		std::uint64_t estimatedCpuBytes = 0;
		std::uint64_t estimatedGpuBytes = 0;
	};

	struct StreamingDiagnosticsSummary
	{
		std::uint64_t frameId = 0;
		std::size_t trackedAssetCount = 0;
		std::size_t deferredAssetCount = 0;
		std::size_t loadingAssetCount = 0;
		std::size_t residentAssetCount = 0;
		std::size_t protectedAssetCount = 0;
		std::size_t releaseEligibleCount = 0;
		std::size_t failedAssetCount = 0;
		std::uint64_t estimatedResidentBytes = 0;
		std::size_t activeNeedCount = 0;
		bool diagnosticsEnabled = false;
		std::vector<StreamingDiagnosticRecord> records;
	};

	TERRAENGINE_API StreamingPolicy MakeDefaultStreamingPolicy();
	TERRAENGINE_API StreamingPolicy SanitizeStreamingPolicy(const StreamingPolicy& policy);
	TERRAENGINE_API StreamingAssetPolicy MakeDefaultStreamingAssetPolicy(StreamableAssetType type);
	TERRAENGINE_API StreamingAssetPolicy SanitizeStreamingAssetPolicy(const StreamingAssetPolicy& policy);
	TERRAENGINE_API TextureMipResidency ResolveTextureMipResidency(const StreamingPolicy& policy, std::uint32_t sourceMipCount);
	TERRAENGINE_API std::int32_t PriorityClassBias(StreamingPriorityClass priorityClass);
	TERRAENGINE_API bool ValidatePreloadRule(const PreloadRule& rule, std::string* reason = nullptr);
	TERRAENGINE_API std::vector<StreamingNeed> EvaluatePreloadRules(const std::vector<PreloadRule>& rules, double nowSeconds = 0.0);
	TERRAENGINE_API std::vector<VirtualGeometryPageNeedInput> BuildVirtualGeometryPageNeedInputsFromSelectionStates(const std::vector<TerraEngine::Renderer::VirtualGeometryViewSelectionState>& states, const std::vector<VirtualGeometryPageCatalogEntry>& catalogs, float preloadBias = 1.0f);
	TERRAENGINE_API std::vector<StreamingNeed> BuildVirtualGeometryPageStreamingNeeds(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy, double nowSeconds = 0.0);
	TERRAENGINE_API VirtualGeometryPageRequestPlan BuildVirtualGeometryPageRequestPlan(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy);
	TERRAENGINE_API std::vector<VirtualGeometryPageRequestPacket> BuildVirtualGeometryPageRequestPackets(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy);
	TERRAENGINE_API std::vector<StreamingNeed> MergeStreamingNeeds(std::vector<StreamingNeed> needs, double nowSeconds = 0.0);
	// This declaration describes the worker-safe planning phase. The actual
	// ResourceManager registration and load/upload mutation still happens later
	// at a protected handoff point.
	TERRAENGINE_API TerraEngine::Scheduling::FrameWorkloadDeclaration BuildResourcePreparationWorkloadDeclaration(std::size_t estimatedNeedCount);
	TERRAENGINE_API StreamingProtectionReason ProtectionForNeed(StreamingNeedType needType);
	TERRAENGINE_API bool IsProtectedNeed(StreamingNeedType needType, const StreamingPolicy& policy);
	TERRAENGINE_API StreamablePayloadState StateAfterRegistration(bool deferScenePayloads);
	TERRAENGINE_API StreamablePayloadState StateAfterLoadResult(bool success, bool requiresGpuUpload);
	TERRAENGINE_API std::string_view ToString(StreamableAssetType type);
	TERRAENGINE_API std::string_view ToString(StreamablePayloadState state);
	TERRAENGINE_API std::string_view ToString(StreamingNeedType type);
	TERRAENGINE_API std::string_view ToString(StreamingProtectionReason reason);
	TERRAENGINE_API std::string_view ToString(PreloadCriteriaType type);
	TERRAENGINE_API std::string_view ToString(MemoryBudgetInteraction interaction);
	TERRAENGINE_API std::string_view ToString(StreamingPriorityClass priorityClass);
	TERRAENGINE_API std::string_view ToString(StreamingResidencyTier residencyTier);
}
