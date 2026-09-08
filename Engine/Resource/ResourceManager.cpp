// ResourceManager.cpp — Resource Manager System Implementation
//
// Implements the core logic for the ResourceManager, managing asset directories and resource registries.
// It coordinates the loading of different asset formats and integrates with the renderer to synchronize GPU resources.
// This module handles project-level asset resolution and serves as the primary gateway for content consumption.

#include "Resource/ResourceManager.h"
#include "Renderer/RenderSystem.h"

#include <cstring>
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
#include <mutex>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "Core/Logger/Logger.h"
#include "Animation/Assets/AnimationClipAsset.h"
#include "Audio/Assets/AudioClipAsset.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Assets/AnimationMaskAsset.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "File Formats/TerraEngineAnimationFile.h"
#include "Runtime/ContentMount/AnimationContentResolver.h"
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

// The prepared transaction stores type-erased pointers because skeletons,
// clips, and the specialist byte-backed resources live in different caches.
// Each pointer still carries its exact no-throw deleter, and ResourceManager
// restores the concrete type only after every cache has passed commit checks.
struct PreparedAnimationAssetReloadBatch::State
{
	struct Entry
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		AssetType assetType = AssetType::Skeleton;
		std::string sourceKey;
		TerraEngine::Animation::AnimationGeneration observedGeneration = 0U;
		TerraEngine::Animation::AnimationGeneration nextGeneration = 0U;
		std::uint64_t publisherRetirementGeneration = 0U;
		std::uint64_t cpuBytes = 0U;
		void* payload = nullptr;
		void (*destroyPayload)(void*) noexcept = nullptr;
		// These non-owning destinations are populated only while every animation
		// cache lock is held during commit. They prevent any lookup or validation
		// from occurring after the first live pointer swap.
		void* destinationSlot = nullptr;
		TerraEngine::Animation::AnimationGeneration* generationSlot = nullptr;

		Entry() noexcept = default;
		~Entry()
		{
			Reset();
		}
		Entry(const Entry&) = delete;
		Entry& operator=(const Entry&) = delete;
		Entry(Entry&& other) noexcept
			: assetId(other.assetId),
			assetType(other.assetType),
			sourceKey(std::move(other.sourceKey)),
			observedGeneration(other.observedGeneration),
			nextGeneration(other.nextGeneration),
			publisherRetirementGeneration(other.publisherRetirementGeneration),
			cpuBytes(other.cpuBytes),
			payload(std::exchange(other.payload, nullptr)),
			destroyPayload(std::exchange(other.destroyPayload, nullptr)),
			destinationSlot(std::exchange(other.destinationSlot, nullptr)),
			generationSlot(std::exchange(other.generationSlot, nullptr))
		{
		}
		Entry& operator=(Entry&& other) noexcept
		{
			if(this == &other) return *this;
			Reset();
			assetId = other.assetId;
			assetType = other.assetType;
			sourceKey = std::move(other.sourceKey);
			observedGeneration = other.observedGeneration;
			nextGeneration = other.nextGeneration;
			publisherRetirementGeneration = other.publisherRetirementGeneration;
			cpuBytes = other.cpuBytes;
			payload = std::exchange(other.payload, nullptr);
			destroyPayload = std::exchange(other.destroyPayload, nullptr);
			destinationSlot = std::exchange(other.destinationSlot, nullptr);
			generationSlot = std::exchange(other.generationSlot, nullptr);
			return *this;
		}

		void Reset() noexcept
		{
			if(payload != nullptr && destroyPayload != nullptr) destroyPayload(payload);
			payload = nullptr;
			destroyPayload = nullptr;
		}

		[[nodiscard]] void* ReleasePayload() noexcept
		{
			destroyPayload = nullptr;
			return std::exchange(payload, nullptr);
		}
	};

	void UnlockPublicationLocks() noexcept
	{
		// The publisher calls this after its own no-throw table swap. Keeping all
		// locks until then prevents independent cache readers seeing a half commit.
		if(ragdollLock.owns_lock()) ragdollLock.unlock();
		if(motionDatabaseLock.owns_lock()) motionDatabaseLock.unlock();
		if(retargetProfileLock.owns_lock()) retargetProfileLock.unlock();
		if(animationMaskLock.owns_lock()) animationMaskLock.unlock();
		if(animationControllerLock.owns_lock()) animationControllerLock.unlock();
		if(animationClipLock.owns_lock()) animationClipLock.unlock();
		if(skeletonLock.owns_lock()) skeletonLock.unlock();
		if(retirementLock.owns_lock()) retirementLock.unlock();
	}

	[[nodiscard]] bool OwnsPublicationLocks() const noexcept
	{
		return skeletonLock.owns_lock() || animationClipLock.owns_lock()
			|| animationControllerLock.owns_lock() || animationMaskLock.owns_lock()
			|| retargetProfileLock.owns_lock() || motionDatabaseLock.owns_lock()
			|| ragdollLock.owns_lock();
	}

	ResourceManager* owner = nullptr;
	std::vector<Entry> entries;
	bool diagnosticsFinalized = false;
	std::unique_lock<std::shared_mutex> skeletonLock;
	std::unique_lock<std::shared_mutex> animationClipLock;
	std::unique_lock<std::shared_mutex> animationControllerLock;
	std::unique_lock<std::shared_mutex> animationMaskLock;
	std::unique_lock<std::shared_mutex> retargetProfileLock;
	std::unique_lock<std::shared_mutex> motionDatabaseLock;
	std::unique_lock<std::shared_mutex> ragdollLock;
	std::unique_lock<std::mutex> retirementLock;
};

PreparedAnimationAssetReloadBatch::PreparedAnimationAssetReloadBatch() noexcept = default;
PreparedAnimationAssetReloadBatch::~PreparedAnimationAssetReloadBatch() = default;
PreparedAnimationAssetReloadBatch::PreparedAnimationAssetReloadBatch(
	PreparedAnimationAssetReloadBatch&& other) noexcept = default;
PreparedAnimationAssetReloadBatch& PreparedAnimationAssetReloadBatch::operator=(
	PreparedAnimationAssetReloadBatch&& other) noexcept = default;

bool PreparedAnimationAssetReloadBatch::Accepted() const noexcept
{
	return m_accepted;
}

bool PreparedAnimationAssetReloadBatch::Committed() const noexcept
{
	return m_committed;
}

std::size_t PreparedAnimationAssetReloadBatch::Size() const noexcept
{
	return m_state != nullptr ? m_state->entries.size() : 0U;
}

std::string_view PreparedAnimationAssetReloadBatch::Diagnostic() const noexcept
{
	return m_diagnostic;
}

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
		case AssetType::PhysicsMaterial:
			return { ".tephysmat" };
		case AssetType::CookedCollider:
			return { ".tepcoll" };
		case AssetType::Skeleton:
			return { ".teskel" };
		case AssetType::AnimationClip:
			return { ".teanim" };
		case AssetType::AnimationController:
			return { ".teanimctrl" };
		case AssetType::AnimationMask:
			return { ".teanimmask" };
		case AssetType::RetargetProfile:
			return { ".teretarget" };
		case AssetType::MotionDatabase:
			return { ".temotiondb" };
		case AssetType::RagdollProfile:
			return { ".teragdoll" };
		case AssetType::AudioClip:
			return { ".teaudio" };
		case AssetType::Terrain:
			return { ".teterrain" };
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

	std::string AnimationValidationRecoveryAction(TerraEngine::Animation::AnimationStatus status)
	{
		using TerraEngine::Animation::AnimationStatus;
		switch (status)
		{
		case AnimationStatus::MissingAsset:
			return "Verify the cooked artifact is installed or mounted and that the registered path is correct.";
		case AnimationStatus::IncompatibleAsset:
			return "Reimport the skeleton, clips, and deformable meshes together so their compatibility metadata matches.";
		case AnimationStatus::UnsupportedMajorVersion:
			return "Recook the asset with this TerraEngine version; unsupported binary layouts are never guessed at runtime.";
		case AnimationStatus::ContentHashMismatch:
			return "Restore or recook the asset because its checked payload no longer matches the recorded content hash.";
		case AnimationStatus::RangeOverflow:
		case AnimationStatus::RangeOutOfBounds:
		case AnimationStatus::TruncatedInput:
			return "Recook or restore the asset; one or more serialized byte ranges are truncated or outside the file.";
		case AnimationStatus::HardLimitExceeded:
			return "Split or simplify the authored content until every count is within the documented runtime hard limits.";
		case AnimationStatus::Unsupported:
			return "Use the cooked TerraEngine runtime extension instead of a DCC source file.";
		default:
			return "Recook the asset and inspect the import diagnostics for the first malformed field.";
		}
	}

	void RejectAnimationValidation(
		AnimationResourceValidationResult& result,
		TerraEngine::Animation::AnimationStatus status,
		std::string code,
		std::string_view detail)
	{
		result.status = status;
		result.diagnosticCode = std::move(code);
		result.message = std::string(detail) + " Action: " + AnimationValidationRecoveryAction(status);
	}

	AnimationResourceValidationResult MakeAnimationValidationBase(
		const ResourceManager& manager,
		const std::filesystem::path& path,
		AssetType type,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint)
	{
		AnimationResourceValidationResult result;
		result.assetType = type;
		result.resolvedPath = manager.ResolveAssetPath(path);
		result.expectedCompatibilityFingerprint = expectedCompatibilityFingerprint;
		return result;
	}

	void ProbeAnimationEnvelopeVersion(
		std::span<const std::uint8_t> bytes,
		AnimationResourceValidationResult& result)
	{
		if (bytes.size() < sizeof(TerraEngine::Animation::AnimationAssetEnvelope))
		{
			return;
		}
		TerraEngine::Animation::AnimationAssetEnvelope envelope{};
		std::memcpy(&envelope, bytes.data(), sizeof(envelope));
		result.detectedMajorVersion = envelope.majorVersion;
		result.detectedMinorVersion = envelope.minorVersion;
	}

	std::unique_ptr<TerraEngine::Animation::SkeletonAsset> ReadSkeletonResource(
		const ResourceManager& manager,
		const std::filesystem::path& resolvedPath,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		AnimationResourceValidationResult& result)
	{
		using namespace TerraEngine::Animation;
		result = MakeAnimationValidationBase(manager, resolvedPath, AssetType::Skeleton, expectedCompatibilityFingerprint);
		const std::vector<std::uint8_t> bytes = manager.ReadFileBytes(result.resolvedPath);
		result.fileSizeBytes = bytes.size();
		ProbeAnimationEnvelopeVersion(bytes, result);
		if (bytes.empty())
		{
			RejectAnimationValidation(result, AnimationStatus::MissingAsset, "SkeletonAssetMissing",
				"The cooked .teskel artifact could not be read from the project or mounted runtime content.");
			return nullptr;
		}

		AnimationAssetReader reader;
		if (const AnimationStatus status = reader.Parse(bytes); status != AnimationStatus::Ok)
		{
			RejectAnimationValidation(result, status, "SkeletonEnvelopeRejected",
				"The common animation envelope, chunk table, or content hash is invalid.");
			return nullptr;
		}
		result.detectedMajorVersion = reader.Envelope().majorVersion;
		result.detectedMinorVersion = reader.Envelope().minorVersion;
		result.assetId = reader.Envelope().assetId.value;
		result.actualCompatibilityFingerprint = reader.Envelope().compatibilityFingerprint;
		std::memcpy(&result.contentFingerprint, reader.Envelope().contentHash.data(),
			sizeof(result.contentFingerprint));
		result.contentHash = reader.Envelope().contentHash;
		result.contentHashPresent = true;
		result.compatibilityFingerprintPresent = reader.Envelope().compatibilityFingerprint != 0U;
		if (!result.compatibilityFingerprintPresent)
		{
			RejectAnimationValidation(result, AnimationStatus::IncompatibleAsset, "SkeletonFingerprintMissing",
				"The skeleton does not publish the mandatory compatibility fingerprint.");
			return nullptr;
		}
		if (expectedCompatibilityFingerprint != 0U
			&& reader.Envelope().compatibilityFingerprint != expectedCompatibilityFingerprint)
		{
			RejectAnimationValidation(result, AnimationStatus::IncompatibleAsset, "SkeletonFingerprintMismatch",
				"The skeleton fingerprint does not match the binding requested by the runtime.");
			return nullptr;
		}

		auto asset = std::make_unique<SkeletonAsset>();
		const AnimationCompatibilityFingerprint fingerprintToValidate = expectedCompatibilityFingerprint != 0U
			? expectedCompatibilityFingerprint
			: reader.Envelope().compatibilityFingerprint;
		if (const AnimationStatus status = DeserializeSkeletonAsset(bytes, fingerprintToValidate, *asset);
			status != AnimationStatus::Ok)
		{
			RejectAnimationValidation(result, status, "SkeletonPayloadRejected",
				"The skeleton payload failed its checked tables, hierarchy, bind-pose, or semantic-identity validation.");
			return nullptr;
		}

		result.jointCount = static_cast<std::uint32_t>(asset->joints.size());
		result.status = AnimationStatus::Ok;
		result.diagnosticCode = "SkeletonAssetValid";
		result.message = "The source-independent skeleton envelope, fingerprint, hierarchy, inverse binds, sockets, and ranges are valid.";
		return asset;
	}

	std::unique_ptr<TerraEngine::Animation::AnimationClipAsset> ReadAnimationClipResource(
		const ResourceManager& manager,
		const std::filesystem::path& resolvedPath,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		AnimationResourceValidationResult& result)
	{
		using namespace TerraEngine::Animation;
		result = MakeAnimationValidationBase(manager, resolvedPath, AssetType::AnimationClip, expectedCompatibilityFingerprint);
		const std::vector<std::uint8_t> bytes = manager.ReadFileBytes(result.resolvedPath);
		result.fileSizeBytes = bytes.size();
		ProbeAnimationEnvelopeVersion(bytes, result);
		if (bytes.empty())
		{
			RejectAnimationValidation(result, AnimationStatus::MissingAsset, "AnimationClipMissing",
				"The cooked .teanim artifact could not be read from the project or mounted runtime content.");
			return nullptr;
		}

		AnimationAssetReader reader;
		if (const AnimationStatus status = reader.Parse(bytes); status != AnimationStatus::Ok)
		{
			RejectAnimationValidation(result, status, "AnimationClipEnvelopeRejected",
				"The common animation envelope, chunk table, or content hash is invalid.");
			return nullptr;
		}
		result.detectedMajorVersion = reader.Envelope().majorVersion;
		result.detectedMinorVersion = reader.Envelope().minorVersion;
		result.assetId = reader.Envelope().assetId.value;
		result.actualCompatibilityFingerprint = reader.Envelope().compatibilityFingerprint;
		std::memcpy(&result.contentFingerprint, reader.Envelope().contentHash.data(),
			sizeof(result.contentFingerprint));
		result.contentHash = reader.Envelope().contentHash;
		result.contentHashPresent = true;
		result.compatibilityFingerprintPresent = reader.Envelope().compatibilityFingerprint != 0U;
		if (!result.compatibilityFingerprintPresent)
		{
			RejectAnimationValidation(result, AnimationStatus::IncompatibleAsset, "AnimationClipFingerprintMissing",
				"The animation clip does not publish the mandatory skeleton compatibility fingerprint.");
			return nullptr;
		}
		if (expectedCompatibilityFingerprint != 0U
			&& reader.Envelope().compatibilityFingerprint != expectedCompatibilityFingerprint)
		{
			RejectAnimationValidation(result, AnimationStatus::IncompatibleAsset, "AnimationClipFingerprintMismatch",
				"The animation clip fingerprint does not match the target skeleton binding.");
			return nullptr;
		}

		auto asset = std::make_unique<AnimationClipAsset>();
		if (const AnimationStatus status = DeserializeAnimationClipAsset(bytes, expectedCompatibilityFingerprint, *asset);
			status != AnimationStatus::Ok)
		{
			RejectAnimationValidation(result, status, "AnimationClipPayloadRejected",
				"The clip payload failed its checked segment, track, event, curve, morph, or stream-table validation.");
			return nullptr;
		}
		AnimationCodecView decoder;
		if (const AnimationStatus status = decoder.Open(*asset); status != AnimationStatus::Ok)
		{
			RejectAnimationValidation(result, status, "AnimationClipCodecRejected",
				"The clip metadata is valid, but its independently seekable codec ranges or decoder version are invalid.");
			return nullptr;
		}

		result.clipSegmentCount = static_cast<std::uint32_t>(asset->segments.size());
		result.clipTrackCount = static_cast<std::uint32_t>(asset->tracks.size());
		result.status = AnimationStatus::Ok;
		result.diagnosticCode = "AnimationClipValid";
		result.message = "The source-independent clip envelope, fingerprint, codec, segment tables, and checked stream ranges are valid.";
		return asset;
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
		case TerraEngine::Resource::ResidencyAssetType::Skeleton:
			return AssetType::Skeleton;
		case TerraEngine::Resource::ResidencyAssetType::AnimationClip:
			return AssetType::AnimationClip;
		case TerraEngine::Resource::ResidencyAssetType::AnimationController:
			return AssetType::AnimationController;
		case TerraEngine::Resource::ResidencyAssetType::AnimationMask:
			return AssetType::AnimationMask;
		case TerraEngine::Resource::ResidencyAssetType::RetargetProfile:
			return AssetType::RetargetProfile;
		case TerraEngine::Resource::ResidencyAssetType::MotionDatabase:
			return AssetType::MotionDatabase;
	case TerraEngine::Resource::ResidencyAssetType::RagdollProfile:
		return AssetType::RagdollProfile;
	case TerraEngine::Resource::ResidencyAssetType::AudioClip:
		return AssetType::AudioClip;
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

	bool IsNativeAnimationAssetPath(const std::filesystem::path& path)
	{
		for (std::uint32_t value = static_cast<std::uint32_t>(AssetType::Skeleton);
			value <= static_cast<std::uint32_t>(AssetType::RagdollProfile); ++value)
		{
			if (HasNativeExtensionForAssetType(path, static_cast<AssetType>(value)))
			{
				return true;
			}
		}
		return false;
	}

	bool IsNativeAudioAssetPath(const std::filesystem::path& path)
	{
		return HasNativeExtensionForAssetType(path, AssetType::AudioClip);
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

	// Validate the common envelope and the type-specific payload before an
	// opaque animation blob is allowed to cross the cache publication boundary.
	// The caller may request the stable UUID encoded in the envelope so the
	// path registry and immutable generation ledger remain keyed consistently.
	bool ValidateOpaqueAnimationPayload(
		std::span<const std::uint8_t> bytes,
		AssetType type,
		TerraEngine::UUID* assetId = nullptr,
		TerraEngine::Animation::AnimationStatus* failureStatus = nullptr,
		TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint = 0u)
	{
		using namespace TerraEngine::Animation;
		AnimationAssetReader envelopeReader;
		const AnimationStatus envelopeStatus = envelopeReader.Parse(bytes);
		if (envelopeStatus != AnimationStatus::Ok)
		{
			if (failureStatus != nullptr) *failureStatus = envelopeStatus;
			return false;
		}
		if (assetId != nullptr)
		{
			*assetId = envelopeReader.Envelope().assetId.value;
		}
		AnimationStatus status = AnimationStatus::Unsupported;
		switch (type)
		{
		case AssetType::AnimationController:
		{
			AnimationControllerAsset asset;
			status = DeserializeAnimationControllerAsset(bytes, expectedCompatibilityFingerprint, asset);
			break;
		}
		case AssetType::AnimationMask:
		{
			AnimationMaskAsset asset;
			status = DeserializeAnimationMaskAsset(bytes, expectedCompatibilityFingerprint, asset);
			break;
		}
		case AssetType::RetargetProfile:
		{
			RetargetProfileAsset asset;
			status = DeserializeRetargetProfileAsset(bytes, expectedCompatibilityFingerprint, asset);
			break;
		}
		case AssetType::MotionDatabase:
		{
			MotionDatabaseAsset asset;
			status = DeserializeMotionDatabaseAsset(bytes, expectedCompatibilityFingerprint, asset);
			break;
		}
		case AssetType::RagdollProfile:
		{
			RagdollProfileAsset asset;
			status = DeserializeRagdollProfileAsset(bytes, expectedCompatibilityFingerprint, asset);
			break;
		}
		default:
			break;
		}
		if (failureStatus != nullptr) *failureStatus = status;
		return status == AnimationStatus::Ok;
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

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const TerraEngine::Animation::SkeletonAsset& resource) const
{
	std::uint64_t bytes = sizeof(TerraEngine::Animation::SkeletonAsset);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.joints));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.inverseBindMatrices));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.sockets));
	bytes = AddPayloadBytes(bytes, resource.source.sourcePackagePath.size());
	for (const TerraEngine::Animation::SkeletonJoint& joint : resource.joints)
	{
		bytes = AddPayloadBytes(bytes, joint.displayName.size());
		bytes = AddPayloadBytes(bytes, joint.normalizedSourcePath.size());
		bytes = AddPayloadBytes(bytes, joint.semanticRole.size());
	}
	for (const TerraEngine::Animation::SkeletonSocket& socket : resource.sockets)
	{
		bytes = AddPayloadBytes(bytes, socket.displayName.size());
		bytes = AddPayloadBytes(bytes, socket.semanticRole.size());
	}
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const TerraEngine::Animation::AnimationClipAsset& resource) const
{
	std::uint64_t bytes = sizeof(TerraEngine::Animation::AnimationClipAsset);
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.segments));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.tracks));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.namedChannels));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.events));
	bytes = AddPayloadBytes(bytes, VectorPayloadBytes(resource.streamData));
	for (const TerraEngine::Animation::AnimationClipNamedChannel& channel : resource.namedChannels)
	{
		bytes = AddPayloadBytes(bytes, channel.name.size());
	}
	for (const TerraEngine::Animation::AnimationClipEvent& event : resource.events)
	{
		bytes = AddPayloadBytes(bytes, event.payload.size());
	}
	return bytes;
}

std::uint64_t ResourceManager::ComputeLoadedResourceBytes(const OpaqueAnimationResource& resource) const
{
	// Opaque assets are retained exactly as their cooked byte payload.  Their
	// specialist readers validate and interpret the bytes after this ownership
	// boundary, so residency must not substitute a guessed decoded size.
	return AddPayloadBytes(sizeof(OpaqueAnimationResource), VectorPayloadBytes(resource.bytes));
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
	// Coordinated reloads never allocate after their cache swaps. This bounded
	// reserve holds one retired payload per supported pending publisher record.
	m_RetiredAnimationPayloads.reserve(16384U);
	assetRoot = std::filesystem::current_path();
	engineRoot = std::filesystem::current_path();
	m_ResidencyPolicy = TerraEngine::Resource::MakeDefaultResidencyPolicy();
	m_StreamingPolicy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
	materialCache.assetType = AssetType::Material;
	materialCache.tracksResidency = true;
	meshCache.assetType = AssetType::Mesh;
	meshCache.tracksResidency = true;
	skeletonCache.assetType = AssetType::Skeleton;
	animationClipCache.assetType = AssetType::AnimationClip;
	// Animation payloads are CPU-owned immutable snapshots.  Tracking them in
	// the ordinary residency system makes gameplay-critical retention and
	// pressure-driven release use the same protected boundary as other assets.
	skeletonCache.tracksResidency = true;
	animationClipCache.tracksResidency = true;
	animationControllerCache.assetType = AssetType::AnimationController;
	animationControllerCache.tracksResidency = true;
	animationMaskCache.assetType = AssetType::AnimationMask;
	animationMaskCache.tracksResidency = true;
	retargetProfileCache.assetType = AssetType::RetargetProfile;
	retargetProfileCache.tracksResidency = true;
	motionDatabaseCache.assetType = AssetType::MotionDatabase;
	motionDatabaseCache.tracksResidency = true;
	ragdollProfileCache.assetType = AssetType::RagdollProfile;
	ragdollProfileCache.tracksResidency = true;
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
	if (m_EditorStartupProfileConfigured && !m_ConfiguredExportArchivePath.has_value()) {
		m_ConfiguredRenderSettings = m_EditorStartupRenderSettings;
		m_RenderValidationEnabled = m_EditorStartupValidationEnabled;
		m_RenderDebugEnabled = m_EditorStartupDebugEnabled;
		TERRAENGINE_LOG(LogLevel::Info, "Editor startup renderer profile applied before system initialization. origin="
			<< m_EditorStartupOrigin << ", project="
			<< (m_EditorStartupProjectRoot ? m_EditorStartupProjectRoot->string() : std::string("<hub-defaults>"))
			<< ", validation=" << (m_RenderValidationEnabled ? "on" : "off")
			<< ", debug=" << (m_RenderDebugEnabled ? "on" : "off"));
		return;
	}

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
	ClearAnimationRuntimeGenerations();

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
	if (m_EditorStartupProjectRoot.has_value())
	{
		// Direct-project bootstrap is explicit and independent of the process
		// working directory. Full project activation may later update assetRoot.
		return *m_EditorStartupProjectRoot;
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

void ResourceManager::ConfigureEditorStartupProfile(const RenderSettings& settings,
	bool validationEnabled, bool debugEnabled,
	std::optional<std::filesystem::path> projectRoot, std::string origin)
{
	m_EditorStartupProfileConfigured = true;
	m_EditorStartupRenderSettings = settings;
	m_EditorStartupValidationEnabled = validationEnabled;
	m_EditorStartupDebugEnabled = debugEnabled;
	m_EditorStartupProjectRoot = projectRoot
		? std::optional<std::filesystem::path>(projectRoot->lexically_normal())
		: std::nullopt;
	m_EditorStartupOrigin = std::move(origin);
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

AnimationResourceValidationResult ResourceManager::ValidateSkeletonAssetFile(
	const std::filesystem::path& path,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint) const
{
	AnimationResourceValidationResult result = MakeAnimationValidationBase(
		*this, path, AssetType::Skeleton, expectedCompatibilityFingerprint);
	if (!HasNativeExtensionForAssetType(path, AssetType::Skeleton))
	{
		RejectAnimationValidation(result, TerraEngine::Animation::AnimationStatus::Unsupported,
			"SkeletonSourceFormatRejected",
			"Runtime skeleton loading accepts only cooked .teskel files and never parses a glTF or FBX source package.");
		return result;
	}
	ReadSkeletonResource(*this, result.resolvedPath, expectedCompatibilityFingerprint, result);
	return result;
}

AnimationResourceValidationResult ResourceManager::ValidateAnimationClipAssetFile(
	const std::filesystem::path& path,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint) const
{
	AnimationResourceValidationResult result = MakeAnimationValidationBase(
		*this, path, AssetType::AnimationClip, expectedCompatibilityFingerprint);
	if (!HasNativeExtensionForAssetType(path, AssetType::AnimationClip))
	{
		RejectAnimationValidation(result, TerraEngine::Animation::AnimationStatus::Unsupported,
			"AnimationClipSourceFormatRejected",
			"Runtime clip loading accepts only cooked .teanim files and never parses a glTF or FBX source package.");
		return result;
	}
	ReadAnimationClipResource(*this, result.resolvedPath, expectedCompatibilityFingerprint, result);
	return result;
}

AnimationResourceValidationResult ResourceManager::ValidateAnimationAssetFile(
	const std::filesystem::path& path,
	AssetType type,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint) const
{
	if (type == AssetType::Skeleton)
	{
		return ValidateSkeletonAssetFile(path, expectedCompatibilityFingerprint);
	}
	if (type == AssetType::AnimationClip)
	{
		return ValidateAnimationClipAssetFile(path, expectedCompatibilityFingerprint);
	}
	AnimationResourceValidationResult result = MakeAnimationValidationBase(
		*this, path, type, expectedCompatibilityFingerprint);
	if (!IsAnimationAssetType(type) || !HasNativeExtensionForAssetType(path, type))
	{
		RejectAnimationValidation(result, TerraEngine::Animation::AnimationStatus::Unsupported,
			"AnimationSourceFormatRejected",
			"Runtime animation loading accepts only the cooked extension for the requested typed asset.");
		return result;
	}
	const std::vector<std::uint8_t> bytes = ReadFileBytes(result.resolvedPath);
	result.fileSizeBytes = bytes.size();
	ProbeAnimationEnvelopeVersion(bytes, result);
	if (bytes.empty())
	{
		RejectAnimationValidation(result, TerraEngine::Animation::AnimationStatus::MissingAsset,
			"AnimationAssetMissing", "The cooked animation artifact could not be read.");
		return result;
	}
	TerraEngine::Animation::AnimationAssetReader reader;
	const TerraEngine::Animation::AnimationStatus envelopeStatus = reader.Parse(bytes);
	if (envelopeStatus == TerraEngine::Animation::AnimationStatus::Ok)
	{
		result.detectedMajorVersion = reader.Envelope().majorVersion;
		result.detectedMinorVersion = reader.Envelope().minorVersion;
		result.assetId = reader.Envelope().assetId.value;
		result.actualCompatibilityFingerprint = reader.Envelope().compatibilityFingerprint;
		std::memcpy(&result.contentFingerprint, reader.Envelope().contentHash.data(),
			sizeof(result.contentFingerprint));
		result.contentHash = reader.Envelope().contentHash;
		result.contentHashPresent = true;
		result.compatibilityFingerprintPresent = reader.Envelope().compatibilityFingerprint != 0u;
	}
	TerraEngine::Animation::AnimationStatus status = envelopeStatus;
	TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
	if (ValidateOpaqueAnimationPayload(bytes, type, &assetId, &status, expectedCompatibilityFingerprint))
	{
		result.status = TerraEngine::Animation::AnimationStatus::Ok;
		result.diagnosticCode = "AnimationAssetValidated";
		result.message = "The complete typed animation payload passed envelope, bounds, hash, and compatibility validation.";
		return result;
	}
	RejectAnimationValidation(result, status, "AnimationAssetRejected",
		"The typed cooked animation payload failed validation before publication.");
	return result;
}

TerraEngine::UUID ResourceManager::LoadSkeleton(
	const std::filesystem::path& path,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint)
{
	if (!HasNativeExtensionForAssetType(path, AssetType::Skeleton))
	{
		TERRAENGINE_LOG(LogLevel::Warning,
			"Skipping non-cooked skeleton source; runtime requires .teskel: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	if (const std::optional<TerraEngine::UUID> cached = ResolveCached(key, skeletonCache))
	{
		std::shared_lock lock(skeletonCache.mutex);
		const auto resource = skeletonCache.resources.find(*cached);
		if (resource != skeletonCache.resources.end()
			&& (expectedCompatibilityFingerprint == 0U
				|| resource->second->compatibilityFingerprint == expectedCompatibilityFingerprint))
		{
			return *cached;
		}
		TERRAENGINE_LOG(LogLevel::Error,
			"Cached skeleton fingerprint does not match the requested binding for " << resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}

	AnimationResourceValidationResult validation;
	std::unique_ptr<TerraEngine::Animation::SkeletonAsset> asset = ReadSkeletonResource(
		*this, resolvedPath, expectedCompatibilityFingerprint, validation);
	if (!asset)
	{
		TERRAENGINE_LOG(LogLevel::Error,
			validation.diagnosticCode << ": " << validation.message << " path=" << validation.resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}

	const TerraEngine::UUID id = asset->assetId.value;
	const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*asset);
	{
		std::unique_lock lock(skeletonCache.mutex);
		if (const auto registered = skeletonCache.pathToId.find(key);
			registered != skeletonCache.pathToId.end() && registered->second != id)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Registered skeleton UUID does not match the cooked envelope for " << resolvedPath);
			return TerraEngine::EMPTY_UUID;
		}
		// A concurrent request may have published this exact resource while this
		// caller parsed from disk. Reuse it instead of manufacturing a false
		// hot-reload generation for unchanged data.
		if(const auto existing = skeletonCache.resources.find(id); existing != skeletonCache.resources.end()) {
			return id;
		}
		const std::optional<TerraEngine::Animation::AnimationGeneration> generation =
			AdvanceAnimationGenerationLocked(skeletonCache, id);
		if(!generation.has_value()) {
			TERRAENGINE_LOG(LogLevel::Error, "Skeleton runtime generation exhausted for " << resolvedPath);
			return TerraEngine::EMPTY_UUID;
		}
		asset->runtimeGeneration = *generation;
		skeletonCache.pathToId[key] = id;
		skeletonCache.idToPath[id] = key;
		skeletonCache.resources[id] = std::move(asset);
		skeletonCache.inFlight.erase(key);
	}
	RegisterResidencyResource(id, AssetType::Skeleton, key, cpuBytes, 0U, true,
		TerraEngine::Resource::ResidencyState::Active);
	UpdateStreamableLoadResult(id, AssetType::Skeleton, key, true, cpuBytes, 0U);
	return id;
}

TerraEngine::UUID ResourceManager::LoadAnimationClip(
	const std::filesystem::path& path,
	TerraEngine::Animation::AnimationCompatibilityFingerprint expectedCompatibilityFingerprint)
{
	if (!HasNativeExtensionForAssetType(path, AssetType::AnimationClip))
	{
		TERRAENGINE_LOG(LogLevel::Warning,
			"Skipping non-cooked animation source; runtime requires .teanim: " << path);
		return TerraEngine::EMPTY_UUID;
	}

	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	if (const std::optional<TerraEngine::UUID> cached = ResolveCached(key, animationClipCache))
	{
		std::shared_lock lock(animationClipCache.mutex);
		const auto resource = animationClipCache.resources.find(*cached);
		if (resource != animationClipCache.resources.end()
			&& (expectedCompatibilityFingerprint == 0U
				|| resource->second->compatibilityFingerprint == expectedCompatibilityFingerprint))
		{
			return *cached;
		}
		TERRAENGINE_LOG(LogLevel::Error,
			"Cached animation clip fingerprint does not match the requested skeleton for " << resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}

	AnimationResourceValidationResult validation;
	std::unique_ptr<TerraEngine::Animation::AnimationClipAsset> asset = ReadAnimationClipResource(
		*this, resolvedPath, expectedCompatibilityFingerprint, validation);
	if (!asset)
	{
		TERRAENGINE_LOG(LogLevel::Error,
			validation.diagnosticCode << ": " << validation.message << " path=" << validation.resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}

	const TerraEngine::UUID id = asset->assetId.value;
	const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*asset);
	{
		std::unique_lock lock(animationClipCache.mutex);
		if (const auto registered = animationClipCache.pathToId.find(key);
			registered != animationClipCache.pathToId.end() && registered->second != id)
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Registered animation-clip UUID does not match the cooked envelope for " << resolvedPath);
			return TerraEngine::EMPTY_UUID;
		}
		// See the equivalent skeleton branch: concurrent readers share one
		// publication rather than turning ordinary cache contention into a reload.
		if(const auto existing = animationClipCache.resources.find(id); existing != animationClipCache.resources.end()) {
			return id;
		}
		const std::optional<TerraEngine::Animation::AnimationGeneration> generation =
			AdvanceAnimationGenerationLocked(animationClipCache, id);
		if(!generation.has_value()) {
			TERRAENGINE_LOG(LogLevel::Error, "Animation clip runtime generation exhausted for " << resolvedPath);
			return TerraEngine::EMPTY_UUID;
		}
		asset->runtimeGeneration = *generation;
		animationClipCache.pathToId[key] = id;
		animationClipCache.idToPath[id] = key;
		animationClipCache.resources[id] = std::move(asset);
		animationClipCache.inFlight.erase(key);
	}
	RegisterResidencyResource(id, AssetType::AnimationClip, key, cpuBytes, 0U, true,
		TerraEngine::Resource::ResidencyState::Active);
	UpdateStreamableLoadResult(id, AssetType::AnimationClip, key, true, cpuBytes, 0U);
	return id;
}

TerraEngine::UUID ResourceManager::LoadOpaqueAnimationAsset(const std::filesystem::path& path, AssetType type)
{
	if (!HasNativeExtensionForAssetType(path, type))
	{
		return TerraEngine::EMPTY_UUID;
	}

	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	const auto loadIntoCache = [&](auto& cache) {
		if (const std::optional<TerraEngine::UUID> cached = ResolveCached(key, cache))
		{
			return *cached;
		}
		auto resource = std::make_unique<OpaqueAnimationResource>();
		resource->bytes = ReadFileBytes(resolvedPath);
		if (resource->bytes.empty()
			|| !ValidateOpaqueAnimationPayload(resource->bytes, type, &resource->assetId)
			|| resource->assetId == TerraEngine::EMPTY_UUID)
		{
			return TerraEngine::EMPTY_UUID;
		}
		const TerraEngine::UUID id = resource->assetId;
		const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*resource);
		{
			std::unique_lock lock(cache.mutex);
			if (const auto registered = cache.pathToId.find(key);
				registered != cache.pathToId.end() && registered->second != id)
			{
				return TerraEngine::EMPTY_UUID;
			}
			if (cache.resources.contains(id))
			{
				return id;
			}
			if (!AdvanceAnimationGenerationLocked(cache, id).has_value())
			{
				return TerraEngine::EMPTY_UUID;
			}
			cache.pathToId[key] = id;
			cache.idToPath[id] = key;
			cache.resources[id] = std::move(resource);
			cache.inFlight.erase(key);
			cache.inFlightTokens.erase(key);
		}
		RegisterResidencyResource(id, type, key, cpuBytes, 0u, true,
			TerraEngine::Resource::ResidencyState::Active);
		UpdateStreamableLoadResult(id, type, key, true, cpuBytes, 0u);
		return id;
	};

	switch (type)
	{
	case AssetType::AnimationController: return loadIntoCache(animationControllerCache);
	case AssetType::AnimationMask: return loadIntoCache(animationMaskCache);
	case AssetType::RetargetProfile: return loadIntoCache(retargetProfileCache);
	case AssetType::MotionDatabase: return loadIntoCache(motionDatabaseCache);
	case AssetType::RagdollProfile: return loadIntoCache(ragdollProfileCache);
	default: return TerraEngine::EMPTY_UUID;
	}
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadOpaqueAnimationAssetAsync(
	const std::filesystem::path& path,
	AssetType type)
{
	if (!HasNativeExtensionForAssetType(path, type))
	{
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}

	const LoaderFunction<OpaqueAnimationResource> loader = [this, type](const std::filesystem::path& resolvedPath) {
		auto resource = std::make_unique<OpaqueAnimationResource>();
		resource->bytes = ReadFileBytes(resolvedPath);
		return resource->bytes.empty() || !ValidateOpaqueAnimationPayload(resource->bytes, type, &resource->assetId)
			? std::unique_ptr<OpaqueAnimationResource>{} : std::move(resource);
	};

	switch (type)
	{
	case AssetType::AnimationController: return LoadOrSchedule(path, animationControllerCache, loader, true);
	case AssetType::AnimationMask: return LoadOrSchedule(path, animationMaskCache, loader, true);
	case AssetType::RetargetProfile: return LoadOrSchedule(path, retargetProfileCache, loader, true);
	case AssetType::MotionDatabase: return LoadOrSchedule(path, motionDatabaseCache, loader, true);
	case AssetType::RagdollProfile: return LoadOrSchedule(path, ragdollProfileCache, loader, true);
	default: return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}
}

std::shared_future<TerraEngine::UUID> ResourceManager::LoadAnimationAssetAsync(
	const std::filesystem::path& path,
	AssetType type)
{
	if (!IsAnimationAssetType(type) || !HasNativeExtensionForAssetType(path, type))
	{
		return MakeReadyFuture(TerraEngine::EMPTY_UUID);
	}
	if (type == AssetType::Skeleton)
	{
		const LoaderFunction<TerraEngine::Animation::SkeletonAsset> loader =
			[this](const std::filesystem::path& resolvedPath) {
				AnimationResourceValidationResult validation;
				return ReadSkeletonResource(*this, resolvedPath, 0u, validation);
			};
		return LoadOrSchedule(path, skeletonCache, loader, true);
	}
	if (type == AssetType::AnimationClip)
	{
		const LoaderFunction<TerraEngine::Animation::AnimationClipAsset> loader =
			[this](const std::filesystem::path& resolvedPath) {
				AnimationResourceValidationResult validation;
				return ReadAnimationClipResource(*this, resolvedPath, 0u, validation);
			};
		return LoadOrSchedule(path, animationClipCache, loader, true);
	}
	return LoadOpaqueAnimationAssetAsync(path, type);
}

bool ResourceManager::ReloadSkeleton(const TerraEngine::UUID& id)
{
	std::string sourceKey;
	TerraEngine::Animation::AnimationGeneration observedGeneration = 0U;
	{
		std::shared_lock lock(skeletonCache.mutex);
		const auto path = skeletonCache.idToPath.find(id);
		const auto generation = skeletonCache.runtimeGenerations.find(id);
		if(path == skeletonCache.idToPath.end()
			|| skeletonCache.resources.find(id) == skeletonCache.resources.end()
			|| generation == skeletonCache.runtimeGenerations.end()
			|| generation->second == std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max()) return false;
		sourceKey = path->second;
		observedGeneration = generation->second;
	}
	AnimationResourceValidationResult validation;
	std::unique_ptr<TerraEngine::Animation::SkeletonAsset> candidate = ReadSkeletonResource(
		*this, sourceKey, 0U, validation);
	if(!candidate || candidate->assetId.value != id) return false;
	const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*candidate);
	{
		std::unique_lock lock(skeletonCache.mutex);
		const auto path = skeletonCache.idToPath.find(id);
		const auto generation = skeletonCache.runtimeGenerations.find(id);
		if(path == skeletonCache.idToPath.end() || path->second != sourceKey
			|| skeletonCache.resources.find(id) == skeletonCache.resources.end()
			|| generation == skeletonCache.runtimeGenerations.end()
			|| generation->second != observedGeneration) return false;
		const std::optional<TerraEngine::Animation::AnimationGeneration> next =
			AdvanceAnimationGenerationLocked(skeletonCache, id);
		if(!next.has_value()) return false;
		candidate->runtimeGeneration = *next;
		skeletonCache.resources[id] = std::move(candidate);
	}
	RegisterResidencyResource(id, AssetType::Skeleton, sourceKey, cpuBytes, 0U, true,
		TerraEngine::Resource::ResidencyState::Active);
	UpdateStreamableLoadResult(id, AssetType::Skeleton, sourceKey, true, cpuBytes, 0U);
	return true;
}

bool ResourceManager::ReloadAnimationClip(const TerraEngine::UUID& id)
{
	std::string sourceKey;
	TerraEngine::Animation::AnimationGeneration observedGeneration = 0U;
	{
		std::shared_lock lock(animationClipCache.mutex);
		const auto path = animationClipCache.idToPath.find(id);
		const auto generation = animationClipCache.runtimeGenerations.find(id);
		if(path == animationClipCache.idToPath.end()
			|| animationClipCache.resources.find(id) == animationClipCache.resources.end()
			|| generation == animationClipCache.runtimeGenerations.end()
			|| generation->second == std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max()) return false;
		sourceKey = path->second;
		observedGeneration = generation->second;
	}
	AnimationResourceValidationResult validation;
	std::unique_ptr<TerraEngine::Animation::AnimationClipAsset> candidate = ReadAnimationClipResource(
		*this, sourceKey, 0U, validation);
	if(!candidate || candidate->assetId.value != id) return false;
	const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*candidate);
	{
		std::unique_lock lock(animationClipCache.mutex);
		const auto path = animationClipCache.idToPath.find(id);
		const auto generation = animationClipCache.runtimeGenerations.find(id);
		if(path == animationClipCache.idToPath.end() || path->second != sourceKey
			|| animationClipCache.resources.find(id) == animationClipCache.resources.end()
			|| generation == animationClipCache.runtimeGenerations.end()
			|| generation->second != observedGeneration) return false;
		const std::optional<TerraEngine::Animation::AnimationGeneration> next =
			AdvanceAnimationGenerationLocked(animationClipCache, id);
		if(!next.has_value()) return false;
		candidate->runtimeGeneration = *next;
		animationClipCache.resources[id] = std::move(candidate);
	}
	RegisterResidencyResource(id, AssetType::AnimationClip, sourceKey, cpuBytes, 0U, true,
		TerraEngine::Resource::ResidencyState::Active);
	UpdateStreamableLoadResult(id, AssetType::AnimationClip, sourceKey, true, cpuBytes, 0U);
	return true;
}

bool ResourceManager::ReloadAnimationAsset(const TerraEngine::UUID& id, AssetType type)
{
	if (id == TerraEngine::EMPTY_UUID || !IsAnimationAssetType(type))
	{
		return false;
	}

	if (type == AssetType::Skeleton)
	{
		return ReloadSkeleton(id);
	}
	if (type == AssetType::AnimationClip)
	{
		return ReloadAnimationClip(id);
	}

	const auto reloadOpaque = [&](auto& cache) {
		std::string sourceKey;
		TerraEngine::Animation::AnimationGeneration observedGeneration = 0u;
		{
			std::shared_lock lock(cache.mutex);
			const auto path = cache.idToPath.find(id);
			const auto generation = cache.runtimeGenerations.find(id);
			if (path == cache.idToPath.end() || cache.resources.find(id) == cache.resources.end()
				|| generation == cache.runtimeGenerations.end()
				|| generation->second == std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max())
			{
				return false;
			}
			sourceKey = path->second;
			observedGeneration = generation->second;
		}

		std::vector<std::uint8_t> bytes = ReadFileBytes(sourceKey);
		if (bytes.empty() || !ValidateOpaqueAnimationPayload(bytes, type))
		{
			return false;
		}
		TerraEngine::Animation::AnimationAssetReader reader;
		if (reader.Parse(bytes) != TerraEngine::Animation::AnimationStatus::Ok
			|| reader.Envelope().assetId.value != id)
		{
			return false;
		}

		auto candidate = std::make_unique<OpaqueAnimationResource>();
		candidate->assetId = id;
		candidate->bytes = std::move(bytes);
		const std::uint64_t cpuBytes = ComputeLoadedResourceBytes(*candidate);
		{
			std::unique_lock lock(cache.mutex);
			const auto path = cache.idToPath.find(id);
			const auto generation = cache.runtimeGenerations.find(id);
			if (path == cache.idToPath.end() || path->second != sourceKey
				|| cache.resources.find(id) == cache.resources.end()
				|| generation == cache.runtimeGenerations.end() || generation->second != observedGeneration)
			{
				return false;
			}
			if (!AdvanceAnimationGenerationLocked(cache, id).has_value())
			{
				return false;
			}
			cache.resources[id] = std::move(candidate);
		}
		RegisterResidencyResource(id, type, sourceKey, cpuBytes, 0u, true,
			TerraEngine::Resource::ResidencyState::Active);
		UpdateStreamableLoadResult(id, type, sourceKey, true, cpuBytes, 0u);
		return true;
	};

	switch (type)
	{
	case AssetType::AnimationController: return reloadOpaque(animationControllerCache);
	case AssetType::AnimationMask: return reloadOpaque(animationMaskCache);
	case AssetType::RetargetProfile: return reloadOpaque(retargetProfileCache);
	case AssetType::MotionDatabase: return reloadOpaque(motionDatabaseCache);
	case AssetType::RagdollProfile: return reloadOpaque(ragdollProfileCache);
	default: return false;
	}
}

PreparedAnimationAssetReloadBatch ResourceManager::PrepareAnimationAssetReloadBatch(
	std::span<const AnimationAssetReloadRequest> requests)
{
	PreparedAnimationAssetReloadBatch prepared;
	constexpr std::size_t kMaximumAtomicAnimationReloadAssets = 256U;
	if(requests.size() > kMaximumAtomicAnimationReloadAssets) {
		prepared.m_diagnostic = "The animation cache reload batch exceeds the hard 256-asset bound.";
		return prepared;
	}

	try {
		auto state = std::make_unique<PreparedAnimationAssetReloadBatch::State>();
		state->owner = this;
		state->entries.reserve(requests.size());
		std::unordered_set<std::string> identities;
		identities.reserve(requests.size());

		for(const AnimationAssetReloadRequest& request : requests) {
			if(request.assetId == TerraEngine::EMPTY_UUID || !IsAnimationAssetType(request.assetType)) {
				prepared.m_diagnostic = "Every animation cache reload request requires a non-empty typed asset identity.";
				return prepared;
			}
			const std::string identity = TerraEngine::ToHexString(request.assetId);
			if(!identities.insert(identity).second) {
				prepared.m_diagnostic = "An atomic animation cache reload batch contains the same asset identity more than once.";
				return prepared;
			}

			PreparedAnimationAssetReloadBatch::State::Entry entry;
			entry.assetId = request.assetId;
			entry.assetType = request.assetType;
			entry.publisherRetirementGeneration = request.publisherRetirementGeneration;
			const auto capturePublication = [&](const auto& cache) {
				std::shared_lock lock(cache.mutex);
				const auto path = cache.idToPath.find(request.assetId);
				const auto resource = cache.resources.find(request.assetId);
				const auto generation = cache.runtimeGenerations.find(request.assetId);
				if(path == cache.idToPath.end() || resource == cache.resources.end()
					|| !resource->second || generation == cache.runtimeGenerations.end()
					|| generation->second == 0U
					|| generation->second == std::numeric_limits<
						TerraEngine::Animation::AnimationGeneration>::max()) {
					return false;
				}
				entry.sourceKey = path->second;
				entry.observedGeneration = generation->second;
				entry.nextGeneration = generation->second + 1U;
				return true;
			};

			bool captured = false;
			switch(request.assetType) {
			case AssetType::Skeleton: captured = capturePublication(skeletonCache); break;
			case AssetType::AnimationClip: captured = capturePublication(animationClipCache); break;
			case AssetType::AnimationController: captured = capturePublication(animationControllerCache); break;
			case AssetType::AnimationMask: captured = capturePublication(animationMaskCache); break;
			case AssetType::RetargetProfile: captured = capturePublication(retargetProfileCache); break;
			case AssetType::MotionDatabase: captured = capturePublication(motionDatabaseCache); break;
			case AssetType::RagdollProfile: captured = capturePublication(ragdollProfileCache); break;
			default: break;
			}
			if(!captured) {
				prepared.m_diagnostic = "A requested animation asset is not a live, reloadable cache publication.";
				return prepared;
			}

			if(request.assetType == AssetType::Skeleton) {
				AnimationResourceValidationResult validation;
				auto candidate = ReadSkeletonResource(*this, entry.sourceKey,
					request.expectedCompatibilityFingerprint, validation);
				if(!candidate || candidate->assetId.value != request.assetId
					|| (request.expectedContentHashPresent
						&& (!validation.contentHashPresent
							|| validation.contentHash != request.expectedContentHash))
					|| (request.expectedContentFingerprint != 0U
						&& validation.contentFingerprint != request.expectedContentFingerprint)) {
					prepared.m_diagnostic = "A staged skeleton replacement failed checked parsing or durable identity validation.";
					return prepared;
				}
				candidate->runtimeGeneration = entry.nextGeneration;
				entry.cpuBytes = ComputeLoadedResourceBytes(*candidate);
				entry.payload = candidate.release();
				entry.destroyPayload = [](void* payload) noexcept {
					delete static_cast<TerraEngine::Animation::SkeletonAsset*>(payload);
				};
			} else if(request.assetType == AssetType::AnimationClip) {
				AnimationResourceValidationResult validation;
				auto candidate = ReadAnimationClipResource(*this, entry.sourceKey,
					request.expectedCompatibilityFingerprint, validation);
				if(!candidate || candidate->assetId.value != request.assetId
					|| (request.expectedContentHashPresent
						&& (!validation.contentHashPresent
							|| validation.contentHash != request.expectedContentHash))
					|| (request.expectedContentFingerprint != 0U
						&& validation.contentFingerprint != request.expectedContentFingerprint)) {
					prepared.m_diagnostic = "A staged animation clip replacement failed checked parsing or durable identity validation.";
					return prepared;
				}
				candidate->runtimeGeneration = entry.nextGeneration;
				entry.cpuBytes = ComputeLoadedResourceBytes(*candidate);
				entry.payload = candidate.release();
				entry.destroyPayload = [](void* payload) noexcept {
					delete static_cast<TerraEngine::Animation::AnimationClipAsset*>(payload);
				};
			} else {
				auto candidate = std::make_unique<OpaqueAnimationResource>();
				candidate->bytes = ReadFileBytes(entry.sourceKey);
				TerraEngine::UUID parsedAssetId = TerraEngine::EMPTY_UUID;
				if(candidate->bytes.empty()
					|| !ValidateOpaqueAnimationPayload(
						candidate->bytes, request.assetType, &parsedAssetId)
					|| parsedAssetId != request.assetId) {
					prepared.m_diagnostic = "A staged specialist animation replacement failed checked parsing or durable identity validation.";
					return prepared;
				}
				TerraEngine::Animation::AnimationAssetReader reader;
				if(reader.Parse(candidate->bytes) != TerraEngine::Animation::AnimationStatus::Ok
					|| (request.expectedCompatibilityFingerprint != 0U
						&& reader.Envelope().compatibilityFingerprint != request.expectedCompatibilityFingerprint)) {
					prepared.m_diagnostic = "A staged specialist animation replacement changed compatibility after publisher validation.";
					return prepared;
				}
				std::uint64_t contentFingerprint = 0U;
				std::memcpy(&contentFingerprint, reader.Envelope().contentHash.data(), sizeof(contentFingerprint));
				if(request.expectedContentHashPresent
					&& reader.Envelope().contentHash != request.expectedContentHash) {
					prepared.m_diagnostic = "A staged specialist animation replacement changed its complete content hash after publisher validation.";
					return prepared;
				}
				if(request.expectedContentFingerprint != 0U
					&& contentFingerprint != request.expectedContentFingerprint) {
					prepared.m_diagnostic = "A staged specialist animation replacement changed content after publisher validation.";
					return prepared;
				}
				candidate->assetId = parsedAssetId;
				entry.cpuBytes = ComputeLoadedResourceBytes(*candidate);
				entry.payload = candidate.release();
				entry.destroyPayload = [](void* payload) noexcept {
					delete static_cast<OpaqueAnimationResource*>(payload);
				};
			}
			state->entries.push_back(std::move(entry));
		}

		prepared.m_state = std::move(state);
		prepared.m_accepted = true;
		prepared.m_diagnostic = "Every replacement parsed successfully and is staged for one protected cache boundary.";
		return prepared;
	} catch(const std::exception& exception) {
		prepared.m_diagnostic = "Animation cache reload staging failed before publication: "
			+ std::string(exception.what());
		return prepared;
	} catch(...) {
		prepared.m_diagnostic = "Animation cache reload staging failed before publication with an unknown exception.";
		return prepared;
	}
}

bool ResourceManager::CommitPreparedAnimationAssetReloadBatch(
	PreparedAnimationAssetReloadBatch& prepared) noexcept
{
	if(!prepared.m_accepted || prepared.m_committed || !prepared.m_state
		|| prepared.m_state->owner != this) {
		return false;
	}

	try {
		PreparedAnimationAssetReloadBatch::State& state = *prepared.m_state;
		// These locks outlive the callback and are released only by the publisher's
		// postcommit hook. That makes cache and publisher publication one visible
		// boundary rather than two adjacent, independently observable operations.
		state.skeletonLock = std::unique_lock<std::shared_mutex>(skeletonCache.mutex, std::defer_lock);
		state.animationClipLock = std::unique_lock<std::shared_mutex>(animationClipCache.mutex, std::defer_lock);
		state.animationControllerLock = std::unique_lock<std::shared_mutex>(animationControllerCache.mutex, std::defer_lock);
		state.animationMaskLock = std::unique_lock<std::shared_mutex>(animationMaskCache.mutex, std::defer_lock);
		state.retargetProfileLock = std::unique_lock<std::shared_mutex>(retargetProfileCache.mutex, std::defer_lock);
		state.motionDatabaseLock = std::unique_lock<std::shared_mutex>(motionDatabaseCache.mutex, std::defer_lock);
		state.ragdollLock = std::unique_lock<std::shared_mutex>(ragdollProfileCache.mutex, std::defer_lock);
		state.retirementLock = std::unique_lock<std::mutex>(m_RetiredAnimationPayloadMutex, std::defer_lock);
		std::lock(state.skeletonLock, state.animationClipLock, state.animationControllerLock,
			state.animationMaskLock, state.retargetProfileLock, state.motionDatabaseLock,
			state.ragdollLock, state.retirementLock);
		constexpr std::size_t kMaximumRetiredAnimationPayloads = 16384U;
		if(state.entries.size() > kMaximumRetiredAnimationPayloads - m_RetiredAnimationPayloads.size()) {
			state.UnlockPublicationLocks();
			return false;
		}

		const auto bindDestination = [](auto& cache,
			PreparedAnimationAssetReloadBatch::State::Entry& entry) noexcept {
			const auto path = cache.idToPath.find(entry.assetId);
			const auto resource = cache.resources.find(entry.assetId);
			const auto generation = cache.runtimeGenerations.find(entry.assetId);
			if(path == cache.idToPath.end() || path->second != entry.sourceKey
				|| resource == cache.resources.end() || !resource->second
				|| generation == cache.runtimeGenerations.end()
				|| generation->second != entry.observedGeneration
				|| entry.nextGeneration != entry.observedGeneration + 1U
				|| entry.payload == nullptr) {
				return false;
			}
			entry.destinationSlot = &resource->second;
			entry.generationSlot = &generation->second;
			return true;
		};

		for(auto& entry : state.entries) {
			bool valid = false;
			switch(entry.assetType) {
			case AssetType::Skeleton: valid = bindDestination(skeletonCache, entry); break;
			case AssetType::AnimationClip: valid = bindDestination(animationClipCache, entry); break;
			case AssetType::AnimationController: valid = bindDestination(animationControllerCache, entry); break;
			case AssetType::AnimationMask: valid = bindDestination(animationMaskCache, entry); break;
			case AssetType::RetargetProfile: valid = bindDestination(retargetProfileCache, entry); break;
			case AssetType::MotionDatabase: valid = bindDestination(motionDatabaseCache, entry); break;
			case AssetType::RagdollProfile: valid = bindDestination(ragdollProfileCache, entry); break;
			default: break;
			}
			if(!valid) {
				for(auto& reset : state.entries) {
					reset.destinationSlot = nullptr;
					reset.generationSlot = nullptr;
				}
				state.UnlockPublicationLocks();
				return false;
			}
		}

		// No operation below this point allocates, performs IO, or searches a map.
		// Every concrete unique_ptr swap and integer assignment is no-throw, so a
		// successful callback can safely be followed by the publisher's own swaps.
		for(auto& entry : state.entries) {
			switch(entry.assetType) {
			case AssetType::Skeleton:
			{
				auto replacement = std::unique_ptr<TerraEngine::Animation::SkeletonAsset>(
					static_cast<TerraEngine::Animation::SkeletonAsset*>(entry.ReleasePayload()));
				static_cast<std::unique_ptr<TerraEngine::Animation::SkeletonAsset>*>(
					entry.destinationSlot)->swap(replacement);
				m_RetiredAnimationPayloads.push_back({entry.assetId, entry.observedGeneration,
					entry.publisherRetirementGeneration,
					replacement.release(), [](void* payload) noexcept {
						delete static_cast<TerraEngine::Animation::SkeletonAsset*>(payload);
					}});
				break;
			}
			case AssetType::AnimationClip:
			{
				auto replacement = std::unique_ptr<TerraEngine::Animation::AnimationClipAsset>(
					static_cast<TerraEngine::Animation::AnimationClipAsset*>(entry.ReleasePayload()));
				static_cast<std::unique_ptr<TerraEngine::Animation::AnimationClipAsset>*>(
					entry.destinationSlot)->swap(replacement);
				m_RetiredAnimationPayloads.push_back({entry.assetId, entry.observedGeneration,
					entry.publisherRetirementGeneration,
					replacement.release(), [](void* payload) noexcept {
						delete static_cast<TerraEngine::Animation::AnimationClipAsset*>(payload);
					}});
				break;
			}
			case AssetType::AnimationController:
			case AssetType::AnimationMask:
			case AssetType::RetargetProfile:
			case AssetType::MotionDatabase:
			case AssetType::RagdollProfile:
			{
				auto replacement = std::unique_ptr<OpaqueAnimationResource>(
					static_cast<OpaqueAnimationResource*>(entry.ReleasePayload()));
				static_cast<std::unique_ptr<OpaqueAnimationResource>*>(
					entry.destinationSlot)->swap(replacement);
				m_RetiredAnimationPayloads.push_back({entry.assetId, entry.observedGeneration,
					entry.publisherRetirementGeneration,
					replacement.release(), [](void* payload) noexcept {
						delete static_cast<OpaqueAnimationResource*>(payload);
					}});
				break;
			}
			default: break;
			}
			*entry.generationSlot = entry.nextGeneration;
			entry.destinationSlot = nullptr;
			entry.generationSlot = nullptr;
		}
		prepared.m_committed = true;
		return true;
	} catch(...) {
		// Lock acquisition and pre-publication map validation are the only
		// operations above that may throw, and both occur before the first swap.
		prepared.m_state->UnlockPublicationLocks();
		return false;
	}
}

void ResourceManager::FinalizeCommittedAnimationAssetReloadBatch(
	PreparedAnimationAssetReloadBatch& prepared) noexcept
{
	if(!prepared.m_committed || !prepared.m_state || prepared.m_state->owner != this
		|| prepared.m_state->diagnosticsFinalized) {
		return;
	}
	const bool standaloneCommit = prepared.m_state->OwnsPublicationLocks();
	prepared.m_state->diagnosticsFinalized = true;
	for(const auto& entry : prepared.m_state->entries) {
		try {
			RegisterResidencyResource(
				entry.assetId,
				entry.assetType,
				entry.sourceKey,
				entry.cpuBytes,
				0U,
				true,
				TerraEngine::Resource::ResidencyState::Active);
			UpdateStreamableLoadResult(
				entry.assetId,
				entry.assetType,
				entry.sourceKey,
				true,
				entry.cpuBytes,
				0U);
		} catch(...) {
			// The immutable payload and publisher are already complete. Diagnostic
			// refresh failure must never turn a successful atomic commit into a
			// misleading rollback after consumers can observe the new generation.
			TERRAENGINE_LOG(LogLevel::Warning,
				"Animation reload committed but residency diagnostics could not refresh for "
				<< TerraEngine::ToHexString(entry.assetId));
		}
	}
	if(standaloneCommit) {
		// Direct ResourceManager callers have no publisher retirement owner. Their
		// finalization is the documented protected boundary, so release locks and
		// reclaim the just-retired payloads here.
		prepared.m_state->UnlockPublicationLocks();
		// Iterate the prepared state directly after unlocking. Allocating a temporary
		// list here would violate this noexcept post-publication path and could
		// terminate after the live cache had already switched generations.
		for(const auto& entry : prepared.m_state->entries) {
			std::lock_guard<std::mutex> lock(m_RetiredAnimationPayloadMutex);
			for(auto iterator = m_RetiredAnimationPayloads.begin();
				iterator != m_RetiredAnimationPayloads.end(); ) {
				if(iterator->assetId == entry.assetId
					&& iterator->cacheGeneration == entry.observedGeneration) {
					if(iterator->payload != nullptr && iterator->destroyPayload != nullptr) iterator->destroyPayload(iterator->payload);
					iterator = m_RetiredAnimationPayloads.erase(iterator);
				} else ++iterator;
			}
		}
	}
}

void ResourceManager::ReleasePreparedAnimationAssetReloadBatchLocks(
	PreparedAnimationAssetReloadBatch& prepared) noexcept
{
	if(prepared.m_state && prepared.m_state->owner == this) {
		// A first publisher publication replaces an already-resident cache payload
		// but has no old publisher generation to retire. Reclaim that key-zero
		// payload immediately after the publisher's new tables are visible.
		prepared.m_state->UnlockPublicationLocks();
		// The callback is noexcept and runs after the publisher swap, so it must not
		// allocate. The prepared entries remain owned by the caller for the whole
		// callback and can be inspected safely after their locks are released.
		for(const auto& entry : prepared.m_state->entries) {
			if(entry.publisherRetirementGeneration == 0U) {
				RetireAnimationAssetPayloadGeneration(entry.assetId, 0U);
			}
		}
	}
}

void ResourceManager::RetireAnimationAssetPayloadGeneration(const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationGeneration generation) noexcept
{
	std::lock_guard<std::mutex> lock(m_RetiredAnimationPayloadMutex);
	for(auto iterator = m_RetiredAnimationPayloads.begin();
		iterator != m_RetiredAnimationPayloads.end(); ) {
		if(iterator->assetId == id && iterator->publisherGeneration == generation) {
			if(iterator->payload != nullptr && iterator->destroyPayload != nullptr) {
				iterator->destroyPayload(iterator->payload);
			}
			iterator = m_RetiredAnimationPayloads.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void ResourceManager::ReleaseAllRetiredAnimationAssetPayloadsAtProtectedBoundary()
{
	std::lock_guard<std::mutex> lock(m_RetiredAnimationPayloadMutex);
	for(RetiredAnimationPayload& retired : m_RetiredAnimationPayloads) {
		if(retired.payload != nullptr && retired.destroyPayload != nullptr) {
			retired.destroyPayload(retired.payload);
		}
	}
	m_RetiredAnimationPayloads.clear();
}

std::size_t ResourceManager::GetRetiredAnimationPayloadCountForDiagnostics() const
{
	std::lock_guard<std::mutex> lock(m_RetiredAnimationPayloadMutex);
	return m_RetiredAnimationPayloads.size();
}

void ResourceManager::ClearAnimationRuntimeGenerations()
{
	{
		std::unique_lock lock(skeletonCache.mutex);
		skeletonCache.runtimeGenerations.clear();
	}
	{
		std::unique_lock lock(animationClipCache.mutex);
		animationClipCache.runtimeGenerations.clear();
	}
	const auto clearOpaqueLedger = [](auto& cache) {
		std::unique_lock lock(cache.mutex);
		cache.runtimeGenerations.clear();
	};
	clearOpaqueLedger(animationControllerCache);
	clearOpaqueLedger(animationMaskCache);
	clearOpaqueLedger(retargetProfileCache);
	clearOpaqueLedger(motionDatabaseCache);
	clearOpaqueLedger(ragdollProfileCache);
}

// ResourceManager::ClearCaches is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::ClearCaches()
{
	// Session teardown invalidates every cache publication, so no publisher can
	// still lawfully retain these payloads. Release the bounded old-generation
	// archive before clearing the live owners.
	ReleaseAllRetiredAnimationAssetPayloadsAtProtectedBoundary();
	// Acquire exclusive locks in a consistent order to avoid deadlocks if
	// multiple threads ever clear different caches concurrently.
	{
		std::unique_lock lock(materialCache.mutex);
		materialCache.pathToId.clear();
		materialCache.idToPath.clear();
		materialCache.resources.clear();
		materialCache.inFlight.clear();
		materialCache.inFlightTokens.clear();
	}
	{
		std::unique_lock lock(meshCache.mutex);
		meshCache.pathToId.clear();
		meshCache.idToPath.clear();
		meshCache.resources.clear();
		meshCache.inFlight.clear();
		meshCache.inFlightTokens.clear();
	}
	{
		std::unique_lock lock(skeletonCache.mutex);
		// ClearCaches is a teardown boundary for payload ownership. Keep the
		// session ledger, but advance every known publication so a retained pose
		// cannot compare equal to a later reload of the same cooked GUID.
		for(auto& [id, generation] : skeletonCache.runtimeGenerations) {
			(void)id;
			if(generation != std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max()) {
				++generation;
			}
		}
		skeletonCache.pathToId.clear();
		skeletonCache.idToPath.clear();
		skeletonCache.resources.clear();
		skeletonCache.inFlight.clear();
		skeletonCache.inFlightTokens.clear();
	}
	{
		std::unique_lock lock(animationClipCache.mutex);
		for(auto& [id, generation] : animationClipCache.runtimeGenerations) {
			(void)id;
			if(generation != std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max()) {
				++generation;
			}
		}
		animationClipCache.pathToId.clear();
		animationClipCache.idToPath.clear();
		animationClipCache.resources.clear();
		animationClipCache.inFlight.clear();
		animationClipCache.inFlightTokens.clear();
	}
	const auto clearOpaqueAnimationCache = [](auto& cache) {
		std::unique_lock lock(cache.mutex);
		for (auto& [id, generation] : cache.runtimeGenerations)
		{
			(void)id;
			if (generation != std::numeric_limits<TerraEngine::Animation::AnimationGeneration>::max())
			{
				++generation;
			}
		}
		cache.pathToId.clear();
		cache.idToPath.clear();
		cache.resources.clear();
		cache.inFlight.clear();
		cache.inFlightTokens.clear();
	};
	clearOpaqueAnimationCache(animationControllerCache);
	clearOpaqueAnimationCache(animationMaskCache);
	clearOpaqueAnimationCache(retargetProfileCache);
	clearOpaqueAnimationCache(motionDatabaseCache);
	clearOpaqueAnimationCache(ragdollProfileCache);
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
		shaderCache.inFlightTokens.clear();
	}
	{
		// Clear paired shader lookups so future installs cannot reuse stale stage combinations.
		std::unique_lock lock(shaderCollectionCache.mutex);
		shaderCollectionCache.pathToId.clear();
		shaderCollectionCache.idToPath.clear();
		shaderCollectionCache.resources.clear();
		shaderCollectionCache.inFlight.clear();
		shaderCollectionCache.inFlightTokens.clear();
	}
	{
		std::unique_lock lock(textureCache.mutex);
		textureCache.pathToId.clear();
		textureCache.idToPath.clear();
		textureCache.resources.clear();
		textureCache.inFlight.clear();
		textureCache.inFlightTokens.clear();
	}
	{
		std::unique_lock lock(physicsMaterialPaths.mutex);
		physicsMaterialPaths.pathToId.clear();
		physicsMaterialPaths.idToPath.clear();
	}
	{
		std::unique_lock lock(cookedColliderPaths.mutex);
		cookedColliderPaths.pathToId.clear();
		cookedColliderPaths.idToPath.clear();
	}
	{
		std::unique_lock pathLock(audioClipPaths.mutex);
		audioClipPaths.pathToId.clear();
		audioClipPaths.idToPath.clear();
		std::unique_lock assetLock(audioClipMutex);
		audioClipAssets.clear();
		audioClipGenerations.clear();
	}
	{
		std::unique_lock lock(terrainPaths.mutex);
		terrainPaths.pathToId.clear();
		terrainPaths.idToPath.clear();
	}
	{
		// Navigation companions are derived CPU data. Teardown drops the registry
		// and current immutable roots; in-flight workers keep prior snapshots alive
		// only through their explicit shared leases.
		std::unique_lock pathLock(navigationGeometryPaths.mutex);
		navigationGeometryPaths.pathToId.clear();
		navigationGeometryPaths.idToPath.clear();
		std::unique_lock snapshotLock(navigationGeometryMutex);
		navigationGeometrySnapshots.clear();
	}
	ClearResidencyState();
	ClearStreamingState();
	// Mesh and animation cache ownership changed as one protected teardown
	// publication, so wake binding synchronization exactly once.
	m_AnimationBindingResourceRevision.fetch_add(1U, std::memory_order_release);
}

TerraEngine::UUID ResourceManager::LoadAudioClip(const std::filesystem::path& path)
{
	if(!HasNativeExtensionForAssetType(path, AssetType::AudioClip)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Skipping non-cooked audio source; runtime requires .teaudio: " << path);
		return TerraEngine::EMPTY_UUID;
	}
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	{
		std::shared_lock pathLock(audioClipPaths.mutex);
		const auto registered = audioClipPaths.pathToId.find(key);
		if(registered != audioClipPaths.pathToId.end()) {
			std::shared_lock assetLock(audioClipMutex);
			if(audioClipAssets.contains(registered->second)) return registered->second;
		}
	}
	const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
	if(bytes.empty() || bytes.size()
		> TerraEngine::Audio::kAudioFileMaximumPayloadBytes + TerraEngine::Audio::kAudioFileHeaderSize)
		return TerraEngine::EMPTY_UUID;
	const TerraEngine::Audio::AudioFileReadResult parsed = TerraEngine::Audio::ReadAudioFile(bytes);
	if(!parsed.Succeeded()) {
		TERRAENGINE_LOG(LogLevel::Error, "Audio TEAF envelope rejected: " << resolvedPath);
		return TerraEngine::EMPTY_UUID;
	}
	std::vector<std::uint8_t> payload(parsed.payload.begin(), parsed.payload.end());
	const TerraEngine::UUID id = parsed.metadata.assetId;
	std::uint64_t generation = 1U;
	{
		std::unique_lock pathLock(audioClipPaths.mutex);
		if(const auto registered = audioClipPaths.pathToId.find(key);
			registered != audioClipPaths.pathToId.end() && registered->second != id) return TerraEngine::EMPTY_UUID;
		audioClipPaths.pathToId[key] = id;
		audioClipPaths.idToPath[id] = key;
		std::unique_lock assetLock(audioClipMutex);
		if(const auto existing = audioClipAssets.find(id); existing != audioClipAssets.end()) return id;
		generation = audioClipGenerations[id] + 1U;
		if(generation == 0U) return TerraEngine::EMPTY_UUID;
		auto asset = TerraEngine::Audio::AudioClipAsset::Create(parsed.metadata, std::move(payload), generation);
		if(!asset) return TerraEngine::EMPTY_UUID;
		audioClipGenerations[id] = generation;
		audioClipAssets[id] = std::move(asset);
	}
	RegisterResidencyResource(id, AssetType::AudioClip, key,
		static_cast<std::uint64_t>(sizeof(TerraEngine::Audio::AudioClipAsset) + parsed.payload.size()), 0U, true,
		TerraEngine::Resource::ResidencyState::Active);
	UpdateStreamableLoadResult(id, AssetType::AudioClip, key, true,
		static_cast<std::uint64_t>(sizeof(TerraEngine::Audio::AudioClipAsset) + parsed.payload.size()), 0U);
	return id;
}

bool ResourceManager::ReloadAudioClip(const TerraEngine::UUID& id)
{
	std::filesystem::path path;
	{
		std::shared_lock lock(audioClipPaths.mutex);
		const auto found = audioClipPaths.idToPath.find(id);
		if(found == audioClipPaths.idToPath.end()) return false;
		path = found->second;
	}
	// Remove no live value before parsing: a bad reimport keeps the last known
	// good immutable generation visible to every source and retained packet.
	const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
	if(bytes.empty() || bytes.size()
		> TerraEngine::Audio::kAudioFileMaximumPayloadBytes + TerraEngine::Audio::kAudioFileHeaderSize) return false;
	const auto parsed = TerraEngine::Audio::ReadAudioFile(bytes);
	if(!parsed.Succeeded() || parsed.metadata.assetId != id) return false;
	std::vector<std::uint8_t> payload(parsed.payload.begin(), parsed.payload.end());
	std::unique_lock lock(audioClipMutex);
	const std::uint64_t generation = audioClipGenerations[id] + 1U;
	auto replacement = TerraEngine::Audio::AudioClipAsset::Create(parsed.metadata, std::move(payload), generation);
	if(!replacement) return false;
	audioClipGenerations[id] = generation;
	audioClipAssets[id] = std::move(replacement);
	return true;
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
	// Animation cache removals advance the retained ledger under the same lock
	// as the payload removal. This makes delete a real tombstone publication.
	auto removeAnimationFromCache = [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		for (auto it = cache.pathToId.begin(); it != cache.pathToId.end(); ) {
			it = it->second == id ? cache.pathToId.erase(it) : std::next(it);
		}
		cache.idToPath.erase(id);
		cache.resources.erase(id);
		if(!AdvanceAnimationGenerationLocked(cache, id).has_value()) {
			TERRAENGINE_LOG(LogLevel::Error, "Animation runtime generation exhausted while deleting " << ToHexString(id));
		}
	};

	// Dispatch by explicit engine state so each case documents one supported behavior.
	switch (type) {
	case AssetType::Mesh: removeFromCache(meshCache); break;
	case AssetType::Skeleton: removeAnimationFromCache(skeletonCache); break;
	case AssetType::AnimationClip: removeAnimationFromCache(animationClipCache); break;
	case AssetType::AnimationController: removeAnimationFromCache(animationControllerCache); break;
	case AssetType::AnimationMask: removeAnimationFromCache(animationMaskCache); break;
	case AssetType::RetargetProfile: removeAnimationFromCache(retargetProfileCache); break;
	case AssetType::MotionDatabase: removeAnimationFromCache(motionDatabaseCache); break;
	case AssetType::RagdollProfile: removeAnimationFromCache(ragdollProfileCache); break;
	case AssetType::Texture: removeFromCache(textureCache); break;
	case AssetType::Material: removeFromCache(materialCache); break;
	case AssetType::ShaderCollection: removeFromCache(shaderCollectionCache); break;
	case AssetType::PhysicsMaterial:
	case AssetType::CookedCollider: {
		AssetPathRegistry& registry = type == AssetType::PhysicsMaterial
			? physicsMaterialPaths : cookedColliderPaths;
		std::unique_lock lock(registry.mutex);
		for(auto iterator = registry.pathToId.begin(); iterator != registry.pathToId.end();) {
			iterator = iterator->second == id ? registry.pathToId.erase(iterator) : std::next(iterator);
		}
		registry.idToPath.erase(id);
		break;
	}
	case AssetType::AudioClip: {
		std::unique_lock lock(audioClipPaths.mutex);
		for(auto iterator = audioClipPaths.pathToId.begin(); iterator != audioClipPaths.pathToId.end();) {
			iterator = iterator->second == id ? audioClipPaths.pathToId.erase(iterator) : std::next(iterator);
		}
		audioClipPaths.idToPath.erase(id);
		std::unique_lock assetLock(audioClipMutex);
		audioClipAssets.erase(id);
		audioClipGenerations.erase(id);
		break;
	}
	case AssetType::Terrain: {
		std::unique_lock lock(terrainPaths.mutex);
		for(auto iterator = terrainPaths.pathToId.begin(); iterator != terrainPaths.pathToId.end();) {
			iterator = iterator->second == id ? terrainPaths.pathToId.erase(iterator) : std::next(iterator);
		}
		terrainPaths.idToPath.erase(id);
		break;
	}
	}
	if(type == AssetType::Mesh) {
		// Deletion is a tombstone publication for animator bindings: retained
		// runtime handles must observe that their immutable dependency vanished.
		m_AnimationBindingResourceRevision.fetch_add(1U, std::memory_order_release);
	}

	if (type == AssetType::Mesh)
	{
		std::unique_lock lock(m_MeshLocalBoundsMutex);
		m_MeshLocalBoundsCache.erase(id);
	}

	// Physics assets have no renderer-owned object or descriptor to retire.
	if(type != AssetType::PhysicsMaterial && type != AssetType::CookedCollider && type != AssetType::AudioClip
		&& type != AssetType::Skeleton && type != AssetType::AnimationClip
		&& type != AssetType::AnimationController && type != AssetType::AnimationMask
		&& type != AssetType::RetargetProfile && type != AssetType::MotionDatabase
		&& type != AssetType::RagdollProfile)
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
		// Residency ownership mirrors the real cache payload. Animation objects
		// are typed parsed assets (or typed immutable envelopes), never decoded
		// meshes, so diagnostics and release accounting must preserve their class.
		switch (type)
		{
		case AssetType::Mesh:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::DecodedMesh;
			break;
		case AssetType::Texture:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::TexturePixels;
			break;
		case AssetType::Material:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::MaterialMetadata;
			break;
		case AssetType::ShaderCollection:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::ShaderCollection;
			break;
		case AssetType::Skeleton:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::Skeleton;
			break;
		case AssetType::AnimationClip:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::AnimationClip;
			break;
		case AssetType::AnimationController:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::AnimationController;
			break;
		case AssetType::AnimationMask:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::AnimationMask;
			break;
		case AssetType::RetargetProfile:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::RetargetProfile;
			break;
		case AssetType::MotionDatabase:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::MotionDatabase;
			break;
		case AssetType::RagdollProfile:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::RagdollProfile;
			break;
		case AssetType::AudioClip:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::AudioClip;
			break;
		case AssetType::PhysicsMaterial:
		case AssetType::CookedCollider:
			cpuRecord.payloadKind = TerraEngine::Resource::CpuPayloadOwnershipKind::Unknown;
			break;
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
			if (!cache.resources.contains(id))
			{
				return;
			}
			if (IsAnimationAssetType(type) && !AdvanceAnimationGenerationLocked(cache, id).has_value())
			{
				// Generation exhaustion fails closed: retaining memory is safer than
				// making a stale animation handle alias a future reload.
				return;
			}
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

bool ResourceManager::ReleaseStreamableAssetForRuntime(
	const TerraEngine::UUID& id,
	AssetType type,
	std::uint64_t frameId)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return false;
	}
	return ReleaseResidentResource(
		id,
		type,
		TerraEngine::Resource::ResidencyReleaseReason::ExplicitRuntimeUnload,
		TerraEngine::Resource::ResidencyReleaseDomain::CpuAndGpu,
		frameId,
		0U);
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
	std::int32_t priority,
	const TerraEngine::UUID& ownerObjectId)
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
	// Scene-owned runtime needs must remain distinct: two animators can share an
	// asset, and removing one component must not release the other's protection.
	need.needId = ownerObjectId == TerraEngine::EMPTY_UUID
		? std::string(TerraEngine::Resource::ToString(needType))
		: std::string("animator:") + ToHexString(ownerObjectId);
	need.assetId = id;
	need.needType = needType;
	need.objectId = ownerObjectId;
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
	// Replace an earlier publication from the same owner before merging. Keeping
	// the old record could retain its earlier expiry time and make an actively
	// refreshed dependency appear idle under memory pressure.
	record.needSources.erase(std::remove_if(
		record.needSources.begin(), record.needSources.end(), [&need](const auto& existing) {
			return existing.assetId == need.assetId
				&& existing.needType == need.needType
				&& existing.needId == need.needId
				&& existing.objectId == need.objectId;
		}), record.needSources.end());
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

std::size_t ResourceManager::ReleaseStreamingNeedsForObject(
	const TerraEngine::UUID& ownerObjectId,
	TerraEngine::Resource::StreamingNeedType needType)
{
	if (ownerObjectId == TerraEngine::EMPTY_UUID)
	{
		return 0u;
	}

	const double nowSeconds = ResidencyClockSeconds();
	std::size_t releasedCount = 0u;
	std::unique_lock lock(m_StreamingMutex);
	for (auto& [assetId, record] : m_StreamableAssets)
	{
		(void)assetId;
		const std::size_t priorCount = record.needSources.size();
		record.needSources.erase(std::remove_if(
			record.needSources.begin(), record.needSources.end(),
			[&](const TerraEngine::Resource::StreamingNeed& need) {
				return need.objectId == ownerObjectId && need.needType == needType;
			}), record.needSources.end());
		releasedCount += priorCount - record.needSources.size();
		if (record.needSources.size() != priorCount)
		{
			record.needSources = TerraEngine::Resource::MergeStreamingNeeds(
				std::move(record.needSources), nowSeconds);
			record.protectionReason = ComputeStreamingProtection(record.needSources, m_StreamingPolicy);
		}
	}
	return releasedCount;
}

void ResourceManager::PublishSceneRegionAssetNeeds(
	std::span<const TerraEngine::Resource::SceneRegionAssetNeed> needs,
	std::uint64_t frameId)
{
	for (const TerraEngine::Resource::SceneRegionAssetNeed& need : needs)
	{
		if (need.assetId == TerraEngine::EMPTY_UUID) continue;
		const AssetType type = need.assetType == TerraEngine::Resource::StreamableAssetType::Texture ? AssetType::Texture
			: (need.assetType == TerraEngine::Resource::StreamableAssetType::Material ? AssetType::Material
				: (need.assetType == TerraEngine::Resource::StreamableAssetType::ShaderCollection ? AssetType::ShaderCollection : AssetType::Mesh));
		MarkStreamingNeed(need.assetId, type, TerraEngine::Resource::StreamingNeedType::RuntimeExplicit,
			frameId, need.regionId.empty() ? "resident world region" : need.regionId, need.priority, need.ownerObjectId);
	}
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
	std::string_view reason,
	const TerraEngine::UUID& ownerObjectId)
{
	MarkStreamingNeed(id, type, needType, frameId, reason,
		needType == TerraEngine::Resource::StreamingNeedType::Visible ? 100 : 10,
		ownerObjectId);

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
	case AssetType::Skeleton:
	case AssetType::AnimationClip:
	case AssetType::AnimationController:
	case AssetType::AnimationMask:
	case AssetType::RetargetProfile:
	case AssetType::MotionDatabase:
	case AssetType::RagdollProfile:
		return LoadAnimationAssetAsync(sourcePath, type);
	case AssetType::Texture:
		return LoadTextureAsync(sourcePath);
	case AssetType::Material:
		return LoadMaterialAsync(sourcePath);
	case AssetType::ShaderCollection:
		return MakeReadyFuture(id);
	}
	return MakeReadyFuture(TerraEngine::EMPTY_UUID);
}

bool ResourceManager::CancelStreamableLoad(const TerraEngine::UUID& id, AssetType type)
{
	if (id == TerraEngine::EMPTY_UUID)
	{
		return false;
	}
	bool cancelled = false;
	std::string sourceKey;
	VisitAssetCache(type, [&](auto& cache) {
		std::unique_lock lock(cache.mutex);
		const auto path = cache.idToPath.find(id);
		if (path == cache.idToPath.end())
		{
			return;
		}
		sourceKey = path->second;
		if (!cache.inFlight.contains(sourceKey))
		{
			return;
		}
		// Removing the token is the cancellation publication. The shared future
		// remains valid for existing waiters, but its worker can no longer publish
		// into this cache and a later request may install a fresh token/future.
		cache.inFlightTokens.erase(sourceKey);
		cache.inFlight.erase(sourceKey);
		cancelled = true;
	});
	if (!cancelled)
	{
		return false;
	}
	{
		std::unique_lock lock(m_StreamingMutex);
		if (auto record = m_StreamableAssets.find(id); record != m_StreamableAssets.end())
		{
			record->second.payloadState = TerraEngine::Resource::StreamablePayloadState::Deferred;
			record->second.failureReason = "Asynchronous load cancelled before publication.";
		}
	}
	{
		std::unique_lock lock(m_ResidencyMutex);
		if (auto record = m_ResidentAssets.find(id); record != m_ResidentAssets.end())
		{
			record->second.inFlight = false;
			record->second.lastTouchedReason = "streaming load cancelled";
		}
	}
	return true;
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

	const auto hasRegisteredPath = [&](const AssetPathRegistry& registry) {
		std::shared_lock lock(registry.mutex);
		return registry.idToPath.contains(id);
	};
	return isAvailable(meshCache) || isAvailable(textureCache) || isAvailable(materialCache) ||
		isAvailable(shaderCollectionCache) || isAvailable(shaderCache)
		|| isAvailable(skeletonCache) || isAvailable(animationClipCache)
		|| isAvailable(animationControllerCache) || isAvailable(animationMaskCache)
		|| isAvailable(retargetProfileCache) || isAvailable(motionDatabaseCache)
		|| isAvailable(ragdollProfileCache)
		|| hasRegisteredPath(physicsMaterialPaths) || hasRegisteredPath(cookedColliderPaths)
		|| hasRegisteredPath(audioClipPaths) || hasRegisteredPath(terrainPaths)
		|| hasRegisteredPath(navigationGeometryPaths);
}

// ResourceManager::RegisterAssetPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ResourceManager::RegisterAssetPath(const TerraEngine::UUID& id, const std::filesystem::path& path, AssetType type)
{
	const std::filesystem::path resolvedPath = ResolveAssetPath(path);
	const std::string key = NormalizeKey(resolvedPath);
	const bool canBePreferredLoadPath = HasNativeExtensionForAssetType(resolvedPath, type);
	if(type == AssetType::PhysicsMaterial || type == AssetType::CookedCollider || type == AssetType::AudioClip || type == AssetType::Terrain) {
		AssetPathRegistry& registry = type == AssetType::PhysicsMaterial
			? physicsMaterialPaths : (type == AssetType::CookedCollider ? cookedColliderPaths : (type == AssetType::AudioClip ? audioClipPaths : terrainPaths));
		std::unique_lock lock(registry.mutex);
		registry.pathToId[key] = id;
		if(canBePreferredLoadPath) registry.idToPath[id] = key;
		return;
	}

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

std::filesystem::path ResourceManager::GetRegisteredAssetPath(const TerraEngine::UUID& id, AssetType type) const
{
	if(id == TerraEngine::EMPTY_UUID) return {};
	if(type == AssetType::PhysicsMaterial || type == AssetType::CookedCollider || type == AssetType::AudioClip || type == AssetType::Terrain) {
		const AssetPathRegistry& registry = type == AssetType::PhysicsMaterial
			? physicsMaterialPaths : (type == AssetType::CookedCollider ? cookedColliderPaths : (type == AssetType::AudioClip ? audioClipPaths : terrainPaths));
		std::shared_lock lock(registry.mutex);
		const auto found = registry.idToPath.find(id);
		return found != registry.idToPath.end() ? std::filesystem::path(found->second) : std::filesystem::path{};
	}
	std::filesystem::path path;
	const auto capture = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		const auto found = cache.idToPath.find(id);
		if(found != cache.idToPath.end()) path = found->second;
	};
	switch(type) {
	case AssetType::Mesh: capture(meshCache); break;
	case AssetType::Skeleton: capture(skeletonCache); break;
	case AssetType::AnimationClip: capture(animationClipCache); break;
	case AssetType::AnimationController: capture(animationControllerCache); break;
	case AssetType::AnimationMask: capture(animationMaskCache); break;
	case AssetType::RetargetProfile: capture(retargetProfileCache); break;
	case AssetType::MotionDatabase: capture(motionDatabaseCache); break;
	case AssetType::RagdollProfile: capture(ragdollProfileCache); break;
	case AssetType::Texture: capture(textureCache); break;
	case AssetType::Material: capture(materialCache); break;
	case AssetType::ShaderCollection: capture(shaderCollectionCache); break;
	case AssetType::PhysicsMaterial:
	case AssetType::CookedCollider: break;
	}
	return path;
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
		case AssetType::Skeleton:
			return LoadSkeleton(candidatePath);
		case AssetType::AnimationClip:
			return LoadAnimationClip(candidatePath);
		case AssetType::AudioClip:
			return LoadAudioClip(candidatePath);
		case AssetType::AnimationController:
		case AssetType::AnimationMask:
		case AssetType::RetargetProfile:
		case AssetType::MotionDatabase:
		case AssetType::RagdollProfile:
			return LoadOpaqueAnimationAsset(candidatePath, type);
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
	if(type == AssetType::PhysicsMaterial || type == AssetType::CookedCollider || type == AssetType::AudioClip || type == AssetType::Terrain) {
		const AssetPathRegistry& registry = type == AssetType::PhysicsMaterial
			? physicsMaterialPaths : (type == AssetType::CookedCollider ? cookedColliderPaths : (type == AssetType::AudioClip ? audioClipPaths : terrainPaths));
		resolvedId = tryFindRegisteredMatch(registry);
		return resolvedId;
	}
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
	case AssetType::Skeleton:
		capturePath(skeletonCache);
		break;
	case AssetType::AnimationClip:
		capturePath(animationClipCache);
		break;
	case AssetType::AnimationController:
		capturePath(animationControllerCache);
		break;
	case AssetType::AnimationMask:
		capturePath(animationMaskCache);
		break;
	case AssetType::RetargetProfile:
		capturePath(retargetProfileCache);
		break;
	case AssetType::MotionDatabase:
		capturePath(motionDatabaseCache);
		break;
	case AssetType::RagdollProfile:
		capturePath(ragdollProfileCache);
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
	case AssetType::PhysicsMaterial: {
		std::shared_lock lock(physicsMaterialPaths.mutex);
		const auto found = physicsMaterialPaths.idToPath.find(id);
		if(found != physicsMaterialPaths.idToPath.end()) result = found->second;
		break;
	}
	case AssetType::CookedCollider: {
		std::shared_lock lock(cookedColliderPaths.mutex);
		const auto found = cookedColliderPaths.idToPath.find(id);
		if(found != cookedColliderPaths.idToPath.end()) result = found->second;
		break;
	}
	case AssetType::AudioClip: {
		std::shared_lock lock(audioClipPaths.mutex);
		const auto found = audioClipPaths.idToPath.find(id);
		if(found != audioClipPaths.idToPath.end()) result = found->second;
		break;
	}
	case AssetType::Terrain: {
		std::shared_lock lock(terrainPaths.mutex);
		const auto found = terrainPaths.idToPath.find(id);
		if(found != terrainPaths.idToPath.end()) result = found->second;
		break;
	}
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

std::optional<MeshAnimationBindingSnapshot>
ResourceManager::TryCopyMeshAnimationBindingSnapshot(
	const TerraEngine::UUID& id) const
{
	MeshAnimationBindingSnapshot snapshot;
	if(id == TerraEngine::EMPTY_UUID) return snapshot;
	std::shared_lock lock(meshCache.mutex, std::try_to_lock);
	if(!lock.owns_lock()) return std::nullopt;
	const auto found = meshCache.resources.find(id);
	if(found == meshCache.resources.end() || !found->second) return snapshot;

	// Copy only the immutable compatibility facts consumed by animation. Large
	// vertex, index, morph-delta, and virtual-geometry streams remain cache-owned
	// and are never duplicated at the protected frame boundary.
	const Mesh& mesh = *found->second;
	const TemfDeformableMetadata& metadata = mesh.deformableMetadata;
	snapshot.present = true;
	snapshot.hasDeformablePayload = mesh.hasDeformablePayload;
	snapshot.runtimeLayoutVersion = mesh.runtimeLayout.version;
	snapshot.deformablePayloadGeneration = mesh.deformablePayloadGeneration;
	snapshot.skinningVertexCount = metadata.skinningVertexCount;
	snapshot.meshletSkinningDataCount = mesh.runtimeLayout.meshletSkinningDataCount;
	snapshot.meshletSkinningDataBytes = mesh.meshletSkinningData.size();
	snapshot.palettePartitionCount = metadata.palettePartitionCount;
	snapshot.paletteRecordCount = mesh.deformablePalettes.size();
	snapshot.singlePaletteJointCount = mesh.deformablePalettes.size() == 1U
		? mesh.deformablePalettes.front().jointCount : 0U;
	snapshot.morphTargetCount = metadata.morphTargetCount;
	snapshot.morphRecordCount = mesh.deformableMorphs.size();
	snapshot.vertexPaletteIndexCount =
		mesh.deformableVertexPaletteIndices.size();
	snapshot.sourceSkeletonJointCount = metadata.sourceSkeletonJointCount;
	snapshot.skeletonCompatibilityFingerprint =
		metadata.skeletonCompatibilityFingerprint;
	snapshot.flags = metadata.flags;
	std::copy_n(metadata.deformationBoundsMin,
		snapshot.deformationBoundsMinimum.size(),
		snapshot.deformationBoundsMinimum.begin());
	std::copy_n(metadata.deformationBoundsMax,
		snapshot.deformationBoundsMaximum.size(),
		snapshot.deformationBoundsMaximum.begin());
	snapshot.paletteJoints = mesh.deformablePaletteJoints;
	snapshot.morphStableIds.reserve(mesh.deformableMorphs.size());
	for(const TemfDeformableMorphRecord& morph : mesh.deformableMorphs) {
		snapshot.morphStableIds.push_back(morph.stableTargetId);
	}
	return snapshot;
}

TerraEngine::Animation::SkeletonAsset* ResourceManager::GetSkeleton(const TerraEngine::UUID& id)
{
	return GetResourceById<TerraEngine::Animation::SkeletonAsset>(
		id, skeletonCache, [this](const std::filesystem::path& path) { return LoadSkeleton(path); });
}

TerraEngine::Animation::AnimationClipAsset* ResourceManager::GetAnimationClip(const TerraEngine::UUID& id)
{
	return GetResourceById<TerraEngine::Animation::AnimationClipAsset>(
		id, animationClipCache, [this](const std::filesystem::path& path) { return LoadAnimationClip(path); });
}

TerraEngine::Animation::AnimationGeneration ResourceManager::GetAnimationAssetRuntimeGeneration(
	const TerraEngine::UUID& id,
	AssetType type) const noexcept
{
	if(id == TerraEngine::EMPTY_UUID) return 0U;
	const auto readGeneration = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		const auto found = cache.runtimeGenerations.find(id);
		return found != cache.runtimeGenerations.end() ? found->second : 0U;
	};
	switch(type) {
	case AssetType::Skeleton: return readGeneration(skeletonCache);
	case AssetType::AnimationClip: return readGeneration(animationClipCache);
	case AssetType::AnimationController: return readGeneration(animationControllerCache);
	case AssetType::AnimationMask: return readGeneration(animationMaskCache);
	case AssetType::RetargetProfile: return readGeneration(retargetProfileCache);
	case AssetType::MotionDatabase: return readGeneration(motionDatabaseCache);
	case AssetType::RagdollProfile: return readGeneration(ragdollProfileCache);
	default: return 0U;
	}
}

std::shared_ptr<const TerraEngine::Audio::AudioClipAsset> ResourceManager::GetAudioClip(const TerraEngine::UUID& id)
{
	if(id == TerraEngine::EMPTY_UUID) return {};
	{
		std::shared_lock lock(audioClipMutex);
		if(const auto found = audioClipAssets.find(id); found != audioClipAssets.end()) return found->second;
	}
	std::filesystem::path path;
	{
		std::shared_lock lock(audioClipPaths.mutex);
		if(const auto found = audioClipPaths.idToPath.find(id); found != audioClipPaths.idToPath.end()) path = found->second;
	}
	if(path.empty() || LoadAudioClip(path) == TerraEngine::EMPTY_UUID) return {};
	std::shared_lock lock(audioClipMutex);
	const auto found = audioClipAssets.find(id);
	return found != audioClipAssets.end() ? found->second : std::shared_ptr<const TerraEngine::Audio::AudioClipAsset>{};
}

std::optional<TerraEngine::Animation::AnimationGeneration>
ResourceManager::TryGetAnimationAssetRuntimeGeneration(
	const TerraEngine::UUID& id,
	AssetType type) const noexcept
{
	if(id == TerraEngine::EMPTY_UUID) return 0U;
	const auto tryReadGeneration = [&](const auto& cache)
		-> std::optional<TerraEngine::Animation::AnimationGeneration> {
		std::shared_lock lock(cache.mutex, std::try_to_lock);
		if(!lock.owns_lock()) return std::nullopt;
		const auto found = cache.runtimeGenerations.find(id);
		return found != cache.runtimeGenerations.end() ? found->second : 0U;
	};
	switch(type) {
	case AssetType::Skeleton: return tryReadGeneration(skeletonCache);
	case AssetType::AnimationClip: return tryReadGeneration(animationClipCache);
	case AssetType::AnimationController: return tryReadGeneration(animationControllerCache);
	case AssetType::AnimationMask: return tryReadGeneration(animationMaskCache);
	case AssetType::RetargetProfile: return tryReadGeneration(retargetProfileCache);
	case AssetType::MotionDatabase: return tryReadGeneration(motionDatabaseCache);
	case AssetType::RagdollProfile: return tryReadGeneration(ragdollProfileCache);
	default: return 0U;
	}
}

bool ResourceManager::IsAnimationAssetPublished(
	const TerraEngine::UUID& id,
	AssetType type) const noexcept
{
	if(id == TerraEngine::EMPTY_UUID || !IsAnimationAssetType(type)) return false;
	const auto isPublished = [&](const auto& cache) {
		std::shared_lock lock(cache.mutex);
		const auto resource = cache.resources.find(id);
		const auto generation = cache.runtimeGenerations.find(id);
		return resource != cache.resources.end() && resource->second
			&& generation != cache.runtimeGenerations.end() && generation->second != 0U;
	};
	switch(type) {
	case AssetType::Skeleton: return isPublished(skeletonCache);
	case AssetType::AnimationClip: return isPublished(animationClipCache);
	case AssetType::AnimationController: return isPublished(animationControllerCache);
	case AssetType::AnimationMask: return isPublished(animationMaskCache);
	case AssetType::RetargetProfile: return isPublished(retargetProfileCache);
	case AssetType::MotionDatabase: return isPublished(motionDatabaseCache);
	case AssetType::RagdollProfile: return isPublished(ragdollProfileCache);
	default: return false;
	}
}

bool ResourceManager::CopySkeletonPublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::SkeletonAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(skeletonCache.mutex);
	const auto resource = skeletonCache.resources.find(id);
	const auto publishedGeneration = skeletonCache.runtimeGenerations.find(id);
	if(resource == skeletonCache.resources.end() || publishedGeneration == skeletonCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || resource->second->runtimeGeneration != publishedGeneration->second) {
		return false;
	}

	// Copying under the same lock as the generation lookup forms an immutable
	// boundary for workers even if a hot reload publishes immediately after it.
	output = *resource->second;
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyAnimationClipPublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationClipAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(animationClipCache.mutex);
	const auto resource = animationClipCache.resources.find(id);
	const auto publishedGeneration = animationClipCache.runtimeGenerations.find(id);
	if(resource == animationClipCache.resources.end() || publishedGeneration == animationClipCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || resource->second->runtimeGeneration != publishedGeneration->second) {
		return false;
	}
	// See CopySkeletonPublication: callers receive a matched payload and
	// generation pair rather than a raw cache pointer that reload can replace.
	output = *resource->second;
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyAnimationControllerPublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationControllerAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(animationControllerCache.mutex);
	const auto resource = animationControllerCache.resources.find(id);
	const auto publishedGeneration = animationControllerCache.runtimeGenerations.find(id);
	if(resource == animationControllerCache.resources.end()
		|| publishedGeneration == animationControllerCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || !resource->second) {
		return false;
	}

	// Deserialize while the matching opaque payload and session generation are
	// protected by one cache lock. A reload can therefore never pair new bytes
	// with the generation that belonged to the retired controller publication.
	TerraEngine::Animation::AnimationControllerAsset candidate;
	if(TerraEngine::Animation::DeserializeAnimationControllerAsset(
		resource->second->bytes, 0U, candidate) != TerraEngine::Animation::AnimationStatus::Ok) {
		return false;
	}
	output = std::move(candidate);
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyAnimationMaskPublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationMaskAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(animationMaskCache.mutex);
	const auto resource = animationMaskCache.resources.find(id);
	const auto publishedGeneration = animationMaskCache.runtimeGenerations.find(id);
	if(resource == animationMaskCache.resources.end()
		|| publishedGeneration == animationMaskCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || !resource->second) {
		return false;
	}

	// Masks use the same immutable opaque-cache publication contract as graph
	// documents. The caller receives a value object and cannot retain cache bytes.
	TerraEngine::Animation::AnimationMaskAsset candidate;
	if(TerraEngine::Animation::DeserializeAnimationMaskAsset(
		resource->second->bytes, 0U, candidate) != TerraEngine::Animation::AnimationStatus::Ok) {
		return false;
	}
	output = std::move(candidate);
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyRetargetProfilePublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::RetargetProfileAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(retargetProfileCache.mutex);
	const auto resource = retargetProfileCache.resources.find(id);
	const auto publishedGeneration = retargetProfileCache.runtimeGenerations.find(id);
	if(resource == retargetProfileCache.resources.end()
		|| publishedGeneration == retargetProfileCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || !resource->second) {
		return false;
	}
	TerraEngine::Animation::RetargetProfileAsset candidate;
	if(TerraEngine::Animation::DeserializeRetargetProfileAsset(
		resource->second->bytes, 0U, candidate) != TerraEngine::Animation::AnimationStatus::Ok) {
		return false;
	}
	candidate.runtimeGeneration = publishedGeneration->second;
	output = std::move(candidate);
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyMotionDatabasePublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::MotionDatabaseAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(motionDatabaseCache.mutex);
	const auto resource = motionDatabaseCache.resources.find(id);
	const auto publishedGeneration = motionDatabaseCache.runtimeGenerations.find(id);
	if(resource == motionDatabaseCache.resources.end()
		|| publishedGeneration == motionDatabaseCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || !resource->second) {
		return false;
	}
	TerraEngine::Animation::MotionDatabaseAsset candidate;
	if(TerraEngine::Animation::DeserializeMotionDatabaseAsset(
		resource->second->bytes, 0U, candidate) != TerraEngine::Animation::AnimationStatus::Ok) {
		return false;
	}
	candidate.runtimeGeneration = publishedGeneration->second;
	output = std::move(candidate);
	generation = publishedGeneration->second;
	return true;
}

bool ResourceManager::CopyRagdollProfilePublication(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::RagdollProfileAsset& output,
	TerraEngine::Animation::AnimationGeneration& generation) const
{
	std::shared_lock lock(ragdollProfileCache.mutex);
	const auto resource = ragdollProfileCache.resources.find(id);
	const auto publishedGeneration = ragdollProfileCache.runtimeGenerations.find(id);
	if(resource == ragdollProfileCache.resources.end()
		|| publishedGeneration == ragdollProfileCache.runtimeGenerations.end()
		|| publishedGeneration->second == 0U || !resource->second) {
		return false;
	}
	TerraEngine::Animation::RagdollProfileAsset candidate;
	if(TerraEngine::Animation::DeserializeRagdollProfileAsset(
		resource->second->bytes, 0U, candidate) != TerraEngine::Animation::AnimationStatus::Ok) {
		return false;
	}
	candidate.runtimeGeneration = publishedGeneration->second;
	output = std::move(candidate);
	generation = publishedGeneration->second;
	return true;
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
	const std::filesystem::path logicalPath = absolutePath.lexically_relative(assetRoot);
	const std::string logicalPathString = ContentMount::NormalizeLogicalPath(logicalPath.generic_string());

	// A packaged animation asset has exactly one authority: the active TEPAK.
	// Failing closed here prevents a missing/corrupt archive entry from silently
	// loading a developer's source or build-tree copy with different contents.
	if (IsExportRuntime() && IsNativeAnimationAssetPath(absolutePath))
	{
		if (!ContentMount::Instance().IsMounted())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Packaged animation read rejected because no export archive is mounted: " << logicalPathString);
			return {};
		}
		const TerraEngine::ResolvedAnimationContent content =
			TerraEngine::AnimationContentResolver::Resolve(ContentMount::Instance(), logicalPathString);
		if (!content.Succeeded())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Packaged animation entry is missing or has the wrong cooked class: " << logicalPathString);
			return {};
		}
		const TerraEngine::AnimationContentReadResult read =
			TerraEngine::AnimationContentResolver::ReadRange(
				ContentMount::Instance(), logicalPathString, 0U, content.logicalSize, content.logicalSize);
		return read.Succeeded() ? read.bytes : std::vector<std::uint8_t>{};
	}

	// Packaged audio follows the same single-authority rule as animation. The
	// archive class and complete TEAF bytes are checked before publication, and
	// an exported runtime never substitutes a developer's loose cooked file.
	if (IsExportRuntime() && IsNativeAudioAssetPath(absolutePath))
	{
		if (!ContentMount::Instance().IsMounted())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Packaged audio read rejected because no export archive is mounted: " << logicalPathString);
			return {};
		}
		const std::optional<MountedContentEntry> entry = ContentMount::Instance().FindEntry(logicalPathString);
		if (!entry || entry->assetClass != "audio")
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Packaged audio entry is missing or has the wrong cooked class: " << logicalPathString);
			return {};
		}
		const std::uint64_t maximumBytes = TerraEngine::Audio::kAudioFileMaximumPayloadBytes
			+ TerraEngine::Audio::kAudioFileHeaderSize;
		MountedContentReadResult read = ContentMount::Instance().ReadAllChecked(logicalPathString, maximumBytes);
		if (!read.Succeeded() || !TerraEngine::Audio::ReadAudioFile(read.bytes).Succeeded())
		{
			TERRAENGINE_LOG(LogLevel::Error,
				"Packaged audio entry failed bounded TEAF validation: " << logicalPathString);
			return {};
		}
		return std::move(read.bytes);
	}

	// In export builds, the ContentMount provides access to assets packed into 
	// a .tepak archive. If a mount is active, we attempt to read through it 
	// using a logical path relative to the asset root.
	if (ContentMount::Instance().IsMounted())
	{
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

bool ResourceManager::ReadFileBytesInto(
	const std::filesystem::path& path,
	std::vector<std::uint8_t>& output) const
{
	const std::filesystem::path absolutePath = ResolveAssetPath(path);
	// Mounted and exported content has additional authority and validation rules.
	// Preserve the established owned-result implementation for those uncommon
	// paths; repeated loose editor reads use this buffer directly and retain it.
	if (IsExportRuntime() || ContentMount::Instance().IsMounted()) {
		output = ReadFileBytes(path);
		return !output.empty() || std::filesystem::exists(absolutePath);
	}
	if (!std::filesystem::exists(absolutePath) || std::filesystem::is_directory(absolutePath)) {
		output.clear();
		return false;
	}

	std::ifstream file(absolutePath, std::ios::binary | std::ios::ate);
	if (!file) {
		output.clear();
		return false;
	}
	const std::streamsize length = file.tellg();
	if (length < 0) {
		output.clear();
		return false;
	}
	output.resize(static_cast<std::size_t>(length));
	if (length == 0) return true;
	file.seekg(0, std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(output.data()), length)) {
		output.clear();
		return false;
	}
	return true;
}

AnimationRangeReadResult ResourceManager::ReadAnimationAssetRange(
	const std::filesystem::path& path,
	std::uint64_t offset,
	std::uint64_t size,
	std::uint64_t maximumReadBytes) const
{
	AnimationRangeReadResult result;
	result.resolvedPath = ResolveAssetPath(path);
	if (!IsNativeAnimationAssetPath(result.resolvedPath) || size == 0U
		|| maximumReadBytes == 0U || size > maximumReadBytes
		|| offset > std::numeric_limits<std::uint64_t>::max() - size)
	{
		result.status = size > maximumReadBytes
			? AnimationRangeReadStatus::ReadLimitExceeded
			: AnimationRangeReadStatus::InvalidRequest;
		result.message = "Animation range request has an invalid type, size, limit, or overflowing offset.";
		return result;
	}

	const std::string logicalPath = ContentMount::NormalizeLogicalPath(
		result.resolvedPath.lexically_relative(assetRoot).generic_string());
	if (ContentMount::Instance().IsMounted())
	{
		const TerraEngine::AnimationContentReadResult mounted =
			TerraEngine::AnimationContentResolver::ReadRange(
				ContentMount::Instance(), logicalPath, offset, size, maximumReadBytes);
		if (mounted.Succeeded())
		{
			result.status = AnimationRangeReadStatus::Ok;
			result.bytes = mounted.bytes;
			result.message = "Read the complete bounded animation range from the mounted archive.";
			return result;
		}

		// Export runtimes never try a second authority. Editor sessions may have a
		// diagnostic archive mounted while still iterating on loose cooked files.
		if (IsExportRuntime())
		{
			switch (mounted.content.status)
			{
			case TerraEngine::AnimationContentStatus::NotFound:
			case TerraEngine::AnimationContentStatus::NotMounted:
				result.status = AnimationRangeReadStatus::NotFound;
				break;
			case TerraEngine::AnimationContentStatus::OutOfRange:
				result.status = AnimationRangeReadStatus::OutOfRange;
				break;
			case TerraEngine::AnimationContentStatus::ReadLimitExceeded:
				result.status = AnimationRangeReadStatus::ReadLimitExceeded;
				break;
			case TerraEngine::AnimationContentStatus::NotAnimationAsset:
			case TerraEngine::AnimationContentStatus::ForbiddenRuntimePath:
				result.status = AnimationRangeReadStatus::NotAnimationAsset;
				break;
			case TerraEngine::AnimationContentStatus::ArchiveError:
			case TerraEngine::AnimationContentStatus::Ok:
				result.status = AnimationRangeReadStatus::IoError;
				break;
			}
			result.message = "Mounted animation range failed closed; no loose-file fallback was attempted.";
			return result;
		}
	}
	else if (IsExportRuntime())
	{
		result.status = AnimationRangeReadStatus::NotFound;
		result.message = "The configured export archive is not mounted; loose animation reads are forbidden.";
		return result;
	}

	std::ifstream file(result.resolvedPath, std::ios::binary | std::ios::ate);
	if (!file)
	{
		result.status = AnimationRangeReadStatus::NotFound;
		result.message = "The loose cooked animation file could not be opened.";
		return result;
	}
	const std::streamoff fileLength = file.tellg();
	if (fileLength < 0 || static_cast<std::uint64_t>(fileLength) < offset
		|| size > static_cast<std::uint64_t>(fileLength) - offset)
	{
		result.status = AnimationRangeReadStatus::OutOfRange;
		result.message = "The requested animation range is outside the cooked file.";
		return result;
	}
	if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
		|| size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
	{
		result.status = AnimationRangeReadStatus::ReadLimitExceeded;
		result.message = "The requested animation range cannot be represented by this runtime.";
		return result;
	}

	result.bytes.resize(static_cast<std::size_t>(size));
	file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
	if (!file.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(size)))
	{
		result.bytes.clear();
		result.status = AnimationRangeReadStatus::IoError;
		result.message = "The loose cooked animation range could not be read completely.";
		return result;
	}
	result.status = AnimationRangeReadStatus::Ok;
	result.message = "Read the complete bounded animation range from a loose editor artifact.";
	return result;
}

AnimationCookedRangeTable ResourceManager::DescribeAnimationClipSegmentRanges(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationGeneration generation) const
{
	using namespace TerraEngine::Animation;
	AnimationCookedRangeTable result;
	result.generation = generation;
	if(id == TerraEngine::EMPTY_UUID || generation == 0U) {
		result.message = "A cooked clip range query requires a non-empty asset and generation.";
		return result;
	}

	// Capture only the checked metadata needed to relate the immutable cache
	// publication to its file. Payload IO deliberately happens after this lock.
	AnimationGuid cachedAssetId{};
	AnimationGuid cachedSkeletonId{};
	AnimationCompatibilityFingerprint cachedCompatibility = 0U;
	AnimationFixedTick cachedDuration = 0;
	std::uint32_t cachedSampleRate = 0U;
	std::uint16_t cachedFormatVersion = 0U;
	std::uint64_t cachedStreamSize = 0U;
	std::vector<AnimationClipSegment> cachedSegments;
	{
		std::shared_lock lock(animationClipCache.mutex);
		const auto publishedGeneration = animationClipCache.runtimeGenerations.find(id);
		if(publishedGeneration == animationClipCache.runtimeGenerations.end()
			|| publishedGeneration->second != generation) {
			result.status = AnimationCookedRangeStatus::StaleGeneration;
			result.message = "The requested clip generation is no longer the cache publication.";
			return result;
		}
		const auto resource = animationClipCache.resources.find(id);
		const auto path = animationClipCache.idToPath.find(id);
		if(resource == animationClipCache.resources.end() || !resource->second
			|| resource->second->runtimeGeneration != generation
			|| path == animationClipCache.idToPath.end()) {
			result.status = AnimationCookedRangeStatus::MissingAsset;
			result.message = "The requested clip generation has no published payload or cooked path.";
			return result;
		}
		result.logicalPath = path->second;
		cachedAssetId = resource->second->assetId;
		cachedSkeletonId = resource->second->skeletonAssetId;
		cachedCompatibility = resource->second->compatibilityFingerprint;
		cachedDuration = resource->second->durationTicks;
		cachedSampleRate = resource->second->sampleRate;
		cachedFormatVersion = resource->second->formatVersion;
		cachedStreamSize = resource->second->streamData.size();
		cachedSegments = resource->second->segments;
	}

	const auto failRead = [&](const AnimationRangeReadResult& read,
		std::string_view context) {
		result.blocks.clear();
		result.status = read.status == AnimationRangeReadStatus::NotFound
			? AnimationCookedRangeStatus::MissingAsset
			: (read.status == AnimationRangeReadStatus::IoError
				? AnimationCookedRangeStatus::IoError
				: AnimationCookedRangeStatus::InvalidFormat);
		result.message = std::string(context) + ": " + read.message;
		return result;
	};
	const auto failFormat = [&](std::string_view message) {
		result.blocks.clear();
		result.status = AnimationCookedRangeStatus::InvalidFormat;
		result.message = std::string(message);
		return result;
	};

	const AnimationRangeReadResult envelopeRead = ReadAnimationAssetRange(
		result.logicalPath, 0U, sizeof(AnimationAssetEnvelope), sizeof(AnimationAssetEnvelope));
	if(!envelopeRead.Succeeded()) return failRead(envelopeRead, "Could not read the clip envelope");
	AnimationAssetEnvelope envelope{};
	std::memcpy(&envelope, envelopeRead.bytes.data(), sizeof(envelope));
	if(envelope.magic != kAnimationAssetMagic
		|| envelope.majorVersion != kAnimationAssetMajorVersion
		|| envelope.headerSize != sizeof(AnimationAssetEnvelope)
		|| envelope.endianMarker != kAnimationLittleEndianMarker
		|| envelope.assetId != cachedAssetId
		|| envelope.assetId.value != id
		|| envelope.compatibilityFingerprint != cachedCompatibility) {
		return failFormat("The clip envelope does not match the published cache generation.");
	}
	std::uint64_t fileSize = 0U;
	if(!CheckedAdd(sizeof(AnimationAssetEnvelope), envelope.payloadSize, fileSize)
		|| envelope.chunkCount == 0U || envelope.chunkCount > 64U) {
		return failFormat("The clip envelope has an overflowing size or unsupported chunk count.");
	}
	std::uint64_t chunkTableBytes = 0U;
	if(!CheckedMultiply(envelope.chunkCount, sizeof(AnimationChunkEntry), chunkTableBytes)
		|| ValidateAnimationRange(fileSize, envelope.chunkTableOffset, chunkTableBytes)
			!= AnimationStatus::Ok) {
		return failFormat("The clip chunk table is outside the cooked envelope.");
	}
	const AnimationRangeReadResult chunkRead = ReadAnimationAssetRange(
		result.logicalPath, envelope.chunkTableOffset, chunkTableBytes, chunkTableBytes);
	if(!chunkRead.Succeeded()) return failRead(chunkRead, "Could not read the clip chunk table");

	AnimationChunkEntry payloadChunk{};
	bool foundPayload = false;
	for(std::uint32_t index = 0U; index < envelope.chunkCount; ++index) {
		AnimationChunkEntry entry{};
		std::memcpy(&entry,
			chunkRead.bytes.data() + static_cast<std::size_t>(index) * sizeof(entry), sizeof(entry));
		if(ValidateAnimationRange(fileSize, entry.offset, entry.size) != AnimationStatus::Ok) {
			return failFormat("A clip chunk points outside the cooked envelope.");
		}
		if(entry.type != kAnimationChunkPayload) continue;
		if(foundPayload) return failFormat("The clip contains more than one DATA chunk.");
		payloadChunk = entry;
		foundPayload = true;
	}
	if(!foundPayload || payloadChunk.size < sizeof(AnimationClipAssetFileHeaderV1)) {
		return failFormat("The clip has no complete DATA header.");
	}

	const AnimationRangeReadResult headerRead = ReadAnimationAssetRange(
		result.logicalPath, payloadChunk.offset,
		sizeof(AnimationClipAssetFileHeaderV1), sizeof(AnimationClipAssetFileHeaderV1));
	if(!headerRead.Succeeded()) return failRead(headerRead, "Could not read the clip DATA header");
	AnimationClipAssetFileHeaderV1 header{};
	std::memcpy(&header, headerRead.bytes.data(), sizeof(header));
	if(header.magic != kAnimationClipPayloadMagic
		|| header.formatVersion != cachedFormatVersion
		|| header.headerSize != sizeof(AnimationClipAssetFileHeaderV1)
		|| header.skeletonAssetId != cachedSkeletonId
		|| header.durationTicks != cachedDuration
		|| header.sampleRate != cachedSampleRate
		|| header.segmentCount != cachedSegments.size()
		|| header.streamDataSize != cachedStreamSize
		|| ValidateAnimationRange(payloadChunk.size,
			header.streamDataOffset, header.streamDataSize) != AnimationStatus::Ok) {
		return failFormat("The clip DATA header does not match the published typed payload.");
	}

	std::uint64_t streamBase = 0U;
	if(!CheckedAdd(payloadChunk.offset, header.streamDataOffset, streamBase)) {
		return failFormat("The clip stream base overflows its cooked file range.");
	}
	result.blocks.reserve(cachedSegments.size());
	for(std::size_t index = 0U; index < cachedSegments.size(); ++index) {
		const AnimationClipSegment& segment = cachedSegments[index];
		if(segment.streamSize == 0U
			|| ValidateAnimationRange(header.streamDataSize,
				segment.streamOffset, segment.streamSize) != AnimationStatus::Ok) {
			return failFormat("A published clip segment has no valid stream-data interval.");
		}
		std::uint64_t absoluteOffset = 0U;
		if(!CheckedAdd(streamBase, segment.streamOffset, absoluteOffset)
			|| ValidateAnimationRange(fileSize, absoluteOffset, segment.streamSize)
				!= AnimationStatus::Ok) {
			return failFormat("A clip segment overflows the cooked file range.");
		}
		result.blocks.push_back({static_cast<std::uint32_t>(index),
			absoluteOffset, segment.streamSize});
	}
	result.status = AnimationCookedRangeStatus::Ok;
	result.message = "Resolved checked physical ranges for the published clip segments.";
	return result;
}

AnimationCookedRangeTable ResourceManager::DescribeMotionDatabaseBlockRanges(
	const TerraEngine::UUID& id,
	TerraEngine::Animation::AnimationGeneration generation) const
{
	using namespace TerraEngine::Animation;

	AnimationCookedRangeTable result;
	result.generation = generation;
	if(id == TerraEngine::EMPTY_UUID || generation == 0U) {
		result.message = "A cooked motion-database range query requires a non-empty asset and generation.";
		return result;
	}

	std::shared_lock lock(motionDatabaseCache.mutex);
	const auto publishedGeneration = motionDatabaseCache.runtimeGenerations.find(id);
	if(publishedGeneration == motionDatabaseCache.runtimeGenerations.end()
		|| publishedGeneration->second != generation) {
		result.status = AnimationCookedRangeStatus::StaleGeneration;
		result.message = "The requested motion-database generation is no longer published.";
		return result;
	}
	const auto resource = motionDatabaseCache.resources.find(id);
	const auto path = motionDatabaseCache.idToPath.find(id);
	if(resource == motionDatabaseCache.resources.end() || !resource->second
		|| path == motionDatabaseCache.idToPath.end()) {
		result.status = AnimationCookedRangeStatus::MissingAsset;
		result.message = "The requested motion-database generation has no payload or cooked path.";
		return result;
	}
	result.logicalPath = path->second;

	// Use the bytes owned by this exact cache publication instead of reopening the
	// path. A loose-file reimport may already have replaced that path on disk while
	// this generation remains visible, and mixing those generations would let an
	// otherwise valid range publish bytes from the wrong database.
	const std::span<const std::uint8_t> serialized = resource->second->bytes;
	MotionDatabaseAsset authoritativeAsset;
	const AnimationStatus decodeStatus = DeserializeMotionDatabaseAsset(
		serialized, 0U, authoritativeAsset);
	if(decodeStatus != AnimationStatus::Ok
		|| authoritativeAsset.assetId.value != id
		|| authoritativeAsset.residentBlocks.size()
			> kMotionDatabaseMaximumResidentBlocks) {
		result.status = AnimationCookedRangeStatus::InvalidFormat;
		result.message = "The cached motion-database bytes no longer describe the published identity and bounded block table.";
		return result;
	}

	AnimationAssetReader reader;
	if(reader.Parse(serialized) != AnimationStatus::Ok) {
		result.status = AnimationCookedRangeStatus::InvalidFormat;
		result.message = "The cached motion-database envelope could not be parsed while resolving streamed blocks.";
		return result;
	}
	const bool hasStreamChunk = std::any_of(
		reader.Chunks().begin(), reader.Chunks().end(), [](const AnimationChunkEntry& entry) {
			return entry.type == kAnimationChunkStreamData;
		});
	if(!hasStreamChunk) {
		result.status = AnimationCookedRangeStatus::UnsupportedLayout;
		// DATA-only files remain valid and fully resident. Their logical records are
		// split across prefix and extension arrays, so no fabricated file interval
		// may be sent to asynchronous streaming IO.
		result.message = "This valid legacy motion database has no STRM chunk; deterministic complete-database fallback remains authoritative.";
		return result;
	}

	if(authoritativeAsset.residentBlocks.empty()) {
		result.status = AnimationCookedRangeStatus::Ok;
		result.message = "The checked STRM chunk belongs to an empty motion database and therefore exposes no block ranges.";
		return result;
	}

	// Resolve the first block through the format-owned API. The full deserialize
	// above already validated every STRM byte and proved that resident-block CPU
	// offsets are contiguous and bounded. Deriving later absolute offsets from the
	// checked first range avoids reparsing a potentially large database once per
	// block while retaining the exact on-disk STRM mapping.
	MotionDatabaseStreamBlockRange firstRange;
	const AnimationStatus rangeStatus = ResolveMotionDatabaseStreamBlockRange(
		serialized, 0U, firstRange);
	if(rangeStatus == AnimationStatus::Unsupported) {
		result.status = AnimationCookedRangeStatus::UnsupportedLayout;
		result.message = "This valid legacy motion database has no contiguous STRM block layout.";
		return result;
	}
	const MotionDatabaseResidentBlock& firstBlock = authoritativeAsset.residentBlocks.front();
	if(rangeStatus != AnimationStatus::Ok
		|| firstRange.blockIndex != firstBlock.blockIndex
		|| firstRange.firstSample != firstBlock.firstSample
		|| firstRange.sampleCount != firstBlock.sampleCount
		|| firstRange.byteSize != firstBlock.cpuByteSize
		|| firstRange.contentFingerprint != firstBlock.contentFingerprint
		|| firstRange.absoluteByteOffset < firstBlock.cpuByteOffset) {
		result.status = AnimationCookedRangeStatus::InvalidFormat;
		result.message = "The motion-database STRM range does not match its authoritative DATA block metadata.";
		return result;
	}
	const std::uint64_t streamBase =
		firstRange.absoluteByteOffset - firstBlock.cpuByteOffset;

	result.blocks.reserve(authoritativeAsset.residentBlocks.size());
	for(const MotionDatabaseResidentBlock& block : authoritativeAsset.residentBlocks) {
		std::uint64_t absoluteOffset = 0U;
		if(!CheckedAdd(streamBase, block.cpuByteOffset, absoluteOffset)
			|| ValidateAnimationRange(serialized.size(), absoluteOffset, block.cpuByteSize)
				!= AnimationStatus::Ok) {
			result.blocks.clear();
			result.status = AnimationCookedRangeStatus::InvalidFormat;
			result.message = "A motion-database STRM block overflows the cached cooked asset range.";
			return result;
		}
		result.blocks.push_back({block.blockIndex, absoluteOffset, block.cpuByteSize});
	}
	result.status = AnimationCookedRangeStatus::Ok;
	result.message = "Resolved checked physical ranges for the published motion-database STRM blocks.";
	return result;
}
