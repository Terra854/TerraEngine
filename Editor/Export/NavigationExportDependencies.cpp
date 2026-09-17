// NavigationExportDependencies.cpp - checked navigation companion discovery.

#include "NavigationExportDependencies.h"

#include "Navigation/Geometry/NavigationGeometryFormat.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::Export
{
	namespace
	{
		constexpr std::uintmax_t kMaximumNavigationCompanionBytes = 512ULL * 1024ULL * 1024ULL;

		std::vector<std::byte> ReadBoundedBytes(const std::filesystem::path& path)
		{
			std::error_code error;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			if(error || size == 0U || size > kMaximumNavigationCompanionBytes) return {};
			std::ifstream input(path, std::ios::binary);
			if(!input) return {};
			std::vector<std::byte> bytes(static_cast<std::size_t>(size));
			input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return input ? std::move(bytes) : std::vector<std::byte>{};
		}
	}

	NavigationExportClosure NavigationExportDependencyResolver::Resolve(
		const std::filesystem::path& stagedContentRoot)
	{
		NavigationExportClosure result{};
		std::error_code error;
		if(!std::filesystem::is_directory(stagedContentRoot, error)) {
			result.issues.push_back({"navigation.staging_missing", {},
				"Navigation dependency discovery requires a staged content directory."});
			return result;
		}

		for(const auto& entry : std::filesystem::recursive_directory_iterator(
			stagedContentRoot, std::filesystem::directory_options::skip_permission_denied, error)) {
			if(error || !entry.is_regular_file()) continue;
			const std::filesystem::path relative = entry.path().lexically_relative(stagedContentRoot);
			const std::string logicalPath = relative.generic_string();
			const std::string extension = entry.path().extension().string();
			if(extension == ".tenavgeom") {
				const auto bytes = ReadBoundedBytes(entry.path());
				const auto decoded = TerraEngine::Navigation::DecodeNavigationGeometry(bytes);
				if(!decoded.accepted) {
					result.issues.push_back({"navigation.geometry_invalid", logicalPath,
						"Navigation geometry companion is missing, empty, oversized, or malformed: " + decoded.reason});
					continue;
				}
				// Cooked geometry is runtime data, not an authored navigation source.
				// Keep the package source-free even when it contains this companion.
				result.dependencies.push_back({logicalPath, false});
			} else if(extension == ".tenav") {
				// Saved navigation is derived and optional. It is staged for fast startup,
				// but runtime always falls back to checked fresh generation on rejection.
				if(ReadBoundedBytes(entry.path()).empty()) {
					result.issues.push_back({"navigation.cache_invalid", logicalPath,
						"Optional navigation cache is empty or exceeds the export byte limit."});
					continue;
				}
				result.dependencies.push_back({logicalPath, true});
			}
		}
		std::sort(result.dependencies.begin(), result.dependencies.end(), [](const auto& left, const auto& right) {
			return left.logicalPath < right.logicalPath;
		});
		return result;
	}

	bool NavigationExportDependencyResolver::WriteRuntimeManifest(
		const NavigationExportClosure& closure,
		const std::filesystem::path& stagedContentRoot,
		std::string& outError)
	{
		if(!closure.Complete()) {
			outError = closure.issues.front().message;
			return false;
		}
		nlohmann::json dependencies = nlohmann::json::array();
		for(const NavigationExportDependency& dependency : closure.dependencies)
			dependencies.push_back({{"path", dependency.logicalPath}, {"optionalCache", dependency.optionalCache}});
		const nlohmann::json manifest = {
			{"schemaVersion", 1U},
			{"sourceFree", closure.sourceFree},
			{"dependencies", std::move(dependencies)},
			{"runtimeFallback", "fresh_generation"},
			{"notice", "Ground navigation incorporates Recast Navigation under the zlib license."}
		};
		const std::filesystem::path destination = stagedContentRoot / "navigation" / "runtime-manifest.json";
		std::error_code error;
		std::filesystem::create_directories(destination.parent_path(), error);
		if(error) { outError = "Could not create navigation runtime manifest directory."; return false; }
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if(!output) { outError = "Could not open navigation runtime manifest."; return false; }
		output << manifest.dump(2);
		if(!output.good()) { outError = "Could not finish navigation runtime manifest."; return false; }
		return true;
	}
}
