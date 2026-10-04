// ExportPackageValidator.cpp - TEPAK runtime package validation implementation.

#include "ExportPackageValidator.h"
#include "Runtime/ArtifactValidation.h"
#include "Physics/Assets/PhysicsMaterial.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "TEPAK/PackReader.h"
#include "Utilities/UUID.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <span>
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
	// Older packages did not declare a mode. Continue accepting them as
	// fail-closed packages, but reject any new manifest that claims a different
	// runtime recovery behavior.
	if (rendererRecovery.contains("mode")
		&& (!rendererRecovery["mode"].is_string()
			|| rendererRecovery["mode"].get<std::string>() != "failClosed")) {
		outError = "Renderer failure export manifest must use failClosed mode.";
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
	const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(
		std::span<const std::uint8_t>(bytes.data(), bytes.size()), {});
	if (!validated.IsComplete()) {
		outError = "Virtual geometry mesh payload requires a canonical TEMFPRD1 recook: "
			+ normalizedPath + ". " + validated.diagnostic;
		return false;
	}
	if ((validated.directory.header.representationFlags
			& kTemfRepresentationVirtualGeometry) == 0u) {
		outError = "Virtual geometry mesh payload does not declare the canonical virtual representation: "
			+ normalizedPath;
		return false;
	}
	const auto rootBlock = std::find_if(
		validated.directory.blocks.begin(),
		validated.directory.blocks.end(),
		[](const TemfBlockDescriptor& block) {
			return block.streamKind
					== static_cast<std::uint16_t>(TemfStreamKind::Metadata)
				&& block.attributeMask == kTemfMetadataVirtualHeader;
		});
	if (rootBlock == validated.directory.blocks.end()) {
		outError = "Virtual geometry mesh payload is missing its canonical hierarchy root: "
			+ normalizedPath;
		return false;
	}
	const std::span<const std::uint8_t> encoded(bytes.data()
			+ static_cast<std::size_t>(rootBlock->encodedPayloadOffset),
		static_cast<std::size_t>(rootBlock->encodedBytes));
	const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock(
		{ *rootBlock, encoded, validated.directory.quantizationDomains, 0u },
		{});
	VirtualGeometryHeader root{};
	if (!decoded.IsComplete() || decoded.bytes.size() != sizeof(root)) {
		outError = "Virtual geometry mesh payload has an invalid canonical hierarchy root: "
			+ normalizedPath;
		return false;
	}
	std::memcpy(&root, decoded.bytes.data(), sizeof(root));
	constexpr std::uint32_t requiredFlags =
		kVirtualGeometryValidationFlagAdaptiveTargetsPresent
		| kVirtualGeometryValidationFlagFallbackCoverageValidated
		| kVirtualGeometryValidationFlagClusterHierarchyValidated
		| kVirtualGeometryValidationFlagPageLinksValidated
		| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
	if (root.payloadGeneration != kVirtualGeometryPayloadGeneration
		|| (root.flags & requiredFlags) != requiredFlags
		|| root.requestedTargetCount == 0u
		|| root.pageCount == 0u
		|| root.clusterPageLinkCount == 0u
		|| root.coneSafetyCount == 0u) {
		outError = "Virtual geometry mesh payload does not prove all required production metadata: "
			+ normalizedPath;
		return false;
	}
	return true;
}

std::string HashPhysicsPackageBytes(const tepak::ByteBuffer& bytes)
{
	// This intentionally matches the deterministic FNV-1a export provenance
	// hash. It is an integrity cross-check, not a replacement for TEPAK hashes.
	std::uint64_t hash = 14695981039346656037ULL;
	for(const std::uint8_t byte : bytes) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	std::ostringstream output;
	output << std::hex << std::setfill('0') << std::setw(16) << hash;
	return output.str();
}


bool ValidatePhysicsManifestAsset(
	const tepak::PackReader& reader,
	const std::unordered_set<std::string>& entries,
	const nlohmann::json& asset,
	std::string_view assetKind,
	std::unordered_set<std::string>& packagePaths,
	std::string& outError)
{
	if(!asset.is_object()) {
		outError = "Physics manifest " + std::string(assetKind) + " entry must be an object.";
		return false;
	}
	const std::string path = asset.value("packagePath", std::string{});
	const std::string checksum = asset.value("checksum", std::string{});
	const std::string guid = asset.value("guid", std::string{});
	const std::uint64_t expectedSize = asset.value("size", 0ULL);
	const std::string expectedExtension = assetKind == "collider" ? ".tepcoll" : ".tephysmat";
	if(guid.empty() || path.empty() || checksum.empty() || expectedSize == 0U) {
		outError = "Physics manifest " + std::string(assetKind) + " is missing its GUID, package path, checksum, or non-zero size.";
		return false;
	}
	if(NormalizePackagePath(path).ends_with(".tecollider")) {
		outError = "Physics manifest references source-only collider recipe '" + NormalizePackagePath(path) + "'. Re-cook it to a .tepcoll envelope before exporting.";
		return false;
	}
	if(!NormalizePackagePath(path).ends_with(expectedExtension)) {
		outError = "Physics manifest " + std::string(assetKind) + " has the wrong runtime extension: " + NormalizePackagePath(path);
		return false;
	}
	if(!packagePaths.insert(NormalizePackagePath(path)).second) {
		outError = "Physics manifest references the same runtime payload more than once: " + NormalizePackagePath(path);
		return false;
	}
	if(!RequireEntry(entries, path, outError)) {
		outError = "Physics manifest references a missing runtime " + std::string(assetKind) + ": " + NormalizePackagePath(path);
		return false;
	}

	const tepak::ByteBuffer bytes = reader.ReadAll(path);
	if(bytes.empty()) {
		outError = "Physics runtime " + std::string(assetKind) + " payload is empty: " + NormalizePackagePath(path) + ". Re-cook or republish it before exporting.";
		return false;
	}
	const auto validity = TerraEngine::Runtime::ValidateArtifact({std::as_bytes(std::span(bytes)),
		assetKind == "collider" ? TerraEngine::Runtime::ArtifactKind::CookedCollider : TerraEngine::Runtime::ArtifactKind::PhysicsMaterial,
		TerraEngine::Runtime::HostNativeImageTarget(), path});
	if(!validity.IsValid()) {
		outError = validity.code + ": " + path + ": " + validity.diagnostic;
		return false;
	}
	if(bytes.size() != expectedSize || HashPhysicsPackageBytes(bytes) != checksum) {
		outError = "Physics runtime " + std::string(assetKind) + " payload checksum or size does not match the manifest: " + NormalizePackagePath(path) + ". Re-export after recooking the asset.";
		return false;
	}

	if(assetKind == "collider") {
		TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
		const std::span<const std::byte> colliderBytes(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
		const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(colliderBytes, diagnostics);
		if(!collider) {
			const std::string detail = diagnostics.entries.empty() ? "The envelope is malformed." : diagnostics.entries.front().message;
			outError = "Physics cooked collider is corrupt: " + NormalizePackagePath(path) + ". " + detail + " Re-cook it before exporting.";
			return false;
		}
		const auto expectedId = TerraEngine::ParseUUID(guid);
		if(!expectedId || *expectedId != collider->Envelope().assetId) {
			outError = "Physics collider GUID differs from its validated envelope: " + path;
			return false;
		}
		const auto compatibility = collider->CheckCompatibility(
			TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
			TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
			TerraEngine::Physics::kJoltCookProviderVersion,
			TerraEngine::Physics::kJoltCookRevision);
		if(!compatibility.compatible) {
			outError = "Physics cooked collider provider mismatch: " + NormalizePackagePath(path) + ". " + compatibility.message + " Re-cook with Jolt 5.6.1 revision 2e28006e4a367624bb512bf2c37ca99a93d9d1ea.";
			return false;
		}
	}
	if(assetKind == "material") {
		const auto document = nlohmann::json::parse(bytes.begin(), bytes.end());
		TerraEngine::Physics::PhysicsMaterial material; TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
		const auto expectedId = TerraEngine::ParseUUID(guid);
		if(!TerraEngine::Physics::PhysicsMaterial::LoadJson(document, material, diagnostics)
			|| !expectedId || *expectedId != material.assetId) {
			outError = "Physics material GUID differs from its validated schema: " + path;
			return false;
		}
	}

	return true;
}

bool ValidatePhysicsManifestBlock(
	const nlohmann::json& physics,
	const tepak::PackReader& reader,
	const std::unordered_set<std::string>& entries,
	std::string& outError)
{
	if(!physics.is_object() || physics.value("schemaVersion", 0U) != 1U) {
		outError = "Physics export manifest is missing supported schemaVersion 1.";
		return false;
	}
	if(!physics.contains("provider") || !physics["provider"].is_object()
		|| physics["provider"].value("identifier", std::string{}) != "Jolt"
		|| physics["provider"].value("version", std::string{}) != "5.6.1"
		|| physics["provider"].value("revision", std::string{}) != "2e28006e4a367624bb512bf2c37ca99a93d9d1ea") {
		outError = "Physics export manifest must require Jolt 5.6.1 revision 2e28006e4a367624bb512bf2c37ca99a93d9d1ea.";
		return false;
	}
	if(physics.value("cookFormatVersion", 0U) != TerraEngine::Physics::kJoltCookPayloadFormatVersion) {
		outError = "Physics export manifest cookFormatVersion is unsupported. Re-cook all colliders with the current project provider.";
		return false;
	}
	if(!physics.contains("settings") || !physics["settings"].is_object()) {
		outError = "Physics export manifest is missing its runtime settings dependency.";
		return false;
	}
	const nlohmann::json& settings = physics["settings"];
	const std::string settingsPath = settings.value("packagePath", std::string{});
	const std::string settingsHash = settings.value("contentHash", std::string{});
	if(settingsPath.empty() || settingsHash.empty() || settings.value("schemaVersion", 0U) == 0U || physics.value("layersHash", std::string{}).empty()) {
		outError = "Physics export manifest settings, schema, content hash, or layers hash is missing.";
		return false;
	}
	if(!RequireEntry(entries, settingsPath, outError)) {
		outError = "Physics runtime settings payload is missing: " + NormalizePackagePath(settingsPath);
		return false;
	}
	const tepak::ByteBuffer settingsBytes = reader.ReadAll(settingsPath);
	if(settingsBytes.empty() || HashPhysicsPackageBytes(settingsBytes) != settingsHash) {
		outError = "Physics runtime settings payload is empty or checksum-mismatched: " + NormalizePackagePath(settingsPath);
		return false;
	}
	try {
		const nlohmann::json settingsDocument = nlohmann::json::parse(settingsBytes.begin(), settingsBytes.end());
		if(!settingsDocument.is_object() || settingsDocument.value("schemaVersion", 0U) != settings.value("schemaVersion", 0U)
			|| !settingsDocument.contains("layers") || !settingsDocument["layers"].is_array() || settingsDocument["layers"].size() != 32U) {
			outError = "Physics runtime settings payload is invalid or does not contain 32 collision layers.";
			return false;
		}
		const std::string serializedLayers = settingsDocument["layers"].dump();
		tepak::ByteBuffer layerBytes;
		layerBytes.reserve(serializedLayers.size());
		for(const char value : serializedLayers) layerBytes.push_back(static_cast<std::uint8_t>(value));
		if(HashPhysicsPackageBytes(layerBytes) != physics.value("layersHash", std::string{})) {
			outError = "Physics runtime collision-layer hash does not match the manifest. Re-export after saving Physics Settings.";
			return false;
		}
	}
	catch(const std::exception&) {
		outError = "Physics runtime settings payload is not valid JSON.";
		return false;
	}

	if(!physics.contains("materials") || !physics["materials"].is_array()
		|| !physics.contains("colliders") || !physics["colliders"].is_array()
		|| !physics.contains("validation") || physics["validation"].value("status", std::string{}) != "validated"
		|| physics["validation"].value("fixtureSchemaVersion", 0U) == 0U) {
		outError = "Physics export manifest is missing validated material, collider, or validation records.";
		return false;
	}
	std::unordered_set<std::string> packagePaths;
	std::unordered_set<std::string> materialGuids;
	std::unordered_set<std::string> colliderGuids;
	for(const nlohmann::json& material : physics["materials"]) {
		if(!ValidatePhysicsManifestAsset(reader, entries, material, "material", packagePaths, outError)) return false;
		const std::string materialGuid = material.value("guid", std::string{});
		if(!materialGuids.insert(materialGuid).second) {
			outError = "Physics export manifest repeats material GUID '" + materialGuid + "'. Keep each runtime material identity unique.";
			return false;
		}

		// Checksums prove transport integrity, while this typed decode proves the
		// referenced bytes are still an engine-owned physics material instead of
		// arbitrary JSON with a copied checksum.
		const tepak::ByteBuffer materialBytes = reader.ReadAll(material.value("packagePath", std::string{}));
		try {
			const nlohmann::json materialDocument = nlohmann::json::parse(materialBytes.begin(), materialBytes.end());
			const std::optional<TerraEngine::UUID> parsedGuid = TerraEngine::ParseUUID(materialGuid);
			TerraEngine::Physics::PhysicsMaterial decodedMaterial;
			TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
			if(!parsedGuid || !TerraEngine::Physics::PhysicsMaterial::LoadJson(materialDocument, decodedMaterial, diagnostics)
				|| decodedMaterial.assetId != *parsedGuid) {
				outError = "Physics runtime material payload is malformed or does not match manifest GUID '" + materialGuid + "'. Rebuild the material before exporting.";
				return false;
			}
		}
		catch(const std::exception&) {
			outError = "Physics runtime material payload is not valid JSON for manifest GUID '" + materialGuid + "'. Rebuild the material before exporting.";
			return false;
		}
	}
	const std::string defaultMaterialGuid = physics.value("defaultMaterialGuid", std::string{});
	if((!materialGuids.empty() && defaultMaterialGuid.empty())
		|| (!defaultMaterialGuid.empty() && !materialGuids.contains(defaultMaterialGuid))) {
		outError = "Physics export manifest defaultMaterialGuid does not resolve to a packaged physics material. Select a staged material or clear the optional default.";
		return false;
	}
	for(const nlohmann::json& collider : physics["colliders"]) {
		if(collider.value("sourceProvenanceHash", std::string{}).empty() || collider.value("recipeHash", std::string{}).empty()) {
			outError = "Physics cooked collider manifest record is missing source or recipe provenance. Re-cook it before exporting.";
			return false;
		}
		if(!ValidatePhysicsManifestAsset(reader, entries, collider, "collider", packagePaths, outError)) return false;
		const std::string colliderGuid = collider.value("guid", std::string{});
		if(!colliderGuids.insert(colliderGuid).second) {
			outError = "Physics export manifest repeats cooked collider GUID '" + colliderGuid + "'. Keep each runtime collider identity unique.";
			return false;
		}
	}
	return true;
}

bool ValidatePhysicsStartupScene(
	const tepak::PackReader& reader,
	std::string_view startupScenePath,
	const nlohmann::json& physics,
	std::string& outError)
{
	// Physics data may be represented by component records or by the portable
	// parity-fixture metadata. A text scan keeps this package gate compatible
	// with legacy scene encodings while still blocking a source-only dependency
	// before a runtime world can be created.
	const tepak::ByteBuffer sceneBytes = reader.ReadAll(startupScenePath);
	if(sceneBytes.empty()) {
		outError = "Physics export package startup scene is empty, so physics dependencies cannot be validated before launch.";
		return false;
	}
	const std::string sceneText(reinterpret_cast<const char*>(sceneBytes.data()), sceneBytes.size());
	if(sceneText.find(".tecollider") != std::string::npos) {
		outError = "Export package startup scene still references a source-only .tecollider recipe. Cook and stage its .tepcoll envelope before exporting.";
		return false;
	}
	if(sceneText.find("\"physicsExportFixture\"") != std::string::npos
		&& physics.value("colliders", nlohmann::json::array()).empty()) {
		outError = "Physics startup fixture requires at least one packaged cooked collider. Run the collider cooker before exporting.";
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

		static constexpr std::array<std::string_view, 31> kRequiredRuntimeEntries = {
			"game.json",
			"fonts/roboto-medium.ttf",
			"shaders/default.task.tspf",
			"shaders/default.mesh.tspf",
			"shaders/default.frag.tspf",
			"shaders/default.conventionalvisibilityidentity.frag.spv",
			"shaders/editor/sceneviewgrid.vert.spv",
			"shaders/editor/sceneviewgrid.frag.spv",
			"shaders/sky/skybackground.task.tspf",
			"shaders/sky/skybackground.mesh.tspf",
			"shaders/sky/skybackground.frag.tspf",
			"shaders/terrain/terrainvisibility.task.tspf",
			"shaders/terrain/terrainvisibility.mesh.tspf",
			"shaders/terrain/terrainvisibility.frag.tspf",
			"shaders/terrain/terrainsurface.comp.tspf",
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

		// The same typed policy checks all required known formats during staging,
		// packaging, and startup. Opaque authored payloads retain their legal words.
		for(const tepak::PackEntry& entry : reader.GetEntries()) {
			const auto payload = reader.ReadAll(entry.normalizedPath);
			const auto validity = TerraEngine::Runtime::ValidateArtifact({std::as_bytes(std::span(payload)),
				TerraEngine::Runtime::InferArtifactKind(entry.normalizedPath),
				platform == ExportPlatform::Windows ? TerraEngine::Runtime::NativeImageTarget::WindowsX64
					: TerraEngine::Runtime::NativeImageTarget::LinuxX64, entry.normalizedPath});
			if(!validity.IsValid()) { outError = validity.code + ": " + entry.normalizedPath + ": " + validity.diagnostic; return false; }
		}

		// Recipes are intentionally editor-only. Reject them even if a manifest
		// forgets to list the file, because a standalone runtime must not depend on
		// source geometry or a machine-installed cook tool.
		for(const std::string& entry : entries) {
			if(entry.ends_with(".tecollider")) {
				outError = "Export package contains source-only collider recipe: " + entry + ". Package the cooked .tepcoll output instead.";
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

		if(!manifest.contains("startupScene") || !manifest["startupScene"].is_string() || manifest["startupScene"].get<std::string>().empty()) {
			outError = "Export package is missing a startup scene. Select and stage a startup scene before exporting.";
			return false;
		}
		const std::string startupScene = manifest["startupScene"].get<std::string>();
		if(!RequireEntry(entries, startupScene, outError)) {
			outError = "Export package startup scene is missing from TEPAK: " + NormalizePackagePath(startupScene);
			return false;
		}

		// Physics is optional for legacy non-physics packages. Once present, the
		// complete block is mandatory and is validated before startup so corrupt
		// provider bytes can never reach a newly created runtime physics world.
		if(manifest.contains("physics") && !ValidatePhysicsManifestBlock(manifest["physics"], reader, entries, outError)) {
			return false;
		}
		if(manifest.contains("physics") && !ValidatePhysicsStartupScene(reader, startupScene, manifest["physics"], outError)) {
			return false;
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
			const auto validity = TerraEngine::Runtime::ValidateArtifact({std::as_bytes(std::span(bytes)),
				TerraEngine::Runtime::InferArtifactKind(entry.normalizedPath),
				platform == ExportPlatform::Windows ? TerraEngine::Runtime::NativeImageTarget::WindowsX64
					: TerraEngine::Runtime::NativeImageTarget::LinuxX64, entry.normalizedPath});
			if(!validity.IsValid()) {
				report.passed = false;
				report.findings.push_back({validity.code, entry.normalizedPath, validity.diagnostic, true, validity.IsExplicitStub()});
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
	summary << "Explicit stub evidence: "
		<< (std::none_of(report.findings.begin(), report.findings.end(), [](const auto& finding) { return finding.placeholderDetected; }) ? "PASS" : "FAIL") << '\n';
	for(const ExportPackageValidationFinding& finding : report.findings) {
		summary << "[" << finding.code << "] " << finding.path << " " << finding.message << '\n';
	}
	summary << "Final: " << (report.passed ? "PASS" : "FAIL") << '\n';
	report.summaryText = summary.str();
	return report;
}
