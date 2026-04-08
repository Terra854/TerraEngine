// ExportValidator.cpp — Pre-export validation implementation.

#include "ExportValidator.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

// --- Helpers ---

static void AddDiagnostic(ExportValidationResult& result,
	ExportDiagnosticSeverity severity,
	const std::string& category,
	const std::string& message,
	const std::string& filePath = {},
	int line = 0)
{
	ExportDiagnostic diag;
	diag.severity = severity;
	diag.category = category;
	diag.message = message;
	diag.filePath = filePath;
	diag.line = line;
	result.diagnostics.push_back(std::move(diag));

	if (severity == ExportDiagnosticSeverity::Error) {
		result.errorCount++;
		result.passed = false;
	}
	else if (severity == ExportDiagnosticSeverity::Warning) {
		result.warningCount++;
	}
}

// --- Profile validation ---

ExportValidationResult ExportValidator::ValidateProfile(const BuildProfile& profile) const {
	ExportValidationResult result;

	// Name must be non-empty.
	if (profile.name.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile name cannot be empty.");
	}

	// ID must exist.
	if (profile.id.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Build profile ID is missing. Save the profile to generate one.");
	}

	// Output directory should be set.
	if (profile.outputDirectory.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Warning,
			"Profile", "Output directory is not set. A default will be used.");
	}

	// Every export must nominate a startup scene because exported games now
	// always boot through a concrete scene instead of supporting a no-scene path.
	if (profile.startupScene.empty()) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"Profile", "Startup scene must be selected.");
	}

	return result;
}

// --- Managed source validation ---

ExportValidationResult ExportValidator::ValidateManagedSource(const std::filesystem::path& projectRoot) const {
	ExportValidationResult result;

	if (projectRoot.empty() || !std::filesystem::exists(projectRoot)) {
		AddDiagnostic(result, ExportDiagnosticSeverity::Error,
			"ManagedSource", "Project root directory does not exist: " + projectRoot.string());
		return result;
	}

	// Resolve the source directory from the .csproj path.
	const auto projectDir = projectRoot;

	// Scan all .cs files in the project directory for unsupported patterns.
	// These regex patterns detect common anti-patterns that break NativeAOT.
	std::regex reflectionPattern(R"(\bType\.GetType\b|\bActivator\.CreateInstance\b|\bAssembly\.Load\b|\bMethodInfo\b|\bFieldInfo\b)");
	std::regex dynamicPattern(R"(\bdynamic\b)");
	
	// Fast rudimentary check for [SerializeField] applied to known non-blittable types like string or object.
	std::regex nonBlittableFieldPattern(R"(\[SerializeField\]\s*(?:public\s+|private\s+|protected\s+|internal\s+)*(?:string|object|dynamic)\b)");

	uint32_t scannedFiles = 0;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(projectDir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".cs") continue;

		scannedFiles++;

		std::ifstream file(entry.path());
		if (!file) continue;

		std::string line;
		int lineNumber = 0;
		while (std::getline(file, line)) {
			lineNumber++;

			// Check for reflection usage.
			if (std::regex_search(line, reflectionPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"Reflection API usage detected. NativeAOT export does not support runtime reflection.",
					entry.path().string(), lineNumber);
			}

			// Check for dynamic keyword usage.
			if (std::regex_search(line, dynamicPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Warning,
					"ManagedSource",
					"'dynamic' keyword usage detected. This may fail in NativeAOT builds.",
					entry.path().string(), lineNumber);
			}

			// Check for non-blittable serialized fields.
			if (std::regex_search(line, nonBlittableFieldPattern)) {
				AddDiagnostic(result, ExportDiagnosticSeverity::Error,
					"ManagedSource",
					"Unsupported non-blittable type used with [SerializeField]. NativeAOT interop only supports primitive blittable types (e.g. float, int, Vector3).",
					entry.path().string(), lineNumber);
			}
		}
	}

	AddDiagnostic(result, ExportDiagnosticSeverity::Info,
		"ManagedSource",
		"Scanned " + std::to_string(scannedFiles) + " C# source file(s).");

	return result;
}

// --- Combined validation ---

ExportValidationResult ExportValidator::ValidateAll(const BuildProfile& profile, const std::filesystem::path& projectRoot) const {
	ExportValidationResult result = ValidateProfile(profile);

	if (profile.enableManagedCompilation) {
		auto sourceResult = ValidateManagedSource(projectRoot);
		
		result.errorCount += sourceResult.errorCount;
		result.warningCount += sourceResult.warningCount;
		if (!sourceResult.passed) {
			result.passed = false;
		}
		result.diagnostics.insert(result.diagnostics.end(),
			sourceResult.diagnostics.begin(),
			sourceResult.diagnostics.end());
	}

	return result;
}
