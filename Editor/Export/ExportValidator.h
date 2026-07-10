// ExportValidator.h — Pre-export validation system
//
// Defines the diagnostic levels and interface for the pre-export validation system. It provides mechanisms for checking build profile integrity and scanning managed source code for patterns incompatible with NativeAOT.

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
// struct ExportDiagnostic groups related state and operations so callers have one documented contract.
struct ExportDiagnostic {
	ExportDiagnosticSeverity severity = ExportDiagnosticSeverity::Info;
	std::string category;   // e.g. "Profile", "ManagedSource", "Assets"
	std::string message;    // Human-readable description
	std::string filePath;   // Optional: source file or asset path
	int line = 0;           // Optional: source line number
	// These stable automation fields augment the existing editor-facing text.
	// Empty values are normalized by the evidence writer for legacy findings.
	std::string stage;
	std::string code;
	std::string suggestion;
};

/// Aggregated result of one validation pass.
// struct ExportValidationResult groups related state and operations so callers have one documented contract.
struct ExportValidationResult {
	bool passed = true;      // false if any Error-level diagnostics exist
	uint32_t errorCount = 0;
	uint32_t warningCount = 0;
	std::vector<ExportDiagnostic> diagnostics;
	// Material summaries are duplicated from diagnostics so export UI and tests
	// can display product-facing fallback decisions without scraping generic
	// human-readable diagnostic messages.
	std::vector<std::string> materialFallbackDiagnostics;
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
	ExportValidationResult ValidateAll(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& runtimeShaderDirectory = {}) const;
};
