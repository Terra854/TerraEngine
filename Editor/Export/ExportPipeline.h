// ExportPipeline.h — Export build process orchestrator
//
// Orchestrates the complete end-to-end export workflow for TerraEngine projects. It coordinates between validation, managed code compilation, asset staging, and final TEPAK packaging to produce a self-contained runtime distribution.

#pragma once

#include "BuildProfile.h"
#include "ExportValidator.h"
#include "TepakPackager.h"
#include "Resource/ResourceManager.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// Phase of the export pipeline for progress reporting.
enum class ExportPhase : uint32_t {
	Idle        = 0,
	Validating  = 1,
	Compiling   = 2,
	Packaging   = 3,
	Staging     = 4,
	Complete    = 5,
	Failed      = 6,
};

/// Progress report emitted by the export pipeline.
// struct ExportProgress groups related state and operations so callers have one documented contract.
struct ExportProgress {
	ExportPhase phase = ExportPhase::Idle;
	float       overallProgress = 0.0f; // 0.0 – 1.0
	std::string statusMessage;
	bool        finished = false;
};

using ExportProgressCallback = std::function<void(const ExportProgress&)>;

/// Full result of one export pipeline invocation.
// struct ExportResult groups related state and operations so callers have one documented contract.
struct ExportResult {
	bool success = false;
	std::filesystem::path outputDirectory;
	ExportValidationResult validationResult;
	PackageResult packageResult;
	double totalElapsedSeconds = 0.0;
	std::string errorMessage;
};

/// Orchestrates the complete export build pipeline.
// class ExportPipeline groups related state and operations so callers have one documented contract.
class ExportPipeline {
public:
	/// Runs the full export pipeline synchronously and returns the result.
	/// @param profile Build profile with all export settings.
	/// @param projectRoot Root directory of the TerraEngine project.
	/// @param progressCallback Optional callback for phase-level progress.
	ExportResult Execute(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		ExportProgressCallback progressCallback = nullptr);

private:
	/// Phase 1: Pre-export validation.
	bool RunValidation(const BuildProfile& profile, const std::filesystem::path& projectRoot, ExportResult& result, ExportProgressCallback progressCallback);

	/// Phase 2: NativeAOT compilation via dotnet publish.
	bool RunManagedCompilation(const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	/// Phase 3: Asset staging (merging raw and compiled assets).
	bool RunAssetStaging(const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& stagingDir,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	/// Phase 4: TEPAK asset packaging.
	bool RunAssetPackaging(const BuildProfile& profile,
		const std::filesystem::path& stagingDir,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	/// Phase 5: Stage output directory (copy engine binaries and managed runtime).
	bool RunOutputStaging(const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	/// Writes the export manifest into the supplied directory. The manifest now
	/// only contains the export data that still varies per project, such as the
	/// startup scene and asset GUID map, because runtime paths are deterministic.
	bool WriteExportManifest(const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& targetDirectory);

	// struct StagedAsset groups related state and operations so callers have one documented contract.
	struct StagedAsset {
		std::string guid;
		std::string path;
		AssetType type;
	};

	ExportValidator m_Validator;
	TepakPackager   m_Packager;
	std::vector<StagedAsset> m_StagedAssets;
};
