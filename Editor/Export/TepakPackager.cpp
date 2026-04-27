// TepakPackager.cpp — TEPAK archive packaging implementation
//
// Implements the asset packaging logic by interfacing with the TEPAK PackWriter API. It translates build profile settings into packaging requests, manages worker thread allocation, and handles zstd compression level mapping for optimized archive creation.

#include "TepakPackager.h"

#include "TEPAK/PackWriter.h"
#include "TEPAK/PackTypes.h"

#include <chrono>
#include <iostream>
#include <thread>

// --- Compression level resolution ---

int32_t TepakPackager::ResolveCompressionLevel(CompressionPreset preset) {
	// These levels balance export time against runtime decode speed. Higher
	// levels produce smaller archives but do not significantly slow decode
	// because TEPAK decompresses independent extents.
	switch (preset) {
	case CompressionPreset::None:   return 0;
	case CompressionPreset::Fast:   return 3;
	case CompressionPreset::Normal: return 9;
	case CompressionPreset::Best:   return 19;
	default:                         return 9;
	}
}

// --- Packaging ---

PackageResult TepakPackager::Package(
	const std::filesystem::path& assetDirectory,
	const std::filesystem::path& outputPath,
	const BuildProfile& profile,
	PackageProgressCallback progressCallback) const
{
	PackageResult result;
	const auto startTime = std::chrono::steady_clock::now();

	// Ensure the asset directory exists.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(assetDirectory) ||
		!std::filesystem::is_directory(assetDirectory)) {
		result.success = false;
		result.errorMessage = "Asset directory does not exist: " + assetDirectory.string();
		return result;
	}

	// Build the TEPAK PackagingRequest from the profile settings.
	tepak::PackagingRequest request;
	request.inputRoot = assetDirectory;
	request.outputPackPath = outputPath;
	request.overwriteExisting = true;
	request.deterministicMode = true;

	// Resolve compression settings from the profile preset.
	const bool compressEnabled = (profile.compressionPreset != CompressionPreset::None);
	request.enableCompression = compressEnabled;
	request.compressionLevel = ResolveCompressionLevel(profile.compressionPreset);

	// Dictionary training is only useful when compression is enabled.
	request.enableDictionaryTraining = compressEnabled;

	// Worker count: 0 means auto-detect (use hardware concurrency).
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (profile.tepakWorkerCount == 0) {
		const auto hwThreads = std::thread::hardware_concurrency();
		request.workerCount = hwThreads > 0 ? hwThreads : 4;
	}
	else {
		request.workerCount = profile.tepakWorkerCount;
	}

	// Wire up the progress callback to translate TEPAK progress into
	// our simplified PackageProgress struct.
	if (progressCallback) {
		request.progressCallback = [&progressCallback, &startTime](const tepak::PackProgress& p) {
			PackageProgress progress;
			progress.totalBytes = p.totalLogicalBytes;
			progress.processedBytes = p.processedLogicalBytes;
			progress.totalEntries = p.totalEntries;
			progress.completedEntries = p.completedEntries;
			progress.elapsedSeconds = p.elapsedSeconds;
			progress.estimatedRemaining = p.estimatedRemainingSeconds;
			progress.currentEntry = p.currentEntryPath;
			progress.complete = p.finalUpdate;
			progressCallback(progress);
		};
		request.progressUpdateIntervalMs = 500; // Update UI twice per second.
	}

	try {
		// Create the output directory if it doesn't exist.
		std::filesystem::create_directories(outputPath.parent_path());

		// Run the TEPAK pack writer. This discovers files, trains a dictionary,
		// compresses extents across worker threads, and writes the archive.
		const auto writeResult = tepak::PackWriter::Create(request);

		const auto endTime = std::chrono::steady_clock::now();
		const double elapsed = std::chrono::duration<double>(endTime - startTime).count();

		result.success = true;
		result.outputPath = outputPath;
		result.archiveSize = writeResult.descriptor.archiveSize;
		result.entryCount = writeResult.descriptor.fileCount;
		result.elapsedSeconds = elapsed;
	}
	catch (const std::exception& e) {
		result.success = false;
		result.errorMessage = std::string("TEPAK packaging failed: ") + e.what();
	}

	return result;
}
