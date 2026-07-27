// AnimationAssetActions.h - discoverable, reversible animation asset lifecycle actions.
//
// The Project Browser owns one instance for its open project. The service keeps
// editor metadata private, writes only staged files below the supplied asset
// root, and leaves AssetDatabase registration/import ownership unchanged.

#pragma once

#include "../Animation/AnimationAuthoringTransaction.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Assets {
	enum class AnimationAssetKind : std::uint8_t {
		Skeleton,
		Clip,
		Controller,
		Mask,
		RetargetProfile,
		MotionDatabase,
		RagdollProfile
	};

	enum class AnimationAssetValidationState : std::uint8_t {
		NotValidated,
		Pending,
		Valid,
		Invalid,
		Canceled
	};

	struct AnimationAssetEntry {
		std::uint64_t id = 0U;
		AnimationAssetKind kind{};
		std::string name;
		std::string folder;
		std::filesystem::path sourcePath;
		std::uint64_t generation = 1U;
		AnimationAssetValidationState validation = AnimationAssetValidationState::NotValidated;
		std::string diagnostic;
		bool valid = false;
	};

	// A worker receives this immutable request and returns its result through
	// CompleteValidation at the editor boundary. The request has no pointer back
	// into this catalog, so cancellation/stale completion is always safe.
	struct AnimationAssetValidationRequest {
		std::uint64_t assetId = 0U;
		std::uint64_t expectedGeneration = 0U;
		std::uint64_t requestId = 0U;
		std::filesystem::path sourcePath;
		AnimationAssetKind kind{};
	};

	struct AnimationAssetValidationResult {
		std::uint64_t assetId = 0U;
		std::uint64_t expectedGeneration = 0U;
		std::uint64_t requestId = 0U;
		bool succeeded = false;
		std::string diagnostic;
	};

	// The browser/menus call this typed catalog. Passing an asset root enables
	// durable staged files; omitting it supports detached preview/test documents.
	class AnimationAssetActions final {
	public:
		explicit AnimationAssetActions(std::filesystem::path assetRoot = {});

		Animation::AnimationAuthoringStatus Create(AnimationAssetKind kind, std::string name,
			std::string folder, AnimationAssetEntry& out) noexcept;
		Animation::AnimationAuthoringStatus Import(const std::filesystem::path& source,
			std::string destinationFolder, AnimationAssetEntry& out) noexcept;
		// Generated model-package children are deliberately read-only because a
		// package reimport owns their bytes and identities. This action creates a
		// separate loose Skeleton or Clip document below this catalog's Assets root.
		// It accepts only a checked file contained by generatedRoot, decodes it with
		// the production codec, assigns a new durable asset GUID, then publishes the
		// reserialized result atomically. The source package is never modified.
		Animation::AnimationAuthoringStatus ExtractReadOnlyPackageChild(
			AnimationAssetKind kind, const std::filesystem::path& generatedSource,
			const std::filesystem::path& generatedRoot, std::string name,
			std::string destinationFolder, AnimationAssetEntry& out) noexcept;
		// AdoptExisting makes watcher-discovered project files available to the same
		// lifecycle/validation model without copying or rewriting their bytes.
		Animation::AnimationAuthoringStatus AdoptExisting(
			const std::filesystem::path& source,
			AnimationAssetEntry& out) noexcept;
		Animation::AnimationAuthoringStatus Duplicate(std::uint64_t sourceId, std::string name,
			AnimationAssetEntry& out) noexcept;
		Animation::AnimationAuthoringStatus Rename(std::uint64_t id, std::string name) noexcept;
		Animation::AnimationAuthoringStatus Move(std::uint64_t id, std::string folder) noexcept;
		Animation::AnimationAuthoringStatus Delete(std::uint64_t id) noexcept;
		[[nodiscard]] const AnimationAssetEntry* Inspect(std::uint64_t id) const noexcept;
		[[nodiscard]] const AnimationAssetEntry* InspectByPath(
			const std::filesystem::path& source) const noexcept;
		// EnsureTracked adopts a watcher-discovered animation document only once.
		// Browser context actions can therefore share stable lifecycle history.
		Animation::AnimationAuthoringStatus EnsureTracked(
			const std::filesystem::path& source,
			AnimationAssetEntry& out) noexcept;

		Animation::AnimationAuthoringStatus BeginValidation(std::uint64_t id,
			AnimationAssetValidationRequest& outRequest) noexcept;
		[[nodiscard]] static AnimationAssetValidationResult ValidateCandidate(
			const AnimationAssetValidationRequest& request) noexcept;
		Animation::AnimationAuthoringStatus CompleteValidation(
			const AnimationAssetValidationResult& result) noexcept;
		Animation::AnimationAuthoringStatus CancelValidation(
			const AnimationAssetValidationRequest& request) noexcept;

		// Lifecycle mutations share inverse entries, so a menu action is restored as
		// one logical edit instead of partially replaying move/name/file changes.
		Animation::AnimationAuthoringStatus Undo() noexcept;
		Animation::AnimationAuthoringStatus Redo() noexcept;
		[[nodiscard]] std::size_t UndoDepth() const noexcept { return undo_.size(); }
		[[nodiscard]] std::size_t RedoDepth() const noexcept { return redo_.size(); }
		[[nodiscard]] static std::vector<AnimationAssetKind> SupportedKinds();
		[[nodiscard]] static const char* DisplayName(AnimationAssetKind kind) noexcept;
		[[nodiscard]] static const char* Extension(AnimationAssetKind kind) noexcept;
		[[nodiscard]] static std::optional<AnimationAssetKind> KindForPath(
			const std::filesystem::path& path) noexcept;
		[[nodiscard]] const std::filesystem::path& AssetRoot() const noexcept { return assetRoot_; }
		[[nodiscard]] const std::string& LastDiagnostic() const noexcept { return lastDiagnostic_; }
		// Project Browser supplies the active project's Assets root before its first
		// lifecycle action. Changing roots with retained assets is rejected by design.
		Animation::AnimationAuthoringStatus SetAssetRoot(std::filesystem::path assetRoot) noexcept;

	private:
		struct HistoryEntry {
			std::optional<AnimationAssetEntry> before;
			std::optional<AnimationAssetEntry> after;
			// Delete stores the bounded source snapshot so Undo never recreates a
			// placeholder in place of an artist's last-good document.
			std::string deletedSourceBytes;
		};

		[[nodiscard]] bool IsValidName(const std::string& value) const noexcept;
		[[nodiscard]] bool IsSafeFolder(const std::string& value) const noexcept;
		[[nodiscard]] std::filesystem::path BuildSourcePath(const AnimationAssetEntry& entry) const;
		[[nodiscard]] std::filesystem::path MakeUniqueSourcePath(const AnimationAssetEntry& entry) const;
		[[nodiscard]] Animation::AnimationAuthoringStatus WriteCreatedDocument(AnimationAssetEntry& entry) noexcept;
		[[nodiscard]] Animation::AnimationAuthoringStatus CopyDocument(
			const AnimationAssetEntry& source, AnimationAssetEntry& destination) noexcept;
		[[nodiscard]] Animation::AnimationAuthoringStatus MoveDocument(
			const AnimationAssetEntry& before, AnimationAssetEntry& after) noexcept;
		void CommitInverse(std::optional<AnimationAssetEntry> before,
			std::optional<AnimationAssetEntry> after) noexcept;
		Animation::AnimationAuthoringStatus ApplyHistory(const HistoryEntry& entry,
			bool applyAfter) noexcept;

		std::filesystem::path assetRoot_;
		std::map<std::uint64_t, AnimationAssetEntry> assets_;
		std::vector<HistoryEntry> undo_;
		std::vector<HistoryEntry> redo_;
		std::uint64_t nextId_ = 1U;
		std::uint64_t nextValidationRequestId_ = 1U;
		std::map<std::uint64_t, std::uint64_t> validationRequests_;
		std::string lastDiagnostic_;
	};
}
