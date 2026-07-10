// ExportEvidence.cpp - deterministic export evidence serialization.

#include "ExportEvidence.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

namespace {
	using Json = nlohmann::json;

	std::string PathToUtf8(const std::filesystem::path& path) {
		const std::u8string encoded = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	std::string NormalizeLogicalPath(const std::filesystem::path& path) {
		return PathToUtf8(path);
	}

	std::string InferAssetType(const std::filesystem::path& path) {
		const std::string extension = path.extension().string();
		if(path.filename() == "game.json") return "manifest";
		if(extension == ".tescene") return "scene";
		if(extension == ".temf") return "mesh";
		if(extension == ".tmat") return "material";
		if(extension == ".tetf") return "texture";
		if(extension == ".teui" || extension == ".ui") return "ui";
		if(extension == ".cs" || extension == ".dll") return "script";
		if(extension == ".tspf" || extension == ".spv") return "shader";
		if(extension == ".ttf" || extension == ".otf") return "font";
		return "other";
	}

	std::string InferSourceKind(const std::filesystem::path& path) {
		if(path.filename() == "game.json") return "generated_manifest";
		const std::string extension = path.extension().string();
		if(extension == ".temf" || extension == ".tmat" || extension == ".tetf" || extension == ".tspf" || extension == ".spv") {
			return "compiled";
		}
		if(InferAssetType(path) == "shader" || InferAssetType(path) == "font") return "engine_runtime";
		return "raw";
	}

	std::string HashBytes(const std::vector<unsigned char>& bytes) {
		// FNV-1a is deliberately simple and deterministic; the evidence hash is an
		// identity signal rather than a security boundary.
		std::uint64_t hash = 14695981039346656037ull;
		for(unsigned char byte : bytes) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		std::ostringstream stream;
		stream << std::hex << std::setfill('0') << std::setw(16) << hash;
		return stream.str();
	}

	bool ContainsPlaceholder(const std::vector<unsigned char>& bytes) {
		static constexpr std::array<unsigned char, 11> marker = {
			'p', 'l', 'a', 'c', 'e', 'h', 'o', 'l', 'd', 'e', 'r'
		};
		if(bytes.size() < marker.size()) return false;
		return std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end(), [](unsigned char left, unsigned char right) {
			return static_cast<unsigned char>(std::tolower(left)) == right;
		}) != bytes.end();
	}

	Json DiagnosticToJson(const ExportDiagnostic& diagnostic) {
		std::string severity = "info";
		if(diagnostic.severity == ExportDiagnosticSeverity::Warning) severity = "warning";
		if(diagnostic.severity == ExportDiagnosticSeverity::Error) severity = "error";
		return {
			{"severity", severity},
			{"category", diagnostic.category},
			{"stage", diagnostic.stage.empty() ? "validation" : diagnostic.stage},
			{"code", diagnostic.code.empty() ? "export.validation.finding" : diagnostic.code},
			{"message", diagnostic.message},
			{"path", diagnostic.filePath},
			{"line", diagnostic.line},
			{"suggestion", diagnostic.suggestion}
		};
	}

	std::vector<ExportDiagnostic> SortedDiagnostics(const ExportResult& result) {
		std::vector<ExportDiagnostic> diagnostics = result.diagnostics;
		if(diagnostics.empty()) diagnostics = result.validationResult.diagnostics;
		std::sort(diagnostics.begin(), diagnostics.end(), [](const ExportDiagnostic& left, const ExportDiagnostic& right) {
			return std::tie(left.stage, left.severity, left.category, left.filePath, left.code)
				< std::tie(right.stage, right.severity, right.category, right.filePath, right.code);
		});
		return diagnostics;
	}

	Json ResultToJson(const ExportResult& result) {
		const std::vector<ExportDiagnostic> sortedDiagnostics = SortedDiagnostics(result);
		const std::uint32_t errorCount = static_cast<std::uint32_t>(std::count_if(
			sortedDiagnostics.begin(), sortedDiagnostics.end(), [](const auto& diagnostic) {
				return diagnostic.severity == ExportDiagnosticSeverity::Error;
			}));
		const std::uint32_t warningCount = static_cast<std::uint32_t>(std::count_if(
			sortedDiagnostics.begin(), sortedDiagnostics.end(), [](const auto& diagnostic) {
				return diagnostic.severity == ExportDiagnosticSeverity::Warning;
			}));
		Json diagnostics = Json::array();
		for(const ExportDiagnostic& diagnostic : sortedDiagnostics) diagnostics.push_back(DiagnosticToJson(diagnostic));
		return {
			{"type", "result"},
			{"schemaVersion", result.schemaVersion},
			{"runId", result.runId},
			{"success", result.success},
			{"invocationKind", ToString(result.invocationKind)},
			{"stage", ToString(result.stage)},
			{"failureCategory", ToString(result.failureCategory)},
			{"exitCode", result.exitCode},
			{"message", result.message.empty() ? result.errorMessage : result.message},
			{"affectedPath", PathToUtf8(result.affectedPath)},
			{"profileId", result.profileId},
			{"projectRoot", PathToUtf8(result.projectRoot)},
			{"outputDirectory", PathToUtf8(result.outputDirectory)},
			{"evidenceDirectory", PathToUtf8(result.evidenceDirectory)},
			{"elapsedSeconds", result.totalElapsedSeconds},
			{"validation", {{"errorCount", errorCount}, {"warningCount", warningCount}}},
			{"package", {{"archivePath", PathToUtf8(result.packageResult.outputPath)}, {"byteSize", result.packageResult.archiveSize}, {"entryCount", result.packageResult.entryCount}}},
			{"diagnostics", std::move(diagnostics)}
		};
	}

	bool WriteTextFile(const std::filesystem::path& path, const std::string& contents, std::string& outError) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) {
			outError = "Could not open evidence file for writing: " + PathToUtf8(path);
			return false;
		}
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		if(!output.good()) {
			outError = "Could not finish writing evidence file: " + PathToUtf8(path);
			return false;
		}
		return true;
	}
}

bool BuildExportContentInventory(
	const std::filesystem::path& rootDirectory,
	std::vector<ExportContentInventoryEntry>& outEntries,
	std::string& outError)
{
	outEntries.clear();
	outError.clear();
	std::error_code errorCode;
	if(!std::filesystem::is_directory(rootDirectory, errorCode)) {
		outError = "Package input directory was not found: " + PathToUtf8(rootDirectory);
		return false;
	}

	for(const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(rootDirectory, errorCode)) {
		if(errorCode) {
			outError = "Could not enumerate package input: " + errorCode.message();
			return false;
		}
		if(!entry.is_regular_file()) continue;

		std::ifstream input(entry.path(), std::ios::binary);
		std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if(!input.eof() && input.fail()) {
			outError = "Could not read staged package input: " + PathToUtf8(entry.path());
			return false;
		}

		ExportContentInventoryEntry inventoryEntry;
		const std::filesystem::path relative = std::filesystem::relative(entry.path(), rootDirectory, errorCode);
		if(errorCode) {
			outError = "Could not normalize staged package path: " + PathToUtf8(entry.path());
			return false;
		}
		inventoryEntry.logicalPath = NormalizeLogicalPath(relative);
		inventoryEntry.assetType = InferAssetType(relative);
		inventoryEntry.sourceKind = InferSourceKind(relative);
		inventoryEntry.byteSize = bytes.size();
		inventoryEntry.contentHash = HashBytes(bytes);
		inventoryEntry.placeholderDetected = ContainsPlaceholder(bytes);
		outEntries.push_back(std::move(inventoryEntry));
	}

	NormalizeExportContentInventory(outEntries);
	return true;
}

void NormalizeExportContentInventory(std::vector<ExportContentInventoryEntry>& entries) {
	std::sort(entries.begin(), entries.end(), [](const ExportContentInventoryEntry& left, const ExportContentInventoryEntry& right) {
		return left.logicalPath < right.logicalPath;
	});
}

bool EquivalentExportContentInventories(
	std::vector<ExportContentInventoryEntry> left,
	std::vector<ExportContentInventoryEntry> right)
{
	NormalizeExportContentInventory(left);
	NormalizeExportContentInventory(right);
	if(left.size() != right.size()) return false;
	for(std::size_t index = 0; index < left.size(); ++index) {
		const auto& a = left[index];
		const auto& b = right[index];
		if(a.logicalPath != b.logicalPath || a.assetType != b.assetType || a.sourceKind != b.sourceKind
			|| a.byteSize != b.byteSize || a.contentHash != b.contentHash || a.required != b.required
			|| a.placeholderDetected != b.placeholderDetected) return false;
	}
	return true;
}

std::string NormalizeResolvedManifestForParity(const std::string& manifestJson) {
	try {
		Json manifest = Json::parse(manifestJson);
		manifest.erase("runId");
		manifest.erase("invocationKind");
		manifest.erase("outputDirectory");
		return manifest.dump();
	} catch(...) {
		return {};
	}
}

std::string SerializeExportProgressRecord(const std::string& runId, const ExportProgress& progress) {
	return Json({
		{"type", "progress"},
		{"schemaVersion", 1},
		{"runId", runId},
		{"stage", ToString(progress.stage)},
		{"progress", progress.overallProgress},
		{"message", progress.statusMessage}
	}).dump();
}

std::string SerializeExportResultRecord(const ExportResult& result) {
	return ResultToJson(result).dump();
}

bool WriteExportEvidenceBundle(
	const ExportRequest& request,
	const ExportResult& result,
	const ExportEvidenceInputs& inputs,
	std::string& outError)
{
	outError.clear();
	std::error_code errorCode;
	std::filesystem::create_directories(request.evidenceDirectory, errorCode);
	if(errorCode) {
		outError = "Could not create evidence directory: " + errorCode.message();
		return false;
	}

	std::vector<ExportContentInventoryEntry> inventory = inputs.inventory;
	NormalizeExportContentInventory(inventory);
	Json inventoryEntries = Json::array();
	for(const ExportContentInventoryEntry& entry : inventory) {
		inventoryEntries.push_back({
			{"logicalPath", entry.logicalPath}, {"assetType", entry.assetType}, {"sourceKind", entry.sourceKind},
			{"byteSize", entry.byteSize}, {"contentHash", entry.contentHash}, {"required", entry.required},
			{"placeholderDetected", entry.placeholderDetected}
		});
	}

	const std::vector<ExportDiagnostic> sortedDiagnostics = SortedDiagnostics(result);
	const std::uint32_t errorCount = static_cast<std::uint32_t>(std::count_if(
		sortedDiagnostics.begin(), sortedDiagnostics.end(), [](const auto& diagnostic) {
			return diagnostic.severity == ExportDiagnosticSeverity::Error;
		}));
	const std::uint32_t warningCount = static_cast<std::uint32_t>(std::count_if(
		sortedDiagnostics.begin(), sortedDiagnostics.end(), [](const auto& diagnostic) {
			return diagnostic.severity == ExportDiagnosticSeverity::Warning;
		}));
	Json validationDiagnostics = Json::array();
	for(const ExportDiagnostic& diagnostic : sortedDiagnostics) validationDiagnostics.push_back(DiagnosticToJson(diagnostic));
	const Json validation = {
		{"schemaVersion", 1},
		{"passed", result.success && result.validationResult.passed},
		{"errorCount", errorCount},
		{"warningCount", warningCount},
		{"diagnostics", std::move(validationDiagnostics)}
	};
	const Json inventoryDocument = {
		{"schemaVersion", 1}, {"runId", request.runId}, {"entryCount", inventory.size()}, {"entries", std::move(inventoryEntries)}
	};

	std::string progressText;
	for(const std::string& line : inputs.progressJsonLines) progressText += line + "\n";
	const std::string finalRecord = SerializeExportResultRecord(result);
	if(inputs.progressJsonLines.empty() || inputs.progressJsonLines.back() != finalRecord) progressText += finalRecord + "\n";

	// A completed-stage manifest is written byte-for-byte so CI can compare it
	// directly with archived game.json. Only the explicit partial placeholder
	// uses a trailing newline for readability.
	const std::string manifestText = inputs.resolvedManifestJson.empty() ? "{}\n" : inputs.resolvedManifestJson;
	const std::string packageText = inputs.packageValidationReport.empty()
		? "Package validation did not run.\nFinal: FAIL\n"
		: inputs.packageValidationReport;

	return WriteTextFile(request.evidenceDirectory / "export-result.json", ResultToJson(result).dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "resolved-manifest.json", manifestText, outError)
		&& WriteTextFile(request.evidenceDirectory / "content-inventory.json", inventoryDocument.dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "validation-report.json", validation.dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "progress.jsonl", progressText, outError)
		&& WriteTextFile(request.evidenceDirectory / "package-validation.txt", packageText, outError);
}
