// MotionMatchGpu.cpp - Bounded asynchronous backend state with no frame waits.

#include "Animation/MotionMatching/MotionMatchGpu.h"

#include <algorithm>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint32_t DivideRoundUp(std::uint32_t value,
			std::uint32_t divisor) noexcept {
			return value == 0U ? 0U : 1U + (value - 1U) / divisor;
		}

		std::size_t EffectiveBlockCount(const MotionDatabaseAsset& database) noexcept {
			if(!database.residentBlocks.empty()) return database.residentBlocks.size();
			// Submit's count overload consults this helper before dispatch planning.
			// Guard malformed legacy layouts here so invalid input is rejected instead
			// of reaching an integer division by zero.
			return database.samples.empty() || database.storageLayout.samplesPerBlock == 0U ? 0U
				: (database.samples.size() + database.storageLayout.samplesPerBlock - 1U)
					/ database.storageLayout.samplesPerBlock;
		}

		MotionDatabaseResidentBlock EffectiveBlock(const MotionDatabaseAsset& database,
			std::uint32_t blockIndex) noexcept {
			if(!database.residentBlocks.empty()) return database.residentBlocks[blockIndex];
			MotionDatabaseResidentBlock block;
			block.blockIndex = blockIndex;
			block.firstSample = blockIndex * database.storageLayout.samplesPerBlock;
			block.sampleCount = std::min<std::uint32_t>(database.storageLayout.samplesPerBlock,
				static_cast<std::uint32_t>(database.samples.size()) - block.firstSample);
			block.cpuByteOffset = static_cast<std::uint64_t>(block.firstSample)
				* database.storageLayout.cpuSampleStrideBytes;
			block.cpuByteSize = static_cast<std::uint64_t>(block.sampleCount)
				* database.storageLayout.cpuSampleStrideBytes;
			block.gpuByteOffset = static_cast<std::uint64_t>(block.firstSample)
				* database.storageLayout.gpuSampleStrideBytes;
			block.gpuByteSize = static_cast<std::uint64_t>(block.sampleCount)
				* database.storageLayout.gpuSampleStrideBytes;
			return block;
		}

		AnimationStatus ValidateGpuQuery(const MotionDatabaseAsset& database,
			const MotionMatchQuery& query) noexcept {
			if(!query.database.IsValid() || query.database.assetId != database.assetId
				|| query.database.generation != database.runtimeGeneration
				|| query.expectedSchemaFingerprint != database.featureSchemaFingerprint) {
				return AnimationStatus::StaleHandle;
			}
			if(query.maximumCandidates == 0U || query.maximumSamplesToVisit == 0U
				|| query.reserved != 0U
				|| (query.requiredTags & query.excludedTags) != 0U
				|| (query.requiredTransitionFlags & query.excludedTransitionFlags) != 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(query.maximumCandidates > kMotionDatabaseMaximumTopK
				|| query.maximumSamplesToVisit > kMotionDatabaseMaximumSamples
				|| query.excludedSampleIds.size() > kMotionDatabaseMaximumTopK
				|| !std::is_sorted(query.excludedSampleIds.begin(), query.excludedSampleIds.end())) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		MotionMatchGpuQueryPacket CopyQuery(const MotionMatchQuery& query) noexcept {
			MotionMatchGpuQueryPacket packet;
			for(std::size_t feature = 0U; feature < kMotionDatabaseFeatureCount; ++feature) {
				packet.features[feature] = query.features.values[feature];
				packet.featureWeights[feature] = query.featureWeights[feature];
			}
			packet.excludedSampleCount = static_cast<std::uint32_t>(query.excludedSampleIds.size());
			std::copy(query.excludedSampleIds.begin(), query.excludedSampleIds.end(),
				packet.excludedSampleIds.begin());
			packet.maximumPhaseDistance = query.maximumPhaseDistance;
			packet.requiredTags = query.requiredTags;
			packet.excludedTags = query.excludedTags;
			packet.preferredTags = query.preferredTags;
			packet.lastSampleId = query.lastSampleId;
			packet.lastSourceClipId = query.lastSourceClipId;
			packet.requiredContacts = query.requiredContacts;
			packet.preferredContacts = query.preferredContacts;
			packet.preferredContinuityGroup = query.preferredContinuityGroup;
			packet.excludedRegionFlags = query.excludedRegionFlags;
			packet.preferredPhase = query.preferredPhase;
			packet.requiredTransitionFlags = query.requiredTransitionFlags;
			packet.excludedTransitionFlags = query.excludedTransitionFlags;
			packet.preferredTransitionFlags = query.preferredTransitionFlags;
			packet.phaseWeight = query.phaseWeight;
			packet.continuityPenalty = query.continuityPenalty;
			packet.continuityBreakPenalty = query.continuityBreakPenalty;
			packet.sameClipPenalty = query.sameClipPenalty;
			packet.missingPreferredTagPenalty = query.missingPreferredTagPenalty;
			packet.contactMismatchPenalty = query.contactMismatchPenalty;
			packet.transitionMismatchPenalty = query.transitionMismatchPenalty;
			return packet;
		}

		bool ValidRecordCost(const MotionMatchGpuCandidateRecord& record) noexcept {
			std::uint64_t featureCost = 0U;
			for(const std::uint64_t blockCost : record.featureBlockCosts) featureCost += blockCost;
			const std::uint64_t totalCost = record.featureCost + record.phaseCost + record.tagCost
				+ record.contactCost + record.continuityCost + record.transitionCost;
			return record.stableSampleId != 0U && record.sourceClipId != 0U
				&& record.reserved == 0U && record.exclusionReasons == 0U
				&& featureCost == record.featureCost && totalCost == record.totalCost
				&& record.totalCost != std::numeric_limits<std::uint64_t>::max();
		}

		MotionMatchCandidate CandidateFromRecord(
			const MotionMatchGpuCandidateRecord& record) noexcept {
			MotionMatchCandidate candidate;
			candidate.stableSampleId = record.stableSampleId;
			candidate.sourceClipId = record.sourceClipId;
			candidate.sourceTick = record.sourceTick;
			candidate.score.featureBlockCosts = record.featureBlockCosts;
			candidate.score.featureCost = record.featureCost;
			candidate.score.phaseCost = record.phaseCost;
			candidate.score.tagCost = record.tagCost;
			candidate.score.contactCost = record.contactCost;
			candidate.score.continuityCost = record.continuityCost;
			candidate.score.transitionCost = record.transitionCost;
			candidate.score.totalCost = record.totalCost;
			return candidate;
		}
	}

	MotionMatchGpuCandidateRecord MakeMotionMatchGpuCandidateRecord(
		const MotionMatchCandidate& candidate) noexcept {
		MotionMatchGpuCandidateRecord record;
		record.stableSampleId = candidate.stableSampleId;
		record.sourceClipId = candidate.sourceClipId;
		record.sourceTick = candidate.sourceTick;
		record.featureBlockCosts = candidate.score.featureBlockCosts;
		record.featureCost = candidate.score.featureCost;
		record.phaseCost = candidate.score.phaseCost;
		record.tagCost = candidate.score.tagCost;
		record.contactCost = candidate.score.contactCost;
		record.continuityCost = candidate.score.continuityCost;
		record.transitionCost = candidate.score.transitionCost;
		record.totalCost = candidate.score.totalCost;
		return record;
	}

	AnimationStatus PlanMotionMatchGpuDispatch(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, std::span<const std::uint32_t> residentBlockIndices,
		const MotionMatchGpuGeneration& generation,
		MotionMatchGpuDispatchPlan& output) noexcept {
		if(ValidateMotionDatabaseAsset(database) != AnimationStatus::Ok
			|| !generation.IsValid() || generation.database != query.database
			|| generation.schemaFingerprint != database.featureSchemaFingerprint) {
			return AnimationStatus::InvalidArgument;
		}
		if(const AnimationStatus queryStatus = ValidateGpuQuery(database, query);
			queryStatus != AnimationStatus::Ok) {
			return queryStatus;
		}
		const std::size_t availableBlocks = EffectiveBlockCount(database);
		if(residentBlockIndices.empty() || residentBlockIndices.size() > availableBlocks
			|| residentBlockIndices.size() > kMotionMatchGpuMaximumResidentBlocks
			|| !std::is_sorted(residentBlockIndices.begin(), residentBlockIndices.end())
			|| std::adjacent_find(residentBlockIndices.begin(), residentBlockIndices.end())
				!= residentBlockIndices.end()
			|| residentBlockIndices.back() >= availableBlocks) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			MotionMatchGpuDispatchPlan staged;
			staged.generation = generation;
			staged.topK = static_cast<std::uint32_t>(query.maximumCandidates);
			staged.readbackStrideBytes = sizeof(MotionMatchGpuCandidateRecord);
			staged.readbackBytes = static_cast<std::uint32_t>(
				query.maximumCandidates * sizeof(MotionMatchGpuCandidateRecord));
			staged.scoreRanges.reserve(residentBlockIndices.size());
			std::uint32_t remainingBudget = static_cast<std::uint32_t>(
				std::min<std::size_t>(query.maximumSamplesToVisit,
					std::numeric_limits<std::uint32_t>::max()));
			for(const std::uint32_t blockIndex : residentBlockIndices) {
				if(remainingBudget == 0U) break;
				const MotionDatabaseResidentBlock block = EffectiveBlock(database, blockIndex);
				const std::uint32_t count = std::min(block.sampleCount, remainingBudget);
				if(count == 0U) continue;
				MotionMatchGpuDispatchRange range;
				range.blockIndex = blockIndex;
				range.firstSample = block.firstSample;
				range.sampleCount = count;
				range.threadGroupCount = DivideRoundUp(count, 64U);
				range.gpuByteOffset = block.gpuByteOffset;
				range.gpuByteSize = static_cast<std::uint64_t>(count)
					* database.storageLayout.gpuSampleStrideBytes;
				staged.scoreRanges.push_back(range);
				staged.sampleCount += count;
				remainingBudget -= count;
			}
			if(staged.scoreRanges.empty() || staged.sampleCount == 0U) {
				return AnimationStatus::NotReady;
			}
			staged.firstSample = staged.scoreRanges.front().firstSample;
			staged.scoreDispatchCount = static_cast<std::uint32_t>(staged.scoreRanges.size());

			std::uint32_t reductionInput = staged.sampleCount;
			while(reductionInput > staged.topK) {
				if(staged.reductionPasses.size() >= kMotionMatchGpuMaximumReductionPasses) {
					return AnimationStatus::HardLimitExceeded;
				}
				const std::uint32_t groups = DivideRoundUp(
					reductionInput, kMotionMatchGpuReductionTileSize);
				const std::uint64_t outputCount64 = static_cast<std::uint64_t>(groups)
					* staged.topK;
				if(outputCount64 >= reductionInput
					|| outputCount64 > std::numeric_limits<std::uint32_t>::max()) {
					return AnimationStatus::HardLimitExceeded;
				}
				const std::uint32_t outputCount = static_cast<std::uint32_t>(outputCount64);
				staged.reductionPasses.push_back({reductionInput, groups, outputCount, staged.topK});
				reductionInput = outputCount;
			}
			staged.reductionDispatchCount = static_cast<std::uint32_t>(
				staged.reductionPasses.size());
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	MotionMatchGpuQueue::MotionMatchGpuQueue(
		std::shared_ptr<MotionMatchGpuBackend> backend) noexcept
		: backend_(std::move(backend)) {}

	AnimationStatus MotionMatchGpuQueue::SetBackend(
		std::shared_ptr<MotionMatchGpuBackend> backend) noexcept {
		std::scoped_lock lock(mutex_);
		if(!entries_.empty()) return AnimationStatus::Rejected;
		backend_ = std::move(backend);
		return AnimationStatus::Ok;
	}

	AnimationStatus MotionMatchGpuQueue::Submit(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, std::size_t residentBlockCount,
		MotionMatchGpuGeneration& generation, MotionMatchGpuDispatchPlan& plan) noexcept {
		const std::size_t availableBlocks = EffectiveBlockCount(database);
		if(residentBlockCount == 0U || residentBlockCount > availableBlocks
			|| residentBlockCount > kMotionMatchGpuMaximumResidentBlocks) {
			return AnimationStatus::HardLimitExceeded;
		}
		try {
			std::vector<std::uint32_t> indices(residentBlockCount);
			for(std::size_t index = 0U; index < residentBlockCount; ++index) {
				indices[index] = static_cast<std::uint32_t>(index);
			}
			return Submit(database, query, indices, generation, plan);
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus MotionMatchGpuQueue::Submit(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, std::span<const std::uint32_t> residentBlockIndices,
		MotionMatchGpuGeneration& generation, MotionMatchGpuDispatchPlan& plan) noexcept {
		std::scoped_lock lock(mutex_);
		if(backend_ == nullptr) return AnimationStatus::RendererUnavailable;
		if(entries_.size() >= kMotionMatchGpuMaximumInFlightRequests || nextSequence_ == 0U) {
			return AnimationStatus::Rejected;
		}
		const MotionMatchGpuGeneration stagedGeneration{
			query.database, database.featureSchemaFingerprint, nextSequence_};
		MotionMatchGpuDispatchPlan stagedPlan;
		const AnimationStatus planStatus = PlanMotionMatchGpuDispatch(database, query,
			residentBlockIndices, stagedGeneration, stagedPlan);
		if(planStatus != AnimationStatus::Ok) return planStatus;
		MotionMatchGpuSubmission submission;
		try {
			submission.plan = stagedPlan;
			submission.query = CopyQuery(query);
		} catch(...) {
			return AnimationStatus::InternalError;
		}
		MotionMatchGpuBackendTicket ticket;
		const AnimationStatus backendStatus = backend_->Submit(submission, ticket);
		if(backendStatus != AnimationStatus::Queued && backendStatus != AnimationStatus::Ok) {
			return backendStatus;
		}
		const std::uint64_t readbackBytes = static_cast<std::uint64_t>(ticket.readbackCapacity)
			* ticket.readbackStrideBytes;
		if(!ticket.IsValid() || ticket.readbackCapacity != stagedPlan.topK
			|| readbackBytes > kMotionMatchGpuMaximumReadbackBytes) {
			backend_->Cancel(ticket);
			return AnimationStatus::InvalidArgument;
		}
		try {
			Entry entry;
			entry.generation = stagedGeneration;
			entry.backendTicket = ticket;
			entry.state = MotionMatchGpuRequestState::Submitted;
			entry.readback.generation = stagedGeneration;
			entry.readback.status = AnimationStatus::NotReady;
			entry.readback.state = MotionMatchGpuRequestState::Submitted;
			entries_.push_back(std::move(entry));
		} catch(...) {
			backend_->Cancel(ticket);
			return AnimationStatus::InternalError;
		}
		++nextSequence_;
		generation = stagedGeneration;
		plan = std::move(stagedPlan);
		return AnimationStatus::Queued;
	}

	AnimationStatus MotionMatchGpuQueue::PollBackends(AnimationAssetHandle activeDatabase,
		std::uint64_t activeSchemaFingerprint) noexcept {
		if(!activeDatabase.IsValid() || activeSchemaFingerprint == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		std::scoped_lock lock(mutex_);
		if(backend_ == nullptr) return AnimationStatus::RendererUnavailable;
		AnimationStatus aggregateStatus = AnimationStatus::NotReady;
		for(Entry& entry : entries_) {
			if(entry.generation.database != activeDatabase
				|| entry.generation.schemaFingerprint != activeSchemaFingerprint) {
				if(entry.state == MotionMatchGpuRequestState::Submitted
					|| entry.state == MotionMatchGpuRequestState::AwaitingFence) {
					backend_->Cancel(entry.backendTicket);
				}
				entry.state = MotionMatchGpuRequestState::Stale;
				entry.readback = {entry.generation, AnimationStatus::StaleHandle, false,
					MotionMatchGpuRequestState::Stale, 0U, {}};
				aggregateStatus = AnimationStatus::StaleHandle;
				continue;
			}
			if(entry.state != MotionMatchGpuRequestState::Submitted
				&& entry.state != MotionMatchGpuRequestState::AwaitingFence) {
				continue;
			}
			std::array<MotionMatchGpuCandidateRecord, kMotionDatabaseMaximumTopK> records{};
			std::uint32_t candidateCount = 0U;
			std::uint64_t completedFence = 0U;
			const AnimationStatus status = backend_->TryReadback(entry.backendTicket,
				std::span<MotionMatchGpuCandidateRecord>(records).first(entry.backendTicket.readbackCapacity),
				candidateCount, completedFence);
			if(status == AnimationStatus::NotReady) {
				entry.state = MotionMatchGpuRequestState::AwaitingFence;
				entry.readback.state = entry.state;
				continue;
			}
			if(status == AnimationStatus::Cancelled) {
				entry.state = MotionMatchGpuRequestState::Cancelled;
				entry.readback = {entry.generation, status, true, entry.state, completedFence, {}};
				aggregateStatus = status;
				continue;
			}
			if(status != AnimationStatus::Ok || completedFence < entry.backendTicket.signalFenceValue
				|| candidateCount > entry.backendTicket.readbackCapacity) {
				entry.state = MotionMatchGpuRequestState::Failed;
				entry.readback = {entry.generation,
					status == AnimationStatus::Ok ? AnimationStatus::Rejected : status,
					false, entry.state, completedFence, {}};
				aggregateStatus = entry.readback.status;
				continue;
			}
			try {
				std::vector<MotionMatchCandidate> candidates;
				candidates.reserve(candidateCount);
				bool invalid = false;
				for(std::uint32_t index = 0U; index < candidateCount; ++index) {
					// The final tiled reduction pads a short eligible set with a
					// zero-id/max-cost sentinel. It consumes readback capacity but is not
					// a candidate and therefore never crosses into CPU selection.
					if(records[index].stableSampleId == 0U
						&& records[index].totalCost == std::numeric_limits<std::uint64_t>::max()) {
						continue;
					}
					if(!ValidRecordCost(records[index])) {
						invalid = true;
						break;
					}
					candidates.push_back(CandidateFromRecord(records[index]));
				}
				std::sort(candidates.begin(), candidates.end(), IsMotionMatchCandidateBetter);
				if(std::adjacent_find(candidates.begin(), candidates.end(),
					[](const MotionMatchCandidate& left, const MotionMatchCandidate& right) {
						return left.stableSampleId == right.stableSampleId;
					}) != candidates.end()) {
					invalid = true;
				}
				if(invalid) {
					entry.state = MotionMatchGpuRequestState::Failed;
					entry.readback = {entry.generation, AnimationStatus::Rejected, false,
						entry.state, completedFence, {}};
					aggregateStatus = AnimationStatus::Rejected;
					continue;
				}
				entry.state = MotionMatchGpuRequestState::ReadbackReady;
				entry.readback = {entry.generation,
					candidates.empty() ? AnimationStatus::NotReady : AnimationStatus::Ok,
					false, entry.state, completedFence, std::move(candidates)};
				aggregateStatus = entry.readback.status;
			} catch(...) {
				entry.state = MotionMatchGpuRequestState::Failed;
				entry.readback = {entry.generation, AnimationStatus::InternalError,
					false, entry.state, completedFence, {}};
				aggregateStatus = AnimationStatus::InternalError;
			}
		}
		return aggregateStatus;
	}

	void MotionMatchGpuQueue::Cancel(const MotionMatchGpuGeneration& generation) noexcept {
		std::scoped_lock lock(mutex_);
		for(Entry& entry : entries_) {
			if(entry.generation == generation
				&& entry.state != MotionMatchGpuRequestState::Cancelled) {
				if(backend_ != nullptr && (entry.state == MotionMatchGpuRequestState::Submitted
					|| entry.state == MotionMatchGpuRequestState::AwaitingFence)) {
					backend_->Cancel(entry.backendTicket);
				}
				entry.state = MotionMatchGpuRequestState::Cancelled;
				entry.readback = {generation, AnimationStatus::Cancelled, true,
					entry.state, 0U, {}};
			}
		}
	}

	AnimationStatus MotionMatchGpuQueue::TryConsume(const MotionMatchGpuGeneration& expected,
		MotionMatchGpuReadback& output) noexcept {
		std::scoped_lock lock(mutex_);
		auto entry = std::find_if(entries_.begin(), entries_.end(), [&expected](const Entry& candidate) {
			return candidate.generation == expected;
		});
		if(entry == entries_.end()) return AnimationStatus::StaleHandle;
		if(entry->state == MotionMatchGpuRequestState::Submitted
			|| entry->state == MotionMatchGpuRequestState::AwaitingFence) {
			return AnimationStatus::NotReady;
		}
		try {
			output = entry->readback;
			const AnimationStatus status = output.status;
			entries_.erase(entry);
			return status;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	std::optional<MotionMatchGpuRequestState> MotionMatchGpuQueue::State(
		const MotionMatchGpuGeneration& generation) const noexcept {
		std::scoped_lock lock(mutex_);
		const auto found = std::find_if(entries_.begin(), entries_.end(),
			[&generation](const Entry& entry) { return entry.generation == generation; });
		return found == entries_.end()
			? std::optional<MotionMatchGpuRequestState>{}
			: std::optional<MotionMatchGpuRequestState>{found->state};
	}

	std::size_t MotionMatchGpuQueue::PendingCount() const noexcept {
		std::scoped_lock lock(mutex_);
		return entries_.size();
	}
}
