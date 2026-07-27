// AnimatorInspectorMetadata.h - User-facing names and guidance for Animator authoring.
//
// Stable reflection property names are intentionally kept out of the visible
// Inspector. This table gives creators a plain-language label, explains when a
// setting matters, and supplies the named choices used by enum controls.

#pragma once

#include "Engine/Animation/Runtime/AnimatorPlayback.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace TerraEngine::Editor::Animation
{
	enum class AnimatorInspectorControlKind : std::uint8_t
	{
		ReadOnly,
		Asset,
		Choice,
		Toggle,
		JsonObject,
		UnsignedInteger
	};

	struct AnimatorInspectorChoice
	{
		std::uint32_t value = 0U;
		std::string_view label;
		std::string_view guidance;
	};

	struct AnimatorInspectorPropertyMetadata
	{
		std::string_view stableName;
		std::string_view label;
		std::string_view explanation;
		std::string_view usage;
		AnimatorInspectorControlKind control = AnimatorInspectorControlKind::ReadOnly;
		std::span<const AnimatorInspectorChoice> choices;
	};

	// The Inspector uses one normalized range model for both the editable slider
	// and the authored summary. Keeping the clamp outside ImGui makes the rule
	// deterministic and directly testable: a current frame can never render or
	// publish above the duration of the currently published playback clip.
	struct AnimatorInspectorFrameRange
	{
		std::uint32_t currentFrame = 0U;
		std::uint32_t maximumFrame = 0U;
	};

	[[nodiscard]] inline AnimatorInspectorFrameRange BuildAnimatorInspectorFrameRange(
		std::int64_t requestedFrame,
		TerraEngine::Animation::AnimationFixedTick clipDurationTicks,
		bool loop = false) noexcept
	{
		// A looped clip's exact duration aliases tick zero. Use the shared runtime
		// rule here so the Inspector never advertises that alias as a selectable
		// maximum while one-shot clips still expose their true final endpoint.
		const TerraEngine::Animation::AnimationFixedTick maximumDistinctTick =
			TerraEngine::Animation::ClampAnimatorInitialPlaybackTick(
				clipDurationTicks,
				clipDurationTicks,
				loop);
		const std::uint64_t nonNegativeDuration = maximumDistinctTick > 0
			? static_cast<std::uint64_t>(maximumDistinctTick)
			: 0U;
		const std::uint32_t maximumFrame = static_cast<std::uint32_t>(
			std::min<std::uint64_t>(
				nonNegativeDuration,
				std::numeric_limits<std::uint32_t>::max()));
		const std::uint64_t nonNegativeRequestedFrame = requestedFrame > 0
			? static_cast<std::uint64_t>(requestedFrame)
			: 0U;
		return {
			static_cast<std::uint32_t>(std::min(
				nonNegativeRequestedFrame,
				static_cast<std::uint64_t>(maximumFrame))),
			maximumFrame
		};
	}

	[[nodiscard]] inline std::string FormatAnimatorInspectorFrameRange(
		const AnimatorInspectorFrameRange& range)
	{
		return std::to_string(range.currentFrame)
			+ "/" + std::to_string(range.maximumFrame);
	}

	inline constexpr std::array kRequestedModeChoices{
		AnimatorInspectorChoice{0U, "Auto (Recommended)", "Chooses the fastest compatible execution path automatically."},
		AnimatorInspectorChoice{1U, "GPU Visual", "Forces visual GPU evaluation. Use only when animation does not drive authoritative gameplay."},
		AnimatorInspectorChoice{2U, "Hybrid Authoritative", "Keeps authoritative CPU results while using GPU evaluation for visual work."},
		AnimatorInspectorChoice{3U, "CPU Deterministic Fallback", "Forces deterministic CPU evaluation for rollback, recording, or diagnostics."}
	};
	inline constexpr std::array kRootMotionChoices{
		AnimatorInspectorChoice{0U, "Ignore", "Discards root movement and keeps the object transform unchanged."},
		AnimatorInspectorChoice{1U, "Extract Only (Recommended)", "Calculates root movement for gameplay code without moving the object automatically."},
		AnimatorInspectorChoice{2U, "Apply to Object", "Moves the object's Transform using the animation's extracted root motion."},
		AnimatorInspectorChoice{3U, "Apply to Character", "Sends extracted movement through the character controller for collision-aware motion."}
	};
	inline constexpr std::array kUpdatePolicyChoices{
		AnimatorInspectorChoice{0U, "Every Frame (Recommended)", "Updates during normal frame animation scheduling."},
		AnimatorInspectorChoice{1U, "Fixed Step", "Updates on fixed simulation ticks for deterministic gameplay or physics timing."},
		AnimatorInspectorChoice{2U, "Manual", "Updates only when gameplay or tooling explicitly requests evaluation."}
	};
	inline constexpr std::array kTimePolicyChoices{
		AnimatorInspectorChoice{0U, "Scaled (Recommended)", "Uses game time and follows pause or slow-motion time scaling."},
		AnimatorInspectorChoice{1U, "Unscaled", "Uses real elapsed time and continues independently of game-time scaling."},
		AnimatorInspectorChoice{2U, "Fixed", "Uses fixed-tick time and is normally paired with Fixed Step updates."}
	};
	inline constexpr std::array kEventPolicyChoices{
		AnimatorInspectorChoice{0U, "Dispatch (Recommended)", "Delivers animation events to normal runtime consumers immediately."},
		AnimatorInspectorChoice{1U, "Retain", "Keeps events in the bounded runtime history so code or tools can consume them later."},
		AnimatorInspectorChoice{2U, "Suppress", "Evaluates animation without delivering authored animation events."}
	};
	inline constexpr std::array kCullingPolicyChoices{
		AnimatorInspectorChoice{0U, "Automatic (Recommended)", "Suspends only visual work that is safe to skip while preserving gameplay authority."},
		AnimatorInspectorChoice{1U, "Always Update", "Keeps all animation work active even when the object is not visible."},
		AnimatorInspectorChoice{2U, "Suspend Visual When Invisible", "Stops visual pose work while invisible; do not use when off-screen pose output is required."}
	};

	inline constexpr std::array kAnimatorInspectorProperties{
		AnimatorInspectorPropertyMetadata{
			"schemaVersion", "Serialization Version",
			"Version of the saved Animator component data.",
			"Managed automatically by the editor. You never need to change this value.",
			AnimatorInspectorControlKind::ReadOnly, {}},
		AnimatorInspectorPropertyMetadata{
			"controllerAssetId", "Animation Controller",
			"The state machine or animation graph that decides which clips play and how they blend.",
			"Drag an Animation Controller here for normal gameplay. Leave empty to use Default Clip directly.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"defaultClipAssetId", "Default Clip",
			"A direct-playback fallback used when no Animation Controller is assigned.",
			"Drag an Animation Clip here for simple playback or imported-model preview. A controller takes priority when assigned.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"loopDefaultClip", "Loop Default Clip",
			"Repeats the active animation clip after it reaches the end.",
			"Leave enabled for idle or preview animation. Disable it for a one-shot clip. With an Animation Controller, the choice follows active state clips without disabling authored transitions or crossfades.",
			AnimatorInspectorControlKind::Toggle, {}},
		AnimatorInspectorPropertyMetadata{
			"playAutomatically", "Play Automatically",
			"Controls whether this Animator begins advancing when the scene enters Play Mode.",
			"Leave enabled for normal playback. Disable it to hold the configured start pose until C# or tooling calls Play; switching it off while running returns to that authored pose. This setting also applies to Animation Controllers.",
			AnimatorInspectorControlKind::Toggle, {}},
		AnimatorInspectorPropertyMetadata{
			"defaultClipStartFrame", "Default Clip Start Frame",
			"The fixed-timeline frame used when playback is freshly bound.",
			"The control shows current/max and cannot exceed the published clip duration. A looping clip ends at its last distinct frame because its duration endpoint is the next loop's frame 0. Set 0 to begin at the start; the animation timeline uses 60 frames per second, so frame 30 is 0.5 seconds. With an Animation Controller, this selects the controller entry clip's initial frame.",
			AnimatorInspectorControlKind::UnsignedInteger, {}},
		AnimatorInspectorPropertyMetadata{
			"skeletonOverrideAssetId", "Skeleton Override",
			"Replaces the skeleton supplied by the imported model or prefab.",
			"Normally leave empty. Assign only when the model should use a different, compatible Skeleton asset.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"avatarOverrideAssetId", "Avatar Mapping Override",
			"Replaces the semantic bone mapping used by humanoid and retargeting workflows.",
			"Normally leave empty. Assign a project-provided avatar mapping only when the model's default mapping is unsuitable.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"retargetProfileOverrideAssetId", "Retarget Profile",
			"Maps animation authored for one skeleton onto a different target skeleton.",
			"Assign a Retarget Profile when source clips and this character use different rigs; otherwise leave empty.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"ragdollProfileOverrideAssetId", "Ragdoll Profile",
			"Defines physics bodies, constraints, drive settings, and animation recovery behavior.",
			"Assign a Ragdoll Profile only when this character can enter physical ragdoll simulation.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"motionDatabaseOverrideAssetId", "Motion Database",
			"Searchable motion samples used by motion-matching controller nodes.",
			"Assign a Motion Database when the controller performs motion matching; ordinary clip graphs do not need one.",
			AnimatorInspectorControlKind::Asset, {}},
		AnimatorInspectorPropertyMetadata{
			"requestedMode", "Execution Mode",
			"Selects where pose evaluation runs and whether results must be authoritative.",
			"Keep Auto for normal authoring. Force a mode only for a specific gameplay requirement or diagnostics.",
			AnimatorInspectorControlKind::Choice, kRequestedModeChoices},
		AnimatorInspectorPropertyMetadata{
			"rootMotionPolicy", "Root Motion",
			"Controls what happens to movement authored on the animation's root bone.",
			"Extract Only is the safest default. Apply to Character for collision-aware characters, or Apply to Object for direct transform motion.",
			AnimatorInspectorControlKind::Choice, kRootMotionChoices},
		AnimatorInspectorPropertyMetadata{
			"updatePolicy", "Update Schedule",
			"Controls when the animator is evaluated.",
			"Use Every Frame normally, Fixed Step for simulation-aligned animation, or Manual when code owns every update.",
			AnimatorInspectorControlKind::Choice, kUpdatePolicyChoices},
		AnimatorInspectorPropertyMetadata{
			"timePolicy", "Time Source",
			"Selects which clock advances animation playback.",
			"Use Scaled for gameplay, Unscaled for pause-independent animation, or Fixed with fixed-tick simulation.",
			AnimatorInspectorControlKind::Choice, kTimePolicyChoices},
		AnimatorInspectorPropertyMetadata{
			"initialParameters", "Initial Controller Parameters",
			"JSON object applied once when the runtime controller instance is created.",
			R"(Enter controller parameter names and values, for example {"Speed": 1.0, "Grounded": true}. Names and types must match the controller.)",
			AnimatorInspectorControlKind::JsonObject, {}},
		AnimatorInspectorPropertyMetadata{
			"eventPolicy", "Animation Events",
			"Controls whether authored clip events are delivered, retained, or ignored.",
			"Use Dispatch normally, Retain when another system polls events later, or Suppress when this Animator must stay silent.",
			AnimatorInspectorControlKind::Choice, kEventPolicyChoices},
		AnimatorInspectorPropertyMetadata{
			"cullingPolicy", "Off-Screen Updates",
			"Controls animation work when the object is outside visible cameras.",
			"Use Automatic normally. Always Update for off-screen gameplay dependencies; suspend visual work only when pose output is not required.",
			AnimatorInspectorControlKind::Choice, kCullingPolicyChoices},
		AnimatorInspectorPropertyMetadata{
			"diagnosticOverrides", "Diagnostic Overrides",
			"Reserved per-Animator diagnostic flags stored for compatibility with diagnostic tooling.",
			"No public flags are currently defined. Use the Animation Diagnostics window instead of editing this value.",
			AnimatorInspectorControlKind::ReadOnly, {}}
	};

	// Returns null only for a newly introduced reflected field that has not yet
	// received user-facing documentation. Tests guard against that regression.
	[[nodiscard]] constexpr const AnimatorInspectorPropertyMetadata*
		FindAnimatorInspectorProperty(std::string_view stableName) noexcept
	{
		for(const AnimatorInspectorPropertyMetadata& property : kAnimatorInspectorProperties)
		{
			if(property.stableName == stableName)
			{
				return &property;
			}
		}
		return nullptr;
	}

	[[nodiscard]] constexpr const AnimatorInspectorChoice*
		FindAnimatorInspectorChoice(
			const AnimatorInspectorPropertyMetadata& property,
			std::uint32_t value) noexcept
	{
		for(const AnimatorInspectorChoice& choice : property.choices)
		{
			if(choice.value == value)
			{
				return &choice;
			}
		}
		return nullptr;
	}
}
