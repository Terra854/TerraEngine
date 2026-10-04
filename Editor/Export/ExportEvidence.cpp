// ExportEvidence.cpp - deterministic export evidence serialization.

#include "ExportEvidence.h"
#include "Runtime/ArtifactValidation.h"

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
		if(path.filename() == "PhysicsSettings.json") return "physics_settings";
		if(extension == ".tephysmat") return "physics_material";
		if(extension == ".tepcoll") return "cooked_collider";
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
		if(path.filename() == "PhysicsSettings.json") return "generated_runtime_settings";
		const std::string extension = path.extension().string();
		if(extension == ".temf" || extension == ".tmat" || extension == ".tetf" || extension == ".tspf" || extension == ".spv"
			|| extension == ".tephysmat" || extension == ".tepcoll") {
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

	// Pull the portable provenance fields from game.json once, then reuse the
	// compact summary in every durable evidence document. This prevents a later
	// evidence reader from needing build-machine paths or a provider SDK.
	Json PhysicsEvidenceSummary(const std::string& manifestText) {
		try {
			const Json manifest = Json::parse(manifestText);
			const auto physicsIt = manifest.find("physics");
			if(physicsIt == manifest.end() || !physicsIt->is_object()) return {{"present", false}};
			const Json& physics = *physicsIt;
			const Json provider = physics.value("provider", Json::object());
			const Json settings = physics.value("settings", Json::object());
			return {
				{"present", true}, {"schemaVersion", physics.value("schemaVersion", 0U)},
				{"provider", provider}, {"cookFormatVersion", physics.value("cookFormatVersion", 0U)},
				{"settingsHash", settings.value("contentHash", std::string{})},
				{"layersHash", physics.value("layersHash", std::string{})},
				{"defaultMaterialGuid", physics.value("defaultMaterialGuid", std::string{})},
				{"materialGuids", [&physics]() { Json values = Json::array(); for(const Json& asset : physics.value("materials", Json::array())) values.push_back(asset.value("guid", std::string{})); return values; }()},
				{"colliderGuids", [&physics]() { Json values = Json::array(); for(const Json& asset : physics.value("colliders", Json::array())) values.push_back(asset.value("guid", std::string{})); return values; }()},
				{"validation", physics.value("validation", Json::object())}
			};
		}
		catch(...) { return {{"present", false}, {"parseError", true}}; }
	}

	std::string PhysicsPackageSummaryText(const Json& physics) {
		if(!physics.value("present", false)) return "Physics: no resolved physics manifest section\n";
		const Json provider = physics.value("provider", Json::object());
		std::ostringstream output;
		output << "Physics provider: " << provider.value("identifier", std::string("unknown"))
			<< ' ' << provider.value("version", std::string("unknown"))
			<< " revision " << provider.value("revision", std::string("unknown")) << '\n';
		output << "Physics settings hash: " << physics.value("settingsHash", std::string{})
			<< ", layers hash: " << physics.value("layersHash", std::string{}) << '\n';
		output << "Physics assets: " << physics.value("materialGuids", Json::array()).size() << " materials, "
			<< physics.value("colliderGuids", Json::array()).size() << " cooked colliders\n";
		return output.str();
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
		const auto validation = TerraEngine::Runtime::ValidateArtifact({
			std::as_bytes(std::span(bytes)), TerraEngine::Runtime::InferArtifactKind(inventoryEntry.logicalPath),
			TerraEngine::Runtime::HostNativeImageTarget(), inventoryEntry.logicalPath});
		// The compatibility flag names explicit producer stub evidence only.
		// An invalid required artifact is rejected separately, never renamed a stub.
		inventoryEntry.placeholderDetected = validation.IsExplicitStub();
		if(inventoryEntry.required && !validation.IsValid()) {
			outError = validation.code + ": " + inventoryEntry.logicalPath + ": " + validation.diagnostic;
			return false;
		}
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
	const Json physics = PhysicsEvidenceSummary(inputs.resolvedManifestJson);
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
		{"diagnostics", std::move(validationDiagnostics)},
		{"physics", physics}
	};
	const Json inventoryDocument = {
		{"schemaVersion", 1}, {"runId", request.runId}, {"entryCount", inventory.size()}, {"entries", std::move(inventoryEntries)}, {"physics", physics}
	};

	std::string progressText;
	for(const std::string& line : inputs.progressJsonLines) progressText += line + "\n";
	if(physics.value("present", false)) {
		progressText += Json({{"type", "physics_provenance"}, {"schemaVersion", 1}, {"runId", request.runId},
			{"stage", "asset_staging"}, {"physics", physics}}).dump() + "\n";
	}
	const std::string finalRecord = SerializeExportResultRecord(result);
	if(inputs.progressJsonLines.empty() || inputs.progressJsonLines.back() != finalRecord) progressText += finalRecord + "\n";

	// A completed-stage manifest is written byte-for-byte so CI can compare it
	// directly with archived game.json. Only the explicit partial placeholder
	// uses a trailing newline for readability.
	const std::string manifestText = inputs.resolvedManifestJson.empty() ? "{}\n" : inputs.resolvedManifestJson;
	std::string packageText = inputs.packageValidationReport.empty()
		? "Package validation did not run.\nFinal: FAIL\n"
		: inputs.packageValidationReport;
	if(!packageText.empty() && packageText.back() != '\n') packageText += '\n';
	packageText += PhysicsPackageSummaryText(physics);
	Json resultDocument = ResultToJson(result);
	resultDocument["physics"] = physics;

	return WriteTextFile(request.evidenceDirectory / "export-result.json", resultDocument.dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "resolved-manifest.json", manifestText, outError)
		&& WriteTextFile(request.evidenceDirectory / "content-inventory.json", inventoryDocument.dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "validation-report.json", validation.dump(2) + "\n", outError)
		&& WriteTextFile(request.evidenceDirectory / "progress.jsonl", progressText, outError)
		&& WriteTextFile(request.evidenceDirectory / "package-validation.txt", packageText, outError);
}
