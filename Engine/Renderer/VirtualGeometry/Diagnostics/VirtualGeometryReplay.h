#pragma once
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer
{
	enum class VirtualGeometryReplayCounterAvailability : std::uint8_t
	{
		Pending,
		Authoritative
	};

	struct VirtualGeometryReplayArtifact
	{
		std::uint64_t frameId = 0, hash = 0;
		std::uint32_t authoritativeCounter = 0;
		std::uint64_t commandGeneration = 0u;
		std::uint64_t publishedFrameId = 0u;
		VirtualGeometryReplayCounterAvailability counterAvailability =
			VirtualGeometryReplayCounterAvailability::Pending;
	};

	struct VirtualGeometryDelayedCounterReadback
	{
		std::uint64_t sourceFrameId = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint64_t availableAfterFrameId = 0u;
		std::uint32_t authoritativeCounter = 0u;
		std::uint64_t planHash = 0u;
	};

	// Backends publish this only after the command generation has completed and
	// the counter copy is readable. A zero counter is valid for a zero-work frame;
	// the completion/evidence identities, rather than the count, establish truth.
	struct VirtualGeometryAuthoritativeCounterReadback
	{
		std::uint64_t sourceFrameId = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint64_t completedFrameId = 0u;
		std::uint32_t authoritativeCounter = 0u;
		std::string evidenceId;

		[[nodiscard]] bool IsAuthoritative() const noexcept
		{
			return sourceFrameId != 0u && commandGeneration != 0u
				&& completedFrameId >= sourceFrameId && !evidenceId.empty();
		}
	};

	// Delayed readbacks remain private until their declared completion frame.
	// This prevents an uncompleted or recycled Vulkan counter from being exposed
	// as an authoritative result during same-frame preparation.
	class VirtualGeometryReplayReadbackQueue
	{
	public:
		void EnqueuePending(const VirtualGeometryGpuPlan &plan);
		void Enqueue(const VirtualGeometryGpuPlan &plan, std::uint64_t availableAfterFrameId,
			std::uint32_t authoritativeCounter);
		[[nodiscard]] std::vector<VirtualGeometryReplayArtifact> PublishAuthoritative(
			const std::vector<VirtualGeometryAuthoritativeCounterReadback> &readbacks);
		[[nodiscard]] std::vector<VirtualGeometryReplayArtifact> PublishCompleted(std::uint64_t completedFrameId);
		[[nodiscard]] std::size_t PendingCount() const;
		void Clear();

	private:
		std::vector<VirtualGeometryDelayedCounterReadback> pendingReadbacks;
	};

	// The serialized form is intentionally line-oriented and stable. Tooling can
	// persist it as an evidence artifact without parsing renderer-only objects.
	[[nodiscard]] std::string SerializeVirtualGeometryReplayArtifact(const VirtualGeometryReplayArtifact &artifact);
	[[nodiscard]] VirtualGeometryReplayArtifact BuildVirtualGeometryReplayArtifact(std::uint64_t, const VirtualGeometryGpuPlan &, std::uint32_t);
} // namespace TerraEngine::Renderer
