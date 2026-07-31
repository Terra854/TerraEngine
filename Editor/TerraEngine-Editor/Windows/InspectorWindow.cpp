// InspectorWindow.cpp — Object Inspector Window Implementation
//
// Implements the property editing interface, rendering specialized ImGui widgets for different data types.
// It synchronizes changes made in the UI back to the underlying engine objects, ensuring real-time feedback in the scene.

#include "InspectorWindow.h"
#include "AnimatorInspectorMetadata.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/ScriptCompilationSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "SceneViewWindow.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/Resource/VirtualGeometryCodec.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scene/TransactionalMutation.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/Components/UIComponent.h"
#include "Engine/Components/ScriptComponent.h"
#include "Engine/Components/AnimatorComponent.h"
#include "Engine/Components/Physics/RigidBodyComponent.h"
#include "Engine/Components/Physics/ColliderSetComponent.h"
#include "Engine/Components/Physics/ConstraintSetComponent.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"
#include "Engine/Animation/Assets/AnimationControllerAsset.h"
#include "AssetDatabase.h"
#include "AssetImportResult.h"
#include "Editor/AssetCompilers/MeshCompiler.h"
#include "Editor/AssetCompilers/MaterialAssetWriter.h"
#include "Editor/AssetCompilers/TextureCompiler.h"
#include "ModelPackageImporter.h"
#include "Engine/File Formats/TerraEngineMaterialFile.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "Engine/File Formats/TerraEngineTextureFile.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

using ReflectionProperty = TerraEngine::Reflection::Property;
using ReflectionType = TerraEngine::Reflection::Type;
using ReflectionVariant = TerraEngine::Reflection::Variant;

namespace
{
struct TemfInspectorSummary
{
	TemfProductionHeader header{};
	std::uint32_t blockCount = 0u;
	std::string error;
};

// Inspector summaries are produced only after the complete immutable file has
// passed the same bounded validator used by the runtime loader.
std::optional<TemfInspectorSummary> ReadTemfInspectorSummary(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return std::nullopt;
	}
	input.seekg(0, std::ios::end);
	const std::streamoff size = input.tellg();
	input.seekg(0, std::ios::beg);
	if (size <= 0)
	{
		return std::nullopt;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char*>(bytes.data()), size);
	const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(bytes, {});
	if (!input || !validated.IsComplete())
	{
		return std::nullopt;
	}
	return TemfInspectorSummary{ validated.directory.header,
		static_cast<std::uint32_t>(validated.directory.blocks.size()), {} };
}

TerraEngine::UUID GetObjectUuid(entt::entity entity)
{
	if (std::optional<Object> object = ObjectManager::instance().getObject(entity))
	{
		return ObjectManager::instance().getUUID(*object);
	}
	return TerraEngine::EMPTY_UUID;
}

const char* AnimationExecutionPolicyName(TerraEngine::Animation::AnimationExecutionPolicy policy)
{
	switch (policy) {
	case TerraEngine::Animation::AnimationExecutionPolicy::Auto: return "Auto";
	case TerraEngine::Animation::AnimationExecutionPolicy::ForceGpuVisual: return "GPU Visual";
	case TerraEngine::Animation::AnimationExecutionPolicy::ForceHybridAuthoritative: return "Hybrid Authoritative";
	case TerraEngine::Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback: return "CPU Deterministic Fallback";
	}
	return "Unknown";
}

const char* AnimatorTimePolicyName(TerraEngine::Animation::AnimatorTimePolicy policy)
{
	switch (policy) {
	case TerraEngine::Animation::AnimatorTimePolicy::Scaled: return "Scaled";
	case TerraEngine::Animation::AnimatorTimePolicy::Unscaled: return "Unscaled";
	case TerraEngine::Animation::AnimatorTimePolicy::Fixed: return "Fixed";
	}
	return "Unknown";
}

const char* AnimatorPlaybackRunStateName(TerraEngine::Animation::AnimatorPlaybackRunState state)
{
	switch (state) {
	case TerraEngine::Animation::AnimatorPlaybackRunState::Stopped: return "Stopped";
	case TerraEngine::Animation::AnimatorPlaybackRunState::Playing: return "Playing";
	case TerraEngine::Animation::AnimatorPlaybackRunState::Paused: return "Paused";
	case TerraEngine::Animation::AnimatorPlaybackRunState::Completed: return "Completed";
	}
	return "Unknown";
}

std::string AnimationFingerprintText(TerraEngine::Animation::AnimationCompatibilityFingerprint fingerprint)
{
	char text[24]{};
	std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(fingerprint));
	return text;
}

bool IsGeneratedPackageSubAssetPath(const std::filesystem::path& path)
{
	const std::filesystem::path normalized = std::filesystem::absolute(path).lexically_normal();
	for (const auto& part : normalized)
	{
		const std::string name = part.string();
		const std::string suffix = "_Generated";
		if (name.size() > suffix.size()
			&& name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			return true;
		}
	}
	return false;
}

std::string LowercaseNormalizedPath(const std::filesystem::path& path)
{
	// Windows compares paths without regard to letter case. Normalizing both
	// sides before the prefix check also gives separators one stable spelling.
	std::string text = path.lexically_normal().generic_string();
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
		return static_cast<char>(std::tolower(value));
	});
	return text;
}

bool IsPathInsideOrEqual(const std::filesystem::path& candidate, const std::filesystem::path& root)
{
	const std::string normalizedCandidate = LowercaseNormalizedPath(candidate);
	std::string normalizedRoot = LowercaseNormalizedPath(root);
	if (!normalizedRoot.ends_with('/'))
	{
		normalizedRoot.push_back('/');
	}
	return normalizedCandidate == normalizedRoot.substr(0U, normalizedRoot.size() - 1U)
		|| normalizedCandidate.rfind(normalizedRoot, 0U) == 0U;
}

std::optional<std::filesystem::path> ResolvePublishedAnimationClipPath(
	const std::filesystem::path& buildRoot,
	const std::filesystem::path& publishedRelativePath)
{
	// A rooted path such as "\\clip.teanim" is not reported as absolute on
	// every Windows path form, but joining it can still discard BuildRoot.
	// Reject every root-bearing form and every parent traversal explicitly.
	if (publishedRelativePath.empty()
		|| publishedRelativePath.is_absolute()
		|| publishedRelativePath.has_root_name()
		|| publishedRelativePath.has_root_directory()
		|| publishedRelativePath.extension() != ".teanim")
	{
		return std::nullopt;
	}
	for (const std::filesystem::path& part : publishedRelativePath)
	{
		if (part == "..")
		{
			return std::nullopt;
		}
	}

	std::error_code pathError;
	const std::filesystem::path absoluteRoot =
		std::filesystem::absolute(buildRoot, pathError).lexically_normal();
	if (pathError)
	{
		return std::nullopt;
	}
	const std::filesystem::path lexicalCandidate =
		(absoluteRoot / publishedRelativePath).lexically_normal();
	if (!IsPathInsideOrEqual(lexicalCandidate, absoluteRoot))
	{
		return std::nullopt;
	}

	// Resolve existing symlinks as a second boundary check. This prevents a
	// manifest path that looks contained from opening a file outside BuildRoot.
	const std::filesystem::path canonicalRoot =
		std::filesystem::weakly_canonical(absoluteRoot, pathError);
	if (pathError)
	{
		return std::nullopt;
	}
	const std::filesystem::path canonicalCandidate =
		std::filesystem::weakly_canonical(lexicalCandidate, pathError);
	if (pathError || !IsPathInsideOrEqual(canonicalCandidate, canonicalRoot))
	{
		return std::nullopt;
	}
	return canonicalCandidate;
}

template<typename ComponentType>
struct PhysicsEditCandidate
{
	PhysicsEditCandidate(TerraEngine::UUID id, const ComponentType& source)
		: objectId(id), value(source) {}
	PhysicsEditCandidate(const PhysicsEditCandidate&) = default;
	PhysicsEditCandidate& operator=(const PhysicsEditCandidate&) = default;
	// Component mutexes are intentionally non-movable; copying authored data
	// creates a fresh lock and is safe for detached transaction preparation.
	PhysicsEditCandidate(PhysicsEditCandidate&& other) noexcept
		: objectId(other.objectId), value(other.value) {}
	PhysicsEditCandidate& operator=(PhysicsEditCandidate&& other) noexcept
	{
		objectId = other.objectId;
		value = other.value;
		return *this;
	}
	TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
	ComponentType value;
};

std::uint64_t g_PhysicsInspectorTransactionSequence = 0U;

template<typename ComponentType>
std::vector<PhysicsEditCandidate<ComponentType>> CopyPhysicsCandidates(const std::vector<entt::entity>& entities)
{
	std::vector<PhysicsEditCandidate<ComponentType>> candidates;
	candidates.reserve(entities.size());
	ObjectManager& objects = ObjectManager::instance();
	for (const entt::entity entity : entities)
	{
		const std::optional<Object> object = objects.getObject(entity);
		ComponentType* component = object ? objects.GetComponent<ComponentType>(*object) : nullptr;
		if (!object || component == nullptr)
		{
			return {};
		}
		auto lock = component->acquireReadLock();
		candidates.emplace_back(objects.getUUID(*object), *component);
	}
	return candidates;
}

template<typename ValueType>
std::vector<TerraEngine::UUID> SubmitPhysicsPropertyBatch(
	const std::vector<std::pair<TerraEngine::UUID, ValueType>>& values,
	std::string componentName,
	std::string propertyName,
	std::string undoLabel,
	std::string transactionPrefix)
{
	if (values.empty())
	{
		return {};
	}
	ObjectManager& objects = ObjectManager::instance();
	TerraEngine::Scene::TransactionalMutationBuilder builder(
		std::move(transactionPrefix) + "-" + std::to_string(++g_PhysicsInspectorTransactionSequence),
		TerraEngine::Scene::TransactionOrigin::Editor,
		"InspectorPhysicsMultiEdit");
	builder.ObserveScene(objects).SetUndoGroup(std::move(undoLabel));
	std::vector<TerraEngine::UUID> changedObjects;
	changedObjects.reserve(values.size());
	for (const auto& [objectId, value] : values)
	{
		builder.ObserveObject(objects, objectId)
			.UpdateComponentProperty(objectId, componentName, propertyName, TerraEngine::Reflection::Variant{value});
		changedObjects.push_back(objectId);
	}
	TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(builder.Build());
	return changedObjects;
}

template<typename ValueType>
bool HasMixedValues(const std::vector<ValueType>& values)
{
	for (std::size_t index = 1U; index < values.size(); ++index)
	{
		if (values[index] != values.front()) return true;
	}
	return false;
}

bool DrawMixedCheckbox(const char* label, bool& value, bool mixed)
{
	if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
	const bool changed = ImGui::Checkbox(label, &value);
	if (mixed) ImGui::PopItemFlag();
	return changed;
}

bool DrawMixedDragFloat(const char* label, float& value, bool mixed, float speed, float minimum, float maximum, const char* format)
{
	if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
	const bool changed = ImGui::DragFloat(label, &value, speed, minimum, maximum, format);
	if (mixed) ImGui::PopItemFlag();
	return changed;
}

bool DrawMixedCombo(const char* label, int& value, bool mixed, const char* const* labels, int count)
{
	if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
	const bool changed = ImGui::Combo(label, &value, labels, count);
	if (mixed) ImGui::PopItemFlag();
	return changed;
}

bool SameVector3(const TerraEngine::Physics::PhysicsVector3& left, const TerraEngine::Physics::PhysicsVector3& right)
{
	return left.x == right.x && left.y == right.y && left.z == right.z;
}

glm::vec3 ColliderPreviewHalfExtents(const TerraEngine::Physics::ColliderRecord& record)
{
	// The disabled preview fields must describe the authored primitive before an
	// edit begins. Convert each stored shape convention into the same half-extent
	// box used by Scene View handles so activating the preview causes no value jump.
	return std::visit([](const auto& shape) -> glm::vec3 {
		using Shape = std::decay_t<decltype(shape)>;
		if constexpr (std::is_same_v<Shape, TerraEngine::Physics::BoxColliderShape>)
		{
			return {shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z};
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::SphereColliderShape>)
		{
			return glm::vec3(shape.radius);
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::CapsuleColliderShape>
			|| std::is_same_v<Shape, TerraEngine::Physics::CylinderColliderShape>)
		{
			return {shape.radius, shape.height * 0.5F, shape.radius};
		}
		else if constexpr (std::is_same_v<Shape, TerraEngine::Physics::TaperedCapsuleColliderShape>)
		{
			const float radius = std::max(shape.topRadius, shape.bottomRadius);
			return {radius, shape.height * 0.5F, radius};
		}
		else
		{
			// Asset-backed shapes do not expose editable primitive dimensions. The
			// neutral box keeps the read-only preview bounded until mesh fitting runs.
			return glm::vec3(0.5F);
		}
	}, record.shape);
}

bool IsPhysicsAuthoringComponent(std::string_view componentName)
{
	return componentName == "RigidBodyComponent" || componentName == "ColliderSetComponent"
		|| componentName == "ConstraintSetComponent" || componentName == "CharacterControllerComponent";
}

bool TryResolveGeneratedPackageTextureName(const TerraEngine::UUID& textureGuid, std::string& outName)
{
	return AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(
		textureGuid,
		GeneratedModelPackageChildKind::Texture,
		outName);
}

std::optional<GeneratedModelPackageChildKind> GeneratedChildKindForUuidProperty(std::string_view propertyName)
{
	if (propertyName == "meshId")
	{
		return GeneratedModelPackageChildKind::Mesh;
	}
	if (propertyName == "materialId")
	{
		return GeneratedModelPackageChildKind::Material;
	}
	if (propertyName.find("Texture") != std::string_view::npos
		|| propertyName.find("texture") != std::string_view::npos)
	{
		return GeneratedModelPackageChildKind::Texture;
	}
	if (propertyName == "skeletonOverrideAssetId")
	{
		return GeneratedModelPackageChildKind::Skeleton;
	}
	if (propertyName == "defaultClipAssetId")
	{
		return GeneratedModelPackageChildKind::AnimationClip;
	}
	return std::nullopt;
}

bool TryResolveGeneratedPackageChildNameForProperty(
	const TerraEngine::UUID& assetGuid,
	std::string_view propertyName,
	std::string& outName)
{
	const std::optional<GeneratedModelPackageChildKind> kind = GeneratedChildKindForUuidProperty(propertyName);
	if (kind.has_value())
	{
		return AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(assetGuid, *kind, outName);
	}

	const GeneratedModelPackageChildKind candidateKinds[] = {
		GeneratedModelPackageChildKind::Mesh,
		GeneratedModelPackageChildKind::Material,
		GeneratedModelPackageChildKind::Texture,
		GeneratedModelPackageChildKind::Skeleton,
		GeneratedModelPackageChildKind::AnimationClip
	};
	for (GeneratedModelPackageChildKind candidateKind : candidateKinds)
	{
		if (AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(assetGuid, candidateKind, outName))
		{
			return true;
		}
	}
	return false;
}

void DrawAssetImportProgress(const AssetImportProgress& progress)
{
	const std::string fileName = progress.sourcePath.empty()
		? std::string("Asset")
		: progress.sourcePath.filename().string();
	char overlay[256]{};
	std::snprintf(overlay, sizeof(overlay), "%s - %s (%.1fs)",
		fileName.c_str(),
		progress.stage.empty() ? "Importing" : progress.stage.c_str(),
		progress.elapsedSeconds);
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progress.failed
		? ImVec4(0.8f, 0.2f, 0.2f, 1.0f)
		: ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
	ImGui::ProgressBar(std::clamp(progress.progress, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), overlay);
	ImGui::PopStyleColor();
	if (!progress.message.empty())
	{
		ImGui::TextDisabled("%s", progress.message.c_str());
	}
}

template <typename ValueType>
// IsMetaValueType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool IsMetaValueType(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::IsType<ValueType>(value);
}

template <typename ValueType>
// MetaValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ValueType MetaValue(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::GetValue<ValueType>(value);
}

template <typename ValueType>
// IsMetaType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool IsMetaType(const ReflectionType& type)
{
	return TerraEngine::Reflection::IsType<ValueType>(type);
}

// IsValid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline bool IsValid(const ReflectionVariant& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

// IsValid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline bool IsValid(const ReflectionProperty& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

// IsValid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline bool IsValid(const ReflectionType& value)
{
	return TerraEngine::Reflection::IsValid(value);
}

// MetaPropertyName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline std::string MetaPropertyName(const ReflectionProperty& property)
{
	return TerraEngine::Reflection::GetPropertyName(property);
}

// MetaPropertyType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline ReflectionType MetaPropertyType(const ReflectionProperty& property)
{
	return TerraEngine::Reflection::GetPropertyType(property);
}

// FindMetaProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
inline ReflectionProperty FindMetaProperty(const ReflectionType& type, std::string_view name)
{
	return TerraEngine::Reflection::FindProperty(type, name);
}

// Convert internal registry keys like "LightComponent" to user-facing names.
// This keeps serialization/component lookup stable while matching editor UX labels.
std::string BuildDisplayComponentName(const std::string& componentName)
{
	if (componentName == "LightComponent")
	{
		return "Light";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (componentName == "Script")
	{
		return "Script";
	}

	const std::string componentSuffix = "Component";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (componentName.size() > componentSuffix.size()
		&& componentName.compare(componentName.size() - componentSuffix.size(), componentSuffix.size(), componentSuffix) == 0)
	{
		return componentName.substr(0, componentName.size() - componentSuffix.size());
	}

	return componentName;
}

std::string BuildDisplayPropertyName(const std::string& componentName, const std::string& propertyName)
{
	if(componentName == "AnimatorComponent")
	{
		if(const auto* metadata =
			TerraEngine::Editor::Animation::FindAnimatorInspectorProperty(propertyName))
		{
			return std::string(metadata->label);
		}
	}

	if (componentName == "SkyComponent")
	{
		if (propertyName == "sourceAsset")
		{
			return "Source Asset";
		}
		if (propertyName == "exposure")
		{
			return "Exposure";
		}
		if (propertyName == "rotationOffsetDegrees")
		{
			return "Rotation Offset";
		}
		if (propertyName == "horizonTint")
		{
			return "Horizon Tint";
		}
		if (propertyName == "zenithTint")
		{
			return "Zenith Tint";
		}
		if (propertyName == "groundTint")
		{
			return "Ground Tint";
		}
	}

	return propertyName;
}

void DrawAnimatorPropertyGuidance(std::string_view propertyName, bool labelHovered)
{
	const auto* metadata =
		TerraEngine::Editor::Animation::FindAnimatorInspectorProperty(propertyName);
	if(metadata == nullptr || !labelHovered)
	{
		return;
	}

	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0F);
	ImGui::TextUnformatted(metadata->explanation.data(), metadata->explanation.data() + metadata->explanation.size());
	ImGui::Spacing();
	ImGui::TextUnformatted("How to use it:");
	ImGui::TextWrapped("%.*s", static_cast<int>(metadata->usage.size()), metadata->usage.data());
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

std::vector<AssetType> CandidateAssetTypesForProperty(std::string_view propertyName)
{
	if(propertyName == "controllerAssetId") return {AssetType::AnimationController};
	if(propertyName == "defaultClipAssetId") return {AssetType::AnimationClip};
	if(propertyName == "skeletonOverrideAssetId") return {AssetType::Skeleton};
	if(propertyName == "avatarOverrideAssetId") return {AssetType::Skeleton};
	if(propertyName == "retargetProfileOverrideAssetId") return {AssetType::RetargetProfile};
	if(propertyName == "ragdollProfileOverrideAssetId") return {AssetType::RagdollProfile};
	if(propertyName == "motionDatabaseOverrideAssetId") return {AssetType::MotionDatabase};
	if(propertyName == "meshId") return {AssetType::Mesh};
	if(propertyName == "materialId") return {AssetType::Material};
	if(propertyName.find("Texture") != std::string_view::npos
		|| propertyName.find("texture") != std::string_view::npos)
	{
		return {AssetType::Texture};
	}
	return {
		AssetType::Mesh,
		AssetType::Material,
		AssetType::Texture,
		AssetType::ShaderCollection
	};
}

std::string EmptyAnimatorAssetLabel(std::string_view propertyName)
{
	if(propertyName == "controllerAssetId") return "None - uses Default Clip";
	if(propertyName == "defaultClipAssetId") return "None - reference pose";
	if(propertyName == "skeletonOverrideAssetId") return "None - uses model skeleton";
	return "None - no override";
}

bool ResolveInspectorAssetLabel(
	const TerraEngine::UUID& assetId,
	std::string_view propertyName,
	std::string& outLabel)
{
	if(assetId == TerraEngine::EMPTY_UUID)
	{
		outLabel = propertyName.ends_with("AssetId")
			? EmptyAnimatorAssetLabel(propertyName)
			: "None";
		return true;
	}

	// Imported model children have a stable package identity even though they do
	// not appear as separate editable source files in AssetDatabase.
	if(TryResolveGeneratedPackageChildNameForProperty(assetId, propertyName, outLabel))
	{
		return true;
	}
	for(const auto& [path, metadata] : AssetDatabase::Instance().GetAssets())
	{
		if(metadata.guid == assetId)
		{
			outLabel = path.stem().string();
			return true;
		}
	}
	for(const AssetType assetType : CandidateAssetTypesForProperty(propertyName))
	{
		const std::filesystem::path resourcePath =
			ResourceManager::Instance().DescribeAssetReference(assetId, assetType);
		if(!resourcePath.empty())
		{
			outLabel = resourcePath.stem().string();
			return true;
		}
	}

	outLabel = "(Missing asset)";
	return false;
}

bool IsSkyComponentName(const std::string& componentName)
{
	return componentName == "SkyComponent";
}

void NotifySceneAfterComponentMutation(const std::string& componentName)
{
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	if (IsSkyComponentName(componentName))
	{
		SceneManager::Instance().MarkActiveSceneSkyStateDirty();
	}
}

void ResizeVirtualGeometryTargetRatios(VirtualGeometryAdaptiveSettingsMetadata& settings, int targetCount)
{
	const std::vector<float> defaults = AssetDatabase::Instance().GetDefaultVirtualGeometryAdaptiveSettings().targetRatios;
	targetCount = std::clamp(targetCount, 1, static_cast<int>(defaults.size()));
	const std::size_t previousSize = settings.targetRatios.size();
	settings.targetRatios.resize(static_cast<std::size_t>(targetCount));
	for (std::size_t index = previousSize; index < settings.targetRatios.size(); ++index)
	{
		settings.targetRatios[index] = index < defaults.size()
			? defaults[index]
			: settings.targetRatios[index - 1u] * 0.5f;
	}
	if (!settings.targetRatios.empty())
	{
		settings.targetRatios.front() = 1.0f;
	}
}

void DrawVirtualGeometrySettingsFields(VirtualGeometryAdaptiveSettingsMetadata& settings)
{
	if (settings.targetRatios.empty())
	{
		settings.targetRatios = AssetDatabase::Instance().GetDefaultVirtualGeometryAdaptiveSettings().targetRatios;
	}

	if (ImGui::BeginTable("VirtualGeometryOverrideSettingsTable", 2, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 180.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

		auto drawLabeled = [](const char* label, const std::function<void()>& widget) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);
			widget();
		};

		int targetCount = static_cast<int>(settings.targetRatios.size());
		drawLabeled("Target Count", [&]() {
			if (ImGui::SliderInt("##VirtualGeometryTargetCount", &targetCount, 1, 8))
			{
				ResizeVirtualGeometryTargetRatios(settings, targetCount);
			}
		});

		for (std::size_t index = 0; index < settings.targetRatios.size(); ++index)
		{
			const std::string label = "Ratio " + std::to_string(index);
			const std::string id = "##VirtualGeometryRatio" + std::to_string(index);
			drawLabeled(label.c_str(), [&]() {
				const float minimum = index == 0u ? 1.0f : 0.001f;
				const float maximum = index == 0u ? 1.0f : 0.999f;
				ImGui::DragFloat(id.c_str(), &settings.targetRatios[index], 0.005f, minimum, maximum, "%.4f");
				if (index == 0u)
				{
					settings.targetRatios[index] = 1.0f;
				}
			});
		}

		auto drawU32 = [&](const char* label, const char* id, std::uint32_t& value, int minimum, int maximum) {
			drawLabeled(label, [&]() {
				int signedValue = static_cast<int>(value);
				if (ImGui::DragInt(id, &signedValue, 1.0f, minimum, maximum))
				{
					value = static_cast<std::uint32_t>(std::max(minimum, signedValue));
				}
			});
		};

		drawU32("Min Source Tris", "##VirtualGeometryMinSourceTriangles", settings.minimumSourceTriangles, 1, 10000000);
		drawU32("Min Generated Tris", "##VirtualGeometryMinGeneratedTriangles", settings.minimumGeneratedTriangles, 1, 10000000);
		drawU32("Min Meshlets", "##VirtualGeometryMinMeshlets", settings.minimumMeshletsPerGeneratedLevel, 1, 1000000);

		drawLabeled("Min Triangle Reduction %", [&]() {
			float percentage =
				settings.minimumUsefulTriangleReduction * 100.0f;
			if (ImGui::DragFloat(
				"##VirtualGeometryTriangleReduction",
				&percentage,
				0.5f,
				0.0f,
				95.0f,
				"%.1f%%"))
			{
				// Persistence and the cooker use a normalized [0,1] fraction.
				settings.minimumUsefulTriangleReduction =
					percentage / 100.0f;
			}
		});
		drawLabeled("Min Meshlet Reduction %", [&]() {
			float percentage =
				settings.minimumUsefulMeshletReduction * 100.0f;
			if (ImGui::DragFloat(
				"##VirtualGeometryMeshletReduction",
				&percentage,
				0.5f,
				0.0f,
				95.0f,
				"%.1f%%"))
			{
				settings.minimumUsefulMeshletReduction =
					percentage / 100.0f;
			}
		});
		drawLabeled("Max Error", [&]() {
			ImGui::DragFloat("##VirtualGeometryMaxError", &settings.maximumGeometricError, 0.005f, 0.0f, 1000.0f, "%.3f");
		});

		ImGui::EndTable();
	}
}

// VkFormatToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const char* VkFormatToString(uint32_t vkFormat)
{
	switch (vkFormat)
	{
	case 37: return "R8G8B8A8_UNORM";
	case 43: return "R8G8B8A8_SRGB";
	case 50: return "B8G8R8A8_UNORM";
	case 56: return "B8G8R8A8_SRGB";
	case 131: return "BC1_RGB_UNORM_BLOCK";
	case 133: return "BC1_RGB_SRGB_BLOCK";
	case 137: return "BC3_UNORM_BLOCK";
	case 139: return "BC3_SRGB_BLOCK";
	case 141: return "BC4_UNORM_BLOCK";
	case 142: return "BC4_SNORM_BLOCK";
	case 143: return "BC5_UNORM_BLOCK";
	case 144: return "BC5_SNORM_BLOCK";
	case 145: return "BC7_UNORM_BLOCK";
	case 146: return "BC7_SRGB_BLOCK";
	case 157: return "ASTC_4x4_UNORM_BLOCK";
	case 158: return "ASTC_4x4_SRGB_BLOCK";
	default: return "Other/Unknown";
	}
}

// BuildScriptBindingStatusMessage is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string BuildScriptBindingStatusMessage(const ScriptBinding& binding)
{
	if (binding.status == "Attached")
	{
		return {};
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "BrokenType")
	{
		return "The active Assembly-CSharp build does not currently expose this class as a public non-abstract TerraEngine.Script type. This usually means the script failed to compile, was renamed, changed namespace, or no longer inherits from TerraEngine.Script.";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "LiveReady")
	{
		return {};
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "InstanceUnavailable")
	{
		return "TerraEngine could not find a live managed instance for this binding. The object may have been destroyed, disabled, or is still waiting for the next script update.";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "ReloadPending")
	{
		return "Assembly-CSharp is recompiling or reloading, so live values are temporarily unavailable.";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status.empty())
	{
		return {};
	}

	return "Script status: " + binding.status + ".";
}

// BuildScriptBindingStatusColor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ImVec4 BuildScriptBindingStatusColor(const ScriptBinding& binding)
{
	if (binding.status == "BrokenType")
	{
		return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (binding.status == "InstanceUnavailable")
	{
		return ImVec4(1.0f, 0.7f, 0.4f, 1.0f);
	}

	return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

// ReadScriptMetadataString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ReadScriptMetadataString(const AssetMetadata* assetMetadata, const char* key)
{
	if (assetMetadata == nullptr)
	{
		return {};
	}

	const auto settingIt = assetMetadata->settings.find(key);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (settingIt == assetMetadata->settings.end() || !settingIt->is_string())
	{
		return {};
	}

	return settingIt->get<std::string>();
}

// Inspector headers use the script class name without namespaces because that
// is the same source Unity uses for "My Custom Script (Script)" cards.
std::string ExtractScriptLeafName(const std::string& fullName)
{
	if (fullName.empty())
	{
		return {};
	}

	const std::size_t lastNamespaceSeparator = fullName.find_last_of('.');
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (lastNamespaceSeparator == std::string::npos)
	{
		return fullName;
	}

	return fullName.substr(lastNamespaceSeparator + 1);
}

// Unity nicifies PascalCase class names into inspector titles like
// "New Mono Behaviour Script" while leaving the backing identifier untouched.
std::string NicifyScriptIdentifier(const std::string& identifier)
{
	if (identifier.empty())
	{
		return {};
	}

	std::string nicified;
	nicified.reserve(identifier.size() * 2);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < identifier.size(); ++index)
	{
		const unsigned char current = static_cast<unsigned char>(identifier[index]);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (current == '_' || current == '-')
		{
			if (!nicified.empty() && nicified.back() != ' ')
			{
				nicified.push_back(' ');
			}
			continue;
		}

		bool shouldInsertSpace = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (index > 0 && !nicified.empty() && nicified.back() != ' ')
		{
			const unsigned char previous = static_cast<unsigned char>(identifier[index - 1]);
			const unsigned char next = index + 1 < identifier.size()
				? static_cast<unsigned char>(identifier[index + 1])
				: 0;
			shouldInsertSpace =
				(std::isupper(current) != 0 && (std::islower(previous) != 0 || std::isdigit(previous) != 0))
				|| (std::isupper(current) != 0 && std::isupper(previous) != 0 && next != 0 && std::islower(next) != 0)
				|| (std::isdigit(current) != 0 && std::isalpha(previous) != 0)
				|| (std::isalpha(current) != 0 && std::isdigit(previous) != 0);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shouldInsertSpace)
		{
			nicified.push_back(' ');
		}

		nicified.push_back(static_cast<char>(current));
	}

	return nicified;
}

// BuildScriptBindingDisplayName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string BuildScriptBindingDisplayName(const ScriptBinding& binding, const AssetMetadata* assetMetadata)
{
	const std::string currentFullName = ReadScriptMetadataString(assetMetadata, "scriptFullName");
	const std::string scriptLeafName = ExtractScriptLeafName(currentFullName.empty() ? binding.className : currentFullName);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptLeafName.empty())
	{
		return "Missing Script";
	}

	return NicifyScriptIdentifier(scriptLeafName);
}

// BuildScriptAssetDisplayName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string BuildScriptAssetDisplayName(const ScriptBinding& binding, const AssetMetadata* assetMetadata)
{
	if (assetMetadata != nullptr && !assetMetadata->sourcePath.empty())
	{
		return assetMetadata->sourcePath.stem().string();
	}

	const std::string currentFullName = ReadScriptMetadataString(assetMetadata, "scriptFullName");
	const std::string scriptLeafName = ExtractScriptLeafName(currentFullName.empty() ? binding.className : currentFullName);
	return scriptLeafName.empty() ? "MissingScript" : scriptLeafName;
}

// FormatScriptFieldValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string FormatScriptFieldValue(const ScriptFieldValue& fieldValue)
{
	if (fieldValue.value.is_string())
	{
		return fieldValue.value.get<std::string>();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.value.is_boolean())
	{
		return fieldValue.value.get<bool>() ? "True" : "False";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.value.is_number_integer())
	{
		return std::to_string(fieldValue.value.get<long long>());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.value.is_number_unsigned())
	{
		return std::to_string(fieldValue.value.get<unsigned long long>());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.value.is_number_float())
	{
		return std::to_string(fieldValue.value.get<double>());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (fieldValue.value.is_null())
	{
		return "None";
	}

	return fieldValue.value.dump();
}

// ScriptAssetPayloadType is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const char* ScriptAssetPayloadType(std::string_view valueKind)
{
	if (valueKind == "MeshAssetReference")
	{
		return "ASSET_MESH";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "MaterialAssetReference")
	{
		return "ASSET_MATERIAL";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "TextureAssetReference")
	{
		return "ASSET_TEXTURE";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "ShaderAssetReference")
	{
		return "ASSET_SHADER";
	}

	return nullptr;
}

// ScriptAssetImporterName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const char* ScriptAssetImporterName(std::string_view valueKind)
{
	if (valueKind == "MeshAssetReference")
	{
		return "MeshImporter";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "MaterialAssetReference")
	{
		return "MaterialImporter";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "TextureAssetReference")
	{
		return "TextureImporter";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (valueKind == "ShaderAssetReference")
	{
		return "ShaderImporter";
	}

	return nullptr;
}

// BuildScriptAssetReferenceValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string BuildScriptAssetReferenceValue(const AssetMetadata& metadata)
{
	// Store the authored stem so script-side asset resolution can continue to
	// use the same top-level name lookup path it already supports.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadata.sourcePath.empty())
	{
		return metadata.sourcePath.stem().string();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!metadata.compiledPath.empty())
	{
		return metadata.compiledPath.stem().string();
	}

	return {};
}

// ExtractScriptFieldRootPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ExtractScriptFieldRootPath(std::string_view fieldPath)
{
	const std::size_t dotIndex = fieldPath.find('.');
	const std::size_t bracketIndex = fieldPath.find('[');
	const std::size_t cutIndex = dotIndex == std::string::npos
		? bracketIndex
		: bracketIndex == std::string::npos ? dotIndex : std::min(dotIndex, bracketIndex);
	return cutIndex == std::string::npos ? std::string(fieldPath) : std::string(fieldPath.substr(0, cutIndex));
}

// ExtractScriptFieldRelativePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ExtractScriptFieldRelativePath(std::string_view rootPath, std::string_view fullPath)
{
	if (fullPath.size() <= rootPath.size())
	{
		return {};
	}

	std::string relativePath = std::string(fullPath.substr(rootPath.size()));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!relativePath.empty() && relativePath.front() == '.')
	{
		relativePath.erase(relativePath.begin());
	}

	return relativePath;
}

// ReplaceFirstCollectionPlaceholder is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ReplaceFirstCollectionPlaceholder(const std::string& fieldPath, std::size_t index)
{
	const std::size_t placeholderIndex = fieldPath.find("[]");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (placeholderIndex == std::string::npos)
	{
		return fieldPath;
	}

	return fieldPath.substr(0, placeholderIndex) + "[" + std::to_string(index) + "]" + fieldPath.substr(placeholderIndex + 2);
}

// MaterializeCollectionDescriptor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ScriptFieldDescriptor MaterializeCollectionDescriptor(const ScriptFieldDescriptor& templateDescriptor, std::size_t index)
{
	ScriptFieldDescriptor descriptor = templateDescriptor;
	const auto materializeChildren = [&](const auto& self, ScriptFieldDescriptor& currentDescriptor) -> void {
		currentDescriptor.fieldPath = ReplaceFirstCollectionPlaceholder(currentDescriptor.fieldPath, index);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (ScriptFieldDescriptor& child : currentDescriptor.children)
		{
			self(self, child);
		}
	};

	materializeChildren(materializeChildren, descriptor);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.displayName == "Element")
	{
		descriptor.displayName = "Element " + std::to_string(index);
	}
	return descriptor;
}

// BuildDefaultScriptFieldJson is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json BuildDefaultScriptFieldJson(const ScriptFieldDescriptor& descriptor)
{
	// Prefer the managed field initializer captured from the C# type. When the
	// bridge cannot provide one, fall back to a type-based editor default.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!descriptor.defaultValue.is_null())
	{
		return descriptor.defaultValue;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.containerKind == "Collection")
	{
		return nlohmann::json::array();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.containerKind == "NestedObject")
	{
		nlohmann::json objectValue = nlohmann::json::object();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ScriptFieldDescriptor& child : descriptor.children)
		{
			objectValue[child.serializedName] = BuildDefaultScriptFieldJson(child);
		}
		return objectValue;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Bool")
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Int")
	{
		return 0;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Float")
	{
		return 0.0f;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "String")
	{
		return "";
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Enum")
	{
		return descriptor.enumOptions.empty() ? "" : descriptor.enumOptions.front();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Vector2")
	{
		return nlohmann::json::array({ 0.0f, 0.0f });
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Vector3")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f });
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Vector4")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 0.0f });
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "Quaternion")
	{
		return nlohmann::json::array({ 0.0f, 0.0f, 0.0f, 1.0f });
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (descriptor.valueKind == "EntityReference")
	{
		return 0;
	}

	return nullptr;
}

// ResolveScriptJsonPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
nlohmann::json* ResolveScriptJsonPath(nlohmann::json& rootValue, std::string_view rootPath, std::string_view fullPath, bool createMissing)
{
	std::string relativePath = ExtractScriptFieldRelativePath(rootPath, fullPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (relativePath.empty())
	{
		return &rootValue;
	}

	nlohmann::json* current = &rootValue;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < relativePath.size();)
	{
		if (relativePath[index] == '.')
		{
			++index;
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (relativePath[index] == '[')
		{
			const std::size_t closingBracket = relativePath.find(']', index);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (closingBracket == std::string::npos)
			{
				return nullptr;
			}

			const std::size_t arrayIndex = static_cast<std::size_t>(std::stoul(relativePath.substr(index + 1, closingBracket - index - 1)));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!current->is_array())
			{
				if (!createMissing)
				{
					return nullptr;
				}

				*current = nlohmann::json::array();
			}

			while (current->size() <= arrayIndex)
			{
				current->push_back(nullptr);
			}

			current = &(*current)[arrayIndex];
			index = closingBracket + 1;
			continue;
		}

		const std::size_t tokenEnd = relativePath.find_first_of(".[", index);
		const std::string token = relativePath.substr(index, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - index);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!current->is_object())
		{
			if (!createMissing)
			{
				return nullptr;
			}

			*current = nlohmann::json::object();
		}

		current = &(*current)[token];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (tokenEnd == std::string::npos)
		{
			break;
		}

		index = tokenEnd;
	}

	return current;
}

// ResolveScriptJsonPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const nlohmann::json* ResolveScriptJsonPath(const nlohmann::json& rootValue, std::string_view rootPath, std::string_view fullPath)
{
	std::string relativePath = ExtractScriptFieldRelativePath(rootPath, fullPath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (relativePath.empty())
	{
		return &rootValue;
	}

	const nlohmann::json* current = &rootValue;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::size_t index = 0; index < relativePath.size();)
	{
		if (relativePath[index] == '.')
		{
			++index;
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (relativePath[index] == '[')
		{
			const std::size_t closingBracket = relativePath.find(']', index);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (closingBracket == std::string::npos || !current->is_array())
			{
				return nullptr;
			}

			const std::size_t arrayIndex = static_cast<std::size_t>(std::stoul(relativePath.substr(index + 1, closingBracket - index - 1)));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (arrayIndex >= current->size())
			{
				return nullptr;
			}

			current = &(*current)[arrayIndex];
			index = closingBracket + 1;
			continue;
		}

		const std::size_t tokenEnd = relativePath.find_first_of(".[", index);
		const std::string token = relativePath.substr(index, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - index);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!current->is_object())
		{
			return nullptr;
		}

		auto tokenIt = current->find(token);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (tokenIt == current->end())
		{
			return nullptr;
		}

		current = &(*tokenIt);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (tokenEnd == std::string::npos)
		{
			break;
		}

		index = tokenEnd;
	}

	return current;
}
}

// Command that records a property change on a component. Captures the 
// previous and next values to allow bidirectional state switching.
class ModifyComponentCommand : public ICommand {
public:
	ModifyComponentCommand(entt::entity entity, const std::string& component, const std::string& property, ReflectionVariant oldVal, ReflectionVariant newVal)
		// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		: m_Uuid(GetObjectUuid(entity)), m_Component(component), m_Property(property), m_OldValue(oldVal), m_NewValue(newVal) {}

	void Undo() override {
		ApplyValue(m_OldValue);
	}

	// Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Redo() override {
		ApplyValue(m_NewValue);
	}

	// GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string GetName() const override {
		return "Modify " + m_Component + "." + m_Property;
	}

private:
	// ApplyValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ApplyValue(const ReflectionVariant& val) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
			auto& reg = ComponentAccessRegistry();
			if (reg.count(m_Component)) {
				// Setters handle thread-safety internally via component mutexes.
				reg[m_Component].entitySetter(ObjectManager::instance().getRegistry(), obj->EntityId(), m_Property, val);
				NotifySceneAfterComponentMutation(m_Component);
			}
		}
	}

	TerraEngine::UUID m_Uuid;
	std::string m_Component;
	std::string m_Property;
	ReflectionVariant m_OldValue;
	ReflectionVariant m_NewValue;
};

// ObjectMetadataField names the supported states explicitly so serialization and switches stay readable.
enum class ObjectMetadataField {
	Tag,
	Layer,
	Active,
	Static
};

// class ModifyObjectMetadataCommand groups related state and operations so callers have one documented contract.
class ModifyObjectMetadataCommand : public ICommand {
public:
	ModifyObjectMetadataCommand(entt::entity entity, ObjectMetadataField field, ReflectionVariant oldVal, ReflectionVariant newVal)
		// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		: m_Uuid(GetObjectUuid(entity)), m_Field(field), m_OldValue(oldVal), m_NewValue(newVal) {}

	void Undo() override { ApplyValue(m_OldValue); }
	// Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Redo() override { ApplyValue(m_NewValue); }

	std::string GetName() const override {
		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (m_Field) {
			case ObjectMetadataField::Tag: return "Modify Object Tag";
			case ObjectMetadataField::Layer: return "Modify Object Layer";
			case ObjectMetadataField::Active: return "Modify Object Active State";
			case ObjectMetadataField::Static: return "Modify Object Static State";
		}
		return "Modify Object Metadata";
	}

private:
	// ApplyValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ApplyValue(const ReflectionVariant& val) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (m_Field) {
				case ObjectMetadataField::Tag: ObjectManager::instance().setObjectTag(*obj, MetaValue<std::string>(val)); break;
				case ObjectMetadataField::Layer: ObjectManager::instance().setObjectLayer(*obj, MetaValue<std::string>(val)); break;
				case ObjectMetadataField::Active: ObjectManager::instance().setObjectActive(*obj, MetaValue<bool>(val)); break;
				case ObjectMetadataField::Static: ObjectManager::instance().setObjectStatic(*obj, MetaValue<bool>(val)); break;
			}
		}
	}

	TerraEngine::UUID m_Uuid;
	ObjectMetadataField m_Field;
	ReflectionVariant m_OldValue;
	ReflectionVariant m_NewValue;
};

InspectorWindow::InspectorWindow(MainEditorSubsystem* owner)
	// EditorWindow is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: EditorWindow("Inspector", true, "General"), m_Owner(owner) {
}

InspectorWindow::~InspectorWindow() = default;

bool InspectorWindow::IsPrefabOverride(entt::entity obj, const std::string& compName, const std::string& propertyName) const
{
	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	if (!object)
	{
		return false;
	}
	PrefabLinkComponent* link = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
	if (link == nullptr)
	{
		return false;
	}

	const std::string token = compName + "." + propertyName;
	std::istringstream stream(link->overriddenProperties);
	std::string item;
	while (std::getline(stream, item, ';'))
	{
		if (item == token || item == compName)
		{
			return true;
		}
	}
	return false;
}

void InspectorWindow::ResetPrefabOverride(entt::entity obj, const std::string& compName, const std::string& propertyName)
{
	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	if (!object)
	{
		return;
	}
	PrefabLinkComponent* link = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
	if (link == nullptr)
	{
		return;
	}

	const std::string propertyToken = compName + "." + propertyName;
	const std::string componentToken = compName;
	std::vector<std::string> keptTokens;
	std::istringstream stream(link->overriddenProperties);
	std::string item;
	while (std::getline(stream, item, ';'))
	{
		const bool resetsWholeComponent = propertyName.empty() && (item == componentToken || item.rfind(componentToken + ".", 0) == 0);
		if (!item.empty() && item != propertyToken && item != componentToken && !resetsWholeComponent)
		{
			keptTokens.push_back(item);
		}
	}

	link->overriddenProperties.clear();
	for (std::size_t index = 0; index < keptTokens.size(); ++index)
	{
		if (index > 0)
		{
			link->overriddenProperties += ';';
		}
		link->overriddenProperties += keptTokens[index];
	}
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Prefab override reset requested for " << compName << "." << propertyName);
}

void InspectorWindow::DrawPrefabLinkInspector(entt::entity obj)
{
	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	if (!object)
	{
		return;
	}
	PrefabLinkComponent* link = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
	if (link == nullptr)
	{
		return;
	}

	const std::string prefabName = link->prefabName.empty() ? ObjectManager::instance().getObjectName(*object) : link->prefabName;
	if (ImGui::CollapsingHeader("Prefab", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Prefab: %s", prefabName.c_str());
		if (!link->prefabPath.empty()) {
			ImGui::TextDisabled("%s", link->prefabPath.c_str());
		}
		if (ImGui::Button("Open Source")) {
			m_Owner->OpenPrefabFromObject(obj);
		}
		ImGui::SameLine();
		if (ImGui::Button("Unlink")) {
			ObjectManager::instance().RemoveComponent<PrefabLinkComponent>(*object);
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			return;
		}
		if (ImGui::Button("Apply To Defaults")) {
			TERRAENGINE_LOG(LogLevel::Info, "Prefab authoring request: apply selected inspector values to defaults for " << prefabName);
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset Object")) {
			m_Owner->ResetPrefabInstanceToDefaults(obj);
			return;
		}
	}
}

// InspectorWindow::UpdateInspectorDragState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::UpdateInspectorDragState() {
	if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		m_InspectorValueDragging = true;
	}
}

TerraEngine::UUID InspectorWindow::ResolveAnimatorStartClipAssetId(
	TerraEngine::UUID controllerAssetId,
	TerraEngine::UUID defaultClipAssetId)
{
	if(controllerAssetId == TerraEngine::EMPTY_UUID)
	{
		m_AnimatorFrameRangeControllerId = TerraEngine::EMPTY_UUID;
		m_AnimatorFrameRangeControllerGeneration = 0U;
		m_AnimatorFrameRangeControllerEntryClipId = TerraEngine::EMPTY_UUID;
		return defaultClipAssetId;
	}

	ResourceManager& resources = ResourceManager::Instance();
	const TerraEngine::Animation::AnimationGeneration observedGeneration =
		resources.GetAnimationAssetRuntimeGeneration(
			controllerAssetId,
			AssetType::AnimationController);
	if(m_AnimatorFrameRangeControllerId == controllerAssetId
		&& m_AnimatorFrameRangeControllerGeneration == observedGeneration)
	{
		return m_AnimatorFrameRangeControllerEntryClipId != TerraEngine::EMPTY_UUID
			? m_AnimatorFrameRangeControllerEntryClipId
			: defaultClipAssetId;
	}

	TerraEngine::UUID entryClipAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::Animation::AnimationControllerAsset controller;
	TerraEngine::Animation::AnimationGeneration copiedGeneration = 0U;
	if(observedGeneration != 0U
		&& resources.CopyAnimationControllerPublication(
			controllerAssetId,
			controller,
			copiedGeneration)
		&& copiedGeneration == observedGeneration)
	{
		// Controllers may nest entry states. Follow the same bounded entry-leaf
		// rule as runtime binding, then resolve a clip node at that leaf.
		const TerraEngine::Animation::AnimationControllerState* entry = nullptr;
		for(const auto& state : controller.states)
		{
			if(state.parentStateId == 0U && state.entry)
			{
				entry = &state;
				break;
			}
		}
		for(std::size_t depth = 0U; entry != nullptr && depth < 64U; ++depth)
		{
			const TerraEngine::Animation::AnimationControllerState* child = nullptr;
			for(const auto& state : controller.states)
			{
				if(state.parentStateId == entry->stableId && state.entry)
				{
					child = &state;
					break;
				}
			}
			if(child == nullptr) break;
			entry = child;
		}
		if(entry != nullptr)
		{
			for(const auto& node : controller.nodes)
			{
				if(node.stableId == entry->nodeId
					&& node.kind
						== TerraEngine::Animation::AnimationControllerNodeKind::Clip
					&& node.dependencyAssetId.IsValid())
				{
					entryClipAssetId = node.dependencyAssetId.value;
					break;
				}
			}
		}
	}

	m_AnimatorFrameRangeControllerId = controllerAssetId;
	m_AnimatorFrameRangeControllerGeneration = observedGeneration;
	m_AnimatorFrameRangeControllerEntryClipId = entryClipAssetId;
	return entryClipAssetId != TerraEngine::EMPTY_UUID
		? entryClipAssetId
		: defaultClipAssetId;
}

std::optional<std::uint32_t> InspectorWindow::ResolveAnimatorPlaybackMaximumFrame(
	TerraEngine::UUID clipAssetId)
{
	if(clipAssetId == TerraEngine::EMPTY_UUID)
	{
		m_AnimatorFrameRangeClipId = TerraEngine::EMPTY_UUID;
		m_AnimatorFrameRangeClipGeneration = 0U;
		m_AnimatorFrameRangeMaximum.reset();
		return m_AnimatorFrameRangeMaximum;
	}

	ResourceManager& resources = ResourceManager::Instance();
	const TerraEngine::Animation::AnimationGeneration observedGeneration =
		resources.GetAnimationAssetRuntimeGeneration(
			clipAssetId,
			AssetType::AnimationClip);
	if(m_AnimatorFrameRangeClipId == clipAssetId
		&& m_AnimatorFrameRangeClipGeneration == observedGeneration)
	{
		return m_AnimatorFrameRangeMaximum;
	}

	// Copy only after the cheap generation key changes. This keeps normal
	// Inspector drawing independent of clip size while still invalidating the
	// maximum immediately after an atomic reimport publication.
	TerraEngine::Animation::AnimationClipAsset clip;
	TerraEngine::Animation::AnimationGeneration copiedGeneration = 0U;
	std::optional<std::uint32_t> maximum;
	if(observedGeneration != 0U
		&& resources.CopyAnimationClipPublication(
			clipAssetId,
			clip,
			copiedGeneration)
		&& copiedGeneration == observedGeneration)
	{
		maximum = TerraEngine::Editor::Animation::BuildAnimatorInspectorFrameRange(
			std::numeric_limits<std::uint32_t>::max(),
			clip.durationTicks).maximumFrame;
	}

	m_AnimatorFrameRangeClipId = clipAssetId;
	m_AnimatorFrameRangeClipGeneration = observedGeneration;
	m_AnimatorFrameRangeMaximum = maximum;
	return m_AnimatorFrameRangeMaximum;
}

// InspectorWindow::DrawProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawProperty(const std::string& compName, ReflectionProperty prop, entt::entity obj, ComponentAccessFunctions& access) {
	std::string name = MetaPropertyName(prop);
	const std::string displayName = BuildDisplayPropertyName(compName, name);
	ReflectionVariant var = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
	ReflectionType type = MetaPropertyType(prop);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!IsValid(var)) return;

	// Snapshot the initial value when the user first interacts with a control.
	auto startEditing = [&](const ReflectionVariant& initialValue) {
		if (ImGui::IsItemActivated()) {
			m_PreEditValue = initialValue;
		}
	};

	// Push the command to the UndoRedoSystem once the interaction is finalized
	// (e.g. mouse button released after a drag).
	auto endEditing = [&]() {
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			ReflectionVariant newVal = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, m_PreEditValue, newVal));
			NotifySceneAfterComponentMutation(compName);
		}
	};

	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::AlignTextToFramePadding();
	const bool prefabOverride = IsPrefabOverride(obj, compName, name);
	if (prefabOverride) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.28f, 1.0f));
	}
	ImGui::TextUnformatted(displayName.c_str());
	if (prefabOverride) {
		ImGui::PopStyleColor();
	}
	// Capture hover state before opening the context-menu path because popup
	// contents can replace ImGui's current item while the menu is visible.
	const bool propertyLabelHovered =
		ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
	if (ImGui::BeginPopupContextItem(("PrefabPropertyReset_" + compName + "_" + name).c_str())) {
		if (ImGui::MenuItem("Reset To Prefab Default")) {
			ResetPrefabOverride(obj, compName, name);
		}
		ImGui::EndPopup();
	}
	if(compName == "AnimatorComponent")
	{
		DrawAnimatorPropertyGuidance(name, propertyLabelHovered);
	}
	ImGui::TableSetColumnIndex(1);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (IsMetaType<float>(type)) {
		float val = MetaValue<float>(var);
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::DragFloat(("##" + name).c_str(), &val, 0.1f)) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (compName == "CameraComponent") {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (name == "nearPlane") {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (val < 0.001f) val = 0.001f;
					
					ReflectionVariant farVar = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "farPlane");
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (IsValid(farVar)) {
						float farP = MetaValue<float>(farVar);
						if (farP < val + 0.001f) {
							access.entitySetter(ObjectManager::instance().getRegistry(), obj, "farPlane", val + 0.001f);
						}
					}
				}
				// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
				else if (name == "farPlane") {
					ReflectionVariant nearVar = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "nearPlane");
					if (IsValid(nearVar)) {
						float nearP = MetaValue<float>(nearVar);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (val < nearP + 0.001f) val = nearP + 0.001f;
					}
				}
			}
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, val);
		}
		startEditing(var);
		endEditing();
		UpdateInspectorDragState();
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<int>(type)) {
		int val = MetaValue<int>(var);
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::DragInt(("##" + name).c_str(), &val)) {
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, val);
		}
		startEditing(var);
		endEditing();
		UpdateInspectorDragState();
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<std::uint32_t>(type)) {
		std::uint32_t val = MetaValue<std::uint32_t>(var);
		const auto* animatorMetadata = compName == "AnimatorComponent"
			? TerraEngine::Editor::Animation::FindAnimatorInspectorProperty(name)
			: nullptr;
		if(animatorMetadata != nullptr
			&& animatorMetadata->control
				== TerraEngine::Editor::Animation::AnimatorInspectorControlKind::ReadOnly)
		{
			const std::string readOnlyValue = name == "schemaVersion"
				? std::to_string(val) + " (managed by editor)"
				: val == 0U
					? "None (use Animation Diagnostics)"
					: "Legacy flags: 0x" + [&]() {
						char text[16]{};
						std::snprintf(text, sizeof(text), "%08X", val);
						return std::string(text);
					}();
			char readOnlyBuffer[128]{};
			std::strncpy(
				readOnlyBuffer,
				readOnlyValue.c_str(),
				sizeof(readOnlyBuffer) - 1U);
			ImGui::SetNextItemWidth(-1.0F);
			ImGui::InputText(
				("##" + name).c_str(),
				readOnlyBuffer,
				sizeof(readOnlyBuffer),
				ImGuiInputTextFlags_ReadOnly);
		}
		else if(animatorMetadata != nullptr
			&& animatorMetadata->control
				== TerraEngine::Editor::Animation::AnimatorInspectorControlKind::Choice)
		{
			const auto* selected =
				TerraEngine::Editor::Animation::FindAnimatorInspectorChoice(*animatorMetadata, val);
			const std::string unknown = "Unknown (" + std::to_string(val) + ")";
			const char* preview = selected != nullptr ? selected->label.data() : unknown.c_str();
			ImGui::SetNextItemWidth(-1.0F);
			if(ImGui::BeginCombo(("##" + name).c_str(), preview))
			{
				for(const auto& choice : animatorMetadata->choices)
				{
					const bool isSelected = choice.value == val;
					if(ImGui::Selectable(choice.label.data(), isSelected) && !isSelected)
					{
						const ReflectionVariant oldValue =
							access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, choice.value);
						const ReflectionVariant newValue =
							access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						UndoRedoSystem::Instance().PushCommand(
							std::make_unique<ModifyComponentCommand>(
								obj, compName, name, oldValue, newValue));
						NotifySceneAfterComponentMutation(compName);
					}
					if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
					{
						ImGui::SetTooltip("%.*s",
							static_cast<int>(choice.guidance.size()),
							choice.guidance.data());
					}
					if(isSelected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
		}
		else
		{
			const bool isAnimatorStartFrame = animatorMetadata != nullptr
				&& animatorMetadata->control
					== TerraEngine::Editor::Animation::AnimatorInspectorControlKind::UnsignedInteger
				&& name == "defaultClipStartFrame";
			if(isAnimatorStartFrame)
			{
				const ReflectionVariant clipVariant = access.entityGetter(
					ObjectManager::instance().getRegistry(),
					obj,
					"defaultClipAssetId");
				const TerraEngine::UUID clipAssetId =
					IsValid(clipVariant)
						&& IsMetaValueType<TerraEngine::UUID>(clipVariant)
					? MetaValue<TerraEngine::UUID>(clipVariant)
					: TerraEngine::EMPTY_UUID;
				const ReflectionVariant controllerVariant = access.entityGetter(
					ObjectManager::instance().getRegistry(),
					obj,
					"controllerAssetId");
				const TerraEngine::UUID controllerAssetId =
					IsValid(controllerVariant)
						&& IsMetaValueType<TerraEngine::UUID>(controllerVariant)
					? MetaValue<TerraEngine::UUID>(controllerVariant)
					: TerraEngine::EMPTY_UUID;
				const TerraEngine::UUID startClipAssetId =
					ResolveAnimatorStartClipAssetId(
						controllerAssetId,
						clipAssetId);
				const std::optional<std::uint32_t> maximum =
					ResolveAnimatorPlaybackMaximumFrame(startClipAssetId);
				if(maximum)
				{
					const ReflectionVariant loopVariant = access.entityGetter(
						ObjectManager::instance().getRegistry(),
						obj,
						"loopDefaultClip");
					const bool loopDefaultClip =
						IsValid(loopVariant)
							&& IsMetaValueType<bool>(loopVariant)
						? MetaValue<bool>(loopVariant)
						: true;
					const auto range =
						TerraEngine::Editor::Animation::BuildAnimatorInspectorFrameRange(
							val,
							static_cast<TerraEngine::Animation::AnimationFixedTick>(*maximum),
							loopDefaultClip);
					if(range.currentFrame != val)
					{
						// A shortened reimport may invalidate a previously legal
						// authored value. Normalize it through the shared reflected
						// setter before drawing so neither the scene nor UI can retain
						// current > max.
						access.entitySetter(
							ObjectManager::instance().getRegistry(),
							obj,
							name,
							range.currentFrame);
						NotifySceneAfterComponentMutation(compName);
						val = range.currentFrame;
					}
					// ImGui applies drag/navigation deltas before clamping. Keep the
					// temporary value signed so moving left from zero saturates there
					// instead of wrapping through uint32 and appearing at the maximum.
					std::int64_t editedFrame = static_cast<std::int64_t>(val);
					const std::int64_t minimumFrame = 0;
					const std::int64_t maximumFrame =
						static_cast<std::int64_t>(range.maximumFrame);
					const std::string format =
						std::string("%") + PRId64 + "/"
						+ std::to_string(range.maximumFrame);
					ImGui::SetNextItemWidth(-1.0F);
					if(ImGui::SliderScalar(
						("##" + name).c_str(),
						ImGuiDataType_S64,
						&editedFrame,
						&minimumFrame,
						&maximumFrame,
						format.c_str(),
						ImGuiSliderFlags_AlwaysClamp))
					{
						const std::uint32_t boundedFrame =
							TerraEngine::Editor::Animation::BuildAnimatorInspectorFrameRange(
								editedFrame,
								static_cast<TerraEngine::Animation::AnimationFixedTick>(
									range.maximumFrame),
								false).currentFrame;
						access.entitySetter(
							ObjectManager::instance().getRegistry(),
							obj,
							name,
							boundedFrame);
					}
					startEditing(var);
					endEditing();
					UpdateInspectorDragState();
				}
				else
				{
					// A missing/unpublished clip has no trustworthy maximum. Keep
					// the value read-only until publication instead of accepting an
					// input that cannot satisfy the requested clamp contract.
					char unavailable[64]{};
					std::snprintf(
						unavailable,
						sizeof(unavailable),
						"%u/?",
						val);
					ImGui::SetNextItemWidth(-1.0F);
					ImGui::BeginDisabled();
					ImGui::InputText(
						("##" + name).c_str(),
						unavailable,
						sizeof(unavailable),
						ImGuiInputTextFlags_ReadOnly);
					ImGui::EndDisabled();
				}
			}
			else
			{
				int signedValue = static_cast<int>(val);
				ImGui::SetNextItemWidth(-1.0f);
				// Generic unsigned fields remain numeric when a component has not
				// supplied a documented domain-specific control.
				if (ImGui::DragInt(("##" + name).c_str(), &signedValue, 1.0f, 0, std::numeric_limits<int>::max())) {
					const std::uint32_t clampedValue = static_cast<std::uint32_t>(std::max(0, signedValue));
					access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, clampedValue);
				}
				startEditing(var);
				endEditing();
				UpdateInspectorDragState();
			}
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<bool>(type)) {
		bool val = MetaValue<bool>(var);
		if (ImGui::Checkbox(("##" + name).c_str(), &val)) {
			// Checkboxes are instantaneous interactions, so we push the 
			// command immediately.
			ReflectionVariant oldB = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, val);
			ReflectionVariant newB = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, oldB, newB));
			NotifySceneAfterComponentMutation(compName);
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<std::string>(type)) {
		std::string val = MetaValue<std::string>(var);
		// Controller parameter JSON can be substantially longer than an ordinary
		// one-line property. Keep a bounded but useful editing buffer and always
		// reserve the final byte for a terminator.
		char buffer[2048]{};
		strncpy(buffer, val.c_str(), sizeof(buffer) - 1U);
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::InputText(("##" + name).c_str(), buffer, sizeof(buffer))) {
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, std::string(buffer));
		}
		startEditing(var);
		endEditing();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginDragDropTarget()) {
			const char* expected = nullptr;
			if (name == "sourceMarkup") expected = "ASSET_TUIML";

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (expected) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(expected)) {
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(newId)) {
						std::string path = meta->sourcePath.generic_string();
						ReflectionVariant oldV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, path);
						ReflectionVariant newV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, oldV, newV));
						NotifySceneAfterComponentMutation(compName);
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<glm::vec3>(type)) {
		glm::vec3 val = MetaValue<glm::vec3>(var);
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::DragFloat3(("##" + name).c_str(), &val.x, 0.1f)) {
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, val);
		}
		startEditing(var);
		endEditing();
		UpdateInspectorDragState();
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<glm::quat>(type)) {
		glm::quat q = MetaValue<glm::quat>(var);
		glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::DragFloat3(("##" + name).c_str(), &euler.x, 0.1f)) {
			glm::quat newQ = glm::quat(glm::radians(euler));
			access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, newQ);
		}
		startEditing(var);
		endEditing();
		UpdateInspectorDragState();
	}
	// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	else if (IsMetaType<TerraEngine::UUID>(type)) {
		TerraEngine::UUID val = MetaValue<TerraEngine::UUID>(var);
		std::string label;
		const bool found = ResolveInspectorAssetLabel(val, name, label);

		char buf[128]{};
		strncpy(buf, label.c_str(), sizeof(buf) - 1u);
		
		ImGui::SetNextItemWidth(-1.0f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (val != TerraEngine::EMPTY_UUID && !found) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
		ImGui::InputText(("##" + name).c_str(), buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
		if (val != TerraEngine::EMPTY_UUID && !found) ImGui::PopStyleColor();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupContextItem()) {
			if (ImGui::MenuItem("Clear")) {
				ReflectionVariant oldId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
				access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, TerraEngine::EMPTY_UUID);
				ReflectionVariant nextId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, oldId, nextId));
				NotifySceneAfterComponentMutation(compName);
			}
			ImGui::EndPopup();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginDragDropTarget()) {
			const char* payloads[] = { "ASSET_MESH", "ASSET_MATERIAL", "ASSET_TEXTURE", "ASSET_SHADER", "ASSET_GUID" };
			const char* expected = nullptr;

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (name == "meshId") expected = "ASSET_MESH";
			else if (name == "materialId") expected = "ASSET_MATERIAL";
			else if (name.find("Texture") != std::string::npos || name.find("texture") != std::string::npos) expected = "ASSET_TEXTURE";

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (expected) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(expected)) {
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
					ReflectionVariant oldId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
					access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, newId);
					ReflectionVariant nextId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, oldId, nextId));
					NotifySceneAfterComponentMutation(compName);
				}
			} else {
				// Generic UUID property, accept any asset payload
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const char* p : payloads) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(p)) {
						IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
						TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
						ReflectionVariant oldId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						access.entitySetter(ObjectManager::instance().getRegistry(), obj, name, newId);
						ReflectionVariant nextId = access.entityGetter(ObjectManager::instance().getRegistry(), obj, name);
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, name, oldId, nextId));
						NotifySceneAfterComponentMutation(compName);
						break;
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}
}

// InspectorWindow::DrawScriptComponentInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawScriptComponentInspector(entt::entity obj) {
	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	if (!object)
	{
		return;
	}
	ScriptComponent* scriptComponent = ObjectManager::instance().GetComponent<ScriptComponent>(*object);
	if (scriptComponent == nullptr) {
		return;
	}

	auto& assetDatabase = AssetDatabase::Instance();
	EditorContext* context = m_Owner != nullptr ? m_Owner->GetContext() : nullptr;
	const bool playMode = ScriptingSystem::Instance().IsRunning();
	const bool reloadPending = context != nullptr
		&& (context->scripting.coordinatorState == "Queued"
			|| context->scripting.coordinatorState == "Building"
			|| context->scripting.coordinatorState == "Reloading");
	auto guard = scriptComponent->acquireWriteLock();
	std::vector<ScriptBinding>& bindings = scriptComponent->GetBindings();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (bindings.empty()) {
		return;
	}

	std::map<std::string, int> shortNameCounts;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScriptBinding& binding : bindings)
	{
		shortNameCounts[ExtractScriptLeafName(binding.className)]++;
	}

	// Mirror Unity's inspector model: each binding gets its own component
	// card even though Terra stores all bindings inside one ScriptComponent.
	for (std::size_t bindingIndex = 0; bindingIndex < bindings.size(); ++bindingIndex) {
		ScriptBinding& binding = bindings[bindingIndex];
		const AssetMetadata* assetMetadata = assetDatabase.FindAssetByGuid(binding.assetGuid);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (assetMetadata == nullptr && !binding.className.empty()) {
			assetMetadata = assetDatabase.FindScriptAssetByFullName(binding.className);
		}
		std::string displayName = BuildScriptBindingDisplayName(binding, assetMetadata);
		const bool needsDisambiguation = shortNameCounts[ExtractScriptLeafName(binding.className)] > 1;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (needsDisambiguation)
		{
			const std::string scriptNamespace = ReadScriptMetadataString(assetMetadata, "scriptNamespace");
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!scriptNamespace.empty())
			{
				displayName += " [" + scriptNamespace + "]";
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (assetMetadata != nullptr)
			{
				displayName += " [" + assetMetadata->sourcePath.parent_path().string() + "]";
			}
			// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			else if (!binding.className.empty())
			{
				displayName += " [" + binding.className + "]";
			}
		}
		const std::string scriptAssetName = BuildScriptAssetDisplayName(binding, assetMetadata);
		const std::string componentTitle = displayName + " (Script)";
		std::string statusMessage = reloadPending ? (context != nullptr ? context->scripting.lastSummary : std::string{}) : BuildScriptBindingStatusMessage(binding);

		ImGui::PushID(binding.bindingId.c_str());
		bool bindingEnabled = binding.enabled;
		bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
		ImVec2 headerPosition = ImGui::GetCursorScreenPos();
		float headerWidth = ImGui::GetContentRegionAvail().x;
		float headerHeight = ImGui::GetFrameHeight();

		ImGui::GetWindowDrawList()->AddRectFilled(
			headerPosition,
			ImVec2(headerPosition.x + headerWidth, headerPosition.y + headerHeight),
			ImGui::GetColorU32(ImGuiCol_Header));

		ImGui::SetNextItemAllowOverlap();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::InvisibleButton("##header_bg", ImVec2(headerWidth, headerHeight))) {
			isOpen = !isOpen;
			ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
		}

		bool removeBinding = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginPopupContextItem("ComponentSettings")) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Remove Component")) {
				removeBinding = true;
			}
			ImGui::EndPopup();
		}

		ImGui::SetCursorScreenPos(headerPosition);
		ImGui::Dummy(ImVec2(2.0f, 0.0f));
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
			isOpen = !isOpen;
			ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Checkbox("##compActive", &bindingEnabled)) {
			const std::string oldJson = scriptComponent->GetBindingsJson();
			binding.enabled = bindingEnabled;
			scriptComponent->SynchronizeStorageFromBindings();
			const std::string newJson = scriptComponent->GetBindingsJson();
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", oldJson, newJson));
		}
		ImGui::PopStyleColor();

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(componentTitle.c_str());

		ImGui::SetCursorScreenPos(ImVec2(headerPosition.x, headerPosition.y + headerHeight));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (removeBinding) {
			const std::string oldJson = scriptComponent->GetBindingsJson();
			scriptComponent->RemoveBinding(binding.bindingId);
			const std::string newJson = scriptComponent->GetBindingsJson();
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", oldJson, newJson));
			ImGui::Separator();
			ImGui::PopID();
			break;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (isOpen) {
			ImGui::Indent();
			char scriptBuffer[256];
			strncpy(scriptBuffer, scriptAssetName.c_str(), sizeof(scriptBuffer));
			scriptBuffer[sizeof(scriptBuffer) - 1] = '\0';
			ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputText("##ScriptAsset", scriptBuffer, sizeof(scriptBuffer), ImGuiInputTextFlags_ReadOnly);
			const bool hoveredScriptField = ImGui::IsItemHovered();
			ImGui::EndDisabled();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assetMetadata != nullptr && hoveredScriptField) {
				ImGui::SetTooltip("%s", assetMetadata->sourcePath.string().c_str());
			}

			auto getCurrentRootValue = [&](const ScriptFieldDescriptor& rootDescriptor) -> nlohmann::json {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (playMode) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (const ScriptFieldSnapshot* liveSnapshot = binding.FindLiveSnapshot(rootDescriptor.fieldPath)) {
						return liveSnapshot->value;
					}

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (const ScriptFieldValue* runtimeOverride = binding.FindRuntimeOverride(rootDescriptor.fieldPath)) {
						return runtimeOverride->value;
					}
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (const ScriptFieldValue* authoredValue = binding.FindSerializedValue(rootDescriptor.fieldPath)) {
					return authoredValue->value;
				}

				return BuildDefaultScriptFieldJson(rootDescriptor);
			};

			auto commitRootValue = [&](const ScriptFieldDescriptor& rootDescriptor, const nlohmann::json& nextRootValue, bool pushUndoImmediately) -> bool {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (playMode) {
					std::string liveWriteError;
					if (!ScriptingSystem::Instance().WriteLiveBindingValue(static_cast<uint32_t>(obj), binding, rootDescriptor.fieldPath, nextRootValue, &liveWriteError)) {
						m_LastScriptMessage = liveWriteError.empty() ? "TerraEngine could not apply the live script edit." : liveWriteError;
						m_LastScriptMessageIsError = true;
						return false;
					}

					m_LastScriptMessage.clear();
					m_LastScriptMessageIsError = false;
					return true;
				}

				const std::string oldJson = scriptComponent->GetBindingsJson();
				ScriptFieldValue& storedValue = binding.UpsertSerializedValue(rootDescriptor.fieldPath, rootDescriptor.valueKind);
				storedValue.fieldPath = rootDescriptor.fieldPath;
				storedValue.fieldName = rootDescriptor.fieldPath;
				storedValue.displayName = rootDescriptor.displayName;
				storedValue.valueKind = rootDescriptor.valueKind;
				storedValue.value = nextRootValue;
				storedValue.compatibility = "Current";
				storedValue.persistenceState = "Current";
				storedValue.sourceTypeFingerprint = rootDescriptor.typeFingerprint;
				storedValue.diagnosticMessage.clear();
				scriptComponent->SynchronizeStorageFromBindings();
				const std::string newJson = scriptComponent->GetBindingsJson();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (pushUndoImmediately && oldJson != newJson) {
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", oldJson, newJson));
				}

				return true;
			};

			auto removeStoredField = [&](std::string_view fieldPath) {
				const std::string oldJson = scriptComponent->GetBindingsJson();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (binding.RemoveSerializedValue(fieldPath)) {
					scriptComponent->SynchronizeStorageFromBindings();
					const std::string newJson = scriptComponent->GetBindingsJson();
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", oldJson, newJson));
				}
			};

			auto renderField = [&](const auto& self, const ScriptFieldDescriptor& rootDescriptor, const ScriptFieldDescriptor& descriptor) -> void {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (descriptor.hiddenInInspector) {
					return;
				}

				nlohmann::json currentRootValue = getCurrentRootValue(rootDescriptor);
				const nlohmann::json* resolvedValue = ResolveScriptJsonPath(currentRootValue, rootDescriptor.fieldPath, descriptor.fieldPath);
				const nlohmann::json currentValue = resolvedValue != nullptr ? *resolvedValue : BuildDefaultScriptFieldJson(descriptor);
				const bool editable = descriptor.editable && descriptor.valueKind != "Unsupported" && !reloadPending && binding.status != "BrokenType";

				ImGui::PushID(descriptor.fieldPath.c_str());
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (descriptor.containerKind == "NestedObject") {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::TreeNodeEx(descriptor.displayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
						// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
						for (const ScriptFieldDescriptor& child : descriptor.children) {
							self(self, rootDescriptor, child);
						}
						ImGui::TreePop();
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
					}
					ImGui::PopID();
					return;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (descriptor.containerKind == "Collection") {
					nlohmann::json collectionValue = currentValue.is_array() ? currentValue : nlohmann::json::array();
					const std::string treeLabel = descriptor.displayName + " [" + std::to_string(collectionValue.size()) + "]";
					const bool open = ImGui::TreeNodeEx(treeLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (editable) {
						ImGui::SameLine();
						if (ImGui::SmallButton("+")) {
							nlohmann::json nextRootValue = currentRootValue;
							nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (collectionTarget != nullptr) {
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (!collectionTarget->is_array()) {
									*collectionTarget = nlohmann::json::array();
								}

								collectionTarget->push_back(descriptor.children.empty() ? nullptr : BuildDefaultScriptFieldJson(descriptor.children.front()));
								commitRootValue(rootDescriptor, nextRootValue, true);
							}
						}
					}

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (open) {
						for (std::size_t elementIndex = 0; elementIndex < collectionValue.size(); ++elementIndex) {
							if (!descriptor.children.empty()) {
								ScriptFieldDescriptor elementDescriptor = MaterializeCollectionDescriptor(descriptor.children.front(), elementIndex);
								ImGui::PushID(static_cast<int>(elementIndex));
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (editable) {
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (ImGui::SmallButton("x")) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
										if (collectionTarget != nullptr && collectionTarget->is_array() && elementIndex < collectionTarget->size()) {
											collectionTarget->erase(collectionTarget->begin() + static_cast<nlohmann::json::difference_type>(elementIndex));
											commitRootValue(rootDescriptor, nextRootValue, true);
										}
									}
									ImGui::SameLine();
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (ImGui::SmallButton("^") && elementIndex > 0) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
										if (collectionTarget != nullptr && collectionTarget->is_array()) {
											std::swap((*collectionTarget)[elementIndex], (*collectionTarget)[elementIndex - 1]);
											commitRootValue(rootDescriptor, nextRootValue, true);
										}
									}
									ImGui::SameLine();
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (ImGui::SmallButton("v") && elementIndex + 1 < collectionValue.size()) {
										nlohmann::json nextRootValue = currentRootValue;
										nlohmann::json* collectionTarget = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true);
										// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
										if (collectionTarget != nullptr && collectionTarget->is_array()) {
											std::swap((*collectionTarget)[elementIndex], (*collectionTarget)[elementIndex + 1]);
											commitRootValue(rootDescriptor, nextRootValue, true);
										}
									}
									ImGui::SameLine();
								}

								self(self, rootDescriptor, elementDescriptor);
								ImGui::PopID();
							}
						}

						ImGui::TreePop();
					}

					ImGui::PopID();
					return;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!descriptor.readOnlyReason.empty() && descriptor.valueKind == "Unsupported") {
					ImGui::TextDisabled("%s", descriptor.displayName.c_str());
					ImGui::TextWrapped("%s", descriptor.readOnlyReason.c_str());
					ImGui::PopID();
					return;
				}

				auto startEditing = [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::IsItemActivated()) {
						m_PreEditScriptJson = scriptComponent->GetBindingsJson();
					}
				};

				auto endEditing = [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::IsItemDeactivatedAfterEdit()) {
						const std::string newJson = scriptComponent->GetBindingsJson();
						if (m_PreEditScriptJson != newJson) {
							UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", m_PreEditScriptJson, newJson));
						}
					}
				};

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (descriptor.valueKind == "Bool") {
					bool editedValue = currentValue.is_boolean() ? currentValue.get<bool>() : false;
					if (ImGui::Checkbox(descriptor.displayName.c_str(), &editedValue) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue, true);
						}
					}
				} else if (descriptor.valueKind == "Int") {
					int editedValue = currentValue.is_number_integer() ? currentValue.get<int>() : 0;
					const bool changed = descriptor.hasRange
						? ImGui::SliderInt(descriptor.displayName.c_str(), &editedValue, static_cast<int>(descriptor.rangeMin), static_cast<int>(descriptor.rangeMax))
						: ImGui::DragInt(descriptor.displayName.c_str(), &editedValue, 1.0f);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "Float") {
					float editedValue = currentValue.is_number() ? currentValue.get<float>() : 0.0f;
					const bool changed = descriptor.hasRange
						? ImGui::SliderFloat(descriptor.displayName.c_str(), &editedValue, static_cast<float>(descriptor.rangeMin), static_cast<float>(descriptor.rangeMax))
						: ImGui::DragFloat(descriptor.displayName.c_str(), &editedValue, 0.1f);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = editedValue;
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "String") {
					std::string editedValue = currentValue.is_string() ? currentValue.get<std::string>() : std::string{};
					char textBuffer[256];
					strncpy(textBuffer, editedValue.c_str(), sizeof(textBuffer));
					textBuffer[sizeof(textBuffer) - 1] = '\0';
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::InputText(descriptor.displayName.c_str(), textBuffer, sizeof(textBuffer)) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = std::string(textBuffer);
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();
				} else if (descriptor.valueKind == "MeshAssetReference"
					|| descriptor.valueKind == "MaterialAssetReference"
					|| descriptor.valueKind == "TextureAssetReference"
					|| descriptor.valueKind == "ShaderAssetReference") {
					std::string editedValue = currentValue.is_string() ? currentValue.get<std::string>() : std::string{};
					char textBuffer[256];
					strncpy(textBuffer, editedValue.c_str(), sizeof(textBuffer));
					textBuffer[sizeof(textBuffer) - 1] = '\0';
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::InputText(descriptor.displayName.c_str(), textBuffer, sizeof(textBuffer)) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = std::string(textBuffer);
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::BeginPopupContextItem()) {
						if (ImGui::MenuItem("Clear") && editable) {
							nlohmann::json nextRootValue = currentRootValue;
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
								*targetValue = std::string{};
								commitRootValue(rootDescriptor, nextRootValue, true);
							}
						}
						ImGui::EndPopup();
					}

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (editable && ImGui::BeginDragDropTarget()) {
						const char* payloadType = ScriptAssetPayloadType(descriptor.valueKind);
						const char* expectedImporter = ScriptAssetImporterName(descriptor.valueKind);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (payloadType != nullptr) {
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType)) {
								IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
								const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
								const AssetMetadata* metadata = assetDatabase.FindAssetByGuid(assetGuid);
								// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
								if (metadata != nullptr
									&& expectedImporter != nullptr
									&& metadata->importerName == expectedImporter) {
									const std::string assetReference = BuildScriptAssetReferenceValue(*metadata);
									nlohmann::json nextRootValue = currentRootValue;
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
										*targetValue = assetReference;
										commitRootValue(rootDescriptor, nextRootValue, true);
									}
								}
							}
						}
						ImGui::EndDragDropTarget();
					}
				} else if (descriptor.valueKind == "Enum") {
					int selectedIndex = 0;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (currentValue.is_string()) {
						auto optionIt = std::find(descriptor.enumOptions.begin(), descriptor.enumOptions.end(), currentValue.get<std::string>());
						if (optionIt != descriptor.enumOptions.end()) {
							selectedIndex = static_cast<int>(std::distance(descriptor.enumOptions.begin(), optionIt));
						}
					}

					std::vector<const char*> options;
					options.reserve(descriptor.enumOptions.size());
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (const std::string& option : descriptor.enumOptions) {
						options.push_back(option.c_str());
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!options.empty() && ImGui::Combo(descriptor.displayName.c_str(), &selectedIndex, options.data(), static_cast<int>(options.size())) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = descriptor.enumOptions[static_cast<std::size_t>(selectedIndex)];
							commitRootValue(rootDescriptor, nextRootValue, true);
						}
					}
				} else if (descriptor.valueKind == "Vector2" || descriptor.valueKind == "Vector3" || descriptor.valueKind == "Vector4" || descriptor.valueKind == "Quaternion") {
					const int componentCount = descriptor.valueKind == "Vector2" ? 2 : descriptor.valueKind == "Vector3" ? 3 : 4;
					float components[4] = { 0.0f, 0.0f, 0.0f, descriptor.valueKind == "Quaternion" ? 1.0f : 0.0f };
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (currentValue.is_array()) {
						// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
						for (int componentIndex = 0; componentIndex < componentCount && componentIndex < static_cast<int>(currentValue.size()); ++componentIndex) {
							components[componentIndex] = currentValue[static_cast<std::size_t>(componentIndex)].get<float>();
						}
					}

					const bool changed = componentCount == 2
						? ImGui::DragFloat2(descriptor.displayName.c_str(), components, 0.1f)
						: componentCount == 3
							? ImGui::DragFloat3(descriptor.displayName.c_str(), components, 0.1f)
							: ImGui::DragFloat4(descriptor.displayName.c_str(), components, 0.1f);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (changed && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							nlohmann::json nextVector = nlohmann::json::array();
							// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
							for (int componentIndex = 0; componentIndex < componentCount; ++componentIndex) {
								nextVector.push_back(components[componentIndex]);
							}
							*targetValue = std::move(nextVector);
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();
					UpdateInspectorDragState();
				} else if (descriptor.valueKind == "EntityReference") {
					int entityId = currentValue.is_number_integer() ? currentValue.get<int>() : 0;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragInt(descriptor.displayName.c_str(), &entityId, 1.0f, 0) && editable) {
						nlohmann::json nextRootValue = currentRootValue;
						if (nlohmann::json* targetValue = ResolveScriptJsonPath(nextRootValue, rootDescriptor.fieldPath, descriptor.fieldPath, true)) {
							*targetValue = entityId;
							commitRootValue(rootDescriptor, nextRootValue, false);
						}
					}
					startEditing();
					endEditing();
					UpdateInspectorDragState();
				} else {
					std::string readOnlyValue = currentValue.is_discarded() ? "None" : currentValue.dump();
					ImGui::TextDisabled("%s: %s", descriptor.displayName.c_str(), readOnlyValue.c_str());
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!descriptor.tooltip.empty() && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", descriptor.tooltip.c_str());
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (context != nullptr) {
					if (ImGui::IsItemActive()) {
						context->scripting.activeEditedBindingId = binding.bindingId;
						context->scripting.activeEditedFieldPath = descriptor.fieldPath;
					} else if (context->scripting.activeEditedBindingId == binding.bindingId && context->scripting.activeEditedFieldPath == descriptor.fieldPath) {
						context->scripting.activeEditedBindingId.clear();
						context->scripting.activeEditedFieldPath.clear();
					}
				}

				ImGui::PopID();
			};

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!binding.fieldDescriptors.empty()) {
				for (const ScriptFieldDescriptor& rootDescriptor : binding.fieldDescriptors) {
					renderField(renderField, rootDescriptor, rootDescriptor);
				}
			} else {
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const ScriptFieldValue& fieldValue : binding.serializedValues) {
					ImGui::TextDisabled("%s: %s", fieldValue.fieldName.c_str(), FormatScriptFieldValue(fieldValue).c_str());
				}
			}

			std::string brokenFieldToRemove;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const ScriptFieldValue& fieldValue : binding.serializedValues) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (fieldValue.compatibility != "Broken") {
					continue;
				}

				ImGui::PushID((fieldValue.fieldPath + ":broken").c_str());
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "%s is stale after reload.", fieldValue.displayName.empty() ? fieldValue.fieldName.c_str() : fieldValue.displayName.c_str());
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!fieldValue.diagnosticMessage.empty()) {
					ImGui::TextWrapped("%s", fieldValue.diagnosticMessage.c_str());
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::SmallButton("Clear Value")) {
					brokenFieldToRemove = fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath;
				}
				ImGui::SameLine();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::SmallButton("Remove Stale Data")) {
					brokenFieldToRemove = fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath;
				}
				ImGui::PopID();

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!brokenFieldToRemove.empty()) {
					break;
				}
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!brokenFieldToRemove.empty()) {
				removeStoredField(brokenFieldToRemove);
			}

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const ScriptFieldDiagnostic& diagnostic : binding.diagnostics) {
				if (diagnostic.message.empty()) {
					continue;
				}

				// Unsupported fields already render their read-only explanation
				// inline, so repeating the same warning below every frame only adds
				// noisy duplicate yellow rows to the inspector.
				if (diagnostic.code == "UnsupportedType") {
					continue;
				}

				const ImVec4 diagnosticColor = diagnostic.severity == "Error"
					? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
					: diagnostic.severity == "Warning"
						? ImVec4(1.0f, 0.8f, 0.35f, 1.0f)
						: ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
				ImGui::TextColored(diagnosticColor, "%s", diagnostic.message.c_str());
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!statusMessage.empty()) {
				ImGui::TextColored(reloadPending ? ImVec4(1.0f, 0.8f, 0.35f, 1.0f) : BuildScriptBindingStatusColor(binding), "%s", statusMessage.c_str());
			}
			ImGui::Unindent();
		}

		ImGui::Separator();
		ImGui::PopID();
	}
}

// InspectorWindow::DrawScriptDropTarget is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawScriptDropTarget(entt::entity obj) {
	// Use the visible inspector body as the drop zone so scripts can be
	// attached by drag-and-drop without rendering a dedicated placeholder box.
	ImGuiWindow* inspectorWindow = ImGui::GetCurrentWindow();
	const ImRect dropBounds(inspectorWindow->InnerRect.Min, inspectorWindow->InnerRect.Max);
	if (!ImGui::BeginDragDropTargetCustom(dropBounds, inspectorWindow->GetID("InspectorScriptDropTarget"))) {
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_SCRIPT")) {
		IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
		const TerraEngine::UUID scriptGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
		const AssetMetadata* scriptMetadata = AssetDatabase::Instance().FindAssetByGuid(scriptGuid);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scriptMetadata == nullptr) {
			m_LastScriptMessage = "The dropped script asset no longer exists in the asset database.";
			m_LastScriptMessageIsError = true;
		} else {
			const std::string className = AssetDatabase::Instance().GetScriptFullName(scriptGuid);
			const bool sourceLooksAttachable = scriptMetadata->settings.contains("isAttachable")
				&& scriptMetadata->settings["isAttachable"].get<bool>();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (className.empty()) {
				m_LastScriptMessage = "The dropped script file does not currently declare a usable gameplay class.";
				m_LastScriptMessageIsError = true;
			} else if (!sourceLooksAttachable) {
				m_LastScriptMessage = "The dropped script source does not currently inherit from TerraEngine.Script.";
				m_LastScriptMessageIsError = true;
			} else {
				std::optional<Object> object = ObjectManager::instance().getObject(obj);
				ScriptComponent* scriptComponent = object ? ObjectManager::instance().GetComponent<ScriptComponent>(*object) : nullptr;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (scriptComponent == nullptr && object) {
					scriptComponent = ObjectManager::instance().AddComponent<ScriptComponent>(*object);
				}
				if (scriptComponent == nullptr) {
					m_LastScriptMessage = "Could not attach ScriptComponent to the selected object.";
					m_LastScriptMessageIsError = true;
					return;
				}

				auto guard = scriptComponent->acquireWriteLock();
				const std::string oldJson = scriptComponent->GetBindingsJson();
				// Terra mirrors Unity here: one object can hold multiple script
				// bindings, including multiple instances of the exact same class.
				ScriptBinding& binding = scriptComponent->AddBinding(scriptGuid, className);
				ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
				scriptingSystem.InitializeBindingMetadata(binding);
				const bool assemblyCanAttachImmediately = binding.status == "Attached";
				scriptComponent->SynchronizeStorageFromBindings();
				const std::string newJson = scriptComponent->GetBindingsJson();
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, "Script", "BindingsJson", oldJson, newJson));

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (assemblyCanAttachImmediately) {
					m_LastScriptMessage = "Attached " + className + " to the selected object.";
				} else {
					// Persist the binding immediately, but do not force a rebuild just
					// because the user attached an existing script asset in the editor.
					// If the gameplay assembly is stale or missing, the binding will
					// activate on the next normal successful compile or reload.
					m_LastScriptMessage = "Added " + className + " to the selected object. The binding will activate when the gameplay assembly is next ready.";
				}
				m_LastScriptMessageIsError = false;
			}
		}
	}

	ImGui::EndDragDropTarget();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_LastScriptMessage.empty()) {
		const ImVec4 messageColor = m_LastScriptMessageIsError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.5f, 0.85f, 0.5f, 1.0f);
		ImGui::TextColored(messageColor, "%s", m_LastScriptMessage.c_str());
	}
}

// InspectorWindow::DrawLightComponentInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawLightComponentInspector(const std::string& compName, entt::entity obj, ComponentAccessFunctions& access)
{
	auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);
		widgetFunc();
	};

	// Keeping undo/redo efficient by updating live values during interaction
	// and only committing the final state to the undo stack on deactivation.
	auto setFloatProperty = [&](const char* propertyName, float newValue) {
		ReflectionVariant oldVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		if (!IsValid(oldVariant) || !IsMetaValueType<float>(oldVariant))
		{
			return;
		}

		const float oldValue = MetaValue<float>(oldVariant);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::abs(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.entitySetter(ObjectManager::instance().getRegistry(), obj, propertyName, newValue);
	};

	auto setBoolProperty = [&](const char* propertyName, bool newValue) {
		ReflectionVariant oldVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(oldVariant) || !IsMetaValueType<bool>(oldVariant))
		{
			return;
		}

		const bool oldValue = MetaValue<bool>(oldVariant);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (oldValue == newValue)
		{
			return;
		}

		access.entitySetter(ObjectManager::instance().getRegistry(), obj, propertyName, newValue);
		
		// Checkboxes are instantaneous, push immediately
		ReflectionVariant nextVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, propertyName, oldVariant, nextVariant));
	};

	auto setUintProperty = [&](const char* propertyName, std::uint32_t newValue) {
		ReflectionVariant oldVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(oldVariant) || !IsMetaValueType<std::uint32_t>(oldVariant))
		{
			return;
		}

		const std::uint32_t oldValue = MetaValue<std::uint32_t>(oldVariant);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (oldValue == newValue)
		{
			return;
		}

		access.entitySetter(ObjectManager::instance().getRegistry(), obj, propertyName, newValue);
	};

	auto setVec3Property = [&](const char* propertyName, const glm::vec3& newValue) {
		ReflectionVariant oldVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(oldVariant) || !IsMetaValueType<glm::vec3>(oldVariant))
		{
			return;
		}

		const glm::vec3 oldValue = MetaValue<glm::vec3>(oldVariant);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (glm::length(oldValue - newValue) <= 0.0001f)
		{
			return;
		}

		access.entitySetter(ObjectManager::instance().getRegistry(), obj, propertyName, newValue);
	};

	auto startEditing = [&](const char* propertyName) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemActivated()) {
			m_PreEditValue = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		}
	};

	auto endEditing = [&](const char* propertyName) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			ReflectionVariant newVal = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, propertyName, m_PreEditValue, newVal));
		}
	};

	constexpr std::uint32_t kMaxAuthorableType = static_cast<std::uint32_t>(LightType::Area);
	std::uint32_t encodedType = static_cast<std::uint32_t>(LightType::Point);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ReflectionVariant typeVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "type"); IsValid(typeVariant) && IsMetaValueType<std::uint32_t>(typeVariant))
	{
		encodedType = MetaValue<std::uint32_t>(typeVariant);
	}
	const bool hasUnsupportedType = encodedType > kMaxAuthorableType;
	std::uint32_t effectiveType = hasUnsupportedType ? static_cast<std::uint32_t>(LightType::Point) : encodedType;

	const bool isDirectional = effectiveType == static_cast<std::uint32_t>(LightType::Directional);
	const bool isSpot = effectiveType == static_cast<std::uint32_t>(LightType::Spot);
	const bool isArea = effectiveType == static_cast<std::uint32_t>(LightType::Area);

	const ImGuiTreeNodeFlags sectionFlags = ImGuiTreeNodeFlags_DefaultOpen;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::CollapsingHeader("General", sectionFlags))
	{
		if (ImGui::BeginTable("LightGeneral", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

			const char* lightTypes[] = { "Directional", "Point", "Spot", "Area" };
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (hasUnsupportedType)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.24f, 1.0f), "Unsupported legacy light type value: %u", encodedType);
				ImGui::TextUnformatted("Select a supported type to replace it.");
			}

			int typeIndex = hasUnsupportedType
				? static_cast<int>(LightType::Point)
				: static_cast<int>(effectiveType);
			drawLabeledWidget("Type", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Combo("##Type", &typeIndex, lightTypes, IM_ARRAYSIZE(lightTypes)))
				{
					typeIndex = std::clamp(typeIndex, 0, static_cast<int>(kMaxAuthorableType));
					effectiveType = static_cast<std::uint32_t>(typeIndex);
					
					ReflectionVariant oldV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "type");
					setUintProperty("type", effectiveType);
					ReflectionVariant nextV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "type");
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "type", oldV, nextV));
				}
			});

			std::uint32_t currentAppearance = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant appVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "appearance"); IsValid(appVariant) && IsMetaValueType<std::uint32_t>(appVariant))
			{
				currentAppearance = MetaValue<std::uint32_t>(appVariant);
			}

			const char* appearanceTypes[] = { "Color", "Filter and Temperature" };
			int appIndex = static_cast<int>(currentAppearance);
			drawLabeledWidget("Appearance", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Combo("##Appearance", &appIndex, appearanceTypes, IM_ARRAYSIZE(appearanceTypes)))
				{
					ReflectionVariant oldV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "appearance");
					setUintProperty("appearance", static_cast<std::uint32_t>(appIndex));
					ReflectionVariant nextV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "appearance");
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "appearance", oldV, nextV));
				}
			});

			glm::vec3 lightColor = glm::vec3(1.0f);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant colorVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "color"); IsValid(colorVariant) && IsMetaValueType<glm::vec3>(colorVariant))
			{
				lightColor = MetaValue<glm::vec3>(colorVariant);
			}
			drawLabeledWidget(currentAppearance == 0u ? "Color" : "Filter", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::ColorEdit3("##Color", &lightColor.x))
				{
					setVec3Property("color", lightColor);
				}
				startEditing("color");
				endEditing("color");
			});

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentAppearance == 1u)
			{
				float temperature = 6500.0f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant tempVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "temperature"); IsValid(tempVariant) && IsMetaValueType<float>(tempVariant))
				{
					temperature = MetaValue<float>(tempVariant);
				}
				drawLabeledWidget("Temperature", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::SliderFloat("##Temperature", &temperature, 1000.0f, 20000.0f, "%.0f K"))
					{
						setFloatProperty("temperature", temperature);
					}
					startEditing("temperature");
					endEditing("temperature");
				});
			}

			float intensity = 1.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant intensityVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "intensity"); IsValid(intensityVariant) && IsMetaValueType<float>(intensityVariant))
			{
				intensity = MetaValue<float>(intensityVariant);
			}

			std::uint32_t currentUnit = 0u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant unitVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "unit"); IsValid(unitVariant) && IsMetaValueType<std::uint32_t>(unitVariant))
			{
				currentUnit = MetaValue<std::uint32_t>(unitVariant);
			}

			const char* intensityFormat = "%.2f";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isDirectional)
			{
				intensityFormat = "%.2f lux";
			}
			else
			{
				intensityFormat = currentUnit == 0u ? "%.2f lm" : "%.2f lux";
			}

			drawLabeledWidget("Intensity", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat("##Intensity", &intensity, 0.05f, 0.0f, 1000000.0f, intensityFormat))
				{
					setFloatProperty("intensity", std::max(0.0f, intensity));
				}
				startEditing("intensity");
				endEditing("intensity");
			});
			UpdateInspectorDragState();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!isDirectional)
			{
				const char* lightUnits[] = { "Lumens", "Lux" };
				int unitIndex = std::clamp(static_cast<int>(currentUnit), 0, 1);
				drawLabeledWidget("Unit", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Combo("##Unit", &unitIndex, lightUnits, IM_ARRAYSIZE(lightUnits)))
					{
						ReflectionVariant oldUnit = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "unit");
						ReflectionVariant oldInt = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "intensity");

						setUintProperty("unit", static_cast<std::uint32_t>(unitIndex));

						// Convert intensity value to maintain perceived brightness
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (currentUnit == 0u && unitIndex == 1) // Lumens -> Lux
						{
							setFloatProperty("intensity", std::max(0.0f, intensity / (4.0f * glm::pi<float>())));
						}
						else if (currentUnit == 1u && unitIndex == 0) // Lux -> Lumens
						{
							setFloatProperty("intensity", std::max(0.0f, intensity * (4.0f * glm::pi<float>())));
						}

						ReflectionVariant newUnit = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "unit");
						ReflectionVariant newInt = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "intensity");
						
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "unit", oldUnit, newUnit));
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "intensity", oldInt, newInt));
					}
				});
			}

			float indirectMultiplier = 1.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant indVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "indirectMultiplier"); IsValid(indVariant) && IsMetaValueType<float>(indVariant))
			{
				indirectMultiplier = MetaValue<float>(indVariant);
			}
			drawLabeledWidget("Indirect", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat("##Indirect", &indirectMultiplier, 0.05f, 0.0f, 10.0f))
				{
					setFloatProperty("indirectMultiplier", std::max(0.0f, indirectMultiplier));
				}
				startEditing("indirectMultiplier");
				endEditing("indirectMultiplier");
			});
			UpdateInspectorDragState();

			ImGui::EndTable();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::CollapsingHeader("Shape", sectionFlags))
	{
		if (ImGui::BeginTable("LightShape", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!isDirectional)
			{
				float range = 10.0f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant rangeVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "range"); IsValid(rangeVariant) && IsMetaValueType<float>(rangeVariant))
				{
					range = MetaValue<float>(rangeVariant);
				}
				drawLabeledWidget("Range", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##Range", &range, 0.1f, 0.1f, 10000.0f, "%.2f m"))
					{
						setFloatProperty("range", std::max(0.1f, range));
					}
					startEditing("range");
					endEditing("range");
				});
				UpdateInspectorDragState();
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isSpot)
			{
				float innerDegrees = 20.0f;
				float outerDegrees = 30.0f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant innerVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "innerConeAngle"); IsValid(innerVariant) && IsMetaValueType<float>(innerVariant))
				{
					innerDegrees = glm::degrees(MetaValue<float>(innerVariant));
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant outerVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "outerConeAngle"); IsValid(outerVariant) && IsMetaValueType<float>(outerVariant))
				{
					outerDegrees = glm::degrees(MetaValue<float>(outerVariant));
				}

				drawLabeledWidget("Inner Angle", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##InnerAngle", &innerDegrees, 0.1f, 0.0f, 89.0f, "%.1f deg")) {
						innerDegrees = std::clamp(innerDegrees, 0.0f, 89.0f);
						outerDegrees = std::clamp(outerDegrees, innerDegrees + 0.1f, 89.9f);
						setFloatProperty("innerConeAngle", glm::radians(innerDegrees));
						setFloatProperty("outerConeAngle", glm::radians(outerDegrees));
					}
					startEditing("innerConeAngle");
					endEditing("innerConeAngle");
					// Outer angle also changes if inner was pushed past it
					startEditing("outerConeAngle");
					endEditing("outerConeAngle");
				});
				UpdateInspectorDragState();

				drawLabeledWidget("Outer Angle", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##OuterAngle", &outerDegrees, 0.1f, 0.1f, 89.9f, "%.1f deg")) {
						innerDegrees = std::clamp(innerDegrees, 0.0f, 89.0f);
						outerDegrees = std::clamp(outerDegrees, innerDegrees + 0.1f, 89.9f);
						setFloatProperty("innerConeAngle", glm::radians(innerDegrees));
						setFloatProperty("outerConeAngle", glm::radians(outerDegrees));
					}
					startEditing("innerConeAngle");
					endEditing("innerConeAngle");
					startEditing("outerConeAngle");
					endEditing("outerConeAngle");
				});
				UpdateInspectorDragState();
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (isArea)
			{
				float width = 1.0f;
				float height = 1.0f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant wVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "width"); IsValid(wVariant) && IsMetaValueType<float>(wVariant))
				{
					width = MetaValue<float>(wVariant);
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant hVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "height"); IsValid(hVariant) && IsMetaValueType<float>(hVariant))
				{
					height = MetaValue<float>(hVariant);
				}

				drawLabeledWidget("Width", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##Width", &width, 0.1f, 0.01f, 1000.0f, "%.2f m")) setFloatProperty("width", std::max(0.01f, width));
					startEditing("width");
					endEditing("width");
				});
				UpdateInspectorDragState();

				drawLabeledWidget("Height", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##Height", &height, 0.1f, 0.01f, 1000.0f, "%.2f m")) setFloatProperty("height", std::max(0.01f, height));
					startEditing("height");
					endEditing("height");
				});
				UpdateInspectorDragState();
			}

			ImGui::EndTable();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::CollapsingHeader("Shadows", sectionFlags))
	{
		if (ImGui::BeginTable("LightShadows", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

			bool castsShadows = false;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant castVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "castsShadows"); IsValid(castVariant) && IsMetaValueType<bool>(castVariant))
			{
				castsShadows = MetaValue<bool>(castVariant);
			}
			drawLabeledWidget("Realtime Shadows", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Checkbox("##RealtimeShadows", &castsShadows))
				{
					setBoolProperty("castsShadows", castsShadows);
				}
			});

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (castsShadows)
			{
				float shadowStrength = 1.0f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant strVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "shadowStrength"); IsValid(strVariant) && IsMetaValueType<float>(strVariant))
				{
					shadowStrength = MetaValue<float>(strVariant);
				}
				drawLabeledWidget("Strength", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::SliderFloat("##Strength", &shadowStrength, 0.0f, 1.0f))
					{
						setFloatProperty("shadowStrength", std::clamp(shadowStrength, 0.0f, 1.0f));
					}
					startEditing("shadowStrength");
					endEditing("shadowStrength");
				});

				float shadowBias = 0.005f;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ReflectionVariant biasVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "shadowBias"); IsValid(biasVariant) && IsMetaValueType<float>(biasVariant))
				{
					shadowBias = MetaValue<float>(biasVariant);
				}
				drawLabeledWidget("Bias", [&]() {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::DragFloat("##Bias", &shadowBias, 0.001f, 0.0f, 0.1f, "%.4f"))
					{
						setFloatProperty("shadowBias", std::clamp(shadowBias, 0.0f, 0.1f));
					}
					startEditing("shadowBias");
					endEditing("shadowBias");
				});
				UpdateInspectorDragState();
			}

			ImGui::EndTable();
		}
	}

	// These fields affect which lights survive the renderer's light budget.
	// They are always useful to inspect, regardless of light type or shadow state.
	if (ImGui::CollapsingHeader("Culling", sectionFlags))
	{
		if (ImGui::BeginTable("LightCulling", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

			float authoredPriority = 0.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant priorityVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "authoredPriority"); IsValid(priorityVariant) && IsMetaValueType<float>(priorityVariant))
			{
				authoredPriority = MetaValue<float>(priorityVariant);
			}
			drawLabeledWidget("Priority", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat("##AuthoredPriority", &authoredPriority, 0.01f, 0.0f, 1000.0f))
				{
					setFloatProperty("authoredPriority", std::max(0.0f, authoredPriority));
				}
				startEditing("authoredPriority");
				endEditing("authoredPriority");
			});
			UpdateInspectorDragState();

			float stabilityPreference = 0.0f;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ReflectionVariant stabilityVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "stabilityPreference"); IsValid(stabilityVariant) && IsMetaValueType<float>(stabilityVariant))
			{
				stabilityPreference = MetaValue<float>(stabilityVariant);
			}
			drawLabeledWidget("Stability", [&]() {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat("##StabilityPreference", &stabilityPreference, 0.01f, 0.0f, 1.0f))
				{
					setFloatProperty("stabilityPreference", std::clamp(stabilityPreference, 0.0f, 1.0f));
				}
				startEditing("stabilityPreference");
				endEditing("stabilityPreference");
			});
			UpdateInspectorDragState();

			ImGui::EndTable();
		}
	}
}

// InspectorWindow::DrawUIComponentInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawUIComponentInspector(const std::string& compName, entt::entity obj, ComponentAccessFunctions& access)
{
	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	UIComponent* uiComponent = object ? ObjectManager::instance().GetComponent<UIComponent>(*object) : nullptr;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiComponent == nullptr)
	{
		return;
	}

	auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);
		widgetFunc();
	};

	auto setStringProperty = [&](const char* propertyName, const std::string& newValue) {
		ReflectionVariant oldVariant = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsValid(oldVariant) || !IsMetaValueType<std::string>(oldVariant))
		{
			return;
		}

		const std::string oldValue = MetaValue<std::string>(oldVariant);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (oldValue == newValue)
		{
			return;
		}

		access.entitySetter(ObjectManager::instance().getRegistry(), obj, propertyName, newValue);
	};

	auto startEditing = [&](const char* propertyName) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemActivated()) {
			m_PreEditValue = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
		}
	};

	auto endEditing = [&](const char* propertyName) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			ReflectionVariant newVal = access.entityGetter(ObjectManager::instance().getRegistry(), obj, propertyName);
			UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, propertyName, m_PreEditValue, newVal));
		}
	};

	// The custom UIComponent inspector owns its own property table because the
	// generic component path is bypassed for this specialized multi-asset UI.
	if (ImGui::BeginTable("UIComponentPropertyTable", 2, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		drawLabeledWidget("Source Asset", [&]() {
			std::string markupPath = uiComponent->GetSourceMarkup();
			char markupBuffer[256];
			strncpy(markupBuffer, markupPath.c_str(), sizeof(markupBuffer));
			markupBuffer[sizeof(markupBuffer) - 1] = '\0';
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::InputText("##UIComponentSourceMarkup", markupBuffer, sizeof(markupBuffer)))
			{
				setStringProperty("sourceMarkup", markupBuffer);
			}
			startEditing("sourceMarkup");
			endEditing("sourceMarkup");

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginPopupContextItem())
			{
				if (ImGui::MenuItem("Clear"))
				{
					ReflectionVariant oldV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "sourceMarkup");
					setStringProperty("sourceMarkup", "");
					ReflectionVariant nextV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "sourceMarkup");
					UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "sourceMarkup", oldV, nextV));
				}
				ImGui::EndPopup();
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TUIML"))
				{
					IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
					const TerraEngine::UUID assetGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (const AssetMetadata* meta = AssetDatabase::Instance().FindAssetByGuid(assetGuid))
					{
						ReflectionVariant oldV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "sourceMarkup");
						setStringProperty("sourceMarkup", meta->sourcePath.generic_string());
						ReflectionVariant nextV = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "sourceMarkup");
						UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "sourceMarkup", oldV, nextV));
					}
				}
				ImGui::EndDragDropTarget();
			}
		});

		ImGui::EndTable();
	}
}

// InspectorWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::QueuePhysicsRebuildIntentsForChangedObjects(
	const std::vector<TerraEngine::UUID>& objectIds, std::string reason)
{
	m_PhysicsRebuildIntents.Queue(objectIds, std::move(reason));
}

std::vector<InspectorWindow::PhysicsRebuildIntent> InspectorWindow::DrainPhysicsRebuildIntentsForProtectedBoundary()
{
	// Moving the completed UI batch gives the lifecycle coordinator one explicit
	// protected-boundary handoff and prevents a provider rebuild during OnGui.
	return m_PhysicsRebuildIntents.Drain();
}

void InspectorWindow::DrawPhysicsRuntimeState(entt::entity obj)
{
	ImGui::SeparatorText("Runtime State (read-only)");
	const auto snapshot = TerraEngine::Physics::PhysicsSystem::Instance().LatestBodySnapshot();
	const TerraEngine::UUID objectId = GetObjectUuid(obj);
	if (!snapshot || !snapshot->bodies)
	{
		ImGui::TextDisabled("No published physics state.");
		return;
	}
	for (const TerraEngine::Physics::PhysicsBodyState& state : *snapshot->bodies)
	{
		if (state.objectId != objectId)
		{
			continue;
		}
		ImGui::Text("World tick: %llu", static_cast<unsigned long long>(snapshot->tick));
		ImGui::Text("Linear velocity: %.3f, %.3f, %.3f m/s", state.linearVelocity.x, state.linearVelocity.y, state.linearVelocity.z);
		ImGui::Text("Angular velocity: %.3f, %.3f, %.3f rad/s", state.angularVelocity.x, state.angularVelocity.y, state.angularVelocity.z);
		ImGui::Text("Awake: %s", state.awake ? "yes" : "no");
		return;
	}
	ImGui::TextDisabled("This object has no published runtime body.");
}

void InspectorWindow::DrawRigidBodyInspector(const std::vector<entt::entity>& entities)
{
	auto candidates = CopyPhysicsCandidates<RigidBodyComponent>(entities);
	if (candidates.empty()) return;
	auto submit = [&](const char* propertyName, auto value, auto setValue, const char* label) {
		for (auto& candidate : candidates) setValue(candidate.value, value);
		for (const auto& candidate : candidates)
		{
			const auto validation = candidate.value.Validate();
			if (!validation.accepted)
			{
				ImGui::TextColored(ImVec4(1, .45F, .25F, 1), "%s", validation.message.c_str());
				return;
			}
		}
		std::vector<std::pair<TerraEngine::UUID, decltype(value)>> values;
		for (const auto& candidate : candidates) values.emplace_back(candidate.objectId, value);
		QueuePhysicsRebuildIntentsForChangedObjects(
			SubmitPhysicsPropertyBatch(values, "RigidBodyComponent", propertyName, label, "inspector-rigidbody"), label);
	};

	std::vector<int> motions;
	for (const auto& candidate : candidates) motions.push_back(static_cast<int>(candidate.value.motionType));
	int motion = motions.front();
	const char* motionLabels[] = {"Static", "Dynamic", "Kinematic"};
	if (DrawMixedCombo("Motion Type", motion, HasMixedValues(motions), motionLabels, IM_ARRAYSIZE(motionLabels)))
		submit("motionType", static_cast<TerraEngine::Physics::PhysicsMotionType>(motion), [](auto& body, auto value) { body.motionType = value; }, "Edit Rigid Body Motion Type");

	std::vector<bool> gravities;
	for (const auto& candidate : candidates) gravities.push_back(candidate.value.useGravity);
	bool useGravity = gravities.front();
	if (DrawMixedCheckbox("Use World Gravity", useGravity, HasMixedValues(gravities)))
		submit("useGravity", useGravity, [](auto& body, bool value) { body.useGravity = value; }, "Edit Rigid Body Gravity");
	ImGui::SameLine(); ImGui::TextDisabled("(m/s^2)");

	std::vector<bool> sleepingValues;
	for (const auto& candidate : candidates) sleepingValues.push_back(candidate.value.allowSleeping);
	bool allowSleeping = sleepingValues.front();
	if (DrawMixedCheckbox("Allow Sleeping", allowSleeping, HasMixedValues(sleepingValues)))
		submit("allowSleeping", allowSleeping, [](auto& body, bool value) { body.allowSleeping = value; }, "Edit Rigid Body Sleeping");

	auto drawFloat = [&](const char* widgetLabel, const char* propertyName, auto member, float speed, float minimum, float maximum, const char* format, const char* historyLabel) {
		std::vector<float> values;
		for (const auto& candidate : candidates) values.push_back(candidate.value.*member);
		float value = values.front();
		if (DrawMixedDragFloat(widgetLabel, value, HasMixedValues(values), speed, minimum, maximum, format))
			submit(propertyName, value, [member](auto& body, float updated) { body.*member = updated; }, historyLabel);
	};
	if (std::any_of(candidates.begin(), candidates.end(), [](const auto& candidate) { return candidate.value.massMode == TerraEngine::Physics::PhysicsMassMode::ExplicitMass; }))
		drawFloat("Mass (kg)", "mass", &RigidBodyComponent::mass, .05F, .001F, 100000.0F, "%.3f", "Edit Rigid Body Mass");
	drawFloat("Linear Damping", "linearDamping", &RigidBodyComponent::linearDamping, .01F, 0.0F, 100.0F, "%.3f", "Edit Rigid Body Linear Damping");
	drawFloat("Angular Damping", "angularDamping", &RigidBodyComponent::angularDamping, .01F, 0.0F, 100.0F, "%.3f", "Edit Rigid Body Angular Damping");
	drawFloat("Max Linear Velocity (m/s)", "maxLinearVelocity", &RigidBodyComponent::maxLinearVelocity, 1.0F, .001F, 100000.0F, "%.2f", "Edit Rigid Body Velocity Limit");
	DrawPhysicsRuntimeState(entities.front());
}

void InspectorWindow::DrawColliderSetInspector(const std::vector<entt::entity>& entities)
{
	auto candidates = CopyPhysicsCandidates<ColliderSetComponent>(entities);
	if (candidates.empty()) return;
	auto commit = [&](const char* label) {
		std::vector<std::pair<TerraEngine::UUID, std::vector<TerraEngine::Physics::ColliderRecord>>> values;
		for (const auto& candidate : candidates)
		{
			const auto validation = candidate.value.Validate();
			if (!validation.accepted)
			{
				ImGui::TextColored(ImVec4(1, .45F, .25F, 1), "Collider validation: %s", validation.message.c_str());
				return;
			}
			values.emplace_back(candidate.objectId, candidate.value.colliders);
		}
		QueuePhysicsRebuildIntentsForChangedObjects(
			SubmitPhysicsPropertyBatch(values, "ColliderSetComponent", "colliders", label, "inspector-colliders"), label);
	};

	if (ImGui::Button("Add Collider"))
	{
		for (auto& candidate : candidates) candidate.value.Add();
		commit("Add Collider Records");
	}

	// Only stable IDs present on every selected object are editable as one group.
	// This avoids accidentally pairing records merely because their vector indices
	// happen to match after independent prefab overrides or reordering.
	std::vector<TerraEngine::UUID> commonRecordIds;
	for (const auto& record : candidates.front().value.colliders)
	{
		const bool presentEverywhere = std::all_of(candidates.begin() + 1, candidates.end(), [&](const auto& candidate) {
			return std::any_of(candidate.value.colliders.begin(), candidate.value.colliders.end(), [&](const auto& other) { return other.recordId == record.recordId; });
		});
		if (presentEverywhere) commonRecordIds.push_back(record.recordId);
	}
	if (candidates.size() > 1U && commonRecordIds.empty() && !candidates.front().value.colliders.empty())
	{
		ImGui::TextDisabled("No collider record IDs are shared by every selected object.");
	}

	for (std::size_t displayIndex = 0; displayIndex < commonRecordIds.size(); ++displayIndex)
	{
		const TerraEngine::UUID recordId = commonRecordIds[displayIndex];
		std::vector<TerraEngine::Physics::ColliderRecord*> records;
		for (auto& candidate : candidates)
		{
			auto record = std::find_if(candidate.value.colliders.begin(), candidate.value.colliders.end(), [&](const auto& value) { return value.recordId == recordId; });
			records.push_back(&*record);
		}
		ImGui::PushID(TerraEngine::ToHexString(recordId).c_str());
		ImGui::SeparatorText(("Collider " + std::to_string(displayIndex + 1U)).c_str());
		ImGui::TextDisabled("Stable record ID: %s", TerraEngine::ToHexString(recordId).c_str());

		std::vector<bool> enabledValues;
		for (const auto* record : records) enabledValues.push_back(record->enabled);
		bool enabled = enabledValues.front();
		if (DrawMixedCheckbox("Enabled", enabled, HasMixedValues(enabledValues)))
		{
			for (auto* record : records) record->enabled = enabled;
			commit("Edit Collider Enabled State");
		}
		std::vector<bool> triggerValues;
		for (const auto* record : records) triggerValues.push_back(record->isTrigger);
		bool trigger = triggerValues.front();
		if (DrawMixedCheckbox("Trigger", trigger, HasMixedValues(triggerValues)))
		{
			for (auto* record : records) record->isTrigger = trigger;
			commit("Edit Collider Trigger State");
		}

		// Shape handles are meaningful only for one concrete object. Multi-edit
		// remains available above, but it must not ambiguously target one viewport
		// preview while changing several independently authored collider records.
		const std::shared_ptr<SceneViewWindow> sceneView = candidates.size() == 1U && m_Owner != nullptr
			? m_Owner->GetWindow<SceneViewWindow>() : nullptr;
		if (sceneView)
		{
			glm::vec3 editCenter(
				records.front()->localCenter.x,
				records.front()->localCenter.y,
				records.front()->localCenter.z);
			glm::vec3 editHalfExtents = ColliderPreviewHalfExtents(*records.front());
			const bool editActive = sceneView->ReadPhysicsColliderPreview(
				candidates.front().objectId, recordId, editCenter, editHalfExtents);
			ImGui::SeparatorText("Shape");
			ImGui::BeginDisabled(!editActive);
			bool shapeChanged = ImGui::DragFloat3("Center (m)", &editCenter.x, .01F, -100000.0F, 100000.0F, "%.3f");
			UpdateInspectorDragState();
			shapeChanged = ImGui::DragFloat3("Half Extents (m)", &editHalfExtents.x, .01F, .001F, 1000.0F, "%.3f") || shapeChanged;
			UpdateInspectorDragState();
			ImGui::EndDisabled();
			if (shapeChanged)
			{
				sceneView->UpdatePhysicsColliderPreview(
					candidates.front().objectId, recordId, editCenter, editHalfExtents);
			}
			if (!editActive)
			{
				if (ImGui::Button("Edit Shape in Scene View"))
				{
					sceneView->BeginPhysicsColliderPreview(entities.front(), recordId);
				}
			}
			else
			{
				if (ImGui::Button("Fit to Render Mesh")) sceneView->RequestPhysicsColliderFitToRenderMesh(entities.front());
				ImGui::SameLine();
				if (ImGui::Button("Apply Collider Edit")) sceneView->CommitPhysicsColliderPreview();
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) sceneView->CancelPhysicsColliderPreview();
			}
			if (sceneView->IsPhysicsColliderPreviewSelection(recordId)
				&& !sceneView->PhysicsColliderPreviewStatus().empty())
			{
				ImGui::TextWrapped("%s", sceneView->PhysicsColliderPreviewStatus().c_str());
			}
		}
		else
		{
			// Multi-object editing has no unambiguous Scene View target, so retain one
			// direct center field here. A single-object Inspector uses the shape field
			// above and therefore never displays the same center twice.
			TerraEngine::Physics::PhysicsVector3 center = records.front()->localCenter;
			const bool mixedCenter = std::any_of(records.begin() + 1, records.end(), [&](const auto* record) { return !SameVector3(record->localCenter, center); });
			if (mixedCenter) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
			const bool centerChanged = ImGui::DragFloat3("Center (m)", &center.x, .01F, -100000.0F, 100000.0F, "%.3f");
			if (mixedCenter) ImGui::PopItemFlag();
			if (centerChanged)
			{
				for (auto* record : records) record->localCenter = center;
				commit("Edit Collider Center");
			}
		}

		if (ImGui::Button("Duplicate"))
		{
			for (auto& candidate : candidates)
			{
				auto source = std::find_if(candidate.value.colliders.begin(), candidate.value.colliders.end(), [&](const auto& value) { return value.recordId == recordId; });
				auto duplicate = *source;
				duplicate.recordId = TerraEngine::GenerateUUID();
				candidate.value.colliders.insert(source + 1, duplicate);
			}
			commit("Duplicate Collider Records");
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove"))
		{
			for (auto& candidate : candidates) (void)candidate.value.Remove(recordId);
			commit("Remove Collider Records");
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Move Up"))
		{
			for (auto& candidate : candidates)
			{
				auto record = std::find_if(candidate.value.colliders.begin(), candidate.value.colliders.end(), [&](const auto& value) { return value.recordId == recordId; });
				if (record != candidate.value.colliders.begin()) std::iter_swap(record, record - 1);
			}
			commit("Reorder Collider Records");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Move Down"))
		{
			for (auto& candidate : candidates)
			{
				auto record = std::find_if(candidate.value.colliders.begin(), candidate.value.colliders.end(), [&](const auto& value) { return value.recordId == recordId; });
				if (record + 1 != candidate.value.colliders.end()) std::iter_swap(record, record + 1);
			}
			commit("Reorder Collider Records");
		}
		ImGui::PopID();
	}
}

void InspectorWindow::DrawConstraintSetInspector(const std::vector<entt::entity>& entities)
{
	auto candidates = CopyPhysicsCandidates<ConstraintSetComponent>(entities);
	if (candidates.empty()) return;
	auto commit = [&](const char* label) {
		std::vector<std::pair<TerraEngine::UUID, std::vector<TerraEngine::Physics::ConstraintRecord>>> values;
		for (const auto& candidate : candidates)
		{
			const auto validation = candidate.value.Validate();
			if (!validation.accepted)
			{
				ImGui::TextColored(ImVec4(1, .45F, .25F, 1), "Constraint validation: %s", validation.message.c_str());
				return;
			}
			values.emplace_back(candidate.objectId, candidate.value.constraints);
		}
		QueuePhysicsRebuildIntentsForChangedObjects(
			SubmitPhysicsPropertyBatch(values, "ConstraintSetComponent", "constraints", label, "inspector-constraints"), label);
	};
	if (ImGui::Button("Add Constraint"))
	{
		for (auto& candidate : candidates) candidate.value.Add();
		commit("Add Constraint Records");
	}

	std::vector<TerraEngine::UUID> commonRecordIds;
	for (const auto& record : candidates.front().value.constraints)
	{
		if (std::all_of(candidates.begin() + 1, candidates.end(), [&](const auto& candidate) {
			return std::any_of(candidate.value.constraints.begin(), candidate.value.constraints.end(), [&](const auto& other) { return other.recordId == record.recordId; });
		})) commonRecordIds.push_back(record.recordId);
	}
	if (candidates.size() > 1U && commonRecordIds.empty() && !candidates.front().value.constraints.empty())
		ImGui::TextDisabled("No constraint record IDs are shared by every selected object.");

	for (std::size_t displayIndex = 0; displayIndex < commonRecordIds.size(); ++displayIndex)
	{
		const TerraEngine::UUID recordId = commonRecordIds[displayIndex];
		std::vector<TerraEngine::Physics::ConstraintRecord*> records;
		for (auto& candidate : candidates)
		{
			auto record = std::find_if(candidate.value.constraints.begin(), candidate.value.constraints.end(), [&](const auto& value) { return value.recordId == recordId; });
			records.push_back(&*record);
		}
		ImGui::PushID(TerraEngine::ToHexString(recordId).c_str());
		ImGui::SeparatorText(("Constraint " + std::to_string(displayIndex + 1U)).c_str());
		ImGui::TextDisabled("Stable record ID: %s", TerraEngine::ToHexString(recordId).c_str());
		std::vector<bool> enabledValues;
		for (const auto* record : records) enabledValues.push_back(record->enabled);
		bool enabled = enabledValues.front();
		if (DrawMixedCheckbox("Enabled", enabled, HasMixedValues(enabledValues)))
		{
			for (auto* record : records) record->enabled = enabled;
			commit("Edit Constraint Enabled State");
		}
		std::vector<bool> collideValues;
		for (const auto* record : records) collideValues.push_back(record->collideConnected);
		bool collideConnected = collideValues.front();
		if (DrawMixedCheckbox("Collide Connected", collideConnected, HasMixedValues(collideValues)))
		{
			for (auto* record : records) record->collideConnected = collideConnected;
			commit("Edit Constraint Collision");
		}
		std::vector<float> forceValues;
		for (const auto* record : records) forceValues.push_back(record->breakForce);
		float breakForce = forceValues.front();
		if (DrawMixedDragFloat("Break Force (N)", breakForce, HasMixedValues(forceValues), 1.0F, 0.0F, 10000000.0F, "%.1f"))
		{
			for (auto* record : records) record->breakForce = breakForce;
			commit("Edit Constraint Break Force");
		}

		if (ImGui::Button("Duplicate"))
		{
			for (auto& candidate : candidates)
			{
				auto source = std::find_if(candidate.value.constraints.begin(), candidate.value.constraints.end(), [&](const auto& value) { return value.recordId == recordId; });
				auto duplicate = *source;
				duplicate.recordId = TerraEngine::GenerateUUID();
				candidate.value.constraints.insert(source + 1, duplicate);
			}
			commit("Duplicate Constraint Records");
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove"))
		{
			for (auto& candidate : candidates) (void)candidate.value.Remove(recordId);
			commit("Remove Constraint Records");
			ImGui::PopID();
			break;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Move Up"))
		{
			for (auto& candidate : candidates)
			{
				auto record = std::find_if(candidate.value.constraints.begin(), candidate.value.constraints.end(), [&](const auto& value) { return value.recordId == recordId; });
				if (record != candidate.value.constraints.begin()) std::iter_swap(record, record - 1);
			}
			commit("Reorder Constraint Records");
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Move Down"))
		{
			for (auto& candidate : candidates)
			{
				auto record = std::find_if(candidate.value.constraints.begin(), candidate.value.constraints.end(), [&](const auto& value) { return value.recordId == recordId; });
				if (record + 1 != candidate.value.constraints.end()) std::iter_swap(record, record + 1);
			}
			commit("Reorder Constraint Records");
		}
		ImGui::PopID();
	}
}

void InspectorWindow::DrawCharacterControllerInspector(const std::vector<entt::entity>& entities)
{
	auto candidates = CopyPhysicsCandidates<CharacterControllerComponent>(entities);
	if (candidates.empty()) return;
	auto submit = [&](const char* propertyName, auto value, auto setValue, const char* label) {
		for (auto& candidate : candidates) setValue(candidate.value, value);
		for (const auto& candidate : candidates)
		{
			const auto validation = candidate.value.Validate();
			if (!validation.accepted)
			{
				ImGui::TextColored(ImVec4(1, .45F, .25F, 1), "%s", validation.message.c_str());
				return;
			}
		}
		std::vector<std::pair<TerraEngine::UUID, decltype(value)>> values;
		for (const auto& candidate : candidates) values.emplace_back(candidate.objectId, value);
		QueuePhysicsRebuildIntentsForChangedObjects(
			SubmitPhysicsPropertyBatch(values, "CharacterControllerComponent", propertyName, label, "inspector-character"), label);
	};
	auto drawFloat = [&](const char* widgetLabel, const char* propertyName, auto member, float minimum, float maximum, const char* historyLabel) {
		std::vector<float> values;
		for (const auto& candidate : candidates) values.push_back(candidate.value.*member);
		float value = values.front();
		if (DrawMixedDragFloat(widgetLabel, value, HasMixedValues(values), .01F, minimum, maximum, "%.3f"))
			submit(propertyName, value, [member](auto& character, float updated) { character.*member = updated; }, historyLabel);
	};
	drawFloat("Radius (m)", "radius", &CharacterControllerComponent::radius, .01F, 1000.0F, "Edit Character Radius");
	drawFloat("Height (m)", "height", &CharacterControllerComponent::height, .02F, 2000.0F, "Edit Character Height");
	drawFloat("Step Height (m)", "stepHeight", &CharacterControllerComponent::stepHeight, 0.0F, 100.0F, "Edit Character Step Height");
	drawFloat("Maximum Slope (degrees)", "maximumSlopeAngleDegrees", &CharacterControllerComponent::maximumSlopeAngleDegrees, 0.0F, 89.9F, "Edit Character Slope Limit");
	std::vector<bool> stairValues;
	for (const auto& candidate : candidates) stairValues.push_back(candidate.value.allowStairWalking);
	bool stairWalking = stairValues.front();
	if (DrawMixedCheckbox("Allow Stair Walking", stairWalking, HasMixedValues(stairValues)))
		submit("allowStairWalking", stairWalking, [](auto& character, bool value) { character.allowStairWalking = value; }, "Edit Character Stair Walking");
	std::vector<bool> gravityValues;
	for (const auto& candidate : candidates) gravityValues.push_back(candidate.value.useGravity);
	bool useGravity = gravityValues.front();
	if (DrawMixedCheckbox("Use World Gravity", useGravity, HasMixedValues(gravityValues)))
		submit("useGravity", useGravity, [](auto& character, bool value) { character.useGravity = value; }, "Edit Character Gravity");
}

void InspectorWindow::DrawAnimationPreviewControls(
	const TerraEngine::Editor::Animation::AnimationPreviewSessionDescriptor& descriptor)
{
	using TerraEngine::Animation::AnimationStatus;
	using TerraEngine::Animation::AnimatorPlaybackRunState;
	using TerraEngine::Editor::Animation::AnimationPreviewEndReason;
	using TerraEngine::Editor::Animation::AnimationPreviewSnapshot;

	const bool playModeActive = ScriptingSystem::Instance().IsRunning();
	AnimationPreviewSnapshot snapshot = m_AnimationPreview.GetSnapshot();
	if (snapshot.active
		&& (snapshot.session.clipAssetId != descriptor.clipAssetId
			|| snapshot.session.clipGeneration != descriptor.clipGeneration
			|| snapshot.session.compatibilityFingerprint != descriptor.compatibilityFingerprint))
	{
		// A reimport can retain the GUID while publishing a new immutable
		// generation. Never continue a cursor against mixed-generation metadata.
		m_AnimationPreview.EndSession(AnimationPreviewEndReason::ClipGenerationChanged);
		snapshot = m_AnimationPreview.GetSnapshot();
	}

	if (snapshot.active && !playModeActive)
	{
		(void)m_AnimationPreview.Tick(static_cast<double>(ImGui::GetIO().DeltaTime));
		snapshot = m_AnimationPreview.GetSnapshot();
	}

	ImGui::SeparatorText("Clip Preview");
	ImGui::PushID("AnimationPreviewControls");
	if (!snapshot.active)
	{
		ImGui::BeginDisabled(playModeActive);
		if (ImGui::Button("Start Preview", ImVec2(-1.0F, 0.0F)))
		{
			(void)m_AnimationPreview.BeginSession(descriptor);
			snapshot = m_AnimationPreview.GetSnapshot();
		}
		ImGui::EndDisabled();
		if (playModeActive)
		{
			ImGui::TextDisabled("Stop Play Mode before starting an editor preview.");
		}
	}
	else
	{
		if (ImGui::Button("End Preview", ImVec2(-1.0F, 0.0F)))
		{
			m_AnimationPreview.EndSession();
			snapshot = m_AnimationPreview.GetSnapshot();
		}
	}

	if (snapshot.active)
	{
		const AnimatorPlaybackRunState runState = snapshot.playback.runState;
		if (runState == AnimatorPlaybackRunState::Playing)
		{
			if (ImGui::Button("Pause")) (void)m_AnimationPreview.Pause();
		}
		else if (runState == AnimatorPlaybackRunState::Paused)
		{
			if (ImGui::Button("Resume")) (void)m_AnimationPreview.Resume();
		}
		else
		{
			if (ImGui::Button("Play")) (void)m_AnimationPreview.Play();
		}
		ImGui::SameLine();
		if (ImGui::Button("Stop")) (void)m_AnimationPreview.Stop();

		bool looping = snapshot.playback.looping;
		if (ImGui::Checkbox("Loop", &looping)) (void)m_AnimationPreview.SetLooping(looping);
		ImGui::SameLine();
		bool reverse = snapshot.playback.reverse;
		if (ImGui::Checkbox("Reverse", &reverse)) (void)m_AnimationPreview.SetReverse(reverse);

		float speed = snapshot.session.speed;
		ImGui::SetNextItemWidth(-1.0F);
		if (ImGui::SliderFloat("Speed", &speed, 0.0F, 4.0F, "%.2fx"))
		{
			(void)m_AnimationPreview.SetSpeed(speed);
		}

		float normalizedTime = snapshot.playback.normalizedTime;
		ImGui::SetNextItemWidth(-1.0F);
		if (ImGui::SliderFloat("Time", &normalizedTime, 0.0F, 1.0F, "%.3f"))
		{
			(void)m_AnimationPreview.SeekNormalized(normalizedTime);
		}
		snapshot = m_AnimationPreview.GetSnapshot();
	}

	if (ImGui::BeginTable("AnimationPreviewStatus", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
	{
		auto row = [](const char* label, const std::string& value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%s", value.c_str());
		};
		row("State", snapshot.active ? AnimatorPlaybackRunStateName(snapshot.playback.runState) : "Inactive");
		row("Status", std::string(TerraEngine::Animation::ToString(snapshot.latestStatus)));
		row("Tick", std::to_string(snapshot.playback.resolvedTick)
			+ " / " + std::to_string(descriptor.durationTicks));
		row("Loop Count", std::to_string(snapshot.playback.loopCount));
		row("Safe Pose", snapshot.safePoseReason);
		ImGui::EndTable();
	}
	ImGui::TextDisabled("Preview owns only a transient fixed-tick cursor; authored Animator fields are never changed.");
	ImGui::PopID();
}

void InspectorWindow::DrawAnimatorInspector(
	entt::entity entity,
	const std::vector<entt::entity>& selectedObjects)
{
	const std::optional<Object> object = ObjectManager::instance().getObject(entity);
	AnimatorComponent* animator = object
		? ObjectManager::instance().GetComponent<AnimatorComponent>(*object)
		: nullptr;
	if (!object || animator == nullptr)
	{
		ImGui::TextDisabled("Animator data is no longer available for this selection.");
		m_AnimationPreview.EndSession(TerraEngine::Editor::Animation::AnimationPreviewEndReason::SelectionChanged);
		return;
	}

	TerraEngine::UUID controllerAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID defaultClipAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID skeletonAssetId = TerraEngine::EMPTY_UUID;
	bool loopDefaultClip = true;
	bool playAutomatically = true;
	std::uint32_t defaultClipStartFrame = 0U;
	TerraEngine::Animation::AnimationExecutionPolicy requestedMode =
		TerraEngine::Animation::AnimationExecutionPolicy::Auto;
	TerraEngine::Animation::AnimatorTimePolicy timePolicy =
		TerraEngine::Animation::AnimatorTimePolicy::Scaled;
	TerraEngine::Animation::AnimatorRootMotionPolicy rootMotionPolicy =
		TerraEngine::Animation::AnimatorRootMotionPolicy::ExtractOnly;
	std::string initialParameters;
	{
		// Copy authored values while holding the component read lock, then release
		// it before drawing UI or querying independently published runtime state.
		auto lock = animator->acquireReadLock();
		controllerAssetId = animator->controllerAssetId;
		defaultClipAssetId = animator->defaultClipAssetId;
		skeletonAssetId = animator->skeletonOverrideAssetId;
		loopDefaultClip = animator->loopDefaultClip;
		playAutomatically = animator->playAutomatically;
		defaultClipStartFrame = animator->defaultClipStartFrame;
		requestedMode = animator->requestedMode;
		timePolicy = animator->timePolicy;
		rootMotionPolicy = animator->rootMotionPolicy;
		initialParameters = animator->initialParametersJson;
	}

	// Reflection widgets above remain the only mutation path, so their existing
	// undo and prefab handling applies equally to Basic and Advanced controls.
	// This copied comparison merely tells a multi-selection user when values are
	// not uniform; it never locks or retains another live component while drawing.
	bool mixedRequestedMode = false;
	bool mixedRootPolicy = false;
	bool mixedInitialParameters = false;
	for (const entt::entity selected : selectedObjects)
	{
		const std::optional<Object> selectedObject = ObjectManager::instance().getObject(selected);
		AnimatorComponent* selectedAnimator = selectedObject
			? ObjectManager::instance().GetComponent<AnimatorComponent>(*selectedObject)
			: nullptr;
		if (selectedAnimator == nullptr)
		{
			continue;
		}
		auto selectedLock = selectedAnimator->acquireReadLock();
		mixedRequestedMode |= selectedAnimator->requestedMode != requestedMode;
		mixedRootPolicy |= selectedAnimator->rootMotionPolicy != rootMotionPolicy;
		mixedInitialParameters |= selectedAnimator->initialParametersJson != initialParameters;
	}

	ImGui::SeparatorText("Authored Animation");
	if (selectedObjects.size() > 1U)
	{
		ImGui::TextDisabled("Mixed selection: mode %s, root policy %s, initial parameters %s.",
			mixedRequestedMode ? "mixed" : "uniform",
			mixedRootPolicy ? "mixed" : "uniform",
			mixedInitialParameters ? "mixed" : "uniform");
	}
	if (ImGui::BeginTable("AnimatorAuthoredStatus", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
	{
		auto row = [](const char* label, const std::string& value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%s", value.c_str());
		};
		std::string controllerLabel;
		std::string defaultClipLabel;
		std::string skeletonLabel;
		(void)ResolveInspectorAssetLabel(controllerAssetId, "controllerAssetId", controllerLabel);
		(void)ResolveInspectorAssetLabel(defaultClipAssetId, "defaultClipAssetId", defaultClipLabel);
		(void)ResolveInspectorAssetLabel(skeletonAssetId, "skeletonOverrideAssetId", skeletonLabel);
		row("Animation Controller", controllerLabel);
		row("Default Clip", defaultClipLabel);
		row("Loop Default Clip", loopDefaultClip ? "Enabled" : "Disabled");
		row("Play Automatically", playAutomatically ? "Enabled" : "Disabled");
		const TerraEngine::UUID startClipAssetId =
			ResolveAnimatorStartClipAssetId(
				controllerAssetId,
				defaultClipAssetId);
		const std::optional<std::uint32_t> maximumFrame =
			ResolveAnimatorPlaybackMaximumFrame(startClipAssetId);
		row("Default Clip Start Frame", maximumFrame
			? TerraEngine::Editor::Animation::FormatAnimatorInspectorFrameRange(
				TerraEngine::Editor::Animation::BuildAnimatorInspectorFrameRange(
					defaultClipStartFrame,
					static_cast<TerraEngine::Animation::AnimationFixedTick>(*maximumFrame),
					loopDefaultClip))
			: std::to_string(defaultClipStartFrame) + "/?");
		row("Skeleton Override", skeletonLabel);
		row("Requested Mode", AnimationExecutionPolicyName(requestedMode));
		row("Time Policy", AnimatorTimePolicyName(timePolicy));
		row("Root Motion", rootMotionPolicy == TerraEngine::Animation::AnimatorRootMotionPolicy::Ignore ? "Ignore"
			: rootMotionPolicy == TerraEngine::Animation::AnimatorRootMotionPolicy::ApplyToObject ? "Apply to Object"
			: rootMotionPolicy == TerraEngine::Animation::AnimatorRootMotionPolicy::ApplyToCharacter ? "Apply to Character"
			: "Extract Only");
		row("Initial Parameters", initialParameters.empty() ? "{}" : initialParameters);
		ImGui::EndTable();
	}
	if (requestedMode == TerraEngine::Animation::AnimationExecutionPolicy::Auto)
	{
		ImGui::TextDisabled("Auto selects the compatible published execution path. Choose an Advanced requested mode only when diagnosing compatibility.");
	}
	if (controllerAssetId == TerraEngine::EMPTY_UUID
		&& defaultClipAssetId == TerraEngine::EMPTY_UUID)
	{
		ImGui::TextWrapped("Setup needed: assign either an Animation Controller or a Default Clip. A Skeleton Override is optional when the model already supplies a compatible skeleton.");
	}

	const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*object);
	const std::shared_ptr<const TerraEngine::Animation::PublishedAnimatorPose> published =
		TerraEngine::Animation::AnimationSystem::Instance().LoadPublishedAnimatorPoseForObject(objectId);
	ImGui::SeparatorText("Runtime Animation");
	if (!published)
	{
		ImGui::TextDisabled("No generation-safe runtime animator pose is published for this object.");
		ImGui::TextWrapped("Safe pose: the authored/reference pose remains active until compatible clip and skeleton generations are bound.");
	}
	else
	{
		const TerraEngine::Animation::AnimatorPlaybackStateSnapshot playback =
			TerraEngine::Animation::AnimationSystem::Instance().LoadAnimatorPlaybackState(published->animator);
		if (ImGui::BeginTable("AnimatorRuntimeStatus", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			auto row = [](const char* label, const std::string& value) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(label);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", value.c_str());
			};
			row("State", AnimatorPlaybackRunStateName(playback.runState));
			row("Status", std::string(TerraEngine::Animation::ToString(published->status)));
			row("Normalized Time", std::to_string(playback.normalizedTime));
			row("Loop Count", std::to_string(playback.loopCount));
			row("Effective Speed", std::to_string(playback.effectiveSpeed));
			row("Clip Generation", std::to_string(published->clip.generation));
			row("Skeleton Generation", std::to_string(published->skeleton.generation));
			row("Safe Pose", published->safePose
				? "Yes - " + std::string(TerraEngine::Animation::ToString(published->status))
				: "No - latest complete pose is active");
			ImGui::EndTable();
		}
	}

	if (m_LastAnimationClipDescriptor)
	{
		GeneratedModelPackageChildMetadata currentClipMetadata;
		const bool descriptorStillPublished =
			AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(
				m_LastAnimationClipDescriptor->clipAssetId.value,
				GeneratedModelPackageChildKind::AnimationClip,
				currentClipMetadata)
			&& currentClipMetadata.publishedGeneration == m_LastAnimationClipDescriptor->clipGeneration
			&& currentClipMetadata.compatibilityFingerprint
				== m_LastAnimationClipDescriptor->compatibilityFingerprint;
		if (!descriptorStillPublished)
		{
			// The source package may reimport while an entity remains selected.
			// Never let that entity reuse a descriptor from the prior generation.
			m_AnimationPreview.EndSession(
				TerraEngine::Editor::Animation::AnimationPreviewEndReason::ClipGenerationChanged);
			m_LastAnimationClipDescriptor.reset();
			ImGui::SeparatorText("Clip Preview");
			ImGui::TextWrapped("The previously inspected clip generation is no longer published. Select the generated Animation Clip again.");
			return;
		}

		// Rebind only the editor selection identity. The clip GUID, generation,
		// compatibility fingerprint, and BuildRoot path remain manifest-owned.
		TerraEngine::Editor::Animation::AnimationPreviewSessionDescriptor descriptor =
			*m_LastAnimationClipDescriptor;
		descriptor.selectionId = objectId;
		DrawAnimationPreviewControls(descriptor);
	}
	else
	{
		ImGui::SeparatorText("Clip Preview");
		ImGui::TextWrapped("Select a generated Animation Clip in the Project Browser to inspect it and make its transient preview controls available here.");
	}
}

void InspectorWindow::OnGui() {
	m_InspectorValueDragging = false;

	auto selectedPackageChild = m_Owner->GetSelectedPackageChildAsset();
	auto selectedAsset = m_Owner->GetSelectedAsset();
	std::vector<entt::entity> selectedEntities = m_Owner->GetSelectedEntities();
	auto selectedEntity = m_Owner->GetSelectedEntity();

	// Preview state follows the Inspector selection rather than the scene. This
	// check runs before every early return so asset/entity changes and Play Mode
	// transitions cannot leave an editor-owned cursor alive off-screen.
	TerraEngine::UUID previewSelectionId = TerraEngine::EMPTY_UUID;
	if (selectedPackageChild)
	{
		previewSelectionId = selectedPackageChild->guid;
	}
	else if (!selectedAsset && selectedEntity)
	{
		previewSelectionId = GetObjectUuid(*selectedEntity);
	}
	m_AnimationPreview.SynchronizeContext(
		previewSelectionId,
		ScriptingSystem::Instance().IsRunning());

	if (selectedPackageChild) {
		DrawModelPackageChildInspector(
			selectedPackageChild->packagePath,
			selectedPackageChild->guid,
			selectedPackageChild->kind,
			selectedPackageChild->displayName);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedAsset) {
		std::string ext = selectedAsset->extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ext == ".tmat") {
			DrawMaterialInspector(*selectedAsset);
		} else if (TerraEngine::AssetPipeline::IsModelPackageSourceExtension(*selectedAsset)) {
			DrawModelPackageInspector(*selectedAsset);
		} else if (IsSupportedMeshSourceExtension(*selectedAsset)) {
			DrawMeshInspector(*selectedAsset);
		} else if (IsSupportedTextureSourceExtension(*selectedAsset)) {
			DrawTextureInspector(*selectedAsset);
		} else {
			ImGui::Text("Selected Asset: %s", selectedAsset->filename().string().c_str());
		}
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity) {
		auto& objManager = ObjectManager::instance();
		if (std::optional<Object> object = objManager.getObject(*selectedEntity)) {
			entt::entity obj = *selectedEntity;
			
			auto& accessReg = ComponentAccessRegistry();

			// Header: Active Checkbox, Name Input, Static Checkbox
			ImGui::PushID("InspectorHeaderTop");
			
			bool isActive = ObjectManager::instance().isObjectActive(*object);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Checkbox("##ActiveGlobal", &isActive)) {
				ReflectionVariant oldVal = ObjectManager::instance().isObjectActive(*object);
				ObjectManager::instance().setObjectActive(*object, isActive);
				ReflectionVariant newVal = ObjectManager::instance().isObjectActive(*object);
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj, ObjectMetadataField::Active, oldVal, newVal));
			}
			ImGui::PopID();
			
			ImGui::SameLine();
			
			std::string name = ObjectManager::instance().getObjectName(*object);
			char nameBuf[256];
			strncpy(nameBuf, name.c_str(), sizeof(nameBuf));
			
			// Calculate width for name input: available width minus "Static" checkbox width
			float staticWidth = ImGui::CalcTextSize("Static").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetFrameHeight();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - staticWidth - ImGui::GetStyle().ItemSpacing.x);
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::InputText("##ObjectName", nameBuf, sizeof(nameBuf))) {
				ObjectManager::instance().setObjectName(*object, nameBuf);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsItemActivated()) {
				m_PreEditName = name;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (m_PreEditName != nameBuf) {
					UndoRedoSystem::Instance().PushCommand(std::make_unique<RenameObjectCommand>(obj, m_PreEditName, nameBuf));
				}
			}
			
			ImGui::SameLine();
			
			bool isStatic = ObjectManager::instance().isObjectStatic(*object);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Checkbox("Static", &isStatic)) {
				ReflectionVariant oldVal = ObjectManager::instance().isObjectStatic(*object);
				ObjectManager::instance().setObjectStatic(*object, isStatic);
				ReflectionVariant newVal = ObjectManager::instance().isObjectStatic(*object);
				UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj, ObjectMetadataField::Static, oldVal, newVal));
			}

			// Tag and Layer
			ImGui::Spacing();
			
			// Use a table for alignment
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginTable("TagLayerTable", 4, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("TagLabel", ImGuiTableColumnFlags_WidthFixed, 30.0f);
				ImGui::TableSetupColumn("TagValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("LayerLabel", ImGuiTableColumnFlags_WidthFixed, 40.0f);
				ImGui::TableSetupColumn("LayerValue", ImGuiTableColumnFlags_WidthStretch, 1.0f);

				ImGui::TableNextRow();
				
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Tag");
				
				ImGui::TableSetColumnIndex(1);
				std::string tag = ObjectManager::instance().getObjectTag(*object);
				ImGui::SetNextItemWidth(-1.0f);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginCombo("##TagCombo", tag.c_str())) {
					const auto& tags = objManager.getAvailableTags();
					for (const auto& t : tags) {
						bool is_selected = (tag == t);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (ImGui::Selectable(t.c_str(), is_selected)) {
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (tag != t) {
								ReflectionVariant oldV = tag;
								ObjectManager::instance().setObjectTag(*object, t);
								ReflectionVariant newV = t;
								UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj, ObjectMetadataField::Tag, oldV, newV));
							}
						}
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (is_selected) ImGui::SetItemDefaultFocus();
					}
					
					ImGui::Separator();
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Selectable("Add Tag...")) {
						ImGui::OpenPopup("AddTagPopup");
						memset(m_TagInputBuffer, 0, sizeof(m_TagInputBuffer));
					}
					
					ImGui::EndCombo();
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginPopup("AddTagPopup")) {
					ImGui::Text("New Tag Name:");
					ImGui::InputText("##NewTagName", m_TagInputBuffer, sizeof(m_TagInputBuffer));
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Button("Add") || (ImGui::IsKeyPressed(ImGuiKey_Enter))) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (strlen(m_TagInputBuffer) > 0) {
							objManager.registerTag(m_TagInputBuffer);
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Button("Cancel")) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				ImGui::TableSetColumnIndex(2);
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("Layer");
				
				ImGui::TableSetColumnIndex(3);
				std::string layer = ObjectManager::instance().getObjectLayer(*object);
				ImGui::SetNextItemWidth(-1.0f);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginCombo("##LayerCombo", layer.c_str())) {
					const auto& layers = objManager.getAvailableLayers();
					for (const auto& l : layers) {
						bool is_selected = (layer == l);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (ImGui::Selectable(l.c_str(), is_selected)) {
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (layer != l) {
								ReflectionVariant oldV = layer;
								ObjectManager::instance().setObjectLayer(*object, l);
								ReflectionVariant newV = l;
								UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyObjectMetadataCommand>(obj, ObjectMetadataField::Layer, oldV, newV));
							}
						}
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (is_selected) ImGui::SetItemDefaultFocus();
					}

					ImGui::Separator();
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Selectable("Add Layer...")) {
						ImGui::OpenPopup("AddLayerPopup");
						memset(m_LayerInputBuffer, 0, sizeof(m_LayerInputBuffer));
					}

					ImGui::EndCombo();
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginPopup("AddLayerPopup")) {
					ImGui::Text("New Layer Name:");
					ImGui::InputText("##NewLayerName", m_LayerInputBuffer, sizeof(m_LayerInputBuffer));
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Button("Add") || (ImGui::IsKeyPressed(ImGuiKey_Enter))) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (strlen(m_LayerInputBuffer) > 0) {
							objManager.registerLayer(m_LayerInputBuffer);
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Button("Cancel")) {
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}

				ImGui::EndTable();
			}
			
			DrawPrefabLinkInspector(obj);

			ImGui::Separator();
			
			std::vector<std::string>& inspectorComponentOrder = m_PerObjectComponentOrders[obj];
			
			std::unordered_set<std::string> activeComponents;
			bool hasTransform = false;
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (auto& [compName, access] : accessReg) {
				if (compName == "PrefabLinkComponent") {
					continue;
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (access.entityHasComponent(ObjectManager::instance().getRegistry(), obj)) {
					if (IsPhysicsAuthoringComponent(compName)
						&& !std::all_of(selectedEntities.begin(), selectedEntities.end(), [&](entt::entity selectedObject) {
							return access.entityHasComponent(ObjectManager::instance().getRegistry(), selectedObject);
						}))
					{
						continue;
					}
					activeComponents.insert(compName);
					if (compName == "Transform") hasTransform = true;
				}
			}

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (auto it = inspectorComponentOrder.begin(); it != inspectorComponentOrder.end(); ) {
				if (activeComponents.find(*it) == activeComponents.end()) it = inspectorComponentOrder.erase(it);
				else ++it;
			}
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& compName : activeComponents) {
				bool found = false;
				for (const auto& existing : inspectorComponentOrder) if (existing == compName) found = true;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!found) inspectorComponentOrder.push_back(compName);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (hasTransform) {
				auto it = std::find(inspectorComponentOrder.begin(), inspectorComponentOrder.end(), "Transform");
				if (it != inspectorComponentOrder.end() && it != inspectorComponentOrder.begin()) {
					inspectorComponentOrder.erase(it);
					inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
				} else if (it == inspectorComponentOrder.end()) {
					inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
				}
			}

			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (size_t i = 0; i < inspectorComponentOrder.size(); ++i) {
				std::string compName = inspectorComponentOrder[i];
				if (accessReg.find(compName) == accessReg.end()) continue;
				ComponentAccessFunctions& access = accessReg[compName];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (compName == "Script") {
					// ScriptComponent is just the serialized backing store. Draw each
					// script binding as its own Unity-style component block instead.
					DrawScriptComponentInspector(obj);
					continue;
				}
				const std::string displayName = BuildDisplayComponentName(compName);

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!access.entityHasComponent(ObjectManager::instance().getRegistry(), obj)) continue; 

				ImGui::PushID(compName.c_str());
				
				bool compActive = true;
				// Use the reflection type captured by the engine registry so
				// the editor queries the same EnTT meta context that owns the
				// component registrations.
				ReflectionType type = access.reflectionType;
				ReflectionProperty activeProp = FindMetaProperty(type, "active");
				if (IsValid(activeProp)) {
					ReflectionVariant var = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "active");
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (IsValid(var)) compActive = MetaValue<bool>(var);
				}

				bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
				ImVec2 p = ImGui::GetCursorScreenPos();
				float availWidth = ImGui::GetContentRegionAvail().x;
				float lineHeight = ImGui::GetFrameHeight();

				ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + availWidth, p.y + lineHeight), ImGui::GetColorU32(ImGuiCol_Header));

				ImGui::SetNextItemAllowOverlap();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::InvisibleButton("##header_bg", ImVec2(availWidth, lineHeight))) {
					isOpen = !isOpen;
					ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (compName != "Transform") {
					if (ImGui::BeginPopupContextItem("ComponentSettings")) {
						if (ImGui::MenuItem("Remove Component")) {
							UndoRedoSystem::Instance().PushCommand(std::make_unique<RemoveComponentCommand>(obj, compName));
							access.entityRemoveComponent(ObjectManager::instance().getRegistry(), obj);
							NotifySceneAfterComponentMutation(compName);
						}
						ImGui::EndPopup();
					}
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!access.entityHasComponent(ObjectManager::instance().getRegistry(), obj)) {
					ImGui::PopID();
					continue;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (compName != "Transform") {
					if (ImGui::BeginDragDropSource()) {
						int sourceIdx = (int)i;
						ImGui::SetDragDropPayload("COMPONENT_ORDER", &sourceIdx, sizeof(int));
						ImGui::Text("Reorder %s", displayName.c_str());
						ImGui::EndDragDropSource();
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::BeginDragDropTarget()) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_ORDER")) {
							int sourceIdx = *(const int*)payload->Data;
							int targetIdx = (int)i;
							// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
							if (sourceIdx >= 0 && sourceIdx < inspectorComponentOrder.size() && targetIdx < inspectorComponentOrder.size()) {
								std::string sourceName = inspectorComponentOrder[sourceIdx];
								if (sourceName != "Transform") {
									// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
									if (sourceIdx < targetIdx) {
										inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx + 1, sourceName);
										inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx);
									} else if (sourceIdx > targetIdx) {
										inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx, sourceName);
										inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx + 1);
									}
								}
							}
						}
						ImGui::EndDragDropTarget();
					}
				}

				ImGui::SetCursorScreenPos(p);
				ImGui::Dummy(ImVec2(2, 0)); ImGui::SameLine(); 
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
					isOpen = !isOpen;
					ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
				}

				ImGui::SameLine();
				
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (compName != "Transform") {
					ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
					if (ImGui::Checkbox("##compActive", &compActive)) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (IsValid(activeProp)) {
							ReflectionVariant oldA = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "active");
							access.entitySetter(ObjectManager::instance().getRegistry(), obj, "active", compActive);
							ReflectionVariant newA = access.entityGetter(ObjectManager::instance().getRegistry(), obj, "active");
							UndoRedoSystem::Instance().PushCommand(std::make_unique<ModifyComponentCommand>(obj, compName, "active", oldA, newA));
							NotifySceneAfterComponentMutation(compName);
						}
					}
					ImGui::PopStyleColor();
				} else {
					ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0)); 
				}

				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(displayName.c_str());
				if (ImGui::BeginPopupContextItem(("PrefabComponentReset_" + compName).c_str())) {
					if (ImGui::MenuItem("Reset Component To Prefab Defaults")) {
						ResetPrefabOverride(obj, compName, "");
					}
					ImGui::EndPopup();
				}

				ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineHeight));
				ImGui::Dummy(ImVec2(0,0));

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (isOpen) {
					ImGui::Indent();
					if (compName == "LightComponent")
					{
						DrawLightComponentInspector(compName, obj, access);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (compName == "UIComponent")
					{
						DrawUIComponentInspector(compName, obj, access);
					}
					else if (compName == "RigidBodyComponent")
					{
						DrawRigidBodyInspector(selectedEntities);
					}
					else if (compName == "ColliderSetComponent")
					{
						DrawColliderSetInspector(selectedEntities);
					}
					else if (compName == "ConstraintSetComponent")
					{
						DrawConstraintSetInspector(selectedEntities);
					}
					else if (compName == "CharacterControllerComponent")
					{
						DrawCharacterControllerInspector(selectedEntities);
					}
					else if (compName == "AnimatorComponent")
					{
						// Keep reflection as the sole mutation path so all animator edits
						// preserve prefab override and undo behavior. Tabs only organize
						// those durable fields; runtime data remains below and read-only.
						auto drawAnimatorProperties = [&](bool advanced) {
							if (!ImGui::BeginTable(advanced ? "AnimatorAdvancedProperties" : "AnimatorBasicProperties", 2, ImGuiTableFlags_SizingStretchProp))
							{
								return;
							}
							ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 190.0f);
							ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);
							for (const ReflectionProperty& prop : TerraEngine::Reflection::GetProperties(type))
							{
								const std::string propertyName = MetaPropertyName(prop);
								if (propertyName == "active")
								{
									continue;
								}
								const bool basic = propertyName == "controllerAssetId"
									|| propertyName == "defaultClipAssetId"
									|| propertyName == "loopDefaultClip"
									|| propertyName == "playAutomatically"
									|| propertyName == "defaultClipStartFrame"
									|| propertyName == "skeletonOverrideAssetId"
									|| propertyName == "requestedMode"
									|| propertyName == "rootMotionPolicy"
									|| propertyName == "initialParameters";
								if (basic == advanced)
								{
									continue;
								}
								DrawProperty(compName, prop, obj, access);
							}
							ImGui::EndTable();
						};
						if (ImGui::BeginTabBar("AnimatorAuthoringTabs"))
						{
							if (ImGui::BeginTabItem("Basic"))
							{
								ImGui::TextWrapped("Assign a controller or default clip, then keep the recommended policy choices unless your gameplay requires different behavior.");
								ImGui::TextDisabled("Hover a field name for what the setting does and how to use it.");
								ImGui::Spacing();
								drawAnimatorProperties(false);
								ImGui::EndTabItem();
							}
							if (ImGui::BeginTabItem("Advanced"))
							{
								ImGui::TextWrapped("Optional overrides for retargeting, ragdoll, motion matching, timing, events, and off-screen behavior.");
								ImGui::TextDisabled("Hover a field name for usage guidance and recommended defaults.");
								ImGui::Spacing();
								drawAnimatorProperties(true);
								ImGui::TextDisabled("Advanced overrides can reject incompatible published content; Auto is preferred for normal authoring.");
								ImGui::EndTabItem();
							}
							ImGui::EndTabBar();
						}
						DrawAnimatorInspector(obj, selectedEntities);
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (compName == "Script")
					{
						DrawScriptComponentInspector(obj);
					}
					else
					{
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (ImGui::BeginTable("PropertyTable", 2, ImGuiTableFlags_SizingStretchProp)) {
							ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
							ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

							// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
							for (const ReflectionProperty& prop : TerraEngine::Reflection::GetProperties(type)) {
								if (MetaPropertyName(prop) == "active") continue;
								DrawProperty(compName, prop, obj, access);
							}
							ImGui::EndTable();
						}
					}
					ImGui::Unindent();
				}
				ImGui::Separator();
				ImGui::PopID();
			}
			ImGui::Spacing();
			DrawScriptDropTarget(obj);
			ImGui::Spacing();

			float availWidth = ImGui::GetContentRegionAvail().x;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Button("Add Component", ImVec2(availWidth, 0))) {
				ImGui::OpenPopup("AddComponentPopup");
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginPopup("AddComponentPopup")) {
				static char searchBuf[64] = "";
				ImGui::InputTextWithHint("##CompSearch", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf));
				
				ImGui::Separator();

				std::vector<std::string> availableComponents;
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (auto& [name, access] : accessReg) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (name != "Script" && !access.entityHasComponent(ObjectManager::instance().getRegistry(), obj)) {
						availableComponents.push_back(name);
					}
				}
				std::sort(availableComponents.begin(), availableComponents.end(), [](const std::string& lhs, const std::string& rhs) {
					return BuildDisplayComponentName(lhs) < BuildDisplayComponentName(rhs);
				});

				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const auto& compName : availableComponents) {
					if (compName == "PrefabLinkComponent") {
						continue;
					}
					const std::string displayName = BuildDisplayComponentName(compName);
					if (searchBuf[0] != '\0') {
						std::string lowerName = compName;
						std::string lowerDisplayName = displayName;
						std::string lowerFilter = searchBuf;
						std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
						std::transform(lowerDisplayName.begin(), lowerDisplayName.end(), lowerDisplayName.begin(), ::tolower);
						std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (lowerName.find(lowerFilter) == std::string::npos
							&& lowerDisplayName.find(lowerFilter) == std::string::npos) continue;
					}

					ImGui::PushID(compName.c_str());
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::Selectable(displayName.c_str())) {
						UndoRedoSystem::Instance().PushCommand(std::make_unique<AddComponentCommand>(obj, compName));
						accessReg[compName].entityAddComponent(ObjectManager::instance().getRegistry(), obj);
						NotifySceneAfterComponentMutation(compName);
						ImGui::CloseCurrentPopup();
					}
					ImGui::PopID();
				}
				ImGui::EndPopup();
			}

		} else {
			m_Owner->SetSelectedEntity(std::nullopt);
		}
	}
}

// InspectorWindow::DrawModelPackageInspector shows the source package itself, not the read-only generated children.
// Generated mesh/material/texture sub-assets stay selectable in the Project Browser, while this view remains package-level.
void InspectorWindow::DrawModelPackageInspector(const std::filesystem::path& path) {
	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	const auto& assets = AssetDatabase::Instance().GetAssets();
	const auto metaIt = assets.find(absolutePath);

	ImGui::Text("Model Package: %s", path.filename().string().c_str());
	ImGui::Separator();

	if (const std::optional<AssetImportProgress> progress = AssetDatabase::Instance().GetImportProgress(absolutePath)) {
		DrawAssetImportProgress(*progress);
		return;
	}

	if (metaIt == assets.end()) {
		ImGui::TextDisabled("This package is not registered in the asset database yet.");
		return;
	}

	const AssetMetadata& metadata = metaIt->second;
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	const bool hasImportResult = TerraEngine::AssetPipeline::LoadModelPackageImportResult(absolutePath, importResult);

	// Keep the package inspector read-only because edits belong either on the source glTF file
	// or on explicitly generated sub-assets selected from the Project Browser.
	if (ImGui::BeginTable("GltfPackageSummary", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 100.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

		auto drawRow = [](const char* label, const std::string& value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextWrapped("%s", value.empty() ? "-" : value.c_str());
		};

		drawRow("GUID", TerraEngine::ToHexString(metadata.guid));
		drawRow("Source", metadata.sourcePath.generic_string());
		drawRow("Compiled Mesh", metadata.compiledPath.generic_string());
		drawRow("Import Status", hasImportResult ? TerraEngine::AssetPipeline::ToString(importResult.status) : "Not imported");
		drawRow("Package Type", hasImportResult
			? TerraEngine::AssetPipeline::ToString(importResult.package.packageKind)
			: "-");
		drawRow("Generated Root", hasImportResult ? importResult.package.importRoot.generic_string() : "-");

		ImGui::EndTable();
	}

	if (hasImportResult && !importResult.diagnostics.empty()) {
		ImGui::SeparatorText("Diagnostics");
		for (const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : importResult.diagnostics) {
			ImGui::TextWrapped("[%s] %s: %s",
				TerraEngine::AssetPipeline::ToString(diagnostic.severity),
				diagnostic.code.c_str(),
				diagnostic.message.c_str());
			if (!diagnostic.fallback.empty()) {
				ImGui::TextDisabled("%s", diagnostic.fallback.c_str());
			}
		}
	}
}

// InspectorWindow::DrawModelPackageChildInspector shows generated package children that do not have standalone source files.
void InspectorWindow::DrawModelPackageChildInspector(const std::filesystem::path& packagePath, TerraEngine::UUID guid, const std::string& kind, const std::string& displayName) {
	const std::filesystem::path absolutePath = std::filesystem::absolute(packagePath);
	TerraEngine::AssetPipeline::ModelPackageImportResult importResult;
	const bool hasImportResult = TerraEngine::AssetPipeline::LoadModelPackageImportResult(absolutePath, importResult);

	ImGui::Text("Generated %s: %s", kind.empty() ? "Sub-Asset" : kind.c_str(), displayName.empty() ? TerraEngine::ToHexString(guid).c_str() : displayName.c_str());
	ImGui::Separator();

	if (!hasImportResult) {
		ImGui::TextDisabled("This package has no generated import result yet.");
		if (m_AnimationPreview.IsActive())
		{
			m_AnimationPreview.EndSession(
				TerraEngine::Editor::Animation::AnimationPreviewEndReason::InvalidClip);
		}
		return;
	}

	auto drawRow = [](const char* label, const std::string& value) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextWrapped("%s", value.empty() ? "-" : value.c_str());
	};

	auto drawRowU32 = [&](const char* label, std::uint32_t value) {
		drawRow(label, std::to_string(value));
	};

	auto drawCommonTable = [&](const char* tableName, const std::function<void()>& rows) {
		if (ImGui::BeginTable(tableName, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 150.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			rows();
			ImGui::EndTable();
		}
	};

	const auto animationClipChild = std::find_if(
		importResult.animationManifest.subAssets.begin(),
		importResult.animationManifest.subAssets.end(),
		[&](const TerraEngine::AssetPipeline::GeneratedAnimationSubAsset& child) {
			return child.guid == guid
				&& child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::AnimationClip;
		});
	if (animationClipChild != importResult.animationManifest.subAssets.end())
	{
		GeneratedModelPackageChildMetadata publishedMetadata;
		const bool published = AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(
			guid,
			GeneratedModelPackageChildKind::AnimationClip,
			publishedMetadata)
			&& publishedMetadata.packageGuid == importResult.packageGuid
			&& publishedMetadata.publishedGeneration == importResult.animationManifest.publishedGeneration
			&& publishedMetadata.compatibilityFingerprint == animationClipChild->compatibilityFingerprint
			&& publishedMetadata.compiledPath == animationClipChild->compiledPath;
		if (!published)
		{
			// AssetDatabase performs the dependency-fingerprint and atomic-publication
			// checks. Fail closed instead of opening candidate or partial outputs.
			ImGui::TextColored(
				ImVec4(1.0F, 0.45F, 0.25F, 1.0F),
				"This animation clip is not part of a complete published manifest generation.");
			m_AnimationPreview.EndSession(
				TerraEngine::Editor::Animation::AnimationPreviewEndReason::ClipGenerationChanged);
			if (m_LastAnimationClipDescriptor
				&& m_LastAnimationClipDescriptor->clipAssetId.value == guid)
			{
				m_LastAnimationClipDescriptor.reset();
				m_InspectedAnimationClip.reset();
				m_InspectedAnimationClipKey.clear();
			}
			return;
		}

		const std::filesystem::path buildRoot =
			std::filesystem::absolute(AssetDatabase::Instance().GetBuildRoot()).lexically_normal();
		const std::optional<std::filesystem::path> resolvedCompiledPath =
			ResolvePublishedAnimationClipPath(buildRoot, animationClipChild->compiledPath);
		const std::filesystem::path compiledPath =
			resolvedCompiledPath.value_or(std::filesystem::path{});
		const std::string cacheKey = TerraEngine::ToHexString(guid)
			+ "|" + std::to_string(publishedMetadata.publishedGeneration)
			+ "|" + animationClipChild->contentHashHex
			+ "|" + compiledPath.generic_string();

		if (cacheKey != m_InspectedAnimationClipKey)
		{
			// Decode once per immutable publication identity. The cache is replaced
			// atomically only after the full file and envelope validate.
			if (m_AnimationPreview.IsActive())
			{
				m_AnimationPreview.EndSession(
					TerraEngine::Editor::Animation::AnimationPreviewEndReason::ClipGenerationChanged);
			}
			m_InspectedAnimationClipKey = cacheKey;
			m_InspectedAnimationClip.reset();
			m_LastAnimationClipDescriptor.reset();
			m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::MissingAsset;

			if (!resolvedCompiledPath)
			{
				m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::InvalidType;
			}
			else
			{
				std::error_code fileError;
				const std::uintmax_t fileSize = std::filesystem::file_size(compiledPath, fileError);
				if (fileError)
				{
					m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::MissingAsset;
				}
				else if (fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())
					|| fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
				{
					m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::RangeOverflow;
				}
				else
				{
					std::ifstream input(compiledPath, std::ios::binary);
					std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
					if (!input.is_open())
					{
						m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::MissingAsset;
					}
					else
					{
						if (!bytes.empty())
						{
							input.read(
								reinterpret_cast<char*>(bytes.data()),
								static_cast<std::streamsize>(bytes.size()));
						}
						if ((!bytes.empty() && input.gcount() != static_cast<std::streamsize>(bytes.size()))
							|| input.bad())
						{
							m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::TruncatedInput;
						}
						else
						{
							auto decoded = std::make_unique<TerraEngine::Animation::AnimationClipAsset>();
							m_InspectedAnimationClipStatus = TerraEngine::Animation::DeserializeAnimationClipAsset(
								bytes,
								animationClipChild->compatibilityFingerprint,
								*decoded);
							if (m_InspectedAnimationClipStatus == TerraEngine::Animation::AnimationStatus::Ok
								&& (decoded->assetId.value != guid
									|| decoded->compatibilityFingerprint != animationClipChild->compatibilityFingerprint))
							{
								m_InspectedAnimationClipStatus = TerraEngine::Animation::AnimationStatus::IncompatibleAsset;
							}
							if (m_InspectedAnimationClipStatus == TerraEngine::Animation::AnimationStatus::Ok)
							{
								m_InspectedAnimationClip = std::move(decoded);
							}
						}
					}
				}
			}
		}

		drawCommonTable("GeneratedAnimationClipSummary", [&]() {
			drawRow("GUID", TerraEngine::ToHexString(animationClipChild->guid));
			drawRow("Semantic Key", animationClipChild->semanticKey);
			drawRow("Display Name", animationClipChild->displayName);
			drawRow("Compiled Clip", animationClipChild->compiledPath.generic_string());
			drawRow("Published Generation", std::to_string(publishedMetadata.publishedGeneration));
			drawRow("Compatibility", AnimationFingerprintText(animationClipChild->compatibilityFingerprint));
			drawRow("Content Hash", animationClipChild->contentHashHex);
			drawRow("Read Only", animationClipChild->readOnlyPackageChild ? "Yes" : "No");
			drawRow("Decode Status", std::string(TerraEngine::Animation::ToString(m_InspectedAnimationClipStatus)));
		});

		if (!m_InspectedAnimationClip)
		{
			if (m_AnimationPreview.IsActive())
			{
				m_AnimationPreview.EndSession(
					TerraEngine::Editor::Animation::AnimationPreviewEndReason::InvalidClip);
			}
			ImGui::TextColored(
				ImVec4(1.0F, 0.45F, 0.25F, 1.0F),
				"The published .teanim clip could not be inspected: %s",
				std::string(TerraEngine::Animation::ToString(m_InspectedAnimationClipStatus)).c_str());
			return;
		}

		const TerraEngine::Animation::AnimationClipAsset& clip = *m_InspectedAnimationClip;
		const double durationSeconds = static_cast<double>(clip.durationTicks)
			/ static_cast<double>(TerraEngine::Animation::kAnimationFixedTicksPerSecond);
		drawCommonTable("GeneratedAnimationClipDecodedSummary", [&]() {
			drawRow("Skeleton GUID", TerraEngine::ToHexString(clip.skeletonAssetId.value));
			drawRow("Duration Ticks", std::to_string(clip.durationTicks));
			drawRow("Duration Seconds", std::to_string(durationSeconds));
			drawRowU32("Sample Rate", clip.sampleRate);
			drawRow("Segments", std::to_string(clip.segments.size()));
			drawRow("Tracks", std::to_string(clip.tracks.size()));
			drawRow("Named Channels", std::to_string(clip.namedChannels.size()));
			drawRow("Events", std::to_string(clip.events.size()));
			drawRow("Root Motion", clip.rootMotion.enabled ? "Enabled" : "Disabled");
			drawRow("Translation Error (m)", std::to_string(clip.compression.translationErrorMeters));
			drawRow("Rotation Error (deg)", std::to_string(clip.compression.rotationErrorDegrees));
			drawRow("Scale Error", std::to_string(clip.compression.scaleErrorFraction));
			drawRow("Morph Error", std::to_string(clip.compression.morphError));
		});

		TerraEngine::Editor::Animation::AnimationPreviewSessionDescriptor descriptor;
		descriptor.selectionId = guid;
		descriptor.clipAssetId = clip.assetId;
		descriptor.clipGeneration = std::max<std::uint64_t>(1U, publishedMetadata.publishedGeneration);
		descriptor.compatibilityFingerprint = animationClipChild->compatibilityFingerprint;
		descriptor.durationTicks = clip.durationTicks;
		descriptor.sampleRate = clip.sampleRate;
		descriptor.displayName = animationClipChild->displayName;
		descriptor.compiledPath = compiledPath;
		descriptor.looping = true;
		descriptor.reverse = false;
		descriptor.speed = 1.0F;
		m_LastAnimationClipDescriptor = descriptor;
		DrawAnimationPreviewControls(descriptor);
		return;
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : importResult.generatedMeshes) {
		if (mesh.guid != guid) {
			continue;
		}

		drawCommonTable("GeneratedMeshChildSummary", [&]() {
			drawRow("GUID", TerraEngine::ToHexString(mesh.guid));
			drawRow("Package", absolutePath.filename().string());
			drawRow("Source Node", mesh.sourceNodePath);
			drawRowU32("Source Mesh", static_cast<std::uint32_t>(mesh.sourceMeshIndex));
			drawRow("Source Primitive", mesh.sourcePrimitiveIndex ? std::to_string(*mesh.sourcePrimitiveIndex) : "-");
			drawRow("Compiled Mesh", mesh.compiledPath.generic_string());
			drawRow("Bounds Min", std::to_string(mesh.localBoundsMin[0]) + ", " + std::to_string(mesh.localBoundsMin[1]) + ", " + std::to_string(mesh.localBoundsMin[2]));
			drawRow("Bounds Max", std::to_string(mesh.localBoundsMax[0]) + ", " + std::to_string(mesh.localBoundsMax[1]) + ", " + std::to_string(mesh.localBoundsMax[2]));
		});

		const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / mesh.compiledPath;
		const std::optional<TemfInspectorSummary> summary = ReadTemfInspectorSummary(compiledPath);
		if (summary) {
			ImGui::Spacing();
			ImGui::Text("Compiled Info (.temf - canonical production blocks)");
			drawCommonTable("GeneratedMeshCompiledSummary", [&]() {
				drawRowU32("Version", static_cast<std::uint32_t>(summary->header.formatVersion));
				drawRowU32("Production Blocks", summary->blockCount);
				drawRowU32("Virtual Pages", summary->header.pageCount);
				drawRowU32("Quantization Domains", summary->header.quantizationDomainCount);
				drawRowU32("Dependencies", summary->header.dependencyCount);
			});
		}
		else {
			ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "Compiled TEMF is missing or invalid.");
		}

		ImGui::Spacing();
		if (ImGui::CollapsingHeader("Virtual Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
			drawCommonTable("GeneratedMeshVirtualGeometrySummary", [&]() {
				drawRow("Requested", mesh.virtualGeometryRequested ? "Yes" : "No");
				drawRow("Cooked", mesh.virtualGeometryCooked ? "Yes" : "No");
				drawRow("Status", mesh.virtualGeometryStatus);
				drawRowU32("Detail Levels", mesh.virtualGeometryDetailLevels);
				drawRowU32("Meshlets", mesh.virtualGeometryMeshlets);
				drawRowU32("Source Triangles", mesh.virtualGeometrySourceTriangles);
				drawRowU32("Clusters", mesh.virtualGeometryClusterSummary.clusterCount);
				drawRowU32("Material Sections", mesh.virtualGeometryClusterSummary.materialSectionCount);
				drawRowU32("Source Material Slots", mesh.virtualGeometryClusterSummary.sourceMaterialAssignmentCount);
				drawRow("Material Coverage", mesh.virtualGeometryClusterSummary.materialCoverageValid ? "Valid" : "Needs Review");
				drawRow("Cone Safety", mesh.virtualGeometryConeSafetySummary.metadataValid ? "Valid" : "Missing");
				drawRowU32("Cone Records", mesh.virtualGeometryConeSafetySummary.recordCount);
				drawRowU32("Cone Eligible", mesh.virtualGeometryConeSafetySummary.eligibleClusterCount);
				drawRowU32("Cone Disabled", mesh.virtualGeometryConeSafetySummary.disabledClusterCount);
			});

			for (const std::string& warning : mesh.virtualGeometryWarnings) {
				ImGui::TextWrapped("%s", warning.c_str());
			}
			for (const std::string& reason : mesh.virtualGeometryConeSafetySummary.disabledReasons) {
				ImGui::TextWrapped("%s", reason.c_str());
			}
			if (!mesh.virtualGeometryTargets.empty() && ImGui::BeginTable("GeneratedMeshVirtualGeometryTargets", 9, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
				ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Desired T/M", ImGuiTableColumnFlags_WidthFixed, 95.0f);
				ImGui::TableSetupColumn("Actual T/M", ImGuiTableColumnFlags_WidthFixed, 95.0f);
				ImGui::TableSetupColumn("Nearest LOD", ImGuiTableColumnFlags_WidthFixed, 85.0f);
				ImGui::TableSetupColumn("Error", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableHeadersRow();
				for (const TerraEngine::AssetPipeline::GeneratedModelMeshVirtualGeometryTarget& target : mesh.virtualGeometryTargets) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Text("%u", target.targetIndex);
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%.3f", target.requestedRatio);
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(target.status.c_str());
					ImGui::TableSetColumnIndex(3);
					ImGui::TextWrapped("%s", target.reason.empty() ? "-" : target.reason.c_str());
					ImGui::TableSetColumnIndex(4);
					// Requested targets and actual candidates are different
					// facts. Showing both keeps merged rows from masquerading as
					// generated LODs when the cooker rejected the candidate.
					ImGui::Text("%u / %u", target.desiredTriangleCount, target.desiredMeshletCount);
					ImGui::TableSetColumnIndex(5);
					if (target.actualTriangleCount > 0u || target.actualMeshletCount > 0u) {
						ImGui::Text("%u / %u", target.actualTriangleCount, target.actualMeshletCount);
					}
					else {
						ImGui::TextUnformatted("-");
					}
					ImGui::TableSetColumnIndex(6);
					ImGui::Text("%u", target.nearestGeneratedLevelIndex);
					ImGui::TableSetColumnIndex(7);
					if (target.geometricError > 0.0f) {
						ImGui::Text("%.4f", target.geometricError);
					}
					else {
						ImGui::TextUnformatted("-");
					}
					ImGui::TableSetColumnIndex(8);
					ImGui::TextWrapped("%s", target.message.empty() ? "-" : target.message.c_str());
				}
				ImGui::EndTable();
			}
		}
		DrawGeneratedMeshVirtualGeometryOverrideControls(absolutePath, mesh.guid);

		if (!importResult.materialAssignments.empty() && ImGui::CollapsingHeader("Material Assignments", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const TerraEngine::AssetPipeline::ModelMaterialAssignment& assignment : importResult.materialAssignments) {
				if (assignment.meshGuid == mesh.guid) {
					ImGui::TextWrapped("%s -> %s", assignment.slotName.empty() ? "Material Slot" : assignment.slotName.c_str(), TerraEngine::ToHexString(assignment.materialGuid).c_str());
				}
			}
		}
		return;
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelTextureAsset& texture : importResult.generatedTextures) {
		if (texture.guid != guid) {
			continue;
		}

		drawCommonTable("GeneratedTextureChildSummary", [&]() {
			drawRow("GUID", TerraEngine::ToHexString(texture.guid));
			drawRow("Package", absolutePath.filename().string());
			drawRow("Source", texture.sourcePath.generic_string());
			drawRow("Compiled Texture", texture.compiledPath.generic_string());
			drawRow("Usage", TerraEngine::AssetPipeline::ToString(texture.usage));
			drawRow("Semantic", TerraEngine::AssetPipeline::ToString(texture.semantic));
		});
		return;
	}

	for (const TerraEngine::AssetPipeline::GeneratedModelMaterialAsset& material : importResult.generatedMaterials) {
		if (material.guid != guid) {
			continue;
		}

		drawCommonTable("GeneratedMaterialChildSummary", [&]() {
			drawRow("GUID", TerraEngine::ToHexString(material.guid));
			drawRow("Package", absolutePath.filename().string());
			drawRow("Source", material.sourcePath.generic_string());
			drawRow("Compiled Material", material.compiledPath.generic_string());
			drawRow("Base Color", std::to_string(material.baseColorFactor[0]) + ", " + std::to_string(material.baseColorFactor[1]) + ", " + std::to_string(material.baseColorFactor[2]) + ", " + std::to_string(material.baseColorFactor[3]));
			drawRow("Metallic", std::to_string(material.metallicFactor));
			drawRow("Roughness", std::to_string(material.roughnessFactor));
			drawRow("Double Sided", material.doubleSided ? "Yes" : "No");
			drawRowU32("Textures", static_cast<std::uint32_t>(material.textureGuids.size()));
		});
		return;
	}

	if (kind == "AnimationClip" && m_AnimationPreview.IsActive())
	{
		m_AnimationPreview.EndSession(
			TerraEngine::Editor::Animation::AnimationPreviewEndReason::InvalidClip);
	}
	ImGui::TextDisabled("Generated child %s was not found in the package import result.", TerraEngine::ToHexString(guid).c_str());
}

void InspectorWindow::DrawVirtualGeometryOverrideControls(const AssetMetadata& metadata) {
	AssetDatabase& assetDatabase = AssetDatabase::Instance();
	if (!assetDatabase.CanEditVirtualGeometryPerMeshOverride(metadata)) {
		return;
	}

	VirtualGeometryAdaptiveSettingsMetadata resolvedSettings;
	assetDatabase.TryGetVirtualGeometryAdaptiveSettings(metadata, resolvedSettings);
	const std::string editKey = "mesh:" + TerraEngine::ToHexString(metadata.guid);
	if (m_VirtualGeometryEditKey != editKey) {
		m_VirtualGeometryEditKey = editKey;
		m_VirtualGeometryEditSettings = resolvedSettings;
		m_VirtualGeometrySettingsMessage.clear();
		m_VirtualGeometrySettingsMessageIsError = false;
	}

	ImGui::Spacing();
	if (!ImGui::CollapsingHeader("Virtual Geometry LOD Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	bool overrideEnabled = m_VirtualGeometryEditSettings.overridePresent || assetDatabase.HasVirtualGeometryPerMeshOverride(metadata);
	if (ImGui::Checkbox("Per-Mesh Override", &overrideEnabled)) {
		if (overrideEnabled) {
			m_VirtualGeometryEditSettings = resolvedSettings;
			m_VirtualGeometryEditSettings.overridePresent = true;
			m_VirtualGeometryEditSettings.settingsSource = "PerMeshOverride";
		}
		else {
			std::string error;
			if (assetDatabase.ClearVirtualGeometryPerMeshOverride(metadata.guid, &error)) {
				m_VirtualGeometryEditSettings = assetDatabase.GetDefaultVirtualGeometryAdaptiveSettings();
				m_VirtualGeometrySettingsMessage = "Per-mesh override reset to project defaults.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
	}

	ImGui::TextDisabled("Source: %s", overrideEnabled ? "PerMeshOverride" : "ProjectDefaults");
	if (!resolvedSettings.valid) {
		for (const std::string& error : resolvedSettings.validationErrors) {
			ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", error.c_str());
		}
	}

	if (overrideEnabled) {
		m_VirtualGeometryEditSettings.overridePresent = true;
		m_VirtualGeometryEditSettings.settingsSource = "PerMeshOverride";
		DrawVirtualGeometrySettingsFields(m_VirtualGeometryEditSettings);
		if (ImGui::Button("Save Settings")) {
			std::string error;
			if (assetDatabase.SaveVirtualGeometryPerMeshOverride(metadata.guid, m_VirtualGeometryEditSettings, &error)) {
				m_VirtualGeometrySettingsMessage = "Per-mesh virtual geometry settings saved.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild")) {
			std::string error;
			if (!assetDatabase.SaveVirtualGeometryPerMeshOverride(metadata.guid, m_VirtualGeometryEditSettings, &error)) {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
			else if (m_Owner != nullptr && m_Owner->RebuildAssetFromInspector(assetDatabase.GetAssetRoot() / metadata.sourcePath, &error)) {
				m_VirtualGeometrySettingsMessage = "Rebuild requested with current per-mesh settings.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error.empty() ? "Rebuild request failed." : error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			std::string error;
			if (assetDatabase.ClearVirtualGeometryPerMeshOverride(metadata.guid, &error)) {
				m_VirtualGeometryEditSettings = assetDatabase.GetDefaultVirtualGeometryAdaptiveSettings();
				m_VirtualGeometrySettingsMessage = "Per-mesh override reset to project defaults.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
	}

	if (!m_VirtualGeometrySettingsMessage.empty()) {
		const ImVec4 color = m_VirtualGeometrySettingsMessageIsError
			? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
			: ImVec4(0.55f, 0.85f, 0.65f, 1.0f);
		ImGui::TextColored(color, "%s", m_VirtualGeometrySettingsMessage.c_str());
	}
}

void InspectorWindow::DrawGeneratedMeshVirtualGeometryOverrideControls(const std::filesystem::path& packagePath, TerraEngine::UUID meshGuid) {
	AssetDatabase& assetDatabase = AssetDatabase::Instance();
	VirtualGeometryAdaptiveSettingsMetadata resolvedSettings;
	assetDatabase.TryGetGeneratedMeshVirtualGeometryAdaptiveSettings(packagePath, meshGuid, resolvedSettings);
	const std::string editKey = "generated-mesh:" + TerraEngine::ToHexString(meshGuid);
	if (m_VirtualGeometryEditKey != editKey) {
		m_VirtualGeometryEditKey = editKey;
		m_VirtualGeometryEditSettings = resolvedSettings;
		m_VirtualGeometrySettingsMessage.clear();
		m_VirtualGeometrySettingsMessageIsError = false;
	}

	ImGui::Spacing();
	if (!ImGui::CollapsingHeader("Virtual Geometry LOD Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		return;
	}

	bool overrideEnabled = m_VirtualGeometryEditSettings.overridePresent;
	if (ImGui::Checkbox("Per-Mesh Override", &overrideEnabled)) {
		if (overrideEnabled) {
			m_VirtualGeometryEditSettings = resolvedSettings;
			m_VirtualGeometryEditSettings.overridePresent = true;
			m_VirtualGeometryEditSettings.settingsSource = "PerMeshOverride";
		}
		else {
			std::string error;
			if (assetDatabase.ClearGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, meshGuid, &error)) {
				m_VirtualGeometryEditSettings = assetDatabase.GetDefaultVirtualGeometryAdaptiveSettings();
				m_VirtualGeometrySettingsMessage = "Generated mesh override reset to project defaults.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
	}

	ImGui::TextDisabled("Source: %s", overrideEnabled ? "PerMeshOverride" : "ProjectDefaults");
	if (!resolvedSettings.valid) {
		for (const std::string& error : resolvedSettings.validationErrors) {
			ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", error.c_str());
		}
	}

	if (overrideEnabled) {
		m_VirtualGeometryEditSettings.overridePresent = true;
		m_VirtualGeometryEditSettings.settingsSource = "PerMeshOverride";
		DrawVirtualGeometrySettingsFields(m_VirtualGeometryEditSettings);
		if (ImGui::Button("Save Settings")) {
			std::string error;
			if (assetDatabase.SaveGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, meshGuid, m_VirtualGeometryEditSettings, &error)) {
				m_VirtualGeometrySettingsMessage = "Generated mesh virtual geometry settings saved.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Rebuild Package")) {
			std::string error;
			if (!assetDatabase.SaveGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, meshGuid, m_VirtualGeometryEditSettings, &error)) {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
			else if (m_Owner != nullptr && m_Owner->RebuildAssetFromInspector(packagePath, &error)) {
				m_VirtualGeometrySettingsMessage = "Package rebuild requested with current generated mesh settings.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error.empty() ? "Package rebuild request failed." : error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset")) {
			std::string error;
			if (assetDatabase.ClearGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, meshGuid, &error)) {
				m_VirtualGeometryEditSettings = assetDatabase.GetDefaultVirtualGeometryAdaptiveSettings();
				m_VirtualGeometrySettingsMessage = "Generated mesh override reset to project defaults.";
				m_VirtualGeometrySettingsMessageIsError = false;
			}
			else {
				m_VirtualGeometrySettingsMessage = error;
				m_VirtualGeometrySettingsMessageIsError = true;
			}
		}
	}

	if (!m_VirtualGeometrySettingsMessage.empty()) {
		const ImVec4 color = m_VirtualGeometrySettingsMessageIsError
			? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
			: ImVec4(0.55f, 0.85f, 0.65f, 1.0f);
		ImGui::TextColored(color, "%s", m_VirtualGeometrySettingsMessage.c_str());
	}
}

// InspectorWindow::DrawMaterialInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawMaterialInspector(const std::filesystem::path& path) {
	if (m_LastInspectedAsset != path) {
		m_LastInspectedAsset = path;
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (in.is_open()) {
			std::streamsize size = in.tellg();
			in.seekg(0, std::ios::beg);
			m_MaterialDataBuffer.resize(size);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!in.read((char*)m_MaterialDataBuffer.data(), size)) {
				m_MaterialDataBuffer.clear();
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_MaterialDataBuffer.size() < sizeof(MaterialHeader)) {
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid material file!");
		return;
	}

	MaterialHeader* header = (MaterialHeader*)m_MaterialDataBuffer.data();
	bool changed = false;

	ImGui::Text("Material: %s", path.filename().string().c_str());
	ImGui::Separator();
	const bool readOnlyPackageSubAsset = IsGeneratedPackageSubAssetPath(path);
	if (readOnlyPackageSubAsset) {
		ImGui::TextDisabled("Read-only generated package sub-asset.");
		ImGui::BeginDisabled();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("MaterialSettings", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		auto drawLabeledWidget = [&](const char* label, auto widgetFunc) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);
			widgetFunc();
		};

		// Shading Model
		const char* shadingModels[] = { "Unlit", "MetallicRoughness", "SpecularGlossiness" };
		int shadingModelIdx = (int)header->shadingModel;
		drawLabeledWidget("Shading Model", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Combo("##ShadingModel", &shadingModelIdx, shadingModels, IM_ARRAYSIZE(shadingModels))) {
				header->shadingModel = (uint32_t)shadingModelIdx;
				changed = true;
			}
		});

		// Alpha Mode
		const char* alphaModes[] = { "Opaque", "Mask", "Blend" };
		int alphaModeIdx = (int)header->alphaMode;
		drawLabeledWidget("Alpha Mode", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Combo("##AlphaMode", &alphaModeIdx, alphaModes, IM_ARRAYSIZE(alphaModes))) {
				header->alphaMode = (uint32_t)alphaModeIdx;
				changed = true;
			}
		});

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (header->alphaMode == (uint32_t)AlphaMode::Mask) {
			drawLabeledWidget("Alpha Cutoff", [&]() {
				if (ImGui::DragFloat("##AlphaCutoff", &header->alphaCutoff, 0.01f, 0.0f, 1.0f)) changed = true;
			});
		}

		// Shader Collection
		{
			TerraEngine::UUID currentShaderId = header->shaderCollectionId;
			std::string shaderName = "None";
			bool shaderExists = false;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentShaderId != TerraEngine::EMPTY_UUID) {
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
				for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (meta.guid == currentShaderId) {
						shaderName = path.filename().string();
						shaderExists = true;
						break;
					}
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (shaderName == "None" && currentShaderId == ResourceManager::Instance().GetDefaultShaderCollectionID()) {
					shaderName = "Default Shader";
					shaderExists = true;
				}

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!shaderExists) {
					shaderName = "(Missing)";
				}
			}

			drawLabeledWidget("Shader", [&]() {
				char buf[128];
				strncpy(buf, shaderName.c_str(), sizeof(buf));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (currentShaderId != TerraEngine::EMPTY_UUID && !shaderExists) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
				ImGui::InputText("##shader", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
				if (currentShaderId != TerraEngine::EMPTY_UUID && !shaderExists) ImGui::PopStyleColor();

				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginDragDropTarget()) {
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_SHADER")) {
						header->shaderCollectionId = *(const TerraEngine::UUID*)payload->Data;
						changed = true;
					}
					ImGui::EndDragDropTarget();
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::BeginPopupContextItem("shader_context")) {
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::MenuItem("Set Default")) {
						header->shaderCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
						changed = true;
					}
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (ImGui::MenuItem("Clear")) {
						header->shaderCollectionId = TerraEngine::EMPTY_UUID;
						changed = true;
					}
					ImGui::EndPopup();
				}
			});
		}

		drawLabeledWidget("Base Color", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::ColorEdit4("##BaseColor", header->baseColorFactor)) changed = true;
		});

		drawLabeledWidget("Metallic", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::DragFloat("##Metallic", &header->metallicFactor, 0.01f, 0.0f, 1.0f)) changed = true;
		});

		drawLabeledWidget("Roughness", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::DragFloat("##Roughness", &header->roughnessFactor, 0.01f, 0.0f, 1.0f)) changed = true;
		});
		
		drawLabeledWidget("Emissive", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::ColorEdit3("##Emissive", header->emissiveFactor)) changed = true;
		});
		
		drawLabeledWidget("Normal Scale", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::DragFloat("##NormalScale", &header->normalScale, 0.01f)) changed = true;
		});

		drawLabeledWidget("Occlusion", [&]() {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::DragFloat("##Occlusion", &header->occlusionStrength, 0.01f, 0.0f, 1.0f)) changed = true;
		});

		ImGui::EndTable();
	}

	ImGui::Separator();
	ImGui::Text("Textures");

	auto drawTextureSlot = [&](const char* label, TextureSemantic semantic) {
		TextureBinding* targetBinding = nullptr;
		TextureBinding* allBindings = (TextureBinding*)(m_MaterialDataBuffer.data() + header->textureArrayOffset);
		
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (uint32_t i = 0; i < header->textureCount; ++i) {
			if (allBindings[i].semantic == semantic) {
				targetBinding = &allBindings[i];
				break;
			}
		}

		TerraEngine::UUID currentId = targetBinding ? targetBinding->textureUuid : TerraEngine::EMPTY_UUID;
		std::string assetName = "None";
		bool assetExists = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (currentId != TerraEngine::EMPTY_UUID) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& [path, meta] : AssetDatabase::Instance().GetAssets()) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (meta.guid == currentId) {
					assetName = path.filename().string();
					assetExists = true;
					break;
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!assetExists) {
				assetExists = TryResolveGeneratedPackageTextureName(currentId, assetName);
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!assetExists) {
				assetName = "(Missing)";
			}
		}

		ImGui::PushID(label);
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginTable("TextureSlotTable", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);

			char buf[128];
			strncpy(buf, assetName.c_str(), sizeof(buf));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (currentId != TerraEngine::EMPTY_UUID && !assetExists) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
			ImGui::InputText("##tex", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
			if (currentId != TerraEngine::EMPTY_UUID && !assetExists) ImGui::PopStyleColor();

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::BeginDragDropTarget()) {
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_TEXTURE")) {
					TerraEngine::UUID newId = *(const TerraEngine::UUID*)payload->Data;
					
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!targetBinding) {
						// Add new binding
						size_t insertPos = header->textureArrayOffset + (header->textureCount * sizeof(TextureBinding));
						m_MaterialDataBuffer.insert(m_MaterialDataBuffer.begin() + insertPos, sizeof(TextureBinding), 0);
						
						// CRITICAL: Re-acquire ALL pointers after resize as buffer may have moved
						header = (MaterialHeader*)m_MaterialDataBuffer.data();
						targetBinding = (TextureBinding*)(m_MaterialDataBuffer.data() + insertPos);
						
						targetBinding->semantic = semantic;
						targetBinding->samplerFilter = SamplerFilter::Bilinear;
						targetBinding->addressU = SamplerAddressMode::Repeat;
						targetBinding->addressV = SamplerAddressMode::Repeat;
						targetBinding->addressW = SamplerAddressMode::Repeat;
						targetBinding->uvScale[0] = 1.0f; targetBinding->uvScale[1] = 1.0f;
						
						header->textureCount++;
						header->floatParameterOffset += sizeof(TextureBinding);
					}
					
					targetBinding->textureUuid = newId;
					changed = true;
				}
				ImGui::EndDragDropTarget();
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (targetBinding && ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Clear")) {
					TextureBinding* allBindings = (TextureBinding*)(m_MaterialDataBuffer.data() + header->textureArrayOffset);
					int index = -1;
					// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
					for (uint32_t i = 0; i < header->textureCount; ++i) {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (&allBindings[i] == targetBinding) {
							index = i;
							break;
						}
					}

					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (index != -1) {
						size_t bindingOffset = header->textureArrayOffset + (index * sizeof(TextureBinding));
						m_MaterialDataBuffer.erase(m_MaterialDataBuffer.begin() + bindingOffset, m_MaterialDataBuffer.begin() + bindingOffset + sizeof(TextureBinding));

						// Re-acquire header after modification
						header = (MaterialHeader*)m_MaterialDataBuffer.data();
						header->textureCount--;
						header->floatParameterOffset -= sizeof(TextureBinding);
						changed = true;

						targetBinding = nullptr;
					}
				}
				ImGui::EndPopup();
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (targetBinding && assetExists) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(1); // Indent within the second column
				
				ImGui::SetNextItemWidth(-1.0f);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat2("##UVScale", targetBinding->uvScale, 0.01f)) changed = true;
				ImGui::SameLine(); ImGui::Text("UV Scale");

				ImGui::SetNextItemWidth(-1.0f);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::DragFloat2("##UVOffset", targetBinding->uvOffset, 0.01f)) changed = true;
				ImGui::SameLine(); ImGui::Text("UV Offset");
				
				const char* filters[] = { "Nearest", "Bilinear", "Trilinear", "Anisotropic" };
				int filterIdx = (int)targetBinding->samplerFilter;
				ImGui::SetNextItemWidth(-1.0f);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (ImGui::Combo("##Filter", &filterIdx, filters, IM_ARRAYSIZE(filters))) {
					targetBinding->samplerFilter = (SamplerFilter)filterIdx;
					changed = true;
				}
			}
			ImGui::EndTable();
		}
		ImGui::PopID();
	};

	drawTextureSlot("Base Map", TextureSemantic::Base);
	drawTextureSlot("Normal Map", TextureSemantic::Normal);
	drawTextureSlot("Metallic Roughness", TextureSemantic::MetallicRoughness);
	drawTextureSlot("Occlusion", TextureSemantic::Occlusion);
	drawTextureSlot("Emissive", TextureSemantic::Emissive);

	ImGui::Separator();
	
	// Material Flags
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("MaterialFlags", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		auto drawLabeledCheckbox = [&](const char* label, bool& value, auto onValueChange) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Checkbox(("##" + std::string(label)).c_str(), &value)) {
				onValueChange(value);
				changed = true;
			}
		};

		bool doubleSided = (header->flags & DoubleSided) != 0;
		drawLabeledCheckbox("Double Sided", doubleSided, [&](bool val) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (val) header->flags |= DoubleSided;
			else header->flags &= ~DoubleSided;
		});

		bool useVertexColor = (header->flags & UseVertexColor) != 0;
		drawLabeledCheckbox("Use Vertex Color", useVertexColor, [&](bool val) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (val) header->flags |= UseVertexColor;
			else header->flags &= ~UseVertexColor;
		});

		bool receivesShadows = (header->flags & ReceivesShadows) != 0;
		drawLabeledCheckbox("Receives Shadows", receivesShadows, [&](bool val) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (val) header->flags |= ReceivesShadows;
			else header->flags &= ~ReceivesShadows;
		});

		ImGui::EndTable();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (readOnlyPackageSubAsset) {
		ImGui::EndDisabled();
		changed = false;
	}
	if (changed) {
		// Inspector saves use the validated candidate/backup publication path.
		// A malformed edit or failed replacement therefore leaves the previous
		// material generation live instead of partially truncating the asset.
		if (TerraEngine::AssetPipeline::PublishMaterialAssetBytes(
			path,
			std::span<const std::uint8_t>(
				m_MaterialDataBuffer.data(),
				m_MaterialDataBuffer.size()))) {
			// Reload only after the complete candidate becomes visible.
			auto& assets = AssetDatabase::Instance().GetAssets();
			auto it = assets.find(std::filesystem::absolute(path));
			if (it != assets.end()) {
				ResourceManager::Instance().ReloadMaterial(it->second.guid);
			}
		}
	}
}

// InspectorWindow::DrawMeshInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawMeshInspector(const std::filesystem::path& path) {
	ImGui::Text("Mesh Asset: %s", path.filename().string().c_str());
	ImGui::Separator();

	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	if (const std::optional<AssetImportProgress> progress = AssetDatabase::Instance().GetImportProgress(absolutePath)) {
		DrawAssetImportProgress(*progress);
		return;
	}

	auto& assets = AssetDatabase::Instance().GetAssets();
	auto it = assets.find(absolutePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it == assets.end()) {
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Asset not found in database.");
		return;
	}

	const AssetMetadata& meta = it->second;
	std::filesystem::path fullCompiledPath = AssetDatabase::Instance().GetBuildRoot() / meta.compiledPath;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (meta.compiledPath.empty() || !std::filesystem::exists(fullCompiledPath)) {
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Compiled mesh file (.temf) not found.");
		return;
	}

	const std::optional<TemfInspectorSummary> production = ReadTemfInspectorSummary(fullCompiledPath);
	if (!production) {
		ImGui::TextColored(ImVec4(1, 0, 0, 1), "Compiled mesh requires a canonical TEMFPRD1 recook.");
		return;
	}

	auto drawInfoRow = [](const char* label, std::string_view value) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted(value.data(), value.data() + value.size());
	};

	auto drawInfoRowU32 = [&](const char* label, uint32_t value) {
		drawInfoRow(label, std::to_string(value).c_str());
	};

	ImGui::Text("Compiled Info (.temf - canonical production blocks)");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("MeshInfoTable", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

		drawInfoRowU32("Version", (uint32_t)production->header.formatVersion);
		drawInfoRowU32("Production Blocks", production->blockCount);
		drawInfoRowU32("Virtual Pages", production->header.pageCount);
		drawInfoRowU32("Quantization Domains", production->header.quantizationDomainCount);
		drawInfoRowU32("Dependencies", production->header.dependencyCount);
		char flagsBuf[16];
		snprintf(flagsBuf, sizeof(flagsBuf), "0x%08X", production->header.representationFlags);
		drawInfoRow("Representation Flags", flagsBuf);

		ImGui::EndTable();
	}

	VirtualGeometryAssetMetadata virtualGeometryMetadata{};
	if (AssetDatabase::Instance().TryGetVirtualGeometryMetadata(meta, virtualGeometryMetadata))
	{
		ImGui::Spacing();
		if (ImGui::CollapsingHeader("Virtual Geometry", ImGuiTreeNodeFlags_DefaultOpen))
		{
			const char* status = "Missing";
			if (virtualGeometryMetadata.needsRebuild)
			{
				status = "Needs Rebuild";
			}
			else if (virtualGeometryMetadata.hasVirtualGeometry)
			{
				status = "Cooked";
			}

			if (ImGui::BeginTable("VirtualGeometryInfoTable", 2, ImGuiTableFlags_SizingStretchProp))
			{
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

				drawInfoRow("Status", status);
				drawInfoRowU32("Payload Generation", virtualGeometryMetadata.payloadGeneration);
				drawInfoRowU32("Detail Levels", virtualGeometryMetadata.detailLevelCount);
				drawInfoRowU32("Source Triangles", virtualGeometryMetadata.sourceTriangleCount);
				drawInfoRowU32("Resident Meshlets", virtualGeometryMetadata.meshletCount);
				drawInfoRow("Codec / Profile",
					virtualGeometryMetadata.codec + " / "
						+ virtualGeometryMetadata.codecProfile);
				drawInfoRow("Format Hash",
					virtualGeometryMetadata.formatHash.empty()
						? "Unavailable"
						: virtualGeometryMetadata.formatHash);
				drawInfoRowU32("Encoded KiB",
					static_cast<std::uint32_t>(
						virtualGeometryMetadata.encodedGeometryBytes / 1024u));
				drawInfoRowU32("Decoded KiB",
					static_cast<std::uint32_t>(
						virtualGeometryMetadata.decodedGeometryBytes / 1024u));
				drawInfoRow("Stage Cache",
					virtualGeometryMetadata.cacheState + ": "
						+ virtualGeometryMetadata.cacheReason);
				drawInfoRowU32("Material Sections", virtualGeometryMetadata.materialSectionCount);
				drawInfoRow("Cone Safety", virtualGeometryMetadata.coneSafetyMetadataValid ? "Valid" : "Missing");
				drawInfoRowU32("Cone Records", virtualGeometryMetadata.coneSafetyRecordCount);
				drawInfoRowU32("Cone Eligible", virtualGeometryMetadata.coneEligibleClusterCount);
				drawInfoRowU32("Cone Disabled", virtualGeometryMetadata.coneDisabledClusterCount);

				ImGui::EndTable();
			}

			for (const std::string& warning : virtualGeometryMetadata.warnings)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f), "%s", warning.c_str());
			}
			if (!virtualGeometryMetadata.rebuildGuidance.empty())
			{
				ImGui::TextWrapped("%s", virtualGeometryMetadata.rebuildGuidance.c_str());
			}
			for (const std::string& reason : virtualGeometryMetadata.coneDisabledReasons)
			{
				ImGui::TextWrapped("%s", reason.c_str());
			}
		}
	}
	DrawVirtualGeometryOverrideControls(meta);
	std::vector<VirtualGeometryDetailDiagnosticMetadata> detailDiagnostics;
	if (AssetDatabase::Instance().TryGetVirtualGeometryDetailDiagnostics(meta, detailDiagnostics) && !detailDiagnostics.empty())
	{
		ImGui::Spacing();
		if (ImGui::CollapsingHeader("Virtual Geometry Cook Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)
			&& ImGui::BeginTable("VirtualGeometryCookDiagnostics", 8, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 60.0f);
			ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Desired T/M", ImGuiTableColumnFlags_WidthFixed, 95.0f);
			ImGui::TableSetupColumn("Actual T/M", ImGuiTableColumnFlags_WidthFixed, 95.0f);
			ImGui::TableSetupColumn("Nearest LOD", ImGuiTableColumnFlags_WidthFixed, 85.0f);
			ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			for (const VirtualGeometryDetailDiagnosticMetadata& diagnostic : detailDiagnostics)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%u", diagnostic.targetIndex);
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%.3f", diagnostic.requestedRatio);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(diagnostic.status.c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextWrapped("%s", diagnostic.reason.empty() ? "-" : diagnostic.reason.c_str());
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("%u / %u", diagnostic.desiredTriangleCount, diagnostic.desiredMeshletCount);
				ImGui::TableSetColumnIndex(5);
				if (diagnostic.actualTriangleCount > 0u || diagnostic.actualMeshletCount > 0u)
				{
					ImGui::Text("%u / %u", diagnostic.actualTriangleCount, diagnostic.actualMeshletCount);
				}
				else
				{
					ImGui::TextUnformatted("-");
				}
				ImGui::TableSetColumnIndex(6);
				ImGui::Text("%u", diagnostic.nearestGeneratedLevelIndex);
				ImGui::TableSetColumnIndex(7);
				ImGui::TextWrapped("%s", diagnostic.message.empty() ? "-" : diagnostic.message.c_str());
			}
			ImGui::EndTable();
		}
	}

	ImGui::Spacing();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::CollapsingHeader("Meshlet Payload", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginTable("MeshPayloadTable", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

			drawInfoRowU32("Header Bytes", production->header.headerBytes);
			drawInfoRowU32("Directory Bytes", static_cast<uint32_t>(production->header.directoryBytes));
			drawInfoRowU32("Payload Offset", static_cast<uint32_t>(production->header.payloadOffset));
			drawInfoRowU32("Payload Bytes", static_cast<uint32_t>(production->header.payloadBytes));
			drawInfoRowU32("Page Table Offset", static_cast<uint32_t>(production->header.pageTableOffset));
			drawInfoRowU32("Block Table Offset", static_cast<uint32_t>(production->header.blockTableOffset));
			drawInfoRowU32("Quantization Table Offset", static_cast<uint32_t>(production->header.quantizationTableOffset));

			ImGui::EndTable();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::CollapsingHeader("Bounds & Quantization", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::BeginTable("MeshQuantTable", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

			drawInfoRowU32("Quantization Domains", production->header.quantizationDomainCount);
			drawInfoRowU32("Selected Quantization Profile", production->header.selectedQuantizationProfileId);
			drawInfoRowU32("Logical Page Size", production->header.logicalPageSizeBytes);
			drawInfoRow("Format Contract", "TEMFPRD1");

			ImGui::EndTable();
		}
	}
}

// InspectorWindow::DrawTextureInspector is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void InspectorWindow::DrawTextureInspector(const std::filesystem::path& path) {
	ImGui::Text("Texture Asset: %s", path.filename().string().c_str());
	ImGui::Separator();

	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	if (const std::optional<AssetImportProgress> progress = AssetDatabase::Instance().GetImportProgress(absolutePath)) {
		DrawAssetImportProgress(*progress);
		return;
	}

	auto& assets = AssetDatabase::Instance().GetAssets();
	auto it = assets.find(absolutePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (it == assets.end()) {
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Asset not found in database.");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (AssetDatabase::Instance().IsAssetCompiling(absolutePath)) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
		// Use a negative fraction animated by time to show an indeterminate progress bar
		ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "Compiling...");
		ImGui::PopStyleColor();
		return;
	}

	const AssetMetadata& meta = it->second;
	std::filesystem::path fullCompiledPath = AssetDatabase::Instance().GetBuildRoot() / meta.compiledPath;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (meta.compiledPath.empty() || !std::filesystem::exists(fullCompiledPath)) {
		ImGui::TextColored(ImVec4(1, 1, 0, 1), "Compiled texture file (.tetf) not found.");
		return;
	}

	TETFHeader header{};
	TETFDesc desc{};
	std::ifstream in(fullCompiledPath, std::ios::binary);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (in.is_open()) {
		in.read((char*)&header, sizeof(TETFHeader));
		if (header.magic == kMagic) {
			std::vector<TETFChunkEntry> chunks(header.chunkCount);
			in.seekg(header.tocOffset, std::ios::beg);
			in.read((char*)chunks.data(), chunks.size() * sizeof(TETFChunkEntry));
			
			bool foundDesc = false;
			uint32_t descFourCC = MakeFourCC('D', 'E', 'S', 'C');
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& chunk : chunks) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (chunk.id == descFourCC) {
					in.seekg(chunk.offset, std::ios::beg);
					in.read((char*)&desc, sizeof(TETFDesc));
					foundDesc = true;
					break;
				}
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!foundDesc) {
				ImGui::TextColored(ImVec4(1, 0, 0, 1), "DESC chunk not found in TETF!");
				return;
			}
		} else {
			ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid TETF header!");
			return;
		}
	}

	ImGui::Text("Compiled Info (.tetf)");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("TextureInfoTable", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

		auto drawRow = [](const char* label, const char* value) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(value);
		};

		char buf[64];
		snprintf(buf, sizeof(buf), "%u x %u", (uint32_t)desc.width, (uint32_t)desc.height);
		drawRow("Dimensions", buf);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (desc.depth > 1) drawRow("Depth", std::to_string(desc.depth).c_str());
		if (desc.layers > 1) drawRow("Layers", std::to_string(desc.layers).c_str());
		drawRow("Mips", std::to_string(desc.mipCount).c_str());

		drawRow("Format", VkFormatToString(desc.vkFormat));

		const char* semantics[] = { "Unknown", "Color", "Data" };
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (desc.semantic < 3) drawRow("Semantic", semantics[desc.semantic]);

		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::Text("Import Settings");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginTable("TextureImportTable", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("Widget", ImGuiTableColumnFlags_WidthStretch);

		// Mutable reference to update metadata directly in the database
		AssetMetadata& metadata = const_cast<AssetMetadata&>(it->second);
		bool settingsChanged = false;

		// UASTC Quality
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("UASTC Quality");
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1.0f);

		int currentQuality = 1; // Balanced default
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("quality")) {
			currentQuality = metadata.settings["quality"].get<int>();
		}

		const char* qualityPresets[] = { "Low (Fast)", "Balanced", "High (Slower)", "Ultra (Very Slow)" };
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Combo("##UastcQuality", &currentQuality, qualityPresets, IM_ARRAYSIZE(qualityPresets))) {
			metadata.settings["quality"] = currentQuality;
			settingsChanged = true;
		}

		// Generate Mipmaps
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted("Generate Mipmaps");
		ImGui::TableSetColumnIndex(1);
		
		bool genMips = true;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (metadata.settings.contains("generateMipmaps")) {
			genMips = metadata.settings["generateMipmaps"].get<bool>();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Checkbox("##GenMips", &genMips)) {
			metadata.settings["generateMipmaps"] = genMips;
			settingsChanged = true;
		}

		// Downsampling Filter
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (genMips) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted("Filter Type");
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);

			const char* filters[] = { "Box", "Triangle", "Lanczos", "Kaiser" };
			std::string currentFilter = "Lanczos";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (metadata.settings.contains("filterType")) {
				currentFilter = metadata.settings["filterType"].get<std::string>();
			}

			int filterIdx = 2; // Lanczos default
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (int i = 0; i < 4; ++i) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (currentFilter == filters[i]) {
					filterIdx = i;
					break;
				}
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::Combo("##FilterType", &filterIdx, filters, IM_ARRAYSIZE(filters))) {
				metadata.settings["filterType"] = filters[filterIdx];
				settingsChanged = true;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (settingsChanged) {
			// Trigger re-import which will call CompileTexture with new settings
			AssetDatabase::Instance().ImportAssetWithProgress(absolutePath, true);
			
			// Notify the renderer to reload the updated asset from disk
			RenderSystem::Instance().ReloadTexture(metadata.guid);
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("Reimport", ImVec2(-1, 0))) {
		AssetDatabase::Instance().ImportAssetWithProgress(absolutePath, true);
		RenderSystem::Instance().ReloadTexture(it->second.guid);
	}
}
