// ResourceManagerNavigation.cpp - checked navigation companion loading.

#include "Resource/ResourceManager.h"

#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <system_error>

void ResourceManager::RegisterNavigationGeometryPath(
	const TerraEngine::UUID& id, const std::filesystem::path& path) {
	if(id == TerraEngine::EMPTY_UUID || path.empty()) {
		return;
	}
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	std::unique_lock lock(navigationGeometryPaths.mutex);
	navigationGeometryPaths.pathToId[key] = id;
	navigationGeometryPaths.idToPath[id] = key;
}

NavigationGeometryLoadResult ResourceManager::LoadNavigationGeometryCompanion(
	const TerraEngine::UUID& id,
	const std::filesystem::path& path,
	const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& descriptor,
	std::size_t maximumBytes) {
	NavigationGeometryLoadResult result;
	result.resolvedPath = ResolveAssetPath(path);
	if(id == TerraEngine::EMPTY_UUID || path.empty() || maximumBytes == 0U) {
		result.reason = "InvalidNavigationGeometryRequest";
		return result;
	}
	std::error_code sizeError;
	const std::uintmax_t fileBytes = std::filesystem::file_size(result.resolvedPath, sizeError);
	if(sizeError) {
		result.reason = "NavigationGeometryMissing";
		return result;
	}
	if(fileBytes > maximumBytes) {
		result.sourceBytes = static_cast<std::size_t>(std::min<std::uintmax_t>(
			fileBytes, std::numeric_limits<std::size_t>::max()));
		result.reason = "NavigationGeometryByteLimitExceeded";
		return result;
	}

	const std::vector<std::uint8_t> rawBytes = ReadFileBytes(result.resolvedPath);
	result.sourceBytes = rawBytes.size();
	if(rawBytes.empty()) {
		result.reason = "NavigationGeometryMissing";
		return result;
	}

	std::vector<std::byte> bytes(rawBytes.size());
	for(std::size_t index = 0U; index < rawBytes.size(); ++index) {
		bytes[index] = static_cast<std::byte>(rawBytes[index]);
	}
	TerraEngine::Navigation::NavigationGeometryDecodeResult decoded =
		TerraEngine::Navigation::DecodeNavigationGeometry(bytes);
	if(!decoded.accepted) {
		result.reason = decoded.reason.empty() ? "NavigationGeometryDecodeRejected" : std::move(decoded.reason);
		return result;
	}

	TerraEngine::Navigation::NavigationGeometrySnapshotCreateResult created =
		TerraEngine::Navigation::NavigationGeometrySnapshot::Create(descriptor, decoded.document);
	if(!created.accepted || !created.snapshot) {
		result.reason = created.reason.empty() ? "NavigationGeometrySnapshotRejected" : std::move(created.reason);
		return result;
	}

	{
		// Exchange one immutable generation only after the complete file and copied
		// snapshot validate. Existing worker leases retain the prior shared object.
		std::unique_lock lock(navigationGeometryMutex);
		navigationGeometrySnapshots[id] = created.snapshot;
	}
	RegisterNavigationGeometryPath(id, path);
	result.accepted = true;
	result.snapshot = std::move(created.snapshot);
	return result;
}

NavigationGeometryLoadResult ResourceManager::LoadRegisteredNavigationGeometryCompanion(
	const TerraEngine::UUID& id,
	const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& descriptor,
	std::size_t maximumBytes) {
	std::filesystem::path path;
	{
		std::shared_lock lock(navigationGeometryPaths.mutex);
		const auto found = navigationGeometryPaths.idToPath.find(id);
		if(found != navigationGeometryPaths.idToPath.end()) {
			path = found->second;
		}
	}
	return LoadNavigationGeometryCompanion(id, path, descriptor, maximumBytes);
}

std::shared_ptr<const TerraEngine::Navigation::NavigationGeometrySnapshot>
ResourceManager::GetNavigationGeometrySnapshot(const TerraEngine::UUID& id) const {
	std::shared_lock lock(navigationGeometryMutex);
	const auto found = navigationGeometrySnapshots.find(id);
	return found == navigationGeometrySnapshots.end() ? nullptr : found->second;
}

bool ResourceManager::ReleaseNavigationGeometrySnapshot(const TerraEngine::UUID& id) {
	std::unique_lock lock(navigationGeometryMutex);
	return navigationGeometrySnapshots.erase(id) != 0U;
}
