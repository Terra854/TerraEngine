// AssetDatabase.cpp — Editor asset database implementation
//
// Implements the asset lifecycle management including automated project scanning, metadata persistence, and asynchronous asset compilation. It integrates with the filesystem watcher to detect external changes and syncs the database with the engine's resource manager.

#include "AssetDatabase.h"
#include "ThreadPool/ThreadPool.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "Engine/Core/Logger/Logger.h"
#include <array>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Editor/AssetCompilers/ShaderCompiler.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "Editor/AssetCompilers/PhysicsColliderCompiler.h"
#include "Editor/AssetCompilers/AudioClipCompiler.h"
#include "Editor/AssetCompilers/NavigationGeometryCompiler.h"
#include "AssetImportResult.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include "Engine/File Formats/TerraEngineAnimationFile.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/VirtualGeometryCodec.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/Terrain/TerrainAssetStore.h"

using json = nlohmann::json;

namespace {
	constexpr auto kRecentlyRemovedAssetRetention = std::chrono::seconds(5);
	constexpr const char* kModelPackageCompilerName = "TerraEngineModelPackageCompiler.exe";
	constexpr const char* kMeshCompilerName = "TerraEngineMeshCompiler.exe";
	constexpr const char* kTextureCompilerName = "TerraEngineTextureCompiler.exe";
	constexpr const char* kShaderCompilerName = "TerraEngineShaderCompiler.exe";
#ifdef _WIN32
	constexpr const char* kPhysicsColliderCompilerName = "TerraEnginePhysicsColliderCompiler.exe";
#else
	constexpr const char* kPhysicsColliderCompilerName = "TerraEnginePhysicsColliderCompiler";
#endif

	// AnimationHotReloadPublisher invokes this callback only after its own staged
	// generation has passed stale-state validation. ResourceManager then commits
	// every prepared live cache under one lock set; false leaves both owners at
	// their complete previous generation.
	struct AnimationResourceBatchCommitContext {
		PreparedAnimationAssetReloadBatch* prepared = nullptr;
	};

	bool CommitPreparedAnimationResourceBatch(void* opaqueContext) noexcept {
		auto* context = static_cast<AnimationResourceBatchCommitContext*>(opaqueContext);
		return context != nullptr && context->prepared != nullptr
			&& ResourceManager::Instance().CommitPreparedAnimationAssetReloadBatch(
				*context->prepared);
	}

	void ReleasePreparedAnimationResourceBatchLocks(void* opaqueContext) noexcept {
		auto* context = static_cast<AnimationResourceBatchCommitContext*>(opaqueContext);
		if(context != nullptr && context->prepared != nullptr) {
			ResourceManager::Instance().ReleasePreparedAnimationAssetReloadBatchLocks(*context->prepared);
		}
	}

	void RetireAnimationResourcePayload(void*, const TerraEngine::UUID& assetId,
		std::uint64_t generation) noexcept {
		ResourceManager::Instance().RetireAnimationAssetPayloadGeneration(assetId, generation);
	}

	std::optional<TerraEngine::Physics::PhysicsAssetKind> PhysicsAssetKindForImporter(
		std::string_view importerName) {
		if(importerName == "PhysicsColliderRecipeImporter"
			|| importerName == "CookedColliderImporter") {
			return TerraEngine::Physics::PhysicsAssetKind::CookedCollider;
		}
		if(importerName == "PhysicsMaterialImporter") {
			return TerraEngine::Physics::PhysicsAssetKind::Material;
		}
		return std::nullopt;
	}

	std::string StableAssetOrganizationFingerprint(std::string_view value) {
		// FNV-1a is deterministic across editor launches, unlike std::hash whose
		// persistence guarantees are implementation-defined.
		std::uint64_t hash = 1469598103934665603ull;
		for (const unsigned char byte : value) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		std::ostringstream stream;
		stream << std::hex << std::setw(16) << std::setfill('0') << hash;
		return stream.str();
	}

	void AppendVirtualGeometryCacheHash(
		std::uint64_t& hash,
		std::string_view value)
	{
		for(const unsigned char byte : value)
		{
			hash = (hash ^ byte) * 1099511628211ull;
		}
		// Delimit fields so different input partitions cannot alias.
		hash = (hash ^ 0xffu) * 1099511628211ull;
	}

	std::string BuildVirtualGeometryStageCacheKey(
		const std::filesystem::path& sourcePath,
		const VirtualGeometryAdaptiveCompileSettings& settings,
		const std::filesystem::path& compilerPath)
	{
		std::ifstream source(sourcePath, std::ios::binary);
		if(!source)
		{
			return {};
		}

		std::uint64_t hash = 1469598103934665603ull;
		std::array<char, 64u * 1024u> buffer{};
		while(source)
		{
			source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize read = source.gcount();
			for(std::streamsize index = 0; index < read; ++index)
			{
				hash = (hash ^ static_cast<unsigned char>(buffer[index]))
					* 1099511628211ull;
			}
		}
		if(!source.eof())
		{
			return {};
		}

		std::ostringstream recipe;
		recipe << "virtual-geometry-stage-cache-v1;"
			<< settings.settingsSource << ';'
			<< settings.minimumSourceTriangles << ';'
			<< settings.minimumGeneratedTriangles << ';'
			<< settings.minimumMeshletsPerGeneratedLevel << ';'
			<< settings.minimumUsefulTriangleReduction << ';'
			<< settings.minimumUsefulMeshletReduction << ';'
			<< settings.maximumGeometricError << ';';
		for(const float ratio : settings.targetRatios)
		{
			recipe << ratio << ',';
		}
		std::error_code metadataError;
		recipe << compilerPath.filename().generic_string() << ';'
			<< std::filesystem::file_size(compilerPath, metadataError) << ';';
		metadataError.clear();
		const auto compilerWriteTime =
			std::filesystem::last_write_time(compilerPath, metadataError);
		if(!metadataError)
		{
			recipe << compilerWriteTime.time_since_epoch().count();
		}
		AppendVirtualGeometryCacheHash(hash, recipe.str());

		std::ostringstream key;
		key << std::hex << std::setw(16) << std::setfill('0') << hash;
		return key.str();
	}

	bool ValidateVirtualGeometryCachedMesh(
		const std::filesystem::path& candidate)
	{
		std::ifstream input(candidate, std::ios::binary);
		if (!input)
		{
			return false;
		}
		input.seekg(0, std::ios::end);
		const std::streamoff size = input.tellg();
		input.seekg(0, std::ios::beg);
		if (size <= 0)
		{
			return false;
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		input.read(reinterpret_cast<char*>(bytes.data()), size);
		return input && TerraEngine::Resource::ValidateTemfProductionFile(bytes, {}).IsComplete();
	}

	std::string HashVirtualGeometryFormatRecord(
		const VirtualGeometryHeader& header)
	{
		std::uint64_t hash = 1469598103934665603ull;
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(&header);
		for(std::size_t index = 0u; index < sizeof(header); ++index)
		{
			hash = (hash ^ bytes[index]) * 1099511628211ull;
		}
		std::ostringstream text;
		text << std::hex << std::setw(16) << std::setfill('0') << hash;
		return text.str();
	}

	bool PublishVirtualGeometryStageCache(
		const std::filesystem::path& candidate,
		const std::filesystem::path& cachePath,
		std::string_view privateIdentity)
	{
		if(ValidateVirtualGeometryCachedMesh(cachePath))
		{
			return true;
		}
		std::error_code error;
		std::filesystem::create_directories(cachePath.parent_path(), error);
		if(error)
		{
			return false;
		}

		const std::filesystem::path staging =
			cachePath.string() + "." + std::string(privateIdentity) + ".tmp";
		std::filesystem::remove(staging, error);
		error.clear();
		std::filesystem::copy_file(
			candidate,
			staging,
			std::filesystem::copy_options::overwrite_existing,
			error);
		if(error || !ValidateVirtualGeometryCachedMesh(staging))
		{
			std::filesystem::remove(staging, error);
			return false;
		}

		std::filesystem::rename(staging, cachePath, error);
		if(error)
		{
			// A concurrent worker may have won publication with the same content
			// key. Its complete verified entry is an acceptable resume point.
			std::filesystem::remove(staging, error);
			return ValidateVirtualGeometryCachedMesh(cachePath);
		}
		return true;
	}

	void PublishVirtualGeometryCacheProvenance(
		const std::filesystem::path& compiledPath,
		std::string_view cacheKey,
		std::string_view state,
		std::string_view reason,
		std::string_view privateIdentity)
	{
		const std::filesystem::path finalPath =
			compiledPath.string() + ".vgcache.json";
		const std::filesystem::path stagingPath =
			finalPath.string() + "." + std::string(privateIdentity) + ".tmp";
		const std::filesystem::path previousPath =
			finalPath.string() + ".previous";
		{
			std::ofstream output(stagingPath, std::ios::binary | std::ios::trunc);
			output << json{
				{ "schemaVersion", 1 },
				{ "stage", "complete-mesh" },
				{ "inputKey", cacheKey },
				{ "state", state },
				{ "reason", reason },
				{ "valid", true }
			}.dump(2) << '\n';
			if(!output)
			{
				std::error_code ignored;
				std::filesystem::remove(stagingPath, ignored);
				return;
			}
		}

		std::error_code error;
		std::filesystem::remove(previousPath, error);
		error.clear();
		if(std::filesystem::exists(finalPath))
		{
			std::filesystem::rename(finalPath, previousPath, error);
			if(error)
			{
				std::filesystem::remove(stagingPath, error);
				return;
			}
		}
		std::filesystem::rename(stagingPath, finalPath, error);
		if(error)
		{
			std::filesystem::remove(stagingPath, error);
			error.clear();
			if(std::filesystem::exists(previousPath))
			{
				std::filesystem::rename(previousPath, finalPath, error);
			}
			return;
		}
		std::filesystem::remove(previousPath, error);
	}

	bool HasParentTraversal(const std::filesystem::path& path) {
		return std::find(path.begin(), path.end(), std::filesystem::path("..")) != path.end();
	}

	std::string LowerPath(std::filesystem::path path) {
		std::string text = path.lexically_normal().generic_string();
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		return text;
	}

	bool PathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
		std::string candidate = LowerPath(path);
		std::string boundary = LowerPath(root);
		if (!boundary.ends_with('/')) {
			boundary.push_back('/');
		}
		return candidate == boundary.substr(0u, boundary.size() - 1u) || candidate.rfind(boundary, 0u) == 0u;
	}

	std::optional<std::pair<std::string, std::filesystem::path>> ResolveContainedProjectAsset(
		const std::filesystem::path& projectRoot,
		const std::filesystem::path& assetRoot,
		const std::filesystem::path& relative) {
		if (relative.empty() || relative.is_absolute() || HasParentTraversal(relative)
			|| relative.begin() == relative.end() || relative.begin()->generic_string() != "Assets"
			|| relative.extension().empty()) {
			return std::nullopt;
		}

		std::error_code canonicalError;
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(assetRoot, canonicalError);
		if (canonicalError) {
			return std::nullopt;
		}
		const std::filesystem::path absolute = (projectRoot / relative).lexically_normal();
		const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, canonicalError);
		if (canonicalError || !PathIsWithin(canonical, canonicalRoot)) {
			return std::nullopt;
		}
		return std::pair(relative.lexically_normal().generic_string(), absolute);
	}

	struct AssetImportCancelledException : std::runtime_error {
		AssetImportCancelledException()
			: std::runtime_error("asset import cancelled") {}
	};

	// UuidToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string UuidToString(const TerraEngine::UUID& uuid) {
		std::ostringstream oss;
		for (const auto& byte : uuid) {
			oss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
		}
		return oss.str();
	}

	// StringToUuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	TerraEngine::UUID StringToUuid(const std::string& str) {
		TerraEngine::UUID uuid;
		if (str.length() != uuid.size() * 2) return TerraEngine::EMPTY_UUID;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (size_t i = 0; i < uuid.size(); ++i) {
			uuid[i] = static_cast<std::uint8_t>(std::stoul(str.substr(i * 2, 2), nullptr, 16));
		}
		return uuid;
	}

	std::wstring QuoteCommandLineArgument(const std::filesystem::path& value) {
		std::wstring text = value.wstring();
		std::wstring quoted = L"\"";
		for (wchar_t c : text) {
			if (c == L'"') {
				quoted += L"\\\"";
			} else {
				quoted.push_back(c);
			}
		}
		quoted += L"\"";
		return quoted;
	}

	std::wstring QuoteCommandLineArgument(const std::string& value) {
		return QuoteCommandLineArgument(std::filesystem::path(value));
	}

	std::wstring QuoteCommandLineArgument(const char* value) {
		return QuoteCommandLineArgument(std::string(value != nullptr ? value : ""));
	}

	std::filesystem::path GetEditorExecutableDirectory() {
#ifdef _WIN32
		std::wstring buffer(MAX_PATH, L'\0');
		DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		while (copied == buffer.size()) {
			buffer.resize(buffer.size() * 2u, L'\0');
			copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		}
		if (copied == 0u) {
			return std::filesystem::current_path();
		}
		buffer.resize(copied);
		return std::filesystem::path(buffer).parent_path();
#else
		return std::filesystem::current_path();
#endif
	}


	// ReadScriptMetadataString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string ReadScriptMetadataString(const AssetMetadata& metadata, const char* key) {
		const auto settingIt = metadata.settings.find(key);
		if (settingIt == metadata.settings.end() || !settingIt->is_string()) {
			return {};
		}

		return settingIt->get<std::string>();
	}

	// Import sidecars are editor bookkeeping written next to source assets. They
	// must remain readable for MCP/package diagnostics, but they are not user
	// assets and should not receive their own importer, GUID, or browser entry.
	bool IsImportSidecarPath(const std::filesystem::path& path) {
		const std::string filename = path.filename().string();
		return filename.size() >= std::string(".import.json").size()
			&& filename.compare(filename.size() - std::string(".import.json").size(), std::string(".import.json").size(), ".import.json") == 0;
	}

	bool IsMotionDatabaseAuthoringSidecarPath(const std::filesystem::path& path) {
		// Motion-database authoring JSON is a durable editor document paired with
		// its cooked asset. Importing it as a generic project asset can race its
		// exact-byte save token and also exposes bookkeeping in the Project Browser.
		const std::string filename = path.filename().string();
		constexpr std::string_view suffix = ".temotiondb.authoring.json";
		return filename.size() >= suffix.size()
			&& filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool IsGeneratedAnimationPackageBinary(const std::filesystem::path& path) {
		const std::string extension = LowerPath(path.extension());
		if(extension != ".teskel" && extension != ".teanim") {
			return false;
		}
		// Skeletons and clips below a package-owned Generated directory are exposed
		// through its manifest. Treating them as loose assets would create editable
		// .meta files and a second GUID for the same runtime payload.
		for(const std::filesystem::path& part : path.parent_path()) {
			const std::string name = part.string();
			if(name.size() >= std::string("_Generated").size()
				&& name.ends_with("_Generated")) {
				return true;
			}
		}
		return false;
	}

	bool IsModelPackageTransactionPath(const std::filesystem::path& path) {
		// ModelPackageImporter writes package candidates below a reserved
		// `.ta-<16 lowercase hex digits>` Assets directory. Those files are one
		// unpublished transaction, not independently authored assets. Ignoring only
		// the exact reserved shape keeps ordinary dot-folders discoverable while
		// preventing the watcher from racing package publication or assigning
		// metadata to private material/prefab candidates.
		for(const std::filesystem::path& part : path) {
			const std::string name = part.string();
			if(name.size() != 20U || !name.starts_with(".ta-")) {
				continue;
			}
			const bool isLowerHexToken = std::all_of(name.begin() + 4, name.end(), [](char value) {
				return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
			});
			if(isLowerHexToken) {
				return true;
			}
		}
		return false;
	}

	bool IsIgnoredAssetDatabasePath(const std::filesystem::path& path) {
		// The subtree next to a terrain root contains private manifests and blobs.
		// Neither the watcher nor scan may create sidecars inside that transaction.
		for(const std::filesystem::path& part : path) {
			if(LowerPath(part.generic_string()).ends_with(".teterrain.data")) return true;
		}
		if(IsModelPackageTransactionPath(path)) {
			return true;
		}
		if (path.extension() == ".meta") {
			std::filesystem::path sourcePath = path;
			sourcePath.replace_extension("");
			return IsImportSidecarPath(sourcePath)
				|| IsMotionDatabaseAuthoringSidecarPath(sourcePath)
				|| IsGeneratedAnimationPackageBinary(sourcePath);
		}
		return IsImportSidecarPath(path)
			|| IsMotionDatabaseAuthoringSidecarPath(path)
			|| IsGeneratedAnimationPackageBinary(path);
	}

	bool IsTrackedSourceAssetPath(const std::filesystem::path& path) {
		return path.extension() != ".meta" && !IsIgnoredAssetDatabasePath(path);
	}

	bool IsModelPackageCompileOutputReady(
		const AssetMetadata& metadata,
		const std::filesystem::path& absSourcePath,
		const std::filesystem::path& assetRoot,
		const std::filesystem::path& buildRoot)
	{
		TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
		if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult))
		{
			return false;
		}
		if (importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed || importResult.generatedMeshes.empty())
		{
			return false;
		}
		if (importResult.packageGuid != metadata.guid)
		{
			return false;
		}
		const std::string_view expectedImporterVersion =
			TerraEngine::AssetPipeline::GetModelPackageImporterVersion(absSourcePath);
		if (expectedImporterVersion.empty()
			|| importResult.animationManifest.importerVersion != expectedImporterVersion)
		{
			// Source timestamps cannot detect importer-only behavior changes. Treat
			// an older recipe as incomplete so the normal startup import transaction
			// regenerates every package child before registering it.
			return false;
		}

		// Model-package parents are containers. The package compiler publishes a
		// sidecar plus generated child meshes instead of the obsolete parent TEMF,
		// so startup dirtiness must be based on the child render outputs.
		for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes)
		{
			if (mesh.compiledPath.empty() || !std::filesystem::exists(buildRoot / mesh.compiledPath))
			{
				return false;
			}
		}

		// Generated prefabs are source documents under Assets. If the sidecar says
		// one exists, keep the package dirty until the document is present so the
		// Project Browser never advertises a missing drag source.
		for (const TerraEngine::AssetPipeline::GeneratedModelPrefabAsset& prefab : importResult.generatedPrefabs)
		{
			if (!prefab.sourcePath.empty() && !std::filesystem::exists(assetRoot / prefab.sourcePath))
			{
				return false;
			}
		}

		return true;
	}

	bool IsAssetCompileOutputReady(
		const AssetMetadata& metadata,
		const std::filesystem::path& absSourcePath,
		const std::filesystem::path& assetRoot,
		const std::filesystem::path& buildRoot)
	{
		if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
		{
			return IsModelPackageCompileOutputReady(metadata, absSourcePath, assetRoot, buildRoot);
		}
		if (metadata.compiledPath.empty())
		{
			return true;
		}
		return std::filesystem::exists(buildRoot / metadata.compiledPath);
	}

	bool PublishCompiledAssetAtomically(const std::filesystem::path& stagedPath, const std::filesystem::path& finalPath)
	{
		std::error_code ec;
		std::filesystem::create_directories(finalPath.parent_path(), ec);
		if (ec)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create compiled asset directory " << finalPath.parent_path() << ": " << ec.message());
			return false;
		}

		if (!std::filesystem::exists(stagedPath))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Compiled asset staging file is missing: " << stagedPath);
			return false;
		}

		const std::filesystem::path backupPath = finalPath.string() + ".previous";
		std::filesystem::remove(backupPath, ec);
		ec.clear();

		const bool hadPreviousOutput = std::filesystem::exists(finalPath);
		if (hadPreviousOutput)
		{
			std::filesystem::rename(finalPath, backupPath, ec);
			if (ec)
			{
				TERRAENGINE_LOG(LogLevel::Error, "Failed to preserve previous compiled asset before publish: " << finalPath << " (" << ec.message() << ")");
				return false;
			}
		}

		std::filesystem::rename(stagedPath, finalPath, ec);
		if (ec)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to publish staged compiled asset " << stagedPath << " to " << finalPath << ": " << ec.message());
			if (hadPreviousOutput)
			{
				std::error_code restoreError;
				std::filesystem::rename(backupPath, finalPath, restoreError);
				if (restoreError)
				{
					TERRAENGINE_LOG(LogLevel::Error, "Failed to restore previous compiled asset " << finalPath << ": " << restoreError.message());
				}
			}
			return false;
		}

		if (hadPreviousOutput)
		{
			std::filesystem::remove(backupPath, ec);
			if (ec)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Published compiled asset but could not remove backup " << backupPath << ": " << ec.message());
			}
		}
		return true;
	}

	std::optional<AssetType> RuntimeAssetTypeForGeneratedKind(GeneratedModelPackageChildKind kind)
	{
		switch (kind)
		{
		case GeneratedModelPackageChildKind::Mesh:
		case GeneratedModelPackageChildKind::DeformableMesh:
			return AssetType::Mesh;
		case GeneratedModelPackageChildKind::Material:
			return AssetType::Material;
		case GeneratedModelPackageChildKind::Texture:
			return AssetType::Texture;
		case GeneratedModelPackageChildKind::Skeleton:
			return AssetType::Skeleton;
		case GeneratedModelPackageChildKind::AnimationClip:
			return AssetType::AnimationClip;
		case GeneratedModelPackageChildKind::MorphTarget:
		case GeneratedModelPackageChildKind::Prefab:
			return std::nullopt;
		}
		return std::nullopt;
	}

	std::optional<AssetType> RuntimeAssetTypeForCookedAnimationImporter(std::string_view importerName)
	{
		// Cooked animation files are copied as immutable runtime artifacts. Keep
		// this mapping in the AssetDatabase so watcher, delete, and registration
		// paths cannot accidentally disagree about their stable resource type.
		if (importerName == "SkeletonAssetImporter") return AssetType::Skeleton;
		if (importerName == "AnimationClipAssetImporter") return AssetType::AnimationClip;
		if (importerName == "AnimationControllerAssetImporter") return AssetType::AnimationController;
		if (importerName == "AnimationMaskAssetImporter") return AssetType::AnimationMask;
		if (importerName == "RetargetProfileAssetImporter") return AssetType::RetargetProfile;
		if (importerName == "MotionDatabaseAssetImporter") return AssetType::MotionDatabase;
		if (importerName == "RagdollProfileAssetImporter") return AssetType::RagdollProfile;
		return std::nullopt;
	}

	std::optional<TerraEngine::Animation::AnimationReloadAssetType> ReloadAssetTypeForRuntimeType(
		AssetType type)
	{
		using ReloadType = TerraEngine::Animation::AnimationReloadAssetType;
		switch (type)
		{
		case AssetType::Skeleton: return ReloadType::Skeleton;
		case AssetType::AnimationClip: return ReloadType::AnimationClip;
		case AssetType::AnimationController: return ReloadType::AnimationController;
		case AssetType::AnimationMask: return ReloadType::AnimationMask;
		case AssetType::RetargetProfile: return ReloadType::RetargetProfile;
		case AssetType::MotionDatabase: return ReloadType::MotionDatabase;
		case AssetType::RagdollProfile: return ReloadType::RagdollProfile;
		default: return std::nullopt;
		}
	}

	std::optional<TerraEngine::UUID> ReadCookedAnimationAssetId(
		const std::filesystem::path& path)
	{
		TerraEngine::Animation::AnimationAssetEnvelope envelope{};
		std::ifstream input(path, std::ios::binary);
		if (!input.read(reinterpret_cast<char*>(&envelope), sizeof(envelope))
			|| envelope.magic != TerraEngine::Animation::kAnimationAssetMagic
			|| envelope.headerSize != sizeof(envelope)
			|| envelope.endianMarker != TerraEngine::Animation::kAnimationLittleEndianMarker
			|| envelope.assetId.value == TerraEngine::EMPTY_UUID)
		{
			return std::nullopt;
		}
		return envelope.assetId.value;
	}

	std::optional<TerraEngine::UUID> ReadTerrainAssetId(
		const std::filesystem::path& path)
	{
		// The small public terrain descriptor is the identity authority shared by
		// scenes, the private area bundle, and ResourceManager. Adopting that UUID
		// prevents an unrelated generated .meta GUID from breaking scene reloads.
		std::ifstream input(path, std::ios::binary);
		nlohmann::json descriptor;
		try
		{
			if(!input || !(input >> descriptor) || !descriptor.is_object()
				|| descriptor.value("format", std::string{}) != "TerraEngineTerrain"
				|| descriptor.value("rootFormat", std::string{}) != "TERRAROOT1"
				|| descriptor.value("version", 0u) != 1u
				|| !descriptor.contains("assetId") || !descriptor["assetId"].is_string())
				return std::nullopt;
		}
		catch(const nlohmann::json::exception&)
		{
			return std::nullopt;
		}
		const auto assetId = TerraEngine::ParseUUID(descriptor["assetId"].get<std::string>());
		if(!assetId || *assetId == TerraEngine::EMPTY_UUID) return std::nullopt;
		const std::filesystem::path dataDirectory = path.string() + ".data";
		std::ifstream current(dataDirectory / "CURRENT", std::ios::binary);
		std::string rootHash;
		if(!current || !std::getline(current, rootHash) || rootHash.size() != 32U) return std::nullopt;
		std::ifstream rootInput(dataDirectory / "roots" / (rootHash + ".root"), std::ios::binary);
		std::stringstream rootBytes;
		if(rootInput) rootBytes << rootInput.rdbuf();
		const auto decoded = TerraEngine::Terrain::DecodeTerrainRootManifest(rootBytes.str());
		return decoded.Succeeded() && decoded.root && decoded.root->assetUuid == *assetId
			&& TerraEngine::Terrain::ToHexString(decoded.contentHash) == rootHash ? assetId : std::nullopt;
	}

	std::optional<GeneratedModelPackageChildKind> GeneratedKindForManifestKind(
		TerraEngine::AssetPipeline::AnimationImportSubAssetKind kind)
	{
		using ManifestKind = TerraEngine::AssetPipeline::AnimationImportSubAssetKind;
		switch (kind)
		{
		case ManifestKind::Skeleton: return GeneratedModelPackageChildKind::Skeleton;
		case ManifestKind::AnimationClip: return GeneratedModelPackageChildKind::AnimationClip;
		case ManifestKind::DeformableMesh: return GeneratedModelPackageChildKind::DeformableMesh;
		case ManifestKind::MorphTarget: return GeneratedModelPackageChildKind::MorphTarget;
		case ManifestKind::Material: return GeneratedModelPackageChildKind::Material;
		case ManifestKind::Texture: return GeneratedModelPackageChildKind::Texture;
		case ManifestKind::Prefab: return GeneratedModelPackageChildKind::Prefab;
		case ManifestKind::Mesh: return GeneratedModelPackageChildKind::Mesh;
		}
		return std::nullopt;
	}

	std::optional<TerraEngine::AssetPipeline::AnimationImportSubAssetKind> ManifestKindForGeneratedKind(
		GeneratedModelPackageChildKind kind)
	{
		using ManifestKind = TerraEngine::AssetPipeline::AnimationImportSubAssetKind;
		switch (kind)
		{
		case GeneratedModelPackageChildKind::Skeleton: return ManifestKind::Skeleton;
		case GeneratedModelPackageChildKind::AnimationClip: return ManifestKind::AnimationClip;
		case GeneratedModelPackageChildKind::DeformableMesh: return ManifestKind::DeformableMesh;
		case GeneratedModelPackageChildKind::MorphTarget: return ManifestKind::MorphTarget;
		case GeneratedModelPackageChildKind::Material: return ManifestKind::Material;
		case GeneratedModelPackageChildKind::Texture: return ManifestKind::Texture;
		case GeneratedModelPackageChildKind::Prefab: return ManifestKind::Prefab;
		case GeneratedModelPackageChildKind::Mesh: return ManifestKind::Mesh;
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> ResolveGeneratedBuildArtifact(
		const std::filesystem::path& buildRoot,
		const std::filesystem::path& relativePath)
	{
		// Manifest paths are project-relative payload locations. Reject absolute or
		// parent-traversing records before they can become a runtime path alias.
		if (relativePath.empty() || relativePath.is_absolute() || HasParentTraversal(relativePath))
		{
			return std::nullopt;
		}
		const std::filesystem::path root = std::filesystem::absolute(buildRoot).lexically_normal();
		const std::filesystem::path candidate = std::filesystem::absolute(buildRoot / relativePath).lexically_normal();
		if (!PathIsWithin(candidate, root) || !std::filesystem::is_regular_file(candidate))
		{
			return std::nullopt;
		}
		return candidate;
	}

	struct GeneratedPackageResourceAlias
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		GeneratedModelPackageChildKind kind = GeneratedModelPackageChildKind::Mesh;
		std::filesystem::path compiledPath;
	};

	void AppendGeneratedPackageAlias(
		std::vector<GeneratedPackageResourceAlias>& aliases,
		GeneratedPackageResourceAlias candidate)
	{
		const std::optional<AssetType> candidateType = RuntimeAssetTypeForGeneratedKind(candidate.kind);
		if (!candidateType || candidate.guid == TerraEngine::EMPTY_UUID)
		{
			return;
		}
		const auto duplicate = std::find_if(aliases.begin(), aliases.end(), [&](const GeneratedPackageResourceAlias& current) {
			return current.guid == candidate.guid
				&& RuntimeAssetTypeForGeneratedKind(current.kind) == candidateType;
		});
		if (duplicate == aliases.end())
		{
			aliases.push_back(std::move(candidate));
		}
	}

	bool IsPublishedAnimationManifest(
		const TerraEngine::AssetPipeline::ModelPackageImportResult& importResult)
	{
		const TerraEngine::AssetPipeline::AnimationImportManifest& manifest = importResult.animationManifest;
		if(importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed
			|| importResult.packageGuid == TerraEngine::EMPTY_UUID
			|| manifest.packageGuid != importResult.packageGuid
			|| manifest.cancelled
			|| !manifest.publishedAtomically
			|| manifest.publishedGeneration == 0u
			|| manifest.candidateGeneration != manifest.publishedGeneration)
		{
			return false;
		}

		// Recomputing the dependency fingerprint makes registration fail closed if
		// a sidecar was partially edited or copied without its matching children.
		return !manifest.dependencyFingerprintHex.empty()
			&& manifest.dependencyFingerprintHex
				== TerraEngine::AssetPipeline::BuildAnimationImportDependencyFingerprint(manifest)
			&& TerraEngine::AssetPipeline::ValidateAnimationImportManifestDependencies(manifest);
	}

	std::optional<std::filesystem::path> ResolveGeneratedSourceArtifact(
		const std::filesystem::path& assetRoot,
		const std::filesystem::path& relativePath)
	{
		// Generated source documents are package-owned, but they still must remain
		// beneath Assets before any inspector-side validation opens them.
		if(relativePath.empty() || relativePath.is_absolute() || HasParentTraversal(relativePath))
		{
			return std::nullopt;
		}
		const std::filesystem::path root = std::filesystem::absolute(assetRoot).lexically_normal();
		const std::filesystem::path candidate = std::filesystem::absolute(assetRoot / relativePath).lexically_normal();
		if(!PathIsWithin(candidate, root) || !std::filesystem::is_regular_file(candidate))
		{
			return std::nullopt;
		}
		return candidate;
	}

	constexpr std::size_t kMaximumAnimationSummaryDiagnostics = 64u;
	constexpr std::size_t kMaximumAnimationSummaryDiagnosticCharacters = 512u;

	void AppendAnimationSummaryDiagnostic(
		ModelPackageAnimationSummaryMetadata& summary,
		std::string diagnostic)
	{
		// Inspector and automation responses are intentionally bounded. Import logs
		// retain full details, while this summary stays safe to display or serialize.
		if(summary.diagnostics.size() >= kMaximumAnimationSummaryDiagnostics)
		{
			return;
		}
		if(diagnostic.size() > kMaximumAnimationSummaryDiagnosticCharacters)
		{
			diagnostic.resize(kMaximumAnimationSummaryDiagnosticCharacters - 3u);
			diagnostic += "...";
		}
		summary.diagnostics.push_back(std::move(diagnostic));
	}

	bool ContainsUuid(const std::vector<TerraEngine::UUID>& values, const TerraEngine::UUID& value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	bool AreClose(float lhs, float rhs)
	{
		return std::abs(lhs - rhs) <= 0.0001f;
	}

	bool AreRatiosEqual(const std::vector<float>& lhs, const std::vector<float>& rhs)
	{
		if (lhs.size() != rhs.size())
		{
			return false;
		}
		for (std::size_t index = 0; index < lhs.size(); ++index)
		{
			if (!AreClose(lhs[index], rhs[index]))
			{
				return false;
			}
		}
		return true;
	}

	std::string GeneratedChildNameFallback(
		const std::string& displayName,
		const std::filesystem::path& sourceNodePath,
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& compiledPath,
		const TerraEngine::UUID& guid)
	{
		if (!displayName.empty())
		{
			return displayName;
		}
		if (!sourceNodePath.empty())
		{
			const std::filesystem::path nodeName = sourceNodePath.filename();
			if (!nodeName.empty())
			{
				return nodeName.string();
			}
			return sourceNodePath.string();
		}
		if (!sourcePath.empty())
		{
			const std::filesystem::path sourceName = sourcePath.stem();
			if (!sourceName.empty())
			{
				return sourceName.string();
			}
		}
		if (!compiledPath.empty())
		{
			const std::filesystem::path compiledName = compiledPath.stem();
			if (!compiledName.empty())
			{
				return compiledName.string();
			}
		}
		return TerraEngine::ToHexString(guid);
	}

	std::vector<std::string> ValidateVirtualGeometryAdaptiveSettings(const VirtualGeometryAdaptiveSettingsMetadata& settings)
	{
		std::vector<std::string> errors;
		if (settings.targetRatios.empty())
		{
			errors.push_back("At least one target ratio is required.");
		}

		float previousRatio = 2.0f;
		for (std::size_t index = 0; index < settings.targetRatios.size(); ++index)
		{
			const float ratio = settings.targetRatios[index];
			if (!std::isfinite(ratio) || ratio <= 0.0f || ratio > 1.0f)
			{
				errors.push_back("Target ratios must be finite values in the range (0, 1].");
				break;
			}
			if (index > 0u && ratio >= previousRatio)
			{
				errors.push_back("Target ratios must be strictly descending.");
				break;
			}
			previousRatio = ratio;
		}

		if (settings.targetRatios.empty() || !AreClose(settings.targetRatios.front(), 1.0f))
		{
			errors.push_back("The first target ratio must be 1.0 so LOD0 preserves the source detail.");
		}
		if (settings.minimumSourceTriangles == 0u)
		{
			errors.push_back("Minimum source triangles must be greater than zero.");
		}
		if (settings.minimumGeneratedTriangles == 0u)
		{
			errors.push_back("Minimum generated triangles must be greater than zero.");
		}
		if (settings.minimumMeshletsPerGeneratedLevel == 0u)
		{
			errors.push_back("Minimum meshlets per generated level must be greater than zero.");
		}
		if (!std::isfinite(settings.minimumUsefulTriangleReduction)
			|| settings.minimumUsefulTriangleReduction < 0.0f
			|| settings.minimumUsefulTriangleReduction > 0.95f)
		{
			errors.push_back("Minimum useful triangle reduction must be between 0 and 0.95.");
		}
		if (!std::isfinite(settings.minimumUsefulMeshletReduction)
			|| settings.minimumUsefulMeshletReduction < 0.0f
			|| settings.minimumUsefulMeshletReduction > 0.95f)
		{
			errors.push_back("Minimum useful meshlet reduction must be between 0 and 0.95.");
		}
		if (!std::isfinite(settings.maximumGeometricError) || settings.maximumGeometricError < 0.0f)
		{
			errors.push_back("Maximum geometric error must be finite and non-negative.");
		}
		return errors;
	}

	json BuildVirtualGeometryOverrideJson(
		const VirtualGeometryAdaptiveSettingsMetadata& settings,
		const VirtualGeometryAdaptiveSettingsMetadata& defaults)
	{
		json overrideBlock = json::object();
		json overriddenFields = json::array();
		overrideBlock["enabled"] = true;
		overrideBlock["settingsSource"] = "PerMeshOverride";

		if (!AreRatiosEqual(settings.targetRatios, defaults.targetRatios))
		{
			json ratios = json::array();
			for (float ratio : settings.targetRatios)
			{
				ratios.push_back(ratio);
			}
			overrideBlock["targetRatios"] = ratios;
			overriddenFields.push_back("targetRatios");
		}
		if (settings.minimumSourceTriangles != defaults.minimumSourceTriangles)
		{
			overrideBlock["minimumSourceTriangles"] = settings.minimumSourceTriangles;
			overriddenFields.push_back("minimumSourceTriangles");
		}
		if (settings.minimumGeneratedTriangles != defaults.minimumGeneratedTriangles)
		{
			overrideBlock["minimumGeneratedTriangles"] = settings.minimumGeneratedTriangles;
			overriddenFields.push_back("minimumGeneratedTriangles");
		}
		if (settings.minimumMeshletsPerGeneratedLevel != defaults.minimumMeshletsPerGeneratedLevel)
		{
			overrideBlock["minimumMeshletsPerGeneratedLevel"] = settings.minimumMeshletsPerGeneratedLevel;
			overriddenFields.push_back("minimumMeshletsPerGeneratedLevel");
		}
		if (!AreClose(settings.minimumUsefulTriangleReduction, defaults.minimumUsefulTriangleReduction))
		{
			overrideBlock["minimumUsefulTriangleReduction"] = settings.minimumUsefulTriangleReduction;
			overriddenFields.push_back("minimumUsefulTriangleReduction");
		}
		if (!AreClose(settings.minimumUsefulMeshletReduction, defaults.minimumUsefulMeshletReduction))
		{
			overrideBlock["minimumUsefulMeshletReduction"] = settings.minimumUsefulMeshletReduction;
			overriddenFields.push_back("minimumUsefulMeshletReduction");
		}
		if (!AreClose(settings.maximumGeometricError, defaults.maximumGeometricError))
		{
			overrideBlock["maximumGeometricError"] = settings.maximumGeometricError;
			overriddenFields.push_back("maximumGeometricError");
		}

		overrideBlock["overriddenFields"] = overriddenFields;
		return overrideBlock;
	}

	void ApplyVirtualGeometryOverrideJson(const json& overrideBlock, VirtualGeometryAdaptiveSettingsMetadata& settings)
	{
		if (!overrideBlock.is_object() || !overrideBlock.value("enabled", true))
		{
			return;
		}

		settings.overridePresent = true;
		settings.settingsSource = "PerMeshOverride";
		if (overrideBlock.contains("targetRatios") && overrideBlock["targetRatios"].is_array())
		{
			settings.targetRatios.clear();
			for (const json& value : overrideBlock["targetRatios"])
			{
				if (value.is_number())
				{
					settings.targetRatios.push_back(value.get<float>());
				}
			}
		}
		settings.minimumSourceTriangles = overrideBlock.value("minimumSourceTriangles", settings.minimumSourceTriangles);
		settings.minimumGeneratedTriangles = overrideBlock.value("minimumGeneratedTriangles", settings.minimumGeneratedTriangles);
		settings.minimumMeshletsPerGeneratedLevel = overrideBlock.value("minimumMeshletsPerGeneratedLevel", settings.minimumMeshletsPerGeneratedLevel);
		settings.minimumUsefulTriangleReduction = overrideBlock.value("minimumUsefulTriangleReduction", settings.minimumUsefulTriangleReduction);
		settings.minimumUsefulMeshletReduction = overrideBlock.value("minimumUsefulMeshletReduction", settings.minimumUsefulMeshletReduction);
		settings.maximumGeometricError = overrideBlock.value("maximumGeometricError", settings.maximumGeometricError);
	}

	TerraEngine::AssetPipeline::GeneratedModelMeshVirtualGeometrySettings ToGeneratedVirtualGeometrySettings(const VirtualGeometryAdaptiveSettingsMetadata& settings, bool hasOverride)
	{
		TerraEngine::AssetPipeline::GeneratedModelMeshVirtualGeometrySettings generatedSettings;
		generatedSettings.hasOverride = hasOverride;
		generatedSettings.targetRatios = hasOverride ? settings.targetRatios : std::vector<float>{};
		generatedSettings.minimumSourceTriangles = settings.minimumSourceTriangles;
		generatedSettings.minimumGeneratedTriangles = settings.minimumGeneratedTriangles;
		generatedSettings.minimumMeshletsPerGeneratedLevel = settings.minimumMeshletsPerGeneratedLevel;
		generatedSettings.minimumUsefulTriangleReduction = settings.minimumUsefulTriangleReduction;
		generatedSettings.minimumUsefulMeshletReduction = settings.minimumUsefulMeshletReduction;
		generatedSettings.maximumGeometricError = settings.maximumGeometricError;
		generatedSettings.settingsSource = hasOverride ? "PerMeshOverride" : "ProjectDefaults";
		return generatedSettings;
	}

	void ApplyGeneratedVirtualGeometrySettings(
		const TerraEngine::AssetPipeline::GeneratedModelMeshVirtualGeometrySettings& generatedSettings,
		VirtualGeometryAdaptiveSettingsMetadata& settings)
	{
		if (!generatedSettings.hasOverride)
		{
			return;
		}
		settings.overridePresent = true;
		settings.settingsSource = "PerMeshOverride";
		if (!generatedSettings.targetRatios.empty())
		{
			settings.targetRatios = generatedSettings.targetRatios;
		}
		settings.minimumSourceTriangles = generatedSettings.minimumSourceTriangles;
		settings.minimumGeneratedTriangles = generatedSettings.minimumGeneratedTriangles;
		settings.minimumMeshletsPerGeneratedLevel = generatedSettings.minimumMeshletsPerGeneratedLevel;
		settings.minimumUsefulTriangleReduction = generatedSettings.minimumUsefulTriangleReduction;
		settings.minimumUsefulMeshletReduction = generatedSettings.minimumUsefulMeshletReduction;
		settings.maximumGeometricError = generatedSettings.maximumGeometricError;
	}

	VirtualGeometryAdaptiveCompileSettings ToCompileVirtualGeometrySettings(const VirtualGeometryAdaptiveSettingsMetadata& settings)
	{
		VirtualGeometryAdaptiveCompileSettings compileSettings;
		compileSettings.targetRatios = settings.targetRatios;
		compileSettings.minimumSourceTriangles = settings.minimumSourceTriangles;
		compileSettings.minimumGeneratedTriangles = settings.minimumGeneratedTriangles;
		compileSettings.minimumMeshletsPerGeneratedLevel = settings.minimumMeshletsPerGeneratedLevel;
		compileSettings.minimumUsefulTriangleReduction = settings.minimumUsefulTriangleReduction;
		compileSettings.minimumUsefulMeshletReduction = settings.minimumUsefulMeshletReduction;
		compileSettings.maximumGeometricError = settings.maximumGeometricError;
		compileSettings.settingsSource = settings.overridePresent ? "PerMeshOverride" : settings.settingsSource;
		return compileSettings;
	}

	std::string JoinValidationErrors(const std::vector<std::string>& errors)
	{
		std::string message;
		for (std::size_t index = 0; index < errors.size(); ++index)
		{
			if (index != 0u)
			{
				message += " ";
			}
			message += errors[index];
		}
		return message;
	}

	const char* ToVirtualGeometryTargetStatusText(std::uint32_t status)
	{
		switch (static_cast<VirtualGeometryDetailTargetRecordStatus>(status))
		{
		case VirtualGeometryDetailTargetRecordStatus::Requested: return "Requested";
		case VirtualGeometryDetailTargetRecordStatus::Generated: return "Generated";
		case VirtualGeometryDetailTargetRecordStatus::Merged: return "Merged";
		case VirtualGeometryDetailTargetRecordStatus::Skipped: return "Skipped";
		case VirtualGeometryDetailTargetRecordStatus::Rejected: return "Rejected";
		}
		return "Unknown";
	}

	const char* ToVirtualGeometrySkippedReasonText(std::uint32_t reason)
	{
		switch (static_cast<VirtualGeometrySkippedDetailRecordReason>(reason))
		{
		case VirtualGeometrySkippedDetailRecordReason::None: return "";
		case VirtualGeometrySkippedDetailRecordReason::MeshTooSmall: return "MeshTooSmall";
		case VirtualGeometrySkippedDetailRecordReason::TooFewTriangles: return "TooFewTriangles";
		case VirtualGeometrySkippedDetailRecordReason::TooFewMeshlets: return "TooFewMeshlets";
		case VirtualGeometrySkippedDetailRecordReason::InsufficientTriangleReduction: return "InsufficientTriangleReduction";
		case VirtualGeometrySkippedDetailRecordReason::InsufficientMeshletReduction: return "InsufficientMeshletReduction";
		case VirtualGeometrySkippedDetailRecordReason::InvalidSimplificationOutput: return "InvalidSimplificationOutput";
		case VirtualGeometrySkippedDetailRecordReason::MaterialSafetyRejected: return "MaterialSafetyRejected";
		case VirtualGeometrySkippedDetailRecordReason::GeometricErrorRejected: return "GeometricErrorRejected";
		case VirtualGeometrySkippedDetailRecordReason::DuplicateOfPreviousLevel: return "DuplicateOfPreviousLevel";
		}
		return "Unknown";
	}

	std::string ReadVirtualGeometryString(const std::string& stringTable, std::uint32_t offset, std::uint32_t size)
	{
		if (size == 0u || offset >= stringTable.size())
		{
			return {};
		}
		const std::size_t readableSize = std::min<std::size_t>(size, stringTable.size() - offset);
		return std::string(stringTable.data() + offset, readableSize);
	}

}

// AssetDatabase::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
AssetDatabase& AssetDatabase::Instance() {
	static AssetDatabase instance;
	return instance;
}

AssetDatabase::AssetDatabase() {}
AssetDatabase::~AssetDatabase() {}

#ifdef TERRAENGINE_EDITOR_TESTS
void AssetDatabase::SetAsyncImportCompileHookForTests(AsyncImportCompileHookForTests hook) {
	// The worker copies the hook while holding this mutex, then invokes the copy
	// without the mutex so a blocking test hook cannot prevent later cleanup.
	std::lock_guard<std::mutex> lock(asyncImportCompileHookMutex);
	asyncImportCompileHookForTests = std::move(hook);
}

void AssetDatabase::SetAnimationPackagePostPublisherPrepareHookForTests(
	AnimationPackagePostPublisherPrepareHookForTests hook) {
	std::lock_guard<std::mutex> lock(animationPackageTestHookMutex);
	animationPackagePostPublisherPrepareHookForTests = std::move(hook);
}

std::optional<TerraEngine::Animation::AnimationReloadGenerationView>
AssetDatabase::GetAnimationReloadGenerationForTests(const TerraEngine::UUID& assetId) const {
	return animationHotReloadPublisher.QueryGeneration(assetId);
}
#endif

// AssetDatabase::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Initialize(const std::filesystem::path& projectRootPath) {
	// Retire and physically settle the previous owner before changing roots used
	// by compilers or scans. A fresh project receives independent scan authority.
	Shutdown();
	++discovery.projectGeneration;
	discoveryOpen = true;
	discovery.requestedGeneration = 1U;
	discovery.scanningGeneration = discovery.completedGeneration = 0U;
	discovery.pending = true;
	discovery.lastFailure.clear();
	// A project boundary invalidates prior dependency observations before the new
	// index begins publishing its initial collider and material registrations.
	TerraEngine::Physics::PhysicsAssetChangeJournal::Instance().BeginSourceEpoch();
	this->projectRoot = std::filesystem::absolute(projectRootPath);
	this->assetRoot = this->projectRoot / "Assets";
	this->buildRoot = this->projectRoot / "Build" / "Assets";
	this->assetIndexPath = this->projectRoot / "Library" / "AssetIndex.json";
	{
		std::lock_guard lifecycle(importLifecycleMutex);
		++importProjectGeneration;
		importAdmissionOpen = true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(assetRoot)) std::filesystem::create_directories(assetRoot);
	if (!std::filesystem::exists(buildRoot)) std::filesystem::create_directories(buildRoot);
	if (!std::filesystem::exists(assetIndexPath.parent_path())) std::filesystem::create_directories(assetIndexPath.parent_path());

	assets.clear();
	generatedModelPackageResources.clear();
	{
		// Material observations are project-local. Reset them on initialization so
		// a newly opened project never interprets a previous project's deletion as
		// a fallback condition for its own terrain layer UUIDs.
		std::lock_guard<std::mutex> lock(terrainMaterialChangeMutex);
		terrainMaterialChanges.clear();
		nextTerrainMaterialChangeGeneration = 0u;
	}
	// Initialize is a protected project boundary. Publisher retains from the
	// previous project are no longer valid, so reclaim their real cache payloads
	// before Reset discards the only publisher-generation retirement records.
	ResourceManager::Instance().ReleaseAllRetiredAnimationAssetPayloadsAtProtectedBoundary();
	animationHotReloadPublisher.Reset();
#ifdef TERRAENGINE_EDITOR_TESTS
	SetAnimationPackagePostPublisherPrepareHookForTests({});
#endif
	{
		std::lock_guard<std::mutex> lock(animationReimportStatusMutex);
		animationReimportStatuses.clear();
		nextAnimationReimportStatusSequence = 0u;
	}
	animationReloadBoundaryToRetireNextUpdate = 0u;
	loadingAssetIndex = false;
	assetIndexSaveDeferred = false;
	recentlyRemovedAssets.clear();
	pendingReconcileImports.clear();
	pendingReconcileRemovals.clear();
	pendingReconcileOrphanedMetas.clear();
	if (projectReconcileFuture.valid()) {
		projectReconcileFuture.wait();
		(void)projectReconcileFuture.get();
	}
	TERRAENGINE_LOG(LogLevel::Info, "AssetDatabase initialized. Watching: " << assetRoot);
	
	// Load the last known asset table first so large projects can open with
	// usable browser/resource state before the slower filesystem reconciliation
	// walks every source file. Projects without an index also reconcile in the
	// background; the browser can show filesystem tiles immediately while compile
	// jobs publish per-asset progress instead of holding the user on the hub.
	if (LoadAssetIndex()) {
		StartBackgroundProjectReconcile();
	} else {
		StartBackgroundProjectReconcile();
	}
	
	// Start Watcher
	fileWatcher.Start(assetRoot);
}

// AssetDatabase::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Shutdown() {
	{
		// Closing authority precedes cancellation and every physical wait. Worker
		// publication acquires this gate before its short per-request arbiter.
		std::lock_guard lifecycle(importLifecycleMutex);
		importAdmissionOpen = false;
		++importProjectGeneration;
	}
	discoveryOpen = false;
	++discovery.projectGeneration;
	// Project close revokes future publication without deleting authored content.
	// Cancel unfinished work, preserve monotonic explicit-discard intent, and
	// physically settle every compiler before roots or executor ownership change.
	fileWatcher.Stop();
	RequestCancelAllImports(false);
	DrainAllImportsAndWait();
	importExecutor.reset(); // All bool futures/processes settled before pool destruction.
	if (projectReconcileFuture.valid()) {
		projectReconcileFuture.wait();
		try { (void)projectReconcileFuture.get(); } catch (...) { ++discovery.failedScans; }
		++discovery.retiredScans;
	}
	discovery.scanning = discovery.pending = false;
	applyingReconcileGeneration = 0U;
	pendingEvents.clear();
	assets.clear();
	generatedModelPackageResources.clear();
	// Draining all imports completes the editor's protected shutdown boundary.
	// Reclaim old cache generations before resetting publisher bookkeeping.
	ResourceManager::Instance().ReleaseAllRetiredAnimationAssetPayloadsAtProtectedBoundary();
	animationHotReloadPublisher.Reset();
#ifdef TERRAENGINE_EDITOR_TESTS
	SetAnimationPackagePostPublisherPrepareHookForTests({});
#endif
	animationReloadBoundaryToRetireNextUpdate = 0u;
	deferredImports.clear();
	deferredImportMetadata.clear();
	pendingReconcileImports.clear();
	pendingReconcileRemovals.clear();
	pendingReconcileOrphanedMetas.clear();
	recentlyRemovedAssets.clear();
}

// AssetDatabase::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Update() {
	// One protected application can register/remove many records. Persist its
	// complete catalog once, rather than rewriting the full index per record.
	++assetIndexSaveBatchDepth;
	struct IndexSaveBoundary {
		AssetDatabase& owner;
		~IndexSaveBoundary() noexcept { owner.EndAssetIndexSaveBatch(); }
	} indexSaveBoundary{*this};
	// The prior update boundary is now complete. The publisher still refuses to
	// retire generations held by evaluator/GPU/managed references.
	if (animationReloadBoundaryToRetireNextUpdate != 0u)
	{
		animationHotReloadPublisher.RetireThroughBoundary(
			animationReloadBoundaryToRetireNextUpdate, RetireAnimationResourcePayload);
		// Retained frame/GPU/managed generations are deliberately skipped. Keep the
		// completed boundary scheduled until the bounded publisher queue is empty so
		// their later ReleaseGeneration becomes observable without another reload.
		if (!animationHotReloadPublisher.HasPendingRetirements())
		{
			animationReloadBoundaryToRetireNextUpdate = 0u;
		}
	}
	PruneRecentlyRemovedAssets();
	// Loss is consumed before scan publication so a newer demand cannot be
	// mistaken for the demand satisfied by an already-running scan.
	if (discoveryOpen && fileWatcher.ConsumeReconcileRequired()) {
		++discovery.requestedGeneration;
		discovery.pending = true;
	}
	PublishBackgroundProjectReconcile();
	ProcessPendingProjectReconcile();

	// Process Watcher Events
	auto events = fileWatcher.PopEvents();
	if (discoveryOpen && !events.empty() && (discovery.scanning || applyingReconcileGeneration != 0U)) {
		++discovery.requestedGeneration;
		discovery.pending = true;
	}
	for (const auto& e : events) {
		// Debounce: update or add entry in pending
		// Debounce time: 200ms
		pendingEvents[std::filesystem::absolute(e.path)] = { e.status, 0.2f };
	}

	// Process Pending
	// We assume Update is called with some frequency.
	float dt = 0.016f;

	auto it = pendingEvents.begin();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (it != pendingEvents.end()) {
		it->second.timer -= dt;
		if (it->second.timer <= 0.0f) {
			OnFileEvent(it->first, it->second.status);
			it = pendingEvents.erase(it);
		} else {
			++it;
		}
	}

	// Process Async Jobs
	std::vector<std::pair<std::filesystem::path, bool>> readyDeferredImports;
	// Reserve before any completion ownership changes; moving a prepared path
	// into this storage cannot lose a follow-up after its old record is erased.
	readyDeferredImports.reserve(asyncJobs.size());
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto jobIt = asyncJobs.begin(); jobIt != asyncJobs.end(); ) {
		if (jobIt->submissionFailed || (!jobIt->admitted && jobIt->progressState->cancelRequested.load())
			|| (jobIt->future.valid() && jobIt->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
			std::filesystem::path completedPath = jobIt->absSourcePath;
			try { FinalizeAsyncImportJob(*jobIt); }
			catch (...) { ++importAdmission.failed; try { TERRAENGINE_LOG(LogLevel::Error, "Import owner finalization failed; capacity was released."); } catch (...) {} }
			jobIt = asyncJobs.erase(jobIt);
			if (const auto deferredIt = deferredImports.find(completedPath); deferredIt != deferredImports.end()) {
				readyDeferredImports.emplace_back(std::move(completedPath), deferredIt->second);
				deferredImports.erase(deferredIt);
			}
		} else {
			++jobIt;
		}
	}
	for (const auto& [path, force] : readyDeferredImports) {
		if (std::filesystem::exists(path)) {
			ImportAssetWithProgress(path, force);
		} else deferredImportMetadata.erase(path);
	}
	DispatchQueuedImports();
	if (discoveryOpen && discovery.pending) StartBackgroundProjectReconcile();
}

bool AssetDatabase::SetMaxActiveImports(std::size_t limit)
{
	// Configuration belongs to a protected quiescent boundary. Recreating an
	// idle owned executor never changes the independent global frame pool.
	if (limit == 0U || !asyncJobs.empty() || !deferredImports.empty() || !deferredImportMetadata.empty()
		|| importAdmission.active != 0U) return false;
	importExecutor.reset();
	importAdmission.configuredCapacity = limit;
	importAdmission.peakActive = 0U;
	return true;
}

AssetImportAdmissionDiagnostics AssetDatabase::GetImportAdmissionDiagnostics() const
{
	auto snapshot = importAdmission;
	{ std::lock_guard lifecycle(importLifecycleMutex); snapshot.open = importAdmissionOpen; }
	for (const auto& job : asyncJobs) if (!job.admitted && !job.submissionFailed && !job.progressState->cancelRequested.load()) ++snapshot.queued;
	return snapshot;
}

void AssetDatabase::DispatchQueuedImports()
{
	{ std::lock_guard lifecycle(importLifecycleMutex); if (!importAdmissionOpen) return; }
	for (auto& job : asyncJobs) {
		if (importAdmission.active >= importAdmission.configuredCapacity) break;
		if (job.admitted || job.submissionFailed || job.progressState->cancelRequested.load()) continue;
		try {
#ifdef TERRAENGINE_EDITOR_TESTS
			const auto admissionHook = importAdmissionHookForTests;
			job.progressState->beforePublicationHook = admissionHook;
			if (admissionHook) admissionHook(job.absSourcePath, ImportAdmissionBoundaryForTests::ExecutorCreation);
#endif
			if (!importExecutor) importExecutor = ThreadPool::CreateOwned(importAdmission.configuredCapacity);
			// A task captures owned immutable inputs. No callback reads the vector
			// entry after admission, and only its bool future escapes this scope.
			auto task = importExecutor->createTask([this, metadata = job.metadata, path = job.absSourcePath,
				progress = job.progressState, settings = job.resolvedMeshSettings] {
				return RunAsyncImportWork(metadata, path, progress, settings);
			});
			job.future = task->getFuture();
			job.admitted = true;
			++importAdmission.active;
			try {
#ifdef TERRAENGINE_EDITOR_TESTS
				if (admissionHook) admissionHook(job.absSourcePath, ImportAdmissionBoundaryForTests::Enqueue);
#endif
				importExecutor->enqueue(task);
			}
			catch (...) {
				// Failed insertion guarantees the task was never admitted. Discard
				// its pending future instead of waiting for an unsubmitted promise.
				job.future = {}; job.admitted = false; --importAdmission.active;
				throw;
			}
			++importAdmission.submitted;
			importAdmission.peakActive = std::max(importAdmission.peakActive, importAdmission.active);
		} catch (...) {
			job.submissionFailed = true;
			try { SetImportProgress(job.progressState, "Failed", "Import submission failed; reserved capacity was released.", 1.0F, false, true); } catch (...) {}
		}
	}
}

AssetDiscoveryDiagnostics AssetDatabase::GetDiscoveryDiagnostics() const
{
	auto snapshot = discovery;
	snapshot.pendingImports = pendingReconcileImports.size();
	return snapshot;
}
// AssetDatabase::IsAssetCompiling is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::IsCompiling() const {
	for (const AsyncCompileJob& job : asyncJobs) {
		if (!job.progressState || !job.progressState->cancelRequested.load()) {
			return true;
		}
	}
	return false;
}

bool AssetDatabase::IsAssetCompiling(const std::filesystem::path& absSourcePath) const {
	const std::filesystem::path normalizedPath = std::filesystem::absolute(absSourcePath);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& job : asyncJobs) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		// Cancellation revokes publication, but the compiler still owns its staging
		// paths until the retained future physically settles.
		if (job.absSourcePath == normalizedPath) {
			return true;
		}
	}
	return false;
}

size_t AssetDatabase::GetCompileJobsCount() const {
	// Logical cancellation hides UI work but cannot erase physical ownership.
	// This count includes queued and canceled records until finalization settles.
	return asyncJobs.size();
}

bool AssetDatabase::CancelImport(const std::filesystem::path& absSourcePath, bool removeAssetAfterCancel) {
	const std::filesystem::path normalizedPath = std::filesystem::absolute(absSourcePath);
	for (AsyncCompileJob& job : asyncJobs) {
		if (job.absSourcePath == normalizedPath && job.progressState) {
			const AssetImportProgress snapshot = SnapshotImportProgress(job.progressState);
			if (!RequestImportCancellation(job.progressState, removeAssetAfterCancel)) {
				// Atomic publication already won the boundary. Returning false tells
				// the caller that the completed result will be finalized normally.
				return false;
			}
			TerminateCompilerProcess(job.progressState);
			// Discard pre-cancel follow-up intent only here; a later explicit retry
			// must survive the old generation's eventual physical cleanup.
			deferredImports.erase(normalizedPath);
			deferredImportMetadata.erase(normalizedPath);
			SetImportProgress(job.progressState, "Cancelling", "Stopping import and preparing cleanup.", snapshot.progress, false, false);
			CleanupCancelledImport(job.metadata, job.absSourcePath, job.progressState->removeAssetAfterCancel.load());
			return true;
		}
	}
	return false;
}

void AssetDatabase::RequestCancelAllImports(bool removeAssetsAfterCancel) {
	for (AsyncCompileJob& job : asyncJobs) {
		if (job.progressState) {
			if (!RequestImportCancellation(job.progressState, removeAssetsAfterCancel)) {
				// A fully published compiler result is too late to cancel. The drain
				// path below will register it instead of discarding coherent output.
				continue;
			}
			TerminateCompilerProcess(job.progressState);
			// Diagnostics and catalog cleanup may allocate. Neither can prevent
			// later requests from receiving cancellation or the subsequent drain.
			try {
				const auto snapshot = SnapshotImportProgress(job.progressState);
				SetImportProgress(job.progressState, "Cancelling", "Stopping import and preparing cleanup.", snapshot.progress, false, false);
			} catch (...) {}
			try { CleanupCancelledImport(job.metadata, job.absSourcePath, job.progressState->removeAssetAfterCancel.load()); } catch (...) {}
		}
	}
	deferredImports.clear();
	deferredImportMetadata.clear();
}

void AssetDatabase::CancelAllImportsAndWait(bool removeAssetsAfterCancel) {
	RequestCancelAllImports(removeAssetsAfterCancel);
	DrainAllImportsAndWait();
}

void AssetDatabase::DrainAllImportsAndWait() {
	// Only teardown or explicit cancellation waits. Keep every active future
	// until it physically settles, then release its slot at owner finalization.
	// Queued canceled records own no submitted task and therefore never wait.
	while (!asyncJobs.empty()) {
		DispatchQueuedImports();
		// Cancellation/closed admission never starts a queued compiler. Ordinary
		// explicit drains admit eligible FIFO work and settle it on the owner.
		auto& job = asyncJobs.front();
		if (!job.admitted && !job.submissionFailed) job.progressState->cancelRequested.store(true);
		try { FinalizeAsyncImportJob(job); } catch (...) { ++importAdmission.failed; }
		asyncJobs.erase(asyncJobs.begin());
	}
	deferredImports.clear();
	deferredImportMetadata.clear();
}

void AssetDatabase::FinalizeAsyncImportJob(AsyncCompileJob& job)
{
	// A finalizer may allocate metadata or throw. Physical settlement is still
	// required before slot release, and this guard releases that slot exactly once.
	struct SlotRelease { AssetDatabase& database; AsyncCompileJob& job;
		~SlotRelease() noexcept { if (job.admitted) { --database.importAdmission.active; job.admitted = false; } }
	} slot{*this, job};
	bool compiled = false;
	bool workerReturned = false;
	if (job.future.valid())
	{
		// Update calls this only for a ready future. Shutdown and explicit MCP
		// import intentionally wait here because they own a protected boundary.
		job.future.wait();
		try
		{
			compiled = job.future.get();
			workerReturned = true;
		}
		catch (const std::exception& exception)
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Asset import worker ended while finalizing " << job.absSourcePath << ": " << exception.what());
		}
		catch (...)
		{
			TERRAENGINE_LOG(LogLevel::Warning,
				"Asset import worker ended while finalizing with an unknown error: " << job.absSourcePath);
		}
	}

	{ std::lock_guard lifecycle(importLifecycleMutex);
		if (job.progressState->projectGeneration != importProjectGeneration || !importAdmissionOpen) {
			job.progressState->cancelRequested.store(true); ++importAdmission.retired;
		}
	}
	const bool cancelled = job.progressState && job.progressState->cancelRequested.load();
#ifdef TERRAENGINE_EDITOR_TESTS
	const auto finalizationHook = importAdmissionHookForTests;
	if (finalizationHook) finalizationHook(job.absSourcePath, ImportAdmissionBoundaryForTests::Finalization);
#endif
	if (cancelled)
	{
		++importAdmission.canceled;
		if (const std::optional<AssetType> animationType =
			RuntimeAssetTypeForCookedAnimationImporter(job.metadata.importerName))
		{
			RecordAnimationReimportStatus({job.absSourcePath, job.metadata.guid,
				*ReloadAssetTypeForRuntimeType(*animationType), AnimationReimportState::Cancelled,
				0u, ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					job.metadata.guid, *animationType), 0u, 0u, true,
				"Animation reimport was cancelled before main-thread publication."});
		}
		SetImportProgress(job.progressState, "Cancelled", "Cancelled import cleanup is complete.", 1.0f, false, true);
		CleanupCancelledImport(
			job.metadata,
			job.absSourcePath,
			job.progressState->removeAssetAfterCancel.load());
	}
	else if (workerReturned && compiled)
	{
		SetImportProgress(
			job.progressState,
			"Registering",
			"Updating metadata and editor resources on the main thread.",
			0.95f,
			false,
			false);
		FinishImport(job.metadata, job.absSourcePath, job.needsImport);
		SetImportProgress(job.progressState, "Complete", "Import finished.", 1.0f, false, false);
	}
	else
	{
		++importAdmission.failed;
		if (const std::optional<AssetType> animationType =
			RuntimeAssetTypeForCookedAnimationImporter(job.metadata.importerName))
		{
			const std::optional<AnimationReimportStatus> latest =
				GetLatestAnimationReimportStatus(job.absSourcePath);
			if (!latest || latest->state != AnimationReimportState::RetainedLastKnownGood)
			{
				RecordAnimationReimportStatus({job.absSourcePath, job.metadata.guid,
					*ReloadAssetTypeForRuntimeType(*animationType), AnimationReimportState::Failed,
					0u, ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						job.metadata.guid, *animationType), 0u, 0u, true,
					"Animation worker failed; the prior compiled artifact and live generation remain authoritative."});
			}
		}
		SetImportProgress(
			job.progressState,
			"Failed",
			"Compilation failed; the prior published asset was preserved.",
			1.0f,
			false,
			true);
		TERRAENGINE_LOG(LogLevel::Warning, "Asset import did not complete: " << job.absSourcePath);
	}
}

void AssetDatabase::FinalizeActiveImportForExplicitReimport(const std::filesystem::path& sourcePath)
{
	const std::filesystem::path normalizedPath = std::filesystem::absolute(sourcePath);
	const auto active = std::find_if(asyncJobs.begin(), asyncJobs.end(), [&](const AsyncCompileJob& job) {
		return job.absSourcePath == normalizedPath;
	});
	if (active != asyncJobs.end())
	{
		if (!active->admitted) (void)RequestImportCancellation(active->progressState, false);
		// Waiting only for the matching future preserves progress and throughput
		// for unrelated startup/file-watcher jobs in this same editor session.
		FinalizeAsyncImportJob(*active);
		asyncJobs.erase(active);
	}

	// A duplicate request may have arrived while this job was compiling. The
	// explicit synchronous reimport below supersedes that one path only.
	deferredImports.erase(normalizedPath);
	deferredImportMetadata.erase(normalizedPath);
	DispatchQueuedImports();
}

std::vector<AssetImportProgress> AssetDatabase::GetImportProgressSnapshot() const {
	std::vector<AssetImportProgress> snapshot;
	snapshot.reserve(asyncJobs.size());
	for (const AsyncCompileJob& job : asyncJobs) {
		if (job.progressState && job.progressState->cancelRequested.load()) {
			continue;
		}
		snapshot.push_back(SnapshotImportProgress(job.progressState));
	}
	return snapshot;
}

std::optional<AssetImportProgress> AssetDatabase::GetImportProgress(const std::filesystem::path& absSourcePath) const {
	const std::filesystem::path normalizedPath = std::filesystem::absolute(absSourcePath);
	for (const AsyncCompileJob& job : asyncJobs) {
		if (job.absSourcePath == normalizedPath && (!job.progressState || !job.progressState->cancelRequested.load())) {
			return SnapshotImportProgress(job.progressState);
		}
	}
	return std::nullopt;
}

std::vector<AnimationReimportStatus> AssetDatabase::GetAnimationReimportStatusSnapshot() const
{
	std::lock_guard<std::mutex> lock(animationReimportStatusMutex);
	return {animationReimportStatuses.begin(), animationReimportStatuses.end()};
}

std::optional<AnimationReimportStatus> AssetDatabase::GetLatestAnimationReimportStatus(
	const std::filesystem::path& sourcePath) const
{
	std::error_code error;
	const std::filesystem::path normalized = sourcePath.empty()
		? std::filesystem::path{}
		: std::filesystem::absolute(sourcePath, error).lexically_normal();
	std::lock_guard<std::mutex> lock(animationReimportStatusMutex);
	for (auto it = animationReimportStatuses.rbegin(); it != animationReimportStatuses.rend(); ++it)
	{
		if (it->sourcePath == normalized)
		{
			return *it;
		}
	}
	return std::nullopt;
}

void AssetDatabase::RecordAnimationReimportStatus(AnimationReimportStatus status)
{
	if (!status.sourcePath.empty())
	{
		std::error_code error;
		status.sourcePath = std::filesystem::absolute(status.sourcePath, error).lexically_normal();
	}
	std::lock_guard<std::mutex> lock(animationReimportStatusMutex);
	status.sequence = ++nextAnimationReimportStatusSequence;
	animationReimportStatuses.push_back(std::move(status));
	constexpr std::size_t kMaximumAnimationReimportStatuses = 512u;
	while (animationReimportStatuses.size() > kMaximumAnimationReimportStatuses)
	{
		animationReimportStatuses.pop_front();
	}
}

bool AssetDatabase::ShouldCompileAssetAsync(const std::string& importerName) const {
	// Heavy asset compilers run on worker jobs and, on Windows, through child
	// executables. That lets the UI show per-file progress and cancel the
	// compiler process while FinishImport still publishes state on the main thread.
	return importerName == "TextureImporter"
		|| importerName == "MeshImporter"
		|| importerName == "AudioClipImporter"
		|| importerName == "ShaderImporter"
		|| importerName == "PhysicsColliderRecipeImporter"
		|| RuntimeAssetTypeForCookedAnimationImporter(importerName).has_value()
		|| TerraEngine::AssetPipeline::IsModelPackageImporterName(importerName);
}

void AssetDatabase::SetImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState, std::string stage, std::string message, float progress, bool indeterminate, bool failed) const {
	if (!progressState) {
		return;
	}
	if (progressState->cancelRequested.load() && stage != "Cancelling" && stage != "Cancelled") {
		throw AssetImportCancelledException();
	}

	std::lock_guard<std::mutex> lock(progressState->mutex);
	progressState->snapshot.stage = std::move(stage);
	progressState->snapshot.message = std::move(message);
	progressState->snapshot.progress = std::clamp(progress, 0.0f, 1.0f);
	progressState->snapshot.indeterminate = indeterminate;
	progressState->snapshot.failed = failed;
	progressState->snapshot.cancelRequested = progressState->cancelRequested.load();
	progressState->snapshot.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - progressState->startedAt).count();
}

AssetImportProgress AssetDatabase::SnapshotImportProgress(const std::shared_ptr<AsyncImportProgressState>& progressState) const {
	if (!progressState) {
		return {};
	}

	std::lock_guard<std::mutex> lock(progressState->mutex);
	AssetImportProgress snapshot = progressState->snapshot;
	snapshot.cancelRequested = progressState->cancelRequested.load();
	snapshot.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - progressState->startedAt).count();
	return snapshot;
}

void AssetDatabase::TerminateCompilerProcess(const std::shared_ptr<AsyncImportProgressState>& progressState) const {
#ifdef _WIN32
	if (!progressState) {
		return;
	}
	std::lock_guard<std::mutex> lock(progressState->processMutex);
	if (progressState->compilerProcessHandle != nullptr) {
		TerminateProcess(static_cast<HANDLE>(progressState->compilerProcessHandle), 1u);
	}
#else
	if (!progressState) {
		return;
	}
	std::lock_guard<std::mutex> lock(progressState->processMutex);
	if (progressState->compilerProcessHandle != nullptr) {
		const pid_t processId = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(progressState->compilerProcessHandle));
		(void)kill(processId, SIGTERM);
	}
#endif
}

bool AssetDatabase::RequestImportCancellation(
	const std::shared_ptr<AsyncImportProgressState>& progressState,
	bool removeAssetAfterCancel) const {
	if (!progressState) {
		return false;
	}

	std::lock_guard<std::mutex> lock(progressState->publicationMutex);
	if (progressState->publicationCompleted) {
		if (!removeAssetAfterCancel) {
			return false;
		}
		// Explicit discard remains authoritative even if the worker completed
		// its atomic rename just before the user clicked cancel. The cleanup path
		// will remove that complete output together with the authored asset.
		progressState->removeAssetAfterCancel.store(true);
		progressState->cancelRequested.store(true);
		return true;
	}

	// Destructive intent is monotonic. Repeating a non-destructive shutdown or
	// automation cancellation must never downgrade an earlier explicit discard.
	if (removeAssetAfterCancel) {
		progressState->removeAssetAfterCancel.store(true);
	}
	progressState->cancelRequested.store(true);
	return true;
}

bool AssetDatabase::MarkImportPublicationComplete(
	const std::shared_ptr<AsyncImportProgressState>& progressState) const {
	if (!progressState) {
		return true;
	}

	std::lock_guard lifecycle(importLifecycleMutex);
	std::lock_guard<std::mutex> lock(progressState->publicationMutex);
	if (!importAdmissionOpen || progressState->projectGeneration != importProjectGeneration) return false;
	if (progressState->cancelRequested.load()) {
		return false;
	}
	progressState->publicationCompleted = true;
	return true;
}

bool AssetDatabase::PublishCompiledAssetFromWorker(
	const std::filesystem::path& stagedPath,
	const std::filesystem::path& finalPath,
	const std::shared_ptr<AsyncImportProgressState>& progressState) const {
	if (!progressState) {
		return PublishCompiledAssetAtomically(stagedPath, finalPath);
	}

	// Hold only the short final rename boundary. Compiler work remains parallel,
	// while cancellation can still terminate a long-running child process.
	std::lock_guard lifecycle(importLifecycleMutex);
	std::lock_guard<std::mutex> lock(progressState->publicationMutex);
	if (!importAdmissionOpen || progressState->projectGeneration != importProjectGeneration) return false;
	if (progressState->cancelRequested.load()) {
		return false;
	}
	if (!PublishCompiledAssetAtomically(stagedPath, finalPath)) {
		return false;
	}
	progressState->publicationCompleted = true;
	return true;
}

// AssetDatabase::OnFileEvent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::OnFileEvent(const std::filesystem::path& path, FileStatus status) {
	// Ignore .meta files and directories
	if (path.extension() == ".meta") return;
	if (IsIgnoredAssetDatabasePath(path)) return;
	// Check directory only if it exists (for created/modified)
	if ((status == FileStatus::Created || status == FileStatus::Modified) && std::filesystem::is_directory(path)) return;

	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extension == ".cs") {
		ScriptCompilationSubsystem::Instance().NotifyScriptSourceChanged(path);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (status == FileStatus::Created || status == FileStatus::Modified) {
		if (std::filesystem::exists(path)) {
			ImportAssetWithProgress(path);
		}
	} else if (status == FileStatus::Erased) {
		RemoveAsset(path);
	}
}

// AssetDatabase::ScanProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::ScanProject() {
	// 1. Cleanup orphaned .meta files
	if (std::filesystem::exists(assetRoot)) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (entry.path().extension() == ".meta") {
				std::filesystem::path sourcePath = entry.path();
				sourcePath.replace_extension(""); // Remove .meta extension to get source
				
				// Import sidecar metadata may have been produced by older scans;
				// remove it so bookkeeping files never stay registered as assets.
				if (IsIgnoredAssetDatabasePath(sourcePath)) {
					std::filesystem::remove(entry.path());
					continue;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!std::filesystem::exists(sourcePath)) {
					// Orphaned meta found
					AssetMetadata tempMeta;
					// Try to load to delete compiled asset
					if (LoadMetadata(entry.path(), tempMeta)) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (!tempMeta.compiledPath.empty()) {
							std::filesystem::path fullCompiled = buildRoot / tempMeta.compiledPath;
							if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled);
						}
					}
					// Delete the meta file
					std::filesystem::remove(entry.path());
				}
			}
		}
	}

	// 2. Import existing assets (Sync DB with Disk)
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(assetRoot)) {
		for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
			if (entry.is_regular_file()) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.path().extension() == ".meta") continue;
				if (IsIgnoredAssetDatabasePath(entry.path())) continue;
				ImportAsset(std::filesystem::absolute(entry.path()));
			}
		}
	}
}

void AssetDatabase::ImportAsset(const std::filesystem::path& sourcePath, bool force) {
	ImportAssetInternal(sourcePath, force, false);
}

void AssetDatabase::ImportAssetWithProgress(const std::filesystem::path& sourcePath, bool force) {
	ImportAssetInternal(sourcePath, force, true);
}

// AssetDatabase::ImportAssetInternal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::ImportAssetInternal(const std::filesystem::path& sourcePath, bool force, bool allowAsyncCompile) {
	{ std::lock_guard lifecycle(importLifecycleMutex); if (!importAdmissionOpen) return; }
	const auto absSourcePath = std::filesystem::absolute(sourcePath);
	if (absSourcePath.extension() == ".meta") return;
	if (IsIgnoredAssetDatabasePath(absSourcePath)) return;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::is_directory(absSourcePath)) return;
	if (IsAssetCompiling(absSourcePath)) {
		auto existing = std::find_if(asyncJobs.begin(), asyncJobs.end(), [&](const AsyncCompileJob& job) {
			return job.absSourcePath == absSourcePath;
		});
		AssetMetadata latest = existing->metadata;
		AssetMetadata sidecar;
		if (LoadMetadata(absSourcePath.string() + ".meta", sidecar) && sidecar.guid == latest.guid) latest = std::move(sidecar);
		latest.lastModified = std::filesystem::last_write_time(absSourcePath);
		const bool latestNeedsImport = force || latest.lastModified != existing->metadata.lastModified
			|| latest.settings != existing->metadata.settings;
		if (!existing->admitted && !existing->progressState->cancelRequested.load()) {
			// Unsubmitted work has no closure yet; replace its snapshot rather than
			// launching a stale generation followed by a redundant compile.
			existing->metadata = std::move(latest);
			existing->needsImport = existing->needsImport || latestNeedsImport;
			if (existing->metadata.importerName == "MeshImporter") {
				VirtualGeometryAdaptiveSettingsMetadata settings;
				TryGetVirtualGeometryAdaptiveSettings(existing->metadata, settings);
				existing->resolvedMeshSettings = settings;
			}
			return;
		}
		// Preserve edits before the active finalizer writes its older sidecar.
		deferredImportMetadata[absSourcePath] = std::move(latest);
		// Worker-backed imports can receive repeated file watcher or reimport
		// requests while the first compile is still running. Coalesce those into
		// one follow-up pass so large glTF/FBX files do not start duplicate jobs.
		deferredImports[absSourcePath] = deferredImports[absSourcePath] || latestNeedsImport;
		TERRAENGINE_LOG(LogLevel::Info, "Deferred duplicate asset import while compile is active: " << absSourcePath);
		return;
	}

	const std::string importerName = GetImporterName(absSourcePath);
	AssetMetadata metadata;
	std::filesystem::path metaFilePath = absSourcePath.string() + ".meta";
	
	bool needsImport = force;
	bool metadataValid = false;

	// 1. Prefer in-memory metadata if we are forcing or it already exists
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (const auto latest = deferredImportMetadata.find(absSourcePath); latest != deferredImportMetadata.end()) {
		metadata = std::move(latest->second); deferredImportMetadata.erase(latest); metadataValid = true;
	} else if (assets.count(absSourcePath)) {
		metadata = assets[absSourcePath];
		metadataValid = true;
	} 
	// 2. Otherwise try loading from disk
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (std::filesystem::exists(metaFilePath)) {
		metadataValid = LoadMetadata(metaFilePath, metadata);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadataValid) {
		if (!TryReuseRecentlyRemovedScriptMetadata(absSourcePath, metadata)) {
			GenerateDefaultMetadata(absSourcePath, metadata);
		}
		needsImport = true;
	} else if (!force) {
		// Check timestamp only if not forcing
		auto currentModTime = std::filesystem::last_write_time(absSourcePath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (currentModTime > metadata.lastModified) {
			needsImport = true;
			metadata.lastModified = currentModTime;
		}
	}

	// Always normalize the metadata path back to the file that actually exists
	// on disk so inspector labels and meta persistence follow asset renames.
	metadata.sourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	metadata.importerName = importerName;
	if (RuntimeAssetTypeForCookedAnimationImporter(importerName))
	{
		// A cooked envelope owns its durable identity. New loose imports adopt that
		// GUID instead of generating an unrelated .meta GUID that the typed loader
		// would later (correctly) reject. Existing metadata never changes silently.
		if (const std::optional<TerraEngine::UUID> cookedId = ReadCookedAnimationAssetId(absSourcePath))
		{
			if (!metadataValid)
			{
				metadata.guid = *cookedId;
			}
			else if (metadata.guid != *cookedId)
			{
				needsImport = true;
				TERRAENGINE_LOG(LogLevel::Warning,
					"Cooked animation identity differs from its existing metadata; reimport will fail closed: "
					<< absSourcePath);
			}
		}
	}
	else if(importerName == "TerrainImporter")
	{
		// TerrainComponent persists the descriptor UUID, so metadata must use that
		// same identity even when migrating a project created before this rule.
		if(const std::optional<TerraEngine::UUID> terrainId = ReadTerrainAssetId(absSourcePath))
		{
			if(metadata.guid != *terrainId)
			{
				metadata.guid = *terrainId;
				needsImport = true;
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (metadata.importerName == "ScriptImporter") {
		UpdateScriptMetadata(absSourcePath, metadata);
	}

	// Determine Compiled Path (Relative)
	std::filesystem::path relativeSourcePath = std::filesystem::relative(absSourcePath, assetRoot);
	std::filesystem::path compiledRelativePath = relativeSourcePath;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) compiledRelativePath.replace_extension(".temf");
	else if (metadata.importerName == "TextureImporter") compiledRelativePath.replace_extension(".tetf");
	else if (metadata.importerName == "ShaderImporter") compiledRelativePath.replace_extension(".tspf");
	else if (metadata.importerName == "MaterialImporter") compiledRelativePath.replace_extension(".tmat");
	else if (metadata.importerName == "PhysicsColliderRecipeImporter") compiledRelativePath.replace_extension(".tepcoll");
	// Source media keeps its .meta UUID across move, rename, and reimport while
	// the generated TEAF envelope remains the only runtime-facing payload.
	else if (metadata.importerName == "AudioClipImporter") compiledRelativePath.replace_extension(".teaudio");
	else if (metadata.importerName == "ScriptImporter") compiledRelativePath.clear();
	else if (metadata.importerName == "GenericImporter" || metadata.importerName == "TerrainImporter"
		|| metadata.importerName == "NavigationGeometryCompanionImporter") compiledRelativePath.clear();
	
	// Store relative path in metadata
	metadata.compiledPath = compiledRelativePath;

	if (!metadata.compiledPath.empty()) {
		// Resolve Absolute Paths for checking/compiling
		// Use buildRoot to ensure path includes Assets/
		std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;

		const bool compileOutputReady = IsAssetCompileOutputReady(metadata, absSourcePath, assetRoot, buildRoot);
		if (needsImport || !compileOutputReady) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(fullCompiledPath.parent_path())) {
				std::filesystem::create_directories(fullCompiledPath.parent_path());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (allowAsyncCompile && ShouldCompileAssetAsync(metadata.importerName)) {
				if (const std::optional<AssetType> animationType =
					RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName))
				{
					RecordAnimationReimportStatus({absSourcePath, metadata.guid,
						*ReloadAssetTypeForRuntimeType(*animationType), AnimationReimportState::Queued,
						0u, ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
							metadata.guid, *animationType), 0u, 0u, true,
						"Cooked animation change was coalesced and queued for worker validation."});
				}
				auto progressState = std::make_shared<AsyncImportProgressState>();
				progressState->snapshot.sourcePath = metadata.sourcePath;
				progressState->snapshot.importerName = metadata.importerName;
				SetImportProgress(progressState, "Queued", "Waiting for the asset compiler worker.", 0.05f, true, false);
				std::optional<VirtualGeometryAdaptiveSettingsMetadata> resolvedMeshSettings;
				if (metadata.importerName == "MeshImporter") {
					VirtualGeometryAdaptiveSettingsMetadata settings;
					TryGetVirtualGeometryAdaptiveSettings(metadata, settings);
					resolvedMeshSettings = settings;
				}

				progressState->projectGeneration = importProjectGeneration;
				asyncJobs.push_back({metadata, {}, needsImport, absSourcePath, progressState, resolvedMeshSettings});
				DispatchQueuedImports();
				return; // Defer registration until async job finishes
			} else {
				bool compiled = CompileAsset(metadata);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!compiled) {
					TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << absSourcePath);
				}
			}
		}
	}
	
	FinishImport(metadata, absSourcePath, needsImport);
}


bool AssetDatabase::RunAsyncImportWork(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath,
	const std::shared_ptr<AsyncImportProgressState>& progressState,
	const std::optional<VirtualGeometryAdaptiveSettingsMetadata>& resolvedMeshSettings)
{
	try {
		if (const std::optional<AssetType> animationType =
			RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName))
		{
			RecordAnimationReimportStatus({absSourcePath, metadata.guid,
				*ReloadAssetTypeForRuntimeType(*animationType), AnimationReimportState::Compiling,
				0u, ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
					metadata.guid, *animationType), 0u, 0u, true,
				"Worker is validating and staging the complete cooked animation envelope."});
		}
		SetImportProgress(progressState, "Importing", "Reading source data and writing compiled output.", 0.15f, true, false);
		bool compiled = false;
#ifdef TERRAENGINE_EDITOR_TESTS
		AsyncImportCompileHookForTests compileHook;
		{
			std::lock_guard<std::mutex> lock(asyncImportCompileHookMutex);
			compileHook = asyncImportCompileHookForTests;
		}
		if (compileHook) {
			compiled = compileHook(metadata, progressState->cancelRequested);
		} else {
			compiled = CompileAsset(metadata, progressState, resolvedMeshSettings);
		}
#else
		compiled = CompileAsset(metadata, progressState, resolvedMeshSettings);
#endif
		// Compilers with a staged output mark this inside the publication
		// lock. This second idempotent mark also covers package compilers
		// and the deterministic test hook after they report success.
		if (compiled) {
			compiled = MarkImportPublicationComplete(progressState);
		}
		SetImportProgress(progressState,
			compiled ? "Finalizing" : "Failed",
			compiled ? "Compile output is ready for main-thread registration." : "Compilation failed; see the log for details.",
			compiled ? 0.90f : 1.0f,
			false,
			!compiled);
		return compiled;
	} catch (const AssetImportCancelledException&) {
		progressState->cancelRequested.store(true);
		return false;
	} catch (const std::exception& exception) {
		TERRAENGINE_LOG(LogLevel::Error, "Asset import worker failed: " << exception.what());
		return false;
	}

}
// AssetDatabase::FinishImport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport) {
	// Always save metadata
	SaveMetadata(metadata);
	RegisterImportedAsset(metadata, absSourcePath, needsImport);
	RequestAssetIndexSave();
}

void AssetDatabase::RegisterImportedAsset(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport) {
	bool assetAlreadyKnown = false;
	std::optional<AssetMetadata> previousMetadata;
	for(const auto& [path, registered] : assets) {
		(void)path;
		if(registered.guid == metadata.guid) {
			assetAlreadyKnown = true;
			previousMetadata = registered;
			break;
		}
	}
	// Asset GUIDs must stay one-to-one with their live source path. Rename
	// operations can briefly surface the same GUID under both the old and new
	// path, and keeping the stale entry makes GUID lookups return outdated
	// script names after a rename.
	for (auto it = assets.begin(); it != assets.end(); ) {
		if (it->first != absSourcePath && it->second.guid == metadata.guid) {
			it = assets.erase(it);
		} else {
			++it;
		}
	}

	assets[absSourcePath] = metadata;
	if(metadata.importerName == "MaterialImporter") {
		// A GUID observed at a different path is a rename; a known path is a
		// reimport. Both retain authored terrain layer references by UUID.
		const TerrainMaterialChangeKind changeKind = previousMetadata
			? (previousMetadata->sourcePath != metadata.sourcePath
				? TerrainMaterialChangeKind::Renamed : TerrainMaterialChangeKind::Reimported)
			: TerrainMaterialChangeKind::Imported;
		RecordTerrainMaterialChange(metadata, changeKind);
	}
	if(metadata.importerName == "TerrainImporter") {
		// Terrain roots are already durable runtime manifests. Their private data
		// directory stays next to the root, so no generic one-file compiler runs.
		ResourceManager::Instance().RegisterAssetPath(metadata.guid, absSourcePath, AssetType::Terrain);
	}
	if(metadata.importerName == "NavigationGeometryCompanionImporter") {
		// Navigation companions are immutable CPU geometry artifacts rather than
		// renderer resources. Register their durable path in the typed navigation
		// registry so worker jobs can acquire a generation-qualified snapshot
		// without depending on mesh GPU residency.
		ResourceManager::Instance().RegisterNavigationGeometryPath(metadata.guid, absSourcePath);
	}

	// Register with Engine ResourceManager so it can resolve GUIDs to compiled paths
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadata.compiledPath.empty()) {
		AssetType type;
		bool known = false;
		const std::optional<AssetType> cookedAnimationType =
			RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "MeshImporter") { type = AssetType::Mesh; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; known = true; }
		else if (metadata.importerName == "PhysicsMaterialImporter") { type = AssetType::PhysicsMaterial; known = true; }
		else if (metadata.importerName == "PhysicsColliderRecipeImporter"
			|| metadata.importerName == "CookedColliderImporter") { type = AssetType::CookedCollider; known = true; }
		else if(metadata.importerName == "AudioClipImporter") { type = AssetType::AudioClip; known = true; }
		else if (cookedAnimationType) {
			type = *cookedAnimationType;
			known = true;
		}

		if (known) {
			const std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			AnimationResourceValidationResult animationValidation;
			bool registrationAccepted = true;
			if (cookedAnimationType)
			{
				animationValidation = ResourceManager::Instance().ValidateAnimationAssetFile(
					fullCompiled, *cookedAnimationType);
				registrationAccepted = animationValidation.Passed()
					&& animationValidation.assetId == metadata.guid;
				if (!registrationAccepted)
				{
					RecordAnimationReimportStatus({absSourcePath, metadata.guid,
						*ReloadAssetTypeForRuntimeType(*cookedAnimationType),
						AnimationReimportState::RetainedLastKnownGood, 0u,
						ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
							metadata.guid, *cookedAnimationType), 0u, 0u, true,
						animationValidation.Passed()
							? "Cooked animation registration rejected an envelope GUID mismatch."
							: animationValidation.diagnosticCode + ": " + animationValidation.message});
				}
			}

			if (!registrationAccepted)
			{
				TERRAENGINE_LOG(LogLevel::Warning,
					"Animation registration retained its prior path and generation: " << fullCompiled);
			}
			else
			{
			// Register source path as an alias so the engine can resolve things like "viking_room.png" to the same GUID.
			// We do this first so that idToPath (the preferred load path) is eventually overwritten by the compiled path.
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, absSourcePath, type);
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, fullCompiled, type);
			if(type == AssetType::AudioClip && needsImport) {
				// Audio ResourceManager performs its parse before replacing the shared
				// generation, so corrupt watched reimports retain the prior clip.
				(void)ResourceManager::Instance().ReloadAudioClip(metadata.guid);
			}

			if (cookedAnimationType)
			{
				const TerraEngine::Animation::AnimationReloadOutcome previous =
					animationHotReloadPublisher.Query(metadata.guid);
				RecordAnimationReimportStatus({absSourcePath, metadata.guid,
					*ReloadAssetTypeForRuntimeType(*cookedAnimationType), AnimationReimportState::Registered,
					previous.activeGeneration + 1u, previous.activeGeneration, 0u, 0u,
					false, "Validated cooked animation path and identity were registered atomically."});

				if (needsImport || previous.activeGeneration == 0u)
				{
					TerraEngine::Animation::AnimationReloadCandidate candidate;
					candidate.assetId = metadata.guid;
					candidate.generation = previous.activeGeneration + 1u;
					candidate.compatibilityFingerprint = animationValidation.actualCompatibilityFingerprint;
					candidate.parsed = true;
					candidate.dependenciesReady = true;
					candidate.assetType = *ReloadAssetTypeForRuntimeType(*cookedAnimationType);
					candidate.contentFingerprint = animationValidation.contentFingerprint;
					candidate.contentHash = animationValidation.contentHash;
					candidate.contentHashPresent = animationValidation.contentHashPresent;
					const std::array candidates{candidate};
					ResourceManager& resources = ResourceManager::Instance();
					std::vector<AnimationAssetReloadRequest> cacheRequests;
					if(resources.GetAnimationAssetRuntimeGeneration(
						metadata.guid, *cookedAnimationType) != 0U) {
						cacheRequests.push_back({metadata.guid, *cookedAnimationType,
							candidate.compatibilityFingerprint, candidate.contentFingerprint,
							previous.activeGeneration, candidate.contentHash,
							candidate.contentHashPresent});
					}
					auto publisherBatch = animationHotReloadPublisher.PrepareBatch(candidates);
					PreparedAnimationAssetReloadBatch resourceBatch;
					bool resourcesPrepared = true;
					if(!cacheRequests.empty()) {
						resourceBatch = resources.PrepareAnimationAssetReloadBatch(cacheRequests);
						resourcesPrepared = resourceBatch.Accepted();
					}

					if(publisherBatch.Accepted() && resourcesPrepared) {
						AnimationResourceBatchCommitContext context{&resourceBatch};
						(void)animationHotReloadPublisher.CommitPreparedBatch(
							publisherBatch,
						cacheRequests.empty() ? nullptr : CommitPreparedAnimationResourceBatch,
						cacheRequests.empty() ? nullptr : &context,
						cacheRequests.empty() ? nullptr : ReleasePreparedAnimationResourceBatchLocks);
					}
					std::vector<TerraEngine::Animation::AnimationReloadOutcome> outcomes =
						std::move(publisherBatch).ReleaseOutcomes();
					TerraEngine::Animation::AnimationReloadOutcome outcome;
					if(!outcomes.empty()) outcome = std::move(outcomes.front());
					const bool liveReloaded = outcome.published
						&& (cacheRequests.empty() || resourceBatch.Committed());
					const TerraEngine::Animation::AnimationReloadOutcome active =
						animationHotReloadPublisher.Query(metadata.guid);
					if(liveReloaded) {
						resources.FinalizeCommittedAnimationAssetReloadBatch(resourceBatch);
						animationReloadBoundaryToRetireNextUpdate = std::max(
							animationReloadBoundaryToRetireNextUpdate, outcome.publicationBoundary);
					}
					const std::string diagnostic = liveReloaded
						? "Animation publisher and every resident typed cache switched at one protected boundary."
						: !resourcesPrepared
							? "Animation cache staging failed; publisher and all live caches retained their prior generation: "
								+ std::string(resourceBatch.Diagnostic())
							: outcome.reason;
					RecordAnimationReimportStatus({absSourcePath, metadata.guid,
						candidate.assetType,
						liveReloaded ? AnimationReimportState::Published
							: AnimationReimportState::RetainedLastKnownGood,
						candidate.generation, active.activeGeneration,
						active.publicationBoundary, 0u, !liveReloaded, diagnostic});
				}
			}

			(void)needsImport;
			// Import registration is allowed to update the asset database and
			// ResourceManager path aliases, but it must not synchronously destroy
			// or rewrite Vulkan descriptors. The editor can import while a view
			// has already recorded command buffers for the current frame; tearing
			// down mesh/material/texture GPU objects here would make those command
			// buffers invalid before submit. The renderer/resource residency path
			// owns descriptor-safe retirement once a later frame observes the new
			// asset revision.
			}
		}
	}

	if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
	{
		RegisterGeneratedModelPackageMaterialMetadata(absSourcePath);
		// Generated child resources only publish GUID-to-path aliases here. Live
		// GPU descriptor invalidation must happen through the renderer's normal
		// retirement path, otherwise already-recorded command buffers may still
		// reference descriptor sets that a package reimport just updated or freed.
		RegisterGeneratedModelPackageResources(absSourcePath);
		UpdateModelPackageAnimationSummary(absSourcePath);
	}

	const auto oldPhysicsKind = previousMetadata
		? PhysicsAssetKindForImporter(previousMetadata->importerName)
		: std::nullopt;
	const auto newPhysicsKind = PhysicsAssetKindForImporter(metadata.importerName);
	if(oldPhysicsKind && (!newPhysicsKind || *oldPhysicsKind != *newPhysicsKind)) {
		PublishPhysicsAssetChange(*previousMetadata,
			TerraEngine::Physics::PhysicsAssetChangeKind::Removed);
	}
	if(newPhysicsKind && (!assetAlreadyKnown || !oldPhysicsKind || *oldPhysicsKind != *newPhysicsKind)) {
		PublishPhysicsAssetChange(metadata, TerraEngine::Physics::PhysicsAssetChangeKind::Added);
	} else if(newPhysicsKind && needsImport) {
		PublishPhysicsAssetChange(metadata, TerraEngine::Physics::PhysicsAssetChangeKind::Updated);
	}
}

void AssetDatabase::PublishPhysicsAssetChange(
	const AssetMetadata& metadata,
	TerraEngine::Physics::PhysicsAssetChangeKind changeKind) {
	const auto kind = PhysicsAssetKindForImporter(metadata.importerName);
	if(!kind || metadata.guid == TerraEngine::EMPTY_UUID) return;
	// Publication occurs only after the main-thread ResourceManager alias update
	// or removal has completed, so physics never observes a generation whose
	// corresponding compiled resource is still half-published.
	TerraEngine::Physics::PhysicsAssetChangeJournal::Instance().Publish(
		metadata.guid, *kind, changeKind);
}

bool AssetDatabase::LoadAssetIndex() {
	if (!std::filesystem::exists(assetIndexPath)) {
		return false;
	}

	std::ifstream file(assetIndexPath);
	if (!file.is_open()) {
		return false;
	}

	json index;
	try {
		file >> index;
	} catch (const json::exception& exception) {
		TERRAENGINE_LOG(LogLevel::Warning, "Asset index is malformed and will be rebuilt: " << assetIndexPath << " (" << exception.what() << ")");
		return false;
	}
	// Generated package children use the normal registration path, which may
	// request an index save. Close the Windows file handle before registration
	// and defer those requests so no save attempts to replace the file currently
	// being loaded and no partial prefix of the index is published.
	file.close();

	if (index.value("version", 0) != 1 || !index.contains("assets") || !index["assets"].is_array()) {
		TERRAENGINE_LOG(LogLevel::Warning, "Asset index version is unsupported and will be rebuilt: " << assetIndexPath);
		return false;
	}

	loadingAssetIndex = true;
	assetIndexSaveDeferred = false;
	std::size_t loadedCount = 0u;
	for (const json& entry : index["assets"]) {
		try {
			AssetMetadata metadata;
			metadata.guid = StringToUuid(entry.at("guid").get<std::string>());
			metadata.importerName = entry.at("importerName").get<std::string>();
			using FileDuration = std::filesystem::file_time_type::duration;
			metadata.lastModified = std::filesystem::file_time_type(FileDuration(entry.at("lastModified").get<FileDuration::rep>()));
			metadata.sourcePath = entry.at("sourcePath").get<std::string>();
			metadata.compiledPath = entry.value("compiledPath", std::string());
			metadata.settings = entry.value("settings", json::object());

			const std::filesystem::path absSourcePath = std::filesystem::absolute(assetRoot / metadata.sourcePath);
			if (!std::filesystem::exists(absSourcePath)) {
				continue;
			}
			if (!metadata.compiledPath.empty()
				&& !TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)
				&& !std::filesystem::exists(buildRoot / metadata.compiledPath)) {
				continue;
			}

			RegisterImportedAsset(metadata, absSourcePath, false);
			++loadedCount;
		} catch (const std::exception& exception) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping invalid asset index entry in " << assetIndexPath << ": " << exception.what());
		}
	}
	loadingAssetIndex = false;
	if (assetIndexSaveDeferred) {
		assetIndexSaveDeferred = false;
		RequestAssetIndexSave();
	}

	TERRAENGINE_LOG(LogLevel::Info, "Loaded " << loadedCount << " assets from startup asset index: " << assetIndexPath);
	return loadedCount > 0u || std::filesystem::exists(assetIndexPath);
}

void AssetDatabase::RequestAssetIndexSave() {
	assetIndexSaveDeferred = true;
	if (loadingAssetIndex || assetIndexSaveBatchDepth != 0U) {
		// Multiple generated materials and prefabs may be registered from one
		// package entry. One final save publishes the complete database after the
		// outer load finishes instead of racing a shared temporary file per child.
		return;
	}
	TrySaveDeferredAssetIndex();
}

void AssetDatabase::EndAssetIndexSaveBatch() noexcept {
	if (assetIndexSaveBatchDepth == 0U) return;
	--assetIndexSaveBatchDepth;
	if (assetIndexSaveBatchDepth == 0U && !loadingAssetIndex) TrySaveDeferredAssetIndex();
}

void AssetDatabase::TrySaveDeferredAssetIndex() noexcept {
	if (!assetIndexSaveDeferred) return;
	// Disk failure or allocation failure retains demand for the next protected
	// Update. Scope destruction must never replace the original exception.
	try { if (SaveAssetIndex()) assetIndexSaveDeferred = false; } catch (...) {}
}

bool AssetDatabase::SaveAssetIndex() const {
#ifdef TERRAENGINE_EDITOR_TESTS
	++assetIndexSaveAttemptsForTests;
#endif
	if (assetIndexPath.empty()) {
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(assetIndexPath.parent_path(), ec);
	if (ec) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to create asset index directory " << assetIndexPath.parent_path() << ": " << ec.message());
		return false;
	}

	json index;
	index["version"] = 1;
	index["assets"] = json::array();
	for (const auto& [absPath, metadata] : assets) {
		if (!std::filesystem::exists(absPath)) {
			continue;
		}

		json entry;
		entry["guid"] = UuidToString(metadata.guid);
		entry["importerName"] = metadata.importerName;
		entry["lastModified"] = metadata.lastModified.time_since_epoch().count();
		entry["sourcePath"] = metadata.sourcePath.generic_string();
		entry["compiledPath"] = metadata.compiledPath.generic_string();
		entry["settings"] = metadata.settings;
		index["assets"].push_back(std::move(entry));
	}

	const std::filesystem::path tempPath = assetIndexPath.string() + ".tmp";
	{
		std::ofstream output(tempPath, std::ios::trunc);
		if (!output.is_open()) {
			return false;
		}
		output << std::setw(4) << index << '\n';
	}

	std::filesystem::rename(tempPath, assetIndexPath, ec);
	if (ec) {
		std::filesystem::remove(assetIndexPath, ec);
		ec.clear();
		std::filesystem::rename(tempPath, assetIndexPath, ec);
	}
	if (ec) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to publish asset index " << assetIndexPath << ": " << ec.message());
		return false;
	}
	return true;
}

void AssetDatabase::StartBackgroundProjectReconcile() {
	if (!discoveryOpen || projectReconcileFuture.valid() || applyingReconcileGeneration != 0U) {
		return;
	}

	const std::filesystem::path scanRoot = assetRoot;
	const auto projectGeneration = discovery.projectGeneration, requestedGeneration = discovery.requestedGeneration;
#ifdef TERRAENGINE_EDITOR_TESTS
	const auto scanHook = projectReconcileHookForTests;
#endif
	discovery.scanningGeneration = requestedGeneration;
	discovery.scanning = true;
	try {
	projectReconcileFuture = std::async(std::launch::async, [scanRoot, projectGeneration, requestedGeneration
#ifdef TERRAENGINE_EDITOR_TESTS
		, scanHook
#endif
	]() {
		ProjectReconcileSnapshot snapshot;
		snapshot.projectGeneration = projectGeneration; snapshot.requestedGeneration = requestedGeneration;
#ifdef TERRAENGINE_EDITOR_TESTS
		if (scanHook) scanHook(false);
#endif
		if (!std::filesystem::exists(scanRoot)) {
			return snapshot;
		}

		for (const auto& entry : std::filesystem::recursive_directory_iterator(scanRoot)) {
			const std::filesystem::path path = entry.path();
			if (entry.is_regular_file() && path.extension() == ".meta") {
				std::filesystem::path sourcePath = path;
				sourcePath.replace_extension("");
				if (IsIgnoredAssetDatabasePath(sourcePath) || !std::filesystem::exists(sourcePath)) {
					snapshot.orphanedMetaFiles.push_back(path);
				}
				continue;
			}

			if (entry.is_regular_file() && IsTrackedSourceAssetPath(path)) {
				snapshot.sourceFiles.push_back(std::filesystem::absolute(path));
			}
		}
#ifdef TERRAENGINE_EDITOR_TESTS
		if (scanHook) scanHook(true);
#endif
		return snapshot;
	});
	} catch (...) {
		discovery.scanning = false; discovery.pending = true; ++discovery.failedScans;
		discovery.lastFailure = "Discovery scan submission failed; recovery remains pending.";
	}
}

void AssetDatabase::PublishBackgroundProjectReconcile() {
	if (!projectReconcileFuture.valid()) {
		return;
	}
	if (projectReconcileFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		return;
	}

	ProjectReconcileSnapshot snapshot;
	try { snapshot = projectReconcileFuture.get(); }
	catch (...) {
		discovery.scanning = false; discovery.pending = true; ++discovery.failedScans;
		discovery.lastFailure = "Discovery scan failed; recovery remains pending.";
		return;
	}
	discovery.scanning = false;
	if (!discoveryOpen || snapshot.projectGeneration != discovery.projectGeneration) { ++discovery.retiredScans; return; }
	// The scan is authoritative for known source membership. Remove stale
	// catalog entries at the protected owner boundary; never delete disk here.
	std::set<std::filesystem::path> present(snapshot.sourceFiles.begin(), snapshot.sourceFiles.end());
	for (auto iterator = assets.begin(); iterator != assets.end();) {
		if (IsTrackedSourceAssetPath(iterator->first) && !present.contains(iterator->first)
			&& !std::filesystem::exists(iterator->first)) {
			pendingReconcileRemovals.push_back(iterator->first);
			++iterator;
		} else ++iterator;
	}
	applyingReconcileGeneration = snapshot.requestedGeneration;
	for (const std::filesystem::path& metaPath : snapshot.orphanedMetaFiles) {
		pendingReconcileOrphanedMetas.push_back(metaPath);
	}
	for (const std::filesystem::path& sourcePath : snapshot.sourceFiles) {
		pendingReconcileImports.push_back(sourcePath);
	}
	TERRAENGINE_LOG(LogLevel::Info,
		"Queued asset database reconcile: sources=" << pendingReconcileImports.size()
		<< " orphanedMetas=" << pendingReconcileOrphanedMetas.size());
}

void AssetDatabase::ProcessPendingProjectReconcile() {
	// All disk/catalog application shares a small owner-boundary budget. The
	// worker walks directories; normal editor frames never wait for that walk.
	constexpr std::size_t kMaxReconcileImportsPerFrame = 16u;
	std::size_t processedCount = 0u;
	while (!pendingReconcileOrphanedMetas.empty() && processedCount < kMaxReconcileImportsPerFrame) {
			const auto metaPath = pendingReconcileOrphanedMetas.front(); pendingReconcileOrphanedMetas.pop_front();
			AssetMetadata tempMeta;
			std::filesystem::path sourcePath = metaPath;
			sourcePath.replace_extension("");
			if (LoadMetadata(metaPath, tempMeta) && !tempMeta.compiledPath.empty()) {
				std::error_code removeCompiledError;
				std::filesystem::remove(buildRoot / tempMeta.compiledPath, removeCompiledError);
			}
			std::error_code removeMetaError;
			std::filesystem::remove(metaPath, removeMetaError);
		RequestAssetIndexSave();
		++processedCount;
	}
	while (!pendingReconcileRemovals.empty() && processedCount < kMaxReconcileImportsPerFrame) {
		const auto path = pendingReconcileRemovals.front(); pendingReconcileRemovals.pop_front();
		if (!std::filesystem::exists(path)) OnFileEvent(path, FileStatus::Erased);
		++processedCount;
	}
	while (!pendingReconcileImports.empty() && processedCount < kMaxReconcileImportsPerFrame) {
		const std::filesystem::path sourcePath = pendingReconcileImports.front();
		pendingReconcileImports.pop_front();
		if (std::filesystem::exists(sourcePath)) {
			// Startup and reconcile imports use the progress-aware path so heavy
			// compilers run after the editor is visible and asset tiles can show
			// which files are still cooking.
			ImportAssetWithProgress(sourcePath);
		}
		++processedCount;
	}
	if (applyingReconcileGeneration != 0U && pendingReconcileImports.empty() && pendingReconcileRemovals.empty() && pendingReconcileOrphanedMetas.empty()) {
		discovery.completedGeneration = applyingReconcileGeneration;
		applyingReconcileGeneration = 0U;
		discovery.pending = discovery.requestedGeneration > discovery.completedGeneration;
	}
}

void AssetDatabase::RegisterGeneratedModelPackageMaterialMetadata(const std::filesystem::path& absSourcePath) {
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult)) {
		return;
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials)
	{
		const std::filesystem::path materialSourcePath = assetRoot / material.sourcePath;
		if (!std::filesystem::exists(materialSourcePath))
		{
			continue;
		}

		AssetMetadata materialMetadata;
		materialMetadata.guid = material.guid;
		materialMetadata.importerName = "MaterialImporter";
		materialMetadata.sourcePath = material.sourcePath;
		materialMetadata.compiledPath = material.compiledPath;
		materialMetadata.lastModified = std::filesystem::last_write_time(materialSourcePath);
		materialMetadata.settings = json::object();
		// Package import already wrote a coherent package sidecar and generated
		// material files. Register the generated material metadata here on the
		// main thread so background model import never mutates the live database.
		FinishImport(materialMetadata, std::filesystem::absolute(materialSourcePath), false);
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelPrefabAsset& prefab : importResult.generatedPrefabs)
	{
		const std::filesystem::path prefabSourcePath = assetRoot / prefab.sourcePath;
		if (!std::filesystem::exists(prefabSourcePath))
		{
			continue;
		}

		AssetMetadata prefabMetadata;
		prefabMetadata.guid = prefab.guid;
		prefabMetadata.importerName = "PrefabImporter";
		prefabMetadata.sourcePath = prefab.sourcePath;
		prefabMetadata.compiledPath.clear();
		prefabMetadata.lastModified = std::filesystem::last_write_time(prefabSourcePath);
		prefabMetadata.settings = json::object();
		// Generated prefabs are editor documents, not render resources. Register
		// metadata only so Project Browser, prefab editing, and AssetIndex can
		// open the configured package object without polluting GPU residency.
		FinishImport(prefabMetadata, std::filesystem::absolute(prefabSourcePath), false);
	}
}

void AssetDatabase::RegisterGeneratedModelPackageResources(const std::filesystem::path& absSourcePath)
{
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	if(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult)
		|| importResult.status == TerraEngine::AssetPipeline::ModelImportStatus::Failed)
	{
		// Failed candidates never replace a working registration. The package
		// importer deliberately leaves the last-known-good sidecar in place.
		return;
	}

	const auto packageIt = assets.find(std::filesystem::absolute(absSourcePath));
	if(packageIt == assets.end() || packageIt->second.guid != importResult.packageGuid)
	{
		TERRAENGINE_LOG(LogLevel::Warning,
			"Generated animation package registration rejected because the sidecar package GUID does not match its source metadata: "
			<< absSourcePath);
		return;
	}

	const bool hasPublishedAnimationManifest = IsPublishedAnimationManifest(importResult);
	std::vector<GeneratedPackageResourceAlias> aliases;
	if(hasPublishedAnimationManifest)
	{
		aliases.reserve(importResult.animationManifest.subAssets.size());
		for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : importResult.animationManifest.subAssets)
		{
			const std::optional<GeneratedModelPackageChildKind> kind = GeneratedKindForManifestKind(child.kind);
			if(!kind || !RuntimeAssetTypeForGeneratedKind(*kind))
			{
				continue;
			}
			if(!child.readOnlyPackageChild)
			{
				TERRAENGINE_LOG(LogLevel::Warning,
					"Generated animation package registration rejected an editable manifest child: "
					<< child.semanticKey);
				return;
			}

			const std::optional<std::filesystem::path> compiledPath = ResolveGeneratedBuildArtifact(
				buildRoot, child.compiledPath);
			if(!compiledPath)
			{
				// Treat missing runtime data as an invalid generation instead of exposing
				// a mixture of old and new child paths to the engine.
				TERRAENGINE_LOG(LogLevel::Warning,
					"Generated animation package runtime artifact is missing or unsafe: "
					<< child.compiledPath);
				return;
			}
			const std::optional<AssetType> runtimeType = RuntimeAssetTypeForGeneratedKind(*kind);
			if (runtimeType && (*runtimeType == AssetType::Skeleton
				|| *runtimeType == AssetType::AnimationClip))
			{
				const AnimationResourceValidationResult validation =
					ResourceManager::Instance().ValidateAnimationAssetFile(
						*compiledPath, *runtimeType, child.compatibilityFingerprint);
				if (!validation.Passed() || validation.assetId != child.guid)
				{
					// Validate every animation child before publishing any alias. A bad
					// sibling therefore cannot create a mixed package generation.
					RecordAnimationReimportStatus({absSourcePath, child.guid,
						*ReloadAssetTypeForRuntimeType(*runtimeType),
						AnimationReimportState::RetainedLastKnownGood,
						importResult.animationManifest.publishedGeneration,
						animationHotReloadPublisher.Query(child.guid).activeGeneration,
						0u, 0u, true,
						validation.Passed()
							? "Generated animation child GUID does not match its manifest identity."
							: validation.diagnosticCode + ": " + validation.message});
					TERRAENGINE_LOG(LogLevel::Warning,
						"Generated animation package retained its prior alias generation because child validation failed: "
						<< child.semanticKey);
					return;
				}
			}
			AppendGeneratedPackageAlias(aliases, {child.guid, *kind, *compiledPath});
		}
	}
	else
	{
		// Sidecars created before the legacy animation manifest still identify
		// stable generated render resources. Preserve that read-only compatibility
		// path while reserving atomic all-or-none publication for current manifests.
		aliases.reserve(importResult.generatedMeshes.size()
			+ importResult.generatedMaterials.size()
			+ importResult.generatedTextures.size());
		const auto appendLegacyAlias = [&](const TerraEngine::UUID& guid,
			GeneratedModelPackageChildKind kind,
			const std::filesystem::path& relativePath) {
			if(const std::optional<std::filesystem::path> compiledPath =
				ResolveGeneratedBuildArtifact(buildRoot, relativePath))
			{
				AppendGeneratedPackageAlias(aliases, {guid, kind, *compiledPath});
			}
		};
		for(const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes)
		{
			appendLegacyAlias(mesh.guid, GeneratedModelPackageChildKind::Mesh, mesh.compiledPath);
		}
		for(const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials)
		{
			appendLegacyAlias(material.guid, GeneratedModelPackageChildKind::Material, material.compiledPath);
		}
		for(const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures)
		{
			appendLegacyAlias(texture.guid, GeneratedModelPackageChildKind::Texture, texture.compiledPath);
		}
	}

	if(aliases.empty())
	{
		return;
	}

	// Publish every new preferred path before retiring children that disappeared.
	// Stable IDs are re-registered in place, so a move/rename changes their load
	// path without destroying an already loaded resource mid-frame.
	for(const GeneratedPackageResourceAlias& alias : aliases)
	{
		const std::optional<AssetType> type = RuntimeAssetTypeForGeneratedKind(alias.kind);
		if(type)
		{
			ResourceManager::Instance().RegisterAssetPath(alias.guid, alias.compiledPath, *type);
		}
	}

	std::vector<std::pair<TerraEngine::UUID, GeneratedModelPackageChildKind>> nextRegistrations;
	nextRegistrations.reserve(aliases.size());
	for(const GeneratedPackageResourceAlias& alias : aliases)
	{
		nextRegistrations.emplace_back(alias.guid, alias.kind);
	}

	if(const auto previousIt = generatedModelPackageResources.find(importResult.packageGuid);
		previousIt != generatedModelPackageResources.end())
	{
		for(const auto& [previousGuid, previousKind] : previousIt->second)
		{
			const std::optional<AssetType> previousType = RuntimeAssetTypeForGeneratedKind(previousKind);
			const bool remainsPublished = previousType && std::any_of(
				nextRegistrations.begin(), nextRegistrations.end(),
				[&](const auto& current) {
					return current.first == previousGuid
						&& RuntimeAssetTypeForGeneratedKind(current.second) == previousType;
				});
			if(previousType && !remainsPublished)
			{
				// DeleteResource removes stale path aliases and sends any loaded render
				// object through the renderer's protected invalidation path.
				ResourceManager::Instance().DeleteResource(previousGuid, *previousType);
			}
		}
	}

	generatedModelPackageResources[importResult.packageGuid] = std::move(nextRegistrations);
	// Aliases are now published as one coherent package generation.  Only after
	// that protected boundary may evaluators advance their last-known-good
	// generation; malformed children deliberately leave the old publication up.
	PublishGeneratedAnimationHotReload(absSourcePath);
}

void AssetDatabase::PublishGeneratedAnimationHotReload(const std::filesystem::path& absSourcePath)
{
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult)
		|| !IsPublishedAnimationManifest(importResult))
	{
		return;
	}

	struct PreparedAnimationChild
	{
		const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset* child = nullptr;
		AssetType type = AssetType::AnimationClip;
		std::filesystem::path cookedPath;
		AnimationResourceValidationResult validation;
		TerraEngine::Animation::AnimationReloadCandidate candidate;
	};
	std::vector<PreparedAnimationChild> prepared;

	for (const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : importResult.animationManifest.subAssets)
	{
		const std::optional<GeneratedModelPackageChildKind> kind = GeneratedKindForManifestKind(child.kind);
		const std::optional<AssetType> type = kind ? RuntimeAssetTypeForGeneratedKind(*kind) : std::nullopt;
		if (!type || (*type != AssetType::Skeleton && *type != AssetType::AnimationClip))
		{
			continue;
		}

		const std::optional<std::filesystem::path> cookedPath = ResolveGeneratedBuildArtifact(buildRoot, child.compiledPath);
		PreparedAnimationChild item;
		item.child = &child;
		item.type = *type;
		if (cookedPath)
		{
			item.cookedPath = *cookedPath;
			item.validation = *type == AssetType::Skeleton
				? ResourceManager::Instance().ValidateSkeletonAssetFile(*cookedPath, child.compatibilityFingerprint)
				: ResourceManager::Instance().ValidateAnimationClipAssetFile(*cookedPath, child.compatibilityFingerprint);
		}
		item.candidate.assetId = child.guid;
		item.candidate.generation = importResult.animationManifest.publishedGeneration;
		item.candidate.compatibilityFingerprint = child.compatibilityFingerprint;
		item.candidate.parsed = cookedPath && item.validation.Passed()
			&& item.validation.assetId == child.guid;
		item.candidate.dependenciesReady = item.candidate.parsed;
		item.candidate.assetType = *ReloadAssetTypeForRuntimeType(*type);
		item.candidate.contentFingerprint = item.validation.contentFingerprint;
		item.candidate.contentHash = item.validation.contentHash;
		item.candidate.contentHashPresent = item.validation.contentHashPresent;
		item.candidate.dependenciesAreCompleteDeclaration = true;
		prepared.push_back(std::move(item));
	}

	if (prepared.empty())
	{
		return;
	}

	// Dependency records are built only for animation children in this package.
	// Render-resource dependencies stay in the package manifest and do not enter
	// the evaluator-generation graph.
	for (PreparedAnimationChild& item : prepared)
	{
		for (const TerraEngine::AssetPipeline::AnimationImportDependency& sourceDependency :
			item.child->dependencies)
		{
			const auto dependency = std::find_if(prepared.begin(), prepared.end(), [&](const auto& candidate) {
				return candidate.child->guid == sourceDependency.assetGuid;
			});
			if (dependency == prepared.end())
			{
				continue;
			}
			item.candidate.dependencies.push_back({sourceDependency.assetGuid,
				dependency->candidate.generation,
				sourceDependency.compatibilityFingerprint != 0u
					? sourceDependency.compatibilityFingerprint
					: dependency->candidate.compatibilityFingerprint,
				sourceDependency.required});
		}
		RecordAnimationReimportStatus({absSourcePath, item.child->guid,
			item.candidate.assetType, AnimationReimportState::Registered,
			item.candidate.generation,
			animationHotReloadPublisher.Query(item.child->guid).activeGeneration,
			0u, 0u, false,
			"Generated package child passed complete-batch validation and alias registration."});
	}

	std::vector<TerraEngine::Animation::AnimationReloadCandidate> candidates;
	candidates.reserve(prepared.size());
	for (const PreparedAnimationChild& item : prepared)
	{
		candidates.push_back(item.candidate);
	}
	auto publisherBatch = animationHotReloadPublisher.PrepareBatch(candidates);
#ifdef TERRAENGINE_EDITOR_TESTS
	if(publisherBatch.Accepted()) {
		AnimationPackagePostPublisherPrepareHookForTests hook;
		{
			std::lock_guard<std::mutex> lock(animationPackageTestHookMutex);
			hook = animationPackagePostPublisherPrepareHookForTests;
		}
		if(hook) hook();
	}
#endif

	ResourceManager& resources = ResourceManager::Instance();
	std::vector<AnimationAssetReloadRequest> cacheRequests;
	cacheRequests.reserve(prepared.size());
	for(const PreparedAnimationChild& item : prepared) {
		if(resources.GetAnimationAssetRuntimeGeneration(item.child->guid, item.type) != 0U) {
			cacheRequests.push_back({item.child->guid, item.type,
				item.candidate.compatibilityFingerprint, item.candidate.contentFingerprint,
				animationHotReloadPublisher.Query(item.child->guid).activeGeneration,
				item.candidate.contentHash, item.candidate.contentHashPresent});
		}
	}
	PreparedAnimationAssetReloadBatch resourceBatch;
	bool resourcesPrepared = true;
	if(publisherBatch.Accepted() && !cacheRequests.empty()) {
		resourceBatch = resources.PrepareAnimationAssetReloadBatch(cacheRequests);
		resourcesPrepared = resourceBatch.Accepted();
	}
	if(publisherBatch.Accepted() && resourcesPrepared) {
		AnimationResourceBatchCommitContext context{&resourceBatch};
		(void)animationHotReloadPublisher.CommitPreparedBatch(
			publisherBatch,
			cacheRequests.empty() ? nullptr : CommitPreparedAnimationResourceBatch,
			cacheRequests.empty() ? nullptr : &context,
			cacheRequests.empty() ? nullptr : ReleasePreparedAnimationResourceBatchLocks);
	}
	std::vector<TerraEngine::Animation::AnimationReloadOutcome> outcomes =
		std::move(publisherBatch).ReleaseOutcomes();
	if (outcomes.size() != prepared.size())
	{
		return;
	}

	const bool published = resourcesPrepared
		&& (cacheRequests.empty() || resourceBatch.Committed())
		&& std::all_of(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
			return outcome.published;
		});
	if (published)
	{
		resources.FinalizeCommittedAnimationAssetReloadBatch(resourceBatch);
		// Dependency edges were validated and swapped inside PreparedBatch, so no
		// post-publication best-effort graph mutation can split invalidation state.
	}

	for (std::size_t index = 0u; index < prepared.size(); ++index)
	{
		const PreparedAnimationChild& item = prepared[index];
		const TerraEngine::Animation::AnimationReloadOutcome& outcome = outcomes[index];
		const TerraEngine::Animation::AnimationReloadOutcome active =
			animationHotReloadPublisher.Query(item.child->guid);
		if (published)
		{
			animationReloadBoundaryToRetireNextUpdate = std::max(
				animationReloadBoundaryToRetireNextUpdate, outcome.publicationBoundary);
		}
		const std::string diagnostic = published
			? "Complete generated animation batch and every resident typed cache switched at one protected boundary."
			: !resourcesPrepared
				? "Generated animation cache staging failed; publisher and all live caches retained their prior generation: "
					+ std::string(resourceBatch.Diagnostic())
				: "The coordinated generated animation commit was rejected; publisher and live caches retained their prior complete generation. "
					+ outcome.reason;
		RecordAnimationReimportStatus({absSourcePath, item.child->guid,
			item.candidate.assetType,
			published
				? AnimationReimportState::Published
				: AnimationReimportState::RetainedLastKnownGood,
			item.candidate.generation, active.activeGeneration,
			active.publicationBoundary, 0u,
			!published, diagnostic});
		if (!published)
		{
			// This is intentionally non-destructive: the publisher has retained a
			// previously accepted generation, and the next corrected import can retry.
			TERRAENGINE_LOG(LogLevel::Warning,
				"Animation reload candidate retained last-known-good generation for "
				<< item.child->semanticKey << ": " << diagnostic);
		}
	}
}

void AssetDatabase::UpdateModelPackageAnimationSummary(const std::filesystem::path& absSourcePath)
{
	const auto packageIt = assets.find(std::filesystem::absolute(absSourcePath));
	if(packageIt == assets.end()
		|| !TerraEngine::AssetPipeline::IsModelPackageImporterName(packageIt->second.importerName))
	{
		return;
	}

	ModelPackageAnimationSummaryMetadata summary;
	if(!TryGetModelPackageAnimationSummary(packageIt->second, summary))
	{
		return;
	}

	json summaryJson = {
		{"readable", summary.readable},
		{"publishedAtomically", summary.publishedAtomically},
		{"preservedLastKnownGood", summary.preservedLastKnownGood},
		{"prefabBindingsValid", summary.prefabBindingsValid},
		{"schemaVersion", summary.schemaVersion},
		{"candidateGeneration", summary.candidateGeneration},
		{"publishedGeneration", summary.publishedGeneration},
		{"lastKnownGoodGeneration", summary.lastKnownGoodGeneration},
		{"generatedChildCount", summary.generatedChildCount},
		{"readOnlyChildCount", summary.readOnlyChildCount},
		{"skeletonCount", summary.skeletonCount},
		{"animationClipCount", summary.animationClipCount},
		{"deformableMeshCount", summary.deformableMeshCount},
		{"morphTargetCount", summary.morphTargetCount},
		{"prefabCount", summary.prefabCount},
		{"infoCount", summary.infoCount},
		{"warningCount", summary.warningCount},
		{"errorCount", summary.errorCount},
		{"dependencyFingerprint", summary.dependencyFingerprint},
		{"diagnostics", summary.diagnostics}
	};

	AssetMetadata& writableMetadata = assets[std::filesystem::absolute(absSourcePath)];
	if(!writableMetadata.settings.is_object())
	{
		writableMetadata.settings = json::object();
	}
	const auto existing = writableMetadata.settings.find("animationPackage");
	if(existing != writableMetadata.settings.end() && *existing == summaryJson)
	{
		return;
	}

	// This updates only the parent package metadata. Manifest children remain
	// read-only views and never receive independent .meta sidecars.
	writableMetadata.settings["animationPackage"] = std::move(summaryJson);
	(void)SaveMetadata(writableMetadata);
	RequestAssetIndexSave();
}

void AssetDatabase::CleanupModelPackageGeneratedAssets(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath) {
	if (!TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
		return;
	}

	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	const bool loadedImportResult = TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult);
	std::vector<std::pair<TerraEngine::UUID, AssetType>> retiredRuntimeAliases;
	const auto retireRuntimeAlias = [&](const TerraEngine::UUID& guid, AssetType type) {
		if(guid == TerraEngine::EMPTY_UUID)
		{
			return;
		}
		const auto alreadyRetired = std::find(retiredRuntimeAliases.begin(), retiredRuntimeAliases.end(),
			std::pair<TerraEngine::UUID, AssetType>{guid, type});
		if(alreadyRetired != retiredRuntimeAliases.end())
		{
			return;
		}
		ResourceManager::Instance().DeleteResource(guid, type);
		retiredRuntimeAliases.emplace_back(guid, type);
	};

	// The session table is authoritative for aliases that were actually exposed.
	// Remove these first so deleted or renamed-away package children cannot remain
	// resolvable after their owning package leaves the database.
	if(const auto registeredIt = generatedModelPackageResources.find(metadata.guid);
		registeredIt != generatedModelPackageResources.end())
	{
		for(const auto& [guid, kind] : registeredIt->second)
		{
			if(const std::optional<AssetType> type = RuntimeAssetTypeForGeneratedKind(kind))
			{
				retireRuntimeAlias(guid, *type);
			}
		}
		generatedModelPackageResources.erase(registeredIt);
	}
	const auto isSafeGeneratedRoot = [](const std::filesystem::path& relativeRoot) {
		if (relativeRoot.empty() || relativeRoot.is_absolute()) {
			return false;
		}
		for (const std::filesystem::path& part : relativeRoot) {
			if (part == "..") {
				return false;
			}
		}
		const std::string folderName = relativeRoot.filename().string();
		const std::string suffix = "_Generated";
		return folderName.size() > suffix.size()
			&& folderName.compare(folderName.size() - suffix.size(), suffix.size(), suffix) == 0;
	};

	if (loadedImportResult) {
		for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes) {
			retireRuntimeAlias(mesh.guid, AssetType::Mesh);
		}
		for (const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures) {
			retireRuntimeAlias(texture.guid, AssetType::Texture);
		}
		for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials) {
			retireRuntimeAlias(material.guid, AssetType::Material);
			const std::filesystem::path materialSourcePath = std::filesystem::absolute(assetRoot / material.sourcePath);
			if (auto materialIt = assets.find(materialSourcePath); materialIt != assets.end()) {
				assets.erase(materialIt);
			}
			std::error_code removeMetaError;
			std::filesystem::remove(materialSourcePath.string() + ".meta", removeMetaError);
		}
		for (const TerraEngine::AssetPipeline::GeneratedModelPrefabAsset& prefab : importResult.generatedPrefabs) {
			const std::filesystem::path prefabSourcePath = std::filesystem::absolute(assetRoot / prefab.sourcePath);
			if (auto prefabIt = assets.find(prefabSourcePath); prefabIt != assets.end()) {
				assets.erase(prefabIt);
			}
			std::error_code removeMetaError;
			std::filesystem::remove(prefabSourcePath.string() + ".meta", removeMetaError);
		}
		for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : importResult.animationManifest.subAssets)
		{
			const std::optional<GeneratedModelPackageChildKind> kind = GeneratedKindForManifestKind(child.kind);
			if(kind)
			{
				if(const std::optional<AssetType> type = RuntimeAssetTypeForGeneratedKind(*kind))
				{
					retireRuntimeAlias(child.guid, *type);
				}
			}
		}
		generatedModelPackageResources.erase(importResult.packageGuid);
	}

	std::filesystem::path generatedRoot = loadedImportResult ? importResult.package.importRoot : std::filesystem::path();
	if (!isSafeGeneratedRoot(generatedRoot)) {
		generatedRoot = metadata.sourcePath.parent_path() / (absSourcePath.stem().string() + "_Generated");
	}

	std::error_code ec;
	if (isSafeGeneratedRoot(generatedRoot)) {
		const std::filesystem::path assetGeneratedRoot = std::filesystem::absolute(assetRoot / generatedRoot).lexically_normal();
		const std::filesystem::path buildGeneratedRoot = std::filesystem::absolute(buildRoot / generatedRoot).lexically_normal();

		for (auto assetIt = assets.begin(); assetIt != assets.end(); ) {
			const std::filesystem::path candidatePath = std::filesystem::absolute(assetIt->first).lexically_normal();
			std::error_code relativeError;
			const std::filesystem::path relativeToGeneratedRoot = std::filesystem::relative(candidatePath, assetGeneratedRoot, relativeError);
			const bool insideGeneratedRoot = !relativeError
				&& !relativeToGeneratedRoot.empty()
				&& *relativeToGeneratedRoot.begin() != "..";
			if (insideGeneratedRoot) {
				assetIt = assets.erase(assetIt);
			} else {
				++assetIt;
			}
		}

		std::filesystem::remove_all(assetGeneratedRoot, ec);
		ec.clear();
		std::filesystem::remove_all(buildGeneratedRoot, ec);
		ec.clear();
	}

	std::filesystem::remove(TerraEngine::AssetPipeline::GetImportResultPath(absSourcePath), ec);
	ec.clear();
	std::filesystem::remove(TerraEngine::AssetPipeline::GetImportResultPath(absSourcePath).string() + ".tmp", ec);
}

void AssetDatabase::CleanupCancelledImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool removeSourceAsset) {
	pendingEvents.erase(absSourcePath);
	// Retry ownership is managed by cancellation/admission, not by this old
	// generation's cleanup: a fresh request may already be waiting for settlement.

	if (!removeSourceAsset) {
		// Shutdown and automation cancellation only stop worker-owned work. The
		// compiler functions remove their unique staging files after the worker has
		// observed cancellation; authored source, .meta identity, live registration,
		// and the last atomically published output remain untouched here.
		TERRAENGINE_LOG(LogLevel::Info, "Cancelled asset import and preserved published asset state for: " << absSourcePath);
		return;
	}

	std::error_code ec;
	const bool registeredAssetRemoved = assets.find(absSourcePath) != assets.end();
	if (registeredAssetRemoved) {
		// RemoveAsset owns registered resource, package-child, compiled, metadata,
		// journal, and index cleanup. Do not repeat those side effects below.
		RemoveAsset(absSourcePath);
	} else {
		// A first import may not have reached registration yet. Explicit discard
		// still removes any final output or generated package state it created.
		if (!metadata.compiledPath.empty()) {
			std::filesystem::remove(buildRoot / metadata.compiledPath, ec);
			ec.clear();
		}

		const std::filesystem::path metaPath = absSourcePath.string() + ".meta";
		std::filesystem::remove(metaPath, ec);
		ec.clear();

		if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
			CleanupModelPackageGeneratedAssets(metadata, absSourcePath);
		}
	}

	std::filesystem::remove(absSourcePath, ec);
	TERRAENGINE_LOG(LogLevel::Info, "Cancelled asset import and destructively removed its asset state for: " << absSourcePath);
}

// AssetDatabase::DeleteAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::DeleteAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it == assets.end()) return;

	AssetMetadata metadata = it->second;
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;

	// Clean up Scene references ONLY during explicit Delete (user intent)
	SceneManager::Instance().NullifyAssetReferences(metadata.guid);

	// 1. Delete source file from disk.
	std::error_code ec;
	if (std::filesystem::exists(fullSourcePath)) {
		std::filesystem::remove(fullSourcePath, ec);
	}

	// 2. Delegate everything else (DB cleanup, engine notification, meta/compiled deletion) to RemoveAsset
	RemoveAsset(absPath);
	
	if (!ec) {
		TERRAENGINE_LOG(LogLevel::Info, "Deleted asset: " << fullSourcePath.filename().string());
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to delete source file " << fullSourcePath << ": " << ec.message());
	}
}

// AssetDatabase::GenerateDefaultMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::GenerateDefaultMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	outMetadata.guid = TerraEngine::GenerateUUID();
	outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
	outMetadata.importerName = GetImporterName(sourcePath);
	outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
	outMetadata.settings = json::object(); 

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outMetadata.importerName == "TextureImporter") {
		outMetadata.settings["generateMipmaps"] = true;
		outMetadata.settings["filterType"] = "Lanczos";
		outMetadata.settings["quality"] = 1;
		outMetadata.settings["semantic"] = "color";
	}
	if (outMetadata.importerName == "PhysicsColliderRecipeImporter") {
		// Recipes compile beside their source using the engine-owned envelope
		// extension; the GUID remains in the source .meta through later moves.
		outMetadata.compiledPath = outMetadata.sourcePath;
		outMetadata.compiledPath.replace_extension(".tepcoll");
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outMetadata.importerName == "ScriptImporter") {
		outMetadata.settings["compileStatus"] = "Unknown";
		outMetadata.settings["scriptClassName"] = "";
		outMetadata.settings["scriptNamespace"] = "";
		outMetadata.settings["scriptFullName"] = "";
		UpdateScriptMetadata(sourcePath, outMetadata);
	}
}

// AssetDatabase::GetImporterName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AssetDatabase::GetImporterName(const std::filesystem::path& filePath) const {
	std::string ext = filePath.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (TerraEngine::AssetPipeline::IsModelPackageSourceExtension(filePath)) return TerraEngine::AssetPipeline::GetModelPackageImporterName(filePath);
	if (IsSupportedMeshSourceExtension(filePath)) return "MeshImporter";
	if (IsSupportedTextureSourceExtension(filePath)) return "TextureImporter";
	if (ext == ".hlsl" || ext == ".glsl") return "ShaderImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tmat") return "MaterialImporter";
	// Physics material JSON, authored collider recipes, and compiled collider
	// envelopes use distinct importers so a move/rename retains their .meta GUID.
	if (ext == ".tephysmat") return "PhysicsMaterialImporter";
	if (ext == ".tecollider") return "PhysicsColliderRecipeImporter";
	if (ext == ".tepcoll") return "CookedColliderImporter";
	if (ext == ".teterrain") return "TerrainImporter";
	if (ext == ".wav" || ext == ".ogg") return "AudioClipImporter";
	if (ext == ".teaudio") return "CookedAudioClipImporter";
	// Navigation companions are complete runtime-decodable derived data. They do
	// not recursively compile when discovered by the asset watcher.
	if (ext == ".tenavgeom") return "NavigationGeometryCompanionImporter";
	if (ext == ".teskel") return "SkeletonAssetImporter";
	if (ext == ".teanim") return "AnimationClipAssetImporter";
	if (ext == ".teanimctrl") return "AnimationControllerAssetImporter";
	if (ext == ".teanimmask") return "AnimationMaskAssetImporter";
	if (ext == ".teretarget") return "RetargetProfileAssetImporter";
	if (ext == ".temotiondb") return "MotionDatabaseAssetImporter";
	if (ext == ".teragdoll") return "RagdollProfileAssetImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".cs") return "ScriptImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuiml") return "UIImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuss") return "StyleImporter";
	if (ext == ".teprefab") return "PrefabImporter";
	return "GenericImporter";
}

// AssetDatabase::UpdateScriptMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::UpdateScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& metadata) {
	std::ifstream input(sourcePath);
	if (!input) {
		metadata.settings["scriptClassName"] = "";
		metadata.settings["scriptNamespace"] = "";
		metadata.settings["scriptFullName"] = "";
		metadata.settings["isAttachable"] = false;
		return;
	}

	std::stringstream buffer;
	buffer << input.rdbuf();
	const std::string sourceText = buffer.str();

	// Keep script asset metadata cheap to refresh from the file watcher. The
	// regex pass only extracts the namespace, class name, and immediate base
	// type needed for inspector labels and attachability checks.
	static const std::regex namespacePattern(R"(\bnamespace\s+([A-Za-z_][A-Za-z0-9_\.]*))");
	static const std::regex classPattern(R"(\bclass\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*([A-Za-z_][A-Za-z0-9_\.]*))?)");

	std::smatch namespaceMatch;
	std::smatch classMatch;
	const std::string scriptNamespace = std::regex_search(sourceText, namespaceMatch, namespacePattern) ? namespaceMatch[1].str() : std::string{};
	const std::string className = std::regex_search(sourceText, classMatch, classPattern) ? classMatch[1].str() : std::string{};
	const std::string baseType = classMatch.size() > 2 ? classMatch[2].str() : std::string{};
	const bool derivesFromScript = baseType == "Script" || baseType == "TerraEngine.Script" || baseType.ends_with(".Script");

	metadata.settings["scriptClassName"] = className;
	metadata.settings["scriptNamespace"] = scriptNamespace;
	metadata.settings["scriptFullName"] = scriptNamespace.empty() ? className : (scriptNamespace + "." + className);
	metadata.settings["isAttachable"] = derivesFromScript;
}

// AssetDatabase::PruneRecentlyRemovedAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::PruneRecentlyRemovedAssets() {
	// Remove rename tombstones once the short reconciliation window expires so
	// the cache cannot grow unbounded during long editor sessions.
	const auto now = std::chrono::steady_clock::now();
	recentlyRemovedAssets.erase(
		std::remove_if(
			recentlyRemovedAssets.begin(),
			recentlyRemovedAssets.end(),
			[&](const RecentlyRemovedAsset& removedAsset) {
				return removedAsset.expiresAt <= now;
			}),
		recentlyRemovedAssets.end());
}

// AssetDatabase::TryReuseRecentlyRemovedScriptMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::TryReuseRecentlyRemovedScriptMetadata(const std::filesystem::path& sourcePath, AssetMetadata& outMetadata) {
	if (GetImporterName(sourcePath) != "ScriptImporter") {
		return false;
	}

	AssetMetadata currentScriptMetadata;
	currentScriptMetadata.importerName = "ScriptImporter";
	currentScriptMetadata.settings = json::object();
	UpdateScriptMetadata(sourcePath, currentScriptMetadata);

	const std::string currentFullName = ReadScriptMetadataString(currentScriptMetadata, "scriptFullName");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentFullName.empty()) {
		return false;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto it = recentlyRemovedAssets.begin(); it != recentlyRemovedAssets.end(); ++it) {
		if (it->metadata.importerName != "ScriptImporter") {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ReadScriptMetadataString(it->metadata, "scriptFullName") != currentFullName) {
			continue;
		}

		// Preserve the previous GUID when a filesystem rename arrives as an
		// erase + create pair instead of a metadata-preserving move.
		outMetadata = it->metadata;
		outMetadata.lastModified = std::filesystem::last_write_time(sourcePath);
		outMetadata.sourcePath = std::filesystem::relative(sourcePath, assetRoot);
		outMetadata.importerName = "ScriptImporter";
		recentlyRemovedAssets.erase(it);
		TERRAENGINE_LOG(LogLevel::Info, "Reused existing script metadata for renamed asset " << sourcePath << " so its GUID stays stable.");
		return true;
	}

	return false;
}

// AssetDatabase::LoadMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::LoadMetadata(const std::filesystem::path& metaFilePath, AssetMetadata& outMetadata) {
	std::ifstream file(metaFilePath);
	if (!file.is_open()) return false;
	try {
		json j;
		file >> j;
		outMetadata.guid = StringToUuid(j["guid"].get<std::string>());
		outMetadata.importerName = j["importerName"].get<std::string>();
		using FileDuration = std::filesystem::file_time_type::duration;
		// Keep the raw duration units intact so we compare the same clock ticks recorded on disk.
		FileDuration::rep storedTicks = j["lastModified"].get<FileDuration::rep>();
		outMetadata.lastModified = std::filesystem::file_time_type(FileDuration(storedTicks));
		outMetadata.sourcePath = j["sourcePath"].get<std::string>();
		outMetadata.compiledPath = j["compiledPath"].get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (j.contains("settings")) outMetadata.settings = j["settings"];
		return true;
	} catch (...) { return false; }
}

// AssetDatabase::SaveMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::SaveMetadata(const AssetMetadata& metadata) {
	std::filesystem::path metaFilePath = (assetRoot / metadata.sourcePath).string() + ".meta";
	std::ofstream file(metaFilePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!file.is_open()) return false;
	try {
		json j;
		j["guid"] = UuidToString(metadata.guid);
		j["importerName"] = metadata.importerName;
		// Persist the file clock's native tick count so reloads compare identical units and avoid false recompiles.
		j["lastModified"] = metadata.lastModified.time_since_epoch().count();
		j["sourcePath"] = metadata.sourcePath.string();
		j["compiledPath"] = metadata.compiledPath.string();
		j["settings"] = metadata.settings;
		file << std::setw(4) << j << std::endl;
		return true;
	} catch (...) { return false; }
}

bool AssetDatabase::CompileModelPackageWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState) {
	// Every process writes only private roots. The owner publishes the complete
	// candidate after physical process settlement and its final authority check.
	using namespace TerraEngine::AssetPipeline;
	const auto fullSourcePath = assetRoot / metadata.sourcePath;
	const auto token = UuidToString(TerraEngine::GenerateUUID());
	const ModelPackagePreparationRoots preparation{assetRoot / (".ta-" + token), buildRoot / (".tb-" + token)};
	struct CandidateCleanup {
		const ModelPackagePreparationRoots& roots;
		~CandidateCleanup() noexcept {
			std::error_code ignored;
			std::filesystem::remove_all(roots.assetRoot, ignored);
			std::filesystem::remove_all(roots.buildRoot, ignored);
		}
	} cleanup{preparation};
	ModelPackageImportOptions options{};
	options.sourcePath = fullSourcePath;
	options.assetRoot = assetRoot;
	options.buildRoot = buildRoot;
	options.sourceRelativePath = metadata.sourcePath;
	options.packageGuid = metadata.guid;
	options.shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	options.forceReimport = true;
	options.preparationRoots = preparation;
	const auto candidateSidecar = GetImportResultPath(preparation.assetRoot / metadata.sourcePath);
#ifdef _WIN32
	const auto compilerPath = GetEditorExecutableDirectory() / kModelPackageCompilerName;
	if (!std::filesystem::exists(compilerPath)) return false;
	const std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --source " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --asset-root " + QuoteCommandLineArgument(assetRoot)
		+ L" --build-root " + QuoteCommandLineArgument(buildRoot)
		+ L" --source-relative " + QuoteCommandLineArgument(metadata.sourcePath)
		+ L" --package-guid " + QuoteCommandLineArgument(UuidToString(metadata.guid))
		+ L" --shader-guid " + QuoteCommandLineArgument(UuidToString(options.shaderCollectionId))
		+ L" --prepare-asset-root " + QuoteCommandLineArgument(preparation.assetRoot)
		+ L" --prepare-build-root " + QuoteCommandLineArgument(preparation.buildRoot);
	if (!RunCompilerProcess(compilerPath, commandLine, progressState, "Model package compiler", candidateSidecar)) return false;
#else
	options.progressCallback = [this, progressState](float progress, std::string_view stage, std::string_view message) {
		if (progressState) SetImportProgress(progressState, std::string(stage), std::string(message), progress, false, false);
	};
	options.cancellationRequested = [progressState] { return progressState && progressState->cancelRequested.load(); };
	// Read prior recipes from the authoritative sidecar, never from empty private
	// roots. This mirrors the process compiler's retained per-mesh overrides.
	ModelPackageImportResult previous;
	if (LoadModelPackageImportResult(fullSourcePath, previous)) {
		for (const auto& mesh : previous.generatedMeshes) if (mesh.virtualGeometrySettings.hasOverride)
			options.virtualGeometryMeshOverrides.push_back({mesh.guid, mesh.virtualGeometrySettings});
	}
	const auto result = ImportModelPackage(options);
	if (result.status == ModelImportStatus::Failed || !SaveModelPackageImportResult(preparation.assetRoot / metadata.sourcePath, result)) return false;
#endif
#ifdef TERRAENGINE_EDITOR_TESTS
	if (progressState && progressState->beforePublicationHook)
		progressState->beforePublicationHook(fullSourcePath, ImportAdmissionBoundaryForTests::BeforePublication);
#endif
	if (!progressState) return PublishPreparedModelPackage(preparation, options);
	std::lock_guard lifecycle(importLifecycleMutex);
	std::lock_guard publication(progressState->publicationMutex);
	if (!importAdmissionOpen || progressState->projectGeneration != importProjectGeneration || progressState->cancelRequested.load()) return false;
	if (!PublishPreparedModelPackage(preparation, options)) return false;
	progressState->publicationCompleted = true;
	return true;
}

bool AssetDatabase::RunCompilerProcess(const std::filesystem::path& compilerPath, const std::wstring& commandLine, const std::shared_ptr<AsyncImportProgressState>& progressState, std::string_view logPrefix, const std::filesystem::path& expectedOutputPath) {
#ifdef _WIN32
	SECURITY_ATTRIBUTES securityAttributes{};
	securityAttributes.nLength = sizeof(securityAttributes);
	securityAttributes.bInheritHandle = TRUE;

	HANDLE readPipe = nullptr;
	HANDLE writePipe = nullptr;
	PROCESS_INFORMATION processInfo{};
	// Own pipes and the native process even when line capture/progress parsing
	// throws. A failed worker cannot release its import slot while this child lives.
	struct CompilerOwnership {
		HANDLE& read; HANDLE& write; PROCESS_INFORMATION& process;
		const std::shared_ptr<AsyncImportProgressState>& progress;
		bool settled = false;
		~CompilerOwnership() noexcept {
			if (process.hProcess) {
				if (!settled) { TerminateProcess(process.hProcess, 1U); WaitForSingleObject(process.hProcess, INFINITE); }
				if (progress) { try {
					std::lock_guard lock(progress->processMutex);
					if (progress->compilerProcessHandle == process.hProcess) progress->compilerProcessHandle = nullptr;
				} catch (...) {} }
				CloseHandle(process.hThread); CloseHandle(process.hProcess);
			}
			if (read) CloseHandle(read);
			if (write) CloseHandle(write);
		}
	} ownership{readPipe, writePipe, processInfo, progressState};
	if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0u)) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create compiler output pipe for " << compilerPath);
		return false;
	}
	SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0u);

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.dwFlags = STARTF_USESTDHANDLES;
	startupInfo.hStdOutput = writePipe;
	startupInfo.hStdError = writePipe;
	startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');
	const std::wstring workingDirectory = compilerPath.parent_path().wstring();
	const BOOL created = CreateProcessW(
		compilerPath.wstring().c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		TRUE,
		CREATE_NO_WINDOW,
		nullptr,
		workingDirectory.c_str(),
		&startupInfo,
		&processInfo);
	CloseHandle(writePipe);
	writePipe = nullptr;

	if (!created) {
		CloseHandle(readPipe);
		readPipe = nullptr;
		TERRAENGINE_LOG(LogLevel::Error, "Failed to launch compiler: " << compilerPath);
		return false;
	}

	if (progressState) {
		std::lock_guard<std::mutex> lock(progressState->processMutex);
		progressState->compilerProcessHandle = processInfo.hProcess;
		if (progressState->cancelRequested.load()) {
			TerminateProcess(processInfo.hProcess, 1u);
		}
	}

	std::string pendingLine;
	char buffer[4096]{};
	DWORD bytesRead = 0u;
	while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) && bytesRead > 0u) {
		if (progressState && progressState->cancelRequested.load()) {
			TerminateProcess(processInfo.hProcess, 1u);
		}
		pendingLine.append(buffer, buffer + bytesRead);
		std::size_t newline = std::string::npos;
		while ((newline = pendingLine.find('\n')) != std::string::npos) {
			std::string line = pendingLine.substr(0u, newline);
			pendingLine.erase(0u, newline + 1u);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				continue;
			}
			try {
				const json progressDocument = json::parse(line);
				if (progressDocument.value("type", std::string()) == "progress") {
					SetImportProgress(
						progressState,
						progressDocument.value("stage", std::string("Importing")),
						progressDocument.value("message", std::string()),
						progressDocument.value("progress", 0.0f),
						false,
						false);
				}
			} catch (const AssetImportCancelledException&) {
				TerminateProcess(processInfo.hProcess, 1u);
			} catch (const std::exception&) {
				TERRAENGINE_LOG(LogLevel::Info, logPrefix << ": " << line);
			}
		}
	}
	CloseHandle(readPipe);
	readPipe = nullptr;

	WaitForSingleObject(processInfo.hProcess, INFINITE);
	ownership.settled = true;
	DWORD exitCode = 1u;
	GetExitCodeProcess(processInfo.hProcess, &exitCode);
	if (progressState) {
		std::lock_guard<std::mutex> lock(progressState->processMutex);
		if (progressState->compilerProcessHandle == processInfo.hProcess) {
			progressState->compilerProcessHandle = nullptr;
		}
	}
	return exitCode == 0u && (expectedOutputPath.empty() || std::filesystem::exists(expectedOutputPath));
#else
	(void)compilerPath;
	(void)commandLine;
	(void)progressState;
	(void)logPrefix;
	(void)expectedOutputPath;
	return false;
#endif
}

bool AssetDatabase::CompileMeshWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState, std::optional<VirtualGeometryAdaptiveSettingsMetadata> resolvedMeshSettings) {
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	const std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	const std::filesystem::path stagedCompiledPath = fullCompiledPath.string() + "." + UuidToString(metadata.guid) + ".tmp";
	const std::filesystem::path stagedMeshReportPath = stagedCompiledPath.string() + ".meshreport";
	std::error_code cleanupError;
	std::filesystem::remove(stagedCompiledPath, cleanupError);
	std::filesystem::remove(stagedMeshReportPath, cleanupError);

	VirtualGeometryAdaptiveSettingsMetadata resolvedSettings;
	if (resolvedMeshSettings.has_value()) {
		resolvedSettings = *resolvedMeshSettings;
	} else {
		TryGetVirtualGeometryAdaptiveSettings(metadata, resolvedSettings);
	}
	if (!resolvedSettings.valid) {
		TERRAENGINE_LOG(LogLevel::Warning, "Invalid virtual geometry override for " << fullSourcePath << ": " << JoinValidationErrors(resolvedSettings.validationErrors));
	}
	const VirtualGeometryAdaptiveCompileSettings compileSettings = ToCompileVirtualGeometrySettings(resolvedSettings);
	const std::string privateIdentity = UuidToString(metadata.guid);
	const std::filesystem::path cacheCompilerPath =
		GetEditorExecutableDirectory() / kMeshCompilerName;
	const std::string stageCacheKey = BuildVirtualGeometryStageCacheKey(
		fullSourcePath, compileSettings, cacheCompilerPath);
	const std::filesystem::path stageCachePath =
		buildRoot / ".virtual-geometry-cache" / stageCacheKey / "mesh.temf";

	// Cache reuse still stages privately and enters the same protected
	// publication boundary as a fresh compiler result. Cancellation therefore
	// cannot make a cache hit visible after the caller has rejected the job.
	if(!stageCacheKey.empty()
		&& ValidateVirtualGeometryCachedMesh(stageCachePath)
		&& (!progressState || !progressState->cancelRequested.load()))
	{
		std::error_code cacheError;
		std::filesystem::copy_file(
			stageCachePath,
			stagedCompiledPath,
			std::filesystem::copy_options::overwrite_existing,
			cacheError);
		if(!cacheError
			&& ValidateVirtualGeometryCachedMesh(stagedCompiledPath)
			&& PublishCompiledAssetFromWorker(
				stagedCompiledPath, fullCompiledPath, progressState))
		{
			PublishVirtualGeometryCacheProvenance(
				fullCompiledPath,
				stageCacheKey,
				"hit",
				"verified content-addressed complete-mesh stage",
				privateIdentity);
			std::filesystem::remove(stagedMeshReportPath, cacheError);
			return true;
		}
		std::filesystem::remove(stagedCompiledPath, cacheError);
	}

#ifdef _WIN32
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kMeshCompilerName;
	if (!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Mesh compiler executable is missing: " << compilerPath);
		return false;
	}

	std::ostringstream targetRatios;
	for (std::size_t index = 0u; index < compileSettings.targetRatios.size(); ++index) {
		if (index != 0u) {
			targetRatios << ",";
		}
		targetRatios << compileSettings.targetRatios[index];
	}

	const std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --input " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --output " + QuoteCommandLineArgument(stagedCompiledPath)
		+ L" --adaptive-target-ratios " + QuoteCommandLineArgument(targetRatios.str())
		+ L" --adaptive-min-source-triangles " + QuoteCommandLineArgument(std::to_string(compileSettings.minimumSourceTriangles))
		+ L" --adaptive-min-generated-triangles " + QuoteCommandLineArgument(std::to_string(compileSettings.minimumGeneratedTriangles))
		+ L" --adaptive-min-meshlets-per-level " + QuoteCommandLineArgument(std::to_string(compileSettings.minimumMeshletsPerGeneratedLevel))
		+ L" --adaptive-min-triangle-reduction " + QuoteCommandLineArgument(std::to_string(compileSettings.minimumUsefulTriangleReduction))
		+ L" --adaptive-min-meshlet-reduction " + QuoteCommandLineArgument(std::to_string(compileSettings.minimumUsefulMeshletReduction))
		+ L" --adaptive-max-geometric-error " + QuoteCommandLineArgument(std::to_string(compileSettings.maximumGeometricError))
		+ L" --adaptive-settings-source " + QuoteCommandLineArgument(compileSettings.settingsSource);

	if (!RunCompilerProcess(compilerPath, commandLine, progressState, "Mesh compiler", stagedCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
	const bool cachePublished =
		!stageCacheKey.empty()
		&& (!progressState || !progressState->cancelRequested.load())
		&& PublishVirtualGeometryStageCache(
			stagedCompiledPath, stageCachePath, privateIdentity);
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
	PublishVirtualGeometryCacheProvenance(
		fullCompiledPath,
		stageCacheKey,
		cachePublished ? "miss-published" : "miss-cache-unavailable",
		cachePublished
			? "fresh compiler output validated and published to stage cache"
			: "fresh compiler output published live; cache publication failed",
		privateIdentity);
	std::filesystem::remove(stagedMeshReportPath, cleanupError);
	return true;
#else
	const MeshCompileProgressCallback progressCallback = [this, progressState](float progress, std::string_view stage, std::string_view message) {
		if (progressState) {
			SetImportProgress(progressState, std::string(stage), std::string(message), progress, false, false);
		}
	};
	if (!CompileMesh(fullSourcePath, stagedCompiledPath, true, nullptr, 60.0f, 1e-6f, &compileSettings, progressCallback)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
	const bool cachePublished =
		!stageCacheKey.empty()
		&& (!progressState || !progressState->cancelRequested.load())
		&& PublishVirtualGeometryStageCache(
			stagedCompiledPath, stageCachePath, privateIdentity);
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
	PublishVirtualGeometryCacheProvenance(
		fullCompiledPath,
		stageCacheKey,
		cachePublished ? "miss-published" : "miss-cache-unavailable",
		cachePublished
			? "fresh compiler output validated and published to stage cache"
			: "fresh compiler output published live; cache publication failed",
		privateIdentity);
	std::filesystem::remove(stagedMeshReportPath, cleanupError);
	return true;
#endif
}

bool AssetDatabase::CompileTextureWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState) {
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	const std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	const std::filesystem::path stagedCompiledPath = fullCompiledPath.string() + "." + UuidToString(metadata.guid) + ".tmp";
	std::error_code cleanupError;
	std::filesystem::remove(stagedCompiledPath, cleanupError);

	std::string semanticStr = "color";
	if (metadata.settings.contains("semantic")) semanticStr = metadata.settings["semantic"];
	TextureFileSemantic semantic = semanticStr == "data" ? TextureFileSemantic::Data : TextureFileSemantic::Color;

	std::uint32_t quality = 1;
	if (metadata.settings.contains("quality")) {
		quality = static_cast<std::uint32_t>(metadata.settings["quality"].get<int>());
	}

	bool generateMipmaps = true;
	if (metadata.settings.contains("generateMipmaps")) {
		generateMipmaps = metadata.settings["generateMipmaps"].get<bool>();
	}

	std::string filterStr = "Lanczos";
	if (metadata.settings.contains("filterType")) {
		filterStr = metadata.settings["filterType"].get<std::string>();
	}

#ifdef _WIN32
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kTextureCompilerName;
	if (!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Texture compiler executable is missing: " << compilerPath);
		return false;
	}
	const std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --input " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --output " + QuoteCommandLineArgument(stagedCompiledPath)
		+ L" --semantic " + QuoteCommandLineArgument(semantic == TextureFileSemantic::Data ? "data" : "color")
		+ L" --quality " + QuoteCommandLineArgument(std::to_string(quality))
		+ (generateMipmaps ? L" --generate-mipmaps" : L" --no-mipmaps")
		+ L" --filter " + QuoteCommandLineArgument(filterStr);

	if (!RunCompilerProcess(compilerPath, commandLine, progressState, "Texture compiler", stagedCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	return true;
#else
	TextureFilterType filter = TextureFilterType::Lanczos;
	if (filterStr == "Box") filter = TextureFilterType::Box;
	else if (filterStr == "Triangle") filter = TextureFilterType::Triangle;
	else if (filterStr == "Kaiser") filter = TextureFilterType::Kaiser;
	const TextureCompileProgressCallback progressCallback = [this, progressState](float progress, std::string_view stage, std::string_view message) {
		if (progressState) {
			SetImportProgress(progressState, std::string(stage), std::string(message), progress, false, false);
		}
	};
	if (!CompileTexture(fullSourcePath, stagedCompiledPath, semantic, quality, generateMipmaps, filter, progressCallback)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	return true;
#endif
}

bool AssetDatabase::CompileShaderWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState) {
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	const std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	const std::filesystem::path stagedCompiledPath = fullCompiledPath.string() + "." + UuidToString(metadata.guid) + ".tmp";
	const std::filesystem::path stagedShaderBytecodePath = std::filesystem::path(stagedCompiledPath).replace_extension(".spv");
	std::error_code cleanupError;
	std::filesystem::remove(stagedCompiledPath, cleanupError);
	std::filesystem::remove(stagedShaderBytecodePath, cleanupError);

	std::string stage = "vs";
	if (metadata.settings.contains("stage")) {
		stage = metadata.settings["stage"];
	} else {
		const std::string filename = fullSourcePath.filename().string();
		if (filename.find("frag") != std::string::npos || filename.find("pixel") != std::string::npos) {
			stage = "ps";
		}
	}
	std::string target;
	if (metadata.settings.contains("target")) {
		target = metadata.settings["target"];
	}
	std::string entry = "main";
	if (metadata.settings.contains("entryPoint")) {
		entry = metadata.settings["entryPoint"];
	}

#ifdef _WIN32
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kShaderCompilerName;
	if (!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Shader compiler executable is missing: " << compilerPath);
		return false;
	}

	std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --input " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --output " + QuoteCommandLineArgument(stagedCompiledPath)
		+ L" --stage " + QuoteCommandLineArgument(stage)
		+ L" --entry " + QuoteCommandLineArgument(entry);
	if (!target.empty()) {
		commandLine += L" --target " + QuoteCommandLineArgument(target);
	}

	if (!RunCompilerProcess(compilerPath, commandLine, progressState, "Shader compiler", stagedCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedShaderBytecodePath, cleanupError);
		return false;
	}
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedShaderBytecodePath, cleanupError);
		return false;
	}
	return true;
#else
	std::filesystem::path toolsDir = projectRoot / "out/build/Build/Debug";
	if (!std::filesystem::exists(toolsDir / "dxc.exe") && !std::filesystem::exists(toolsDir / "dxc")) {
		toolsDir = std::filesystem::current_path();
	}
	if (!std::filesystem::exists(toolsDir / "dxc.exe") && !std::filesystem::exists(toolsDir / "dxc")) {
		toolsDir = projectRoot / "out/build_vs_18/Build/Editor";
	}

#if defined(_WIN32)
	std::filesystem::path dxcPath = toolsDir / "dxc.exe";
#else
	std::filesystem::path dxcPath = toolsDir / "dxc";
#endif
	if (!CompileShader(fullSourcePath, stagedCompiledPath, stage, target, entry, dxcPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	if (!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	return true;
#endif
}

bool AssetDatabase::CompilePhysicsColliderWithProcess(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState) {
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	const std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	// The headless compiler validates the output extension before it cooks. Keep
	// the editor-owned staging filename unique while retaining `.tepcoll`.
	const std::filesystem::path stagedCompiledPath = fullCompiledPath.parent_path()
		/ (fullCompiledPath.stem().string() + "." + UuidToString(metadata.guid) + ".tepcoll");
	std::error_code cleanupError;
	std::filesystem::remove(stagedCompiledPath, cleanupError);
	const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "PhysicsSettings.json";

#ifdef _WIN32
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kPhysicsColliderCompilerName;
	if(!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Physics collider compiler executable is missing: " << compilerPath);
		return false;
	}
	std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --input " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --output " + QuoteCommandLineArgument(stagedCompiledPath)
		+ L" --asset-id " + QuoteCommandLineArgument(UuidToString(metadata.guid));
	if(std::filesystem::exists(settingsPath)) {
		commandLine += L" --settings " + QuoteCommandLineArgument(settingsPath);
	}
	if(!RunCompilerProcess(compilerPath, commandLine, progressState, "Physics collider compiler", stagedCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	if(!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	return true;
#else
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kPhysicsColliderCompilerName;
	if(!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Physics collider compiler executable is missing: " << compilerPath);
		return false;
	}
	const std::string compiler = compilerPath.string();
	const std::string input = fullSourcePath.string();
	const std::string output = stagedCompiledPath.string();
	const std::string assetId = UuidToString(metadata.guid);
	const std::string settings = settingsPath.string();
	const bool hasSettings = std::filesystem::exists(settingsPath);
	const pid_t processId = fork();
	if(processId == 0) {
		if(hasSettings) {
			execl(compiler.c_str(), compiler.c_str(), "--input", input.c_str(), "--output", output.c_str(),
				"--asset-id", assetId.c_str(), "--settings", settings.c_str(), static_cast<char*>(nullptr));
		} else {
			execl(compiler.c_str(), compiler.c_str(), "--input", input.c_str(), "--output", output.c_str(),
				"--asset-id", assetId.c_str(), static_cast<char*>(nullptr));
		}
		_exit(127);
	}
	if(processId < 0) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to launch physics collider compiler: " << compilerPath);
		return false;
	}
	if(progressState) {
		std::lock_guard<std::mutex> lock(progressState->processMutex);
		progressState->compilerProcessHandle = reinterpret_cast<void*>(static_cast<std::intptr_t>(processId));
	}
	int processStatus = 0;
	while(waitpid(processId, &processStatus, WNOHANG) == 0) {
		if(progressState && progressState->cancelRequested.load()) {
			(void)kill(processId, SIGTERM);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if(progressState) {
		std::lock_guard<std::mutex> lock(progressState->processMutex);
		progressState->compilerProcessHandle = nullptr;
	}
	if(!WIFEXITED(processStatus) || WEXITSTATUS(processStatus) != 0 || !std::filesystem::exists(stagedCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	if(!PublishCompiledAssetFromWorker(stagedCompiledPath, fullCompiledPath, progressState)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		return false;
	}
	return true;
#endif
}

// AssetDatabase::CompileAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool AssetDatabase::CompileAsset(const AssetMetadata& metadata, const std::shared_ptr<AsyncImportProgressState>& progressState, std::optional<VirtualGeometryAdaptiveSettingsMetadata> resolvedMeshSettings) {
	if (metadata.compiledPath.empty()) return true;

	// Resolve full paths using buildRoot
	std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;
	
	fullSourcePath.make_preferred();
	fullCompiledPath.make_preferred();

	TERRAENGINE_LOG(LogLevel::Info, "Compiling Asset via Internal Compiler: " << fullSourcePath);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
		return CompileModelPackageWithProcess(metadata, progressState);
	}
	else if (metadata.importerName == "MeshImporter") {
		return CompileMeshWithProcess(metadata, progressState, resolvedMeshSettings);
	} 
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (metadata.importerName == "TextureImporter") {
		return CompileTextureWithProcess(metadata, progressState);
	} 
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (metadata.importerName == "ShaderImporter") {
		return CompileShaderWithProcess(metadata, progressState);
	} 
	else if(metadata.importerName == "PhysicsColliderRecipeImporter") {
		return CompilePhysicsColliderWithProcess(metadata, progressState);
	}
	else if(metadata.importerName == "AudioClipImporter") {
		// Cook into a private sibling first. Only the database's generation/cancel
		// arbiter may replace the authoritative artifact after validation completes.
		const auto stagedPath = fullCompiledPath.parent_path()
			/ (fullCompiledPath.stem().string() + ".staged" + fullCompiledPath.extension().string());
		const std::filesystem::path decoderStagingPath = stagedPath.string() + ".staged";
		struct AudioCandidateCleanup {
			const std::filesystem::path& path; const std::filesystem::path& decoderPath;
			~AudioCandidateCleanup() noexcept {
				std::error_code ignored; std::filesystem::remove(path, ignored);
				std::filesystem::remove(decoderPath, ignored);
			}
		} cleanup{stagedPath, decoderStagingPath};
		const TerraEngine::AssetPipeline::AudioClipCompileReport report =
			TerraEngine::AssetPipeline::CompileAudioClip({fullSourcePath, stagedPath, metadata.guid},
				progressState ? &progressState->cancelRequested : nullptr);
		if(!report.Succeeded()) {
			TERRAENGINE_LOG(LogLevel::Error, "Audio clip compile failed ("
				<< TerraEngine::AssetPipeline::ToString(report.status) << "): " << fullSourcePath);
			return false;
		}
#ifdef TERRAENGINE_EDITOR_TESTS
		if (progressState && progressState->beforePublicationHook)
			progressState->beforePublicationHook(fullSourcePath, ImportAdmissionBoundaryForTests::BeforePublication);
#endif
		if (!PublishCompiledAssetFromWorker(stagedPath, fullCompiledPath, progressState)) {
			std::error_code ignored;
			std::filesystem::remove(stagedPath, ignored);
			return false;
		}
		return true;
	}
	else if(metadata.importerName == "PhysicsMaterialImporter" || metadata.importerName == "CookedColliderImporter"
		|| RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName).has_value()) {
		// Cooked animation assets are already versioned runtime envelopes. Copy
		// through staging rather than reparsing/re-serializing them so the watcher
		// publishes a complete generation or leaves the prior build artifact intact.
		const std::filesystem::path stagedPath = fullCompiledPath.parent_path()
			/ (fullCompiledPath.stem().string() + ".staged" + fullCompiledPath.extension().string());
		std::error_code error;
		std::filesystem::create_directories(fullCompiledPath.parent_path(), error);
		if(error) return false;
		std::filesystem::copy_file(fullSourcePath, stagedPath, std::filesystem::copy_options::overwrite_existing, error);
		if(error) return false;
		if (const std::optional<AssetType> animationType =
			RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName))
		{
			const AnimationResourceValidationResult validation =
				ResourceManager::Instance().ValidateAnimationAssetFile(stagedPath, *animationType);
			if (!validation.Passed() || validation.assetId != metadata.guid)
			{
				std::filesystem::remove(stagedPath, error);
				RecordAnimationReimportStatus({fullSourcePath, metadata.guid,
					*ReloadAssetTypeForRuntimeType(*animationType), AnimationReimportState::RetainedLastKnownGood,
					0u, ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						metadata.guid, *animationType), 0u, 0u, true,
					validation.Passed()
						? "Cooked envelope GUID does not match its durable asset metadata; prior output was retained."
						: validation.diagnosticCode + ": " + validation.message});
				return false;
			}
		}
		if(!PublishCompiledAssetFromWorker(stagedPath, fullCompiledPath, progressState)) {
			std::filesystem::remove(stagedPath, error);
			return false;
		}
		return true;
	}
	else if (metadata.importerName == "MaterialImporter") {
		std::error_code ec;
		std::filesystem::copy_file(fullSourcePath, fullCompiledPath, std::filesystem::copy_options::overwrite_existing, ec);
		return !ec;
	}
	else {
		return true;
	}
}

bool AssetDatabase::UnregisterProvisionalTerrainCopy(const std::filesystem::path& sourcePath,
	const TerraEngine::UUID& expectedId, std::string& outError) {
	// All identity checks precede any mutation. A reused path or UUID belongs to
	// its new owner and must survive cleanup of an older failed operation.
	const auto path = std::filesystem::absolute(sourcePath).lexically_normal();
	const auto relative = path.lexically_relative(std::filesystem::absolute(assetRoot).lexically_normal());
	if(expectedId == TerraEngine::EMPTY_UUID || relative.empty() || relative.is_absolute()
		|| *relative.begin() == ".." || path.extension() != ".teterrain") {
		outError = "Terrain copy cleanup path or identity is outside its asset ownership.";
		return false;
	}
	const auto found = assets.find(path);
	if(found != assets.end() && (found->second.guid != expectedId
		|| found->second.importerName != "TerrainImporter")) {
		outError = "Terrain copy cleanup found a different registration at the reserved path.";
		return false;
	}
	for(const auto& [registeredPath, metadata] : assets) {
		if(metadata.guid == expectedId && registeredPath.lexically_normal() != path) {
			outError = "Terrain copy cleanup identity has another registered owner.";
			return false;
		}
	}
	ResourceManager& resources = ResourceManager::Instance();
	const auto resourcePath = resources.GetRegisteredAssetPath(expectedId, AssetType::Terrain);
	if(!resourcePath.empty() && std::filesystem::absolute(resourcePath).lexically_normal() != path) {
		outError = "Terrain copy cleanup resource path belongs to another owner.";
		return false;
	}
	// Resource removal and metadata removal are idempotent. File cleanup happens
	// later in the service, which retains its obligation if any step throws.
	if(!resourcePath.empty()) resources.DeleteResource(expectedId, AssetType::Terrain);
	if(found != assets.end()) assets.erase(found);
	RequestAssetIndexSave();
	outError.clear();
	return true;
}

// AssetDatabase::RemoveAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::RemoveAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it != assets.end()) {
		// Keep a value copy because publication happens after erasing the map entry.
		// Retaining a reference here would make the journal read freed metadata.
		const AssetMetadata metadata = it->second;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "ScriptImporter") {
			// Keep a short-lived tombstone so a rename can reattach the script
			// to its previous metadata and preserve the original GUID.
			recentlyRemovedAssets.push_back({ metadata, absPath, std::chrono::steady_clock::now() + kRecentlyRemovedAssetRetention });
		}

		AssetType type;
		bool knownResource = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) { type = AssetType::Mesh; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; knownResource = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; knownResource = true; }
	else if (metadata.importerName == "PhysicsMaterialImporter") { type = AssetType::PhysicsMaterial; knownResource = true; }
	else if (metadata.importerName == "TerrainImporter") { type = AssetType::Terrain; knownResource = true; }
		else if (metadata.importerName == "PhysicsColliderRecipeImporter"
			|| metadata.importerName == "CookedColliderImporter") { type = AssetType::CookedCollider; knownResource = true; }
		else if(metadata.importerName == "AudioClipImporter") { type = AssetType::AudioClip; knownResource = true; }
		else if (const std::optional<AssetType> animationType = RuntimeAssetTypeForCookedAnimationImporter(metadata.importerName)) {
			type = *animationType;
			knownResource = true;
		}

		if (knownResource) {
			ResourceManager::Instance().DeleteResource(metadata.guid, type);
		}
		if(metadata.importerName == "MaterialImporter") {
			// Deletion deliberately records a fallback condition instead of touching
			// terrain blobs. Restoring/reimporting this GUID reactivates the layer.
			RecordTerrainMaterialChange(metadata, TerrainMaterialChangeKind::Deleted);
		}

		if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
			CleanupModelPackageGeneratedAssets(metadata, absPath);
		}

		std::error_code ec;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!metadata.compiledPath.empty()) {
			std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			if (std::filesystem::exists(fullCompiled)) std::filesystem::remove(fullCompiled, ec);
		}
		
		std::filesystem::path meta = absPath.string() + ".meta";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(meta)) std::filesystem::remove(meta, ec);
		assets.erase(it);
		PublishPhysicsAssetChange(metadata, TerraEngine::Physics::PhysicsAssetChangeKind::Removed);
		RequestAssetIndexSave();
	}
}

// AssetDatabase::FindAssetByGuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const AssetMetadata* AssetDatabase::FindAssetByGuid(const TerraEngine::UUID& guid) const {
	for (const auto& [path, metadata] : assets) {
		if (metadata.guid == guid) {
			return &metadata;
		}
	}

	return nullptr;
}

TerraEngine::Physics::PhysicsAssetChangeSnapshot AssetDatabase::GetPhysicsAssetChangesSince(
	TerraEngine::Physics::PhysicsAssetGeneration observedGeneration) const {
	return TerraEngine::Physics::PhysicsAssetChangeJournal::Instance().SnapshotSince(observedGeneration);
}

TerraEngine::Physics::PhysicsAssetGeneration AssetDatabase::GetPhysicsAssetChangeGeneration() const {
	return TerraEngine::Physics::PhysicsAssetChangeJournal::Instance().CurrentGeneration();
}

TerrainMaterialChangeSnapshot AssetDatabase::GetTerrainMaterialChangesSince(
	std::uint64_t observedGeneration) const
{
	std::lock_guard<std::mutex> lock(terrainMaterialChangeMutex);
	TerrainMaterialChangeSnapshot snapshot;
	snapshot.generation = nextTerrainMaterialChangeGeneration;
	if(observedGeneration > snapshot.generation) {
		snapshot.requiresFullRefresh = true;
		return snapshot;
	}
	if(!terrainMaterialChanges.empty() && observedGeneration + 1u < terrainMaterialChanges.front().generation)
		snapshot.requiresFullRefresh = true;
	for(const TerrainMaterialChange& change : terrainMaterialChanges)
		if(change.generation > observedGeneration) snapshot.changes.push_back(change);
	return snapshot;
}

std::uint64_t AssetDatabase::GetTerrainMaterialChangeGeneration() const
{
	std::lock_guard<std::mutex> lock(terrainMaterialChangeMutex);
	return nextTerrainMaterialChangeGeneration;
}

void AssetDatabase::RecordTerrainMaterialChange(
	const AssetMetadata& metadata, TerrainMaterialChangeKind changeKind)
{
	if(metadata.guid == TerraEngine::EMPTY_UUID) return;
	std::lock_guard<std::mutex> lock(terrainMaterialChangeMutex);
	TerrainMaterialChange change;
	change.materialAssetId = metadata.guid;
	change.kind = changeKind;
	change.generation = ++nextTerrainMaterialChangeGeneration;
	change.sourcePath = metadata.sourcePath;
	terrainMaterialChanges.push_back(std::move(change));
	// A bounded journal makes repeated project scans safe. Readers that lag the
	// oldest retained event receive requiresFullRefresh and rebuild diagnostics.
	constexpr std::size_t kMaximumTerrainMaterialChanges = 1024u;
	while(terrainMaterialChanges.size() > kMaximumTerrainMaterialChanges)
		terrainMaterialChanges.pop_front();
}

bool AssetDatabase::TryGetGeneratedModelPackageChildDisplayName(
	const TerraEngine::UUID& guid,
	GeneratedModelPackageChildKind kind,
	std::string& outName) const
{
	GeneratedModelPackageChildMetadata child;
	if(!TryGetGeneratedModelPackageChildMetadata(guid, kind, child))
	{
		return false;
	}
	outName = child.displayName;
	return true;
}

bool AssetDatabase::TryGetGeneratedModelPackageChildIds(
	const TerraEngine::UUID& packageGuid,
	GeneratedModelPackageChildKind kind,
	std::vector<TerraEngine::UUID>& outChildIds) const
{
	outChildIds.clear();
	for (const auto& [sourcePath, metadata] : assets)
	{
		if (!TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
		{
			continue;
		}

		TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
		if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourcePath, importResult))
		{
			continue;
		}

		if (metadata.guid != packageGuid && importResult.packageGuid != packageGuid)
		{
			continue;
		}

		const std::optional<TerraEngine::AssetPipeline::AnimationImportSubAssetKind> manifestKind
			= ManifestKindForGeneratedKind(kind);
		if(manifestKind && IsPublishedAnimationManifest(importResult))
		{
			// Published manifests are authoritative. In particular, a deformable mesh
			// must not also appear as an ordinary Mesh child merely because both share
			// the same legacy generatedMeshes transport record.
			for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : importResult.animationManifest.subAssets)
			{
				if(child.kind == *manifestKind && child.guid != TerraEngine::EMPTY_UUID
					&& !ContainsUuid(outChildIds, child.guid))
				{
					outChildIds.push_back(child.guid);
				}
			}
		}
		else if(kind == GeneratedModelPackageChildKind::Mesh)
		{
			// Sidecars written before AnimationImportManifest treated every generated
			// render payload as a static mesh. Preserve that legacy discovery fallback.
			for(const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes)
			{
				if(mesh.guid != TerraEngine::EMPTY_UUID && !ContainsUuid(outChildIds, mesh.guid))
				{
					outChildIds.push_back(mesh.guid);
				}
			}
		}

		// Older static-package sidecars predate AnimationImportManifest. Preserve
		// their material/texture/prefab discovery without weakening the published
		// manifest requirement for new animation-only kinds.
		if(outChildIds.empty() && kind == GeneratedModelPackageChildKind::Material)
		{
			for(const auto& material : importResult.generatedMaterials)
			{
				if(material.guid != TerraEngine::EMPTY_UUID) outChildIds.push_back(material.guid);
			}
		}
		else if(outChildIds.empty() && kind == GeneratedModelPackageChildKind::Texture)
		{
			for(const auto& texture : importResult.generatedTextures)
			{
				if(texture.guid != TerraEngine::EMPTY_UUID) outChildIds.push_back(texture.guid);
			}
		}
		else if(outChildIds.empty() && kind == GeneratedModelPackageChildKind::Prefab)
		{
			for(const auto& prefab : importResult.generatedPrefabs)
			{
				if(prefab.guid != TerraEngine::EMPTY_UUID) outChildIds.push_back(prefab.guid);
			}
		}

		std::sort(outChildIds.begin(), outChildIds.end());
		outChildIds.erase(std::unique(outChildIds.begin(), outChildIds.end()), outChildIds.end());
		return !outChildIds.empty();
	}

	return false;
}

bool AssetDatabase::TryGetGeneratedModelPackageChildMetadata(
	const TerraEngine::UUID& guid,
	GeneratedModelPackageChildKind kind,
	GeneratedModelPackageChildMetadata& outMetadata) const
{
	outMetadata = {};
	for(const auto& [sourcePath, metadata] : assets)
	{
		if(!TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
		{
			continue;
		}

		TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
		if(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourcePath, importResult))
		{
			continue;
		}

		if(kind == GeneratedModelPackageChildKind::Mesh)
		{
			const bool publishedManifest = IsPublishedAnimationManifest(importResult);
			const auto manifestChild = publishedManifest
				? std::find_if(importResult.animationManifest.subAssets.begin(), importResult.animationManifest.subAssets.end(),
					[&](const auto& child) {
						return child.guid == guid
							&& child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Mesh;
					})
				: importResult.animationManifest.subAssets.end();
			if(publishedManifest && manifestChild == importResult.animationManifest.subAssets.end())
			{
				continue;
			}
			for(const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes)
			{
				if(mesh.guid != guid)
				{
					continue;
				}
				outMetadata.packageGuid = importResult.packageGuid;
				outMetadata.guid = mesh.guid;
				outMetadata.kind = kind;
				outMetadata.displayName = GeneratedChildNameFallback(
					mesh.displayName, mesh.sourceNodePath, mesh.sourcePath, mesh.compiledPath, mesh.guid);
				outMetadata.semanticKey = publishedManifest ? manifestChild->semanticKey
					: mesh.sourceNodePath.empty() ? "mesh/" + std::to_string(mesh.sourceMeshIndex)
						: "mesh/" + mesh.sourceNodePath;
				outMetadata.sourcePath = mesh.sourcePath;
				outMetadata.compiledPath = mesh.compiledPath;
				outMetadata.publishedGeneration = importResult.animationManifest.publishedGeneration;
				if(publishedManifest)
				{
					outMetadata.compatibilityFingerprint = manifestChild->compatibilityFingerprint;
					outMetadata.readOnlyPackageChild = manifestChild->readOnlyPackageChild;
					for(const auto& dependency : manifestChild->dependencies)
					{
						if(dependency.assetGuid != TerraEngine::EMPTY_UUID
							&& !ContainsUuid(outMetadata.dependencies, dependency.assetGuid))
						{
							outMetadata.dependencies.push_back(dependency.assetGuid);
						}
					}
				}
				for(const auto& assignment : importResult.materialAssignments)
				{
					if(assignment.meshGuid == mesh.guid && assignment.materialGuid != TerraEngine::EMPTY_UUID
						&& !ContainsUuid(outMetadata.dependencies, assignment.materialGuid))
					{
						outMetadata.dependencies.push_back(assignment.materialGuid);
					}
				}
				return true;
			}
			continue;
		}

		const std::optional<TerraEngine::AssetPipeline::AnimationImportSubAssetKind> manifestKind
			= ManifestKindForGeneratedKind(kind);
		if(manifestKind && IsPublishedAnimationManifest(importResult))
		{
			for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : importResult.animationManifest.subAssets)
			{
				if(child.guid != guid || child.kind != *manifestKind)
				{
					continue;
				}
				outMetadata.packageGuid = importResult.packageGuid;
				outMetadata.guid = child.guid;
				outMetadata.kind = kind;
				outMetadata.displayName = GeneratedChildNameFallback(
					child.displayName, child.semanticKey, child.sourcePath, child.compiledPath, child.guid);
				outMetadata.semanticKey = child.semanticKey;
				outMetadata.sourcePath = child.sourcePath;
				outMetadata.compiledPath = child.compiledPath;
				outMetadata.compatibilityFingerprint = child.compatibilityFingerprint;
				outMetadata.publishedGeneration = importResult.animationManifest.publishedGeneration;
				outMetadata.readOnlyPackageChild = child.readOnlyPackageChild;
				for(const TerraEngine::AssetPipeline::AnimationImportDependency& dependency : child.dependencies)
				{
					if(dependency.assetGuid != TerraEngine::EMPTY_UUID
						&& !ContainsUuid(outMetadata.dependencies, dependency.assetGuid))
					{
						outMetadata.dependencies.push_back(dependency.assetGuid);
					}
				}
				return true;
			}
		}

		if(kind == GeneratedModelPackageChildKind::Material)
		{
			for(const auto& material : importResult.generatedMaterials)
			{
				if(material.guid != guid) continue;
				outMetadata = {importResult.packageGuid, material.guid, kind,
					GeneratedChildNameFallback(material.displayName, {}, material.sourcePath, material.compiledPath, material.guid),
					"material", material.sourcePath, material.compiledPath};
				outMetadata.dependencies = material.textureGuids;
				return true;
			}
		}
		else if(kind == GeneratedModelPackageChildKind::Texture)
		{
			for(const auto& texture : importResult.generatedTextures)
			{
				if(texture.guid != guid) continue;
				outMetadata = {importResult.packageGuid, texture.guid, kind,
					GeneratedChildNameFallback(texture.displayName, {}, texture.sourcePath, texture.compiledPath, texture.guid),
					"texture", texture.sourcePath, texture.compiledPath};
				return true;
			}
		}
		else if(kind == GeneratedModelPackageChildKind::Prefab)
		{
			for(const auto& prefab : importResult.generatedPrefabs)
			{
				if(prefab.guid != guid) continue;
				outMetadata = {importResult.packageGuid, prefab.guid, kind,
					GeneratedChildNameFallback(prefab.displayName, {}, prefab.sourcePath, {}, prefab.guid),
					"prefab/package-root", prefab.sourcePath, {}};
				outMetadata.dependencies = prefab.meshGuids;
				for(const TerraEngine::UUID& materialGuid : prefab.materialGuids)
				{
					if(!ContainsUuid(outMetadata.dependencies, materialGuid)) outMetadata.dependencies.push_back(materialGuid);
				}
				return true;
			}
		}
	}
	return false;
}

bool AssetDatabase::TryGetModelPackageAnimationSummary(
	const AssetMetadata& metadata,
	ModelPackageAnimationSummaryMetadata& outSummary) const
{
	outSummary = {};
	if(!TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
	{
		return false;
	}

	const std::filesystem::path sourcePath = std::filesystem::absolute(assetRoot / metadata.sourcePath);
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	if(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourcePath, importResult))
	{
		return false;
	}

	outSummary.readable = true;
	const TerraEngine::AssetPipeline::AnimationImportManifest& manifest = importResult.animationManifest;
	outSummary.schemaVersion = manifest.schemaVersion;
	outSummary.candidateGeneration = manifest.candidateGeneration;
	outSummary.publishedGeneration = manifest.publishedGeneration;
	outSummary.lastKnownGoodGeneration = manifest.lastKnownGoodGeneration;
	outSummary.preservedLastKnownGood = manifest.preservedLastKnownGood;
	outSummary.generatedChildCount = manifest.subAssets.size();
	outSummary.dependencyFingerprint = manifest.dependencyFingerprintHex;
	outSummary.publishedAtomically = IsPublishedAnimationManifest(importResult)
		&& manifest.packageGuid == metadata.guid;

	const auto appendValidationError = [&](std::string code, std::string message) {
		++outSummary.errorCount;
		outSummary.prefabBindingsValid = false;
		AppendAnimationSummaryDiagnostic(outSummary, std::move(code) + ": " + std::move(message));
	};
	const auto appendValidationWarning = [&](std::string code, std::string message) {
		++outSummary.warningCount;
		AppendAnimationSummaryDiagnostic(outSummary, std::move(code) + ": " + std::move(message));
	};

	for(const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : importResult.diagnostics)
	{
		switch(diagnostic.severity)
		{
		case TerraEngine::AssetPipeline::ModelDiagnosticSeverity::Info: ++outSummary.infoCount; break;
		case TerraEngine::AssetPipeline::ModelDiagnosticSeverity::Warning: ++outSummary.warningCount; break;
		case TerraEngine::AssetPipeline::ModelDiagnosticSeverity::Error: ++outSummary.errorCount; break;
		}
		const std::string code = diagnostic.code.empty() ? "ModelImportDiagnostic" : diagnostic.code;
		AppendAnimationSummaryDiagnostic(outSummary,
			code + (diagnostic.message.empty() ? std::string{} : ": " + diagnostic.message));
	}

	if(importResult.packageGuid == TerraEngine::EMPTY_UUID
		|| importResult.packageGuid != metadata.guid
		|| manifest.packageGuid != importResult.packageGuid)
	{
		appendValidationError("AnimationPackageGuidMismatch",
			"The source metadata, import result, and animation manifest must identify the same package.");
	}
	if(!outSummary.publishedAtomically)
	{
		appendValidationError("AnimationManifestNotPublished",
			"The manifest is cancelled, incomplete, stale, or does not match its dependency fingerprint.");
	}

	std::map<TerraEngine::UUID, const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset*> childrenById;
	std::vector<TerraEngine::UUID> skeletonIds;
	std::vector<TerraEngine::UUID> clipIds;
	std::vector<TerraEngine::UUID> deformableMeshIds;
	std::vector<TerraEngine::UUID> morphTargetIds;
	std::vector<const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset*> prefabs;
	for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : manifest.subAssets)
	{
		if(child.readOnlyPackageChild)
		{
			++outSummary.readOnlyChildCount;
		}
		else
		{
			appendValidationError("AnimationPackageChildEditable",
				"Generated child '" + child.semanticKey + "' must remain a read-only package view.");
		}

		if(child.guid == TerraEngine::EMPTY_UUID || !childrenById.emplace(child.guid, &child).second)
		{
			appendValidationError("AnimationPackageChildIdentityInvalid",
				"A generated child has an empty or duplicate stable GUID.");
		}

		switch(child.kind)
		{
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Skeleton:
			++outSummary.skeletonCount;
			skeletonIds.push_back(child.guid);
			break;
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::AnimationClip:
			++outSummary.animationClipCount;
			clipIds.push_back(child.guid);
			break;
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::DeformableMesh:
			++outSummary.deformableMeshCount;
			deformableMeshIds.push_back(child.guid);
			break;
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::MorphTarget:
			++outSummary.morphTargetCount;
			morphTargetIds.push_back(child.guid);
			break;
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Prefab:
			++outSummary.prefabCount;
			prefabs.push_back(&child);
			break;
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Material:
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Texture:
		case TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Mesh:
			break;
		}
	}

	for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child : manifest.subAssets)
	{
		for(const TerraEngine::AssetPipeline::AnimationImportDependency& dependency : child.dependencies)
		{
			const auto dependencyIt = childrenById.find(dependency.assetGuid);
			if(dependency.required && dependencyIt == childrenById.end())
			{
				appendValidationError("AnimationPackageDependencyMissing",
					"Generated child '" + child.semanticKey + "' requires a child GUID that is absent from the published manifest.");
				continue;
			}
			if(dependencyIt != childrenById.end() && dependency.compatibilityFingerprint != 0u
				&& dependency.compatibilityFingerprint != dependencyIt->second->compatibilityFingerprint)
			{
				appendValidationError("AnimationPackageDependencyIncompatible",
					"Generated child '" + child.semanticKey + "' references a different compatibility generation.");
			}
		}
	}

	const bool hasAnimatedBindings = !skeletonIds.empty() || !clipIds.empty()
		|| !deformableMeshIds.empty() || !morphTargetIds.empty();
	if(hasAnimatedBindings && prefabs.empty())
	{
		appendValidationError("AnimationPrefabMissing",
			"An animated package must publish a prefab that binds its generated children.");
	}

	const auto validateBindingArray = [&](const json& bindings,
		const char* fieldName,
		const std::vector<TerraEngine::UUID>& expectedIds,
		std::string_view prefabName) {
		if(expectedIds.empty())
		{
			return true;
		}
		const auto field = bindings.find(fieldName);
		if(field == bindings.end() || !field->is_array())
		{
			appendValidationError("AnimationPrefabBindingMissing",
				"Prefab '" + std::string(prefabName) + "' is missing binding array '" + fieldName + "'.");
			return false;
		}

		std::vector<TerraEngine::UUID> actualIds;
		for(const json& value : *field)
		{
			if(!value.is_string())
			{
				appendValidationError("AnimationPrefabBindingInvalid",
					"Prefab binding array '" + std::string(fieldName) + "' contains a non-GUID value.");
				return false;
			}
			const std::optional<TerraEngine::UUID> parsed = TerraEngine::ParseUUID(value.get<std::string>());
			if(!parsed || !ContainsUuid(expectedIds, *parsed))
			{
				appendValidationError("AnimationPrefabBindingInvalid",
					"Prefab binding array '" + std::string(fieldName) + "' references a child outside its package kind.");
				return false;
			}
			if(!ContainsUuid(actualIds, *parsed)) actualIds.push_back(*parsed);
		}
		for(const TerraEngine::UUID& expectedId : expectedIds)
		{
			if(!ContainsUuid(actualIds, expectedId))
			{
				appendValidationError("AnimationPrefabBindingIncomplete",
					"Prefab binding array '" + std::string(fieldName) + "' omits a published package child.");
				return false;
			}
		}
		return true;
	};

	for(const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset* prefab : prefabs)
	{
		if(prefab == nullptr)
		{
			continue;
		}
		std::vector<TerraEngine::UUID> dependencyIds;
		for(const TerraEngine::AssetPipeline::AnimationImportDependency& dependency : prefab->dependencies)
		{
			if(dependency.required && dependency.assetGuid != TerraEngine::EMPTY_UUID)
			{
				dependencyIds.push_back(dependency.assetGuid);
			}
		}
		for(const std::vector<TerraEngine::UUID>* expected : {&skeletonIds, &clipIds, &deformableMeshIds, &morphTargetIds})
		{
			for(const TerraEngine::UUID& expectedId : *expected)
			{
				if(!ContainsUuid(dependencyIds, expectedId))
				{
					appendValidationError("AnimationPrefabDependencyIncomplete",
						"Prefab '" + prefab->semanticKey + "' does not depend on every generated animation child.");
				}
			}
		}

		const std::optional<std::filesystem::path> prefabPath = ResolveGeneratedSourceArtifact(assetRoot, prefab->sourcePath);
		if(!prefabPath)
		{
			appendValidationError("AnimationPrefabDocumentMissing",
				"The generated prefab document is missing or resolves outside the project Assets root.");
			continue;
		}

		try
		{
			std::ifstream input(*prefabPath);
			json prefabDocument;
			input >> prefabDocument;
			const auto bindingsIt = prefabDocument.find("animationBindings");
			if(hasAnimatedBindings && (bindingsIt == prefabDocument.end() || !bindingsIt->is_object()))
			{
				appendValidationError("AnimationPrefabBindingsMissing",
					"The generated prefab has no animationBindings object.");
				continue;
			}
			if(!hasAnimatedBindings)
			{
				continue;
			}

			const json& bindings = *bindingsIt;
			// The generated-prefab schema names every value as an asset identity so
			// editor automation cannot confuse source node indices with stable GUIDs.
			validateBindingArray(bindings, "skeletonAssetIds", skeletonIds, prefab->displayName);
			validateBindingArray(bindings, "clipAssetIds", clipIds, prefab->displayName);
			validateBindingArray(bindings, "deformableMeshAssetIds", deformableMeshIds, prefab->displayName);
			validateBindingArray(bindings, "morphTargetAssetIds", morphTargetIds, prefab->displayName);

			TerraEngine::UUID boundSkeleton = TerraEngine::EMPTY_UUID;
			if(!skeletonIds.empty())
			{
				const auto skeletonField = bindings.find("primarySkeletonAssetId");
				const std::optional<TerraEngine::UUID> parsed = skeletonField != bindings.end() && skeletonField->is_string()
					? TerraEngine::ParseUUID(skeletonField->get<std::string>()) : std::nullopt;
				if(!parsed || !ContainsUuid(skeletonIds, *parsed))
				{
					appendValidationError("AnimationPrefabSkeletonBindingInvalid",
						"The prefab primarySkeletonAssetId does not reference a skeleton from this package.");
				}
				else
				{
					boundSkeleton = *parsed;
				}
			}

			if(!clipIds.empty())
			{
				const auto clipField = bindings.find("defaultClipAssetId");
				const std::optional<TerraEngine::UUID> parsed = clipField != bindings.end() && clipField->is_string()
					? TerraEngine::ParseUUID(clipField->get<std::string>()) : std::nullopt;
				if(!parsed || !ContainsUuid(clipIds, *parsed))
				{
					appendValidationError("AnimationPrefabDefaultClipInvalid",
						"The prefab defaultClipAssetId does not reference an animation clip from this package.");
				}
			}

			if(boundSkeleton != TerraEngine::EMPTY_UUID)
			{
				bool animatorBound = false;
				for(const json& object : prefabDocument.value("objects", json::array()))
				{
					const json components = object.value("components", json::object());
					const auto animator = components.find("AnimatorComponent");
					if(animator == components.end() || !animator->is_object()) continue;
					const auto skeletonOverride = animator->find("skeletonOverrideAssetId");
					const std::optional<TerraEngine::UUID> parsed = skeletonOverride != animator->end() && skeletonOverride->is_string()
						? TerraEngine::ParseUUID(skeletonOverride->get<std::string>()) : std::nullopt;
					const auto defaultClip = animator->find("defaultClipAssetId");
					const std::optional<TerraEngine::UUID> parsedClip = defaultClip != animator->end() && defaultClip->is_string()
						? TerraEngine::ParseUUID(defaultClip->get<std::string>()) : std::nullopt;
					const bool clipBound = clipIds.empty()
						|| (parsedClip && ContainsUuid(clipIds, *parsedClip));
					animatorBound = parsed && *parsed == boundSkeleton && clipBound;
					if(animatorBound) break;
				}
				if(!animatorBound)
				{
					appendValidationError("AnimationPrefabAnimatorBindingInvalid",
						"AnimatorComponent does not bind the prefab's published skeleton and default clip.");
				}
			}
		}
		catch(const std::exception& exception)
		{
			appendValidationError("AnimationPrefabDocumentInvalid",
				"The generated prefab could not be parsed: " + std::string(exception.what()));
		}
	}

	if(outSummary.diagnostics.size() == kMaximumAnimationSummaryDiagnostics
		&& importResult.diagnostics.size() > kMaximumAnimationSummaryDiagnostics)
	{
		appendValidationWarning("AnimationSummaryDiagnosticsTruncated",
			"Additional importer diagnostics are available in the package sidecar and editor log.");
	}
	return true;
}

bool AssetDatabase::IsMeshAsset(const AssetMetadata& metadata) const {
	// Model-package parents are logical containers. Their generated child
	// meshes own the renderable TEMF payloads and are queried through the
	// package sidecar, so generic mesh-only editor controls must ignore the
	// parent source asset.
	return metadata.importerName == "MeshImporter";
}

bool AssetDatabase::TryGetVirtualGeometryMetadata(const AssetMetadata& metadata, VirtualGeometryAssetMetadata& outMetadata) const {
	outMetadata = {};
	if (metadata.compiledPath.empty()) {
		outMetadata.warnings.push_back("Compiled mesh path is empty.");
		return false;
	}

	const std::filesystem::path compiledPath = buildRoot / metadata.compiledPath;
	std::ifstream input(compiledPath, std::ios::binary);
	if (!input) {
		outMetadata.warnings.push_back("Compiled mesh file is missing.");
		return false;
	}

	// Read and validate the complete canonical envelope before publishing any
	// inspector metadata. This intentionally gives historical cooked files a
	// recook state instead of attempting a sidecar migration or fallback parse.
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);
	if (size <= 0)
	{
		outMetadata.warnings.push_back("Compiled mesh is empty and requires a canonical TEMFPRD1 recook.");
		return false;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char*>(bytes.data()), size);
	const TerraEngine::Resource::TemfProductionValidationResult validated =
		TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
	if (!validated.IsComplete())
	{
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Compiled mesh requires a canonical TEMFPRD1 recook: " + validated.diagnostic);
		outMetadata.rebuildGuidance = "Rebuild this mesh from source; legacy cooked mesh layouts are not supported.";
		return true;
	}
	outMetadata.readable = true;
	outMetadata.payloadGeneration = validated.directory.header.formatVersion;
	outMetadata.hasVirtualGeometry =
		(validated.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) != 0u;
	outMetadata.meshletCount = static_cast<std::uint32_t>(validated.directory.blocks.size());
	outMetadata.pageCount = validated.directory.header.pageCount;
	outMetadata.encodedGeometryBytes = validated.directory.header.payloadBytes;
	for (const TemfBlockDescriptor& block : validated.directory.blocks)
	{
		outMetadata.decodedGeometryBytes += block.decodedBytes;
	}
	outMetadata.pageStreamingReady = outMetadata.hasVirtualGeometry
		&& validated.directory.header.pageCount != 0u;
	outMetadata.smallMeshStreamingSkipped = outMetadata.hasVirtualGeometry
		&& validated.directory.header.pageCount == 0u;
	outMetadata.aggregateProfileCurrent = true;
	outMetadata.aggregateProfileRepairRequired = false;
	outMetadata.aggregateProfileDiagnostic = "Canonical TEMFPRD1 block directory validated.";
	outMetadata.codec = "temfprd1";
	outMetadata.codecProfile = "production-blocks";
	if (!outMetadata.hasVirtualGeometry)
	{
		outMetadata.warnings.push_back("Compiled mesh uses direct-resident geometry.");
	}
	return true;

}

VirtualGeometryAdaptiveSettingsMetadata AssetDatabase::GetDefaultVirtualGeometryAdaptiveSettings() const {
	VirtualGeometryAdaptiveSettingsMetadata settings;
	settings.settingsSource = "ProjectDefaults";
	settings.targetRatios.clear();
	for (float ratio : kVirtualGeometryDefaultHalfOctaveRatios) {
		settings.targetRatios.push_back(ratio);
	}
	return settings;
}

bool AssetDatabase::CanEditVirtualGeometryPerMeshOverride(const AssetMetadata& metadata) const {
	return metadata.importerName == "MeshImporter";
}

bool AssetDatabase::TryGetVirtualGeometryAdaptiveSettings(const AssetMetadata& metadata, VirtualGeometryAdaptiveSettingsMetadata& outSettings) const {
	outSettings = GetDefaultVirtualGeometryAdaptiveSettings();
	if (!IsMeshAsset(metadata)) {
		outSettings.valid = false;
		outSettings.validationErrors.push_back("Selected asset is not a mesh asset.");
		return false;
	}

	const json overrideBlock = metadata.settings.is_object()
		? metadata.settings.value("virtualGeometryOverride", json::object())
		: json::object();
	if (!overrideBlock.is_object() || overrideBlock.empty()) {
		return true;
	}

	// Sparse per-mesh overrides inherit any missing fields from project
	// defaults. Validation happens after merging so an incomplete override block
	// cannot corrupt the resolved settings used by rebuilds.
	ApplyVirtualGeometryOverrideJson(overrideBlock, outSettings);
	outSettings.validationErrors = ValidateVirtualGeometryAdaptiveSettings(outSettings);
	outSettings.valid = outSettings.validationErrors.empty();
	return true;
}

bool AssetDatabase::HasVirtualGeometryPerMeshOverride(const AssetMetadata& metadata) const {
	return IsMeshAsset(metadata)
		&& metadata.settings.is_object()
		&& metadata.settings.contains("virtualGeometryOverride")
		&& metadata.settings["virtualGeometryOverride"].is_object()
		&& !metadata.settings["virtualGeometryOverride"].empty()
		&& metadata.settings["virtualGeometryOverride"].value("enabled", true);
}

bool AssetDatabase::SaveVirtualGeometryPerMeshOverride(const TerraEngine::UUID& assetGuid, const VirtualGeometryAdaptiveSettingsMetadata& settings, std::string* outError) {
	for (auto& [path, metadata] : assets) {
		if (metadata.guid != assetGuid) {
			continue;
		}
		if (!CanEditVirtualGeometryPerMeshOverride(metadata)) {
			if (outError != nullptr) {
				*outError = "Selected asset is not a mesh asset.";
			}
			return false;
		}

		std::vector<std::string> errors = ValidateVirtualGeometryAdaptiveSettings(settings);
		if (!errors.empty()) {
			if (outError != nullptr) {
				*outError = JoinValidationErrors(errors);
			}
			return false;
		}

		if (!metadata.settings.is_object()) {
			metadata.settings = json::object();
		}
		metadata.settings["virtualGeometryOverride"] = BuildVirtualGeometryOverrideJson(settings, GetDefaultVirtualGeometryAdaptiveSettings());
		if (!SaveMetadata(metadata)) {
			if (outError != nullptr) {
				*outError = "Failed to save mesh metadata.";
			}
			return false;
		}
		RequestAssetIndexSave();
		return true;
	}

	if (outError != nullptr) {
		*outError = "Mesh asset was not found in the asset database.";
	}
	return false;
}

bool AssetDatabase::ClearVirtualGeometryPerMeshOverride(const TerraEngine::UUID& assetGuid, std::string* outError) {
	for (auto& [path, metadata] : assets) {
		if (metadata.guid != assetGuid) {
			continue;
		}
		if (!CanEditVirtualGeometryPerMeshOverride(metadata)) {
			if (outError != nullptr) {
				*outError = "Selected asset is not a mesh asset.";
			}
			return false;
		}
		if (metadata.settings.is_object()) {
			metadata.settings.erase("virtualGeometryOverride");
		}
		if (!SaveMetadata(metadata)) {
			if (outError != nullptr) {
				*outError = "Failed to save mesh metadata.";
			}
			return false;
		}
		RequestAssetIndexSave();
		return true;
	}

	if (outError != nullptr) {
		*outError = "Mesh asset was not found in the asset database.";
	}
	return false;
}

bool AssetDatabase::RebuildVirtualGeometryAsset(const TerraEngine::UUID& assetGuid, std::string* outError) {
	for (const auto& [path, metadata] : assets) {
		if (metadata.guid != assetGuid) {
			continue;
		}
		if (!CanEditVirtualGeometryPerMeshOverride(metadata)) {
			if (outError != nullptr) {
				*outError = "Selected asset is not a mesh asset.";
			}
			return false;
		}
		// Route repair through the normal worker/staging pipeline. This limits the
		// recook to the selected mesh and preserves the last complete compiled
		// artifact until the new VGAP-bearing output is ready to publish.
		ImportAssetWithProgress(assetRoot / metadata.sourcePath, true);
		return true;
	}

	if (outError != nullptr) {
		*outError = "Mesh asset was not found in the asset database.";
	}
	return false;
}

bool AssetDatabase::TryGetGeneratedMeshVirtualGeometryAdaptiveSettings(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, VirtualGeometryAdaptiveSettingsMetadata& outSettings) const {
	outSettings = GetDefaultVirtualGeometryAdaptiveSettings();
	TerraEngine::AssetPipeline::ModelPackageImportResult result;
	if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(std::filesystem::absolute(packagePath), result)) {
		outSettings.valid = false;
		outSettings.validationErrors.push_back("Package import sidecar is missing.");
		return false;
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes) {
		if (mesh.guid != meshGuid) {
			continue;
		}
		ApplyGeneratedVirtualGeometrySettings(mesh.virtualGeometrySettings, outSettings);
		outSettings.validationErrors = ValidateVirtualGeometryAdaptiveSettings(outSettings);
		outSettings.valid = outSettings.validationErrors.empty();
		return true;
	}

	outSettings.valid = false;
	outSettings.validationErrors.push_back("Generated mesh was not found in the package sidecar.");
	return false;
}

bool AssetDatabase::SaveGeneratedMeshVirtualGeometryPerMeshOverride(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, const VirtualGeometryAdaptiveSettingsMetadata& settings, std::string* outError) {
	std::vector<std::string> errors = ValidateVirtualGeometryAdaptiveSettings(settings);
	if (!errors.empty()) {
		if (outError != nullptr) {
			*outError = JoinValidationErrors(errors);
		}
		return false;
	}

	const std::filesystem::path absolutePackagePath = std::filesystem::absolute(packagePath);
	TerraEngine::AssetPipeline::ModelPackageImportResult result;
	if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absolutePackagePath, result)) {
		if (outError != nullptr) {
			*outError = "Package import sidecar is missing.";
		}
		return false;
	}

	for (TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes) {
		if (mesh.guid != meshGuid) {
			continue;
		}
		mesh.virtualGeometrySettings = ToGeneratedVirtualGeometrySettings(settings, true);
		if (!TerraEngine::AssetPipeline::SaveModelPackageImportResult(absolutePackagePath, result)) {
			if (outError != nullptr) {
				*outError = "Failed to save package import sidecar.";
			}
			return false;
		}
		return true;
	}

	if (outError != nullptr) {
		*outError = "Generated mesh was not found in the package sidecar.";
	}
	return false;
}

bool AssetDatabase::ClearGeneratedMeshVirtualGeometryPerMeshOverride(const std::filesystem::path& packagePath, const TerraEngine::UUID& meshGuid, std::string* outError) {
	const std::filesystem::path absolutePackagePath = std::filesystem::absolute(packagePath);
	TerraEngine::AssetPipeline::ModelPackageImportResult result;
	if (!TerraEngine::AssetPipeline::LoadModelPackageImportResult(absolutePackagePath, result)) {
		if (outError != nullptr) {
			*outError = "Package import sidecar is missing.";
		}
		return false;
	}

	for (TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes) {
		if (mesh.guid != meshGuid) {
			continue;
		}
		mesh.virtualGeometrySettings = ToGeneratedVirtualGeometrySettings(GetDefaultVirtualGeometryAdaptiveSettings(), false);
		if (!TerraEngine::AssetPipeline::SaveModelPackageImportResult(absolutePackagePath, result)) {
			if (outError != nullptr) {
				*outError = "Failed to save package import sidecar.";
			}
			return false;
		}
		return true;
	}

	if (outError != nullptr) {
		*outError = "Generated mesh was not found in the package sidecar.";
	}
	return false;
}

bool AssetDatabase::RebuildGeneratedMeshVirtualGeometryAsset(const std::filesystem::path& packagePath, std::string* outError) {
	const std::filesystem::path absolutePackagePath = std::filesystem::absolute(packagePath);
	if (!std::filesystem::exists(absolutePackagePath)) {
		if (outError != nullptr) {
			*outError = "Package source file is missing.";
		}
		return false;
	}
	ImportAsset(absolutePackagePath, true);
	return true;
}

bool AssetDatabase::TryGetVirtualGeometryDetailDiagnostics(const AssetMetadata& metadata, std::vector<VirtualGeometryDetailDiagnosticMetadata>& outDiagnostics) const {
	outDiagnostics.clear();
	if (!IsMeshAsset(metadata)) {
		return false;
	}

	const std::filesystem::path compiledPath = buildRoot / metadata.compiledPath;
	std::ifstream input(compiledPath, std::ios::binary);
	if (!input) {
		return true;
	}
	// Read the complete file before looking at individual records. Inspector
	// diagnostics intentionally use the same TEMFPRD1 directory validation as
	// the runtime, so historical inline-header layouts never become a fallback.
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);
	if (size <= 0)
	{
		return true;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!input)
	{
		return true;
	}
	const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
	if (!validated.IsComplete()
		|| (validated.directory.header.representationFlags & kTemfRepresentationVirtualGeometry) == 0u)
	{
		return true;
	}

	const auto DecodeStream = [&](std::uint64_t attributeMask, std::vector<std::uint8_t>& destination)
	{
		destination.clear();
		for (const TemfBlockDescriptor& descriptor : validated.directory.blocks)
		{
			if (descriptor.streamKind != static_cast<std::uint16_t>(TemfStreamKind::Metadata)
				|| descriptor.attributeMask != attributeMask)
			{
				continue;
			}
			const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock(
				{ descriptor,
					std::span<const std::uint8_t>(bytes).subspan(
						static_cast<std::size_t>(descriptor.encodedPayloadOffset),
						static_cast<std::size_t>(descriptor.encodedBytes)),
					validated.directory.quantizationDomains, 0u }, {});
			if (!decoded.IsComplete())
			{
				destination.clear();
				return false;
			}
			destination.insert(destination.end(), decoded.bytes.begin(), decoded.bytes.end());
		}
		return !destination.empty();
	};
	const auto DecodeRecords = [&](std::uint64_t attributeMask, auto& destination)
	{
		using Record = typename std::remove_reference_t<decltype(destination)>::value_type;
		std::vector<std::uint8_t> decoded;
		if (!DecodeStream(attributeMask, decoded) || decoded.size() % sizeof(Record) != 0u)
		{
			destination.clear();
			return false;
		}
		destination.resize(decoded.size() / sizeof(Record));
		std::memcpy(destination.data(), decoded.data(), decoded.size());
		return true;
	};

	std::vector<std::uint8_t> virtualHeaderBytes;
	std::vector<VirtualGeometryRequestedTargetRecord> requestedTargets;
	std::vector<VirtualGeometryDetailLevelRecord> detailLevels;
	std::vector<VirtualGeometrySkippedDetailRecord> skippedDetails;
	std::vector<std::uint8_t> diagnosticStringBytes;
	if (!DecodeStream(kTemfMetadataVirtualHeader, virtualHeaderBytes)
		|| virtualHeaderBytes.size() != sizeof(VirtualGeometryHeader)
		|| !DecodeRecords(kTemfMetadataVirtualRequestedTargets, requestedTargets)
		|| !DecodeRecords(kTemfMetadataVirtualDetailLevels, detailLevels))
	{
		return true;
	}
	VirtualGeometryHeader virtualHeader{};
	std::memcpy(&virtualHeader, virtualHeaderBytes.data(), sizeof(virtualHeader));
	if (virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration
		|| requestedTargets.size() != virtualHeader.requestedTargetCount)
	{
		return true;
	}
	// Skipped targets and their messages are optional, but decode them when
	// present so the inspector retains the compiler's explicit reason text.
	DecodeRecords(kTemfMetadataVirtualSkippedDetails, skippedDetails);
	DecodeStream(kTemfMetadataVirtualDiagnosticStrings, diagnosticStringBytes);
	const std::string diagnosticStrings(
		reinterpret_cast<const char*>(diagnosticStringBytes.data()), diagnosticStringBytes.size());

	for (const VirtualGeometryRequestedTargetRecord& target : requestedTargets) {
		VirtualGeometryDetailDiagnosticMetadata diagnostic;
		diagnostic.targetIndex = target.targetIndex;
		diagnostic.requestedRatio = target.requestedRatio;
		diagnostic.status = ToVirtualGeometryTargetStatusText(target.status);
		diagnostic.desiredTriangleCount = target.desiredTriangleCount;
		diagnostic.desiredMeshletCount = target.desiredMeshletCount;
		diagnostic.triangleCount = target.desiredTriangleCount;
		diagnostic.meshletCount = target.desiredMeshletCount;

		for (const VirtualGeometryDetailLevelRecord& detail : detailLevels) {
			if (detail.requestedTargetIndex == target.targetIndex) {
				diagnostic.status = ToVirtualGeometryTargetStatusText(detail.targetStatus);
				diagnostic.actualTriangleCount = detail.primitiveCount;
				diagnostic.actualMeshletCount = detail.meshletCount;
				diagnostic.nearestGeneratedLevelIndex = detail.lodIndex;
				diagnostic.triangleCount = diagnostic.actualTriangleCount;
				diagnostic.meshletCount = diagnostic.actualMeshletCount;
				break;
			}
		}
		for (const VirtualGeometrySkippedDetailRecord& skipped : skippedDetails) {
			if (skipped.targetIndex == target.targetIndex) {
				diagnostic.status = ToVirtualGeometryTargetStatusText(skipped.status);
				diagnostic.reason = ToVirtualGeometrySkippedReasonText(skipped.reason);
				diagnostic.actualTriangleCount = skipped.candidateTriangleCount;
				diagnostic.actualMeshletCount = skipped.candidateMeshletCount;
				diagnostic.nearestGeneratedLevelIndex = skipped.nearestGeneratedLodIndex;
				diagnostic.message = ReadVirtualGeometryString(
					diagnosticStrings,
					skipped.messageStringOffset,
					skipped.messageStringSize);
				diagnostic.triangleCount = diagnostic.actualTriangleCount > 0u ? diagnostic.actualTriangleCount : diagnostic.triangleCount;
				diagnostic.meshletCount = diagnostic.actualMeshletCount > 0u ? diagnostic.actualMeshletCount : diagnostic.meshletCount;
				break;
			}
		}
		outDiagnostics.push_back(std::move(diagnostic));
	}

	if (outDiagnostics.empty()) {
		VirtualGeometryDetailDiagnosticMetadata summary;
		// A valid virtual header with no requested-target rows is still a
		// canonical cooked asset. Publish its bounded root facts rather than
		// synthesizing data from a retired inline mesh header.
		summary.status = "Cooked";
		summary.desiredTriangleCount = virtualHeader.sourceTriangleCount;
		summary.desiredMeshletCount = detailLevels.empty() ? 0u : detailLevels.front().meshletCount;
		summary.actualTriangleCount = virtualHeader.sourceTriangleCount;
		summary.actualMeshletCount = summary.desiredMeshletCount;
		summary.triangleCount = summary.actualTriangleCount;
		summary.meshletCount = summary.actualMeshletCount;
		outDiagnostics.push_back(summary);
	}
	return true;
}

bool AssetDatabase::TryGetVirtualGeometryClusterSummary(const AssetMetadata& metadata, VirtualGeometryClusterSummaryMetadata& outSummary) const {
	outSummary = {};
	if (!IsMeshAsset(metadata)) {
		return false;
	}

	VirtualGeometryAssetMetadata meshMetadata;
	if (!TryGetVirtualGeometryMetadata(metadata, meshMetadata)) {
		return true;
	}
	outSummary.clusterCount = meshMetadata.clusterCount;
	outSummary.materialSectionCount = meshMetadata.materialSectionCount;
	outSummary.clusterMaterialSectionCount = meshMetadata.clusterCount == 0u ? 0u : outSummary.materialSectionCount;
	outSummary.fallbackCoverageValid = meshMetadata.clusterCount == 0u || !meshMetadata.needsRebuild;
	outSummary.materialCoverageValid = outSummary.fallbackCoverageValid && outSummary.materialSectionCount > 0u;
	return true;
}

bool AssetDatabase::TryGetVirtualGeometryPageSummary(const AssetMetadata& metadata, VirtualGeometryPageSummaryMetadata& outSummary) const {
	outSummary = {};
	if (!IsMeshAsset(metadata)) {
		return false;
	}

	VirtualGeometryAssetMetadata meshMetadata;
	if (!TryGetVirtualGeometryMetadata(metadata, meshMetadata)) {
		return true;
	}
	outSummary.pageCount = meshMetadata.pageCount;
	outSummary.residentPageCount = meshMetadata.pageCount;
	outSummary.pinnedFallbackPageCount = meshMetadata.pageCount > 0u ? 1u : 0u;
	outSummary.pageStreamingReady = meshMetadata.pageStreamingReady;
	outSummary.smallMeshStreamingSkipped = meshMetadata.smallMeshStreamingSkipped;
	outSummary.skippedStreamingReason = meshMetadata.skippedStreamingReason;
	outSummary.integrityErrors = meshMetadata.pageIntegrityErrors;
	return true;
}

// AssetDatabase::FindScriptAssetByFullName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const AssetMetadata* AssetDatabase::FindScriptAssetByFullName(std::string_view fullName) const {
	if (fullName.empty()) {
		return nullptr;
	}

	const AssetMetadata* match = nullptr;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& [path, metadata] : assets) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ReadScriptMetadataString(metadata, "scriptFullName") != fullName) {
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (match != nullptr) {
			return nullptr;
		}

		match = &metadata;
	}

	return match;
}

// AssetDatabase::GetScriptFullName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AssetDatabase::GetScriptFullName(const TerraEngine::UUID& guid) const {
	if (const AssetMetadata* metadata = FindAssetByGuid(guid)) {
		if (metadata->settings.contains("scriptFullName")) {
			return metadata->settings["scriptFullName"].get<std::string>();
		}
	}

	return {};
}

// AssetDatabase::SetScriptCompileState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::SetScriptCompileState(const std::string& compileState, const std::string& summary) {
	bool updatedScriptAsset = false;
	for (auto& [path, metadata] : assets) {
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		metadata.settings["compileStatus"] = compileState;
		metadata.settings["lastBuildSummary"] = summary;
		updatedScriptAsset = true;
		if (!SaveMetadata(metadata)) {
			TERRAENGINE_LOG(LogLevel::Warning,
				"Failed to persist script compile state for " << metadata.sourcePath << ".");
		}
	}
	if (updatedScriptAsset) {
		// Script compilation is independent from asset importing. Publish the
		// terminal coordinator state into AssetIndex explicitly so a prior
		// reconcile/import save cannot leave durable metadata at "Compiling".
		RequestAssetIndexSave();
	}
}

AssetOrganizationImpact AssetDatabase::PrepareAssetOrganization(
	const std::filesystem::path& sourceProjectRelative,
	std::string_view operation,
	const std::filesystem::path& destinationProjectRelative) const {
	AssetOrganizationImpact impact;
	const auto source = ResolveContainedProjectAsset(projectRoot, assetRoot, sourceProjectRelative);
	if (!source || !std::filesystem::is_regular_file(source->second)) {
		impact.errorCategory = "validation";
		impact.error = "Asset source must identify an existing project-contained file.";
		return impact;
	}

	impact.source = source->first;
	if (!destinationProjectRelative.empty()) {
		const auto destination = ResolveContainedProjectAsset(projectRoot, assetRoot, destinationProjectRelative);
		if (!destination) {
			impact.errorCategory = "validation";
			impact.error = "Asset destination must remain canonically contained under Assets.";
			return impact;
		}
		impact.destination = destination->first;
	}
	impact.fingerprint = StableAssetOrganizationFingerprint(
		impact.source + "|" + std::string(operation) + "|" + impact.destination);

	const std::filesystem::path generatedRoot = source->second.parent_path() / (source->second.stem().string() + "_Generated");
	std::error_code iteratorError;
	if (std::filesystem::exists(generatedRoot, iteratorError)) {
		for (std::filesystem::recursive_directory_iterator iterator(generatedRoot, iteratorError), end;
			iterator != end && !iteratorError; iterator.increment(iteratorError)) {
			if (iterator->is_regular_file()) {
				impact.generatedChildren.push_back(std::filesystem::relative(iterator->path(), projectRoot).generic_string());
			}
		}
	}

	// Reference discovery intentionally reads only modest text-like project
	// files. Binary assets are repaired by their importers/index metadata and
	// must never be rewritten by a string replacement pass.
	for (std::filesystem::recursive_directory_iterator iterator(assetRoot, iteratorError), end;
		iterator != end && !iteratorError; iterator.increment(iteratorError)) {
		if (!iterator->is_regular_file() || iterator->path() == source->second
			|| iterator->path().extension() == ".meta" || iterator->file_size() > 4u * 1024u * 1024u) {
			continue;
		}
		std::ifstream input(iterator->path(), std::ios::binary);
		const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (contents.find(impact.source) != std::string::npos) {
			const std::string dependent = std::filesystem::relative(iterator->path(), projectRoot).generic_string();
			impact.dependents.push_back(dependent);
			impact.repairPlan.push_back("replace " + impact.source + " in " + dependent);
		}
	}
	impact.rollbackJournalEstimate = 1u + impact.generatedChildren.size() + impact.dependents.size();
	impact.valid = true;
	return impact;
}

AssetOrganizationResult AssetDatabase::PublishAssetOrganization(
	const std::filesystem::path& sourceProjectRelative,
	const std::filesystem::path& destinationProjectRelative,
	std::string_view operation,
	std::string_view approvedImpactFingerprint) {
	AssetOrganizationResult result;
	const bool duplicate = operation == "duplicate";
	if (!duplicate && operation != "move" && operation != "rename") {
		result.errorCategory = "validation";
		result.error = "Unsupported asset organization operation.";
		return result;
	}
	const AssetOrganizationImpact impact = PrepareAssetOrganization(sourceProjectRelative, operation, destinationProjectRelative);
	if (!impact.valid) {
		result.errorCategory = impact.errorCategory;
		result.error = impact.error;
		return result;
	}
	if (!approvedImpactFingerprint.empty() && approvedImpactFingerprint != impact.fingerprint) {
		result.errorCategory = "conflict";
		result.error = "Approved asset impact no longer matches the normalized request.";
		return result;
	}

	const auto source = ResolveContainedProjectAsset(projectRoot, assetRoot, impact.source);
	const auto destination = ResolveContainedProjectAsset(projectRoot, assetRoot, impact.destination);
	if (!source || !destination) {
		result.errorCategory = "validation";
		result.error = "Asset source and destination must remain under Assets.";
		return result;
	}
	if (std::filesystem::exists(destination->second)) {
		result.errorCategory = "conflict";
		result.error = "Asset destination already exists.";
		return result;
	}

	std::string stableId = StableAssetOrganizationFingerprint(impact.source);
	if (const auto metadata = assets.find(source->second); metadata != assets.end()) {
		stableId = UuidToString(metadata->second.guid);
	}
	if (duplicate) {
		stableId = StableAssetOrganizationFingerprint(impact.destination + "|duplicate");
	}

	struct ReferenceJournalEntry {
		std::filesystem::path path;
		std::string original;
	};
	std::vector<ReferenceJournalEntry> references;
	for (const std::string& dependent : impact.dependents) {
		const std::filesystem::path path = projectRoot / dependent;
		std::ifstream input(path, std::ios::binary);
		references.push_back({path, std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>())});
	}

	const std::filesystem::path sourceMeta = source->second.string() + ".meta";
	const std::filesystem::path destinationMeta = destination->second.string() + ".meta";
	const std::filesystem::path sourceGenerated = source->second.parent_path() / (source->second.stem().string() + "_Generated");
	const std::filesystem::path destinationGenerated = destination->second.parent_path() / (destination->second.stem().string() + "_Generated");
	bool primaryPublished = false;
	bool metaPublished = false;
	bool generatedPublished = false;
	std::error_code fileError;
	std::filesystem::create_directories(destination->second.parent_path(), fileError);
	if (!fileError) {
		if (duplicate) {
			std::filesystem::copy_file(source->second, destination->second, std::filesystem::copy_options::none, fileError);
		} else {
			std::filesystem::rename(source->second, destination->second, fileError);
		}
		primaryPublished = !fileError;
	}
	if (!fileError && std::filesystem::exists(sourceMeta)) {
		if (duplicate) {
			// A duplicate receives fresh metadata during ScanProject so its GUID
			// cannot alias the original asset identity.
		} else {
			std::filesystem::rename(sourceMeta, destinationMeta, fileError);
			metaPublished = !fileError;
		}
	}
	if (!fileError && std::filesystem::exists(sourceGenerated)) {
		if (duplicate) {
			std::filesystem::copy(sourceGenerated, destinationGenerated, std::filesystem::copy_options::recursive, fileError);
		} else {
			std::filesystem::rename(sourceGenerated, destinationGenerated, fileError);
		}
		generatedPublished = !fileError;
	}
	if (!fileError) {
		for (ReferenceJournalEntry& entry : references) {
			std::string updated = entry.original;
			std::size_t cursor = 0u;
			while ((cursor = updated.find(impact.source, cursor)) != std::string::npos) {
				updated.replace(cursor, impact.source.size(), impact.destination);
				cursor += impact.destination.size();
			}
			std::ofstream output(entry.path, std::ios::binary | std::ios::trunc);
			output << updated;
			if (!output.good()) {
				fileError = std::make_error_code(std::errc::io_error);
				break;
			}
			result.repairedReferences.push_back(std::filesystem::relative(entry.path, projectRoot).generic_string());
		}
	}

	if (fileError) {
		for (const ReferenceJournalEntry& entry : references) {
			std::ofstream output(entry.path, std::ios::binary | std::ios::trunc);
			output << entry.original;
		}
		std::error_code rollbackError;
		if (generatedPublished) {
			if (duplicate) std::filesystem::remove_all(destinationGenerated, rollbackError);
			else std::filesystem::rename(destinationGenerated, sourceGenerated, rollbackError);
		}
		if (metaPublished) std::filesystem::rename(destinationMeta, sourceMeta, rollbackError);
		if (primaryPublished) {
			if (duplicate) std::filesystem::remove(destination->second, rollbackError);
			else std::filesystem::rename(destination->second, source->second, rollbackError);
		}
		result.errorCategory = "internal";
		result.error = "Asset publication failed: " + fileError.message();
		result.rollback = rollbackError ? "failed" : "completed";
		return result;
	}

	// Publication is protected and complete before the editor index is
	// refreshed, so observers never receive an index pointing at half-moved
	// source/generated files.
	ScanProject();
	result.published = true;
	result.path = impact.destination;
	result.assetId = stableId;
	result.rollback = "not_required";
	return result;
}
