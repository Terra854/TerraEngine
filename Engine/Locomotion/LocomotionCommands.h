// LocomotionCommands.h - bounded copied command admission and protected publication.

#pragma once

#include "LocomotionTypes.h"

#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <span>
#include <unordered_map>

namespace TerraEngine::Locomotion {
	enum class CommandKind : std::uint8_t {
		Acquire, SetIntent, FollowNavigation, Stop, SetMode, Transfer, Release
	};
	enum class ReceiptState : std::uint8_t { Queued, Applied, Adjusted, Rejected, Expired };

	struct TERRAENGINE_API LocomotionCommand {
		CommandKind kind = CommandKind::Acquire;
		LocomotionHandle handle{};
		std::uint64_t expectedAuthorityEpoch = 0U;
		std::uint64_t expectedConfigurationRevision = 0U;
		std::uint64_t ownerToken = 0U;
		std::uint64_t producerId = 0U;
		std::uint64_t sequence = 0U;
		std::uint64_t correlationId = 0U;
		IntentKind intentKind = IntentKind::Player;
		DirectionSpace directionSpace = DirectionSpace::World;
		glm::dvec3 direction{0.0};
		double requestedSpeed = 0.0;
		IntentLifetime lifetime = IntentLifetime::OneUpdate;
		MotionMode mode = MotionMode::Velocity;
		std::uint64_t transferProducerId = 0U;
	};

	struct TERRAENGINE_API CommandReceipt {
		LocomotionHandle handle{};
		std::uint64_t producerId = 0U;
		std::uint64_t sequence = 0U;
		std::uint64_t correlationId = 0U;
		ReceiptState state = ReceiptState::Rejected;
		LocomotionReason reason = LocomotionReason::None;
		std::uint64_t acceptedUpdateEpoch = 0U;
		std::uint64_t authorityEpoch = 0U;
		std::uint64_t ownerToken = 0U;
	};

	struct TERRAENGINE_API CommandBatchAdmission {
		bool queued = false;
		LocomotionReason reason = LocomotionReason::None;
		std::vector<CommandReceipt> receipts;
	};

	struct TERRAENGINE_API AuthorityTransition {
		MovementAuthority previous{};
		MovementAuthority next{};
	};

	// The protected caller owns the exact live binding/physical preflight. It may
	// reject a proposed command without giving worker code any live scene access.
	using LocomotionExternalPreflight = std::function<LocomotionReason(
		const LocomotionCommand&, const LocomotionRuntimeRecord&)>;

	struct TERRAENGINE_API PreparedCommandBatch {
		struct SlotUpdate {
			std::uint32_t slotIndex = kInvalidLocomotionSlot;
			LocomotionRuntimeRecord next{};
		};
		std::uint64_t storeRevision = 0U;
		std::uint64_t batchId = 0U;
		bool accepted = false;
		LocomotionReason reason = LocomotionReason::None;
		std::vector<SlotUpdate> updates;
		std::vector<AuthorityTransition> transitions;
		std::vector<CommandReceipt> receipts;
		std::shared_ptr<const LocomotionAuthorityRoot> nextRoot;
		std::uint64_t nextOwnerToken = 0U;
		// Prepare keeps admission serialized through the physical reservation and
		// same-boundary commit. Producers may wait briefly, but cannot invalidate a
		// prepared authority swap or replace a safety stop halfway through it.
		std::unique_lock<std::mutex> reservation;
	};

	struct TERRAENGINE_API BindResult {
		LocomotionHandle handle{};
		LocomotionReason reason = LocomotionReason::None;
		[[nodiscard]] bool Succeeded() const noexcept { return reason == LocomotionReason::None && handle.IsValid(); }
	};

	// All mutation methods run at a protected scheduler boundary. SubmitBatch is
	// thread safe for managed/automation producers and retains only copied values.
	class TERRAENGINE_API LocomotionCommandStore final {
	public:
		LocomotionCommandStore();
		BindResult Bind(LocomotionHandle identity, ResolvedLocomotionBindings bindings);
		[[nodiscard]] bool UpdateBinding(LocomotionHandle handle, ResolvedLocomotionBindings bindings);
		[[nodiscard]] std::optional<LocomotionRuntimeRecord> Read(LocomotionHandle handle) const;
		[[nodiscard]] std::vector<LocomotionRuntimeRecord> SnapshotLive() const;
		// Capture copies commands eligible at this movement-update cutoff. It does
		// not drain them; Publish still validates the whole batch and commits it.
		[[nodiscard]] std::vector<LocomotionCommand> SnapshotEligibleCommands(
			std::uint64_t updateEpoch) const;
		CommandBatchAdmission SubmitBatch(std::span<const LocomotionCommand> commands,
			std::uint64_t earliestUpdateEpoch);
		[[nodiscard]] std::optional<PreparedCommandBatch> PrepareNextBatch(
			std::uint64_t updateEpoch, const LocomotionExternalPreflight& preflight) const;
		// The caller reserves and publishes required physical/animation transitions
		// before this same-boundary commit. A successful Prepare at the same revision
		// makes this state/root/receipt swap non-fallible.
		[[nodiscard]] bool CommitPreparedBatch(PreparedCommandBatch&& prepared) noexcept;
		[[nodiscard]] bool RejectPreparedBatch(PreparedCommandBatch&& prepared,
			LocomotionReason reason) noexcept;
		[[nodiscard]] CommandReceipt ReadReceipt(LocomotionHandle handle, std::uint64_t producerId,
			std::uint64_t sequence, std::uint64_t correlationId) const;
		[[nodiscard]] std::shared_ptr<const LocomotionAuthorityRoot> CopyAuthorityRoot() const;
		[[nodiscard]] std::shared_ptr<const LocomotionObservationRoot> CopyObservationRoot() const;
		[[nodiscard]] bool PublishObservations(std::span<const LocomotionObservation> observations);
		[[nodiscard]] bool Revoke(LocomotionHandle handle, LocomotionReason reason);
		[[nodiscard]] bool ReleaseSlot(LocomotionHandle handle);
		std::size_t RevokeScene(std::uint64_t sceneLifetime, LocomotionReason reason);
		void ExpireOneUpdateIntents(std::uint64_t updateEpoch);
		[[nodiscard]] std::size_t LiveCount() const;
		[[nodiscard]] std::size_t PendingCount() const;

	private:
		struct ProducerHighWater { std::uint64_t sequence = 0U; };
		struct Slot {
			std::uint32_t generation = 0U;
			bool alive = false;
			LocomotionRuntimeRecord record{};
			std::unordered_map<std::uint64_t, ProducerHighWater> producers;
			std::optional<LocomotionCommand> safetyStop;
			std::uint64_t safetyEarliestUpdateEpoch = 0U;
			std::size_t pendingCount = 0U;
		};
		struct ControllerKey {
			std::uint64_t worldId = 0U;
			std::uint32_t worldGeneration = 0U;
			std::uint32_t slotIndex = 0U;
			std::uint32_t slotGeneration = 0U;
			[[nodiscard]] bool operator==(const ControllerKey&) const = default;
		};
		struct ControllerKeyHash { std::size_t operator()(const ControllerKey& key) const noexcept; };
		struct PendingBatch {
			std::uint64_t id = 0U;
			std::uint64_t earliestUpdateEpoch = 0U;
			std::vector<LocomotionCommand> commands;
		};
		struct ReceiptKey {
			LocomotionHandle handle{};
			std::uint64_t producerId = 0U;
			std::uint64_t sequence = 0U;
			[[nodiscard]] bool operator==(const ReceiptKey&) const = default;
		};
		struct ReceiptKeyHash { std::size_t operator()(const ReceiptKey& key) const noexcept; };
		struct RetainedReceipt { LocomotionCommand command{}; CommandReceipt receipt{}; };
		[[nodiscard]] bool Matches(LocomotionHandle handle) const noexcept;
		void RetainReceipt(const LocomotionCommand& command, CommandReceipt receipt);
		void CompleteReceipt(const CommandReceipt& receipt) noexcept;
		void RemovePendingFor(LocomotionHandle handle);
		[[nodiscard]] std::shared_ptr<const LocomotionAuthorityRoot> BuildAuthorityRoot(
			std::span<const PreparedCommandBatch::SlotUpdate> updates) const;
		mutable std::mutex mutex_;
		std::vector<Slot> slots_;
		std::vector<std::uint32_t> freeSlots_;
		std::unordered_map<ControllerKey, std::uint32_t, ControllerKeyHash> controllerSlots_;
		std::deque<PendingBatch> pending_;
		std::size_t pendingCount_ = 0U;
		std::uint64_t nextBatchId_ = 1U;
		std::uint64_t nextOwnerToken_ = 1U;
		std::uint32_t storeGenerationPrefix_ = 0U;
		std::uint64_t revision_ = 1U;
		std::list<RetainedReceipt> receiptRing_;
		std::optional<std::list<RetainedReceipt>::iterator> firstTerminalReceipt_;
		std::size_t terminalReceiptCount_ = 0U;
		std::unordered_map<ReceiptKey, std::list<RetainedReceipt>::iterator, ReceiptKeyHash> receiptIndex_;
		std::shared_ptr<const LocomotionAuthorityRoot> authorityRoot_;
		std::shared_ptr<const LocomotionObservationRoot> observationRoot_;
	};
}
