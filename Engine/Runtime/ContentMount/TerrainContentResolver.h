// TerrainContentResolver.h — checked mounted reads for terrain root bundles.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ContentMount;

namespace TerraEngine
{
	enum class TerrainContentStatus : std::uint8_t
	{
		Ok,
		NotMounted,
		NotFound,
		ForbiddenRuntimePath,
		WrongAssetClass,
		ReadLimitExceeded,
		ArchiveError
	};

	struct TerrainContentReadResult final
	{
		TerrainContentStatus status = TerrainContentStatus::NotMounted;
		std::string logicalPath;
		std::uint64_t archiveHash = 0U;
		std::vector<std::uint8_t> bytes;

		[[nodiscard]] bool Succeeded() const noexcept { return status == TerrainContentStatus::Ok; }
	};

	// Export runtime terrain reads have one authority: ContentMount. This helper
	// validates paths/classes and never falls back to a loose developer file.
	class TERRAENGINE_API TerrainContentResolver final
	{
	public:
		inline static constexpr std::uint64_t kMaximumRootBytes = 4ULL * 1024ULL * 1024ULL;
		inline static constexpr std::uint64_t kMaximumAreaBlobBytes = 128ULL * 1024ULL * 1024ULL;

		[[nodiscard]] static TerrainContentReadResult ReadRoot(
			const ::ContentMount& mount,
			std::string_view logicalPath);
		[[nodiscard]] static TerrainContentReadResult ReadAreaBlob(
			const ::ContentMount& mount,
			std::string_view logicalPath);
	};
}
