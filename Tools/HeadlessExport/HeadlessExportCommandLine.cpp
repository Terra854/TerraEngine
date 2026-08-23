// HeadlessExportCommandLine.cpp - argument validation and canonical request construction.

#include "HeadlessExportCommandLine.h"

#include "ExportRuntimeLayout.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#elif defined(__linux__)
	#include <array>
	#include <unistd.h>
#endif

namespace {
	std::string PathToUtf8(const std::filesystem::path& path) {
		const std::u8string encoded = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	std::filesystem::path ResolvePath(
		const std::string& suppliedText,
		const std::filesystem::path& launchDirectory,
		bool mustExist,
		std::string& outError)
	{
		const std::filesystem::path suppliedPath = std::filesystem::u8path(
			suppliedText.begin(), suppliedText.end());
		const std::filesystem::path absolutePath = suppliedPath.is_absolute()
			? suppliedPath
			: launchDirectory / suppliedPath;
		std::error_code errorCode;
		// Existing inputs use their canonical identity. New output/evidence paths
		// preserve the caller's absolute spelling so Windows does not unexpectedly
		// rewrite a short-path component (RUNNER~1) into its long-path alias.
		const std::filesystem::path resolved = mustExist
			? std::filesystem::canonical(absolutePath, errorCode)
			: std::filesystem::absolute(absolutePath, errorCode).lexically_normal();
		if(errorCode) {
			outError = "Could not resolve path '" + suppliedText + "': " + errorCode.message();
			return {};
		}
		return resolved;
	}

	std::filesystem::path NormalizePathForComparison(const std::filesystem::path& path) {
		// Safety checks compare filesystem identity, independently from the stable
		// user-facing spelling retained in the export request and evidence.
		std::error_code errorCode;
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, errorCode);
		return (errorCode ? std::filesystem::absolute(path) : canonical).lexically_normal();
	}

	bool IsSameOrDescendant(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
		const std::filesystem::path normalizedCandidate = NormalizePathForComparison(candidate);
		const std::filesystem::path normalizedParent = NormalizePathForComparison(parent);
		const std::filesystem::path relative = normalizedCandidate.lexically_relative(normalizedParent);
		if(relative.empty()) return normalizedCandidate == normalizedParent;
		const auto first = relative.begin();
		return first == relative.end() || *first != "..";
	}

	std::string GenerateRunId() {
		static std::atomic<std::uint64_t> sequence = 0;
		const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		std::ostringstream stream;
		stream << "export-" << ticks << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
		return stream.str();
	}

	bool IsSafeRunId(const std::string& runId) {
		return !runId.empty() && std::all_of(runId.begin(), runId.end(), [](unsigned char character) {
			return std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.';
		});
	}

	bool ValidateProfileJson(const std::filesystem::path& profilePath, std::string& outError) {
		try {
			std::ifstream input(profilePath);
			const nlohmann::json document = nlohmann::json::parse(input);
			if(!document.is_object() || !document.contains("id") || !document["id"].is_string()) {
				outError = "Build profile must be a JSON object with a string id: " + PathToUtf8(profilePath);
				return false;
			}
			return true;
		} catch(const std::exception& exception) {
			outError = "Build profile JSON is invalid: " + std::string(exception.what());
			return false;
		}
	}
}

HeadlessExportParseResult ParseHeadlessExportCommandLine(
	const std::vector<std::string>& arguments,
	const std::filesystem::path& executablePath,
	const std::filesystem::path& launchDirectory)
{
	HeadlessExportParseResult result;
	std::map<std::string, std::string> values;
	const std::set<std::string> valueOptions = {"--project", "--profile", "--output", "--evidence", "--run-id"};

	for(std::size_t index = 0; index < arguments.size(); ++index) {
		const std::string& argument = arguments[index];
		if(argument == "--help") {
			if(arguments.size() != 1) {
				result.message = "--help cannot be combined with export options.";
				return result;
			}
			result.success = true;
			result.showHelp = true;
			result.failureCategory = ExportFailureCategory::None;
			return result;
		}
		if(!valueOptions.contains(argument)) {
			result.message = "Unknown argument: " + argument;
			return result;
		}
		if(values.contains(argument)) {
			result.message = "Duplicate argument: " + argument;
			return result;
		}
		if(index + 1 >= arguments.size() || arguments[index + 1].starts_with("--")) {
			result.message = "Missing value for argument: " + argument;
			return result;
		}
		values.emplace(argument, arguments[++index]);
	}

	for(const char* required : {"--project", "--profile", "--output"}) {
		if(!values.contains(required)) {
			result.message = std::string("Missing required argument: ") + required;
			return result;
		}
	}

	std::string pathError;
	result.request.projectRoot = ResolvePath(values["--project"], launchDirectory, true, pathError);
	if(result.request.projectRoot.empty()) {
		result.message = pathError;
		return result;
	}
	// Projects created by the installed editor use game.json as their canonical
	// project manifest. Older automation fixtures still carry project.json, so
	// accept either marker during the compatibility window instead of requiring
	// users to manufacture a duplicate file solely for headless export.
	const bool hasGameManifest =
		std::filesystem::is_regular_file(result.request.projectRoot / "game.json");
	const bool hasLegacyProjectManifest =
		std::filesystem::is_regular_file(result.request.projectRoot / "project.json");
	if(!std::filesystem::is_directory(result.request.projectRoot)
		|| (!hasGameManifest && !hasLegacyProjectManifest)) {
		result.message = "Project root must contain game.json or legacy project.json: "
			+ PathToUtf8(result.request.projectRoot);
		return result;
	}

	result.request.profilePath = ResolvePath(values["--profile"], launchDirectory, true, pathError);
	if(result.request.profilePath.empty() || !std::filesystem::is_regular_file(result.request.profilePath)) {
		result.message = pathError.empty() ? "Build profile is not a file." : pathError;
		return result;
	}
	if(!ValidateProfileJson(result.request.profilePath, result.message)) return result;

	result.request.profile = LoadBuildProfile(result.request.profilePath);
	result.request.finalOutputDirectory = ResolvePath(values["--output"], launchDirectory, false, pathError);
	if(result.request.finalOutputDirectory.empty()) {
		result.message = pathError;
		return result;
	}
	result.request.runId = values.contains("--run-id") ? values["--run-id"] : GenerateRunId();
	if(!IsSafeRunId(result.request.runId)) {
		result.message = "Run id may contain only letters, numbers, '.', '_' and '-'.";
		return result;
	}

	if(values.contains("--evidence")) {
		result.request.evidenceDirectory = ResolvePath(values["--evidence"], launchDirectory, false, pathError);
	} else {
		result.request.evidenceDirectory = BuildDefaultExportEvidenceDirectory(
			result.request.projectRoot, result.request.runId);
	}
	if(result.request.evidenceDirectory.empty()) {
		result.message = pathError;
		return result;
	}

	if(result.request.finalOutputDirectory == result.request.projectRoot
		|| IsSameOrDescendant(result.request.projectRoot, result.request.finalOutputDirectory)) {
		result.message = "Output directory must not equal or contain the source project.";
		return result;
	}
	if(IsSameOrDescendant(result.request.evidenceDirectory, result.request.finalOutputDirectory)
		|| IsSameOrDescendant(result.request.finalOutputDirectory, result.request.evidenceDirectory)) {
		result.message = "Evidence directory must be outside the transactional output directory.";
		return result;
	}

	if(!DiscoverExportRuntimeLayout(executablePath, result.request.runtimeLayout, result.message)
		|| !ValidateExportRuntimeLayout(result.request.runtimeLayout, result.request.profile.platform, result.message)) {
		result.failureCategory = ExportFailureCategory::InvalidRuntimeLayout;
		return result;
	}
	if(IsSameOrDescendant(result.request.finalOutputDirectory, result.request.runtimeLayout.installRoot)
		|| IsSameOrDescendant(result.request.runtimeLayout.installRoot, result.request.finalOutputDirectory)) {
		result.message = "Output directory and installed runtime must not contain one another.";
		return result;
	}

	// The output override is applied only to this request-local copy. The source
	// profile file is never saved or modified by headless export.
	result.request.profile.outputDirectory = result.request.finalOutputDirectory;
	result.request.invocationKind = ExportInvocationKind::HeadlessCli;
	result.success = true;
	result.failureCategory = ExportFailureCategory::None;
	return result;
}

std::string GetHeadlessExportUsage() {
	return
		"Usage: TerraEngine-Export --project <project-root> --profile <build-profile.json> "
		"--output <final-output-directory> [--evidence <evidence-directory>] "
		"[--run-id <stable-test-run-id>] [--help]\n";
}

std::filesystem::path ResolveHeadlessExportExecutablePath(const char* argvZero) {
#ifdef _WIN32
	std::wstring buffer(32768, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if(length > 0 && length < buffer.size()) {
		buffer.resize(length);
		return std::filesystem::path(buffer);
	}
#elif defined(__linux__)
	std::array<char, 4096> buffer{};
	const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	if(length > 0) return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
	return std::filesystem::absolute(argvZero == nullptr ? "TerraEngine-Export" : argvZero);
}
