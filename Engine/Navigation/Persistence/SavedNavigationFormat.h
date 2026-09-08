// SavedNavigationFormat.h - checked optional .tenav derived-data container.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Navigation {
	inline constexpr std::uint32_t kSavedNavigationFormatVersion = 1U;
	inline constexpr std::size_t kMaximumSavedNavigationBytes = 512U * 1024U * 1024U;
	inline constexpr std::size_t kMaximumSavedNavigationChunks = 65536U;

	struct SavedNavigationDescriptor {
		std::uint32_t formatVersion = kSavedNavigationFormatVersion;
		std::string backendAbi;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t settingsFingerprint = 0U;

		[[nodiscard]] bool operator==(const SavedNavigationDescriptor&) const = default;
	};

	struct SavedNavigationChunk {
		NavigationCoverageKey key{};
		NavigationGeneration generation = 0U;
		std::vector<std::byte> bytes;
	};

	struct SavedNavigationWriteResult {
		bool accepted = false;
		std::string reason;
		std::uint64_t checksum = 0U;
		std::vector<std::byte> bytes;
	};

	struct SavedNavigationReadResult {
		bool accepted = false;
		bool matchesFreshRuntimeGeneration = false;
		std::string reason;
		SavedNavigationDescriptor descriptor{};
		std::vector<SavedNavigationChunk> chunks;
	};

	[[nodiscard]] TERRAENGINE_API SavedNavigationWriteResult EncodeSavedNavigation(
		const SavedNavigationDescriptor& descriptor,
		std::span<const SavedNavigationChunk> chunks);
	[[nodiscard]] TERRAENGINE_API SavedNavigationReadResult DecodeSavedNavigation(
		std::span<const std::byte> bytes,
		const SavedNavigationDescriptor& expected);
}
