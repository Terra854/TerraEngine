// BinaryIO.h — Contains declarations for the Binary IO file-specific types, helper functions, and module integration code used by binary IO helpers and worker coordination used by runtime content reads.

#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace tepak::util {

template <typename T>
// WritePod is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WritePod(std::ostream& stream, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>, "Only POD values can be written directly.");
	stream.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!stream) {
		throw std::runtime_error("Failed to write binary value.");
	}
}

template <typename T>
// ReadPod is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
T ReadPod(std::istream& stream) {
	static_assert(std::is_trivially_copyable_v<T>, "Only POD values can be read directly.");
	T value {};
	stream.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!stream) {
		throw std::runtime_error("Failed to read binary value.");
	}
	return value;
}

// WritePadding is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void WritePadding(std::ostream& stream, std::uint64_t bytes) {
	if (bytes == 0U) {
		return;
	}

	const std::vector<char> padding(static_cast<std::size_t>(bytes), '\0');
	stream.write(padding.data(), static_cast<std::streamsize>(padding.size()));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!stream) {
		throw std::runtime_error("Failed to write binary padding.");
	}
}

// AlignValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::uint64_t AlignValue(std::uint64_t value, std::uint64_t alignment) {
	if (alignment == 0U) {
		return value;
	}

	const auto remainder = value % alignment;
	return remainder == 0U ? value : (value + (alignment - remainder));
}

} // namespace tepak::util
