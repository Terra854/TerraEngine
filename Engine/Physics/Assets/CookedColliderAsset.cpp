// CookedColliderAsset.cpp - portable envelope serialization and validation.

#include "CookedColliderAsset.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace TerraEngine::Physics
{
	namespace
	{
		constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
		constexpr std::size_t kMaximumEnvelopeStringLength = 1024U;
		constexpr std::size_t kMaximumCookWarnings = 16U;

		[[nodiscard]] bool IsEmpty(const TerraEngine::UUID& id)
		{
			return id == TerraEngine::EMPTY_UUID;
		}

		[[nodiscard]] CookedColliderValidationResult Reject(CookedColliderValidationCode code, std::string message)
		{
			return {false, code, std::move(message)};
		}

		[[nodiscard]] bool IsKnownPlatform(CookedColliderPlatform platform)
		{
			return platform == CookedColliderPlatform::WindowsX64 || platform == CookedColliderPlatform::LinuxX64
				|| platform == CookedColliderPlatform::MacOSArm64 || platform == CookedColliderPlatform::Generic;
		}

		[[nodiscard]] bool IsKnownStatus(CookedColliderStatus status)
		{
			return status == CookedColliderStatus::Valid || status == CookedColliderStatus::StaleLastKnownGood
				|| status == CookedColliderStatus::InvalidUnpublished;
		}

		void AppendByte(std::vector<std::byte>& target, std::uint8_t value)
		{
			target.push_back(static_cast<std::byte>(value));
		}

		template<typename Value>
		void AppendUnsigned(std::vector<std::byte>& target, Value value)
		{
			static_assert(std::is_unsigned_v<Value>);
			for(std::size_t index = 0U; index < sizeof(Value); ++index)
				AppendByte(target, static_cast<std::uint8_t>(value >> (index * 8U)));
		}

		void AppendFloat(std::vector<std::byte>& target, float value)
		{
			AppendUnsigned(target, std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
		}

		void AppendUuid(std::vector<std::byte>& target, const TerraEngine::UUID& id)
		{
			for(const std::uint8_t byte : id) AppendByte(target, byte);
		}

		bool AppendString(std::vector<std::byte>& target, std::string_view value)
		{
			if(value.size() > kMaximumEnvelopeStringLength) return false;
			AppendUnsigned(target, static_cast<std::uint32_t>(value.size()));
			for(const char character : value) AppendByte(target, static_cast<std::uint8_t>(character));
			return true;
		}

		class EnvelopeReader final
		{
		public:
			explicit EnvelopeReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

			template<typename Value>
			[[nodiscard]] bool ReadUnsigned(Value& value)
			{
				static_assert(std::is_unsigned_v<Value>);
				if(bytes_.size() - offset_ < sizeof(Value)) return false;
				value = 0U;
				for(std::size_t index = 0U; index < sizeof(Value); ++index)
					value |= static_cast<Value>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << (index * 8U);
				return true;
			}

			[[nodiscard]] bool ReadFloat(float& value)
			{
				std::uint32_t bits = 0U;
				if(!ReadUnsigned(bits)) return false;
				value = std::bit_cast<float>(bits);
				return true;
			}

			[[nodiscard]] bool ReadUuid(TerraEngine::UUID& id)
			{
				if(bytes_.size() - offset_ < id.size()) return false;
				for(std::uint8_t& byte : id) byte = std::to_integer<std::uint8_t>(bytes_[offset_++]);
				return true;
			}

			[[nodiscard]] bool ReadString(std::string& value, std::size_t maximumLength = kMaximumEnvelopeStringLength)
			{
				std::uint32_t length = 0U;
				if(!ReadUnsigned(length) || length > maximumLength || bytes_.size() - offset_ < length) return false;
				value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
				offset_ += length;
				return true;
			}

			[[nodiscard]] bool ReadPayload(std::vector<std::byte>& payload, std::uint64_t length)
			{
				if(length > kCookedColliderMaximumPayloadBytes || bytes_.size() - offset_ != length) return false;
				payload.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), bytes_.end());
				offset_ = bytes_.size();
				return true;
			}

			[[nodiscard]] bool AtEnd() const noexcept { return offset_ == bytes_.size(); }

		private:
			std::span<const std::byte> bytes_;
			std::size_t offset_ = 0U;
		};

		void AddValidationDiagnostic(CookedColliderDiagnostics& diagnostics, const CookedColliderValidationResult& validation)
		{
			diagnostics.Add(CookedColliderDiagnosticSeverity::Error, std::string(ToString(validation.code)), validation.message);
		}
	}

	bool CookedColliderBounds::IsFiniteAndOrdered() const noexcept
	{
		return minimum.IsFinite() && maximum.IsFinite()
			&& minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
	}

	void CookedColliderDiagnostics::Add(CookedColliderDiagnosticSeverity severity, std::string code, std::string message)
	{
		if(entries.size() >= kCookedColliderMaximumDiagnostics)
		{
			++suppressedCount;
			return;
		}
		if(code.size() > kCookedColliderMaximumDiagnosticLength) code.resize(kCookedColliderMaximumDiagnosticLength);
		if(message.size() > kCookedColliderMaximumDiagnosticLength) message.resize(kCookedColliderMaximumDiagnosticLength);
		entries.push_back({severity, std::move(code), std::move(message)});
	}

	bool CookedColliderDiagnostics::HasErrors() const noexcept
	{
		return std::any_of(entries.begin(), entries.end(), [](const CookedColliderDiagnostic& entry) {
			return entry.severity == CookedColliderDiagnosticSeverity::Error;
		});
	}

	std::optional<CookedColliderAsset> CookedColliderAsset::Create(
		CookedColliderEnvelope envelope, std::vector<std::byte> payload, CookedColliderDiagnostics& diagnostics)
	{
		envelope.payloadSize = static_cast<std::uint64_t>(payload.size());
		envelope.payloadChecksum = ComputePayloadChecksum(payload);
		CookedColliderAsset asset(std::move(envelope), std::move(payload));
		const CookedColliderValidationResult validation = asset.Validate();
		if(!validation.accepted)
		{
			AddValidationDiagnostic(diagnostics, validation);
			return std::nullopt;
		}
		return asset;
	}

	CookedColliderValidationResult CookedColliderAsset::Validate() const
	{
		if(envelope_.magic != kCookedColliderEnvelopeMagic)
			return Reject(CookedColliderValidationCode::InvalidMagic, "Cooked collider envelope magic is not TEPC.");
		if(envelope_.formatVersion != kCookedColliderEnvelopeVersion)
			return Reject(CookedColliderValidationCode::UnsupportedVersion, "Cooked collider envelope version requires an explicit migration or recook.");
		if(envelope_.endianness != CookedColliderEndianness::Little)
			return Reject(CookedColliderValidationCode::UnsupportedEndianness, "Cooked collider payloads must declare canonical little-endian envelope metadata.");
		if(!IsKnownPlatform(envelope_.platform))
			return Reject(CookedColliderValidationCode::InvalidPlatform, "Cooked collider platform marker is not engine-owned.");
		if(IsEmpty(envelope_.assetId))
			return Reject(CookedColliderValidationCode::MissingAssetId, "Cooked collider assets require a stable asset GUID.");
		if(IsEmpty(envelope_.sourceAssetId))
			return Reject(CookedColliderValidationCode::MissingSourceAssetId, "Cooked collider assets require a stable source asset GUID.");
		if(envelope_.sourceContentHash == 0U)
			return Reject(CookedColliderValidationCode::MissingSourceHash, "Cooked collider assets require a source content hash.");
		if(envelope_.recipeHash == 0U)
			return Reject(CookedColliderValidationCode::MissingRecipeHash, "Cooked collider assets require a normalized recipe hash.");
		if(envelope_.providerFamily != CookedColliderProviderFamily::Jolt)
			return Reject(CookedColliderValidationCode::InvalidProviderFamily, "Cooked collider provider family is not supported by this build.");
		if(envelope_.providerVersion != kJoltCookProviderVersion)
			return Reject(CookedColliderValidationCode::ProviderVersionMismatch, "Cooked collider payload was not produced by the required Jolt 5.6.0 cook version.");
		if(envelope_.cookRevision != kJoltCookRevision)
			return Reject(CookedColliderValidationCode::CookRevisionMismatch, "Cooked collider payload was not produced by the required pinned Jolt revision.");
		if(envelope_.payloadFormatFamily != kJoltCookPayloadFormatFamily)
			return Reject(CookedColliderValidationCode::PayloadFormatFamilyMismatch, "Cooked collider payload format family is not the required engine-owned Jolt shape format.");
		if(envelope_.payloadFormatVersion != kJoltCookPayloadFormatVersion)
			return Reject(CookedColliderValidationCode::PayloadFormatVersionMismatch, "Cooked collider payload format version requires an explicit recook.");
		if(envelope_.vertexCount == 0U || envelope_.partCount == 0U)
			return Reject(CookedColliderValidationCode::InvalidGeometryCounts, "Cooked collider geometry must contain vertices and at least one part.");
		if(!envelope_.bounds.IsFiniteAndOrdered())
			return Reject(CookedColliderValidationCode::InvalidBounds, "Cooked collider bounds must be finite and ordered.");
		if(payload_.empty())
			return Reject(CookedColliderValidationCode::EmptyPayload, "Cooked collider provider payload must not be empty.");
		if(payload_.size() > kCookedColliderMaximumPayloadBytes)
			return Reject(CookedColliderValidationCode::PayloadLimitExceeded, "Cooked collider provider payload exceeds the bounded asset limit.");
		if(envelope_.payloadSize != payload_.size())
			return Reject(CookedColliderValidationCode::PayloadSizeMismatch, "Cooked collider payload size does not match its envelope.");
		if(envelope_.payloadChecksum != ComputePayloadChecksum(payload_))
			return Reject(CookedColliderValidationCode::ChecksumMismatch, "Cooked collider payload checksum does not match its envelope.");
		if(!IsKnownStatus(envelope_.status) || envelope_.status == CookedColliderStatus::InvalidUnpublished)
			return Reject(CookedColliderValidationCode::InvalidStatus, "Unpublished collider diagnostics are not loadable cooked assets.");
		if(envelope_.status == CookedColliderStatus::StaleLastKnownGood)
		{
			if(!envelope_.lastKnownGood || envelope_.lastKnownGood->sourceContentHash == 0U
				|| envelope_.lastKnownGood->recipeHash == 0U || envelope_.lastKnownGood->recookFailure.empty()
				|| envelope_.lastKnownGood->recookFailure.size() > kCookedColliderMaximumDiagnosticLength)
				return Reject(CookedColliderValidationCode::InvalidLastKnownGood, "Stale last-known-good collider data requires bounded failed-recook provenance.");
		}
		if(envelope_.provenanceSummary.size() > kMaximumEnvelopeStringLength || envelope_.cookWarnings.size() > kMaximumCookWarnings)
			return Reject(CookedColliderValidationCode::MalformedEnvelope, "Cooked collider provenance or warning count exceeds the envelope bound.");
		for(const std::string& warning : envelope_.cookWarnings)
			if(warning.size() > kCookedColliderMaximumDiagnosticLength)
				return Reject(CookedColliderValidationCode::MalformedEnvelope, "Cooked collider warning exceeds the bounded diagnostic length.");
		return {true, CookedColliderValidationCode::None, {}};
	}

	CookedColliderCompatibilityResult CookedColliderAsset::CheckCompatibility(
		CookedColliderPlatform runtimePlatform,
		CookedColliderProviderFamily providerFamily,
		std::string_view providerVersion,
		std::string_view cookRevision) const
	{
		const CookedColliderValidationResult validation = Validate();
		if(!validation.accepted) return {false, validation.code, validation.message};
		if(envelope_.platform != CookedColliderPlatform::Generic && envelope_.platform != runtimePlatform)
			return {false, CookedColliderValidationCode::InvalidPlatform, "Cooked collider platform marker does not match the runtime platform."};
		if(envelope_.providerFamily != providerFamily)
			return {false, CookedColliderValidationCode::InvalidProviderFamily, "Cooked collider provider family does not match the active backend."};
		if(envelope_.providerVersion != providerVersion)
			return {false, CookedColliderValidationCode::ProviderVersionMismatch, "Cooked collider provider version does not match the active backend."};
		if(envelope_.cookRevision != cookRevision)
			return {false, CookedColliderValidationCode::CookRevisionMismatch, "Cooked collider revision does not match the active backend cook revision."};
		return {true, CookedColliderValidationCode::None, {}};
	}

	std::vector<std::byte> CookedColliderAsset::Serialize() const
	{
		if(!Validate().accepted) return {};
		std::vector<std::byte> result;
		result.reserve(256U + payload_.size());
		AppendUnsigned(result, envelope_.magic);
		AppendUnsigned(result, envelope_.formatVersion);
		AppendUnsigned(result, static_cast<std::uint8_t>(envelope_.endianness));
		AppendUnsigned(result, static_cast<std::uint8_t>(envelope_.platform));
		AppendUuid(result, envelope_.assetId);
		AppendUuid(result, envelope_.sourceAssetId);
		AppendUnsigned(result, envelope_.sourceContentHash);
		AppendUnsigned(result, envelope_.recipeHash);
		AppendUnsigned(result, envelope_.settingsHash);
		AppendUnsigned(result, static_cast<std::uint8_t>(envelope_.providerFamily));
		if(!AppendString(result, envelope_.providerVersion) || !AppendString(result, envelope_.cookRevision)
			|| !AppendString(result, envelope_.payloadFormatFamily)) return {};
		AppendUnsigned(result, envelope_.payloadFormatVersion);
		AppendUnsigned(result, envelope_.vertexCount);
		AppendUnsigned(result, envelope_.triangleCount);
		AppendUnsigned(result, envelope_.partCount);
		AppendUnsigned(result, envelope_.materialSlotCount);
		AppendFloat(result, envelope_.bounds.minimum.x);
		AppendFloat(result, envelope_.bounds.minimum.y);
		AppendFloat(result, envelope_.bounds.minimum.z);
		AppendFloat(result, envelope_.bounds.maximum.x);
		AppendFloat(result, envelope_.bounds.maximum.y);
		AppendFloat(result, envelope_.bounds.maximum.z);
		AppendUnsigned(result, envelope_.payloadSize);
		AppendUnsigned(result, envelope_.payloadChecksum);
		AppendUnsigned(result, static_cast<std::uint8_t>(envelope_.status));
		AppendUnsigned(result, static_cast<std::uint8_t>(envelope_.lastKnownGood.has_value()));
		if(envelope_.lastKnownGood)
		{
			AppendUnsigned(result, envelope_.lastKnownGood->sourceContentHash);
			AppendUnsigned(result, envelope_.lastKnownGood->recipeHash);
			if(!AppendString(result, envelope_.lastKnownGood->recookFailure)) return {};
		}
		if(!AppendString(result, envelope_.provenanceSummary)) return {};
		AppendUnsigned(result, static_cast<std::uint32_t>(envelope_.cookWarnings.size()));
		for(const std::string& warning : envelope_.cookWarnings)
			if(!AppendString(result, warning)) return {};
		result.insert(result.end(), payload_.begin(), payload_.end());
		return result;
	}

	std::optional<CookedColliderAsset> CookedColliderAsset::Deserialize(std::span<const std::byte> bytes, CookedColliderDiagnostics& diagnostics)
	{
		EnvelopeReader reader(bytes);
		CookedColliderEnvelope envelope;
		std::uint8_t endianness = 0U;
		std::uint8_t platform = 0U;
		std::uint8_t family = 0U;
		std::uint8_t status = 0U;
		std::uint8_t hasLastKnownGood = 0U;
		std::uint32_t warningCount = 0U;
		if(!reader.ReadUnsigned(envelope.magic) || !reader.ReadUnsigned(envelope.formatVersion)
			|| !reader.ReadUnsigned(endianness) || !reader.ReadUnsigned(platform)
			|| !reader.ReadUuid(envelope.assetId) || !reader.ReadUuid(envelope.sourceAssetId)
			|| !reader.ReadUnsigned(envelope.sourceContentHash) || !reader.ReadUnsigned(envelope.recipeHash)
			|| !reader.ReadUnsigned(envelope.settingsHash) || !reader.ReadUnsigned(family)
			|| !reader.ReadString(envelope.providerVersion) || !reader.ReadString(envelope.cookRevision)
			|| !reader.ReadString(envelope.payloadFormatFamily) || !reader.ReadUnsigned(envelope.payloadFormatVersion)
			|| !reader.ReadUnsigned(envelope.vertexCount) || !reader.ReadUnsigned(envelope.triangleCount)
			|| !reader.ReadUnsigned(envelope.partCount) || !reader.ReadUnsigned(envelope.materialSlotCount)
			|| !reader.ReadFloat(envelope.bounds.minimum.x) || !reader.ReadFloat(envelope.bounds.minimum.y) || !reader.ReadFloat(envelope.bounds.minimum.z)
			|| !reader.ReadFloat(envelope.bounds.maximum.x) || !reader.ReadFloat(envelope.bounds.maximum.y) || !reader.ReadFloat(envelope.bounds.maximum.z)
			|| !reader.ReadUnsigned(envelope.payloadSize) || !reader.ReadUnsigned(envelope.payloadChecksum)
			|| !reader.ReadUnsigned(status) || !reader.ReadUnsigned(hasLastKnownGood))
		{
			diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider envelope ended before its required metadata.");
			return std::nullopt;
		}
		envelope.endianness = static_cast<CookedColliderEndianness>(endianness);
		envelope.platform = static_cast<CookedColliderPlatform>(platform);
		envelope.providerFamily = static_cast<CookedColliderProviderFamily>(family);
		envelope.status = static_cast<CookedColliderStatus>(status);
		if(hasLastKnownGood > 1U)
		{
			diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider last-known-good marker is invalid.");
			return std::nullopt;
		}
		if(hasLastKnownGood != 0U)
		{
			CookedColliderLastKnownGood lastKnownGood;
			if(!reader.ReadUnsigned(lastKnownGood.sourceContentHash) || !reader.ReadUnsigned(lastKnownGood.recipeHash)
				|| !reader.ReadString(lastKnownGood.recookFailure, kCookedColliderMaximumDiagnosticLength))
			{
				diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider last-known-good metadata is truncated.");
				return std::nullopt;
			}
			envelope.lastKnownGood = std::move(lastKnownGood);
		}
		if(!reader.ReadString(envelope.provenanceSummary) || !reader.ReadUnsigned(warningCount) || warningCount > kMaximumCookWarnings)
		{
			diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider provenance or warning table is malformed.");
			return std::nullopt;
		}
		envelope.cookWarnings.reserve(warningCount);
		for(std::uint32_t warning = 0U; warning < warningCount; ++warning)
		{
			std::string message;
			if(!reader.ReadString(message, kCookedColliderMaximumDiagnosticLength))
			{
				diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider warning table is truncated.");
				return std::nullopt;
			}
			envelope.cookWarnings.push_back(std::move(message));
		}
		std::vector<std::byte> payload;
		if(!reader.ReadPayload(payload, envelope.payloadSize) || !reader.AtEnd())
		{
			diagnostics.Add(CookedColliderDiagnosticSeverity::Error, "MalformedEnvelope", "Cooked collider payload size does not match the serialized envelope.");
			return std::nullopt;
		}
		// Create recomputes trusted payload metadata, so restore the serialized
		// values afterward to ensure tampered size/checksum fields are rejected.
		const std::uint64_t serializedSize = envelope.payloadSize;
		const std::uint64_t serializedChecksum = envelope.payloadChecksum;
		envelope.payloadSize = serializedSize;
		envelope.payloadChecksum = serializedChecksum;
		CookedColliderAsset asset(std::move(envelope), std::move(payload));
		const CookedColliderValidationResult validation = asset.Validate();
		if(!validation.accepted)
		{
			AddValidationDiagnostic(diagnostics, validation);
			return std::nullopt;
		}
		return asset;
	}

	std::uint64_t CookedColliderAsset::ComputePayloadChecksum(std::span<const std::byte> payload) noexcept
	{
		std::uint64_t hash = kFnvOffsetBasis;
		for(const std::byte value : payload)
		{
			hash ^= std::to_integer<std::uint8_t>(value);
			hash *= kFnvPrime;
		}
		return hash;
	}

	CookedColliderPlatform CookedColliderAsset::HostPlatform() noexcept
	{
#if defined(_WIN32) && defined(_M_X64)
		return CookedColliderPlatform::WindowsX64;
#elif defined(__linux__) && defined(__x86_64__)
		return CookedColliderPlatform::LinuxX64;
#elif defined(__APPLE__) && defined(__aarch64__)
		return CookedColliderPlatform::MacOSArm64;
#else
		return CookedColliderPlatform::Generic;
#endif
	}

	std::string_view ToString(CookedColliderEndianness endianness)
	{
		return endianness == CookedColliderEndianness::Little ? "Little" : "Unknown";
	}

	std::string_view ToString(CookedColliderPlatform platform)
	{
		switch(platform)
		{
		case CookedColliderPlatform::WindowsX64: return "WindowsX64";
		case CookedColliderPlatform::LinuxX64: return "LinuxX64";
		case CookedColliderPlatform::MacOSArm64: return "MacOSArm64";
		case CookedColliderPlatform::Generic: return "Generic";
		}
		return "Unknown";
	}

	std::string_view ToString(CookedColliderProviderFamily family)
	{
		return family == CookedColliderProviderFamily::Jolt ? "Jolt" : "Unknown";
	}

	std::string_view ToString(CookedColliderStatus status)
	{
		switch(status)
		{
		case CookedColliderStatus::Valid: return "Valid";
		case CookedColliderStatus::StaleLastKnownGood: return "StaleLastKnownGood";
		case CookedColliderStatus::InvalidUnpublished: return "InvalidUnpublished";
		}
		return "Unknown";
	}

	std::string_view ToString(CookedColliderDiagnosticSeverity severity)
	{
		switch(severity)
		{
		case CookedColliderDiagnosticSeverity::Info: return "Info";
		case CookedColliderDiagnosticSeverity::Warning: return "Warning";
		case CookedColliderDiagnosticSeverity::Error: return "Error";
		}
		return "Unknown";
	}

	std::string_view ToString(CookedColliderValidationCode code)
	{
		switch(code)
		{
		case CookedColliderValidationCode::None: return "None";
		case CookedColliderValidationCode::InvalidMagic: return "InvalidMagic";
		case CookedColliderValidationCode::UnsupportedVersion: return "UnsupportedVersion";
		case CookedColliderValidationCode::UnsupportedEndianness: return "UnsupportedEndianness";
		case CookedColliderValidationCode::MissingAssetId: return "MissingAssetId";
		case CookedColliderValidationCode::MissingSourceAssetId: return "MissingSourceAssetId";
		case CookedColliderValidationCode::MissingSourceHash: return "MissingSourceHash";
		case CookedColliderValidationCode::MissingRecipeHash: return "MissingRecipeHash";
		case CookedColliderValidationCode::InvalidProviderFamily: return "InvalidProviderFamily";
		case CookedColliderValidationCode::ProviderVersionMismatch: return "ProviderVersionMismatch";
		case CookedColliderValidationCode::CookRevisionMismatch: return "CookRevisionMismatch";
		case CookedColliderValidationCode::PayloadFormatFamilyMismatch: return "PayloadFormatFamilyMismatch";
		case CookedColliderValidationCode::PayloadFormatVersionMismatch: return "PayloadFormatVersionMismatch";
		case CookedColliderValidationCode::InvalidPlatform: return "InvalidPlatform";
		case CookedColliderValidationCode::InvalidGeometryCounts: return "InvalidGeometryCounts";
		case CookedColliderValidationCode::InvalidBounds: return "InvalidBounds";
		case CookedColliderValidationCode::EmptyPayload: return "EmptyPayload";
		case CookedColliderValidationCode::PayloadLimitExceeded: return "PayloadLimitExceeded";
		case CookedColliderValidationCode::PayloadSizeMismatch: return "PayloadSizeMismatch";
		case CookedColliderValidationCode::ChecksumMismatch: return "ChecksumMismatch";
		case CookedColliderValidationCode::InvalidStatus: return "InvalidStatus";
		case CookedColliderValidationCode::InvalidLastKnownGood: return "InvalidLastKnownGood";
		case CookedColliderValidationCode::MalformedEnvelope: return "MalformedEnvelope";
		case CookedColliderValidationCode::TrailingData: return "TrailingData";
		}
		return "Unknown";
	}
}
