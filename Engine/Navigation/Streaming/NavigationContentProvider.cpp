// NavigationContentProvider.cpp - checked geometry/cache reads via ContentMount.
#include "NavigationContentProvider.h"

#include "Navigation/Geometry/NavigationGeometryFormat.h"

namespace TerraEngine::Navigation
{
	namespace
	{
		NavigationContentStatus MapStatus(ContentReadStatus status) noexcept
		{
			switch(status)
			{
			case ContentReadStatus::Ok: return NavigationContentStatus::Ok;
			case ContentReadStatus::NotMounted: return NavigationContentStatus::NotMounted;
			case ContentReadStatus::NotFound: return NavigationContentStatus::NotFound;
			case ContentReadStatus::ReadLimitExceeded: return NavigationContentStatus::ReadLimitExceeded;
			case ContentReadStatus::OutOfRange:
			case ContentReadStatus::ArchiveError: return NavigationContentStatus::ArchiveError;
			}
			return NavigationContentStatus::ArchiveError;
		}
	}

	NavigationContentResult NavigationContentProvider::LoadGeometry(
		const ::ContentMount& mount, std::string_view logicalPath,
		NavigationGeometrySnapshotDescriptor descriptor, std::uint64_t maximumBytes)
	{
		NavigationContentResult result;
		result.logicalPath = ::ContentMount::NormalizeLogicalPath(logicalPath);
		if(result.logicalPath.empty()) { result.status = NavigationContentStatus::NotFound; result.reason = "EmptyLogicalPath"; return result; }
		const MountedContentReadResult read = mount.ReadAllChecked(result.logicalPath, maximumBytes);
		result.status = MapStatus(read.status);
		if(!read.Succeeded()) { result.reason = "MountedReadFailed"; return result; }
		std::vector<std::byte> bytes;
		bytes.reserve(read.bytes.size());
		for(const std::uint8_t value : read.bytes) bytes.push_back(static_cast<std::byte>(value));
		const NavigationGeometryDecodeResult decoded = DecodeNavigationGeometry(bytes);
		if(!decoded.accepted) { result.status = NavigationContentStatus::Malformed; result.reason = decoded.reason; return result; }
		descriptor.sourceRevision = descriptor.sourceRevision == 0U ? decoded.fingerprint : descriptor.sourceRevision;
		const NavigationGeometrySnapshotCreateResult snapshot = NavigationGeometrySnapshot::Create(descriptor, decoded.document);
		if(!snapshot.accepted) { result.status = NavigationContentStatus::Malformed; result.reason = snapshot.reason; return result; }
		result.geometry = snapshot.snapshot;
		return result;
	}

	NavigationContentResult NavigationContentProvider::LoadCacheChunk(
		const ::ContentMount& mount, std::string_view logicalPath, std::uint64_t maximumBytes)
	{
		NavigationContentResult result;
		result.logicalPath = ::ContentMount::NormalizeLogicalPath(logicalPath);
		if(result.logicalPath.empty()) { result.status = NavigationContentStatus::NotFound; result.reason = "EmptyLogicalPath"; return result; }
		const MountedContentReadResult read = mount.ReadAllChecked(result.logicalPath, maximumBytes);
		result.status = MapStatus(read.status);
		if(read.Succeeded()) result.cacheBytes = read.bytes;
		else result.reason = "MountedReadFailed";
		return result;
	}
}
