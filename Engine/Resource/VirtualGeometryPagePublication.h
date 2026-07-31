// VirtualGeometryPagePublication.h - atomic publication records for streamed geometry pages.
//
// Streaming workers build these value-only candidates away from live renderer
// state. The protected publication boundary validates the whole candidate and
// either exposes every field together or preserves the prior visible record.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	inline constexpr std::uint32_t kInvalidVirtualGeometryPageId = 0xffffffffu;
	inline constexpr std::uint32_t kInvalidVirtualGeometryDescriptorIndex = 0xffffffffu;

	enum class VirtualGeometryPagePublicationState : std::uint8_t
	{
		Missing,
		Candidate,
		PendingIO,
		PendingDecode,
		PendingUpload,
		Resident,
		Failed,
		RetirePending,
		Retired
	};

	// This compact immutable record is the resource-side residency truth a
	// renderer may snapshot. Pending states are deliberately non-resident.
	struct VirtualGeometryPhysicalPageResidencyRow
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		std::uint32_t pageId = kInvalidVirtualGeometryPageId;
		VirtualGeometryPagePublicationState state = VirtualGeometryPagePublicationState::Missing;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t storedByteCount = 0u;
		std::uint64_t decodedByteCount = 0u;
		std::uint64_t integrityValue = 0u;
	};

	struct VirtualGeometryPhysicalPageResidencySnapshot
	{
		std::uint64_t publicationFrame = 0u;
		std::uint64_t publicationGeneration = 0u;
		std::vector<VirtualGeometryPhysicalPageResidencyRow> pages;
	};

	struct VirtualGeometryPhysicalPageResidencyBatchResult
	{
		VirtualGeometryPhysicalPageResidencySnapshot visible{};
		bool accepted = false;
		std::string rejectionReason;
	};

	struct VirtualGeometryPagePayloadCandidate
	{
		std::uint64_t storedByteCount = 0u;
		std::uint64_t decodedByteCount = 0u;
		std::uint64_t integrityValue = 0u;
		bool storedBytesVerified = false;
		bool decodedBlocksVerified = false;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct VirtualGeometryHierarchyFixupCandidate
	{
		std::vector<std::uint32_t> clusterIds;
		std::uint32_t fallbackPageId = kInvalidVirtualGeometryPageId;
		bool fallbackCoverageVerified = false;
		bool hierarchyAcyclic = false;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct VirtualGeometryPageDescriptorIntent
	{
		std::uint64_t allocationId = 0u;
		std::uint32_t bufferIndex = kInvalidVirtualGeometryDescriptorIndex;
		std::uint32_t descriptorIndex = kInvalidVirtualGeometryDescriptorIndex;
		bool uploadCompleted = false;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct VirtualGeometryPageRetirementIntent
	{
		std::uint64_t allocationId = 0u;
		std::uint64_t completionFence = 0u;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct VirtualGeometryPagePublicationCandidate
	{
		std::uint64_t candidateId = 0u;
		std::uint64_t publicationGeneration = 0u;
		TerraEngine::Renderer::VirtualGeometryPageIdentity key{};
		VirtualGeometryPagePayloadCandidate payload{};
		VirtualGeometryHierarchyFixupCandidate hierarchy{};
		VirtualGeometryPageDescriptorIntent descriptor{};
		std::vector<std::uint32_t> dependencyPageIds;
		std::vector<VirtualGeometryPageRetirementIntent> retirements;
		bool dependenciesVerified = false;
		bool residencyTransitionVerified = false;

		[[nodiscard]] bool IsComplete() const noexcept;
	};

	struct VirtualGeometryPagePublication
	{
		std::uint64_t candidateId = 0u;
		std::uint64_t publicationGeneration = 0u;
		TerraEngine::Renderer::VirtualGeometryPageIdentity key{};
		VirtualGeometryPagePayloadCandidate payload{};
		VirtualGeometryHierarchyFixupCandidate hierarchy{};
		VirtualGeometryPageDescriptorIntent descriptor{};
		std::vector<std::uint32_t> dependencyPageIds;
		std::vector<VirtualGeometryPageRetirementIntent> retirements;
		VirtualGeometryPagePublicationState state = VirtualGeometryPagePublicationState::Missing;
		std::uint64_t publicationFrame = 0u;

		[[nodiscard]] bool IsVisible() const noexcept;
	};

	struct VirtualGeometryPagePublicationResult
	{
		VirtualGeometryPagePublication visible{};
		bool accepted = false;
		std::string rejectionReason;
	};

	// A batch is the protected boundary used by a frame.  Its rows are staged in
	// private worker memory and become visible together, so a hierarchy cannot
	// observe only half of a dependency update.
	struct VirtualGeometryPagePublicationBatchResult
	{
		std::vector<VirtualGeometryPagePublication> visible;
		bool accepted = false;
		std::string rejectionReason;
	};

	// This store is owned by a protected scheduler callback. Workers never obtain
	// a mutable reference: they submit value candidates, then the boundary swaps
	// the entire visible vector only after the batch validator accepts every row.
	class TERRAENGINE_API VirtualGeometryPagePublicationStore
	{
	public:
		[[nodiscard]] std::vector<VirtualGeometryPagePublication> Snapshot() const;
		[[nodiscard]] std::uint64_t LastPublicationFrame() const noexcept;
		VirtualGeometryPagePublicationBatchResult PublishAtProtectedBoundary(
			const std::vector<VirtualGeometryPagePublicationCandidate>& candidates,
			std::uint64_t publicationFrame);
		void Clear() noexcept;

	private:
		std::vector<VirtualGeometryPagePublication> visiblePages;
		std::uint64_t lastPublicationFrame = 0u;
	};

	// The protected streaming callback owns this store. A failed or pending update
	// never downgrades an already resident last-known-good row.
	class TERRAENGINE_API VirtualGeometryPhysicalPageResidencyStore
	{
	public:
		[[nodiscard]] VirtualGeometryPhysicalPageResidencySnapshot Snapshot() const;
		VirtualGeometryPhysicalPageResidencyBatchResult PublishAtProtectedBoundary(
			const std::vector<VirtualGeometryPhysicalPageResidencyRow>& updates,
			std::uint64_t publicationFrame);
		void Clear() noexcept;

	private:
		VirtualGeometryPhysicalPageResidencySnapshot visibleSnapshot{};
	};

	// The caller passes the currently visible row and a private candidate. A
	// rejection returns the prior row unchanged so no decoded byte, descriptor,
	// hierarchy fixup, or retirement can leak into live state independently.
	TERRAENGINE_API VirtualGeometryPagePublicationResult PublishVirtualGeometryPageAtomically(
		const VirtualGeometryPagePublication &prior, const VirtualGeometryPagePublicationCandidate &candidate, std::uint64_t publicationFrame);
	TERRAENGINE_API VirtualGeometryPagePublicationBatchResult PublishVirtualGeometryPagesAtomically(const std::vector<VirtualGeometryPagePublication> &priors,
		const std::vector<VirtualGeometryPagePublicationCandidate> &candidates,
		std::uint64_t publicationFrame);
} // namespace TerraEngine::Resource
