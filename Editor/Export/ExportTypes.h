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
