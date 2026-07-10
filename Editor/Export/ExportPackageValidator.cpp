// ExportPackageValidator.cpp - TEPAK runtime package validation implementation.

#include "ExportPackageValidator.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "TEPAK/PackReader.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
std::string PathToUtf8(const std::filesystem::path& path)
{
	const std::u8string encoded = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

std::string NormalizePackagePath(std::string_view path)
{
	std::string normalized(path);
	for (char& value : normalized) {
		if (value == '\\') {
			value = '/';
		}
		if (value >= 'A' && value <= 'Z') {
			value = static_cast<char>(value + ('a' - 'A'));
		}
	}

	while (!normalized.empty() && normalized.front() == '/') {
		normalized.erase(normalized.begin());
	}
	if (normalized.starts_with("./")) {
		normalized.erase(0, 2);
	}
	return normalized;
}

bool RequireEntry(const std::unordered_set<std::string>& entries, std::string_view path, std::string& outError)
{
	const std::string normalized = NormalizePackagePath(path);
	if (entries.contains(normalized)) {
		return true;
	}

	outError = "Export package is missing required runtime entry: " + normalized;
	return false;
}

bool ValidateTableRange(
	const tepak::ByteBuffer& bytes,
	std::uint32_t offset,
	std::uint32_t count,
	std::uint32_t stride,
	std::string_view label,
	std::string_view path,
	std::string& outError)
{
	if (offset == 0u || count == 0u) {
		outError = "Virtual geometry mesh payload is missing required " + std::string(label) + " metadata: " + std::string(path);
		return false;
	}

	const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(count) * stride;
	if (end > bytes.size()) {
		outError = "Virtual geometry mesh payload " + std::string(label) + " metadata is outside the packed file: " + std::string(path);
		return false;
	}
	return true;
}

bool ValidateVirtualGeometryManifestBlock(const nlohmann::json& virtualGeometry, std::string& outError)
{
	if (!virtualGeometry.contains("requiredMetadata") || !virtualGeometry["requiredMetadata"].is_array()) {
		outError = "Virtual geometry export manifest is missing requiredMetadata.";
		return false;
	}

	std::unordered_set<std::string> requiredMetadata;
	for (const nlohmann::json& entry : virtualGeometry["requiredMetadata"]) {
		if (entry.is_string()) {
			requiredMetadata.insert(entry.get<std::string>());
		}
	}
	static constexpr std::array<std::string_view, 5> kRequiredMetadata = {
		"adaptiveTargets",
		"fallbackCoverage",
		"clusterHierarchy",
		"pageLinks",
		"coneSafety",
	};
	for (std::string_view metadata : kRequiredMetadata) {
		if (!requiredMetadata.contains(std::string(metadata))) {
			outError = "Virtual geometry export manifest is missing required metadata declaration: " + std::string(metadata);
			return false;
		}
	}

	static constexpr std::array<std::string_view, 4> kRequiredQualityFields = {
		"geometryBudgetBytes",
		"lodErrorScale",
		"maxLodDrop",
		"preloadBias",
	};
	for (std::string_view field : kRequiredQualityFields) {
		if (!virtualGeometry.contains(std::string(field))) {
			outError = "Virtual geometry export manifest is missing quality field: " + std::string(field);
			return false;
		}
	}

	if (!virtualGeometry.contains("coverage") || !virtualGeometry["coverage"].is_object()) {
		outError = "Virtual geometry export manifest is missing coverage declaration.";
		return false;
	}
	const nlohmann::json& coverage = virtualGeometry["coverage"];
	if (!coverage.contains("fixtureManifest") || !coverage["fixtureManifest"].is_string() || coverage["fixtureManifest"].get<std::string>().empty()) {
		outError = "Virtual geometry export manifest coverage is missing fixtureManifest.";
		return false;
	}
	if (!coverage.contains("requiredCategories") || !coverage["requiredCategories"].is_array()) {
		outError = "Virtual geometry export manifest coverage is missing requiredCategories.";
		return false;
	}
	bool exportsRuntimeMetadata = false;
	for (const nlohmann::json& category : coverage["requiredCategories"]) {
		if (category.is_string() && category.get<std::string>() == "export") {
			exportsRuntimeMetadata = true;
		}
	}
	if (!exportsRuntimeMetadata) {
		outError = "Virtual geometry export manifest coverage must include the export category.";
		return false;
	}
	return true;
}

bool ValidateRendererRecoveryManifestBlock(const nlohmann::json& rendererRecovery, std::string& outError)
{
	if (!rendererRecovery.is_object()) {
		outError = "Renderer recovery export manifest must be an object.";
		return false;
	}
	if (!rendererRecovery.contains("maxRendererRebuildAttemptsPerIncident")
		|| !rendererRecovery["maxRendererRebuildAttemptsPerIncident"].is_number_unsigned()
		|| rendererRecovery["maxRendererRebuildAttemptsPerIncident"].get<std::uint32_t>() == 0u) {
		outError = "Renderer recovery export manifest is missing a positive rebuild attempt budget.";
		return false;
	}
	if (!rendererRecovery.contains("fatalMessage")
		|| !rendererRecovery["fatalMessage"].is_string()
		|| rendererRecovery["fatalMessage"].get<std::string>().empty()) {
		outError = "Renderer recovery export manifest is missing a fatal message.";
		return false;
	}
	if (rendererRecovery.value("fullApplicationRestart", false)) {
		outError = "Renderer recovery export manifest must not enable full application restart.";
		return false;
	}
	return true;
}

bool ValidateVirtualGeometryTemfPayload(const tepak::ByteBuffer& bytes, std::string_view path, std::string& outError)
{
	const std::string normalizedPath = NormalizePackagePath(path);
	if (bytes.size() < sizeof(MeshHeader)) {
		outError = "Virtual geometry mesh payload is too small: " + normalizedPath;
		return false;
	}

	MeshHeader header{};
	std::memcpy(&header, bytes.data(), sizeof(MeshHeader));
	std::uint32_t magic = 0u;
	std::memcpy(&magic, header.magic, sizeof(magic));
	if (magic != kMeshMagicValue || header.version != kMeshCurrentVersion) {
		outError = "Virtual geometry mesh payload is not a current TEMF file: " + normalizedPath;
		return false;
	}

	if ((header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) == 0u
		|| header.virtualGeometryHeaderOffset == 0u
		|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
		|| header.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration) {
		outError = "Virtual geometry mesh payload is stale or missing its current TEMF virtual geometry header: " + normalizedPath;
		return false;
	}

	const std::uint64_t virtualHeaderEnd = static_cast<std::uint64_t>(header.virtualGeometryHeaderOffset) + sizeof(VirtualGeometryHeader);
	if (virtualHeaderEnd > bytes.size()) {
		outError = "Virtual geometry mesh payload header range is outside the packed file: " + normalizedPath;
		return false;
	}

	VirtualGeometryHeader virtualHeader{};
	std::memcpy(&virtualHeader, bytes.data() + header.virtualGeometryHeaderOffset, sizeof(VirtualGeometryHeader));
	if (virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration
		|| virtualHeader.headerSize != sizeof(VirtualGeometryHeader)
		|| virtualHeader.detailLevelCount == 0u
		|| virtualHeader.sourceTriangleCount == 0u
		|| virtualHeader.sourceVertexCount == 0u) {
		outError = "Virtual geometry mesh payload root metadata is invalid and must be rebuilt: " + normalizedPath;
		return false;
	}

	constexpr std::uint32_t requiredFlags = kVirtualGeometryValidationFlagAdaptiveTargetsPresent
		| kVirtualGeometryValidationFlagFallbackCoverageValidated
		| kVirtualGeometryValidationFlagClusterHierarchyValidated
		| kVirtualGeometryValidationFlagPageLinksValidated
		| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
	if ((virtualHeader.flags & kVirtualGeometryValidationFlagAdaptiveTargetsPresent) == 0u
		|| virtualHeader.requestedTargetCount == 0u
		|| virtualHeader.requestedTargetTableOffset == 0u) {
		outError = "Virtual geometry mesh payload is missing adaptive cook metadata: " + normalizedPath;
		return false;
	}
	if ((virtualHeader.flags & kVirtualGeometryValidationFlagFallbackCoverageValidated) == 0u) {
		outError = "Virtual geometry mesh payload is missing fallback coverage validation: " + normalizedPath;
		return false;
	}
	if ((virtualHeader.flags & kVirtualGeometryValidationFlagClusterHierarchyValidated) == 0u
		|| virtualHeader.clusterCount == 0u
		|| virtualHeader.clusterTableOffset == 0u) {
		outError = "Virtual geometry mesh payload is missing cluster hierarchy metadata: " + normalizedPath;
		return false;
	}
	if ((virtualHeader.flags & kVirtualGeometryValidationFlagPageLinksValidated) == 0u
		|| virtualHeader.pageCount == 0u
		|| virtualHeader.pageTableOffset == 0u
		|| virtualHeader.clusterPageLinkCount == 0u
		|| virtualHeader.clusterPageLinkTableOffset == 0u) {
		outError = "Virtual geometry mesh payload is missing page metadata or cluster-page links: " + normalizedPath;
		return false;
	}
	if ((virtualHeader.flags & kVirtualGeometryValidationFlagConeSafetyMetadataValidated) == 0u
		|| virtualHeader.coneSafetyCount != virtualHeader.clusterCount
		|| virtualHeader.coneSafetyTableOffset == 0u) {
		outError = "Virtual geometry mesh payload is missing cone safety metadata: " + normalizedPath;
		return false;
	}
	if ((virtualHeader.flags & requiredFlags) != requiredFlags) {
		outError = "Virtual geometry mesh payload is missing required adaptive validation flags: " + normalizedPath;
		return false;
	}

	if (!ValidateTableRange(bytes, virtualHeader.detailLevelTableOffset, virtualHeader.detailLevelCount, sizeof(VirtualGeometryDetailLevelRecord), "detail", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.cullableGroupTableOffset, virtualHeader.cullableGroupCount, sizeof(VirtualGeometryCullableGroupRecord), "cullable group", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.requestedTargetTableOffset, virtualHeader.requestedTargetCount, sizeof(VirtualGeometryRequestedTargetRecord), "adaptive target", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.clusterTableOffset, virtualHeader.clusterCount, sizeof(VirtualGeometryClusterRecord), "cluster", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.pageTableOffset, virtualHeader.pageCount, sizeof(VirtualGeometryPageRecord), "page", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.clusterPageLinkTableOffset, virtualHeader.clusterPageLinkCount, sizeof(VirtualGeometryClusterPageLinkRecord), "cluster-page link", normalizedPath, outError)
		|| !ValidateTableRange(bytes, virtualHeader.coneSafetyTableOffset, virtualHeader.coneSafetyCount, sizeof(VirtualGeometryConeSafetyRecord), "cone safety", normalizedPath, outError)) {
		return false;
	}

	return true;
}
}

bool ValidateExportPackage(const std::filesystem::path& packagePath, ExportPlatform platform, std::string& outError)
{
	try {
		// The TEPAK content contract is intentionally platform-neutral. The
		// platform argument documents the caller's target and leaves room for
		// platform-specific packaged payloads without changing existing callers.
		(void)platform;
		const tepak::PackReader reader = tepak::PackReader::Open(packagePath);
		std::unordered_set<std::string> entries;
		for (const tepak::PackEntry& entry : reader.GetEntries()) {
			entries.insert(entry.normalizedPath);
		}

		static constexpr std::array<std::string_view, 26> kRequiredRuntimeEntries = {
			"game.json",
			"fonts/roboto-medium.ttf",
			"shaders/default.task.tspf",
			"shaders/default.mesh.tspf",
			"shaders/default.frag.tspf",
			"shaders/editor/sceneviewgrid.vert.spv",
			"shaders/editor/sceneviewgrid.frag.spv",
			"shaders/sky/skybackground.task.tspf",
			"shaders/sky/skybackground.mesh.tspf",
			"shaders/sky/skybackground.frag.tspf",
			"shaders/ui/ui_instanced.vert.spv",
			"shaders/ui/ui_instanced.frag.spv",
			"shaders/ui/ui_static.vert.spv",
			"shaders/ui/ui_static.frag.spv",
			"shaders/ui/ui_sdf_text.vert.spv",
			"shaders/ui/ui_sdf_text.frag.spv",
			"shaders/lighting/clusterdepthmask.comp.spv",
			"shaders/lighting/clustergrid.comp.spv",
			"shaders/lighting/clusterlightcount.comp.spv",
			"shaders/lighting/clusterlightfill.comp.spv",
			"shaders/lighting/clusterlightprefix.comp.spv",
			"shaders/lighting/lightcull.comp.spv",
			"shaders/postprocess/hizgenerate.comp.spv",
			"shaders/postprocess/vrsgenerate.comp.spv",
			"shaders/postprocess/gtao.comp.spv",
			"shaders/postprocess/gtaoblur.comp.spv",
		};

		for (std::string_view requiredEntry : kRequiredRuntimeEntries) {
			if (!RequireEntry(entries, requiredEntry, outError)) {
				return false;
			}
		}

		const tepak::ByteBuffer manifestBytes = reader.ReadAll("game.json");
		if (manifestBytes.empty()) {
			outError = "Export package contains an empty game.json.";
			return false;
		}

		const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
		const nlohmann::json manifest = nlohmann::json::parse(manifestText);

		if (manifest.contains("virtualGeometry") && manifest["virtualGeometry"].is_object()) {
			if (!ValidateVirtualGeometryManifestBlock(manifest["virtualGeometry"], outError)) {
				return false;
			}
			static constexpr std::array<std::string_view, 4> kRequiredVirtualGeometryEntries = {
				"shaders/default.task.tspf",
				"shaders/default.mesh.tspf",
				"shaders/default.frag.tspf",
				"shaders/postprocess/hizgenerate.comp.spv",
			};
			for (std::string_view requiredVirtualGeometryEntry : kRequiredVirtualGeometryEntries) {
				if (!RequireEntry(entries, requiredVirtualGeometryEntry, outError)) {
					outError = "Virtual geometry export is missing required shader resource: " + NormalizePackagePath(requiredVirtualGeometryEntry);
					return false;
				}
			}
		}

		if (!manifest.contains("rendererRecovery")
			|| !ValidateRendererRecoveryManifestBlock(manifest["rendererRecovery"], outError)) {
			return false;
		}

		if (auto startupIt = manifest.find("startupScene"); startupIt != manifest.end() && startupIt->is_string()) {
			const std::string startupScene = startupIt->get<std::string>();
			if (!startupScene.empty() && !RequireEntry(entries, startupScene, outError)) {
				outError = "Export package startup scene is missing from TEPAK: " + NormalizePackagePath(startupScene);
				return false;
			}
		}

		if (auto assetsIt = manifest.find("assets"); assetsIt != manifest.end() && assetsIt->is_array()) {
			for (const nlohmann::json& assetEntry : *assetsIt) {
				if (!assetEntry.is_object()) {
					continue;
				}

				const std::string assetPath = assetEntry.value("path", std::string{});
				if (!assetPath.empty() && !RequireEntry(entries, assetPath, outError)) {
					outError = "Export package manifest references missing asset path: " + NormalizePackagePath(assetPath);
					return false;
				}
				const bool requiresVirtualGeometry = assetEntry.contains("virtualGeometry")
					&& assetEntry["virtualGeometry"].is_object()
					&& assetEntry["virtualGeometry"].value("required", false);
				if (requiresVirtualGeometry) {
					if (assetPath.empty()) {
						outError = "Virtual geometry manifest asset is missing its compiled TEMF path.";
						return false;
					}
					const tepak::ByteBuffer assetBytes = reader.ReadAll(assetPath);
					if (!ValidateVirtualGeometryTemfPayload(assetBytes, assetPath, outError)) {
						return false;
					}
				}
			}
		}
	}
	catch (const std::exception& exception) {
		outError = std::string("Failed to validate export package: ") + exception.what();
		return false;
	}

	outError.clear();
	return true;
}

bool ValidateExportPackage(const std::filesystem::path& packagePath, std::string& outError)
{
	return ValidateExportPackage(packagePath, ExportPlatform::Windows, outError);
}

ExportPackageValidationReport InspectExportPackage(
	const std::filesystem::path& packagePath,
	ExportPlatform platform)
{
	ExportPackageValidationReport report;
	report.archivePath = packagePath;
	std::error_code errorCode;
	report.archiveSize = std::filesystem::file_size(packagePath, errorCode);
	if(errorCode) report.archiveSize = 0;

	std::string validationError;
	report.passed = ValidateExportPackage(packagePath, platform, validationError);
	if(!report.passed) {
		ExportPackageValidationFinding finding;
		finding.code = "export.package.runtime_contract";
		finding.message = validationError;
		finding.required = true;
		report.findings.push_back(std::move(finding));
	}

	try {
		const tepak::PackReader reader = tepak::PackReader::Open(packagePath);
		report.entryCount = static_cast<std::uint32_t>(reader.GetEntries().size());
		const tepak::ByteBuffer manifestBytes = reader.ReadAll("game.json");
		report.archivedManifestJson.assign(
			reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
		for(const tepak::PackEntry& entry : reader.GetEntries()) {
			const tepak::ByteBuffer bytes = reader.ReadAll(entry.normalizedPath);
			static constexpr std::string_view marker = "placeholder";
			const bool hasPlaceholder = std::search(
				bytes.begin(), bytes.end(), marker.begin(), marker.end(),
				[](std::uint8_t left, char right) {
					return static_cast<char>(std::tolower(static_cast<unsigned char>(left))) == right;
				}) != bytes.end();
			if(hasPlaceholder) {
				report.passed = false;
				report.findings.push_back({
					"export.package.placeholder_payload",
					entry.normalizedPath,
					"Packaged entry contains the forbidden placeholder marker.",
					true,
					true
				});
			}
		}
	} catch(const std::exception& exception) {
		if(report.findings.empty()) {
			report.findings.push_back({
				"export.package.read_failed", PathToUtf8(packagePath), exception.what(), true, false
			});
		}
		report.passed = false;
	}

	std::ostringstream summary;
	summary << "Archive: " << PathToUtf8(packagePath) << '\n';
	summary << "Archive size: " << report.archiveSize << " bytes\n";
	summary << "Entry count: " << report.entryCount << '\n';
	summary << "Startup scene: " << (report.passed ? "PASS" : "see findings") << '\n';
	summary << "Required runtime entries: " << (report.passed ? "PASS" : "see findings") << '\n';
	summary << "Manifest asset references: " << (report.passed ? "PASS" : "see findings") << '\n';
	summary << "Placeholder scan: "
		<< (std::none_of(report.findings.begin(), report.findings.end(), [](const auto& finding) { return finding.placeholderDetected; }) ? "PASS" : "FAIL") << '\n';
	for(const ExportPackageValidationFinding& finding : report.findings) {
		summary << "[" << finding.code << "] " << finding.path << " " << finding.message << '\n';
	}
	summary << "Final: " << (report.passed ? "PASS" : "FAIL") << '\n';
	report.summaryText = summary.str();
	return report;
}
