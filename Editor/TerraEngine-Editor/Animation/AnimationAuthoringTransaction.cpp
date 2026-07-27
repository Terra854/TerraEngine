// AnimationAuthoringTransaction.cpp - generation-safe publication and history.

#include "AnimationAuthoringTransaction.h"

#include <utility>

namespace TerraEngine::Editor::Animation {
	namespace {
		bool SameAsset(const AnimationAuthoringRevision& left, const AnimationAuthoringRevision& right) noexcept {
			return left.assetId.IsValid() && left.assetId == right.assetId;
		}
	}

	AnimationAuthoringTransaction::AnimationAuthoringTransaction(std::string label)
		: label_(std::move(label)) {}

	AnimationAuthoringStatus AnimationAuthoringTransaction::Begin(
		const AnimationAuthoringRevision& before,
		TerraEngine::Animation::AnimationGuid expectedAssetId,
		std::uint64_t expectedGeneration) noexcept {
		if(!before.assetId.IsValid() || before.assetId != expectedAssetId
			|| expectedGeneration == 0U || before.generation != expectedGeneration) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		if(cancellationRequested_) {
			cancellationRequested_->store(true, std::memory_order_release);
		}
		before_ = before;
		candidate_ = before;
		expectedAssetId_ = expectedAssetId;
		expectedGeneration_ = expectedGeneration;
		activeRequestId_ = 0U;
		cancellationRequested_.reset();
		active_ = true;
		canceled_ = false;
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringTransaction::Replace(std::string candidateBytes) noexcept {
		if(!active_) return AnimationAuthoringStatus::Rejected;
		if(candidateBytes.empty()) return AnimationAuthoringStatus::InvalidArgument;
		if(candidateBytes.size() > kMaxAuthoredBytes) return AnimationAuthoringStatus::CapacityExceeded;
		candidate_.bytes = std::move(candidateBytes);
		if(cancellationRequested_) {
			cancellationRequested_->store(true, std::memory_order_release);
		}
		activeRequestId_ = 0U;
		cancellationRequested_.reset();
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringTransaction::PrepareCompile(
		AnimationAuthoringCompileRequest& outRequest) noexcept {
		if(!active_) return canceled_ ? AnimationAuthoringStatus::Canceled : AnimationAuthoringStatus::Rejected;
		if(candidate_.bytes.empty()) return AnimationAuthoringStatus::InvalidArgument;
		if(candidate_.bytes.size() > kMaxAuthoredBytes) return AnimationAuthoringStatus::CapacityExceeded;
		if(cancellationRequested_) {
			cancellationRequested_->store(true, std::memory_order_release);
		}
		cancellationRequested_ = std::make_shared<std::atomic_bool>(false);
		activeRequestId_ = nextRequestId_++;
		outRequest = {expectedAssetId_, expectedGeneration_, activeRequestId_,
			candidate_.bytes, cancellationRequested_};
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringTransaction::CompleteCompile(
		const AnimationAuthoringCompileResult& result,
		const AnimationAuthoringRevision& current,
		const AnimationAuthoringPublisher& publisher,
		AnimationAuthoringRevision& outPublished,
		std::string& outDiagnostic) noexcept {
		if(!active_) return canceled_ ? AnimationAuthoringStatus::Canceled : AnimationAuthoringStatus::Rejected;
		if(result.assetId != expectedAssetId_ || result.expectedGeneration != expectedGeneration_
			|| result.requestId == 0U || result.requestId != activeRequestId_
			|| !SameAsset(current, before_) || current.generation != expectedGeneration_
			|| current.bytes != before_.bytes) {
			return AnimationAuthoringStatus::Stale;
		}
		if(result.canceled || (cancellationRequested_
			&& cancellationRequested_->load(std::memory_order_acquire))) {
			return AnimationAuthoringStatus::Canceled;
		}
		if(!result.succeeded || result.validatedBytes != candidate_.bytes) {
			outDiagnostic = result.diagnostic;
			return AnimationAuthoringStatus::ValidationFailed;
		}
		if(!publisher) return AnimationAuthoringStatus::InvalidArgument;

		AnimationAuthoringRevision replacement = candidate_;
		replacement.generation = current.generation + 1U;
		if(!publisher(current, replacement, outDiagnostic)) {
			return AnimationAuthoringStatus::PublicationFailed;
		}
		outPublished = std::move(replacement);
		active_ = false;
		activeRequestId_ = 0U;
		cancellationRequested_.reset();
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringTransaction::Cancel() noexcept {
		if(!active_) return AnimationAuthoringStatus::Rejected;
		if(cancellationRequested_) {
			cancellationRequested_->store(true, std::memory_order_release);
		}
		activeRequestId_ = 0U;
		canceled_ = true;
		active_ = false;
		return AnimationAuthoringStatus::Canceled;
	}

	AnimationAuthoringStatus AnimationAuthoringHistory::Commit(
		const AnimationAuthoringRevision& before,
		const AnimationAuthoringRevision& after) noexcept {
		if(!SameAsset(before, after) || before.generation == 0U
			|| after.generation != before.generation + 1U || before.bytes == after.bytes) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		undo_.push_back({before, after});
		redo_.clear();
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringHistory::PublishHistory(
		const AnimationAuthoringRevision& current,
		const AnimationAuthoringRevision& desiredBytes,
		const AnimationAuthoringPublisher& publisher,
		AnimationAuthoringRevision& outPublished,
		std::string& outDiagnostic) noexcept {
		if(!SameAsset(current, desiredBytes) || current.generation == 0U || !publisher) {
			return AnimationAuthoringStatus::InvalidArgument;
		}
		AnimationAuthoringRevision replacement = desiredBytes;
		replacement.generation = current.generation + 1U;
		if(!publisher(current, replacement, outDiagnostic)) {
			return AnimationAuthoringStatus::PublicationFailed;
		}
		outPublished = std::move(replacement);
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringHistory::Undo(
		const AnimationAuthoringRevision& current,
		const AnimationAuthoringPublisher& publisher,
		AnimationAuthoringRevision& outPublished,
		std::string& outDiagnostic) noexcept {
		if(undo_.empty()) return AnimationAuthoringStatus::Rejected;
		const Entry& entry = undo_.back();
		if(!SameAsset(current, entry.after) || current.bytes != entry.after.bytes) {
			return AnimationAuthoringStatus::Stale;
		}
		const AnimationAuthoringStatus status = PublishHistory(
			current, entry.before, publisher, outPublished, outDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) return status;
		redo_.push_back(entry);
		undo_.pop_back();
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus AnimationAuthoringHistory::Redo(
		const AnimationAuthoringRevision& current,
		const AnimationAuthoringPublisher& publisher,
		AnimationAuthoringRevision& outPublished,
		std::string& outDiagnostic) noexcept {
		if(redo_.empty()) return AnimationAuthoringStatus::Rejected;
		const Entry& entry = redo_.back();
		if(!SameAsset(current, entry.before) || current.bytes != entry.before.bytes) {
			return AnimationAuthoringStatus::Stale;
		}
		const AnimationAuthoringStatus status = PublishHistory(
			current, entry.after, publisher, outPublished, outDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) return status;
		undo_.push_back(entry);
		redo_.pop_back();
		return AnimationAuthoringStatus::Ok;
	}

	void AnimationAuthoringHistory::Clear() noexcept {
		undo_.clear();
		redo_.clear();
	}
}
