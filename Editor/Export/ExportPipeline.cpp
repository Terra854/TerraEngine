// ExportPipeline.cpp — Export build process implementation
//
// Implements the detailed steps of the export pipeline, including project manifest generation, script binding rewrites for NativeAOT, and output directory staging. It manages the execution of external tools like the dotnet CLI and handles progress reporting throughout the build process.

#include "ExportPipeline.h"
#include "ExportEvidence.h"
#include "ExportOutputTransaction.h"
#include "ExportPackageValidator.h"
#include "ExportScriptBindingRewriter.h"
#include "AssetImportResult.h"
#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

// --- Helpers ---

static std::string PathToUtf8(const std::filesystem::path& path) {
	const std::u8string encoded = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

static void EmitProgress(ExportProgressCallback& cb, ExportPhase phase, float progress, const std::string& message, bool finished = false) {
	if (!cb) return;
	ExportProgress p;
	p.phase = phase;
	p.stage = StageForPhase(phase);
	p.overallProgress = progress;
	p.statusMessage = message;
	p.finished = finished;
	cb(p);
}

// ResolveOutputDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::filesystem::path ResolveOutputDirectory(const BuildProfile& profile, const std::filesystem::path& projectRoot) {
	if (!profile.outputDirectory.empty()) {
		return profile.outputDirectory;
	}

	return projectRoot / "Builds" / profile.name;
}

// ResolveProjectExportName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string ResolveProjectExportName(const std::filesystem::path& projectRoot, const BuildProfile& profile) {
	// Export artifacts are named after the project directory so the launcher can
	// deterministically resolve "<ProjectName>.tepak" back to the matching
	// executable at runtime. If the directory name is somehow empty, fall back
	// to the profile name instead of producing an empty filename.
	std::string projectName = PathToUtf8(projectRoot.filename());
	if (projectName.empty()) {
		projectName = profile.name.empty() ? "TerraEngine" : profile.name;
	}
	return projectName;
}

// ResolveArchiveFileName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string ResolveArchiveFileName(const std::string& projectName) {
	return projectName + ".tepak";
}

// RemoveIfExists is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove(path, errorCode);
}

// RemoveDirectoryIfExists is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveDirectoryIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove_all(path, errorCode);
}

static std::string QualityFromRank(std::uint32_t rank)
{
	if (rank == 0u) return "disabled";
	if (rank == 1u) return "low";
	if (rank == 2u) return "medium";
	return "high";
}

static std::string ShadowQualityFromSettings(const nlohmann::json& settings)
{
	const std::uint32_t maxShadowLights = settings.value("maxShadowLights", 8u);
	const std::uint32_t shadowResolution = settings.value("shadowResolution", 2048u);
	if (maxShadowLights == 0u || shadowResolution == 0u) return "disabled";
	if (maxShadowLights <= 2u || shadowResolution <= 1024u) return "low";
	if (maxShadowLights <= 4u || shadowResolution <= 1536u) return "medium";
	return "high";
}

static std::string AmbientOcclusionQualityFromSettings(const nlohmann::json& settings)
{
	const float strength = settings.value("ambientOcclusionStrength", 1.0f);
	if (strength <= 0.0f) return "disabled";
	return QualityFromRank(settings.value("ambientOcclusionQuality", 2u));
}

static bool IsModelPackageImporterName(const std::string& importer)
{
	return importer == "FbxPackageImporter" || importer == "GltfPackageImporter";
}

static bool IsCompiledVirtualGeometryMeshPath(const std::string& path)
{
	std::filesystem::path compiledPath(path);
	return compiledPath.extension() == ".temf" || compiledPath.extension() == ".TEMF";
}

static nlohmann::json BuildVirtualGeometryExportManifest(const BuildProfile& profile)
{
	nlohmann::json virtualGeometry;
	virtualGeometry["quality"] = QualityFromRank(profile.virtualGeometryQualityLevel);
	virtualGeometry["qualityLevel"] = profile.virtualGeometryQualityLevel;
	virtualGeometry["diagnosticsEnabled"] = profile.virtualGeometryDiagnosticsEnabled;
	virtualGeometry["geometryBudgetBytes"] = profile.virtualGeometryBudgetBytes;
	virtualGeometry["lodErrorScale"] = profile.virtualGeometryLodErrorScale;
	virtualGeometry["maxLodDrop"] = profile.virtualGeometryMaxLodDrop;
	virtualGeometry["hiZOcclusionEnabled"] = profile.virtualGeometryHiZOcclusionEnabled;
	virtualGeometry["preloadBias"] = profile.virtualGeometryPreloadBias;
	// Keep this declaration in lockstep with ExportPackageValidator so an
	// exported package advertises every TEMF metadata table required by the
	// virtual-geometry renderer before validation scans the packaged meshes.
	virtualGeometry["requiredMetadata"] = nlohmann::json::array({
		"adaptiveTargets",
		"fallbackCoverage",
		"clusterHierarchy",
		"pageLinks",
		"coneSafety"
	});
	// Export validation also records the fixture coverage assumptions that made
	// the current virtual-geometry package format safe enough to run. The
	// runtime does not load these fixtures; the fields are an audit trail for
	// developers and for package validation before the build is accepted.
	virtualGeometry["coverage"] = {
		{ "fixtureManifest", "Tests/Fixtures/VirtualGeometry/hardening-fixtures.json" },
		{ "requiredCategories", nlohmann::json::array({ "export", "streaming-pressure", "multi-view" }) }
	};
	virtualGeometry["requiredShaders"] = nlohmann::json::array({
		"Shaders/Default.task.tspf",
		"Shaders/Default.mesh.tspf",
		"Shaders/Default.frag.tspf",
		"Shaders/PostProcess/HiZGenerate.comp.spv"
	});
	return virtualGeometry;
}

static int ExecuteUtf8ShellCommand(const std::string& commandLine) {
#ifdef _WIN32
	const int wideLength = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, commandLine.data(), static_cast<int>(commandLine.size()), nullptr, 0);
	if(wideLength <= 0) return -1;
	std::wstring wideCommand(static_cast<std::size_t>(wideLength), L'\0');
	if(MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, commandLine.data(), static_cast<int>(commandLine.size()),
		wideCommand.data(), wideLength) != wideLength) return -1;
	return _wsystem(wideCommand.c_str());
#else
	return std::system(commandLine.c_str());
#endif
}

static nlohmann::json BuildRendererRecoveryManifest(const BuildProfile& profile)
{
	nlohmann::json recovery;
	recovery["validationEnabled"] = profile.enableVulkanValidationLayers;
	recovery["diagnosticsEnabled"] = profile.rendererRecoveryDiagnosticsEnabled;
	recovery["mode"] = "failClosed";
	recovery["fatalMessage"] = profile.rendererRecoveryFatalMessage.empty()
		? "Graphics device lost. Restart the application to restore rendering."
		: profile.rendererRecoveryFatalMessage;
	recovery["fullApplicationRestart"] = false;
	return recovery;
}

static nlohmann::json ResolveRenderProfileManifest(const BuildProfile& profile, const std::filesystem::path& projectRoot)
{
	nlohmann::json renderProfile;
	renderProfile["profileName"] = "Project Render Settings";
	renderProfile["shadows"] = "high";
	renderProfile["ambientOcclusion"] = "medium";
	renderProfile["variableRateShading"] = "disabled";
	renderProfile["textureBudgetMb"] = 512u;
	renderProfile["resolutionScale"] = 1.0f;
	renderProfile["validationEnabled"] = profile.enableVulkanValidationLayers;
	renderProfile["debugEnabled"] = profile.enableRenderDebugDiagnostics;

	const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "RenderSettings.json";
	if (!std::filesystem::exists(settingsPath)) {
		return renderProfile;
	}

	try {
		std::ifstream file(settingsPath);
		nlohmann::json savedSettings;
		file >> savedSettings;
		if (auto settingsIt = savedSettings.find("settings"); settingsIt != savedSettings.end() && settingsIt->is_object()) {
			// Build profiles no longer own renderer quality. The export manifest
			// records a concise product-facing summary of the authoritative
			// project render settings so boot logs still show what was exported.
			renderProfile["shadows"] = ShadowQualityFromSettings(*settingsIt);
			renderProfile["ambientOcclusion"] = AmbientOcclusionQualityFromSettings(*settingsIt);
			renderProfile["textureBudgetMb"] = settingsIt->value("textureBudgetMb", 512u);
			renderProfile["resolutionScale"] = settingsIt->value("resolutionScale", 1.0f);
			renderProfile["variableRateShading"] = settingsIt->value("variableRateShading", 0u) > 0u ? "enabled" : "disabled";
		}
	}
	catch (...) {
		// A malformed optional settings file should not make export crash here;
		// validation can report richer diagnostics while the manifest remains
		// deterministic with documented defaults.
	}

	return renderProfile;
}

static std::optional<nlohmann::json> LoadProjectRenderSettingsManifest(const std::filesystem::path& projectRoot)
{
	const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "RenderSettings.json";
	if (!std::filesystem::exists(settingsPath)) {
		return std::nullopt;
	}

	try {
		std::ifstream file(settingsPath);
		if (!file) {
			return std::nullopt;
		}

		nlohmann::json savedSettings;
		file >> savedSettings;
		if (!savedSettings.is_object()) {
			return std::nullopt;
		}

		// Preserve the editor-authored render settings document exactly instead
		// of reducing it to profile labels. Export runtime can then apply the
		// same renderer contract that the project used in the editor.
		return savedSettings;
	}
	catch (...) {
		// Render settings remain optional for old projects. A malformed file
		// should be reported by validation, but manifest generation still needs
		// a deterministic fallback path so export does not crash here.
		return std::nullopt;
	}
}

static nlohmann::json BuildResidencyPolicyManifest()
{
	const TerraEngine::Resource::ResidencyPolicy profile = ResourceManager::Instance().GetResidencyPolicy();
	nlohmann::json policy;
	policy["enabled"] = profile.enabled;
	policy["cleanupIntervalSeconds"] = profile.cleanupIntervalSeconds;
	policy["cpu"] = {
		{ "idleSecondsThreshold", profile.cpu.idleSecondsThreshold },
		{ "memoryBudgetBytes", profile.cpu.memoryBudgetBytes },
		{ "reloadGraceSeconds", profile.cpu.reloadGraceSeconds }
	};
	policy["gpu"] = {
		{ "idleSecondsThreshold", profile.gpu.idleSecondsThreshold },
		{ "memoryBudgetBytes", profile.gpu.memoryBudgetBytes },
		{ "reloadGraceSeconds", profile.gpu.reloadGraceSeconds },
		{ "strictBudgetMode", profile.gpu.strictBudgetMode }
	};
	return policy;
}

static nlohmann::json BuildStreamingPolicyManifest()
{
	const TerraEngine::Resource::StreamingPolicy profile = ResourceManager::Instance().GetStreamingPolicy();
	nlohmann::json policy;
	policy["enabled"] = profile.enabled;
	policy["deferScenePayloadsOnLoad"] = profile.deferScenePayloadsOnLoad;
	policy["streamingDistance"] = profile.streamingDistance;
	policy["visibleRetentionSeconds"] = profile.visibleRetentionSeconds;
	policy["neededRetentionSeconds"] = profile.neededRetentionSeconds;
	policy["maxLoadsPerUpdate"] = profile.maxLoadsPerUpdate;
	policy["textureMipStreamingEnabled"] = profile.textureMipStreamingEnabled;
	policy["textureMinStreamMip"] = profile.textureMinStreamMip;
	policy["textureMaxStreamMip"] = profile.textureMaxStreamMip;
	policy["virtualGeometryPageMemoryBudgetBytes"] = profile.virtualGeometryPageMemoryBudgetBytes;
	policy["virtualGeometryPageUploadBudgetBytes"] = profile.virtualGeometryPageUploadBudgetBytes;
	policy["virtualGeometryPageRetirementBudgetBytes"] = profile.virtualGeometryPageRetirementBudgetBytes;
	policy["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(profile.memoryBudgetInteraction));
	return policy;
}

// CopyRuntimeShaderTree stages every engine-owned shader into the TEPAK input.
// Some renderer features still reference shaders from nested editor/history
// folders, so the export package keeps the complete installed shader layout.
static bool CopyRuntimeShaderTree(const std::filesystem::path& shadersSrc, const std::filesystem::path& shadersDest, std::string& outError) {
	if (!std::filesystem::exists(shadersSrc)) {
		outError = "Runtime shader directory not found: " + PathToUtf8(shadersSrc);
		return false;
	}

	RemoveDirectoryIfExists(shadersDest);
	std::filesystem::create_directories(shadersDest);
	for (const auto& entry : std::filesystem::recursive_directory_iterator(shadersSrc)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const auto relative = std::filesystem::relative(entry.path(), shadersSrc);
		const auto dest = shadersDest / relative;
		std::filesystem::create_directories(dest.parent_path());
		std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
	}

	return true;
}

// CopyRuntimeAssetTree stages an engine-owned runtime asset folder into the
// TEPAK input root. Fonts use this path so exported UI text does not depend on
// loose sidecar files or system-installed fonts.
static bool CopyRuntimeAssetTree(const std::filesystem::path& sourceRoot, const std::filesystem::path& destinationRoot, const std::string& description, std::string& outError) {
	if (!std::filesystem::exists(sourceRoot)) {
		outError = description + " directory not found: " + PathToUtf8(sourceRoot);
		return false;
	}

	RemoveDirectoryIfExists(destinationRoot);
	std::filesystem::create_directories(destinationRoot);
	for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const auto relative = std::filesystem::relative(entry.path(), sourceRoot);
		const auto dest = destinationRoot / relative;
		std::filesystem::create_directories(dest.parent_path());
		std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
	}

	return true;
}

// RemoveDebugSymbolFiles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveDebugSymbolFiles(const std::filesystem::path& rootDirectory) {
	if (!std::filesystem::exists(rootDirectory)) {
		return;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::recursive_directory_iterator(rootDirectory)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		// Exports should not carry loose debug symbol payloads when the build
		// profile explicitly disables them. Remove every generated PDB in the
		// managed output so bridge/runtime symbols cannot leak into the package.
		if (entry.path().extension() == ".pdb") {
			RemoveIfExists(entry.path());
		}
	}
}

// struct ProjectManifestMetadata groups related state and operations so callers have one documented contract.
struct ProjectManifestMetadata {
	std::string name = "TerraEngine";
	std::string version = "1.0.0";
};

// LoadProjectManifestMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ProjectManifestMetadata LoadProjectManifestMetadata(const std::filesystem::path& projectRoot) {
	ProjectManifestMetadata metadata;
	if (!projectRoot.filename().empty()) {
		metadata.name = PathToUtf8(projectRoot.filename());
	}

	const std::filesystem::path manifestPath = projectRoot / "game.json";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(manifestPath)) {
		return metadata;
	}

	try {
		std::ifstream input(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!input) {
			return metadata;
		}

		const nlohmann::json manifest = nlohmann::json::parse(input);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string()) {
			const std::string manifestName = nameIt->get<std::string>();
			if (!manifestName.empty()) {
				metadata.name = manifestName;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto versionIt = manifest.find("version"); versionIt != manifest.end() && versionIt->is_string()) {
			const std::string manifestVersion = versionIt->get<std::string>();
			if (!manifestVersion.empty()) {
				metadata.version = manifestVersion;
			}
		}
	}
	catch (...) {
		// Export validation already reports richer context elsewhere. Falling back
		// here keeps manifest generation resilient even if metadata is malformed.
	}

	return metadata;
}

// --- Public API ---

ExportResult ExportPipeline::Execute(
	const ExportRequest& request,
	ExportProgressCallback progressCallback)
{
	const auto startTime = std::chrono::steady_clock::now();
	ExportResult result;
	result.runId = request.runId;
	result.invocationKind = request.invocationKind;
	result.profileId = request.profile.id;
	result.projectRoot = request.projectRoot;
	result.outputDirectory = request.finalOutputDirectory;
	result.evidenceDirectory = request.evidenceDirectory;
	m_StagedAssets.clear();

	std::vector<std::string> progressRecords;
	ExportProgressCallback capturedProgress = [&](const ExportProgress& progress) {
		progressRecords.push_back(SerializeExportProgressRecord(request.runId, progress));
		if(progressCallback) progressCallback(progress);
	};
	std::string resolvedManifest;
	std::vector<ExportContentInventoryEntry> inventory;
	std::string packageValidationReport;
	ExportOutputTransaction transaction(request.finalOutputDirectory, request.runId);

	auto writeEvidence = [&](std::string& outError) {
		ExportEvidenceInputs evidenceInputs;
		evidenceInputs.resolvedManifestJson = resolvedManifest;
		evidenceInputs.inventory = inventory;
		evidenceInputs.progressJsonLines = progressRecords;
		evidenceInputs.packageValidationReport = packageValidationReport;
		return WriteExportEvidenceBundle(request, result, evidenceInputs, outError);
	};

	auto finish = [&](ExportFailureCategory category, ExportStage stage, std::string message) {
		result.success = category == ExportFailureCategory::None;
		result.failureCategory = category;
		result.stage = stage;
		result.exitCode = ExitCodeForFailure(category);
		result.message = std::move(message);
		result.errorMessage = result.success ? std::string{} : result.message;
		std::vector<ExportDiagnostic> stageDiagnostics = std::move(result.diagnostics);
		result.diagnostics = result.validationResult.diagnostics;
		result.diagnostics.insert(result.diagnostics.end(),
			std::make_move_iterator(stageDiagnostics.begin()),
			std::make_move_iterator(stageDiagnostics.end()));
		if(category != ExportFailureCategory::None) {
			ExportDiagnostic diagnostic;
			diagnostic.severity = ExportDiagnosticSeverity::Error;
			diagnostic.category = std::string(ToString(category));
			diagnostic.stage = std::string(ToString(stage));
			diagnostic.code = "export." + std::string(ToString(category));
			diagnostic.message = result.message;
			diagnostic.filePath = PathToUtf8(result.affectedPath);
			result.diagnostics.push_back(std::move(diagnostic));
		}
		result.totalElapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();

		if(!result.success) {
			std::string rollbackError;
			if(!transaction.Rollback(rollbackError)) {
				result.failureCategory = ExportFailureCategory::PublicationFailed;
				result.stage = ExportStage::Publication;
				result.exitCode = ExitCodeForFailure(result.failureCategory);
				result.message += " Rollback failed: " + rollbackError;
				result.errorMessage = result.message;
			}
		}

		std::string evidenceError;
		if(!writeEvidence(evidenceError)) {
			result.success = false;
			result.failureCategory = ExportFailureCategory::EvidenceWriteFailed;
			result.stage = ExportStage::Evidence;
			result.exitCode = ExitCodeForFailure(result.failureCategory);
			result.message = evidenceError;
			result.errorMessage = evidenceError;
			// A second best-effort write updates export-result.json when the first
			// attempt failed after creating only part of the evidence directory.
			std::string ignoredError;
			writeEvidence(ignoredError);
		}
		return result;
	};

	try {
		std::string transactionError;
		if(!transaction.Begin(transactionError)) {
			result.affectedPath = transaction.LockPath();
			return finish(ExportFailureCategory::OutputConflict, ExportStage::Lock, transactionError);
		}

		std::string runtimeError;
		if(!ValidateExportRuntimeLayout(request.runtimeLayout, request.profile.platform, runtimeError)) {
			result.affectedPath = request.runtimeLayout.installRoot;
			return finish(ExportFailureCategory::InvalidRuntimeLayout, ExportStage::Arguments, runtimeError);
		}

		BuildProfile executionProfile = request.profile;
		executionProfile.outputDirectory = transaction.StagingDirectory();
		const std::filesystem::path packageInputDirectory = transaction.ScratchDirectory() / "PackageInput";

		result.stage = ExportStage::Validation;
		if(!RunValidation(executionProfile, request.projectRoot, request.runtimeLayout, result, capturedProgress)) {
			result.affectedPath = request.profilePath.empty() ? request.projectRoot : request.profilePath;
			return finish(ExportFailureCategory::ValidationFailed, result.stage, result.errorMessage);
		}

		if(executionProfile.enableManagedCompilation) {
			result.stage = ExportStage::ManagedCompilation;
			if(!RunManagedCompilation(executionProfile, request.projectRoot, result, capturedProgress)) {
				result.affectedPath = request.projectRoot / "Assembly-CSharp.csproj";
				return finish(ExportFailureCategory::ManagedCompilationFailed, result.stage, result.errorMessage);
			}
		}

		result.stage = ExportStage::AssetStaging;
		if(!RunAssetStaging(executionProfile, request.projectRoot, request.runtimeLayout,
			packageInputDirectory, result, capturedProgress)) {
			result.affectedPath = packageInputDirectory;
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage, result.errorMessage);
		}

		const std::filesystem::path manifestPath = packageInputDirectory / "game.json";
		{
			// Close the manifest handle before Commit removes the scratch tree on
			// Windows; an open stream would otherwise leave an orphan directory.
			std::ifstream manifestInput(manifestPath, std::ios::binary);
			resolvedManifest.assign(
				std::istreambuf_iterator<char>(manifestInput), std::istreambuf_iterator<char>());
		}
		std::string inventoryError;
		if(!BuildExportContentInventory(packageInputDirectory, inventory, inventoryError)) {
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage, inventoryError);
		}
		const auto placeholderEntry = std::find_if(inventory.begin(), inventory.end(), [](const auto& entry) {
			return entry.required && entry.placeholderDetected;
		});
		if(placeholderEntry != inventory.end()) {
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage,
				"Required staged entry contains a placeholder marker: " + placeholderEntry->logicalPath);
		}

		result.stage = ExportStage::Packaging;
		if(!RunAssetPackaging(executionProfile, request.projectRoot, packageInputDirectory, result, capturedProgress)) {
			result.affectedPath = transaction.StagingDirectory();
			return finish(ExportFailureCategory::PackagingFailed, result.stage, result.errorMessage);
		}

		result.stage = ExportStage::PackageValidation;
		ExportPackageValidationReport inspection = InspectExportPackage(
			result.packageResult.outputPath, executionProfile.platform);
		const bool manifestMatchesEvidence = inspection.archivedManifestJson == resolvedManifest;
		inspection.passed = inspection.passed && manifestMatchesEvidence;
		const std::size_t finalLine = inspection.summaryText.rfind("Final: ");
		if(finalLine != std::string::npos) inspection.summaryText.erase(finalLine);
		inspection.summaryText += std::string("Manifest evidence match: ")
			+ (manifestMatchesEvidence ? "PASS\n" : "FAIL\n")
			+ "Final: " + (inspection.passed ? "PASS\n" : "FAIL\n");
		packageValidationReport = inspection.summaryText;
		if(!inspection.passed || !manifestMatchesEvidence) {
			result.affectedPath = result.packageResult.outputPath;
			for(const ExportPackageValidationFinding& finding : inspection.findings) {
				ExportDiagnostic diagnostic;
				diagnostic.severity = ExportDiagnosticSeverity::Error;
				diagnostic.category = "Package";
				diagnostic.stage = "package_validation";
				diagnostic.code = finding.code;
				diagnostic.message = finding.message;
				diagnostic.filePath = finding.path;
				result.diagnostics.push_back(std::move(diagnostic));
			}
			const std::string error = inspection.findings.empty()
				? "Export package manifest does not match resolved-manifest evidence."
				: inspection.findings.front().message;
			return finish(ExportFailureCategory::PackageValidationFailed, result.stage, error);
		}

		result.stage = ExportStage::RuntimeStaging;
		if(!RunOutputStaging(executionProfile, request.projectRoot, request.runtimeLayout, result, capturedProgress)) {
			result.affectedPath = transaction.StagingDirectory();
			return finish(ExportFailureCategory::RuntimeStagingFailed, result.stage, result.errorMessage);
		}

		std::error_code scratchCleanupError;
		std::filesystem::remove_all(transaction.ScratchDirectory(), scratchCleanupError);
		if(scratchCleanupError) {
			result.affectedPath = transaction.ScratchDirectory();
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication,
				"Could not clean validated export scratch data: " + scratchCleanupError.message());
		}
		if(!transaction.MarkStagedAndValidated(transactionError)) {
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication, transactionError);
		}
		const std::filesystem::path stagedArchivePath = result.packageResult.outputPath;
		const std::filesystem::path archiveFileName = stagedArchivePath.filename();
		result.packageResult.outputPath = request.finalOutputDirectory / archiveFileName;
		const std::string stagedArchiveText = PathToUtf8(stagedArchivePath);
		const std::size_t archiveSummaryOffset = packageValidationReport.find(stagedArchiveText);
		if(archiveSummaryOffset != std::string::npos) {
			packageValidationReport.replace(
				archiveSummaryOffset, stagedArchiveText.size(), PathToUtf8(result.packageResult.outputPath));
		}
		result.outputDirectory = request.finalOutputDirectory;
		result.success = true;
		result.failureCategory = ExportFailureCategory::None;
		result.stage = ExportStage::Complete;
		result.exitCode = 0;
		result.message = "Export completed successfully.";
		result.errorMessage.clear();
		result.diagnostics = result.validationResult.diagnostics;
		result.totalElapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();
		ExportProgress completeProgress;
		completeProgress.phase = ExportPhase::Complete;
		completeProgress.stage = ExportStage::Complete;
		completeProgress.overallProgress = 1.0f;
		completeProgress.statusMessage = result.message;
		completeProgress.finished = true;
		// Store the eventual stdout record before evidence publication, while the
		// external callback remains deferred until the output swap succeeds.
		progressRecords.push_back(SerializeExportProgressRecord(request.runId, completeProgress));
		std::string evidenceError;
		if(!writeEvidence(evidenceError)) {
			result.affectedPath = request.evidenceDirectory;
			return finish(ExportFailureCategory::EvidenceWriteFailed, ExportStage::Evidence, evidenceError);
		}
		if(!transaction.Commit(transactionError)) {
			result.affectedPath = request.finalOutputDirectory;
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication, transactionError);
		}
		// The evidence bundle is already durable. Publish the completion callback
		// only after the validated directory swap succeeds, without reopening the
		// evidence files after the protected boundary.
		if(progressCallback) {
			try {
				progressCallback(completeProgress);
			} catch(...) {
				// Publication has completed and must never be reclassified as a
				// failure because a presentation-only callback threw an exception.
			}
		}
		return result;
	} catch(const std::exception& exception) {
		return finish(ExportFailureCategory::UnexpectedFailure, result.stage,
			std::string("Contained export failure: ") + exception.what());
	} catch(...) {
		return finish(ExportFailureCategory::UnexpectedFailure, result.stage,
			"Contained export failure with no diagnostic text.");
	}
}

static bool ValidateManagedRuntimeOutput(
	const std::filesystem::path& managedLibrary,
	std::string& outError)
{
	std::error_code errorCode;
	if(!std::filesystem::is_regular_file(managedLibrary, errorCode)) {
		outError = "NativeAOT publish did not produce the required gameplay library: " + PathToUtf8(managedLibrary);
		return false;
	}
	const std::uintmax_t byteSize = std::filesystem::file_size(managedLibrary, errorCode);
	if(errorCode || byteSize == 0) {
		outError = "NativeAOT gameplay library is empty or unreadable: " + PathToUtf8(managedLibrary);
		return false;
	}

	std::ifstream input(managedLibrary, std::ios::binary);
	const std::vector<unsigned char> bytes{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	static constexpr std::string_view marker = "placeholder";
	const bool containsPlaceholder = std::search(
		bytes.begin(), bytes.end(), marker.begin(), marker.end(),
		[](unsigned char left, char right) {
			return static_cast<char>(std::tolower(left)) == right;
		}) != bytes.end();
	if(containsPlaceholder) {
		outError = "NativeAOT gameplay library contains the forbidden placeholder marker: " + PathToUtf8(managedLibrary);
		return false;
	}
	return true;
}

// --- Phase implementations ---

bool ExportPipeline::RunValidation(const BuildProfile& profile, const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout, ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Validating, 0.0f,
		"Validating build profile...");

	result.validationResult = m_Validator.ValidateAll(profile, projectRoot, runtimeLayout.shaderDirectory);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.validationResult.passed) {
		result.errorMessage = "Pre-export validation failed with " +
			std::to_string(result.validationResult.errorCount) + " error(s).";
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Validating, 0.1f,
		"Validation passed.");
	return true;
}

bool ExportPipeline::RunManagedCompilation(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Compiling, 0.15f,
		"Compiling managed project with NativeAOT...");

	// The managed project is strictly required to be at the project root for export.
	std::filesystem::path managedProject = projectRoot / "Assembly-CSharp.csproj";

	if (!std::filesystem::exists(managedProject)) {
		result.errorMessage = "Managed project not found: " + PathToUtf8(managedProject);
		return false;
	}

	if (!IsExportPlatformNativeToHost(profile.platform)) {
		result.errorMessage = "Cross-OS NativeAOT export is not supported by dotnet. This host can publish " +
			GetExportRuntimeIdentifier(GetHostExportPlatform()) + "; the selected profile targets " +
			GetExportRuntimeIdentifier(profile.platform) + ".";
		return false;
	}

	// Resolve the output directory for the NativeAOT publish output.
	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	std::filesystem::create_directories(outputDir);
	const std::filesystem::path managedOutputDir = outputDir / "Managed";

	// NativeAOT publish does not clean removed files from a previous export.
	// Clear the export Managed directory first so stale bridge/runtime payloads
	// such as TerraEngine.CSharp.dll cannot survive after the export pipeline
	// stops copying them.
	RemoveDirectoryIfExists(managedOutputDir);

	// Wipe the intermediate object directory to prevent stale NativeAOT caching
	// across exports, ensuring we compile the latest bootstrap sources.
	// The .csproj places obj files in Build/Assembly-CSharp/obj by default.
	std::filesystem::path objDir = projectRoot / "Build" / "Assembly-CSharp" / "obj";
	if (std::filesystem::exists(objDir)) {
		try { std::filesystem::remove_all(objDir); } catch (...) {}
	}
	
	// Also wipe any old obj folder just in case.
	std::filesystem::path oldObjDir = outputDir / "obj";
	if (std::filesystem::exists(oldObjDir)) {
		try { std::filesystem::remove_all(oldObjDir); } catch (...) {}
	}

	// Old projects may still contain interop sources that conflict with the source
	// generator. Export reports the problem but never deletes project source; the
	// user remains in control of their checked-in content.
	std::filesystem::path obsoleteExportDir = projectRoot / "Project" / "Internal" / "Export";
	if (std::filesystem::exists(obsoleteExportDir)) {
		result.errorMessage = "Obsolete 'Project/Internal/Export' source exists. Remove it manually before exporting so the source generator has one interop definition.";
		return false;
	}

	// Build the dotnet publish command in the shared build-profile helper so
	// profile toggles are tested without launching a real NativeAOT publish.
	// The CLI reserves stdout for one-JSON-object-per-line automation records.
	// Both cmd.exe and POSIX shells accept this redirection, so verbose MSBuild
	// output remains human-readable on stderr without corrupting JSONL output.
	const std::string commandLine = BuildNativeAotPublishCommand(profile, managedProject, managedOutputDir) + " 1>&2";

	// Execute the dotnet publish command.
	const int exitCode = ExecuteUtf8ShellCommand(commandLine);
	if (exitCode != 0) {
		result.errorMessage = "NativeAOT compilation failed (exit code " +
			std::to_string(exitCode) + ").";
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!profile.includeDebugSymbols) {
		RemoveDebugSymbolFiles(managedOutputDir);
	}
	std::string managedOutputError;
	if(!ValidateManagedRuntimeOutput(
		managedOutputDir / GetExportManagedRuntimeLibraryName(profile.platform), managedOutputError)) {
		result.errorMessage = managedOutputError;
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Compiling, 0.45f,
		"Managed compilation completed.");
	return true;
}

bool ExportPipeline::RunAssetStaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout,
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::AssetStaging, 0.45f,
		"Staging assets for packaging...");

	try {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(stagingDir)) {
			std::filesystem::remove_all(stagingDir);
		}
		std::filesystem::create_directories(stagingDir);

		const auto rawAssets = projectRoot / "Assets";
		const auto compiledAssets = projectRoot / "Build" / "Assets";

		// 1. Copy engine bootstrap sources for NativeAOT.
		// These must be included in the user's project so that the InitManagedRuntime
		// entry point is exported from the gameplay DLL.
		std::filesystem::path engineManagedSrc = projectRoot / ".." / "Engine" / "Managed" / "Source" / "Export";
		// If we are in a sub-repo or different structure, we might need a better way to find this.
		// For now, assume standard dev layout. 
		// Actually, we can just copy them from the project's source if they were added there,
		// but they aren't by default. We'll copy them to the staging area so they are compiled.
		// Wait, NativeAOT compiles the CSProj. The CSProj needs to reference these files.
		// If the user's project doesn't have them, NativeAOT won't export the symbols.
		// The correct fix is ensuring the user's CSProj includes them or they are copied into the project.
		
		// 2. Copy raw assets (baseline).
		if (std::filesystem::exists(rawAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(rawAssets)) {
				if (!entry.is_regular_file()) continue;
				
				// Exclude .meta files from the exported build.
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() == ".meta") continue;

				auto relative = std::filesystem::relative(entry.path(), rawAssets);
				auto dest = stagingDir / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
		}

		// 2. Overlay compiled assets (priority).
		// This replaces raw source (like .png) with engine-ready counterparts (like .tetf)
		// while leaving raw files that don't need compilation (like .tescene) intact.
		if (std::filesystem::exists(compiledAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(compiledAssets)) {
				if (!entry.is_regular_file()) continue;

				auto relative = std::filesystem::relative(entry.path(), compiledAssets);
				auto dest = stagingDir / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
		}

		// Engine-owned runtime payloads are packed with project content so the
		// standalone launcher can resolve them entirely through ContentMount.
		std::string runtimeAssetError;
		if (!CopyRuntimeShaderTree(runtimeLayout.shaderDirectory, stagingDir / "Shaders", runtimeAssetError)) {
			result.errorMessage = runtimeAssetError;
			return false;
		}
		if (!CopyRuntimeAssetTree(runtimeLayout.fontDirectory, stagingDir / "Fonts", "Runtime font", runtimeAssetError)) {
			result.errorMessage = runtimeAssetError;
			return false;
		}

		// Rewrite only the staged scene and prefab copies so export runtime
		// assets resolve script bindings through deterministic numeric IDs.
		std::string rewriteError;
		if (!RewriteStagedScriptBindings(stagingDir, rewriteError)) {
			result.errorMessage = rewriteError;
			return false;
		}

		// 3. Scan metadata to build the GUID map for the export manifest.
		// In an exported build, the engine uses this map to resolve the UUIDs found
		// in scene/prefab files back to the logical paths inside the TEPAK archive.
		std::unordered_set<std::string> stagedAssetGuids;
		auto addStagedAsset = [this, &stagedAssetGuids](std::string guid, AssetType type, const std::filesystem::path& path) -> bool {
			if (guid.empty() || path.empty()) {
				return false;
			}
			if (!stagedAssetGuids.insert(guid).second) {
				return false;
			}

			StagedAsset asset;
			asset.guid = std::move(guid);
			asset.path = path.generic_string();
			asset.type = type;
			asset.virtualGeometryRequired = asset.type == AssetType::Mesh
				&& IsCompiledVirtualGeometryMeshPath(asset.path);
			m_StagedAssets.push_back(std::move(asset));
			return true;
		};
		auto resolveSourceAssetPath = [&rawAssets](const std::filesystem::path& metaPath, const std::string& sourcePathText) {
			if (!sourcePathText.empty()) {
				const std::filesystem::path sourcePath(sourcePathText);
				return sourcePath.is_absolute() ? sourcePath : rawAssets / sourcePath;
			}

			std::filesystem::path inferredSourcePath = metaPath;
			inferredSourcePath.replace_extension();
			return inferredSourcePath;
		};
		auto stageModelPackageChildren = [&addStagedAsset](const std::filesystem::path& sourceAssetPath) -> bool {
			TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
			if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, importResult)
				|| importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed) {
				return false;
			}

			bool addedAny = false;
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes) {
				// Package parents are containers; only generated child TEMFs are
				// renderable at runtime. Skip stale failed child cooks so the
				// manifest never references missing or unusable package output.
				if (mesh.compiledPath.empty()
					|| mesh.virtualGeometryStatus == "Failed"
					|| mesh.virtualGeometryStatus == "NeedsRebuild") {
					continue;
				}
				addedAny = addStagedAsset(TerraEngine::ToHexString(mesh.guid), AssetType::Mesh, mesh.compiledPath) || addedAny;
			}
			for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials) {
				if (!material.compiledPath.empty()) {
					addedAny = addStagedAsset(TerraEngine::ToHexString(material.guid), AssetType::Material, material.compiledPath) || addedAny;
				}
			}
			for (const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures) {
				if (!texture.compiledPath.empty()) {
					addedAny = addStagedAsset(TerraEngine::ToHexString(texture.guid), AssetType::Texture, texture.compiledPath) || addedAny;
				}
			}
			return addedAny;
		};
		if (std::filesystem::exists(rawAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(rawAssets)) {
				if (!entry.is_regular_file()) continue;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() != ".meta") continue;

				try {
					std::ifstream f(entry.path());
					nlohmann::json meta = nlohmann::json::parse(f);

					std::string guid = meta.value("guid", "");
					std::string importer = meta.value("importerName", "");
					std::string compiledPath = meta.value("compiledPath", "");

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (guid.empty()) continue;

					AssetType type;
					bool supported = true;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (importer == "MeshImporter") type = AssetType::Mesh;
					else if (IsModelPackageImporterName(importer)) {
						const std::filesystem::path sourceAssetPath = resolveSourceAssetPath(entry.path(), meta.value("sourcePath", std::string{}));
						if (stageModelPackageChildren(sourceAssetPath)) {
							continue;
						}

						// Older package imports may still have a parent compiled
						// mesh. Use it only when the actual compiled payload exists;
						// otherwise skip the container so package validation does not
						// fail on a manifest entry like AntiqueCamera.temf.
						if (!compiledPath.empty() && std::filesystem::exists(compiledAssets / compiledPath)) {
							type = AssetType::Mesh;
						}
						else {
							continue;
						}
					}
					else if (importer == "TextureImporter") type = AssetType::Texture;
					else if (importer == "MaterialImporter") type = AssetType::Material;
					else supported = false;

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (supported) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						std::filesystem::path manifestPath;
						if (!compiledPath.empty()) {
							// The compiled path is relative to the Build/Assets/ directory,
							// which matches our stagingDir layout.
							manifestPath = compiledPath;
						}
						else {
							// If the asset isn't compiled (like a .tescene), use its 
							// relative path within the Assets/ folder.
							auto rel = std::filesystem::relative(entry.path().parent_path() / meta.value("sourcePath", ""), rawAssets);
							manifestPath = rel;
						}
						addStagedAsset(std::move(guid), type, manifestPath);
					}
				}
				catch (...) {
					// Skip malformed or unreadable meta files.
				}
			}
		}

		// 4. Stage the export manifest directly into the TEPAK input root so the
		// launcher can read game.json from inside the archive that it already
		// mounted, instead of depending on a second file beside the executable.
		if (!WriteExportManifest(profile, projectRoot, stagingDir)) {
			result.errorMessage = "Failed to stage game.json into the export archive input.";
			return false;
		}

		return true;
	}
	catch (const std::exception& e) {
		result.errorMessage = std::string("Asset staging failed: ") + e.what();
		return false;
	}
}

bool ExportPipeline::RunAssetPackaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Packaging, 0.50f,
		"Packaging assets into TEPAK archive...");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(stagingDir)) {
		result.errorMessage = "Asset staging directory not found: " + PathToUtf8(stagingDir);
		return false;
	}

	// Resolve the output path for the TEPAK archive.
	const std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const auto tepakPath = outputDir / std::filesystem::u8path(ResolveArchiveFileName(projectName));

	// Package progress is forwarded to the pipeline progress callback.
	PackageProgressCallback packageCallback;
	if (progressCallback) {
		packageCallback = [&progressCallback](const PackageProgress& p) {
			float fraction = 0.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (p.totalEntries > 0) {
				fraction = static_cast<float>(p.completedEntries) /
					static_cast<float>(p.totalEntries);
			}
			// Map packaging progress to the 0.50–0.80 range of overall progress.
			float overall = 0.50f + fraction * 0.30f;
			EmitProgress(progressCallback, ExportPhase::Packaging, overall,
				"Packaging: " + p.currentEntry);
		};
	}

	result.packageResult = m_Packager.Package(stagingDir, tepakPath, profile, packageCallback);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.packageResult.success) {
		result.errorMessage = result.packageResult.errorMessage;
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Packaging, 0.80f,
		"Asset packaging completed (" +
		std::to_string(result.packageResult.entryCount) + " entries).");
	return true;
}

bool ExportPipeline::RunOutputStaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Staging, 0.85f,
		"Staging output directory...");

	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const std::string launcherFileName = GetExportLauncherFileName(projectName, profile.platform);
	const std::string legacyLauncherFileName = GetExportLauncherFileName(profile.name, profile.platform);
	const std::string archiveFileName = ResolveArchiveFileName(projectName);
	std::filesystem::create_directories(outputDir);

	result.outputDirectory = outputDir;

	// Remove the legacy sidecar manifest and the old fixed-name archive so the
	// export folder reflects the current convention after repeated exports.
	RemoveIfExists(outputDir / "game.json");
	if (archiveFileName != "content.tepak") {
		RemoveIfExists(outputDir / "content.tepak");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (legacyLauncherFileName != launcherFileName) {
		RemoveIfExists(outputDir / std::filesystem::u8path(legacyLauncherFileName));
	}

	// Copy runtime binaries from the explicitly validated installed layout. This
	// keeps export independent of the caller's current working directory.
	try {
		const std::filesystem::path launcherSourceName = GetExportLauncherFileName("TerraEngine-Launcher", profile.platform);
		std::filesystem::path launcherSrc = runtimeLayout.launcherDirectory / launcherSourceName;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(launcherSrc)) {
			std::filesystem::copy_file(launcherSrc, outputDir / std::filesystem::u8path(launcherFileName), std::filesystem::copy_options::overwrite_existing);
		} else {
			result.errorMessage = "Required launcher binary not found: " + PathToUtf8(launcherSrc);
			return false;
		}

		// Copy required native engine libraries beside the launcher. The names
		// follow each platform's native convention while preserving the Windows
		// DLL contract already used by exported builds.
		std::vector<std::string> engineLibraries = {
			GetExportEngineLibraryName(profile.platform),
			GetExportGlfwLibraryName(profile.platform)
		};
		for (const auto& libraryName : engineLibraries) {
			std::filesystem::path librarySrc = runtimeLayout.launcherDirectory / libraryName;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(librarySrc)) {
				librarySrc = runtimeLayout.engineDirectory / libraryName;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(librarySrc)) {
				try {
					std::filesystem::copy_file(librarySrc, outputDir / libraryName, std::filesystem::copy_options::overwrite_existing);
				}
				catch (const std::exception& e) {
					std::cerr << "[Export] Failed to copy " << libraryName << ": " << e.what() << std::endl;
					result.errorMessage = "Failed to copy engine runtime library: " + libraryName;
					return false;
				}
			} else {
				result.errorMessage = "Required engine runtime library not found: " + PathToUtf8(librarySrc);
				return false;
			}
		}

		// Managed export payload now comes exclusively from the NativeAOT publish
		// output staged earlier in RunManagedCompilation(). TerraEngine.CSharp and
		// the editor's CoreCLR runtime files are no longer part of exported builds.

		// Shaders and fonts are packaged into TEPAK during asset staging. Remove
		// stale loose copies from older exports so the output folder reflects the
		// mounted-content runtime contract.
		RemoveDirectoryIfExists(outputDir / "Shaders");
		RemoveDirectoryIfExists(outputDir / "Fonts");
	}
	catch (const std::exception& e) {
		result.errorMessage = std::string("Failed to copy engine binaries: ") + e.what();
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Staging, 0.95f,
		"Output staging completed.");
	return true;
}

bool ExportPipeline::WriteExportManifest(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& targetDirectory)
{
	try {
		// The launcher now reads game.json from inside the mounted TEPAK archive.
		// Keep only the data that can differ per export in this file. Fixed
		// runtime paths, such as Managed/Assembly-CSharp.dll, are now derived by
		// convention so the manifest stays small and harder to desynchronize.
		nlohmann::json manifest;
		const ProjectManifestMetadata projectMetadata = LoadProjectManifestMetadata(projectRoot);
		manifest["name"] = projectMetadata.name;
		manifest["version"] = projectMetadata.version;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!profile.startupScene.empty()) {
			manifest["startupScene"] = profile.startupScene;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!profile.includedScenes.empty()) {
			manifest["scenes"] = profile.includedScenes;
		}

		manifest["renderProfile"] = ResolveRenderProfileManifest(profile, projectRoot);
		if (std::optional<nlohmann::json> renderSettings = LoadProjectRenderSettingsManifest(projectRoot)) {
			// Export renderer diagnostics are build-profile decisions, not the
			// editor's local Preferences state. Mirror the profile values into the
			// exact render settings payload so every packaged metadata path agrees.
			(*renderSettings)["validationEnabled"] = profile.enableVulkanValidationLayers;
			(*renderSettings)["debugEnabled"] = profile.enableRenderDebugDiagnostics;
			manifest["renderSettings"] = *renderSettings;
		}
		manifest["residencyPolicy"] = BuildResidencyPolicyManifest();
		manifest["streamingPolicy"] = BuildStreamingPolicyManifest();
		manifest["rendererRecovery"] = BuildRendererRecoveryManifest(profile);
		manifest["virtualGeometry"] = BuildVirtualGeometryExportManifest(profile);

		// Include the asset GUID map so the engine runtime can resolve 
		// cross-resource references (UUIDs) back to filenames in the archive.
		manifest["assets"] = nlohmann::json::array();
		for (const auto& asset : m_StagedAssets) {
			nlohmann::json entry;
			entry["guid"] = asset.guid;
			entry["path"] = asset.path;
			entry["type"] = static_cast<uint32_t>(asset.type);
			if (asset.virtualGeometryRequired)
			{
				entry["virtualGeometry"] = {
					{ "required", true },
					{ "compiledPayload", asset.path },
					{ "diagnosticsEnabled", profile.virtualGeometryDiagnosticsEnabled },
					{ "requiresAdaptiveCookData", true },
					{ "requiresClusterFallback", true },
					{ "requiresPageData", true }
				};
			}
			manifest["assets"].push_back(std::move(entry));
		}

		const auto manifestPath = targetDirectory / "game.json";
		std::ofstream out(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!out) {
			return false;
		}
		out << manifest.dump(4);
		return out.good();
	}
	catch (...) {
		return false;
	}
}
