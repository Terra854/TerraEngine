// ExportPipeline.h - shared editor and headless export orchestrator.

#pragma once

#include "ExportTypes.h"
#include "Resource/ResourceManager.h"

#include <filesystem>
#include <string>
#include <vector>

/// Runs every export stage without owning a window, graphics device, or live
/// scene. Callers supply all filesystem and runtime dependencies explicitly.
class ExportPipeline {
public:
	ExportResult Execute(
		const ExportRequest& request,
		ExportProgressCallback progressCallback = nullptr,
		std::shared_ptr<ExportCancellationState> cancellation = {});

private:
	// Long loops and stage boundaries fail through one cancellation outcome.
	void CheckCancellation() const;
	std::shared_ptr<ExportCancellationState> m_Cancellation;
	bool RunValidation(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const ExportRuntimeLayout& runtimeLayout,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	bool RunManagedCompilation(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const ExportRuntimeLayout& runtimeLayout,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	bool RunAssetStaging(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const ExportRuntimeLayout& runtimeLayout,
		const std::filesystem::path& stagingDirectory,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	bool RunAssetPackaging(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& stagingDirectory,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	bool RunOutputStaging(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const ExportRuntimeLayout& runtimeLayout,
		ExportResult& result,
		ExportProgressCallback progressCallback);

	bool WriteExportManifest(
		const BuildProfile& profile,
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& targetDirectory);

	struct StagedAsset {
		std::string guid;
		std::string path;
		AssetType type;
		std::string checksum;
		std::uint64_t byteSize = 0U;
		std::string sourceProvenanceHash;
		std::string recipeHash;
		bool virtualGeometryRequired = false;
	};

	ExportValidator m_Validator;
	TepakPackager m_Packager;
	std::vector<StagedAsset> m_StagedAssets;
	// Built from validated staged physics files only. Keeping this separate from
	// the generic asset map makes source-only collider recipes impossible to
	// accidentally advertise as runtime dependencies.
	PhysicsExportManifest m_PhysicsManifest;
};
