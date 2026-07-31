// VirtualGeometryStreamingPolicy.h - deterministic, worker-safe page request policy.

#pragma once

#include "Core/Export.h"
#include "Resource/ResourceStreaming.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	// This input is a copied prior-frame observation.  It contains no pointers
	// to ResourceManager or RHI state, so planning remains safe on a worker.
	struct VirtualGeometryStreamingFeedback
	{
		StreamableAssetId assetId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = kVirtualGeometryInvalidIndex;
		std::uint32_t fallbackPageId = kVirtualGeometryInvalidIndex;
		std::vector<std::uint32_t> dependencyPageIds;
		std::int32_t priority = 0;
		std::uint64_t observedFrame = 0u;
		bool visible = true;
		// These are planning inputs, not completion claims. A worker may use them
		// to reserve deterministic decode/upload work, but only a separately
		// verified publication candidate can make a page resident.
		std::uint64_t expectedStoredByteCount = 0u;
		std::uint64_t expectedDecodedByteCount = 0u;
		std::uint64_t expectedIntegrityValue = 0u;
		// A populated exact-range request authorizes worker IO. Raw identity decode
		// is explicit because arbitrary encoded bytes must never be called decoded.
		std::optional<VirtualGeometryStaticPageReadRequest> staticPageRead;
		bool rawPayloadIsDecoded = false;
		// Appended after the established aggregate-initialization fields so
		// existing request producers retain source compatibility.
		std::uint32_t clusterId = kVirtualGeometryInvalidIndex;
		// Diagnostics explain why an exact cooked byte range was or was not
		// attached while RenderSystem still owned the live mesh snapshot. This
		// string is copied into worker plans and never grants IO by itself.
		std::string staticPageReadDiagnostic;
	};

	struct VirtualGeometryStreamingPolicy
	{
		std::uint32_t maximumRequests = 0u;
		bool requireFallback = true;
		bool expandDependencies = true;
	};

	// Value-only decode intent emitted by worker-safe planning. The executor must
	// verify physical bytes and integrity before it creates a publication candidate.
	struct VirtualGeometryStreamingDecodeIntent
	{
		VirtualGeometryStreamingFeedback request{};
		bool requiresPhysicalRead = true;
		bool requiresIntegrityVerification = true;
	};

	// Value-only upload intent emitted after the corresponding decode intent. It
	// intentionally has no RHI handle so worker preparation cannot mutate device state.
	struct VirtualGeometryStreamingUploadIntent
	{
		VirtualGeometryStreamingFeedback request{};
		bool requiresVerifiedDecode = true;
		bool requiresProtectedUploadCommit = true;
	};

	enum class VirtualGeometryStreamingDecodeStatus : std::uint8_t
	{
		NotRequested,
		PendingDecode,
		PendingUpload,
		Cancelled,
		Failed
	};

	// Owned bytes remain in the immutable prepared plan until a backend-owned
	// upload path consumes them. PendingUpload is intentionally not residency.
	struct VirtualGeometryStreamingDecodedPage
	{
		VirtualGeometryStreamingFeedback request{};
		VirtualGeometryStreamingDecodeStatus status = VirtualGeometryStreamingDecodeStatus::NotRequested;
		std::vector<std::uint8_t> decodedBytes;
		std::string detail;
	};

	struct VirtualGeometryStreamingPlan
	{
		std::vector<VirtualGeometryStreamingFeedback> requests;
		std::vector<VirtualGeometryStreamingDecodeIntent> decodeIntents;
		std::vector<VirtualGeometryStreamingUploadIntent> uploadIntents;
		std::vector<VirtualGeometryStreamingDecodedPage> decodedPages;
		std::uint32_t duplicateFeedbackCount = 0u;
		std::uint32_t rejectedFeedbackCount = 0u;
		std::uint32_t expandedDependencyCount = 0u;
	};

	TERRAENGINE_API VirtualGeometryStreamingPlan BuildVirtualGeometryStreamingPlan(
		std::vector<VirtualGeometryStreamingFeedback> feedback, const VirtualGeometryStreamingPolicy &policy = {});
} // namespace TerraEngine::Resource
