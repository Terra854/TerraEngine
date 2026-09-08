// ProceduralNavigationSource.cpp - atomic immutable runtime source handoff.
#include "Navigation/Geometry/ProceduralNavigationSource.h"

#include <atomic>

namespace TerraEngine::Navigation
{
	ProceduralNavigationSourcePublication ProceduralNavigationSource::Publish(const ProceduralNavigationSourceInput& input)
	{
		ProceduralNavigationSourcePublication result;
		const NavigationGeometrySnapshotCreateResult created = NavigationGeometrySnapshot::Create(input.descriptor, input.geometry);
		if(!created.accepted) { result.reason = created.reason; return result; }
		std::atomic_store_explicit(&snapshot, created.snapshot, std::memory_order_release);
		result.accepted = true;
		result.snapshot = std::move(created.snapshot);
		return result;
	}

	std::shared_ptr<const NavigationGeometrySnapshot> ProceduralNavigationSource::Acquire() const noexcept
	{
		return std::atomic_load_explicit(&snapshot, std::memory_order_acquire);
	}
}
