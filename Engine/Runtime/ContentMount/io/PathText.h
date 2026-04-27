// PathText.h — Contains declarations for the Path Text constants, type definitions, and lightweight declarations used by runtime content path normalization and UTF path conversion utilities.

#pragma once

#include <filesystem>
#include <string>

namespace tepak::io {

// PathToUtf8String is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::string PathToUtf8String(const std::filesystem::path& path) {
	// TEPAK persists and reports paths as UTF-8 so archive metadata stays
	// stable across Windows code pages and other host-specific encodings.
	const auto utf8Path = path.generic_u8string();
	return std::string(utf8Path.begin(), utf8Path.end());
}

// LowercaseAsciiInPlace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline void LowercaseAsciiInPlace(std::string& text) {
	for (char& value : text) {
		if (value >= 'A' && value <= 'Z') {
			value = static_cast<char>(value - 'A' + 'a');
		}
	}
}

} // namespace tepak::io
