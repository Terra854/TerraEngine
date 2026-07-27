// AnimationContentResolver.cpp - checked animation discovery and range reads.

#include "AnimationContentResolver.h"

#include "ContentMount.h"

#include <algorithm>
#include <array>

namespace TerraEngine
{
	namespace
	{
		std::string ExpectedAnimationClass(std::string_view logicalPath)
		{
			const std::size_t dot = logicalPath.find_last_of('.');
			const std::string_view extension = dot == std::string_view::npos
				? std::string_view{} : logicalPath.substr(dot);
			if(extension == ".teskel") return "skeleton";
			if(extension == ".teanim") return "animation-clip";
			if(extension == ".teanimctrl") return "animation-controller";
			if(extension == ".teanimmask") return "animation-mask";
			if(extension == ".teretarget") return "retarget-profile";
			if(extension == ".temotiondb") return "motion-database";
			if(extension == ".teragdoll") return "ragdoll-profile";
			return {};
		}

		bool IsForbiddenRuntimePath(std::string_view logicalPath)
		{
			return logicalPath.empty() || logicalPath.starts_with("source/")
				|| logicalPath.starts_with("editor/") || logicalPath.starts_with("build/")
				|| logicalPath == ".." || logicalPath.find("../") != std::string_view::npos
				|| logicalPath.find(':') != std::string_view::npos;
		}

		AnimationContentStatus MapReadStatus(ContentReadStatus status)
		{
			switch(status)
			{
			case ContentReadStatus::Ok: return AnimationContentStatus::Ok;
			case ContentReadStatus::NotMounted: return AnimationContentStatus::NotMounted;
			case ContentReadStatus::NotFound: return AnimationContentStatus::NotFound;
			case ContentReadStatus::OutOfRange: return AnimationContentStatus::OutOfRange;
			case ContentReadStatus::ReadLimitExceeded: return AnimationContentStatus::ReadLimitExceeded;
			case ContentReadStatus::ArchiveError: return AnimationContentStatus::ArchiveError;
			}
			return AnimationContentStatus::ArchiveError;
		}

		ResolvedAnimationContent DescribeRead(
			std::string normalizedPath,
			const MountedContentReadResult& read)
		{
			ResolvedAnimationContent result;
			result.logicalPath = std::move(normalizedPath);
			result.assetClass = read.entry.assetClass;
			result.logicalSize = read.entry.logicalSize;
			result.archiveHash = read.entry.entryHash;
			result.status = MapReadStatus(read.status);
			if(read.status != ContentReadStatus::Ok) return result;

			if(IsForbiddenRuntimePath(result.logicalPath))
			{
				result.status = AnimationContentStatus::ForbiddenRuntimePath;
				return result;
			}
			const std::string expectedClass = ExpectedAnimationClass(result.logicalPath);
			if(expectedClass.empty() || expectedClass != result.assetClass)
			{
				result.status = AnimationContentStatus::NotAnimationAsset;
				return result;
			}
			result.status = AnimationContentStatus::Ok;
			result.present = true;
			return result;
		}
	}

	std::vector<ResolvedAnimationContent> AnimationContentResolver::Discover(const ::ContentMount& mount)
	{
		std::vector<ResolvedAnimationContent> result;
		for(const MountedContentEntry& entry : mount.SnapshotEntries())
		{
			if(ExpectedAnimationClass(entry.logicalPath).empty()) continue;
			// Resolve performs a real bounded archive read, so discovery cannot
			// advertise metadata whose payload is unreadable after a remount.
			ResolvedAnimationContent candidate = Resolve(mount, entry.logicalPath);
			if(candidate.Succeeded()) result.push_back(std::move(candidate));
		}
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
			return left.logicalPath < right.logicalPath;
		});
		return result;
	}

	ResolvedAnimationContent AnimationContentResolver::Resolve(
		const ::ContentMount& mount,
		std::string_view logicalPath)
	{
		const std::string normalized = ContentMount::NormalizeLogicalPath(logicalPath);
		if(IsForbiddenRuntimePath(normalized))
		{
			ResolvedAnimationContent rejected;
			rejected.logicalPath = normalized;
			rejected.status = AnimationContentStatus::ForbiddenRuntimePath;
			return rejected;
		}

		const std::optional<MountedContentEntry> metadata = mount.FindEntry(normalized);
		if(!metadata)
		{
			ResolvedAnimationContent missing;
			missing.logicalPath = normalized;
			missing.status = mount.IsMounted()
				? AnimationContentStatus::NotFound : AnimationContentStatus::NotMounted;
			return missing;
		}
		const std::uint64_t probeSize = std::min(metadata->logicalSize, kMaximumProbeBytes);
		const MountedContentReadResult read = mount.ReadRangeChecked(
			normalized, 0U, probeSize, kMaximumProbeBytes);
		return DescribeRead(normalized, read);
	}

	AnimationContentReadResult AnimationContentResolver::ReadRange(
		const ::ContentMount& mount,
		std::string_view logicalPath,
		std::uint64_t offset,
		std::uint64_t size,
		std::uint64_t maximumReadBytes)
	{
		AnimationContentReadResult result;
		const std::string normalized = ContentMount::NormalizeLogicalPath(logicalPath);
		if(IsForbiddenRuntimePath(normalized))
		{
			result.content.logicalPath = normalized;
			result.content.status = AnimationContentStatus::ForbiddenRuntimePath;
			return result;
		}

		MountedContentReadResult read = mount.ReadRangeChecked(
			normalized, offset, size, maximumReadBytes);
		result.content = DescribeRead(normalized, read);
		if(result.content.Succeeded()) result.bytes = std::move(read.bytes);
		return result;
	}
}
