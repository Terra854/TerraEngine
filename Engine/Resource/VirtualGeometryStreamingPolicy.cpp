// VirtualGeometryStreamingPolicy.cpp - deterministic page request coalescing.

#include "VirtualGeometryStreamingPolicy.h"

#include <algorithm>
#include <map>

namespace TerraEngine::Resource
{
	VirtualGeometryStreamingPlan BuildVirtualGeometryStreamingPlan(
		std::vector<VirtualGeometryStreamingFeedback> feedback, const VirtualGeometryStreamingPolicy &policy)
	{
		VirtualGeometryStreamingPlan plan;
		using Key = std::pair<std::string, std::uint32_t>;
		std::map<Key, VirtualGeometryStreamingFeedback> coalesced;
		const auto normalizeDependencies = [](VirtualGeometryStreamingFeedback &value)
		{
			std::sort(value.dependencyPageIds.begin(), value.dependencyPageIds.end());
			value.dependencyPageIds.erase(
				std::unique(value.dependencyPageIds.begin(), value.dependencyPageIds.end()),
				value.dependencyPageIds.end());
		};
		const auto mergeFeedback = [&coalesced, &plan, &normalizeDependencies](VirtualGeometryStreamingFeedback value)
		{
			const Key key{ TerraEngine::ToHexString(value.assetId), value.pageId };
			// Keep value available when the key already exists; emplace may otherwise
			// consume the merge input before its priority and dependency proof merge.
			auto [entry, inserted] = coalesced.emplace(key, value);
			if (inserted)
			{
				return true;
			}

			++plan.duplicateFeedbackCount;
			VirtualGeometryStreamingFeedback &merged = entry->second;
			merged.priority = std::max(merged.priority, value.priority);
			merged.visible = merged.visible || value.visible;
			merged.observedFrame = std::max(merged.observedFrame, value.observedFrame);
			merged.expectedStoredByteCount = std::max(merged.expectedStoredByteCount, value.expectedStoredByteCount);
			merged.expectedDecodedByteCount = std::max(merged.expectedDecodedByteCount, value.expectedDecodedByteCount);
			merged.expectedIntegrityValue = std::max(merged.expectedIntegrityValue, value.expectedIntegrityValue);
			// A complete ordered block list is indivisible. Mixing blocks from two
			// feedback copies could combine descriptors from different cooked pages,
			// so deterministic coalescing keeps the first complete value-only list.
			if (merged.exactBlockReads.empty() && !value.exactBlockReads.empty())
			{
				merged.exactBlockReads = value.exactBlockReads;
				merged.quantizationDomains = value.quantizationDomains;
				merged.availableDecoderCapabilities = value.availableDecoderCapabilities;
			}
			merged.dependencyPageIds.insert(
				merged.dependencyPageIds.end(), value.dependencyPageIds.begin(), value.dependencyPageIds.end());
			normalizeDependencies(merged);
			return false;
		};

		for (VirtualGeometryStreamingFeedback &value : feedback)
		{
			if (value.assetId == TerraEngine::EMPTY_UUID
				|| value.pageId == kVirtualGeometryInvalidIndex
				|| (policy.requireFallback && value.fallbackPageId == kVirtualGeometryInvalidIndex))
			{
				++plan.rejectedFeedbackCount;
				continue;
			}

			normalizeDependencies(value);
			const bool containsInvalidDependency = std::any_of(
				value.dependencyPageIds.begin(), value.dependencyPageIds.end(), [&value](std::uint32_t dependencyPageId)
				{
					return dependencyPageId == kVirtualGeometryInvalidIndex || dependencyPageId == value.pageId;
				});
			if (containsInvalidDependency)
			{
				++plan.rejectedFeedbackCount;
				continue;
			}

			mergeFeedback(value);
			if (!policy.expandDependencies)
			{
				continue;
			}

			// A non-root page cannot be used safely until its authored fallback is
			// planned too. Derived records are value-only and retain the root's
			// generation/priority inputs; they never imply physical residency.
			if (value.fallbackPageId != value.pageId)
			{
				VirtualGeometryStreamingFeedback fallback = value;
				fallback.pageId = value.fallbackPageId;
				fallback.fallbackPageId = value.fallbackPageId;
				fallback.dependencyPageIds.clear();
				fallback.visible = true;
				// A root request's byte range cannot be reused for another physical
				// page. Its own catalog entry must supply any exact-range source.
				fallback.exactBlockReads.clear();
				fallback.quantizationDomains.clear();
				fallback.canonicalPageReadDiagnostic =
					"Dependency expansion requires an independently resolved compact range for the fallback page.";
				if (mergeFeedback(std::move(fallback)))
				{
					++plan.expandedDependencyCount;
				}
			}
			for (const std::uint32_t dependencyPageId : value.dependencyPageIds)
			{
				VirtualGeometryStreamingFeedback dependency = value;
				dependency.pageId = dependencyPageId;
				dependency.dependencyPageIds.clear();
				dependency.exactBlockReads.clear();
				dependency.quantizationDomains.clear();
				dependency.canonicalPageReadDiagnostic =
					"Dependency expansion requires an independently resolved compact range for this dependency page.";
				if (mergeFeedback(std::move(dependency)))
				{
					++plan.expandedDependencyCount;
				}
			}
		}
		for (const auto &[key, value] : coalesced)
		{
			(void)key;
			plan.requests.push_back(value);
		}
		std::sort(plan.requests.begin(),
			plan.requests.end(),
			[](const auto &left, const auto &right)
			{
				if (left.priority != right.priority)
					return left.priority > right.priority;
				if (left.visible != right.visible)
					return left.visible;
				if (left.observedFrame != right.observedFrame)
					return left.observedFrame > right.observedFrame;
				const std::string leftAsset = TerraEngine::ToHexString(left.assetId);
				const std::string rightAsset = TerraEngine::ToHexString(right.assetId);
				return leftAsset != rightAsset ? leftAsset < rightAsset : left.pageId < right.pageId;
			});
		if (policy.maximumRequests != 0u && plan.requests.size() > policy.maximumRequests)
		{
			plan.requests.resize(policy.maximumRequests);
		}
		plan.decodeIntents.reserve(plan.requests.size());
		plan.uploadIntents.reserve(plan.requests.size());
		for (const VirtualGeometryStreamingFeedback &request : plan.requests)
		{
			// These declarations make decode/upload ownership visible to diagnostics
			// without fabricating byte verification or a device allocation on workers.
			plan.decodeIntents.push_back({ request, true, true });
			plan.uploadIntents.push_back({ request, true, true });
		}
		return plan;
	}
} // namespace TerraEngine::Resource
