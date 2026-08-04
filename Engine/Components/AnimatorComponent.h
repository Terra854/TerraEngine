// AnimatorComponent.h - Authored animation references and execution policies.
//
// This component records creator intent that belongs in scenes and prefabs.
// AnimationSystem owns every runtime instance, pose, cursor, GPU allocation,
// effective mode, and generation-safe handle outside this authored record.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Core/Object/Component.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <string>
#include <string_view>

// AnimatorComponent contains only stable authored references and policies. A
// component replacement receives a new ObjectManager component generation, so
// stale AnimatorInstanceStore work cannot be mistaken for this authored data.
struct AnimatorComponent final : public Component<AnimatorComponent>
{
	static constexpr std::string_view kComponentName = "AnimatorComponent";

	std::uint32_t schemaVersion = TerraEngine::Animation::kAnimatorComponentSchemaVersion;
	TerraEngine::UUID controllerAssetId = TerraEngine::EMPTY_UUID;
	// Imported prefabs may be immediately previewed before a controller has been
	// authored. The stable clip identity supplies that direct-playback fallback;
	// controller-driven instances ignore it once a compatible program is bound.
	TerraEngine::UUID defaultClipAssetId = TerraEngine::EMPTY_UUID;
	// Playback loops by default so imported characters remain visibly animated.
	// When a controller is assigned, the choice follows every active state clip;
	// transition exit times and crossfade durations still use unwrapped time.
	bool loopDefaultClip = true;
	// Runtime worlds start authored playback by default. Disable this to hold the
	// configured initial pose until gameplay or tooling sends an explicit Play
	// command. This switch applies to direct clips and Animation Controllers.
	bool playAutomatically = true;
	// Playback begins from this persistent fixed-timeline frame whenever the editor
	// or runtime creates a fresh binding. Controllers apply it to their resolved
	// entry clip. Animation time uses 60 fixed ticks per second, and binding clamps
	// the value to the resolved clip duration after import or reimport.
	std::uint32_t defaultClipStartFrame = 0U;
	TerraEngine::UUID skeletonOverrideAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID avatarOverrideAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID retargetProfileOverrideAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID ragdollProfileOverrideAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID motionDatabaseOverrideAssetId = TerraEngine::EMPTY_UUID;
	TerraEngine::Animation::AnimationExecutionPolicy requestedMode =
		TerraEngine::Animation::AnimationExecutionPolicy::Auto;
	// Root motion is opt-in because extracting it requires authoritative CPU work.
	// Ordinary visual-only crowds therefore remain eligible for GPU evaluation.
	TerraEngine::Animation::AnimatorRootMotionPolicy rootMotionPolicy =
		TerraEngine::Animation::AnimatorRootMotionPolicy::Ignore;
	TerraEngine::Animation::AnimatorUpdatePolicy updatePolicy =
		TerraEngine::Animation::AnimatorUpdatePolicy::EveryFrame;
	TerraEngine::Animation::AnimatorTimePolicy timePolicy =
		TerraEngine::Animation::AnimatorTimePolicy::Scaled;
	// Initial parameters use a JSON object string until the typed controller
	// parameter records land. This is authored source, never a runtime snapshot.
	std::string initialParametersJson = "{}";
	TerraEngine::Animation::AnimatorEventPolicy eventPolicy =
		TerraEngine::Animation::AnimatorEventPolicy::Dispatch;
	TerraEngine::Animation::AnimatorCullingPolicy cullingPolicy =
		TerraEngine::Animation::AnimatorCullingPolicy::Automatic;
	// Diagnostic overrides are authored opt-ins. Collected histories and pose
	// captures remain transient runtime data and are deliberately absent here.
	std::uint32_t diagnosticOverrides = 0U;

	// Reflection exposes enums as fixed-width values because the generic editor
	// and scene serializer already understand uint32 properties. The component
	// keeps strongly typed fields for native callers.
	[[nodiscard]] std::uint32_t GetRequestedModeValue() const noexcept
	{
		return static_cast<std::uint32_t>(requestedMode);
	}

	void SetRequestedModeValue(std::uint32_t value) noexcept
	{
		requestedMode = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback)
			? static_cast<TerraEngine::Animation::AnimationExecutionPolicy>(value)
			: TerraEngine::Animation::AnimationExecutionPolicy::Auto;
	}

	[[nodiscard]] std::uint32_t GetRootMotionPolicyValue() const noexcept
	{
		return static_cast<std::uint32_t>(rootMotionPolicy);
	}

	void SetRootMotionPolicyValue(std::uint32_t value) noexcept
	{
		rootMotionPolicy = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorRootMotionPolicy::ApplyToCharacter)
			? static_cast<TerraEngine::Animation::AnimatorRootMotionPolicy>(value)
			: TerraEngine::Animation::AnimatorRootMotionPolicy::Ignore;
	}

	[[nodiscard]] std::uint32_t GetUpdatePolicyValue() const noexcept
	{
		return static_cast<std::uint32_t>(updatePolicy);
	}

	void SetUpdatePolicyValue(std::uint32_t value) noexcept
	{
		updatePolicy = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorUpdatePolicy::Manual)
			? static_cast<TerraEngine::Animation::AnimatorUpdatePolicy>(value)
			: TerraEngine::Animation::AnimatorUpdatePolicy::EveryFrame;
	}

	[[nodiscard]] std::uint32_t GetTimePolicyValue() const noexcept
	{
		return static_cast<std::uint32_t>(timePolicy);
	}

	void SetTimePolicyValue(std::uint32_t value) noexcept
	{
		timePolicy = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorTimePolicy::Fixed)
			? static_cast<TerraEngine::Animation::AnimatorTimePolicy>(value)
			: TerraEngine::Animation::AnimatorTimePolicy::Scaled;
	}

	[[nodiscard]] const std::string& GetInitialParametersJson() const noexcept
	{
		return initialParametersJson;
	}

	void SetInitialParametersJson(const std::string& value)
	{
		initialParametersJson = value;
	}

	[[nodiscard]] std::uint32_t GetEventPolicyValue() const noexcept
	{
		return static_cast<std::uint32_t>(eventPolicy);
	}

	void SetEventPolicyValue(std::uint32_t value) noexcept
	{
		eventPolicy = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorEventPolicy::Suppress)
			? static_cast<TerraEngine::Animation::AnimatorEventPolicy>(value)
			: TerraEngine::Animation::AnimatorEventPolicy::Dispatch;
	}

	[[nodiscard]] std::uint32_t GetCullingPolicyValue() const noexcept
	{
		return static_cast<std::uint32_t>(cullingPolicy);
	}

	void SetCullingPolicyValue(std::uint32_t value) noexcept
	{
		cullingPolicy = value <= static_cast<std::uint32_t>(TerraEngine::Animation::AnimatorCullingPolicy::SuspendVisualWhenInvisible)
			? static_cast<TerraEngine::Animation::AnimatorCullingPolicy>(value)
			: TerraEngine::Animation::AnimatorCullingPolicy::Automatic;
	}

	static void reflect(entt::meta_factory<AnimatorComponent>& factory)
	{
		// Only the properties registered here are durable scene/prefab authoring
		// data. Runtime handles, poses, cursors, and effective mode have no entry.
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::schemaVersion>(factory, "schemaVersion");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::controllerAssetId>(factory, "controllerAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::defaultClipAssetId>(factory, "defaultClipAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::loopDefaultClip>(factory, "loopDefaultClip");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::playAutomatically>(factory, "playAutomatically");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::defaultClipStartFrame>(factory, "defaultClipStartFrame");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::skeletonOverrideAssetId>(factory, "skeletonOverrideAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::avatarOverrideAssetId>(factory, "avatarOverrideAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::retargetProfileOverrideAssetId>(factory, "retargetProfileOverrideAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::ragdollProfileOverrideAssetId>(factory, "ragdollProfileOverrideAssetId");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::motionDatabaseOverrideAssetId>(factory, "motionDatabaseOverrideAssetId");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetRequestedModeValue, &AnimatorComponent::GetRequestedModeValue>(factory, "requestedMode");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetRootMotionPolicyValue, &AnimatorComponent::GetRootMotionPolicyValue>(factory, "rootMotionPolicy");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetUpdatePolicyValue, &AnimatorComponent::GetUpdatePolicyValue>(factory, "updatePolicy");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetTimePolicyValue, &AnimatorComponent::GetTimePolicyValue>(factory, "timePolicy");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetInitialParametersJson, &AnimatorComponent::GetInitialParametersJson>(factory, "initialParameters");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetEventPolicyValue, &AnimatorComponent::GetEventPolicyValue>(factory, "eventPolicy");
		TerraEngine::Reflection::RegisterAccessorProperty<&AnimatorComponent::SetCullingPolicyValue, &AnimatorComponent::GetCullingPolicyValue>(factory, "cullingPolicy");
		TerraEngine::Reflection::RegisterDataProperty<&AnimatorComponent::diagnosticOverrides>(factory, "diagnosticOverrides");
	}
};
