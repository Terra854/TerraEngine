// ExportPipeline.cpp — Export pipeline orchestrator implementation.

#include "ExportPipeline.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

// --- Helpers ---

static void EmitProgress(ExportProgressCallback& cb, ExportPhase phase, float progress, const std::string& message, bool finished = false) {
	if (!cb) return;
	ExportProgress p;
	p.phase = phase;
	p.overallProgress = progress;
	p.statusMessage = message;
	p.finished = finished;
	cb(p);
}

static std::filesystem::path ResolveOutputDirectory(const BuildProfile& profile, const std::filesystem::path& projectRoot) {
	if (!profile.outputDirectory.empty()) {
		return profile.outputDirectory;
	}

	return projectRoot / "Builds" / profile.name;
}

static std::string ResolveProjectExportName(const std::filesystem::path& projectRoot, const BuildProfile& profile) {
	// Export artifacts are named after the project directory so the launcher can
	// deterministically resolve "<ProjectName>.tepak" back to the matching
	// executable at runtime. If the directory name is somehow empty, fall back
	// to the profile name instead of producing an empty filename.
	std::string projectName = projectRoot.filename().string();
	if (projectName.empty()) {
		projectName = profile.name.empty() ? "TerraEngine" : profile.name;
	}
	return projectName;
}

static std::string ResolveLauncherFileName(const std::string& projectName, ExportPlatform platform) {
	return platform == ExportPlatform::Windows ? projectName + ".exe" : projectName;
}

static std::string ResolveArchiveFileName(const std::string& projectName) {
	return projectName + ".tepak";
}

static void RemoveIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove(path, errorCode);
}

static void RemoveDirectoryIfExists(const std::filesystem::path& path) {
	std::error_code errorCode;
	std::filesystem::remove_all(path, errorCode);
}

static void RemoveDebugSymbolFiles(const std::filesystem::path& rootDirectory) {
	if (!std::filesystem::exists(rootDirectory)) {
		return;
	}

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

struct ProjectManifestMetadata {
	std::string name = "TerraEngine";
	std::string version = "1.0.0";
};

static ProjectManifestMetadata LoadProjectManifestMetadata(const std::filesystem::path& projectRoot) {
	ProjectManifestMetadata metadata;
	if (!projectRoot.filename().string().empty()) {
		metadata.name = projectRoot.filename().string();
	}

	const std::filesystem::path manifestPath = projectRoot / "game.json";
	if (!std::filesystem::exists(manifestPath)) {
		return metadata;
	}

	try {
		std::ifstream input(manifestPath);
		if (!input) {
			return metadata;
		}

		const nlohmann::json manifest = nlohmann::json::parse(input);
		if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string()) {
			const std::string manifestName = nameIt->get<std::string>();
			if (!manifestName.empty()) {
				metadata.name = manifestName;
			}
		}

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

static uint64_t ComputeExportScriptId(const std::string& className) {
	const uint64_t fnvOffsetBasis = 14695981039346656037ULL;
	const uint64_t fnvPrime = 1099511628211ULL;
	uint64_t hash = fnvOffsetBasis;
	for (unsigned char value : className) {
		hash ^= static_cast<uint64_t>(value);
		hash *= fnvPrime;
	}
	return hash;
}

static bool RewriteBindingPayloads(nlohmann::json& node, std::unordered_map<uint64_t, std::string>& observedScriptIds, std::string& outError) {
	if (node.is_object()) {
		if (node.contains("components") && node["components"].is_object()) {
			nlohmann::json& components = node["components"];
			if (components.contains("Script") && components["Script"].is_object()) {
				nlohmann::json& scriptComponent = components["Script"];
				if (scriptComponent.contains("BindingsJson") && scriptComponent["BindingsJson"].is_string()) {
					nlohmann::json bindings = nlohmann::json::parse(scriptComponent["BindingsJson"].get<std::string>(), nullptr, false);
					if (bindings.is_discarded() || !bindings.is_array()) {
						outError = "Failed to parse Script.BindingsJson during export staging.";
						return false;
					}

					for (nlohmann::json& binding : bindings) {
						if (!binding.is_object()) {
							continue;
						}

						const std::string className = binding.value("className", std::string{});
						if (className.empty()) {
							if (binding.contains("scriptId") && binding["scriptId"].is_number_unsigned()) {
								continue;
							}

							outError = "Export staging found a script binding without a className.";
							return false;
						}

						const uint64_t scriptId = ComputeExportScriptId(className);
						if (auto existing = observedScriptIds.find(scriptId); existing != observedScriptIds.end() && existing->second != className) {
							outError = "NativeAOT export failed because script hash collision " + std::to_string(scriptId) + " maps both '" + existing->second + "' and '" + className + "'.";
							return false;
						}

						observedScriptIds[scriptId] = className;
						binding["scriptId"] = scriptId;
						binding.erase("className");
					}

					scriptComponent["BindingsJson"] = bindings.dump();
				}
			}
		}

		for (auto& [_, child] : node.items()) {
			if (!RewriteBindingPayloads(child, observedScriptIds, outError)) {
				return false;
			}
		}
	}
	else if (node.is_array()) {
		for (nlohmann::json& child : node) {
			if (!RewriteBindingPayloads(child, observedScriptIds, outError)) {
				return false;
			}
		}
	}

	return true;
}

static bool RewriteStagedScriptBindings(const std::filesystem::path& stagingDir, std::string& outError) {
	std::unordered_map<uint64_t, std::string> observedScriptIds;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(stagingDir)) {
		if (!entry.is_regular_file()) {
			continue;
		}

		const std::filesystem::path extension = entry.path().extension();
		if (extension != ".tescene" && extension != ".teprefab") {
			continue;
		}

		std::ifstream input(entry.path());
		if (!input) {
			outError = "Failed to open staged asset for script rewrite: " + entry.path().string();
			return false;
		}

		nlohmann::json assetJson = nlohmann::json::parse(input, nullptr, false);
		if (assetJson.is_discarded()) {
			outError = "Failed to parse staged asset JSON: " + entry.path().string();
			return false;
		}

		if (!RewriteBindingPayloads(assetJson, observedScriptIds, outError)) {
			outError += " File: " + entry.path().string();
			return false;
		}

		std::ofstream output(entry.path(), std::ios::trunc);
		if (!output) {
			outError = "Failed to write staged asset after script rewrite: " + entry.path().string();
			return false;
		}

		output << assetJson.dump(4);
		if (!output.good()) {
			outError = "Failed to flush staged asset after script rewrite: " + entry.path().string();
			return false;
		}
	}

	return true;
}

// --- Public API ---

ExportResult ExportPipeline::Execute(
	const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	ExportProgressCallback progressCallback)
{
	ExportResult result;
	const auto startTime = std::chrono::steady_clock::now();

	m_StagedAssets.clear();

	// Use a temporary staging directory for asset merging and packing.
	// This ensures the raw and compiled content is unified before being TEPAKed.
	std::filesystem::path stagingDir = projectRoot / "Temp" / "ExportStaging";

	try {
		// Phase 1: Validation
		if (!RunValidation(profile, projectRoot, result, progressCallback)) {
			result.totalElapsedSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - startTime).count();
			EmitProgress(progressCallback, ExportPhase::Failed, 1.0f,
				"Export failed during validation.", true);
			return result;
		}

		// Phase 2: Managed compilation
		if (profile.enableManagedCompilation) {
			if (!RunManagedCompilation(profile, projectRoot, result, progressCallback)) {
				result.totalElapsedSeconds = std::chrono::duration<double>(
					std::chrono::steady_clock::now() - startTime).count();
				EmitProgress(progressCallback, ExportPhase::Failed, 1.0f,
					"Export failed during managed compilation.", true);
				return result;
			}
		}

		// Phase 3: Asset staging
		if (!RunAssetStaging(profile, projectRoot, stagingDir, result, progressCallback)) {
			std::filesystem::remove_all(stagingDir);
			result.totalElapsedSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - startTime).count();
			EmitProgress(progressCallback, ExportPhase::Failed, 1.0f,
				"Export failed during asset staging.", true);
			return result;
		}

		// Phase 4: Asset packaging
		if (!RunAssetPackaging(profile, stagingDir, result, progressCallback)) {
			std::filesystem::remove_all(stagingDir);
			result.totalElapsedSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - startTime).count();
			EmitProgress(progressCallback, ExportPhase::Failed, 1.0f,
				"Export failed during asset packaging.", true);
			return result;
		}

		// Cleanup staging directory after packaging.
		std::filesystem::remove_all(stagingDir);

		// Phase 5: Output staging
		if (!RunOutputStaging(profile, projectRoot, result, progressCallback)) {
			result.totalElapsedSeconds = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - startTime).count();
			EmitProgress(progressCallback, ExportPhase::Failed, 1.0f,
				"Export failed during output staging.", true);
			return result;
		}

		result.success = true;
		result.totalElapsedSeconds = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();
		EmitProgress(progressCallback, ExportPhase::Complete, 1.0f,
			"Export completed successfully in " +
			std::to_string(static_cast<int>(result.totalElapsedSeconds)) + "s.", true);
		return result;
	}
	catch (const std::exception& e) {
		std::filesystem::remove_all(stagingDir);
		result.success = false;
		result.errorMessage = std::string("Unhandled exception: ") + e.what();
		return result;
	}
}

// --- Phase implementations ---

bool ExportPipeline::RunValidation(const BuildProfile& profile, const std::filesystem::path& projectRoot, ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Validating, 0.0f,
		"Validating build profile...");

	result.validationResult = m_Validator.ValidateAll(profile, projectRoot);

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
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Compiling, 0.15f,
		"Compiling managed project with NativeAOT...");

	// The managed project is strictly required to be at the project root for export.
	std::filesystem::path managedProject = projectRoot / "Assembly-CSharp.csproj";

	if (!std::filesystem::exists(managedProject)) {
		result.errorMessage = "Managed project not found: " + managedProject.string();
		return false;
	}

	// Resolve the output directory for the NativeAOT publish output.
	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	std::filesystem::create_directories(outputDir);
	const std::filesystem::path managedOutputDir = outputDir / "Managed";

	// NativeAOT publish does not clean removed files from a previous export.
	// Clear the export Managed directory first so stale bridge/runtime payloads
	// such as TerraEngine.CSharp.dll cannot survive after the export pipeline
	// stops copying them.
	RemoveDirectoryIfExists(managedOutputDir);

	// Wipe the intermediate object directory to prevent stale NativeAOT caching
	// across exports, ensuring we compile the latest bootstrap sources.
	// The .csproj places obj files in Build/Assembly-CSharp/obj by default.
	std::filesystem::path objDir = projectRoot / "Build" / "Assembly-CSharp" / "obj";
	if (std::filesystem::exists(objDir)) {
		try { std::filesystem::remove_all(objDir); } catch (...) {}
	}
	
	// Also wipe any old obj folder just in case.
	std::filesystem::path oldObjDir = outputDir / "obj";
	if (std::filesystem::exists(oldObjDir)) {
		try { std::filesystem::remove_all(oldObjDir); } catch (...) {}
	}

	// WIPE OBSOLETE INTEROP FOLDER:
	// Previously, we copied ExportBootstrap.cs to the user's project. This is now
	// handled entirely by the Source Generator. We forcefully delete the old folder
	// here to guarantee the compiler doesn't pick up stale, conflicting files.
	std::filesystem::path obsoleteExportDir = projectRoot / "Project" / "Internal" / "Export";
	if (std::filesystem::exists(obsoleteExportDir)) {
		try {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(obsoleteExportDir)) {
				std::filesystem::permissions(entry, std::filesystem::perms::all);
			}
			std::filesystem::remove_all(obsoleteExportDir);
		} catch (const std::exception& e) {
			result.errorMessage = std::string("CRITICAL: Failed to delete obsolete folder. Please manually delete 'Project/Internal/Export' from your project before exporting! Error: ") + e.what();
			return false;
		}
	}
	if (std::filesystem::exists(obsoleteExportDir)) {
		result.errorMessage = "CRITICAL: Obsolete 'Project/Internal/Export' folder still exists. Please manually delete it to prevent C# compiler conflicts.";
		return false;
	}

	// Build the dotnet publish command for NativeAOT.
	// The runtime identifier is set based on the target platform.
	std::string rid = "win-x64";
	if (profile.platform == ExportPlatform::Linux) {
		rid = "linux-x64";
	}

	std::ostringstream cmd;
	cmd << "dotnet publish"
		<< " \"" << managedProject.string() << "\""
		<< " -c Release"
		<< " -r " << rid
		<< " -p:PublishAot=true"
		<< " -p:DefineConstants=TERRAENGINE_EXPORT"
		<< " -o \"" << managedOutputDir.string() << "\"";

	if (profile.stripNativeAotOutput) {
		cmd << " -p:StripSymbols=true";
	}
	if (!profile.includeDebugSymbols) {
		cmd << " -p:DebugType=none";
	}
	if (!profile.additionalPublishArgs.empty()) {
		cmd << " " << profile.additionalPublishArgs;
	}

	const std::string commandLine = cmd.str();

	// Execute the dotnet publish command.
	const int exitCode = std::system(commandLine.c_str());
	if (exitCode != 0) {
		result.errorMessage = "NativeAOT compilation failed (exit code " +
			std::to_string(exitCode) + ").";
		return false;
	}

	if (!profile.includeDebugSymbols) {
		RemoveDebugSymbolFiles(managedOutputDir);
	}

	EmitProgress(progressCallback, ExportPhase::Compiling, 0.45f,
		"Managed compilation completed.");
	return true;
}

bool ExportPipeline::RunAssetStaging(const BuildProfile& profile,
	const std::filesystem::path& projectRoot,
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Packaging, 0.45f,
		"Staging assets for packaging...");

	try {
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
				
				// Exclude .meta files from the exported build.
				if (entry.path().extension() == ".meta") continue;

				auto relative = std::filesystem::relative(entry.path(), rawAssets);
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

				auto relative = std::filesystem::relative(entry.path(), compiledAssets);
				auto dest = stagingDir / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
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
		if (std::filesystem::exists(rawAssets)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(rawAssets)) {
				if (!entry.is_regular_file()) continue;
				if (entry.path().extension() != ".meta") continue;

				try {
					std::ifstream f(entry.path());
					nlohmann::json meta = nlohmann::json::parse(f);

					std::string guid = meta.value("guid", "");
					std::string importer = meta.value("importerName", "");
					std::string compiledPath = meta.value("compiledPath", "");

					if (guid.empty()) continue;

					AssetType type;
					bool supported = true;
					if (importer == "MeshImporter") type = AssetType::Mesh;
					else if (importer == "TextureImporter") type = AssetType::Texture;
					else if (importer == "MaterialImporter") type = AssetType::Material;
					else supported = false;

					if (supported) {
						StagedAsset asset;
						asset.guid = guid;
						asset.type = type;

						if (!compiledPath.empty()) {
							// The compiled path is relative to the Build/Assets/ directory,
							// which matches our stagingDir layout.
							asset.path = compiledPath;
						}
						else {
							// If the asset isn't compiled (like a .tescene), use its 
							// relative path within the Assets/ folder.
							auto rel = std::filesystem::relative(entry.path().parent_path() / meta.value("sourcePath", ""), rawAssets);
							asset.path = rel.generic_string();
						}
						m_StagedAssets.push_back(std::move(asset));
					}
				}
				catch (...) {
					// Skip malformed or unreadable meta files.
				}
			}
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
	const std::filesystem::path& stagingDir,
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Packaging, 0.50f,
		"Packaging assets into TEPAK archive...");

	if (!std::filesystem::exists(stagingDir)) {
		result.errorMessage = "Asset staging directory not found: " + stagingDir.string();
		return false;
	}

	// Resolve the project root for default output path resolution.
	// We can deduce the project root from the staging directory (Project/Temp/ExportStaging).
	const auto projectRoot = stagingDir.parent_path().parent_path();

	// Resolve the output path for the TEPAK archive.
	const std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const auto tepakPath = outputDir / ResolveArchiveFileName(projectName);

	// Package progress is forwarded to the pipeline progress callback.
	PackageProgressCallback packageCallback;
	if (progressCallback) {
		packageCallback = [&progressCallback](const PackageProgress& p) {
			float fraction = 0.0f;
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
	ExportResult& result,
	ExportProgressCallback progressCallback)
{
	EmitProgress(progressCallback, ExportPhase::Staging, 0.85f,
		"Staging output directory...");

	std::filesystem::path outputDir = ResolveOutputDirectory(profile, projectRoot);
	const std::string projectName = ResolveProjectExportName(projectRoot, profile);
	const std::string launcherFileName = ResolveLauncherFileName(projectName, profile.platform);
	const std::string legacyLauncherFileName = ResolveLauncherFileName(profile.name, profile.platform);
	const std::string archiveFileName = ResolveArchiveFileName(projectName);
	std::filesystem::create_directories(outputDir);

	result.outputDirectory = outputDir;

	// Remove the legacy sidecar manifest and the old fixed-name archive so the
	// export folder reflects the current convention after repeated exports.
	RemoveIfExists(outputDir / "game.json");
	if (archiveFileName != "content.tepak") {
		RemoveIfExists(outputDir / "content.tepak");
	}
	if (legacyLauncherFileName != launcherFileName) {
		RemoveIfExists(outputDir / legacyLauncherFileName);
	}

	// Copy engine binaries and shaders from the editor's build directory.
	// By convention, the editor runs from <EngineBuild>/Editor/ and
	// engine binaries are in <EngineBuild>/Launcher/ or <EngineBuild>/Engine/.
	try {
		std::filesystem::path editorDir = std::filesystem::current_path();
		std::filesystem::path engineBuildDir = editorDir.parent_path();
		
		std::filesystem::path launcherSrc = engineBuildDir / "Launcher" / "TerraEngine-Launcher.exe";
		if (std::filesystem::exists(launcherSrc)) {
			std::filesystem::copy_file(launcherSrc, outputDir / launcherFileName, std::filesystem::copy_options::overwrite_existing);
		}

		// Copy required native engine DLLs beside the launcher.
		std::vector<std::string> engineDlls = { "TerraEngine.dll", "rttr_core.dll", "glfw3.dll" };
		for (const auto& dll : engineDlls) {
			std::filesystem::path dllSrc = engineBuildDir / "Launcher" / dll;
			if (!std::filesystem::exists(dllSrc)) {
				dllSrc = engineBuildDir / "Engine" / dll;
			}
			if (std::filesystem::exists(dllSrc)) {
				try {
					std::filesystem::copy_file(dllSrc, outputDir / dll, std::filesystem::copy_options::overwrite_existing);
				}
				catch (const std::exception& e) {
					std::cerr << "[Export] Failed to copy " << dll << ": " << e.what() << std::endl;
					result.errorMessage = "Failed to copy engine DLL: " + dll;
					return false;
				}
			} else {
				result.errorMessage = "Required engine binary not found: " + dllSrc.string();
				return false;
			}
		}

		// Managed export payload now comes exclusively from the NativeAOT publish
		// output staged earlier in RunManagedCompilation(). TerraEngine.CSharp and
		// the editor's CoreCLR runtime files are no longer part of exported builds.

		// Copy engine shaders.
		std::filesystem::path shadersSrc = editorDir / "Shaders";
		if (std::filesystem::exists(shadersSrc)) {
			std::filesystem::path shadersDest = outputDir / "Shaders";
			// Replace the staged shader tree wholesale so stale runtime shader
			// payloads from a previous export cannot linger beside the new build.
			RemoveDirectoryIfExists(shadersDest);
			std::filesystem::create_directories(shadersDest);
			for (const auto& entry : std::filesystem::recursive_directory_iterator(shadersSrc)) {
				if (!entry.is_regular_file()) continue;
				auto relative = std::filesystem::relative(entry.path(), shadersSrc);
				if (!relative.empty() && relative.begin()->string() == "Editor") {
					// Editor-only debug/view shaders are not valid runtime content
					// for exported games, so keep the export package limited to the
					// gameplay shader set.
					continue;
				}
				auto dest = shadersDest / relative;
				std::filesystem::create_directories(dest.parent_path());
				std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
			}
		}
	}
	catch (const std::exception& e) {
		result.errorMessage = std::string("Failed to copy engine binaries/shaders: ") + e.what();
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
		if (!profile.startupScene.empty()) {
			manifest["startupScene"] = profile.startupScene;
		}
		if (!profile.includedScenes.empty()) {
			manifest["scenes"] = profile.includedScenes;
		}

		// Include the asset GUID map so the engine runtime can resolve 
		// cross-resource references (UUIDs) back to filenames in the archive.
		manifest["assets"] = nlohmann::json::array();
		for (const auto& asset : m_StagedAssets) {
			nlohmann::json entry;
			entry["guid"] = asset.guid;
			entry["path"] = asset.path;
			entry["type"] = static_cast<uint32_t>(asset.type);
			manifest["assets"].push_back(std::move(entry));
		}

		const auto manifestPath = targetDirectory / "game.json";
		std::ofstream out(manifestPath);
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
