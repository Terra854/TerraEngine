#pragma once

#include "TEPAK/PackTypes.h"

namespace tepak::validation {

void AddFinding(ValidationReport& report,
	ValidationSeverity severity,
	std::string category,
	std::string location,
	std::string ruleIdentifier,
	std::string message,
	std::string entryPath = {});

} // namespace tepak::validation
