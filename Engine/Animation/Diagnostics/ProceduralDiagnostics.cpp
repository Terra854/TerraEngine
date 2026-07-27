// ProceduralDiagnostics.cpp - Atomic bounded diagnostic publication.

#include "Animation/Diagnostics/ProceduralDiagnostics.h"

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		bool SupportedSolver(ProceduralSolverKind solver) noexcept {
			return static_cast<std::uint8_t>(solver)
				<= static_cast<std::uint8_t>(ProceduralSolverKind::PelvisCompensation);
		}

		bool SupportedState(ProceduralSolverState state) noexcept {
			return static_cast<std::uint8_t>(state)
				<= static_cast<std::uint8_t>(ProceduralSolverState::InvalidInput);
		}

		bool SupportedStatus(AnimationStatus status) noexcept {
			return static_cast<std::uint16_t>(status)
				<= static_cast<std::uint16_t>(AnimationStatus::ContentHashMismatch);
		}
	}

	bool ValidateProceduralDiagnosticRecord(
		const ProceduralDiagnosticRecord& record) noexcept {
		return record.requestedGeneration.IsValid()
			&& record.observedGeneration.IsValid()
			&& SupportedSolver(record.solver)
			&& SupportedState(record.state)
			&& SupportedStatus(record.status)
			&& IsFinite(record)
			&& record.targetErrorMeters >= 0.0F
			&& record.targetErrorRadians >= 0.0F
			&& record.inputWeight >= 0.0F && record.inputWeight <= 1.0F
			&& record.finalContribution >= 0.0F && record.finalContribution <= 1.0F
			&& (record.iterationLimit == 0U
				? record.iterationCount == 0U
				: record.iterationCount <= record.iterationLimit);
	}

	AnimationStatus ProceduralDiagnosticsBuffer::Configure(std::size_t capacity) noexcept {
		if(capacity > kProceduralDiagnosticRecordHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}

		try {
			std::vector<ProceduralDiagnosticRecord> published;
			std::vector<ProceduralDiagnosticRecord> staging;
			published.reserve(capacity);
			staging.reserve(capacity);
			std::scoped_lock lock(mutex_);
			records_.swap(published);
			staging_.swap(staging);
			generation_ = {};
			counters_ = {};
			counters_.capacity = capacity;
			configured_ = true;
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus ProceduralDiagnosticsBuffer::Publish(
		const ProceduralGenerationStamp& generation,
		std::span<const ProceduralDiagnosticRecord> records) noexcept {
		if(!generation.IsValid()) {
			return AnimationStatus::InvalidArgument;
		}
		if(records.size() > kProceduralDiagnosticRecordHardMaximum) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(const ProceduralDiagnosticRecord& record : records) {
			if(!ValidateProceduralDiagnosticRecord(record)
				|| record.requestedGeneration != generation) {
				std::scoped_lock lock(mutex_);
				if(configured_) {
					++counters_.rejectedBatches;
				}
				return AnimationStatus::InvalidArgument;
			}
		}

		static_assert(std::is_nothrow_copy_constructible_v<ProceduralDiagnosticRecord>);
		std::scoped_lock lock(mutex_);
		if(!configured_) {
			return AnimationStatus::NotReady;
		}
		const std::size_t capacity = counters_.capacity;
		if(capacity == 0U && !records.empty()) {
			++counters_.rejectedBatches;
			return AnimationStatus::Rejected;
		}

		staging_.clear();
		const std::size_t incomingBegin = records.size() > capacity
			? records.size() - capacity : 0U;
		const std::size_t incomingCount = records.size() - incomingBegin;
		const std::size_t existingKeep = incomingCount >= capacity
			? 0U : std::min(records_.size(), capacity - incomingCount);
		const std::size_t existingBegin = records_.size() - existingKeep;
		for(std::size_t index = existingBegin; index < records_.size(); ++index) {
			staging_.push_back(records_[index]);
		}
		for(std::size_t index = incomingBegin; index < records.size(); ++index) {
			staging_.push_back(records[index]);
		}

		const std::size_t oldAndIncoming = records_.size() + records.size();
		const std::size_t evicted = oldAndIncoming > staging_.size()
			? oldAndIncoming - staging_.size() : 0U;
		records_.swap(staging_);
		generation_ = generation;
		++counters_.publishedBatches;
		counters_.publishedRecords += records.size();
		counters_.evictedRecords += evicted;
		counters_.size = records_.size();
		counters_.highWaterRecords = std::max(
			counters_.highWaterRecords, records_.size());
		return AnimationStatus::Ok;
	}

	std::optional<ProceduralDiagnosticsSnapshot>
		ProceduralDiagnosticsBuffer::AcquireCopy() const noexcept {
		try {
			std::scoped_lock lock(mutex_);
			if(!configured_) {
				return std::nullopt;
			}
			ProceduralDiagnosticsSnapshot snapshot;
			snapshot.generation = generation_;
			snapshot.counters = counters_;
			snapshot.records = records_;
			return snapshot;
		} catch(const std::bad_alloc&) {
			return std::nullopt;
		} catch(...) {
			return std::nullopt;
		}
	}

	void ProceduralDiagnosticsBuffer::Clear() noexcept {
		std::scoped_lock lock(mutex_);
		records_.clear();
		staging_.clear();
		generation_ = {};
		counters_.size = 0U;
	}
}
