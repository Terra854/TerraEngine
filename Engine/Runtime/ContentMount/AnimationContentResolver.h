// AnimationContentResolver.h - archive-only, bounded animation reads.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ContentMount;

namespace TerraEngine
{
	enum class AnimationContentStatus : std::uint8_t
	{
		Ok,
		NotMounted,
		NotFound,
		NotAnimationAsset,
		ForbiddenRuntimePath,
		OutOfRange,
		ReadLimitExceeded,
		ArchiveError
	};

	struct ResolvedAnimationContent final
	{
		std::string logicalPath;
		std::string assetClass;
		std::uint64_t logicalSize = 0U;
		std::uint64_t archiveHash = 0U;
		AnimationContentStatus status = AnimationContentStatus::NotMounted;
		bool present = false;

		[[nodiscard]] bool Succeeded() const noexcept { return status == AnimationContentStatus::Ok; }
	};

	struct AnimationContentReadResult final
	{
		ResolvedAnimationContent content;
		std::vector<std::uint8_t> bytes;

		[[nodiscard]] bool Succeeded() const noexcept { return content.Succeeded(); }
	};

	// Resolver APIs accept only a mounted archive. They cannot fall back to loose
	// project, source, editor, or build-tree files when a packaged asset is absent.
	class TERRAENGINE_API AnimationContentResolver final
	{
	public:
		inline static constexpr std::uint64_t kMaximumProbeBytes = 4096U;
		inline static constexpr std::uint64_t kDefaultMaximumRangeBytes = 64ULL * 1024ULL * 1024ULL;

		[[nodiscard]] static std::vector<ResolvedAnimationContent> Discover(const ::ContentMount& mount);
		[[nodiscard]] static ResolvedAnimationContent Resolve(
			const ::ContentMount& mount,
			std::string_view logicalPath);
		[[nodiscard]] static AnimationContentReadResult ReadRange(
			const ::ContentMount& mount,
			std::string_view logicalPath,
			std::uint64_t offset,
			std::uint64_t size,
			std::uint64_t maximumReadBytes = kDefaultMaximumRangeBytes);
	};
}
