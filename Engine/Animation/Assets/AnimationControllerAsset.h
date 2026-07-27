// AnimationControllerAsset.h - Bounded authored controller graph document.
//
// This is durable authoring data only. T065 later compiles it into immutable
// evaluation opcodes, so these records deliberately contain stable IDs and no
// runtime pointers, cursors, or editor-owned allocations.

#pragma once

#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/AnimationTypes.h"
#include "File Formats/TerraEngineAnimationFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kAnimationControllerAssetCurrentVersion = 1U;
	inline constexpr std::uint32_t kAnimationControllerPayloadMagic = MakeAnimationFourCC('C', 'T', 'R', 'L');

	enum class AnimationControllerParameterType : std::uint8_t {
		Float = 0U, Integer = 1U, Boolean = 2U, Trigger = 3U,
		Vector = 4U, Transform = 5U, Tag = 6U
	};
	using AnimationControllerParameterValue = std::variant<float, std::int32_t, bool,
		std::monostate, AnimationFloat3, AnimationTransform, std::string>;

	struct AnimationControllerParameter {
		std::uint64_t stableId = 0U;
		std::string name;
		AnimationControllerParameterType type = AnimationControllerParameterType::Float;
		AnimationControllerParameterValue defaultValue = 0.0F;
		auto operator<=>(const AnimationControllerParameter&) const = default;
	};
	using AnimationControllerGraphVariable = AnimationControllerParameter;

	struct AnimationControllerReusableBinding {
		std::uint64_t stableId = 0U;
		std::uint64_t sourceParameterId = 0U;
		std::uint64_t destinationVariableId = 0U;
		auto operator<=>(const AnimationControllerReusableBinding&) const = default;
	};

	enum class AnimationControllerNodeKind : std::uint8_t {
		Clip = 0U, BlendTree1D = 1U, BlendTree2D = 2U, Subgraph = 3U,
		MotionMatch = 4U, ProceduralPose = 5U
	};
	enum class AnimationControllerBlendOutOfDomainPolicy : std::uint8_t { Clamp = 0U, Extrapolate = 1U };
	// Samples name child nodes by stable ID so a controller stays relocatable and
	// the compiler can reject cycles before publishing an immutable program.
	struct AnimationControllerBlendSample {
		std::uint64_t stableId = 0U;
		std::uint64_t childNodeId = 0U;
		float positionX = 0.0F;
		float positionY = 0.0F;
		auto operator<=>(const AnimationControllerBlendSample &) const = default;
	};
	struct AnimationControllerNode {
		std::uint64_t stableId = 0U;
		AnimationControllerNodeKind kind = AnimationControllerNodeKind::Clip;
		std::uint32_t flags = 0U;
		AnimationGuid dependencyAssetId{};
		AnimationGeneration dependencyGeneration = 0U;
		AnimationCompatibilityFingerprint dependencyFingerprint = 0U;
		AnimationControllerBlendOutOfDomainPolicy outOfDomainPolicy = AnimationControllerBlendOutOfDomainPolicy::Clamp;
		AnimationCompatibilityFingerprint outputCompatibilityFingerprint = 0U;
		std::vector<AnimationControllerBlendSample> blendSamples;
		auto operator<=>(const AnimationControllerNode&) const = default;
	};
	struct AnimationControllerState {
		std::uint64_t stableId = 0U;
		std::uint64_t nodeId = 0U;
		std::uint64_t parentStateId = 0U;
		bool entry = false;
		bool exit = false;
		bool anyState = false;
		auto operator<=>(const AnimationControllerState&) const = default;
	};
	// Append-only authored policy values survive round-trip before the compiler
	// converts them into immutable runtime transition records.
	enum class AnimationControllerInterruptionSource : std::uint8_t { None = 0U, Current = 1U, Next = 2U };
	enum class AnimationControllerInterruptionPolicy : std::uint8_t { Reject = 0U, Replace = 1U };
	enum class AnimationControllerTriggerConsumption : std::uint8_t { SingleConsumer = 0U, MultiConsumer = 1U };
	enum class AnimationControllerTriggerReset : std::uint8_t { EndStep = 0U, Manual = 1U };
	enum class AnimationControllerTriggerFailure : std::uint8_t { Preserve = 0U, Reset = 1U };
	struct AnimationControllerTransition {
		std::uint64_t stableId = 0U;
		std::uint64_t sourceStateId = 0U;
		std::uint64_t destinationStateId = 0U;
		std::uint64_t conditionParameterId = 0U;
		AnimationFixedTick durationTicks = 0;
		AnimationFixedTick exitTimeTicks = 0;
		float destinationOffsetNormalized = 0.0F;
		std::uint32_t priority = 0U;
		AnimationControllerInterruptionSource interruptionSource = AnimationControllerInterruptionSource::Current;
		AnimationControllerInterruptionPolicy interruptionPolicy = AnimationControllerInterruptionPolicy::Reject;
		AnimationControllerTriggerConsumption triggerConsumption = AnimationControllerTriggerConsumption::SingleConsumer;
		AnimationControllerTriggerReset triggerReset = AnimationControllerTriggerReset::EndStep;
		AnimationControllerTriggerFailure triggerFailure = AnimationControllerTriggerFailure::Preserve;
		auto operator<=>(const AnimationControllerTransition&) const = default;
	};
	struct AnimationControllerLayer {
		std::uint64_t stableId = 0U;
		std::uint64_t rootStateId = 0U;
		AnimationGuid maskAssetId{};
		AnimationGeneration maskGeneration = 0U;
		AnimationCompatibilityFingerprint maskFingerprint = 0U;
		bool additive = false;
		float defaultWeight = 1.0F;
		auto operator<=>(const AnimationControllerLayer&) const = default;
	};
	struct AnimationControllerSyncMarker {
		std::uint64_t stableId = 0U;
		std::string name;
		std::uint64_t clipEventId = 0U;
		std::uint64_t stateId = 0U;
		auto operator<=>(const AnimationControllerSyncMarker&) const = default;
	};
	struct AnimationControllerSyncGroup {
		std::uint64_t stableId = 0U;
		std::string name;
		std::uint64_t leaderStateId = 0U;
		// Marker names remain author-friendly, while event and state IDs let T070
		// resolve phase without changing the frozen AnimationClip v1 format.
		std::vector<AnimationControllerSyncMarker> markers;
		auto operator<=>(const AnimationControllerSyncGroup&) const = default;
	};
	struct AnimationControllerSubgraphDependency {
		std::uint64_t stableId = 0U;
		AnimationGuid controllerAssetId{};
		AnimationGeneration generation = 0U;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		auto operator<=>(const AnimationControllerSubgraphDependency&) const = default;
	};

	struct AnimationControllerAsset {
		AnimationGuid assetId{};
		AnimationGuid skeletonAssetId{};
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		std::uint16_t formatVersion = kAnimationControllerAssetCurrentVersion;
		std::vector<AnimationControllerParameter> parameters;
		std::vector<AnimationControllerGraphVariable> graphVariables;
		std::vector<AnimationControllerReusableBinding> reusableBindings;
		std::vector<AnimationControllerNode> nodes;
		std::vector<AnimationControllerState> states;
		std::vector<AnimationControllerTransition> transitions;
		std::vector<AnimationControllerLayer> layers;
		std::vector<AnimationControllerSyncGroup> syncGroups;
		std::vector<AnimationControllerSubgraphDependency> subgraphDependencies;
		auto operator<=>(const AnimationControllerAsset&) const = default;
	};

	TERRAENGINE_API AnimationStatus ValidateAnimationControllerAsset(const AnimationControllerAsset& asset) noexcept;
	TERRAENGINE_API AnimationStatus SerializeAnimationControllerAsset(const AnimationControllerAsset& asset, std::vector<std::uint8_t>& output);
	TERRAENGINE_API AnimationStatus DeserializeAnimationControllerAsset(std::span<const std::uint8_t> input,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint, AnimationControllerAsset& output);
}
