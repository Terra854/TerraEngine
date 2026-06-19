// ExportPackageValidator.cpp - TEPAK runtime package validation implementation.

#include "ExportPackageValidator.h"

#include "TEPAK/PackReader.h"

#include <array>
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
