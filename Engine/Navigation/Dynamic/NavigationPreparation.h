// NavigationPreparation.h -- Bounded copied dynamic navigation work packets.
//
// Dynamic source work is queued as copied engine values. The production
// scheduler may dispatch these packets through the shared ThreadPool; the
// deterministic testing quantum executes the same bounded state transitions
// synchronously without creating a second worker pool or thread.

#pragma once

#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Navigation
{
	enum class NavigationDynamicWorkKind : std::uint8_t
	{
		Build,
		Search,
		RemovalTombstone
	};

	enum class NavigationPreparationState : std::uint8_t
	{
		Invalid,
		Queued,
		Ready,
		Cancelled,
		RejectedCapacity
	};

	struct NavigationDynamicWorkHandle
	{
		NavigationUuid source{};
		NavigationGeneration revision = 0U;
		std::uint64_t sequence = 0U;
		[[nodiscard]] constexpr bool IsValid() const noexcept { return revision != 0U && sequence != 0U; }
		[[nodiscard]] constexpr bool operator==(const NavigationDynamicWorkHandle&) const noexcept = default;
	};

	struct NavigationDynamicWorkRequest
	{
		NavigationDynamicWorkKind kind = NavigationDynamicWorkKind::Build;
		NavigationUuid source{};
		NavigationGeneration revision = 0U;
		std::size_t estimatedBytes = 0U;
		std::shared_ptr<const std::vector<std::byte>> copiedInput;
	};

	struct NavigationPreparationLimits
	{
		std::uint32_t maximumConcurrentBuilds = 4U;
		std::uint32_t maximumConcurrentSearches = 8U;
		std::uint32_t maximumCompletionQueueEntries = 64U;
		std::size_t maximumPreparationBytes = 256U * 1024U * 1024U;
	};

	struct NavigationPreparationCompletion
	{
		NavigationDynamicWorkHandle handle{};
		NavigationPreparationState state = NavigationPreparationState::Invalid;
		std::uint64_t inputFingerprint = 0U;
		std::byte firstInputByte{};
	};

	struct NavigationPreparationTickResult
	{
		std::uint32_t completedBuildCount = 0U;
		std::uint32_t completedSearchCount = 0U;
	};

	struct NavigationPreparationQueueSnapshot
	{
		std::vector<NavigationPreparationCompletion> completions;
		std::vector<NavigationDynamicWorkHandle> removalTombstones;
		std::size_t droppedOrCoalescedCount = 0U;
		[[nodiscard]] NavigationGeneration newestRevisionFor(NavigationUuid source) const noexcept;
		[[nodiscard]] bool ContainsRemovalTombstone(NavigationUuid source, NavigationGeneration revision) const noexcept;
	};

	class TERRAENGINE_API NavigationPreparationCoordinator final
	{
	public:
		explicit NavigationPreparationCoordinator(NavigationPreparationLimits limits);
		[[nodiscard]] NavigationDynamicWorkHandle Submit(const NavigationDynamicWorkRequest& request);
		[[nodiscard]] NavigationDynamicWorkHandle SubmitRemovalTombstone(NavigationUuid source, NavigationGeneration revision);
		[[nodiscard]] bool Cancel(NavigationDynamicWorkHandle handle);
		[[nodiscard]] NavigationPreparationTickResult RunWorkerQuantumForTesting(std::uint32_t maximumPackets);
		[[nodiscard]] NavigationPreparationCompletion ReadCompletion(NavigationDynamicWorkHandle handle) const;
		[[nodiscard]] std::optional<NavigationPreparationCompletion> TryTakePublishableCompletion(NavigationDynamicWorkHandle handle);
		[[nodiscard]] NavigationPreparationQueueSnapshot CompletionSnapshot() const;
		[[nodiscard]] std::size_t PreparationBytesReserved() const noexcept;
		[[nodiscard]] std::size_t SharedInputBytesCharged() const noexcept;
		[[nodiscard]] bool WorkerPacketsHaveNoLiveSceneReferences() const noexcept;

	private:
		struct PendingPacket
		{
			NavigationDynamicWorkHandle handle{};
			NavigationDynamicWorkKind kind = NavigationDynamicWorkKind::Build;
			std::shared_ptr<const std::vector<std::byte>> frozenInput;
			std::size_t reservedBytes = 0U;
			bool cancelled = false;
		};

		[[nodiscard]] static std::uint64_t Fingerprint(const std::vector<std::byte>& bytes) noexcept;
		[[nodiscard]] static bool SameSource(NavigationUuid first, NavigationUuid second) noexcept;
		void EnqueueCompletion(NavigationPreparationCompletion completion, NavigationDynamicWorkKind kind);

		NavigationPreparationLimits limits_{};
		mutable std::mutex mutex_;
		std::uint64_t nextSequence_ = 1U;
		std::size_t preparationBytesReserved_ = 0U;
		std::size_t sharedInputBytesCharged_ = 0U;
		std::vector<PendingPacket> pending_;
		std::vector<NavigationPreparationCompletion> completions_;
		std::vector<NavigationDynamicWorkHandle> removalTombstones_;
		std::vector<NavigationPreparationCompletion> allCompletions_;
		std::unordered_map<const void*, std::shared_ptr<const std::vector<std::byte>>> frozenInputs_;
		std::size_t droppedOrCoalescedCount_ = 0U;
	};
}
