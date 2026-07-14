// CookedColliderAsset.h - immutable, versioned envelope for provider cooked bytes.
//
// The envelope records enough project-owned provenance to reject stale or
// incompatible bytes safely. Provider serialization remains opaque payload data.

#pragma once

#include "Core/Export.h"
#include "Physics/PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Physics
{
	inline constexpr std::uint32_t kCookedColliderEnvelopeMagic = 0x54455043U; // "TEPC" in canonical byte order.
	inline constexpr std::uint32_t kCookedColliderEnvelopeVersion = 1U;
	inline constexpr std::string_view kCookedColliderExtension = ".tepcoll";
	inline constexpr std::string_view kJoltCookProviderVersion = "5.6.0";
	inline constexpr std::string_view kJoltCookRevision = "e77f175595e64cb44218cc9d9d56fc365ad0e36a";
	inline constexpr std::string_view kJoltCookPayloadFormatFamily = "TerraEngine.Jolt.ShapeWithChildren";
	inline constexpr std::uint32_t kJoltCookPayloadFormatVersion = 1U;
	inline constexpr std::size_t kCookedColliderMaximumPayloadBytes = 64U * 1024U * 1024U;
	inline constexpr std::size_t kCookedColliderMaximumDiagnostics = 16U;
	inline constexpr std::size_t kCookedColliderMaximumDiagnosticLength = 512U;

	enum class CookedColliderEndianness : std::uint8_t
	{
		Little
	};

	enum class CookedColliderPlatform : std::uint8_t
	{
		WindowsX64,
		LinuxX64,
		MacOSArm64,
		Generic
	};

	// This family label is stable project metadata, not a public provider type.
	enum class CookedColliderProviderFamily : std::uint8_t
	{
		Jolt
	};

	enum class CookedColliderStatus : std::uint8_t
	{
		Valid,
		StaleLastKnownGood,
		InvalidUnpublished
	};

	enum class CookedColliderDiagnosticSeverity : std::uint8_t
	{
		Info,
		Warning,
		Error
	};

	enum class CookedColliderValidationCode : std::uint8_t
	{
		None,
		InvalidMagic,
		UnsupportedVersion,
		UnsupportedEndianness,
		MissingAssetId,
		MissingSourceAssetId,
		MissingSourceHash,
		MissingRecipeHash,
		InvalidProviderFamily,
		ProviderVersionMismatch,
		CookRevisionMismatch,
		PayloadFormatFamilyMismatch,
		PayloadFormatVersionMismatch,
		InvalidPlatform,
		InvalidGeometryCounts,
		InvalidBounds,
		EmptyPayload,
		PayloadLimitExceeded,
		PayloadSizeMismatch,
		ChecksumMismatch,
		InvalidStatus,
		InvalidLastKnownGood,
		MalformedEnvelope,
		TrailingData
	};

	struct CookedColliderBounds
	{
		PhysicsVector3 minimum;
		PhysicsVector3 maximum;

		[[nodiscard]] bool IsFiniteAndOrdered() const noexcept;
	};

	struct CookedColliderDiagnostic
	{
		CookedColliderDiagnosticSeverity severity = CookedColliderDiagnosticSeverity::Info;
		std::string code;
		std::string message;
	};

	// Diagnostics are bounded because corrupt payloads can be opened by editor,
	// headless, automation, and export flows at the same time.
	struct CookedColliderDiagnostics
	{
		std::vector<CookedColliderDiagnostic> entries;
		std::size_t suppressedCount = 0U;

		void Add(CookedColliderDiagnosticSeverity severity, std::string code, std::string message);
		[[nodiscard]] bool HasErrors() const noexcept;
	};

	struct CookedColliderLastKnownGood
	{
		std::uint64_t sourceContentHash = 0U;
		std::uint64_t recipeHash = 0U;
		std::string recookFailure;
	};

	// This contains all serializable metadata. The payload lives separately so
	// callers cannot accidentally interleave provider bytes with project fields.
	struct CookedColliderEnvelope
	{
		std::uint32_t magic = kCookedColliderEnvelopeMagic;
		std::uint32_t formatVersion = kCookedColliderEnvelopeVersion;
		CookedColliderEndianness endianness = CookedColliderEndianness::Little;
		CookedColliderPlatform platform = CookedColliderPlatform::Generic;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID sourceAssetId = TerraEngine::EMPTY_UUID;
		std::uint64_t sourceContentHash = 0U;
		std::uint64_t recipeHash = 0U;
		std::uint64_t settingsHash = 0U;
		CookedColliderProviderFamily providerFamily = CookedColliderProviderFamily::Jolt;
		std::string providerVersion = std::string(kJoltCookProviderVersion);
		std::string cookRevision = std::string(kJoltCookRevision);
		// The engine-owned payload format is distinct from the selected provider
		// version, so a backend upgrade cannot reinterpret opaque old bytes.
		std::string payloadFormatFamily = std::string(kJoltCookPayloadFormatFamily);
		std::uint32_t payloadFormatVersion = kJoltCookPayloadFormatVersion;
		std::uint32_t vertexCount = 0U;
		std::uint32_t triangleCount = 0U;
		std::uint32_t partCount = 0U;
		std::uint32_t materialSlotCount = 0U;
		CookedColliderBounds bounds;
		std::uint64_t payloadSize = 0U;
		std::uint64_t payloadChecksum = 0U;
		CookedColliderStatus status = CookedColliderStatus::Valid;
		std::optional<CookedColliderLastKnownGood> lastKnownGood;
		std::string provenanceSummary;
		std::vector<std::string> cookWarnings;
	};

	struct CookedColliderValidationResult
	{
		bool accepted = false;
		CookedColliderValidationCode code = CookedColliderValidationCode::None;
		std::string message;
	};

	struct CookedColliderCompatibilityResult
	{
		bool compatible = false;
		CookedColliderValidationCode code = CookedColliderValidationCode::None;
		std::string message;
	};

	// The class exposes only const accessors after creation, which makes sharing
	// one cooked record among readers thread-safe without a lock or provider cache.
	class TERRAENGINE_API CookedColliderAsset final
	{
	public:
		[[nodiscard]] static std::optional<CookedColliderAsset> Create(
			CookedColliderEnvelope envelope,
			std::vector<std::byte> payload,
			CookedColliderDiagnostics& diagnostics);
		[[nodiscard]] static std::optional<CookedColliderAsset> Deserialize(
			std::span<const std::byte> bytes,
			CookedColliderDiagnostics& diagnostics);

		[[nodiscard]] CookedColliderValidationResult Validate() const;
		[[nodiscard]] CookedColliderCompatibilityResult CheckCompatibility(
			CookedColliderPlatform runtimePlatform,
			CookedColliderProviderFamily providerFamily,
			std::string_view providerVersion,
			std::string_view cookRevision) const;
		[[nodiscard]] std::vector<std::byte> Serialize() const;
		[[nodiscard]] const CookedColliderEnvelope& Envelope() const noexcept { return envelope_; }
		[[nodiscard]] std::span<const std::byte> Payload() const noexcept { return payload_; }
		[[nodiscard]] static std::uint64_t ComputePayloadChecksum(std::span<const std::byte> payload) noexcept;
		[[nodiscard]] static CookedColliderPlatform HostPlatform() noexcept;

	private:
		CookedColliderAsset(CookedColliderEnvelope envelope, std::vector<std::byte> payload)
			: envelope_(std::move(envelope)), payload_(std::move(payload)) {}

		CookedColliderEnvelope envelope_;
		std::vector<std::byte> payload_;
	};

	TERRAENGINE_API std::string_view ToString(CookedColliderEndianness endianness);
	TERRAENGINE_API std::string_view ToString(CookedColliderPlatform platform);
	TERRAENGINE_API std::string_view ToString(CookedColliderProviderFamily family);
	TERRAENGINE_API std::string_view ToString(CookedColliderStatus status);
	TERRAENGINE_API std::string_view ToString(CookedColliderDiagnosticSeverity severity);
	TERRAENGINE_API std::string_view ToString(CookedColliderValidationCode code);
}
