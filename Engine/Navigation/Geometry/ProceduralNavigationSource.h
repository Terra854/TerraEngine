// ProceduralNavigationSource.h - typed runtime source publication.
#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <memory>
#include <string>

namespace TerraEngine::Navigation
{
	struct ProceduralNavigationSourceInput
	{
		NavigationGeometrySnapshotDescriptor descriptor{};
		NavigationGeometryDocument geometry{};
	};

	struct ProceduralNavigationSourcePublication
	{
		bool accepted = false;
		std::string reason;
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
	};

	// Producers submit copied provider-neutral geometry. Publication swaps one
	// immutable snapshot; it never runs jobs or retains a producer callback.
	class TERRAENGINE_API ProceduralNavigationSource final
	{
	public:
		[[nodiscard]] ProceduralNavigationSourcePublication Publish(const ProceduralNavigationSourceInput& input);
		[[nodiscard]] std::shared_ptr<const NavigationGeometrySnapshot> Acquire() const noexcept;

	private:
		std::shared_ptr<const NavigationGeometrySnapshot> snapshot;
	};
}
