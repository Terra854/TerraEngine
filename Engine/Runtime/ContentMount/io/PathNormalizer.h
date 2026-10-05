// PathNormalizer.h — Contains declarations for the Path Normalizer file-specific types, helper functions, and module integration code used by runtime content path normalization and UTF path conversion utilities.

#pragma once

#include "TEPAK/PackTypes.h"
#include "model/PackModel.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tepak::io {

// Persisted names are validated without repair. Empty is reserved for root.
void ValidateStoredPath(std::string_view path, bool allowRoot);
// Windows case-insensitive identity also catches non-ASCII filename aliases.
std::string StoredPathIdentity(std::string_view path);
std::string NormalizeRelativePath(const std::filesystem::path& inputRoot, const std::filesystem::path& sourcePath);
std::string NormalizeLookupPath(std::string_view logicalPath);
std::string ExtractLeafName(std::string_view normalizedPath);
std::vector<std::string> EnumerateDirectoryPaths(std::string_view normalizedFilePath);
std::string DetectAssetClass(const std::filesystem::path& sourcePath);
model::PackagingInventory DiscoverPackagingInventory(const PackagingRequest& request);

} // namespace tepak::io
