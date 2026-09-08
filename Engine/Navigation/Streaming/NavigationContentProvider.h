// NavigationContentProvider.h - checked mounted reads for navigation inputs.
#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Runtime/ContentMount/ContentMount.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationContentStatus : std::uint8_t
	{
		Ok,
		NotMounted,
		NotFound,
		Malformed,
		ReadLimitExceeded,
		ArchiveError
	};

	struct NavigationContentResult
	{
		NavigationContentStatus status = NavigationContentStatus::NotMounted;
		std::string logicalPath;
		std::string reason;
		std::shared_ptr<const NavigationGeometrySnapshot> geometry;
		std::vector<std::uint8_t> cacheBytes;
		[[nodiscard]] bool Succeeded() const noexcept { return status == NavigationContentStatus::Ok; }
	};

	// Runtime navigation content is addressed only by normalized mounted paths.
	// This class does not probe the checkout or create a provider worker pool.
	class NavigationContentProvider final
	{
	public:
		static constexpr std::uint64_t kMaximumGeometryBytes = 128ULL * 1024ULL * 1024ULL;
		static constexpr std::uint64_t kMaximumCacheBytes = 256ULL * 1024ULL * 1024ULL;

		[[nodiscard]] static NavigationContentResult LoadGeometry(
			const ::ContentMount& mount,
			std::string_view logicalPath,
			NavigationGeometrySnapshotDescriptor descriptor,
			std::uint64_t maximumBytes = kMaximumGeometryBytes);
		[[nodiscard]] static NavigationContentResult LoadCacheChunk(
			const ::ContentMount& mount,
			std::string_view logicalPath,
			std::uint64_t maximumBytes = kMaximumCacheBytes);
	};
}
