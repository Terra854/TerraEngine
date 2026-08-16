// DerivedSceneCache.h - validated, disposable derived scene envelope contract.
//
// Derived scene data is deliberately a cache rather than project authority.
// The editor may delete it at any time and load the authored scene through the
// same preparation path.  This header contains only value data, so workers can
// validate and decode an envelope without touching ObjectManager or any live
// component storage.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Scene {

	inline constexpr std::uint32_t kDerivedSceneMagic = 0x4453434EU; // "DSCN"
	inline constexpr std::uint32_t kDerivedSceneFormatVersion = 2U;
	inline constexpr std::string_view kDerivedSceneUnboundedLineage = "TerraSceneUnboundedV1";
	inline constexpr std::size_t kDerivedSceneMaximumSectionCount = 64U;
	inline constexpr std::uint64_t kDerivedSceneMaximumArtifactBytes = 1024ULL * 1024ULL * 1024ULL;

	enum class DerivedSceneByteOrder : std::uint8_t {
		LittleEndian = 1U,
		BigEndian = 2U
	};

	enum class DerivedSceneSectionType : std::uint32_t {
		Objects = 1U,
		Components = 2U,
		Hierarchy = 3U,
		Spatial = 4U,
		Rendering = 5U,
		Physics = 6U
	};

	enum class DerivedSceneCacheState : std::uint8_t {
		Missing,
		Building,
		Valid,
		Decoded,
		Published,
		Stale,
		Rejected
	};

	// Every identity that can alter deserialized meaning is compared before a
	// cache hit.  Hash values are canonical text supplied by the caller; this
	// layer does not make platform-specific filesystem assumptions.
	struct DerivedSceneCompatibilityIdentity {
		std::string sourceId;
		std::string sourceHash;
		std::string dependencyHash;
		std::string componentSchemaFingerprint;
		std::string reflectionFingerprint;
		std::string engineCompatibility;
		std::string platform;
		DerivedSceneByteOrder byteOrder = DerivedSceneByteOrder::LittleEndian;
		// Appended fields preserve source initialization of the identity while the
		// binary v2 envelope still requires the explicit unbounded lineage.
		std::string formatLineage = std::string(kDerivedSceneUnboundedLineage);
		std::string spatialArtifactHash;
	};

	struct DerivedSceneSectionDescriptor {
		DerivedSceneSectionType type = DerivedSceneSectionType::Objects;
		std::uint32_t version = 1U;
		std::uint64_t offset = 0U;
		std::uint64_t storedBytes = 0U;
		std::uint64_t decodedBytes = 0U;
		std::uint64_t itemCount = 0U;
		bool required = true;
		std::uint64_t checksum = 0U;
	};

	struct DerivedSceneManifest {
		std::uint32_t magic = kDerivedSceneMagic;
		std::uint32_t formatVersion = kDerivedSceneFormatVersion;
		std::uint64_t headerBytes = 0U;
		std::uint64_t artifactBytes = 0U;
		DerivedSceneCompatibilityIdentity identity;
		std::uint64_t objectCount = 0U;
		std::uint64_t componentCount = 0U;
		std::uint64_t hierarchyCount = 0U;
		std::uint64_t payloadChecksum = 0U;
		std::string createdBy;
		std::vector<DerivedSceneSectionDescriptor> sections;
	};

	// The payload is kept separate from the descriptor so its offsets can be
	// checked before a reader allocates or copies section storage.
	struct DerivedSceneSection {
		DerivedSceneSectionDescriptor descriptor;
		std::vector<std::byte> payload;
	};

	struct DerivedSceneEnvelope {
		DerivedSceneManifest manifest;
		std::vector<DerivedSceneSection> sections;
	};

	struct DerivedSceneValidationResult {
		bool accepted = false;
		DerivedSceneCacheState state = DerivedSceneCacheState::Rejected;
		std::string reason;
	};

	// Worker preparation returns owned value records.  A cancellation callback
	// is sampled between sections; it can safely read an atomic project-close
	// generation without exposing live scene state to the worker.
	struct PreparedDerivedSceneSection {
		DerivedSceneSectionDescriptor descriptor;
		std::vector<std::byte> payload;
	};

	struct DerivedSceneDecodeResult {
		bool accepted = false;
		bool cancelled = false;
		bool usedAuthoritativeSource = false;
		DerivedSceneCacheState state = DerivedSceneCacheState::Rejected;
		std::string reason;
		std::vector<PreparedDerivedSceneSection> sections;
	};

	TERRAENGINE_API std::uint64_t CalculateDerivedSceneChecksum(const std::vector<std::byte>& bytes);
	TERRAENGINE_API DerivedSceneValidationResult ValidateDerivedSceneEnvelope(
		const DerivedSceneEnvelope& envelope,
		const DerivedSceneCompatibilityIdentity& expectedIdentity);
	TERRAENGINE_API std::vector<std::byte> EncodeDerivedSceneEnvelope(const DerivedSceneEnvelope& envelope);
	TERRAENGINE_API std::optional<DerivedSceneEnvelope> DecodeDerivedSceneEnvelope(const std::vector<std::byte>& bytes, std::string* outError = nullptr);
	TERRAENGINE_API DerivedSceneDecodeResult PrepareDerivedSceneSections(
		const DerivedSceneEnvelope& envelope,
		const DerivedSceneCompatibilityIdentity& expectedIdentity,
		const std::function<bool()>& cancellationRequested = {});

}
