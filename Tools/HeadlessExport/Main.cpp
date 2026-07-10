// Main.cpp - non-interactive TerraEngine export entry point.
//
// This executable links only the shared export support layer. It never creates
// editor UI, a display connection, or a graphics device.

#include "HeadlessExportCommandLine.h"

#include "ExportEvidence.h"
#include "ExportPipeline.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
	ExportResult MakeCommandLineResult(
		const HeadlessExportParseResult& parsed,
		bool helpRequested)
	{
		ExportResult result;
		result.runId = parsed.request.runId.empty() ? "argument-result" : parsed.request.runId;
		result.invocationKind = ExportInvocationKind::HeadlessCli;
		result.success = helpRequested;
		result.stage = ExportStage::Arguments;
		result.failureCategory = helpRequested ? ExportFailureCategory::None : parsed.failureCategory;
		result.exitCode = ExitCodeForFailure(result.failureCategory);
		result.message = helpRequested ? "Help requested." : parsed.message;
		result.errorMessage = result.success ? std::string{} : result.message;
		result.profileId = parsed.request.profile.id;
		result.projectRoot = parsed.request.projectRoot;
		result.outputDirectory = parsed.request.finalOutputDirectory;
		result.evidenceDirectory = parsed.request.evidenceDirectory;
		if(!helpRequested) {
			result.affectedPath = parsed.failureCategory == ExportFailureCategory::InvalidRuntimeLayout
				? parsed.request.runtimeLayout.installRoot
				: (!parsed.request.profilePath.empty() ? parsed.request.profilePath : parsed.request.projectRoot);
			result.validationResult.passed = false;
			result.validationResult.errorCount = 1;
			ExportDiagnostic diagnostic;
			diagnostic.severity = ExportDiagnosticSeverity::Error;
			diagnostic.category = std::string(ToString(parsed.failureCategory));
			diagnostic.stage = "arguments";
			diagnostic.code = "export." + std::string(ToString(parsed.failureCategory));
			diagnostic.message = parsed.message;
			const std::u8string encodedPath = result.affectedPath.generic_u8string();
			diagnostic.filePath.assign(
				reinterpret_cast<const char*>(encodedPath.data()), encodedPath.size());
			result.diagnostics.push_back(diagnostic);
			result.validationResult.diagnostics.push_back(std::move(diagnostic));
		}
		return result;
	}

	int RunHeadlessExport(std::vector<std::string> arguments, const char* argvZero) {
		try {
		const std::filesystem::path launchDirectory = std::filesystem::current_path();
		const std::filesystem::path executablePath = ResolveHeadlessExportExecutablePath(argvZero);
		const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
			arguments, executablePath, launchDirectory);

		if(parsed.showHelp) {
			std::cerr << GetHeadlessExportUsage();
			const ExportResult result = MakeCommandLineResult(parsed, true);
			std::cout << SerializeExportResultRecord(result) << '\n';
			return 0;
		}

		if(!parsed.success) {
			ExportResult result = MakeCommandLineResult(parsed, false);
			std::cerr << '[' << ToString(result.stage) << "/" << ToString(result.failureCategory)
				<< "] " << result.message << '\n';
			if(!parsed.request.evidenceDirectory.empty()) {
				ExportEvidenceInputs evidence;
				std::string ignoredError;
				WriteExportEvidenceBundle(parsed.request, result, evidence, ignoredError);
			}
			std::cout << SerializeExportResultRecord(result) << '\n';
			return result.exitCode;
		}

		ExportPipeline pipeline;
		const ExportResult result = pipeline.Execute(parsed.request, [&parsed](const ExportProgress& progress) {
			std::cout << SerializeExportProgressRecord(parsed.request.runId, progress) << '\n';
			std::cout.flush();
		});
		if(!result.success) {
			std::cerr << '[' << ToString(result.stage) << "/" << ToString(result.failureCategory)
				<< "] " << result.message << '\n';
		}
		std::cout << SerializeExportResultRecord(result) << '\n';
		return result.exitCode;
		} catch(const std::exception& exception) {
		ExportResult result;
		result.runId = "unexpected-failure";
		result.invocationKind = ExportInvocationKind::HeadlessCli;
		result.stage = ExportStage::Arguments;
		result.failureCategory = ExportFailureCategory::UnexpectedFailure;
		result.exitCode = ExitCodeForFailure(result.failureCategory);
		result.message = exception.what();
		result.errorMessage = result.message;
		std::cerr << "[arguments/unexpected_failure] " << result.message << '\n';
		std::cout << SerializeExportResultRecord(result) << '\n';
		return result.exitCode;
		}
	}

#ifdef _WIN32
	std::string PathArgumentToUtf8(const std::filesystem::path& argument) {
		const std::u8string encoded = argument.u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
	std::vector<std::string> arguments;
	for(int index = 1; index < argc; ++index) {
		arguments.push_back(PathArgumentToUtf8(std::filesystem::path(argv[index])));
	}
	// ResolveHeadlessExportExecutablePath uses GetModuleFileNameW on Windows, so
	// no lossy narrow argv[0] fallback is required.
	return RunHeadlessExport(std::move(arguments), nullptr);
}
#else
int main(int argc, char** argv) {
	std::vector<std::string> arguments;
	for(int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
	return RunHeadlessExport(std::move(arguments), argc > 0 ? argv[0] : nullptr);
}
#endif
