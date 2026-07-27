// ExportPipeline.cpp — Export build process implementation
//
// Implements the detailed steps of the export pipeline, including project manifest generation, script binding rewrites for NativeAOT, and output directory staging. It manages the execution of external tools like the dotnet CLI and handles progress reporting throughout the build process.

#include "ExportPipeline.h"
#include "ExportEvidence.h"
#include "AnimationExportDependencies.h"
#include "ExportOutputTransaction.h"
#include "ExportPackageValidator.h"
#include "ExportScriptBindingRewriter.h"
#include "NativeAotProjectWorkspace.h"
#include "io/PathNormalizer.h"
#include "validation/AnimationPackValidator.h"
#include "AssetImportResult.h"
#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Physics/PhysicsSettings.h"
#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

// --- Helpers ---

static std::string PathToUtf8(const std::filesystem::path& path) {
	const std::u8string encoded = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

// Export provenance hashes are deterministic identifiers for package auditing,
// not cryptographic signatures. The same FNV-1a representation is used by
// ExportEvidence so manifest and inventory comparisons remain straightforward.
static std::string HashBytesForExport(const std::vector<std::byte>& bytes) {
	std::uint64_t hash = 14695981039346656037ULL;
	for(const std::byte byte : bytes) {
		hash ^= static_cast<std::uint8_t>(byte);
		hash *= 1099511628211ULL;
	}
	std::ostringstream output;
	output << std::hex << std::setfill('0') << std::setw(16) << hash;
	return output.str();
}

static bool ReadExportBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes) {
	std::ifstream input(path, std::ios::binary);
	if(!input) return false;
	std::vector<char> rawBytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	bytes.resize(rawBytes.size());
	for(std::size_t index = 0U; index < rawBytes.size(); ++index) {
		bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(rawBytes[index]));
	}
	return input.eof() || !input.fail();
}

static std::string HashPhysicsValue(std::uint64_t value) {
	std::ostringstream output;
	output << std::hex << std::setfill('0') << std::setw(16) << value;
	return output.str();
}

static nlohmann::json SerializePhysicsSettings(const TerraEngine::Physics::PhysicsSettings& settings) {
	nlohmann::json layers = nlohmann::json::array();
	for(const TerraEngine::Physics::CollisionLayer& layer : settings.layers) {
		layers.push_back({
			{"id", layer.id}, {"name", layer.name}, {"collisionMask", layer.collisionMask},
			{"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}
		});
	}
	return {
		{"schemaVersion", settings.schemaVersion},
		{"gravity", {settings.gravity.x, settings.gravity.y, settings.gravity.z}},
		{"fixedTimeStep", settings.fixedTimeStep}, {"maxSubsteps", settings.maxSubsteps},
		{"maxAccumulatedTime", settings.maxAccumulatedTime}, {"velocityIterations", settings.velocityIterations},
		{"positionIterations", settings.positionIterations}, {"layers", std::move(layers)},
		{"maxCastDistance", settings.maxCastDistance}, {"maxShapeExtent", settings.maxShapeExtent},
		{"maxHitsPerQuery", settings.maxHitsPerQuery}, {"maxRequestsPerBatch", settings.maxRequestsPerBatch},
		{"retainedResultTicks", settings.retainedResultTicks},
		{"debugVisualizationEnabled", settings.debugVisualizationEnabled},
		{"retainedDebugTicks", settings.retainedDebugTicks}, {"maxDebugPrimitives", settings.maxDebugPrimitives}
	};
}

static bool LoadValidatedPhysicsSettings(
	const std::filesystem::path& settingsPath,
	TerraEngine::Physics::PhysicsSettings& settings,
	nlohmann::json& resolvedDocument,
	std::string& error)
{
	settings = TerraEngine::Physics::PhysicsSettings::Defaults();
	if(!std::filesystem::exists(settingsPath)) {
		// Projects created before Physics Settings existed still export the
		// provider-neutral defaults as an explicit runtime payload.
		resolvedDocument = SerializePhysicsSettings(settings);
		return true;
	}

	try {
		std::ifstream input(settingsPath);
		if(!input) { error = "Physics Settings could not be opened: " + PathToUtf8(settingsPath); return false; }
		nlohmann::json document;
		input >> document;
		if(!document.is_object() || !document.contains("layers") || !document["layers"].is_array()
			|| document["layers"].size() != settings.layers.size()) {
			error = "Physics Settings must provide exactly 32 stable collision layers.";
			return false;
		}
		settings.schemaVersion = document.value("schemaVersion", settings.schemaVersion);
		if(!document.contains("gravity") || !document["gravity"].is_array() || document["gravity"].size() != 3U) {
			error = "Physics Settings must provide a three-value gravity vector.";
			return false;
		}
		settings.gravity = {document["gravity"][0].get<float>(), document["gravity"][1].get<float>(), document["gravity"][2].get<float>()};
		settings.fixedTimeStep = document.value("fixedTimeStep", settings.fixedTimeStep);
		settings.maxSubsteps = document.value("maxSubsteps", settings.maxSubsteps);
		settings.maxAccumulatedTime = document.value("maxAccumulatedTime", settings.maxAccumulatedTime);
		settings.velocityIterations = document.value("velocityIterations", settings.velocityIterations);
		settings.positionIterations = document.value("positionIterations", settings.positionIterations);
		settings.maxCastDistance = document.value("maxCastDistance", settings.maxCastDistance);
		settings.maxShapeExtent = document.value("maxShapeExtent", settings.maxShapeExtent);
		settings.maxHitsPerQuery = document.value("maxHitsPerQuery", settings.maxHitsPerQuery);
		settings.maxRequestsPerBatch = document.value("maxRequestsPerBatch", settings.maxRequestsPerBatch);
		settings.retainedResultTicks = document.value("retainedResultTicks", settings.retainedResultTicks);
		settings.debugVisualizationEnabled = document.value("debugVisualizationEnabled", settings.debugVisualizationEnabled);
		settings.retainedDebugTicks = document.value("retainedDebugTicks", settings.retainedDebugTicks);
		settings.maxDebugPrimitives = document.value("maxDebugPrimitives", settings.maxDebugPrimitives);
		for(std::size_t index = 0U; index < settings.layers.size(); ++index) {
			const nlohmann::json& source = document["layers"][index];
			if(!source.is_object()) { error = "Physics Settings layer " + std::to_string(index) + " is not an object."; return false; }
			auto& destination = settings.layers[index];
			destination.id = source.value("id", destination.id);
			destination.name = source.value("name", destination.name);
			destination.collisionMask = source.value("collisionMask", destination.collisionMask);
			destination.broadPhaseGroup = source.value("broadPhaseGroup", destination.broadPhaseGroup);
			destination.enabled = source.value("enabled", destination.enabled);
		}
		const TerraEngine::Physics::PhysicsSettingsValidationResult validation = settings.Validate();
		if(!validation.accepted) { error = "Physics Settings are invalid: " + validation.message; return false; }
		resolvedDocument = SerializePhysicsSettings(settings);
		return true;
	}
	catch(const std::exception& exception) {
		error = std::string("Physics Settings JSON is invalid: ") + exception.what();
		return false;
	}
}

static nlohmann::json BuildPhysicsManifestJson(const PhysicsExportManifest& physics) {
	auto assetToJson = [](const PhysicsExportAssetRecord& asset) {
		return nlohmann::json{{"guid", asset.guid}, {"packagePath", asset.packagePath}, {"checksum", asset.checksum},
			{"size", asset.byteSize}, {"sourceProvenanceHash", asset.sourceProvenanceHash}, {"recipeHash", asset.recipeHash}};
	};
	nlohmann::json materials = nlohmann::json::array();
	for(const PhysicsExportAssetRecord& material : physics.materials) materials.push_back(assetToJson(material));
	nlohmann::json colliders = nlohmann::json::array();
	for(const PhysicsExportAssetRecord& collider : physics.colliders) colliders.push_back(assetToJson(collider));
	return {
		{"schemaVersion", physics.schemaVersion},
		{"provider", {{"identifier", physics.providerIdentifier}, {"version", physics.providerVersion}, {"revision", physics.providerRevision}}},
		{"cookFormatVersion", physics.cookFormatVersion},
		{"settings", {{"packagePath", physics.settingsPackagePath}, {"schemaVersion", physics.settingsSchemaVersion}, {"contentHash", physics.settingsHash}}},
		{"layersHash", physics.layersHash}, {"defaultMaterialGuid", physics.defaultMaterialGuid},
		{"materials", std::move(materials)}, {"colliders", std::move(colliders)},
		{"validation", {{"fixtureSchemaVersion", physics.validationFixtureSchemaVersion}, {"status", physics.validationStatus}}}
	};
}

// Staged scenes and prefabs must point at immutable runtime envelopes, never
// at authoring recipes.  The export path deliberately performs this rewrite
// after every cooked payload has been discovered so metadata-less headless
// cooks are handled exactly like editor-imported assets.
static bool RewriteStagedPhysicsColliderReferences(
	const std::filesystem::path& stagingDirectory,
	std::string& error)
{
	for(const auto& entry : std::filesystem::recursive_directory_iterator(stagingDirectory)) {
		if(!entry.is_regular_file()) continue;
		const std::filesystem::path extension = entry.path().extension();
		if(extension != ".tescene" && extension != ".teprefab") continue;

		std::ifstream input(entry.path());
		if(!input) {
			error = "Could not open staged scene while resolving physics collider references: " + PathToUtf8(entry.path());
			return false;
		}
		nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
		if(document.is_discarded()) {
			error = "Could not parse staged scene while resolving physics collider references: " + PathToUtf8(entry.path());
			return false;
		}

		bool changed = false;
		std::function<bool(nlohmann::json&)> rewrite = [&](nlohmann::json& node) {
			if(node.is_object()) {
				for(auto& [_, child] : node.items()) {
					if(!rewrite(child)) return false;
				}
				return true;
			}
			if(node.is_array()) {
				for(nlohmann::json& child : node) {
					if(!rewrite(child)) return false;
				}
				return true;
			}
			if(!node.is_string()) return true;

			const std::filesystem::path sourcePath = node.get<std::string>();
			if(sourcePath.extension() != ".tecollider") return true;
			std::filesystem::path runtimePath = sourcePath;
			const auto firstPart = runtimePath.begin();
			if(firstPart != runtimePath.end() && firstPart->generic_string() == "Assets") {
				runtimePath = runtimePath.lexically_relative("Assets");
			}
			runtimePath.replace_extension(".tepcoll");
			if(runtimePath.empty() || !std::filesystem::exists(stagingDirectory / runtimePath)) {
				error = "Staged scene references source-only collider '" + sourcePath.generic_string()
					+ "' without a cooked runtime envelope. Run the collider cooker before exporting.";
				return false;
			}
			node = runtimePath.generic_string();
			changed = true;
			return true;
		};

		if(!rewrite(document)) return false;
		if(!changed) continue;
		std::ofstream output(entry.path(), std::ios::trunc);
		if(!output) {
			error = "Could not write staged scene after resolving physics collider references: " + PathToUtf8(entry.path());
			return false;
		}
		output << document.dump(4);
		if(!output.good()) {
			error = "Could not finish staged scene physics collider rewrite: " + PathToUtf8(entry.path());
			return false;
		}
	}
	return true;
}

static void EmitProgress(ExportProgressCallback& cb, ExportPhase phase, float progress, const std::string& message, bool finished = false) {
	if (!cb) return;
	ExportProgress p;
	p.phase = phase;
	p.stage = StageForPhase(phase);
	p.overallProgress = progress;
	p.statusMessage = message;
	p.finished = finished;
	cb(p);
}

// ResolveOutputDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::filesystem::path ResolveOutputDirectory(const BuildProfile& profile, const std::filesystem::path& projectRoot) {
	if (!profile.outputDirectory.empty()) {
		return profile.outputDirectory;
	}

	return projectRoot / "Builds" / profile.name;
}

// ResolveProjectExportName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string ResolveProjectExportName(const std::filesystem::path& projectRoot, const BuildProfile& profile) {
	// Export artifacts are named after the project directory so the launcher can
	// deterministically resolve "<ProjectName>.tepak" back to the matching
	// executable at runtime. If the directory name is somehow empty, fall back
	// to the profile name instead of producing an empty filename.
	std::string projectName = PathToUtf8(projectRoot.filename());
	if (projectName.empty()) {
		projectName = profile.name.empty() ? "TerraEngine" : profile.name;
	}
	return projectName;
}

// ResolveArchiveFileName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static std::string ResolveArchiveFileName(const std::string& projectName) {
	return projectName + ".tepak";
}

// RemoveIfExists is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove(path, errorCode);
}

// RemoveDirectoryIfExists is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveDirectoryIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove_all(path, errorCode);
}

static std::string QualityFromRank(std::uint32_t rank)
{
	if (rank == 0u) return "disabled";
	if (rank == 1u) return "low";
	if (rank == 2u) return "medium";
	return "high";
}

static std::string ShadowQualityFromSettings(const nlohmann::json& settings)
{
	const std::uint32_t maxShadowLights = settings.value("maxShadowLights", 8u);
	const std::uint32_t shadowResolution = settings.value("shadowResolution", 2048u);
	if (maxShadowLights == 0u || shadowResolution == 0u) return "disabled";
	if (maxShadowLights <= 2u || shadowResolution <= 1024u) return "low";
	if (maxShadowLights <= 4u || shadowResolution <= 1536u) return "medium";
	return "high";
}

static std::string AmbientOcclusionQualityFromSettings(const nlohmann::json& settings)
{
	const float strength = settings.value("ambientOcclusionStrength", 1.0f);
	if (strength <= 0.0f) return "disabled";
	return QualityFromRank(settings.value("ambientOcclusionQuality", 2u));
}

static bool IsModelPackageImporterName(const std::string& importer)
{
	return importer == "FbxPackageImporter" || importer == "GltfPackageImporter";
}

static bool IsCompiledVirtualGeometryMeshPath(const std::string& path)
{
	std::filesystem::path compiledPath(path);
	return compiledPath.extension() == ".temf" || compiledPath.extension() == ".TEMF";
}

static nlohmann::json BuildVirtualGeometryExportManifest(const BuildProfile& profile)
{
	nlohmann::json virtualGeometry;
	virtualGeometry["quality"] = QualityFromRank(profile.virtualGeometryQualityLevel);
	virtualGeometry["qualityLevel"] = profile.virtualGeometryQualityLevel;
	virtualGeometry["diagnosticsEnabled"] = profile.virtualGeometryDiagnosticsEnabled;
	virtualGeometry["geometryBudgetBytes"] = profile.virtualGeometryBudgetBytes;
	virtualGeometry["lodErrorScale"] = profile.virtualGeometryLodErrorScale;
	virtualGeometry["maxLodDrop"] = profile.virtualGeometryMaxLodDrop;
	virtualGeometry["hiZOcclusionEnabled"] = profile.virtualGeometryHiZOcclusionEnabled;
	virtualGeometry["preloadBias"] = profile.virtualGeometryPreloadBias;
	// Keep this declaration in lockstep with ExportPackageValidator so an
	// exported package advertises every TEMF metadata table required by the
	// virtual-geometry renderer before validation scans the packaged meshes.
	virtualGeometry["requiredMetadata"] = nlohmann::json::array({
		"adaptiveTargets",
		"fallbackCoverage",
		"clusterHierarchy",
		"pageLinks",
		"coneSafety"
	});
	// Export validation also records the fixture coverage assumptions that made
	// the current virtual-geometry package format safe enough to run. The
	// runtime does not load these fixtures; the fields are an audit trail for
	// developers and for package validation before the build is accepted.
	virtualGeometry["coverage"] = {
		{ "fixtureManifest", "Tests/Fixtures/VirtualGeometry/hardening-fixtures.json" },
		{ "requiredCategories", nlohmann::json::array({ "export", "streaming-pressure", "multi-view" }) }
	};
	virtualGeometry["requiredShaders"] = nlohmann::json::array({
		"Shaders/Default.task.tspf",
		"Shaders/Default.mesh.tspf",
		"Shaders/Default.frag.tspf",
		"Shaders/PostProcess/HiZGenerate.comp.spv"
	});
	return virtualGeometry;
}

static int ExecuteUtf8ShellCommand(const std::string& commandLine) {
#ifdef _WIN32
	const int wideLength = MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, commandLine.data(), static_cast<int>(commandLine.size()), nullptr, 0);
	if(wideLength <= 0) return -1;
	std::wstring wideCommand(static_cast<std::size_t>(wideLength), L'\0');
	if(MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, commandLine.data(), static_cast<int>(commandLine.size()),
		wideCommand.data(), wideLength) != wideLength) return -1;
	return _wsystem(wideCommand.c_str());
#else
	return std::system(commandLine.c_str());
#endif
}

static std::string QuotePosixShellValue(const std::string& value) {
	// Single quotes preserve spaces and shell metacharacters in the project and
	// installed-toolchain paths. POSIX shells represent a literal quote by
	// closing, escaping, and reopening the quoted value.
	std::string quoted = "'";
	for(const char character : value) {
		if(character == '\'') {
			quoted += "'";
			quoted += '\\';
			quoted += "'";
		}
		else quoted += character;
	}
	quoted += "'";
	return quoted;
}

#ifdef _WIN32
static std::string EscapeWindowsCommandExpansion(std::string_view value) {
	// cmd.exe expands %NAME% even inside quotes. Prefix each percent with a
	// caret so valid project/install paths containing percent signs remain
	// literal when setting environment variables or changing directories.
	std::string escaped;
	escaped.reserve(value.size());
	for(const char character : value) {
		if(character == '%') escaped += '^';
		escaped += character;
	}
	return escaped;
}
#endif

static int ExecuteNativeAotToolchainCommand(
	const std::string& commandLine,
	const NativeAotPublishContext& context)
{
	const std::string toolchainDirectory = PathToUtf8(context.workingDirectory);
	const std::string packageCache = PathToUtf8(context.packageCache);
	const std::string dotnetCliHome = PathToUtf8(context.dotnetCliHome);
#ifdef _WIN32
	// cmd.exe is already the _wsystem host. pushd scopes global.json discovery
	// to the installed toolchain and the set commands scope cache variables to
	// this child shell, leaving the editor and the user's profile untouched.
	const std::string wrapped = "pushd \"" + EscapeWindowsCommandExpansion(toolchainDirectory) + "\" && "
		"set \"NUGET_PACKAGES=" + EscapeWindowsCommandExpansion(packageCache) + "\" && "
		"set \"DOTNET_CLI_HOME=" + EscapeWindowsCommandExpansion(dotnetCliHome) + "\" && "
		"set \"DOTNET_CLI_TELEMETRY_OPTOUT=1\" && "
		"set \"DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1\" && "
		"set \"DOTNET_NOLOGO=1\" && "
		"set \"NUGET_XMLDOC_MODE=skip\" && "
		"set \"NUGET_CERT_REVOCATION_MODE=offline\" && "
		+ commandLine;
#else
	const std::string wrapped = "cd " + QuotePosixShellValue(toolchainDirectory) + " && "
		"NUGET_PACKAGES=" + QuotePosixShellValue(packageCache) + " "
		"DOTNET_CLI_HOME=" + QuotePosixShellValue(dotnetCliHome) + " "
		"DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 "
		"DOTNET_NOLOGO=1 NUGET_XMLDOC_MODE=skip "
		"NUGET_CERT_REVOCATION_MODE=offline " + commandLine;
#endif
	return ExecuteUtf8ShellCommand(wrapped);
}

static nlohmann::json BuildRendererRecoveryManifest(const BuildProfile& profile)
{
	nlohmann::json recovery;
	recovery["validationEnabled"] = profile.enableVulkanValidationLayers;
	recovery["diagnosticsEnabled"] = profile.rendererRecoveryDiagnosticsEnabled;
	recovery["mode"] = "failClosed";
	recovery["fatalMessage"] = profile.rendererRecoveryFatalMessage.empty()
		? "Graphics device lost. Restart the application to restore rendering."
		: profile.rendererRecoveryFatalMessage;
	recovery["fullApplicationRestart"] = false;
	return recovery;
}

static nlohmann::json ResolveRenderProfileManifest(const BuildProfile& profile, const std::filesystem::path& projectRoot)
{
	nlohmann::json renderProfile;
	renderProfile["profileName"] = "Project Render Settings";
	renderProfile["shadows"] = "high";
	renderProfile["ambientOcclusion"] = "medium";
	renderProfile["variableRateShading"] = "disabled";
	renderProfile["textureBudgetMb"] = 512u;
	renderProfile["resolutionScale"] = 1.0f;
	renderProfile["validationEnabled"] = profile.enableVulkanValidationLayers;
	renderProfile["debugEnabled"] = profile.enableRenderDebugDiagnostics;

	const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "RenderSettings.json";
	if (!std::filesystem::exists(settingsPath)) {
		return renderProfile;
	}

	try {
		std::ifstream file(settingsPath);
		nlohmann::json savedSettings;
		file >> savedSettings;
		if (auto settingsIt = savedSettings.find("settings"); settingsIt != savedSettings.end() && settingsIt->is_object()) {
			// Build profiles no longer own renderer quality. The export manifest
			// records a concise product-facing summary of the authoritative
			// project render settings so boot logs still show what was exported.
			renderProfile["shadows"] = ShadowQualityFromSettings(*settingsIt);
			renderProfile["ambientOcclusion"] = AmbientOcclusionQualityFromSettings(*settingsIt);
			renderProfile["textureBudgetMb"] = settingsIt->value("textureBudgetMb", 512u);
			renderProfile["resolutionScale"] = settingsIt->value("resolutionScale", 1.0f);
			renderProfile["variableRateShading"] = settingsIt->value("variableRateShading", 0u) > 0u ? "enabled" : "disabled";
		}
	}
	catch (...) {
		// A malformed optional settings file should not make export crash here;
		// validation can report richer diagnostics while the manifest remains
		// deterministic with documented defaults.
	}

	return renderProfile;
}

static std::optional<nlohmann::json> LoadProjectRenderSettingsManifest(const std::filesystem::path& projectRoot)
{
	const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "RenderSettings.json";
	if (!std::filesystem::exists(settingsPath)) {
		return std::nullopt;
	}

	try {
		std::ifstream file(settingsPath);
		if (!file) {
			return std::nullopt;
		}

		nlohmann::json savedSettings;
		file >> savedSettings;
		if (!savedSettings.is_object()) {
			return std::nullopt;
		}

		// Preserve the editor-authored render settings document exactly instead
		// of reducing it to profile labels. Export runtime can then apply the
		// same renderer contract that the project used in the editor.
		return savedSettings;
	}
	catch (...) {
		// Render settings remain optional for old projects. A malformed file
		// should be reported by validation, but manifest generation still needs
		// a deterministic fallback path so export does not crash here.
		return std::nullopt;
	}
}

static nlohmann::json BuildResidencyPolicyManifest()
{
	const TerraEngine::Resource::ResidencyPolicy profile = ResourceManager::Instance().GetResidencyPolicy();
	nlohmann::json policy;
	policy["enabled"] = profile.enabled;
	policy["cleanupIntervalSeconds"] = profile.cleanupIntervalSeconds;
	policy["cpu"] = {
		{ "idleSecondsThreshold", profile.cpu.idleSecondsThreshold },
		{ "memoryBudgetBytes", profile.cpu.memoryBudgetBytes },
		{ "reloadGraceSeconds", profile.cpu.reloadGraceSeconds }
	};
	policy["gpu"] = {
		{ "idleSecondsThreshold", profile.gpu.idleSecondsThreshold },
		{ "memoryBudgetBytes", profile.gpu.memoryBudgetBytes },
		{ "reloadGraceSeconds", profile.gpu.reloadGraceSeconds },
		{ "strictBudgetMode", profile.gpu.strictBudgetMode }
	};
	return policy;
}

static nlohmann::json BuildStreamingPolicyManifest()
{
	const TerraEngine::Resource::StreamingPolicy profile = ResourceManager::Instance().GetStreamingPolicy();
	nlohmann::json policy;
	policy["enabled"] = profile.enabled;
	policy["deferScenePayloadsOnLoad"] = profile.deferScenePayloadsOnLoad;
	policy["streamingDistance"] = profile.streamingDistance;
	policy["visibleRetentionSeconds"] = profile.visibleRetentionSeconds;
	policy["neededRetentionSeconds"] = profile.neededRetentionSeconds;
	policy["maxLoadsPerUpdate"] = profile.maxLoadsPerUpdate;
	policy["textureMipStreamingEnabled"] = profile.textureMipStreamingEnabled;
	policy["textureMinStreamMip"] = profile.textureMinStreamMip;
	policy["textureMaxStreamMip"] = profile.textureMaxStreamMip;
	policy["virtualGeometryPageMemoryBudgetBytes"] = profile.virtualGeometryPageMemoryBudgetBytes;
	policy["virtualGeometryPageUploadBudgetBytes"] = profile.virtualGeometryPageUploadBudgetBytes;
	policy["virtualGeometryPageRetirementBudgetBytes"] = profile.virtualGeometryPageRetirementBudgetBytes;
	policy["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(profile.memoryBudgetInteraction));
	return policy;
}

// CopyRuntimeShaderTree stages every engine-owned shader into the TEPAK input.
// Some renderer features still reference shaders from nested editor/history
// folders, so the export package keeps the complete installed shader layout.
static bool CopyRuntimeShaderTree(const std::filesystem::path& shadersSrc, const std::filesystem::path& shadersDest, std::string& outError) {
	if (!std::filesystem::exists(shadersSrc)) {
		outError = "Runtime shader directory not found: " + PathToUtf8(shadersSrc);
		return false;
	}

	RemoveDirectoryIfExists(shadersDest);
	std::filesystem::create_directories(shadersDest);
	for (const auto& entry : std::filesystem::recursive_directory_iterator(shadersSrc)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const auto relative = std::filesystem::relative(entry.path(), shadersSrc);
		const auto dest = shadersDest / relative;
		std::filesystem::create_directories(dest.parent_path());
		std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
	}

	return true;
}

// CopyRuntimeAssetTree stages an engine-owned runtime asset folder into the
// TEPAK input root. Fonts use this path so exported UI text does not depend on
// loose sidecar files or system-installed fonts.
static bool CopyRuntimeAssetTree(const std::filesystem::path& sourceRoot, const std::filesystem::path& destinationRoot, const std::string& description, std::string& outError) {
	if (!std::filesystem::exists(sourceRoot)) {
		outError = description + " directory not found: " + PathToUtf8(sourceRoot);
		return false;
	}

	RemoveDirectoryIfExists(destinationRoot);
	std::filesystem::create_directories(destinationRoot);
	for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceRoot)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const auto relative = std::filesystem::relative(entry.path(), sourceRoot);
		const auto dest = destinationRoot / relative;
		std::filesystem::create_directories(dest.parent_path());
		std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
	}

	return true;
}

// RemoveDebugSymbolFiles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static void RemoveDebugSymbolFiles(const std::filesystem::path& rootDirectory) {
	if (!std::filesystem::exists(rootDirectory)) {
		return;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& entry : std::filesystem::recursive_directory_iterator(rootDirectory)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		// Exports should not carry loose debug symbol payloads when the build
		// profile explicitly disables them. Remove every generated PDB in the
		// managed output so bridge/runtime symbols cannot leak into the package.
		if (entry.path().extension() == ".pdb") {
			RemoveIfExists(entry.path());
		}
	}
}

// struct ProjectManifestMetadata groups related state and operations so callers have one documented contract.
struct ProjectManifestMetadata {
	std::string name = "TerraEngine";
	std::string version = "1.0.0";
};

// LoadProjectManifestMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
static ProjectManifestMetadata LoadProjectManifestMetadata(const std::filesystem::path& projectRoot) {
	ProjectManifestMetadata metadata;
	if (!projectRoot.filename().empty()) {
		metadata.name = PathToUtf8(projectRoot.filename());
	}

	const std::filesystem::path manifestPath = projectRoot / "game.json";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(manifestPath)) {
		return metadata;
	}

	try {
		std::ifstream input(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!input) {
			return metadata;
		}

		const nlohmann::json manifest = nlohmann::json::parse(input);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string()) {
			const std::string manifestName = nameIt->get<std::string>();
			if (!manifestName.empty()) {
				metadata.name = manifestName;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto versionIt = manifest.find("version"); versionIt != manifest.end() && versionIt->is_string()) {
			const std::string manifestVersion = versionIt->get<std::string>();
			if (!manifestVersion.empty()) {
				metadata.version = manifestVersion;
			}
		}
	}
	catch (...) {
		// Export validation already reports richer context elsewhere. Falling back
		// here keeps manifest generation resilient even if metadata is malformed.
	}

	return metadata;
}

// --- Public API ---

ExportResult ExportPipeline::Execute(
	const ExportRequest& request,
	ExportProgressCallback progressCallback)
{
	const auto startTime = std::chrono::steady_clock::now();
	ExportResult result;
	result.runId = request.runId;
	result.invocationKind = request.invocationKind;
	result.profileId = request.profile.id;
	result.projectRoot = request.projectRoot;
	result.outputDirectory = request.finalOutputDirectory;
	result.evidenceDirectory = request.evidenceDirectory;
	m_StagedAssets.clear();
	m_PhysicsManifest = PhysicsExportManifest{};

	std::vector<std::string> progressRecords;
	ExportProgressCallback capturedProgress = [&](const ExportProgress& progress) {
		progressRecords.push_back(SerializeExportProgressRecord(request.runId, progress));
		if(progressCallback) progressCallback(progress);
	};
	std::string resolvedManifest;
	std::vector<ExportContentInventoryEntry> inventory;
	std::string packageValidationReport;
	ExportOutputTransaction transaction(request.finalOutputDirectory, request.runId);

	auto writeEvidence = [&](std::string& outError) {
		ExportEvidenceInputs evidenceInputs;
		evidenceInputs.resolvedManifestJson = resolvedManifest;
		evidenceInputs.inventory = inventory;
		evidenceInputs.progressJsonLines = progressRecords;
		evidenceInputs.packageValidationReport = packageValidationReport;
		return WriteExportEvidenceBundle(request, result, evidenceInputs, outError);
	};

	auto finish = [&](ExportFailureCategory category, ExportStage stage, std::string message) {
		result.success = category == ExportFailureCategory::None;
		result.failureCategory = category;
		result.stage = stage;
		result.exitCode = ExitCodeForFailure(category);
		result.message = std::move(message);
		result.errorMessage = result.success ? std::string{} : result.message;
		std::vector<ExportDiagnostic> stageDiagnostics = std::move(result.diagnostics);
		result.diagnostics = result.validationResult.diagnostics;
		result.diagnostics.insert(result.diagnostics.end(),
			std::make_move_iterator(stageDiagnostics.begin()),
			std::make_move_iterator(stageDiagnostics.end()));
		if(category != ExportFailureCategory::None) {
			ExportDiagnostic diagnostic;
			diagnostic.severity = ExportDiagnosticSeverity::Error;
			diagnostic.category = std::string(ToString(category));
			diagnostic.stage = std::string(ToString(stage));
			diagnostic.code = "export." + std::string(ToString(category));
			diagnostic.message = result.message;
			diagnostic.filePath = PathToUtf8(result.affectedPath);
			result.diagnostics.push_back(std::move(diagnostic));
		}
		result.totalElapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();

		if(!result.success) {
			std::string rollbackError;
			if(!transaction.Rollback(rollbackError)) {
				result.failureCategory = ExportFailureCategory::PublicationFailed;
				result.stage = ExportStage::Publication;
				result.exitCode = ExitCodeForFailure(result.failureCategory);
				result.message += " Rollback failed: " + rollbackError;
				result.errorMessage = result.message;
			}
		}

		std::string evidenceError;
		if(!writeEvidence(evidenceError)) {
			result.success = false;
			result.failureCategory = ExportFailureCategory::EvidenceWriteFailed;
			result.stage = ExportStage::Evidence;
			result.exitCode = ExitCodeForFailure(result.failureCategory);
			result.message = evidenceError;
			result.errorMessage = evidenceError;
			// A second best-effort write updates export-result.json when the first
			// attempt failed after creating only part of the evidence directory.
			std::string ignoredError;
			writeEvidence(ignoredError);
		}
		return result;
	};

	try {
		std::string transactionError;
		if(!transaction.Begin(transactionError)) {
			result.affectedPath = transaction.LockPath();
			return finish(ExportFailureCategory::OutputConflict, ExportStage::Lock, transactionError);
		}

		std::string runtimeError;
		if(!ValidateExportRuntimeLayout(request.runtimeLayout, request.profile.platform, runtimeError)) {
			result.affectedPath = request.runtimeLayout.installRoot;
			return finish(ExportFailureCategory::InvalidRuntimeLayout, ExportStage::Arguments, runtimeError);
		}

		BuildProfile executionProfile = request.profile;
		executionProfile.outputDirectory = transaction.StagingDirectory();
		const std::filesystem::path packageInputDirectory = transaction.ScratchDirectory() / "PackageInput";

		result.stage = ExportStage::Validation;
		if(!RunValidation(executionProfile, request.projectRoot, request.runtimeLayout, result, capturedProgress)) {
			result.affectedPath = request.profilePath.empty() ? request.projectRoot : request.profilePath;
			return finish(ExportFailureCategory::ValidationFailed, result.stage, result.errorMessage);
		}

		if(executionProfile.enableManagedCompilation) {
			result.stage = ExportStage::ManagedCompilation;
			if(!RunManagedCompilation(executionProfile, request.projectRoot,
				request.runtimeLayout, result, capturedProgress)) {
				result.affectedPath = request.projectRoot / "Assembly-CSharp.csproj";
				return finish(ExportFailureCategory::ManagedCompilationFailed, result.stage, result.errorMessage);
			}
		}

		result.stage = ExportStage::AssetStaging;
		if(!RunAssetStaging(executionProfile, request.projectRoot, request.runtimeLayout,
			packageInputDirectory, result, capturedProgress)) {
			result.affectedPath = packageInputDirectory;
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage, result.errorMessage);
		}

		const std::filesystem::path manifestPath = packageInputDirectory / "game.json";
		{
			// Close the manifest handle before Commit removes the scratch tree on
			// Windows; an open stream would otherwise leave an orphan directory.
			std::ifstream manifestInput(manifestPath, std::ios::binary);
			resolvedManifest.assign(
				std::istreambuf_iterator<char>(manifestInput), std::istreambuf_iterator<char>());
		}
		std::string inventoryError;
		if(!BuildExportContentInventory(packageInputDirectory, inventory, inventoryError)) {
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage, inventoryError);
		}
		const auto placeholderEntry = std::find_if(inventory.begin(), inventory.end(), [](const auto& entry) {
			return entry.required && entry.placeholderDetected;
		});
		if(placeholderEntry != inventory.end()) {
			return finish(ExportFailureCategory::AssetStagingFailed, result.stage,
				"Required staged entry contains a placeholder marker: " + placeholderEntry->logicalPath);
		}

		result.stage = ExportStage::Packaging;
		if(!RunAssetPackaging(executionProfile, request.projectRoot, packageInputDirectory, result, capturedProgress)) {
			result.affectedPath = transaction.StagingDirectory();
			return finish(ExportFailureCategory::PackagingFailed, result.stage, result.errorMessage);
		}

		result.stage = ExportStage::PackageValidation;
		ExportPackageValidationReport inspection = InspectExportPackage(
			result.packageResult.outputPath, executionProfile.platform);
		const bool manifestMatchesEvidence = inspection.archivedManifestJson == resolvedManifest;
		inspection.passed = inspection.passed && manifestMatchesEvidence;
		const std::size_t finalLine = inspection.summaryText.rfind("Final: ");
		if(finalLine != std::string::npos) inspection.summaryText.erase(finalLine);
		inspection.summaryText += std::string("Manifest evidence match: ")
			+ (manifestMatchesEvidence ? "PASS\n" : "FAIL\n")
			+ "Final: " + (inspection.passed ? "PASS\n" : "FAIL\n");
		packageValidationReport = inspection.summaryText;
		if(!inspection.passed || !manifestMatchesEvidence) {
			result.affectedPath = result.packageResult.outputPath;
			for(const ExportPackageValidationFinding& finding : inspection.findings) {
				ExportDiagnostic diagnostic;
				diagnostic.severity = ExportDiagnosticSeverity::Error;
				diagnostic.category = "Package";
				diagnostic.stage = "package_validation";
				diagnostic.code = finding.code;
				diagnostic.message = finding.message;
				diagnostic.filePath = finding.path;
				result.diagnostics.push_back(std::move(diagnostic));
			}
			const std::string error = inspection.findings.empty()
				? "Export package manifest does not match resolved-manifest evidence."
				: inspection.findings.front().message;
			return finish(ExportFailureCategory::PackageValidationFailed, result.stage, error);
		}

		// Deep animation validation reopens the packaged archive and reconstructs
		// the exact bytes consumers will mount. It verifies append-only TEPAK class
		// codes, hashes, source-free manifests, dependency closure, and headless/CPU
		// fallback capability before the output transaction may publish anything.
		const TerraEngine::Export::AnimationPackValidationReport animationInspection =
			TerraEngine::Export::AnimationPackValidator::ValidateArchive(result.packageResult.outputPath);
		std::string animationEvidenceError;
		if(!TerraEngine::Export::AnimationPackValidator::WriteEvidence(
			animationInspection,
			request.evidenceDirectory / "animation-validation.json",
			animationEvidenceError)) {
			result.affectedPath = request.evidenceDirectory / "animation-validation.json";
			return finish(ExportFailureCategory::EvidenceWriteFailed, ExportStage::Evidence, animationEvidenceError);
		}
		packageValidationReport += "Animation archive validation: "
			+ std::string(animationInspection.accepted ? "PASS\n" : "FAIL\n");
		if(!animationInspection.accepted) {
			for(const TerraEngine::Export::AnimationPackEvidence& evidence : animationInspection.evidence) {
				if(evidence.passed) continue;
				ExportDiagnostic diagnostic;
				diagnostic.severity = ExportDiagnosticSeverity::Error;
				diagnostic.category = "Animation";
				diagnostic.stage = "package_validation";
				diagnostic.code = evidence.rule;
				diagnostic.message = evidence.message;
				diagnostic.filePath = evidence.path;
				result.diagnostics.push_back(std::move(diagnostic));
			}
			result.affectedPath = result.packageResult.outputPath;
			const auto failedEvidence = std::find_if(
				animationInspection.evidence.begin(),
				animationInspection.evidence.end(),
				[](const TerraEngine::Export::AnimationPackEvidence& evidence) { return !evidence.passed; });
			return finish(
				ExportFailureCategory::PackageValidationFailed,
				result.stage,
				failedEvidence == animationInspection.evidence.end()
					? "Animation archive validation failed without a diagnostic."
					: failedEvidence->message);
		}

		result.stage = ExportStage::RuntimeStaging;
		if(!RunOutputStaging(executionProfile, request.projectRoot, request.runtimeLayout, result, capturedProgress)) {
			result.affectedPath = transaction.StagingDirectory();
			return finish(ExportFailureCategory::RuntimeStagingFailed, result.stage, result.errorMessage);
		}

		std::error_code scratchCleanupError;
		std::filesystem::remove_all(transaction.ScratchDirectory(), scratchCleanupError);
		if(scratchCleanupError) {
			result.affectedPath = transaction.ScratchDirectory();
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication,
				"Could not clean validated export scratch data: " + scratchCleanupError.message());
		}
		if(!transaction.MarkStagedAndValidated(transactionError)) {
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication, transactionError);
		}
		const std::filesystem::path stagedArchivePath = result.packageResult.outputPath;
		const std::filesystem::path archiveFileName = stagedArchivePath.filename();
		result.packageResult.outputPath = request.finalOutputDirectory / archiveFileName;
		const std::string stagedArchiveText = PathToUtf8(stagedArchivePath);
		const std::size_t archiveSummaryOffset = packageValidationReport.find(stagedArchiveText);
		if(archiveSummaryOffset != std::string::npos) {
			packageValidationReport.replace(
				archiveSummaryOffset, stagedArchiveText.size(), PathToUtf8(result.packageResult.outputPath));
		}
		result.outputDirectory = request.finalOutputDirectory;
		result.success = true;
		result.failureCategory = ExportFailureCategory::None;
		result.stage = ExportStage::Complete;
		result.exitCode = 0;
		result.message = "Export completed successfully.";
		result.errorMessage.clear();
		result.diagnostics = result.validationResult.diagnostics;
		result.totalElapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();
		ExportProgress completeProgress;
		completeProgress.phase = ExportPhase::Complete;
		completeProgress.stage = ExportStage::Complete;
		completeProgress.overallProgress = 1.0f;
		completeProgress.statusMessage = result.message;
		completeProgress.finished = true;
		// Store the eventual stdout record before evidence publication, while the
		// external callback remains deferred until the output swap succeeds.
		progressRecords.push_back(SerializeExportProgressRecord(request.runId, completeProgress));
		std::string evidenceError;
		if(!writeEvidence(evidenceError)) {
			result.affectedPath = request.evidenceDirectory;
			return finish(ExportFailureCategory::EvidenceWriteFailed, ExportStage::Evidence, evidenceError);
		}
		if(!transaction.Commit(transactionError)) {
			result.affectedPath = request.finalOutputDirectory;
			return finish(ExportFailureCategory::PublicationFailed, ExportStage::Publication, transactionError);
		}
		// The evidence bundle is already durable. Publish the completion callback
		// only after the validated directory swap succeeds, without reopening the
		// evidence files after the protected boundary.
		if(progressCallback) {
			try {
				progressCallback(completeProgress);
			} catch(...) {
				// Publication has completed and must never be reclassified as a
				// failure because a presentation-only callback threw an exception.
			}
		}
		return result;
	} catch(const std::exception& exception) {
		return finish(ExportFailureCategory::UnexpectedFailure, result.stage,
			std::string("Contained export failure: ") + exception.what());
	} catch(...) {
		return finish(ExportFailureCategory::UnexpectedFailure, result.stage,
			"Contained export failure with no diagnostic text.");
	}
}

static bool ValidateManagedRuntimeOutput(
	const std::filesystem::path& managedLibrary,
	std::string& outError)
{
	std::error_code errorCode;
	if(!std::filesystem::is_regular_file(managedLibrary, errorCode)) {
		outError = "NativeAOT publish did not produce the required gameplay library: " + PathToUtf8(managedLibrary);
		return false;
	}
	const std::uintmax_t byteSize = std::filesystem::file_size(managedLibrary, errorCode);
	if(errorCode || byteSize == 0) {
		outError = "NativeAOT gameplay library is empty or unreadable: " + PathToUtf8(managedLibrary);
		return false;
	}

	std::ifstream input(managedLibrary, std::ios::binary);
	const std::vector<unsigned char> bytes{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	static constexpr std::string_view marker = "placeholder";
	const bool containsPlaceholder = std::search(
		bytes.begin(), bytes.end(), marker.begin(), marker.end(),
		[](unsigned char left, char right) {
			return static_cast<char>(std::tolower(left)) == right;
		}) != bytes.end();
	if(containsPlaceholder) {
		outError = "NativeAOT gameplay library contains the forbidden placeholder marker: " + PathToUtf8(managedLibrary);
		return false;
	}
	return true;
}

// --- Phase implementations ---

bool ExportPipeline::RunValidation(const BuildProfile& profile, const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout, ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Validating, 0.0f,
		"Validating build profile...");

	result.validationResult = m_Validator.ValidateAll(profile, projectRoot, runtimeLayout.shaderDirectory);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.validationResult.passed) {
		result.errorMessage = "Pre-export validation failed with " +
			std::to_string(result.validationResult.errorCount) + " error(s).";
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Validating, 0.1f,
		"Validation passed.");
	return true;
}

bool ExportPipeline::RunManagedCompilation(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Compiling, 0.15f,
		"Compiling managed project with NativeAOT...");

	// The authored managed project is strictly required to be at the project root.
	// NativeAOT may later access it through a short alias, but validation always
	// reports the real user-owned path.
	const std::filesystem::path sourceManagedProject =
		projectRoot / "Assembly-CSharp.csproj";

	if (!std::filesystem::exists(sourceManagedProject)) {
		result.errorMessage = "Managed project not found: "
			+ PathToUtf8(sourceManagedProject);
		return false;
	}

	if (!IsExportPlatformNativeToHost(profile.platform)) {
		result.errorMessage = "Cross-OS NativeAOT export is not supported by dotnet. This host can publish " +
			GetExportRuntimeIdentifier(GetHostExportPlatform()) + "; the selected profile targets " +
			GetExportRuntimeIdentifier(profile.platform) + ".";
		return false;
	}

	NativeAotProjectWorkspace nativeAotWorkspace(projectRoot);
	std::string workspaceError;
	if(!nativeAotWorkspace.Prepare(workspaceError)) {
		result.errorMessage = workspaceError;
		return false;
	}
	const std::filesystem::path& buildProjectRoot =
		nativeAotWorkspace.BuildProjectRoot();
	const std::filesystem::path managedProject =
		buildProjectRoot / "Assembly-CSharp.csproj";
	std::error_code equivalentError;
	if(!std::filesystem::equivalent(
			managedProject, sourceManagedProject, equivalentError)
		|| equivalentError) {
		result.errorMessage = "The private NativeAOT project path does not "
			"resolve to the authored managed project";
		if(equivalentError) {
			result.errorMessage += ": " + equivalentError.message();
		}
		return false;
	}

	// Resolve the output directory for the NativeAOT publish output.
	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	std::filesystem::create_directories(outputDir);
	const std::filesystem::path managedOutputDir = outputDir / "Managed";
	// All SDK, compiler, linker, package, and publish output is private to this
	// operation. This prevents concurrent exports from sharing assets files and
	// keeps long project/output paths away from NativeAOT child processes.
	const std::filesystem::path& nativeAotPublishOutputDir =
		nativeAotWorkspace.PublishDirectory();
	const NativeAotPublishContext nativeAotContext{
		.workingDirectory = runtimeLayout.nativeAotToolchainDirectory,
		.nugetConfig = runtimeLayout.nativeAotNugetConfig,
		.nugetFeed = runtimeLayout.nativeAotNugetFeed,
		.packageCache = nativeAotWorkspace.PackageCacheDirectory(),
		.dotnetCliHome = nativeAotWorkspace.DotnetCliHomeDirectory(),
		.intermediateOutputDirectory =
			nativeAotWorkspace.IntermediateOutputDirectory(),
		.binaryOutputDirectory =
			nativeAotWorkspace.BinaryOutputDirectory()
	};

	// NativeAOT publish does not clean removed files from a previous export.
	// Clear the export Managed directory first so stale bridge/runtime payloads
	// such as TerraEngine.CSharp.dll cannot survive after the export pipeline
	// stops copying them.
	RemoveDirectoryIfExists(managedOutputDir);

	// Old projects may still contain interop sources that conflict with the source
	// generator. Export reports the problem but never deletes project source; the
	// user remains in control of their checked-in content.
	std::filesystem::path obsoleteExportDir = projectRoot / "Project" / "Internal" / "Export";
	if (std::filesystem::exists(obsoleteExportDir)) {
		result.errorMessage = "Obsolete 'Project/Internal/Export' source exists. Remove it manually before exporting so the source generator has one interop definition.";
		return false;
	}

	// Restore first with PublishAot=true so all SDK-selected compiler packages
	// come only from the installed feed. Publish then consumes that exact assets
	// file with --no-restore and cannot silently contact a user or network source.
	const int restoreExitCode = ExecuteNativeAotToolchainCommand(
		BuildNativeAotRestoreCommand(profile, managedProject, nativeAotContext) + " 1>&2",
		nativeAotContext);
	if(restoreExitCode != 0) {
		result.errorMessage = "NativeAOT offline restore failed (exit code "
			+ std::to_string(restoreExitCode) + ").";
		return false;
	}

	// The CLI reserves stdout for one-JSON-object-per-line automation records.
	// Both cmd.exe and POSIX shells accept this redirection, so verbose MSBuild
	// output remains human-readable on stderr without corrupting JSONL output.
	const int publishExitCode = ExecuteNativeAotToolchainCommand(
		BuildNativeAotPublishCommand(
			profile, managedProject, nativeAotPublishOutputDir, nativeAotContext) + " 1>&2",
		nativeAotContext);
	if(publishExitCode != 0) {
		result.errorMessage = "NativeAOT compilation failed (exit code " +
			std::to_string(publishExitCode) + ").";
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!profile.includeDebugSymbols) {
		RemoveDebugSymbolFiles(nativeAotPublishOutputDir);
	}
	std::string managedOutputError;
	if(!ValidateManagedRuntimeOutput(
		nativeAotPublishOutputDir / GetExportManagedRuntimeLibraryName(profile.platform),
		managedOutputError)) {
		result.errorMessage = managedOutputError;
		return false;
	}

	std::error_code copyError;
	std::filesystem::copy(
		nativeAotPublishOutputDir,
		managedOutputDir,
		std::filesystem::copy_options::recursive
			| std::filesystem::copy_options::overwrite_existing,
		copyError);
	if(copyError) {
		result.errorMessage = "Could not publish validated NativeAOT output "
			"from the private workspace: " + copyError.message();
		return false;
	}
	if(!ValidateManagedRuntimeOutput(
			managedOutputDir / GetExportManagedRuntimeLibraryName(profile.platform),
			managedOutputError)) {
		result.errorMessage = managedOutputError;
		return false;
	}

	// Successful operations report cleanup failures instead of silently leaking
	// a junction or per-run compiler state. Early failures use the same cleanup
	// path through the workspace destructor while preserving their primary error.
	if(!nativeAotWorkspace.Cleanup(workspaceError)) {
		result.errorMessage = "Managed output was validated, but the private "
			"NativeAOT workspace could not be cleaned: " + workspaceError;
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Compiling, 0.45f,
		"Managed compilation completed.");
	return true;
}

bool ExportPipeline::RunAssetStaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout,
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::AssetStaging, 0.45f,
		"Staging assets for packaging...");

	try {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(stagingDir)) {
			std::filesystem::remove_all(stagingDir);
		}
		std::filesystem::create_directories(stagingDir);

		const auto rawAssets = projectRoot / "Assets";
		const auto compiledAssets = projectRoot / "Build" / "Assets";

		// 1. Copy engine bootstrap sources for NativeAOT.
		// These must be included in the user's project so that the InitManagedRuntime
		// entry point is exported from the gameplay DLL.
		std::filesystem::path engineManagedSrc = projectRoot / ".." / "Engine" / "Managed" / "Source" / "Export";
		// If we are in a sub-repo or different structure, we might need a better way to find this.
		// For now, assume standard dev layout. 
		// Actually, we can just copy them from the project's source if they were added there,
		// but they aren't by default. We'll copy them to the staging area so they are compiled.
		// Wait, NativeAOT compiles the CSProj. The CSProj needs to reference these files.
		// If the user's project doesn't have them, NativeAOT won't export the symbols.
		// The correct fix is ensuring the user's CSProj includes them or they are copied into the project.
		
		// 2. Copy raw assets (baseline).
		if (std::filesystem::exists(rawAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(rawAssets)) {
				if (!entry.is_regular_file()) continue;

				const std::filesystem::path relative = std::filesystem::relative(entry.path(), rawAssets);
				// Exclude .meta files from the exported build.
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() == ".meta") continue;
				// Collider recipes are editor/compiler input. A shipped runtime may
				// consume only the validated .tepcoll envelope referenced below.
				if(entry.path().extension() == ".tecollider") continue;
				// Raw model packages and generated import sidecars contain editor and
				// source-machine paths. Only their checked cooked children and sanitized
				// runtime manifests may cross the TEPAK publication boundary.
				if(TerraEngine::Export::IsForbiddenAnimationRuntimePath(relative.generic_string())) continue;

				auto dest = stagingDir / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
		}

		// 2. Overlay compiled assets (priority).
		// This replaces raw source (like .png) with engine-ready counterparts (like .tetf)
		// while leaving raw files that don't need compilation (like .tescene) intact.
		if (std::filesystem::exists(compiledAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(compiledAssets)) {
				if (!entry.is_regular_file()) continue;
				// A malformed or stale build directory must not reintroduce authoring
				// recipes after the raw-asset pass deliberately excluded them. The
				// standalone runtime has only cooked collider support, so it must never
				// receive source geometry that would require the editor cooker.
				if(entry.path().extension() == ".tecollider") continue;

				auto relative = std::filesystem::relative(entry.path(), compiledAssets);
				if(TerraEngine::Export::IsForbiddenAnimationRuntimePath(relative.generic_string())) continue;
				auto dest = stagingDir / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
		}

		// Resolve Physics Settings into the package instead of relying on an
		// editor-local ProjectSettings path. Older projects receive an explicit
		// validated default document, keeping runtime behavior deterministic.
		TerraEngine::Physics::PhysicsSettings physicsSettings;
		nlohmann::json resolvedPhysicsSettings;
		std::string physicsSettingsError;
		if(!LoadValidatedPhysicsSettings(projectRoot / "ProjectSettings" / "PhysicsSettings.json",
			physicsSettings, resolvedPhysicsSettings, physicsSettingsError)) {
			result.errorMessage = physicsSettingsError;
			return false;
		}
		const std::string physicsSettingsText = resolvedPhysicsSettings.dump(2);
		const std::filesystem::path stagedPhysicsSettingsPath = stagingDir / "ProjectSettings" / "PhysicsSettings.json";
		std::filesystem::create_directories(stagedPhysicsSettingsPath.parent_path());
		{
			std::ofstream output(stagedPhysicsSettingsPath, std::ios::binary | std::ios::trunc);
			if(!output) { result.errorMessage = "Could not stage resolved Physics Settings."; return false; }
			output << physicsSettingsText;
			if(!output.good()) { result.errorMessage = "Could not finish staging resolved Physics Settings."; return false; }
		}
		std::vector<std::byte> physicsSettingsBytes;
		if(!ReadExportBytes(stagedPhysicsSettingsPath, physicsSettingsBytes)) {
			result.errorMessage = "Could not read staged Physics Settings for hashing.";
			return false;
		}
		m_PhysicsManifest.settingsPackagePath = "ProjectSettings/PhysicsSettings.json";
		m_PhysicsManifest.settingsSchemaVersion = physicsSettings.schemaVersion;
		m_PhysicsManifest.settingsHash = HashBytesForExport(physicsSettingsBytes);
		const nlohmann::json layersOnly = resolvedPhysicsSettings.at("layers");
		const std::string layersText = layersOnly.dump();
		std::vector<std::byte> layersBytes(layersText.size());
		for(std::size_t index = 0U; index < layersText.size(); ++index) layersBytes[index] = static_cast<std::byte>(static_cast<unsigned char>(layersText[index]));
		m_PhysicsManifest.layersHash = HashBytesForExport(layersBytes);

		// Engine-owned runtime payloads are packed with project content so the
		// standalone launcher can resolve them entirely through ContentMount.
		std::string runtimeAssetError;
		if (!CopyRuntimeShaderTree(runtimeLayout.shaderDirectory, stagingDir / "Shaders", runtimeAssetError)) {
			result.errorMessage = runtimeAssetError;
			return false;
		}
		if (!CopyRuntimeAssetTree(runtimeLayout.fontDirectory, stagingDir / "Fonts", "Runtime font", runtimeAssetError)) {
			result.errorMessage = runtimeAssetError;
			return false;
		}

		// Rewrite only the staged scene and prefab copies so export runtime
		// assets resolve script bindings through deterministic numeric IDs.
		std::string rewriteError;
		if (!RewriteStagedScriptBindings(stagingDir, rewriteError)) {
			result.errorMessage = rewriteError;
			return false;
		}

		// 3. Scan metadata to build the GUID map for the export manifest.
		// In an exported build, the engine uses this map to resolve the UUIDs found
		// in scene/prefab files back to the logical paths inside the TEPAK archive.
		std::unordered_set<std::string> stagedAssetGuids;
		std::string stagedAssetIdentityError;
		auto readStagedAssetEvidence = [&stagingDir](
			const std::string& packagePath,
			std::string& checksum,
			std::uint64_t& byteSize) -> bool {
			std::vector<std::byte> bytes;
			if(!ReadExportBytes(stagingDir / std::filesystem::path(packagePath), bytes)) {
				return false;
			}
			checksum = HashBytesForExport(bytes);
			byteSize = static_cast<std::uint64_t>(bytes.size());
			return true;
		};
		auto addStagedAsset = [this, &stagedAssetGuids, &stagedAssetIdentityError, &readStagedAssetEvidence](
			std::string guid,
			AssetType type,
			const std::filesystem::path& path,
			std::string checksum,
			std::uint64_t byteSize) -> bool {
			if (guid.empty() || path.empty()) {
				return false;
			}
			const std::string packagePath = path.generic_string();
			const std::string normalizedPath = tepak::io::NormalizeLookupPath(packagePath);
			if(normalizedPath.empty()) {
				return false;
			}
			const auto existing = std::find_if(m_StagedAssets.begin(), m_StagedAssets.end(), [&guid](const StagedAsset& asset) {
				return asset.guid == guid;
			});
			if(existing != m_StagedAssets.end()) {
				// Metadata, generated-package sidecars, and cooked-envelope discovery
				// may independently prove one runtime identity. Compare the TEPAK
				// lookup key rather than host casing, then require byte evidence before
				// coalescing so a reused GUID cannot conceal a different payload.
				if(existing->type != type
					|| tepak::io::NormalizeLookupPath(existing->path) != normalizedPath) {
					stagedAssetIdentityError =
						"Staged asset GUID resolves to a different runtime type or canonical path: "
						+ guid;
					return false;
				}
				if(existing->checksum.empty()
					&& !readStagedAssetEvidence(
						existing->path,
						existing->checksum,
						existing->byteSize)) {
					stagedAssetIdentityError =
						"Could not verify existing staged content for duplicate asset GUID: "
						+ guid;
					return false;
				}
				if(checksum.empty()) {
					if(packagePath == existing->path) {
						// An identical physical spelling names the same staged file, so
						// reuse the evidence just read instead of buffering it twice.
						checksum = existing->checksum;
						byteSize = existing->byteSize;
					}
					else if(!readStagedAssetEvidence(packagePath, checksum, byteSize)) {
						stagedAssetIdentityError =
							"Could not verify candidate staged content for duplicate asset GUID: "
							+ guid;
						return false;
					}
				}
				if(existing->checksum != checksum || existing->byteSize != byteSize) {
					stagedAssetIdentityError =
						"Staged asset GUID resolves to different content at the same canonical path: "
						+ guid;
					return false;
				}
				return true;
			}
			if (!stagedAssetGuids.insert(guid).second) {
				stagedAssetIdentityError =
					"Staged asset GUID could not be inserted exactly once: " + guid;
				return false;
			}

			StagedAsset asset;
			asset.guid = std::move(guid);
			// Retain the authored spelling for backwards-compatible game.json output.
			// Duplicate comparison always uses the canonical TEPAK lookup form above.
			asset.path = packagePath;
			asset.type = type;
			asset.checksum = std::move(checksum);
			asset.byteSize = byteSize;
			asset.virtualGeometryRequired = asset.type == AssetType::Mesh
				&& IsCompiledVirtualGeometryMeshPath(asset.path);
			m_StagedAssets.push_back(std::move(asset));
			return true;
		};
		auto addValidatedPhysicsAsset = [this, &addStagedAsset, &stagingDir, &stagedAssetIdentityError](
			const std::string& guid, AssetType type, const std::filesystem::path& packagePath, std::string& error) -> bool {
			const std::filesystem::path stagedPath = stagingDir / packagePath;
			std::vector<std::byte> bytes;
			if(!ReadExportBytes(stagedPath, bytes)) {
				error = "Validated physics output is missing from export staging: " + PathToUtf8(packagePath);
				return false;
			}

			PhysicsExportAssetRecord record;
			record.guid = guid;
			record.packagePath = packagePath.generic_string();
			record.byteSize = bytes.size();
			record.checksum = HashBytesForExport(bytes);
			const std::optional<TerraEngine::UUID> parsedGuid = TerraEngine::ParseUUID(guid);
			if(!parsedGuid) { error = "Physics asset metadata has an invalid GUID: " + guid; return false; }

			if(type == AssetType::PhysicsMaterial) {
				try {
					const nlohmann::json document = nlohmann::json::parse(bytes.begin(), bytes.end());
					TerraEngine::Physics::PhysicsMaterial material;
					TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
					if(!TerraEngine::Physics::PhysicsMaterial::LoadJson(document, material, diagnostics)
						|| material.assetId != *parsedGuid) {
						error = "Physics material is invalid or does not match its metadata GUID: " + PathToUtf8(packagePath);
						return false;
					}
				}
				catch(const std::exception&) {
					error = "Physics material could not be parsed: " + PathToUtf8(packagePath);
					return false;
				}
				if(!addStagedAsset(guid, type, packagePath, record.checksum, record.byteSize)) {
					error = stagedAssetIdentityError.empty()
						? "Physics material GUID is duplicated in export staging: " + guid
						: stagedAssetIdentityError;
					return false;
				}
				m_PhysicsManifest.materials.push_back(std::move(record));
				return true;
			}

			TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
			const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(bytes, diagnostics);
			if(!collider || collider->Envelope().assetId != *parsedGuid
				|| !collider->CheckCompatibility(TerraEngine::Physics::CookedColliderAsset::HostPlatform(),
					TerraEngine::Physics::CookedColliderProviderFamily::Jolt,
					TerraEngine::Physics::kJoltCookProviderVersion, TerraEngine::Physics::kJoltCookRevision).compatible) {
				error = "Cooked collider is invalid, incompatible, or does not match its metadata GUID: " + PathToUtf8(packagePath);
				return false;
			}
			const auto& envelope = collider->Envelope();
			record.sourceProvenanceHash = HashPhysicsValue(envelope.sourceContentHash);
			record.recipeHash = HashPhysicsValue(envelope.recipeHash);
			if(!addStagedAsset(guid, type, packagePath, record.checksum, record.byteSize)) {
				error = stagedAssetIdentityError.empty()
					? "Cooked collider GUID is duplicated in export staging: " + guid
					: stagedAssetIdentityError;
				return false;
			}
			m_PhysicsManifest.colliders.push_back(std::move(record));
			return true;
		};
		auto resolveSourceAssetPath = [&rawAssets](const std::filesystem::path& metaPath, const std::string& sourcePathText) {
			if (!sourcePathText.empty()) {
				const std::filesystem::path sourcePath(sourcePathText);
				return sourcePath.is_absolute() ? sourcePath : rawAssets / sourcePath;
			}

			std::filesystem::path inferredSourcePath = metaPath;
			inferredSourcePath.replace_extension();
			return inferredSourcePath;
		};
		auto stageModelPackageChildren = [&addStagedAsset](const std::filesystem::path& sourceAssetPath) -> bool {
			TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
			if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, importResult)
				|| importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed) {
				return false;
			}

			bool addedAny = false;
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes) {
				// Package parents are containers; only generated child TEMFs are
				// renderable at runtime. Skip stale failed child cooks so the
				// manifest never references missing or unusable package output.
				if (mesh.compiledPath.empty()
					|| mesh.virtualGeometryStatus == "Failed"
					|| mesh.virtualGeometryStatus == "NeedsRebuild") {
					continue;
				}
				addedAny = addStagedAsset(
					TerraEngine::ToHexString(mesh.guid),
					AssetType::Mesh,
					mesh.compiledPath,
					{},
					0U) || addedAny;
			}
			for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials) {
				if (!material.compiledPath.empty()) {
					addedAny = addStagedAsset(
						TerraEngine::ToHexString(material.guid),
						AssetType::Material,
						material.compiledPath,
						{},
						0U) || addedAny;
				}
			}
			for (const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures) {
				if (!texture.compiledPath.empty()) {
					addedAny = addStagedAsset(
						TerraEngine::ToHexString(texture.guid),
						AssetType::Texture,
						texture.compiledPath,
						{},
						0U) || addedAny;
				}
			}
			return addedAny;
		};
		if (std::filesystem::exists(rawAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(rawAssets)) {
				if (!entry.is_regular_file()) continue;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() != ".meta") continue;

				try {
					std::ifstream f(entry.path());
					nlohmann::json meta = nlohmann::json::parse(f);

					std::string guid = meta.value("guid", "");
					std::string importer = meta.value("importerName", "");
					std::string compiledPath = meta.value("compiledPath", "");

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (guid.empty()) continue;

					AssetType type;
					bool supported = true;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (importer == "MeshImporter") type = AssetType::Mesh;
					else if (IsModelPackageImporterName(importer)) {
						const std::filesystem::path sourceAssetPath = resolveSourceAssetPath(entry.path(), meta.value("sourcePath", std::string{}));
						const bool stagedPackageChildren =
							stageModelPackageChildren(sourceAssetPath);
						if(!stagedAssetIdentityError.empty()) {
							result.errorMessage = stagedAssetIdentityError;
							return false;
						}
						if(stagedPackageChildren) {
							continue;
						}

						// Older package imports may still have a parent compiled
						// mesh. Use it only when the actual compiled payload exists;
						// otherwise skip the container so package validation does not
						// fail on a manifest entry like AntiqueCamera.temf.
						if (!compiledPath.empty() && std::filesystem::exists(compiledAssets / compiledPath)) {
							type = AssetType::Mesh;
						}
						else {
							continue;
						}
					}
					else if (importer == "TextureImporter") type = AssetType::Texture;
					else if (importer == "MaterialImporter") type = AssetType::Material;
					else if(importer == "PhysicsMaterialImporter") type = AssetType::PhysicsMaterial;
					else if(importer == "PhysicsColliderRecipeImporter" || importer == "CookedColliderImporter") type = AssetType::CookedCollider;
					else supported = false;

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (supported) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						std::filesystem::path manifestPath;
						if (!compiledPath.empty()) {
							// The compiled path is relative to the Build/Assets/ directory,
							// which matches our stagingDir layout.
							manifestPath = compiledPath;
						}
						else {
							// If the asset isn't compiled (like a .tescene), use its 
							// relative path within the Assets/ folder.
							auto rel = std::filesystem::relative(entry.path().parent_path() / meta.value("sourcePath", ""), rawAssets);
							manifestPath = rel;
						}
						if(type == AssetType::PhysicsMaterial || type == AssetType::CookedCollider) {
							std::string physicsAssetError;
							if(!addValidatedPhysicsAsset(guid, type, manifestPath, physicsAssetError)) {
								result.errorMessage = physicsAssetError;
								return false;
							}
						}
						else {
							if(!addStagedAsset(
								std::move(guid),
								type,
								manifestPath,
								{},
								0U)
								&& !stagedAssetIdentityError.empty()) {
								result.errorMessage = stagedAssetIdentityError;
								return false;
							}
						}
					}
				}
				catch (...) {
					// Skip malformed or unreadable meta files.
				}
			}
		}

		// Headless and CI cooks write immutable output directly into Build/Assets
		// and intentionally do not need editor sidecar metadata. Discover those
		// files from the staged tree as a second, typed pass so every runtime
		// material and collider still receives an identity, checksum, and manifest
		// record. Metadata-discovered records are skipped by package path.
		auto hasPhysicsRecord = [this](AssetType type, const std::filesystem::path& packagePath) {
			const std::string normalizedPath = packagePath.generic_string();
			const std::vector<PhysicsExportAssetRecord>& records = type == AssetType::PhysicsMaterial
				? m_PhysicsManifest.materials : m_PhysicsManifest.colliders;
			return std::any_of(records.begin(), records.end(), [&normalizedPath](const PhysicsExportAssetRecord& record) {
				return record.packagePath == normalizedPath;
			});
		};
		for(const auto& entry : std::filesystem::recursive_directory_iterator(stagingDir)) {
			if(!entry.is_regular_file()) continue;
			const std::filesystem::path extension = entry.path().extension();
			const AssetType type = extension == ".tephysmat" ? AssetType::PhysicsMaterial
				: extension == ".tepcoll" ? AssetType::CookedCollider : AssetType::ShaderCollection;
			if(type != AssetType::PhysicsMaterial && type != AssetType::CookedCollider) continue;
			const std::filesystem::path packagePath = std::filesystem::relative(entry.path(), stagingDir);
			if(hasPhysicsRecord(type, packagePath)) continue;

			std::vector<std::byte> bytes;
			if(!ReadExportBytes(entry.path(), bytes)) {
				result.errorMessage = "Could not read staged physics payload for manifest discovery: " + PathToUtf8(packagePath);
				return false;
			}
			std::string guid;
			if(type == AssetType::PhysicsMaterial) {
				try {
					const nlohmann::json materialDocument = nlohmann::json::parse(bytes.begin(), bytes.end());
					TerraEngine::Physics::PhysicsMaterial material;
					TerraEngine::Physics::PhysicsMaterialDiagnostics diagnostics;
					if(!TerraEngine::Physics::PhysicsMaterial::LoadJson(materialDocument, material, diagnostics)) {
						result.errorMessage = "Staged physics material is invalid during manifest discovery: " + PathToUtf8(packagePath);
						return false;
					}
					guid = TerraEngine::ToHexString(material.assetId);
				}
				catch(const std::exception&) {
					result.errorMessage = "Staged physics material is not valid JSON during manifest discovery: " + PathToUtf8(packagePath);
					return false;
				}
			}
			else {
				TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
				const auto collider = TerraEngine::Physics::CookedColliderAsset::Deserialize(bytes, diagnostics);
				if(!collider) {
					result.errorMessage = "Staged cooked collider is invalid during manifest discovery: " + PathToUtf8(packagePath);
					return false;
				}
				guid = TerraEngine::ToHexString(collider->Envelope().assetId);
			}

			std::string physicsAssetError;
			if(!addValidatedPhysicsAsset(guid, type, packagePath, physicsAssetError)) {
				result.errorMessage = physicsAssetError;
				return false;
			}
		}

		std::string physicsReferenceError;
		if(!RewriteStagedPhysicsColliderReferences(stagingDir, physicsReferenceError)) {
			result.errorMessage = physicsReferenceError;
			return false;
		}

		// Resolve animation closure from checked cooked envelopes plus real import
		// sidecars before writing either runtime manifest. A missing dependency or
		// shader rejects the entire staging transaction, so no partial archive can
		// advertise an unusable character package.
		const TerraEngine::Export::AnimationExportClosure animationClosure =
			TerraEngine::Export::AnimationExportDependencyResolver::Resolve({rawAssets, stagingDir});
		if(!animationClosure.Complete()) {
			for(const TerraEngine::Export::AnimationExportIssue& issue : animationClosure.issues) {
				ExportDiagnostic diagnostic;
				diagnostic.severity = ExportDiagnosticSeverity::Error;
				diagnostic.category = "Animation";
				diagnostic.stage = "asset_staging";
				diagnostic.code = issue.code;
				diagnostic.message = issue.message;
				diagnostic.filePath = issue.path;
				result.diagnostics.push_back(std::move(diagnostic));
			}
			result.errorMessage = animationClosure.issues.empty()
				? "Animation export dependency closure is incomplete."
				: animationClosure.issues.front().message;
			return false;
		}
		std::string animationManifestError;
		if(!TerraEngine::Export::AnimationExportDependencyResolver::WriteRuntimeManifests(
			animationClosure, stagingDir, animationManifestError)) {
			result.errorMessage = animationManifestError;
			return false;
		}
		for(const TerraEngine::Export::AnimationExportAsset& asset : animationClosure.assets) {
			// Generated prefabs are authored runtime documents loaded by SceneManager,
			// not ResourceManager assets. Keep the exact prefab record in the
			// animation manifests and TEPAK, but omit it from game.json's typed GUID
			// map. Every other unknown type still fails closed so a new dependency
			// family cannot silently bypass runtime registration.
			const bool isGeneratedPrefabDocument =
				asset.assetType == TerraEngine::Export::kUnknownAnimationExportAssetType
				&& asset.generatedPackageAsset
				&& !asset.cookedAnimationEnvelope
				&& std::filesystem::path(asset.path).extension() == ".teprefab"
				&& asset.assetClass == "other";
			if(isGeneratedPrefabDocument) {
				continue;
			}
			if(asset.assetType == TerraEngine::Export::kUnknownAnimationExportAssetType
				|| asset.assetType > static_cast<std::uint32_t>(AssetType::RagdollProfile)) {
				result.errorMessage = "Animation dependency has no stable runtime AssetType: " + asset.path;
				return false;
			}
			if(!addStagedAsset(
				asset.assetGuid,
				static_cast<AssetType>(asset.assetType),
				asset.path,
				asset.fileHash,
				asset.byteSize)) {
				result.errorMessage = stagedAssetIdentityError.empty()
					? "Animation dependency GUID is duplicated in export staging: " + asset.assetGuid
					: stagedAssetIdentityError;
				return false;
			}
		}

		// Directory iteration order is platform-dependent. Sort durable records
		// before serializing so editor/headless exports produce the same manifest.
		auto sortPhysicsAssets = [](std::vector<PhysicsExportAssetRecord>& assets) {
			std::sort(assets.begin(), assets.end(), [](const auto& left, const auto& right) {
				return std::tie(left.guid, left.packagePath) < std::tie(right.guid, right.packagePath);
			});
		};
		sortPhysicsAssets(m_PhysicsManifest.materials);
		sortPhysicsAssets(m_PhysicsManifest.colliders);
		if(!m_PhysicsManifest.materials.empty()) {
			// The first GUID in canonical order is the deterministic fallback. A
			// future explicit project default can replace this without changing the
			// append-only manifest schema.
			m_PhysicsManifest.defaultMaterialGuid = m_PhysicsManifest.materials.front().guid;
		}

		// 4. Stage the export manifest directly into the TEPAK input root so the
		// launcher can read game.json from inside the archive that it already
		// mounted, instead of depending on a second file beside the executable.
		if (!WriteExportManifest(profile, projectRoot, stagingDir)) {
			result.errorMessage = "Failed to stage game.json into the export archive input.";
			return false;
		}

		return true;
	}
	catch (const std::exception& e) {
		result.errorMessage = std::string("Asset staging failed: ") + e.what();
		return false;
	}
}

bool ExportPipeline::RunAssetPackaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Packaging, 0.50f,
		"Packaging assets into TEPAK archive...");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(stagingDir)) {
		result.errorMessage = "Asset staging directory not found: " + PathToUtf8(stagingDir);
		return false;
	}

	// Resolve the output path for the TEPAK archive.
	const std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const auto tepakPath = outputDir / std::filesystem::u8path(ResolveArchiveFileName(projectName));

	// Package progress is forwarded to the pipeline progress callback.
	PackageProgressCallback packageCallback;
	if (progressCallback) {
		packageCallback = [&progressCallback](const PackageProgress& p) {
			float fraction = 0.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (p.totalEntries > 0) {
				fraction = static_cast<float>(p.completedEntries) /
					static_cast<float>(p.totalEntries);
			}
			// Map packaging progress to the 0.50–0.80 range of overall progress.
			float overall = 0.50f + fraction * 0.30f;
			EmitProgress(progressCallback, ExportPhase::Packaging, overall,
				"Packaging: " + p.currentEntry);
		};
	}

	result.packageResult = m_Packager.Package(stagingDir, tepakPath, profile, packageCallback);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.packageResult.success) {
		result.errorMessage = result.packageResult.errorMessage;
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Packaging, 0.80f,
		"Asset packaging completed (" +
		std::to_string(result.packageResult.entryCount) + " entries).");
	return true;
}

bool ExportPipeline::RunOutputStaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const ExportRuntimeLayout& runtimeLayout,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Staging, 0.85f,
		"Staging output directory...");

	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const std::string launcherFileName = GetExportLauncherFileName(projectName, profile.platform);
	const std::string legacyLauncherFileName = GetExportLauncherFileName(profile.name, profile.platform);
	const std::string archiveFileName = ResolveArchiveFileName(projectName);
	std::filesystem::create_directories(outputDir);

	result.outputDirectory = outputDir;

	// Remove the legacy sidecar manifest and the old fixed-name archive so the
	// export folder reflects the current convention after repeated exports.
	RemoveIfExists(outputDir / "game.json");
	if (archiveFileName != "content.tepak") {
		RemoveIfExists(outputDir / "content.tepak");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (legacyLauncherFileName != launcherFileName) {
		RemoveIfExists(outputDir / std::filesystem::u8path(legacyLauncherFileName));
	}

	// Copy runtime binaries from the explicitly validated installed layout. This
	// keeps export independent of the caller's current working directory.
	try {
		const std::filesystem::path launcherSourceName = GetExportLauncherFileName("TerraEngine-Launcher", profile.platform);
		std::filesystem::path launcherSrc = runtimeLayout.launcherDirectory / launcherSourceName;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(launcherSrc)) {
			std::filesystem::copy_file(launcherSrc, outputDir / std::filesystem::u8path(launcherFileName), std::filesystem::copy_options::overwrite_existing);
		} else {
			result.errorMessage = "Required launcher binary not found: " + PathToUtf8(launcherSrc);
			return false;
		}

		// Copy required native engine libraries beside the launcher. The names
		// follow each platform's native convention while preserving the Windows
		// DLL contract already used by exported builds.
		std::vector<std::string> engineLibraries = {
			GetExportEngineLibraryName(profile.platform),
			GetExportGlfwLibraryName(profile.platform)
		};
		for (const auto& libraryName : engineLibraries) {
			std::filesystem::path librarySrc = runtimeLayout.launcherDirectory / libraryName;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(librarySrc)) {
				librarySrc = runtimeLayout.engineDirectory / libraryName;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (std::filesystem::exists(librarySrc)) {
				try {
					std::filesystem::copy_file(librarySrc, outputDir / libraryName, std::filesystem::copy_options::overwrite_existing);
				}
				catch (const std::exception& e) {
					std::cerr << "[Export] Failed to copy " << libraryName << ": " << e.what() << std::endl;
					result.errorMessage = "Failed to copy engine runtime library: " + libraryName;
					return false;
				}
			} else {
				result.errorMessage = "Required engine runtime library not found: " + PathToUtf8(librarySrc);
				return false;
			}
		}

		// Managed export payload now comes exclusively from the NativeAOT publish
		// output staged earlier in RunManagedCompilation(). TerraEngine.CSharp and
		// the editor's CoreCLR runtime files are no longer part of exported builds.

		// Shaders and fonts are packaged into TEPAK during asset staging. Remove
		// stale loose copies from older exports so the output folder reflects the
		// mounted-content runtime contract.
		RemoveDirectoryIfExists(outputDir / "Shaders");
		RemoveDirectoryIfExists(outputDir / "Fonts");
	}
	catch (const std::exception& e) {
		result.errorMessage = std::string("Failed to copy engine binaries: ") + e.what();
		return false;
	}

	EmitProgress(progressCallback, ExportPhase::Staging, 0.95f,
		"Output staging completed.");
	return true;
}

bool ExportPipeline::WriteExportManifest(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& targetDirectory)
{
	try {
		// The launcher now reads game.json from inside the mounted TEPAK archive.
		// Keep only the data that can differ per export in this file. Fixed
		// runtime paths, such as Managed/Assembly-CSharp.dll, are now derived by
		// convention so the manifest stays small and harder to desynchronize.
		nlohmann::json manifest;
		const ProjectManifestMetadata projectMetadata = LoadProjectManifestMetadata(projectRoot);
		manifest["name"] = projectMetadata.name;
		manifest["version"] = projectMetadata.version;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!profile.startupScene.empty()) {
			manifest["startupScene"] = profile.startupScene;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!profile.includedScenes.empty()) {
			manifest["scenes"] = profile.includedScenes;
		}

		manifest["renderProfile"] = ResolveRenderProfileManifest(profile, projectRoot);
		if (std::optional<nlohmann::json> renderSettings = LoadProjectRenderSettingsManifest(projectRoot)) {
			// Export renderer diagnostics are build-profile decisions, not the
			// editor's local Preferences state. Mirror the profile values into the
			// exact render settings payload so every packaged metadata path agrees.
			(*renderSettings)["validationEnabled"] = profile.enableVulkanValidationLayers;
			(*renderSettings)["debugEnabled"] = profile.enableRenderDebugDiagnostics;
			manifest["renderSettings"] = *renderSettings;
		}
		manifest["residencyPolicy"] = BuildResidencyPolicyManifest();
		manifest["streamingPolicy"] = BuildStreamingPolicyManifest();
		manifest["rendererRecovery"] = BuildRendererRecoveryManifest(profile);
		manifest["virtualGeometry"] = BuildVirtualGeometryExportManifest(profile);
		manifest["physics"] = BuildPhysicsManifestJson(m_PhysicsManifest);
		const bool hasAnimationAssets = std::any_of(
			m_StagedAssets.begin(), m_StagedAssets.end(), [](const StagedAsset& asset) {
				return asset.type >= AssetType::Skeleton && asset.type <= AssetType::RagdollProfile;
			});
		if(hasAnimationAssets) {
			// The generic asset table remains the authoritative GUID-to-path map. This
			// compact section tells startup code where to find the richer transitive
			// dependency/capability document without embedding source-side metadata.
			manifest["animation"] = {
				{"schemaVersion", TerraEngine::Export::kAnimationExportManifestSchemaVersion},
				{"runtimeManifest", TerraEngine::Export::kAnimationRuntimeManifestPath},
				{"cpuFallback", true},
				{"headless", true}
			};
		}

		// Include the asset GUID map so the engine runtime can resolve 
		// cross-resource references (UUIDs) back to filenames in the archive.
		manifest["assets"] = nlohmann::json::array();
		std::vector<StagedAsset> sortedAssets = m_StagedAssets;
		std::sort(sortedAssets.begin(), sortedAssets.end(), [](const StagedAsset& left, const StagedAsset& right) {
			return std::tie(left.guid, left.path, left.type) < std::tie(right.guid, right.path, right.type);
		});
		for (const auto& asset : sortedAssets) {
			nlohmann::json entry;
			entry["guid"] = asset.guid;
			entry["path"] = asset.path;
			entry["type"] = static_cast<uint32_t>(asset.type);
			if (asset.virtualGeometryRequired)
			{
				entry["virtualGeometry"] = {
					{ "required", true },
					{ "compiledPayload", asset.path },
					{ "diagnosticsEnabled", profile.virtualGeometryDiagnosticsEnabled },
					{ "requiresAdaptiveCookData", true },
					{ "requiresClusterFallback", true },
					{ "requiresPageData", true }
				};
			}
			manifest["assets"].push_back(std::move(entry));
		}

		const auto manifestPath = targetDirectory / "game.json";
		std::ofstream out(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!out) {
			return false;
		}
		out << manifest.dump(4);
		return out.good();
	}
	catch (...) {
		return false;
	}
}
