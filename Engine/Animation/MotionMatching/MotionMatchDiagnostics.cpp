// MotionMatchDiagnostics.cpp - Atomic copied diagnostics retention.

#include "Animation/MotionMatching/MotionMatchDiagnostics.h"

#include <algorithm>
#include <new>

namespace TerraEngine::Animation {
	MotionMatchDiagnosticRecord MakeMotionMatchDiagnosticRecord(
		const MotionMatchGpuGeneration& generation, AnimationFixedTick tick,
		const MotionMatchSelection& selection) noexcept {
		MotionMatchDiagnosticRecord record;
		record.generation = generation;
		record.tick = tick;
		record.status = selection.status;
		record.usedFallback = selection.usedCpuFallback
			|| selection.authoritativeScores.usedFallback;
		record.usedGpuCandidates = selection.usedGpuCandidates;
		record.usedCpuFallback = selection.usedCpuFallback;
		record.candidateBudgetExhausted =
			selection.authoritativeScores.candidateBudgetExhausted;
		record.selectionSource = selection.source;
		record.fallbackReason = selection.fallbackReason;
		record.gpuStatus = selection.gpuStatus;
		record.databaseGeneration = generation.database.generation;
		record.visitedSamples = selection.authoritativeScores.visitedSamples;
		record.evaluatedSamples = selection.authoritativeScores.evaluatedSamples;
		record.excludedSamples = selection.authoritativeScores.excludedSamples;
		record.exclusionReasonCounts = selection.authoritativeScores.exclusionReasonCounts;
		record.retainedExclusionDetails = static_cast<std::uint32_t>(std::min<std::size_t>(
			selection.authoritativeScores.exclusionDetails.size(),
			kMotionMatchDiagnosticExclusionDetailLimit));
		for(std::size_t index = 0U; index < record.retainedExclusionDetails; ++index) {
			record.exclusionDetails[index] =
				selection.authoritativeScores.exclusionDetails[index];
		}
		record.chosen = selection.chosen;
		return record;
	}

	AnimationStatus MotionMatchDiagnostics::Configure(std::size_t capacity) noexcept {
		if(capacity > GetAnimationRuntimeLimit(
			AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			std::vector<MotionMatchDiagnosticRecord> staged;
			staged.reserve(capacity);
			std::scoped_lock lock(mutex_);
			records_.swap(staged);
			capacity_ = capacity;
			nextWriteIndex_ = 0U;
			evicted_ = 0U;
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus MotionMatchDiagnostics::Publish(
		const MotionMatchDiagnosticRecord& record) noexcept {
		if(!record.generation.IsValid()
			|| (record.databaseGeneration != 0U
				&& record.databaseGeneration != record.generation.database.generation)
			|| record.retainedExclusionDetails > kMotionMatchDiagnosticExclusionDetailLimit) {
			return AnimationStatus::InvalidArgument;
		}
		std::scoped_lock lock(mutex_);
		if(capacity_ == 0U) return AnimationStatus::Rejected;
		try {
			if(records_.size() < capacity_) {
				records_.push_back(record);
				return AnimationStatus::Ok;
			}
			// Overwrite the oldest slot in O(1); shifting a 36,000-entry history on
			// every publish would turn optional diagnostics into a hidden hot path.
			records_[nextWriteIndex_] = record;
			nextWriteIndex_ = (nextWriteIndex_ + 1U) % capacity_;
			++evicted_;
			return AnimationStatus::Ok;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	std::optional<MotionMatchDiagnosticsSnapshot>
		MotionMatchDiagnostics::AcquireCopy() const noexcept {
		try {
			std::scoped_lock lock(mutex_);
			MotionMatchDiagnosticsSnapshot snapshot;
			snapshot.evictedRecords = evicted_;
			snapshot.records.reserve(records_.size());
			if(records_.size() < capacity_ || records_.empty()) {
				snapshot.records = records_;
			} else {
				// The circular buffer is copied into oldest-to-newest order so consumers
				// never need access to mutable ring indices.
				for(std::size_t index = 0U; index < records_.size(); ++index) {
					snapshot.records.push_back(
						records_[(nextWriteIndex_ + index) % records_.size()]);
				}
			}
			return snapshot;
		} catch(...) {
			return std::nullopt;
		}
	}
}
