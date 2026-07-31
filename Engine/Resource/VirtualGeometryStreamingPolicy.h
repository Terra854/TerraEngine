// VirtualGeometryStreamingPolicy.h - deterministic, worker-safe page request policy.

#pragma once

#include "Core/Export.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceStreaming.h"

#include <cstdint>
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
		// Each entry is an ordered, value-only description of one physical TEMF
		// block. The worker reads and decodes every entry before it exposes one
		// concatenated page. Keeping the exact read next to its descriptor prevents
		// a later block from accidentally using the first block's byte range.
		struct ExactBlockReadDescriptor
		{
			VirtualGeometryStaticPageReadRequest staticPageRead{};
			TemfBlockDescriptor productionBlock{};
		};
		std::vector<ExactBlockReadDescriptor> exactBlockReads;
		std::vector<TemfQuantizationDomain> quantizationDomains;
		std::uint64_t availableDecoderCapabilities = 0u;
		// The cluster identifier lets diagnostics relate this value-only page
		// request to the immutable renderer selection that produced it.
		std::uint32_t clusterId = kVirtualGeometryInvalidIndex;
		// Diagnostics explain why an exact cooked byte range was or was not
		// attached while RenderSystem still owned the live mesh snapshot. This
		// string is copied into worker plans and never grants IO by itself.
		std::string canonicalPageReadDiagnostic;
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
		// Counts reflect completed/cancelled physical reads even when a later
		// block rejects the aggregate page and no decoded bytes are published.
		std::uint32_t completedPhysicalReadCount = 0u;
		std::uint32_t cancelledPhysicalReadCount = 0u;
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
