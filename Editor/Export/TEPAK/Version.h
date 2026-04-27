// Version.h — TEPAK version information
//
// Contains the constant definitions for the TEPAK library version and binary format designation. It ensures that archive planning and reading operations remain compatible with the supported format version.

#pragma once

#include <string_view>

namespace tepak {

// struct Version groups related state and operations so callers have one documented contract.
struct Version final {
	static constexpr std::string_view kProjectName = "TEPAK";
	static constexpr std::string_view kProjectVersion = "0.1.0";
	static constexpr std::string_view kPublicFormatDesignation = "1.0";
	static constexpr unsigned int kVersionMajor = 1U;
	static constexpr unsigned int kVersionMinor = 0U;
};

} // namespace tepak
