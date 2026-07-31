// VirtualGeometryQuantization.h - globally anchored position quantization.
//
// Every cluster in one asset uses the same bounds. Shared vertices therefore
// produce the same integer representation even when neighboring clusters are
// encoded by different worker jobs or placed on different physical pages.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include "Engine/File Formats/TerraEngineMeshFile.h"

namespace TerraEngine::Editor
{
	struct VirtualGeometryQuantizationBounds
	{
		std::array<float, 3> minimum{};
		std::array<float, 3> maximum{};

		[[nodiscard]] bool Valid() const noexcept;
	};

	using VirtualGeometryQuantizedPosition = std::array<std::uint16_t, 3>;

	// Profile names are serialized cook choices. Keep their versions alongside
	// the declaration so a future profile revision cannot silently reinterpret
	// already-cooked attribute data.
	enum class VirtualGeometryQuantizationProfileName : std::uint8_t
	{
		Cinematic,
		Balanced,
		Memory
	};

	enum class VirtualGeometryQuantizationAttribute : std::uint8_t
	{
		Position,
		Normal,
		Tangent,
		TextureCoordinate,
		SkinWeight,
		MorphDelta
	};

	enum class VirtualGeometryQuantizationDomainKind : std::uint8_t
	{
		SharedBoundary,
		Page,
		Asset,
		DeformableEnvelope
	};

	struct VirtualGeometryQuantizationProfile
	{
		VirtualGeometryQuantizationProfileName name = VirtualGeometryQuantizationProfileName::Balanced;
		std::uint32_t version = 1u;
		std::array<std::uint8_t, 6u> attributeBits{};
		float maximumPositionError = 0.0f;
		float maximumDeformationError = 0.0f;
		std::uint64_t settingsHash = 0u;

		[[nodiscard]] bool Valid() const noexcept;
	};

	struct VirtualGeometryQuantizationDomain
	{
		VirtualGeometryQuantizationDomainKind kind = VirtualGeometryQuantizationDomainKind::Asset;
		std::uint64_t stableId = 0u;
		VirtualGeometryQuantizationBounds bounds{};
		bool protectsSharedBoundary = false;
		bool includesDeformationEnvelope = false;

		[[nodiscard]] bool Valid() const noexcept;
	};

	enum class VirtualGeometryQuantizationSuitability : std::uint8_t
	{
		Suitable,
		InvalidProfile,
		NonFiniteInput,
		DegenerateExtent,
		ExtentTooLarge,
		ErrorLimitExceeded
	};

	struct VirtualGeometryQuantizationAnalysis
	{
		VirtualGeometryQuantizationSuitability suitability = VirtualGeometryQuantizationSuitability::NonFiniteInput;
		std::array<float, 3u> halfStepError{};
		float maximumEuclideanError = 0.0f;
		std::string_view diagnostic = "quantization input was not analyzed";

		[[nodiscard]] bool Suitable() const noexcept { return suitability == VirtualGeometryQuantizationSuitability::Suitable; }
	};

	// A stream range is supplied from normalized source data before worker jobs
	// encode blocks. Keeping the source range explicit lets the cooker use one
	// shared grid for protected seams while still choosing page-local grids for
	// interior attributes.
	struct VirtualGeometryProductionQuantizationStreamRange
	{
		VirtualGeometryQuantizationAttribute attribute = VirtualGeometryQuantizationAttribute::Position;
		TemfQuantizationDomainScope scope = TemfQuantizationDomainScope::Asset;
		TemfStreamKind streamKind = TemfStreamKind::Position;
		std::uint64_t stableIdentity = 0u;
		std::uint8_t componentCount = 0u;
		std::uint8_t signedness = 0u;
		std::uint32_t flags = 0u;
		std::array<double, 4u> origin{};
		std::array<double, 4u> extent{};
		bool protectsSharedBoundary = false;
		bool includesDeformationEnvelope = false;
	};

	struct VirtualGeometryProductionQuantizationRequest
	{
		VirtualGeometryQuantizationProfileName profile = VirtualGeometryQuantizationProfileName::Balanced;
		std::span<const VirtualGeometryProductionQuantizationStreamRange> streams;
	};

	struct VirtualGeometryProductionQuantizationResult
	{
		std::vector<TemfQuantizationDomain> domains;
		std::uint64_t settingsHash = 0u;
		std::uint64_t domainSetHash = 0u;
		std::string diagnostic;

		[[nodiscard]] bool IsComplete() const noexcept
		{
			return !domains.empty() && settingsHash != 0u && domainSetHash != 0u && diagnostic.empty();
		}
	};

	[[nodiscard]] VirtualGeometryProductionQuantizationResult BuildVirtualGeometryProductionQuantizationDomains(
		const VirtualGeometryProductionQuantizationRequest& request) noexcept;

	[[nodiscard]] const VirtualGeometryQuantizationProfile &GetVirtualGeometryQuantizationProfile(
		VirtualGeometryQuantizationProfileName name) noexcept;

	[[nodiscard]] std::uint8_t GetVirtualGeometryQuantizationBits(
		const VirtualGeometryQuantizationProfile &profile, VirtualGeometryQuantizationAttribute attribute) noexcept;

	// Half a quantization step is the maximum reconstruction error along one
	// axis. The Euclidean value conservatively combines all three axis errors.
	[[nodiscard]] VirtualGeometryQuantizationAnalysis AnalyzeVirtualGeometryQuantization(
		const VirtualGeometryQuantizationDomain &domain, const VirtualGeometryQuantizationProfile &profile) noexcept;

	[[nodiscard]] std::uint32_t QuantizeVirtualGeometryAttribute(
		float value, float minimum, float maximum, std::uint8_t bits) noexcept;

	[[nodiscard]] float DequantizeVirtualGeometryAttribute(
		std::uint32_t value, float minimum, float maximum, std::uint8_t bits) noexcept;

	[[nodiscard]] VirtualGeometryQuantizedPosition QuantizeVirtualGeometryPosition(
		const std::array<float, 3> &position, const VirtualGeometryQuantizationBounds &bounds);

	[[nodiscard]] std::array<float, 3> DequantizeVirtualGeometryPosition(
		const VirtualGeometryQuantizedPosition &position, const VirtualGeometryQuantizationBounds &bounds);

	// The two spans contain corresponding shared-boundary vertices from
	// independently encoded clusters. Equality proves the encoded seam cannot
	// crack because both sides reconstruct the same integer lattice points.
	[[nodiscard]] bool ValidateVirtualGeometryWatertightBoundary(
		std::span<const VirtualGeometryQuantizedPosition> first, std::span<const VirtualGeometryQuantizedPosition> second) noexcept;
} // namespace TerraEngine::Editor
