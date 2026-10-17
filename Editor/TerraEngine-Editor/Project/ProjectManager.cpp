// ProjectManager.cpp — Editor project lifecycle implementation
//
// Implements the project management logic, including project creation from templates and project loading from disk. It manages the cleanup of legacy interop scripts and ensures that project manifests are correctly synchronized with the active editor state.

#include "ProjectManager.h"
#include <array>
#include <bit>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Scene/DerivedSceneCache.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Navigation/Agents/NavigationAgentSystem.h"
#include "Engine/Navigation/NavigationSystem.h"
#include "Resource/ResourceStreaming.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr std::string_view kDefaultProjectVersion = "1.0.0";
	constexpr std::array<std::string_view, 2> kDefaultEditorLayoutFiles = {
		"EditorLayout.terra",
		"EditorLayout.ini"
	};

	std::vector<std::byte> ReadBinaryFile(const fs::path& path, std::string* outError)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
		{
			if (outError != nullptr) *outError = "Could not open '" + path.string() + "'.";
			return {};
		}
		const std::streamsize length = input.tellg();
		if (length < 0 || static_cast<std::uint64_t>(length) > TerraEngine::Scene::kDerivedSceneMaximumArtifactBytes)
		{
			if (outError != nullptr) *outError = "File is empty, unreadable, or exceeds the derived-scene size bound: '" + path.string() + "'.";
			return {};
		}
		input.seekg(0, std::ios::beg);
		std::vector<std::byte> bytes(static_cast<std::size_t>(length));
		if (length > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), length))
		{
			if (outError != nullptr) *outError = "Could not read all bytes from '" + path.string() + "'.";
			return {};
		}
		return bytes;
	}

	std::string FormatChecksum(const std::vector<std::byte>& bytes)
	{
		std::ostringstream output;
		output << std::hex << std::setfill('0') << std::setw(16)
			<< TerraEngine::Scene::CalculateDerivedSceneChecksum(bytes);
		return output.str();
	}

	std::string GetDerivedScenePlatformIdentity()
	{
#ifdef _WIN32
		constexpr std::string_view operatingSystem = "Windows";
#elif defined(__APPLE__)
		constexpr std::string_view operatingSystem = "macOS";
#else
		constexpr std::string_view operatingSystem = "Linux";
#endif
		return std::string(operatingSystem) + (sizeof(void*) == 8U ? "-x64" : "-x86");
	}

	TerraEngine::Scene::DerivedSceneCompatibilityIdentity BuildDerivedSceneIdentity(
		const fs::path& assetsRoot,
		const fs::path& sourcePath,
		const std::vector<std::byte>& sourceBytes)
	{
		std::error_code relativeError;
		const fs::path sourceId = fs::relative(sourcePath, assetsRoot, relativeError);
		TerraEngine::Scene::DerivedSceneCompatibilityIdentity identity;
		identity.sourceId = relativeError ? sourcePath.filename().generic_string() : sourceId.generic_string();
		identity.sourceHash = FormatChecksum(sourceBytes);
		// The current authored scene has no external section dependency table. Keep
		// this identity explicit so a later dependency graph can replace it without
		// weakening validation of existing cache files.
		identity.dependencyHash = "no-external-derived-dependencies-v1";
		identity.componentSchemaFingerprint = "terraengine-component-schema-v1";
		identity.reflectionFingerprint = "terraengine-reflection-schema-v1";
		identity.engineCompatibility = "TerraEngine-derived-scene-v1";
		identity.platform = GetDerivedScenePlatformIdentity();
		identity.byteOrder = std::endian::native == std::endian::little
			? TerraEngine::Scene::DerivedSceneByteOrder::LittleEndian
			: TerraEngine::Scene::DerivedSceneByteOrder::BigEndian;
		return identity;
	}

	fs::path GetDerivedScenePath(const fs::path& projectRoot, const std::string& sourceId)
	{
		fs::path relativeSource(sourceId);
		fs::path derivedPath = projectRoot / "Library" / "DerivedScenes" / relativeSource;
		derivedPath += ".bin";
		return derivedPath.lexically_normal();
	}

	bool PublishBinaryFileAtomically(const fs::path& finalPath, const std::vector<std::byte>& bytes, std::string* outError)
	{
		std::error_code errorCode;
		fs::create_directories(finalPath.parent_path(), errorCode);
		if (errorCode)
		{
			if (outError != nullptr) *outError = "Could not create the derived-scene directory: " + errorCode.message();
			return false;
		}
		const fs::path stagedPath = finalPath.string() + ".tmp";
		{
			std::ofstream output(stagedPath, std::ios::binary | std::ios::trunc);
			if (!output)
			{
				if (outError != nullptr) *outError = "Could not open the derived-scene staging file.";
				return false;
			}
			if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			output.flush();
			if (!output)
			{
				output.close();
				fs::remove(stagedPath, errorCode);
				if (outError != nullptr) *outError = "Could not flush the complete derived-scene staging file.";
				return false;
			}
		}

		bool published = false;
#ifdef _WIN32
		published = MoveFileExW(stagedPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		if (!published) errorCode = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
		fs::rename(stagedPath, finalPath, errorCode);
		published = !errorCode;
#endif
		if (!published)
		{
			std::error_code cleanupError;
			fs::remove(stagedPath, cleanupError);
			if (outError != nullptr) *outError = "Could not atomically publish the derived scene: " + errorCode.message();
		}
		return published;
	}

	fs::path ResolveProjectTemplateRoot()
	{
		// Developer and test builds should use the committed source templates.
		// Installed editors fall back to templates beside the editor executable so
		// project creation remains self-contained after the source tree is removed.
#ifdef TERRAENGINE_SOURCE_DIR
		const fs::path sourceTemplates = fs::path(TERRAENGINE_SOURCE_DIR)
			/ "Editor" / "TerraEngine-Editor" / "Project" / "Templates";
		if (fs::exists(sourceTemplates / "UserSettings" / kDefaultEditorLayoutFiles.front()))
		{
			return sourceTemplates;
		}
#endif

		fs::path editorDirectory;
#ifdef _WIN32
		wchar_t executablePath[MAX_PATH];
		if (GetModuleFileNameW(nullptr, executablePath, MAX_PATH) != 0)
		{
			editorDirectory = fs::path(executablePath).parent_path();
		}
		else
		{
			editorDirectory = fs::current_path();
		}
#else
		editorDirectory = fs::current_path();
#endif

		const std::array<fs::path, 4> candidates = {
			editorDirectory / "Project" / "Templates",
			editorDirectory / "Editor" / "TerraEngine-Editor" / "Project" / "Templates",
			editorDirectory / "Templates",
			editorDirectory.parent_path() / "Project" / "Templates"
		};
		for (const fs::path& candidate : candidates)
		{
			if (fs::exists(candidate / "UserSettings" / kDefaultEditorLayoutFiles.front()))
			{
				return candidate;
			}
		}
		return {};
	}

	bool CopyDefaultEditorLayout(const fs::path& projectRoot, std::string* outError)
	{
		const fs::path resolvedTemplateRoot = ResolveProjectTemplateRoot();
		if (resolvedTemplateRoot.empty())
		{
			const std::string message = "Default editor layout templates are missing from the editor installation.";
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}
		const fs::path templateRoot = resolvedTemplateRoot / "UserSettings";

		// Copy instead of parsing and reserializing so ImGui docking identifiers,
		// table widths, and window visibility remain exactly as the approved layout.
		for (const std::string_view fileName : kDefaultEditorLayoutFiles)
		{
			const fs::path source = templateRoot / fileName;
			const fs::path destination = projectRoot / "UserSettings" / fileName;
			std::error_code errorCode;
			fs::copy_file(source, destination, fs::copy_options::overwrite_existing, errorCode);
			if (errorCode)
			{
				const std::string message = "Failed to copy default editor layout '" + source.string() + "': " + errorCode.message();
				if (outError != nullptr)
				{
					*outError = message;
				}
				return false;
			}
		}
		return true;
	}

	std::string NormalizePathForRecentComparison(const fs::path& path)
	{
		std::error_code errorCode;
		fs::path normalized = fs::weakly_canonical(path, errorCode);
		if (errorCode)
		{
			normalized = fs::absolute(path, errorCode).lexically_normal();
			if (errorCode)
			{
				normalized = path.lexically_normal();
			}
		}

		std::string value = normalized.make_preferred().string();
#ifdef _WIN32
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
#endif
		while (value.size() > 1 && (value.back() == '\\' || value.back() == '/'))
		{
			value.pop_back();
		}
		return value;
	}

	bool IsSameOrChildPath(const fs::path& candidatePath, const fs::path& rootPath)
	{
		const std::string candidate = NormalizePathForRecentComparison(candidatePath);
		const std::string root = NormalizePathForRecentComparison(rootPath);
		if (candidate.empty() || root.empty())
		{
			return false;
		}
		if (candidate == root)
		{
			return true;
		}

		std::string rootWithSeparator = root;
		if (rootWithSeparator.back() != '\\' && rootWithSeparator.back() != '/')
		{
			rootWithSeparator.push_back(fs::path::preferred_separator);
		}
		return candidate.rfind(rootWithSeparator, 0) == 0;
	}

	bool IsRecentProjectSuppressedByEnvironment()
	{
		if (const char* value = std::getenv("TERRAENGINE_SUPPRESS_PROJECT_RECENTS"))
		{
			std::string text = value;
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return text == "1" || text == "true" || text == "yes" || text == "on";
		}
		return false;
	}

	bool IsTemporaryProjectPath(const fs::path& projectPath)
	{
		if (projectPath.empty())
		{
			return true;
		}

		std::error_code errorCode;
		const fs::path tempRoot = fs::temp_directory_path(errorCode);
		if (!errorCode && !tempRoot.empty() && IsSameOrChildPath(projectPath, tempRoot))
		{
			return true;
		}

		return false;
	}

	bool ShouldTrackRecentProjectPath(const fs::path& projectPath)
	{
		return !IsRecentProjectSuppressedByEnvironment() && !IsTemporaryProjectPath(projectPath);
	}

	bool PruneTransientRecentProjects(std::vector<RecentProjectEntry>& recentProjects)
	{
		const auto originalSize = recentProjects.size();
		recentProjects.erase(
			std::remove_if(
				recentProjects.begin(),
				recentProjects.end(),
				[](const RecentProjectEntry& entry) {
					// Automation and unit tests create disposable projects under the
					// OS temp directory. Those paths are useful for the current test
					// process, but they should never occupy the user's project hub.
					return IsTemporaryProjectPath(fs::path(entry.path));
				}),
			recentProjects.end());
		return recentProjects.size() != originalSize;
	}


	nlohmann::json BuildDefaultStreamingPolicyManifest()
	{
		const TerraEngine::Resource::StreamingPolicy policy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["deferScenePayloadsOnLoad"] = policy.deferScenePayloadsOnLoad;
		object["streamingDistance"] = policy.streamingDistance;
		object["visibleRetentionSeconds"] = policy.visibleRetentionSeconds;
		object["neededRetentionSeconds"] = policy.neededRetentionSeconds;
		object["maxLoadsPerUpdate"] = policy.maxLoadsPerUpdate;
		// New projects should make the texture residency behavior explicit in
		// game.json so editor previews and exported runtime builds start from
		// the same mip-tail streaming defaults.
		object["textureMipStreamingEnabled"] = policy.textureMipStreamingEnabled;
		object["textureMinStreamMip"] = policy.textureMinStreamMip;
		object["textureMaxStreamMip"] = policy.textureMaxStreamMip;
		object["virtualGeometryPageMemoryBudgetBytes"] = policy.virtualGeometryPageMemoryBudgetBytes;
		object["virtualGeometryPageUploadBudgetBytes"] = policy.virtualGeometryPageUploadBudgetBytes;
		object["virtualGeometryPageRetirementBudgetBytes"] = policy.virtualGeometryPageRetirementBudgetBytes;
		object["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(policy.memoryBudgetInteraction));
		object["diagnosticsEnabled"] = policy.diagnosticsEnabled;
		return object;
	}

	bool EnsureProjectManifest(const fs::path& projectRoot, std::string_view requestedName, std::string* outError = nullptr)
	{
		const fs::path manifestPath = projectRoot / "game.json";

		if (fs::exists(manifestPath))
		{
			return true;
		}

		nlohmann::json manifest;
		manifest["name"] = requestedName.empty() ? projectRoot.filename().string() : std::string(requestedName);
		manifest["version"] = kDefaultProjectVersion;
		manifest["streamingPolicy"] = BuildDefaultStreamingPolicyManifest();

		std::ofstream manifestFile(manifestPath);

		if (!manifestFile)
		{
			const std::string message = "Failed to create project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);

			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		manifestFile << manifest.dump(4);

		if (!manifestFile.good())
		{
			const std::string message = "Failed to write project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);

			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Created default project manifest at " << manifestPath.string());
		return true;
	}

	bool IsBlankDisplayName(const std::string& value)
	{
		return std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isspace(character) != 0;
		});
	}
}

ProjectManager::ProjectManager()
	: editorPreferencesStorage(TerraEngine::Editor::ResolveEditorPreferencesStorageLocation()) {
	LoadEditorPreferences();
}

ProjectManager::~ProjectManager() {
	CloseProject();
	SaveEditorPreferences();
}


bool ProjectManager::CreateProject(const fs::path& location, const std::string& name, ProjectTemplate tmpl, std::string* outError) {
	if (fs::exists(location) && !fs::is_empty(location)) {
		std::string msg = "Project location must be empty or new: " + location.string();
		TERRAENGINE_LOG(LogLevel::Error, msg);

		if (outError) *outError = msg;
		return false;
	}

	try {
		fs::create_directories(location);
		fs::create_directories(location / "Assets");
		fs::create_directories(location / "Assets" / "Scenes");
		fs::create_directories(location / "Library");
		fs::create_directories(location / "Build");
		fs::create_directories(location / "Temp");
		fs::create_directories(location / "ProjectSettings");
		fs::create_directories(location / "UserSettings");
		// Every new project starts from the approved editor workspace while keeping
		// subsequent layout changes isolated to that project's UserSettings folder.
		if (!CopyDefaultEditorLayout(location, outError)) {
			return false;
		}

		// Create the authoritative default scene in the same unbounded format that
		// the scene loader accepts. Keeping this template canonical prevents a new
		// project from opening with "No Active Scene" after strict format checks.
		{
			fs::path scenePath = location / "Assets" / "Scenes" / "Scene.tescene";
			std::ofstream sceneFile(scenePath);
			sceneFile << R"({
	"format": "TerraSceneUnboundedV1",
	"name": "Scene",
	"sceneSettings": {
		"indirectStrength": 1.0,
		"ambientOcclusionStrength": 1.0,
		"ambientOcclusionQuality": 2
	},
	"objects": [
		{
			"uuid": "00000000000000000000000000000001",
			"name": "Sky",
			"components": {
				"SkyComponent": {
					"sourceAsset": "",
					"exposure": 0.7,
					"rotationOffsetDegrees": 0.0,
					"horizonTint": [0.45, 0.58, 0.72],
					"zenithTint": [0.08, 0.16, 0.36],
					"groundTint": [0.06, 0.07, 0.09]
				},
				"LightComponent": {
					"type": 0,
					"unit": 1,
					"color": [1.0, 1.0, 1.0],
					"intensity": 2.0,
					"castsShadows": false
				}
			},
			"children": []
		},
		{
			"uuid": "00000000000000000000000000000002",
			"name": "Main Camera",
			"components": {
				"Transform": {
					"WorldPose": {
						"position": {
							"cell": { "x": "0", "y": "0", "z": "0" },
							"offsetMetres": [0.0, 1.0, -10.0]
						},
						"rotation": [1.0, 0.0, 0.0, 0.0],
						"scale": [1.0, 1.0, 1.0]
					},
					"ParentRelativePose": {
						"translationMetres": [0.0, 1.0, -10.0],
						"rotation": [1.0, 0.0, 0.0, 0.0],
						"scale": [1.0, 1.0, 1.0],
						"transformSpaceId": 0
					}
				},
				"CameraComponent": {
					"verticalFovDegrees": 45.0,
					"aspectRatio": 1.777777,
					"nearPlane": 0.1,
					"farPlane": 100.0,
					"up": [0.0, 1.0, 0.0]
				}
			},
			"children": []
		}
	]
})";
			if (!sceneFile)
			{
				const std::string message = "Could not write the default scene: " + scenePath.string();
				if (outError != nullptr) *outError = message;
				return false;
			}
		}

		ProjectSettings settings;
		// settings.projectName = name; // Field removed

		currentSettings = settings;
		projectRoot = location;
		SaveProjectSettings();

		if (!EnsureProjectManifest(location, name, outError)) {
			return false;
		}

		// Load it immediately
		return LoadProject(location, outError);
	} catch (const std::exception& e) {
		std::string msg = std::string("Failed to create project: ") + e.what();
		TERRAENGINE_LOG(LogLevel::Error, msg);

		if (outError) *outError = msg;
		return false;
	}
}


bool ProjectManager::LoadProject(const fs::path& location, std::string* outError) {
	// The user must select the project folder itself, not the settings file inside it.
	fs::path projectDir = location;
	fs::path settingsPath = projectDir / "ProjectSettings" / "ProjectSettings.terra";
	// Trace project discovery through the shared logger for consistent editor diagnostics.
	TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Checking for settings at: " << fs::absolute(settingsPath).string());

	if (!fs::exists(settingsPath)) {
		std::string msg = "Not a valid project folder (missing ProjectSettings/ProjectSettings.terra): " + fs::absolute(settingsPath).string();
		TERRAENGINE_LOG(LogLevel::Error, msg);

		if (outError) *outError = msg;
		return false;
	}

	try {
		std::ifstream file(settingsPath);
		nlohmann::json j;
		file >> j;
		currentSettings = j.get<ProjectSettings>();
		// A successfully validated project replaces the prior lifecycle generation.
		// Worker decode jobs holding the older value will cancel before publication.
		projectClosing.store(true, std::memory_order_release);
		const std::uint64_t activeGeneration = projectLoadGeneration.fetch_add(1U, std::memory_order_acq_rel) + 1U;
		projectRoot = projectDir;
		isProjectLoaded = true;
		projectClosing.store(false, std::memory_order_release);
		{
			std::scoped_lock readinessLock(readinessMutex);
			lastSceneLoadDecision = {};
			readinessState = {};
			readinessState.projectGeneration = activeGeneration;
			readinessState.projectSettingsReady = true;
			readinessState.editorMetadataDeferred = true;
		}
		rendererRecoveryActionState.canSaveProjectData = true;
		rendererRecoveryActionState.lastSafeSaveRoot = projectRoot;
		rendererRecoveryActionState.lastSafeSaveMessage.clear();

		// Derive name from directory
		std::string projectName = projectDir.filename().string();
		if (!EnsureProjectManifest(projectDir, projectName)) {
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Continuing without a freshly written game.json for project " << projectDir.string());
		}
		AddToRecentProjects(projectDir, projectName);

		// CLEAN OBSOLETE INTEROP SCRIPTS:
		// The export interop layer (ExportBootstrap, ExportVTable, etc.) is now
		// provided entirely by the engine DLL and the Roslyn Source Generator.
		// We forcefully delete any legacy script files that might exist in the
		// project to ensure they do not cause "CS0436: Type conflict" errors.
		try {
			fs::path obsoleteDir = projectDir / "Project" / "Internal" / "Export";
			if (fs::exists(obsoleteDir)) {

				for (const auto& entry : fs::recursive_directory_iterator(obsoleteDir)) {
					fs::permissions(entry, fs::perms::all);
				}
				fs::remove_all(obsoleteDir);
				TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Deleted obsolete interop folder: " << obsoleteDir.string());
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Failed to delete obsolete interop folder: " << e.what());
		}

		return true;
	} catch (const std::exception& e) {
		std::string msg = std::string("Failed to load project settings: ") + e.what();
		TERRAENGINE_LOG(LogLevel::Error, msg);

		if (outError) *outError = msg;
		return false;
	}
}

bool ProjectManager::PrepareSceneForLoading(const fs::path& scenePath, ProjectSceneLoadDecision& outDecision, std::string* outError)
{
	outDecision = {};
	if (!isProjectLoaded || projectClosing.load(std::memory_order_acquire))
	{
		if (outError != nullptr) *outError = "A project must remain open while its scene is prepared.";
		return false;
	}

	const std::uint64_t requestedGeneration = projectLoadGeneration.load(std::memory_order_acquire);
	const auto cancellationRequested = [this, requestedGeneration]()
	{
		return projectClosing.load(std::memory_order_acquire)
			|| projectLoadGeneration.load(std::memory_order_acquire) != requestedGeneration;
	};
	const fs::path assetsRoot = (projectRoot / "Assets").lexically_normal();
	const fs::path sourcePath = (scenePath.is_absolute() ? scenePath : assetsRoot / scenePath).lexically_normal();
	std::error_code relativeError;
	const fs::path relativeSource = fs::relative(sourcePath, assetsRoot, relativeError);
	if (relativeError || relativeSource.empty() || *relativeSource.begin() == "..")
	{
		if (outError != nullptr) *outError = "The requested scene must resolve beneath the active project's Assets directory.";
		return false;
	}

	std::string sourceReadError;
	const std::vector<std::byte> sourceBytes = ReadBinaryFile(sourcePath, &sourceReadError);
	if (sourceBytes.empty())
	{
		if (outError != nullptr) *outError = sourceReadError.empty() ? "The authored scene is empty." : sourceReadError;
		return false;
	}
	const TerraEngine::Scene::DerivedSceneCompatibilityIdentity identity = BuildDerivedSceneIdentity(assetsRoot, sourcePath, sourceBytes);
	const fs::path derivedPath = GetDerivedScenePath(projectRoot, identity.sourceId);
	outDecision.projectGeneration = requestedGeneration;
	outDecision.sourcePath = sourcePath;
	outDecision.derivedPath = derivedPath;

	std::string derivedMissReason = "derived scene is missing";
	if (fs::exists(derivedPath) && !cancellationRequested())
	{
		std::string derivedReadError;
		const std::vector<std::byte> encoded = ReadBinaryFile(derivedPath, &derivedReadError);
		std::string decodeError;
		const std::optional<TerraEngine::Scene::DerivedSceneEnvelope> envelope =
			TerraEngine::Scene::DecodeDerivedSceneEnvelope(encoded, &decodeError);
		if (envelope.has_value())
		{
			const TerraEngine::Scene::DerivedSceneDecodeResult derived =
				TerraEngine::Scene::PrepareDerivedSceneSections(*envelope, identity, cancellationRequested);
			if (derived.accepted)
			{
				const auto objectsSection = std::find_if(derived.sections.begin(), derived.sections.end(), [](const auto& section)
				{
					return section.descriptor.type == TerraEngine::Scene::DerivedSceneSectionType::Objects;
				});
				if (objectsSection != derived.sections.end())
				{
					std::vector<std::uint8_t> cbor;
					cbor.reserve(objectsSection->payload.size());
					for (const std::byte value : objectsSection->payload) cbor.push_back(std::to_integer<std::uint8_t>(value));
					nlohmann::json cachedDocument = nlohmann::json::from_cbor(cbor, true, false);
					if (!cachedDocument.is_discarded())
					{
						TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(cachedDocument, cancellationRequested);
						if (prepared.accepted)
						{
							outDecision.prepared = true;
							outDecision.cacheHit = true;
							outDecision.cacheState = "derived-hit";
							outDecision.reason = "compatible checksummed derived scene reused";
							outDecision.sceneDocument = std::move(cachedDocument);
							outDecision.preparedScene = std::move(prepared);
						}
						else derivedMissReason = prepared.reason;
					}
					else derivedMissReason = "derived scene object section contains invalid CBOR";
				}
				else derivedMissReason = "derived scene object section is missing";
			}
			else
			{
				derivedMissReason = derived.reason;
				outDecision.cancelled = derived.cancelled;
			}
		}
		else derivedMissReason = decodeError.empty() ? derivedReadError : decodeError;
	}

	if (!outDecision.prepared)
	{
		if (cancellationRequested())
		{
			outDecision.cancelled = true;
			outDecision.cacheState = "cancelled";
			outDecision.reason = "scene preparation was cancelled by project close or replacement";
		}
		else
		{
			const std::string authoredText(reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size());
			nlohmann::json authoredDocument = nlohmann::json::parse(authoredText, nullptr, false);
			if (authoredDocument.is_discarded())
			{
				if (outError != nullptr) *outError = "The authoritative scene JSON is invalid: '" + sourcePath.string() + "'.";
				return false;
			}
			TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(authoredDocument, cancellationRequested);
			if (!prepared.accepted)
			{
				outDecision.cancelled = prepared.cancelled;
				if (outError != nullptr) *outError = prepared.reason;
				return false;
			}
			outDecision.prepared = true;
			outDecision.usedAuthoritativeSource = true;
			outDecision.cacheState = "source-fallback";
			outDecision.reason = derivedMissReason;
			outDecision.sceneDocument = std::move(authoredDocument);
			outDecision.preparedScene = std::move(prepared);

			// Cache publication is best effort. The authoritative source has already
			// prepared successfully, so an unavailable Library directory cannot make
			// the project or scene fail to open.
			const std::vector<std::uint8_t> cbor = nlohmann::json::to_cbor(outDecision.sceneDocument);
			TerraEngine::Scene::DerivedSceneSection objectSection;
			objectSection.descriptor.type = TerraEngine::Scene::DerivedSceneSectionType::Objects;
			objectSection.descriptor.itemCount = outDecision.preparedScene.objectCount;
			objectSection.payload.reserve(cbor.size());
			for (const std::uint8_t value : cbor) objectSection.payload.push_back(static_cast<std::byte>(value));
			objectSection.descriptor.storedBytes = objectSection.payload.size();
			objectSection.descriptor.decodedBytes = objectSection.payload.size();
			objectSection.descriptor.checksum = TerraEngine::Scene::CalculateDerivedSceneChecksum(objectSection.payload);

			TerraEngine::Scene::DerivedSceneEnvelope envelope;
			envelope.manifest.identity = identity;
			envelope.manifest.objectCount = outDecision.preparedScene.objectCount;
			envelope.manifest.componentCount = outDecision.preparedScene.componentCount;
			envelope.manifest.hierarchyCount = outDecision.preparedScene.hierarchyCount;
			envelope.manifest.payloadChecksum = objectSection.descriptor.checksum;
			envelope.manifest.createdBy = "TerraEngine ProjectManager";
			envelope.sections.push_back(objectSection);
			envelope.manifest.sections.push_back(objectSection.descriptor);
			const std::vector<std::byte> encoded = TerraEngine::Scene::EncodeDerivedSceneEnvelope(envelope);
			std::string verificationError;
			const auto verificationEnvelope = TerraEngine::Scene::DecodeDerivedSceneEnvelope(encoded, &verificationError);
			const bool verified = verificationEnvelope.has_value()
				&& TerraEngine::Scene::ValidateDerivedSceneEnvelope(*verificationEnvelope, identity).accepted;
			if (verified && !cancellationRequested())
			{
				std::string publishError;
				outDecision.regenerationPublished = PublishBinaryFileAtomically(derivedPath, encoded, &publishError);
				if (!outDecision.regenerationPublished && !publishError.empty()) outDecision.reason += "; regeneration: " + publishError;
			}
			else if (!verificationError.empty()) outDecision.reason += "; regeneration validation: " + verificationError;
		}
	}

	if (!outDecision.prepared)
	{
		if (outError != nullptr) *outError = outDecision.reason;
		return false;
	}

	{
		std::scoped_lock readinessLock(readinessMutex);
		// Retain only compact decision evidence; copying a 10,000-object document
		// into project status would defeat the cache's memory benefit.
		lastSceneLoadDecision = {};
		lastSceneLoadDecision.prepared = outDecision.prepared;
		lastSceneLoadDecision.cacheHit = outDecision.cacheHit;
		lastSceneLoadDecision.usedAuthoritativeSource = outDecision.usedAuthoritativeSource;
		lastSceneLoadDecision.regenerationPublished = outDecision.regenerationPublished;
		lastSceneLoadDecision.cancelled = outDecision.cancelled;
		lastSceneLoadDecision.projectGeneration = outDecision.projectGeneration;
		lastSceneLoadDecision.sourcePath = outDecision.sourcePath;
		lastSceneLoadDecision.derivedPath = outDecision.derivedPath;
		lastSceneLoadDecision.cacheState = outDecision.cacheState;
		lastSceneLoadDecision.reason = outDecision.reason;
		readinessState.sceneReadinessSource = outDecision.cacheHit ? "derived" : "source";
		readinessState.sceneReadinessReason = outDecision.reason;
	}
	if (outError != nullptr) outError->clear();
	return true;
}

std::uint64_t ProjectManager::GetProjectLoadGeneration() const
{
	return projectLoadGeneration.load(std::memory_order_acquire);
}

bool ProjectManager::IsProjectLoadGenerationCurrent(std::uint64_t generation) const
{
	return !projectClosing.load(std::memory_order_acquire)
		&& projectLoadGeneration.load(std::memory_order_acquire) == generation;
}

ProjectSceneLoadDecision ProjectManager::GetLastSceneLoadDecision() const
{
	std::scoped_lock readinessLock(readinessMutex);
	return lastSceneLoadDecision;
}

ProjectReadinessState ProjectManager::GetReadinessState() const
{
	std::scoped_lock readinessLock(readinessMutex);
	return readinessState;
}

void ProjectManager::MarkSimulationEssentialReady(const ProjectSceneLoadDecision& decision)
{
	std::scoped_lock readinessLock(readinessMutex);
	if (decision.prepared && decision.projectGeneration == readinessState.projectGeneration)
	{
		readinessState.simulationEssentialReady = true;
		readinessState.editorMetadataDeferred = true;
		readinessState.sceneReadinessSource = decision.cacheHit ? "derived" : "source";
		readinessState.sceneReadinessReason = decision.reason;
	}
}

void ProjectManager::MarkEditorMetadataReady()
{
	std::scoped_lock readinessLock(readinessMutex);
	if (readinessState.simulationEssentialReady)
	{
		readinessState.editorMetadataDeferred = false;
		readinessState.editorMetadataReady = true;
	}
}


void ProjectManager::CloseProject() {
	if (isProjectLoaded) {
		// Invalidate worker-prepared scene packets before any project paths or
		// settings are cleared. A late job then rejects atomically at publication.
		projectClosing.store(true, std::memory_order_release);
		projectLoadGeneration.fetch_add(1U, std::memory_order_acq_rel);
		TerraEngine::Navigation::NavigationSystem::Instance().RevokeAllOwners(
			TerraEngine::Navigation::NavigationReason::SceneReplaced);
		TerraEngine::Navigation::NavigationAgentSystem::Instance().RevokeAllAgents(
			TerraEngine::Navigation::NavigationReason::SceneReplaced);
		SaveProjectSettings();
		isProjectLoaded = false;
		currentSettings.reset();
		projectRoot.clear();
		rendererRecoveryActionState.canSaveProjectData = false;
		rendererRecoveryActionState.lastSafeSaveMessage = "No active project is available for renderer-recovery save.";
		std::scoped_lock readinessLock(readinessMutex);
		lastSceneLoadDecision = {};
		readinessState = {};
		readinessState.projectGeneration = projectLoadGeneration.load(std::memory_order_acquire);
	}
}


bool ProjectManager::HasActiveProject() const {
	return isProjectLoaded;
}


const ProjectSettings& ProjectManager::GetSettings() const {
	static ProjectSettings empty;
	return currentSettings.has_value() ? *currentSettings : empty;
}

// ProjectManager::SetProjectDisplayName validates and persists the editable
// project label used by Project Settings without exposing mutable settings
// storage to every editor window.
bool ProjectManager::SetProjectDisplayName(const std::string& displayName) {
	if (!currentSettings.has_value() || IsBlankDisplayName(displayName)) {
		return false;
	}

	currentSettings->projectDisplayName = displayName;
	SaveProjectSettings();
	return true;
}


std::filesystem::path ProjectManager::GetProjectRoot() const {
	return projectRoot;
}


const std::vector<RecentProjectEntry>& ProjectManager::GetRecentProjects() const {
	return editorPrefs.recentProjects;
}


const EditorPreferences& ProjectManager::GetEditorPreferences() const {
	return editorPrefs;
}

bool ProjectManager::ReadAutomationSetting(
	bool projectScope,
	std::string_view key,
	nlohmann::json& value,
	std::string& error) const {
	if (projectScope) {
		if (!currentSettings.has_value()) {
			error = "A project must be open before reading project settings.";
			return false;
		}
		value = TerraEngine::Editor::Settings::ReadProjectAutomationSetting(*currentSettings, key);
	} else {
		value = TerraEngine::Editor::Settings::ReadEditorAutomationSetting(editorPrefs, key);
	}
	if (value.is_null()) {
		error = "Setting key is not allowlisted.";
		return false;
	}
	return true;
}

bool ProjectManager::WriteAutomationSetting(
	bool projectScope,
	std::string_view key,
	const nlohmann::json& value,
	std::string& error) {
	if (projectScope) {
		if (!currentSettings.has_value()) {
			error = "A project must be open before writing project settings.";
			return false;
		}
		ProjectSettings candidate = *currentSettings;
		if (!TerraEngine::Editor::Settings::WriteProjectAutomationSetting(candidate, key, value, error)) {
			return false;
		}
		currentSettings = std::move(candidate);
		SaveProjectSettings();
		return true;
	}

	EditorPreferences candidate = editorPrefs;
	if (!TerraEngine::Editor::Settings::WriteEditorAutomationSetting(candidate, key, value, error)) {
		return false;
	}
	editorPrefs = std::move(candidate);
	SaveEditorPreferences();
	return true;
}

const RendererRecoveryProjectActionState& ProjectManager::GetRendererRecoveryActionState() const {
	return rendererRecoveryActionState;
}


std::string ProjectManager::GetExternalScriptEditorCommand() const {
	return editorPrefs.externalScriptEditorCommand;
}


bool ProjectManager::ShouldAutoBuildScripts() const {
	return editorPrefs.autoBuildScripts;
}


void ProjectManager::SetExternalScriptEditorCommand(const std::string& commandTemplate) {
	editorPrefs.externalScriptEditorCommand = commandTemplate;
	SaveEditorPreferences();
}


void ProjectManager::SetAutoBuildScripts(bool enabled) {
	editorPrefs.autoBuildScripts = enabled;
	SaveEditorPreferences();
}

// ProjectManager::SetEditorMcpPreferences stores the bridge defaults that the editor reapplies during startup.
void ProjectManager::SetEditorMcpPreferences(bool enabled, const std::string& writeModeDefault, const std::string& recentSession, std::uint16_t listenPort) {
	editorPrefs.editorMcpEnabled = enabled;
	editorPrefs.editorMcpWriteModeDefault = writeModeDefault;
	editorPrefs.editorMcpRecentSession = recentSession;
	// The bridge validates this value before calling ProjectManager. Keep the
	// persistence layer defensive so no other caller can save an ephemeral or
	// privileged endpoint accidentally.
	if (TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(listenPort)) {
		editorPrefs.editorMcpListenPort = listenPort;
	}
	SaveEditorPreferences();
}

void ProjectManager::SetRendererDiagnosticPreferences(bool vulkanValidationLayers, bool renderDebugDiagnostics) {
	// Vulkan validation and render debug overlays are local editor diagnostics.
	// Store them with user preferences so toggling them does not dirty project
	// configuration or silently affect teammate/export defaults.
	editorPrefs.vulkanValidationLayers = vulkanValidationLayers;
	editorPrefs.renderDebugDiagnostics = renderDebugDiagnostics;
	editorPrefs.rendererDiagnosticsPreferencesInitialized = true;
	SaveEditorPreferences();
}

bool ProjectManager::SetSceneViewOverlayPreference(const TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference& preference, std::string* outError) {
	// Scene View overlay options are editor preferences, not scene data, so
	// changing gizmo/icon visibility never dirties the active project or export.
	// Keep the old value until both complete validation and staged persistence
	// succeed, so rejected updates cannot leak a partially changed live state.
	EditorPreferences candidate = editorPrefs;
	std::string validationError;
	if (!TerraEngine::Editor::Settings::TryReplaceSceneViewOverlayPreference(candidate, preference, validationError)) {
		if (outError != nullptr) {
			*outError = validationError;
		}
		return false;
	}

	EditorPreferences previous = editorPrefs;
	editorPrefs = std::move(candidate);
	std::string persistenceError;
	if (!SaveEditorPreferences(&persistenceError)) {
		editorPrefs = std::move(previous);
		if (outError != nullptr) {
			*outError = persistenceError;
		}
		return false;
	}
	if (outError != nullptr) {
		outError->clear();
	}
	return true;
}


void ProjectManager::SaveProjectSettings() {
	if (!currentSettings.has_value() || projectRoot.empty()) return;

	fs::path settingsPath = projectRoot / "ProjectSettings" / "ProjectSettings.terra";

	if (!fs::exists(settingsPath.parent_path())) {
		fs::create_directories(settingsPath.parent_path());
	}

	std::ofstream file(settingsPath);
	nlohmann::json j = *currentSettings;
	file << j.dump(4);
}


bool ProjectManager::SaveEditorPreferences(std::string* outError) {
	const fs::path& preferencesPath = editorPreferencesStorage.path;
	if (preferencesPath.empty()) {
		if (outError != nullptr) {
			*outError = "The platform user configuration directory is unavailable; editor preferences were not written.";
		}
		return false;
	}
	std::error_code errorCode;
	if (!preferencesPath.parent_path().empty()) {
		fs::create_directories(preferencesPath.parent_path(), errorCode);
		if (errorCode) {
			if (outError != nullptr) {
				*outError = "Editor preference directory could not be created: " + errorCode.message();
			}
			return false;
		}
	}

	const fs::path stagedPath = preferencesPath.string() + ".tmp";
	std::string serializedPreferences;
	try {
		// Serialize before opening the staging file. If a future preference type
		// rejects serialization, the prior final document and staging path remain
		// untouched.
		serializedPreferences = nlohmann::json(editorPrefs).dump(4);
	} catch (const std::exception& exception) {
		if (outError != nullptr) {
			*outError = std::string("Editor preferences could not be serialized: ") + exception.what();
		}
		return false;
	}
	{
		std::ofstream file(stagedPath, std::ios::binary | std::ios::trunc);
		if (!file) {
			if (outError != nullptr) {
				*outError = "Editor preference staging file could not be opened.";
			}
			return false;
		}
		file << serializedPreferences;
		file.flush();
		if (!file) {
			file.close();
			fs::remove(stagedPath, errorCode);
			if (outError != nullptr) {
				*outError = "Editor preference staging file could not be written completely.";
			}
			return false;
		}
	}

	// The staged file is complete before this single-file publication boundary.
	// MoveFileEx replaces in one filesystem operation on Windows; POSIX rename
	// provides the matching same-directory replacement behavior elsewhere.
	bool published = false;
#ifdef _WIN32
	published = MoveFileExW(
		stagedPath.c_str(),
		preferencesPath.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
	if (!published) {
		errorCode = std::error_code(static_cast<int>(GetLastError()), std::system_category());
	}
#else
	fs::rename(stagedPath, preferencesPath, errorCode);
	published = !errorCode;
#endif
	if (!published) {
		std::error_code cleanupError;
		fs::remove(stagedPath, cleanupError);
		if (outError != nullptr) {
			*outError = "Editor preferences could not be atomically published: " + errorCode.message();
		}
		return false;
	}
	if (outError != nullptr) {
		outError->clear();
	}
	return true;
}

bool ProjectManager::SaveProjectDataForRendererRecovery(std::string* outError) {
	// Device-loss recovery may run while Vulkan objects are invalid, so this
	// path intentionally writes only CPU-owned project/editor metadata through
	// the existing save functions. Scene documents, imports, cooks, and asset
	// database rewrites remain outside this emergency save to avoid hidden work.
	if (!isProjectLoaded || !currentSettings.has_value() || projectRoot.empty()) {
		const std::string message = "No active project data is available for renderer recovery.";
		rendererRecoveryActionState.canSaveProjectData = false;
		rendererRecoveryActionState.lastSafeSaveMessage = message;
		if (outError != nullptr) {
			*outError = message;
		}
		return false;
	}

	try {
		SaveProjectSettings();
		SaveEditorPreferences();
		rendererRecoveryActionState.canSaveProjectData = true;
		rendererRecoveryActionState.lastSafeSaveRoot = projectRoot;
		rendererRecoveryActionState.lastSafeSaveMessage = "Project settings and editor preferences saved without renderer state.";
		return true;
	} catch (const std::exception& exception) {
		const std::string message = std::string("Renderer-recovery project save failed: ") + exception.what();
		rendererRecoveryActionState.lastSafeSaveMessage = message;
		if (outError != nullptr) {
			*outError = message;
		}
		return false;
	}
}


void ProjectManager::LoadEditorPreferences() {
	const fs::path& preferencesPath = editorPreferencesStorage.path;
	if (!preferencesPath.empty() && fs::exists(preferencesPath)) {
		try {
			std::ifstream file(preferencesPath);
			nlohmann::json j;
			file >> j;
			editorPrefs = j.get<EditorPreferences>();
			const bool prunedTransientRecents = PruneTransientRecentProjects(editorPrefs.recentProjects);
			if (prunedTransientRecents)
			{
				SaveEditorPreferences();
			}
		} catch (...) {
			// ignore corrupt prefs
		}
	}
}


void ProjectManager::AddToRecentProjects(const fs::path& path, const std::string& name) {
	auto& list = editorPrefs.recentProjects;
	std::string pathStr = fs::absolute(path).string();

	// Remove existing entries first so a disposable automation project that was
	// saved by an older editor build is cleaned up as soon as it is opened again.
	list.erase(std::remove_if(list.begin(), list.end(), [&](const RecentProjectEntry& e) {
		return e.path == pathStr;
	}), list.end());

	if (!ShouldTrackRecentProjectPath(path))
	{
		SaveEditorPreferences();
		return;
	}

	// Add to front
	RecentProjectEntry entry;
	entry.name = name;
	entry.path = pathStr;
	entry.lastOpened = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	list.insert(list.begin(), entry);

	// Trim

	if (list.size() > 10) {
		list.resize(10);
	}
	SaveEditorPreferences();
}
