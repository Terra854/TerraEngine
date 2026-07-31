#include "VirtualGeometryQuantization.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace TerraEngine::Editor
{
	namespace
	{
		// The selected values are inside the public ranges in
		// quality-profiles.json. A settings hash covers the exact choices so a
		// rule change invalidates recipes and resident page generations.
		constexpr std::array<std::uint8_t, 6u> kCinematicBits{ 16u, 12u, 10u, 16u, 8u, 16u };
		constexpr std::array<std::uint8_t, 6u> kBalancedBits{ 14u, 10u, 9u, 14u, 8u, 14u };
		constexpr std::array<std::uint8_t, 6u> kMemoryBits{ 12u, 8u, 8u, 12u, 8u, 12u };
		constexpr std::uint64_t kCinematicSettingsHash = 0xd31359a6f2a21efbull;
		constexpr std::uint64_t kBalancedSettingsHash = 0x7f0ae2f0f147540bull;
		constexpr std::uint64_t kMemorySettingsHash = 0x38930e33853e606bull;
		constexpr VirtualGeometryQuantizationProfile kCinematicProfile{
			VirtualGeometryQuantizationProfileName::Cinematic, 1u, kCinematicBits, 0.0001f, 0.0001f,
			kCinematicSettingsHash };
		constexpr VirtualGeometryQuantizationProfile kBalancedProfile{
			VirtualGeometryQuantizationProfileName::Balanced, 1u, kBalancedBits, 0.0005f, 0.0005f,
			kBalancedSettingsHash };
		constexpr VirtualGeometryQuantizationProfile kMemoryProfile{
			VirtualGeometryQuantizationProfileName::Memory, 1u, kMemoryBits, 0.0010f, 0.0010f,
			kMemorySettingsHash };

		[[nodiscard]] constexpr std::size_t ToAttributeIndex(VirtualGeometryQuantizationAttribute attribute) noexcept
		{
			return static_cast<std::size_t>(attribute);
		}

		[[nodiscard]] constexpr std::uint32_t MaximumQuantizedValue(std::uint8_t bits) noexcept
		{
			return bits == 0u || bits > 24u ? 0u : (1u << bits) - 1u;
		}

		void HashQuantizationValue(std::uint64_t& hash, const void* data, std::size_t size) noexcept
		{
			for (const auto* byte = static_cast<const std::uint8_t*>(data); size-- != 0u; ++byte)
			{
				hash = (hash ^ *byte) * 1099511628211ull;
			}
		}

		[[nodiscard]] bool IsFiniteRange(
			const VirtualGeometryProductionQuantizationStreamRange& range) noexcept
		{
			if (range.stableIdentity == 0u || range.componentCount == 0u || range.componentCount > 4u
				|| static_cast<std::uint32_t>(range.scope) > static_cast<std::uint32_t>(TemfQuantizationDomainScope::DeformableEnvelope)
				|| static_cast<std::uint32_t>(range.streamKind) > static_cast<std::uint32_t>(TemfStreamKind::Metadata)
				|| (range.scope == TemfQuantizationDomainScope::SharedBoundary && !range.protectsSharedBoundary)
				|| (range.scope == TemfQuantizationDomainScope::DeformableEnvelope && !range.includesDeformationEnvelope))
			{
				return false;
			}
			for (std::size_t component = 0u; component < range.componentCount; ++component)
			{
				if (!std::isfinite(range.origin[component]) || !std::isfinite(range.extent[component])
					|| range.extent[component] < 0.0 || range.extent[component] > 1.0e8)
				{
					return false;
				}
			}
			return true;
		}
	} // namespace

	bool VirtualGeometryQuantizationBounds::Valid() const noexcept
	{
		for (std::size_t axis = 0u; axis < minimum.size(); ++axis)
		{
			if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || maximum[axis] <= minimum[axis])
			{
				return false;
			}
		}
		return true;
	}

	bool VirtualGeometryQuantizationProfile::Valid() const noexcept
	{
		if (version == 0u || !std::isfinite(maximumPositionError) || maximumPositionError <= 0.0f ||
			!std::isfinite(maximumDeformationError) || maximumDeformationError <= 0.0f
			|| settingsHash == 0u)
		{
			return false;
		}
		return std::all_of(attributeBits.begin(), attributeBits.end(), [](std::uint8_t bits) { return bits >= 2u && bits <= 24u; });
	}

	bool VirtualGeometryQuantizationDomain::Valid() const noexcept
	{
		return stableId != 0u && bounds.Valid() &&
			(kind != VirtualGeometryQuantizationDomainKind::SharedBoundary || protectsSharedBoundary) &&
			(kind != VirtualGeometryQuantizationDomainKind::DeformableEnvelope || includesDeformationEnvelope);
	}

	VirtualGeometryProductionQuantizationResult BuildVirtualGeometryProductionQuantizationDomains(
		const VirtualGeometryProductionQuantizationRequest& request) noexcept
	{
		VirtualGeometryProductionQuantizationResult result{};
		const auto& profile = GetVirtualGeometryQuantizationProfile(request.profile);
		if (!profile.Valid() || request.streams.empty())
		{
			result.diagnostic = "production quantization needs a valid named profile and at least one stream range";
			return result;
		}

		result.settingsHash = profile.settingsHash;
		result.domains.reserve(request.streams.size());
		for (const VirtualGeometryProductionQuantizationStreamRange& range : request.streams)
		{
			if (!IsFiniteRange(range))
			{
				result.domains.clear();
				result.diagnostic = "production quantization stream range is invalid or omits required boundary coverage";
				return result;
			}
			const std::uint8_t bits = GetVirtualGeometryQuantizationBits(profile, range.attribute);
			const std::uint32_t maximumValue = MaximumQuantizedValue(bits);
			if (maximumValue == 0u)
			{
				result.domains.clear();
				result.diagnostic = "production quantization profile has no supported bit width for an attribute";
				return result;
			}

			TemfQuantizationDomain domain{};
			domain.domainId = static_cast<std::uint32_t>(result.domains.size());
			domain.domainVersion = static_cast<std::uint16_t>(profile.version);
			domain.domainScope = static_cast<std::uint16_t>(range.scope);
			domain.streamKind = static_cast<std::uint16_t>(range.streamKind);
			domain.componentCount = range.componentCount;
			domain.signedness = range.signedness;
			domain.flags = range.flags;
			for (std::size_t component = 0u; component < range.componentCount; ++component)
			{
				domain.componentBitWidths[component] = bits;
				domain.origin[component] = range.origin[component];
				domain.extent[component] = range.extent[component];
				// Zero-extent components are intentional constants. Their zero
				// step makes that rule explicit without dividing by zero.
				domain.quantizationStep[component] = range.extent[component] == 0.0
					? 0.0 : range.extent[component] / static_cast<double>(maximumValue);
			}
			std::uint64_t domainHash = profile.settingsHash;
			HashQuantizationValue(domainHash, &range.stableIdentity, sizeof(range.stableIdentity));
			HashQuantizationValue(domainHash, &domain, offsetof(TemfQuantizationDomain, domainHash));
			domain.domainHash = domainHash;
			result.domains.push_back(domain);
		}

		std::uint64_t setHash = profile.settingsHash;
		for (const TemfQuantizationDomain& domain : result.domains)
		{
			HashQuantizationValue(setHash, &domain.domainHash, sizeof(domain.domainHash));
		}
		result.domainSetHash = setHash;
		return result;
	}

	const VirtualGeometryQuantizationProfile &GetVirtualGeometryQuantizationProfile(VirtualGeometryQuantizationProfileName name) noexcept
	{
		switch (name)
		{
		case VirtualGeometryQuantizationProfileName::Cinematic: return kCinematicProfile;
		case VirtualGeometryQuantizationProfileName::Memory: return kMemoryProfile;
		case VirtualGeometryQuantizationProfileName::Balanced: return kBalancedProfile;
		}
		return kBalancedProfile;
	}

	std::uint8_t GetVirtualGeometryQuantizationBits(
		const VirtualGeometryQuantizationProfile &profile, VirtualGeometryQuantizationAttribute attribute) noexcept
	{
		const std::size_t index = ToAttributeIndex(attribute);
		return index < profile.attributeBits.size() ? profile.attributeBits[index] : 0u;
	}

	VirtualGeometryQuantizationAnalysis AnalyzeVirtualGeometryQuantization(
		const VirtualGeometryQuantizationDomain &domain, const VirtualGeometryQuantizationProfile &profile) noexcept
	{
		VirtualGeometryQuantizationAnalysis analysis{};
		if (!profile.Valid())
		{
			analysis.suitability = VirtualGeometryQuantizationSuitability::InvalidProfile;
			analysis.diagnostic = "quantization profile has an unsupported version, error limit, or bit count";
			return analysis;
		}
		if (domain.stableId == 0u)
		{
			analysis.suitability = VirtualGeometryQuantizationSuitability::NonFiniteInput;
			analysis.diagnostic = "quantization domain needs a stable source identity before page work";
			return analysis;
		}
		if ((domain.kind == VirtualGeometryQuantizationDomainKind::SharedBoundary && !domain.protectsSharedBoundary) ||
			(domain.kind == VirtualGeometryQuantizationDomainKind::DeformableEnvelope && !domain.includesDeformationEnvelope))
		{
			analysis.suitability = VirtualGeometryQuantizationSuitability::NonFiniteInput;
			analysis.diagnostic = "quantization domain omitted required shared-boundary or deformation-envelope coverage";
			return analysis;
		}

		for (std::size_t axis = 0u; axis < domain.bounds.minimum.size(); ++axis)
		{
			const float minimum = domain.bounds.minimum[axis];
			const float maximum = domain.bounds.maximum[axis];
			if (!std::isfinite(minimum) || !std::isfinite(maximum))
			{
				analysis.suitability = VirtualGeometryQuantizationSuitability::NonFiniteInput;
				analysis.diagnostic = "quantization bounds contain a non-finite coordinate";
				return analysis;
			}
			const float extent = maximum - minimum;
			if (extent <= 0.0f)
			{
				analysis.suitability = VirtualGeometryQuantizationSuitability::DegenerateExtent;
				analysis.diagnostic = "quantization domain has a zero or inverted extent";
				return analysis;
			}
			if (!std::isfinite(extent) || extent >= 1.0e8f)
			{
				analysis.suitability = VirtualGeometryQuantizationSuitability::ExtentTooLarge;
				analysis.diagnostic = "quantization domain extent exceeds the conservative production limit";
				return analysis;
			}
			const std::uint32_t maximumValue = MaximumQuantizedValue(GetVirtualGeometryQuantizationBits(profile, VirtualGeometryQuantizationAttribute::Position));
			analysis.halfStepError[axis] = maximumValue == 0u ? 0.0f : extent / (2.0f * static_cast<float>(maximumValue));
		}
		analysis.maximumEuclideanError = std::sqrt(
			analysis.halfStepError[0u] * analysis.halfStepError[0u] + analysis.halfStepError[1u] * analysis.halfStepError[1u] +
			analysis.halfStepError[2u] * analysis.halfStepError[2u]);
		if (analysis.maximumEuclideanError > profile.maximumPositionError)
		{
			analysis.suitability = VirtualGeometryQuantizationSuitability::ErrorLimitExceeded;
			analysis.diagnostic = "quantization profile cannot meet its declared position-error limit for this domain";
			return analysis;
		}
		analysis.suitability = VirtualGeometryQuantizationSuitability::Suitable;
		analysis.diagnostic = "quantization profile meets the declared half-step position-error limit";
		return analysis;
	}

	std::uint32_t QuantizeVirtualGeometryAttribute(float value, float minimum, float maximum, std::uint8_t bits) noexcept
	{
		const std::uint32_t maximumValue = MaximumQuantizedValue(bits);
		if (maximumValue == 0u || !std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
		{
			return 0u;
		}
		const float normalized = std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
		return static_cast<std::uint32_t>(std::lround(normalized * static_cast<float>(maximumValue)));
	}

	float DequantizeVirtualGeometryAttribute(std::uint32_t value, float minimum, float maximum, std::uint8_t bits) noexcept
	{
		const std::uint32_t maximumValue = MaximumQuantizedValue(bits);
		if (maximumValue == 0u || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
		{
			return 0.0f;
		}
		const float normalized = static_cast<float>(std::min(value, maximumValue)) / static_cast<float>(maximumValue);
		return minimum + normalized * (maximum - minimum);
	}

	VirtualGeometryQuantizedPosition QuantizeVirtualGeometryPosition(const std::array<float, 3> &position, const VirtualGeometryQuantizationBounds &bounds)
	{
		VirtualGeometryQuantizedPosition result{};
		if (!bounds.Valid())
		{
			return result;
		}
		for (std::size_t axis = 0u; axis < result.size(); ++axis)
			result[axis] = static_cast<std::uint16_t>(QuantizeVirtualGeometryAttribute(position[axis], bounds.minimum[axis], bounds.maximum[axis], 16u));
		return result;
	}

	std::array<float, 3> DequantizeVirtualGeometryPosition(const VirtualGeometryQuantizedPosition &position, const VirtualGeometryQuantizationBounds &bounds)
	{
		std::array<float, 3> result{};
		if (!bounds.Valid())
		{
			return result;
		}
		for (std::size_t axis = 0u; axis < result.size(); ++axis)
			result[axis] = DequantizeVirtualGeometryAttribute(position[axis], bounds.minimum[axis], bounds.maximum[axis], 16u);
		return result;
	}

	bool ValidateVirtualGeometryWatertightBoundary(
		std::span<const VirtualGeometryQuantizedPosition> first, std::span<const VirtualGeometryQuantizedPosition> second) noexcept
	{
		return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin());
	}
} // namespace TerraEngine::Editor
