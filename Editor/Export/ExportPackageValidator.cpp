// ExportPackageValidator.cpp - TEPAK runtime package validation implementation.

#include "ExportPackageValidator.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "TEPAK/PackReader.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
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
