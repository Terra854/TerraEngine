// ResourceManager.cpp — Resource Manager System Implementation
//
// Implements the core logic for the ResourceManager, managing asset directories and resource registries.
// It coordinates the loading of different asset formats and integrates with the renderer to synchronize GPU resources.
// This module handles project-level asset resolution and serves as the primary gateway for content consumption.

#include "Resource/ResourceManager.h"
#include "Renderer/RenderSystem.h"

#include <fstream>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Runtime/ContentMount/ContentMount.h"
// Concrete resource definitions are required here so the cache-owned
// std::unique_ptr instances have complete types when the deleters compile.
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "Resource/ShaderCollection.h"
#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"

namespace
{
	constexpr std::string_view kDefaultManifestProductName = "TerraEngine";
	constexpr std::string_view kDefaultManifestVersion = "1.0.0";

	// SupportedExtensionsForAssetType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::vector<std::string_view> SupportedExtensionsForAssetType(AssetType type)
	{
		switch (type)
		{
		case AssetType::Mesh:
			return { ".temf" };
		case AssetType::Texture:
			return { ".tetf" };
		case AssetType::Material:
			return { ".tmat" };
		case AssetType::ShaderCollection:
			return { ".tspf" };
		}

		return {};
	}

	std::string JsonStringOr(const nlohmann::json& object, std::string_view key, std::string fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_string())
		{
			return it->get<std::string>();
		}
		return fallback;
	}

	std::uint32_t JsonUIntOr(const nlohmann::json& object, std::string_view key, std::uint32_t fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_number_unsigned())
		{
			return it->get<std::uint32_t>();
		}
		return fallback;
	}

	template <typename T>
	std::uint64_t VectorPayloadBytes(const std::vector<T>& values)
	{
		return static_cast<std::uint64_t>(values.size()) * static_cast<std::uint64_t>(sizeof(T));
	}

	std::uint64_t AddPayloadBytes(std::uint64_t left, std::uint64_t right)
	{
		constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
		return left > max - right ? max : left + right;
	}

	float JsonFloatOr(const nlohmann::json& object, std::string_view key, float fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_number())
		{
			return it->get<float>();
		}
		return fallback;
	}

	bool JsonBoolOr(const nlohmann::json& object, std::string_view key, bool fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_boolean())
		{
			return it->get<bool>();
		}
		return fallback;
	}

	std::uint32_t QualityRank(const std::string& quality)
	{
		if (quality == "disabled") return 0u;
		if (quality == "low") return 1u;
		if (quality == "medium") return 2u;
		return 3u;
	}

	RenderSettings SettingsFromRenderProfileJson(const nlohmann::json& renderProfile)
	{
		RenderSettings settings;
		const std::string shadows = JsonStringOr(renderProfile, "shadows", "high");
		const std::string ao = JsonStringOr(renderProfile, "ambientOcclusion", "high");
		const std::string vrs = JsonStringOr(renderProfile, "variableRateShading", "disabled");
		const std::uint32_t shadowRank = QualityRank(shadows);
		const std::uint32_t aoRank = QualityRank(ao);

		settings.maxShadowLights = shadowRank == 0u ? 0u : (shadowRank == 1u ? 2u : (shadowRank == 2u ? 4u : 8u));
		settings.shadowResolution = shadowRank == 0u ? 0u : (shadowRank == 1u ? 1024u : (shadowRank == 2u ? 1536u : 2048u));
		settings.shadowFilteringSize = shadowRank == 0u ? 0u : (shadowRank == 1u ? 3u : 5u);
		settings.ambientOcclusionStrength = aoRank == 0u ? 0.0f : 1.0f;
		settings.ambientOcclusionQuality = aoRank;
		settings.variableRateShading = vrs == "disabled" ? 0u : 1u;
		settings.validationEnabled = JsonBoolOr(renderProfile, "validationEnabled", false) ? 1u : 0u;
		settings.debugEnabled = JsonBoolOr(renderProfile, "debugEnabled", false) ? 1u : 0u;
		return settings;
	}

	std::uint64_t JsonUInt64Or(const nlohmann::json& object, std::string_view key, std::uint64_t fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_number_unsigned())
		{
			return it->get<std::uint64_t>();
		}
		return fallback;
	}

	double JsonDoubleOr(const nlohmann::json& object, std::string_view key, double fallback)
	{
		if (auto it = object.find(std::string(key)); it != object.end() && it->is_number())
		{
			return it->get<double>();
		}
		return fallback;
	}

	TerraEngine::Resource::ResidencyPolicy ResidencyPolicyFromJson(const nlohmann::json& object)
	{
		TerraEngine::Resource::ResidencyPolicy policy = TerraEngine::Resource::MakeDefaultResidencyPolicy();
		policy.enabled = JsonBoolOr(object, "enabled", policy.enabled);
		policy.cleanupIntervalSeconds = JsonDoubleOr(object, "cleanupIntervalSeconds", policy.cleanupIntervalSeconds);
		if (auto cpuIt = object.find("cpu"); cpuIt != object.end() && cpuIt->is_object())
		{
			policy.cpu.idleSecondsThreshold = JsonDoubleOr(*cpuIt, "idleSecondsThreshold", policy.cpu.idleSecondsThreshold);
			policy.cpu.memoryBudgetBytes = JsonUInt64Or(*cpuIt, "memoryBudgetBytes", policy.cpu.memoryBudgetBytes);
			policy.cpu.reloadGraceSeconds = JsonDoubleOr(*cpuIt, "reloadGraceSeconds", policy.cpu.reloadGraceSeconds);
		}
		else
		{
			policy.cpu.idleSecondsThreshold = JsonDoubleOr(object, "idleSecondsThreshold", policy.cpu.idleSecondsThreshold);
			policy.cpu.memoryBudgetBytes = JsonUInt64Or(object, "memoryBudgetBytes", policy.cpu.memoryBudgetBytes);
			policy.cpu.reloadGraceSeconds = JsonDoubleOr(object, "reloadGraceSeconds", policy.cpu.reloadGraceSeconds);
		}
		if (auto gpuIt = object.find("gpu"); gpuIt != object.end() && gpuIt->is_object())
		{
			policy.gpu.idleSecondsThreshold = JsonDoubleOr(*gpuIt, "idleSecondsThreshold", policy.gpu.idleSecondsThreshold);
			policy.gpu.memoryBudgetBytes = JsonUInt64Or(*gpuIt, "memoryBudgetBytes", policy.gpu.memoryBudgetBytes);
			policy.gpu.reloadGraceSeconds = JsonDoubleOr(*gpuIt, "reloadGraceSeconds", policy.gpu.reloadGraceSeconds);
			policy.gpu.strictBudgetMode = JsonBoolOr(*gpuIt, "strictBudgetMode", policy.gpu.strictBudgetMode);
		}
		else
		{
			policy.gpu.idleSecondsThreshold = JsonDoubleOr(object, "idleSecondsThreshold", policy.gpu.idleSecondsThreshold);
			policy.gpu.memoryBudgetBytes = JsonUInt64Or(object, "memoryBudgetBytes", policy.gpu.memoryBudgetBytes);
			policy.gpu.reloadGraceSeconds = JsonDoubleOr(object, "reloadGraceSeconds", policy.gpu.reloadGraceSeconds);
			policy.gpu.strictBudgetMode = JsonBoolOr(object, "strictBudgetMode", policy.gpu.strictBudgetMode);
		}
		return TerraEngine::Resource::SanitizeResidencyPolicy(policy);
	}

	TerraEngine::Resource::MemoryBudgetInteraction MemoryBudgetInteractionFromString(const std::string& value)
	{
		if (value == "prefer-streaming-release")
		{
			return TerraEngine::Resource::MemoryBudgetInteraction::PreferStreamingRelease;
		}
		if (value == "conservative")
		{
			return TerraEngine::Resource::MemoryBudgetInteraction::Conservative;
		}
		return TerraEngine::Resource::MemoryBudgetInteraction::UseResidencyPolicy;
	}

	TerraEngine::Resource::StreamingPolicy StreamingPolicyFromJson(const nlohmann::json& object)
	{
		TerraEngine::Resource::StreamingPolicy policy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
		policy.enabled = JsonBoolOr(object, "enabled", policy.enabled);
		policy.deferScenePayloadsOnLoad = JsonBoolOr(object, "deferScenePayloadsOnLoad", policy.deferScenePayloadsOnLoad);
		policy.streamingDistance = JsonDoubleOr(object, "streamingDistance", policy.streamingDistance);
		policy.visibleRetentionSeconds = JsonDoubleOr(object, "visibleRetentionSeconds", policy.visibleRetentionSeconds);
		policy.neededRetentionSeconds = JsonDoubleOr(object, "neededRetentionSeconds", policy.neededRetentionSeconds);
		policy.maxLoadsPerUpdate = JsonUIntOr(object, "maxLoadsPerUpdate", policy.maxLoadsPerUpdate);
		policy.textureMipStreamingEnabled = JsonBoolOr(object, "textureMipStreamingEnabled", policy.textureMipStreamingEnabled);
		policy.textureMinStreamMip = JsonUIntOr(object, "textureMinStreamMip", policy.textureMinStreamMip);
		policy.textureMaxStreamMip = JsonUIntOr(object, "textureMaxStreamMip", policy.textureMaxStreamMip);
		policy.virtualGeometryPageMemoryBudgetBytes = JsonUInt64Or(object, "virtualGeometryPageMemoryBudgetBytes", policy.virtualGeometryPageMemoryBudgetBytes);
		policy.virtualGeometryPageUploadBudgetBytes = JsonUInt64Or(object, "virtualGeometryPageUploadBudgetBytes", policy.virtualGeometryPageUploadBudgetBytes);
		policy.virtualGeometryPageRetirementBudgetBytes = JsonUInt64Or(object, "virtualGeometryPageRetirementBudgetBytes", policy.virtualGeometryPageRetirementBudgetBytes);
		policy.memoryBudgetInteraction = MemoryBudgetInteractionFromString(JsonStringOr(object, "memoryBudgetInteraction", std::string(TerraEngine::Resource::ToString(policy.memoryBudgetInteraction))));
		policy.diagnosticsEnabled = JsonBoolOr(object, "diagnosticsEnabled", policy.diagnosticsEnabled);
		return TerraEngine::Resource::SanitizeStreamingPolicy(policy);
	}

	TerraEngine::Resource::StreamableAssetType ToStreamableAssetType(AssetType type)
	{
		switch (type)
		{
		case AssetType::Mesh: return TerraEngine::Resource::StreamableAssetType::Mesh;
		case AssetType::Texture: return TerraEngine::Resource::StreamableAssetType::Texture;
		case AssetType::Material: return TerraEngine::Resource::StreamableAssetType::Material;
		case AssetType::ShaderCollection: return TerraEngine::Resource::StreamableAssetType::ShaderCollection;
		}
		return TerraEngine::Resource::StreamableAssetType::Other;
	}

	AssetType AssetTypeFromResidencyType(TerraEngine::Resource::ResidencyAssetType type)
	{
		// Residency diagnostics store their own asset enum so tests and UI do
		// not depend on ResourceManager internals. Cleanup converts back to the
		// concrete cache enum here when the fast lookup map has no entry.
		switch (type)
		{
		case TerraEngine::Resource::ResidencyAssetType::Mesh:
			return AssetType::Mesh;
		case TerraEngine::Resource::ResidencyAssetType::Texture:
			return AssetType::Texture;
		case TerraEngine::Resource::ResidencyAssetType::ShaderCollection:
			return AssetType::ShaderCollection;
		case TerraEngine::Resource::ResidencyAssetType::Material:
		case TerraEngine::Resource::ResidencyAssetType::Shader:
		case TerraEngine::Resource::ResidencyAssetType::FontAtlas:
		case TerraEngine::Resource::ResidencyAssetType::Other:
			return AssetType::Material;
		}
		return AssetType::Material;
	}

	double ResidencyClockSeconds()
	{
		using Clock = std::chrono::steady_clock;
		static const Clock::time_point startTime = Clock::now();
		const std::chrono::duration<double> elapsed = Clock::now() - startTime;
		return elapsed.count();
	}

	TerraEngine::Resource::StreamingProtectionReason ComputeStreamingProtection(
		const std::vector<TerraEngine::Resource::StreamingNeed>& needs,
		const TerraEngine::Resource::StreamingPolicy& policy)
	{
		for (const TerraEngine::Resource::StreamingNeed& need : needs)
		{
			if (TerraEngine::Resource::IsProtectedNeed(need.needType, policy))
			{
				return TerraEngine::Resource::ProtectionForNeed(need.needType);
			}
		}
		return TerraEngine::Resource::StreamingProtectionReason::None;
	}

	void RefreshStreamableNeedState(
		TerraEngine::Resource::StreamableAssetRecord& record,
		const TerraEngine::Resource::StreamingPolicy& policy,
		double nowSeconds)
	{
		// Streaming needs are transient signals that must be re-published while
		// active. Prune expired entries even when no new need is added; otherwise
		// looking away from an object would leave a stale active-view protection
		// in diagnostics until the asset was released.
		record.needSources = TerraEngine::Resource::MergeStreamingNeeds(std::move(record.needSources), nowSeconds);
		record.protectionReason = ComputeStreamingProtection(record.needSources, policy);
	}

	bool HasResidentStreamingMemory(TerraEngine::Resource::StreamablePayloadState state)
	{
		// Diagnostics can outlive the exact frame that loaded or released a
		// payload. Treat only states that actually own RAM or VRAM as resident so
		// the window never reports stale byte counts for deferred/released assets.
		switch (state)
		{
		case TerraEngine::Resource::StreamablePayloadState::RamResident:
		case TerraEngine::Resource::StreamablePayloadState::UploadingVram:
		case TerraEngine::Resource::StreamablePayloadState::VramResident:
		case TerraEngine::Resource::StreamablePayloadState::PendingRelease:
			return true;
		default:
			return false;
		}
	}

	std::optional<RenderSettings> SettingsFromRenderSettingsJson(const nlohmann::json& renderSettingsDocument)
	{
		const nlohmann::json* settingsObject = &renderSettingsDocument;
		if (auto settingsIt = renderSettingsDocument.find("settings"); settingsIt != renderSettingsDocument.end() && settingsIt->is_object())
		{
			settingsObject = &(*settingsIt);
		}

		if (!settingsObject->is_object())
		{
			return std::nullopt;
		}

		RenderSettings settings;
		// Keep this field list aligned with RenderSystem::LoadSettings and
		// SaveSettings. The export manifest carries the full editor-authored
		// settings object so standalone builds do not have to reverse-engineer
		// exact values from lossy profile labels.
		settings.shadowResolution = JsonUIntOr(*settingsObject, "shadowResolution", 2048u);
		settings.shadowFilteringSize = JsonUIntOr(*settingsObject, "shadowFilteringSize", 5u);
		settings.maxUploadedLights = JsonUIntOr(*settingsObject, "maxUploadedLights", 256u);
		settings.maxDynamicLights = JsonUIntOr(*settingsObject, "maxDynamicLights", 128u);
		settings.maxShadowLights = JsonUIntOr(*settingsObject, "maxShadowLights", 8u);
		settings.maxAreaLights = JsonUIntOr(*settingsObject, "maxAreaLights", 4u);
		settings.enableLTC = JsonUIntOr(*settingsObject, "enableLTC", 1u);
		settings.enableIBL = JsonUIntOr(*settingsObject, "enableIBL", 1u);
		settings.indirectStrength = JsonFloatOr(*settingsObject, "indirectStrength", 1.0f);
		settings.ambientOcclusionStrength = JsonFloatOr(*settingsObject, "ambientOcclusionStrength", 1.0f);
		settings.ambientOcclusionQuality = JsonUIntOr(*settingsObject, "ambientOcclusionQuality", 2u);
		settings.anisotropicFiltering = JsonUIntOr(*settingsObject, "anisotropicFiltering", 4u);
		settings.variableRateShading = JsonUIntOr(*settingsObject, "variableRateShading", 0u);
		// Diagnostic startup flags are canonical at the top level of
		// RenderSettings.json so export manifests can mirror them without
		// interpreting the nested settings payload. Accept nested values as a
		// compatibility fallback for hand-authored files.
		settings.validationEnabled = JsonBoolOr(
			renderSettingsDocument,
			"validationEnabled",
			JsonBoolOr(*settingsObject, "validationEnabled", false)) ? 1u : 0u;
		settings.debugEnabled = JsonBoolOr(
			renderSettingsDocument,
			"debugEnabled",
			JsonBoolOr(*settingsObject, "debugEnabled", false)) ? 1u : 0u;
		return settings;
	}

	std::string RenderProfileSummaryFromJson(const nlohmann::json& renderProfile)
	{
		std::ostringstream stream;
		stream
			<< "profile=" << JsonStringOr(renderProfile, "profileName", "Default")
			<< " shadows=" << JsonStringOr(renderProfile, "shadows", "high")
			<< " ao=" << JsonStringOr(renderProfile, "ambientOcclusion", "high")
			<< " vrs=" << JsonStringOr(renderProfile, "variableRateShading", "disabled")
			<< " textureBudgetMb=" << JsonUIntOr(renderProfile, "textureBudgetMb", 512u)
			<< " resolutionScale=" << JsonFloatOr(renderProfile, "resolutionScale", 1.0f)
			<< " validation=" << (JsonBoolOr(renderProfile, "validationEnabled", false) ? "on" : "off")
			<< " debug=" << (JsonBoolOr(renderProfile, "debugEnabled", false) ? "on" : "off");
		return stream.str();
	}

	void ApplyRenderDiagnosticFlagsFromSettingsFile(
		const std::filesystem::path& settingsPath,
		bool& validationEnabled,
		bool& debugEnabled)
	{
		// The renderer must be able to request Vulkan validation in any compile
		// mode, but normal project sessions do not have an export manifest.
		// Read only the tiny diagnostic flags here; RenderSystem still owns the
		// full render-quality settings load so there is one authority for frame
		// behavior.
		if (settingsPath.empty() || !std::filesystem::exists(settingsPath))
		{
			return;
		}

		try
		{
			std::ifstream file(settingsPath);
			nlohmann::json document;
			file >> document;
			validationEnabled = JsonBoolOr(document, "validationEnabled", validationEnabled);
			debugEnabled = JsonBoolOr(document, "debugEnabled", debugEnabled);
		}
		catch (const std::exception& exception)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to read render diagnostic flags from " << settingsPath << ": " << exception.what());
		}
	}

	// AssetReferenceLooksLikePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool AssetReferenceLooksLikePath(std::string_view assetReference)
	{
		const std::filesystem::path queryPath(assetReference);
		return queryPath.is_absolute()
			|| queryPath.has_extension()
			|| assetReference.find('/') != std::string_view::npos
			|| assetReference.find('\\') != std::string_view::npos;
	}

	// HasNativeExtensionForAssetType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool HasNativeExtensionForAssetType(const std::filesystem::path& path, AssetType type)
	{
		std::string extension = path.extension().generic_string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::string_view nativeExtension : SupportedExtensionsForAssetType(type))
		{
			if (extension == nativeExtension)
			{
				return true;
			}
		}

		return false;
	}

	// FindEditorProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::optional<std::filesystem::path> FindEditorProjectRoot(const std::filesystem::path& assetRoot)
	{
		if (assetRoot.empty())
		{
			return std::nullopt;
		}

		std::filesystem::path cursor = assetRoot.lexically_normal();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int depth = 0; depth < 6; ++depth)
		{
			if (std::filesystem::exists(cursor / "ProjectSettings" / "ProjectSettings.terra")
				|| std::filesystem::exists(cursor / "game.json"))
			{
				return cursor;
			}

			const std::filesystem::path parent = cursor.parent_path();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (parent.empty() || parent == cursor)
			{
				break;
			}

			cursor = parent;
		}

		return std::nullopt;
	}
}

// ResourceManager::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ResourceManager& ResourceManager::Instance()
{
	static ResourceManager instance;
	return instance;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const Material& resource) const
{
	std::uint64_t bytes = sizeof(Material);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.storage));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.resolvedSlots));
	for (const std::string& warning : resource.fallbackWarnings)
	{
		bytes = AddPayloadBytes(bytes, static_cast<std::uint64_t>(warning.size()));
	}
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const Mesh& resource) const
{
	std::uint64_t bytes = sizeof(Mesh);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.storage));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.meshletVertexRemapDecoded));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.meshletRecords));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.meshletSections));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.meshletBounds));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryDetailLevels));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryMaterialSections));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometrySectionRanges));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryCullableGroups));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryPages));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryRequestedTargets));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometrySkippedDetails));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryClusters));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryClusterPageLinks));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryConeSafety));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryValidationFlags));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.virtualGeometryDiagnostics));
	bytes = AddPayloadBytes(bytes, static_cast<std::uint64_t>(resource.virtualGeometryDiagnosticStrings.size()));
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const Shader& resource) const
{
	std::uint64_t bytes = sizeof(Shader);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.storage));
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const ShaderCollection& resource) const
{
	std::uint64_t bytes = sizeof(ShaderCollection);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.stages));
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const Texture& resource) const
{
	std::uint64_t bytes = sizeof(Texture);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.levelTable));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.chunkTable));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.storage));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.transcodedData));
	return bytes;
}

// Out-of-line destructor ensures the concrete resource types are complete when
// the cache members are destroyed, keeping unique_ptr deleters well-defined.
ResourceManager::~ResourceManager() = default;

ResourceManager::ResourceManager()
	// threadPool is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: threadPool(ThreadPool::instance())
{
	assetRoot = std::filesystem::current_path();
	engineRoot = std::filesystem::current_path();
	m_ResidencyPolicy = TerraEngine::Resource::MakeDefaultResidencyPolicy();
	m_StreamingPolicy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
	materialCache.assetType = AssetType::Material;
	materialCache.tracksResidency = true;
	meshCache.assetType = AssetType::Mesh;
	meshCache.tracksResidency = true;
	shaderCollectionCache.assetType = AssetType::ShaderCollection;
	// Shader collections are part of pipeline creation state, so register them
	// for exact CPU accounting and let RegisterResidencyResource pin them until
	// pipeline lifetime can safely publish release candidates.
	shaderCollectionCache.tracksResidency = true;
	textureCache.assetType = AssetType::Texture;
	textureCache.tracksResidency = true;

	basist::basisu_transcoder_init();
	basist::uastc_init();
}

// ResourceManager::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::Initialize()
{
	// Each engine session starts with a fresh startup-scene override so stale
	// export manifest data cannot leak into the next launch.
	m_ConfiguredStartupScene.reset();
	m_ConfiguredRenderSettings.reset();
	m_RenderProfileSummary = "profile=Default";
	m_RenderValidationEnabled = false;
	m_RenderDebugEnabled = false;
	m_RendererRecoveryPolicy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();
	m_LastStartupError.reset();
	ResetManifestMetadata();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_ConfiguredExportArchivePath.has_value()) {
		ApplyRenderDiagnosticFlagsFromSettingsFile(
			GetProjectRoot() / "ProjectSettings" / "RenderSettings.json",
			m_RenderValidationEnabled,
			m_RenderDebugEnabled);
		if (!m_RenderValidationEnabled && !m_RenderDebugEnabled)
		{
			ApplyRenderDiagnosticFlagsFromSettingsFile(
				assetRoot / "ProjectSettings" / "RenderSettings.json",
				m_RenderValidationEnabled,
				m_RenderDebugEnabled);
		}
		return;
	}

	try {
		const std::filesystem::path archivePath = *m_ConfiguredExportArchivePath;
		TERRAENGINE_LOG(LogLevel::Info, "Export runtime initializing from TEPAK archive: " << archivePath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!std::filesystem::exists(archivePath)) {
			throw std::runtime_error("Configured TEPAK archive does not exist: " + archivePath.string());
		}

		// Mount the export archive now, inside engine startup, so scene and
		// resource loading can depend on the mounted content root without the
		// launcher performing content I/O before Engine::Run().
		if (!ContentMount::Instance().Mount(archivePath)) {
			throw std::runtime_error("Failed to mount TEPAK archive: " + archivePath.string());
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!ContentMount::Instance().Exists("game.json")) {
			throw std::runtime_error("Mounted TEPAK archive is missing game.json: " + archivePath.string());
		}

		const std::vector<uint8_t> manifestBytes = ContentMount::Instance().ReadAll("game.json");
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (manifestBytes.empty()) {
			throw std::runtime_error("game.json inside TEPAK archive is empty: " + archivePath.string());
		}

		const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
		const nlohmann::json manifest = nlohmann::json::parse(manifestText);
		ApplyManifestMetadata(manifest, archivePath.stem().string());
		std::uint32_t registeredAssetCount = 0u;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (manifest.contains("startupScene")) {
			m_ConfiguredStartupScene = manifest["startupScene"].get<std::string>();
		}

		if (auto settingsIt = manifest.find("renderSettings"); settingsIt != manifest.end() && settingsIt->is_object()) {
			m_ConfiguredRenderSettings = SettingsFromRenderSettingsJson(*settingsIt);
		}

		if (auto renderIt = manifest.find("renderProfile"); renderIt != manifest.end() && renderIt->is_object()) {
			if (!m_ConfiguredRenderSettings.has_value()) {
				m_ConfiguredRenderSettings = SettingsFromRenderProfileJson(*renderIt);
			}
			m_RenderProfileSummary = RenderProfileSummaryFromJson(*renderIt);
			m_RenderValidationEnabled = JsonBoolOr(*renderIt, "validationEnabled", false);
			m_RenderDebugEnabled = JsonBoolOr(*renderIt, "debugEnabled", false);
		}

		if (auto recoveryIt = manifest.find("rendererRecovery"); recoveryIt != manifest.end() && recoveryIt->is_object()) {
			m_RendererRecoveryPolicy = TerraEngine::Renderer::MakeExportRendererRecoveryPolicy(
				m_RenderValidationEnabled,
				JsonBoolOr(*recoveryIt, "diagnosticsEnabled", true),
				recoveryIt->value("fatalMessage", std::string("Graphics device lost. Restart the application to restore rendering.")));
		}
		else
		{
			m_RendererRecoveryPolicy = TerraEngine::Renderer::MakeExportRendererRecoveryPolicy(
				m_RenderValidationEnabled,
				m_RenderDebugEnabled,
				"Graphics device lost. Restart the application to restore rendering.");
		}

		if (auto residencyIt = manifest.find("residencyPolicy"); residencyIt != manifest.end() && residencyIt->is_object()) {
			SetResidencyPolicy(ResidencyPolicyFromJson(*residencyIt));
		}

		if (auto streamingIt = manifest.find("streamingPolicy"); streamingIt != manifest.end() && streamingIt->is_object()) {
			SetStreamingPolicy(StreamingPolicyFromJson(*streamingIt));
		}

		// Register the exported asset lookup table before SceneManager loads the
		// startup scene so UUID references inside scenes and prefabs can resolve
		// back to logical archive paths immediately.
		if (manifest.contains("assets")) {
			for (const auto& assetEntry : manifest["assets"]) {
				try {
					const std::string guidStr = assetEntry.value("guid", "");
					const std::string path = assetEntry.value("path", "");
					const uint32_t typeInt = assetEntry.value("type", 0u);

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (auto guid = TerraEngine::ParseUUID(guidStr)) {
						RegisterAssetPath(*guid, path, static_cast<AssetType>(typeInt));
						++registeredAssetCount;
					}
				}
				catch (...) {
					// Skip malformed entries so one bad manifest record does not
					// prevent every other exported asset from being registered.
				}
			}
		}

		TERRAENGINE_LOG(LogLevel::Info,
			"Export runtime mounted. archive=" << archivePath
			<< " engineRoot=" << engineRoot
			<< " assetRoot=" << assetRoot
			<< " startupScene=" << (m_ConfiguredStartupScene.has_value() ? m_ConfiguredStartupScene->generic_string() : std::string("<none>"))
			<< " renderProfile={" << m_RenderProfileSummary << "}"
			<< " manifestAssets=" << registeredAssetCount);
	}
	catch (const std::exception& exception) {
		// Leave the manager in a clean state if export startup fails halfway
		// through so the engine's higher-level exception handling can retry or
		// report the failure without a half-mounted archive lingering around.
		const std::string errorMessage = exception.what();
		Shutdown();
		m_LastStartupError = errorMessage;
		throw;
	}
	catch (...) {
		Shutdown();
		m_LastStartupError = "Unknown export startup failure.";
		throw;
	}
}

// ResourceManager::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::Shutdown()
{
	m_ConfiguredStartupScene.reset();
	m_ConfiguredRenderSettings.reset();
	m_RenderProfileSummary = "profile=Default";
	m_RenderValidationEnabled = false;
	m_RenderDebugEnabled = false;
	m_RendererRecoveryPolicy = TerraEngine::Renderer::MakeEditorRendererRecoveryPolicy();
	ResetManifestMetadata();
	ClearCaches();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ContentMount::Instance().IsMounted()) {
		ContentMount::Instance().Unmount();
	}
}

// ResourceManager::SetEngineRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::SetEngineRoot(const std::filesystem::path& engineRootPath)
{
	engineRoot = engineRootPath.lexically_normal();
	TERRAENGINE_LOG(LogLevel::Debug, "Engine root set to " << engineRoot);
}

// ResourceManager::SetAssetRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::SetAssetRoot(const std::filesystem::path& assetRootPath)
{
	// Normalize immediately so the cache keys stay predictable even if
	// callers provide paths with redundant separators or relative jumps.
	assetRoot = assetRootPath.lexically_normal();
	if (!IsExportRuntime())
	{
		RefreshEditorProjectManifest();
	}
	// Verbose debug logging makes it obvious which asset tree subsequent loads will pull from.
	TERRAENGINE_LOG(LogLevel::Debug, "Asset root set to " << assetRoot);
}

// ResourceManager::GetAssetRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ResourceManager::GetAssetRoot() const
{
	return assetRoot;
}

// ResourceManager::GetProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ResourceManager::GetProjectRoot() const
{
	// Export runtimes do not have the editor's authored project layout, so the
	// runtime root doubles as the project root for writable top-level settings.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (IsExportRuntime())
	{
		return assetRoot;
	}

	// Prefer explicit project markers so custom asset-folder names still resolve
	// correctly when the editor is pointed somewhere inside a project checkout.
	if (const std::optional<std::filesystem::path> projectRoot = FindEditorProjectRoot(assetRoot))
	{
		return *projectRoot;
	}

	// Fresh projects may not have written ProjectSettings.terra or game.json yet.
	// In that common case, an Assets asset root still means the project root is
	// its parent directory.
	std::string assetFolderName = assetRoot.filename().string();
	std::transform(assetFolderName.begin(), assetFolderName.end(), assetFolderName.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (assetFolderName == "assets" && assetRoot.has_parent_path())
	{
		return assetRoot.parent_path();
	}

	// Fall back to the configured asset root so callers still receive a stable,
	// normalized directory even if no project markers can be found.
	return assetRoot;
}

// ResourceManager::ConfigureExportArchive is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ConfigureExportArchive(const std::filesystem::path& archivePath)
{
	if (archivePath.empty()) {
		m_ConfiguredExportArchivePath.reset();
		return;
	}

	// Anchor both engine and asset roots to the export folder so path-derived
	// cache keys and default shader lookups stay correct even if the process is
	// launched from a different working directory.
	m_ConfiguredExportArchivePath = std::filesystem::absolute(archivePath);
	const std::filesystem::path runtimeRoot = m_ConfiguredExportArchivePath->parent_path();
	SetEngineRoot(runtimeRoot);
	SetAssetRoot(runtimeRoot);
	TERRAENGINE_LOG(LogLevel::Info,
		"Configured export runtime paths. archive=" << *m_ConfiguredExportArchivePath
		<< " runtimeRoot=" << runtimeRoot);
}

// ResourceManager::GetConfiguredStartupScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<std::filesystem::path> ResourceManager::GetConfiguredStartupScene() const
{
	return m_ConfiguredStartupScene;
}

std::optional<std::string> ResourceManager::GetLastStartupError() const
{
	return m_LastStartupError;
}

// ResourceManager::GetProductName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ResourceManager::GetProductName() const
{
	return m_ProductName;
}

// ResourceManager::GetVersion is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ResourceManager::GetVersion() const
{
	return m_Version;
}

std::string ResourceManager::GetRenderProfileSummary() const
{
	return m_RenderProfileSummary;
}

bool ResourceManager::IsRenderValidationEnabled() const
{
	return m_RenderValidationEnabled;
}

bool ResourceManager::IsRenderDebugEnabled() const
{
	return m_RenderDebugEnabled;
}

void ResourceManager::SetRenderValidationEnabled(bool enabled)
{
	// Validation layers are a startup/backend request rather than a render-pass
	// toggle. Editor preferences still store the user's intent here so the next
	// backend creation can read the preference without writing project files.
	m_RenderValidationEnabled = enabled;
}

void ResourceManager::SetRenderDebugEnabled(bool enabled)
{
	// Render debug diagnostics are a TerraEngine runtime switch, so the editor
	// can apply them immediately. Vulkan validation is intentionally excluded
	// because layers must be requested before the Vulkan instance is created.
	m_RenderDebugEnabled = enabled;
}

TerraEngine::Renderer::RendererRecoveryPolicy ResourceManager::GetRendererRecoveryPolicy() const
{
	return m_RendererRecoveryPolicy;
}

std::optional<RenderSettings> ResourceManager::GetConfiguredRenderSettings() const
{
	return m_ConfiguredRenderSettings;
}

// ResourceManager::IsExportRuntime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ResourceManager::IsExportRuntime() const
{
	// Export sessions always configure a TEPAK archive before engine startup.
	// Editor/game-project sessions leave this unset and continue using the
	// authored on-disk project layout instead.
	return m_ConfiguredExportArchivePath.has_value();
}

// ResourceManager::ResetManifestMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ResetManifestMetadata(std::string_view fallbackProductName)
{
	m_ProductName = fallbackProductName.empty()
		? std::string(kDefaultManifestProductName)
		: std::string(fallbackProductName);
	m_Version = std::string(kDefaultManifestVersion);
}

// ResourceManager::ApplyManifestMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ApplyManifestMetadata(const nlohmann::json& manifest, std::string_view fallbackProductName)
{
	ResetManifestMetadata(fallbackProductName);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string())
	{
		const std::string manifestName = nameIt->get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!manifestName.empty())
		{
			m_ProductName = manifestName;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto versionIt = manifest.find("version"); versionIt != manifest.end() && versionIt->is_string())
	{
		const std::string manifestVersion = versionIt->get<std::string>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!manifestVersion.empty())
		{
			m_Version = manifestVersion;
		}
	}
}

// ResourceManager::RefreshEditorProjectManifest is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::RefreshEditorProjectManifest()
{
	const std::optional<std::filesystem::path> projectRoot = FindEditorProjectRoot(assetRoot);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!projectRoot.has_value())
	{
		ResetManifestMetadata();
		return;
	}

	const std::string fallbackProductName = projectRoot->filename().string();
	ResetManifestMetadata(fallbackProductName);

	const std::filesystem::path manifestPath = *projectRoot / "game.json";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(manifestPath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Project manifest is missing at " << manifestPath << "; using fallback product metadata.");
		return;
	}

	try
	{
		std::ifstream manifestStream(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!manifestStream)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Project manifest could not be opened at " << manifestPath << "; using fallback product metadata.");
			return;
		}

		const nlohmann::json manifest = nlohmann::json::parse(manifestStream);
		ApplyManifestMetadata(manifest, fallbackProductName);

		// Editor projects use game.json as the project-level runtime contract.
		// Loading these policies here keeps editor previews, export manifests,
		// and standalone runtime startup aligned without storing resource
		// cleanup or streaming knobs in individual build profiles.
		if (auto residencyIt = manifest.find("residencyPolicy"); residencyIt != manifest.end() && residencyIt->is_object())
		{
			SetResidencyPolicy(ResidencyPolicyFromJson(*residencyIt));
		}

		if (auto streamingIt = manifest.find("streamingPolicy"); streamingIt != manifest.end() && streamingIt->is_object())
		{
			SetStreamingPolicy(StreamingPolicyFromJson(*streamingIt));
		}
	}
	catch (const std::exception& exception)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Project manifest could not be parsed at " << manifestPath << ": " << exception.what());
	}
}

// ResourceManager::LoadDefaultResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::LoadDefaultResources()
{
	TerraEngine::UUID defaultMeshId{};
	defaultMeshId.fill(0);
	defaultMeshId[15] = 1;

	TerraEngine::UUID defaultFragId{};
	defaultFragId.fill(0);
	defaultFragId[15] = 2;

	TerraEngine::UUID defaultTaskId{};
	defaultTaskId.fill(0);
	defaultTaskId[15] = 3;

	TerraEngine::UUID defaultMeshOnlyCollectionId{};
	defaultMeshOnlyCollectionId.fill(0);
	defaultMeshOnlyCollectionId[15] = 1;

	TerraEngine::UUID defaultTaskCollectionId{};
	defaultTaskCollectionId.fill(0);
	defaultTaskCollectionId[15] = 2;

	auto ResolveEnginePath = [this](const std::filesystem::path& p) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (p.is_absolute()) return p.lexically_normal();
		return (engineRoot / p).lexically_normal();
	};

	// Pre-register Mesh Shader
	const std::string meshPath = "Shaders/Default.mesh.tspf";
	std::string meshKey = NormalizeKey(ResolveEnginePath(meshPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[meshKey] = defaultMeshId;
	}
	
	// LoadShader normally uses assetRoot, so we must pass the absolute path
	LoadShader(ResolveEnginePath(meshPath));

	// Pre-register Task Shader
	const std::string taskPath = "Shaders/Default.task.tspf";
	std::string taskKey = NormalizeKey(ResolveEnginePath(taskPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[taskKey] = defaultTaskId;
	}
	LoadShader(ResolveEnginePath(taskPath));
	const bool taskShaderReady = (GetShader(defaultTaskId) != nullptr);

	// Pre-register Fragment Shader
	const std::string fragPath = "Shaders/Default.frag.tspf";
	std::string fragKey = NormalizeKey(ResolveEnginePath(fragPath));
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId[fragKey] = defaultFragId;
	}
	LoadShader(ResolveEnginePath(fragPath));
	// Pre-register mesh-only fallback collection UUID.
	std::ostringstream fallbackCollectionKeyStream;
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	fallbackCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string fallbackCollectionKey = fallbackCollectionKeyStream.str();

	// Pre-register task+mesh+fragment collection UUID.
	std::ostringstream taskCollectionKeyStream;
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Task) << ":" << taskKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Mesh) << ":" << meshKey << ";";
	taskCollectionKeyStream << static_cast<std::uint32_t>(ShaderStage::Fragment) << ":" << fragKey << ";";
	const std::string taskCollectionKey = taskCollectionKeyStream.str();
	{
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId[fallbackCollectionKey] = defaultMeshOnlyCollectionId;
		shaderCollectionCache.pathToId[taskCollectionKey] = defaultTaskCollectionId;
	}

	std::vector<ShaderStagePath> fallbackStages = {
		{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
		{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
	};
	LoadShaderCollection(fallbackStages);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (taskShaderReady)
	{
		std::vector<ShaderStagePath> taskStages = {
			{ ShaderStage::Task, ResolveEnginePath(taskPath) },
			{ ShaderStage::Mesh, ResolveEnginePath(meshPath) },
			{ ShaderStage::Fragment, ResolveEnginePath(fragPath) }
		};
		LoadShaderCollection(taskStages);
	}
	else
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Default task shader was not available; renderer will use mesh-only fallback collection.");
	}
}

// ResourceManager::GetDefaultShaderCollectionID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::GetDefaultShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 1;
	return id;
}

// ResourceManager::GetDefaultTaskShaderCollectionID is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::GetDefaultTaskShaderCollectionID() const
{
	TerraEngine::UUID id{};
	id.fill(0);
	id[15] = 2;
	return id;
}

// ResourceManager::ClearCaches is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ClearCaches()
{
	// Acquire exclusive locks in a consistent order to avoid deadlocks if
	// multiple threads ever clear different caches concurrently.
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId.clear();
		materialCache.idToPath.clear();
		materialCache.resources.clear();
		materialCache.inFlight.clear();
	}
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId.clear();
		meshCache.idToPath.clear();
		meshCache.resources.clear();
		meshCache.inFlight.clear();
	}
	{
		std::unique_lock lock(m_MeshLocalBoundsMutex);
		m_MeshLocalBoundsCache.clear();
	}
	{
		std::unique_lock lock(shaderCache.mutex);
		shaderCache.pathToId.clear();
		shaderCache.idToPath.clear();
		shaderCache.resources.clear();
		shaderCache.inFlight.clear();
	}
	{
		// Clear paired shader lookups so future installs cannot reuse stale stage combinations.
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId.clear();
		shaderCollectionCache.idToPath.clear();
		shaderCollectionCache.resources.clear();
		shaderCollectionCache.inFlight.clear();
	}
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId.clear();
		textureCache.idToPath.clear();
		textureCache.resources.clear();
		textureCache.inFlight.clear();
	}
	ClearResidencyState();
	ClearStreamingState();
}

// ResourceManager::DeleteResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::DeleteResource(const TerraEngine::UUID& id, AssetType type)
{
	auto removeFromCache = [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto it = cache.pathToId.begin(); it != cache.pathToId.end(); ) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (it->second == id) {
				it = cache.pathToId.erase(it);
			}
			else {
				++it;
			}
		}
		cache.idToPath.erase(id);
		cache.resources.erase(id);
	};

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (type) {
	case AssetType::Mesh: removeFromCache(meshCache); break;
	case AssetType::Texture: removeFromCache(textureCache); break;
	case AssetType::Material: removeFromCache(materialCache); break;
	case AssetType::ShaderCollection: removeFromCache(shaderCollectionCache); break;
	}

	if (type == AssetType::Mesh)
	{
		std::unique_lock lock(m_MeshLocalBoundsMutex);
		m_MeshLocalBoundsCache.erase(id);
	}

	// Notify renderer to purge any GPU-side references/buffers
	RenderSystem::Instance().DeleteResource(id);
	{
		std::unique_lock lock(m_ResidencyMutex);
		m_ResidentAssets.erase(id);
		m_ResidentAssetTypes.erase(id);
	}
	{
		std::unique_lock lock(m_StreamingMutex);
		m_StreamableAssets.erase(id);
	}
}

void ResourceManager::SetResidencyPolicy(const TerraEngine::Resource::ResidencyPolicy& policy)
{
	std::unique_lock lock(m_ResidencyMutex);
	m_ResidencyPolicy = TerraEngine::Resource::SanitizeResidencyPolicy(policy);
}

TerraEngine::Resource::ResidencyPolicy ResourceManager::GetResidencyPolicy() const
{
	std::shared_lock lock(m_ResidencyMutex);
	return m_ResidencyPolicy;
}

void ResourceManager::SetResidencyDiagnosticsEnabled(bool enabled)
{
	std::unique_lock lock(m_ResidencyMutex);
	m_ResidencyPolicy.diagnosticsEnabled = enabled;
	m_ResidencyDiagnostics.diagnosticsEnabled = enabled;
}

TerraEngine::Resource::ResidencyDiagnosticsSummary ResourceManager::GetResidencyDiagnosticsSummary() const
{
	std::uint64_t frameId = 0u;
	std::vector<TerraEngine::Resource::ReleaseRecord> releaseRecords;
	{
		std::shared_lock lock(m_ResidencyMutex);
		frameId = m_ResidencyDiagnostics.frameId;
		releaseRecords = m_ResidencyDiagnostics.records;
	}

	// Residency diagnostics are queried directly by editor UI. Rebuild from
	// the live resident table so backend GPU uploads that happened after the
	// last cleanup tick are visible immediately instead of looking like
	// zero-byte VRAM residents until the next cleanup interval.
	const TerraEngine::Resource::ResidencySnapshot snapshot = BuildResidencySnapshot(frameId);
	return TerraEngine::Resource::BuildResidencyDiagnostics(
		snapshot,
		releaseRecords,
		snapshot.policy.diagnosticsEnabled);
}

bool ResourceManager::IsResourceResident(const TerraEngine::UUID& id) const
{
	std::shared_lock lock(m_ResidencyMutex);
	const auto it = m_ResidentAssets.find(id);
	return it != m_ResidentAssets.end()
		&& it->second.state != TerraEngine::Resource::ResidencyState::Released
		&& it->second.state != TerraEngine::Resource::ResidencyState::PendingRelease;
}

void ResourceManager::RegisterResidencyResource(
	const TerraEngine::UUID& id,
	AssetType type,
	const std::string& sourceKey,
	std::uint64_t cpuBytes,
	std::uint64_t gpuBytes,
	bool reloadable,
	TerraEngine::Resource::ResidencyState state)
{
	const TerraEngine::Resource::StreamingAssetPolicy assetPolicy = ResolveStreamingAssetPolicy(id, type);
	std::unique_lock lock(m_ResidencyMutex);
	TerraEngine::Resource::ResidentAsset& asset = m_ResidentAssets[id];
	asset.assetId = id;
	asset.assetType = TerraEngine::Resource::ToResidencyAssetType(type);
	asset.sourceKey = sourceKey;
	asset.state = state;
	asset.lastTouchedReason = "load";
	asset.lastUsedSeconds = ResidencyClockSeconds();
	asset.estimatedCpuBytes = cpuBytes;
	asset.estimatedGpuBytes = gpuBytes;
	asset.cpuPayloads.clear();
	asset.gpuAllocations.clear();
	if (cpuBytes > 0u)
	{
		TerraEngine::Resource::CpuPayloadOwnershipRecord cpuRecord;
		cpuRecord.ownerAssetId = id;
		cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::DecodedMesh;
		if (type == AssetType::Texture)
		{
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::TexturePixels;
		}
		else if (type == AssetType::Material)
		{
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::MaterialMetadata;
		}
		else if (type == AssetType::ShaderCollection)
		{
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::ShaderCollection;
		}
		cpuRecord.byteSize = cpuBytes;
		cpuRecord.state = TerraEngine::Resource::CpuPayloadOwnershipState::Retained;
		cpuRecord.detail = "ResourceManager retained CPU payload";
		asset.cpuPayloads.push_back(std::move(cpuRecord));
		asset.exactCpuBytes = TerraEngine::Resource::AggregateCpuPayloadBytes(asset.cpuPayloads, id);
	}
	else
	{
		asset.exactCpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
	}
	if (gpuBytes > 0u)
	{
		TerraEngine::Resource::GpuAllocationOwnershipRecord gpuRecord;
		gpuRecord.ownerAssetId = id;
		gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::MeshBuffer;
		if (type == AssetType::Texture)
		{
			gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::TextureImage;
		}
		else if (type == AssetType::Material)
		{
			gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::MaterialBuffer;
		}
		else if (type == AssetType::ShaderCollection)
		{
			gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::ShaderCollection;
		}
		gpuRecord.allocationId = std::hash<std::string>{}(TerraEngine::ToHexString(id) + ":resource-manager-registration");
		gpuRecord.byteSize = gpuBytes;
		gpuRecord.state = TerraEngine::Resource::GpuAllocationOwnershipState::Published;
		gpuRecord.detail = "ResourceManager registered GPU payload";
		asset.gpuAllocations.push_back(std::move(gpuRecord));
		asset.exactGpuBytes = TerraEngine::Resource::AggregateGpuAllocationBytes(asset.gpuAllocations, id);
	}
	else
	{
		asset.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
	}
	asset.reloadable = reloadable;
	asset.inFlight = false;
	asset.priorityClass = assetPolicy.priorityClass;
	asset.residencyTier = assetPolicy.residencyTier;
	if (type == AssetType::ShaderCollection)
	{
		// Shader collections are intentionally pinned until pipeline lifetime
		// and shader-collection residency are tracked together. This keeps
		// release builds from evicting a collection while Vulkan still needs it
		// to build or rebuild a graphics pipeline.
		asset.reloadable = false;
		asset.pinReason = TerraEngine::Resource::ResidencyPinReason::PolicyProtected;
		asset.state = TerraEngine::Resource::ResidencyState::Pinned;
	}
	if (asset.pinReason != TerraEngine::Resource::ResidencyPinReason::None)
	{
		asset.state = TerraEngine::Resource::ResidencyState::Pinned;
	}
	m_ResidentAssetTypes[id] = type;
}

void ResourceManager::TouchResource(
	const TerraEngine::UUID& id,
	AssetType type,
	std::uint64_t frameId,
	TerraEngine::Resource::ResidencyPinReason reason,
	std::string_view detail)
{
	const TerraEngine::Resource::StreamingAssetPolicy assetPolicy = ResolveStreamingAssetPolicy(id, type);
	std::unique_lock lock(m_ResidencyMutex);
	TerraEngine::Resource::ResidentAsset& asset = m_ResidentAssets[id];
	asset.assetId = id;
	asset.assetType = TerraEngine::Resource::ToResidencyAssetType(type);
	asset.lastUsedFrame = frameId;
	asset.lastUsedSeconds = ResidencyClockSeconds();
	asset.lastTouchedReason = detail.empty() ? std::string(TerraEngine::Resource::ToString(reason)) : std::string(detail);
	asset.priorityClass = assetPolicy.priorityClass;
	asset.residencyTier = assetPolicy.residencyTier;
	if (asset.sourceKey.empty())
	{
		asset.sourceKey = ToHexString(id);
	}
	if (reason != TerraEngine::Resource::ResidencyPinReason::None)
	{
		asset.pinReason = reason;
		asset.state = TerraEngine::Resource::ResidencyState::Pinned;
	}
	else if (asset.state != TerraEngine::Resource::ResidencyState::Pinned)
	{
		asset.state = TerraEngine::Resource::ResidencyState::Active;
	}
	m_ResidentAssetTypes[id] = type;
}

void ResourceManager::PinResource(
	const TerraEngine::UUID& id,
	AssetType type,
	TerraEngine::Resource::ResidencyPinReason reason,
	std::string_view detail)
{
	TouchResource(id, type, 0u, reason, detail);
}

void ResourceManager::UnpinResource(const TerraEngine::UUID& id)
{
	std::unique_lock lock(m_ResidencyMutex);
	if (auto it = m_ResidentAssets.find(id); it != m_ResidentAssets.end())
	{
		it->second.pinReason = TerraEngine::Resource::ResidencyPinReason::None;
		if (it->second.state == TerraEngine::Resource::ResidencyState::Pinned)
		{
			it->second.state = TerraEngine::Resource::ResidencyState::Idle;
		}
	}
}

void ResourceManager::MarkResourceUploadedToGpu(
	const TerraEngine::UUID& id,
	AssetType type,
	std::uint64_t frameId,
	std::uint64_t gpuBytes,
	std::string_view detail,
	std::uint32_t textureResidentMipCount,
	std::uint32_t textureSourceMipCount,
	std::uint32_t textureResidentMipOffset)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return;
	}

	const double nowSeconds = ResidencyClockSeconds();
	const TerraEngine::Resource::StreamingAssetPolicy assetPolicy = ResolveStreamingAssetPolicy(id, type);
	std::uint64_t cpuBytes = 0u;
	std::string sourceKey;
	std::string registeredSourceKey;
	const std::string fallbackSourceKey = ToHexString(id);
	{
		std::shared_lock lock(m_StreamingMutex);
		if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
		{
			registeredSourceKey = it->second.sourceKey;
		}
	}
	{
		std::unique_lock lock(m_ResidencyMutex);
		TerraEngine::Resource::ResidentAsset& asset = m_ResidentAssets[id];
		asset.assetId = id;
		asset.assetType = TerraEngine::Resource::ToResidencyAssetType(type);
		asset.lastUsedFrame = frameId;
		asset.lastUsedSeconds = nowSeconds;
		asset.lastTouchedReason = detail.empty() ? "backend upload" : std::string(detail);
		asset.estimatedGpuBytes = std::max(asset.estimatedGpuBytes, gpuBytes);
		if (gpuBytes > 0u)
		{
			TerraEngine::Resource::GpuAllocationOwnershipRecord gpuRecord;
			gpuRecord.ownerAssetId = id;
			gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::MeshBuffer;
			if (type == AssetType::Texture)
			{
				gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::TextureImage;
			}
			else if (type == AssetType::Material)
			{
				gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::MaterialBuffer;
			}
			else if (type == AssetType::ShaderCollection)
			{
				gpuRecord.resourceKind = TerraEngine::Resource::GpuAllocationOwnershipKind::ShaderCollection;
			}
			gpuRecord.allocationId = std::hash<std::string>{}(TerraEngine::ToHexString(id) + ":" + std::string(detail));
			gpuRecord.byteSize = gpuBytes;
			gpuRecord.state = TerraEngine::Resource::GpuAllocationOwnershipState::Published;
			gpuRecord.firstVisibleFrame = frameId;
			gpuRecord.detail = detail.empty() ? "backend upload" : std::string(detail);
			asset.gpuAllocations.clear();
			asset.gpuAllocations.push_back(std::move(gpuRecord));
			asset.exactGpuBytes = TerraEngine::Resource::AggregateGpuAllocationBytes(asset.gpuAllocations, id);
		}
		else
		{
			asset.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
		}
		asset.priorityClass = assetPolicy.priorityClass;
		asset.residencyTier = assetPolicy.residencyTier;
		if (asset.state != TerraEngine::Resource::ResidencyState::Pinned)
		{
			asset.state = TerraEngine::Resource::ResidencyState::Active;
		}
		// Backend-only uploads for generated package meshes may reach here
		// before a residency record has a path. Preserve the streamable
		// compiled path so diagnostics can still relate package parent rows to
		// generated child meshes that share the same TEMF payload.
		if (!registeredSourceKey.empty() && (asset.sourceKey.empty() || asset.sourceKey == fallbackSourceKey))
		{
			asset.sourceKey = registeredSourceKey;
		}
		else if (asset.sourceKey.empty())
		{
			asset.sourceKey = fallbackSourceKey;
		}
		cpuBytes = asset.estimatedCpuBytes;
		sourceKey = asset.sourceKey;
		m_ResidentAssetTypes[id] = type;
	}

	{
		std::unique_lock lock(m_StreamingMutex);
		TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
		record.assetId = id;
		record.assetType = ToStreamableAssetType(type);
		if (!record.assetPolicyExplicit)
		{
			record.assetPolicy = TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(record.assetType);
		}
		if (!record.sourceKey.empty() && (sourceKey.empty() || sourceKey == fallbackSourceKey))
		{
			sourceKey = record.sourceKey;
		}
		if (!sourceKey.empty())
		{
			record.sourceKey = sourceKey;
		}
		record.payloadState = TerraEngine::Resource::StreamablePayloadState::VramResident;
		record.estimatedCpuBytes = std::max(record.estimatedCpuBytes, cpuBytes);
		record.estimatedGpuBytes = std::max(record.estimatedGpuBytes, gpuBytes);
		record.exactCpuBytes = cpuBytes > 0u
			? TerraEngine::Resource::MakeExactResidencyBytes(cpuBytes)
			: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
		record.exactGpuBytes = gpuBytes > 0u
			? TerraEngine::Resource::MakeExactResidencyBytes(gpuBytes)
			: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
		if (type == AssetType::Texture)
		{
			record.textureResidentMipCount = textureResidentMipCount;
			record.textureSourceMipCount = textureSourceMipCount;
			record.textureResidentMipOffset = textureResidentMipOffset;
		}
		record.lastNeededSeconds = nowSeconds;
		record.failureReason.clear();
		record.residencyGeneration += 1u;
	}
}

TerraEngine::Resource::ResidencySnapshot ResourceManager::BuildResidencySnapshot(std::uint64_t frameId) const
{
	std::shared_lock lock(m_ResidencyMutex);
	TerraEngine::Resource::ResidencySnapshot snapshot;
	snapshot.frameId = frameId;
	snapshot.nowSeconds = ResidencyClockSeconds();
	snapshot.policy = m_ResidencyPolicy;
	snapshot.residentAssets.reserve(m_ResidentAssets.size());
	for (const auto& [id, asset] : m_ResidentAssets)
	{
		if (asset.state == TerraEngine::Resource::ResidencyState::Released)
		{
			continue;
		}
		snapshot.residentAssets.push_back(asset);
		if (asset.pinReason != TerraEngine::Resource::ResidencyPinReason::None)
		{
			snapshot.pinnedAssetIds.push_back(id);
		}
		if (asset.lastUsedFrame == frameId || asset.state == TerraEngine::Resource::ResidencyState::Active)
		{
			snapshot.activeAssetIds.push_back(id);
		}
	}
	return snapshot;
}

std::vector<TerraEngine::Resource::ReleaseRecord> ResourceManager::RunResidencyCleanup(std::uint64_t frameId)
{
	TerraEngine::Resource::ResidencyPolicy policy;
	std::vector<TerraEngine::Resource::ResidentAsset> assets;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Resource::StreamingProtectionReason> streamingProtections;
	const double nowSeconds = ResidencyClockSeconds();
	{
		std::unique_lock lock(m_StreamingMutex);
		for (auto& [id, streamable] : m_StreamableAssets)
		{
			RefreshStreamableNeedState(streamable, m_StreamingPolicy, nowSeconds);
			if (!streamable.needSources.empty() || streamable.protectionReason != TerraEngine::Resource::StreamingProtectionReason::None)
			{
				streamingProtections[id] = streamable.protectionReason;
			}
		}
		m_StreamingDiagnostics = BuildStreamingDiagnosticsLocked(frameId);
	}
	{
		std::shared_lock lock(m_ResidencyMutex);
		policy = m_ResidencyPolicy;
		if (!policy.enabled || (nowSeconds - m_LastResidencyCleanupSeconds) < policy.cleanupIntervalSeconds)
		{
			return {};
		}
		assets.reserve(m_ResidentAssets.size());
		for (const auto& [id, asset] : m_ResidentAssets)
		{
			TerraEngine::Resource::ResidentAsset cleanupAsset = asset;
			// A resource is active for the frame that touched it. On later
			// cleanup frames it becomes idle unless an explicit pin protects it.
			if (cleanupAsset.pinReason == TerraEngine::Resource::ResidencyPinReason::None
				&& cleanupAsset.state == TerraEngine::Resource::ResidencyState::Active
				&& cleanupAsset.lastUsedFrame != frameId)
			{
				cleanupAsset.state = TerraEngine::Resource::ResidencyState::Idle;
			}
			if (auto streamingIt = streamingProtections.find(id); streamingIt != streamingProtections.end())
			{
				// Streaming needs are folded into the existing residency snapshot
				// instead of running a second release path. This keeps RAM/VRAM
				// retirement synchronized with the backend frame fences.
				cleanupAsset.state = TerraEngine::Resource::ResidencyState::Active;
				if (streamingIt->second != TerraEngine::Resource::StreamingProtectionReason::None)
				{
					cleanupAsset.pinReason = TerraEngine::Resource::ResidencyPinReason::PolicyProtected;
				}
			}
			assets.push_back(std::move(cleanupAsset));
		}
	}

	TerraEngine::Resource::ResidencySnapshot cleanupSnapshot;
	cleanupSnapshot.frameId = frameId;
	cleanupSnapshot.nowSeconds = nowSeconds;
	cleanupSnapshot.policy = policy;
	cleanupSnapshot.residentAssets = assets;
	std::vector<TerraEngine::Resource::ReleaseCandidate> candidates = TerraEngine::Resource::BuildReleaseCandidates(cleanupSnapshot);
	std::vector<TerraEngine::Resource::ReleaseRecord> records;
	records.reserve(candidates.size());
	for (const TerraEngine::Resource::ReleaseCandidate& candidate : candidates)
	{
		AssetType type = AssetTypeFromResidencyType(candidate.assetType);
		{
			std::shared_lock lock(m_ResidencyMutex);
			if (auto typeIt = m_ResidentAssetTypes.find(candidate.assetId); typeIt != m_ResidentAssetTypes.end())
			{
				type = typeIt->second;
			}
		}

		TerraEngine::Resource::ReleaseRecord record;
		record.frameId = frameId;
		record.assetId = candidate.assetId;
		record.assetType = candidate.assetType;
		record.releaseReason = candidate.releaseReason;
		record.releaseDomain = candidate.releaseDomain;
		record.result = ReleaseResidentResource(candidate.assetId, type, candidate.releaseReason, candidate.releaseDomain, frameId, candidate.estimatedReclaimableBytes)
			? TerraEngine::Resource::ResidencyReleaseResult::Released
			: TerraEngine::Resource::ResidencyReleaseResult::Deferred;
		record.bytesReclaimed = record.result == TerraEngine::Resource::ResidencyReleaseResult::Released ? candidate.estimatedReclaimableBytes : 0u;
		record.explanation = record.result == TerraEngine::Resource::ResidencyReleaseResult::Released
			? std::string("Released ") + std::string(TerraEngine::Resource::ToString(candidate.releaseDomain)) + " residency payload."
			: "Release was deferred because the resource could not be detached this frame.";
		records.push_back(std::move(record));
	}

	{
		std::unique_lock lock(m_ResidencyMutex);
		m_LastResidencyCleanupFrame = frameId;
		m_LastResidencyCleanupSeconds = nowSeconds;
		TerraEngine::Resource::ResidencySnapshot diagnosticsSnapshot;
		diagnosticsSnapshot.frameId = frameId;
		diagnosticsSnapshot.nowSeconds = nowSeconds;
		diagnosticsSnapshot.policy = m_ResidencyPolicy;
		diagnosticsSnapshot.residentAssets.reserve(m_ResidentAssets.size());
		for (const auto& [id, asset] : m_ResidentAssets)
		{
			if (asset.state == TerraEngine::Resource::ResidencyState::Released)
			{
				continue;
			}
			diagnosticsSnapshot.residentAssets.push_back(asset);
		}
		for (const TerraEngine::Resource::ResidentAsset& asset : diagnosticsSnapshot.residentAssets)
		{
			if (asset.state == TerraEngine::Resource::ResidencyState::Active)
			{
				diagnosticsSnapshot.activeAssetIds.push_back(asset.assetId);
			}
			if (asset.pinReason != TerraEngine::Resource::ResidencyPinReason::None)
			{
				diagnosticsSnapshot.pinnedAssetIds.push_back(asset.assetId);
			}
		}
		m_ResidencyDiagnostics = TerraEngine::Resource::BuildResidencyDiagnostics(diagnosticsSnapshot, records, m_ResidencyPolicy.diagnosticsEnabled);
	}
	return records;
}

bool ResourceManager::ReleaseResidentResource(
	const TerraEngine::UUID& id,
	AssetType type,
	TerraEngine::Resource::ResidencyReleaseReason reason,
	TerraEngine::Resource::ResidencyReleaseDomain domain,
	std::uint64_t frameId,
	std::uint64_t /*candidateBytes*/)
{
	const bool releaseCpu = domain == TerraEngine::Resource::ResidencyReleaseDomain::Cpu
		|| domain == TerraEngine::Resource::ResidencyReleaseDomain::CpuAndGpu;
	const bool releaseGpu = domain == TerraEngine::Resource::ResidencyReleaseDomain::Gpu
		|| domain == TerraEngine::Resource::ResidencyReleaseDomain::CpuAndGpu;
	bool detachedCpuPayload = false;
	if (releaseCpu)
	{
		VisitAssetCache(type, [&](auto& cache) {
			std::unique_lock lock(cache.mutex);
			detachedCpuPayload = cache.resources.erase(id) > 0u;
		});
	}

	bool hasResidentPayload = false;
	std::uint64_t remainingCpuBytes = 0u;
	std::uint64_t remainingGpuBytes = 0u;
	{
		std::shared_lock lock(m_ResidencyMutex);
		if (auto it = m_ResidentAssets.find(id); it != m_ResidentAssets.end())
		{
			hasResidentPayload = it->second.state != TerraEngine::Resource::ResidencyState::Released
				&& ((releaseCpu && it->second.estimatedCpuBytes > 0u) || (releaseGpu && it->second.estimatedGpuBytes > 0u));
		}
	}

	if ((!releaseCpu || !detachedCpuPayload) && !hasResidentPayload)
	{
		return false;
	}

	if (releaseGpu)
	{
		RenderSystem::Instance().RetireLoadedResource(id, frameId, "resource-residency");
	}
	{
		std::unique_lock lock(m_ResidencyMutex);
		if (auto it = m_ResidentAssets.find(id); it != m_ResidentAssets.end())
		{
			if (releaseCpu)
			{
				it->second.estimatedCpuBytes = 0u;
				it->second.cpuPayloads.clear();
				it->second.exactCpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			}
			if (releaseGpu)
			{
				it->second.estimatedGpuBytes = 0u;
				it->second.gpuAllocations.clear();
				it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			}
			remainingCpuBytes = it->second.estimatedCpuBytes;
			remainingGpuBytes = it->second.estimatedGpuBytes;
			it->second.state = (remainingCpuBytes == 0u && remainingGpuBytes == 0u)
				? TerraEngine::Resource::ResidencyState::Released
				: TerraEngine::Resource::ResidencyState::Idle;
			it->second.releaseGeneration += 1u;
			it->second.lastTouchedReason = std::string(TerraEngine::Resource::ToString(reason));
		}
	}
	if (remainingCpuBytes == 0u && remainingGpuBytes == 0u)
	{
		MarkStreamableReleased(id, frameId);
	}
	else
	{
		UpdateStreamableLoadResult(id, type, {}, true, remainingCpuBytes, remainingGpuBytes);
	}
	return true;
}

void ResourceManager::ClearResidencyState()
{
	std::unique_lock lock(m_ResidencyMutex);
	m_ResidentAssets.clear();
	m_ResidentAssetTypes.clear();
	m_ResidencyDiagnostics = {};
	m_LastResidencyCleanupFrame = 0u;
	m_LastResidencyCleanupSeconds = 0.0;
}

void ResourceManager::SetStreamingPolicy(const TerraEngine::Resource::StreamingPolicy& policy)
{
	const TerraEngine::Resource::StreamingPolicy sanitizedPolicy = TerraEngine::Resource::SanitizeStreamingPolicy(policy);
	TerraEngine::Resource::StreamingPolicy previousPolicy;
	{
		std::unique_lock lock(m_StreamingMutex);
		previousPolicy = m_StreamingPolicy;
		m_StreamingPolicy = sanitizedPolicy;
		m_StreamingDiagnostics.diagnosticsEnabled = m_StreamingPolicy.diagnosticsEnabled;
	}

	const bool textureMipPolicyChanged =
		previousPolicy.textureMipStreamingEnabled != sanitizedPolicy.textureMipStreamingEnabled
		|| previousPolicy.textureMinStreamMip != sanitizedPolicy.textureMinStreamMip
		|| previousPolicy.textureMaxStreamMip != sanitizedPolicy.textureMaxStreamMip;
	if (!textureMipPolicyChanged)
	{
		return;
	}

	std::vector<TerraEngine::UUID> residentTextureIds;
	{
		std::shared_lock lock(m_ResidencyMutex);
		for (const auto& [id, asset] : m_ResidentAssets)
		{
			if (asset.assetType == TerraEngine::Resource::ResidencyAssetType::Texture
				&& asset.estimatedGpuBytes > 0u)
			{
				residentTextureIds.push_back(id);
			}
		}
	}
	{
		std::shared_lock lock(m_StreamingMutex);
		for (const auto& [id, record] : m_StreamableAssets)
		{
			if (record.assetType == TerraEngine::Resource::StreamableAssetType::Texture
				&& (record.estimatedGpuBytes > 0u || record.textureResidentMipCount > 0u)
				&& std::find(residentTextureIds.begin(), residentTextureIds.end(), id) == residentTextureIds.end())
			{
				residentTextureIds.push_back(id);
			}
		}
	}

	for (const TerraEngine::UUID& textureId : residentTextureIds)
	{
		// Texture mip policy affects the physical VkImage dimensions and mip
		// count. Existing images cannot be retargeted in place, so evict only
		// the GPU copy and let the next visible use upload the policy-selected
		// mip tail from the still-resident CPU texture data.
		RenderSystem::Instance().RetireLoadedResource(textureId, 0u, "texture-mip-policy-changed");
	}

	{
		std::unique_lock lock(m_ResidencyMutex);
		for (const TerraEngine::UUID& textureId : residentTextureIds)
		{
			if (auto it = m_ResidentAssets.find(textureId); it != m_ResidentAssets.end())
			{
				it->second.estimatedGpuBytes = 0u;
				it->second.gpuAllocations.clear();
				it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
				if (it->second.state != TerraEngine::Resource::ResidencyState::Pinned)
				{
					it->second.state = it->second.estimatedCpuBytes > 0u
						? TerraEngine::Resource::ResidencyState::Idle
						: TerraEngine::Resource::ResidencyState::Released;
				}
				it->second.lastTouchedReason = "texture mip policy changed";
				it->second.releaseGeneration += 1u;
			}
		}
	}

	{
		std::unique_lock lock(m_StreamingMutex);
		for (const TerraEngine::UUID& textureId : residentTextureIds)
		{
			if (auto it = m_StreamableAssets.find(textureId); it != m_StreamableAssets.end())
			{
				it->second.payloadState = it->second.estimatedCpuBytes > 0u
					? TerraEngine::Resource::StreamablePayloadState::RamResident
					: TerraEngine::Resource::StreamablePayloadState::Released;
				it->second.estimatedGpuBytes = 0u;
				it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
				if (it->second.estimatedCpuBytes == 0u)
				{
					it->second.exactCpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
				}
				it->second.textureResidentMipCount = 0u;
				it->second.textureSourceMipCount = 0u;
				it->second.textureResidentMipOffset = 0u;
				it->second.residencyGeneration += 1u;
			}
		}
	}
}

void ResourceManager::MarkTextureStreamingDistance(
	const TerraEngine::UUID& id,
	double viewDistance,
	std::uint64_t frameId,
	std::string_view reason)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return;
	}

	const double nowSeconds = ResidencyClockSeconds();
	const double safeDistance = std::max(0.0, viewDistance);
	bool mipResidencyChanged = false;
	{
		std::unique_lock lock(m_StreamingMutex);
		TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
		record.assetId = id;
		record.assetType = TerraEngine::Resource::StreamableAssetType::Texture;
		if (!record.assetPolicyExplicit)
		{
			record.assetPolicy = TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(record.assetType);
		}
		// Multiple active views can see the same texture. Keep the nearest
		// distance for the current diagnostics/update window because it needs
		// the highest resident mip quality.
		if (record.textureDistanceUpdatedSeconds <= 0.0
			|| nowSeconds - record.textureDistanceUpdatedSeconds > 0.25
			|| safeDistance < record.textureClosestViewDistance)
		{
			record.textureClosestViewDistance = safeDistance;
			record.textureDistanceUpdatedSeconds = nowSeconds;
		}
		if (record.textureSourceMipCount > 0u && record.textureResidentMipCount > 0u)
		{
			TerraEngine::Resource::TextureMipResidency desired =
				TerraEngine::Resource::ResolveTextureMipResidency(m_StreamingPolicy, record.textureSourceMipCount);
			if (m_StreamingPolicy.textureMipStreamingEnabled
				&& m_StreamingPolicy.textureMaxStreamMip > 0u
				&& record.textureSourceMipCount > 0u)
			{
				const std::uint32_t maxSourceMip = record.textureSourceMipCount - 1u;
				const std::uint32_t minStreamMip = std::min(m_StreamingPolicy.textureMinStreamMip, maxSourceMip);
				const std::uint32_t maxStreamMip = std::clamp(m_StreamingPolicy.textureMaxStreamMip, minStreamMip, maxSourceMip);
				if (maxStreamMip > minStreamMip)
				{
					const double fadeDistance = std::max(1.0, m_StreamingPolicy.streamingDistance);
					const double normalized = std::clamp(record.textureClosestViewDistance / fadeDistance, 0.0, 1.0);
					const double mipSpan = static_cast<double>(maxStreamMip - minStreamMip);
					desired.sourceMipOffset = minStreamMip
						+ static_cast<std::uint32_t>(std::round(normalized * mipSpan));
					desired.sourceMipOffset = std::clamp(desired.sourceMipOffset, minStreamMip, maxStreamMip);
					desired.residentMipCount = record.textureSourceMipCount - desired.sourceMipOffset;
					desired.partial = desired.sourceMipOffset > 0u;
				}
			}
			mipResidencyChanged =
				desired.residentMipCount != record.textureResidentMipCount
				|| desired.sourceMipOffset != record.textureResidentMipOffset;
		}
	}

	TouchResource(id, AssetType::Texture, frameId, TerraEngine::Resource::ResidencyPinReason::None, reason);
	if (mipResidencyChanged)
	{
		RenderSystem::Instance().RetireLoadedResource(id, frameId, "texture-mip-distance-changed");
		{
			std::unique_lock lock(m_ResidencyMutex);
			if (auto it = m_ResidentAssets.find(id); it != m_ResidentAssets.end())
			{
				it->second.estimatedGpuBytes = 0u;
				it->second.gpuAllocations.clear();
				it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
				if (it->second.state != TerraEngine::Resource::ResidencyState::Pinned)
				{
					it->second.state = it->second.estimatedCpuBytes > 0u
						? TerraEngine::Resource::ResidencyState::Idle
						: TerraEngine::Resource::ResidencyState::Released;
				}
				it->second.lastTouchedReason = "texture mip distance changed";
				it->second.releaseGeneration += 1u;
			}
		}
		std::unique_lock lock(m_StreamingMutex);
		if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
		{
			it->second.payloadState = it->second.estimatedCpuBytes > 0u
				? TerraEngine::Resource::StreamablePayloadState::RamResident
				: TerraEngine::Resource::StreamablePayloadState::Released;
			it->second.estimatedGpuBytes = 0u;
			it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			if (it->second.estimatedCpuBytes == 0u)
			{
				it->second.exactCpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			}
			it->second.textureResidentMipCount = 0u;
			it->second.textureSourceMipCount = 0u;
			it->second.textureResidentMipOffset = 0u;
			it->second.residencyGeneration += 1u;
		}
	}
}

TerraEngine::Resource::TextureMipResidency ResourceManager::ResolveTextureMipResidencyForTexture(
	const TerraEngine::UUID& id,
	std::uint32_t sourceMipCount) const
{
	TerraEngine::Resource::StreamingPolicy policy;
	double closestDistance = 0.0;
	bool hasRecentDistance = false;
	const double nowSeconds = ResidencyClockSeconds();
	{
		std::shared_lock lock(m_StreamingMutex);
		policy = m_StreamingPolicy;
		if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
		{
			closestDistance = it->second.textureClosestViewDistance;
			hasRecentDistance = (nowSeconds - it->second.textureDistanceUpdatedSeconds) <= std::max(0.25, m_StreamingPolicy.visibleRetentionSeconds);
		}
	}

	TerraEngine::Resource::TextureMipResidency residency = TerraEngine::Resource::ResolveTextureMipResidency(policy, sourceMipCount);
	if (!policy.textureMipStreamingEnabled
		|| sourceMipCount == 0u
		|| policy.textureMaxStreamMip == 0u
		|| !hasRecentDistance)
	{
		return residency;
	}

	const std::uint32_t maxSourceMip = sourceMipCount - 1u;
	const std::uint32_t minStreamMip = std::min(policy.textureMinStreamMip, maxSourceMip);
	const std::uint32_t maxStreamMip = std::clamp(policy.textureMaxStreamMip, minStreamMip, maxSourceMip);
	if (maxStreamMip <= minStreamMip)
	{
		return residency;
	}

	// Distance mip residency deliberately uses the same streaming distance knob
	// as visibility streaming. Near assets start at the configured minimum
	// source mip index, and assets near or beyond the streaming edge start at
	// the configured maximum source mip index.
	const double fadeDistance = std::max(1.0, policy.streamingDistance);
	const double normalized = std::clamp(closestDistance / fadeDistance, 0.0, 1.0);
	const double mipSpan = static_cast<double>(maxStreamMip - minStreamMip);
	residency.sourceMipOffset = minStreamMip
		+ static_cast<std::uint32_t>(std::round(normalized * mipSpan));
	residency.sourceMipOffset = std::clamp(residency.sourceMipOffset, minStreamMip, maxStreamMip);
	residency.residentMipCount = sourceMipCount - residency.sourceMipOffset;
	residency.partial = residency.sourceMipOffset > 0u;
	return residency;
}

TerraEngine::Resource::StreamingPolicy ResourceManager::GetStreamingPolicy() const
{
	std::shared_lock lock(m_StreamingMutex);
	return m_StreamingPolicy;
}

void ResourceManager::SetStreamingDiagnosticsEnabled(bool enabled)
{
	std::unique_lock lock(m_StreamingMutex);
	m_StreamingPolicy.diagnosticsEnabled = enabled;
	m_StreamingDiagnostics.diagnosticsEnabled = enabled;
}

TerraEngine::Resource::StreamingDiagnosticsSummary ResourceManager::GetStreamingDiagnosticsSummary() const
{
	std::shared_lock lock(m_StreamingMutex);
	// The Resource Residency window can be drawn after the renderer uploads a
	// mesh but before the next residency cleanup publishes a cached summary.
	// Build the snapshot from the live streamable records so diagnostics report
	// the current RAM/VRAM byte counts instead of a stale zero-byte cleanup view.
	return BuildStreamingDiagnosticsLocked(m_StreamingDiagnostics.frameId);
}

TerraEngine::Resource::StreamableAssetRecord ResourceManager::RegisterStreamableAsset(
	const TerraEngine::UUID& id,
	AssetType type,
	const std::filesystem::path& path,
	bool referencedByScene)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	const double nowSeconds = ResidencyClockSeconds();
	std::unique_lock lock(m_StreamingMutex);
	TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
	record.assetId = id;
	record.assetType = ToStreamableAssetType(type);
	if (!record.assetPolicyExplicit)
	{
		record.assetPolicy = TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(record.assetType);
	}
	record.sourceKey = key;
	record.lastReferencedSeconds = nowSeconds;
	if (record.payloadState == TerraEngine::Resource::StreamablePayloadState::Registered
		|| record.payloadState == TerraEngine::Resource::StreamablePayloadState::Released)
	{
		record.payloadState = TerraEngine::Resource::StateAfterRegistration(referencedByScene && m_StreamingPolicy.enabled && m_StreamingPolicy.deferScenePayloadsOnLoad);
	}
	return record;
}

std::optional<TerraEngine::Resource::StreamableAssetRecord> ResourceManager::GetStreamableAsset(const TerraEngine::UUID& id) const
{
	std::shared_lock lock(m_StreamingMutex);
	if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
	{
		return it->second;
	}
	return std::nullopt;
}

TerraEngine::Resource::StreamingAssetPolicy ResourceManager::ResolveStreamingAssetPolicy(const TerraEngine::UUID& id, AssetType type) const
{
	std::shared_lock lock(m_StreamingMutex);
	if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
	{
		return TerraEngine::Resource::SanitizeStreamingAssetPolicy(it->second.assetPolicy);
	}
	return TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(ToStreamableAssetType(type));
}

void ResourceManager::SetStreamableAssetPolicy(
	const TerraEngine::UUID& id,
	AssetType type,
	const TerraEngine::Resource::StreamingAssetPolicy& policy)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return;
	}

	const TerraEngine::Resource::StreamingAssetPolicy sanitized = TerraEngine::Resource::SanitizeStreamingAssetPolicy(policy);
	{
		std::unique_lock lock(m_StreamingMutex);
		TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
		record.assetId = id;
		record.assetType = ToStreamableAssetType(type);
		record.assetPolicy = sanitized;
		record.assetPolicyExplicit = true;
	}
	{
		std::unique_lock lock(m_ResidencyMutex);
		if (auto it = m_ResidentAssets.find(id); it != m_ResidentAssets.end())
		{
			it->second.priorityClass = sanitized.priorityClass;
			it->second.residencyTier = sanitized.residencyTier;
		}
	}
}

TerraEngine::Resource::StreamingAssetPolicy ResourceManager::GetStreamableAssetPolicy(const TerraEngine::UUID& id, AssetType type) const
{
	return ResolveStreamingAssetPolicy(id, type);
}

void ResourceManager::MarkStreamingNeed(
	const TerraEngine::UUID& id,
	AssetType type,
	TerraEngine::Resource::StreamingNeedType needType,
	std::uint64_t frameId,
	std::string_view reason,
	std::int32_t priority)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return;
	}

	const double nowSeconds = ResidencyClockSeconds();
	TerraEngine::Resource::StreamingProtectionReason protection = TerraEngine::Resource::ProtectionForNeed(needType);
	std::unique_lock lock(m_StreamingMutex);
	TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
	record.assetId = id;
	record.assetType = ToStreamableAssetType(type);
	if (!record.assetPolicyExplicit)
	{
		record.assetPolicy = TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(record.assetType);
	}
	record.lastNeededSeconds = nowSeconds;
	if (needType == TerraEngine::Resource::StreamingNeedType::Visible)
	{
		record.lastVisibleSeconds = nowSeconds;
	}
	if (record.payloadState == TerraEngine::Resource::StreamablePayloadState::Registered)
	{
		record.payloadState = TerraEngine::Resource::StreamablePayloadState::Deferred;
	}
	TerraEngine::Resource::StreamingNeed need;
	need.needId = std::string(TerraEngine::Resource::ToString(needType));
	need.assetId = id;
	need.needType = needType;
	need.priority = priority
		+ TerraEngine::Resource::PriorityClassBias(record.assetPolicy.priorityClass)
		+ record.assetPolicy.priorityBias;
	need.reason = reason.empty() ? std::string(TerraEngine::Resource::ToString(needType)) : std::string(reason);
	if (needType == TerraEngine::Resource::StreamingNeedType::Visible)
	{
		need.expiresAtSeconds = nowSeconds + m_StreamingPolicy.visibleRetentionSeconds;
	}
	else if (needType != TerraEngine::Resource::StreamingNeedType::FallbackDefault
		&& needType != TerraEngine::Resource::StreamingNeedType::BackendInFlight)
	{
		// Editor, UI, prefab, preload, and explicit gameplay marks are
		// republished while active. Giving them a retention window prevents a
		// one-frame selection or preview from pinning an asset forever.
		need.expiresAtSeconds = nowSeconds + m_StreamingPolicy.neededRetentionSeconds;
	}
	record.needSources.push_back(std::move(need));
	record.needSources = TerraEngine::Resource::MergeStreamingNeeds(std::move(record.needSources), nowSeconds);
	record.protectionReason = ComputeStreamingProtection(record.needSources, m_StreamingPolicy);
	lock.unlock();

	// Streaming protection is mirrored into loaded residency so existing cleanup
	// and diagnostics remain the single place that actually release RAM/VRAM.
	TerraEngine::Resource::ResidencyPinReason residencyReason = TerraEngine::Resource::ResidencyPinReason::None;
	switch (protection)
	{
	case TerraEngine::Resource::StreamingProtectionReason::EditorSelection:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::EditorSelection;
		break;
	case TerraEngine::Resource::StreamingProtectionReason::PrefabEditing:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::PrefabEditing;
		break;
	case TerraEngine::Resource::StreamingProtectionReason::Preview:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::Preview;
		break;
	case TerraEngine::Resource::StreamingProtectionReason::FallbackResource:
	case TerraEngine::Resource::StreamingProtectionReason::DefaultResource:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::FallbackResource;
		break;
	case TerraEngine::Resource::StreamingProtectionReason::InFlightLoad:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::Loading;
		break;
	case TerraEngine::Resource::StreamingProtectionReason::BackendInFlight:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::BackendInFlight;
		break;
	default:
		residencyReason = TerraEngine::Resource::ResidencyPinReason::None;
		break;
	}
	TouchResource(id, type, frameId, residencyReason, reason);
}

void ResourceManager::PublishVisibleStreamingTouches(
	const TerraEngine::Renderer::VisibleStreamingTouchSet& touchSet,
	std::uint64_t frameId,
	std::string_view reason)
{
	if (!touchSet.valid)
	{
		return;
	}

	const std::string detail = reason.empty()
		? std::string("visible command cache touch set")
		: std::string(reason);
	const auto publishResource = [&](const TerraEngine::Renderer::VisibleResourceTouch& touch) {
		if (touch.resourceId == TerraEngine::EMPTY_UUID)
		{
			return;
		}
		const AssetType type = touch.kind == ::VisibleStreamingResourceKind::Material
			? AssetType::Material
			: AssetType::Mesh;
		// The touch set has already removed duplicate draw-command references.
		// Reusing the existing single-resource path here preserves retention,
		// priority bias, residency mirroring, and diagnostics semantics.
		MarkStreamingNeed(
			touch.resourceId,
			type,
			TerraEngine::Resource::StreamingNeedType::Visible,
			frameId,
			detail,
			touch.priority);
	};

	for (const TerraEngine::Renderer::VisibleResourceTouch& touch : touchSet.meshes)
	{
		publishResource(touch);
	}
	for (const TerraEngine::Renderer::VisibleResourceTouch& touch : touchSet.materials)
	{
		publishResource(touch);
	}
	for (const TerraEngine::Renderer::VisibleTextureTouch& touch : touchSet.textures)
	{
		if (touch.textureId == TerraEngine::EMPTY_UUID)
		{
			continue;
		}
		MarkStreamingNeed(
			touch.textureId,
			AssetType::Texture,
			TerraEngine::Resource::StreamingNeedType::Visible,
			frameId,
			detail,
			touch.priority);
		MarkTextureStreamingDistance(touch.textureId, touch.nearestViewDistance, frameId, detail);
	}
}

std::shared_future<TerraEngine::UUID> ResourceManager::RequestStreamableLoad(
	const TerraEngine::UUID& id,
	AssetType type,
	TerraEngine::Resource::StreamingNeedType needType,
	std::uint64_t frameId,
	std::string_view reason)
{
	MarkStreamingNeed(id, type, needType, frameId, reason, needType == TerraEngine::Resource::StreamingNeedType::Visible ? 100 : 10);

	{
		std::unique_lock lock(m_StreamingMutex);
		if (m_StreamingPolicy.maxLoadsPerUpdate > 0u)
		{
			if (m_StreamingLoadBudgetFrame != frameId)
			{
				m_StreamingLoadBudgetFrame = frameId;
				m_StreamingLoadsIssuedThisFrame = 0u;
			}
			if (m_StreamingLoadsIssuedThisFrame >= m_StreamingPolicy.maxLoadsPerUpdate)
			{
				// The need remains recorded above, so callers that republish
				// visibility each frame will naturally retry once the next frame
				// opens load budget. Returning EMPTY_UUID keeps this call
				// non-blocking instead of forcing a hidden queue here.
				return MakeReadyFuture(TerraEngine::EMPTY_UUID);
			}
			++m_StreamingLoadsIssuedThisFrame;
		}
	}

	std::filesystem::path sourcePath;
	{
		std::shared_lock lock(m_StreamingMutex);
		if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
		{
			sourcePath = it->second.sourceKey;
		}
	}
	if (sourcePath.empty())
	{
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	switch (type)
	{
	case AssetType::Mesh:
		return LoadMeshAsync(sourcePath);
	case AssetType::Texture:
		return LoadTextureAsync(sourcePath);
	case AssetType::Material:
		return LoadMaterialAsync(sourcePath);
	case AssetType::ShaderCollection:
		return MakeReadyFuture(id);
	}
	return MakeReadyFuture(TerraEngine::EMPTY_UUID);
}

void ResourceManager::UpdateStreamableLoadResult(
	const TerraEngine::UUID& id,
	AssetType type,
	const std::string& sourceKey,
	bool success,
	std::uint64_t cpuBytes,
	std::uint64_t gpuBytes,
	std::string_view failureReason)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return;
	}
	std::unique_lock lock(m_StreamingMutex);
	TerraEngine::Resource::StreamableAssetRecord& record = m_StreamableAssets[id];
	record.assetId = id;
	record.assetType = ToStreamableAssetType(type);
	if (!record.assetPolicyExplicit)
	{
		record.assetPolicy = TerraEngine::Resource::MakeDefaultStreamingAssetPolicy(record.assetType);
	}
	if (!sourceKey.empty())
	{
		record.sourceKey = sourceKey;
	}
	if (!success)
	{
		record.payloadState = TerraEngine::Resource::StreamablePayloadState::Failed;
	}
	else if (gpuBytes > 0u)
	{
		record.payloadState = TerraEngine::Resource::StreamablePayloadState::VramResident;
	}
	else
	{
		record.payloadState = TerraEngine::Resource::StreamablePayloadState::RamResident;
	}
	record.estimatedCpuBytes = success ? cpuBytes : 0u;
	record.estimatedGpuBytes = success ? gpuBytes : 0u;
	record.exactCpuBytes = success
		? (cpuBytes > 0u ? TerraEngine::Resource::MakeExactResidencyBytes(cpuBytes) : TerraEngine::Resource::MakeZeroByStateResidencyBytes())
		: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
	record.exactGpuBytes = success
		? (gpuBytes > 0u ? TerraEngine::Resource::MakeExactResidencyBytes(gpuBytes) : TerraEngine::Resource::MakeZeroByStateResidencyBytes())
		: TerraEngine::Resource::MakeZeroByStateResidencyBytes();
	if (!success || gpuBytes == 0u)
	{
		record.textureResidentMipCount = 0u;
		record.textureSourceMipCount = 0u;
		record.textureResidentMipOffset = 0u;
	}
	record.failureReason = success ? std::string{} : std::string(failureReason);
	record.lastNeededSeconds = ResidencyClockSeconds();
	record.residencyGeneration += success ? 1u : 0u;
}

void ResourceManager::MarkStreamableReleased(const TerraEngine::UUID& id, std::uint64_t frameId)
{
	std::unique_lock lock(m_StreamingMutex);
	if (auto it = m_StreamableAssets.find(id); it != m_StreamableAssets.end())
	{
		it->second.payloadState = TerraEngine::Resource::StreamablePayloadState::Released;
		it->second.lastReleaseEligibleSeconds = ResidencyClockSeconds();
		it->second.needSources.clear();
		it->second.protectionReason = TerraEngine::Resource::StreamingProtectionReason::None;
		it->second.estimatedCpuBytes = 0u;
		it->second.estimatedGpuBytes = 0u;
		it->second.exactCpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
		it->second.exactGpuBytes = TerraEngine::Resource::MakeZeroByStateResidencyBytes();
		it->second.textureResidentMipCount = 0u;
		it->second.textureSourceMipCount = 0u;
		it->second.textureResidentMipOffset = 0u;
		it->second.textureClosestViewDistance = 0.0;
		it->second.textureDistanceUpdatedSeconds = 0.0;
		it->second.failureReason.clear();
		it->second.residencyGeneration = frameId;
	}
}

TerraEngine::Resource::StreamingDiagnosticsSummary ResourceManager::BuildStreamingDiagnosticsLocked(std::uint64_t frameId) const
{
	TerraEngine::Resource::StreamingDiagnosticsSummary summary;
	summary.frameId = frameId;
	summary.diagnosticsEnabled = m_StreamingPolicy.diagnosticsEnabled;
	summary.trackedAssetCount = m_StreamableAssets.size();
	for (const auto& [id, record] : m_StreamableAssets)
	{
		switch (record.payloadState)
		{
		case TerraEngine::Resource::StreamablePayloadState::Deferred:
		case TerraEngine::Resource::StreamablePayloadState::Released:
		case TerraEngine::Resource::StreamablePayloadState::LoadingRam:
			summary.deferredAssetCount++;
			break;
		case TerraEngine::Resource::StreamablePayloadState::UploadingVram:
			summary.loadingAssetCount++;
			break;
		case TerraEngine::Resource::StreamablePayloadState::RamResident:
		case TerraEngine::Resource::StreamablePayloadState::VramResident:
			summary.residentAssetCount++;
			break;
		case TerraEngine::Resource::StreamablePayloadState::PendingRelease:
			summary.releaseEligibleCount++;
			break;
		case TerraEngine::Resource::StreamablePayloadState::Failed:
			summary.failedAssetCount++;
			break;
		case TerraEngine::Resource::StreamablePayloadState::Registered:
			break;
		}
		if (record.protectionReason != TerraEngine::Resource::StreamingProtectionReason::None)
		{
			summary.protectedAssetCount++;
		}
		summary.activeNeedCount += record.needSources.size();
		const bool ownsResidentMemory = HasResidentStreamingMemory(record.payloadState);
		const std::uint64_t diagnosticCpuBytes = ownsResidentMemory ? record.estimatedCpuBytes : 0u;
		const std::uint64_t diagnosticGpuBytes = ownsResidentMemory ? record.estimatedGpuBytes : 0u;
		summary.estimatedResidentBytes += diagnosticCpuBytes + diagnosticGpuBytes;
		if (m_StreamingPolicy.diagnosticsEnabled)
		{
			TerraEngine::Resource::StreamingDiagnosticRecord diagnostic;
			diagnostic.assetId = id;
			diagnostic.assetType = record.assetType;
			diagnostic.sourceKey = record.sourceKey;
			diagnostic.payloadState = record.payloadState;
			diagnostic.lastVisibleSeconds = record.lastVisibleSeconds;
			diagnostic.lastNeededSeconds = record.lastNeededSeconds;
			diagnostic.needSources = record.needSources;
			diagnostic.protectionReason = record.protectionReason;
			diagnostic.assetPolicy = record.assetPolicy;
			diagnostic.textureResidentMipCount = record.textureResidentMipCount;
			diagnostic.textureSourceMipCount = record.textureSourceMipCount;
			diagnostic.textureResidentMipOffset = record.textureResidentMipOffset;
			diagnostic.textureClosestViewDistance = record.textureClosestViewDistance;
			diagnostic.releaseEligibility = record.needSources.empty() && record.protectionReason == TerraEngine::Resource::StreamingProtectionReason::None ? "eligible-or-waiting-policy" : "protected";
			diagnostic.failureReason = record.failureReason;
			diagnostic.estimatedCpuBytes = diagnosticCpuBytes;
			diagnostic.estimatedGpuBytes = diagnosticGpuBytes;
			diagnostic.exactCpuBytes = ownsResidentMemory ? record.exactCpuBytes : TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			diagnostic.exactGpuBytes = ownsResidentMemory ? record.exactGpuBytes : TerraEngine::Resource::MakeZeroByStateResidencyBytes();
			summary.records.push_back(std::move(diagnostic));
		}
	}
	return summary;
}

void ResourceManager::ClearStreamingState()
{
	std::unique_lock lock(m_StreamingMutex);
	m_StreamableAssets.clear();
	m_StreamingDiagnostics = {};
	m_StreamingDiagnostics.diagnosticsEnabled = m_StreamingPolicy.diagnosticsEnabled;
}

// ResourceManager::HasResource is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ResourceManager::HasResource(const TerraEngine::UUID& id) const
{
	auto isAvailable = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		return cache.resources.find(id) != cache.resources.end() || cache.idToPath.find(id) != cache.idToPath.end();
	};

	return isAvailable(meshCache) || isAvailable(textureCache) || isAvailable(materialCache) ||
		isAvailable(shaderCollectionCache) || isAvailable(shaderCache);
}

// ResourceManager::RegisterAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	const bool canBePreferredLoadPath = HasNativeExtensionForAssetType(resolvedPath, type);

	VisitAssetCache(type, [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		cache.pathToId[key] = id;
		// Source assets such as .obj are useful aliases for resolving authored
		// references, but render-time resource loads must use compiled artifacts
		// such as .temf. Keeping idToPath limited to native formats prevents the
		// renderer from parsing a source file while its compiler job is still
		// producing the real runtime payload.
		if (canBePreferredLoadPath)
		{
			cache.idToPath[id] = key;
		}
	});
	RegisterStreamableAsset(id, type, path, true);
}

// ResourceManager::ResolveAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
TerraEngine::UUID ResourceManager::ResolveAssetReference(std::string_view assetReference, AssetType type)
{
	if (assetReference.empty())
	{
		return TerraEngine::EMPTY_UUID;
	}

	const auto buildCandidatePaths = [&]() -> std::vector<std::filesystem::path> {
		std::vector<std::filesystem::path> candidatePaths;
		const std::filesystem::path queryPath(assetReference);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (AssetReferenceLooksLikePath(assetReference))
		{
			// Explicit paths are resolved literally relative to the asset root,
			// then retried with native runtime extensions. This lets authored
			// scene data refer to source assets like Models/cube.obj while the
			// renderer actually loads Models/cube.temf.
			candidatePaths.push_back(queryPath);
			if (queryPath.has_extension())
			{
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (std::string_view extension : SupportedExtensionsForAssetType(type))
				{
					if (queryPath.extension() != extension)
					{
						std::filesystem::path candidate = queryPath;
						candidate.replace_extension(extension);
						candidatePaths.push_back(std::move(candidate));
					}
				}
			}
			return candidatePaths;
		}

		const std::filesystem::path topLevelName = queryPath.filename();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (queryPath.has_extension())
		{
			// A bare filename like "Player.temf" is assumed to live directly
			// under the asset root, not somewhere deeper in the tree.
			candidatePaths.push_back(topLevelName);

			// Also try substituting with native extensions for this asset type
			// if the provided extension is different. This handles cases where
			// a document refers to a source asset (e.g. "viking_room.png") but
			// the engine needs the compiled version ("viking_room.tetf").
			for (std::string_view extension : SupportedExtensionsForAssetType(type))
			{
				if (topLevelName.extension() != extension)
				{
					std::filesystem::path candidate = topLevelName;
					candidate.replace_extension(extension);
					candidatePaths.push_back(std::move(candidate));
				}
			}

			return candidatePaths;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::string_view extension : SupportedExtensionsForAssetType(type))
		{
			std::filesystem::path candidate = topLevelName;
			candidate += extension;
			candidatePaths.push_back(std::move(candidate));
		}

		return candidatePaths;
	};

	const std::vector<std::filesystem::path> candidatePaths = buildCandidatePaths();

	const auto tryFindRegisteredMatch = [&](auto& cache) -> TerraEngine::UUID {
		std::shared_lock lock(cache.mutex);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::filesystem::path& candidatePath : candidatePaths)
		{
			const std::string exactKey = NormalizeKey(ResolveAssetPath(candidatePath));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto exactMatch = cache.pathToId.find(exactKey); exactMatch != cache.pathToId.end())
			{
				return exactMatch->second;
			}
		}

		return TerraEngine::EMPTY_UUID;
	};

	const auto tryLoadExplicitPath = [&](const std::filesystem::path& candidatePath) -> TerraEngine::UUID {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!HasNativeExtensionForAssetType(candidatePath, type))
		{
			return TerraEngine::EMPTY_UUID;
		}

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (type)
		{
		case AssetType::Mesh:
			return LoadMesh(candidatePath);
		case AssetType::Texture:
			return LoadTexture(candidatePath);
		case AssetType::Material:
			return LoadMaterial(candidatePath);
		case AssetType::ShaderCollection:
			return TerraEngine::EMPTY_UUID;
		}

		return TerraEngine::EMPTY_UUID;
	};

	TerraEngine::UUID resolvedId = TerraEngine::EMPTY_UUID;
	VisitAssetCache(type, [&](auto& cache) {
		resolvedId = tryFindRegisteredMatch(cache);
	});
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (resolvedId != TerraEngine::EMPTY_UUID)
	{
		return resolvedId;
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::path& candidatePath : candidatePaths)
	{
		resolvedId = tryLoadExplicitPath(candidatePath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resolvedId != TerraEngine::EMPTY_UUID)
		{
			return resolvedId;
		}
	}

	return TerraEngine::EMPTY_UUID;
}

// ResourceManager::DescribeAssetReference is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ResourceManager::DescribeAssetReference(const TerraEngine::UUID& id, AssetType type) const
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return {};
	}

	std::filesystem::path result;
	const auto capturePath = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto pathIt = cache.idToPath.find(id); pathIt != cache.idToPath.end())
		{
			result = std::filesystem::path(pathIt->second);
		}
	};

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (type)
	{
	case AssetType::Mesh:
		capturePath(meshCache);
		break;
	case AssetType::Texture:
		capturePath(textureCache);
		break;
	case AssetType::Material:
		capturePath(materialCache);
		break;
	case AssetType::ShaderCollection:
		capturePath(shaderCollectionCache);
		break;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (result.is_absolute())
	{
		const std::filesystem::path relativePath = result.lexically_relative(assetRoot);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!relativePath.empty())
		{
			return relativePath;
		}
	}

	return result;
}

// ResourceManager::GetMaterial is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Material* ResourceManager::GetMaterial(const TerraEngine::UUID& id)
{
	return GetResourceById<Material>(id, materialCache, [this](const std::filesystem::path& p) { return LoadMaterial(p); });
}

// ResourceManager::GetMesh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Mesh* ResourceManager::GetMesh(const TerraEngine::UUID& id)
{
	return GetResourceById<Mesh>(id, meshCache, [this](const std::filesystem::path& p) { return LoadMesh(p); });
}

// ResourceManager::GetShader is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Shader* ResourceManager::GetShader(const TerraEngine::UUID& id)
{
	return GetResourceById<Shader>(id, shaderCache, [this](const std::filesystem::path& p) { return LoadShader(p); });
}

// ResourceManager::GetShaderCollection is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ShaderCollection* ResourceManager::GetShaderCollection(const TerraEngine::UUID& id)
{
	return GetResourceById<ShaderCollection>(id, shaderCollectionCache);
}

// ResourceManager::GetTexture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
Texture* ResourceManager::GetTexture(const TerraEngine::UUID& id)
{
	return GetResourceById<Texture>(id, textureCache, [this](const std::filesystem::path& p) { return LoadTexture(p); });
}

// ResourceManager::ResolveAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ResourceManager::ResolveAssetPath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	return (assetRoot / path).lexically_normal();
}

// ResourceManager::ResolveEngineAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ResourceManager::ResolveEngineAssetPath(const std::filesystem::path& path) const
{
	if (path.is_absolute())
	{
		return path.lexically_normal();
	}

	// Editor sessions keep engine-owned payloads such as shaders and fonts in
	// the installed engine tree. Export sessions set engineRoot to the runtime
	// folder, so the same absolute path maps back to TEPAK through ReadFileBytes.
	return (engineRoot / path).lexically_normal();
}

// ResourceManager::NormalizeKey is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ResourceManager::NormalizeKey(const std::filesystem::path& path) const
{
	// Use generic_string to avoid mismatches between Windows backslashes and
	// POSIX forward slashes when keys are compared across platforms.
	return path.lexically_normal().generic_string();
}

// ResourceManager::ReadFileBytes is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<std::uint8_t> ResourceManager::ReadFileBytes(const std::filesystem::path& path) const
{
	const std::filesystem::path absolutePath = ResolveAssetPath(path);

	// In export builds, the ContentMount provides access to assets packed into 
	// a .tepak archive. If a mount is active, we attempt to read through it 
	// using a logical path relative to the asset root.
	if (ContentMount::Instance().IsMounted())
	{
		const std::filesystem::path logicalPath = absolutePath.lexically_relative(assetRoot);
		const std::string logicalPathString = ContentMount::NormalizeLogicalPath(logicalPath.generic_string());

		TERRAENGINE_LOG(LogLevel::Debug, "Reading resource file from mount: " << logicalPathString);
		const std::vector<std::uint8_t> mountedBytes = ContentMount::Instance().ReadAll(logicalPathString);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!mountedBytes.empty())
		{
			return mountedBytes;
		}

		// Mounted zero-byte files are valid authored assets. TEPAK returns an
		// empty buffer for them, so probe existence separately before treating
		// the empty read as a cache miss.
		if (ContentMount::Instance().Exists(logicalPathString))
		{
			TERRAENGINE_LOG(LogLevel::Debug, "Mounted resource file is empty: " << logicalPathString);
			return {};
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Resource not found in mount: " << logicalPathString);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(absolutePath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Resource file does not exist on disk: " << absolutePath);
		return {};
	}

	std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!file)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open resource file: " << absolutePath);
		return {};
	}

	const std::streamsize length = file.tellg();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (length <= 0)
	{
		// Empty authored files are valid for assets such as placeholder UI
		// stylesheets, so return an empty payload without escalating to an
		// error. Callers that require non-empty content must validate it.
		TERRAENGINE_LOG(LogLevel::Debug, "Resource file is empty: " << absolutePath);
		return {};
	}

	// Emit a debug log with the resolved size so callers can correlate reads with disk usage.
	TERRAENGINE_LOG(LogLevel::Debug, "Reading resource file " << absolutePath << " (" << length << " bytes)");

	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length));

	// Sequential read is the simplest and most cache-friendly path because
	// each resource file is authored as a single contiguous blob.
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(buffer.data()), length))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to read resource file: " << absolutePath);
		return {};
	}

	return buffer;
}
