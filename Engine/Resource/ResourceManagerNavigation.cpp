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
	RequestDeliveries deliveries;
	std::unique_lock gate(m_PublicationGate);
	InvalidateIdentityLocked(ResourceKind::Navigation, id, deliveries);
	const std::filesystem::path resolvedPath = (path.is_absolute() ? path : assetRoot / path).lexically_normal();
	const std::string key = NormalizeKey(resolvedPath);
	std::string previous;
	{ std::shared_lock lock(navigationGeometryPaths.mutex);
		if (const auto found = navigationGeometryPaths.idToPath.find(id); found != navigationGeometryPaths.idToPath.end()) previous = found->second; }
	if (!previous.empty() && previous != key) InvalidateKeyLocked(ResourceKind::Navigation, previous, deliveries);
	InvalidateKeyLocked(ResourceKind::Navigation, key, deliveries);
	std::unique_lock lock(navigationGeometryPaths.mutex);
	navigationGeometryPaths.pathToId[key] = id;
	navigationGeometryPaths.idToPath[id] = key;
	lock.unlock();
	gate.unlock();
	DeliverRequests(std::move(deliveries));
}

NavigationGeometryLoadResult ResourceManager::LoadNavigationGeometryCompanion(
	const TerraEngine::UUID& id,
	const std::filesystem::path& path,
	const TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor& descriptor,
	std::size_t maximumBytes) {
	NavigationGeometryLoadResult result;
	RequestPtr request;
	RequestDeliveries deliveries;
	if(id == TerraEngine::EMPTY_UUID || path.empty() || maximumBytes == 0U) {
		result.reason = "InvalidNavigationGeometryRequest";
		return result;
	}
	try {
		{
			std::lock_guard gate(m_PublicationGate);
			// Capture the root, absolute path and request epoch together. A project
			// replacement must invalidate this request rather than admit old-root
			// bytes under the new project's publication authority.
			result.resolvedPath = (path.is_absolute() ? path : assetRoot / path).lexically_normal();
			if(!IsNavigationGeometryCompanionPath(result.resolvedPath)) {
				// Only checked immutable companions participate in runtime capture;
				// authored source metadata is never a fallback for rejected paths.
				result.reason = "NavigationGeometryCompanionExtensionRejected";
				return result;
			}
			const auto key = NormalizeKey(result.resolvedPath);
			std::string previous;
			{ std::shared_lock paths(navigationGeometryPaths.mutex);
				if (const auto found = navigationGeometryPaths.idToPath.find(id); found != navigationGeometryPaths.idToPath.end()) previous = found->second; }
			if (!previous.empty() && previous != key) InvalidateKeyLocked(ResourceKind::Navigation, previous, deliveries);
			request = AdmitRequestLocked(ResourceKind::Navigation, key, id, true, true, {}, deliveries);
			if (!request) { result.reason = "NavigationGeometryAdmissionClosed"; return result; }
			std::unique_lock paths(navigationGeometryPaths.mutex);
			navigationGeometryPaths.pathToId[key] = id;
			navigationGeometryPaths.idToPath[id] = key;
		}
		DeliverRequests(std::move(deliveries));
		ScheduleRequestWork(request, [&] {
		InvokeRequestHook(request->key, 0U);
	std::error_code sizeError;
	const std::uintmax_t fileBytes = std::filesystem::file_size(result.resolvedPath, sizeError);
	if(sizeError) {
		result.reason = "NavigationGeometryMissing";
		FinishRequest(request, RequestOutcome::Failed, result.reason); return;
	}
	if(fileBytes > maximumBytes) {
		result.sourceBytes = static_cast<std::size_t>(std::min<std::uintmax_t>(
			fileBytes, std::numeric_limits<std::size_t>::max()));
		result.reason = "NavigationGeometryByteLimitExceeded";
		FinishRequest(request, RequestOutcome::Failed, result.reason); return;
	}

	const std::vector<std::uint8_t> rawBytes = ReadFileBytes(result.resolvedPath);
	result.sourceBytes = rawBytes.size();
	if(rawBytes.empty()) {
		result.reason = "NavigationGeometryMissing";
		FinishRequest(request, RequestOutcome::Failed, result.reason); return;
	}

	std::vector<std::byte> bytes(rawBytes.size());
	for(std::size_t index = 0U; index < rawBytes.size(); ++index) {
		bytes[index] = static_cast<std::byte>(rawBytes[index]);
	}
	TerraEngine::Navigation::NavigationGeometryDecodeResult decoded =
		TerraEngine::Navigation::DecodeNavigationGeometry(bytes);
	if(!decoded.accepted) {
		result.reason = decoded.reason.empty() ? "NavigationGeometryDecodeRejected" : std::move(decoded.reason);
		FinishRequest(request, RequestOutcome::Failed, result.reason); return;
	}

	TerraEngine::Navigation::NavigationGeometrySnapshotCreateResult created =
		TerraEngine::Navigation::NavigationGeometrySnapshot::Create(descriptor, decoded.document);
	if(!created.accepted || !created.snapshot) {
		result.reason = created.reason.empty() ? "NavigationGeometrySnapshotRejected" : std::move(created.reason);
		FinishRequest(request, RequestOutcome::Failed, result.reason); return;
	}

	InvokeRequestHook(request->key, 1U);
	RequestDeliveries published;
	std::unique_lock gate(m_PublicationGate);
	if (!HasAuthorityLocked(request) || !IdentityStillCurrentLocked(request, id)) {
		result.reason = "NavigationGeometryOwnershipObsolete";
		FinishRequestLocked(request, RequestOutcome::Obsolete, TerraEngine::EMPTY_UUID, result.reason, published);
		gate.unlock(); DeliverRequests(std::move(published)); return;
	}
	std::shared_lock paths(navigationGeometryPaths.mutex);
	if (navigationGeometryPaths.idToPath.at(id) != request->key || navigationGeometryPaths.pathToId.at(request->key) != id) {
		paths.unlock(); result.reason = "NavigationGeometryRegistrationChanged";
		FinishRequestLocked(request, RequestOutcome::Obsolete, TerraEngine::EMPTY_UUID, result.reason, published);
		gate.unlock(); DeliverRequests(std::move(published)); return;
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

	result.accepted = true;
	paths.unlock();
	FinishRequestLocked(request, RequestOutcome::Published, id, {}, published);
	gate.unlock();
	DeliverRequests(std::move(published));
		}, true);
		request->future.get();
	} catch (...) { AbortRequestSetup(request); result.reason = "NavigationGeometryRequestFailed"; }
	if (!result.accepted && result.reason.empty()) result.reason = "NavigationGeometryRequestObsolete";
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
	RequestDeliveries deliveries;
	std::unique_lock gate(m_PublicationGate);
	InvalidateIdentityLocked(ResourceKind::Navigation, id, deliveries);
	{ std::shared_lock paths(navigationGeometryPaths.mutex);
		const auto found = navigationGeometryPaths.idToPath.find(id);
		if (found != navigationGeometryPaths.idToPath.end()) {
			const auto key = found->second;
			paths.unlock();
			InvalidateKeyLocked(ResourceKind::Navigation, key, deliveries);
		}
	}
	std::unique_lock lock(navigationGeometryMutex);
	const bool released = navigationGeometrySnapshots.erase(id) != 0U;
	if(released) navigationGeometryPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
	lock.unlock();
	gate.unlock();
	DeliverRequests(std::move(deliveries));
	return released;
}
