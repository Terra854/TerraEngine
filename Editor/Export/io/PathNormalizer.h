// PathNormalizer.h — TEPAK path normalization interface
//
// Provides the interface for portable path normalization and asset class detection within the TEPAK system. it defines the mechanisms for inventory discovery and path-to-UTF8 conversion.

#pragma once

#include "TEPAK/PackTypes.h"
#include "model/PackModel.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tepak::io {

std::string NormalizeRelativePath(const std::filesystem::path& inputRoot, const std::filesystem::path& sourcePath);
std::string NormalizeLookupPath(std::string_view logicalPath);
std::string ExtractLeafName(std::string_view normalizedPath);
std::vector<std::string> EnumerateDirectoryPaths(std::string_view normalizedFilePath);
std::string DetectAssetClass(const std::filesystem::path& sourcePath);
model::PackagingInventory DiscoverPackagingInventory(const PackagingRequest& request);

} // namespace tepak::io
