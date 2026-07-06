// ResourceStreaming.cpp - deterministic visibility streaming helper logic.
//
// These helpers are deliberately pure or close to pure. ResourceManager owns
// synchronized cache mutation, while renderer/scene systems can build
// candidate lists on worker tasks and feed compact needs back to the manager.

#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

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

		bool PageCoversLod(const VirtualGeometryStreamingPage& page, std::uint32_t lodIndex)
		{
			if (page.lodCount == 0u)
			{
				return false;
			}
			const std::uint64_t first = page.firstLodIndex;
			const std::uint64_t lastExclusive = first + page.lodCount;
			return static_cast<std::uint64_t>(lodIndex) >= first && static_cast<std::uint64_t>(lodIndex) < lastExclusive;
		}

		const VirtualGeometryStreamingPage* FindPageById(const std::vector<VirtualGeometryStreamingPage>& pages, std::uint32_t pageId)
		{
			for (const VirtualGeometryStreamingPage& page : pages)
			{
				if (page.pageId == pageId)
				{
					return &page;
				}
			}
			return nullptr;
		}

		const VirtualGeometryPageCatalogEntry* FindPageCatalog(
			const std::vector<VirtualGeometryPageCatalogEntry>& catalogs,
			const StreamableAssetId& assetId)
		{
			for (const VirtualGeometryPageCatalogEntry& catalog : catalogs)
			{
				if (catalog.assetId == assetId)
				{
					return &catalog;
				}
			}
			return nullptr;
		}

		void AppendVirtualGeometryPageNeed(
			std::vector<StreamingNeed>& needs,
			const VirtualGeometryPageNeedInput& input,
			const VirtualGeometryStreamingPage& page,
			StreamingNeedType needType,
			std::int32_t priority,
			double expiresAtSeconds,
			std::string reason)
		{
			StreamingNeed need;
			need.needId = "virtual-geometry-page:" + std::to_string(page.pageId);
			need.assetId = input.assetId;
			need.needType = needType;
			need.viewId = input.viewId;
			need.objectId = input.objectId;
			need.expiresAtSeconds = expiresAtSeconds;
			need.priority = priority;
			need.reason = std::move(reason);
			needs.push_back(std::move(need));
		}

		void AppendVirtualGeometryPageRequestPacket(
			std::vector<VirtualGeometryPageRequestPacket>& packets,
			const VirtualGeometryPageNeedInput& input,
			const VirtualGeometryStreamingPage& page,
			bool fallbackRequest,
			std::int32_t priority,
			std::string reason)
		{
			VirtualGeometryPageRequestPacket packet;
			packet.assetId = input.assetId;
			packet.objectId = input.objectId;
			packet.viewId = input.viewId;
			packet.pageId = page.pageId;
			packet.clusterId = input.desiredClusterId;
			packet.desiredLodIndex = fallbackRequest ? page.firstLodIndex : input.desiredLodIndex;
			packet.packetId = input.packetId;
			packet.batchId = input.batchId;
			packet.packetOffset = input.packetOffset;
			packet.packetCount = input.packetCount;
			packet.priority = priority;
			packet.fallbackRequest = fallbackRequest;
			packet.payloadSource = page.payloadSource != ::VirtualGeometryPagePayloadSource::Unknown
				? page.payloadSource
				: (fallbackRequest ? input.fallbackPayloadSource : input.desiredPayloadSource);
			packet.expectedEmission = fallbackRequest
				? ::VirtualGeometryEmittedPageSource::FallbackPage
				: ::VirtualGeometryEmittedPageSource::HighDetailPage;
			packet.payloadByteOffset = page.payloadByteOffset;
			packet.payloadByteSize = page.payloadByteSize == 0u ? page.byteSize : page.payloadByteSize;
			packet.payloadBufferIndex = page.payloadBufferIndex;
			packet.payloadDescriptorIndex = page.payloadDescriptorIndex;
			packet.reason = std::move(reason);
			packets.push_back(std::move(packet));
		}

		void AppendVirtualGeometryPageRequestFailure(
			VirtualGeometryPageRequestPlan& plan,
			const VirtualGeometryPageNeedInput& input,
			std::uint32_t pageId,
			std::uint32_t fallbackPageId,
			const VirtualGeometryStreamingPage* fallbackPage,
			std::string reason,
			bool fallbackFailure)
		{
			VirtualGeometryPageRequestFailureRecord failure;
			failure.assetId = input.assetId;
			failure.objectId = input.objectId;
			failure.viewId = input.viewId;
			failure.pageId = pageId;
			failure.fallbackPageId = fallbackPageId;
			failure.clusterId = input.desiredClusterId;
			failure.desiredLodIndex = input.desiredLodIndex;
			failure.fallbackAvailable = fallbackPage != nullptr && !fallbackPage->failed;
			failure.fallbackResident = fallbackPage != nullptr && fallbackPage->resident && !fallbackPage->failed;
			failure.reason = std::move(reason);
			if (fallbackFailure)
			{
				++plan.unavailableFallbackPages;
			}
			else
			{
				++plan.missingDesiredPages;
			}
			plan.failures.push_back(std::move(failure));
		}

			void SortVirtualGeometryPageRequestPackets(std::vector<VirtualGeometryPageRequestPacket>& packets)
			{
				std::sort(packets.begin(), packets.end(), [](const VirtualGeometryPageRequestPacket& left, const VirtualGeometryPageRequestPacket& right) {
					if (left.priority != right.priority)
					{
					return left.priority > right.priority;
				}
				if (left.assetId != right.assetId)
				{
					return UuidLess(left.assetId, right.assetId);
				}
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
					return left.fallbackRequest && !right.fallbackRequest;
				});
			}

			bool PageRequestEquivalent(const VirtualGeometryPageRequestPacket& left, const VirtualGeometryPageRequestPacket& right)
			{
				return left.assetId == right.assetId
					&& left.objectId == right.objectId
					&& left.viewId == right.viewId
					&& left.pageId == right.pageId
					&& left.fallbackRequest == right.fallbackRequest;
			}

			void CoalesceVirtualGeometryPageRequestPackets(std::vector<VirtualGeometryPageRequestPacket>& packets)
			{
				std::vector<VirtualGeometryPageRequestPacket> merged;
				merged.reserve(packets.size());
				for (VirtualGeometryPageRequestPacket packet : packets)
				{
					auto existing = std::find_if(merged.begin(), merged.end(), [&](const VirtualGeometryPageRequestPacket& candidate) {
						return PageRequestEquivalent(candidate, packet);
					});
					if (existing == merged.end())
					{
						merged.push_back(std::move(packet));
						continue;
					}

					// Multiple visible clusters can ask for the same page in one
					// frame. Keep the strongest priority and widest packet range so
					// the protected ResourceManager handoff performs one IO request
					// without losing diagnostics about the visible work that needed it.
					existing->priority = std::max(existing->priority, packet.priority);
					existing->packetOffset = std::min(existing->packetOffset, packet.packetOffset);
					existing->packetCount = std::max(existing->packetCount, packet.packetCount);
					existing->payloadByteSize = std::max(existing->payloadByteSize, packet.payloadByteSize);
					// Preserve a concrete shader-visible binding when any
					// coalesced request already reached the protected upload
					// publication boundary. Invalid values remain invalid so a
					// queued request cannot pretend that metadata is renderable.
					if (existing->payloadBufferIndex == 0xffffffffu)
					{
						existing->payloadBufferIndex = packet.payloadBufferIndex;
					}
					if (existing->payloadDescriptorIndex == 0xffffffffu)
					{
						existing->payloadDescriptorIndex = packet.payloadDescriptorIndex;
					}
					if (existing->reason.find(packet.reason) == std::string::npos)
					{
						existing->reason += "; " + packet.reason;
					}
				}
				packets = std::move(merged);
				SortVirtualGeometryPageRequestPackets(packets);
			}

		std::int32_t ScoreVirtualGeometryPagePriority(
			const VirtualGeometryPageNeedInput& input,
			const VirtualGeometryStreamingPage& page,
			StreamingPriorityClass priorityClass)
		{
			// Page request scoring is pure data math. ResourceManager still owns
			// the later protected cache mutation, IO dispatch, and GPU upload
			// publication boundary.
			const float importance = std::isfinite(input.screenImportance) ? std::clamp(input.screenImportance, 0.0f, 1.0f) : 0.0f;
			const float predicted = std::isfinite(input.predictedNeed) ? std::clamp(input.predictedNeed, 0.0f, 1.0f) : 0.0f;
			const std::int32_t budgetBoost = input.budgetPressureBytes > 0u
				? 25 + static_cast<std::int32_t>(std::min<std::uint64_t>(75u, input.budgetPressureBytes / (1024u * 1024u)))
				: 0;
			return page.priority
				+ input.assetPriority
				+ PriorityClassBias(priorityClass)
				+ static_cast<std::int32_t>(importance * 100.0f)
				+ static_cast<std::int32_t>(predicted * 50.0f)
				+ budgetBoost;
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
			policy.textureMipStreamingEnabled = true;
			policy.textureMinStreamMip = 0u;
			policy.textureMaxStreamMip = 13u;
			policy.virtualGeometryPageMemoryBudgetBytes = 0u;
			policy.virtualGeometryPageUploadBudgetBytes = 0u;
			policy.virtualGeometryPageRetirementBudgetBytes = 0u;
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

	std::vector<VirtualGeometryPageNeedInput> BuildVirtualGeometryPageNeedInputsFromSelectionStates(
		const std::vector<TerraEngine::Renderer::VirtualGeometryViewSelectionState>& states,
		const std::vector<VirtualGeometryPageCatalogEntry>& catalogs,
		float preloadBias)
	{
		const float finitePreloadBias = std::isfinite(preloadBias) ? std::clamp(preloadBias, 0.0f, 16.0f) : 1.0f;
		std::vector<VirtualGeometryPageNeedInput> inputs;
		for (const TerraEngine::Renderer::VirtualGeometryViewSelectionState& state : states)
		{
			const std::string viewId = std::string(::ToString(state.viewType)) + ":" + std::to_string(state.outputId);
			for (const TerraEngine::Renderer::VirtualGeometryPageNeedRecord& need : state.pageNeeds)
			{
				const VirtualGeometryPageCatalogEntry* catalog = FindPageCatalog(catalogs, need.meshId);
				if (catalog == nullptr || catalog->pages.empty())
				{
					continue;
				}

				VirtualGeometryPageNeedInput input;
				input.assetId = need.meshId;
				input.objectId = need.objectId;
				input.viewId = viewId;
				input.desiredLodIndex = need.desiredLodIndex;
				input.desiredClusterId = need.clusterId;
				input.packetId = need.packetId;
				input.batchId = need.batchId;
				input.packetOffset = need.packetOffset;
				input.packetCount = need.packetCount;
				input.screenImportance = std::isfinite(need.screenImportance)
					? std::clamp(need.screenImportance, 0.0f, 1.0f)
					: 0.0f;
				// The render snapshot carries current screen importance; applying
				// preload bias turns that into a short-term prediction without
				// reading camera objects or mutable scene state from workers.
				input.predictedNeed = std::clamp(input.screenImportance * finitePreloadBias, 0.0f, 1.0f);
				input.assetPriority = catalog->assetPriority;
				input.budgetPressureBytes = catalog->budgetPressureBytes;
				input.fallbackRequired = need.fallbackRequired;
				input.desiredPayloadSource = need.desiredPayloadSource;
				input.fallbackPayloadSource = need.fallbackPayloadSource;
				input.pages = catalog->pages;
				inputs.push_back(std::move(input));
			}
		}
		return inputs;
	}

	std::vector<StreamingNeed> BuildVirtualGeometryPageStreamingNeeds(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy, double nowSeconds)
	{
		const StreamingPolicy sanitized = SanitizeStreamingPolicy(policy);
		if (!sanitized.enabled)
		{
			return {};
		}

		std::vector<StreamingNeed> needs;
		for (const VirtualGeometryPageNeedInput& input : inputs)
		{
			if (input.assetId == TerraEngine::EMPTY_UUID || input.pages.empty())
			{
				continue;
			}

			const double retention = input.retentionSeconds > 0.0 ? input.retentionSeconds : sanitized.neededRetentionSeconds;
			const double expiresAtSeconds = retention > 0.0 && nowSeconds > 0.0 ? nowSeconds + retention : 0.0;
			const VirtualGeometryStreamingPage* desiredPage = nullptr;
			for (const VirtualGeometryStreamingPage& page : input.pages)
			{
				if (PageCoversLod(page, input.desiredLodIndex))
				{
					desiredPage = &page;
					break;
				}
			}
			if (desiredPage == nullptr)
			{
				continue;
			}

			if (!desiredPage->resident && !desiredPage->failed)
			{
				AppendVirtualGeometryPageNeed(
					needs,
					input,
					*desiredPage,
					StreamingNeedType::RuntimeExplicit,
					ScoreVirtualGeometryPagePriority(input, *desiredPage, StreamingPriorityClass::Important),
					expiresAtSeconds,
					"virtual geometry desired LOD page");
			}

			// Desired pages improve quality, but fallback pages protect
			// visibility. Keeping them as separate needs lets the scheduler load
			// the fallback first when memory or IO pressure forces a tradeoff.
			const VirtualGeometryStreamingPage* fallbackPage = FindPageById(input.pages, desiredPage->fallbackPageId);
			if (input.fallbackRequired && fallbackPage != nullptr && !fallbackPage->resident && !fallbackPage->failed)
			{
				AppendVirtualGeometryPageNeed(
					needs,
					input,
					*fallbackPage,
					StreamingNeedType::FallbackDefault,
					ScoreVirtualGeometryPagePriority(input, *fallbackPage, StreamingPriorityClass::Critical),
					expiresAtSeconds,
					"virtual geometry fallback page");
			}
		}

		return MergeStreamingNeeds(std::move(needs), nowSeconds);
	}

	VirtualGeometryPageRequestPlan BuildVirtualGeometryPageRequestPlan(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy)
	{
		const StreamingPolicy sanitized = SanitizeStreamingPolicy(policy);
		VirtualGeometryPageRequestPlan plan;
		if (!sanitized.enabled)
		{
			return plan;
		}

		for (const VirtualGeometryPageNeedInput& input : inputs)
		{
			if (input.assetId == TerraEngine::EMPTY_UUID)
			{
				continue;
			}
			if (input.pages.empty())
			{
				AppendVirtualGeometryPageRequestFailure(
					plan,
					input,
					0xffffffffu,
					0xffffffffu,
					nullptr,
					"virtual geometry page catalog is empty",
					false);
				continue;
			}

			const VirtualGeometryStreamingPage* desiredPage = nullptr;
			for (const VirtualGeometryStreamingPage& page : input.pages)
			{
				if (PageCoversLod(page, input.desiredLodIndex))
				{
					desiredPage = &page;
					break;
				}
			}
			if (desiredPage == nullptr)
			{
				AppendVirtualGeometryPageRequestFailure(
					plan,
					input,
					0xffffffffu,
					0xffffffffu,
					nullptr,
					"desired virtual geometry LOD has no streamable page",
					false);
				continue;
			}

			// Request packets are immutable handoff records. The worker-safe
			// planner can build them in bulk, while ResourceManager and the RHI
			// remain responsible for protected cache mutation and GPU upload.
			const VirtualGeometryStreamingPage* fallbackPage = FindPageById(input.pages, desiredPage->fallbackPageId);
			if (desiredPage->failed)
			{
				AppendVirtualGeometryPageRequestFailure(
					plan,
					input,
					desiredPage->pageId,
					desiredPage->fallbackPageId,
					fallbackPage,
					"desired virtual geometry page failed; fallback must preserve visibility",
					false);
			}
			else if (!desiredPage->resident)
			{
				AppendVirtualGeometryPageRequestPacket(
					plan.packets,
					input,
					*desiredPage,
					false,
					ScoreVirtualGeometryPagePriority(input, *desiredPage, StreamingPriorityClass::Important),
					"virtual geometry desired page async IO request");
			}

			if (!input.fallbackRequired)
			{
				continue;
			}

			if (fallbackPage == nullptr)
			{
				AppendVirtualGeometryPageRequestFailure(
					plan,
					input,
					desiredPage->pageId,
					desiredPage->fallbackPageId,
					nullptr,
					"virtual geometry fallback page is missing from the catalog",
					true);
				continue;
			}
			if (fallbackPage->failed)
			{
				AppendVirtualGeometryPageRequestFailure(
					plan,
					input,
					fallbackPage->pageId,
					desiredPage->fallbackPageId,
					fallbackPage,
					"virtual geometry fallback page failed",
					true);
				continue;
			}
			if (!fallbackPage->resident)
			{
				AppendVirtualGeometryPageRequestPacket(
					plan.packets,
					input,
					*fallbackPage,
					true,
					ScoreVirtualGeometryPagePriority(input, *fallbackPage, StreamingPriorityClass::Critical),
					"virtual geometry fallback page async IO request");
			}
		}

			CoalesceVirtualGeometryPageRequestPackets(plan.packets);
			return plan;
		}

	std::vector<VirtualGeometryPageRequestPacket> BuildVirtualGeometryPageRequestPackets(const std::vector<VirtualGeometryPageNeedInput>& inputs, const StreamingPolicy& policy)
	{
		return BuildVirtualGeometryPageRequestPlan(inputs, policy).packets;
	}

	VirtualGeometryPageRequestQueuePlan BuildVirtualGeometryPageRequestQueuePlan(
		const VirtualGeometryPageRequestPlan& requestPlan,
		const std::vector<VirtualGeometryQueuedPageRequest>& previousQueue,
		const VirtualGeometryPageRetryPolicy& retryPolicy,
		std::uint64_t frameId)
	{
		VirtualGeometryPageRequestQueuePlan queuePlan;
		const std::uint32_t maxRetryCount = std::max(1u, retryPolicy.maxRetryCount);
		const std::uint64_t retryDelayFrames = std::max<std::uint64_t>(1u, retryPolicy.retryDelayFrames);
			auto findQueued = [&](const VirtualGeometryPageRequestPacket& packet) {
				return std::find_if(queuePlan.queuedRequests.begin(), queuePlan.queuedRequests.end(), [&](const VirtualGeometryQueuedPageRequest& candidate) {
					return PageRequestEquivalent(candidate.packet, packet);
				});
			};
			auto pushReady = [&](VirtualGeometryQueuedPageRequest queued, bool retry) {
				queued.nextRetryFrame = frameId + retryDelayFrames;
				queuePlan.readyPackets.push_back(queued.packet);
				queuePlan.queuedRequests.push_back(std::move(queued));
				queuePlan.retriedPackets += retry ? 1u : 0u;
			};

		for (const VirtualGeometryQueuedPageRequest& previous : previousQueue)
		{
			if (previous.retryCount >= maxRetryCount)
			{
				queuePlan.droppedRequests.push_back(previous);
				continue;
			}
			if (previous.nextRetryFrame > frameId)
			{
				queuePlan.queuedRequests.push_back(previous);
				continue;
			}
			VirtualGeometryQueuedPageRequest retry = previous;
			++retry.retryCount;
			pushReady(std::move(retry), true);
		}

			for (const VirtualGeometryPageRequestPacket& packet : requestPlan.packets)
			{
				if (auto existing = findQueued(packet); existing != queuePlan.queuedRequests.end())
				{
					existing->packet.priority = std::max(existing->packet.priority, packet.priority);
					existing->packet.reason += "; coalesced visible request";
					continue;
				}
				VirtualGeometryQueuedPageRequest queued;
				queued.packet = packet;
				queued.retryCount = 0u;
				queued.nextRetryFrame = frameId + retryDelayFrames;
				queuePlan.readyPackets.push_back(packet);
			queuePlan.queuedRequests.push_back(std::move(queued));
			queuePlan.fallbackPackets += packet.fallbackRequest ? 1u : 0u;
		}

			CoalesceVirtualGeometryPageRequestPackets(queuePlan.readyPackets);
			return queuePlan;
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

	TerraEngine::Scheduling::FrameWorkloadDeclaration BuildResourcePreparationWorkloadDeclaration(std::size_t estimatedNeedCount)
	{
		TerraEngine::Scheduling::FrameWorkloadDeclaration declaration = TerraEngine::Scheduling::MakeWorkerPreparePhase(
			"ResourceCandidatePlanning",
			"ResourceStreaming",
			{ "RenderVisibility", "StreamingPolicy", "PreloadRules" },
			{ "StreamingNeeds" },
			"ResourceStreaming: Candidate Planning");
		declaration.estimatedWorkItemCount = estimatedNeedCount;
		declaration.inlineWorkItemThreshold = 16u;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe;
		declaration.safeSubwork = "Dependency discovery, preload-rule evaluation, virtual geometry page need calculation, and residency candidate sorting on immutable inputs.";
		declaration.protectedCommit = "ResourceManager registration, cache mutation, GPU upload scheduling, virtual geometry page publication, and residency publication.";
		declaration.protectedReason = declaration.protectedCommit;
		return declaration;
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

	std::string_view ToString(ExactResidencyByteStatus status)
	{
		switch (status)
		{
		case ExactResidencyByteStatus::Exact: return "exact";
		case ExactResidencyByteStatus::ZeroByState: return "zero-by-state";
		case ExactResidencyByteStatus::Unavailable: return "unavailable";
		}
		return "unavailable";
	}
}
