// TepakPackager.h — TEPAK archive packaging interface
//
// Provides a wrapper for creating TEPAK archives as part of the export process. it defines the progress reporting and result structures used to communicate the status of asset packaging to the editor UI.

#pragma once

#include "BuildProfile.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

/// Progress report emitted periodically during TEPAK packaging.
struct PackageProgress {
	uint64_t totalBytes = 0;
	uint64_t processedBytes = 0;
	uint32_t totalEntries = 0;
	uint32_t completedEntries = 0;
	double   elapsedSeconds = 0.0;
	double   estimatedRemaining = 0.0;
	std::string currentEntry;
	bool     complete = false;
};

using PackageProgressCallback = std::function<void(const PackageProgress&)>;

/// Result of a packaging operation.

struct PackageResult {
	bool success = false;
	std::filesystem::path outputPath;
	uint64_t archiveSize = 0;
	uint32_t entryCount = 0;
	double   elapsedSeconds = 0.0;
	std::string errorMessage;
};

/// Creates TEPAK archives from project assets for export builds.

class TepakPackager {
public:
	/// Packages the given asset directory into a .tepak archive using the
	/// settings from the build profile.
	/// @param assetDirectory Root directory of compiled assets to package.
	/// @param outputPath Destination path for the .tepak file.
	/// @param profile Build profile providing compression and worker settings.
	/// @param progressCallback Optional callback for progress reporting.
	PackageResult Package(
		const std::filesystem::path& assetDirectory,
		const std::filesystem::path& outputPath,
		const BuildProfile& profile,
		PackageProgressCallback progressCallback = nullptr) const;

private:
	/// Maps the BuildProfile compression preset to a zstd compression level.
	static int32_t ResolveCompressionLevel(CompressionPreset preset);
};
