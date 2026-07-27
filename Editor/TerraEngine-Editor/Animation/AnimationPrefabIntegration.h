// AnimationPrefabIntegration.h - typed prefab edits and exact Play Mode restore.
//
// Worker code uses this model to prepare copied authoring candidates and typed
// Scene transactions. Only PublishPrepared/PublishPlayModeRestore are protected
// boundaries; no method reaches into ObjectManager or component storage.

#pragma once

#include "Engine/Scene/TransactionalMutation.h"
#include "Engine/Utilities/UUID.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace TerraEngine::Editor::Animation {
	inline constexpr std::uint32_t kAnimationPrefabResultSchemaVersion = 1U;
	inline constexpr std::size_t kMaximumAnimationPrefabProperties = 512U;
	inline constexpr std::size_t kMaximumAnimationPrefabSelection = 128U;
	inline constexpr std::size_t kMaximumPlayModeCaptures = 8U;

	enum class AnimationPrefabStatus : std::uint8_t {
		Ok = 0U,
		InvalidArgument,
		NotFound,
		Incompatible,
		Stale,
		CapacityExceeded,
		Rejected,
		InternalError
	};

	// These append-only identities mirror reflected authored AnimatorComponent
	// names. A prefab path therefore survives display-name changes and instance
	// UUID remapping without storing fragile vector indices.
	enum class AnimationPrefabPropertyId : std::uint16_t {
		ControllerAssetId = 0U,
		DefaultClipAssetId,
		SkeletonOverrideAssetId,
		AvatarOverrideAssetId,
		RetargetProfileOverrideAssetId,
		RagdollProfileOverrideAssetId,
		MotionDatabaseOverrideAssetId,
		RequestedMode,
		RootMotionPolicy,
		UpdatePolicy,
		TimePolicy,
		InitialParameters,
		EventPolicy,
		CullingPolicy,
		DiagnosticOverrides,
		AttachmentTarget,
		AttachmentJoint,
		AttachmentOffset,
		MorphDefault
	};

	struct AnimationPrefabPropertyPath {
		TerraEngine::UUID sourceObjectId = TerraEngine::EMPTY_UUID;
		AnimationPrefabPropertyId property = AnimationPrefabPropertyId::ControllerAssetId;
		// Collection records such as attachments and morph defaults keep a stable
		// authored identity here instead of using their current array position.
		TerraEngine::UUID elementId = TerraEngine::EMPTY_UUID;

		auto operator<=>(const AnimationPrefabPropertyPath&) const = default;
	};

	struct AnimationPrefabConnectedReference {
		TerraEngine::UUID sourceObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID resolvedInstanceObjectId = TerraEngine::EMPTY_UUID;

		auto operator<=>(const AnimationPrefabConnectedReference&) const = default;
	};

	using AnimationPrefabValue = std::variant<
		std::monostate,
		bool,
		std::uint32_t,
		std::int64_t,
		float,
		std::string,
		TerraEngine::UUID,
		AnimationPrefabConnectedReference>;

	struct AnimationPrefabDocument {
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		std::uint64_t generation = 0U;
		std::map<AnimationPrefabPropertyPath, AnimationPrefabValue> defaults;

		auto operator<=>(const AnimationPrefabDocument&) const = default;
	};

	struct AnimationPrefabObjectBinding {
		TerraEngine::UUID instanceObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t lifetimeGeneration = 0U;
		std::uint64_t metadataGeneration = 0U;
		std::uint64_t componentGeneration = 0U;
		std::uint64_t hierarchyRevision = 0U;
		std::uint64_t transformRevision = 0U;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t parentLifetimeGeneration = 0U;
		std::uint64_t parentTransformRevision = 0U;

		auto operator<=>(const AnimationPrefabObjectBinding&) const = default;
	};

	struct AnimationPrefabInstance {
		TerraEngine::UUID instanceId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		std::uint64_t generation = 0U;
		std::uint64_t sceneGeneration = 0U;
		std::uint64_t sceneRevision = 0U;
		std::map<TerraEngine::UUID, AnimationPrefabObjectBinding> objects;
		std::map<AnimationPrefabPropertyPath, AnimationPrefabValue> overrides;

		auto operator<=>(const AnimationPrefabInstance&) const = default;
	};

	enum class AnimationPrefabMixedState : std::uint8_t {
		Uniform = 0U,
		Mixed,
		Unavailable,
		Incompatible
	};

	struct AnimationPrefabMixedValue {
		std::uint32_t schemaVersion = kAnimationPrefabResultSchemaVersion;
		AnimationPrefabMixedState state = AnimationPrefabMixedState::Unavailable;
		AnimationPrefabValue uniformValue;
		std::size_t compatibleCount = 0U;
		std::size_t unavailableCount = 0U;

		auto operator<=>(const AnimationPrefabMixedValue&) const = default;
	};

	struct AnimationPrefabWorkDeclaration {
		std::string_view phase = "UI/EditorAuthoring";
		std::string_view reads = "copied prefab source, instance bindings, authored generations";
		std::string_view writes = "owned candidates and typed deferred Scene transaction";
		std::string_view protectedBoundary = "PublishAnimationPrefabAuthoring";
		bool workerSafePreparation = true;
		bool protectedPublication = true;
		bool directLiveObjectMutation = false;

		auto operator<=>(const AnimationPrefabWorkDeclaration&) const = default;
	};

	enum class AnimationPrefabMutationKind : std::uint8_t {
		GroupedOverride = 0U,
		ApplyOverrides,
		RevertOverrides
	};

	struct AnimationPrefabPreparedInstance {
		TerraEngine::UUID instanceId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedGeneration = 0U;
		AnimationPrefabInstance candidate;
	};

	struct AnimationPrefabPreparedMutation {
		std::uint32_t schemaVersion = kAnimationPrefabResultSchemaVersion;
		AnimationPrefabMutationKind kind = AnimationPrefabMutationKind::GroupedOverride;
		AnimationPrefabWorkDeclaration declaration;
		TerraEngine::UUID expectedSourceId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedSourceGeneration = 0U;
		bool writesSource = false;
		AnimationPrefabDocument sourceCandidate;
		std::vector<AnimationPrefabPreparedInstance> instances;
		TerraEngine::Scene::Transaction sceneTransaction;
		std::size_t authoredMutationCount = 0U;
		std::size_t compatibleInstanceCount = 0U;
		std::size_t skippedIncompatibleInstanceCount = 0U;
	};

	// The five byte ranges intentionally preserve authored serialization rather
	// than reconstructing fields from runtime state. Embedded NULs and unknown
	// forward-compatible chunks therefore round-trip exactly on Play Mode exit.
	struct AnimationPlayModeAuthoredState {
		TerraEngine::UUID sceneId = TerraEngine::EMPTY_UUID;
		std::uint64_t sceneGeneration = 0U;
		std::vector<std::byte> sceneDocumentBytes;
		std::vector<std::byte> animatorComponentBytes;
		std::vector<std::byte> hierarchyBytes;
		std::vector<std::byte> attachmentBytes;
		std::vector<std::byte> animationAssetBytes;

		auto operator<=>(const AnimationPlayModeAuthoredState&) const = default;
	};

	struct AnimationPlayModeCaptureHandle {
		std::uint32_t slotIndex = 0xffffffffU;
		std::uint32_t generation = 0U;

		[[nodiscard]] bool IsValid() const noexcept {
			return slotIndex < kMaximumPlayModeCaptures && generation != 0U;
		}

		auto operator<=>(const AnimationPlayModeCaptureHandle&) const = default;
	};

	struct AnimationPlayModePreparedRestore {
		std::uint32_t schemaVersion = kAnimationPrefabResultSchemaVersion;
		AnimationPrefabWorkDeclaration declaration{
			"PostFrame/PlayModeExit",
			"copied authored Play Mode capture and current scene generation",
			"owned exact restore candidate",
			"PublishAnimationPlayModeRestore",
			true,
			true,
			false};
		AnimationPlayModeCaptureHandle handle;
		AnimationPlayModeAuthoredState restored;
	};

	[[nodiscard]] std::string_view ToString(AnimationPrefabStatus status) noexcept;
	[[nodiscard]] std::string_view ToStableComponentName(AnimationPrefabPropertyId property) noexcept;
	[[nodiscard]] std::string_view ToStablePropertyName(AnimationPrefabPropertyId property) noexcept;
	[[nodiscard]] std::string StableAnimationPrefabPropertyPath(
		const AnimationPrefabPropertyPath& path);
	[[nodiscard]] AnimationPrefabValue ResolveAnimationPrefabValue(
		const AnimationPrefabDocument& source,
		const AnimationPrefabInstance& instance,
		const AnimationPrefabPropertyPath& path);

	class AnimationPrefabIntegration final {
	public:
		AnimationPrefabIntegration() = default;
		~AnimationPrefabIntegration();
		AnimationPrefabIntegration(const AnimationPrefabIntegration&) = delete;
		AnimationPrefabIntegration& operator=(const AnimationPrefabIntegration&) = delete;

		[[nodiscard]] AnimationPrefabStatus InspectMixedValue(
			const AnimationPrefabDocument& source,
			std::span<const AnimationPrefabInstance> instances,
			const AnimationPrefabPropertyPath& path,
			AnimationPrefabMixedValue& output) const noexcept;
		[[nodiscard]] AnimationPrefabStatus DuplicateConnected(
			const AnimationPrefabInstance& source,
			TerraEngine::UUID duplicateInstanceId,
			const std::map<TerraEngine::UUID, AnimationPrefabObjectBinding>& duplicateObjects,
			AnimationPrefabInstance& output) const noexcept;

		// Preparation may run on a worker because every input and output is owned.
		// The returned Scene transaction is queued by the editor at its protected
		// authoring boundary; this class never commits it against ObjectManager.
		[[nodiscard]] AnimationPrefabStatus PrepareGroupedOverride(
			const AnimationPrefabDocument& source,
			std::span<const AnimationPrefabInstance> instances,
			const AnimationPrefabPropertyPath& path,
			const AnimationPrefabValue& value,
			AnimationPrefabPreparedMutation& output) const noexcept;
		[[nodiscard]] AnimationPrefabStatus PrepareApplyOverrides(
			const AnimationPrefabDocument& source,
			const AnimationPrefabInstance& instance,
			std::span<const AnimationPrefabPropertyPath> selectedPaths,
			AnimationPrefabPreparedMutation& output) const noexcept;
		[[nodiscard]] AnimationPrefabStatus PrepareRevertOverrides(
			const AnimationPrefabDocument& source,
			const AnimationPrefabInstance& instance,
			std::span<const AnimationPrefabPropertyPath> selectedPaths,
			AnimationPrefabPreparedMutation& output) const noexcept;
		[[nodiscard]] AnimationPrefabStatus PublishPrepared(
			const AnimationPrefabPreparedMutation& prepared,
			AnimationPrefabDocument& source,
			std::vector<AnimationPrefabInstance>& instances) const noexcept;

		[[nodiscard]] AnimationPrefabStatus CapturePlayMode(
			const AnimationPlayModeAuthoredState& authored,
			AnimationPlayModeCaptureHandle& output) noexcept;
		[[nodiscard]] AnimationPrefabStatus PreparePlayModeRestore(
			AnimationPlayModeCaptureHandle handle,
			TerraEngine::UUID currentSceneId,
			std::uint64_t currentSceneGeneration,
			AnimationPlayModePreparedRestore& output) const noexcept;
		[[nodiscard]] AnimationPrefabStatus PublishPlayModeRestore(
			const AnimationPlayModePreparedRestore& prepared,
			AnimationPlayModeAuthoredState& authored) noexcept;
		void ClearPlayModeCaptures() noexcept;
		[[nodiscard]] std::size_t ActivePlayModeCaptureCount() const noexcept;

	private:
		struct PlayModeSlot {
			std::uint32_t generation = 1U;
			bool retired = false;
			std::optional<AnimationPlayModeAuthoredState> captured;
		};

		[[nodiscard]] static AnimationPrefabStatus ResolvePlayModeSlot(
			std::array<PlayModeSlot, kMaximumPlayModeCaptures>& slots,
			AnimationPlayModeCaptureHandle handle,
			PlayModeSlot*& output) noexcept;
		[[nodiscard]] static AnimationPrefabStatus ResolvePlayModeSlot(
			const std::array<PlayModeSlot, kMaximumPlayModeCaptures>& slots,
			AnimationPlayModeCaptureHandle handle,
			const PlayModeSlot*& output) noexcept;
		static void RetirePlayModeSlot(PlayModeSlot& slot) noexcept;

		mutable std::mutex playModeMutex_;
		std::array<PlayModeSlot, kMaximumPlayModeCaptures> playModeSlots_{};
		std::size_t activePlayModeCaptures_ = 0U;
	};
}
