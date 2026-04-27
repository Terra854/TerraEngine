// ValidationReport.h — TEPAK validation reporting interface
//
// Defines the interface for collecting and managing findings during an archive validation pass. It provides helpers for adding discrete diagnostics to a validation report, enabling detailed feedback on archive health and compliance.

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
