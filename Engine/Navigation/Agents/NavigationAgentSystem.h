// NavigationAgentSystem.h - protected coordinator for optional navigation agents.
//
// The system owns bounded runtime records and advisory guidance. It never owns
// Transform, velocity, or provider state; object/component reads occur only in
// Capture/Publish, while preparation receives copied values.

#pragma once

#include "Core/SystemBase.h"
#include "NavigationAgentCapture.h"
#include "NavigationAgentCommandQueue.h"
#include "NavigationAgentPreparation.h"
#include "NavigationAgentStore.h"
#include "Navigation/NavigationSystem.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

class ObjectManager;

namespace TerraEngine::Navigation {
	struct NavigationAgentStatusSnapshot {
		NavigationAgentState state = NavigationAgentState::Idle;
		NavigationOutcome outcome = NavigationOutcome::Queued;
		NavigationReason reason = NavigationReason::None;
		NavigationGeneration configurationRevision = 0U;
		NavigationGeneration intentSequence = 0U;
		NavigationGeneration publicationRevision = 0U;
	};

	// Editor and automation diagnostics consume this copied summary instead of
	// reading the mutable runtime store. Route polylines stay out of this shared
	// list so a 4,096-agent scene does not copy every route for one selection.
	struct NavigationAgentDiagnosticRecord {
		NavigationAgentIdentity identity{};
		NavigationAgentGuidanceSnapshot guidance{};
	};

	struct NavigationAgentDiagnosticsSnapshot {
		NavigationGeneration revision = 0U;
		std::vector<NavigationAgentDiagnosticRecord> agents;
		std::size_t activeAgents = 0U;
		std::size_t requestingAgents = 0U;
		std::size_t followingAgents = 0U;
		std::size_t arrivedAgents = 0U;
		std::size_t stoppedAgents = 0U;
		std::size_t failedAgents = 0U;
		std::size_t pendingCommands = 0U;
		std::size_t commandCapacity = 0U;
		std::size_t retainedReceipts = 0U;
		// Approximate allocation owned by the current live publication and runtime.
		// Retired shared snapshots retained by external readers are intentionally
		// excluded because this system no longer owns their lifetime.
		std::size_t currentOwnedMemoryBytes = 0U;
		std::uint64_t retryCount = 0U;
		std::uint64_t rejectedCommandCount = 0U;
		std::uint64_t coalescedCommandCount = 0U;
		// This is the protected store-to-publication cost, not end-to-end path
		// search time. It remains cheap enough to update every publication.
		std::uint64_t lastProtectedPublishMicroseconds = 0U;
	};

	// A selected route is copied only when an editor actually asks for it. The
	// accompanying summary comes from the immutable publication named by
	// diagnosticsRevision, so the UI cannot combine route points with a newer
	// state/reason record.
	struct NavigationAgentSelectedDiagnostic {
		NavigationAgentDiagnosticRecord agent{};
		std::vector<NavigationLocation> routePoints;
		bool routeCurrent = false;
	};

	// This protected-boundary copy is the only entity-to-identity bridge exposed
	// to scripting. Callers receive values from an immutable published snapshot;
	// they never dereference ObjectManager or ECS storage from a managed callback.
	struct NavigationAgentPublishedEntityBinding {
		std::uint32_t entity = 0U;
		NavigationObjectIdentity object{};
		NavigationAgentIdentity agent{};

		[[nodiscard]] bool operator<(const NavigationAgentPublishedEntityBinding& other) const noexcept {
			return entity < other.entity;
		}
	};

	class TERRAENGINE_API NavigationAgentSystem final : public SystemsBase {
	public:
		static NavigationAgentSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		[[nodiscard]] NavigationAgentIdentity FindIdentity(ObjectManager& objects, UUID objectId);
		[[nodiscard]] std::optional<NavigationAgentIdentity> ResolvePublishedAgent(
			std::uint32_t entity) const noexcept;
		// Resolves an owner UUID through the protected immutable binding copy. This
		// is safe for scripting/editor callers because it never touches ECS storage.
		[[nodiscard]] std::optional<NavigationAgentIdentity> ResolvePublishedAgent(
			const UUID& objectId) const noexcept;
		[[nodiscard]] std::optional<NavigationObjectIdentity> ResolvePublishedObject(
			std::uint32_t entity) const noexcept;
		// A nonzero value means a bounded target snapshot omitted this many
		// transform owners. Callers can report capacity instead of stale identity.
		[[nodiscard]] std::uint32_t PublishedObjectBindingOverflowCount() const noexcept;
		[[nodiscard]] bool HasLiveIdentity(NavigationAgentIdentity identity) const noexcept;
		[[nodiscard]] NavigationAgentReceipt QueueLocationDestination(NavigationAgentIdentity identity,
			NavigationLocation destination, NavigationGeneration expectedConfigurationRevision,
			NavigationGeneration expectedIntentSequence, std::uint64_t producerOrderingToken,
			std::uint64_t correlationId);
		[[nodiscard]] NavigationAgentReceipt QueueObjectDestination(NavigationAgentIdentity identity,
			NavigationObjectIdentity destination, NavigationGeneration expectedConfigurationRevision,
			NavigationGeneration expectedIntentSequence, std::uint64_t producerOrderingToken,
			std::uint64_t correlationId);
		[[nodiscard]] NavigationAgentReceipt QueueStop(NavigationAgentIdentity identity,
			NavigationGeneration expectedConfigurationRevision,
			NavigationGeneration expectedIntentSequence, std::uint64_t producerOrderingToken,
			std::uint64_t correlationId);
		[[nodiscard]] std::optional<NavigationAgentReceipt> ReadReceipt(
			NavigationAgentIdentity identity, std::uint64_t correlationId) const;
		[[nodiscard]] NavigationAgentStatusSnapshot ReadStatus(NavigationAgentIdentity identity) const;
		[[nodiscard]] NavigationAgentGuidanceSnapshot ReadGuidance(NavigationAgentIdentity identity) const;
		[[nodiscard]] std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> PublishedDiagnostics() const noexcept;
		[[nodiscard]] std::optional<NavigationAgentSelectedDiagnostic> ReadPublishedSelectedDiagnostic(
			NavigationAgentIdentity identity, NavigationGeneration diagnosticsRevision) const;
		// NavigationAgentCommandSystem invokes this protected operation before
		// NavigationSystem captures route work. It consumes only copied commands,
		// validates live attachments on the coordinator thread, and submits a fresh
		// accepted destination to NavigationSystem without waiting for agent workers.
		// NavigationAgentSystem::Update also calls it for direct-update compatibility.
		void CaptureCommandAdmission();

		// MainEngine invokes this after every protected scene/spatial/physics/script
		// edge. Cached guidance is invalidated before a later consumer can use stale
		// observations; the next Navigation capture reconstructs it from copies.
		void ObserveProtectedBoundary(NavigationProtectedBoundary boundary,
			std::uint64_t publicationGeneration);
		void RevokeAllAgents(NavigationReason reason);
		void SetContractTestWorkAvailable(bool available) noexcept;

	private:
		struct Binding {
			UUID objectId = EMPTY_UUID;
			NavigationAgentIdentity identity{};
			NavigationGeneration componentGeneration = 0U;
			NavigationGeneration componentValueRevision = 0U;
			double nextRequestTimeSeconds = 0.0;
			std::size_t waypointIndex = 0U;
			std::vector<NavigationLocation> routePoints;
			NavigationObservationEvidence routeObservation{};
			NavigationGeneration routeGeneration = 0U;
			NavigationGeneration capturedOwnerTransformRevision = 0U;
			NavigationGeneration capturedTargetTransformRevision = 0U;
		};

		NavigationAgentSystem() = default;
		void CaptureProtected();
		void DrainQueuedCommandsLocked(ObjectManager& objects, const NavigationWorldHandle& world);
		// Publish only a conservative queue-size hint for scheduler declaration
		// liveness. NavigationSystem never treats it as route authority.
		void RefreshPendingRouteAdmissionDemandLocked() const;
		void PrepareCapturedWork();
		void PrepareCapturedRange(std::size_t begin, std::size_t end);
		void PublishPreparedWork();
		void ReconcileBindings(ObjectManager& objects, const NavigationWorldHandle& world);
		void RebuildPublishedEntityBindings(ObjectManager& objects);
		void PublishDiagnosticsLocked();
		void ReleaseRuntimeRecord(const NavigationAgentRuntimeRecord& record);
		// The vector remains the deterministic ownership/order source. This protected
		// sidecar only maps a UUID to its current vector slot so hot runtime passes do
		// not repeatedly scan every agent; callers still validate the full identity.
		void RebuildBindingIndexLocked();
		[[nodiscard]] Binding* FindBindingForObject(UUID objectId);
		[[nodiscard]] const Binding* FindBindingForObject(UUID objectId) const;
		[[nodiscard]] Binding* FindBinding(const NavigationAgentIdentity& identity);
		[[nodiscard]] const Binding* FindBinding(const NavigationAgentIdentity& identity) const;
		[[nodiscard]] bool IsCurrentAttachment(ObjectManager& objects,
			const NavigationAgentRuntimeRecord& record) const;

		mutable std::mutex mutex_;
		std::unique_ptr<NavigationAgentStore> store_;
		std::unique_ptr<NavigationAgentCommandQueue> queue_;
		std::vector<Binding> bindings_;
		std::unordered_map<UUID, std::size_t> bindingIndexes_;
		std::vector<NavigationAgentCapture> captured_;
		std::vector<NavigationAgentCandidate> prepared_;
		std::shared_ptr<const std::vector<NavigationAgentPublishedEntityBinding>> publishedEntityBindings_;
		std::shared_ptr<const NavigationAgentDiagnosticsSnapshot> publishedDiagnostics_;
		std::atomic_uint32_t publishedObjectBindingOverflowCount_{0U};
		NavigationGeneration bindingSnapshotGeneration_ = 1U;
		NavigationGeneration publishedBindingSnapshotGeneration_ = 0U;
		std::uint64_t publishedObjectMembershipRevision_ = 0U;
		bool hasPublishedEntityBindingSnapshot_ = false;
		double elapsedSeconds_ = 0.0;
		NavigationGeneration lastBoundaryGeneration_ = 0U;
		NavigationGeneration publicationGeneration_ = 0U;
		NavigationGeneration diagnosticsRevision_ = 0U;
		std::uint64_t retryCount_ = 0U;
		std::uint64_t rejectedCommandCount_ = 0U;
		std::uint64_t lastProtectedPublishMicroseconds_ = 0U;
		NavigationProtectedBoundary lastBoundary_ = NavigationProtectedBoundary::FrameEnd;
		std::atomic_bool initialized_{false};
		std::atomic_bool contractTestWorkAvailable_{false};
	};
}
