#pragma once

#include "TEPAK/PackTypes.h"

namespace tepak {

// PackValidator performs structural checks and optional full readback so bad
// archives are rejected before they are shipped or mounted at runtime.
class PackValidator final {
public:
	// Validation checks hierarchy structure first, then optionally performs full
	// extent and entry readback so malformed packs fail before they are adopted.
	static ValidationReport Validate(const std::filesystem::path& packPath, bool deepReadback = true);
};

} // namespace tepak
