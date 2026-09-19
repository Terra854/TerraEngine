// SavedNavigationFormat.h - checked optional .tenav derived-data container.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationWorld.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Navigation {
	// Version two persists enough provenance to distinguish an authoritative empty
	// tile from omitted coverage. Version-one bytes deliberately have no upgrade
	// path because their payload cannot prove that distinction safely.
	inline constexpr std::uint32_t kSavedNavigationFormatVersion = 2U;
	// Provider tile headers now use bounded shard-local coordinates while cache
	// index keys retain canonical int64 addresses. Older global-float payloads
	// must miss safely and trigger a fresh build rather than being reinterpreted.
	inline constexpr std::string_view kSavedNavigationBackendAbi = "recast-1.6.0-dtpolyref64-shardlocal-v2";
	inline constexpr std::size_t kMaximumSavedNavigationBytes = 512U * 1024U * 1024U;
	inline constexpr std::size_t kMaximumSavedNavigationChunks = 65536U;
	inline constexpr std::size_t kMaximumSavedNavigationSourceFingerprints = 65536U;

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
		// This ordered evidence is captured from immutable geometry. It is checked
		// again against a newly captured scene before any cached tile is published.
		std::vector<std::uint64_t> sourceFingerprints;
		// Canonical cell bounds make an otherwise opaque provider byte block prove
		// which world-space coverage address it is allowed to represent.
		NavigationGeometryBounds bounds{};
		std::vector<std::byte> bytes;
		std::uint64_t checksum = 0U;
		bool empty = false;
		bool supportComplete = false;
		bool clearanceComplete = false;
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
