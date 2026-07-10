// ExportTypes.cpp - stable text and process-exit mappings.

#include "ExportTypes.h"

std::string_view ToString(ExportInvocationKind kind) {
	switch(kind) {
	case ExportInvocationKind::Editor: return "editor";
	case ExportInvocationKind::HeadlessCli: return "headless_cli";
	case ExportInvocationKind::AutomatedBaseline: return "automated_baseline";
	}
	return "editor";
}

std::string_view ToString(ExportStage stage) {
	switch(stage) {
	case ExportStage::Arguments: return "arguments";
	case ExportStage::Lock: return "lock";
	case ExportStage::Validation: return "validation";
	case ExportStage::ManagedCompilation: return "managed_compilation";
	case ExportStage::AssetStaging: return "asset_staging";
	case ExportStage::Packaging: return "packaging";
	case ExportStage::PackageValidation: return "package_validation";
	case ExportStage::RuntimeStaging: return "runtime_staging";
	case ExportStage::Evidence: return "evidence";
	case ExportStage::Publication: return "publication";
	case ExportStage::Complete: return "complete";
	}
	return "arguments";
}

std::string_view ToString(ExportFailureCategory category) {
	switch(category) {
	case ExportFailureCategory::None: return "none";
	case ExportFailureCategory::InvalidArguments: return "invalid_arguments";
	case ExportFailureCategory::InvalidRuntimeLayout: return "invalid_runtime_layout";
	case ExportFailureCategory::OutputConflict: return "output_conflict";
	case ExportFailureCategory::ValidationFailed: return "validation_failed";
	case ExportFailureCategory::ManagedCompilationFailed: return "managed_compilation_failed";
	case ExportFailureCategory::AssetStagingFailed: return "asset_staging_failed";
	case ExportFailureCategory::PackagingFailed: return "packaging_failed";
	case ExportFailureCategory::PackageValidationFailed: return "package_validation_failed";
	case ExportFailureCategory::RuntimeStagingFailed: return "runtime_staging_failed";
	case ExportFailureCategory::EvidenceWriteFailed: return "evidence_write_failed";
	case ExportFailureCategory::PublicationFailed: return "publication_failed";
	case ExportFailureCategory::UnexpectedFailure: return "unexpected_failure";
	}
	return "unexpected_failure";
}

int ExitCodeForFailure(ExportFailureCategory category) {
	switch(category) {
	case ExportFailureCategory::None: return 0;
	case ExportFailureCategory::InvalidArguments:
	case ExportFailureCategory::InvalidRuntimeLayout: return 2;
	case ExportFailureCategory::OutputConflict: return 3;
	case ExportFailureCategory::ValidationFailed: return 4;
	case ExportFailureCategory::ManagedCompilationFailed: return 5;
	case ExportFailureCategory::AssetStagingFailed: return 6;
	case ExportFailureCategory::PackagingFailed:
	case ExportFailureCategory::PackageValidationFailed: return 7;
	case ExportFailureCategory::RuntimeStagingFailed:
	case ExportFailureCategory::EvidenceWriteFailed: return 8;
	case ExportFailureCategory::PublicationFailed: return 9;
	case ExportFailureCategory::UnexpectedFailure: return 10;
	}
	return 10;
}

ExportStage StageForPhase(ExportPhase phase) {
	switch(phase) {
	case ExportPhase::Idle: return ExportStage::Arguments;
	case ExportPhase::Validating: return ExportStage::Validation;
	case ExportPhase::Compiling: return ExportStage::ManagedCompilation;
	case ExportPhase::AssetStaging: return ExportStage::AssetStaging;
	case ExportPhase::Packaging: return ExportStage::Packaging;
	case ExportPhase::Staging: return ExportStage::RuntimeStaging;
	case ExportPhase::Complete: return ExportStage::Complete;
	case ExportPhase::Failed: return ExportStage::Complete;
	}
	return ExportStage::Arguments;
}
