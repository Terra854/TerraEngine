// ResourceManagerNavigation.cpp - checked navigation companion loading.

#include "Resource/ResourceManager.h"

#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <system_error>

namespace {
	[[nodiscard]] bool IsNavigationGeometryCompanionPath(const std::filesystem::path& path) {
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		return extension == ".tenavgeom";
	}

	[[nodiscard]] bool SameDescriptor(
		const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& left,
		const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& right) noexcept {
		return left.sourceUuid == right.sourceUuid && left.sourceRevision == right.sourceRevision
			&& left.canonicalPlacementRevision == right.canonicalPlacementRevision
			&& left.physicalReadyRevision == right.physicalReadyRevision
			&& left.sourceKind == right.sourceKind && left.subshapeIndex == right.subshapeIndex;
	}
}

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
	if(!IsNavigationGeometryCompanionPath(result.resolvedPath)) {
		// A mesh can participate only through its checked immutable companion.
		// Refusing every other extension keeps automatic capture independent of
		// removed authored metadata and prevents source-file fallback.
		result.reason = "NavigationGeometryCompanionExtensionRejected";
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

	bool publishedReplacement = false;
	{
		// Exchange one immutable generation only after the complete file and copied
		// snapshot validate. Existing worker leases retain the prior shared object.
		std::unique_lock lock(navigationGeometryMutex);
		const auto existing = navigationGeometrySnapshots.find(id);
		if(existing != navigationGeometrySnapshots.end()
			&& existing->second->ContentFingerprint() == created.snapshot->ContentFingerprint()
			&& SameDescriptor(existing->second->Descriptor(), created.snapshot->Descriptor())) {
			// Capture may ask for the same companion on later frames. Reuse the
			// immutable generation so observing resource publication does not create
			// a self-triggered capture loop.
			result.snapshot = existing->second;
		} else {
			navigationGeometrySnapshots[id] = created.snapshot;
			result.snapshot = created.snapshot;
			publishedReplacement = true;
		}
	}
	// This generation changes only after the map holds a complete immutable
	// snapshot. Frame-end capture can therefore use it as resource evidence
	// without observing a partly decoded companion.
	if(publishedReplacement) navigationGeometryPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
	RegisterNavigationGeometryPath(id, path);
	result.accepted = true;
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
	if(path.empty()) {
		// Mesh identity is already the eligibility key used by automatic capture.
		// Resolve its adjacent cooked companion directly rather than consulting an
		// authored navigation-source record or inferring renderer residency.
		path = GetRegisteredAssetPath(id, AssetType::Mesh);
		if(!path.empty()) path.replace_extension(".tenavgeom");
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
	const bool released = navigationGeometrySnapshots.erase(id) != 0U;
	if(released) navigationGeometryPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
	return released;
}
