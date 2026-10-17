// Version.h — Contains declarations for the Version constants, type definitions, and lightweight declarations used by public runtime TEPAK reader and archive type declarations.

#pragma once

#include <string_view>

namespace tepak {

// Project release and public pack-format versions are separate constants.
struct Version final {
	static constexpr std::string_view kProjectName = "TEPAK";
	static constexpr std::string_view kProjectVersion = "0.1.0";
	static constexpr std::string_view kPublicFormatDesignation = "1.0";
	static constexpr unsigned int kVersionMajor = 1U;
	static constexpr unsigned int kVersionMinor = 0U;
};

} // namespace tepak
