// RetargetEditorWindow.h - generation-safe retarget profile authoring surface.
#pragma once

#include "../Animation/AnimationAuthoringTransaction.h"
#include "../Framework/EditorWindow.h"
#include "Engine/Animation/Assets/RetargetProfileAsset.h"
#include "Engine/Animation/Assets/SkeletonAsset.h"
#include "Engine/Animation/Retargeting/RetargetValidator.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class MainEditorSubsystem;

namespace TerraEngine::Editor::Animation {
	// This snapshot labels every durable input used by one detached edit. The
	// persisted hash protects loose files while the handles protect live asset
	// generations, so a reimport cannot be overwritten by an older editor tab.
	struct RetargetAuthoringGenerationSnapshot {
		TerraEngine::Animation::AnimationAssetHandle profile{};
		TerraEngine::Animation::AnimationAssetHandle sourceSkeleton{};
		TerraEngine::Animation::AnimationAssetHandle targetSkeleton{};
		TerraEngine::Animation::AnimationCompatibilityFingerprint sourceFingerprint = 0U;
		TerraEngine::Animation::AnimationCompatibilityFingerprint targetFingerprint = 0U;
		std::uint64_t sourceReferencePoseHash = 0U;
		std::uint64_t targetReferencePoseHash = 0U;
		std::uint64_t persistedBytesHash = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
		auto operator<=>(const RetargetAuthoringGenerationSnapshot&) const = default;
	};

	// Each row reports an error in the two reference poses after the source rig
	// is normalized to the target height. These values are editor diagnostics,
	// not a replacement for runtime motion validation.
	struct RetargetMappingError {
		std::uint64_t mappingId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		float appliedTranslationScale = 1.0F;
		float positionErrorMeters = 0.0F;
		float rotationErrorRadians = 0.0F;
		bool usesAuthoredOrientationBasis = false;

		auto operator<=>(const RetargetMappingError&) const = default;
	};

	// Auto-map never guesses when a semantic has more than one eligible target.
	// The complete candidate remains editable and the UI can explain each skipped
	// row instead of silently picking the lowest stable ID.
	struct RetargetAutoMapDiagnostic {
		std::string semantic;
		std::vector<std::uint64_t> sourceJointIds;
		std::vector<std::uint64_t> targetJointIds;
		std::string message;

		auto operator<=>(const RetargetAutoMapDiagnostic&) const = default;
	};

	struct RetargetContactError {
		std::uint64_t contactId = 0U;
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		TerraEngine::Animation::AnimationFloat3 alignedSourcePosition{};
		TerraEngine::Animation::AnimationFloat3 targetPosition{};
		float positionErrorMeters = 0.0F;
		float orientationErrorRadians = 0.0F;
		float toleranceMeters = 0.0F;
		bool preservePosition = false;
		bool preserveOrientation = false;
		bool required = false;
		bool withinTolerance = false;

		auto operator<=>(const RetargetContactError&) const = default;
	};

	// This provider-neutral packet contains only stable IDs and copied geometry.
	// ImGui, automation, and future renderer-backed previews can consume the same
	// side-by-side pose without retaining mutable skeleton or profile objects.
	struct RetargetPreviewJoint {
		std::uint64_t jointId = 0U;
		std::uint64_t parentJointId = 0U;
		TerraEngine::Animation::AnimationFloat3 position{};

		auto operator<=>(const RetargetPreviewJoint&) const = default;
	};

	struct RetargetPosePreviewPacket {
		std::vector<RetargetPreviewJoint> sourceJoints;
		std::vector<RetargetPreviewJoint> targetJoints;
		std::vector<RetargetContactError> contacts;
		std::uint64_t sourcePoseHash = 0U;
		std::uint64_t targetPoseHash = 0U;

		auto operator<=>(const RetargetPosePreviewPacket&) const = default;
	};

	struct RetargetQuantitativeErrorReport {
		std::vector<RetargetMappingError> mappings;
		std::vector<RetargetContactError> contacts;
		float coverage = 0.0F;
		float meanPositionErrorMeters = 0.0F;
		float maximumPositionErrorMeters = 0.0F;
		float maximumRotationErrorRadians = 0.0F;
		float maximumContactPositionErrorMeters = 0.0F;
		float maximumContactOrientationErrorRadians = 0.0F;
		std::uint32_t contactsWithinTolerance = 0U;

		auto operator<=>(const RetargetQuantitativeErrorReport&) const = default;
	};

	// The model owns a detached candidate. UI controls never mutate the selected
	// cooked object in place; publication is the only path back to durable bytes.
	class RetargetEditorModel final {
	public:
		bool Begin(
			const TerraEngine::Animation::RetargetProfileAsset& profile,
			const TerraEngine::Animation::SkeletonAsset& sourceSkeleton,
			const TerraEngine::Animation::SkeletonAsset& targetSkeleton,
			const RetargetAuthoringGenerationSnapshot& generation,
			bool bindSelectedSkeletons = false);
		[[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
		[[nodiscard]] bool IsDirty() const noexcept { return dirty_; }
		[[nodiscard]] const TerraEngine::Animation::RetargetProfileAsset& Candidate() const noexcept {
			return candidate_;
		}
		[[nodiscard]] const TerraEngine::Animation::SkeletonAsset& SourceSkeleton() const noexcept {
			return sourceSkeleton_;
		}
		[[nodiscard]] const TerraEngine::Animation::SkeletonAsset& TargetSkeleton() const noexcept {
			return targetSkeleton_;
		}
		[[nodiscard]] const RetargetAuthoringGenerationSnapshot& Generation() const noexcept {
			return generation_;
		}
		[[nodiscard]] const RetargetQuantitativeErrorReport& Errors() const noexcept {
			return errors_;
		}
		[[nodiscard]] const TerraEngine::Animation::RetargetValidationResult& Validation() const noexcept {
			return validation_;
		}
		[[nodiscard]] const std::vector<RetargetAutoMapDiagnostic>& AutoMapDiagnostics() const noexcept {
			return autoMapDiagnostics_;
		}
		[[nodiscard]] const RetargetPosePreviewPacket& Preview() const noexcept {
			return preview_;
		}

		// Auto-map is semantic-only and sorts both rigs by stable ID. The same
		// inputs therefore always produce byte-for-byte identical mapping rows.
		TerraEngine::Animation::AnimationStatus AutoMap();
		bool SetMappingTarget(std::uint64_t mappingId, std::uint64_t targetJointId);
		bool SetMappingRequired(std::uint64_t mappingId, bool required);
		bool AddExplicitMapping(std::uint64_t sourceJointId, std::uint64_t targetJointId);
		bool RemoveMapping(std::uint64_t mappingId);
		bool AddContact(std::uint64_t mappingId);
		bool SetContactTarget(std::uint64_t contactId, std::uint64_t targetJointId);
		bool SetContactTolerance(std::uint64_t contactId, float toleranceMeters);
		bool SetContactPolicy(
			std::uint64_t contactId,
			bool preservePosition,
			bool preserveOrientation,
			bool required);
		// Chains are authored from each rig root to the selected effector. This
		// preserves the root-to-effector order required by runtime validation.
		bool AddChainToEffectors(
			std::uint64_t sourceEffectorJointId,
			std::uint64_t targetEffectorJointId);
		bool SetContactTargetChain(std::uint64_t contactId, std::uint64_t targetChainId);
		bool RemoveChain(std::uint64_t chainId);
		bool RemoveContact(std::uint64_t contactId);
		TerraEngine::Animation::AnimationStatus RefreshErrors();
		TerraEngine::Animation::AnimationStatus Validate();
		void MarkPublished() noexcept { dirty_ = false; }

	private:
		TerraEngine::Animation::RetargetProfileAsset candidate_{};
		TerraEngine::Animation::SkeletonAsset sourceSkeleton_{};
		TerraEngine::Animation::SkeletonAsset targetSkeleton_{};
		RetargetAuthoringGenerationSnapshot generation_{};
		RetargetQuantitativeErrorReport errors_{};
		TerraEngine::Animation::RetargetValidationResult validation_{};
		std::vector<RetargetAutoMapDiagnostic> autoMapDiagnostics_;
		RetargetPosePreviewPacket preview_{};
		bool loaded_ = false;
		bool dirty_ = false;
	};

	struct RetargetAuthoringPublishResult {
		TerraEngine::Animation::AnimationStatus status =
			TerraEngine::Animation::AnimationStatus::NotReady;
		bool published = false;
		std::string diagnostic;
	};

	struct RetargetAuthoringCompileProduct {
		AnimationAuthoringCompileResult authoring;
		TerraEngine::Animation::RetargetValidationResult validation;
	};

	// The service groups every detached mapping/contact edit into one shared
	// AnimationAuthoringTransaction and records complete before/after bytes in
	// AnimationAuthoringHistory. Only CompleteCommit/Undo/Redo may touch disk.
	class RetargetAuthoringService final {
	public:
		using SnapshotProvider = std::function<
			std::optional<RetargetAuthoringGenerationSnapshot>()>;

		RetargetAuthoringService();
		AnimationAuthoringStatus Open(
			const std::filesystem::path& destination,
			const TerraEngine::Animation::RetargetProfileAsset& profile,
			const TerraEngine::Animation::SkeletonAsset& sourceSkeleton,
			const TerraEngine::Animation::SkeletonAsset& targetSkeleton,
			const RetargetAuthoringGenerationSnapshot& generation,
			std::span<const std::uint8_t> persistedBytes,
			bool bindSelectedSkeletons = false) noexcept;
		AnimationAuthoringStatus BeginEdit() noexcept;
		[[nodiscard]] RetargetEditorModel* EditableModel() noexcept;
		[[nodiscard]] const RetargetEditorModel& Model() const noexcept { return model_; }
		AnimationAuthoringStatus PrepareCommit(AnimationAuthoringCompileRequest& outRequest) noexcept;
		[[nodiscard]] static RetargetAuthoringCompileProduct CompileCandidate(
			const AnimationAuthoringCompileRequest& request,
			const TerraEngine::Animation::SkeletonAsset& sourceSkeleton,
			const TerraEngine::Animation::SkeletonAsset& targetSkeleton) noexcept;
		AnimationAuthoringStatus CompleteCommit(
			const RetargetAuthoringCompileProduct& product,
			const SnapshotProvider& currentSnapshot) noexcept;
		AnimationAuthoringStatus Cancel() noexcept;
		AnimationAuthoringStatus Undo(const SnapshotProvider& currentSnapshot) noexcept;
		AnimationAuthoringStatus Redo(const SnapshotProvider& currentSnapshot) noexcept;

		[[nodiscard]] bool HasOpenDocument() const noexcept { return published_.assetId.IsValid(); }
		[[nodiscard]] bool HasCandidate() const noexcept { return editing_; }
		[[nodiscard]] std::size_t UndoDepth() const noexcept { return history_.UndoDepth(); }
		[[nodiscard]] std::size_t RedoDepth() const noexcept { return history_.RedoDepth(); }
		[[nodiscard]] std::uint64_t Generation() const noexcept { return published_.generation; }
		[[nodiscard]] const std::string& Diagnostic() const noexcept { return diagnostic_; }

		// The static publisher is intentionally exposed for focused concurrency
		// tests. Callers provide exact revision bytes; the implementation holds one
		// protected publication boundary across the definitive compare and replace.
		static RetargetAuthoringPublishResult Publish(
			const std::filesystem::path& destination,
			const AnimationAuthoringRevision& expectedCurrent,
			const AnimationAuthoringRevision& replacement,
			const TerraEngine::Animation::SkeletonAsset& sourceSkeleton,
			const TerraEngine::Animation::SkeletonAsset& targetSkeleton,
			const RetargetAuthoringGenerationSnapshot& expected,
			const SnapshotProvider& currentSnapshot);

	private:
		[[nodiscard]] bool PublishRevision(
			const AnimationAuthoringRevision& expectedCurrent,
			const AnimationAuthoringRevision& replacement,
			const SnapshotProvider& currentSnapshot,
			std::string& outDiagnostic) noexcept;
		AnimationAuthoringStatus AdoptPublishedRevision(
			AnimationAuthoringRevision revision) noexcept;
		[[nodiscard]] bool CurrentRevisionMatches(
			const SnapshotProvider& currentSnapshot,
			AnimationAuthoringRevision& outCurrent) const noexcept;

		std::filesystem::path destination_;
		RetargetEditorModel model_;
		AnimationAuthoringRevision published_;
		RetargetAuthoringGenerationSnapshot generation_{};
		AnimationAuthoringTransaction transaction_;
		AnimationAuthoringHistory history_;
		std::string diagnostic_;
		bool editing_ = false;
	};
}

class RetargetEditorWindow final : public EditorWindow {
public:
	explicit RetargetEditorWindow(MainEditorSubsystem* owner = nullptr);
	~RetargetEditorWindow() override;
	void SetErrors(std::vector<float> errors);

protected:
	void OnGui() override;

private:
	struct State;
	MainEditorSubsystem* owner_ = nullptr;
	std::unique_ptr<State> state_;
};
