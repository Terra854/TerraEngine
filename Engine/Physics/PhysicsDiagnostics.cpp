#include "PhysicsDiagnostics.h"

#include <algorithm>
#include <mutex>

namespace TerraEngine::Physics {
	namespace {
		std::mutex g_DiagnosticsSnapshotMutex;
		std::shared_ptr<const PhysicsDiagnosticsSnapshot> g_DiagnosticsSnapshot;
	}

	bool PhysicsDebugPacket::IsValidFor(PhysicsWorldHandle activeWorld, PhysicsTick currentTick) const noexcept
	{
		return world.IsValid() && world == activeWorld && producedTick <= currentTick && currentTick <= expiryTick;
	}

	PhysicsDebugPacketHistory::PhysicsDebugPacketHistory(std::size_t requestedCapacity)
		: capacity(std::max<std::size_t>(1U, requestedCapacity))
	{
	}

	void PhysicsDebugPacketHistory::Publish(std::shared_ptr<const PhysicsDebugPacket> packet)
	{
		// Ignore malformed input rather than inserting a null/expired entry that
		// would make renderer-unavailable and pause diagnostics harder to reason about.
		if (!packet || !packet->world.IsValid() || packet->expiryTick < packet->producedTick)
		{
			return;
		}
		std::lock_guard<std::mutex> lock(mutex);
		packets.push_back(std::move(packet));
		if (packets.size() > capacity)
		{
			packets.erase(packets.begin(), packets.begin() + static_cast<std::ptrdiff_t>(packets.size() - capacity));
		}
	}

	std::vector<std::shared_ptr<const PhysicsDebugPacket>> PhysicsDebugPacketHistory::Select(
		PhysicsWorldHandle activeWorld,
		PhysicsTick currentTick,
		const PhysicsDebugDisplaySelection& selection) const
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<std::shared_ptr<const PhysicsDebugPacket>> selected;
		const std::size_t maximum = std::max<std::size_t>(1U, selection.maximumPackets);
		for (auto packet = packets.rbegin(); packet != packets.rend() && selected.size() < maximum; ++packet)
		{
			if (*packet && (*packet)->IsValidFor(activeWorld, currentTick))
			{
				selected.push_back(*packet);
			}
		}
		return selected;
	}

	std::size_t PhysicsDebugPacketHistory::Size() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return packets.size();
	}

	void PhysicsDebugPacketHistory::Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		packets.clear();
	}

	PhysicsDebugPacketHistory& PublishedPhysicsDebugPackets()
	{
		static PhysicsDebugPacketHistory history;
		return history;
	}

	void PublishPhysicsDiagnosticsSnapshot(std::shared_ptr<const PhysicsDiagnosticsSnapshot> snapshot)
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsSnapshotMutex);
		g_DiagnosticsSnapshot = std::move(snapshot);
	}

	std::shared_ptr<const PhysicsDiagnosticsSnapshot> AcquirePhysicsDiagnosticsSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsSnapshotMutex);
		return g_DiagnosticsSnapshot;
	}
}
