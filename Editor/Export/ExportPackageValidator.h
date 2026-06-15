// ExportPackageValidator.h - validates the runtime contract of a completed TEPAK export.
//
// The export pipeline packages engine-owned runtime files and project assets into
// one archive. This helper checks that archive before output staging so packaged
// builds fail early with actionable missing-path errors.

#pragma once

#include <filesystem>
#include <string>

bool ValidateExportPackage(const std::filesystem::path& packagePath, std::string& outError);
