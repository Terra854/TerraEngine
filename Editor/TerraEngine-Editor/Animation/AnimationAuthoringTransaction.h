// AnimationAuthoringTransaction.h - reusable fail-closed authoring publication.
//
// A transaction owns a private byte candidate. Validation jobs receive copied
// packets and the editor boundary publishes only the exact validated packet.

#pragma once

#include "Engine/Animation/AnimationTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Animation {
	enum class AnimationAuthoringStatus : std::uint8_t {
		Ok,
		InvalidArgument,
		Rejected,
		Canceled,
		Stale,
		CapacityExceeded,
		ValidationFailed,
		PublicationFailed
	};

	struct AnimationAuthoringRevision {
		TerraEngine::Animation::AnimationGuid assetId{};
		std::uint64_t generation = 0U;
		std::string bytes;

		auto operator<=>(const AnimationAuthoringRevision&) const = default;
	};

	struct AnimationAuthoringCompileRequest {
		TerraEngine::Animation::AnimationGuid assetId{};
		std::uint64_t expectedGeneration = 0U;
		std::uint64_t requestId = 0U;
		std::string candidateBytes;
		// The copied token lets a worker stop without retaining the transaction.
		std::shared_ptr<std::atomic_bool> cancellationRequested;
	};

	struct AnimationAuthoringCompileResult {
		TerraEngine::Animation::AnimationGuid assetId{};
		std::uint64_t expectedGeneration = 0U;
		std::uint64_t requestId = 0U;
		std::string validatedBytes;
		bool succeeded = false;
		bool canceled = false;
		std::string diagnostic;
	};

	// A publisher must either make the complete revision durable or leave the
	// prior revision unchanged. Returning false tells the transaction not to move
	// its generation or history cursor.
	using AnimationAuthoringPublisher = std::function<bool(
		const AnimationAuthoringRevision& expectedCurrent,
		const AnimationAuthoringRevision& replacement,
		std::string& diagnostic)>;

	class AnimationAuthoringTransaction final {
	public:
		explicit AnimationAuthoringTransaction(std::string label = "Animation edit");
		AnimationAuthoringStatus Begin(
			const AnimationAuthoringRevision& before,
			TerraEngine::Animation::AnimationGuid expectedAssetId,
			std::uint64_t expectedGeneration) noexcept;
		AnimationAuthoringStatus Replace(std::string candidateBytes) noexcept;
		AnimationAuthoringStatus PrepareCompile(AnimationAuthoringCompileRequest& outRequest) noexcept;
		AnimationAuthoringStatus CompleteCompile(
			const AnimationAuthoringCompileResult& result,
			const AnimationAuthoringRevision& current,
			const AnimationAuthoringPublisher& publisher,
			AnimationAuthoringRevision& outPublished,
			std::string& outDiagnostic) noexcept;
		AnimationAuthoringStatus Cancel() noexcept;
		[[nodiscard]] bool IsActive() const noexcept { return active_; }
		[[nodiscard]] bool IsCanceled() const noexcept { return canceled_; }
		[[nodiscard]] const std::string& Label() const noexcept { return label_; }
		[[nodiscard]] const AnimationAuthoringRevision& Before() const noexcept { return before_; }
		[[nodiscard]] const AnimationAuthoringRevision& Candidate() const noexcept { return candidate_; }

	private:
		static constexpr std::size_t kMaxAuthoredBytes = 64U * 1024U * 1024U;
		std::string label_;
		AnimationAuthoringRevision before_;
		AnimationAuthoringRevision candidate_;
		TerraEngine::Animation::AnimationGuid expectedAssetId_{};
		std::uint64_t expectedGeneration_ = 0U;
		std::uint64_t nextRequestId_ = 1U;
		std::uint64_t activeRequestId_ = 0U;
		std::shared_ptr<std::atomic_bool> cancellationRequested_;
		bool active_ = false;
		bool canceled_ = false;
	};

	class AnimationAuthoringHistory final {
	public:
		AnimationAuthoringStatus Commit(
			const AnimationAuthoringRevision& before,
			const AnimationAuthoringRevision& after) noexcept;
		AnimationAuthoringStatus Undo(
			const AnimationAuthoringRevision& current,
			const AnimationAuthoringPublisher& publisher,
			AnimationAuthoringRevision& outPublished,
			std::string& outDiagnostic) noexcept;
		AnimationAuthoringStatus Redo(
			const AnimationAuthoringRevision& current,
			const AnimationAuthoringPublisher& publisher,
			AnimationAuthoringRevision& outPublished,
			std::string& outDiagnostic) noexcept;
		void Clear() noexcept;
		[[nodiscard]] std::size_t UndoDepth() const noexcept { return undo_.size(); }
		[[nodiscard]] std::size_t RedoDepth() const noexcept { return redo_.size(); }

	private:
		struct Entry {
			AnimationAuthoringRevision before;
			AnimationAuthoringRevision after;
		};
		AnimationAuthoringStatus PublishHistory(
			const AnimationAuthoringRevision& current,
			const AnimationAuthoringRevision& desiredBytes,
			const AnimationAuthoringPublisher& publisher,
			AnimationAuthoringRevision& outPublished,
			std::string& outDiagnostic) noexcept;
		std::vector<Entry> undo_;
		std::vector<Entry> redo_;
	};
}
