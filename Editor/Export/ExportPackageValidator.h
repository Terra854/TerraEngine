// ExportPackageValidator.h - validates the runtime contract of a completed TEPAK export.
//
// The export pipeline packages engine-owned runtime files and project assets into
// one archive. This helper checks that archive before output staging so packaged
// builds fail early with actionable missing-path errors.

#pragma once

#include "BuildProfile.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ExportPackageValidationFinding {
	std::string code;
	std::string path;
	std::string message;
	bool required = false;
	bool placeholderDetected = false;
};

struct ExportPackageValidationReport {
	bool passed = false;
	std::filesystem::path archivePath;
	std::uint64_t archiveSize = 0;
	std::uint32_t entryCount = 0;
	std::string archivedManifestJson;
	std::vector<ExportPackageValidationFinding> findings;
	std::string summaryText;
};

bool ValidateExportPackage(const std::filesystem::path& packagePath, std::string& outError);
bool ValidateExportPackage(const std::filesystem::path& packagePath, ExportPlatform platform, std::string& outError);

/// Returns the existing runtime-contract decision together with stable findings
/// and a placeholder scan that evidence and CI can consume without parsing the
/// human-readable failure wording.
ExportPackageValidationReport InspectExportPackage(
	const std::filesystem::path& packagePath,
	ExportPlatform platform);
