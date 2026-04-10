// ExportValidator.h — Pre-export validation for build profiles and managed source.
//
// The validator runs before the export pipeline to catch configuration errors
// and unsupported managed code patterns early. Results are presented in the
// BuildProfilesWindow as a diagnostic list.

#pragma once

#include "BuildProfile.h"

#include <string>
#include <vector>

/// Severity level for a validation finding.
enum class ExportDiagnosticSeverity : uint32_t {
	Info    = 0,
	Warning = 1,
	Error   = 2,
};

/// One discrete finding from the pre-export validation pass.
struct ExportDiagnostic {
	ExportDiagnosticSeverity severity = ExportDiagnosticSeverity::Info;
	std::string category;   // e.g. "Profile", "ManagedSource", "Assets"
	std::string message;    // Human-readable description
	std::string filePath;   // Optional: source file or asset path
	int line = 0;           // Optional: source line number
};

/// Aggregated result of one validation pass.
struct ExportValidationResult {
	bool passed = true;      // false if any Error-level diagnostics exist
	uint32_t errorCount = 0;
	uint32_t warningCount = 0;
	std::vector<ExportDiagnostic> diagnostics;
};

/// Validates a build profile and optionally scans managed source code for
/// unsupported patterns (non-blittable fields, reflection usage, etc.).
class ExportValidator {
public:
	/// Validates the build profile settings (paths, platform, etc.).
	ExportValidationResult ValidateProfile(const BuildProfile& profile) const;

/// Scans managed source files for patterns incompatible with NativeAOT
/// export, including reflection emit, dynamic binder usage, and serialized
/// field types outside the fixed-size v1 hydration ABI subset.
	ExportValidationResult ValidateManagedSource(const std::filesystem::path& projectRoot) const;

	/// Runs both profile and managed source validation.
	ExportValidationResult ValidateAll(const BuildProfile& profile, const std::filesystem::path& projectRoot) const;
};
