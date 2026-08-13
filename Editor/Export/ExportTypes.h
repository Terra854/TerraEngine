// ExportTypes.h - stable request, progress, and result contracts for export.
//
// The editor and headless executable intentionally share these types so an
// entry point cannot silently choose different validation or packaging rules.

#pragma once

#include "BuildProfile.h"
#include "ExportRuntimeLayout.h"
#include "ExportValidator.h"
#include "TepakPackager.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class ExportInvocationKind : std::uint32_t {
	Editor = 0,
	HeadlessCli = 1,
	AutomatedBaseline = 2,
};

enum class ExportStage : std::uint32_t {
	Arguments = 0,
	Lock,
	Validation,
	ManagedCompilation,
	AssetStaging,
	Packaging,
	PackageValidation,
	RuntimeStaging,
	Evidence,
	Publication,
	Complete,
};

enum class ExportFailureCategory : std::uint32_t {
	None = 0,
	InvalidArguments,
	InvalidRuntimeLayout,
	OutputConflict,
	ValidationFailed,
	ManagedCompilationFailed,
	AssetStagingFailed,
	PackagingFailed,
	PackageValidationFailed,
	RuntimeStagingFailed,
	EvidenceWriteFailed,
	PublicationFailed,
	UnexpectedFailure,
};

/// Phase retained for the existing editor progress widgets. Every phase maps to
/// a stable ExportStage before it is serialized for automation.
enum class ExportPhase : std::uint32_t {
	Idle = 0,
	Validating,
	Compiling,
	AssetStaging,
	Packaging,
	Staging,
	Complete,
	Failed,
};

struct ExportProgress {
	ExportPhase phase = ExportPhase::Idle;
	ExportStage stage = ExportStage::Arguments;
	float overallProgress = 0.0f;
	std::string statusMessage;
	bool finished = false;
};

// The physics subsection is versioned independently because it is consumed by
// the runtime before a scene can create a physics world.  These records contain
// only durable package identities; provider handles and editor-only recipes
// deliberately stay outside the export format.
struct PhysicsExportAssetRecord {
	std::string guid;
	std::string packagePath;
	std::string checksum;
	std::uint64_t byteSize = 0U;
	std::string sourceProvenanceHash;
	std::string recipeHash;
};

struct PhysicsExportManifest {
	static constexpr std::uint32_t kSchemaVersion = 1U;

	std::uint32_t schemaVersion = kSchemaVersion;
	std::string providerIdentifier = "Jolt";
	std::string providerVersion = "5.6.1";
	std::string providerRevision = "2e28006e4a367624bb512bf2c37ca99a93d9d1ea";
	std::uint32_t cookFormatVersion = 1U;
	std::string settingsPackagePath;
	std::uint32_t settingsSchemaVersion = 1U;
	std::string settingsHash;
	std::string layersHash;
	std::string defaultMaterialGuid;
	std::vector<PhysicsExportAssetRecord> materials;
	std::vector<PhysicsExportAssetRecord> colliders;
	std::uint32_t validationFixtureSchemaVersion = 1U;
	std::string validationStatus = "validated";
};

using ExportProgressCallback = std::function<void(const ExportProgress&)>;

struct ExportRequest {
	std::filesystem::path projectRoot;
	BuildProfile profile;
	std::filesystem::path profilePath;
	std::filesystem::path finalOutputDirectory;
	std::filesystem::path evidenceDirectory;
	ExportRuntimeLayout runtimeLayout;
	ExportInvocationKind invocationKind = ExportInvocationKind::Editor;
	std::string runId;
};

struct ExportResult {
	static constexpr std::uint32_t kSchemaVersion = 1;

	std::uint32_t schemaVersion = kSchemaVersion;
	std::string runId;
	bool success = false;
	ExportInvocationKind invocationKind = ExportInvocationKind::Editor;
	ExportStage stage = ExportStage::Arguments;
	ExportFailureCategory failureCategory = ExportFailureCategory::None;
	int exitCode = 0;
	std::string message;
	std::filesystem::path affectedPath;
	std::string profileId;
	std::filesystem::path projectRoot;
	std::filesystem::path outputDirectory;
	std::filesystem::path evidenceDirectory;
	ExportValidationResult validationResult;
	PackageResult packageResult;
	double totalElapsedSeconds = 0.0;
	std::string errorMessage;
	std::vector<ExportDiagnostic> diagnostics;
};

std::string_view ToString(ExportInvocationKind kind);
std::string_view ToString(ExportStage stage);
std::string_view ToString(ExportFailureCategory category);
int ExitCodeForFailure(ExportFailureCategory category);
ExportStage StageForPhase(ExportPhase phase);
