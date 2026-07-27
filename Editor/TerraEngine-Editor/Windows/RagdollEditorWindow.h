// RagdollEditorWindow.h - detached, generation-safe ragdoll authoring surface.
#pragma once

#include "../Animation/AnimationAuthoringTransaction.h"
#include "../Framework/EditorWindow.h"
#include "Engine/Animation/Assets/RagdollProfileAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Engine/Animation/Ragdoll/RagdollProfileValidator.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class MainEditorSubsystem;

namespace TerraEngine::Editor::Animation {
	struct RagdollAuthoringGenerationSnapshot {
		TerraEngine::Animation::AnimationAssetHandle profile{};
		TerraEngine::Animation::AnimationAssetHandle skeleton{};
		TerraEngine::Animation::AnimationCompatibilityFingerprint skeletonFingerprint = 0U;
		std::uint64_t referencePoseHash = 0U;
		std::uint64_t persistedBytesHash = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
		auto operator<=>(const RagdollAuthoringGenerationSnapshot&) const = default;
	};

	// Recovery clips are resolved outside the profile so a stale generation or
	// changed compatibility fingerprint fails closed before publication.
	struct RagdollRecoveryClipDependency {
		TerraEngine::Animation::AnimationGuid assetId{};
		TerraEngine::Animation::AnimationGeneration generation = 0U;
		TerraEngine::Animation::AnimationCompatibilityFingerprint skeletonFingerprint = 0U;
		std::string displayName;

		[[nodiscard]] bool IsValid() const noexcept;
		auto operator<=>(const RagdollRecoveryClipDependency&) const = default;
	};

	using RagdollCollisionLayerProvider = std::function<
		std::vector<TerraEngine::Animation::RagdollCollisionLayerRule>()>;
	using RagdollRecoveryClipProvider = std::function<
		std::optional<RagdollRecoveryClipDependency>(TerraEngine::Animation::AnimationGuid)>;

	// The preview packet is provider-neutral: tests, ImGui, and future renderer
	// adapters can consume the same finite world-space line segments.
	struct RagdollVisualizationLine {
		TerraEngine::Animation::AnimationFloat3 start{};
		TerraEngine::Animation::AnimationFloat3 end{};
		std::uint64_t bodyId = 0U;
		std::uint64_t colliderId = 0U;
		bool selected = false;

		auto operator<=>(const RagdollVisualizationLine&) const = default;
	};

	struct RagdollCollisionVisualizationEntry {
		std::uint64_t bodyId = 0U;
		std::uint64_t colliderId = 0U;
		TerraEngine::Animation::RagdollColliderShape shape =
			TerraEngine::Animation::RagdollColliderShape::Capsule;
		TerraEngine::Animation::AnimationTransform bodyPose{};
		TerraEngine::Animation::AnimationTransform colliderPose{};
		TerraEngine::Animation::AnimationFloat3 dimensions{};
		bool sensor = false;
		bool selected = false;

		auto operator<=>(const RagdollCollisionVisualizationEntry&) const = default;
	};

	struct RagdollActivationPreviewJoint {
		std::uint64_t stableJointId = 0U;
		std::uint64_t bodyId = 0U;
		float physicsWeight = 0.0F;
		TerraEngine::Animation::AnimationTransform jointPose{};
		TerraEngine::Animation::AnimationTransform bodyPose{};

		auto operator<=>(const RagdollActivationPreviewJoint&) const = default;
	};

	struct RagdollActivationPreviewResult {
		float normalizedTime = 0.0F;
		bool activating = true;
		std::vector<RagdollActivationPreviewJoint> joints;
		std::vector<RagdollVisualizationLine> poseLines;

		auto operator<=>(const RagdollActivationPreviewResult&) const = default;
	};

	struct RagdollRecoveryTestInput {
		TerraEngine::Animation::RagdollRecoveryPosture posture =
			TerraEngine::Animation::RagdollRecoveryPosture::Any;
		std::uint64_t activeContactMask = 0U;
		float uprightDot = 1.0F;
		float poseErrorMeters = 0.0F;
		float linearSpeed = 0.0F;
		float angularSpeed = 0.0F;
		float availableClearanceRadiusMeters = 100.0F;
		float availableClearanceHeightMeters = 100.0F;
	};

	struct RagdollRecoveryTestResult {
		bool eligible = false;
		std::vector<std::string> failures;
	};

	struct RagdollAuthoringCompileRequest {
		AnimationAuthoringCompileRequest authoring;
		TerraEngine::Animation::SkeletonAsset skeleton;
		RagdollAuthoringGenerationSnapshot sourceGeneration{};
		std::vector<TerraEngine::Animation::RagdollCollisionLayerRule> collisionLayers;
		std::vector<RagdollRecoveryClipDependency> recoveryDependencies;
	};

	struct RagdollAuthoringCompileProduct {
		AnimationAuthoringCompileResult authoring;
		TerraEngine::Animation::AnimationStatus validationStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		TerraEngine::Animation::RagdollProfileValidationReport validation;
		std::vector<std::string> dependencyDiagnostics;
	};

	class RagdollEditorModel final {
	public:
		bool Begin(
			const TerraEngine::Animation::RagdollProfileAsset& profile,
			const TerraEngine::Animation::SkeletonAsset& skeleton,
			const RagdollAuthoringGenerationSnapshot& generation,
			RagdollCollisionLayerProvider collisionLayerProvider = {},
			RagdollRecoveryClipProvider recoveryClipProvider = {});
		// RebindSkeleton changes only the detached candidate.  The on-disk profile
		// remains the last-known-good revision until a generated, fully validated
		// replacement is published through the normal atomic save path.
		bool RebindSkeleton(
			const TerraEngine::Animation::SkeletonAsset& skeleton,
			const RagdollAuthoringGenerationSnapshot& generation);
		TerraEngine::Animation::AnimationStatus GenerateFromSkeleton();
		bool ReplaceBody(const TerraEngine::Animation::RagdollBodyRecord& body);
		bool ReplaceCollider(
			std::uint64_t bodyId,
			const TerraEngine::Animation::RagdollColliderRecord& collider);
		bool ReplaceConstraint(const TerraEngine::Animation::RagdollConstraintRecord& constraint);
		bool ReplaceBlend(const TerraEngine::Animation::RagdollBlendRecord& blend);
		bool ReplaceRecovery(const TerraEngine::Animation::RagdollRecoveryRecord& recovery);
		// SelectRecoveryClip copies the current resolved GUID, generation, and
		// fingerprint as one identity instead of inventing profile-owned metadata.
		bool SelectRecoveryClip(
			std::uint64_t candidateId,
			TerraEngine::Animation::AnimationGuid clipAssetId);
		bool RemoveRecovery(std::uint64_t candidateId);
		void SetCollisionVisualizationEnabled(bool enabled) noexcept;
		void SelectCollider(std::uint64_t colliderId);
		void SetActivationPreview(float normalizedTime, bool activating);
		[[nodiscard]] RagdollRecoveryTestResult TestRecovery(
			std::uint64_t candidateId,
			const RagdollRecoveryTestInput& input) const;
		TerraEngine::Animation::AnimationStatus Validate();

		AnimationAuthoringStatus PrepareCommit(RagdollAuthoringCompileRequest& outRequest) noexcept;
		AnimationAuthoringStatus Cancel() noexcept;

		[[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
		[[nodiscard]] bool IsDirty() const noexcept { return dirty_; }
		[[nodiscard]] bool HasPendingEdit() const noexcept { return editing_; }
		[[nodiscard]] bool CollisionVisualizationEnabled() const noexcept {
			return collisionVisualizationEnabled_;
		}
		[[nodiscard]] const TerraEngine::Animation::RagdollProfileAsset& Candidate() const noexcept {
			return candidate_;
		}
		[[nodiscard]] const TerraEngine::Animation::RagdollProfileAsset& Published() const noexcept {
			return publishedProfile_;
		}
		[[nodiscard]] const TerraEngine::Animation::SkeletonAsset& Skeleton() const noexcept {
			return skeleton_;
		}
		[[nodiscard]] const RagdollAuthoringGenerationSnapshot& Generation() const noexcept {
			return generation_;
		}
		[[nodiscard]] const TerraEngine::Animation::RagdollProfileValidationReport& Validation() const noexcept {
			return validation_;
		}
		[[nodiscard]] const std::vector<std::string>& DependencyDiagnostics() const noexcept {
			return dependencyDiagnostics_;
		}
		[[nodiscard]] const std::vector<RagdollCollisionVisualizationEntry>& CollisionVisualization() const noexcept {
			return collisionVisualization_;
		}
		[[nodiscard]] const std::vector<RagdollVisualizationLine>& CollisionGeometry() const noexcept {
			return collisionGeometry_;
		}
		[[nodiscard]] const RagdollActivationPreviewResult& ActivationPreview() const noexcept {
			return activationPreview_;
		}
		[[nodiscard]] std::size_t UndoDepth() const noexcept { return history_.UndoDepth(); }
		[[nodiscard]] std::size_t RedoDepth() const noexcept { return history_.RedoDepth(); }
		[[nodiscard]] const std::string& Diagnostic() const noexcept { return diagnostic_; }

	private:
		friend class RagdollAuthoringService;

		bool BeginMutation() noexcept;
		void RefreshDerivedPreview();
		AnimationAuthoringStatus AdoptPublishedRevision(
			AnimationAuthoringRevision revision,
			RagdollAuthoringGenerationSnapshot generation) noexcept;

		TerraEngine::Animation::RagdollProfileAsset publishedProfile_{};
		TerraEngine::Animation::RagdollProfileAsset candidate_{};
		TerraEngine::Animation::SkeletonAsset skeleton_{};
		RagdollAuthoringGenerationSnapshot generation_{};
		TerraEngine::Animation::RagdollProfileValidationReport validation_{};
		std::vector<std::string> dependencyDiagnostics_;
		std::vector<RagdollCollisionVisualizationEntry> collisionVisualization_;
		std::vector<RagdollVisualizationLine> collisionGeometry_;
		RagdollActivationPreviewResult activationPreview_{};
		AnimationAuthoringRevision publishedRevision_{};
		AnimationAuthoringTransaction transaction_;
		AnimationAuthoringHistory history_;
		RagdollCollisionLayerProvider collisionLayerProvider_;
		RagdollRecoveryClipProvider recoveryClipProvider_;
		std::string diagnostic_;
		std::uint64_t selectedColliderId_ = 0U;
		bool collisionVisualizationEnabled_ = true;
		bool loaded_ = false;
		bool editing_ = false;
		bool dirty_ = false;
	};

	struct RagdollAuthoringPublishResult {
		AnimationAuthoringStatus authoringStatus = AnimationAuthoringStatus::Rejected;
		TerraEngine::Animation::AnimationStatus status =
			TerraEngine::Animation::AnimationStatus::NotReady;
		bool published = false;
		std::string diagnostic;
	};

	class RagdollAuthoringService final {
	public:
		using SnapshotProvider = std::function<
			std::optional<RagdollAuthoringGenerationSnapshot>()>;

		// CompileCandidate is worker-safe: all providers were resolved into copied
		// value packets during PrepareCommit and no editor or filesystem state is used.
		[[nodiscard]] static RagdollAuthoringCompileProduct CompileCandidate(
			const RagdollAuthoringCompileRequest& request) noexcept;
		static RagdollAuthoringPublishResult CompletePublish(
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const RagdollAuthoringCompileProduct& product,
			const SnapshotProvider& currentSnapshot);
		static RagdollAuthoringPublishResult Undo(
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const SnapshotProvider& currentSnapshot);
		static RagdollAuthoringPublishResult Redo(
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const SnapshotProvider& currentSnapshot);

	private:
		static RagdollAuthoringPublishResult PublishHistory(
			bool undo,
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const SnapshotProvider& currentSnapshot);
		static bool PublishRevision(
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const SnapshotProvider& currentSnapshot,
			const AnimationAuthoringRevision& expectedCurrent,
			const AnimationAuthoringRevision& replacement,
			RagdollAuthoringGenerationSnapshot& outGeneration,
			std::string& outDiagnostic) noexcept;
	};
}

class RagdollEditorWindow final : public EditorWindow {
public:
	explicit RagdollEditorWindow(MainEditorSubsystem* owner = nullptr);
	~RagdollEditorWindow() override;

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	std::unique_ptr<State> state_;
};
