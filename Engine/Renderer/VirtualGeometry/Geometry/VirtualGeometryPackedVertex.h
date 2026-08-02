// VirtualGeometryPackedVertex.h - canonical 128-bit render vertex packing.
//
// The production layout spends more precision on positions than the previous
// 10-bit format while preserving the same 16-byte GPU stride. Named cook
// profiles can select a smaller source lattice; values are expanded into the
// fixed runtime fields so shaders need one vendor-neutral Vulkan ABI.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace TerraEngine
{
	inline constexpr std::uint32_t kVirtualGeometryPackedPositionBits = 16u;
	inline constexpr std::uint32_t kVirtualGeometryPackedNormalBits = 12u;
	inline constexpr std::uint32_t kVirtualGeometryPackedTangentBits = 10u;
	inline constexpr std::uint32_t kVirtualGeometryPackedTextureBits = 16u;

	struct VirtualGeometryPackedVertex
	{
		std::array<std::uint32_t, 4u> words{};
	};

	static_assert(sizeof(VirtualGeometryPackedVertex) == 16u,
		"The renderer-owned packed vertex must remain exactly four GPU words.");

	namespace VirtualGeometryPackedVertexDetail
	{
		[[nodiscard]] inline std::uint32_t MaximumValue(std::uint32_t bits) noexcept
		{
			return bits == 0u || bits > 31u ? 0u : (1u << bits) - 1u;
		}

		inline void InsertBits(
			std::array<std::uint32_t, 4u>& words,
			std::uint32_t bitOffset,
			std::uint32_t bitCount,
			std::uint32_t value) noexcept
		{
			const std::uint32_t wordIndex = bitOffset / 32u;
			const std::uint32_t wordBit = bitOffset % 32u;
			const std::uint32_t mask = MaximumValue(bitCount);
			const std::uint64_t shifted = static_cast<std::uint64_t>(value & mask) << wordBit;
			words[wordIndex] |= static_cast<std::uint32_t>(shifted);
			if (wordBit + bitCount > 32u)
			{
				words[wordIndex + 1u] |= static_cast<std::uint32_t>(shifted >> 32u);
			}
		}

		[[nodiscard]] inline std::uint32_t ExtractBits(
			const std::array<std::uint32_t, 4u>& words,
			std::uint32_t bitOffset,
			std::uint32_t bitCount) noexcept
		{
			const std::uint32_t wordIndex = bitOffset / 32u;
			const std::uint32_t wordBit = bitOffset % 32u;
			std::uint64_t joined = words[wordIndex];
			if (wordBit + bitCount > 32u)
			{
				joined |= static_cast<std::uint64_t>(words[wordIndex + 1u]) << 32u;
			}
			return static_cast<std::uint32_t>((joined >> wordBit) & MaximumValue(bitCount));
		}

		[[nodiscard]] inline std::uint32_t QuantizeProfiledUnit(
			float value,
			std::uint32_t sourceBits,
			std::uint32_t containerBits) noexcept
		{
			const std::uint32_t sourceMaximum = MaximumValue(std::min(sourceBits, containerBits));
			const std::uint32_t containerMaximum = MaximumValue(containerBits);
			if (sourceMaximum == 0u || containerMaximum == 0u || !std::isfinite(value))
			{
				return 0u;
			}
			const double normalized = std::clamp(static_cast<double>(value), 0.0, 1.0);
			const std::uint32_t sourceValue = static_cast<std::uint32_t>(
				std::llround(normalized * static_cast<double>(sourceMaximum)));
			return static_cast<std::uint32_t>(std::llround(
				static_cast<double>(sourceValue) * containerMaximum / sourceMaximum));
		}

		[[nodiscard]] inline std::array<float, 2u> EncodeOctahedral(
			const std::array<float, 3u>& direction) noexcept
		{
			const float length = std::abs(direction[0]) + std::abs(direction[1])
				+ std::abs(direction[2]);
			if (!std::isfinite(length) || length <= 1.0e-20f)
			{
				return { 0.5f, 0.5f };
			}
			float x = direction[0] / length;
			float y = direction[1] / length;
			if (direction[2] < 0.0f)
			{
				const float previousX = x;
				x = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
				y = (1.0f - std::abs(previousX)) * (y >= 0.0f ? 1.0f : -1.0f);
			}
			return { x * 0.5f + 0.5f, y * 0.5f + 0.5f };
		}

		[[nodiscard]] inline std::array<float, 3u> DecodeOctahedral(
			const std::array<float, 2u>& encodedUnit) noexcept
		{
			float x = encodedUnit[0] * 2.0f - 1.0f;
			float y = encodedUnit[1] * 2.0f - 1.0f;
			float z = 1.0f - std::abs(x) - std::abs(y);
			if(z < 0.0f)
			{
				const float previousX = x;
				x = (1.0f - std::abs(y))
					* (previousX >= 0.0f ? 1.0f : -1.0f);
				y = (1.0f - std::abs(previousX))
					* (y >= 0.0f ? 1.0f : -1.0f);
			}
			const float length = std::sqrt(x * x + y * y + z * z);
			if(!std::isfinite(length) || length <= 1.0e-20f)
			{
				return { 0.0f, 0.0f, 1.0f };
			}
			return { x / length, y / length, z / length };
		}
	} // namespace VirtualGeometryPackedVertexDetail

	// Inputs are already normalized to their asset domains. Position is
	// [-1, 1], directions are unit vectors, and texture coordinates are [0, 1].
	[[nodiscard]] inline VirtualGeometryPackedVertex PackVirtualGeometryVertex(
		const std::array<float, 3u>& position,
		const std::array<float, 3u>& normal,
		const std::array<float, 3u>& tangent,
		float tangentSign,
		const std::array<float, 2u>& textureCoordinate,
		std::uint32_t positionProfileBits,
		std::uint32_t normalProfileBits,
		std::uint32_t tangentProfileBits,
		std::uint32_t textureProfileBits) noexcept
	{
		using namespace VirtualGeometryPackedVertexDetail;
		VirtualGeometryPackedVertex result{};
		InsertBits(result.words, 0u, 16u, QuantizeProfiledUnit(
			position[0] * 0.5f + 0.5f, positionProfileBits, kVirtualGeometryPackedPositionBits));
		InsertBits(result.words, 16u, 16u, QuantizeProfiledUnit(
			position[1] * 0.5f + 0.5f, positionProfileBits, kVirtualGeometryPackedPositionBits));
		InsertBits(result.words, 32u, 16u, QuantizeProfiledUnit(
			position[2] * 0.5f + 0.5f, positionProfileBits, kVirtualGeometryPackedPositionBits));

		const std::array<float, 2u> encodedNormal = EncodeOctahedral(normal);
		InsertBits(result.words, 48u, 12u, QuantizeProfiledUnit(
			encodedNormal[0], normalProfileBits, kVirtualGeometryPackedNormalBits));
		InsertBits(result.words, 60u, 12u, QuantizeProfiledUnit(
			encodedNormal[1], normalProfileBits, kVirtualGeometryPackedNormalBits));

		const std::array<float, 2u> encodedTangent = EncodeOctahedral(tangent);
		InsertBits(result.words, 72u, 10u, QuantizeProfiledUnit(
			encodedTangent[0], tangentProfileBits, kVirtualGeometryPackedTangentBits));
		InsertBits(result.words, 82u, 10u, QuantizeProfiledUnit(
			encodedTangent[1], tangentProfileBits, kVirtualGeometryPackedTangentBits));
		InsertBits(result.words, 92u, 1u, tangentSign < 0.0f ? 1u : 0u);

		InsertBits(result.words, 96u, 16u, QuantizeProfiledUnit(
			textureCoordinate[0], textureProfileBits, kVirtualGeometryPackedTextureBits));
		InsertBits(result.words, 112u, 16u, QuantizeProfiledUnit(
			textureCoordinate[1], textureProfileBits, kVirtualGeometryPackedTextureBits));
		return result;
	}

	[[nodiscard]] inline std::array<float, 3u> DecodeVirtualGeometryPackedPosition(
		const VirtualGeometryPackedVertex& vertex) noexcept
	{
		using namespace VirtualGeometryPackedVertexDetail;
		constexpr float inverseMaximum = 1.0f / 65535.0f;
		return {
			static_cast<float>(ExtractBits(vertex.words, 0u, 16u)) * inverseMaximum * 2.0f - 1.0f,
			static_cast<float>(ExtractBits(vertex.words, 16u, 16u)) * inverseMaximum * 2.0f - 1.0f,
			static_cast<float>(ExtractBits(vertex.words, 32u, 16u)) * inverseMaximum * 2.0f - 1.0f
		};
	}

	[[nodiscard]] inline std::array<float, 3u> DecodeVirtualGeometryPackedNormal(
		const VirtualGeometryPackedVertex& vertex) noexcept
	{
		using namespace VirtualGeometryPackedVertexDetail;
		return DecodeOctahedral({
			static_cast<float>(ExtractBits(vertex.words, 48u, 12u)) / 4095.0f,
			static_cast<float>(ExtractBits(vertex.words, 60u, 12u)) / 4095.0f });
	}

	[[nodiscard]] inline std::array<float, 4u> DecodeVirtualGeometryPackedTangent(
		const VirtualGeometryPackedVertex& vertex) noexcept
	{
		using namespace VirtualGeometryPackedVertexDetail;
		const std::array<float, 3u> direction = DecodeOctahedral({
			static_cast<float>(ExtractBits(vertex.words, 72u, 10u)) / 1023.0f,
			static_cast<float>(ExtractBits(vertex.words, 82u, 10u)) / 1023.0f });
		return { direction[0], direction[1], direction[2],
			ExtractBits(vertex.words, 92u, 1u) != 0u ? -1.0f : 1.0f };
	}

	[[nodiscard]] inline std::array<float, 2u> DecodeVirtualGeometryPackedTextureCoordinate(
		const VirtualGeometryPackedVertex& vertex) noexcept
	{
		using namespace VirtualGeometryPackedVertexDetail;
		return {
			static_cast<float>(ExtractBits(vertex.words, 96u, 16u)) / 65535.0f,
			static_cast<float>(ExtractBits(vertex.words, 112u, 16u)) / 65535.0f };
	}
} // namespace TerraEngine
