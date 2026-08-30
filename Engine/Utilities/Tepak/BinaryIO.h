// BinaryIO.h - Shared checked binary stream helpers for the TEPAK format.

#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace tepak::util {

	// WritePod writes the exact in-memory bytes of a trivially copyable value.
	// Callers remain responsible for choosing stable on-disk field types and order.
	template<typename T>
	void WritePod(std::ostream& stream, const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>,
			"Only trivially copyable values can be written directly.");
		stream.write(reinterpret_cast<const char*>(&value),
			static_cast<std::streamsize>(sizeof(T)));
		if(!stream) {
			throw std::runtime_error("Failed to write binary value.");
		}
	}

	// ReadPod mirrors WritePod and rejects a truncated or failed stream instead
	// of returning a partially initialized archive field.
	template<typename T>
	[[nodiscard]] T ReadPod(std::istream& stream)
	{
		static_assert(std::is_trivially_copyable_v<T>,
			"Only trivially copyable values can be read directly.");
		T value{};
		stream.read(reinterpret_cast<char*>(&value),
			static_cast<std::streamsize>(sizeof(T)));
		if(!stream) {
			throw std::runtime_error("Failed to read binary value.");
		}
		return value;
	}

	// WritePadding emits deterministic zero bytes between aligned archive fields.
	inline void WritePadding(std::ostream& stream, std::uint64_t bytes)
	{
		if(bytes == 0U) {
			return;
		}

		const std::vector<char> padding(static_cast<std::size_t>(bytes), '\0');
		stream.write(padding.data(), static_cast<std::streamsize>(padding.size()));
		if(!stream) {
			throw std::runtime_error("Failed to write binary padding.");
		}
	}

	// AlignValue rounds a byte position upward without changing an already
	// aligned value. Zero alignment intentionally means no alignment constraint.
	[[nodiscard]] inline std::uint64_t AlignValue(
		std::uint64_t value,
		std::uint64_t alignment)
	{
		if(alignment == 0U) {
			return value;
		}

		const std::uint64_t remainder = value % alignment;
		return remainder == 0U ? value : value + (alignment - remainder);
	}

} // namespace tepak::util
