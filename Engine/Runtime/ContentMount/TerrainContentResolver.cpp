// TerrainContentResolver.cpp — archive-only terrain bundle validation.

#include "TerrainContentResolver.h"

#include "ContentMount.h"

namespace TerraEngine
{
	namespace
	{
		bool IsForbiddenTerrainPath(std::string_view path)
		{
			return path.empty() || path.starts_with("source/") || path.starts_with("editor/")
				|| path.starts_with("build/") || path == ".." || path.find("../") != std::string_view::npos
				|| path.find(':') != std::string_view::npos;
		}

		TerrainContentStatus MapStatus(ContentReadStatus status)
		{
			switch(status)
			{
			case ContentReadStatus::Ok: return TerrainContentStatus::Ok;
			case ContentReadStatus::NotMounted: return TerrainContentStatus::NotMounted;
			case ContentReadStatus::NotFound: return TerrainContentStatus::NotFound;
			case ContentReadStatus::ReadLimitExceeded: return TerrainContentStatus::ReadLimitExceeded;
			case ContentReadStatus::OutOfRange:
			case ContentReadStatus::ArchiveError: return TerrainContentStatus::ArchiveError;
			}
			return TerrainContentStatus::ArchiveError;
		}

		TerrainContentReadResult ReadChecked(
			const ::ContentMount& mount,
			std::string_view logicalPath,
			std::string_view expectedSuffix,
			std::uint64_t maximumBytes)
		{
			TerrainContentReadResult result;
			result.logicalPath = ContentMount::NormalizeLogicalPath(logicalPath);
			if(IsForbiddenTerrainPath(result.logicalPath))
			{
				result.status = TerrainContentStatus::ForbiddenRuntimePath;
				return result;
			}
			if(!result.logicalPath.ends_with(expectedSuffix))
			{
				result.status = TerrainContentStatus::WrongAssetClass;
				return result;
			}

			MountedContentReadResult read = mount.ReadAllChecked(result.logicalPath, maximumBytes);
			result.status = MapStatus(read.status);
			result.archiveHash = read.entry.entryHash;
			if(!result.Succeeded()) return result;
			// Pack metadata classifies binary terrain roots and area blobs. Directory
			// manifests use the ordinary data class and are decoded by TerrainAssetStore.
			if(read.entry.assetClass != "terrain")
			{
				result.status = TerrainContentStatus::WrongAssetClass;
				return result;
			}
			result.bytes = std::move(read.bytes);
			return result;
		}
	}

	TerrainContentReadResult TerrainContentResolver::ReadRoot(
		const ::ContentMount& mount,
		std::string_view logicalPath)
	{
		return ReadChecked(mount, logicalPath, ".teterrain", kMaximumRootBytes);
	}

	TerrainContentReadResult TerrainContentResolver::ReadAreaBlob(
		const ::ContentMount& mount,
		std::string_view logicalPath)
	{
		return ReadChecked(mount, logicalPath, ".tearea", kMaximumAreaBlobBytes);
	}
}
