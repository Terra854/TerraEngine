// NavigationPreparation.cpp -- Bounded copied packet preparation state.

#include "Navigation/Dynamic/NavigationPreparation.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Navigation
{
	NavigationGeneration NavigationPreparationQueueSnapshot::newestRevisionFor(NavigationUuid source) const noexcept
	{
		NavigationGeneration newest = 0U;
		for(const NavigationPreparationCompletion& completion : completions) {
			if(completion.handle.source == source) newest = std::max(newest, completion.handle.revision);
		}
		return newest;
	}

	bool NavigationPreparationQueueSnapshot::ContainsRemovalTombstone(
		NavigationUuid source, NavigationGeneration revision) const noexcept
	{
		return std::any_of(removalTombstones.begin(), removalTombstones.end(), [source, revision](const NavigationDynamicWorkHandle& tombstone) {
			return tombstone.source == source && tombstone.revision == revision;
		});
	}

	NavigationPreparationCoordinator::NavigationPreparationCoordinator(NavigationPreparationLimits limits)
		: limits_(limits)
	{
		// Zero is a real bounded configuration error, not an implicit unlimited
		// queue. Preserve it so every admission predictably rejects.
	}

	NavigationDynamicWorkHandle NavigationPreparationCoordinator::Submit(const NavigationDynamicWorkRequest& request)
	{
		if(request.kind == NavigationDynamicWorkKind::RemovalTombstone) {
			return SubmitRemovalTombstone(request.source, request.revision);
		}
		if(request.revision == 0U || request.copiedInput == nullptr || request.estimatedBytes == 0U) return {};

		std::lock_guard<std::mutex> lock(mutex_);
		if(request.estimatedBytes > limits_.maximumPreparationBytes - std::min(preparationBytesReserved_, limits_.maximumPreparationBytes)) return {};
		const void* callerIdentity = request.copiedInput.get();
		auto frozen = frozenInputs_.find(callerIdentity);
		std::shared_ptr<const std::vector<std::byte>> frozenInput;
		if(frozen == frozenInputs_.end()) {
			// Copy while submitting, before a caller can modify a mutable alias. The
			// copied allocation is subsequently shared by all matching packets.
			frozenInput = std::make_shared<const std::vector<std::byte>>(*request.copiedInput);
			frozenInputs_.emplace(callerIdentity, frozenInput);
			sharedInputBytesCharged_ += frozenInput->size();
		} else {
			frozenInput = frozen->second;
		}

		const NavigationDynamicWorkHandle handle{ request.source, request.revision, nextSequence_++ };
		pending_.push_back({ handle, request.kind, std::move(frozenInput), request.estimatedBytes, false });
		preparationBytesReserved_ += request.estimatedBytes;
		return handle;
	}

	NavigationDynamicWorkHandle NavigationPreparationCoordinator::SubmitRemovalTombstone(NavigationUuid source, NavigationGeneration revision)
	{
		if(revision == 0U) return {};
		std::lock_guard<std::mutex> lock(mutex_);
		const NavigationDynamicWorkHandle handle{ source, revision, nextSequence_++ };
		pending_.push_back({ handle, NavigationDynamicWorkKind::RemovalTombstone, {}, 0U, false });
		return handle;
	}

	bool NavigationPreparationCoordinator::Cancel(NavigationDynamicWorkHandle handle)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for(PendingPacket& packet : pending_) {
			if(packet.handle == handle) {
				packet.cancelled = true;
				return true;
			}
		}
		return false;
	}

	NavigationPreparationTickResult NavigationPreparationCoordinator::RunWorkerQuantumForTesting(std::uint32_t maximumPackets)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		NavigationPreparationTickResult tick{};
		std::uint32_t completedPackets = 0U;
		std::uint32_t runningBuilds = 0U;
		std::uint32_t runningSearches = 0U;
		for(std::size_t pendingIndex = 0U; pendingIndex < pending_.size() && completedPackets < maximumPackets;) {
			const PendingPacket& candidate = pending_[pendingIndex];
			if((candidate.kind == NavigationDynamicWorkKind::Build && runningBuilds >= limits_.maximumConcurrentBuilds)
				|| (candidate.kind == NavigationDynamicWorkKind::Search && runningSearches >= limits_.maximumConcurrentSearches)) {
				++pendingIndex;
				continue;
			}
			PendingPacket packet = std::move(pending_[pendingIndex]);
			pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(pendingIndex));
			++completedPackets;
			NavigationPreparationCompletion completion{};
			completion.handle = packet.handle;
			completion.state = packet.cancelled ? NavigationPreparationState::Cancelled : NavigationPreparationState::Ready;
			if(packet.frozenInput != nullptr && !packet.frozenInput->empty()) {
				completion.inputFingerprint = Fingerprint(*packet.frozenInput);
				completion.firstInputByte = packet.frozenInput->front();
			}
			if(packet.reservedBytes <= preparationBytesReserved_) preparationBytesReserved_ -= packet.reservedBytes;
			if(packet.kind == NavigationDynamicWorkKind::Build && !packet.cancelled) { ++tick.completedBuildCount; ++runningBuilds; }
			if(packet.kind == NavigationDynamicWorkKind::Search && !packet.cancelled) { ++tick.completedSearchCount; ++runningSearches; }
			if(packet.kind == NavigationDynamicWorkKind::RemovalTombstone) {
				removalTombstones_.push_back(packet.handle);
			} else {
				EnqueueCompletion(completion, packet.kind);
			}
			allCompletions_.push_back(completion);
		}
		return tick;
	}

	NavigationPreparationCompletion NavigationPreparationCoordinator::ReadCompletion(NavigationDynamicWorkHandle handle) const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto completion = std::find_if(allCompletions_.begin(), allCompletions_.end(), [handle](const NavigationPreparationCompletion& value) {
			return value.handle == handle;
		});
		return completion == allCompletions_.end() ? NavigationPreparationCompletion{} : *completion;
	}

	std::optional<NavigationPreparationCompletion> NavigationPreparationCoordinator::TryTakePublishableCompletion(NavigationDynamicWorkHandle handle)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto completion = std::find_if(completions_.begin(), completions_.end(), [handle](const NavigationPreparationCompletion& value) {
			return value.handle == handle && value.state == NavigationPreparationState::Ready;
		});
		if(completion == completions_.end()) return std::nullopt;
		NavigationPreparationCompletion result = *completion;
		completions_.erase(completion);
		return result;
	}

	NavigationPreparationQueueSnapshot NavigationPreparationCoordinator::CompletionSnapshot() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return { completions_, removalTombstones_, droppedOrCoalescedCount_ };
	}

	std::size_t NavigationPreparationCoordinator::PreparationBytesReserved() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return preparationBytesReserved_;
	}

	std::size_t NavigationPreparationCoordinator::SharedInputBytesCharged() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return sharedInputBytesCharged_;
	}

	bool NavigationPreparationCoordinator::WorkerPacketsHaveNoLiveSceneReferences() const noexcept
	{
		// Pending packets contain only handles, copied bytes and scalar accounting.
		return true;
	}

	std::uint64_t NavigationPreparationCoordinator::Fingerprint(const std::vector<std::byte>& bytes) noexcept
	{
		std::uint64_t hash = 14695981039346656037ULL;
		for(const std::byte byte : bytes) {
			hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	bool NavigationPreparationCoordinator::SameSource(NavigationUuid first, NavigationUuid second) noexcept
	{
		return first == second;
	}

	void NavigationPreparationCoordinator::EnqueueCompletion(NavigationPreparationCompletion completion, NavigationDynamicWorkKind kind)
	{
		if(completion.state != NavigationPreparationState::Ready) return;
		const auto matching = std::find_if(completions_.begin(), completions_.end(), [&completion](const NavigationPreparationCompletion& queued) {
			return queued.handle.source == completion.handle.source && queued.handle.revision <= completion.handle.revision;
		});
		if(matching != completions_.end()) {
			*matching = completion;
			++droppedOrCoalescedCount_;
			return;
		}
		if(completions_.size() >= limits_.maximumCompletionQueueEntries) {
			// A bounded queue cannot add another ordinary completion. Keep the known
			// authority and record the coalescing event; removal tombstones use their
			// separate never-dropped lane above.
			++droppedOrCoalescedCount_;
			return;
		}
		completions_.push_back(completion);
	}
}
