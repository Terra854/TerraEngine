// NavigationGeometryCompiler.h - deterministic provider-neutral companion cook.

#pragma once

#include "Navigation/Geometry/NavigationGeometryFormat.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::Editor {
	struct NavigationGeometryCompileRequest {
		Navigation::NavigationGeometryDocument geometry;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t settingsFingerprint = 0U;
		const std::atomic_bool* cancellationRequested = nullptr;
	};

	struct NavigationGeometryCompileResult {
		bool accepted = false;
		bool cancelled = false;
		std::string reason;
		std::vector<std::byte> bytes;
		std::uint64_t sourceFingerprint = 0U;
		std::uint64_t settingsFingerprint = 0U;
		std::uint64_t checksum = 0U;
	};

	struct NavigationGeometryPublishRequest {
		NavigationGeometryCompileRequest compile;
		std::filesystem::path outputPath;
	};

	// The caller schedules this bounded value operation on the shared engine task
	// pool. The function creates no thread and reads no live AssetDatabase state.
	[[nodiscard]] NavigationGeometryCompileResult CompileNavigationGeometry(
		const NavigationGeometryCompileRequest& request);

	// A complete runtime-decodable staging file is flushed before the narrow
	// atomic replacement boundary. Failure preserves the last-known-good output.
	[[nodiscard]] NavigationGeometryCompileResult CompileAndPublishNavigationGeometry(
		const NavigationGeometryPublishRequest& request);
}
