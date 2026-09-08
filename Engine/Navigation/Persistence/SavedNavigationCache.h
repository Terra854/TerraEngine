// SavedNavigationCache.h - atomic optional cache I/O with runtime fallback.

#pragma once

#include "Navigation/Persistence/SavedNavigationFormat.h"

#include <atomic>
#include <filesystem>

namespace TerraEngine::Navigation {
	// The in-memory form is useful to editor jobs and tests before a destination
	// path is selected. It executes the exact encoder used by the file overload.
	[[nodiscard]] TERRAENGINE_API SavedNavigationWriteResult WriteSavedNavigationAtomically(
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks);
	[[nodiscard]] TERRAENGINE_API SavedNavigationReadResult ReadSavedNavigation(
		std::span<const std::byte> bytes,
		const SavedNavigationDescriptor& expected);

	[[nodiscard]] TERRAENGINE_API SavedNavigationWriteResult WriteSavedNavigationCacheAtomically(
		const std::filesystem::path& destination,
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks,
		const std::atomic_bool* cancellation = nullptr);
	[[nodiscard]] TERRAENGINE_API SavedNavigationReadResult ReadSavedNavigationCache(
		const std::filesystem::path& source,
		const SavedNavigationDescriptor& expected);
}
