#include "VirtualGeometryReplay.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Renderer
{
	void VirtualGeometryReplayReadbackQueue::EnqueuePending(const VirtualGeometryGpuPlan &plan)
	{
		if(!plan.IsComplete() || plan.frameId == 0u || plan.generation == 0u)
		{
			return;
		}
		const auto existing = std::find_if(pendingReadbacks.begin(), pendingReadbacks.end(),
			[&plan](const VirtualGeometryDelayedCounterReadback &pending) {
				return pending.sourceFrameId == plan.frameId
					&& pending.commandGeneration == plan.generation;
			});
		if(existing == pendingReadbacks.end())
		{
			// UINT64_MAX marks an unresolved counter: it is intentionally not
			// published until a backend supplies an authoritative readback receipt.
			pendingReadbacks.push_back({ plan.frameId, plan.generation,
				std::numeric_limits<std::uint64_t>::max(), 0u, plan.deterministicHash });
		}
	}

	void VirtualGeometryReplayReadbackQueue::Enqueue(
		const VirtualGeometryGpuPlan &plan,
		std::uint64_t availableAfterFrameId,
		std::uint32_t authoritativeCounter)
	{
		if(!plan.IsComplete())
		{
			return;
		}
		pendingReadbacks.push_back({
			plan.frameId,
			plan.generation,
			std::max(availableAfterFrameId, plan.frameId),
			authoritativeCounter,
			plan.deterministicHash });
		std::sort(pendingReadbacks.begin(), pendingReadbacks.end(),
			[](const VirtualGeometryDelayedCounterReadback &left,
				const VirtualGeometryDelayedCounterReadback &right) {
				if(left.availableAfterFrameId != right.availableAfterFrameId)
				{
					return left.availableAfterFrameId < right.availableAfterFrameId;
				}
				if(left.sourceFrameId != right.sourceFrameId)
				{
					return left.sourceFrameId < right.sourceFrameId;
				}
				return left.commandGeneration < right.commandGeneration;
			});
	}

	std::vector<VirtualGeometryReplayArtifact>
		VirtualGeometryReplayReadbackQueue::PublishCompleted(std::uint64_t completedFrameId)
	{
		std::vector<VirtualGeometryReplayArtifact> published;
		auto pending = pendingReadbacks.begin();
		while(pending != pendingReadbacks.end()
			&& pending->availableAfterFrameId <= completedFrameId)
		{
			published.push_back({
				pending->sourceFrameId,
				pending->planHash,
				pending->authoritativeCounter,
				pending->commandGeneration,
				completedFrameId,
				VirtualGeometryReplayCounterAvailability::Authoritative });
			++pending;
		}
		pendingReadbacks.erase(pendingReadbacks.begin(), pending);
		return published;
	}

	std::vector<VirtualGeometryReplayArtifact>
		VirtualGeometryReplayReadbackQueue::PublishAuthoritative(
			const std::vector<VirtualGeometryAuthoritativeCounterReadback> &readbacks)
	{
		std::vector<VirtualGeometryReplayArtifact> published;
		for(const VirtualGeometryAuthoritativeCounterReadback &readback : readbacks)
		{
			if(!readback.IsAuthoritative())
			{
				continue;
			}
			const auto pending = std::find_if(pendingReadbacks.begin(), pendingReadbacks.end(),
				[&readback](const VirtualGeometryDelayedCounterReadback &candidate) {
					return candidate.sourceFrameId == readback.sourceFrameId
						&& candidate.commandGeneration == readback.commandGeneration;
				});
			if(pending == pendingReadbacks.end())
			{
				continue;
			}
			published.push_back({ pending->sourceFrameId, pending->planHash,
				readback.authoritativeCounter, pending->commandGeneration,
				readback.completedFrameId,
				VirtualGeometryReplayCounterAvailability::Authoritative });
			pendingReadbacks.erase(pending);
		}
		return published;
	}

	std::size_t VirtualGeometryReplayReadbackQueue::PendingCount() const
	{
		return pendingReadbacks.size();
	}

	void VirtualGeometryReplayReadbackQueue::Clear()
	{
		pendingReadbacks.clear();
	}

	std::string SerializeVirtualGeometryReplayArtifact(const VirtualGeometryReplayArtifact &artifact)
	{
		return "frame=" + std::to_string(artifact.frameId)
			+ ";hash=" + std::to_string(artifact.hash)
			+ ";counter=" + std::to_string(artifact.authoritativeCounter)
			+ ";commandGeneration=" + std::to_string(artifact.commandGeneration)
			+ ";publishedFrame=" + std::to_string(artifact.publishedFrameId)
			+ ";availability=" + (artifact.counterAvailability
				== VirtualGeometryReplayCounterAvailability::Authoritative
				? "authoritative" : "pending");
	}

	VirtualGeometryReplayArtifact BuildVirtualGeometryReplayArtifact(
		std::uint64_t frameId,
		const VirtualGeometryGpuPlan &plan,
		std::uint32_t authoritativeCounter)
	{
		// Kept for callers that have already crossed the protected readback
		// boundary. New backend code should queue the counter before publishing it.
		return { frameId, plan.deterministicHash, authoritativeCounter,
			plan.generation, frameId,
			VirtualGeometryReplayCounterAvailability::Authoritative };
	}
} // namespace TerraEngine::Renderer
