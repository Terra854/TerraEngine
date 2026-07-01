// AssetDatabase.cpp — Editor asset database implementation
//
// Implements the asset lifecycle management including automated project scanning, metadata persistence, and asynchronous asset compilation. It integrates with the filesystem watcher to detect external changes and syncs the database with the engine's resource manager.

#include "AssetDatabase.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "Engine/Core/Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "Editor/AssetCompilers/ShaderCompiler.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "AssetImportResult.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"

using json = nlohmann::json;

namespace {
	constexpr auto kRecentlyRemovedAssetRetention = std::chrono::seconds(5);
	constexpr const char* kModelPackageCompilerName = "TerraEngineModelPackageCompiler.exe";
	constexpr const char* kMeshCompilerName = "TerraEngineMeshCompiler.exe";
	constexpr const char* kTextureCompilerName = "TerraEngineTextureCompiler.exe";
	constexpr const char* kShaderCompilerName = "TerraEngineShaderCompiler.exe";

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

	bool IsIgnoredAssetDatabasePath(const std::filesystem::path& path) {
		if (path.extension() == ".meta") {
			std::filesystem::path sourcePath = path;
			sourcePath.replace_extension("");
			return IsImportSidecarPath(sourcePath);
		}
		return IsImportSidecarPath(path);
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

	// RegisterGeneratedModelPackageResources bridges package-owned sub-assets into the
	// runtime resource table. Some generated package outputs, especially
	// textures, are not standalone source assets in the browser, but materials
	// still reference their stable package GUIDs and need ResourceManager paths.
	void RegisterGeneratedModelPackageResources(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& buildRoot)
	{
		const std::filesystem::path sidecarPath = TerraEngine::AssetPipeline::GetImportResultPath(sourcePath);
		if (!std::filesystem::exists(sidecarPath))
		{
			return;
		}

		std::ifstream in(sidecarPath);
		if (!in)
		{
			return;
		}

		json result;
		try
		{
			in >> result;
		}
		catch (const json::exception& ex)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping generated model package resource registration for malformed import sidecar "
				<< sidecarPath << ": " << ex.what());
			return;
		}
		for (const json& mesh : result.value("generatedMeshes", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(mesh.value("guid", ""));
			const json virtualGeometry = mesh.value("virtualGeometry", json::object());
			const std::string packageStatus = result.value("status", std::string());
			const std::string virtualGeometryStatus = virtualGeometry.value("status", std::string());
			if (!guid
				|| packageStatus == "Failed"
				|| !mesh.contains("compiledPath")
				|| virtualGeometryStatus == "Failed"
				|| virtualGeometryStatus == "NeedsRebuild")
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / mesh["compiledPath"].get<std::string>(), AssetType::Mesh);
		}

		for (const json& material : result.value("generatedMaterials", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(material.value("guid", ""));
			if (!guid || !material.contains("compiledPath"))
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / material["compiledPath"].get<std::string>(), AssetType::Material);
		}

		for (const json& texture : result.value("generatedTextures", json::array()))
		{
			const auto guid = TerraEngine::ParseUUID(texture.value("guid", ""));
			if (!guid || !texture.contains("compiledPath"))
			{
				continue;
			}
			ResourceManager::Instance().RegisterAssetPath(*guid, buildRoot / texture["compiledPath"].get<std::string>(), AssetType::Texture);
		}
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

	template <typename RecordType>
	bool ReadVirtualGeometryRecordTable(std::ifstream& input, std::uint32_t offset, std::uint32_t count, std::vector<RecordType>& outRecords)
	{
		outRecords.clear();
		if (offset == 0u || count == 0u)
		{
			return true;
		}
		outRecords.resize(count);
		input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		input.read(reinterpret_cast<char*>(outRecords.data()), static_cast<std::streamsize>(outRecords.size() * sizeof(RecordType)));
		return static_cast<bool>(input);
	}
}

// AssetDatabase::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
AssetDatabase& AssetDatabase::Instance() {
	static AssetDatabase instance;
	return instance;
}

AssetDatabase::AssetDatabase() {}
AssetDatabase::~AssetDatabase() {}

// AssetDatabase::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Initialize(const std::filesystem::path& projectRootPath) {
	this->projectRoot = std::filesystem::absolute(projectRootPath);
	this->assetRoot = this->projectRoot / "Assets";
	this->buildRoot = this->projectRoot / "Build" / "Assets";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(assetRoot)) std::filesystem::create_directories(assetRoot);
	if (!std::filesystem::exists(buildRoot)) std::filesystem::create_directories(buildRoot);

	assets.clear();
	recentlyRemovedAssets.clear();
	TERRAENGINE_LOG(LogLevel::Info, "AssetDatabase initialized. Watching: " << assetRoot);
	
	// Initial Scan
	ScanProject();
	
	// Start Watcher
	fileWatcher.Start(assetRoot);
}

// AssetDatabase::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Shutdown() {
	CancelAllImportsAndWait(true);
	fileWatcher.Stop();
	assets.clear();
	deferredImports.clear();
	recentlyRemovedAssets.clear();
}

// AssetDatabase::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::Update() {
	PruneRecentlyRemovedAssets();

	// Process Watcher Events
	auto events = fileWatcher.PopEvents();
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
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto jobIt = asyncJobs.begin(); jobIt != asyncJobs.end(); ) {
		if (jobIt->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			const std::filesystem::path completedPath = jobIt->absSourcePath;
			bool compiled = jobIt->future.get();
			const bool cancelled = jobIt->progressState && jobIt->progressState->cancelRequested.load();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (cancelled) {
				SetImportProgress(jobIt->progressState, "Cancelled", "Cleaning up cancelled import outputs.", 1.0f, false, true);
				CleanupCancelledImport(jobIt->metadata, jobIt->absSourcePath, jobIt->progressState->removeAssetAfterCancel.load());
			} else if (!compiled) {
				SetImportProgress(jobIt->progressState, "Failed", "Compilation failed; see the log for details.", 1.0f, false, true);
				TERRAENGINE_LOG(LogLevel::Error, "Asset compilation failed for: " << jobIt->absSourcePath);
			} else {
				SetImportProgress(jobIt->progressState, "Registering", "Updating metadata and editor resources on the main thread.", 0.95f, false, false);
				FinishImport(jobIt->metadata, jobIt->absSourcePath, jobIt->needsImport);
				SetImportProgress(jobIt->progressState, "Complete", "Import finished.", 1.0f, false, false);
			}
			jobIt = asyncJobs.erase(jobIt);
			if (const auto deferredIt = deferredImports.find(completedPath); deferredIt != deferredImports.end()) {
				readyDeferredImports.push_back(*deferredIt);
				deferredImports.erase(deferredIt);
			}
		} else {
			++jobIt;
		}
	}
	for (const auto& [path, force] : readyDeferredImports) {
		if (std::filesystem::exists(path)) {
			ImportAssetWithProgress(path, force);
		}
	}
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
		if (job.absSourcePath == normalizedPath && (!job.progressState || !job.progressState->cancelRequested.load())) {
			return true;
		}
	}
	return false;
}

size_t AssetDatabase::GetCompileJobsCount() const {
	size_t count = 0u;
	for (const AsyncCompileJob& job : asyncJobs) {
		if (!job.progressState || !job.progressState->cancelRequested.load()) {
			++count;
		}
	}
	return count;
}

bool AssetDatabase::CancelImport(const std::filesystem::path& absSourcePath, bool removeAssetAfterCancel) {
	const std::filesystem::path normalizedPath = std::filesystem::absolute(absSourcePath);
	for (AsyncCompileJob& job : asyncJobs) {
		if (job.absSourcePath == normalizedPath && job.progressState) {
			const AssetImportProgress snapshot = SnapshotImportProgress(job.progressState);
			job.progressState->cancelRequested.store(true);
			job.progressState->removeAssetAfterCancel.store(removeAssetAfterCancel);
			TerminateCompilerProcess(job.progressState);
			SetImportProgress(job.progressState, "Cancelling", "Stopping import and preparing cleanup.", snapshot.progress, false, false);
			CleanupCancelledImport(job.metadata, job.absSourcePath, removeAssetAfterCancel);
			return true;
		}
	}
	return false;
}

void AssetDatabase::RequestCancelAllImports(bool removeAssetsAfterCancel) {
	for (AsyncCompileJob& job : asyncJobs) {
		if (job.progressState) {
			const AssetImportProgress snapshot = SnapshotImportProgress(job.progressState);
			job.progressState->cancelRequested.store(true);
			job.progressState->removeAssetAfterCancel.store(removeAssetsAfterCancel);
			TerminateCompilerProcess(job.progressState);
			SetImportProgress(job.progressState, "Cancelling", "Stopping import and preparing cleanup.", snapshot.progress, false, false);
		}
		CleanupCancelledImport(job.metadata, job.absSourcePath, removeAssetsAfterCancel);
	}
	deferredImports.clear();
}

void AssetDatabase::CancelAllImportsAndWait(bool removeAssetsAfterCancel) {
	RequestCancelAllImports(removeAssetsAfterCancel);

	for (AsyncCompileJob& job : asyncJobs) {
		if (job.future.valid()) {
			job.future.wait();
			try {
				(void)job.future.get();
			} catch (const std::exception& exception) {
				TERRAENGINE_LOG(LogLevel::Warning, "Asset import worker ended during cancellation: " << exception.what());
			} catch (...) {
				TERRAENGINE_LOG(LogLevel::Warning, "Asset import worker ended during cancellation with an unknown error.");
			}
		}
		CleanupCancelledImport(job.metadata, job.absSourcePath, job.progressState && job.progressState->removeAssetAfterCancel.load());
	}
	asyncJobs.clear();
	deferredImports.clear();
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

bool AssetDatabase::ShouldCompileAssetAsync(const std::string& importerName) const {
	// Heavy asset compilers run on worker jobs and, on Windows, through child
	// executables. That lets the UI show per-file progress and cancel the
	// compiler process while FinishImport still publishes state on the main thread.
	return importerName == "TextureImporter"
		|| importerName == "MeshImporter"
		|| importerName == "ShaderImporter"
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
	(void)progressState;
#endif
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
	const auto absSourcePath = std::filesystem::absolute(sourcePath);
	if (absSourcePath.extension() == ".meta") return;
	if (IsIgnoredAssetDatabasePath(absSourcePath)) return;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::is_directory(absSourcePath)) return;
	if (IsAssetCompiling(absSourcePath)) {
		// Worker-backed imports can receive repeated file watcher or reimport
		// requests while the first compile is still running. Coalesce those into
		// one follow-up pass so large glTF/FBX files do not start duplicate jobs.
		deferredImports[absSourcePath] = deferredImports[absSourcePath] || force;
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
	if (assets.count(absSourcePath)) {
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
	else if (metadata.importerName == "ScriptImporter") compiledRelativePath.clear();
	else if (metadata.importerName == "GenericImporter") compiledRelativePath.clear(); 
	
	// Store relative path in metadata
	metadata.compiledPath = compiledRelativePath;

	if (!metadata.compiledPath.empty()) {
		// Resolve Absolute Paths for checking/compiling
		// Use buildRoot to ensure path includes Assets/
		std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;

		if (needsImport || !std::filesystem::exists(fullCompiledPath)) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::exists(fullCompiledPath.parent_path())) {
				std::filesystem::create_directories(fullCompiledPath.parent_path());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (allowAsyncCompile && ShouldCompileAssetAsync(metadata.importerName)) {
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

				auto future = std::async(std::launch::async, [this, metadata, progressState, resolvedMeshSettings]() {
					try {
						SetImportProgress(progressState, "Importing", "Reading source data and writing compiled output.", 0.15f, true, false);
						const bool compiled = CompileAsset(metadata, progressState, resolvedMeshSettings);
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
				});
				asyncJobs.push_back({metadata, std::move(future), needsImport, absSourcePath, progressState});
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

// AssetDatabase::FinishImport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::FinishImport(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath, bool needsImport) {
	// Always save metadata
	SaveMetadata(metadata);

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

	// Register with Engine ResourceManager so it can resolve GUIDs to compiled paths
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadata.compiledPath.empty()) {
		AssetType type;
		bool known = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) { type = AssetType::Mesh; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "TextureImporter") { type = AssetType::Texture; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "MaterialImporter") { type = AssetType::Material; known = true; }
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (metadata.importerName == "ShaderImporter") { type = AssetType::ShaderCollection; known = true; }

		if (known) {
			// Register source path as an alias so the engine can resolve things like "viking_room.png" to the same GUID.
			// We do this first so that idToPath (the preferred load path) is eventually overwritten by the compiled path.
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, absSourcePath, type);

			std::filesystem::path fullCompiled = buildRoot / metadata.compiledPath;
			ResourceManager::Instance().RegisterAssetPath(metadata.guid, fullCompiled, type);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (needsImport) {
				if (type == AssetType::Material) {
					RenderSystem::Instance().ReloadMaterial(metadata.guid);
				} else if (type == AssetType::Mesh) {
					RenderSystem::Instance().ReloadMesh(metadata.guid);
				} else if (type == AssetType::Texture) {
					RenderSystem::Instance().ReloadTexture(metadata.guid);
				}
			}
		}
	}

	if (TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName))
	{
		RegisterGeneratedModelPackageMaterialMetadata(absSourcePath);
		RegisterGeneratedModelPackageResources(absSourcePath, buildRoot);
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
}

void AssetDatabase::CleanupModelPackageGeneratedAssets(const AssetMetadata& metadata, const std::filesystem::path& absSourcePath) {
	if (!TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName)) {
		return;
	}

	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	const bool loadedImportResult = TerraEngine::AssetPipeline::LoadModelPackageImportResult(absSourcePath, importResult);
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
			ResourceManager::Instance().DeleteResource(mesh.guid, AssetType::Mesh);
		}
		for (const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures) {
			ResourceManager::Instance().DeleteResource(texture.guid, AssetType::Texture);
		}
		for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials) {
			ResourceManager::Instance().DeleteResource(material.guid, AssetType::Material);
			const std::filesystem::path materialSourcePath = std::filesystem::absolute(assetRoot / material.sourcePath);
			if (auto materialIt = assets.find(materialSourcePath); materialIt != assets.end()) {
				assets.erase(materialIt);
			}
			std::error_code removeMetaError;
			std::filesystem::remove(materialSourcePath.string() + ".meta", removeMetaError);
		}
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
	std::error_code ec;
	if (assets.find(absSourcePath) != assets.end()) {
		RemoveAsset(absSourcePath);
	}

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

	if (removeSourceAsset) {
		std::filesystem::remove(absSourcePath, ec);
		ec.clear();
	}

	pendingEvents.erase(absSourcePath);
	deferredImports.erase(absSourcePath);
	TERRAENGINE_LOG(LogLevel::Info, "Cancelled asset import and cleaned partial outputs for: " << absSourcePath);
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".cs") return "ScriptImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuiml") return "UIImporter";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ext == ".tuss") return "StyleImporter";
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
	const std::filesystem::path fullSourcePath = assetRoot / metadata.sourcePath;
	const std::filesystem::path fullCompiledPath = buildRoot / metadata.compiledPath;

#ifdef _WIN32
	const std::filesystem::path compilerPath = GetEditorExecutableDirectory() / kModelPackageCompilerName;
	if (!std::filesystem::exists(compilerPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Model package compiler executable is missing: " << compilerPath);
		return false;
	}

	const std::wstring commandLine = QuoteCommandLineArgument(compilerPath)
		+ L" --source " + QuoteCommandLineArgument(fullSourcePath)
		+ L" --asset-root " + QuoteCommandLineArgument(assetRoot)
		+ L" --build-root " + QuoteCommandLineArgument(buildRoot)
		+ L" --source-relative " + QuoteCommandLineArgument(metadata.sourcePath)
		+ L" --package-guid " + QuoteCommandLineArgument(UuidToString(metadata.guid))
		+ L" --shader-guid " + QuoteCommandLineArgument(UuidToString(ResourceManager::Instance().GetDefaultShaderCollectionID()));
	return RunCompilerProcess(compilerPath, commandLine, progressState, "Model package compiler", fullCompiledPath);
#else
	TerraEngine::AssetPipeline::ModelPackageImportOptions options{};
	options.sourcePath = fullSourcePath;
	options.assetRoot = assetRoot;
	options.buildRoot = buildRoot;
	options.sourceRelativePath = metadata.sourcePath;
	options.packageGuid = metadata.guid;
	options.shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	options.forceReimport = true;
	options.progressCallback = [this, progressState](float progress, std::string_view stage, std::string_view message) {
		if (progressState) {
			SetImportProgress(progressState, std::string(stage), std::string(message), progress, false, false);
		}
	};
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult = TerraEngine::AssetPipeline::ImportModelPackage(options);
	if (!TerraEngine::AssetPipeline::SaveModelPackageImportResult(fullSourcePath, importResult)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to save model package import result sidecar for " << fullSourcePath);
	}
	return importResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed && std::filesystem::exists(fullCompiledPath);
#endif
}

bool AssetDatabase::RunCompilerProcess(const std::filesystem::path& compilerPath, const std::wstring& commandLine, const std::shared_ptr<AsyncImportProgressState>& progressState, std::string_view logPrefix, const std::filesystem::path& expectedOutputPath) {
#ifdef _WIN32
	SECURITY_ATTRIBUTES securityAttributes{};
	securityAttributes.nLength = sizeof(securityAttributes);
	securityAttributes.bInheritHandle = TRUE;

	HANDLE readPipe = nullptr;
	HANDLE writePipe = nullptr;
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

	PROCESS_INFORMATION processInfo{};
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

	if (!created) {
		CloseHandle(readPipe);
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

	WaitForSingleObject(processInfo.hProcess, INFINITE);
	DWORD exitCode = 1u;
	GetExitCodeProcess(processInfo.hProcess, &exitCode);
	if (progressState) {
		std::lock_guard<std::mutex> lock(progressState->processMutex);
		if (progressState->compilerProcessHandle == processInfo.hProcess) {
			progressState->compilerProcessHandle = nullptr;
		}
	}
	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
		std::filesystem::remove(stagedCompiledPath, cleanupError);
		std::filesystem::remove(stagedMeshReportPath, cleanupError);
		return false;
	}
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
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
	if (!PublishCompiledAssetAtomically(stagedCompiledPath, fullCompiledPath)) {
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
	else if (metadata.importerName == "MaterialImporter") {
		std::error_code ec;
		std::filesystem::copy_file(fullSourcePath, fullCompiledPath, std::filesystem::copy_options::overwrite_existing, ec);
		return !ec;
	}
	else {
		return true;
	}
}

// AssetDatabase::RemoveAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AssetDatabase::RemoveAsset(const std::filesystem::path& sourcePath) {
	const auto absPath = std::filesystem::absolute(sourcePath);
	auto it = assets.find(absPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it != assets.end()) {
		const auto& metadata = it->second;

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

		if (knownResource) {
			ResourceManager::Instance().DeleteResource(metadata.guid, type);
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

bool AssetDatabase::IsMeshAsset(const AssetMetadata& metadata) const {
	return metadata.importerName == "MeshImporter"
		|| TerraEngine::AssetPipeline::IsModelPackageImporterName(metadata.importerName);
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

	MeshHeader header{};
	input.read(reinterpret_cast<char*>(&header), sizeof(MeshHeader));
	if (!input || std::memcmp(header.magic, "TEMF", 4) != 0) {
		outMetadata.warnings.push_back("Compiled mesh header is not a valid TEMF file.");
		return false;
	}

	outMetadata.readable = true;
	outMetadata.meshletCount = header.meshletCount;
	outMetadata.payloadGeneration = header.virtualGeometryPayloadGeneration;
	outMetadata.hasVirtualGeometry = (header.meshletFlags & kMeshletHeaderFlagHasVirtualGeometry) != 0u;
	if (!outMetadata.hasVirtualGeometry) {
		outMetadata.warnings.push_back("Virtual geometry payload is not present; rebuild the mesh to generate it.");
		outMetadata.rebuildGuidance = "Rebuild this mesh from source to generate virtual geometry metadata for the current TEMF v1 contract.";
		return true;
	}

	if (header.virtualGeometryHeaderOffset == 0u
		|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)
		|| header.virtualGeometryPayloadGeneration != kVirtualGeometryPayloadGeneration) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry payload metadata is stale or incomplete; rebuild this mesh.");
		outMetadata.rebuildGuidance = "Rebuild this mesh so the compiled artifact is regenerated with the current virtual geometry payload generation.";
		return true;
	}

	VirtualGeometryHeader virtualHeader{};
	input.seekg(static_cast<std::streamoff>(header.virtualGeometryHeaderOffset), std::ios::beg);
	input.read(reinterpret_cast<char*>(&virtualHeader), sizeof(VirtualGeometryHeader));
	if (!input || virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry root header could not be read; rebuild this mesh.");
		outMetadata.rebuildGuidance = "Rebuild this mesh because the virtual geometry root header is unreadable or incompatible.";
		return true;
	}

	outMetadata.detailLevelCount = virtualHeader.detailLevelCount;
	outMetadata.sourceTriangleCount = virtualHeader.sourceTriangleCount;
	outMetadata.requestedTargetCount = virtualHeader.requestedTargetCount;
	outMetadata.skippedDetailCount = virtualHeader.skippedDetailCount;
	outMetadata.clusterCount = virtualHeader.clusterCount;
	outMetadata.materialSectionCount = virtualHeader.materialSectionCount;
	outMetadata.pageCount = virtualHeader.pageCount;
	if (virtualHeader.detailLevelCount == 0u || virtualHeader.cullableGroupCount == 0u) {
		outMetadata.needsRebuild = true;
		outMetadata.warnings.push_back("Virtual geometry contains no renderable detail levels or cullable groups.");
		outMetadata.rebuildGuidance = "Rebuild this mesh because the compiled virtual geometry tables contain no renderable detail.";
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
		ImportAsset(assetRoot / metadata.sourcePath, true);
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

	VirtualGeometryAssetMetadata meshMetadata;
	if (!TryGetVirtualGeometryMetadata(metadata, meshMetadata) || !meshMetadata.hasVirtualGeometry) {
		return true;
	}

	const std::filesystem::path compiledPath = buildRoot / metadata.compiledPath;
	std::ifstream input(compiledPath, std::ios::binary);
	if (!input) {
		return true;
	}

	MeshHeader header{};
	input.read(reinterpret_cast<char*>(&header), sizeof(header));
	if (!input
		|| std::memcmp(header.magic, "TEMF", 4) != 0
		|| header.virtualGeometryHeaderOffset == 0u
		|| header.virtualGeometryHeaderSize != sizeof(VirtualGeometryHeader)) {
		return true;
	}

	VirtualGeometryHeader virtualHeader{};
	input.seekg(static_cast<std::streamoff>(header.virtualGeometryHeaderOffset), std::ios::beg);
	input.read(reinterpret_cast<char*>(&virtualHeader), sizeof(virtualHeader));
	if (!input || virtualHeader.payloadGeneration != kVirtualGeometryPayloadGeneration) {
		return true;
	}

	std::vector<VirtualGeometryRequestedTargetRecord> requestedTargets;
	std::vector<VirtualGeometryDetailLevelRecord> detailLevels;
	std::vector<VirtualGeometrySkippedDetailRecord> skippedDetails;
	if (!ReadVirtualGeometryRecordTable(input, virtualHeader.requestedTargetTableOffset, virtualHeader.requestedTargetCount, requestedTargets)
		|| !ReadVirtualGeometryRecordTable(input, virtualHeader.detailLevelTableOffset, virtualHeader.detailLevelCount, detailLevels)
		|| !ReadVirtualGeometryRecordTable(input, virtualHeader.skippedDetailTableOffset, virtualHeader.skippedDetailCount, skippedDetails)) {
		return true;
	}
	std::string diagnosticStrings;
	if (virtualHeader.diagnosticStringOffset != 0u && virtualHeader.diagnosticStringSize != 0u)
	{
		diagnosticStrings.resize(virtualHeader.diagnosticStringSize);
		input.seekg(static_cast<std::streamoff>(virtualHeader.diagnosticStringOffset), std::ios::beg);
		input.read(diagnosticStrings.data(), static_cast<std::streamsize>(diagnosticStrings.size()));
		if (!input)
		{
			diagnosticStrings.clear();
		}
	}

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
		summary.status = meshMetadata.needsRebuild ? "NeedsRebuild" : "Cooked";
		summary.desiredTriangleCount = meshMetadata.sourceTriangleCount;
		summary.desiredMeshletCount = meshMetadata.meshletCount;
		summary.actualTriangleCount = meshMetadata.sourceTriangleCount;
		summary.actualMeshletCount = meshMetadata.meshletCount;
		summary.triangleCount = meshMetadata.sourceTriangleCount;
		summary.meshletCount = meshMetadata.meshletCount;
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
	for (auto& [path, metadata] : assets) {
		if (metadata.importerName != "ScriptImporter") {
			continue;
		}

		metadata.settings["compileStatus"] = compileState;
		metadata.settings["lastBuildSummary"] = summary;
		SaveMetadata(metadata);
	}
}
